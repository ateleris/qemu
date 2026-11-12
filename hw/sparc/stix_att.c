/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * STIX IDPU attenuator mechanism and controller (attenuator plan A-1,
 * D-Aa..Ae, Ah, Ai). Two motors drive one shaft between the OUT and IN
 * end switches; AttnCmdSta (CTRL APB 0x5C) reports the live status the
 * FSW polls every 50 ms (ASW AttCtrl.c:133-148). The FSW driver writes
 * cmd, or cmd | 1 << 4 (ERRORMASK, the switch override), as the whole
 * register and reads single bits (BSW Attenuator.c:99-104, :200-239);
 * MOTORxUSED is the drive flag, cleared at the end (AttCtrl.c:387-388).
 *
 * Lazy evaluation (D-Ac): no timer. Every 0x5C access and every QOM get
 * advances the pure state to now (QEMU_CLOCK_VIRTUAL); the next event
 * (the presented target switch plus the debounce, or the drive-window
 * end) is solved analytically at the command. The advance is
 * idempotent, so an extra read changes nothing (the PL-1/PL-2 purity
 * note). Getters advance too but never trace (D-Ai): an edge trace
 * carries the computed event time and fires only from an MMIO access.
 *
 * The fault layer (A-3, D-Ag): named failures with the platform's
 * dotted IDs, one QOM property each, persistent until cleared and
 * across a device reset. A fault set or clear mid-move advances the
 * state to now under the old faults, folds the position at the
 * boundary and re-solves the end from now (piecewise). Forcing
 * (force.cmdsta-set/-clear, bits 11:5 only) applies to the 0x5C
 * readback after everything else and clears on reset, like adc-inject.
 *
 * Re-entrancy (D-Ai): the unit has no timer, BH, DMA or IRQ and never
 * calls address_space_* or a stixctrl MMIO handler, so the
 * a83fe9a3a5 guard rule does not apply. The 0x5C hook covers 0x5C
 * only; 0x60/0x64 stay plain storage, so the trace golden is
 * unchanged (D-Aa). The ATT_C current (hkAdc ch 8) follows the
 * mechanism under the current.* faults (A-2 = D-Af, A-3 = D-Ag).
 */

#include "qemu/osdep.h"
#include "qapi/qapi-builtin-type-infos.h"
#include "qapi/visitor.h"
#include "qemu/timer.h"
#include "stix_att.h"
#include "trace.h"

/* 1 ppm of the shaft takes travel_ms ns: the full travel is travel_ms ms */
#define STIX_ATT_MS_NS (1000 * 1000LL)

static const char *const stix_att_end_name[STIX_ATT_END__MAX] = {
    "none", "target", "timeout", "override", "emergency", "fail", "reset",
};

/* ---- the A-3 fault layer (D-Ag) ---- */

/*
 * The implemented catalogue rows (the plan's "A-3 set"). The IDs are
 * the platform's dotted IDs (D-PLq); each is one QOM property under
 * /machine/stixctrl/attenuator. resolves marks the rows whose set or
 * clear re-solves a running move from now (the piecewise inject,
 * D-Ag): every bool fault but current.zero — the stuck sensors
 * through the stop rule, which reads the presented target switch
 * (D-Ae). current.offset is int32, not bool, and has its own accessor
 * below; it never touches the move.
 */
enum {
    STIX_ATT_F_SAB_OPEN,
    STIX_ATT_F_SAB_CLOSED,
    STIX_ATT_F_SBC_OPEN,
    STIX_ATT_F_SBC_CLOSED,
    STIX_ATT_F_M1_DEAD,
    STIX_ATT_F_M2_DEAD,
    STIX_ATT_F_JAMMED,
    STIX_ATT_F_C_ZERO,
    STIX_ATT_F_BOOL__MAX,
};

#define STIX_ATT_F_C_OFFSET_ID "attenuator.current.offset"

static const struct {
    const char *id;
    size_t off;                 /* the bool field in StixAttState */
    bool resolves;              /* a set or clear re-solves the move */
} stix_att_fault_bool[STIX_ATT_F_BOOL__MAX] = {
    [STIX_ATT_F_SAB_OPEN] = { "attenuator.sensor_ab.stuck_open",
                              offsetof(StixAttState, f_ab_open), true },
    [STIX_ATT_F_SAB_CLOSED] = { "attenuator.sensor_ab.stuck_closed",
                                offsetof(StixAttState, f_ab_closed), true },
    [STIX_ATT_F_SBC_OPEN] = { "attenuator.sensor_bc.stuck_open",
                              offsetof(StixAttState, f_bc_open), true },
    [STIX_ATT_F_SBC_CLOSED] = { "attenuator.sensor_bc.stuck_closed",
                                offsetof(StixAttState, f_bc_closed), true },
    [STIX_ATT_F_M1_DEAD] = { "attenuator.motor1.dead",
                             offsetof(StixAttState, f_m1_dead), true },
    [STIX_ATT_F_M2_DEAD] = { "attenuator.motor2.dead",
                             offsetof(StixAttState, f_m2_dead), true },
    [STIX_ATT_F_JAMMED] = { "attenuator.mechanism.jammed",
                            offsetof(StixAttState, f_jammed), true },
    [STIX_ATT_F_C_ZERO] = { "attenuator.current.zero",
                            offsetof(StixAttState, f_c_zero), false },
};

