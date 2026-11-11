#!/usr/bin/env bash
# FAULT smoke: the SAFE-mode fault gate (P4-3). Boots the STIX model on
# a FRESH flash (an empty run dir: TMBuffer flash backups during a link
# outage write flash, and the shared flashes must stay untouched), with
# a QMP socket in the run dir, and proves the FSW's fault detection
# reacts as its code says. Times are seconds from the TMTC connect:
#   connect: TM(5,1) 0x14A3 (FDIR enabled, ~+14 s) within 30 s — the
#            TC(236,10) below must come AFTER it: FDIR_config sets
#            auto_fdir_protection_prevented (FDIR.c:1022), so a config
#            TC before 0x14A3 cancels the automatic enable (0x14A3 then
#            never comes, so the gate fails loudly, never silently).
#            The 35 s first send leaves ~20 s for a slow host. Forbids
#            TM(1,2)/TM(1,8) (a rejected or failed TC) for the run.
#   @35  TC(236,10) 00080000 22 00000003 (1V5_V consecutive count = 3)
#        and @37 TC(236,10) 00080000 01 0000079F (FDIR_HK_VOLTAGE_LINE_
#        CHECK, 1V5 low limit = 1951): TM(1,1), TM(1,7) each within 5 s
#        (they come in the same second)
#   @40  QMP adc-inject[24] (DPU_1V5_V) = 1800: TM(5,3) 9449 07 0708
#        079F (id 7, value, limit) and TM(5,4) D420 00080000, 10 s
#   @55  clear it (4294967295)
#   @58  TC(236,10) 00100000 22 00000003 (PSU_T consecutive count = 3)
#        and @60 TC(236,10) 00100000 02 00000ED8 (FDIR_HK_TEMP_LINE_
#        CHECK, PSU_T upper limit = 3800): TM(1,1), TM(1,7) each, 5 s
#   @65  adc-inject[40] (PSU_T) = 3900: 9449 17 0F3C 0ED8 and
#        D420 00100000, 10 s
#   @80  clear it
#   @85  stixspw timecodes=false: TM(5,2) 0x542B within 90 s (the FSW
#        time-code timeout is 60 s, ParamMgmtDef.h:206; seen ~61 s);
#        from here any TM(5,3) 0x9449 fails the run (the limits stay
#        tightened, the nominal 2048 / 2300 are inside them)
#   @180 timecodes=true: TM(5,1) 0x1429 within 10 s
#   hold to +200 s.
# Why the count TCs: the SW-limit TM goes out when the consecutive
# out-of-limit count EQUALS hkLineLimits[line].count (HKMonitor.c:467;
# checks every 1 s). ParamMgmtDef.h gives 3 (value, range 1..255), but
# ParamMgmt_init (ParamMgmt.c:34-36) feeds HKMonitor_updateParameter
# the value, then the min, then the max of every parameter, and the
# count branch (HKMonitor.c:960-964) ignores the type — so the max,
# 255, wins: after boot every line needs 255 consecutive seconds
# (FSW bug; seen live: hkLineLimits.count = 0xff for all lines in the
# gate ELF, paramsOutOfBoundsCount[7] counting past 3 with no TM). The
# count TC goes through ParamMgmt_setParamValue (type OTHER) and sets
# the count to 3 (FDIR.c config table, paramType 0x22 = the
# *_CONSECUTIVE_MEASUREMENTS enum position 34 in both enums, allowed
# 1..255 by TCVerifDynamic.c:511/562). Then the TM comes ~3 s after
# the injection; 10 s is the margin. 0x142E (the boot-armed TC20 timer,
# ~+29 s) is NOT forbidden: it is a no-op in SAFE. Each trip signals a
# move to SAFE, harmless in SAFE; the gate stays in SAFE.
# --negative injects 2048 (inside 1951..4095) at @40 instead of 1800:
# the gate then passes only when the probe FAILS with exactly the two
# @40 expectations (9449 07..., D420 00080000) not matched — proof the
# gate detects a missing reaction, and that everything before @40
# (0x14A3, the first two TC(236,10) acks — their 5 s windows expire
# before the run ends — and the QMP connect) still passed.
#
# Like tc-smoke.sh this script does NOT take the /tmp/stix-qtest-21000
# flock itself; the caller must hold it (no script does this for you).
#
# Usage: fault-smoke.sh <qemu-binary> [--negative] [--dry-run]
#                        [--check-guard]
#   STIX_ASW_ELF           as in tc-smoke.sh
#   STIX_FAULT_RUN_DIR     run dir (default /tmp/stix-fault-run).
#                          Canonicalised, then anything at or above
#                          /tmp/stix-uprev-run or /tmp/stix-baseline-run
#                          (or /) is refused, as in nominal-smoke.sh.
#                          The QMP socket is <run dir>/qmp.sock and its
#                          path must stay under 108 bytes.
#   STIX_FAULT_SECONDS     the probe's --seconds (default 200)
#   --dry-run prints the QEMU and probe command lines, boots nothing.
#   --check-guard only runs the run-dir guard and exits 0/2.
#
# Exit codes: 0 pass (or SKIP / dry-run / check-guard ok), 1 a failed
#             expectation, a forbidden TM, a QMP error, a closed
#             connection or a failed UART check, 2 environment problem.
set -euo pipefail

