#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-builtin-type-infos.h"
#include "qapi/visitor.h"
#include "qobject/qbool.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "qobject/qnull.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/sparc/grlib_stix.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "qemu/host-utils.h"
#include "chardev/char-fe.h"
#include "qemu/timer.h"
#include "system/runstate.h"
#include "StixIdpu.h"
#include "stix_det.h"
#include "stix_att.h"
#include "stix_edac.h"

#include "trace.h"
#include "qom/object.h"

/* HK ADC channels: hkAdc ADDR is 7 bits (STIXCTRL_HKADC_ADDR_MASK) */
#define STIXCTRL_HK_CHANNELS (STIXCTRL_HKADC_ADDR_MASK + 1)
/* HK protection lines: hkProtConfig[9], selector = line + 1 */
#define STIXCTRL_HK_PROT_LINES 9

#define STIXCTRL_REG_SIZE sizeof(stixCtrlRegs_s) /* Size of memory mapped registers */ // TODO: Set to right size

typedef struct
{
  uint32_t coarse;
  uint16_t fine;
} stixCtrl_scet_s;

/*static uint64_t scetToMs(stixCtrl_scet_s _scet)
{
	return toMs(_scet.coarse, _scet.fine);
}*/

struct STIXCTRL
{
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq_detectors;
    qemu_irq irq_hk;
    qemu_irq irq_other;
    qemu_irq irq_edac;
    qemu_irq irq_rotb;

    stixCtrlRegs_s reg;
    //stixCtrl_scet_s reference_t;
    //int64_t reference_time;
    /* SCET correction in ns: current SCET = QEMU_CLOCK_VIRTUAL - correction.
     * Signed int64: setting the clock backwards just grows the correction.
     * (D1, review T1: counter semantics, ns resolution.) */
    int64_t time_correction_ns;
    /* SCET set staging (HSWICD 4.51 two-register commit, D1 amended with
     * Laszlo 2026-11-09): the 0xC8 write STAGES the seconds field and the
     * UNSYNCED status flag without moving the running time; the 0xCC write
     * COMMITS (staged seconds, written subsec) as the new on-board time. */
    uint32_t staged_secs;
    uint32_t unsynced_flag;
    /*
     * D-J/correction 6: called after a 0xCC commit (the SPW re-arms
     * its time-code timer at the moved rollover)
     */
    StixCtrlScetCommitFn scet_commit_fn;
    void *scet_commit_opaque;
    /* Watchdog (0xD4), counted lazily from QEMU_CLOCK_VIRTUAL: while
     * running, COUNT = wdg_count - cycles elapsed since wdg_base_ns. */
    uint32_t wdg_reload;
    uint32_t wdg_count;
    bool wdg_running;
    int64_t wdg_base_ns;
    QEMUTimer *wdg_timer;
    /* ROTB (0x54/0x58, BLOCK window 0xE0800000): device-internal ring
     * (D-A). rd/wr/count/full are the FIFO state; the ring CONTENT is
     * zeroed at realize and kept across system resets. */
    uint32_t rotb_blocks;
    uint8_t *rotb_ring;
    uint32_t rotb_rd;
    uint32_t rotb_wr;
    uint32_t rotb_count;
    bool rotb_full;
    MemoryRegion rotb_block_window;
    /* AHB data windows (D-A device RAM, D-G reset-persistent): the
     * SRAM/ELUT/LACC area, the LVAR BRAM, LTRG and ETRG */
    MemoryRegion sram;
    MemoryRegion lvar;
    MemoryRegion ltrg;
    MemoryRegion etrg;
    /*
     * P3-1 (D-P3a): when true, an hkAdc write converts immediately and
     * returns the nominal raw value (stixctrl_hk_value); when false the
     * legacy behaviour (VAL 0, READY forced) is kept for A/B comparison
     * and rollback.
     */
    bool hk_adc;
    /*
     * P4-1 (D-P4c): when true, hkCmdSta POWERSTAT (bits 9:5) is composed
     * on read from the commanded enables and guest writes to it are
     * dropped; when false it is plain storage (legacy, A/B comparison).
     */
    bool powerstat;
    /*
     * P4-2 (D-P4a/b): runtime HK ADC overrides, one per channel, set
     * over QMP as adc-inject[N]. STIXCTRL_ADC_INJECT_NONE = no override;
     * otherwise a raw 12-bit value that replaces the channel's reading.
     * Runtime-only: set over QMP after start-up (the reset at machine
     * start clears a -global value), and consulted only with hk-adc=on.
     * Cleared on device reset.
     */
    uint32_t adc_inject[STIXCTRL_HK_CHANNELS];
    /*
     * PL-2 (D-PLo): the last guest conversion per channel (the value
     * the hkAdc write returned and its virtual time; -1 = none since
     * reset), for the hw-state staleness check. Written only in the
     * hkAdc write path, read only by the hw-state getter.
     */
    uint32_t hk_last_val[STIXCTRL_HK_CHANNELS];
    int64_t hk_last_ns[STIXCTRL_HK_CHANNELS];
    /*
     * P4-4 (D-P4d): when true, the nine HK protection comparators run
     * and hkIrqSta (0x90), hkProtConfig[9] (0x94..0xB4) and
     * UnlockingReg (0xD0) have their ICD semantics; when false the
     * three are plain storage (legacy, A/B comparison).
     */
    bool hk_prot;
    /* consecutive samples above threshold per line, saturating at 2 */
    uint8_t hk_prot_high[STIXCTRL_HK_PROT_LINES];
    /*
     * UnlockingReg got 0xA5: the next register write may change a
     * protected hkProtConfig field
     */
    bool hk_prot_unlocked;
    /*
     * Detector plan D-1 (D-Da): the detector controller (0x00-0x4C, the
     * QL tick and IRQ 3) lives in stix_det.c; its registers stay in reg
     */
    StixDetState det;
    /*
     * Attenuator plan A-1 (D-Aa): the mechanism and AttnCmdSta (0x5C)
     * live in stix_att.c; the QOM face is the child object
     * /machine/stixctrl/attenuator
     */
    StixAttState att;
    StixAttConfig att_cfg;      /* the att-* qdev properties */
    /*
     * Memory-upset plan D-Ua (M-1): the EDAC unit, the QOM child at
     * /machine/stixctrl/edac. It owns the SEC counters and DeDetected
     * latches the 0x50/0x54 reads compose, and the edac-at-boot state
     * the reset applies.
     */
    StixEdac edac;
};

#define STIXCTRL_ADC_INJECT_NONE 0xFFFFFFFFu

static void stixctrl_adc_inject_clear(STIXCTRL *s)
{
    uint32_t ch;

    for (ch = 0; ch < STIXCTRL_HK_CHANNELS; ch++) {
        s->adc_inject[ch] = STIXCTRL_ADC_INJECT_NONE;
    }
}

static void stixctrl_hk_last_clear(STIXCTRL *s)
{
    uint32_t ch;

    for (ch = 0; ch < STIXCTRL_HK_CHANNELS; ch++) {
        s->hk_last_val[ch] = 0;
        s->hk_last_ns[ch] = -1;
    }
}

/*static void grlib_stixctrl_timer_tick(void *opaque)
{
    STIXCTRL *stixctrl = opaque;

    stixctrl->time_ticks++;
    stixctrl->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, grlib_stixctrl_timer_tick, stixctrl);
    timer_mod_ms(stixctrl->timer, 1);
}*/

#define STIX_NSEC_PER_SEC 1000000000LL
#define STIX_SUBSEC_PER_SEC 65536LL

static stixCtrl_scet_s nsToSCET(int64_t ns)
{
	/* ns >= 0 by construction: the correction is only ever set to
	 * now - target, and the elapsed time since then is >= 0. */
	int64_t coarse = ns / STIX_NSEC_PER_SEC;
	int64_t rem = ns - coarse * STIX_NSEC_PER_SEC;
	stixCtrl_scet_s scet;

	scet.coarse = (uint32_t)coarse;
	scet.fine = (uint16_t)(rem * STIX_SUBSEC_PER_SEC / STIX_NSEC_PER_SEC);

	return scet;
}

/* D-J: the live SCET, without a guest register access (grlib_stix.h) */
void stixctrl_scet_now(STIXCTRL *s, uint32_t *coarse, uint16_t *fine)
{
    stixCtrl_scet_s scet = nsToSCET(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                                    s->time_correction_ns);

    *coarse = scet.coarse;
    *fine = scet.fine;
}

/*
 * D-5: the SCET exactly as the 0xC8/0xCC read composes it (bit 31 of
 * the seconds is the UNSYNCED status flag), for the detector chunks
 */
void stixctrl_scet_reg(STIXCTRL *s, uint32_t *coarse_reg, uint16_t *fine)
{
    stixCtrl_scet_s scet = nsToSCET(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                                    s->time_correction_ns);

    *coarse_reg = scet.coarse | (s->unsynced_flag << 31);
    *fine = scet.fine;
}

void stixctrl_set_scet_commit_hook(STIXCTRL *s,
                                   StixCtrlScetCommitFn fn, void *opaque)
{
    s->scet_commit_fn = fn;
    s->scet_commit_opaque = opaque;
}

/* Watchdog, from the FSW driver (BSW WDT.c header comment, WDT.c,
 * HWControl/Watchdog.c): not running at power-up; RELOAD loads COUNT from
 * RELOADVAL and enables counting; DISABLE stops counting, ENABLE resumes
 * it; CHANGE sets RELOADVAL only; FORCE resets the IDPU at once. COUNT
 * drops by one every 105 ms (WATCHDOG_CYCLE_DURATION) and the IDPU resets
 * when it reaches 0 (~6.6 s at the FSW's 63 cycles). Before this the
 * register read back the raw last write (COUNT = 0), so the FSW's
 * Watchdog task logged "invalid number of cycles" every second and
 * raised FDIR — a path the hardware never takes. */
