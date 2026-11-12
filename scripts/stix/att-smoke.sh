#!/usr/bin/env bash
# ATT smoke: the attenuator-model gate (attenuator plan A-4,
# docs/stix-port/2026-11-11-attenuator-plan.md). Boots the STIX model on
# a private fresh flash with a QMP socket in the run dir and proves the
# attenuator's fault paths against the FSW. Three parts, in separate
# boots; which of them run is decided by the ELF flavour, positively:
#   AX  nm on $STIX_ASW_ELF finds a symbol containing moveStartCoarseT
#       (the AX stub's static, .555127): the gate ELF. Only the AX
#       subset runs; the PFM parts SKIP with exit 77.
#   PFM otherwise the ELF boots once and fsw-accesses is read over QMP
#       after the boot: > 0 means the FSW touches 0x5C (the PFM SAFE
#       entry, OpModeMgmt.c:174) and the PFM parts run; 0 accesses on a
#       non-AX ELF exit 2 (environment: a stripped or HW_VERSION_FSW_
#       IDPU ELF must not pass as PFM).
# Parts (times in seconds from the TMTC connect; every payload below is
# [L] until a live probe run confirms it — the plan's rule for A-4):
#
# AX subset (the gate ELF; the att -> presented -> comparator wiring):
#   connect: TM(5,1) 0x14A3 (FDIR protection enabled) within 30 s [L]
#   @35  qom-set attenuator.current.offset 800: TM(5,3) 0x9434
#        06 0334 02EF (ATT_C id 6, value 820, threshold 751) within 5 s
#        [L]; forbid any 0021 mode event from the set (SAFE already, no
#        mode change)
#   @45  qom-get fsw-accesses --expect-return 0 [L]: the AX ELF never
#        touches 0x5C
#   @50  fault-clear attenuator.current.offset; hold to 60 s
#   The PFM sequence and the silence run SKIP (exit 77) and the record
#   line says "PASS (AX subset), SKIP (PFM sequence), SKIP (silence
#   run)".
#
# PFM sequence (a PFM ELF, Q1; CONF, LV on, FDIR on):
#   connect: 0x14A3 within 30 s [L]; forbid TM(1,2), TM(1,8) and
#            TM(5,3) 0x942B for the whole run (the timeout-finishes
#            default: a TIMEOUT ends the move silently, correction 3's
#            two TM(1,7) are never evidence of a move end)
#   @35  TC(236,1) 04 00 -> CONF (nominal-smoke's bytes): 0021 02 04 [L]
#   @40  TC(236,4) 05 04 (PSU/LVPS: LV on)
#   @45  qom-get truth.position = out, presented.cmdsta = 0x500, and
#        fsw-accesses > 0 (the > is checked by this script; the probe's
#        --expect-return only equals) — all [L]
#   @46  C = TC(236,5) 04 01 (close M1 -> IN); @50 truth = in [L]
#   @52  O = TC(236,4) 04 01 (open M1 -> OUT); @56 truth = out [L]
#   @58  qom-set attenuator.motor1.dead true; @60 C: TM(5,2) 0x5436
#        04 01 00 01 00 within 6 s [L]; @66 truth = out [L]; @67 clear
#   @70  C: TM(5,1) 0x1423 0000 0002 (the manual re-enable) [L];
#        @75 truth = in [L]
#   @77  qom-set sensor_ab.stuck_open + sensor_bc.stuck_closed; @79 O:
#        0x5436 01 00 01 01 00 within 6 s [L]; @85 fault-clear all
#   @87  qom-set current.offset 800: 0x9434 06 0334 02EF within 5 s
#        [L], then 0021 04 02 (CONF -> SAFE; the context goes to this
#        run's private flash) [L]
#   hold to 100 s.
#   The plan's "Blocking change from the flash-latency plan" voids these
#   fixed offsets for a real PFM ELF (FS-ready at ~26 s or later): they
#   run only with a PFM ELF (none exists yet), the first PFM boot is a
#   recon run, and the schedule is to be re-anchored on 0x0032/0x142E
#   then (pfm-boot-drive.py is the in-tree pattern for that).
#
# Silence run (a PFM ELF; a second, short boot — the single stuck
# switch is FSW-silent, correction 2, so the evidence is the SID2
# bits): fresh boot, CONF @30 [L], LV on @35, then
#   @40  qom-set sensor_bc.stuck_closed true
#   @42  C at OUT: the move ends one 10 ms debounce off OUT (Doc vs
#        code 12), the correlation check passes ((1,1) is not the
#        opposite end), so 0x5436 must NOT fire — forbidden for the
#        whole run
#   @45  truth.position = between and truth.position-ppm = 10000 (the
#        plan's outcome table says "out"; the model's debounce drive
#        puts the truth 10 ms of the default 1000 ms travel off OUT)
#        and presented.sensor-bc = true — all [L]
#   the SID2 TM(3,25) on 0x5A4 (every 64 s, mode-independent,
#        HKReport.c:126-139) is dumped whole (--dump-apid) and
#        att-sid2-check.py --expect-bc-closed requires status4 bit 13
#        (BC) set and bit 14 (AB) clear in the last packet after the
#        move [L]: the plan's original "both bits set" is the outcome
#        table's approximation; the model as built releases AB one
#        debounce off OUT.
#
# --negative, per part, fails exactly as planned:
#   AX subset: the @35 set is skipped (the inactive value 0), so the
#        0x9434 expectation fails and nothing else does.
#   PFM: the motor1.dead set is skipped; the run must fail on the @66
#        truth read (truth = in, want out) with the @60 0x5436 and @70
#        0x1423 expectations unmatched. The probe fails the run at the
#        first expired expectation (the @66 read), so the @70 group is
#        never sent — the plan's three failing items collapse to the
#        first; the @60/@70 expectations are asserted unmatched.
#   silence: sensor_ab.stuck_open is set together with
#        sensor_bc.stuck_closed and O is issued at OUT, so 0x5436
#        01 00 01 01 00 fires and the whole-run forbid fails the run —
#        proof the forbid bites.
#
# Flash: slot mode refreshes ./flash.bin as a private copy of the master
# (stix_flash_private, as boot-smoke.sh); legacy mode truncates a fresh
# empty sparse flash (the fault-smoke pattern), because att-smoke's own
# run dir is never the uprev dir and the master is never booted in
# place. The shared flashes are never touched (the run-dir guard below
# refuses them and their parents, strict in both modes).
#
# In legacy mode (STIX_PORT_BASE unset) this script, like tc-smoke.sh,
# does NOT take the /tmp/stix-qtest-21000 flock (slot 0) itself; the
# caller must hold it. With STIX_PORT_BASE=auto or a slot number it
# takes its own slot (scripts/stix/lib/slot.sh) and needs no outside
# lock; each part gets <run dir>-<part>[-neg]-s<k>.
#
# Usage: att-smoke.sh <qemu-binary> [--negative] [--dry-run]
#                     [--check-guard]
#   STIX_ASW_ELF              as in tc-smoke.sh (the flavour ELF)
#   STIX_ATT_RUN_DIR          run-dir base (default /tmp/stix-att-run);
#                             each part appends -<part>[-neg] and slot
#                             mode -s<k>. Anything at or above the
#                             shared flash dirs (or /) is refused.
#   STIX_ATT_AX_SECONDS       the AX subset's probe --seconds (default
#                             60: the plan's hold-to-60)
#   STIX_ATT_PFM_SECONDS      the PFM sequence's --seconds (default 100)
#   STIX_ATT_SILENCE_SECONDS  the silence run's --seconds (default 120:
#                             a SID2 can land 64 s after the move)
#   STIX_ATT_FLAVOUR_SECONDS  the flavour boot's --seconds (default 45)
#   --dry-run prints the QEMU and probe command lines, boots nothing.
#   --check-guard only runs the run-dir guards and exits 0/2.
#
# Exit codes: 0 pass (or dry-run / check-guard ok), 77 SKIP (no ELF; or
#             the PFM parts on an AX ELF, with the record line), 1 a
#             failed expectation, a forbidden TM, a return mismatch, a
#             QMP error, a closed connection or a failed SID2 check (in
#             --negative: any other failure), 2 environment problem.
set -euo pipefail