QEMU_BIN_ARG="${1:?usage: fault-smoke.sh <qemu-binary> \
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
RUN_DIR="${STIX_FAULT_RUN_DIR:-/tmp/stix-fault-run}"
DURATION="${STIX_FAULT_SECONDS:-200}"
PROBE="$REPO_ROOT/scripts/stix/tmtc-probe.py"
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
    echo "a link outage writes flash (TMBuffer backups), so this gate"
    echo "needs its own empty run dir away from the shared ones"
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
        echo "STIX_FAULT_SECONDS must be whole seconds (got: $DURATION)"
        exit 2
        ;;
esac
if [ "$DURATION" -lt 200 ] && [ "$NEGATIVE" -eq 0 ]; then
    echo "STIX_FAULT_SECONDS must be >= 200 (the 0x1429 window ends" \
        "at +165 s; got: $DURATION)"
    exit 2
fi

# TC units, SpW header 40 02 00 00 + CCSDS TC + CRC-16/CCITT-FALSE
# (poly 0x1021, init 0xFFFF, over the CCSDS packet from 1D AC up to the
# application data), as the other gates' TCs: APID 0x5AC (1D AC), seq
# flags C0 + count (1..4: distinct acks 1DACC001..4), length 0x000E
# (= 14, the TCData.c:38 length for TC(236,10)), DFH 19 EC 0A 00 (PUS
# v1, ack flags 9 = acceptance + completion -> TM(1,1) and TM(1,7),
# service 236, subtype 10, source 0), application data fdirMask u32,
# paramType u8, value u32 (TCData.h:151-153). Computed with:
#   crc=0xFFFF; for b in sp: crc^=b<<8; 8x: crc=(crc<<1)^0x1021 if
#   crc&0x8000 else crc<<1 (16 bit)
# — the same routine reproduces TC_CONF/TC_S20/TC_NOM/TC17 of
# nominal-smoke.sh and the probe self-test's TC(17,1) units exactly.
# paramType is the FDIR.h enum position: FDIR_HK_VOLTAGE_LINE_1V5_LOW_
# LIMIT = 1 (FDIR.h:169), FDIR_HK_TEMP_LINE_PSU_UPP_LIMIT = 2
# (FDIR.h:236), *_1V5_CONSECUTIVE_MEASUREMENTS / *_PSU_CONSECUTIVE_
# MEASUREMENTS = 34 = 0x22 (FDIR.h:202, :268).
TC_1V5_CNT="40 02 00 00 1D AC C0 01 00 0E 19 EC 0A 00 00 08 00 00 22 00 00 00 03 61 2D"
TC_1V5_LOW="40 02 00 00 1D AC C0 02 00 0E 19 EC 0A 00 00 08 00 00 01 00 00 07 9F B4 A4"
TC_PSU_CNT="40 02 00 00 1D AC C0 03 00 0E 19 EC 0A 00 00 10 00 00 22 00 00 00 03 02 2A"
TC_PSU_UPP="40 02 00 00 1D AC C0 04 00 0E 19 EC 0A 00 00 10 00 00 02 00 00 0E D8 3B 18"

qom_set() {  # path property json-value
    printf '{"execute": "qom-set", "arguments": {"path": "%s", "property": "%s", "value": %s}}' \
        "$1" "$2" "$3"
}
CLEAR=4294967295   # 0xFFFFFFFF, "no override"; -1 is rejected (§26)
if [ "$NEGATIVE" -eq 1 ]; then
    V1V5=2048      # the nominal value, inside 1951..4095: no trip
else
    V1V5=1800      # 0x708 < 1951: trips the low limit
fi