#define STIX_WDG_CYCLE_NS (105 * 1000 * 1000LL)

static uint32_t stixctrl_wdg_count(STIXCTRL *s)
{
    int64_t cycles;

    if (!s->wdg_running) {
        return s->wdg_count;
    }
    cycles = (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->wdg_base_ns)
             / STIX_WDG_CYCLE_NS;
    return cycles >= s->wdg_count ? 0 : s->wdg_count - (uint32_t)cycles;
}

/* (Re)start counting down from <count> now; arm the expiry. */
static void stixctrl_wdg_run(STIXCTRL *s, uint32_t count)
{
    s->wdg_count = count;
    s->wdg_running = true;
    s->wdg_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_mod_ns(s->wdg_timer, s->wdg_base_ns + count * STIX_WDG_CYCLE_NS);
}

static void stixctrl_wdg_stop(STIXCTRL *s)
{
    s->wdg_count = stixctrl_wdg_count(s);
    s->wdg_running = false;
    timer_del(s->wdg_timer);
}

static void stixctrl_wdg_expired(void *opaque)
{
    STIXCTRL *s = opaque;

    trace_stixctrl_watchdog_reset("count expired");
    s->wdg_running = false;
    s->wdg_count = 0;
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

static void stixctrl_wdg_write(STIXCTRL *s, uint32_t value)
{
    uint32_t cmd = (value >> STIXCTRL_WATCHDOG_COMMAND) & STIXCTRL_WATCHDOG_COMMAND_MASK;

    switch (cmd) {
    case STIXCTRL_WATCHDOG_COMMAND_V_RELOAD:
        stixctrl_wdg_run(s, s->wdg_reload);
        break;
    case STIXCTRL_WATCHDOG_COMMAND_V_FORCE:
    case STIXCTRL_WATCHDOG_COMMAND_V_FORCE1:
    case STIXCTRL_WATCHDOG_COMMAND_V_FORCE2:
        stixctrl_wdg_stop(s);
        trace_stixctrl_watchdog_reset("FORCE command");
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        break;
    case STIXCTRL_WATCHDOG_COMMAND_V_DISABLE:
        if (s->wdg_running) {
            stixctrl_wdg_stop(s);
        }
        break;
    case STIXCTRL_WATCHDOG_COMMAND_V_ENABLE:
        if (!s->wdg_running) {
            stixctrl_wdg_run(s, s->wdg_count);
        }
        break;
    case STIXCTRL_WATCHDOG_COMMAND_V_CHANGE:
        s->wdg_reload = (value >> STIXCTRL_WATCHDOG_RELOADVAL) & STIXCTRL_WATCHDOG_RELOADVAL_MASK;
        break;
    default: /* SAFE: no action */
        break;
    }
    trace_stixctrl_watchdog_cmd(cmd, s->wdg_reload, stixctrl_wdg_count(s), s->wdg_running);
}

/*
 * rotbCmdSta (0x54) is composed on read: the stored COMMAND bits (1:0)
 * plus live status — READY follows rotbConfig.ENABLE (the FSW's
 * RotBuf_init polls it with a fatal timeout), FULL is the ring state,
 * BLOCKCOUNT (23:7) the block count, and the EDAC status comes from the
 * EDAC unit (M-1): SECCOUNT (31:24) and DEDETECTED (4), 0 with no
 * upsets. The EDACRESET strobe (bit 2) is never stored, so it reads 0.
 */
static uint32_t stixctrl_rotb_cmdsta(STIXCTRL *s)
{
    uint32_t cfg = ((volatile const uint32_t *)&s->reg)[0x58 / sizeof(uint32_t)];
    uint32_t sta = ((volatile const uint32_t *)&s->reg)[0x54 / sizeof(uint32_t)];

    return (sta & 0x7u) |
           (((cfg >> STIXCTRL_ROTBCONFIG_ENABLE) & 1u)
            << STIXCTRL_ROTBCMDSTA_READY) |
           ((s->rotb_full ? 1u : 0u) << STIXCTRL_ROTBCMDSTA_FULL) |
           ((s->rotb_count & STIXCTRL_ROTBCMDSTA_BLOCKCOUNT_MASK)
            << STIXCTRL_ROTBCMDSTA_BLOCKCOUNT) |
           stix_edac_rotb_status(&s->edac);
}

/*
 * the block at the write pointer becomes the newest one in the ring
 * (PUTBLOCK in TESTMODE, and the detector chunks, D-5); the caller
 * checks that it is not full
 */
static void stixctrl_rotb_push(STIXCTRL *s)
{
    s->rotb_wr = (s->rotb_wr + 1) % s->rotb_blocks;
    s->rotb_count++;
    s->rotb_full = s->rotb_count == s->rotb_blocks;
}

/*
 * D-5 (detector plan D-Dg): the hardware fill path. The detector model
 * hands over one chunk at a nominal swap (timer context, BQL held, the
 * device's re-entrancy guard engaged by the caller). It is appended only
 * with rotbConfig ENABLE = 1 and TESTMODE = 0 (in TESTMODE the window and
 * PUTBLOCK own the ring, §12). It goes in whole or not at all: with fewer
 * free blocks than the chunk it is dropped and IRQ 7 pulses once (the
 * docs say the hardware overwrites the oldest block; dropping keeps the
 * §12 drop-new rule and never leaves a partial chunk, plan Doc vs code
 * 7). Reaching capacity only sets FULL, as for PUTBLOCK.
 */
StixDetRotbResult stixctrl_rotb_append(STIXCTRL *s, const uint8_t *blocks,
                                       uint32_t n_blocks,
                                       uint32_t *rotb_count)
{
    uint32_t cfg = s->reg.rotbConfig;
    uint32_t i;

    *rotb_count = s->rotb_count;
    if (!((cfg >> STIXCTRL_ROTBCONFIG_ENABLE) & 1u)) {
        return STIX_DET_ROTB_DISABLED;
    }
    if ((cfg >> STIXCTRL_ROTBCONFIG_TESTMODE) & 1u) {
        return STIX_DET_ROTB_TESTMODE;
    }
    if (s->rotb_blocks - s->rotb_count < n_blocks) {
        qemu_irq_pulse(s->irq_rotb);
        return STIX_DET_ROTB_FULL;
    }
    for (i = 0; i < n_blocks; i++) {
        memcpy(&s->rotb_ring[s->rotb_wr * STIXCTRL_BLOCK_SIZE],
               blocks + i * STIXCTRL_BLOCK_SIZE, STIXCTRL_BLOCK_SIZE);
        /* M-4 (D-Uc write rule): the FPGA rewrite cures the cell */
        stix_edac_host_write(&s->edac, STIX_EDAC_ROTBUF,
                             (hwaddr)s->rotb_wr * STIXCTRL_BLOCK_SIZE,
                             STIXCTRL_BLOCK_SIZE);
        stixctrl_rotb_push(s);
    }
    *rotb_count = s->rotb_count;
    return STIX_DET_ROTB_APPENDED;
}

/*
 * D-Uc (M-2): stix_det's host-side writes into the CACHE SRAM (the
 * LACC/EACC materialization) retire every poison they overwrite, like a
 * guest write — the FPGA rewrite cures the cell. @off is a
 * STIXCTRL_SRAM offset, @len its length in bytes.
 */
void stixctrl_edac_sram_write(STIXCTRL *s, hwaddr off, hwaddr len)
{
    stix_edac_host_write(&s->edac, STIX_EDAC_CACHE, off, len);
}

/* ROTB FIFO commands, from the FSW driver (RotBuf.c
 * RotBuf_advancePointer/RotBuf_clear, OpModeMgmt.c mode switches):
 * GETBLOCK advances the read pointer, PUTBLOCK the write pointer
 * (TESTMODE only — in normal mode the detectors' HW feeds blocks),
 * RESETFIFO clears the pointers. They execute on EVERY 0x54 write and
 * regardless of ENABLE (lenient; visible in the rotb_cmd trace). The FSW
 * issues each command as a read-modify-write that clears COMMAND first,
 * so no write re-issues a stale command. The EDAC strobe-clear write
 * 0xFFFFFFFB carries COMMAND = 3, so it executes RESETFIFO — a
 * documented side effect (Q7), harmless at the FSW's call sites, which
 * clear the FIFO anyway. */
static void stixctrl_rotb_command(STIXCTRL *s, uint32_t cmd)
{
    uint32_t cfg = ((volatile const uint32_t *)&s->reg)[0x58 / sizeof(uint32_t)];
    bool testmode = (cfg >> STIXCTRL_ROTBCONFIG_TESTMODE) & 1u;

    switch (cmd) {
    case STIXCTRL_ROTBCMDSTA_COMMAND_V_GETBLOCK:
        if (s->rotb_count > 0) {
            s->rotb_rd = (s->rotb_rd + 1) % s->rotb_blocks;
            s->rotb_count--;
            s->rotb_full = false;
        }
        /* else: GETBLOCK on an empty ring holds the pointers */
        break;
    case STIXCTRL_ROTBCMDSTA_COMMAND_V_PUTBLOCK:
        if (!testmode) {
            trace_stixctrl_rotb_ignored("putblock-not-testmode", 0);
            break;
        }
        if (s->rotb_full) {
            /* D-F: a PUTBLOCK into a full ring is dropped (drop-new) and
             * pulses IRQ 7 once per rejected write; reaching capacity
             * only sets FULL. The SW-PUTBLOCK half of the rule is an
             * assumption until the HSWICD confirms it. */
            trace_stixctrl_rotb_overflow(s->rotb_count);
            qemu_irq_pulse(s->irq_rotb);
            break;
        }
        stixctrl_rotb_push(s);
        break;
    case STIXCTRL_ROTBCMDSTA_COMMAND_V_RESETFIFO:
        s->rotb_rd = s->rotb_wr = s->rotb_count = 0;
        s->rotb_full = false;
        break;
    default: /* COMMAND 0: no-op */
        break;
    }

    trace_stixctrl_rotb_cmd(cmd, s->rotb_rd, s->rotb_wr, s->rotb_count,
                            s->rotb_full);
}

/*
 * BLOCK window (0xE0800000, one 512 B block): the SW view of the ring.
 * Reads show the block at the read pointer, in any mode; TESTMODE writes
 * land in the block at the write pointer. M-4: the EDAC reactions run
 * where the CPU touches the ring (stix_edac_rotb_check on the read, the
 * write-clear on the store; no overlay — the ring has no MemoryRegion).
 */
static uint64_t grlib_stixctrl_rotb_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    STIXCTRL *s = opaque;
    hwaddr off = addr & (STIXCTRL_BLOCK_SIZE - 1);
    const uint8_t *p = &s->rotb_ring[s->rotb_rd * STIXCTRL_BLOCK_SIZE + off];
    uint64_t val = ldn_be_p(p, size);

    stix_edac_rotb_check(&s->edac, s->rotb_rd, (uint32_t)off, size, &val);
    return val;
}

