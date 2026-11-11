/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * STIX IDPU detector controller model, tier 0 (detector plan D-1):
 * fsmCmd strobes, live fsmSta/fsmEvents, testPulseCmdSta, the 100 ms
 * tick, the QL bank switch with IRQ 3, and the register reset values.
 *
 * FSW references are to the ASW tree (BSW = BSW_ASW/src/BSW/...).
 *
 * Re-entrancy (D-Dj): the tick writes the accumulator banks through
 * memory_region_get_ram_ptr (each write followed by
 * memory_region_set_dirty), never with address_space_*, and its only
 * other output is qemu_irq_pulse. No MMIO dispatch happens from timer
 * context.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "stix_det.h"
#include "trace.h"

#define STIX_DET_TICK_NS (100 * 1000 * 1000LL)
#define STIX_DET_QL_BANKS 4u

/* LVAR has 40 u32 slots per bank (Detectors.h:42 Detectors_MAXINTTIME) */
#define STIX_DET_LVAR_SLOTS (STIXCTRL_LVAR0_SIZE / sizeof(uint32_t))
#define STIX_DET_LVAR_MAX 0xFFFFu   /* LVAR clamps at 16 bits [D] */

/* fsmCmd: strobes 8:0 (reserved 4:3 excluded), config 12:9 and 31:16 */
#define FSMCMD_BIT(f) (STIXCTRL_FSMCMD_##f##_MASK << STIXCTRL_FSMCMD_##f)
#define STIX_DET_FSMCMD_STROBES \
    (FSMCMD_BIT(SETENABLE) | FSMCMD_BIT(CLEARENABLE) | FSMCMD_BIT(RESET) | \
     FSMCMD_BIT(SETCALENABLE) | FSMCMD_BIT(CALCLEAR) | \
     FSMCMD_BIT(QLCLEAR) | FSMCMD_BIT(OVERFLOWCLEAR))
#define STIX_DET_FSMCMD_CONFIG \
    (FSMCMD_BIT(CALTQ) | FSMCMD_BIT(FIFOIRQEN) | FSMCMD_BIT(QLIRQEN) | \
     FSMCMD_BIT(CALIRQEN) | FSMCMD_BIT(ACTIVETLUT))

/* testPulseCmdSta: Busy reads 1 (idle, active low), QuarterSeu 0 */
#define STIX_DET_TP_BUSY \
    (STIXCTRL_TESTPULSECMDSTA_BUSY_MASK << STIXCTRL_TESTPULSECMDSTA_BUSY)
#define STIX_DET_TP_SEU \
    (STIXCTRL_TESTPULSECMDSTA_QUARTERSEU_MASK << \
     STIXCTRL_TESTPULSECMDSTA_QUARTERSEU)

/* bank offsets inside the controller's RAM regions (StixIdpu.h:262-340) */
static const hwaddr stix_det_lacc_off[STIX_DET_QL_BANKS] = {
    STIXCTRL_LACC0_START - STIXCTRL_SRAM_START,
    STIXCTRL_LACC1_START - STIXCTRL_SRAM_START,
    STIXCTRL_LACC2_START - STIXCTRL_SRAM_START,
    STIXCTRL_LACC3_START - STIXCTRL_SRAM_START,
};
static const hwaddr stix_det_ltrg_off[STIX_DET_QL_BANKS] = {
    0,
    STIXCTRL_LTRG1_START - STIXCTRL_LTRG0_START,
    STIXCTRL_LTRG2_START - STIXCTRL_LTRG0_START,
    STIXCTRL_LTRG3_START - STIXCTRL_LTRG0_START,
};
static const hwaddr stix_det_lvar_off[STIX_DET_QL_BANKS] = {
    0,
    STIXCTRL_LVAR1_START - STIXCTRL_LVAR0_START,
    STIXCTRL_LVAR2_START - STIXCTRL_LVAR0_START,
    STIXCTRL_LVAR3_START - STIXCTRL_LVAR0_START,
};

