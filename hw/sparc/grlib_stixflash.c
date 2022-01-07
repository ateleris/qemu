#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sparc/grlib.h"
#include "hw/sysbus.h"
#include "qemu/module.h"
#include "chardev/char-fe.h"
#include "qemu/qemu-print.h"

#include "trace.h"
#include "qom/object.h"

#define STIXFLASH_REG_SIZE 256 /* Size of memory mapped registers */
#define FLASH_FILE "flash.bin"

/* DEFINES TAKEN FROM FilesystemDefinition.h */
#define		FLASH_PAGE_DATA_SIZE		4096uLL // bytes
#define		FLASH_PAGE_HEADER_SIZE		128uLL // bytes
#define		FLASH_PAGE_SIZE				(FLASH_PAGE_DATA_SIZE + FLASH_PAGE_HEADER_SIZE)	// 4'224 bytes

/* DEFINES TAKEN FROM stixidpu.h */
#define FTNANDCTRL_PAGEADDRESS_CHIPADDRESS 20
#define FTNANDCTRL_PAGEADDRESS_PAGEADDRESS 0
#define FTNANDCTRL_CMDSTA_COMMAND 0
#define FTNANDCTRL_CMDSTA_COMMAND_MASK 0x7
#define FTNANDCTRL_CMDSTA_MCMADDRESS 3
#define FTNANDCTRL_CMDSTA_MCMADDRESS_MASK 0x1
#define FTNANDCTRL_CMDSTA_FLASHTYPE 7
#define FTNANDCTRL_CMDSTA_BUSY 4
#define FTNANDCTRL_CMDSTA_STATUS 24

/* DEFINES TAKEN FROM flash.c */
#define COMMAND_RESET 0
#define COMMAND_BLOCK_ERASE 1
#define COMMAND_PAGE_READ 2
#define COMMAND_PAGE_WRITE 3
#define COMMAND_FSM_ACCESS 4
#define MEMORY_TYPE_BIT (1 << FTNANDCTRL_CMDSTA_FLASHTYPE)

#define ERROR_CODE_FAILED (FTNANDCTRL_CMDSTA_STATUS + 0)
#define ERROR_CODE_READY (FTNANDCTRL_CMDSTA_STATUS + 6)
#define ERROR_CODE_WRPROT (FTNANDCTRL_CMDSTA_STATUS + 7)

#define FLASH_CMD_STA_ADDRESS 0
#define FLASH_PAGE_ADDRESS 4
#define FLASH_RAM_ADDRESS 8

OBJECT_DECLARE_SIMPLE_TYPE(STIXFLASH, GRLIB_STIXFLASH)

struct STIXFLASH
{
    SysBusDevice parent_obj;

    FILE *f;
    uint8_t pageData[FLASH_PAGE_SIZE];

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t ram_address;
    uint32_t page_address;
    uint32_t cmd_sta;

    uint32_t chip;
    uint32_t page;
    uint32_t command;
    uint32_t mcm;
    uint32_t memory_bit_type;
};

static uint64_t grlib_stixflash_read(void *opaque, hwaddr addr,
                                     unsigned size)
{
    STIXFLASH *stixflash = opaque;

    qemu_printf("Read requested: addr = %lu, chip = %u, page = %u, mcm = %u, page_address = %u, memory_bit_type = %u, ram_address = 0x%x command = %u, status = %u\n",
                addr, stixflash->chip, stixflash->page, stixflash->mcm, stixflash->page_address, stixflash->memory_bit_type, stixflash->ram_address, stixflash->command, stixflash->cmd_sta);

    if (addr == FLASH_CMD_STA_ADDRESS)
    {
        return stixflash->cmd_sta & ~(1u << FTNANDCTRL_CMDSTA_BUSY); //| (1u << FTNANDCTRL_CMDSTA_BUSY); // add this to fake a busy flash status
    }
    else if (addr == FLASH_PAGE_ADDRESS)
    {
        assert(false);
    }
    else if (addr == FLASH_RAM_ADDRESS)
    {
        assert(false);
    }
    else
    {
        assert(false);
    }
}

