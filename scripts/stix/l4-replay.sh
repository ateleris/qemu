#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# L4 replay wrapper: GH #1061 RED/GREEN in the emulator (L4 plan D-L4c,
# docs/stix-port/2026-11-13-l4-replay-plan.md).  Four phases:
#
#   1. collect — boot the collection build (STIX_L4_COLLECT_ELF, default
#      the GREEN ELF; collection behaviour is identical in RED and
#      GREEN, bundle builds table) on a FRESH flash in <run>/collect:
#      a connect group with NO expectations (boot-time TM is advisory,
#      see below), TC(236,1) CONF @50, S20=0 @60, NOMINAL @70, det-flare
#      true over QMP @75 (detector-smoke's QMP group pattern), hold
#      STIX_L4_COLLECT_SECONDS (default 600), CONF @<end> to flush the
#      last RAW_COUNTS file.  QEMU traces stixdet_chunk,
#      stixdet_chunk_drop and stixdet_nominal_swap into collect.trace.
#   2. truth + snapshot — build <run>/truth.txt from the collect UART
#      and trace, never from TM, in the format l4-decode.py reads
#      ('chunk <SCET s> <rcr>' per archived chunk, 'file <name> <scet>
#      <chunks>' per RAW_COUNTS file).  Chunk SCETs come from the
#      trace's stixdet_chunk lines (scet 0xC:0xF — the chunk header
#      timestamp the FSW archives); the rcr tags and the file
#      boundaries come from the UART in line order ('rcr = ' per chunk
#      header, 'Flush DET stream' per file close; DEBUG_PRINT builds —
#      the bundle's cflags carry -DDEBUG_PRINT).  The truth counts
#      ARCHIVED chunks only: the chunk the FSW drops at NOMINAL entry
#      ('Skipped first Det data chunk', ArchiveManager.c:140, expected
#      exactly once per collection) is excluded, so archived =
#      model chunks - 1 (asserted).  Gate before any request run (the
#      bundle's "count the files" discipline): >= 12 files, >= 1 chunk,
#      every rcr tag 0 (D-L4e stillness by rate; the decode re-verifies
#      the structure rcr bytes).  Then the snapshot flash-collect.bin
#      (sparse copy, size and allocation verified) in the collect dir.
#   3. requests — for BUILD in {red, green}: fresh dir, flash.bin = a
#      sparse copy of the snapshot, boot to CONFIGURATION only (no
#      NOMINAL entry: no context write, no detector, the archive
#      frozen).  No hard boot-event expects (the probe-connect race):
#      the probe log is grep-checked advisory-only for 0x0022 and
#      0x14A3 after the run, recorded in record.txt, never gating.
#      The hard expects attach to the sends only: CONF @50 (TM(1,1),
#      TM(5,1) 0021 02 04, TM(1,7); from here on forbid 0021 04 02 /
#      0021 05 02 / 0x142E — the snapshot context's stored S20=0 keeps
#      the TC20 shutdown away [L]), S20=0 @60 (parity), ZIX37013 with
#      PIX00462 0xFFF @65 (l4-tc.py flare, seq 3; TEST_PLAN §8 caveat:
#      without it every energy byte is 0), request A @70 (l4-tc.py
#      request, seq 4: start = the truth's first archived chunk, tmin
#      0, tmax 90000, tunit 1, det mask 0xFFFFFFFF, 1 box), request B
#      @STIX_L4_REQ_B_AT (default 600 — well after A's 30-s service
#      slot; seq 5: start = data end + 600 s, tmax 600).  The probe
#      dumps 0x5BC (L4) and 0x5A7 (events) and expects the TM(1,1)/
#      TM(1,7) acks of every send.  Nothing else is sent between A and
#      B (the bundle's stale-counter precondition).
#   4. verdict — l4-decode.py report per build against the truth file,
#      then the Goal-2 rules; the wrapper exits non-zero unless all
#      hold:
#      green-a   every archived chunk appears exactly once on the
#                relative-time axis (EXPECTED-SET PASS, dups 0, rcr
#                [0], ebin 0x3e1, pixel 0xfff)
#      green-b   B only TM(5,2) 0x543F (events eid543f=1), ZERO
#                SSID-24 packets, GHOST none
#      red-a     samples == red-round (lastround + 1 — the bundle's
#                "15 = 14 + 1" signature) and no rel outside the truth
#                set (extra 0), every sample byte-identical to GREEN's
#                same-relative-time sample (identity matched=all,
#                samediff 0)
#      red-b     >= 1 packet, GHOST primary (byte-identical structure
#                including the rel-time bytes; the 'rel-times-only'
#                fallback is reported but FAILS the verdict, D-L4b)
#      The decode summaries print on every outcome; every artefact
#      stays in the run dir (on PASS only the three working flash.bin
#      copies are dropped — the snapshot flash-collect.bin survives).
#
# Boot-time TM (0x0022 Application SW boot success, 0x14A3 FDIR
# protection enabled, 0x8030 Context file(s) not found) is ADVISORY
# ONLY in every phase: tm-hold is not on this branch, so boot TM can
# race the probe connect.  It is recorded from the probe logs in
# record.txt and never expected or forbidden; the verdicts use only
# packets after the sends.
#
# In slot mode (STIX_PORT_BASE, scripts/stix/lib/slot.sh) one slot is
# held for the whole wrapper: the port and the run dir <dir>-s<k>.
# Legacy (unset) takes no lock, as the other smokes.
#
# Usage: l4-replay.sh <qemu-binary> [--collect-only] [--dry-run]
#                     [--check-guard]
#   STIX_L4_RED_ELF      the RED wrapped ELF (required; SKIP 77 when
#                        unset or missing)
#   STIX_L4_GREEN_ELF    the GREEN wrapped ELF (required; SKIP 77)
#   STIX_L4_COLLECT_ELF  the collection build (default the GREEN ELF)
#   STIX_L4_RUN_DIR      run dir (default /tmp/stix-l4-replay), holding
#                        the collect/, red/ and green/ subdirs,
#                        truth.txt and record.txt.  Canonicalised; the
#                        shared uprev/baseline/GS dirs, anything inside
#                        them, their parents and / are refused.
#   STIX_L4_COLLECT_SECONDS  flare-on seconds in phase 1 (default 600;
#                        ~1 chunk/s at the flare rate, ~7 chunks per
#                        256-KiB file, so >= 12 files)
#   STIX_L4_REQ_B_AT     request B's send time, s from the connect
#                        (default 600)
#   STIX_L4_REQ_SECONDS  the request runs' probe seconds (default
#                        REQ_B_AT + 300; A's processing time is [L])
#   STIX_TMTC_PROBE      the probe script (tests substitute a fake)
#   STIX_FLASH_MAX_ALLOC_KB  caps every flash copy (default 65536; a
#                        longer collection may need a higher cap)
#   A run that did not PASS leaves .unfinished in the run dir; the next
#   run moves that dir aside to <dir>.fail-<date>-<pid>, never reusing
#   it.  --dry-run prints the QEMU/probe lines of all four phases
#   (request starts are 0 — they come from the truth) and creates
#   nothing on disk.  --check-guard only runs the run-dir guard and
#   exits 0/2.
#
# Exit codes: 0 pass (or dry-run / check-guard ok / collect-only gate
#             pass), 77 SKIP (an ELF unset or missing), 1 a failed
#             probe, UART check, truth gate or verdict, 2 environment
#             problem.
set -euo pipefail

