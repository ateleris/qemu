/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * STIX IDPU aspect system (aspect plan AS-1 registers, AS-2 generator):
 * the register semantics of asCmdSta (0x68) and asQlData[4]
 * (0x6C-0x7B), the device reset, and the photodiode generator (D-ASb):
 * a 1 ms diode tick, N consecutive 12-bit samples summed per row with
 * AVGSIZE latched at the row start (the QlIntTime rule, D-Dd), a
 * 63-row double buffer that flips into ONE 512 B ROTB block per
 * 63*N ms, descriptor 0x4000, the LongTimer sampled at the flip,
 * pushed with stixctrl_rotb_append (the ENABLE /\ !TESTMODE gating,
 * whole-append-or-drop with the IRQ 7 pulse).
 *
 * asCmdSta (StixIdpu.h:667-685): ASAON (bit 0) and ASBON (bit 1) enable
 * the two ADC channels, AVGSIZE (10:8) selects the samples per summed
 * row (values 0..4; the FSW validates the value itself,
 * Aspect.c:108-118). The reserved bits 2..7 and 11..31
 * (StixIdpu.h:681-685) are dropped on write and read 0. Each asQlData
 * word is stored whole, but only its 16-bit DATA field reads back (the
 * FSW masks the upper half, Aspect.c:127-133).
 *
 * Tick lifecycle: the tick is armed by the first asCmdSta write that
 * sets a channel-enable bit (readout follows the channel supply, the
 * [F] power-on fact) and the reset stops it (D-ASf); the FSW powers the
 * channels at every boot/SAFE/NOMINAL entry (OpModeMgmt.c:1066-1068,
 * :229-230, :680), so between resets it never stops — a power-off does
 * not stop the readout, an off channel writes zeros and the block is
 * still produced (D-ASb, open question 1). The scene knobs (as-pattern,
 * as-sun, as-rate-ppm, as-enable) survive a reset; they apply at the
 * next tick.
 *
 * as-rate-ppm (D-ASd): the tick period becomes (1 + ppm*1e-6) ms of
 * virtual time while the LongTimer stays 1 s per 1e9 ns, so the block
 * period in LongTimer time is 63*N*(1+ppm*1e-6) ms — the arithmetic the
 * D-ASh trip points are pinned on (a negative rate shortens the block;
 * at N=16, -2000 ppm gives a 1005..1006 ms pair delta and inc = 15).
 * Since the period is an integer ns count, ppm is carried exactly.
 *
 * The experiment knobs (AS-3, D-ASd): as-jitter-next-ms shifts ONE
 * flip's timestamp then resets, as-step-next-ms (+ as-step-at-block)
 * shifts every flip from the armed one on, as-drop-next skips the ROTB
 * push of the next k flips — all at whole-ms exactness in the
 * corrected-SCET ns domain, validated N-aware by the shared pair check
 * so the FSW's increment estimate (AspectCtrl.c:219-230) stays in
 * 1..16. They are state, not scene: the reset clears them (D-ASf).
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "qapi/qapi-builtin-type-infos.h"
#include "qapi/visitor.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "hw/core/qdev.h"
#include "stix_aspect.h"
#include "stix_det.h"
#include "trace.h"

/* the stored asCmdSta bits: ASAON, ASBON and AVGSIZE (10:8) */
#define STIX_ASPECT_CMDSTA_RW \
    ((STIXCTRL_ASCMDSTA_ASAON_MASK << STIXCTRL_ASCMDSTA_ASAON) | \
     (STIXCTRL_ASCMDSTA_ASBON_MASK << STIXCTRL_ASCMDSTA_ASBON) | \
     (STIXCTRL_ASCMDSTA_AVGSIZE_MASK << STIXCTRL_ASCMDSTA_AVGSIZE))

/* the channel-enable bit of diode pair d (0,1 = A, 2,3 = B) */
#define STIX_ASPECT_CHON(a, ch) \
    ((a->reg->asCmdSta >> (ch)) & 1u)

static const char *const stix_aspect_pattern_name[STIX_ASPECT_PATTERN__MAX] = {
    [STIX_ASPECT_PATTERN_ZERO] = "zero",
    [STIX_ASPECT_PATTERN_RAMP] = "ramp",
};