static void grlib_stixflash_write(void *opaque, hwaddr addr,
                                  uint64_t value, unsigned size)
{
    STIXFLASH *stixflash = opaque;

    if (addr == FLASH_CMD_STA_ADDRESS)
    {
        stixflash->cmd_sta = (uint32_t)value;
        stixflash->command = (stixflash->cmd_sta >> FTNANDCTRL_CMDSTA_COMMAND) & FTNANDCTRL_CMDSTA_COMMAND_MASK;
        stixflash->mcm = (stixflash->cmd_sta >> FTNANDCTRL_CMDSTA_MCMADDRESS) & FTNANDCTRL_CMDSTA_MCMADDRESS_MASK;
    }
    else if (addr == FLASH_PAGE_ADDRESS)
    {
        stixflash->page_address = (uint32_t)value;
        stixflash->chip = (stixflash->page_address >> FTNANDCTRL_CMDSTA_COMMAND) & 0x7;
        stixflash->page = (stixflash->page_address >> FTNANDCTRL_PAGEADDRESS_PAGEADDRESS) & 0x3FFFF;
        stixflash->memory_bit_type = (stixflash->page_address) & 0x1;
    }
    else if (addr == FLASH_RAM_ADDRESS)
    {
        stixflash->ram_address = (uint32_t)value;
    }
    else
    {
        assert(0);
    }

    // Separating the last write command out on purpose for legibility
    if (addr == FLASH_CMD_STA_ADDRESS)
    {
        // make sure we are not busy anymore when we send out the ready read interrupt
        stixflash->cmd_sta &= ~(1u << FTNANDCTRL_CMDSTA_BUSY);

        // make sure we tell the software that the read was good.
        stixflash->cmd_sta |= (1u << ERROR_CODE_READY);

        // qemu_printf("Flash request fully configured\n");
        qemu_printf("Write completed: chip = %u, page = %u, mcm = %u, page_address = %u, memory_bit_type = %u, ram_address = 0x%x command = %u, status = %u\n",
                    stixflash->chip, stixflash->page, stixflash->mcm, stixflash->page_address, stixflash->memory_bit_type, stixflash->ram_address, stixflash->command, stixflash->cmd_sta);

        if (stixflash->command == COMMAND_PAGE_READ)
        {
            qemu_irq_pulse(stixflash->irq); //, 1); // NB: needs to be checked if '1' is OK in all cases. There is a comment that for LEON level needs to be equal to IRQ number.
        }
    }

    // qemu_printf("WRITE: %lu - %lu - %u\n", addr, value, size);
    // qemu_printf_grlib_apbuart_readl_unknown(addr);
}

static const MemoryRegionOps grlib_stixflash_ops = {
    .write = grlib_stixflash_write,
    .read = grlib_stixflash_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void init_and_load_flash_file(STIXFLASH *stixflash)
{
    qemu_printf("page size: %llu\n", FLASH_PAGE_SIZE);
    qemu_printf("file: %s\n", FLASH_FILE);

    stixflash->f = fopen(FLASH_FILE, "r+b");
    while (stixflash->f == NULL)
    {
        qemu_printf("can't open file, creating a new one\n");

        stixflash->f = fopen(FLASH_FILE, "w+b");
        if (stixflash->f == NULL)
        {
            qemu_printf("can't create file %d\n", errno);
            exit(1);
        }

        memset(stixflash->pageData, 0xFF, FLASH_PAGE_SIZE);

        fseek(stixflash->f, 0, SEEK_SET);
        for (uint32_t i = 0; i < 32 * 1024; i++)
        {
            fwrite(stixflash->pageData, sizeof(stixflash->pageData[0]), sizeof(stixflash->pageData) / sizeof(stixflash->pageData[0]), stixflash->f);
        }
        qemu_printf("file has been created");
        if (0 != fclose(stixflash->f))
        {
            qemu_printf("problem closing file");
        }
        stixflash->f = fopen(FLASH_FILE, "r+b");
    }

    fseek(stixflash->f, 0L, SEEK_END);
    qemu_printf("file size = %ld\n", ftell(stixflash->f));
    fseek(stixflash->f, 0L, SEEK_SET);
}

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

    init_and_load_flash_file(stixflash);
}

static void grlib_stixflash_reset(DeviceState *d)
{
    // STIXFLASH *stixflash = GRLIB_STIXFLASH(d);
}

/*static Property grlib_stixflash_properties[] = {
    DEFINE_PROP_UINT32("irq-line",  STIXFLASH, irq_line,  8),
    DEFINE_PROP_END_OF_LIST(),
};*/

static void grlib_stixflash_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = grlib_stixflash_realize;
    dc->reset = grlib_stixflash_reset;
    // device_class_set_props(dc, grlib_stixflash_properties);
}

static const TypeInfo grlib_stixflash_info = {
    .name = TYPE_GRLIB_STIXFLASH,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STIXFLASH),
    .class_init = grlib_stixflash_class_init,
};

static void grlib_stixflash_register_types(void)
{
    type_register_static(&grlib_stixflash_info);
}

type_init(grlib_stixflash_register_types)