static uint32_t *stix_det_regp(StixDetState *d, hwaddr addr)
{
    return &((uint32_t *)d->reg)[addr / sizeof(uint32_t)];
}

static void stix_det_ram_zero(MemoryRegion *mr, hwaddr off, hwaddr len)
{
    memset((uint8_t *)memory_region_get_ram_ptr(mr) + off, 0, len);
    memory_region_set_dirty(mr, off, len);
}

static void stix_det_ram_stl(MemoryRegion *mr, hwaddr off, uint32_t val)
{
    stl_be_p((uint8_t *)memory_region_get_ram_ptr(mr) + off, val);
    memory_region_set_dirty(mr, off, sizeof(uint32_t));
}

/* QlIntTime is latched at the start of each QL period [D, RP-0067] */
static void stix_det_latch_ql_int_time(StixDetState *d)
{
    d->ql_int_time = (d->reg->fsmQlConfig >> STIXCTRL_FSMQLCONFIG_QLINTTIME) &
                     STIXCTRL_FSMQLCONFIG_QLINTTIME_MASK;
}

/* (re)start the tick one period from now (SetEnable, Reset) */
static void stix_det_tick_start(StixDetState *d)
{
    d->tick_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          STIX_DET_TICK_NS;
    timer_mod_ns(d->tick, d->tick_deadline_ns);
    trace_stixdet_tick_arm(1, d->tick_deadline_ns);
}

static void stix_det_tick_stop(StixDetState *d)
{
    timer_del(d->tick);
    trace_stixdet_tick_arm(0, 0);
}

/*
 * Zero one QL bank on activation: LACC 0x6000, LTRG 0x40 and LVAR 0xA0
 * only. The rest of each 0x100 LVAR stride is the WDT
 * UNCHANGED_STORAGE (+0xA8, WDT.c:74) and MemBram (0x3C0).
 */
static void stix_det_bank_zero(StixDetState *d, uint32_t ql_bank)
{
    stix_det_ram_zero(d->sram, stix_det_lacc_off[ql_bank],
                      STIXCTRL_LACC0_SIZE);
    stix_det_ram_zero(d->ltrg, stix_det_ltrg_off[ql_bank],
                      STIXCTRL_LTRG0_SIZE);
    stix_det_ram_zero(d->lvar, stix_det_lvar_off[ql_bank],
                      STIXCTRL_LVAR0_SIZE);
}

/*
 * D-Dd bank lifecycle:
 * 1. while a bank is active, its LVAR slots are written per tick
 *    (stix_det_tick); nothing else writes it;
 * 2. at the swap the finished bank's LACC and LTRG are written in full
 *    from the shadow (tier 0 has no count source: zeros); its LVAR is
 *    not rewritten (it holds the per-tick slots);
 * 3. ActiveQl advances and the newly active bank is zeroed
 *    (stix_det_bank_zero); the active bank is also zeroed on a fresh
 *    enable (SetEnable 0 -> 1 at tick 0, stix_det_fsm_cmd), so no RAM
 *    from before a reset leaks into a QL period; QlReady sets and IRQ 3
 *    pulses if QlIrqEn;
 * 4. the finished bank is not touched again until it is active again,
 *    three swaps later. The FSW reads it after the IRQ, as bank
 *    (ActiveQl - 1) mod 4 (Detectors.c:144-166).
 */
static void stix_det_ql_swap(StixDetState *d)
{
    uint32_t done = d->active_ql_bank;
    uint32_t next = (done + 1) % STIX_DET_QL_BANKS;
    bool irq = (d->reg->fsmCmd >> STIXCTRL_FSMCMD_QLIRQEN) &
               STIXCTRL_FSMCMD_QLIRQEN_MASK;

    stix_det_ram_zero(d->sram, stix_det_lacc_off[done], STIXCTRL_LACC0_SIZE);
    stix_det_ram_zero(d->ltrg, stix_det_ltrg_off[done], STIXCTRL_LTRG0_SIZE);

    d->active_ql_bank = next;
    stix_det_bank_zero(d, next);

    d->ql_tick = 0;
    stix_det_latch_ql_int_time(d);
    d->ql_ready = true;
    trace_stixdet_ql_swap(done, next, d->ql_int_time, irq);
    if (irq) {
        qemu_irq_pulse(*d->irq);
    }
}

