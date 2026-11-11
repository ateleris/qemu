#!/usr/bin/env bash
# NOMINAL smoke: the SAFE -> CONFIGURATION -> NOMINAL gate (P3-2).
# Boots the STIX model on a FRESH flash — an empty run dir, so the
# device creates a new sparse flash.bin — because ANY NOMINAL entry
# writes a flash context (ContextMgmt_deinit, OpModeMgmt.c:317): a
# reused flash.bin would silently change every later boot (S20, limits,
# LV/HV defaults). It drives the mode changes with TC(236,1) /
# TC(236,6) and expects the PUS reports (recon-phase3-nominal §4):
#   connect:  TM(5,3) 0x8030 (no context file: proves the fresh flash),
#             TM(5,1) 0x0022 (the P3-1 boot success report),
#             TM(5,1) 0x14A3 (FDIR enable), TM(5,1) 0x142E (the boot
#             TC20 timer, harmless in SAFE)
#   CONF:     TM(1,1), TM(5,1) 0021 02 04, TM(1,7); forbid 0021 04 02
#   S20=0:    TM(1,1), TM(1,7) — the FSW sends two TM(1,7) here
#             (HWConfig.c:1044 and TcService236.c:213); one is
#             expected, never "exactly one"
#   NOMINAL:  TM(1,1), TM(5,1) 0021 04 05, TM(1,7), SID2 (0x5A4) with
#             the mode nibble 5; forbids 0021 05 02 / TM(1,8) / 142E /
#             TM(5,3) 0x9449
#   TC(17,1): TM(1,1), TM(17,2), TM(1,7), a heartbeat (0x5E5)
# --negative drops the S20=0 send: the TC20 reaction then fires about
# 30 s after NOMINAL entry, and the gate passes only when the fallback
# is seen — both TM(5,1) 0x142E and 0021 05 02 (NOMINAL -> SAFE; the
# probe does not check their order).
#
# Expected NOMINAL-entry side effects, NOT failures: one TM(5,2) 0x542C
# (no LUTs in flash; expected but NOT checked by this gate — seen in the
# p3probe4 experiment, and the recon marks it inferred), LUTMgmt
# messages, and SID2 showing LV/HV/SpW off (POWERSTAT = 0, D-P3a). The
# UART is checked after the run: "init: SAFE mode" present, no
# "storing new context[01] failed", and NO IDeF-X write timeout
# (XrayDetFDIR.c:731) in either mode: since detector plan D-2 the model
# answers the IDeF-X interface, so the 384 NOMINAL-entry register
# writes complete; any count other than 0 is a FAIL (before D-2 there
# were exactly 384, a WARN otherwise). Zero timeouts alone would also
# pass if NOMINAL entry returned before the writes (OpModeMgmt.c:576),
# so QEMU runs with -trace stixdet_idefx_cmd and the gate also requires
# positive evidence in both modes: at least 32 delivered write address
# blocks (the word after a delivered SCINITSEQ, tracked per GrpAddr,
# with R/W = 0) for EACH register 1..12, i.e. the 32 ASICs x 12
# registers of XrayDetFDIR_resetASICregistersToDefault. R3 also gets
# the RCR state-0 write (RateCtrl.c:575), so it counts 64 here.
#
# Like tc-smoke.sh this script does NOT take the /tmp/stix-qtest-21000
# flock itself; the caller must hold it (no script does this for you).
#
# Usage: nominal-smoke.sh <qemu-binary> [--negative] [--dry-run]
#                          [--check-guard]
#   STIX_ASW_ELF             as in tc-smoke.sh
#   STIX_NOMINAL_RUN_DIR     run dir (default /tmp/stix-nominal-run).
#                            Canonicalised, then anything at or above
#                            /tmp/stix-uprev-run or
#                            /tmp/stix-baseline-run is refused: those
#                            flashes must never see a NOMINAL entry.
#   STIX_NOMINAL_SECONDS     the probe's --seconds (default 190)
#   --dry-run prints the QEMU and probe command lines, boots nothing.
#   --check-guard only runs the run-dir guard and exits 0/2.
#
# Exit codes: 0 pass (or SKIP / dry-run / check-guard ok), 1 a failed
#             expectation, a forbidden TM, a closed connection or a
#             failed UART check, 2 environment problem.
set -euo pipefail

