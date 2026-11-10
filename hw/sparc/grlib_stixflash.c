#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/sparc/grlib_stix.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "chardev/char-fe.h"
#include "system/physmem.h"
#include "qemu/qemu-print.h"
#include "StixIdpu.h"

#if defined(CONFIG_FALLOCATE_PUNCH_HOLE)
#include <linux/falloc.h>
#endif

#include "trace.h"
#include "qom/object.h"

#define STIXFLASH_REG_SIZE 256 /* Size of memory mapped registers */
#define FLASH_FILE "flash.bin"

/* DEFINES TAKEN FROM FilesystemDefinition.h */
// all values are taken from the SAMSUNG K9F8G08X0M datasheet, STIX-DD-0814-ESC
#define		FLASH_PAGE_DATA_SIZE		4096uLL											// bytes
#define		FLASH_PAGE_HEADER_SIZE		128uLL											// bytes
#define		FLASH_PAGE_SIZE				(FLASH_PAGE_DATA_SIZE + FLASH_PAGE_HEADER_SIZE)	// 4'224 bytes
#define		FLASH_PAGES_PER_BLOCK		64uLL											// quantity
#define		FLASH_BLOCK_DATA_SIZE		(FLASH_PAGE_DATA_SIZE * FLASH_PAGES_PER_BLOCK)	// 262'144 bytes		= 256 KB
#define		FLASH_BLOCK_SIZE			(FLASH_PAGE_SIZE * FLASH_PAGES_PER_BLOCK)		// 270'336 bytes		= 264 KB
#define		FLASH_BLOCKS_PER_DEVICE		4096uLL											// quantity
#define		FLASH_DEVICE_SIZE			(FLASH_BLOCK_SIZE * FLASH_BLOCKS_PER_DEVICE)	// 1'107'296'256 bytes	= 1'056 MB
#define		FLASH_DEVICES_PER_MCM		8uLL											// quantity
#define		FLASH_MCM_SIZE				(FLASH_DEVICE_SIZE * FLASH_DEVICES_PER_MCM)		// 8'858'370'048 bytes	= 8.25 GB
#define		FLASH_NUM_MCMS				2uLL											// quantity
#define		FLASH_TOTAL_STORAGE			(FLASH_MCM_SIZE * FLASH_NUM_MCMS)				// 17'716'740'096 bytes	= 16.5 GB	

/* DEFINES TAKEN FROM stixidpu.h */
/*#define FTNANDCTRL_PAGEADDRESS_CHIPADDRESS 20
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
#define FTNANDCTRL_CMDSTA_STATUS 24*/

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

/* DEFINES TAKEN FROM stixIoConstants.h */
#define FLASH_PAGENUM_IN_BLOCK 64

OBJECT_DECLARE_SIMPLE_TYPE(STIXFLASH, GRLIB_STIXFLASH)

struct STIXFLASH
{
    SysBusDevice parent_obj;

    char *flash_file;   /* qdev property, defaults to "flash.bin" */
    /* D-I: when set (the default), the file stores every byte XOR 0xFF,
     * so sparse holes (file byte 0x00) are erased flash. With off, the
     * file is plain storage and legacy all-0xFF files stay usable. */
    bool inverted_storage;
    FILE *f;
    uint8_t page_data[FLASH_PAGE_SIZE];
    /* scratch buffer holding the on-disk (inverted) image of a page, so
     * page_data itself always keeps the guest image */
    uint8_t io_page[FLASH_PAGE_SIZE];

    MemoryRegion iomem;
    qemu_irq irq;

    hwaddr ram_address;
    uint32_t page_address;
    uint32_t cmd_sta;

    uint32_t write_counter; // flash commanding is configured with three writes, this counter keeps track of that

    /* host I/O failures are reported once per device instance, not per
     * page (a block erase touches 64 pages) */
    bool io_error_reported;

    uint32_t chip;
    uint32_t page;
    uint32_t command;
    uint32_t mcm;
    uint32_t memory_bit_type;
};

/* ---- error contract: guest-commanded failures never abort QEMU ----
 *
 * All file positions are handled with fseeko/ftello and off_t (the FILE *
 * API itself stays for the realize-time file creation).
 */

