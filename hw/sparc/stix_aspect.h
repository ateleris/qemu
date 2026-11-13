/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * STIX IDPU aspect system model (aspect plan AS-1, D-ASa): the
 * semantics of asCmdSta (CTRL APB 0x68) and asQlData[4] (0x6C-0x7B,
 * StixIdpu.h:1643-1644) and the device reset. Internal to
 * grlib_stixctrl.c; not a QOM device (the runtime knobs of D-ASc/D-ASd
 * join the controller's class with the AS-2 generator).
 *
 * The FSW drives the registers through Aspect_setPower (channel
 * enables, bits 0/1, Aspect.c:80-96), Aspect_setSampleCountPerSum
 * (AVGSIZE 10:8, value 0..4 validated in the FSW, Aspect.c:108-118)
 * and Aspect_getQLData (the four 16-bit fields, Aspect.c:127-133); the
 * bit macros are StixIdpu.h:667-693.
 */

#ifndef HW_SPARC_STIX_ASPECT_H
#define HW_SPARC_STIX_ASPECT_H

#include "system/memory.h"
#include "hw/sparc/grlib_stix.h"
#include "StixIdpu.h"

/* the APB window this unit owns (as-model on) */
#define STIX_ASPECT_REG_FIRST 0x68u     /* asCmdSta */
#define STIX_ASPECT_QL_FIRST 0x6Cu      /* asQlData[0] */
#define STIX_ASPECT_REG_LAST 0x7Bu      /* asQlData[3] */

typedef struct StixAspectState {
    /*
     * Bindings into the owning STIXCTRL, set by stix_aspect_realize.
     * The registers stay in the controller's register file (one source
     * of truth, the D-Da pattern); this struct holds only derived and
     * internal state (the tick, rows and knobs of AS-2/AS-3).
     */
    stixCtrlRegs_s *reg;
    STIXCTRL *owner;

    /* as-model (qdev property): off keeps 0x68-0x7B plain storage */
    bool model;
} StixAspectState;

void stix_aspect_realize(StixAspectState *a, STIXCTRL *owner,
                         stixCtrlRegs_s *reg);
/* D-ASf: asCmdSta = 0, asQlData = 0, the unit state cleared */
void stix_aspect_reset(StixAspectState *a);
/* true when the model owns the APB offset (as-model on, 0x68-0x7B) */
bool stix_aspect_owns(StixAspectState *a, hwaddr addr);
uint32_t stix_aspect_read(StixAspectState *a, hwaddr addr);
void stix_aspect_write(StixAspectState *a, hwaddr addr, uint32_t value);

#endif
