#!/usr/bin/env bash
# Long-run soak: boot the STIX model with the ASW ELF and verify the FSW is
# still alive at the end of the window. Catches steady-state crashes like
# the SCET assert abort (review finding T1) and guest hangs.
#
# Liveness = QEMU still runs at the end, and the TM FLOW kept going over a
# WINDOW-second window ending MARGIN seconds before the end: the window is
# split into consecutive SLICE-second slices and every slice must see at
# least one new TM chunk line in the probe log — sampled WHILE QEMU IS
# STILL RUNNING; the script itself ends the run. The first version of this
# script ran QEMU in the foreground and sampled afterwards, so it compared
# a closed file with itself and failed every run — the reported "FSW stall
# at ~4.8 min" was the 600 s kill landing at FSW time (which runs ~0.48x
# wall). See docs/stix-port/2026-11-09-task-b-stall-analysis.md and
# scripts/stix/tests/test-soak.sh.
#
# The heartbeat is TM flow, not UART growth: R3 models the watchdog device,
# so the FSW Watchdog task's once-per-second error print is gone and a
# healthy guest can have a silent UART for the whole window. Housekeeping
# TM packets arrive about once per second on the TMTC TCP port that
# tmtc-probe.py reads (started here, as the operational configuration).
# A chunk line is what the probe prints per received chunk:
# "HH:MM:SS +NNNNB <hex>"; only its COUNT is sampled — the HH:MM:SS
# timestamps are wall clock and WSL2 steps the clock. UART growth over the
# window is still measured and reported in the PASS line, but not gated on.
#
# Usage: soak.sh <qemu-binary> [seconds=600]
#   STIX_RUN_DIR, STIX_ASW_ELF   as in boot-smoke.sh
#   STIX_SOAK_WINDOW (120)       liveness window, seconds
#   STIX_SOAK_SLICE (60)         slice length; WINDOW must be a positive
#                                multiple of SLICE
#   STIX_TMTC_PROBE              TM probe script (tests substitute a fake)
#   STIX_SOAK_MARGIN (30)        gap between window end and the end of the run
#   STIX_PORT_BASE               the slot (scripts/stix/lib/slot.sh); slot
#                                mode boots a private copy of the flash
#
# Exit codes: 0 pass, 1 TM heartbeat stopped (no TM chunk in some slice),
#             2 environment problem, QEMU crash, or QEMU exited early.
set -euo pipefail

QEMU_BIN_ARG="${1:?usage: soak.sh <qemu-binary> [seconds]}"
SECONDS_TOTAL="${2:-600}"
WINDOW="${STIX_SOAK_WINDOW:-120}"
SLICE="${STIX_SOAK_SLICE:-60}"
MARGIN="${STIX_SOAK_MARGIN:-30}"
ASW_ELF="${STIX_ASW_ELF:-/tmp/stix-baseline-run/ASW_em_baseline.elf}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_DIR="${STIX_RUN_DIR:-/tmp/stix-uprev-run}"

if [ "$SECONDS_TOTAL" -le "$((WINDOW + MARGIN))" ]; then
    echo "soak window ${WINDOW}s + margin ${MARGIN}s does not fit in ${SECONDS_TOTAL}s"
    exit 2
fi
if [ "$SLICE" -le 0 ] || [ "$WINDOW" -le 0 ] || [ $((WINDOW % SLICE)) -ne 0 ]; then
    echo "soak: STIX_SOAK_WINDOW (${WINDOW}s) must be a positive multiple of STIX_SOAK_SLICE (${SLICE}s)"
    exit 2
fi

QEMU_BIN="$(realpath -e "$QEMU_BIN_ARG")" || {
    echo "qemu binary not found: $QEMU_BIN_ARG"; exit 2;
}
[ -x "$QEMU_BIN" ] || { echo "not executable: $QEMU_BIN"; exit 2; }
[ -f "$ASW_ELF" ] || { echo "ASW ELF missing: $ASW_ELF"; exit 2; }

# Slot mode (STIX_PORT_BASE, scripts/stix/lib/slot.sh): the slot's port
# and run dir <dir>-s<k>; unset = legacy, 20000+uid and the dir as is.
. "$REPO_ROOT/scripts/stix/lib/slot.sh"
stix_slot_setup
RUN_DIR="$(stix_slot_dir "$RUN_DIR")"
stix_run_dir_guard "$RUN_DIR"
if ss -ltn | grep -q ":${TMTC_PORT} "; then
    echo "TMTC port ${TMTC_PORT} busy — another QEMU instance running?${STIX_SLOT_TAG}"
    exit 2
fi

mkdir -p "$RUN_DIR"
cd "$RUN_DIR"
stix_flash_private
[ -f flash.bin ] || { echo "flash.bin missing — run capture-golden.sh first"; exit 2; }
rm -f uart.soak.raw tm.soak-probe.log qemu-stdout.soak.log
: > uart.soak.raw

PROBE_PID=""
QEMU_PID=""
cleanup() {
    # `|| true`: under set -e a failed kill (process already gone) would
    # otherwise become the script's exit status
    if [ -n "$QEMU_PID" ]; then kill "$QEMU_PID" 2>/dev/null || true; fi
    if [ -n "$PROBE_PID" ]; then kill "$PROBE_PID" 2>/dev/null || true; fi
}
trap cleanup EXIT

