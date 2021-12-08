#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sparc/grlib.h"
#include "hw/sysbus.h"
#include "qemu/module.h"
#include "chardev/char-fe.h"

#include "trace.h"
#include "qom/object.h"

#define STIXFLASH_REG_SIZE 256     /* Size of memory mapped registers */

OBJECT_DECLARE_SIMPLE_TYPE(STIXFLASH, GRLIB_STIXFLASH)

struct STIXFLASH {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
};

static uint64_t grlib_stixflash_read(void *opaque, hwaddr addr,
                                   unsigned size)
{
    //trace_grlib_apbuart_readl_unknown(addr);
    return 0;
}

static void grlib_stixflash_write(void *opaque, hwaddr addr,
                                uint64_t value, unsigned size)
{
    //trace_grlib_apbuart_readl_unknown(addr);
}

static const MemoryRegionOps grlib_stixflash_ops = {
    .write      = grlib_stixflash_write,
    .read       = grlib_stixflash_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void grlib_stixflash_realize(DeviceState *dev, Error **errp)
{
    STIXFLASH *stixflash = GRLIB_STIXFLASH(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    /*qemu_chr_fe_set_handlers(&uart->chr,
                             grlib_apbuart_can_receive,
                             grlib_apbuart_receive,
                             grlib_apbuart_event,
                             NULL, uart, NULL, true);*/

    sysbus_init_irq(sbd, &stixflash->irq);

    memory_region_init_io(&stixflash->iomem, OBJECT(stixflash), &grlib_stixflash_ops, stixflash,
                          "stixflash", STIXFLASH_REG_SIZE);

    sysbus_init_mmio(sbd, &stixflash->iomem);
}

static void grlib_stixflash_reset(DeviceState *d)
{
    //STIXFLASH *stixflash = GRLIB_STIXFLASH(d);
}

static void grlib_stixflash_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = grlib_stixflash_realize;
    dc->reset = grlib_stixflash_reset;
    //device_class_set_props(dc, grlib_apbuart_properties);
}

static const TypeInfo grlib_stixflash_info = {
    .name          = TYPE_GRLIB_STIXFLASH,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STIXFLASH),
    .class_init    = grlib_stixflash_class_init,
};

static void grlib_stixflash_register_types(void)
{
    type_register_static(&grlib_stixflash_info);
}

type_init(grlib_stixflash_register_types)