usage="usage: l4-replay.sh <qemu-binary> [--collect-only] [--dry-run] \
[--check-guard]"
QEMU_BIN_ARG="${1:?$usage}"
shift
COLLECT_ONLY=0
DRY_RUN=0
CHECK_GUARD=0
while [ $# -gt 0 ]; do
    case "$1" in
        --collect-only)
            [ "$COLLECT_ONLY" -eq 0 ] || {
                echo "--collect-only given twice"; exit 2; }
            COLLECT_ONLY=1 ;;
        --dry-run)
            [ "$DRY_RUN" -eq 0 ] || { echo "--dry-run given twice"; exit 2; }
            DRY_RUN=1 ;;
        --check-guard)
            [ "$CHECK_GUARD" -eq 0 ] || {
                echo "--check-guard given twice"; exit 2; }
            CHECK_GUARD=1 ;;
        *) echo "unknown argument: $1"; echo "$usage"; exit 2 ;;
    esac
    shift
done

. "$(dirname "${BASH_SOURCE[0]}")/lib/paths.sh"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
L4TC="$REPO_ROOT/scripts/stix/l4-tc.py"
L4DEC="$REPO_ROOT/scripts/stix/l4-decode.py"
PROBE="${STIX_TMTC_PROBE:-$REPO_ROOT/scripts/stix/tmtc-probe.py}"
RED_ELF="${STIX_L4_RED_ELF:-}"
GREEN_ELF="${STIX_L4_GREEN_ELF:-}"
COLLECT_ELF="${STIX_L4_COLLECT_ELF:-$GREEN_ELF}"
COLLECT_SECONDS="${STIX_L4_COLLECT_SECONDS:-600}"
REQ_B_AT="${STIX_L4_REQ_B_AT:-600}"
REQ_SECONDS="${STIX_L4_REQ_SECONDS:-$((REQ_B_AT + 300))}"
RUN_DIR="${STIX_L4_RUN_DIR:-/tmp/stix-l4-replay}"
# the request numbers and TC sequence counts of the flow (the bundle's
# GREEN 61x / RED 71x request numbers; one TC seq per send so the acks
# 1DACC00x are distinct)
REQ_A_GREEN=611
REQ_B_GREEN=612
REQ_A_RED=711
REQ_B_RED=712

. "$REPO_ROOT/scripts/stix/lib/slot.sh"
# dry-run, check-guard and the SKIPs boot nothing: they peek at the slot
# (no lock, no wait, no disk guard)
SKIP_WHY=""
if [ -z "$RED_ELF" ]; then
    SKIP_WHY="STIX_L4_RED_ELF unset; point it at the RED wrapped ELF (ASW_em_red3.elf)"
elif [ ! -f "$RED_ELF" ]; then
    SKIP_WHY="STIX_L4_RED_ELF not a file ($RED_ELF)"
elif [ -z "$GREEN_ELF" ]; then
    SKIP_WHY="STIX_L4_GREEN_ELF unset; point it at the GREEN wrapped ELF (ASW_em_green3.elf)"
elif [ ! -f "$GREEN_ELF" ]; then
    SKIP_WHY="STIX_L4_GREEN_ELF not a file ($GREEN_ELF)"
elif [ ! -f "$COLLECT_ELF" ]; then
    SKIP_WHY="STIX_L4_COLLECT_ELF not a file ($COLLECT_ELF)"
fi
if [ "$DRY_RUN" -eq 1 ] || [ "$CHECK_GUARD" -eq 1 ] ||
        [ -n "$SKIP_WHY" ]; then
    stix_slot_setup peek
else
    stix_slot_setup
fi
RUN_DIR="$(stix_slot_dir "$RUN_DIR")"

# The shared flashes must never see this gate (the collect run enters
# NOMINAL and writes a flash context): canonical first, then refuse /,
# the shared run dirs, anything inside them and any parent of them
# (nominal-smoke's guard).  The collect/red/green subdirs live inside
# the base dir, so guarding the base guards them.
RUN_DIR="$(realpath -m -- "$RUN_DIR")"
refuse_run_dir() {
    echo "refusing run dir $RUN_DIR ($1)"
    echo "the collect run enters NOMINAL and writes a flash context, so"
    echo "this wrapper needs its own run dir away from the shared ones"
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
COLLECT_DIR="$RUN_DIR/collect"
RED_DIR="$RUN_DIR/red"
GREEN_DIR="$RUN_DIR/green"
QMP_SOCK="$COLLECT_DIR/qmp.sock"
# sun_path is 108 bytes including the NUL
if [ "${#QMP_SOCK}" -ge 108 ]; then
    echo "refusing run dir $RUN_DIR (the QMP socket path $QMP_SOCK is" \
        "${#QMP_SOCK} bytes; a unix socket path must stay under 108)"
    exit 2
fi

if [ "$CHECK_GUARD" -eq 1 ]; then
    echo "GUARD OK: run dir $RUN_DIR (canonical)"
    echo "GUARD OK: collect $COLLECT_DIR, red $RED_DIR, green $GREEN_DIR"
    exit 0
fi

QEMU_BIN="$(realpath -e "$QEMU_BIN_ARG")" || {
    echo "qemu binary not found: $QEMU_BIN_ARG"; exit 2; }
[ -x "$QEMU_BIN" ] || { echo "not executable: $QEMU_BIN"; exit 2; }

for v in "$COLLECT_SECONDS" "$REQ_B_AT" "$REQ_SECONDS"; do
    case "$v" in
        '' | *[!0-9]*)
            echo "STIX_L4_* time knobs must be whole seconds (got: $v)"
            exit 2
            ;;
    esac
