#!/usr/bin/env bash
# DETECTOR smoke: the detector-model gate for tiers 0+1 (detector plan
# D-3, docs/stix-port/2026-11-11-detector-plan.md) and, with --flare,
# for the synthetic sky and the flare switch (tier 2, D-4). Boots the
# STIX model on a FRESH flash (an empty run dir: a NOMINAL entry writes
# a flash context, as in nominal-smoke.sh), with a QMP socket in the
# run dir (the --flare mode flips det-flare through it), and QEMU
# traces stixdet_idefx_cmd with wall-clock timestamps into its own
# trace file. Without --flare QEMU runs with
# -global grlib-stixctrl.det-source=zero (the tiers 0+1 zero sky the
# D-3 checks expect; the model's default is the synthetic sky since
# D-4). Times are seconds from the TMTC connect:
#   connect: the nominal-smoke group, TM(5,3) 0x8030 (fresh flash),
#            TM(5,1) 0x0022, 0x14A3, 0x142E, each within 50 s
#   @50  TC(236,1) 04 00 (CONF): TM(1,1), TM(5,1) 0021 02 04, TM(1,7);
#        forbid 0021 04 02
#   @60  TC(236,6) 07 04 00 00 (S20 = 0): TM(1,1), TM(1,7)
#   @62  TC(236,6) 02 02 00 1E (system 2 detectors, parameter 2 ASIC
#        verification period, u16 30 s; HWConfig.c:974-976): TM(1,1),
#        TM(1,7)
#   @70  TC(236,1) 05 00 (NOMINAL): TM(1,1), TM(5,1) 0021 04 05 within
#        10 s (its arrival is T_nom), TM(1,7). Forbidden from here to
#        the end: 0x542E, 0x9438, 0x142C (a verification mismatch),
#        0x9433, 0x1427 (SEU), 0x942E, 0x5437 (noisy detector),
#        0021 05 02 (NOMINAL -> SAFE), TM(1,8) and 0x142E (TC20)
#   @215 TC(236,1) 04 00 (CONF again, a new sequence count): TM(1,1),
#        TM(5,1) 0021 05 04 within 10 s (its arrival is T_exit),
#        TM(1,7), and the NOMINAL-exit flush of all five QL products
#        (QLAccuDblBuffs.c:117-125) within 15 s: TM(21,6) on 0x5DC with
#        SSID 30, 31, 32, 33 and 34; forbid 0021 04 02
#   hold to +240 s.
# The probe appends every 0x5DC packet to ql.dump.hex (--dump-apid;
# whole CCSDS packets, the SSID at byte 16), and after the run
# ql-check.py --expect-zero-flush (T_exit - T_nom) checks the flush:
# the sky is zero in tiers 0+1, so every count, trigger, RCR, variance
# and flare byte must be 0, the header fields at their defaults, about
# one SSID 30/33/34 sample per 4 s and one SSID 31 sample per 8 s of
# NOMINAL (+-2), and exactly one SSID 32 dump of 32 samples whose
# subcol bytes are 0..31 once each.
# UART checks: "init: SAFE mode" present; no "storing new context[01]
# failed"; no IDeF-X error (XrayDetFDIR.c:731 write timeouts, "failed
# to read register" read timeouts, XrayDetFDIR.c:613); none of "packet
# sequence error", "RotBuf overrun", "UNKNOWN RB-BLOCK", "TOO MANY
# RB-BLOCKS".
# ASIC verification evidence (trace): a successful verification is
# silent (ENABLE_TRACE_DETFDIR 0, configFSW.h:89), so the evidence is
# the trace, checked by idefx-trace-check.py: delivered read address
# blocks (word 0x84000 | reg << 7, reg 1..12, delivered_grp_mask != 0),
# clustered by a > 5 s gap. Each cluster must be one run of exactly 384
# (every reg 1..12 x every grp 0..15 twice, i.e. 32 ASICs), inside the
# NOMINAL window [T_nom - 2 s, T_exit + 2 s], at least 3 runs, and no
# undelivered read address block. The probe's HH:MM:SS times are local
# and the trace's ISO times are UTC; both become epoch seconds.
# --flare (D-4) keeps the synthetic sky (det-source=synthetic, 20 cps
# per fpga_det quiet, 2000 flare) and adds two QMP groups:
#   @135 qom-set /machine/stixctrl det-flare true
#   @195 qom-set /machine/stixctrl det-flare false
# (the @215 CONF follows as before). The QL check then becomes
# ql-check.py --expect-sky-flush (T_exit - T_nom) (the same products,
# headers and cadences, but non-zero content: SSID 30 band 0, SSID 31,
# SSID 33 in every sample but one, every SSID 30 RCR byte 0, SSID 32
# non-zero for every subcol, nothing in ebin 0/31, at least half in
# the line bins ebin 18/19/27), and a second check, ql-contrast:
# ql-check.py --lc-contrast ON OFF with the switch times (the probe's
# QMP send times) minus T_nom: the SSID 30 band-0 median of the flare
# samples must be >= 8 compressed steps above the quiet median. The
# SSID 34 flags in the flare window are printed, not asserted. The
# QEMU process's user+sys CPU seconds are printed in every mode, for
# the D-4 cost record (det-source=zero vs synthetic).
# --flare --negative sends det-flare false at @135 (no flip; the QMP
# path stays exercised) and passes only when exactly ql-contrast
# fails.
# --negative alone runs QEMU with -global grlib-stixctrl.det-idefx=off
# (CmdReady 0, accepted-diffs §32). A read timeout is not a mismatch
# (XrayDetFDIR.c:611-615), so no TM fault is expected: the probe must
# still pass (every forbid quiet, the flush seen) and ql-check must
# still pass. The gate passes only when exactly two checks fail, the
# way the plan says: the UART check (exactly 384 XrayDetFDIR.c:731
# lines, plus "failed to read register" TRACE_ERRORs, 12 per
# verification run, at least 3 runs) and the trace check, which in
# this mode must find the trace file present and empty: no IDeF-X
# command words at all (a missing trace FAILS the negative). Every
# other check must pass.
#
# Like tc-smoke.sh this script does NOT take the /tmp/stix-qtest-21000
# flock itself; the caller must hold it (no script does this for you).
#
# Usage: detector-smoke.sh <qemu-binary> [--flare] [--negative]
#                           [--dry-run] [--check-guard]
#   STIX_ASW_ELF            as in tc-smoke.sh
#   STIX_DETECTOR_RUN_DIR   run dir (default /tmp/stix-detector-run).
#                           Canonicalised, then anything at or above
#                           /tmp/stix-uprev-run or /tmp/stix-baseline-run
#                           (or /) is refused, as in nominal-smoke.sh.
#                           The QMP socket is <run dir>/qmp.sock and its
#                           path must stay under 108 bytes.
#   STIX_DETECTOR_SECONDS   the probe's --seconds (default 240, >= 235:
#                           the flush window ends at +230 s)
#   The probe always runs with --hold (it watches the stream to
#   --seconds after the last match, so a late forbid cannot slip by).
#   --dry-run prints the QEMU and probe command lines, boots nothing.
#   --check-guard only runs the run-dir guard and exits 0/2.
#
# Exit codes: 0 pass (or dry-run / check-guard ok), 77 SKIP (no ELF;
#             the convention for new gates), 1 a failed
#             expectation, a forbidden TM, a closed connection, a failed
#             QL, UART or trace check (in --negative: any other set of
#             failed checks), 2 environment problem.
set -euo pipefail

