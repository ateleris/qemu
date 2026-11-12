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

#include "hw/core/qdev.h"
#include "qom/object.h"

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

#endif /* HW_SPARC_STIX_EDAC_H */
