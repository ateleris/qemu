#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/sparc/grlib_stix.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "chardev/char-fe.h"
#include "qemu/qemu-print.h"
#include "qemu/timer.h"
#include "system/runstate.h"
#include "StixIdpu.h"

#include "trace.h"
#include "qom/object.h"

#define STIXCTRL_REG_SIZE sizeof(stixCtrlRegs_s) /* Size of memory mapped registers */ // TODO: Set to right size

OBJECT_DECLARE_SIMPLE_TYPE(STIXCTRL, GRLIB_STIXCTRL)

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
    /* Watchdog (0xD4), counted lazily from QEMU_CLOCK_VIRTUAL: while
     * running, COUNT = wdg_count - cycles elapsed since wdg_base_ns. */
    uint32_t wdg_reload;
    uint32_t wdg_count;
    bool wdg_running;
    int64_t wdg_base_ns;
    QEMUTimer *wdg_timer;
};

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
    
    uint32_t regval = ((volatile uint32_t *)&stixctrl->reg)[(uint32_t)addr / sizeof(uint32_t)];

    qemu_printf("CTRL | Read requested: addr = 0x%x, regval = 0x%x\n", (uint32_t)addr, regval);

    return regval;
}

static void grlib_stixctrl_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    STIXCTRL *stixctrl = opaque;

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
        }
        trace_stixctrl_scet_write((uint32_t)addr, (uint32_t)value);
    }

    /* model repair: HK ADC conversion completes immediately. The FSW
     * (HK.c) writes a conversion command to hkAdc (0x8c) and polls the
     * READY bit (bit 7) until the conversion is done; the real FPGA
     * asserts it per conversion. Without this the poll never ends. */
    if (addr == 0x8c /* hkAdc */)
    {
        value |= (1u << STIXCTRL_HKADC_READY);
    }

    /* model repair: rotbCmdSta (0x54) writable fields are the COMMAND
     * bits (1:0) and the EDACRESET strobe (bit 2) only. BLOCKCOUNT
     * (23:7), SECCOUNT (31:24), READY, FULL and DEDETECTED are read-only
     * status. The FSW's EDAC counter reset clears its strobe with a
     * full-register write of ~(1u << 2) = 0xFFFFFFFB (EDAC.c); storing
     * that raw set BLOCKCOUNT to 131071 and the FSW's HK level check
     * asserted (HKAcquire.c:325) — a baseline-inherited defect masked
     * on 5.2 by the SCET abort killing runs ~4 FSW-seconds earlier. */
    if (addr == 0x54 /* rotbCmdSta */)
    {
        value &= 0x7u;
    }

    /* model repair: rotbConfig (0x58) ENABLE gates the rotating buffer;
     * the FPGA asserts rotbCmdSta.READY (bit 3) once enabled, and the
     * FSW's RotBuf_init waits for it (timeout is fatal: "Fatal error:
     * 1" at init). The pre-port model passed this by accident — the
     * EDAC strobe-clear write 0xFFFFFFFB set READY through the raw
     * register store, which the 0x54 masking above removed. */
    if (addr == 0x58 /* rotbConfig */)
    {
        volatile uint32_t *rotbCmdSta = &((volatile uint32_t *)&stixctrl->reg)[0x54 / 4];
        if (value & (1u << STIXCTRL_ROTBCONFIG_ENABLE))
        {
            *rotbCmdSta |= 1u << STIXCTRL_ROTBCMDSTA_READY;
        }
        else
        {
            *rotbCmdSta &= ~(1u << STIXCTRL_ROTBCMDSTA_READY);
        }
    }

    ((volatile uint32_t *)&stixctrl->reg)[(uint32_t)addr / sizeof(uint32_t)] = (uint32_t)value;
    
    qemu_printf("CTRL | Write completed addr = 0x%x, new regval = 0x%x\n", (uint32_t)addr, ((volatile uint32_t *)&stixctrl->reg)[(uint32_t)addr / sizeof(uint32_t)]);
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

    sysbus_init_irq(sbd, &STIXCTRL->irq_detectors);
    sysbus_init_irq(sbd, &STIXCTRL->irq_hk);
    sysbus_init_irq(sbd, &STIXCTRL->irq_other);
    sysbus_init_irq(sbd, &STIXCTRL->irq_edac);
    sysbus_init_irq(sbd, &STIXCTRL->irq_rotb);

    memory_region_init_io(&STIXCTRL->iomem, OBJECT(STIXCTRL), &grlib_stixctrl_ops, STIXCTRL, "stixctrl", STIXCTRL_REG_SIZE);

    sysbus_init_mmio(sbd, &STIXCTRL->iomem);

    STIXCTRL->wdg_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, stixctrl_wdg_expired, STIXCTRL);

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
}

static void grlib_stixctrl_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = grlib_stixctrl_realize;
    device_class_set_legacy_reset(dc, grlib_stixctrl_reset);
}

static const TypeInfo grlib_stixctrl_info = {
    .name = TYPE_GRLIB_STIXCTRL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STIXCTRL),
    .class_init = grlib_stixctrl_class_init,
};

static void grlib_stixctrl_register_types(void)
{
    type_register_static(&grlib_stixctrl_info);
}

type_init(grlib_stixctrl_register_types)