done
if [ "$REQ_B_AT" -le 100 ]; then
    echo "STIX_L4_REQ_B_AT must be > 100 (request A is sent at +70;" \
        "got: $REQ_B_AT)"
    exit 2
fi
if [ "$REQ_SECONDS" -lt $((REQ_B_AT + 60)) ]; then
    echo "STIX_L4_REQ_SECONDS must be >= STIX_L4_REQ_B_AT + 60 (got:" \
        "$REQ_SECONDS < $((REQ_B_AT + 60)))"
    exit 2
fi

if [ -n "$SKIP_WHY" ]; then
    echo "SKIP: $SKIP_WHY"
    exit 77
fi

# TC units, SpW header + CCSDS + CRC-16/CCITT-FALSE precomputed (the
# nominal-smoke/elut-smoke bytes): CONF seq 1, S20=0 seq 2, NOMINAL
# seq 3, the collection's closing CONF seq 4.  The flare and request
# units are built per request run by l4-tc.py (seqs 3, 4, 5).
TC_CONF="40 02 00 00 1D AC C0 01 00 07 19 EC 01 00 04 00 4A C1"
TC_S20="40 02 00 00 1D AC C0 02 00 09 19 EC 06 00 07 04 00 00 AF BA"
TC_NOM="40 02 00 00 1D AC C0 03 00 07 19 EC 01 00 05 00 BF 97"
TC_END="40 02 00 00 1D AC C0 04 00 07 19 EC 01 00 04 00 3D 0D"

# Phase timings, seconds from the TMTC connect.
C_FLARE_AT=75                       # det-flare true over QMP
C_CONF_AT=$((C_FLARE_AT + COLLECT_SECONDS + 5))   # CONF to flush
C_SECONDS=$((C_CONF_AT + 45))       # hold past the flush
R_CONF_AT=50
R_S20_AT=60
R_FLARE_AT=65
R_A_AT=70

qom_set() {  # path property json-value (detector-smoke's form)
    printf '{"execute": "qom-set", "arguments": %s' \
        "{\"path\": \"$1\", \"property\": \"$2\", \"value\": $3}}"
}
stat_val() { # "k=v ..." key -> v (a field must START with key=, so
              # 'files=' does not match inside 'lastround=')
    awk -v key="$2" '{
        for (i = 1; i <= NF; i++)
            if (index($i, key "=") == 1) {
                print substr($i, length(key) + 2); exit
            }
    }' <<< "$1"
}
collect_qemu_args() {
    # one -trace with the file backend (a second one would reset the
    # trace file, detector-smoke's note); the truncated-RX event goes
    # into the same file and is checked there
    QEMU_ARGS=(
        -M leon3_generic -m 64M -no-reboot
        -display none -monitor none
        "${STIX_SLOT_QEMU_ARGS[@]}"
        -qmp "unix:$QMP_SOCK,server=on,wait=off"
        -serial "file:$COLLECT_DIR/uart.collect.raw"
        -kernel "$COLLECT_ELF"
        -msg timestamp=on
        -trace "events=$COLLECT_DIR/trace.events,file=$COLLECT_DIR/collect.trace"
    )
}
# one boot: the probe starts first (it retries until QEMU binds the
# TMTC port); the caller has cd'd into the boot's run dir and filled
# QEMU_ARGS.  Probe args arrive as "$@".  Leaves BOOT_RC.
boot() { # name seconds probe-log qemu-log probe-args...
    local name="$1" secs="$2" plog="$3" qlog="$4" rc alive
    shift 4
    if ss -ltn | grep -q ":${TMTC_PORT} "; then
        echo "TMTC port ${TMTC_PORT} busy — another QEMU instance" \
            "running?${STIX_SLOT_TAG}"
        exit 2
    fi
    stix_disk_guard
    timeout "$((secs + 30))" python3 -u "$PROBE" "$@" > "$plog" 2>&1 &
    PROBE_PID=$!
    timeout "$((secs + 30))" "$QEMU_BIN" "${QEMU_ARGS[@]}" \
        > "$qlog" 2>&1 &
    QEMU_PID=$!
    set +e
    wait "$PROBE_PID"
    rc=$?
    PROBE_PID=""
    kill -0 "$QEMU_PID" 2>/dev/null
    alive=$?
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null
    QEMU_PID=""
    set -e
    if [ "$alive" -ne 0 ]; then
        tail -5 "$qlog"
        echo "L4-REPLAY: ERROR ($name: QEMU died or never started)${STIX_SLOT_TAG}"
        exit 2
    fi
    if [ "$rc" -eq 124 ]; then
        tail -5 "$plog"
        echo "L4-REPLAY: ERROR ($name: the probe hit its own timeout)${STIX_SLOT_TAG}"
        exit 2
    fi
    if grep -q "could not connect" "$plog"; then
        tail -5 "$plog"
        echo "L4-REPLAY: ERROR ($name: the probe never connected)${STIX_SLOT_TAG}"
        exit 2
    fi
    BOOT_RC=$rc
}
fail_probe() { # name plog rc
    local why
    why="$(grep -m1 -o 'FORBIDDEN TM: .*' "$2" | cut -d' ' -f3-)" || true
    if [ -n "$why" ]; then
        why="forbidden: $why"
    else
        why="$(grep -m1 -o 'EXPECTATION NOT MATCHED: .*' "$2" |
            cut -d' ' -f4-)" || true
        if [ -n "$why" ]; then
            why="not matched: $why"
        elif grep -q '^QMP ERROR' "$2"; then
            why="$(grep -m1 '^QMP ERROR' "$2")"
        elif grep -q '^CONNECTION CLOSED' "$2"; then
            why="connection closed"
        else
            why="probe rc=$3; see $2"
        fi
    fi
    tail -5 "$2"
    echo "L4-REPLAY: FAIL ($1: $why)${STIX_SLOT_TAG}"
    exit 1
}
# boot-time TM is advisory only: recorded, never gating
boot_tm_advisory() { # plog label
    local a="not-seen" b="not-seen"
    grep -qi "002200000008" "$1" && a="seen"
    grep -qiE "TM apid=0x5A7 type=5 subtype=1 .*data=[0-9a-f]*14a3" "$1" &&
        b="seen"
    echo "boot TM advisory ($2): 0x0022 $a, 0x14A3 $b" | tee -a "$RECORD"
}
record_elf() { # label path
    [ "$DRY_RUN" -eq 0 ] || return 0
    {
        echo "elf $1: $2"
        echo "elf $1 md5: $(md5sum "$2" | cut -d' ' -f1)"
        echo "elf $1 sha256: $(sha256sum "$2" | cut -d' ' -f1)"
    } | tee -a "$RECORD"
}

