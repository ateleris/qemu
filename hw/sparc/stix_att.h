/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * STIX IDPU attenuator model (attenuator plan A-1/A-2, D-Aa..Af, Ah,
 * Ai; the fault layer is A-3, D-Ag): the mechanism (one shaft, two
 * motors, the AB/BC end switches), the AttnCmdSta controller behind
 * CTRL APB offset 0x5C, and the ATT_C current profile (hkAdc ch 8), at
 * /machine/stixctrl/attenuator. Internal to grlib_stixctrl.c; the
 * truth/presented properties, the faults and the verbs are added to
 * the child object's class by stix_att.c.
 */

#ifndef HW_SPARC_STIX_ATT_H
#define HW_SPARC_STIX_ATT_H

#include "qapi/error.h"
#include "qom/object.h"
#include "system/memory.h"
#include "qobject/qdict.h"
#include "hw/sparc/grlib_stix.h"
#include "StixIdpu.h"

#define TYPE_STIX_ATT "stix-att"

OBJECT_DECLARE_SIMPLE_TYPE(StixAttState, STIX_ATT)

/* the shaft travel: 0 = OUT (uncovered), 1000000 = IN (covered) */
#define STIX_ATT_PPM 1000000u

/* internal motor mask; MOTOR1USED/MOTOR2USED carry the same bits */
#define STIX_ATT_M1 1u
#define STIX_ATT_M2 2u

/* how the last move ended (truth.last-end, the stixatt_end reason) */
typedef enum StixAttEnd {
    STIX_ATT_END_NONE,
    STIX_ATT_END_TARGET,
    STIX_ATT_END_TIMEOUT,
    STIX_ATT_END_OVERRIDE,
    STIX_ATT_END_EMERGENCY,
    STIX_ATT_END_FAIL,          /* A-3 */
    STIX_ATT_END_RESET,
    STIX_ATT_END__MAX,
} StixAttEnd;

/*
 * The qdev properties on the controller (D-Ah). They live in the
 * controller's own memory, not in the child object: the class defaults
 * are written before instance_init, and object_initialize_child() zeroes
 * the child's memory there.
 */
typedef struct StixAttConfig {
    bool model;                 /* att-model: off keeps 0x5C plain storage */
    char *initial;              /* att-initial: out | in (owned by qdev) */
    uint32_t travel_ms;         /* att-travel-ms: full travel OUT <-> IN */
    uint32_t ramp_ms;           /* att-ramp-ms: the drive window */
    uint32_t debounce_ms;       /* att-debounce-ms: switch seen -> stop */
    bool timeout_finishes;      /* att-timeout-finishes (Q4) */
} StixAttConfig;

struct StixAttState {
    Object parent_obj;

    /* the qdev config, copied from the controller at realize (D-Ah) */
    StixAttConfig cfg;

    /* bound by stix_att_realize: LVEN is sampled from it per command */
    stixCtrlRegs_s *reg;

    /*
     * bound by stix_att_realize, like stix_det's owner: presented.att-c
     * reads the controller's ch 8 split (adc-inject[8], D-Ah)
     */
    STIXCTRL *owner;

    /* the mechanism (truth) */
    uint32_t pos_ppm;           /* survives a device reset (D-Ad) */
    bool moving;
    bool dir_in;                /* the shaft's target: IN or OUT */
    int64_t move_t0_ns;
    uint32_t move_motors;
    bool move_powered;          /* LVEN sampled at the command [A] */
    bool move_override;         /* ERRORMASK: the switches are ignored */
    int64_t ramp_end_ns;        /* the drive window, anchored at the cmd */
    /* the move's end, solved at the command (D-Ac) */
    int64_t end_ns;
    uint32_t end_pos;
    StixAttEnd end_reason;
    bool end_timeout;
    bool end_finishes;

    /* the controller view (presented, D-Ae) */
    uint32_t cmd;               /* bits 3:0 of the last write */
    bool override;              /* ERRORMASK of the last write */
    bool op_status;             /* OPERATIONSTATUS: 1 = finished/idle */
    bool timeout;
    uint32_t motors_used;

    /*
     * The A-3 fault layer (D-Ag): named failures, persistent until
     * cleared and across a device reset (a broken switch stays broken
     * over an IDPU reboot). f_* are the catalogue rows; force_set /
     * force_clear are the bits-11:5 read masks, which are cleared on
     * reset like the adc-inject forcing.
     */
    bool f_ab_open, f_ab_closed;    /* sensor_ab.stuck_open / stuck_closed */
    bool f_bc_open, f_bc_closed;    /* sensor_bc.stuck_open / stuck_closed */
    bool f_m1_dead, f_m2_dead;      /* motor1.dead / motor2.dead */
    bool f_jammed;                  /* mechanism.jammed */
    bool f_c_zero;                  /* current.zero */
    int32_t f_c_offset;             /* current.offset, raw ADC counts */
    uint32_t force_set, force_clear; /* force.cmdsta-set / -clear */

    /* bookkeeping */
    uint64_t moves;
    StixAttEnd last_end;
    int64_t last_end_ns;
    uint64_t fsw_accesses;      /* guest 0x5C accesses since realize */
};

bool stix_att_realize(StixAttState *a, STIXCTRL *owner,
                      const StixAttConfig *cfg,
                      stixCtrlRegs_s *reg, Error **errp);
void stix_att_unrealize(StixAttState *a);
void stix_att_reset(StixAttState *a);
/* true when the model owns the APB offset (att-model on, 0x5C only) */
bool stix_att_owns(StixAttState *a, hwaddr addr);
uint32_t stix_att_read(StixAttState *a, hwaddr addr);
void stix_att_write(StixAttState *a, hwaddr addr, uint32_t value);
/* the 0x5C readback as the FSW sees it now (no access counter) */
uint32_t stix_att_cmdsta(StixAttState *a);

/*
 * The ATT_C current (hkAdc ch 8, A-2 = D-Af): the mechanism's raw draw,
 * and what the FSW converts (== truth until the A-3 fault layer).
 * Called from the PL-2a split's two halves; both advance the lazy state
 * idempotently and never trace (D-Ai).
 */
uint32_t stix_att_att_c_truth(StixAttState *a);
uint32_t stix_att_att_c_presented(StixAttState *a);

/*
 * A-3 (D-Ah): the hw-state "attenuator" object for the controller's
 * stixctrl_get_hw_state(): truth, presented, fsw_accesses and
 * faults_active. Pure like the getters (no trace, the lazy advance is
 * idempotent); the caller owns the returned QDict.
 */
QDict *stix_att_hw_state(StixAttState *a);

#endif
