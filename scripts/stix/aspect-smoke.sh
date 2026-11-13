#!/usr/bin/env bash
# ASPECT smoke: the aspect-generator gate (aspect plan AS-4, D-ASi,
# docs/stix-port/2026-11-13-aspect-plan.md). Boots the STIX model on a
# FRESH flash (as in detector-smoke.sh: a NOMINAL entry writes a flash
# context), drives SAFE -> CONF -> NOMINAL -> CONF, and then fires BOTH
# aspect-burst request paths the FSW has. Times are seconds from the
# TMTC connect:
#   connect:  TM(5,3) 0x8030 (fresh flash), TM(5,1) 0x0022, 0x14A3,
#             0x142E, each within 50 s
#   @50  TC(236,1) 04 00 (CONF): TM(1,1), TM(5,1) 0021 02 04, TM(1,7);
#        forbid 0021 04 02
#   @60  TC(236,6) 07 04 00 00 (S20 = 0, the TC20 keep-alive): TM(1,1),
#        TM(1,7)
#   @70  TC(236,1) 05 00 (NOMINAL): TM(1,1), TM(5,1) 0021 04 05 within
#        10 s (its arrival is T_nom), TM(1,7). AspectCtrl_configure()
#        re-applies the AVGSIZE param at NOMINAL entry (OpModeMgmt.c:641,
#        AspectCtrl.c:536-548), and the boot already set AVGSIZE 4
#        (OpModeMgmt.c:1066-1068), so every archived block is N = 16.
#        Forbids from here on: 0021 05 02 (NOMINAL -> SAFE), 0x142E
#        (TC20), and in the positive TM(1,8)
#   @75  QMP qom-set /machine/stixctrl as-sun 0 (a no-op; the QMP path
#        the AS-5 knob legs use)
#   @320 TC(236,1) 04 00 (CONF again): TM(1,1), TM(5,1) 0021 05 04
#        within 10 s (its arrival is T_exit), TM(1,7), and the QL flush
#        TM(21,6) SSIDs 30..34 on 0x5DC within 15 s. The NOMINAL->CONF
#        flush (OpModeMgmt.c:292-294) writes the ONE aspect file (the
#        SAFE-, CONF- and NOMINAL-era records, all N = 16); forbid
#        0021 04 02
#   @340 TC(238,1) "select data for download", processing level 5, one
#        box over the whole archive (tmin 0, tmax 0x0FFFFF x 0.1 s),
#        time_binwidth 16 (app data per TM_238.c:20-79 and
#        TCData.h:406-420): TM(1,1), TM(1,7). The Background task drains
#        the request FIFO every 30 s (background.c:63, :174-178), so the
#        answer comes within 45 s; user_request_number 1.
#   @350 TC(238,2) (send the request list): TM(1,1), TM(1,7) and the
#        list report TM(238,3) on APID 0x5A8 (PID_90 + CAT_DIAGNOSTICS,
#        TM_238.c:116 with :157-158), immediately
#   @400 TC(238,10), the dedicated burst request [0, 0x0FFFFFFF] s with
#        averaging 16 (TCData.h:433-435): TM(1,1) and then TM(1,7) —
#        AspectCtrl_doTask sends the completion report itself
#        (AspectCtrl.c:519-534, TcService238.c:199-202); the answer is
#        the same window again with user_request_number 0
#        (AspectCtrl.c:107 with :530). Level-5 processing of the first
#        path is done by then (dequeued at the <= 370 s tick,
#        SciReport.c:391).
#   hold to +480 s.
# The probe appends every 0x5DC packet to asp.dump.hex (--dump-apid;
# whole CCSDS packets, the SSID at byte 16). After the run
# aspect-check.py decodes SSID 42 out of it (the QL flush SSIDs share
# the APID and are ignored) and asserts the D-ASh row 1 signature on
# BOTH paths (--expect-ideal 1 0 --summing 16 --averaging 16): one
# product per user_request_number (1 = the TC(238,1) path, 0 = the
# TC(238,10) path), full 510-sample packets with a short last one, SGF
# FIRST -> CONT -> ... -> LAST, summingFpga 16 (the FSW's
# once-per-request estimate, AspectCtrl.c:219-233), averagingTc 16, and
# packet start times 510 * 16 * 16 = 130560 ms apart (510 samples of
# averaging 16 rows of 16 ms, AspectCtrl.c:364/:372). In the positive the probe also
# forbids EID 0x543F (no BSD data found), 0x5440 (box processing error)
# and 0x5446 (request deleted) from the TC(238,1) on.
# --negative boots with -global grlib-stixctrl.as-enable=false: no
# aspect blocks, no aspect file, and the failure reports instead of
# data — TM(5,2) EID 0x543F "no BSD data found" (SciReport.c:535-544 via
# :597-601) within 45 s of the TC(238,1), and TM(1,8) FID_48435 "no
# selected data (scientific + aspect) found" (AspectCtrl.c:530-531,
# TMData.h:214) for the TC(238,10), app data 1DAC C008 BD33. The
# TM(1,8) forbid is disarmed at +325 s, before the expected one.
# aspect-check.py --expect-none asserts no SSID 42 packet at all.
# UART checks (as in detector-smoke.sh): "init: SAFE mode" present; none
# of "packet sequence error", "UNKNOWN RB-BLOCK", "TOO MANY RB-BLOCKS",
# "RotBuf overrun" (the aspect blocks between the detector chunks must
# not confuse the compactor); "Skipped first Det data chunk"
# (ArchiveManager.c:140) exactly once in both modes, "Skipped first
# Aspect data chunk" (ArchiveManager.c:219) exactly once in the
# positive and 0 in the negative; no other *E line from the chunk path,
# the archive or the filesystem.
# Trace checks: QEMU traces the stixasp_* events into asp.trace (the
# flips and their SCETs are the AS-5 legs' placement evidence). The
# positive needs >= 100 stixasp_push lines and 0 drops; the negative
# needs 0 flips and 0 pushes (the tick may arm, generation is off).
# Clocks: every window is timed on the probe's CLOCK_MONOTONIC stamps
# ([m=...]), never on the wall clock, as in detector-smoke.sh.
#
# In legacy mode (STIX_PORT_BASE unset) this script, like the other
# smokes, does NOT take the /tmp/stix-qtest-21000 flock itself; the
# caller must hold it. With STIX_PORT_BASE=auto or a slot number it
# takes a slot (scripts/stix/lib/slot.sh) in every mode: the slot's
# TMTC port, <run dir>-s<k>, and slot=k busy= load= on the verdict.
#
# Usage: aspect-smoke.sh <qemu-binary> [--negative] [--dry-run]
#                        [--check-guard]
#   STIX_ASW_ELF            as in tc-smoke.sh
#   STIX_ASPECT_RUN_DIR     run dir (default /tmp/stix-aspect-run).
#                           Canonicalised, then anything at or above
#                           /tmp/stix-uprev-run or /tmp/stix-baseline-run
#                           (or /) is refused, as in detector-smoke.sh.
#                           The negative adds the suffix -neg; slot mode
#                           adds -s<k>. A run that did not PASS leaves
#                           .unfinished; the next run moves that dir
#                           aside to <dir>.fail-<date>-<pid>.
#   STIX_ASPECT_SECONDS     the probe's --seconds (default 480, >= 450:
#                           the TC(238,10) group ends at +445 s)
#   STIX_TMTC_PROBE         the probe script (tests substitute a fake)
#                           The QMP socket is <run dir>/qmp.sock and its
#                           path must stay under 108 bytes.
#   --dry-run prints the QEMU and probe command lines, boots nothing.
#   --check-guard only runs the run-dir guard and exits 0/2.
#
# Exit codes: 0 pass (or dry-run / check-guard ok), 77 SKIP (no ELF;
#             the convention for new gates), 1 a failed expectation, a
#             forbidden TM, a closed connection, a failed aspect, UART
#             or trace check, 2 environment problem.
set -euo pipefail