QEMU_BIN_ARG="${1:?usage: detector-smoke.sh <qemu-binary> \
[--flare] [--negative] [--dry-run] [--check-guard]}"
NEGATIVE=0
FLARE=0
DRY_RUN=0
CHECK_GUARD=0
for a in "${@:2}"; do
    case "$a" in
        --negative)
            [ "$NEGATIVE" -eq 0 ] || { echo "--negative given twice"; exit 2; }
            NEGATIVE=1 ;;
        --flare)
            [ "$FLARE" -eq 0 ] || { echo "--flare given twice"; exit 2; }
            FLARE=1 ;;
        --dry-run)
            [ "$DRY_RUN" -eq 0 ] || { echo "--dry-run given twice"; exit 2; }
            DRY_RUN=1 ;;
        --check-guard)
            [ "$CHECK_GUARD" -eq 0 ] || {
                echo "--check-guard given twice"; exit 2; }
            CHECK_GUARD=1 ;;
        *) echo "unknown argument: $a"; exit 2 ;;
    esac
done

ASW_ELF="${STIX_ASW_ELF:-/tmp/stix-baseline-run/ASW_em_baseline.elf}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_DIR="${STIX_DETECTOR_RUN_DIR:-/tmp/stix-detector-run}"
DURATION="${STIX_DETECTOR_SECONDS:-240}"
PROBE="$REPO_ROOT/scripts/stix/tmtc-probe.py"
QLCHECK="$REPO_ROOT/scripts/stix/ql-check.py"
TRACECHECK="$REPO_ROOT/scripts/stix/idefx-trace-check.py"
TMTC_PORT=$((20000 + $(id -u)))