/*
 * One 100 ms tick of the active QL period. Tick k (0-based) writes its
 * LVAR count to slot k of the active bank while k < 40; later ticks of
 * a longer period (QlIntTime is 16 bits) are dropped, so no write ever
 * leaves the bank's 0xA0 bytes (D-Dd, a model choice: the hardware
 * behaviour is undocumented). The period is QlIntTime + 1 ticks.
 */
static void stix_det_tick(void *opaque)
{
    StixDetState *d = opaque;
    uint32_t slot = d->ql_tick;

    if (slot < STIX_DET_LVAR_SLOTS) {
        stix_det_ram_stl(d->lvar,
                         stix_det_lvar_off[d->active_ql_bank] +
                         slot * sizeof(uint32_t),
                         MIN(d->lvar_tick_count, STIX_DET_LVAR_MAX));
    }
    d->lvar_tick_count = 0;

    d->ql_tick++;
    if (d->ql_tick > d->ql_int_time) {
        stix_det_ql_swap(d);
    }

    d->tick_deadline_ns += STIX_DET_TICK_NS;
    timer_mod_ns(d->tick, d->tick_deadline_ns);
}

/*
 * fsmCmd write (D-Dc). The FSW issues every strobe as fsmCmd |= bit
 * (Detectors.c:136, 186, 1093-1124, 1198-1201), so the strobes read 0
 * or a stored ClearEnable would re-fire on the next RMW. The config bits
 * (CalTq, FifoIrqEn, QlIrqEn, CalIrqEn, ActiveTlut) are stored.
 * - ClearEnable clears Enable and stops the tick; it wins over
 *   SetEnable and freezes everything else [I].
 * - SetEnable (0 -> 1) starts the tick; at a period start (tick 0,
 *   e.g. after a device reset) it latches QlIntTime and zeroes the
 *   active bank (a fresh enable).
 * - Reset clears QlReady and SUMCNT, restarts the QL period and
 *   re-latches QlIntTime; ActiveQl and the banks stay [I]. It re-phases
 *   the tick only while enabled.
 * - QlClear clears QlReady; SetCalEnable sets CalEnable. CalClear and
 *   OverflowClear clear status that never sets (no calibration and no
 *   FIFO model: CalReady and QueueOverflow read 0).
 */
static void stix_det_fsm_cmd(StixDetState *d, uint32_t value)
{
    uint32_t strobes = value & STIX_DET_FSMCMD_STROBES;

    d->reg->fsmCmd = value & STIX_DET_FSMCMD_CONFIG;

    if (strobes & FSMCMD_BIT(CLEARENABLE)) {
        if (d->enable) {
            d->enable = false;
            stix_det_tick_stop(d);
        }
    } else if ((strobes & FSMCMD_BIT(SETENABLE)) && !d->enable) {
        d->enable = true;
        if (d->ql_tick == 0) {
            /* a fresh period: no stale RAM in the active bank */
            stix_det_latch_ql_int_time(d);
            stix_det_bank_zero(d, d->active_ql_bank);
        }
        stix_det_tick_start(d);
    }
    if (strobes & FSMCMD_BIT(RESET)) {
        d->ql_ready = false;
        d->sumcnt = 0;
        d->ql_tick = 0;
        d->lvar_tick_count = 0;
        stix_det_latch_ql_int_time(d);
        if (d->enable) {
            stix_det_tick_start(d);
        }
    }
    if (strobes & FSMCMD_BIT(QLCLEAR)) {
        d->ql_ready = false;
    }
    if (strobes & FSMCMD_BIT(SETCALENABLE)) {
        d->cal_enable = true;
    }
    if (strobes) {
        trace_stixdet_fsm_strobe(strobes, d->enable, d->active_ql_bank,
                                 d->ql_ready);
    }
}