# An unfinished run dir is moved aside, never reused (its evidence
# survives; detector-smoke's rule).  Dry-run writes nothing.
RECORD="$RUN_DIR/record.txt"
if [ "$DRY_RUN" -eq 0 ]; then
    if [ -e "$RUN_DIR/.unfinished" ]; then
        KEPT="$RUN_DIR.fail-$(date +%Y%m%d-%H%M%S)-$$"
        mv -T -- "$RUN_DIR" "$KEPT"
        echo "l4-replay: kept the unfinished run's dir as $KEPT"
    fi
    mkdir -p "$RUN_DIR" "$COLLECT_DIR" "$RED_DIR" "$GREEN_DIR"
    touch "$RUN_DIR/.unfinished"
    {
        echo "l4-replay $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "qemu: $QEMU_BIN"
        echo "knobs: collect_seconds=$COLLECT_SECONDS req_b_at=$REQ_B_AT" \
            "req_seconds=$REQ_SECONDS collect_only=$COLLECT_ONLY"
        echo "slot: ${STIX_SLOT:-legacy} port $TMTC_PORT"
    } > "$RECORD"
fi

# ------------------------------------------------ phase 1: collect
echo "== phase 1: collect ($COLLECT_SECONDS s of flare in NOMINAL)"
record_elf collect "$COLLECT_ELF"
if [ "$DRY_RUN" -eq 0 ]; then
    cd "$COLLECT_DIR"
    rm -f flash.bin flash-collect.bin uart.collect.raw uart.collect.txt \
          collect.probe.log qemu-stdout.collect.log collect.trace trace.events
    truncate -s 17716740096 flash.bin
    # the truth events and the truncated-RX witness, one -trace file
    printf '%s\n' stixdet_chunk stixdet_chunk_drop stixdet_nominal_swap \
        stixspw_rx_truncated > trace.events
fi
COLLECT_PROBE_ARGS=(
    --port "$TMTC_PORT" --seconds "$C_SECONDS" --window 20 --hold
    --qmp "$QMP_SOCK"
    --forbid "0x5A1:1:2" --forbid "0x5A1:1:8"
    --send-at 50 --send-hex "$TC_CONF"
    --expect "0x5A1:1:1:1DACC001@12"
    --expect "0x5A7:5:1:00210204@12"
    --expect "0x5A1:1:7:1DACC001@12"
    --forbid "0x5A7:5:1:00210402"
    --forbid "0x5A7:5:1:00210502"
    --forbid "0x5A7:5:1:142e"
    --send-at 60 --send-hex "$TC_S20"
    --expect "0x5A1:1:1:1DACC002@12"
    --expect "0x5A1:1:7:1DACC002@12"
    --send-at 70 --send-hex "$TC_NOM"
    --expect "0x5A1:1:1:1DACC003@12"
    --expect "0x5A7:5:1:00210405@12"
    --expect "0x5A1:1:7:1DACC003@12"
    --send-at "$C_FLARE_AT" --qmp-cmd \
"$(qom_set /machine/stixctrl det-flare true)"
    --send-at "$C_CONF_AT" --send-hex "$TC_END"
    --expect "0x5A1:1:1:1DACC004@12"
    --expect "0x5A7:5:1:00210504@12"
    --expect "0x5A1:1:7:1DACC004@12"
)
if [ "$DRY_RUN" -eq 1 ]; then
    echo "DRY RUN — the commands l4-replay would run (collect-only=$COLLECT_ONLY):"
    collect_qemu_args
    echo "# phase 1: collect, ELF $COLLECT_ELF, run dir $COLLECT_DIR"
    printf '(cd %s && timeout %s %s ' "$COLLECT_DIR" \
        "$((C_SECONDS + 30))" "$QEMU_BIN"
    printf '%q ' "${QEMU_ARGS[@]}"
    printf '> qemu-stdout.collect.log 2>&1)\n'
    printf 'python3 -u %s ' "$PROBE"
    printf '%q ' "${COLLECT_PROBE_ARGS[@]}"
    printf '\n'
else
    cd "$COLLECT_DIR"
    collect_qemu_args
    boot collect "$C_SECONDS" collect.probe.log \
        qemu-stdout.collect.log "${COLLECT_PROBE_ARGS[@]}"
    [ "$BOOT_RC" -eq 0 ] || fail_probe collect collect.probe.log "$BOOT_RC"
    echo "CHECK probe (collect): ok"
    boot_tm_advisory collect.probe.log collect
fi

# ------------------------------------------------ phase 2: truth + snapshot
if [ "$DRY_RUN" -eq 1 ]; then
    echo "# phase 2: truth + snapshot"
    echo "(trace events: stixdet_chunk stixdet_chunk_drop" \
        "stixdet_nominal_swap stixspw_rx_truncated)"
    echo "(inline truth builder: $COLLECT_DIR/collect.trace +"
    echo " $COLLECT_DIR/uart.collect.txt -> $RUN_DIR/truth.txt;"
    echo " archived = stixdet_chunk chunks - 1 dropped first, rcr tags"
    echo " and file closes from the UART in line order)"
    echo "(cd $COLLECT_DIR && timeout 60 cp --sparse=always flash.bin flash-collect.bin)"
