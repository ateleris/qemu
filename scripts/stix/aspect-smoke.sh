#!/usr/bin/env bash
# ASPECT smoke: the aspect-generator gate (aspect plan AS-4/AS-5, D-ASi,
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
#   @150 the leg's knob (one qom-set over QMP): mid-NOMINAL, ~78
#        archived blocks in (--drift's rate goes @55, pre-NOMINAL)
#   @320 TC(236,1) 04 00 (CONF again): TM(1,1), TM(5,1) 0021 05 04
#        within 10 s (its arrival is T_exit), TM(1,7), and the QL flush
#        TM(21,6) SSIDs 30..34 on 0x5DC within 15 s. The NOMINAL->CONF
#        flush (OpModeMgmt.c:292-294) writes the ONE aspect file (the
#        SAFE-, CONF- and NOMINAL-era records, all N = 16); forbid
#        0021 04 02
#   @330 #1031-shaped legs only (--drift, --step-1031): TC(237,18)
#        01 DD 00 00 00 00 64 sets NUMBER_ASPECT_GAPS_ALLOWED = 100
#        (param id 477 = 0x01DD, TcService237.c:66-94, TCData.h:103-105,
#        ParamMgmtDef.h:547; the anchor count: NOMINAL_QL_PERIOD = 315
#        at ParamMgmtDef.h:385, the AX block :67-113 in), @331 the
#        read-back TC(237,19) 01 DD 00 whose TM(237,20) on APID 0x5A8
#        (TM_237.c:29-48) must carry 01 DD 00 00 00 00 64 - a wrong
#        param index fails loudly here
#   @340 TC(238,1) "select data for download", processing level 5, one
#        box over the whole archive (tmin 0, tmax 0x0FFFFF x 0.1 s),
#        time_binwidth = the leg's averaging (16, or 64 on the
#        #1031-shaped legs; the box layout TCData.h:406-420, level-5
#        dispatch SciReport.c:586-602): TM(1,1), TM(1,7). The Background
#        task drains the request FIFO every 30 s (background.c:63,
#        :174-178), so the answer comes within 45 s; urn 1.
#   @350 TC(238,2) (send the request list): TM(1,1), TM(1,7) and the
#        list report TM(238,3) on APID 0x5A8 (PID_90 + CAT_DIAGNOSTICS,
#        TM_238.c:116 with :157-158), immediately
#   @400 TC(238,10), the dedicated burst request [0, 0x0FFFFFFF] s with
#        the leg's averaging (TCData.h:433-435, the GET16 in
#        AspectCtrl.c:500-506): TM(1,1) and then TM(1,7) - or, on the
#        error legs, TM(1,8) FID_48435 (AspectCtrl.c:519-534); the
#        answer is the same window again with user_request_number 0
#   hold to +480 s.
# The legs (each a separate run; the D-ASh rows of the plan):
#   --ideal            row 1 (control): no knob, the ideal signature on
#                      both paths.
#   --jitter           row 2: as-jitter-next-ms 6 @150; two gap events,
#                      budget 1 exhausted -> EID 0x5440 (urn 1) and
#                      TM(1,8) FID_48435 (the TC(238,10) path); the
#                      product ends early with a forced packet.
#   --step-gap         row 3: as-step-next-ms 6 @150; one gap event, the
#                      request completes with exactly one forced packet.
#   --drift            row 4 (#1031-shaped): as-rate-ppm -2000 @55 (all
#                      NOMINAL blocks drift; the estimate pair reads
#                      1005..1006 ms -> inc 15), budget 100 + averaging
#                      64: a gap at every block, ~100 forced packets of
#                      0/1 samples (a leading 0-sample one, MINOR 3),
#                      then EID 0x5440 / TM(1,8).
#   --drop             row 5 (corrected, plan Amendment B1): as-drop-next
#                      2 @150; the archive stores each block with the
#                      previous block's end, so the gap shows one record
#                      later: one gap event, the request completes.
#   --avg              Amendment B2 (row 6 redesigned: TC(236,6) is
#                      CONF/MAINTENANCE-only, TCVerifDynamic.c:66): Avg16
#                      epoch 1, @332 TC(236,6) 03 01 00 01 (Avg2) in CONF,
#                      epoch 2 @340-@400, then one averaging-64 request
#                      over both files with budget 100: epoch 1's packet,
#                      then a gap-forced packet per Avg2 record, 0x5440.
#   --step-1031        row 7 (#1031-shaped): a -1 ms step armed at the
#                      FIRST archived flip's own block_seq (the first
#                      stixasp_push after the epoch's skip + 1: the
#                      skipped block's raw end is the first record's
#                      stored time, ArchiveManager.c:217-223/:243-245, so
#                      the estimate pair straddles the step -> 1007 ms ->
#                      inc 15). A watcher arms the knob over QMP the
#                      moment the trace shows the first appended push.
#                      Plus the second epoch (plan Amendment B3: the
#                      epoch-1 request @340 is answered in CONF first):
#                      @400 NOMINAL again, @460 CONF (the second file
#                      via the NOMINAL->CONF flush), @470 a second
#                      TC(238,1) urn 2 with the window [405 s, ...]
#                      inside the second file (the epoch discriminator:
#                      inc 16, one small product). No TC(238,10) path.
#                      + @414 TC(238,2). The TC(238,10) at @400
#                      re-requests the same epochs and fails like urn 1.
#   --step-1031-control  row 7's control: the same -1 ms step mid-window
#                      (@150, qom-set) leaves the estimate pair clean;
#                      1 ms < the 5 ms gap threshold, so the request
#                      completes control-like (the ideal signature).
#   --negative         -global grlib-stixctrl.as-enable=false: no
#                      blocks, no aspect file, EID 0x543F + TM(1,8)
#                      FID_48435, no SSID 42 packet at all.
# The probe appends every 0x5DC packet to asp.dump.hex (--dump-apid;
# whole CCSDS packets, the SSID at byte 16). After the run
# aspect-check.py decodes SSID 42 out of it (the QL flush SSIDs share
# the APID and are ignored) and asserts the leg's D-ASh signature; the
# [L]-loose modes print ASPECT-CHECK NOTE lines with the observed
# numbers for the reviewer to pin.
# UART checks (as in detector-smoke.sh): "init: SAFE mode" present; none
# of "packet sequence error", "UNKNOWN RB-BLOCK", "TOO MANY RB-BLOCKS",
# "RotBuf overrun" (the aspect blocks between the detector chunks must
# not confuse the compactor); "Skipped first Det data chunk"
# (ArchiveManager.c:140) once per archive epoch with detector data
# (once; twice on --step-1031, whose second NOMINAL epoch follows the
# @320 flush that resets the skip flag, ArchiveManager.c:270-278),
# "Skipped first Aspect data chunk" (ArchiveManager.c:219) once per
# epoch with aspect blocks (once; twice on --step-1031); the error legs
# (--jitter, --drift, --avg, --step-1031) show the FSW's own
# "Terminating aspect report due to too many gaps." (AspectCtrl.c:312)
# once per failed request (2: the urn-1 and the TC(238,10) requests),
# the completing legs none; no other *E line from the chunk path, the
# archive or the filesystem.
# Trace checks: QEMU traces the stixasp_* events into asp.trace (the
# flips and their SCETs are the placement evidence). The positive needs
# >= 100 stixasp_push lines (98 on --drop: two knob drops) and the leg's
# knob trace lines (--jitter: one stixasp_jitter; --step-gap and the
# step legs: stixasp_step at the armed block_seq; --drop: two
# stixasp_drop knob lines; --drift: the -2000 scene line); --step-1031
# cross-checks the step's block_seq against the watcher's armed value
# and reports a placement failure (not a falsification) on a mismatch.
# Clocks: every window is timed on the probe's CLOCK_MONOTONIC stamps
# ([m=...]), never on the wall clock, as in detector-smoke.sh.
#
# In legacy mode (STIX_PORT_BASE unset) this script, like the other
# smokes, does NOT take the /tmp/stix-qtest-21000 flock itself; the
# caller must hold it. With STIX_PORT_BASE=auto or a slot number it
# takes a slot (scripts/stix/lib/slot.sh) in every mode: the slot's
# TMTC port, <run dir>-s<k>, and slot=k busy= load= on the verdict.
#
# Usage: aspect-smoke.sh <qemu-binary> [--ideal] [--negative]
#                        [--jitter] [--step-gap] [--drift] [--drop]
#                        [--avg] [--step-1031] [--step-1031-control]
#                        [--dry-run] [--check-guard]
#   Exactly one leg; no leg argument = --ideal.
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