static void grlib_stixctrl_rotb_write(void *opaque, hwaddr addr,
                                      uint64_t value, unsigned size)
{
    STIXCTRL *s = opaque;
    hwaddr off = addr & (STIXCTRL_BLOCK_SIZE - 1);
    uint32_t cfg = ((volatile const uint32_t *)&s->reg)[0x58 / sizeof(uint32_t)];
    bool testmode = (cfg >> STIXCTRL_ROTBCONFIG_TESTMODE) & 1u;

    /* the window is the TESTMODE fill path: stores are ignored while
     * TESTMODE is off or the ring is full. They are block stores, not
     * PUTBLOCK commands, so they never pulse IRQ 7 (D-F). */
    if (!testmode || s->rotb_full) {
        trace_stixctrl_rotb_ignored(!testmode ? "window-write-not-testmode"
                                              : "window-write-full",
                                    (uint32_t)off);
        return;
    }

    stn_be_p(&s->rotb_ring[s->rotb_wr * STIXCTRL_BLOCK_SIZE + off], size,
             value);
    /* M-4 (D-Uc): the store cures every poisoned word it touches */
    stix_edac_rotb_write(&s->edac, s->rotb_wr, (uint32_t)off, size);
}

static const MemoryRegionOps grlib_stixctrl_rotb_ops = {
    .read = grlib_stixctrl_rotb_read,
    .write = grlib_stixctrl_rotb_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/*
 * P3-1 (D-P3a): nominal HK ADC raw values, recon-hk-adc §4 "Proposed
 * raw". Every value sits inside both the BSW boot self-test window
 * (HK.c:34-39) and the PFM/IDB soft limits, so the build flavour (AX or
 * PFM) and a flash context with flight limits do not matter. Unused
 * channels read 0. The state-dependent entries (3, 4, 12, 16, 20, 48,
 * 52) are never read — stixctrl_hk_value() overrides them; they are
 * kept as the off/base value for documentation. ATT_C (8) only with
 * att-model=off (A-2). The FSW checks the
 * SPW0/1_V, ATT_V and HV lines only while POWERSTAT reports the supply
 * on (HKMonitor.c:357-370); since P4-1 it does (stixctrl_hkcmdsta), and
 * the "on" values here pass those checks.
 */
static const uint16_t stixctrl_hk_adc_table[STIXCTRL_HK_CHANNELS] = {
    [0] = 700,   /* DPU_1V5_C, 162..2185 */
    [1] = 800,   /* DPU_2V5_C, 0..1991 */
    [2] = 1009,  /* DPU_3V3_C, 132..4037 */
    [3] = 50,    /* DPU_SPW_C: overridden from SPWEN, 0 lines 0..294 */
    [4] = 20,    /* DET_C: overridden, 0 quarters 0..80 */
    /*
     * A-2 (D-Af): overridden from the attenuator mechanism (idle 20;
     * 120/220 with one/both motors moving, 320/620 stalled); with
     * att-model=off this static 20 is what ch 8 reads
     */
    [8] = 20,
    [12] = 10,   /* ATT_V off; on: 1670 from LVEN, 585..3370 */
    [16] = 16,   /* HV_17_32_V off; on: 1000 from HV1732EN, 0..2543 */
    [20] = 15,   /* HV_01_16_V off; on: 1000 from HV0116EN, 0..2543 */
    [24] = 2048, /* DPU_1V5_V, BSW self-test 1951..2193 */
    [28] = 3413, /* DPU_2V5_V, BSW self-test 3357..3473 */
    [32] = 2450, /* DET_Q2_T, 1690..4023 (~20 degC) */
    [33] = 2450, /* DET_Q1_T */
    [34] = 2450, /* DET_Q4_T */
    [35] = 2450, /* DET_Q3_T */
    [36] = 2450, /* ATT_T, 1196..3867 */
    [40] = 2300, /* PSU_T, 824..3800 */
    [44] = 3958, /* DPU_2V9_V, BSW self-test 3784..4094 */
    [48] = 20,   /* SPW0_V off; on: 3240 from SPWEN bit 0, 3011..3468 */
    [52] = 20,   /* SPW1_V off; on: 3240 from SPWEN bit 1 */
    [56] = 2200, /* DPU_PCB_T, 824..3800 */
    [60] = 2000, /* DPU_FPGA_T, 497..3800 */
    [64] = 100,  /* ASP_PHOTOA0_V, 0..4095 */
    [65] = 100,  /* ASP_PHOTOA1_V */
    [66] = 2450, /* ASP_TIM05_T, ~20 degC */
    [67] = 2450, /* ASP_TIM06_T */
    [68] = 3125, /* ASP_REF_2V5A_V, 3050..3473 */
    [69] = 2450, /* ASP_TIM01_T */
    [70] = 2450, /* ASP_TIM03_T, TIM03/04 <= 3921 */
    [71] = 1700, /* ASP_VSENSA_V, 1316..2095 */
    [72] = 100,  /* ASP_PHOTOB0_V */
    [73] = 100,  /* ASP_PHOTOB1_V */
    [74] = 2450, /* ASP_TIM08_T */
    [75] = 2450, /* ASP_TIM07_T */
    [76] = 3125, /* ASP_REF_2V5B_V, 3050..3473 */
    [77] = 2450, /* ASP_TIM02_T */
    [78] = 2450, /* ASP_TIM04_T */
    [79] = 1700, /* ASP_VSENSB_V, 1316..2095 */
};

/*
 * The "truth" ADC value for one channel (platform plan D-PLo, PL-2a):
 * the static nominal table plus the overrides that follow the CURRENT
 * commanded register state (hkCmdSta, idefxConfig and, for ch 8, the
 * attenuator mechanism — as they stand when the hkAdc write happens),
 * without any injection. POWERSTAT (hkCmdSta bits 9:5) is not
 * consulted: the values follow the commanded enables, which POWERSTAT
 * mirrors (stixctrl_hkcmdsta). Pure: no register is written (the ch 8
 * advance is idempotent, see stix_att.c).
 */
static uint32_t stixctrl_hk_truth(STIXCTRL *s, uint32_t ch)
{
    uint32_t cmdsta;
    uint32_t spwen;

    cmdsta = s->reg.hkCmdSta;
    spwen = (cmdsta >> STIXCTRL_HKCMDSTA_SPWEN) & STIXCTRL_HKCMDSTA_SPWEN_MASK;

    switch (ch) {
    case STIXCTRL_HKADC_ADDR_V_HK_DPU_SPW_C: /* 3 */
        /* spw_c_limits[n], n = the SPWEN line count (recon §4) */
        if (spwen == 0) {
            return 50;
        }
        return spwen == 3 ? 1830 : 900;
    case STIXCTRL_HKADC_ADDR_V_HK_DET_C: /* 4 */
        /*
         * det_c_limits[n]; idefxConfig[3:0] = the quarter power
         * (Detectors.c:555-558)
         */
        return 20u + 75u * ctpop32(s->reg.idefxConfig & 0xFu);
    case STIXCTRL_HKADC_ADDR_V_HK_ATT_C: /* 8 */
        /*
         * A-2 (D-Af): the mechanism's current; att-model=off keeps the
         * static table value (D-Ab)
         */
        return s->att.cfg.model ? stix_att_att_c_truth(&s->att)
                                : stixctrl_hk_adc_table[ch];
    case STIXCTRL_HKADC_ADDR_V_HK_ATT_V: /* 12 */
        return ((cmdsta >> STIXCTRL_HKCMDSTA_LVEN) &
                STIXCTRL_HKCMDSTA_LVEN_MASK) ? 1670 : 10;
    case STIXCTRL_HKADC_ADDR_V_HK_HV_17_32_V: /* 16 */
        return ((cmdsta >> STIXCTRL_HKCMDSTA_HV1732EN) &
                STIXCTRL_HKCMDSTA_HV1732EN_MASK) ? 1000 : 16;
    case STIXCTRL_HKADC_ADDR_V_HK_HV_01_16_V: /* 20 */
        return ((cmdsta >> STIXCTRL_HKCMDSTA_HV0116EN) &
                STIXCTRL_HKCMDSTA_HV0116EN_MASK) ? 1000 : 15;
    case STIXCTRL_HKADC_ADDR_V_HK_DPU_SPW0_V: /* 48 */
        /* no single-bit define: hkCmdSta bit 3 = SPWEN bit 0 */
        return ((cmdsta >> STIXCTRL_HKCMDSTA_SPWEN) & 1u) ? 3240 : 20;
    case STIXCTRL_HKADC_ADDR_V_HK_DPU_SPW1_V: /* 52 */
        /* no single-bit define: hkCmdSta bit 4 = SPWEN bit 1 */
        return ((cmdsta >> (STIXCTRL_HKCMDSTA_SPWEN + 1)) & 1u) ? 3240 : 20;
    default:
        return stixctrl_hk_adc_table[ch];
    }
}

/*
 * The value the FSW reads (the "presented" value, D-PLo): P4-2's
 * injection beats everything (D-Af); otherwise the truth, with ATT_C
 * (ch 8) through the attenuator's presented layer (A-2).
 */
static uint32_t stixctrl_hk_value(STIXCTRL *s, uint32_t ch)
{
    if (s->adc_inject[ch] != STIXCTRL_ADC_INJECT_NONE) {
        return s->adc_inject[ch];
    }
    if (ch == STIXCTRL_HKADC_ADDR_V_HK_ATT_C && s->att.cfg.model) {
        return stix_att_att_c_presented(&s->att);
    }
    return stixctrl_hk_truth(s, ch);
}

/*
 * A-2 (D-Af): the same split for the attenuator child's truth.att-c /
 * presented.att-c getters (grlib_stix.h); no guest access involved.
 */
uint32_t stixctrl_hk_att_c_truth(STIXCTRL *s)
{
    return stixctrl_hk_truth(s, STIXCTRL_HKADC_ADDR_V_HK_ATT_C);
}

uint32_t stixctrl_hk_att_c_presented(STIXCTRL *s)
{
    return stixctrl_hk_value(s, STIXCTRL_HKADC_ADDR_V_HK_ATT_C);
}

/*
 * P4-1 (D-P4c): hkCmdSta as the FSW reads it. POWERSTAT (bits 9:5) is
 * the PSU power-good of the supplies (PSU.h:36-43): bit 5 LV, 6 HV01,
 * 7 HV17, 8 SPW0, 9 SPW1, i.e. the commanded enables in bits 4:0
 * (StixIdpu.h POWERSTAT_V_*). The model has no power-up delay (the FSW
 * waits 10 ms before it looks). The real FPGA also cuts a supply on a
 * protection trip (latched until the enable is cleared); the model
 * does not (D-P4d), so the blockade mask is always 0 for now. A supply
 * is therefore good exactly while it is commanded on. The write handler
 * keeps bits 9:5 of the stored value clear.
 */
static uint32_t stixctrl_hkcmdsta(STIXCTRL *s)
{
    uint32_t cmdsta = s->reg.hkCmdSta;
    /*
     * supplies cut by a protection trip: none. A later cut option would
     * set the line's supply bit here when stixctrl_hk_prot_sample sets
     * PROTSTAT (latched until the FSW clears the enable, ICD 4.46).
     */
    uint32_t blockade = 0;
    uint32_t enables;

    if (!s->powerstat) {
        return cmdsta;
    }

    /* bits 4:0 = LVEN, HV0116EN, HV1732EN, SPWEN[1:0], in POWERSTAT order */
    enables = (cmdsta >> STIXCTRL_HKCMDSTA_LVEN) & STIXCTRL_HKCMDSTA_LVEN_MASK;
    enables |= ((cmdsta >> STIXCTRL_HKCMDSTA_HV0116EN) &
                STIXCTRL_HKCMDSTA_HV0116EN_MASK) << 1;
    enables |= ((cmdsta >> STIXCTRL_HKCMDSTA_HV1732EN) &
                STIXCTRL_HKCMDSTA_HV1732EN_MASK) << 2;
    enables |= ((cmdsta >> STIXCTRL_HKCMDSTA_SPWEN) &
                STIXCTRL_HKCMDSTA_SPWEN_MASK) << 3;

    return cmdsta | ((enables & ~blockade) << STIXCTRL_HKCMDSTA_POWERSTAT);
}

/*
 * P4-4 (D-P4d): HK hardware protection (ICD §4.48-4.52, DD §5.6).
 * Line i (hkProtConfig[i], selector i + 1) watches one HK ADC channel
 * and owns IRQEN/PROTSTAT bit 1 << (8 - i): 1V5 = 0x100 ... HV01 =
 * 0x001. The reset thresholds are ICD Table 4-75; ProtEn resets to 0.
 */
static const uint8_t stixctrl_hk_prot_ch[STIXCTRL_HK_PROT_LINES] = {
    STIXCTRL_HKADC_ADDR_V_HK_DPU_1V5_C,     /* 0 */
    STIXCTRL_HKADC_ADDR_V_HK_DPU_2V5_C,     /* 1 */
    STIXCTRL_HKADC_ADDR_V_HK_DPU_3V3_C,     /* 2 */
    STIXCTRL_HKADC_ADDR_V_HK_DPU_SPW_C,     /* 3 */
    STIXCTRL_HKADC_ADDR_V_HK_DET_C,         /* 4 */
    STIXCTRL_HKADC_ADDR_V_HK_ATT_C,         /* 8 */
    STIXCTRL_HKADC_ADDR_V_HK_ATT_V,         /* 12 */
    STIXCTRL_HKADC_ADDR_V_HK_HV_17_32_V,    /* 16 */
    STIXCTRL_HKADC_ADDR_V_HK_HV_01_16_V,    /* 20 */
};

static const uint16_t stixctrl_hk_prot_reset_thr[STIXCTRL_HK_PROT_LINES] = {
    2282, 3416, 3826, 3826, 546, 751, 4095, 2323, 2323,
};

#define STIXCTRL_HK_PROT_EN (1u << STIXCTRL_HKPROTCONFIG_PROTEN)
#define STIXCTRL_HK_PROT_CFG_MASK \
    (STIXCTRL_HK_PROT_EN | \
     (STIXCTRL_HKPROTCONFIG_UPTHRESHOLD_MASK << \
      STIXCTRL_HKPROTCONFIG_UPTHRESHOLD))

static uint32_t stixctrl_hk_prot_bit(uint32_t line)
{
    return 1u << (STIXCTRL_HK_PROT_LINES - 1 - line);
}

static void stixctrl_hk_prot_reset(STIXCTRL *s)
{
    uint32_t i;

    s->reg.hkIrqSta = 0;
    for (i = 0; i < STIXCTRL_HK_PROT_LINES; i++) {
        s->reg.hkProtConfig[i] = stixctrl_hk_prot_reset_thr[i];
        s->hk_prot_high[i] = 0;
    }
    s->reg.unlockingReg = 0;
    s->hk_prot_unlocked = false;
}

/*
 * One comparator sample of channel ch. The real loop samples every
 * line continuously (< 1 ms) and needs two consecutive samples 850 us
 * apart strictly above the threshold ("exceeded in two consecutive
 * measurements", ICD 4.48). The model samples a line only at these
 * events: a guest conversion of its channel (an hkAdc write) and a set
 * or clear of its adc-inject; "consecutive" means two such events in a
 * row above threshold, and a sample at or below it restarts the count.
 * The count runs with ProtEn off too; PROTSTAT sets only with ProtEn
 * (the turn-off action "is only performed when it is enabled", DD). A
 * newly set bit latches the sample in HOTSAMPLE (one shared field,
 * last trip wins) and pulses irq_hk if its IRQEN is set. A bit that is
 * still set is not re-raised; once cleared, the next high sample sets
 * it again. There is no supply cut (D-P4d): see stixctrl_hkcmdsta.
 */
static void stixctrl_hk_prot_sample(STIXCTRL *s, uint32_t ch, uint32_t val)
{
    uint32_t line, cfg, thr, bit, protstat, irqen;

    if (!s->hk_prot || !s->hk_adc) {
        return;
    }
    for (line = 0; line < STIXCTRL_HK_PROT_LINES; line++) {
        if (stixctrl_hk_prot_ch[line] == ch) {
            break;
        }
    }
    if (line == STIXCTRL_HK_PROT_LINES) {
        return;
    }

    cfg = s->reg.hkProtConfig[line];
    thr = (cfg >> STIXCTRL_HKPROTCONFIG_UPTHRESHOLD) &
          STIXCTRL_HKPROTCONFIG_UPTHRESHOLD_MASK;
    if (val <= thr) {
        s->hk_prot_high[line] = 0;
        return;
    }
    if (s->hk_prot_high[line] < 2) {
        s->hk_prot_high[line]++;
    }
    if (s->hk_prot_high[line] < 2 || !(cfg & STIXCTRL_HK_PROT_EN)) {
        return;
    }

    bit = stixctrl_hk_prot_bit(line);
    protstat = (s->reg.hkIrqSta >> STIXCTRL_HKIRQSTA_PROTSTAT) &
               STIXCTRL_HKIRQSTA_PROTSTAT_MASK;
    if (protstat & bit) {
        return;
    }
    irqen = (s->reg.hkIrqSta >> STIXCTRL_HKIRQSTA_IRQEN) &
            STIXCTRL_HKIRQSTA_IRQEN_MASK;
    s->reg.hkIrqSta &= ~(STIXCTRL_HKIRQSTA_HOTSAMPLE_MASK <<
                         STIXCTRL_HKIRQSTA_HOTSAMPLE);
    s->reg.hkIrqSta |= (bit << STIXCTRL_HKIRQSTA_PROTSTAT) |
                       ((val & STIXCTRL_HKIRQSTA_HOTSAMPLE_MASK) <<
                        STIXCTRL_HKIRQSTA_HOTSAMPLE);
    trace_stixctrl_hk_prot_trip(line + 1, ch, val, thr, !!(irqen & bit));
    if (irqen & bit) {
        qemu_irq_pulse(s->irq_hk);
    }
}

/*
 * hkIrqSta write: IRQEN (8:0) R/W, PROTSTAT (17:9) write-1-to-clear
 * (HK_clearProtectionStatus writes tmp | mask, then tmp: HK.c:272-277),
 * HOTSAMPLE (31:20) and 19:18 read-only. Enabling an IRQEN bit whose
 * PROTSTAT is already set raises nothing (irq_hk only pulses on a new
 * PROTSTAT bit; the FSW clears PROTSTAT before it arms, FDIR.c:540).
 */
static void stixctrl_hk_irqsta_write(STIXCTRL *s, uint32_t value)
{
    uint32_t sta = s->reg.hkIrqSta;
    uint32_t w1c = (value >> STIXCTRL_HKIRQSTA_PROTSTAT) &
                   STIXCTRL_HKIRQSTA_PROTSTAT_MASK;

    sta &= ~(w1c << STIXCTRL_HKIRQSTA_PROTSTAT);
    sta &= ~(STIXCTRL_HKIRQSTA_IRQEN_MASK << STIXCTRL_HKIRQSTA_IRQEN);
    sta |= value & (STIXCTRL_HKIRQSTA_IRQEN_MASK << STIXCTRL_HKIRQSTA_IRQEN);
    s->reg.hkIrqSta = sta;
}

/*
 * hkProtConfig[line] write: ProtEn (31) and the threshold (11:0); 30:12
 * read 0. "Write protected by UnlockingReg Token (except for protection
 * enabling)" (ICD 4.49): without the token only a ProtEn 0 -> 1 takes
 * effect, a threshold change or a ProtEn clear is dropped (traced).
 * The FSW unlocks right before every threshold write and ProtEn clear,
 * and sets ProtEn without it (HK.c:205-245). unlocked is the token
 * state before this write cycle; the caller has already spent it.
 */
static void stixctrl_hk_protconfig_write(STIXCTRL *s, uint32_t line,
                                         uint32_t value, bool unlocked)
{
    uint32_t old = s->reg.hkProtConfig[line];
    uint32_t cfg = value & STIXCTRL_HK_PROT_CFG_MASK;

    if (!unlocked) {
        uint32_t locked = old | (cfg & STIXCTRL_HK_PROT_EN);

        if (locked != cfg) {
            trace_stixctrl_hk_prot_locked(line + 1, (uint32_t)value, locked);
        }
        cfg = locked;
    }
    s->reg.hkProtConfig[line] = cfg;
}

static uint64_t grlib_stixctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    STIXCTRL *stixctrl = opaque;

    // update timer constantly // TODO: check if this should only be done on a read of 0xC8 or 0xCC
    stixCtrl_scet_s scet = nsToSCET(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - stixctrl->time_correction_ns);
    /* bit 31 = UNSYNCED status flag (Timing.c TIMING_COARSE_UNSYNCED);
     * the FSW reads and checks it (Timing.c:136). */
    stixctrl->reg.longTimerSec = scet.coarse | (stixctrl->unsynced_flag << 31);
    stixctrl->reg.longTimerSubSec = scet.fine;
    stixctrl->reg.watchDog =
        (stixctrl->wdg_reload << STIXCTRL_WATCHDOG_RELOADVAL) |
        (stixctrl_wdg_count(stixctrl) << STIXCTRL_WATCHDOG_COUNT);

    /*
     * 0x50, 0x54 and 0x88 are composed live (cacheCmdSta via the EDAC
     * unit, stixctrl_rotb_cmdsta, stixctrl_hkcmdsta); the rest is stored
     */
    if (addr == 0x50 /* cacheCmdSta */) {
        uint32_t regval = stix_edac_cache_read(&stixctrl->edac,
                                               stixctrl->reg.cacheCmdSta);

        trace_stixctrl_io_read((uint32_t)addr, regval);

        return regval;
    }
    if (addr == 0x54 /* rotbCmdSta */)
    {
        uint32_t regval = stixctrl_rotb_cmdsta(stixctrl);

        trace_stixctrl_io_read((uint32_t)addr, regval);

        return regval;
    }
    if (addr == 0x88 /* hkCmdSta */) {
        uint32_t regval = stixctrl_hkcmdsta(stixctrl);

        trace_stixctrl_io_read((uint32_t)addr, regval);

        return regval;
    }
    /* A-1: AttnCmdSta (0x5C), composed in stix_att.c */
    if (stix_att_owns(&stixctrl->att, addr)) {
        uint32_t regval = stix_att_read(&stixctrl->att, addr);

        trace_stixctrl_io_read((uint32_t)addr, regval);

        return regval;
    }
    /* D-1: 0x00-0x4C (detector controller), composed in stix_det.c */
    if (stix_det_owns(&stixctrl->det, addr)) {
        uint32_t regval = stix_det_read(&stixctrl->det, addr);

        trace_stixctrl_io_read((uint32_t)addr, regval);

        return regval;
    }

    uint32_t regval = ((volatile uint32_t *)&stixctrl->reg)[(uint32_t)addr / sizeof(uint32_t)];

    trace_stixctrl_io_read((uint32_t)addr, regval);

    return regval;
}

