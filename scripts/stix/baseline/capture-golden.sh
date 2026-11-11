#!/usr/bin/env bash
# Capture the golden baseline artifacts from the STIX model (since R3: the
# current model on the normal FSW ELF; originally the old v5.2 model).
#
# Usage: capture-golden.sh <qemu-binary> <out-dir>
#
# Notes:
#   - Guest UART goes to its own file (-serial file:). QEMU's device
#     activity (CTRL/SPW/flash/TMTC, -trace 'stix*' on stderr) and the
#     flash realize info lines are captured into qemu-stdout.golden.log
#     (stdout+stderr merged).
#   - NO -icount: under icount the WDT path reaches the CTRL SCET-set
#     registers and hits assert(0) in grlib_stixctrl.c (QEMU SIGABRT).
#     Determinism is provided by content stability instead (verified: two
#     90 s runs produce identical normalized content).
#   - flash.bin (FLASH_TOTAL_STORAGE = 17,716,740,096 B apparent) lives in
#     RUN_DIR (STIX_RUN_DIR, default /tmp/stix-uprev-run). Since D-I the
#     model creates a missing file instantly as a sparse, inverted file (a
#     hole is erased flash), so it costs only the pages the FSW writes. This
#     script never deletes it. /tmp/stix-baseline-run holds the frozen v11.1
#     reference (a plain 16.5 GB file): never run there.
#   - The TMTC server listens on IO_TCP_SERVER_BASE_PORT + getuid()
#     (20000 + uid, e.g. 21000), NOT the 2221 constant.
set -euo pipefail

QEMU_BIN="${1:?usage: capture-golden.sh <qemu-binary> <out-dir>}"
OUT_DIR="${2:?usage: capture-golden.sh <qemu-binary> <out-dir>}"
ASW_ELF="${STIX_ASW_ELF:-/tmp/stix-baseline-run/ASW_em_baseline.elf}"
RUN_DIR="${STIX_RUN_DIR:-/tmp/stix-uprev-run}"
DURATION=90

# The one intended writer of the master flash: slot mode (STIX_PORT_BASE,
# scripts/stix/lib/slot.sh) never opens the shared flash, so it is
# refused here, and the flags recorded below never change.
if [ -n "${STIX_PORT_BASE:-}" ]; then
    echo "capture-golden.sh refuses slot mode (STIX_PORT_BASE=$STIX_PORT_BASE):" \
        "it writes the master flash; unset STIX_PORT_BASE"
    exit 2
fi
. "$(dirname "${BASH_SOURCE[0]}")/../lib/slot.sh"

[ -x "$QEMU_BIN" ] || { echo "not executable: $QEMU_BIN"; exit 2; }
[ -f "$ASW_ELF" ] || { echo "ASW ELF missing: $ASW_ELF"; exit 2; }
stix_disk_guard

# The TMTC server binds 20000+getuid(); refuse to run if it is taken.
TMTC_PORT=$((20000 + $(id -u)))
if ss -ltn | grep -q ":${TMTC_PORT} "; then
    echo "TMTC port ${TMTC_PORT} busy — another QEMU instance running?"
    exit 2
fi

mkdir -p "$RUN_DIR" "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"   # absolute: the script cd's to RUN_DIR next
cd "$RUN_DIR"

if [ "$RUN_DIR" = /tmp/stix-baseline-run ]; then
    echo "refusing to run in the frozen v11.1 reference dir $RUN_DIR"; exit 2
fi
# Exclusive against the slot-mode copies (stix_flash_private takes it
# shared): no copy is taken while this run writes the master.
exec {FLASH_LOCK_FD}>> .flash.lock
flock -x -w 60 "$FLASH_LOCK_FD" || {
    echo "the flash lock $RUN_DIR/.flash.lock is busy (a slot-mode copy?)"; exit 2; }
[ -f flash.bin ] || echo "flash.bin absent: the model creates it (sparse, inverted; D-I)"

echo "Capturing golden artifacts (${DURATION}s)..."
timeout "$DURATION" "$QEMU_BIN" \
    -M leon3_generic -m 64M -no-reboot \
    -display none -monitor none \
    -serial file:uart.golden.raw \
    -kernel "$ASW_ELF" \
    -trace 'stix*' \
    > qemu-stdout.golden.log 2>&1 || true

# in_asm head: translation log of the same run would need -d; a separate
# short run keeps the golden run clean and the artifact small.
timeout 10 "$QEMU_BIN" \
    -M leon3_generic -m 64M -no-reboot -display none -monitor none \
    -serial file:/dev/null \
    -kernel "$ASW_ELF" -d in_asm -D asm.golden.raw \
    > /dev/null 2>&1 || true
head -200 asm.golden.raw > "$OUT_DIR/asm-head.golden.log" 2>/dev/null || true

grep -q "STIX ASW_v2" uart.golden.raw || {
    echo "capture FAILED: no STIX banner in UART output"; exit 2;
}

cp uart.golden.raw "$OUT_DIR/uart.golden.log"
cp qemu-stdout.golden.log "$OUT_DIR/"

cat > "$OUT_DIR/run-flags.txt" <<EOF
qemu-binary: $QEMU_BIN
asw-elf: $ASW_ELF
machine-args: -M leon3_generic -m 64M -no-reboot -display none
    -monitor none -serial file:<uart> -trace 'stix*'
icount: none (see header note in capture-golden.sh)
duration-seconds: $DURATION
tmtc-port: $TMTC_PORT (IO_TCP_SERVER_BASE_PORT + getuid)
asw-elf-sha256: $(sha256sum "$ASW_ELF" | cut -d" " -f1)
flash-bin: $RUN_DIR/flash.bin (sparse, inverted storage, D-I; persistent, never delete)
EOF

echo "Golden artifacts in $OUT_DIR:"
ls -la "$OUT_DIR"
