#!/usr/bin/env bash
# Capture the golden baseline artifacts from the old (v5.2) STIX model.
#
# Usage: capture-golden.sh <qemu-binary> <out-dir>
#
# Notes:
#   - Guest UART goes to its own file (-serial file:), QEMU's device prints
#     (CTRL/SPW/flash register traces) go to stdout — captured separately.
#   - NO -icount: under icount the WDT path reaches the CTRL SCET-set
#     registers and hits assert(0) in grlib_stixctrl.c (QEMU SIGABRT).
#     Determinism is provided by content stability instead (verified: two
#     90 s runs produce identical normalized content).
#   - flash.bin (16.5 GB, FLASH_TOTAL_STORAGE) is created once by the model
#     in the CWD and must PERSIST. This script never deletes it; if absent
#     it performs one long creation boot first.
#   - The TMTC server listens on IO_TCP_SERVER_BASE_PORT + getuid()
#     (20000 + uid, e.g. 21000), NOT the 2221 constant.
set -euo pipefail

QEMU_BIN="${1:?usage: capture-golden.sh <qemu-binary> <out-dir>}"
OUT_DIR="${2:?usage: capture-golden.sh <qemu-binary> <out-dir>}"
ASW_ELF="${STIX_ASW_ELF:-/mnt/c/Users/laszl/Development/stix_asw_src/ASW/ASW/bin/ASW_em.elf}"
RUN_DIR=/tmp/stix-baseline-run
DURATION=90

[ -x "$QEMU_BIN" ] || { echo "not executable: $QEMU_BIN"; exit 2; }
[ -f "$ASW_ELF" ] || { echo "ASW ELF missing: $ASW_ELF"; exit 2; }

# The TMTC server binds 20000+getuid(); refuse to run if it is taken.
TMTC_PORT=$((20000 + $(id -u)))
if ss -ltn | grep -q ":${TMTC_PORT} "; then
    echo "TMTC port ${TMTC_PORT} busy — another QEMU instance running?"
    exit 2
fi

mkdir -p "$RUN_DIR" "$OUT_DIR"
cd "$RUN_DIR"

if [ ! -f flash.bin ]; then
    echo "flash.bin absent — one long creation run (~3 min, 16.5 GB)..."
    timeout 600 "$QEMU_BIN" -M leon3_generic -m 64M -no-reboot -nographic \
        -kernel "$ASW_ELF" > /dev/null 2>&1 || true
    [ -f flash.bin ] || { echo "flash.bin creation failed"; exit 2; }
fi

echo "Capturing golden artifacts (${DURATION}s)..."
timeout "$DURATION" "$QEMU_BIN" \
    -M leon3_generic -m 64M -no-reboot \
    -display none -monitor none \
    -serial file:uart.golden.raw \
    -kernel "$ASW_ELF" \
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
machine-args: -M leon3_generic -m 64M -no-reboot -display none -monitor none -serial file:<uart>
icount: none (see header note in capture-golden.sh)
duration-seconds: $DURATION
tmtc-port: $TMTC_PORT (IO_TCP_SERVER_BASE_PORT + getuid)
flash-bin: $RUN_DIR/flash.bin (16.5 GB, persistent, never delete)
EOF

echo "Golden artifacts in $OUT_DIR:"
ls -la "$OUT_DIR"