static void grlib_stixctrl_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    STIXCTRL *stixctrl = opaque;

    /*
     * P4-4 (D-P4d): UnlockingReg (0xD0) and the protection registers.
     * 0xA5 "unlocks protected registers for one write cycle" (ICD 4.52):
     * the token is spent by the next write to any register of this bank,
     * whatever it is (the FSW's next write is always the hkProtConfig
     * write, HK.c:213-214, :240-241). Only bits 7:0 (the Token field)
     * are compared, so 0xFFFFFFA5 unlocks too; any other value locks.
     * Writes to the ROTB block window or the AHB data windows are other
     * MemoryRegions and do not spend it. The token is write-only: 0xD0
     * reads 0.
     */
    if (stixctrl->hk_prot) {
        bool unlocked = stixctrl->hk_prot_unlocked;

        stixctrl->hk_prot_unlocked = false;
        if (addr == 0xD0 /* unlockingReg */) {
            stixctrl->hk_prot_unlocked =
                ((uint32_t)value & 0xFFu) == STIXCTRL_UNLOCKING_TOKEN;
            trace_stixctrl_hk_prot_unlock((uint32_t)value,
                                          stixctrl->hk_prot_unlocked);
            return;
        }
        if (addr == 0x90 /* hkIrqSta */) {
            stixctrl_hk_irqsta_write(stixctrl, (uint32_t)value);
            trace_stixctrl_io_write((uint32_t)addr, stixctrl->reg.hkIrqSta);
            return;
        }
        if (addr >= 0x94 && addr < 0x94 + 4 * STIXCTRL_HK_PROT_LINES) {
            uint32_t line = ((uint32_t)addr - 0x94) / 4;

            stixctrl_hk_protconfig_write(stixctrl, line, (uint32_t)value,
                                         unlocked);
            trace_stixctrl_io_write((uint32_t)addr,
                                    stixctrl->reg.hkProtConfig[line]);
            return;
        }
    }

    /*
     * A-1: the attenuator (0x5C), like the detector: after the hk-prot
     * block, so this write spends an unlock token like any other
     * (D-Aa); 0x60/0x64 stay plain storage below
     */
    if (stix_att_owns(&stixctrl->att, addr)) {
        stix_att_write(&stixctrl->att, addr, (uint32_t)value);
        trace_stixctrl_io_write((uint32_t)addr,
                                stix_att_cmdsta(&stixctrl->att));
        return;
    }

    /*
     * D-1: the detector controller (0x00-0x4C). After the hk-prot block,
     * so these writes spend an unlock token like any other.
     */
    if (stix_det_owns(&stixctrl->det, addr)) {
        /*
         * the stored value, as below (fsmCmd keeps only its config); a
         * dropped read-only write traces stixdet_ro_write_dropped instead
         */
        if (stix_det_write(&stixctrl->det, addr, (uint32_t)value)) {
            trace_stixctrl_io_write((uint32_t)addr,
                                    ((uint32_t *)&stixctrl->reg)
                                    [(uint32_t)addr / sizeof(uint32_t)]);
        }
        return;
    }

    if (addr == 0xD4 /* watchDog */)
    {
        stixctrl_wdg_write(stixctrl, (uint32_t)value);
        return;
    }

    // update the register as per write request
    // mostly automatic, special for timing
    if (addr == 0xC8 || addr == 0xCC)
    {
        /* SCET set, HSWICD 4.51 two-register commit (review T1; D1 amended
         * with Laszlo 2026-11-09 after the soak caught the flag bit). The
         * FSW (IIR.c IIR_setElapsedTime) writes 0xC8 then 0xCC:
         *  - 0xC8 STAGES the seconds field (bits 30:0) and the UNSYNCED
         *    status flag (bit 31 = Timing.c TIMING_COARSE_UNSYNCED); the
         *    running time is NOT touched ("When written SubSec and
         *    previously stored LongTimerSec.Secs fields will be set as a
         *    new value of on-board time").
         *  - 0xCC COMMITS the new on-board time: seconds = staged value,
         *    sub-seconds = value & 0xFFFF (1/65536 s units). Bits 31:16
         *    are status and do not affect the time. */
        if (addr == 0xC8)
        {
            stixctrl->staged_secs = value & 0x7FFFFFFFu;
            stixctrl->unsynced_flag = (uint32_t)(value >> 31) & 1u;
        }
        else
        {
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            int64_t new_subns = (int64_t)(value & 0xFFFFu) * STIX_NSEC_PER_SEC
                                / STIX_SUBSEC_PER_SEC;
            stixctrl->time_correction_ns = now -
                ((int64_t)stixctrl->staged_secs * STIX_NSEC_PER_SEC + new_subns);
            /* correction 6: the rollover moved; the SPW re-arms */
            if (stixctrl->scet_commit_fn != NULL) {
                stixctrl->scet_commit_fn(stixctrl->scet_commit_opaque);
            }
        }
        trace_stixctrl_scet_write((uint32_t)addr, (uint32_t)value);
    }

    /*
     * P3-1 (D-P3a): the HK ADC conversion completes immediately — the
     * real FPGA scans continuously and asserts READY per conversion
     * ("except the first 130 ms after reset", HK.c:142), so READY is
     * always set by the time the FSW's poll (HK.c:130-158) reads it;
     * the old model only forced READY and left VAL at 0, which failed
     * the BSW boot self-test. The value is the nominal raw reading of
     * the addressed channel (recon-hk-adc §4), with the state overrides
     * from the CURRENT hkCmdSta/idefxConfig. hk-adc=off keeps the
     * legacy behaviour for A/B comparison and rollback.
     */
    if (addr == 0x8c /* hkAdc */)
    {
        if (stixctrl->hk_adc) {
            uint32_t ch = (uint32_t)value & STIXCTRL_HKADC_ADDR_MASK;
            uint32_t val = stixctrl_hk_value(stixctrl, ch);

            /* PL-2: the last conversion, for hw-state (no side effect) */
            stixctrl->hk_last_val[ch] = val & STIXCTRL_HKADC_VAL_MASK;
            stixctrl->hk_last_ns[ch] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            value = ((val & STIXCTRL_HKADC_VAL_MASK) << STIXCTRL_HKADC_VAL) |
                    (1u << STIXCTRL_HKADC_READY) |
                    ch;
            /* P4-4: the protection comparator sees this conversion */
            stixctrl_hk_prot_sample(stixctrl, ch, val);
        } else {
            value |= (1u << STIXCTRL_HKADC_READY);
        }
    }

    /*
     * hkCmdSta (0x88), P4-1: POWERSTAT (9:5) is status, composed on read
     * (stixctrl_hkcmdsta). The PSU driver writes the register
     * read-modify-write (PSU.c:20-90), so a stored status bit would keep
     * a supply "good" after its enable is cleared; drop them.
     */
    if (addr == 0x88 /* hkCmdSta */ && stixctrl->powerstat) {
        value &= ~((uint64_t)STIXCTRL_HKCMDSTA_POWERSTAT_MASK <<
                   STIXCTRL_HKCMDSTA_POWERSTAT);
    }

    /*
     * cacheCmdSta (0x50), M-1: OPMODE, SCRUBRATE and EDACON are stored;
     * SECCOUNT (15:8) and DEDETECTED (18) are composed read-only, and
     * EDACRESET (bit 17) is a write-1 strobe that self-clears (the
     * EDAC.c:299-302 set-then-clear pair).
     */
    if (addr == 0x50 /* cacheCmdSta */) {
        value = stix_edac_cache_write(&stixctrl->edac,
                                      stixctrl->reg.cacheCmdSta,
                                      (uint32_t)value);
    }

    /*
     * rotbCmdSta (0x54): the writable field is COMMAND (1:0); BLOCKCOUNT,
     * SECCOUNT, READY, FULL and DEDETECTED are read-only status, composed
     * in the read handler (stixctrl_rotb_cmdsta). The EDACRESET strobe
     * (bit 2) clears the EDAC counter and latch and self-clears, so it
     * never lands in the stored word. The FSW's EDAC counter reset clears
     * its strobe with a full-register write of ~(1u << 2) = 0xFFFFFFFB
     * (EDAC.c); the baseline stored that raw and set BLOCKCOUNT to
     * 131071, so the FSW's HK level check asserted (HKAcquire.c:325) —
     * masked on 5.2 by the SCET abort killing runs ~4 FSW-seconds
     * earlier. The strobe-clear write carries COMMAND = 3, so it
     * executes RESETFIFO (harmless, see stixctrl_rotb_command).
     */
    if (addr == 0x54 /* rotbCmdSta */)
    {
        if ((value >> STIXCTRL_ROTBCMDSTA_EDACRESET) &
            STIXCTRL_ROTBCMDSTA_EDACRESET_MASK) {
            stix_edac_clear(&stixctrl->edac, STIX_EDAC_ROTBUF);
        }
        value &= STIXCTRL_ROTBCMDSTA_COMMAND_MASK;
    }

    ((volatile uint32_t *)&stixctrl->reg)[(uint32_t)addr / sizeof(uint32_t)] = (uint32_t)value;

    if (addr == 0x54 /* rotbCmdSta */)
    {
        stixctrl_rotb_command(stixctrl, (uint32_t)value & STIXCTRL_ROTBCMDSTA_COMMAND_MASK);
    }

    /* the stored value: 0x50, 0x54 and 0x88 are composed on read */
    trace_stixctrl_io_write((uint32_t)addr,
                            ((volatile uint32_t *)&stixctrl->reg) /* re-read */
                            [(uint32_t)addr / sizeof(uint32_t)]);
}

