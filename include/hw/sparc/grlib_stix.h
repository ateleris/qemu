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

#include "qom/object.h"

/* STIX FLASH (FTNANDCTRL register emulation) */
#define TYPE_GRLIB_STIXFLASH "grlib-stixflash"

/* STIX SPW (GRSPW register emulation + TMTC TCP bridge) */
#define TYPE_GRLIB_STIXSPW "grlib-stixspw"

/* STIX WD (compiled but not instantiated on the baseline machine) */
#define TYPE_GRLIB_STIXWD "grlib-stixwd"

/* STIX CTRL (detector/HK/EDAC/ROTB controller registers) */
#define TYPE_GRLIB_STIXCTRL "grlib-stixctrl"

/* STIX SDRAM controller register stub (model repair commit) */
#define TYPE_GRLIB_STIXSDRAM "grlib-stixsdram"

/*
 * The CTRL instance type lives here (correction 7) so the SPW device
 * can hold a typed scet-source link to it; the struct itself stays in
 * grlib_stixctrl.c.
 */
OBJECT_DECLARE_SIMPLE_TYPE(STIXCTRL, GRLIB_STIXCTRL)

/*
 * D-J: the SCET the CTRL device would report right now — the same
 * formula as the 0xC8/0xCC register read, without a guest access.
 * *coarse is the seconds field without the UNSYNCED status bit.
 */
void stixctrl_scet_now(STIXCTRL *s, uint32_t *coarse, uint16_t *fine);

/*
 * D-J / correction 6: a callback invoked after the guest commits a new
 * on-board time (the CTRL 0xCC write), so the SPW time-code timer can
 * re-arm at the moved rollover. Single subscriber: the last
 * registration replaces the previous one (the machine has one SPW).
 */
typedef void (*StixCtrlScetCommitFn)(void *opaque);
void stixctrl_set_scet_commit_hook(STIXCTRL *s,
                                   StixCtrlScetCommitFn fn, void *opaque);

/*
 * A-2 (D-Af): hkAdc ch 8 (ATT_C) through the PL-2a split, for the
 * attenuator child's truth.att-c / presented.att-c getters: the
 * mechanism's draw, and the value the FSW converts (adc-inject[8]
 * forcing included).
 */
uint32_t stixctrl_hk_att_c_truth(STIXCTRL *s);
uint32_t stixctrl_hk_att_c_presented(STIXCTRL *s);

/* The IDPU's custom devices were (deliberately, like-for-like with the
 * 2022 model) registered in the AMBA PnP table under the FTMCTRL device
 * id — drvmgr sees three FTMCTRLs. Pinned by the PnP qtest. */
#define GRLIB_FTMCTRL_DEV (0x054)

#endif /* HW_SPARC_GRLIB_STIX_H */
