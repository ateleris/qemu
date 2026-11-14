#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# ROTB fill run (RF-3, plan docs/stix-port/2026-11-14-rotb-fill-plan.md):
# one attribution-matrix cell, end to end.  Modelled on l4-replay.sh
# phase 1 (fresh flash, CONF, params, S20=0, NOMINAL, det-flare) with
# nominal-smoke's run-dir guard and slot ports (lib/slot.sh).  The TC/TM
# side is scripts/stix/rotb-fill-drive.py, which anchors the whole
# schedule on the GUEST clock (Amendment A: tmtc-probe's --send-at is
# wall time, unusable under icount): it polls hw-state's vclock_ns over
# QMP and fires each schedule item when the guest seconds since the
# first sample reach its t.
#
# Per run (its own --out dir, fresh flash):
#   QEMU  -M leon3_generic -m 64M, the slot's TMTC port, QMP on TCP
#         port TMTC+4, one -trace (stixdet_chunk, stixdet_chunk_drop,
#         stixctrl_rotb_overflow, stixctrl_rotb_overwrite,
#         stixflash_op_start, stixflash_op_done -> trace.log), UART to
#         uart.raw, the cell's -icount / flash-latency / rotb-overflow
#         globals, STIX_ROTB_QEMU_EXTRA appended verbatim.
#   drive schedule (guest s):
#     50  TC(236,1) 04 00 CONF                 (seq 1)
#     55  TC(237,18) MIN=ticks + read-back     (seqs 4, 5; + MAX seqs
#         6, 7 with --max-ticks)  — the read-backs are --expect'ed,
#         so a wrong param index fails the run loudly
#     60  TC(236,6) S20=0                      (seq 2, the known unit)
#     70  TC(236,1) 05 00 NOMINAL              (seq 3, the known unit)
#     75  QMP qom-set det-flare-cps, then det-flare true (that order)
#     end --guest-seconds (default 1200)
#   The drive writes fill.csv (wall_mono, vclock_ns, rotb.count/full/
#   rd/wr/capacity — the rotb cells empty on a binary without the
#   hw-state rotb section), tm.log, events.log; the wrapper writes
#   run.json (every parameter) and exits with the drive's rc.
#
# Usage: rotb-fill-run.sh <qemu-binary> --ticks N [--max-ticks N]
#                         [--cps N] [--icount SHIFT] [--pace wall|fast]
#                         [--latency on|off] [--latency-scale K]
#                         [--overflow POLICY] [--guest-seconds N]
#                         [--flash FILE] --out DIR [--dry-run]
#   --ticks N           the integration param written to
#                       NOMINAL_ACCU_MIN_INTEGRATION (index 312; param
#                       N = (N+1) x 0.1 s, so 0.1 s = 0, 0.5 s = 4)
#   --max-ticks N       also write NOMINAL_ACCU_MAX_INTEGRATION
#                       (index 313); without it the ELF default holds
#   --cps N             det-flare-cps (default 2000, detector-smoke's)
#   --icount SHIFT      -icount shift=SHIFT; 0 = no icount (default 3,
#                       Amendment A: the "fast CPU" cell)
#   --pace wall|fast    pacing (default wall); fast adds ,sleep=off to
#                       the icount: guest idle time is skipped instead
#                       of waited out, the guest's own timing is
#                       unchanged (needs --icount > 0)
#   --latency on|off    the flash latency model (default on)
#   --latency-scale K   multiplies tread-us/tprog-us/tbers-us
#                       (defaults 130/300/1500)
#   --overflow POLICY   drop-new (default) | overwrite-oldest (RF-0's
#                       grlib-stixctrl.rotb-overflow)
#   --rcr on|off        the FSW rate control (default on). off sets
#                       RCR_ENABLED (index 273, AX ELF) to 0 before
#                       NOMINAL; NOMINAL entry re-runs RateCtrl_init,
#                       which reads it (OpModeMgmt.c:645 ->
#                       QLAccuDblBuffs.c:103 -> RateCtrl.c:667), so the
#                       ASIC reg 3 pixel masks stay at the RCR-0 config
#   --guest-seconds N   the drive's end in guest seconds (default 1200)
#   --flash FILE        sparse-copy this flash.bin as the start state
#                       (cp --sparse=always, e.g. the RF-2b
#                       aged-flash.bin); default fresh (truncate)
#   --out DIR           the run dir (required; must be fresh and away
#                       from the shared flash dirs — nominal-smoke's
#                       guard: a NOMINAL entry writes a flash context)
#   --dry-run           print the QEMU argv and the schedule JSON and
#                       exit 0, creating nothing
#   STIX_ASW_ELF        the FSW ELF (default $STIX_DEFAULT_ELF, the AX
#                       gate ELF; missing -> exit 77, the gate SKIP)
#   STIX_PORT_BASE      the slot (lib/slot.sh): TMTC port and run-dir
#                       suffix; unset = legacy 20000+uid, no lock
#   STIX_ROTB_QEMU_EXTRA   extra QEMU args, appended verbatim (plan RF-3)
#   STIX_ROTB_SAMPLE_EVERY the drive's --sample-every (default 1.0)
#   STIX_ROTB_WALL_PER_GUEST  wall-timeout multiplier (default 10,
#                       +300 s): under icount the guest clock can run
#                       slower than the wall; the timeout only bounds a
#                       wedged QEMU, a dead one fails the drive at once
#
# Exit codes: the drive's rc (0 pass, 1 fail, 124 the wall timeout), 2
# environment/argument problem, 77 SKIP (no ELF).
set -euo pipefail