/* Size of the backing file, or -1 if it cannot be determined. */
static off_t flash_file_size(STIXFLASH *stixflash)
{
    if (fseeko(stixflash->f, 0, SEEK_END) != 0) {
        return -1;
    }
    return ftello(stixflash->f);
}

/* A page is beyond EOF when any of its bytes would land past the end of
 * the backing file: the file size IS the flash capacity (decision D-H).
 * This also fixes the baseline's `+ 1` off-by-one, which demanded a
 * phantom extra byte after the last complete page. */
static bool flash_page_beyond_eof(uint64_t position_in_file, off_t file_size)
{
    return position_in_file + FLASH_PAGE_SIZE > (uint64_t)file_size;
}

/* File offset of page_in_chip inside the commanded chip/MCM. The FSW's
 * linear page address is MCM-major, then chip, then page
 * (SPLIT_ADDRESS, BSW_ASW/src/BSW/Flash/Flash.c:70-73) and is issued as
 * pageAddress = chip<<20 | page, cmdSta bit 3 = mcm (Flash.c:81-84); the
 * decode into stixflash->chip / stixflash->mcm already exists in the
 * write handler. Chip 0/MCM 0 stays bit-for-bit page * 4224, so existing
 * flash.bin content remains valid. */
static uint64_t flash_offset(const STIXFLASH *stixflash, uint32_t page_in_chip)
{
    return (((uint64_t)stixflash->mcm * FLASH_DEVICES_PER_MCM + stixflash->chip)
            * (FLASH_BLOCKS_PER_DEVICE * FLASH_PAGENUM_IN_BLOCK)
            + (page_in_chip & 0x3FFFF)) * FLASH_PAGE_SIZE;
}

/* D-I: with inverted storage the file stores every byte XOR 0xFF, so a
 * sparse hole (0x00) reads back as guest 0xFF. In place is fine: the
 * callers invert either the freshly read page or the io_page scratch
 * image, never a buffer the guest image must be preserved in. */
static void flash_invert_page(uint8_t *buf)
{
    size_t i;

    for (i = 0; i < (size_t)FLASH_PAGE_SIZE; i++) {
        buf[i] ^= 0xFFu;
    }
}

/* Fail the in-flight command: STATUS.FAILED (bit 24) and STATUS.READY
 * (bit 30) set, BUSY (bit 4) clear, write sequence aborted. The FSW's ISR
 * (BSW_ASW/src/BSW/Flash/Flash.c:138-148) waits for the IRQ, then
 * requires BUSY==0 && READY==1 before it reads FAILED, so FAILED always
 * travels with READY and the IRQ pulse. */
static void flash_fail(STIXFLASH *stixflash, const char *why)
{
    stixflash->cmd_sta |= (1u << ERROR_CODE_FAILED);
    stixflash->cmd_sta |= (1u << ERROR_CODE_READY);
    stixflash->cmd_sta &= ~(1u << FTNANDCTRL_CMDSTA_BUSY);
    stixflash->write_counter = 0;
    trace_stixflash_error(why, stixflash->cmd_sta, stixflash->page_address);
    qemu_irq_pulse(stixflash->irq);
}

/* Host I/O failure (short fread/fwrite, failed seek): fail the command
 * like any other, but log only once per device instance, not per page. */
static void flash_io_error(STIXFLASH *stixflash, const char *op)
{
    if (!stixflash->io_error_reported) {
        error_report("stixflash: host I/O error on %s (file %s), failing the command",
                     op, stixflash->flash_file);
        stixflash->io_error_reported = true;
    }
    flash_fail(stixflash, "host-io");
}

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
}

