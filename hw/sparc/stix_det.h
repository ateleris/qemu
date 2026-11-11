/*
 * STIX IDPU detector controller model (detector plan D-Da): the readout
 * FSM, the QL accumulator bank switch and IRQ 3 behind CTRL's APB
 * offsets 0x00-0x4C. Internal to grlib_stixctrl.c; not a QOM device.
 *
 * Index spaces: every detector-related name carries its space
 * (fpga_det, pix, ebin, grp, ...; detector plan "Index spaces"). The
 * tier-0 model has none yet: it only moves QL banks (ql_bank 0..3).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SPARC_STIX_DET_H
#define HW_SPARC_STIX_DET_H

#include "system/memory.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"
#include "StixIdpu.h"

/* the last detector register: fsmCalRes (StixIdpu.h stixCtrlRegs_s) */
#define STIX_DET_REG_LAST 0x4Cu

typedef struct StixDetState {
    /*
     * Bindings into the owning STIXCTRL, set by stix_det_realize. The
     * registers stay in the controller's register file (one source of
     * truth: stixctrl_hk_value reads idefxConfig there); this struct
     * holds only derived and internal state.
     */
    stixCtrlRegs_s *reg;
    MemoryRegion *sram;         /* ELUT, LACC0-3 (and TLUT, EACC) */
    MemoryRegion *lvar;         /* LVAR0-3, shared with WDT/MemBram BRAM */
    MemoryRegion *ltrg;         /* LTRG0-3 */
    qemu_irq *irq;              /* IRQMP line 3 (IRQ_STIXCTRL_DETCTRL) */

    /* det-model (qdev property): false keeps the legacy plain storage */
    bool model;

    /* readout FSM status (fsmSta is composed from these on read) */
    bool enable;
    bool ql_ready;
    bool cal_enable;
    uint32_t active_ql_bank;    /* fsmSta.ActiveQl, 0..3 */
    uint32_t sumcnt;            /* fsmEvents; no count source before D-4 */

    /* the 100 ms tick; runs only while enable */
    QEMUTimer *tick;
    int64_t tick_deadline_ns;   /* absolute, so the tick does not drift */
    uint32_t ql_tick;           /* ticks done in the current QL period */
    uint32_t ql_int_time;       /* QlIntTime latched for this period */
    uint32_t lvar_tick_count;   /* this tick's LVAR count (D-4 adds) */
} StixDetState;

void stix_det_realize(StixDetState *d, stixCtrlRegs_s *reg,
                      MemoryRegion *sram, MemoryRegion *lvar,
                      MemoryRegion *ltrg, qemu_irq *irq);
void stix_det_unrealize(StixDetState *d);
void stix_det_reset(StixDetState *d);

/* true when the model owns the APB offset (det-model on, 0x00-0x4C) */
bool stix_det_owns(StixDetState *d, hwaddr addr);
uint32_t stix_det_read(StixDetState *d, hwaddr addr);
/* false when the write was dropped (a read-only register) */
bool stix_det_write(StixDetState *d, hwaddr addr, uint32_t value);

#endif
