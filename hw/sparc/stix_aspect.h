/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * STIX IDPU aspect system model (aspect plan AS-1/AS-2/AS-3, D-ASa):
 * the semantics of asCmdSta (CTRL APB 0x68) and asQlData[4]
 * (0x6C-0x7B, StixIdpu.h:1643-1644), the device reset, since AS-2 the
 * generator (a 1 ms diode tick, N-sample row sums with the AVGSIZE
 * latched at the row start, the 63-row double buffer that flips into
 * one 512 B ROTB block (descriptor 0x4000, LongTimer at the flip) per
 * 63*N ms) and since AS-3 the experiment knobs (jitter, step, drop).
 * Internal to grlib_stixctrl.c; not a QOM device (the runtime
 * knobs are class properties on the controller, the det-* pattern).
 *
 * The FSW drives the registers through Aspect_setPower (channel
 * enables, bits 0/1, Aspect.c:80-96), Aspect_setSampleCountPerSum
 * (AVGSIZE 10:8, value 0..4 validated in the FSW, Aspect.c:108-118)
 * and Aspect_getQLData (the four 16-bit fields, Aspect.c:127-133); the
 * bit macros are StixIdpu.h:667-693. The block the FSW decodes is one
 * ROTBUF_DESCRIPTOR_ASPECT_DATA block (RotAccumulator.h:16-31: 504 B
 * data, 6 B time, 63 samples per block).
 */

#ifndef HW_SPARC_STIX_ASPECT_H
#define HW_SPARC_STIX_ASPECT_H

#include "system/memory.h"
#include "qemu/timer.h"
#include "qobject/qdict.h"
#include "hw/sparc/grlib_stix.h"
#include "StixIdpu.h"

/* the APB window this unit owns (as-model on) */
#define STIX_ASPECT_REG_FIRST 0x68u     /* asCmdSta */
#define STIX_ASPECT_QL_FIRST 0x6Cu      /* asQlData[0] */
#define STIX_ASPECT_REG_LAST 0x7Bu      /* asQlData[3] */

/* the generator (D-ASb, the [F] hardware facts) */
#define STIX_ASPECT_ROWS 63u            /* rows per block (0x3F) */
#define STIX_ASPECT_DATA_OFF 8u         /* desc u16 + SCET 6 B */
#define STIX_ASPECT_DESC 0x4000u        /* aspect, block number 0 */
#define STIX_ASPECT_TICK_NS 1000000LL   /* the fixed 1 kHz diode tick */
#define STIX_ASPECT_PPM_MAX 5000        /* as-rate-ppm bound (D-ASd) */
#define STIX_ASPECT_STEP_MS_MAX 32      /* jitter/step ms bound (D-ASd) */

/* as-pattern (D-ASc) */
#define STIX_ASPECT_PATTERN_ZERO 0u
#define STIX_ASPECT_PATTERN_RAMP 1u
#define STIX_ASPECT_PATTERN__MAX 2u

typedef struct StixAspectState {
    /*
     * Bindings into the owning STIXCTRL, set by stix_aspect_realize.
     * The registers stay in the controller's register file (one source
     * of truth, the D-Da pattern); this struct holds only derived and
     * internal state (the tick, rows and knobs).
     */
    stixCtrlRegs_s *reg;
    STIXCTRL *owner;

    /* as-model (qdev property): off keeps 0x68-0x7B plain storage */
    bool model;

    /* the tick (D-ASb): 1 ms of virtual time per enabled-channel
     * sample, absolute deadlines, started by the first channel power-on
     * and stopped by the reset (D-ASf) */
    QEMUTimer *tick;
    bool tick_on;
    int64_t tick_deadline_ns;

    /* the 63-row double buffer and the row accumulator */
    uint16_t page[2][STIX_ASPECT_ROWS][4];  /* A0, A1, B0, B1 per row */
    uint32_t page_fill;             /* the page being filled (0/1) */
    uint32_t row;                   /* rows completed in the open page */
    uint32_t row_samples;           /* samples drawn into the open row */
    uint32_t row_n;                 /* AVGSIZE latched at the row start */
    uint32_t acc[4];                /* the open row's per-diode sum */
    uint8_t blk[STIX_ASPECT_DATA_OFF + STIX_ASPECT_ROWS * 8u];

    /* counters and the last flip (D-ASg) */
    uint32_t block_seq;             /* flips performed */
    uint32_t pushed;
    uint32_t dropped;               /* not appended: gating, full, knobs */
    uint32_t dropped_knobs;         /* the as-drop-next subset of dropped */
    uint32_t rotb_full;             /* the ring-full subset of dropped */
    uint32_t jittered;              /* blocks that carried the one-shot */
    uint32_t stepped;               /* blocks that carry the step */
    uint32_t flip_coarse;           /* SCET reg sampled at the last flip */
    uint16_t flip_fine;

    /* the scene knobs (D-ASc/D-ASd): survive a reset */
    uint32_t pattern;               /* STIX_ASPECT_PATTERN_* */
    uint32_t sun;                   /* 0..0xFFFF, clamped into 12 bits */
    int32_t rate_ppm;               /* -5000..5000, >= 0 at N = 1 */
    bool enable;                    /* false = no blocks at all */

    /* the experiment knobs (AS-3, D-ASd): state, not scene — the reset
     * clears them (D-ASf) */
    int32_t jitter_ms;              /* the pending one-shot, +-32 */
    int32_t step_ms;                /* the persistent step, +-32 */
    uint32_t step_at;               /* as-step-at-block: 0 = the next flip */
    uint32_t step_start;            /* the resolved first stepped seq */
    bool step_fired;                /* the boundary trace has fired */
    uint32_t drop_next;             /* flips left to not append */
} StixAspectState;

void stix_aspect_instance_init(StixAspectState *a);
void stix_aspect_realize(StixAspectState *a, STIXCTRL *owner,
                         stixCtrlRegs_s *reg);
void stix_aspect_unrealize(StixAspectState *a);
/* D-ASf: the tick stops, asCmdSta = 0, asQlData = 0, the unit state
 * cleared; the scene knobs survive */
void stix_aspect_reset(StixAspectState *a);
/* true when the model owns the APB offset (as-model on, 0x68-0x7B) */
bool stix_aspect_owns(StixAspectState *a, hwaddr addr);
uint32_t stix_aspect_read(StixAspectState *a, hwaddr addr);
void stix_aspect_write(StixAspectState *a, hwaddr addr, uint32_t value);
/* the runtime class properties on /machine/stixctrl (the det-* pattern) */
void stix_aspect_class_init(ObjectClass *klass, size_t as_offset);
/* the hw-state "as" section (D-ASg), one accessor, pure */
void stix_aspect_hw_state(StixAspectState *a, QDict *as);

#endif