QEMU_BIN_ARG="${1:?usage: att-smoke.sh <qemu-binary> \
[--negative] [--dry-run] [--check-guard]}"
NEGATIVE=0
DRY_RUN=0
CHECK_GUARD=0
for a in "${@:2}"; do
    case "$a" in
        --negative)
            [ "$NEGATIVE" -eq 0 ] || { echo "--negative given twice"; exit 2; }
            NEGATIVE=1 ;;
        --dry-run)
            [ "$DRY_RUN" -eq 0 ] || { echo "--dry-run given twice"; exit 2; }
            DRY_RUN=1 ;;
        --check-guard)
            [ "$CHECK_GUARD" -eq 0 ] || {
                echo "--check-guard given twice"; exit 2; }
            CHECK_GUARD=1 ;;
        *) echo "unknown argument: $a"; exit 2 ;;
    esac
done

. "$(dirname "${BASH_SOURCE[0]}")/lib/paths.sh"
ASW_ELF="${STIX_ASW_ELF:-$STIX_DEFAULT_ELF}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BASE_DIR="${STIX_ATT_RUN_DIR:-/tmp/stix-att-run}"
PROBE="$REPO_ROOT/scripts/stix/tmtc-probe.py"
SID2CHECK="$REPO_ROOT/scripts/stix/att-sid2-check.py"
ATT_PATH="/machine/stixctrl/attenuator"
# Slot mode (STIX_PORT_BASE, scripts/stix/lib/slot.sh): the slot's port
# and run dir <dir>-s<k>; unset = legacy, 20000+uid and the dir as is.
. "$REPO_ROOT/scripts/stix/lib/slot.sh"
# dry-run, check-guard and the ELF-absent SKIP boot nothing: they peek at
# the slot (no lock, no wait, no disk guard)
if [ "$DRY_RUN" -eq 1 ] || [ "$CHECK_GUARD" -eq 1 ] ||
        [ ! -f "$ASW_ELF" ]; then
    stix_slot_setup peek