QEMU_BIN_ARG="${1:?usage: aspect-smoke.sh <qemu-binary> [--ideal|"\
"--negative] [--jitter] [--step-gap] [--drift] [--drop] [--avg] "\
"[--step-1031] [--step-1031-control] [--dry-run] [--check-guard]}"
LEG="ideal"
LEG_GIVEN=0
NEGATIVE=0
DRY_RUN=0
CHECK_GUARD=0
for a in "${@:2}"; do
    case "$a" in
        --ideal | --negative | --jitter | --step-gap | --drift | \
        --drop | --avg | --step-1031 | --step-1031-control)
            if [ "$LEG_GIVEN" -eq 1 ]; then
                echo "leg given twice ($LEG and $a)"; exit 2
            fi
            LEG_GIVEN=1
            case "$a" in
                --ideal) LEG="ideal" ;;
                --negative) LEG="negative" ;;
                --jitter) LEG="jitter" ;;
                --step-gap) LEG="step-gap" ;;
                --drift) LEG="drift" ;;
                --drop) LEG="drop" ;;
                --avg) LEG="avg" ;;
                --step-1031) LEG="step-1031" ;;
                --step-1031-control) LEG="step-1031c" ;;
            esac ;;
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
[ "$LEG" = "negative" ] && NEGATIVE=1

. "$(dirname "${BASH_SOURCE[0]}")/lib/paths.sh"
ASW_ELF="${STIX_ASW_ELF:-$STIX_DEFAULT_ELF}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_DIR="${STIX_ASPECT_RUN_DIR:-/tmp/stix-aspect-run}"
# per-leg default and floor: the two-epoch legs of Amendment B end later
# (--avg's 0x5440 window closes at +500 s, --step-1031's urn-2 answer at
# +530 s); the others end with the TC(238,10) group at +445 s
case "$LEG" in
    avg) DEFAULT_SECONDS=550; MIN_SECONDS=520 ;;
    step-1031) DEFAULT_SECONDS=560; MIN_SECONDS=540 ;;
    *) DEFAULT_SECONDS=480; MIN_SECONDS=450 ;;
esac
DURATION="${STIX_ASPECT_SECONDS:-$DEFAULT_SECONDS}"
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
# the legs run in their own dirs (an .unfinished dir of one leg never
# interacts with another leg's)
MODE_SFX=""
[ "$NEGATIVE" -eq 1 ] && MODE_SFX="-neg"
case "$LEG" in
    ideal) MODE="positive" ;;
    step-1031c)
        MODE="step-1031-control"
        MODE_SFX="-step-1031c" ;;
    *)
        MODE="$LEG"
        if [ "$NEGATIVE" -eq 0 ]; then
            MODE_SFX="-$LEG"
        fi ;;
esac
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
if [ "$DURATION" -lt "$MIN_SECONDS" ]; then
    echo "STIX_ASPECT_SECONDS must be >= $MIN_SECONDS for this leg (its" \
        "last expected answer; got: $DURATION)"
    exit 2
fi

