#!/usr/bin/env bash
# DETECTOR smoke: the detector-model gate for tiers 0+1 (detector plan
# D-3, docs/stix-port/2026-11-11-detector-plan.md), with --flare for the
# synthetic sky and the flare switch (tier 2, D-4), and in every mode for
# the nominal integration's ROTB science chunks (tier 3, D-5). Boots the
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
# blocks (word 0x84000 | reg << 7, reg 1..12, delivered_grp_mask != 0);
# a run ends at an FSM strobe or a > 5 s gap. Each run must be exactly
# 384 (every reg 1..12 x every grp 0..15 twice, i.e. 32 ASICs), inside
# the NOMINAL window, at least 3 runs, and no undelivered read address
# block. The window comes from the same trace by ORDER: QEMU also traces
# stixdet_fsm_strobe, and NOMINAL runs from the strobe that sets the FSM
# Enable to the one that clears it.
# Clocks: no gate timing uses the wall clock. T_nom, T_exit and the QMP
# switch times are the probe's CLOCK_MONOTONIC [m=...] stamps, so the
# NOMINAL window the ql checks get cannot shrink when the wall clock is
# stepped (WSL's timesyncd steps it back ~2.5 s every 32 s poll; three ql
# FAILs came from that), and the trace check compares no times across
# clocks. The wall/monotonic ratio over the run is printed for the
# record only.
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
# monotonic QMP send stamps) minus T_nom: the SSID 30 band-0 median of the flare
# samples must be >= 8 compressed steps above the quiet median. The
# SSID 34 flags in the flare window are printed, not asserted. The
# QEMU process's user+sys CPU seconds are printed in every mode, for
# the D-4 cost record (det-source=zero vs synthetic).
# --flare --negative sends det-flare false at @135 (no flip; the QMP
# path stays exercised) and passes only when exactly ql-contrast
# fails.
# ROTB science chunks (D-5), in every mode: QEMU also traces
# stixdet_scene, stixdet_nominal_swap, stixdet_chunk, stixdet_chunk_drop
# and stixctrl_rotb_cmd into the same trace file, and
# chunk-trace-check.py checks (all on the virtual clock and tick counts
# the model writes into the lines, never the wall clock): every nominal
# swap made a 49-block chunk, none dropped; every quiet integration took
# exactly 200 ticks (the FSW's max time, 20 s), at least 5 of them (1
# with --flare, whose quiet stretches are short); the chunks come at the
# integration cadence; and GETBLOCK >= 49 x (chunks - 1), i.e. the FSW
# drained the ring (check "chunk"). --flare adds "chunk-flare": at least
# 40 integrations inside the flare window, each exactly 10 ticks (the
# min time, 1 s; the 1800 events come within a tick). The UART must show
# "Skipped first Det data chunk" (ArchiveManager.c:140, the chunk reached
# the archive) exactly once, and no *E line from the chunk path, the
# archive or the filesystem (check "uart-chunk"). The flash.bin space
# allocated by the run is printed for the NAND cost record.
# --no-rotb runs the zero sky with -global grlib-stixctrl.det-rotb=off:
# the nominal swaps go on but no chunk is made. It passes only when
# exactly the two chunk checks fail as planned: "uart-chunk" (no "Skipped
# first Det data chunk") and "chunk" (chunk-trace-check.py --expect-none:
# >= 3 nominal swaps, no chunk at all).
# --flare --negative must now fail exactly ql-contrast and chunk-flare
# (no flip, so neither the LC contrast nor the 1 s integrations).
# D-6 (the index-space end-to-end check), in the --flare modes only: the
# D-3 modes keep their zero sky and their byte-for-byte checks (a
# deposit would break the zero-flush contract), while the --flare flush
# checks tolerate extra counts and the deposit lands in their quiet
# phase (@85 < 135). At @85 -- T_nom is due within 10 s of the @70 TC,
# so this is T_nom + 5..15 s, always inside the first 8 QL periods
# (T_nom + 32 s) that SSID 32 sums -- the probe injects 250 counts at
# fpga_det 5, pix 0, ebin 20 through the write-only det-inject property
# (qom-set /machine/stixctrl det-inject "fpga_det=5,pix=0,ebin=20,
# count=250"). In the TM the detector index is a subcol: the target
# sample is the one whose detector byte is utils_subcolIdFromFpgaId[5]
# = 10 (STIX-FSW 600a7a72, ASW/ASW/src/utils.c:113-119; the one
# FSW-sourced index value in the scripts -- stix_det.c already carries
# the full table, accepted-diffs §32). A third check, ql-inject,
# runs ql-check.py --ssid32-inject 10 20 0x2F 0x10 on the dump: in the
# one SSID 32 dump of the exit flush the target sample's byte 21
# (= ebin 20 + 1) is >= 0x2F (the S0 byte of 250; the quiet background
# there is a few counts), byte 21 of every other sample is < 0x10, and
# the target's other bytes are only compared with its subcol 9/11
# neighbours' range and logged (the plan's [L] item). The deposit also
# shows as a one-sample spike in SSID 30 band 3 and SSID 33; the sky
# checks see it, nothing asserts it. --flare --negative injects too
# (the deposit does not depend on the flare switch), so its
# expected-failure set stays ql-contrast chunk-flare.
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
# In legacy mode (STIX_PORT_BASE unset) this script, like tc-smoke.sh,
# does NOT take the /tmp/stix-qtest-21000 flock itself; the caller must
# hold it. With STIX_PORT_BASE=auto or a slot number it takes a slot
# (scripts/stix/lib/slot.sh) in every mode, --flare included: the slot's
# TMTC port, <run dir>-s<k>, and slot=k busy= load= on the verdict.
#
# Usage: detector-smoke.sh <qemu-binary> [--flare] [--negative]
#                           [--no-rotb] [--dry-run] [--check-guard]
#   (--flare carries the D-6 det-inject deposit at @85; see above)
#   --no-rotb combines with neither --flare nor --negative.
#   STIX_ASW_ELF            as in tc-smoke.sh
#   STIX_DETECTOR_RUN_DIR   run dir (default /tmp/stix-detector-run).
#                           Canonicalised, then anything at or above
#                           /tmp/stix-uprev-run or /tmp/stix-baseline-run
#                           (or /) is refused, as in nominal-smoke.sh.
#                           Each mode adds its suffix (-neg, -flare,
#                           -flareneg, -norotb; none for the positive),
#                           then slot
#                           mode -s<k>. A run that did not PASS leaves
#                           .unfinished; the next run moves that dir aside
#                           to <dir>.fail-<date>-<pid>, never reusing it.
#   STIX_TMTC_PROBE         the probe script (tests substitute a fake)
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
[--flare] [--negative] [--no-rotb] [--dry-run] [--check-guard]}"
NEGATIVE=0
FLARE=0
NO_ROTB=0
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
        --no-rotb)
            [ "$NO_ROTB" -eq 0 ] || { echo "--no-rotb given twice"; exit 2; }
            NO_ROTB=1 ;;
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
if [ "$NO_ROTB" -eq 1 ] && { [ "$FLARE" -eq 1 ] || [ "$NEGATIVE" -eq 1 ]; }
then
    echo "--no-rotb combines with neither --flare nor --negative"
    exit 2