/* the same names as the detector's chunk drop (stix_det.c) */
static const char *const stix_aspect_rotb_name[] = {
    [STIX_DET_ROTB_APPENDED] = "appended",
    [STIX_DET_ROTB_DISABLED] = "rotb-disabled",
    [STIX_DET_ROTB_TESTMODE] = "rotb-testmode",
    [STIX_DET_ROTB_FULL] = "ring-full",
};

static void stix_aspect_tick(void *opaque);
static void stix_aspect_flip(StixAspectState *a);

/* (re)arm the tick one diode tick from now (the stix_det tick pattern) */
static void stix_aspect_tick_start(StixAspectState *a)
{
    if (a->tick_on) {
        return;
    }
    a->tick_on = true;
    a->tick_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          STIX_ASPECT_TICK_NS + a->rate_ppm;
    timer_mod_ns(a->tick, a->tick_deadline_ns);
    trace_stixasp_tick_arm(1, a->tick_deadline_ns);
}

/* the next tick carries the rate: period = 1 ms + ppm ns (D-ASd) */
static void stix_aspect_tick_next(StixAspectState *a)
{
    a->tick_deadline_ns += STIX_ASPECT_TICK_NS + a->rate_ppm;
    timer_mod_ns(a->tick, a->tick_deadline_ns);
}

/*
 * D-ASc: one 12-bit ADC sample of diode d (0..3 = A0, A1, B0, B1).
 * ramp = base[d] + ((block_seq & 0x3F) << 6 | row) & 0xFFF, the sun
 * offset added on top and the result clamped into 12 bits (a saturating
 * ADC) before the N-sum.
 */
static uint32_t stix_aspect_sample(StixAspectState *a, uint32_t d)
{
    static const uint32_t base[4] = { 0x000u, 0x100u, 0x200u, 0x300u };
    uint32_t s;

    if (a->pattern == STIX_ASPECT_PATTERN_ZERO) {
        s = 0;
    } else {
        s = (base[d] + (((a->block_seq & 0x3Fu) << 6) | a->row)) & 0xFFFu;
    }
    s += a->sun;
    return s > 0xFFFu ? 0xFFFu : s;
}

/*
 * The open row is full: it lands in the open page and in asQlData (the
 * D-ASe "very last accumulation per diode", the last completed N-sum,
 * summed mod 65536 per D-ASb); the 63rd row flips the pages.
 */
static void stix_aspect_row_done(StixAspectState *a)
{
    uint32_t d;

    for (d = 0; d < 4; d++) {
        uint16_t v = a->acc[d] & 0xFFFFu;

        a->page[a->page_fill][a->row][d] = v;
        a->reg->asQlData[d] = v;
        a->acc[d] = 0;
    }
    a->row_samples = 0;
    a->row++;
    if (a->row == STIX_ASPECT_ROWS) {
        stix_aspect_flip(a);
    }
}

/*
 * The flip (D-ASb): the full page becomes one 512 B block — descriptor
 * 0x4000 with the block-number and reserved bits 0, the SCET as the
 * 0xC8/0xCC read at the flip (the integration END, the UNSYNCED flag
 * in bit 31 exactly as for the detector chunks), then the 63 rows as
 * 4 x u16 big-endian each. One stixctrl_rotb_append push, so the block
 * obeys the rotbConfig ENABLE /\ !TESTMODE gating and the drop-new
 * ring rule; a timer callback holds the BQL, so it can never interleave
 * inside a detector chunk (D-Dj).
 *
 * AS-3 (D-ASd): the one-shot jitter (the next flip only, then 0) and
 * the persistent step (every flip from step_start on) shift the block
 * timestamp by whole ms in the corrected-SCET ns domain, so the FSW's
 * whole-ms truncation (Timing.c:395-399) sees exactly +-d ms; the
 * as-drop-next counter skips only the ROTB push — the pages keep
 * flipping on the real timeline, which is what keeps the FSW's archive
 * time-rewrite chain continuous across a loss (ArchiveManager.c:243-245,
 * D-ASh row 5).
 */