QEMU_BIN_ARG="${1:?usage: aspect-smoke.sh <qemu-binary> \
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

. "$(dirname "${BASH_SOURCE[0]}")/lib/paths.sh"
ASW_ELF="${STIX_ASW_ELF:-$STIX_DEFAULT_ELF}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_DIR="${STIX_ASPECT_RUN_DIR:-/tmp/stix-aspect-run}"
DURATION="${STIX_ASPECT_SECONDS:-480}"
PROBE="${STIX_TMTC_PROBE:-$REPO_ROOT/scripts/stix/tmtc-probe.py}"
ASPCHECK="$REPO_ROOT/scripts/stix/aspect-check.py"
# Slot mode (STIX_PORT_BASE, scripts/stix/lib/slot.sh): the slot's port
# and run dir <dir>-s<k>; unset = legacy, 20000+uid and the dir as is.
. "$REPO_ROOT/scripts/stix/lib/slot.sh"
# dry-run, check-guard and the ELF-absent SKIP boot nothing: they peek at
# the slot (no lock, no wait, no disk guard)
if [ "$DRY_RUN" -eq 1 ] || [ "$CHECK_GUARD" -eq 1 ] ||
        [ ! -f "$ASW_ELF" ]; then
    stix_slot_setup peek
else
    stix_slot_setup
