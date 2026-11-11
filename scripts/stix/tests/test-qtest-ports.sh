#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Gate-ports plan G-3: which TCP/UDP ports the STIX qtest suite binds.
# bind(2) calls of the qtest and of every QEMU it spawns are logged by an
# LD_PRELOAD shim (bind-log.c; a stand-in for strace -f -e trace=bind).
#
# Usage: test-qtest-ports.sh <qemu-binary> <stix-qtest-binary> [--suite]
#   default  (a) tmtc-hermetic must pass (a SKIP fails) and bind nothing
#            on the default TM port 20000+uid
#   --suite  (c) the whole suite under a slot (STIX_PORT_BASE=auto, or a
#            slot number; the suite reads STIX_QTEST_PORT_BASE): all ok,
#            at least one bind logged from a spawned QEMU, and every
#            successful bind is either ephemeral (port 0) or inside the
#            slot's suite band B+3..B+99. A refused bind (EADDRINUSE)
#            is a test's own "port busy" case on a port it holds, and is
#            only counted.
#
# Exit codes: 0 pass, 1 fail, 2 environment problem.
set -u

QEMU="${1:?usage: test-qtest-ports.sh <qemu-binary> <stix-qtest-binary> [--suite]}"
QT="${2:?usage: test-qtest-ports.sh <qemu-binary> <stix-qtest-binary> [--suite]}"
MODE="${3:-}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_PORT=$((20000 + $(id -u)))

[ -x "$QEMU" ] && [ -x "$QT" ] || { echo "not executable: $QEMU / $QT"; exit 2; }
W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
cc -shared -fPIC -o "$W/bind-log.so" "$HERE/bind-log.c" -ldl ||
    { echo "cannot build the bind logger"; exit 2; }
QEMU="$(realpath "$QEMU")"
QT="$(realpath "$QT")"

run_logged() { # out-file qtest-args...; sets QT_PID (the suite's pid)
    local out="$1"
    shift
    env QTEST_QEMU_BINARY="$QEMU" LD_PRELOAD="$W/bind-log.so" \
        BIND_LOG_FILE="$W/binds.log" "$QT" --tap "$@" > "$out" 2>&1 &
    QT_PID=$!
    wait "$QT_PID"
}

if [ "$MODE" != --suite ]; then
    : > "$W/binds.log"
    run_logged "$W/out" -p /sparc/sparc/stix/tmtc-hermetic
    rc=$?
    if [ ! -s "$W/binds.log" ]; then
        echo "QTEST-PORTS (a): ERROR (the bind logger saw no bind at all)"
        exit 2
    fi
    # a SKIP is not a pass: on this binary the test must really run
    if [ "$rc" -ne 0 ] || ! grep -q '^ok ' "$W/out" ||
        grep -q '^not ok' "$W/out" || grep -q '# SKIP' "$W/out"; then
        tail -5 "$W/out"
        echo "QTEST-PORTS (a): FAIL (tmtc-hermetic rc=$rc)"
        exit 1
    fi
    if grep -Eq " (tcp|udp) $DEFAULT_PORT " "$W/binds.log"; then
        grep -E " (tcp|udp) $DEFAULT_PORT " "$W/binds.log" | head -3
        echo "QTEST-PORTS (a): FAIL (tmtc-hermetic binds the default TM" \
            "port $DEFAULT_PORT)"
        exit 1
    fi
    echo "QTEST-PORTS (a): PASS (tmtc-hermetic binds: $(cut -d' ' -f2-3 \
        "$W/binds.log" | sort -u | paste -sd,); none on $DEFAULT_PORT)"
    exit 0
fi

# (c): take a slot like the gates do; the suite reads STIX_QTEST_PORT_BASE
STIX_PORT_BASE="${STIX_PORT_BASE:-auto}"
. "$HERE/../lib/slot.sh"
stix_slot_setup
B="$STIX_QTEST_PORT_BASE"
: > "$W/binds.log"
run_logged "$W/out"
rc=$?
NOK="$(grep -c '^ok ' "$W/out")"
if [ "$rc" -ne 0 ] || grep -q '^not ok' "$W/out" || [ "$NOK" -eq 0 ]; then
    grep -E '^not ok|Bail|ERROR' "$W/out" | head -5
    echo "QTEST-PORTS (c): FAIL (suite rc=$rc, $NOK ok)$STIX_SLOT_TAG"
    exit 1
fi
BAD="$(awk -v lo=$((B + 3)) -v hi=$((B + 99)) \
    '$4 == "ok" && $3 != 0 && ($3 < lo || $3 > hi) { print $2 " " $3 }' \
    "$W/binds.log" | sort | uniq -c | paste -sd,)"
if [ -n "$BAD" ]; then
    echo "QTEST-PORTS (c): FAIL (binds outside $((B + 3))..$((B + 99)):" \
        "$BAD)$STIX_SLOT_TAG"
    exit 1
fi
# the logger must reach the QEMUs the suite spawns, not only the suite
NCHILD="$(awk -v p="$QT_PID" '$1 != p' "$W/binds.log" | wc -l)"
if [ "$NCHILD" -eq 0 ]; then
    echo "QTEST-PORTS (c): ERROR (no bind logged from a spawned QEMU)"
    exit 2
fi
NBAND="$(awk '$4 == "ok" && $3 != 0' "$W/binds.log" | wc -l)"
NEPH="$(awk '$4 == "ok" && $3 == 0' "$W/binds.log" | wc -l)"
NREF="$(awk '$4 != "ok"' "$W/binds.log" | wc -l)"
echo "QTEST-PORTS (c): PASS ($NOK ok; $NBAND binds in $((B + 3))..$((B + 99))," \
    "$NEPH ephemeral, $NREF refused; $NCHILD from QEMUs)$STIX_SLOT_TAG"
