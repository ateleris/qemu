#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# ELUT smoke: the optional two-boot flight-ELUT persistence gate (ELUT
# plan E-2, docs/stix-port/2026-11-11-elut-plan.md).
#
# Run 1 (upload) boots a FRESH private flash (an empty sparse file of
# the full size, as nominal-smoke: no context, so TM(5,3) 0x8030 at
# connect), goes to CONFIGURATION, sets
# S20 = 0, sends the flight ELUT as 96 x TC(237,7) (elut-upload.py
# encode, every one acked TM(1,1) + TM(1,7)), applies it with TC(237,8)
# LUT ID 1 (FPGA written, 4 flash copies, CONFIG_CURRENT_ELUT_ID set),
# and enters NOMINAL once, which stores the context holding that ID.
# Run 2 (reboot) is a new QEMU process on the SAME flash, with no
# TC(237,7): it must boot with a context (0x8030 forbidden) and, at its
# NOMINAL entry, put the ELUT back into the FPGA from flash.
#
# E-4 (positive run 2 only): the no-fallback leg on the real ELUT. The
# gate writes a tiny synthetic .dssevs into its own run dir (never a
# user file: one 0xE setup word with the baseline 2000 for dss_det 0,
# then 4 events at adc 2000, fpga_det 0, pix 0), starts run 2's QEMU
# with -global grlib-stixctrl.det-replay-file=<run dir>/dssevs.elut2
# (realize-time: it cannot be set by QMP) and the traces
# stixdet_elut_fallback, stixdet_replay_scan and stixdet_replay_state,
# and sets det-source=replay-dss by QMP at @5, before the NOMINAL
# entry. The replay then starts at the SetEnable 0->1 edge of the
# NOMINAL entry, where stix_det.c classifies all 384 ELUT rows, and
# feeds its events: at flight-range adc 2000 the honoured ELUT branch
# bins them at the CSV's ebin, while the linear fallback (adc <= the
# 2000 baseline) would drop them. The verdict is elut-replay-check.py:
# the pre-scan line, a state=start (nominal) line (until D-7 grows a
# per-classification summary trace this is the witness that the rows
# were classified while det-source=replay-dss), zero ELUT fallback
# lines, an eof line with events_fed = the file's 4 events, and, in
# the SSID 32 dump of run 2's NOMINAL-exit flush (the TC(236,1) 04 00
# at @115 ends NOMINAL so the flush sends the one spectra dump of its
# first 8 QL periods), the counts at exactly that honoured ebin in
# exactly the one subcol. Without det-source the replay never starts
# and the exit flush's dump stays zero: no vacuous pass.
#
# The FPGA ELUT (12 pix x 32 fpga_det x 4096 adc bytes at 0xE0000000) is
# dumped by QMP pmemsave and checked by elut-upload.py check-dump (the
# FSW loop replay through fpga_det -> subcol, an independent oracle from
# the CSV edges, and the checker's own mapping sensitivity):
#   r1-pre      CONF, before the upload           constant 0
#   r1-apply    after TC(237,8)                   PASS
#   r1-nominal  after the NOMINAL entry           PASS ("apply after
#               reload": a failed flash load keeps the RAM table, so
#               this is NOT the persistence proof)
#   r2-pre      run 2, CONF                       constant 0
#   r2-nominal  run 2, after the NOMINAL entry    PASS
# The persistence proof is run 2's conjunction: no TC(237,7) in run 2
# (the RAM ELUT[] of a new process can only be filled by load_lut),
# r2-nominal PASS, and no "could not find LUT (type=3" / "problem
# reading LUT (type=3" in the run-2 UART.
#
# --negative (run once at acceptance): the same two boots without
# TC(237,8). r1-apply must stay constant 0 (TC(237,7) only fills RAM),
# r1-nominal PASSES (on a flash with no ELUT file the NOMINAL-entry
# reload misses, LUTMgmt.c:1140, and applyELUT writes the unpersisted
# RAM table), run 2 logs "could not find LUT (type=3, id=0" and its
# NOMINAL entry applies the all-zero RAM table: r2-nominal constant 31.
#
# AFTER A PASS THE RUN DIR HOLDS A SEEDED FLASH (ELUT ID 1 plus a
# context) in legacy mode, until the next elut-smoke run; slot mode
# deletes it on PASS. A seeded flash gives no 0x8030 at boot, so it must
# never be used for gates that expect one. It also carries the flight
# ELUT, under which a DSS replay (baseline about adc 2000) lands almost
# entirely in ebin 30/31 (plan "DSS replay caveat"). --save-template
# PATH keeps a verified sparse copy of it, taken before any cleanup.
#
# Ports, run dir and lock (scripts/stix/lib/slot.sh): STIX_PORT_BASE
# unset is legacy (port 20000+uid; the caller holds the
# /tmp/stix-qtest-21000 flock), auto or k >= 1 takes a slot (port B_k,
# run dir <dir>-s<k>).
#
# Usage: elut-smoke.sh <qemu-binary> [--negative] [--dry-run]
#                      [--check-guard] [--save-template PATH]
#   STIX_ELUT_CSV        the flight ELUT CSV (by path only; never copied).
#                        Unset or unreadable: SKIP, exit 77.
#   STIX_ASW_ELF         as in tc-smoke.sh; absent: SKIP, exit 77
#   STIX_ELUT_RUN_DIR    run dir (default /tmp/stix-elut-run; --negative
#                        uses <dir>-neg). Canonicalised; the shared
#                        uprev/baseline/GS run dirs, their parents and
#                        children and / are refused.
#   STIX_PORT_BASE       the slot, see above
#   STIX_TMTC_PROBE      the probe script (tests substitute a fake)
#   --dry-run prints both runs' QEMU and probe command lines, boots
#             nothing (it SKIPs like a real run without the CSV or ELF).
#   --check-guard only runs the run-dir guard and exits 0/2.
#
# Exit codes: 0 pass (or dry-run / check-guard ok), 1 a failed check,
#             2 environment problem, 77 SKIP.
set -euo pipefail