/* fsmSta (0x18), composed live (StixIdpu.h:890-912) */
static uint32_t stix_det_fsm_sta(StixDetState *d)
{
    return ((uint32_t)d->enable << STIXCTRL_FSMSTA_ENABLE) |
           (d->active_ql_bank << STIXCTRL_FSMSTA_ACTIVEQL) |
           ((uint32_t)d->ql_ready << STIXCTRL_FSMSTA_QLREADY) |
           ((uint32_t)d->cal_enable << STIXCTRL_FSMSTA_CALENABLE);
    /* QueueOverflow, ActiveNominal (D-5) and CalReady read 0 */
}

bool stix_det_owns(StixDetState *d, hwaddr addr)
{
    return d->model && addr <= STIX_DET_REG_LAST;
}

uint32_t stix_det_read(StixDetState *d, hwaddr addr)
{
    switch (addr) {
    case 0x18: /* fsmSta */
        return stix_det_fsm_sta(d);
    case 0x30: /* fsmEvents: SUMCNT so far */
        return d->sumcnt;
    case 0x3C: /* fsmResult1, never read by the FSW */
    case 0x40: /* fsmResult2 */
    case 0x48: /* fsmCalCount: calibration never completes */
    case 0x4C: /* fsmCalRes */
        return 0;
    default:
        return *stix_det_regp(d, addr);
    }
}

/* false when the write was dropped (a read-only register) */
bool stix_det_write(StixDetState *d, hwaddr addr, uint32_t value)
{
    switch (addr) {
    case 0x10: /* testPulseCmdSta */
        d->reg->testPulseCmdSta = (value & ~(STIX_DET_TP_BUSY |
                                             STIX_DET_TP_SEU)) |
                                  STIX_DET_TP_BUSY;
        break;
    case 0x14: /* fsmCmd */
        stix_det_fsm_cmd(d, value);
        break;
    case 0x18: /* read-only: fsmSta, fsmEvents, results */
    case 0x30:
    case 0x3C:
    case 0x40:
    case 0x48:
    case 0x4C:
        trace_stixdet_ro_write_dropped((uint32_t)addr, value);
        return false;
    default:
        /*
         * ideFxCmdSta stays plain storage until D-2 (CmdReady and the
         * IDeF-X store land together, accepted-diffs §24)
         */
        *stix_det_regp(d, addr) = value;
        break;
    }
    return true;
}

/*
 * D-Dh device reset (det-model on): explicit reset values for
 * 0x00-0x4C, since the controller reset does not clear its register
 * file otherwise: testPulseCmdSta Busy = 1, all others 0. ideFxCmdSta
 * is 0 here, not D-Dh's 0x80000000: CmdReady without the IDeF-X store
 * is the §24 trap, so CmdReady arrives with the store in D-2. The tick
 * stops, the FSM state clears; the RAM banks are kept (they survive a
 * reset, accepted-diffs §13).
 */
void stix_det_reset(StixDetState *d)
{
    hwaddr addr;

    if (!d->model) {
        return;
    }
    for (addr = 0; addr <= STIX_DET_REG_LAST; addr += sizeof(uint32_t)) {
        *stix_det_regp(d, addr) = 0;
    }
    d->reg->testPulseCmdSta = STIX_DET_TP_BUSY;

    timer_del(d->tick);
    d->enable = false;
    d->ql_ready = false;
    d->cal_enable = false;
    d->active_ql_bank = 0;
    d->sumcnt = 0;
    d->ql_tick = 0;
    d->ql_int_time = 0;
    d->lvar_tick_count = 0;
}

void stix_det_realize(StixDetState *d, stixCtrlRegs_s *reg,
                      MemoryRegion *sram, MemoryRegion *lvar,
                      MemoryRegion *ltrg, qemu_irq *irq)
{
    d->reg = reg;
    d->sram = sram;
    d->lvar = lvar;
    d->ltrg = ltrg;
    d->irq = irq;
    d->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, stix_det_tick, d);
}

void stix_det_unrealize(StixDetState *d)
{
    timer_free(d->tick);
    d->tick = NULL;
}