usage="usage: rotb-fill-run.sh <qemu-binary> --ticks N [--max-ticks N] \
[--cps N] [--icount SHIFT] [--pace wall|fast] [--latency on|off] \
[--latency-scale K] [--overflow drop-new|overwrite-oldest] \
[--rcr on|off] [--guest-seconds N] [--flash FILE] --out DIR [--dry-run]"

QEMU_BIN_ARG="${1:?$usage}"
shift
TICKS=""
MAX_TICKS=""
CPS=2000
ICOUNT=3
PACE=wall
LATENCY=on
LATENCY_SCALE=1
OVERFLOW=drop-new
RCR=on
GUEST_SECONDS=1200
FLASH=""
OUT_DIR=""
DRY_RUN=0
while [ $# -gt 0 ]; do
    case "$1" in
        --ticks)
            [ $# -ge 2 ] || { echo "--ticks needs a value"; exit 2; }
            [ -z "$TICKS" ] || { echo "--ticks given twice"; exit 2; }
            TICKS="$2"; shift ;;
        --max-ticks)
            [ $# -ge 2 ] || { echo "--max-ticks needs a value"; exit 2; }
            [ -z "$MAX_TICKS" ] || { echo "--max-ticks given twice"; exit 2; }
            MAX_TICKS="$2"; shift ;;
        --cps)
            [ $# -ge 2 ] || { echo "--cps needs a value"; exit 2; }
            CPS="$2"; shift ;;
        --icount)
            [ $# -ge 2 ] || { echo "--icount needs a value"; exit 2; }
            ICOUNT="$2"; shift ;;
        --pace)
            [ $# -ge 2 ] || { echo "--pace needs a value"; exit 2; }
            PACE="$2"; shift ;;
        --latency)
            [ $# -ge 2 ] || { echo "--latency needs a value"; exit 2; }
            LATENCY="$2"; shift ;;
        --latency-scale)
            [ $# -ge 2 ] || { echo "--latency-scale needs a value"; exit 2; }
            LATENCY_SCALE="$2"; shift ;;
        --overflow)
            [ $# -ge 2 ] || { echo "--overflow needs a value"; exit 2; }
            OVERFLOW="$2"; shift ;;
        --rcr)
            [ $# -ge 2 ] || { echo "--rcr needs a value"; exit 2; }
            RCR="$2"; shift ;;
        --guest-seconds)
            [ $# -ge 2 ] || { echo "--guest-seconds needs a value"; exit 2; }
            GUEST_SECONDS="$2"; shift ;;
        --flash)
            [ $# -ge 2 ] || { echo "--flash needs a value"; exit 2; }
            [ -z "$FLASH" ] || { echo "--flash given twice"; exit 2; }
            FLASH="$2"; shift ;;
        --out)
            [ $# -ge 2 ] || { echo "--out needs a value"; exit 2; }
            [ -z "$OUT_DIR" ] || { echo "--out given twice"; exit 2; }
            OUT_DIR="$2"; shift ;;
        --dry-run)
            [ "$DRY_RUN" -eq 0 ] || { echo "--dry-run given twice"; exit 2; }
            DRY_RUN=1 ;;
        *) echo "unknown argument: $1"; echo "$usage"; exit 2 ;;
    esac
    shift
