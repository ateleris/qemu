#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/sparc/grlib_stix.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "chardev/char-fe.h"
#include "system/physmem.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "migration/vmstate.h"
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

/*
 * D-F6: a latency must stay well below the FSW's 50 ms page/erase
 * timeouts (FILESYSTEM_PAGE_READ_TIMEOUT & co., configFSW.h:336-338);
 * below 20 us the IRQ may beat the FSW's rtems_event_receive and the
 * op no longer blocks the calling task.
 */
#define STIXFLASH_LATENCY_MAX_US  40000u
#define STIXFLASH_LATENCY_WARN_US 20u

OBJECT_DECLARE_SIMPLE_TYPE(STIXFLASH, GRLIB_STIXFLASH)

/*
 * Flash latency plan D-F1: the third write (cmdSta) latches the command
 * into this record (issue); stixflash_complete() executes it. The
 * record keeps the addressing of the issue, so the RAMAD/PAGEAD
 * registers may change while the op is in flight. One op at a time,
 * as in the FSW driver (Flash.c:239-244, 289-294, 335-340).
 */
typedef struct StixFlashOp {
    bool pending;
    uint32_t cmd;
    uint32_t mcm;
    uint32_t chip;
    uint32_t page;
    uint32_t page_address;  /* the PAGEAD value at issue, for traces */
    hwaddr ram_address;
    /* a program's RAM sample at issue was refused (D-F3) */
    bool sample_failed;
} StixFlashOp;

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

    /* the latched command (D-F1); page_data holds its page image */
    StixFlashOp op;

    /* D-F6 properties: off = every op completes inline */
    bool latency;
    uint32_t tread_us;
    uint32_t tprog_us;
    uint32_t tbers_us;
    /* completion timer (QEMU_CLOCK_VIRTUAL), created last in realize */
    QEMUTimer *timer;

    /* D-F9 read-only counters */
    uint64_t ops_read;
    uint64_t ops_prog;
    uint64_t ops_erase;
    uint64_t ops_abort;
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
 * write handler decodes them and the issue latches them into the op
 * record. Chip 0/MCM 0 stays bit-for-bit page * 4224, so existing
 * flash.bin content remains valid. */
