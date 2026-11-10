#!/usr/bin/env bash
# Long-run soak: boot the STIX model with the ASW ELF and verify the FSW is
# still alive at the end of the window. Catches steady-state crashes like
# the SCET assert abort (review finding T1) and guest hangs.
#
# Liveness = QEMU still runs at the end, and the UART log grew over a
# WINDOW-second window ending MARGIN seconds before the end — sampled
# WHILE QEMU IS STILL RUNNING; the script itself ends the run. The first version of this script ran QEMU in the
# foreground and sampled afterwards, so it compared a closed file with
# itself and failed every run — the reported "FSW stall at ~4.8 min" was
# the 600 s kill landing at FSW time (which runs ~0.48x wall). See
# docs/stix-port/2026-11-09-task-b-stall-analysis.md and
# scripts/stix/tests/test-soak.sh.
#
# The heartbeat today is the FSW Watchdog task's once-per-second error
# print (the watchdog device is not modeled, so the error never clears).
# The TM probe stays attached as the operational configuration (ground
# station present); its TM total is reported, not gated.
#
# Usage: soak.sh <qemu-binary> [seconds=600]
#   STIX_RUN_DIR, STIX_ASW_ELF   as in boot-smoke.sh
#   STIX_SOAK_WINDOW (120)       liveness window, seconds
#   STIX_TMTC_PROBE              TM probe script (tests substitute a fake)
#   STIX_SOAK_MARGIN (30)        gap between window end and the end of the run
#
# Exit codes: 0 pass, 1 UART heartbeat stopped (guest hung),
#             2 environment problem, QEMU crash, or QEMU exited early.
set -euo pipefail

QEMU_BIN_ARG="${1:?usage: soak.sh <qemu-binary> [seconds]}"
SECONDS_TOTAL="${2:-600}"
WINDOW="${STIX_SOAK_WINDOW:-120}"
MARGIN="${STIX_SOAK_MARGIN:-30}"
ASW_ELF="${STIX_ASW_ELF:-/mnt/c/Users/laszl/Development/stix_asw_src/ASW/ASW/bin/ASW_em.elf}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_DIR="${STIX_RUN_DIR:-/tmp/stix-baseline-run}"

if [ "$SECONDS_TOTAL" -le "$((WINDOW + MARGIN))" ]; then
    echo "soak window ${WINDOW}s + margin ${MARGIN}s does not fit in ${SECONDS_TOTAL}s"
    exit 2
fi

QEMU_BIN="$(realpath -e "$QEMU_BIN_ARG")" || {
    echo "qemu binary not found: $QEMU_BIN_ARG"; exit 2;
}
[ -x "$QEMU_BIN" ] || { echo "not executable: $QEMU_BIN"; exit 2; }
[ -f "$ASW_ELF" ] || { echo "ASW ELF missing: $ASW_ELF"; exit 2; }

TMTC_PORT=$((20000 + $(id -u)))
if ss -ltn | grep -q ":${TMTC_PORT} "; then
    echo "TMTC port ${TMTC_PORT} busy — another QEMU instance running?"
    exit 2
fi

mkdir -p "$RUN_DIR"
cd "$RUN_DIR"
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
# and let it catch the server as soon as QEMU binds.
timeout "$((SECONDS_TOTAL + 20))" python3 "${STIX_TMTC_PROBE:-$REPO_ROOT/scripts/stix/tmtc-probe.py}" \
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
    echo "SOAK: FAIL — QEMU exited rc=$rc before the ${SECONDS_TOTAL}s limit. Tail:"
    tail -5 qemu-stdout.soak.log
    echo "UART tail:"
    tail -3 uart.soak.raw | tr -d '\000'
    exit 2
}

sleep_until_while_qemu_runs "$((SECONDS_TOTAL - WINDOW - MARGIN))" || qemu_ended_early
SIZE_START=$(stat -c %s uart.soak.raw)
sleep_until_while_qemu_runs "$((SECONDS_TOTAL - MARGIN))" || qemu_ended_early
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

if [ "$SIZE_END" -le "$SIZE_START" ]; then
    echo "SOAK: FAIL — UART heartbeat stopped: no output in the ${WINDOW}s window before T-${MARGIN}s ($SIZE_START -> $SIZE_END bytes)"
    echo "UART tail:"
    tail -5 uart.soak.raw | tr -d '\000'
    exit 1
fi

TM_TOTAL=$(grep '^# total' tm.soak-probe.log 2>/dev/null | awk '{print $3}' || true)
echo "SOAK: PASS (${SECONDS_TOTAL}s; UART grew $SIZE_START -> $SIZE_END bytes in the ${WINDOW}s window; probe TM total ${TM_TOTAL:-none} bytes)"