# TM(5,x) events are on APID 0x5A7; the probe matches the application
# data (EID first) as a prefix. Payloads: TM_5.c sizes 0x122 for 0x9449
# (id u8, value u16, limit u16; utils.c:576, high nibble first),
# 0xD420 one u32 mask (TM_5.c:422), 0x542B / 0x1429 none.
PROBE_ARGS=(
    --port "$TMTC_PORT" --seconds "$DURATION" --window 20
    --qmp "$QMP_SOCK"
    --expect "0x5A7:5:1:14a3@30"
    --forbid "0x5A1:1:2"
    --forbid "0x5A1:1:8"
    --send-at 35 --send-hex "$TC_1V5_CNT"
    --expect "0x5A1:1:1:1DACC001@5"
    --expect "0x5A1:1:7:1DACC001@5"
    --send-at 37 --send-hex "$TC_1V5_LOW"
    --expect "0x5A1:1:1:1DACC002@5"
    --expect "0x5A1:1:7:1DACC002@5"
    --send-at 40 --qmp-cmd "$(qom_set /machine/stixctrl 'adc-inject[24]' "$V1V5")"
    --expect "0x5A7:5:3:9449070708079f@10"
    --expect "0x5A7:5:4:d42000080000@10"
    --send-at 55 --qmp-cmd "$(qom_set /machine/stixctrl 'adc-inject[24]' "$CLEAR")"
    --send-at 58 --send-hex "$TC_PSU_CNT"
    --expect "0x5A1:1:1:1DACC003@5"
    --expect "0x5A1:1:7:1DACC003@5"
    --send-at 60 --send-hex "$TC_PSU_UPP"
    --expect "0x5A1:1:1:1DACC004@5"
    --expect "0x5A1:1:7:1DACC004@5"
    --send-at 65 --qmp-cmd "$(qom_set /machine/stixctrl 'adc-inject[40]' 3900)"
    --expect "0x5A7:5:3:9449170f3c0ed8@10"
    --expect "0x5A7:5:4:d42000100000@10"
    --send-at 80 --qmp-cmd "$(qom_set /machine/stixctrl 'adc-inject[40]' "$CLEAR")"
    --send-at 85 --qmp-cmd "$(qom_set /machine/stixspw timecodes false)"
    --expect "0x5A7:5:2:542b@90"
    --forbid "0x5A7:5:3:9449"
    --send-at 180 --qmp-cmd "$(qom_set /machine/stixspw timecodes true)"
    --expect "0x5A7:5:1:1429@10"
    --hold
)

if [ "$DRY_RUN" -eq 1 ]; then
    echo "DRY RUN — the commands fault-smoke would run (negative=$NEGATIVE):"
    echo "(cd $RUN_DIR && timeout $((DURATION + 30)) $QEMU_BIN \
-M leon3_generic -m 64M -no-reboot -display none -monitor none \
-qmp unix:$QMP_SOCK,server=on,wait=off \
-serial file:$RUN_DIR/uart.fault.raw -kernel $ASW_ELF \
> qemu-stdout.fault.log 2>&1)"
    printf 'python3 -u %s ' "$PROBE"
    printf '%q ' "${PROBE_ARGS[@]}"
    printf '\n'
    exit 0
fi

[ -f "$ASW_ELF" ] || {
    echo "SKIP: ASW ELF absent ($ASW_ELF); set STIX_ASW_ELF to enable"
    exit 0
}

if ss -ltn | grep -q ":${TMTC_PORT} "; then
    echo "TMTC port ${TMTC_PORT} busy — another QEMU instance running?"
    exit 2
fi

mkdir -p "$RUN_DIR"
cd "$RUN_DIR"
# Fresh flash every run: clear only this run dir's own files, by
# explicit path (the shared run dirs are refused above).
rm -f flash.bin uart.fault.raw uart.fault.txt fault.probe.log \
      qemu-stdout.fault.log qmp.sock

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

# The probe starts first: it retries the TMTC connect until QEMU binds
# the port, and connects to the QMP socket at its first --qmp-cmd.
timeout "$((DURATION + 30))" python3 -u "$PROBE" "${PROBE_ARGS[@]}" \
    > fault.probe.log 2>&1 &
PROBE_PID=$!

timeout "$((DURATION + 30))" "$QEMU_BIN" \
    -M leon3_generic -m 64M -no-reboot \
    -display none -monitor none \
    -qmp "unix:$QMP_SOCK,server=on,wait=off" \
    -serial file:"$RUN_DIR/uart.fault.raw" \
    -kernel "$ASW_ELF" > qemu-stdout.fault.log 2>&1 &
