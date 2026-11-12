/*
 * STIX IDPU detector controller model (detector plan D-Da): the readout
 * FSM, the QL accumulator bank switch and IRQ 3 (D-1), the IDeF-X
 * serial interface with its ASIC register store (D-2), and the
 * synthetic sky with the flare switch (D-4), and the nominal integration
 * with its ROTB science chunks (D-5), and the DSS .dssevs replay (D-7),
 * behind CTRL's APB offsets 0x00-0x4C. Internal to grlib_stixctrl.c;
 * not a QOM device (its runtime scene properties are added to the
 * controller's class by stix_det_class_init).
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
#include "qom/object.h"
#include "hw/sparc/grlib_stix.h"
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
#define STIX_DET_N_EBIN 32u         /* ebin 0..31 (science energy bin) */
#define STIX_DET_N_ADC 4096u        /* adc 0..4095 (raw 12-bit ADC value) */
/* pix 0..11 are accumulated; the guard ring (pix 12) never is */
#define STIX_DET_N_PIX_ACC 12u

/*
 * D-4/D-7 (D-Db det-source): where the counts come from. replay-dss
 * feeds the pre-scanned .dssevs table instead of the synthetic sky.
 */
typedef enum StixDetSource {
    STIX_DET_SOURCE_ZERO,
    STIX_DET_SOURCE_SYNTHETIC,
    STIX_DET_SOURCE_REPLAY_DSS,
    STIX_DET_SOURCE__MAX,
} StixDetSource;

/*
 * D-7 (D-Dk): when a loaded replay starts. nominal = the first fsmCmd
 * SetEnable 0->1 edge after a reset (NOMINAL entry); qmp = only a
 * det-replay-start=true.
 */
typedef enum StixDetReplayTrigger {
    STIX_DET_REPLAY_TRIGGER_NOMINAL,
    STIX_DET_REPLAY_TRIGGER_QMP,
    STIX_DET_REPLAY_TRIGGER__MAX,
} StixDetReplayTrigger;

/*
 * D-7 (D-Dk): one pre-scanned .dssevs event. t_tick is the replay time
 * in 100 ms ticks (the file's waits, summed); fpga_det = dss_det (the
 * identity of plan "Index spaces"); pix is the word's type field
 * 0..11; adc the raw 12-bit value.
 */
typedef struct StixDetReplayEvent {
    uint32_t t_tick;
    uint8_t fpga_det;
    uint8_t pix;
    uint16_t adc;
} StixDetReplayEvent;

/* D-7: the event table's cap, 64 Mi events = 512 MiB of table */
#define STIX_DET_REPLAY_MAX_EVENTS (1u << 26)

/* D-4 (D-Df): the synthetic sky's two spectral profiles */
enum { STIX_DET_PROFILE_QUIET, STIX_DET_PROFILE_FLARE, STIX_DET_N_PROFILE };

/*
 * D-5 (D-Dg): one detector chunk in the ROTB, 49 blocks of
 * STIXCTRL_BLOCK_SIZE (RotAccumulator.h LAST_DETECTOR_BLOCK_NUMBER 48)
 */
#define STIX_DET_CHUNK_BLOCKS 49u
#define STIX_DET_NOM_BANKS 2u       /* EACC0/1, ETRG0/1 */