static uint64_t flash_offset(const StixFlashOp *op, uint32_t page_in_chip)
{
    return (((uint64_t)op->mcm * FLASH_DEVICES_PER_MCM + op->chip)
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

/*
 * D-F3: the read DMA at a timer completion runs outside any MMIO
 * dispatch, so it engages the device's re-entrancy guard itself, as
 * stixspw_guard_enter/exit do (grlib_stixspw.c, fix a83fe9a3a5). A
 * ramAddress inside this device's MMIO is then refused by memory.c
 * (MEMTX_ACCESS_ERROR), exactly as on the inline path, where memory.c
 * has engaged the guard for the cmdSta write already.
 */
static bool stixflash_guard_enter(STIXFLASH *s)
{
    MemReentrancyGuard *g = &DEVICE(s)->mem_reentrancy_guard;
    bool was = g->engaged_in_io;

    g->engaged_in_io = true;
    return was;
}

static void stixflash_guard_exit(STIXFLASH *s, bool was)
{
    DEVICE(s)->mem_reentrancy_guard.engaged_in_io = was;
}

/*
 * Status of a finished (or refused) command: STATUS.FAILED (bit 24) and
 * STATUS.READY (bit 30) set, BUSY (bit 4) clear. The FSW's ISR
 * (BSW_ASW/src/BSW/Flash/Flash.c:138-148) waits for the IRQ, then
 * requires BUSY==0 && READY==1 before it reads FAILED, so FAILED always
 * travels with READY and the IRQ pulse. This status-only variant is the
 * completion's (D-F1): completion never touches write_counter, which
 * belongs to the issue.
 */
static void flash_fail_status(STIXFLASH *stixflash, const char *why,
                              uint32_t page_address)
{
    stixflash->cmd_sta |= (1u << ERROR_CODE_FAILED);
    stixflash->cmd_sta |= (1u << ERROR_CODE_READY);
    stixflash->cmd_sta &= ~(1u << FTNANDCTRL_CMDSTA_BUSY);
    trace_stixflash_error(why, stixflash->cmd_sta, page_address);
}

/*
 * A guest-commanded failure at issue time (sequence violation, unknown
 * command): no op starts, so it fails at once, aborts the write sequence
 * and pulses the IRQ.
 */
static void flash_fail(STIXFLASH *stixflash, const char *why)
{
    stixflash->write_counter = 0;
    flash_fail_status(stixflash, why, stixflash->page_address);
    qemu_irq_pulse(stixflash->irq);
}

/*
 * Host I/O failure (short fread/fwrite, failed seek): the command fails
 * like any other ("host-io"), but it is logged only once per device
 * instance, not per page. Returns the failure reason for the caller.
 */
static const char *flash_io_error(STIXFLASH *stixflash, const char *op)
{
    if (!stixflash->io_error_reported) {
        error_report("stixflash: host I/O error on %s (file %s), failing the command",
                     op, stixflash->flash_file);
        stixflash->io_error_reported = true;
    }
    return "host-io";
}

/*
    Generally, a flash command is prepared by a setup step of the control block and executed with START_FLASH_OPERATION.
    Executing the command will cause THREE WRITE function calls, often followed by ONE OR MANY READ calls (e.g., to check
    the success or failure of the flash command).

    FLASH RESET, which has command value zero (0), triggers THREE WRITE function calls and then ONE READ function
    call, in which the status register is checked: if ((FLASH_REGISTERS->cmdSta & (1u << FTNANDCTRL_CMDSTA_BUSY)) == 0)
*/

/*
 * The execute helpers run at completion (D-F1). Each returns NULL on
 * success or the failure reason; none of them touches the status
 * register or the IRQ: stixflash_complete() sets the status and pulses
 * the IRQ once, last.
 */
static const char *execute_flash_erase(STIXFLASH *stixflash)
{
    const StixFlashOp *op = &stixflash->op;
    off_t file_size = flash_file_size(stixflash);
    uint32_t base_page;
    uint64_t offset;

    if (file_size < 0) {
        return flash_io_error(stixflash, "erase");
    }

    memset(stixflash->page_data, 0xFF, FLASH_PAGE_SIZE);

    // erase the block CONTAINING the commanded page (base = page & ~63,
    // 64 pages, never crossing a chip: base is 64-aligned inside the
    // 18-bit page field). The FSW aligns the page itself
    // (Flash.c:251-254), so the device-side rule is defensive.
    base_page = op->page & ~(uint32_t)(FLASH_PAGENUM_IN_BLOCK - 1);
    offset = flash_offset(op, base_page);

    if (stixflash->inverted_storage) {
        uint64_t length = (uint64_t)FLASH_PAGENUM_IN_BLOCK * FLASH_PAGE_SIZE;

        // pages past the backing file are skipped: the erase still
        // succeeds and the file is not extended (D-H)
        if ((uint64_t)file_size <= offset) {
            return NULL;
        }
        if (offset + length > (uint64_t)file_size) {
            length = (uint64_t)file_size - offset;
        }

        // flush BEFORE punching: buffered data written after the punch
        // would refill the hole
        if (fflush(stixflash->f) != 0) {
            return flash_io_error(stixflash, "erase");
        }

#ifdef CONFIG_FALLOCATE_PUNCH_HOLE
        // the inverted erased block IS a file hole
        if (fallocate(fileno(stixflash->f),
                      FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                      (off_t)offset, (off_t)length) == 0) {
            return NULL;
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
        uint64_t position_in_file = flash_offset(op, base_page + i);

        // pages past the backing file are skipped: the erase still succeeds
        // and the file is not extended (D-H)
        if(flash_page_beyond_eof(position_in_file, file_size))
        {
            continue;
        }

        if(fseeko(stixflash->f, (off_t)position_in_file, SEEK_SET) != 0)
        {
            return flash_io_error(stixflash, "erase");
        }
        if(fwrite(stixflash->page_data, sizeof(uint8_t), FLASH_PAGE_SIZE, stixflash->f) != (size_t)FLASH_PAGE_SIZE)
        {
            return flash_io_error(stixflash, "erase");
        }
    }

    // push the erased pages out before the completion IRQ
    if(fflush(stixflash->f) != 0)
    {
        return flash_io_error(stixflash, "erase");
    }

    return NULL;
}

static const char *execute_flash_read(STIXFLASH *stixflash)
{
    const StixFlashOp *op = &stixflash->op;
    uint64_t position_in_file = flash_offset(op, op->page);
    off_t file_size = flash_file_size(stixflash);
    size_t ret;

    if (file_size < 0) {
        return flash_io_error(stixflash, "read");
    }

    // a page past the backing file reads back as an erased page (D-H)
    if (flash_page_beyond_eof(position_in_file, file_size)) {
        memset(stixflash->page_data, 0xFF, FLASH_PAGE_SIZE);
    } else {
        if (fseeko(stixflash->f, (off_t)position_in_file, SEEK_SET) != 0) {
            return flash_io_error(stixflash, "read");
        }

        ret = fread(stixflash->page_data, sizeof(uint8_t), FLASH_PAGE_SIZE, stixflash->f);
        if (ret != (size_t)FLASH_PAGE_SIZE) {
            return flash_io_error(stixflash, "read");
        }

        // D-I: a file byte 0x00 is guest 0xFF
        if (stixflash->inverted_storage) {
            flash_invert_page(stixflash->page_data);
        }
    }

    /*
     * D-F2/D-F3: the data lands in guest RAM at completion, under the
     * re-entrancy guard; a refused or failed DMA fails the command
     */
    {
        bool was = stixflash_guard_enter(stixflash);
        MemTxResult r = address_space_write(&address_space_memory,
                                            op->ram_address,
                                            MEMTXATTRS_UNSPECIFIED,
                                            stixflash->page_data,
                                            FLASH_PAGE_SIZE);

        stixflash_guard_exit(stixflash, was);
        if (r != MEMTX_OK) {
            return "dma";
        }
    }

    return NULL;
}

/* page_data holds the guest page image sampled at issue */
static const char *execute_flash_write(STIXFLASH *stixflash)
{
    const StixFlashOp *op = &stixflash->op;
    uint64_t position_in_file = flash_offset(op, op->page);
    off_t file_size = flash_file_size(stixflash);
    const uint8_t *out = stixflash->page_data;
    size_t ret;

    if (file_size < 0) {
        return flash_io_error(stixflash, "write");
    }

    /* D-F3: the RAM sample at issue was refused: nothing is written */
    if (op->sample_failed) {
        return "dma";
    }

    // a page past the backing file fails the command; the file is never
    // extended (a hole would read 0x00 = bad-block marker on the next boot
    // scan, D-H)
    if (flash_page_beyond_eof(position_in_file, file_size)) {
        return "beyond-backing-file";
    }

    if (fseeko(stixflash->f, (off_t)position_in_file, SEEK_SET) != 0) {
        return flash_io_error(stixflash, "write");
    }

    // D-I: on disk every byte is stored XOR 0xFF. The inversion goes into
    // the io_page scratch image; page_data itself keeps the guest image.
    if (stixflash->inverted_storage) {
        memcpy(stixflash->io_page, stixflash->page_data, FLASH_PAGE_SIZE);
        flash_invert_page(stixflash->io_page);
        out = stixflash->io_page;
    }

    ret = fwrite(out, sizeof(uint8_t), FLASH_PAGE_SIZE, stixflash->f);
    if (ret != (size_t)FLASH_PAGE_SIZE) {
        return flash_io_error(stixflash, "write");
    }

    // push the page out before the completion IRQ (no fsync: the guest
    // must be able to observe its own write from the host side)
    if (fflush(stixflash->f) != 0) {
        return flash_io_error(stixflash, "write");
    }

    return NULL;
}

/*
 * D-F1 completion: execute the latched op (host file I/O, the read
 * DMA), then set the final status (BUSY 0, READY 1, WRPROT 1, FAILED
 * from the result) and pulse the IRQ once, last, so the FSW's ISR
 * (Flash.c:125-174) always sees the final status. Completion never
 * touches write_counter: a completion can land between the RAMAD/PAGEAD
 * writes of the next sequence (Flash_reset, Flash.c:81-84, 212).
 */
static void stixflash_complete(STIXFLASH *stixflash)
{
    StixFlashOp *op = &stixflash->op;
    const char *why;

    switch (op->cmd) {
    case COMMAND_BLOCK_ERASE:
        why = execute_flash_erase(stixflash);
        break;
    case COMMAND_PAGE_READ:
        why = execute_flash_read(stixflash);
        break;
    case COMMAND_PAGE_WRITE:
        why = execute_flash_write(stixflash);
        break;
    default:
        g_assert_not_reached(); /* issue latches only these three */
    }

    op->pending = false;
    stixflash->cmd_sta |= (1u << ERROR_CODE_WRPROT);
    if (why) {
        flash_fail_status(stixflash, why, op->page_address);
    } else {
        stixflash->cmd_sta &= ~(1u << FTNANDCTRL_CMDSTA_BUSY);
        stixflash->cmd_sta |= (1u << ERROR_CODE_READY);
        stixflash->cmd_sta &= ~(1u << ERROR_CODE_FAILED);
    }
    trace_stixflash_op_done(op->cmd, op->page_address, stixflash->cmd_sta);
    qemu_irq_pulse(stixflash->irq);
}

static void stixflash_timer_cb(void *opaque)
{
    STIXFLASH *stixflash = opaque;

    if (stixflash->op.pending) {
        stixflash_complete(stixflash);
    }
}

/* D-F4: drop the pending op: no IRQ, no DMA, nothing committed */
static void stixflash_abort(STIXFLASH *stixflash)
{
    timer_del(stixflash->timer);
    stixflash->op.pending = false;
    stixflash->ops_abort++;
    trace_stixflash_op_abort(stixflash->op.cmd);
}

/* D-F6: the op's latency in us; 0 = complete inline */
static uint32_t stixflash_delay_us(const STIXFLASH *stixflash, uint32_t cmd)
{
    if (!stixflash->latency) {
        return 0;
    }
    switch (cmd) {
    case COMMAND_PAGE_READ:
        return stixflash->tread_us;
    case COMMAND_PAGE_WRITE:
        return stixflash->tprog_us;
    case COMMAND_BLOCK_ERASE:
        return stixflash->tbers_us;
    default:
        return 0;
    }
}

/*
 * A write out of the RAMAD -> PAGEAD -> cmdSta order. With no op
 * pending it fails at once (FAILED | READY, IRQ), as before. While an
 * op is in flight (D-F4, review R2) it only restarts the sequence:
 * FAILED/READY and an IRQ would land on the op in flight, and the FSW's
 * ISR (Flash.c:132-174) would credit them to it.
 */
static void stixflash_sequence_error(STIXFLASH *stixflash)
{
    if (stixflash->op.pending) {
        stixflash->write_counter = 0;
        trace_stixflash_error("sequence", stixflash->cmd_sta,
                              stixflash->page_address);
        return;
    }
    flash_fail(stixflash, "sequence");
}

/*
 * D-F1 issue: the cmdSta write with write_counter 2. It latches the op
 * record and zeroes write_counter (the counter belongs to the issue).
 * RESET stays synchronous and raises no IRQ (D-F4: Flash_reset polls
 * BUSY with no time base, Flash.c:214-221); an unknown command fails at
 * once. A program samples guest RAM here (D-F2).
 */
static void stixflash_issue(STIXFLASH *stixflash, uint32_t value)
{
    StixFlashOp *op = &stixflash->op;
    uint32_t cmd = (value >> FTNANDCTRL_CMDSTA_COMMAND) &
                   FTNANDCTRL_CMDSTA_COMMAND_MASK;
    uint32_t delay_us;

    stixflash->write_counter = 0;

    /*
     * D-F4 (review R2): the busy decision comes before any register
     * change. A RESET aborts the op in flight; any other command is
     * dropped and leaves the pending op's status alone.
     */
    if (op->pending) {
        if (cmd != COMMAND_RESET) {
            trace_stixflash_busy_reject(cmd);
            return;
        }
        stixflash_abort(stixflash);
    }

    stixflash->cmd_sta = value;
    /* WRPROT (bit 31) is 1 = not write protected, at all times (D-F5) */
    stixflash->cmd_sta |= (1u << ERROR_CODE_WRPROT);
    stixflash->command = (value >> FTNANDCTRL_CMDSTA_COMMAND) &
                         FTNANDCTRL_CMDSTA_COMMAND_MASK;
    stixflash->mcm = (value >> FTNANDCTRL_CMDSTA_MCMADDRESS) &
                     FTNANDCTRL_CMDSTA_MCMADDRESS_MASK;
    stixflash->memory_bit_type = (value >> FTNANDCTRL_CMDSTA_FLASHTYPE) &
                                 FTNANDCTRL_CMDSTA_FLASHTYPE_MASK;

    switch (stixflash->command) {
    case COMMAND_RESET:
        stixflash->cmd_sta &= ~(1u << FTNANDCTRL_CMDSTA_BUSY);
        stixflash->cmd_sta |= (1u << ERROR_CODE_READY);
        stixflash->cmd_sta &= ~(1u << ERROR_CODE_FAILED);
        return;

    case COMMAND_BLOCK_ERASE:
    case COMMAND_PAGE_READ:
    case COMMAND_PAGE_WRITE:
        break;

    default:
        /*
         * unknown command, incl. COMMAND_FSM_ACCESS (4), which is
         * defined in the FSW driver (Flash.c:60) but never issued by it
         */
        flash_fail(stixflash, "command");
        return;
    }

    op->pending = true;
    op->cmd = stixflash->command;
    op->mcm = stixflash->mcm;
    op->chip = stixflash->chip;
    op->page = stixflash->page;
    op->page_address = stixflash->page_address;
    op->ram_address = stixflash->ram_address;
    op->sample_failed = false;

    /* in flight: BUSY set, READY and FAILED clear (D-F5) */
    stixflash->cmd_sta |= (1u << FTNANDCTRL_CMDSTA_BUSY);
    stixflash->cmd_sta &= ~(1u << ERROR_CODE_READY);
    stixflash->cmd_sta &= ~(1u << ERROR_CODE_FAILED);

    delay_us = stixflash_delay_us(stixflash, op->cmd);
    trace_stixflash_op_start(op->cmd, op->mcm, op->chip, op->page, delay_us);

    switch (op->cmd) {
    case COMMAND_PAGE_READ:
        stixflash->ops_read++;
        break;
    case COMMAND_PAGE_WRITE:
        /*
         * D-F2: the page leaves guest RAM at issue, inside this MMIO
         * write, where memory.c holds the re-entrancy guard (D-F3)
         */
        stixflash->ops_prog++;
        op->sample_failed =
            address_space_read(&address_space_memory, op->ram_address,
                               MEMTXATTRS_UNSPECIFIED, stixflash->page_data,
                               FLASH_PAGE_SIZE) != MEMTX_OK;
        break;
    default:
        stixflash->ops_erase++;
        break;
    }

    if (delay_us == 0) {
        stixflash_complete(stixflash);
    } else {
        timer_mod(stixflash->timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)delay_us * SCALE_US);
    }
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
    case FLASH_CMD_STA_ADDRESS: /* the third write issues the command */
        if (stixflash->write_counter != 2)
        {
            stixflash_sequence_error(stixflash);
            return;
        }
        stixflash_issue(stixflash, (uint32_t)value);
        return;

    case FLASH_PAGE_ADDRESS: // configure the flash addressing
        if (stixflash->write_counter != 1)
        {
            stixflash_sequence_error(stixflash);
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
            stixflash_sequence_error(stixflash);
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

static bool init_and_load_flash_file(STIXFLASH *stixflash, Error **errp)
{
    trace_stixflash_page_size(FLASH_PAGE_SIZE);
    trace_stixflash_file(stixflash->flash_file);

    stixflash->f = fopen(stixflash->flash_file, "r+b");
    if (stixflash->f == NULL)
    {
        info_report("stixflash: flash file %s missing, creating a new one",
                    stixflash->flash_file);

        stixflash->f = fopen(stixflash->flash_file, "w+b");
        if (stixflash->f == NULL)
        {
            error_setg_errno(errp, errno,
                             "stixflash: cannot create flash file %s",
                             stixflash->flash_file);
            return false;
        }

        if (stixflash->inverted_storage) {
            /* D-I: erased flash is a file hole. Create the backing file
             * sparse and instantly instead of writing 16.5 GB of 0xFF;
             * holes read 0x00, which inverted reads turn into guest
             * 0xFF. */
            if (ftruncate(fileno(stixflash->f),
                          (off_t)FLASH_TOTAL_STORAGE) != 0) {
                int err = errno;

                /* the file was created by this very call: do not leave an
                 * empty backing file behind for the next run to open */
                fclose(stixflash->f);
                stixflash->f = NULL;
                unlink(stixflash->flash_file);
                error_setg_errno(errp, err,
                                 "stixflash: cannot size %s to %llu bytes",
                                 stixflash->flash_file,
                                 (unsigned long long)FLASH_TOTAL_STORAGE);
                return false;
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
            warn_report("stixflash: problem closing %s",
                        stixflash->flash_file);
        }

        stixflash->f = fopen(stixflash->flash_file, "r+b");
        if (stixflash->f == NULL)
        {
            error_setg_errno(errp, errno,
                             "stixflash: cannot reopen flash file %s",
                             stixflash->flash_file);
            return false;
        }
    }

    fseek(stixflash->f, 0L, SEEK_END);
    trace_stixflash_file_size(ftell(stixflash->f));
    fseek(stixflash->f, 0L, SEEK_SET);

    return true;
}

/* D-F6: validate one latency property at realize */
static bool stixflash_check_latency(const char *name, uint32_t us,
                                    Error **errp)
{
    if (us > STIXFLASH_LATENCY_MAX_US) {
        error_setg(errp, "stixflash: %s=%" PRIu32 " exceeds %u us; the FSW "
                   "times flash ops out after 50 ms", name, us,
                   STIXFLASH_LATENCY_MAX_US);
        return false;
    }
    if (us != 0 && us < STIXFLASH_LATENCY_WARN_US) {
        warn_report("stixflash: %s=%" PRIu32 " us is shorter than the FSW's "
                    "issue-to-wait path; ops may not block the caller",
                    name, us);
    }
    return true;
}

static void grlib_stixflash_realize(DeviceState *dev, Error **errp)
{
    STIXFLASH *stixflash = GRLIB_STIXFLASH(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (!stixflash_check_latency("tread-us", stixflash->tread_us, errp) ||
        !stixflash_check_latency("tprog-us", stixflash->tprog_us, errp) ||
        !stixflash_check_latency("tbers-us", stixflash->tbers_us, errp)) {
        return;
    }

    sysbus_init_irq(sbd, &stixflash->irq);

    memory_region_init_io(&stixflash->iomem, OBJECT(stixflash), &grlib_stixflash_ops, stixflash, "stixflash", STIXFLASH_REG_SIZE);

    sysbus_init_mmio(sbd, &stixflash->iomem);

    if (!stixflash->flash_file) {
        stixflash->flash_file = g_strdup("flash.bin");
    }
    if (!init_and_load_flash_file(stixflash, errp)) {
        return;
    }

    /*
     * D-F4: the op state and its timer are set up last, after the
     * backing file opened (realize can fail above); no op is pending yet
     */
    stixflash->op.pending = false;
    stixflash->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, stixflash_timer_cb,
                                    stixflash);
}

static void grlib_stixflash_unrealize(DeviceState *dev)
{
    STIXFLASH *stixflash = GRLIB_STIXFLASH(dev);

    if (stixflash->timer) {
        timer_free(stixflash->timer);
        stixflash->timer = NULL;
    }
    stixflash->op.pending = false;
    if (stixflash->f) {
        fclose(stixflash->f);
        stixflash->f = NULL;
    }
}

/*
 * D-F4: drop any pending op and return the command sequence to the
 * power-on state of the zeroed instance
 */
static void grlib_stixflash_reset(DeviceState *d)
{
    STIXFLASH *stixflash = GRLIB_STIXFLASH(d);

    if (stixflash->timer) {
        timer_del(stixflash->timer);
    }
    stixflash->op.pending = false;
    stixflash->write_counter = 0;
    stixflash->cmd_sta = 0;
}


static const Property grlib_stixflash_properties[] = {
    DEFINE_PROP_STRING("flash-file", STIXFLASH, flash_file),
    DEFINE_PROP_BOOL("inverted-storage", STIXFLASH, inverted_storage, true),
    /* D-F6/D-F7: off until F-3 flips the default */
    DEFINE_PROP_BOOL("latency", STIXFLASH, latency, false),
    /* [A] datasheet-typical K9F8G08X0M timings incl. the page transfer */
    DEFINE_PROP_UINT32("tread-us", STIXFLASH, tread_us, 130),
    DEFINE_PROP_UINT32("tprog-us", STIXFLASH, tprog_us, 300),
    DEFINE_PROP_UINT32("tbers-us", STIXFLASH, tbers_us, 1500),
};

/*
 * Review SHOULD-1: no vmstate yet, so the device blocks migration and
 * savevm instead of silently dropping a pending op. A real vmsd belongs
 * to platform roadmap item R-r3 (savevm/loadvm) and needs: cmd_sta,
 * write_counter, ram_address, page_address, chip, page, command, mcm,
 * memory_bit_type, the StixFlashOp record (pending, cmd, mcm, chip,
 * page, page_address, ram_address, sample_failed), page_data (a
 * program's issue-time sample) and the timer (VMSTATE_TIMER_PTR).
 */
static const VMStateDescription vmstate_grlib_stixflash = {
    .name = "grlib-stixflash",
    .unmigratable = 1,
};

/* D-F9: read-only op counters */
static void grlib_stixflash_instance_init(Object *obj)
{
    STIXFLASH *stixflash = GRLIB_STIXFLASH(obj);

    object_property_add_uint64_ptr(obj, "ops-read", &stixflash->ops_read,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "ops-prog", &stixflash->ops_prog,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "ops-erase", &stixflash->ops_erase,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "ops-abort", &stixflash->ops_abort,
                                   OBJ_PROP_FLAG_READ);
}

static void grlib_stixflash_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = grlib_stixflash_realize;
    dc->unrealize = grlib_stixflash_unrealize;
    dc->vmsd = &vmstate_grlib_stixflash;
    device_class_set_legacy_reset(dc, grlib_stixflash_reset);
    device_class_set_props(dc, grlib_stixflash_properties);
}

static const TypeInfo grlib_stixflash_info = {
    .name = TYPE_GRLIB_STIXFLASH,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STIXFLASH),
    .instance_init = grlib_stixflash_instance_init,
    .class_init = grlib_stixflash_class_init,
};

static void grlib_stixflash_register_types(void)
{
    type_register_static(&grlib_stixflash_info);
}

type_init(grlib_stixflash_register_types)