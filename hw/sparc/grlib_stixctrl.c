#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/sparc/grlib_stix.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "chardev/char-fe.h"
#include "qemu/qemu-print.h"
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

static uint64_t grlib_stixctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    STIXCTRL *stixctrl = opaque;

    // update timer constantly // TODO: check if this should only be done on a read of 0xC8 or 0xCC
    stixCtrl_scet_s scet = nsToSCET(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - stixctrl->time_correction_ns);
    /* bit 31 = UNSYNCED status flag (Timing.c TIMING_COARSE_UNSYNCED);
     * the FSW reads and checks it (Timing.c:136). */
    stixctrl->reg.longTimerSec = scet.coarse | (stixctrl->unsynced_flag << 31);
    stixctrl->reg.longTimerSubSec = scet.fine;
    
    uint32_t regval = ((volatile uint32_t *)&stixctrl->reg)[(uint32_t)addr / sizeof(uint32_t)];

    qemu_printf("CTRL | Read requested: addr = 0x%x, regval = 0x%x\n", (uint32_t)addr, regval);

    return regval;
}

static void grlib_stixctrl_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    STIXCTRL *stixctrl = opaque;

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