fi

# The run-dir guard of detector-smoke.sh (review M1 there): canonical
# first, then refuse the two shared run dirs, anything inside them, any
# parent of them, and / — on the given dir and on the final one (with
# the mode and slot suffixes).
RUN_DIR="$(realpath -m -- "$RUN_DIR")"
SHARED_UPREV="$(realpath -m -- "$STIX_UPREV_DIR")"
SHARED_OLD="$(realpath -m -- /tmp/stix-uprev-run)"
SHARED_BASE="$(realpath -m -- /tmp/stix-baseline-run)"
refuse_run_dir() {
    echo "refusing run dir $RUN_DIR ($1)"
    echo "a NOMINAL entry writes a flash context, so this gate needs its"
    echo "own empty run dir away from the shared ones"
    exit 2
}
guard_shared() {
    local shared
    for shared in "$SHARED_UPREV" "$SHARED_OLD" "$SHARED_BASE"; do
        case "$RUN_DIR" in
            "$shared" | "$shared"/*)
                refuse_run_dir "it is $shared or inside it" ;;
        esac
        case "$shared" in
            "$RUN_DIR"/*)
                refuse_run_dir "it is a parent of $shared" ;;
        esac
    done
}
if [ "$RUN_DIR" = "/" ]; then
    refuse_run_dir "it is the root directory"
fi
guard_shared
MODE_SFX=""
[ "$NEGATIVE" -eq 1 ] && MODE_SFX="-neg"
RUN_DIR="$(stix_slot_dir "$RUN_DIR$MODE_SFX")"
guard_shared
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
        echo "STIX_ASPECT_SECONDS must be whole seconds (got: $DURATION)"
        exit 2
        ;;
esac
if [ "$DURATION" -lt 450 ]; then
    echo "STIX_ASPECT_SECONDS must be >= 450 (the TC(238,10) group ends" \
        "at +445 s; got: $DURATION)"
    exit 2
fi

# TC units: SpW header 40 02 00 00 + CCSDS TC on APID 0x1DAC + CRC-16/
# CCITT-FALSE (poly 0x1021, init 0xFFFF, over the CCSDS packet), built
# as in nominal-smoke.sh. The four mode TCs are that smoke's/detector-
# smoke's units (seq 1, 2, 3, 5); the three service-238 TCs (seq 6..8)
# are built here because their CRCs cover the 26/0/10-byte application
# data (the payloads in the header above). The builder is anchored: it
# must reproduce the four known units byte for byte, or the gate exits
# before booting anything.
tc_units() {
    python3 - <<'PY'
import sys


def crc16(d):
    c = 0xFFFF
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 \
                else (c << 1) & 0xFFFF
    return c


def tc(seq, svc, sub, app):
    # the SpW unit prefix 40 02 00 00, then the CCSDS packet: primary
    # header, secondary header 19 <svc> <sub> 00, app data, CRC
    b = bytes([0x1D, 0xAC, 0xC0, seq]) + (len(app) + 5).to_bytes(2, "big") \
        + bytes([0x19, svc, sub, 0x00]) + app
    return "40020000" + (b + crc16(b).to_bytes(2, "big")).hex().upper()


# the four known units (nominal-smoke.sh seq 1-3, detector-smoke.sh seq 5)
anchor = {
    1: ("40 02 00 00 1D AC C0 01 00 07 19 EC 01 00 04 00 4A C1",
        0xEC, 1, bytes([4, 0])),
    2: ("40 02 00 00 1D AC C0 02 00 09 19 EC 06 00 07 04 00 00 AF BA",
        0xEC, 6, bytes([7, 4, 0, 0])),
    3: ("40 02 00 00 1D AC C0 03 00 07 19 EC 01 00 05 00 BF 97",
        0xEC, 1, bytes([5, 0])),
    5: ("40 02 00 00 1D AC C0 05 00 07 19 EC 01 00 04 00 D6 2E",
        0xEC, 1, bytes([4, 0])),
}
units = {}
for seq, (known, svc, sub, app) in anchor.items():
    units[seq] = tc(seq, svc, sub, app)
    if units[seq] != known.replace(" ", ""):
        sys.exit(f"TC builder anchor mismatch at seq {seq}: "
                 f"got {units[seq]}")
# TC(238,1): urn 1, level 5, start 0, subsec 0, det mask 0, 1 box
# (tmin 0, tmax 0x0FFFFF x 0.1 s, binwidth 16, E packed 0)
app1 = bytes([0, 0, 0, 1, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
              0, 0, 0, 0x0F, 0xFF, 0xFF, 0, 16, 0, 0])
units[6] = tc(6, 0xEE, 1, app1)
units[7] = tc(7, 0xEE, 2, b"")
# TC(238,10): start 0 s, end 0x0FFFFFFF s, averaging 16
units[8] = tc(8, 0xEE, 10, bytes([0, 0, 0, 0, 0x0F, 0xFF, 0xFF, 0xFF,
                                  0, 16]))
for seq in sorted(units):
    print(units[seq])
PY
}
TC_HEX="$(tc_units)"    # set -e: a failed anchor exits here
mapfile -t TC_ARR <<< "$TC_HEX"
TC_CONF="${TC_ARR[0]}"
TC_S20="${TC_ARR[1]}"
TC_NOM="${TC_ARR[2]}"
TC_CONF2="${TC_ARR[3]}"
TC_238_1="${TC_ARR[4]}"
TC_238_2="${TC_ARR[5]}"
TC_238_10="${TC_ARR[6]}"

# The aspect-scene no-op over QMP (@75; the negative sends it too, the
# QMP path and the timing stay the same).
qom_set() {  # path property json-value
    printf '{"execute": "qom-set", "arguments": %s' \
        "{\"path\": \"$1\", \"property\": \"$2\", \"value\": $3}}"
}

# The TC(238,1) group's answer differs: data in the positive (forbid the
# failure events), the 0x543F event in the negative (within 45 s: the
# Background FIFO tick runs every 30 s).
if [ "$NEGATIVE" -eq 1 ]; then
    # TM(5,2): the EID is a u16 (TM_5.c:212), 0x543F then urn u32 1
    ASK1_ARGS=(--expect "0x5A7:5:2:543f00000001@45"
               --forbid "0x5A7:5:2:5440"
               --forbid "0x5A7:5:2:5446")
else
    ASK1_ARGS=(--forbid "0x5A7:5:2:543f"
               --forbid "0x5A7:5:2:5440"
               --forbid "0x5A7:5:2:5446")
fi
# The TC(238,10) group's completion report: TM(1,8) FID_48435 in the
# negative, TM(1,7) in the positive. The plain TM(1,8) forbid of the
# NOMINAL group must be gone by then: the negative disarms it at
# +325 s (the group's send was at +70 s, the expected report comes at
# +400 s).
if [ "$NEGATIVE" -eq 1 ]; then
    ASK2_ARGS=(--expect "0x5A1:1:8:1DACC008BD33")
    NOM_FORBID_18=(--forbid "0x5A1:1:8@325")
else
    ASK2_ARGS=(--expect "0x5A1:1:7:1DACC008")
    NOM_FORBID_18=(--forbid "0x5A1:1:8")
fi

# TM(5,x) on 0x5A7, the subtype from the EID's severity nibble (1 ->
# TM(5,1), 5 -> TM(5,2), 9 -> TM(5,3)); TM(1,x) on 0x5A1; TM(238,3) on
# 0x5A8; TM(21,6) on 0x5DC, matched on the SSID (the first application
# byte).
PROBE_ARGS=(
    --port "$TMTC_PORT" --seconds "$DURATION" --window 20
    --dump-apid "0x5DC:$RUN_DIR/asp.dump.hex"
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
    --send-at 70 --send-hex "$TC_NOM"
    --expect "0x5A1:1:1:1DACC003"
    --expect "0x5A7:5:1:00210405@10"
    --expect "0x5A1:1:7:1DACC003"
    --forbid "0x5A7:5:1:00210502"
    --forbid "0x5A7:5:1:142e"
    ${NOM_FORBID_18+"${NOM_FORBID_18[@]}"}
    --qmp "$QMP_SOCK"
    --send-at 75
    --qmp-cmd "$(qom_set /machine/stixctrl as-sun 0)"
    --send-at 320 --send-hex "$TC_CONF2"
    --expect "0x5A1:1:1:1DACC005"
    --expect "0x5A7:5:1:00210504@10"
    --expect "0x5A1:1:7:1DACC005"
    --expect "0x5DC:21:6:1e@15"
    --expect "0x5DC:21:6:1f@15"
    --expect "0x5DC:21:6:20@15"
    --expect "0x5DC:21:6:21@15"
    --expect "0x5DC:21:6:22@15"
    --forbid "0x5A7:5:1:00210402"
    --send-at 340 --send-hex "$TC_238_1"
    --expect "0x5A1:1:1:1DACC006"
    --expect "0x5A1:1:7:1DACC006"
    ${ASK1_ARGS+"${ASK1_ARGS[@]}"}
    --send-at 350 --send-hex "$TC_238_2"
    --expect "0x5A1:1:1:1DACC007"
    --expect "0x5A1:1:7:1DACC007"
    --expect "0x5A8:238:3"
    --send-at 400 --send-hex "$TC_238_10"
    --expect "0x5A1:1:1:1DACC008"
    ${ASK2_ARGS+"${ASK2_ARGS[@]}"}
    --hold
)
QEMU_ARGS=(
    -M leon3_generic -m 64M -no-reboot
    -display none -monitor none
    "${STIX_SLOT_QEMU_ARGS[@]}"
    -qmp "unix:$QMP_SOCK,server=on,wait=off"
    -serial "file:$RUN_DIR/uart.aspect.raw"
    -kernel "$ASW_ELF"
    -msg timestamp=on
    -trace "events=$RUN_DIR/asp.events,file=$RUN_DIR/asp.trace"
)
if [ "$NEGATIVE" -eq 1 ]; then
    QEMU_ARGS+=(-global grlib-stixctrl.as-enable=false)
fi
MODE="positive"
[ "$NEGATIVE" -eq 1 ] && MODE="negative"

if [ "$DRY_RUN" -eq 1 ]; then
    echo "DRY RUN — the commands aspect-smoke would run ($MODE):"
    printf '(cd %s && timeout %s %s ' "$RUN_DIR" "$((DURATION + 30))" \
        "$QEMU_BIN"
    printf '%q ' "${QEMU_ARGS[@]}"
    printf '> qemu-stdout.aspect.log 2>&1)\n'
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
    echo "TMTC port ${TMTC_PORT} busy — another QEMU instance" \
        "running?${STIX_SLOT_TAG}"
    exit 2
fi

# A run that did not PASS leaves .unfinished behind; its dir is moved
# aside, never reused, so its evidence survives any later run.
if [ -e "$RUN_DIR/.unfinished" ]; then
    KEPT="$RUN_DIR.fail-$(date +%Y%m%d-%H%M%S)-$$"
    mv -T -- "$RUN_DIR" "$KEPT"
    echo "aspect-smoke: kept the unfinished run's dir as $KEPT"
fi
mkdir -p "$RUN_DIR"
cd "$RUN_DIR"
touch .unfinished
# Fresh flash every run: clear only this run dir's own files, by
# explicit path (the shared run dirs are refused above), then pre-create
# an empty sparse flash.bin of the full size, so QEMU never creates one
# itself (an older build fills a missing one densely, 16.5 GB). The dump
# is appended to, so it must start empty.
rm -f flash.bin uart.aspect.raw uart.aspect.txt aspect.probe.log \
      qemu-stdout.aspect.log asp.trace asp.dump.hex asp.check.log \
      qmp.sock
truncate -s 17716740096 flash.bin
# the trace events: the aspect generator's scene, tick, flips, pushes
# and drops (the flips' block_seq/SCET are the AS-5 legs' placement
# evidence)
printf '%s\n' stixasp_scene stixasp_tick_arm stixasp_flip stixasp_push \
    stixasp_drop stixasp_jitter stixasp_step > asp.events

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
# the port, then stays on the connection for the whole run (--hold).
timeout "$((DURATION + 30))" python3 -u "$PROBE" "${PROBE_ARGS[@]}" \
    > aspect.probe.log 2>&1 &
PROBE_PID=$!

timeout "$((DURATION + 30))" "$QEMU_BIN" "${QEMU_ARGS[@]}" \
    > qemu-stdout.aspect.log 2>&1 &
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
    tail -5 qemu-stdout.aspect.log
    echo "ASPECT-SMOKE: ERROR (QEMU died or never started;" \
        "qemu rc=$QEMU_RC)${STIX_SLOT_TAG}"
    exit 2
fi
if [ "$PROBE_RC" -eq 124 ]; then
    tail -5 aspect.probe.log
    echo "ASPECT-SMOKE: ERROR (the probe hit its own timeout)${STIX_SLOT_TAG}"
    exit 2
fi
if [ "$PROBE_RC" -eq 2 ]; then
    tail -5 aspect.probe.log
    echo "ASPECT-SMOKE: ERROR (the probe rejected its" \
        "arguments)${STIX_SLOT_TAG}"
    exit 2
fi
if grep -q "could not connect" aspect.probe.log 2>/dev/null; then
    tail -5 aspect.probe.log
    echo "ASPECT-SMOKE: ERROR (the probe never connected;" \
        "QEMU rc=$QEMU_RC)${STIX_SLOT_TAG}"
    exit 2
fi

# Every check below runs and records its name in FAILED, so the report
# is complete. The aspect check's first mismatch (and how many more) is
# repeated in the verdict line (WHY, with its leading "; ").
FAILED=()
WHY=""
asp_why() { # aspect-check log -> one line
    local first n
    first="$(grep -m1 '^ASPECT-CHECK MISMATCH: ' "$1" | cut -d' ' -f3-)" \
        || true
    n="$(grep -c '^ASPECT-CHECK MISMATCH: ' "$1")" || true
    if [ -n "$first" ]; then
        [ "$n" -gt 1 ] && first="$first (+$((n - 1)) more)"
        echo "$first"
    else
        grep -m1 -E '^ASPECT-CHECK: (FAIL|ERROR)' "$1" | cut -d' ' -f2- ||
            echo "no aspect-check verdict"
    fi
}

# 1. the probe (TM expectations and forbids)
if [ "$PROBE_RC" -ne 0 ]; then
    REASON="$(grep -m1 -o 'FORBIDDEN TM: .*' aspect.probe.log \
        | cut -d' ' -f3-)" || true
    if [ -n "$REASON" ]; then
        REASON="forbidden: $REASON"
    else
        REASON="$(grep -o 'EXPECTATION NOT MATCHED: .*' aspect.probe.log \
            | cut -d' ' -f4- | paste -sd' ')" || true
        if [ -n "$REASON" ]; then
            REASON="not matched: $REASON"
        elif grep -q '^CONNECTION CLOSED' aspect.probe.log; then
            REASON="connection closed"
        else
            REASON="probe rc=$PROBE_RC; see aspect.probe.log"
        fi
    fi
    tail -5 aspect.probe.log
    echo "CHECK probe: FAIL ($REASON)"
    FAILED+=(probe)
else
    echo "CHECK probe: ok (every TM expectation matched, no forbid fired)"
fi

# T_nom / T_exit: the probe's monotonic stamps ([m=...]) of the arrivals
# of 0021 04 05 / 0021 05 04 (the HH:MM:SS are for reading only)
P_NOM='EXPECT OK: 0x5A7:5:1:00210405@10'
P_EXIT='EXPECT OK: 0x5A7:5:1:00210504@10'
mono_of() { # probe-log pattern -> the [m=...] stamp of its first line
    grep -m1 -- "$1" aspect.probe.log |
        sed -n 's/.*\[m=\([0-9][0-9.]*\)\].*/\1/p'
}
hms_of() {
    grep -m1 -- "$1" aspect.probe.log | cut -d' ' -f1
}
T_NOM_M="$(mono_of "$P_NOM")" || true
T_EXIT_M="$(mono_of "$P_EXIT")" || true
NOM_SECONDS=""
if [ -n "$T_NOM_M" ] && [ -n "$T_EXIT_M" ]; then
    NOM_SECONDS="$(awk -v a="$T_NOM_M" -v b="$T_EXIT_M" \
        'BEGIN { printf "%.1f", b - a }')"
    echo "NOMINAL window: T_nom $(hms_of "$P_NOM"), T_exit" \
        "$(hms_of "$P_EXIT"), $NOM_SECONDS s (monotonic)"
