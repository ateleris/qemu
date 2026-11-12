#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# PFM boot smoke (flash latency plan F-5,
# docs/stix-port/2026-11-11-flash-latency-plan.md "PFM boot gate"): the
# PFM flight build must boot in the emulator. Without NAND latency its
# FS-init task, lifted to priority 1 by priority inheritance on the
# filesystem mutex, never blocks during the 130,800-page boot scan, so
# the watchdog task never runs and the emulated watchdog resets the IDPU
# at about 6.8 s. With the latency model on (the default since F-3) the
# task sleeps through every NAND op, as on the IDPU.
#
# Positive mode, at the shipped defaults (no t*-us override):
#   - pfm-boot-drive.py checks the TM side and drives NOMINAL with TCs
#     anchored on TM events (its docstring has the windows). Measured on
#     the release ELF with latency on: 0x14A3 at +16 s and 0x142E at
#     +32 s are NOT gated by FS init (0x0032 came later than +46 s), so
#     the windows are: TM(5,1) 0x0022 Application SW boot success by @20
#     and before 0x0032, TM(5,1) 0x14A3 FDIR protection enabled by @40,
#     TM(5,1) 0x142E Preparing for shutdown by 0x14A3 + 25 s capped
#     @160, TM(5,1) 0x0032 Filesystem initialisation finished (status
#     01) and TM(5,3) 0x8030 Context file(s) not found by @150 each.
#     The mode change waits on the filesystem/context, so CONF goes at
#     max(0x142E, 0x0032) + 5 s, then S20 = 0 and NOMINAL 10 / 20 s
#     after CONF; the boot deadlines are evaluated even while CONF is
#     still waiting, so an expired window is named, not a later step's.
#     No TM(5,3) 0x8021 Watchdog failure detected; HK liveness after
#     0x0032. The driver prints the measured 0x0032 offset, and the
#     verdict repeats it.
#     Why a driver and not tmtc-probe.py: the windows chain on TM
#     arrivals, which --send-at's fixed offsets from the connect cannot
#     express, and a probe restart per phase would drop the connection
#     (and with it TM) between phases. The driver stays on the socket;
#     its log can be re-judged with --replay.
#   - the watchdog: QEMU runs with -no-reboot (a watchdog reset exits
#     it) and -trace enable=stixctrl_watchdog_*; PASS needs QEMU alive
#     at the deadline, no stixctrl_watchdog_reset line and at least one
#     watchdog cmd=1 (RELOAD) line, which proves the trace is live.
#   - UART, debug flavour only (an ELF containing "init: SAFE mode"):
#     "init: SAFE mode", then the four "could not find LUT (type=1..4"
#     lines, then "Main delete init" (FS init done; its UART time is
#     printed); no "Fatal error", "Read timeout", "Write timeout" or
#     "Erase timeout" line (MemFilesystem.c:1108, 1191, 1268) — the
#     witness that the latency never comes near the FSW's 50 ms
#     timeouts.
# --negative: -global grlib-stixflash.latency=off for 30 s, no TM driver;
#   PASS only if the watchdog trace says "count expired" and QEMU exited
#   by itself (-no-reboot); with the debug ELF also no "delete init".
#   This is the clean A/B that shows the gate sees the original failure.
# --no-nominal: the TM boot checks only, no TCs.
#
# Every run boots a fresh, empty sparse flash.bin in its own run dir; on
# PASS the run dir is removed unless STIX_KEEP_RUN=1, on FAIL it is kept.
#
# Usage: pfm-boot-smoke.sh <qemu-binary> [--negative] [--no-nominal]
#                          [--dry-run]
#   STIX_PFM_ELF       the release ELF (default
#                      /home/laszlo/dev/stix-elf/ASW_pfm_v186.elf)
#   STIX_PFM_DBG=1     run the debug ELF instead: STIX_PFM_DBG_ELF
#                      (default /home/laszlo/dev/stix-elf/ASW_pfm_dbg.elf)
#   STIX_PFM_RUN_DIR   run dir (default /tmp/stix-pfm-run; -neg is
#                      appended in --negative, then -s<k> in slot mode)
#   STIX_PFM_SECONDS   positive run length (default 240)
#   STIX_PFM_NEG_SECONDS  negative run length (default 30)
#   STIX_PORT_BASE     the slot (scripts/stix/lib/slot.sh); unset =
#                      legacy 20000+uid, the caller holds the 21000 flock
#   STIX_PFM_DRIVE     test hook: the TM driver, a python script run with
#                      python3 -u (default scripts/stix/pfm-boot-drive.py),
#                      like STIX_TMTC_PROBE in soak.sh
#   An ELF whose .sha256 file differs only WARNs (a newer release can be
#   tried); an AX build (nm shows moveStartCoarseT) is refused.
#
# Exit codes: 0 pass (or dry-run), 1 fail, 2 environment problem,
#             77 SKIP (the ELF is absent).
set -euo pipefail

