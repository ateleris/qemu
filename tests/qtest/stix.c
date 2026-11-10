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
#include "libqtest.h"

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

/* -global arg moving the TM server off the default 20000+uid port, so a
 * test can run beside a live FSW session (Task C will make the server
 * opt-in under qtest; this stays valid then). */
static gchar *stix_private_tm_port_arg(void)
{
    return g_strdup_printf("-global grlib-stixspw.tmtc-port=%d",
                           30000 + (int)(getpid() % 20000));
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
    g_autofree gchar *port = stix_private_tm_port_arg();
    g_autofree uint8_t *zeros = g_malloc0(FLASH_PAGE_BYTES);
    QTestState *qts;
    uint32_t sta, pending;
    struct stat st;
    const uint32_t failed_ready = FTNAND_FAILED | FTNAND_READY;

    qts = stix_qtest_new_extra(port);

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
    gchar *port;
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

    port = stix_private_tm_port_arg();
    qts = qtest_initf("-M leon3_generic -global grlib-stixflash.flash-file=%s %s",
                      tmp, port);
    g_free(port);
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
        g_autofree gchar *port = stix_private_tm_port_arg();
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

        qts = qtest_initf("-M leon3_generic "
                          "-global grlib-stixflash.flash-file=%s %s",
                          path, port);

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
        g_autofree gchar *port = stix_private_tm_port_arg();
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
                          "-global grlib-stixflash.inverted-storage=off %s",
                          path, port);

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
#define SPW_DMA_PS (1u << 5)
#define SPW_IRQ_BIT (1u << 13)              /* IRQ_SPW = 13 */

#define TXDSC_EN (1u << 12)

/* TX descriptor layout (spwTXdsc_s): ctrl@0, haddr@4, dlen@8, daddr@12. */
#define TXDSC_ADDR  0x40001000u
#define TXDATA_ADDR 0x40002000u

static void test_spw_tx(void)
{
    QTestState *qts;
    uint32_t ctrl;

    qts = stix_qtest_new();

    /* TM payload + TX descriptor in guest RAM (values written via
     * qtest_writel are target-endian, matching the device's raw-read +
     * bswap32 handling). */
    qtest_writel(qts, TXDATA_ADDR, 0xaabbccddu);
    qtest_writel(qts, TXDATA_ADDR + 4, 0x11223344u);
    qtest_writel(qts, TXDSC_ADDR + 0, TXDSC_EN);        /* ctrl: EN, no IE */
    qtest_writel(qts, TXDSC_ADDR + 4, 0);               /* haddr */
    qtest_writel(qts, TXDSC_ADDR + 8, 8);               /* dlen */
    qtest_writel(qts, TXDSC_ADDR + 12, TXDATA_ADDR);    /* daddr */

    /* Point the DMA engine at the descriptor table. */
    qtest_writel(qts, SPW_DMATxDesc, TXDSC_ADDR);
    qtest_writel(qts, IRQMP_CLEAR, SPW_IRQ_BIT);

    /* Trigger transmission (TE). No TCP client is attached: the TM send
     * fails silently in the model and the exchange still completes. */
    qtest_writel(qts, SPW_DMAControl, SPW_DMA_TE);

    /* Descriptor EN bit cleared in guest RAM. */
    ctrl = qtest_readl(qts, TXDSC_ADDR);
    g_assert_cmpuint(ctrl & TXDSC_EN, ==, 0);

    /* IRQ 13 latched in IRQMP pending. */
    g_assert_cmpuint(qtest_readl(qts, IRQMP_PENDING) & SPW_IRQ_BIT,
                     ==, SPW_IRQ_BIT);

    /* dmaControl ends with PS set (exchange-complete modifier). */
    g_assert_cmpuint(qtest_readl(qts, SPW_DMAControl) & SPW_DMA_PS,
                     ==, SPW_DMA_PS);

    /* Status forced to the link-running value by the write handler. */
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
    g_autofree gchar *port = stix_private_tm_port_arg();
    QTestState *qts = stix_qtest_new_extra(port);

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
    g_autofree gchar *port = stix_private_tm_port_arg();
    QTestState *qts = stix_qtest_new_extra(port);
    QDict *resp;
    QList *props;
    const QListEntry *entry;
    g_autofree gchar *ctrl_path = NULL;
    g_autofree gchar *irqmp_path = NULL;
    int i;

    /* find the two devices under /machine/unattached by type, not by the
     * volatile device[N] names */
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

        if (g_str_equal(type, "child<grlib-stixctrl>")) {
            g_assert_null(ctrl_path);
            /* qom-list reports the child name; qom-get/qom-set want the
             * canonical path, and link values are canonical paths */
            ctrl_path = g_strdup_printf("/machine/unattached/%s", name);
        } else if (g_str_equal(type, "child<grlib-irqmp>")) {
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
        QTestState *qts = stix_qtest_new_extra(stix_private_tm_port_arg());
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

/* one test instance: private TM port plus a small ring (the default ring
 * costs ~53.5 MB of host RAM per instance) */
static QTestState *rotb_qtest_new(unsigned blocks)
{
    g_autofree gchar *port = stix_private_tm_port_arg();
    g_autofree gchar *extra = NULL;

    if (rotb_blocks_supported()) {
        extra = g_strdup_printf("%s -global grlib-stixctrl.rotb-blocks=%u",
                                port, blocks);
    } else {
        extra = g_strdup_printf("%s", port);
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
    g_autofree gchar *port = stix_private_tm_port_arg();
    QTestState *qts;
    gint64 deadline;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("needs TCG to run the boot stub");
        g_unlink(elf);
        return;
    }
    extra = g_strdup_printf("-accel tcg -kernel %s %s", elf, port);
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
    }

    return g_test_run();
}
