#!/usr/bin/env bash
# Capture the golden baseline artifacts from the STIX model (since R3: the
# current model on the normal FSW ELF; originally the old v5.2 model).
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
ASW_ELF="${STIX_ASW_ELF:-/mnt/c/Users/laszl/Development/stix_asw_src/ASW/ASW/bin/ASW_em.elf}"
RUN_DIR="${STIX_RUN_DIR:-/tmp/stix-uprev-run}"
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
OUT_DIR="$(cd "$OUT_DIR" && pwd)"   # absolute: the script cd's to RUN_DIR next
cd "$RUN_DIR"

if [ "$RUN_DIR" = /tmp/stix-baseline-run ]; then
    echo "refusing to run in the frozen v11.1 reference dir $RUN_DIR"; exit 2
fi
[ -f flash.bin ] || echo "flash.bin absent: the model creates it (sparse, inverted; D-I)"

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
asw-elf-sha256: $(sha256sum "$ASW_ELF" | cut -d" " -f1)
flash-bin: $RUN_DIR/flash.bin (sparse, inverted storage, D-I; persistent, never delete)
EOF

echo "Golden artifacts in $OUT_DIR:"
ls -la "$OUT_DIR"