else
    tr -d '\000' < uart.collect.raw > uart.collect.txt
    if ! grep -q "init: SAFE mode" uart.collect.txt; then
        echo "L4-REPLAY: FAIL (the collect UART has no 'init: SAFE mode'" \
            "banner)${STIX_SLOT_TAG}"
        exit 1
    fi
    if grep -Eq 'storing new context[01] failed' uart.collect.txt; then
        grep -Em1 'storing new context[01] failed' uart.collect.txt
        echo "L4-REPLAY: FAIL (a flash context write failed)${STIX_SLOT_TAG}"
        exit 1
    fi
    if grep -q 'stixspw_rx_truncated' collect.trace; then
        grep -m1 'stixspw_rx_truncated' collect.trace
        echo "L4-REPLAY: FAIL (a TC unit was truncated at SpW RX)${STIX_SLOT_TAG}"
        exit 1
    fi
    # no error from the chunk path, the archive, the filesystem or the
    # RCR path (detector-smoke's pattern); the dropped first chunk is
    # the one expected *E
    CHUNK_TAGS='Archive|RotAccuComp|RotAccumulator|FS|FSapi|MemStream'
    CHUNK_TAGS="$CHUNK_TAGS|MemCommon|FIFO|RateCtrl"
    CHUNK_ERR="$(grep -E "^[[:space:]]*[0-9:.]+ +($CHUNK_TAGS) +\\*E" \
        uart.collect.txt | grep -v 'Skipped first Det data chunk' |
        head -3 || true)"
    if [ -n "$CHUNK_ERR" ]; then
        echo "$CHUNK_ERR"
        echo "L4-REPLAY: FAIL (chunk/archive/filesystem/RCR error on the" \
            "collect UART)${STIX_SLOT_TAG}"
        exit 1
    fi
    echo "CHECK uart (collect): ok (banner, no context failure, no chunk-path error)"
    set +e
    python3 - "$COLLECT_DIR/collect.trace" \
        "$COLLECT_DIR/uart.collect.txt" "$RUN_DIR/truth.txt" <<'TRUTH_EOF'
import re
import sys

trace_path, uart_path, out_path = sys.argv[1:4]
CHUNK = re.compile(r"^\S+ stixdet_chunk chunk: nom_bank \d+, \d+ blocks, "
                   r"scet 0x([0-9a-f]+):0x([0-9a-f]+), ")