QEMU_PID=$!

set +e
wait "$PROBE_PID"
PROBE_RC=$?
PROBE_PID=""
kill -0 "$QEMU_PID" 2>/dev/null
QEMU_ALIVE=$?
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null
QEMU_RC=$?
QEMU_PID=""
set -e

# On every path the log tail(s) come BEFORE the verdict, so the verdict
# line is always the last thing the gate prints.
if [ "$QEMU_ALIVE" -ne 0 ]; then
    tail -5 qemu-stdout.fault.log
    echo "FAULT-SMOKE: ERROR (QEMU died or never started; qemu rc=$QEMU_RC)"
    exit 2
fi
if [ "$PROBE_RC" -eq 124 ]; then
    tail -5 fault.probe.log
    echo "FAULT-SMOKE: ERROR (the probe hit its own timeout)"
    exit 2
fi
if [ "$PROBE_RC" -eq 2 ]; then
    tail -5 fault.probe.log
    echo "FAULT-SMOKE: ERROR (the probe rejected its arguments)"
    exit 2
fi
if grep -q "QMP ERROR: cannot connect" fault.probe.log 2>/dev/null; then
    tail -5 fault.probe.log
    echo "FAULT-SMOKE: ERROR (no QMP connection; QEMU rc=$QEMU_RC)"
    exit 2
fi
if grep -q "could not connect" fault.probe.log 2>/dev/null; then
    tail -5 fault.probe.log
    echo "FAULT-SMOKE: ERROR (the probe never connected; QEMU rc=$QEMU_RC)"
    exit 2
fi

# The failure reason, by kind (as nominal-smoke.sh review m6)
why_failed() {
    local why
    why="$(grep -m1 -o 'FORBIDDEN TM: .*' fault.probe.log \
        | cut -d' ' -f3-)" || true
    if [ -n "$why" ]; then echo "forbidden: $why"; return; fi
    why="$(grep -m1 -o 'QMP ERROR: .*' fault.probe.log \
        | cut -d' ' -f3-)" || true
    if [ -n "$why" ]; then echo "qmp error: $why"; return; fi
    why="$(grep -o 'EXPECTATION NOT MATCHED: .*' fault.probe.log \
        | cut -d' ' -f4- | paste -sd' ')" || true
    if [ -n "$why" ]; then echo "not matched: $why"; return; fi
    if grep -q '^CONNECTION CLOSED' fault.probe.log; then
        echo "connection closed"; return
    fi
    echo "probe rc=$PROBE_RC; see fault.probe.log"
}

if [ "$NEGATIVE" -eq 1 ]; then
    # PASS only on the expected failure: rc 1 with exactly the two @40
    # expectations unmatched, nothing forbidden, no QMP error.
    MISSED="$(grep -o 'EXPECTATION NOT MATCHED: .*' fault.probe.log \
        | cut -d' ' -f4- | sort | paste -sd' ')" || true
    WANT="0x5A7:5:3:9449070708079f@10 0x5A7:5:4:d42000080000@10"
    if [ "$PROBE_RC" -eq 1 ] && [ "$MISSED" = "$WANT" ] && \
        ! grep -q 'FORBIDDEN TM: \|QMP ERROR: ' fault.probe.log; then
        tail -5 fault.probe.log
        echo "expected failure seen: not matched: $MISSED"
        echo "FAULT-SMOKE (negative): PASS"
        exit 0
    fi
    tail -5 fault.probe.log
    if [ "$PROBE_RC" -eq 0 ]; then
        echo "FAULT-SMOKE (negative): FAIL (the probe passed: the gate" \
            "did not detect the missing reaction)"
    else
        echo "FAULT-SMOKE (negative): FAIL (wrong failure: $(why_failed))"
    fi
    exit 1
fi

if [ "$PROBE_RC" -ne 0 ]; then
    tail -5 fault.probe.log
    echo "FAULT-SMOKE: FAIL ($(why_failed))"
    exit 1
fi

# The probe passed. The UART check, NULs stripped.
tr -d '\000' < uart.fault.raw > uart.fault.txt
if ! grep -q "init: SAFE mode" uart.fault.txt; then
    echo "FAULT-SMOKE: FAIL (the UART has no 'init: SAFE mode' banner)"
    exit 1
fi
echo "FAULT-SMOKE: PASS"
exit 0
