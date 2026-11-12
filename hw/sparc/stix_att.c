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
 * Re-entrancy (D-Ai): the unit has no timer, BH, DMA or IRQ and never
 * calls address_space_* or a stixctrl MMIO handler, so the
 * a83fe9a3a5 guard rule does not apply. The 0x5C hook covers 0x5C
 * only; 0x60/0x64 stay plain storage, so the trace golden is
 * unchanged (D-Aa). The ATT_C current (hkAdc ch 8) follows the
 * mechanism (A-2 = D-Af); no faults (A-3).
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

/* ---- the lazy mechanism (D-Ac) ---- */

/*
 * The shaft position at @now; the caller has committed any end at or
 * before it. While a move runs, pos_ppm holds the position the move
 * started from, so interpolate; an unpowered or motorless command
 * moves nothing.
 */
static uint32_t stix_att_pos_at(StixAttState *a, int64_t now)
{
    int64_t t, pos;

    if (!a->moving) {
        return a->pos_ppm;
    }
    if (!a->move_powered || a->move_motors == 0) {
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
 * Start (or restart, D-Ae) a move toward @dir_in with @motors, from
 * the position at @now. The end is solved here: the presented target
 * switch plus the debounce (already-actuated included), else the drive
 * window with TIMEOUT = 1; the override runs the full window and ends
 * without TIMEOUT. Without LV the shaft only moves if it already sits
 * on the target switch.
 */
static void stix_att_move_start(StixAttState *a, int64_t now, bool dir_in,
                                uint32_t motors)
{
    int64_t ramp_ns = now + (int64_t)a->cfg.ramp_ms * STIX_ATT_MS_NS;
    int64_t debounce_ns = (int64_t)a->cfg.debounce_ms * STIX_ATT_MS_NS;
    int64_t dist, t_reach;
    bool can_move;

    a->pos_ppm = stix_att_pos_at(a, now);   /* a restart continues here */
    a->moving = true;
    a->dir_in = dir_in;
    a->move_t0_ns = now;
    a->move_motors = motors;
    a->move_powered = (a->reg->hkCmdSta >> STIXCTRL_HKCMDSTA_LVEN) &
                      STIXCTRL_HKCMDSTA_LVEN_MASK;
    a->move_override = a->override;

    can_move = a->move_powered && motors != 0;
    dist = dir_in ? (int64_t)STIX_ATT_PPM - a->pos_ppm : (int64_t)a->pos_ppm;
    t_reach = now + dist * (int64_t)a->cfg.travel_ms;

    a->op_status = false;                   /* moving */
    a->timeout = false;
    a->motors_used = motors;                /* the drive flag */
    a->moves++;

    if (a->move_override) {
        a->end_ns = ramp_ns;
        a->end_reason = STIX_ATT_END_OVERRIDE;
        a->end_timeout = false;
        a->end_finishes = true;
    } else if (dist == 0 || (can_move && t_reach + debounce_ns <= ramp_ns)) {
        a->end_ns = dist == 0 ? now + debounce_ns : t_reach + debounce_ns;
        a->end_reason = STIX_ATT_END_TARGET;
        a->end_timeout = false;
        a->end_finishes = true;
    } else {
        a->end_ns = ramp_ns;
        a->end_reason = STIX_ATT_END_TIMEOUT;
        a->end_timeout = true;
        a->end_finishes = a->cfg.timeout_finishes;
    }
    a->end_pos = stix_att_pos_at(a, a->end_ns);
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

/* the presented switches; the fault layer arrives with A-3 */
static bool stix_att_switch_ab(StixAttState *a, uint32_t pos)
{
    return pos == 0;                /* OUT actuates AB (D-Ad) */
}

static bool stix_att_switch_bc(StixAttState *a, uint32_t pos)
{
    return pos == STIX_ATT_PPM;     /* IN actuates BC */
}

uint32_t stix_att_cmdsta(StixAttState *a)
{
    uint32_t pos = stix_att_pos_at(a, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));

    return a->cmd |
           ((a->override ? 1u : 0u) << STIXCTRL_ATTNCMDSTA_ERRORMASK) |
           ((a->timeout ? 1u : 0u) << STIXCTRL_ATTNCMDSTA_TIMEOUT) |
           (((a->motors_used & STIX_ATT_M1) ? 1u : 0u)
            << STIXCTRL_ATTNCMDSTA_MOTOR1USED) |
           (((a->motors_used & STIX_ATT_M2) ? 1u : 0u)
            << STIXCTRL_ATTNCMDSTA_MOTOR2USED) |
           ((a->op_status ? 1u : 0u) << STIXCTRL_ATTNCMDSTA_OPERATIONSTATUS) |
           ((stix_att_switch_bc(a, pos) ? 1u : 0u)
            << STIXCTRL_ATTNCMDSTA_POSITIONBC) |
           ((stix_att_switch_ab(a, pos) ? 1u : 0u)
            << STIXCTRL_ATTNCMDSTA_POSITIONAB);
    /* FAILFLAG never sets before A-3 */
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
 * while it is driven but not moving — stalled on its target end stop
 * (a block point or a jam would read the same; both arrive with A-3).
 * 120/220 stay under 0x100, so the FSW sums them (AttCtrl.c:344), and
 * inside the PFM soft limits; the 320/620 stalls leave the sum and
 * stay below the 751 HW-protection threshold (no trip, D-P4d).
 */
#define STIX_ATT_C_IDLE  20u
#define STIX_ATT_C_MOVE  100u
#define STIX_ATT_C_STALL 300u

/*
 * The truth for stixctrl_hk_truth() (PL-2a's split). Advances the lazy
 * state like a getter and never traces (D-Ai): the 2-20 Hz hw-state
 * poll runs through the same code, and a move end committed here is
 * traced with its computed event time by the next 0x5C access.
 */
uint32_t stix_att_att_c_truth(StixAttState *a)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t pos, add, c = STIX_ATT_C_IDLE;

    stix_att_advance(a, now, false);
    if (!a->moving || !a->move_powered) {
        return c;               /* no powered motor is driven */
    }
    pos = stix_att_pos_at(a, now);
    add = pos == (a->dir_in ? STIX_ATT_PPM : 0u) ? STIX_ATT_C_STALL
                                                 : STIX_ATT_C_MOVE;
    if (a->move_motors & STIX_ATT_M1) {
        c += add;
    }
    if (a->move_motors & STIX_ATT_M2) {
        c += add;
    }
    return c;
}

/* what the FSW converts; the current.* fault layer arrives with A-3 */
uint32_t stix_att_att_c_presented(StixAttState *a)
{
    return stix_att_att_c_truth(a);
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
 * and the position is kept (D-Ae). No trace: the unit is silent at
 * reset (D-Ai).
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
     * that cannot move it (LV off, no motor), nor in the debounce after
     * it reached the end stop
     */
    if (a->moving && a->move_powered && a->move_motors != 0 &&
        pos != (a->dir_in ? STIX_ATT_PPM : 0)) {
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

/* torque, not the flag: a powered, commanded motor while the move runs */
static bool stix_att_driving(StixAttState *a, uint32_t motor)
{
    return a->moving && a->move_powered && (a->move_motors & motor) != 0;
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

/* presented == truth until the A-3 fault layer exists */
static void stix_att_p_ab_get(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    stix_att_ab_get(obj, v, name, opaque, errp);
}

static void stix_att_p_bc_get(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    stix_att_bc_get(obj, v, name, opaque, errp);
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
