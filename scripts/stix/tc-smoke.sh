#!/usr/bin/env bash
# TC smoke: the end-to-end TC gate (P2-6/P2-7). Boots the STIX model
# with the ASW ELF exactly like boot-smoke.sh (same run dir, same port,
# same one-QEMU guard), sends TC(17,1) seq 0 at ~20 s and expects,
# within STIX_TC_WINDOW (10 s):
#   TM(1,1)  on APID 0x5A1 with the application-data prefix 1DACC000
#   TM(17,2) on APID 0x5A7
#   TM(1,7)  on APID 0x5A1 with the application-data prefix 1DACC000
# then sends a TC(17,1) whose CRC trailer is corrupted and expects
# TM(1,2) keyed on FID 2, then TC(9,129) (coarse 0x10000000) and
# expects TM(1,7) for it within 3 s plus a later heartbeat whose CUC
# coarse (SP[10:14]) shows the new SCET 0x1000000x. Expectations key
# on APID and PUS type/subtype (correction 5), never on the 0x49 SpW
# header byte (findings F1).
#
# QEMU is stopped at the end; flash.bin is left in place.
#
# Usage: tc-smoke.sh <qemu-binary> [--dry-run]
#   STIX_ASW_ELF, STIX_RUN_DIR            as in boot-smoke.sh
#   STIX_TC_SEND_AT (20), STIX_TC_WINDOW (10), STIX_TC_SECONDS (60)
#   STIX_TMTC_PROBE                       the TM probe script (tests)
#   STIX_PORT_BASE                        the slot (scripts/stix/lib/slot.sh);
#                                         slot mode boots a private flash copy
#   --dry-run prints the commands without booting anything.
#
# Exit codes: 0 pass (or SKIP / dry-run), 1 a TM expectation failed,
#             2 environment problem.
set -euo pipefail

QEMU_BIN_ARG="${1:?usage: tc-smoke.sh <qemu-binary> [--dry-run]}"
DRY_RUN="${2:-}"
[ -z "$DRY_RUN" ] || [ "$DRY_RUN" = "--dry-run" ] || {
    echo "unknown argument: $DRY_RUN"; exit 2; }

. "$(dirname "${BASH_SOURCE[0]}")/lib/paths.sh"
ASW_ELF="${STIX_ASW_ELF:-$STIX_DEFAULT_ELF}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_DIR="${STIX_RUN_DIR:-$STIX_UPREV_DIR}"
SEND_AT="${STIX_TC_SEND_AT:-20}"
WINDOW="${STIX_TC_WINDOW:-10}"
DURATION="${STIX_TC_SECONDS:-60}"
PROBE="${STIX_TMTC_PROBE:-$REPO_ROOT/scripts/stix/tmtc-probe.py}"
# Slot mode (STIX_PORT_BASE, scripts/stix/lib/slot.sh): the slot's port
# and run dir <dir>-s<k>; unset = legacy, 20000+uid and the dir as is.
. "$REPO_ROOT/scripts/stix/lib/slot.sh"
# dry-run, check-guard and the ELF-absent SKIP boot nothing: they peek at
# the slot (no lock, no wait, no disk guard)
if [ "$DRY_RUN" = "--dry-run" ] || [ ! -f "$ASW_ELF" ]; then
    stix_slot_setup peek
else
    stix_slot_setup
fi
RUN_DIR="$(stix_slot_dir "$RUN_DIR")"
stix_run_dir_guard "$RUN_DIR"

QEMU_BIN="$(realpath -e "$QEMU_BIN_ARG")" || {
    echo "qemu binary not found: $QEMU_BIN_ARG"; exit 2; }
[ -x "$QEMU_BIN" ] || { echo "not executable: $QEMU_BIN"; exit 2; }

# TC(17,1) seq 0 (FSW-RX §6) and seq 1 with the CRC trailer flipped:
# the FSW must answer the first with the three TM reports and the
# second with TM(1,2) keyed on TCPacketID+SeqCtl+FID 2
# (1DACC0010002, FSW TCBuffer.c:336-342 — review M2).
TC0="40 02 00 00 1D AC C0 00 00 05 19 11 01 00 D7 7E"
TC1BAD="40 02 00 00 1D AC C0 01 00 05 19 11 01 00 12 34"

# TC(9,129) "Accept Time Synchronisation" (FSW-RX §6): coarse
# 0x10000000, fine 0. seq 2 is free (TC0 = seq 0, the corrupt one =
# seq 1), so the bytes are exactly the recon's, CRC FA 63 included.
# The FSW answers TM(1,7) with TCPacketID+SeqCtl 1DACC002 (the @3
# tightens that expectation's window to 3 s), and its heartbeats then
# carry the new SCET. The FSW's TM carries the SCET as CUC time in the
# PUS TM data field header: coarse = SP[10:14] big-endian (unit bytes
# 14:18), fine = SP[14:16] — real heartbeats tick coarse +1 per second.
# --expect-cuc hex-prefix-matches the coarse: "1000000" = 0x1000000x,
# a few seconds after the set.
TC9="40 02 00 00 1D AC C0 02 00 0B 19 09 81 00 10 00 00 00 00 00 FA 63"

# Integer seconds only: the schedule arithmetic below needs it.
for v in "$SEND_AT" "$WINDOW" "$DURATION"; do
    case "$v" in
        '' | *[!0-9]*)
            echo "STIX_TC_SEND_AT / STIX_TC_WINDOW / STIX_TC_SECONDS must be whole seconds (got: $v)"
            exit 2
            ;;
    esac