static bool execute_flash_erase(STIXFLASH *stixflash)
{
    off_t file_size = flash_file_size(stixflash);
    uint32_t base_page;
    uint64_t offset;

    if (file_size < 0) {
        flash_io_error(stixflash, "erase");
        return false;
    }

    memset(stixflash->page_data, 0xFF, FLASH_PAGE_SIZE);

    // erase the block CONTAINING the commanded page (base = page & ~63,
    // 64 pages, never crossing a chip: base is 64-aligned inside the
    // 18-bit page field). The FSW aligns the page itself
    // (Flash.c:251-254), so the device-side rule is defensive.
    base_page = stixflash->page & ~(uint32_t)(FLASH_PAGENUM_IN_BLOCK - 1);
    offset = flash_offset(stixflash, base_page);

    if (stixflash->inverted_storage) {
        uint64_t length = (uint64_t)FLASH_PAGENUM_IN_BLOCK * FLASH_PAGE_SIZE;

        // pages past the backing file are skipped: the erase still
        // succeeds and the file is not extended (D-H)
        if ((uint64_t)file_size <= offset) {
            qemu_irq_pulse(stixflash->irq);
            return true;
        }
        if (offset + length > (uint64_t)file_size) {
            length = (uint64_t)file_size - offset;
        }

        // flush BEFORE punching: buffered data written after the punch
        // would refill the hole
        if (fflush(stixflash->f) != 0) {
            flash_io_error(stixflash, "erase");
            return false;
        }

#ifdef CONFIG_FALLOCATE_PUNCH_HOLE
        // the inverted erased block IS a file hole
        if (fallocate(fileno(stixflash->f),
                      FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                      (off_t)offset, (off_t)length) == 0) {
            qemu_irq_pulse(stixflash->irq);
            return true;
        }
        trace_stixflash_punch_fallback(errno);
#else
        trace_stixflash_punch_fallback(ENOSYS);
#endif

        // fallback (and builds without punch support): store the erased
        // page as 0x00 bytes, which inverted reads turn into guest 0xFF
        memset(stixflash->page_data, 0x00, FLASH_PAGE_SIZE);
    }

    // iterate through pages in block (plain mode writes 0xFF pages)
    for(uint32_t i = 0; i < FLASH_PAGENUM_IN_BLOCK; i++)
    {
        uint64_t position_in_file = flash_offset(stixflash, base_page + i);

        // pages past the backing file are skipped: the erase still succeeds
        // and the file is not extended (D-H)
        if(flash_page_beyond_eof(position_in_file, file_size))
        {
            continue;
        }

        if(fseeko(stixflash->f, (off_t)position_in_file, SEEK_SET) != 0)
        {
            flash_io_error(stixflash, "erase");
            return false;
        }
        if(fwrite(stixflash->page_data, sizeof(uint8_t), FLASH_PAGE_SIZE, stixflash->f) != (size_t)FLASH_PAGE_SIZE)
        {
            flash_io_error(stixflash, "erase");
            return false;
        }
    }

    // push the erased pages out before the completion IRQ
    if(fflush(stixflash->f) != 0)
    {
        flash_io_error(stixflash, "erase");
        return false;
    }

    qemu_irq_pulse(stixflash->irq); //, 1); // NB: needs to be checked if '1' is OK in all cases. There is a comment that for LEON level needs to be equal to IRQ number.
    return true;
}

static bool execute_flash_read(STIXFLASH *stixflash)
{
    uint64_t position_in_file = flash_offset(stixflash, stixflash->page);
    off_t file_size = flash_file_size(stixflash);
    size_t ret;

    if (file_size < 0) {
        flash_io_error(stixflash, "read");
        return false;
    }

    // a page past the backing file reads back as an erased page (D-H)
    if (flash_page_beyond_eof(position_in_file, file_size)) {
        memset(stixflash->page_data, 0xFF, FLASH_PAGE_SIZE);
    } else {
        if (fseeko(stixflash->f, (off_t)position_in_file, SEEK_SET) != 0) {
            flash_io_error(stixflash, "read");
            return false;
        }

        ret = fread(stixflash->page_data, sizeof(uint8_t), FLASH_PAGE_SIZE, stixflash->f);
        if (ret != (size_t)FLASH_PAGE_SIZE) {
            flash_io_error(stixflash, "read");
            return false;
        }

        // D-I: a file byte 0x00 is guest 0xFF
        if (stixflash->inverted_storage) {
            flash_invert_page(stixflash->page_data);
        }
    }

    // update the flash register to be not busy anymore
    stixflash->cmd_sta &= ~(1u << FTNANDCTRL_CMDSTA_BUSY);

    // the address space of QEMU is NOT FSW; must use wrapper function to translate the address and properly copy the data
    physical_memory_write(stixflash->ram_address, stixflash->page_data, FLASH_PAGE_SIZE);

    // need to send out an interrupt that we're done
    qemu_irq_pulse(stixflash->irq); //, 1); // NB: needs to be checked if '1' is OK in all cases. There is a comment that for LEON level needs to be equal to IRQ number.
    return true;
}

