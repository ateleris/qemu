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
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qbool.h"
#include "qobject/qnum.h"

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

/* Driver register order: ram address, page address, cmd/sta. The device
 * asserts this exact write_counter sequence (0 -> 1 -> 2 -> execute). */
static void flash_issue(QTestState *qts, uint32_t ram, uint32_t page,
                        uint32_t cmd)
{
    qtest_writel(qts, FLASH_RAMAD, ram);
    qtest_writel(qts, FLASH_PAGEAD, page);
    qtest_writel(qts, FLASH_CMDSTA, FTNAND_FLASHTYPE | cmd);
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
 * For tests where QEMU itself must bind the port (tmtc-port=<free>):
 * the release-then-bind window is unavoidable there (P2-1 review).
 */
static int stix_free_tcp_port(void)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    socklen_t addrlen = sizeof(addr);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int port;

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
     *    not extended (holes would read 0x00 = bad-block marker). */
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
 * one. The device keeps only bits 2:0 of the write, so the BLOCKCOUNT and
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

#define TMTC_BASE_PORT 20000 /* IO_TCP_SERVER_BASE_PORT (tmtc_io_server.h) */

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
 * the default TM port (20000+uid) held — tmtc-server=auto starts no server
 * under qtest (P2-1). RED on the pre-P2-1 binary: the device still binds
 * the port at realize ("Bind failed"), QEMU exits, qtest_init aborts.
 * The hold is skipped only when the port is genuinely busy (EADDRINUSE —
 * with SO_REUSEADDR that means a real listener, i.e. the condition under
 * test already holds); any other hold failure skips the test instead of
 * passing it vacuously. */
static void test_tmtc_hermetic(void)
{
    int listening = 0;
    socklen_t llen = sizeof(listening);
    int held = stix_hold_tcp_port(TMTC_BASE_PORT + (int)getuid());
    QTestState *qts;

    if (held < 0) {
        if (errno == EADDRINUSE) {
            g_test_message("port %d already busy, hold skipped",
                           TMTC_BASE_PORT + (int)getuid());
        } else {
            g_autofree gchar *why = g_strdup_printf(
                "cannot hold the default TM port %d: %s",
                TMTC_BASE_PORT + (int)getuid(), g_strerror(errno));
            g_test_skip(why);
            return;
        }
    } else {
        /* the hold is really a listener */
        g_assert(getsockopt(held, SOL_SOCKET, SO_ACCEPTCONN,
                            &listening, &llen) == 0);
        g_assert_cmpint(listening, ==, 1);
    }

    qts = stix_qtest_new();

    /* alive: a PnP readl round-trips, as in test_pnp */
    g_assert_cmphex(stix_pnp_read(qts, 0), ==, pnp_golden[0]);

    qtest_quit(qts);
    stix_qtest_cleanup();
    if (held >= 0) {
        close(held);
    }
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
 * release-then-bind window stix_free_tcp_port() accepts).
 */
static int stix_free_udp_port(void)
{
    int port;
    int fd = stix_udp_bind_free(&port);

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

/* NOMINAL entry order (OpModeMgmt.c:669-672): SetEnable, then Reset */
static void det_start(QTestState *qts, uint32_t ql_int_time)
{
    ctrl_write(qts, CTRL_FSMQLCONFIG, ql_int_time << 16);
    det_cmd_or(qts, FSMCMD_SETENABLE);
    det_cmd_or(qts, FSMCMD_RESET);
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
/* just below LACC0 (the ELUT's last words) and past the LACC3 stride */
#define DET_BELOW_LACC0 (QL_LACC(0) - 0x10u)
#define DET_PAST_LACC3  QL_LACC(4)

static void det_fill_gaps(QTestState *qts)
{
    uint32_t n;

    /*
     * LTRG3 ends at the end of the 0x100 ltrg region, so there is no
     * LTRG word past it to guard
     */
    det_fill(qts, DET_BELOW_LACC0, 0x10u, DET_SENTINEL);
    det_fill(qts, DET_PAST_LACC3, 0x10u, DET_SENTINEL);

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
    det_expect(qts, DET_PAST_LACC3, 0x10u, DET_SENTINEL);

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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (qtest_has_machine("leon3_generic")) {
        qtest_add_func("/sparc/stix/pnp", test_pnp);
        qtest_add_func("/sparc/stix/flash", test_flash);
        qtest_add_func("/sparc/stix/flash-error-paths", test_flash_error_paths);
        qtest_add_func("/sparc/stix/flash-addressing", test_flash_addressing);
        qtest_add_func("/sparc/stix/flash-storage", test_flash_storage);
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
    }

    return g_test_run();
}