/* the commanded motors that are not dead: torque and draw (D-Ag) */
static uint32_t stix_att_live_motors(const StixAttState *a)
{
    return a->move_motors &
           (a->f_m1_dead ? ~STIX_ATT_M1 : ~0u) &
           (a->f_m2_dead ? ~STIX_ATT_M2 : ~0u);
}

static const char *stix_att_fault_id(int i)
{
    return stix_att_fault_bool[i].id;
}

/* ---- the lazy mechanism (D-Ac) ---- */

/*
 * The shaft position at @now; the caller has committed any end at or
 * before it. While a move runs, pos_ppm holds the position the move
 * started from, so interpolate; an unpowered or motorless command
 * moves nothing, and neither do dead motors nor a jam (A-3).
 */
static uint32_t stix_att_pos_at(StixAttState *a, int64_t now)
{
    int64_t t, pos;

    if (!a->moving) {
        return a->pos_ppm;
    }
    if (!a->move_powered || a->f_jammed || stix_att_live_motors(a) == 0) {
        return a->pos_ppm;
    }
    t = (now - a->move_t0_ns) / (int64_t)a->cfg.travel_ms;
    if (t < 0) {
        t = 0;
    }
    pos = (int64_t)a->pos_ppm + (a->dir_in ? t : -t);
    pos = MAX(0, MIN((int64_t)STIX_ATT_PPM, pos));
    return (uint32_t)pos;
}

/*
 * Commit any move end at or before @now. Pure in the PL-1/PL-2 sense:
 * the same now twice changes nothing. trace_ok says the caller is an
 * MMIO access; a getter passes false and stays silent (D-Ai).
 */
static void stix_att_advance(StixAttState *a, int64_t now, bool trace_ok)
{
    if (!a->moving || now < a->end_ns) {
        return;
    }
    a->pos_ppm = a->end_pos;
    a->moving = false;
    a->motors_used = 0;
    a->timeout = a->end_timeout;
    a->op_status = a->end_finishes;
    a->last_end = a->end_reason;
    a->last_end_ns = a->end_ns;
    if (trace_ok) {
        trace_stixatt_end(stix_att_end_name[a->end_reason], a->end_ns,
                          a->pos_ppm);
    }
}

/*
 * Solve the running move's end from the current state (D-Ae): the
 * presented target switch plus the debounce — the stuck layer
 * included, so a stuck switch also changes when the motor stops —
 * else the drive window with TIMEOUT = 1; the override runs the full
 * window and ends without TIMEOUT. pos_ppm and move_t0_ns must be
 * current (a command or a fault boundary has folded them); the ramp
 * end stays anchored at the command (ramp_end_ns): a mid-move inject
 * never extends the drive past it, so a target end whose debounce no
 * longer fits inside the window ends at the ramp end (TIMEOUT).
 */
static void stix_att_move_resolve(StixAttState *a, int64_t now)
{
    int64_t debounce_ns = (int64_t)a->cfg.debounce_ms * STIX_ATT_MS_NS;
    int64_t dist, t_reach;
    bool can_move, stuck_closed, stuck_open;

    /* the presented target switch: AB for open, BC for close */
    stuck_closed = a->dir_in ? a->f_bc_closed : a->f_ab_closed;
    stuck_open = a->dir_in ? a->f_bc_open : a->f_ab_open;
    can_move = a->move_powered && !a->f_jammed &&
               stix_att_live_motors(a) != 0;
    dist = a->dir_in ? (int64_t)STIX_ATT_PPM - a->pos_ppm : (int64_t)a->pos_ppm;
    t_reach = now + dist * (int64_t)a->cfg.travel_ms;

    if (a->move_override) {
        a->end_ns = a->ramp_end_ns;
        a->end_reason = STIX_ATT_END_OVERRIDE;
        a->end_timeout = false;
        a->end_finishes = true;
    } else if ((stuck_closed || (dist == 0 && !stuck_open)) &&
               now + debounce_ns <= a->ramp_end_ns) {
        /*
         * the presented target switch reads actuated now (stuck
         * closed, or the healthy switch with the truth already on it):
         * the drive runs one debounce and stops, so the truth ends
         * that much off the end
         */
        a->end_ns = now + debounce_ns;
        a->end_reason = STIX_ATT_END_TARGET;
        a->end_timeout = false;
        a->end_finishes = true;
    } else if (can_move && !stuck_open &&
               t_reach + debounce_ns <= a->ramp_end_ns) {
        a->end_ns = t_reach + debounce_ns;
        a->end_reason = STIX_ATT_END_TARGET;
        a->end_timeout = false;
        a->end_finishes = true;
    } else {
        /*
         * the drive window ends first: LV off, dead motors, a
         * stuck-open target, the truth cannot arrive inside the
         * window, or the target's debounce no longer fits in it
         */
        a->end_ns = a->ramp_end_ns;
        a->end_reason = STIX_ATT_END_TIMEOUT;
        a->end_timeout = true;
        a->end_finishes = a->cfg.timeout_finishes;
    }
    a->end_pos = stix_att_pos_at(a, a->end_ns);
}