static bool execute_flash_write(STIXFLASH *stixflash)
{
    uint64_t position_in_file = flash_offset(stixflash, stixflash->page);
    off_t file_size = flash_file_size(stixflash);
    const uint8_t *out = stixflash->page_data;
    size_t ret;

    if (file_size < 0) {
        flash_io_error(stixflash, "write");
        return false;
    }

    // a page past the backing file fails the command; the file is never
    // extended (a hole would read 0x00 = bad-block marker on the next boot
    // scan, D-H)
    if (flash_page_beyond_eof(position_in_file, file_size)) {
        flash_fail(stixflash, "beyond-backing-file");
        return false;
    }

    if (fseeko(stixflash->f, (off_t)position_in_file, SEEK_SET) != 0) {
        flash_io_error(stixflash, "write");
        return false;
    }

    // the address space of QEMU is NOT FSW; must use wrapper function to translate the address and properly copy the data
    physical_memory_read(stixflash->ram_address, stixflash->page_data, FLASH_PAGE_SIZE);

    // D-I: on disk every byte is stored XOR 0xFF. The inversion goes into
    // the io_page scratch image; page_data itself keeps the guest image.
    if (stixflash->inverted_storage) {
        memcpy(stixflash->io_page, stixflash->page_data, FLASH_PAGE_SIZE);
        flash_invert_page(stixflash->io_page);
        out = stixflash->io_page;
    }

    ret = fwrite(out, sizeof(uint8_t), FLASH_PAGE_SIZE, stixflash->f);
    if (ret != (size_t)FLASH_PAGE_SIZE) {
        flash_io_error(stixflash, "write");
        return false;
    }

    // push the page out before the completion IRQ (no fsync: the guest
    // must be able to observe its own write from the host side)
    if (fflush(stixflash->f) != 0) {
        flash_io_error(stixflash, "write");
        return false;
    }

    // need to send out an interrupt that we're done
    qemu_irq_pulse(stixflash->irq); //, 1); // NB: needs to be checked if '1' is OK in all cases. There is a comment that for LEON level needs to be equal to IRQ number.
    return true;
}

static uint64_t grlib_stixflash_read(void *opaque, hwaddr addr, unsigned size)
{
    STIXFLASH *stixflash = opaque;
    uint32_t value;

    /*
    qemu_printf("FLASH | Read requested: addr = %lu, chip = %u, page = %u, mcm = %u, page_address = %u, memory_bit_type = %u, ram_address = 0x%x, command = %u, status = 0x%x\n",
                addr, stixflash->chip, stixflash->page, stixflash->mcm, stixflash->page_address, stixflash->memory_bit_type, (uint32_t)stixflash->ram_address, stixflash->command, stixflash->cmd_sta);
    */

    // reading mid-sequence is tolerated (the FSW never does it): return the
    // live registers instead of asserting
    if (stixflash->write_counter != 0) {
        trace_stixflash_midseq_read(addr, stixflash->write_counter);
    }

    switch (addr)
    {
    case FLASH_CMD_STA_ADDRESS:
        value = stixflash->cmd_sta;
        break;

    case FLASH_PAGE_ADDRESS:
        value = stixflash->page_address;
        break;

    case FLASH_RAM_ADDRESS:
        value = (uint32_t)stixflash->ram_address;
        break;

    default:
        trace_stixflash_unmapped_read(addr);
        value = 0;
        break;
    }

    return value;
}