# TC units: SpW header 40 02 00 00 + CCSDS TC on APID 0x1DAC + CRC-16/
# CCITT-FALSE (poly 0x1021, init 0xFFFF, over the CCSDS packet), built
# as in nominal-smoke.sh. The four mode TCs are that smoke's/detector-
# smoke's units (seq 1, 2, 3, 5); the service-238 TCs (seq 6..8) and the
# AS-5 leg TCs (seq 9..17: the 237,18/237,19 gap-budget pair, the
# 236,6 AvgSize change, the averaging-64 request pair, the second
# epoch's mode and request TCs) are built here because their CRCs cover
# their application data. The builder is anchored: it must reproduce
# the four known units byte for byte, or the gate exits before booting
# anything.
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


def tc238_1(seq, urn, start_s, binwidth):
    # TC(238,1): urn u32, level 5, start sec u32 + subsec u16, detector
    # mask u32, 1 box: tmin u24 0, tmax u24 0x0FFFFF (x 0.1 s), binwidth
    # u16, E packed 0 (TCData.h:406-420, TcService238.c)
    # subsec u16 0, detector mask u32 0, number of boxes u8 = 1 (a 0
    # here keeps the length but the FSW then rejects the TC with
    # FID_48022 "invalid length of dynamic packet", TCVerifDynamic.c:296)
    app = bytes([0, 0, 0, urn, 5]) + start_s.to_bytes(4, "big") + \
        bytes(2 + 4) + bytes([1]) + bytes([0, 0, 0, 0x0F, 0xFF, 0xFF]) + \
        binwidth.to_bytes(2, "big") + bytes(2)
    return tc(seq, 0xEE, 1, app)


# TC(238,1): urn 1, level 5, start 0, subsec 0, det mask 0, 1 box
# (tmin 0, tmax 0x0FFFFF x 0.1 s, binwidth 16, E packed 0)
units[6] = tc238_1(6, 1, 0, 16)
# the AS-4 unit the FSW accepted live (urn 1, 1 box, binwidth 16)
if units[6] != tc(6, 0xEE, 1, bytes([0, 0, 0, 1, 5, 0, 0, 0, 0, 0, 0, 0, 0,
                                    0, 0, 1, 0, 0, 0, 0x0F, 0xFF, 0xFF,
                                    0, 16, 0, 0])):
    sys.exit("TC(238,1) builder drifted from the AS-4 unit: "
             + units[6])
units[7] = tc(7, 0xEE, 2, b"")
# TC(238,10): start 0 s, end 0x0FFFFFFF s, averaging 16
units[8] = tc(8, 0xEE, 10, bytes([0, 0, 0, 0, 0x0F, 0xFF, 0xFF, 0xFF,
                                  0, 16]))
# AS-5 legs (the app-data layouts per TcService237.c:66-144,
# TCData.h:103-105 / HWConfig.c:974-976 + TCData.h:142-144 /
# TcService238.c:500-506):
# TC(237,18): param id 477 = 0x01DD (NUMBER_ASPECT_GAPS_ALLOWED,
# ParamMgmtDef.h:547), sub-id 0, value 100
units[9] = tc(9, 0xED, 18, bytes([0x01, 0xDD, 0, 0, 0, 0, 0x64]))
# TC(237,19): read param 0x01DD back (TM(237,20) on 0x5A8 must echo it)
units[10] = tc(10, 0xED, 19, bytes([0x01, 0xDD, 0]))
# TC(236,6): system 3 = E_SYSTEM_ASPECT (HWConfig.h:13), parameter 1 =
# E_ASP_PARAM_CONFIG_SUMMED_VALUES (HWConfig.h:48), value 1 = AVG2
units[11] = tc(11, 0xEC, 6, bytes([3, 1, 0, 1]))
# the averaging-64 request pair of the #1031-shaped legs
units[12] = tc238_1(12, 1, 0, 64)
units[13] = tc(13, 0xEE, 10, bytes([0, 0, 0, 0, 0x0F, 0xFF, 0xFF, 0xFF,
                                    0, 64]))
# the second epoch (--step-1031): NOMINAL again, CONF again
units[14] = tc(14, 0xEC, 1, bytes([5, 0]))
units[15] = tc(15, 0xEC, 1, bytes([4, 0]))
# the second-epoch request: urn 2, window [330 s, ...] inside the second
# file (the first file ends ~T_exit 1 = ~325 s, the second starts ~337 s)
units[16] = tc238_1(16, 2, 405, 64)
units[17] = tc(17, 0xEE, 2, b"")
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
TC_237_18="${TC_ARR[7]}"
TC_237_19="${TC_ARR[8]}"
TC_236_6_AVG2="${TC_ARR[9]}"
TC_238_1_64="${TC_ARR[10]}"
TC_238_10_64="${TC_ARR[11]}"
TC_NOM2="${TC_ARR[12]}"
TC_CONF3="${TC_ARR[13]}"
TC_238_1_E2="${TC_ARR[14]}"
TC_238_2_E2="${TC_ARR[15]}"

# The aspect-scene no-op over QMP (@75; the negative sends it too, the
# QMP path and the timing stay the same).
qom_set() {  # path property json-value
    printf '{"execute": "qom-set", "arguments": %s' \
        "{\"path\": \"$1\", \"property\": \"$2\", \"value\": $3}}"
}

# The TC(238,1) group's answer differs: data in the completing legs
# (forbid the failure events), the 0x5440 event on the error legs
# (--jitter, --drift, --avg, --step-1031: the first forced packet eats
# the budget, the second gap has none left -> FSW_ERROR,
# AspectCtrl.c:297-315 + SciReport.c:461-477 -> TM(5,2) 0x5440 with the
# urn u32, SciReport.c:470-474; the 0x543F event must never fire - there
# is data). The 0x5440 expect carries the urn, so --step-1031's later
# urn-2 failure would not match it.
if [ "$NEGATIVE" -eq 1 ]; then
    # TM(5,2): the EID is a u16 (TM_5.c:212), 0x543F then urn u32 1
    ASK1_ARGS=(--expect "0x5A7:5:2:543f00000001@45"
               --forbid "0x5A7:5:2:5440"
               --forbid "0x5A7:5:2:5446")