static void stix_aspect_flip(StixAspectState *a)
{
    StixDetRotbResult r;
    uint32_t coarse_reg, rotb_count = 0, row, d;
    uint16_t fine;
    int64_t off_ns = 0;
    bool jittered = a->jitter_ms != 0;
    bool stepped = a->step_ms != 0 && a->block_seq >= a->step_start;

    if (jittered) {
        off_ns += (int64_t)a->jitter_ms * 1000000LL;
    }
    if (stepped) {
        off_ns += (int64_t)a->step_ms * 1000000LL;
    }
    stixctrl_scet_reg_at(a->owner, off_ns, &coarse_reg, &fine);
    stw_be_p(a->blk, STIX_ASPECT_DESC);
    stl_be_p(a->blk + 2, coarse_reg);
    stw_be_p(a->blk + 6, fine);
    for (row = 0; row < STIX_ASPECT_ROWS; row++) {
        for (d = 0; d < 4; d++) {
            stw_be_p(a->blk + STIX_ASPECT_DATA_OFF + 8u * row + 2u * d,
                     a->page[a->page_fill][row][d]);
        }
    }
    trace_stixasp_flip(a->block_seq, a->row_n, coarse_reg, fine);
    a->flip_coarse = coarse_reg;
    a->flip_fine = fine;

    if (jittered) {
        trace_stixasp_jitter(a->block_seq, a->jitter_ms);
        a->jitter_ms = 0;           /* one-shot: the next flip is clean */
        a->jittered++;
    }
    if (stepped) {
        if (!a->step_fired) {
            trace_stixasp_step(a->block_seq, a->step_ms);
            a->step_fired = true;   /* the boundary, once per arming */
        }
        a->stepped++;
    }

    if (a->drop_next != 0) {
        a->drop_next--;
        a->dropped++;
        a->dropped_knobs++;
        trace_stixasp_drop(a->block_seq, "knob");
    } else {
        r = stixctrl_rotb_append(a->owner, a->blk, 1, &rotb_count);
        trace_stixasp_push(a->block_seq, stix_aspect_rotb_name[r],
                           rotb_count);
        if (r == STIX_DET_ROTB_APPENDED) {
            a->pushed++;
        } else {
            a->dropped++;
            if (r == STIX_DET_ROTB_FULL) {
                a->rotb_full++;
                trace_stixasp_drop(a->block_seq, "rotb-full");
            }
        }
    }
    a->block_seq++;
    a->page_fill ^= 1u;
    a->row = 0;
}

/*
 * One diode tick: every enabled channel draws one 12-bit sample into
 * the open row (an off channel contributes 0), AVGSIZE is latched at
 * the row start so a mid-row change applies from the next row (the
 * QlIntTime latch rule, D-Dd). With as-enable=false the generator is
 * off: no samples, no rows, no blocks. D-Dj: the guest-visible writes
 * (asQlData, the ROTB ring) run with the controller's mem_reentrancy
 * guard engaged, outside any MMIO dispatch.
 */
static void stix_aspect_tick(void *opaque)
{
    StixAspectState *a = opaque;
    MemReentrancyGuard *guard = &DEVICE(a->owner)->mem_reentrancy_guard;
    bool guard_was = guard->engaged_in_io;
    uint32_t d;

    guard->engaged_in_io = true;
    if (a->enable) {
        if (a->row_samples == 0) {
            a->row_n = 1u << ((a->reg->asCmdSta >> STIXCTRL_ASCMDSTA_AVGSIZE) &
                              STIXCTRL_ASCMDSTA_AVGSIZE_MASK);
        }
        for (d = 0; d < 4; d++) {
            /* channels A/B: diode pairs 0-1 / 2-3, ASAON=0, ASBON=1 */
            if (STIX_ASPECT_CHON(a, d / 2u)) {
                a->acc[d] += stix_aspect_sample(a, d);
            }
        }
        a->row_samples++;
        if (a->row_samples >= a->row_n) {
            stix_aspect_row_done(a);
        }
    }
    stix_aspect_tick_next(a);
    guard->engaged_in_io = guard_was;
}