# TM probe: retries connecting until its own deadline, so start it first
# and let it catch the server as soon as QEMU binds. -u because stdout is
# a file here, so Python block-buffers it — the per-slice counts below read
# this log while the probe still runs, and unflushed lines would read as
# "no TM" until exit. (STIX_TMTC_PROBE names a python script too.)
timeout "$((SECONDS_TOTAL + 20))" python3 -u "${STIX_TMTC_PROBE:-$REPO_ROOT/scripts/stix/tmtc-probe.py}" \
    --port "$TMTC_PORT" --seconds "$((SECONDS_TOTAL + 10))" \
    > tm.soak-probe.log 2>&1 &
PROBE_PID=$!

# The script ends the run itself (below), on the same clock it samples
# with; timeout is only a backstop if the script dies. (A soak where
# timeout owned the end killed QEMU ~34 s early on WSL2 — the host's time
# sync stepped the clock — and the kill landed inside the window.)
timeout "$((SECONDS_TOTAL + 60))" "$QEMU_BIN" \
    -M leon3_generic -m 64M -no-reboot \
    -display none -monitor none \
    "${STIX_SLOT_QEMU_ARGS[@]}" \
    -serial file:"$RUN_DIR/uart.soak.raw" \
    -kernel "$ASW_ELF" > qemu-stdout.soak.log 2>&1 &
QEMU_PID=$!

# All deadlines are measured from QEMU's start on the monotonic clock
# (/proc/uptime), not chained sleeps, so polling overhead cannot drift.
now_ms() { awk '{ printf "%d", $1 * 1000 }' /proc/uptime; }
QEMU_T0=$(now_ms)

# Sleep until <offset seconds> after QEMU start, returning early (status 1)
# if QEMU has exited.
sleep_until_while_qemu_runs() {
    local end=$((QEMU_T0 + $1 * 1000))
    while [ "$(now_ms)" -lt "$end" ]; do
        kill -0 "$QEMU_PID" 2>/dev/null || return 1
        sleep 0.2
    done
    kill -0 "$QEMU_PID" 2>/dev/null
}

qemu_ended_early() {
    local rc=0
    wait "$QEMU_PID" || rc=$?
    QEMU_PID=""
    echo "SOAK: FAIL — QEMU exited rc=$rc before the ${SECONDS_TOTAL}s" \
        "limit.${STIX_SLOT_TAG} Tail:"
    tail -5 qemu-stdout.soak.log
    echo "UART tail:"
    tail -3 uart.soak.raw | tr -d '\000'
    exit 2
}

# The TM gate: sample the probe log's chunk-line COUNT at every slice
# boundary (monotonic clock, sampled while QEMU runs). The HH:MM:SS
# timestamps in the log are NOT parsed: they are wall clock, and WSL2
# steps the clock.
N_SLICES=$((WINDOW / SLICE))
WINDOW_START=$((SECONDS_TOTAL - WINDOW - MARGIN))
CHUNK_RE='^[0-9]{2}:[0-9]{2}:[0-9]{2} \+ *[0-9]+B '
tm_chunk_count() {
    grep -cE "$CHUNK_RE" tm.soak-probe.log 2>/dev/null || true
}

sleep_until_while_qemu_runs "$WINDOW_START" || qemu_ended_early
SIZE_START=$(stat -c %s uart.soak.raw)
TM_PREV="$(tm_chunk_count)"
TM_PREV="${TM_PREV:-0}"
SLICE_COUNTS=""
for ((i = 1; i <= N_SLICES; i++)); do
    SLICE_START=$((WINDOW_START + (i - 1) * SLICE))
    SLICE_END=$((WINDOW_START + i * SLICE))
    sleep_until_while_qemu_runs "$SLICE_END" || qemu_ended_early
    TM_NOW="$(tm_chunk_count)"
    TM_NOW="${TM_NOW:-0}"
    NEW_CHUNKS=$((TM_NOW - TM_PREV))
    TM_PREV="$TM_NOW"
    SLICE_COUNTS="$SLICE_COUNTS $NEW_CHUNKS"
    if [ "$NEW_CHUNKS" -eq 0 ]; then
        echo "SOAK: FAIL — TM heartbeat stopped: no TM in slice ${i}/${N_SLICES}" \
            "(${SLICE_START}s..${SLICE_END}s after start)${STIX_SLOT_TAG}"
        echo "TM probe log tail:"
        tail -5 tm.soak-probe.log
        exit 1
    fi
done
SIZE_END=$(stat -c %s uart.soak.raw)

# QEMU must still be running at the end of the run; then this script
# ends it (a healthy soak has no natural end).
sleep_until_while_qemu_runs "$SECONDS_TOTAL" || qemu_ended_early
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""
# The probe exits once QEMU's TM socket closes; give it a moment to print
# its total (the EXIT trap kills it if it never connected).
for _ in 1 2 3 4 5; do
    grep -q '^# connected' tm.soak-probe.log 2>/dev/null || break
    kill -0 "$PROBE_PID" 2>/dev/null || break
    sleep 1
done

# UART growth is reported, not gated: since R3 models the watchdog, a
# healthy guest can have a silent UART (the once-per-second watchdog error
# print is gone). The TM gate above is the liveness verdict.
TM_TOTAL=$(grep '^# total' tm.soak-probe.log 2>/dev/null | awk '{print $3}' || true)
echo "SOAK: PASS (${SECONDS_TOTAL}s; TM chunks/slice:${SLICE_COUNTS}  —" \
    "${N_SLICES} slices of ${SLICE}s; UART grew $SIZE_START -> $SIZE_END" \
    "bytes in the ${WINDOW}s window; probe TM total ${TM_TOTAL:-none}" \
    "bytes)${STIX_SLOT_TAG}"
stix_slot_cleanup 0 flash.bin
