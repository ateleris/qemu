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
#define FTNANDCTRL_PAGEADDRESS_CHIPADDRESS_MASK 0x7
#define FTNANDCTRL_PAGEADDRESS_PAGEADDRESS 0
#define FTNANDCTRL_PAGEADDRESS_PAGEADDRESS_MASK 0x3FFFF
#define FTNANDCTRL_CMDSTA_COMMAND 0
#define FTNANDCTRL_CMDSTA_COMMAND_MASK 0x7
#define FTNANDCTRL_CMDSTA_MCMADDRESS 3
#define FTNANDCTRL_CMDSTA_MCMADDRESS_MASK 0x1
#define FTNANDCTRL_CMDSTA_FLASHTYPE 7
#define FTNANDCTRL_CMDSTA_FLASHTYPE_MASK 0x1
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
    uint8_t page_data[FLASH_PAGE_SIZE];

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t ram_address;
    uint32_t page_address;
    uint32_t cmd_sta;

    uint32_t write_counter; // flash commanding is configured with three writes, this counter keeps track of that

    uint32_t chip;
    uint32_t page;
    uint32_t command;
    uint32_t mcm;
    uint32_t memory_bit_type;
};

/*
    Generally, a flash command is prepared by a setup step of the control block and executed with START_FLASH_OPERATION.
    Executing the command will cause THREE WRITE function calls, often followed by ONE OR MANY READ calls (e.g., to check
    the success or failure of the flash command).

    FLASH RESET, which has command value zero (0), triggers THREE WRITE function calls and then ONE READ function
    call, in which the status register is checked: if ((FLASH_REGISTERS->cmdSta & (1u << FTNANDCTRL_CMDSTA_BUSY)) == 0)



*/

/*
    This function resets the flash.

    The reset command, at this point, always successfully completes and does not use
    any of the flash targets (page addres, chip, etc.)
*/
static void execute_flash_reset(STIXFLASH *stixflash)
{
    // with the following register update, we signal that the flash successfully reset
    stixflash->cmd_sta &= ~(1u << FTNANDCTRL_CMDSTA_BUSY);
    
    // make sure we tell the software that the read was good.
    // NB: it also works without this, but I assume we should set it to ready
    // let's see if we need it
    // stixflash->cmd_sta |= (1u << ERROR_CODE_READY);
}

static void execute_flash_read(STIXFLASH *stixflash)
{
    int32_t ret = 0;

    fseek(stixflash->f, 0, SEEK_END);
    int64_t length = ftell(stixflash->f);

    // TODO: check if page_address is unique or if we do neet to include chip, page and mcm
    uint32_t position_in_file = stixflash->page_address * FLASH_PAGE_SIZE;

    if(length < (position_in_file + FLASH_PAGE_SIZE + 1))
    {
        assert(false);
    }

    fseek(stixflash->f, position_in_file, SEEK_SET);
    ret = fread(stixflash->page_data, sizeof(uint8_t), FLASH_PAGE_SIZE, stixflash->f);
    assert(ret);
    fseek(stixflash->f, 0, SEEK_END);

    // update the flash register to be not busy anymore
    stixflash->cmd_sta &= ~(1u << FTNANDCTRL_CMDSTA_BUSY);

    // need to send out an interrupt that we're done
    qemu_irq_pulse(stixflash->irq); //, 1); // NB: needs to be checked if '1' is OK in all cases. There is a comment that for LEON level needs to be equal to IRQ number. 
}

