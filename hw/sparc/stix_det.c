/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * STIX IDPU detector controller model. Tier 0 (detector plan D-1):
 * fsmCmd strobes, live fsmSta/fsmEvents, testPulseCmdSta, the 100 ms
 * tick, the QL bank switch with IRQ 3, and the register reset values.
 * Tier 1 (D-2): the IDeF-X serial command interface (ideFxCmdSta,
 * idefxModeData, idefxWriteMask), the 32-ASIC register store behind it,
 * and the index-space conversion tables. Tier 2 (D-4): the synthetic
 * sky (D-Df): per-tick gating and exposure, live SUMCNT and LVAR, the
 * LACC/LTRG materialization at the swap, the flare switch and the
 * scene properties. Tier 3 (D-5, D-Dg): the nominal integration (the
 * swap rule over fsmIntTime/fsmIntEvents, ActiveNominal, EACC/ETRG) and
 * the 49-block science chunk it appends to the ROTB ring. D-6: the
 * det-inject one-shot deposit, through the D-Df event sink
 * stix_det_add_counts. D-7 (D-Dk): the DSS .dssevs replay — the file
 * pre-scanned at realize into an event table, the ELUT lookup with the
 * linear fallback for degenerate rows, and the trigger/offset clock;
 * its ticks feed stix_det_add_counts like det-inject. S1: the mix tape
 * (det-tape-load/-play, JSON steps on guest time over a background
 * layer, per-pair rates on the 100 ms tick). A1: det-align, the ELUT
 * row classification and its report. C1: the ADC-faithful sky
 * (det-adc-mode): the cached per-row bin-mass map and the draw fold.
 * S2: tape step spectra (line/flat at adc or keV, CSV files), binned
 * through the loaded ELUT by a per-spec bin-mass row.
 *
 * FSW references are to STIX-FSW 600a7a72, the ASW tree (BSW =
 * BSW_ASW/src/BSW/...; utils.c, utils.h = ASW/src/utils.*; Detectors.c
 * = BSW Detectors.c), never to stix_asw_src.
 *
 * Re-entrancy (D-Dj): the tick writes the accumulator banks through
 * memory_region_get_ram_ptr (each write followed by
 * memory_region_set_dirty) and the ROTB ring through its host buffer
 * (stixctrl_rotb_append), never with address_space_*; its only other
 * outputs are qemu_irq_pulse (IRQ 3, and IRQ 7 for a dropped chunk).
 * These are writes into guest-visible memory from timer context, outside
 * any MMIO dispatch, so the tick engages the controller's
 * mem_reentrancy_guard for its whole run (the a83fe9a3a5 rule, as
 * stixspw_guard_enter/exit): should a future change let it reach the
 * controller's own MMIO, that access is refused ("Blocked re-entrant
 * IO") as on the in-MMIO paths. The QMP setters of the scene only store
 * fields; the next tick applies them.
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "qapi/qapi-builtin-type-infos.h"
#include "qapi/visitor.h"
#include "qobject/qlist.h"
#include "qobject/qbool.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qnum.h"
#include "qobject/qstring.h"
#include "hw/core/qdev.h"
#include "stix_det.h"
#include "trace.h"

#define STIX_DET_TICK_NS (100 * 1000 * 1000LL)
#define STIX_DET_QL_BANKS 4u

/* LVAR has 40 u32 slots per bank (Detectors.h:42 Detectors_MAXINTTIME) */
#define STIX_DET_LVAR_SLOTS (STIXCTRL_LVAR0_SIZE / sizeof(uint32_t))
#define STIX_DET_LVAR_MAX 0xFFFFu   /* LVAR clamps at 16 bits [D] */

/* fsmCmd: strobes 8:0 (reserved 4:3 excluded), config 12:9 and 31:16 */
#define FSMCMD_BIT(f) (STIXCTRL_FSMCMD_##f##_MASK << STIXCTRL_FSMCMD_##f)
#define STIX_DET_FSMCMD_STROBES \
    (FSMCMD_BIT(SETENABLE) | FSMCMD_BIT(CLEARENABLE) | FSMCMD_BIT(RESET) | \
     FSMCMD_BIT(SETCALENABLE) | FSMCMD_BIT(CALCLEAR) | \
     FSMCMD_BIT(QLCLEAR) | FSMCMD_BIT(OVERFLOWCLEAR))
#define STIX_DET_FSMCMD_CONFIG \
    (FSMCMD_BIT(CALTQ) | FSMCMD_BIT(FIFOIRQEN) | FSMCMD_BIT(QLIRQEN) | \
     FSMCMD_BIT(CALIRQEN) | FSMCMD_BIT(ACTIVETLUT))

/* testPulseCmdSta: Busy reads 1 (idle, active low), QuarterSeu 0 */
#define STIX_DET_TP_BUSY \
    (STIXCTRL_TESTPULSECMDSTA_BUSY_MASK << STIXCTRL_TESTPULSECMDSTA_BUSY)
#define STIX_DET_TP_SEU \
    (STIXCTRL_TESTPULSECMDSTA_QUARTERSEU_MASK << \
     STIXCTRL_TESTPULSECMDSTA_QUARTERSEU)

/* bank offsets inside the controller's RAM regions (StixIdpu.h:262-340) */
static const hwaddr stix_det_lacc_off[STIX_DET_QL_BANKS] = {
    STIXCTRL_LACC0_START - STIXCTRL_SRAM_START,
    STIXCTRL_LACC1_START - STIXCTRL_SRAM_START,
    STIXCTRL_LACC2_START - STIXCTRL_SRAM_START,
    STIXCTRL_LACC3_START - STIXCTRL_SRAM_START,
};
static const hwaddr stix_det_ltrg_off[STIX_DET_QL_BANKS] = {
    0,
    STIXCTRL_LTRG1_START - STIXCTRL_LTRG0_START,
    STIXCTRL_LTRG2_START - STIXCTRL_LTRG0_START,
    STIXCTRL_LTRG3_START - STIXCTRL_LTRG0_START,
};
static const hwaddr stix_det_lvar_off[STIX_DET_QL_BANKS] = {
    0,
    STIXCTRL_LVAR1_START - STIXCTRL_LVAR0_START,
    STIXCTRL_LVAR2_START - STIXCTRL_LVAR0_START,
    STIXCTRL_LVAR3_START - STIXCTRL_LVAR0_START,
};

/* the nominal banks: EACC n in the SRAM region, ETRG n in its own */
static const hwaddr stix_det_eacc_off[STIX_DET_NOM_BANKS] = {
    STIXCTRL_EACC0_START - STIXCTRL_SRAM_START,
    STIXCTRL_EACC1_START - STIXCTRL_SRAM_START,
};
static const hwaddr stix_det_etrg_off[STIX_DET_NOM_BANKS] = {
    0,
    STIXCTRL_ETRG1_START - STIXCTRL_ETRG0_START,
};

/* D-4 synthetic sky (defined after the index tables it uses) */
static void stix_det_sky_tick(StixDetState *d);
static void stix_det_sky_materialize(StixDetState *d, uint32_t ql_bank);
static void stix_det_sky_clear_expo(StixDetState *d);
/* D-7 replay helpers used by the FSM write and the QL swap */
static void stix_det_replay_enable_edge(StixDetState *d);
static void stix_det_replay_classify(StixDetState *d);
/* D-6 det-inject (defined after the sky: it uses the sky's gating) */
static void stix_det_inject_apply(StixDetState *d);
/* D-7 DSS replay (defined after the event sink it feeds) */
static void stix_det_replay_tick(StixDetState *d);
/* D-5 nominal integration (defined after the sky it draws from) */
static void stix_det_nominal_tick(StixDetState *d);
static void stix_det_nom_fresh(StixDetState *d);
static void stix_det_nom_latch(StixDetState *d);
/* S1 mix tape (defined after the scene properties it shares) */
static void stix_det_tape_tick(StixDetState *d);
static void stix_det_tape_stop(StixDetState *d, const char *why);
/* S1 per-pair rate: the tape step's cps or the background's */
static uint32_t stix_det_tape_pair_cps(const StixDetState *d,
                                       uint32_t fpga_det, uint32_t pix);
/* S2: the tape step spectrum (parsed with the tape) and its row */
static void stix_det_spec_free(StixDetSpec *sp);
static bool stix_det_spec_active(const StixDetState *d);
static void stix_det_spec_build(StixDetState *d);
static const char *stix_det_spec_desc(const StixDetSpec *sp);
/* A1: the ELUT row classification gate (the pass itself is
 * stix_det_replay_classify, extended with the summary and the C1 map) */
static bool stix_det_classify_wanted(const StixDetState *d);
/* C1: the ADC-faithful sky */
static bool stix_det_adc_mode_on(const StixDetState *d);
static void stix_det_adc_map_rebuild(StixDetState *d);
static double stix_det_adc_fold_share(const StixDetState *d, uint32_t fpga_det,
                                      uint32_t pix, uint32_t profile,
                                      uint32_t ebin_mask);
/* the scene properties' offset helper, shared by the tape/align/adc
 * accessors (defined with the scene properties below) */
static StixDetState *stix_det_of(Object *obj, void *opaque);

static uint32_t *stix_det_regp(StixDetState *d, hwaddr addr)
{
    return &((uint32_t *)d->reg)[addr / sizeof(uint32_t)];
}

static void stix_det_ram_zero(MemoryRegion *mr, hwaddr off, hwaddr len)
{
    memset((uint8_t *)memory_region_get_ram_ptr(mr) + off, 0, len);
    memory_region_set_dirty(mr, off, len);
}

static void stix_det_ram_stl(MemoryRegion *mr, hwaddr off, uint32_t val)
{
    stl_be_p((uint8_t *)memory_region_get_ram_ptr(mr) + off, val);
    memory_region_set_dirty(mr, off, sizeof(uint32_t));
}

/* QlIntTime is latched at the start of each QL period [D, RP-0067] */
static void stix_det_latch_ql_int_time(StixDetState *d)
{
    d->ql_int_time = (d->reg->fsmQlConfig >> STIXCTRL_FSMQLCONFIG_QLINTTIME) &
                     STIXCTRL_FSMQLCONFIG_QLINTTIME_MASK;
}

/* (re)start the tick one period from now (SetEnable, Reset) */
static void stix_det_tick_start(StixDetState *d)
{
    d->tick_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          STIX_DET_TICK_NS;
    timer_mod_ns(d->tick, d->tick_deadline_ns);
    trace_stixdet_tick_arm(1, d->tick_deadline_ns);
}

static void stix_det_tick_stop(StixDetState *d)
{
    timer_del(d->tick);
    trace_stixdet_tick_arm(0, 0);
}

/*
 * Zero one QL bank on activation: LACC 0x6000, LTRG 0x40 and LVAR 0xA0
 * only. The rest of each 0x100 LVAR stride is the WDT
 * UNCHANGED_STORAGE (+0xA8, WDT.c:74) and MemBram (0x3C0).
 */
static void stix_det_bank_zero(StixDetState *d, uint32_t ql_bank)
{
    stix_det_ram_zero(d->sram, stix_det_lacc_off[ql_bank],
                      STIXCTRL_LACC0_SIZE);
    /* the FPGA rewrite cures any armed upset (D-Uc host-write rule) */
    stixctrl_edac_sram_write(d->owner, stix_det_lacc_off[ql_bank],
                             STIXCTRL_LACC0_SIZE);
    stix_det_ram_zero(d->ltrg, stix_det_ltrg_off[ql_bank],
                      STIXCTRL_LTRG0_SIZE);
    stix_det_ram_zero(d->lvar, stix_det_lvar_off[ql_bank],
                      STIXCTRL_LVAR0_SIZE);
}

/*
 * D-Dd bank lifecycle:
 * 1. while a bank is active, its LVAR slots are written per tick
 *    (stix_det_tick); nothing else writes it;
 * 2. at the swap the finished bank's LACC and LTRG are written in full
 *    from the period's exposure (stix_det_sky_materialize; zeros with
 *    det-source=zero); its LVAR is not rewritten (it holds the per-tick
 *    slots);
 * 3. ActiveQl advances and the newly active bank is zeroed
 *    (stix_det_bank_zero); the active bank is also zeroed on a fresh
 *    enable (SetEnable 0 -> 1 at tick 0, stix_det_fsm_cmd), so no RAM
 *    from before a reset leaks into a QL period; QlReady sets and IRQ 3
 *    pulses if QlIrqEn;
 * 4. the finished bank is not touched again until it is active again,
 *    three swaps later. The FSW reads it after the IRQ, as bank
 *    (ActiveQl - 1) mod 4 (Detectors.c:144-166).
 */
static void stix_det_ql_swap(StixDetState *d)
{
    uint32_t done = d->active_ql_bank;
    uint32_t next = (done + 1) % STIX_DET_QL_BANKS;
    bool irq = (d->reg->fsmCmd >> STIXCTRL_FSMCMD_QLIRQEN) &
               STIXCTRL_FSMCMD_QLIRQEN_MASK;

    /*
     * D-Dk/S1-A1/C1: re-classify the ELUT rows for the coming period —
     * so a TC(237,8) apply during NOMINAL takes effect within one
     * period, and so the ADC bin-mass map (when the mode is on) is
     * rebuilt BEFORE the swap materializes its banks: the bank closed
     * by the first swap after a mid-period row rewrite already folds
     * with the new map (deliberately unlike the replay's event-time
     * lookup, which still shows the old binning in that bank).
     */
    if (stix_det_classify_wanted(d)) {
        stix_det_replay_classify(d);
    }
    /* S2: the spectrum row for the next period's ticks (approach 2) */
    if (stix_det_spec_active(d)) {
        stix_det_spec_build(d);
    }
    stix_det_sky_materialize(d, done);

    d->active_ql_bank = next;
    stix_det_bank_zero(d, next);

    d->ql_tick = 0;
    stix_det_latch_ql_int_time(d);
    d->ql_ready = true;
    trace_stixdet_ql_swap(done, next, d->ql_int_time, irq);
    if (irq) {
        qemu_irq_pulse(*d->irq);
    }
}

/*
 * One 100 ms tick of the active QL period. The sky first adds the
 * tick's exposure, SUMCNT and LVAR count (stix_det_sky_tick, gated per
 * tick). Tick k (0-based) writes its LVAR count to slot k of the
 * active bank while k < 40; later ticks of a longer period (QlIntTime
 * is 16 bits) are dropped, so no write ever leaves the bank's 0xA0
 * bytes (D-Dd, a model choice: the hardware behaviour is
 * undocumented). The period is QlIntTime + 1 ticks.
 */
static void stix_det_tick(void *opaque)
{
    StixDetState *d = opaque;
    MemReentrancyGuard *guard = &DEVICE(d->owner)->mem_reentrancy_guard;
    bool guard_was = guard->engaged_in_io;
    uint32_t slot = d->ql_tick;

    /* D-Dj: out-of-MMIO writes into guest-visible memory, see the top */
    guard->engaged_in_io = true;
    /*
     * S1: the tape first — the play/stop requests apply (D-Dj) and the
     * running step advances, so the sky tick below reads this tick's
     * per-pair rates from the tape state. The step clock only advances
     * here, i.e. only while FSM Enable = 1 (D3: SAFE freezes playback).
     */
    stix_det_tape_tick(d);
    stix_det_sky_tick(d);
    stix_det_replay_tick(d);
    stix_det_inject_apply(d);
    if (slot < STIX_DET_LVAR_SLOTS) {
        stix_det_ram_stl(d->lvar,
                         stix_det_lvar_off[d->active_ql_bank] +
                         slot * sizeof(uint32_t),
                         MIN(d->lvar_tick_count, STIX_DET_LVAR_MAX));
    }
    d->lvar_tick_count = 0;

    d->ql_tick++;
    if (d->ql_tick > d->ql_int_time) {
        stix_det_ql_swap(d);
    }
    stix_det_nominal_tick(d);

    d->tick_deadline_ns += STIX_DET_TICK_NS;
    timer_mod_ns(d->tick, d->tick_deadline_ns);
    guard->engaged_in_io = guard_was;
}

/*
 * fsmCmd write (D-Dc). The FSW issues every strobe as fsmCmd |= bit
 * (Detectors.c:136, 186, 1093-1124, 1198-1201), so the strobes read 0
 * or a stored ClearEnable would re-fire on the next RMW. The config bits
 * (CalTq, FifoIrqEn, QlIrqEn, CalIrqEn, ActiveTlut) are stored.
 * - ClearEnable clears Enable and stops the tick; it wins over
 *   SetEnable and freezes everything else [I].
 * - SetEnable (0 -> 1) starts the tick; at a period start (tick 0,
 *   e.g. after a device reset) it latches QlIntTime and zeroes the
 *   active bank (a fresh enable); likewise at the start of a nominal
 *   integration it latches the nominal parameters and zeroes the active
 *   EACC/ETRG bank (D-5).
 * - Reset clears QlReady and SUMCNT, restarts the QL period and the
 *   nominal integration and re-latches their parameters; ActiveQl,
 *   ActiveNominal and the banks stay [I]. It re-phases the tick only
 *   while enabled.
 * - QlClear clears QlReady; SetCalEnable sets CalEnable. CalClear and
 *   OverflowClear clear status that never sets (no calibration and no
 *   FIFO model: CalReady and QueueOverflow read 0).
 */
static void stix_det_fsm_cmd(StixDetState *d, uint32_t value)
{
    uint32_t strobes = value & STIX_DET_FSMCMD_STROBES;

    d->reg->fsmCmd = value & STIX_DET_FSMCMD_CONFIG;

    if (strobes & FSMCMD_BIT(CLEARENABLE)) {
        if (d->enable) {
            d->enable = false;
            stix_det_tick_stop(d);
        }
    } else if ((strobes & FSMCMD_BIT(SETENABLE)) && !d->enable) {
        d->enable = true;
        if (d->ql_tick == 0) {
            /* a fresh period: no stale RAM or exposure in the bank */
            stix_det_latch_ql_int_time(d);
            stix_det_bank_zero(d, d->active_ql_bank);
            stix_det_sky_clear_expo(d);
        }
        if (d->nom_tick == 0) {
            stix_det_nom_fresh(d);
        }
        stix_det_tick_start(d);
        /* D-Dk: the nominal trigger and the ELUT row classification */
        stix_det_replay_enable_edge(d);
    }
    if (strobes & FSMCMD_BIT(RESET)) {
        d->ql_ready = false;
        d->sumcnt = 0;
        d->ql_tick = 0;
        d->lvar_tick_count = 0;
        stix_det_latch_ql_int_time(d);
        d->nom_tick = 0;
        stix_det_nom_latch(d);
        if (d->enable) {
            stix_det_tick_start(d);
        }
    }
    if (strobes & FSMCMD_BIT(QLCLEAR)) {
        d->ql_ready = false;
    }
    if (strobes & FSMCMD_BIT(SETCALENABLE)) {
        d->cal_enable = true;
    }
    if (strobes) {
        trace_stixdet_fsm_strobe(strobes, d->enable, d->active_ql_bank,
                                 d->ql_ready);
    }
}

/* fsmSta (0x18), composed live (StixIdpu.h:890-912) */
static uint32_t stix_det_fsm_sta(StixDetState *d)
{
    return ((uint32_t)d->enable << STIXCTRL_FSMSTA_ENABLE) |
           (d->active_ql_bank << STIXCTRL_FSMSTA_ACTIVEQL) |
           (d->active_nom_bank << STIXCTRL_FSMSTA_ACTIVENOMINAL) |
           ((uint32_t)d->ql_ready << STIXCTRL_FSMSTA_QLREADY) |
           ((uint32_t)d->cal_enable << STIXCTRL_FSMSTA_CALENABLE);
    /* QueueOverflow and CalReady read 0 */
}

/*
 * ---- Index spaces (detector plan "Index spaces") ----
 *
 * The one conversion table set of the model. Each table is the FSW's,
 * transcribed number for number, with its file:line. No other file
 * holds an index mapping. stix_det_index_selfcheck proves at realize
 * that every forward/inverse pair composes to the identity and that the
 * arithmetic identities below agree with the FSW's dete_data table.
 */

/* utils_subcolIdFromFpgaId, utils.c:113-119: subcol of each fpga_det */
static const uint8_t stix_det_subcol_from_fpga_det[STIX_DET_N_FPGA_DET] = {
     0,  1,  5,  6,  4, 10, 11, 12,
    13, 14,  9, 15,  7,  8,  2,  3,
    30, 31, 25, 26, 21, 27, 19, 20,
    17, 18, 16, 22, 23, 24, 28, 29,
};

/* utils_fpgaIdFromSubcolId, utils.c:121-127: fpga_det of each subcol */
static const uint8_t stix_det_fpga_det_from_subcol[STIX_DET_N_SUBCOL] = {
     0,  1, 14, 15,  4,  2,  3, 12,
    13, 10,  5,  6,  7,  8,  9, 11,
    26, 24, 25, 22, 23, 20, 27, 28,
    29, 18, 19, 21, 30, 31, 16, 17,
};

/*
 * Detectors_frontendFromFpga, Detectors.c:528-534: fe_sc (1..32) of each
 * fpga_det
 */
static const uint8_t stix_det_fe_sc_from_fpga_det[STIX_DET_N_FPGA_DET] = {
     5, 11,  1,  2,  6,  7, 12, 13,
    10, 16, 14, 15,  8,  9,  3,  4,
    22, 28, 31, 32, 26, 27, 20, 21,
    17, 23, 18, 19, 24, 25, 29, 30,
};

/*
 * Detectors_fpgaFromFrontendMinusOne, Detectors.c:536-540, which equals
 * utils_oldSubcolIdToFpgaId, utils.c:129-133: fpga_det of fe_sc - 1
 */
static const uint8_t stix_det_fpga_det_from_fe_sc_m1[STIX_DET_N_FPGA_DET] = {
     2,  3, 14, 15,  0,  4,  5, 12, 13,  8,  1,  6,  7, 10, 11,  9,
    24, 26, 27, 22, 23, 16, 25, 28, 29, 20, 21, 17, 30, 31, 18, 19,
};

/*
 * dete_data, utils.c:74-107 (TN-15), indexed by subcol: the columns
 * indexFPGA (fpga_det), groupFPGA (grp), deteFPGA (asic) and quarterId
 * (quarter). The model uses the identities fpga_det = 2 * grp + asic and
 * quarter = grp / 4; the self-check holds them against this table.
 */
static const struct {
    uint8_t fpga_det, grp, asic, quarter;
} stix_det_dete_data[STIX_DET_N_SUBCOL] = {
    {  0,  0, 0, 0 }, {  1,  0, 1, 0 }, { 14,  7, 0, 1 }, { 15,  7, 1, 1 },
    {  4,  2, 0, 0 }, {  2,  1, 0, 0 }, {  3,  1, 1, 0 }, { 12,  6, 0, 1 },
    { 13,  6, 1, 1 }, { 10,  5, 0, 1 }, {  5,  2, 1, 0 }, {  6,  3, 0, 0 },
    {  7,  3, 1, 0 }, {  8,  4, 0, 1 }, {  9,  4, 1, 1 }, { 11,  5, 1, 1 },
    { 26, 13, 0, 3 }, { 24, 12, 0, 3 }, { 25, 12, 1, 3 }, { 22, 11, 0, 2 },
    { 23, 11, 1, 2 }, { 20, 10, 0, 2 }, { 27, 13, 1, 3 }, { 28, 14, 0, 3 },
    { 29, 14, 1, 3 }, { 18,  9, 0, 2 }, { 19,  9, 1, 2 }, { 21, 10, 1, 2 },
    { 30, 15, 0, 3 }, { 31, 15, 1, 3 }, { 16,  8, 0, 2 }, { 17,  8, 1, 2 },
};

/* utils_pixelId2channelId, utils.c:50-53: asic_ch of each pix */
static const uint8_t stix_det_asic_ch_from_pix[STIX_DET_N_PIX] = {
    26, 15, 8, 1, 29, 18, 5, 0, 30, 21, 11, 3, 31,
};

/* utils_channelId2pixelId, utils.c:56-62: pix of each asic_ch, 255 none */
#define STIX_DET_NO_PIX 255u
static const uint8_t stix_det_pix_from_asic_ch[STIX_DET_N_ASIC_CH] = {
      7,   3, 255,  11, 255,   6, 255, 255,
      2, 255, 255,  10, 255, 255, 255,   1,
    255, 255,   5, 255, 255,   9, 255, 255,
    255, 255,   0, 255, 255,   4,   8,  12,
};

static inline uint32_t stix_det_fpga_det_of(uint32_t grp, uint32_t asic)
{
    return 2 * grp + asic;
}

static inline uint32_t stix_det_quarter_of_grp(uint32_t grp)
{
    return grp / 4;
}

/* the inverse of stix_det_fpga_det_of: grp = fpga_det / 2 */
static inline uint32_t stix_det_grp_of(uint32_t fpga_det)
{
    return fpga_det / 2;
}

/*
 * Range-check one table entry before it is used as an index; false
 * sets errp naming the table, the entry and the bad value
 */
static bool stix_det_index_in_range(const char *table, uint32_t i,
                                    uint32_t value, uint32_t lo,
                                    uint32_t hi, Error **errp)
{
    if (value < lo || value > hi) {
        error_setg(errp, "stix_det: %s[%u] = %u is outside %u..%u",
                   table, i, value, lo, hi);
        return false;
    }
    return true;
}

/*
 * The realize-time self-check of the table set; false sets errp. It
 * proves the set is CONSISTENT (every entry in range, every
 * forward/inverse pair composing to the identity both ways, the
 * identities fpga_det = 2 * grp + asic and quarter = grp / 4 holding
 * on every dete_data row). It cannot prove fidelity to the FSW: a
 * consistent but wrong transcription would pass. Fidelity was checked
 * value by value against the FSW tree (accepted-diffs §32).
 */