QEMU_BIN_ARG="${1:?usage: pfm-boot-smoke.sh <qemu-binary> [--negative] \
[--no-nominal] [--dry-run]}"
NEGATIVE=0
NOMINAL=1
DRY_RUN=0
for a in "${@:2}"; do
    case "$a" in
        --negative)
            [ "$NEGATIVE" -eq 0 ] || { echo "--negative given twice"; exit 2; }
            NEGATIVE=1 ;;
        --no-nominal)
            [ "$NOMINAL" -eq 1 ] || { echo "--no-nominal given twice"; exit 2; }
            NOMINAL=0 ;;
        --dry-run)
            [ "$DRY_RUN" -eq 0 ] || { echo "--dry-run given twice"; exit 2; }
            DRY_RUN=1 ;;
        *) echo "unknown argument: $a"; exit 2 ;;
    esac
done

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$REPO_ROOT/scripts/stix/lib/paths.sh"
DRIVE="${STIX_PFM_DRIVE:-$REPO_ROOT/scripts/stix/pfm-boot-drive.py}"
if [ "${STIX_PFM_DBG:-0}" = 1 ]; then
    ELF="${STIX_PFM_DBG_ELF:-$STIX_ELF_DIR/ASW_pfm_dbg.elf}"
else
    ELF="${STIX_PFM_ELF:-$STIX_ELF_DIR/ASW_pfm_v186.elf}"
fi
RUN_DIR="${STIX_PFM_RUN_DIR:-/tmp/stix-pfm-run}"
[ "$NEGATIVE" -eq 0 ] || RUN_DIR="${RUN_DIR%/}-neg"
if [ "$NEGATIVE" -eq 1 ]; then
    DURATION="${STIX_PFM_NEG_SECONDS:-30}"
else
    DURATION="${STIX_PFM_SECONDS:-240}"
fi
case "$DURATION" in
    '' | *[!0-9]*) echo "run length must be whole seconds (got: $DURATION)"
                   exit 2 ;;
esac

. "$REPO_ROOT/scripts/stix/lib/slot.sh"
if [ "$DRY_RUN" -eq 1 ] || [ ! -f "$ELF" ]; then
    stix_slot_setup peek
else
    stix_slot_setup
fi
if [ ! -f "$ELF" ]; then
    echo "SKIP: PFM ELF absent ($ELF)"
    exit 77
fi
RUN_DIR="$(realpath -m -- "$(stix_slot_dir "$RUN_DIR")")"
stix_run_dir_guard "$RUN_DIR" strict

# capture first: under pipefail, nm|strings dying of SIGPIPE after an
# early `grep -q` match would read as "no match"
SYMS="$(nm "$ELF" 2>/dev/null || true)"
if grep -q moveStartCoarseT <<< "$SYMS"; then
    echo "refusing $ELF: an AX build (moveStartCoarseT); this gate needs PFM"
    exit 2
fi
if [ -f "$ELF.sha256" ]; then
    want="$(cut -d' ' -f1 < "$ELF.sha256")"
    have="$(sha256sum "$ELF" | cut -d' ' -f1)"
    [ "$want" = "$have" ] ||
        echo "WARN: $ELF sha256 $have differs from $ELF.sha256 ($want)"