static uint64_t grlib_stixflash_read(void *opaque, hwaddr addr, unsigned size)
{
    STIXFLASH *stixflash = opaque;

    qemu_printf("Read requested: addr = %lu, chip = %u, page = %u, mcm = %u, page_address = %u, memory_bit_type = %u, ram_address = 0x%x, command = %u, status = 0x%x\n",
                addr, stixflash->chip, stixflash->page, stixflash->mcm, stixflash->page_address, stixflash->memory_bit_type, stixflash->ram_address, stixflash->command, stixflash->cmd_sta);

    assert(stixflash->write_counter == 0);

    if (addr == FLASH_CMD_STA_ADDRESS) // this is a read of the flash status register
    {
        return stixflash->cmd_sta;
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

static void grlib_stixflash_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    STIXFLASH *stixflash = opaque;

    switch (addr)
    {
    case FLASH_CMD_STA_ADDRESS: // update the status register
        assert(stixflash->write_counter == 2);
        stixflash->cmd_sta = (uint32_t)value;
        stixflash->command = (stixflash->cmd_sta >> FTNANDCTRL_CMDSTA_COMMAND) & FTNANDCTRL_CMDSTA_COMMAND_MASK;
        stixflash->mcm = (stixflash->cmd_sta >> FTNANDCTRL_CMDSTA_MCMADDRESS) & FTNANDCTRL_CMDSTA_MCMADDRESS_MASK;
        stixflash->memory_bit_type = (stixflash->cmd_sta >> FTNANDCTRL_CMDSTA_FLASHTYPE) & FTNANDCTRL_CMDSTA_FLASHTYPE_MASK;
        stixflash->write_counter += 1;
        break;
    
    case FLASH_PAGE_ADDRESS: // configure the flash addressing
        assert(stixflash->write_counter == 1);
        stixflash->page_address = (uint32_t)value;
        stixflash->chip = (stixflash->page_address >> FTNANDCTRL_PAGEADDRESS_CHIPADDRESS) & FTNANDCTRL_PAGEADDRESS_CHIPADDRESS_MASK;
        stixflash->page = (stixflash->page_address >> FTNANDCTRL_PAGEADDRESS_PAGEADDRESS) & FTNANDCTRL_PAGEADDRESS_PAGEADDRESS_MASK;
        stixflash->write_counter += 1;
        break;

    case FLASH_RAM_ADDRESS: // configure the ram address (write-back?)
        assert(stixflash->write_counter == 0);
        stixflash->ram_address = (uint32_t)value;
        stixflash->write_counter += 1;
        break;
    default:
        assert(false);
    }

    // Separating the last write command out on purpose for legibility
    if (stixflash->write_counter == 3)
    {
        // mock the flash and fpga readiness a bit; while executing the command
        // we are not ready; at the end of this routine, we will be again
        stixflash->cmd_sta &= ~(1u << ERROR_CODE_READY);

        // now we execute the flash command
        switch (stixflash->command)
        {
        case COMMAND_RESET:
            execute_flash_reset(stixflash);
            break;
        
        case COMMAND_PAGE_READ:
            execute_flash_read(stixflash);
            break;
        
        default:
            qemu_printf("UNKNOWN COMMAND %u\n", stixflash->command);
            assert(false);
        }

        // make sure we tell the software that the read was good.
        stixflash->cmd_sta |= (1u << ERROR_CODE_READY);

        // qemu_printf("Flash request fully configured\n");
        qemu_printf("Write completed: chip = %u, page = %u, mcm = %u, page_address = %u, memory_bit_type = %u, ram_address = 0x%x command = %u, status = 0x%x\n",
                    stixflash->chip, stixflash->page, stixflash->mcm, stixflash->page_address, stixflash->memory_bit_type, stixflash->ram_address, stixflash->command, stixflash->cmd_sta);

        // reset the write counter to 0
        stixflash->write_counter = 0;
    }

    assert(stixflash->write_counter < 4);
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

        memset(stixflash->page_data, 0xFF, FLASH_PAGE_SIZE);

        fseek(stixflash->f, 0, SEEK_SET);
        for (uint32_t i = 0; i < 32 * 1024; i++)
        {
            fwrite(stixflash->page_data, sizeof(stixflash->page_data[0]), sizeof(stixflash->page_data) / sizeof(stixflash->page_data[0]), stixflash->f);
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