elif [ "$LEG" = "jitter" ] || [ "$LEG" = "drift" ] || \
        [ "$LEG" = "avg" ] || [ "$LEG" = "step-1031" ]; then
    # a missed window ends the probe run: >= 60 s on the #1031-shaped
    # legs (two 30 s Background slots), 90 s on --avg (Amendment B4)
    case "$LEG" in
        avg) ASK1_WIN=90 ;;
        drift | step-1031) ASK1_WIN=60 ;;
        *) ASK1_WIN=45 ;;
    esac
    ASK1_ARGS=(--forbid "0x5A7:5:2:543f"
               --expect "0x5A7:5:2:544000000001@$ASK1_WIN"
               --forbid "0x5A7:5:2:5446")
else
    ASK1_ARGS=(--forbid "0x5A7:5:2:543f"
               --forbid "0x5A7:5:2:5440"
               --forbid "0x5A7:5:2:5446")
fi
# The TC(238,10) group's completion report: TM(1,8) FID_48435 on the
# error legs and in the negative, TM(1,7) in the completing ones. The
# plain TM(1,8) forbid of the NOMINAL group must be gone by then: it is
# disarmed at +325 s (the group's send was at +70 s, the expected report
# comes at +400 s). The hex carries the request's own seq byte (8, or D
# for the averaging-64 TC of the #1031-shaped legs).
if [ "$NEGATIVE" -eq 1 ]; then
    ASK2_ARGS=(--expect "0x5A1:1:8:1DACC008BD33")
    NOM_FORBID_18=(--forbid "0x5A1:1:8@325")
elif [ "$LEG" = "avg" ] || [ "$LEG" = "step-1031" ]; then
    # Amendment B: no TC(238,10) path on the two-epoch legs; any TM(1,8)
    # (e.g. --avg's TC(236,6) rejected as wrong mode) fails the run
    ASK2_ARGS=()
    NOM_FORBID_18=(--forbid "0x5A1:1:8")
elif [ "$LEG" = "jitter" ]; then
    ASK2_ARGS=(--expect "0x5A1:1:8:1DACC008BD33")
    NOM_FORBID_18=(--forbid "0x5A1:1:8@325")
elif [ "$LEG" = "drift" ]; then
    ASK2_ARGS=(--expect "0x5A1:1:8:1DACC00DBD33")
    NOM_FORBID_18=(--forbid "0x5A1:1:8@325")
else
    ASK2_ARGS=(--expect "0x5A1:1:7:1DACC008")
    NOM_FORBID_18=(--forbid "0x5A1:1:8")
fi
# the #1031-shaped legs request with averaging 64 (TC seq 12/13 instead
# of 6/8, so the TM(1,x) acceptance echoes carry those seq bytes)
case "$LEG" in
    drift | step-1031 | avg)
        TC_A1="$TC_238_1_64"; TC_A2="$TC_238_2"; TC_A10="$TC_238_10_64"
        ACK1="1DACC00C"; ACK10="1DACC00D" ;;
    *)
        TC_A1="$TC_238_1"; TC_A2="$TC_238_2"; TC_A10="$TC_238_10"
        ACK1="1DACC006"; ACK10="1DACC008" ;;
esac
# the leg's knob group(s): a QMP qom-set at @150 (--drift applies the
# rate at @55, before NOMINAL, so the whole archived stream - and the
# estimate pair - is drifted), or --avg's TC(236,6) at @150
KNOB_ARGS=()
KNOB_AT=150
case "$LEG" in
    jitter)
        KNOB_ARGS=(--qmp-cmd "$(qom_set /machine/stixctrl \
            as-jitter-next-ms 6)") ;;
    step-gap)
        KNOB_ARGS=(--qmp-cmd "$(qom_set /machine/stixctrl \
            as-step-next-ms 6)") ;;
    drop)
        KNOB_ARGS=(--qmp-cmd "$(qom_set /machine/stixctrl \
            as-drop-next 2)") ;;
    step-1031c)
        KNOB_ARGS=(--qmp-cmd "$(qom_set /machine/stixctrl \
            as-step-next-ms -1)") ;;
    drift)
        KNOB_AT=55
        KNOB_ARGS=(--qmp-cmd "$(qom_set /machine/stixctrl \
            as-rate-ppm -2000)") ;;