fi
FLAVOUR=release
if grep -q 'init: SAFE mode' <<< "$(strings "$ELF" || true)"; then
    FLAVOUR=debug
fi

QEMU_BIN="$(realpath -e "$QEMU_BIN_ARG")" || {
    echo "qemu binary not found: $QEMU_BIN_ARG"; exit 2; }
[ -x "$QEMU_BIN" ] || { echo "not executable: $QEMU_BIN"; exit 2; }

QEMU_ARGS=(-M leon3_generic -m 64M -no-reboot -display none -monitor none
           "${STIX_SLOT_QEMU_ARGS[@]}"
           -serial "file:$RUN_DIR/uart.pfm.raw"
           -trace "enable=stixctrl_watchdog_*,file=$RUN_DIR/wdg.trace")
[ "$NEGATIVE" -eq 0 ] || QEMU_ARGS+=(-global grlib-stixflash.latency=off)
QEMU_ARGS+=(-kernel "$ELF")
DRIVE_ARGS=(--port "$TMTC_PORT" --seconds "$DURATION")
[ "$NOMINAL" -eq 1 ] || DRIVE_ARGS+=(--no-nominal)
if [ "$NEGATIVE" -eq 1 ]; then
    QEMU_TIMEOUT="$DURATION"
else
    QEMU_TIMEOUT="$((DURATION + 30))"
fi

if [ "$DRY_RUN" -eq 1 ]; then
    echo "DRY RUN — pfm-boot-smoke ($FLAVOUR, negative=$NEGATIVE," \
        "nominal=$NOMINAL), run dir $RUN_DIR:"
    echo "(cd $RUN_DIR && timeout $QEMU_TIMEOUT $QEMU_BIN ${QEMU_ARGS[*]}" \
        "> qemu-stdout.pfm.log 2>&1)"
    if [ "$NEGATIVE" -eq 0 ]; then
        printf 'python3 -u %s %s' "$DRIVE" "${DRIVE_ARGS[*]}"
        printf ' > pfm.drive.log\n'
    fi
    exit 0
fi

if ss -ltn | grep -q ":${TMTC_PORT} "; then
    echo "TMTC port ${TMTC_PORT} busy — another QEMU instance running?${STIX_SLOT_TAG}"
    exit 2
fi

V="PFM-BOOT-SMOKE"
[ "$NEGATIVE" -eq 0 ] || V="$V (negative)"
mkdir -p "$RUN_DIR"
cd "$RUN_DIR"
rm -f flash.bin uart.pfm.raw uart.pfm.txt wdg.trace pfm.drive.log \
      qemu-stdout.pfm.log
truncate -s 17716740096 flash.bin