fi

# 2. the SSID 42 burst answers (both request paths in one dump)
ASPECT_ARGS=(asp.dump.hex)
if [ "$NEGATIVE" -eq 1 ]; then
    ASPECT_ARGS+=(--expect-none)
else
    ASPECT_ARGS+=(--expect-ideal 1 0 --summing 16 --averaging 16)
fi
if [ ! -s asp.dump.hex ]; then
    echo "CHECK aspect: FAIL (no 0x5DC packet dumped)"
    FAILED+=(aspect)
    WHY="$WHY; aspect: no 0x5DC packet dumped"
else
    set +e
    python3 "$ASPCHECK" "${ASPECT_ARGS[@]}" > asp.check.log 2>&1
    ASP_RC=$?
    set -e
    grep -E '^(SSID|ASPECT-CHECK)' asp.check.log || true
    if [ "$ASP_RC" -eq 0 ]; then
        echo "CHECK aspect: ok ($(wc -l < asp.dump.hex) packets in" \
            "asp.dump.hex)"
    else
        echo "CHECK aspect: FAIL ($(asp_why asp.check.log); aspect-check" \
            "rc=$ASP_RC; see asp.check.log)"
        FAILED+=(aspect)
        WHY="$WHY; aspect: $(asp_why asp.check.log)"
    fi
