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
#include "qobject/qdict.h"
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

/* the mix tape (S1): at most 64 steps, at most 64 KiB of JSON */
#define STIX_DET_TAPE_MAX_STEPS 64u
#define STIX_DET_TAPE_MAX_BYTES (64u * 1024u)

/* the tape step kinds (S1 ships rate/replay/off; single is S3) */
typedef enum StixDetTapeKind {
    STIX_DET_TAPE_RATE,
    STIX_DET_TAPE_REPLAY,
    STIX_DET_TAPE_OFF,
    STIX_DET_TAPE_SINGLE,       /* parsed but rejected in S1 (S3) */
    STIX_DET_TAPE_KIND__MAX,
} StixDetTapeKind;

/*
 * S2: a rate step's spectrum. FLAT is the S1 quiet template (the wire
 * name "flat"); the others are ADC-domain stimuli binned through the
 * loaded ELUT (line at an adc, line or flat band in keV, or a weighted
 * CSV read once at det-tape-load). keV specs go through the D5 law
 * (adc = base + floor(gain * keV + 0.5), gain = (4096 - base) / K), or,
 * with align, through the preimage of the loaded ELUT.
 */
typedef enum StixDetSpecKind {
    STIX_DET_SPEC_FLAT,         /* the quiet template (S1 "flat") */
    STIX_DET_SPEC_LINE_ADC,
    STIX_DET_SPEC_LINE_KEV,
    STIX_DET_SPEC_FLAT_KEV,
    STIX_DET_SPEC_FILE_ADC,     /* CSV header adc,weight */
    STIX_DET_SPEC_FILE_KEV,     /* CSV header kev,weight */
    STIX_DET_SPEC__MAX,
} StixDetSpecKind;

typedef struct StixDetSpecPoint {
    double value;               /* adc (integer) or keV */
    double weight;              /* >= 0, relative */
} StixDetSpecPoint;

typedef struct StixDetSpec {
    uint8_t kind;               /* StixDetSpecKind */
    bool align;                 /* effective: the step's, else the tape's */
    double v0, v1;              /* line: v0; flat_kev: [v0, v1) keV */
    StixDetSpecPoint *pts;      /* file: owned copy, 1..4096 points */
    uint32_t n_pts;
    char *desc;                 /* trace/hw-state text (no comma) */
} StixDetSpec;

#define STIX_DET_SPEC_MAX_POINTS 4096u
#define STIX_DET_SPEC_MAX_FILE (256u * 1024u)

/* one tape step (S1): kind rate|replay|off (single is S3), fpga_det/pix
 * masks, cps; label is optional. The fpga_det_mask/pix_mask bits are
 * fpga_det/pix indexed (plan T1). S2: spec (zero = FLAT). */
typedef struct StixDetTapeStep {
    uint8_t kind;               /* StixDetTapeKind */
    double for_s;               /* 0.1..86400 s on guest time */
    uint32_t fpga_det_mask;
    uint32_t pix_mask;          /* 0..0xFFF, pix bits */
    uint32_t cps;               /* counts/s per fpga_det (rate steps) */
    char *label;                /* optional, <= 64 chars */
    StixDetSpec spec;           /* S2: the rate step's spectrum */
} StixDetTapeStep;