void stix_aspect_instance_init(StixAspectState *a)
{
    a->pattern = STIX_ASPECT_PATTERN_RAMP;
    a->row_n = 1;
    a->enable = true;           /* D-ASd: generation on by default */
}

void stix_aspect_realize(StixAspectState *a, STIXCTRL *owner,
                         stixCtrlRegs_s *reg)
{
    a->owner = owner;
    a->reg = reg;
    a->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, stix_aspect_tick, a);
}

void stix_aspect_unrealize(StixAspectState *a)
{
    timer_free(a->tick);
    a->tick = NULL;
}

/*
 * D-ASf: a watchdog reboot cuts the aspect supply, and the FSW
 * re-powers and re-configures at every boot/SAFE/NOMINAL entry
 * (OpModeMgmt.c:1066-1068), so the tick stops, the registers start
 * from 0 (channels off, AVGSIZE 0) and the row/buffer state, the
 * counters and the experiment knobs (jitter, step, drop: state, not
 * scene) clear. The scene knobs (as-pattern, as-sun, as-rate-ppm,
 * as-enable) survive: they describe the Sun, the D-Db rule.
 * as-model=off keeps today's plain storage and no reset
 * (grlib_stixctrl.c falls through to the register file).
 */
void stix_aspect_reset(StixAspectState *a)
{
    uint32_t i, p, r;

    if (!a->model) {
        return;
    }
    timer_del(a->tick);
    a->tick_on = false;
    a->page_fill = 0;
    a->row = 0;
    a->row_samples = 0;
    a->row_n = 1;
    for (p = 0; p < 2; p++) {
        for (r = 0; r < STIX_ASPECT_ROWS; r++) {
            memset(a->page[p][r], 0, sizeof(a->page[p][r]));
        }
    }
    memset(a->acc, 0, sizeof(a->acc));
    a->block_seq = 0;
    a->pushed = 0;
    a->dropped = 0;
    a->dropped_knobs = 0;
    a->rotb_full = 0;
    a->jittered = 0;
    a->stepped = 0;
    a->flip_coarse = 0;
    a->flip_fine = 0;
    a->jitter_ms = 0;
    a->step_ms = 0;
    a->step_at = 0;
    a->step_start = 0;
    a->step_fired = false;
    a->drop_next = 0;
    a->reg->asCmdSta = 0;
    for (i = 0; i < 4; i++) {
        a->reg->asQlData[i] = 0;
    }
}

bool stix_aspect_owns(StixAspectState *a, hwaddr addr)
{
    return a->model && addr >= STIX_ASPECT_REG_FIRST &&
           addr <= STIX_ASPECT_REG_LAST;
}

uint32_t stix_aspect_read(StixAspectState *a, hwaddr addr)
{
    unsigned idx = (addr - STIX_ASPECT_REG_FIRST) / 4u;

    /* word index from 0x68 (0 = asCmdSta, 1..4 = QL), never negative */
    if (idx == 0) {
        return a->reg->asCmdSta;    /* stored masked; reserved reads 0 */
    }
    return a->reg->asQlData[idx - 1] & STIXCTRL_ASQLDATA_DATA_MASK;
}

void stix_aspect_write(StixAspectState *a, hwaddr addr, uint32_t value)
{
    unsigned idx = (addr - STIX_ASPECT_REG_FIRST) / 4u;

    if (idx == 0) {
        a->reg->asCmdSta = value & STIX_ASPECT_CMDSTA_RW;
        /*
         * the readout starts with the channel supply (the [F] power-on
         * fact; the FSW's Aspect_setPower / HWConfig_aspectPowerOn);
         * once running, config writes never stop or re-phase it
         */
        if (a->reg->asCmdSta &
            ((STIXCTRL_ASCMDSTA_ASAON_MASK << STIXCTRL_ASCMDSTA_ASAON) |
             (STIXCTRL_ASCMDSTA_ASBON_MASK << STIXCTRL_ASCMDSTA_ASBON))) {
            stix_aspect_tick_start(a);
        }
        return;
    }
    a->reg->asQlData[idx - 1] = value;
}

/* ---- the runtime scene knobs (D-ASc/D-ASd), on /machine/stixctrl ---- */