QEMU_BIN_ARG="${1:?usage: nominal-smoke.sh <qemu-binary> \
[--negative] [--dry-run] [--check-guard]}"
NEGATIVE=0
DRY_RUN=0
CHECK_GUARD=0
for a in "${@:2}"; do
    case "$a" in
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
        *) echo "unknown argument: $a"; exit 2 ;;
    esac
done

ASW_ELF="${STIX_ASW_ELF:-/tmp/stix-baseline-run/ASW_em_baseline.elf}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_DIR="${STIX_NOMINAL_RUN_DIR:-/tmp/stix-nominal-run}"
DURATION="${STIX_NOMINAL_SECONDS:-190}"
PROBE="$REPO_ROOT/scripts/stix/tmtc-probe.py"
TMTC_PORT=$((20000 + $(id -u)))

# The shared flashes must never be touched (review M1): a NOMINAL entry
# writes a flash context, and the old plain string compare was bypassed
# by a trailing slash, //, /., a symlink or a relative path. Canonical
# first (realpath -m also fixes -serial file:$RUN_DIR/... after the cd
# for a relative STIX_NOMINAL_RUN_DIR), then refuse the two shared run
# dirs, anything inside them, any parent of them, and /.
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

if [ "$CHECK_GUARD" -eq 1 ]; then
    echo "GUARD OK: run dir $RUN_DIR (canonical)"
    exit 0
fi

QEMU_BIN="$(realpath -e "$QEMU_BIN_ARG")" || {
    echo "qemu binary not found: $QEMU_BIN_ARG"; exit 2; }
[ -x "$QEMU_BIN" ] || { echo "not executable: $QEMU_BIN"; exit 2; }

for v in "$DURATION"; do
    case "$v" in
        '' | *[!0-9]*)
            echo "STIX_NOMINAL_SECONDS must be whole seconds (got: $v)"
            exit 2
            ;;
    esac
done

# TC units, SpW header + CCSDS + CRC-16/CCITT-FALSE already computed
# (the FSW does not check the sequence count; these are seq 1..4 so the
# acks 1DACC00x are distinct):
TC_CONF="40 02 00 00 1D AC C0 01 00 07 19 EC 01 00 04 00 4A C1"
TC_S20="40 02 00 00 1D AC C0 02 00 09 19 EC 06 00 07 04 00 00 AF BA"
TC_NOM="40 02 00 00 1D AC C0 03 00 07 19 EC 01 00 05 00 BF 97"
TC17="40 02 00 00 1D AC C0 04 00 05 19 11 01 00 16 B8"

# The connect group (before any send) proves the flash is fresh and
# pins the P3-1 boot report; @50 windows from the connect.
CONNECT_ARGS=(
    --expect "0x5A7:5:3:8030@50"
    --expect "0x5A7:5:1:002200000008000000@50"
    --expect "0x5A7:5:1:14a3@50"
    --expect "0x5A7:5:1:142e@50"
)
# The CONF group (both modes): mode 4 manual, forbid CONF->SAFE.
CONF_ARGS=(
    --send-at 50 --send-hex "$TC_CONF"
    --expect "0x5A1:1:1:1DACC001"
    --expect "0x5A7:5:1:00210204"
    --expect "0x5A1:1:7:1DACC001"
    --forbid "0x5A7:5:1:00210402"
)

if [ "$NEGATIVE" -eq 1 ]; then
    # No S20=0: NOMINAL entry re-arms the TC20 timer, so the fallback —
    # both 0x142E and 0021 05 02, in either order — must be seen; no
    # forbids here and no TC17 group.
    PROBE_ARGS=(
        --port "$TMTC_PORT" --seconds "$DURATION" --window 20
        "${CONNECT_ARGS[@]}"
        "${CONF_ARGS[@]}"
        --send-at 70 --send-hex "$TC_NOM"
        --expect "0x5A1:1:1:1DACC003"
        --expect "0x5A7:5:1:00210405"
        --expect "0x5A1:1:7:1DACC003"
        --expect "0x5A7:5:1:142e@50"
        --expect "0x5A7:5:1:00210502@50"
        --hold
    )