/*
 * Start (or restart, D-Ae) a move toward @dir_in with @motors, from
 * the position at @now. Without LV the shaft only moves if it already
 * sits on the target switch.
 */
static void stix_att_move_start(StixAttState *a, int64_t now, bool dir_in,
                                uint32_t motors)
{
    a->pos_ppm = stix_att_pos_at(a, now);   /* a restart continues here */
    a->moving = true;
    a->dir_in = dir_in;
    a->move_t0_ns = now;
    a->move_motors = motors;
    a->move_powered = (a->reg->hkCmdSta >> STIXCTRL_HKCMDSTA_LVEN) &
                      STIXCTRL_HKCMDSTA_LVEN_MASK;
    a->move_override = a->override;
    a->ramp_end_ns = now + (int64_t)a->cfg.ramp_ms * STIX_ATT_MS_NS;

    a->op_status = false;                   /* moving */
    a->timeout = false;
    a->motors_used = motors;                /* the drive flag */
    a->moves++;

    stix_att_move_resolve(a, now);
}

/*
 * A mechanism fault changed at @now (D-Ag, piecewise): fold the
 * position at the boundary under the OLD faults before the caller
 * stores the new value, then re-solve the running move from now after
 * the store. Exact: the end time is unchanged unless the fault moves
 * it.
 */
static void stix_att_fault_fold(StixAttState *a, int64_t now)
{
    if (a->moving) {
        a->pos_ppm = stix_att_pos_at(a, now);
        a->move_t0_ns = now;
    }
}

static void stix_att_fault_resolve(StixAttState *a, int64_t now)
{
    if (a->moving) {
        stix_att_move_resolve(a, now);
    }
}

/* 6 (emergency) stops now: OPERATIONSTATUS 1, the flags clear (D-Ae) */
static void stix_att_emergency(StixAttState *a, int64_t now)
{
    if (a->moving) {
        a->pos_ppm = stix_att_pos_at(a, now);
        a->moving = false;
        a->last_end = STIX_ATT_END_EMERGENCY;
        a->last_end_ns = now;
        trace_stixatt_end(stix_att_end_name[STIX_ATT_END_EMERGENCY], now,
                          a->pos_ppm);
    }
    a->op_status = true;
    a->timeout = false;
    a->motors_used = 0;
}

/* ---- the register (0x5C) ---- */

/* the truth switches (D-Ad): AB actuates at OUT, BC at IN */
static bool stix_att_switch_ab(StixAttState *a, uint32_t pos)
{
    return pos == 0;                /* OUT actuates AB (D-Ad) */
}

static bool stix_att_switch_bc(StixAttState *a, uint32_t pos)
{
    return pos == STIX_ATT_PPM;     /* IN actuates BC */
}

/* what the FPGA and the FSW read: the stuck layer wins (A-3, D-Ag) */
static bool stix_att_switch_ab_p(StixAttState *a, uint32_t pos)
{
    if (a->f_ab_closed) {
        return true;
    }
    if (a->f_ab_open) {
        return false;
    }
    return stix_att_switch_ab(a, pos);
}

static bool stix_att_switch_bc_p(StixAttState *a, uint32_t pos)
{
    if (a->f_bc_closed) {
        return true;
    }
    if (a->f_bc_open) {
        return false;
    }
    return stix_att_switch_bc(a, pos);
}

uint32_t stix_att_cmdsta(StixAttState *a)
{
    uint32_t pos = stix_att_pos_at(a, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    uint32_t value;

    value = a->cmd |
            ((a->override ? 1u : 0u) << STIXCTRL_ATTNCMDSTA_ERRORMASK) |
            ((a->timeout ? 1u : 0u) << STIXCTRL_ATTNCMDSTA_TIMEOUT) |
            (((a->motors_used & STIX_ATT_M1) ? 1u : 0u)
             << STIXCTRL_ATTNCMDSTA_MOTOR1USED) |
            (((a->motors_used & STIX_ATT_M2) ? 1u : 0u)
             << STIXCTRL_ATTNCMDSTA_MOTOR2USED) |
            ((a->op_status ? 1u : 0u)
             << STIXCTRL_ATTNCMDSTA_OPERATIONSTATUS) |
            ((stix_att_switch_bc_p(a, pos) ? 1u : 0u)
             << STIXCTRL_ATTNCMDSTA_POSITIONBC) |
            ((stix_att_switch_ab_p(a, pos) ? 1u : 0u)
             << STIXCTRL_ATTNCMDSTA_POSITIONAB);
    /* FAILFLAG (bit 11) sets only through forcing (A-later has the fault) */

    /*
     * A-3 (D-Ag): the forcing masks apply to what a read returns,
     * after everything else. A bit in both masks reads 0 (the clear
     * wins). They touch bits 11:5 only, so the command readback and
     * ERRORMASK are untouched.
     */
    return (value | a->force_set) & ~a->force_clear;
}

bool stix_att_owns(StixAttState *a, hwaddr addr)
{
    return a->cfg.model && addr == 0x5C;
}

uint32_t stix_att_read(StixAttState *a, hwaddr addr)
{
    stix_att_advance(a, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), true);
    a->fsw_accesses++;
    return stix_att_cmdsta(a);
}