fi

# 3. the UART, NULs stripped
tr -d '\000' < uart.aspect.raw > uart.aspect.txt
UART_OK=1
if ! grep -q "init: SAFE mode" uart.aspect.txt; then
    echo "CHECK uart: the UART has no 'init: SAFE mode' banner"
    UART_OK=0
fi
if grep -Eq 'storing new context[01] failed' uart.aspect.txt; then
    grep -Em1 'storing new context[01] failed' uart.aspect.txt
    echo "CHECK uart: a flash context write failed"
    UART_OK=0
fi
for pat in "packet sequence error" "RotBuf overrun" "UNKNOWN RB-BLOCK" \
    "TOO MANY RB-BLOCKS"; do
    if grep -q "$pat" uart.aspect.txt; then
        grep -m1 "$pat" uart.aspect.txt
        echo "CHECK uart: '$pat' on the UART"
        UART_OK=0
    fi
done
if [ "$UART_OK" -eq 1 ]; then
    echo "CHECK uart: ok (SAFE banner, no context, ROTB or sequence error)"
else
    FAILED+=(uart)
fi
# the first chunk of each kind reached the archive, which drops it with
# this TRACE_ERROR (ArchiveManager.c:140 Det, :219 Aspect); the aspect
# line is once per flush cycle that carried aspect blocks (here the one
# boot epoch), and absent with as-enable=false
SKIPPED_DET="$(grep -c 'Skipped first Det data chunk' uart.aspect.txt \
    || true)"