else
    PROBE_ARGS=(
        --port "$TMTC_PORT" --seconds "$DURATION" --window 20
        "${CONNECT_ARGS[@]}"
        "${CONF_ARGS[@]}"
        --send-at 60 --send-hex "$TC_S20"
        --expect "0x5A1:1:1:1DACC002"
        --expect "0x5A1:1:7:1DACC002"
        --send-at 70 --send-hex "$TC_NOM"
        --expect "0x5A1:1:1:1DACC003"
        --expect "0x5A7:5:1:00210405"
        --expect "0x5A1:1:7:1DACC003"
        --expect "0x5A4:3:25:0215@85"
        --forbid "0x5A7:5:1:00210502"
        --forbid "0x5A1:1:8"
        --forbid "0x5A7:5:1:142e"
        --forbid "0x5A7:5:3:9449"
        --send-at 165 --send-hex "$TC17"
        --expect "0x5A1:1:1:1DACC004"
        --expect "0x5A7:17:2"
        --expect "0x5A1:1:7:1DACC004"
        --expect "0x5E5:3:25"
        --hold
    )
fi

if [ "$DRY_RUN" -eq 1 ]; then
    echo "DRY RUN — the commands nominal-smoke would run (negative=$NEGATIVE):"
    echo "(cd $RUN_DIR && timeout $((DURATION + 30)) $QEMU_BIN \
-M leon3_generic -m 64M -no-reboot -display none -monitor none \
-serial file:$RUN_DIR/uart.nominal.raw -kernel $ASW_ELF \
-trace stixdet_idefx_cmd > qemu-stdout.nominal.log 2>&1)"
    printf 'python3 -u %s ' "$PROBE"
    printf '%q ' "${PROBE_ARGS[@]}"
    printf '\n'
    exit 0
fi

[ -f "$ASW_ELF" ] || {
    echo "SKIP: ASW ELF absent ($ASW_ELF); set STIX_ASW_ELF to enable"
    exit 0
}
# the D-2 IDeF-X evidence check uses gawk (strtonum, and, rshift)
command -v gawk > /dev/null || { echo "gawk is required"; exit 2; }

if ss -ltn | grep -q ":${TMTC_PORT} "; then
    echo "TMTC port ${TMTC_PORT} busy — another QEMU instance running?"
    exit 2
fi

mkdir -p "$RUN_DIR"
cd "$RUN_DIR"
# Fresh flash every run: clear only this run dir's own files, by
# explicit path (the shared run dirs are refused above).
rm -f flash.bin uart.nominal.raw uart.nominal.txt \
      nominal.probe.log qemu-stdout.nominal.log

