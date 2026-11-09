/*
 * STIX IDPU SDRAM controller register stub (model repair).
 *
 * The real IDPU has an FTSDCTRL16 SDRAM controller at 0x80000C00. The FSW's
 * RamUtils_initSDRAM() writes STANDARD_SDRAM_CONFIG (0x900001, INIT bit set)
 * to configStatus and polls the INIT bit (bit 0) for up to 360000 iterations;
 * without a device here every read returns 0, the poll times out, and
 * MemCommon_init() fails into the TMBuffer assert (TMBuffer.c:276).
 *
 * Minimal store-and-return model: the FSW's written value already carries
 * INIT=1, so the first poll read succeeds. No PnP entry is added: the FSW
 * addresses the controller directly via SDRAM_REGISTERS (no drvmgr lookup).
 */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/sparc/grlib_stix.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "StixIdpu.h"

#define STIXSDRAM_REG_SIZE sizeof(ftSdCtrl16Regs_s)

OBJECT_DECLARE_SIMPLE_TYPE(STIXSDRAM, GRLIB_STIXSDRAM)

struct STIXSDRAM
{
    SysBusDevice parent_obj;

    ftSdCtrl16Regs_s reg;

    MemoryRegion iomem;
};

static uint64_t grlib_stixsdram_read(void *opaque, hwaddr addr, unsigned size)
{
    STIXSDRAM *s = opaque;

    return ((volatile uint32_t *)&s->reg)[(uint32_t)addr / sizeof(uint32_t)];
}

static void grlib_stixsdram_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    STIXSDRAM *s = opaque;

    ((volatile uint32_t *)&s->reg)[(uint32_t)addr / sizeof(uint32_t)] =
        (uint32_t)value;
}

static const MemoryRegionOps grlib_stixsdram_ops = {
    .read = grlib_stixsdram_read,
    .write = grlib_stixsdram_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void grlib_stixsdram_realize(DeviceState *dev, Error **errp)
{
    STIXSDRAM *s = GRLIB_STIXSDRAM(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &grlib_stixsdram_ops, s,
                          "stixsdram", STIXSDRAM_REG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void grlib_stixsdram_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = grlib_stixsdram_realize;
}

static const TypeInfo grlib_stixsdram_info = {
    .name = TYPE_GRLIB_STIXSDRAM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STIXSDRAM),
    .class_init = grlib_stixsdram_class_init,
};

static void grlib_stixsdram_register_types(void)
{
    type_register_static(&grlib_stixsdram_info);
}

type_init(grlib_stixsdram_register_types)