static bool stix_det_index_selfcheck(Error **errp)
{
    uint32_t i, used_asic_ch = 0;

    /* 1. every entry in range, before any is used as an index */
    for (i = 0; i < STIX_DET_N_FPGA_DET; i++) {
        if (!stix_det_index_in_range("subcol_from_fpga_det", i,
                                     stix_det_subcol_from_fpga_det[i], 0,
                                     STIX_DET_N_SUBCOL - 1, errp) ||
            !stix_det_index_in_range("fpga_det_from_subcol", i,
                                     stix_det_fpga_det_from_subcol[i], 0,
                                     STIX_DET_N_FPGA_DET - 1, errp) ||
            !stix_det_index_in_range("fe_sc_from_fpga_det", i,
                                     stix_det_fe_sc_from_fpga_det[i], 1,
                                     STIX_DET_N_FPGA_DET, errp) ||
            !stix_det_index_in_range("fpga_det_from_fe_sc_m1", i,
                                     stix_det_fpga_det_from_fe_sc_m1[i], 0,
                                     STIX_DET_N_FPGA_DET - 1, errp) ||
            !stix_det_index_in_range("dete_data.fpga_det", i,
                                     stix_det_dete_data[i].fpga_det, 0,
                                     STIX_DET_N_FPGA_DET - 1, errp) ||
            !stix_det_index_in_range("dete_data.grp", i,
                                     stix_det_dete_data[i].grp, 0,
                                     STIX_DET_N_GRP - 1, errp) ||
            !stix_det_index_in_range("dete_data.asic", i,
                                     stix_det_dete_data[i].asic, 0,
                                     STIX_DET_N_ASIC - 1, errp) ||
            !stix_det_index_in_range("dete_data.quarter", i,
                                     stix_det_dete_data[i].quarter, 0,
                                     STIX_DET_N_QUARTER - 1, errp)) {
            return false;
        }
    }
    for (i = 0; i < STIX_DET_N_PIX; i++) {
        if (!stix_det_index_in_range("asic_ch_from_pix", i,
                                     stix_det_asic_ch_from_pix[i], 0,
                                     STIX_DET_N_ASIC_CH - 1, errp)) {
            return false;
        }
    }
    for (i = 0; i < STIX_DET_N_ASIC_CH; i++) {
        if (stix_det_pix_from_asic_ch[i] != STIX_DET_NO_PIX &&
            !stix_det_index_in_range("pix_from_asic_ch", i,
                                     stix_det_pix_from_asic_ch[i], 0,
                                     STIX_DET_N_PIX - 1, errp)) {
            return false;
        }
    }

    /* 2. forward/inverse pairs compose to the identity both ways */
    for (i = 0; i < STIX_DET_N_FPGA_DET; i++) {
        if (stix_det_fpga_det_from_subcol[stix_det_subcol_from_fpga_det[i]]
            != i ||
            stix_det_subcol_from_fpga_det[stix_det_fpga_det_from_subcol[i]]
            != i) {
            error_setg(errp, "stix_det: fpga_det/subcol tables disagree "
                       "at %u", i);
            return false;
        }
        if (stix_det_fpga_det_from_fe_sc_m1[stix_det_fe_sc_from_fpga_det[i]
                                            - 1] != i ||
            stix_det_fe_sc_from_fpga_det[stix_det_fpga_det_from_fe_sc_m1[i]]
            != i + 1) {
            error_setg(errp, "stix_det: fpga_det/fe_sc tables disagree "
                       "at %u", i);
            return false;
        }
    }

    /* 3. dete_data agrees with the subcol table and the identities */
    for (i = 0; i < STIX_DET_N_SUBCOL; i++) {
        uint32_t grp = stix_det_dete_data[i].grp;

        if (stix_det_dete_data[i].fpga_det !=
            stix_det_fpga_det_from_subcol[i] ||
            stix_det_fpga_det_of(grp, stix_det_dete_data[i].asic) !=
            stix_det_dete_data[i].fpga_det ||
            stix_det_grp_of(stix_det_dete_data[i].fpga_det) != grp ||
            stix_det_quarter_of_grp(grp) != stix_det_dete_data[i].quarter) {
            error_setg(errp, "stix_det: dete_data disagrees with the "
                       "index identities at subcol %u", i);
            return false;
        }
    }

    /* 4. pix <-> asic_ch: injective, and only those asic_ch have a pix */
    for (i = 0; i < STIX_DET_N_PIX; i++) {
        uint32_t asic_ch = stix_det_asic_ch_from_pix[i];

        if (stix_det_pix_from_asic_ch[asic_ch] != i ||
            (used_asic_ch & (1u << asic_ch))) {
            error_setg(errp, "stix_det: pix/asic_ch tables disagree at "
                       "pix %u", i);
            return false;
        }
        used_asic_ch |= 1u << asic_ch;
    }
    for (i = 0; i < STIX_DET_N_ASIC_CH; i++) {
        if (stix_det_pix_from_asic_ch[i] != STIX_DET_NO_PIX &&
            !(used_asic_ch & (1u << i))) {
            error_setg(errp, "stix_det: asic_ch %u maps to a pix that "
                       "does not map back", i);
            return false;
        }
    }
    return true;
}

/*
 * ---- IDeF-X interface (D-De) ----
 *
 * At the FSW-visible register level, not at STROBE/DIN bit level. A
 * write to ideFxCmdSta executes at once, so CmdReady (bit 31) always
 * reads 1. Words go to every grp in idefxWriteMask (GrpAddr's grp when
 * the mask is 0); a grp takes part only if its quarter is powered, its
 * Enable bit is set and its ReadMode bit is 0. The response, in
 * idefxModeData[15:0], comes from GrpAddr's grp, or is 0.
 */

#define IDEFXCMDSTA_CMDREADY \
    ((uint32_t)STIXCTRL_IDEFXCMDSTA_CMDREADY_MASK << \
     STIXCTRL_IDEFXCMDSTA_CMDREADY)
/* the stored command fields: GrpAddr 23:20, CmdCounter 19:16, CmdData */
#define IDEFXCMDSTA_FIELDS 0x00FFFFFFu
#define IDEFX_HI16 0xFFFF0000u      /* ReadMode / WriteMask (grp bits) */

/*
 * CmdCounter values (hw/sparc/StixIdpu.h:1047-1063; FSW
 * BSW_ASW/src/BSW/include/private/StixIdpu.h:1050-1066)
 */
enum {
    IDEFX_CMD_RESET = 0,
    IDEFX_CMD_ADCTEMPREAD = 1,
    IDEFX_CMD_ADCBUFFREAD = 2,
    IDEFX_CMD_ADCMUXREAD = 3,
    IDEFX_CMD_SCEND = 4,
    IDEFX_CMD_BLOCK = 8,            /* REGADDRESS / BYTEWRITE */
    IDEFX_CMD_SCINITSEQ = 11,
};

enum { STIX_IDEFX_IDLE, STIX_IDEFX_SC_ADDR, STIX_IDEFX_SC_DATA };

/*
 * cmd 1 context: the first cmd 1 after a reg-13 read address block is
 * readTemp's A, the cmd 1 after the following data block its B
 * (Detectors.c:883-950)
 */
enum {
    STIX_IDEFX_ADC_NONE,
    STIX_IDEFX_ADC_TEMP_A,      /* reg-13 read address block seen */
    STIX_IDEFX_ADC_TEMP_A_DONE, /* A answered */
    STIX_IDEFX_ADC_TEMP_B,      /* the data block after A seen */
};

#define STIX_IDEFX_REG_TEMP 13u
/* canned ADC answers: A >= B (XRayADCsCtrl.c:540-546), the baseline */
#define STIX_IDEFX_ADC_TEMP_A_VAL 0x900u
#define STIX_IDEFX_ADC_TEMP_B_VAL 0x700u
#define STIX_IDEFX_ADC_BASELINE 1850u   /* [D, DSS app TempScript] */

/*
 * Register sizes in bits (commonFSW.h:251-262, XRayADCsCtrl.c:317-331).
 * R0 (TEST) is not used by the FSW and R13 (TEMP) is a dummy that only
 * sets the ADC context, so neither stores anything.
 */
static const uint8_t stix_idefx_reg_bits[STIX_DET_IDEFX_N_REG] = {
    0, 2, 3, 192, 32, 4, 2, 5, 2, 2, 3, 3, 32, 0,
};

/*
 * The HW defaults (ParamMgmtDef.h:525-536 ASIC_REGISTER_n_HW_DEFAULT;
 * the IDeF-X DS table agrees) for the registers up to 32 bits; R3 is 62
 * on every asic_ch (stix_det_idefx_defaults)
 */
static const uint32_t stix_idefx_reg_default[STIX_DET_IDEFX_N_REG] = {
    0, 1, 0, 0, 0, 15, 1, 0, 3, 0, 3, 3, 0xFFFFFFFFu, 0,
};
#define STIX_IDEFX_R3_DEFAULT_TH 62u

static uint32_t stix_idefx_reg_nbytes(uint32_t reg)
{
    return reg < STIX_DET_IDEFX_N_REG ? (stix_idefx_reg_bits[reg] + 7) / 8 : 0;
}

/* the defaults into the registers of one fpga_det */
static void stix_det_idefx_defaults(StixDetState *d, uint32_t fpga_det)
{
    uint32_t reg, n, bit;

    memset(d->idefx_reg[fpga_det], 0, sizeof(d->idefx_reg[fpga_det]));
    for (reg = 1; reg < STIX_DET_IDEFX_N_REG; reg++) {
        uint32_t nbytes = stix_idefx_reg_nbytes(reg);
        uint8_t *v = d->idefx_reg[fpga_det][reg];

        if (reg == 3) {
            /* 32 six-bit fields of 62, MSB first: FB EF BE, 8 times */
            for (bit = 0; bit < stix_idefx_reg_bits[reg]; bit++) {
                if ((STIX_IDEFX_R3_DEFAULT_TH >> (5 - bit % 6)) & 1) {
                    v[bit / 8] |= 0x80u >> (bit % 8);
                }
            }
            continue;
        }
        for (n = 0; n < nbytes; n++) {
            v[n] = (uint8_t)(stix_idefx_reg_default[reg] >>
                             (8 * (nbytes - 1 - n)));
        }
    }
}

static void stix_det_idefx_defaults_mask(StixDetState *d,
                                         uint32_t fpga_det_mask,
                                         const char *why)
{
    uint32_t fpga_det;

    for (fpga_det = 0; fpga_det < STIX_DET_N_FPGA_DET; fpga_det++) {
        if (fpga_det_mask & (1u << fpga_det)) {
            stix_det_idefx_defaults(d, fpga_det);
        }
    }
    trace_stixdet_idefx_defaults(fpga_det_mask, why);
}

static uint32_t stix_det_grp_fpga_det_mask(uint32_t grp)
{
    return 3u << stix_det_fpga_det_of(grp, 0);
}

static void stix_det_idefx_idle(StixIdefxDecoder *dec)
{
    memset(dec, 0, sizeof(*dec));
    dec->asic = -1;
}

/*
 * A data block written into an N-bit shift register: the register keeps
 * the last N bits written (IDeF-X DS), so the byte shifts in at the
 * LSB end and the bits above N fall off
 */
static void stix_det_idefx_shift_in(StixDetState *d, uint32_t fpga_det,
                                    uint32_t reg, uint8_t byte)
{
    uint32_t nbytes = stix_idefx_reg_nbytes(reg);
    uint32_t top_bits;
    uint8_t *v;

    if (!nbytes) {
        return;     /* R0, R13 and unknown addresses store nothing */
    }
    top_bits = stix_idefx_reg_bits[reg] - 8 * (nbytes - 1);
    v = d->idefx_reg[fpga_det][reg];
    memmove(v, v + 1, nbytes - 1);
    v[nbytes - 1] = byte;
    v[0] &= (uint8_t)((1u << top_bits) - 1);
}

/*
 * Byte k of a read: the N bits MSB first, left-aligned and zero padded,
 * so a register shorter than 8 bits reads value << (8 - N), which
 * readReg shifts back (Detectors.c:838-841); 0 past the end
 */
static uint8_t stix_det_idefx_read_byte(StixDetState *d, uint32_t fpga_det,
                                        uint32_t reg, uint32_t k)
{
    uint32_t nbytes = stix_idefx_reg_nbytes(reg);
    uint32_t pad, hi, lo;
    const uint8_t *v;

    if (k >= nbytes) {
        return 0;
    }
    pad = 8 * nbytes - stix_idefx_reg_bits[reg];
    v = d->idefx_reg[fpga_det][reg];
    hi = v[k];
    lo = k + 1 < nbytes ? v[k + 1] : 0;
    return (uint8_t)(pad ? (hi << pad) | (lo >> (8 - pad)) : hi);
}

/*
 * One command word into one grp's decoder (D-De); returns the response.
 * - cmd 0 (RESET) ends any transaction.
 * - cmd 11 (SCINITSEQ) latches RD (bit 9), SC (8), All (7) and the
 *   address (6:4) from INIT_INTERFACE_SEQ; the FSW's word is
 *   0xBxx30 | asic << 6, so asic 0 is address 3, asic 1 address 7 and
 *   asic 2 sets All. RD = 0, SC = 1 opens slow control on the
 *   addressed asic (both with All). RD = SC = 1 resets the registers of
 *   the addressed asic (both with All) to the defaults [A]: the DS says
 *   "all ASICs", but the FSW resets one ASIC per subcol
 *   (XRayADCsCtrl_resetASIC, XRayADCsCtrl.c:833, Detectors_resetReg
 *   with asic 0 or 1), and resetting the sibling too would make the
 *   next verification report it (0x542E). It starts no transaction.
 *   RD = 1, SC = 0 (readout) is not modelled. Every SCINITSEQ idles the
 *   decoder first, so a read restarts at byte 0.
 * - The first cmd 8 is the address block (R/W bit 14, register 13:7),
 *   each further cmd 8 a data block (byte 14:7): a write shifts it into
 *   the register of the addressed asic (both with All), a read answers
 *   the next byte. A read with All, or of no asic, answers 0, and a
 *   cmd 8 outside a slow-control transaction is ignored.
 * - cmd 4 (SCEND) ends the transaction.
 * - cmd 1 answers readTemp's A, then B, by context, else the baseline;
 *   cmd 2 and 3 answer the baseline.
 * Every other command answers 0 and changes nothing.
 */
static uint16_t stix_det_idefx_decode(StixDetState *d, uint32_t grp,
                                      uint32_t cmd, uint32_t data)
{
    StixIdefxDecoder *dec = &d->idefx_dec[grp];
    uint32_t asic, fpga_det, block, rd, sc, addr, all, mask = 0;
    int asic_addr;
    uint8_t byte;

    switch (cmd) {
    case IDEFX_CMD_RESET:
    case IDEFX_CMD_SCEND:
        stix_det_idefx_idle(dec);
        return 0;
    case IDEFX_CMD_SCINITSEQ:
        rd = (data >> 9) & 1;
        sc = (data >> 8) & 1;
        addr = (data >> 4) & 7;
        stix_det_idefx_idle(dec);
        all = (data >> 7) & 1;
        asic_addr = addr == 3 ? 0 : addr == 7 ? 1 : -1;
        if (rd && sc) {
            /* [A] the addressed ASIC only (both with All), see above */
            for (asic = 0; asic < STIX_DET_N_ASIC; asic++) {
                if (all || asic_addr == (int)asic) {
                    mask |= 1u << stix_det_fpga_det_of(grp, asic);
                }
            }
            if (mask) {
                stix_det_idefx_defaults_mask(d, mask, "RD=SC=1");
            }
        } else if (sc) {
            dec->phase = STIX_IDEFX_SC_ADDR;
            dec->all = all;
            dec->asic = asic_addr;
        }
        return 0;
    case IDEFX_CMD_BLOCK:
        if (dec->phase == STIX_IDEFX_SC_ADDR) {
            dec->read = (data >> 14) & 1;
            dec->reg = (data >> 7) & 0x7F;
            dec->phase = STIX_IDEFX_SC_DATA;
            if (dec->read && dec->reg == STIX_IDEFX_REG_TEMP) {
                dec->adc_ctx = STIX_IDEFX_ADC_TEMP_A;
            }
            return 0;
        }
        if (dec->phase != STIX_IDEFX_SC_DATA) {
            return 0;
        }
        if (dec->adc_ctx == STIX_IDEFX_ADC_TEMP_A_DONE) {
            dec->adc_ctx = STIX_IDEFX_ADC_TEMP_B;
        }
        block = dec->block_idx;
        dec->block_idx = MIN(block + 1, 0xFFu);
        if (dec->read) {
            if (dec->all || dec->asic < 0) {
                return 0;
            }
            fpga_det = stix_det_fpga_det_of(grp, dec->asic);
            byte = stix_det_idefx_read_byte(d, fpga_det, dec->reg, block);
            trace_stixdet_idefx_reg(fpga_det, dec->reg, 1, block, byte);
            return byte;
        }
        byte = (uint8_t)(data >> 7);
        for (asic = 0; asic < STIX_DET_N_ASIC; asic++) {
            if (dec->all || dec->asic == (int8_t)asic) {
                fpga_det = stix_det_fpga_det_of(grp, asic);
                stix_det_idefx_shift_in(d, fpga_det, dec->reg, byte);
                trace_stixdet_idefx_reg(fpga_det, dec->reg, 0, block, byte);
            }
        }
        return 0;
    case IDEFX_CMD_ADCTEMPREAD:
        if (dec->adc_ctx == STIX_IDEFX_ADC_TEMP_A) {
            dec->adc_ctx = STIX_IDEFX_ADC_TEMP_A_DONE;
            return STIX_IDEFX_ADC_TEMP_A_VAL;
        }
        if (dec->adc_ctx == STIX_IDEFX_ADC_TEMP_B) {
            dec->adc_ctx = STIX_IDEFX_ADC_NONE;
            return STIX_IDEFX_ADC_TEMP_B_VAL;
        }
        return STIX_IDEFX_ADC_BASELINE;
    case IDEFX_CMD_ADCBUFFREAD:
    case IDEFX_CMD_ADCMUXREAD:
        return STIX_IDEFX_ADC_BASELINE;
    default:
        return 0;
    }
}

/* a grp takes part: quarter powered, Enable set, ReadMode 0 [D, RP-0067] */
static bool stix_det_grp_takes_part(StixDetState *d, uint32_t grp)
{
    uint32_t cfg = d->reg->idefxConfig;

    return ((cfg >> stix_det_quarter_of_grp(grp)) & 1) &&
           ((cfg >> (STIXCTRL_IDEFXCONFIG_ENABLE + grp)) & 1) &&
           !((d->reg->idefxModeData >> (STIXCTRL_IDEFXMODEDATA_READMODE +
                                         grp)) & 1);
}

/* ideFxCmdSta write: one 20-bit word, executed at once */
static void stix_det_idefx_cmd(StixDetState *d, uint32_t value)
{
    uint32_t grp_addr = (value >> STIXCTRL_IDEFXCMDSTA_GRPADDR) &
                        STIXCTRL_IDEFXCMDSTA_GRPADDR_MASK;
    uint32_t cmd = (value >> STIXCTRL_IDEFXCMDSTA_CMDCOUNTER) &
                   STIXCTRL_IDEFXCMDSTA_CMDCOUNTER_MASK;
    uint32_t data = value & STIXCTRL_IDEFXCMDSTA_CMDDATA_MASK;
    uint32_t write_mask = d->reg->idefxWriteMask >>
                          STIXCTRL_IDEFXWRITEMASK_WRITEMASK;
    uint32_t delivered_grp_mask = 0;
    uint16_t response = 0;
    uint32_t grp;

    d->reg->ideFxCmdSta = (value & IDEFXCMDSTA_FIELDS) | IDEFXCMDSTA_CMDREADY;
    if (!write_mask) {
        write_mask = 1u << grp_addr;    /* lenient: GrpAddr's grp */
    }
    for (grp = 0; grp < STIX_DET_N_GRP; grp++) {
        if ((write_mask & (1u << grp)) && stix_det_grp_takes_part(d, grp)) {
            uint16_t rx = stix_det_idefx_decode(d, grp, cmd, data);

            delivered_grp_mask |= 1u << grp;
            if (grp == grp_addr) {
                response = rx;
            }
        }
    }
    d->reg->idefxModeData = (d->reg->idefxModeData & IDEFX_HI16) | response;
    trace_stixdet_idefx_cmd(grp_addr, cmd << 16 | data, delivered_grp_mask,
                            response);
}

/*
 * idefxConfig write: a quarter-power 0 -> 1 edge resets its ASICs and
 * idles its grp decoders (a power-on reset)
 */
static void stix_det_idefx_config(StixDetState *d, uint32_t value)
{
    uint32_t rising = value & ~d->reg->idefxConfig & 0xFu;
    uint32_t fpga_det_mask = 0;
    uint32_t grp;

    d->reg->idefxConfig = value;
    if (!rising) {
        return;
    }
    for (grp = 0; grp < STIX_DET_N_GRP; grp++) {
        if (rising & (1u << stix_det_quarter_of_grp(grp))) {
            fpga_det_mask |= stix_det_grp_fpga_det_mask(grp);
            stix_det_idefx_idle(&d->idefx_dec[grp]);
        }
    }
    stix_det_idefx_defaults_mask(d, fpga_det_mask, "quarter power on");
}

/*
 * ---- The synthetic sky (D-4, detector plan D-Df) ----
 *
 * Accumulators, not events: per 100 ms tick the model gates each
 * (fpga_det, pix), adds the tick's expected counts to the period's
 * exposure, and draws the tick's SUMCNT and LVAR counts; at the QL swap
 * it draws every LACC cell of the finished bank from the exposure and
 * writes LTRG per grp. The sky is generated in ebin space and never goes
 * through the FPGA ELUT (a recorded deviation, accepted-diffs §34).
 */

/* the default scene (D-Db); rates are counts/s per fpga_det */
#define STIX_DET_QUIET_CPS_DEFAULT 20u
#define STIX_DET_FLARE_CPS_DEFAULT 2000u
#define STIX_DET_CPS_MAX 1000000u
#define STIX_DET_SEED_DEFAULT 1u

/* accumulator widths [D, BRAM widths]: LACC wraps, LTRG and LVAR clamp */
#define STIX_DET_LTRG_MAX 0xFFFFFFu
/* Poisson by inversion below this mean, by a rounded normal above */
#define STIX_DET_POISSON_NORMAL 30.0

static const char *const stix_det_source_name[STIX_DET_SOURCE__MAX] = {
    [STIX_DET_SOURCE_ZERO] = "zero",
    [STIX_DET_SOURCE_SYNTHETIC] = "synthetic",
    [STIX_DET_SOURCE_REPLAY_DSS] = "replay-dss",
};

/*
 * The template (D-Df [I]; the energies are approximate and do not
 * matter to the gates, since no ELUT is involved). Uniform over
 * fpga_det (QLDetPerf, QLDetPerf.c:124-143, flags a non-uniform sky).
 * - pix share: large pix 0-7 weight 1.0, small pix 8-11 0.22 (the area
 *   ratio, about 4.5 [D]), normalized to 1 over pix 0..11;
 * - quiet ebin share: 10 % flat over ebin 1-11, 15 % flat over ebin
 *   12-30, and the Ba-133 lines: 45 % in ebin 18 (31 keV), 10 % in
 *   ebin 19 (35 keV), 20 % in ebin 27 (81 keV);
 * - flare ebin share: 70 % thermal over ebin 1-10, falling x0.7 per
 *   bin, and 30 % power law (bin number ^ -3) over ebin 11-25.
 * Nothing lands in ebin 0 or 31.
 */
static void stix_det_sky_template(StixDetState *d)
{
    double *quiet = d->ebin_share[STIX_DET_PROFILE_QUIET];
    double *flare = d->ebin_share[STIX_DET_PROFILE_FLARE];
    double w_total = 0, thermal = 0, power = 0;
    uint32_t pix, ebin;

    for (pix = 0; pix < STIX_DET_N_PIX_ACC; pix++) {
        d->pix_share[pix] = pix < 8 ? 1.0 : 0.22;
        w_total += d->pix_share[pix];
    }
    for (pix = 0; pix < STIX_DET_N_PIX_ACC; pix++) {
        d->pix_share[pix] /= w_total;
    }

    memset(d->ebin_share, 0, sizeof(d->ebin_share));
    for (ebin = 1; ebin <= 11; ebin++) {
        quiet[ebin] += 0.10 / 11;
    }
    for (ebin = 12; ebin <= 30; ebin++) {
        quiet[ebin] += 0.15 / 19;
    }
    quiet[18] += 0.45;
    quiet[19] += 0.10;
    quiet[27] += 0.20;

    for (ebin = 1; ebin <= 10; ebin++) {
        thermal += pow(0.7, ebin - 1);
    }
    for (ebin = 11; ebin <= 25; ebin++) {
        power += pow(ebin, -3.0);
    }
    for (ebin = 1; ebin <= 10; ebin++) {
        flare[ebin] = 0.70 * pow(0.7, ebin - 1) / thermal;
    }
    for (ebin = 11; ebin <= 25; ebin++) {
        flare[ebin] = 0.30 * pow(ebin, -3.0) / power;
    }
}