fi

. "$(dirname "${BASH_SOURCE[0]}")/lib/paths.sh"
ASW_ELF="${STIX_ASW_ELF:-$STIX_DEFAULT_ELF}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_DIR="${STIX_DETECTOR_RUN_DIR:-/tmp/stix-detector-run}"
DURATION="${STIX_DETECTOR_SECONDS:-240}"
PROBE="${STIX_TMTC_PROBE:-$REPO_ROOT/scripts/stix/tmtc-probe.py}"
QLCHECK="$REPO_ROOT/scripts/stix/ql-check.py"
TRACECHECK="$REPO_ROOT/scripts/stix/idefx-trace-check.py"
CHUNKCHECK="$REPO_ROOT/scripts/stix/chunk-trace-check.py"
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

# The run-dir guard of nominal-smoke.sh (review M1 there): canonical
# first (realpath -m resolves a trailing slash, //, /., a symlink or a
# relative path), then refuse the two shared run dirs, anything inside
# them, any parent of them, and /. Checked on the given dir and on the
# final one (with the mode and slot suffixes).
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
# Every mode has its own run dir, in legacy and slot mode: a later mode
# must never overwrite an earlier mode's evidence (a --negative FAIL was
# once lost to the next mode's run).
MODE_SFX=""
[ "$NEGATIVE" -eq 1 ] && MODE_SFX="-neg"
[ "$FLARE" -eq 1 ] && MODE_SFX="-flare"
[ "$FLARE" -eq 1 ] && [ "$NEGATIVE" -eq 1 ] && MODE_SFX="-flareneg"
[ "$NO_ROTB" -eq 1 ] && MODE_SFX="-norotb"
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
# D-6: at @85 (T_nom + 5..15 s, inside the 8 QL periods SSID 32 sums)
# the one-shot det-inject deposit of the index-space check; see the
# header. It runs in both --flare modes.
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
        --send-at 85
        --qmp-cmd "$(qom_set /machine/stixctrl det-inject \
            '"fpga_det=5,pix=0,ebin=20,count=250"')"
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
    "${STIX_SLOT_QEMU_ARGS[@]}"
    -qmp "unix:$QMP_SOCK,server=on,wait=off"
    -serial "file:$RUN_DIR/uart.detector.raw"
    -kernel "$ASW_ELF"
    -msg timestamp=on
    -trace "events=$RUN_DIR/idefx.events,file=$RUN_DIR/idefx.trace"
)
if [ "$FLARE" -eq 0 ]; then
    # the tiers 0+1 zero sky of the D-3 checks
    QEMU_ARGS+=(-global grlib-stixctrl.det-source=zero)
    if [ "$NEGATIVE" -eq 1 ]; then
        QEMU_ARGS+=(-global grlib-stixctrl.det-idefx=off)
    fi
    if [ "$NO_ROTB" -eq 1 ]; then
        QEMU_ARGS+=(-global grlib-stixctrl.det-rotb=off)
    fi
