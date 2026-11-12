/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * STIX IDPU attenuator model (attenuator plan A-1, D-Aa..Ae, Ah, Ai):
 * the mechanism (one shaft, two motors, the AB/BC end switches) and the
 * AttnCmdSta controller behind CTRL APB offset 0x5C, at
 * /machine/stixctrl/attenuator. Internal to grlib_stixctrl.c; the
 * truth/presented properties are added to the child object's class by
 * stix_att.c. No faults (A-3) and no ATT_C profile (A-2).
 */

#ifndef HW_SPARC_STIX_ATT_H
#define HW_SPARC_STIX_ATT_H

#include "qapi/error.h"
#include "qom/object.h"
#include "system/memory.h"
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

    /* the mechanism (truth) */
    uint32_t pos_ppm;           /* survives a device reset (D-Ad) */
    bool moving;
    bool dir_in;                /* the shaft's target: IN or OUT */
    int64_t move_t0_ns;
    uint32_t move_motors;
    bool move_powered;          /* LVEN sampled at the command [A] */
    bool move_override;         /* ERRORMASK: the switches are ignored */
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

    /* bookkeeping */
    uint64_t moves;
    StixAttEnd last_end;
    int64_t last_end_ns;
    uint64_t fsw_accesses;      /* guest 0x5C accesses since realize */
};

bool stix_att_realize(StixAttState *a, const StixAttConfig *cfg,
                      stixCtrlRegs_s *reg, Error **errp);
void stix_att_unrealize(StixAttState *a);
void stix_att_reset(StixAttState *a);
/* true when the model owns the APB offset (att-model on, 0x5C only) */
bool stix_att_owns(StixAttState *a, hwaddr addr);
uint32_t stix_att_read(StixAttState *a, hwaddr addr);
void stix_att_write(StixAttState *a, hwaddr addr, uint32_t value);
/* the 0x5C readback as the FSW sees it now (no access counter) */
uint32_t stix_att_cmdsta(StixAttState *a);

#endif