SKIPPED_ASP="$(grep -c 'Skipped first Aspect data chunk' \
    uart.aspect.txt || true)"
# no other error from the chunk path, the archive or the filesystem on
# the fresh flash (the FSW TRACE_TAGs of RotAccumulator.c,
# RotAccuCompactor.c, ArchiveManager.c and MemoryMgmt/)
CHUNK_TAGS='Archive|RotAccuComp|RotAccumulator|FS|FSapi|MemStream'
CHUNK_TAGS="$CHUNK_TAGS|MemCommon|FIFO"
CHUNK_ERR="$(grep -E "^[[:space:]]*[0-9:.]+ +($CHUNK_TAGS) +\\*E" \
    uart.aspect.txt | grep -Ev 'Skipped first (Det|Aspect) data chunk' |
    head -3 || true)"
if [ -n "$CHUNK_ERR" ]; then
    echo "CHECK uart-chunk: FAIL (chunk/archive/filesystem error:" \
        "$(echo "$CHUNK_ERR" | head -1 | cut -c1-120))"
    FAILED+=(uart-chunk)
elif [ "$SKIPPED_DET" -eq 1 ] && { { [ "$NEGATIVE" -eq 0 ] &&
    [ "$SKIPPED_ASP" -eq 1 ]; } ||
    { [ "$NEGATIVE" -eq 1 ] && [ "$SKIPPED_ASP" -eq 0 ]; }; }; then
    echo "CHECK uart-chunk: ok ('Skipped first Det data chunk' once," \
        "'Skipped first Aspect data chunk' $SKIPPED_ASP, no" \
        "chunk/archive/filesystem error)"