static void grlib_stixflash_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    STIXFLASH *stixflash = opaque;

    switch (addr)
    {
    case FLASH_CMD_STA_ADDRESS: // update the status register
        if (stixflash->write_counter != 2)
        {
            flash_fail(stixflash, "sequence");
            return;
        }
        stixflash->cmd_sta = (uint32_t)value;

        // small workaround, this un-bysies the FLASH; should be updated once we want to have the flash busy
        stixflash->cmd_sta |= (1u << ERROR_CODE_WRPROT);

        stixflash->command = (stixflash->cmd_sta >> FTNANDCTRL_CMDSTA_COMMAND) & FTNANDCTRL_CMDSTA_COMMAND_MASK;
        stixflash->mcm = (stixflash->cmd_sta >> FTNANDCTRL_CMDSTA_MCMADDRESS) & FTNANDCTRL_CMDSTA_MCMADDRESS_MASK;
        stixflash->memory_bit_type = (stixflash->cmd_sta >> FTNANDCTRL_CMDSTA_FLASHTYPE) & FTNANDCTRL_CMDSTA_FLASHTYPE_MASK;
        stixflash->write_counter += 1;
        break;

    case FLASH_PAGE_ADDRESS: // configure the flash addressing
        if (stixflash->write_counter != 1)
        {
            flash_fail(stixflash, "sequence");
            return;
        }
        stixflash->page_address = (uint32_t)value;
        stixflash->chip = (stixflash->page_address >> FTNANDCTRL_PAGEADDRESS_CHIPADDRESS) & FTNANDCTRL_PAGEADDRESS_CHIPADDRESS_MASK;
        stixflash->page = (stixflash->page_address >> FTNANDCTRL_PAGEADDRESS_PAGEADDRESS) & FTNANDCTRL_PAGEADDRESS_PAGEADDRESS_MASK;
        stixflash->write_counter += 1;
        break;

    case FLASH_RAM_ADDRESS: // configure the ram address (write-back?)
        if (stixflash->write_counter != 0)
        {
            flash_fail(stixflash, "sequence");
            return;
        }
        stixflash->ram_address = (uint32_t)value;
        stixflash->write_counter += 1;
        break;

    default:
        // unknown register offset: ignore, keep the sequence state intact
        trace_stixflash_unmapped_write(addr, value);
        return;
    }

    // Separating the last write command out on purpose for legibility
    if (stixflash->write_counter == 3)
    {
        bool ok = true;

        // mock the flash and fpga readiness a bit; while executing the command
        // we are not ready; at the end of this routine, we will be again
        stixflash->cmd_sta &= ~(1u << ERROR_CODE_READY);

        // now we execute the flash command
        switch (stixflash->command)
        {
        case COMMAND_RESET:
            execute_flash_reset(stixflash);
            break;

        case COMMAND_BLOCK_ERASE:
            // fake a write protection to the cell we are working on
            stixflash->cmd_sta &= ~(1u << ERROR_CODE_WRPROT);

            ok = execute_flash_erase(stixflash);

            // un-write-protect the cell
            stixflash->cmd_sta |= (1u << ERROR_CODE_WRPROT);
            break;

        case COMMAND_PAGE_READ:
            ok = execute_flash_read(stixflash);
            break;

        case COMMAND_PAGE_WRITE:
            // fake a write protection to the cell we are working on
            stixflash->cmd_sta &= ~(1u << ERROR_CODE_WRPROT);

            ok = execute_flash_write(stixflash);

            // un-write-protect the cell
            stixflash->cmd_sta |= (1u << ERROR_CODE_WRPROT);
            break;

        default:
            // unknown command, incl. COMMAND_FSM_ACCESS (4), which is
            // defined in the FSW driver (Flash.c:60) but never issued by it
            flash_fail(stixflash, "command");
            return;
        }

        if (!ok)
        {
            // flash_fail() already latched FAILED|READY, pulsed the IRQ and
            // reset the write sequence; the success path below must not run
            // (it would clear FAILED)
            return;
        }

        // make sure we tell the software that the command finished.
        stixflash->cmd_sta |= (1u << ERROR_CODE_READY);

        // ensure we flag no error; could be used to inject errors
        stixflash->cmd_sta &= ~(1u << ERROR_CODE_FAILED);

        /*
        qemu_printf("FLASH | Write completed: chip = %u, page = %u, mcm = %u, page_address = %u, memory_bit_type = %u, ram_address = 0x%x command = %u, status = 0x%x\n",
                    stixflash->chip, stixflash->page, stixflash->mcm, stixflash->page_address, stixflash->memory_bit_type, (uint32_t)stixflash->ram_address, stixflash->command, stixflash->cmd_sta);
        */

        // reset the write counter to 0
        stixflash->write_counter = 0;
    }
}