fi
MODE="positive"
[ "$NEGATIVE" -eq 1 ] && MODE="negative"
[ "$FLARE" -eq 1 ] && MODE="flare $MODE"
[ "$NO_ROTB" -eq 1 ] && MODE="no-rotb"

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
    echo "TMTC port ${TMTC_PORT} busy — another QEMU instance" \
        "running?${STIX_SLOT_TAG}"
    exit 2
fi

# A run that did not PASS leaves .unfinished behind; its dir is moved
# aside, never reused, so its evidence survives any later run.
if [ -e "$RUN_DIR/.unfinished" ]; then
    KEPT="$RUN_DIR.fail-$(date +%Y%m%d-%H%M%S)-$$"
    mv -T -- "$RUN_DIR" "$KEPT"
    echo "detector-smoke: kept the unfinished run's dir as $KEPT"
fi
mkdir -p "$RUN_DIR"
cd "$RUN_DIR"
touch .unfinished
# Fresh flash every run: clear only this run dir's own files, by
# explicit path (the shared run dirs are refused above), then pre-create
# an empty sparse flash.bin of the full size, so QEMU never creates one
# itself (an older build fills a missing one densely, 16.5 GB). The dump is
# appended to, so it must start empty.
rm -f flash.bin uart.detector.raw uart.detector.txt detector.probe.log \
      qemu-stdout.detector.log idefx.trace ql.dump.hex ql.check.log \
      ql.contrast.log ql.inject.log trace.check.log chunk.check.log \
      chunk-flare.check.log qmp.sock