void stix_att_write(StixAttState *a, hwaddr addr, uint32_t value)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    stix_att_advance(a, now, true);
    a->fsw_accesses++;

    /* bits 4:0 only; the status bits are ignored on write (D-Ae) */
    a->cmd = value & STIXCTRL_ATTNCMDSTA_COMMAND_MASK;
    a->override = (value >> STIXCTRL_ATTNCMDSTA_ERRORMASK) &
                  STIXCTRL_ATTNCMDSTA_ERRORMASK_MASK;
    trace_stixatt_cmd(a->cmd, a->override, stix_att_pos_at(a, now));

    switch (a->cmd) {
    case STIXCTRL_ATTNCMDSTA_COMMAND_V_EMERGENCYSHUTDOWN:
        stix_att_emergency(a, now);
        break;
    case STIXCTRL_ATTNCMDSTA_COMMAND_V_OPENMOTOR1:
        stix_att_move_start(a, now, false, STIX_ATT_M1);
        break;
    case STIXCTRL_ATTNCMDSTA_COMMAND_V_OPENMOTOR2:
        stix_att_move_start(a, now, false, STIX_ATT_M2);
        break;
    case STIXCTRL_ATTNCMDSTA_COMMAND_V_OPENBOTHMOTORS:
        stix_att_move_start(a, now, false, STIX_ATT_M1 | STIX_ATT_M2);
        break;
    case STIXCTRL_ATTNCMDSTA_COMMAND_V_CLOSEMOTOR1:
        stix_att_move_start(a, now, true, STIX_ATT_M1);
        break;
    case STIXCTRL_ATTNCMDSTA_COMMAND_V_CLOSEMOTOR2:
        stix_att_move_start(a, now, true, STIX_ATT_M2);
        break;
    case STIXCTRL_ATTNCMDSTA_COMMAND_V_CLOSEBOTHMOTORS:
        stix_att_move_start(a, now, true, STIX_ATT_M1 | STIX_ATT_M2);
        break;
    default:
        /* stored for readback, no motion (D-Ae) */
        break;
    }
}

/* ---- the ATT_C current (hkAdc ch 8, A-2 = D-Af) ---- */

/*
 * Raw ADC counts [A] (Q3 left the real currents unknown): 20 idle; each
 * powered, live, driven motor adds 100 while the shaft moves and 300
 * while it is driven but not moving — stalled on its target end stop,
 * or against a jam or block point (A-3). A dead motor draws nothing
 * (D-Ag). 120/220 stay under 0x100, so the FSW sums them
 * (AttCtrl.c:344), and inside the PFM soft limits; the 320/620 stalls
 * leave the sum and stay below the 751 HW-protection threshold (no
 * trip, D-P4d).
 */
#define STIX_ATT_C_IDLE  20u
#define STIX_ATT_C_MOVE  100u
#define STIX_ATT_C_STALL 300u
#define STIX_ATT_C_MAX   4095u    /* the 12-bit ADC range for the clamp */

/*
 * The truth for stixctrl_hk_truth() (PL-2a's split). Advances the lazy
 * state like a getter and never traces (D-Ai): the 2-20 Hz hw-state
 * poll runs through the same code, and a move end committed here is
 * traced with its computed event time by the next 0x5C access.
 */
uint32_t stix_att_att_c_truth(StixAttState *a)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t pos, live, add, c = STIX_ATT_C_IDLE;

    stix_att_advance(a, now, false);
    if (!a->moving || !a->move_powered) {
        return c;               /* no powered motor is driven */
    }
    live = stix_att_live_motors(a);
    if (live == 0) {
        return c;               /* only dead motors are commanded */
    }
    pos = stix_att_pos_at(a, now);
    add = (a->f_jammed ||
           pos == (a->dir_in ? STIX_ATT_PPM : 0u)) ? STIX_ATT_C_STALL
                                                   : STIX_ATT_C_MOVE;
    if (live & STIX_ATT_M1) {
        c += add;
    }
    if (live & STIX_ATT_M2) {
        c += add;
    }
    return c;
}

/*
 * What the FSW converts: the truth through the current.* faults
 * (A-3, D-Ag), before the adc-inject[8] forcing, which the
 * controller's split applies first. clamp(truth + offset).
 */
uint32_t stix_att_att_c_presented(StixAttState *a)
{
    uint32_t truth = stix_att_att_c_truth(a);
    int32_t c;

    if (a->f_c_zero) {
        return 0;               /* "motion with no current" */
    }
    c = (int32_t)truth + a->f_c_offset;
    return (uint32_t)MAX(0, MIN((int32_t)STIX_ATT_C_MAX, c));
}

/* ---- lifecycle ---- */

bool stix_att_realize(StixAttState *a, STIXCTRL *owner,
                      const StixAttConfig *cfg,
                      stixCtrlRegs_s *reg, Error **errp)
{
    a->owner = owner;
    a->cfg = *cfg;
    a->reg = reg;
    if (a->cfg.travel_ms == 0 || a->cfg.ramp_ms == 0) {
        error_setg(errp, "grlib-stixctrl: att-travel-ms and att-ramp-ms "
                   "must be above 0");
        return false;
    }
    /* att-initial: out (the default, the AX stub's isAB) or in (D-Ad) */
    if (!a->cfg.initial || !strcmp(a->cfg.initial, "out")) {
        a->pos_ppm = 0;
    } else if (!strcmp(a->cfg.initial, "in")) {
        a->pos_ppm = STIX_ATT_PPM;
    } else {
        error_setg(errp, "grlib-stixctrl: att-initial '%s' is not 'out' "
                   "or 'in'", a->cfg.initial);
        return false;
    }
    return true;
}