/* xorshift64* (D-Df PRNG), reseeded from det-seed at every reset */
static uint64_t stix_det_rand64(StixDetState *d)
{
    uint64_t x = d->prng;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    d->prng = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* uniform in [0, 1) */
static double stix_det_rand_unit(StixDetState *d)
{
    return ldexp((double)(stix_det_rand64(d) >> 11), -53);
}

/* splitmix64 of the seed, so every seed (0 included) gives a state != 0 */
static void stix_det_reseed(StixDetState *d)
{
    uint64_t z = d->seed + 0x9E3779B97F4A7C15ULL;

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    d->prng = z ? z : 1;
}

/*
 * A Poisson draw: by inversion for a mean below 30, by a rounded normal
 * (Box-Muller) above. Runs are statistically, not bit-for-bit,
 * reproducible across hosts' libm; on one host the same seed and the
 * same steps give the same counts.
 */
static uint64_t stix_det_poisson(StixDetState *d, double lambda)
{
    if (lambda <= 0) {
        return 0;
    }
    if (lambda < STIX_DET_POISSON_NORMAL) {
        double u = stix_det_rand_unit(d);
        double p = exp(-lambda);
        double cdf = p;
        uint64_t k = 0;

        /* the cap only guards a cdf that rounds below u near 1 */
        while (u > cdf && k < 1000) {
            k++;
            p *= lambda / k;
            cdf += p;
        }
        return k;
    } else {
        double u1 = 1.0 - stix_det_rand_unit(d);    /* (0, 1] */
        double u2 = stix_det_rand_unit(d);
        double z = sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
        double n = floor(lambda + sqrt(lambda) * z + 0.5);

        return n > 0 ? (uint64_t)n : 0;
    }
}

static void stix_det_sky_clear_expo(StixDetState *d)
{
    memset(d->expo, 0, sizeof(d->expo));
    /* the injected counts belong to the period, like the exposure */
    memset(d->inj_ql, 0, sizeof(d->inj_ql));
    /* S2: the spectrum exposure too (only written when the flag is) */
    if (d->spec_expo_any) {
        memset(d->spec_expo, 0, sizeof(d->spec_expo));
        d->spec_expo_any = false;
    }
}

/*
 * The R3 threshold of one asic_ch of one fpga_det. The store holds R3
 * raw, in wire order; the FSW reads it back as
 * utils_getReg3(asic_ch, utils_reorderThreshold3(raw)) (utils.c:538-555,
 * 663-677), which is the 6-bit field number asic_ch of the raw bytes
 * counted MSB first (fields 0..31 at bit 6 * asic_ch from the top).
 */
static uint32_t stix_det_r3_th(StixDetState *d, uint32_t fpga_det,
                               uint32_t asic_ch)
{
    const uint8_t *r3 = d->idefx_reg[fpga_det][3];
    uint32_t pos = 6 * asic_ch;
    uint32_t byte = pos / 8;
    uint32_t window = (uint32_t)r3[byte] << 8 |
                      (byte + 1 < STIX_DET_IDEFX_REG_BYTES ? r3[byte + 1] : 0);

    return (window >> (10 - pos % 8)) & 0x3F;
}

/* a grp reads out: quarter powered, Enable set, ReadMode 1 (not config) */
static bool stix_det_grp_reads_out(StixDetState *d, uint32_t grp)
{
    uint32_t cfg = d->reg->idefxConfig;

    return ((cfg >> stix_det_quarter_of_grp(grp)) & 1) &&
           ((cfg >> (STIXCTRL_IDEFXCONFIG_ENABLE + grp)) & 1) &&
           ((d->reg->idefxModeData >> (STIXCTRL_IDEFXMODEDATA_READMODE +
                                        grp)) & 1);
}

/*
 * The pix (bit i = pix i) of one fpga_det that count in this tick
 * (D-Df, gating per tick): its grp reads out, and the R3 threshold of
 * asic_ch = utils_pixelId2channelId[pix] is not 63 (63 disables the
 * channel; XRayADCsCtrl.c:205-209 sets non-pixel channels to it).
 * ALIMON (R12) is not used: its per-asic_ch bit order is undefined
 * (plan Doc vs code 12).
 */
static uint32_t stix_det_pix_gate(StixDetState *d, uint32_t fpga_det)
{
    uint32_t pix, pix_mask = 0;

    if (!stix_det_grp_reads_out(d, stix_det_grp_of(fpga_det))) {
        return 0;
    }
    for (pix = 0; pix < STIX_DET_N_PIX_ACC; pix++) {
        if (stix_det_r3_th(d, fpga_det, stix_det_asic_ch_from_pix[pix]) !=
            0x3F) {
            pix_mask |= 1u << pix;
        }
    }
    return pix_mask;
}

/* the share of one profile's counts in the ebin of @ebin_mask (bit i) */
static double stix_det_ebin_share_masked(StixDetState *d, uint32_t profile,
                                         uint32_t ebin_mask)
{
    double share = 0;
    uint32_t ebin;

    for (ebin = 0; ebin < STIX_DET_N_EBIN; ebin++) {
        if ((ebin_mask >> ebin) & 1) {
            share += d->ebin_share[profile][ebin];
        }
    }
    return share;
}

/*
 * One tick of the sky (D-Df): for every gated (fpga_det, pix) add the
 * tick's expected counts to the exposure of the active profile; draw
 * the tick's SUMCNT (SUMDMASK fpga_det, SUMEMASK ebin, all pix) and LVAR
 * count (VARDMASK fpga_det, VARPMASK pix, VAREMASK ebin). The masks are
 * read live; their bit i is index i ([A] for the FPGA-only pix and ebin
 * masks, plan "Index spaces"). QLACC is not masked. The nominal
 * integration (D-5) gets the same exposure for the fpga_det in SUMDMASK
 * (EACC is gated by it [D, RP-0067]); the others count in ETRG only.
 *
 * S1: while a tape plays the per-pair rate is the active step's cps on
 * its masks, else the background's (kind off contributes 0; an all-32
 * off step silences the background too); the scene's det-flare and the
 * two *-cps knobs are ignored and the quiet profile is used. The rate
 * still flows through the exposure + template and the per-tick pix
 * gate: a step rate on a gated-off pix produces nothing, like a real
 * event. On stop/done the scene knobs resume.
 *
 * C1: with the ADC mode on the SUMEMASK/VAREMASK shares are the
 * map-folded ones (per pair, from the cached bin-mass map); the
 * exposure and the totals stay ebin-space.
 *
 * S2: a pair inside a running spectrum step's masks takes its counts
 * c = tick counts x pix share into the destination-ebin exposure
 * spec_expo (and nom_spec_expo in SUMDMASK, else ETRG only) through
 * the step's bin-mass row; its SUMCNT/LVAR shares are the row's masked
 * sums (already ELUT-binned: no C1 fold); nothing goes to expo or
 * nom_expo for it. Pairs outside the masks keep the background path.
 */
static void stix_det_sky_tick(StixDetState *d)
{
    bool playing = d->tape_state == STIX_DET_TAPE_PLAYING;
    uint32_t profile = !playing && d->flare ? STIX_DET_PROFILE_FLARE :
                                              STIX_DET_PROFILE_QUIET;
    uint32_t cps = !playing && d->flare ? d->flare_cps : d->quiet_cps;
    uint32_t sumd_fpga_det = d->reg->fsmSumDmask;
    uint32_t vard_fpga_det = d->reg->fsmQlVarDMask;
    uint32_t varp_pix = (d->reg->fsmQlConfig >>
                         STIXCTRL_FSMQLCONFIG_QLVARPMASK) &
                        STIXCTRL_FSMQLCONFIG_QLVARPMASK_MASK;
    uint32_t sum_ebin_mask = d->reg->fsmSumEmask;
    uint32_t var_ebin_mask = d->reg->fsmQlVarEMask;
    double sum_share, var_share, sum_lambda = 0, var_lambda = 0;
    bool folded = stix_det_adc_mode_on(d) && d->adc_map_valid;
    bool spec = stix_det_spec_active(d);
    const StixDetTapeStep *st = &d->tape_steps[d->tape_step];
    uint32_t fpga_det, pix;
    uint64_t n;

    if (d->source != STIX_DET_SOURCE_SYNTHETIC ||
        (!playing && cps == 0)) {
        return;
    }
    sum_share = stix_det_ebin_share_masked(d, profile, sum_ebin_mask);
    var_share = stix_det_ebin_share_masked(d, profile, var_ebin_mask);

    for (fpga_det = 0; fpga_det < STIX_DET_N_FPGA_DET; fpga_det++) {
        uint32_t pix_mask = stix_det_pix_gate(d, fpga_det);

        for (pix = 0; pix < STIX_DET_N_PIX_ACC; pix++) {
            uint32_t pair_cps = playing ?
                stix_det_tape_pair_cps(d, fpga_det, pix) : cps;
            double tick_counts, c, pair_sum_share, pair_var_share;

            if (!((pix_mask >> pix) & 1)) {
                continue;
            }
            tick_counts = pair_cps * (STIX_DET_TICK_NS / 1e9);
            if (spec && ((st->fpga_det_mask >> fpga_det) & 1) &&
                ((st->pix_mask >> pix) & 1)) {
                const double *row = d->spec_row[fpga_det][pix];
                bool in_sum = (sumd_fpga_det >> fpga_det) & 1;
                bool in_var = ((vard_fpga_det >> fpga_det) & 1) &&
                              ((varp_pix >> pix) & 1);
                double row_all = 0, row_sum = 0, row_var = 0;
                uint32_t t;

                c = tick_counts * d->pix_share[pix];
                for (t = 0; t < STIX_DET_N_EBIN; t++) {
                    double m = c * row[t];

                    if (m == 0.0) {
                        continue;
                    }
                    d->spec_expo[fpga_det][pix][t] += m;
                    d->spec_expo_any = true;
                    if (in_sum) {
                        d->nom_spec_expo[fpga_det][pix][t] += m;
                        d->nom_spec_expo_any = true;
                    }
                    row_all += row[t];
                    row_sum += ((sum_ebin_mask >> t) & 1) ? row[t] : 0;
                    row_var += ((var_ebin_mask >> t) & 1) ? row[t] : 0;
                }
                if (in_sum) {
                    sum_lambda += c * row_sum;
                } else {
                    d->nom_trg_extra_grp[stix_det_grp_of(fpga_det)] +=
                        c * row_all;
                }
                if (in_var) {
                    var_lambda += c * row_var;
                }
                continue;
            }
            d->expo[fpga_det][pix][profile] += tick_counts;
            c = tick_counts * d->pix_share[pix];
            pair_sum_share = folded ?
                stix_det_adc_fold_share(d, fpga_det, pix, profile,
                                        sum_ebin_mask) : sum_share;
            pair_var_share = folded ?
                stix_det_adc_fold_share(d, fpga_det, pix, profile,
                                        var_ebin_mask) : var_share;
            if ((sumd_fpga_det >> fpga_det) & 1) {
                d->nom_expo[fpga_det][pix][profile] += tick_counts;
                sum_lambda += c * pair_sum_share;
            } else {
                d->nom_trg_extra_grp[stix_det_grp_of(fpga_det)] += c;
            }
            if (((vard_fpga_det >> fpga_det) & 1) &&
                ((varp_pix >> pix) & 1)) {
                var_lambda += c * pair_var_share;
            }
        }
    }

    n = stix_det_poisson(d, sum_lambda);
    d->sumcnt = n > UINT32_MAX - d->sumcnt ? UINT32_MAX : d->sumcnt + n;
    n = stix_det_poisson(d, var_lambda);
    d->lvar_tick_count = MIN(d->lvar_tick_count + n, (uint64_t)UINT32_MAX);
}

/*
 * The D-Df event sink (D-6 det-inject; the D-7 replay will call it too):
 * add @n integer counts at (pix @pix, fpga_det @fpga_det, ebin @ebin).
 * The counts are kept per cell in inj_ql / inj_nom, on top of the drawn
 * sky, so they land at their own cell (the template shares stay out of
 * it) in the LACC of the finished bank at the next QL swap and in EACC
 * — and so the next chunk — at the next nominal swap, wrapping at 16
 * bits with the draw. The same rules as the sky, read live:
 * - the pair counts only where stix_det_pix_gate has the pix on; where
 *   it is off the deposit is dropped, like a real event;
 * - EACC is SUMDMASK-gated [D, RP-0067]: a deposit at an fpga_det
 *   outside SUMDMASK does not land in EACC and counts in ETRG only,
 *   through nom_trg_extra_grp, as the sky's does (the triggers are not
 *   masked [I]);
 * - SUMCNT takes the SUMDMASK x SUMEMASK share, LVAR the VARDMASK x
 *   VARPMASK x VAREMASK share (the inject is applied before the LVAR
 *   slot write, so the share goes into the slot this tick writes);
 * - QLACC is not masked.
 */
void stix_det_add_counts(StixDetState *d, uint32_t fpga_det, uint32_t pix,
                         uint32_t ebin, uint32_t n)
{
    uint32_t varp_pix = (d->reg->fsmQlConfig >>
                         STIXCTRL_FSMQLCONFIG_QLVARPMASK) &
                        STIXCTRL_FSMQLCONFIG_QLVARPMASK_MASK;
    uint64_t lvar;

    g_assert(fpga_det < STIX_DET_N_FPGA_DET);
    g_assert(pix < STIX_DET_N_PIX_ACC);
    g_assert(ebin < STIX_DET_N_EBIN);
    if (!((stix_det_pix_gate(d, fpga_det) >> pix) & 1)) {
        return;
    }
    d->inj_ql[fpga_det][pix][ebin] += n;
    if ((d->reg->fsmSumDmask >> fpga_det) & 1) {
        d->inj_nom[fpga_det][pix][ebin] += n;
        if ((d->reg->fsmSumEmask >> ebin) & 1) {
            d->sumcnt = n > UINT32_MAX - d->sumcnt ? UINT32_MAX :
                        d->sumcnt + n;
        }
    } else {
        d->nom_trg_extra_grp[stix_det_grp_of(fpga_det)] += n;
    }
    if (((d->reg->fsmQlVarDMask >> fpga_det) & 1) &&
        ((varp_pix >> pix) & 1) &&
        ((d->reg->fsmQlVarEMask >> ebin) & 1)) {
        lvar = (uint64_t)d->lvar_tick_count + n;
        d->lvar_tick_count = (uint32_t)MIN(lvar, (uint64_t)UINT32_MAX);
    }
}

/*
 * The pending det-inject request (D-Dj: the setter stores, the tick
 * applies): one call into the event sink, then gone.
 */
static void stix_det_inject_apply(StixDetState *d)
{
    if (!d->inject_pending) {
        return;
    }
    d->inject_pending = false;
    stix_det_add_counts(d, d->inject_fpga_det, d->inject_pix,
                        d->inject_ebin, d->inject_n);
}

/*
 * ---- The DSS .dssevs replay (D-7, plan D-Dk) ----
 *
 * The file is a stream of big-endian u32 words, no header (detector
 * plan "The user's .dssevs file" [V]): [31:20] val12 (the adc of an
 * event, otherwise a setup value), [19:16] type, [15:11] dss_det,
 * [10:0] wait in 20 ns units. Every word's wait adds to the replay
 * time. Types 0x0-0xB are events (type = pix, val = adc); 0xD and 0xE
 * are one setup word per dss_det (the 0xE value is the baseline the
 * linear fallback needs, the 0xD one is not implemented and counts as
 * unsupported for det-replay-status); 0xF is wait-only padding. The
 * file is pre-scanned once at realize into an event table; each tick
 * feeds the events due in it through the D-Df event sink, so the
 * det-inject gating and mask rules apply unchanged.
 * fpga_det = dss_det (identity, plan "Index spaces").
 */

/* the word layout [V] */
#define STIX_DET_DSSEVS_VAL_SHIFT 20
#define STIX_DET_DSSEVS_VAL_MASK  0xFFFu    /* adc 0..4095 / setup value */
#define STIX_DET_DSSEVS_TYPE_SHIFT 16
#define STIX_DET_DSSEVS_TYPE_MASK  0xFu
#define STIX_DET_DSSEVS_DET_SHIFT 11
#define STIX_DET_DSSEVS_DET_MASK  0x1Fu     /* dss_det 0..31 */
#define STIX_DET_DSSEVS_WAIT_MASK 0x7FFu    /* 20 ns units */
#define STIX_DET_DSSEVS_WAIT_NS 20u
#define STIX_DET_DSSEVS_TYPE_SETUP_D 0xDu
#define STIX_DET_DSSEVS_TYPE_SETUP_E 0xEu
#define STIX_DET_DSSEVS_TYPE_PAD 0xFu
/*
 * the FPGA ELUT: uint8 val[pix][fpga_det][adc] at the start of the
 * SRAM window (Detectors.h:47-52, StixIdpu.h STIXCTRL_ELUT_*)
 */
#define STIX_DET_ELUT_ROW_BYTES (STIX_DET_N_FPGA_DET * STIX_DET_N_ADC)

QEMU_BUILD_BUG_ON(STIXCTRL_ELUT_SIZE !=
                  STIX_DET_N_PIX_ACC * STIX_DET_ELUT_ROW_BYTES);

/* the realize pre-scan reads in 1 MB chunks, never whole into memory */
#define STIX_DET_REPLAY_SCAN_CHUNK (1024u * 1024u)

static const char *const stix_det_replay_trigger_name
                                         [STIX_DET_REPLAY_TRIGGER__MAX] = {
    [STIX_DET_REPLAY_TRIGGER_NOMINAL] = "nominal",
    [STIX_DET_REPLAY_TRIGGER_QMP] = "qmp",
};

/*
 * The pre-scan (D-Dk): read det-replay-file in 1 MB chunks, decode
 * each word by its type, add every word's wait to the time, and keep
 * only the events. A missing, unreadable or malformed file is a
 * realize error naming the path. The scan time and the table size are
 * traced (stixdet_replay_scan).
 */
static bool stix_det_replay_scan(StixDetState *d, Error **errp)
{
    g_autofree uint8_t *buf = g_malloc(STIX_DET_REPLAY_SCAN_CHUNK);
    uint64_t t_ns = 0, n_words = 0, n_bytes = 0;
    int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    FILE *f;
    size_t got;

    f = fopen(d->replay_file, "rb");
    if (!f) {
        error_setg(errp, "det-replay-file: cannot read '%s': %s",
                   d->replay_file, strerror(errno));
        return false;
    }
    d->replay_unsupported = 0;  /* recounted per scan */
    for (;;) {
        size_t i, words;

        got = fread(buf, 1, STIX_DET_REPLAY_SCAN_CHUNK, f);
        if (ferror(f)) {
            error_setg(errp, "det-replay-file: cannot read '%s': %s",
                       d->replay_file, strerror(errno));
            fclose(f);
            return false;
        }
        n_bytes += got;
        words = got / 4;
        for (i = 0; i < words; i++) {
            uint32_t w = ldl_be_p(buf + 4 * i);
            uint32_t type = (w >> STIX_DET_DSSEVS_TYPE_SHIFT) &
                            STIX_DET_DSSEVS_TYPE_MASK;
            uint32_t dss_det = (w >> STIX_DET_DSSEVS_DET_SHIFT) &
                               STIX_DET_DSSEVS_DET_MASK;

            n_words++;
            t_ns += (uint64_t)(w & STIX_DET_DSSEVS_WAIT_MASK) *
                    STIX_DET_DSSEVS_WAIT_NS;
            if (t_ns / STIX_DET_TICK_NS > UINT32_MAX) {
                error_setg(errp, "det-replay-file: '%s' word %" PRIu64
                           " is past 2^32 ticks of replay time",
                           d->replay_file, n_words - 1);
                fclose(f);
                return false;
            }
            if (type <= 0xB) {
                /* an event: type = pix, val = adc; fpga_det = dss_det */
                if (d->replay_n_ev == STIX_DET_REPLAY_MAX_EVENTS) {
                    error_setg(errp, "det-replay-file: '%s' word %" PRIu64
                               " has more than %u events (the replay cap)",
                               d->replay_file, n_words - 1,
                               STIX_DET_REPLAY_MAX_EVENTS);
                    fclose(f);
                    return false;
                }
                if (d->replay_n_ev == d->replay_alloc) {
                    d->replay_alloc = d->replay_alloc ?
                                      MIN(d->replay_alloc * 2,
                                          STIX_DET_REPLAY_MAX_EVENTS) : 1024;
                    d->replay_ev = g_renew(StixDetReplayEvent, d->replay_ev,
                                           d->replay_alloc);
                }
                d->replay_ev[d->replay_n_ev++] = (StixDetReplayEvent) {
                    .t_tick = (uint32_t)(t_ns / STIX_DET_TICK_NS),
                    .fpga_det = dss_det,
                    .pix = type,
                    .adc = (w >> STIX_DET_DSSEVS_VAL_SHIFT) &
                           STIX_DET_DSSEVS_VAL_MASK,
                };
            } else if (type == STIX_DET_DSSEVS_TYPE_SETUP_E) {
                /* the per-dss_det baseline the fallback bins from */
                d->replay_base_dss_det[dss_det] =
                    (w >> STIX_DET_DSSEVS_VAL_SHIFT) &
                    STIX_DET_DSSEVS_VAL_MASK;
            } else if (type == STIX_DET_DSSEVS_TYPE_SETUP_D) {
                /*
                 * a setup word of a kind the model does not implement:
                 * counted for det-replay-status (D-PLm req 4); a 0xF
                 * wait-only padding word is not one
                 */
                d->replay_unsupported++;
            } else if (type != STIX_DET_DSSEVS_TYPE_PAD) {
                error_setg(errp, "det-replay-file: '%s' word %" PRIu64
                           " has type 0xC, which the .dssevs format does "
                           "not define", d->replay_file, n_words - 1);
                fclose(f);
                return false;
            }
        }
        if (got < STIX_DET_REPLAY_SCAN_CHUNK) {
            break;              /* short read: end of file */
        }
    }
    fclose(f);
    if (n_bytes % 4) {
        error_setg(errp, "det-replay-file: '%s' is %" PRIu64
                   " bytes, not a multiple of the 4-byte word size",
                   d->replay_file, n_bytes);
        return false;
    }
    d->replay_loaded = true;
    d->replay_armed = true;     /* the nominal trigger is pending */
    trace_stixdet_replay_scan(d->replay_file, n_words, d->replay_n_ev,
                              t_ns, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) -
                              t0);
    return true;
}

/*
 * The ebin of one event in the honoured branch: the FPGA ELUT the FSW
 * wrote, val[pix][fpga_det][adc] (Detectors.h:47-52); 32 (unassigned)
 * drops the event, as the FPGA would (LUTMgmt.c:834-935).
 */
static uint32_t stix_det_replay_elut_ebin(StixDetState *d, uint32_t fpga_det,
                                          uint32_t pix, uint32_t adc)
{
    const uint8_t *elut = memory_region_get_ram_ptr(d->sram);

    return elut[(pix * STIX_DET_N_FPGA_DET + fpga_det) *
                STIX_DET_N_ADC + adc];
}

/*
 * The linear non-physical fallback of one event (D-Dk, for a degenerate
 * row): ebin = (adc - base) * 32 / (4096 - base) clamped to 0..31, with
 * the baseline from the file's 0xE setup word; events at or below it
 * are dropped. This differs from what the FPGA would do with this ELUT
 * (it would bin everything into the row's constant).
 */
static uint32_t stix_det_replay_fallback_ebin(StixDetState *d,
                                              uint32_t fpga_det,
                                              uint32_t adc)
{
    uint64_t base = d->replay_base_dss_det[fpga_det];

    return MIN(((uint64_t)adc - base) * STIX_DET_N_EBIN /
               (STIX_DET_N_ADC - base), STIX_DET_N_EBIN - 1);
}

/*
 * A1: is a classification pass wanted at this point? The established
 * replay-dss case, plus det-align and the ADC-faithful sky (C1), which
 * both need the row states (and, for the ADC mode, the map rebuild).
 */
static bool stix_det_classify_wanted(const StixDetState *d)
{
    return d->align || d->source == STIX_DET_SOURCE_REPLAY_DSS ||
           stix_det_adc_mode_on(d) || stix_det_spec_active(d);
}

/*
 * The sim calibration baseline of one fpga_det (C1/D5): the dss_det's
 * 0xE baseline from the realized replay table, or 0 without one (the
 * status reports base_source=zero). Identical across pix by default.
 */
static uint32_t stix_det_cal_base(const StixDetState *d, uint32_t fpga_det)
{
    return d->replay_loaded ? d->replay_base_dss_det[fpga_det] : 0;
}

/*
 * The sim calibration's ebin of one adc, the D-7 law (C1/D5): the
 * grid the fallback bins on, E(e) = base + ceil(e*W/32); ebin e covers
 * adc [E(e), E(e+1)), so the bin of adc a is (a - base)*32/W, floor,
 * clamped to 0..31 (adc below the grid clamps into ebin 0: adc = base
 * = E(0) sits inside sim ebin 0's interval; only the map build sees
 * the clamped low edge, the replay fallback drops adc <= base before
 * it would ever bin one).
 */
static uint32_t stix_det_cal_ebin(uint32_t base, uint32_t adc)
{
    int64_t diff = (int64_t)adc - (int64_t)base;
    int64_t w = (int64_t)(STIX_DET_N_ADC - base);
    int64_t ebin = diff * (int64_t)STIX_DET_N_EBIN / w;

    return (uint32_t)MAX(0, MIN(ebin, (int64_t)STIX_DET_N_EBIN - 1));
}

/*
 * The ebin the loaded ELUT gives one (fpga_det, pix, adc) — the one
 * lookup of the replay tick and the S2 spectrum build: a honoured row's
 * val (32 = unassigned -> -1, the FPGA drops it); a degenerate row's
 * D-7 linear fallback (adc at or below the baseline -> -1). The row
 * state is the last classification's (replay_row_fallback).
 */