done
[ -n "$TICKS" ] || { echo "--ticks is required"; echo "$usage"; exit 2; }
[ -n "$OUT_DIR" ] || { echo "--out is required"; echo "$usage"; exit 2; }

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$REPO_ROOT/scripts/stix/lib/paths.sh"
DRIVE="$REPO_ROOT/scripts/stix/rotb-fill-drive.py"
ELF="${STIX_ASW_ELF:-$STIX_DEFAULT_ELF}"

. "$REPO_ROOT/scripts/stix/lib/slot.sh"
[ -f "$DRIVE" ] || { echo "not a file: $DRIVE"; exit 2; }
# dry-run boots nothing: peek at the slot (no lock, no wait, no disk
# guard); a missing ELF is a SKIP, except in dry-run
if [ "$DRY_RUN" -eq 1 ]; then
    stix_slot_setup peek
else
    if [ ! -f "$ELF" ]; then
        echo "SKIP: ASW ELF absent ($ELF); set STIX_ASW_ELF to enable"
        exit 77
    fi
    stix_slot_setup
fi
OUT_DIR="$(stix_slot_dir "$OUT_DIR")"
QEMU_BIN="$(realpath -e "$QEMU_BIN_ARG")" || {
    echo "qemu binary not found: $QEMU_BIN_ARG"; exit 2; }
[ -x "$QEMU_BIN" ] || { echo "not executable: $QEMU_BIN"; exit 2; }

# integer/enum validation before anything else
check_uint() { # name value
    case "$2" in
        '' | *[!0-9]*)
            echo "rotb-fill-run: $1 must be a whole number (got: $2)"
            exit 2 ;;
    esac
}
check_uint ticks "$TICKS"
check_uint cps "$CPS"
check_uint icount "$ICOUNT"
check_uint latency-scale "$LATENCY_SCALE"
check_uint guest-seconds "$GUEST_SECONDS"
[ -z "$MAX_TICKS" ] || check_uint max-ticks "$MAX_TICKS"
[ "$CPS" -gt 0 ] || { echo "--cps must be > 0"; exit 2; }
[ "$ICOUNT" -le 15 ] || { echo "--icount shift must be 0..15"; exit 2; }
case "$PACE" in wall | fast) ;; *)
    echo "--pace wants wall|fast (got: $PACE)"; exit 2 ;;
esac
[ "$PACE" = wall ] || [ "$ICOUNT" -gt 0 ] || {
    echo "--pace fast needs --icount > 0 (sleep=off is an icount option)"
    exit 2
}
case "$LATENCY" in on | off) ;; *)
    echo "--latency wants on|off (got: $LATENCY)"; exit 2 ;;
esac
case "$RCR" in on | off) ;; *)
    echo "--rcr wants on|off (got: $RCR)"; exit 2 ;;