else
    echo "CHECK uart-chunk: FAIL ($SKIPPED_DET 'Skipped first Det data" \
        "chunk' and $SKIPPED_ASP 'Skipped first Aspect data chunk'" \
        "lines; expected 1 and $((1 - NEGATIVE)))"
    FAILED+=(uart-chunk)
fi

# 4. the generator's own trace: flips and pushes on the positive, none
# on the negative (the placement evidence of the AS-5 legs lives here).
# A push result other than "appended" (rotb-disabled/rotb-testmode in the
# pre-NOMINAL epochs) is printed, not asserted; a drop would break the
# archive and is.
if [ -f asp.trace ]; then
    PUSH_N="$(grep -c 'stixasp_push' asp.trace || true)"
    FLIP_N="$(grep -c 'stixasp_flip' asp.trace || true)"
    DROP_N="$(grep -c 'stixasp_drop' asp.trace || true)"
    APP_N="$(grep -c 'stixasp_push.* appended,' asp.trace || true)"
else
    PUSH_N=""; FLIP_N=""; DROP_N=""; APP_N=""
fi
if [ "$NEGATIVE" -eq 1 ]; then
    if [ "$FLIP_N" = 0 ] && [ "$PUSH_N" = 0 ]; then
        echo "CHECK trace: ok (as-enable=false: 0 flips, 0 pushes)"
    else
        echo "CHECK trace: FAIL (${FLIP_N:-?} flips, ${PUSH_N:-?} pushes" \
            "with as-enable=false; expected 0 and 0)"
        FAILED+=(trace)
    fi