truncate -s 17716740096 flash.bin
# the trace events: the IDeF-X words, and the FSM strobes that mark the
# NOMINAL window in the same file (one -trace option: a second one would
# reset the trace file); for the D-5 chunk checks the scene (the flare
# window), the nominal swaps, the chunks, the drops and the ROTB commands
printf '%s\n' stixdet_idefx_cmd stixdet_fsm_strobe stixdet_scene \
    stixdet_nominal_swap stixdet_chunk stixdet_chunk_drop \
    stixctrl_rotb_cmd > idefx.events

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

# Clocks: every window below is timed on CLOCK_MONOTONIC (the probe's
# [m=...] stamps), never on the wall clock, which WSL's timesyncd can
# step mid-run. These (realtime, monotonic) pairs from before and after
# the run only report the wall/monotonic ratio, for the record.
# STIX_CLOCK_CMD is a test hook printing "REALTIME MONOTONIC".
clock_pair() {
    if [ -n "${STIX_CLOCK_CMD:-}" ]; then
        "$STIX_CLOCK_CMD"
    else
        python3 -c 'import time
print(f"{time.time():.3f} {time.monotonic():.3f}")'
    fi
}
CLOCK_START="$(clock_pair)"
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
CLOCK_END="$(clock_pair)"
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
    echo "DETECTOR-SMOKE: ERROR (QEMU died or never started;" \
        "qemu rc=$QEMU_RC)${STIX_SLOT_TAG}"
    exit 2
fi
if [ "$PROBE_RC" -eq 124 ]; then
    tail -5 detector.probe.log
    echo "DETECTOR-SMOKE: ERROR (the probe hit its own timeout)${STIX_SLOT_TAG}"
    exit 2
fi
if [ "$PROBE_RC" -eq 2 ]; then
    tail -5 detector.probe.log
    echo "DETECTOR-SMOKE: ERROR (the probe rejected its" \
        "arguments)${STIX_SLOT_TAG}"
    exit 2
fi
if grep -q "could not connect" detector.probe.log 2>/dev/null; then
    tail -5 detector.probe.log
    echo "DETECTOR-SMOKE: ERROR (the probe never connected;" \
        "QEMU rc=$QEMU_RC)${STIX_SLOT_TAG}"
    exit 2
fi

# Every check below runs and records its name in FAILED, so the report
# is complete and --negative can compare the set. The QL checks also
# record why in WHY_QL, which the verdict line repeats: the first
# ql-check mismatch (and how many more), or its FAIL / ERROR line.
FAILED=()
WHY_QL=""
ql_why() { # ql-check log -> one line
    local first n
    first="$(grep -m1 '^QL-CHECK MISMATCH: ' "$1" | cut -d' ' -f3-)" || true
    n="$(grep -c '^QL-CHECK MISMATCH: ' "$1")" || true
    if [ -n "$first" ]; then
        [ "$n" -gt 1 ] && first="$first (+$((n - 1)) more)"
        echo "$first"
    else
        grep -m1 -E '^QL-CHECK: (FAIL|ERROR)' "$1" | cut -d' ' -f2- ||
            echo "no ql-check verdict"
    fi
}
add_why() { # check why
    WHY_QL="$WHY_QL; $1: $2"
}

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

# T_nom / T_exit: the probe's monotonic stamps ([m=...]) of the arrivals
# of 0021 04 05 / 0021 05 04 (the HH:MM:SS are for reading only)
P_NOM='EXPECT OK: 0x5A7:5:1:00210405@10'
P_EXIT='EXPECT OK: 0x5A7:5:1:00210504@10'
mono_of() { # probe-log pattern -> the [m=...] stamp of its first line
    grep -m1 -- "$1" detector.probe.log |
        sed -n 's/.*\[m=\([0-9][0-9.]*\)\].*/\1/p'
}
hms_of() {
    grep -m1 -- "$1" detector.probe.log | cut -d' ' -f1
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

[ -n "$QEMU_CPU" ] && echo "QEMU CPU ($MODE): $QEMU_CPU"
# shellcheck disable=SC2086 # two "REALTIME MONOTONIC" pairs
echo "clock: wall/monotonic over the run $(echo $CLOCK_START $CLOCK_END |
    awk '{ if ($4 > $2) printf "%.3f", ($3 - $1) / ($4 - $2);
           else print "?" }') (for the record; no check uses the wall clock)"