done
SEND2=$((SEND_AT + WINDOW + 2))
SEND3=$((SEND2 + WINDOW + 2))
# the whole schedule must fit the probe's own --seconds window
if [ "$DURATION" -lt "$((SEND3 + WINDOW + 2))" ]; then
    echo "STIX_TC_SECONDS ($DURATION) too short for send3 at ${SEND3}s"
    echo "raising STIX_TC_SECONDS to $((SEND3 + WINDOW + 5))"
    DURATION=$((SEND3 + WINDOW + 5))
fi

PROBE_ARGS=(
    --port "$TMTC_PORT" --seconds "$DURATION" --window "$WINDOW"
    --send-at "$SEND_AT" --send-hex "$TC0"
    --expect "0x5A1:1:1:1DACC000"
    --expect "0x5A7:17:2"
    --expect "0x5A1:1:7:1DACC000"
    --send-at "$SEND2" --send-hex "$TC1BAD"
    --expect "0x5A1:1:2:1DACC0010002"
    --send-at "$SEND3" --send-hex "$TC9"
    --expect "0x5A1:1:7:1DACC002@3"
    --expect-cuc "0x5E5:1000000"
)

if [ "$DRY_RUN" = "--dry-run" ]; then
    echo "DRY RUN — the commands tc-smoke would run:"
    echo "(cd $RUN_DIR && timeout $((DURATION + 20)) $QEMU_BIN \
-M leon3_generic -m 64M -no-reboot -display none -monitor none\
${STIX_SLOT_QEMU_ARGS[*]:+ ${STIX_SLOT_QEMU_ARGS[*]}} \
-serial file:$RUN_DIR/uart.tc.raw -kernel $ASW_ELF \
> qemu-stdout.tc.log 2>&1)"
    printf 'python3 -u %s ' "$PROBE"
    printf '%q ' "${PROBE_ARGS[@]}"
    printf '\n'
    exit 0
fi

[ -f "$ASW_ELF" ] || { echo "SKIP: ASW ELF absent ($ASW_ELF); set STIX_ASW_ELF to enable"; exit 0; }

if ss -ltn | grep -q ":${TMTC_PORT} "; then
    echo "TMTC port ${TMTC_PORT} busy — another QEMU instance running?${STIX_SLOT_TAG}"
    exit 2
fi

mkdir -p "$RUN_DIR"
cd "$RUN_DIR"
stix_flash_private
[ -f flash.bin ] || { echo "flash.bin missing — run capture-golden.sh first"; exit 2; }
rm -f uart.tc.raw tc.smoke.probe.log qemu-stdout.tc.log

PROBE_PID=""
QEMU_PID=""
cleanup() {
    # `|| true`: under set -e a failed kill (process already gone) would
    # otherwise become the script's exit status
    if [ -n "$QEMU_PID" ]; then kill "$QEMU_PID" 2>/dev/null || true; fi
    if [ -n "$PROBE_PID" ]; then kill "$PROBE_PID" 2>/dev/null || true; fi
}
trap cleanup EXIT

# The probe starts first: it retries connecting until QEMU binds the
# TMTC port, then stays on the connection for the whole run.
timeout "$((DURATION + 20))" python3 -u "$PROBE" "${PROBE_ARGS[@]}" \
    > tc.smoke.probe.log 2>&1 &
PROBE_PID=$!

timeout "$((DURATION + 20))" "$QEMU_BIN" \
    -M leon3_generic -m 64M -no-reboot \
    -display none -monitor none \
    "${STIX_SLOT_QEMU_ARGS[@]}" \
    -serial file:"$RUN_DIR/uart.tc.raw" \
    -kernel "$ASW_ELF" > qemu-stdout.tc.log 2>&1 &
QEMU_PID=$!

# The probe exits as soon as every expectation matched (or failed).
set +e
wait "$PROBE_PID"
PROBE_RC=$?
# m1: is QEMU still alive at that point? (checked before this script
# kills it, so an early death is distinguishable)
kill -0 "$QEMU_PID" 2>/dev/null
QEMU_ALIVE=$?
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_RC=$?
QEMU_PID=""
set -e

# m1: exactly one verdict line on every path — PASS (exit 0),
# FAIL (<which expectation>, exit 1), ERROR (environment problem,
# QEMU dying or never starting, exit 2).
if [ "$QEMU_ALIVE" -ne 0 ]; then
    echo "TC-SMOKE: ERROR (QEMU died or never started; qemu rc=$QEMU_RC)${STIX_SLOT_TAG}"
    tail -5 qemu-stdout.tc.log
    exit 2
fi
if [ "$PROBE_RC" -eq 124 ]; then
    echo "TC-SMOKE: ERROR (the probe hit its own timeout)${STIX_SLOT_TAG}"
    tail -5 tc.smoke.probe.log
    exit 2
fi
if grep -q "could not connect" tc.smoke.probe.log 2>/dev/null; then
    echo "TC-SMOKE: ERROR (the probe never connected; QEMU rc=$QEMU_RC)${STIX_SLOT_TAG}"
    tail -5 tc.smoke.probe.log
    exit 2
fi
if [ "$PROBE_RC" -eq 0 ]; then
    stix_slot_cleanup 0 flash.bin
    echo "TC-SMOKE: PASS${STIX_SLOT_TAG}"
    exit 0
fi
WHY="$(grep -m1 -o 'EXPECTATION NOT MATCHED: .*' tc.smoke.probe.log \
    | cut -d' ' -f4-)" || true
echo "TC-SMOKE: FAIL (${WHY:-probe rc=$PROBE_RC; see tc.smoke.probe.log})${STIX_SLOT_TAG}"
tail -5 tc.smoke.probe.log
exit 1