static StixAspectState *stix_aspect_of(Object *obj, void *opaque)
{
    return (StixAspectState *)((uint8_t *)obj + GPOINTER_TO_SIZE(opaque));
}

static void stix_aspect_trace_scene(StixAspectState *a)
{
    trace_stixasp_scene(stix_aspect_pattern_name[a->pattern], a->sun,
                        a->rate_ppm, a->enable);
}

static void stix_aspect_pattern_get(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp)
{
    StixAspectState *a = stix_aspect_of(obj, opaque);
    char *value = g_strdup(stix_aspect_pattern_name[a->pattern]);

    visit_type_str(v, name, &value, errp);
    g_free(value);
}

static void stix_aspect_pattern_set(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp)
{
    StixAspectState *a = stix_aspect_of(obj, opaque);
    g_autofree char *value = NULL;
    uint32_t i;

    if (!visit_type_str(v, name, &value, errp)) {
        return;
    }
    for (i = 0; i < STIX_ASPECT_PATTERN__MAX; i++) {
        if (!strcmp(value, stix_aspect_pattern_name[i])) {
            a->pattern = i;
            stix_aspect_trace_scene(a);
            return;
        }
    }
    error_setg(errp, "%s: unknown pattern '%s' (zero or ramp)", name, value);
}

static void stix_aspect_sun_get(Object *obj, Visitor *v,
                                const char *name, void *opaque, Error **errp)
{
    uint32_t value = stix_aspect_of(obj, opaque)->sun;

    visit_type_uint32(v, name, &value, errp);
}

static void stix_aspect_sun_set(Object *obj, Visitor *v,
                                const char *name, void *opaque, Error **errp)
{
    StixAspectState *a = stix_aspect_of(obj, opaque);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value > 0xFFFFu) {
        error_setg(errp, "%s: %" PRIu32 " is past the u16 sun field", name,
                   value);
        return;
    }
    a->sun = value;
    stix_aspect_trace_scene(a);
}

static void stix_aspect_rate_get(Object *obj, Visitor *v,
                                 const char *name, void *opaque, Error **errp)
{
    int32_t value = stix_aspect_of(obj, opaque)->rate_ppm;

    visit_type_int32(v, name, &value, errp);
}

/*
 * D-ASd, the N-aware pair check shared by the rate/step/jitter setters:
 * the FSW's once-per-request increment estimate is floor(pair delta / 63)
 * from two whole-ms-truncated timestamps (Timing_ToMs, Timing.c:395-399),
 * and its ASSERTs (AspectCtrl.c:219-230) hang unless it stays in 1..16.
 * A (rate, step, jitter) combination is accepted only if every pair
 * delta it can produce does: the pair period P = 63*N*(1+ppm*1e-6) ms
 * shifts by up to |step| + |jitter| ms at the estimate pair, and the
 * truncated delta is floor(P) or floor(P) + 1, so the envelope must
 * keep floor(P - spread) >= 63 and floor(P + spread) + 1 <= 1070, i.e.
 * P - spread >= 63 ms and P + spread < 1070 ms at the latched N (at
 * N = 1 that rejects any ppm < 0 and any +-32 ms jitter/step; a +-32 ms
 * jitter alone needs N >= 2, together with a +-32 ms step N >= 3). Raw AVGSIZE 5..7 (N = 32..128) is out of the FSW
 * contract (Aspect.c:108-118 validates 0..4); the upper bound is not
 * enforced there (P already exceeds it).
 */
static bool stix_aspect_pair_ok(StixAspectState *a, int32_t ppm,
                                int32_t step_ms, int32_t jitter_ms)
{
    int64_t p_ns = (int64_t)STIX_ASPECT_ROWS * a->row_n *
                   (STIX_ASPECT_TICK_NS + ppm);
    int64_t spread_ns = (int64_t)(step_ms < 0 ? -step_ms : step_ms) *
                        1000000LL +
                        (int64_t)(jitter_ms < 0 ? -jitter_ms : jitter_ms) *
                        1000000LL;

    if (p_ns - spread_ns < (int64_t)STIX_ASPECT_ROWS * STIX_ASPECT_TICK_NS) {
        return false;               /* the pair delta can truncate < 63 ms */
    }
    if (a->row_n <= 16 &&
        p_ns + spread_ns >= 1070LL * 1000000LL) {
        return false;               /* the pair delta can reach inc > 16 */
    }
    return true;
}