# 2. the QL flush content (zero sky, or with --flare the synthetic sky)
QL_FLUSH_ARG=--expect-zero-flush
[ "$FLARE" -eq 1 ] && QL_FLUSH_ARG=--expect-sky-flush
if [ -z "$NOM_SECONDS" ]; then
    echo "CHECK ql: FAIL (no NOMINAL window: 0021 04 05 or 0021 05 04" \
        "not seen)"
    FAILED+=(ql)
    add_why ql "no NOMINAL window"
elif [ ! -s ql.dump.hex ]; then
    echo "CHECK ql: FAIL (no 0x5DC packet dumped)"
    FAILED+=(ql)
    add_why ql "no 0x5DC packet dumped"
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
        echo "CHECK ql: FAIL ($(ql_why ql.check.log); ql-check" \
            "rc=$QL_RC; see ql.check.log)"
        FAILED+=(ql)
        add_why ql "$(ql_why ql.check.log)"
    fi
fi

# 2b. --flare: the LC band-0 contrast across the flare switch. The
# switch times are the probe's QMP send times, relative to T_nom.
if [ "$FLARE" -eq 1 ]; then
    mapfile -t QMP_M < <(grep '>> qmp .*det-flare' detector.probe.log |
        sed -n 's/.*\[m=\([0-9][0-9.]*\)\].*/\1/p')
    if [ -z "$NOM_SECONDS" ] || [ "${#QMP_M[@]}" -ne 2 ] || \
        [ ! -s ql.dump.hex ]; then
        echo "CHECK ql-contrast: FAIL (no NOMINAL window, no dump, or" \
            "not exactly two det-flare QMP sends: ${#QMP_M[@]})"
        FAILED+=(ql-contrast)
        add_why ql-contrast "no window, no dump or ${#QMP_M[@]} QMP sends"
    else
        read -r FLARE_ON_S FLARE_OFF_S < <(awk -v t="$T_NOM_M" \
            -v a="${QMP_M[0]}" -v b="${QMP_M[1]}" \
            'BEGIN { printf "%.1f %.1f\n", a - t, b - t }')
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
            echo "CHECK ql-contrast: FAIL ($(ql_why ql.contrast.log);" \
                "ql-check rc=$QLC_RC; see ql.contrast.log)"
            FAILED+=(ql-contrast)
            add_why ql-contrast "$(ql_why ql.contrast.log)"
        fi
    fi
fi

