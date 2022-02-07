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

#define STIXWD_REG_SIZE 256 /* Size of memory mapped registers */ // TODO: Set to right size

// #define WDT_REG_ADDR (STIXCTRL_APB_BASE_wdtCtrlSta)
// #define STIX_STATUS_REG (STIXCTRL_APB_BASE_stixStatus)
// #define WDT_QUANTUM 5000000

/* Conversion constant from watchdog units to cycles for watchdog interval */
// #define WDT_CONSTANT 65600
// #define WDT_CONSTANT 656

OBJECT_DECLARE_SIMPLE_TYPE(STIXWD, GRLIB_STIXWD)

struct STIXWD
{
    SysBusDevice parent_obj;

    uint32_t interval;
    uint32_t wd_value;
    uint8_t wd_enable;
    uint32_t wd_register;
    uint32_t wd_stix_status;
    FILE *f;

    MemoryRegion iomem;
    qemu_irq irq;
};

void watchdog_ISR(void *opaque);

void watchdog_ISR(void *opaque)
{
    STIXWD *stixwd = opaque;

    if (stixwd->wd_value == 0x00 && stixwd->wd_enable == 1)
    {
        // perform reset
        qemu_printf("Watchdog reset.\n");
        stixwd->wd_enable = 0;
        assert(0);
    }
    else
    {
        // decrement watchdog
        stixwd->wd_value--;
    }

    // schedule next event
    if (stixwd->wd_enable == 1)
    {
        QEMUTimer *user_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, watchdog_ISR, stixwd);
        int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

        timer_mod(user_timer, now + 0.5 * 1000);
    }
}

static uint64_t grlib_stixwd_read(void *opaque, hwaddr addr, unsigned size)
{
    return (0);
}

static void grlib_stixwd_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
}

static const MemoryRegionOps grlib_stixwd_ops = {
    .write = grlib_stixwd_write,
    .read = grlib_stixwd_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void grlib_stixwd_init_and_load_file(STIXWD *stixwd)
{
    int res;

    qemu_printf("Watchdog_subsystem - INIT \n");

    stixwd->interval = 0xFFFFFFFF;
    stixwd->wd_value = 0xFFFFFFFF;

    // Open or create new watchfog file
    stixwd->f = fopen("watchdog.txt", "r+");
    if (stixwd->f == NULL)
    {
        stixwd->f = fopen("watchdog.txt", "w+");
        if (stixwd->f == NULL)
        {
            qemu_printf("Error: Create watchdog file\n");
            exit(1);
        }
    }
    else
    {
        // Read watchdog register value
        res = fread(&stixwd->wd_register, sizeof(uint32_t), 1, stixwd->f);
        if (res < 0)
        {
            qemu_printf("Error: Read failed from watchdog file\n");
            exit(1);
        }
    }

    watchdog_ISR(stixwd);
}

static void grlib_stixwd_realize(DeviceState *dev, Error **errp)
{
    STIXWD *STIXWD = GRLIB_STIXWD(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    sysbus_init_irq(sbd, &STIXWD->irq);

    memory_region_init_io(&STIXWD->iomem, OBJECT(STIXWD), &grlib_stixwd_ops, STIXWD, "stixwd", STIXWD_REG_SIZE);

    sysbus_init_mmio(sbd, &STIXWD->iomem);

    grlib_stixwd_init_and_load_file(STIXWD);
}

static void grlib_stixwd_reset(DeviceState *d)
{
    // stixwd *stixwd = grlib_stixwd(d);
}

static void grlib_stixwd_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = grlib_stixwd_realize;
    dc->reset = grlib_stixwd_reset;
}

static const TypeInfo grlib_stixwd_info = {
    .name = TYPE_GRLIB_STIXWD,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STIXWD),
    .class_init = grlib_stixwd_class_init,
};

static void grlib_stixwd_register_types(void)
{
    type_register_static(&grlib_stixwd_info);
}

type_init(grlib_stixwd_register_types)