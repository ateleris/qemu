#!/usr/bin/env bash
# P2-2 (task F1) guard: the STIX devices must not print to the QEMU
# console by default. Every live qemu_printf in grlib_stixspw.c,
# tmtc_io_server.c, grlib_stixctrl.c and grlib_stixflash.c became a
# trace_stix* event (accepted-diffs.md): the console stays silent unless
# the user asks for the events with -trace 'stix*'.
#
# Usage: test-no-console-prints.sh <qemu-binary> <stix-qtest-binary>
#
# Runs the spw-tx, ctrl and flash qtests — which between them drive the
# converted MMIO, TX/no-client and flash-realize print paths (the TMTC
# server paths are off under qtest, P2-1) — with stdout+stderr captured.
# Fails if a qtest exits non-zero, reports a skip or a failure, or if any
# of the old console prints appears.
set -u

QEMU="${1:?usage: test-no-console-prints.sh <qemu-binary> <stix-qtest-binary>}"
STIX_QTEST="${2:?usage: test-no-console-prints.sh <qemu-binary> <stix-qtest-binary>}"

[ -x "$QEMU" ] || { echo "not executable: $QEMU"; exit 2; }
[ -x "$STIX_QTEST" ] || { echo "not executable: $STIX_QTEST"; exit 2; }

# The old console prints, verbatim from the pre-P2-2 sources: the SPW and
# CTRL MMIO prefixes, the TMTC server/packet lines, and the flash realize
# prints (grlib_stixflash.c "page size:", "file:", "can't open file, ...",
# "problem closing file", "file size = ").
PATTERNS=(
    "SPW |"
    "CTRL |"
    "FLASH |"
    "IOTCPSERVER"
    "recv_packet"
    "STIX SPW"
    "socket is closed"
    "Send packet"
    "page size: "
    "file: "
    "can't open file, creating a new one"
    "problem closing file"
    "file size = "
)

TMP_OUT="$(mktemp)"
trap 'rm -f "$TMP_OUT"' EXIT

rc=0
for t in spw-tx ctrl flash; do
    QTEST_QEMU_BINARY="$QEMU" "$STIX_QTEST" --tap -p "/sparc/sparc/stix/$t" \
        > "$TMP_OUT" 2>&1
    trc=$?

    if [ "$trc" -ne 0 ]; then
        echo "FAIL - qtest $t exited rc=$trc"
        tail -5 "$TMP_OUT"
        rc=1
        continue
    fi
    if grep -q "^not ok" "$TMP_OUT"; then
        echo "FAIL - qtest $t reported a failure"
        grep "^not ok" "$TMP_OUT" | head -3
        rc=1
        continue
    fi
    if grep -q "# SKIP" "$TMP_OUT"; then
        echo "FAIL - qtest $t reported a skip"
        grep "# SKIP" "$TMP_OUT" | head -3
        rc=1
        continue
    fi
    if ! grep -q "^ok 1 /" "$TMP_OUT"; then
        echo "FAIL - qtest $t did not pass (binary or machine missing?)"
        tail -5 "$TMP_OUT"
        rc=1
        continue
    fi

    hit=""
    for p in "${PATTERNS[@]}"; do
        if grep -Fq -- "$p" "$TMP_OUT"; then
            hit="$p"
            break
        fi
    done

    if [ -n "$hit" ]; then
        echo "FAIL - $t: console print matched: $hit"
        grep -F -- "$hit" "$TMP_OUT" | head -5
        rc=1
    else
        echo "ok   - $t: no console prints"
    fi
done

exit $rc