# 2c. --flare: the D-6 index-space deposit (see the header). In the TM
# the detector index is a subcol; the target sample's detector byte is
# utils_subcolIdFromFpgaId[5] = 10 (STIX-FSW 600a7a72,
# ASW/ASW/src/utils.c:113-119; the one FSW-sourced index value in the
# scripts -- stix_det.c already carries the full table, accepted-diffs
# §32). The bytes: 0x2F is the S0 byte of the 250 deposited counts
# (monotone, so more counts only raise it), 0x10 the quiet-sky bound.
INJECT_SUBCOL=10
if [ "$FLARE" -eq 1 ]; then
    if [ ! -s ql.dump.hex ]; then
        echo "CHECK ql-inject: FAIL (no 0x5DC packet dumped)"
        FAILED+=(ql-inject)
        add_why ql-inject "no 0x5DC packet dumped"
    else
        set +e
        python3 "$QLCHECK" ql.dump.hex \
            --ssid32-inject "$INJECT_SUBCOL" 20 0x2F 0x10 \
            > ql.inject.log 2>&1
        QLI_RC=$?
        set -e
        grep -E '^QL-CHECK' ql.inject.log || true
        if [ "$QLI_RC" -eq 0 ]; then
            echo "CHECK ql-inject: ok (250 counts at fpga_det 5, pix 0," \
                "ebin 20 seen at subcol $INJECT_SUBCOL, byte 21)"
        else
            echo "CHECK ql-inject: FAIL ($(ql_why ql.inject.log);" \
                "ql-check rc=$QLI_RC; see ql.inject.log)"
            FAILED+=(ql-inject)
            add_why ql-inject "$(ql_why ql.inject.log)"
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
# D-5: the first chunk reached the archive, which drops it with this
# TRACE_ERROR (ArchiveManager.c:140); later chunks are archived silently
SKIPPED_FIRST="$(grep -c 'Skipped first Det data chunk' uart.detector.txt \
    || true)"
# ... and no error from the chunk path, the archive or the filesystem on
# the fresh flash (the FSW TRACE_TAGs of RotAccumulator.c,
# RotAccuCompactor.c, ArchiveManager.c and MemoryMgmt/)
CHUNK_TAGS='Archive|RotAccuComp|RotAccumulator|FS|FSapi|MemStream'
CHUNK_TAGS="$CHUNK_TAGS|MemCommon|FIFO"
CHUNK_ERR="$(grep -E "^[[:space:]]*[0-9:.]+ +($CHUNK_TAGS) +\\*E" \
    uart.detector.txt | grep -v 'Skipped first Det data chunk' | head -3 ||
    true)"
if [ -n "$CHUNK_ERR" ]; then
    echo "CHECK uart-chunk: FAIL (chunk/archive/filesystem error:" \
        "$(echo "$CHUNK_ERR" | head -1 | cut -c1-120))"
    FAILED+=(uart-chunk)
elif [ "$SKIPPED_FIRST" -eq 1 ]; then
    echo "CHECK uart-chunk: ok ('Skipped first Det data chunk' once, no" \
        "chunk/archive/filesystem error)"
else
    echo "CHECK uart-chunk: FAIL ($SKIPPED_FIRST 'Skipped first Det data" \
        "chunk' lines; expected 1)"
    FAILED+=(uart-chunk)
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
# Neither branch compares a time with another clock: the runs are
# placed in NOMINAL by the FSM strobes' order in the same trace.
TRACE_ARGS=(idefx.trace)
[ "$IDEFX_NEG" -eq 1 ] && TRACE_ARGS+=(--expect-none)
set +e
python3 "$TRACECHECK" "${TRACE_ARGS[@]}" > trace.check.log 2>&1
TRACE_RC=$?
set -e
cat trace.check.log
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

# 5. the ROTB science chunks (D-5), from the same trace: every swap
# made a chunk, quiet integrations at the max time, the FSW drained the
# ring. --no-rotb: swaps but no chunk at all (det-rotb=off), which counts
# as failed as planned.
CHUNK_ARGS=(idefx.trace)
if [ "$NO_ROTB" -eq 1 ]; then
    CHUNK_ARGS+=(--expect-none)
elif [ "$FLARE" -eq 1 ]; then
    CHUNK_ARGS+=(--min-quiet 1)
fi
set +e
python3 "$CHUNKCHECK" "${CHUNK_ARGS[@]}" > chunk.check.log 2>&1
CHUNK_RC=$?
set -e
cat chunk.check.log
if [ "$NO_ROTB" -eq 1 ]; then
    if [ "$CHUNK_RC" -eq 0 ]; then
        echo "CHECK chunk: FAIL as planned (nominal swaps, no chunk)"
        FAILED+=(chunk)
    else
        echo "CHECK chunk: FAIL (rc=$CHUNK_RC; --no-rotb wants nominal" \
            "swaps and no chunk)"
        FAILED+=(chunk-not-none)
    fi