esac
# The request/list times and the extra groups per leg (Amendment B).
# Default: the urn-1 request @340, the list @350, the TC(238,10) path
# @400. The two-epoch legs answer every request in CONF and drop the
# TC(238,10) path:
# --avg: @320 CONF (file 1, Avg16), @332 TC(236,6) 03 01 00 01 = Avg2
#   (12 s after CONF: the NOMINAL->CONF mode commits only after the
#   flush, OpModeMgmt.c:287-302/:702), @334/@335 budget 100 + read-back,
#   @340 NOMINAL (epoch 2, 126 ms blocks), @400 CONF (file 2), @410 the
#   averaging-64 request over both files, @412 the list.
# --step-1031: @340 the epoch-1 request and @342 the list in CONF, then
#   @400 NOMINAL (epoch 2), @460 CONF (file 2), @470 the urn-2 request
#   with the window start 405 s (SCET counts from boot; file 1 ends
#   ~325 s, file 2 ~460 s; the overlap match finds file 2 only,
#   MemFilesystemHelper.c:57-71), @472 its list.
T_REQ=340
T_LIST=350
PRE_REQ_ARGS=()
E2_ARGS=()
A10_ARGS=(
    --send-at 400 --send-hex "$TC_A10"
    --expect "0x5A1:1:1:$ACK10"
    ${ASK2_ARGS+"${ASK2_ARGS[@]}"}
)
ql_flush() { # the QL flush TM(21,6) SSIDs 30-34 of a NOMINAL->CONF
    printf '%s\n' --expect "0x5DC:21:6:1e@15" --expect "0x5DC:21:6:1f@15" \
        --expect "0x5DC:21:6:20@15" --expect "0x5DC:21:6:21@15" \
        --expect "0x5DC:21:6:22@15"
}
mapfile -t QL_FLUSH < <(ql_flush)
case "$LEG" in
    avg)
        T_REQ=410
        T_LIST=412
        A10_ARGS=()
        PRE_REQ_ARGS=(
            --send-at 332 --send-hex "$TC_236_6_AVG2"
            --expect "0x5A1:1:1:1DACC00B"
            --expect "0x5A1:1:7:1DACC00B"
            --send-at 334 --send-hex "$TC_237_18"
            --expect "0x5A1:1:1:1DACC009"
            --expect "0x5A1:1:7:1DACC009"
            --send-at 335 --send-hex "$TC_237_19"
            --expect "0x5A1:1:1:1DACC00A"
            --expect "0x5A1:1:7:1DACC00A"
            --expect "0x5A8:237:20:01DD0000000064"
            --send-at 340 --send-hex "$TC_NOM2"
            --expect "0x5A1:1:1:1DACC00E"
            --expect "0x5A7:5:1:00210405@10"
            --expect "0x5A1:1:7:1DACC00E"
            --send-at 400 --send-hex "$TC_CONF3"
            --expect "0x5A1:1:1:1DACC00F"
            --expect "0x5A7:5:1:00210504@10"
            --expect "0x5A1:1:7:1DACC00F"
            "${QL_FLUSH[@]}"
            --forbid "0x5A7:5:1:00210402"
        ) ;;
    step-1031)
        T_LIST=342
        A10_ARGS=()
        E2_ARGS=(
            --send-at 400 --send-hex "$TC_NOM2"
            --expect "0x5A1:1:1:1DACC00E"
            --expect "0x5A7:5:1:00210405@10"
            --expect "0x5A1:1:7:1DACC00E"
            --send-at 460 --send-hex "$TC_CONF3"
            --expect "0x5A1:1:1:1DACC00F"
            --expect "0x5A7:5:1:00210504@10"
            --expect "0x5A1:1:7:1DACC00F"
            "${QL_FLUSH[@]}"
            --forbid "0x5A7:5:1:00210402"
            --send-at 470 --send-hex "$TC_238_1_E2"
            --expect "0x5A1:1:1:1DACC010"
            --expect "0x5A1:1:7:1DACC010@60"
            --forbid "0x5A7:5:2:543f"
            --forbid "0x5A7:5:2:5440"
            --forbid "0x5A7:5:2:5446"
            --send-at 472 --send-hex "$TC_238_2_E2"
            --expect "0x5A1:1:1:1DACC011"
            --expect "0x5A1:1:7:1DACC011"
            --expect "0x5A8:238:3@60"
        ) ;;
esac

# TM(5,x) on 0x5A7, the subtype from the EID's severity nibble (1 ->
# TM(5,1), 5 -> TM(5,2), 9 -> TM(5,3)); TM(1,x) on 0x5A1; TM(237,20) and
# TM(238,3) on 0x5A8; TM(21,6) on 0x5DC, matched on the SSID (the first
# application byte).
# the gap-budget TCs of the #1031-shaped legs (read-back check: a wrong
# param index fails loudly here instead of silently leaving the budget
# at 1)
BUDGET_ARGS=()
if [ "$LEG" = "drift" ] || [ "$LEG" = "step-1031" ]; then
    BUDGET_ARGS=(
        --send-at 330 --send-hex "$TC_237_18"
        --expect "0x5A1:1:1:1DACC009"
        --expect "0x5A1:1:7:1DACC009"
        --send-at 331 --send-hex "$TC_237_19"
        --expect "0x5A1:1:1:1DACC00A"
        --expect "0x5A1:1:7:1DACC00A"
        --expect "0x5A8:237:20:01DD0000000064"
    )
fi
# the knob groups go to @55 (--drift's rate, pre-NOMINAL) or @150
KNOB55_ARGS=()
KNOB150_ARGS=()
# each group names its own time: tmtc-probe.py keeps the latest
# --send-at, so a bare group fires with whatever group precedes it
if [ "${#KNOB_ARGS[@]}" -gt 0 ]; then
    case "$KNOB_AT" in
        55) KNOB55_ARGS=(--send-at 55 "${KNOB_ARGS[@]}") ;;
        150) KNOB150_ARGS=(--send-at 150 "${KNOB_ARGS[@]}") ;;
    esac
fi

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
    ${KNOB55_ARGS+"${KNOB55_ARGS[@]}"}
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
    ${KNOB150_ARGS+"${KNOB150_ARGS[@]}"}
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
    ${BUDGET_ARGS+"${BUDGET_ARGS[@]}"}
    ${PRE_REQ_ARGS+"${PRE_REQ_ARGS[@]}"}
    --send-at "$T_REQ" --send-hex "$TC_A1"
    --expect "0x5A1:1:1:$ACK1"
    --expect "0x5A1:1:7:$ACK1"
    ${ASK1_ARGS+"${ASK1_ARGS[@]}"}
    --send-at "$T_LIST" --send-hex "$TC_A2"
    --expect "0x5A1:1:1:1DACC007"
    --expect "0x5A1:1:7:1DACC007"
    --expect "0x5A8:238:3"
    ${A10_ARGS+"${A10_ARGS[@]}"}
    ${E2_ARGS+"${E2_ARGS[@]}"}
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