static const MemoryRegionOps grlib_stixctrl_ops = {
    .write = grlib_stixctrl_write,
    .read = grlib_stixctrl_read,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void grlib_stixctrl_realize(DeviceState *dev, Error **errp)
{
    STIXCTRL *STIXCTRL = GRLIB_STIXCTRL(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    /*
     * D-Ua: the EDAC unit (created in instance_init) is realized with
     * its parent; it owns the SEC counters/DeDetected latches and its
     * reset is its own qemu_register_reset hook
     */
    if (!qdev_realize(DEVICE(&STIXCTRL->edac), NULL, errp)) {
        return;
    }

    /* BLOCKCOUNT is 17 bits (23:7): the ring holds at most 0x1FFFF blocks
     * (the default 104448 costs ~53.5 MB of host RAM) */
    if (STIXCTRL->rotb_blocks == 0 ||
        STIXCTRL->rotb_blocks > STIXCTRL_ROTBCMDSTA_BLOCKCOUNT_MASK)
    {
        error_setg(errp, "rotb-blocks must be 1..%u, not %u",
                   STIXCTRL_ROTBCMDSTA_BLOCKCOUNT_MASK, STIXCTRL->rotb_blocks);
        return;
    }
    STIXCTRL->rotb_ring = g_malloc0((size_t)STIXCTRL->rotb_blocks * STIXCTRL_BLOCK_SIZE);

    sysbus_init_irq(sbd, &STIXCTRL->irq_detectors);
    sysbus_init_irq(sbd, &STIXCTRL->irq_hk);
    sysbus_init_irq(sbd, &STIXCTRL->irq_other);
    sysbus_init_irq(sbd, &STIXCTRL->irq_edac);
    sysbus_init_irq(sbd, &STIXCTRL->irq_rotb);

    memory_region_init_io(&STIXCTRL->iomem, OBJECT(STIXCTRL), &grlib_stixctrl_ops, STIXCTRL, "stixctrl", STIXCTRL_REG_SIZE);

    sysbus_init_mmio(sbd, &STIXCTRL->iomem);

    memory_region_init_io(&STIXCTRL->rotb_block_window, OBJECT(STIXCTRL), &grlib_stixctrl_rotb_ops, STIXCTRL, "stixctrl.rotb-block", STIXCTRL_BLOCK_SIZE);

    sysbus_init_mmio(sbd, &STIXCTRL->rotb_block_window);

    /* AHB data windows (plan §3.3, D-A/D-G): plain device RAM, zeroed at
     * power-on only, contents survive system reset. Sizes: SRAM has a
     * STIXCTRL_SRAM_SIZE macro; the LVAR window has none — 0x400 covers
     * LVAR0-3 (4 × 0x100 stride, ending 0xE020039F) plus the BRAM areas
     * behind them (WDT UNCHANGED_STORAGE 0xE02000A8, MemBram 64 B ending
     * 0xE0200400); LTRG and ETRG derive from their END/START macros. */
    if (!memory_region_init_ram(&STIXCTRL->sram, OBJECT(dev), "stixctrl.sram",
                                STIXCTRL_SRAM_SIZE, errp) ||
        !memory_region_init_ram(&STIXCTRL->lvar, OBJECT(dev), "stixctrl.lvar",
                                0x400, errp) ||
        !memory_region_init_ram(&STIXCTRL->ltrg, OBJECT(dev), "stixctrl.ltrg",
                                STIXCTRL_LTRG3_END - STIXCTRL_LTRG0_START + 1,
                                errp) ||
        !memory_region_init_ram(&STIXCTRL->etrg, OBJECT(dev), "stixctrl.etrg",
                                STIXCTRL_ETRG1_END - STIXCTRL_ETRG0_START + 1,
                                errp)) {
        return;
    }

    sysbus_init_mmio(sbd, &STIXCTRL->sram);
    sysbus_init_mmio(sbd, &STIXCTRL->lvar);
    sysbus_init_mmio(sbd, &STIXCTRL->ltrg);
    sysbus_init_mmio(sbd, &STIXCTRL->etrg);

    /*
     * D-Ua, M-2 (D-Uc): wire the CACHE memory to the EDAC unit — the
     * stored cacheCmdSta word gates the overlay reactions (EDACON
     * bit 16, OPMODE bit 0), the SRAM window is the backing RAM the
     * overlays sit on, and the sysbus slot pulses IRQMP line 6 (D-Ue).
     */
    stix_edac_cache_link(&STIXCTRL->edac, &STIXCTRL->reg.cacheCmdSta,
                         &STIXCTRL->sram, &STIXCTRL->irq_edac);

    /*
     * D-Ua, M-4: wire the ROTBUF memory to the EDAC unit — the ring the
     * detectors fill and the 512 B window exposes (no MemoryRegion, so
     * no overlay: the reactions run in the window handlers and the
     * append), the stored rotbConfig word gates (EDACON bit 12) and the
     * EDAC sysbus slot pulses the shared IRQMP line 6 (D-Ue).
     */
    stix_edac_rotb_link(&STIXCTRL->edac, &STIXCTRL->reg.rotbConfig,
                        STIXCTRL->rotb_ring, STIXCTRL->rotb_blocks,
                        &STIXCTRL->irq_edac);

    STIXCTRL->wdg_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, stixctrl_wdg_expired, STIXCTRL);

    if (!stix_det_realize(&STIXCTRL->det, STIXCTRL, &STIXCTRL->reg,
                          &STIXCTRL->sram, &STIXCTRL->lvar, &STIXCTRL->ltrg,
                          &STIXCTRL->etrg, &STIXCTRL->irq_detectors, errp)) {
        timer_free(STIXCTRL->wdg_timer);
        STIXCTRL->wdg_timer = NULL;
        g_free(STIXCTRL->rotb_ring);
        STIXCTRL->rotb_ring = NULL;
        return;
    }

    if (!stix_att_realize(&STIXCTRL->att, STIXCTRL, &STIXCTRL->att_cfg,
                          &STIXCTRL->reg, errp)) {
        stix_det_unrealize(&STIXCTRL->det);
        timer_free(STIXCTRL->wdg_timer);
        STIXCTRL->wdg_timer = NULL;
        g_free(STIXCTRL->rotb_ring);
        STIXCTRL->rotb_ring = NULL;
        return;
    }

    //STIXCTRL->reference_time = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    //STIXCTRL->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, grlib_stixctrl_timer_tick, STIXCTRL);
    //timer_mod(STIXCTRL->timer, 1);
}

static void grlib_stixctrl_reset(DeviceState *d)
{
    STIXCTRL *stixctrl = GRLIB_STIXCTRL(d);

    /* SCET staging state clears with the device; the running time keeps
     * its correction (a system reset does not rewind the on-board clock
     * the FSW has committed). */
    stixctrl->staged_secs = 0;
    stixctrl->unsynced_flag = 0;

    /* the watchdog is not running after a reset (WDT.c: "it is not
     * running at startup"); the FSW programs it again at init */
    timer_del(stixctrl->wdg_timer);
    stixctrl->wdg_running = false;
    stixctrl->wdg_count = 0;
    stixctrl->wdg_reload = 0;

    /* the ROTB FIFO pointers reset with the device; the ring CONTENT is
     * kept (zeroed only at realize). The FSW issues RESETFIFO at init
     * (RotBuf.c:78), so it never relies on content across a reset. */
    stixctrl->rotb_rd = 0;
    stixctrl->rotb_wr = 0;
    stixctrl->rotb_count = 0;
    stixctrl->rotb_full = false;

    /*
     * M-1: cacheCmdSta takes its ICD reset at every device reset, EdacOn
     * = 1 (STIX-ICD-0026 txt:2119; the model read 0 before). OpMode and
     * ScrubRate keep 0 against the ICD (txt:2124/txt:2122, accepted
     * diff): OPMODE 0 keeps the CACHE DE-to-IRQ path live with no FSW
     * setup, ScrubRate 0 keeps the scrub algorithm's OFF early-return.
     * The EdacOn bit follows the EDAC unit's edac-at-boot state (D-Uk).
     */
    stixctrl->reg.cacheCmdSta =
        stix_edac_boot_edacon(&stixctrl->edac, STIX_EDAC_CACHE)
        ? (1u << STIXCTRL_CACHECMDSTA_EDACON) : 0;

    /*
     * D-Uk: rotbConfig's EDACON takes the hand-over state; the other
     * bits keep today's reset behaviour (untouched)
     */
    stixctrl->reg.rotbConfig =
        (stixctrl->reg.rotbConfig &
         ~(STIXCTRL_ROTBCONFIG_EDACON_MASK << STIXCTRL_ROTBCONFIG_EDACON)) |
        (stix_edac_boot_edacon(&stixctrl->edac, STIX_EDAC_ROTBUF)
         << STIXCTRL_ROTBCONFIG_EDACON);

    /* the AHB windows (stixctrl.sram/.lvar/.ltrg/.etrg) are deliberately
     * NOT cleared here: they are plain RAM, zeroed at power-on only, and
     * their contents survive system reset (D-G) — the FSW's BRAM keeps
     * the reboot-persistent state (WDT UNCHANGED_STORAGE, MemBram). */

    /*
     * P4-2 (D-P4b): injections clear with the device, so a reboot is
     * deterministic and a bad SPW*_V cannot break the SpW init after a
     * watchdog reset
     */
    stixctrl_adc_inject_clear(stixctrl);
    stixctrl_hk_last_clear(stixctrl);

    /*
     * P4-4 (D-P4d): the protection registers take their ICD reset
     * values (reset does not clear reg); hk-prot=off keeps the legacy
     * plain storage untouched
     */
    if (stixctrl->hk_prot) {
        stixctrl_hk_prot_reset(stixctrl);
    }

    /*
     * D-1 (D-Dh): the detector registers 0x00-0x4C take explicit reset
     * values and the QL tick stops; det-model=off leaves them alone
     */
    stix_det_reset(&stixctrl->det);

    /*
     * A-1 (D-Ae): a running attenuator move aborts at the reset
     * instant, the registers go idle and the position is kept;
     * att-model=off leaves 0x5C alone
     */
    stix_att_reset(&stixctrl->att);
}

static void grlib_stixctrl_unrealize(DeviceState *dev)
{
    STIXCTRL *stixctrl = GRLIB_STIXCTRL(dev);

    qdev_unrealize(DEVICE(&stixctrl->edac));
    g_free(stixctrl->rotb_ring);
    stixctrl->rotb_ring = NULL;
    stix_det_unrealize(&stixctrl->det);
    stix_att_unrealize(&stixctrl->att);
}

/*
 * P4-2 (D-P4a): adc-inject[N] accessors, one class property per
 * channel; the channel number is the property's opaque. A setter (not
 * a plain uint32 pointer property) so the value is range-checked and
 * traced, and so the HK protection comparator can run on a set (P4-4).
 */
static void stixctrl_adc_inject_get(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp)
{
    STIXCTRL *s = GRLIB_STIXCTRL(obj);
    uint32_t value = s->adc_inject[GPOINTER_TO_UINT(opaque)];

    visit_type_uint32(v, name, &value, errp);
}

static void stixctrl_adc_inject_set(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp)
{
    STIXCTRL *s = GRLIB_STIXCTRL(obj);
    uint32_t ch = GPOINTER_TO_UINT(opaque);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    /* reject rather than mask, so a typo in a script is an error */
    if (value != STIXCTRL_ADC_INJECT_NONE &&
        value > STIXCTRL_HKADC_VAL_MASK) {
        error_setg(errp, "%s: %u is not a 12-bit ADC value "
                   "(0..%u, or %u to clear)", name, value,
                   STIXCTRL_HKADC_VAL_MASK, STIXCTRL_ADC_INJECT_NONE);
        return;
    }
    s->adc_inject[ch] = value;
    if (value == STIXCTRL_ADC_INJECT_NONE) {
        trace_stixctrl_adc_inject_clear(ch);
    } else {
        trace_stixctrl_adc_inject_set(ch, value);
    }
    /*
     * P4-4: the protection loop samples the new reading (a clear
     * samples the nominal value again, a low sample)
     */
    stixctrl_hk_prot_sample(s, ch, stixctrl_hk_value(s, ch));
}

/* PL-2: one supply line of hw-state.supplies */
static void stixctrl_hw_state_line(QDict *lines, const char *name,
                                   uint32_t enables, uint32_t good,
                                   uint32_t bit)
{
    QDict *l = qdict_new();

    qdict_put_bool(l, "enabled", (enables >> bit) & 1u);
    qdict_put_bool(l, "good", (good >> bit) & 1u);
    qdict_put(lines, name, l);
}

/*
 * PL-2 (platform plan D-PLo): the read-only hardware state as one JSON
 * object, sampled atomically on the main loop (the getter runs under
 * the BQL). Pure getters only: no MMIO handler runs (the CTRL read
 * handler stores the SCET and the watchdog into reg on every read), no
 * register, timer or IRQ is touched, so two reads with no virtual time
 * in between are equal. "schema" versions the layout; every index
 * names its space (ch = HK ADC channel, line = hkProtConfig selector,
 * quarter = idefxConfig bit).
 */
static char *stixctrl_get_hw_state(Object *obj, Error **errp)
{
    STIXCTRL *s = GRLIB_STIXCTRL(obj);
    g_autoptr(QDict) root = qdict_new();
    QDict *sup = qdict_new(), *lines = qdict_new();
    QDict *adc = qdict_new(), *prot = qdict_new(), *wdg = qdict_new();
    QDict *scet = qdict_new(), *det = qdict_new(), *model = qdict_new();
    QList *plines = qlist_new(), *qon = qlist_new();
    uint32_t cmdsta = stixctrl_hkcmdsta(s);
    uint32_t enables, good, sta, ch, line, q, coarse;
    uint16_t fine;
    GString *json;

    qdict_put_int(root, "schema", 1);
    qdict_put_int(root, "vclock_ns", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));

    qdict_put_bool(model, "hk_adc", s->hk_adc);
    qdict_put_bool(model, "powerstat", s->powerstat);
    qdict_put_bool(model, "hk_prot", s->hk_prot);
    qdict_put_bool(model, "det_model", s->det.model);
    qdict_put_bool(model, "att_model", s->att.cfg.model);
    qdict_put(root, "model", model);

    /* hkCmdSta bits 4:0 (LV, HV01, HV17, SPW0, SPW1) and POWERSTAT 9:5 */
    enables = cmdsta & 0x1Fu;
    good = (cmdsta >> STIXCTRL_HKCMDSTA_POWERSTAT) &
           STIXCTRL_HKCMDSTA_POWERSTAT_MASK;
    qdict_put_int(sup, "enables", enables);
    qdict_put_int(sup, "powerstat", good);
    stixctrl_hw_state_line(lines, "lv", enables, good, 0);
    stixctrl_hw_state_line(lines, "hv01", enables, good, 1);
    stixctrl_hw_state_line(lines, "hv17", enables, good, 2);
    stixctrl_hw_state_line(lines, "spw0", enables, good, 3);
    stixctrl_hw_state_line(lines, "spw1", enables, good, 4);
    qdict_put(sup, "lines", lines);
    qdict_put(root, "supplies", sup);

    /*
     * every channel with a table entry (the used ones) or an active
     * injection, keyed by ch
     */
    for (ch = 0; ch < STIXCTRL_HK_CHANNELS; ch++) {
        g_autofree char *key = NULL;
        QDict *c;

        if (stixctrl_hk_adc_table[ch] == 0 &&
            s->adc_inject[ch] == STIXCTRL_ADC_INJECT_NONE) {
            continue;
        }
        c = qdict_new();
        qdict_put_int(c, "truth", stixctrl_hk_truth(s, ch));
        qdict_put_int(c, "presented", stixctrl_hk_value(s, ch));
        qdict_put_bool(c, "injected",
                       s->adc_inject[ch] != STIXCTRL_ADC_INJECT_NONE);
        if (s->hk_last_ns[ch] >= 0) {
            QDict *last = qdict_new();

            qdict_put_int(last, "value", s->hk_last_val[ch]);
            qdict_put_int(last, "vclock_ns", s->hk_last_ns[ch]);
            qdict_put(c, "last", last);
        } else {
            qdict_put_null(c, "last");
        }
        key = g_strdup_printf("%u", ch);
        qdict_put(adc, key, c);
    }
    qdict_put(root, "hk_adc", adc);

    sta = s->reg.hkIrqSta;
    qdict_put_int(prot, "irqen", (sta >> STIXCTRL_HKIRQSTA_IRQEN) &
                                 STIXCTRL_HKIRQSTA_IRQEN_MASK);
    qdict_put_int(prot, "protstat", (sta >> STIXCTRL_HKIRQSTA_PROTSTAT) &
                                    STIXCTRL_HKIRQSTA_PROTSTAT_MASK);
    qdict_put_int(prot, "hotsample", (sta >> STIXCTRL_HKIRQSTA_HOTSAMPLE) &
                                     STIXCTRL_HKIRQSTA_HOTSAMPLE_MASK);
    for (line = 0; line < STIXCTRL_HK_PROT_LINES; line++) {
        uint32_t cfg = s->reg.hkProtConfig[line];
        QDict *l = qdict_new();

        qdict_put_int(l, "line", line + 1);
        qdict_put_int(l, "ch", stixctrl_hk_prot_ch[line]);
        qdict_put_int(l, "bit", stixctrl_hk_prot_bit(line));
        qdict_put_int(l, "threshold",
                      (cfg >> STIXCTRL_HKPROTCONFIG_UPTHRESHOLD) &
                      STIXCTRL_HKPROTCONFIG_UPTHRESHOLD_MASK);
        qdict_put_bool(l, "prot_en", !!(cfg & STIXCTRL_HK_PROT_EN));
        qlist_append(plines, l);
    }
    qdict_put(prot, "lines", plines);
    qdict_put(root, "hk_prot", prot);

    qdict_put_bool(wdg, "running", s->wdg_running);
    qdict_put_int(wdg, "count", stixctrl_wdg_count(s));
    qdict_put_int(wdg, "reload", s->wdg_reload);
    qdict_put(root, "watchdog", wdg);

    stixctrl_scet_now(s, &coarse, &fine);
    qdict_put_int(scet, "coarse", coarse);
    qdict_put_int(scet, "fine", fine);
    qdict_put_bool(scet, "unsynced", s->unsynced_flag);
    qdict_put(root, "scet", scet);

    /*
     * idefxConfig 3:0 = quarter power. The FSM, QL bank and replay come
     * through the detector branch's accessor (D-PLm req. 8)
     */
    for (q = 0; q < 4; q++) {
        qlist_append_bool(qon, (s->reg.idefxConfig >> q) & 1u);
    }
    qdict_put(det, "quarter_on", qon);
    qdict_put(root, "det", det);

    /* A-3 (D-Ah): the attenuator's truth/presented/faults summary */
    qdict_put(root, "attenuator", stix_att_hw_state(&s->att));

    json = qobject_to_json(QOBJECT(root));
    return g_string_free(json, false);
}