elif [ "$CHUNK_RC" -eq 0 ]; then
    echo "CHECK chunk: ok"
else
    echo "CHECK chunk: FAIL (rc=$CHUNK_RC; see chunk.check.log)"
    FAILED+=(chunk)
fi
# 5b. --flare: the integrations inside the flare window end at 1 s
if [ "$FLARE" -eq 1 ]; then
    set +e
    python3 "$CHUNKCHECK" idefx.trace --flare > chunk-flare.check.log 2>&1
    CHUNKF_RC=$?
    set -e
    grep '^  ' chunk-flare.check.log || true
    if [ "$CHUNKF_RC" -eq 0 ]; then
        echo "CHECK chunk-flare: ok"
    else
        echo "CHECK chunk-flare: FAIL (rc=$CHUNKF_RC; see" \
            "chunk-flare.check.log)"
        FAILED+=(chunk-flare)
    fi
fi
# the NAND cost record (D-5): what the run wrote to its sparse flash
[ -f flash.bin ] && echo "flash: $(du -k flash.bin | cut -f1) KiB" \
    "allocated in flash.bin ($MODE)"

GOT="${FAILED[*]:-}"
# PASS: the run dir may be reused; in slot mode drop the flash copy and
# the trace (legacy keeps everything)
pass_cleanup() {
    rm -f .unfinished
    stix_slot_cleanup 0 flash.bin idefx.trace
}
if [ "$FLARE" -eq 1 ] && [ "$NEGATIVE" -eq 1 ]; then
    # PASS only when the missing flip is all that fails: the LC
    # contrast check and the 1 s flare integrations; the sky, probe,
    # UART, trace and quiet chunk checks must pass.
    if [ "$GOT" = "ql-contrast chunk-flare" ]; then
        echo "DETECTOR-SMOKE (flare negative): PASS (failed exactly as" \
            "planned: no flare switch, no LC contrast, no 1 s" \
            "integrations; everything else passed)${STIX_SLOT_TAG}"
        pass_cleanup
        exit 0
    fi
    echo "DETECTOR-SMOKE (flare negative): FAIL (failed checks:" \
        "'${GOT}', want 'ql-contrast chunk-flare'$WHY_QL)${STIX_SLOT_TAG}"
    exit 1
fi
if [ "$NO_ROTB" -eq 1 ]; then
    # PASS only when exactly the two chunk checks fail as planned
    if [ "$GOT" = "uart-chunk chunk" ]; then
        echo "DETECTOR-SMOKE (no-rotb): PASS (failed exactly as planned:" \
            "nominal swaps but no chunk, nothing archived; everything" \
            "else passed)${STIX_SLOT_TAG}"
        pass_cleanup
        exit 0
    fi
    echo "DETECTOR-SMOKE (no-rotb): FAIL (failed checks: '${GOT}', want" \
        "'uart-chunk chunk'$WHY_QL)${STIX_SLOT_TAG}"
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
            "words at all; the trace is empty)${STIX_SLOT_TAG}"
        pass_cleanup
        exit 0
    fi
    echo "DETECTOR-SMOKE (negative): FAIL (failed checks: '${GOT}'," \
        "want '$WANT' with 384 write timeouts and 12 x >= 3 read" \
        "failures; got $IDEFX_W and $IDEFX_R$WHY_QL)${STIX_SLOT_TAG}"
    exit 1
fi
# the D-3 positive keeps its verdict wording; --flare names its mode
TAG="DETECTOR-SMOKE"
[ "$FLARE" -eq 1 ] && TAG="DETECTOR-SMOKE ($MODE)"
if [ -n "$GOT" ]; then
    echo "$TAG: FAIL (failed checks: $GOT$WHY_QL)${STIX_SLOT_TAG}"
    exit 1
fi
echo "$TAG: PASS${STIX_SLOT_TAG}"
pass_cleanup
exit 0