/* the tape's state word (det-tape-status, hw-state det.tape.state) */
typedef enum StixDetTapeState {
    STIX_DET_TAPE_IDLE,         /* nothing loaded */
    STIX_DET_TAPE_READY,        /* loaded, not playing */
    STIX_DET_TAPE_PLAYING,
    STIX_DET_TAPE_DONE,         /* the last step ended without loop */
    STIX_DET_TAPE_STOPPED,      /* det-tape-play=false mid-playback */
    STIX_DET_TAPE__MAX,
} StixDetTapeState;

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
     * det-rotb-burst (FF plan): the last live chunk's SCET — the chunk
     * as built at a nominal swap whose append was APPENDED or FULL —
     * and the status of the last burst. A device reset clears both.
     */
    bool last_chunk_valid;
    uint32_t last_chunk_coarse; /* as 0xC8 reads, UNSYNCED in bit 31 */
    uint16_t last_chunk_fine;
    uint32_t burst_requested;
    uint32_t burst_appended;
    const char *burst_stop;     /* "none" before any burst */
    uint32_t burst_rotb_count;
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
    /*
     * S1, the mix tape: det-tape-load (JSON) fills steps; det-tape-play
     * arms the request (D-Dj) the next tick starts on; the step clock
     * rides the 100 ms tick (only while FSM Enable = 1). A device reset
     * stops playback and keeps the tape (state ready).
     */
    StixDetTapeStep tape_steps[STIX_DET_TAPE_MAX_STEPS];
    uint32_t tape_n_steps;
    uint32_t tape_step;         /* the running step's index */
    uint32_t tape_left_ticks;   /* ticks left in the running step */
    uint32_t tape_bg_cps;       /* the background layer, counts/s per
                                 * fpga_det (unmasked: all 384 pairs) */
    bool tape_in_replay;        /* a replay step switched the source */
    char *tape_name;            /* 1..64 chars */
    bool tape_loop;
    bool tape_align;            /* the tape-level default of "align" */
    bool tape_adc_mode;         /* overrides det-adc-mode while playing */
    uint8_t tape_state;         /* StixDetTapeState */
    bool tape_play_req;         /* det-tape-play=true, applied next tick */
    bool tape_stop_req;         /* det-tape-play=false, applied next tick */
    /*
     * A1: det-align turns the ELUT row classification on always (it
     * already runs for replay-dss) and keeps its summary for
     * det-align-status and hw-state det.align. Survives a device reset.
     */
    bool align;
    uint32_t align_rows;
    uint32_t align_fallback;
    uint32_t align_honoured;
    uint32_t align_mismatch_cells;
    uint32_t align_unassigned_cells;
    bool align_has_table;       /* a replay table gave the sim baseline */
    /*
     * C1: det-adc-mode folds the drawn sky through the loaded ELUT via
     * the cached per-row bin-mass map; det-cal-kev-full anchors the
     * keV <-> adc law. Both survive a device reset.
     */
    bool adc_mode;
    uint32_t adc_kev_full;      /* keV mapped to adc 4096, 1..4096 */
    bool adc_map_valid;
    bool adc_map_armed;         /* rebuild at the next classification point */
    double adc_map[STIX_DET_N_FPGA_DET][STIX_DET_N_PIX_ACC]
                  [STIX_DET_N_EBIN][STIX_DET_N_EBIN];
    /*
     * S2: the aligned keV specs' preimage misses of the running step's
     * last row build — one per (pair, ebin) whose honoured row has no
     * adc for that ebin (the mass is dropped). A gauge: reset at every
     * step start and at each build (det-align-status preimage_miss).
     */
    uint32_t spec_preimage_miss;
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
 * det-replay-start, the read-only det-replay-status, and the
 * det-rotb-burst pair) on the owning device class; @det_offset is
 * offsetof(owner, det), so the owner's struct stays private
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
 * The hw-state "det" object (D-PLm req 8, platform PL-11c): the scene,
 * the QL bank in use, the FSM enable and the replay progress, filled
 * by the controller's stixctrl_get_hw_state next to its quarter_on.
 * Pure: nothing advanced, nothing traced. The caller owns @det.
 */
void stix_det_hw_state(StixDetState *d, QDict *det);

/*
 * A1 (fixed now for S2/S3): an adc whose loaded ELUT row (pix @pix,
 * fpga_det @fpga_det) maps to @ebin — the midpoint of that adc run in
 * the honoured row. -1 when the row is degenerate (the caller uses the
 * sim calibration) or no adc maps to @ebin (the caller counts a
 * preimage_miss). Not called before S2.
 */
int stix_det_align_adc(StixDetState *d, uint32_t fpga_det, uint32_t pix,
                       uint32_t ebin);

/*
 * Provided by grlib_stixctrl.c for the detector model (D-5):
 * - stixctrl_scet_reg: the SCET as 0xC8 (UNSYNCED in bit 31) and 0xCC
 *   read right now;
 * - stixctrl_rotb_append: append @n_blocks blocks of STIXCTRL_BLOCK_SIZE
 *   to the ROTB ring if rotbConfig has ENABLE = 1 and TESTMODE = 0. With
 *   the default drop-new policy a chunk that does not fit is dropped
 *   whole and pulses IRQ 7 once; with rotb-overflow=overwrite-oldest
 *   (RF-0, Amendment B) it always goes in, each block into a full ring
 *   overwriting the oldest one (rd advances, count stays at capacity,
 *   IRQ 7 pulses per overwritten block, trace stixctrl_rotb_overwrite).
 *   Both policies return APPENDED only when every block is in the ring.
 *   *rotb_count is the block count afterwards.
 */
void stixctrl_scet_reg(STIXCTRL *s, uint32_t *coarse_reg, uint16_t *fine);
/* AS-3 (D-ASd): the same read with the timestamp offset by @off_ns
 * (whole milliseconds for the aspect jitter/step) */
void stixctrl_scet_reg_at(STIXCTRL *s, int64_t off_ns,
                          uint32_t *coarse_reg, uint16_t *fine);
StixDetRotbResult stixctrl_rotb_append(STIXCTRL *s, const uint8_t *blocks,
                                       uint32_t n_blocks,
                                       uint32_t *rotb_count);
/*
 * Provided by grlib_stixctrl.c (memory-upset plan D-Uc, M-2): stix_det's
 * host-side writes into the CACHE SRAM (the LACC/EACC materialization)
 * retire every armed upset they overwrite, like a guest write — the FPGA
 * rewrite cures the cell. @off is a STIXCTRL_SRAM offset, @len its
 * length in bytes.
 */
void stixctrl_edac_sram_write(STIXCTRL *s, hwaddr off, hwaddr len);

#endif
