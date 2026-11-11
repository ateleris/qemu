/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * STIX IDPU detector controller model. Tier 0 (detector plan D-1):
 * fsmCmd strobes, live fsmSta/fsmEvents, testPulseCmdSta, the 100 ms
 * tick, the QL bank switch with IRQ 3, and the register reset values.
 * Tier 1 (D-2): the IDeF-X serial command interface (ideFxCmdSta,
 * idefxModeData, idefxWriteMask), the 32-ASIC register store behind it,
 * and the index-space conversion tables.
 *
 * FSW references are to the ASW tree (BSW = BSW_ASW/src/BSW/...;
 * utils.c, utils.h = ASW/src/utils.*; Detectors.c = BSW Detectors.c).
 *
 * Re-entrancy (D-Dj): the tick writes the accumulator banks through
 * memory_region_get_ram_ptr (each write followed by
 * memory_region_set_dirty), never with address_space_*, and its only
 * other output is qemu_irq_pulse. No MMIO dispatch happens from timer
 * context.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
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
 *    from the shadow (tier 0 has no count source: zeros); its LVAR is
 *    not rewritten (it holds the per-tick slots);
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

    stix_det_ram_zero(d->sram, stix_det_lacc_off[done], STIXCTRL_LACC0_SIZE);
    stix_det_ram_zero(d->ltrg, stix_det_ltrg_off[done], STIXCTRL_LTRG0_SIZE);

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
 * One 100 ms tick of the active QL period. Tick k (0-based) writes its
 * LVAR count to slot k of the active bank while k < 40; later ticks of
 * a longer period (QlIntTime is 16 bits) are dropped, so no write ever
 * leaves the bank's 0xA0 bytes (D-Dd, a model choice: the hardware
 * behaviour is undocumented). The period is QlIntTime + 1 ticks.
 */
static void stix_det_tick(void *opaque)
{
    StixDetState *d = opaque;
    uint32_t slot = d->ql_tick;

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

    d->tick_deadline_ns += STIX_DET_TICK_NS;
    timer_mod_ns(d->tick, d->tick_deadline_ns);
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
 *   active bank (a fresh enable).
 * - Reset clears QlReady and SUMCNT, restarts the QL period and
 *   re-latches QlIntTime; ActiveQl and the banks stay [I]. It re-phases
 *   the tick only while enabled.
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
            /* a fresh period: no stale RAM in the active bank */
            stix_det_latch_ql_int_time(d);
            stix_det_bank_zero(d, d->active_ql_bank);
        }
        stix_det_tick_start(d);
    }
    if (strobes & FSMCMD_BIT(RESET)) {
        d->ql_ready = false;
        d->sumcnt = 0;
        d->ql_tick = 0;
        d->lvar_tick_count = 0;
        stix_det_latch_ql_int_time(d);
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
           ((uint32_t)d->ql_ready << STIXCTRL_FSMSTA_QLREADY) |
           ((uint32_t)d->cal_enable << STIXCTRL_FSMSTA_CALENABLE);
    /* QueueOverflow, ActiveNominal (D-5) and CalReady read 0 */
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
 * RAM banks are kept (they survive a reset, accepted-diffs §13).
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
}

bool stix_det_realize(StixDetState *d, stixCtrlRegs_s *reg,
                      MemoryRegion *sram, MemoryRegion *lvar,
                      MemoryRegion *ltrg, qemu_irq *irq, Error **errp)
{
    if (!stix_det_index_selfcheck(errp)) {
        return false;
    }
    d->reg = reg;
    d->sram = sram;
    d->lvar = lvar;
    d->ltrg = ltrg;
    d->irq = irq;
    d->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, stix_det_tick, d);
    return true;
}

void stix_det_unrealize(StixDetState *d)
{
    timer_free(d->tick);
    d->tick = NULL;
}
