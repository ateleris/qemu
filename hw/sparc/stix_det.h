/*
 * STIX IDPU detector controller model (detector plan D-Da): the readout
 * FSM, the QL accumulator bank switch and IRQ 3 (D-1), and the IDeF-X
 * serial interface with its ASIC register store (D-2), behind CTRL's
 * APB offsets 0x00-0x4C. Internal to grlib_stixctrl.c; not a QOM device.
 *
 * Index spaces: every detector-related name carries its space
 * (fpga_det, grp, asic, quarter, asic_ch, pix, subcol, fe_sc, ebin;
 * detector plan "Index spaces"). The one conversion table set, copied
 * from the FSW, lives in stix_det.c and is self-checked at realize.
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

/* index space sizes (detector plan "Index spaces") */
#define STIX_DET_N_FPGA_DET 32u     /* fpga_det 0..31 = 2 * grp + asic */
#define STIX_DET_N_GRP 16u          /* grp 0..15 = fpga_det / 2 */
#define STIX_DET_N_QUARTER 4u       /* quarter 0..3 = grp / 4 */
#define STIX_DET_N_ASIC 2u          /* asic 0/1 in a grp (2 = All) */
#define STIX_DET_N_ASIC_CH 32u      /* asic_ch 0..31 */
#define STIX_DET_N_PIX 13u          /* pix 0..11, 12 = guard ring */
#define STIX_DET_N_SUBCOL 32u       /* subcol 0..31 (ICD number - 1) */

/* IDeF-X slow-control register addresses 0..13 (1..12 stored) */
#define STIX_DET_IDEFX_N_REG 14u
#define STIX_DET_IDEFX_REG_BYTES 24u    /* R3: 192 bits */

/* one grp's slow-control transaction decoder (D-De) */
typedef struct StixIdefxDecoder {
    uint8_t phase;      /* STIX_IDEFX_IDLE, _SC_ADDR, _SC_DATA */
    bool all;           /* INIT All = 1: both asic */
    int8_t asic;        /* the addressed asic, -1 for none */
    bool read;          /* address block R/W = 1 */
    uint8_t reg;        /* address block register 0..127 */
    uint8_t block_idx;  /* data blocks so far: the next byte to read */
    uint8_t adc_ctx;    /* STIX_IDEFX_ADC_*: the cmd 1 answer context */
} StixIdefxDecoder;

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

    /* det-idefx (qdev property): false keeps 0x00-0x0C plain storage */
    bool idefx;
    /*
     * The IDeF-X register store, indexed by fpga_det and register
     * address: each register is an N-bit shift register, held
     * right-aligned in its (N + 7) / 8 bytes. R3 is held raw, in wire
     * order (the FSW reorders it on both sides).
     */
    uint8_t idefx_reg[STIX_DET_N_FPGA_DET][STIX_DET_IDEFX_N_REG]
                     [STIX_DET_IDEFX_REG_BYTES];
    StixIdefxDecoder idefx_dec[STIX_DET_N_GRP];
} StixDetState;

bool stix_det_realize(StixDetState *d, stixCtrlRegs_s *reg,
                      MemoryRegion *sram, MemoryRegion *lvar,
                      MemoryRegion *ltrg, qemu_irq *irq, Error **errp);
void stix_det_unrealize(StixDetState *d);
void stix_det_reset(StixDetState *d);

/* true when the model owns the APB offset (det-model on, 0x00-0x4C) */
bool stix_det_owns(StixDetState *d, hwaddr addr);
uint32_t stix_det_read(StixDetState *d, hwaddr addr);
/* false when the write was dropped (a read-only register) */
bool stix_det_write(StixDetState *d, hwaddr addr, uint32_t value);

#endif