void stix_att_unrealize(StixAttState *a)
{
    /* nothing to free: the lazy model owns no timer, BH or IRQ (D-Ai) */
}

/*
 * Device reset (system or watchdog): a running move aborts at the
 * reset instant, the registers go idle (OPERATIONSTATUS 1, the rest 0)
 * and the position is kept (D-Ae). The faults survive the reset — a
 * broken switch stays broken over an IDPU reboot (D-Ag) — while the
 * forcing masks clear, like adc-inject. No trace: the unit is silent
 * at reset (D-Ai).
 */
void stix_att_reset(StixAttState *a)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!a->cfg.model) {
        return;
    }
    if (a->moving) {
        a->pos_ppm = stix_att_pos_at(a, now);
        a->moving = false;
        a->last_end = STIX_ATT_END_RESET;
        a->last_end_ns = now;
    }
    a->cmd = 0;
    a->override = false;
    a->op_status = true;
    a->timeout = false;
    a->motors_used = 0;
    a->force_set = 0;
    a->force_clear = 0;
}

/* ---- the QOM surface (D-Ah): read-only truth/presented ---- */

/* getters advance the lazy state but never trace (D-Ai) */
static void stix_att_get_advance(StixAttState *a)
{
    stix_att_advance(a, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), false);
}

static const char *stix_att_pos_name(StixAttState *a)
{
    uint32_t pos = stix_att_pos_at(a, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));

    /*
     * "moving" only while the shaft really travels: not for a command
     * that cannot move it (LV off, no live motor, a jam), nor in the
     * debounce after it reached the end stop
     */
    if (a->moving && a->move_powered && stix_att_live_motors(a) != 0 &&
        !a->f_jammed && pos != (a->dir_in ? STIX_ATT_PPM : 0)) {
        return a->dir_in ? "moving-in" : "moving-out";
    }
    if (pos == 0) {
        return "out";
    }
    if (pos == STIX_ATT_PPM) {
        return "in";
    }
    return "between";
}

static void stix_att_pos_get(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    char *value;

    stix_att_get_advance(a);
    value = g_strdup(stix_att_pos_name(a));
    visit_type_str(v, name, &value, errp);
    g_free(value);
}

