#!/bin/sh
# Start the STIX emulator for the ATS ground station: TM server on
# 127.0.0.1:${STIX_GS_TM_PORT:-21500} (not the default 21000, which the
# qtests and gate scripts use), run dir /tmp/stix-gs-run with its own
# sparse flash.bin (created on first run, a few KB on disk).
#
# Usage: run-qemu-gs.sh <qemu-system-sparc>
#   STIX_ASW_ELF     FSW ELF (default: the normal build ASW_em_baseline.elf)
#   STIX_GS_RUN_DIR  run dir (default /tmp/stix-gs-run)
#   STIX_GS_TM_PORT  TM TCP port (default 21500)
# FSW console: tail -f $STIX_GS_RUN_DIR/uart.raw | tr -d '\000'
set -eu
QEMU="${1:?usage: run-qemu-gs.sh <qemu-system-sparc>}"
ELF="${STIX_ASW_ELF:-/tmp/stix-baseline-run/ASW_em_baseline.elf}"
RUN="${STIX_GS_RUN_DIR:-/tmp/stix-gs-run}"
PORT="${STIX_GS_TM_PORT:-21500}"
[ "$RUN" = /tmp/stix-baseline-run ] && { echo "refusing the frozen v11.1 run dir"; exit 2; }
mkdir -p "$RUN" && cd "$RUN"
# The model prints every SPW register access on stdout; keep only the
# connection and packet lines so the log stays small.
"$QEMU" -M leon3_generic -m 64M -no-reboot -display none -monitor none \
    -serial file:"$RUN/uart.raw" -kernel "$ELF" \
    -global grlib-stixspw.tmtc-port="$PORT" 2>&1 \
  | grep --line-buffered -E "IOTCPSERVER|[Cc]lient|Send packet len|recv_packet|socket is closed" \
  > "$RUN/qemu.log"