static const Property grlib_stixctrl_properties[] = {
    DEFINE_PROP_UINT32("rotb-blocks", STIXCTRL, rotb_blocks,
                       STIXCTRL_ROTB_MAX_BLOCKS),
    DEFINE_PROP_BOOL("hk-adc", STIXCTRL, hk_adc, true),
    DEFINE_PROP_BOOL("powerstat", STIXCTRL, powerstat, true),
    DEFINE_PROP_BOOL("hk-prot", STIXCTRL, hk_prot, true),
    /* D-1 (D-Db): off keeps 0x00-0x4C plain storage, never reset */
    DEFINE_PROP_BOOL("det-model", STIXCTRL, det.model, true),
    /*
     * D-2 (D-Db): off keeps ideFxCmdSta, idefxModeData, idefxWriteMask
     * and idefxConfig plain storage with CmdReady 0 (the 384 timeouts)
     */
    DEFINE_PROP_BOOL("det-idefx", STIXCTRL, det.idefx, true),
    /* D-4 (D-Db): the synthetic sky's PRNG seed, applied at every reset */
    DEFINE_PROP_UINT32("det-seed", STIXCTRL, det.seed, 1),
    /* D-5 (D-Db): off makes no ROTB chunks; the nominal swap stays */
    DEFINE_PROP_BOOL("det-rotb", STIXCTRL, det.rotb, true),
    /*
     * D-7 (D-Dk): the .dssevs file, pre-scanned at realize into the
     * replay table; a missing, unreadable or malformed file is a
     * realize error naming the path
     */
    DEFINE_PROP_STRING("det-replay-file", STIXCTRL, det.replay_file),
    /*
     * A-1 (D-Ah): the attenuator mechanism. att-model=off keeps 0x5C
     * plain storage, never reset (legacy, A/B comparison)
     */
    DEFINE_PROP_BOOL("att-model", STIXCTRL, att_cfg.model, true),
    DEFINE_PROP_STRING("att-initial", STIXCTRL, att_cfg.initial),
    DEFINE_PROP_UINT32("att-travel-ms", STIXCTRL, att_cfg.travel_ms, 1000),
    DEFINE_PROP_UINT32("att-ramp-ms", STIXCTRL, att_cfg.ramp_ms, 1600),
    DEFINE_PROP_UINT32("att-debounce-ms", STIXCTRL, att_cfg.debounce_ms, 10),
    DEFINE_PROP_BOOL("att-timeout-finishes", STIXCTRL, att_cfg.timeout_finishes,
                     true),
};