else
    stix_slot_setup
fi

BASE_DIR="$(realpath -m -- "$BASE_DIR")"
SHARED_UPREV="$(realpath -m -- "$STIX_UPREV_DIR")"
SHARED_OLD="$(realpath -m -- /tmp/stix-uprev-run)"
SHARED_BASE="$(realpath -m -- /tmp/stix-baseline-run)"
guard_dir() {  # dir: refuse the shared flash dirs, their parents, and /
    local d="$1" shared
    d="$(realpath -m -- "$d")"
    if [ "$d" = "/" ]; then
        echo "refusing run dir $1 (it is the root directory)"
        exit 2
    fi
    for shared in "$SHARED_UPREV" "$SHARED_OLD" "$SHARED_BASE"; do
        case "$d" in
            "$shared" | "$shared"/*)
                echo "refusing run dir $1 (it is $shared or inside it;" \
                    "the shared flashes must stay untouched)"
                exit 2 ;;
        esac
        case "$shared" in
            "$d"/*)
                echo "refusing run dir $1 (it is a parent of the shared" \
                    "flash dir $shared)"
                exit 2 ;;
        esac
    done
}
# every part's dir, guarded: the parts boot in their own dirs so one
# part's evidence never overwrites another's
for part in fm ax ax-neg pfm pfm-neg silence silence-neg; do
    guard_dir "$BASE_DIR-$part"
done

if [ "$CHECK_GUARD" -eq 1 ]; then
    echo "GUARD OK: run dirs under $BASE_DIR (canonical," \
        "<part>[-neg][, slot mode -s<k>])"
    exit 0
fi

QEMU_BIN="$(realpath -e "$QEMU_BIN_ARG")" || {
    echo "qemu binary not found: $QEMU_BIN_ARG"; exit 2; }
[ -x "$QEMU_BIN" ] || { echo "not executable: $QEMU_BIN"; exit 2; }

# The flavour, decided positively (A-4): the AX stub's static means AX;
# anything else goes through the runtime fsw-accesses boot below.
AX_SYMBOL=""
if [ -f "$ASW_ELF" ]; then
    AX_SYMBOL="$(nm "$ASW_ELF" 2>/dev/null | grep -m1 moveStartCoarseT ||
        true)"
fi
FLAVOUR=runtime
if [ -n "$AX_SYMBOL" ]; then
    FLAVOUR=ax
    echo "flavour: AX (nm: $AX_SYMBOL)"
elif [ -f "$ASW_ELF" ]; then
    echo "flavour: runtime (no moveStartCoarseT symbol; reading" \
        "fsw-accesses after a boot)"
fi

[ -f "$ASW_ELF" ] || {
    echo "SKIP: ASW ELF absent ($ASW_ELF); set STIX_ASW_ELF to enable"
    exit 77
}

# TC units: SpW header 40 02 00 00 + CCSDS on APID 0x5AC + CRC-16/
# CCITT-FALSE over the packet (poly 0x1021, init 0xFFFF — the
# tcbuild.py / fault-smoke.sh routine), built here because the
# TC(236,4/5) units are new; one hand-typed CRC byte would fail the
# FSW's check silently. TC_CONF is pinned against nominal-smoke.sh's
# known bytes, which the FSW accepts live.
command -v python3 > /dev/null || { echo "python3 is required"; exit 2; }
eval "$(python3 <<'PYEOF'
def crc16(sp):
    crc = 0xFFFF
    for b in sp:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc

def unit(seq, svc, sub, app):
    # DFH: PUS v1, ack 9 (acceptance + completion), service, subtype,
    # source 0; length = DFH + CRC + app - 1
    sp = bytes([0x1D, 0xAC, 0xC0 | (seq >> 8), seq & 0xFF,
                0, len(app) + 5, 0x19, svc, sub, 0]) + bytes(app)
    c = crc16(sp)
    return "40 02 00 00 " + " ".join("%02X" % b for b in sp) + \
        " %02X %02X" % (c >> 8, c & 0xFF)

# seq 1..7: the TM(1,1) acks 1DACC001..7 are distinct
print('TC_CONF="%s"' % unit(1, 0xEC, 1, [0x04, 0x00]))   # mode 4 = CONF
print('TC_LV="%s"'   % unit(2, 0xEC, 4, [0x05, 0x04]))   # PSU, LVPS: LV on
print('TC_C1="%s"'   % unit(3, 0xEC, 5, [0x04, 0x01]))   # close M1
print('TC_O1="%s"'   % unit(4, 0xEC, 4, [0x04, 0x01]))   # open M1
print('TC_C2="%s"'   % unit(5, 0xEC, 5, [0x04, 0x01]))   # close M1
print('TC_C3="%s"'   % unit(6, 0xEC, 5, [0x04, 0x01]))   # close M1
print('TC_O2="%s"'   % unit(7, 0xEC, 4, [0x04, 0x01]))   # open M1
PYEOF
)"
[ "$TC_CONF" = "40 02 00 00 1D AC C0 01 00 07 19 EC 01 00 04 00 4A C1" ] || {
    echo "att-smoke: the TC builder does not reproduce nominal-smoke's" \
        "TC_CONF ($TC_CONF)"
    exit 2
}

qom_set() {  # property json-value on the attenuator object
    printf '{"execute": "qom-set", "arguments": {"path": "%s", "property": "%s", "value": %s}}' \
        "$ATT_PATH" "$1" "$2"
}
qom_get() {  # property on the attenuator object
    printf '{"execute": "qom-get", "arguments": {"path": "%s", "property": "%s"}}' \
        "$ATT_PATH" "$1"
}
fault_clear() {  # one fault id, or "all" (A-3's verb)
    printf '{"execute": "qom-set", "arguments": {"path": "%s", "property": "fault-clear", "value": "%s"}}' \
        "$ATT_PATH" "$1"
}

AX_SECS="${STIX_ATT_AX_SECONDS:-60}"
PFM_SECS="${STIX_ATT_PFM_SECONDS:-100}"
SIL_SECS="${STIX_ATT_SILENCE_SECONDS:-120}"
FM_SECS="${STIX_ATT_FLAVOUR_SECONDS:-45}"

ax_args() {  # the AX subset; the negative skips the set (value 0)
    local set_value=800
    [ "$NEGATIVE" -eq 1 ] && set_value=0
    PROBE_ARGS=(
        --port "$TMTC_PORT" --seconds "$AX_SECS" --window 20
        --qmp "$QMP_SOCK"
        --expect "0x5A7:5:1:14a3@30"
        --send-at 35 --qmp-cmd "$(qom_set attenuator.current.offset \
            "$set_value")"
        --expect "0x5A7:5:3:943406033402ef@5"
        --forbid "0x5A7:5:1:0021"
        --send-at 45 --qmp-cmd "$(qom_get fsw-accesses)"
        --expect-return 0
        --send-at 50 --qmp-cmd "$(fault_clear attenuator.current.offset)"
        --hold
    )
}

fm_args() {  # the flavour boot: read fsw-accesses after the SAFE entry
    PROBE_ARGS=(
        --port "$TMTC_PORT" --seconds "$FM_SECS" --window 20
        --qmp "$QMP_SOCK"
        --send-at 35 --qmp-cmd "$(qom_get fsw-accesses)"
    )
}

pfm_args() {  # the PFM sequence; the negative skips the motor1.dead set
    PROBE_ARGS=(
        --port "$TMTC_PORT" --seconds "$PFM_SECS" --window 20
        --qmp "$QMP_SOCK"
        --expect "0x5A7:5:1:14a3@30"
        --forbid "0x5A1:1:2"
        --forbid "0x5A1:1:8"
        --forbid "0x5A7:5:3:942b"
        --send-at 35 --send-hex "$TC_CONF"
        --expect "0x5A7:5:1:00210204@10"
        --send-at 40 --send-hex "$TC_LV"
        --send-at 45 --qmp-cmd "$(qom_get truth.position)"
        --expect-return out
        --send-at 45 --qmp-cmd "$(qom_get presented.cmdsta)"
        --expect-return 1280
        --send-at 45 --qmp-cmd "$(qom_get fsw-accesses)"
        --send-at 46 --send-hex "$TC_C1"
        --send-at 50 --qmp-cmd "$(qom_get truth.position)"
        --expect-return in
        --send-at 52 --send-hex "$TC_O1"
        --send-at 56 --qmp-cmd "$(qom_get truth.position)"
        --expect-return out
    )
    if [ "$NEGATIVE" -eq 0 ]; then
        PROBE_ARGS+=(
            --send-at 58 --qmp-cmd "$(qom_set attenuator.motor1.dead true)"
        )
    fi
    PROBE_ARGS+=(
        --send-at 60 --send-hex "$TC_C2"
        --expect "0x5A7:5:2:54360401000100@6"
        --send-at 66 --qmp-cmd "$(qom_get truth.position)"
        --expect-return out
    )
    if [ "$NEGATIVE" -eq 0 ]; then
        PROBE_ARGS+=(
            --send-at 67 --qmp-cmd "$(fault_clear attenuator.motor1.dead)"
        )
    fi
    PROBE_ARGS+=(
        --send-at 70 --send-hex "$TC_C3"
        --expect "0x5A7:5:1:142300000002@6"
        --send-at 75 --qmp-cmd "$(qom_get truth.position)"
        --expect-return in
        --send-at 77 --qmp-cmd "$(qom_set attenuator.sensor_ab.stuck_open \
            true)"
        --send-at 77 --qmp-cmd "$(qom_set attenuator.sensor_bc.stuck_closed \
            true)"
        --send-at 79 --send-hex "$TC_O2"
        --expect "0x5A7:5:2:54360100010100@6"
        --send-at 85 --qmp-cmd "$(fault_clear all)"
        --send-at 87 --qmp-cmd "$(qom_set attenuator.current.offset 800)"
        --expect "0x5A7:5:3:943406033402ef@5"
        --expect "0x5A7:5:1:00210402@15"
        --hold
    )
}

silence_args() {  # the stuck-sensor silence case; the negative sets both
    PROBE_ARGS=(
        --port "$TMTC_PORT" --seconds "$SIL_SECS" --window 20
        --qmp "$QMP_SOCK"
        --dump-apid "0x5A4:$PART_DIR/sid2.dump.hex"
        --expect "0x5A7:5:1:14a3@30"
        --forbid "0x5A7:5:2:5436"
        --send-at 30 --send-hex "$TC_CONF"
        --expect "0x5A7:5:1:00210204@10"
        --send-at 35 --send-hex "$TC_LV"
    )
    if [ "$NEGATIVE" -eq 1 ]; then
        # both stuck sensors and O at OUT: 0x5436 01 00 01 01 00 fires
        # and the whole-run forbid must fail the run; the truth reads
        # are dropped (the run ends at the forbid, before +45 s)
        PROBE_ARGS+=(
            --send-at 40 --qmp-cmd "$(qom_set \
                attenuator.sensor_ab.stuck_open true)"
            --send-at 40 --qmp-cmd "$(qom_set \
                attenuator.sensor_bc.stuck_closed true)"
            --send-at 42 --send-hex "$TC_O1"
        )
    else
        PROBE_ARGS+=(
            --send-at 40 --qmp-cmd "$(qom_set \
                attenuator.sensor_bc.stuck_closed true)"
            --send-at 42 --send-hex "$TC_C1"
            --send-at 45 --qmp-cmd "$(qom_get truth.position)"
            --expect-return between
            --send-at 45 --qmp-cmd "$(qom_get truth.position-ppm)"
            --expect-return 10000
            --send-at 45 --qmp-cmd "$(qom_get presented.sensor-bc)"
            --expect-return true
        )
    fi
    PROBE_ARGS+=(--hold)
}

PROBE_PID=""
QEMU_PID=""
cleanup() {
    # `|| true`: under set -e a failed kill (process already gone) would
    # otherwise become the script's exit status
    if [ -n "$QEMU_PID" ]; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if [ -n "$PROBE_PID" ]; then
        kill "$PROBE_PID" 2>/dev/null || true
        wait "$PROBE_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# boot_and_probe <tag> <args-fn> <secs>: one boot in <BASE_DIR>-<tag>
# (with a private fresh flash and the QMP socket; -neg appended to the
# dir under --negative, as the guard lists), the probe over it,
# then the common error paths. Leaves PROBE_RC, the probe log
# <tag>.probe.log (relative to the part dir) and QMP_SOCK set.
boot_and_probe() {
    local tag="$1" argfn="$2" secs="$3"
    local sfx=""

    [ "$NEGATIVE" -eq 1 ] && sfx=-neg
    PART_DIR="$(stix_slot_dir "$BASE_DIR-$tag$sfx")"
    guard_dir "$PART_DIR"
    mkdir -p "$PART_DIR"
    cd "$PART_DIR"
    QMP_SOCK="$PART_DIR/qmp.sock"
    if [ "${#QMP_SOCK}" -ge 108 ]; then
        echo "refusing run dir $PART_DIR (the QMP socket path is" \
            "${#QMP_SOCK} bytes; a unix socket path must stay under 108)"
        exit 2
    fi
    rm -f flash.bin "uart.$tag.raw" "$tag.probe.log" \
        "qemu-stdout.$tag.log" qmp.sock sid2.dump.hex
    if [ "$STIX_SLOT_MODE" -eq 1 ]; then
        stix_flash_private
        [ -f flash.bin ] || {
            echo "flash.bin missing — run capture-golden.sh first"
            exit 2
        }
    else
        truncate -s 17716740096 flash.bin
    fi
    "$argfn"
    PROBE_PID=""
    QEMU_PID=""
    timeout "$((secs + 30))" python3 -u "$PROBE" "${PROBE_ARGS[@]}" \
        > "$tag.probe.log" 2>&1 &
    PROBE_PID=$!
    timeout "$((secs + 30))" "$QEMU_BIN" \
        -M leon3_generic -m 64M -no-reboot \
        -display none -monitor none \
        "${STIX_SLOT_QEMU_ARGS[@]}" \
        -qmp "unix:$QMP_SOCK,server=on,wait=off" \
        -serial "file:$PART_DIR/uart.$tag.raw" \
        -kernel "$ASW_ELF" > "qemu-stdout.$tag.log" 2>&1 &
    QEMU_PID=$!
    set +e
    wait "$PROBE_PID"
    PROBE_RC=$?
    PROBE_PID=""
    kill -0 "$QEMU_PID" 2>/dev/null
    QEMU_ALIVE=$?
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null
    QEMU_RC=$?
    QEMU_PID=""
    set -e
    # On every path the log tail comes BEFORE the verdict.
    if [ "$QEMU_ALIVE" -ne 0 ]; then
        tail -5 "qemu-stdout.$tag.log"
        echo "ATT-SMOKE: ERROR (QEMU died or never started in $tag;" \
            "qemu rc=$QEMU_RC)${STIX_SLOT_TAG}"
        exit 2
    fi
    if [ "$PROBE_RC" -eq 124 ]; then
        tail -5 "$tag.probe.log"
        echo "ATT-SMOKE: ERROR (the $tag probe hit its own " \
            "timeout)${STIX_SLOT_TAG}"
        exit 2
    fi
    if [ "$PROBE_RC" -eq 2 ]; then
        tail -5 "$tag.probe.log"
        echo "ATT-SMOKE: ERROR (the $tag probe rejected its " \
            "arguments)${STIX_SLOT_TAG}"
        exit 2
    fi
    if grep -q "could not connect" "$tag.probe.log" 2>/dev/null; then
        tail -5 "$tag.probe.log"
        echo "ATT-SMOKE: ERROR (the $tag probe never connected;" \
            "QEMU rc=$QEMU_RC)${STIX_SLOT_TAG}"
        exit 2
    fi
}

why_failed() {  # probe log -> the failure reason, by kind
    local why
    why="$(grep -m1 -o 'FORBIDDEN TM: .*' "$1" | cut -d' ' -f3-)" || true
    if [ -n "$why" ]; then echo "forbidden: $why"; return; fi
    why="$(grep -m1 -o 'RETURN MISMATCH: .*' "$1" | cut -d' ' -f3-)" || true
    if [ -n "$why" ]; then echo "return mismatch: $why"; return; fi
    why="$(grep -m1 -o 'QMP ERROR: .*' "$1" | cut -d' ' -f3-)" || true
    if [ -n "$why" ]; then echo "qmp error: $why"; return; fi
    why="$(grep -o 'EXPECTATION NOT MATCHED: .*' "$1" \
        | cut -d' ' -f4- | paste -sd' ')" || true
    if [ -n "$why" ]; then echo "not matched: $why"; return; fi
    if grep -q '^CONNECTION CLOSED' "$1"; then
        echo "connection closed"; return
    fi
    echo "probe rc=$PROBE_RC; see the part's probe log"
}

fsw_accesses_of() {  # probe log -> the fsw-accesses qom-get's return
    sed -n 's/.*fsw-accesses.*-> return \([0-9][0-9]*\) at .*/\1/p' "$1" \
        | tail -1
}