static int stix_det_adc_ebin(StixDetState *d, uint32_t fpga_det,
                             uint32_t pix, uint32_t adc)
{
    uint32_t ebin;

    if (d->replay_row_fallback[fpga_det][pix]) {
        if (adc <= d->replay_base_dss_det[fpga_det]) {
            return -1;
        }
        return stix_det_replay_fallback_ebin(d, fpga_det, adc);
    }
    ebin = stix_det_replay_elut_ebin(d, fpga_det, pix, adc);
    return ebin < STIX_DET_N_EBIN ? (int)ebin : -1;
}

/* the D5 gain of one fpga_det: (4096 - base) / K, adc per keV */
static double stix_det_kev_gain(const StixDetState *d, uint32_t fpga_det)
{
    return (double)(STIX_DET_N_ADC - stix_det_cal_base(d, fpga_det)) /
           d->adc_kev_full;
}

/*
 * D5: the adc of @kev on one fpga_det, base + floor(gain * kev + 0.5)
 * (half up; never rint's half-even); -1 when it is past 4095 (an ADC
 * overflow: the caller drops the mass). K is read now, at the build.
 */
static int stix_det_kev_adc(const StixDetState *d, uint32_t fpga_det,
                            double kev)
{
    uint32_t base = stix_det_cal_base(d, fpga_det);
    /* gain * kev as (4096 - base) * kev / K: exact for integer keV */
    double adc = base + floor((STIX_DET_N_ADC - base) * kev /
                              d->adc_kev_full + 0.5);

    return adc > STIX_DET_N_ADC - 1 ? -1 : (int)adc;
}

/*
 * D5: the science ebin of @kev, floor(32 * kev / K); -1 when kev >= K
 * (an ADC overflow, decision Q7: dropped, never clamped to ebin 31)
 */
static int stix_det_kev_ebin(const StixDetState *d, double kev)
{
    if (kev >= d->adc_kev_full) {
        return -1;
    }
    return (int)floor(STIX_DET_N_EBIN * kev / d->adc_kev_full);
}

/*
 * Classify every (fpga_det, pix) ELUT row (D-Dk): a constant row (all
 * 0 — the power-on/EDAC state — or all 31 — what applyELUT writes from
 * the all-zero boundary table of an empty flash, LUTMgmt.c:912 — or
 * any other constant) cannot be a calibration, so the row falls back
 * to the linear binning. Runs at each SetEnable 0->1 edge and at each
 * QL swap while the classification is wanted (D-Dk: replay-dss; A1:
 * det-align; C1: the ADC mode), so a TC(237,8) apply during NOMINAL
 * takes effect within one period. A row newly classified degenerate
 * traces stixdet_elut_fallback once.
 *
 * A1: the pass also fills the det-align summary (rows, fallback,
 * honoured, mismatch_cells, unassigned_cells) and traces one
 * stixdet_align line per classification — the classification itself
 * becomes visible (the E-4 vacuity gap). mismatch_cells compares the
 * honoured rows' ELUT values against the sim calibration's linear bin
 * (the D-7 law from the replay table's baselines); without a table
 * there is no comparison and calibration reads "none" — that marks the
 * comparison as not computed, not a zero-mismatch verdict.
 *
 * C1: while the ADC mode is on the pass rebuilds the per-row bin-mass
 * map (stix_det_adc_map_rebuild) from the fresh row states.
 */