elif [ "${PUSH_N:-0}" -ge 100 ] && [ "${DROP_N:-1}" -eq 0 ]; then
    echo "CHECK trace: ok ($PUSH_N pushes of $FLIP_N flips, $APP_N" \
        "appended, $DROP_N drops; asp.trace holds the flips' block_seq" \
        "and SCET for the AS-5 placement)"
    if [ "$APP_N" -ne "$PUSH_N" ]; then
        echo "NOTE trace: $((PUSH_N - APP_N)) push(es) not appended" \
            "(rotb-disabled/testmode before NOMINAL)"
    fi
else
    echo "CHECK trace: FAIL (${PUSH_N:-?} pushes of ${FLIP_N:-?} flips," \
        "${DROP_N:-?} drops; expected >= 100 pushes and 0 drops)"
    FAILED+=(trace)
fi

GOT="${FAILED[*]:-}"
if [ -n "$GOT" ]; then
    echo "ASPECT-SMOKE ($MODE): FAIL (failed checks: $GOT$WHY)${STIX_SLOT_TAG}"
    exit 1
fi
# PASS: the run dir may be reused; in slot mode drop the flash copy
# (asp.trace stays: it is the AS-5 placement evidence)
rm -f .unfinished
stix_slot_cleanup 0 flash.bin
if [ "$NEGATIVE" -eq 1 ]; then
    echo "ASPECT-SMOKE (negative): PASS (no aspect file, 0x543F and" \
        "TM(1,8) FID_48435, no SSID 42 packet)${STIX_SLOT_TAG}"
else
    echo "ASPECT-SMOKE: PASS (both request paths answered D-ASh row 1:" \
        "510-sample packets, summing 16, averaging 16, continuous)" \
        "${STIX_SLOT_TAG}"
fi
exit 0