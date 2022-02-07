#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sparc/grlib.h"
#include "hw/sysbus.h"
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

static uint64_t toMs(uint32_t _coarse, uint16_t _fine)
{
	uint64_t fine_millisec = ((uint32_t)_fine * 1000) / 0x10000;
	return (uint64_t)_coarse * 1000LU + fine_millisec;
}

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
    int64_t time_correction;
};

/*static void grlib_stixctrl_timer_tick(void *opaque)
{
    STIXCTRL *stixctrl = opaque;

    stixctrl->time_ticks++;
    stixctrl->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, grlib_stixctrl_timer_tick, stixctrl);
    timer_mod_ms(stixctrl->timer, 1);
}*/

static stixCtrl_scet_s msToSCET(uint64_t ms)
{
	assert(ms / 1000LU <= 0xFFFFFFFF);
	stixCtrl_scet_s scet;
	
	scet.coarse = (uint32_t)(ms / 1000LU);
	scet.fine = (uint16_t)((ms - (uint64_t)scet.coarse * 1000LU) * (uint64_t)0x10000 / 1000LU);

	return scet;
}

static uint64_t grlib_stixctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    STIXCTRL *stixctrl = opaque;

    // update timer constantly // TODO: check if this should only be done on a read of 0xC8 or 0xCC
    stixCtrl_scet_s scet = msToSCET(qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) - stixctrl->time_correction);
    stixctrl->reg.longTimerSec = scet.coarse;
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
    if(addr == 0xC8 || addr == 0xCC)
    {
        // have not been testing this yet. Stop when it happens the first time -> timecode and TC9 needed
        assert(0);
        stixCtrl_scet_s scet_virtual = msToSCET(qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        stixCtrl_scet_s scet_corrected = msToSCET(qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) - stixctrl->time_correction);
        qemu_printf("Current virtual time: s = %u, ss = %u", scet_virtual.coarse, scet_virtual.fine);
        qemu_printf("Current corrected time: s = %u, ss = %u", scet_corrected.coarse, scet_corrected.fine);

        uint64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
        uint64_t time_correction_s = (stixctrl->time_correction / 1000LU) * 1000LU;

        if(addr == 0xC8)
        {
            uint64_t time_correction_ms = time_correction_s - (stixctrl->time_correction / 1000LU) * 1000LU;
            stixctrl->time_correction = time_correction_ms + (now - toMs(value, 0));
        }
        else
        {
            stixctrl->time_correction = time_correction_s + (now - toMs(0, value));
        }

        scet_corrected = msToSCET(qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) - stixctrl->time_correction);
        qemu_printf("New corrected time: s = %u, ss = %u", scet_corrected.coarse, scet_corrected.fine);
    }
    
    ((volatile uint32_t *)&stixctrl->reg)[(uint32_t)addr / sizeof(uint32_t)] = (uint32_t)value;
    
    qemu_printf("CTRL | Write completed addr = 0x%x, new regval = 0x%x\n", (uint32_t)addr, ((volatile uint32_t *)&stixctrl->reg)[(uint32_t)addr / sizeof(uint32_t)]);
}

static const MemoryRegionOps grlib_stixctrl_ops = {
    .write = grlib_stixctrl_write,
    .read = grlib_stixctrl_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
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
    // stixctrl *stixctrl = grlib_stixctrl(d);
}

static void grlib_stixctrl_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = grlib_stixctrl_realize;
    dc->reset = grlib_stixctrl_reset;
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