/* what the ROTB did with a chunk (stixctrl_rotb_append) */
typedef enum StixDetRotbResult {
    STIX_DET_ROTB_APPENDED,
    STIX_DET_ROTB_DISABLED,     /* rotbConfig ENABLE = 0 */
    STIX_DET_ROTB_TESTMODE,     /* rotbConfig TESTMODE = 1 */
    STIX_DET_ROTB_FULL,         /* fewer free blocks than the chunk */
} StixDetRotbResult;

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
    MemoryRegion *etrg;         /* ETRG0-1 (D-5) */
    qemu_irq *irq;              /* IRQMP line 3 (IRQ_STIXCTRL_DETCTRL) */
    /*
     * the owning controller: its ROTB ring takes the chunks, its SCET
     * stamps them, and its mem_reentrancy_guard covers the tick (D-Dj)
     */
    STIXCTRL *owner;

    /* det-model (qdev property): false keeps the legacy plain storage */
    bool model;

    /* readout FSM status (fsmSta is composed from these on read) */
    bool enable;
    bool ql_ready;
    bool cal_enable;
    uint32_t active_ql_bank;    /* fsmSta.ActiveQl, 0..3 */
    uint32_t sumcnt;            /* fsmEvents: SUMCNT, saturating */

    /* the 100 ms tick; runs only while enable */
    QEMUTimer *tick;
    int64_t tick_deadline_ns;   /* absolute, so the tick does not drift */
    uint32_t ql_tick;           /* ticks done in the current QL period */
    uint32_t ql_int_time;       /* QlIntTime latched for this period */
    uint32_t lvar_tick_count;   /* this tick's LVAR count */

    /*
     * D-5 (D-Dg): the nominal integration. ActiveNominal toggles at each
     * nominal swap; nom_tick counts the ticks of the running
     * integration; its parameters (fsmIntTime min/max, fsmIntEvents) are
     * latched at its start.
     */
    uint32_t active_nom_bank;   /* fsmSta.ActiveNominal, 0..1 */
    uint32_t nom_tick;
    uint32_t nom_int_time_min;
    uint32_t nom_int_time_max;
    uint32_t nom_int_events_max;
    /* det-rotb (qdev property): false makes no chunks (the swap stays) */
    bool rotb;

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

    /*
     * D-4 (D-Db): the scene, set over QMP at run time. It describes the
     * Sun, not the IDPU, so a device reset keeps it.
     */
    uint32_t source;            /* StixDetSource */
    bool flare;                 /* the flare switch */
    uint32_t quiet_cps;         /* counts/s per fpga_det, quiet */
    uint32_t flare_cps;         /* counts/s per fpga_det, flare */
    /* det-seed (qdev property): the PRNG seed, applied at every reset */
    uint32_t seed;
    uint64_t prng;              /* xorshift64* state */
    /*
     * D-Df exposure of the active QL period: the expected counts of each
     * (fpga_det, pix) so far, per profile, summed over the ticks in
     * which that pair was gated on (rate x 100 ms x the pix share). At
     * the swap each LACC cell is drawn as Poisson(sum over profiles of
     * expo x the profile's ebin share).
     */
    double expo[STIX_DET_N_FPGA_DET][STIX_DET_N_PIX_ACC][STIX_DET_N_PROFILE];
    /*
     * D-5: the exposure of the running nominal integration, as expo but
     * only for the ticks in which the fpga_det was in SUMDMASK (EACC is
     * gated by it); nom_trg_extra_grp holds, per grp, the expected
     * events of read-out fpga_det outside SUMDMASK, which count in ETRG
     * only (the triggers are not masked [I]).
     */
    double nom_expo[STIX_DET_N_FPGA_DET][STIX_DET_N_PIX_ACC]
                   [STIX_DET_N_PROFILE];
    double nom_trg_extra_grp[STIX_DET_N_GRP];
    /*
     * D-6 (D-Db det-inject): the pending one-shot deposit request, stored
     * by the QMP setter (D-Dj: fields only) and applied by the next tick
     * through stix_det_add_counts. A device reset drops it.
     */
    bool inject_pending;
    uint32_t inject_fpga_det;
    uint32_t inject_pix;
    uint32_t inject_ebin;
    uint32_t inject_n;
    /*
     * D-6 (D-Df event sink): the injected counts per cell, on top of the
     * drawn sky, so a deposit lands at its own (pix, fpga_det, ebin) and
     * not through the template shares. inj_ql materializes into the
     * finished bank's LACC at the next QL swap, inj_nom into EACC (and so
     * the next chunk) at the next nominal swap; both are consumed (and
     * cleared with the exposure) at their swap.
     */
    uint32_t inj_ql[STIX_DET_N_FPGA_DET][STIX_DET_N_PIX_ACC]
                   [STIX_DET_N_EBIN];
    uint32_t inj_nom[STIX_DET_N_FPGA_DET][STIX_DET_N_PIX_ACC]
                    [STIX_DET_N_EBIN];
    /* the chunk being built at a nominal swap */
    uint8_t chunk[STIX_DET_CHUNK_BLOCKS * STIXCTRL_BLOCK_SIZE];
    /*
     * D-7 (D-Dk): the DSS .dssevs replay. The table is the realize
     * pre-scan of det-replay-file; the trigger and offset are scene
     * knobs (they survive a device reset), a running replay does not
     * (it stops and re-arms on its trigger).
     */
    char *replay_file;          /* det-replay-file (qdev, init-time) */
    uint32_t replay_trigger;    /* StixDetReplayTrigger (scene knob) */
    uint64_t replay_offset_ms;  /* det-replay-offset-ms, file ms (scene) */
    bool replay_start_req;      /* det-replay-start=true, applied next tick */
    bool replay_running;        /* det-replay-start reads it */
    bool replay_armed;          /* the nominal trigger is pending */
    bool replay_stopped;        /* det-replay-start=false stopped it */
    bool replay_ended;          /* EOF reached (det-replay-status) */
    bool replay_class_pending;  /* re-classify the ELUT rows next tick */
    bool replay_loaded;         /* a table was scanned at realize */
    StixDetReplayEvent *replay_ev;
    uint32_t replay_n_ev;
    uint32_t replay_alloc;
    uint32_t replay_cursor;     /* next table entry to feed */
    uint32_t replay_time_tick;  /* replay time, 100 ms ticks from 0 */
    uint64_t replay_events_done;
    uint64_t replay_dropped;    /* events the D-6 gate dropped (status) */
    uint64_t replay_unsupported; /* 0xD setup words, not implemented */
    /* baseline per dss_det (= fpga_det) from the file's 0xE setup words */
    uint32_t replay_base_dss_det[STIX_DET_N_FPGA_DET];
    /* the ELUT row (fpga_det, pix) is degenerate: linear fallback */
    uint8_t replay_row_fallback[STIX_DET_N_FPGA_DET][STIX_DET_N_PIX_ACC];
    /* the template (D-Df), built at realize: pix share and ebin shares */
    double pix_share[STIX_DET_N_PIX_ACC];
    double ebin_share[STIX_DET_N_PROFILE][STIX_DET_N_EBIN];
} StixDetState;

