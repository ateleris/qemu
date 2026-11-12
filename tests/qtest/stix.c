/*
 * STIX IDPU emulation regression tests (qtest).
 *
 * Pins the APB PnP table exactly as the old (v5.2, repaired) model returns
 * it. The FSW's drvmgr discovers devices through these words; all three
 * STIX devices were (deliberately, like-for-like) registered as
 * GAISLER/FTMCTRL version 0 — 0x054 in the device field.
 *
 * The PnP area at 0x800FF000 holds one 32-bit word per APB device entry:
 * config word (version [31:24], device [23:12], irq+flags [11:0]) followed
 * by an address word (base [31:20], mask, area). Values pinned from the
 * old build (tip 93a9824b2d + repairs); see
 * docs/stix-port/baseline/README.md.
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "libqtest.h"
#include "qapi/error.h"
#include "qobject/qbool.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "qobject/qnum.h"
#include "qobject/qstring.h"

#define APB_PNP_BASE 0x800FF000
#define PNP_ENTRY_COUNT 12   /* words 0x00..0x0b; 0x0c.. must be zero */

/* Pinned from old build 93a9824b2d + repairs 23fb928739/0df394b42b.
 * Decoded: (0,1) APBUART, (2,3) IRQMP, (4,5) APBUART#2, (6,7) CTRL/FTMCTRL,
 * (8,9) FLASH/FTMCTRL, (10,11) SPW/FTMCTRL. */
static const uint32_t pnp_golden[PNP_ENTRY_COUNT] = {
    0x0100d040, 0x0020fff1, /* APBUART */
    0x0101100a, 0x0030fff1, /* IRQMP */
    0x0100c022, 0x0010fff1, /* APBUART #2 */
    0x01054003, 0x0050fff1, /* STIX CTRL as FTMCTRL, irq 3 */
    0x01054001, 0x0080fff1, /* STIX FLASH as FTMCTRL, irq 1 */
    0x0105400d, 0x00a0fff1, /* STIX SPW as FTMCTRL, irq 13 */
};

static inline uint32_t stix_pnp_read(QTestState *qts, unsigned index)
{
    return qtest_readl(qts, APB_PNP_BASE + 4u * index);
}

/* Shared assertion helper: pin one PnP entry word (also used by later
 * device tests). */
static void stix_pnp_check(QTestState *qts, unsigned index, uint32_t expected)
{
    uint32_t actual = stix_pnp_read(qts, index);
    g_test_message("pnp[%02u]: got 0x%08x want 0x%08x",
                   index, actual, expected);
    g_assert_cmphex(actual, ==, expected);
}

/* ---- flash device (grlib_stixflash) ---- */

#define FLASH_BASE    0x80000800u
#define FLASH_CMDSTA  (FLASH_BASE + 0u)   /* status/command (read asserted 0) */
#define FLASH_PAGEAD  (FLASH_BASE + 4u)
#define FLASH_RAMAD   (FLASH_BASE + 8u)

#define IRQMP_BASE    0x80000200u
#define IRQMP_PENDING (IRQMP_BASE + 0x04u)
#define IRQMP_CLEAR   (IRQMP_BASE + 0x0cu)
#define FLASH_IRQ_BIT (1u << 1)           /* IRQ_FLASH = 1 */

#define FTNAND_BUSY      (1u << 4)
#define FTNAND_FLASHTYPE (1u << 7)          /* MEMORY_TYPE_BIT in the FSW driver */
#define FTNAND_FAILED    (1u << 24)         /* ERROR_CODE_FAILED */
#define FTNAND_READY     (1u << 30)         /* ERROR_CODE_READY */

#define FLASH_PAGE_BYTES 4224u              /* 4096 data + 128 header */

#define CMD_RESET       0u
#define CMD_BLOCK_ERASE 1u
#define CMD_PAGE_READ   2u
#define CMD_PAGE_WRITE  3u

/*
 * Flash latency plan D-F6 defaults (tread-us/tprog-us/tbers-us) and the
 * switch the latency qtests pass explicitly (on by default since F-3; the
 * explicit switch keeps them testing latency if the default changes).
 */
#define FLASH_TREAD_US  130
#define FLASH_TPROG_US  300
#define FLASH_TBERS_US  1500
#define FLASH_LATENCY_ON "-global grlib-stixflash.latency=on"

static void flash_step_us(QTestState *qts, int64_t us)
{
    qtest_clock_step(qts, us * 1000);
}

/*
 * F-2: let a command complete under either latency default: step the
 * virtual clock past the longest op (block erase) plus 1 us. With
 * latency off every op has already completed inline and this only moves
 * the clock.
 */
static void flash_wait(QTestState *qts)
{
    flash_step_us(qts, FLASH_TBERS_US + 1);
}

/* Driver register order: ram address, page address, cmd/sta. The device
 * asserts this exact write_counter sequence (0 -> 1 -> 2 -> execute). */
static void flash_issue_nowait(QTestState *qts, uint32_t ram, uint32_t page,
                               uint32_t cmd)
{
    qtest_writel(qts, FLASH_RAMAD, ram);
    qtest_writel(qts, FLASH_PAGEAD, page);
    qtest_writel(qts, FLASH_CMDSTA, FTNAND_FLASHTYPE | cmd);
}

/* issue and wait for the completion (flash_wait) */
static void flash_issue(QTestState *qts, uint32_t ram, uint32_t page,
                        uint32_t cmd)
{
    flash_issue_nowait(qts, ram, page, cmd);
    flash_wait(qts);
}

/* Common qtest setup: pass -global grlib-stixflash.flash-file=<small temp
 * file> so the flash device never creates a 16.5 GB flash.bin in the test
 * process CWD (the `make check` footgun: meson runs tests from the build
 * root). The v5.2 baseline binary has no such property; the wrapper script
 * strips the argument for old-build validation runs. */
static char *stix_tmp_flash_path;

static QTestState *stix_qtest_new_extra(const char *extra)
{
    QTestState *qts;
    gchar *path = NULL;
    int fd = g_file_open_tmp("stix-flash-XXXXXX", &path, NULL);
    g_assert(fd >= 0);
    /* D-I: the default is inverted storage (file byte 0x00 = guest 0xFF),
     * so the erased 1 MB backing file is 1 MB of holes, created instantly
     * with ftruncate. Guest-visible expectations do not change: the guest
     * still reads 0xFF everywhere. */
    g_assert(ftruncate(fd, 1024 * 1024) == 0);
    close(fd);
    stix_tmp_flash_path = path;
    qts = qtest_initf("-M leon3_generic "
                      "-global grlib-stixflash.flash-file=%s %s", path, extra);
    return qts;
}

static QTestState *stix_qtest_new(void)
{
    return stix_qtest_new_extra("");
}

/* P2-1: qtest starts no TM server (grlib-stixspw tmtc-server=auto is off
 * under qtest), so tests that never attach a TM client pass no port at
 * all. Tests hold ports with stix_hold_tcp_port() (a specific port) or
 * stix_hold_free_tcp_port() (an ephemeral one, returned already held, so
 * no other process can take it in between). */

/* Hold 127.0.0.1:<port> with bind+listen. SO_REUSEADDR is set like on the
 * device's own bind, so the hold fails only when a real listener owns the
 * port (EADDRINUSE). Returns the socket (close it to release the port) or
 * -1 with errno preserved for the caller. */
static int stix_hold_tcp_port(int port)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons(port),
    };
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int optval = 1;

    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 1) != 0) {
        int err = errno;

        close(fd);
        errno = err;
        return -1;
    }
    return fd;
}

/* Race-free hold: bind 127.0.0.1:0, listen, and hand back BOTH the fd and
 * the kernel-assigned port — the port is never released before the caller
 * is done with it. */
static int stix_hold_free_tcp_port(int *port)
{
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    int fd = stix_hold_tcp_port(0);

    if (fd < 0) {
        return -1;
    }
    g_assert(getsockname(fd, (struct sockaddr *)&addr, &addrlen) == 0);
    *port = ntohs(addr.sin_port);
    g_assert_cmpint(*port, >, 0);
    return fd;
}

/*
 * Gate-ports plan D-G4: when the suite runs in a slot (STIX_QTEST_PORT_BASE
 * = the slot's base port B, exported by scripts/stix/lib/slot.sh; the
 * scripts' STIX_PORT_BASE stays the slot selector), the ports QEMU must
 * bind itself come from the slot's suite band B+3..B+99. The slot lock
 * makes the band exclusive, so the release-then-bind window below cannot
 * race another suite or probe. Returns B, or 0 outside a slot.
 */
static int stix_slot_band_base(void)
{
    const char *env = getenv("STIX_QTEST_PORT_BASE");
    long base;

    if (!env || !*env) {
        return 0;
    }
    /* set but unusable: fail loudly, never fall back to ephemeral ports */
    if (qemu_strtol(env, NULL, 10, &base) != 0 ||
        base < 1024 || base + 99 > 65535) {
        g_error("STIX_QTEST_PORT_BASE='%s': not a port base B with "
                "1024 <= B and B+99 <= 65535", env);
    }
    return (int)base;
}

#define STIX_SLOT_BAND_FIRST 3
#define STIX_SLOT_BAND_PORTS 97 /* B+3 .. B+99 */

/*
 * For tests where QEMU itself must bind the port (tmtc-port=<free>):
 * outside a slot, the release-then-bind window is unavoidable there (P2-1
 * review); in a slot, the next bindable port of the band, round-robin.
 */
static int stix_free_tcp_port(void)
{
    static unsigned next;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    socklen_t addrlen = sizeof(addr);
    int base = stix_slot_band_base();
    int fd, port, i;

    for (i = 0; base && i < STIX_SLOT_BAND_PORTS; i++) {
        port = base + STIX_SLOT_BAND_FIRST + next++ % STIX_SLOT_BAND_PORTS;
        fd = stix_hold_tcp_port(port);
        if (fd >= 0) {
            close(fd);
            return port;
        }
    }
    g_assert(!base); /* the whole band busy: a stray process in the slot */

    fd = socket(AF_INET, SOCK_STREAM, 0);
    g_assert(fd >= 0);
    g_assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    g_assert(getsockname(fd, (struct sockaddr *)&addr, &addrlen) == 0);
    port = ntohs(addr.sin_port);
    close(fd);
    g_assert_cmpint(port, >, 0);
    return port;
}

static void stix_qtest_cleanup(void)
{
    if (stix_tmp_flash_path) {
        g_unlink(stix_tmp_flash_path);
        g_clear_pointer(&stix_tmp_flash_path, g_free);
    }
}

static void test_flash(void)
{
    QTestState *qts;
    uint32_t sta, pending;

    qts = stix_qtest_new();

    /* RESET: completes immediately, BUSY clear (device never sets BUSY). */
    flash_issue(qts, 0x40000000u, 0, CMD_RESET);
    sta = qtest_readl(qts, FLASH_CMDSTA);
    g_assert_cmpuint(sta & FTNAND_BUSY, ==, 0);

    /* PAGE_READ page 0 chip 0: page content lands at the RAM address and
     * IRQ 1 pulses. flash.bin is all-0xFF, so RAM reads back 0xffffffff. */
    qtest_writel(qts, IRQMP_CLEAR, FLASH_IRQ_BIT);
    flash_issue(qts, 0x40000000u, 0, CMD_PAGE_READ);
    pending = qtest_readl(qts, IRQMP_PENDING);
    g_assert_cmpuint(pending & FLASH_IRQ_BIT, ==, FLASH_IRQ_BIT);
    g_assert_cmphex(qtest_readl(qts, 0x40000000u), ==, 0xffffffffu);
    sta = qtest_readl(qts, FLASH_CMDSTA);
    g_assert_cmpuint(sta & FTNAND_BUSY, ==, 0);

    /* BLOCK_ERASE block 0: IRQ 1 pulses again; content stays 0xFF. */
    qtest_writel(qts, IRQMP_CLEAR, FLASH_IRQ_BIT);
    flash_issue(qts, 0x40000000u, 0, CMD_BLOCK_ERASE);
    pending = qtest_readl(qts, IRQMP_PENDING);
    g_assert_cmpuint(pending & FLASH_IRQ_BIT, ==, FLASH_IRQ_BIT);

    /* PAGE_READ after erase: RAM still 0xFF. */
    qtest_writel(qts, IRQMP_CLEAR, FLASH_IRQ_BIT);
    flash_issue(qts, 0x40000000u, 0, CMD_PAGE_READ);
    g_assert_cmphex(qtest_readl(qts, 0x40000000u), ==, 0xffffffffu);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* Error contract (B1): a guest-commanded failure must end in
 * STATUS.FAILED | STATUS.READY with one IRQ 1 pulse — the FSW's ISR
 * (Flash.c:138-148) waits for the IRQ, then requires BUSY==0 && READY==1
 * before it reads FAILED — and never in a QEMU abort (the baseline had 14
 * guest-reachable asserts in this device). Sequence violations, unknown
 * commands and unknown-register writes all go through the same
 * three-register window (ram -> page -> cmd) the FSW driver uses.
 *
 * The backing file is the 1 MB all-0xFF temp file, so page 300 (offset
 * 1,267,200) and the 64-page block at page 320 are past EOF: decision D-H
 * makes the file size the flash capacity (read -> 0xFF page, erase ->
 * skipped, write -> FAILED, file never extended). */
static void test_flash_error_paths(void)
{
    g_autofree uint8_t *zeros = g_malloc0(FLASH_PAGE_BYTES);
    QTestState *qts;
    uint32_t sta, pending;
    struct stat st;
    const uint32_t failed_ready = FTNAND_FAILED | FTNAND_READY;

    qts = stix_qtest_new();

    /* 1. Out-of-order PAGEAD write (write_counter == 0; driver order is
     *    ram -> page -> cmd): sequence violation. FAILED|READY set, BUSY
     *    clear, IRQ 1 pending, QEMU still alive; the next correct
     *    three-write sequence clears FAILED again. */
    qtest_writel(qts, IRQMP_CLEAR, FLASH_IRQ_BIT);
    qtest_writel(qts, FLASH_PAGEAD, 0xdeadbeefu);
    sta = qtest_readl(qts, FLASH_CMDSTA);
    g_assert_cmpuint(sta & failed_ready, ==, failed_ready);
    g_assert_cmpuint(sta & FTNAND_BUSY, ==, 0);
    pending = qtest_readl(qts, IRQMP_PENDING);
    g_assert_cmpuint(pending & FLASH_IRQ_BIT, ==, FLASH_IRQ_BIT);
    /* QEMU alive: this read round-trips (an aborted device tears down the
     * qtest connection and fails the test). */
    g_assert_cmphex(qtest_readl(qts, FLASH_CMDSTA), ==, sta);
    flash_issue(qts, 0x40000000u, 0, CMD_RESET);
    sta = qtest_readl(qts, FLASH_CMDSTA);
    g_assert_cmpuint(sta & FTNAND_FAILED, ==, 0);

    /* 2. Undefined command 5, then command 4 = COMMAND_FSM_ACCESS
     *    (defined in the FSW driver, never issued by it): FAILED + IRQ 1
     *    each time. */
    qtest_writel(qts, IRQMP_CLEAR, FLASH_IRQ_BIT);
    flash_issue(qts, 0x40000000u, 0, 5u);
    sta = qtest_readl(qts, FLASH_CMDSTA);
    g_assert_cmpuint(sta & FTNAND_FAILED, ==, FTNAND_FAILED);
    pending = qtest_readl(qts, IRQMP_PENDING);
    g_assert_cmpuint(pending & FLASH_IRQ_BIT, ==, FLASH_IRQ_BIT);

    qtest_writel(qts, IRQMP_CLEAR, FLASH_IRQ_BIT);
    flash_issue(qts, 0x40000000u, 0, 4u);
    sta = qtest_readl(qts, FLASH_CMDSTA);
    g_assert_cmpuint(sta & FTNAND_FAILED, ==, FTNAND_FAILED);
    pending = qtest_readl(qts, IRQMP_PENDING);
    g_assert_cmpuint(pending & FLASH_IRQ_BIT, ==, FLASH_IRQ_BIT);

    /* 3. Register reads: PAGEAD returns the last written page address. A
     *    write past the register file (offset 0x0C) is ignored: no state
     *    change, device stays alive (the reads/writes below prove it). */
    flash_issue(qts, 0x40001000u, 0x123u, CMD_RESET);
    g_assert_cmpuint(qtest_readl(qts, FLASH_PAGEAD), ==, 0x123u);
    sta = qtest_readl(qts, FLASH_CMDSTA);
    qtest_writel(qts, FLASH_BASE + 0x0cu, 0xa5a5a5a5u);
    g_assert_cmphex(qtest_readl(qts, FLASH_CMDSTA), ==, sta);

    /* 4. D-H: PAGE_READ past EOF fills the page with 0xFF and succeeds.
     *    Guest RAM starts zeroed so the write-back is proven. */
    qtest_memwrite(qts, 0x40000000u, zeros, FLASH_PAGE_BYTES);
    qtest_writel(qts, IRQMP_CLEAR, FLASH_IRQ_BIT);
    flash_issue(qts, 0x40000000u, 300, CMD_PAGE_READ);
    g_assert_cmphex(qtest_readl(qts, 0x40000000u), ==, 0xffffffffu);
    sta = qtest_readl(qts, FLASH_CMDSTA);
    g_assert_cmpuint(sta & FTNAND_FAILED, ==, 0);
    pending = qtest_readl(qts, IRQMP_PENDING);
    g_assert_cmpuint(pending & FLASH_IRQ_BIT, ==, FLASH_IRQ_BIT);

    /* 5. D-H: BLOCK_ERASE of a block entirely past EOF succeeds without
     *    touching the backing file. */
    qtest_writel(qts, IRQMP_CLEAR, FLASH_IRQ_BIT);
    flash_issue(qts, 0x40000000u, 320, CMD_BLOCK_ERASE);
    sta = qtest_readl(qts, FLASH_CMDSTA);
    g_assert_cmpuint(sta & FTNAND_FAILED, ==, 0);
    g_assert_cmpint(stat(stix_tmp_flash_path, &st), ==, 0);
    g_assert_cmpuint(st.st_size, ==, 1024u * 1024u);

    /* 6. D-H: PAGE_WRITE past EOF fails with FAILED + IRQ 1; the file is
     *    not extended (a hole reads 0x00 in plain mode = bad-block
     *    marker; with the default inversion it reads 0xFF = erased).
     */
    qtest_writel(qts, IRQMP_CLEAR, FLASH_IRQ_BIT);
    flash_issue(qts, 0x40000000u, 300, CMD_PAGE_WRITE);
    sta = qtest_readl(qts, FLASH_CMDSTA);
    g_assert_cmpuint(sta & FTNAND_FAILED, ==, FTNAND_FAILED);
    pending = qtest_readl(qts, IRQMP_PENDING);
    g_assert_cmpuint(pending & FLASH_IRQ_BIT, ==, FLASH_IRQ_BIT);
    g_assert_cmpint(stat(stix_tmp_flash_path, &st), ==, 0);
    g_assert_cmpuint(st.st_size, ==, 1024u * 1024u);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * F-1 (flash latency plan D-F4): the device reset handler drops any
 * pending op and returns the sequence state to its power-on value. A
 * system reset after RAMAD and PAGEAD (write_counter 2) must leave
 * write_counter 0 and cmdSta 0, so a fresh three-write sequence runs
 * instead of failing as a "sequence" violation. RED before F-1: the
 * reset handler was empty, cmdSta kept the last op's status and the
 * RAMAD write after the reset failed the sequence.
 */
static void test_flash_reset_state(void)
{
    QTestState *qts;
    uint32_t sta;

    qts = stix_qtest_new();

    /* a completed read leaves a non-zero cmdSta (READY | WRPROT | ...) */
    flash_issue(qts, 0x40000000u, 0, CMD_PAGE_READ);
    g_assert_cmphex(qtest_readl(qts, FLASH_CMDSTA), !=, 0);

    /* mid-sequence: RAMAD, PAGEAD (write_counter 2), then reset */
    qtest_writel(qts, FLASH_RAMAD, 0x40000000u);
    qtest_writel(qts, FLASH_PAGEAD, 0);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, FLASH_CMDSTA), ==, 0);

    /* a fresh three-write sequence succeeds: FAILED clear, IRQ 1 */
    qtest_writel(qts, IRQMP_CLEAR, FLASH_IRQ_BIT);
    qtest_writel(qts, 0x40000000u, 0);
    flash_issue(qts, 0x40000000u, 0, CMD_PAGE_READ);
    sta = qtest_readl(qts, FLASH_CMDSTA);
    g_assert_cmpuint(sta & FTNAND_FAILED, ==, 0);
    g_assert_cmpuint(sta & FTNAND_READY, ==, FTNAND_READY);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & FLASH_IRQ_BIT, ==,
                     FLASH_IRQ_BIT);
    g_assert_cmphex(qtest_readl(qts, 0x40000000u), ==, 0xffffffffu);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- B2: flash addressing helpers ---- */

/* Full backing-file size: 2 MCMs x 8 chips x 4096 blocks x 64 pages x
 * 4224 B = 17,716,740,096. Created sparse with ftruncate; sparse holes
 * read 0x00, so host-side checks compare against written patterns only —
 * 0xFF is expected only where the device erased. On a filesystem that
 * does not do sparse files the test skips instead of eating 16.5 GB. */
#define FLASH_TOTAL_STORAGE 17716740096ULL

static QTestState *flash_qtest_new_sparse(uint64_t size, gchar **path)
{
    gchar *tmp = NULL;
    struct stat st;
    QTestState *qts;
    int fd = g_file_open_tmp("stix-flash-sparse-XXXXXX", &tmp, NULL);

    g_assert(fd >= 0);
    g_assert(ftruncate(fd, (off_t)size) == 0);
    g_assert(fstat(fd, &st) == 0);
    close(fd);

    if ((uint64_t)st.st_blocks * 512 > 1024u * 1024u) {
        g_unlink(tmp);
        g_free(tmp);
        *path = NULL;
        g_test_skip("filesystem does not create sparse files");
        return NULL;
    }

    qts = qtest_initf("-M leon3_generic -global grlib-stixflash.flash-file=%s",
                      tmp);
    *path = tmp;
    return qts;
}

/* FSW-style command issue (START_FLASH_OPERATION, Flash.c:81-84, with the
 * linear address split by SPLIT_ADDRESS, Flash.c:70-73):
 * pageAddress = chip<<20 | page, cmdSta = FLASHTYPE | mcm<<3 | cmd. */
static void flash_issue_mcm(QTestState *qts, uint32_t ram, uint32_t chip,
                            uint32_t page, uint32_t cmd, uint32_t mcm)
{
    qtest_writel(qts, FLASH_RAMAD, ram);
    qtest_writel(qts, FLASH_PAGEAD, (chip << 20) | (page & 0x3ffffu));
    qtest_writel(qts, FLASH_CMDSTA, FTNAND_FLASHTYPE | (mcm << 3) | cmd);
    flash_wait(qts);
}

/* Host-side check: pull one page at offset out of the backing file,
 * while QEMU is still running. */
static void flash_pread_page(const gchar *path, uint64_t offset, void *buf)
{
    int fd = open(path, O_RDONLY);
    ssize_t ret;

    g_assert(fd >= 0);
    ret = pread(fd, buf, FLASH_PAGE_BYTES, (off_t)offset);
    g_assert_cmpint((int)ret, ==, (int)FLASH_PAGE_BYTES);
    close(fd);
}

/* D-I: with the default inverted storage the file stores every byte
 * XOR 0xFF, so a host-side pread must be inverted before it can be
 * compared against the guest pattern (in place is fine). */
static void flash_invert(uint8_t *buf, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        buf[i] ^= 0xFFu;
    }
}

/* B2: addressing. The baseline computed the file offset as the raw
 * pageAddress register * 4224: chip c landed at c*2^20 pages*4224 (the
 * real stride is 2^18 pages), MCM was ignored so FLASH B's control
 * region aliased exactly onto FLASH A's (MCM1 block 64 == MCM0 block
 * 64), and the 16.5 GB file truncated to 512 MiB through uint32 ftell.
 * Chip 0/MCM 0 offsets are page*4224 before and after. The full-size
 * sparse backing file holds erased flash (D-I: holes read 0x00, which
 * inverted storage turns into guest 0xFF), so host-side preads are
 * inverted before comparing; an erased page is host 0x00. */
static void test_flash_addressing(void)
{
    g_autofree uint8_t *pa = g_malloc(FLASH_PAGE_BYTES);
    g_autofree uint8_t *pb = g_malloc(FLASH_PAGE_BYTES);
    g_autofree uint8_t *pc = g_malloc(FLASH_PAGE_BYTES);
    g_autofree uint8_t *pd = g_malloc(FLASH_PAGE_BYTES);
    g_autofree uint8_t *z0 = g_malloc0(FLASH_PAGE_BYTES);
    g_autofree uint8_t *host = g_malloc(FLASH_PAGE_BYTES);
    QTestState *qts;
    gchar *path = NULL;
    uint32_t sta;

    memset(pa, 0xAA, FLASH_PAGE_BYTES);
    memset(pb, 0xBB, FLASH_PAGE_BYTES);
    memset(pc, 0xCC, FLASH_PAGE_BYTES);
    memset(pd, 0xDD, FLASH_PAGE_BYTES);

    qts = flash_qtest_new_sparse(FLASH_TOTAL_STORAGE, &path);
    if (!qts) {
        return; /* skipped: filesystem does not do sparse files */
    }

    /* 1. Chip-0 compatibility guard: chip 0/MCM 0 stays page*4224. The
     *    host pread happens below, before qtest_quit: the page must be
     *    on disk while QEMU is still running, which is exactly what the
     *    fflush after every write guarantees. */
    qtest_memwrite(qts, 0x40000000u, pa, FLASH_PAGE_BYTES);
    flash_issue_mcm(qts, 0x40000000u, 0, 5, CMD_PAGE_WRITE, 0);

    /* 2. Chip stride: chip 1 begins after chip 0's 4096*64 pages, at
     *    file offset 1,107,296,256 = 262144*4224. */
    qtest_memwrite(qts, 0x40000000u, pb, FLASH_PAGE_BYTES);
    flash_issue_mcm(qts, 0x40000000u, 1, 0, CMD_PAGE_WRITE, 0);
    flash_pread_page(path, (uint64_t)262144 * FLASH_PAGE_BYTES, host);
    flash_invert(host, FLASH_PAGE_BYTES);
    g_assert_cmpint(memcmp(host, pb, FLASH_PAGE_BYTES), ==, 0);

    /* 3. MCM separation (the FLASH A/B alias). The same page 4096 on
     *    MCM0 chip 0 (offset 17,301,504) and MCM1 chip 0 (offset
     *    8*262144 pages further) must land apart, and the device must
     *    read MCM0's page back as C: guest RAM still holds D, so only a
     *    real fetch from the file can produce C. */
    qtest_memwrite(qts, 0x40000000u, pc, FLASH_PAGE_BYTES);
    flash_issue_mcm(qts, 0x40000000u, 0, 4096, CMD_PAGE_WRITE, 0);
    qtest_memwrite(qts, 0x40000000u, pd, FLASH_PAGE_BYTES);
    flash_issue_mcm(qts, 0x40000000u, 0, 4096, CMD_PAGE_WRITE, 1);
    flash_pread_page(path, (uint64_t)4096 * FLASH_PAGE_BYTES, host);
    flash_invert(host, FLASH_PAGE_BYTES);
    g_assert_cmpint(memcmp(host, pc, FLASH_PAGE_BYTES), ==, 0);
    flash_pread_page(path, ((uint64_t)8 * 262144 + 4096) * FLASH_PAGE_BYTES,
                     host);
    flash_invert(host, FLASH_PAGE_BYTES);
    g_assert_cmpint(memcmp(host, pd, FLASH_PAGE_BYTES), ==, 0);
    flash_issue_mcm(qts, 0x40000000u, 0, 4096, CMD_PAGE_READ, 0);
    qtest_memread(qts, 0x40000000u, host, FLASH_PAGE_BYTES);
    g_assert_cmpint(memcmp(host, pc, FLASH_PAGE_BYTES), ==, 0);

    /* 4. Past the baseline's 512 MiB ftell truncation (guard: green
     *    since B1's 64-bit file size, red on the 5.2 wrapper): chip 0
     *    page 130000 lives at 549,120,000. RAM is refilled with B first
     *    so the read-back proves the device fetched the page. */
    qtest_memwrite(qts, 0x40000000u, pa, FLASH_PAGE_BYTES);
    flash_issue_mcm(qts, 0x40000000u, 0, 130000, CMD_PAGE_WRITE, 0);
    qtest_memwrite(qts, 0x40000000u, pb, FLASH_PAGE_BYTES);
    flash_issue_mcm(qts, 0x40000000u, 0, 130000, CMD_PAGE_READ, 0);
    qtest_memread(qts, 0x40000000u, host, FLASH_PAGE_BYTES);
    g_assert_cmpint(memcmp(host, pa, FLASH_PAGE_BYTES), ==, 0);
    sta = qtest_readl(qts, FLASH_CMDSTA);
    g_assert_cmpuint(sta & FTNAND_FAILED, ==, 0);

    /* step 1's host check: still before qtest_quit (the page must be on
     * disk while QEMU lives - the fflush), and before step 5's erase,
     * which covers block 0 and therefore wipes page 5. The file holds
     * the inverted pattern (D-I). */
    flash_pread_page(path, (uint64_t)5 * FLASH_PAGE_BYTES, host);
    flash_invert(host, FLASH_PAGE_BYTES);
    g_assert_cmpint(memcmp(host, pa, FLASH_PAGE_BYTES), ==, 0);

    /* 5. The erase covers the block CONTAINING the commanded page
     *    (base = page & ~63, 64 pages, never crossing a chip); the FSW
     *    aligns the page itself before issuing, so the device-side rule
     *    is defensive. The erase is commanded with page 10, hence block
     *    0 = pages 0..63: page 63 is erased (a file hole, host 0x00
     *    under D-I), page 64 keeps A (host ~A). */
    qtest_memwrite(qts, 0x40000000u, pa, FLASH_PAGE_BYTES);
    flash_issue_mcm(qts, 0x40000000u, 0, 63, CMD_PAGE_WRITE, 0);
    flash_issue_mcm(qts, 0x40000000u, 0, 64, CMD_PAGE_WRITE, 0);
    flash_issue_mcm(qts, 0x40000000u, 0, 10, CMD_BLOCK_ERASE, 0);
    sta = qtest_readl(qts, FLASH_CMDSTA);
    g_assert_cmpuint(sta & FTNAND_FAILED, ==, 0);
    flash_pread_page(path, (uint64_t)63 * FLASH_PAGE_BYTES, host);
    g_assert_cmpint(memcmp(host, z0, FLASH_PAGE_BYTES), ==, 0);
    flash_pread_page(path, (uint64_t)64 * FLASH_PAGE_BYTES, host);
    flash_invert(host, FLASH_PAGE_BYTES);
    g_assert_cmpint(memcmp(host, pa, FLASH_PAGE_BYTES), ==, 0);

    qtest_quit(qts);
    g_unlink(path);
    g_free(path);
}

/* B3 storage (D-I): the backing file stores every byte XOR 0xFF, so a
 * sparse hole (0x00) is erased flash. A missing file is created
 * instantly as a sparse 17,716,740,096-byte ftruncate; an erase of an
 * inverted block punches a hole (host 0x00). Legacy plain storage
 * (inverted-storage=off) keeps all-0xFF files - such as the frozen
 * baseline copy - byte-identical. */
static void test_flash_storage(void)
{
    /* ---- (a) missing file: created instantly, sparse, erased ---- */
    {
        g_autofree gchar *dir = g_dir_make_tmp("stix-flash-storage-XXXXXX",
                                               NULL);
        g_autofree gchar *path = g_build_filename(dir, "flash.bin", NULL);
        g_autofree uint8_t *p = g_malloc(FLASH_PAGE_BYTES);
        g_autofree uint8_t *z0 = g_malloc0(FLASH_PAGE_BYTES);
        g_autofree uint8_t *pff = g_malloc(FLASH_PAGE_BYTES);
        g_autofree uint8_t *host = g_malloc(FLASH_PAGE_BYTES);
        struct stat st;
        gboolean sparse = TRUE;
        size_t i;
        QTestState *qts;
        uint32_t sta;

        for (i = 0; i < FLASH_PAGE_BYTES; i++) {
            p[i] = (uint8_t)(i ^ 0xA5u); /* position-tagged pattern P */
        }
        memset(pff, 0xFF, FLASH_PAGE_BYTES);

        qts = qtest_initf("-M leon3_generic -global grlib-stixflash.flash-file=%s",
                          path);

        /* creation is one ftruncate: the size is there, the blocks are
         * not. If this filesystem cannot do sparse files, skip rather
         * than hold 16.5 GB (clean up first, also on the skip path). */
        g_assert(stat(path, &st) == 0);
        if ((uint64_t)st.st_blocks * 512 > 1024u * 1024u) {
            sparse = FALSE;
        } else {
            g_assert_cmpuint((uint64_t)st.st_size, ==, FLASH_TOTAL_STORAGE);
        }

        if (sparse) {
            /* guest: the fresh file reads back erased */
            qtest_writel(qts, 0x40000000u, 0);
            qtest_writel(qts, IRQMP_CLEAR, FLASH_IRQ_BIT);
            flash_issue(qts, 0x40000000u, 0, CMD_PAGE_READ);
            g_assert_cmphex(qtest_readl(qts, 0x40000000u), ==, 0xffffffffu);

            /* ---- (b) write / read / erase round trip ---- */
            qtest_memwrite(qts, 0x40000000u, p, FLASH_PAGE_BYTES);
            flash_issue(qts, 0x40000000u, 5, CMD_PAGE_WRITE);

            /* guest read-back (RAM clobbered first so the fetch is
             * proven) */
            qtest_memwrite(qts, 0x40000000u, z0, FLASH_PAGE_BYTES);
            flash_issue(qts, 0x40000000u, 5, CMD_PAGE_READ);
            qtest_memread(qts, 0x40000000u, host, FLASH_PAGE_BYTES);
            g_assert_cmpint(memcmp(host, p, FLASH_PAGE_BYTES), ==, 0);

            /* host: the file holds the inverted pattern */
            flash_pread_page(path, (uint64_t)5 * FLASH_PAGE_BYTES, host);
            flash_invert(host, FLASH_PAGE_BYTES);
            g_assert_cmpint(memcmp(host, p, FLASH_PAGE_BYTES), ==, 0);

            /* erase page 5's block: guest page 5 is 0xFF again, the host
             * range is a hole (0x00) */
            flash_issue(qts, 0x40000000u, 5, CMD_BLOCK_ERASE);
            sta = qtest_readl(qts, FLASH_CMDSTA);
            g_assert_cmpuint(sta & FTNAND_FAILED, ==, 0);
            qtest_memwrite(qts, 0x40000000u, p, FLASH_PAGE_BYTES);
            flash_issue(qts, 0x40000000u, 5, CMD_PAGE_READ);
            qtest_memread(qts, 0x40000000u, host, FLASH_PAGE_BYTES);
            g_assert_cmpint(memcmp(host, pff, FLASH_PAGE_BYTES), ==, 0);
            flash_pread_page(path, (uint64_t)5 * FLASH_PAGE_BYTES, host);
            g_assert_cmpint(memcmp(host, z0, FLASH_PAGE_BYTES), ==, 0);
        }

        qtest_quit(qts);
        g_unlink(path);
        g_rmdir(dir);

        if (!sparse) {
            g_test_skip("filesystem does not create sparse files");
            return;
        }
    }

    /* ---- (c) legacy plain storage keeps all-0xFF files usable ---- */
    {
        g_autofree gchar *path = NULL;
        g_autofree uint8_t *p = g_malloc(FLASH_PAGE_BYTES);
        g_autofree uint8_t *pff = g_malloc(FLASH_PAGE_BYTES);
        g_autofree uint8_t *host = g_malloc(FLASH_PAGE_BYTES);
        g_autofree gchar *buf = g_malloc(4096);
        int fd = g_file_open_tmp("stix-flash-legacy-XXXXXX", &path, NULL);
        int i;
        size_t k;
        QTestState *qts;

        g_assert(fd >= 0);
        memset(buf, 0xFF, 4096);
        for (i = 0; i < 256; i++) {
            g_assert(write(fd, buf, 4096) == 4096);
        }
        close(fd);

        for (k = 0; k < FLASH_PAGE_BYTES; k++) {
            p[k] = (uint8_t)(k ^ 0xA5u);
        }
        memset(pff, 0xFF, FLASH_PAGE_BYTES);

        qts = qtest_initf("-M leon3_generic "
                          "-global grlib-stixflash.flash-file=%s "
                          "-global grlib-stixflash.inverted-storage=off",
                          path);

        /* guest: a file byte 0xFF reads back 0xFF in plain mode */
        qtest_writel(qts, 0x40000000u, 0);
        qtest_writel(qts, IRQMP_CLEAR, FLASH_IRQ_BIT);
        flash_issue(qts, 0x40000000u, 0, CMD_PAGE_READ);
        g_assert_cmphex(qtest_readl(qts, 0x40000000u), ==, 0xffffffffu);

        /* write P to page 2: the host file holds P un-inverted */
        qtest_memwrite(qts, 0x40000000u, p, FLASH_PAGE_BYTES);
        flash_issue(qts, 0x40000000u, 2, CMD_PAGE_WRITE);
        flash_pread_page(path, (uint64_t)2 * FLASH_PAGE_BYTES, host);
        g_assert_cmpint(memcmp(host, p, FLASH_PAGE_BYTES), ==, 0);

        /* erase page 2's block: plain erase writes 0xFF */
        flash_issue(qts, 0x40000000u, 2, CMD_BLOCK_ERASE);
        flash_pread_page(path, (uint64_t)2 * FLASH_PAGE_BYTES, host);
        g_assert_cmpint(memcmp(host, pff, FLASH_PAGE_BYTES), ==, 0);

        qtest_quit(qts);
        g_unlink(path);
    }
}

/* ---- SPW device (grlib_stixspw) ---- */

#define SPW_BASE       0x80000a00u
#define SPW_CONTROL    (SPW_BASE + 0x00u)
#define SPW_STATUS     (SPW_BASE + 0x04u)
#define SPW_DMAControl (SPW_BASE + 0x20u)
#define SPW_DMATxDesc  (SPW_BASE + 0x28u)

#define SPW_DMA_TE (1u << 0)
#define SPW_DMA_TI (1u << 2)
#define SPW_DMA_PS (1u << 5)
#define SPW_IRQ_BIT (1u << 13)              /* IRQ_SPW = 13 */

#define SPW_CTRL_IE (1u << 3)
#define SPW_CTRL_RS (1u << 6)

#define SPW_DMAXRX    (SPW_BASE + 0x24u)
#define SPW_NODEADDR  (SPW_BASE + 0x08u)

#define TXDSC_EN (1u << 12)
#define TXDSC_WR (1u << 13)
#define TXDSC_IE (1u << 14)

/* TX descriptor layout (spwTXdsc_s): ctrl@0, haddr@4, dlen@8, daddr@12. */
#define TXDSC_ADDR  0x40001000u
#define TXDATA_ADDR 0x40002000u

static void test_spw_tx(void)
{
    QTestState *qts;
    uint32_t ctrl;

    qts = stix_qtest_new();

    /*
     * TM payload + TX descriptor in guest RAM (values written via
     * qtest_writel are target-endian, matching the device's raw-read +
     * bswap32 handling). The descriptor is armed exactly as the FSW
     * does it: ctrl 0x7000 = EN|WR|IE (SPW.c:718-721, txdsc.ctrl in
     * the golden stdout).
     */
    qtest_writel(qts, TXDATA_ADDR, 0xaabbccddu);
    qtest_writel(qts, TXDATA_ADDR + 4, 0x11223344u);
    qtest_writel(qts, TXDSC_ADDR + 0, TXDSC_EN | TXDSC_WR | TXDSC_IE);
    qtest_writel(qts, TXDSC_ADDR + 4, 0);               /* haddr */
    qtest_writel(qts, TXDSC_ADDR + 8, 8);               /* dlen */
    qtest_writel(qts, TXDSC_ADDR + 12, TXDATA_ADDR);    /* daddr */

    /* Point the DMA engine at the descriptor table. */
    qtest_writel(qts, SPW_DMATxDesc, TXDSC_ADDR);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);

    /*
     * control.IE set first: the FSW wraps SPW_send in an IE critical
     * section (SPW.c:707-731, D-M).
     */
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_IE);

    /*
     * Trigger transmission (TE|TI, the SPW.c:725 dmaControl |= TE
     * write). No TCP client is attached: the TM send fails silently in
     * the model and the exchange still completes.
     */
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_TE | SPW_DMA_TI);

    /* Descriptor EN bit cleared in guest RAM. */
    ctrl = qtest_readl(qts, TXDSC_ADDR);
    g_assert_cmpuint(ctrl & TXDSC_EN, ==, 0);

    /* IRQ 13 latched in IRQMP pending. */
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT,
                     ==, SPW_IRQ_BIT);

    /* dmaControl ends with PS set (exchange-complete modifier). */
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_PS,
                     ==, SPW_DMA_PS);

    /* Status keeps LS = RUN (0x00a00000). */
    g_assert_cmphex(qtest_readl(qts, SPW_STATUS), ==, 0x00a00000u);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- CTRL device (grlib_stixctrl) ---- */

#define CTRL_BASE 0x80000500u

/* Register offsets in stixCtrlRegs_s */
#define CTRL_ROTBCMDSTA 0x54u
#define CTRL_ROTBCONFIG 0x58u
#define CTRL_HKCMDSTA   0x88u
#define CTRL_HKADC      0x8cu
#define CTRL_WatchDog   0xd4u
#define CTRL_MAGICREG   0xd8u
#define CTRL_STIXSTATUS 0xf0u

#define HKADC_READY (1u << 7)
/*
 * hkAdc layout (StixIdpu.h:920-968): VAL 31:20, READY 7, ADDR 6:0,
 * bits 19:8 reserved-zero.
 */
#define HKADC_VAL_SHIFT 20u
#define HKADC_VAL_MASK 0xFFFu
#define HKADC_RESV_MASK 0xFFF00u
#define HKADC_ADDR_MASK 0x7Fu

static uint32_t ctrl_read(QTestState *qts, uint32_t off)
{
    return qtest_readl(qts, CTRL_BASE + off);
}

static void ctrl_write(QTestState *qts, uint32_t off, uint32_t val)
{
    qtest_writel(qts, CTRL_BASE + off, val);
}

/* ---- CTRL SCET set semantics (review T1, decision D1) ---- */

#define CTRL_LONGTIMERSEC 0xc8u
#define CTRL_LONGTIMERSUBSEC 0xccu
#define NSEC_PER_SEC 1000000000LL
#define SUBSEC_PER_SEC 65536

/* seconds field of 0xC8 = bits 30:0; bit 31 is the UNSYNCED status flag
 * (Timing.c TIMING_COARSE_UNSYNCED), checked separately. */
static uint32_t ctrl_sec(QTestState *qts)
{
    return ctrl_read(qts, CTRL_LONGTIMERSEC) & 0x7fffffffu;
}

static uint32_t ctrl_flag(QTestState *qts)
{
    return ctrl_read(qts, CTRL_LONGTIMERSEC) >> 31;
}

static uint32_t ctrl_sub(QTestState *qts)
{
    return ctrl_read(qts, CTRL_LONGTIMERSUBSEC) & 0xffffu;
}

/* tolerance for one read round-trip, in sub-second ticks */
#define SUBSEC_TOL 1

static void test_ctrl_scet(void)
{
    QTestState *qts = stix_qtest_new();

    /* 1. the FSW's real write pair (Timing.c timerLostSpWTCsCallback):
     *    stage 0xC8 = 0x7b | UNSYNCED (bit 31), then commit 0xCC = 0xe0c4.
     *    Must not abort (red on 5.2: assert(0); red on the first D1 draft:
     *    the flag bit moved the clock +68 years). */
    ctrl_write(qts, CTRL_LONGTIMERSEC, 0x8000007bu);
    ctrl_write(qts, CTRL_LONGTIMERSUBSEC, 0xe0c4u);

    /* 2. commit lands: time = (staged secs, written subsec); the UNSYNCED
     *    flag is readable in bit 31 of 0xC8. */
    g_assert_cmpuint(ctrl_sec(qts), ==, 0x7b);
    uint32_t sub = ctrl_sub(qts);
    g_assert_cmpuint(sub, >=, 0xe0c4 - SUBSEC_TOL);
    g_assert_cmpuint(sub, <=, 0xe0c4 + SUBSEC_TOL);
    g_assert_cmpuint(ctrl_flag(qts), ==, 1);

    /* 3. a lone 0xC8 write stages only (HSWICD 4.51: the commit happens
     *    at the SubSec write): running time untouched, flag follows the
     *    written bit 31. */
    ctrl_write(qts, CTRL_LONGTIMERSEC, 1000);
    g_assert_cmpuint(ctrl_sec(qts), ==, 0x7b);
    g_assert_cmpuint(ctrl_flag(qts), ==, 0);

    /* 4. the committed clock free-runs: a 0.5 s step from (0x7b, 0xe0c4)
     *    lands at (0x7c, 0xe0c4 + 0x8000 - 0x10000 = 0x60c4). */
    qtest_clock_step(qts, (int64_t)500 * 1000 * 1000);
    g_assert_cmpuint(ctrl_sec(qts), ==, 0x7c);
    sub = ctrl_sub(qts);
    g_assert_cmpuint(sub, >=, 0x60c4 - SUBSEC_TOL);
    g_assert_cmpuint(sub, <=, 0x60c4 + SUBSEC_TOL);

    /* 5. a fresh commit sets seconds and fraction together. */
    ctrl_write(qts, CTRL_LONGTIMERSEC, 5);
    ctrl_write(qts, CTRL_LONGTIMERSUBSEC, 0x8000u);
    g_assert_cmpuint(ctrl_sec(qts), ==, 5);
    sub = ctrl_sub(qts);
    g_assert_cmpuint(sub, >=, 0x8000 - SUBSEC_TOL);
    g_assert_cmpuint(sub, <=, 0x8000 + SUBSEC_TOL);

    /* 6. large epoch commits cleanly (no wrap, no assert). */
    ctrl_write(qts, CTRL_LONGTIMERSEC, 0x30000000u);
    ctrl_write(qts, CTRL_LONGTIMERSUBSEC, 0);
    g_assert_cmpuint(ctrl_sec(qts), ==, 0x30000000u);

    /* 7. the clock can be set backwards. */
    ctrl_write(qts, CTRL_LONGTIMERSEC, 10);
    ctrl_write(qts, CTRL_LONGTIMERSUBSEC, 0x8000u);
    g_assert_cmpuint(ctrl_sec(qts), ==, 10);

    /* 8. a flagged commit carries the UNSYNCED bit through. */
    ctrl_write(qts, CTRL_LONGTIMERSEC, 0x80000001u);
    ctrl_write(qts, CTRL_LONGTIMERSUBSEC, 0);
    g_assert_cmpuint(ctrl_sec(qts), ==, 1);
    g_assert_cmpuint(ctrl_flag(qts), ==, 1);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

static void test_ctrl(void)
{
    QTestState *qts;

    qts = stix_qtest_new();

    /* Reset state pins (probe-pinned from the repaired old build).
     * Under -accel qtest the virtual clock is idle, so longTimer reads
     * (0xC8/0xCC, handled by the read handler's SCET refresh) are stable.
     * NOTE: reads of 0xC8/0xCC are fine; WRITES to them assert(0). */
    g_test_message("ctrl[0x54] = 0x%08x", ctrl_read(qts, CTRL_ROTBCMDSTA));
    g_test_message("ctrl[0x58] = 0x%08x", ctrl_read(qts, CTRL_ROTBCONFIG));
    g_test_message("ctrl[0x88] = 0x%08x", ctrl_read(qts, CTRL_HKCMDSTA));
    g_test_message("ctrl[0xf0] = 0x%08x", ctrl_read(qts, CTRL_STIXSTATUS));

    g_assert_cmphex(ctrl_read(qts, CTRL_ROTBCMDSTA), ==, 0x00000000u);
    g_assert_cmphex(ctrl_read(qts, CTRL_ROTBCONFIG), ==, 0x00000000u);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKCMDSTA), ==, 0x00000000u);
    g_assert_cmphex(ctrl_read(qts, CTRL_STIXSTATUS), ==, 0x00000000u);

    /* The watchdog register (0xD4) is a command register, not plain
     * storage: the FSW's 0x40000000 is FORCE, which resets the IDPU.
     * Pinned in /sparc/stix/ctrl-watchdog. */

    /* magicReg stores too (FSW start-up magic 0xa56d5b74). */
    ctrl_write(qts, CTRL_MAGICREG, 0xa56d5b74u);
    g_assert_cmphex(ctrl_read(qts, CTRL_MAGICREG), ==, 0xa56d5b74u);

    /* Model repair pin: an hkAdc write completes the conversion
     * immediately — READY (bit 7) must be set in the stored value. */
    ctrl_write(qts, CTRL_HKADC, 0x18u);
    g_assert_cmpuint(ctrl_read(qts, CTRL_HKADC) & HKADC_READY, ==, HKADC_READY);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

static void test_pnp(void)
{
    QTestState *qts;
    unsigned i;

    qts = stix_qtest_new();

    for (i = 0; i < PNP_ENTRY_COUNT; i++) {
        stix_pnp_check(qts, i, pnp_golden[i]);
    }
    for (i = PNP_ENTRY_COUNT; i < 16; i++) {
        stix_pnp_check(qts, i, 0);
    }

    /* Sub-word access equivalence (Review Focus: the STIX byte-access hack
     * was dropped because v11.1's pnp ops handle sub-word reads natively;
     * this pins byte/half/word agreement so a future upstream ops change
     * cannot silently break the FSW's drvmgr access path). */
    for (i = 0; i < PNP_ENTRY_COUNT; i++) {
        uint64_t a = APB_PNP_BASE + 4u * i;
        uint32_t word = qtest_readl(qts, a);
        /* target is big-endian: assemble high-to-low */
        uint32_t from_bytes = (qtest_readb(qts, a) << 24)
                            | (qtest_readb(qts, a + 1) << 16)
                            | (qtest_readb(qts, a + 2) << 8)
                            | qtest_readb(qts, a + 3);
        uint32_t from_halves = (qtest_readw(qts, a) << 16)
                             | qtest_readw(qts, a + 2);
        g_assert_cmphex(from_bytes, ==, word);

        /* Half-word reads: the v11.1 native pnp ops return the correct
         * halves. The 5.2 STIX byte-access hack returned the word's LOW
         * half at any half-word offset (endian-shift bug) - a quirk the
         * port intentionally fixed; the FSW reads PnP words only (drvmgr),
         * so the difference is not guest-observable on any pinned flow.
         * Both behaviors are pinned so a third variant cannot sneak in. */
        uint32_t hack_signature = ((word & 0xffffu) << 16) | (word & 0xffffu);
        g_assert(from_halves == word || from_halves == hack_signature);
    }

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ROTB command/status (0x54, review follow-up found by the soak): only
 * the COMMAND field (bits 1:0) and the EDACRESET strobe (bit 2) are
 * writable. BLOCKCOUNT (bits 23:7), SECCOUNT (bits 31:24), READY, FULL
 * and DEDETECTED are read-only status. The FSW's EDAC counter reset
 * (EDAC.c, E_EDAC_MODULE_ROTBUF) clears its strobe with a full-register
 * write of ~(1u << 2) = 0xFFFFFFFB; storing that raw made
 * RotBuf_blocksCount() read 0x1FFFF = 131071 blocks, level 8 > mask 7,
 * and the next HK report asserted (HKAcquire.c:325) — the soak crash at
 * ~64 s FSW, masked on the 5.2 baseline by the SCET abort at ~60 s. */
static void test_ctrl_rotb(void)
{
    QTestState *qts = stix_qtest_new();

    /* the FSW's EDACRESET strobe pair, verbatim */
    ctrl_write(qts, 0x54, 0x00000004u);
    ctrl_write(qts, 0x54, 0xFFFFFFFBu);

    /* no strobe write may leak into read-only status fields */
    g_assert_cmphex(ctrl_read(qts, 0x54) & ~0x7u, ==, 0);

    /* the FSW's GETBLOCK read-modify-write still lands in COMMAND */
    ctrl_write(qts, 0x54, 0x1u);
    g_assert_cmphex(ctrl_read(qts, 0x54) & 0x7u, ==, 0x1u);

    /* rotbConfig ENABLE (bit 31) must assert rotbCmdSta.READY (bit 3):
     * the FSW's RotBuf_init waits for it and a timeout is fatal
     * ("Fatal error: 1" at init). This passed by accident before the
     * masking above — the EDAC strobe-clear write 0xFFFFFFFB set READY
     * through the raw register store. */
    ctrl_write(qts, 0x58, 1u << 31);
    g_assert_cmpuint((ctrl_read(qts, 0x54) >> 3) & 1u, ==, 1u);
    ctrl_write(qts, 0x58, 0);
    g_assert_cmpuint((ctrl_read(qts, 0x54) >> 3) & 1u, ==, 0u);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* CTRL watchdog (0xD4). Semantics from the FSW driver (BSW WDT.c header
 * comment, WDT.c and HWControl/Watchdog.c): COMMAND bits 31:29, RELOADVAL
 * bits 16:11, COUNT bits 5:0. Not running at power-up. RELOAD loads COUNT
 * from RELOADVAL and also enables counting; DISABLE stops it; ENABLE
 * resumes; CHANGE sets RELOADVAL without touching COUNT; FORCE resets the
 * IDPU at once. COUNT drops by one every 105 ms (Watchdog.c
 * WATCHDOG_CYCLE_DURATION) and the IDPU resets when it runs out.
 * Unmodeled, the register read back 0 after every feed, so the FSW's
 * Watchdog task logged an error each second and raised FDIR — a path the
 * hardware never takes. */
#define WDG                0xD4u
#define WDG_CMD(c)         ((uint32_t)(c) << 29)
#define WDG_RELOAD         WDG_CMD(1)
#define WDG_FORCE          WDG_CMD(2)
#define WDG_DISABLE        WDG_CMD(5)
#define WDG_ENABLE         WDG_CMD(6)
#define WDG_CHANGE(n)      (WDG_CMD(7) | ((uint32_t)(n) << 11))
#define WDG_COUNT(qts)     (ctrl_read(qts, WDG) & 0x3Fu)
#define WDG_CYCLE_NS       (105 * 1000 * 1000LL)

/* true if a RESET event arrived; the QMP round trip pulls in any event
 * emitted before it (events are only buffered while reading QMP) */
static bool wdg_saw_reset(QTestState *qts)
{
    QDict *ev;
    bool seen;

    qobject_unref(qtest_qmp(qts, "{'execute': 'query-status'}"));
    ev = qtest_qmp_event_ref(qts, "RESET");
    seen = ev != NULL;
    qobject_unref(ev);
    return seen;
}

static void test_ctrl_watchdog(void)
{
    QTestState *qts = stix_qtest_new();

    /* power-up: not running */
    g_assert_cmpuint(WDG_COUNT(qts), ==, 0);
    qtest_clock_step(qts, 100 * WDG_CYCLE_NS);
    g_assert_false(wdg_saw_reset(qts));

    /* the FSW init sequence, DISABLE_WATCHDOG build (Watchdog_init ->
     * setRefreshInterval(63), enable): CHANGE, RELOAD, DISABLE, ENABLE,
     * DISABLE, RELOAD. The final feed must read back 63 cycles — the
     * check behind the per-second "invalid number of cycles" error. */
    ctrl_write(qts, WDG, WDG_CHANGE(63));
    g_assert_cmpuint(WDG_COUNT(qts), ==, 0);       /* CHANGE keeps COUNT */
    g_assert_cmpuint((ctrl_read(qts, WDG) >> 11) & 0x3Fu, ==, 63);
    ctrl_write(qts, WDG, WDG_RELOAD);
    ctrl_write(qts, WDG, WDG_DISABLE);
    ctrl_write(qts, WDG, WDG_ENABLE);
    ctrl_write(qts, WDG, WDG_DISABLE);
    ctrl_write(qts, WDG, WDG_RELOAD);
    g_assert_cmpuint(WDG_COUNT(qts), ==, 63);

    /* RELOAD enabled counting: one cycle per 105 ms */
    qtest_clock_step(qts, 10 * WDG_CYCLE_NS);
    g_assert_cmpuint(WDG_COUNT(qts), ==, 53);

    /* DISABLE freezes the count, ENABLE resumes from it */
    ctrl_write(qts, WDG, WDG_DISABLE);
    qtest_clock_step(qts, 20 * WDG_CYCLE_NS);
    g_assert_cmpuint(WDG_COUNT(qts), ==, 53);
    ctrl_write(qts, WDG, WDG_ENABLE);
    qtest_clock_step(qts, 3 * WDG_CYCLE_NS);
    g_assert_cmpuint(WDG_COUNT(qts), ==, 50);

    /* a 1 s feed cadence (Watchdog task period) never expires */
    for (int i = 0; i < 20; i++) {
        ctrl_write(qts, WDG, WDG_RELOAD);
        g_assert_cmpuint(WDG_COUNT(qts), ==, 63);
        qtest_clock_step(qts, 1000 * 1000 * 1000LL);
    }
    g_assert_false(wdg_saw_reset(qts));

    /* no feed: the IDPU resets once the count runs out (~6.6 s) */
    ctrl_write(qts, WDG, WDG_RELOAD);
    qtest_clock_step(qts, 62 * WDG_CYCLE_NS);
    g_assert_false(wdg_saw_reset(qts));
    qtest_clock_step(qts, 2 * WDG_CYCLE_NS);
    qtest_qmp_eventwait(qts, "RESET");

    /* reset stops the watchdog again */
    g_assert_cmpuint(WDG_COUNT(qts), ==, 0);
    qtest_clock_step(qts, 100 * WDG_CYCLE_NS);
    g_assert_false(wdg_saw_reset(qts));

    /* FORCE resets at once (WDT_forceIdpuReset, the trap handler) */
    ctrl_write(qts, WDG, WDG_FORCE);
    qtest_qmp_eventwait(qts, "RESET");

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* CTRL IRQ output wiring. The baseline connected all five CTRL IRQ outputs
 * with sysbus_connect_irq(dev, 0, ...): every call rebound the same link,
 * the last one won, and only output 0 (detectors) stayed connected - to
 * the ROTB CPU IRQ line 7. Each sysbus IRQ output is exposed over QOM as a
 * link property "sysbus-irq[i]" (sysbus_init_irq -> qdev_init_gpio_out_
 * named, hw/core/sysbus.c) whose value is the canonical path of the
 * destination input pin; IRQMP's inputs are its "unnamed-gpio-in[k]" child
 * objects (hw/core/gpio.c). The fixed wiring gives outputs 0-4
 * (detectors, hk, other, edac, rotb) their own IRQMP lines 3-7
 * (StixIdpu.h IRQ_STIXCTRL_*). */
static void test_ctrl_irq_lines(void)
{
    QTestState *qts = stix_qtest_new();
    QDict *resp;
    QList *props;
    const QListEntry *entry;
    g_autofree gchar *ctrl_path = NULL;
    g_autofree gchar *irqmp_path = NULL;
    int i;

    /*
     * P4-2: the controller has the fixed path /machine/stixctrl; the
     * IRQMP is still found under /machine/unattached by type, not by the
     * volatile device[N] names
     */
    ctrl_path = g_strdup("/machine/stixctrl");
    resp = qtest_qmp(qts,
                     "{'execute': 'qom-list',"
                     " 'arguments': {'path': '/machine/unattached'}}");
    g_assert(qdict_haskey(resp, "return"));
    props = qobject_to(QList, qdict_get(resp, "return"));
    g_assert_nonnull(props);
    QLIST_FOREACH_ENTRY(props, entry) {
        QDict *prop = qobject_to(QDict, qlist_entry_obj(entry));
        const char *name = qdict_get_str(prop, "name");
        const char *type = qdict_get_str(prop, "type");

        /* the controller is no longer an unattached child */
        g_assert_false(g_str_equal(type, "child<grlib-stixctrl>"));
        if (g_str_equal(type, "child<grlib-irqmp>")) {
            /* qom-list reports the child name; qom-get/qom-set want the
             * canonical path, and link values are canonical paths */
            g_assert_null(irqmp_path);
            irqmp_path = g_strdup_printf("/machine/unattached/%s", name);
        }
    }
    qobject_unref(resp);
    g_assert_nonnull(ctrl_path);
    g_assert_nonnull(irqmp_path);

    for (i = 0; i < 5; i++) {
        g_autofree gchar *prop = g_strdup_printf("sysbus-irq[%d]", i);
        g_autofree gchar *want = g_strdup_printf("%s/unnamed-gpio-in[%d]",
                                                 irqmp_path, 3 + i);
        const char *got;

        resp = qtest_qmp(qts,
                         "{'execute': 'qom-get', 'arguments':"
                         " {'path': %s, 'property': %s}}",
                         ctrl_path, prop);
        got = qdict_get_try_str(resp, "return");
        g_test_message("sysbus-irq[%d]: got '%s' want '%s'", i, got, want);
        g_assert_cmpstr(got, ==, want);
        qobject_unref(resp);
    }

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- CTRL ROTB (A2): ring, FIFO commands, BLOCK window ---- */

#define ROTB_WINDOW       0xE0800000u  /* STIXCTRL_BLOCK_START, 512 B per block */
#define ROTB_BLOCK_WORDS  128u         /* 512 B / 4 */
#define ROTB_CMD_GETBLOCK   1u         /* rotbCmdSta COMMAND values */
#define ROTB_CMD_PUTBLOCK   2u
#define ROTB_CMD_RESETFIFO  3u
#define ROTB_STA_READY     (1u << 3)    /* rotbCmdSta bit fields */
#define ROTB_STA_FULL      (1u << 5)
#define ROTB_COUNT_SHIFT   7u           /* BLOCKCOUNT, bits 23:7 */
#define ROTB_COUNT_MASK    0x1FFFFu
#define ROTB_CFG_ENABLE    (1u << 31)   /* rotbConfig */
#define ROTB_CFG_TESTMODE  (1u << 13)
#define ROTB_IRQ_BIT       (1u << 7)    /* IRQ_STIXCTRL_ROTB = 7 */

/* the FSW issues every command as a read-modify-write of rotbCmdSta
 * (RotBuf.c RotBuf_advancePointer/RotBuf_clear): clear COMMAND, OR the new
 * one. Since M-1 the device keeps only the COMMAND bits 1:0 of the write
 * (bit 2 is the self-clearing EdacReset strobe), so the BLOCKCOUNT and
 * READY bits echoed back are ignored. */
static void rotb_cmd(QTestState *qts, uint32_t cmd)
{
    uint32_t v = ctrl_read(qts, CTRL_ROTBCMDSTA);

    ctrl_write(qts, CTRL_ROTBCMDSTA, (v & ~3u) | cmd);
}

/* BLOCKCOUNT (bits 23:7) of the composed rotbCmdSta read */
static uint32_t rotb_count(QTestState *qts)
{
    return (ctrl_read(qts, CTRL_ROTBCMDSTA) >> ROTB_COUNT_SHIFT) & ROTB_COUNT_MASK;
}

static uint32_t rotb_full(QTestState *qts)
{
    return (ctrl_read(qts, CTRL_ROTBCMDSTA) >> 5) & 1u;
}

static void rotb_cfg(QTestState *qts, bool enable, bool testmode)
{
    ctrl_write(qts, CTRL_ROTBCONFIG,
               (enable ? ROTB_CFG_ENABLE : 0u) | (testmode ? ROTB_CFG_TESTMODE : 0u));
}

/* does this QEMU know the rotb-blocks property? The RED run executes
 * against a pre-A2 binary, where an unknown -global property aborts QEMU
 * at startup (qdev_prop_set_globals uses &error_fatal) — so pass the
 * argument only when the model supports it. The ring size does not affect
 * any RED assertion; without it the default (104448 blocks) is used. */
static bool rotb_blocks_supported(void)
{
    static int supported = -1;

    if (supported < 0) {
        QTestState *qts = stix_qtest_new();
        QDict *resp;
        QList *props;
        const QListEntry *entry;

        resp = qtest_qmp(qts,
                         "{'execute': 'device-list-properties',"
                         " 'arguments': {'typename': 'grlib-stixctrl'}}");
        props = qobject_to(QList, qdict_get(resp, "return"));
        g_assert_nonnull(props);
        supported = 0;
        QLIST_FOREACH_ENTRY(props, entry) {
            QDict *prop = qobject_to(QDict, qlist_entry_obj(entry));

            if (g_str_equal(qdict_get_str(prop, "name"), "rotb-blocks")) {
                supported = 1;
                break;
            }
        }
        qobject_unref(resp);
        qtest_quit(qts);
        stix_qtest_cleanup();
    }

    return supported != 0;
}

/* one test instance: a small ring (the default ring costs ~53.5 MB of
 * host RAM per instance) */
static QTestState *rotb_qtest_new(unsigned blocks)
{
    g_autofree gchar *extra = NULL;

    if (rotb_blocks_supported()) {
        extra = g_strdup_printf("-global grlib-stixctrl.rotb-blocks=%u",
                                blocks);
    } else {
        extra = g_strdup("");
    }

    return stix_qtest_new_extra(extra);
}

/* FIFO pointer arithmetic through the COMMAND field (RotBuf.c
 * RotBuf_advancePointer/RotBuf_clear; the OpModeMgmt.c SAFE/NOMINAL
 * switches toggle TESTMODE around them). */
static void test_rotb_fifo(void)
{
    QTestState *qts = rotb_qtest_new(8);

    /* ENABLE asserts READY: the FSW's RotBuf_init waits for it */
    rotb_cfg(qts, true, false);
    g_assert_cmpuint(ctrl_read(qts, CTRL_ROTBCMDSTA) & ROTB_STA_READY,
                     ==, ROTB_STA_READY);

    /* RESETFIFO at init: count 0 (the main.c:214 assert condition) */
    rotb_cmd(qts, ROTB_CMD_RESETFIFO);
    g_assert_cmpuint(rotb_count(qts), ==, 0);

    /* TESTMODE: PUTBLOCK advances the write pointer */
    rotb_cfg(qts, true, true);
    rotb_cmd(qts, ROTB_CMD_PUTBLOCK);
    rotb_cmd(qts, ROTB_CMD_PUTBLOCK);
    rotb_cmd(qts, ROTB_CMD_PUTBLOCK);
    g_assert_cmpuint(rotb_count(qts), ==, 3);

    /* normal mode: GETBLOCK drains */
    rotb_cfg(qts, true, false);
    rotb_cmd(qts, ROTB_CMD_GETBLOCK);
    rotb_cmd(qts, ROTB_CMD_GETBLOCK);
    rotb_cmd(qts, ROTB_CMD_GETBLOCK);
    g_assert_cmpuint(rotb_count(qts), ==, 0);

    /* GETBLOCK on an empty ring holds */
    rotb_cmd(qts, ROTB_CMD_GETBLOCK);
    g_assert_cmpuint(rotb_count(qts), ==, 0);

    /* PUTBLOCK outside TESTMODE is ignored: in normal mode HW feeds */
    rotb_cmd(qts, ROTB_CMD_PUTBLOCK);
    g_assert_cmpuint(rotb_count(qts), ==, 0);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* the BLOCK window shows ring[rd] on reads and ring[wr] on TESTMODE
 * writes: the FSW fills and reads through RotBuf_dataAddress() =
 * 0xE0800000 with 32-bit and byte stores (Common.c Volatile_memset) */
static void test_rotb_window(void)
{
    QTestState *qts = rotb_qtest_new(4);
    uint32_t i;
    int b;

    rotb_cfg(qts, true, true);
    qtest_writel(qts, ROTB_WINDOW, 0x11111111u);
    qtest_writel(qts, ROTB_WINDOW + 4u * 127, 0x22222222u);
    rotb_cmd(qts, ROTB_CMD_PUTBLOCK);

    /* window reads show the current read block (rd = 0) */
    g_assert_cmphex(qtest_readl(qts, ROTB_WINDOW), ==, 0x11111111u);
    g_assert_cmphex(qtest_readl(qts, ROTB_WINDOW + 4u * 127), ==, 0x22222222u);

    /* byte-wise fill of block 1 */
    for (b = 0; b < 512; b++) {
        qtest_writeb(qts, ROTB_WINDOW + b, 0x5A);
    }
    rotb_cmd(qts, ROTB_CMD_PUTBLOCK);

    /* GETBLOCK moves rd to block 1: every word reads 0x5A5A5A5A and
     * block 0's markers are gone from view */
    rotb_cmd(qts, ROTB_CMD_GETBLOCK);
    for (i = 0; i < ROTB_BLOCK_WORDS; i++) {
        g_assert_cmphex(qtest_readl(qts, ROTB_WINDOW + 4u * i), ==, 0x5A5A5A5Au);
    }

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* IRQ 7 rule D-F: reaching capacity sets FULL only; a PUTBLOCK INTO a full
 * ring is dropped (drop-new) and pulses IRQ 7 once per rejected write.
 * The FSW fills to exactly cap with IRQ 7 enabled since boot
 * (HWSelftest.c:510-526, RotAccumulator.c:259-266), so reaching cap must
 * stay silent. The SW-PUTBLOCK half of the rule is an assumption
 * (HSWICD unconfirmed). Window stores never pulse (they are not
 * PUTBLOCKs). */
static void test_rotb_full_irq(void)
{
    QTestState *qts = rotb_qtest_new(4);
    uint32_t n, i;

    rotb_cfg(qts, true, true);
    qtest_writel(qts, IRQMP_CLEAR, ROTB_IRQ_BIT);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & ROTB_IRQ_BIT, ==, 0);

    /* fill to exactly cap, block n carrying marker n */
    for (n = 0; n < 4; n++) {
        for (i = 0; i < ROTB_BLOCK_WORDS; i++) {
            qtest_writel(qts, ROTB_WINDOW + 4u * i, 0xAA000000u | n);
        }
        rotb_cmd(qts, ROTB_CMD_PUTBLOCK);
    }
    g_assert_cmpuint(rotb_count(qts), ==, 4);
    g_assert_cmpuint(rotb_full(qts), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & ROTB_IRQ_BIT, ==, 0);

    /* a window write while full is ignored, with no IRQ */
    qtest_writel(qts, ROTB_WINDOW, 0xDEADBEEFu);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & ROTB_IRQ_BIT, ==, 0);

    /* the 5th PUTBLOCK is rejected: one pulse, count stays 4, the block
     * is not stored (drop-new) */
    rotb_cmd(qts, ROTB_CMD_PUTBLOCK);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & ROTB_IRQ_BIT,
                     ==, ROTB_IRQ_BIT);
    g_assert_cmpuint(rotb_count(qts), ==, 4);

    /* drain in normal mode: markers 0,1,2,3 in order; FULL clears on the
     * first GETBLOCK */
    rotb_cfg(qts, true, false);
    for (n = 0; n < 4; n++) {
        for (i = 0; i < ROTB_BLOCK_WORDS; i++) {
            g_assert_cmphex(qtest_readl(qts, ROTB_WINDOW + 4u * i),
                            ==, 0xAA000000u | n);
        }
        rotb_cmd(qts, ROTB_CMD_GETBLOCK);
        g_assert_cmpuint(rotb_full(qts), ==, 0);
    }
    g_assert_cmpuint(rotb_count(qts), ==, 0);

    rotb_cmd(qts, ROTB_CMD_RESETFIFO);
    g_assert_cmpuint(rotb_count(qts), ==, 0);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* the FSW's own ROTB self-test shape (HWSelftest.c:510-563): fill until
 * FULL in TESTMODE, then switch to normal mode and drain while comparing
 * every word. The fill loop is bounded (2*cap + 8): on a broken model
 * FULL never sets and an unbounded loop would hang instead of failing. */
static void test_rotb_selftest_shape(void)
{
    QTestState *qts = rotb_qtest_new(8);
    const uint32_t bound = 2 * 8 + 8;
    uint32_t n = 0, m = 0, i;

    rotb_cfg(qts, true, true);
    rotb_cmd(qts, ROTB_CMD_RESETFIFO);

    while (!rotb_full(qts) && n < bound) {
        for (i = 0; i < ROTB_BLOCK_WORDS; i++) {
            qtest_writel(qts, ROTB_WINDOW + 4u * i, n);
        }
        rotb_cmd(qts, ROTB_CMD_PUTBLOCK);
        n++;
    }
    /* stopped because FULL, not because of the bound */
    g_assert_cmpuint(n, ==, 8);
    g_assert_cmpuint(rotb_full(qts), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & ROTB_IRQ_BIT, ==, 0);

    rotb_cfg(qts, true, false);
    while (rotb_count(qts) != 0 && m < bound) {
        for (i = 0; i < ROTB_BLOCK_WORDS; i++) {
            g_assert_cmphex(qtest_readl(qts, ROTB_WINDOW + 4u * i), ==, m);
        }
        rotb_cmd(qts, ROTB_CMD_GETBLOCK);
        m++;
    }
    g_assert_cmpuint(m, ==, 8);
    g_assert_cmpuint(rotb_count(qts), ==, 0);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* the EDAC strobe pair (EDAC.c:305-306) with blocks queued: the
 * strobe-clear write 0xFFFFFFFB carries COMMAND = 3, so the documented
 * Q7 side effect executes RESETFIFO. Harmless at the FSW's call sites,
 * which clear the FIFO anyway (RotAccumulator.c:295, HWSelftest.c:569). */
static void test_rotb_strobe(void)
{
    QTestState *qts = rotb_qtest_new(8);
    uint32_t i;

    rotb_cfg(qts, true, true);
    for (i = 0; i < 3; i++) {
        rotb_cmd(qts, ROTB_CMD_PUTBLOCK);
    }
    g_assert_cmpuint(rotb_count(qts), ==, 3);

    /* the FSW's EDACRESET strobe pair, verbatim */
    ctrl_write(qts, CTRL_ROTBCMDSTA, 0x00000004u);
    ctrl_write(qts, CTRL_ROTBCMDSTA, 0xFFFFFFFBu);

    g_assert_cmpuint(rotb_count(qts), ==, 0);
    g_assert_cmpuint(rotb_full(qts), ==, 0);
    /* no SECCOUNT (31:24) garbage leaks into the composed read */
    g_assert_cmpuint(ctrl_read(qts, CTRL_ROTBCMDSTA) & 0xFF000000u, ==, 0);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* system reset: the ring pointers reset, the ring CONTENT is kept (it is
 * zeroed only at realize). The FSW issues RESETFIFO at init
 * (RotBuf.c:78), so it never relies on content across a reset. */
static void test_rotb_reset(void)
{
    QTestState *qts = rotb_qtest_new(8);

    rotb_cfg(qts, true, true);
    qtest_writel(qts, ROTB_WINDOW, 0x12345678u);
    rotb_cmd(qts, ROTB_CMD_PUTBLOCK);
    qtest_writel(qts, ROTB_WINDOW, 0x9ABCDEF0u);
    rotb_cmd(qts, ROTB_CMD_PUTBLOCK);
    g_assert_cmpuint(rotb_count(qts), ==, 2);

    qtest_system_reset(qts);
    g_assert_cmpuint(rotb_count(qts), ==, 0);
    g_assert_cmpuint(rotb_full(qts), ==, 0);

    /* the ring content survived; block 0 is visible again (rd = 0) */
    rotb_cfg(qts, true, true);
    g_assert_cmphex(qtest_readl(qts, ROTB_WINDOW), ==, 0x12345678u);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- CTRL AHB data windows (A3): SRAM/ELUT/LACC, LVAR BRAM, LTRG, ETRG ---- */

#define AHB_SRAM       0xE0000000u  /* STIXCTRL_SRAM_START, 2 MB */
#define AHB_SRAM_GAP   0xE0100000u  /* inside SRAM, between ELUT and LACC0 */
#define AHB_SRAM_LAST  0xE01FFFFCu  /* last SRAM word */
#define AHB_LACC0      0xE0180000u  /* STIXCTRL_LACC0_START */
#define AHB_LVAR       0xE0200000u  /* STIXCTRL_LVAR0_START, 0x400 window */
#define AHB_LVAR_LAST  0xE02003FCu  /* last LVAR window word */
#define AHB_UNCHANGED  0xE02000A8u  /* WDT UNCHANGED_STORAGE (trap info) */
#define AHB_MEMBRAM    0xE02003C0u  /* MemBram_STORAGE_ADDRESS (op mode) */
#define AHB_MEMBRAM_MOD 0xE02003C3u /* MemBram EDAC_reboot_module (byte) */
#define AHB_LTRG       0xE0400000u  /* STIXCTRL_LTRG0_START, 0x100 window */
#define AHB_LTRG_LAST  0xE04000FCu
#define AHB_ETRG       0xE0600000u  /* STIXCTRL_ETRG0_START, 0x80 window */
#define AHB_ETRG_LAST  0xE060007Cu

/* the four AHB windows are plain device RAM (D-A): fresh instance reads
 * 0, all of 8/16/32-bit accesses work (D-G), and the windows neither
 * alias each other nor lose data. The FSW byte-stores MemBram at every
 * boot (OpModeMgmt.c:950-954 -> MemBram.c:79-82). */
static void test_ahb_windows(void)
{
    QTestState *qts = rotb_qtest_new(4);
    uint16_t w16;
    uint8_t b8;

    /* a fresh instance reads 0 everywhere */
    g_assert_cmpuint(qtest_readl(qts, AHB_SRAM), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, AHB_LVAR), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, AHB_LTRG), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, AHB_ETRG), ==, 0);

    /* 32-bit write/read of distinct patterns across all four windows,
     * including the LACC banks, a gap inside SRAM and the last word of
     * each window */
    qtest_writel(qts, AHB_SRAM, 0x10101010u);
    qtest_writel(qts, AHB_LACC0, 0x22222222u);
    qtest_writel(qts, AHB_SRAM_GAP, 0x33333333u);
    qtest_writel(qts, AHB_SRAM_LAST, 0x44444444u);
    qtest_writel(qts, AHB_LVAR, 0x55555555u);
    qtest_writel(qts, AHB_LVAR_LAST, 0x66666666u);
    qtest_writel(qts, AHB_LTRG, 0x77777777u);
    qtest_writel(qts, AHB_LTRG_LAST, 0x88888888u);
    qtest_writel(qts, AHB_ETRG, 0x99999999u);
    qtest_writel(qts, AHB_ETRG_LAST, 0xAAAAAAAAu);

    g_assert_cmphex(qtest_readl(qts, AHB_SRAM), ==, 0x10101010u);
    g_assert_cmphex(qtest_readl(qts, AHB_LACC0), ==, 0x22222222u);
    g_assert_cmphex(qtest_readl(qts, AHB_SRAM_GAP), ==, 0x33333333u);
    g_assert_cmphex(qtest_readl(qts, AHB_SRAM_LAST), ==, 0x44444444u);
    g_assert_cmphex(qtest_readl(qts, AHB_LVAR), ==, 0x55555555u);
    g_assert_cmphex(qtest_readl(qts, AHB_LVAR_LAST), ==, 0x66666666u);
    g_assert_cmphex(qtest_readl(qts, AHB_LTRG), ==, 0x77777777u);
    g_assert_cmphex(qtest_readl(qts, AHB_LTRG_LAST), ==, 0x88888888u);
    g_assert_cmphex(qtest_readl(qts, AHB_ETRG), ==, 0x99999999u);
    g_assert_cmphex(qtest_readl(qts, AHB_ETRG_LAST), ==, 0xAAAAAAAAu);

    /* 16-bit and 8-bit access (D-G: all widths; the SRAM window covers
     * 0xE01FFFFC so the SRAM/LVAR edge does not alias into LVAR) */
    qtest_writew(qts, AHB_MEMBRAM, 0xBEEFu);
    qtest_writew(qts, AHB_MEMBRAM + 2, 0xF00Du);
    w16 = qtest_readw(qts, AHB_MEMBRAM);
    g_assert_cmphex(w16, ==, 0xBEEFu);
    b8 = qtest_readb(qts, AHB_MEMBRAM + 2);
    g_assert_cmphex(b8, ==, 0xF0);

    /* a byte store touches only its byte; big-endian, so byte 0 of the
     * word at 0xE02003C0 is the most significant */
    qtest_writel(qts, AHB_MEMBRAM, 0x00000000u);
    qtest_writeb(qts, AHB_MEMBRAM, 0x03u);
    g_assert_cmphex(qtest_readl(qts, AHB_MEMBRAM), ==, 0x03000000u);

    /* window edges do not alias: the last ETRG write left the first
     * ETRG word untouched */
    g_assert_cmphex(qtest_readl(qts, AHB_ETRG), ==, 0x99999999u);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* the FSW's reboot-persistent BRAM state survives a system reset (D-G):
 * UNCHANGED_STORAGE holds the low half of the bad-trap magic (WDT.c:74),
 * MemBram gets the operation mode as a byte at every boot
 * (OpModeMgmt.c:950-954), and SRAM is ordinary scratch. QEMU RAM is
 * zeroed at power-on only; nothing in the reset path clears it. */
static void test_bram_persist(void)
{
    QTestState *qts = rotb_qtest_new(4);

    qtest_writel(qts, AHB_UNCHANGED, 0x00002C45u);
    qtest_writeb(qts, AHB_MEMBRAM, 0x03u);
    qtest_writel(qts, AHB_SRAM + 0x1000u, 0xCAFEF00Du);

    qtest_system_reset(qts);

    g_assert_cmphex(qtest_readl(qts, AHB_UNCHANGED), ==, 0x00002C45u);
    g_assert_cmphex(qtest_readb(qts, AHB_MEMBRAM), ==, 0x03u);
    g_assert_cmphex(qtest_readl(qts, AHB_SRAM + 0x1000u), ==, 0xCAFEF00Du);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* GPTIMER prescaler set by the -kernel boot stub (hw/sparc/leon3.c).
 * On the IDPU the boot PROM programs the prescaler for a 1 MHz timer tick
 * at the 20 MHz system clock (reload 19, BSW IIR.h IIR_GPT_PRESCALER) and
 * the FSW never writes it; RTEMS assumes 1 us per timer count. The stub
 * inherited upstream's 40 MHz value (39), which halved the tick rate: FSW
 * time ran at ~0.48x wall while the CTRL SCET ran at full rate.
 *
 * Unlike the other tests this one executes guest code (TCG): a minimal
 * ELF that spins at 0x40000000, so the stub runs and jumps to it. TM is
 * moved to a private port so it cannot collide with a running FSW. */
#define GPTIMER_SCALER_RELOAD 0x80000304u
#define GPTIMER0_CONFIG       0x80000318u

static gchar *write_spin_elf(void)
{
    static const uint8_t elf[] = {
        /* ELF32, big-endian, EXEC, EM_SPARC, entry 0x40000000, phoff 52 */
        0x7f, 'E', 'L', 'F', 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 2, 0, 2, 0, 0, 0, 1, 0x40, 0, 0, 0, 0, 0, 0, 52,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 52, 0, 32, 0, 1, 0, 0,
        0, 0, 0, 0,
        /* PT_LOAD: offset 84, vaddr = paddr = 0x40000000, 8 bytes, R+X */
        0, 0, 0, 1, 0, 0, 0, 84, 0x40, 0, 0, 0, 0x40, 0, 0, 0,
        0, 0, 0, 8, 0, 0, 0, 8, 0, 0, 0, 5, 0, 0, 0, 4,
        /* ba . ; nop */
        0x10, 0x80, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    };
    gchar *path = NULL;
    int fd = g_file_open_tmp("stix-spin-XXXXXX.elf", &path, NULL);

    g_assert(fd >= 0);
    g_assert(write(fd, elf, sizeof(elf)) == sizeof(elf));
    close(fd);
    return path;
}

static void test_gptimer_prescaler(void)
{
    g_autofree gchar *elf = write_spin_elf();
    g_autofree gchar *extra = NULL;
    QTestState *qts;
    gint64 deadline;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("needs TCG to run the boot stub");
        g_unlink(elf);
        return;
    }
    extra = g_strdup_printf("-accel tcg -kernel %s", elf);
    qts = stix_qtest_new_extra(extra);

    /* the stub's last store enables timer 0; the prescaler precedes it */
    deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (!(qtest_readl(qts, GPTIMER0_CONFIG) & 1u)) {
        g_assert(g_get_monotonic_time() < deadline);
        g_usleep(1000);
    }
    g_assert_cmpuint(qtest_readl(qts, GPTIMER_SCALER_RELOAD), ==, 19);

    qtest_quit(qts);
    stix_qtest_cleanup();
    g_unlink(elf);
}

/* ---- P2-1: hermetic TM server, realize errors instead of exit(1) ---- */

/* env keys handing the g_test_trap_subprocess child its parameters; the
 * parent picks the port and the temp flash file */
#define TMTC_TEST_PORT_ENV  "STIX_TEST_TMTC_PORT"
#define TMTC_TEST_FLASH_ENV "STIX_TEST_TMTC_FLASH"
#define FLASH_TEST_DIR_ENV  "STIX_TEST_FLASH_DIR"

/* full path of a qtest registered under "/sparc/stix/<name>"; the
 * registration prepends qtest_get_arch() */
static gchar *stix_test_path(const char *name)
{
    return g_strdup_printf("/%s/sparc/stix/%s", qtest_get_arch(), name);
}

/* /sparc/stix/tmtc-hermetic: the machine must start under qtest even with
 * its TM port held — tmtc-server=auto starts no server under qtest (P2-1).
 * The port is a race-free ephemeral hold passed as tmtc-port=<held>, so
 * the test never touches the default port 20000+uid that the gate
 * scripts use (gate-ports plan D-G4; the tmtc-port=0 default path is no
 * longer exercised here, accepted-diffs §41). RED on the pre-P2-1 11.1
 * binary: the device still binds the port at realize ("Bind failed"),
 * QEMU exits, qtest_init aborts. The 5.2 baseline has no tmtc-port
 * property, so the check does not apply there: it SKIPs explicitly.
 */
static void test_tmtc_hermetic(void)
{
    g_autofree gchar *extra = NULL;
    int listening = 0;
    socklen_t llen = sizeof(listening);
    bool has_port = false;
    QTestState *qts;
    QDict *resp;
    QList *props;
    int port, held;

    qts = stix_qtest_new();
    resp = qtest_qmp(qts,
                     "{'execute': 'device-list-properties',"
                     " 'arguments': {'typename': 'grlib-stixspw'}}");
    props = qobject_to(QList, qdict_get(resp, "return"));
    if (props) {
        const QListEntry *entry;

        QLIST_FOREACH_ENTRY(props, entry) {
            QDict *prop = qobject_to(QDict, qlist_entry_obj(entry));

            if (g_str_equal(qdict_get_str(prop, "name"), "tmtc-port")) {
                has_port = true;
                break;
            }
        }
    }
    qobject_unref(resp);
    qtest_quit(qts);
    stix_qtest_cleanup();
    if (!has_port) {
        g_autofree gchar *bin = g_path_get_basename(qtest_qemu_binary(NULL));

        /* only the 5.2 baseline (via its wrapper) may lack the property */
        g_assert_cmpstr(bin, ==, "qemu-system-sparc-52-wrapper.sh");
        g_test_skip("5.2 baseline: no tmtc-port property, the hermetic "
                    "check does not apply");
        return;
    }

    held = stix_hold_free_tcp_port(&port);
    g_assert_cmpint(held, >=, 0);
    /* the hold is really a listener */
    g_assert(getsockopt(held, SOL_SOCKET, SO_ACCEPTCONN,
                        &listening, &llen) == 0);
    g_assert_cmpint(listening, ==, 1);

    extra = g_strdup_printf("-global grlib-stixspw.tmtc-port=%d", port);
    qts = stix_qtest_new_extra(extra);

    /* alive: a PnP readl round-trips, as in test_pnp */
    g_assert_cmphex(stix_pnp_read(qts, 0), ==, pnp_golden[0]);

    qtest_quit(qts);
    stix_qtest_cleanup();
    close(held);
}

/* g_test_trap_subprocess child (precedent:
 * tests/qtest/test-x86-cpuid-compat.c). tmtc-server=on on an occupied
 * port must fail realize with the port number and strerror ("Address
 * already in use") in the message. RED on the pre-P2-1 binary:
 * tmtc-server is an unknown -global property, so QEMU dies with a
 * different message. */
static void test_tmtc_bind_error_subprocess(void)
{
    const char *port = g_getenv(TMTC_TEST_PORT_ENV);
    const char *flash = g_getenv(TMTC_TEST_FLASH_ENV);
    QTestState *qts;

    g_assert_nonnull(port);
    g_assert_nonnull(flash);

    /* qtest_init aborts: QEMU realize fails on the bind and exits */
    qts = qtest_initf("-M leon3_generic "
                      "-global grlib-stixflash.flash-file=%s "
                      "-global grlib-stixspw.tmtc-server=on "
                      "-global grlib-stixspw.tmtc-port=%s",
                      flash, port);
    qtest_quit(qts);
}

static void test_tmtc_bind_error(void)
{
    g_autofree gchar *path = NULL;
    g_autofree gchar *portstr = NULL;
    g_autofree gchar *pat = NULL;
    g_autofree gchar *sub = stix_test_path("tmtc-bind-error/subprocess");
    int fd = g_file_open_tmp("stix-flash-XXXXXX", &path, NULL);
    int port = 0;
    int held;

    g_assert(fd >= 0);
    g_assert(ftruncate(fd, 1024 * 1024) == 0);

    /* hold an ephemeral port race-free across the subprocess run */
    held = stix_hold_free_tcp_port(&port);
    g_assert_cmpint(held, >=, 0);

    portstr = g_strdup_printf("%d", port);
    g_setenv(TMTC_TEST_PORT_ENV, portstr, TRUE);
    g_setenv(TMTC_TEST_FLASH_ENV, path, TRUE);

    g_test_trap_subprocess(sub, 30 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    /* one glob: the bind message carries loopback, the port and strerror */
    pat = g_strdup_printf("*127.0.0.1:%d*Address already in use*", port);
    g_test_trap_assert_stderr(pat);

    close(held);
    close(fd);
    g_unlink(path);
}

/* g_test_trap_subprocess child: a flash backing file under a nonexistent
 * directory must fail realize with the path named in the error. RED on
 * the pre-P2-1 binary: exit(1) with only "can't create file <errno>" on
 * stdout, no path on stderr. */
static void test_flash_create_error_subprocess(void)
{
    const char *dir = g_getenv(FLASH_TEST_DIR_ENV);
    g_autofree gchar *path = NULL;
    QTestState *qts;

    g_assert_nonnull(dir);
    path = g_strdup_printf("%s/missing/flash.bin", dir);

    qts = qtest_initf("-M leon3_generic -global grlib-stixflash.flash-file=%s",
                      path);
    qtest_quit(qts);
}

static void test_flash_create_error(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("stix-flash-err-XXXXXX", NULL);
    g_autofree gchar *pat = g_strdup_printf("*cannot create flash file %s/missing/flash.bin*",
                                            dir);
    g_autofree gchar *sub = stix_test_path("flash-create-error/subprocess");

    g_setenv(FLASH_TEST_DIR_ENV, dir, TRUE);

    g_test_trap_subprocess(sub, 30 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr(pat);

    g_rmdir(dir);
}

/* ---- P2-3: SPW register semantics (W1C, reserved bits, D-M) ---- */

#define SPW_DMA_FSW_OPEN 0x181eu /* RD|NS|AI|RE|TI|RI, SPW.c:572-584 */

/*
 * The FSW's SPW_open sequence (SPW.c:495-584) and the GRSPW register
 * rules behind it: RS self-clears, dmaMaxRxLength reads back, the
 * dmaControl clear write 0xFFFE01E0 reads 0 (reserved 31:17 + W1C
 * PR/PS/TA/RA), the latched write 0x181e reads back; status is W1C with
 * LS stuck at RUN; PS is set by a TX and cleared by writing 1; a write
 * to another register leaves dmaControl alone; TE clears after every
 * processed TE write; TXDSC_EN gates the TX; the TX IRQ needs TXDSC_IE
 * and TI and is deferred while control.IE = 0 (D-M); system reset
 * restores the power-on values and clears the latch.
 * RED on the pre-P2-3 binary: the write handler clobbered status to
 * 0xA00000 and dmaControl to 0 or PS on every write.
 */
static void test_spw_regs(void)
{
    QTestState *qts;
    uint32_t v;

    qts = stix_qtest_new();

    /* ---- SPW_open sequence ---- */

    /* control: RS self-clears, no other bit was set */
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_RS);
    g_assert_cmphex(qtest_readl(qts, SPW_CONTROL), ==, 0);

    /* dmaMaxRxLength: the read-back must equal 252 (SPW.c:523-528) */
    qtest_writel(qts, SPW_DMAXRX, 252);
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAXRX), ==, 252);

    /*
     * clear write 0xFFFE0000|RA|TA|PR|PS: reserved 31:17 and the W1C
     * bits set; the read-back must be exactly 0 (SPW.c:531-542)
     */
    qtest_writel(qts, SPW_DMAControl, 0xFFFE01E0u);
    g_assert_cmphex(qtest_readl(qts, SPW_DMAControl), ==, 0);

    /*
     * RX (bit 10) is the read-only reception status and AT (bit 9) a
     * self-clearing strobe: writes are ignored and both read 0
     */
    qtest_writel(qts, SPW_DMAControl, (1u << 9) | (1u << 10));
    g_assert_cmphex(qtest_readl(qts, SPW_DMAControl), ==, 0);

    /* the latched write reads back its latched bits */
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN);
    g_assert_cmphex(qtest_readl(qts, SPW_DMAControl), ==, SPW_DMA_FSW_OPEN);

    /* reserved bits 31:17 never read back */
    qtest_writel(qts, SPW_DMAControl, 0xFFFE0000u | SPW_DMA_FSW_OPEN);
    g_assert_cmphex(qtest_readl(qts, SPW_DMAControl), ==, SPW_DMA_FSW_OPEN);

    /* ---- status: W1C writes never disturb LS = RUN ---- */
    qtest_writel(qts, SPW_STATUS, 0xFFFFFFFFu);
    g_assert_cmphex(qtest_readl(qts, SPW_STATUS), ==, 0x00a00000u);
    qtest_writel(qts, SPW_STATUS, 0);
    g_assert_cmphex(qtest_readl(qts, SPW_STATUS), ==, 0x00a00000u);

    /* ---- TX: PS set, TE clears, latch bits kept, IRQ deferred ---- */

    /* descriptor EN|WR|IE with 8 payload bytes (txdsc.ctrl 0x7000) */
    qtest_writel(qts, TXDATA_ADDR, 0xaabbccddu);
    qtest_writel(qts, TXDATA_ADDR + 4, 0x11223344u);
    qtest_writel(qts, TXDSC_ADDR + 0, TXDSC_EN | TXDSC_WR | TXDSC_IE);
    qtest_writel(qts, TXDSC_ADDR + 4, 0);
    qtest_writel(qts, TXDSC_ADDR + 8, 8);
    qtest_writel(qts, TXDSC_ADDR + 12, TXDATA_ADDR);
    qtest_writel(qts, SPW_DMATxDesc, TXDSC_ADDR);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);

    /*
     * control.IE is 0: inside the FSW's SPW_send critical section
     * (SPW.c:707-731). FSW-style read-modify-write: dmaControl |= TE.
     */
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN | SPW_DMA_TE);

    v = qtest_readl(qts, SPW_DMAControl);
    g_assert_cmpuint(v & SPW_DMA_PS, ==, SPW_DMA_PS);
    g_assert_cmpuint(v & SPW_DMA_TE, ==, 0);
    g_assert_cmphex(v, ==, SPW_DMA_FSW_OPEN | SPW_DMA_PS);
    /* descriptor EN cleared in guest RAM */
    g_assert_cmpuint(qtest_readl(qts, TXDSC_ADDR) & TXDSC_EN, ==, 0);

    /* IE was 0: the IRQ is latched, not pending */
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT, ==, 0);

    /* a write to another register keeps PS and the latch bits (RD) */
    qtest_writel(qts, SPW_NODEADDR, 0x40);
    g_assert_cmphex(qtest_readl(qts, SPW_DMAControl),
                    ==, SPW_DMA_FSW_OPEN | SPW_DMA_PS);

    /* IE 0->1 pulses exactly the latched IRQ */
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_IE);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT,
                     ==, SPW_IRQ_BIT);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_IE); /* no 0->1 edge */
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT, ==, 0);

    /* ---- a control write with RS drops the latch (SPW_close) ---- */
    qtest_writel(qts, SPW_CONTROL, 0);
    qtest_writel(qts, TXDSC_ADDR, TXDSC_EN | TXDSC_WR | TXDSC_IE);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN | SPW_DMA_TE);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT, ==, 0);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_RS); /* RS clears the latch */
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_IE); /* IE 0->1 */
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT, ==, 0);

    /* ---- control.IE already set: the TX IRQ comes directly ---- */
    qtest_writel(qts, TXDSC_ADDR, TXDSC_EN | TXDSC_WR | TXDSC_IE);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN | SPW_DMA_TE);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT,
                     ==, SPW_IRQ_BIT);

    /* ---- TXDSC_IE clear: the TX runs, no IRQ ---- */
    qtest_writel(qts, TXDSC_ADDR, TXDSC_EN | TXDSC_WR);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN | SPW_DMA_PS);
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_PS, ==, 0);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN | SPW_DMA_TE);
    /* the TX ran: descriptor EN cleared, PS set again, no IRQ */
    g_assert_cmpuint(qtest_readl(qts, TXDSC_ADDR) & TXDSC_EN, ==, 0);
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_PS,
                     ==, SPW_DMA_PS);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT, ==, 0);

    /* ---- TI clear: the TX runs, no IRQ; latch bits as written ---- */
    qtest_writel(qts, TXDSC_ADDR, TXDSC_EN | TXDSC_WR | TXDSC_IE);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_PS); /* W1C PS, latch 0 */
    g_assert_cmphex(qtest_readl(qts, SPW_DMAControl), ==, 0);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_TE);
    /* the TX ran: EN cleared, PS set (TI was clear, so no IRQ) */
    g_assert_cmpuint(qtest_readl(qts, TXDSC_ADDR) & TXDSC_EN, ==, 0);
    g_assert_cmphex(qtest_readl(qts, SPW_DMAControl), ==, SPW_DMA_PS);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT, ==, 0);

    /* ---- correction 9: TE clears even with TXDSC_EN clear ---- */
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_PS); /* W1C: clear PS */
    g_assert_cmphex(qtest_readl(qts, SPW_DMAControl), ==, 0);
    qtest_writel(qts, TXDSC_ADDR, 0); /* descriptor not armed */
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_TE);
    g_assert_cmphex(qtest_readl(qts, SPW_DMAControl), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT, ==, 0);

    /*
     * ---- system reset: power-on values, latch cleared ----
     *
     * positive control first: with IE = 0 an armed TX latches (the
     * descriptor EN is cleared, nothing pending) and IE 0->1 gives the
     * IRQ.
     */
    qtest_writel(qts, SPW_CONTROL, 0);
    qtest_writel(qts, TXDSC_ADDR, TXDSC_EN | TXDSC_WR | TXDSC_IE);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN | SPW_DMA_TE);
    g_assert_cmpuint(qtest_readl(qts, TXDSC_ADDR) & TXDSC_EN, ==, 0);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT, ==, 0);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_IE);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT,
                     ==, SPW_IRQ_BIT);

    /* latch again: IE off, armed TX, nothing pending */
    qtest_writel(qts, SPW_CONTROL, 0);
    qtest_writel(qts, TXDSC_ADDR, TXDSC_EN | TXDSC_WR | TXDSC_IE);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN | SPW_DMA_TE);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT, ==, 0);

    qtest_system_reset(qts);

    g_assert_cmphex(qtest_readl(qts, SPW_CONTROL), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SPW_STATUS), ==, 0x00a00000u);
    g_assert_cmphex(qtest_readl(qts, SPW_DMAControl), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAXRX), ==, 0);

    /*
     * the reset cleared dmaTxDescAddr: point it back. The latch must be
     * cleared too, so IE 0->1 gives no IRQ.
     */
    qtest_writel(qts, SPW_DMATxDesc, TXDSC_ADDR);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_IE);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT, ==, 0);

    /*
     * positive control after the reset: an armed TX with IE = 0 latches
     * again and IE 0->1 pulses
     */
    qtest_writel(qts, SPW_CONTROL, 0);
    qtest_writel(qts, TXDSC_ADDR, TXDSC_EN | TXDSC_WR | TXDSC_IE);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN | SPW_DMA_TE);
    g_assert_cmpuint(qtest_readl(qts, TXDSC_ADDR) & TXDSC_EN, ==, 0);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_IE);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT,
                     ==, SPW_IRQ_BIT);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- P2-4: TMTC transport on the main loop, non-blocking TM ---- */

/*
 * The SPW device's QOM path. Since P4-2 (D-P4a) leon3.c adds it as the
 * fixed child /machine/stixspw (before that it was an unattached
 * device[N], found by a type scan). Check the type, so a wrong object
 * at that path cannot pass.
 */
static gchar *stix_spw_path(QTestState *qts)
{
    QDict *resp = qtest_qmp(qts,
                            "{'execute': 'qom-get',"
                            " 'arguments': {'path': '/machine/stixspw',"
                            " 'property': 'type'}}");

    g_assert(qdict_haskey(resp, "return"));
    g_assert_cmpstr(qdict_get_str(resp, "return"), ==, "grlib-stixspw");
    qobject_unref(resp);
    return g_strdup("/machine/stixspw");
}

/*
 * Tolerant qom-get for a bool property: *ok is false when the
 * property is missing (the pre-P2-4 binary).
 */
static bool stix_qom_get_bool(QTestState *qts, const char *path,
                              const char *prop, bool *ok)
{
    QDict *resp = qtest_qmp(qts,
                            "{'execute': 'qom-get',"
                            " 'arguments': {'path': %s, 'property': %s}}",
                            path, prop);
    QBool *b = qobject_to(QBool, qdict_get(resp, "return"));
    bool val = b != NULL && qbool_get_bool(b);

    if (ok != NULL) {
        *ok = b != NULL;
    }
    qobject_unref(resp);
    return val;
}

/* A uint32 counter property, or -1 when the property is unknown. */
static int64_t stix_qom_counter(QTestState *qts, const char *path,
                                const char *prop)
{
    QDict *resp = qtest_qmp(qts,
                            "{'execute': 'qom-get',"
                            " 'arguments': {'path': %s, 'property': %s}}",
                            path, prop);
    QNum *n = qobject_to(QNum, qdict_get(resp, "return"));
    int64_t val = n != NULL ? qnum_get_int(n) : -1;

    qobject_unref(resp);
    return val;
}

/* The tm-dropped counter, or -1 when the property is unknown. */
static int64_t stix_qom_tm_dropped(QTestState *qts, const char *path)
{
    return stix_qom_counter(qts, path, "tm-dropped");
}

/*
 * Connect to the TM server without the handshake: for tests that send
 * the STIX_CONTROL token themselves, split across writes.
 */
static int stix_tc_connect(uint16_t port)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons(port),
    };
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    g_assert_cmpint(fd, >=, 0);
    g_assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    return fd;
}

/*
 * Connect to the TM server and send the ground station's 12-byte
 * STIX_CONTROL handshake.
 */
static int stix_tm_connect(uint16_t port, bool small_rcvbuf)
{
    static const char token[] = "STIX_CONTROL";
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons(port),
    };
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    g_assert_cmpint(fd, >=, 0);
    if (small_rcvbuf) {
        int rcvbuf = 1024;

        g_assert(setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                            &rcvbuf, sizeof(rcvbuf)) == 0);
    }
    g_assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    g_assert_cmpint(send(fd, token, sizeof(token) - 1, 0),
                    ==, sizeof(token) - 1);
    return fd;
}

/*
 * Wait, bounded, until tmtc-connected reaches @want: the QEMU main
 * loop runs freely between qtest commands, and the property flips on
 * the accept (correction 16: wait for the accept before driving a TX)
 * and again on the disconnect, when the accept is re-armed.
 */
static bool stix_wait_tmtc_state(QTestState *qts, const char *path,
                                 bool want, int64_t timeout_us)
{
    gint64 deadline = g_get_monotonic_time() + timeout_us;
    bool saw_property = false;

    while (g_get_monotonic_time() < deadline) {
        bool ok;
        bool connected = stix_qom_get_bool(qts, path, "tmtc-connected", &ok);

        if (ok) {
            saw_property = true;
            if (connected == want) {
                return true;
            }
        }
        g_usleep(20 * 1000);
    }
    g_test_message("tmtc-connected %s within the timeout",
                   saw_property ? "never reached the wanted state"
                                : "is unknown");
    return false;
}

/*
 * Arm the one TX descriptor for a dlen-byte payload at TXDATA_ADDR and
 * trigger the exchange the way spw-tx does. The device clears the
 * descriptor's EN, so every TX re-arms first.
 */
static void stix_drive_tx(QTestState *qts, uint32_t dlen)
{
    qtest_writel(qts, TXDSC_ADDR + 0, TXDSC_EN | TXDSC_WR | TXDSC_IE);
    qtest_writel(qts, TXDSC_ADDR + 4, 0);
    qtest_writel(qts, TXDSC_ADDR + 8, dlen);
    qtest_writel(qts, TXDSC_ADDR + 12, TXDATA_ADDR);
    qtest_writel(qts, SPW_DMATxDesc, TXDSC_ADDR);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_TE | SPW_DMA_TI);
}

/*
 * Read exactly @len bytes off the TM stream, non-blocking and bounded
 * (10 s). Reads never go past @len, so the caller can prove nothing
 * else is on the stream with a follow-up EAGAIN read.
 */
static void stix_tm_read_exact(int fd, uint8_t *buf, size_t len)
{
    size_t got = 0;
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

    while (got < len && g_get_monotonic_time() < deadline) {
        ssize_t n = recv(fd, buf + got, len - got, MSG_DONTWAIT);

        if (n > 0) {
            got += n;
        } else if (n == 0) {
            g_assert_not_reached(); /* the server never closes first */
        } else {
            g_usleep(1000);
        }
    }
    g_assert_cmpuint(got, ==, len);
}

/* A non-blocking read on a drained stream must hit EAGAIN. */
static void stix_tm_read_eagain(int fd, uint8_t *buf, size_t len)
{
    errno = 0;
    g_assert_cmpint(recv(fd, buf, len, MSG_DONTWAIT), ==, -1);
    g_assert_cmpint(errno, ==, EAGAIN);
}

/*
 * TM over TCP: with tmtc-server=on on a private port the test connects,
 * sends the STIX_CONTROL token, waits for the accept, drives one TX and
 * reads exactly the DMA'd bytes back — the wire format is unchanged
 * (raw [SpW header][CCSDS packet], no length prefix, nothing else on
 * the stream: a follow-up non-blocking read must hit EAGAIN). RED on
 * the pre-P2-4 binary: the accept is not observable (no
 * tmtc-connected) and the bounded wait times out.
 */
static void test_tm_tcp(void)
{
    const uint8_t expect[8] = {
        0xaa, 0xbb, 0xcc, 0xdd, 0x11, 0x22, 0x33, 0x44
    };
    g_autofree gchar *extra = NULL;
    g_autofree gchar *path = NULL;
    uint8_t buf[64];
    QTestState *qts;
    int fd, port;

    port = stix_free_tcp_port();
    extra = g_strdup_printf("-global grlib-stixspw.tmtc-server=on "
                            "-global grlib-stixspw.tmtc-port=%d", port);
    qts = stix_qtest_new_extra(extra);
    path = stix_spw_path(qts);

    fd = stix_tm_connect(port, false);
    g_assert(stix_wait_tmtc_state(qts, path, true, 10 * G_TIME_SPAN_SECOND));

    /* drive one TX exactly like spw-tx: 8 payload bytes */
    qtest_writel(qts, TXDATA_ADDR, 0xaabbccddu);
    qtest_writel(qts, TXDATA_ADDR + 4, 0x11223344u);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_IE);
    stix_drive_tx(qts, 8);

    /* read the DMA'd bytes back: non-blocking, bounded */
    stix_tm_read_exact(fd, buf, sizeof(expect));
    g_assert_cmpint(memcmp(buf, expect, sizeof(expect)), ==, 0);

    /* nothing else on the stream: a non-blocking read must hit EAGAIN */
    stix_tm_read_eagain(fd, buf, sizeof(buf));

    close(fd);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * A stalled client must not block the guest or the qtest. The client
 * sets a small SO_RCVBUF and never reads; 2000 x 4 KiB TXes overflow
 * the socket buffers and the 256 KiB out-queue many times over. Every
 * qtest round trip must keep returning — a hang would leave the loop
 * stuck inside one command, and the outer qt.sh timeout bounds that —
 * and tm-dropped must be > 0 afterwards. Every TX carries its number
 * in the first data word (the wire carries the guest's DMA'd bytes
 * verbatim, the mapping test_tm_tcp pins), so the drained stream can
 * be parsed on whole-packet boundaries: only whole 4096-byte packets,
 * strictly increasing numbers, gaps only from drops, and nothing
 * lost silently (received + dropped == sent) — the no-split
 * invariant the GS bridge's CCSDS splitter depends on. RED on the
 * pre-P2-4 binary: tmtc-connected is unknown (the direct probe showed
 * the old transport hanging the whole of QEMU after 8 such TXes).
 */
#define TM_STALLED_PKT 4096
#define TM_STALLED_TXES 2000
#define TM_STALLED_STREAM (4u * 1024 * 1024)
#define TM_STALLED_QUIET_US (500 * 1000)

static void test_tm_stalled_client(void)
{
    g_autofree gchar *extra = NULL;
    g_autofree gchar *path = NULL;
    g_autofree uint8_t *payload = g_malloc(TM_STALLED_PKT);
    g_autofree uint8_t *stream = g_malloc(TM_STALLED_STREAM);
    QTestState *qts;
    gint64 hard_deadline, quiet_deadline;
    size_t total, off, units;
    int64_t dropped;
    int fd, port, i, last_seq;

    port = stix_free_tcp_port();
    extra = g_strdup_printf("-global grlib-stixspw.tmtc-server=on "
                            "-global grlib-stixspw.tmtc-port=%d", port);
    qts = stix_qtest_new_extra(extra);
    path = stix_spw_path(qts);

    fd = stix_tm_connect(port, true);
    g_assert(stix_wait_tmtc_state(qts, path, true, 10 * G_TIME_SPAN_SECOND));

    for (i = 0; i < TM_STALLED_PKT; i++) {
        payload[i] = (uint8_t)(i ^ 0xa5);
    }
    qtest_memwrite(qts, TXDATA_ADDR, payload, TM_STALLED_PKT);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_IE);

    g_test_timer_start();
    for (i = 0; i < TM_STALLED_TXES; i++) {
        /* number the packet: its first data word */
        qtest_writel(qts, TXDATA_ADDR, (uint32_t)i);
        stix_drive_tx(qts, TM_STALLED_PKT);
        /* every round trip must keep returning */
        qtest_readl(qts, SPW_STATUS);
        g_assert_cmpfloat(g_test_timer_elapsed(), <, 60.0);
    }

    dropped = stix_qom_tm_dropped(qts, path);
    g_assert_cmpint(dropped, >=, 0); /* the property exists */
    g_assert_cmpint(dropped, >, 0);  /* and something was dropped */

    /*
     * Drain the stream and parse it on packet boundaries. The queue
     * keeps draining while we read, so a single EAGAIN is not the end
     * yet: stop after a short quiet period instead.
     */
    total = 0;
    quiet_deadline = g_get_monotonic_time() + TM_STALLED_QUIET_US;
    hard_deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    while (total < TM_STALLED_STREAM &&
           g_get_monotonic_time() < hard_deadline &&
           g_get_monotonic_time() < quiet_deadline) {
        ssize_t n = recv(fd, stream + total, TM_STALLED_STREAM - total,
                         MSG_DONTWAIT);

        if (n > 0) {
            total += n;
            quiet_deadline = g_get_monotonic_time() + TM_STALLED_QUIET_US;
        } else if (n == 0) {
            g_assert_not_reached(); /* the server never closes first */
        } else {
            g_usleep(10 * 1000);
        }
    }

    g_assert_cmpuint(total, >, 0);
    /* no split packet on the wire after partial sends / drops */
    g_assert_cmpuint(total % TM_STALLED_PKT, ==, 0);
    units = 0;
    last_seq = -1;
    for (off = 0; off < total; off += TM_STALLED_PKT) {
        const uint8_t *u = stream + off;
        uint32_t seq = ((uint32_t)u[0] << 24) | ((uint32_t)u[1] << 16) |
                       ((uint32_t)u[2] << 8) | u[3];

        /* drops remove whole packets: gaps, never reordering */
        g_assert_cmpint((int)seq, >, last_seq);
        g_assert_cmpuint(seq, <, TM_STALLED_TXES);
        g_assert_cmpint(memcmp(u + 4, payload + 4, TM_STALLED_PKT - 4),
                        ==, 0);
        last_seq = seq;
        units++;
    }
    g_assert_cmpuint(units, >, 0);
    /* nothing lost silently: the rest was dropped and counted */
    g_assert_cmpuint(units + (size_t)dropped, ==, TM_STALLED_TXES);
    g_test_message("tm-stalled-client: %zu whole packets received, "
                   "%" PRId64 " dropped", units, dropped);

    close(fd);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * One client at a time, and the accept is re-armed on disconnect: A
 * connects; B completes its handshake through the backlog and waits
 * there; a TX reaches A only (B's socket stays empty). A disconnects,
 * the transport accepts B and the next TX reaches B byte-exactly,
 * with nothing else on its stream. All waits are bounded. While B
 * waits in the backlog the accept handler must not busy-spin the main
 * loop (the P2-4-review M1): it is unregistered while a client is
 * connected. The spin itself is not observable from a qtest — its RED
 * evidence is a /proc CPU-time probe, kept out of the suite on
 * purpose (see accepted-diffs §17).
 */
static void test_tm_reconnect(void)
{
    const uint8_t expect_a[8] = {
        0xaa, 0xbb, 0xcc, 0xdd, 0x11, 0x22, 0x33, 0x44
    };
    const uint8_t expect_b[8] = {
        0xde, 0xad, 0xbe, 0xef, 0x55, 0x66, 0x77, 0x88
    };
    g_autofree gchar *extra = NULL;
    g_autofree gchar *path = NULL;
    uint8_t buf[64];
    QTestState *qts;
    bool saw_false;
    int fd_a, fd_b, port;

    port = stix_free_tcp_port();
    extra = g_strdup_printf("-global grlib-stixspw.tmtc-server=on "
                            "-global grlib-stixspw.tmtc-port=%d", port);
    qts = stix_qtest_new_extra(extra);
    path = stix_spw_path(qts);

    fd_a = stix_tm_connect(port, false);
    g_assert(stix_wait_tmtc_state(qts, path, true, 10 * G_TIME_SPAN_SECOND));

    /* B's handshake completes in the kernel; B waits in the backlog */
    fd_b = stix_tm_connect(port, false);

    /* one TX: A takes it, B's socket stays empty */
    qtest_writel(qts, TXDATA_ADDR, 0xaabbccddu);
    qtest_writel(qts, TXDATA_ADDR + 4, 0x11223344u);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_IE);
    stix_drive_tx(qts, 8);
    stix_tm_read_exact(fd_a, buf, sizeof(expect_a));
    g_assert_cmpint(memcmp(buf, expect_a, sizeof(expect_a)), ==, 0);
    stix_tm_read_eagain(fd_b, buf, sizeof(buf));

    /* A goes away: the accept is re-armed and takes B from the backlog */
    close(fd_a);
    saw_false = stix_wait_tmtc_state(qts, path, false, G_TIME_SPAN_SECOND);
    g_assert(stix_wait_tmtc_state(qts, path, true, 10 * G_TIME_SPAN_SECOND));
    if (!saw_false) {
        g_test_message("tmtc-connected never showed false: the "
                       "disconnect and the new accept beat one poll "
                       "interval");
    }

    /* the next TX is byte-exact on B, and again nothing else */
    qtest_writel(qts, TXDATA_ADDR, 0xdeadbeefu);
    qtest_writel(qts, TXDATA_ADDR + 4, 0x55667788u);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_IE);
    stix_drive_tx(qts, 8);
    stix_tm_read_exact(fd_b, buf, sizeof(expect_b));
    g_assert_cmpint(memcmp(buf, expect_b, sizeof(expect_b)), ==, 0);
    stix_tm_read_eagain(fd_b, buf, sizeof(buf));

    close(fd_b);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- P2-5: SPW RX descriptor DMA with a no-spill queue (spw-rx) ---- */

#define SPW_DMARxDesc (SPW_BASE + 0x2cu)
#define SPW_DMA_RE (1u << 1)
#define SPW_DMA_RI (1u << 3)
#define SPW_DMA_PR (1u << 6)
#define SPW_DMA_RD (1u << 11)

#define SPW_RXDSC_IE (1u << 27)
#define SPW_RXDSC_WR (1u << 26)
#define SPW_RXDSC_EN (1u << 25)
#define SPW_RXDSC_TR (1u << 31)

#define RXDSC_ADDR  0x40003000u  /* 1 KiB-aligned descriptor table */
#define RXBUF_BASE  0x40004000u
#define RX_BUF_STEP 252u         /* SPWCONTROL_RX_BUF_SIZE spacing */
#define RX_MAXLEN   252u
#define RX_GUARD    0x5au

/*
 * Arm RX descriptor i the way the FSW does (BSW/SPW/SPW.c:413-434):
 * ctrl = IE|EN (plus WR on the last one), daddr = base + i*252.
 * en = false disarms again (the FSW never does that; the tests do).
 */
static void stix_rx_arm(QTestState *qts, int i, bool wrap, bool en)
{
    uint32_t ctrl = SPW_RXDSC_IE | (en ? SPW_RXDSC_EN : 0) |
                    (wrap ? SPW_RXDSC_WR : 0);

    qtest_writel(qts, RXDSC_ADDR + 8u * (uint32_t)i, ctrl);
    qtest_writel(qts, RXDSC_ADDR + 8u * (uint32_t)i + 4u,
                 RXBUF_BASE + RX_BUF_STEP * (uint32_t)i);
}

/*
 * The FSW's whole RX setup: table, 2 descriptors, maxRxLength, IE,
 * dmaControl
 */
static void stix_rx_setup(QTestState *qts)
{
    qtest_writel(qts, SPW_DMARxDesc, RXDSC_ADDR); /* selector -> 0 */
    stix_rx_arm(qts, 0, false, true);
    stix_rx_arm(qts, 1, true, true);
    qtest_writel(qts, SPW_DMAXRX, RX_MAXLEN);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_IE);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN);
}

/* qom-set the qtest-only rx-inject property; false when refused. */
static bool stix_rx_inject(QTestState *qts, const char *path,
                           const uint8_t *data, size_t len)
{
    GString *hex = g_string_new(NULL);
    QDict *resp;
    bool ok;
    size_t i;

    for (i = 0; i < len; i++) {
        g_string_append_printf(hex, "%s%02x", i ? " " : "", data[i]);
    }
    resp = qtest_qmp(qts,
                     "{'execute': 'qom-set',"
                     " 'arguments': {'path': %s, 'property': 'rx-inject',"
                     " 'value': %s}}",
                     path, hex->str);
    ok = qdict_haskey(resp, "return");
    if (!ok) {
        QDict *err = qdict_get_qdict(resp, "error");

        g_test_message("rx-inject refused: %s",
                       err != NULL ? qdict_get_try_str(err, "desc") : "?");
    }
    qobject_unref(resp);
    g_string_free(hex, TRUE);
    return ok;
}

/* The rx-dropped counter, or -1 when the property is unknown. */
static int64_t stix_qom_rx_dropped(QTestState *qts, const char *path)
{
    return stix_qom_counter(qts, path, "rx-dropped");
}

/* The TC framing resync counter, or -1 when the property is unknown. */
static int64_t stix_qom_tc_resync(QTestState *qts, const char *path)
{
    return stix_qom_counter(qts, path, "tc-resync");
}

/*
 * qom-set a bool property; false when refused. (%s inserts a JSON
 * string in libqtest's format, so the boolean itself must be a
 * literal.)
 */
static bool stix_qom_set_bool(QTestState *qts, const char *path,
                              const char *prop, bool value)
{
    QDict *resp = qtest_qmp(qts,
                            value
                            ? "{'execute': 'qom-set',"
                              " 'arguments': {'path': %s, 'property': %s,"
                              " 'value': true}}"
                            : "{'execute': 'qom-set',"
                              " 'arguments': {'path': %s, 'property': %s,"
                              " 'value': false}}",
                            path, prop);
    bool ok = qdict_haskey(resp, "return");

    qobject_unref(resp);
    return ok;
}

/*
 * RX descriptor DMA (P2-5, FSW-RX §2): units injected through the
 * qtest-only rx-inject property are DMA'd into the armed descriptor,
 * the ctrl word comes back with the length (EN cleared, WR/IE kept),
 * PR is set and IRQ 13 fires when RI and the descriptor's IE are set
 * (gated by control.IE per D-M, latched like the TX IRQ). A unit that
 * finds no armed descriptor, or RE or RD clear, waits in the 64-unit
 * no-spill queue (D-L) with RD cleared, and is retried on a
 * dmaControl write with RD — the ISR's post-re-arm write
 * (BSW/SPW/SPW.c:349-350). A 300-byte unit is truncated to
 * dmaMaxRxLength with TR (correction 11). RED on the pre-P2-5
 * binary: rx-inject is unknown.
 */
static void test_spw_rx(void)
{
    /* real TC(17,1) packets (FSW-RX §6): seq 0, 1 and 2 */
    static const uint8_t tc0[16] = {
        0x40, 0x02, 0x00, 0x00, 0x1d, 0xac, 0xc0, 0x00,
        0x00, 0x05, 0x19, 0x11, 0x01, 0x00, 0xd7, 0x7e
    };
    static const uint8_t tc1[16] = {
        0x40, 0x02, 0x00, 0x00, 0x1d, 0xac, 0xc0, 0x01,
        0x00, 0x05, 0x19, 0x11, 0x01, 0x00, 0x6f, 0x1f
    };
    static const uint8_t tc2[16] = {
        0x40, 0x02, 0x00, 0x00, 0x1d, 0xac, 0xc0, 0x02,
        0x00, 0x05, 0x19, 0x11, 0x01, 0x00, 0xb7, 0x9d
    };
    g_autofree gchar *path = NULL;
    g_autofree uint8_t *big = g_malloc(300);
    g_autofree uint8_t *got = g_malloc(2 * RX_BUF_STEP);
    uint8_t small[4];
    QTestState *qts;
    gint64 deadline;
    uint32_t ctrl, dma;
    int64_t dropped;
    int i;

    qts = stix_qtest_new_extra("");
    path = stix_spw_path(qts);

    stix_rx_setup(qts);
    memset(got, RX_GUARD, 2 * RX_BUF_STEP);
    qtest_memwrite(qts, RXBUF_BASE, got, 2 * RX_BUF_STEP);

    /* a. unit 1 lands byte-exact in buffer 0; ctrl, PR and IRQ 13 */
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    g_assert(stix_rx_inject(qts, path, tc0, sizeof(tc0)));

    qtest_memread(qts, RXBUF_BASE, got, sizeof(tc0));
    g_assert_cmpint(memcmp(got, tc0, sizeof(tc0)), ==, 0);
    qtest_memread(qts, RXBUF_BASE + sizeof(tc0), got, 1);
    g_assert_cmpuint(got[0], ==, RX_GUARD); /* nothing beyond the unit */
    ctrl = qtest_readl(qts, RXDSC_ADDR);
    g_assert_cmphex(ctrl, ==, SPW_RXDSC_IE | sizeof(tc0));
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_PR,
                     ==, SPW_DMA_PR);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT,
                     ==, SPW_IRQ_BIT);

    /*
     * b. unit 2 lands in desc1 (the selector was left at 1 by a);
     * the next unit wraps to desc0 only after desc0 is re-armed
     */
    g_assert(stix_rx_inject(qts, path, tc1, sizeof(tc1)));
    qtest_memread(qts, RXBUF_BASE + RX_BUF_STEP, got, sizeof(tc1));
    g_assert_cmpint(memcmp(got, tc1, sizeof(tc1)), ==, 0);
    ctrl = qtest_readl(qts, RXDSC_ADDR + 8u);
    g_assert_cmphex(ctrl,
                    ==, SPW_RXDSC_IE | SPW_RXDSC_WR | sizeof(tc1));

    /* tc2 finds desc0 disarmed: queued, buffer 0 untouched, RD clear */
    g_assert(stix_rx_inject(qts, path, tc2, sizeof(tc2)));
    qtest_memread(qts, RXBUF_BASE, got, sizeof(tc0));
    g_assert_cmpint(memcmp(got, tc0, sizeof(tc0)), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, RXDSC_ADDR) & SPW_RXDSC_EN, ==, 0);
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_RD,
                     ==, 0);
    stix_rx_arm(qts, 0, false, true);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN); /* RD: retry */
    qtest_memread(qts, RXBUF_BASE, got, sizeof(tc2));
    g_assert_cmpint(memcmp(got, tc2, sizeof(tc2)), ==, 0);

    /* c. a 300-byte unit: ctrl = 252|TR, only 252 bytes written */
    qtest_writel(qts, SPW_DMARxDesc, RXDSC_ADDR);
    stix_rx_arm(qts, 0, false, true); /* desc1 stays disarmed */
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN);
    for (i = 0; i < 300; i++) {
        big[i] = (uint8_t)(i ^ 0x3c);
    }
    memset(got, RX_GUARD, 48);
    qtest_memwrite(qts, RXBUF_BASE + RX_BUF_STEP, got, 48);
    g_assert(stix_rx_inject(qts, path, big, 300));

    ctrl = qtest_readl(qts, RXDSC_ADDR);
    g_assert_cmphex(ctrl, ==, SPW_RXDSC_IE | SPW_RXDSC_TR | RX_MAXLEN);
    qtest_memread(qts, RXBUF_BASE, got, RX_MAXLEN);
    g_assert_cmpint(memcmp(got, big, RX_MAXLEN), ==, 0);
    qtest_memread(qts, RXBUF_BASE + RX_BUF_STEP, got, 48);
    for (i = 0; i < 48; i++) {
        g_assert_cmpuint(got[i], ==, RX_GUARD);
    }

    /*
     * d. both descriptors unarmed: queued, no guest write, RD cleared
     * only because a unit is pending (correction 10); a re-arm plus a
     * dmaControl write with RD delivers it
     */
    qtest_writel(qts, SPW_DMARxDesc, RXDSC_ADDR);
    stix_rx_arm(qts, 0, false, false);
    stix_rx_arm(qts, 1, true, false);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN);
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_RD,
                     ==, SPW_DMA_RD); /* nothing pending: RD kept */
    memset(got, RX_GUARD, sizeof(tc2));
    qtest_memwrite(qts, RXBUF_BASE, got, sizeof(tc2));
    g_assert(stix_rx_inject(qts, path, tc2, sizeof(tc2)));
    qtest_memread(qts, RXBUF_BASE, got, sizeof(tc2));
    for (i = 0; i < (int)sizeof(tc2); i++) {
        g_assert_cmpuint(got[i], ==, RX_GUARD);
    }
    ctrl = qtest_readl(qts, RXDSC_ADDR);
    g_assert_cmphex(ctrl, ==, SPW_RXDSC_IE); /* no write-back at all */
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_RD,
                     ==, 0);
    stix_rx_arm(qts, 0, false, true);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN);
    qtest_memread(qts, RXBUF_BASE, got, sizeof(tc2));
    g_assert_cmpint(memcmp(got, tc2, sizeof(tc2)), ==, 0);

    /*
     * e. 65 queued units: rx-dropped = 1, the first 64 delivered in
     * order as the descriptors are re-armed
     */
    qtest_writel(qts, SPW_DMARxDesc, RXDSC_ADDR);
    stix_rx_arm(qts, 0, false, false);
    stix_rx_arm(qts, 1, true, false);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN);
    for (i = 0; i < 65; i++) {
        small[0] = (uint8_t)i;
        small[1] = 0x55;
        small[2] = 0xaa;
        small[3] = 0x33;
        g_assert(stix_rx_inject(qts, path, small, sizeof(small)));
    }
    dropped = stix_qom_rx_dropped(qts, path);
    g_assert_cmpint(dropped, ==, 1);
    for (i = 0; i < 64; i++) {
        uint32_t buf = RXBUF_BASE + RX_BUF_STEP * (uint32_t)(i & 1);

        stix_rx_arm(qts, i & 1, (i & 1) != 0, true);
        qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN);
        qtest_memread(qts, buf, got, sizeof(small));
        g_assert_cmpuint(got[0], ==, (uint8_t)i);
        g_assert_cmpuint(got[1], ==, 0x55);
        g_assert_cmpuint(got[2], ==, 0xaa);
        g_assert_cmpuint(got[3], ==, 0x33);
    }
    g_assert_cmpint(stix_qom_rx_dropped(qts, path), ==, 1);

    /*
     * f. M1 of the P2-5 review: a unit that arrives while
     * control.IE = 0 — the FSW's SPW_send critical section
     * (SPW.c:707-731) — is held in the queue, not delivered: PR set
     * inside the window would be eaten by the FSW's own
     * `dmaControl |= TE` read-modify-write (W1C, SPW.c:725) and the
     * deferred ISR would skip it. The IE 0->1 edge delivers the held
     * unit (PR set, IRQ 13) and then pulses the TX IRQ that the TX in
     * the same window latched.
     */
    qtest_writel(qts, SPW_DMARxDesc, RXDSC_ADDR);
    stix_rx_arm(qts, 0, false, true);
    qtest_writel(qts, SPW_CONTROL, 0); /* IE clear: the SPW_send window */
    /*
     * the FSW's dmaControl value, with the stale PR from the earlier
     * items W1C-cleared in the same write (a PR-only write would also
     * clear the latched RD/RE/RI)
     */
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN | SPW_DMA_PR);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    memset(got, RX_GUARD, sizeof(tc0));
    qtest_memwrite(qts, RXBUF_BASE, got, sizeof(tc0));
    g_assert(stix_rx_inject(qts, path, tc0, sizeof(tc0)));
    /* held: no DMA, no ctrl write-back, no PR, RD untouched */
    ctrl = qtest_readl(qts, RXDSC_ADDR);
    g_assert_cmphex(ctrl, ==, SPW_RXDSC_IE | SPW_RXDSC_EN);
    qtest_memread(qts, RXBUF_BASE, got, sizeof(tc0));
    for (i = 0; i < (int)sizeof(tc0); i++) {
        g_assert_cmpuint(got[i], ==, RX_GUARD);
    }
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_PR,
                     ==, 0);
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_RD,
                     ==, SPW_DMA_RD);

    /* the FSW's SPW_send: a TX runs, then the |= TE RMW */
    qtest_writel(qts, TXDSC_ADDR + 0, TXDSC_EN | TXDSC_WR | TXDSC_IE);
    qtest_writel(qts, TXDSC_ADDR + 4, 0);
    qtest_writel(qts, TXDSC_ADDR + 8, 8);
    qtest_writel(qts, TXDSC_ADDR + 12, TXDATA_ADDR);
    qtest_writel(qts, SPW_DMATxDesc, TXDSC_ADDR);
    dma = qtest_readl(qts, SPW_DMAControl);
    qtest_writel(qts, SPW_DMAControl, dma | SPW_DMA_TE);
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_PS,
                     ==, SPW_DMA_PS); /* the TX ran */
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_PR,
                     ==, 0); /* still held: PR was never set */

    /* the IE 0->1 edge: the held unit is delivered, IRQ 13 fires */
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_IE);
    ctrl = qtest_readl(qts, RXDSC_ADDR);
    g_assert_cmphex(ctrl, ==, SPW_RXDSC_IE | sizeof(tc0));
    qtest_memread(qts, RXBUF_BASE, got, sizeof(tc0));
    g_assert_cmpint(memcmp(got, tc0, sizeof(tc0)), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_PR,
                     ==, SPW_DMA_PR);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT,
                     ==, SPW_IRQ_BIT);

    /*
     * m2 of the P2-5 review: RI and the descriptor's IE only gate the
     * IRQ (the unit is delivered either way); RE gates the delivery
     */
    /* RI = 0: delivered, PR set, no IRQ */
    qtest_writel(qts, SPW_DMARxDesc, RXDSC_ADDR);
    stix_rx_arm(qts, 0, false, true);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN & ~SPW_DMA_RI);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    g_assert(stix_rx_inject(qts, path, tc1, sizeof(tc1)));
    ctrl = qtest_readl(qts, RXDSC_ADDR);
    g_assert_cmphex(ctrl, ==, SPW_RXDSC_IE | sizeof(tc1));
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_PR,
                     ==, SPW_DMA_PR);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT,
                     ==, 0);

    /* descriptor IE = 0: delivered, no IRQ, IE not kept */
    qtest_writel(qts, SPW_DMARxDesc, RXDSC_ADDR);
    qtest_writel(qts, RXDSC_ADDR, SPW_RXDSC_EN); /* armed, no IE */
    qtest_writel(qts, RXDSC_ADDR + 4, RXBUF_BASE);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    g_assert(stix_rx_inject(qts, path, tc2, sizeof(tc2)));
    ctrl = qtest_readl(qts, RXDSC_ADDR);
    g_assert_cmphex(ctrl, ==, sizeof(tc2));
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT,
                     ==, 0);

    /* RE = 0: queued, delivered after RE and RD are written */
    qtest_writel(qts, SPW_DMARxDesc, RXDSC_ADDR);
    stix_rx_arm(qts, 0, false, true);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN & ~SPW_DMA_RE);
    g_assert(stix_rx_inject(qts, path, tc0, sizeof(tc0)));
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_RD,
                     ==, 0); /* queued on a DMA-channel condition */
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN); /* RE + RD */
    qtest_memread(qts, RXBUF_BASE, got, sizeof(tc0));
    g_assert_cmpint(memcmp(got, tc0, sizeof(tc0)), ==, 0);

    /*
     * g. qtest_system_reset clears the queue and the selector. The
     * selector is 1 here (the RE sub-case above delivered into
     * desc0) and shows in the register readback — GRSPW puts the
     * selector in DESCSEL, bits 9:3 of dmaRxDescAddr. tc1 finds desc1
     * disarmed and is queued. After the reset the readback is 0 (base
     * and selector cleared); rewriting the base, re-arming and
     * writing RD delivers nothing (the queued tc1 is gone), and only
     * a fresh unit lands in desc0, byte-exactly.
     */
    g_assert_cmphex(qtest_readl(qts, SPW_DMARxDesc),
                    ==, RXDSC_ADDR | (1u << 3)); /* selector is 1 */
    g_assert(stix_rx_inject(qts, path, tc1, sizeof(tc1))); /* queued */
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_RD,
                     ==, 0);

    qtest_system_reset(qts);
    deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < deadline &&
           qtest_readl(qts, SPW_DMAControl) != 0) {
        g_usleep(20 * 1000);
    }
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SPW_DMARxDesc), ==, 0);

    /* the queue is empty: base, descriptors, RD — and no delivery */
    qtest_writel(qts, SPW_DMARxDesc, RXDSC_ADDR);
    stix_rx_arm(qts, 0, false, true);
    stix_rx_arm(qts, 1, true, true);
    qtest_writel(qts, SPW_DMAXRX, RX_MAXLEN);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_IE);
    memset(got, RX_GUARD, sizeof(tc1));
    qtest_memwrite(qts, RXBUF_BASE, got, sizeof(tc1));
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN); /* RD: retry */
    qtest_memread(qts, RXBUF_BASE, got, sizeof(tc1));
    for (i = 0; i < (int)sizeof(tc1); i++) {
        g_assert_cmpuint(got[i], ==, RX_GUARD); /* nothing delivered */
    }

    /* a fresh unit lands in desc0, byte-exactly */
    g_assert(stix_rx_inject(qts, path, tc0, sizeof(tc0)));
    qtest_memread(qts, RXBUF_BASE, got, sizeof(tc0));
    g_assert_cmpint(memcmp(got, tc0, sizeof(tc0)), ==, 0);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * TC ingress over TCP (P2-6, D-K): after the STIX_CONTROL strip the
 * stream is framed on the CCSDS length — units of 4 + 7 + BE16(bytes
 * 8:10), reassembled across recv chunks and handed to the RX DMA
 * path. seq 0 arrives as token+[2 bytes], [8 bytes], [6 bytes]; then
 * seq 1, one garbage byte and seq 2 arrive in ONE write (exactly one
 * resync, counted in the read-only QOM property tc-resync). Before
 * the third fill the test re-arms desc0 and writes dmaControl with RD
 * (correction 4). RED on the pre-P2-6 binary: no descriptor is filled
 * (the bytes were traced and dropped).
 */
static void test_tc_tcp(void)
{
    static const uint8_t tc0[16] = {
        0x40, 0x02, 0x00, 0x00, 0x1d, 0xac, 0xc0, 0x00,
        0x00, 0x05, 0x19, 0x11, 0x01, 0x00, 0xd7, 0x7e
    };
    static const uint8_t tc1[16] = {
        0x40, 0x02, 0x00, 0x00, 0x1d, 0xac, 0xc0, 0x01,
        0x00, 0x05, 0x19, 0x11, 0x01, 0x00, 0x6f, 0x1f
    };
    static const uint8_t tc2[16] = {
        0x40, 0x02, 0x00, 0x00, 0x1d, 0xac, 0xc0, 0x02,
        0x00, 0x05, 0x19, 0x11, 0x01, 0x00, 0xb7, 0x9d
    };
    static const char token[] = "STIX_CONTROL";
    g_autofree gchar *extra = NULL;
    g_autofree gchar *path = NULL;
    g_autofree uint8_t *got = g_malloc(2 * RX_BUF_STEP);
    uint8_t w1[14], blob[33];
    QTestState *qts;
    gint64 deadline;
    uint32_t ctrl;
    int64_t resync;
    int fd, port;

    port = stix_free_tcp_port();
    extra = g_strdup_printf("-global grlib-stixspw.tmtc-server=on "
                            "-global grlib-stixspw.tmtc-port=%d", port);
    qts = stix_qtest_new_extra(extra);
    path = stix_spw_path(qts);

    stix_rx_setup(qts);
    memset(got, RX_GUARD, 2 * RX_BUF_STEP);
    qtest_memwrite(qts, RXBUF_BASE, got, 2 * RX_BUF_STEP);

    fd = stix_tc_connect(port);
    g_assert(stix_wait_tmtc_state(qts, path, true, 10 * G_TIME_SPAN_SECOND));

    /* seq 0 in three writes, spaced so the chunks arrive apart */
    memcpy(w1, token, sizeof(token) - 1);
    memcpy(w1 + sizeof(token) - 1, tc0, 2);
    g_assert_cmpint(send(fd, w1, sizeof(w1), 0), ==, (int)sizeof(w1));
    g_usleep(50 * 1000);
    g_assert_cmpint(send(fd, tc0 + 2, 8, 0), ==, 8);
    g_usleep(50 * 1000);
    g_assert_cmpint(send(fd, tc0 + 10, 6, 0), ==, 6);

    /* then seq 1, one garbage byte and seq 2 — all in one write */
    memcpy(blob, tc1, sizeof(tc1));
    blob[16] = 0xee;
    memcpy(blob + 17, tc2, sizeof(tc2));
    g_assert_cmpint(send(fd, blob, sizeof(blob), 0), ==, (int)sizeof(blob));

    /* bounded wait for the first two fills */
    deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < deadline &&
           ((qtest_readl(qts, RXDSC_ADDR) & SPW_RXDSC_EN) ||
            (qtest_readl(qts, RXDSC_ADDR + 8u) & SPW_RXDSC_EN))) {
        g_usleep(20 * 1000);
    }
    ctrl = qtest_readl(qts, RXDSC_ADDR);
    g_assert_cmphex(ctrl, ==, SPW_RXDSC_IE | sizeof(tc0));
    qtest_memread(qts, RXBUF_BASE, got, sizeof(tc0));
    g_assert_cmpint(memcmp(got, tc0, sizeof(tc0)), ==, 0);
    ctrl = qtest_readl(qts, RXDSC_ADDR + 8u);
    g_assert_cmphex(ctrl,
                    ==, SPW_RXDSC_IE | SPW_RXDSC_WR | sizeof(tc1));
    qtest_memread(qts, RXBUF_BASE + RX_BUF_STEP, got, sizeof(tc1));
    g_assert_cmpint(memcmp(got, tc1, sizeof(tc1)), ==, 0);

    /* exactly one resync: the 0xEE garbage byte */
    resync = stix_qom_tc_resync(qts, path);
    g_assert_cmpint(resync, ==, 1);

    /*
     * re-arm desc0 and write dmaControl with RD (correction 4): the
     * queued seq 2 is delivered into it
     */
    stix_rx_arm(qts, 0, false, true);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN);
    deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < deadline &&
           (qtest_readl(qts, RXDSC_ADDR) & SPW_RXDSC_EN)) {
        g_usleep(20 * 1000);
    }
    ctrl = qtest_readl(qts, RXDSC_ADDR);
    g_assert_cmphex(ctrl, ==, SPW_RXDSC_IE | sizeof(tc2));
    qtest_memread(qts, RXBUF_BASE, got, sizeof(tc2));
    g_assert_cmpint(memcmp(got, tc2, sizeof(tc2)), ==, 0);

    /* nothing beyond each 16-byte unit was written (m6) */
    qtest_memread(qts, RXBUF_BASE + 16, got, 1);
    g_assert_cmpuint(got[0], ==, RX_GUARD);
    qtest_memread(qts, RXBUF_BASE + RX_BUF_STEP + 16, got, 1);
    g_assert_cmpuint(got[0], ==, RX_GUARD);

    /*
     * m6: a disconnect mid-unit discards the partial unit (12 bytes:
     * the header parsed, the body incomplete); after a reconnect the
     * next whole unit is delivered — the framing state resets on
     * close and on accept, and the dropped half causes no resync
     */
    g_assert_cmpint(send(fd, tc1, 12, 0), ==, 12);
    g_usleep(50 * 1000);
    close(fd);
    g_assert(stix_wait_tmtc_state(qts, path, false, 10 * G_TIME_SPAN_SECOND));
    fd = stix_tc_connect(port);
    g_assert(stix_wait_tmtc_state(qts, path, true, 10 * G_TIME_SPAN_SECOND));

    g_assert_cmpint(send(fd, tc1, sizeof(tc1), 0), ==, (int)sizeof(tc1));
    /* the selector is at 1 and desc1 is disarmed: queue, then re-arm */
    stix_rx_arm(qts, 1, true, true);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN);
    deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < deadline &&
           (qtest_readl(qts, RXDSC_ADDR + 8u) & SPW_RXDSC_EN)) {
        g_usleep(20 * 1000);
    }
    ctrl = qtest_readl(qts, RXDSC_ADDR + 8u);
    g_assert_cmphex(ctrl,
                    ==, SPW_RXDSC_IE | SPW_RXDSC_WR | sizeof(tc1));
    qtest_memread(qts, RXBUF_BASE + RX_BUF_STEP, got, sizeof(tc1));
    g_assert_cmpint(memcmp(got, tc1, sizeof(tc1)), ==, 0);
    qtest_memread(qts, RXBUF_BASE + RX_BUF_STEP + 16, got, 1);
    g_assert_cmpuint(got[0], ==, RX_GUARD);
    resync = stix_qom_tc_resync(qts, path);
    g_assert_cmpint(resync, ==, 1); /* the discarded half: no resync */

    close(fd);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- P2-7: SpW time codes from the CTRL SCET (spw-timecode) ---- */

#define SPW_TIME (SPW_BASE + 0x14u)
#define SPW_STATUS_TO (1u << 0)
#define SPW_CTRL_TQ (1u << 8)
#define SPW_CTRL_TR (1u << 11)
/* the FSW's control value in the golden stdout: NP|TR|LI|TQ|IE|AS */
#define SPW_CTRL_FSW 0x100b0cu

/*
 * Step the virtual clock to just past the next SCET coarse rollover;
 * a 10 ms margin keeps the SCET fine field under 0x1000 (62.5 ms) at
 * fire time, inside D-J's emit window.
 */
static void tc_step_rollover(QTestState *qts)
{
    int64_t ns_into = (int64_t)ctrl_sub(qts) * NSEC_PER_SEC / 65536;

    qtest_clock_step(qts, NSEC_PER_SEC - ns_into + 10 * 1000 * 1000);
}

/*
 * D-J: the SPW generates a time code at each SCET second rollover with
 * TIMECNT = new coarse & 63 (TCTRL 7:6 = 0), only when control.TR is
 * set, only in the first 1/16 s of the second (fine < 0x1000 — a
 * mid-second code would drag the synchronised FSW clock,
 * Timing.c:199-328). It latches status.TO and raises IRQ 13 when
 * control.TQ is set, gated by control.IE with the D-M latch. The SCET
 * comes from the CTRL device (the scet-source link); a guest 0xC8/0xCC
 * commit moves the rollover (correction 6, the commit hook), and the
 * timer survives a system reset (correction 16). RED on the pre-P2-7
 * binary: no TO at the rollover.
 */
static void test_spw_timecode(void)
{
    g_autofree gchar *path = NULL;
    QTestState *qts;
    uint32_t coarse, time;
    bool ok;

    qts = stix_qtest_new();
    path = stix_spw_path(qts);

    /* the FSW's control value (golden stdout) */
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);

    /* a. a rollover emits: TO, TIMECNT = coarse & 63, TCTRL 0, IRQ 13 */
    tc_step_rollover(qts);
    coarse = ctrl_sec(qts);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_TO,
                     ==, SPW_STATUS_TO);
    time = qtest_readl(qts, SPW_TIME);
    g_assert_cmpuint(time & 63u, ==, coarse & 63u);
    g_assert_cmpuint((time >> 6) & 3u, ==, 0);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT,
                     ==, SPW_IRQ_BIT);

    /*
     * b. a TX in between (the spw-tx sequence) does not clear TO;
     * writing 1 to TO does (W1C)
     */
    qtest_writel(qts, TXDATA_ADDR, 0xaabbccddu);
    qtest_writel(qts, TXDATA_ADDR + 4, 0x11223344u);
    qtest_writel(qts, TXDSC_ADDR + 0, TXDSC_EN | TXDSC_WR | TXDSC_IE);
    qtest_writel(qts, TXDSC_ADDR + 4, 0);
    qtest_writel(qts, TXDSC_ADDR + 8, 8);
    qtest_writel(qts, TXDSC_ADDR + 12, TXDATA_ADDR);
    qtest_writel(qts, SPW_DMATxDesc, TXDSC_ADDR);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_TE | SPW_DMA_TI);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_TO,
                     ==, SPW_STATUS_TO);
    qtest_writel(qts, SPW_STATUS, SPW_STATUS_TO);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_TO, ==, 0);

    /*
     * b2. the D-M path for TO (m4): a rollover with control.IE = 0 —
     * the SPW_send window, SPW.c:707-731 — sets TO but no IRQ; the IE
     * 0->1 write pulses the latch and TO is still there for the ISR
     */
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW & ~SPW_CTRL_IE);
    tc_step_rollover(qts);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_TO,
                     ==, SPW_STATUS_TO);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT,
                     ==, 0);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT,
                     ==, SPW_IRQ_BIT);
    qtest_writel(qts, SPW_STATUS, SPW_STATUS_TO);

    /* b3. TQ = 0 (m4): TO set and TIMECNT latched, but no IRQ */
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW & ~SPW_CTRL_TQ);
    tc_step_rollover(qts);
    coarse = ctrl_sec(qts);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_TO,
                     ==, SPW_STATUS_TO);
    time = qtest_readl(qts, SPW_TIME);
    g_assert_cmpuint(time & 63u, ==, coarse & 63u);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT,
                     ==, 0);
    qtest_writel(qts, SPW_STATUS, SPW_STATUS_TO);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW);

    /* c. with control.TR = 0 a rollover gives no TO */
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW & ~SPW_CTRL_TR);
    tc_step_rollover(qts);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_TO, ==, 0);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW);

    /* d. timecodes=false silences the generator; true re-arms it */
    ok = stix_qom_set_bool(qts, path, "timecodes", false);
    g_assert(ok);
    tc_step_rollover(qts);
    tc_step_rollover(qts);
    tc_step_rollover(qts);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_TO, ==, 0);
    ok = stix_qom_set_bool(qts, path, "timecodes", true);
    g_assert(ok);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    tc_step_rollover(qts);
    coarse = ctrl_sec(qts);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_TO,
                     ==, SPW_STATUS_TO);
    time = qtest_readl(qts, SPW_TIME);
    g_assert_cmpuint(time & 63u, ==, coarse & 63u);
    qtest_writel(qts, SPW_STATUS, SPW_STATUS_TO);

    /*
     * e. a guest SCET commit mid-second moves the rollover (correction
     * 6): commit (0x12345678, fine 0x8000) and the next code comes
     * 0.5 s later with the NEW coarse
     */
    ctrl_write(qts, CTRL_LONGTIMERSEC, 0x12345678u);
    ctrl_write(qts, CTRL_LONGTIMERSUBSEC, 0x8000u);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    qtest_clock_step(qts, (int64_t)500 * 1000 * 1000 + 10 * 1000 * 1000);
    coarse = ctrl_sec(qts);
    g_assert_cmpuint(coarse, ==, 0x12345679u);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_TO,
                     ==, SPW_STATUS_TO);
    time = qtest_readl(qts, SPW_TIME);
    g_assert_cmpuint(time & 63u, ==, 0x12345679u & 63u);
    qtest_writel(qts, SPW_STATUS, SPW_STATUS_TO);

    /*
     * f. across a system reset: the timer keeps running (QEMU timers
     * are not deleted and the CTRL keeps its SCET correction), but TR
     * cleared by the reset suppresses the code until the FSW re-writes
     * control at SPW_open. (The reset's own re-arm is belt-and-braces:
     * a surviving timer fires at the same rollover either way, so this
     * case cannot distinguish it — it proves the timer is alive and
     * silent while TR = 0, and emitting again after SPW_open.)
     */
    qtest_system_reset(qts);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    tc_step_rollover(qts);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_TO, ==, 0);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    tc_step_rollover(qts);
    coarse = ctrl_sec(qts);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_TO,
                     ==, SPW_STATUS_TO);
    time = qtest_readl(qts, SPW_TIME);
    g_assert_cmpuint(time & 63u, ==, coarse & 63u);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- P2-8: UDP transport for the ground station (spw-udp) ---- */

#define UDP_TEST_PORT_ENV "STIX_TEST_UDP_PORT"
#define UDP_TEST_HOST_ENV "STIX_TEST_UDP_HOST"

/*
 * Bind a UDP socket on 127.0.0.1:0 and keep it: the returned port stays
 * taken (no TOCTOU) and receives the datagrams.
 */
static int stix_udp_bind_free(int *port)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    socklen_t addrlen = sizeof(addr);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(bind(fd, (struct sockaddr *)&addr, sizeof(addr)), ==, 0);
    g_assert(getsockname(fd, (struct sockaddr *)&addr, &addrlen) == 0);
    *port = ntohs(addr.sin_port);
    g_assert_cmpint(*port, >, 0);
    return fd;
}

/*
 * A free UDP port for QEMU's own bind: bind, read, close (the same
 * release-then-bind window stix_free_tcp_port() accepts), or in a slot
 * the next bindable port of the band B+3..B+99, round-robin.
 */
static int stix_free_udp_port(void)
{
    static unsigned next;
    int base = stix_slot_band_base();
    int port, fd, i;

    for (i = 0; base && i < STIX_SLOT_BAND_PORTS; i++) {
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        };

        port = base + STIX_SLOT_BAND_FIRST + next++ % STIX_SLOT_BAND_PORTS;
        addr.sin_port = htons((uint16_t)port);
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        g_assert_cmpint(fd, >=, 0);
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            close(fd);
            return port;
        }
        close(fd);
    }
    g_assert(!base); /* the whole band busy: a stray process in the slot */

    fd = stix_udp_bind_free(&port);
    close(fd);
    return port;
}

/*
 * Send one datagram from an unrelated ephemeral source port (D-N: the
 * model's TC socket is not connected, so any source must be accepted).
 */
static void stix_udp_send_ephemeral(int port, const void *buf, size_t len)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons((uint16_t)port),
    };
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(sendto(fd, buf, len, 0, (struct sockaddr *)&addr,
                           sizeof(addr)), ==, (int)len);
    close(fd);
}

/*
 * A shared sender socket for a burst. One source port is one flow, and
 * the kernel keeps loopback order within a flow but does NOT order
 * across flows — a fresh socket per datagram (what
 * stix_udp_send_ephemeral opens, and measured here: about 2/3 of
 * three-datagram bursts arrive reordered) must not be used when the
 * test pins the arrival order.
 */
static int stix_udp_sender_open(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    g_assert_cmpint(fd, >=, 0);
    return fd;
}

static void stix_udp_send_on(int fd, int port, const void *buf, size_t len)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons((uint16_t)port),
    };

    g_assert_cmpint(sendto(fd, buf, len, 0, (struct sockaddr *)&addr,
                           sizeof(addr)), ==, (int)len);
}

/*
 * recvfrom one datagram, non-blocking and bounded (10 s). *from_port
 * gets the sender's source port when non-NULL.
 */
static ssize_t stix_udp_recv(int fd, void *buf, size_t len, int *from_port)
{
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    ssize_t n;

    while (g_get_monotonic_time() < deadline) {
        n = recvfrom(fd, buf, len, MSG_DONTWAIT, (struct sockaddr *)&addr,
                     &addrlen);
        if (n >= 0) {
            if (from_port != NULL) {
                *from_port = ntohs(addr.sin_port);
            }
            return n;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            g_assert_cmpint(errno, ==, 0); /* always fails, prints errno */
        }
        g_usleep(1000);
    }
    g_assert_not_reached(); /* no datagram within the timeout */
}

/*
 * Bounded wait (10 s) until the RX descriptor i loses EN, i.e. a unit
 * was delivered into it.
 */
static void spw_udp_wait_rx(QTestState *qts, int i)
{
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    uint32_t addr = RXDSC_ADDR + 8u * (uint32_t)i;

    while (g_get_monotonic_time() < deadline &&
           (qtest_readl(qts, addr) & SPW_RXDSC_EN)) {
        g_usleep(20 * 1000);
    }
}

/*
 * (a) + (b) in one instance (no TCP server: the UDP transport is
 * independent of the tmtc-server OnOffAuto), plus the size bounds and
 * the drain loop:
 * (a) one TX yields exactly one datagram = the DMA'd buffer minus its
 *     4-byte SpW header, first byte CCSDS version 0;
 * (b) a bare TC(17,1) datagram from an unrelated ephemeral source port
 *     fills an armed descriptor with 40 02 00 00 + the SP (len 16),
 *     nothing beyond it;
 * (m6) an undersized datagram (below the 7-byte bare-SP floor) is
 *     dropped, and the next valid one is still delivered; three
 *     datagrams sent back to back from one sender socket come out in
 *     send order through the drain loop and the no-spill queue (the
 *     kernel orders within one flow, not across the fresh-per-datagram
 *     sockets of stix_udp_send_ephemeral). The upper bound is not
 *     testable over IPv4 loopback: a UDP payload above 65507 fails
 *     sendto with EMSGSIZE, and the bound (65542) lies above that.
 */
static void spw_udp_case_tm_tc(void)
{
    /* the DMA'd TX buffer: SpW header 49 02 00 00 + a version-0 SP */
    static const uint8_t tm_full[12] = {
        0x49, 0x02, 0x00, 0x00, 0x0d, 0x5e, 0x5e, 0xc0,
        0x00, 0x03, 0x19, 0x42
    };
    /*
     * TC(17,1) seq 0..2 (FSW-RX §6), each with the SpW header the
     * model adds; the bare datagram is the last 12 bytes
     */
    static const uint8_t tc_unit[3][16] = {
        { 0x40, 0x02, 0x00, 0x00, 0x1d, 0xac, 0xc0, 0x00,
          0x00, 0x05, 0x19, 0x11, 0x01, 0x00, 0xd7, 0x7e },
        { 0x40, 0x02, 0x00, 0x00, 0x1d, 0xac, 0xc0, 0x01,
          0x00, 0x05, 0x19, 0x11, 0x01, 0x00, 0x6f, 0x1f },
        { 0x40, 0x02, 0x00, 0x00, 0x1d, 0xac, 0xc0, 0x02,
          0x00, 0x05, 0x19, 0x11, 0x01, 0x00, 0xb7, 0x9d },
    };
    static const uint8_t short_dg[6] = {
        0x1d, 0xac, 0xc0, 0x00, 0x00, 0x05
    };
    g_autofree gchar *extra = NULL;
    g_autofree gchar *path = NULL;
    g_autofree uint8_t *got = g_malloc(2 * RX_BUF_STEP);
    uint8_t buf[128];
    QTestState *qts;
    ssize_t n;
    int burst_fd, from;
    int tm_fd, tm_port, tc_port;

    tm_fd = stix_udp_bind_free(&tm_port);
    tc_port = stix_free_udp_port();
    extra = g_strdup_printf("-global grlib-stixspw.udp-tm-port=%d "
                            "-global grlib-stixspw.udp-tc-port=%d",
                            tm_port, tc_port);
    qts = stix_qtest_new_extra(extra);
    path = stix_spw_path(qts);

    /* a. one TX, one datagram, the 4-byte SpW header stripped */
    qtest_memwrite(qts, TXDATA_ADDR, tm_full, sizeof(tm_full));
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_IE);
    stix_drive_tx(qts, sizeof(tm_full));

    n = stix_udp_recv(tm_fd, buf, sizeof(buf), &from);
    g_assert_cmpint(n, ==, (ssize_t)sizeof(tm_full) - 4);
    g_assert_cmpint(memcmp(buf, tm_full + 4, n), ==, 0);
    g_assert_cmpuint(buf[0] >> 5, ==, 0); /* CCSDS version 0 */
    g_assert_cmpint(from, >, 0); /* a real source port sent it */
    /* exactly one datagram: a second one never arrives */
    g_usleep(100 * 1000);
    errno = 0;
    g_assert_cmpint(recv(tm_fd, buf, sizeof(buf), MSG_DONTWAIT), ==, -1);
    g_assert_cmpint(errno, ==, EAGAIN);

    /* b. a bare TC datagram from an ephemeral source port fills desc0 */
    stix_rx_setup(qts);
    memset(got, RX_GUARD, 2 * RX_BUF_STEP);
    qtest_memwrite(qts, RXBUF_BASE, got, 2 * RX_BUF_STEP);
    stix_udp_send_ephemeral(tc_port, tc_unit[0] + 4, 12);
    spw_udp_wait_rx(qts, 0);
    g_assert_cmphex(qtest_readl(qts, RXDSC_ADDR),
                    ==, SPW_RXDSC_IE | sizeof(tc_unit[0]));
    qtest_memread(qts, RXBUF_BASE, got, sizeof(tc_unit[0]));
    g_assert_cmpint(memcmp(got, tc_unit[0], sizeof(tc_unit[0])), ==, 0);
    qtest_memread(qts, RXBUF_BASE + sizeof(tc_unit[0]), got, 1);
    g_assert_cmpuint(got[0], ==, RX_GUARD); /* nothing beyond the unit */

    /* an undersized datagram is dropped: desc0 and its buffer unchanged */
    stix_udp_send_ephemeral(tc_port, short_dg, sizeof(short_dg));
    g_usleep(100 * 1000);
    g_assert_cmphex(qtest_readl(qts, RXDSC_ADDR),
                    ==, SPW_RXDSC_IE | sizeof(tc_unit[0]));
    qtest_memread(qts, RXBUF_BASE, got, sizeof(tc_unit[0]));
    g_assert_cmpint(memcmp(got, tc_unit[0], sizeof(tc_unit[0])), ==, 0);

    /* and the next valid one is still delivered (into armed desc1) */
    stix_udp_send_ephemeral(tc_port, tc_unit[1] + 4, 12);
    spw_udp_wait_rx(qts, 1);
    g_assert_cmphex(qtest_readl(qts, RXDSC_ADDR + 8u),
                    ==, SPW_RXDSC_IE | SPW_RXDSC_WR | sizeof(tc_unit[1]));
    qtest_memread(qts, RXBUF_BASE + RX_BUF_STEP, got, sizeof(tc_unit[1]));
    g_assert_cmpint(memcmp(got, tc_unit[1], sizeof(tc_unit[1])), ==, 0);

    /*
     * m6: three datagrams sent back to back from ONE sender socket,
     * both descriptors now unarmed — they queue (the drain loop) and
     * come out in send order as the descriptors are re-armed: tc2
     * first, then tc0, then tc1. One socket is one flow: the kernel
     * keeps loopback order within a flow, and does not order across
     * flows (a fresh socket per datagram, as stix_udp_send_ephemeral
     * opens, reorders) — so the burst shares one socket.
     */
    burst_fd = stix_udp_sender_open();
    stix_udp_send_on(burst_fd, tc_port, tc_unit[2] + 4, 12);
    stix_udp_send_on(burst_fd, tc_port, tc_unit[0] + 4, 12);
    stix_udp_send_on(burst_fd, tc_port, tc_unit[1] + 4, 12);
    close(burst_fd);

    stix_rx_arm(qts, 0, false, true);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN); /* RD: retry */
    spw_udp_wait_rx(qts, 0);
    g_assert_cmphex(qtest_readl(qts, RXDSC_ADDR),
                    ==, SPW_RXDSC_IE | sizeof(tc_unit[2]));
    qtest_memread(qts, RXBUF_BASE, got, sizeof(tc_unit[2]));
    g_assert_cmpint(memcmp(got, tc_unit[2], sizeof(tc_unit[2])), ==, 0);
    stix_rx_arm(qts, 1, true, true);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN);
    spw_udp_wait_rx(qts, 1);
    g_assert_cmphex(qtest_readl(qts, RXDSC_ADDR + 8u),
                    ==, SPW_RXDSC_IE | SPW_RXDSC_WR | sizeof(tc_unit[0]));
    qtest_memread(qts, RXBUF_BASE + RX_BUF_STEP, got, sizeof(tc_unit[0]));
    g_assert_cmpint(memcmp(got, tc_unit[0], sizeof(tc_unit[0])), ==, 0);
    stix_rx_arm(qts, 0, false, true);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN);
    spw_udp_wait_rx(qts, 0);
    qtest_memread(qts, RXBUF_BASE, got, sizeof(tc_unit[1]));
    g_assert_cmpint(memcmp(got, tc_unit[1], sizeof(tc_unit[1])), ==, 0);

    qtest_quit(qts);
    stix_qtest_cleanup();
    close(tm_fd);
}

/*
 * (c) udp-spw-header=on: a TM datagram keeps the 4-byte header, a TC
 * datagram passes unchanged, and nothing lands beyond the unit.
 */
static void spw_udp_case_header(void)
{
    static const uint8_t tm_full[12] = {
        0x49, 0x02, 0x00, 0x00, 0x0d, 0x5e, 0x5e, 0xc0,
        0x00, 0x03, 0x19, 0x42
    };
    static const uint8_t tc_unit[16] = {
        0x40, 0x02, 0x00, 0x00, 0x1d, 0xac, 0xc0, 0x00,
        0x00, 0x05, 0x19, 0x11, 0x01, 0x00, 0xd7, 0x7e
    };
    g_autofree gchar *extra = NULL;
    g_autofree uint8_t *got = g_malloc(2 * RX_BUF_STEP);
    uint8_t buf[128];
    QTestState *qts;
    ssize_t n;
    int tm_fd, tm_port, tc_port;

    tm_fd = stix_udp_bind_free(&tm_port);
    tc_port = stix_free_udp_port();
    extra = g_strdup_printf("-global grlib-stixspw.udp-tm-port=%d "
                            "-global grlib-stixspw.udp-tc-port=%d "
                            "-global grlib-stixspw.udp-spw-header=on",
                            tm_port, tc_port);
    qts = stix_qtest_new_extra(extra);

    qtest_memwrite(qts, TXDATA_ADDR, tm_full, sizeof(tm_full));
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_IE);
    stix_drive_tx(qts, sizeof(tm_full));
    n = stix_udp_recv(tm_fd, buf, sizeof(buf), NULL);
    g_assert_cmpint(n, ==, (ssize_t)sizeof(tm_full));
    g_assert_cmpint(memcmp(buf, tm_full, sizeof(tm_full)), ==, 0);

    stix_rx_setup(qts);
    memset(got, RX_GUARD, 2 * RX_BUF_STEP);
    qtest_memwrite(qts, RXBUF_BASE, got, 2 * RX_BUF_STEP);
    /* the datagram carries the SpW header itself: delivered unchanged */
    stix_udp_send_ephemeral(tc_port, tc_unit, sizeof(tc_unit));
    spw_udp_wait_rx(qts, 0);
    g_assert_cmphex(qtest_readl(qts, RXDSC_ADDR),
                    ==, SPW_RXDSC_IE | sizeof(tc_unit));
    qtest_memread(qts, RXBUF_BASE, got, sizeof(tc_unit));
    g_assert_cmpint(memcmp(got, tc_unit, sizeof(tc_unit)), ==, 0);
    qtest_memread(qts, RXBUF_BASE + sizeof(tc_unit), got, 1);
    g_assert_cmpuint(got[0], ==, RX_GUARD); /* nothing beyond the unit */

    qtest_quit(qts);
    stix_qtest_cleanup();
    close(tm_fd);
}

/*
 * (d) TCP and UDP both on: one TX reaches both, TCP raw (the D-K unit)
 * and UDP stripped.
 */
static void spw_udp_case_both(void)
{
    static const uint8_t tm_full[12] = {
        0x49, 0x02, 0x00, 0x00, 0x0d, 0x5e, 0x5e, 0xc0,
        0x00, 0x03, 0x19, 0x42
    };
    g_autofree gchar *extra = NULL;
    g_autofree gchar *path = NULL;
    uint8_t buf[128];
    QTestState *qts;
    ssize_t n;
    int fd, tcp_port, tm_fd, tm_port;

    tcp_port = stix_free_tcp_port();
    tm_fd = stix_udp_bind_free(&tm_port);
    extra = g_strdup_printf("-global grlib-stixspw.tmtc-server=on "
                            "-global grlib-stixspw.tmtc-port=%d "
                            "-global grlib-stixspw.udp-tm-port=%d",
                            tcp_port, tm_port);
    qts = stix_qtest_new_extra(extra);
    path = stix_spw_path(qts);

    /* the TCP client first, so the TX below has somewhere to go */
    fd = stix_tm_connect((uint16_t)tcp_port, false);
    g_assert(stix_wait_tmtc_state(qts, path, true, 10 * G_TIME_SPAN_SECOND));

    qtest_memwrite(qts, TXDATA_ADDR, tm_full, sizeof(tm_full));
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_IE);
    stix_drive_tx(qts, sizeof(tm_full));

    stix_tm_read_exact(fd, buf, sizeof(tm_full)); /* TCP raw */
    g_assert_cmpint(memcmp(buf, tm_full, sizeof(tm_full)), ==, 0);
    stix_tm_read_eagain(fd, buf, sizeof(buf));
    n = stix_udp_recv(tm_fd, buf, sizeof(buf), NULL); /* UDP stripped */
    g_assert_cmpint(n, ==, (ssize_t)sizeof(tm_full) - 4);
    g_assert_cmpint(memcmp(buf, tm_full + 4, n), ==, 0);

    close(fd);
    qtest_quit(qts);
    stix_qtest_cleanup();
    close(tm_fd);
}

/*
 * (f) udp-tm-port with nothing listening: three TXs complete (an
 * unconnected UDP socket never reports the loopback ICMP error, which
 * would only surface on a LATER send — three TXs make a wrongly
 * connected socket fail here), nothing is counted in tm-dropped, QEMU
 * stays alive.
 */
static void spw_udp_case_no_listener(void)
{
    static const uint8_t tm_full[12] = {
        0x49, 0x02, 0x00, 0x00, 0x0d, 0x5e, 0x5e, 0xc0,
        0x00, 0x03, 0x19, 0x42
    };
    g_autofree gchar *extra = NULL;
    g_autofree gchar *path = NULL;
    QTestState *qts;
    int dead_port, i;

    dead_port = stix_free_udp_port(); /* bound, then closed: no listener */
    extra = g_strdup_printf("-global grlib-stixspw.udp-tm-port=%d",
                            dead_port);
    qts = stix_qtest_new_extra(extra);
    path = stix_spw_path(qts);

    qtest_memwrite(qts, TXDATA_ADDR, tm_full, sizeof(tm_full));
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_IE);
    for (i = 0; i < 3; i++) {
        stix_drive_tx(qts, sizeof(tm_full));
        /* let the loopback ICMP be processed between the sends */
        g_usleep(50 * 1000);
    }

    /* every exchange ran to completion */
    g_assert_cmphex(qtest_readl(qts, TXDSC_ADDR) & TXDSC_EN, ==, 0);
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_PS,
                     ==, SPW_DMA_PS);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT,
                     ==, SPW_IRQ_BIT);
    /* QEMU alive; no send was counted (see the accepted-diffs §21) */
    g_assert_cmphex(qtest_readl(qts, SPW_STATUS), ==, 0x00a00000u);
    g_assert_cmpint(stix_qom_tm_dropped(qts, path), ==, 0);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * UDP transport (P2-8, D-B): one bare CCSDS Space Packet per datagram,
 * in both directions; the sub-cases live in spw_udp_case_* (one QEMU
 * instance each, so nothing is reassigned on g_autofree locals).
 * RED on the pre-P2-8 binary: the udp-* -global properties are unknown.
 */
static void test_spw_udp(void)
{
    spw_udp_case_tm_tc();
    spw_udp_case_header();
    spw_udp_case_both();
    spw_udp_case_no_listener();
}

/* g_test_trap_subprocess child: udp-tc-port on an occupied port. */
static void test_spw_udp_bind_error_subprocess(void)
{
    const char *port = g_getenv(UDP_TEST_PORT_ENV);
    const char *flash = g_getenv(TMTC_TEST_FLASH_ENV);
    QTestState *qts;

    g_assert_nonnull(port);
    g_assert_nonnull(flash);

    /* qtest_init aborts: QEMU realize fails on the bind and exits */
    qts = qtest_initf("-M leon3_generic "
                      "-global grlib-stixflash.flash-file=%s "
                      "-global grlib-stixspw.udp-tc-port=%s", flash, port);
    qtest_quit(qts);
}

/*
 * e. udp-tc-port already bound by the test: QEMU fails at realize with
 * the port and strerror in the message (the tmtc-bind-error pattern).
 * RED on the pre-P2-8 binary: the property is unknown, so the stderr
 * text differs.
 */
static void test_spw_udp_bind_error(void)
{
    g_autofree gchar *path = NULL;
    g_autofree gchar *portstr = NULL;
    g_autofree gchar *pat = NULL;
    g_autofree gchar *sub = stix_test_path("spw-udp-bind-error/subprocess");
    int fd = g_file_open_tmp("stix-flash-XXXXXX", &path, NULL);
    int port;
    int held;

    g_assert(fd >= 0);
    g_assert(ftruncate(fd, 1024 * 1024) == 0);

    /* hold a UDP port race-free across the subprocess run */
    held = stix_udp_bind_free(&port);
    g_assert_cmpint(held, >=, 0);

    portstr = g_strdup_printf("%d", port);
    g_setenv(UDP_TEST_PORT_ENV, portstr, TRUE);
    g_setenv(TMTC_TEST_FLASH_ENV, path, TRUE);

    g_test_trap_subprocess(sub, 30 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    /* one glob: the bind message carries loopback, the port and strerror */
    pat = g_strdup_printf("*127.0.0.1:%d*Address already in use*", port);
    g_test_trap_assert_stderr(pat);

    close(held);
    close(fd);
    g_unlink(path);
}

/* g_test_trap_subprocess child: udp-tm-host that is not IPv4. */
static void test_spw_udp_host_error_subprocess(void)
{
    const char *host = g_getenv(UDP_TEST_HOST_ENV);
    const char *flash = g_getenv(TMTC_TEST_FLASH_ENV);
    QTestState *qts;

    g_assert_nonnull(host);
    g_assert_nonnull(flash);

    /* qtest_init aborts: QEMU realize fails on the host and exits */
    qts = qtest_initf("-M leon3_generic "
                      "-global grlib-stixflash.flash-file=%s "
                      "-global grlib-stixspw.udp-tm-host=%s "
                      "-global grlib-stixspw.udp-tm-port=21501",
                      flash, host);
    qtest_quit(qts);
}

/*
 * m6: a udp-tm-host that is not an IPv4 address fails realize with an
 * error naming the value. RED on the pre-P2-8 binary: the property is
 * unknown, so the stderr text differs.
 */
static void test_spw_udp_host_error(void)
{
    g_autofree gchar *path = NULL;
    g_autofree gchar *pat = NULL;
    g_autofree gchar *sub = stix_test_path("spw-udp-host-error/subprocess");
    int fd = g_file_open_tmp("stix-flash-XXXXXX", &path, NULL);

    g_assert(fd >= 0);
    g_assert(ftruncate(fd, 1024 * 1024) == 0);

    g_setenv(UDP_TEST_HOST_ENV, "not-an-ip", TRUE);
    g_setenv(TMTC_TEST_FLASH_ENV, path, TRUE);

    g_test_trap_subprocess(sub, 30 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    pat = g_strdup_printf("*udp-tm-host 'not-an-ip'*IPv4*");
    g_test_trap_assert_stderr(pat);

    close(fd);
    g_unlink(path);
}

/* ---- P3-1: HK ADC returns nominal values (hk-adc) ---- */

#define CTRL_IDEFXCONFIG 0x0cu /* idefxConfig; bits 3:0 = quarter power */

/*
 * One HK ADC conversion: write the channel command to hkAdc and read
 * the register back. The model completes the conversion immediately
 * (the FSW writes the channel and polls READY, HK.c:130-158).
 */
static uint32_t hk_convert(QTestState *qts, uint32_t ch)
{
    ctrl_write(qts, CTRL_HKADC, ch);
    return ctrl_read(qts, CTRL_HKADC);
}

/*
 * The whole hkAdc layout in one check: READY set, ADDR echoing the
 * channel, bits 19:8 zero, VAL = the expected raw value.
 */
static void hk_check(QTestState *qts, uint32_t ch, uint32_t val)
{
    uint32_t reg = hk_convert(qts, ch);

    g_assert_cmpuint(reg & HKADC_READY, ==, HKADC_READY);
    g_assert_cmpuint(reg & HKADC_ADDR_MASK, ==, ch);
    g_assert_cmpuint(reg & HKADC_RESV_MASK, ==, 0);
    g_assert_cmpuint((reg >> HKADC_VAL_SHIFT) & HKADC_VAL_MASK, ==, val);
}

/*
 * P3-1 (D-P3a): an hkAdc write converts immediately and returns the
 * nominal raw value of the addressed channel (recon-hk-adc §4), with
 * the state overrides taken from the CURRENT hkCmdSta and idefxConfig:
 * SPWEN (hkCmdSta bits 4:3) drives SPW_C by line count and SPW0_V /
 * SPW1_V by their own bit, idefxConfig[3:0] gives the DET_C quarter
 * count, LVEN / HV0116EN / HV1732EN the ATT_V and HV lines. Unused
 * channels read 0. The overrides never consult POWERSTAT. With
 * hk-adc=off the legacy behaviour (VAL 0, READY forced) is kept.
 * RED on the pre-P3-1 binary: VAL is 0 everywhere and the property is
 * unknown.
 */
static void test_hk_adc(void)
{
    static const uint32_t spw_c[4] = { 50, 900, 900, 1830 };
    static const struct {
        uint32_t mask;
        uint32_t val;
    } idefx[] = {
        { 0x0, 20 },  { 0x1, 95 },  { 0x3, 170 }, { 0x7, 245 },
        { 0xF, 320 }, { 0x5, 170 }, { 0xA, 170 },
    };
    QTestState *qts;
    uint32_t spwen, i;

    qts = stix_qtest_new();

    /* a. the BSW boot self-test channels (HK.c:34-39) */
    hk_check(qts, 24, 2048);
    hk_check(qts, 28, 3413);
    hk_check(qts, 44, 3958);

    /*
     * a2. the write replaces the whole register, it does not OR: the
     * command's upper bits and any stale VAL must not survive
     */
    ctrl_write(qts, CTRL_HKADC, 0xFFFFFF18u);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKADC),
                    ==, (2048u << HKADC_VAL_SHIFT) | HKADC_READY | 0x18u);

    /* b. one spot check per group */
    hk_check(qts, 0, 700);   /* DPU_1V5_C */
    hk_check(qts, 32, 2450); /* DET_Q2_T */
    hk_check(qts, 60, 2000); /* FPGA_T */
    hk_check(qts, 68, 3125); /* ASP_REF_2V5A */
    hk_check(qts, 71, 1700); /* VSENSA */

    /*
     * c. SPWEN (hkCmdSta bits 4:3): SPW_C follows the line count,
     * SPW0_V / SPW1_V their own bit
     */
    for (spwen = 0; spwen < 4; spwen++) {
        ctrl_write(qts, CTRL_HKCMDSTA, spwen << 3);
        hk_check(qts, 3, spw_c[spwen]);
        hk_check(qts, 48, spwen & 1 ? 3240 : 20);
        hk_check(qts, 52, spwen & 2 ? 3240 : 20);
    }
    ctrl_write(qts, CTRL_HKCMDSTA, 0);

    /*
     * d. idefxConfig[3:0]: DET_C = 20 + 75 * quarters, non-contiguous
     * masks too; a bit above the field does not count
     */
    for (i = 0; i < G_N_ELEMENTS(idefx); i++) {
        ctrl_write(qts, CTRL_IDEFXCONFIG, idefx[i].mask);
        hk_check(qts, 4, idefx[i].val);
    }
    ctrl_write(qts, CTRL_IDEFXCONFIG, 0x10u);
    hk_check(qts, 4, 20);

    /* e. the PSU enable bits */
    ctrl_write(qts, CTRL_HKCMDSTA, 1u); /* LVEN */
    hk_check(qts, 12, 1670);            /* ATT_V on */
    ctrl_write(qts, CTRL_HKCMDSTA, 0);
    hk_check(qts, 12, 10);              /* ATT_V off */
    ctrl_write(qts, CTRL_HKCMDSTA, 1u << 1); /* HV0116EN */
    hk_check(qts, 20, 1000);
    ctrl_write(qts, CTRL_HKCMDSTA, 1u << 2); /* HV1732EN */
    hk_check(qts, 16, 1000);
    ctrl_write(qts, CTRL_HKCMDSTA, 0);
    hk_check(qts, 20, 15);
    hk_check(qts, 16, 16);

    /* f. unused channels read 0, READY still set */
    hk_check(qts, 5, 0);
    hk_check(qts, 100, 0);
    hk_check(qts, 127, 0);

    /*
     * h. the overrides follow the enable bits only: a guest write of
     * ones to POWERSTAT (bits 9:5) does not stick (P4-1, see
     * /sparc/stix/powerstat) and every state-dependent line reads its
     * off value
     */
    ctrl_write(qts, CTRL_HKCMDSTA, 0x3E0u);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKCMDSTA), ==, 0);
    hk_check(qts, 48, 20);
    hk_check(qts, 52, 20);
    hk_check(qts, 12, 10);
    hk_check(qts, 3, 50);
    ctrl_write(qts, CTRL_HKCMDSTA, 0);

    qtest_quit(qts);
    stix_qtest_cleanup();

    /* g. hk-adc=off: the legacy behaviour, VAL 0 with READY set */
    qts = stix_qtest_new_extra("-global grlib-stixctrl.hk-adc=off");
    hk_check(qts, 24, 0);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- P4-1: POWERSTAT (hkCmdSta bits 9:5) ---- */

#define HKCMDSTA_POWERSTAT_SHIFT 5
#define HKCMDSTA_ENABLES_MASK    0x1Fu  /* LVEN, HV0116EN, HV1732EN, SPWEN */

/*
 * P4-1 (D-P4c): POWERSTAT is the PSU power-good (PSU.h:36-43), composed
 * on read from the commanded enables: bit 5 LV, 6 HV01, 7 HV17, 8 SPW0,
 * 9 SPW1 follow hkCmdSta bits 0..4. Guest writes to bits 9:5 are
 * ignored, and the other fields (e.g. HV0116VAL 23:16) stay plain
 * storage. powerstat=off keeps the legacy plain storage for A/B runs.
 * RED on the pre-P4-1 binary: bits 9:5 read back whatever was written
 * and the property is unknown.
 */
static void test_powerstat(void)
{
    QTestState *qts;
    uint32_t bit;

    qts = stix_qtest_new();

    /* reset: nothing commanded, nothing powered */
    g_assert_cmphex(ctrl_read(qts, CTRL_HKCMDSTA), ==, 0);

    /* each enable drives exactly its own status bit */
    for (bit = 0; bit < 5; bit++) {
        ctrl_write(qts, CTRL_HKCMDSTA, 1u << bit);
        g_assert_cmphex(ctrl_read(qts, CTRL_HKCMDSTA), ==,
                        (1u << bit) |
                        (1u << (bit + HKCMDSTA_POWERSTAT_SHIFT)));
    }

    /* all on */
    ctrl_write(qts, CTRL_HKCMDSTA, HKCMDSTA_ENABLES_MASK);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKCMDSTA), ==,
                    HKCMDSTA_ENABLES_MASK |
                    (HKCMDSTA_ENABLES_MASK << HKCMDSTA_POWERSTAT_SHIFT));

    /*
     * written status bits do not stick: the PSU driver writes hkCmdSta
     * read-modify-write (PSU.c:20-90), so a stale status read must not
     * turn a supply "on"
     */
    ctrl_write(qts, CTRL_HKCMDSTA, 0x3E0u | 0x5u);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKCMDSTA), ==,
                    0x5u | (0x5u << HKCMDSTA_POWERSTAT_SHIFT));

    /*
     * the FSW's own pattern, PSU_setLVState(false): read (status bits
     * set), clear LVEN, write the rest back. LV must drop to "off".
     */
    ctrl_write(qts, CTRL_HKCMDSTA, HKCMDSTA_ENABLES_MASK);
    ctrl_write(qts, CTRL_HKCMDSTA, ctrl_read(qts, CTRL_HKCMDSTA) & ~1u);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKCMDSTA), ==,
                    0x1Eu | (0x1Eu << HKCMDSTA_POWERSTAT_SHIFT));

    /* the other fields are plain storage (HV0116VAL 23:16, HV1732VAL 31:24) */
    ctrl_write(qts, CTRL_HKCMDSTA, 0xA5C30000u | 0x2u);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKCMDSTA), ==,
                    0xA5C30000u | 0x2u | (0x2u << HKCMDSTA_POWERSTAT_SHIFT));

    /* switching off clears the status again */
    ctrl_write(qts, CTRL_HKCMDSTA, 0);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKCMDSTA), ==, 0);

    qtest_quit(qts);
    stix_qtest_cleanup();

    /* powerstat=off: the legacy plain storage, nothing synthesised */
    qts = stix_qtest_new_extra("-global grlib-stixctrl.powerstat=off");
    ctrl_write(qts, CTRL_HKCMDSTA, HKCMDSTA_ENABLES_MASK);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKCMDSTA), ==, HKCMDSTA_ENABLES_MASK);
    ctrl_write(qts, CTRL_HKCMDSTA, 0x3E0u);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKCMDSTA), ==, 0x3E0u);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- P4-2: HK ADC injection over QMP (adc-inject[N]) ---- */

#define ADC_INJECT_NONE 0xFFFFFFFFu
#define HK_CHANNELS     128u /* hkAdc ADDR is 7 bits */

/* qom-set adc-inject[ch]; true on success, false on a QMP error */
static bool adc_inject_set(QTestState *qts, uint32_t ch, uint32_t val)
{
    g_autofree gchar *name = g_strdup_printf("adc-inject[%u]", ch);
    QDict *resp = qtest_qmp(qts,
                            "{'execute': 'qom-set',"
                            " 'arguments': {'path': '/machine/stixctrl',"
                            " 'property': %s, 'value': %u}}", name, val);
    bool ok = qdict_haskey(resp, "return");

    qobject_unref(resp);
    return ok;
}

static uint32_t adc_inject_get(QTestState *qts, uint32_t ch)
{
    g_autofree gchar *name = g_strdup_printf("adc-inject[%u]", ch);
    QDict *resp = qtest_qmp(qts,
                            "{'execute': 'qom-get',"
                            " 'arguments': {'path': '/machine/stixctrl',"
                            " 'property': %s}}", name);
    uint32_t val;

    g_assert(qdict_haskey(resp, "return"));
    val = (uint32_t)qdict_get_int(resp, "return");
    qobject_unref(resp);
    return val;
}

/*
 * P4-2 (D-P4a/b): adc-inject[0..127] on /machine/stixctrl override one
 * HK ADC channel at run time. 0xFFFFFFFF = no override (default); any
 * other value outside 0..0xFFF is rejected; an injection beats both the
 * nominal table and the state overrides; a device reset clears all.
 * RED on the pre-P4-2 binary: /machine/stixctrl does not exist.
 */
static void test_adc_inject(void)
{
    QTestState *qts = stix_qtest_new();
    uint32_t ch;

    /* every channel starts without an override */
    for (ch = 0; ch < HK_CHANNELS; ch++) {
        g_assert_cmphex(adc_inject_get(qts, ch), ==, ADC_INJECT_NONE);
    }

    /* an injection replaces the nominal value; the others are unchanged */
    g_assert_true(adc_inject_set(qts, 24, 1800));
    g_assert_cmphex(adc_inject_get(qts, 24), ==, 1800);
    hk_check(qts, 24, 1800);
    hk_check(qts, 28, 3413);
    hk_check(qts, 44, 3958);

    /* it beats the state overrides (SPW_C follows SPWEN otherwise) */
    ctrl_write(qts, CTRL_HKCMDSTA, 3u << 3);
    hk_check(qts, 3, 1830);
    g_assert_true(adc_inject_set(qts, 3, 777));
    hk_check(qts, 3, 777);
    /* clearing brings the state override back */
    g_assert_true(adc_inject_set(qts, 3, ADC_INJECT_NONE));
    hk_check(qts, 3, 1830);
    g_assert_true(adc_inject_set(qts, 3, 777));
    ctrl_write(qts, CTRL_HKCMDSTA, 0);
    hk_check(qts, 3, 777);

    /* the range ends are accepted */
    g_assert_true(adc_inject_set(qts, 40, 0));
    hk_check(qts, 40, 0);
    g_assert_true(adc_inject_set(qts, 40, 0xFFF));
    hk_check(qts, 40, 0xFFF);

    /* out of range is an error and leaves the old value */
    g_assert_false(adc_inject_set(qts, 40, 0x1000));
    g_assert_false(adc_inject_set(qts, 40, 0xFFFFFFFEu));
    {
        /* -1 is not "none": clearing needs the literal 4294967295 */
        QDict *resp = qtest_qmp(qts,
                                "{'execute': 'qom-set',"
                                " 'arguments': {'path': '/machine/stixctrl',"
                                " 'property': 'adc-inject[40]',"
                                " 'value': -1}}");
        g_assert(qdict_haskey(resp, "error"));
        qobject_unref(resp);
    }
    g_assert_cmphex(adc_inject_get(qts, 40), ==, 0xFFF);
    hk_check(qts, 40, 0xFFF);

    /* clearing restores the nominal value */
    g_assert_true(adc_inject_set(qts, 40, ADC_INJECT_NONE));
    hk_check(qts, 40, 2300);

    /* a reset clears every injection */
    qtest_system_reset(qts);
    for (ch = 0; ch < HK_CHANNELS; ch++) {
        g_assert_cmphex(adc_inject_get(qts, ch), ==, ADC_INJECT_NONE);
    }
    hk_check(qts, 24, 2048);

    /* there is no channel 128 */
    {
        QDict *resp = qtest_qmp(qts,
                                "{'execute': 'qom-get',"
                                " 'arguments': {'path': '/machine/stixctrl',"
                                " 'property': 'adc-inject[128]'}}");
        g_assert(qdict_haskey(resp, "error"));
        qobject_unref(resp);
    }

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- P4-4: HK hardware protection comparator and IRQ (hk-prot) ---- */

#define CTRL_HKIRQSTA         0x90u
#define CTRL_HKPROTCONFIG(i)  (0x94u + 4u * (i))
#define CTRL_UNLOCKINGREG     0xd0u
#define HK_PROT_LINES         9u
#define HK_PROT_UNLOCK        0xA5u
#define HKPROT_EN             (1u << 31)
#define HKPROT_THR_MASK       0xFFFu
#define HKIRQSTA_IRQEN_MASK   0x1FFu
#define HKIRQSTA_PROTSTAT_SH  9u
#define HKIRQSTA_HOTSAMPLE_SH 20u
#define HK_IRQ_BIT            (1u << 4) /* IRQ_STIXCTRL_HK = 4 */
#define HK_PROT_DET           4u        /* line index of DET_C */
#define HK_PROT_DET_BIT       0x10u     /* its IRQEN/PROTSTAT bit */

/* ICD Table 4-75, line order 1V5 2V5 3V3 SPW DET ATT_C ATT_V HV17 HV01 */
static const uint32_t hk_prot_reset_thr[HK_PROT_LINES] = {
    2282, 3416, 3826, 3826, 546, 751, 4095, 2323, 2323,
};
/* the HK ADC channel each line watches */
static const uint32_t hk_prot_ch[HK_PROT_LINES] = {
    0, 1, 2, 3, 4, 8, 12, 16, 20,
};

static uint32_t hk_protstat(QTestState *qts)
{
    return (ctrl_read(qts, CTRL_HKIRQSTA) >> HKIRQSTA_PROTSTAT_SH) & 0x1FFu;
}

static uint32_t hk_hotsample(QTestState *qts)
{
    return ctrl_read(qts, CTRL_HKIRQSTA) >> HKIRQSTA_HOTSAMPLE_SH;
}

static bool hk_irq_pending(QTestState *qts)
{
    return qtest_readl(qts, IRQMP_PENDING) & HK_IRQ_BIT;
}

/* the FSW's HK_clearProtectionStatus (HK.c:272-277): tmp | mask, then tmp */
static void hk_protstat_clear(QTestState *qts, uint32_t mask)
{
    uint32_t tmp = ctrl_read(qts, CTRL_HKIRQSTA) &
                   ~(0x1FFu << HKIRQSTA_PROTSTAT_SH);

    ctrl_write(qts, CTRL_HKIRQSTA, tmp | (mask << HKIRQSTA_PROTSTAT_SH));
    ctrl_write(qts, CTRL_HKIRQSTA, tmp);
}

/* an unlocked hkProtConfig write (HK_setProtectionStatusThreshold) */
static void hk_prot_write_unlocked(QTestState *qts, uint32_t i, uint32_t val)
{
    ctrl_write(qts, CTRL_UNLOCKINGREG, HK_PROT_UNLOCK);
    ctrl_write(qts, CTRL_HKPROTCONFIG(i), val);
}

/*
 * P4-4 (D-P4d): the nine HK protection comparators (ICD §4.48-4.52).
 * Line i (selector i + 1) watches channel hk_prot_ch[i] and owns
 * IRQEN/PROTSTAT bit 1 << (8 - i). A line samples at each conversion
 * of its channel and when an adc-inject is set for it; it trips on the
 * second consecutive sample strictly above its threshold, and PROTSTAT
 * sets only with ProtEn. HOTSAMPLE is the tripping sample; IRQEN is
 * R/W, PROTSTAT write-1-to-clear; irq_hk pulses only when a PROTSTAT
 * bit newly sets with its IRQEN set. hkProtConfig is write-protected
 * by the 0xA5 token in UnlockingReg (one write cycle), except setting
 * ProtEn. hk-prot=off keeps the legacy plain storage.
 * RED on the pre-P4-4 binary: the thresholds reset to 0, nothing
 * trips, the lock is not enforced and the property is unknown.
 */
static void test_hk_prot(void)
{
    QTestState *qts = stix_qtest_new();
    uint32_t i, v;

    /* a. reset values */
    g_assert_cmphex(ctrl_read(qts, CTRL_HKIRQSTA), ==, 0);
    for (i = 0; i < HK_PROT_LINES; i++) {
        g_assert_cmphex(ctrl_read(qts, CTRL_HKPROTCONFIG(i)), ==,
                        hk_prot_reset_thr[i]);
    }
    g_assert_cmphex(ctrl_read(qts, CTRL_UNLOCKINGREG), ==, 0);

    /* b. hkIrqSta: IRQEN R/W, PROTSTAT W1C, HOTSAMPLE and 19:18 RO */
    ctrl_write(qts, CTRL_HKIRQSTA, 0xFFFFFFFFu);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKIRQSTA), ==, HKIRQSTA_IRQEN_MASK);
    ctrl_write(qts, CTRL_HKIRQSTA, 0x0A5u);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKIRQSTA), ==, 0x0A5u);
    ctrl_write(qts, CTRL_HKIRQSTA, 0);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKIRQSTA), ==, 0);

    /* c. the unlock rules (DET_C, threshold 546) */
    /* a locked threshold change is ignored */
    ctrl_write(qts, CTRL_HKPROTCONFIG(HK_PROT_DET), 100);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKPROTCONFIG(HK_PROT_DET)), ==, 546);
    /* a wrong token does not unlock */
    ctrl_write(qts, CTRL_UNLOCKINGREG, 0x5A);
    ctrl_write(qts, CTRL_HKPROTCONFIG(HK_PROT_DET), 100);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKPROTCONFIG(HK_PROT_DET)), ==, 546);
    /* an armed token is disarmed by a wrong value */
    ctrl_write(qts, CTRL_UNLOCKINGREG, HK_PROT_UNLOCK);
    ctrl_write(qts, CTRL_UNLOCKINGREG, 0x5A);
    ctrl_write(qts, CTRL_HKPROTCONFIG(HK_PROT_DET), 100);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKPROTCONFIG(HK_PROT_DET)), ==, 546);
    /* 0xA5 unlocks one write; the token reads back 0 (write-only) */
    ctrl_write(qts, CTRL_UNLOCKINGREG, HK_PROT_UNLOCK);
    g_assert_cmphex(ctrl_read(qts, CTRL_UNLOCKINGREG), ==, 0);
    ctrl_write(qts, CTRL_HKPROTCONFIG(HK_PROT_DET), 100);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKPROTCONFIG(HK_PROT_DET)), ==, 100);
    /* ... and only one */
    ctrl_write(qts, CTRL_HKPROTCONFIG(HK_PROT_DET), 200);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKPROTCONFIG(HK_PROT_DET)), ==, 100);
    /* the token is spent by any write cycle, not only hkProtConfig's */
    ctrl_write(qts, CTRL_UNLOCKINGREG, HK_PROT_UNLOCK);
    ctrl_write(qts, CTRL_MAGICREG, 0);
    ctrl_write(qts, CTRL_HKPROTCONFIG(HK_PROT_DET), 200);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKPROTCONFIG(HK_PROT_DET)), ==, 100);
    /* setting ProtEn needs no token; the locked threshold part is dropped */
    ctrl_write(qts, CTRL_HKPROTCONFIG(HK_PROT_DET), HKPROT_EN | 300);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKPROTCONFIG(HK_PROT_DET)), ==,
                    HKPROT_EN | 100);
    /* clearing ProtEn needs it (HK_setProtectionActionEnable) */
    ctrl_write(qts, CTRL_HKPROTCONFIG(HK_PROT_DET), 100);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKPROTCONFIG(HK_PROT_DET)), ==,
                    HKPROT_EN | 100);
    hk_prot_write_unlocked(qts, HK_PROT_DET, 100);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKPROTCONFIG(HK_PROT_DET)), ==, 100);
    /* reserved bits 30:12 read 0 */
    hk_prot_write_unlocked(qts, HK_PROT_DET, 0x7FFFF000u | 546);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKPROTCONFIG(HK_PROT_DET)), ==, 546);

    /* d. the comparator on DET_C (ch 4, threshold 546) */
    ctrl_write(qts, CTRL_HKPROTCONFIG(HK_PROT_DET), HKPROT_EN | 546);
    ctrl_write(qts, CTRL_HKIRQSTA, HK_PROT_DET_BIT);
    qtest_writel(qts, IRQMP_CLEAR, HK_IRQ_BIT);
    /* equal is not above */
    g_assert_true(adc_inject_set(qts, 4, 546));
    hk_check(qts, 4, 546);
    hk_check(qts, 4, 546);
    g_assert_cmphex(hk_protstat(qts), ==, 0);
    g_assert_false(hk_irq_pending(qts));
    /* one high sample (the injection) is not enough ... */
    g_assert_true(adc_inject_set(qts, 4, 547));
    g_assert_cmphex(hk_protstat(qts), ==, 0);
    g_assert_false(hk_irq_pending(qts));
    /* ... the second consecutive one (the next conversion) trips */
    hk_check(qts, 4, 547);
    g_assert_cmphex(hk_protstat(qts), ==, HK_PROT_DET_BIT);
    g_assert_cmpuint(hk_hotsample(qts), ==, 547);
    g_assert_true(hk_irq_pending(qts));
    g_assert_cmphex(ctrl_read(qts, CTRL_HKIRQSTA) & HKIRQSTA_IRQEN_MASK, ==,
                    HK_PROT_DET_BIT);
    /*
     * writing 0 to PROTSTAT keeps it: HK_setProtectionStatusIRQMask
     * (the ISR's mask write) writes PROTSTAT = 0 before the W1C clear
     */
    ctrl_write(qts, CTRL_HKIRQSTA, HK_PROT_DET_BIT);
    g_assert_cmphex(hk_protstat(qts), ==, HK_PROT_DET_BIT);
    ctrl_write(qts, CTRL_HKIRQSTA, 0);
    g_assert_cmphex(hk_protstat(qts), ==, HK_PROT_DET_BIT);
    g_assert_cmpuint(hk_hotsample(qts), ==, 547);
    ctrl_write(qts, CTRL_HKIRQSTA, HK_PROT_DET_BIT);
    /* a further high sample with the bit still set: no new IRQ */
    qtest_writel(qts, IRQMP_CLEAR, HK_IRQ_BIT);
    hk_check(qts, 4, 547);
    g_assert_cmphex(hk_protstat(qts), ==, HK_PROT_DET_BIT);
    g_assert_false(hk_irq_pending(qts));
    /* the FSW clear: the bit drops, IRQEN and HOTSAMPLE stay */
    hk_protstat_clear(qts, HK_PROT_DET_BIT);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKIRQSTA), ==,
                    (547u << HKIRQSTA_HOTSAMPLE_SH) | HK_PROT_DET_BIT);
    /* still high: the next sample sets it again, a new bit, a new IRQ */
    g_assert_true(adc_inject_set(qts, 4, 600));
    g_assert_cmphex(hk_protstat(qts), ==, HK_PROT_DET_BIT);
    g_assert_cmpuint(hk_hotsample(qts), ==, 600);
    g_assert_true(hk_irq_pending(qts));

    /* a low sample restarts the count */
    hk_protstat_clear(qts, HK_PROT_DET_BIT);
    qtest_writel(qts, IRQMP_CLEAR, HK_IRQ_BIT);
    g_assert_true(adc_inject_set(qts, 4, ADC_INJECT_NONE)); /* 20: low */
    g_assert_true(adc_inject_set(qts, 4, 700));              /* high 1 */
    hk_check(qts, 4, 700);                                   /* high 2 */
    g_assert_cmphex(hk_protstat(qts), ==, HK_PROT_DET_BIT);
    hk_protstat_clear(qts, HK_PROT_DET_BIT);
    hk_check(qts, 4, 700); /* high 3: re-sets */
    hk_protstat_clear(qts, HK_PROT_DET_BIT);
    g_assert_true(adc_inject_set(qts, 4, 546));              /* low */
    g_assert_true(adc_inject_set(qts, 4, 700));              /* high 1 */
    g_assert_cmphex(hk_protstat(qts), ==, 0);
    hk_check(qts, 4, 700);                                   /* high 2 */
    g_assert_cmphex(hk_protstat(qts), ==, HK_PROT_DET_BIT);

    /* IRQEN clear: PROTSTAT and HOTSAMPLE still set, no IRQ */
    ctrl_write(qts, CTRL_HKIRQSTA, 0);
    hk_protstat_clear(qts, HK_PROT_DET_BIT);
    qtest_writel(qts, IRQMP_CLEAR, HK_IRQ_BIT);
    g_assert_true(adc_inject_set(qts, 4, 650));
    g_assert_cmphex(hk_protstat(qts), ==, HK_PROT_DET_BIT);
    g_assert_cmpuint(hk_hotsample(qts), ==, 650);
    g_assert_false(hk_irq_pending(qts));
    /* enabling IRQEN later does not raise one for the old bit */
    ctrl_write(qts, CTRL_HKIRQSTA, HK_PROT_DET_BIT);
    g_assert_false(hk_irq_pending(qts));

    /* ProtEn clear: no PROTSTAT, no IRQ */
    hk_prot_write_unlocked(qts, HK_PROT_DET, 546);
    hk_protstat_clear(qts, HK_PROT_DET_BIT);
    hk_check(qts, 4, 650);
    hk_check(qts, 4, 650);
    g_assert_cmphex(hk_protstat(qts), ==, 0);
    g_assert_false(hk_irq_pending(qts));
    /* the comparator ran meanwhile: re-enabling trips at the next sample */
    ctrl_write(qts, CTRL_HKPROTCONFIG(HK_PROT_DET), HKPROT_EN | 546);
    hk_check(qts, 4, 650);
    g_assert_cmphex(hk_protstat(qts), ==, HK_PROT_DET_BIT);
    g_assert_true(hk_irq_pending(qts));
    g_assert_true(adc_inject_set(qts, 4, ADC_INJECT_NONE));
    hk_protstat_clear(qts, HK_PROT_DET_BIT);

    /* e. every line: its channel, its bit; HOTSAMPLE is shared */
    ctrl_write(qts, CTRL_HKIRQSTA, HKIRQSTA_IRQEN_MASK);
    for (i = 0; i < HK_PROT_LINES; i++) {
        hk_prot_write_unlocked(qts, i, HKPROT_EN | 1500);
    }
    for (i = 0; i < HK_PROT_LINES; i++) {
        uint32_t ch = hk_prot_ch[i];

        qtest_writel(qts, IRQMP_CLEAR, HK_IRQ_BIT);
        v = 1501 + i;
        g_assert_true(adc_inject_set(qts, ch, v));
        hk_check(qts, ch, v);
        g_test_message("line %u ch %u: PROTSTAT 0x%03x", i, ch,
                       hk_protstat(qts));
        g_assert_cmphex(hk_protstat(qts), ==, 1u << (8 - i));
        g_assert_cmpuint(hk_hotsample(qts), ==, v);
        g_assert_true(hk_irq_pending(qts));
        g_assert_true(adc_inject_set(qts, ch, ADC_INJECT_NONE));
        hk_protstat_clear(qts, 1u << (8 - i));
    }
    /* two lines set: both bits, the last sample wins HOTSAMPLE */
    g_assert_true(adc_inject_set(qts, 0, 3000));
    hk_check(qts, 0, 3000);
    g_assert_true(adc_inject_set(qts, 20, 2500));
    hk_check(qts, 20, 2500);
    g_assert_cmphex(hk_protstat(qts), ==, 0x101u);
    g_assert_cmpuint(hk_hotsample(qts), ==, 2500);
    /* an unprotected channel never trips */
    hk_protstat_clear(qts, 0x1FFu);
    g_assert_true(adc_inject_set(qts, 5, 4095));
    hk_check(qts, 5, 4095);
    hk_check(qts, 24, 2048);
    g_assert_true(adc_inject_set(qts, 0, ADC_INJECT_NONE));
    g_assert_true(adc_inject_set(qts, 20, ADC_INJECT_NONE));
    hk_check(qts, 0, 700);
    hk_check(qts, 20, 15);
    g_assert_cmphex(hk_protstat(qts), ==, 0);

    /* f. a reset restores everything; ATT_V's 4095 cannot be exceeded */
    qtest_system_reset(qts);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKIRQSTA), ==, 0);
    for (i = 0; i < HK_PROT_LINES; i++) {
        g_assert_cmphex(ctrl_read(qts, CTRL_HKPROTCONFIG(i)), ==,
                        hk_prot_reset_thr[i]);
    }
    ctrl_write(qts, CTRL_HKPROTCONFIG(6), HKPROT_EN | 4095);
    g_assert_true(adc_inject_set(qts, 12, 4095));
    hk_check(qts, 12, 4095);
    g_assert_cmphex(hk_protstat(qts), ==, 0);
    /* the token does not survive a reset */
    ctrl_write(qts, CTRL_UNLOCKINGREG, HK_PROT_UNLOCK);
    qtest_system_reset(qts);
    ctrl_write(qts, CTRL_HKPROTCONFIG(0), 5);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKPROTCONFIG(0)), ==, 2282);

    qtest_quit(qts);
    stix_qtest_cleanup();

    /* g. hk-prot=off: the legacy plain storage, nothing trips */
    qts = stix_qtest_new_extra("-global grlib-stixctrl.hk-prot=off");
    g_assert_cmphex(ctrl_read(qts, CTRL_HKPROTCONFIG(4)), ==, 0);
    ctrl_write(qts, CTRL_HKPROTCONFIG(4), 0x7FFFFFFFu);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKPROTCONFIG(4)), ==, 0x7FFFFFFFu);
    ctrl_write(qts, CTRL_HKPROTCONFIG(4), HKPROT_EN | 1);
    ctrl_write(qts, CTRL_HKIRQSTA, 0xFFFFFFFFu);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKIRQSTA), ==, 0xFFFFFFFFu);
    ctrl_write(qts, CTRL_HKIRQSTA, HK_PROT_DET_BIT);
    ctrl_write(qts, CTRL_UNLOCKINGREG, HK_PROT_UNLOCK);
    g_assert_cmphex(ctrl_read(qts, CTRL_UNLOCKINGREG), ==, HK_PROT_UNLOCK);
    qtest_writel(qts, IRQMP_CLEAR, HK_IRQ_BIT);
    g_assert_true(adc_inject_set(qts, 4, 600));
    hk_check(qts, 4, 600);
    hk_check(qts, 4, 600);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKIRQSTA), ==, HK_PROT_DET_BIT);
    g_assert_false(hk_irq_pending(qts));
    qtest_quit(qts);
    stix_qtest_cleanup();

    /*
     * h. hk-adc=off: the registers keep their ICD semantics (reset
     * values, lock), but with no ADC model nothing is sampled
     */
    qts = stix_qtest_new_extra("-global grlib-stixctrl.hk-adc=off");
    g_assert_cmphex(ctrl_read(qts, CTRL_HKPROTCONFIG(HK_PROT_DET)), ==, 546);
    ctrl_write(qts, CTRL_HKPROTCONFIG(HK_PROT_DET), HKPROT_EN | 100);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKPROTCONFIG(HK_PROT_DET)), ==,
                    HKPROT_EN | 546);
    ctrl_write(qts, CTRL_HKIRQSTA, HK_PROT_DET_BIT);
    qtest_writel(qts, IRQMP_CLEAR, HK_IRQ_BIT);
    g_assert_true(adc_inject_set(qts, 4, 600));
    hk_check(qts, 4, 0);
    hk_check(qts, 4, 0);
    g_assert_cmphex(ctrl_read(qts, CTRL_HKIRQSTA), ==, HK_PROT_DET_BIT);
    g_assert_false(hk_irq_pending(qts));
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- P4-5: SpW link-down injection (spw-link-down, D-P4e) ---- */

#define SPW_STATUS_DE   (1u << 3)
#define SPW_STATUS_LS_SHIFT 21
#define SPW_LS_READY    2u
#define SPW_LS_RUN      5u
#define SPW_CTRL_LI     (1u << 9)

static uint32_t spw_ls(QTestState *qts)
{
    return (qtest_readl(qts, SPW_STATUS) >> SPW_STATUS_LS_SHIFT) & 7u;
}

static bool spw_irq_pending(QTestState *qts)
{
    return (qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT) != 0;
}

/*
 * D-P4e: the runtime bool link-down (qom-set, like timecodes). While it
 * is true status.LS reads READY (2) — the only value FDIR.c:711 counts
 * — whatever the guest writes to status; DE is set on entry and re-set
 * every 1 s after the ISR's write-1-to-clear, each time with IRQ 13 when
 * control.LI and control.IE are set (the SPW_open value; LI with IE = 0
 * latches per D-M); time codes stop; RX units are held in the no-spill
 * queue and a TX (TE) waits, both completed on the clear (the TX bytes
 * reach the UDP TM port only then; a clear with control.IE = 0 leaves
 * the RX unit held for the IE edge). The clear
 * brings LS back to RUN; time codes resume only if timecodes is true,
 * and link-down never changes the timecodes value. A system reset
 * clears link-down. RED on the pre-P4-5 binary: link-down is unknown.
 */
static void test_spw_link_down(void)
{
    static const uint8_t tc0[16] = {
        0x40, 0x02, 0x00, 0x00, 0x1d, 0xac, 0xc0, 0x00,
        0x00, 0x05, 0x19, 0x11, 0x01, 0x00, 0xd7, 0x7e
    };
    /* the DMA'd TX buffer: SpW header 49 02 00 00 + a version-0 SP */
    static const uint8_t tm_full[12] = {
        0x49, 0x02, 0x00, 0x00, 0x0d, 0x5e, 0x5e, 0xc0,
        0x00, 0x03, 0x19, 0x42
    };
    g_autofree gchar *extra = NULL;
    g_autofree gchar *path = NULL;
    uint8_t got[sizeof(tc0)];
    uint8_t dg[64];
    QTestState *qts;
    int tm_fd, tm_port;
    bool ok, v;

    tm_fd = stix_udp_bind_free(&tm_port);
    extra = g_strdup_printf("-global grlib-stixspw.udp-tm-port=%d",
                            tm_port);
    qts = stix_qtest_new_extra(extra);
    path = stix_spw_path(qts);

    stix_rx_setup(qts);
    memset(got, RX_GUARD, sizeof(got));
    qtest_memwrite(qts, RXBUF_BASE, got, sizeof(got));
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW);

    /* a. the default: up, LS = RUN, no DE */
    v = stix_qom_get_bool(qts, path, "link-down", &ok);
    g_assert_true(ok);
    g_assert_false(v);
    g_assert_cmpuint(spw_ls(qts), ==, SPW_LS_RUN);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_DE, ==, 0);

    /* b. down: LS = READY, DE, IRQ 13 at once */
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    g_assert_true(stix_qom_set_bool(qts, path, "link-down", true));
    g_assert_true(stix_qom_get_bool(qts, path, "link-down", &ok));
    g_assert_cmpuint(spw_ls(qts), ==, SPW_LS_READY);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_DE,
                     ==, SPW_STATUS_DE);
    g_assert_true(spw_irq_pending(qts));
    /* setting it again is no new entry: no IRQ */
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    g_assert_true(stix_qom_set_bool(qts, path, "link-down", true));
    g_assert_false(spw_irq_pending(qts));

    /*
     * c. the ISR's DE clear works (W1C), SPW_open's status clear write
     * (LS field 0, SPW.c:549-558) leaves LS at READY, and 1 s later DE
     * and IRQ 13 are back
     */
    qtest_writel(qts, SPW_STATUS, SPW_STATUS_DE);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_DE, ==, 0);
    qtest_writel(qts, SPW_STATUS, 0x3ffu);
    g_assert_cmpuint(spw_ls(qts), ==, SPW_LS_READY);
    qtest_writel(qts, SPW_STATUS, 0xffffffffu);
    g_assert_cmpuint(spw_ls(qts), ==, SPW_LS_READY);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    qtest_clock_step(qts, NSEC_PER_SEC / 2);
    g_assert_false(spw_irq_pending(qts));
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_DE, ==, 0);
    qtest_clock_step(qts, NSEC_PER_SEC / 2 + 1000);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_DE,
                     ==, SPW_STATUS_DE);
    g_assert_true(spw_irq_pending(qts));
    g_assert_cmpuint(spw_ls(qts), ==, SPW_LS_READY);

    /* d. no time codes while down; the timecodes value is untouched */
    qtest_writel(qts, SPW_STATUS, SPW_STATUS_TO | SPW_STATUS_DE);
    tc_step_rollover(qts);
    tc_step_rollover(qts);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_TO, ==, 0);
    g_assert_true(stix_qom_get_bool(qts, path, "timecodes", &ok));

    /* e. RX is held: buffer untouched, desc0 armed, no PR; RD retries too */
    g_assert(stix_rx_inject(qts, path, tc0, sizeof(tc0)));
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN); /* RD: retry */
    qtest_memread(qts, RXBUF_BASE, got, sizeof(got));
    g_assert_cmpuint(got[0], ==, RX_GUARD);
    g_assert_cmpuint(qtest_readl(qts, RXDSC_ADDR) & SPW_RXDSC_EN,
                     ==, SPW_RXDSC_EN);
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_PR, ==, 0);
    g_assert_cmpint(stix_qom_rx_dropped(qts, path), ==, 0);

    /*
     * f. a TX waits: TE stays, the descriptor stays armed, no PS, no
     * datagram. The descriptor has no IE, so the IRQ at the clear (i)
     * can only come from the RX delivery.
     */
    qtest_memwrite(qts, TXDATA_ADDR, tm_full, sizeof(tm_full));
    qtest_writel(qts, TXDSC_ADDR + 0, TXDSC_EN | TXDSC_WR);
    qtest_writel(qts, TXDSC_ADDR + 4, 0);
    qtest_writel(qts, TXDSC_ADDR + 8, sizeof(tm_full));
    qtest_writel(qts, TXDSC_ADDR + 12, TXDATA_ADDR);
    qtest_writel(qts, SPW_DMATxDesc, TXDSC_ADDR);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN | SPW_DMA_TE);
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) &
                     (SPW_DMA_TE | SPW_DMA_PS), ==, SPW_DMA_TE);
    g_assert_cmpuint(qtest_readl(qts, TXDSC_ADDR) & TXDSC_EN, ==, TXDSC_EN);
    g_usleep(100 * 1000);
    errno = 0;
    g_assert_cmpint(recv(tm_fd, dg, sizeof(dg), MSG_DONTWAIT), ==, -1);
    g_assert_cmpint(errno, ==, EAGAIN);

    /* g. LI = 0: DE re-sets, no IRQ */
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW & ~SPW_CTRL_LI);
    qtest_writel(qts, SPW_STATUS, SPW_STATUS_DE);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    qtest_clock_step(qts, NSEC_PER_SEC + 1000);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_DE,
                     ==, SPW_STATUS_DE);
    g_assert_false(spw_irq_pending(qts));

    /*
     * h. LI with IE = 0: no IRQ, latched; the IE 0->1 write pulses it
     * and does not release the held RX unit
     */
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW & ~SPW_CTRL_IE);
    qtest_writel(qts, SPW_STATUS, SPW_STATUS_DE);
    qtest_clock_step(qts, NSEC_PER_SEC + 1000);
    g_assert_false(spw_irq_pending(qts));
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW);
    g_assert_true(spw_irq_pending(qts));
    qtest_memread(qts, RXBUF_BASE, got, sizeof(got));
    g_assert_cmpuint(got[0], ==, RX_GUARD);

    /*
     * i. clear: LS = RUN; the held unit is delivered (PR, IRQ — the TX
     * descriptor has no IE) and the waiting TX completes (PS, EN
     * cleared, its bytes on the UDP TM port); no more DE afterwards
     */
    qtest_writel(qts, SPW_STATUS, SPW_STATUS_DE);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    g_assert_true(stix_qom_set_bool(qts, path, "link-down", false));
    g_assert_false(stix_qom_get_bool(qts, path, "link-down", &ok));
    g_assert_cmpuint(spw_ls(qts), ==, SPW_LS_RUN);
    qtest_memread(qts, RXBUF_BASE, got, sizeof(got));
    g_assert_cmpint(memcmp(got, tc0, sizeof(tc0)), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_PR,
                     ==, SPW_DMA_PR);
    g_assert_true(spw_irq_pending(qts));
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) &
                     (SPW_DMA_TE | SPW_DMA_PS), ==, SPW_DMA_PS);
    g_assert_cmpuint(qtest_readl(qts, TXDSC_ADDR) & TXDSC_EN, ==, 0);
    g_assert_cmpint(stix_udp_recv(tm_fd, dg, sizeof(dg), NULL),
                    ==, (ssize_t)sizeof(tm_full) - 4);
    g_assert_cmpint(memcmp(dg, tm_full + 4, sizeof(tm_full) - 4), ==, 0);
    qtest_clock_step(qts, 2 * NSEC_PER_SEC);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_DE, ==, 0);

    /* j. time codes resume at the next rollover */
    qtest_writel(qts, SPW_STATUS, SPW_STATUS_TO);
    tc_step_rollover(qts);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_TO,
                     ==, SPW_STATUS_TO);
    qtest_writel(qts, SPW_STATUS, SPW_STATUS_TO);

    /*
     * k. timecodes=false, then a link outage: the clear does not
     * re-enable the codes; timecodes=true while down waits for the clear
     */
    g_assert_true(stix_qom_set_bool(qts, path, "timecodes", false));
    g_assert_true(stix_qom_set_bool(qts, path, "link-down", true));
    g_assert_true(stix_qom_set_bool(qts, path, "link-down", false));
    tc_step_rollover(qts);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_TO, ==, 0);
    g_assert_false(stix_qom_get_bool(qts, path, "timecodes", &ok));
    g_assert_true(stix_qom_set_bool(qts, path, "link-down", true));
    g_assert_true(stix_qom_set_bool(qts, path, "timecodes", true));
    tc_step_rollover(qts);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_TO, ==, 0);
    g_assert_true(stix_qom_set_bool(qts, path, "link-down", false));
    tc_step_rollover(qts);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_TO,
                     ==, SPW_STATUS_TO);

    /*
     * m. a clear while control.IE = 0 (the SPW_send window) keeps the
     * held unit back (M1 of the P2-5 review); the IE edge delivers it
     * with the IRQ
     */
    stix_rx_setup(qts);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN | SPW_DMA_PR);
    memset(got, RX_GUARD, sizeof(got));
    qtest_memwrite(qts, RXBUF_BASE, got, sizeof(got));
    g_assert_true(stix_qom_set_bool(qts, path, "link-down", true));
    g_assert(stix_rx_inject(qts, path, tc0, sizeof(tc0)));
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW & ~SPW_CTRL_IE);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
    g_assert_true(stix_qom_set_bool(qts, path, "link-down", false));
    qtest_memread(qts, RXBUF_BASE, got, sizeof(got));
    g_assert_cmpuint(got[0], ==, RX_GUARD);
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_PR, ==, 0);
    g_assert_false(spw_irq_pending(qts));
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW);
    qtest_memread(qts, RXBUF_BASE, got, sizeof(got));
    g_assert_cmpint(memcmp(got, tc0, sizeof(tc0)), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_PR,
                     ==, SPW_DMA_PR);
    g_assert_true(spw_irq_pending(qts));
    qtest_writel(qts, SPW_STATUS, SPW_STATUS_DE);

    /* l. a system reset clears link-down: LS = RUN, no DE later */
    g_assert_true(stix_qom_set_bool(qts, path, "link-down", true));
    qtest_system_reset(qts);
    g_assert_false(stix_qom_get_bool(qts, path, "link-down", &ok));
    g_assert_true(ok);
    g_assert_cmpuint(spw_ls(qts), ==, SPW_LS_RUN);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW);
    qtest_clock_step(qts, 2 * NSEC_PER_SEC);
    g_assert_cmpuint(qtest_readl(qts, SPW_STATUS) & SPW_STATUS_DE, ==, 0);

    qtest_quit(qts);
    close(tm_fd);
    stix_qtest_cleanup();
}

/*
 * The link-up path runs the RX DMA from QMP, outside any MMIO dispatch.
 * A held unit whose descriptor daddr is this device's dmaControl would
 * write RD there; that nested MMIO write retried the queue while the
 * outer retry still held the head, so the head was delivered and freed
 * twice (a double free, and rx_count wrapped). The device's re-entrancy
 * guard must refuse the nested write, as on the in-MMIO paths, and the
 * queue must drain once, in order. RED on ca01170003: the clear aborts
 * QEMU (glibc's double-free check).
 */
static void test_spw_link_down_reentry(void)
{
    /* dmaControl = RD|NS|AI|RE|TI|RI, big-endian on the bus */
    static const uint8_t rd_word[4] = { 0x00, 0x00, 0x18, 0x1e };
    static const uint8_t tc1[16] = {
        0x40, 0x02, 0x00, 0x00, 0x1d, 0xac, 0xc0, 0x01,
        0x00, 0x05, 0x19, 0x11, 0x01, 0x00, 0x6f, 0x1f
    };
    g_autofree gchar *path = NULL;
    uint8_t got[sizeof(tc1)];
    QTestState *qts;

    qts = stix_qtest_new();
    path = stix_spw_path(qts);

    stix_rx_setup(qts);
    /* desc0 points at the device's own dmaControl */
    qtest_writel(qts, RXDSC_ADDR + 4u, SPW_DMAControl);
    memset(got, RX_GUARD, sizeof(got));
    qtest_memwrite(qts, RXBUF_BASE + RX_BUF_STEP, got, sizeof(got));
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW);

    g_assert_true(stix_qom_set_bool(qts, path, "link-down", true));
    g_assert(stix_rx_inject(qts, path, rd_word, sizeof(rd_word)));
    g_assert(stix_rx_inject(qts, path, tc1, sizeof(tc1)));
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN | SPW_DMA_PR);

    g_assert_true(stix_qom_set_bool(qts, path, "link-down", false));

    /*
     * the device is alive; each unit was delivered once: desc0 took
     * the 4-byte unit (its write to dmaControl refused), desc1 the TC
     */
    g_assert_cmpuint(spw_ls(qts), ==, SPW_LS_RUN);
    g_assert_cmphex(qtest_readl(qts, RXDSC_ADDR),
                    ==, SPW_RXDSC_IE | sizeof(rd_word));
    g_assert_cmphex(qtest_readl(qts, RXDSC_ADDR + 8u),
                    ==, SPW_RXDSC_IE | SPW_RXDSC_WR | sizeof(tc1));
    qtest_memread(qts, RXBUF_BASE + RX_BUF_STEP, got, sizeof(got));
    g_assert_cmpint(memcmp(got, tc1, sizeof(tc1)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SPW_DMAControl),
                    ==, SPW_DMA_FSW_OPEN | SPW_DMA_PR);
    g_assert_cmpint(stix_qom_rx_dropped(qts, path), ==, 0);

    /* the queue is empty: a re-armed desc0 gets nothing on a retry */
    stix_rx_arm(qts, 0, false, true);
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_FSW_OPEN | SPW_DMA_PR);
    g_assert_cmpuint(qtest_readl(qts, RXDSC_ADDR) & SPW_RXDSC_EN,
                     ==, SPW_RXDSC_EN);
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_PR, ==, 0);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- Detector FSM, QL swap and IRQ 3 (detector plan D-1, tier 0) ---- */

#define CTRL_IDEFXCMDSTA     0x00u
#define CTRL_TESTPULSECMDSTA 0x10u
#define CTRL_FSMCMD          0x14u
#define CTRL_FSMSTA          0x18u
#define CTRL_FSMQLCONFIG     0x20u
#define CTRL_FSMEVENTS       0x30u
#define CTRL_FSMRESULT1      0x3cu
#define CTRL_FSMRESULT2      0x40u
#define CTRL_FSMCALCOUNT     0x48u
#define CTRL_FSMCALRES       0x4cu
#define CTRL_DET_LAST        0x4cu

/* fsmCmd (StixIdpu.h:785-825): strobes 8:0, configuration 12:9, 31:16 */
#define FSMCMD_SETENABLE     (1u << 0)
#define FSMCMD_CLEARENABLE   (1u << 1)
#define FSMCMD_RESET         (1u << 2)
#define FSMCMD_SETCALENABLE  (1u << 5)
#define FSMCMD_CALCLEAR      (1u << 6)
#define FSMCMD_QLCLEAR       (1u << 7)
#define FSMCMD_OVERFLOWCLEAR (1u << 8)
#define FSMCMD_QLIRQEN       (1u << 11)
#define FSMCMD_FIFOIRQEN     (1u << 12)
#define FSMCMD_CONFIG_MASK   0xFFFF1E00u

/* fsmSta (StixIdpu.h:890-912) */
#define FSMSTA_ENABLE        (1u << 0)
#define FSMSTA_ACTIVEQL(v)   (((v) >> 2) & 3u)
#define FSMSTA_QLREADY       (1u << 5)
#define FSMSTA_CALENABLE     (1u << 7)

#define TESTPULSE_BUSY       (1u << 16)

#define DET_IRQ_BIT          (1u << 3) /* IRQ_STIXCTRL_DETCTRL = 3 */
#define DET_TICK_NS          (100 * 1000 * 1000LL)

/*
 * QL banks (StixIdpu.h:262-340): LACC n at 0xE0180000 + n * 0x8000
 * (0x6000 used), LTRG n at 0xE0400000 + n * 0x40, LVAR n at
 * 0xE0200000 + n * 0x100 (0xA0 used: 40 slots of u32)
 */
#define QL_LACC(n)           (0xE0180000u + (n) * 0x8000u)
#define QL_LACC_SIZE         0x6000u
#define QL_LTRG(n)           (0xE0400000u + (n) * 0x40u)
#define QL_LTRG_SIZE         0x40u
#define QL_LVAR(n)           (0xE0200000u + (n) * 0x100u)
#define QL_LVAR_SIZE         0xA0u

static uint32_t det_sta(QTestState *qts)
{
    return ctrl_read(qts, CTRL_FSMSTA);
}

/* the FSW's strobe idiom: fsmCmd |= bit (Detectors.c:1093-1124) */
static void det_cmd_or(QTestState *qts, uint32_t bits)
{
    ctrl_write(qts, CTRL_FSMCMD, ctrl_read(qts, CTRL_FSMCMD) | bits);
}

#define CTRL_FSMINTTIME      0x1cu
#define CTRL_FSMINTEVENTS    0x2cu

/*
 * Detectors_setNominalIntParams (Detectors.c:1140-1149): fsmIntTime =
 * min 15:0 | max 31:16 (ticks - 1), fsmIntEvents = the event limit
 */
static void nom_set_params(QTestState *qts, uint32_t time_min,
                           uint32_t time_max, uint32_t events_max)
{
    ctrl_write(qts, CTRL_FSMINTTIME,
               (time_min & 0xFFFFu) | (time_max & 0xFFFFu) << 16);
    ctrl_write(qts, CTRL_FSMINTEVENTS, events_max);
}

/*
 * NOMINAL entry order: the nominal integration parameters
 * (OpModeMgmt.c:625), then SetEnable and Reset (OpModeMgmt.c:669-672)
 */
static void det_start_nom(QTestState *qts, uint32_t ql_int_time,
                          uint32_t time_min, uint32_t time_max,
                          uint32_t events_max)
{
    nom_set_params(qts, time_min, time_max, events_max);
    ctrl_write(qts, CTRL_FSMQLCONFIG, ql_int_time << 16);
    det_cmd_or(qts, FSMCMD_SETENABLE);
    det_cmd_or(qts, FSMCMD_RESET);
}

/*
 * ... with the FSW's NOMINAL-entry parameters 9 / 199 / 1800 (1 s min,
 * 20 s max, 1800 events; ParamMgmtDef.h:382-384), so the D-5 nominal
 * swap rule does not swap (and reset SUMCNT) every tick, as it would
 * with the reset values 0 / 0 / 0
 */
static void det_start(QTestState *qts, uint32_t ql_int_time)
{
    det_start_nom(qts, ql_int_time, 9, 199, 1800);
}

static void det_step_ticks(QTestState *qts, int64_t ticks)
{
    qtest_clock_step(qts, ticks * DET_TICK_NS);
}

static bool det_irq_pending(QTestState *qts)
{
    return qtest_readl(qts, IRQMP_PENDING) & DET_IRQ_BIT;
}

/*
 * D-Dc: fsmCmd bits 8:0 are strobes that act on the write and read 0;
 * the configuration bits round-trip; reserved bits read 0. SetEnable /
 * ClearEnable (wins, freezes), Reset (QlReady and SUMCNT cleared,
 * ActiveQl kept), QlClear; fsmSta, fsmEvents and the result registers
 * are read-only. RED on the pre-D-1 binary: fsmCmd stores the strobes.
 */
static void test_det_fsm_strobes(void)
{
    QTestState *qts = stix_qtest_new();
    uint32_t sta;

    g_assert_cmphex(ctrl_read(qts, CTRL_FSMCMD), ==, 0);
    g_assert_cmphex(det_sta(qts), ==, 0);

    /* config bits round-trip; strobes and reserved 15:13, 4:3 read 0 */
    ctrl_write(qts, CTRL_FSMCMD, 0x1234FE18u | FSMCMD_SETENABLE);
    g_assert_cmphex(ctrl_read(qts, CTRL_FSMCMD), ==, 0x12341E00u);
    g_assert_cmphex(det_sta(qts) & FSMSTA_ENABLE, ==, FSMSTA_ENABLE);

    /* a |= RMW of another strobe does not re-fire an earlier one */
    det_cmd_or(qts, FSMCMD_CLEARENABLE);
    g_assert_cmphex(det_sta(qts) & FSMSTA_ENABLE, ==, 0);
    det_cmd_or(qts, FSMCMD_SETENABLE);
    det_cmd_or(qts, FSMCMD_QLCLEAR);
    g_assert_cmphex(det_sta(qts) & FSMSTA_ENABLE, ==, FSMSTA_ENABLE);
    g_assert_cmphex(ctrl_read(qts, CTRL_FSMCMD), ==, 0x12341E00u);

    /* ClearEnable wins over SetEnable in the same write */
    ctrl_write(qts, CTRL_FSMCMD, FSMCMD_SETENABLE | FSMCMD_CLEARENABLE);
    g_assert_cmphex(det_sta(qts) & FSMSTA_ENABLE, ==, 0);
    g_assert_cmphex(ctrl_read(qts, CTRL_FSMCMD), ==, 0);

    /* 1 s QL period; one swap sets QlReady */
    det_start(qts, 9);
    det_step_ticks(qts, 10);
    sta = det_sta(qts);
    g_assert_cmphex(sta & FSMSTA_QLREADY, ==, FSMSTA_QLREADY);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(sta), ==, 1);
    g_assert_cmphex(ctrl_read(qts, CTRL_FSMEVENTS), ==, 0);

    /*
     * Reset clears QlReady and SUMCNT, keeps Enable and ActiveQl, and
     * restarts the period: the next swap is a full period later
     */
    det_step_ticks(qts, 5);
    det_cmd_or(qts, FSMCMD_RESET);
    sta = det_sta(qts);
    g_assert_cmphex(sta & FSMSTA_QLREADY, ==, 0);
    g_assert_cmphex(sta & FSMSTA_ENABLE, ==, FSMSTA_ENABLE);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(sta), ==, 1);
    g_assert_cmphex(ctrl_read(qts, CTRL_FSMEVENTS), ==, 0);
    det_step_ticks(qts, 9);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 1);
    det_step_ticks(qts, 1);
    sta = det_sta(qts);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(sta), ==, 2);
    g_assert_cmphex(sta & FSMSTA_QLREADY, ==, FSMSTA_QLREADY);

    /* ClearEnable freezes: QlReady, ActiveQl stay; no swap while off */
    det_cmd_or(qts, FSMCMD_CLEARENABLE);
    det_step_ticks(qts, 50);
    sta = det_sta(qts);
    g_assert_cmphex(sta & FSMSTA_ENABLE, ==, 0);
    g_assert_cmphex(sta & FSMSTA_QLREADY, ==, FSMSTA_QLREADY);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(sta), ==, 2);
    g_assert_cmphex(ctrl_read(qts, CTRL_FSMEVENTS), ==, 0);

    /* QlClear clears QlReady */
    det_cmd_or(qts, FSMCMD_QLCLEAR);
    g_assert_cmphex(det_sta(qts) & FSMSTA_QLREADY, ==, 0);

    /* read-only registers ignore writes; results read 0 */
    ctrl_write(qts, CTRL_FSMSTA, 0xFFFFFFFFu);
    g_assert_cmphex(det_sta(qts), ==, 2u << 2);
    ctrl_write(qts, CTRL_FSMEVENTS, 0xFFFFFFFFu);
    g_assert_cmphex(ctrl_read(qts, CTRL_FSMEVENTS), ==, 0);
    ctrl_write(qts, CTRL_FSMRESULT1, 0xFFFFFFFFu);
    ctrl_write(qts, CTRL_FSMRESULT2, 0xFFFFFFFFu);
    ctrl_write(qts, CTRL_FSMCALCOUNT, 0xFFFFFFFFu);
    ctrl_write(qts, CTRL_FSMCALRES, 0xFFFFFFFFu);
    g_assert_cmphex(ctrl_read(qts, CTRL_FSMRESULT1), ==, 0);
    g_assert_cmphex(ctrl_read(qts, CTRL_FSMRESULT2), ==, 0);
    g_assert_cmphex(ctrl_read(qts, CTRL_FSMCALCOUNT), ==, 0);
    g_assert_cmphex(ctrl_read(qts, CTRL_FSMCALRES), ==, 0);

    /* SetCalEnable sets CalEnable (calibration never completes) */
    det_cmd_or(qts, FSMCMD_SETCALENABLE | FSMCMD_CALCLEAR |
               FSMCMD_OVERFLOWCLEAR);
    g_assert_cmphex(det_sta(qts) & FSMSTA_CALENABLE, ==, FSMSTA_CALENABLE);
    g_assert_cmphex(ctrl_read(qts, CTRL_FSMCMD), ==, 0);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * D-Dd: the QL bank switches every (QlIntTime + 1) x 100 ms while
 * Enable = 1, cycling ActiveQl 0..3; QlIntTime is latched at the start
 * of each period. RED on the pre-D-1 binary: ActiveQl never moves.
 */
static void test_det_ql_swap(void)
{
    QTestState *qts = stix_qtest_new();
    int n;

    det_start(qts, 39);
    det_step_ticks(qts, 39);
    g_assert_cmphex(det_sta(qts) & FSMSTA_QLREADY, ==, 0);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 0);
    det_step_ticks(qts, 1);
    g_assert_cmphex(det_sta(qts) & FSMSTA_QLREADY, ==, FSMSTA_QLREADY);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 1);

    /* four swaps wrap to 0 */
    for (n = 2; n <= 4; n++) {
        det_step_ticks(qts, 39);
        g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, (n - 1) & 3);
        det_step_ticks(qts, 1);
        g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, n & 3);
    }

    /* a mid-period change applies at the next period */
    det_step_ticks(qts, 20);
    ctrl_write(qts, CTRL_FSMQLCONFIG, 9u << 16);
    det_step_ticks(qts, 19);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 0);
    det_step_ticks(qts, 1);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 1);

    /* QlIntTime = 9 gives a 1 s period */
    det_step_ticks(qts, 9);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 1);
    det_step_ticks(qts, 1);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 2);

    /* Enable = 0: no swap */
    det_cmd_or(qts, FSMCMD_CLEARENABLE);
    det_step_ticks(qts, 100);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 2);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * D-Dd: each swap pulses IRQ 3 (IRQMP pending bit 3) only with
 * QlIrqEn; no pulse without a swap; QlClear clears QlReady.
 */
static void test_det_ql_irq(void)
{
    QTestState *qts = stix_qtest_new();

    qtest_writel(qts, IRQMP_CLEAR, DET_IRQ_BIT);
    det_start(qts, 9);
    det_step_ticks(qts, 10);
    g_assert_cmphex(det_sta(qts) & FSMSTA_QLREADY, ==, FSMSTA_QLREADY);
    g_assert_false(det_irq_pending(qts));

    /* Detectors_init sets QlIrqEn (Detectors.c:315-327) */
    det_cmd_or(qts, FSMCMD_QLIRQEN | FSMCMD_FIFOIRQEN);
    det_cmd_or(qts, FSMCMD_QLCLEAR);
    g_assert_cmphex(det_sta(qts) & FSMSTA_QLREADY, ==, 0);
    det_step_ticks(qts, 9);
    g_assert_false(det_irq_pending(qts));
    g_assert_cmphex(det_sta(qts) & FSMSTA_QLREADY, ==, 0);
    det_step_ticks(qts, 1);
    g_assert_true(det_irq_pending(qts));
    g_assert_cmphex(det_sta(qts) & FSMSTA_QLREADY, ==, FSMSTA_QLREADY);

    /* the ISR's end: QlClear, then the IRQMP clear (Detectors.c:233) */
    det_cmd_or(qts, FSMCMD_QLCLEAR);
    qtest_writel(qts, IRQMP_CLEAR, DET_IRQ_BIT);
    g_assert_cmphex(det_sta(qts) & FSMSTA_QLREADY, ==, 0);
    det_step_ticks(qts, 9);
    g_assert_false(det_irq_pending(qts));
    det_step_ticks(qts, 1);
    g_assert_true(det_irq_pending(qts));

    /* QlIrqEn cleared again: swaps without a pulse */
    ctrl_write(qts, CTRL_FSMCMD,
               ctrl_read(qts, CTRL_FSMCMD) & ~FSMCMD_QLIRQEN);
    qtest_writel(qts, IRQMP_CLEAR, DET_IRQ_BIT);
    det_step_ticks(qts, 30);
    g_assert_false(det_irq_pending(qts));
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 2);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* bulk fill / check of big-endian u32 words (qtest_memwrite/memread) */
static void det_fill(QTestState *qts, uint32_t addr, uint32_t len,
                     uint32_t val)
{
    g_autofree uint8_t *buf = g_malloc(len);
    uint32_t off;

    for (off = 0; off < len; off += 4) {
        stl_be_p(buf + off, val);
    }
    qtest_memwrite(qts, addr, buf, len);
}

static void det_expect(QTestState *qts, uint32_t addr, uint32_t len,
                       uint32_t val)
{
    g_autofree uint8_t *buf = g_malloc(len);
    uint32_t off;

    qtest_memread(qts, addr, buf, len);
    for (off = 0; off < len; off += 4) {
        uint32_t got = (uint32_t)ldl_be_p(buf + off);

        if (got != val) {
            g_test_message("at 0x%08x", addr + off);
        }
        g_assert_cmphex(got, ==, val);
    }
}

/* every byte outside the four banks' used parts keeps its sentinel */
#define DET_SENTINEL 0xA5A55A5Au
/*
 * just below LACC0 (the ELUT's last words), and the unused 0x2000 of
 * each EACC stride. (Up to D-4 the words at QL_LACC(4) were guarded too;
 * that is EACC0, 0xE01A0000, which the D-5 nominal integration writes.)
 */
#define DET_BELOW_LACC0 (QL_LACC(0) - 0x10u)
#define DET_EACC(n)     (0xE01A0000u + (n) * 0x8000u)
#define DET_EACC_SIZE   0x6000u

static void det_fill_gaps(QTestState *qts)
{
    uint32_t n;

    /*
     * LTRG3 ends at the end of the 0x100 ltrg region, so there is no
     * LTRG word past it to guard
     */
    det_fill(qts, DET_BELOW_LACC0, 0x10u, DET_SENTINEL);
    for (n = 0; n < 2; n++) {
        det_fill(qts, DET_EACC(n) + DET_EACC_SIZE, 0x8000u - DET_EACC_SIZE,
                 DET_SENTINEL);
    }

    for (n = 0; n < 4; n++) {
        det_fill(qts, QL_LVAR(n) + QL_LVAR_SIZE, 0x100u - QL_LVAR_SIZE,
                 DET_SENTINEL);
        det_fill(qts, QL_LACC(n) + QL_LACC_SIZE, 0x8000u - QL_LACC_SIZE,
                 DET_SENTINEL);
    }
}

static void det_expect_gaps(QTestState *qts)
{
    uint32_t n;

    det_expect(qts, DET_BELOW_LACC0, 0x10u, DET_SENTINEL);
    for (n = 0; n < 2; n++) {
        det_expect(qts, DET_EACC(n) + DET_EACC_SIZE, 0x8000u - DET_EACC_SIZE,
                   DET_SENTINEL);
    }

    for (n = 0; n < 4; n++) {
        det_expect(qts, QL_LVAR(n) + QL_LVAR_SIZE, 0x100u - QL_LVAR_SIZE,
                   DET_SENTINEL);
        det_expect(qts, QL_LACC(n) + QL_LACC_SIZE, 0x8000u - QL_LACC_SIZE,
                   DET_SENTINEL);
    }
}

static void det_expect_bank(QTestState *qts, uint32_t n, uint32_t val)
{
    det_expect(qts, QL_LACC(n), QL_LACC_SIZE, val);
    det_expect(qts, QL_LTRG(n), QL_LTRG_SIZE, val);
    det_expect(qts, QL_LVAR(n), QL_LVAR_SIZE, val);
}

static void det_bank_lifecycle(QTestState *qts, uint32_t ql_int_time)
{
    int64_t period = ql_int_time + 1;
    uint32_t n;

    /* garbage in all four banks, sentinels in the gaps */
    for (n = 0; n < 4; n++) {
        det_fill(qts, QL_LACC(n), QL_LACC_SIZE, 0x11110000u | n);
        det_fill(qts, QL_LTRG(n), QL_LTRG_SIZE, 0x22220000u | n);
        det_fill(qts, QL_LVAR(n), QL_LVAR_SIZE, 0x33330000u | n);
    }
    det_fill_gaps(qts);

    ctrl_write(qts, CTRL_FSMQLCONFIG, ql_int_time << 16);
    det_cmd_or(qts, FSMCMD_RESET);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 0);

    det_step_ticks(qts, period);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 1);
    /*
     * the finished bank 0 is written in full (zeros: no count source),
     * the newly active bank 1 is zeroed, banks 2 and 3 are untouched
     */
    det_expect_bank(qts, 0, 0);
    det_expect_bank(qts, 1, 0);
    det_expect(qts, QL_LACC(2), QL_LACC_SIZE, 0x11110002u);
    det_expect(qts, QL_LVAR(3), QL_LVAR_SIZE, 0x33330003u);

    /* a marker in the finished bank (ActiveQl - 1) & 3 = 0 */
    det_fill(qts, QL_LACC(0), QL_LACC_SIZE, 0xCAFE0000u);
    det_fill(qts, QL_LTRG(0), QL_LTRG_SIZE, 0xCAFE0001u);
    det_fill(qts, QL_LVAR(0), QL_LVAR_SIZE, 0xCAFE0002u);

    /* it survives the next two swaps */
    det_step_ticks(qts, period);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 2);
    det_step_ticks(qts, period);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 3);
    det_expect(qts, QL_LACC(0), QL_LACC_SIZE, 0xCAFE0000u);
    det_expect(qts, QL_LTRG(0), QL_LTRG_SIZE, 0xCAFE0001u);
    det_expect(qts, QL_LVAR(0), QL_LVAR_SIZE, 0xCAFE0002u);

    /* and is zeroed on the third, when bank 0 becomes active again */
    det_step_ticks(qts, period - 1);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 3);
    det_expect(qts, QL_LACC(0), QL_LACC_SIZE, 0xCAFE0000u);
    det_step_ticks(qts, 1);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 0);
    det_expect_bank(qts, 0, 0);

    /* WDT UNCHANGED_STORAGE, MemBram and every other gap byte unchanged */
    det_expect_gaps(qts);
    g_assert_cmphex(qtest_readl(qts, AHB_UNCHANGED), ==, DET_SENTINEL);
    g_assert_cmphex(qtest_readl(qts, AHB_MEMBRAM), ==, DET_SENTINEL);
}

static void det_fill_bank(QTestState *qts, uint32_t n, uint32_t val)
{
    det_fill(qts, QL_LACC(n), QL_LACC_SIZE, val);
    det_fill(qts, QL_LTRG(n), QL_LTRG_SIZE, val);
    det_fill(qts, QL_LVAR(n), QL_LVAR_SIZE, val);
}

/*
 * A fresh enable (SetEnable 0 -> 1 at the start of a period, e.g. the
 * first one after a device reset) zeroes the active bank 0, so stale RAM
 * (here: from before the reset) cannot leak into a QL period. With
 * QlIntTime 9 the period writes only LVAR slots 0..9; slots 10..39 of
 * the finished bank are zero only because of that enable-time zeroing.
 * Four periods wrap ActiveQl back to 0.
 */
static void det_fresh_enable(QTestState *qts)
{
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 0);
    det_fill_bank(qts, 0, 0x5717E000u);
    det_fill_gaps(qts);

    ctrl_write(qts, CTRL_FSMQLCONFIG, 9u << 16);
    det_cmd_or(qts, FSMCMD_SETENABLE);
    det_expect_bank(qts, 0, 0);

    det_step_ticks(qts, 10);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 1);
    det_expect_bank(qts, 0, 0);
    det_step_ticks(qts, 30);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 0);
    det_expect_gaps(qts);
}

/*
 * D-Dd bank lifecycle: at a swap the finished bank's LACC and LTRG are
 * written in full, the newly active one zeroed (LVAR 0xA0 only); LVAR is
 * written per tick; after the swap the model leaves the finished bank
 * alone until it is active again, three swaps later. A fresh enable
 * zeroes the active bank. QlIntTime 9, 39 and 99 (a period longer than
 * the 40 LVAR slots) leave the WDT storage (0xE02000A8), MemBram
 * (0xE02003C0) and the words around the LACC banks intact.
 */
static void test_det_ql_banks(void)
{
    QTestState *qts = stix_qtest_new();

    det_fresh_enable(qts);
    det_bank_lifecycle(qts, 39);
    det_bank_lifecycle(qts, 99);

    /* a non-fresh enable (mid-period, after ClearEnable) keeps the bank */
    det_step_ticks(qts, 5);
    det_cmd_or(qts, FSMCMD_CLEARENABLE);
    qtest_writel(qts, QL_LVAR(0), 0x00C0FFEEu);
    det_cmd_or(qts, FSMCMD_SETENABLE);
    g_assert_cmphex(qtest_readl(qts, QL_LVAR(0)), ==, 0x00C0FFEEu);

    /* the first enable after a (watchdog) reset is fresh again */
    qtest_system_reset(qts);
    det_fresh_enable(qts);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * A detector register write spends the hk-prot unlock token like any
 * CTRL write (§28): token, fsmCmd write, then the protected
 * hkProtConfig write stays locked.
 */
static void test_det_token_spend(void)
{
    QTestState *qts = stix_qtest_new();

    ctrl_write(qts, CTRL_UNLOCKINGREG, HK_PROT_UNLOCK);
    ctrl_write(qts, CTRL_FSMCMD, FSMCMD_QLCLEAR);
    ctrl_write(qts, CTRL_HKPROTCONFIG(HK_PROT_DET), 100);
    g_assert_cmpuint(ctrl_read(qts, CTRL_HKPROTCONFIG(HK_PROT_DET)), ==,
                     hk_prot_reset_thr[HK_PROT_DET]);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * D-Dc: testPulseCmdSta Busy (bit 16, active low: 1 = idle,
 * Detectors.c:966-971) reads 1 and QuarterSeu (31:28) reads 0, so the
 * ISR's SEU branch never runs; the other bits are stored.
 */
static void test_det_testpulse(void)
{
    QTestState *qts = stix_qtest_new();

    g_assert_cmphex(ctrl_read(qts, CTRL_TESTPULSECMDSTA), ==, TESTPULSE_BUSY);
    ctrl_write(qts, CTRL_TESTPULSECMDSTA, 0xF0000000u);
    g_assert_cmphex(ctrl_read(qts, CTRL_TESTPULSECMDSTA), ==, TESTPULSE_BUSY);
    ctrl_write(qts, CTRL_TESTPULSECMDSTA, 0x0000FFFFu);
    g_assert_cmphex(ctrl_read(qts, CTRL_TESTPULSECMDSTA), ==, 0x0001FFFFu);
    ctrl_write(qts, CTRL_TESTPULSECMDSTA, 0);
    g_assert_cmphex(ctrl_read(qts, CTRL_TESTPULSECMDSTA), ==, TESTPULSE_BUSY);
    ctrl_write(qts, CTRL_TESTPULSECMDSTA, 0xFFFFFFFFu);
    g_assert_cmphex(ctrl_read(qts, CTRL_TESTPULSECMDSTA), ==, 0x0FFFFFFFu);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * D-Dh: a device reset writes explicit reset values for 0x00-0x4C
 * (ideFxCmdSta CmdReady = 0x80000000 since D-2, testPulseCmdSta Busy,
 * all others 0) and stops the tick. HK DET_C follows the cleared
 * quarter power. RED on the pre-D-1 binary: the registers survive the
 * reset; on the D-1 binary: ideFxCmdSta read 0.
 */
static void test_det_reg_reset(void)
{
    QTestState *qts = stix_qtest_new();
    uint32_t off;

    for (off = 0; off <= CTRL_DET_LAST; off += 4) {
        ctrl_write(qts, off, 0xFFFFFFFFu);
    }
    hk_check(qts, 4, 20 + 4 * 75);
    det_start(qts, 9);
    det_cmd_or(qts, FSMCMD_QLIRQEN);
    det_step_ticks(qts, 5);

    qtest_system_reset(qts);

    for (off = 0; off <= CTRL_DET_LAST; off += 4) {
        uint32_t want = off == CTRL_TESTPULSECMDSTA ? TESTPULSE_BUSY :
                        off == CTRL_IDEFXCMDSTA ? 0x80000000u : 0;

        g_test_message("reg 0x%02x", off);
        g_assert_cmphex(ctrl_read(qts, off), ==, want);
    }
    hk_check(qts, 4, 20);

    /* the tick stopped with the reset: no swap and no IRQ 3 later */
    qtest_writel(qts, IRQMP_CLEAR, DET_IRQ_BIT);
    det_step_ticks(qts, 50);
    g_assert_cmphex(det_sta(qts), ==, 0);
    g_assert_false(det_irq_pending(qts));

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * det-model=off keeps the legacy model: 0x00-0x4C are plain storage,
 * nothing ticks, and the registers survive a reset.
 */
static void test_det_model_off(void)
{
    QTestState *qts =
        stix_qtest_new_extra("-global grlib-stixctrl.det-model=off");

    ctrl_write(qts, CTRL_FSMQLCONFIG, 9u << 16);
    ctrl_write(qts, CTRL_FSMCMD, 0xFFFFFFFFu);
    g_assert_cmphex(ctrl_read(qts, CTRL_FSMCMD), ==, 0xFFFFFFFFu);
    g_assert_cmphex(ctrl_read(qts, CTRL_FSMSTA), ==, 0);
    ctrl_write(qts, CTRL_FSMSTA, 0x5u);
    g_assert_cmphex(ctrl_read(qts, CTRL_FSMSTA), ==, 0x5u);
    g_assert_cmphex(ctrl_read(qts, CTRL_TESTPULSECMDSTA), ==, 0);
    ctrl_write(qts, CTRL_TESTPULSECMDSTA, 0xF0000000u);
    g_assert_cmphex(ctrl_read(qts, CTRL_TESTPULSECMDSTA), ==, 0xF0000000u);
    ctrl_write(qts, CTRL_IDEFXCMDSTA, 0x12345u);

    qtest_writel(qts, IRQMP_CLEAR, DET_IRQ_BIT);
    det_step_ticks(qts, 50);
    g_assert_cmphex(ctrl_read(qts, CTRL_FSMSTA), ==, 0x5u);
    g_assert_false(det_irq_pending(qts));

    qtest_system_reset(qts);
    g_assert_cmphex(ctrl_read(qts, CTRL_FSMCMD), ==, 0xFFFFFFFFu);
    g_assert_cmphex(ctrl_read(qts, CTRL_FSMSTA), ==, 0x5u);
    g_assert_cmphex(ctrl_read(qts, CTRL_IDEFXCMDSTA), ==, 0x12345u);
    g_assert_cmphex(ctrl_read(qts, CTRL_FSMQLCONFIG), ==, 9u << 16);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- IDeF-X interface and register store (detector plan D-2) ---- */

#define CTRL_IDEFXMODEDATA   0x04u
#define CTRL_IDEFXWRITEMASK  0x08u
#define CTRL_IDEFXCONFIG     0x0cu
#define IDEFX_CMDREADY       (1u << 31)

/* IDeF-X register sizes in bits, 1..12 (commonFSW.h:251-262) */
static const uint32_t idefx_reg_bits[13] = {
    0, 2, 3, 192, 32, 4, 2, 5, 2, 2, 3, 3, 32,
};

/*
 * the HW defaults as the FSW reads them back (ParamMgmtDef.h:525-536):
 * one byte for the short registers (after readReg's >> (8 - len)), R4
 * and R12 big-endian u32; R3 is 62 on every channel (checked apart)
 */
static const uint32_t idefx_reg_default[13] = {
    0, 1, 0, 0, 0, 15, 1, 0, 3, 0, 3, 3, 0xFFFFFFFFu,
};

static uint32_t idefx_reg_bytes(uint32_t reg)
{
    return (idefx_reg_bits[reg] + 7) / 8;
}

/*
 * One word through Detectors_transmission (Detectors.c:677-724):
 * WAIT_FOR_READY, the word (CmdCounter 19:16, CmdData 15:0, GrpAddr
 * 23:20), WAIT_FOR_READY, then the response in idefxModeData[15:0].
 * The model executes at once, so one poll stands for the FSW's 1000.
 * Returns false where the FSW would return E_RET_TIMEOUT.
 */
static bool idefx_word(QTestState *qts, uint32_t grp, uint32_t word,
                       uint16_t *rx)
{
    if (!(ctrl_read(qts, CTRL_IDEFXCMDSTA) & IDEFX_CMDREADY)) {
        return false;
    }
    ctrl_write(qts, CTRL_IDEFXCMDSTA,
               (((word >> 16) & 0xFu) << 16) | (word & 0xFFFFu) | grp << 20);
    if (!(ctrl_read(qts, CTRL_IDEFXCMDSTA) & IDEFX_CMDREADY)) {
        return false;
    }
    if (rx) {
        *rx = ctrl_read(qts, CTRL_IDEFXMODEDATA) & 0xFFFFu;
    }
    return true;
}

/* asserting variant: CmdReady is 1 before and after every command */
static uint16_t idefx_word_ok(QTestState *qts, uint32_t grp, uint32_t word)
{
    uint16_t rx = 0xDEADu;

    g_assert_true(idefx_word(qts, grp, word, &rx));
    return rx;
}

/* Detectors_setQuarterPowerMask / setGroupEnableMask / setModeMask */
static void idefx_power(QTestState *qts, uint32_t quarter_mask,
                        uint32_t grp_enable_mask, uint32_t grp_mode_mask)
{
    uint32_t cfg = ctrl_read(qts, CTRL_IDEFXCONFIG);

    cfg = (cfg & ~0xFu) | (quarter_mask & 0xFu);
    ctrl_write(qts, CTRL_IDEFXCONFIG, cfg);
    cfg = (cfg & ~(0xFFFFu << 4)) | ((grp_enable_mask & 0xFFFFu) << 4);
    ctrl_write(qts, CTRL_IDEFXCONFIG, cfg);
    ctrl_write(qts, CTRL_IDEFXMODEDATA, (grp_mode_mask & 0xFFFFu) << 16);
}

/* NOMINAL entry (OpModeMgmt.c:470-501): all quarters, groups, modes */
static void idefx_power_all(QTestState *qts)
{
    idefx_power(qts, 0xF, 0xFFFF, 0xFFFF);
}

/* WRITE_TO_GROUPS (Detectors.c:657) */
static void idefx_write_to_groups(QTestState *qts, uint32_t grp_mask)
{
    ctrl_write(qts, CTRL_IDEFXWRITEMASK,
               (ctrl_read(qts, CTRL_IDEFXWRITEMASK) & ~0xFFFF0000u) |
               (grp_mask & 0xFFFFu) << 16);
}

/*
 * The transaction frame of every Detectors_* operation: clear the
 * group's ReadMode bit, WriteMask = 1 << grp; restore both at the end
 */
static uint32_t idefx_begin(QTestState *qts, uint32_t grp)
{
    uint32_t mode = ctrl_read(qts, CTRL_IDEFXMODEDATA) >> 16;

    ctrl_write(qts, CTRL_IDEFXMODEDATA, (mode & ~(1u << grp)) << 16);
    idefx_write_to_groups(qts, 1u << grp);
    return mode;
}

static void idefx_end(QTestState *qts, uint32_t mode)
{
    idefx_write_to_groups(qts, 0);
    ctrl_write(qts, CTRL_IDEFXMODEDATA, mode << 16);
}

/* Detectors_writeReg (Detectors.c:728-785), asic 0, 1 or 2 (All) */
static void idefx_write_reg(QTestState *qts, uint32_t grp, uint32_t asic,
                            uint32_t reg, const uint8_t *data,
                            uint32_t nbytes)
{
    uint32_t mode = idefx_begin(qts, grp);
    uint32_t n;

    idefx_word_ok(qts, grp, 0);
    idefx_word_ok(qts, grp, 0xBFD30u | asic << 6);
    idefx_word_ok(qts, grp, 0x80000u | reg << 7);
    for (n = 0; n < nbytes; n++) {
        idefx_word_ok(qts, grp, 0x80000u | (uint32_t)data[n] << 7);
    }
    idefx_word_ok(qts, grp, 0x48000u);
    idefx_word_ok(qts, grp, 0);
    idefx_end(qts, mode);
}

/*
 * Detectors_readReg (Detectors.c:787-851): raw[] gets the response
 * bytes as the FSW reads them, out[] the bytes after the last-byte
 * shift >> (8 - len % 8) (:840)
 */
static void idefx_read_reg_raw(QTestState *qts, uint32_t grp, uint32_t asic,
                               uint32_t reg, uint8_t *raw, uint8_t *out)
{
    uint32_t len = idefx_reg_bits[reg];
    uint32_t nbytes = idefx_reg_bytes(reg);
    uint32_t mode = idefx_begin(qts, grp);
    uint32_t n;

    idefx_word_ok(qts, grp, 0);
    idefx_word_ok(qts, grp, 0xBFD30u | asic << 6);
    idefx_word_ok(qts, grp, 0x84000u | reg << 7);
    for (n = 0; n < nbytes; n++) {
        raw[n] = (uint8_t)idefx_word_ok(qts, grp, 0x80000u);
        out[n] = raw[n];
    }
    if (len % 8) {
        out[nbytes - 1] = (uint8_t)(out[nbytes - 1] >> (8 - len % 8));
    }
    idefx_word_ok(qts, grp, 0x48000u);
    idefx_word_ok(qts, grp, 0);
    idefx_end(qts, mode);
}

static void idefx_read_reg(QTestState *qts, uint32_t grp, uint32_t asic,
                           uint32_t reg, uint8_t *out)
{
    uint8_t raw[24];

    idefx_read_reg_raw(qts, grp, asic, reg, raw, out);
}

/* a short (<= 8 bit) register or a 32-bit one, as a number */
static uint32_t idefx_read_val(QTestState *qts, uint32_t grp, uint32_t asic,
                               uint32_t reg)
{
    uint8_t out[24];
    uint32_t n, v = 0;

    g_assert_cmpuint(idefx_reg_bytes(reg), <=, 4);
    idefx_read_reg(qts, grp, asic, reg, out);
    for (n = 0; n < idefx_reg_bytes(reg); n++) {
        v = v << 8 | out[n];
    }
    return v;
}

static void idefx_write_val(QTestState *qts, uint32_t grp, uint32_t asic,
                            uint32_t reg, uint32_t val)
{
    uint32_t nbytes = idefx_reg_bytes(reg);
    uint8_t data[4];
    uint32_t n;

    g_assert_cmpuint(nbytes, <=, 4);
    for (n = 0; n < nbytes; n++) {
        data[n] = (uint8_t)(val >> (8 * (nbytes - 1 - n)));
    }
    idefx_write_reg(qts, grp, asic, reg, data, nbytes);
}

/* Detectors_resetReg (Detectors.c:853-881) */
static void idefx_reset_reg(QTestState *qts, uint32_t grp, uint32_t asic)
{
    uint32_t mode = idefx_begin(qts, grp);

    idefx_word_ok(qts, grp, 0);
    idefx_word_ok(qts, grp, 0xBFF30u | asic << 6);
    idefx_word_ok(qts, grp, 0);
    idefx_end(qts, mode);
}

/* every register of (grp, asic) holds its HW default */
static void idefx_expect_defaults(QTestState *qts, uint32_t grp,
                                  uint32_t asic)
{
    static const uint8_t r3_pat[3] = { 0xFB, 0xEF, 0xBE };
    uint8_t r3[24];
    uint32_t reg, n;

    for (reg = 1; reg <= 12; reg++) {
        if (reg == 3) {
            continue;
        }
        g_test_message("grp %u asic %u reg %u", grp, asic, reg);
        g_assert_cmphex(idefx_read_val(qts, grp, asic, reg), ==,
                        idefx_reg_default[reg]);
    }
    /* 62 = 0b111110 on all 32 channels: FB EF BE, eight times */
    idefx_read_reg(qts, grp, asic, 3, r3);
    for (n = 0; n < 24; n++) {
        g_assert_cmphex(r3[n], ==, r3_pat[n % 3]);
    }
}

/*
 * D-De: ideFxCmdSta reads CmdReady = 1 from reset (D-Dh 0x80000000)
 * and after every command of every FSW sequence; the command fields
 * read back; idefxModeData 15:0 is read-only. RED on the D-1 binary:
 * ideFxCmdSta reads 0 (plain storage, no CmdReady).
 */
static void test_det_idefx_ready(void)
{
    QTestState *qts = stix_qtest_new();
    uint32_t grp;
    uint16_t a, b;

    g_assert_cmphex(ctrl_read(qts, CTRL_IDEFXCMDSTA), ==, IDEFX_CMDREADY);

    /* the boot's resetReg x 16 with nothing powered (OpModeMgmt.c:1061) */
    for (grp = 0; grp < 16; grp++) {
        idefx_reset_reg(qts, grp, 2);
    }

    idefx_power_all(qts);
    idefx_write_val(qts, 4, 1, 5, 9);
    g_assert_cmphex(idefx_read_val(qts, 4, 1, 5), ==, 9);
    idefx_reset_reg(qts, 4, 2);

    /* readTemp, and commands the model does not decode */
    idefx_begin(qts, 4);
    idefx_word_ok(qts, 4, 0);
    idefx_word_ok(qts, 4, 0xBFD30u);
    idefx_word_ok(qts, 4, 0x84680u);
    idefx_word_ok(qts, 4, 0x60000u);
    a = idefx_word_ok(qts, 4, 0x10000u);
    idefx_word_ok(qts, 4, 0x80000u);
    b = idefx_word_ok(qts, 4, 0x10000u);
    g_assert_cmphex(a, >=, b);
    idefx_word_ok(qts, 4, 0x70000u);
    idefx_word_ok(qts, 4, 0x48000u);
    idefx_word_ok(qts, 4, 0x5FFFFu);
    idefx_word_ok(qts, 4, 0x9FFFFu);
    idefx_word_ok(qts, 4, 0xCFFFFu);
    idefx_word_ok(qts, 4, 0xFFFFFu);
    idefx_word_ok(qts, 4, 0);

    /* the command fields read back (reserved 30:24 read 0) */
    ctrl_write(qts, CTRL_IDEFXCMDSTA, 0x7FF5ABCDu);
    g_assert_cmphex(ctrl_read(qts, CTRL_IDEFXCMDSTA), ==, 0x80F5ABCDu);

    /* idefxModeData: ReadMode 31:16 R/W, the response 15:0 read-only */
    ctrl_write(qts, CTRL_IDEFXMODEDATA, 0xA5A5FFFFu);
    g_assert_cmphex(ctrl_read(qts, CTRL_IDEFXMODEDATA), ==, 0xA5A50000u);
    /* idefxWriteMask: the mask 31:16 R/W, reserved 15:0 read 0 */
    ctrl_write(qts, CTRL_IDEFXWRITEMASK, 0x5A5AFFFFu);
    g_assert_cmphex(ctrl_read(qts, CTRL_IDEFXWRITEMASK), ==, 0x5A5A0000u);

    qtest_system_reset(qts);
    g_assert_cmphex(ctrl_read(qts, CTRL_IDEFXCMDSTA), ==, IDEFX_CMDREADY);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* a test pattern per (grp, asic, reg, byte), in range for short regs */
static uint8_t idefx_pattern(uint32_t grp, uint32_t asic, uint32_t reg,
                             uint32_t n)
{
    uint32_t v = (grp * 29 + asic * 101 + reg * 31 + n * 7 + 0x5A) & 0xFF;
    uint32_t len = idefx_reg_bits[reg];

    return len < 8 ? (uint8_t)(v & ((1u << len) - 1)) : (uint8_t)v;
}

/*
 * D-De: every register 1..12 on both asic of two grp (different
 * quarters) round-trips through the FSW's writeReg/readReg; short
 * registers come back left-aligned (R1 0x01 -> 0x40, R7 0x15 -> 0xA8);
 * the 24 bytes of R3 come back in order; R4 and R12 round-trip as
 * 32-bit; a write longer than the register keeps the last N bits.
 */
static void test_det_idefx_roundtrip(void)
{
    static const uint32_t grps[2] = { 3, 12 };
    QTestState *qts = stix_qtest_new();
    uint8_t data[24], raw[24], out[24];
    uint32_t g, asic, reg, n;

    idefx_power_all(qts);

    for (g = 0; g < 2; g++) {
        for (asic = 0; asic < 2; asic++) {
            for (reg = 1; reg <= 12; reg++) {
                for (n = 0; n < idefx_reg_bytes(reg); n++) {
                    data[n] = idefx_pattern(grps[g], asic, reg, n);
                }
                idefx_write_reg(qts, grps[g], asic, reg, data,
                                idefx_reg_bytes(reg));
            }
        }
    }
    for (g = 0; g < 2; g++) {
        for (asic = 0; asic < 2; asic++) {
            for (reg = 1; reg <= 12; reg++) {
                g_test_message("grp %u asic %u reg %u", grps[g], asic, reg);
                idefx_read_reg(qts, grps[g], asic, reg, out);
                for (n = 0; n < idefx_reg_bytes(reg); n++) {
                    g_assert_cmphex(out[n], ==,
                                    idefx_pattern(grps[g], asic, reg, n));
                }
            }
        }
    }
    /* the other grp of the same quarters keep their defaults */
    idefx_expect_defaults(qts, 2, 0);
    idefx_expect_defaults(qts, 13, 1);

    /* left alignment of the short registers, seen on the wire */
    idefx_write_val(qts, 3, 0, 1, 0x01);
    idefx_read_reg_raw(qts, 3, 0, 1, raw, out);
    g_assert_cmphex(raw[0], ==, 0x40);
    g_assert_cmphex(out[0], ==, 0x01);
    idefx_write_val(qts, 3, 0, 7, 0x15);
    idefx_read_reg_raw(qts, 3, 0, 7, raw, out);
    g_assert_cmphex(raw[0], ==, 0xA8);
    g_assert_cmphex(out[0], ==, 0x15);

    /* R4 and R12 as 32-bit values */
    idefx_write_val(qts, 12, 1, 4, 0x12345678u);
    g_assert_cmphex(idefx_read_val(qts, 12, 1, 4), ==, 0x12345678u);
    idefx_write_val(qts, 12, 1, 12, 0x89ABCDEFu);
    g_assert_cmphex(idefx_read_val(qts, 12, 1, 12), ==, 0x89ABCDEFu);

    /* more bytes than the length: the last N bits stay */
    data[0] = 0x11;
    data[1] = 0x22;
    data[2] = 0x33;
    data[3] = 0x44;
    data[4] = 0x55;
    idefx_write_reg(qts, 12, 1, 4, data, 5);
    g_assert_cmphex(idefx_read_val(qts, 12, 1, 4), ==, 0x22334455u);
    data[0] = 0x03;
    data[1] = 0xFE;
    idefx_write_reg(qts, 12, 1, 1, data, 2);
    g_assert_cmphex(idefx_read_val(qts, 12, 1, 1), ==, 0x02);
    data[0] = 0xFF;
    data[1] = 0x6C;
    idefx_write_reg(qts, 12, 1, 7, data, 2);
    g_assert_cmphex(idefx_read_val(qts, 12, 1, 7), ==, 0x0C);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* D-De: asic = 2 (All, 0xBFDB0) writes both ASICs of the group */
static void test_det_idefx_broadcast(void)
{
    QTestState *qts = stix_qtest_new();
    uint8_t data[24], out[24];
    uint32_t n;

    idefx_power_all(qts);
    idefx_write_val(qts, 5, 2, 5, 0x9);
    g_assert_cmphex(idefx_read_val(qts, 5, 0, 5), ==, 0x9);
    g_assert_cmphex(idefx_read_val(qts, 5, 1, 5), ==, 0x9);
    g_assert_cmphex(idefx_read_val(qts, 4, 0, 5), ==, 15);
    g_assert_cmphex(idefx_read_val(qts, 6, 1, 5), ==, 15);

    for (n = 0; n < 24; n++) {
        data[n] = (uint8_t)(0xC0 + n);
    }
    idefx_write_reg(qts, 5, 2, 3, data, 24);
    idefx_read_reg(qts, 5, 0, 3, out);
    g_assert_cmpmem(out, 24, data, 24);
    idefx_read_reg(qts, 5, 1, 3, out);
    g_assert_cmpmem(out, 24, data, 24);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* non-default values in every register of (grp, asic); R5 reads 0 */
static void idefx_dirty(QTestState *qts, uint32_t grp, uint32_t asic)
{
    uint8_t data[24];
    uint32_t reg, n;

    for (reg = 1; reg <= 12; reg++) {
        for (n = 0; n < idefx_reg_bytes(reg); n++) {
            data[n] = (uint8_t)(idefx_reg_bits[reg] < 8 ?
                                ~idefx_reg_default[reg] &
                                ((1u << idefx_reg_bits[reg]) - 1) :
                                0x12 + n);
        }
        idefx_write_reg(qts, grp, asic, reg, data, idefx_reg_bytes(reg));
    }
    g_assert_cmphex(idefx_read_val(qts, grp, asic, 5), ==, 0);
}

/*
 * D-De defaults: the RD = SC = 1 command (resetReg, 0xBFF30 | asic << 6)
 * resets the addressed ASIC only, both with asic 2 (All) [A: the FSW
 * resets one ASIC per subcol, XRayADCsCtrl.c:833]; so does a
 * quarter-power 0 -> 1 edge for its
 * quarter's groups, and system_reset for all (which also idles a
 * half-open transaction).
 */
static void test_det_idefx_reset(void)
{
    QTestState *qts = stix_qtest_new();
    uint32_t cfg;

    idefx_power_all(qts);
    idefx_dirty(qts, 2, 0);
    idefx_dirty(qts, 2, 1);
    idefx_dirty(qts, 9, 0);
    idefx_dirty(qts, 0, 1);

    /* resetReg(grp, 0): asic 0 only; the sibling keeps its values */
    idefx_reset_reg(qts, 2, 0);
    idefx_expect_defaults(qts, 2, 0);
    g_assert_cmphex(idefx_read_val(qts, 2, 1, 5), ==, 0);
    /* resetReg(grp, 1) (address 7): asic 1 only */
    idefx_dirty(qts, 2, 0);
    idefx_reset_reg(qts, 2, 1);
    idefx_expect_defaults(qts, 2, 1);
    g_assert_cmphex(idefx_read_val(qts, 2, 0, 5), ==, 0);
    /* resetReg(grp, 2) (All, the boot's call): both */
    idefx_dirty(qts, 2, 1);
    idefx_reset_reg(qts, 2, 2);
    idefx_expect_defaults(qts, 2, 0);
    idefx_expect_defaults(qts, 2, 1);
    g_assert_cmphex(idefx_read_val(qts, 9, 0, 5), ==, 0);
    g_assert_cmphex(idefx_read_val(qts, 0, 1, 5), ==, 0);

    /* quarter 2 (grp 8-11) power 1 -> 0 -> 1: grp 9 defaults, grp 0 kept */
    cfg = ctrl_read(qts, CTRL_IDEFXCONFIG);
    ctrl_write(qts, CTRL_IDEFXCONFIG, cfg & ~(1u << 2));
    ctrl_write(qts, CTRL_IDEFXCONFIG, cfg);
    idefx_expect_defaults(qts, 9, 0);
    g_assert_cmphex(idefx_read_val(qts, 0, 1, 5), ==, 0);
    /* a write of the powered mask (no edge) changes nothing */
    idefx_dirty(qts, 9, 0);
    ctrl_write(qts, CTRL_IDEFXCONFIG, cfg);
    g_assert_cmphex(idefx_read_val(qts, 9, 0, 5), ==, 0);
    /* the edge also idles the quarter's decoders: a half-open write */
    idefx_begin(qts, 9);
    idefx_word_ok(qts, 9, 0);
    idefx_word_ok(qts, 9, 0xBFD30u);
    idefx_word_ok(qts, 9, 0x80000u | 5u << 7);
    ctrl_write(qts, CTRL_IDEFXCONFIG, cfg & ~(1u << 2));
    ctrl_write(qts, CTRL_IDEFXCONFIG, cfg);
    idefx_word_ok(qts, 9, 0x80000u | 0x03u << 7);
    idefx_word_ok(qts, 9, 0x48000u);
    idefx_word_ok(qts, 9, 0);
    idefx_end(qts, 0xFFFF);
    g_assert_cmphex(idefx_read_val(qts, 9, 0, 5), ==, 15);

    /* a half-open write transaction, then system_reset */
    idefx_begin(qts, 0);
    idefx_word_ok(qts, 0, 0);
    idefx_word_ok(qts, 0, 0xBFD70u);
    idefx_word_ok(qts, 0, 0x80000u | 5u << 7);
    qtest_system_reset(qts);
    idefx_power_all(qts);
    /* a data block alone is not a transaction after the reset */
    idefx_write_to_groups(qts, 1u << 0);
    ctrl_write(qts, CTRL_IDEFXMODEDATA, 0);
    idefx_word_ok(qts, 0, 0x80000u | 0x03u << 7);
    idefx_word_ok(qts, 0, 0x48000u);
    idefx_word_ok(qts, 0, 0);
    idefx_end(qts, 0xFFFF);
    idefx_expect_defaults(qts, 0, 1);
    idefx_expect_defaults(qts, 9, 0);
    idefx_expect_defaults(qts, 2, 1);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * one raw write of R5 = val with WriteMask and ReadMode as given (the
 * FSW's frame is bypassed); returns the OR of the responses
 */
static uint16_t idefx_raw_write_r5(QTestState *qts, uint32_t grp_addr,
                                   uint32_t write_mask, uint32_t mode_mask,
                                   uint32_t asic, uint32_t val)
{
    uint16_t rx = 0;

    ctrl_write(qts, CTRL_IDEFXMODEDATA, mode_mask << 16);
    idefx_write_to_groups(qts, write_mask);
    rx |= idefx_word_ok(qts, grp_addr, 0);
    rx |= idefx_word_ok(qts, grp_addr, 0xBFD30u | asic << 6);
    rx |= idefx_word_ok(qts, grp_addr, 0x80000u | 5u << 7);
    rx |= idefx_word_ok(qts, grp_addr, 0x80000u | val << 7);
    rx |= idefx_word_ok(qts, grp_addr, 0x48000u);
    rx |= idefx_word_ok(qts, grp_addr, 0);
    idefx_end(qts, 0xFFFF);
    return rx;
}

/*
 * one raw read of R5 (asic 0) with WriteMask and ReadMode as given;
 * returns the data block's response
 */
static uint16_t idefx_raw_read_r5(QTestState *qts, uint32_t grp_addr,
                                  uint32_t write_mask, uint32_t mode_mask)
{
    uint16_t rx;

    ctrl_write(qts, CTRL_IDEFXMODEDATA, mode_mask << 16);
    idefx_write_to_groups(qts, write_mask);
    idefx_word_ok(qts, grp_addr, 0);
    idefx_word_ok(qts, grp_addr, 0xBFD30u);
    idefx_word_ok(qts, grp_addr, 0x84000u | 5u << 7);
    rx = idefx_word_ok(qts, grp_addr, 0x80000u);
    idefx_word_ok(qts, grp_addr, 0x48000u);
    idefx_word_ok(qts, grp_addr, 0);
    idefx_end(qts, 0xFFFF);
    return rx;
}

/*
 * D-De delivery: a grp takes part only with its quarter powered, its
 * Enable bit set and its ReadMode bit 0; otherwise the command is
 * ignored, CmdReady stays 1 and the response is 0. A word goes to the
 * WriteMask grp (GrpAddr's grp when WriteMask = 0); the response comes
 * from GrpAddr's grp.
 */
static void test_det_idefx_gating(void)
{
    QTestState *qts = stix_qtest_new();
    uint8_t out[24];
    uint32_t n;

    idefx_power_all(qts);
    idefx_write_val(qts, 6, 0, 5, 7);
    g_assert_cmphex(idefx_read_val(qts, 6, 0, 5), ==, 7);

    /* grp 6 disabled: the write is ignored and the read answers 0 */
    idefx_power(qts, 0xF, 0xFFFF & ~(1u << 6), 0xFFFF);
    idefx_write_val(qts, 6, 0, 5, 2);
    g_assert_cmphex(idefx_read_val(qts, 6, 0, 5), ==, 0);
    idefx_read_reg(qts, 6, 0, 3, out);
    for (n = 0; n < 24; n++) {
        g_assert_cmphex(out[n], ==, 0);
    }
    idefx_power_all(qts);
    g_assert_cmphex(idefx_read_val(qts, 6, 0, 5), ==, 7);

    /* ReadMode = 1 (readout mode): ignored, response 0 */
    g_assert_cmphex(idefx_raw_write_r5(qts, 6, 1u << 6, 1u << 6, 0, 3),
                    ==, 0);
    g_assert_cmphex(idefx_read_val(qts, 6, 0, 5), ==, 7);
    /* the same with ReadMode = 0 is delivered */
    idefx_raw_write_r5(qts, 6, 1u << 6, 0, 0, 3);
    g_assert_cmphex(idefx_read_val(qts, 6, 0, 5), ==, 3);
    /* WriteMask = 0: delivered to GrpAddr's grp (lenient) */
    idefx_raw_write_r5(qts, 6, 0, 0, 0, 4);
    g_assert_cmphex(idefx_read_val(qts, 6, 0, 5), ==, 4);
    /* WriteMask names grp 7: grp 7 takes it, grp 6 does not */
    idefx_raw_write_r5(qts, 6, 1u << 7, 0, 0, 5);
    g_assert_cmphex(idefx_read_val(qts, 6, 0, 5), ==, 4);
    g_assert_cmphex(idefx_read_val(qts, 7, 0, 5), ==, 5);
    /* the response comes from GrpAddr's grp: 0 if it is not in WriteMask */
    idefx_begin(qts, 7);
    idefx_word_ok(qts, 7, 0);
    idefx_word_ok(qts, 7, 0xBFD30u);
    idefx_word_ok(qts, 7, 0x84000u | 5u << 7);
    g_assert_cmphex(idefx_word_ok(qts, 6, 0x80000u), ==, 0);
    idefx_word_ok(qts, 7, 0x48000u);
    idefx_word_ok(qts, 7, 0);
    idefx_end(qts, 0xFFFF);

    /*
     * several grp in WriteMask, different contents (grp 6 R5 = 4, grp 7
     * R5 = 5; 5 << 4 = 0x50 on the wire): the response is GrpAddr's
     */
    g_assert_cmphex(idefx_raw_read_r5(qts, 6, 3u << 6, 0), ==, 4u << 4);
    g_assert_cmphex(idefx_raw_read_r5(qts, 7, 3u << 6, 0), ==, 5u << 4);
    /* ReadMode gates reads too: grp 6 in readout mode answers 0 */
    g_assert_cmphex(idefx_raw_read_r5(qts, 6, 1u << 6, 1u << 6), ==, 0);
    g_assert_cmphex(idefx_raw_read_r5(qts, 6, 3u << 6, 1u << 6), ==, 0);
    g_assert_cmphex(idefx_raw_read_r5(qts, 7, 3u << 6, 1u << 6), ==,
                    5u << 4);

    /* quarter 1 (grp 4-7) unpowered: ignored, the read answers 0 */
    idefx_power(qts, 0xF & ~(1u << 1), 0xFFFF, 0xFFFF);
    idefx_write_val(qts, 6, 0, 5, 1);
    g_assert_cmphex(idefx_read_val(qts, 6, 0, 5), ==, 0);
    g_assert_cmphex(idefx_read_val(qts, 8, 0, 5), ==, 15);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * D-De cmd 1 by context: Detectors_readTemp (Detectors.c:883-950)
 * gets A = 0x900 then B = 0x700 (A >= B, XRayADCsCtrl.c:540-546);
 * Detectors_adcRead (:976-1003) gets the baseline 1850; cmd 2/3 too.
 */
static void test_det_idefx_adc(void)
{
    QTestState *qts = stix_qtest_new();
    uint32_t mode;
    uint16_t a, b, rx[4];

    idefx_power_all(qts);
    idefx_write_val(qts, 1, 0, 5, 0x6);

    /* readTemp(1, 0) */
    mode = idefx_begin(qts, 1);
    idefx_word_ok(qts, 1, 0);
    idefx_word_ok(qts, 1, 0xBFD30u);
    idefx_word_ok(qts, 1, 0x84680u);
    idefx_word_ok(qts, 1, 0x60000u);
    a = idefx_word_ok(qts, 1, 0x10000u) & 0xFFF;
    idefx_word_ok(qts, 1, 0x80000u);
    b = idefx_word_ok(qts, 1, 0x10000u) & 0xFFF;
    idefx_word_ok(qts, 1, 0x70000u);
    idefx_word_ok(qts, 1, 0x48000u);
    idefx_word_ok(qts, 1, 0);
    idefx_end(qts, mode);
    g_assert_cmphex(a, ==, 0x900);
    g_assert_cmphex(b, ==, 0x700);
    g_assert_cmphex(a, >=, b);
    /* the temperature read touched no register */
    g_assert_cmphex(idefx_read_val(qts, 1, 0, 5), ==, 0x6);

    /* adcRead(1): 0x60000, 0x10000, 0x70000, 0; the ADC is rx[1] */
    mode = idefx_begin(qts, 1);
    rx[0] = idefx_word_ok(qts, 1, 0x60000u);
    rx[1] = idefx_word_ok(qts, 1, 0x10000u);
    rx[2] = idefx_word_ok(qts, 1, 0x70000u);
    rx[3] = idefx_word_ok(qts, 1, 0);
    idefx_end(qts, mode);
    g_assert_cmpuint(rx[1] & 0xFFF, ==, 1850);
    g_assert_cmpuint(rx[0], ==, 0);
    g_assert_cmpuint(rx[2], ==, 0);
    g_assert_cmpuint(rx[3], ==, 0);

    /* cmd 2 (ADCBUFFREAD) and 3 (ADCMUXREAD) give the baseline too */
    mode = idefx_begin(qts, 1);
    g_assert_cmpuint(idefx_word_ok(qts, 1, 0x20000u), ==, 1850);
    g_assert_cmpuint(idefx_word_ok(qts, 1, 0x30000u), ==, 1850);
    idefx_word_ok(qts, 1, 0);
    idefx_end(qts, mode);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * det-idefx=off keeps the legacy interface: ideFxCmdSta,
 * idefxModeData and idefxWriteMask are plain storage with CmdReady 0,
 * so every FSW transmission times out (the 384 NOMINAL-entry timeouts).
 */
static void test_det_idefx_off(void)
{
    QTestState *qts =
        stix_qtest_new_extra("-global grlib-stixctrl.det-idefx=off");

    g_assert_cmphex(ctrl_read(qts, CTRL_IDEFXCMDSTA), ==, 0);
    idefx_power_all(qts);
    g_assert_false(idefx_word(qts, 3, 0, NULL));
    ctrl_write(qts, CTRL_IDEFXCMDSTA, 0x00B3FD30u);
    g_assert_cmphex(ctrl_read(qts, CTRL_IDEFXCMDSTA), ==, 0x00B3FD30u);
    ctrl_write(qts, CTRL_IDEFXMODEDATA, 0xFFFF1234u);
    g_assert_cmphex(ctrl_read(qts, CTRL_IDEFXMODEDATA), ==, 0xFFFF1234u);
    ctrl_write(qts, CTRL_IDEFXWRITEMASK, 0x0001FFFFu);
    g_assert_cmphex(ctrl_read(qts, CTRL_IDEFXWRITEMASK), ==, 0x0001FFFFu);

    /* the rest of the detector model stays on: the D-Dh reset still runs */
    qtest_system_reset(qts);
    g_assert_cmphex(ctrl_read(qts, CTRL_IDEFXCMDSTA), ==, 0);
    g_assert_cmphex(ctrl_read(qts, CTRL_IDEFXMODEDATA), ==, 0);
    g_assert_cmphex(ctrl_read(qts, CTRL_TESTPULSECMDSTA), ==, TESTPULSE_BUSY);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- Synthetic sky and the flare switch (detector plan D-4, tier 2) ---- */

#define CTRL_FSMQLVARDMASK   0x24u
#define CTRL_FSMQLVAREMASK   0x28u
#define CTRL_FSMSUMDMASK     0x34u
#define CTRL_FSMSUMEMASK     0x38u

/* LACC [pix][fpga_det][ebin] u16 (Detectors.h:68-79); LTRG per grp u32 */
/* pix 0..11; pix 12, the guard ring, is never accumulated */
#define SKY_N_PIX            12u
#define SKY_N_FPGA_DET       32u    /* fpga_det = 2 * grp + asic */
#define SKY_N_EBIN           32u
#define SKY_N_GRP            16u
#define SKY_N_LVAR_SLOT      40u
#define SKY_CTRL_PATH        "/machine/stixctrl"

/*
 * The D-Df template, as the test expects it: per fpga_det the rate is
 * shared by pix with weight 1.0 (large pix 0-7) and 0.22 (small pix
 * 8-11), total weight 8.88; the quiet spectrum puts 45 % in ebin 18,
 * 10 % in ebin 19, 20 % in ebin 27, 10 % flat over ebin 1-11 and 15 %
 * flat over ebin 12-30; the flare spectrum 70 % in ebin 1-10 (thermal)
 * and 30 % in ebin 11-25 (power law). Nothing in ebin 0 or 31.
 */
#define SKY_PIX_W_TOTAL      8.88
#define SKY_QUIET_EBIN18     0.45

typedef struct SkyBank {
    uint16_t lacc[SKY_N_PIX][SKY_N_FPGA_DET][SKY_N_EBIN];
    uint32_t ltrg_grp[SKY_N_GRP];
    uint32_t lvar_slot[SKY_N_LVAR_SLOT];
} SkyBank;

/* one QL bank as the FSW reads it: big-endian (Detectors.h:68-95) */
static void sky_read_bank(QTestState *qts, uint32_t n, SkyBank *b)
{
    g_autofree uint8_t *raw = g_malloc(QL_LACC_SIZE);
    uint8_t small[QL_LVAR_SIZE];
    uint32_t pix, fpga_det, ebin, i;

    qtest_memread(qts, QL_LACC(n), raw, QL_LACC_SIZE);
    for (pix = 0; pix < SKY_N_PIX; pix++) {
        for (fpga_det = 0; fpga_det < SKY_N_FPGA_DET; fpga_det++) {
            for (ebin = 0; ebin < SKY_N_EBIN; ebin++) {
                uint32_t off = ((pix * 32 + fpga_det) * 32 + ebin) * 2;

                b->lacc[pix][fpga_det][ebin] = lduw_be_p(raw + off);
            }
        }
    }
    qtest_memread(qts, QL_LTRG(n), small, QL_LTRG_SIZE);
    for (i = 0; i < SKY_N_GRP; i++) {
        b->ltrg_grp[i] = ldl_be_p(small + 4 * i);
    }
    qtest_memread(qts, QL_LVAR(n), small, QL_LVAR_SIZE);
    for (i = 0; i < SKY_N_LVAR_SLOT; i++) {
        b->lvar_slot[i] = ldl_be_p(small + 4 * i);
    }
}

static uint64_t sky_fpga_det_sum(const SkyBank *b, uint32_t fpga_det)
{
    uint64_t sum = 0;
    uint32_t pix, ebin;

    for (pix = 0; pix < SKY_N_PIX; pix++) {
        for (ebin = 0; ebin < SKY_N_EBIN; ebin++) {
            sum += b->lacc[pix][fpga_det][ebin];
        }
    }
    return sum;
}

static uint64_t sky_pix_sum(const SkyBank *b, uint32_t fpga_det, uint32_t pix)
{
    uint64_t sum = 0;
    uint32_t ebin;

    for (ebin = 0; ebin < SKY_N_EBIN; ebin++) {
        sum += b->lacc[pix][fpga_det][ebin];
    }
    return sum;
}

static uint64_t sky_ebin_sum(const SkyBank *b, uint32_t ebin)
{
    uint64_t sum = 0;
    uint32_t pix, fpga_det;

    for (pix = 0; pix < SKY_N_PIX; pix++) {
        for (fpga_det = 0; fpga_det < SKY_N_FPGA_DET; fpga_det++) {
            sum += b->lacc[pix][fpga_det][ebin];
        }
    }
    return sum;
}

static uint64_t sky_total(const SkyBank *b)
{
    uint64_t sum = 0;
    uint32_t fpga_det;

    for (fpga_det = 0; fpga_det < SKY_N_FPGA_DET; fpga_det++) {
        sum += sky_fpga_det_sum(b, fpga_det);
    }
    return sum;
}

/* |got - lambda| within k Poisson sigmas */
static void sky_assert_poisson(const char *what, double got, double lambda,
                               double k)
{
    double tol = k * sqrt(lambda > 1 ? lambda : 1);

    g_test_message("%s: %.0f, expected %.1f +- %.1f", what, got, lambda,
                   tol);
    g_assert_cmpfloat(fabs(got - lambda), <=, tol);
}

static bool sky_set_str(QTestState *qts, const char *prop, const char *val)
{
    QDict *resp = qtest_qmp(qts,
                            "{'execute': 'qom-set',"
                            " 'arguments': {'path': %s, 'property': %s,"
                            " 'value': %s}}", SKY_CTRL_PATH, prop, val);
    bool ok = qdict_haskey(resp, "return");

    qobject_unref(resp);
    return ok;
}

static bool sky_set_u32(QTestState *qts, const char *prop, uint32_t val)
{
    QDict *resp = qtest_qmp(qts,
                            "{'execute': 'qom-set',"
                            " 'arguments': {'path': %s, 'property': %s,"
                            " 'value': %u}}", SKY_CTRL_PATH, prop, val);
    bool ok = qdict_haskey(resp, "return");

    qobject_unref(resp);
    return ok;
}

static bool sky_set_u64(QTestState *qts, const char *prop, uint64_t val)
{
    QDict *resp = qtest_qmp(qts,
                            "{'execute': 'qom-set',"
                            " 'arguments': {'path': %s, 'property': %s,"
                            " 'value': %" PRIu64 "}}", SKY_CTRL_PATH, prop,
                            val);
    bool ok = qdict_haskey(resp, "return");

    qobject_unref(resp);
    return ok;
}

/* a string property, or NULL when it is unknown */
static char *sky_get_str(QTestState *qts, const char *prop)
{
    QDict *resp = qtest_qmp(qts,
                            "{'execute': 'qom-get',"
                            " 'arguments': {'path': %s, 'property': %s}}",
                            SKY_CTRL_PATH, prop);
    QString *str = qobject_to(QString, qdict_get(resp, "return"));
    char *val = str ? g_strdup(qstring_get_str(str)) : NULL;

    qobject_unref(resp);
    return val;
}

static void sky_expect_scene(QTestState *qts, const char *source, bool flare,
                             int64_t quiet_cps, int64_t flare_cps)
{
    g_autofree char *got = sky_get_str(qts, "det-source");
    bool ok;

    g_assert_cmpstr(got, ==, source);
    g_assert_true(stix_qom_get_bool(qts, SKY_CTRL_PATH, "det-flare", &ok) ==
                  flare);
    g_assert_true(ok);
    g_assert_cmpint(stix_qom_counter(qts, SKY_CTRL_PATH, "det-quiet-cps"),
                    ==, quiet_cps);
    g_assert_cmpint(stix_qom_counter(qts, SKY_CTRL_PATH, "det-flare-cps"),
                    ==, flare_cps);
}

/*
 * The NOMINAL-entry detector set-up the sky needs (OpModeMgmt.c:470-513,
 * 634; QLVariance.c:86): every quarter, grp and ReadMode on is done by
 * the caller (idefx_power*); here the FPGA masks (all fpga_det in
 * SUMDMASK and VARDMASK, SUMEMASK 0xFFE = ebin 1-11, VAREMASK
 * 0x7FFE0000 = ebin 17-30), the nominal integration parameters
 * (OpModeMgmt.c:625), QlIntTime and VARPMASK (pix bits), then SetEnable
 * and Reset.
 */
static void sky_start_nom(QTestState *qts, uint32_t ql_int_time,
                          uint32_t varpmask_pix, uint32_t time_min,
                          uint32_t time_max, uint32_t events_max)
{
    ctrl_write(qts, CTRL_FSMSUMDMASK, 0xFFFFFFFFu);
    ctrl_write(qts, CTRL_FSMSUMEMASK, 0xFFEu);
    ctrl_write(qts, CTRL_FSMQLVARDMASK, 0xFFFFFFFFu);
    ctrl_write(qts, CTRL_FSMQLVAREMASK, 0x7FFE0000u);
    nom_set_params(qts, time_min, time_max, events_max);
    ctrl_write(qts, CTRL_FSMQLCONFIG,
               ql_int_time << 16 | (varpmask_pix & 0xFFFu));
    det_cmd_or(qts, FSMCMD_SETENABLE);
    det_cmd_or(qts, FSMCMD_RESET);
}

/* ... with the FSW's nominal integration parameters 9 / 199 / 1800 */
static void sky_start(QTestState *qts, uint32_t ql_int_time,
                      uint32_t varpmask_pix)
{
    sky_start_nom(qts, ql_int_time, varpmask_pix, 9, 199, 1800);
}

/*
 * det-counts-zero: det-source=zero (here from -global, as the D-3 gate
 * runs) gives an all-zero finished bank and no SUMCNT, even with every
 * quarter, grp and ReadMode on; switching to synthetic at run time
 * fills the next bank (the D-Dd lifecycle is unchanged).
 */
static void test_det_counts_zero(void)
{
    QTestState *qts =
        stix_qtest_new_extra("-global grlib-stixctrl.det-source=zero");
    SkyBank *b = g_new0(SkyBank, 1);
    uint32_t i;

    sky_expect_scene(qts, "zero", false, 20, 2000);
    idefx_power_all(qts);
    sky_start(qts, 39, 0xFFF);
    det_step_ticks(qts, 40);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 1);
    sky_read_bank(qts, 0, b);
    g_assert_cmpuint(sky_total(b), ==, 0);
    for (i = 0; i < SKY_N_GRP; i++) {
        g_assert_cmpuint(b->ltrg_grp[i], ==, 0);
    }
    for (i = 0; i < SKY_N_LVAR_SLOT; i++) {
        g_assert_cmpuint(b->lvar_slot[i], ==, 0);
    }
    g_assert_cmpuint(ctrl_read(qts, CTRL_FSMEVENTS), ==, 0);

    g_assert_true(sky_set_str(qts, "det-source", "synthetic"));
    det_step_ticks(qts, 40);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 2);
    sky_read_bank(qts, 1, b);
    g_assert_cmpuint(sky_total(b), >, 0);
    g_assert_cmpuint(ctrl_read(qts, CTRL_FSMEVENTS), >, 0);

    g_free(b);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * det-counts-quiet: the default scene (synthetic, 20 cps per fpga_det)
 * gives a finished bank of about 32 x 20 x 4 s = 2560 counts (5 sigma),
 * uniform over fpga_det (the largest below the QLDetPerf limits, 5 x
 * the median and Cbad 300, QLDetPerf.c:124-143), dominated by the lines
 * in ebin 18/19/27, the small pix at about 0.22 of a large one, and
 * SUMCNT (fsmEvents) about the SUMEMASK ebin 1-11 share, 10 %.
 */
static void test_det_counts_quiet(void)
{
    QTestState *qts = stix_qtest_new();
    SkyBank *b = g_new0(SkyBank, 1);
    uint64_t per_fpga_det[SKY_N_FPGA_DET], sorted[SKY_N_FPGA_DET];
    uint64_t total, lines, large = 0, small = 0, median, max = 0;
    uint32_t fpga_det, pix, i, j;

    sky_expect_scene(qts, "synthetic", false, 20, 2000);
    idefx_power_all(qts);
    sky_start(qts, 39, 0xFFF);
    det_step_ticks(qts, 40);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 1);
    sky_read_bank(qts, 0, b);

    total = sky_total(b);
    sky_assert_poisson("quiet bank sum", total, 32 * 20 * 4, 5);
    sky_assert_poisson("SUMCNT (ebin 1-11, 10 %)",
                       ctrl_read(qts, CTRL_FSMEVENTS), 32 * 20 * 4 * 0.10, 5);

    for (fpga_det = 0; fpga_det < SKY_N_FPGA_DET; fpga_det++) {
        per_fpga_det[fpga_det] = sky_fpga_det_sum(b, fpga_det);
        sorted[fpga_det] = per_fpga_det[fpga_det];
        max = MAX(max, per_fpga_det[fpga_det]);
        g_assert_cmpuint(per_fpga_det[fpga_det], >, 0);
    }
    for (i = 1; i < SKY_N_FPGA_DET; i++) {      /* insertion sort */
        uint64_t v = sorted[i];

        for (j = i; j > 0 && sorted[j - 1] > v; j--) {
            sorted[j] = sorted[j - 1];
        }
        sorted[j] = v;
    }
    median = sorted[SKY_N_FPGA_DET / 2];
    g_test_message("per fpga_det: median %" PRIu64 ", max %" PRIu64,
                   median, max);
    g_assert_cmpuint(max, <, 5 * MAX(median, 1));
    g_assert_cmpuint(max, <, 300);

    lines = sky_ebin_sum(b, 18) + sky_ebin_sum(b, 19) + sky_ebin_sum(b, 27);
    g_assert_cmpfloat((double)lines / total, >, 0.65);
    g_assert_cmpuint(sky_ebin_sum(b, 18), >, sky_ebin_sum(b, 27));
    g_assert_cmpuint(sky_ebin_sum(b, 27), >, sky_ebin_sum(b, 19));
    g_assert_cmpuint(sky_ebin_sum(b, 0), ==, 0);
    g_assert_cmpuint(sky_ebin_sum(b, 31), ==, 0);

    for (fpga_det = 0; fpga_det < SKY_N_FPGA_DET; fpga_det++) {
        for (pix = 0; pix < SKY_N_PIX; pix++) {
            if (pix < 8) {
                large += sky_pix_sum(b, fpga_det, pix);
            } else {
                small += sky_pix_sum(b, fpga_det, pix);
            }
        }
    }
    /* per pix: small / large = 0.22 (4 small, 8 large pix) */
    g_assert_cmpfloat((small / 4.0) / (large / 8.0), >, 0.15);
    g_assert_cmpfloat((small / 4.0) / (large / 8.0), <, 0.30);

    g_free(b);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * det-counts-flare: qom-set det-flare true at run time; the period that
 * starts after it gives a 100x bank (2000 / 20 cps), thermal in ebin
 * 1-10; false brings the next bank back to the quiet sum.
 */
static void test_det_counts_flare(void)
{
    QTestState *qts = stix_qtest_new();
    SkyBank *b = g_new0(SkyBank, 1);
    uint64_t total, thermal = 0;
    uint32_t ebin, pix, fpga_det, max_cell = 0;
    bool ok;

    idefx_power_all(qts);
    sky_start(qts, 39, 0xFFF);
    det_step_ticks(qts, 40);
    sky_read_bank(qts, 0, b);
    sky_assert_poisson("quiet bank 0", sky_total(b), 2560, 5);

    g_assert_true(stix_qom_set_bool(qts, SKY_CTRL_PATH, "det-flare", true));
    g_assert_true(stix_qom_get_bool(qts, SKY_CTRL_PATH, "det-flare", &ok));
    g_assert_true(ok);
    det_step_ticks(qts, 40);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 2);
    sky_read_bank(qts, 1, b);
    total = sky_total(b);
    sky_assert_poisson("flare bank 1", total, 32 * 2000 * 4, 5);
    for (ebin = 1; ebin <= 10; ebin++) {
        thermal += sky_ebin_sum(b, ebin);
    }
    g_assert_cmpfloat((double)thermal / total, >, 0.6);
    g_assert_cmpuint(sky_ebin_sum(b, 1), >, sky_ebin_sum(b, 18));
    /* no LACC cell near the u16 wrap (largest about 200, plan D-4) */
    for (pix = 0; pix < SKY_N_PIX; pix++) {
        for (fpga_det = 0; fpga_det < SKY_N_FPGA_DET; fpga_det++) {
            for (ebin = 0; ebin < SKY_N_EBIN; ebin++) {
                max_cell = MAX(max_cell, b->lacc[pix][fpga_det][ebin]);
            }
        }
    }
    g_assert_cmpuint(max_cell, <, 1000);

    g_assert_true(stix_qom_set_bool(qts, SKY_CTRL_PATH, "det-flare", false));
    det_step_ticks(qts, 40);
    sky_read_bank(qts, 2, b);
    sky_assert_poisson("quiet bank 2", sky_total(b), 2560, 5);

    g_free(b);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * R3 wire bytes with threshold @th_hot on asic_ch @hot_asic_ch and 62 on
 * every other: the 32 six-bit fields MSB first are asic_ch 0..31 in
 * wire order (utils_getReg3 after utils_reorderThreshold3, utils.c:
 * 538-555, 663-677)
 */
static void sky_r3_wire(uint8_t r3[24], uint32_t hot_asic_ch, uint32_t th_hot)
{
    uint32_t asic_ch, bit;

    memset(r3, 0, 24);
    for (asic_ch = 0; asic_ch < 32; asic_ch++) {
        uint32_t th = asic_ch == hot_asic_ch ? th_hot : 62;

        for (bit = 0; bit < 6; bit++) {
            if ((th >> (5 - bit)) & 1) {
                uint32_t pos = asic_ch * 6 + bit;

                r3[pos / 8] |= 0x80u >> (pos % 8);
            }
        }
    }
}

/*
 * det-counts-gating (per tick, D-Df): a (fpga_det, pix) counts only with
 * its quarter powered, its grp enabled, its ReadMode bit 1 (readout)
 * and the R3 threshold of asic_ch = the pix's channel below 63.
 * - quarter 0 off: fpga_det 0..7 (grp 0..3) and LTRG grp 0..3 are zero;
 * - grp 5 disabled: fpga_det 10, 11;
 * - grp 7 in ReadMode 0 (config): fpga_det 14, 15;
 * - fpga_det 18 (grp 9, asic 0) with TH = 63 on asic_ch 1 = pix 3
 *   (utils_pixelId2channelId, utils.c:50-53): only that pix is zero.
 * Then a ReadMode pulse of one tick on grp 12 (fpga_det 24, 25) removes
 * one tick of 40 from that grp's exposure, not the whole period.
 */
static void test_det_counts_gating(void)
{
    QTestState *qts = stix_qtest_new();
    SkyBank *b = g_new0(SkyBank, 1);
    uint32_t mode = 0xFFFFu & ~(1u << 7);
    uint32_t fpga_det, pix, grp;
    double ref = 0, ratio;
    uint32_t n_ref = 0;
    uint8_t r3[24];

    g_assert_true(sky_set_u32(qts, "det-quiet-cps", 100000));
    idefx_power(qts, 0xF, 0xFFFF, 0xFFFF);
    sky_r3_wire(r3, 1, 63);             /* asic_ch 1 = pix 3 */
    idefx_write_reg(qts, 9, 0, 3, r3, sizeof(r3));
    idefx_power(qts, 0xE, 0xFFFFu & ~(1u << 5), mode);

    sky_start(qts, 39, 0xFFF);
    det_step_ticks(qts, 40);
    sky_read_bank(qts, 0, b);
    for (fpga_det = 0; fpga_det < SKY_N_FPGA_DET; fpga_det++) {
        uint64_t sum = sky_fpga_det_sum(b, fpga_det);
        bool off = fpga_det < 8 || fpga_det == 10 || fpga_det == 11 ||
                   fpga_det == 14 || fpga_det == 15;

        g_test_message("fpga_det %u: %" PRIu64, fpga_det, sum);
        if (off) {
            g_assert_cmpuint(sum, ==, 0);
        } else {
            g_assert_cmpuint(sum, >, 100000);
        }
    }
    for (pix = 0; pix < SKY_N_PIX; pix++) {
        if (pix == 3) {
            g_assert_cmpuint(sky_pix_sum(b, 18, pix), ==, 0);
        } else {
            g_assert_cmpuint(sky_pix_sum(b, 18, pix), >, 1000);
        }
    }
    for (grp = 0; grp < SKY_N_GRP; grp++) {
        bool off = grp < 4 || grp == 5 || grp == 7;

        if (off) {
            g_assert_cmpuint(b->ltrg_grp[grp], ==, 0);
        } else {
            g_assert_cmpuint(b->ltrg_grp[grp], >, 0);
        }
    }

    /* one tick of 40 with grp 12 in config mode, mid-period */
    det_step_ticks(qts, 10);
    qtest_clock_step(qts, DET_TICK_NS / 2);
    ctrl_write(qts, CTRL_IDEFXMODEDATA, (mode & ~(1u << 12)) << 16);
    det_step_ticks(qts, 1);
    ctrl_write(qts, CTRL_IDEFXMODEDATA, mode << 16);
    qtest_clock_step(qts, DET_TICK_NS / 2);
    det_step_ticks(qts, 28);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 2);
    sky_read_bank(qts, 1, b);
    for (fpga_det = 16; fpga_det < SKY_N_FPGA_DET; fpga_det++) {
        if (fpga_det != 18 && fpga_det != 24 && fpga_det != 25) {
            ref += sky_fpga_det_sum(b, fpga_det);
            n_ref++;
        }
    }
    ref /= n_ref;
    ratio = (sky_fpga_det_sum(b, 24) + sky_fpga_det_sum(b, 25)) / (2 * ref);
    g_test_message("grp 12 after a one-tick ReadMode pulse: %.4f of the "
                   "others (39/40 = 0.975)", ratio);
    g_assert_cmpfloat(ratio, >, 0.965);
    g_assert_cmpfloat(ratio, <, 0.985);

    g_free(b);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * The FSW's R3 layout, rebuilt here from its description and kept
 * independent of the model's decoder (STIX-FSW 600a7a72, utils.c:521-555,
 * 663-677): in software, 6-bit field c (threshold of asic_ch c) sits at
 * bit 6c counted from the LSB of the 24 bytes read as one big-endian
 * number; the wire carries the same fields in reversed order (software
 * field c at LSB field position 31 - c).
 */
static void sky_r3_fsw_wire(uint8_t wire[24], const uint8_t th_sw[32])
{
    uint8_t sw[24] = { 0 };
    uint32_t c, bit;

    for (c = 0; c < 32; c++) {
        for (bit = 0; bit < 6; bit++) {
            uint32_t b = 6 * c + bit;   /* from the LSB */

            if ((th_sw[c] >> bit) & 1) {
                sw[23 - b / 8] |= 1u << (b % 8);
            }
        }
    }
    memset(wire, 0, 24);
    for (c = 0; c < 32; c++) {
        for (bit = 0; bit < 6; bit++) {
            uint32_t from = 6 * c + bit;
            uint32_t to = 6 * (31 - c) + bit;

            if ((sw[23 - from / 8] >> (from % 8)) & 1) {
                wire[23 - to / 8] |= 1u << (to % 8);
            }
        }
    }
}

/*
 * The test's own oracle for pix -> asic_ch (STIX-FSW 600a7a72,
 * utils.c:50-53); asic_ch 31 is the guard ring (FSW "pixel" 12).
 */
static const uint8_t sky_oracle_pix2asic_ch[12] = {
    26, 15, 8, 1, 29, 18, 5, 0, 30, 21, 11, 3
};

/* the pixel on asic_ch @c: 0..11, 12 for the guard ring, -1 for none */
static int sky_oracle_pix_of(uint32_t c)
{
    int pix;

    if (c == 31) {
        return 12;
    }
    for (pix = 0; pix < 12; pix++) {
        if (sky_oracle_pix2asic_ch[pix] == c) {
            return pix;
        }
    }
    return -1;
}

/* software R3 thresholds enabling the pix in @pix_mask, built correctly */
static void sky_th_correct(uint8_t th_sw[32], uint32_t pix_mask)
{
    uint32_t c;

    for (c = 0; c < 32; c++) {
        int pix = sky_oracle_pix_of(c);

        th_sw[c] = pix >= 0 && pix < 12 && (pix_mask >> pix) & 1 ? 62 : 63;
    }
}

/*
 * software R3 thresholds as the FSW's RCR state build makes them
 * (STIX-FSW 600a7a72, RateCtrl.c:898-913): field c takes the pixel of
 * asic_ch 31 - c; the guard-ring slot 12 is read past the end of the
 * 12-entry active list, so @guard_reads_active picks either outcome
 */
static void sky_th_fsw_mirrored(uint8_t th_sw[32], uint32_t pix_mask,
                                bool guard_reads_active)
{
    uint32_t c;

    for (c = 0; c < 32; c++) {
        int pix = sky_oracle_pix_of(31 - c);
        bool on = pix == 12 ? guard_reads_active
                            : pix >= 0 && (pix_mask >> pix) & 1;

        th_sw[c] = on ? 62 : 63;
    }
}

/*
 * det-counts-r3-fsw: R3 written in the FSW's layout gates exactly the
 * expected pix (fpga_det = 2 * grp + asic, all four in grp 0/1):
 * - fpga_det 0: a correct build of the full 12-pix mask -> all 12 count
 *   (what a fixed FSW would give);
 * - fpga_det 1, 2: the FSW's mirrored build of the full mask, the
 *   out-of-bounds guard slot read as inactive / active -> pix {0, 3, 6,
 *   8} / {0, 3, 6, 7, 8} (STIX-FSW issue #1069);
 * - fpga_det 3: a correct build of pix {4, 11} -> only those two.
 */
static void test_det_counts_r3_fsw(void)
{
    static const struct {
        uint32_t fpga_det;
        uint32_t expect_pix_mask;
    } want[] = {
        { 0, 0xFFFu },
        { 1, 1u << 0 | 1u << 3 | 1u << 6 | 1u << 8 },
        { 2, 1u << 0 | 1u << 3 | 1u << 6 | 1u << 7 | 1u << 8 },
        { 3, 1u << 4 | 1u << 11 },
    };
    QTestState *qts = stix_qtest_new();
    SkyBank *b = g_new0(SkyBank, 1);
    uint8_t th[4][32];
    uint8_t r3[24];
    uint32_t i, pix;

    sky_th_correct(th[0], 0xFFFu);
    sky_th_fsw_mirrored(th[1], 0xFFFu, false);
    sky_th_fsw_mirrored(th[2], 0xFFFu, true);
    sky_th_correct(th[3], 1u << 4 | 1u << 11);

    g_assert_true(sky_set_u32(qts, "det-quiet-cps", 100000));
    idefx_power_all(qts);
    for (i = 0; i < ARRAY_SIZE(want); i++) {
        sky_r3_fsw_wire(r3, th[i]);
        idefx_write_reg(qts, want[i].fpga_det / 2, want[i].fpga_det % 2, 3,
                        r3, sizeof(r3));
    }
    sky_start(qts, 39, 0xFFF);
    det_step_ticks(qts, 40);
    sky_read_bank(qts, 0, b);
    for (i = 0; i < ARRAY_SIZE(want); i++) {
        for (pix = 0; pix < SKY_N_PIX; pix++) {
            uint64_t sum = sky_pix_sum(b, want[i].fpga_det, pix);

            g_test_message("fpga_det %u pix %u: %" PRIu64,
                           want[i].fpga_det, pix, sum);
            if ((want[i].expect_pix_mask >> pix) & 1) {
                g_assert_cmpuint(sum, >, 1000);
            } else {
                g_assert_cmpuint(sum, ==, 0);
            }
        }
    }

    g_free(b);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* one period of the default sky with seed @seed into @b */
static void sky_seeded_bank(uint32_t seed, SkyBank *b)
{
    g_autofree char *arg =
        g_strdup_printf("-global grlib-stixctrl.det-seed=%u", seed);
    QTestState *qts = stix_qtest_new_extra(arg);

    g_assert_cmpint(stix_qom_counter(qts, SKY_CTRL_PATH, "det-seed"),
                    ==, seed);
    idefx_power_all(qts);
    sky_start(qts, 39, 0xFFF);
    det_step_ticks(qts, 40);
    sky_read_bank(qts, 0, b);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* det-counts-seed: the same seed and steps give identical banks */
static void test_det_counts_seed(void)
{
    SkyBank *a = g_new0(SkyBank, 1);
    SkyBank *b = g_new0(SkyBank, 1);
    SkyBank *c = g_new0(SkyBank, 1);

    sky_seeded_bank(7, a);
    sky_seeded_bank(7, b);
    sky_seeded_bank(8, c);
    g_assert_cmpuint(sky_total(a), >, 0);
    g_assert_cmpint(memcmp(a, b, sizeof(*a)), ==, 0);
    g_assert_cmpint(memcmp(a, c, sizeof(*a)), !=, 0);
    g_free(a);
    g_free(b);
    g_free(c);
}

/*
 * det-lvar-ltrg: LVAR slots 0..39 each get one tick's count of the
 * VARPMASK pix, VARDMASK fpga_det and VAREMASK ebin (bit i = index i,
 * [A]); their sum matches the masked LACC sum of the bank; LTRG per grp
 * is 1.25 x the events of its fpga_det 2 * grp and 2 * grp + 1; the
 * layout is big-endian with LACC at ((pix * 32 + fpga_det) * 32 + ebin)
 * * 2. Then the clamps at 1 M cps and a 10 s period: LVAR 16 bits, LTRG
 * 24 bits, and LACC wraps at 16 bits (it does not saturate).
 */
static void test_det_lvar_ltrg(void)
{
    QTestState *qts = stix_qtest_new();
    SkyBank *b = g_new0(SkyBank, 1);
    const uint32_t varp_pix = 0x0F0u;           /* pix 4..7 */
    const uint32_t vard_fpga_det = 0x0000FF00u; /* fpga_det 8..15 */
    const uint32_t vare_ebin = 0x7FFE0000u;     /* ebin 17..30 */
    uint64_t masked = 0, slots = 0;
    uint32_t pix, fpga_det, ebin, grp, i, saturated = 0;
    double cell;

    g_assert_true(sky_set_u32(qts, "det-quiet-cps", 20000));
    idefx_power_all(qts);
    sky_start(qts, 39, varp_pix);
    ctrl_write(qts, CTRL_FSMQLVARDMASK, vard_fpga_det);
    ctrl_write(qts, CTRL_FSMQLVAREMASK, vare_ebin);
    det_step_ticks(qts, 40);
    sky_read_bank(qts, 0, b);

    for (pix = 0; pix < SKY_N_PIX; pix++) {
        for (fpga_det = 0; fpga_det < SKY_N_FPGA_DET; fpga_det++) {
            for (ebin = 0; ebin < SKY_N_EBIN; ebin++) {
                if ((varp_pix >> pix & 1) &&
                    (vard_fpga_det >> fpga_det & 1) &&
                    (vare_ebin >> ebin & 1)) {
                    masked += b->lacc[pix][fpga_det][ebin];
                }
            }
        }
    }
    for (i = 0; i < SKY_N_LVAR_SLOT; i++) {
        g_assert_cmpuint(b->lvar_slot[i], >, 0);
        g_assert_cmpuint(b->lvar_slot[i], <, 0x10000u);
        slots += b->lvar_slot[i];
    }
    /* two independent draws of the same mean: sigma = sqrt(2 lambda) */
    g_test_message("LVAR slot sum %" PRIu64 ", masked LACC sum %" PRIu64,
                   slots, masked);
    g_assert_cmpfloat(fabs((double)slots - masked), <=,
                      5 * sqrt(2.0 * masked));

    for (grp = 0; grp < SKY_N_GRP; grp++) {
        uint64_t events = sky_fpga_det_sum(b, 2 * grp) +
                          sky_fpga_det_sum(b, 2 * grp + 1);

        g_assert_cmpuint(b->ltrg_grp[grp], ==, (5 * events + 2) / 4);
    }

    /* layout: 80000 counts per fpga_det; large pix 1/8.88, ebin 18 45 % */
    cell = 80000 / SKY_PIX_W_TOTAL * SKY_QUIET_EBIN18;
    sky_assert_poisson("LACC[pix 0][fpga_det 3][ebin 18]",
                       b->lacc[0][3][18], cell, 5);
    sky_assert_poisson("LACC[pix 9][fpga_det 30][ebin 18]",
                       b->lacc[9][30][18], 0.22 * cell, 5);
    g_assert_cmpuint(b->lacc[0][3][0], ==, 0);
    g_assert_cmpuint(b->lacc[0][3][31], ==, 0);
    qtest_quit(qts);
    stix_qtest_cleanup();

    /* the clamps: 1 M cps per fpga_det, QlIntTime 99 (10 s) */
    qts = stix_qtest_new();
    g_assert_true(sky_set_u32(qts, "det-quiet-cps", 1000000));
    idefx_power_all(qts);
    det_fill_gaps(qts);
    sky_start(qts, 99, 0xFFF);
    ctrl_write(qts, CTRL_FSMQLVAREMASK, 0xFFFFFFFFu);
    det_step_ticks(qts, 100);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 1);
    sky_read_bank(qts, 0, b);
    for (i = 0; i < SKY_N_LVAR_SLOT; i++) {
        g_assert_cmphex(b->lvar_slot[i], ==, 0xFFFFu);
    }
    for (grp = 0; grp < SKY_N_GRP; grp++) {
        g_assert_cmphex(b->ltrg_grp[grp], ==, 0xFFFFFFu);
    }
    /* about 500 k counts per big cell: wrapped, so not all 0xFFFF */
    for (pix = 0; pix < 8; pix++) {
        for (fpga_det = 0; fpga_det < SKY_N_FPGA_DET; fpga_det++) {
            saturated += b->lacc[pix][fpga_det][18] == 0xFFFFu;
        }
    }
    g_assert_cmpuint(saturated, <, 5);
    /* the WDT storage and MemBram are untouched (D-Dd slot rule) */
    det_expect_gaps(qts);
    g_assert_cmphex(qtest_readl(qts, AHB_UNCHANGED), ==, DET_SENTINEL);
    g_assert_cmphex(qtest_readl(qts, AHB_MEMBRAM), ==, DET_SENTINEL);

    g_free(b);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * det-props: the scene knobs (D-Db) on /machine/stixctrl. Defaults,
 * read-back, rejection of out-of-range rates and unknown sources (the
 * old value stays), settable while the VM is stopped (-S, platform
 * plan D-PLm req. 5); they survive system_reset (they describe the
 * Sun), and a reset mid-period leaves no tick behind.
 */
static void test_det_props(void)
{
    QTestState *qts = stix_qtest_new();

    sky_expect_scene(qts, "synthetic", false, 20, 2000);
    g_assert_cmpint(stix_qom_counter(qts, SKY_CTRL_PATH, "det-seed"), ==, 1);

    g_assert_false(sky_set_u32(qts, "det-quiet-cps", 1000001));
    g_assert_false(sky_set_u32(qts, "det-flare-cps", 1000001));
    g_assert_false(sky_set_str(qts, "det-source", "sky"));
    g_assert_false(sky_set_str(qts, "det-source", "replay-dss"));
    g_assert_false(sky_set_str(qts, "det-source", ""));
    sky_expect_scene(qts, "synthetic", false, 20, 2000);
    g_assert_true(sky_set_u32(qts, "det-quiet-cps", 1000000));
    g_assert_true(sky_set_u32(qts, "det-flare-cps", 0));
    sky_expect_scene(qts, "synthetic", false, 1000000, 0);

    g_assert_true(sky_set_str(qts, "det-source", "zero"));
    g_assert_true(stix_qom_set_bool(qts, SKY_CTRL_PATH, "det-flare", true));
    g_assert_true(sky_set_u32(qts, "det-quiet-cps", 33));
    g_assert_true(sky_set_u32(qts, "det-flare-cps", 4444));
    qtest_system_reset(qts);
    sky_expect_scene(qts, "zero", true, 33, 4444);

    /* a reset mid-period: no tick, swap or LVAR write afterwards */
    g_assert_true(sky_set_str(qts, "det-source", "synthetic"));
    idefx_power_all(qts);
    sky_start(qts, 39, 0xFFF);
    det_step_ticks(qts, 15);
    g_assert_cmpuint(ctrl_read(qts, CTRL_FSMEVENTS), >, 0);
    qtest_system_reset(qts);
    det_fill_bank(qts, 0, 0x5EED0000u);
    det_step_ticks(qts, 100);
    det_expect_bank(qts, 0, 0x5EED0000u);
    g_assert_cmphex(det_sta(qts), ==, 0);
    g_assert_cmpuint(ctrl_read(qts, CTRL_FSMEVENTS), ==, 0);
    qtest_quit(qts);
    stix_qtest_cleanup();

    /* settable while stopped at -S */
    qts = stix_qtest_new_extra("-S");
    g_assert_true(stix_qom_set_bool(qts, SKY_CTRL_PATH, "det-flare", true));
    g_assert_true(sky_set_u32(qts, "det-flare-cps", 3000));
    g_assert_true(sky_set_str(qts, "det-source", "zero"));
    sky_expect_scene(qts, "zero", true, 20, 3000);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- Nominal integration and ROTB chunks (detector plan D-5, tier 3) ---- */

#define FSMSTA_ACTIVENOMINAL (1u << 4)

/* ETRG n at 0xE0600000 + n * 0x40 (StixIdpu.h:342-350): u32 per grp */
#define DET_ETRG(n)          (0xE0600000u + (n) * 0x40u)
#define DET_ETRG_SIZE        0x40u

/*
 * The chunk (RotAccumulator.c:150-220, RotAccuCompactor.c:76-270): 49
 * blocks of 512 B; block n starts with the u16 descriptor 0x8000 | n
 * (ROTBUF_DESCRIPTOR_DETECTORS_DATA, block number 5:0), then the SCET
 * (coarse u32 at +2, fine u16 at +6), then 504 data bytes. The data
 * stream is the EACC bank, 12 x 32 x 32 u16 [pix][fpga_det][ebin], the
 * ETRG bank, 16 x (u16 hw, u16 lw) per grp, and a 56 B pad.
 */
#define CHUNK_BLOCKS         49u
#define CHUNK_DESC_DETECTOR  0x8000u
#define CHUNK_DATA_OFF       8u
#define CHUNK_PAD            56u

typedef struct NomChunk {
    uint16_t desc[CHUNK_BLOCKS];
    uint32_t scet_coarse_reg[CHUNK_BLOCKS];     /* as 0xC8 reads */
    uint16_t scet_fine[CHUNK_BLOCKS];
    uint16_t eacc[SKY_N_PIX][SKY_N_FPGA_DET][SKY_N_EBIN];
    uint32_t etrg_grp[SKY_N_GRP];
    uint8_t pad[CHUNK_PAD];
} NomChunk;

/*
 * Read one chunk from the ring as the FSW does: the block at the read
 * pointer through the 0xE0800000 window, then GETBLOCK, 49 times. The
 * decode is the FSW's: type (w0 >> 16) & 0xC000 must be detector data,
 * the block number (w0 >> 16) & 0x3F must run 0..48 (the compactor
 * compares the whole byte, RotAccuCompactor.c:88-96), the counts are
 * read as u16 from +8 of every block, 12288 of them in rot-counter
 * order (pix << 10 | fpga_det << 5 | ebin), then the 16 trigger pairs,
 * then the pad. Asserts the framing; returns the decoded chunk.
 */
static void nom_read_chunk(QTestState *qts, NomChunk *c)
{
    g_autofree uint8_t *stream = g_malloc0(CHUNK_BLOCKS * 504u);
    uint32_t n, i, k = 0, grp;

    for (n = 0; n < CHUNK_BLOCKS; n++) {
        uint8_t blk[512];
        uint32_t w0;

        for (i = 0; i < ROTB_BLOCK_WORDS; i++) {
            stl_be_p(blk + 4 * i, qtest_readl(qts, ROTB_WINDOW + 4u * i));
        }
        rotb_cmd(qts, ROTB_CMD_GETBLOCK);
        w0 = ldl_be_p(blk);
        c->desc[n] = w0 >> 16;
        g_assert_cmphex((w0 >> 16) & 0xC000u, ==, CHUNK_DESC_DETECTOR);
        g_assert_cmpuint((w0 >> 16) & 0x3Fu, ==, n);
        g_assert_cmpuint((uint8_t)(w0 >> 16), ==, n);
        c->scet_coarse_reg[n] = ldl_be_p(blk + 2);
        c->scet_fine[n] = lduw_be_p(blk + 6);
        memcpy(stream + n * 504u, blk + CHUNK_DATA_OFF, 504u);
    }
    for (k = 0; k < SKY_N_PIX * SKY_N_FPGA_DET * SKY_N_EBIN; k++) {
        c->eacc[k >> 10][(k >> 5) & 0x1F][k & 0x1F] =
            lduw_be_p(stream + 2 * k);
    }
    for (grp = 0; grp < SKY_N_GRP; grp++) {
        uint16_t hw = lduw_be_p(stream + 2 * k + 4 * grp);
        uint16_t lw = lduw_be_p(stream + 2 * k + 4 * grp + 2);

        c->etrg_grp[grp] = (uint32_t)hw << 16 | lw;
    }
    memcpy(c->pad, stream + 2 * k + 4 * SKY_N_GRP, CHUNK_PAD);
    g_assert_cmpuint(2 * k + 4 * SKY_N_GRP + CHUNK_PAD, ==,
                     CHUNK_BLOCKS * 504u);
}

static uint64_t nom_fpga_det_sum(const NomChunk *c, uint32_t fpga_det)
{
    uint64_t sum = 0;
    uint32_t pix, ebin;

    for (pix = 0; pix < SKY_N_PIX; pix++) {
        for (ebin = 0; ebin < SKY_N_EBIN; ebin++) {
            sum += c->eacc[pix][fpga_det][ebin];
        }
    }
    return sum;
}

static bool nom_active(QTestState *qts)
{
    return det_sta(qts) & FSMSTA_ACTIVENOMINAL;
}

/* every block's SCET is what 0xC8/0xCC read at the swap */
static void nom_expect_scet_now(QTestState *qts, const NomChunk *c)
{
    uint32_t coarse_reg = ctrl_read(qts, CTRL_LONGTIMERSEC);
    uint32_t fine = ctrl_read(qts, CTRL_LONGTIMERSUBSEC);
    uint32_t n;

    for (n = 0; n < CHUNK_BLOCKS; n++) {
        g_assert_cmphex(c->scet_coarse_reg[n], ==, coarse_reg);
        g_assert_cmphex(c->scet_fine[n], ==, fine & 0xFFFFu);
    }
}

/*
 * det-chunk-format: a nominal integration of 5 s (time rule, no event
 * limit) ends with one 49-block chunk in the ring, decoded the way
 * RotAccumulator / RotAccuCompactor decode it: descriptors 0x8000 | n,
 * n = 0..48; every block carries the SCET 0xC8/0xCC read at the swap
 * (the integration end, ArchiveManager.c:131); the payload is the EACC
 * bank [pix][fpga_det][ebin] and the ETRG bank by grp, then a zero pad.
 * The decode equals what was materialized: every ETRG word is exactly
 * 1.25 x (rounded) the decoded EACC events of its grp's two fpga_det
 * (no SUMDMASK bit is clear), the pix the R3 switched off (fpga_det
 * 18, asic_ch 1 = pix 3) is zero at its own offsets only, nothing lands
 * in ebin 0 or 31, and the total matches the rate. The EACC and ETRG
 * banks read zero afterwards (read and clear). The next integration
 * uses bank 1 and makes the next chunk, 5 s later.
 * RED on the D-4 binary: the ring stays empty.
 */
static void test_det_chunk_format(void)
{
    QTestState *qts = rotb_qtest_new(128);
    NomChunk *c = g_new0(NomChunk, 1);
    uint32_t pix, fpga_det, ebin, grp, coarse_1, fine_1;
    uint64_t total = 0;
    uint8_t r3[24];

    g_assert_true(sky_set_u32(qts, "det-quiet-cps", 1000));
    idefx_power_all(qts);
    sky_r3_wire(r3, 1, 63);             /* asic_ch 1 = pix 3 */
    idefx_write_reg(qts, 9, 0, 3, r3, sizeof(r3));
    idefx_power_all(qts);
    rotb_cfg(qts, true, false);

    sky_start_nom(qts, 39, 0xFFF, 9, 49, 0xFFFFFFFFu);
    det_step_ticks(qts, 49);
    g_assert_cmpuint(rotb_count(qts), ==, 0);
    g_assert_false(nom_active(qts));
    det_step_ticks(qts, 1);
    g_assert_cmpuint(rotb_count(qts), ==, CHUNK_BLOCKS);
    g_assert_true(nom_active(qts));

    nom_read_chunk(qts, c);
    g_assert_cmpuint(rotb_count(qts), ==, 0);
    nom_expect_scet_now(qts, c);
    coarse_1 = c->scet_coarse_reg[0];
    fine_1 = c->scet_fine[0];

    for (pix = 0; pix < SKY_N_PIX; pix++) {
        for (fpga_det = 0; fpga_det < SKY_N_FPGA_DET; fpga_det++) {
            uint64_t sum = 0;

            g_assert_cmpuint(c->eacc[pix][fpga_det][0], ==, 0);
            g_assert_cmpuint(c->eacc[pix][fpga_det][31], ==, 0);
            for (ebin = 0; ebin < SKY_N_EBIN; ebin++) {
                sum += c->eacc[pix][fpga_det][ebin];
            }
            if (fpga_det == 18 && pix == 3) {
                g_assert_cmpuint(sum, ==, 0);
            } else {
                g_assert_cmpuint(sum, >, 0);
            }
            total += sum;
        }
    }
    sky_assert_poisson("chunk EACC total", total,
                       1000.0 * 5 * (32 - 1.0 / SKY_PIX_W_TOTAL), 5);
    for (grp = 0; grp < SKY_N_GRP; grp++) {
        uint64_t ev = nom_fpga_det_sum(c, 2 * grp) +
                      nom_fpga_det_sum(c, 2 * grp + 1);

        g_assert_cmpuint(c->etrg_grp[grp], ==, (5 * ev + 2) / 4);
    }
    for (pix = 0; pix < CHUNK_PAD; pix++) {
        g_assert_cmpuint(c->pad[pix], ==, 0);
    }
    det_expect(qts, DET_EACC(0), DET_EACC_SIZE, 0);
    det_expect(qts, DET_ETRG(0), DET_ETRG_SIZE, 0);

    /* the next integration, in bank 1, gives the next chunk 5 s later */
    det_step_ticks(qts, 49);
    g_assert_cmpuint(rotb_count(qts), ==, 0);
    det_step_ticks(qts, 1);
    g_assert_cmpuint(rotb_count(qts), ==, CHUNK_BLOCKS);
    g_assert_false(nom_active(qts));
    nom_read_chunk(qts, c);
    nom_expect_scet_now(qts, c);
    g_assert_true(c->scet_coarse_reg[0] > coarse_1 ||
                  (c->scet_coarse_reg[0] == coarse_1 &&
                   c->scet_fine[0] > fine_1));
    g_assert_cmpuint(nom_fpga_det_sum(c, 0), >, 0);
    det_expect(qts, DET_EACC(1), DET_EACC_SIZE, 0);
    det_expect(qts, DET_ETRG(1), DET_ETRG_SIZE, 0);

    g_free(c);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * det-nominal-rule (D-Dg): the integration ends at the first tick t
 * (ticks since its start) with ((t >= IntTimeMax + 1) || (SUMCNT >=
 * IntEventsMax)) && t >= IntTimeMin + 1; then ActiveNominal toggles and
 * SUMCNT restarts at 0. With the FSW's 9 / 199 / 1800 the quiet sky (64
 * SUMEMASK events/s) ends on the max time, 20 s; the flare (~4600
 * events per tick) on the min time, 1 s. The parameters are latched at
 * the start of each integration, so a write takes effect at the next
 * one; a Reset strobe restarts the count and re-latches. With an event
 * limit of 100 and no min time the quiet sky ends on the events.
 * RED on the D-4 binary: ActiveNominal never moves.
 */
static void test_det_nominal_rule(void)
{
    QTestState *qts = stix_qtest_new();
    int k;

    idefx_power_all(qts);
    sky_start(qts, 39, 0xFFF);

    /* quiet: the max time, 200 ticks */
    det_step_ticks(qts, 199);
    g_assert_false(nom_active(qts));
    sky_assert_poisson("SUMCNT after 199 quiet ticks",
                       ctrl_read(qts, CTRL_FSMEVENTS),
                       32 * 20 * 19.9 * 0.10, 5);
    det_step_ticks(qts, 1);
    g_assert_true(nom_active(qts));
    g_assert_cmpuint(ctrl_read(qts, CTRL_FSMEVENTS), ==, 0);

    /* flare: 1800 events within a tick, so the min time, 10 ticks */
    g_assert_true(stix_qom_set_bool(qts, SKY_CTRL_PATH, "det-flare", true));
    det_step_ticks(qts, 9);
    g_assert_true(nom_active(qts));
    g_assert_cmpuint(ctrl_read(qts, CTRL_FSMEVENTS), >=, 1800);
    det_step_ticks(qts, 1);
    g_assert_false(nom_active(qts));

    /* a new min time applies from the next integration on */
    nom_set_params(qts, 29, 199, 1800);
    det_step_ticks(qts, 9);
    g_assert_false(nom_active(qts));
    det_step_ticks(qts, 1);
    g_assert_true(nom_active(qts));
    det_step_ticks(qts, 29);
    g_assert_true(nom_active(qts));
    det_step_ticks(qts, 1);
    g_assert_false(nom_active(qts));

    /* quiet again; a new max time applies from the next integration on */
    g_assert_true(stix_qom_set_bool(qts, SKY_CTRL_PATH, "det-flare", false));
    nom_set_params(qts, 9, 49, 1800);
    det_step_ticks(qts, 199);
    g_assert_false(nom_active(qts));
    det_step_ticks(qts, 1);
    g_assert_true(nom_active(qts));
    det_step_ticks(qts, 49);
    g_assert_true(nom_active(qts));
    det_step_ticks(qts, 1);
    g_assert_false(nom_active(qts));

    /* Reset restarts the integration count */
    det_step_ticks(qts, 20);
    det_cmd_or(qts, FSMCMD_RESET);
    det_step_ticks(qts, 49);
    g_assert_false(nom_active(qts));
    det_step_ticks(qts, 1);
    g_assert_true(nom_active(qts));

    /* the event limit: 100 events at ~6.4 per tick, no min time */
    nom_set_params(qts, 0, 199, 100);
    det_cmd_or(qts, FSMCMD_RESET);
    for (k = 1; k <= 40 && nom_active(qts); k++) {
        det_step_ticks(qts, 1);
    }
    g_test_message("event limit 100 reached after %d ticks", k - 1);
    g_assert_false(nom_active(qts));
    g_assert_cmpint(k - 1, >=, 8);
    g_assert_cmpint(k - 1, <=, 30);
    g_assert_cmpuint(ctrl_read(qts, CTRL_FSMEVENTS), ==, 0);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * det-chunk-gating (D-Dg): a chunk is appended only with rotbConfig
 * ENABLE = 1, TESTMODE = 0 and the FSM enabled; in TESTMODE or with
 * ENABLE = 0 the swap still happens (ActiveNominal toggles) but the
 * ring stays as it is; with the FSM disabled nothing swaps.
 * det-rotb=off swaps without chunks.
 * RED on the D-4 binary: no swap.
 */
static void test_det_chunk_gating(void)
{
    QTestState *qts = rotb_qtest_new(256);

    det_start_nom(qts, 39, 0, 9, 0xFFFFFFFFu);  /* 10-tick integrations */

    rotb_cfg(qts, true, true);
    det_step_ticks(qts, 10);
    g_assert_true(nom_active(qts));
    g_assert_cmpuint(rotb_count(qts), ==, 0);

    rotb_cfg(qts, false, false);
    det_step_ticks(qts, 10);
    g_assert_false(nom_active(qts));
    g_assert_cmpuint(rotb_count(qts), ==, 0);

    rotb_cfg(qts, true, false);
    det_step_ticks(qts, 10);
    g_assert_true(nom_active(qts));
    g_assert_cmpuint(rotb_count(qts), ==, CHUNK_BLOCKS);

    det_cmd_or(qts, FSMCMD_CLEARENABLE);
    det_step_ticks(qts, 100);
    g_assert_true(nom_active(qts));
    g_assert_cmpuint(rotb_count(qts), ==, CHUNK_BLOCKS);

    det_cmd_or(qts, FSMCMD_SETENABLE);
    det_step_ticks(qts, 10);
    g_assert_false(nom_active(qts));
    g_assert_cmpuint(rotb_count(qts), ==, 2 * CHUNK_BLOCKS);
    qtest_quit(qts);
    stix_qtest_cleanup();

    qts = stix_qtest_new_extra("-global grlib-stixctrl.rotb-blocks=256 "
                               "-global grlib-stixctrl.det-rotb=off");
    rotb_cfg(qts, true, false);
    det_start_nom(qts, 39, 0, 9, 0xFFFFFFFFu);
    det_step_ticks(qts, 10);
    g_assert_true(nom_active(qts));
    g_assert_cmpuint(rotb_count(qts), ==, 0);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * det-chunk-full (D-Dg "Ring full"): with rotb-blocks=60 the first chunk
 * fits (49 blocks); the second finds 11 free and is dropped whole, with
 * one IRQ 7 pulse; BLOCKCOUNT stays 49 and FULL stays 0. Each further
 * dropped chunk pulses again. Draining the ring returns the first chunk
 * intact, and the next chunk fits again.
 * RED on the D-4 binary: no chunk.
 */
static void test_det_chunk_full(void)
{
    QTestState *qts = rotb_qtest_new(60);
    NomChunk *c = g_new0(NomChunk, 1);

    rotb_cfg(qts, true, false);
    qtest_writel(qts, IRQMP_CLEAR, ROTB_IRQ_BIT);
    det_start_nom(qts, 39, 0, 9, 0xFFFFFFFFu);

    det_step_ticks(qts, 10);
    g_assert_cmpuint(rotb_count(qts), ==, CHUNK_BLOCKS);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & ROTB_IRQ_BIT, ==, 0);

    det_step_ticks(qts, 10);
    g_assert_cmpuint(rotb_count(qts), ==, CHUNK_BLOCKS);
    g_assert_cmpuint(rotb_full(qts), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & ROTB_IRQ_BIT,
                     ==, ROTB_IRQ_BIT);
    qtest_writel(qts, IRQMP_CLEAR, ROTB_IRQ_BIT);

    det_step_ticks(qts, 10);
    g_assert_cmpuint(rotb_count(qts), ==, CHUNK_BLOCKS);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & ROTB_IRQ_BIT,
                     ==, ROTB_IRQ_BIT);
    qtest_writel(qts, IRQMP_CLEAR, ROTB_IRQ_BIT);

    nom_read_chunk(qts, c);
    g_assert_cmpuint(rotb_count(qts), ==, 0);
    det_step_ticks(qts, 10);
    g_assert_cmpuint(rotb_count(qts), ==, CHUNK_BLOCKS);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & ROTB_IRQ_BIT, ==, 0);

    g_free(c);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * det-chunk-sumdmask: SUMDMASK (fpga_det bits) gates EACC [D, RP-0067]:
 * with fpga_det 5 masked its chunk counts are 0 while its QL LACC counts
 * are not (QLACC is not masked). The triggers are not masked [I]: ETRG
 * grp 2 (fpga_det 4, 5) holds 1.25 x the events of both, so it exceeds
 * 1.25 x fpga_det 4's EACC by about 1.25 x 5000; every other grp is
 * exactly 1.25 x its EACC events.
 * RED on the D-4 binary: no chunk.
 */
static void test_det_chunk_sumdmask(void)
{
    QTestState *qts = rotb_qtest_new(128);
    NomChunk *c = g_new0(NomChunk, 1);
    SkyBank *b = g_new0(SkyBank, 1);
    uint64_t extra;
    uint32_t grp;

    g_assert_true(sky_set_u32(qts, "det-quiet-cps", 1000));
    idefx_power_all(qts);
    rotb_cfg(qts, true, false);
    sky_start_nom(qts, 39, 0xFFF, 9, 49, 0xFFFFFFFFu);
    ctrl_write(qts, CTRL_FSMSUMDMASK, ~(1u << 5));
    det_step_ticks(qts, 50);
    g_assert_cmpuint(rotb_count(qts), ==, CHUNK_BLOCKS);

    sky_read_bank(qts, 0, b);
    g_assert_cmpuint(sky_fpga_det_sum(b, 5), >, 1000);
    nom_read_chunk(qts, c);
    g_assert_cmpuint(nom_fpga_det_sum(c, 5), ==, 0);
    g_assert_cmpuint(nom_fpga_det_sum(c, 4), >, 3000);
    for (grp = 0; grp < SKY_N_GRP; grp++) {
        uint64_t ev = nom_fpga_det_sum(c, 2 * grp) +
                      nom_fpga_det_sum(c, 2 * grp + 1);

        if (grp != 2) {
            g_assert_cmpuint(c->etrg_grp[grp], ==, (5 * ev + 2) / 4);
        }
    }
    extra = c->etrg_grp[2] - (5 * nom_fpga_det_sum(c, 4) + 2) / 4;
    g_test_message("ETRG grp 2 beyond fpga_det 4: %" PRIu64, extra);
    g_assert_cmpuint(extra, >, 5000);
    g_assert_cmpuint(extra, <, 7500);

    g_free(b);
    g_free(c);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * det-chunk-guard (D-Dj; the a83fe9a3a5 rule for out-of-MMIO writes):
 * the tick writes the QL and EACC banks and the ROTB ring from timer
 * context, outside any MMIO dispatch, with the device's
 * mem_reentrancy_guard engaged, as the SpW out-of-MMIO paths do; it must
 * release the guard. After timer-driven swaps and chunks every CTRL MMIO
 * path still answers: the detector registers, the composed rotbCmdSta,
 * GETBLOCK and the block window, and IRQ 3 still pulses. A guard left
 * engaged would refuse all of them ("Blocked re-entrant IO").
 * RED on the D-4 binary: no chunk.
 */
static void test_det_chunk_guard(void)
{
    QTestState *qts = rotb_qtest_new(128);

    rotb_cfg(qts, true, false);
    ctrl_write(qts, CTRL_FSMCMD, FSMCMD_QLIRQEN);
    det_start_nom(qts, 9, 0, 9, 0xFFFFFFFFu);
    qtest_writel(qts, IRQMP_CLEAR, DET_IRQ_BIT);
    det_step_ticks(qts, 20);

    g_assert_cmpuint(rotb_count(qts), ==, 2 * CHUNK_BLOCKS);
    g_assert_cmphex(ctrl_read(qts, CTRL_FSMCMD), ==, FSMCMD_QLIRQEN);
    g_assert_cmphex(ctrl_read(qts, CTRL_FSMINTTIME), ==, 9u << 16);
    g_assert_cmphex(det_sta(qts) & FSMSTA_ENABLE, ==, FSMSTA_ENABLE);
    g_assert_true(det_irq_pending(qts));
    g_assert_cmphex(qtest_readl(qts, ROTB_WINDOW) >> 16, ==,
                    CHUNK_DESC_DETECTOR);
    rotb_cmd(qts, ROTB_CMD_GETBLOCK);
    g_assert_cmpuint(rotb_count(qts), ==, 2 * CHUNK_BLOCKS - 1);
    g_assert_cmphex(qtest_readl(qts, ROTB_WINDOW) >> 16, ==,
                    CHUNK_DESC_DETECTOR | 1u);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- det-inject: the index-space end-to-end deposit (plan D-6) ---- */

/* one LACC cell as the FSW reads it: u16 big-endian at
 * ((pix * 32 + fpga_det) * 32 + ebin) * 2 (Detectors.h:68-79) */
static uint16_t inject_lacc_cell(QTestState *qts, uint32_t bank,
                                 uint32_t pix, uint32_t fpga_det,
                                 uint32_t ebin)
{
    uint8_t raw[2];

    qtest_memread(qts, QL_LACC(bank) +
                  ((pix * 32u + fpga_det) * 32u + ebin) * 2u, raw, 2);
    return lduw_be_p(raw);
}

/*
 * det-inject-lacc: a deposit at (fpga_det F, pix P, ebin B) through the
 * write-only det-inject property lands exactly at the big-endian u16
 * LACC offset ((P * 32 + F) * 32 + B) * 2 of the finished bank, and
 * nowhere else: with the zero sky every neighbour cell (F +- 1, P +- 1,
 * B +- 1) and the whole bank total stay at N. Three deposits on three
 * different banks, covering the range ends.
 * RED on the D-5 binary: no det-inject property, the bank stays 0.
 */
static void test_det_inject_lacc(void)
{
    static const struct {
        uint32_t fpga_det, pix, ebin, n;
    } dep[] = {
        { 5, 0, 20, 250 },
        { 16, 7, 3, 1000 },
        { 30, 11, 30, 61 },
    };
    static const struct { int df, dp, db; } nb[] = {
        { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 },
        { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 },
    };
    QTestState *qts =
        stix_qtest_new_extra("-global grlib-stixctrl.det-source=zero");
    SkyBank *b = g_new0(SkyBank, 1);
    uint32_t i, k, bank;

    idefx_power_all(qts);
    sky_start(qts, 39, 0xFFF);
    for (i = 0; i < ARRAY_SIZE(dep); i++) {
        g_autofree char *val = g_strdup_printf(
            "fpga_det=%u,pix=%u,ebin=%u,count=%u",
            dep[i].fpga_det, dep[i].pix, dep[i].ebin, dep[i].n);

        g_assert_true(sky_set_str(qts, "det-inject", val));
        det_step_ticks(qts, 40);
        g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, (i + 1) & 3);
        bank = i & 3;
        sky_read_bank(qts, bank, b);
        /* the raw offset and byte order, then the same cell decoded */
        g_assert_cmphex(inject_lacc_cell(qts, bank, dep[i].pix,
                                         dep[i].fpga_det, dep[i].ebin),
                        ==, dep[i].n);
        g_assert_cmpuint(b->lacc[dep[i].pix][dep[i].fpga_det]
                         [dep[i].ebin], ==, dep[i].n);
        for (k = 0; k < ARRAY_SIZE(nb); k++) {
            int32_t fn = (int32_t)dep[i].fpga_det + nb[k].df;
            int32_t pn = (int32_t)dep[i].pix + nb[k].dp;
            int32_t bn = (int32_t)dep[i].ebin + nb[k].db;

            if (fn < 0 || fn > 31 || pn < 0 || pn > 11 || bn < 0 ||
                bn > 31) {
                continue;
            }
            g_assert_cmpuint(b->lacc[pn][fn][bn], ==, 0);
        }
        g_assert_cmpuint(sky_total(b), ==, dep[i].n);
    }

    g_free(b);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * det-inject-chunk: the same deposit lands in EACC, so the next chunk's
 * payload holds it at the matching offset (the chunk stream is the EACC
 * bank [pix][fpga_det][ebin], RotAccuCompactor.c:174-245), and ETRG of
 * grp = fpga_det / 2 grows by 1.25 x N (rounded), every other grp
 * staying 0. EACC keeps the SUMDMASK rule (D-Df): a deposit at an
 * fpga_det outside SUMDMASK stays out of the chunk's EACC and counts in
 * ETRG only, through the nominal exposure's extra-trigger draw.
 * RED on the D-5 binary: no det-inject property, the ring stays empty.
 */
static void test_det_inject_chunk(void)
{
    /* the zero sky, so the neighbours and ETRG are exact */
    QTestState *qts = stix_qtest_new_extra(
        "-global grlib-stixctrl.rotb-blocks=128 "
        "-global grlib-stixctrl.det-source=zero");
    NomChunk *c = g_new0(NomChunk, 1);
    uint64_t extra, total;
    uint32_t fpga_det, grp;

    idefx_power_all(qts);
    rotb_cfg(qts, true, false);
    sky_start_nom(qts, 39, 0xFFF, 9, 49, 0xFFFFFFFFu);
    g_assert_true(sky_set_str(qts, "det-inject",
                              "fpga_det=5,pix=0,ebin=20,count=250"));
    det_step_ticks(qts, 50);
    g_assert_cmpuint(rotb_count(qts), ==, CHUNK_BLOCKS);
    nom_read_chunk(qts, c);
    g_assert_cmpuint(c->eacc[0][5][20], ==, 250);
    /* the neighbours F +- 1, P + 1, B +- 1 stay zero */
    g_assert_cmpuint(c->eacc[0][4][20], ==, 0);
    g_assert_cmpuint(c->eacc[0][6][20], ==, 0);
    g_assert_cmpuint(c->eacc[1][5][20], ==, 0);
    g_assert_cmpuint(c->eacc[0][5][19], ==, 0);
    g_assert_cmpuint(c->eacc[0][5][21], ==, 0);
    g_assert_cmpuint(nom_fpga_det_sum(c, 5), ==, 250);
    /* the whole chunk's EACC sums to the deposit, like the LACC total */
    total = 0;
    for (fpga_det = 0; fpga_det < SKY_N_FPGA_DET; fpga_det++) {
        total += nom_fpga_det_sum(c, fpga_det);
    }
    g_assert_cmpuint(total, ==, 250);
    g_assert_cmpuint(c->etrg_grp[2], ==, (5 * 250 + 2) / 4);
    for (grp = 0; grp < SKY_N_GRP; grp++) {
        if (grp != 2) {
            g_assert_cmpuint(c->etrg_grp[grp], ==, 0);
        }
    }
    /* read and clear: the bank is zero after the swap */
    det_expect(qts, DET_EACC(0), DET_EACC_SIZE, 0);
    det_expect(qts, DET_ETRG(0), DET_ETRG_SIZE, 0);

    /* the SUMDMASK rule: fpga_det 7 masked, so its deposit is ETRG-only */
    ctrl_write(qts, CTRL_FSMSUMDMASK, ~(1u << 7));
    g_assert_true(sky_set_str(qts, "det-inject",
                              "fpga_det=7,pix=3,ebin=10,count=500"));
    det_step_ticks(qts, 50);
    g_assert_cmpuint(rotb_count(qts), ==, CHUNK_BLOCKS);
    nom_read_chunk(qts, c);
    g_assert_cmpuint(c->eacc[3][7][10], ==, 0);
    g_assert_cmpuint(nom_fpga_det_sum(c, 7), ==, 0);
    /* grp 3 = fpga_det 6 (zero sky, no deposit) + 1.25 x ~500 extra */
    extra = c->etrg_grp[3];
    g_test_message("ETRG grp 3 from the masked deposit: %" PRIu64, extra);
    g_assert_cmpuint(extra, >, 400);
    g_assert_cmpuint(extra, <, 850);
    for (grp = 0; grp < SKY_N_GRP; grp++) {
        if (grp != 3) {
            g_assert_cmpuint(c->etrg_grp[grp], ==, 0);
        }
    }

    g_free(c);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * det-inject-props: the write-only det-inject rejects a missing key, an
 * unknown key, a duplicate key, an empty or malformed value, a value
 * that is not a number, count 0 and an index outside its space
 * (fpga_det > 31, pix > 11 — the guard ring is never accumulated —,
 * ebin > 31); a rejected set stores nothing, so the bank then holds
 * only the one valid deposit. The keys may come in any order, and the
 * space ends are inclusive.
 * RED on the D-5 binary: no det-inject property.
 */
static void test_det_inject_props(void)
{
    static const char *const bad[] = {
        "fpga_det=5,pix=0,ebin=20",                     /* count missing */
        "fpga_det=5,pix=0,count=250",                   /* ebin missing */
        "pix=0,ebin=20,count=250",                      /* fpga_det miss. */
        "fpga_det=5,pix=0,ebin=20,count=250,subcol=9",  /* unknown key */
        "fpga_det=5,pix=0,ebin=20,count=250,count=1",   /* duplicate */
        "fpga_det=32,pix=0,ebin=20,count=250",          /* fpga_det > 31 */
        "fpga_det=5,pix=12,ebin=20,count=250",          /* pix > 11 */
        "fpga_det=5,pix=0,ebin=32,count=250",           /* ebin > 31 */
        "fpga_det=5,pix=0,ebin=20,count=0",             /* no counts */
        "fpga_det=x,pix=0,ebin=20,count=250",           /* not a number */
        "fpga_det=5,pix=0,ebin=20,count=+250",          /* leading '+' */
        "fpga_det= 5,pix=0,ebin=20,count=250",          /* leading space */
        "fpga_det=5, pix=0, ebin=20, count=250",        /* a space */
        "fpga_det=5,,pix=0,ebin=20,count=250",          /* empty field */
        "",
        "fpga_det=5",
    };
    QTestState *qts =
        stix_qtest_new_extra("-global grlib-stixctrl.det-source=zero");
    SkyBank *b = g_new0(SkyBank, 1);
    uint32_t i;

    idefx_power_all(qts);
    sky_start(qts, 39, 0xFFF);

    /* write-only (D-Db): qom-get answers "is not readable" */
    g_assert_null(sky_get_str(qts, "det-inject"));
    g_assert_true(sky_set_str(qts, "det-inject",
                              "count=250,ebin=20,pix=0,fpga_det=5"));
    for (i = 0; i < ARRAY_SIZE(bad); i++) {
        g_test_message("bad: '%s'", bad[i]);
        g_assert_false(sky_set_str(qts, "det-inject", bad[i]));
    }
    det_step_ticks(qts, 40);
    sky_read_bank(qts, 0, b);
    g_assert_cmpuint(b->lacc[0][5][20], ==, 250);
    g_assert_cmpuint(sky_total(b), ==, 250);

    /* the space ends are inclusive */
    g_assert_true(sky_set_str(qts, "det-inject",
                              "fpga_det=31,pix=11,ebin=31,count=1"));
    det_step_ticks(qts, 40);
    sky_read_bank(qts, 1, b);
    g_assert_cmpuint(b->lacc[11][31][31], ==, 1);
    g_assert_cmpuint(sky_total(b), ==, 1);

    g_free(b);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * det-inject-gated: a deposit at a (fpga_det, pix) pair that
 * stix_det_pix_gate has off at the applying tick is dropped whole —
 * the gate check comes before the inj_ql increment — while the same
 * deposit at a gated-on pair in the same run lands. The gate: TH = 63
 * on asic_ch 1 = pix 3 of fpga_det 18 (as det-chunk-format sets it),
 * so 18/3 is off and 18/2 is on. The dropped deposit leaves the
 * finished LACC bank all zero and the next chunk's EACC (and ETRG) all
 * zero; the landed one shows at its own cell in both.
 * RED on the D-5 binary: no det-inject property.
 */
static void test_det_inject_gated(void)
{
    /* the zero sky, so the banks and the chunk are exact */
    QTestState *qts = stix_qtest_new_extra(
        "-global grlib-stixctrl.rotb-blocks=128 "
        "-global grlib-stixctrl.det-source=zero");
    NomChunk *c = g_new0(NomChunk, 1);
    SkyBank *b = g_new0(SkyBank, 1);
    uint32_t pix, fpga_det, ebin, grp;
    uint8_t r3[24];

    idefx_power_all(qts);
    sky_r3_wire(r3, 1, 63);             /* asic_ch 1 = pix 3 */
    idefx_write_reg(qts, 9, 0, 3, r3, sizeof(r3));
    idefx_power_all(qts);
    rotb_cfg(qts, true, false);
    sky_start_nom(qts, 39, 0xFFF, 9, 49, 0xFFFFFFFFu);

    /* gated off: nothing lands, not even a trigger */
    g_assert_true(sky_set_str(qts, "det-inject",
                              "fpga_det=18,pix=3,ebin=20,count=250"));
    det_step_ticks(qts, 40);            /* the QL swap */
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 1);
    det_expect(qts, QL_LACC(0), QL_LACC_SIZE, 0);
    det_step_ticks(qts, 10);            /* the nominal swap */
    g_assert_cmpuint(rotb_count(qts), ==, CHUNK_BLOCKS);
    nom_read_chunk(qts, c);
    for (pix = 0; pix < SKY_N_PIX; pix++) {
        for (fpga_det = 0; fpga_det < SKY_N_FPGA_DET; fpga_det++) {
            for (ebin = 0; ebin < SKY_N_EBIN; ebin++) {
                g_assert_cmpuint(c->eacc[pix][fpga_det][ebin], ==, 0);
            }
        }
    }
    for (grp = 0; grp < SKY_N_GRP; grp++) {
        g_assert_cmpuint(c->etrg_grp[grp], ==, 0);
    }
    det_expect(qts, DET_EACC(0), DET_EACC_SIZE, 0);
    det_expect(qts, DET_ETRG(0), DET_ETRG_SIZE, 0);

    /* the same deposit at the gated-on 18/2 lands in both */
    g_assert_true(sky_set_str(qts, "det-inject",
                              "fpga_det=18,pix=2,ebin=20,count=250"));
    det_step_ticks(qts, 30);            /* the next QL swap */
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 2);
    sky_read_bank(qts, 1, b);
    g_assert_cmpuint(b->lacc[2][18][20], ==, 250);
    g_assert_cmpuint(sky_total(b), ==, 250);
    det_step_ticks(qts, 20);            /* the next nominal swap */
    g_assert_cmpuint(rotb_count(qts), ==, CHUNK_BLOCKS);
    nom_read_chunk(qts, c);
    g_assert_cmpuint(c->eacc[2][18][20], ==, 250);
    g_assert_cmpuint(nom_fpga_det_sum(c, 18), ==, 250);
    g_assert_cmpuint(c->etrg_grp[9], ==, (5 * 250 + 2) / 4);

    g_free(c);
    g_free(b);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * det-inject-reset: a device reset drops a pending det-inject
 * (in-flight model state, not a scene knob): the deposit is set, the
 * reset (the qtest system_reset the other tests use) comes before any
 * tick, and after the scene is re-armed — with the pair gated on, so a
 * surviving request would land at the first tick — the LACC bank past
 * the QL swap is all zero, and SUMCNT with it.
 * RED on the D-5 binary: no det-inject property.
 */
static void test_det_inject_reset(void)
{
    QTestState *qts =
        stix_qtest_new_extra("-global grlib-stixctrl.det-source=zero");

    idefx_power_all(qts);
    sky_start(qts, 39, 0xFFF);
    g_assert_true(sky_set_str(qts, "det-inject",
                              "fpga_det=5,pix=0,ebin=20,count=250"));
    /* the reset lands before any tick applies the deposit */
    qtest_system_reset(qts);
    idefx_power_all(qts);
    sky_start(qts, 39, 0xFFF);
    det_step_ticks(qts, 40);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 1);
    det_expect(qts, QL_LACC(0), QL_LACC_SIZE, 0);
    g_assert_cmpuint(ctrl_read(qts, CTRL_FSMEVENTS), ==, 0);

    qtest_quit(qts);
    stix_qtest_cleanup();
}


/* ---- DSS replay (detector plan D-7) ---- */

/*
 * The .dssevs word [V, detector plan "The user's .dssevs file"]:
 * big-endian u32, [31:20] val12 | [19:16] type | [15:11] dss_det |
 * [10:0] wait11 (20 ns units). Types 0x0-0xB are events (type = pix,
 * val = adc), 0xD/0xE one setup word per dss_det (the 0xE value is the
 * baseline), 0xF wait-only padding. fpga_det = dss_det (plan "Index
 * spaces"). Each test generates its own small synthetic file in a temp
 * dir; the user's file is never read.
 */
#define DET7_WORD(val, type, dss_det, wait) \
    (((uint32_t)(val) << 20) | ((uint32_t)(type) << 16) | \
     ((uint32_t)(dss_det) << 11) | (wait))
#define DET7_PAD_WORD   DET7_WORD(2048, 0xF, 0, 0x7FF)
#define DET7_PAD_NS     (2047 * 20)         /* one 0xF word, 40.94 us */
/* the FPGA ELUT row (pix, fpga_det) at the start of the SRAM window */
#define DET7_ELUT_ADDR(pix, fpga_det) \
    (AHB_SRAM + ((pix) * 32u + (fpga_det)) * 4096u)

/* env keys handing the det-replay-missing subprocess its parameters */
#define DET7_TEST_GLOBAL_ENV "STIX_TEST_DET7_GLOBAL"
#define DET7_TEST_FLASH_ENV  "STIX_TEST_DET7_FLASH"
/* the replay event cap (STIX_DET_REPLAY_MAX_EVENTS in stix_det.h) */
#define DET7_REPLAY_MAX_EVENTS (1u << 26)

typedef struct Det7File {
    uint32_t *w;
    size_t n, alloc;
} Det7File;

static void det7_word(Det7File *f, uint32_t w)
{
    if (f->n == f->alloc) {
        f->alloc = f->alloc ? f->alloc * 2 : 4096;
        f->w = g_renew(uint32_t, f->w, f->alloc);
    }
    f->w[f->n++] = w;
}

/*
 * the setup header: one 0xD (unused) and one 0xE (baseline 2000) word
 * per dss_det, alternating as the real file does [V]
 */
static void det7_setup(Det7File *f)
{
    uint32_t dss_det;

    for (dss_det = 0; dss_det < 32; dss_det++) {
        det7_word(f, DET7_WORD(75, 0xD, dss_det, 0));
        det7_word(f, DET7_WORD(2000, 0xE, dss_det, 0));
    }
}

/* burn @ns with 0xF padding words, like the real file's wait-only tail */
static void det7_pad(Det7File *f, uint64_t ns)
{
    while (ns >= DET7_PAD_NS) {
        det7_word(f, DET7_PAD_WORD);
        ns -= DET7_PAD_NS;
    }
    if (ns) {
        det7_word(f, DET7_WORD(2048, 0xF, 0, ns / 20));
    }
}

/*
 * write the word stream as a .dssevs in a fresh temp dir; the caller
 * unlinks *path and rmdir's *dir when done
 */
static void det7_write(const Det7File *f, gchar **dir, gchar **path)
{
    GError *err = NULL;
    FILE *fh;
    size_t i;

    *dir = g_dir_make_tmp("stix-det-replay-XXXXXX", &err);
    g_assert_no_error(err);
    *path = g_build_filename(*dir, "scenario.dssevs", NULL);
    fh = fopen(*path, "wb");
    g_assert_nonnull(fh);
    for (i = 0; i < f->n; i++) {
        uint8_t b[4];

        stl_be_p(b, f->w[i]);
        g_assert(fwrite(b, 1, 4, fh) == 4);
    }
    g_assert(fclose(fh) == 0);
}

/* ... to a caller-chosen path (the malformed files) */
static void det7_write_path(const char *path, const uint32_t *w, size_t n)
{
    FILE *fh = fopen(path, "wb");
    size_t i;

    g_assert_nonnull(fh);
    for (i = 0; i < n; i++) {
        uint8_t b[4];

        stl_be_p(b, w[i]);
        g_assert(fwrite(b, 1, 4, fh) == 4);
    }
    g_assert(fclose(fh) == 0);
}

static QTestState *det7_qtest(const char *replay_file, const char *extra)
{
    g_autofree gchar *all = g_strdup_printf(
        "-global grlib-stixctrl.det-replay-file=%s %s", replay_file,
        extra ? extra : "");

    return stix_qtest_new_extra(all);
}

/* a small -D trace log, as text */
static gchar *det7_read_log(const char *path)
{
    gchar *text = NULL;

    g_assert(g_file_get_contents(path, &text, NULL, NULL));
    return text;
}

static int det7_count_str(const char *hay, const char *needle)
{
    int n = 0;
    const char *p = hay;

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p++;
    }
    return n;
}

/*
 * one "key=value" field of the det-replay-status line (D-PLm req 4);
 * the keys are unique and no value holds a comma, so a strstr of
 * "key=" finds the field; NULL when the property is absent
 */
static char *det7_status_field(QTestState *qts, const char *key)
{
    g_autofree char *line = sky_get_str(qts, "det-replay-status");
    g_autofree char *pat = g_strdup_printf("%s=", key);
    const char *p = line ? strstr(line, pat) : NULL;
    const char *end;

    if (!p) {
        return NULL;
    }
    p += strlen(pat);
    end = strchr(p, ',');
    return g_strndup(p, end ? (size_t)(end - p) : strlen(p));
}

/*
 * the honoured test row: ebin = adc/128, with 32 (unassigned) for
 * adc < 256 (the plan's det-replay-elut example)
 */
static void det7_elut_row_honoured(uint8_t *row)
{
    uint32_t adc;

    for (adc = 0; adc < 4096; adc++) {
        row[adc] = adc < 256 ? 32 : adc / 128;
    }
}

/*
 * det-replay-decode: the word decode. 0x0-0xB are events with
 * pix = type and fpga_det = dss_det, 0xD/0xE are setup (the baseline
 * 2000 comes from the 0xE words), 0xF is wait only — and every word's
 * wait sums into the replay time: the t_tick-0 events land in bank 0
 * (ticks 1-10 at QlIntTime 9), the event after 2 s of padding (t_tick
 * 20) in bank 1 and the one after 4 s (t_tick 40) in bank 3. The
 * linear fallback (the ELUT is all 0 at realize, every row degenerate)
 * bins (adc - 2000) * 32 / 2096: adc 2001 -> ebin 0, 2262 -> 4, 2393
 * -> 6, 4095 -> 31; adc at or below the baseline is dropped.
 * RED on the D-6 binary: det-replay-file is an unknown -global, QEMU
 * exits.
 */
static void test_det_replay_decode(void)
{
    Det7File f = { 0 };
    g_autofree gchar *dir = NULL, *path = NULL;
    QTestState *qts;
    SkyBank *b = g_new0(SkyBank, 1);

    det7_setup(&f);
    det7_word(&f, DET7_WORD(2001, 3, 2, 0));    /* ebin 0 */
    det7_word(&f, DET7_WORD(2262, 2, 9, 0));    /* ebin 4 */
    det7_word(&f, DET7_WORD(2000, 2, 9, 0));    /* adc == base: dropped */
    det7_word(&f, DET7_WORD(1999, 2, 9, 0));    /* adc < base: dropped */
    det7_pad(&f, 2 * NSEC_PER_SEC);             /* -> t_tick 20 */
    det7_word(&f, DET7_WORD(2393, 5, 10, 0));   /* ebin 6 */
    det7_pad(&f, 2 * NSEC_PER_SEC);             /* -> t_tick 40 */
    det7_word(&f, DET7_WORD(4095, 0, 15, 0));   /* ebin 31 (clamped) */
    det7_write(&f, &dir, &path);

    qts = det7_qtest(path, NULL);
    g_assert_true(sky_set_str(qts, "det-source", "replay-dss"));
    idefx_power_all(qts);
    sky_start(qts, 9, 0xFFF);       /* SetEnable starts the replay */
    /*
     * each bank is read right after its own swap: the swap zeroes the
     * newly active bank, so after 40 ticks bank 0 is active (and
     * zeroed) again
     */
    det_step_ticks(qts, 10);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 1);
    sky_read_bank(qts, 0, b);
    g_assert_cmpuint(b->lacc[3][2][0], ==, 1);
    g_assert_cmpuint(b->lacc[2][9][4], ==, 1);
    g_assert_cmpuint(sky_total(b), ==, 2);
    g_assert_cmpuint(b->ltrg_grp[1], ==, 1);    /* fpga_det 2 -> grp 1 */
    det_step_ticks(qts, 10);
    sky_read_bank(qts, 1, b);
    g_assert_cmpuint(b->lacc[5][10][6], ==, 1);
    g_assert_cmpuint(sky_total(b), ==, 1);
    g_assert_cmpuint(b->ltrg_grp[5], ==, 1);    /* fpga_det 10 -> grp 5 */
    det_step_ticks(qts, 10);
    sky_read_bank(qts, 2, b);
    g_assert_cmpuint(sky_total(b), ==, 0);
    det_step_ticks(qts, 10);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 0);
    sky_read_bank(qts, 3, b);
    g_assert_cmpuint(b->lacc[0][15][31], ==, 1);
    g_assert_cmpuint(sky_total(b), ==, 1);
    g_assert_cmpuint(b->ltrg_grp[7], ==, 1);    /* fpga_det 15 -> grp 7 */
    /* SUMCNT: the events in SUMEMASK 0xFFE (ebin 4 and 6) */
    g_assert_cmpuint(ctrl_read(qts, CTRL_FSMEVENTS), ==, 2);

    g_free(b);
    qtest_quit(qts);
    stix_qtest_cleanup();
    g_unlink(path);
    g_rmdir(dir);
    g_free(f.w);
}

/*
 * det-replay-elut: both ELUT branches, rows independent, and the branch
 * switch at the next QL swap. The qtest writes the rows into
 * 0xE0000000 + (pix * 32 + fpga_det) * 4096 directly:
 * - (fpga_det 5, pix 0) honoured (ebin = adc/128, 32 for adc < 256,
 *   dss_det 5's baseline lowered to 64 so the 32-entry region is above
 *   it): adc 2000 -> ebin 15, adc 100 (a 32 entry) dropped — the
 *   fallback would have binned it 0;
 * - (9,2) all-31 and (10,0) all-0 and (15,1) all-31 degenerate: the
 *   linear fallback from the 0xE baselines;
 * - (9,2) rewritten to honoured after the swap at tick 10 switches
 *   branch at the swap at tick 20: its adc-2178 event lands in ebin 17
 *   (the fallback would give 2).
 * stixdet_elut_fallback fires once per degenerate row at the
 * classification (the SetEnable edge), never for the honoured row and
 * never again for the rewritten one.
 * RED on the D-6 binary: det-replay-file is an unknown -global.
 */
static void test_det_replay_elut(void)
{
    Det7File f = { 0 };
    g_autofree uint8_t *row = g_malloc(4096);
    g_autofree gchar *dir = NULL, *path = NULL, *log = NULL, *extra = NULL;
    g_autofree gchar *text = NULL;
    QTestState *qts;
    SkyBank *b = g_new0(SkyBank, 1);
    int fd;

    det7_setup(&f);
    det7_word(&f, DET7_WORD(64, 0xE, 5, 0));    /* dss_det 5 base 64 */
    det7_word(&f, DET7_WORD(2262, 1, 15, 0));   /* all-31 row: ebin 4 */
    det7_word(&f, DET7_WORD(2000, 0, 5, 0));    /* honoured: ebin 15 */
    det7_word(&f, DET7_WORD(100, 0, 5, 0));     /* honoured 32: dropped */
    det7_word(&f, DET7_WORD(2393, 0, 10, 0));   /* all-0 row: ebin 6 */
    det7_word(&f, DET7_WORD(2262, 2, 9, 0));    /* all-31 row: ebin 4 */
    det7_pad(&f, 12 * 100 * 1000 * 1000);       /* -> t_tick 12 */
    det7_word(&f, DET7_WORD(2393, 2, 9, 0));    /* still fallback: 6 */
    det7_pad(&f, 13 * 100 * 1000 * 1000);       /* -> t_tick 25 */
    det7_word(&f, DET7_WORD(2178, 2, 9, 0));    /* honoured now: 17 */
    det7_write(&f, &dir, &path);

    fd = g_file_open_tmp("stix-det-replay-trace-XXXXXX", &log, NULL);
    g_assert(fd >= 0);
    close(fd);
    extra = g_strdup_printf("-trace stixdet_elut_fallback -D %s", log);
    qts = det7_qtest(path, extra);

    det7_elut_row_honoured(row);
    qtest_memwrite(qts, DET7_ELUT_ADDR(0, 5), row, 4096);
    memset(row, 31, 4096);
    qtest_memwrite(qts, DET7_ELUT_ADDR(2, 9), row, 4096);
    qtest_memwrite(qts, DET7_ELUT_ADDR(1, 15), row, 4096);
    memset(row, 0, 4096);
    qtest_memwrite(qts, DET7_ELUT_ADDR(0, 10), row, 4096);

    g_assert_true(sky_set_str(qts, "det-source", "replay-dss"));
    idefx_power_all(qts);
    sky_start(qts, 9, 0xFFF);
    det_step_ticks(qts, 10);            /* bank 0: the t_tick-0 events */
    sky_read_bank(qts, 0, b);
    g_assert_cmpuint(b->lacc[1][15][4], ==, 1);
    g_assert_cmpuint(b->lacc[0][5][15], ==, 1);
    g_assert_cmpuint(b->lacc[0][5][0], ==, 0);
    g_assert_cmpuint(b->lacc[0][10][6], ==, 1);
    g_assert_cmpuint(b->lacc[2][9][4], ==, 1);
    g_assert_cmpuint(sky_total(b), ==, 4);

    /* the row switch between two swaps takes effect at the next one */
    det7_elut_row_honoured(row);
    qtest_memwrite(qts, DET7_ELUT_ADDR(2, 9), row, 4096);
    det_step_ticks(qts, 20);            /* banks 1 and 2 */
    sky_read_bank(qts, 1, b);
    g_assert_cmpuint(b->lacc[2][9][6], ==, 1);
    g_assert_cmpuint(sky_total(b), ==, 1);
    sky_read_bank(qts, 2, b);
    g_assert_cmpuint(b->lacc[2][9][17], ==, 1);
    g_assert_cmpuint(sky_total(b), ==, 1);

    text = det7_read_log(log);
    g_assert_cmpint(det7_count_str(text, "fpga_det 9 pix 2 degenerate"),
                    ==, 1);
    g_assert_cmpint(det7_count_str(text, "fpga_det 10 pix 0 degenerate"),
                    ==, 1);
    g_assert_cmpint(det7_count_str(text, "fpga_det 15 pix 1 degenerate"),
                    ==, 1);
    g_assert_cmpint(det7_count_str(text, "fpga_det 5 pix 0 degenerate"), ==, 0);

    g_free(b);
    qtest_quit(qts);
    stix_qtest_cleanup();
    unlink(log);
    g_unlink(path);
    g_rmdir(dir);
    g_free(f.w);
}

/*
 * det-replay-trigger: with trigger nominal (the default) the first
 * events arrive in the first tick after SetEnable and nothing before
 * it (the tick only runs while Enable; the qmp case below pins the
 * start gating); with qmp only det-replay-start=true starts it, false
 * stops it (a start retracted before the next tick never runs) and the
 * banks stay empty; det-replay-offset-ms=1000 skips the t_tick-0
 * event; the end of the file stops the replay (no loop); a
 * system_reset stops it and re-arms the nominal trigger. At the key
 * points det-replay-status (D-PLm req 4) reads armed before the
 * start, running mid-file, ended after the EOF and stopped after a
 * det-replay-start=false, with events= fed so far and count= the
 * table's 3 events.
 * The file: e1 (dss_det 2, pix 3, adc 2262 -> ebin 4) at t_tick 0, e2
 * (dss_det 9, pix 2, adc 2393 -> ebin 6) at t_tick 10, e3 (dss_det 15,
 * pix 0, adc 3492 -> ebin 22) at t_tick 60.
 * RED on the D-6 binary: det-replay-file is an unknown -global.
 */
static void test_det_replay_trigger(void)
{
    Det7File f = { 0 };
    g_autofree gchar *dir = NULL, *path = NULL;
    QTestState *qts;
    SkyBank *b = g_new0(SkyBank, 1);
    bool ok;

    det7_setup(&f);
    det7_word(&f, DET7_WORD(2262, 3, 2, 0));
    det7_pad(&f, NSEC_PER_SEC);                 /* e2 at t_tick 10 */
    det7_word(&f, DET7_WORD(2393, 2, 9, 0));
    det7_pad(&f, 5 * NSEC_PER_SEC);             /* e3 at t_tick 60 */
    det7_word(&f, DET7_WORD(3492, 0, 15, 0));
    det7_write(&f, &dir, &path);

    /* a. trigger nominal: the first tick after SetEnable */
    qts = det7_qtest(path, NULL);
    g_assert_true(sky_set_str(qts, "det-source", "replay-dss"));
    g_assert_cmpstr(sky_get_str(qts, "det-replay-trigger"), ==, "nominal");
    /* armed before the start; the table's 3 events are count= */
    g_assert_cmpstr(det7_status_field(qts, "state"), ==, "armed");
    g_assert_cmpstr(det7_status_field(qts, "events"), ==, "0");
    g_assert_cmpstr(det7_status_field(qts, "count"), ==, "3");
    idefx_power_all(qts);
    sky_start(qts, 0, 0xFFF);       /* a 1-tick period: a swap per tick */
    det_step_ticks(qts, 1);
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 1);
    sky_read_bank(qts, 0, b);
    g_assert_cmpuint(b->lacc[3][2][4], ==, 1);
    g_assert_cmpuint(sky_total(b), ==, 1);
    g_assert_cmpstr(det7_status_field(qts, "state"), ==, "running");
    g_assert_cmpstr(det7_status_field(qts, "events"), ==, "1");
    det_step_ticks(qts, 9);         /* tick 10: e2 */
    sky_read_bank(qts, 1, b);
    g_assert_cmpuint(b->lacc[2][9][6], ==, 1);
    g_assert_cmpuint(sky_total(b), ==, 1);
    g_assert_cmpstr(det7_status_field(qts, "events"), ==, "2");
    sky_read_bank(qts, 2, b);
    g_assert_cmpuint(sky_total(b), ==, 0);
    det_step_ticks(qts, 19);        /* tick 29: e3 not yet */
    sky_read_bank(qts, 3, b);
    g_assert_cmpuint(sky_total(b), ==, 0);
    det_step_ticks(qts, 31);        /* tick 60: e3, then the EOF */
    sky_read_bank(qts, 3, b);
    g_assert_cmpuint(b->lacc[0][15][22], ==, 1);
    g_assert_cmpuint(sky_total(b), ==, 1);
    g_assert_cmpstr(det7_status_field(qts, "state"), ==, "ended");
    g_assert_cmpstr(det7_status_field(qts, "events"), ==, "3");
    qtest_quit(qts);
    stix_qtest_cleanup();

    /* b. trigger qmp: only det-replay-start starts; false stops */
    qts = det7_qtest(path, NULL);
    g_assert_true(sky_set_str(qts, "det-source", "replay-dss"));
    g_assert_true(sky_set_str(qts, "det-replay-trigger", "qmp"));
    g_assert_cmpstr(sky_get_str(qts, "det-replay-trigger"), ==, "qmp");
    /* armed: loaded, waiting for det-replay-start=true */
    g_assert_cmpstr(det7_status_field(qts, "state"), ==, "armed");
    idefx_power_all(qts);
    sky_start(qts, 9, 0xFFF);
    det_step_ticks(qts, 10);        /* a full period without a start */
    g_assert_false(stix_qom_get_bool(qts, SKY_CTRL_PATH,
                                     "det-replay-start", &ok));
    g_assert_true(ok);
    sky_read_bank(qts, 0, b);
    g_assert_cmpuint(sky_total(b), ==, 0);
    g_assert_true(stix_qom_set_bool(qts, SKY_CTRL_PATH,
                                    "det-replay-start", true));
    det_step_ticks(qts, 10);        /* tick 20: e1 and e2 in bank 1 */
    g_assert_true(stix_qom_get_bool(qts, SKY_CTRL_PATH,
                                    "det-replay-start", &ok));
    sky_read_bank(qts, 1, b);
    g_assert_cmpuint(b->lacc[3][2][4], ==, 1);
    g_assert_cmpuint(b->lacc[2][9][6], ==, 1);
    g_assert_cmpuint(sky_total(b), ==, 2);
    g_assert_cmpstr(det7_status_field(qts, "state"), ==, "running");
    g_assert_cmpstr(det7_status_field(qts, "events"), ==, "2");
    g_assert_true(stix_qom_set_bool(qts, SKY_CTRL_PATH,
                                    "det-replay-start", false));
    g_assert_cmpstr(det7_status_field(qts, "state"), ==, "stopped");
    g_assert_cmpstr(det7_status_field(qts, "events"), ==, "2");
    det_step_ticks(qts, 40);
    /*
     * a start retracted before the next tick (true then false) never
     * runs: the false cancels the stored request (D-Dj), so e3
     * (t_tick 60) never arrives and the banks stay empty
     */
    g_assert_true(stix_qom_set_bool(qts, SKY_CTRL_PATH,
                                    "det-replay-start", true));
    g_assert_true(stix_qom_set_bool(qts, SKY_CTRL_PATH,
                                    "det-replay-start", false));
    det_step_ticks(qts, 10);
    g_assert_false(stix_qom_get_bool(qts, SKY_CTRL_PATH,
                                     "det-replay-start", &ok));
    /* the retracted start never ran: still stopped, events unchanged */
    g_assert_cmpstr(det7_status_field(qts, "state"), ==, "stopped");
    g_assert_cmpstr(det7_status_field(qts, "events"), ==, "2");
    det_expect(qts, QL_LACC(0), QL_LACC_SIZE, 0);
    det_expect(qts, QL_LACC(1), QL_LACC_SIZE, 0);
    det_expect(qts, QL_LACC(2), QL_LACC_SIZE, 0);
    det_expect(qts, QL_LACC(3), QL_LACC_SIZE, 0);
    /* a restart re-feeds from the offset */
    g_assert_true(stix_qom_set_bool(qts, SKY_CTRL_PATH,
                                    "det-replay-start", true));
    det_step_ticks(qts, 10);        /* ticks 71-80 -> bank 3 */
    sky_read_bank(qts, 3, b);
    g_assert_cmpuint(b->lacc[3][2][4], ==, 1);
    g_assert_cmpuint(b->lacc[2][9][6], ==, 1);
    g_assert_cmpuint(sky_total(b), ==, 2);
    g_assert_cmpstr(det7_status_field(qts, "state"), ==, "running");
    g_assert_cmpstr(det7_status_field(qts, "events"), ==, "2");
    qtest_quit(qts);
    stix_qtest_cleanup();

    /*
     * c. the offset skips the events before it (det-replay-offset-ms,
     * D-PLm req 3; 1000 ms = the old det-replay-offset=1); the end of
     * the file stops the replay (no loop). The nominal integration
     * params are set to a 65536-tick max so no nominal swap resets
     * SUMCNT in the 250 ticks this sub-case runs.
     */
    qts = det7_qtest(path, NULL);
    g_assert_true(sky_set_str(qts, "det-source", "replay-dss"));
    g_assert_true(sky_set_u64(qts, "det-replay-offset-ms", 1000));
    g_assert_cmpint(stix_qom_counter(qts, SKY_CTRL_PATH,
                                     "det-replay-offset-ms"), ==, 1000);
    /*
     * over the cap: 429496729600 ms is 2^32 ticks, one past the u32
     * tick count the feeder holds; the value is rejected and the old
     * one kept
     */
    g_assert_false(sky_set_u64(qts, "det-replay-offset-ms",
                               UINT64_C(429496729600)));
    g_assert_cmpint(stix_qom_counter(qts, SKY_CTRL_PATH,
                                     "det-replay-offset-ms"), ==, 1000);
    idefx_power_all(qts);
    sky_start_nom(qts, 39, 0xFFF, 9, 0xFFFF, 1800);
    det_step_ticks(qts, 45);        /* bank 0 done at tick 40 */
    sky_read_bank(qts, 0, b);
    g_assert_cmpuint(b->lacc[3][2][4], ==, 0);  /* e1 skipped */
    g_assert_cmpuint(b->lacc[2][9][6], ==, 1);  /* e2 at the first tick */
    g_assert_cmpuint(sky_total(b), ==, 1);
    det_step_ticks(qts, 40);        /* bank 1 done at tick 80 */
    sky_read_bank(qts, 1, b);
    g_assert_cmpuint(b->lacc[0][15][22], ==, 1);
    g_assert_cmpuint(sky_total(b), ==, 1);
    g_assert_false(stix_qom_get_bool(qts, SKY_CTRL_PATH,
                                     "det-replay-start", &ok));
    /* the EOF ended it: e2 and e3 fed, e1 skipped by the offset */
    g_assert_cmpstr(det7_status_field(qts, "state"), ==, "ended");
    g_assert_cmpstr(det7_status_field(qts, "events"), ==, "2");
    det_step_ticks(qts, 165);       /* tick 250: both banks re-written */
    det_expect(qts, QL_LACC(0), QL_LACC_SIZE, 0);
    det_expect(qts, QL_LACC(1), QL_LACC_SIZE, 0);
    g_assert_cmpuint(ctrl_read(qts, CTRL_FSMEVENTS), ==, 1);
    qtest_quit(qts);
    stix_qtest_cleanup();

    /* d. a system_reset stops the replay and re-arms the trigger */
    qts = det7_qtest(path, NULL);
    g_assert_true(sky_set_str(qts, "det-source", "replay-dss"));
    idefx_power_all(qts);
    sky_start(qts, 39, 0xFFF);
    det_step_ticks(qts, 1);         /* e1 fed, the replay running */
    g_assert_true(stix_qom_get_bool(qts, SKY_CTRL_PATH,
                                    "det-replay-start", &ok));
    g_assert_cmpstr(det7_status_field(qts, "state"), ==, "running");
    qtest_system_reset(qts);
    g_assert_false(stix_qom_get_bool(qts, SKY_CTRL_PATH,
                                     "det-replay-start", &ok));
    /* the reset re-arms: armed again, not stopped (D-PLm req 4) */
    g_assert_cmpstr(det7_status_field(qts, "state"), ==, "armed");
    idefx_power_all(qts);           /* the reset powered the ASICs off */
    sky_start(qts, 39, 0xFFF);      /* the nominal trigger re-armed */
    det_step_ticks(qts, 40);
    sky_read_bank(qts, 0, b);
    g_assert_cmpuint(b->lacc[3][2][4], ==, 1);  /* e1 re-fed */
    g_assert_cmpuint(b->lacc[2][9][6], ==, 1);
    g_assert_cmpuint(sky_total(b), ==, 2);
    det_step_ticks(qts, 40);        /* tick 80: e3 with a fresh clock */
    sky_read_bank(qts, 1, b);
    g_assert_cmpuint(b->lacc[0][15][22], ==, 1);
    g_assert_cmpuint(sky_total(b), ==, 1);
    qtest_quit(qts);
    stix_qtest_cleanup();

    /* e. with trigger qmp a reset stops it and nothing re-arms */
    qts = det7_qtest(path, NULL);
    g_assert_true(sky_set_str(qts, "det-source", "replay-dss"));
    g_assert_true(sky_set_str(qts, "det-replay-trigger", "qmp"));
    idefx_power_all(qts);
    sky_start(qts, 39, 0xFFF);
    g_assert_true(stix_qom_set_bool(qts, SKY_CTRL_PATH,
                                    "det-replay-start", true));
    det_step_ticks(qts, 50);
    sky_read_bank(qts, 0, b);
    g_assert_cmpuint(b->lacc[3][2][4], ==, 1);
    g_assert_cmpuint(b->lacc[2][9][6], ==, 1);
    g_assert_cmpuint(sky_total(b), ==, 2);
    g_assert_cmpstr(det7_status_field(qts, "state"), ==, "running");
    g_assert_cmpstr(det7_status_field(qts, "events"), ==, "2");
    qtest_system_reset(qts);
    /* armed again (the re-arm rule), nothing re-starts it though */
    g_assert_cmpstr(det7_status_field(qts, "state"), ==, "armed");
    idefx_power_all(qts);           /* else the check below is vacuous */
    sky_start(qts, 39, 0xFFF);      /* no auto start with trigger qmp */
    det_step_ticks(qts, 80);
    det_expect(qts, QL_LACC(0), QL_LACC_SIZE, 0);
    det_expect(qts, QL_LACC(1), QL_LACC_SIZE, 0);
    det_expect(qts, QL_LACC(2), QL_LACC_SIZE, 0);
    det_expect(qts, QL_LACC(3), QL_LACC_SIZE, 0);
    g_assert_true(stix_qom_set_bool(qts, SKY_CTRL_PATH,
                                    "det-replay-start", true));
    det_step_ticks(qts, 40);        /* ticks 81-120 -> bank 2 */
    g_assert_cmpuint(FSMSTA_ACTIVEQL(det_sta(qts)), ==, 3);
    sky_read_bank(qts, 2, b);
    g_assert_cmpuint(b->lacc[3][2][4], ==, 1);
    g_assert_cmpuint(sky_total(b), ==, 2);
    qtest_quit(qts);
    stix_qtest_cleanup();

    /*
     * f. det-replay-offset-ms (D-PLm req 3) rounds down to the tick:
     * 5050 ms is tick 50 (not 51), so the event exactly at t_tick 50
     * arrives in the first tick after the start — an offset that
     * rounded up would skip it — and pos_ns reads the tick the feeder
     * is at. The table: one event (dss_det 2, pix 3, adc 2262 -> ebin
     * 4) at t_tick 50; the 32 0xD setup words read unsupported=32.
     */
    {
        Det7File f2 = { 0 };
        g_autofree gchar *dir2 = NULL, *path2 = NULL;

        det7_setup(&f2);
        det7_pad(&f2, 5 * NSEC_PER_SEC);        /* -> t_tick 50 */
        det7_word(&f2, DET7_WORD(2262, 3, 2, 0));
        det7_write(&f2, &dir2, &path2);

        qts = det7_qtest(path2, NULL);
        g_assert_true(sky_set_str(qts, "det-source", "replay-dss"));
        g_assert_true(sky_set_u64(qts, "det-replay-offset-ms", 5050));
        g_assert_cmpint(stix_qom_counter(qts, SKY_CTRL_PATH,
                                         "det-replay-offset-ms"), ==, 5050);
        g_assert_cmpstr(det7_status_field(qts, "unsupported"), ==, "32");
        g_assert_cmpstr(det7_status_field(qts, "count"), ==, "1");
        idefx_power_all(qts);
        sky_start(qts, 0, 0xFFF);   /* a 1-tick period: a swap per tick */
        det_step_ticks(qts, 1);     /* tick 51: the t_tick-50 event due */
        sky_read_bank(qts, 0, b);
        g_assert_cmpuint(b->lacc[3][2][4], ==, 1);
        g_assert_cmpuint(sky_total(b), ==, 1);
        g_assert_cmpstr(det7_status_field(qts, "state"), ==, "ended");
        g_assert_cmpstr(det7_status_field(qts, "events"), ==, "1");
        g_assert_cmpstr(det7_status_field(qts, "pos_ns"), ==,
                        "5100000000");
        g_assert_cmpstr(det7_status_field(qts, "duration_ns"), ==,
                        "5000000000");
        qtest_quit(qts);
        stix_qtest_cleanup();
        g_unlink(path2);
        g_rmdir(dir2);
        g_free(f2.w);
    }

    stix_qtest_cleanup();
    g_unlink(path);
    g_rmdir(dir);
    g_free(f.w);
    g_free(b);
}

/*
 * det-replay-missing: a missing, unreadable or malformed file is a
 * realize error naming the path (the subprocess cases, in the
 * test_plat_realize_error style; the cap case is a sparse file of zero
 * words, each a type-0 event, one word past the event cap), and
 * without a table det-source=replay-dss and det-replay-start=true are
 * rejected while the trigger and offset defaults read back and
 * det-replay-status reads state=idle (D-PLm req 4).
 * RED on the D-6 binary: det-replay-file is an unknown -global, so the
 * subprocess fails with a different message.
 */
static void test_det_replay_missing_subprocess(void)
{
    const char *global = g_getenv(DET7_TEST_GLOBAL_ENV);
    const char *flash = g_getenv(DET7_TEST_FLASH_ENV);
    QTestState *qts;

    g_assert_nonnull(global);
    g_assert_nonnull(flash);
    /* qtest_init aborts: QEMU realize fails and exits */
    qts = qtest_initf("-M leon3_generic "
                      "-global grlib-stixflash.flash-file=%s -global %s",
                      flash, global);
    qtest_quit(qts);
}

static void test_det_replay_missing(void)
{
    g_autofree gchar *flash = NULL;
    g_autofree gchar *dir = g_dir_make_tmp("stix-det-replay-XXXXXX", NULL);
    g_autofree gchar *sub = stix_test_path("det-replay-missing/subprocess");
    g_autofree gchar *absent = g_build_filename(dir, "absent.dssevs", NULL);
    g_autofree gchar *nodir = g_build_filename(dir, "no", "such.dssevs",
                                               NULL);
    g_autofree gchar *truncated = g_build_filename(dir, "truncated.dssevs",
                                                   NULL);
    g_autofree gchar *badtype = g_build_filename(dir, "badtype.dssevs",
                                                 NULL);
    g_autofree gchar *capfile = g_build_filename(dir, "cap.dssevs", NULL);
    g_autofree gchar *g_absent = NULL, *g_nodir = NULL;
    g_autofree gchar *g_trunc = NULL, *g_bad = NULL;
    g_autofree gchar *g_cap = NULL, *g_cap_pat = NULL;
    struct {
        const char *global;
        const char *pattern;
    } cases[] = {
        { NULL, "*det-replay-file: cannot read*absent.dssevs*" },
        { NULL, "*det-replay-file: cannot read*such.dssevs*" },
        { NULL, "*not a multiple of the 4-byte word size*" },
        { NULL, "*word 0 has type 0xC*" },
        { NULL, NULL },
    };
    QTestState *qts;
    Det7File f = { 0 };
    uint32_t badword = DET7_WORD(0, 0xC, 0, 0);
    bool ok;
    int fd;
    size_t i;

    /* a truncated file: one word and a half */
    det7_word(&f, DET7_WORD(2048, 0xF, 0, 0));
    det7_word(&f, DET7_WORD(2048, 0xF, 0, 0));
    det7_write_path(truncated, f.w, f.n);
    g_assert(truncate(truncated, 6) == 0);
    /* a malformed file: one word of the undefined type 0xC */
    det7_write_path(badtype, &badword, 1);
    g_free(f.w);
    f.w = NULL;
    f.n = 0;

    /*
     * the cap: a sparse file of zero words, each a type-0 event, one
     * word past the cap — no 256 MB of disk behind it
     */
    fd = open(capfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    g_assert(fd >= 0);
    g_assert(ftruncate(fd, 4ull * (DET7_REPLAY_MAX_EVENTS + 1)) == 0);
    close(fd);

    fd = g_file_open_tmp("stix-flash-XXXXXX", &flash, NULL);
    g_assert(fd >= 0);
    g_assert(ftruncate(fd, 1024 * 1024) == 0);
    g_setenv(DET7_TEST_FLASH_ENV, flash, TRUE);

    /* without a table: the rejections and the defaults */
    qts = stix_qtest_new();
    g_assert_false(sky_set_str(qts, "det-source", "replay-dss"));
    g_assert_false(stix_qom_set_bool(qts, SKY_CTRL_PATH,
                                     "det-replay-start", true));
    g_assert_false(sky_set_str(qts, "det-replay-trigger", "whenever"));
    g_assert_cmpstr(sky_get_str(qts, "det-replay-trigger"), ==, "nominal");
    g_assert_cmpint(stix_qom_counter(qts, SKY_CTRL_PATH,
                                     "det-replay-offset-ms"), ==, 0);
    g_assert_false(stix_qom_get_bool(qts, SKY_CTRL_PATH,
                                     "det-replay-start", &ok));
    /* idle: no table loaded, no path, nothing to report */
    g_assert_cmpstr(sky_get_str(qts, "det-replay-status"), ==,
                    "state=idle,pos_ns=0,events=0,dropped=0,"
                    "unsupported=0,table=,count=0,duration_ns=0");
    qtest_quit(qts);
    stix_qtest_cleanup();

    g_absent = g_strdup_printf("grlib-stixctrl.det-replay-file=%s", absent);
    g_nodir = g_strdup_printf("grlib-stixctrl.det-replay-file=%s", nodir);
    g_trunc = g_strdup_printf("grlib-stixctrl.det-replay-file=%s",
                              truncated);
    g_bad = g_strdup_printf("grlib-stixctrl.det-replay-file=%s", badtype);
    g_cap = g_strdup_printf("grlib-stixctrl.det-replay-file=%s", capfile);
    g_cap_pat = g_strdup_printf("*word %" PRIu64 " has more than %u "
                                "events (the replay cap)*",
                                (uint64_t)DET7_REPLAY_MAX_EVENTS,
                                DET7_REPLAY_MAX_EVENTS);
    cases[0].global = g_absent;
    cases[1].global = g_nodir;
    cases[2].global = g_trunc;
    cases[3].global = g_bad;
    cases[4].global = g_cap;
    cases[4].pattern = g_cap_pat;
    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_setenv(DET7_TEST_GLOBAL_ENV, cases[i].global, TRUE);
        g_test_trap_subprocess(sub, 30 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr(cases[i].pattern);
    }
    close(fd);
    g_unlink(flash);
    g_unlink(truncated);
    g_unlink(badtype);
    g_unlink(capfile);
    g_rmdir(dir);
}

static void stix_det7_register(void)
{
    qtest_add_func("/sparc/stix/det-replay-missing/subprocess",
                   test_det_replay_missing_subprocess);
    qtest_add_func("/sparc/stix/det-replay-decode", test_det_replay_decode);
    qtest_add_func("/sparc/stix/det-replay-elut", test_det_replay_elut);
    qtest_add_func("/sparc/stix/det-replay-trigger",
                   test_det_replay_trigger);
    qtest_add_func("/sparc/stix/det-replay-missing",
                   test_det_replay_missing);
}


/* ---- Platform controller (PL-1): OBT source, one-shot, inject ---- */

/*
 * The platform-test controller plan (2026-11-11-platform-plan.md) keeps
 * its qtests in this one appended block behind stix_platform_register(),
 * so a merge with the detector branch touches main() in one line.
 */

#define OBT_CUC(coarse, fine) (((uint64_t)(coarse) << 16) | (fine))
#define OBT_DEFAULT_EPOCH     OBT_CUC(0x10000000u, 0)
#define OBT_MASK              0xFFFFFFFFFFFFull
#define MS_NS                 (1000 * 1000LL)

/*
 * Every platform qtest says tmtc-server=off explicitly: under qtest the
 * auto default binds nothing already, but no test here may ever take
 * the gates' TMTC port 20000 + uid (PL-3 review S7).
 */
static QTestState *plat_qtest_new(const char *extra)
{
    g_autofree gchar *all = g_strdup_printf(
        "-global grlib-stixspw.tmtc-server=off %s", extra);

    return stix_qtest_new_extra(all);
}

/* A uint64 property, or *ok = false when qom-get fails. */
static uint64_t stix_qom_get_u64(QTestState *qts, const char *path,
                                 const char *prop, bool *ok)
{
    QDict *resp = qtest_qmp(qts,
                            "{'execute': 'qom-get',"
                            " 'arguments': {'path': %s, 'property': %s}}",
                            path, prop);
    QNum *n = qobject_to(QNum, qdict_get(resp, "return"));
    uint64_t val = 0;

    *ok = n != NULL && qnum_get_try_uint(n, &val);
    qobject_unref(resp);
    return val;
}

/* qom-set of an integer property; true when QEMU accepted it. */
static bool stix_qom_set_u64(QTestState *qts, const char *path,
                             const char *prop, uint64_t value)
{
    QDict *resp = qtest_qmp(qts,
                            "{'execute': 'qom-set',"
                            " 'arguments': {'path': %s, 'property': %s,"
                            " 'value': %" PRIu64 "}}",
                            path, prop, value);
    bool ok = qdict_haskey(resp, "return");

    qobject_unref(resp);
    return ok;
}

static uint64_t plat_obt(QTestState *qts)
{
    bool ok;
    uint64_t v = stix_qom_get_u64(qts, "/machine/stixspw", "obt", &ok);

    g_assert(ok);
    return v;
}

static bool spw_to(QTestState *qts)
{
    return (qtest_readl(qts, SPW_STATUS) & SPW_STATUS_TO) != 0;
}

static void spw_to_clear(QTestState *qts)
{
    qtest_writel(qts, SPW_STATUS, SPW_STATUS_TO);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);
}

/* Step to 10 ms past the next OBT rollover (fine < 0x1000 at fire). */
static void obt_step_rollover(QTestState *qts)
{
    uint64_t fine = plat_obt(qts) & 0xFFFFu;
    int64_t ns_into = (int64_t)fine * NSEC_PER_SEC / SUBSEC_PER_SEC;

    qtest_clock_step(qts, NSEC_PER_SEC - ns_into + 10 * MS_NS);
}

/*
 * D-PLb: timecode-source=obt makes a platform OBT the time-code source:
 * OBT = obt-epoch + vclock · 65536 / 1e9 (CUC 32.16), the codes come at
 * the OBT rollovers with TIMECNT = OBT coarse & 63, a guest SCET commit
 * does not move them, a runtime `obt` set (an OBT jump) re-arms, and the
 * OBT survives a system reset. In the default scet mode `obt` reads the
 * IDPU SCET (coarse without UNSYNCED) and a set is refused. RED before
 * PL-1: timecode-source, obt-epoch and obt are unknown.
 */
static void test_spw_obt(void)
{
    const uint64_t epoch = OBT_CUC(0x10000005u, 0);
    g_autofree gchar *extra = NULL;
    const char *path = "/machine/stixspw";
    QTestState *qts;
    uint64_t obt, before;
    bool ok;

    extra = g_strdup_printf("-global grlib-stixspw.timecode-source=obt "
                            "-global grlib-stixspw.obt-epoch=%" PRIu64,
                            epoch);
    qts = plat_qtest_new(extra);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW);
    spw_to_clear(qts);

    /* the OBT starts at the epoch and advances exactly with the vclock */
    g_assert_cmphex(plat_obt(qts), ==, epoch);
    qtest_clock_step(qts, 250 * MS_NS);
    g_assert_cmphex(plat_obt(qts), ==, epoch + 0x4000u);
    g_assert(!spw_to(qts));

    /*
     * a. the OBT rollover emits with TIMECNT = OBT coarse & 63 (6), not
     * the SCET's (1)
     */
    obt_step_rollover(qts);
    g_assert_cmpuint(ctrl_sec(qts) & 63u, ==, 1);
    g_assert(spw_to(qts));
    g_assert_cmpuint(qtest_readl(qts, SPW_TIME) & 63u, ==,
                     0x10000006u & 63u);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT, ==,
                     SPW_IRQ_BIT);
    spw_to_clear(qts);

    /*
     * b. a guest SCET commit mid-second does not move the codes: in
     * scet mode a commit with fine 0x8000 emits 0.5 s later (spw-timecode
     * e); in obt mode nothing comes until the OBT rollover
     */
    ctrl_write(qts, CTRL_LONGTIMERSEC, 0x12345678u);
    ctrl_write(qts, CTRL_LONGTIMERSUBSEC, 0x8000u);
    qtest_clock_step(qts, 510 * MS_NS);
    g_assert_cmpuint(ctrl_sec(qts), ==, 0x12345679u);
    g_assert(!spw_to(qts));
    obt_step_rollover(qts);
    g_assert(spw_to(qts));
    g_assert_cmpuint(qtest_readl(qts, SPW_TIME) & 63u, ==,
                     0x10000007u & 63u);
    spw_to_clear(qts);

    /*
     * c. an obt set (a jump to 0.75 s into 0x20000010) re-arms: the next
     * code comes 0.25 s later with TIMECNT 0x11; the old schedule
     * (0.99 s away) never fires
     */
    ok = stix_qom_set_u64(qts, path, "obt", OBT_CUC(0x20000010u, 0xC000u));
    g_assert(ok);
    g_assert_cmphex(plat_obt(qts), ==, OBT_CUC(0x20000010u, 0xC000u));
    qtest_clock_step(qts, 240 * MS_NS);
    g_assert(!spw_to(qts));
    qtest_clock_step(qts, 20 * MS_NS);
    g_assert(spw_to(qts));
    g_assert_cmpuint(qtest_readl(qts, SPW_TIME) & 63u, ==,
                     0x20000011u & 63u);
    spw_to_clear(qts);
    /* a value past 48 bits is refused and changes nothing */
    before = plat_obt(qts);
    g_assert(!stix_qom_set_u64(qts, path, "obt", OBT_MASK + 1));
    g_assert_cmphex(plat_obt(qts), ==, before);

    /*
     * d. the OBT survives a system reset, and the codes follow it again
     * once the FSW re-writes control (TR)
     */
    before = plat_obt(qts);
    qtest_system_reset(qts);
    g_assert_cmphex(plat_obt(qts), ==, before);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW);
    spw_to_clear(qts);
    obt_step_rollover(qts);
    obt = plat_obt(qts);
    g_assert(spw_to(qts));
    g_assert_cmpuint(qtest_readl(qts, SPW_TIME) & 63u, ==,
                     (uint32_t)(obt >> 16) & 63u);
    qtest_quit(qts);
    stix_qtest_cleanup();

    /* e. scet mode (the default): obt reads the SCET; a set is refused */
    qts = plat_qtest_new("");
    qtest_clock_step(qts, 300 * MS_NS);
    obt = plat_obt(qts);
    g_assert_cmphex(obt >> 16, ==, ctrl_sec(qts));
    g_assert_cmphex(obt & 0xFFFFu, ==, ctrl_sub(qts));
    ctrl_write(qts, CTRL_LONGTIMERSEC, 0x80000123u); /* UNSYNCED set */
    ctrl_write(qts, CTRL_LONGTIMERSUBSEC, 0x4000u);
    g_assert_cmphex(plat_obt(qts), ==, OBT_CUC(0x123u, 0x4000u));
    g_assert(!stix_qom_set_u64(qts, path, "obt", OBT_DEFAULT_EPOCH));
    g_assert_cmphex(plat_obt(qts), ==, OBT_CUC(0x123u, 0x4000u));
    qtest_quit(qts);
    stix_qtest_cleanup();

    /* f. the default epoch is 0x10000000 (coarse & 63 = 0, D-PLb) */
    qts = plat_qtest_new("-global grlib-stixspw.timecode-source=obt");
    g_assert_cmphex(plat_obt(qts), ==, OBT_DEFAULT_EPOCH);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * D-PLc: timecode-once arms exactly one correct code at the next
 * rollover while timecodes is false; it is a no-op while timecodes is
 * true, refused while link-down is true or control.TR is 0, and a
 * pending once is dropped by link-down and by a timecodes write. RED
 * before PL-1: timecode-once is unknown.
 */
static void test_spw_timecode_once(void)
{
    const char *path = "/machine/stixspw";
    QTestState *qts;
    uint32_t coarse;
    bool ok, v;

    qts = plat_qtest_new("");
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW);
    g_assert(stix_qom_set_bool(qts, path, "timecodes", false));
    spw_to_clear(qts);

    /* a. exactly one code, with the correct TIMECNT, then silence */
    g_assert(stix_qom_set_bool(qts, path, "timecode-once", true));
    v = stix_qom_get_bool(qts, path, "timecode-once", &ok);
    g_assert(ok && v);
    tc_step_rollover(qts);
    coarse = ctrl_sec(qts);
    g_assert(spw_to(qts));
    g_assert_cmpuint(qtest_readl(qts, SPW_TIME) & 63u, ==, coarse & 63u);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT, ==,
                     SPW_IRQ_BIT);
    v = stix_qom_get_bool(qts, path, "timecode-once", &ok);
    g_assert(ok && !v);
    spw_to_clear(qts);
    tc_step_rollover(qts);
    tc_step_rollover(qts);
    g_assert(!spw_to(qts));

    /* b. a no-op while timecodes is true: nothing pending, codes as usual */
    g_assert(stix_qom_set_bool(qts, path, "timecodes", true));
    g_assert(stix_qom_set_bool(qts, path, "timecode-once", true));
    v = stix_qom_get_bool(qts, path, "timecode-once", &ok);
    g_assert(ok && !v);
    tc_step_rollover(qts);
    g_assert(spw_to(qts));
    spw_to_clear(qts);
    g_assert(stix_qom_set_bool(qts, path, "timecodes", false));

    /* c. refused while the link is down */
    g_assert(stix_qom_set_bool(qts, path, "link-down", true));
    g_assert(!stix_qom_set_bool(qts, path, "timecode-once", true));
    g_assert(stix_qom_set_bool(qts, path, "link-down", false));
    tc_step_rollover(qts);
    g_assert(!spw_to(qts));

    /* d. an error with control.TR = 0 (the model would emit nothing) */
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW & ~SPW_CTRL_TR);
    g_assert(!stix_qom_set_bool(qts, path, "timecode-once", true));
    v = stix_qom_get_bool(qts, path, "timecode-once", &ok);
    g_assert(ok && !v);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW);

    /* e. a pending once is dropped by link-down = true */
    g_assert(stix_qom_set_bool(qts, path, "timecode-once", true));
    g_assert(stix_qom_set_bool(qts, path, "link-down", true));
    v = stix_qom_get_bool(qts, path, "timecode-once", &ok);
    g_assert(ok && !v);
    g_assert(stix_qom_set_bool(qts, path, "link-down", false));
    tc_step_rollover(qts);
    g_assert(!spw_to(qts));

    /* f. ... and by a timecodes write */
    g_assert(stix_qom_set_bool(qts, path, "timecode-once", true));
    g_assert(stix_qom_set_bool(qts, path, "timecodes", false));
    v = stix_qom_get_bool(qts, path, "timecode-once", &ok);
    g_assert(ok && !v);
    tc_step_rollover(qts);
    g_assert(!spw_to(qts));

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * D-PLc: timecode-inject (write-only uint32 0..63) emits one code now
 * with that TIMECNT through the TR/TQ/IE path and the D-M latch; 64 is
 * rejected, and it is refused while link-down is true or control.TR is
 * 0. RED before PL-1: timecode-inject is unknown.
 */
static void test_spw_timecode_inject(void)
{
    const char *path = "/machine/stixspw";
    QTestState *qts;

    qts = plat_qtest_new("");
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW);
    g_assert(stix_qom_set_bool(qts, path, "timecodes", false));
    qtest_clock_step(qts, 300 * MS_NS); /* mid-second: a wrong-time code */
    spw_to_clear(qts);

    /* a. the given TIMECNT now, TO and IRQ 13 */
    g_assert(stix_qom_set_u64(qts, path, "timecode-inject", 42));
    g_assert(spw_to(qts));
    g_assert_cmpuint(qtest_readl(qts, SPW_TIME), ==, 42);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT, ==,
                     SPW_IRQ_BIT);
    spw_to_clear(qts);

    /* b. control.IE = 0 latches the IRQ; the IE 0->1 write pulses it */
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW & ~SPW_CTRL_IE);
    g_assert(stix_qom_set_u64(qts, path, "timecode-inject", 5));
    g_assert(spw_to(qts));
    g_assert_cmpuint(qtest_readl(qts, SPW_TIME), ==, 5);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT, ==, 0);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT, ==,
                     SPW_IRQ_BIT);
    spw_to_clear(qts);

    /* c. control.TQ = 0: TO and TIMECNT, no IRQ */
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW & ~SPW_CTRL_TQ);
    g_assert(stix_qom_set_u64(qts, path, "timecode-inject", 63));
    g_assert(spw_to(qts));
    g_assert_cmpuint(qtest_readl(qts, SPW_TIME), ==, 63);
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT, ==, 0);
    spw_to_clear(qts);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW);

    /* d. 64 is rejected: no code, TIMECNT unchanged */
    g_assert(!stix_qom_set_u64(qts, path, "timecode-inject", 64));
    g_assert(!spw_to(qts));
    g_assert_cmpuint(qtest_readl(qts, SPW_TIME), ==, 63);

    /* e. refused while the link is down */
    g_assert(stix_qom_set_bool(qts, path, "link-down", true));
    g_assert(!stix_qom_set_u64(qts, path, "timecode-inject", 7));
    g_assert(!spw_to(qts));
    g_assert(stix_qom_set_bool(qts, path, "link-down", false));

    /* f. an error with control.TR = 0, and nothing emitted */
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW & ~SPW_CTRL_TR);
    g_assert(!stix_qom_set_u64(qts, path, "timecode-inject", 9));
    g_assert(!spw_to(qts));
    g_assert_cmpuint(qtest_readl(qts, SPW_TIME), ==, 63);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- Platform controller (PL-2): read-only state, vclock-ns ---- */

/* the hw-state JSON of a device, parsed; NULL when the property fails */
static QDict *plat_hw_state(QTestState *qts, const char *path)
{
    QDict *resp = qtest_qmp(qts,
                            "{'execute': 'qom-get',"
                            " 'arguments': {'path': %s,"
                            " 'property': 'hw-state'}}", path);
    QString *str = qobject_to(QString, qdict_get(resp, "return"));
    QDict *st = NULL;

    if (str != NULL) {
        st = qobject_to(QDict, qobject_from_json(qstring_get_str(str),
                                                 &error_abort));
        g_assert(st != NULL);
    }
    qobject_unref(resp);
    return st;
}

/* the raw hw-state string (for the equality check) */
static gchar *plat_hw_state_str(QTestState *qts, const char *path)
{
    QDict *resp = qtest_qmp(qts,
                            "{'execute': 'qom-get',"
                            " 'arguments': {'path': %s,"
                            " 'property': 'hw-state'}}", path);
    QString *str = qobject_to(QString, qdict_get(resp, "return"));
    gchar *s;

    g_assert(str != NULL);
    s = g_strdup(qstring_get_str(str));
    qobject_unref(resp);
    return s;
}

static QDict *plat_dict(QDict *d, const char *key)
{
    QDict *v = qdict_get_qdict(d, key);

    g_assert(v != NULL);
    return v;
}

static QDict *plat_hk_ch(QDict *st, uint32_t ch)
{
    g_autofree gchar *key = g_strdup_printf("%u", ch);

    return plat_dict(plat_dict(st, "hk_adc"), key);
}

/* the n-th element of a QList (NULL past the end) */
static QObject *plat_list_nth(QList *l, unsigned n)
{
    const QListEntry *e;

    g_assert(l != NULL);
    for (e = qlist_first(l); e != NULL; e = qlist_next(e)) {
        if (n-- == 0) {
            return qlist_entry_obj(e);
        }
    }
    return NULL;
}

static bool plat_quarter_on(QDict *st, unsigned q)
{
    QList *l = qdict_get_qlist(plat_dict(st, "det"), "quarter_on");
    QBool *b = qobject_to(QBool, plat_list_nth(l, q));

    g_assert(b != NULL);
    return qbool_get_bool(b);
}

static QDict *plat_prot_line(QDict *st, unsigned idx)
{
    QList *l = qdict_get_qlist(plat_dict(st, "hk_prot"), "lines");
    QDict *d = qobject_to(QDict, plat_list_nth(l, idx));

    g_assert(d != NULL);
    return d;
}

static off_t plat_file_size(const char *path)
{
    struct stat st;

    g_assert(stat(path, &st) == 0);
    return st.st_size;
}

/*
 * D-PLo: /machine/stixctrl hw-state is a read-only JSON string (schema
 * 2 since the M-6a edac section) that follows hkCmdSta (enables,
 * POWERSTAT), idefxConfig (quarter power), an injection (truth = the
 * pre-injection value, presented = the injected one), the last
 * conversion, a protection trip, the watchdog and the SCET with its
 * UNSYNCED flag. Reading it is free of
 * side effects: two reads with no clock_step in between are equal, and
 * no stixctrl_io_read or stixspw_io_read trace fires while the getters
 * run (the CTRL read handler stores into reg on every read). RED before
 * PL-2: hw-state is unknown.
 */
static void test_ctrl_hw_state(void)
{
    const char *ctrl = "/machine/stixctrl", *spw = "/machine/stixspw";
    g_autofree gchar *log = NULL;
    g_autofree gchar *extra = NULL;
    g_autofree gchar *a = NULL, *b = NULL, *c = NULL, *d = NULL;
    QTestState *qts;
    QDict *st, *sup, *ch, *last, *prot, *scet, *wdg;
    off_t sz;
    int fd;

    fd = g_file_open_tmp("stix-hwstate-trace-XXXXXX", &log, NULL);
    g_assert(fd >= 0);
    close(fd);
    extra = g_strdup_printf("-trace stixctrl_io_read -trace stixspw_io_read "
                            "-D %s", log);
    qts = plat_qtest_new(extra);

    /* a. JSON, schema 2 (M-6a's edac section), with the virtual time */
    st = plat_hw_state(qts, ctrl);
    g_assert(st != NULL);
    g_assert_cmpint(qdict_get_int(st, "schema"), ==, 2);
    g_assert(qdict_haskey(st, "vclock_ns"));
    sup = plat_dict(st, "supplies");
    g_assert_cmpint(qdict_get_int(sup, "enables"), ==, 0);
    g_assert_cmpint(qdict_get_int(sup, "powerstat"), ==, 0);
    qobject_unref(st);

    /* b. the enables and POWERSTAT follow hkCmdSta writes */
    ctrl_write(qts, CTRL_HKCMDSTA, 0x1Fu);
    st = plat_hw_state(qts, ctrl);
    sup = plat_dict(st, "supplies");
    g_assert_cmpint(qdict_get_int(sup, "enables"), ==, 0x1F);
    g_assert_cmpint(qdict_get_int(sup, "powerstat"), ==, 0x1F);
    g_assert(qdict_get_bool(plat_dict(plat_dict(sup, "lines"), "spw1"),
                            "good"));
    qobject_unref(st);
    ctrl_write(qts, CTRL_HKCMDSTA, 0x01u); /* LV only */
    st = plat_hw_state(qts, ctrl);
    sup = plat_dict(st, "supplies");
    g_assert_cmpint(qdict_get_int(sup, "enables"), ==, 0x01);
    g_assert_cmpint(qdict_get_int(sup, "powerstat"), ==, 0x01);
    g_assert(qdict_get_bool(plat_dict(plat_dict(sup, "lines"), "lv"),
                            "enabled"));
    g_assert(!qdict_get_bool(plat_dict(plat_dict(sup, "lines"), "hv01"),
                             "enabled"));
    /* the state overrides follow too: ATT_V (ch 12) is 1670 with LV */
    g_assert_cmpint(qdict_get_int(plat_hk_ch(st, 12), "truth"), ==, 1670);
    qobject_unref(st);

    /* c. quarter_on follows idefxConfig 3:0 */
    ctrl_write(qts, CTRL_IDEFXCONFIG, 0x5u);
    st = plat_hw_state(qts, ctrl);
    g_assert(plat_quarter_on(st, 0) && !plat_quarter_on(st, 1) &&
             plat_quarter_on(st, 2) && !plat_quarter_on(st, 3));
    g_assert_cmpint(qdict_get_int(plat_hk_ch(st, 4), "truth"), ==,
                    20 + 75 * 2);
    qobject_unref(st);

    /*
     * d. an injected channel: truth = the pre-injection value,
     * presented = the injection; the last conversion is what the guest
     * read, with its virtual time
     */
    st = plat_hw_state(qts, ctrl);
    ch = plat_hk_ch(st, 24);
    g_assert_cmpint(qdict_get_int(ch, "truth"), ==, 2048);
    g_assert_cmpint(qdict_get_int(ch, "presented"), ==, 2048);
    g_assert(!qdict_get_bool(ch, "injected"));
    g_assert(qobject_type(qdict_get(ch, "last")) == QTYPE_QNULL);
    qobject_unref(st);
    g_assert_true(adc_inject_set(qts, 24, 1800));
    qtest_clock_step(qts, 5 * MS_NS);
    hk_check(qts, 24, 1800);
    st = plat_hw_state(qts, ctrl);
    ch = plat_hk_ch(st, 24);
    g_assert_cmpint(qdict_get_int(ch, "truth"), ==, 2048);
    g_assert_cmpint(qdict_get_int(ch, "presented"), ==, 1800);
    g_assert(qdict_get_bool(ch, "injected"));
    last = plat_dict(ch, "last");
    g_assert_cmpint(qdict_get_int(last, "value"), ==, 1800);
    g_assert_cmpint(qdict_get_int(last, "vclock_ns"), ==,
                    qdict_get_int(st, "vclock_ns"));
    qobject_unref(st);

    /* e. the protection fields follow a trip (DET_C, line 5, ch 4) */
    ctrl_write(qts, CTRL_HKPROTCONFIG(HK_PROT_DET), HKPROT_EN | 546);
    ctrl_write(qts, CTRL_HKIRQSTA, HK_PROT_DET_BIT);
    st = plat_hw_state(qts, ctrl);
    prot = plat_dict(st, "hk_prot");
    g_assert_cmpint(qdict_get_int(prot, "irqen"), ==, HK_PROT_DET_BIT);
    g_assert_cmpint(qdict_get_int(prot, "protstat"), ==, 0);
    g_assert_cmpint(qdict_get_int(plat_prot_line(st, HK_PROT_DET), "line"),
                    ==, 5);
    g_assert_cmpint(qdict_get_int(plat_prot_line(st, HK_PROT_DET), "ch"),
                    ==, 4);
    g_assert(qdict_get_bool(plat_prot_line(st, HK_PROT_DET), "prot_en"));
    g_assert_cmpint(qdict_get_int(plat_prot_line(st, HK_PROT_DET),
                                  "threshold"), ==, 546);
    qobject_unref(st);
    g_assert_true(adc_inject_set(qts, 4, 600));
    hk_check(qts, 4, 600);
    st = plat_hw_state(qts, ctrl);
    prot = plat_dict(st, "hk_prot");
    g_assert_cmpint(qdict_get_int(prot, "protstat"), ==, HK_PROT_DET_BIT);
    g_assert_cmpint(qdict_get_int(prot, "hotsample"), ==, 600);
    qobject_unref(st);

    /* f. the watchdog (RELOAD with 63 cycles) and the SCET */
    ctrl_write(qts, WDG, WDG_CHANGE(63));
    ctrl_write(qts, WDG, WDG_RELOAD);
    qtest_clock_step(qts, 2 * WDG_CYCLE_NS);
    st = plat_hw_state(qts, ctrl);
    wdg = plat_dict(st, "watchdog");
    g_assert(qdict_get_bool(wdg, "running"));
    g_assert_cmpint(qdict_get_int(wdg, "reload"), ==, 63);
    g_assert_cmpint(qdict_get_int(wdg, "count"), ==, 61);
    scet = plat_dict(st, "scet");
    g_assert(!qdict_get_bool(scet, "unsynced"));
    qobject_unref(st);
    /* the UNSYNCED flag follows a guest 0xC8/0xCC commit with bit 31 */
    ctrl_write(qts, CTRL_LONGTIMERSEC, 0x80000123u);
    ctrl_write(qts, CTRL_LONGTIMERSUBSEC, 0x4000u);
    st = plat_hw_state(qts, ctrl);
    scet = plat_dict(st, "scet");
    g_assert(qdict_get_bool(scet, "unsynced"));
    g_assert_cmpint(qdict_get_int(scet, "coarse"), ==, 0x123);
    g_assert_cmpint(qdict_get_int(scet, "fine"), ==, 0x4000);
    qobject_unref(st);
    ctrl_write(qts, CTRL_LONGTIMERSEC, 0x00000123u);
    ctrl_write(qts, CTRL_LONGTIMERSUBSEC, 0x4000u);
    st = plat_hw_state(qts, ctrl);
    g_assert(!qdict_get_bool(plat_dict(st, "scet"), "unsynced"));
    qobject_unref(st);

    /*
     * g. reading changes nothing: two reads of each with no clock_step
     * between are equal, and no *_io_read trace fires during them. The
     * positive control: one MMIO read does grow the trace log.
     */
    qtest_clock_step(qts, 300 * MS_NS); /* mid-second, watchdog running */
    sz = plat_file_size(log);
    a = plat_hw_state_str(qts, ctrl);
    b = plat_hw_state_str(qts, spw);
    c = plat_hw_state_str(qts, ctrl);
    d = plat_hw_state_str(qts, spw);
    g_assert_cmpstr(a, ==, c);
    g_assert_cmpstr(b, ==, d);
    g_assert_cmpint(plat_file_size(log), ==, sz);
    ctrl_read(qts, CTRL_HKCMDSTA);
    qtest_readl(qts, SPW_STATUS);
    g_assert_cmpint(plat_file_size(log), >, sz);

    qtest_quit(qts);
    stix_qtest_cleanup();
    unlink(log);
}

/*
 * D-PLo: /machine/stixspw hw-state gives LS and link_down, control
 * TR/TQ/IE/LI, and the time-code fields (source, enabled, once, the
 * last TIMECNT and its virtual time, the next due time) after a
 * rollover. RED before PL-2: hw-state is unknown.
 */
static void test_spw_hw_state(void)
{
    const char *spw = "/machine/stixspw";
    QTestState *qts;
    QDict *st, *ctl, *tc;
    int64_t t_emit, due;
    uint32_t coarse;

    qts = plat_qtest_new("");
    st = plat_hw_state(qts, spw);
    g_assert(st != NULL);
    g_assert_cmpint(qdict_get_int(st, "schema"), ==, 1);
    g_assert_cmpint(qdict_get_int(st, "ls"), ==, SPW_LS_RUN);
    g_assert(!qdict_get_bool(st, "link_down"));
    tc = plat_dict(st, "timecode");
    g_assert_cmpstr(qdict_get_str(tc, "source"), ==, "scet");
    g_assert(qdict_get_bool(tc, "enabled"));
    g_assert(!qdict_get_bool(tc, "once_pending"));
    g_assert(qobject_type(qdict_get(tc, "last_timecnt")) == QTYPE_QNULL);
    /* the timer is armed for the first rollover, 1 s */
    g_assert_cmpint(qdict_get_int(tc, "next_due_vclock_ns"), ==,
                    NSEC_PER_SEC);
    g_assert_cmpint(qdict_get_int(plat_dict(st, "counters"), "rx_dropped"),
                    ==, 0);
    g_assert_cmpint(qdict_get_int(st, "rx_queued"), ==, 0);
    qobject_unref(st);

    /* control bits */
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW);
    st = plat_hw_state(qts, spw);
    ctl = plat_dict(st, "control");
    g_assert_cmpint(qdict_get_int(ctl, "raw"), ==, SPW_CTRL_FSW);
    g_assert(qdict_get_bool(ctl, "tr") && qdict_get_bool(ctl, "tq") &&
             qdict_get_bool(ctl, "ie") && qdict_get_bool(ctl, "li"));
    qobject_unref(st);

    /* the time-code fields after a rollover */
    tc_step_rollover(qts);
    coarse = ctrl_sec(qts);
    st = plat_hw_state(qts, spw);
    tc = plat_dict(st, "timecode");
    g_assert_cmpint(qdict_get_int(tc, "last_timecnt"), ==, coarse & 63u);
    t_emit = qdict_get_int(tc, "last_vclock_ns");
    g_assert_cmpint(t_emit, ==, NSEC_PER_SEC);
    due = qdict_get_int(tc, "next_due_vclock_ns");
    g_assert_cmpint(due, ==, 2 * NSEC_PER_SEC);
    g_assert_cmpint(qdict_get_int(st, "obt") >> 16, ==, coarse);
    qobject_unref(st);

    /* LS and link_down follow link-down */
    g_assert(stix_qom_set_bool(qts, spw, "link-down", true));
    st = plat_hw_state(qts, spw);
    g_assert_cmpint(qdict_get_int(st, "ls"), ==, SPW_LS_READY);
    g_assert(qdict_get_bool(st, "link_down"));
    g_assert(qobject_type(qdict_get(plat_dict(st, "timecode"),
                                    "next_due_vclock_ns")) == QTYPE_QNULL);
    qobject_unref(st);
    g_assert(stix_qom_set_bool(qts, spw, "link-down", false));
    st = plat_hw_state(qts, spw);
    g_assert_cmpint(qdict_get_int(st, "ls"), ==, SPW_LS_RUN);
    g_assert(!qdict_get_bool(st, "link_down"));
    qobject_unref(st);

    /* an injected code is the last one too; obt mode shows its source */
    g_assert(stix_qom_set_u64(qts, spw, "timecode-inject", 33));
    st = plat_hw_state(qts, spw);
    g_assert_cmpint(qdict_get_int(plat_dict(st, "timecode"),
                                  "last_timecnt"), ==, 33);
    qobject_unref(st);
    qtest_quit(qts);
    stix_qtest_cleanup();

    qts = plat_qtest_new("-global grlib-stixspw.timecode-source=obt");
    st = plat_hw_state(qts, spw);
    g_assert_cmpstr(qdict_get_str(plat_dict(st, "timecode"), "source"), ==,
                    "obt");
    g_assert_cmphex(qdict_get_int(st, "obt"), ==, OBT_DEFAULT_EPOCH);
    qobject_unref(st);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * D-PLr: /machine/stixspw vclock-ns is QEMU_CLOCK_VIRTUAL: monotonic,
 * unchanged without a clock_step (qtest), and it advances exactly with
 * clock_step. RED before PL-2: vclock-ns is unknown.
 */
static void test_vclock_ns(void)
{
    QTestState *qts = plat_qtest_new("");
    uint64_t t0, t1;
    bool ok;

    t0 = stix_qom_get_u64(qts, "/machine/stixspw", "vclock-ns", &ok);
    g_assert(ok);
    t1 = stix_qom_get_u64(qts, "/machine/stixspw", "vclock-ns", &ok);
    g_assert(ok);
    g_assert_cmpuint(t1, ==, t0);
    qtest_clock_step(qts, 123456789);
    t1 = stix_qom_get_u64(qts, "/machine/stixspw", "vclock-ns", &ok);
    g_assert_cmpuint(t1, ==, t0 + 123456789);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(stix_qom_get_u64(qts, "/machine/stixspw", "vclock-ns",
                                      &ok), ==, t1 + 1);
    /* read-only */
    g_assert(!stix_qom_set_u64(qts, "/machine/stixspw", "vclock-ns", 0));
    /* S7: plat_qtest_new() never lets a TMTC server bind */
    {
        QDict *resp = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                                "{'path': '/machine/stixspw', "
                                "'property': 'tmtc-server'}}");

        g_assert_cmpstr(qdict_get_try_str(resp, "return"), ==, "off");
        qobject_unref(resp);
    }
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- PL-1/PL-2 review: the extras and the nits ---- */

#define PLAT_TEST_GLOBAL_ENV "STIX_TEST_PLAT_GLOBAL"

static void test_plat_realize_error_subprocess(void)
{
    const char *global = g_getenv(PLAT_TEST_GLOBAL_ENV);
    const char *flash = g_getenv(TMTC_TEST_FLASH_ENV);
    QTestState *qts;

    g_assert_nonnull(global);
    g_assert_nonnull(flash);
    /* qtest_init aborts: QEMU realize fails and exits */
    qts = qtest_initf("-M leon3_generic "
                      "-global grlib-stixspw.tmtc-server=off "
                      "-global grlib-stixflash.flash-file=%s -global %s",
                      flash, global);
    qtest_quit(qts);
}

/*
 * D-PLb: realize refuses a timecode-source other than scet/obt and an
 * obt-epoch wider than the 48-bit CUC (2^48), naming the value.
 */
static void test_plat_realize_error(void)
{
    static const struct {
        const char *global;
        const char *pattern;
    } cases[] = {
        { "grlib-stixspw.timecode-source=utc",
          "*timecode-source 'utc' is not 'scet' or 'obt'*" },
        { "grlib-stixspw.obt-epoch=281474976710656",
          "*obt-epoch 0x1000000000000 is wider than the 48-bit CUC*" },
    };
    g_autofree gchar *path = NULL;
    g_autofree gchar *sub = stix_test_path("plat-realize-error/subprocess");
    int fd = g_file_open_tmp("stix-flash-XXXXXX", &path, NULL);
    size_t i;

    g_assert(fd >= 0);
    g_assert(ftruncate(fd, 1024 * 1024) == 0);
    g_setenv(TMTC_TEST_FLASH_ENV, path, TRUE);
    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_setenv(PLAT_TEST_GLOBAL_ENV, cases[i].global, TRUE);
        g_test_trap_subprocess(sub, 30 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr(cases[i].pattern);
    }
    close(fd);
    g_unlink(path);
}

static gchar *plat_read_log(const char *path)
{
    gchar *text = NULL;

    g_assert(g_file_get_contents(path, &text, NULL, NULL));
    return text;
}

/*
 * D-PLc extras: timecode-once=false cancels a pending once (traced);
 * a pending once survives a system reset and, with TR = 0 after the
 * reset, is consumed at the rollover with nothing emitted (traced
 * emitted=0); once and inject in obt mode follow the OBT. The nits:
 * spw hw-state names obt_kind (scet vs platform), and ctrl hw-state
 * lists a channel with an active injection even when its table entry
 * is 0. RED before the review fixes: obt_kind and ch 5 are missing.
 */
static void test_plat_review_extras(void)
{
    const char *spw = "/machine/stixspw";
    g_autofree gchar *log = NULL;
    g_autofree gchar *extra = NULL;
    g_autofree gchar *text = NULL;
    QTestState *qts;
    QDict *st, *ch;
    bool ok, v;
    int fd;

    fd = g_file_open_tmp("stix-plat-trace-XXXXXX", &log, NULL);
    g_assert(fd >= 0);
    close(fd);
    extra = g_strdup_printf("-trace stixspw_timecode_manual "
                            "-trace stixspw_timecode_once_drop -D %s", log);
    qts = plat_qtest_new(extra);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW);
    g_assert(stix_qom_set_bool(qts, spw, "timecodes", false));
    spw_to_clear(qts);

    /* a. the nits, scet mode: obt_kind "scet" */
    st = plat_hw_state(qts, spw);
    g_assert_cmpstr(qdict_get_try_str(st, "obt_kind"), ==, "scet");
    qobject_unref(st);

    /* b. timecode-once=false cancels: no code at the rollover */
    g_assert(stix_qom_set_bool(qts, spw, "timecode-once", true));
    g_assert(stix_qom_set_bool(qts, spw, "timecode-once", false));
    v = stix_qom_get_bool(qts, spw, "timecode-once", &ok);
    g_assert(ok && !v);
    tc_step_rollover(qts);
    g_assert(!spw_to(qts));

    /*
     * c. a once survives a reset; TR = 0 after it, so the rollover
     * consumes it with nothing emitted
     */
    g_assert(stix_qom_set_bool(qts, spw, "timecode-once", true));
    qtest_system_reset(qts);
    v = stix_qom_get_bool(qts, spw, "timecode-once", &ok);
    g_assert(ok && v);
    tc_step_rollover(qts);
    g_assert(!spw_to(qts));
    v = stix_qom_get_bool(qts, spw, "timecode-once", &ok);
    g_assert(ok && !v);
    qtest_quit(qts);
    text = plat_read_log(log);
    g_assert_nonnull(strstr(text, "pending timecode-once dropped: cancel"));
    g_assert_nonnull(strstr(text, "kind=once"));
    g_assert_nonnull(strstr(text, "emitted=0"));
    stix_qtest_cleanup();
    unlink(log);

    /* d. obt mode: once at the OBT rollover, inject now, obt_kind */
    g_free(extra);
    extra = g_strdup_printf("-global grlib-stixspw.timecode-source=obt "
                            "-global grlib-stixspw.obt-epoch=%" PRIu64,
                            OBT_CUC(0x10000005u, 0));
    qts = plat_qtest_new(extra);
    qtest_writel(qts, SPW_CONTROL, SPW_CTRL_FSW);
    g_assert(stix_qom_set_bool(qts, spw, "timecodes", false));
    spw_to_clear(qts);
    st = plat_hw_state(qts, spw);
    g_assert_cmpstr(qdict_get_try_str(st, "obt_kind"), ==, "platform");
    qobject_unref(st);
    g_assert(stix_qom_set_bool(qts, spw, "timecode-once", true));
    obt_step_rollover(qts);
    g_assert(spw_to(qts));
    g_assert_cmpuint(qtest_readl(qts, SPW_TIME) & 63u, ==,
                     0x10000006u & 63u);
    v = stix_qom_get_bool(qts, spw, "timecode-once", &ok);
    g_assert(ok && !v);
    spw_to_clear(qts);
    obt_step_rollover(qts);
    g_assert(!spw_to(qts)); /* exactly one */
    g_assert(stix_qom_set_u64(qts, spw, "timecode-inject", 17));
    g_assert(spw_to(qts));
    g_assert_cmpuint(qtest_readl(qts, SPW_TIME), ==, 17);
    spw_to_clear(qts);

    /*
     * e. ctrl hw-state lists an injected channel even when its table
     * entry is 0 (ch 5 is unused): truth 0, presented the injection
     */
    st = plat_hw_state(qts, "/machine/stixctrl");
    g_assert(!qdict_haskey(qdict_get_qdict(st, "hk_adc"), "5"));
    qobject_unref(st);
    g_assert_true(adc_inject_set(qts, 5, 77));
    st = plat_hw_state(qts, "/machine/stixctrl");
    ch = plat_hk_ch(st, 5);
    g_assert_cmpint(qdict_get_int(ch, "truth"), ==, 0);
    g_assert_cmpint(qdict_get_int(ch, "presented"), ==, 77);
    g_assert(qdict_get_bool(ch, "injected"));
    qobject_unref(st);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

static void stix_platform_register(void)
{
    qtest_add_func("/sparc/stix/spw-obt", test_spw_obt);
    qtest_add_func("/sparc/stix/spw-timecode-once", test_spw_timecode_once);
    qtest_add_func("/sparc/stix/spw-timecode-inject",
                   test_spw_timecode_inject);
    qtest_add_func("/sparc/stix/ctrl-hw-state", test_ctrl_hw_state);
    qtest_add_func("/sparc/stix/spw-hw-state", test_spw_hw_state);
    qtest_add_func("/sparc/stix/vclock-ns", test_vclock_ns);
    qtest_add_func("/sparc/stix/plat-realize-error/subprocess",
                   test_plat_realize_error_subprocess);
    qtest_add_func("/sparc/stix/plat-realize-error",
                   test_plat_realize_error);
    qtest_add_func("/sparc/stix/plat-review-extras",
                   test_plat_review_extras);
}

/* ---- F-2: NAND op latency (flash latency plan D-F1..D-F6, D-F9) ---- */

#define FLASH_QOM_PATH "/machine/stixflash"
#define FLASH_RAM_A    0x40000000u
#define FLASH_RAM_B    0x40002000u

static bool flash_irq_pending(QTestState *qts)
{
    return (qtest_readl(qts, IRQMP_PENDING) & FLASH_IRQ_BIT) != 0;
}

static void flash_irq_clear(QTestState *qts)
{
    qtest_writel(qts, IRQMP_CLEAR, FLASH_IRQ_BIT);
}

static void flash_ram_fill(QTestState *qts, uint32_t addr, uint8_t byte)
{
    g_autofree uint8_t *buf = g_malloc(FLASH_PAGE_BYTES);

    memset(buf, byte, FLASH_PAGE_BYTES);
    qtest_memwrite(qts, addr, buf, FLASH_PAGE_BYTES);
}

static void flash_assert_busy(QTestState *qts)
{
    uint32_t sta = qtest_readl(qts, FLASH_CMDSTA);

    g_assert_cmpuint(sta & FTNAND_BUSY, ==, FTNAND_BUSY);
    g_assert_cmpuint(sta & (FTNAND_READY | FTNAND_FAILED), ==, 0);
}

static void flash_assert_done_ok(QTestState *qts)
{
    uint32_t sta = qtest_readl(qts, FLASH_CMDSTA);

    g_assert_cmpuint(sta & FTNAND_BUSY, ==, 0);
    g_assert_cmpuint(sta & (FTNAND_READY | FTNAND_FAILED), ==, FTNAND_READY);
}

/*
 * "Exactly one IRQ": the IRQMP pending bit is a latch, so after it was
 * seen and cleared, nothing may set it again for a long while.
 */
static void flash_assert_no_more_irq(QTestState *qts)
{
    flash_irq_clear(qts);
    flash_step_us(qts, 10 * 1000);
    g_assert_false(flash_irq_pending(qts));
}

/*
 * /sparc/stix/flash-latency-read: a page read keeps BUSY set for
 * tread-us; data, status and the IRQ arrive together at the deadline.
 */
static void test_flash_latency_read(void)
{
    QTestState *qts = stix_qtest_new_extra(FLASH_LATENCY_ON);

    flash_ram_fill(qts, FLASH_RAM_A, 0x00);
    flash_irq_clear(qts);
    flash_issue_nowait(qts, FLASH_RAM_A, 0, CMD_PAGE_READ);
    flash_assert_busy(qts);
    g_assert_false(flash_irq_pending(qts));
    g_assert_cmphex(qtest_readl(qts, FLASH_RAM_A), ==, 0);

    flash_step_us(qts, FLASH_TREAD_US - 1);
    flash_assert_busy(qts);
    g_assert_false(flash_irq_pending(qts));
    g_assert_cmphex(qtest_readl(qts, FLASH_RAM_A), ==, 0);

    flash_step_us(qts, 2);
    flash_assert_done_ok(qts);
    g_assert_true(flash_irq_pending(qts));
    g_assert_cmphex(qtest_readl(qts, FLASH_RAM_A), ==, 0xffffffffu);
    g_assert_cmphex(qtest_readl(qts, FLASH_RAM_A + FLASH_PAGE_BYTES - 4),
                    ==, 0xffffffffu);
    flash_assert_no_more_irq(qts);
    g_assert_cmpint(stix_qom_counter(qts, FLASH_QOM_PATH, "ops-read"),
                    ==, 1);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/flash-latency-prog: a program samples guest RAM at issue
 * (D-F2) and reaches the file only at completion.
 */
static void test_flash_latency_prog(void)
{
    g_autofree uint8_t *p = g_malloc(FLASH_PAGE_BYTES);
    g_autofree uint8_t *host = g_malloc(FLASH_PAGE_BYTES);
    g_autofree uint8_t *z0 = g_malloc0(FLASH_PAGE_BYTES);
    QTestState *qts = stix_qtest_new_extra(FLASH_LATENCY_ON);
    size_t i;

    for (i = 0; i < FLASH_PAGE_BYTES; i++) {
        p[i] = (uint8_t)(i ^ 0x5Au);
    }
    qtest_memwrite(qts, FLASH_RAM_A, p, FLASH_PAGE_BYTES);
    flash_irq_clear(qts);
    flash_issue_nowait(qts, FLASH_RAM_A, 5, CMD_PAGE_WRITE);
    flash_assert_busy(qts);

    /* the buffer changes after issue: the file must get P, not this */
    flash_ram_fill(qts, FLASH_RAM_A, 0x11);

    flash_step_us(qts, FLASH_TPROG_US - 1);
    flash_assert_busy(qts);
    g_assert_false(flash_irq_pending(qts));
    /* the file is unchanged (an erased page is a host hole, 0x00) */
    flash_pread_page(stix_tmp_flash_path, 5ULL * FLASH_PAGE_BYTES, host);
    g_assert_cmpint(memcmp(host, z0, FLASH_PAGE_BYTES), ==, 0);

    flash_step_us(qts, 2);
    flash_assert_done_ok(qts);
    g_assert_true(flash_irq_pending(qts));
    flash_pread_page(stix_tmp_flash_path, 5ULL * FLASH_PAGE_BYTES, host);
    flash_invert(host, FLASH_PAGE_BYTES);
    g_assert_cmpint(memcmp(host, p, FLASH_PAGE_BYTES), ==, 0);
    flash_assert_no_more_irq(qts);

    /* and the guest reads back the issue-time image */
    flash_ram_fill(qts, FLASH_RAM_B, 0x00);
    flash_issue(qts, FLASH_RAM_B, 5, CMD_PAGE_READ);
    qtest_memread(qts, FLASH_RAM_B, host, FLASH_PAGE_BYTES);
    g_assert_cmpint(memcmp(host, p, FLASH_PAGE_BYTES), ==, 0);
    g_assert_cmpint(stix_qom_counter(qts, FLASH_QOM_PATH, "ops-prog"),
                    ==, 1);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* /sparc/stix/flash-latency-erase: erased only at tbers-us, one IRQ. */
static void test_flash_latency_erase(void)
{
    g_autofree uint8_t *p = g_malloc(FLASH_PAGE_BYTES);
    g_autofree uint8_t *host = g_malloc(FLASH_PAGE_BYTES);
    g_autofree uint8_t *z0 = g_malloc0(FLASH_PAGE_BYTES);
    QTestState *qts = stix_qtest_new_extra(FLASH_LATENCY_ON);
    size_t i;

    for (i = 0; i < FLASH_PAGE_BYTES; i++) {
        p[i] = (uint8_t)(i ^ 0xC3u);
    }
    qtest_memwrite(qts, FLASH_RAM_A, p, FLASH_PAGE_BYTES);
    flash_issue(qts, FLASH_RAM_A, 3, CMD_PAGE_WRITE);

    flash_irq_clear(qts);
    flash_issue_nowait(qts, FLASH_RAM_A, 3, CMD_BLOCK_ERASE);
    flash_assert_busy(qts);
    flash_step_us(qts, FLASH_TBERS_US - 1);
    flash_assert_busy(qts);
    g_assert_false(flash_irq_pending(qts));
    flash_pread_page(stix_tmp_flash_path, 3ULL * FLASH_PAGE_BYTES, host);
    flash_invert(host, FLASH_PAGE_BYTES);
    g_assert_cmpint(memcmp(host, p, FLASH_PAGE_BYTES), ==, 0);

    flash_step_us(qts, 2);
    flash_assert_done_ok(qts);
    g_assert_true(flash_irq_pending(qts));
    flash_pread_page(stix_tmp_flash_path, 3ULL * FLASH_PAGE_BYTES, host);
    g_assert_cmpint(memcmp(host, z0, FLASH_PAGE_BYTES), ==, 0);
    flash_assert_no_more_irq(qts);
    g_assert_cmpint(stix_qom_counter(qts, FLASH_QOM_PATH, "ops-erase"),
                    ==, 1);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/flash-latency-abort (D-F4, review R1): a RESET aborts a
 * pending op cleanly; a completion that lands between Flash_reset's
 * RAMAD/PAGEAD writes and its RESET cmdSta must not fail the RESET.
 */
static void test_flash_latency_abort(void)
{
    QTestState *qts = stix_qtest_new_extra(FLASH_LATENCY_ON);
    uint32_t sta;

    /* (a) Flash_reset (Flash.c:192-231) during a pending read */
    flash_ram_fill(qts, FLASH_RAM_A, 0x00);
    flash_irq_clear(qts);
    flash_issue_nowait(qts, FLASH_RAM_A, 0, CMD_PAGE_READ);
    flash_assert_busy(qts);
    flash_issue_nowait(qts, FLASH_RAM_A, 0, CMD_RESET);
    flash_assert_done_ok(qts);
    flash_step_us(qts, FLASH_TREAD_US + 100);
    g_assert_false(flash_irq_pending(qts));
    g_assert_cmphex(qtest_readl(qts, FLASH_RAM_A), ==, 0);
    g_assert_cmpint(stix_qom_counter(qts, FLASH_QOM_PATH, "ops-abort"),
                    ==, 1);

    /* (b) the op completes between RAMAD/PAGEAD and the RESET cmdSta */
    flash_issue_nowait(qts, FLASH_RAM_A, 0, CMD_PAGE_READ);
    qtest_writel(qts, FLASH_RAMAD, FLASH_RAM_A);
    qtest_writel(qts, FLASH_PAGEAD, 0);
    flash_step_us(qts, FLASH_TREAD_US + 1);
    flash_assert_done_ok(qts);
    g_assert_true(flash_irq_pending(qts));
    g_assert_cmphex(qtest_readl(qts, FLASH_RAM_A), ==, 0xffffffffu);
    flash_irq_clear(qts);
    qtest_writel(qts, FLASH_CMDSTA, FTNAND_FLASHTYPE | CMD_RESET);
    sta = qtest_readl(qts, FLASH_CMDSTA);
    g_assert_cmpuint(sta & FTNAND_FAILED, ==, 0);
    g_assert_cmpuint(sta & (FTNAND_BUSY | FTNAND_READY), ==, FTNAND_READY);
    g_assert_false(flash_irq_pending(qts));
    g_assert_cmpint(stix_qom_counter(qts, FLASH_QOM_PATH, "ops-abort"),
                    ==, 1);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/flash-latency-busy-reject (D-F4, review R2): while an op
 * is pending, a second command is dropped and a sequence error sets no
 * status and pulses no IRQ; the op in flight completes on its own terms.
 */
static void test_flash_latency_busy_reject(void)
{
    QTestState *qts = stix_qtest_new_extra(FLASH_LATENCY_ON);

    /* (a) a READ issued during a pending READ is dropped */
    flash_ram_fill(qts, FLASH_RAM_A, 0x00);
    flash_ram_fill(qts, FLASH_RAM_B, 0x00);
    flash_irq_clear(qts);
    flash_issue_nowait(qts, FLASH_RAM_A, 0, CMD_PAGE_READ);
    flash_issue_nowait(qts, FLASH_RAM_B, 1, CMD_PAGE_READ);
    flash_assert_busy(qts);
    g_assert_false(flash_irq_pending(qts));
    /* RAMAD/PAGEAD writes while busy are accepted as registers */
    g_assert_cmphex(qtest_readl(qts, FLASH_RAMAD), ==, FLASH_RAM_B);
    flash_step_us(qts, FLASH_TREAD_US + 1);
    flash_assert_done_ok(qts);
    g_assert_true(flash_irq_pending(qts));
    g_assert_cmphex(qtest_readl(qts, FLASH_RAM_A), ==, 0xffffffffu);
    g_assert_cmphex(qtest_readl(qts, FLASH_RAM_B), ==, 0);
    flash_assert_no_more_irq(qts);
    g_assert_cmpint(stix_qom_counter(qts, FLASH_QOM_PATH, "ops-read"),
                    ==, 1);

    /* (b) out-of-order cmdSta and PAGEAD writes during a pending op */
    flash_ram_fill(qts, FLASH_RAM_A, 0x00);
    flash_issue_nowait(qts, FLASH_RAM_A, 0, CMD_PAGE_READ);
    qtest_writel(qts, FLASH_CMDSTA, FTNAND_FLASHTYPE | CMD_PAGE_READ);
    flash_assert_busy(qts);
    g_assert_false(flash_irq_pending(qts));
    qtest_writel(qts, FLASH_PAGEAD, 0x10u);
    flash_assert_busy(qts);
    g_assert_false(flash_irq_pending(qts));
    flash_step_us(qts, FLASH_TREAD_US + 1);
    flash_assert_done_ok(qts);
    g_assert_true(flash_irq_pending(qts));
    g_assert_cmphex(qtest_readl(qts, FLASH_RAM_A), ==, 0xffffffffu);
    flash_assert_no_more_irq(qts);

    /* the sequence state is clean again: a fresh op runs */
    flash_issue(qts, FLASH_RAM_A, 0, CMD_PAGE_READ);
    flash_assert_done_ok(qts);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/flash-latency-dma-guard (D-F3, review R4): the read DMA
 * from the timer runs under the device's re-entrancy guard, like the
 * SPW's out-of-MMIO DMA (a83fe9a3a5). A ramAddress inside the flash's
 * own window is refused and the op completes FAILED | READY with the
 * IRQ. The page is programmed to an all-zero guest image first, so the
 * 4,224 B run-on into EEPROM/SPW/SDRAM-ctrl writes only zeros.
 *
 * The witnesses of the guard are RAMAD == FLASH_BASE after completion
 * (without the guard the page's zeros land in the flash registers and
 * RAMAD reads 0) and the follow-up flash_issue (the sequence state is
 * intact). FAILED | READY alone is not one: the run-on past the window
 * also fails the DMA. A program from the flash's own window is refused
 * at issue, when the RAM sample is taken (D-F2/D-F3): it completes
 * FAILED | READY and leaves the page in the file untouched.
 */
static void test_flash_latency_dma_guard(void)
{
    QTestState *qts = stix_qtest_new_extra(FLASH_LATENCY_ON);
    uint32_t sta;

    flash_ram_fill(qts, FLASH_RAM_A, 0x00);
    flash_issue(qts, FLASH_RAM_A, 7, CMD_PAGE_WRITE);
    flash_assert_done_ok(qts);

    flash_irq_clear(qts);
    flash_issue_nowait(qts, FLASH_BASE, 7, CMD_PAGE_READ);
    flash_assert_busy(qts);
    flash_step_us(qts, FLASH_TREAD_US + 1);
    sta = qtest_readl(qts, FLASH_CMDSTA);
    g_assert_cmpuint(sta & FTNAND_BUSY, ==, 0);
    g_assert_cmpuint(sta & (FTNAND_FAILED | FTNAND_READY), ==,
                     FTNAND_FAILED | FTNAND_READY);
    g_assert_true(flash_irq_pending(qts));
    g_assert_cmphex(qtest_readl(qts, FLASH_RAMAD), ==, FLASH_BASE);
    flash_assert_no_more_irq(qts);

    /* QEMU is up and the device usable */
    flash_issue(qts, FLASH_RAM_A, 0, CMD_PAGE_READ);
    flash_assert_done_ok(qts);
    g_assert_cmphex(qtest_readl(qts, FLASH_RAM_A), ==, 0xffffffffu);

    /* a program from the flash's own window: the sample is refused */
    {
        g_autofree uint8_t *host = g_malloc(FLASH_PAGE_BYTES);
        g_autofree uint8_t *z0 = g_malloc0(FLASH_PAGE_BYTES);

        flash_irq_clear(qts);
        flash_issue_nowait(qts, FLASH_BASE, 8, CMD_PAGE_WRITE);
        flash_assert_busy(qts);
        flash_step_us(qts, FLASH_TPROG_US + 1);
        sta = qtest_readl(qts, FLASH_CMDSTA);
        g_assert_cmpuint(sta & FTNAND_BUSY, ==, 0);
        g_assert_cmpuint(sta & (FTNAND_FAILED | FTNAND_READY), ==,
                         FTNAND_FAILED | FTNAND_READY);
        g_assert_true(flash_irq_pending(qts));
        /* nothing written: the erased page is still a host hole */
        flash_pread_page(stix_tmp_flash_path, 8ULL * FLASH_PAGE_BYTES, host);
        g_assert_cmpint(memcmp(host, z0, FLASH_PAGE_BYTES), ==, 0);
        flash_assert_no_more_irq(qts);
    }

    qtest_quit(qts);
    stix_qtest_cleanup();
}

#define FLASH_TEST_PROP_ENV "STIX_TEST_FLASH_PROP"

/*
 * g_test_trap_subprocess child: start QEMU with latency on and one more
 * -global (FLASH_TEST_PROP_ENV), then quit; the parent checks whether
 * realize failed and what QEMU printed
 */
static void test_flash_latency_props_subprocess(void)
{
    const char *flash = g_getenv(TMTC_TEST_FLASH_ENV);
    const char *prop = g_getenv(FLASH_TEST_PROP_ENV);
    QTestState *qts;

    g_assert_nonnull(flash);
    g_assert_nonnull(prop);
    qts = qtest_initf("-M leon3_generic "
                      "-global grlib-stixflash.flash-file=%s "
                      FLASH_LATENCY_ON " -global grlib-stixflash.%s",
                      flash, prop);
    qtest_quit(qts);
}

static void flash_props_trap(const char *prop, bool ok, const char *pat)
{
    g_autofree gchar *sub = stix_test_path("flash-latency-props/subprocess");

    g_setenv(FLASH_TEST_PROP_ENV, prop, TRUE);
    g_test_trap_subprocess(sub, 30 * G_USEC_PER_SEC, 0);
    if (ok) {
        g_test_trap_assert_passed();
    } else {
        g_test_trap_assert_failed();
    }
    if (pat) {
        g_test_trap_assert_stderr(pat);
    } else {
        g_test_trap_assert_stderr_unmatched("*stixflash*");
    }
}

/*
 * /sparc/stix/flash-latency-props (D-F6): each latency property is
 * validated at realize. 40,000 us is accepted; 40,001 fails realize,
 * naming the FSW's 50 ms timeout; 1-19 us warn that the op may not
 * block the caller. 0 makes that op type complete inline while the
 * others keep their latency.
 */
static void test_flash_latency_props(void)
{
    g_autofree gchar *path = NULL;
    int fd = g_file_open_tmp("stix-flash-XXXXXX", &path, NULL);
    QTestState *qts;

    g_assert(fd >= 0);
    g_assert(ftruncate(fd, 1024 * 1024) == 0);
    close(fd);
    g_setenv(TMTC_TEST_FLASH_ENV, path, TRUE);
    flash_props_trap("tread-us=40001", false, "*tread-us=40001*50 ms*");
    flash_props_trap("tprog-us=40001", false, "*tprog-us=40001*50 ms*");
    flash_props_trap("tbers-us=40001", false, "*tbers-us=40001*50 ms*");
    flash_props_trap("tread-us=40000", true, NULL);
    flash_props_trap("tbers-us=40000", true, NULL);
    flash_props_trap("tread-us=19", true, "*warning*tread-us=19 us*");
    flash_props_trap("tprog-us=1", true, "*warning*tprog-us=1 us*");
    flash_props_trap("tread-us=20", true, NULL);
    g_unlink(path);

    qts = stix_qtest_new_extra(FLASH_LATENCY_ON
                               " -global grlib-stixflash.tread-us=0");
    flash_ram_fill(qts, FLASH_RAM_A, 0x00);
    flash_irq_clear(qts);
    flash_issue_nowait(qts, FLASH_RAM_A, 0, CMD_PAGE_READ);
    flash_assert_done_ok(qts);
    g_assert_true(flash_irq_pending(qts));
    g_assert_cmphex(qtest_readl(qts, FLASH_RAM_A), ==, 0xffffffffu);
    /* a program still takes tprog-us */
    flash_issue_nowait(qts, FLASH_RAM_A, 9, CMD_PAGE_WRITE);
    flash_assert_busy(qts);
    flash_wait(qts);
    flash_assert_done_ok(qts);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/flash-latency-system-reset (D-F4): a system reset during
 * a pending op drops it: no IRQ, no DMA afterwards, cmdSta 0.
 */
static void test_flash_latency_system_reset(void)
{
    QTestState *qts = stix_qtest_new_extra(FLASH_LATENCY_ON);

    flash_ram_fill(qts, FLASH_RAM_A, 0x00);
    flash_issue_nowait(qts, FLASH_RAM_A, 0, CMD_PAGE_READ);
    flash_assert_busy(qts);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, FLASH_CMDSTA), ==, 0);
    flash_irq_clear(qts);
    flash_step_us(qts, 10 * 1000);
    g_assert_false(flash_irq_pending(qts));
    g_assert_cmphex(qtest_readl(qts, FLASH_RAM_A), ==, 0);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/flash-latency-default (F-3, D-F7): latency is on by
 * default, at the datasheet defaults, with no -global: a read is still
 * busy one microsecond before tread-us and done just after it. RED
 * while the default was off (the read completed inside the cmdSta write).
 */
static void test_flash_latency_default(void)
{
    QTestState *qts = stix_qtest_new();

    flash_ram_fill(qts, FLASH_RAM_A, 0x00);
    flash_irq_clear(qts);
    flash_issue_nowait(qts, FLASH_RAM_A, 0, CMD_PAGE_READ);
    flash_assert_busy(qts);
    flash_step_us(qts, FLASH_TREAD_US - 1);
    flash_assert_busy(qts);
    g_assert_false(flash_irq_pending(qts));
    flash_step_us(qts, 2);
    flash_assert_done_ok(qts);
    g_assert_true(flash_irq_pending(qts));
    g_assert_cmphex(qtest_readl(qts, FLASH_RAM_A), ==, 0xffffffffu);
    g_assert_true(stix_qom_get_bool(qts, FLASH_QOM_PATH, "latency", NULL));

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/flash-savevm (review SHOULD-1): the flash has no vmstate
 * yet (platform roadmap R-r3), so it is unmigratable: a savevm must be
 * refused naming the device, instead of a snapshot that silently drops
 * a pending op and the sequence state.
 */
static void test_flash_savevm(void)
{
    QTestState *qts = stix_qtest_new();
    g_autofree char *out = qtest_hmp(qts, "savevm flash-test");

    g_test_message("savevm: %s", out);
    g_assert_nonnull(strstr(out, "non-migratable device"));
    g_assert_nonnull(strstr(out, "grlib-stixflash"));

    qtest_quit(qts);
    stix_qtest_cleanup();
}


/* ---- Attenuator A-1: mechanism and AttnCmdSta (no faults) ---- */

/*
 * The attenuator plan (docs/stix-port/2026-11-11-attenuator-plan.md)
 * keeps its qtests in this one appended block behind
 * stix_att_register(), so a merge with other tracks touches main() in
 * one line. The tests drive the exact FSW words (PFM Attenuator.c:99-104):
 * the command and the override in bits 4:0 of AttnCmdSta (CTRL 0x5C),
 * LV on through hkCmdSta LVEN (0x88). The move timelines use
 * att-travel-ms=500: the plan's +250/+510 expectations and att-override's
 * "reaches IN at 500 ms" predate its Q3 answer, so the default (1000)
 * stays and the property is set explicitly. A-2 adds the ATT_C current
 * profile (hkAdc ch 8, D-Af) and A-3 the fault layer, the forcing
 * masks and the verbs (D-Ag) to the same block.
 */

#define ATT_PATH  "/machine/stixctrl/attenuator"
#define CTRL_ATT  0x5cu
#define ATT_MS_NS (1000 * 1000LL)

/* AttnCmdSta status bits (StixIdpu.h:709-731) */
#define ATT_TO    (1u << 5)     /* TIMEOUT */
#define ATT_M1U   (1u << 6)     /* MOTOR1USED */
#define ATT_M2U   (1u << 7)     /* MOTOR2USED */
#define ATT_OP    (1u << 8)     /* OPERATIONSTATUS, 1 = finished/idle */
#define ATT_BC    (1u << 9)
#define ATT_AB    (1u << 10)

/* 500 ms travel so the plan's qtest timelines hold (see the header) */
static QTestState *att_qtest_new(const char *extra)
{
    g_autofree gchar *all = g_strdup_printf(
        "-global grlib-stixctrl.att-travel-ms=500 %s", extra);

    return stix_qtest_new_extra(all);
}

static void att_lv_on(QTestState *qts)
{
    ctrl_write(qts, CTRL_HKCMDSTA, 0x1u);   /* LVEN */
}

static uint64_t att_qom_u64(QTestState *qts, const char *prop)
{
    bool ok;
    uint64_t v = stix_qom_get_u64(qts, ATT_PATH, prop, &ok);

    g_assert(ok);
    return v;
}

static bool att_qom_bool(QTestState *qts, const char *prop)
{
    bool ok, v = stix_qom_get_bool(qts, ATT_PATH, prop, &ok);

    g_assert(ok);
    return v;
}

/* a string property, or NULL when it is unknown */
static char *att_qom_str(QTestState *qts, const char *prop)
{
    QDict *resp = qtest_qmp(qts,
                            "{'execute': 'qom-get',"
                            " 'arguments': {'path': %s, 'property': %s}}",
                            ATT_PATH, prop);
    const char *s = qdict_get_try_str(resp, "return");
    char *out;

    g_assert_nonnull(s);
    out = g_strdup(s);
    qobject_unref(resp);
    return out;
}

/* assert a string property reads exactly @want */
static void att_qom_str_is(QTestState *qts, const char *prop,
                           const char *want)
{
    g_autofree char *got = att_qom_str(qts, prop);

    g_assert_cmpstr(got, ==, want);
}

/* qom-set of a string property; true when QEMU accepted it */
static bool att_qom_set_str(QTestState *qts, const char *prop,
                            const char *val)
{
    QDict *resp = qtest_qmp(qts,
                            "{'execute': 'qom-set',"
                            " 'arguments': {'path': %s, 'property': %s,"
                            " 'value': %s}}",
                            ATT_PATH, prop, val);
    bool ok = qdict_haskey(resp, "return");

    qobject_unref(resp);
    return ok;
}

/*
 * att-reset (R5): idle at OUT reads 0x500 (OPERATIONSTATUS 1, AB 1),
 * truth.position is out, and fsw-accesses counts the guest 0x5C reads
 * without being cleared by a reset.
 */
static void test_att_reset(void)
{
    QTestState *qts = att_qtest_new("");

    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, ATT_OP | ATT_AB);
    att_qom_str_is(qts, "truth.position", "out");
    g_assert_cmpuint(att_qom_u64(qts, "fsw-accesses"), ==, 1);
    ctrl_read(qts, CTRL_ATT);
    g_assert_cmpuint(att_qom_u64(qts, "fsw-accesses"), ==, 2);
    qtest_system_reset(qts);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, ATT_OP | ATT_AB);
    g_assert_cmpuint(att_qom_u64(qts, "fsw-accesses"), ==, 3);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-close-open: close M1 (4) moves OUT -> IN (AB 1, then both 0 at
 * 250 ms, BC 1 and finished with M1USED 0 at 510 ms), open M1 (1) moves
 * back; the same for M2 (8/2) and both motors (12/3).
 */
static void test_att_close_open(void)
{
    static const struct {
        uint32_t open_cmd, close_cmd, used;
    } cases[] = {
        { 1,  4, ATT_M1U },             /* M1 */
        { 2,  8, ATT_M2U },             /* M2 */
        { 3, 12, ATT_M1U | ATT_M2U },   /* both */
    };
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        QTestState *qts = att_qtest_new("");
        uint32_t close_cmd = cases[i].close_cmd;
        uint32_t open_cmd = cases[i].open_cmd;
        uint32_t used = cases[i].used;

        att_lv_on(qts);
        ctrl_write(qts, CTRL_ATT, close_cmd);
        g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==,
                        close_cmd | used | ATT_AB);
        qtest_clock_step(qts, 250 * ATT_MS_NS);
        g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, close_cmd | used);
        att_qom_str_is(qts, "truth.position", "moving-in");
        g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 500000);
        qtest_clock_step(qts, 260 * ATT_MS_NS);
        g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==,
                        close_cmd | ATT_OP | ATT_BC);
        att_qom_str_is(qts, "truth.position", "in");
        att_qom_str_is(qts, "truth.last-end", "target");

        ctrl_write(qts, CTRL_ATT, open_cmd);
        qtest_clock_step(qts, 510 * ATT_MS_NS);
        g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==,
                        open_cmd | ATT_OP | ATT_AB);
        att_qom_str_is(qts, "truth.position", "out");

        qtest_quit(qts);
        stix_qtest_cleanup();
    }
}

/*
 * att-redundant: a command toward the end the mechanism already sits
 * at ends after the debounce; MOTOR1USED reads 1 right after the write
 * (the FSW samples it there) and 0 after 10 ms (D-Ae, why 10 ms).
 */
static void test_att_redundant(void)
{
    QTestState *qts = att_qtest_new("");

    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 1);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 1u | ATT_M1U | ATT_AB);
    qtest_clock_step(qts, 10 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 1u | ATT_OP | ATT_AB);
    att_qom_str_is(qts, "truth.last-end", "target");

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-override: ERRORMASK ignores the switches; the shaft reaches IN
 * at 500 ms but the drive runs the full window and ends at 1600 ms
 * with no TIMEOUT (D-Ae).
 */
static void test_att_override(void)
{
    QTestState *qts = att_qtest_new("");

    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 4 | 0x10);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 0x14u | ATT_M1U | ATT_AB);
    qtest_clock_step(qts, 500 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 0x14u | ATT_M1U | ATT_BC);
    qtest_clock_step(qts, 1100 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 0x14u | ATT_OP | ATT_BC);
    att_qom_str_is(qts, "truth.last-end", "override");

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-lv-off: without hkCmdSta LVEN the motors have no power; the
 * shaft holds and the move ends at the ramp end with TIMEOUT and
 * OPERATIONSTATUS (att-timeout-finishes on).
 */
static void test_att_lv_off(void)
{
    QTestState *qts = att_qtest_new("");

    ctrl_write(qts, CTRL_ATT, 4);
    qtest_clock_step(qts, 250 * ATT_MS_NS);
    /* no motion; MOTOR1USED is the drive flag even without LV */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_M1U | ATT_AB);
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 0);
    /* nothing travels: the truth stays at the end stop */
    att_qom_str_is(qts, "truth.position", "out");
    g_assert(!att_qom_bool(qts, "truth.motor1-driving"));
    hk_check(qts, 8, 20);                     /* no powered motor: no draw */
    qtest_clock_step(qts, 1350 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==,
                    4u | ATT_TO | ATT_OP | ATT_AB);
    att_qom_str_is(qts, "truth.last-end", "timeout");

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-emergency: command 6 stops the move now; the position stays
 * between the ends with both switches released and OPERATIONSTATUS 1.
 */
static void test_att_emergency(void)
{
    QTestState *qts = att_qtest_new("");

    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 4);
    qtest_clock_step(qts, 250 * ATT_MS_NS);
    ctrl_write(qts, CTRL_ATT, 6);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 6u | ATT_OP);
    att_qom_str_is(qts, "truth.position", "between");
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 500000);
    att_qom_str_is(qts, "truth.last-end", "emergency");
    qtest_clock_step(qts, 1500 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 6u | ATT_OP);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-reset-midmove: a system_reset aborts the move at the reset
 * instant; the position is kept, the registers go idle and
 * truth.last-end is reset (D-Ae).
 */
static void test_att_reset_midmove(void)
{
    QTestState *qts = att_qtest_new("");

    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 4);
    qtest_clock_step(qts, 250 * ATT_MS_NS);
    qtest_system_reset(qts);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, ATT_OP);
    att_qom_str_is(qts, "truth.position", "between");
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 500000);
    att_qom_str_is(qts, "truth.last-end", "reset");

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* att-initial-in: the mechanism boots covered; idle at IN reads 0x300 */
static void test_att_initial_in(void)
{
    QTestState *qts = att_qtest_new(
        "-global grlib-stixctrl.att-initial=in");

    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, ATT_OP | ATT_BC);
    att_qom_str_is(qts, "truth.position", "in");
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 1000000);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-config1: 0x60 and 0x64 stay plain storage (D-Aa): they round-trip
 * and no stixatt_* trace fires for them, so the trace golden keeps its
 * 12 identical 0x60 lines.
 */
static void test_att_config1(void)
{
    g_autofree gchar *log = NULL, *extra = NULL;
    g_autofree gchar *text = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("stix-att-trace-XXXXXX", &log, NULL);
    g_assert(fd >= 0);
    close(fd);
    extra = g_strdup_printf("-trace stixctrl_io_write -trace stixatt_cmd "
                            "-trace stixatt_end -D %s", log);
    qts = stix_qtest_new_extra(extra);

    ctrl_write(qts, 0x60, 0x1E0E4000u);     /* the FSW's AttnConfig1 [G] */
    g_assert_cmphex(ctrl_read(qts, 0x60), ==, 0x1E0E4000u);
    ctrl_write(qts, 0x64, 0x12345678u);
    g_assert_cmphex(ctrl_read(qts, 0x64), ==, 0x12345678u);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, ATT_OP | ATT_AB);

    qtest_quit(qts);
    stix_qtest_cleanup();
    g_assert(g_file_get_contents(log, &text, NULL, NULL));
    g_assert_null(strstr(text, "stixatt"));
    g_assert_nonnull(strstr(text, "addr = 0x60"));
    g_assert_nonnull(strstr(text, "0x1e0e4000"));
    unlink(log);
}

/*
 * att-model-off: 0x5C is plain storage, never reset, and the model
 * never sees an access (D-Ab).
 */
static void test_att_model_off(void)
{
    QTestState *qts = att_qtest_new(
        "-global grlib-stixctrl.att-model=off");

    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 0);
    ctrl_write(qts, CTRL_ATT, 0xFFFF0004u);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 0xFFFF0004u);
    qtest_clock_step(qts, 600 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 0xFFFF0004u);
    /* the QOM face shows what the guest reads, not the model's idle */
    g_assert_cmphex(att_qom_u64(qts, "presented.cmdsta"), ==, 0xFFFF0004u);
    g_assert_cmpuint(att_qom_u64(qts, "fsw-accesses"), ==, 0);
    /*
     * A-2 (D-Ab): the current stays at the static table 20; with the
     * model off no move can run, LVEN or not
     */
    ctrl_write(qts, CTRL_HKCMDSTA, 0x1u);
    ctrl_write(qts, CTRL_ATT, 0x4u);        /* plain storage, no move */
    qtest_clock_step(qts, 250 * ATT_MS_NS);
    hk_check(qts, 8, 20);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* att-qom-names: the dotted property names work over QMP (D-Ah [L]) */
static void test_att_qom_names(void)
{
    QTestState *qts = att_qtest_new("");

    att_qom_str_is(qts, "truth.position", "out");
    att_qom_str_is(qts, "truth.last-end", "none");
    g_assert_cmpuint(att_qom_u64(qts, "presented.cmdsta"), ==,
                     ATT_OP | ATT_AB);
    g_assert_cmpuint(att_qom_u64(qts, "truth.moves"), ==, 0);
    g_assert_cmpuint(att_qom_u64(qts, "fsw-accesses"), ==, 0);
    g_assert(att_qom_bool(qts, "truth.sensor-ab"));
    g_assert(!att_qom_bool(qts, "truth.sensor-bc"));
    g_assert(att_qom_bool(qts, "presented.sensor-ab"));
    g_assert(!att_qom_bool(qts, "presented.sensor-bc"));

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-determinism (D-Ai): the same clock_step script gives identical
 * 0x5C and ATT_C reads with and without interleaved qom-gets, and no
 * stixatt_* trace fires inside a getter: the getter that passes the
 * move end first commits it silently, so run B keeps the cmd line but
 * never gains an end line.
 */
static void test_att_determinism(void)
{
    g_autofree gchar *log_a = NULL, *log_b = NULL, *extra = NULL;
    g_autofree gchar *text_a = NULL, *text_b = NULL;
    QTestState *qts;
    uint32_t r[2][4], c[2];
    int fd, i;

    for (i = 0; i < 2; i++) {
        fd = g_file_open_tmp(i == 0 ? "stix-att-det-a-XXXXXX"
                                    : "stix-att-det-b-XXXXXX",
                             i == 0 ? &log_a : &log_b, NULL);
        g_assert(fd >= 0);
        close(fd);
        extra = g_strdup_printf("-trace stixatt_cmd -trace stixatt_end "
                                "-D %s", i == 0 ? log_a : log_b);
        qts = att_qtest_new(extra);
        att_lv_on(qts);
        ctrl_write(qts, CTRL_ATT, 4);
        r[i][0] = ctrl_read(qts, CTRL_ATT);
        qtest_clock_step(qts, 250 * ATT_MS_NS);
        if (i == 1) {
            /* what a UI poll would read mid-move */
            att_qom_str_is(qts, "truth.position", "moving-in");
            att_qom_u64(qts, "truth.position-ppm");
            att_qom_u64(qts, "presented.cmdsta");
            att_qom_bool(qts, "truth.sensor-ab");
            att_qom_bool(qts, "truth.sensor-bc");
            att_qom_u64(qts, "fsw-accesses");
        }
        r[i][1] = ctrl_read(qts, CTRL_ATT);
        /* the ATT_C conversion (ch 8): 120 with the one motor moving */
        ctrl_write(qts, CTRL_HKADC, 8);
        c[i] = (ctrl_read(qts, CTRL_HKADC) >> HKADC_VAL_SHIFT) &
               HKADC_VAL_MASK;
        qtest_clock_step(qts, 260 * ATT_MS_NS);   /* the move end, 510 ms */
        if (i == 1) {
            /* these cross the end: they must not trace (D-Ai) */
            att_qom_str_is(qts, "truth.last-end", "target");
            att_qom_str_is(qts, "truth.position", "in");
            att_qom_u64(qts, "presented.cmdsta");
        }
        r[i][2] = ctrl_read(qts, CTRL_ATT);
        qtest_clock_step(qts, 100 * ATT_MS_NS);
        r[i][3] = ctrl_read(qts, CTRL_ATT);
        qtest_quit(qts);
        stix_qtest_cleanup();
    }

    for (i = 0; i < 4; i++) {
        g_assert_cmpuint(r[0][i], ==, r[1][i]);
    }
    g_assert_cmpuint(c[0], ==, c[1]);
    g_assert(g_file_get_contents(log_a, &text_a, NULL, NULL));
    g_assert(g_file_get_contents(log_b, &text_b, NULL, NULL));
    g_assert_nonnull(strstr(text_a, "stixatt_cmd"));
    g_assert_nonnull(strstr(text_a, "stixatt_end"));
    g_assert_nonnull(strstr(text_b, "stixatt_cmd"));
    g_assert_null(strstr(text_b, "stixatt_end"));
    unlink(log_a);
    unlink(log_b);
}

/* ---- att-reject-*: the negative qtests ---- */

/* status bits 11:5 in a write are ignored (D-Ae) */
static void test_att_reject_status_bits(void)
{
    QTestState *qts = att_qtest_new("");

    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 0xFE0u | 4);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_M1U | ATT_AB);
    qtest_clock_step(qts, 510 * ATT_MS_NS);
    /* the move ran as command 4: no TIMEOUT, no FAILFLAG */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_OP | ATT_BC);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* the unused command values are stored for readback, no motion (D-Ae) */
static void test_att_reject_cmd_values(void)
{
    static const uint32_t cmds[] = { 0, 5, 7, 9, 10, 11, 13, 14, 15 };
    QTestState *qts = att_qtest_new("");
    size_t i;

    att_lv_on(qts);
    for (i = 0; i < G_N_ELEMENTS(cmds); i++) {
        ctrl_write(qts, CTRL_ATT, cmds[i]);
        g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, ATT_OP | ATT_AB |
                                                       cmds[i]);
        qtest_clock_step(qts, 600 * ATT_MS_NS);
        g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, ATT_OP | ATT_AB |
                                                       cmds[i]);
    }
    g_assert_cmpuint(att_qom_u64(qts, "truth.moves"), ==, 0);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* a command during a move restarts it from the current position [I] */
static void test_att_reject_restart(void)
{
    QTestState *qts = att_qtest_new("");

    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 4);
    qtest_clock_step(qts, 250 * ATT_MS_NS);   /* half way, moving in */
    ctrl_write(qts, CTRL_ATT, 1);             /* restart toward OUT */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 1u | ATT_M1U);
    qtest_clock_step(qts, 255 * ATT_MS_NS);   /* 5 ms before the stop */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 1u | ATT_M1U | ATT_AB);
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 0);
    qtest_clock_step(qts, 10 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 1u | ATT_OP | ATT_AB);
    g_assert_cmpuint(att_qom_u64(qts, "truth.moves"), ==, 2);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* qom-set on the read-only truth/presented/fsw-accesses is rejected */
static void test_att_reject_readonly(void)
{
    static const char *const ro_str[] = {
        "truth.position", "truth.last-end",
    };
    static const char *const ro_bool[] = {
        "truth.sensor-ab", "truth.sensor-bc",
        "truth.motor1-driving", "truth.motor2-driving",
        "presented.sensor-ab", "presented.sensor-bc",
    };
    static const char *const ro_num[] = {
        "truth.position-ppm", "truth.moves", "truth.last-end-ns",
        "presented.cmdsta", "fsw-accesses",
    };
    QTestState *qts = att_qtest_new("");
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(ro_str); i++) {
        g_assert(!att_qom_set_str(qts, ro_str[i], "in"));
    }
    for (i = 0; i < G_N_ELEMENTS(ro_bool); i++) {
        g_assert(!stix_qom_set_bool(qts, ATT_PATH, ro_bool[i], true));
    }
    for (i = 0; i < G_N_ELEMENTS(ro_num); i++) {
        g_assert(!stix_qom_set_u64(qts, ATT_PATH, ro_num[i], 0));
    }

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- A-2: the ATT_C current profile (hkAdc ch 8, D-Af) ---- */

#define HK_PROT_ATT 5u  /* the ATT_C protection line, threshold 751 */

/*
 * att-current (D-Af): hkAdc ch 8 reads 20 idle, 120 with one powered
 * motor moving and 220 with two, and 20 again once the drive stops at
 * the move end.
 */
static void test_att_current(void)
{
    QTestState *qts = att_qtest_new("");

    att_lv_on(qts);
    hk_check(qts, 8, 20);                     /* idle at OUT */
    ctrl_write(qts, CTRL_ATT, 4);             /* close M1 */
    qtest_clock_step(qts, 250 * ATT_MS_NS);
    hk_check(qts, 8, 120);                    /* one motor moving */
    qtest_clock_step(qts, 260 * ATT_MS_NS);   /* the end, 510 ms */
    hk_check(qts, 8, 20);                     /* the drive stopped */
    ctrl_write(qts, CTRL_ATT, 3);             /* open both */
    qtest_clock_step(qts, 250 * ATT_MS_NS);
    hk_check(qts, 8, 220);                    /* two motors moving */
    qtest_clock_step(qts, 260 * ATT_MS_NS);
    hk_check(qts, 8, 20);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-current-override (D-Af): an override close drives the full ramp
 * window: 120/220 to the end stop at 500 ms, then 320/620 while the
 * motor(s) stall at IN, and 20 again once the drive stops at the
 * 1600 ms ramp end.
 */
static void test_att_current_override(void)
{
    static const struct {
        uint32_t cmd;
        uint32_t moving, stall;
    } cases[] = {
        {  4u | 0x10u, 120, 320 },      /* close M1 with ERRORMASK */
        { 12u | 0x10u, 220, 620 },      /* both motors */
    };
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        QTestState *qts = att_qtest_new("");

        att_lv_on(qts);
        ctrl_write(qts, CTRL_ATT, cases[i].cmd);
        qtest_clock_step(qts, 490 * ATT_MS_NS);
        hk_check(qts, 8, cases[i].moving);      /* still travelling */
        qtest_clock_step(qts, 110 * ATT_MS_NS); /* at IN, 600 ms: stalled */
        hk_check(qts, 8, cases[i].stall);
        qtest_clock_step(qts, 990 * ATT_MS_NS); /* 1590 ms: still driven */
        hk_check(qts, 8, cases[i].stall);
        qtest_clock_step(qts, 20 * ATT_MS_NS);  /* past the ramp end */
        hk_check(qts, 8, 20);
        att_qom_str_is(qts, "truth.last-end", "override");

        qtest_quit(qts);
        stix_qtest_cleanup();
    }
}

/*
 * att-current-redundant (D-Af): the stall also covers the 10 ms
 * debounce of a redundant command at the end stop: the powered drive
 * is on with the shaft unable to move, so ch 8 reads 20 + 300 right
 * after the write and 20 once the debounce commits the move end.
 */
static void test_att_current_redundant(void)
{
    QTestState *qts = att_qtest_new("");

    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 4);             /* close M1 */
    qtest_clock_step(qts, 510 * ATT_MS_NS);   /* settled at IN */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_OP | ATT_BC);
    hk_check(qts, 8, 20);

    ctrl_write(qts, CTRL_ATT, 4);             /* the same command again */
    hk_check(qts, 8, 320);                    /* stalled in the debounce */
    qtest_clock_step(qts, 10 * ATT_MS_NS);    /* the debounce commits */
    hk_check(qts, 8, 20);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-current-inject (D-Af): with no fault, the platform's hw-state
 * hk_adc.8 shows truth = presented; adc-inject[8] is forcing and wins
 * at the conversion, in hw-state and in the child's QOM face
 * (truth.att-c/presented.att-c), while the truth keeps the
 * mechanism's value. The polls and conversions do not disturb the
 * move: it still ends on time.
 */
static void test_att_current_inject(void)
{
    QTestState *qts = att_qtest_new("");
    QDict *st, *ch;

    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 4);             /* close M1 */
    qtest_clock_step(qts, 250 * ATT_MS_NS);

    st = plat_hw_state(qts, "/machine/stixctrl");
    ch = plat_hk_ch(st, 8);
    g_assert_cmpint(qdict_get_int(ch, "truth"), ==, 120);
    g_assert_cmpint(qdict_get_int(ch, "presented"), ==, 120);
    g_assert(!qdict_get_bool(ch, "injected"));
    qobject_unref(st);
    /* the child's QOM face reads the same pair (D-Ah) */
    g_assert_cmpuint(att_qom_u64(qts, "truth.att-c"), ==, 120);
    g_assert_cmpuint(att_qom_u64(qts, "presented.att-c"), ==, 120);

    g_assert_true(adc_inject_set(qts, 8, 2000));
    hk_check(qts, 8, 2000);
    st = plat_hw_state(qts, "/machine/stixctrl");
    ch = plat_hk_ch(st, 8);
    g_assert_cmpint(qdict_get_int(ch, "truth"), ==, 120);
    g_assert_cmpint(qdict_get_int(ch, "presented"), ==, 2000);
    g_assert(qdict_get_bool(ch, "injected"));
    qobject_unref(st);
    /* the injection wins there too; the truth keeps the mechanism draw */
    g_assert_cmpuint(att_qom_u64(qts, "truth.att-c"), ==, 120);
    g_assert_cmpuint(att_qom_u64(qts, "presented.att-c"), ==, 2000);
    g_assert_true(adc_inject_set(qts, 8, ADC_INJECT_NONE));
    hk_check(qts, 8, 120);

    /* the polls did not disturb the move: still running at 250 ms */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_M1U);
    qtest_clock_step(qts, 260 * ATT_MS_NS);   /* the end, 510 ms */
    hk_check(qts, 8, 20);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-current-prot (D-Af): with the ATT_C line armed the way the FSW
 * does after 0x14A3 FDIR protection enabled (unlock + ProtEn, the
 * reset threshold 751, D-P4d), the 620 two-motor stall never trips it;
 * an 800 injection trips on the second sample, as the positive control
 * that the line is armed.
 */
static void test_att_current_prot(void)
{
    const uint32_t bit = 1u << (8 - HK_PROT_ATT);
    QTestState *qts = att_qtest_new("");

    att_lv_on(qts);
    hk_prot_write_unlocked(qts, HK_PROT_ATT, HKPROT_EN | 751);
    ctrl_write(qts, CTRL_HKIRQSTA, bit);      /* IRQEN for the line */

    ctrl_write(qts, CTRL_ATT, 12u | 0x10u);   /* override close, both */
    qtest_clock_step(qts, 600 * ATT_MS_NS);   /* stalled at IN */
    hk_check(qts, 8, 620);
    hk_check(qts, 8, 620);
    hk_check(qts, 8, 620);
    g_assert_cmphex(hk_protstat(qts), ==, 0);
    g_assert_false(hk_irq_pending(qts));

    g_assert_true(adc_inject_set(qts, 8, 800));
    hk_check(qts, 8, 800);
    g_assert_cmphex(hk_protstat(qts), ==, bit);
    g_assert_cmpuint(hk_hotsample(qts), ==, 800);
    g_assert_true(hk_irq_pending(qts));

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- A-3: the fault layer, the forcing and the verbs (D-Ag) ---- */

/*
 * qom-set of the signed attenuator.current.offset; true when QEMU
 * accepted it. (stix_qom_set_u64 is unsigned, so the negative half of
 * the range needs its own helper.)
 */
static bool att_offset_set(QTestState *qts, int32_t val)
{
    QDict *resp = qtest_qmp(qts,
                            "{'execute': 'qom-set',"
                            " 'arguments': {'path': %s,"
                            " 'property': 'attenuator.current.offset',"
                            " 'value': %d}}",
                            ATT_PATH, val);
    bool ok = qdict_haskey(resp, "return");

    qobject_unref(resp);
    return ok;
}

/* the fault-clear verb: a fault id or "all"; true when accepted */
static bool att_fault_clear(QTestState *qts, const char *id)
{
    QDict *resp = qtest_qmp(qts,
                            "{'execute': 'qom-set',"
                            " 'arguments': {'path': %s,"
                            " 'property': 'fault-clear', 'value': %s}}",
                            ATT_PATH, id);
    bool ok = qdict_haskey(resp, "return");

    qobject_unref(resp);
    return ok;
}

/*
 * att-fault-sensor-ab-open: the open (AB) switch reads 0 wherever the
 * truth is. Open M1 from IN: the shaft travels to OUT (500 ms) but the
 * stop never fires, so the drive stalls there until the 1600 ms ramp
 * end ends the move with TIMEOUT; clearing the fault unmasks the
 * switch (the plan's outcome row, [L]).
 */
static void test_att_fault_sensor_ab_open(void)
{
    QTestState *qts = att_qtest_new("");

    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 4);             /* close M1, to IN */
    qtest_clock_step(qts, 510 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_OP | ATT_BC);

    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.sensor_ab.stuck_open", true));
    ctrl_write(qts, CTRL_ATT, 1);             /* open M1, back to OUT */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 1u | ATT_M1U | ATT_BC);
    qtest_clock_step(qts, 250 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 1u | ATT_M1U);
    att_qom_str_is(qts, "truth.position", "moving-out");
    g_assert(att_qom_bool(qts, "truth.motor1-driving"));
    qtest_clock_step(qts, 260 * ATT_MS_NS);   /* the truth is at OUT */
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 0);
    att_qom_str_is(qts, "truth.position", "out");
    g_assert(att_qom_bool(qts, "truth.sensor-ab"));
    g_assert(!att_qom_bool(qts, "presented.sensor-ab"));
    hk_check(qts, 8, 320);                    /* driven but not moving */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 1u | ATT_M1U);
    qtest_clock_step(qts, 1100 * ATT_MS_NS);  /* the 1600 ms ramp end */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 1u | ATT_TO | ATT_OP);
    att_qom_str_is(qts, "truth.last-end", "timeout");
    hk_check(qts, 8, 20);

    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.sensor_ab.stuck_open",
                                    false));
    g_assert(att_qom_bool(qts, "presented.sensor-ab"));
    g_assert(!att_qom_bool(qts, "presented.sensor-bc"));

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-fault-sensor-ab-closed: AB reads 1 everywhere. Open M1 from IN:
 * the (stuck) target switch reads actuated at the write, so the drive
 * runs one debounce and stops — the truth ends one debounce of travel
 * off IN (the plan's outcome row says "IN"; the drive does run the
 * debounce, see the As built). Clearing recovers the true switch.
 */
static void test_att_fault_sensor_ab_closed(void)
{
    QTestState *qts = att_qtest_new("");

    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 4);             /* close M1, to IN */
    qtest_clock_step(qts, 510 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_OP | ATT_BC);

    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.sensor_ab.stuck_closed",
                                    true));
    ctrl_write(qts, CTRL_ATT, 1);             /* open M1 at IN */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==,
                    1u | ATT_M1U | ATT_AB | ATT_BC);
    qtest_clock_step(qts, 10 * ATT_MS_NS);    /* the debounce commits */
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 980000);
    att_qom_str_is(qts, "truth.position", "between");
    att_qom_str_is(qts, "truth.last-end", "target");
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 1u | ATT_OP | ATT_AB);
    g_assert(!att_qom_bool(qts, "truth.sensor-ab"));
    g_assert(att_qom_bool(qts, "presented.sensor-ab"));

    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.sensor_ab.stuck_closed",
                                    false));
    /*
     * a real open from 980000 ppm: 490 ms of travel to OUT, then the
     * 10 ms debounce on the (healthy) AB switch
     */
    ctrl_write(qts, CTRL_ATT, 1);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 1u | ATT_M1U);
    qtest_clock_step(qts, 495 * ATT_MS_NS);   /* on AB, debouncing */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 1u | ATT_M1U | ATT_AB);
    qtest_clock_step(qts, 5 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 1u | ATT_OP | ATT_AB);
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 0);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-fault-sensor-bc-open: the close (BC) switch reads 0 wherever the
 * truth is. Close M1 from OUT: the truth reaches IN and stalls there
 * until the ramp end ends the move with TIMEOUT; the register never
 * shows BC. Clearing unmasks it and a redundant close then ends on the
 * (healthy) switch after the debounce.
 */
static void test_att_fault_sensor_bc_open(void)
{
    QTestState *qts = att_qtest_new("");

    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.sensor_bc.stuck_open", true));
    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 4);             /* close M1 from OUT */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_M1U | ATT_AB);
    qtest_clock_step(qts, 250 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_M1U);
    att_qom_str_is(qts, "truth.position", "moving-in");
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 500000);
    g_assert(!att_qom_bool(qts, "presented.sensor-bc"));
    qtest_clock_step(qts, 260 * ATT_MS_NS);   /* the truth is at IN */
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 1000000);
    att_qom_str_is(qts, "truth.position", "in");
    g_assert(att_qom_bool(qts, "truth.sensor-bc"));
    g_assert(!att_qom_bool(qts, "presented.sensor-bc"));
    hk_check(qts, 8, 320);                    /* stalls at IN */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_M1U);
    qtest_clock_step(qts, 1100 * ATT_MS_NS);  /* the 1600 ms ramp end */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_TO | ATT_OP);
    att_qom_str_is(qts, "truth.last-end", "timeout");
    hk_check(qts, 8, 20);

    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.sensor_bc.stuck_open",
                                    false));
    g_assert(att_qom_bool(qts, "presented.sensor-bc"));
    ctrl_write(qts, CTRL_ATT, 4);             /* redundant close at IN */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_M1U | ATT_BC);
    qtest_clock_step(qts, 10 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_OP | ATT_BC);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-fault-sensor-bc-closed: BC reads 1 everywhere. Close M1 from
 * OUT: the stuck target switch reads actuated at the write, so the
 * move ends after the debounce with the truth one debounce of travel
 * off OUT and the FSW (and SID2) seeing IN — the plan's silence case.
 */
static void test_att_fault_sensor_bc_closed(void)
{
    QTestState *qts = att_qtest_new("");

    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.sensor_bc.stuck_closed",
                                    true));
    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 4);             /* close M1 from OUT */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==,
                    4u | ATT_M1U | ATT_AB | ATT_BC);
    qtest_clock_step(qts, 10 * ATT_MS_NS);    /* the debounce commits */
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 20000);
    att_qom_str_is(qts, "truth.position", "between");
    att_qom_str_is(qts, "truth.last-end", "target");
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_OP | ATT_BC);
    g_assert(!att_qom_bool(qts, "truth.sensor-bc"));
    g_assert(att_qom_bool(qts, "presented.sensor-bc"));

    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.sensor_bc.stuck_closed",
                                    false));
    g_assert(!att_qom_bool(qts, "presented.sensor-bc"));
    g_assert(!att_qom_bool(qts, "presented.sensor-ab"));
    ctrl_write(qts, CTRL_ATT, 1);             /* open back to OUT */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 1u | ATT_M1U);
    qtest_clock_step(qts, 20 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 1u | ATT_OP | ATT_AB);
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 0);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-fault-motor-dead: a dead motor has no torque and draws no
 * current, while MOTORxUSED (the drive flag) still sets; the mechanism
 * stays and the move runs out the window to TIMEOUT. One motor, the
 * other, and both together; clearing restores the move.
 */
static void test_att_fault_motor_dead(void)
{
    QTestState *qts = att_qtest_new("");

    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.motor1.dead", true));
    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 4);             /* close M1 from OUT */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_M1U | ATT_AB);
    g_assert(!att_qom_bool(qts, "truth.motor1-driving"));
    qtest_clock_step(qts, 250 * ATT_MS_NS);
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 0);
    att_qom_str_is(qts, "truth.position", "out");
    hk_check(qts, 8, 20);                     /* a dead motor draws 0 */
    qtest_clock_step(qts, 1360 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==,
                    4u | ATT_TO | ATT_OP | ATT_AB);
    att_qom_str_is(qts, "truth.last-end", "timeout");
    g_assert_true(att_fault_clear(qts, "attenuator.motor1.dead"));
    ctrl_write(qts, CTRL_ATT, 4);             /* recovery: the move runs */
    qtest_clock_step(qts, 510 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_OP | ATT_BC);

    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.motor2.dead", true));
    ctrl_write(qts, CTRL_ATT, 2);             /* open M2 from IN */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 2u | ATT_M2U | ATT_BC);
    qtest_clock_step(qts, 250 * ATT_MS_NS);
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 1000000);
    hk_check(qts, 8, 20);
    qtest_clock_step(qts, 1360 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==,
                    2u | ATT_TO | ATT_OP | ATT_BC);
    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.motor2.dead", false));
    ctrl_write(qts, CTRL_ATT, 2);             /* recovery */
    qtest_clock_step(qts, 510 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 2u | ATT_OP | ATT_AB);

    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.motor1.dead", true));
    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.motor2.dead", true));
    ctrl_write(qts, CTRL_ATT, 12);            /* close both from OUT */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==,
                    12u | ATT_M1U | ATT_M2U | ATT_AB);
    qtest_clock_step(qts, 250 * ATT_MS_NS);
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 0);
    hk_check(qts, 8, 20);
    qtest_clock_step(qts, 1360 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==,
                    12u | ATT_TO | ATT_OP | ATT_AB);
    g_assert_true(att_fault_clear(qts, "all"));

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-fault-jammed: the shaft does not move but the motors keep
 * driving it: the truth stays at OUT, the register never sees BC, and
 * ATT_C reads the one-motor stall (320) for the whole window until the
 * TIMEOUT; the torque flag stays set. Clearing unjams the mechanism.
 */
static void test_att_fault_jammed(void)
{
    QTestState *qts = att_qtest_new("");

    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.mechanism.jammed", true));
    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 4);             /* close M1 from OUT */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_M1U | ATT_AB);
    qtest_clock_step(qts, 250 * ATT_MS_NS);
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 0);
    att_qom_str_is(qts, "truth.position", "out");
    g_assert(att_qom_bool(qts, "truth.motor1-driving"));
    hk_check(qts, 8, 320);                    /* driven but not moving */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_M1U | ATT_AB);
    qtest_clock_step(qts, 1360 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==,
                    4u | ATT_TO | ATT_OP | ATT_AB);
    att_qom_str_is(qts, "truth.last-end", "timeout");
    hk_check(qts, 8, 20);

    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.mechanism.jammed", false));
    ctrl_write(qts, CTRL_ATT, 4);             /* recovery: it moves now */
    qtest_clock_step(qts, 510 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_OP | ATT_BC);
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 1000000);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-fault-current-zero: the presented ATT_C reads 0 at all times
 * ("motion with no current") while truth.att-c keeps the mechanism's
 * draw; the register is unaffected. adc-inject[8] forcing still wins
 * over the fault (the truth -> fault -> forcing order, D-Af).
 */
static void test_att_fault_current_zero(void)
{
    QTestState *qts = att_qtest_new("");

    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.current.zero", true));
    hk_check(qts, 8, 0);
    g_assert_cmpuint(att_qom_u64(qts, "truth.att-c"), ==, 20);
    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 4);             /* close M1 from OUT */
    qtest_clock_step(qts, 250 * ATT_MS_NS);
    hk_check(qts, 8, 0);                      /* motion with no current */
    g_assert_cmpuint(att_qom_u64(qts, "truth.att-c"), ==, 120);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_M1U);
    qtest_clock_step(qts, 260 * ATT_MS_NS);
    hk_check(qts, 8, 0);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_OP | ATT_BC);

    g_assert_true(att_fault_clear(qts, "attenuator.current.zero"));
    hk_check(qts, 8, 20);
    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.current.zero", true));
    g_assert_true(adc_inject_set(qts, 8, 2000));
    hk_check(qts, 8, 2000);                   /* the forcing wins */
    g_assert_true(adc_inject_set(qts, 8, ADC_INJECT_NONE));
    hk_check(qts, 8, 0);
    g_assert_true(att_fault_clear(qts, "all"));
    hk_check(qts, 8, 20);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-fault-current-offset: the presented ATT_C is clamp(truth +
 * offset) at all times, truth.att-c keeps the mechanism draw, and
 * hw-state hk_adc.8 shows the split. The clamp catches both ends and
 * clearing restores the raw value.
 */
static void test_att_fault_current_offset(void)
{
    QTestState *qts = att_qtest_new("");
    QDict *st, *ch;

    g_assert_true(att_offset_set(qts, 800));
    hk_check(qts, 8, 820);                    /* 20 idle + 800 */
    g_assert_cmpuint(att_qom_u64(qts, "truth.att-c"), ==, 20);
    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 4);             /* close M1 from OUT */
    qtest_clock_step(qts, 250 * ATT_MS_NS);
    hk_check(qts, 8, 920);                    /* 120 moving + 800 */
    g_assert_cmpuint(att_qom_u64(qts, "truth.att-c"), ==, 120);
    st = plat_hw_state(qts, "/machine/stixctrl");
    ch = plat_hk_ch(st, 8);
    g_assert_cmpint(qdict_get_int(ch, "truth"), ==, 120);
    g_assert_cmpint(qdict_get_int(ch, "presented"), ==, 920);
    qobject_unref(st);

    g_assert_true(att_offset_set(qts, -150));
    hk_check(qts, 8, 0);                      /* 120 - 150 clamps at 0 */
    g_assert_true(att_offset_set(qts, 4000));
    hk_check(qts, 8, 4095);                   /* 4120 clamps at 4095 */
    g_assert_true(att_fault_clear(qts, "attenuator.current.offset"));
    hk_check(qts, 8, 120);
    qtest_clock_step(qts, 260 * ATT_MS_NS);   /* the end, on time */
    hk_check(qts, 8, 20);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-fault-midmove (D-Ag, piecewise): sensor_bc.stuck_open set at
 * 250 ms re-solves the running close from now — the end leaves 510 ms
 * for the 1600 ms ramp end (the truth stalls at IN from 500 ms).
 * Clearing at 700 ms re-solves again: the switch is healthy now and
 * the truth sits on it, so the move ends at 710 ms (target + debounce).
 */
static void test_att_fault_midmove(void)
{
    g_autofree gchar *log = NULL, *extra = NULL;
    g_autofree gchar *text = NULL;
    QTestState *qts;
    uint64_t v0, end_ns;
    bool ok;
    int fd;

    fd = g_file_open_tmp("stix-att-fault-XXXXXX", &log, NULL);
    g_assert(fd >= 0);
    close(fd);
    extra = g_strdup_printf("-trace stixatt_fault -trace stixatt_end "
                            "-D %s", log);
    qts = att_qtest_new(extra);

    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 4);             /* close M1 from OUT */
    qtest_clock_step(qts, 250 * ATT_MS_NS);
    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.sensor_bc.stuck_open", true));
    qtest_clock_step(qts, 260 * ATT_MS_NS);   /* the old end, 510 ms */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_M1U);
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 1000000);
    att_qom_str_is(qts, "truth.position", "in");
    hk_check(qts, 8, 320);                    /* stalled at IN */
    qtest_clock_step(qts, 190 * ATT_MS_NS);   /* 700 ms */
    v0 = stix_qom_get_u64(qts, "/machine/stixspw", "vclock-ns", &ok);
    g_assert(ok);
    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.sensor_bc.stuck_open",
                                    false));
    qtest_clock_step(qts, 5 * ATT_MS_NS);     /* 705: in the debounce */
    /* the healthy switch reads actuated at once (monitored constantly) */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_M1U | ATT_BC);
    qtest_clock_step(qts, 10 * ATT_MS_NS);    /* 715: past the end */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_OP | ATT_BC);
    att_qom_str_is(qts, "truth.last-end", "target");
    end_ns = att_qom_u64(qts, "truth.last-end-ns");
    g_assert_cmpuint(end_ns - v0, ==, 10 * ATT_MS_NS);

    qtest_quit(qts);
    stix_qtest_cleanup();
    g_assert(g_file_get_contents(log, &text, NULL, NULL));
    g_assert_nonnull(strstr(text, "fault attenuator.sensor_bc.stuck_open"
                            " = 1"));
    g_assert_nonnull(strstr(text, "fault attenuator.sensor_bc.stuck_open"
                            " = 0"));
    g_assert_nonnull(strstr(text, "stixatt_end"));
    unlink(log);
}

/*
 * att-fault-m1-dead-midmove (D-Ag, piecewise): motor1.dead set at
 * 250 ms freezes the one-motor close at 500000 ppm (250 ms of travel,
 * 2000 ppm/ms) and re-solves the end to the 1600 ms ramp end — no live
 * motor. The clear at 500 ms re-solves from the frozen position:
 * 500000 ppm to IN is 250 ms, plus the 10 ms debounce, so the end
 * moves to 760 ms and commits on the (healthy) BC switch.
 */
static void test_att_fault_m1_dead_midmove(void)
{
    QTestState *qts = att_qtest_new("");
    uint64_t v0, end_ns;
    bool ok;

    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 4);             /* close M1 from OUT */
    qtest_clock_step(qts, 250 * ATT_MS_NS);
    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.motor1.dead", true));
    /* frozen at the fold: 250 ms of travel, the dead motor draws 0 */
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 500000);
    g_assert(!att_qom_bool(qts, "truth.motor1-driving"));
    hk_check(qts, 8, 20);
    qtest_clock_step(qts, 250 * ATT_MS_NS);   /* 500 ms, still frozen */
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 500000);

    v0 = stix_qom_get_u64(qts, "/machine/stixspw", "vclock-ns", &ok);
    g_assert(ok);
    g_assert_true(att_fault_clear(qts, "attenuator.motor1.dead"));
    /* resumed: 500000 ppm to IN in 250 ms, then the 10 ms debounce */
    qtest_clock_step(qts, 250 * ATT_MS_NS);   /* 750 ms: on BC */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_M1U | ATT_BC);
    qtest_clock_step(qts, 10 * ATT_MS_NS);    /* 760: the end commits */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_OP | ATT_BC);
    att_qom_str_is(qts, "truth.last-end", "target");
    end_ns = att_qom_u64(qts, "truth.last-end-ns");
    g_assert_cmpuint(end_ns - v0, ==, 260 * ATT_MS_NS);
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 1000000);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-fault-jammed-midmove (D-Ag, piecewise): mechanism.jammed set at
 * 250 ms freezes the close at 500000 ppm with the motor driving but
 * stalled against the jam (ATT_C 320, the torque flag stays) and
 * re-solves the end to the 1600 ms ramp end. The clear at 350 ms
 * re-solves from the frozen position: 500000 ppm to IN is 250 ms, plus
 * the 10 ms debounce, so the end moves to 610 ms.
 */
static void test_att_fault_jammed_midmove(void)
{
    QTestState *qts = att_qtest_new("");
    uint64_t v0, end_ns;
    bool ok;

    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 4);             /* close M1 from OUT */
    qtest_clock_step(qts, 250 * ATT_MS_NS);
    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.mechanism.jammed",
                                    true));
    /* frozen at the fold: the motor drives, the shaft holds (320) */
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 500000);
    g_assert(att_qom_bool(qts, "truth.motor1-driving"));
    hk_check(qts, 8, 320);
    qtest_clock_step(qts, 100 * ATT_MS_NS);   /* 350 ms, still frozen */
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 500000);

    v0 = stix_qom_get_u64(qts, "/machine/stixspw", "vclock-ns", &ok);
    g_assert(ok);
    g_assert_true(att_fault_clear(qts, "attenuator.mechanism.jammed"));
    /* resumed: 500000 ppm to IN in 250 ms, then the 10 ms debounce */
    qtest_clock_step(qts, 250 * ATT_MS_NS);   /* 600 ms: on BC */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_M1U | ATT_BC);
    qtest_clock_step(qts, 10 * ATT_MS_NS);    /* 610: the end commits */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_OP | ATT_BC);
    att_qom_str_is(qts, "truth.last-end", "target");
    end_ns = att_qom_u64(qts, "truth.last-end-ns");
    g_assert_cmpuint(end_ns - v0, ==, 260 * ATT_MS_NS);
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 1000000);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-fault-window-end (D-Ag): a mid-move inject never extends the
 * drive past the anchored ramp end. sensor_bc.stuck_open keeps the
 * close running to the 1600 ms window end (the truth stalls at IN from
 * 500 ms); a stuck-closed target set 5 ms before it re-solves to
 * now + debounce = 1605 ms, which no longer fits, so the move ends at
 * the 1600 ms ramp end with the timeout semantics, not one debounce
 * later.
 */
static void test_att_fault_window_end(void)
{
    QTestState *qts = att_qtest_new("");
    uint64_t v0, end_ns;
    bool ok;

    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.sensor_bc.stuck_open",
                                    true));
    att_lv_on(qts);
    v0 = stix_qom_get_u64(qts, "/machine/stixspw", "vclock-ns", &ok);
    g_assert(ok);
    ctrl_write(qts, CTRL_ATT, 4);             /* close M1 from OUT */
    qtest_clock_step(qts, 1595 * ATT_MS_NS);
    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.sensor_bc.stuck_closed",
                                    true));
    qtest_clock_step(qts, 5 * ATT_MS_NS);     /* 1600: the ramp end */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==,
                    4u | ATT_TO | ATT_OP | ATT_BC);
    att_qom_str_is(qts, "truth.last-end", "timeout");
    end_ns = att_qom_u64(qts, "truth.last-end-ns");
    g_assert_cmpuint(end_ns - v0, ==, 1600 * ATT_MS_NS);
    g_assert_cmpuint(att_qom_u64(qts, "truth.position-ppm"), ==, 1000000);
    qtest_clock_step(qts, 10 * ATT_MS_NS);    /* 1610: no second end */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==,
                    4u | ATT_TO | ATT_OP | ATT_BC);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-fault-reset (D-Ag): a fault survives system_reset (a broken
 * switch stays broken over an IDPU reboot) while the forcing masks do
 * not (they clear like adc-inject, D-P4b).
 */
static void test_att_fault_reset(void)
{
    QTestState *qts = att_qtest_new("");

    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 4);             /* close M1, to IN */
    qtest_clock_step(qts, 510 * ATT_MS_NS);
    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.sensor_ab.stuck_closed",
                                    true));
    g_assert_true(stix_qom_set_u64(qts, ATT_PATH, "force.cmdsta-set", 0x800));
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==,
                    4u | 0x800u | ATT_OP | ATT_BC | ATT_AB);

    qtest_system_reset(qts);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, ATT_OP | ATT_BC | ATT_AB);
    att_qom_str_is(qts, "faults-active",
                   "attenuator.sensor_ab.stuck_closed=1");
    g_assert_cmpuint(att_qom_u64(qts, "force.cmdsta-set"), ==, 0);
    g_assert_cmpuint(att_qom_u64(qts, "force.cmdsta-clear"), ==, 0);
    g_assert(!att_qom_bool(qts, "truth.sensor-ab"));
    g_assert(att_qom_bool(qts, "presented.sensor-ab"));
    g_assert_true(att_fault_clear(qts, "all"));
    att_qom_str_is(qts, "faults-active", "");

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-fault-trip: current.offset = 800 with the ATT_C protection line
 * armed the way the FSW does after 0x14A3. The set itself does not
 * sample (D-Af); the next conversions do, and the second consecutive
 * one above 751 trips PROTSTAT and the HK IRQ.
 */
static void test_att_fault_trip(void)
{
    const uint32_t bit = 1u << (8 - HK_PROT_ATT);
    QTestState *qts = att_qtest_new("");

    att_lv_on(qts);
    hk_prot_write_unlocked(qts, HK_PROT_ATT, HKPROT_EN | 751);
    ctrl_write(qts, CTRL_HKIRQSTA, bit);      /* IRQEN for the line */

    g_assert_true(att_offset_set(qts, 800));
    g_assert_cmphex(hk_protstat(qts), ==, 0); /* the set does not sample */
    g_assert_false(hk_irq_pending(qts));
    hk_check(qts, 8, 820);                    /* sample 1: high */
    g_assert_cmphex(hk_protstat(qts), ==, 0);
    hk_check(qts, 8, 820);                    /* sample 2: trips */
    g_assert_cmphex(hk_protstat(qts), ==, bit);
    g_assert_cmpuint(hk_hotsample(qts), ==, 820);
    g_assert_true(hk_irq_pending(qts));

    g_assert_true(att_fault_clear(qts, "attenuator.current.offset"));
    hk_check(qts, 8, 20);                     /* low again */
    g_assert_cmphex(hk_protstat(qts), ==, bit); /* latched */

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-fault-reject: the rejected sets fail with an error and change
 * nothing (D-Ag): forcing bits outside 11:5, an offset outside
 * -4095..4095, an unknown id in fault-clear, qom-set on the read-only
 * faults-active, an att-model change after realize (the generic qdev
 * post-realize rule; the error names the property), and any fault or
 * forcing set with att-model=off.
 */
static void test_att_fault_reject(void)
{
    QTestState *qts = att_qtest_new("");

    g_assert(!stix_qom_set_u64(qts, ATT_PATH, "force.cmdsta-set", 0x1u));
    g_assert(!stix_qom_set_u64(qts, ATT_PATH, "force.cmdsta-set", 0x10u));
    g_assert(!stix_qom_set_u64(qts, ATT_PATH, "force.cmdsta-clear", 0x1001u));
    g_assert(!stix_qom_set_u64(qts, ATT_PATH, "force.cmdsta-set",
                               0xFFFFFFFFu));
    g_assert(!att_offset_set(qts, 4096));
    g_assert(!att_offset_set(qts, -4096));
    g_assert(!att_fault_clear(qts, "attenuator.nonsense"));
    g_assert(!att_fault_clear(qts, "sensor_bc.stuck_open"));
    g_assert(!att_qom_set_str(qts, "faults-active", ""));
    g_assert(!stix_qom_set_bool(qts, "/machine/stixctrl", "att-model",
                                false));
    /* the att-model reject changed nothing: the model still runs */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, ATT_OP | ATT_AB);
    /* the valid neighbours are accepted */
    g_assert(stix_qom_set_u64(qts, ATT_PATH, "force.cmdsta-set", 0x800u));
    g_assert(stix_qom_set_u64(qts, ATT_PATH, "force.cmdsta-clear", 0x400u));
    g_assert(att_offset_set(qts, 4095));
    g_assert(att_offset_set(qts, -4095));
    g_assert_cmpuint(att_qom_u64(qts, "force.cmdsta-set"), ==, 0x800u);
    g_assert_cmpuint(att_qom_u64(qts, "force.cmdsta-clear"), ==, 0x400u);
    att_qom_str_is(qts, "faults-active", "attenuator.current.offset=-4095");
    g_assert_true(att_fault_clear(qts, "all"));
    /* fault-clear does not touch the forcing masks */
    g_assert_cmpuint(att_qom_u64(qts, "force.cmdsta-set"), ==, 0x800u);
    g_assert_cmpuint(att_qom_u64(qts, "force.cmdsta-clear"), ==, 0x400u);
    att_qom_str_is(qts, "faults-active", "");
    g_assert(stix_qom_set_u64(qts, ATT_PATH, "force.cmdsta-set", 0));
    g_assert(stix_qom_set_u64(qts, ATT_PATH, "force.cmdsta-clear", 0));

    qtest_quit(qts);
    stix_qtest_cleanup();

    qts = att_qtest_new("-global grlib-stixctrl.att-model=off");
    g_assert(!stix_qom_set_bool(qts, ATT_PATH,
                                "attenuator.sensor_ab.stuck_open", true));
    g_assert(!att_offset_set(qts, 10));
    g_assert(!stix_qom_set_u64(qts, ATT_PATH, "force.cmdsta-set", 0x800u));
    g_assert(!att_fault_clear(qts, "all"));
    att_qom_str_is(qts, "faults-active", "");

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-force-cmdsta (D-Ag): the forcing masks are applied to what a
 * 0x5C read returns, after everything else — no physics. They show in
 * presented.cmdsta, they are no faults (faults-active stays empty and
 * fault-clear leaves them), and a forced BC bit does not stop a move.
 */
static void test_att_force_cmdsta(void)
{
    QTestState *qts = att_qtest_new("");

    g_assert_true(stix_qom_set_u64(qts, ATT_PATH, "force.cmdsta-set", 0x800));
    g_assert_cmpuint(att_qom_u64(qts, "force.cmdsta-set"), ==, 0x800);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==,
                    0x800u | ATT_OP | ATT_AB);
    g_assert_cmphex(att_qom_u64(qts, "presented.cmdsta"), ==,
                    0x800u | ATT_OP | ATT_AB);
    g_assert_true(stix_qom_set_u64(qts, ATT_PATH, "force.cmdsta-clear",
                                   ATT_OP));
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 0x800u | ATT_AB);
    /* no physics: the truth is untouched and the masks are no faults */
    att_qom_str_is(qts, "truth.position", "out");
    att_qom_str_is(qts, "truth.last-end", "none");
    att_qom_str_is(qts, "faults-active", "");
    g_assert_true(att_fault_clear(qts, "all"));
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 0x800u | ATT_AB);

    /* a forced BC bit does not stop a close */
    g_assert_true(stix_qom_set_u64(qts, ATT_PATH, "force.cmdsta-set", 0));
    g_assert_true(stix_qom_set_u64(qts, ATT_PATH, "force.cmdsta-clear", 0));
    g_assert_true(stix_qom_set_u64(qts, ATT_PATH, "force.cmdsta-set",
                                   ATT_BC));
    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 4);
    qtest_clock_step(qts, 250 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_M1U | ATT_BC);
    att_qom_str_is(qts, "truth.position", "moving-in");
    qtest_clock_step(qts, 260 * ATT_MS_NS);
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==,
                    4u | ATT_OP | ATT_BC);
    att_qom_str_is(qts, "truth.last-end", "target");
    g_assert_true(stix_qom_set_u64(qts, ATT_PATH, "force.cmdsta-set", 0));

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-fault-verbs: faults-active lists every fault off its inactive
 * value in catalogue order as id=value,..., fault-clear removes one id
 * or all of them, and clearing an inactive id is accepted.
 */
static void test_att_fault_verbs(void)
{
    QTestState *qts = att_qtest_new("");

    att_qom_str_is(qts, "faults-active", "");
    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.sensor_ab.stuck_closed",
                                    true));
    att_qom_str_is(qts, "faults-active",
                   "attenuator.sensor_ab.stuck_closed=1");
    g_assert_true(att_offset_set(qts, 800));
    att_qom_str_is(qts, "faults-active",
                   "attenuator.sensor_ab.stuck_closed=1,"
                   "attenuator.current.offset=800");
    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.sensor_bc.stuck_open",
                                    true));
    att_qom_str_is(qts, "faults-active",
                   "attenuator.sensor_ab.stuck_closed=1,"
                   "attenuator.sensor_bc.stuck_open=1,"
                   "attenuator.current.offset=800");

    g_assert_true(att_fault_clear(qts, "attenuator.sensor_ab.stuck_closed"));
    att_qom_str_is(qts, "faults-active",
                   "attenuator.sensor_bc.stuck_open=1,"
                   "attenuator.current.offset=800");
    g_assert_true(att_fault_clear(qts, "all"));
    att_qom_str_is(qts, "faults-active", "");
    g_assert_true(att_fault_clear(qts, "attenuator.sensor_bc.stuck_open"));
    att_qom_str_is(qts, "faults-active", "");
    g_assert_true(att_offset_set(qts, -4095));
    att_qom_str_is(qts, "faults-active", "attenuator.current.offset=-4095");
    g_assert_true(att_fault_clear(qts, "attenuator.current.offset"));
    att_qom_str_is(qts, "faults-active", "");

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * att-fault-hw-state (D-Ah): the controller's hw-state carries an
 * attenuator object with truth, presented, fsw_accesses and
 * faults_active. Six polls across one close (0, 100, 250, 400 and
 * 500 ms, then one with a fault) each see the exact model state: the
 * travel is 2000 ppm/ms, the draw 120 while moving and 320 stalled on
 * BC from 500 ms (the end commits at 510 ms); at the command instant
 * the shaft is commanded (moving-in) but still at OUT with AB
 * actuated. The polls are pure (the PL-1/PL-2 note): they do not
 * disturb the move, and fsw_accesses only counts the guest 0x5C
 * accesses.
 */
static void test_att_fault_hw_state(void)
{
    static const struct {
        int ms;                 /* the step since the last poll */
        uint32_t ppm;           /* the shaft position at the poll */
        const char *pos;        /* truth.position at the poll */
        uint32_t att_c;         /* the mechanism draw at the poll */
        bool ab, bc;            /* the truth (== presented) switches */
    } polls[] = {
        {   0,       0, "moving-in", 120, true,  false },
        { 100,  200000, "moving-in", 120, false, false },
        { 150,  500000, "moving-in", 120, false, false },
        { 150,  800000, "moving-in", 120, false, false },
        { 100, 1000000, "in",        320, false, true  },
    };
    QTestState *qts = att_qtest_new("");
    QDict *st, *att, *tr, *pr, *model;
    size_t i;

    att_lv_on(qts);
    ctrl_write(qts, CTRL_ATT, 4);             /* close M1 from OUT */
    for (i = 0; i < G_N_ELEMENTS(polls); i++) {
        if (polls[i].ms) {          /* qtest refuses a zero step */
            qtest_clock_step(qts, polls[i].ms * ATT_MS_NS);
        }
        st = plat_hw_state(qts, "/machine/stixctrl");
        model = plat_dict(st, "model");
        g_assert(qdict_get_bool(model, "att_model"));
        att = plat_dict(st, "attenuator");
        tr = plat_dict(att, "truth");
        pr = plat_dict(att, "presented");
        g_assert_cmpstr(qdict_get_str(tr, "position"), ==, polls[i].pos);
        g_assert_cmpint(qdict_get_int(tr, "position_ppm"), ==,
                        polls[i].ppm);
        g_assert_cmpint(qdict_get_bool(tr, "sensor_ab"), ==, polls[i].ab);
        g_assert_cmpint(qdict_get_bool(tr, "sensor_bc"), ==, polls[i].bc);
        g_assert_cmpint(qdict_get_bool(pr, "sensor_ab"), ==, polls[i].ab);
        g_assert_cmpint(qdict_get_bool(pr, "sensor_bc"), ==, polls[i].bc);
        g_assert_cmpint(qdict_get_int(tr, "att_c"), ==, polls[i].att_c);
        g_assert_cmpint(qdict_get_int(tr, "moves"), ==, 1);
        g_assert_cmpstr(qdict_get_str(tr, "last_end"), ==, "none");
        g_assert(qdict_get_bool(tr, "motor1_driving"));
        g_assert(!qdict_get_bool(tr, "motor2_driving"));
        g_assert_cmpint(qdict_get_int(pr, "att_c"), ==, polls[i].att_c);
        g_assert_cmpint(qdict_get_int(pr, "cmdsta"), ==,
                        4u | ATT_M1U | (polls[i].ab ? ATT_AB : 0u) |
                        (polls[i].bc ? ATT_BC : 0u));
        g_assert_cmpint(qdict_get_int(att, "fsw_accesses"), ==, 1);
        g_assert_cmpstr(qdict_get_str(att, "faults_active"), ==, "");
        qobject_unref(st);
    }

    /* a sixth poll: current.zero changes only the presented half */
    g_assert_true(stix_qom_set_bool(qts, ATT_PATH,
                                    "attenuator.current.zero", true));
    st = plat_hw_state(qts, "/machine/stixctrl");
    att = plat_dict(st, "attenuator");
    tr = plat_dict(att, "truth");
    pr = plat_dict(att, "presented");
    g_assert_cmpint(qdict_get_int(tr, "att_c"), ==, 320);
    g_assert_cmpint(qdict_get_int(pr, "att_c"), ==, 0);
    g_assert_cmpstr(qdict_get_str(att, "faults_active"), ==,
                    "attenuator.current.zero=1");
    qobject_unref(st);

    /* the polls and the fault set did not disturb the move */
    qtest_clock_step(qts, 10 * ATT_MS_NS);    /* the end, 510 ms */
    g_assert_cmphex(ctrl_read(qts, CTRL_ATT), ==, 4u | ATT_OP | ATT_BC);
    g_assert_true(att_fault_clear(qts, "all"));
    st = plat_hw_state(qts, "/machine/stixctrl");
    att = plat_dict(st, "attenuator");
    g_assert_cmpint(qdict_get_int(plat_dict(att, "presented"), "att_c"),
                    ==, 20);
    g_assert_cmpstr(qdict_get_str(att, "faults_active"), ==, "");
    qobject_unref(st);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

static void stix_att_register(void)
{
    qtest_add_func("/sparc/stix/att-reset", test_att_reset);
    qtest_add_func("/sparc/stix/att-close-open", test_att_close_open);
    qtest_add_func("/sparc/stix/att-redundant", test_att_redundant);
    qtest_add_func("/sparc/stix/att-override", test_att_override);
    qtest_add_func("/sparc/stix/att-lv-off", test_att_lv_off);
    qtest_add_func("/sparc/stix/att-emergency", test_att_emergency);
    qtest_add_func("/sparc/stix/att-reset-midmove", test_att_reset_midmove);
    qtest_add_func("/sparc/stix/att-initial-in", test_att_initial_in);
    qtest_add_func("/sparc/stix/att-config1", test_att_config1);
    qtest_add_func("/sparc/stix/att-model-off", test_att_model_off);
    qtest_add_func("/sparc/stix/att-qom-names", test_att_qom_names);
    qtest_add_func("/sparc/stix/att-determinism", test_att_determinism);
    qtest_add_func("/sparc/stix/att-reject-status-bits",
                   test_att_reject_status_bits);
    qtest_add_func("/sparc/stix/att-reject-cmd-values",
                   test_att_reject_cmd_values);
    qtest_add_func("/sparc/stix/att-reject-restart",
                   test_att_reject_restart);
    qtest_add_func("/sparc/stix/att-reject-readonly",
                   test_att_reject_readonly);
    qtest_add_func("/sparc/stix/att-current", test_att_current);
    qtest_add_func("/sparc/stix/att-current-override",
                   test_att_current_override);
    qtest_add_func("/sparc/stix/att-current-redundant",
                   test_att_current_redundant);
    qtest_add_func("/sparc/stix/att-current-inject",
                   test_att_current_inject);
    qtest_add_func("/sparc/stix/att-current-prot", test_att_current_prot);
    /* A-3: one test per catalogue row, plus the verbs and forcing */
    qtest_add_func("/sparc/stix/att-fault-sensor-ab-open",
                   test_att_fault_sensor_ab_open);
    qtest_add_func("/sparc/stix/att-fault-sensor-ab-closed",
                   test_att_fault_sensor_ab_closed);
    qtest_add_func("/sparc/stix/att-fault-sensor-bc-open",
                   test_att_fault_sensor_bc_open);
    qtest_add_func("/sparc/stix/att-fault-sensor-bc-closed",
                   test_att_fault_sensor_bc_closed);
    qtest_add_func("/sparc/stix/att-fault-motor-dead",
                   test_att_fault_motor_dead);
    qtest_add_func("/sparc/stix/att-fault-jammed", test_att_fault_jammed);
    qtest_add_func("/sparc/stix/att-fault-current-zero",
                   test_att_fault_current_zero);
    qtest_add_func("/sparc/stix/att-fault-current-offset",
                   test_att_fault_current_offset);
    qtest_add_func("/sparc/stix/att-fault-midmove", test_att_fault_midmove);
    qtest_add_func("/sparc/stix/att-fault-m1-dead-midmove",
                   test_att_fault_m1_dead_midmove);
    qtest_add_func("/sparc/stix/att-fault-jammed-midmove",
                   test_att_fault_jammed_midmove);
    qtest_add_func("/sparc/stix/att-fault-window-end",
                   test_att_fault_window_end);
    qtest_add_func("/sparc/stix/att-fault-reset", test_att_fault_reset);
    qtest_add_func("/sparc/stix/att-fault-trip", test_att_fault_trip);
    qtest_add_func("/sparc/stix/att-fault-reject", test_att_fault_reject);
    qtest_add_func("/sparc/stix/att-force-cmdsta", test_att_force_cmdsta);
    qtest_add_func("/sparc/stix/att-fault-verbs", test_att_fault_verbs);
    qtest_add_func("/sparc/stix/att-fault-hw-state",
                   test_att_fault_hw_state);
}

/*
 * ---- Flash damage: device model, sidecar, runtime injection
 * (flash-damage plan X-1, X-2) ----
 *
 * Kept in this one appended block behind stix_flashdmg_register(), like
 * the platform's stix_platform_register() and the attenuator's
 * stix_att_register(), so a merge with another track touches main() in
 * one line.
 */

/*
 * D-X3 refuses every sidecar block < 64 (the ASW image area), so the
 * damage tests use their own 32 MB sparse backing file: block 70 (pages
 * 4480..4543) and page 5000 sit above the common 1 MB file and inside
 * 32 MB.
 */
#define FLASH_DMG_FLASH_BYTES (32u * 1024 * 1024)
#define FLASH_DMG_BLOCK       70u     /* sidecar block: pages 4480..4543 */
#define FLASH_PAGENUM_IN_BLOCK 64     /* pages per block (the device's) */
#define FLASH_DMG_PAGE        4490u   /* a page inside FLASH_DMG_BLOCK */
#define FLASH_DMG_PROG_PAGE   5000u   /* page-scope target */
#define FLASH_DMG_SPARE_OFF   4096u   /* badBlockMarker in the spare area */

static char *stix_dmg_flash_path;   /* the damage tests' backing file */
static char *stix_dmg_map_path;     /* the sidecar passed to the device */

/*
 * Start QEMU with a 32 MB sparse backing file and a sidecar holding
 * @json; @extra carries more -global arguments (e.g. FLASH_LATENCY_ON).
 * The missing-file case of D-X3 is pinned in flash-dmg-map-parse.
 * Returns NULL when the filesystem cannot do sparse files (skipped).
 */
static QTestState *flash_dmg_qtest_new(const char *json, const char *extra)
{
    g_autofree gchar *flash = NULL;
    g_autofree gchar *map = NULL;
    g_autofree gchar *all = NULL;
    struct stat st;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("stix-flash-dmgbin-XXXXXX", &flash, NULL);
    g_assert(fd >= 0);
    g_assert(ftruncate(fd, FLASH_DMG_FLASH_BYTES) == 0);
    g_assert(fstat(fd, &st) == 0);
    close(fd);
    if ((uint64_t)st.st_blocks * 512 > 1024u * 1024u) {
        g_unlink(flash);
        g_test_skip("filesystem does not create sparse files");
        return NULL;
    }

    fd = g_file_open_tmp("stix-flash-dmgmap-XXXXXX", &map, NULL);
    g_assert(fd >= 0);
    g_assert_cmpint(write(fd, json, strlen(json)), ==,
                    (ssize_t)strlen(json));
    close(fd);

    all = g_strdup_printf("-global grlib-stixflash.flash-file=%s "
                          "-global grlib-stixflash.flash-damage-map=%s %s",
                          flash, map, extra);
    stix_dmg_flash_path = g_steal_pointer(&flash);
    stix_dmg_map_path = g_steal_pointer(&map);
    qts = qtest_initf("-M leon3_generic %s", all);
    return qts;
}

static void flash_dmg_cleanup(void)
{
    if (stix_dmg_flash_path) {
        g_unlink(stix_dmg_flash_path);
        g_clear_pointer(&stix_dmg_flash_path, g_free);
    }
    if (stix_dmg_map_path) {
        g_unlink(stix_dmg_map_path);
        g_clear_pointer(&stix_dmg_map_path, g_free);
    }
}

/* the damage-list string, or NULL when the property is unknown (RED) */
static gchar *flash_dmg_list(QTestState *qts)
{
    QDict *resp = qtest_qmp(qts,
                            "{'execute': 'qom-get',"
                            " 'arguments': {'path': %s,"
                            " 'property': 'damage-list'}}",
                            FLASH_QOM_PATH);
    QString *str = qobject_to(QString, qdict_get(resp, "return"));
    gchar *out = str != NULL ? g_strdup(qstring_get_str(str)) : NULL;

    qobject_unref(resp);
    return out;
}

/*
 * /sparc/stix/flash-dmg-erase-fail (X-1): a sidecar erase-fail on block
 * 70 makes every erase of that block take the full tbers-us and fail
 * FAILED | READY with one IRQ, erasing nothing; the file keeps the
 * programmed pattern, a program into the block still lands (the FSW's
 * marker program survives its failed marking erase), and block 71 is
 * unaffected.
 * RED before X-1: flash-damage-map is an unknown property, so QEMU
 * exits at startup and qtest_initf aborts.
 */
static void test_flash_dmg_erase_fail(void)
{
    static const char json[] =
        "{\"v\":1,\"damage\":[{\"kind\":\"erase-fail\",\"block\":70}]}";
    g_autofree uint8_t *p = g_malloc(FLASH_PAGE_BYTES);
    g_autofree uint8_t *host = g_malloc(FLASH_PAGE_BYTES);
    QTestState *qts;
    gchar *list;
    size_t i;

    for (i = 0; i < FLASH_PAGE_BYTES; i++) {
        p[i] = (uint8_t)(i ^ 0x3Cu);
    }

    qts = flash_dmg_qtest_new(json, FLASH_LATENCY_ON);
    if (!qts) {
        return;
    }

    list = flash_dmg_list(qts);
    g_assert_cmpstr(list, ==, "src=map kind=erase-fail block=70");
    g_free(list);

    /* a program into the damaged block lands (the FSW's marker nuance) */
    qtest_memwrite(qts, FLASH_RAM_A, p, FLASH_PAGE_BYTES);
    flash_issue(qts, FLASH_RAM_A, FLASH_DMG_PAGE, CMD_PAGE_WRITE);
    flash_assert_done_ok(qts);

    /* the erase takes its full latency, then fails once */
    flash_irq_clear(qts);
    flash_issue_nowait(qts, FLASH_RAM_A, FLASH_DMG_PAGE, CMD_BLOCK_ERASE);
    flash_assert_busy(qts);
    flash_step_us(qts, FLASH_TBERS_US - 1);
    flash_assert_busy(qts);
    g_assert_false(flash_irq_pending(qts));
    flash_step_us(qts, 2);
    g_assert_cmpuint(qtest_readl(qts, FLASH_CMDSTA)
                     & (FTNAND_BUSY | FTNAND_FAILED | FTNAND_READY),
                     ==, FTNAND_FAILED | FTNAND_READY);
    g_assert_true(flash_irq_pending(qts));
    flash_assert_no_more_irq(qts);

    /* nothing was erased: the file still holds the pattern */
    flash_pread_page(stix_dmg_flash_path,
                     (uint64_t)FLASH_DMG_PAGE * FLASH_PAGE_BYTES, host);
    flash_invert(host, FLASH_PAGE_BYTES);
    g_assert_cmpint(memcmp(host, p, FLASH_PAGE_BYTES), ==, 0);

    /* and the read sees it too: erase-fail does not touch reads */
    flash_ram_fill(qts, FLASH_RAM_B, 0x00);
    flash_issue(qts, FLASH_RAM_B, FLASH_DMG_PAGE, CMD_PAGE_READ);
    flash_assert_done_ok(qts);
    qtest_memread(qts, FLASH_RAM_B, host, FLASH_PAGE_BYTES);
    g_assert_cmpint(memcmp(host, p, FLASH_PAGE_BYTES), ==, 0);

    /* the neighbour block 71 erases normally */
    flash_irq_clear(qts);
    flash_issue(qts, FLASH_RAM_A,
                (FLASH_DMG_BLOCK + 1) * FLASH_PAGENUM_IN_BLOCK,
                CMD_BLOCK_ERASE);
    flash_assert_done_ok(qts);
    g_assert_cmpint(stix_qom_counter(qts, FLASH_QOM_PATH, "ops-erase"), ==, 2);

    qtest_quit(qts);
    flash_dmg_cleanup();
}

/*
 * /sparc/stix/flash-dmg-program-fail (X-1): a sidecar program-fail on
 * page 5000 makes every program of that page fail FAILED | READY after
 * the full tprog-us with one IRQ, writing nothing (the file keeps its
 * hole); the neighbouring page programs and reads back normally.
 * RED before X-1: the property is unknown, QEMU exits at startup.
 */
static void test_flash_dmg_program_fail(void)
{
    static const char json[] =
        "{\"v\":1,\"damage\":[{\"kind\":\"program-fail\","
        "\"mcm\":0,\"chip\":0,\"page\":5000}]}";
    g_autofree uint8_t *p = g_malloc(FLASH_PAGE_BYTES);
    g_autofree uint8_t *host = g_malloc(FLASH_PAGE_BYTES);
    g_autofree uint8_t *z0 = g_malloc0(FLASH_PAGE_BYTES);
    QTestState *qts;
    gchar *list;
    size_t i;

    for (i = 0; i < FLASH_PAGE_BYTES; i++) {
        p[i] = (uint8_t)(i ^ 0x5Au);
    }

    qts = flash_dmg_qtest_new(json, FLASH_LATENCY_ON);
    if (!qts) {
        return;
    }

    list = flash_dmg_list(qts);
    g_assert_cmpstr(list, ==, "src=map kind=program-fail page=5000");
    g_free(list);

    /* the program takes its full latency, then fails once */
    qtest_memwrite(qts, FLASH_RAM_A, p, FLASH_PAGE_BYTES);
    flash_irq_clear(qts);
    flash_issue_nowait(qts, FLASH_RAM_A, FLASH_DMG_PROG_PAGE,
                       CMD_PAGE_WRITE);
    flash_assert_busy(qts);
    flash_step_us(qts, FLASH_TPROG_US - 1);
    flash_assert_busy(qts);
    g_assert_false(flash_irq_pending(qts));
    flash_step_us(qts, 2);
    g_assert_cmpuint(qtest_readl(qts, FLASH_CMDSTA)
                     & (FTNAND_BUSY | FTNAND_FAILED | FTNAND_READY),
                     ==, FTNAND_FAILED | FTNAND_READY);
    g_assert_true(flash_irq_pending(qts));
    flash_assert_no_more_irq(qts);

    /* nothing was written: the erased page is still a host hole */
    flash_pread_page(stix_dmg_flash_path,
                     (uint64_t)FLASH_DMG_PROG_PAGE * FLASH_PAGE_BYTES, host);
    g_assert_cmpint(memcmp(host, z0, FLASH_PAGE_BYTES), ==, 0);

    /* the neighbouring page 5001 is unaffected */
    qtest_memwrite(qts, FLASH_RAM_A, p, FLASH_PAGE_BYTES);
    flash_issue(qts, FLASH_RAM_A, FLASH_DMG_PROG_PAGE + 1, CMD_PAGE_WRITE);
    flash_assert_done_ok(qts);
    flash_ram_fill(qts, FLASH_RAM_B, 0x00);
    flash_issue(qts, FLASH_RAM_B, FLASH_DMG_PROG_PAGE + 1, CMD_PAGE_READ);
    flash_assert_done_ok(qts);
    qtest_memread(qts, FLASH_RAM_B, host, FLASH_PAGE_BYTES);
    g_assert_cmpint(memcmp(host, p, FLASH_PAGE_BYTES), ==, 0);
    g_assert_cmpint(stix_qom_counter(qts, FLASH_QOM_PATH, "ops-prog"), ==, 2);

    qtest_quit(qts);
    flash_dmg_cleanup();
}

/*
 * /sparc/stix/flash-dmg-stuck0 (X-1, O4): a page-scope stuck bit with
 * bit "12.3" and a block-scope stuck block 71, both at value 0, written
 * with the "stuck0" alias (the O4 grammar's value-0 shorthand). Reads
 * complete OK (a stuck cell is not an error) but return the true page
 * with the stuck bits forced to 0 — also for a page programmed in the
 * same session — and the stored file keeps the true bytes (the mask is
 * read-side only). A page in neither scope reads back unmasked.
 * RED before X-1: the property is unknown, QEMU exits at startup.
 */
static void test_flash_dmg_stuck0(void)
{
    static const char json[] =
        "{\"v\":1,\"damage\":["
        "{\"kind\":\"stuck0\",\"page\":4490,\"bit\":\"12.3\"},"
        "{\"kind\":\"stuck0\",\"block\":71}]}";
    g_autofree uint8_t *p = g_malloc(FLASH_PAGE_BYTES);
    g_autofree uint8_t *r = g_malloc(FLASH_PAGE_BYTES);
    g_autofree uint8_t *host = g_malloc(FLASH_PAGE_BYTES);
    QTestState *qts;
    gchar *list;
    size_t i;

    for (i = 0; i < FLASH_PAGE_BYTES; i++) {
        p[i] = (uint8_t)(i ^ 0x96u);
    }

    qts = flash_dmg_qtest_new(json, FLASH_LATENCY_ON);
    if (!qts) {
        return;
    }

    list = flash_dmg_list(qts);
    g_assert_cmpstr(list, ==, "src=map kind=stuck value=0 page=4490 "
                              "bit=12.3\nsrc=map kind=stuck value=0 "
                              "block=71");
    g_free(list);

    /* the bit form: byte 12 bit 3 reads 0, everything else is true */
    qtest_memwrite(qts, FLASH_RAM_A, p, FLASH_PAGE_BYTES);
    flash_issue(qts, FLASH_RAM_A, FLASH_DMG_PAGE, CMD_PAGE_WRITE);
    flash_assert_done_ok(qts);          /* a program into the page is fine */
    flash_issue(qts, FLASH_RAM_B, FLASH_DMG_PAGE, CMD_PAGE_READ);
    flash_assert_done_ok(qts);          /* and the read itself is not */
    qtest_memread(qts, FLASH_RAM_B, r, FLASH_PAGE_BYTES);
    for (i = 0; i < FLASH_PAGE_BYTES; i++) {
        uint8_t want = i == 12 ? (uint8_t)(p[i] & ~(1u << 3)) : p[i];

        if (r[i] != want) {
            g_test_message("byte %zu: got 0x%02x want 0x%02x", i, r[i],
                           want);
        }
        g_assert_cmpuint(r[i], ==, want);
    }
    /* the file keeps the true bytes: the mask is read-side only */
    flash_pread_page(stix_dmg_flash_path,
                     (uint64_t)FLASH_DMG_PAGE * FLASH_PAGE_BYTES, host);
    flash_invert(host, FLASH_PAGE_BYTES);
    g_assert_cmpint(memcmp(host, p, FLASH_PAGE_BYTES), ==, 0);

    /*
     * the block form: every byte of a freshly programmed page in block
     * 71 reads with bit 0 cleared, two pages deep
     */
    qtest_memwrite(qts, FLASH_RAM_A, p, FLASH_PAGE_BYTES);
    flash_issue(qts, FLASH_RAM_A,
                (FLASH_DMG_BLOCK + 1) * FLASH_PAGENUM_IN_BLOCK,
                CMD_PAGE_WRITE);
    flash_assert_done_ok(qts);
    flash_issue(qts, FLASH_RAM_B,
                (FLASH_DMG_BLOCK + 1) * FLASH_PAGENUM_IN_BLOCK,
                CMD_PAGE_READ);
    flash_assert_done_ok(qts);
    qtest_memread(qts, FLASH_RAM_B, r, FLASH_PAGE_BYTES);
    for (i = 0; i < FLASH_PAGE_BYTES; i++) {
        g_assert_cmpuint(r[i], ==, (uint8_t)(p[i] & ~1u));
    }
    qtest_memwrite(qts, FLASH_RAM_A, p, FLASH_PAGE_BYTES);
    flash_issue(qts, FLASH_RAM_A,
                (FLASH_DMG_BLOCK + 1) * FLASH_PAGENUM_IN_BLOCK + 6,
                CMD_PAGE_WRITE);
    flash_assert_done_ok(qts);
    flash_issue(qts, FLASH_RAM_B,
                (FLASH_DMG_BLOCK + 1) * FLASH_PAGENUM_IN_BLOCK + 6,
                CMD_PAGE_READ);
    flash_assert_done_ok(qts);
    qtest_memread(qts, FLASH_RAM_B, r, FLASH_PAGE_BYTES);
    for (i = 0; i < FLASH_PAGE_BYTES; i++) {
        g_assert_cmpuint(r[i], ==, (uint8_t)(p[i] & ~1u));
    }

    /* a page in neither scope reads true */
    qtest_memwrite(qts, FLASH_RAM_A, p, FLASH_PAGE_BYTES);
    flash_issue(qts, FLASH_RAM_A, 4700, CMD_PAGE_WRITE);
    flash_assert_done_ok(qts);
    flash_issue(qts, FLASH_RAM_B, 4700, CMD_PAGE_READ);
    flash_assert_done_ok(qts);
    qtest_memread(qts, FLASH_RAM_B, r, FLASH_PAGE_BYTES);
    g_assert_cmpint(memcmp(r, p, FLASH_PAGE_BYTES), ==, 0);
    g_assert_cmpint(stix_qom_counter(qts, FLASH_QOM_PATH, "ops-read"), ==, 4);

    qtest_quit(qts);
    flash_dmg_cleanup();
}

/*
 * /sparc/stix/flash-dmg-stuck1 (X-1, O4): the mirror of
 * flash-dmg-stuck0 with the stuck value 1, written with the general
 * grammar kind "stuck" + "value": 1. An erased page (0xFF) is
 * unaffected by a stuck-1 bit (the bit already reads 1), a programmed
 * 0 on that bit reads 1, and the stored file keeps the true bytes (the
 * mask is read-side only). The block form forces bit 0 of every byte
 * to 1, two pages deep.
 * RED before O4: kind "stuck" is an unknown kind, the sidecar fails
 * the parse and QEMU exits at startup.
 */
static void test_flash_dmg_stuck1(void)
{
    static const char json[] =
        "{\"v\":1,\"damage\":["
        "{\"kind\":\"stuck\",\"value\":1,\"page\":4490,\"bit\":\"12.3\"},"
        "{\"kind\":\"stuck\",\"value\":1,\"block\":71}]}";
    g_autofree uint8_t *p = g_malloc(FLASH_PAGE_BYTES);
    g_autofree uint8_t *r = g_malloc(FLASH_PAGE_BYTES);
    g_autofree uint8_t *host = g_malloc(FLASH_PAGE_BYTES);
    QTestState *qts;
    gchar *list;
    size_t i;

    /* p[12] = 0x65: bit 3 is a programmed 0, so the stuck-1 read flips it */
    for (i = 0; i < FLASH_PAGE_BYTES; i++) {
        p[i] = (uint8_t)(i ^ 0x69u);
    }

    qts = flash_dmg_qtest_new(json, FLASH_LATENCY_ON);
    if (!qts) {
        return;
    }

    list = flash_dmg_list(qts);
    g_assert_cmpstr(list, ==, "src=map kind=stuck value=1 page=4490 "
                              "bit=12.3\nsrc=map kind=stuck value=1 "
                              "block=71");
    g_free(list);

    /* an erased page reads 0xFF with and without the stuck-1 masks */
    flash_ram_fill(qts, FLASH_RAM_B, 0x00);
    flash_issue(qts, FLASH_RAM_B, FLASH_DMG_PAGE, CMD_PAGE_READ);
    flash_assert_done_ok(qts);
    qtest_memread(qts, FLASH_RAM_B, r, FLASH_PAGE_BYTES);
    for (i = 0; i < FLASH_PAGE_BYTES; i++) {
        g_assert_cmpuint(r[i], ==, 0xffu);
    }
    flash_issue(qts, FLASH_RAM_B,
                (FLASH_DMG_BLOCK + 1) * FLASH_PAGENUM_IN_BLOCK,
                CMD_PAGE_READ);
    flash_assert_done_ok(qts);
    qtest_memread(qts, FLASH_RAM_B, r, FLASH_PAGE_BYTES);
    for (i = 0; i < FLASH_PAGE_BYTES; i++) {
        g_assert_cmpuint(r[i], ==, 0xffu);
    }

    /* the bit form: byte 12 bit 3 reads 1, everything else is true */
    qtest_memwrite(qts, FLASH_RAM_A, p, FLASH_PAGE_BYTES);
    flash_issue(qts, FLASH_RAM_A, FLASH_DMG_PAGE, CMD_PAGE_WRITE);
    flash_assert_done_ok(qts);          /* a program into the page is fine */
    flash_issue(qts, FLASH_RAM_B, FLASH_DMG_PAGE, CMD_PAGE_READ);
    flash_assert_done_ok(qts);
    qtest_memread(qts, FLASH_RAM_B, r, FLASH_PAGE_BYTES);
    for (i = 0; i < FLASH_PAGE_BYTES; i++) {
        uint8_t want = i == 12 ? (uint8_t)(p[i] | (1u << 3)) : p[i];

        if (r[i] != want) {
            g_test_message("byte %zu: got 0x%02x want 0x%02x", i, r[i],
                           want);
        }
        g_assert_cmpuint(r[i], ==, want);
    }
    /* the file keeps the true bytes: the mask is read-side only */
    flash_pread_page(stix_dmg_flash_path,
                     (uint64_t)FLASH_DMG_PAGE * FLASH_PAGE_BYTES, host);
    flash_invert(host, FLASH_PAGE_BYTES);
    g_assert_cmpint(memcmp(host, p, FLASH_PAGE_BYTES), ==, 0);

    /*
     * the block form: every byte of a freshly programmed page in block
     * 71 reads with bit 0 set, two pages deep
     */
    qtest_memwrite(qts, FLASH_RAM_A, p, FLASH_PAGE_BYTES);
    flash_issue(qts, FLASH_RAM_A,
                (FLASH_DMG_BLOCK + 1) * FLASH_PAGENUM_IN_BLOCK,
                CMD_PAGE_WRITE);
    flash_assert_done_ok(qts);
    flash_issue(qts, FLASH_RAM_B,
                (FLASH_DMG_BLOCK + 1) * FLASH_PAGENUM_IN_BLOCK,
                CMD_PAGE_READ);
    flash_assert_done_ok(qts);
    qtest_memread(qts, FLASH_RAM_B, r, FLASH_PAGE_BYTES);
    for (i = 0; i < FLASH_PAGE_BYTES; i++) {
        g_assert_cmpuint(r[i], ==, (uint8_t)(p[i] | 1u));
    }
    qtest_memwrite(qts, FLASH_RAM_A, p, FLASH_PAGE_BYTES);
    flash_issue(qts, FLASH_RAM_A,
                (FLASH_DMG_BLOCK + 1) * FLASH_PAGENUM_IN_BLOCK + 6,
                CMD_PAGE_WRITE);
    flash_assert_done_ok(qts);
    flash_issue(qts, FLASH_RAM_B,
                (FLASH_DMG_BLOCK + 1) * FLASH_PAGENUM_IN_BLOCK + 6,
                CMD_PAGE_READ);
    flash_assert_done_ok(qts);
    qtest_memread(qts, FLASH_RAM_B, r, FLASH_PAGE_BYTES);
    for (i = 0; i < FLASH_PAGE_BYTES; i++) {
        g_assert_cmpuint(r[i], ==, (uint8_t)(p[i] | 1u));
    }

    /* a page in neither scope reads true */
    qtest_memwrite(qts, FLASH_RAM_A, p, FLASH_PAGE_BYTES);
    flash_issue(qts, FLASH_RAM_A, 4700, CMD_PAGE_WRITE);
    flash_assert_done_ok(qts);
    flash_issue(qts, FLASH_RAM_B, 4700, CMD_PAGE_READ);
    flash_assert_done_ok(qts);
    qtest_memread(qts, FLASH_RAM_B, r, FLASH_PAGE_BYTES);
    g_assert_cmpint(memcmp(r, p, FLASH_PAGE_BYTES), ==, 0);
    g_assert_cmpint(stix_qom_counter(qts, FLASH_QOM_PATH, "ops-read"), ==, 6);

    qtest_quit(qts);
    flash_dmg_cleanup();
}

/*
 * /sparc/stix/flash-dmg-stuck-multi (X-1, review finding 3): two stuck
 * entries on one page of block 70, bits "12.3" and "12.4", both value
 * 1 — every matching entry applies, so both bits read set. The mask
 * used to apply only the first matching entry (silently, while
 * damage-list already showed both rows).
 * RED before the fix: bit 4 reads true and byte 12 is 0x6d, not 0x7d.
 */
static void test_flash_dmg_stuck_multi(void)
{
    static const char json[] =
        "{\"v\":1,\"damage\":["
        "{\"kind\":\"stuck\",\"value\":1,\"page\":4490,\"bit\":\"12.3\"},"
        "{\"kind\":\"stuck\",\"value\":1,\"page\":4490,\"bit\":\"12.4\"}]}";
    g_autofree uint8_t *p = g_malloc(FLASH_PAGE_BYTES);
    g_autofree uint8_t *r = g_malloc(FLASH_PAGE_BYTES);
    g_autofree uint8_t *host = g_malloc(FLASH_PAGE_BYTES);
    QTestState *qts;
    gchar *list;
    size_t i;

    /*
     * p[12] = 0x65: bits 3 and 4 are programmed 0, so both stuck-1
     * bits must flip to 1 on the read
     */
    for (i = 0; i < FLASH_PAGE_BYTES; i++) {
        p[i] = (uint8_t)(i ^ 0x69u);
    }

    qts = flash_dmg_qtest_new(json, FLASH_LATENCY_ON);
    if (!qts) {
        return;
    }

    list = flash_dmg_list(qts);
    g_assert_cmpstr(list, ==, "src=map kind=stuck value=1 page=4490 "
                              "bit=12.3\nsrc=map kind=stuck value=1 "
                              "page=4490 bit=12.4");
    g_free(list);

    /* the bit form: byte 12 reads with bits 3 and 4 set, the rest true */
    qtest_memwrite(qts, FLASH_RAM_A, p, FLASH_PAGE_BYTES);
    flash_issue(qts, FLASH_RAM_A, FLASH_DMG_PAGE, CMD_PAGE_WRITE);
    flash_assert_done_ok(qts);          /* a program into the page is fine */
    flash_issue(qts, FLASH_RAM_B, FLASH_DMG_PAGE, CMD_PAGE_READ);
    flash_assert_done_ok(qts);
    qtest_memread(qts, FLASH_RAM_B, r, FLASH_PAGE_BYTES);
    for (i = 0; i < FLASH_PAGE_BYTES; i++) {
        uint8_t want = i == 12 ? (uint8_t)(p[i] | (1u << 3) | (1u << 4))
                               : p[i];

        if (r[i] != want) {
            g_test_message("byte %zu: got 0x%02x want 0x%02x", i, r[i],
                           want);
        }
        g_assert_cmpuint(r[i], ==, want);
    }
    /* the file keeps the true bytes: the mask is read-side only */
    flash_pread_page(stix_dmg_flash_path,
                     (uint64_t)FLASH_DMG_PAGE * FLASH_PAGE_BYTES, host);
    flash_invert(host, FLASH_PAGE_BYTES);
    g_assert_cmpint(memcmp(host, p, FLASH_PAGE_BYTES), ==, 0);

    qtest_quit(qts);
    flash_dmg_cleanup();
}

/*
 * /sparc/stix/flash-dmg-marker-plain (X-1, correction 4 / review
 * finding 4): the tool writes badBlockMarker = 0 as a data byte, so the
 * guest must see 0x00 at spare offset 4096 in both storage modes. The
 * qtest hand-writes the marker bytes (the tool only exists from X-3):
 * in plain mode 0x00 on disk reads 0x00, in inverted mode 0xFF on disk
 * reads 0x00.
 * RED if the inversion rule breaks: in inverted mode the hand-written
 * 0xFF would read 0xFF, in plain mode the 0xFF fill would read 0x00.
 */
static void test_flash_dmg_marker_plain(void)
{
    /* plain mode: a 0xFF page with the marker byte 0x00 on disk */
    {
        g_autofree gchar *path = NULL;
        g_autofree gchar *extra = NULL;
        g_autofree uint8_t *ff = g_malloc(FLASH_PAGE_BYTES);
        g_autofree uint8_t *host = g_malloc(FLASH_PAGE_BYTES);
        const uint8_t marker = 0x00;
        off_t off = (off_t)5 * FLASH_PAGE_BYTES;
        int fd = g_file_open_tmp("stix-flash-marker-XXXXXX", &path, NULL);
        QTestState *qts;

        g_assert(fd >= 0);
        g_assert(ftruncate(fd, 1024 * 1024) == 0);
        memset(ff, 0xFF, FLASH_PAGE_BYTES);
        g_assert_cmpint(pwrite(fd, ff, FLASH_PAGE_BYTES, off), ==,
                        (ssize_t)FLASH_PAGE_BYTES);
        g_assert_cmpint(pwrite(fd, &marker, 1, off + FLASH_DMG_SPARE_OFF),
                        ==, 1);
        close(fd);
        extra = g_strdup_printf("-global grlib-stixflash.flash-file=%s "
                                "-global grlib-stixflash.inverted-storage=off",
                                path);
        qts = qtest_initf("-M leon3_generic %s", extra);
        flash_issue(qts, FLASH_RAM_A, 5, CMD_PAGE_READ);
        qtest_memread(qts, FLASH_RAM_A, host, FLASH_PAGE_BYTES);
        g_assert_cmpuint(host[FLASH_DMG_SPARE_OFF - 1], ==, 0xFF);
        g_assert_cmpuint(host[FLASH_DMG_SPARE_OFF], ==, 0x00);
        g_assert_cmpuint(host[FLASH_DMG_SPARE_OFF + 1], ==, 0xFF);
        qtest_quit(qts);
        g_unlink(path);
    }

    /* inverted mode: the marker byte sits on disk as 0xFF */
    {
        g_autofree gchar *path = NULL;
        g_autofree uint8_t *host = g_malloc(FLASH_PAGE_BYTES);
        const uint8_t marker = 0xFF;
        off_t off = (off_t)5 * FLASH_PAGE_BYTES;
        int fd = g_file_open_tmp("stix-flash-marker-XXXXXX", &path, NULL);
        QTestState *qts;

        g_assert(fd >= 0);
        g_assert(ftruncate(fd, 1024 * 1024) == 0);  /* holes = guest 0xFF */
        g_assert_cmpint(pwrite(fd, &marker, 1, off + FLASH_DMG_SPARE_OFF),
                        ==, 1);
        close(fd);
        qts = qtest_initf("-M leon3_generic "
                          "-global grlib-stixflash.flash-file=%s", path);
        flash_issue(qts, FLASH_RAM_A, 5, CMD_PAGE_READ);
        qtest_memread(qts, FLASH_RAM_A, host, FLASH_PAGE_BYTES);
        g_assert_cmpuint(host[FLASH_DMG_SPARE_OFF - 1], ==, 0xFF);
        g_assert_cmpuint(host[FLASH_DMG_SPARE_OFF], ==, 0x00);
        g_assert_cmpuint(host[FLASH_DMG_SPARE_OFF + 1], ==, 0xFF);
        qtest_quit(qts);
        g_unlink(path);
    }
}

/*
 * /sparc/stix/flash-dmg-quiet (X-1, D-X6): with no map configured the
 * device is byte-identical to the pre-damage model: damage-list is
 * empty, a read/program/erase round completes with the usual statuses,
 * counters and one IRQ per op, and no stixflash_damage_* trace line
 * fires (the golden capture runs -trace 'stix*').
 * RED before X-1: damage-list is an unknown property, the qom-get
 * errors and the test aborts.
 */
static void test_flash_dmg_quiet(void)
{
    g_autofree gchar *log = NULL;
    g_autofree gchar *extra = NULL;
    g_autofree gchar *text = NULL;
    g_autofree uint8_t *p = g_malloc(FLASH_PAGE_BYTES);
    QTestState *qts;
    gchar *list;
    size_t i;
    int fd;

    for (i = 0; i < FLASH_PAGE_BYTES; i++) {
        p[i] = (uint8_t)(i ^ 0xAu);
    }
    fd = g_file_open_tmp("stix-flash-dmgquiet-XXXXXX", &log, NULL);
    g_assert(fd >= 0);
    close(fd);
    extra = g_strdup_printf("%s -trace stixflash_damage_load "
                            "-trace stixflash_damage_hit "
                            "-trace stixflash_op_done -D %s",
                            FLASH_LATENCY_ON, log);
    qts = stix_qtest_new_extra(extra);

    list = flash_dmg_list(qts);
    g_assert_nonnull(list);
    g_assert_cmpstr(list, ==, "");
    g_free(list);

    /* the usual round: one IRQ each, normal statuses and counters */
    flash_ram_fill(qts, FLASH_RAM_A, 0x00);
    flash_irq_clear(qts);
    flash_issue(qts, FLASH_RAM_A, 6, CMD_PAGE_READ);
    flash_assert_done_ok(qts);
    g_assert_cmphex(qtest_readl(qts, FLASH_RAM_A), ==, 0xffffffffu);
    qtest_memwrite(qts, FLASH_RAM_A, p, FLASH_PAGE_BYTES);
    flash_issue(qts, FLASH_RAM_A, 6, CMD_PAGE_WRITE);
    flash_assert_done_ok(qts);
    flash_issue(qts, FLASH_RAM_A, 6, CMD_BLOCK_ERASE);
    flash_assert_done_ok(qts);
    g_assert_cmpint(stix_qom_counter(qts, FLASH_QOM_PATH, "ops-read"), ==, 1);
    g_assert_cmpint(stix_qom_counter(qts, FLASH_QOM_PATH, "ops-prog"), ==, 1);
    g_assert_cmpint(stix_qom_counter(qts, FLASH_QOM_PATH, "ops-erase"), ==, 1);

    qtest_quit(qts);
    stix_qtest_cleanup();
    g_assert(g_file_get_contents(log, &text, NULL, NULL));
    /* positive control: stixflash_op_done is enabled, the ops landed */
    g_assert_nonnull(strstr(text, "stixflash_op_done"));
    g_assert_null(strstr(text, "stixflash_damage"));
    unlink(log);
}

#define DMG_TEST_MAP_ENV "STIX_TEST_DMG_MAP"

/* g_test_trap_subprocess child: start QEMU with the sidecar, then quit. */
static void test_flash_dmg_map_error_subprocess(void)
{
    const char *map = g_getenv(DMG_TEST_MAP_ENV);
    const char *flash = g_getenv(TMTC_TEST_FLASH_ENV);
    QTestState *qts;

    g_assert_nonnull(map);
    g_assert_nonnull(flash);
    /* qtest_init aborts: QEMU realize refuses the malformed sidecar */
    qts = qtest_initf("-M leon3_generic "
                      "-global grlib-stixflash.flash-file=%s "
                      "-global grlib-stixflash.flash-damage-map=%s",
                      flash, map);
    qtest_quit(qts);
}

/*
 * /sparc/stix/flash-dmg-map-parse (X-1, D-X3): a malformed sidecar
 * fails realize, naming the path and the reason; a missing file is
 * silence. RED before X-1: the property is unknown, so every child dies
 * with a different message and the missing-file case cannot boot.
 */
static void test_flash_dmg_map_parse(void)
{
    static const struct {
        const char *json;
        const char *pat;
    } cases[] = {
        { "this is not json", "*JSON parse error*" },
        { "[1,2]", "*top level is not a JSON object*" },
        { "{\"v\":2,\"damage\":[]}", "*unsupported version 2*" },
        { "{\"v\":1,\"damage\":[{\"kind\":\"warp\",\"block\":70}]}",
          "*'kind' must be*got 'warp'*" },
        { "{\"v\":1,\"damage\":[{\"kind\":\"erase-fail\",\"block\":10}]}",
          "*block 10 is in the reserved ASW*" },
        { "{\"v\":1,\"damage\":[{\"kind\":\"program-fail\",\"block\":70}]}",
          "*program-fail is a page kind*" },
        { "{\"v\":1,\"damage\":[{\"kind\":\"stuck0\",\"page\":4490}]}",
          "*needs a bit as 'byte.bit'*" },
        { "{\"v\":1,\"damage\":[{\"kind\":\"stuck0\",\"page\":4490,"
          "\"bit\":\"12.9\"}]}", "*bit < 8, got '12.9'*" },
        { "{\"v\":1,\"damage\":[{\"kind\":\"stuck\",\"page\":4490}]}",
          "*'value' must be a JSON integer in [0, 1]*" },
        { "{\"v\":1,\"damage\":[{\"kind\":\"stuck\",\"page\":4490,"
          "\"value\":2,\"bit\":\"12.3\"}]}",
          "*'value' must be a JSON integer in [0, 1]*" },
        { "{\"v\":1,\"damage\":[{\"kind\":\"stuck0\",\"page\":4490,"
          "\"value\":0,\"bit\":\"12.3\"}]}",
          "*'value' with the 'stuck0' alias*" },
        { "{\"v\":1,\"damage\":[{\"kind\":\"erase-fail\",\"block\":70,"
          "\"value\":1}]}", "*'value' is only valid with kind \"stuck\"*" },
        { "{\"v\":1,\"damage\":[{\"kind\":\"erase-fail\",\"block\":70,"
          "\"pages\":3}]}", "*unknown key 'pages'*" },
    };
    g_autofree gchar *path = NULL;
    g_autofree gchar *map = NULL;
    g_autofree gchar *pat = NULL;
    g_autofree gchar *sub = stix_test_path("flash-dmg-map-parse/subprocess");
    size_t i;
    int fd;

    fd = g_file_open_tmp("stix-flash-XXXXXX", &path, NULL);
    g_assert(fd >= 0);
    g_assert(ftruncate(fd, 1024 * 1024) == 0);
    close(fd);
    g_setenv(TMTC_TEST_FLASH_ENV, path, TRUE);
    g_setenv(DMG_TEST_MAP_ENV, "", TRUE);

    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        fd = g_file_open_tmp("stix-flash-dmg-XXXXXX", &map, NULL);
        g_assert(fd >= 0);
        g_assert_cmpint(write(fd, cases[i].json, strlen(cases[i].json)),
                        ==, (ssize_t)strlen(cases[i].json));
        close(fd);
        g_setenv(DMG_TEST_MAP_ENV, map, TRUE);
        pat = g_strdup_printf("*flash-damage-map %s: *%s*", map,
                              cases[i].pat);
        g_test_trap_subprocess(sub, 30 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr(pat);
        g_unlink(map);
        g_clear_pointer(&map, g_free);
        g_clear_pointer(&pat, g_free);
    }
    g_unlink(path);

    /* a missing sidecar file is silence: QEMU boots, the list is empty */
    {
        g_autofree gchar *missing = g_strdup_printf("%s.does-not-exist",
                                                    path);
        g_autofree gchar *all = g_strdup_printf(
            "-global grlib-stixflash.flash-damage-map=%s", missing);
        QTestState *qts = stix_qtest_new_extra(all);
        gchar *list = flash_dmg_list(qts);

        g_assert_nonnull(list);
        g_assert_cmpstr(list, ==, "");
        g_free(list);
        qtest_quit(qts);
        stix_qtest_cleanup();
    }
}

/*
 * qom-set one of the X-2 verbs ("damage-add"/"damage-remove"); false
 * when QEMU refused it (D-Ag: a rejected set changed nothing). %s
 * inserts the value as a JSON string, so the spec's quotes are escaped
 * by libqtest.
 */
static bool flash_dmg_set(QTestState *qts, const char *prop,
                          const char *value)
{
    QDict *resp = qtest_qmp(qts,
                            "{'execute': 'qom-set',"
                            " 'arguments': {'path': %s, 'property': %s,"
                            " 'value': %s}}",
                            FLASH_QOM_PATH, prop, value);
    bool ok = qdict_haskey(resp, "return");

    if (!ok) {
        QDict *err = qdict_get_qdict(resp, "error");

        g_test_message("%s refused: %s", prop,
                       err != NULL ? qdict_get_try_str(err, "desc") : "?");
    }
    qobject_unref(resp);
    return ok;
}

/*
 * /sparc/stix/flash-dmg-runtime-add (X-2): damage-add injects one
 * runtime entry with the sidecar entry grammar (the same parser); the
 * erase of the injected block takes its full tbers-us and fails
 * FAILED | READY with one IRQ, erasing nothing, and one
 * stixflash_damage_hit trace line (the same hit points as the
 * sidecar); damage-remove of the identical spec drops the entry and
 * the next erase succeeds. These verbs are platform fault verbs, not
 * qtest-only state.
 * RED before X-2: damage-add/damage-remove are unknown properties, the
 * first qom-set errors and the assert aborts.
 */
static void test_flash_dmg_runtime_add(void)
{
    static const char json[] = "{\"v\":1,\"damage\":[]}";
    static const char spec[] = "{\"kind\":\"erase-fail\",\"block\":70}";
    g_autofree uint8_t *p = g_malloc(FLASH_PAGE_BYTES);
    g_autofree uint8_t *host = g_malloc(FLASH_PAGE_BYTES);
    g_autofree gchar *log = NULL;
    g_autofree gchar *extra = NULL;
    g_autofree gchar *text = NULL;
    QTestState *qts;
    gchar *list, *cur;
    size_t i;
    int fd, hits;

    for (i = 0; i < FLASH_PAGE_BYTES; i++) {
        p[i] = (uint8_t)(i ^ 0x3Cu);
    }
    fd = g_file_open_tmp("stix-flash-dmgrun-XXXXXX", &log, NULL);
    g_assert(fd >= 0);
    close(fd);
    extra = g_strdup_printf("%s -trace stixflash_damage_hit "
                            "-trace stixflash_op_done -D %s",
                            FLASH_LATENCY_ON, log);

    qts = flash_dmg_qtest_new(json, extra);

    /* inject: the row appears with src=runtime */
    g_assert_true(flash_dmg_set(qts, "damage-add", spec));
    list = flash_dmg_list(qts);
    g_assert_cmpstr(list, ==, "src=runtime kind=erase-fail block=70");
    g_free(list);

    /* the erase takes its full latency, then fails once */
    qtest_memwrite(qts, FLASH_RAM_A, p, FLASH_PAGE_BYTES);
    flash_issue(qts, FLASH_RAM_A, FLASH_DMG_PAGE, CMD_PAGE_WRITE);
    flash_assert_done_ok(qts);
    flash_irq_clear(qts);
    flash_issue_nowait(qts, FLASH_RAM_A, FLASH_DMG_PAGE, CMD_BLOCK_ERASE);
    flash_assert_busy(qts);
    flash_step_us(qts, FLASH_TBERS_US - 1);
    flash_assert_busy(qts);
    g_assert_false(flash_irq_pending(qts));
    flash_step_us(qts, 2);
    g_assert_cmpuint(qtest_readl(qts, FLASH_CMDSTA)
                     & (FTNAND_BUSY | FTNAND_FAILED | FTNAND_READY),
                     ==, FTNAND_FAILED | FTNAND_READY);
    g_assert_true(flash_irq_pending(qts));
    flash_assert_no_more_irq(qts);

    /* nothing was erased: the file still holds the pattern */
    flash_pread_page(stix_dmg_flash_path,
                     (uint64_t)FLASH_DMG_PAGE * FLASH_PAGE_BYTES, host);
    flash_invert(host, FLASH_PAGE_BYTES);
    g_assert_cmpint(memcmp(host, p, FLASH_PAGE_BYTES), ==, 0);

    /* remove the identical spec: the row goes, the erase lands */
    g_assert_true(flash_dmg_set(qts, "damage-remove", spec));
    list = flash_dmg_list(qts);
    g_assert_cmpstr(list, ==, "");
    g_free(list);
    flash_irq_clear(qts);
    flash_issue(qts, FLASH_RAM_A, FLASH_DMG_PAGE, CMD_BLOCK_ERASE);
    flash_assert_done_ok(qts);
    g_assert_cmpint(stix_qom_counter(qts, FLASH_QOM_PATH, "ops-erase"),
                    ==, 2);

    qtest_quit(qts);
    flash_dmg_cleanup();

    /*
     * exactly one damage hit (the failed runtime erase), none after
     * the remove; positive control: the ops are in the log at all
     */
    g_assert(g_file_get_contents(log, &text, NULL, NULL));
    g_assert_nonnull(strstr(text, "stixflash_op_done"));
    hits = 0;
    for (cur = text; (cur = strstr(cur, "stixflash_damage_hit")) != NULL;
         cur++) {
        hits++;
    }
    g_assert_cmpint(hits, ==, 1);
    unlink(log);
}

/*
 * /sparc/stix/flash-dmg-reset (X-2, D-X1/D-F4): a system_reset keeps
 * the runtime entries (they model broken cells), and the pending-op
 * rules of D-F4 still hold: a reset during an erase of the undamaged
 * block 71 drops the op (cmdSta 0, no completion, no IRQ -- an aborted
 * op can never take a damage hit), and after the reset the entry still
 * fails block 70 with the full latency and one IRQ.
 * RED before X-2: damage-add is an unknown property, the qom-set
 * errors and the assert aborts.
 */
static void test_flash_dmg_reset(void)
{
    static const char json[] = "{\"v\":1,\"damage\":[]}";
    static const char spec[] = "{\"kind\":\"erase-fail\",\"block\":70}";
    QTestState *qts;
    gchar *list;

    qts = flash_dmg_qtest_new(json, FLASH_LATENCY_ON);

    g_assert_true(flash_dmg_set(qts, "damage-add", spec));
    list = flash_dmg_list(qts);
    g_assert_cmpstr(list, ==, "src=runtime kind=erase-fail block=70");
    g_free(list);

    /* a system_reset while an erase of block 71 is in flight: D-F4 */
    flash_irq_clear(qts);
    flash_issue_nowait(qts, FLASH_RAM_A, 71 * FLASH_PAGENUM_IN_BLOCK,
                       CMD_BLOCK_ERASE);
    flash_assert_busy(qts);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, FLASH_CMDSTA), ==, 0);
    flash_step_us(qts, 10 * 1000);
    g_assert_false(flash_irq_pending(qts));

    /* the entry survived the reset ... */
    list = flash_dmg_list(qts);
    g_assert_cmpstr(list, ==, "src=runtime kind=erase-fail block=70");
    g_free(list);

    /* ... and still fails the block, with the full latency, one IRQ */
    flash_irq_clear(qts);
    flash_issue_nowait(qts, FLASH_RAM_A, FLASH_DMG_PAGE, CMD_BLOCK_ERASE);
    flash_assert_busy(qts);
    flash_step_us(qts, FLASH_TBERS_US - 1);
    flash_assert_busy(qts);
    g_assert_false(flash_irq_pending(qts));
    flash_step_us(qts, 2);
    g_assert_cmpuint(qtest_readl(qts, FLASH_CMDSTA)
                     & (FTNAND_BUSY | FTNAND_FAILED | FTNAND_READY),
                     ==, FTNAND_FAILED | FTNAND_READY);
    g_assert_true(flash_irq_pending(qts));
    flash_assert_no_more_irq(qts);

    qtest_quit(qts);
    flash_dmg_cleanup();
}

/*
 * /sparc/stix/flash-dmg-reject (X-2, D-Ag): a rejected set errors and
 * changes nothing. Rejected: an unknown kind, a block in the reserved
 * ASW image area (allow-reserved off, the default), a spec that does
 * not parse, an unknown key, a duplicate of an active spec, a remove
 * of a spec no runtime entry has (including the sidecar's own spec:
 * the map is boot configuration, not removable). damage-list is
 * unchanged after every rejection. "all" removes the runtime rows,
 * leaves the sidecar's, and is idempotent. With
 * allow-reserved=on the reserved block is accepted, and removable.
 * RED before X-2: the verbs are unknown properties; the first add
 * errors and the assert aborts.
 */
static void test_flash_dmg_reject(void)
{
    static const char json[] =
        "{\"v\":1,\"damage\":[{\"kind\":\"stuck\",\"value\":0,"
        "\"page\":4490,\"bit\":\"12.3\"}]}";
    static const char spec[] = "{\"kind\":\"erase-fail\",\"block\":70}";
    static const char *const rejects[] = {
        "{\"kind\":\"warp\",\"block\":70}",         /* unknown kind */
        "{\"kind\":\"erase-fail\",\"block\":10}",   /* reserved, < 64 */
        "{\"kind\":\"erase-fail\"}",                /* no block/page */
        "not json at all",                          /* does not parse */
        "{\"kind\":\"erase-fail\",\"pages\":70}",   /* unknown key */
        "{\"kind\":\"erase-fail\",\"block\":70}",   /* duplicate */
    };
    const char *after_all = "src=map kind=stuck value=0 page=4490 bit=12.3";
    g_autofree gchar *extra = NULL;
    QTestState *qts;
    gchar *list, *snapshot;
    size_t i;

    qts = flash_dmg_qtest_new(json, FLASH_LATENCY_ON);

    g_assert_true(flash_dmg_set(qts, "damage-add", spec));
    snapshot = flash_dmg_list(qts);
    g_assert_cmpstr(snapshot, ==,
                    "src=map kind=stuck value=0 page=4490 bit=12.3\n"
                    "src=runtime kind=erase-fail block=70");

    /* every rejected damage-add leaves the list exactly as it was */
    for (i = 0; i < G_N_ELEMENTS(rejects); i++) {
        g_assert_false(flash_dmg_set(qts, "damage-add", rejects[i]));
        list = flash_dmg_list(qts);
        g_assert_cmpstr(list, ==, snapshot);
        g_free(list);
    }

    /*
     * remove of a spec no runtime entry has: the sidecar's own spec
     * counts as unknown (remove only reaches runtime entries)
     */
    g_assert_false(flash_dmg_set(qts, "damage-remove",
                                 "{\"kind\":\"erase-fail\",\"block\":71}"));
    list = flash_dmg_list(qts);
    g_assert_cmpstr(list, ==, snapshot);
    g_free(list);
    g_assert_false(flash_dmg_set(qts, "damage-remove",
                                 "{\"kind\":\"stuck\",\"value\":0,"
                                 "\"page\":4490,\"bit\":\"12.3\"}"));
    list = flash_dmg_list(qts);
    g_assert_cmpstr(list, ==, snapshot);
    g_free(list);

    /*
     * "all": the runtime row goes, the sidecar row stays, and a
     * second "all" is idempotent (succeeds, changes nothing)
     */
    g_assert_true(flash_dmg_set(qts, "damage-remove", "all"));
    list = flash_dmg_list(qts);
    g_assert_cmpstr(list, ==, after_all);
    g_free(list);
    g_assert_true(flash_dmg_set(qts, "damage-remove", "all"));
    list = flash_dmg_list(qts);
    g_assert_cmpstr(list, ==, after_all);
    g_free(list);
    g_free(snapshot);

    qtest_quit(qts);
    flash_dmg_cleanup();

    /* allow-reserved=on lifts the reserved rule for damage-add only */
    extra = g_strdup_printf("%s -global grlib-stixflash.allow-reserved=on",
                            FLASH_LATENCY_ON);
    qts = flash_dmg_qtest_new(json, extra);
    g_assert_true(flash_dmg_set(qts, "damage-add",
                                "{\"kind\":\"erase-fail\",\"block\":10}"));
    list = flash_dmg_list(qts);
    g_assert_cmpstr(list, ==,
                    "src=map kind=stuck value=0 page=4490 bit=12.3\n"
                    "src=runtime kind=erase-fail block=10");
    g_free(list);
    g_assert_true(flash_dmg_set(qts, "damage-remove", "all"));
    list = flash_dmg_list(qts);
    g_assert_cmpstr(list, ==, after_all);
    g_free(list);
    qtest_quit(qts);
    flash_dmg_cleanup();
}

/* X-1: one registration call from main(), the track's merge surface. */
static void stix_flashdmg_register(void)
{
    qtest_add_func("/sparc/stix/flash-dmg-erase-fail",
                   test_flash_dmg_erase_fail);
    qtest_add_func("/sparc/stix/flash-dmg-program-fail",
                   test_flash_dmg_program_fail);
    qtest_add_func("/sparc/stix/flash-dmg-stuck0", test_flash_dmg_stuck0);
    qtest_add_func("/sparc/stix/flash-dmg-stuck1", test_flash_dmg_stuck1);
    qtest_add_func("/sparc/stix/flash-dmg-stuck-multi",
                   test_flash_dmg_stuck_multi);
    qtest_add_func("/sparc/stix/flash-dmg-marker-plain",
                   test_flash_dmg_marker_plain);
    qtest_add_func("/sparc/stix/flash-dmg-quiet", test_flash_dmg_quiet);
    qtest_add_func("/sparc/stix/flash-dmg-map-parse/subprocess",
                   test_flash_dmg_map_error_subprocess);
    qtest_add_func("/sparc/stix/flash-dmg-map-parse",
                   test_flash_dmg_map_parse);
    qtest_add_func("/sparc/stix/flash-dmg-runtime-add",
                   test_flash_dmg_runtime_add);
    qtest_add_func("/sparc/stix/flash-dmg-reset", test_flash_dmg_reset);
    qtest_add_func("/sparc/stix/flash-dmg-reject", test_flash_dmg_reject);
}

/* ---- EDAC register semantics and reset state (memory-upset plan M-1) ---- */

#define EDAC_SDRAM_EDAC  0x80000C04u  /* Sdram:Edac (FTSDCTRL16) */
#define EDAC_AHBSTAT     0x80000F00u  /* the failing-address word (WDT.c) */
#define EDAC_CACHECMDSTA 0x80000550u  /* StixCtrl:CacheCmdSta */
#define EDAC_ROTBCMDSTA  0x80000554u
#define EDAC_ROTBCONFIG  0x80000558u
#define EDAC_EDAC_PATH   "/machine/stixctrl/edac"

/* Sdram:Edac fields (StixIdpu.h:539-562, ICD txt:3243-3253) */
#define SDRAM_EDAC_DEDETECTED (1u << 31)
#define SDRAM_EDAC_EDACRST    (1u << 13)
#define SDRAM_EDAC_EDACON     (1u << 12)
#define SDRAM_EDAC_SCRUB(x)   ((x) << 8)
#define SDRAM_EDAC_SEC(x)     (x)

/* CacheCmdSta fields (StixIdpu.h:742-771, ICD txt:2114-2128) */
#define CACHE_DEDETECTED (1u << 18)
#define CACHE_EDACRESET  (1u << 17)
#define CACHE_EDACON     (1u << 16)
#define CACHE_SEC(x)     ((x) << 8)
#define CACHE_SCRUB(x)   ((x) << 4)

/* RotbCmdSta / RotbConfig fields (StixIdpu.h:1188-1232) */
#define ROTB_DEDETECTED (1u << 4)
#define ROTB_EDACRESET  (1u << 2)
#define ROTB_SEC(x)     ((x) << 24)
#define ROTBCFG_EDACON  (1u << 12)

/*
 * qom-set of a string property; false when refused (e.g. a bad
 * edac-at-boot value)
 */
static bool stix_qom_set_str(QTestState *qts, const char *path,
                             const char *prop, const char *value)
{
    QDict *resp = qtest_qmp(qts,
                            "{'execute': 'qom-set',"
                            " 'arguments': {'path': %s, 'property': %s,"
                            " 'value': %s}}",
                            path, prop, value);
    bool ok = qdict_haskey(resp, "return");

    if (!ok) {
        QDict *err = qdict_get_qdict(resp, "error");

        g_test_message("qom-set %s.%s refused: %s", path, prop,
                       err != NULL ? qdict_get_try_str(err, "desc") : "?");
    }
    qobject_unref(resp);
    return ok;
}

/*
 * /sparc/stix/edac-reg-reset (M-1): the composed fields read 0 at reset
 * and cacheCmdSta carries its ICD reset value, EdacOn = 1
 * (STIX-ICD-0026 txt:2119). RED on the pre-M-1 binary: cacheCmdSta was
 * plain storage and read 0 (the reset-value fix is the commit's declared
 * boot-golden change, the boot print's CACHE line).
 */
static void test_edac_reg_reset(void)
{
    QTestState *qts = stix_qtest_new();

    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_EDAC), ==, 0);
    g_assert_cmphex(qtest_readl(qts, EDAC_ROTBCMDSTA), ==, 0);
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA), ==, CACHE_EDACON);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/edac-reg-readonly (M-1): writes to the composed
 * SECCOUNT/DEDETECTED fields are dropped, and the EDACRESET strobe never
 * lands in the stored word. RED on the pre-M-1 binary: 0xC04 and 0x550
 * were plain storage and the fields came back, and 0x54 kept the strobe
 * bit (read 0x4).
 */
static void test_edac_reg_readonly(void)
{
    QTestState *qts = stix_qtest_new();

    /* Sdram:Edac: SECCOUNT 7:0 and DEDETECTED 31 do not stick */
    qtest_writel(qts, EDAC_SDRAM_EDAC, SDRAM_EDAC_DEDETECTED | 0xFFu);
    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_EDAC), ==, 0);
    /* the stored EDACON/SCRUBRATE fields do */
    qtest_writel(qts, EDAC_SDRAM_EDAC,
                 SDRAM_EDAC_EDACON | SDRAM_EDAC_SCRUB(1));
    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_EDAC), ==,
                    SDRAM_EDAC_EDACON | SDRAM_EDAC_SCRUB(1));

    /*
     * CacheCmdSta: SECCOUNT 15:8 and DEDETECTED 18 do not stick (and the
     * written EDACON = 0 clears the reset bit, it is R/W)
     */
    qtest_writel(qts, EDAC_CACHECMDSTA, CACHE_DEDETECTED | CACHE_SEC(0xFF));
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA), ==, 0);
    qtest_writel(qts, EDAC_CACHECMDSTA, CACHE_EDACON | CACHE_SCRUB(3));
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA), ==,
                    CACHE_EDACON | CACHE_SCRUB(3));

    /* rotbCmdSta: the strobe bit never reads back */
    qtest_writel(qts, EDAC_ROTBCMDSTA,
                 ROTB_DEDETECTED | ROTB_SEC(0xFF) | ROTB_EDACRESET);
    g_assert_cmphex(qtest_readl(qts, EDAC_ROTBCMDSTA), ==, 0);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/edac-boot-state (M-1, D-Uk): the edac-at-boot property
 * decides the EDACON bits at every device reset. Default hw-reset = the
 * ICD reset values (matches edac-reg-reset); all-on re-arms all three
 * over a guest write; a 3-bit mask (1 = sdram, 2 = cache, 4 = rotb)
 * covers single memories; a bad value is refused and changes nothing.
 * Red-by-construction: the property does not exist before M-1.
 */
static void test_edac_boot_state(void)
{
    QTestState *qts = stix_qtest_new();

    /* default hw-reset: the ICD (cache on, sdram/rotb off) */
    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_EDAC) & SDRAM_EDAC_EDACON,
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA), ==, CACHE_EDACON);
    g_assert_cmphex(qtest_readl(qts, EDAC_ROTBCONFIG) & ROTBCFG_EDACON,
                    ==, 0);

    /* all-on at every reset: a guest-cleared bit comes back */
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "edac-at-boot",
                                   "all-on"));
    qtest_writel(qts, EDAC_SDRAM_EDAC, 0);
    qtest_writel(qts, EDAC_ROTBCONFIG, 0);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_EDAC) & SDRAM_EDAC_EDACON,
                    ==, SDRAM_EDAC_EDACON);
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA) & CACHE_EDACON,
                    ==, CACHE_EDACON);
    g_assert_cmphex(qtest_readl(qts, EDAC_ROTBCONFIG) & ROTBCFG_EDACON,
                    ==, ROTBCFG_EDACON);

    /*
     * mask 4 = rotb only: the cache EDACON goes 0 (the owner applies
     * the hand-over state after its ICD reset)
     */
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "edac-at-boot",
                                   "4"));
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_EDAC) & SDRAM_EDAC_EDACON,
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA), ==, 0);
    g_assert_cmphex(qtest_readl(qts, EDAC_ROTBCONFIG) & ROTBCFG_EDACON,
                    ==, ROTBCFG_EDACON);

    /*
     * a bad value is refused (the parse must consume the whole string,
     * so "4junk" fails too) and the mode is unchanged
     */
    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "edac-at-boot",
                                    "nonsense"));
    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "edac-at-boot",
                                    "4junk"));
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA), ==, 0);
    g_assert_cmphex(qtest_readl(qts, EDAC_ROTBCONFIG) & ROTBCFG_EDACON,
                    ==, ROTBCFG_EDACON);

    qtest_quit(qts);
    stix_qtest_cleanup();

    /*
     * the -global form (the live-gate recipe, M-6): applied at the first
     * reset, before the machine boots. Red-by-construction: the type
     * does not exist before M-1 (the pre-M-1 binary warns and ignores
     * the global).
     */
    qts = stix_qtest_new_extra("-global stix-edac.edac-at-boot=all-on");
    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_EDAC) & SDRAM_EDAC_EDACON,
                    ==, SDRAM_EDAC_EDACON);
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA) & CACHE_EDACON,
                    ==, CACHE_EDACON);
    g_assert_cmphex(qtest_readl(qts, EDAC_ROTBCONFIG) & ROTBCFG_EDACON,
                    ==, ROTBCFG_EDACON);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/edac-strobe (M-1): the EDACRST/EDACRESET write-1 strobes
 * clear the SEC counter and the DeDetected latch, leave EDACON and
 * SCRUBRATE intact and read back 0 (self-clearing). The latches are
 * forced through the unit's test-only properties (M-1 has no upsets
 * yet). Red-by-construction: no EDAC unit and no strobe semantics
 * before M-1.
 */
static void test_edac_strobe(void)
{
    QTestState *qts = stix_qtest_new();
    bool ok;

    /* Sdram:Edac */
    qtest_writel(qts, EDAC_SDRAM_EDAC,
                 SDRAM_EDAC_EDACON | SDRAM_EDAC_SCRUB(2));
    g_assert_true(stix_qom_set_u64(qts, EDAC_EDAC_PATH,
                                   "test-latch-sdram", 0x80000005u));
    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_EDAC), ==,
                    SDRAM_EDAC_EDACON | SDRAM_EDAC_SCRUB(2) |
                    SDRAM_EDAC_DEDETECTED | SDRAM_EDAC_SEC(5));
    qtest_writel(qts, EDAC_SDRAM_EDAC,
                 SDRAM_EDAC_EDACON | SDRAM_EDAC_SCRUB(2) |
                 SDRAM_EDAC_EDACRST);
    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_EDAC), ==,
                    SDRAM_EDAC_EDACON | SDRAM_EDAC_SCRUB(2));
    g_assert_cmpuint(stix_qom_get_u64(qts, EDAC_EDAC_PATH,
                                      "test-latch-sdram", &ok), ==, 0);
    g_assert_true(ok);

    /* CacheCmdSta */
    qtest_writel(qts, EDAC_CACHECMDSTA, CACHE_EDACON);
    g_assert_true(stix_qom_set_u64(qts, EDAC_EDAC_PATH,
                                   "test-latch-cache", 0x80000083u));
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA), ==,
                    CACHE_EDACON | CACHE_DEDETECTED | CACHE_SEC(0x83));
    qtest_writel(qts, EDAC_CACHECMDSTA, CACHE_EDACON | CACHE_EDACRESET);
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA), ==, CACHE_EDACON);
    g_assert_cmpuint(stix_qom_get_u64(qts, EDAC_EDAC_PATH,
                                      "test-latch-cache", &ok), ==, 0);
    g_assert_true(ok);

    /* rotbCmdSta */
    g_assert_true(stix_qom_set_u64(qts, EDAC_EDAC_PATH,
                                   "test-latch-rotb", 0x80000007u));
    g_assert_cmphex(qtest_readl(qts, EDAC_ROTBCMDSTA), ==,
                    ROTB_SEC(7) | ROTB_DEDETECTED);
    qtest_writel(qts, EDAC_ROTBCMDSTA, ROTB_EDACRESET);
    g_assert_cmphex(qtest_readl(qts, EDAC_ROTBCMDSTA), ==, 0);
    g_assert_cmpuint(stix_qom_get_u64(qts, EDAC_EDAC_PATH,
                                      "test-latch-rotb", &ok), ==, 0);
    g_assert_true(ok);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/edac-rotb-strobe-quirk (M-1): the FSW's EDAC_reset(ROTBUF)
 * pair (EDAC.c:305-306) — 0x4 then ~(1u << 2) = 0xFFFFFFFB — still lands
 * as RESETFIFO (the documented side effect, test_rotb_strobe) and clears
 * nothing it should not: the strobe write is not a FIFO op, rotbConfig
 * is untouched, READY stays. Red-by-construction: the strobe is a M-1
 * semantic.
 */
static void test_edac_rotb_strobe_quirk(void)
{
    QTestState *qts = rotb_qtest_new(8);
    bool ok;
    uint32_t i;

    rotb_cfg(qts, true, true);
    for (i = 0; i < 3; i++) {
        rotb_cmd(qts, ROTB_CMD_PUTBLOCK);
    }
    g_assert_cmpuint(rotb_count(qts), ==, 3);
    g_assert_true(stix_qom_set_u64(qts, EDAC_EDAC_PATH,
                                   "test-latch-rotb", 0x42u));

    /* the FSW's EDACRESET strobe pair, verbatim */
    ctrl_write(qts, CTRL_ROTBCMDSTA, 0x00000004u);
    g_assert_cmpuint(rotb_count(qts), ==, 3);   /* not a FIFO op */
    ctrl_write(qts, CTRL_ROTBCMDSTA, 0xFFFFFFFBu);

    /*
     * counters cleared, FIFO emptied by RESETFIFO, READY kept; the
     * stored COMMAND bits (1:0) read back RESETFIFO
     */
    g_assert_cmphex(ctrl_read(qts, CTRL_ROTBCMDSTA), ==,
                    ROTB_STA_READY | ROTB_CMD_RESETFIFO);
    /* rotbConfig (ENABLE|TESTMODE) untouched by the dance */
    g_assert_cmphex(qtest_readl(qts, EDAC_ROTBCONFIG), ==,
                    ROTB_CFG_ENABLE | ROTB_CFG_TESTMODE);
    g_assert_cmpuint(stix_qom_get_u64(qts, EDAC_EDAC_PATH,
                                      "test-latch-rotb", &ok), ==, 0);
    g_assert_true(ok);

    /* the ring works on */
    rotb_cmd(qts, ROTB_CMD_PUTBLOCK);
    g_assert_cmpuint(rotb_count(qts), ==, 1);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- CACHE (FPGA SRAM) upsets (memory-upset plan M-2) ---- */

#define EDAC_ELUT       0xE0001000u  /* inside the ELUT window */
#define EDAC_IRQ_BIT    (1u << 6)    /* IRQ_STIXCTRL_EDAC = 6 */

/* ---- SDRAM upsets and the reboot path (memory-upset plan M-3) ---- */

/* a machine-RAM word and the guest scratch the trap test uses */
#define EDAC_SDRAM_DATA      0x40001000u
#define EDAC_SDRAM_RESULT    0x40002000u
#define EDAC_SDRAM_TRAPMAGIC 0x40002004u

/* A string property, or NULL when qom-get fails. */
static char *stix_qom_get_str(QTestState *qts, const char *path,
                              const char *prop)
{
    QDict *resp = qtest_qmp(qts,
                            "{'execute': 'qom-get',"
                            " 'arguments': {'path': %s, 'property': %s}}",
                            path, prop);
    const char *s = qdict_get_try_str(resp, "return");
    char *val = s ? g_strdup(s) : NULL;

    qobject_unref(resp);
    return val;
}

/* the number of armed poisons in upsets-active (spacing-agnostic) */
static unsigned edac_poison_count(QTestState *qts)
{
    g_autofree char *json = stix_qom_get_str(qts, EDAC_EDAC_PATH,
                                             "upsets-active");
    const char *p = json;
    unsigned n = 0;

    while ((p = strstr(p, "\"memory\"")) != NULL) {
        n++;
        p++;
    }
    return n;
}

/*
 * /sparc/stix/edac-cache-seu (M-2): a 1-bit upset in a LACC word is
 * corrected on read, counts one SEC and self-retires (D-Uh), so a
 * further read counts no more. RED on M-1: no upset-inject verb — the
 * qom-set is refused, nothing arms and the overlay never exists.
 */
static void test_edac_cache_seu(void)
{
    QTestState *qts = stix_qtest_new();
    const uint32_t addr = QL_LACC(0) + 0x40u;
    g_autofree char *json;

    qtest_writel(qts, addr, 0x12345678u);
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0x12345678u);

    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "cache,0xE0180040,0x1"));
    json = stix_qom_get_str(qts, EDAC_EDAC_PATH, "upsets-active");
    /* qobject_to_json (unpretty) separates keys with ": " and ", " */
    g_assert_nonnull(strstr(json, "\"memory\": \"cache\""));
    g_assert_nonnull(strstr(json, "\"mode\": \"seu\""));

    g_assert_cmphex(qtest_readl(qts, addr), ==, 0x12345678u);
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA) & CACHE_SEC(0xFF),
                    ==, CACHE_SEC(1));

    /* the poison retired: nothing armed, a second read counts no more */
    json = stix_qom_get_str(qts, EDAC_EDAC_PATH, "upsets-active");
    g_assert_null(strstr(json, "\"memory\""));
    g_assert_nonnull(strstr(json, "\"corrected\": 1"));
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0x12345678u);
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA) & CACHE_SEC(0xFF),
                    ==, CACHE_SEC(1));

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/edac-cache-seu-raw (M-2, D-Uf): with EDACON = 0 the flip
 * is returned raw, SECCOUNT stays 0 and the poison stays armed (nothing
 * corrected the cell). RED on M-1: no upset-inject verb and no overlay.
 */
static void test_edac_cache_seu_raw(void)
{
    QTestState *qts = stix_qtest_new();
    const uint32_t addr = QL_LACC(0) + 0x44u;

    qtest_writel(qts, addr, 0xCAFEBABEu);
    qtest_writel(qts, EDAC_CACHECMDSTA, 0);     /* EDACON = 0 */
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "cache,0xE0180044,0x1"));

    g_assert_cmphex(qtest_readl(qts, addr), ==, 0xCAFEBABEu ^ 0x1u);
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA) & CACHE_SEC(0xFF),
                    ==, 0);
    /* still armed: the second raw read flips again, no correction */
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0xCAFEBABEu ^ 0x1u);
    g_assert_cmpuint(edac_poison_count(qts), ==, 1);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/edac-cache-ded (M-2, D-Ue): a 2-bit flip returns the
 * corrupted word, latches DEDETECTED and pulses IRQ 6; the FSW's
 * EDACRESET set-then-clear pair (EDAC.c:299-302) clears the latch. The
 * poison is one-shot: the next read returns the clean word. RED on M-1:
 * no injection path — the read returns clean and IRQ 6 never pulses
 * (only the test-latch property could force DEDETECTED).
 */
static void test_edac_cache_ded(void)
{
    QTestState *qts = stix_qtest_new();
    const uint32_t addr = QL_LACC(0) + 0x80u;

    qtest_writel(qts, IRQMP_CLEAR, EDAC_IRQ_BIT);
    qtest_writel(qts, addr, 0xA5A5A5A5u);
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "cache,0xE0180080,0x5"));

    g_assert_cmphex(qtest_readl(qts, addr), ==, 0xA5A5A5A5u ^ 0x5u);
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA),
                    ==, CACHE_EDACON | CACHE_DEDETECTED);
    g_assert_cmphex(qtest_readl(qts, IRQMP_PENDING) & EDAC_IRQ_BIT,
                    ==, EDAC_IRQ_BIT);

    /* one-shot: the detected read retired the poison */
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0xA5A5A5A5u);
    g_assert_cmpuint(edac_poison_count(qts), ==, 0);

    /* the FSW's EDACRESET strobe pair clears the latch, keeps EDACON */
    qtest_writel(qts, EDAC_CACHECMDSTA, CACHE_EDACON | CACHE_EDACRESET);
    qtest_writel(qts, EDAC_CACHECMDSTA, CACHE_EDACON);
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA), ==, CACHE_EDACON);

    qtest_writel(qts, IRQMP_CLEAR, EDAC_IRQ_BIT);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/edac-cache-opmode (M-2, D-Ue): with OPMODE = 1 (not cache
 * mode) a DE read still returns the flip — the register comment gates
 * the IRQ and the status, not the data — but no DEDETECTED and no IRQ 6.
 * RED on M-1: no injection path, no overlay.
 */
static void test_edac_cache_opmode(void)
{
    QTestState *qts = stix_qtest_new();
    const uint32_t addr = QL_LACC(0) + 0xC0u;

    qtest_writel(qts, IRQMP_CLEAR, EDAC_IRQ_BIT);
    qtest_writel(qts, addr, 0x5A5A5A5Au);
    /* EDACON = 1, OPMODE = 1 */
    qtest_writel(qts, EDAC_CACHECMDSTA, CACHE_EDACON | 1u);
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "cache,0xE01800C0,0x5"));

    g_assert_cmphex(qtest_readl(qts, addr), ==, 0x5A5A5A5Au ^ 0x5u);
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA),
                    ==, CACHE_EDACON | 1u);
    g_assert_cmphex(qtest_readl(qts, IRQMP_PENDING) & EDAC_IRQ_BIT, ==, 0);
    /* still armed: the read was not a detected one */
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0x5A5A5A5Au ^ 0x5u);
    g_assert_cmpuint(edac_poison_count(qts), ==, 1);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/edac-cache-tlut (M-2): the same SEU in the ELUT window,
 * with no EDAC-on precondition beyond EDACON: EDACON = 0 returns the
 * flip raw, EDACON = 1 corrects and counts on the next read (the gate
 * is live per read). RED on M-1: no upset-inject verb, no overlay.
 */
static void test_edac_cache_tlut(void)
{
    QTestState *qts = stix_qtest_new();
    const uint32_t addr = EDAC_ELUT;

    qtest_writel(qts, addr, 0x0BADF00Du);
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "cache,0xE0001000,0x1"));

    qtest_writel(qts, EDAC_CACHECMDSTA, 0);     /* EDACON = 0 */
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0x0BADF00Du ^ 0x1u);
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA) & CACHE_SEC(0xFF),
                    ==, 0);

    qtest_writel(qts, EDAC_CACHECMDSTA, CACHE_EDACON);
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0x0BADF00Du);
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA) & CACHE_SEC(0xFF),
                    ==, CACHE_SEC(1));
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0x0BADF00Du);
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA) & CACHE_SEC(0xFF),
                    ==, CACHE_SEC(1));

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/edac-cache-persist (M-2, D-Uh): persist=1 keeps the
 * poison after its first effect, so every read corrects and counts
 * again, saturating at the 8-bit SECCOUNT (D-Ue). RED on M-1: no
 * upset-inject verb (persist mode).
 */
static void test_edac_cache_persist(void)
{
    QTestState *qts = stix_qtest_new();
    const uint32_t addr = QL_LACC(1) + 0x10u;
    int i;

    qtest_writel(qts, addr, 0xDEADBEEFu);
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "cache,0xE0188010,0x1,seu,1"));

    for (i = 1; i <= 3; i++) {
        g_assert_cmphex(qtest_readl(qts, addr), ==, 0xDEADBEEFu);
        g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA) & CACHE_SEC(0xFF),
                        ==, CACHE_SEC(i));
    }
    for (i = 0; i < 260; i++) {
        qtest_readl(qts, addr);
    }
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA) & CACHE_SEC(0xFF),
                    ==, CACHE_SEC(0xFF));
    g_assert_cmpuint(edac_poison_count(qts), ==, 1);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/edac-inject-reject (M-2, D-Ug): unknown memory, unaligned
 * address, zero mask, a mask outside the 32-bit word and an address
 * outside the SRAM are all refused and change nothing, like the
 * adc-inject setter; upset-clear with a bad index is refused too.
 * RED on M-1: the verbs do not exist — every set (including the good
 * one at the end) is refused for a different reason.
 */
static void test_edac_inject_reject(void)
{
    QTestState *qts = stix_qtest_new();
    const uint32_t addr = QL_LACC(0) + 0x40u;
    g_autofree char *json;

    /* the verbs exist (RED on M-1 fails here, naming upsets-active) */
    json = stix_qom_get_str(qts, EDAC_EDAC_PATH, "upsets-active");
    g_assert_nonnull(json);
    g_assert_null(strstr(json, "\"memory\""));

    qtest_writel(qts, addr, 0x11223344u);

    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                    "sram,0xE0180040,0x1"));
    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                    "cache,0xE0180041,0x1"));
    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                    "cache,0xE0180040,0x0"));
    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                    "cache,0xE0180040,0x1FFFFFFFF"));
    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                    "cache,0xE0200000,0x1"));
    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-clear",
                                    "7"));

    /* nothing armed, no counter moved, the word reads clean */
    json = stix_qom_get_str(qts, EDAC_EDAC_PATH, "upsets-active");
    g_assert_null(strstr(json, "\"memory\""));
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA) & CACHE_SEC(0xFF),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0x11223344u);

    /* and a good injection still works afterwards */
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "cache,0xE0180040,0x1"));
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0x11223344u);
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA) & CACHE_SEC(0xFF),
                    ==, CACHE_SEC(1));
    json = stix_qom_get_str(qts, EDAC_EDAC_PATH, "upsets-active");
    g_assert_nonnull(strstr(json, "\"injected\": 1"));

    /*
     * a re-inject of an armed word replaces its record: one poison,
     * two injected, and a clear-all leaves nothing armed
     */
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "cache,0xE0180040,0x3"));
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "cache,0xE0180040,0x5"));
    g_assert_cmpuint(edac_poison_count(qts), ==, 1);
    json = stix_qom_get_str(qts, EDAC_EDAC_PATH, "upsets-active");
    g_assert_nonnull(strstr(json, "\"injected\": 3"));
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-clear",
                                   "all"));
    g_assert_cmpuint(edac_poison_count(qts), ==, 0);
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-clear",
                                   "0") == false);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/edac-cache-subword (M-2, D-Uc): reads of a poisoned word
 * see the flip at every width (u32, u16 LACC-style, u8 ELUT-style), and
 * a write touching any byte of the word clears the whole poison (the
 * EDAC word is the unit). RED on M-1: no overlay — sub-word reads
 * cannot see a flip.
 */
static void test_edac_cache_subword(void)
{
    QTestState *qts = stix_qtest_new();
    const uint32_t wa = QL_LACC(0) + 0x20u;
    const uint32_t wb = QL_LACC(0) + 0x24u;

    /* EDACON = 0: pure overlay mechanics, no status consumed */
    qtest_writel(qts, EDAC_CACHECMDSTA, 0);
    qtest_writel(qts, wa, 0x11223344u);
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "cache,0xE0180020,0x300"));

    /* the flip, at every width (mask sits in byte 2) */
    g_assert_cmphex(qtest_readl(qts, wa), ==, 0x11223044u);
    g_assert_cmphex(qtest_readw(qts, wa + 2), ==, 0x3044u);
    g_assert_cmphex(qtest_readb(qts, wa + 2), ==, 0x30u);
    g_assert_cmphex(qtest_readw(qts, wa), ==, 0x1122u);
    g_assert_cmphex(qtest_readb(qts, wa + 3), ==, 0x44u);

    /* a u8 write touching one byte clears the whole poison */
    qtest_writeb(qts, wa + 3, 0xEEu);
    g_assert_cmphex(qtest_readl(qts, wa), ==, 0x112230EEu);
    g_assert_cmphex(qtest_readw(qts, wa + 2), ==, 0x30EEu);
    g_assert_cmphex(qtest_readb(qts, wa + 2), ==, 0x30u);
    g_assert_cmpuint(edac_poison_count(qts), ==, 0);

    /* ... and so does a u16 write (LACC style) */
    qtest_writel(qts, wb, 0xAABBCCDDu);
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "cache,0xE0180024,0x10000"));
    g_assert_cmphex(qtest_readb(qts, wb + 1), ==, 0xBAu);
    qtest_writew(qts, wb + 2, 0x1234u);
    /* the RMW keeps the untouched flip (EDACON = 0): BB ^ 01 = BA */
    g_assert_cmphex(qtest_readl(qts, wb), ==, 0xAABA1234u);
    g_assert_cmpuint(edac_poison_count(qts), ==, 0);

    /* nothing counted: EDACON was off all along */
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA) & CACHE_SEC(0xFF),
                    ==, 0);

    /*
     * EDACON = 1: an SEU is corrected by the RMW's read half, so a
     * sub-word store leaves the untouched bytes clean
     */
    qtest_writel(qts, EDAC_CACHECMDSTA, CACHE_EDACON);
    qtest_writel(qts, wa, 0x11223344u);
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "cache,0xE0180020,0x100"));
    qtest_writeb(qts, wa + 3, 0x55u);
    g_assert_cmphex(qtest_readl(qts, wa), ==, 0x11223355u);
    g_assert_cmpuint(edac_poison_count(qts), ==, 0);

    /*
     * the page's overlay is parked and reused: two more inject/clear
     * cycles on the same page still see the flip (the MR is not
     * initialized twice)
     */
    for (int c = 0; c < 2; c++) {
        g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                       "cache,0xE0180020,0x300"));
        qtest_writel(qts, EDAC_CACHECMDSTA, 0);
        g_assert_cmphex(qtest_readl(qts, wa), ==, 0x11223055u);
        qtest_writel(qts, wa, 0x11223355u);
        g_assert_cmpuint(edac_poison_count(qts), ==, 0);
        g_assert_cmphex(qtest_readl(qts, wa), ==, 0x11223355u);
    }

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/edac-cache-swap-poison (M-2, D-Uc): the QL swap's
 * host-side LACC materialization retires an armed poison — the FPGA
 * rewrite cures the cell, so the rewritten word reads clean and no SEC
 * is ever counted. Pins the stix_edac_host_write hook. RED on M-1: no
 * upset-inject verb, so nothing can arm before the swap.
 */
static void test_edac_cache_swap_poison(void)
{
    QTestState *qts = stix_qtest_new();
    const uint32_t addr = QL_LACC(0) + 8u;

    /* det-source=zero: the swap materializes zeros (known data) */
    g_assert_true(sky_set_str(qts, "det-source", "zero"));
    det_start(qts, 0);          /* QlIntTime 0: the swap is the next tick */

    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "cache,0xE0180008,0x1"));
    g_assert_cmpuint(edac_poison_count(qts), ==, 1);

    det_step_ticks(qts, 1);     /* the 100 ms tick: the QL swap */
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0);
    g_assert_cmpuint(edac_poison_count(qts), ==, 0);
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0);
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA) & CACHE_SEC(0xFF),
                    ==, 0);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- SDRAM upsets and the reboot path (memory-upset plan M-3) ---- */

/*
 * /sparc/stix/edac-sdram-seu (M-3): a 1-bit upset in a machine-RAM word
 * is corrected on read with EDACON = 1, counts one SEC and self-retires
 * (D-Uh); the FSW's EDACRST pair (EDAC.c:294-297) clears the counter.
 * RED on M-2: the sdram inject is refused, so SECCOUNT stays 0.
 */
static void test_edac_sdram_seu(void)
{
    QTestState *qts = stix_qtest_new();
    const uint32_t addr = EDAC_SDRAM_DATA;

    qtest_writel(qts, EDAC_SDRAM_EDAC, SDRAM_EDAC_EDACON);
    qtest_writel(qts, addr, 0x12345678u);
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "sdram,0x40001000,0x1"));

    g_assert_cmphex(qtest_readl(qts, addr), ==, 0x12345678u);
    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_EDAC) & SDRAM_EDAC_SEC(0xFF),
                    ==, SDRAM_EDAC_SEC(1));

    /* the poison retired: nothing armed, a second read counts no more */
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0x12345678u);
    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_EDAC) & SDRAM_EDAC_SEC(0xFF),
                    ==, SDRAM_EDAC_SEC(1));
    g_assert_cmpuint(edac_poison_count(qts), ==, 0);

    /* the EDACRST set-then-clear pair leaves EDACON intact */
    qtest_writel(qts, EDAC_SDRAM_EDAC,
                 SDRAM_EDAC_EDACON | SDRAM_EDAC_EDACRST);
    qtest_writel(qts, EDAC_SDRAM_EDAC, SDRAM_EDAC_EDACON);
    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_EDAC), ==, SDRAM_EDAC_EDACON);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/edac-sdram-seu-raw (M-3, D-Uf): with EDACON = 0 (the ICD
 * reset, txt:3249) the flip is returned raw, SECCOUNT stays 0 and the
 * poison stays armed. RED on M-2: the sdram inject is refused, the read
 * returns the clean word.
 */
static void test_edac_sdram_seu_raw(void)
{
    QTestState *qts = stix_qtest_new();
    const uint32_t addr = EDAC_SDRAM_DATA + 0x40u;

    qtest_writel(qts, addr, 0xCAFEBABEu);
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "sdram,0x40001040,0x1"));

    g_assert_cmphex(qtest_readl(qts, addr), ==, 0xCAFEBABEu ^ 0x1u);
    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_EDAC) & SDRAM_EDAC_SEC(0xFF),
                    ==, 0);
    /* still armed: the second raw read flips again, no correction */
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0xCAFEBABEu ^ 0x1u);
    g_assert_cmpuint(edac_poison_count(qts), ==, 1);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/edac-sdram-ded-ahbstat (M-3, D-Ud): with EDACON = 1 the
 * DE read is an AHB error response — DEDETECTED latches and the AHBSTAT
 * window 0x80000F00 holds the failing address; with EDACON = 0 it is
 * the raw row and nothing latches. qtest drives no CPU, so the trap
 * itself is pinned by edac-sdram-ded-trap-runtime; the AHBSTAT latch
 * (which only the MEMTX_ERROR branch writes) pins the error path here.
 * The latch survives a system reset (the post-reboot record is the BRAM
 * copy, WDT.c:86-126). RED on M-2: the sdram inject is refused and no
 * AHBSTAT window exists (reads 0 — same as the reset value).
 */
static void test_edac_sdram_ded_ahbstat(void)
{
    QTestState *qts = stix_qtest_new();
    const uint32_t ra = EDAC_SDRAM_DATA + 0x40u;
    const uint32_t rb = EDAC_SDRAM_DATA + 0x80u;

    /* the raw row first: EDACON = 0, no AHB error, nothing latched */
    qtest_writel(qts, ra, 0xA5A5A5A5u);
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "sdram,0x40001040,0x9"));
    g_assert_cmphex(qtest_readl(qts, ra), ==, 0xA5A5A5A5u ^ 0x9u);
    g_assert_cmphex(qtest_readl(qts, EDAC_AHBSTAT), ==, 0);
    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_EDAC)
                    & SDRAM_EDAC_DEDETECTED, ==, 0);

    /* EDACON = 1: the read faults — DEDETECTED and AHBSTAT latch */
    qtest_writel(qts, EDAC_SDRAM_EDAC, SDRAM_EDAC_EDACON);
    qtest_writel(qts, rb, 0x5A5A5A5Au);
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "sdram,0x40001080,0x9"));
    g_assert_cmphex(qtest_readl(qts, rb), ==, 0x5A5A5A5Au ^ 0x9u);
    g_assert_cmphex(qtest_readl(qts, EDAC_AHBSTAT), ==, 0x40001080u);
    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_EDAC),
                    ==, SDRAM_EDAC_EDACON | SDRAM_EDAC_DEDETECTED);

    /* one-shot: the detected read retired its poison (ra stays armed) */
    g_assert_cmphex(qtest_readl(qts, rb), ==, 0x5A5A5A5Au);
    g_assert_cmpuint(edac_poison_count(qts), ==, 1);
    g_assert_cmphex(qtest_readl(qts, EDAC_AHBSTAT), ==, 0x40001080u);

    /* the watchdog reboot: the register resets, the latch does not */
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_EDAC), ==, 0);
    g_assert_cmphex(qtest_readl(qts, EDAC_AHBSTAT), ==, 0x40001080u);
    g_assert_cmpuint(edac_poison_count(qts), ==, 1);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/edac-sdram-persist (M-3, D-Uh): persist=1 models the
 * stuck cell — every read re-detects, AHBSTAT keeps the address and the
 * poison stays armed (the boot-loop mode; the live reboot loop itself
 * is M-6 edac-smoke). RED on M-2: the sdram inject is refused.
 */
static void test_edac_sdram_persist(void)
{
    QTestState *qts = stix_qtest_new();
    const uint32_t addr = EDAC_SDRAM_DATA + 0xC0u;
    g_autofree char *json;

    qtest_writel(qts, EDAC_SDRAM_EDAC, SDRAM_EDAC_EDACON);
    qtest_writel(qts, addr, 0xDEADBEEFu);
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "sdram,0x400010C0,0x5,de,1"));

    g_assert_cmphex(qtest_readl(qts, addr), ==, 0xDEADBEEFu ^ 0x5u);
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0xDEADBEEFu ^ 0x5u);
    g_assert_cmphex(qtest_readl(qts, EDAC_AHBSTAT), ==, 0x400010C0u);
    json = stix_qom_get_str(qts, EDAC_EDAC_PATH, "upsets-active");
    g_assert_nonnull(strstr(json, "\"detected\": 2"));
    g_assert_cmpuint(edac_poison_count(qts), ==, 1);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * The -kernel guest for edac-sdram-ded-trap-runtime (the
 * write_spin_elf pattern, the gptimer test). It sets its own trap
 * table (TBR = 0x40003000), enables traps (PSR = S|PS|ET) and loops
 * reading the poisoned word 0x40001100 into RESULT 0x40002000. The
 * tt 0x09 handler sits at TBR + 0x90 — this emulator routes trap tt to
 * base + tt*16 (int32_helper.c:163-165) — records 0x55 at TRAPMAGIC
 * 0x40002004 and spins.
 */
static gchar *write_trap_elf(void)
{
    /* ELF32, big-endian, EXEC, EM_SPARC, entry 0x40000000, phoff 52 */
    static const uint8_t hdr[52] = {
        0x7f, 'E', 'L', 'F', 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 2, 0, 2, 0, 0, 0, 1, 0x40, 0, 0, 0, 0, 0, 0, 52,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 52, 0, 32, 0, 2, 0, 0,
        0, 0, 0, 0,
    };
    static const uint8_t phdr[2][32] = {
        {   /* the code: offset 116, vaddr = paddr = 0x40000000, 68 B */
            0, 0, 0, 1, 0, 0, 0, 116, 0x40, 0, 0, 0, 0x40, 0, 0, 0,
            0, 0, 0, 68, 0, 0, 0, 68, 0, 0, 0, 5, 0, 0, 0, 4,
        },
        {   /* the trap table: offset 184, vaddr = paddr = 0x40003000 */
            0, 0, 0, 1, 0, 0, 0, 184, 0x40, 0, 0x30, 0, 0x40, 0, 0x30, 0,
            0, 0, 0, 164, 0, 0, 0, 164, 0, 0, 0, 6, 0, 0, 0, 4,
        },
    };
    /* encodings computed and decoded field by field (M-3 review) */
    static const uint8_t code[68] = {
        0x05, 0x10, 0x00, 0x0C,     /* sethi %hi(0x40003000), %g2 */
        0x81, 0x98, 0x80, 0x00,     /* wr   %g2, %g0, %tbr        */
        0x84, 0x10, 0x20, 0xE0,     /* or   %g0, 0xE0, %g2        */
        0x81, 0x88, 0x80, 0x00,     /* wr   %g2, %g0, %psr        */
        0x01, 0x00, 0x00, 0x00,     /* nop                        */
        0x01, 0x00, 0x00, 0x00,     /* nop                        */
        0x03, 0x10, 0x00, 0x04,     /* sethi %hi(0x40001100), %g1 */
        0xC6, 0x00, 0x61, 0x00,     /* ld   [%g1 + 0x100], %g3    */
        0x09, 0x10, 0x00, 0x08,     /* sethi %hi(0x40002000), %g4 */
        0xC6, 0x21, 0x00, 0x00,     /* st   %g3, [%g4]            */
        0x10, 0xBF, 0xFF, 0xFC,     /* ba   loop (0x40000018)     */
        0x01, 0x00, 0x00, 0x00,     /* nop                        */
        0x0B, 0x10, 0x00, 0x08,     /* sethi %hi(0x40002004), %g5 */
        0x8C, 0x10, 0x20, 0x55,     /* or   %g0, 0x55, %g6        */
        0xCC, 0x21, 0x60, 0x04,     /* st   %g6, [%g5 + 4]        */
        0x10, 0x80, 0x00, 0x00,     /* ba   .                     */
        0x01, 0x00, 0x00, 0x00,     /* nop                        */
    };
    /*
     * 164 zero bytes, except the tt 0x09 slot at +0x90: ba 0x40000030
     * and its delay-slot nop (a zero word is unimp)
     */
    static const uint8_t table[164] = {
        [0x90] = 0x10, [0x91] = 0xBF, [0x92] = 0xF3, [0x93] = 0xE8,
        [0x94] = 0x01,              /* the delay slot: nop */
    };
    gchar *path = NULL;
    int fd = g_file_open_tmp("stix-trap-XXXXXX.elf", &path, NULL);

    g_assert(fd >= 0);
    g_assert(write(fd, hdr, sizeof(hdr)) == sizeof(hdr));
    g_assert(write(fd, phdr, sizeof(phdr)) == sizeof(phdr));
    g_assert(write(fd, code, sizeof(code)) == sizeof(code));
    g_assert(write(fd, table, sizeof(table)) == sizeof(table));
    close(fd);
    return path;
}

/*
 * The -kernel guest for edac-sdram-dword (M-3 review finding 1): an
 * aligned ldd/std is one 8-byte io access on SPARC. The guest loops
 * ldd [0x40001200] -> %g2/%g3, std to RESULT + 0x10 and std back into
 * the (overlaid) page at 0x40001208. One phdr: offset 84, vaddr =
 * paddr = 0x40000000, 28 bytes. Encodings computed and decoded field
 * by field.
 */
static gchar *write_dword_elf(void)
{
    static const uint8_t hdr[52] = {
        0x7f, 'E', 'L', 'F', 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 2, 0, 2, 0, 0, 0, 1, 0x40, 0, 0, 0, 0, 0, 0, 52,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 52, 0, 32, 0, 1, 0, 0,
        0, 0, 0, 0,
    };
    static const uint8_t phdr[32] = {
        0, 0, 0, 1, 0, 0, 0, 84, 0x40, 0, 0, 0, 0x40, 0, 0, 0,
        0, 0, 0, 28, 0, 0, 0, 28, 0, 0, 0, 7, 0, 0, 0, 4,
    };
    static const uint8_t code[28] = {
        0x03, 0x10, 0x00, 0x04,     /* sethi %hi(0x40001000), %g1 */
        0xC4, 0x18, 0x62, 0x00,     /* ldd  [%g1 + 0x200], %g2    */
        0x09, 0x10, 0x00, 0x08,     /* sethi %hi(0x40002000), %g4 */
        0xC4, 0x39, 0x20, 0x10,     /* std  %g2, [%g4 + 0x10]     */
        0xC4, 0x38, 0x62, 0x08,     /* std  %g2, [%g1 + 0x208]    */
        0x10, 0xBF, 0xFF, 0xFB,     /* ba   0x40000000            */
        0x01, 0x00, 0x00, 0x00,     /* nop                        */
    };
    gchar *path = NULL;
    int fd = g_file_open_tmp("stix-dword-XXXXXX.elf", &path, NULL);

    g_assert(fd >= 0);
    g_assert(write(fd, hdr, sizeof(hdr)) == sizeof(hdr));
    g_assert(write(fd, phdr, sizeof(phdr)) == sizeof(phdr));
    g_assert(write(fd, code, sizeof(code)) == sizeof(code));
    close(fd);
    return path;
}

/*
 * /sparc/stix/edac-sdram-dword (M-3 review finding 1): with a poison
 * armed elsewhere in the page (the overlay is in place), the guest's
 * 8-byte ldd/std still read and write both words. RED before the fix
 * (overlay valid.max_access_size 4): the 8-byte access is a decode
 * error, the ldd returns 0 and the std is dropped, silently.
 */
static void test_edac_sdram_dword(void)
{
    QTestState *qts;
    g_autofree gchar *elf = NULL;
    g_autofree gchar *extra = NULL;
    gint64 deadline;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("needs TCG to run the guest");
        return;
    }
    elf = write_dword_elf();
    extra = g_strdup_printf("-accel tcg -kernel %s", elf);
    qts = stix_qtest_new_extra(extra);

    /* EDACON = 0 (hw-reset): a raw poison only arms the page overlay */
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "sdram,0x40001000,0x1"));
    qtest_writel(qts, EDAC_SDRAM_DATA + 0x200u, 0x11223344u);
    qtest_writel(qts, EDAC_SDRAM_DATA + 0x204u, 0x55667788u);
    deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (qtest_readl(qts, EDAC_SDRAM_RESULT + 0x10u) != 0x11223344u ||
           qtest_readl(qts, EDAC_SDRAM_RESULT + 0x14u) != 0x55667788u ||
           qtest_readl(qts, EDAC_SDRAM_DATA + 0x208u) != 0x11223344u ||
           qtest_readl(qts, EDAC_SDRAM_DATA + 0x20Cu) != 0x55667788u) {
        g_assert(g_get_monotonic_time() < deadline);
        g_usleep(1000);
    }
    /* the overlay was in place all along */
    g_assert_cmpuint(edac_poison_count(qts), ==, 1);

    qtest_quit(qts);
    stix_qtest_cleanup();
    g_unlink(elf);
}


/*
 * /sparc/stix/edac-sdram-ded-trap-runtime (M-3, D-Ud, the 2026-11-12
 * user decision): boot with hw-reset — the guest reads the DE word raw,
 * no trap; the runtime edac-enable qom-set (a platform verb) arms the
 * trap path, and the next DE load raises trap 0x09: the guest's own
 * handler runs, the poison is retired (one-shot) and the loop is parked
 * in the handler, so RESULT keeps the flipped word. qtest drives no
 * CPU, so the trap is only observable through guest code. RED on M-2:
 * the sdram inject is refused — RESULT never flips, the trap never
 * fires.
 */
static void test_edac_sdram_ded_trap_runtime(void)
{
    QTestState *qts;
    g_autofree gchar *elf = NULL;
    g_autofree gchar *extra = NULL;
    gint64 deadline;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("needs TCG to run the guest");
        return;
    }
    elf = write_trap_elf();
    extra = g_strdup_printf("-accel tcg -kernel %s", elf);
    qts = stix_qtest_new_extra(extra);

    /* the poisoned word holds a known pattern while the guest spins */
    qtest_writel(qts, EDAC_SDRAM_DATA + 0x100u, 0xCAFEBABEu);
    deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (qtest_readl(qts, EDAC_SDRAM_RESULT) != 0xCAFEBABEu) {
        g_assert(g_get_monotonic_time() < deadline);
        g_usleep(1000);
    }

    /* the raw row: the flip is returned, no trap */
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "sdram,0x40001100,0x9"));
    deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (qtest_readl(qts, EDAC_SDRAM_RESULT) != (0xCAFEBABEu ^ 0x9u)) {
        g_assert(g_get_monotonic_time() < deadline);
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_TRAPMAGIC), ==, 0);

    /* the runtime enable: EDACON = 1, as a software write would set */
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "edac-enable", "1"));
    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_EDAC) & SDRAM_EDAC_EDACON,
                    ==, SDRAM_EDAC_EDACON);

    /* the next DE load is the AHB error: trap 0x09, the handler ran */
    deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (qtest_readl(qts, EDAC_SDRAM_TRAPMAGIC) != 0x55u) {
        g_assert(g_get_monotonic_time() < deadline);
        g_usleep(1000);
    }
    /*
     * the loop is parked in the handler: the raw result is still the
     * flip, the one-shot poison is retired, the registers latched
     */
    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_RESULT),
                    ==, 0xCAFEBABEu ^ 0x9u);
    g_assert_cmphex(qtest_readl(qts, EDAC_AHBSTAT), ==, 0x40001100u);
    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_EDAC),
                    ==, SDRAM_EDAC_EDACON | SDRAM_EDAC_DEDETECTED);

    qtest_quit(qts);
    stix_qtest_cleanup();
    g_unlink(elf);
}

/* ---- ROTBUF upsets (memory-upset plan M-4) ---- */

/*
 * /sparc/stix/edac-rotb-seu (M-4): a 1-bit upset in the ring block the
 * window shows is corrected on read with rotbConfig EDACON = 1, counts
 * one SEC and self-retires (the flip lives in the poison table, keyed by
 * the ring byte offset block*512+offset). The grammar rejections (block
 * out of range, offset out of range/unaligned, no block:offset) name the
 * field and change nothing. RED on M-3: the rotbuf inject is refused, so
 * SECCOUNT stays 0.
 */
static void test_edac_rotb_seu(void)
{
    QTestState *qts = rotb_qtest_new(8);
    g_autofree char *json;

    /* the grammar rejections: nothing armed, nothing counted */
    if (rotb_blocks_supported()) {
        g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                        "rotbuf,8:0,0x1"));   /* ring holds 8 */
    }
    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                    "rotbuf,0x20000:0,0x1")); /* out of range */
    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                    "rotbuf,0:512,0x1"));     /* offset OOR */
    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                    "rotbuf,0:2,0x1"));       /* unaligned */
    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                    "rotbuf,0,0x1"));         /* no offset */
    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                    "rotbuf,x:0,0x1"));       /* bad block */
    json = stix_qom_get_str(qts, EDAC_EDAC_PATH, "upsets-active");
    g_assert_nonnull(json);
    g_assert_null(strstr(json, "\"memory\""));

    /* fill block 0 through the window in TESTMODE, make it a block */
    rotb_cfg(qts, true, true);
    qtest_writel(qts, ROTB_WINDOW, 0x12345678u);
    qtest_writel(qts, ROTB_WINDOW + 4u, 0x9ABCDEF0u);
    rotb_cmd(qts, ROTB_CMD_PUTBLOCK);

    /* rotbConfig EDACON = 1 (the FSW's RotAccumulator_EDAC_on path) */
    qtest_writel(qts, EDAC_ROTBCONFIG, ROTB_CFG_ENABLE | ROTBCFG_EDACON);
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "rotbuf,0:0,0x1"));

    /* the window shows block 0 (rd = 0): corrected, one SEC */
    g_assert_cmphex(qtest_readl(qts, ROTB_WINDOW), ==, 0x12345678u);
    g_assert_cmphex(qtest_readl(qts, EDAC_ROTBCMDSTA) & ROTB_SEC(0xFF),
                    ==, ROTB_SEC(1));

    /* the poison retired: a second read counts no more */
    g_assert_cmphex(qtest_readl(qts, ROTB_WINDOW), ==, 0x12345678u);
    g_assert_cmphex(qtest_readl(qts, EDAC_ROTBCMDSTA) & ROTB_SEC(0xFF),
                    ==, ROTB_SEC(1));
    g_assert_cmpuint(edac_poison_count(qts), ==, 0);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/edac-rotb-ded (M-4, D-Ue): a 2-bit flip returns the
 * corrupted block data, latches DEDETECTED (rotbCmdSta bit 4) and pulses
 * the shared EDAC IRQ 6 (StixIdpu.h:1548, the FSW's one ISR checks CACHE
 * then ROTBUF on it); ROTBUF never errors the bus — no trap path. The
 * FSW's EDACRESET strobe pair (EDAC.c:305-306) clears the latch. RED on
 * M-3: the rotbuf inject is refused — no flip, no IRQ, no latch.
 */
static void test_edac_rotb_ded(void)
{
    QTestState *qts = rotb_qtest_new(8);

    qtest_writel(qts, IRQMP_CLEAR, EDAC_IRQ_BIT);
    rotb_cfg(qts, true, true);
    qtest_writel(qts, ROTB_WINDOW, 0xA5A5A5A5u);
    rotb_cmd(qts, ROTB_CMD_PUTBLOCK);
    qtest_writel(qts, EDAC_ROTBCONFIG, ROTB_CFG_ENABLE | ROTBCFG_EDACON);
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "rotbuf,0:0,0x5"));

    g_assert_cmphex(qtest_readl(qts, ROTB_WINDOW), ==, 0xA5A5A5A5u ^ 0x5u);
    g_assert_cmphex(qtest_readl(qts, EDAC_ROTBCMDSTA) & ROTB_DEDETECTED,
                    ==, ROTB_DEDETECTED);
    g_assert_cmphex(qtest_readl(qts, IRQMP_PENDING) & EDAC_IRQ_BIT,
                    ==, EDAC_IRQ_BIT);

    /* one-shot: the detected read retired the poison */
    g_assert_cmphex(qtest_readl(qts, ROTB_WINDOW), ==, 0xA5A5A5A5u);
    g_assert_cmpuint(edac_poison_count(qts), ==, 0);

    /* the FSW's EDACRESET strobe pair: the latch clears, the ring stays */
    qtest_writel(qts, EDAC_ROTBCMDSTA, ROTB_EDACRESET);
    qtest_writel(qts, EDAC_ROTBCMDSTA, 0xFFFFFFFBu);
    g_assert_cmphex(qtest_readl(qts, EDAC_ROTBCMDSTA) & ROTB_DEDETECTED,
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, ROTB_WINDOW), ==, 0xA5A5A5A5u);

    qtest_writel(qts, IRQMP_CLEAR, EDAC_IRQ_BIT);
    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/edac-rotb-fpga-block (M-4, D-Uc write rule): the detector
 * append (stixctrl_rotb_append, the FPGA fill path) rewrites the blocks
 * it lands in and cures their poisons — the rewritten cell is fixed, the
 * window reads the chunk data clean and no counter moves. RED on M-3:
 * the rotbuf inject is refused, so nothing can arm before the swap.
 */
static void test_edac_rotb_fpga_block(void)
{
    QTestState *qts = rotb_qtest_new(64);

    rotb_cfg(qts, true, false);                 /* the append's mode */
    sky_set_str(qts, "det-source", "zero");
    det_start_nom(qts, 39, 0, 9, 0xFFFFFFFFu);  /* 10-tick integrations */

    /* block 0 is the append target (wr = 0): poison a data word */
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "rotbuf,0:16,0x1"));
    g_assert_cmpuint(edac_poison_count(qts), ==, 1);
    /* EDACON = 0: the raw row shows the flip while the cell is armed */
    g_assert_cmphex(qtest_readl(qts, ROTB_WINDOW + 16u), ==, 0x1u);

    det_step_ticks(qts, 10);    /* the first chunk lands on block 0 */
    g_assert_cmphex(qtest_readl(qts, ROTB_WINDOW + 16u), ==, 0);
    g_assert_cmpuint(edac_poison_count(qts), ==, 0);
    g_assert_cmphex(qtest_readl(qts, ROTB_WINDOW + 16u), ==, 0);
    g_assert_cmphex(qtest_readl(qts, EDAC_ROTBCMDSTA)
                    & (ROTB_SEC(0xFF) | ROTB_DEDETECTED), ==, 0);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/edac-enable-all (M-4): with rotbuf wired, edac-enable
 * takes 4 (rotb, RotbConfig EDACON bit 12) and "all-on" sets all three
 * EdacOn bits — as software writes would; a masked set leaves the
 * modules left out alone, "0" is refused. RED on M-3: both sets are
 * refused (rotbuf unwired) and rotbConfig EDACON stays 0.
 */
static void test_edac_enable_all(void)
{
    QTestState *qts = stix_qtest_new();

    /* mask 4 = rotb only: the others keep their hand-over state */
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "edac-enable", "4"));
    g_assert_cmphex(qtest_readl(qts, EDAC_ROTBCONFIG) & ROTBCFG_EDACON,
                    ==, ROTBCFG_EDACON);
    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_EDAC) & SDRAM_EDAC_EDACON,
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA), ==, CACHE_EDACON);

    /* all-on: the remaining two set, nothing clears */
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "edac-enable",
                                   "all-on"));
    g_assert_cmphex(qtest_readl(qts, EDAC_SDRAM_EDAC) & SDRAM_EDAC_EDACON,
                    ==, SDRAM_EDAC_EDACON);
    g_assert_cmphex(qtest_readl(qts, EDAC_CACHECMDSTA) & CACHE_EDACON,
                    ==, CACHE_EDACON);
    g_assert_cmphex(qtest_readl(qts, EDAC_ROTBCONFIG) & ROTBCFG_EDACON,
                    ==, ROTBCFG_EDACON);

    /* an empty mask enables nothing and is refused */
    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "edac-enable", "0"));

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- BRAM silent flips (memory-upset plan M-5) ---- */

/*
 * /sparc/stix/edac-bram-wdt (M-5): a flip in the WDT UNCHANGED_STORAGE
 * area (0xE02000A8, the record WDT_callAfterReset reads at every boot,
 * WDT.c:239-263 — here the low half of the bad-trap magic, WDT.c:74) is
 * silent — no poison, no counter, no status — the inl reads the flipped
 * value and system_reset keeps it (the BRAM survives a reboot,
 * grlib_stixctrl_reset leaves the AHB RAM alone); a fresh QEMU instance
 * is the power cycle and reads 0. RED on M-4: the bram inject is refused
 * ("M-5 wires bram"), so the word never flips.
 */
static void test_edac_bram_wdt(void)
{
    QTestState *qts = stix_qtest_new();
    const uint32_t addr = AHB_UNCHANGED;
    g_autofree char *json;
    QTestState *fresh;

    qtest_writel(qts, addr, 0x5A5AA5A5u);
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "bram,0xE02000A8,0x1"));

    /* silent: nothing armed, no counters, no status of any kind */
    json = stix_qom_get_str(qts, EDAC_EDAC_PATH, "upsets-active");
    g_assert_null(strstr(json, "\"memory\""));
    g_assert_null(strstr(json, "\"bram\""));

    /* the flip is the bytes: the inl reads the flipped value */
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0x5A5AA5A5u ^ 0x1u);

    /* a reboot does not heal the cell */
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0x5A5AA5A5u ^ 0x1u);

    qtest_quit(qts);
    stix_qtest_cleanup();

    /* a fresh instance is the power cycle: the RAM starts zeroed */
    fresh = stix_qtest_new();
    g_assert_cmphex(qtest_readl(fresh, addr), ==, 0);
    qtest_quit(fresh);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/edac-bram-flip (M-5): the same contract on the MemBram
 * record (0xE02003C0), flipping its EDAC_reboot_module field byte
 * (0xE02003C3) — mechanism regression only: the record has no ASW reader
 * (MemBram.h:10-11, a SuSW-only record). RED on M-4: the bram inject is
 * refused, so the field byte never flips.
 */
static void test_edac_bram_flip(void)
{
    QTestState *qts = stix_qtest_new();
    const uint32_t rec = AHB_MEMBRAM;
    const uint32_t fld = AHB_MEMBRAM_MOD;
    QTestState *fresh;

    qtest_writel(qts, rec, 0x12345678u);
    /* mask byte 0 lands on the field byte: 0x78 ^ 0xFF = 0x87 */
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "bram,0xE02003C3,0xFF000000"));
    g_assert_cmpuint(edac_poison_count(qts), ==, 0);
    g_assert_cmphex(qtest_readl(qts, rec), ==, 0x12345687u);
    g_assert_cmphex(qtest_readb(qts, fld), ==, 0x87u);

    /* survives a system reset ... */
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readb(qts, fld), ==, 0x87u);

    qtest_quit(qts);
    stix_qtest_cleanup();

    /* ... and dies at the power cycle (a fresh instance reads 0) */
    fresh = stix_qtest_new();
    g_assert_cmphex(qtest_readl(fresh, rec), ==, 0);
    qtest_quit(fresh);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/edac-bram-reject (M-5, D-Ug): a bram address below the
 * window, at its end, one whose four mask bytes cross it, and one that
 * wraps past it are refused (bounds from the window size), and so are a
 * zero / over-32-bit mask and a mode (the BRAM has no EDAC); every
 * refusal changes nothing. RED on M-4: the rejections pass there for
 * the wrong reason (every bram set is refused) — the trailing good
 * inject is what fails.
 */
static void test_edac_bram_reject(void)
{
    QTestState *qts = stix_qtest_new();
    const uint32_t addr = AHB_UNCHANGED;
    g_autofree char *json;

    qtest_writel(qts, addr, 0x11223344u);

    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                    "bram,0xE01FFF00,0x1"));   /* below */
    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                    "bram,0xE0200400,0x1"));   /* the end */
    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                    "bram,0xE02003FE,0x1"));   /* +4 crosses */
    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                    "bram,0xFFFFFFFFFFFFFFFF,0x1")); /* wraps */
    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                    "bram,0xE02000A8,0x0"));   /* mask 0 */
    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                    "bram,0xE02000A8,0x1FFFFFFFF")); /* >32 */
    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                    "bram,0xE02000A8,0x1,seu")); /* no mode */
    g_assert_false(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                    "bram,0xE02000A8,0x1,de,1"));

    /* nothing armed, the word reads untouched */
    json = stix_qom_get_str(qts, EDAC_EDAC_PATH, "upsets-active");
    g_assert_null(strstr(json, "\"memory\""));
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0x11223344u);

    /* and a good injection still works afterwards */
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "bram,0xE02000A8,0x1"));
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0x11223344u ^ 0x1u);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/* ---- seeded random upsets and the hw-state section (plan M-6a) ---- */

/*
 * The pinned xorshift32 (Marsaglia's 13/17/5) and the draw order of
 * random-seed/random-count, mirrored from stix_edac.c: per upset one
 * draw picks the memory uniformly among the wired EDAC memories (in
 * this order; never bram), one the 4-aligned word uniformly in the
 * memory's extent, one the bit. The state restarts from the seed at
 * every burst.
 */
static uint32_t edac_rand_ref(uint32_t *state)
{
    uint32_t x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static const struct {
    const char *name;
    uint32_t base, words;
} edac_random_mems[] = {
    { "sdram",  0x40000000u, 0x800000u },   /* -m 32M / 4 */
    { "cache",  0xE0000000u, 0x80000u },    /* the 2 MB SRAM / 4 */
    { "rotbuf", 0x00000000u, 0x4000u  },    /* 128 blocks * 512 B / 4 */
};

/* one attempt of the burst: three draws (memory, word, bit) */
static void edac_random_draw(uint32_t *state, unsigned *mi, uint32_t *addr,
                             uint32_t *mask)
{
    unsigned bit;

    *mi = edac_rand_ref(state) % ARRAY_SIZE(edac_random_mems);
    *addr = edac_random_mems[*mi].base +
            (edac_rand_ref(state) % edac_random_mems[*mi].words) * 4u;
    bit = edac_rand_ref(state) % 32;
    *mask = 1u << bit;
}

/*
 * The reference burst, mirroring stix_edac.c's skip rule: an attempt
 * whose word is already placed — the @n_prior records before this
 * burst's, or one of the burst's own — is skipped and its three draws
 * are spent; the burst gives up after 1024 attempts.
 */
static void edac_random_burst(uint32_t seed, unsigned count,
                              uint32_t addr[], uint32_t mask[],
                              const char *mem[], unsigned n_prior)
{
    uint32_t state = seed;
    unsigned placed = 0, draws = 0;

    while (placed < count && draws < 1024) {
        unsigned mi, i, n = n_prior + placed;
        uint32_t a, m;

        edac_random_draw(&state, &mi, &a, &m);
        draws++;
        for (i = 0; i < n; i++) {
            if (mem[i] == edac_random_mems[mi].name && addr[i] == a) {
                break;
            }
        }
        if (i < n) {
            continue;
        }
        mem[n] = edac_random_mems[mi].name;
        addr[n] = a;
        mask[n] = m;
        placed++;
    }
    /* the test's extents and counts never exhaust the attempts */
    g_assert_cmpuint(placed, ==, count);
}

/*
 * /sparc/stix/edac-random (M-6a, D-Ug): random-seed + random-count draw
 * exactly the pinned sequence — the reference burst above recomputes it
 * and every record is checked through upsets-active. A second burst of
 * the same seed skips the words it already poisoned and continues the
 * sequence into fresh ones; after an upset-clear the same seed draws
 * the first burst again. The refusals (seed 0, count 0, a count that
 * does not fit the table) change nothing. Red-by-construction on M-5:
 * the properties do not exist, the qom-set is refused and nothing arms.
 */
static void test_edac_random(void)
{
    const uint32_t seed = 0x1234ABCDu;
    const unsigned count = 5;
    QTestState *qts = stix_qtest_new_extra(
        "-m 32M -global grlib-stixctrl.rotb-blocks=128");
    static const char *const names[] = { "sdram", "cache", "rotbuf" };
    uint32_t addr[16], mask[16];
    const char *mem[16];
    g_autofree char *json = NULL;
    QDict *act, *counters;
    QList *poisons;
    bool ok;
    unsigned i, injected = 0;

    /*
     * The reference generator itself is pinned to canonical xorshift32
     * (13/17/5) outputs computed independently (outside this file), so a
     * misreading shared by the model and edac_rand_ref cannot pass.
     */
    {
        uint32_t st = seed;

        g_assert_cmphex(edac_rand_ref(&st), ==, 0x6EE4450Bu);
        g_assert_cmphex(edac_rand_ref(&st), ==, 0x2EEF9309u);
        g_assert_cmphex(edac_rand_ref(&st), ==, 0x4D55748Eu);
    }

    /* seed 0 (the default) is off */
    g_assert_false(stix_qom_set_u64(qts, EDAC_EDAC_PATH, "random-count",
                                    count));
    json = stix_qom_get_str(qts, EDAC_EDAC_PATH, "upsets-active");
    g_assert_null(strstr(json, "\"memory\""));

    g_assert_true(stix_qom_set_u64(qts, EDAC_EDAC_PATH, "random-seed",
                                   seed));
    g_assert_cmpuint(stix_qom_get_u64(qts, EDAC_EDAC_PATH, "random-seed",
                                      &ok), ==, seed);
    g_assert_true(ok);

    /* count 0 and a count that does not fit the table are refused */
    g_assert_false(stix_qom_set_u64(qts, EDAC_EDAC_PATH, "random-count",
                                    0));
    g_assert_false(stix_qom_set_u64(qts, EDAC_EDAC_PATH, "random-count",
                                    300));      /* the table holds 256 */
    json = stix_qom_get_str(qts, EDAC_EDAC_PATH, "upsets-active");
    g_assert_null(strstr(json, "\"memory\""));

    /* the burst: exactly the reference records, all one-bit SEUs */
    g_assert_true(stix_qom_set_u64(qts, EDAC_EDAC_PATH, "random-count",
                                   count));
    edac_random_burst(seed, count, addr, mask, mem, 0);
    json = stix_qom_get_str(qts, EDAC_EDAC_PATH, "upsets-active");
    act = qobject_to(QDict, qobject_from_json(json, &error_abort));
    poisons = qdict_get_qlist(act, "poisons");
    for (i = 0; i < count; i++) {
        QDict *rec = qobject_to(QDict, plat_list_nth(poisons, i));

        g_assert_nonnull(rec);
        g_assert_cmpstr(qdict_get_str(rec, "memory"), ==, mem[i]);
        g_assert_cmpuint(qdict_get_int(rec, "address"), ==, addr[i]);
        g_assert_cmpuint(qdict_get_int(rec, "mask"), ==, mask[i]);
        g_assert_cmpstr(qdict_get_str(rec, "mode"), ==, "seu");
        g_assert(!qdict_get_bool(rec, "persist"));
    }
    g_assert(plat_list_nth(poisons, count) == NULL);

    /* the counters moved by five, the BRAM has none */
    counters = qdict_get_qdict(act, "counters");
    for (i = 0; i < ARRAY_SIZE(names); i++) {
        QDict *c = qdict_get_qdict(counters, names[i]);

        g_assert_nonnull(c);
        injected += qdict_get_int(c, "injected");
        g_assert_cmpuint(qdict_get_int(c, "corrected"), ==, 0);
        g_assert_cmpuint(qdict_get_int(c, "detected"), ==, 0);
    }
    g_assert_cmpuint(injected, ==, count);
    g_assert_null(qdict_get_qdict(counters, "bram"));
    qobject_unref(act);

    /*
     * the same seed draws the same sequence again: the five words it
     * poisoned are skipped (D-Ug) and the burst continues into fresh
     * ones, records count..2*count - 1 of the draw sequence
     */
    g_assert_true(stix_qom_set_u64(qts, EDAC_EDAC_PATH, "random-count",
                                   count));
    edac_random_burst(seed, count, addr, mask, mem, count);
    json = stix_qom_get_str(qts, EDAC_EDAC_PATH, "upsets-active");
    act = qobject_to(QDict, qobject_from_json(json, &error_abort));
    poisons = qdict_get_qlist(act, "poisons");
    for (i = 0; i < 2 * count; i++) {
        QDict *rec = qobject_to(QDict, plat_list_nth(poisons, i));

        g_assert_nonnull(rec);
        g_assert_cmpstr(qdict_get_str(rec, "memory"), ==, mem[i]);
        g_assert_cmpuint(qdict_get_int(rec, "address"), ==, addr[i]);
        g_assert_cmpuint(qdict_get_int(rec, "mask"), ==, mask[i]);
    }
    g_assert(plat_list_nth(poisons, 2 * count) == NULL);
    counters = qdict_get_qdict(act, "counters");
    injected = 0;
    for (i = 0; i < ARRAY_SIZE(names); i++) {
        injected += qdict_get_int(qdict_get_qdict(counters, names[i]),
                                  "injected");
    }
    g_assert_cmpuint(injected, ==, 2 * count);
    qobject_unref(act);

    /* after an upset-clear the same seed draws the first burst again */
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-clear",
                                   "all"));
    g_assert_true(stix_qom_set_u64(qts, EDAC_EDAC_PATH, "random-count",
                                   count));
    edac_random_burst(seed, count, addr, mask, mem, 0);
    json = stix_qom_get_str(qts, EDAC_EDAC_PATH, "upsets-active");
    act = qobject_to(QDict, qobject_from_json(json, &error_abort));
    poisons = qdict_get_qlist(act, "poisons");
    for (i = 0; i < count; i++) {
        QDict *rec = qobject_to(QDict, plat_list_nth(poisons, i));

        g_assert_nonnull(rec);
        g_assert_cmpstr(qdict_get_str(rec, "memory"), ==, mem[i]);
        g_assert_cmpuint(qdict_get_int(rec, "address"), ==, addr[i]);
        g_assert_cmpuint(qdict_get_int(rec, "mask"), ==, mask[i]);
    }
    g_assert(plat_list_nth(poisons, count) == NULL);
    qobject_unref(act);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

/*
 * /sparc/stix/edac-hw-state (M-6a, D-Ug/D-PLo): the stixctrl hw-state
 * carries the edac section — the same JSON as upsets-active, through
 * the one accessor. Pure like every section: two reads with no
 * clock_step in between are byte-equal, and an armed upset shows the
 * same record and counters in both faces. Red-by-construction on M-5:
 * the section does not exist.
 */
static void test_edac_hw_state(void)
{
    static const char *const names[] = { "sdram", "cache", "rotbuf" };
    static const char *const ck[] = { "injected", "corrected", "detected" };
    QTestState *qts = stix_qtest_new();
    g_autofree char *a = NULL, *b = NULL, *json = NULL;
    QDict *st, *edac, *counters, *act, *rec;
    QList *poisons;
    unsigned i, k;

    /* empty at reset, with the three counters and no bram */
    st = plat_hw_state(qts, "/machine/stixctrl");
    edac = plat_dict(st, "edac");
    poisons = qdict_get_qlist(edac, "poisons");
    g_assert_nonnull(poisons);
    g_assert(plat_list_nth(poisons, 0) == NULL);
    counters = plat_dict(edac, "counters");
    for (i = 0; i < ARRAY_SIZE(names); i++) {
        QDict *c = plat_dict(counters, names[i]);

        g_assert_cmpuint(qdict_get_int(c, ck[0]), ==, 0);
        g_assert_cmpuint(qdict_get_int(c, ck[1]), ==, 0);
        g_assert_cmpuint(qdict_get_int(c, ck[2]), ==, 0);
    }
    g_assert_null(qdict_get_qdict(counters, "bram"));
    qobject_unref(st);

    /* an armed upset shows in the section ... */
    qtest_writel(qts, 0xE0180040u, 0x12345678u);
    g_assert_true(stix_qom_set_str(qts, EDAC_EDAC_PATH, "upset-inject",
                                   "cache,0xE0180040,0x1"));
    st = plat_hw_state(qts, "/machine/stixctrl");
    edac = plat_dict(st, "edac");
    poisons = qdict_get_qlist(edac, "poisons");
    rec = qobject_to(QDict, plat_list_nth(poisons, 0));
    g_assert_nonnull(rec);
    g_assert_cmpstr(qdict_get_str(rec, "memory"), ==, "cache");
    g_assert_cmpuint(qdict_get_int(rec, "address"), ==, 0xE0180040u);
    g_assert_cmpuint(qdict_get_int(rec, "mask"), ==, 1);
    g_assert_cmpstr(qdict_get_str(rec, "mode"), ==, "seu");
    g_assert_cmpuint(qdict_get_int(plat_dict(plat_dict(edac, "counters"),
                                             "cache"), ck[0]), ==, 1);
    qobject_unref(st);

    /* ... exactly as in upsets-active (the same JSON, D-Ug) */
    json = stix_qom_get_str(qts, EDAC_EDAC_PATH, "upsets-active");
    act = qobject_to(QDict, qobject_from_json(json, &error_abort));
    st = plat_hw_state(qts, "/machine/stixctrl");
    edac = plat_dict(st, "edac");
    g_assert(plat_list_nth(qdict_get_qlist(act, "poisons"), 0) != NULL);
    g_assert(plat_list_nth(qdict_get_qlist(edac, "poisons"), 0) != NULL);
    g_assert(plat_list_nth(qdict_get_qlist(act, "poisons"), 1) == NULL);
    g_assert(plat_list_nth(qdict_get_qlist(edac, "poisons"), 1) == NULL);
    for (i = 0; i < ARRAY_SIZE(names); i++) {
        QDict *c = plat_dict(plat_dict(act, "counters"), names[i]);
        QDict *h = plat_dict(plat_dict(edac, "counters"), names[i]);

        for (k = 0; k < 3; k++) {
            g_assert_cmpuint(qdict_get_int(c, ck[k]), ==,
                             qdict_get_int(h, ck[k]));
        }
    }
    qobject_unref(st);
    qobject_unref(act);

    /* pure: two reads with no clock_step in between are byte-equal */
    a = plat_hw_state_str(qts, "/machine/stixctrl");
    b = plat_hw_state_str(qts, "/machine/stixctrl");
    g_assert_cmpstr(a, ==, b);

    qtest_quit(qts);
    stix_qtest_cleanup();
}

static void stix_edac_register(void)
{
    qtest_add_func("/sparc/stix/edac-reg-reset", test_edac_reg_reset);
    qtest_add_func("/sparc/stix/edac-reg-readonly", test_edac_reg_readonly);
    qtest_add_func("/sparc/stix/edac-boot-state", test_edac_boot_state);
    qtest_add_func("/sparc/stix/edac-strobe", test_edac_strobe);
    qtest_add_func("/sparc/stix/edac-rotb-strobe-quirk",
                   test_edac_rotb_strobe_quirk);
    qtest_add_func("/sparc/stix/edac-cache-seu", test_edac_cache_seu);
    qtest_add_func("/sparc/stix/edac-cache-seu-raw",
                   test_edac_cache_seu_raw);
    qtest_add_func("/sparc/stix/edac-cache-ded", test_edac_cache_ded);
    qtest_add_func("/sparc/stix/edac-cache-opmode",
                   test_edac_cache_opmode);
    qtest_add_func("/sparc/stix/edac-cache-tlut", test_edac_cache_tlut);
    qtest_add_func("/sparc/stix/edac-cache-persist",
                   test_edac_cache_persist);
    qtest_add_func("/sparc/stix/edac-inject-reject",
                   test_edac_inject_reject);
    qtest_add_func("/sparc/stix/edac-cache-subword",
                   test_edac_cache_subword);
    qtest_add_func("/sparc/stix/edac-cache-swap-poison",
                   test_edac_cache_swap_poison);
    qtest_add_func("/sparc/stix/edac-sdram-seu", test_edac_sdram_seu);
    qtest_add_func("/sparc/stix/edac-sdram-seu-raw",
                   test_edac_sdram_seu_raw);
    qtest_add_func("/sparc/stix/edac-sdram-ded-ahbstat",
                   test_edac_sdram_ded_ahbstat);
    qtest_add_func("/sparc/stix/edac-sdram-dword",
                   test_edac_sdram_dword);
    qtest_add_func("/sparc/stix/edac-sdram-ded-trap-runtime",
                   test_edac_sdram_ded_trap_runtime);
    qtest_add_func("/sparc/stix/edac-sdram-persist",
                   test_edac_sdram_persist);
    qtest_add_func("/sparc/stix/edac-rotb-seu", test_edac_rotb_seu);
    qtest_add_func("/sparc/stix/edac-rotb-ded", test_edac_rotb_ded);
    qtest_add_func("/sparc/stix/edac-rotb-fpga-block",
                   test_edac_rotb_fpga_block);
    qtest_add_func("/sparc/stix/edac-enable-all", test_edac_enable_all);
    qtest_add_func("/sparc/stix/edac-bram-wdt", test_edac_bram_wdt);
    qtest_add_func("/sparc/stix/edac-bram-flip", test_edac_bram_flip);
    qtest_add_func("/sparc/stix/edac-bram-reject", test_edac_bram_reject);
    qtest_add_func("/sparc/stix/edac-random", test_edac_random);
    qtest_add_func("/sparc/stix/edac-hw-state", test_edac_hw_state);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (qtest_has_machine("leon3_generic")) {
        qtest_add_func("/sparc/stix/pnp", test_pnp);
        qtest_add_func("/sparc/stix/flash", test_flash);
        qtest_add_func("/sparc/stix/flash-error-paths", test_flash_error_paths);
        qtest_add_func("/sparc/stix/flash-addressing", test_flash_addressing);
        qtest_add_func("/sparc/stix/flash-storage", test_flash_storage);
        qtest_add_func("/sparc/stix/flash-reset-state",
                       test_flash_reset_state);
        qtest_add_func("/sparc/stix/flash-latency-read",
                       test_flash_latency_read);
        qtest_add_func("/sparc/stix/flash-latency-prog",
                       test_flash_latency_prog);
        qtest_add_func("/sparc/stix/flash-latency-erase",
                       test_flash_latency_erase);
        qtest_add_func("/sparc/stix/flash-latency-abort",
                       test_flash_latency_abort);
        qtest_add_func("/sparc/stix/flash-latency-busy-reject",
                       test_flash_latency_busy_reject);
        qtest_add_func("/sparc/stix/flash-latency-dma-guard",
                       test_flash_latency_dma_guard);
        qtest_add_func("/sparc/stix/flash-latency-props/subprocess",
                       test_flash_latency_props_subprocess);
        qtest_add_func("/sparc/stix/flash-latency-props",
                       test_flash_latency_props);
        qtest_add_func("/sparc/stix/flash-latency-system-reset",
                       test_flash_latency_system_reset);
        qtest_add_func("/sparc/stix/flash-savevm", test_flash_savevm);
        qtest_add_func("/sparc/stix/flash-latency-default",
                       test_flash_latency_default);
        qtest_add_func("/sparc/stix/spw-tx", test_spw_tx);
        qtest_add_func("/sparc/stix/ctrl", test_ctrl);
        qtest_add_func("/sparc/stix/ctrl-scet", test_ctrl_scet);
        qtest_add_func("/sparc/stix/ctrl-rotb", test_ctrl_rotb);
        qtest_add_func("/sparc/stix/gptimer-prescaler", test_gptimer_prescaler);
        qtest_add_func("/sparc/stix/ctrl-watchdog", test_ctrl_watchdog);
        qtest_add_func("/sparc/stix/ctrl-irq-lines", test_ctrl_irq_lines);
        qtest_add_func("/sparc/stix/rotb-fifo", test_rotb_fifo);
        qtest_add_func("/sparc/stix/rotb-window", test_rotb_window);
        qtest_add_func("/sparc/stix/rotb-full-irq", test_rotb_full_irq);
        qtest_add_func("/sparc/stix/rotb-selftest-shape", test_rotb_selftest_shape);
        qtest_add_func("/sparc/stix/rotb-strobe", test_rotb_strobe);
        qtest_add_func("/sparc/stix/rotb-reset", test_rotb_reset);
        qtest_add_func("/sparc/stix/ahb-windows", test_ahb_windows);
        qtest_add_func("/sparc/stix/bram-persist", test_bram_persist);
        qtest_add_func("/sparc/stix/tmtc-hermetic", test_tmtc_hermetic);
        qtest_add_func("/sparc/stix/tmtc-bind-error/subprocess",
                       test_tmtc_bind_error_subprocess);
        qtest_add_func("/sparc/stix/tmtc-bind-error", test_tmtc_bind_error);
        qtest_add_func("/sparc/stix/flash-create-error/subprocess",
                       test_flash_create_error_subprocess);
        qtest_add_func("/sparc/stix/flash-create-error",
                       test_flash_create_error);
        qtest_add_func("/sparc/stix/spw-regs", test_spw_regs);
        qtest_add_func("/sparc/stix/tm-tcp", test_tm_tcp);
        qtest_add_func("/sparc/stix/tm-stalled-client",
                       test_tm_stalled_client);
        qtest_add_func("/sparc/stix/tm-reconnect", test_tm_reconnect);
        qtest_add_func("/sparc/stix/spw-rx", test_spw_rx);
        qtest_add_func("/sparc/stix/tc-tcp", test_tc_tcp);
        qtest_add_func("/sparc/stix/spw-timecode", test_spw_timecode);
        qtest_add_func("/sparc/stix/spw-udp", test_spw_udp);
        qtest_add_func("/sparc/stix/spw-udp-bind-error/subprocess",
                       test_spw_udp_bind_error_subprocess);
        qtest_add_func("/sparc/stix/spw-udp-bind-error",
                       test_spw_udp_bind_error);
        qtest_add_func("/sparc/stix/spw-udp-host-error/subprocess",
                       test_spw_udp_host_error_subprocess);
        qtest_add_func("/sparc/stix/spw-udp-host-error",
                       test_spw_udp_host_error);
        qtest_add_func("/sparc/stix/hk-adc", test_hk_adc);
        qtest_add_func("/sparc/stix/powerstat", test_powerstat);
        qtest_add_func("/sparc/stix/adc-inject", test_adc_inject);
        qtest_add_func("/sparc/stix/hk-prot", test_hk_prot);
        qtest_add_func("/sparc/stix/spw-link-down", test_spw_link_down);
        qtest_add_func("/sparc/stix/spw-link-down-reentry",
                       test_spw_link_down_reentry);
        qtest_add_func("/sparc/stix/det-fsm-strobes", test_det_fsm_strobes);
        qtest_add_func("/sparc/stix/det-ql-swap", test_det_ql_swap);
        qtest_add_func("/sparc/stix/det-ql-irq", test_det_ql_irq);
        qtest_add_func("/sparc/stix/det-ql-banks", test_det_ql_banks);
        qtest_add_func("/sparc/stix/det-testpulse", test_det_testpulse);
        qtest_add_func("/sparc/stix/det-reg-reset", test_det_reg_reset);
        qtest_add_func("/sparc/stix/det-model-off", test_det_model_off);
        qtest_add_func("/sparc/stix/det-token-spend", test_det_token_spend);
        qtest_add_func("/sparc/stix/det-idefx-ready", test_det_idefx_ready);
        qtest_add_func("/sparc/stix/det-idefx-roundtrip",
                       test_det_idefx_roundtrip);
        qtest_add_func("/sparc/stix/det-idefx-broadcast",
                       test_det_idefx_broadcast);
        qtest_add_func("/sparc/stix/det-idefx-reset", test_det_idefx_reset);
        qtest_add_func("/sparc/stix/det-idefx-gating", test_det_idefx_gating);
        qtest_add_func("/sparc/stix/det-idefx-adc", test_det_idefx_adc);
        qtest_add_func("/sparc/stix/det-idefx-off", test_det_idefx_off);
        qtest_add_func("/sparc/stix/det-counts-zero", test_det_counts_zero);
        qtest_add_func("/sparc/stix/det-counts-quiet", test_det_counts_quiet);
        qtest_add_func("/sparc/stix/det-counts-flare", test_det_counts_flare);
        qtest_add_func("/sparc/stix/det-counts-gating",
                       test_det_counts_gating);
        qtest_add_func("/sparc/stix/det-counts-r3-fsw",
                       test_det_counts_r3_fsw);
        qtest_add_func("/sparc/stix/det-counts-seed", test_det_counts_seed);
        qtest_add_func("/sparc/stix/det-lvar-ltrg", test_det_lvar_ltrg);
        qtest_add_func("/sparc/stix/det-props", test_det_props);
        qtest_add_func("/sparc/stix/det-chunk-format", test_det_chunk_format);
        qtest_add_func("/sparc/stix/det-nominal-rule", test_det_nominal_rule);
        qtest_add_func("/sparc/stix/det-chunk-gating", test_det_chunk_gating);
        qtest_add_func("/sparc/stix/det-chunk-full", test_det_chunk_full);
        qtest_add_func("/sparc/stix/det-chunk-sumdmask",
                       test_det_chunk_sumdmask);
        qtest_add_func("/sparc/stix/det-chunk-guard", test_det_chunk_guard);
        qtest_add_func("/sparc/stix/det-inject-lacc", test_det_inject_lacc);
        qtest_add_func("/sparc/stix/det-inject-chunk",
                       test_det_inject_chunk);
        qtest_add_func("/sparc/stix/det-inject-props",
                       test_det_inject_props);
        qtest_add_func("/sparc/stix/det-inject-gated",
                       test_det_inject_gated);
        qtest_add_func("/sparc/stix/det-inject-reset",
                       test_det_inject_reset);
        stix_det7_register();
        stix_platform_register();
        stix_att_register();
        stix_flashdmg_register();
        stix_edac_register();
    }

    return g_test_run();
}