dry_run_part() {  # tag secs: the part's QEMU and probe command lines
    local dir
    dir="$(stix_slot_dir "$BASE_DIR-$1")"
    printf '(cd %s && timeout %s %s -M leon3_generic -m 64M -no-reboot\
 -display none -monitor none%s -qmp unix:%s/qmp.sock,server=on,wait=off\
 -serial file:%s/uart.%s.raw -kernel %s > qemu-stdout.%s.log 2>&1)\n' \
        "$dir" "$(($2 + 30))" "$QEMU_BIN" \
        "${STIX_SLOT_QEMU_ARGS[*]:+ ${STIX_SLOT_QEMU_ARGS[*]}}" \
        "$dir" "$dir" "$1" "$ASW_ELF" "$1"
    printf 'python3 -u %s ' "$PROBE"
    printf '%q ' "${PROBE_ARGS[@]}"
    printf '\n'
}

if [ "$DRY_RUN" -eq 1 ]; then
    echo "DRY RUN — the commands att-smoke would run" \
        "(negative=$NEGATIVE, flavour=$FLAVOUR):"
    if [ "$FLAVOUR" = ax ]; then
        PART_DIR="$(stix_slot_dir "$BASE_DIR-ax")"
        QMP_SOCK="$PART_DIR/qmp.sock"
        ax_args
        dry_run_part ax "$AX_SECS"
    elif [ -f "$ASW_ELF" ]; then
        echo "(the flavour boot first; the PFM parts run only if it" \
            "reads fsw-accesses > 0)"
        PART_DIR="$(stix_slot_dir "$BASE_DIR-fm")"
        QMP_SOCK="$PART_DIR/qmp.sock"
        fm_args
        dry_run_part fm "$FM_SECS"
        PART_DIR="$(stix_slot_dir "$BASE_DIR-pfm")"
        QMP_SOCK="$PART_DIR/qmp.sock"
        pfm_args
        dry_run_part pfm "$PFM_SECS"
        PART_DIR="$(stix_slot_dir "$BASE_DIR-silence")"
        QMP_SOCK="$PART_DIR/qmp.sock"
        silence_args
        dry_run_part silence "$SIL_SECS"
    else
        echo "(no ELF: nothing to print beyond this note)"
    fi
    exit 0
