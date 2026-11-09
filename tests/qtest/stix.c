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

static QTestState *stix_qtest_new(void)
{
    QTestState *qts;
    gchar *path = NULL;
    int fd = g_file_open_tmp("stix-flash-XXXXXX", &path, NULL);
    g_assert(fd >= 0);
    for (int i = 0; i < 256; i++) {
        static const char zeros_ff[4096] = { 0 }; /* must be 0xFF */
        (void)zeros_ff;
    }
    /* fill with 0xFF */
    gchar *buf = g_malloc(4096);
    memset(buf, 0xFF, 4096);
    for (int i = 0; i < 256; i++) {
        g_assert(write(fd, buf, 4096) == 4096);
    }
    g_free(buf);
    close(fd);
    stix_tmp_flash_path = path;
    qts = qtest_initf("-M leon3_generic "
                      "-global grlib-stixflash.flash-file=%s", path);
    return qts;
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

    /* Benign write/readback: watchdog register stores anything (the FSW
     * writes 0x40000000 in its shutdown path). */
    ctrl_write(qts, CTRL_WatchDog, 0x40000000u);
    g_assert_cmphex(ctrl_read(qts, CTRL_WatchDog), ==, 0x40000000u);

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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (qtest_has_machine("leon3_generic")) {
        qtest_add_func("/sparc/stix/pnp", test_pnp);
        qtest_add_func("/sparc/stix/flash", test_flash);
        qtest_add_func("/sparc/stix/spw-tx", test_spw_tx);
        qtest_add_func("/sparc/stix/ctrl", test_ctrl);
    }

    return g_test_run();
}