esac
case "$OVERFLOW" in drop-new | overwrite-oldest) ;; *)
    echo "--overflow wants drop-new|overwrite-oldest (got: $OVERFLOW)"
    exit 2 ;;
esac
[ "$GUEST_SECONDS" -gt 0 ] || { echo "--guest-seconds must be > 0"; exit 2; }
[ "$LATENCY_SCALE" -ge 1 ] || { echo "--latency-scale must be >= 1"; exit 2; }
for v in "$TICKS" $MAX_TICKS; do
    [ "$v" -le 2147483647 ] || {
        echo "ticks must fit a positive i32 (got: $v)"; exit 2; }
done
if [ -n "$MAX_TICKS" ] && [ "$TICKS" -gt "$MAX_TICKS" ]; then
    echo "--ticks $TICKS > --max-ticks $MAX_TICKS: the FSW would" \
        "reject NOMINAL_ACCU_MIN_INTEGRATION above MAX"
    exit 2
fi
if [ -z "$MAX_TICKS" ] && [ "$TICKS" -gt 199 ]; then
    echo "WARNING: --ticks $TICKS exceeds the ELF's default" \
        "NOMINAL_ACCU_MAX_INTEGRATION (199); pass --max-ticks to" \
        "raise it (the read-back gate still checks the set)"
fi