static const MemoryRegionOps grlib_stixflash_ops = {
    .write = grlib_stixflash_write,
    .read = grlib_stixflash_read,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void init_and_load_flash_file(STIXFLASH *stixflash)
{
    qemu_printf("page size: %llu\n", FLASH_PAGE_SIZE);
    qemu_printf("file: %s\n", stixflash->flash_file);

    stixflash->f = fopen(stixflash->flash_file, "r+b");
    while (stixflash->f == NULL)
    {
        qemu_printf("can't open file, creating a new one\n");

        stixflash->f = fopen(stixflash->flash_file, "w+b");
        if (stixflash->f == NULL)
        {
            qemu_printf("can't create file %d\n", errno);
            exit(1);
        }

        if (stixflash->inverted_storage) {
            /* D-I: erased flash is a file hole. Create the backing file
             * sparse and instantly instead of writing 16.5 GB of 0xFF;
             * holes read 0x00, which inverted reads turn into guest
             * 0xFF. */
            if (ftruncate(fileno(stixflash->f),
                          (off_t)FLASH_TOTAL_STORAGE) != 0) {
                error_report("stixflash: cannot size %s to %llu bytes: %s",
                             stixflash->flash_file,
                             (unsigned long long)FLASH_TOTAL_STORAGE,
                             strerror(errno));
                exit(1);
            }
        } else
        {
            memset(stixflash->page_data, 0xFF, FLASH_PAGE_SIZE);

            fseek(stixflash->f, 0, SEEK_SET);
            for (uint32_t i = 0; i < FLASH_TOTAL_STORAGE / FLASH_PAGE_SIZE; i++)
            {
                fwrite(stixflash->page_data, sizeof(stixflash->page_data[0]), sizeof(stixflash->page_data) / sizeof(stixflash->page_data[0]), stixflash->f);
            }
        }

        trace_stixflash_create(stixflash->flash_file, FLASH_TOTAL_STORAGE,
                               stixflash->inverted_storage);

        if (0 != fclose(stixflash->f))
        {
            qemu_printf("problem closing file");
        }
        
        stixflash->f = fopen(stixflash->flash_file, "r+b");
    }

    fseek(stixflash->f, 0L, SEEK_END);
    qemu_printf("file size = %ld\n", ftell(stixflash->f));
    fseek(stixflash->f, 0L, SEEK_SET);
}

static void grlib_stixflash_realize(DeviceState *dev, Error **errp)
{
    STIXFLASH *stixflash = GRLIB_STIXFLASH(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    sysbus_init_irq(sbd, &stixflash->irq);

    memory_region_init_io(&stixflash->iomem, OBJECT(stixflash), &grlib_stixflash_ops, stixflash, "stixflash", STIXFLASH_REG_SIZE);

    sysbus_init_mmio(sbd, &stixflash->iomem);

    if (!stixflash->flash_file) {
        stixflash->flash_file = g_strdup("flash.bin");
    }
    init_and_load_flash_file(stixflash);
}

static void grlib_stixflash_reset(DeviceState *d)
{
    // STIXFLASH *stixflash = GRLIB_STIXFLASH(d);
}


static const Property grlib_stixflash_properties[] = {
    DEFINE_PROP_STRING("flash-file", STIXFLASH, flash_file),
    DEFINE_PROP_BOOL("inverted-storage", STIXFLASH, inverted_storage, true),
};

static void grlib_stixflash_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = grlib_stixflash_realize;
    device_class_set_legacy_reset(dc, grlib_stixflash_reset);
    device_class_set_props(dc, grlib_stixflash_properties);
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