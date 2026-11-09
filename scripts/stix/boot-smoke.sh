#!/usr/bin/env bash
# Boot smoke: run the STIX model with the ASW ELF and diff the normalized
# UART log against the golden baseline.
#
# Usage: boot-smoke.sh <qemu-binary> [--with-tm]
#   (STIX_ASW_ELF overrides the default ELF path; unset + absent => SKIP)
#   (STIX_RUN_DIR overrides the run directory; STIX_SMOKE_SECONDS the window)
#
# Exit codes: 0 pass (or SKIP), 1 diff, 2 environment problem.
set -euo pipefail

QEMU_BIN_ARG="${1:?usage: boot-smoke.sh <qemu-binary> [--with-tm]}"
ASW_ELF="${STIX_ASW_ELF:-/mnt/c/Users/laszl/Development/stix_asw_src/ASW/ASW/bin/ASW_em.elf}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GOLDEN_DIR="$REPO_ROOT/docs/stix-port/baseline"
RUN_DIR="${STIX_RUN_DIR:-/tmp/stix-baseline-run}"
DURATION="${STIX_SMOKE_SECONDS:-90}"

# Resolve the binary BEFORE cd-ing to the run directory: a relative path is
# valid in the caller's CWD, not in the run dir. Fail loudly if it does not
# exist or is not executable (T2: the old code checked -x pre-cd but kept
# the relative string, so the post-cd run silently died and stale output
# produced a false PASS).
QEMU_BIN="$(realpath -e "$QEMU_BIN_ARG")" || {
    echo "qemu binary not found: $QEMU_BIN_ARG"; exit 2;
}
[ -x "$QEMU_BIN" ] || { echo "not executable: $QEMU_BIN"; exit 2; }

if [ ! -f "$ASW_ELF" ]; then
    echo "SKIP: ASW ELF absent ($ASW_ELF); set STIX_ASW_ELF to enable"
    exit 0
fi

# Flag-drift tripwire (Review Focus: stable compares need pinned flags).
grep -q "icount: none" "$GOLDEN_DIR/run-flags.txt" || {
    echo "run-flags.txt drifted from the pinned no-icount golden"; exit 2; }

# The TMTC server binds 20000+getuid() (e.g. 21000 for uid 1000).
TMTC_PORT=$((20000 + $(id -u)))
if ss -ltn | grep -q ":${TMTC_PORT} "; then
    echo "TMTC port ${TMTC_PORT} busy — another QEMU instance running?"
    exit 2
fi

mkdir -p "$RUN_DIR"
cd "$RUN_DIR"
[ -f flash.bin ] || { echo "flash.bin missing — run capture-golden.sh first"; exit 2; }

# Stale outputs from a previous run must never satisfy this smoke.
rm -f uart.smoke.raw uart.smoke.norm uart.smoke.boot qemu-stdout.smoke.log tm.smoke.log

rc=0
timeout "$DURATION" "$QEMU_BIN" \
    -M leon3_generic -m 64M -no-reboot \
    -display none -monitor none \
    -serial file:"$RUN_DIR/uart.smoke.raw" \
    -kernel "$ASW_ELF" > qemu-stdout.smoke.log 2>&1 || rc=$?
# rc 124 = the run simply reached the time limit: the normal end of a
# healthy boot under a fixed-duration window. Anything else is a crash.
if [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ]; then
    echo "QEMU exited with rc=$rc — tail of qemu-stdout.smoke.log:"
    tail -20 qemu-stdout.smoke.log
    exit 2
fi
if [ ! -s uart.smoke.raw ]; then
    echo "uart.smoke.raw missing or empty — QEMU produced no UART output"
    exit 2
fi

python3 "$REPO_ROOT/scripts/stix/normalize-log.py" \
    uart.smoke.raw uart.smoke.norm
python3 "$REPO_ROOT/scripts/stix/normalize-log.py" \
    "$GOLDEN_DIR/uart.golden.log" uart.golden.norm

# Compare the BOOT SEQUENCE only: everything through the first
# "init: SAFE mode" line. Beyond it, steady-state content (periodic
# Watchdog/HK lines) is window-edge sensitive — the ~60 s "invalid number
# of cycles" line lands inside a 90 s wall-clock window nondeterministically.
cut_boot() {
    awk '{ print } /init: SAFE mode/ { exit }' "$1"
}
cut_boot uart.golden.norm > uart.golden.boot
cut_boot uart.smoke.norm > uart.smoke.boot

if diff -u uart.golden.boot uart.smoke.boot; then
    echo "BOOT SMOKE: PASS (normalized UART matches golden)"
else
    echo "BOOT SMOKE: FAIL (diff above)"
    exit 1
fi

# Optional TM check (--with-tm): TM flows over TCP 20000+uid as RAW packets
# (no TSIM length prefix). Runs a second boot with the probe attached and
# asserts non-empty telemetry with the CCSDS-looking header byte 0x49.
if [ "${2:-}" = "--with-tm" ] || [ "${STIX_WITH_TM:-}" = "1" ]; then
    rm -f tm.smoke.log
    ( timeout 60 python3 "$REPO_ROOT/scripts/stix/tmtc-probe.py" \
        --port "$TMTC_PORT" --seconds 40 > tm.smoke.log 2>&1 ) &
    PROBE_PID=$!
    sleep 1
    tmrc=0
    timeout 45 "$QEMU_BIN" -M leon3_generic -m 64M -no-reboot \
        -display none -monitor none -serial file:/dev/null \
        -kernel "$ASW_ELF" > /dev/null 2>&1 || tmrc=$?
    if [ "$tmrc" -ne 0 ] && [ "$tmrc" -ne 124 ]; then
        echo "TM CHECK: QEMU exited with rc=$tmrc"; exit 2
    fi
    wait $PROBE_PID || true
    if grep -Eq '\+ *[0-9]+B 49' tm.smoke.log; then
        echo "TM CHECK: PASS (telemetry observed on port $TMTC_PORT)"
    else
        echo "TM CHECK: FAIL"; cat tm.smoke.log; exit 1
    fi
fi
