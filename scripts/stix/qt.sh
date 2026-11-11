#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# qt.sh: build, then run the STIX qtest suite (or one test) under a slot
# lock (gate-ports plan D-G5, scripts/stix/lib/slot.sh).
#
# Usage: scripts/stix/qt.sh [--baseline [--full]] [--no-build] [test-name|all]
#   e.g. scripts/stix/qt.sh flash-error-paths
#
# Default: STIX_PORT_BASE=auto (or a slot number from the caller) takes a
# slot in 1..STIX_SLOTS and exports its base port as STIX_QTEST_PORT_BASE,
# so the suite's QEMU-bound ports come from the slot band B+3..B+99
# (tests/qtest/stix.c stix_free_*_port). Both the suite and single tests
# take a slot, so they stay inside the CPU cap. No global 21000 lock and
# no ss check.
#
# --baseline: QTEST_QEMU_BINARY is the 5.2 wrapper
# (baseline/qemu-system-sparc-52-wrapper.sh). The 5.2 device always binds
# 20000+uid, so this takes slot 0 (/tmp/stix-qtest-21000.lock, waiting
# for it like the legacy convention), and it runs in STIX_QT_BASELINE_DIR
# (default /tmp/stix-qt-baseline): the wrapper strips flash-file, so the
# 5.2 model opens flash.bin in its CWD and, if the file is missing,
# writes a full 16.5 GB one. The dir therefore gets a private sparse copy
# of the master flash first (the slot-mode guards), and the run is
# refused without one.
#
# --baseline is for tmtc-hermetic and other tests that do not read flash
# content: the master is stored inverted (D-I), which the 5.2 model reads
# as all-0x00 flash, so flash-content tests fail there spuriously. Without
# a test name it runs only STIX_QT_BASELINE_TESTS (default
# "tmtc-hermetic"); --full runs the whole suite anyway.
#
# Test hooks: STIX_QT_MAKE (the make command), STIX_QT_SUITE (the qtest
# binary). Exit: the suite's status; 2 environment, 3 build failed.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BASELINE=0
FULL=0
BUILD=1
T=all
for a in "$@"; do
    case "$a" in
        --baseline) BASELINE=1 ;;
        --full) FULL=1 ;;
        --no-build) BUILD=0 ;;
        -*) echo "unknown option: $a"; exit 2 ;;
        *) T="$a" ;;
    esac
done

if [ "$FULL" -eq 1 ] && [ "$BASELINE" -eq 0 ]; then
    echo "--full only applies to --baseline (the default run is the suite)"
    exit 2
fi
cd "$REPO_ROOT" || exit 2
if [ "$BUILD" -eq 1 ]; then
    # shellcheck disable=SC2086 # STIX_QT_MAKE is a command line
    ${STIX_QT_MAKE:-make} -C build -j8 qemu-system-sparc tests/qtest/stix 2>&1 \
        | grep -E 'error|warning|FAILED|Linking' | tail -20
    [ "${PIPESTATUS[0]}" -eq 0 ] || { echo "BUILD FAILED"; exit 3; }
fi
SUITE="${STIX_QT_SUITE:-$REPO_ROOT/build/tests/qtest/stix}"

. "$REPO_ROOT/scripts/stix/lib/slot.sh"
if [ "$BASELINE" -eq 1 ]; then
    unset STIX_PORT_BASE STIX_QTEST_PORT_BASE
    QTEST_QEMU_BINARY="$REPO_ROOT/scripts/stix/baseline"
    export QTEST_QEMU_BINARY="$QTEST_QEMU_BINARY/qemu-system-sparc-52-wrapper.sh"
    stix_disk_guard
    exec {QT_SLOT0_FD}>> "$(stix_slot_lock 0)"
    flock -w "${STIX_SLOT_WAIT:-1800}" "$QT_SLOT0_FD" || {
        echo "slot: slot 0 ($(stix_slot_lock 0)) held for ${STIX_SLOT_WAIT:-1800}s"
        exit 2
    }
    echo "slot: 0 base $(stix_slot_base 0) (5.2 baseline)"
    stix_run_dir_guard "${STIX_QT_BASELINE_DIR:-/tmp/stix-qt-baseline}" strict
    mkdir -p "${STIX_QT_BASELINE_DIR:-/tmp/stix-qt-baseline}"
    cd "${STIX_QT_BASELINE_DIR:-/tmp/stix-qt-baseline}" || exit 2
    stix_flash_copy
else
    export QTEST_QEMU_BINARY="$REPO_ROOT/build/qemu-system-sparc"
    STIX_PORT_BASE="${STIX_PORT_BASE:-auto}"
    stix_slot_setup
fi

# the test path starts with the arch libqtest parses from the binary
# name ("sparc", or "sparc-52-wrapper.sh" for the 5.2 wrapper)
ARCH="$(basename "$QTEST_QEMU_BINARY")"
ARCH="${ARCH##*-system-}"
KEEP='^(ok|not ok|1\.\.|Bail|#.*(ERROR|assert|failed))|Aborted|Assertion'
KEEP="$KEEP|ERROR"
if [ "$BASELINE" -eq 1 ] && [ "$FULL" -eq 0 ] && [ "$T" = all ]; then
    PATHS=()
    for t in ${STIX_QT_BASELINE_TESTS:-tmtc-hermetic}; do
        PATHS+=(-p "/$ARCH/sparc/stix/$t")
    done
    echo "baseline: non-flash tests only" \
        "(${STIX_QT_BASELINE_TESTS:-tmtc-hermetic}); --full for all"
    timeout 900 "$SUITE" --tap "${PATHS[@]}" 2>&1 | grep -E "$KEEP"
    rc="${PIPESTATUS[0]}"
elif [ "$T" = all ]; then
    timeout 900 "$SUITE" --tap 2>&1 | grep -E "$KEEP"
    rc="${PIPESTATUS[0]}"
else
    timeout 600 "$SUITE" --tap -p "/$ARCH/sparc/stix/$T" 2>&1 \
        | grep -vE '^(CTRL|FLASH|SPW) \|' | tail -40
    rc="${PIPESTATUS[0]}"
fi
echo "exit=$rc$STIX_SLOT_TAG"
exit "$rc"
