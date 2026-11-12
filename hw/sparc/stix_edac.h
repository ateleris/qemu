/*
 * STIX IDPU EDAC unit (memory-upset plan D-Ua): the SEC counters and
 * DeDetected latches of the three EDAC memories (SDRAM, CACHE, ROTBUF),
 * composed into the register owners' readbacks, plus the edac-at-boot
 * hand-over state (D-Uk). M-1: register semantics only, no upsets — the
 * counters only ever move through the write-1 strobes (which clear) and
 * the test-latch properties; M-2 adds the upset paths.
 *
 * The unit is a QOM child of the stixctrl device at
 * /machine/stixctrl/edac (object_initialize_child in the stixctrl
 * instance_init). It is not on a bus: its reset is a qemu_register_reset
 * hook, and the register owners (grlib_stixctrl.c, grlib_stixsdram.c)
 * compose and clear through the calls below.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SPARC_STIX_EDAC_H
#define HW_SPARC_STIX_EDAC_H

#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "qom/object.h"
#include "system/memory.h"

#define TYPE_STIX_EDAC "stix-edac"

OBJECT_DECLARE_SIMPLE_TYPE(StixEdac, STIX_EDAC)

/*
 * The three EDAC memories (FSW EDAC.h:59-67; EEPROM is out of scope, its
 * register keeps today's plain-storage behaviour).
 */
typedef enum StixEdacModule {
    STIX_EDAC_SDRAM,
    STIX_EDAC_CACHE,
    STIX_EDAC_ROTBUF,
    STIX_EDAC_N_MODULE,
} StixEdacModule;

/* the EDAC word (32 bit) and the overlay granularity (D-Uc) */
#define STIX_EDAC_WORD_SIZE 4u
/* = TARGET_PAGE_SIZE on sparc: an overlay never splits a TCG page */
#define STIX_EDAC_PAGE_SIZE 0x1000u
/* the poison-table capacity; full is refused in upset-inject */
#define STIX_EDAC_N_POISON 256u

typedef struct StixEdacOverlay StixEdacOverlay;

/*
 * One EDAC memory's upset state (D-Ua): the backing RAM the overlays sit
 * on, the owner's stored register word that gates the reactions (EDACON,
 * and for CACHE OPMODE), the DE IRQ line, and the upsets-active
 * counters. Only CACHE is linked in M-2; SDRAM follows in M-3.
 */
typedef struct StixEdacMem {
    MemoryRegion *backing;      /* the guest-mapped RAM (stixctrl->sram) */
    hwaddr base;                /* the guest address of backing offset 0 */
    volatile uint32_t *edacon_reg; /* the owner's word with EDACON */
    qemu_irq *irq;              /* the module's DE line (sysbus slot) */
    StixEdacOverlay *overlays;  /* live 4 KB page overlays */
    StixEdacOverlay *spares;    /* removed overlays, parked for reuse */
    /* upsets-active counters (D-Ug): injects and their effects */
    uint32_t injected;
    uint32_t corrected;
    uint32_t detected;
} StixEdacMem;

/*
 * One armed upset (D-Uc): the 32-bit EDAC word at @addr reads with @mask
 * XORed. The mode derives from the mask (1 bit = SEU, >= 2 = DEU/MBU)
 * unless the upset-inject string overrides it; persist=1 keeps the
 * poison after its first effect (D-Uh).
 */
typedef struct StixEdacPoison {
    StixEdacModule mem;
    uint32_t addr;              /* guest byte address, word-aligned */
    uint32_t mask;              /* XOR mask, nonzero */
    bool de;                    /* true = DEU/MBU, false = SEU */
    bool persist;               /* survive the first effect */
    bool armed;                 /* false = a free slot */
} StixEdacPoison;

struct StixEdac {
    DeviceState parent_obj;

    /*
     * edac-at-boot (D-Uk): "hw-reset" (the ICD reset values), "all-on",
     * or a 3-bit mask (1 = sdram, 2 = cache, 4 = rotb). The register
     * owners apply it at every device reset; the EDAC logic itself only
     * ever reacts to the live EDACON bit.
     */
    char *boot_str;
    uint32_t boot_mask;

    /*
     * D-Ue: the 8-bit SEC counters (saturating once M-2 counts) and the
     * DeDetected latches, per module. Composed on read; the strobes
     * clear them.
     */
    uint8_t sec[STIX_EDAC_N_MODULE];
    bool ded[STIX_EDAC_N_MODULE];