static void stix_aspect_rate_set(Object *obj, Visitor *v,
                                 const char *name, void *opaque, Error **errp)
{
    StixAspectState *a = stix_aspect_of(obj, opaque);
    int32_t value;

    if (!visit_type_int32(v, name, &value, errp)) {
        return;
    }
    if (value < -STIX_ASPECT_PPM_MAX || value > STIX_ASPECT_PPM_MAX) {
        error_setg(errp, "%s: %d ppm is outside +-%d", name, value,
                   STIX_ASPECT_PPM_MAX);
        return;
    }
    if (!stix_aspect_pair_ok(a, value, a->step_ms, a->jitter_ms)) {
        error_setg(errp, "%s: %d ppm with the armed step %d ms and jitter "
                   "%d ms leaves the FSW's increment estimate outside "
                   "1..16 at the latched N = %u", name, value, a->step_ms,
                   a->jitter_ms, a->row_n);
        return;
    }
    a->rate_ppm = value;
    stix_aspect_trace_scene(a);
}

static void stix_aspect_enable_get(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    bool value = stix_aspect_of(obj, opaque)->enable;

    visit_type_bool(v, name, &value, errp);
}

static void stix_aspect_enable_set(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    StixAspectState *a = stix_aspect_of(obj, opaque);
    bool value;

    if (!visit_type_bool(v, name, &value, errp)) {
        return;
    }
    a->enable = value;
    if (!value) { /* the one-shots never outlive the off window */
        a->jitter_ms = 0;
        a->drop_next = 0;
    }
    stix_aspect_trace_scene(a);
}

/* ---- the experiment knobs (AS-3, D-ASd), write-only (det-inject
 * pattern): the current values live in the hw-state "as" section ---- */

/*
 * as-jitter-next-ms: added to the NEXT flipped block's timestamp only,
 * then back to 0 — a one-block error, the two gap events (entry +d,
 * exit -d) of D-ASh row 2. Refused while as-enable=false (a one-shot
 * must not outlive the off window) and by the N-aware pair check with
 * the armed step.
 */
static void stix_aspect_jitter_set(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    StixAspectState *a = stix_aspect_of(obj, opaque);
    int32_t value;

    if (!visit_type_int32(v, name, &value, errp)) {
        return;
    }
    if (value < -STIX_ASPECT_STEP_MS_MAX ||
        value > STIX_ASPECT_STEP_MS_MAX) {
        error_setg(errp, "%s: %d ms is outside +-%d", name, value,
                   STIX_ASPECT_STEP_MS_MAX);
        return;
    }
    if (!a->enable) {
        error_setg(errp, "%s: refused while as-enable=false", name);
        return;
    }
    if (!stix_aspect_pair_ok(a, a->rate_ppm, a->step_ms, value)) {
        error_setg(errp, "%s: %d ms with the armed step %d ms and rate "
                   "%d ppm leaves the FSW's increment estimate outside "
                   "1..16 at the latched N = %u", name, value, a->step_ms,
                   a->rate_ppm, a->row_n);
        return;
    }
    a->jitter_ms = value;
}

/*
 * as-step-next-ms: a persistent step — every flip from the armed one on
 * carries +-d ms (a biased FPGA epoch; the single boundary gap of
 * D-ASh row 3, or the corrupted estimate pair of row 7). 0 clears it.
 * Not refused while as-enable=false: it is state, and it applies when
 * generation resumes. Arming resolves as-step-at-block (0 = the next
 * flip) against the live block_seq at once.
 */