usage="usage: elut-smoke.sh <qemu-binary> [--negative] [--dry-run] \
[--check-guard] [--save-template PATH]"
QEMU_BIN_ARG="${1:?$usage}"
shift
NEGATIVE=0
DRY_RUN=0
CHECK_GUARD=0
TEMPLATE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --negative)
            [ "$NEGATIVE" -eq 0 ] || { echo "--negative given twice"; exit 2; }
            NEGATIVE=1 ;;
        --dry-run)
            [ "$DRY_RUN" -eq 0 ] || { echo "--dry-run given twice"; exit 2; }
            DRY_RUN=1 ;;
        --check-guard)
            [ "$CHECK_GUARD" -eq 0 ] || {
                echo "--check-guard given twice"; exit 2; }
            CHECK_GUARD=1 ;;
        --save-template)
            [ -z "$TEMPLATE" ] || {
                echo "--save-template given twice"; exit 2; }
            [ $# -ge 2 ] && [ -n "$2" ] || {
                echo "--save-template needs a PATH"; exit 2; }
            TEMPLATE="$2"
            shift ;;
        *) echo "unknown argument: $1"; echo "$usage"; exit 2 ;;
    esac
    shift
done
if [ "$NEGATIVE" -eq 1 ] && [ -n "$TEMPLATE" ]; then
    echo "--save-template needs a positive run (a --negative flash holds" \
        "no applied ELUT)"
    exit 2
fi

. "$(dirname "${BASH_SOURCE[0]}")/lib/paths.sh"
ASW_ELF="${STIX_ASW_ELF:-$STIX_DEFAULT_ELF}"
CSV="${STIX_ELUT_CSV:-}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOL="$REPO_ROOT/scripts/stix/elut-upload.py"
PROBE="${STIX_TMTC_PROBE:-$REPO_ROOT/scripts/stix/tmtc-probe.py}"
RUN_DIR="${STIX_ELUT_RUN_DIR:-/tmp/stix-elut-run}"
[ "$NEGATIVE" -eq 0 ] || RUN_DIR="${RUN_DIR%/}-neg"
R1_SECONDS=235
R2_SECONDS=110
if [ "$NEGATIVE" -eq 0 ]; then
    # the positive run 2 holds longer: the E-4 replay leg ends NOMINAL
    # at 115 (TC(236,1)) so the exit flush sends the one SSID 32 dump
    R2_SECONDS=145
fi
LUT_ID=1

. "$REPO_ROOT/scripts/stix/lib/slot.sh"
# dry-run, check-guard and the SKIPs boot nothing: they peek at the slot
# (no lock, no wait, no disk guard)
SKIP_WHY=""
if [ -z "$CSV" ]; then
    SKIP_WHY="STIX_ELUT_CSV unset; point it at the flight ELUT CSV"
elif [ ! -r "$CSV" ] || [ ! -f "$CSV" ]; then
    SKIP_WHY="STIX_ELUT_CSV not a readable file ($CSV)"
