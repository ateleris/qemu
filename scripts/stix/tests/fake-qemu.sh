#!/usr/bin/env bash
# Fake QEMU for boot-smoke tests: parses -serial file:X and, per
# FAKE_QEMU_MODE, either writes the golden UART log to X (good), writes
# nothing (silent), or exits 1 immediately (crash).
set -u
MODE="${FAKE_QEMU_MODE:-good}"
SELF="$(readlink -f "$0")"
SERIAL_OUT=""
prev=""
for a in "$@"; do
    if [ "$prev" = "-serial" ]; then
        SERIAL_OUT="${a#file:}"
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
    crash)
        echo "fake-qemu: simulated crash" >&2
        exit 1
        ;;
    *)
        echo "fake-qemu: unknown FAKE_QEMU_MODE '$MODE'" >&2
        exit 3
        ;;
esac
