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
 * its ticks feed stix_det_add_counts like det-inject.
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

    stix_det_sky_materialize(d, done);

    d->active_ql_bank = next;
    stix_det_bank_zero(d, next);

    d->ql_tick = 0;
    stix_det_latch_ql_int_time(d);
    d->ql_ready = true;
    trace_stixdet_ql_swap(done, next, d->ql_int_time, irq);
    /*
     * D-Dk: re-classify the ELUT rows for the coming period, so a
     * TC(237,8) apply during NOMINAL takes effect within one period
     */
    if (d->replay_loaded && d->source == STIX_DET_SOURCE_REPLAY_DSS) {
        stix_det_replay_classify(d);
    }
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
 */
static void stix_det_sky_tick(StixDetState *d)
{
    uint32_t profile = d->flare ? STIX_DET_PROFILE_FLARE :
                                  STIX_DET_PROFILE_QUIET;
    uint32_t cps = d->flare ? d->flare_cps : d->quiet_cps;
    uint32_t sumd_fpga_det = d->reg->fsmSumDmask;
    uint32_t vard_fpga_det = d->reg->fsmQlVarDMask;
    uint32_t varp_pix = (d->reg->fsmQlConfig >>
                         STIXCTRL_FSMQLCONFIG_QLVARPMASK) &
                        STIXCTRL_FSMQLCONFIG_QLVARPMASK_MASK;
    double tick_counts, sum_share, var_share, sum_lambda = 0, var_lambda = 0;
    uint32_t fpga_det, pix;
    uint64_t n;

    if (d->source != STIX_DET_SOURCE_SYNTHETIC || cps == 0) {
        return;
    }
    tick_counts = cps * (STIX_DET_TICK_NS / 1e9);
    sum_share = stix_det_ebin_share_masked(d, profile, d->reg->fsmSumEmask);
    var_share = stix_det_ebin_share_masked(d, profile, d->reg->fsmQlVarEMask);

    for (fpga_det = 0; fpga_det < STIX_DET_N_FPGA_DET; fpga_det++) {
        uint32_t pix_mask = stix_det_pix_gate(d, fpga_det);

        for (pix = 0; pix < STIX_DET_N_PIX_ACC; pix++) {
            double c;

            if (!((pix_mask >> pix) & 1)) {
                continue;
            }
            d->expo[fpga_det][pix][profile] += tick_counts;
            c = tick_counts * d->pix_share[pix];
            if ((sumd_fpga_det >> fpga_det) & 1) {
                d->nom_expo[fpga_det][pix][profile] += tick_counts;
                sum_lambda += c * sum_share;
            } else {
                d->nom_trg_extra_grp[stix_det_grp_of(fpga_det)] += c;
            }
            if (((vard_fpga_det >> fpga_det) & 1) && ((varp_pix >> pix) & 1)) {
                var_lambda += c * var_share;
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
 * Classify every (fpga_det, pix) ELUT row (D-Dk): a constant row (all
 * 0 — the power-on/EDAC state — or all 31 — what applyELUT writes from
 * the all-zero boundary table of an empty flash, LUTMgmt.c:912 — or
 * any other constant) cannot be a calibration, so the row falls back
 * to the linear binning. Runs at each SetEnable 0->1 edge and at each
 * QL swap while det-source=replay-dss, so a TC(237,8) apply during
 * NOMINAL takes effect within one period. A row newly classified
 * degenerate traces stixdet_elut_fallback once.
 */
static void stix_det_replay_classify(StixDetState *d)
{
    uint32_t fpga_det, pix;

    for (fpga_det = 0; fpga_det < STIX_DET_N_FPGA_DET; fpga_det++) {
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
        }
    }
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
 * here (the first edge after a reset — or after realize — re-arms), and
 * the ELUT rows are classified for the coming period. Only with
 * det-source=replay-dss.
 */
static void stix_det_replay_enable_edge(StixDetState *d)
{
    if (!d->replay_loaded || d->source != STIX_DET_SOURCE_REPLAY_DSS) {
        return;
    }
    stix_det_replay_classify(d);
    d->replay_class_pending = false;
    if (d->replay_armed && !d->replay_running &&
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
        stix_det_replay_classify(d);
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
        uint32_t ebin;

        d->replay_cursor++;
        d->replay_events_done++;
        if (!((stix_det_pix_gate(d, e->fpga_det) >> e->pix) & 1)) {
            /* the D-6 gate dropped it, like a real event: count it */
            d->replay_dropped++;
            continue;
        }
        if (d->replay_row_fallback[e->fpga_det][e->pix]) {
            if (e->adc <= d->replay_base_dss_det[e->fpga_det]) {
                continue;       /* at or below the baseline: no bin */
            }
            ebin = stix_det_replay_fallback_ebin(d, e->fpga_det, e->adc);
        } else {
            ebin = stix_det_replay_elut_ebin(d, e->fpga_det, e->pix,
                                             e->adc);
            if (ebin >= STIX_DET_N_EBIN) {
                continue;       /* 32 = unassigned: the FPGA drops it */
            }
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
 * Draw one accumulator bank from an exposure set (D-Df): each cell
 * [pix][fpga_det][ebin] (u16 big-endian at byte ((pix * 32 + fpga_det)
 * * 32 + ebin) * 2, Detectors.h:68-79, the same layout for LACC and
 * EACC) is Poisson(sum over profiles of ebin share x exposure), plus
 * any injected counts (D-6) at the cell, wrapping at 16 bits. Adds the
 * drawn events per grp to @grp_events; returns the total. The consumed
 * deposits are cleared here.
 */
static uint64_t stix_det_draw_acc(StixDetState *d,
                                  double (*expo_set)[STIX_DET_N_PIX_ACC]
                                                    [STIX_DET_N_PROFILE],
                                  uint32_t (*inj)[STIX_DET_N_PIX_ACC]
                                                 [STIX_DET_N_EBIN],
                                  uint8_t *acc,
                                  uint64_t grp_events[STIX_DET_N_GRP])
{
    uint64_t acc_events = 0;
    uint32_t pix, fpga_det, ebin;

    for (pix = 0; pix < STIX_DET_N_PIX_ACC; pix++) {
        for (fpga_det = 0; fpga_det < STIX_DET_N_FPGA_DET; fpga_det++) {
            const double *expo = expo_set[fpga_det][pix];

            for (ebin = 0; ebin < STIX_DET_N_EBIN; ebin++) {
                double lambda = d->pix_share[pix] *
                    (expo[STIX_DET_PROFILE_QUIET] *
                     d->ebin_share[STIX_DET_PROFILE_QUIET][ebin] +
                     expo[STIX_DET_PROFILE_FLARE] *
                     d->ebin_share[STIX_DET_PROFILE_FLARE][ebin]);
                uint64_t n = stix_det_poisson(d, lambda) +
                             inj[fpga_det][pix][ebin];

                inj[fpga_det][pix][ebin] = 0;
                stw_be_p(acc + ((pix * STIX_DET_N_FPGA_DET + fpga_det) *
                                STIX_DET_N_EBIN + ebin) * sizeof(uint16_t),
                         (uint16_t)n);
                grp_events[stix_det_grp_of(fpga_det)] += n;
                acc_events += n;
            }
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

    lacc_events = stix_det_draw_acc(d, d->expo, d->inj_ql, lacc,
                                    grp_events);
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
 *    0, all or nothing). The FSM is enabled here by construction: the
 *    tick only runs while it is;
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

    eacc_events = stix_det_draw_acc(d, d->nom_expo, d->inj_nom, eacc,
                                    grp_events);
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
 * det-replay-status (D-PLm req 4): one atomic read-only line with the
 * replay's state and progress, for the platform controller. The state
 * is idle (no table loaded), armed (loaded, waiting for its trigger —
 * the nominal SetEnable edge or a det-replay-start=true), running,
 * ended (EOF reached) or stopped (a det-replay-start=false stop; a
 * device reset re-arms instead, so it reads armed again).
 */
static void stix_det_replay_status_get(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    StixDetState *d = stix_det_of(obj, opaque);
    uint64_t duration_ns = 0;
    const char *state;
    char *value;

    if (!d->replay_loaded) {
        state = "idle";
    } else if (d->replay_running) {
        state = "running";
    } else if (d->replay_stopped) {
        state = "stopped";
    } else if (d->replay_ended) {
        state = "ended";
    } else {
        state = "armed";
    }
    if (d->replay_n_ev) {
        duration_ns = (uint64_t)(d->replay_ev[d->replay_n_ev - 1].t_tick *
                                 STIX_DET_TICK_NS);
    }
    value = g_strdup_printf("state=%s,pos_ns=%" PRIu64 ",events=%" PRIu64
                            ",dropped=%" PRIu64 ",unsupported=%" PRIu64
                            ",table=%s,count=%u,duration_ns=%" PRIu64,
                            state,
                            (uint64_t)(d->replay_time_tick *
                                       STIX_DET_TICK_NS),
                            d->replay_events_done, d->replay_dropped,
                            d->replay_unsupported,
                            d->replay_file ? d->replay_file : "",
                            d->replay_n_ev, duration_ns);
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

void stix_det_instance_init(StixDetState *d)
{
    d->source = STIX_DET_SOURCE_SYNTHETIC;
    d->flare = false;
    d->quiet_cps = STIX_DET_QUIET_CPS_DEFAULT;
    d->flare_cps = STIX_DET_FLARE_CPS_DEFAULT;
    d->replay_trigger = STIX_DET_REPLAY_TRIGGER_NOMINAL;
    d->replay_offset_ms = 0;
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
}