static void stix_att_pos_ppm_get(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    uint32_t value;

    stix_att_get_advance(a);
    value = stix_att_pos_at(a, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    visit_type_uint32(v, name, &value, errp);
}

static void stix_att_ab_get(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    uint32_t pos;
    bool value;

    stix_att_get_advance(a);
    pos = stix_att_pos_at(a, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    value = stix_att_switch_ab(a, pos);
    visit_type_bool(v, name, &value, errp);
}

static void stix_att_bc_get(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    uint32_t pos;
    bool value;

    stix_att_get_advance(a);
    pos = stix_att_pos_at(a, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    value = stix_att_switch_bc(a, pos);
    visit_type_bool(v, name, &value, errp);
}

/* torque, not the flag: a powered, live, commanded motor while it runs */
static bool stix_att_driving(StixAttState *a, uint32_t motor)
{
    return a->moving && a->move_powered && (a->move_motors & motor) != 0 &&
           (stix_att_live_motors(a) & motor) != 0;
}

static void stix_att_m1_get(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    bool value;

    stix_att_get_advance(a);
    value = stix_att_driving(a, STIX_ATT_M1);
    visit_type_bool(v, name, &value, errp);
}

static void stix_att_m2_get(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    bool value;

    stix_att_get_advance(a);
    value = stix_att_driving(a, STIX_ATT_M2);
    visit_type_bool(v, name, &value, errp);
}

/* the mechanism's draw, through the controller's truth half (D-Af) */
static void stix_att_truth_c_get(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    uint32_t value = stixctrl_hk_att_c_truth(a->owner);

    visit_type_uint32(v, name, &value, errp);
}

/* what the FSW converts, adc-inject[8] forcing included (D-Ah) */
static void stix_att_presented_c_get(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    uint32_t value = stixctrl_hk_att_c_presented(a->owner);

    visit_type_uint32(v, name, &value, errp);
}

static void stix_att_moves_get(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    uint64_t value = a->moves;

    visit_type_uint64(v, name, &value, errp);
}

static void stix_att_last_end_get(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    char *value;

    stix_att_get_advance(a);
    value = g_strdup(stix_att_end_name[a->last_end]);
    visit_type_str(v, name, &value, errp);
    g_free(value);
}

static void stix_att_last_end_ns_get(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    int64_t value;

    stix_att_get_advance(a);
    value = a->last_end_ns;
    visit_type_int64(v, name, &value, errp);
}

/* what the FPGA and the FSW read: the stuck layer included (A-3) */
static void stix_att_p_ab_get(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    uint32_t pos;
    bool value;

    stix_att_get_advance(a);
    pos = stix_att_pos_at(a, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    value = stix_att_switch_ab_p(a, pos);
    visit_type_bool(v, name, &value, errp);
}

static void stix_att_p_bc_get(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    uint32_t pos;
    bool value;

    stix_att_get_advance(a);
    pos = stix_att_pos_at(a, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    value = stix_att_switch_bc_p(a, pos);
    visit_type_bool(v, name, &value, errp);
}

static void stix_att_cmdsta_get(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    uint32_t value;

    stix_att_get_advance(a);
    /* att-model=off: what the guest reads, the plain storage */
    value = a->cfg.model ? stix_att_cmdsta(a) : a->reg->attnCmdSta;
    visit_type_uint32(v, name, &value, errp);
}

/* guest 0x5C accesses since realize; never cleared on reset (D-Ah) */
static void stix_att_fsw_accesses_get(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    uint64_t value = a->fsw_accesses;

    visit_type_uint64(v, name, &value, errp);
}

/* ---- the A-3 QOM surface: faults, verbs, forcing (D-Ag) ---- */

static bool stix_att_fault_bool_val(StixAttState *a, int i)
{
    return *(bool *)((char *)a + stix_att_fault_bool[i].off);
}

/*
 * faults-active: "id=value,..." for every fault off its inactive
 * value, catalogue order ("" when none). bools read 1, the offset its
 * raw value.
 */
static char *stix_att_faults_active(StixAttState *a)
{
    GString *s = g_string_new(NULL);
    int i;

    for (i = 0; i < STIX_ATT_F_BOOL__MAX; i++) {
        if (!stix_att_fault_bool_val(a, i)) {
            continue;
        }
        g_string_append_printf(s, "%s%s=1", s->len ? "," : "",
                               stix_att_fault_id(i));
    }
    if (a->f_c_offset != 0) {
        g_string_append_printf(s, "%s%s=%d", s->len ? "," : "",
                               STIX_ATT_F_C_OFFSET_ID, a->f_c_offset);
    }
    return g_string_free(s, false);
}

static void stix_att_faults_active_get(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    char *value;

    stix_att_get_advance(a);
    value = stix_att_faults_active(a);
    visit_type_str(v, name, &value, errp);
    g_free(value);
}

/*
 * One fault store (D-Ag): advance silently to now (a QOM set is not an
 * MMIO access, so a committed end is traced by the next 0x5C access,
 * D-Ai), fold the position at the boundary under the old faults, store,
 * trace, re-solve a running move from now (piecewise) when the row
 * resolves (the table above).
 */
static void stix_att_fault_store_bool(StixAttState *a, int i, bool value)
{
    bool *field = (bool *)((char *)a + stix_att_fault_bool[i].off);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool resolves = stix_att_fault_bool[i].resolves;

    stix_att_advance(a, now, false);
    if (resolves) {
        stix_att_fault_fold(a, now);
    }
    *field = value;
    trace_stixatt_fault(stix_att_fault_id(i), value);
    if (resolves) {
        stix_att_fault_resolve(a, now);
    }
}

static void stix_att_fault_bool_get(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    bool value = stix_att_fault_bool_val(a, GPOINTER_TO_UINT(opaque));

    visit_type_bool(v, name, &value, errp);
}

static void stix_att_fault_bool_set(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    bool value;

    if (!visit_type_bool(v, name, &value, errp)) {
        return;
    }
    if (!a->cfg.model) {
        error_setg(errp, "%s: the attenuator model is off (att-model=off)",
                   stix_att_fault_id(GPOINTER_TO_UINT(opaque)));
        return;
    }
    stix_att_fault_store_bool(a, GPOINTER_TO_UINT(opaque), value);
}

static void stix_att_c_offset_get(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    int32_t value = a->f_c_offset;

    visit_type_int32(v, name, &value, errp);
}

static void stix_att_c_offset_set(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    int32_t value;

    if (!visit_type_int32(v, name, &value, errp)) {
        return;
    }
    if (!a->cfg.model) {
        error_setg(errp, "%s: the attenuator model is off (att-model=off)",
                   STIX_ATT_F_C_OFFSET_ID);
        return;
    }
    if (value < -4095 || value > 4095) {
        error_setg(errp, "%s: %d is outside -4095..4095 raw counts", name,
                   value);
        return;
    }
    a->f_c_offset = value;
    trace_stixatt_fault(STIX_ATT_F_C_OFFSET_ID, value);
}

/* force.cmdsta-set / -clear: bits 11:5 only, else rejected (D-Ag) */
#define STIX_ATT_FORCE_MASK 0xFE0u

static void stix_att_force_get(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    uint32_t value = GPOINTER_TO_UINT(opaque) ? a->force_clear : a->force_set;

    visit_type_uint32(v, name, &value, errp);
}

static void stix_att_force_set(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    uint32_t *field = GPOINTER_TO_UINT(opaque) ? &a->force_clear
                                               : &a->force_set;
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (!a->cfg.model) {
        error_setg(errp, "%s: the attenuator model is off (att-model=off)",
                   name);
        return;
    }
    if (value & ~STIX_ATT_FORCE_MASK) {
        error_setg(errp, "%s: 0x%x has bits outside 11:5", name, value);
        return;
    }
    *field = value;
    trace_stixatt_fault(name, value);
}

/*
 * fault-clear: an ID or "all" back to the inactive value. Unknown IDs
 * are rejected; the forcing masks are not touched (they have their own
 * properties).
 */
static void stix_att_fault_clear_store_bool(StixAttState *a, int i)
{
    stix_att_fault_store_bool(a, i, false);
}

static void stix_att_fault_clear_set(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    StixAttState *a = STIX_ATT(obj);
    char *id;
    int i;

    if (!visit_type_str(v, name, &id, errp)) {
        return;
    }
    if (!a->cfg.model) {
        error_setg(errp, "%s: the attenuator model is off (att-model=off)",
                   name);
        goto out;
    }
    if (!strcmp(id, "all")) {
        for (i = 0; i < STIX_ATT_F_BOOL__MAX; i++) {
            stix_att_fault_clear_store_bool(a, i);
        }
        a->f_c_offset = 0;
        trace_stixatt_fault(STIX_ATT_F_C_OFFSET_ID, 0);
        goto out;
    }
    for (i = 0; i < STIX_ATT_F_BOOL__MAX; i++) {
        if (!strcmp(id, stix_att_fault_id(i))) {
            stix_att_fault_clear_store_bool(a, i);
            goto out;
        }
    }
    if (!strcmp(id, STIX_ATT_F_C_OFFSET_ID)) {
        a->f_c_offset = 0;
        trace_stixatt_fault(STIX_ATT_F_C_OFFSET_ID, 0);
        goto out;
    }
    error_setg(errp, "%s: unknown fault id '%s'", name, id);
out:
    g_free(id);
}

/*
 * The hw-state "attenuator" object (D-Ah), sampled by the controller's
 * stixctrl_get_hw_state: truth, presented, fsw_accesses and
 * faults_active. Pure like the getters: the lazy advance is
 * idempotent and nothing traces (D-Ai). The caller owns the QDict.
 */
QDict *stix_att_hw_state(StixAttState *a)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t pos;
    QDict *d = qdict_new(), *t = qdict_new(), *p = qdict_new();
    g_autofree char *active = NULL;

    stix_att_advance(a, now, false);
    pos = stix_att_pos_at(a, now);

    qdict_put_str(t, "position", stix_att_pos_name(a));
    qdict_put_int(t, "position_ppm", pos);
    qdict_put_bool(t, "sensor_ab", stix_att_switch_ab(a, pos));
    qdict_put_bool(t, "sensor_bc", stix_att_switch_bc(a, pos));
    qdict_put_bool(t, "motor1_driving", stix_att_driving(a, STIX_ATT_M1));
    qdict_put_bool(t, "motor2_driving", stix_att_driving(a, STIX_ATT_M2));
    qdict_put_int(t, "att_c", stixctrl_hk_att_c_truth(a->owner));
    qdict_put_int(t, "moves", a->moves);
    qdict_put_str(t, "last_end", stix_att_end_name[a->last_end]);
    qdict_put_int(t, "last_end_ns", a->last_end_ns);
    qdict_put(d, "truth", t);

    qdict_put_bool(p, "sensor_ab", stix_att_switch_ab_p(a, pos));
    qdict_put_bool(p, "sensor_bc", stix_att_switch_bc_p(a, pos));
    qdict_put_int(p, "cmdsta", stix_att_cmdsta(a));
    qdict_put_int(p, "att_c", stixctrl_hk_att_c_presented(a->owner));
    qdict_put(d, "presented", p);

    qdict_put_int(d, "fsw_accesses", a->fsw_accesses);
    active = stix_att_faults_active(a);
    qdict_put_str(d, "faults_active", active);
    return d;
}

static void stix_att_class_init(ObjectClass *klass, const void *data)
{
    object_class_property_add(klass, "truth.position", "string",
                              &str_type_info, stix_att_pos_get, NULL, NULL,
                              NULL);
    object_class_property_set_description(klass, "truth.position",
        "mechanism position: out | in | moving-in | moving-out | between");
    object_class_property_add(klass, "truth.position-ppm", "uint32",
                              &uint32_type_info, stix_att_pos_ppm_get, NULL,
                              NULL, NULL);
    object_class_property_set_description(klass, "truth.position-ppm",
        "shaft position, 0 = OUT .. 1000000 = IN (read-only)");
    object_class_property_add(klass, "truth.sensor-ab", "bool",
                              &bool_type_info, stix_att_ab_get, NULL, NULL,
                              NULL);
    object_class_property_set_description(klass, "truth.sensor-ab",
        "position switch AB, actuated at OUT (read-only)");
    object_class_property_add(klass, "truth.sensor-bc", "bool",
                              &bool_type_info, stix_att_bc_get, NULL, NULL,
                              NULL);
    object_class_property_set_description(klass, "truth.sensor-bc",
        "position switch BC, actuated at IN (read-only)");
    object_class_property_add(klass, "truth.motor1-driving", "bool",
                              &bool_type_info, stix_att_m1_get, NULL, NULL,
                              NULL);
    object_class_property_set_description(klass, "truth.motor1-driving",
        "M1 produces torque (read-only)");
    object_class_property_add(klass, "truth.motor2-driving", "bool",
                              &bool_type_info, stix_att_m2_get, NULL, NULL,
                              NULL);
    object_class_property_set_description(klass, "truth.motor2-driving",
        "M2 produces torque (read-only)");
    object_class_property_add(klass, "truth.att-c", "uint32",
                              &uint32_type_info, stix_att_truth_c_get, NULL,
                              NULL, NULL);
    object_class_property_set_description(klass, "truth.att-c",
        "ATT_C (hkAdc ch 8) the mechanism draws, raw counts (read-only)");
    object_class_property_add(klass, "presented.att-c", "uint32",
                              &uint32_type_info, stix_att_presented_c_get,
                              NULL, NULL, NULL);
    object_class_property_set_description(klass, "presented.att-c",
        "the ATT_C the FSW converts, adc-inject[8] included (read-only)");
    object_class_property_add(klass, "truth.moves", "uint64",
                              &uint64_type_info, stix_att_moves_get, NULL,
                              NULL, NULL);
    object_class_property_set_description(klass, "truth.moves",
        "moves started since realize, restarts included (read-only)");
    object_class_property_add(klass, "truth.last-end", "string",
                              &str_type_info, stix_att_last_end_get, NULL,
                              NULL, NULL);
    object_class_property_set_description(klass, "truth.last-end",
        "none | target | timeout | override | emergency | fail | reset");
    object_class_property_add(klass, "truth.last-end-ns", "int64",
                              &int64_type_info, stix_att_last_end_ns_get,
                              NULL, NULL, NULL);
    object_class_property_set_description(klass, "truth.last-end-ns",
        "virtual-clock ns of the last move end (read-only)");
    object_class_property_add(klass, "presented.sensor-ab", "bool",
                              &bool_type_info, stix_att_p_ab_get, NULL, NULL,
                              NULL);
    object_class_property_set_description(klass, "presented.sensor-ab",
        "the switch as the FPGA and FSW see it (read-only)");
    object_class_property_add(klass, "presented.sensor-bc", "bool",
                              &bool_type_info, stix_att_p_bc_get, NULL, NULL,
                              NULL);
    object_class_property_set_description(klass, "presented.sensor-bc",
        "the switch as the FPGA and FSW see it (read-only)");
    object_class_property_add(klass, "presented.cmdsta", "uint32",
                              &uint32_type_info, stix_att_cmdsta_get, NULL,
                              NULL, NULL);
    object_class_property_set_description(klass, "presented.cmdsta",
        "what a 0x5C read returns now, with no side effects (read-only)");
    object_class_property_add(klass, "fsw-accesses", "uint64",
                              &uint64_type_info, stix_att_fsw_accesses_get,
                              NULL, NULL, NULL);
    object_class_property_set_description(klass, "fsw-accesses",
        "guest 0x5C accesses since realize; 0 on an AX ELF (read-only)");

    /* A-3 (D-Ag): the faults, the verbs and the forcing */
    for (int i = 0; i < STIX_ATT_F_BOOL__MAX; i++) {
        object_class_property_add(klass, stix_att_fault_id(i), "bool",
                                  &bool_type_info, stix_att_fault_bool_get,
                                  stix_att_fault_bool_set, NULL,
                                  GUINT_TO_POINTER(i));
        object_class_property_set_description(klass, stix_att_fault_id(i),
            "fault inject: persistent until cleared, across device resets");
    }
    object_class_property_add(klass, STIX_ATT_F_C_OFFSET_ID, "int32",
                              &int32_type_info, stix_att_c_offset_get,
                              stix_att_c_offset_set, NULL, NULL);
    object_class_property_set_description(klass, STIX_ATT_F_C_OFFSET_ID,
        "ATT_C offset, raw counts -4095..4095, 0 = off (persistent)");
    object_class_property_add(klass, "faults-active", "string",
                              &str_type_info, stix_att_faults_active_get,
                              NULL, NULL, NULL);
    object_class_property_set_description(klass, "faults-active",
        "the active faults as id=value,... (read-only)");
    object_class_property_add(klass, "fault-clear", "string",
                              &str_type_info, NULL,
                              stix_att_fault_clear_set, NULL, NULL);
    object_class_property_set_description(klass, "fault-clear",
        "clear one fault id, or 'all' (write-only)");
    object_class_property_add(klass, "force.cmdsta-set", "uint32",
                              &uint32_type_info, stix_att_force_get,
                              stix_att_force_set, NULL, GUINT_TO_POINTER(0));
    object_class_property_set_description(klass, "force.cmdsta-set",
        "bits 11:5 forced to 1 in the 0x5C readback; clears on reset");
    object_class_property_add(klass, "force.cmdsta-clear", "uint32",
                              &uint32_type_info, stix_att_force_get,
                              stix_att_force_set, NULL,
                              GUINT_TO_POINTER(1));
    object_class_property_set_description(klass, "force.cmdsta-clear",
        "bits 11:5 forced to 0 in the 0x5C readback; clears on reset");
}

static const TypeInfo stix_att_info = {
    .name = TYPE_STIX_ATT,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(StixAttState),
    .class_init = stix_att_class_init,
};

static void stix_att_register_types(void)
{
    type_register_static(&stix_att_info);
}

type_init(stix_att_register_types);