static void stix_det_replay_classify(StixDetState *d)
{
    uint32_t fpga_det, pix;

    d->align_rows = STIX_DET_N_FPGA_DET * STIX_DET_N_PIX_ACC;
    d->align_fallback = 0;
    d->align_honoured = 0;
    d->align_mismatch_cells = 0;
    d->align_unassigned_cells = 0;
    d->align_has_table = d->replay_loaded;

    for (fpga_det = 0; fpga_det < STIX_DET_N_FPGA_DET; fpga_det++) {
        uint32_t base = stix_det_cal_base(d, fpga_det);
        uint32_t mism = 0, unass = 0, honoured = 0;

        for (pix = 0; pix < STIX_DET_N_PIX_ACC; pix++) {
            const uint8_t *row = memory_region_get_ram_ptr(d->sram) +
                                 (pix * STIX_DET_N_FPGA_DET + fpga_det) *
                                 STIX_DET_N_ADC;
            uint32_t adc;
            bool degenerate = true;

            for (adc = 1; adc < STIX_DET_N_ADC; adc++) {
                if (row[adc] != row[0]) {
                    degenerate = false;
                    break;
                }
            }
            if (degenerate && !d->replay_row_fallback[fpga_det][pix]) {
                trace_stixdet_elut_fallback(fpga_det, pix, row[0]);
            }
            d->replay_row_fallback[fpga_det][pix] = degenerate;
            if (degenerate) {
                d->align_fallback++;
                continue;
            }
            honoured++;
            if (d->align_has_table) {
                for (adc = 0; adc < STIX_DET_N_ADC; adc++) {
                    if (row[adc] >= STIX_DET_N_EBIN) {
                        unass++;
                    } else if (row[adc] != stix_det_cal_ebin(base, adc)) {
                        mism++;
                    }
                }
            }
        }
        d->align_honoured += honoured;
        d->align_mismatch_cells += mism;
        d->align_unassigned_cells += unass;
    }
    trace_stixdet_align(d->align_rows, d->align_fallback, d->align_honoured,
                        d->align_mismatch_cells,
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    if (stix_det_adc_mode_on(d)) {
        stix_det_adc_map_rebuild(d);
    }
}

/*
 * A1 (fixed for S2/S3): an adc whose loaded ELUT row (pix @pix,
 * fpga_det @fpga_det) maps to @ebin — the midpoint of the longest run
 * of adc holding that value. -1 when the row is degenerate (the caller
 * uses the sim calibration) or no adc maps to @ebin (the caller counts
 * a preimage_miss). Not called before S2.
 */
int stix_det_align_adc(StixDetState *d, uint32_t fpga_det, uint32_t pix,
                       uint32_t ebin)
{
    const uint8_t *row;
    uint32_t adc, best_len = 0, best_mid = 0, run_len = 0, run_start = 0;

    if (fpga_det >= STIX_DET_N_FPGA_DET || pix >= STIX_DET_N_PIX_ACC ||
        ebin >= STIX_DET_N_EBIN || d->replay_row_fallback[fpga_det][pix]) {
        return -1;
    }
    row = memory_region_get_ram_ptr(d->sram) +
          (pix * STIX_DET_N_FPGA_DET + fpga_det) * STIX_DET_N_ADC;
    for (adc = 0; adc <= STIX_DET_N_ADC; adc++) {
        if (adc < STIX_DET_N_ADC && row[adc] == ebin) {
            if (run_len == 0) {
                run_start = adc;
            }
            run_len++;
            continue;
        }
        if (run_len > best_len) {
            best_len = run_len;
            best_mid = run_start + run_len / 2;
        }
        run_len = 0;
    }
    return best_len ? (int)best_mid : -1;
}

/*
 * ---- S2: the per-spec bin-mass row ----
 *
 * spec_row[fpga_det][pix][ebin] is the fraction of the running step's
 * counts the loaded ELUT puts in each ebin (destination space; the
 * row's drops — unassigned, at or below the baseline, ADC overflow,
 * a preimage miss — are missing mass). Built from the SRAM ram pointer
 * inside the tick only (step start, QL swap), under the tick's
 * mem_reentrancy_guard; never from QMP.
 */

/* a playing tape whose running step has a non-flat spec */
static bool stix_det_spec_active(const StixDetState *d)
{
    return d->tape_state == STIX_DET_TAPE_PLAYING && d->tape_n_steps &&
           d->tape_steps[d->tape_step].kind == STIX_DET_TAPE_RATE &&
           d->tape_steps[d->tape_step].spec.kind != STIX_DET_SPEC_FLAT;
}

/* one adc point of weight @w into the row (a dropped adc is lost mass) */
static void stix_det_spec_put_adc(StixDetState *d, uint32_t fpga_det,
                                  uint32_t pix, int adc, double w,
                                  double *row)
{
    int ebin = adc < 0 ? -1 : stix_det_adc_ebin(d, fpga_det, pix, adc);

    if (ebin >= 0) {
        row[ebin] += w;
    }
}

/*
 * One pair's row, unnormalized (the caller divides by the spec's total
 * weight). Align (keV specs on a honoured row): the keV mass goes to
 * ebins by the D5 law, then each ebin with mass lands at the ELUT bin
 * of its preimage adc (stix_det_align_adc, = that ebin); no preimage is
 * a miss (dropped, counted per (pair, ebin)). On a degenerate row align
 * is a no-op (the fallback is the calibration grid).
 */
static void stix_det_spec_pair(StixDetState *d, const StixDetSpec *sp,
                               uint32_t fpga_det, uint32_t pix, double *row)
{
    bool kev = sp->kind == STIX_DET_SPEC_LINE_KEV ||
               sp->kind == STIX_DET_SPEC_FLAT_KEV ||
               sp->kind == STIX_DET_SPEC_FILE_KEV;
    uint32_t i;

    if (kev && sp->align && !d->replay_row_fallback[fpga_det][pix]) {
        double kmass[STIX_DET_N_EBIN] = { 0 };
        double k_bin = (double)d->adc_kev_full / STIX_DET_N_EBIN;
        uint32_t e;
        int eb;

        switch (sp->kind) {
        case STIX_DET_SPEC_LINE_KEV:
            eb = stix_det_kev_ebin(d, sp->v0);
            if (eb >= 0) {
                kmass[eb] += 1.0;
            }
            break;
        case STIX_DET_SPEC_FLAT_KEV:
            for (e = 0; e < STIX_DET_N_EBIN; e++) {
                double lo = MAX(sp->v0, e * k_bin);
                double hi = MIN(sp->v1, (e + 1) * k_bin);

                if (hi > lo) {
                    kmass[e] += hi - lo;
                }
            }
            break;
        default:        /* a kev,weight file */
            for (i = 0; i < sp->n_pts; i++) {
                eb = stix_det_kev_ebin(d, sp->pts[i].value);
                if (eb >= 0) {
                    kmass[eb] += sp->pts[i].weight;
                }
            }
            break;
        }
        for (e = 0; e < STIX_DET_N_EBIN; e++) {
            int adc;

            if (kmass[e] <= 0) {
                continue;
            }
            adc = stix_det_align_adc(d, fpga_det, pix, e);
            if (adc < 0) {
                d->spec_preimage_miss++;
                continue;
            }
            stix_det_spec_put_adc(d, fpga_det, pix, adc, kmass[e], row);
        }
        return;
    }
    switch (sp->kind) {
    case STIX_DET_SPEC_LINE_ADC:
        stix_det_spec_put_adc(d, fpga_det, pix, (int)sp->v0, 1.0, row);
        break;
    case STIX_DET_SPEC_LINE_KEV:
        stix_det_spec_put_adc(d, fpga_det, pix,
                              stix_det_kev_adc(d, fpga_det, sp->v0), 1.0,
                              row);
        break;
    case STIX_DET_SPEC_FLAT_KEV: {
        /* adc a weighted by the overlap of [a, a+1) with the band */
        double base = stix_det_cal_base(d, fpga_det);
        double gain = stix_det_kev_gain(d, fpga_det);
        double lo = base + (STIX_DET_N_ADC - base) * sp->v0 / d->adc_kev_full;
        double hi = base + (STIX_DET_N_ADC - base) * sp->v1 / d->adc_kev_full;
        uint32_t a, a_end = (uint32_t)MIN(ceil(hi), (double)STIX_DET_N_ADC);
        /* any finite keV loads (Q7): a band past the top is all overflow */
        uint32_t a0 = lo < (double)STIX_DET_N_ADC ? (uint32_t)floor(lo) :
                                                    STIX_DET_N_ADC;

        for (a = a0; a < a_end; a++) {
            double w = MIN(a + 1.0, hi) - MAX((double)a, lo);

            if (w > 0) {
                stix_det_spec_put_adc(d, fpga_det, pix, a, w / gain, row);
            }
        }
        break;
    }
    case STIX_DET_SPEC_FILE_ADC:
        for (i = 0; i < sp->n_pts; i++) {
            stix_det_spec_put_adc(d, fpga_det, pix, (int)sp->pts[i].value,
                                  sp->pts[i].weight, row);
        }
        break;
    case STIX_DET_SPEC_FILE_KEV:
        for (i = 0; i < sp->n_pts; i++) {
            stix_det_spec_put_adc(d, fpga_det, pix,
                                  stix_det_kev_adc(d, fpga_det,
                                                   sp->pts[i].value),
                                  sp->pts[i].weight, row);
        }
        break;
    default:
        break;
    }
}

/* the spec's total weight: what the row is normalized by */
static double stix_det_spec_total(const StixDetSpec *sp)
{
    double total = 0;
    uint32_t i;

    switch (sp->kind) {
    case STIX_DET_SPEC_FLAT_KEV:
        return sp->v1 - sp->v0;     /* keV (the adc path divides by gain) */
    case STIX_DET_SPEC_FILE_ADC:
    case STIX_DET_SPEC_FILE_KEV:
        for (i = 0; i < sp->n_pts; i++) {
            total += sp->pts[i].weight;
        }
        return total;
    default:
        return 1.0;
    }
}

/*
 * Build spec_row for the running spectrum step: one pass over the
 * pairs of its fpga_det/pix masks (0 outside them). Called at the
 * step start (after a classification) and at each QL swap while the
 * step runs (after the classification, before materialize: the
 * rebuild acts on the next period's ticks, so the bank closed by the
 * first swap after an ELUT rewrite still shows the old binning — the
 * replay's bank-1 precedent). Resets the preimage_miss gauge first, and
 * traces stixdet_spectrum.
 */
static void stix_det_spec_build(StixDetState *d)
{
    const StixDetTapeStep *st = &d->tape_steps[d->tape_step];
    const StixDetSpec *sp = &st->spec;
    double total = stix_det_spec_total(sp);
    uint32_t fpga_det, pix, ebin;

    memset(d->spec_row, 0, sizeof(d->spec_row));
    d->spec_preimage_miss = 0;
    for (fpga_det = 0; fpga_det < STIX_DET_N_FPGA_DET; fpga_det++) {
        if (!((st->fpga_det_mask >> fpga_det) & 1)) {
            continue;
        }
        for (pix = 0; pix < STIX_DET_N_PIX_ACC; pix++) {
            double *row = d->spec_row[fpga_det][pix];

            if (!((st->pix_mask >> pix) & 1)) {
                continue;
            }
            stix_det_spec_pair(d, sp, fpga_det, pix, row);
            for (ebin = 0; ebin < STIX_DET_N_EBIN; ebin++) {
                row[ebin] /= total;
            }
        }
    }
    trace_stixdet_spectrum(d->tape_step, stix_det_spec_desc(sp),
                           qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

/*
 * ---- C1: the ADC-faithful sky mode (decision D1) ----
 *
 * Effective while the synthetic sky draws (det-source=synthetic); a
 * tape's adc_mode field overrides the knob while the tape plays and
 * reverts on stop/done. Inert otherwise (replay-dss, zero).
 */
static bool stix_det_adc_mode_on(const StixDetState *d)
{
    bool mode = d->tape_state == STIX_DET_TAPE_PLAYING ? d->tape_adc_mode
                                                       : d->adc_mode;

    return mode && d->source == STIX_DET_SOURCE_SYNTHETIC;
}

/*
 * The cached per-row bin-mass map (decision D1): map[s][t] is the
 * fraction of sim-ebin s's mass the row (pix, fpga_det) puts in ebin
 * t. Built by one walk over the row's 4096 ELUT bytes from the SRAM
 * ram pointer (the stix_det_replay_classify pattern, under the tick's
 * mem_reentrancy_guard), source ebin = the sim calibration grid (the
 * D-7 law from the row's baseline):
 * - honoured row: destination val[adc], 32 (unassigned) drops the adc;
 * - degenerate row: destination = the fallback's (== the calibration)
 *   bin, and the fallback's adc <= base drop loses the adc = base
 *   sliver of ebin 0 (1/len0: 1/72 at base 1800, 1/128 at base 0).
 * adc below the grid owns no sim ebin and is skipped. There is no
 * write hook (a TC(237,8) apply is a plain RAM store): the rebuild
 * runs at the classification points only — the SetEnable edge and the
 * start of stix_det_ql_swap, before materialize — never per tick,
 * never per event; one full 384 x 4096 walk, host ~ms, zero while the
 * mode is off. Traces stixdet_adc_map once per rebuild.
 */
static void stix_det_adc_map_rebuild(StixDetState *d)
{
    uint32_t fpga_det, pix, s, t, nonidentity = 0;
    const uint8_t *elut = memory_region_get_ram_ptr(d->sram);

    for (fpga_det = 0; fpga_det < STIX_DET_N_FPGA_DET; fpga_det++) {
        uint32_t base = stix_det_cal_base(d, fpga_det);

        for (pix = 0; pix < STIX_DET_N_PIX_ACC; pix++) {
            const uint8_t *row = elut +
                (pix * STIX_DET_N_FPGA_DET + fpga_det) * STIX_DET_N_ADC;
            double (*map)[STIX_DET_N_EBIN] = d->adc_map[fpga_det][pix];
            uint32_t counts[STIX_DET_N_EBIN][STIX_DET_N_EBIN];
            uint32_t total[STIX_DET_N_EBIN];
            uint32_t adc;
            bool degenerate = d->replay_row_fallback[fpga_det][pix];
            bool identity = true;

            memset(counts, 0, sizeof(counts));
            memset(total, 0, sizeof(total));
            for (adc = base; adc < STIX_DET_N_ADC; adc++) {
                uint32_t src = stix_det_cal_ebin(base, adc);
                uint32_t dst;

                if (degenerate) {
                    if (adc == base) {
                        continue;   /* the fallback drops adc <= base */
                    }
                    dst = src;
                } else {
                    dst = row[adc];
                    if (dst >= STIX_DET_N_EBIN) {
                        continue;   /* 32 = unassigned: the row drops it */
                    }
                }
                counts[src][dst]++;
                total[src]++;
            }
            for (s = 0; s < STIX_DET_N_EBIN; s++) {
                for (t = 0; t < STIX_DET_N_EBIN; t++) {
                    map[s][t] = total[s] ?
                                (double)counts[s][t] / total[s] : 0.0;
                }
                if (total[s] && map[s][s] != 1.0) {
                    identity = false;
                }
            }
            if (!identity) {
                nonidentity++;
            }
        }
    }
    d->adc_map_valid = true;
    d->adc_map_armed = false;
    trace_stixdet_adc_map(STIX_DET_N_FPGA_DET * STIX_DET_N_PIX_ACC,
                          nonidentity,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

/*
 * The ebin-masked share of one profile's rate after the fold: instead
 * of ebin_share[profile][s] summed over the mask, the mass of ebin s
 * reaches the masked bins t as ebin_share[s] x map[s][t] — a 32x32 dot
 * product over the cached map row (the masks still read live). Only
 * called with the map valid (the mode on).
 */
static double stix_det_adc_fold_share(const StixDetState *d, uint32_t fpga_det,
                                      uint32_t pix, uint32_t profile,
                                      uint32_t ebin_mask)
{
    const double (*map)[STIX_DET_N_EBIN] = d->adc_map[fpga_det][pix];
    double share = 0;
    uint32_t s, t;

    for (s = 0; s < STIX_DET_N_EBIN; s++) {
        double es = d->ebin_share[profile][s];

        if (es == 0.0) {
            continue;
        }
        for (t = 0; t < STIX_DET_N_EBIN; t++) {
            if ((ebin_mask >> t) & 1) {
                share += es * map[s][t];
            }
        }
    }
    return share;
}

/* start (or restart) a loaded replay at det-replay-offset-ms */
static void stix_det_replay_start(StixDetState *d, const char *why)
{
    /*
     * the offset in the model's 100 ms ticks: integer maths, sub-tick
     * offsets round down to the tick, and an event exactly at the
     * offset arrives in the first tick; the setter rejects offsets
     * past UINT32_MAX ticks
     */
    uint64_t off_ticks = d->replay_offset_ms / 100;
    uint32_t lo = 0, hi = d->replay_n_ev;

    d->replay_running = true;
    d->replay_armed = false;
    d->replay_stopped = false;
    d->replay_ended = false;
    d->replay_time_tick = (uint32_t)MIN(off_ticks, (uint64_t)UINT32_MAX);
    /*
     * the table is sorted by t_tick (the waits accumulate); the offset
     * skips the events before it
     */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;

        if (d->replay_ev[mid].t_tick < off_ticks) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    d->replay_cursor = lo;
    d->replay_events_done = 0;
    d->replay_dropped = 0;
    trace_stixdet_replay_state("start", why, d->replay_time_tick,
                               d->replay_cursor, d->replay_events_done);
}

/*
 * The SetEnable 0->1 edge (D-Dk): the nominal trigger starts the replay
 * here, and the ELUT rows are classified for the coming period (A1:
 * whenever the classification is wanted — replay-dss, det-align or the
 * ADC mode — with or without a replay table).
 */
static void stix_det_replay_enable_edge(StixDetState *d)
{
    if (stix_det_classify_wanted(d)) {
        stix_det_replay_classify(d);
    }
    d->replay_class_pending = false;
    if (d->replay_loaded && d->source == STIX_DET_SOURCE_REPLAY_DSS &&
        d->replay_armed && !d->replay_running &&
        d->replay_trigger == STIX_DET_REPLAY_TRIGGER_NOMINAL) {
        stix_det_replay_start(d, "nominal");
    }
}

/*
 * One tick of the replay (D-Dk): apply a pending det-replay-start
 * (D-Dj: the setter stores, the tick applies), advance the replay
 * clock — det-replay-offset-ms + virtual time since the start — and
 * feed every event due through stix_det_add_counts. Only with
 * det-source=replay-dss. Events the D-6 gate drops (the pix gate of
 * the event sink) are counted in replay_dropped for det-replay-status
 * (D-PLm req 4); the ELUT's own drops (an unassigned ebin, an adc at
 * or below the fallback baseline) are not counted there. At the end of
 * the table the replay stops; no loop.
 */
static void stix_det_replay_tick(StixDetState *d)
{
    if (!d->replay_loaded) {
        return;
    }
    if (d->replay_class_pending) {
        d->replay_class_pending = false;
        if (stix_det_classify_wanted(d)) {
            stix_det_replay_classify(d);
        }
    }
    if (d->replay_start_req) {
        d->replay_start_req = false;
        if (!d->replay_running && d->source == STIX_DET_SOURCE_REPLAY_DSS) {
            stix_det_replay_start(d, "qmp");
        }
    }
    if (!d->replay_running || d->source != STIX_DET_SOURCE_REPLAY_DSS) {
        return;
    }
    if (d->replay_time_tick < UINT32_MAX) {
        d->replay_time_tick++;
    }
    while (d->replay_cursor < d->replay_n_ev &&
           d->replay_ev[d->replay_cursor].t_tick <= d->replay_time_tick) {
        const StixDetReplayEvent *e = &d->replay_ev[d->replay_cursor];
        int ebin;

        d->replay_cursor++;
        d->replay_events_done++;
        if (!((stix_det_pix_gate(d, e->fpga_det) >> e->pix) & 1)) {
            /* the D-6 gate dropped it, like a real event: count it */
            d->replay_dropped++;
            continue;
        }
        ebin = stix_det_adc_ebin(d, e->fpga_det, e->pix, e->adc);
        if (ebin < 0) {
            continue;   /* unassigned, or at or below the baseline */
        }
        stix_det_add_counts(d, e->fpga_det, e->pix, ebin, 1);
    }
    if (d->replay_cursor == d->replay_n_ev) {
        d->replay_running = false;
        d->replay_ended = true;
        trace_stixdet_replay_state("eof", "end of file", d->replay_time_tick,
                                   d->replay_cursor, d->replay_events_done);
    }
}

/*
 * C1 largest-remainder split: @n units of source ebin @s go over the
 * map row's destination fractions, integer, deterministic, no new RNG
 * (same loop order, same PRNG calls — the split is pure arithmetic).
 * The row's dropped fraction is a phantom 33rd slot whose counts
 * vanish, so totals change only where the row itself drops (the
 * unassigned cells, the degenerate fallback's adc = base sliver).
 * Ties break by destination ebin, ascending.
 */
static void stix_det_adc_split_counts(const double (*map)[STIX_DET_N_EBIN],
                                      uint32_t s, uint64_t n,
                                      uint64_t dest[STIX_DET_N_EBIN])
{
    double frac[STIX_DET_N_EBIN], drop_frac, w_drop = 0.0;
    uint64_t base_sum = 0, drop_base;
    int64_t left;
    uint32_t t;

    memset(dest, 0, sizeof(*dest) * STIX_DET_N_EBIN);
    for (t = 0; t < STIX_DET_N_EBIN; t++) {
        double exact = (double)n * map[s][t];

        dest[t] = (uint64_t)exact;          /* floor: weights are >= 0 */
        frac[t] = exact - (double)dest[t];
        base_sum += dest[t];
        w_drop += map[s][t];
    }
    w_drop = 1.0 - w_drop;
    if (w_drop < 0.0) {
        w_drop = 0.0;                       /* float rounding guard */
    }
    /* the drop slot: what the row's fractions do not cover */
    drop_base = (uint64_t)((double)n * w_drop);
    drop_frac = (double)n * w_drop - (double)drop_base;
    left = (int64_t)n - (int64_t)base_sum - (int64_t)drop_base;
    while (left > 0) {
        uint32_t best = STIX_DET_N_EBIN;
        double best_frac = -1.0;

        for (t = 0; t < STIX_DET_N_EBIN; t++) {
            if (frac[t] > best_frac) {
                best_frac = frac[t];
                best = t;
            }
        }
        if (drop_frac > best_frac) {
            drop_frac = -1.0;   /* the drop slot wins: counts vanish */
        } else if (best == STIX_DET_N_EBIN) {
            break;              /* no slot left (cannot happen) */
        } else {
            dest[best]++;
            frac[best] = -1.0;
        }
        left--;
    }
}

/*
 * Draw one accumulator bank from an exposure set (D-Df): each cell
 * [pix][fpga_det][ebin] (u16 big-endian at byte ((pix * 32 + fpga_det)
 * * 32 + ebin) * 2, Detectors.h:68-79, the same layout for LACC and
 * EACC) is Poisson(sum over profiles of ebin share x exposure), plus
 * any injected counts (D-6) at the cell, wrapping at 16 bits. Adds the
 * drawn events per grp to @grp_events; returns the total. The consumed
 * deposits are cleared here.
 *
 * C1: with the ADC mode on (the map valid) each drawn count is split
 * over the row's bin-mass map by largest remainder, so the drawn ebin-s
 * mass lands where the loaded ELUT puts it; the injected counts are
 * added after the draw, at their own ebin, science-space. The Poisson
 * loop order and the RNG stream are unchanged; with the mode off the
 * writes are bit-identical to the plain path.
 *
 * S2: with @spec_any each cell also draws Poisson(@spec cell), the
 * destination-ebin spectrum exposure, after the sky's draw and fold
 * and before the deposits (counted for LTRG/ETRG as the sky). Without
 * spectrum exposure the loop is unchanged (and a zero lambda draws no
 * RNG anyway).
 */
static uint64_t stix_det_draw_acc(StixDetState *d,
                                  double (*expo_set)[STIX_DET_N_PIX_ACC]
                                                    [STIX_DET_N_PROFILE],
                                  uint32_t (*inj)[STIX_DET_N_PIX_ACC]
                                                 [STIX_DET_N_EBIN],
                                  const double (*spec)[STIX_DET_N_PIX_ACC]
                                                      [STIX_DET_N_EBIN],
                                  bool spec_any,
                                  uint8_t *acc,
                                  uint64_t grp_events[STIX_DET_N_GRP])
{
    uint64_t acc_events = 0;
    bool folded = stix_det_adc_mode_on(d) && d->adc_map_valid;
    uint32_t pix, fpga_det, ebin;

    for (pix = 0; pix < STIX_DET_N_PIX_ACC; pix++) {
        for (fpga_det = 0; fpga_det < STIX_DET_N_FPGA_DET; fpga_det++) {
            const double *expo = expo_set[fpga_det][pix];
            const double (*map)[STIX_DET_N_EBIN] = folded ?
                d->adc_map[fpga_det][pix] : NULL;
            uint64_t cell[STIX_DET_N_EBIN];
            uint64_t pair_total = 0;

            memset(cell, 0, sizeof(cell));
            for (ebin = 0; ebin < STIX_DET_N_EBIN; ebin++) {
                double lambda = d->pix_share[pix] *
                    (expo[STIX_DET_PROFILE_QUIET] *
                     d->ebin_share[STIX_DET_PROFILE_QUIET][ebin] +
                     expo[STIX_DET_PROFILE_FLARE] *
                     d->ebin_share[STIX_DET_PROFILE_FLARE][ebin]);
                uint64_t n_sky = stix_det_poisson(d, lambda);
                uint64_t n_inj = inj[fpga_det][pix][ebin];

                inj[fpga_det][pix][ebin] = 0;
                if (map) {
                    uint64_t dest[STIX_DET_N_EBIN];
                    uint32_t t;

                    stix_det_adc_split_counts(map, ebin, n_sky, dest);
                    for (t = 0; t < STIX_DET_N_EBIN; t++) {
                        cell[t] += dest[t];
                    }
                } else {
                    cell[ebin] += n_sky;
                }
                /* S2: the spectrum, ELUT-binned at build: never folded */
                if (spec_any) {
                    uint64_t n_spec =
                        stix_det_poisson(d, spec[fpga_det][pix][ebin]);

                    cell[ebin] += n_spec;
                    pair_total += n_spec;
                }
                /* the deposits stay science-space: added after the draw */
                cell[ebin] += n_inj;
                pair_total += n_sky + n_inj;
            }
            for (ebin = 0; ebin < STIX_DET_N_EBIN; ebin++) {
                stw_be_p(acc + ((pix * STIX_DET_N_FPGA_DET + fpga_det) *
                                STIX_DET_N_EBIN + ebin) * sizeof(uint16_t),
                         (uint16_t)cell[ebin]);
            }
            grp_events[stix_det_grp_of(fpga_det)] += pair_total;
            acc_events += pair_total;
        }
    }
    return acc_events;
}

/* triggers per grp: 1.25 x the events, rounded, clamped at 24 bits */
static uint32_t stix_det_trg_of(uint64_t grp_events)
{
    return MIN((5 * grp_events + 2) / 4, (uint64_t)STIX_DET_LTRG_MAX);
}

/*
 * The QL swap (D-Dd step 2, D-Df): write the finished bank's LACC in
 * full (stix_det_draw_acc), then LTRG per grp (u32 big-endian) = 1.25 x
 * the events of its fpga_det 2 * grp and 2 * grp + 1, then clear the
 * exposure for the next period. With det-source=zero the exposure is
 * zero and so is the bank.
 */
static void stix_det_sky_materialize(StixDetState *d, uint32_t ql_bank)
{
    uint8_t *lacc = (uint8_t *)memory_region_get_ram_ptr(d->sram) +
                    stix_det_lacc_off[ql_bank];
    uint64_t grp_events[STIX_DET_N_GRP] = { 0 };
    uint64_t lacc_events;
    uint32_t grp;

    lacc_events = stix_det_draw_acc(d, d->expo, d->inj_ql,
                                    (const double (*)[STIX_DET_N_PIX_ACC]
                                     [STIX_DET_N_EBIN])d->spec_expo,
                                    d->spec_expo_any, lacc, grp_events);
    memory_region_set_dirty(d->sram, stix_det_lacc_off[ql_bank],
                            STIXCTRL_LACC0_SIZE);
    /* the rewrite cures any armed upset in the bank (D-Uc) */
    stixctrl_edac_sram_write(d->owner, stix_det_lacc_off[ql_bank],
                             STIXCTRL_LACC0_SIZE);

    for (grp = 0; grp < STIX_DET_N_GRP; grp++) {
        stix_det_ram_stl(d->ltrg, stix_det_ltrg_off[ql_bank] +
                         grp * sizeof(uint32_t),
                         stix_det_trg_of(grp_events[grp]));
    }
    trace_stixdet_ql_sky(ql_bank, stix_det_source_name[d->source], d->flare,
                         lacc_events);
    stix_det_sky_clear_expo(d);
}

/*
 * ---- The nominal integration and the ROTB chunks (D-5, plan D-Dg) ----
 *
 * The integration runs on the same 100 ms tick. It ends at the first
 * tick t (ticks since its start) with
 *   ((t >= IntTimeMax + 1) || (SUMCNT >= IntEventsMax)) &&
 *   t >= IntTimeMin + 1
 * [D, TN Table 5-8; RP-0067], the parameters latched at its start
 * (fsmIntTime min 15:0 / max 31:16, fsmIntEvents; Detectors.c:1140-1149).
 * At the swap:
 * 1. the finished bank's EACC/ETRG are materialized from the
 *    integration's exposure (stix_det_draw_acc; EACC gated by SUMDMASK,
 *    the ETRG triggers not);
 * 2. ActiveNominal toggles; 3. SUMCNT restarts at 0;
 * 4. with det-rotb on, the bank goes into the ROTB ring as one 49-block
 *    chunk (stixctrl_rotb_append: rotbConfig ENABLE = 1 and TESTMODE =
 *    0; all or nothing with the default drop-new policy, overwritten in
 *    oldest-first order with rotb-overflow=overwrite-oldest, RF-0). The
 *    FSM is enabled here by construction: the tick only runs while it is;
 * 5. the finished bank is zeroed (RotbCtrl's read and clear [D]), also
 *    when no chunk was taken, so every integration starts from zero.
 * The FSW never reads EACC/ETRG in NOMINAL; it reads the chunk
 * (RotAccumulator_doTask, RotAccuCompactor_processDetectorBlock).
 */

/* RotAccumulator.h: descriptor type 0x8000 = detector data */
#define STIX_DET_CHUNK_DESC_DETECTOR 0x8000u
/* data at +8 in every block: u16 descriptor, u32 coarse, u16 fine */
#define STIX_DET_CHUNK_DATA_OFF 8u
#define STIX_DET_CHUNK_DATA_BYTES \
    (STIXCTRL_BLOCK_SIZE - STIX_DET_CHUNK_DATA_OFF)
/* the UNSYNCED status flag, bit 31 of the coarse SCET register (0xC8) */
#define STIX_DET_SCET_UNSYNCED 0x80000000u

static const char *const stix_det_rotb_result_name[] = {
    [STIX_DET_ROTB_APPENDED] = "appended",
    [STIX_DET_ROTB_DISABLED] = "rotb-disabled",
    [STIX_DET_ROTB_TESTMODE] = "rotb-testmode",
    [STIX_DET_ROTB_FULL] = "ring-full",
};

static void stix_det_nom_bank_zero(StixDetState *d, uint32_t nom_bank)
{
    stix_det_ram_zero(d->sram, stix_det_eacc_off[nom_bank],
                      STIXCTRL_EACC0_SIZE);
    /* the FPGA rewrite cures any armed upset (D-Uc host-write rule) */
    stixctrl_edac_sram_write(d->owner, stix_det_eacc_off[nom_bank],
                             STIXCTRL_EACC0_SIZE);
    stix_det_ram_zero(d->etrg, stix_det_etrg_off[nom_bank],
                      STIXCTRL_ETRG0_SIZE);
}

static void stix_det_nom_clear_expo(StixDetState *d)
{
    memset(d->nom_expo, 0, sizeof(d->nom_expo));
    memset(d->nom_trg_extra_grp, 0, sizeof(d->nom_trg_extra_grp));
    /* the injected counts belong to the integration, like the exposure */
    memset(d->inj_nom, 0, sizeof(d->inj_nom));
    if (d->nom_spec_expo_any) {
        memset(d->nom_spec_expo, 0, sizeof(d->nom_spec_expo));
        d->nom_spec_expo_any = false;
    }
}

/* the parameters are latched at the start of each integration */
static void stix_det_nom_latch(StixDetState *d)
{
    d->nom_int_time_min = (d->reg->fsmIntTime >>
                           STIXCTRL_FSMINTTIME_INTTIMEMIN) &
                          STIXCTRL_FSMINTTIME_INTTIMEMIN_MASK;
    d->nom_int_time_max = (d->reg->fsmIntTime >>
                           STIXCTRL_FSMINTTIME_INTTIMEMAX) &
                          STIXCTRL_FSMINTTIME_INTTIMEMAX_MASK;
    d->nom_int_events_max = d->reg->fsmIntEvents;
}

/*
 * a fresh enable at the start of an integration: latch, and no stale
 * RAM (EACC/ETRG survive a reset, §13) or exposure in the active bank
 */
static void stix_det_nom_fresh(StixDetState *d)
{
    stix_det_nom_latch(d);
    stix_det_nom_bank_zero(d, d->active_nom_bank);
    stix_det_nom_clear_expo(d);
}

/*
 * EACC/ETRG of the finished integration (step 1). ETRG per grp counts
 * the triggers of both its fpga_det: the drawn EACC events plus a draw
 * of the events of fpga_det outside SUMDMASK. Returns the EACC events.
 */
static uint64_t stix_det_nom_materialize(StixDetState *d, uint32_t nom_bank)
{
    uint8_t *eacc = (uint8_t *)memory_region_get_ram_ptr(d->sram) +
                    stix_det_eacc_off[nom_bank];
    uint64_t grp_events[STIX_DET_N_GRP] = { 0 };
    uint64_t eacc_events;
    uint32_t grp;

    eacc_events = stix_det_draw_acc(d, d->nom_expo, d->inj_nom,
                                    (const double (*)[STIX_DET_N_PIX_ACC]
                                     [STIX_DET_N_EBIN])d->nom_spec_expo,
                                    d->nom_spec_expo_any, eacc, grp_events);
    memory_region_set_dirty(d->sram, stix_det_eacc_off[nom_bank],
                            STIXCTRL_EACC0_SIZE);
    /* the rewrite cures any armed upset in the bank (D-Uc) */
    stixctrl_edac_sram_write(d->owner, stix_det_eacc_off[nom_bank],
                             STIXCTRL_EACC0_SIZE);
    for (grp = 0; grp < STIX_DET_N_GRP; grp++) {
        uint64_t ev = grp_events[grp] +
                      stix_det_poisson(d, d->nom_trg_extra_grp[grp]);

        stix_det_ram_stl(d->etrg, stix_det_etrg_off[nom_bank] +
                         grp * sizeof(uint32_t), stix_det_trg_of(ev));
    }
    stix_det_nom_clear_expo(d);
    return eacc_events;
}

/*
 * Build the chunk of one finished bank (RotAccuCompactor.c:76-270):
 * block n (0..48) = u16 descriptor 0x8000 | n, the SCET as 0xC8/0xCC
 * read at the integration end (coarse u32 with UNSYNCED in bit 31, fine
 * u16; ArchiveManager.c:131 masks the flag), then the next 504 bytes of
 * the stream EACC (0x6000, [pix][fpga_det][ebin] u16) + ETRG (0x40, u32
 * per grp, read as hw/lw u16 pairs), zero-padded (56 B in block 48).
 * Everything is big-endian, as the banks are.
 */
static void stix_det_chunk_build(StixDetState *d, uint32_t nom_bank,
                                 uint32_t coarse_reg, uint16_t fine)
{
    const uint8_t *part[2] = {
        (uint8_t *)memory_region_get_ram_ptr(d->sram) +
            stix_det_eacc_off[nom_bank],
        (uint8_t *)memory_region_get_ram_ptr(d->etrg) +
            stix_det_etrg_off[nom_bank],
    };
    const uint32_t part_len[2] = { STIXCTRL_EACC0_SIZE, STIXCTRL_ETRG0_SIZE };
    uint32_t n, i = 0, part_off = 0;

    memset(d->chunk, 0, sizeof(d->chunk));
    for (n = 0; n < STIX_DET_CHUNK_BLOCKS; n++) {
        uint8_t *blk = d->chunk + n * STIXCTRL_BLOCK_SIZE;
        uint32_t done = 0;

        stw_be_p(blk, STIX_DET_CHUNK_DESC_DETECTOR | n);
        stl_be_p(blk + 2, coarse_reg);
        stw_be_p(blk + 6, fine);
        while (done < STIX_DET_CHUNK_DATA_BYTES && i < 2) {
            uint32_t len = MIN(STIX_DET_CHUNK_DATA_BYTES - done,
                               part_len[i] - part_off);

            memcpy(blk + STIX_DET_CHUNK_DATA_OFF + done,
                   part[i] + part_off, len);
            done += len;
            part_off += len;
            if (part_off == part_len[i]) {
                i++;
                part_off = 0;
            }
        }
    }
}

/* the nominal swap (steps 1-5 above) */
static void stix_det_nominal_swap(StixDetState *d)
{
    uint32_t done = d->active_nom_bank;
    uint32_t int_ticks = d->nom_tick;
    uint32_t sumcnt = d->sumcnt;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t eacc_events = stix_det_nom_materialize(d, done);
    StixDetRotbResult r;
    uint32_t coarse_reg, rotb_count = 0;
    uint16_t fine;

    d->active_nom_bank = done ^ 1;
    d->sumcnt = 0;
    d->nom_tick = 0;
    stix_det_nom_latch(d);
    trace_stixdet_nominal_swap(done, d->active_nom_bank, int_ticks, sumcnt,
                               eacc_events, now);

    if (d->rotb) {
        stixctrl_scet_reg(d->owner, &coarse_reg, &fine);
        stix_det_chunk_build(d, done, coarse_reg, fine);
        r = stixctrl_rotb_append(d->owner, d->chunk, STIX_DET_CHUNK_BLOCKS,
                                 &rotb_count);
        if (r == STIX_DET_ROTB_APPENDED || r == STIX_DET_ROTB_FULL) {
            /* what a det-rotb-burst repeats (its `last`) */
            d->last_chunk_coarse = coarse_reg;
            d->last_chunk_fine = fine;
            d->last_chunk_valid = true;
        }
        if (r == STIX_DET_ROTB_APPENDED) {
            trace_stixdet_chunk(done, STIX_DET_CHUNK_BLOCKS, coarse_reg, fine,
                                int_ticks, eacc_events, rotb_count, now);
        } else {
            trace_stixdet_chunk_drop(stix_det_rotb_result_name[r], done,
                                     int_ticks, rotb_count, now);
        }
    } else {
        trace_stixdet_chunk_drop("det-rotb-off", done, int_ticks, 0, now);
    }
    stix_det_nom_bank_zero(d, done);
}

/*
 * det-rotb-burst (FF plan): append @n copies of the last live chunk
 * (d->chunk) to the ROTB now. Frame k = 1..n carries the SCET
 * last + (now - last) * k / n on the 48-bit coarse:fine value, the
 * UNSYNCED flag from the current register; with the clock paused or
 * stepped back (now <= last) the delta is 0, so every frame carries
 * `last` and the timestamps never go backwards. Rejected with
 * error_setg and nothing appended: @n = 0, more frames than the ring's
 * block capacity holds chunks, det-rotb off, and no live chunk since
 * the last device reset. An append that is not APPENDED stops the
 * burst — not an error: the status and the trace say how many frames
 * went in and why. Afterwards last = now, so the next live chunk is
 * later than every burst frame.
 */
static void stix_det_rotb_burst(StixDetState *d, uint32_t n, Error **errp)
{
    const char *stop = "done";
    uint32_t coarse_reg, rotb_count = 0, k, appended = 0;
    uint16_t fine;
    uint64_t last48, now48, delta;

    if (n == 0) {
        error_setg(errp, "det-rotb-burst: 0 frames appends nothing");
        return;
    }
    if (n > STIXCTRL_ROTB_MAX_BLOCKS / STIX_DET_CHUNK_BLOCKS) {
        error_setg(errp, "det-rotb-burst: %u frames is past the %u "
                   "chunks the ring's %u blocks hold", n,
                   STIXCTRL_ROTB_MAX_BLOCKS / STIX_DET_CHUNK_BLOCKS,
                   STIXCTRL_ROTB_MAX_BLOCKS);
        return;
    }
    if (!d->rotb) {
        error_setg(errp, "det-rotb-burst: det-rotb is off");
        return;
    }
    if (!d->last_chunk_valid) {
        error_setg(errp, "det-rotb-burst repeats the last live frame: "
                   "none yet");
        return;
    }
    stixctrl_scet_reg(d->owner, &coarse_reg, &fine);
    last48 = ((uint64_t)(d->last_chunk_coarse & ~STIX_DET_SCET_UNSYNCED)
              << 16) | d->last_chunk_fine;
    now48 = ((uint64_t)(coarse_reg & ~STIX_DET_SCET_UNSYNCED) << 16) | fine;
    delta = now48 > last48 ? now48 - last48 : 0;

    for (k = 1; k <= n; k++) {
        StixDetRotbResult r;
        uint64_t scet48 = last48 + delta * k / n;
        uint32_t coarse = (uint32_t)(scet48 >> 16) |
                          (coarse_reg & STIX_DET_SCET_UNSYNCED);
        uint16_t fine16 = (uint16_t)scet48;
        uint32_t b;

        for (b = 0; b < STIX_DET_CHUNK_BLOCKS; b++) {
            uint8_t *blk = d->chunk + b * STIXCTRL_BLOCK_SIZE;

            stl_be_p(blk + 2, coarse);
            stw_be_p(blk + 6, fine16);
        }
        r = stixctrl_rotb_append(d->owner, d->chunk, STIX_DET_CHUNK_BLOCKS,
                                 &rotb_count);
        if (r != STIX_DET_ROTB_APPENDED) {
            stop = stix_det_rotb_result_name[r];
            break;
        }
        appended++;
    }
    /* last = now: the next live chunk is later than every burst frame */
    d->last_chunk_coarse = coarse_reg;
    d->last_chunk_fine = fine;
    d->burst_requested = n;
    d->burst_appended = appended;
    d->burst_stop = stop;
    d->burst_rotb_count = rotb_count;
    trace_stixdet_rotb_burst(n, appended, stop, rotb_count,
                             qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

/* one tick of the running integration: count it, swap when it ends */
static void stix_det_nominal_tick(StixDetState *d)
{
    d->nom_tick++;
    if ((d->nom_tick >= d->nom_int_time_max + 1 ||
         d->sumcnt >= d->nom_int_events_max) &&
        d->nom_tick >= d->nom_int_time_min + 1) {
        stix_det_nominal_swap(d);
    }
}

/* ---- S1: the mix tape (steps on guest time, a background layer) ---- */

static const char *const stix_det_tape_kind_name[STIX_DET_TAPE_KIND__MAX] = {
    [STIX_DET_TAPE_RATE] = "rate",
    [STIX_DET_TAPE_REPLAY] = "replay",
    [STIX_DET_TAPE_OFF] = "off",
    [STIX_DET_TAPE_SINGLE] = "single",
};

static const char *const stix_det_tape_state_name[STIX_DET_TAPE__MAX] = {
    [STIX_DET_TAPE_IDLE] = "idle",
    [STIX_DET_TAPE_READY] = "ready",
    [STIX_DET_TAPE_PLAYING] = "playing",
    [STIX_DET_TAPE_DONE] = "done",
    [STIX_DET_TAPE_STOPPED] = "stopped",
};

/* for_s is tick-quantized: whole 100 ms ticks, 0.1..86400 s */
static uint32_t stix_det_tape_step_ticks(double for_s)
{
    return (uint32_t)(for_s * 10.0 + 0.5);  /* 10 ticks per second */
}

/*
 * The per-pair rate while a tape plays (T2): the active step's cps on
 * its masks (kind off contributes 0), else the background's cps (the
 * background is unmasked: it fills all 384 pairs). The scene knobs are
 * ignored while playing and resume on stop/done.
 */
static uint32_t stix_det_tape_pair_cps(const StixDetState *d,
                                       uint32_t fpga_det, uint32_t pix)
{
    const StixDetTapeStep *st = &d->tape_steps[d->tape_step];

    if (((st->fpga_det_mask >> fpga_det) & 1) &&
        ((st->pix_mask >> pix) & 1)) {
        return st->kind == STIX_DET_TAPE_OFF ? 0 : st->cps;
    }
    return d->tape_bg_cps;
}

/* drop the loaded tape (steps and name); the caller sets the state */
static void stix_det_tape_clear(StixDetState *d)
{
    uint32_t i;

    for (i = 0; i < d->tape_n_steps; i++) {
        g_free(d->tape_steps[i].label);
        stix_det_spec_free(&d->tape_steps[i].spec);
    }
    d->tape_n_steps = 0;
    d->tape_step = 0;
    d->tape_left_ticks = 0;
    g_clear_pointer(&d->tape_name, g_free);
    d->tape_loop = false;
    d->tape_align = false;
    d->tape_adc_mode = false;
    d->tape_bg_cps = 0;
    d->tape_in_replay = false;
}

/*
 * Stop a playing tape: rates revert to the scene knobs at the next
 * tick (the sky tick reads the state), a running replay step stops the
 * replay the way det-replay-start=false does and reverts the source.
 */
static void stix_det_tape_stop(StixDetState *d, const char *why)
{
    if (d->tape_state != STIX_DET_TAPE_PLAYING) {
        return;
    }
    if (d->tape_in_replay) {
        d->tape_in_replay = false;
        if (d->replay_running) {
            d->replay_running = false;
            d->replay_stopped = true;
            trace_stixdet_replay_state("stop", why, d->replay_time_tick,
                                       d->replay_cursor,
                                       d->replay_events_done);
        }
        d->source = STIX_DET_SOURCE_SYNTHETIC;
    }
    d->tape_state = STIX_DET_TAPE_STOPPED;
    d->tape_left_ticks = 0;
}

/* a step starts: trace it, and a replay step switches to replay-dss */
static void stix_det_tape_apply_step(StixDetState *d, uint32_t idx)
{
    const StixDetTapeStep *st = &d->tape_steps[idx];

    d->tape_step = idx;
    d->tape_left_ticks = stix_det_tape_step_ticks(st->for_s);
    /* S2: the preimage_miss gauge belongs to the running step */
    d->spec_preimage_miss = 0;
    /* S3: so does the single step's schedule cursor */
    d->single_next = 0;
    trace_stixdet_tape_step(idx, stix_det_tape_kind_name[st->kind],
                            st->label ? st->label : "",
                            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    if (stix_det_spec_active(d)) {
        /* fresh row states, then the row (in the tick, under its guard) */
        stix_det_replay_classify(d);
        stix_det_spec_build(d);
    }
    if (st->kind != STIX_DET_TAPE_REPLAY) {
        return;
    }
    d->tape_in_replay = true;
    d->source = STIX_DET_SOURCE_REPLAY_DSS;
    /* the rows were not classified for this source yet (D-Dj) */
    d->replay_class_pending = true;
    /* replay_tick applies the start later in this same tick */
    d->replay_start_req = true;
}

/*
 * One tick of the tape (D-Dj): the play/stop requests apply, then the
 * running step counts its tick down. The step clock advances only
 * here — i.e. only while the tick runs, FSM Enable = 1 (D3: SAFE
 * freezes playback). A step owns the ticks its for_s quantizes to; at
 * its end the next step (or the loop restart, or done) applies, and a
 * replay step's end stops the replay engine and reverts the source.
 */
static void stix_det_tape_tick(StixDetState *d)
{
    if (d->tape_stop_req) {
        d->tape_stop_req = false;
        stix_det_tape_stop(d, "det-tape-play=false");
    }
    if (d->tape_play_req) {
        d->tape_play_req = false;
        if (d->tape_state == STIX_DET_TAPE_PLAYING || !d->tape_n_steps) {
            return;     /* the setter refused these; belt and braces */
        }
        d->tape_state = STIX_DET_TAPE_PLAYING;
        /* S3: the drop gauge sums the whole play (plan review M2) */
        d->single_dropped = 0;
        stix_det_tape_apply_step(d, 0);
        return;
    }
    if (d->tape_state != STIX_DET_TAPE_PLAYING) {
        return;
    }
    if (d->tape_left_ticks > 0) {
        d->tape_left_ticks--;
    }
    if (d->tape_left_ticks == 0) {
        const StixDetTapeStep *st = &d->tape_steps[d->tape_step];

        if (st->kind == STIX_DET_TAPE_REPLAY) {
            d->tape_in_replay = false;
            if (d->replay_running) {
                d->replay_running = false;
                d->replay_stopped = true;
                trace_stixdet_replay_state("stop", "tape step end",
                                           d->replay_time_tick,
                                           d->replay_cursor,
                                           d->replay_events_done);
            }
            d->source = STIX_DET_SOURCE_SYNTHETIC;
        }
        if (d->tape_step + 1 < d->tape_n_steps) {
            stix_det_tape_apply_step(d, d->tape_step + 1);
        } else if (d->tape_loop) {
            /* a loop restart is a new play for the drop gauge (M2) */
            d->single_dropped = 0;
            stix_det_tape_apply_step(d, 0);
        } else {
            d->tape_state = STIX_DET_TAPE_DONE;
            d->tape_left_ticks = 0;
        }
    }
}

/* ---- det-tape-load: the JSON parser ---- */

/* a status line must stay one line: names and labels hold no comma */
static bool stix_det_tape_line_text(const char *s, uint32_t max,
                                    Error **errp, const char *what)
{
    uint32_t i, len = strlen(s);

    if (!len || len > max) {
        error_setg(errp, "det-tape-load: %s is empty or past %u "
                   "characters", what, max);
        return false;
    }
    for (i = 0; i < len; i++) {
        if (s[i] == ',' || (unsigned char)s[i] < 0x20 ||
            (unsigned char)s[i] == 0x7F) {
            error_setg(errp, "det-tape-load: %s holds a comma or a "
                       "control character", what);
            return false;
        }
    }
    return true;
}

/* a JSON number out of a dict member; false with errp when absent or
 * not a number; @idx names the step (UINT32_MAX = the tape itself) */
static bool stix_det_tape_num(QDict *obj, const char *key, double *out,
                              Error **errp, uint32_t idx)
{
    QNum *n = qobject_to(QNum, qdict_get(obj, key));

    if (n) {
        *out = qnum_get_double(n);
    }
    if (!n) {
        if (idx == UINT32_MAX) {
            error_setg(errp, "det-tape-load: '%s' is missing or not a "
                       "number", key);
        } else {
            error_setg(errp, "det-tape-load: step %u: '%s' is missing or "
                       "not a number", idx, key);
        }
        return false;
    }
    return true;
}

/* a JSON integer in 0..hi */
static bool stix_det_tape_uint(QDict *obj, const char *key, uint64_t hi,
                               uint64_t *out, Error **errp, uint32_t idx)
{
    QNum *n = qobject_to(QNum, qdict_get(obj, key));
    int64_t v;

    if (!n || !qnum_get_try_int(n, &v) || v < 0 || (uint64_t)v > hi) {
        if (idx == UINT32_MAX) {
            error_setg(errp, "det-tape-load: '%s' is missing or not an "
                       "integer in 0..%" PRIu64, key, hi);
        } else {
            error_setg(errp, "det-tape-load: step %u: '%s' is missing or "
                       "not an integer in 0..%" PRIu64, idx, key, hi);
        }
        return false;
    }
    *out = (uint64_t)v;
    return true;
}

/* an optional JSON bool with a default */
static bool stix_det_tape_bool(QDict *obj, const char *key, bool def,
                               bool *out, Error **errp, uint32_t idx)
{
    QObject *o = qdict_get(obj, key);
    QBool *b = qobject_to(QBool, o);

    if (!o) {
        *out = def;
        return true;
    }
    if (!b) {
        if (idx == UINT32_MAX) {
            error_setg(errp, "det-tape-load: '%s' is not a boolean", key);
        } else {
            error_setg(errp, "det-tape-load: step %u: '%s' is not a "
                       "boolean", idx, key);
        }
        return false;
    }
    *out = qbool_get_bool(b);
    return true;
}

/* ---- S2: the spectrum spec (det-tape-load) ---- */

static void stix_det_spec_free(StixDetSpec *sp)
{
    g_free(sp->pts);
    g_free(sp->desc);
    memset(sp, 0, sizeof(*sp));
}

static const char *stix_det_spec_desc(const StixDetSpec *sp)
{
    return sp->desc ? sp->desc : "flat";
}

/*
 * One CSV number (S2 rule table): [0-9]+(\.[0-9]+)?([eE][-+]?[0-9]+)?,
 * or [0-9]+ only with @int_only; finite. No sign, no inf/nan, no 0x, no
 * spaces: the grammar is checked by hand before g_ascii_strtod sees the
 * text, so the libc's wider syntax never applies.
 */
static bool stix_det_spec_csv_num(const char *s, size_t len, bool int_only,
                                  double *out)
{
    g_autofree char *txt = NULL;
    char *end;
    size_t i = 0, n;

    for (n = 0; i < len && g_ascii_isdigit(s[i]); i++, n++) {
        /* integer part */
    }
    if (!n) {
        return false;
    }
    if (!int_only && i < len && s[i] == '.') {
        for (i++, n = 0; i < len && g_ascii_isdigit(s[i]); i++, n++) {
            /* fraction */
        }
        if (!n) {
            return false;
        }
    }
    if (!int_only && i < len && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < len && (s[i] == '+' || s[i] == '-')) {
            i++;
        }
        for (n = 0; i < len && g_ascii_isdigit(s[i]); i++, n++) {
            /* exponent */
        }
        if (!n) {
            return false;
        }
    }
    if (i != len) {
        return false;
    }
    txt = g_strndup(s, len);
    *out = g_ascii_strtod(txt, &end);
    return end == txt + len && isfinite(*out);
}

static int stix_det_spec_pt_cmp(const void *a, const void *b)
{
    double va = ((const StixDetSpecPoint *)a)->value;
    double vb = ((const StixDetSpecPoint *)b)->value;

    return va < vb ? -1 : va > vb;
}

/*
 * Read a spectrum CSV (D4) once, host-side, in the det-tape-load setter
 * (never in the tick): the size is checked before reading (<= 256 KiB),
 * the first line is exactly "adc,weight" or "kev,weight", then 1..4096
 * lines "value,weight" (no spaces, an optional \r before \n, the final
 * newline optional, no blank or comment lines); adc values are
 * integers 0..4095, keV values numbers (no K bound at load, decision
 * Q7: @kev_full is not consulted, K is read at the row build); weights
 * >= 0 with a sum > 0; repeated values are summed. The parsed points
 * are owned by @sp: a later change or deletion of the file does not
 * affect the loaded tape. Every error names the step, the path and the
 * line.
 */
static bool stix_det_spec_file_load(const char *path, uint32_t kev_full,
                                    StixDetSpec *sp, uint32_t idx,
                                    Error **errp)
{
    g_autofree char *text = NULL, *base = NULL;
    g_autofree StixDetSpecPoint *pts = NULL;
    GError *gerr = NULL;
    GStatBuf stb;
    gsize len;
    const char *p, *end;
    uint32_t lineno = 0, rows = 0, n, i;
    bool kev = false;
    double sum = 0;

    (void)kev_full;     /* Q7: K is read when the row is built */
    if (g_stat(path, &stb) < 0) {
        error_setg(errp, "det-tape-load: step %u: spectrum file '%s': %s",
                   idx, path, g_strerror(errno));
        return false;
    }
    if (!S_ISREG(stb.st_mode)) {
        error_setg(errp, "det-tape-load: step %u: spectrum file '%s' is "
                   "not a regular file", idx, path);
        return false;
    }
    if (stb.st_size > STIX_DET_SPEC_MAX_FILE) {
        error_setg(errp, "det-tape-load: step %u: spectrum file '%s' is "
                   "%" PRId64 " bytes, past the %u KiB cap", idx, path,
                   (int64_t)stb.st_size, STIX_DET_SPEC_MAX_FILE / 1024u);
        return false;
    }
    if (!g_file_get_contents(path, &text, &len, &gerr)) {
        error_setg(errp, "det-tape-load: step %u: spectrum file '%s': %s",
                   idx, path, gerr->message);
        g_error_free(gerr);
        return false;
    }
    if (len > STIX_DET_SPEC_MAX_FILE) {     /* it grew since the stat */
        error_setg(errp, "det-tape-load: step %u: spectrum file '%s' is "
                   "past the %u KiB cap", idx, path,
                   STIX_DET_SPEC_MAX_FILE / 1024u);
        return false;
    }
    pts = g_new(StixDetSpecPoint, STIX_DET_SPEC_MAX_POINTS);
    p = text;
    end = text + len;
    while (p < end || lineno == 0) {
        const char *eol = memchr(p, '\n', end - p);
        size_t ll = (eol ? eol : end) - p;
        const char *comma;
        double v, w;

        lineno++;
        if (eol && ll && p[ll - 1] == '\r') {
            ll--;
        }
        if (lineno == 1) {
            if (ll == 10 && !memcmp(p, "adc,weight", 10)) {
                kev = false;
            } else if (ll == 10 && !memcmp(p, "kev,weight", 10)) {
                kev = true;
            } else {
                error_setg(errp, "det-tape-load: step %u: spectrum file "
                           "'%s' line 1: the header must be exactly "
                           "\"adc,weight\" or \"kev,weight\"", idx, path);
                return false;
            }
        } else {
            if (rows == STIX_DET_SPEC_MAX_POINTS) {
                error_setg(errp, "det-tape-load: step %u: spectrum file "
                           "'%s' line %u: more than %u data rows", idx,
                           path, lineno, STIX_DET_SPEC_MAX_POINTS);
                return false;
            }
            comma = ll ? memchr(p, ',', ll) : NULL;
            if (!comma ||
                !stix_det_spec_csv_num(p, comma - p, !kev, &v) ||
                !stix_det_spec_csv_num(comma + 1, p + ll - comma - 1, false,
                                       &w) ||
                (!kev && v > STIX_DET_N_ADC - 1)) {
                error_setg(errp, "det-tape-load: step %u: spectrum file "
                           "'%s' line %u: not \"%s,weight\" (%s, weight "
                           "a number >= 0; no spaces, no blank lines)",
                           idx, path, lineno, kev ? "kev" : "adc",
                           kev ? "keV a number" : "adc an integer 0..4095");
                return false;
            }
            pts[rows].value = v;
            pts[rows].weight = w;
            sum += w;
            rows++;
        }
        p = eol ? eol + 1 : end;
    }
    if (!rows) {
        error_setg(errp, "det-tape-load: step %u: spectrum file '%s': no "
                   "data rows (1..%u)", idx, path, STIX_DET_SPEC_MAX_POINTS);
        return false;
    }
    if (!(sum > 0)) {
        error_setg(errp, "det-tape-load: step %u: spectrum file '%s': the "
                   "weights sum to 0", idx, path);
        return false;
    }
    if (!isfinite(sum)) {
        error_setg(errp, "det-tape-load: step %u: spectrum file '%s': the "
                   "weights sum is not finite", idx, path);
        return false;
    }
    /* repeated values are summed: sort, then merge equal neighbours */
    qsort(pts, rows, sizeof(*pts), stix_det_spec_pt_cmp);
    for (i = 1, n = 1; i < rows; i++) {
        if (pts[i].value == pts[n - 1].value) {
            pts[n - 1].weight += pts[i].weight;
        } else {
            pts[n++] = pts[i];
        }
    }
    sp->kind = kev ? STIX_DET_SPEC_FILE_KEV : STIX_DET_SPEC_FILE_ADC;
    sp->n_pts = n;
    sp->pts = g_renew(StixDetSpecPoint, g_steal_pointer(&pts), n);
    base = g_path_get_basename(path);
    sp->desc = g_strdup_printf("file=%s unit=%s rows=%u", base,
                               kev ? "kev" : "adc", rows);
    return true;
}

/*
 * The step's "spec" and "align" into st->spec (S2 rule table): absent
 * or "flat" on any kind (the S1 template), else an object with exactly
 * one key of line_adc (an integer 0..4095), line_kev (a finite number
 * >= 0), flat_kev ([k1, k2], 0 <= k1 < k2) or file (a path, 1..4095
 * bytes, no comma; read here) — only on kind rate (a single step is
 * refused, and its align decided, before this runs: S3 M5). keV values
 * have no K
 * bound at load (decision Q7). The step's align defaults to the tape's
 * @tape_align; an explicit true is only valid on a keV spec, the tape
 * default is ignored for adc and flat specs. Called last in the step
 * parse, so no later refusal leaves a loaded spec behind; on an error
 * st->spec is left empty.
 */
static bool stix_det_spec_parse(StixDetState *d, QDict *step,
                                bool tape_align, StixDetTapeStep *st,
                                uint32_t idx, Error **errp)
{
    static const char spec_err[] = "'spec' must be \"flat\" or an object "
                                   "with exactly one key of line_adc, "
                                   "line_kev, flat_kev, file";
    StixDetSpec *sp = &st->spec;
    QObject *o = qdict_get(step, "spec");
    QDict *sd = qobject_to(QDict, o);
    QString *str = qobject_to(QString, o);
    bool align_set = qdict_haskey(step, "align");
    bool align, kev;

    memset(sp, 0, sizeof(*sp));
    if (!stix_det_tape_bool(step, "align", tape_align, &align, errp, idx)) {
        return false;
    }
    if (!o || (str && !strcmp(qstring_get_str(str), "flat"))) {
        sp->kind = STIX_DET_SPEC_FLAT;
    } else if (!sd || qdict_size(sd) != 1) {
        error_setg(errp, "det-tape-load: step %u: %s", idx, spec_err);
        return false;
    } else if (st->kind != STIX_DET_TAPE_RATE) {
        error_setg(errp, "det-tape-load: step %u: a non-flat spec is only "
                   "valid on kind rate", idx);
        return false;
    } else {
        const char *key = qdict_entry_key(qdict_first(sd));
        QObject *val = qdict_get(sd, key);
        QNum *n = qobject_to(QNum, val);

        if (!strcmp(key, "line_adc")) {
            int64_t v;

            if (!n || !qnum_get_try_int(n, &v) || v < 0 ||
                v > STIX_DET_N_ADC - 1) {
                error_setg(errp, "det-tape-load: step %u: 'line_adc' is "
                           "not an integer in 0..%u", idx,
                           STIX_DET_N_ADC - 1);
                return false;
            }
            sp->kind = STIX_DET_SPEC_LINE_ADC;
            sp->v0 = v;
        } else if (!strcmp(key, "line_kev")) {
            double v = n ? qnum_get_double(n) : -1;

            if (!n || !isfinite(v) || v < 0) {
                error_setg(errp, "det-tape-load: step %u: 'line_kev' is "
                           "not a finite number >= 0 (keV)", idx);
                return false;
            }
            sp->kind = STIX_DET_SPEC_LINE_KEV;
            sp->v0 = v;
        } else if (!strcmp(key, "flat_kev")) {
            QList *l = qobject_to(QList, val);
            const QListEntry *e0 = l && qlist_size(l) == 2 ?
                                   qlist_first(l) : NULL;
            QNum *n0 = e0 ? qobject_to(QNum, qlist_entry_obj(e0)) : NULL;
            QNum *n1 = n0 ? qobject_to(QNum,
                                       qlist_entry_obj(qlist_next(e0)))
                          : NULL;
            double k1 = n1 ? qnum_get_double(n0) : -1;
            double k2 = n1 ? qnum_get_double(n1) : -1;

            if (!n1 || !isfinite(k1) || !isfinite(k2) || k1 < 0 ||
                !(k1 < k2)) {
                error_setg(errp, "det-tape-load: step %u: 'flat_kev' is "
                           "not [k1, k2] with 0 <= k1 < k2 (keV)", idx);
                return false;
            }
            sp->kind = STIX_DET_SPEC_FLAT_KEV;
            sp->v0 = k1;
            sp->v1 = k2;
        } else if (!strcmp(key, "file")) {
            QString *fs = qobject_to(QString, val);
            const char *path = fs ? qstring_get_str(fs) : NULL;
            size_t len = path ? strlen(path) : 0;

            if (!path || !len || len > 4095 || strchr(path, ',')) {
                error_setg(errp, "det-tape-load: step %u: 'file' is not a "
                           "path of 1..4095 bytes without a comma", idx);
                return false;
            }
            if (!stix_det_spec_file_load(path, d->adc_kev_full, sp, idx,
                                         errp)) {
                stix_det_spec_free(sp);
                return false;
            }
        } else {
            error_setg(errp, "det-tape-load: step %u: %s (not '%s')", idx,
                       spec_err, key);
            return false;
        }
    }
    kev = sp->kind == STIX_DET_SPEC_LINE_KEV ||
          sp->kind == STIX_DET_SPEC_FLAT_KEV ||
          sp->kind == STIX_DET_SPEC_FILE_KEV;
    if (align_set && align && !kev) {
        error_setg(errp, "det-tape-load: step %u: align applies to keV "
                   "specs (line_kev, flat_kev, a kev,weight file)", idx);
        stix_det_spec_free(sp);
        return false;
    }
    sp->align = kev && align;
    switch (sp->kind) {
    case STIX_DET_SPEC_FLAT:
        sp->desc = g_strdup("flat");
        break;
    case STIX_DET_SPEC_LINE_ADC:
        sp->desc = g_strdup_printf("line_adc=%u", (uint32_t)sp->v0);
        break;
    case STIX_DET_SPEC_LINE_KEV:
        sp->desc = g_strdup_printf("line_kev=%g", sp->v0);
        break;
    case STIX_DET_SPEC_FLAT_KEV:
        sp->desc = g_strdup_printf("flat_kev=%g..%g", sp->v0, sp->v1);
        break;
    default:
        break;      /* the file loader wrote it */
    }
    if (sp->align) {
        char *desc = g_strconcat(sp->desc, " align", NULL);

        g_free(sp->desc);
        sp->desc = desc;
    }
    return true;
}

/*
 * S3: the single fields of one step dict into @st (the single-events
 * plan's rule table; the platform's tapes.py mirrors it): fpga_det, pix
 * and n required integers, exactly one of adc/ebin, wait_us optional
 * (default 20), no spec, align (the step's, else the tape's) an
 * explicit true only with ebin, and for_s must cover n*wait_us at tick
 * quantization (owner decision 1). The implicit masks are the one pair
 * and cps 0: a single step contributes no sky rate (the feed is V2).
 * Called from the single branch of stix_det_tape_parse_step, before the
 * common code re-reads for_s.
 */
static bool stix_det_single_parse(StixDetState *d, QDict *obj,
                                  bool tape_align, StixDetTapeStep *st,
                                  uint32_t idx, Error **errp)
{
    uint64_t u64, burst_us;
    double for_s;
    bool align, align_set;

    /* the spec is refused here, before the S2 parse could see it (M5) */
    if (qdict_get(obj, "spec")) {
        error_setg(errp, "det-tape-load: step %u: a single step takes no "
                   "spec", idx);
        return false;
    }
    if (!stix_det_tape_uint(obj, "fpga_det", STIX_DET_N_FPGA_DET - 1, &u64,
                            errp, idx)) {
        return false;
    }
    st->fpga_det = (uint32_t)u64;
    if (!stix_det_tape_uint(obj, "pix", STIX_DET_N_PIX_ACC - 1, &u64,
                            errp, idx)) {
        return false;
    }
    st->pix = (uint32_t)u64;
    if (!!qdict_get(obj, "adc") == !!qdict_get(obj, "ebin")) {
        error_setg(errp, "det-tape-load: step %u: a single step takes "
                   "exactly one of 'adc' and 'ebin'", idx);
        return false;
    }
    if (qdict_get(obj, "adc")) {
        if (!stix_det_tape_uint(obj, "adc", STIX_DET_N_ADC - 1, &u64,
                                errp, idx)) {
            return false;
        }
        st->adc = (int32_t)u64;
        st->ebin = -1;
    } else {
        if (!stix_det_tape_uint(obj, "ebin", STIX_DET_N_EBIN - 1, &u64,
                                errp, idx)) {
            return false;
        }
        st->ebin = (int32_t)u64;
        st->adc = -1;
    }
    if (!stix_det_tape_uint(obj, "n", 1000000u, &u64, errp, idx)) {
        return false;
    }
    if (!u64) {
        error_setg(errp, "det-tape-load: step %u: 'n' is not an integer "
                   "in 1..1000000", idx);
        return false;
    }
    st->n = (uint32_t)u64;
    if (qdict_get(obj, "wait_us")) {
        if (!stix_det_tape_uint(obj, "wait_us", 10000u, &u64, errp, idx)) {
            return false;
        }
        if (!u64) {
            error_setg(errp, "det-tape-load: step %u: 'wait_us' is not an "
                       "integer in 1..10000", idx);
            return false;
        }
        st->wait_us = (uint32_t)u64;
    } else {
        st->wait_us = 20;           /* the DSS default */
    }
    if (!stix_det_tape_num(obj, "for_s", &for_s, errp, idx)) {
        return false;
    }
    if (for_s < 0.1 || for_s > 86400.0) {
        error_setg(errp, "det-tape-load: step %u: for_s %g s is outside "
                   "0.1..86400", idx, for_s);
        return false;
    }
    st->for_s = for_s;
    burst_us = (uint64_t)st->n * st->wait_us;
    if (burst_us > (uint64_t)stix_det_tape_step_ticks(for_s) * 100000u) {
        error_setg(errp, "det-tape-load: step %u: for_s %g s (%u ticks) "
                   "does not cover n·wait_us = %g s", idx, for_s,
                   stix_det_tape_step_ticks(for_s),
                   (double)burst_us / 1e6);
        return false;
    }
    align_set = qdict_haskey(obj, "align");
    if (!stix_det_tape_bool(obj, "align", tape_align, &align, errp, idx)) {
        return false;
    }
    if (align_set && align && st->adc >= 0) {
        error_setg(errp, "det-tape-load: step %u: align applies to ebin "
                   "singles", idx);
        return false;
    }
    st->spec.align = align;         /* the effective align, as in S2 */
    /* no sky rate: the implicit masks are the one pair, cps 0 */
    st->fpga_det_mask = 1u << st->fpga_det;
    st->pix_mask = 1u << st->pix;
    st->cps = 0;
    return true;
}

/*
 * One step dict into @st. Every error names the step index @idx; masks
 * are fpga_det/pix indexed and a subcol key is refused by name. A
 * non-flat spec is S2's (kind rate only); a single step is S3's and
 * parses wholly in stix_det_single_parse, so S1 never half-implements
 * either.
 */
static bool stix_det_tape_parse_step(StixDetState *d, QDict *obj,
                                     bool tape_align, StixDetTapeStep *st,
                                     uint32_t idx, Error **errp)
{
    const char *kind;
    char *label;
    uint64_t u64;
    double for_s;
    const QDictEntry *e;
    static const char *const known[] = {
        "kind", "for_s", "label", "fpga_det_mask", "pix_mask", "cps",
        "spec", "file", "offset_ms", "align",
    };
    /* single targets one pair: no masks, cps or replay fields (refused
     * as unknown, not ignored); spec stays listed for its own refusal */
    static const char *const known_single[] = {
        "kind", "for_s", "label", "spec", "align",
        "fpga_det", "pix", "adc", "ebin", "n", "wait_us",
    };
    const char *const *keys;
    size_t n_keys;

    memset(st, 0, sizeof(*st));
    if (qdict_haskey(obj, "subcol_mask")) {
        error_setg(errp, "det-tape-load: step %u: \"subcol_mask\" is not "
                   "a tape field; use fpga_det_mask (dss_det = fpga_det)",
                   idx);
        return false;
    }
    kind = qdict_get_try_str(obj, "kind");
    if (!kind) {
        error_setg(errp, "det-tape-load: step %u: 'kind' is missing", idx);
        return false;
    }
    if (!strcmp(kind, "rate")) {
        st->kind = STIX_DET_TAPE_RATE;
    } else if (!strcmp(kind, "replay")) {
        st->kind = STIX_DET_TAPE_REPLAY;
    } else if (!strcmp(kind, "off")) {
        st->kind = STIX_DET_TAPE_OFF;
    } else if (!strcmp(kind, "single")) {
        st->kind = STIX_DET_TAPE_SINGLE;
        if (!stix_det_single_parse(d, obj, tape_align, st, idx, errp)) {
            return false;
        }
    } else {
        error_setg(errp, "det-tape-load: step %u: unknown kind '%s' "
                   "(rate, replay, off, single)", idx, kind);
        return false;
    }
    if (!stix_det_tape_num(obj, "for_s", &for_s, errp, idx)) {
        return false;
    }
    if (for_s < 0.1 || for_s > 86400.0) {
        error_setg(errp, "det-tape-load: step %u: for_s %g s is outside "
                   "0.1..86400", idx, for_s);
        return false;
    }
    st->for_s = for_s;
    /* the label is optional: an empty string is no label */
    label = g_strdup(qdict_get_try_str(obj, "label") ? : "");
    if (label[0] && !stix_det_tape_line_text(label, 64, errp,
                                             "a step label")) {
        g_free(label);
        return false;
    }
    if (st->kind == STIX_DET_TAPE_RATE || st->kind == STIX_DET_TAPE_OFF) {
        if (!stix_det_tape_uint(obj, "fpga_det_mask", 0xFFFFFFFFu, &u64,
                                errp, idx)) {
            g_free(label);
            return false;
        }
        st->fpga_det_mask = (uint32_t)u64;
    }
    if (st->kind == STIX_DET_TAPE_SINGLE) {
        /* the single parse stored the implicit one-pair masks */
    } else if (qdict_get(obj, "pix_mask")) {
        if (!stix_det_tape_uint(obj, "pix_mask", 0xFFFu, &u64, errp, idx)) {
            g_free(label);
            return false;
        }
        st->pix_mask = (uint32_t)u64;
    } else {
        st->pix_mask = 0xFFFu;
    }
    if (st->kind == STIX_DET_TAPE_RATE) {
        if (!stix_det_tape_uint(obj, "cps", STIX_DET_CPS_MAX, &u64, errp,
                                idx)) {
            g_free(label);
            return false;
        }
        st->cps = (uint32_t)u64;
    }
    if (st->kind == STIX_DET_TAPE_REPLAY) {
        const char *file = qdict_get_try_str(obj, "file");
        QNum *off = qobject_to(QNum, qdict_get(obj, "offset_ms"));

        if (!d->replay_loaded) {
            g_free(label);
            error_setg(errp, "det-tape-load: step %u: a replay step "
                       "needs a replay table scanned at realize (set "
                       "det-replay-file at startup)", idx);
            return false;
        }
        if (d->replay_trigger != STIX_DET_REPLAY_TRIGGER_QMP) {
            g_free(label);
            error_setg(errp, "det-tape-load: step %u: a replay step "
                       "needs det-replay-trigger=qmp (the nominal "
                       "trigger starts the replay at the SetEnable "
                       "edge regardless of the tape)", idx);
            return false;
        }
        if (file && strcmp(file, d->replay_file)) {
            g_free(label);
            error_setg(errp, "det-tape-load: step %u: 'file' must match "
                       "the realized det-replay-file", idx);
            return false;
        }
        if (off) {
            int64_t v;

            if (!qnum_get_try_int(off, &v) ||
                (uint64_t)v != d->replay_offset_ms) {
                g_free(label);
                error_setg(errp, "det-tape-load: step %u: 'offset_ms' "
                           "must match det-replay-offset-ms", idx);
                return false;
            }
        }
    }
    /* no unknown fields past the ones parsed (per kind: single takes
     * its fields on top of the common set, a rate step carrying 'n'
     * stays refused) */
    keys = st->kind == STIX_DET_TAPE_SINGLE ? known_single : known;
    n_keys = st->kind == STIX_DET_TAPE_SINGLE ?
             ARRAY_SIZE(known_single) : ARRAY_SIZE(known);
    for (e = qdict_first(obj); e; e = qdict_next(obj, e)) {
        const char *key = qdict_entry_key(e);
        size_t k;

        for (k = 0; k < n_keys; k++) {
            if (!strcmp(key, keys[k])) {
                break;
            }
        }
        if (k == n_keys) {
            g_free(label);
            error_setg(errp, "det-tape-load: step %u: unknown tape step "
                       "field '%s'", idx, key);
            return false;
        }
    }
    /* S2: the spec last, so no later refusal leaves a loaded spec; a
     * single step skipped it (its spec was refused and its align
     * decided in stix_det_single_parse, M5) */
    if (st->kind != STIX_DET_TAPE_SINGLE &&
        !stix_det_spec_parse(d, obj, tape_align, st, idx, errp)) {
        g_free(label);
        stix_det_spec_free(&st->spec);
        return false;
    }
    if (label[0]) {
        st->label = g_steal_pointer(&label);
    } else {
        g_free(label);
    }
    return true;
}

static void stix_det_tape_load_set(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    StixDetState *d = stix_det_of(obj, opaque);
    StixDetTapeStep steps[STIX_DET_TAPE_MAX_STEPS];
    g_autofree char *value = NULL;
    QObject *root;
    g_autoptr(QDict) top = NULL;
    QDict *bg = NULL;
    const char *tape_name;
    uint64_t u64;
    double bg_cps = 0;
    bool loop = false, align = false, adc_mode;
    size_t len;
    uint32_t i, n_steps = 0;

    if (!visit_type_str(v, name, &value, errp)) {
        return;
    }
    len = strlen(value);
    if (len > STIX_DET_TAPE_MAX_BYTES) {
        error_setg(errp, "det-tape-load: the tape is %zu bytes, past "
                   "the %u KiB cap", len, STIX_DET_TAPE_MAX_BYTES / 1024u);
        return;
    }
    root = qobject_from_json(value, errp);
    if (!root) {
        error_prepend(errp, "det-tape-load: invalid JSON: ");
        return;
    }
    top = qobject_to(QDict, root);
    if (!top) {
        qobject_unref(root);
        error_setg(errp, "det-tape-load: the tape is not a JSON object");
        return;
    }
    if (!stix_det_tape_uint(top, "api", 1, &u64, errp, UINT32_MAX)) {
        return;
    }
    if (u64 != 1) {
        error_setg(errp, "det-tape-load: \"api\" must be 1");
        return;
    }
    tape_name = qdict_get_try_str(top, "name");
    if (!tape_name ||
        !stix_det_tape_line_text(tape_name, 64, errp, "the tape name")) {
        return;
    }
    if (!stix_det_tape_bool(top, "loop", false, &loop, errp, UINT32_MAX) ||
        !stix_det_tape_bool(top, "align", false, &align, errp,
                            UINT32_MAX)) {
        return;
    }
    /* C1: a tape's adc_mode overrides the knob while it plays */
    if (!stix_det_tape_bool(top, "adc_mode", d->adc_mode, &adc_mode, errp,
                            UINT32_MAX)) {
        return;
    }
    if (qdict_get(top, "background")) {
        bg = qobject_to(QDict, qdict_get(top, "background"));
        if (!bg) {
            error_setg(errp, "det-tape-load: 'background' is not an "
                       "object");
            return;
        }
        if (qdict_haskey(bg, "fpga_det_mask") ||
            qdict_haskey(bg, "pix_mask") ||
            qdict_haskey(bg, "subcol_mask") || qdict_haskey(bg, "for_s")) {
            error_setg(errp, "det-tape-load: the background is unmasked "
                       "by definition: no fpga_det_mask/pix_mask/subcol_"
                       "mask/for_s on it");
            return;
        }
        if (!stix_det_tape_num(bg, "cps", &bg_cps, errp, UINT32_MAX)) {
            return;
        }
        if (bg_cps < 0 || bg_cps > STIX_DET_CPS_MAX ||
            bg_cps != floor(bg_cps)) {
            error_setg(errp, "det-tape-load: background cps %g is not "
                       "an integer in 0..%u", bg_cps, STIX_DET_CPS_MAX);
            return;
        }
    }
    if (qdict_haskey(top, "steps")) {
        QList *list = qobject_to(QList, qdict_get(top, "steps"));
        const QListEntry *e;

        if (!list) {
            error_setg(errp, "det-tape-load: 'steps' is not an array");
            return;
        }
        QLIST_FOREACH_ENTRY(list, e) {
            QDict *so = qobject_to(QDict, qlist_entry_obj(e));

            if (n_steps == STIX_DET_TAPE_MAX_STEPS) {
                error_setg(errp, "det-tape-load: more than %u steps",
                           STIX_DET_TAPE_MAX_STEPS);
            } else if (!so) {
                error_setg(errp, "det-tape-load: step %u is not an "
                           "object", n_steps);
            } else if (stix_det_tape_parse_step(d, so, align,
                                                &steps[n_steps], n_steps,
                                                errp)) {
                n_steps++;
                continue;
            }
            while (n_steps--) {     /* the parsed labels/specs leak not */
                g_free(steps[n_steps].label);
                stix_det_spec_free(&steps[n_steps].spec);
            }
            return;
        }
    }

    /* commit: a load while playing stops first */
    stix_det_tape_stop(d, "det-tape-load");
    stix_det_tape_clear(d);
    for (i = 0; i < n_steps; i++) {
        d->tape_steps[i] = steps[i];
    }
    d->tape_n_steps = n_steps;
    d->tape_name = g_strdup(tape_name);
    d->tape_loop = loop;
    d->tape_align = align;
    d->tape_adc_mode = adc_mode;
    d->tape_bg_cps = (uint32_t)bg_cps;
    d->tape_state = STIX_DET_TAPE_READY;
    /* the status previews the first step */
    d->tape_step = 0;
    d->tape_left_ticks = n_steps ?
                         stix_det_tape_step_ticks(d->tape_steps[0].for_s) : 0;
    trace_stixdet_tape_load(tape_name, n_steps, loop, align,
                            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static void stix_det_tape_play_set(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    StixDetState *d = stix_det_of(obj, opaque);
    bool value;

    if (!visit_type_bool(v, name, &value, errp)) {
        return;
    }
    if (value) {
        if (d->tape_state == STIX_DET_TAPE_PLAYING) {
            error_setg(errp, "%s: the tape is already playing", name);
            return;
        }
        if (d->tape_state == STIX_DET_TAPE_IDLE) {
            error_setg(errp, "%s: no tape loaded (det-tape-load first)",
                       name);
            return;
        }
        if (!d->tape_n_steps) {
            error_setg(errp, "%s: the loaded tape has no steps", name);
            return;
        }
        if (d->source != STIX_DET_SOURCE_SYNTHETIC) {
            error_setg(errp, "%s: rate steps need det-source=synthetic "
                       "(the source is %s)", name,
                       stix_det_source_name[d->source]);
            return;
        }
        d->tape_play_req = true;
    } else {
        /* accepted also when done/stopped/ready: a no-op stop keeps
         * drivers simple (det.tape.stop after done reverts nothing) */
        d->tape_stop_req = true;
    }
}

static void stix_det_tape_status_get(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    StixDetState *d = stix_det_of(obj, opaque);
    const StixDetTapeStep *st = d->tape_n_steps ?
        &d->tape_steps[d->tape_step] : NULL;
    char *value;

    value = g_strdup_printf("state=%s,name=%s,step=%u/%u,label=%s,"
                            "step_left_ticks=%u,loop=%s,align=%s,"
                            "dropped=%u",
                            stix_det_tape_state_name[d->tape_state],
                            d->tape_name ? d->tape_name : "",
                            d->tape_n_steps ? d->tape_step + 1 : 0,
                            d->tape_n_steps,
                            st && st->label ? st->label : "",
                            d->tape_left_ticks,
                            d->tape_loop ? "true" : "false",
                            d->tape_align ? "true" : "false",
                            d->single_dropped);
    visit_type_str(v, name, &value, errp);
    g_free(value);
}

/* ---- A1: det-align, the classification report ---- */

static void stix_det_align_get(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    bool value = stix_det_of(obj, opaque)->align;

    visit_type_bool(v, name, &value, errp);
}

static void stix_det_align_set(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    StixDetState *d = stix_det_of(obj, opaque);
    bool value;

    if (!visit_type_bool(v, name, &value, errp)) {
        return;
    }
    d->align = value;
}

static void stix_det_align_status_get(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    StixDetState *d = stix_det_of(obj, opaque);
    char *value;

    value = g_strdup_printf("mode=%s,rows=%u,fallback=%u,honoured=%u,"
                            "mismatch_cells=%u,unassigned_cells=%u,"
                            "calibration=%s,preimage_miss=%u",
                            d->align ? "on" : "off", d->align_rows,
                            d->align_fallback, d->align_honoured,
                            d->align_mismatch_cells,
                            d->align_unassigned_cells,
                            d->align_has_table ? "dss-linear" : "none",
                            d->spec_preimage_miss);
    visit_type_str(v, name, &value, errp);
    g_free(value);
}

/* ---- C1: det-adc-mode and det-cal-kev-full ---- */

static void stix_det_adc_mode_get(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    bool value = stix_det_of(obj, opaque)->adc_mode;

    visit_type_bool(v, name, &value, errp);
}

static void stix_det_adc_mode_set(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    StixDetState *d = stix_det_of(obj, opaque);
    bool value;

    if (!visit_type_bool(v, name, &value, errp)) {
        return;
    }
    if (value && !d->adc_mode) {
        /* the mode turning on arms the map rebuild */
        d->adc_map_armed = true;
    }
    if (!value) {
        /* off folds nothing: the map is stale until the next rebuild */
        d->adc_map_valid = false;
    }
    d->adc_mode = value;
}

static void stix_det_adc_cal_get(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    uint32_t value = stix_det_of(obj, opaque)->adc_kev_full;

    visit_type_uint32(v, name, &value, errp);
}

static void stix_det_adc_cal_set(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    StixDetState *d = stix_det_of(obj, opaque);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value < 1 || value > STIX_DET_N_ADC) {
        error_setg(errp, "%s: %u keV is outside 1..%u (the full scale "
                   "maps to adc 4096)", name, value, STIX_DET_N_ADC);
        return;
    }
    if (value != d->adc_kev_full) {
        /* a calibration change re-arms the map rebuild */
        d->adc_map_armed = true;
    }
    d->adc_kev_full = value;
}

static void stix_det_adc_status_get(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp)
{
    StixDetState *d = stix_det_of(obj, opaque);
    bool mode = d->tape_state == STIX_DET_TAPE_PLAYING ?
                d->tape_adc_mode : d->adc_mode;
    char *value;

    value = g_strdup_printf("mode=%s,kev_full=%u,base_source=%s,"
                            "map_rows=%u",
                            mode ? "on" : "off", d->adc_kev_full,
                            d->replay_loaded ? "replay" : "zero",
                            d->adc_map_valid ?
                            STIX_DET_N_FPGA_DET * STIX_DET_N_PIX_ACC : 0);
    visit_type_str(v, name, &value, errp);
    g_free(value);
}

/* ---- the scene properties (D-Db), on /machine/stixctrl ---- */

static StixDetState *stix_det_of(Object *obj, void *opaque)
{
    return (StixDetState *)((uint8_t *)obj + GPOINTER_TO_SIZE(opaque));
}

static void stix_det_trace_scene(StixDetState *d)
{
    trace_stixdet_scene(stix_det_source_name[d->source], d->flare,
                        d->quiet_cps, d->flare_cps,
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static void stix_det_source_get(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    char *value = g_strdup(stix_det_source_name[stix_det_of(obj,
                                                            opaque)->source]);

    visit_type_str(v, name, &value, errp);
    g_free(value);
}

static void stix_det_source_set(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    StixDetState *d = stix_det_of(obj, opaque);
    g_autofree char *value = NULL;
    uint32_t i;

    if (!visit_type_str(v, name, &value, errp)) {
        return;
    }
    for (i = 0; i < STIX_DET_SOURCE__MAX; i++) {
        if (!strcmp(value, stix_det_source_name[i])) {
            if (i == STIX_DET_SOURCE_REPLAY_DSS && !d->replay_loaded) {
                error_setg(errp, "%s: replay-dss needs a replay table "
                           "(set det-replay-file at startup)", name);
                return;
            }
            d->source = i;
            if (i == STIX_DET_SOURCE_REPLAY_DSS) {
                /* the rows were not classified for this source yet */
                d->replay_class_pending = true;
            }
            stix_det_trace_scene(d);
            return;
        }
    }
    error_setg(errp, "%s: unknown source '%s' (zero, synthetic or "
               "replay-dss)", name, value);
}

static void stix_det_flare_get(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    bool value = stix_det_of(obj, opaque)->flare;

    visit_type_bool(v, name, &value, errp);
}

static void stix_det_flare_set(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    StixDetState *d = stix_det_of(obj, opaque);
    bool value;

    if (!visit_type_bool(v, name, &value, errp)) {
        return;
    }
    d->flare = value;
    stix_det_trace_scene(d);
}

/*
 * det-quiet-cps and det-flare-cps share the accessors; each property's
 * opaque says where its StixDetState and its field are (allocated once
 * per class, which lives as long as QEMU does)
 */
typedef struct StixDetCpsProp {
    size_t det_offset;          /* of the StixDetState in the object */
    size_t field_offset;        /* of the uint32_t in StixDetState */
} StixDetCpsProp;

static uint32_t *stix_det_cps_field(StixDetState *d, const StixDetCpsProp *p)
{
    return (uint32_t *)((uint8_t *)d + p->field_offset);
}

static void stix_det_cps_get(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    const StixDetCpsProp *p = opaque;
    StixDetState *d = stix_det_of(obj, GSIZE_TO_POINTER(p->det_offset));
    uint32_t value = *stix_det_cps_field(d, p);

    visit_type_uint32(v, name, &value, errp);
}

static void stix_det_cps_set(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    const StixDetCpsProp *p = opaque;
    StixDetState *d = stix_det_of(obj, GSIZE_TO_POINTER(p->det_offset));
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    /* reject rather than clamp, so a typo in a script is an error */
    if (value > STIX_DET_CPS_MAX) {
        error_setg(errp, "%s: %u counts/s per fpga_det is above %u", name,
                   value, STIX_DET_CPS_MAX);
        return;
    }
    *stix_det_cps_field(d, p) = value;
    stix_det_trace_scene(d);
}

static void *stix_det_cps_prop(size_t det_offset, size_t field_offset)
{
    StixDetCpsProp *p = g_new(StixDetCpsProp, 1);

    p->det_offset = det_offset;
    p->field_offset = field_offset;
    return p;
}

/*
 * det-inject (D-Db, D-6; write-only): "fpga_det=F,pix=P,ebin=B,count=N",
 * a one-shot deposit of N counts at (pix P, fpga_det F, ebin B). The
 * keys are mandatory, so the index spaces are explicit; an unknown or
 * duplicate key, a missing key, a value that is not a number, count 0
 * and an index outside its space (fpga_det 0..31, pix 0..11 — the guard
 * ring is never accumulated —, ebin 0..31) are rejected and store
 * nothing. The setter only stores the request and traces (D-Dj); the
 * next 100 ms tick applies it through stix_det_add_counts, with the
 * sky's per-tick gating: where the (pix, fpga_det) pair is gated off at
 * that tick, the deposit is dropped, like a real event.
 */
static bool stix_det_inject_num(const char *key, char *val, uint32_t hi,
                                uint32_t *out, Error **errp)
{
    unsigned long long v;
    char *end;

    /* strtoull would take "+250", " 5" and "-1": the first character
     * must be a decimal digit */
    if (*val < '0' || *val > '9') {
        error_setg(errp, "det-inject: %s='%s' is not a number in 0..%u",
                   key, val, hi);
        return false;
    }
    errno = 0;
    v = strtoull(val, &end, 10);
    if (errno != 0 || end == val || *end != '\0' || v > hi) {
        error_setg(errp, "det-inject: %s='%s' is not a number in 0..%u",
                   key, val, hi);
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static void stix_det_inject_set(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    StixDetState *d = stix_det_of(obj, opaque);
    uint32_t fpga_det = 0, pix = 0, ebin = 0, n = 0;
    bool got_fpga_det = false, got_pix = false, got_ebin = false,
         got_n = false;
    g_autofree char *value = NULL;
    char *kv, *next;

    if (!visit_type_str(v, name, &value, errp)) {
        return;
    }
    for (kv = value; kv && *kv; kv = next) {
        char *val = strchr(kv, '=');
        const char *key;
        uint32_t *dst, hi;
        bool *got;

        next = strchr(kv, ',');
        if (next) {
            *next++ = '\0';
        }
        if (!val) {
            error_setg(errp, "det-inject: expected key=value, got '%s'",
                       kv);
            return;
        }
        *val++ = '\0';
        key = kv;
        if (!strcmp(key, "fpga_det")) {
            dst = &fpga_det;
            hi = STIX_DET_N_FPGA_DET - 1;
            got = &got_fpga_det;
        } else if (!strcmp(key, "pix")) {
            dst = &pix;
            hi = STIX_DET_N_PIX_ACC - 1;
            got = &got_pix;
        } else if (!strcmp(key, "ebin")) {
            dst = &ebin;
            hi = STIX_DET_N_EBIN - 1;
            got = &got_ebin;
        } else if (!strcmp(key, "count")) {
            dst = &n;
            hi = UINT32_MAX;
            got = &got_n;
        } else {
            error_setg(errp, "det-inject: unknown key '%s' (the format "
                       "is fpga_det=F,pix=P,ebin=B,count=N)", key);
            return;
        }
        if (*got) {
            error_setg(errp, "det-inject: duplicate key '%s'", key);
            return;
        }
        if (!stix_det_inject_num(key, val, hi, dst, errp)) {
            return;
        }
        *got = true;
    }
    if (!got_fpga_det || !got_pix || !got_ebin || !got_n) {
        error_setg(errp, "det-inject: missing %s%s%s%s (the format is "
                   "fpga_det=F,pix=P,ebin=B,count=N)",
                   got_fpga_det ? "" : "fpga_det ",
                   got_pix ? "" : "pix ",
                   got_ebin ? "" : "ebin ",
                   got_n ? "" : "count");
        return;
    }
    if (n == 0) {
        error_setg(errp, "det-inject: count=0 adds no counts");
        return;
    }
    d->inject_pending = true;
    d->inject_fpga_det = fpga_det;
    d->inject_pix = pix;
    d->inject_ebin = ebin;
    d->inject_n = n;
    trace_stixdet_inject(fpga_det, pix, ebin, n);
}

/*
 * det-replay-trigger (D-Db, D-7): nominal (default; the first fsmCmd
 * SetEnable 0->1 edge after a reset, i.e. NOMINAL entry) or qmp (only
 * a det-replay-start=true). A scene knob: it survives a device reset.
 */
static void stix_det_replay_trigger_get(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    StixDetState *d = stix_det_of(obj, opaque);
    char *value = g_strdup(stix_det_replay_trigger_name[d->replay_trigger]);

    visit_type_str(v, name, &value, errp);
    g_free(value);
}

static void stix_det_replay_trigger_set(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    StixDetState *d = stix_det_of(obj, opaque);
    g_autofree char *value = NULL;
    uint32_t i;

    if (!visit_type_str(v, name, &value, errp)) {
        return;
    }
    for (i = 0; i < STIX_DET_REPLAY_TRIGGER__MAX; i++) {
        if (!strcmp(value, stix_det_replay_trigger_name[i])) {
            d->replay_trigger = i;
            stix_det_trace_scene(d);
            return;
        }
    }
    error_setg(errp, "%s: unknown trigger '%s' (nominal or qmp)", name,
               value);
}

/*
 * det-replay-offset-ms (D-PLm req 3; replaces D-Db's det-replay-offset
 * before D-7 landed, so the two never coexist): the file time at
 * start, in milliseconds. The start converts to the model's 100 ms
 * ticks with integer maths (offset_ms / 100): sub-tick offsets round
 * down to the tick, and an event exactly at the offset arrives in the
 * first tick. A value past UINT32_MAX ticks is rejected.
 */
static void stix_det_replay_offset_ms_get(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    uint64_t value = stix_det_of(obj, opaque)->replay_offset_ms;

    visit_type_uint64(v, name, &value, errp);
}

static void stix_det_replay_offset_ms_set(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    StixDetState *d = stix_det_of(obj, opaque);
    uint64_t value;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    /* the feeder's 100 ms ticks are u32: reject what they cannot hold */
    if (value / 100 > UINT32_MAX) {
        error_setg(errp, "%s: %" PRIu64 " ms is past 2^32 ticks of "
                   "replay time", name, value);
        return;
    }
    d->replay_offset_ms = value;
    stix_det_trace_scene(d);
}

/*
 * The replay's state word (D-PLm req 4), shared by det-replay-status
 * and the hw-state accessor below so the two faces cannot drift: idle
 * (no table loaded), armed (loaded, waiting for its trigger — the
 * nominal SetEnable edge or a det-replay-start=true), running, ended
 * (EOF reached) or stopped (a det-replay-start=false stop; a device
 * reset re-arms instead, so it reads armed again).
 */
static const char *stix_det_replay_state_name(const StixDetState *d)
{
    if (!d->replay_loaded) {
        return "idle";
    } else if (d->replay_running) {
        return "running";
    } else if (d->replay_stopped) {
        return "stopped";
    } else if (d->replay_ended) {
        return "ended";
    }
    return "armed";
}

/* the table's last event time in ns; 0 for an empty or absent table */
static uint64_t stix_det_replay_duration_ns(const StixDetState *d)
{
    if (!d->replay_n_ev) {
        return 0;
    }
    return (uint64_t)d->replay_ev[d->replay_n_ev - 1].t_tick *
           STIX_DET_TICK_NS;
}

/*
 * det-replay-status (D-PLm req 4): one atomic read-only line with the
 * replay's state and progress, for the platform controller.
 */
static void stix_det_replay_status_get(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    StixDetState *d = stix_det_of(obj, opaque);
    char *value;

    value = g_strdup_printf("state=%s,pos_ns=%" PRIu64 ",events=%" PRIu64
                            ",dropped=%" PRIu64 ",unsupported=%" PRIu64
                            ",table=%s,count=%u,duration_ns=%" PRIu64,
                            stix_det_replay_state_name(d),
                            (uint64_t)((uint64_t)d->replay_time_tick *
                                       STIX_DET_TICK_NS),
                            d->replay_events_done, d->replay_dropped,
                            d->replay_unsupported,
                            d->replay_file ? d->replay_file : "",
                            d->replay_n_ev,
                            stix_det_replay_duration_ns(d));
    visit_type_str(v, name, &value, errp);
    g_free(value);
}

/*
 * det-replay-start (D-Db, D-7): writing true starts a loaded replay
 * now (applied at the next 100 ms tick, D-Dj), false stops it; reading
 * gives the running state. Needs det-source=replay-dss.
 */
static void stix_det_replay_start_get(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    bool value = stix_det_of(obj, opaque)->replay_running;

    visit_type_bool(v, name, &value, errp);
}

static void stix_det_replay_start_set(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    StixDetState *d = stix_det_of(obj, opaque);
    bool value;

    if (!visit_type_bool(v, name, &value, errp)) {
        return;
    }
    if (value) {
        if (!d->replay_loaded) {
            error_setg(errp, "%s: no replay table (set det-replay-file "
                       "at startup)", name);
            return;
        }
        if (d->source != STIX_DET_SOURCE_REPLAY_DSS) {
            error_setg(errp, "%s: needs det-source=replay-dss", name);
            return;
        }
        d->replay_start_req = true;
    } else {
        /*
         * a stop applies at once; a start is tick-deferred (D-Dj), so
         * false also retracts a start set earlier in the same tick
         */
        d->replay_start_req = false;
        if (d->replay_running) {
            d->replay_running = false;
            d->replay_stopped = true;   /* det-replay-status: stopped */
            trace_stixdet_replay_state("stop", "qmp", d->replay_time_tick,
                                       d->replay_cursor,
                                       d->replay_events_done);
        }
    }
}

/*
 * det-rotb-burst (FF plan; write-only, like det-inject): append this
 * many frames now; det-rotb-burst-status carries the outcome.
 */
static void stix_det_burst_set(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    StixDetState *d = stix_det_of(obj, opaque);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    stix_det_rotb_burst(d, value, errp);
}

/*
 * det-rotb-burst-status (read-only): one atomic line with the last
 * burst's request, what went in and why, like det-replay-status.
 */
static void stix_det_burst_status_get(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    StixDetState *d = stix_det_of(obj, opaque);
    char *value = g_strdup_printf("requested=%u,appended=%u,stop=%s,"
                                  "rotb_count=%u", d->burst_requested,
                                  d->burst_appended, d->burst_stop,
                                  d->burst_rotb_count);

    visit_type_str(v, name, &value, errp);
    g_free(value);
}

void stix_det_instance_init(StixDetState *d)
{
    d->source = STIX_DET_SOURCE_SYNTHETIC;
    d->flare = false;
    d->quiet_cps = STIX_DET_QUIET_CPS_DEFAULT;
    d->flare_cps = STIX_DET_FLARE_CPS_DEFAULT;
    d->replay_trigger = STIX_DET_REPLAY_TRIGGER_NOMINAL;
    d->replay_offset_ms = 0;
    d->burst_stop = "none";
    d->tape_state = STIX_DET_TAPE_IDLE;
    d->align = false;
    d->align_rows = STIX_DET_N_FPGA_DET * STIX_DET_N_PIX_ACC;
    d->adc_mode = false;
    d->adc_kev_full = 160;      /* the D5 default full scale */
}

void stix_det_class_init(ObjectClass *klass, size_t det_offset)
{
    void *opaque = GSIZE_TO_POINTER(det_offset);

    object_class_property_add(klass, "det-source", "str", &str_type_info,
                              stix_det_source_get, stix_det_source_set,
                              NULL, opaque);
    object_class_property_set_description(klass, "det-source",
        "detector count source: zero | synthetic | replay-dss (the "
        "pre-scanned .dssevs table; set after QEMU is up, a table is "
        "scanned at realize) (runtime; survives a device reset)");
    object_class_property_add(klass, "det-flare", "bool", &bool_type_info,
                              stix_det_flare_get, stix_det_flare_set,
                              NULL, opaque);
    object_class_property_set_description(klass, "det-flare",
        "the flare switch: the synthetic sky uses det-flare-cps and the "
        "flare spectrum (runtime)");
    object_class_property_add(klass, "det-quiet-cps", "uint32",
                              &uint32_type_info, stix_det_cps_get,
                              stix_det_cps_set, NULL,
                              stix_det_cps_prop(det_offset,
                                  offsetof(StixDetState, quiet_cps)));
    object_class_property_set_description(klass, "det-quiet-cps",
        "quiet sky, counts/s per fpga_det, 0..1000000 (runtime)");
    object_class_property_add(klass, "det-flare-cps", "uint32",
                              &uint32_type_info, stix_det_cps_get,
                              stix_det_cps_set, NULL,
                              stix_det_cps_prop(det_offset,
                                  offsetof(StixDetState, flare_cps)));
    object_class_property_set_description(klass, "det-flare-cps",
        "flare sky, counts/s per fpga_det, 0..1000000 (runtime)");
    object_class_property_add(klass, "det-inject", "str", &str_type_info,
                              NULL, stix_det_inject_set, NULL, opaque);
    object_class_property_set_description(klass, "det-inject",
        "one-shot count deposit, write-only: fpga_det=F,pix=P,ebin=B,"
        "count=N; lands in the LACC of the finished QL bank at the next "
        "swap and in EACC (the next chunk) at the next nominal swap, "
        "where the pair is gated on at the next tick (runtime)");
    object_class_property_add(klass, "det-replay-trigger", "str",
                              &str_type_info, stix_det_replay_trigger_get,
                              stix_det_replay_trigger_set, NULL, opaque);
    object_class_property_set_description(klass, "det-replay-trigger",
        "when a loaded .dssevs replay starts: nominal (the first fsmCmd "
        "SetEnable edge after a reset, i.e. NOMINAL entry) | qmp (only "
        "det-replay-start=true) (runtime; survives a device reset)");
    object_class_property_add(klass, "det-replay-offset-ms", "uint64",
                              &uint64_type_info,
                              stix_det_replay_offset_ms_get,
                              stix_det_replay_offset_ms_set, NULL, opaque);
    object_class_property_set_description(klass, "det-replay-offset-ms",
        "replay start offset into the .dssevs file, milliseconds "
        "(renamed per D-PLm req 3 from det-replay-offset's seconds; the "
        "two never coexist); sub-tick offsets round down to the 100 ms "
        "tick, an event exactly at the offset arrives in the first "
        "tick; past 2^32 ticks is rejected (runtime; survives a device "
        "reset)");
    object_class_property_add(klass, "det-replay-start", "bool",
                              &bool_type_info, stix_det_replay_start_get,
                              stix_det_replay_start_set, NULL, opaque);
    object_class_property_set_description(klass, "det-replay-start",
        "start (true) or stop (false) a loaded .dssevs replay now, with "
        "det-source=replay-dss; reads the running state (runtime)");
    object_class_property_add(klass, "det-replay-status", "str",
                              &str_type_info, stix_det_replay_status_get,
                              NULL, NULL, opaque);
    object_class_property_set_description(klass, "det-replay-status",
        "read-only replay progress, one atomic line (D-PLm req 4): "
        "state=idle|armed|running|ended|stopped,pos_ns=...,events=...,"
        "dropped=...,unsupported=...,table=...,count=...,duration_ns="
        ".... armed waits for the trigger (the nominal SetEnable edge "
        "or det-replay-start=true), ended is the EOF, stopped is a "
        "det-replay-start=false stop (a device reset re-arms, so it "
        "reads armed); dropped counts only the D-6 gate's drops, "
        "unsupported the 0xD setup words the model does not implement "
        "(runtime)");
    object_class_property_add(klass, "det-rotb-burst", "uint32",
                              &uint32_type_info, NULL, stix_det_burst_set,
                              NULL, opaque);
    object_class_property_set_description(klass, "det-rotb-burst",
        "append this many copies of the last live chunk to the ROTB "
        "now, write-only (one 49-block frame per stixctrl_rotb_append): "
        "frame k of n carries the SCET last + (now - last) * k / n of "
        "the last live chunk's SCET, the UNSYNCED flag from the current "
        "register; with the clock paused (now <= last) every frame "
        "carries `last`. 0, more than the ring's 2131 chunks, "
        "det-rotb=off and no live chunk since the last device reset "
        "are refused; an append that does not fit stops the burst "
        "(stop=ring-full; with rotb-overflow=overwrite-oldest it never "
        "does, the oldest blocks are overwritten instead). Burst frames "
        "get near-zero archived durations (ArchiveManager.c:168) "
        "(runtime)");
    object_class_property_add(klass, "det-rotb-burst-status", "str",
                              &str_type_info, stix_det_burst_status_get,
                              NULL, NULL, opaque);
    object_class_property_set_description(klass, "det-rotb-burst-status",
        "read-only line of the last det-rotb-burst: requested=N,"
        "appended=M,stop=done|ring-full|rotb-disabled|rotb-testmode,"
        "rotb_count=C; requested=0,stop=none before any (runtime)");
    /* S1: the mix tape */
    object_class_property_add(klass, "det-tape-load", "str", &str_type_info,
                              NULL, stix_det_tape_load_set, NULL, opaque);
    object_class_property_set_description(klass, "det-tape-load",
        "load a mix tape, write-only JSON: {\"api\": 1, \"name\": <str "
        "1..64>, \"align\": <bool, default false>, \"adc_mode\": <bool, "
        "optional, default = det-adc-mode>, \"loop\": <bool, default "
        "false>, \"background\": {\"cps\": <0..1000000, unmasked>}, "
        "\"steps\": [<step>, ...]} with at most 64 steps and 64 KiB. A "
        "step: {\"kind\": \"rate\"|\"replay\"|\"off\", \"for_s\": <s "
        "0.1..86400>, \"label\": <str, optional>, \"fpga_det_mask\": "
        "<u32, required for rate/off>, \"pix_mask\": <0..0xFFF, default "
        "0xFFF>, \"cps\": <0..1000000, rate>}; replay steps add "
        "\"file\"/\"offset_ms\" matching det-replay-file/"
        "det-replay-offset-ms and need det-replay-trigger=qmp. Masks "
        "are fpga_det/pix indexed; a rejected load keeps the previous "
        "tape (runtime)");
    object_class_property_add(klass, "det-tape-play", "bool",
                              &bool_type_info, NULL,
                              stix_det_tape_play_set, NULL, opaque);
    object_class_property_set_description(klass, "det-tape-play",
        "true starts the loaded tape at step 0 on the next 100 ms tick "
        "(needs det-source=synthetic; the step clock runs only while "
        "FSM Enable = 1, so SAFE freezes playback); false stops it and "
        "the scene knobs resume at the next tick, as on done (write-"
        "only; refused while already playing or with no tape loaded) "
        "(runtime)");
    object_class_property_add(klass, "det-tape-status", "str",
                              &str_type_info, stix_det_tape_status_get,
                              NULL, NULL, opaque);
    object_class_property_set_description(klass, "det-tape-status",
        "read-only line of the tape: state=idle|ready|playing|done|"
        "stopped,name=...,step=K/N,label=...,step_left_ticks=...,loop="
        "...,align=...; a device reset stops playback and keeps the "
        "tape (state ready) (runtime)");
    /* A1: ELUT alignment */
    object_class_property_add(klass, "det-align", "bool", &bool_type_info,
                              stix_det_align_get, stix_det_align_set,
                              NULL, opaque);
    object_class_property_set_description(klass, "det-align",
        "ELUT alignment reporting: the ELUT row classification runs at "
        "every SetEnable edge and QL swap (not only for replay-dss) and "
        "det-align-status carries its summary (runtime; survives a "
        "device reset)");
    object_class_property_add(klass, "det-align-status", "str",
                              &str_type_info, stix_det_align_status_get,
                              NULL, NULL, opaque);
    object_class_property_set_description(klass, "det-align-status",
        "read-only line of the last classification: mode=off|on,rows=384"
        ",fallback=F,honoured=H,mismatch_cells=M,unassigned_cells=U,"
        "calibration=none|dss-linear. mismatch_cells compares the "
        "honoured rows against the sim calibration's linear bin (the D-7 "
        "law from the replay table's baselines); calibration=none means "
        "the comparison was not computed (no replay table), not that "
        "there are no mismatches (runtime)");
    /* C1: the ADC-faithful sky */
    object_class_property_add(klass, "det-adc-mode", "bool",
                              &bool_type_info, stix_det_adc_mode_get,
                              stix_det_adc_mode_set, NULL, opaque);
    object_class_property_set_description(klass, "det-adc-mode",
        "ADC-faithful sky: the synthetic sky and the ROTB chunk fill "
        "draw through the loaded FPGA ELUT via a cached per-row "
        "bin-mass map (default off; effective while det-source="
        "synthetic; a tape's adc_mode overrides it while the tape "
        "plays) (runtime; survives a device reset)");
    object_class_property_add(klass, "det-cal-kev-full", "uint32",
                              &uint32_type_info, stix_det_adc_cal_get,
                              stix_det_adc_cal_set, NULL, opaque);
    object_class_property_set_description(klass, "det-cal-kev-full",
        "the keV full scale K of the sim calibration (adc = base + "
        "gain*keV, gain = (4096 - base)/K; a documented sim anchor, not "
        "instrument truth); 1..4096, default 160; a change re-arms the "
        "bin-mass map rebuild (runtime; survives a device reset)");
    object_class_property_add(klass, "det-adc-status", "str",
                              &str_type_info, stix_det_adc_status_get,
                              NULL, NULL, opaque);
    object_class_property_set_description(klass, "det-adc-status",
        "read-only line of the ADC sky: mode=off|on,kev_full=...,"
        "base_source=replay|zero,map_rows=0|384 (runtime)");
}

bool stix_det_owns(StixDetState *d, hwaddr addr)
{
    return d->model && addr <= STIX_DET_REG_LAST;
}

uint32_t stix_det_read(StixDetState *d, hwaddr addr)
{
    switch (addr) {
    case 0x18: /* fsmSta */
        return stix_det_fsm_sta(d);
    case 0x30: /* fsmEvents: SUMCNT so far */
        return d->sumcnt;
    case 0x3C: /* fsmResult1, never read by the FSW */
    case 0x40: /* fsmResult2 */
    case 0x48: /* fsmCalCount: calibration never completes */
    case 0x4C: /* fsmCalRes */
        return 0;
    default:
        return *stix_det_regp(d, addr);
    }
}

/* false when the write was dropped (a read-only register) */
bool stix_det_write(StixDetState *d, hwaddr addr, uint32_t value)
{
    switch (addr) {
    case 0x10: /* testPulseCmdSta */
        d->reg->testPulseCmdSta = (value & ~(STIX_DET_TP_BUSY |
                                             STIX_DET_TP_SEU)) |
                                  STIX_DET_TP_BUSY;
        break;
    case 0x14: /* fsmCmd */
        stix_det_fsm_cmd(d, value);
        break;
    case 0x00: /* ideFxCmdSta */
        if (d->idefx) {
            stix_det_idefx_cmd(d, value);
        } else {
            d->reg->ideFxCmdSta = value;
        }
        break;
    case 0x04: /* idefxModeData: ReadMode 31:16 R/W, response read-only */
        d->reg->idefxModeData = d->idefx ?
            (value & IDEFX_HI16) | (d->reg->idefxModeData & ~IDEFX_HI16) :
            value;
        break;
    case 0x08: /* idefxWriteMask: the mask 31:16 R/W, reserved read 0 */
        d->reg->idefxWriteMask = d->idefx ? value & IDEFX_HI16 : value;
        break;
    case 0x0C: /* idefxConfig */
        if (d->idefx) {
            stix_det_idefx_config(d, value);
        } else {
            d->reg->idefxConfig = value;
        }
        break;
    case 0x18: /* read-only: fsmSta, fsmEvents, results */
    case 0x30:
    case 0x3C:
    case 0x40:
    case 0x48:
    case 0x4C:
        trace_stixdet_ro_write_dropped((uint32_t)addr, value);
        return false;
    default:
        *stix_det_regp(d, addr) = value;
        break;
    }
    return true;
}

/*
 * The hw-state "det" object (D-PLm req 8, platform plan PL-11c),
 * filled by the controller's stixctrl_get_hw_state next to its own
 * quarter_on: the scene (det-source/-flare/-quiet-cps/-flare-cps), the
 * QL bank in use (fsmSta.ActiveQl), the FSM enable, and the replay
 * progress det-replay-status carries — the same source fields, never
 * the parsed string. Pure like the getters: nothing is advanced, no
 * register, timer or IRQ is touched and nothing traces (D-Ai), so two
 * reads with no virtual time in between are equal. The caller owns
 * @det (it already holds "quarter_on").
 */
void stix_det_hw_state(StixDetState *d, QDict *det)
{
    QDict *fsm = qdict_new(), *rep = qdict_new();
    QDict *tape = qdict_new(), *align = qdict_new(), *adc = qdict_new();
    const StixDetTapeStep *st = d->tape_n_steps ?
                                &d->tape_steps[d->tape_step] : NULL;

    qdict_put_str(det, "source", stix_det_source_name[d->source]);
    qdict_put_bool(det, "flare", d->flare);
    qdict_put_int(det, "quiet_cps", d->quiet_cps);
    qdict_put_int(det, "flare_cps", d->flare_cps);
    qdict_put_int(det, "ql_bank", d->active_ql_bank);
    qdict_put_bool(fsm, "enable", d->enable);
    qdict_put(det, "fsm", fsm);

    qdict_put_str(rep, "state", stix_det_replay_state_name(d));
    qdict_put_int(rep, "pos_ns",
                  (uint64_t)d->replay_time_tick * STIX_DET_TICK_NS);
    qdict_put_int(rep, "events", d->replay_events_done);
    qdict_put_int(rep, "dropped", d->replay_dropped);
    qdict_put_int(rep, "unsupported", d->replay_unsupported);
    qdict_put_str(rep, "table", d->replay_file ? d->replay_file : "");
    qdict_put_int(rep, "count", d->replay_n_ev);
    qdict_put_int(rep, "duration_ns", stix_det_replay_duration_ns(d));
    qdict_put(det, "replay", rep);

    /* S1: the tape; step is 1-based (the board's "step K of N") */
    qdict_put_str(tape, "state", stix_det_tape_state_name[d->tape_state]);
    qdict_put_str(tape, "name", d->tape_name ? d->tape_name : "");
    qdict_put_int(tape, "step", d->tape_n_steps ? d->tape_step + 1 : 0);
    qdict_put_int(tape, "n_steps", d->tape_n_steps);
    qdict_put_str(tape, "label", st && st->label ? st->label : "");
    qdict_put_str(tape, "kind",
                  st ? stix_det_tape_kind_name[st->kind] : "");
    qdict_put_bool(tape, "loop", d->tape_loop);
    qdict_put_bool(tape, "align", d->tape_align);
    /* S2: the running (or previewed) step's spectrum */
    qdict_put_str(tape, "spec", st ? stix_det_spec_desc(&st->spec) : "");
    /* S3: the single steps' pix-gate drops of the current play */
    qdict_put_int(tape, "dropped", d->single_dropped);
    qdict_put(det, "tape", tape);

    /* A1: the classification report */
    qdict_put_str(align, "mode", d->align ? "on" : "off");
    qdict_put_int(align, "rows", d->align_rows);
    qdict_put_int(align, "fallback", d->align_fallback);
    qdict_put_int(align, "honoured", d->align_honoured);
    qdict_put_int(align, "mismatch_cells", d->align_mismatch_cells);
    qdict_put_int(align, "unassigned_cells", d->align_unassigned_cells);
    qdict_put_str(align, "calibration",
                  d->align_has_table ? "dss-linear" : "none");
    qdict_put_int(align, "preimage_miss", d->spec_preimage_miss);
    qdict_put(det, "align", align);

    /* C1: the ADC sky */
    qdict_put_str(adc, "mode",
                  (d->tape_state == STIX_DET_TAPE_PLAYING ?
                   d->tape_adc_mode : d->adc_mode) ? "on" : "off");
    qdict_put_int(adc, "kev_full", d->adc_kev_full);
    qdict_put_str(adc, "base_source", d->replay_loaded ? "replay" : "zero");
    qdict_put_int(adc, "map_rows",
                  d->adc_map_valid ?
                  STIX_DET_N_FPGA_DET * STIX_DET_N_PIX_ACC : 0);
    qdict_put(det, "adc", adc);
}

/*
 * D-Dh device reset (det-model on): explicit reset values for
 * 0x00-0x4C, since the controller reset does not clear its register
 * file otherwise: ideFxCmdSta CmdReady (0x80000000; 0 with
 * det-idefx=off), testPulseCmdSta Busy = 1, all others 0. The tick
 * stops, the FSM state clears; the decoders idle and every ASIC takes
 * its register defaults (the IDPU reset cuts the ASIC supply [I]); the
 * RAM banks are kept (they survive a reset, accepted-diffs §13). The
 * sky (D-4) loses its exposure and reseeds its PRNG from det-seed; the
 * nominal integration (D-5) restarts in bank 0; the scene (det-source,
 * det-flare, the rates, the replay trigger and offset) is kept: it
 * describes the Sun, not the IDPU (D-Db). A running DSS replay (D-7)
 * stops and re-arms on its trigger.
 */
void stix_det_reset(StixDetState *d)
{
    hwaddr addr;

    if (!d->model) {
        return;
    }
    for (addr = 0; addr <= STIX_DET_REG_LAST; addr += sizeof(uint32_t)) {
        *stix_det_regp(d, addr) = 0;
    }
    d->reg->testPulseCmdSta = STIX_DET_TP_BUSY;

    if (d->idefx) {
        uint32_t grp;

        d->reg->ideFxCmdSta = IDEFXCMDSTA_CMDREADY;
        for (grp = 0; grp < STIX_DET_N_GRP; grp++) {
            stix_det_idefx_idle(&d->idefx_dec[grp]);
        }
        stix_det_idefx_defaults_mask(d, 0xFFFFFFFFu, "device reset");
    }

    timer_del(d->tick);
    d->enable = false;
    d->ql_ready = false;
    d->cal_enable = false;
    d->active_ql_bank = 0;
    d->sumcnt = 0;
    d->ql_tick = 0;
    d->ql_int_time = 0;
    d->lvar_tick_count = 0;
    stix_det_sky_clear_expo(d);
    d->active_nom_bank = 0;
    d->nom_tick = 0;
    d->nom_int_time_min = 0;
    d->nom_int_time_max = 0;
    d->nom_int_events_max = 0;
    /* a pending det-inject is in-flight model state, not a scene knob */
    d->inject_pending = false;
    /* a burst repeats the last live chunk: none survives a reset */
    d->last_chunk_valid = false;
    d->burst_requested = 0;
    d->burst_appended = 0;
    d->burst_stop = "none";
    d->burst_rotb_count = 0;
    /*
     * D-Dk: a running replay stops and re-arms on its trigger; the
     * table and the scene knobs (trigger, offset) stay (they describe
     * the Sun, not the IDPU)
     */
    if (d->replay_running) {
        d->replay_running = false;
        trace_stixdet_replay_state("reset", "device reset",
                                   d->replay_time_tick, d->replay_cursor,
                                   d->replay_events_done);
    }
    d->replay_armed = d->replay_loaded;
    d->replay_stopped = false;      /* the re-arm reads armed, not stopped */
    d->replay_ended = false;
    d->replay_start_req = false;
    d->replay_class_pending = false;
    /*
     * S1: playback stops, the loaded tape is kept (state ready), like
     * the scene. The align/adc-mode knobs are bench intent and stay.
     */
    stix_det_tape_stop(d, "device reset");
    d->tape_state = d->tape_n_steps ? STIX_DET_TAPE_READY
                                    : STIX_DET_TAPE_IDLE;
    d->tape_play_req = false;
    d->tape_stop_req = false;
    d->tape_left_ticks = 0;
    stix_det_nom_clear_expo(d);
    stix_det_reseed(d);
}

bool stix_det_realize(StixDetState *d, STIXCTRL *owner, stixCtrlRegs_s *reg,
                      MemoryRegion *sram, MemoryRegion *lvar,
                      MemoryRegion *ltrg, MemoryRegion *etrg, qemu_irq *irq,
                      Error **errp)
{
    if (!stix_det_index_selfcheck(errp)) {
        return false;
    }
    d->owner = owner;
    d->reg = reg;
    d->etrg = etrg;
    d->sram = sram;
    d->lvar = lvar;
    d->ltrg = ltrg;
    d->irq = irq;
    d->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, stix_det_tick, d);
    stix_det_sky_template(d);
    stix_det_reseed(d);
    /*
     * D-Dk: the .dssevs pre-scan; a failure here is a realize error.
     * device_set_realized does not run unrealize for a realize failure,
     * so the timer is freed here, as the controller does for its own
     * allocations, and with it the table the scan may have buffered.
     */
    if (d->replay_file && *d->replay_file &&
        !stix_det_replay_scan(d, errp)) {
        timer_free(d->tick);
        d->tick = NULL;
        g_free(d->replay_ev);
        d->replay_ev = NULL;
        d->replay_n_ev = 0;
        d->replay_alloc = 0;
        d->replay_loaded = false;
        return false;
    }
    return true;
}

void stix_det_unrealize(StixDetState *d)
{
    timer_free(d->tick);
    d->tick = NULL;
    g_clear_pointer(&d->replay_ev, g_free);
    d->replay_n_ev = 0;
    d->replay_alloc = 0;
    d->replay_loaded = false;
    stix_det_tape_clear(d);
    d->tape_state = STIX_DET_TAPE_IDLE;
}