# --step-1031's knob cannot be a fixed-time qom-set: the step must arm
# at the FIRST archived flip's own block_seq (the first stixasp_push
# after the epoch's skip + 1, D-ASh row 7 / re-review BLOCKER B), which
# is only known once the trace shows that push. A watcher polls
# asp.trace, then arms over QMP:
#   as-step-at-block = <first appended push's block_seq + 1>
#   as-step-next-ms  = -1
# (at-block first, then the step: the arming resolves against the live
# block_seq, stix_aspect.c's step setters). The armed value lands in
# step-armed.log for the post-run placement check.
read -r -d '' STEP_WATCHER_PY <<'EOF' || true
import json, socket, sys, time

trace, qmp_path, log = sys.argv[1], sys.argv[2], sys.argv[3]
# the archive's first record is the NOMINAL epoch's first appended push:
# the ROTB also accepts pushes at boot (before the FSW puts it into test
# mode), so "the first appended push" is the first one that FOLLOWS a
# not-appended (rotb-testmode/-disabled) push
base = None
end = time.monotonic() + 400
while base is None:
    try:
        with open(trace) as f:
            seen_off = False
            for line in f:
                if "stixasp_push" not in line:
                    continue
                if " appended," not in line:
                    seen_off = True
                elif seen_off:
                    base = int(line.split("block ")[1].split()[0])
                    break
    except OSError:
        pass
    if base is not None or time.monotonic() > end:
        break
    time.sleep(0.05)
if base is None:
    print("STEP-WATCHER: no appended push seen", flush=True)
    sys.exit(1)
target = base + 1
s = None
for attempt in range(100):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(qmp_path)
        break
    except OSError:
        time.sleep(0.1)
if s is None:
    print("STEP-WATCHER: no QMP connection", flush=True)
    sys.exit(1)
f = s.makefile("rw")
json.loads(f.readline())            # the greeting
f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
f.flush()
while json.loads(f.readline()).get("event"):
    pass
for prop, val in (("as-step-at-block", target),
                  ("as-step-next-ms", -1)):
    f.write(json.dumps({"execute": "qom-set", "arguments": {
        "path": "/machine/stixctrl", "property": prop,
        "value": val}}) + "\n")
    f.flush()
    reply = json.loads(f.readline())
    while reply.get("event"):
        reply = json.loads(f.readline())
    if "error" in reply:
        print(f"STEP-WATCHER: qom-set {prop} failed: {reply}",
              flush=True)
        sys.exit(1)
with open(log, "w") as out:
    out.write(f"STEP-ARMED at-block={target} first-push={base}\n")
print(f"STEP-WATCHER: armed step at block_seq {target} "
      f"(first appended push {base})", flush=True)
EOF

if [ "$DRY_RUN" -eq 1 ]; then
    echo "DRY RUN — the commands aspect-smoke would run ($MODE):"
    printf '(cd %s && timeout %s %s ' "$RUN_DIR" "$((DURATION + 30))" \
        "$QEMU_BIN"
    printf '%q ' "${QEMU_ARGS[@]}"
    printf '> qemu-stdout.aspect.log 2>&1)\n'
    printf 'python3 -u %s ' "$PROBE"
    printf '%q ' "${PROBE_ARGS[@]}"
    printf '\n'
    if [ "$LEG" = "step-1031" ]; then
        printf 'python3 -u -c %q %q %q %q\n' "$STEP_WATCHER_PY" \
            "$RUN_DIR/asp.trace" "$QMP_SOCK" \
            "$RUN_DIR/step-armed.log"
    fi
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
      qmp.sock step-armed.log step-watcher.log
truncate -s 17716740096 flash.bin
# the trace events: the aspect generator's scene, tick, flips, pushes
# and drops (the flips' block_seq/SCET are the AS-5 legs' placement
# evidence)
printf '%s\n' stixasp_scene stixasp_tick_arm stixasp_flip stixasp_push \
    stixasp_drop stixasp_jitter stixasp_step > asp.events

PROBE_PID=""
QEMU_PID=""
WATCH_PID=""
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
    if [ -n "$WATCH_PID" ]; then
        kill "$WATCH_PID" 2>/dev/null || true
        wait "$WATCH_PID" 2>/dev/null || true
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

# the --step-1031 placement watcher (D-ASh row 7): arms the -1 ms step
# at the first appended push's block_seq + 1, over QMP
if [ "$LEG" = "step-1031" ]; then
    python3 -u -c "$STEP_WATCHER_PY" "$RUN_DIR/asp.trace" \
        "$QMP_SOCK" "$RUN_DIR/step-armed.log" >> step-watcher.log 2>&1 &
    WATCH_PID=$!
fi

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

# 2. the SSID 42 burst answers (both request paths in one dump), the
# leg's D-ASh signature through aspect-check.py
ASPECT_ARGS=(asp.dump.hex)
case "$LEG" in
    negative)
        ASPECT_ARGS+=(--expect-none) ;;
    ideal | step-1031c)
        ASPECT_ARGS+=(--expect-ideal 1 0 --summing 16 --averaging 16) ;;
    jitter)
        ASPECT_ARGS+=(--expect-forced-abort 1 0 --summing 16 --averaging 16) ;;
    step-gap)
        ASPECT_ARGS+=(--expect-forced-complete 1 0 --summing 16 \
            --averaging 16) ;;
    drop)
        # D-ASh row 5 as corrected by the first live run: the archive
        # stores each block with the previous block's end
        # (ArchiveManager.c, the "override the time of the last ending
        # chunk" step), so the 2 missing blocks show one record later
        # as a ~3-block gap: one gap event, the request completes
        ASPECT_ARGS+=(--expect-forced-complete 1 0 --summing 16 \
            --averaging 16) ;;
    avg)
        # Amendment B2: one urn-1 request over the Avg16 and Avg2 epochs
        ASPECT_ARGS+=(--expect-stale 1) ;;
    drift)
        ASPECT_ARGS+=(--expect-tiny 1 0) ;;
    step-1031)
        # Amendment B3: no TC(238,10) path, so no urn 0
        ASPECT_ARGS+=(--expect-tiny 1 --expect-epoch2 2) ;;