fi

if ss -ltn | grep -q ":${TMTC_PORT} "; then
    echo "TMTC port ${TMTC_PORT} busy — another QEMU instance" \
        "running?${STIX_SLOT_TAG}"
    exit 2
fi

if [ "$FLAVOUR" = ax ]; then
    boot_and_probe ax ax_args "$AX_SECS"
    if [ "$NEGATIVE" -eq 1 ]; then
        # PASS only on the planned failure: exactly the 0x9434
        # expectation unmatched, the boot stage matched, nothing else
        MISSED="$(grep -o 'EXPECTATION NOT MATCHED: .*' ax.probe.log \
            | cut -d' ' -f4- | sort | paste -sd' ')" || true
        if [ "$PROBE_RC" -eq 1 ] && [ "$MISSED" = \
            "0x5A7:5:3:943406033402ef@5" ] && \
            grep -q 'EXPECT OK: 0x5A7:5:1:14a3@30' ax.probe.log && \
            ! grep -q 'FORBIDDEN TM: \|QMP ERROR: \|RETURN MISMATCH' \
                ax.probe.log; then
            tail -5 ax.probe.log
            echo "expected failure seen: not matched: $MISSED"
            stix_slot_cleanup 0 flash.bin
            echo "ATT-SMOKE (negative): PASS (AX subset), SKIP (PFM" \
                "sequence), SKIP (silence run)${STIX_SLOT_TAG}"
            exit 77
        fi
        tail -5 ax.probe.log
        if [ "$PROBE_RC" -eq 0 ]; then
            echo "ATT-SMOKE (negative): FAIL (the AX subset passed:" \
                "the gate did not detect the missing trip)${STIX_SLOT_TAG}"
        else
            echo "ATT-SMOKE (negative): FAIL (wrong failure: " \
                "$(why_failed ax.probe.log))${STIX_SLOT_TAG}"
        fi
        exit 1
    fi
    if [ "$PROBE_RC" -ne 0 ]; then
        tail -5 ax.probe.log
        echo "ATT-SMOKE: FAIL (AX subset: $(why_failed ax.probe.log)" \
            ")${STIX_SLOT_TAG}"
        exit 1
    fi
    stix_slot_cleanup 0 flash.bin
    echo "ATT-SMOKE: PASS (AX subset), SKIP (PFM sequence), SKIP" \
        "(silence run)${STIX_SLOT_TAG}"
    exit 77
