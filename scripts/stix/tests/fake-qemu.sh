#!/usr/bin/env bash
# Fake QEMU for the gate-script tests: parses -serial file:X and, per
# FAKE_QEMU_MODE, either writes the golden UART log to X (good), writes
# nothing (silent), exits 1 immediately (crash), appends a line every
# 0.5 s until killed or FAKE_QEMU_RUN_SECONDS elapse (heartbeat), or
# appends for FAKE_QEMU_FREEZE_AFTER seconds and then stays alive but
# silent — a hung guest (freeze). For pfm-boot-smoke: pfm writes one
# watchdog RELOAD into the -trace file and stays alive (the positive
# run's live trace), pfm-reset writes a watchdog "count expired" reset
# and exits at once (the latency-off negative).
#
# FAKE_QEMU_ARGV_FILE, when set, gets one record per invocation appended:
# "argv:" plus the arguments one per line, "cwd: <dir>" and
# "flash-inode: <inode of ./flash.bin, or none>", then "end". A
# -trace ...,file=X argument creates X empty.
set -u
MODE="${FAKE_QEMU_MODE:-good}"
SELF="$(readlink -f "$0")"
if [ -n "${FAKE_QEMU_ARGV_FILE:-}" ]; then
    {
        echo "argv:"
        printf '%s\n' "$@"
        echo "cwd: $(pwd)"
        echo "flash-inode: $(stat -c %i flash.bin 2>/dev/null || echo none)"
        echo "end"
    } >> "$FAKE_QEMU_ARGV_FILE"
fi
SERIAL_OUT=""
TRACE_FILE=""
prev=""
for a in "$@"; do
    if [ "$prev" = "-serial" ]; then
        SERIAL_OUT="${a#file:}"
    fi
    # -trace ...,file=X: create X empty, like QEMU with no event fired
    if [ "$prev" = "-trace" ] && [ "${a#*file=}" != "$a" ]; then
        TRACE_FILE="${a#*file=}"
        : > "$TRACE_FILE"
    fi
    prev="$a"
done
[ -n "$SERIAL_OUT" ] || { echo "fake-qemu: no -serial file:X arg" >&2; exit 3; }

case "$MODE" in
    good)
        cp "$(dirname "$SELF")/../../../docs/stix-port/baseline/uart.golden.log" "$SERIAL_OUT"
        sleep "${FAKE_QEMU_RUN_SECONDS:-30}"
        ;;
    silent)
        sleep "${FAKE_QEMU_RUN_SECONDS:-30}"
        ;;
    heartbeat)
        end=$((SECONDS + ${FAKE_QEMU_RUN_SECONDS:-30}))
        while [ "$SECONDS" -lt "$end" ]; do
            echo "fake-qemu heartbeat $SECONDS" >> "$SERIAL_OUT"
            sleep 0.5
        done
        ;;
    freeze)
        end=$((SECONDS + ${FAKE_QEMU_FREEZE_AFTER:-2}))
        while [ "$SECONDS" -lt "$end" ]; do
            echo "fake-qemu heartbeat $SECONDS" >> "$SERIAL_OUT"
            sleep 0.5
        done
        sleep "${FAKE_QEMU_RUN_SECONDS:-30}"
        ;;
    crash)
        echo "fake-qemu: simulated crash" >&2
        exit 1
        ;;
    pfm)
        # one live watchdog RELOAD (cmd=1), as the FSW feeds it
        echo "stixctrl_watchdog_cmd watchdog cmd=1 reload=536870912" \
            "count=536870912 running=1" >> "$TRACE_FILE"
        sleep "${FAKE_QEMU_RUN_SECONDS:-30}"
        ;;
    pfm-reset)
        # the latency-off negative: the watchdog expires, QEMU exits
        echo "stixctrl_watchdog_reset watchdog resets the IDPU:" \
            "count expired" >> "$TRACE_FILE"
        ;;
    *)
        echo "fake-qemu: unknown FAKE_QEMU_MODE '$MODE'" >&2
        exit 3
        ;;
esac