bool stix_det_realize(StixDetState *d, STIXCTRL *owner, stixCtrlRegs_s *reg,
                      MemoryRegion *sram, MemoryRegion *lvar,
                      MemoryRegion *ltrg, MemoryRegion *etrg, qemu_irq *irq,
                      Error **errp);
void stix_det_unrealize(StixDetState *d);
void stix_det_reset(StixDetState *d);
/* the scene defaults, once per object (instance_init; not on reset) */
void stix_det_instance_init(StixDetState *d);
/*
 * The runtime scene properties (det-source, det-flare, det-quiet-cps,
 * det-flare-cps, det-replay-trigger, det-replay-offset-ms,
 * det-replay-start, and the read-only det-replay-status) on the
 * owning device class; @det_offset is offsetof(owner, det), so the
 * owner's struct stays private
 */
void stix_det_class_init(ObjectClass *klass, size_t det_offset);

/* true when the model owns the APB offset (det-model on, 0x00-0x4C) */
bool stix_det_owns(StixDetState *d, hwaddr addr);
/*
 * The D-Df event sink (D-6 det-inject; the D-7 replay feeds it too):
 * add @n integer counts at (pix @pix, fpga_det @fpga_det, ebin @ebin).
 * Call it from tick context; the counts reach the LACC of the finished
 * bank at the next QL swap and EACC at the next nominal swap.
 */
void stix_det_add_counts(StixDetState *d, uint32_t fpga_det, uint32_t pix,
                         uint32_t ebin, uint32_t n);
uint32_t stix_det_read(StixDetState *d, hwaddr addr);
/* false when the write was dropped (a read-only register) */
bool stix_det_write(StixDetState *d, hwaddr addr, uint32_t value);

/*
 * Provided by grlib_stixctrl.c for the detector model (D-5):
 * - stixctrl_scet_reg: the SCET as 0xC8 (UNSYNCED in bit 31) and 0xCC
 *   read right now;
 * - stixctrl_rotb_append: append @n_blocks blocks of STIXCTRL_BLOCK_SIZE
 *   to the ROTB ring if rotbConfig has ENABLE = 1 and TESTMODE = 0 and
 *   all of them fit; a chunk that does not fit is dropped whole and
 *   pulses IRQ 7 once. *rotb_count is the block count afterwards.
 */
void stixctrl_scet_reg(STIXCTRL *s, uint32_t *coarse_reg, uint16_t *fine);
StixDetRotbResult stixctrl_rotb_append(STIXCTRL *s, const uint8_t *blocks,
                                       uint32_t n_blocks,
                                       uint32_t *rotb_count);

#endif
