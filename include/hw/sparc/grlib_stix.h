/*
 * STIX IDPU emulation device type declarations.
 *
 * STIX-local, not for upstream: the IDPU devices emulated for the Solar
 * Orbiter STIX instrument. Since QEMU's per-peripheral GRLIB header split
 * (January 2024) the historic single grlib.h no longer carries device
 * types; the STIX types live here instead.
 *
 * See docs/stix-port/baseline/README.md for the baseline these preserve.
 */

#ifndef HW_SPARC_GRLIB_STIX_H
#define HW_SPARC_GRLIB_STIX_H

/* STIX FLASH (FTNANDCTRL register emulation) */
#define TYPE_GRLIB_STIXFLASH "grlib,stixflash"

/* STIX SPW (GRSPW register emulation + TMTC TCP bridge) */
#define TYPE_GRLIB_STIXSPW "grlib,stixspw"

/* STIX WD (compiled but not instantiated on the baseline machine) */
#define TYPE_GRLIB_STIXWD "grlib,stixwd"

/* STIX CTRL (detector/HK/EDAC/ROTB controller registers) */
#define TYPE_GRLIB_STIXCTRL "grlib,stixctrl"

/* STIX SDRAM controller register stub (model repair commit) */
#define TYPE_GRLIB_STIXSDRAM "grlib,stixsdram"

#endif /* HW_SPARC_GRLIB_STIX_H */