fi

# runtime flavour: boot once and read fsw-accesses after the SAFE entry
boot_and_probe fm fm_args "$FM_SECS"
FA="$(fsw_accesses_of fm.probe.log)"
if [ -z "$FA" ] || [ "$FA" -eq 0 ]; then
    tail -5 fm.probe.log
    echo "ATT-SMOKE: ERROR (fsw-accesses = ${FA:-<no return>} after the" \
        "boot window on a non-AX ELF: not a PFM attenuator ELF" \
        "(stripped, HW_VERSION_FSW_IDPU, or the boot failed)" \
        ")${STIX_SLOT_TAG}"
    exit 2
fi
echo "flavour: PFM (fsw-accesses = $FA at +35 s)"

boot_and_probe pfm pfm_args "$PFM_SECS"
if [ "$NEGATIVE" -eq 1 ]; then
    # PASS only on the planned failure: the run dies on the @66 truth
    # read (in, want out); the @60 0x5436 and @70 0x1423 expectations
    # unmatched, the boot and the first four reads OK, nothing forbidden
    OKS="$(grep -c 'RETURN OK' pfm.probe.log || true)"
    MISSED="$(grep -o 'EXPECTATION NOT MATCHED: .*' pfm.probe.log \
        | cut -d' ' -f4- | sort | paste -sd' ')" || true
    if [ "$PROBE_RC" -eq 1 ] && \
        [ "$(grep -c 'RETURN MISMATCH' pfm.probe.log || true)" -eq 1 ] && \
        grep -q 'RETURN MISMATCH: want out' pfm.probe.log && \
        [ "$OKS" -eq 4 ] && [ -z "$MISSED" ] && \
        grep -q 'EXPECT OK: 0x5A7:5:1:00210204@10' pfm.probe.log && \
        ! grep -q 'FORBIDDEN TM: \|QMP ERROR: ' pfm.probe.log && \
        ! grep -q 'EXPECT OK: 0x5A7:5:2:5436' pfm.probe.log && \
        ! grep -q 'EXPECT OK: 0x5A7:5:1:1423' pfm.probe.log; then
        tail -5 pfm.probe.log
        echo "expected failure seen: $(why_failed pfm.probe.log)"
        stix_slot_cleanup 0 flash.bin
        echo "ATT-SMOKE (negative): PASS (PFM sequence)${STIX_SLOT_TAG}"
    else
        tail -5 pfm.probe.log
        if [ "$PROBE_RC" -eq 0 ]; then
            echo "ATT-SMOKE (negative): FAIL (the PFM sequence passed:" \
                "the gate did not detect the missing fault)" \
                "${STIX_SLOT_TAG}"
        else
            echo "ATT-SMOKE (negative): FAIL (wrong failure: " \
                "$(why_failed pfm.probe.log))${STIX_SLOT_TAG}"
        fi
        exit 1
    fi