static void stix_aspect_step_set(Object *obj, Visitor *v,
                                 const char *name, void *opaque,
                                 Error **errp)
{
    StixAspectState *a = stix_aspect_of(obj, opaque);
    int32_t value;

    if (!visit_type_int32(v, name, &value, errp)) {
        return;
    }
    if (value < -STIX_ASPECT_STEP_MS_MAX ||
        value > STIX_ASPECT_STEP_MS_MAX) {
        error_setg(errp, "%s: %d ms is outside +-%d", name, value,
                   STIX_ASPECT_STEP_MS_MAX);
        return;
    }
    if (!stix_aspect_pair_ok(a, a->rate_ppm, value, a->jitter_ms)) {
        error_setg(errp, "%s: %d ms with the rate %d ppm and jitter %d ms "
                   "leaves the FSW's increment estimate outside 1..16 at "
                   "the latched N = %u", name, value, a->rate_ppm,
                   a->jitter_ms, a->row_n);
        return;
    }
    a->step_ms = value;
    if (value != 0) { /* 0 clears; keep the last start for the hw-state */
        a->step_start = a->step_at ? a->step_at : a->block_seq;
        a->step_fired = false;
    }
}

/*
 * as-step-at-block: where the step starts — 0 = the next flip,
 * otherwise the block_seq (the model-wide flip counter, in the hw-state
 * and the stixasp_flip trace) whose flip begins the step, the exact
 * placement the D-ASh row-7 leg needs. It is resolved when the step is
 * armed (or here, if one is armed already); a block_seq already passed
 * applies from the next flip.
 */
static void stix_aspect_step_at_set(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp)
{
    StixAspectState *a = stix_aspect_of(obj, opaque);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    a->step_at = value;
    if (a->step_ms != 0) {
        a->step_start = value ? value : a->block_seq;
        a->step_fired = false;
    }
}

/*
 * as-drop-next: the next k flipped blocks are not appended (the double
 * buffer keeps running; the archive's time-rewrite chain,
 * ArchiveManager.c:243-245, leaves no time gap — the only signature is
 * the payload content jump, D-ASh row 5). It counts down at the flips.
 * Refused while as-enable=false: no flips happen there, and the count
 * must not outlive the off window.
 */
static void stix_aspect_drop_set(Object *obj, Visitor *v,
                                 const char *name, void *opaque,
                                 Error **errp)
{
    StixAspectState *a = stix_aspect_of(obj, opaque);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (!a->enable) {
        error_setg(errp, "%s: refused while as-enable=false", name);
        return;
    }
    a->drop_next = value;
}

/* ---- the hw-state "as" section (D-ASg), one accessor, pure ---- */

void stix_aspect_hw_state(StixAspectState *a, QDict *as)
{
    QList *ql = qlist_new();
    uint32_t i;

    qdict_put_bool(as, "asaon",
                   a->reg->asCmdSta &
                   (STIXCTRL_ASCMDSTA_ASAON_MASK << STIXCTRL_ASCMDSTA_ASAON));
    qdict_put_bool(as, "asbon",
                   a->reg->asCmdSta &
                   (STIXCTRL_ASCMDSTA_ASBON_MASK << STIXCTRL_ASCMDSTA_ASBON));
    qdict_put_int(as, "avgsize",
                  (a->reg->asCmdSta >> STIXCTRL_ASCMDSTA_AVGSIZE) &
                  STIXCTRL_ASCMDSTA_AVGSIZE_MASK);
    qdict_put_int(as, "n", a->row_n);
    qdict_put_str(as, "pattern", stix_aspect_pattern_name[a->pattern]);
    qdict_put_int(as, "sun", a->sun);
    qdict_put_int(as, "rate_ppm", a->rate_ppm);
    qdict_put_bool(as, "enable", a->enable);
    qdict_put_bool(as, "tick", a->tick_on);
    qdict_put_int(as, "block_seq", a->block_seq);
    qdict_put_int(as, "rows", a->row);
    qdict_put_int(as, "pushed", a->pushed);
    qdict_put_int(as, "dropped", a->dropped);
    qdict_put_int(as, "dropped_knobs", a->dropped_knobs);
    qdict_put_int(as, "rotb_full", a->rotb_full);
    qdict_put_int(as, "jittered", a->jittered);
    qdict_put_int(as, "stepped", a->stepped);
    /* the experiment knobs' current values (AS-3, D-ASd; fields added
     * to the schema-4 section, no schema bump) */
    qdict_put_int(as, "jitter_next_ms", a->jitter_ms);
    qdict_put_int(as, "step_next_ms", a->step_ms);
    qdict_put_int(as, "step_at_block", a->step_at);
    qdict_put_int(as, "step_start", a->step_start);
    qdict_put_int(as, "drop_next", a->drop_next);
    qdict_put_int(as, "flip_coarse", a->flip_coarse);
    qdict_put_int(as, "flip_fine", a->flip_fine);
    for (i = 0; i < 4; i++) {
        qlist_append_int(ql, a->reg->asQlData[i] &
                         STIXCTRL_ASQLDATA_DATA_MASK);
    }
    qdict_put(as, "ql", ql);
}