PROBE_PID=""
QEMU_PID=""
cleanup() {
    # `|| true`: under set -e a failed kill (process already gone) would
    # otherwise become the script's exit status; the wait reaps the
    # killed child so it does not linger
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

# The probe starts first: it retries connecting until QEMU binds the
# TMTC port, then stays on the connection for the whole run (--hold).
timeout "$((DURATION + 30))" python3 -u "$PROBE" "${PROBE_ARGS[@]}" \
    > nominal.probe.log 2>&1 &
PROBE_PID=$!

timeout "$((DURATION + 30))" "$QEMU_BIN" \
    -M leon3_generic -m 64M -no-reboot \
    -display none -monitor none \
    -serial file:"$RUN_DIR/uart.nominal.raw" \
    -kernel "$ASW_ELF" -trace stixdet_idefx_cmd \
    > qemu-stdout.nominal.log 2>&1 &
QEMU_PID=$!

# --hold keeps the probe on the socket until --seconds; the verdict
# comes at the end.
set +e
wait "$PROBE_PID"
PROBE_RC=$?
kill -0 "$QEMU_PID" 2>/dev/null
QEMU_ALIVE=$?
kill "$QEMU_PID" 2>/dev/null || true
# review m5: capture the real wait status (a `|| true` would zero it)
wait "$QEMU_PID" 2>/dev/null
QEMU_RC=$?
QEMU_PID=""
set -e

# review m4: on every path the log tail(s) come BEFORE the verdict, so
# the verdict line is always the last thing the gate prints.
if [ "$QEMU_ALIVE" -ne 0 ]; then
    tail -5 qemu-stdout.nominal.log
    echo "NOMINAL-SMOKE: ERROR (QEMU died or never started; qemu rc=$QEMU_RC)"
    exit 2
fi
if [ "$PROBE_RC" -eq 124 ]; then
    tail -5 nominal.probe.log
    echo "NOMINAL-SMOKE: ERROR (the probe hit its own timeout)"
    exit 2
fi
if grep -q "could not connect" nominal.probe.log 2>/dev/null; then
    tail -5 nominal.probe.log
    echo "NOMINAL-SMOKE: ERROR (the probe never connected; QEMU rc=$QEMU_RC)"
    exit 2
fi
if [ "$PROBE_RC" -ne 0 ]; then
    # review m6: the reason must say which kind of failure it was
    WHY="$(grep -m1 -o 'FORBIDDEN TM: .*' nominal.probe.log \
        | cut -d' ' -f3-)" || true
    if [ -n "$WHY" ]; then
        WHY="forbidden: $WHY"
    else
        WHY="$(grep -m1 -o 'EXPECTATION NOT MATCHED: .*' nominal.probe.log \
            | cut -d' ' -f4-)" || true
        if [ -n "$WHY" ]; then
            WHY="not matched: $WHY"
        elif grep -q '^CONNECTION CLOSED' nominal.probe.log; then
            WHY="connection closed"
        else
            WHY="probe rc=$PROBE_RC; see nominal.probe.log"
        fi
    fi
    tail -5 nominal.probe.log
    echo "NOMINAL-SMOKE: FAIL ($WHY)"
    exit 1
fi

# The probe passed: every expectation matched (in negative mode that
# includes the NOMINAL->SAFE fallback) and no forbid fired. Now the
# UART checks, NULs stripped.
tr -d '\000' < uart.nominal.raw > uart.nominal.txt
if ! grep -q "init: SAFE mode" uart.nominal.txt; then
    echo "NOMINAL-SMOKE: FAIL (the UART has no 'init: SAFE mode' banner)"
    exit 1
fi
if grep -Eq 'storing new context[01] failed' uart.nominal.txt; then
    grep -Em1 'storing new context[01] failed' uart.nominal.txt
    echo "NOMINAL-SMOKE: FAIL (a flash context write failed)"
    exit 1
fi
# D-2: the IDeF-X interface is modelled, so the NOMINAL-entry register
# writes must not time out (both modes enter NOMINAL)
IDEFX_N="$(grep -c 'XrayDetFDIR.c:731' uart.nominal.txt || true)"
if [ "$IDEFX_N" -ne 0 ]; then
    grep -m1 'XrayDetFDIR.c:731' uart.nominal.txt
    echo "NOMINAL-SMOKE: FAIL ($IDEFX_N IDeF-X write timeouts," \
        "XrayDetFDIR.c:731; expected 0)"
    exit 1
fi
echo "IDeF-X write timeouts (XrayDetFDIR.c:731): 0 (expected)"
# D-2 positive evidence: the per-register delivered write address blocks
# (stixdet_idefx_cmd "grp G word W delivered_grp_mask M ..."; SCINITSEQ
# slow control is 0xbfd30/0xbfd70/0xbfdb0, a write address block
# 0x80000 | reg << 7)
IDEFX_W="$(gawk '
    /stixdet_idefx_cmd/ {
        g = $4; w = strtonum($6); m = strtonum($8)
        if (m != 0 && prev[g] != "" && and(w, 0xFC07F) == 0x80000) {
            r = and(rshift(w, 7), 0x7F)
            if (r >= 1 && r <= 12) { n[r]++ }
        }
        sc = (m != 0 && (w == 0xbfd30 || w == 0xbfd70 || w == 0xbfdb0))
        prev[g] = sc ? "init" : ""
    }
    END {
        ok = 1; s = ""
        for (r = 1; r <= 12; r++) {
            s = s " R" r "=" (n[r] + 0)
            if (n[r] < 32) { ok = 0 }
        }
        print (ok ? "OK" : "LOW") s
    }' qemu-stdout.nominal.log)"
case "$IDEFX_W" in
    OK*)
        echo "IDeF-X delivered write address blocks:${IDEFX_W#OK}" ;;
    *)
        echo "NOMINAL-SMOKE: FAIL (IDeF-X register writes not seen at" \
            "NOMINAL entry, want >= 32 per register:${IDEFX_W#LOW})"
        exit 1 ;;
esac
if [ "$NEGATIVE" -eq 1 ]; then
    echo "NOMINAL-SMOKE (negative): PASS"
else
    echo "NOMINAL-SMOKE: PASS"
fi
exit 0