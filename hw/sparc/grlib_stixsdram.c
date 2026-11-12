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
 *
 * M-1 (memory-upset plan D-Ua): the edac register (0x80000C04) composes
 * SECCOUNT 7:0 and DEDETECTED 31 from the EDAC unit (the stixctrl
 * device's child, linked by leon3.c) and takes its ICD reset value at
 * every device reset with the D-Uk edac-at-boot EDACON state.
 */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/sparc/grlib_stix.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "StixIdpu.h"
#include "stix_edac.h"

#define STIXSDRAM_REG_SIZE sizeof(ftSdCtrl16Regs_s)
#define STIXSDRAM_REG_EDAC offsetof(ftSdCtrl16Regs_s, edac)

OBJECT_DECLARE_SIMPLE_TYPE(STIXSDRAM, GRLIB_STIXSDRAM)

struct STIXSDRAM
{
    SysBusDevice parent_obj;

    ftSdCtrl16Regs_s reg;

    /*
     * M-1 (D-Ua): the EDAC unit, linked by leon3.c (NULL keeps the
     * register plain storage)
     */
    StixEdac *edac;

    MemoryRegion iomem;
};

static uint64_t grlib_stixsdram_read(void *opaque, hwaddr addr, unsigned size)
{
    STIXSDRAM *s = opaque;

    if (addr == STIXSDRAM_REG_EDAC) {
        /*
         * SECCOUNT 7:0 and DEDETECTED 31 are composed; EDACON and
         * SCRUBRATE stay stored
         */
        return stix_edac_sdram_read(s->edac, s->reg.edac);
    }
    return ((volatile uint32_t *)&s->reg)[(uint32_t)addr / sizeof(uint32_t)];
}

static void grlib_stixsdram_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    STIXSDRAM *s = opaque;

    if (addr == STIXSDRAM_REG_EDAC) {
        /*
         * the composed fields are dropped; the EDACRST strobe (bit 13)
         * clears the counter and the latch and self-clears
         */
        s->reg.edac = stix_edac_sdram_write(s->edac, s->reg.edac,
                                            (uint32_t)value);
        return;
    }
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

/*
 * M-1: Sdram:Edac takes its ICD reset at every device reset (EdacOn 0,
 * ScrubRate 0, STIX-ICD-0026 txt:3243-3253), with the D-Uk edac-at-boot
 * EDACON state on top. configStatus is not touched: its reset behaviour
 * stays as today (the INIT repair owns it).
 */
static void grlib_stixsdram_reset(DeviceState *d)
{
    STIXSDRAM *s = GRLIB_STIXSDRAM(d);

    s->reg.edac = stix_edac_boot_edacon(s->edac, STIX_EDAC_SDRAM)
                  ? (1u << FTSDCTRL16_EDAC_EDACON) : 0;
}

static void grlib_stixsdram_instance_init(Object *obj)
{
    STIXSDRAM *s = GRLIB_STIXSDRAM(obj);

    /*
     * M-1 (D-Ua): leon3.c links the EDAC unit (the stixctrl device's
     * child), like the SPW's scet-source link
     */
    object_property_add_link(obj, "edac", TYPE_STIX_EDAC,
                             (Object **)&s->edac,
                             object_property_allow_set_link,
                             OBJ_PROP_LINK_STRONG);
}

static void grlib_stixsdram_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = grlib_stixsdram_realize;
    device_class_set_legacy_reset(dc, grlib_stixsdram_reset);
}

static const TypeInfo grlib_stixsdram_info = {
    .name = TYPE_GRLIB_STIXSDRAM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STIXSDRAM),
    .instance_init = grlib_stixsdram_instance_init,
    .class_init = grlib_stixsdram_class_init,
};

static void grlib_stixsdram_register_types(void)
{
    type_register_static(&grlib_stixsdram_info);
}

type_init(grlib_stixsdram_register_types)