DRIVE_PID=""
QEMU_PID=""
cleanup() {
    if [ -n "$QEMU_PID" ]; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if [ -n "$DRIVE_PID" ]; then
        kill "$DRIVE_PID" 2>/dev/null || true
        wait "$DRIVE_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

fail() {
    echo "$V: FAIL ($FLAVOUR: $1)${STIX_SLOT_TAG}"
    exit 1
}

uart_text() {
    # the release ELF (and the fake QEMU of the tests) writes no UART file
    if [ -f uart.pfm.raw ]; then
        tr -d '\000' < uart.pfm.raw > uart.pfm.txt
    else
        : > uart.pfm.txt
    fi
}

if [ "$NEGATIVE" -eq 1 ]; then
    set +e
    timeout "$QEMU_TIMEOUT" "$QEMU_BIN" "${QEMU_ARGS[@]}" \
        > qemu-stdout.pfm.log 2>&1
    QEMU_RC=$?
    set -e
    uart_text
    [ "$QEMU_RC" -ne 124 ] ||
        fail "QEMU still ran at ${DURATION}s: no watchdog reset with latency off"
    grep -q 'stixctrl_watchdog_reset.*count expired' wdg.trace ||
        fail "QEMU exited (rc $QEMU_RC) without a watchdog 'count expired'"
    if [ "$FLAVOUR" = debug ]; then
        grep -q 'init: SAFE mode' uart.pfm.txt ||
            fail "the UART never reached 'init: SAFE mode'"
        ! grep -q 'delete init' uart.pfm.txt ||
            fail "'delete init' with latency off: the scan did not starve"
    fi
    echo "watchdog: count expired; QEMU exited by itself (rc $QEMU_RC)"
    cd /
    [ "${STIX_KEEP_RUN:-0}" = 1 ] || rm -rf -- "$RUN_DIR"
    echo "$V: PASS ($FLAVOUR)${STIX_SLOT_TAG}"
    exit 0
fi

# The driver starts first and retries the connect until QEMU binds.
timeout "$((DURATION + 30))" python3 -u "$DRIVE" "${DRIVE_ARGS[@]}" \
    > pfm.drive.log 2>&1 &
DRIVE_PID=$!
timeout "$QEMU_TIMEOUT" "$QEMU_BIN" "${QEMU_ARGS[@]}" \
    > qemu-stdout.pfm.log 2>&1 &
QEMU_PID=$!

set +e
wait "$DRIVE_PID"
DRIVE_RC=$?
DRIVE_PID=""
kill -0 "$QEMU_PID" 2>/dev/null
QEMU_ALIVE=$?
kill "$QEMU_PID" 2>/dev/null
wait "$QEMU_PID" 2>/dev/null
QEMU_RC=$?
QEMU_PID=""
set -e
uart_text

FS_AT="$(grep -m1 -o 'PFM 0x0032 at +[0-9.]* s' pfm.drive.log || true)"
echo "${FS_AT:-PFM 0x0032 not seen} ($FLAVOUR)"
if grep -q 'stixctrl_watchdog_reset' wdg.trace 2>/dev/null; then
    grep -m1 'stixctrl_watchdog_reset' wdg.trace
    fail "the watchdog reset the IDPU"
fi
[ "$QEMU_ALIVE" -eq 0 ] ||
    fail "QEMU exited early (rc $QEMU_RC; see qemu-stdout.pfm.log)"
grep -q 'stixctrl_watchdog_cmd.* cmd=1 ' wdg.trace 2>/dev/null ||
    fail "no watchdog cmd=1 (RELOAD) in wdg.trace: the trace is not live"
if [ "$DRIVE_RC" -ne 0 ]; then
    tail -5 pfm.drive.log
    fail "$(grep -m1 -o 'DRIVE: FAIL: .*' pfm.drive.log | cut -d' ' -f3- ||
        echo "driver rc=$DRIVE_RC")"
fi

if [ "$FLAVOUR" = debug ]; then
    if grep -Eq 'Fatal error|Read timeout|Write timeout|Erase timeout' \
            uart.pfm.txt; then
        grep -Em1 'Fatal error|Read timeout|Write timeout|Erase timeout' \
            uart.pfm.txt
        fail "a forbidden UART line"
    fi
    # the key lines, in this order: SAFE, LUT type=1..4, delete init
    ORDER="$(awk '
        /init: SAFE mode/ && !s { s = NR }
        s && /could not find LUT \(type=[1-4],/ {
            match($0, /type=[1-4]/); t = substr($0, RSTART + 5, 1)
            if (!(t in lut)) { lut[t] = NR; n++ }
        }
        s && n == 4 && /Main +delete init/ && !d { d = NR; dl = $1 }
        END { print (s ? "S" : "-") n + 0 (d ? " " dl : " -") }
    ' uart.pfm.txt)"
    case "$ORDER" in
        "S4 "[0-9]*) echo "UART: SAFE, 4 LUT lines, delete init at ${ORDER#S4 }" ;;
        *) fail "UART key lines missing or out of order ($ORDER)" ;;
    esac
fi

echo "watchdog: $(grep -c 'stixctrl_watchdog_cmd.* cmd=1 ' wdg.trace)" \
    "RELOADs, no reset; QEMU alive at the deadline"
cd /
[ "${STIX_KEEP_RUN:-0}" = 1 ] || rm -rf -- "$RUN_DIR"
echo "$V: PASS ($FLAVOUR, ${FS_AT#PFM })${STIX_SLOT_TAG}"
exit 0