/*
 * no injection from the moment the object exists (not only at realize);
 * the detector scene defaults likewise (D-4: a -global or a qom-set at
 * -S before realize must not be overwritten)
 */
static void grlib_stixctrl_instance_init(Object *obj)
{
    /*
     * D-Ua: the EDAC unit, the QOM child at /machine/stixctrl/edac (the
     * attenuator D-Aa pattern). Realized with the parent; its runtime
     * properties (edac-at-boot, test-latch-*) are its own.
     */
    object_initialize_child(obj, "edac", &GRLIB_STIXCTRL(obj)->edac,
                            TYPE_STIX_EDAC);
    stixctrl_adc_inject_clear(GRLIB_STIXCTRL(obj));
    stix_det_instance_init(&GRLIB_STIXCTRL(obj)->det);
    stixctrl_hk_last_clear(GRLIB_STIXCTRL(obj));
    /* A-1 (D-Aa): the QOM face, /machine/stixctrl/attenuator */
    object_initialize_child(obj, "attenuator", &GRLIB_STIXCTRL(obj)->att,
                            TYPE_STIX_ATT);
    /* PL-2 (D-PLo): read-only, no setter */
    object_property_add_str(obj, "hw-state", stixctrl_get_hw_state, NULL);
}

static void grlib_stixctrl_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = grlib_stixctrl_realize;
    dc->unrealize = grlib_stixctrl_unrealize;
    device_class_set_legacy_reset(dc, grlib_stixctrl_reset);
    device_class_set_props(dc, grlib_stixctrl_properties);

    for (uint32_t ch = 0; ch < STIXCTRL_HK_CHANNELS; ch++) {
        g_autofree char *name = g_strdup_printf("adc-inject[%u]", ch);

        object_class_property_add(klass, name, "uint32", &uint32_type_info,
                                  stixctrl_adc_inject_get,
                                  stixctrl_adc_inject_set,
                                  NULL, GUINT_TO_POINTER(ch));
        object_class_property_set_description(klass, name,
            "HK ADC override for this channel: raw 0..4095, "
            "4294967295 = none (runtime, needs hk-adc=on)");
    }
    /* D-4: det-source, det-flare, det-quiet-cps, det-flare-cps */
    stix_det_class_init(klass, offsetof(STIXCTRL, det));
}

static const TypeInfo grlib_stixctrl_info = {
    .name = TYPE_GRLIB_STIXCTRL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STIXCTRL),
    .instance_init = grlib_stixctrl_instance_init,
    .class_init = grlib_stixctrl_class_init,
};

static void grlib_stixctrl_register_types(void)
{
    type_register_static(&grlib_stixctrl_info);
}

type_init(grlib_stixctrl_register_types)