    /*
     * M-2 (D-Uc/D-Ug): the poison table and the sparse page overlays.
     * The poisons and the overlays survive a system reset (D-Uh: a
     * reboot does not heal a cell); the counters are host-side
     * statistics and survive too.
     */
    StixEdacMem mems[STIX_EDAC_N_MODULE];
    StixEdacPoison poison[STIX_EDAC_N_POISON];
    uint32_t n_armed;

    /*
     * M-3 (D-Ud): the AHBSTAT failing-address word (0x80000F00) — the
     * address of the last SDRAM MEMTX_ERROR read. Zeroed at power-on,
     * latched across a system reset [I]: the trap handler reads it
     * before the watchdog reset, and the post-reboot record is the BRAM
     * copy (WDT.c:86-126), not this register.
     */
    uint32_t ahbstat;
    MemoryRegion ahbstat_mr;
};

/*
 * The EDACON state a module's register owner applies at every device
 * reset (D-Uk), true when the module's EDAC is on at hand-over. NULL
 * (no unit linked) falls back to the ICD reset values.
 */
bool stix_edac_boot_edacon(StixEdac *e, StixEdacModule m);

/* Clear one module's SEC counter and DeDetected latch (the strobes). */
void stix_edac_clear(StixEdac *e, StixEdacModule m);

/*
 * Sdram:Edac (0x80000C04): SECCOUNT 7:0 and DEDETECTED 31 composed on
 * read, EDACON/SCRUBRATE stored; a write drops the composed fields and
 * the EDACRST strobe (bit 13) clears counter and latch, self-clearing.
 */
uint32_t stix_edac_sdram_read(StixEdac *e, uint32_t stored);
uint32_t stix_edac_sdram_write(StixEdac *e, uint32_t stored, uint32_t value);

/*
 * CacheCmdSta (0x80000550): SECCOUNT 15:8 and DEDETECTED 18 composed on
 * read, OPMODE/SCRUBRATE/EDACON stored; the EDACRESET strobe (bit 17)
 * clears counter and latch, self-clearing.
 */
uint32_t stix_edac_cache_read(StixEdac *e, uint32_t stored);
uint32_t stix_edac_cache_write(StixEdac *e, uint32_t stored, uint32_t value);

/*
 * RotbCmdSta (0x80000554): the composed bits only (SECCOUNT 31:24,
 * DEDETECTED 4); the 0x54 write filter in grlib_stixctrl.c treats bit 2
 * as the strobe and calls stix_edac_clear().
 */
uint32_t stix_edac_rotb_status(StixEdac *e);

/*
 * M-2 (D-Uc): wire the CACHE memory to the unit, at the register
 * owner's realize. @cache_cmdsta is the stored word whose EDACON (bit
 * 16) and OPMODE (bit 0) gate the overlay reactions, @sram the backing
 * RAM the overlays sit on (STIXCTRL_SRAM_SIZE at STIXCTRL_SRAM_START),
 * @edac_irq the sysbus IRQ slot that pulses IRQMP line 6 (D-Ue).
 */
void stix_edac_cache_link(StixEdac *e, volatile uint32_t *cache_cmdsta,
                          MemoryRegion *sram, qemu_irq *edac_irq);

/*
 * M-3 (D-Uc): wire the SDRAM memory to the unit, at the register
 * owner's realize. @sdram_edac is the stored Sdram:Edac word whose
 * EDACON (bit 12) gates the overlay reactions, @ram the machine RAM the
 * overlays sit on (mapped at LEON3_RAM_OFFSET = 0x40000000, leon3.c),
 * which is also the poison address base. No IRQ: the SDRAM DE reports
 * as an AHB error (trap 0x09, D-Ud), not on the shared line
 * (EDAC.c:69-76).
 */
void stix_edac_sdram_link(StixEdac *e, volatile uint32_t *sdram_edac,
                          MemoryRegion *ram);

/*
 * M-3 (D-Ud): the AHBSTAT window's MemoryRegion (4 bytes at
 * 0x80000F00); leon3.c maps it into the system memory.
 */
MemoryRegion *stix_edac_ahbstat_mr(StixEdac *e);

/*
 * M-2 (D-Uc, the host-side writer rule): a host write into a linked
 * memory (stix_det's LACC/EACC materialization) retires every poison it
 * overwrites, like a guest write — a rewrite cures the cell. @off and
 * @len are byte offsets in the backing RAM.
 */
void stix_edac_host_write(StixEdac *e, StixEdacModule m, hwaddr off,
                          hwaddr len);

#endif /* HW_SPARC_STIX_EDAC_H */