else
    if [ "$PROBE_RC" -ne 0 ]; then
        tail -5 pfm.probe.log
        echo "ATT-SMOKE: FAIL (PFM sequence: " \
            "$(why_failed pfm.probe.log))${STIX_SLOT_TAG}"
        exit 1
    fi
    # the @45 fsw-accesses must be above 0; --expect-return only equals,
    # so the return is taken from the probe log here
    FA="$(fsw_accesses_of pfm.probe.log)"
    if [ -z "$FA" ] || [ "$FA" -eq 0 ]; then
        tail -5 pfm.probe.log
        echo "ATT-SMOKE: FAIL (PFM sequence: fsw-accesses = " \
            "${FA:-<no return>} at +45 s, want > 0)${STIX_SLOT_TAG}"
        exit 1
    fi
    echo "CHECK fsw-accesses: ok ($FA guest 0x5C accesses by +45 s)"
    stix_slot_cleanup 0 flash.bin
    echo "ATT-SMOKE: PASS (PFM sequence)${STIX_SLOT_TAG}"
fi

boot_and_probe silence silence_args "$SIL_SECS"
if [ "$NEGATIVE" -eq 1 ]; then
    # PASS only on the planned failure: the 0x5436 forbid bit
    if [ "$PROBE_RC" -eq 1 ] && \
        grep -q 'FORBIDDEN TM: 0x5A7:5:2:5436' silence.probe.log && \
        ! grep -q 'QMP ERROR: ' silence.probe.log; then
        tail -5 silence.probe.log
        echo "expected failure seen: $(why_failed silence.probe.log)"
        stix_slot_cleanup 0 flash.bin
        echo "ATT-SMOKE (negative): PASS (silence run)${STIX_SLOT_TAG}"
        exit 0
    fi
    tail -5 silence.probe.log
    if [ "$PROBE_RC" -eq 0 ]; then
        echo "ATT-SMOKE (negative): FAIL (the silence run passed: the" \
            "forbid did not bite)${STIX_SLOT_TAG}"
    else
        echo "ATT-SMOKE (negative): FAIL (wrong failure: " \
            "$(why_failed silence.probe.log))${STIX_SLOT_TAG}"
    fi
    exit 1