RCR = re.compile(r"rcr = (\d+)")
chunks, drops = [], 0
for line in open(trace_path, errors="replace"):
    m = CHUNK.match(line)
    if m:
        coarse = int(m.group(1), 16)
        fine = int(m.group(2), 16)
        chunks.append(coarse * 1000 + fine * 1000 // 0x10000)
    elif " stixdet_chunk_drop " in line:
        drops += 1
skip = 0
events = []                       # ("c", rcr) and ("f", None), in order
for line in open(uart_path, errors="replace"):
    if "Skipped first Det data chunk" in line:
        skip += 1
    m = RCR.search(line)
    if m:
        events.append(("c", int(m.group(1))))
    elif "Flush DET stream" in line:
        events.append(("f", None))
errs = []
if drops:
    errs.append("%d stixdet_chunk_drop lines (want 0)" % drops)
if skip != 1:
    errs.append("%d 'Skipped first Det data chunk' lines (want 1)" % skip)
if not chunks:
    errs.append("no stixdet_chunk lines in %s" % trace_path)
n = len(chunks)
marks = [e for e in events if e[0] == "c"]
if len(marks) == n:
    # one print per model chunk; the first is the chunk dropped at
    # NOMINAL entry [L: print order vs the drop]
    drop_first_mark = True
elif len(marks) == n - 1:
    drop_first_mark = False   # the dropped chunk printed no rcr line
else:
    errs.append("%d 'rcr =' lines vs %d stixdet_chunk chunks (want n or n-1)"
                % (len(marks), n))
if errs:
    sys.exit("; ".join(errs))
archived = []                     # (ms, rcr)
files = []
cur = 0
for kind, rcr in events:
    if kind == "c":
        if drop_first_mark:
            drop_first_mark = False
            continue
        # the dropped chunk is model chunk 0 in both cases above, so
        # archived chunk i is always model chunk i + 1
        archived.append((chunks[len(archived) + 1], rcr))
        cur += 1
    elif cur:
        files.append(cur)
        cur = 0
if cur:
    files.append(cur)
if sum(files) != len(archived):
    sys.exit("the UART flush lines close %d chunks but %d are archived"
             % (sum(files), len(archived)))
# the wrapped builds search the files in rounds of <= 3 (filenames[3])
# and zero the L4 counters at every round start (SciReport.c, old tree):
# RED keeps the LAST round, i.e. the last len(files) % 3 files (3 if 0)
last3 = (sum(files[-(len(files) % 3 or 3):]) if len(files) >= 2
         else None)
with open(out_path, "w") as f:
    f.write("# l4-replay truth (UART + trace, never TM): %d model chunks, "
            "%d archived (the first is dropped at NOMINAL entry),\n"
            "# %d RAW_COUNTS files; chunk SCETs from stixdet_chunk, rcr "
            "tags and file closes from the UART\n"
            % (n, len(archived), len(files)))
    for ms, rcr in archived:
        f.write("chunk %d.%03d %d\n" % (ms // 1000, ms % 1000, rcr))
    done = 0
    for i, k in enumerate(files, 1):
        ms = archived[done][0]
        f.write("file RAW_COUNTS_%d %d.%03d %d\n"
                % (i, ms // 1000, ms % 1000, k))
        done += k
rr = "n/a" if last3 is None else str(last3 + 1)
print("TRUTH-STATS chunks=%d files=%d lastround=%s red-round=%s rcrmax=%d"
      % (len(archived), len(files),
         last3 if last3 is not None else "n/a", rr,
         max((r for _, r in archived), default=0)))
TRUTH_EOF
    TRUTH_RC=$?
    set -e
    if [ "$TRUTH_RC" -ne 0 ]; then
        echo "L4-REPLAY: FAIL (the truth builder failed, rc=$TRUTH_RC; see" \
            "the messages above)${STIX_SLOT_TAG}"
        exit 1
    fi
    STATS="$(python3 - "$RUN_DIR/truth.txt" <<'STATS_EOF'
import sys
c = f = k = 0
rcrmax = 0
files = []
for line in open(sys.argv[1]):
    t = line.split()
    if not t or t[0][0] == "#":
        continue
    if t[0] == "chunk":
        c += 1
        rcrmax = max(rcrmax, int(t[2]))
    elif t[0] == "file":
        f += 1
        files.append(int(t[3]))
k = sum(files[-(len(files) % 3 or 3):]) if len(files) >= 2 else 0
print("chunks=%d files=%d lastround=%d red-round=%d rcrmax=%d"
      % (c, f, k, k + 1 if files else 0, rcrmax))
STATS_EOF
)"
    echo "TRUTH GATE: $STATS"
    C="$(stat_val "$STATS" chunks)"
    F="$(stat_val "$STATS" files)"
    RCRM="$(stat_val "$STATS" rcrmax)"
    if [ "$C" -lt 1 ] || [ "$F" -lt 12 ] || [ "$RCRM" -ne 0 ]; then
        echo "L4-REPLAY: FAIL (truth gate: want chunks >= 1, files >= 12," \
            "rcrmax 0; got chunks=$C files=$F rcrmax=$RCRM)${STIX_SLOT_TAG}"
        exit 1
    fi
    echo "CHECK truth gate: ok (>= 12 files, every rcr tag 0)"
    echo "TRUTH GATE ok ($STATS)" >> "$RECORD"
    # the snapshot: a sparse copy, verified (the elut --save-template
    # rules; stix_flash_copy copies the master, this copies the run's
    # own flash aside)
    if ! timeout 60 cp --sparse=always flash.bin flash-collect.bin; then
        rm -f flash-collect.bin
        echo "L4-REPLAY: ERROR (the snapshot copy failed or timed out)${STIX_SLOT_TAG}"
        exit 2
    fi
    SNAP_ALLOC="$(du -k flash-collect.bin | cut -f1)"
    if [ "$(stat -c %s flash-collect.bin)" != "$(stat -c %s flash.bin)" ] ||
            [ "$SNAP_ALLOC" -ge "${STIX_FLASH_MAX_ALLOC_KB:-65536}" ]; then
        rm -f flash-collect.bin
        echo "L4-REPLAY: ERROR (the snapshot is not a verified sparse copy:" \
            "allocated ${SNAP_ALLOC} KiB)${STIX_SLOT_TAG}"
        exit 2
    fi
    echo "CHECK snapshot: ok (flash-collect.bin, allocated ${SNAP_ALLOC} KiB)"
fi
if [ "$COLLECT_ONLY" -eq 1 ]; then
    if [ "$DRY_RUN" -eq 0 ]; then
        echo "snapshot kept: $COLLECT_DIR/flash-collect.bin"
        rm -f "$RUN_DIR/.unfinished"
        echo "L4-REPLAY (collect-only): PASS${STIX_SLOT_TAG}"
    fi
    exit 0
fi

# ------------------------------------------------ phase 3: requests
# The request A/B units and their ack expectations come from l4-tc.py
# (probe-args emission); the starts come from the truth file.  In
# dry-run the starts are unknown: 0 stands in.
SNAP="$COLLECT_DIR/flash-collect.bin"
if [ "$DRY_RUN" -eq 1 ]; then
    A_START=0
    B_START=0
else
    A_START="$(awk '$1 == "chunk" { print int($2); exit }' "$RUN_DIR/truth.txt")"
    B_START="$(awk '$1 == "chunk" { last = int($2) } END { print last + 600 }' \
        "$RUN_DIR/truth.txt")"
    if [ -z "$A_START" ] || [ -z "$B_START" ]; then
        echo "L4-REPLAY: FAIL (cannot read the first/last chunk SCET" \
            "from $RUN_DIR/truth.txt)${STIX_SLOT_TAG}"
        exit 1
    fi
fi
# one request run per build: name, dir, ELF, req numbers, and the
# --dump-apid file names
request_run() { # name dir elf reqA reqB probe-log-tag
    local name="$1" dir="$2" elf="$3" reqA="$4" reqB="$5" tag="$6"
    local enc_out enc_err
    echo "== phase 3: requests ($name: ELF $elf, reqs $reqA/$reqB," \
        "A start $A_START, B start $B_START)"
    record_elf "$name" "$elf"
    if [ "$DRY_RUN" -eq 1 ]; then
        echo "# request run $name (flash.bin = a sparse copy of $SNAP)"
    else
        mkdir -p "$dir"
        cd "$dir"
        # only this run's own files, by explicit path (the shared dirs
        # are refused by the guard); the dump is appended to by the
        # probe, so it must start empty
        rm -f flash.bin l4.dump.hex events.dump.hex \
              "uart.$name.raw" "uart.$name.txt" \
              "$name.probe.log" "qemu-stdout.$name.log" \
              A.report.log B.report.log
        # the frozen snapshot -> this run's own sparse flash copy
        # (stix_flash_copy runs all the slot-library guards)
        STIX_FLASH_MASTER="$SNAP" stix_flash_copy
    fi
    enc_out="$(mktemp)"
    enc_err="$(mktemp)"
    if ! python3 "$L4TC" flare --seq 3 --pixel-mask 0xFFF --t0 "$R_FLARE_AT" \
            --ack-window 10 --done-window 10 \
            > "$enc_out" 2> "$enc_err"; then
        cat "$enc_err"
        rm -f "$enc_out" "$enc_err"
        echo "L4-REPLAY: ERROR (l4-tc.py flare failed)${STIX_SLOT_TAG}"
        exit 2
    fi
    mapfile -t FLARE_ARGS < "$enc_out"
    if ! python3 "$L4TC" request --req "$reqA" --start "$A_START" \
            --box "0,90000,1,0,31,1" --seq 4 --t0 "$R_A_AT" \
            --ack-window 10 --done-window 45 \
            > "$enc_out" 2> "$enc_err"; then
        cat "$enc_err"
        rm -f "$enc_out" "$enc_err"
        echo "L4-REPLAY: ERROR (l4-tc.py request A failed)${STIX_SLOT_TAG}"
        exit 2
    fi
    mapfile -t REQ_A_ARGS < "$enc_out"
    if ! python3 "$L4TC" request --req "$reqB" --start "$B_START" \
            --box "0,600,1,0,31,1" --seq 5 --t0 "$REQ_B_AT" \
            --ack-window 10 --done-window 45 \
            > "$enc_out" 2> "$enc_err"; then
        cat "$enc_err"
        rm -f "$enc_out" "$enc_err"
        echo "L4-REPLAY: ERROR (l4-tc.py request B failed)${STIX_SLOT_TAG}"
        exit 2
    fi
    mapfile -t REQ_B_ARGS < "$enc_out"
    rm -f "$enc_out" "$enc_err"
    PROBE_ARGS=(
        --port "$TMTC_PORT" --seconds "$REQ_SECONDS" --window 20 --hold
        --dump-apid "0x5BC:$dir/l4.dump.hex"
        --dump-apid "0x5A7:$dir/events.dump.hex"
        --forbid "0x5A1:1:2" --forbid "0x5A1:1:8"
        --send-at "$R_CONF_AT" --send-hex "$TC_CONF"
        --expect "0x5A1:1:1:1DACC001@12"
        --expect "0x5A7:5:1:00210204@12"
        --expect "0x5A1:1:7:1DACC001@12"
        --forbid "0x5A7:5:1:00210402"
        --forbid "0x5A7:5:1:00210502"
        --forbid "0x5A7:5:1:142e"
        --send-at "$R_S20_AT" --send-hex "$TC_S20"
        --expect "0x5A1:1:1:1DACC002@12"
        --expect "0x5A1:1:7:1DACC002@12"
        "${FLARE_ARGS[@]}"
        "${REQ_A_ARGS[@]}"
        "${REQ_B_ARGS[@]}"
    )
    if [ "$DRY_RUN" -eq 1 ]; then
        printf '(cd %s && timeout %s %s -M leon3_generic -m 64M ' \
            "$dir" "$((REQ_SECONDS + 30))" "$QEMU_BIN"
        printf '%q ' -no-reboot -display none -monitor none
        [ "$STIX_SLOT_MODE" -eq 1 ] &&
            printf '%q ' "${STIX_SLOT_QEMU_ARGS[@]}"
        printf '%q ' -serial "file:$dir/uart.$name.raw" -kernel "$elf" \
            -trace stixspw_rx_truncated
        printf '> qemu-stdout.%s.log 2>&1)\n' "$name"
        printf 'python3 -u %s ' "$PROBE"
        printf '%q ' "${PROBE_ARGS[@]}"
        printf '\n'
        return 0
    fi
    QEMU_ARGS=(
        -M leon3_generic -m 64M -no-reboot
        -display none -monitor none
        "${STIX_SLOT_QEMU_ARGS[@]}"
        -serial "file:$dir/uart.$name.raw"
        -kernel "$elf"
        -trace stixspw_rx_truncated
    )
    boot "$name" "$REQ_SECONDS" "$tag.probe.log" \
        "qemu-stdout.$name.log" "${PROBE_ARGS[@]}"
    [ "$BOOT_RC" -eq 0 ] || fail_probe "$name" "$tag.probe.log" "$BOOT_RC"
    echo "CHECK probe ($name): ok"
    boot_tm_advisory "$tag.probe.log" "$name"
    tr -d '\000' < "uart.$name.raw" > "uart.$name.txt"
    if ! grep -q "init: SAFE mode" "uart.$name.txt"; then
        echo "L4-REPLAY: FAIL (the $name UART has no 'init: SAFE mode'" \
            "banner)${STIX_SLOT_TAG}"
        exit 1
    fi
    if grep -q 'stixspw_rx_truncated' "qemu-stdout.$name.log"; then
        grep -m1 'stixspw_rx_truncated' "qemu-stdout.$name.log"
        echo "L4-REPLAY: FAIL ($name: a TC unit was truncated at SpW RX)${STIX_SLOT_TAG}"
        exit 1
    fi
    # the request boots stay in CONFIGURATION: a NOMINAL entry event
    # (0021 04 05) would add chunks to the archive under test
    if grep -qiE "TM apid=0x5A7 type=5 subtype=1 .*data=[0-9a-f]*00210405" \
            "$tag.probe.log"; then
        echo "L4-REPLAY: FAIL ($name entered NOMINAL; the archive is not" \
            "frozen)${STIX_SLOT_TAG}"
        exit 1
    fi
    echo "CHECK uart ($name): ok (banner; no NOMINAL entry, the archive frozen)"
}

if [ "$DRY_RUN" -eq 1 ]; then
    echo "# phase 3: requests (red ELF $RED_ELF, green ELF $GREEN_ELF;"
    echo " flash.bin per run = a sparse copy of the snapshot)"
    request_run red "$RED_DIR" "$RED_ELF" "$REQ_A_RED" "$REQ_B_RED" red
    request_run green "$GREEN_DIR" "$GREEN_ELF" "$REQ_A_GREEN" \
        "$REQ_B_GREEN" green
else
    request_run red "$RED_DIR" "$RED_ELF" "$REQ_A_RED" "$REQ_B_RED" red
    request_run green "$GREEN_DIR" "$GREEN_ELF" "$REQ_A_GREEN" \
        "$REQ_B_GREEN" green
fi

# ------------------------------------------------ phase 4: verdict
# l4-decode.py report per build against the truth, then the Goal-2
# rules.  The decode summaries print on every outcome.
if [ "$DRY_RUN" -eq 1 ]; then
    echo "# phase 4: verdict (l4-decode.py per build, then the Goal-2 rules)"
    for b in red green; do
        echo "python3 $L4DEC report --capture $RUN_DIR/$b/l4.dump.hex --request A --req-number <reqA> --truth $RUN_DIR/truth.txt --events $RUN_DIR/$b/events.dump.hex --start $A_START --against <other>/l4.dump.hex > $RUN_DIR/$b/a.report.log"
        echo "python3 $L4DEC report --capture $RUN_DIR/$b/l4.dump.hex --request B --req-number <reqB> --events $RUN_DIR/$b/events.dump.hex > $RUN_DIR/$b/b.report.log"
    done
    echo "DRY RUN — nothing booted, nothing decoded."
    exit 0
fi
FAILED=()
decode() { # dir letter req extra-args...
    local dir="$1" letter="$2" req="$3"
    shift 3
    set +e
    python3 "$L4DEC" report --capture "$dir/l4.dump.hex" \
        --request "$letter" --req-number "$req" \
        --events "$dir/events.dump.hex" "$@" \
        > "$dir/$letter.report.log" 2>&1
    RC=$?
    set -e
    echo "-- $dir/$letter.report.log (rc=$RC)"
    sed 's/^/  /' "$dir/$letter.report.log"
    [ "$RC" -eq 0 ] || FAILED+=("decode-$letter")
}
decode "$RED_DIR" A "$REQ_A_RED" --truth "$RUN_DIR/truth.txt" \
    --start "$A_START" --against "$GREEN_DIR/l4.dump.hex"
RED_A_LOG="$RED_DIR/A.report.log"
decode "$RED_DIR" B "$REQ_B_RED"
RED_B_LOG="$RED_DIR/B.report.log"
decode "$GREEN_DIR" A "$REQ_A_GREEN" --truth "$RUN_DIR/truth.txt" \
    --start "$A_START" --against "$RED_DIR/l4.dump.hex"
GREEN_A_LOG="$GREEN_DIR/A.report.log"
decode "$GREEN_DIR" B "$REQ_B_GREEN"
GREEN_B_LOG="$GREEN_DIR/B.report.log"

# the numbers from the reports and the truth gate
req_field() { # log req letter -> "packets samples"
    sed -n "s/^REQUEST $2 ($3): packets=\([0-9]*\) samples=\([0-9]*\) .*/\1 \2/p" \
        "$1" | head -1
}
C_TRUTH="$(stat_val "$STATS" chunks)"
R_TRUTH="$(stat_val "$STATS" red-round)"
read -r RA_P RA_S <<< "$(req_field "$RED_A_LOG" "$REQ_A_RED" A)"
read -r RB_P RB_S <<< "$(req_field "$RED_B_LOG" "$REQ_B_RED" B)"
read -r GA_P GA_S <<< "$(req_field "$GREEN_A_LOG" "$REQ_A_GREEN" A)"
read -r GB_P GB_S <<< "$(req_field "$GREEN_B_LOG" "$REQ_B_GREEN" B)"

# RED: the last-round signature (Goal 2)
if [ "${RA_S:-0}" -eq "$R_TRUTH" ]; then
    echo "CHECK red-a-samples: ok ($RA_S = red-round $R_TRUTH, of $C_TRUTH)"
else
    echo "CHECK red-a-samples: FAIL (samples ${RA_S:-none}, want the" \
        "red-round $R_TRUTH of $C_TRUTH chunks)"
    FAILED+=(red-a-samples)
fi
if grep -q " dups=0 rcr=\[0\] ebins=\[0x3e1\] pixel=\[0xfff\] closing=" \
        "$RED_A_LOG"; then
    echo "CHECK red-a-shape: ok (dups 0, rcr [0], ebin 0x3e1, pixel 0xfff)"
else
    echo "CHECK red-a-shape: FAIL (dups/rcr/ebin/pixel mismatch)"
    FAILED+=(red-a-shape)
fi
if grep -q "^EXPECTED-SET: FAIL missing=$((C_TRUTH - ${RA_S:-0})) extra=0 " \
        "$RED_A_LOG"; then
    echo "CHECK red-a-set: ok (nothing outside the truth set)"
else
    echo "CHECK red-a-set: FAIL (want FAIL missing=$((C_TRUTH - ${RA_S:-0}))" \
        "extra=0 — RED must send only archived chunks)"
    FAILED+=(red-a-set)
fi
if grep -q "SAMPLE-IDENTITY vs .*: matched=${RA_S:-0} samediff=0 absent=" \
        "$RED_A_LOG"; then
    echo "CHECK red-identity: ok ($RA_S samples byte-identical to GREEN's)"
else
    echo "CHECK red-identity: FAIL (Goal 2: every RED sample must be" \
        "byte-identical to GREEN's same-relative-time sample)"
    FAILED+=(red-identity)
fi
if [ "${RB_P:-0}" -ge 1 ] && grep -q "^GHOST: primary - " "$RED_B_LOG"; then
    echo "CHECK red-b-ghost: ok (the D2 ghost, byte-identical structure)"
else
    echo "CHECK red-b-ghost: FAIL (want >= 1 packet with GHOST: primary;" \
        "rel-times-only does not count, D-L4b)"
    FAILED+=(red-b-ghost)
fi
if grep -q "eid543f=1$" "$RED_A_LOG"; then
    echo "CHECK red-events: ok (one TM(5,2) 0x543F)"
else
    echo "CHECK red-events: FAIL (want eid543f=1)"
    FAILED+=(red-events)
fi

# GREEN: the complete axis, a silent B
if [ "${GA_S:-0}" -eq "$C_TRUTH" ]; then
    echo "CHECK green-a-samples: ok ($GA_S = the truth's $C_TRUTH chunks)"
else
    echo "CHECK green-a-samples: FAIL (samples ${GA_S:-none}, want" \
        "$C_TRUTH)"
    FAILED+=(green-a-samples)
fi
if grep -q " dups=0 rcr=\[0\] ebins=\[0x3e1\] pixel=\[0xfff\] closing=" \
        "$GREEN_A_LOG"; then
    echo "CHECK green-a-shape: ok (dups 0, rcr [0], ebin 0x3e1, pixel 0xfff)"
else
    echo "CHECK green-a-shape: FAIL (dups/rcr/ebin/pixel mismatch)"
    FAILED+=(green-a-shape)
fi
if grep -q "^EXPECTED-SET: PASS missing=0 extra=0 (expected=$C_TRUTH decoded=" \
        "$GREEN_A_LOG"; then
    echo "CHECK green-a-set: ok (every archived chunk exactly once)"
else
    echo "CHECK green-a-set: FAIL (want EXPECTED-SET PASS missing=0 extra=0)"
    FAILED+=(green-a-set)
fi
if [ "${GB_P:-1}" -eq 0 ] && [ "${GB_S:-1}" -eq 0 ] &&
        grep -q "^GHOST: none - " "$GREEN_B_LOG"; then
    echo "CHECK green-b-silent: ok (0 packets, only the 0x543F event)"
else
    echo "CHECK green-b-silent: FAIL (want 0 SSID-24 packets and GHOST none)"
    FAILED+=(green-b-silent)
fi
if grep -q "eid543f=1$" "$GREEN_A_LOG"; then
    echo "CHECK green-events: ok (one TM(5,2) 0x543F)"
else
    echo "CHECK green-events: FAIL (want eid543f=1)"
    FAILED+=(green-events)
fi
if grep -q "SAMPLE-IDENTITY vs .*: matched=${RA_S:-0} samediff=0 absent=" \
        "$GREEN_A_LOG"; then
    echo "CHECK green-identity: ok (GREEN's samples at RED's $RA_S rels match)"
else
    echo "CHECK green-identity: FAIL (GREEN vs RED identity)"
    FAILED+=(green-identity)
fi

GOT="${FAILED[*]:-}"
echo "L4-REPLAY summary: truth chunks=$C_TRUTH red-round=$R_TRUTH;" \
    "red A $RA_S samples / B $RB_P packets; green A $GA_S samples /" \
    "B $GB_P packets" | tee -a "$RECORD"
if [ -n "$GOT" ]; then
    echo "L4-REPLAY: FAIL (failed checks: $GOT)${STIX_SLOT_TAG}"
    exit 1
fi
echo "L4-REPLAY: PASS (green $GA_S/$C_TRUTH complete, B silent; red" \
    "$RA_S/$C_TRUTH = the red-round $R_TRUTH signature, ghost primary;" \
    "identity ok — an emulator result, not hardware evidence)${STIX_SLOT_TAG}" |
    tee -a "$RECORD"
rm -f "$RUN_DIR/.unfinished"
# on PASS drop the three working flashes (the snapshot survives for
# re-decode); a FAIL keeps everything for diagnosis
rm -f "$COLLECT_DIR/flash.bin" "$RED_DIR/flash.bin" "$GREEN_DIR/flash.bin"
exit 0