# The run-dir guard of nominal-smoke.sh (review M1 there): canonical
# first (realpath -m resolves a trailing slash, //, /., a symlink or a
# relative path), then refuse the two shared run dirs, anything inside
# them, any parent of them, and /.
RUN_DIR="$(realpath -m -- "$RUN_DIR")"
SHARED_UPREV="$(realpath -m -- /tmp/stix-uprev-run)"
SHARED_BASE="$(realpath -m -- /tmp/stix-baseline-run)"
refuse_run_dir() {
    echo "refusing run dir $RUN_DIR ($1)"
    echo "a NOMINAL entry writes a flash context, so this gate needs its"
    echo "own empty run dir away from the shared ones"
    exit 2
}
if [ "$RUN_DIR" = "/" ]; then
    refuse_run_dir "it is the root directory"
fi
for shared in "$SHARED_UPREV" "$SHARED_BASE"; do
    case "$RUN_DIR" in
        "$shared" | "$shared"/*)
            refuse_run_dir "it is $shared or inside it" ;;
    esac
    case "$shared" in
        "$RUN_DIR"/*)
            refuse_run_dir "it is a parent of $shared" ;;
    esac
done
QMP_SOCK="$RUN_DIR/qmp.sock"
# sun_path is 108 bytes including the NUL
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

case "$DURATION" in
    '' | *[!0-9]*)
        echo "STIX_DETECTOR_SECONDS must be whole seconds (got: $DURATION)"
        exit 2
        ;;
esac
if [ "$DURATION" -lt 235 ]; then
    echo "STIX_DETECTOR_SECONDS must be >= 235 (the QL flush window" \
        "ends at +230 s; got: $DURATION)"
    exit 2
fi

# TC units: SpW header 40 02 00 00 + CCSDS TC on APID 0x5AC + CRC-16/
# CCITT-FALSE (poly 0x1021, init 0xFFFF, over the CCSDS packet), built
# as in nominal-smoke.sh and fault-smoke.sh; sequence counts 1..5 so
# the acks 1DACC00x are distinct. TC_CONF, TC_S20 are nominal-smoke's
# units; TC_VERIF is TC(236,6) 02 02 00 1E; TC_NOM and TC_CONF2 differ
# from nominal-smoke's only in the sequence count (and so the CRC).
TC_CONF="40 02 00 00 1D AC C0 01 00 07 19 EC 01 00 04 00 4A C1"
TC_S20="40 02 00 00 1D AC C0 02 00 09 19 EC 06 00 07 04 00 00 AF BA"
TC_VERIF="40 02 00 00 1D AC C0 03 00 09 19 EC 06 00 02 02 00 1E 8A E9"
TC_NOM="40 02 00 00 1D AC C0 04 00 07 19 EC 01 00 05 00 0E 3C"
TC_CONF2="40 02 00 00 1D AC C0 05 00 07 19 EC 01 00 04 00 D6 2E"

# D-4: the flare switch over QMP (--flare). The negative sends false at
# @135, so nothing flips but the QMP path and the timing stay the same.
qom_set() {  # path property json-value
    printf '{"execute": "qom-set", "arguments": %s' \
        "{\"path\": \"$1\", \"property\": \"$2\", \"value\": $3}}"
}
FLARE_ON=true
[ "$NEGATIVE" -eq 1 ] && FLARE_ON=false
FLARE_ARGS=()
if [ "$FLARE" -eq 1 ]; then
    FLARE_ARGS=(
        --qmp "$QMP_SOCK"
        --send-at 135
        --qmp-cmd "$(qom_set /machine/stixctrl det-flare "$FLARE_ON")"
        --send-at 195
        --qmp-cmd "$(qom_set /machine/stixctrl det-flare false)"
    )
fi

# TM(5,x) on 0x5A7, the subtype from the EID's severity nibble (1 ->
# TM(5,1), 5 -> TM(5,2), 9 -> TM(5,3)); TM(21,6) QL on 0x5DC, matched
# on the SSID (the first application byte).
PROBE_ARGS=(
    --port "$TMTC_PORT" --seconds "$DURATION" --window 20
    --dump-apid "0x5DC:$RUN_DIR/ql.dump.hex"
    --expect "0x5A7:5:3:8030@50"
    --expect "0x5A7:5:1:002200000008000000@50"
    --expect "0x5A7:5:1:14a3@50"
    --expect "0x5A7:5:1:142e@50"
    --send-at 50 --send-hex "$TC_CONF"
    --expect "0x5A1:1:1:1DACC001"
    --expect "0x5A7:5:1:00210204"
    --expect "0x5A1:1:7:1DACC001"
    --forbid "0x5A7:5:1:00210402"
    --send-at 60 --send-hex "$TC_S20"
    --expect "0x5A1:1:1:1DACC002"
    --expect "0x5A1:1:7:1DACC002"
    --send-at 62 --send-hex "$TC_VERIF"
    --expect "0x5A1:1:1:1DACC003"
    --expect "0x5A1:1:7:1DACC003"
    --send-at 70 --send-hex "$TC_NOM"
    --expect "0x5A1:1:1:1DACC004"
    --expect "0x5A7:5:1:00210405@10"
    --expect "0x5A1:1:7:1DACC004"
    --forbid "0x5A7:5:2:542e"
    --forbid "0x5A7:5:3:9438"
    --forbid "0x5A7:5:1:142c"
    --forbid "0x5A7:5:3:9433"
    --forbid "0x5A7:5:1:1427"
    --forbid "0x5A7:5:3:942e"
    --forbid "0x5A7:5:2:5437"
    --forbid "0x5A7:5:1:00210502"
    --forbid "0x5A1:1:8"
    --forbid "0x5A7:5:1:142e"
    ${FLARE_ARGS[@]+"${FLARE_ARGS[@]}"}
    --send-at 215 --send-hex "$TC_CONF2"
    --expect "0x5A1:1:1:1DACC005"
    --expect "0x5A7:5:1:00210504@10"
    --expect "0x5A1:1:7:1DACC005"
    --expect "0x5DC:21:6:1e@15"
    --expect "0x5DC:21:6:1f@15"
    --expect "0x5DC:21:6:20@15"
    --expect "0x5DC:21:6:21@15"
    --expect "0x5DC:21:6:22@15"
    --forbid "0x5A7:5:1:00210402"
    --hold
)
QEMU_ARGS=(
    -M leon3_generic -m 64M -no-reboot
    -display none -monitor none
    -qmp "unix:$QMP_SOCK,server=on,wait=off"
    -serial "file:$RUN_DIR/uart.detector.raw"
    -kernel "$ASW_ELF"
    -msg timestamp=on
    -trace "enable=stixdet_idefx_cmd,file=$RUN_DIR/idefx.trace"
)
if [ "$FLARE" -eq 0 ]; then
    # the tiers 0+1 zero sky of the D-3 checks
    QEMU_ARGS+=(-global grlib-stixctrl.det-source=zero)
    if [ "$NEGATIVE" -eq 1 ]; then
        QEMU_ARGS+=(-global grlib-stixctrl.det-idefx=off)
    fi
fi
MODE="positive"
[ "$NEGATIVE" -eq 1 ] && MODE="negative"
[ "$FLARE" -eq 1 ] && MODE="flare $MODE"

if [ "$DRY_RUN" -eq 1 ]; then
    echo "DRY RUN — the commands detector-smoke would run ($MODE):"
    printf '(cd %s && timeout %s %s ' "$RUN_DIR" "$((DURATION + 30))" \
        "$QEMU_BIN"
    printf '%q ' "${QEMU_ARGS[@]}"
    printf '> qemu-stdout.detector.log 2>&1)\n'
    printf 'python3 -u %s ' "$PROBE"
    printf '%q ' "${PROBE_ARGS[@]}"
    printf '\n'
    exit 0
fi

[ -f "$ASW_ELF" ] || {
    echo "SKIP: ASW ELF absent ($ASW_ELF); set STIX_ASW_ELF to enable"
    exit 77
}

if ss -ltn | grep -q ":${TMTC_PORT} "; then
    echo "TMTC port ${TMTC_PORT} busy — another QEMU instance running?"
    exit 2
fi

mkdir -p "$RUN_DIR"
cd "$RUN_DIR"
# Fresh flash every run: clear only this run dir's own files, by
# explicit path (the shared run dirs are refused above). The dump is
# appended to, so it must start empty.
rm -f flash.bin uart.detector.raw uart.detector.txt detector.probe.log \
      qemu-stdout.detector.log idefx.trace ql.dump.hex ql.check.log \
      ql.contrast.log qmp.sock

PROBE_PID=""
QEMU_PID=""
cleanup() {
    # `|| true`: under set -e a failed kill (process already gone) would
    # otherwise become the script's exit status
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

# The epoch before the start anchors the probe's local HH:MM:SS times.
RUN_EPOCH="$(date +%s)"
# The probe starts first: it retries the TMTC connect until QEMU binds
# the port, then stays on the connection for the whole run (--hold).
timeout "$((DURATION + 30))" python3 -u "$PROBE" "${PROBE_ARGS[@]}" \
    > detector.probe.log 2>&1 &
PROBE_PID=$!

timeout "$((DURATION + 30))" "$QEMU_BIN" "${QEMU_ARGS[@]}" \
    > qemu-stdout.detector.log 2>&1 &
QEMU_PID=$!

set +e
wait "$PROBE_PID"
PROBE_RC=$?
PROBE_PID=""
kill -0 "$QEMU_PID" 2>/dev/null
QEMU_ALIVE=$?
# D-4 cost record: QEMU's own user+sys CPU (the child of timeout)
QEMU_CPU=""
QEMU_CHILD="$(pgrep -P "$QEMU_PID" 2>/dev/null | head -1)"
if [ -n "$QEMU_CHILD" ] && [ -r "/proc/$QEMU_CHILD/stat" ]; then
    QEMU_CPU="$(awk -v hz="$(getconf CLK_TCK)" \
        '{ sub(/^.*\) /, ""); printf "user %.1f s, sys %.1f s", \
           $12 / hz, $13 / hz }' "/proc/$QEMU_CHILD/stat")"
fi
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null
QEMU_RC=$?
QEMU_PID=""
set -e

# On every path the log tail(s) come BEFORE the verdict, so the verdict
# line is always the last thing the gate prints.
if [ "$QEMU_ALIVE" -ne 0 ]; then
    tail -5 qemu-stdout.detector.log
    echo "DETECTOR-SMOKE: ERROR (QEMU died or never started; qemu rc=$QEMU_RC)"
    exit 2
fi
if [ "$PROBE_RC" -eq 124 ]; then
    tail -5 detector.probe.log
    echo "DETECTOR-SMOKE: ERROR (the probe hit its own timeout)"
    exit 2
fi
if [ "$PROBE_RC" -eq 2 ]; then
    tail -5 detector.probe.log
    echo "DETECTOR-SMOKE: ERROR (the probe rejected its arguments)"
    exit 2
fi
if grep -q "could not connect" detector.probe.log 2>/dev/null; then
    tail -5 detector.probe.log
    echo "DETECTOR-SMOKE: ERROR (the probe never connected; QEMU rc=$QEMU_RC)"
    exit 2
fi

# Every check below runs and records its name in FAILED, so the report
# is complete and --negative can compare the set.
FAILED=()

# 1. the probe (TM expectations and forbids)
if [ "$PROBE_RC" -ne 0 ]; then
    WHY="$(grep -m1 -o 'FORBIDDEN TM: .*' detector.probe.log \
        | cut -d' ' -f3-)" || true
    if [ -n "$WHY" ]; then
        WHY="forbidden: $WHY"
    else
        WHY="$(grep -o 'EXPECTATION NOT MATCHED: .*' detector.probe.log \
            | cut -d' ' -f4- | paste -sd' ')" || true
        if [ -n "$WHY" ]; then
            WHY="not matched: $WHY"
        elif grep -q '^CONNECTION CLOSED' detector.probe.log; then
            WHY="connection closed"
        else
            WHY="probe rc=$PROBE_RC; see detector.probe.log"
        fi
    fi
    tail -5 detector.probe.log
    echo "CHECK probe: FAIL ($WHY)"
    FAILED+=(probe)
else
    echo "CHECK probe: ok (every TM expectation matched, no forbid fired)"
fi

# T_nom / T_exit: the probe's arrival times of 0021 04 05 / 0021 05 04
T_NOM_HMS="$(grep -m1 'EXPECT OK: 0x5A7:5:1:00210405@10' detector.probe.log \
    | cut -d' ' -f1)" || true
T_EXIT_HMS="$(grep -m1 'EXPECT OK: 0x5A7:5:1:00210504@10' detector.probe.log \
    | cut -d' ' -f1)" || true
NOM_SECONDS=""
if [ -n "$T_NOM_HMS" ] && [ -n "$T_EXIT_HMS" ]; then
    NOM_SECONDS="$(python3 - "$T_NOM_HMS" "$T_EXIT_HMS" <<'EOF'
import sys
def s(hms):
    h, m, x = (int(v) for v in hms.split(":"))
    return h * 3600 + m * 60 + x
print((s(sys.argv[2]) - s(sys.argv[1])) % 86400)
EOF
)"
    echo "NOMINAL window: T_nom $T_NOM_HMS, T_exit $T_EXIT_HMS," \
        "$NOM_SECONDS s"
fi

[ -n "$QEMU_CPU" ] && echo "QEMU CPU ($MODE): $QEMU_CPU"

# 2. the QL flush content (zero sky, or with --flare the synthetic sky)
QL_FLUSH_ARG=--expect-zero-flush
[ "$FLARE" -eq 1 ] && QL_FLUSH_ARG=--expect-sky-flush
if [ -z "$NOM_SECONDS" ]; then
    echo "CHECK ql: FAIL (no NOMINAL window: 0021 04 05 or 0021 05 04" \
        "not seen)"
    FAILED+=(ql)
elif [ ! -s ql.dump.hex ]; then
    echo "CHECK ql: FAIL (no 0x5DC packet dumped)"
    FAILED+=(ql)
else
    set +e
    python3 "$QLCHECK" ql.dump.hex --samples \
        "$QL_FLUSH_ARG" "$NOM_SECONDS" > ql.check.log 2>&1
    QL_RC=$?
    set -e
    grep -E '^(SSID|QL-CHECK)' ql.check.log || true
    if [ "$QL_RC" -eq 0 ]; then
        echo "CHECK ql: ok ($(wc -l < ql.dump.hex) packets in ql.dump.hex)"
    else
        echo "CHECK ql: FAIL (ql-check rc=$QL_RC; see ql.check.log)"
        FAILED+=(ql)
    fi
fi

# 2b. --flare: the LC band-0 contrast across the flare switch. The
# switch times are the probe's QMP send times, relative to T_nom.
if [ "$FLARE" -eq 1 ]; then
    mapfile -t QMP_HMS < <(grep '>> qmp .*det-flare' detector.probe.log \
        | cut -d' ' -f1)
    if [ -z "$NOM_SECONDS" ] || [ "${#QMP_HMS[@]}" -ne 2 ] || \
        [ ! -s ql.dump.hex ]; then
        echo "CHECK ql-contrast: FAIL (no NOMINAL window, no dump, or" \
            "not exactly two det-flare QMP sends: ${#QMP_HMS[@]})"
        FAILED+=(ql-contrast)
    else
        read -r FLARE_ON_S FLARE_OFF_S < <(python3 -c '
import sys
def s(hms):
    h, m, x = (int(v) for v in hms.split(":"))
    return h * 3600 + m * 60 + x
t0 = s(sys.argv[1])
print(*((s(a) - t0) % 86400 for a in sys.argv[2:]))
' "$T_NOM_HMS" "${QMP_HMS[0]}" "${QMP_HMS[1]}")
        echo "flare switch: on at T_nom +$FLARE_ON_S s, off at" \
            "T_nom +$FLARE_OFF_S s"
        set +e
        python3 "$QLCHECK" ql.dump.hex \
            --lc-contrast "$FLARE_ON_S" "$FLARE_OFF_S" \
            > ql.contrast.log 2>&1
        QLC_RC=$?
        set -e
        grep -E '^QL-CHECK' ql.contrast.log || true
        if [ "$QLC_RC" -eq 0 ]; then
            echo "CHECK ql-contrast: ok"
        else
            echo "CHECK ql-contrast: FAIL (ql-check rc=$QLC_RC; see" \
                "ql.contrast.log)"
            FAILED+=(ql-contrast)
        fi
    fi
fi

# 3. the UART, NULs stripped
tr -d '\000' < uart.detector.raw > uart.detector.txt
UART_BASE_OK=1
if ! grep -q "init: SAFE mode" uart.detector.txt; then
    echo "CHECK uart: the UART has no 'init: SAFE mode' banner"
    UART_BASE_OK=0
fi
if grep -Eq 'storing new context[01] failed' uart.detector.txt; then
    grep -Em1 'storing new context[01] failed' uart.detector.txt
    echo "CHECK uart: a flash context write failed"
    UART_BASE_OK=0
fi
for pat in "packet sequence error" "RotBuf overrun" "UNKNOWN RB-BLOCK" \
    "TOO MANY RB-BLOCKS"; do
    if grep -q "$pat" uart.detector.txt; then
        grep -m1 "$pat" uart.detector.txt
        echo "CHECK uart: '$pat' on the UART"
        UART_BASE_OK=0
    fi
done
if [ "$UART_BASE_OK" -eq 1 ]; then
    echo "CHECK uart: ok (SAFE banner, no context, ROTB or sequence error)"
else
    FAILED+=(uart)
fi
IDEFX_W="$(grep -c 'XrayDetFDIR.c:731' uart.detector.txt || true)"
IDEFX_R="$(grep -c 'failed to read register' uart.detector.txt || true)"
echo "IDeF-X UART errors: $IDEFX_W write timeouts (XrayDetFDIR.c:731)," \
    "$IDEFX_R register read failures (failed to read register)"
if [ "$IDEFX_W" -eq 0 ] && [ "$IDEFX_R" -eq 0 ]; then
    echo "CHECK uart-idefx: ok (no IDeF-X error)"
else
    echo "CHECK uart-idefx: FAIL ($IDEFX_W write timeouts, $IDEFX_R read" \
        "failures; expected 0)"
    FAILED+=(uart-idefx)
fi

# 4. the ASIC verification runs in the trace (idefx-trace-check.py).
# Positive: >= 3 runs of exactly 384. Negative: the trace must exist
# and hold no IDeF-X command word at all (det-idefx=off fires no
# stixdet_idefx_cmd); only then does trace-verif count as failed as
# planned. A missing or unreadable trace fails the gate in both modes.
# The --flare modes keep det-idefx on, so even --flare --negative takes
# the positive branch here.
IDEFX_NEG=0
[ "$NEGATIVE" -eq 1 ] && [ "$FLARE" -eq 0 ] && IDEFX_NEG=1
TRACE_ARGS=(idefx.trace --run-epoch "$RUN_EPOCH")
[ -n "${T_NOM_HMS:-}" ] && TRACE_ARGS+=(--t-nom "$T_NOM_HMS")
[ -n "${T_EXIT_HMS:-}" ] && TRACE_ARGS+=(--t-exit "$T_EXIT_HMS")
[ "$IDEFX_NEG" -eq 1 ] && TRACE_ARGS+=(--expect-none)
set +e
python3 "$TRACECHECK" "${TRACE_ARGS[@]}"
TRACE_RC=$?
set -e
if [ "$IDEFX_NEG" -eq 1 ]; then
    if [ "$TRACE_RC" -eq 0 ]; then
        echo "CHECK trace-verif: FAIL as planned (the trace is empty:" \
            "no IDeF-X command words at all)"
        FAILED+=(trace-verif)
    else
        echo "CHECK trace-verif: FAIL (rc=$TRACE_RC; the negative wants" \
            "an existing, empty trace)"
        FAILED+=(trace-not-empty)
    fi
elif [ "$TRACE_RC" -eq 0 ]; then
    echo "CHECK trace-verif: ok"
else
    echo "CHECK trace-verif: FAIL (rc=$TRACE_RC)"
    FAILED+=(trace-verif)
fi

GOT="${FAILED[*]:-}"
if [ "$FLARE" -eq 1 ] && [ "$NEGATIVE" -eq 1 ]; then
    # PASS only when the missing flip is all that fails: the LC
    # contrast check; the sky, probe, UART and trace checks must pass.
    if [ "$GOT" = "ql-contrast" ]; then
        echo "DETECTOR-SMOKE (flare negative): PASS (failed exactly as" \
            "planned: no flare switch, no LC contrast; everything else" \
            "passed)"
        exit 0
    fi
    echo "DETECTOR-SMOKE (flare negative): FAIL (failed checks:" \
        "'${GOT}', want 'ql-contrast')"
    exit 1
fi
if [ "$NEGATIVE" -eq 1 ]; then
    # PASS only on the planned failure: exactly the UART IDeF-X check
    # (all 384 NOMINAL-entry writes timed out, and the verification
    # runs logged their 12 register read failures each, >= 3 runs) and
    # the trace check; the probe, the QL flush and the other UART
    # checks must still pass.
    WANT="uart-idefx trace-verif"
    if [ "$GOT" = "$WANT" ] && [ "$IDEFX_W" -eq 384 ] && \
        [ "$IDEFX_R" -ge 36 ] && [ $((IDEFX_R % 12)) -eq 0 ]; then
        echo "DETECTOR-SMOKE (negative): PASS (failed exactly as planned:" \
            "$IDEFX_W write timeouts, $IDEFX_R read failures =" \
            "$((IDEFX_R / 12)) verification runs, no IDeF-X command" \
            "words at all; the trace is empty)"
        exit 0
    fi
    echo "DETECTOR-SMOKE (negative): FAIL (failed checks: '${GOT}'," \
        "want '$WANT' with 384 write timeouts and 12 x >= 3 read" \
        "failures; got $IDEFX_W and $IDEFX_R)"
    exit 1
fi
# the D-3 positive keeps its verdict wording; --flare names its mode
TAG="DETECTOR-SMOKE"
[ "$FLARE" -eq 1 ] && TAG="DETECTOR-SMOKE ($MODE)"
if [ -n "$GOT" ]; then
    echo "$TAG: FAIL (failed checks: $GOT)"
    exit 1
fi
echo "$TAG: PASS"
exit 0