elif [ ! -f "$ASW_ELF" ]; then
    SKIP_WHY="ASW ELF absent ($ASW_ELF); set STIX_ASW_ELF to enable"
fi
if [ "$DRY_RUN" -eq 1 ] || [ "$CHECK_GUARD" -eq 1 ] || [ -n "$SKIP_WHY" ]; then
    stix_slot_setup peek
else
    stix_slot_setup
fi
RUN_DIR="$(stix_slot_dir "$RUN_DIR")"

# The shared flashes must never see this gate: canonical first, then
# refuse /, the shared run dirs (the uprev master, the frozen baseline,
# the GS flash, the legacy /tmp ones), anything inside them and any
# parent of them (nominal-smoke's guard).
RUN_DIR="$(realpath -m -- "$RUN_DIR")"
refuse_run_dir() {
    echo "refusing run dir $RUN_DIR ($1)"
    echo "this gate writes a context and an ELUT into its flash, so it"
    echo "needs its own run dir away from the shared ones"
    exit 2
}
[ "$RUN_DIR" != / ] || refuse_run_dir "it is the root directory"
for shared in "$STIX_UPREV_DIR" /tmp/stix-uprev-run \
              /tmp/stix-baseline-run /tmp/stix-gs-run; do
    shared="$(realpath -m -- "$shared")"
    case "$RUN_DIR" in
        "$shared" | "$shared"/*) refuse_run_dir "it is $shared or inside it" ;;
    esac
    case "$shared" in
        "$RUN_DIR"/*) refuse_run_dir "it is a parent of $shared" ;;
    esac
done
QMP_SOCK="$RUN_DIR/qmp.sock"
if [ "${#QMP_SOCK}" -ge 108 ]; then
    echo "refusing run dir $RUN_DIR (the QMP socket path $QMP_SOCK is" \
        "${#QMP_SOCK} bytes; a unix socket path must stay under 108)"
    exit 2
fi

if [ "$CHECK_GUARD" -eq 1 ]; then
    echo "GUARD OK: run dir $RUN_DIR (canonical)"
    exit 0
fi

QEMU_BIN="$(realpath -e "$QEMU_BIN_ARG")" || {
    echo "qemu binary not found: $QEMU_BIN_ARG"; exit 2; }
[ -x "$QEMU_BIN" ] || { echo "not executable: $QEMU_BIN"; exit 2; }

if [ -n "$SKIP_WHY" ]; then
    echo "SKIP: $SKIP_WHY"
    exit 77
fi

# TC units, as in nominal-smoke.sh (CRC included): CONF seq 1, S20=0
# seq 2, NOMINAL seq 3; run 2 (positive) ends NOMINAL with CONF seq 4
TC_CONF="40 02 00 00 1D AC C0 01 00 07 19 EC 01 00 04 00 4A C1"
TC_S20="40 02 00 00 1D AC C0 02 00 09 19 EC 06 00 07 04 00 00 AF BA"
TC_NOM="40 02 00 00 1D AC C0 03 00 07 19 EC 01 00 05 00 BF 97"
TC_CONF2="40 02 00 00 1D AC C0 04 00 07 19 EC 01 00 04 00 3D 0D"
ELUT_ADDR=3758096384    # 0xE0000000, stixctrl.sram
ELUT_SIZE=1572864       # 12 pix x 32 fpga_det x 4096 adc

# E-4: the run-2 replay leg's parameters (see the header). adc 2000 is
# flight-range; the file's 2000 baseline makes the linear fallback drop
# the events, so only the honoured ELUT branch can put counts in SSID 32.
DET7_ADC=2000
DET7_PIX=0
DET7_FPGA=0
DET7_BASE=2000
DET7_COUNT=4
DET7_DSSEVS="$RUN_DIR/dssevs.elut2"
QL_DUMP="$RUN_DIR/ql.dump.elut2.hex"
REPLAY_CHECK="$REPO_ROOT/scripts/stix/elut-replay-check.py"

pmemsave() { # name -> a --qmp-cmd JSON writing $RUN_DIR/elut-dump.<name>.bin
    printf '{"execute": "pmemsave", "arguments": %s}' \
        "$(printf '{"val": %s, "size": %s, "filename": "%s"}' \
            "$ELUT_ADDR" "$ELUT_SIZE" "$RUN_DIR/elut-dump.$1.bin")"
}
qom_set() { # path property json-value (detector-smoke's form)
    printf '{"execute": "qom-set", "arguments": %s' \
        "{\"path\": \"$1\", \"property\": \"$2\", \"value\": $3}}"
}

# The ELUT sequence from the tool: 96 TC(237,7) at 50 + 0.75 k, each
# TM(1,1)@5 + TM(1,7)@5, then (positive) TC(237,8) at 127, TM(1,7)@38.
# Every window that guards a later step closes before that step's send
# (plan D-Ee).
ENC_ARGS=(encode --csv "$CSV" --lut-id "$LUT_ID" --first-seq 0x010
          --t0 50 --pace 0.75 --emit probe-args)
[ "$NEGATIVE" -eq 0 ] || ENC_ARGS+=(--no-apply)
ENC_OUT="$(mktemp)"
ENC_ERR="$(mktemp)"
if ! python3 "$TOOL" "${ENC_ARGS[@]}" > "$ENC_OUT" 2> "$ENC_ERR"; then
    cat "$ENC_ERR"
    rm -f "$ENC_OUT" "$ENC_ERR"
    echo "ELUT-SMOKE: ERROR (elut-upload.py encode failed on" \
        "$CSV)${STIX_SLOT_TAG}"
    exit 2
fi
mapfile -t ELUT_ARGS < "$ENC_OUT"
ELUT_RECORD="$(cat "$ENC_ERR")"
rm -f "$ENC_OUT" "$ENC_ERR"

# forbids for the whole run: any TC rejection
FORBID_ACKS=(--forbid "0x5A1:1:2" --forbid "0x5A1:1:8")
CONF_ARGS=(
    --send-at 35 --send-hex "$TC_CONF"
    --expect "0x5A1:1:1:1DACC001@12"
    --expect "0x5A7:5:1:00210204@12"
    --expect "0x5A1:1:7:1DACC001@12"
    --forbid "0x5A7:5:1:00210402"
    --send-at 40 --send-hex "$TC_S20"
    --expect "0x5A1:1:1:1DACC002@8"
    --expect "0x5A1:1:7:1DACC002@8"
)
# At each NOMINAL entry: 542c01 (no temperature LUT, once per boot) is
# the positive control for the 542c03 (ELUT apply error) forbid: seen
# live in both runs on 2026-11-11, so it is expected. The boot TC20
# timer 0x142E is neither expected nor forbidden at connect (run 2 boots
# with the stored S20 = 0 and sent none); the NOMINAL forbid arms later.
nominal_args() { # t -> the NOMINAL group at t
    NOM_ARGS=(
        --send-at "$1" --send-hex "$TC_NOM"
        --expect "0x5A1:1:1:1DACC003"
        --expect "0x5A7:5:1:00210405@45"
        --expect "0x5A1:1:7:1DACC003@45"
        --expect "0x5A7:5:2:542c01@45"
        --forbid "0x5A7:5:2:542c03"
        --forbid "0x5A7:5:1:00210502"
        --forbid "0x5A7:5:1:142e"
        --forbid "0x5A7:5:3:9449"
    )
}
nominal_args 175
R1_ARGS=(
    --port "$TMTC_PORT" --seconds "$R1_SECONDS" --window 10 --hold
    --qmp "$QMP_SOCK"
    --expect "0x5A7:5:3:8030@50"
    --expect "0x5A7:5:1:002200000008000000@50"
    --expect "0x5A7:5:1:14a3@30"
    "${FORBID_ACKS[@]}"
    "${CONF_ARGS[@]}"
    --send-at 45 --qmp-cmd "$(pmemsave r1-pre)"
    "${ELUT_ARGS[@]}"
    --send-at 170 --qmp-cmd "$(pmemsave r1-apply)"
    "${NOM_ARGS[@]}"
    --send-at 225 --qmp-cmd "$(pmemsave r1-nominal)"
)
nominal_args 50
R2_ARGS=(
    --port "$TMTC_PORT" --seconds "$R2_SECONDS" --window 10 --hold
    --qmp "$QMP_SOCK"
    --expect "0x5A7:5:1:002200000008000000@50"
    --expect "0x5A7:5:1:14a3@30"
    --forbid "0x5A7:5:3:8030"
    "${FORBID_ACKS[@]}"
    "${CONF_ARGS[@]}"
    --send-at 45 --qmp-cmd "$(pmemsave r2-pre)"
    "${NOM_ARGS[@]}"
    --send-at 100 --qmp-cmd "$(pmemsave r2-nominal)"
)
if [ "$NEGATIVE" -eq 0 ]; then
    # E-4: the replay leg. det-source=replay-dss by QMP before the
    # NOMINAL entry; TC(236,1) at 115 ends NOMINAL (T_nom + ~60 s, past
    # the 8 QL periods SSID 32 sums) so the exit flush sends the one
    # spectra dump.
    R2_ARGS+=(
        --dump-apid "0x5DC:$QL_DUMP"
        --send-at 5 --qmp-cmd "$(qom_set /machine/stixctrl det-source \
            '"replay-dss"')"
        --send-at 115 --send-hex "$TC_CONF2"
        --expect "0x5A1:1:1:1DACC004"
        --expect "0x5A7:5:1:00210504@10"
        --expect "0x5A1:1:7:1DACC004@10"
        --expect "0x5DC:21:6:1e@15"
        --expect "0x5DC:21:6:1f@15"
        --expect "0x5DC:21:6:20@15"
        --expect "0x5DC:21:6:21@15"
        --expect "0x5DC:21:6:22@15"
    )
fi
qemu_args() { # n -> QEMU_ARGS for run n
    QEMU_ARGS=(
        -M leon3_generic -m 64M -no-reboot
        -display none -monitor none
        "${STIX_SLOT_QEMU_ARGS[@]}"
        -qmp "unix:$QMP_SOCK,server=on,wait=off"
        -serial "file:$RUN_DIR/uart.elut$1.raw"
        -kernel "$ASW_ELF"
        -trace stixspw_rx_truncated
    )
    if [ "$1" -eq 2 ] && [ "$NEGATIVE" -eq 0 ]; then
        # E-4: the realize-time replay file and the D-7 traces
        QEMU_ARGS+=(
            -global "grlib-stixctrl.det-replay-file=$DET7_DSSEVS"
            -trace stixdet_elut_fallback
            -trace stixdet_replay_scan
            -trace stixdet_replay_state
        )
    fi
}

MODE="positive"
[ "$NEGATIVE" -eq 0 ] || MODE="negative"
if [ "$DRY_RUN" -eq 1 ]; then
    echo "DRY RUN — the commands elut-smoke would run ($MODE):"
    echo "# $ELUT_RECORD"
    for n in 1 2; do
        qemu_args "$n"
        if [ "$n" -eq 1 ]; then
            secs="$R1_SECONDS"
            pa=("${R1_ARGS[@]}")
        else
            secs="$R2_SECONDS"
            pa=("${R2_ARGS[@]}")
        fi
        echo "# run $n"
        printf '(cd %s && timeout %s %s ' "$RUN_DIR" "$((secs + 30))" \
            "$QEMU_BIN"
        printf '%q ' "${QEMU_ARGS[@]}"
        printf '> qemu-stdout.elut%s.log 2>&1)\n' "$n"
        printf 'python3 -u %s ' "$PROBE"
        printf '%q ' "${pa[@]}"
        printf '\n'
    done
    exit 0
fi

if ss -ltn | grep -q ":${TMTC_PORT} "; then
    echo "TMTC port ${TMTC_PORT} busy — another QEMU instance" \
        "running?${STIX_SLOT_TAG}"
    exit 2
fi

mkdir -p "$RUN_DIR"
cd "$RUN_DIR"
# Only this gate's own files, by explicit path; then a fresh private
# flash: an empty sparse flash.bin of the full size (accepted-diffs §41
# "Fresh-flash gates pre-create flash.bin"), so QEMU never creates one
# itself. Not a copy of the master: the master holds a context (no
# 0x8030 at boot; measured on the first live run), and the gate needs
# a flash with none, so that run 2's forbidden 0x8030 proves the
# context its run 1 stored.
rm -f flash.bin uart.elut1.raw uart.elut2.raw uart.elut1.txt \
      uart.elut2.txt elut1.probe.log elut2.probe.log \
      qemu-stdout.elut1.log qemu-stdout.elut2.log qmp.sock \
      elut-dump.r1-pre.bin elut-dump.r1-apply.bin elut-dump.r1-nominal.bin \
      elut-dump.r2-pre.bin elut-dump.r2-nominal.bin \
      dssevs.elut2 ql.dump.elut2.hex replay.check.log
truncate -s 17716740096 flash.bin
# E-4: the synthetic .dssevs for run 2's replay leg, written by this
# gate into its own run dir (never a user file). Big-endian u32 words
# (stix_det.c): [31:20] val12 | [19:16] type | [15:11] dss_det |
# [10:0] wait in 20 ns units. One 0xE setup word (the baseline 2000
# for dss_det 0, as the real file carries) and 4 events at adc 2000,
# pix 0, dss_det 0, wait 0: they all arrive in the first tick after
# the SetEnable of the NOMINAL entry.
python3 - "$DET7_ADC" "$DET7_BASE" "$DET7_FPGA" "$DET7_PIX" \
    "$DET7_COUNT" <<'EOF'
import struct, sys
adc, base, fpga, pix, n = (int(a) for a in sys.argv[1:6])
with open("dssevs.elut2", "wb") as f:
    f.write(struct.pack(">I", (base << 20) | (0xE << 16) | (fpga << 11)))
    for _ in range(n):
        f.write(struct.pack(">I", (adc << 20) | (pix << 16) | (fpga << 11)))
EOF
echo "$ELUT_RECORD"

PROBE_PID=""
QEMU_PID=""
cleanup() {
    if [ -n "$QEMU_PID" ]; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if [ -n "$PROBE_PID" ]; then
        kill "$PROBE_PID" 2>/dev/null || true
        wait "$PROBE_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

fail() { # reason
    echo "ELUT-SMOKE${VERDICT_MODE}: FAIL ($*)${STIX_SLOT_TAG}"
    exit 1
}
VERDICT_MODE=""
[ "$NEGATIVE" -eq 0 ] || VERDICT_MODE=" (negative)"

# One boot: the probe first (it retries until QEMU binds the port), then
# QEMU; the verdict of the probe, with the reason, as in nominal-smoke.
boot() { # n seconds probe-args...
    local n="$1" secs="$2" rc alive qrc why log="elut$1.probe.log"
    shift 2
    stix_disk_guard
    rm -f qmp.sock
    qemu_args "$n"
    timeout "$((secs + 30))" python3 -u "$PROBE" "$@" > "$log" 2>&1 &
    PROBE_PID=$!
    timeout "$((secs + 30))" "$QEMU_BIN" "${QEMU_ARGS[@]}" \
        > "qemu-stdout.elut$n.log" 2>&1 &
    QEMU_PID=$!
    set +e
    wait "$PROBE_PID"
    rc=$?
    PROBE_PID=""
    kill -0 "$QEMU_PID" 2>/dev/null
    alive=$?
    kill "$QEMU_PID" 2>/dev/null
    wait "$QEMU_PID" 2>/dev/null
    qrc=$?
    QEMU_PID=""
    set -e
    if [ "$alive" -ne 0 ]; then
        tail -5 "qemu-stdout.elut$n.log"
        echo "ELUT-SMOKE${VERDICT_MODE}: ERROR (run $n: QEMU died or never" \
            "started; qemu rc=$qrc)${STIX_SLOT_TAG}"
        exit 2
    fi
    if [ "$rc" -eq 124 ]; then
        tail -5 "$log"
        echo "ELUT-SMOKE${VERDICT_MODE}: ERROR (run $n: the probe hit its" \
            "own timeout)${STIX_SLOT_TAG}"
        exit 2
    fi
    if grep -q "could not connect" "$log"; then
        tail -5 "$log"
        echo "ELUT-SMOKE${VERDICT_MODE}: ERROR (run $n: the probe never" \
            "connected)${STIX_SLOT_TAG}"
        exit 2
    fi
    if [ "$rc" -ne 0 ]; then
        why="$(grep -m1 -o 'FORBIDDEN TM: .*' "$log" | cut -d' ' -f3-)" || true
        if [ -n "$why" ]; then
            why="forbidden: $why"
        else
            why="$(grep -m1 -o 'EXPECTATION NOT MATCHED: .*' "$log" |
                cut -d' ' -f4-)" || true
            if [ -n "$why" ]; then
                why="not matched: $why"
            elif grep -m1 -q '^QMP ERROR' "$log"; then
                why="$(grep -m1 '^QMP ERROR' "$log")"
            elif grep -q '^CONNECTION CLOSED' "$log"; then
                why="connection closed"
            else
                why="probe rc=$rc; see $log"
            fi
        fi
        tail -5 "$log"
        fail "run $n: $why"
    fi
    tr -d '\000' < "uart.elut$n.raw" > "uart.elut$n.txt"
    if grep -q 'stixspw_rx_truncated' "qemu-stdout.elut$n.log"; then
        grep -m1 'stixspw_rx_truncated' "qemu-stdout.elut$n.log"
        fail "run $n: a TC unit was truncated at SpW RX"
    fi
}

dump_check() { # name [--expect-constant V]: check-dump's rc, explicitly
    local name="$1" rc
    shift
    echo "check-dump $name $*:"
    set +e
    python3 "$TOOL" check-dump --csv "$CSV" \
        --dump "$RUN_DIR/elut-dump.$name.bin" "$@" | sed 's/^/  /'
    rc="${PIPESTATUS[0]}"
    set -e
    case "$rc" in
        0) ;;
        1) fail "dump $name: check-dump mismatch${1:+ (want $* )}" ;;
        *) echo "ELUT-SMOKE${VERDICT_MODE}: ERROR (dump $name: check-dump" \
               "rc=$rc)${STIX_SLOT_TAG}"
           exit 2 ;;
    esac
}

uart_has() { # n text
    grep -qF -- "$2" "uart.elut$1.txt" ||
        fail "run $1 UART: '$2' missing"
}
uart_lacks() { # n text
    if grep -qF -- "$2" "uart.elut$1.txt"; then
        grep -m1 -F -- "$2" "uart.elut$1.txt"
        fail "run $1 UART: '$2' present"
    fi
}
mono_of() { # file pattern -> the [m=...] stamp of the first matching line
    grep -m1 -F -- "$2" "$1" | grep -o '\[m=[0-9.]*\]' | tr -d '[]m=' || true
}

# ---- run 1: upload (and, positive, apply), NOMINAL entry
echo "run 1: upload ($MODE)"
boot 1 "$R1_SECONDS" "${R1_ARGS[@]}"
uart_has 1 "init: SAFE mode"
uart_has 1 "could not find LUT (type=3, id=0"
for t in "problem writing LUT" "problem reading LUT (type=3" \
         "could not find LUT (type=3, id=1" "ELUT update got cancelled" \
         "DetectorsApi_setElut(" "LUTMgmt_uploadLUT: processing" \
         "storing new context0 failed" "storing new context1 failed"; do
    uart_lacks 1 "$t"
done
NERASE="$(grep -c 'problem erasing LUT type=3,' uart.elut1.txt || true)"
if [ "$NEGATIVE" -eq 0 ]; then
    [ "$NERASE" -eq 4 ] ||
        fail "run 1 UART: $NERASE 'problem erasing LUT type=3' lines," \
            "want 4 (one per copy)"
else
    [ "$NERASE" -eq 0 ] ||
        fail "run 1 UART: $NERASE 'problem erasing LUT type=3' lines" \
            "without TC(237,8)"
fi
dump_check r1-pre --expect-constant 0
if [ "$NEGATIVE" -eq 0 ]; then
    dump_check r1-apply
    T_SEND="$(mono_of elut1.probe.log '>> sent 19B')"
    T_DONE="$(mono_of elut1.probe.log 'EXPECT OK: 0x5A1:1:7:1DACC070@')"
    if [ -n "$T_SEND" ] && [ -n "$T_DONE" ]; then
        echo "TC(237,8) apply + store: $(awk -v a="$T_SEND" -v b="$T_DONE" \
            'BEGIN { printf "%.2f", b - a }') s (TC to TM(1,7), monotonic)"
    fi
else
    dump_check r1-apply --expect-constant 0
fi
T_SEND="$(mono_of elut1.probe.log '>> sent 18B (40 02 00 00 1D AC C0 03')"
T_DONE="$(mono_of elut1.probe.log 'EXPECT OK: 0x5A1:1:7:1DACC003@')"
if [ -n "$T_SEND" ] && [ -n "$T_DONE" ]; then
    echo "run 1 NOMINAL entry incl. ELUT re-apply: $(awk -v a="$T_SEND" \
        -v b="$T_DONE" 'BEGIN { printf "%.2f", b - a }') s (TC to TM(1,7))"
fi
echo "r1-nominal is the apply after reload, not the flash proof:"
dump_check r1-nominal

# ---- run 2: reboot on the same flash, no upload
rm -f qmp.sock
echo "run 2: reboot on the same flash, no TC(237,7)"
boot 2 "$R2_SECONDS" "${R2_ARGS[@]}"
uart_has 2 "init: SAFE mode"
uart_has 2 "could not find LUT (type=1"
for t in "ELUT update got cancelled" "DetectorsApi_setElut(" \
         "storing new context0 failed" "storing new context1 failed"; do
    uart_lacks 2 "$t"
done
dump_check r2-pre --expect-constant 0
if [ "$NEGATIVE" -eq 0 ]; then
    uart_lacks 2 "could not find LUT (type=3"
    uart_lacks 2 "problem reading LUT (type=3"
    echo "persistence proof (no TC(237,7) in run 2, ELUT loaded from flash):"
    dump_check r2-nominal
    echo "E-4 replay verdict (run 2, det-source=replay-dss):"
    set +e
    python3 "$REPLAY_CHECK" \
        --qemu-log "$RUN_DIR/qemu-stdout.elut2.log" \
        --dssevs "$DET7_DSSEVS" \
        --ssid32-dump "$QL_DUMP" \
        --csv "$CSV" --adc "$DET7_ADC" --fpga-det "$DET7_FPGA" \
        --pix "$DET7_PIX" --base "$DET7_BASE" --count "$DET7_COUNT" \
        > replay.check.log 2>&1
    RRC=$?
    set -e
    sed 's/^/  /' replay.check.log
    case "$RRC" in
        0) ;;
        1) if grep -q '^REPLAY-CHECK: ' replay.check.log; then
               why="$(grep -m1 -o 'REPLAY-CHECK: FAIL: .*' \
                          replay.check.log | cut -d' ' -f3-)" || true
               fail "run 2 replay: $why"
           fi
           echo "ELUT-SMOKE${VERDICT_MODE}: ERROR (replay check rc=1" \
               "without a verdict; see replay.check.log)${STIX_SLOT_TAG}"
           exit 2 ;;
        *) echo "ELUT-SMOKE${VERDICT_MODE}: ERROR (replay check rc=$RRC;" \
               "see replay.check.log)${STIX_SLOT_TAG}"
           exit 2 ;;
    esac
else
    uart_has 2 "could not find LUT (type=3, id=0"
    dump_check r2-nominal --expect-constant 31
fi

# ---- pass: the flash sizes, the template (before any cleanup), cleanup
echo "flash.bin: apparent $(stat -c %s flash.bin) B, allocated" \
    "$(($(stat -c %b flash.bin) * $(stat -c %B flash.bin))) B"
if [ -n "$TEMPLATE" ]; then
    # D-El: pre-flight, copy sparse under timeout 30, verify
    T_ABS="$(realpath -m -- "$TEMPLATE")"
    T_DIR="$(dirname "$T_ABS")"
    case "$T_ABS" in
        /mnt | /mnt/*) fail "--save-template $T_ABS: under /mnt (no holes)" ;;
    esac
    [ ! -e "$T_ABS" ] || fail "--save-template $T_ABS exists; never overwritten"
    [ "$T_ABS" != "$RUN_DIR/flash.bin" ] ||
        fail "--save-template $T_ABS is the source"
    for shared in "$STIX_UPREV_DIR" /tmp/stix-uprev-run \
                  /tmp/stix-baseline-run /tmp/stix-gs-run; do
        shared="$(realpath -m -- "$shared")"
        case "$T_ABS" in
            "$shared"/*) fail "--save-template $T_ABS is inside $shared" ;;
        esac
    done
    [ -d "$T_DIR" ] || fail "--save-template: no directory $T_DIR"
    case "$(stat -f -c %T "$T_DIR")" in
        ext2/ext3 | ext4 | xfs | btrfs | tmpfs) ;;
        *) fail "--save-template: $T_DIR is $(stat -f -c %T "$T_DIR"), not" \
               "a filesystem known to keep holes" ;;
    esac
    stix_disk_guard
    if ! timeout 30 cp --sparse=always flash.bin "$T_ABS"; then
        rm -f "$T_ABS"
        fail "--save-template: the copy failed or timed out"
    fi
    T_SIZE="$(stat -c %s "$T_ABS")"
    T_ALLOC="$(($(stat -c %b "$T_ABS") * $(stat -c %B "$T_ABS")))"
    if [ "$T_SIZE" -ne "$(stat -c %s flash.bin)" ] ||
            [ "$T_ALLOC" -ge $((64 * 1024 * 1024)) ]; then
        rm -f "$T_ABS"
        fail "--save-template: copy apparent $T_SIZE B, allocated $T_ALLOC B" \
            "(want the source's size and < 64 MiB); removed"
    fi
    echo "template flash: $T_ABS (apparent $T_SIZE B, allocated $T_ALLOC B)"
fi
rm -f elut-dump.r1-pre.bin elut-dump.r1-apply.bin elut-dump.r1-nominal.bin \
      elut-dump.r2-pre.bin elut-dump.r2-nominal.bin \
      dssevs.elut2 ql.dump.elut2.hex replay.check.log
stix_slot_cleanup 0 flash.bin qemu-stdout.elut1.log qemu-stdout.elut2.log
echo "ELUT-SMOKE${VERDICT_MODE}: PASS${STIX_SLOT_TAG}"
exit 0