/* the det-* pattern: runtime class properties on the controller */
void stix_aspect_class_init(ObjectClass *klass, size_t as_offset)
{
    void *opaque = GSIZE_TO_POINTER(as_offset);

    object_class_property_add(klass, "as-pattern", "str", &str_type_info,
                              stix_aspect_pattern_get,
                              stix_aspect_pattern_set, NULL, opaque);
    object_class_property_set_description(klass, "as-pattern",
        "the photodiode sample pattern: zero | ramp (the ramp encodes "
        "block_seq, row and diode in every 12-bit sample; runtime; "
        "survives a device reset)");
    object_class_property_add(klass, "as-sun", "uint32",
                              &uint32_type_info, stix_aspect_sun_get,
                              stix_aspect_sun_set, NULL, opaque);
    object_class_property_set_description(klass, "as-sun",
        "sun offset added to every diode sample, 0..65535, clamped into "
        "12 bits before the N-sum (runtime; survives a device reset)");
    object_class_property_add(klass, "as-rate-ppm", "int32",
                              &int32_type_info, stix_aspect_rate_get,
                              stix_aspect_rate_set, NULL, opaque);
    object_class_property_set_description(klass, "as-rate-ppm",
        "diode tick rate error in ppm: the tick period becomes "
        "(1 + ppm*1e-6) ms, so the block period in LongTimer time is "
        "63*N*(1+ppm*1e-6) ms; bounded to +-5000, >= 0 while N = 1 is "
        "latched (D-ASd; runtime; survives a device reset)");
    object_class_property_add(klass, "as-enable", "bool",
                              &bool_type_info, stix_aspect_enable_get,
                              stix_aspect_enable_set, NULL, opaque);
    object_class_property_set_description(klass, "as-enable",
        "the generation switch: false = no samples, no rows, no blocks "
        "at all (runtime; survives a device reset)");
    /* AS-3 (D-ASd), the experiment knobs: write-only, the current
     * values are in the hw-state "as" section; state, not scene (the
     * device reset clears them) */
    object_class_property_add(klass, "as-jitter-next-ms", "int32",
                              &int32_type_info, NULL,
                              stix_aspect_jitter_set, NULL, opaque);
    object_class_property_set_description(klass, "as-jitter-next-ms",
        "one-shot: the next flipped block's timestamp shifts by this "
        "many ms (-32..32), then the knob is back to 0 (runtime)");
    object_class_property_add(klass, "as-step-next-ms", "int32",
                              &int32_type_info, NULL,
                              stix_aspect_step_set, NULL, opaque);
    object_class_property_set_description(klass, "as-step-next-ms",
        "persistent: every flip from the armed one on shifts its "
        "timestamp by this many ms (-32..32); 0 clears (runtime)");
    object_class_property_add(klass, "as-step-at-block", "uint32",
                              &uint32_type_info, NULL,
                              stix_aspect_step_at_set, NULL, opaque);
    object_class_property_set_description(klass, "as-step-at-block",
        "where the step starts: 0 = the next flip, otherwise the "
        "block_seq whose flip begins it (runtime)");
    object_class_property_add(klass, "as-drop-next", "uint32",
                              &uint32_type_info, NULL,
                              stix_aspect_drop_set, NULL, opaque);
    object_class_property_set_description(klass, "as-drop-next",
        "the next k flipped blocks are not appended (the pages keep "
        "flipping); counts down at the flips (runtime)");
}