esac
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
# this TRACE_ERROR (ArchiveManager.c:140 Det, :219 Aspect); once per
# flush cycle that carried data - the one boot epoch on every leg except
# --step-1031, whose second NOMINAL epoch follows the @320 flush that
# reset the skip flag (ArchiveManager.c:270-278) -> a second line of
# each; absent with as-enable=false
SKIPPED_DET="$(grep -c 'Skipped first Det data chunk' uart.aspect.txt \
    || true)"
SKIPPED_ASP="$(grep -c 'Skipped first Aspect data chunk' \
    uart.aspect.txt || true)"
EXPECT_SKIP=1
# the two-epoch legs flush twice with data (Amendment B)
[ "$LEG" = "step-1031" ] || [ "$LEG" = "avg" ] && EXPECT_SKIP=2
# as-enable=false stops only the aspect generator: the detector still
# archives, so its first chunk is still skipped once (AS-4: Det 1, Asp 0)
EXPECT_DET="$EXPECT_SKIP"
EXPECT_ASP="$EXPECT_SKIP"
[ "$NEGATIVE" -eq 1 ] && EXPECT_ASP=0
# the FSW's own gap-terminal evidence (AspectCtrl.c:312): once per
# failed request - the urn-1 and the TC(238,10) requests on the error
# legs (--jitter, --drift, --avg, --step-1031; --step-1031's epoch-2
# request succeeds), none on the completing legs
TERMINATE="$(grep -c 'Terminating aspect report due to too many gaps' \
    uart.aspect.txt || true)"
case "$LEG" in
    jitter | drift) EXPECT_TERMINATE=2 ;;
    # the two-epoch legs have no TC(238,10) path: one failed request
    avg | step-1031) EXPECT_TERMINATE=1 ;;
    *) EXPECT_TERMINATE=0 ;;
esac
[ "$NEGATIVE" -eq 1 ] && EXPECT_TERMINATE=0
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
elif [ "$SKIPPED_DET" -eq "$EXPECT_DET" ] && \
        [ "$SKIPPED_ASP" -eq "$EXPECT_ASP" ] && \
        [ "$TERMINATE" -eq "$EXPECT_TERMINATE" ]; then
    echo "CHECK uart-chunk: ok ('Skipped first Det data chunk'" \
        "$SKIPPED_DET, 'Skipped first Aspect data chunk' $SKIPPED_ASP," \
        "$TERMINATE gap-terminate lines, no chunk/archive/filesystem error)"
else
    echo "CHECK uart-chunk: FAIL ($SKIPPED_DET 'Skipped first Det data" \
        "chunk' and $SKIPPED_ASP 'Skipped first Aspect data chunk' lines," \
        "$TERMINATE gap-terminate lines; expected $EXPECT_DET," \
        "$EXPECT_ASP and $EXPECT_TERMINATE)"
    FAILED+=(uart-chunk)
fi

# 4. the generator's own trace: flips and pushes on the positive, none
# on the negative; the legs' knob lines are the placement evidence (a
# push result other than "appended" - rotb-disabled/rotb-testmode in
# the pre-NOMINAL epochs - is printed, not asserted; an unexpected drop
# is).
if [ -f asp.trace ]; then
    PUSH_N="$(grep -c 'stixasp_push' asp.trace || true)"
    FLIP_N="$(grep -c 'stixasp_flip' asp.trace || true)"
    DROP_N="$(grep -c 'stixasp_drop' asp.trace || true)"
    APP_N="$(grep -c 'stixasp_push.* appended,' asp.trace || true)"
    JIT_N="$(grep -c 'stixasp_jitter' asp.trace || true)"
    STEP_N="$(grep -c 'stixasp_step' asp.trace || true)"
    # the NOMINAL epoch's first appended push (the boot-time pushes
    # before the FSW's ROTB test mode are appended too, but never
    # archived): the first appended push after a not-appended one
    APP1_SEQ="$(sed -n 's/.*push: block \([0-9]*\) \([a-z-]*\),.*/\1 \2/p' \
        asp.trace | awk '$2 != "appended" { off = 1; next }
                         off && !done { print $1; done = 1 }')"
    # (no early awk exit: under pipefail a SIGPIPE'd sed ends the run)
else
    PUSH_N=""; FLIP_N=""; DROP_N=""; APP_N=""; JIT_N=""; STEP_N=""
    APP1_SEQ=""
fi
if [ "$NEGATIVE" -eq 1 ]; then
    if [ "$FLIP_N" = 0 ] && [ "$PUSH_N" = 0 ]; then
        echo "CHECK trace: ok (as-enable=false: 0 flips, 0 pushes)"
    else
        echo "CHECK trace: FAIL (${FLIP_N:-?} flips, ${PUSH_N:-?} pushes" \
            "with as-enable=false; expected 0 and 0)"
        FAILED+=(trace)
    fi
