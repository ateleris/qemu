#!/usr/bin/env bash
# Long-run soak: boot the STIX model with the ASW ELF and verify it survives
# the full window with UART output still growing at the end. Catches
# steady-state crashes like the SCET assert abort (review finding T1).
#
# Usage: soak.sh <qemu-binary> [seconds=600]
#   (STIX_RUN_DIR, STIX_ASW_ELF overrides as in boot-smoke.sh)
#
# Exit codes: 0 pass, 1 UART stalled, 2 environment/QEMU problem.
set -euo pipefail

QEMU_BIN_ARG="${1:?usage: soak.sh <qemu-binary> [seconds]}"
SECONDS_TOTAL="${2:-600}"
ASW_ELF="${STIX_ASW_ELF:-/mnt/c/Users/laszl/Development/stix_asw_src/ASW/ASW/bin/ASW_em.elf}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_DIR="${STIX_RUN_DIR:-/tmp/stix-baseline-run}"

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
rm -f uart.soak.raw qemu-stdout.soak.log

rc=0
timeout "$SECONDS_TOTAL" "$QEMU_BIN" \
    -M leon3_generic -m 64M -no-reboot \
    -display none -monitor none \
    -serial file:"$RUN_DIR/uart.soak.raw" \
    -kernel "$ASW_ELF" > qemu-stdout.soak.log 2>&1 || rc=$?

if [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ]; then
    echo "SOAK: FAIL — QEMU exited rc=$rc before the time limit. Tail:"
    tail -5 qemu-stdout.soak.log
    exit 2
fi

# UART must still be growing in the last minute: compare the line count
# against a snapshot taken ~60 s before the end.
SIZE_NOW=$(stat -c %s uart.soak.raw)
sleep 60
SIZE_END=$(stat -c %s uart.soak.raw)
if [ "$SIZE_END" -le "$SIZE_NOW" ]; then
    echo "SOAK: FAIL — UART did not grow in the last 60 s ($SIZE_NOW -> $SIZE_END)"
    exit 1
fi
echo "SOAK: PASS (${SECONDS_TOTAL}s, UART grew $SIZE_NOW -> $SIZE_END bytes in the last minute)"
