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

#define STIXSPW_REG_SIZE 256 /* Size of memory mapped registers */ // TODO: Set to right size

OBJECT_DECLARE_SIMPLE_TYPE(STIXSPW, GRLIB_STIXSPW)

struct STIXSPW
{
    SysBusDevice parent_obj;

    stixSpwRegs_s reg;

    MemoryRegion iomem;
    qemu_irq irq;
};

static uint64_t grlib_stixspw_read(void *opaque, hwaddr addr, unsigned size)
{
    STIXSPW *stixspw = opaque;

    uint32_t regval = *((uint32_t *)(&stixspw->reg + (uint32_t)addr));

    qemu_printf("SPW | Read requested: addr = %lu, regval = 0x%x\n", //, chip = %u, page = %u, mcm = %u, page_address = %u, memory_bit_type = %u, ram_address = 0x%x, command = %u, status = 0x%x\n",
                addr, regval); //, stixflash->chip, stixflash->page, stixflash->mcm, stixflash->page_address, stixflash->memory_bit_type, (uint32_t)stixflash->ram_address, stixflash->command, stixflash->cmd_sta);

    return regval;
}

static void grlib_stixspw_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    STIXSPW *stixspw = opaque;

    *((uint32_t *)(&stixspw->reg + (uint32_t)addr)) = (uint32_t)value;
    
    qemu_printf("SPW | Write completed addr = %lu, new regval = 0x%x\n", addr, (uint32_t)value);//: chip = %u, page = %u, mcm = %u, page_address = %u, memory_bit_type = %u, ram_address = 0x%x command = %u, status = 0x%x\n",
        //stixflash->chip, stixflash->page, stixflash->mcm, stixflash->page_address, stixflash->memory_bit_type, (uint32_t)stixflash->ram_address, stixflash->command, stixflash->cmd_sta);
}

static const MemoryRegionOps grlib_stixspw_ops = {
    .write = grlib_stixspw_write,
    .read = grlib_stixspw_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void grlib_stixspw_realize(DeviceState *dev, Error **errp)
{
    STIXSPW *STIXSPW = GRLIB_STIXSPW(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    sysbus_init_irq(sbd, &STIXSPW->irq);

    memory_region_init_io(&STIXSPW->iomem, OBJECT(STIXSPW), &grlib_stixspw_ops, STIXSPW, "STIXSPW", STIXSPW_REG_SIZE);

    sysbus_init_mmio(sbd, &STIXSPW->iomem);
}

static void grlib_stixspw_reset(DeviceState *d)
{
    // STIXSPW *STIXSPW = grlib_stixspw(d);
}


static void grlib_stixspw_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = grlib_stixspw_realize;
    dc->reset = grlib_stixspw_reset;
}

static const TypeInfo grlib_stixspw_info = {
    .name = TYPE_GRLIB_STIXSPW,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STIXSPW),
    .class_init = grlib_stixspw_class_init,
};

static void grlib_stixspw_register_types(void)
{
    type_register_static(&grlib_stixspw_info);
}

type_init(grlib_stixspw_register_types)