else
    TRACE_BAD=""
    [ "${PUSH_N:-0}" -ge 100 ] || TRACE_BAD="too few pushes"
    case "$LEG" in
        drop)
            [ "${DROP_N:-1}" -eq 2 ] ||
                TRACE_BAD="$TRACE_BAD, want the 2 knob drops"
            [ "$(grep -c 'stixasp_drop.* (knob)' asp.trace 2>/dev/null)" \
                = 2 ] || TRACE_BAD="$TRACE_BAD, no knob-drop lines" ;;
        ideal | jitter | step-gap | drift | step-1031 | step-1031c | avg)
            [ "${DROP_N:-1}" -eq 0 ] || TRACE_BAD="$TRACE_BAD, drops" ;;
    esac
    case "$LEG" in
        jitter)
            [ "${JIT_N:-0}" -eq 1 ] &&
                grep -q 'stixasp_jitter jitter: block [0-9]*, delta=6 ms' \
                    asp.trace ||
                TRACE_BAD="$TRACE_BAD, no +6 ms jitter line" ;;
        step-gap)
            [ "${STEP_N:-0}" -ge 1 ] &&
                grep -q 'stixasp_step step: block [0-9]*, delta=6 ms' \
                    asp.trace ||
                TRACE_BAD="$TRACE_BAD, no +6 ms step line" ;;
        step-1031c)
            [ "${STEP_N:-0}" -eq 1 ] &&
                grep -q 'stixasp_step step: block [0-9]*, delta=-1 ms' \
                    asp.trace &&
                [ "$(sed -n 's/.*step: block \([0-9]*\), delta=.*/\1/p' \
                    asp.trace | head -1)" -gt \
                    "$(( ${APP1_SEQ:-0} + 5 ))" ] ||
                TRACE_BAD="$TRACE_BAD, the -1 ms step is not mid-window" ;;
        drift)
            grep -q 'stixasp_scene scene pattern=.* rate_ppm=-2000 ' \
                asp.trace ||
                TRACE_BAD="$TRACE_BAD, no -2000 ppm scene line" ;;
        step-1031)
            # the placement IS the leg: the step's block_seq must be the
            # watcher's armed one = the first appended push + 1; a miss
            # degrades to the control signature and is reported as a
            # placement failure, not a falsification
            ARMED="$(cut -d= -f2 step-armed.log 2>/dev/null | \
                cut -d' ' -f1)"
            STEP_SEQ="$(sed -n \
                's/.*step: block \([0-9]*\), delta=.*/\1/p' asp.trace | \
                head -1)"
            if [ "${STEP_N:-0}" -ne 1 ] || [ -z "$ARMED" ] ||
                [ "$STEP_SEQ" != "$ARMED" ] ||
                [ "$ARMED" != "$((${APP1_SEQ:-0} + 1))" ]; then
                TRACE_BAD="$TRACE_BAD, PLACEMENT: step at block_seq" \
                    "${STEP_SEQ:-none}, armed ${ARMED:-none}, first" \
                    "appended push ${APP1_SEQ:-none} (want armed =" \
                    "first+1 and the step trace at armed)"
            fi ;;
    esac
    if [ -z "$TRACE_BAD" ]; then
        echo "CHECK trace: ok ($PUSH_N pushes of $FLIP_N flips, $APP_N" \
            "appended, $DROP_N drops, $JIT_N jitter, $STEP_N step)"
        if [ "$APP_N" -ne "$PUSH_N" ]; then
            echo "NOTE trace: $((PUSH_N - APP_N)) push(es) not appended" \
                "(rotb-disabled/testmode before NOMINAL)"
        fi
    else
        echo "CHECK trace: FAIL (${PUSH_N:-?} pushes of ${FLIP_N:-?}" \
            "flips, ${DROP_N:-?} drops, ${JIT_N:-?} jitter," \
            "${STEP_N:-?} step:$TRACE_BAD)"
        FAILED+=(trace)
    fi
fi

GOT="${FAILED[*]:-}"
if [ -n "$GOT" ]; then
    echo "ASPECT-SMOKE ($MODE): FAIL (failed checks: $GOT$WHY)${STIX_SLOT_TAG}"
    exit 1
fi
# PASS: the run dir may be reused; in slot mode drop the flash copy
# (asp.trace stays: it is the placement evidence)
rm -f .unfinished
stix_slot_cleanup 0 flash.bin
case "$LEG" in
    negative)
        echo "ASPECT-SMOKE (negative): PASS (no aspect file, 0x543F and" \
            "TM(1,8) FID_48435, no SSID 42 packet)${STIX_SLOT_TAG}" ;;
    ideal)
        echo "ASPECT-SMOKE: PASS (both request paths answered D-ASh row 1:" \
            "510-sample packets, summing 16, averaging 16, continuous)" \
            "${STIX_SLOT_TAG}" ;;
    jitter)
        echo "ASPECT-SMOKE (--jitter): PASS (D-ASh row 2: two gap events," \
            "budget exhausted, 0x5440 + TM(1,8) FID_48435, early end)" \
            "${STIX_SLOT_TAG}" ;;
    step-gap)
        echo "ASPECT-SMOKE (--step-gap): PASS (D-ASh row 3: one gap event," \
            "request completes with one forced packet)${STIX_SLOT_TAG}" ;;
    drift)
        echo "ASPECT-SMOKE (--drift): PASS (D-ASh row 4: inc 15, a gap at" \
            "every block, ~100 forced 0/1-sample packets, 0x5440 +" \
            "TM(1,8))${STIX_SLOT_TAG}" ;;
    drop)
        echo "ASPECT-SMOKE (--drop): PASS (D-ASh row 5, corrected: the" \
            "2 missing blocks appear one record later as a gap; one gap" \
            "event, the request completes with one forced packet)" \
            "${STIX_SLOT_TAG}" ;;
    avg)
        echo "ASPECT-SMOKE (--avg): PASS (Amendment B2: one request over" \
            "an Avg16 and an Avg2 epoch; the file-1 estimate is stale for" \
            "every Avg2 record -> gap-forced 0/1-sample packets, 0x5440)" \
            "${STIX_SLOT_TAG}" ;;
    step-1031)
        echo "ASPECT-SMOKE (--step-1031): PASS (D-ASh row 7: inc 15 from" \
            "the straddling pair, ~100 forced 0/1-sample packets, 0x5440," \
            "the second epoch succeeds)${STIX_SLOT_TAG}" ;;
    step-1031c)
        echo "ASPECT-SMOKE (--step-1031-control): PASS (the -1 ms step" \
            "mid-window is silent: the ideal signature, no gap)" \
            "${STIX_SLOT_TAG}" ;;
esac
exit 0