# the run-dir guard (nominal-smoke's, via l4-replay's form): canonical
# first, then refuse /, the shared flash dirs, anything inside them and
# any parent of them; a NOMINAL entry writes a flash context, so this
# wrapper needs its own fresh dir.  A live run additionally refuses an
# existing non-empty dir (never reuse a flash).
OUT_DIR="$(realpath -m -- "$OUT_DIR")"
refuse_run_dir() {
    echo "refusing run dir $OUT_DIR ($1)"
    echo "a fill run enters NOMINAL and writes a flash context, so it"
    echo "needs its own fresh run dir away from the shared ones"
    exit 2
}
[ "$OUT_DIR" != / ] || refuse_run_dir "it is the root directory"
for shared in "$STIX_UPREV_DIR" /tmp/stix-uprev-run \
              /tmp/stix-baseline-run /tmp/stix-gs-run; do
    shared="$(realpath -m -- "$shared")"
    case "$OUT_DIR" in
        "$shared" | "$shared"/*) refuse_run_dir "it is $shared or inside it" ;;
    esac
    case "$shared" in
        "$OUT_DIR"/*) refuse_run_dir "it is a parent of $shared" ;;
    esac
done
if [ "$DRY_RUN" -eq 0 ] && [ -e "$OUT_DIR" ] &&
        [ -n "$(ls -A "$OUT_DIR" 2>/dev/null)" ]; then
    refuse_run_dir "it exists and is not empty (a fill run never" \
        "reuses a dir)"
fi

TMTC_PORT="${TMTC_PORT:?slot.sh must set TMTC_PORT}"
QMP_PORT=$((TMTC_PORT + 4))   # B+4: inside the slot's band (B+1/B+2
                              # are the GS UDP reserves, B+3.. the
                              # qtest suite's; slot mode holds the lock)
WALL=$((GUEST_SECONDS * ${STIX_ROTB_WALL_PER_GUEST:-10} + 300))

# ------------------------------------------------ the TC hex strings
# The three mode TCs are l4-replay's/nominal-smoke's known units (seqs
# 1, 2, 3 — their CRCs cover the seq bytes, so they are used verbatim);
# the TM(1,1)/TM(1,7) acks echo the seq as 1DACC0<seq>, which makes
# each expectation distinct.  The param TCs are built by the RF-1
# builders in rotb-fill-drive.py (--emit-param-*, so the builder lives
# only there) with the AX gate ELF's indices; they take seqs 4..7,
# between CONF's 1 and the acks of the known units.
TC_CONF="40 02 00 00 1D AC C0 01 00 07 19 EC 01 00 04 00 4A C1"
TC_S20="40 02 00 00 1D AC C0 02 00 09 19 EC 06 00 07 04 00 00 AF BA"
TC_NOM="40 02 00 00 1D AC C0 03 00 07 19 EC 01 00 05 00 BF 97"
IDX_MIN=312                   # NOMINAL_ACCU_MIN_INTEGRATION (AX ELF)
IDX_MAX=313                   # NOMINAL_ACCU_MAX_INTEGRATION
TC_SET_MIN="$(python3 -u "$DRIVE" --emit-param-set "$IDX_MIN" \
    "$TICKS" 4)" || exit 2
TC_GET_MIN="$(python3 -u "$DRIVE" --emit-param-get "$IDX_MIN" 5)" \
    || exit 2
TC_SET_MAX=""
TC_GET_MAX=""
if [ -n "$MAX_TICKS" ]; then
    TC_SET_MAX="$(python3 -u "$DRIVE" --emit-param-set "$IDX_MAX" \
        "$MAX_TICKS" 6)" || exit 2
    TC_GET_MAX="$(python3 -u "$DRIVE" --emit-param-get "$IDX_MAX" 7)" \
        || exit 2
fi
# the TM(237,20) read-back expectation hexes (index u16 BE + sub-id 00
# + value i32 BE; aspect-smoke's gate 0x5A8:237:20:01DD0000000064)
RB_MIN="$(printf '%04X00%08X' "$IDX_MIN" "$TICKS")"
IDX_RCR=273                   # RCR_ENABLED (AX ELF)
TC_SET_RCR=""
TC_GET_RCR=""
if [ "$RCR" = off ]; then
    TC_SET_RCR="$(python3 -u "$DRIVE" --emit-param-set "$IDX_RCR" 0 8)" \
        || exit 2
    TC_GET_RCR="$(python3 -u "$DRIVE" --emit-param-get "$IDX_RCR" 9)" \
        || exit 2
fi
RB_RCR="$(printf '%04X00%08X' "$IDX_RCR" 0)"
RB_MAX=""
[ -z "$MAX_TICKS" ] || RB_MAX="$(printf '%04X00%08X' "$IDX_MAX" \
    "$MAX_TICKS")"

# ------------------------------------------------ the drive arguments
# End-of-run expectations (the drive exits 1 listing any never seen):
# the TM(1,1)/TM(1,7) acks of every send (each echoes its seq), the
# TM(237,20) read-back of every param set (a wrong index fails loudly),
# the NOMINAL-entry event 0021 04 05.
EXPECT_ARGS=()
add_expect() { EXPECT_ARGS+=(--expect "$1"); }
# CONF (seq 1)
add_expect "0x5A1:1:1:1DACC001"
add_expect "0x5A1:1:7:1DACC001"
# params, MIN (set seq 4, read-back seq 5)
add_expect "0x5A1:1:1:1DACC004"
add_expect "0x5A1:1:7:1DACC004"
add_expect "0x5A8:237:20:$RB_MIN"
add_expect "0x5A1:1:1:1DACC005"
add_expect "0x5A1:1:7:1DACC005"
# params, MAX (set seq 6, read-back seq 7), only with --max-ticks
if [ -n "$MAX_TICKS" ]; then
    add_expect "0x5A1:1:1:1DACC006"
    add_expect "0x5A1:1:7:1DACC006"
    add_expect "0x5A8:237:20:$RB_MAX"
    add_expect "0x5A1:1:1:1DACC007"
    add_expect "0x5A1:1:7:1DACC007"
fi
# RCR_ENABLED=0 (set seq 8, read-back seq 9), only with --rcr off
if [ "$RCR" = off ]; then
    add_expect "0x5A1:1:1:1DACC008"
    add_expect "0x5A1:1:7:1DACC008"
    add_expect "0x5A8:237:20:$RB_RCR"
    add_expect "0x5A1:1:1:1DACC009"
    add_expect "0x5A1:1:7:1DACC009"
fi
# S20=0 (seq 2), NOMINAL (seq 3) and its entry event
add_expect "0x5A1:1:1:1DACC002"
add_expect "0x5A1:1:7:1DACC002"
add_expect "0x5A1:1:1:1DACC003"
add_expect "0x5A7:5:1:00210405"
add_expect "0x5A1:1:7:1DACC003"

# ------------------------------------------------ the schedule JSON
# t in GUEST seconds (the drive anchors every item on hw-state's
# vclock_ns); equal-t items fire in list order (the det-flare-cps
# qom-set precedes det-flare true).  Marks only annotate events.log.
SCHEDULE_HEAD="$(cat <<EOF
[
 {"t": 0, "mark": "start ticks=$TICKS max=${MAX_TICKS:-elf-default} cps=$CPS"},
 {"t": 50, "mark": "CONF"},
 {"t": 50, "tc": "$TC_CONF"},
 {"t": 55, "mark": "params MIN=$TICKS MAX=${MAX_TICKS:-elf-default}"},
 {"t": 55, "tc": "$TC_SET_MIN"},
 {"t": 55, "tc": "$TC_GET_MIN"}
EOF
)"
if [ -n "$MAX_TICKS" ]; then
    SCHEDULE_HEAD="$SCHEDULE_HEAD$(cat <<EOF
,
 {"t": 55, "tc": "$TC_SET_MAX"},
 {"t": 55, "tc": "$TC_GET_MAX"}
EOF
)"
fi
if [ "$RCR" = off ]; then
    SCHEDULE_HEAD="$SCHEDULE_HEAD$(cat <<EOF
,
 {"t": 55, "mark": "RCR_ENABLED=0"},
 {"t": 55, "tc": "$TC_SET_RCR"},
 {"t": 55, "tc": "$TC_GET_RCR"}
EOF
)"
fi
SCHEDULE_JSON="$SCHEDULE_HEAD$(cat <<EOF
,
 {"t": 60, "mark": "S20=0"},
 {"t": 60, "tc": "$TC_S20"},
 {"t": 70, "mark": "NOMINAL"},
 {"t": 70, "tc": "$TC_NOM"},
 {"t": 75, "mark": "flare cps=$CPS"},
 {"t": 75, "qmp": {"execute": "qom-set", "arguments": {"path": "/machine/stixctrl", "property": "det-flare-cps", "value": $CPS}}},
 {"t": 75, "qmp": {"execute": "qom-set", "arguments": {"path": "/machine/stixctrl", "property": "det-flare", "value": true}}}
]
EOF
)"

# sanity: the schedule the wrapper builds must itself validate
if ! printf '%s' "$SCHEDULE_JSON" | python3 -c 'import json, sys; json.load(sys.stdin)' 2>/dev/null; then
    echo "rotb-fill-run: internal error: the built schedule JSON is" \
        "invalid:"
    printf '%s\n' "$SCHEDULE_JSON"
    exit 2
fi

# ------------------------------------------------ the QEMU command
# One -trace only (a second resets the file, l4-replay's note): the
# fill/drain/overflow events plus the flash-op timings the RF-4
# extraction reads.  rotb-overflow is read at realize, so -global is
# the right form; the flash latency globals are -global too (the
# pfm-boot-smoke pattern).
QEMU_ARGS=(
    -M leon3_generic -m 64M -no-reboot
    -display none -monitor none
    "${STIX_SLOT_QEMU_ARGS[@]}"
    -qmp "tcp:127.0.0.1:$QMP_PORT,server=on,wait=off"
    -serial "file:$OUT_DIR/uart.raw"
    -kernel "$ELF"
    -msg timestamp=on
    -trace "events=$OUT_DIR/trace.events,file=$OUT_DIR/trace.log"
)
if [ "$ICOUNT" -gt 0 ]; then
    if [ "$PACE" = fast ]; then
        QEMU_ARGS+=(-icount "shift=$ICOUNT,sleep=off")
    else
        QEMU_ARGS+=(-icount "shift=$ICOUNT")
    fi
fi
if [ "$LATENCY" = off ]; then
    QEMU_ARGS+=(-global grlib-stixflash.latency=off)
fi
if [ "$LATENCY_SCALE" -ne 1 ]; then
    QEMU_ARGS+=(-global "grlib-stixflash.tread-us=$((130 * LATENCY_SCALE))")
    QEMU_ARGS+=(-global "grlib-stixflash.tprog-us=$((300 * LATENCY_SCALE))")
    QEMU_ARGS+=(-global "grlib-stixflash.tbers-us=$((1500 * LATENCY_SCALE))")
fi
QEMU_ARGS+=(-global "grlib-stixctrl.rotb-overflow=$OVERFLOW")
# plan RF-3's env passthrough, word-split into an array
if [ -n "${STIX_ROTB_QEMU_EXTRA:-}" ]; then
    read -ra QEMU_EXTRA <<< "$STIX_ROTB_QEMU_EXTRA"
    QEMU_ARGS+=("${QEMU_EXTRA[@]}")
fi

DRIVE_ARGS=(
    --port "$TMTC_PORT"
    --qmp "tcp:127.0.0.1:$QMP_PORT"
    --schedule "$OUT_DIR/schedule.json"
    --guest-seconds "$GUEST_SECONDS"
    --out "$OUT_DIR"
    --sample-every "${STIX_ROTB_SAMPLE_EVERY:-1.0}"
    "${EXPECT_ARGS[@]}"
)

# ------------------------------------------------ run.json
ELF_SHA256="null"
if [ -f "$ELF" ]; then
    ELF_SHA256="\"$(sha256sum "$ELF" | cut -d' ' -f1)\""
fi
ICOUNT_JSON="null"
if [ "$ICOUNT" -gt 0 ]; then
    ICOUNT_JSON="\"shift=$ICOUNT\""
fi
write_run_json() { # > $1
    cat > "$1" <<EOF
{
 "created_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
 "wrapper": "scripts/stix/rotb-fill-run.sh",
 "drive": "$DRIVE",
 "qemu": "$QEMU_BIN",
 "elf": "$ELF",
 "elf_sha256": $ELF_SHA256,
 "ticks": $TICKS,
 "max_ticks": ${MAX_TICKS:-null},
 "cps": $CPS,
 "icount": $ICOUNT_JSON,
 "pace": "$PACE",
 "latency": "$LATENCY",
 "latency_scale": $LATENCY_SCALE,
 "overflow": "$OVERFLOW",
 "rcr": "$RCR",
 "guest_seconds": $GUEST_SECONDS,
 "flash": "${FLASH_ABS:-fresh}",
 "out_dir": "$OUT_DIR",
 "tmtc_port": $TMTC_PORT,
 "qmp_port": $QMP_PORT,
 "wall_timeout_s": $WALL,
 "schedule": "$OUT_DIR/schedule.json"
}
EOF
}

# ------------------------------------------------ dry-run
if [ "$DRY_RUN" -eq 1 ]; then
    echo "DRY RUN — the commands rotb-fill-run.sh would run:"
    printf '(cd %s && timeout %s %s' "$OUT_DIR" "$((WALL + 60))" \
        "$QEMU_BIN"
    printf ' %q' "${QEMU_ARGS[@]}"
    printf ' > qemu-stdout.log 2>&1)\n'
    printf 'python3 -u %s' "$DRIVE"
    printf ' %q' "${DRIVE_ARGS[@]}"
    printf ' > drive.log 2>&1\n'
    echo "--- schedule JSON ($OUT_DIR/schedule.json) ---"
    printf '%s\n' "$SCHEDULE_JSON"
    echo "--- expects ---"
    printf '%s\n' "${EXPECT_ARGS[@]}"
    echo "DRY RUN — nothing booted, nothing written."
    exit 0
fi

# ------------------------------------------------ the live run
# Port collisions: another QEMU on the slot's TMTC port, or anything on
# the QMP port (both fatal before any flash is touched).
if ss -ltn | grep -q ":${TMTC_PORT} "; then
    echo "TMTC port ${TMTC_PORT} busy — another QEMU instance" \
        "running?${STIX_SLOT_TAG}"
    exit 2
fi
if ss -ltn | grep -q ":${QMP_PORT} "; then
    echo "QMP port ${QMP_PORT} busy (TMTC+4)${STIX_SLOT_TAG}"
    exit 2
fi
# the flash source is resolved before the cd into the run dir, so a
# relative --flash path survives
FLASH_ABS=""
if [ -n "$FLASH" ]; then
    FLASH_ABS="$(realpath -e -- "$FLASH")" || {
        echo "not a file: $FLASH"; exit 2; }
fi

mkdir -p "$OUT_DIR"
cd "$OUT_DIR"
rm -f flash.bin uart.raw trace.log trace.events qemu-stdout.log \
      drive.log schedule.json run.json fill.csv tm.log events.log
# the run's start state: a fresh sparse flash, or a sparse copy of the
# given one (the RF-2b aged-flash.bin pattern: cp --sparse=always, size
# verified, allocation capped — a dense 16.5 GB flash is refused)
if [ -n "$FLASH_ABS" ]; then
    # shellcheck disable=SC2086
    if ! timeout 120 ${STIX_CP_CMD:-cp --sparse=always} -- "$FLASH_ABS" \
            flash.bin; then
        rm -f flash.bin
        echo "rotb-fill-run: the --flash copy failed or timed out"
        exit 2
    fi
    ALLOC="$(du -k flash.bin | cut -f1)"
    if [ "$(stat -c %s flash.bin)" != "$(stat -c %s "$FLASH_ABS")" ] ||
            [ "$ALLOC" -ge "${STIX_ROTB_FLASH_MAX_ALLOC_KB:-2097152}" ]; then
        rm -f flash.bin
        echo "rotb-fill-run: the --flash copy is not a verified" \
            "sparse copy (allocated ${ALLOC} KiB)"
        exit 2
    fi
    echo "flash: sparse copy of $FLASH_ABS (allocated ${ALLOC} KiB)"
else
    truncate -s 17716740096 flash.bin
    echo "flash: fresh sparse flash.bin"
fi
printf '%s\n' stixdet_chunk stixdet_chunk_drop stixctrl_rotb_overflow \
    stixctrl_rotb_overwrite stixflash_op_start stixflash_op_done \
    > trace.events
write_run_json run.json
printf '%s\n' "$SCHEDULE_JSON" > schedule.json

# The drive starts first (it retries the TMTC connect until QEMU binds
# the port), then QEMU; both under a wall timeout (see WALL in the
# header: it bounds a wedged QEMU, a dead one fails the drive at once).
timeout "$WALL" python3 -u "$DRIVE" "${DRIVE_ARGS[@]}" \
    > drive.log 2>&1 &
DRIVE_PID=$!
timeout "$((WALL + 60))" "$QEMU_BIN" "${QEMU_ARGS[@]}" \
    > qemu-stdout.log 2>&1 &
QEMU_PID=$!

set +e
wait "$DRIVE_PID"
RC=$?
DRIVE_PID=""
kill "$QEMU_PID" 2>/dev/null
wait "$QEMU_PID" 2>/dev/null
QEMU_RC=$?
QEMU_PID=""
set -e

grep '^DRIVE:' drive.log || echo "(no DRIVE verdict in drive.log)"
if [ "$RC" -ne 0 ]; then
    tail -5 drive.log
fi
echo "rotb-fill-run: drive rc=$RC (qemu rc=$QEMU_RC), run dir" \
    "$OUT_DIR${STIX_SLOT_TAG}"
exit "$RC"