fi
if [ "$PROBE_RC" -ne 0 ]; then
    tail -5 silence.probe.log
    echo "ATT-SMOKE: FAIL (silence run: " \
        "$(why_failed silence.probe.log))${STIX_SLOT_TAG}"
    exit 1
fi
# the SID2 evidence: at least one 0x5A4 packet after the C's send (the
# probe log's lines are in order; the drain before a send prints first),
# then the checker on the last dumped packet
AFTER="$(awk '/>> sent .*at \+42s/ { seen = 1 }
    /TM apid=0x5A4/ && seen { n++ } END { print n + 0 }' silence.probe.log)"
if [ "$AFTER" -eq 0 ]; then
    tail -5 silence.probe.log
    echo "ATT-SMOKE: FAIL (silence run: no SID2 (0x5A4) packet after" \
        "the C; SID2 comes every 64 s — raise STIX_ATT_SILENCE_SECONDS" \
        ")${STIX_SLOT_TAG}"
    exit 1
fi
echo "CHECK sid2-after-move: ok ($AFTER SID2 packet(s) after the C)"
set +e
python3 "$SID2CHECK" sid2.dump.hex --expect-bc-closed > sid2.check.log 2>&1
SID2_RC=$?
set -e
grep -E '^SID2|^ATT-SID2-CHECK' sid2.check.log || true
if [ "$SID2_RC" -ne 0 ]; then
    echo "ATT-SMOKE: FAIL (silence run: the SID2 status4 check; see" \
        "sid2.check.log)${STIX_SLOT_TAG}"
    exit 1
fi
stix_slot_cleanup 0 flash.bin
echo "ATT-SMOKE: PASS (silence run)${STIX_SLOT_TAG}"
exit 0
