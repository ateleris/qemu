#!/usr/bin/env bash
# EDAC smoke: the memory-upset gate (the memory-upset plan M-6b). Boots the
# AX gate ELF on a PRIVATE flash copy (the slot library), with a QMP socket
# in the run dir, and drives the two AX-visible upset chains of the M-6
# plan over /machine/stixctrl/edac:
#
#   connect: TM(5,1) 0x0022 00000008 00000000 (the boot report, ~+15 s)
#            and TM(5,1) 0x14A3 (FDIR enabled, ~+30 s) within 60 s;
#            forbids TM(1,2)/TM(1,8) and TM(5,3) 0x9434/0x9449 for the
#            whole run (nominal readings must not trip; no TC is sent).
#   No cache step in v1: the live run (2026-11-12) confirmed the code
#        review — nothing reads the CACHE SRAM in SAFE (the QL LACC
#        reads, QLAccuDblBuffs.c:135, and the TLUT re-trim, FDIR.c:443-451,
#        are NOMINAL-gated; the HK path reads only ADC lines), so a CACHE
#        DE is never latched and SID2 status1 bits 2:0 stay 0. The cache
#        mechanism is pinned by the M-2 qtests; a NOMINAL-window cache
#        leg is a later item.
#   @110 QMP human-monitor-command "xp /4wx 0x4008337C" — the SDRAM
#        inject word's live layout check. 0x4008337C is hkLineLimits[7].
#        min (E_HK_DPU_1V5_V, HKData.h:163), read every 1 s by
#        HKMonitor_regularCheck (HKMonitor.c:446) once 0x14A3 has armed
#        the line checks (fault-smoke's @40 0x9449 chain proves the 1V5
#        check runs every second in SAFE). HKMonitor_limits_s is
#        {u16 min, u16 max, u32 count} = 8 bytes and the ELF symbol
#        hkLineLimits is at 0x40083344 (nm on the AX gate ELF), so
#        entry 7 is at 0x4008337C; the xp must show 0x079f0fff (the
#        parameter-set 1V5 min/max) as its first word, else the gate
#        fails loudly.
#   @112 QMP upset-inject "sdram,0x4008337C,0x3" — an SDRAM double-bit
#        error on the every-second-read word (a data word, never code,
#        M-3's rule). QEMU runs WITHOUT -no-reboot and with
#        -global stix-edac.edac-at-boot=all-on (the D-Uk hand-over state,
#        reapplied at every device reset), so:
#          - within ~1 s the next HKMonitor pass reads the poisoned word,
#            the EDAC overlay answers MEMTX_ERROR and the LEON3 CPU takes
#            the data-access trap 0x09 (M-3 as built);
#          - the UART prints "Unexpected trap (0x09) at address ..."
#            (WDT.c:135, stix_asw_src = the gate ELF's source tree, the
#            same lines in STIX-FSW) and "data_access_exception (0x09,
#            13) ..." (WDT.c:151), storeBadTrapInfo records the AHBSTAT
#            address in the BRAM boot record (WDT.c:86-126) and forces
#            the watchdog (WDT.c:182);
#          - the model turns the FORCE into a system reset (the FORCE
#            command, not the counter — the AX ELF's watchdog never
#            runs, accepted-diffs §10); QEMU re-boots the guest (no
#            -no-reboot — the only gate that omits it). The TMTC
#            transport survives the guest reset (grlib_stixspw.c:1429:
#            "the TMTC transport (P2-1) is kept"), so the probe's
#            connection just pauses and resumes;
#          - the second boot sends TM(5,1) 0x0022 again (~+15 s) and
#            0x14A3 again (~+30 s): both are expected within 90/60 s of
#            the inject. The payload prefix 0022 00000008 00000000 is
#            EID + selfTestDone (FSW_SUBSYSTEM_MASK) + selfTestResult
#            (HKReport.c:249-254 -> TMForm_sendBootSuccess,
#            TMForm.c:162-170; nominal-smoke's proven prefix, cut before
#            the FSW_VERSION byte).
#   hold to the end (STIX_EDAC_SECONDS, default 260). The deadline must
#        cover the inject at +112 s plus the second boot's 0x0022 window
#        (@90).
#
# The boot print differs from the golden by design in this run: with
# edac-at-boot=all-on the boot EDAC print reads E_EDAC_MODULE_SDRAM = 1
# and E_EDAC_MODULE_ROTBUF = 1 (main.c:122-125). The UART checks here
# are grep-only, no golden diff. The 0x542F SEU-report step is NOT in
# this gate (the 1000 s report period, Q4 open — the plan's M-6 bullet).
#
# --negative runs the same schedule UNINJECTED: no upset-inject at all.
# It must then miss exactly the injected-fault expectations — the probe
# fails with exactly the two second-boot strings (0x5A7:5:1:0022...@90,
# 0x5A7:5:1:14a3@90, attached to the @110 group, one shared window so
# both expire together) unmatched, and exactly the probe and uart-trap
# checks fail (the layout and uart-base checks and the probe's
# connect group must still pass).
#
# In legacy mode (STIX_PORT_BASE unset) this script does NOT take the
# /tmp/stix-qtest-21000 flock (slot 0) itself; the caller must hold it.
# With STIX_PORT_BASE=auto or a slot number it takes its own slot
# (scripts/stix/lib/slot.sh) and boots a private copy of the master
# flash (stix_flash_private); in legacy, a run dir without flash.bin
# gets a fresh empty one (fault-smoke's truncate — the SAFE boot
# expectations hold on a fresh flash too).
#
# Usage: edac-smoke.sh <qemu-binary> [--negative] [--dry-run]
#                      [--check-guard]
#   STIX_ASW_ELF            as in tc-smoke.sh
#   STIX_EDAC_RUN_DIR       run dir (default /tmp/stix-edac-run; the
#                           negative appends -neg). Canonicalised, then
#                           anything at or above /tmp/stix-uprev-run or
#                           /tmp/stix-baseline-run (or /) is refused, as
#                           in fault-smoke.sh. The QMP socket is
#                           <run dir>/qmp.sock and its path must stay
#                           under 108 bytes.
#   STIX_EDAC_SECONDS       the probe's --seconds (default 260, the
#                           minimum: the inject at +112 s plus the
#                           second boot's @90 window, with margin)
#   STIX_TMTC_PROBE         the probe script (tests substitute a fake)
#   STIX_PORT_BASE          the slot (scripts/stix/lib/slot.sh): its
#                           port and the run dir <dir>-s<k>
#   --dry-run prints the QEMU and probe command lines, boots nothing.
#   --check-guard only runs the run-dir guard and exits 0/2.
#
# Exit codes: 0 pass (or dry-run / check-guard ok), 77 SKIP (no ELF;
#             the convention for new gates), 1 a failed expectation, a
#             forbidden TM, a failed QMP command, a failed UART or
#             layout check (in --negative: any other set of failed
#             checks), 2 environment problem.
set -euo pipefail

QEMU_BIN_ARG="${1:?usage: edac-smoke.sh <qemu-binary> \
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
RUN_DIR="${STIX_EDAC_RUN_DIR:-/tmp/stix-edac-run}"
DURATION="${STIX_EDAC_SECONDS:-260}"
PROBE="${STIX_TMTC_PROBE:-$REPO_ROOT/scripts/stix/tmtc-probe.py}"
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
# Every mode has its own run dir (detector-smoke's rule: a later mode
# must never overwrite an earlier mode's evidence).
MODE_SFX=""
[ "$NEGATIVE" -eq 1 ] && MODE_SFX="-neg"
RUN_DIR="$(stix_slot_dir "$RUN_DIR$MODE_SFX")"

# The run-dir guard of fault-smoke.sh: canonical first (realpath -m
# resolves a trailing slash, //, /., a symlink or a relative path), then
# refuse the two shared run dirs, anything inside them, any parent of
# them, and /.
RUN_DIR="$(realpath -m -- "$RUN_DIR")"
SHARED_UPREV="$(realpath -m -- "$STIX_UPREV_DIR")"
SHARED_OLD="$(realpath -m -- /tmp/stix-uprev-run)"
SHARED_BASE="$(realpath -m -- /tmp/stix-baseline-run)"
refuse_run_dir() {
    echo "refusing run dir $RUN_DIR ($1)"
    echo "this gate boots a private copy of the master flash, so it"
    echo "needs its own run dir away from the shared ones"
    exit 2
}
if [ "$RUN_DIR" = "/" ]; then
    refuse_run_dir "it is the root directory"
fi
for shared in "$SHARED_UPREV" "$SHARED_OLD" "$SHARED_BASE"; do
    case "$RUN_DIR" in
        "$shared" | "$shared"/*)
            refuse_run_dir "it is $shared or inside it" ;;
    esac
    case "$shared" in
        "$RUN_DIR"/*)
            refuse_run_dir "it is a parent of $shared" ;;
    esac
done
QMP_SOCK="$RUN_DIR/qmp.sock"
# sun_path is 108 bytes including the NUL
if [ "${#QMP_SOCK}" -ge 108 ]; then
    echo "refusing run dir $RUN_DIR (the QMP socket path $QMP_SOCK is" \
        "${#QMP_SOCK} bytes; a unix socket path must stay under 108)"
    exit 2
fi

if [ "$CHECK_GUARD" -eq 1 ]; then
    echo "GUARD OK: run dir $RUN_DIR (canonical)"
    exit 0
fi

QEMU_BIN="$(realpath -e "$QEMU_BIN_ARG")" || {
    echo "qemu binary not found: $QEMU_BIN_ARG"; exit 2; }
[ -x "$QEMU_BIN" ] || { echo "not executable: $QEMU_BIN"; exit 2; }

case "$DURATION" in
    '' | *[!0-9]*)
        echo "STIX_EDAC_SECONDS must be whole seconds (got: $DURATION)"
        exit 2
        ;;
esac
if [ "$DURATION" -lt 260 ]; then
    echo "STIX_EDAC_SECONDS must be >= 260 (the inject at +112 s plus" \
        "the second boot's 0x0022 window; got: $DURATION)"
    exit 2
fi

# The QMP command payloads. The upset-inject value is one JSON string
# (the catalogue's grammar: memory,address,bitmask); the xp dump is the
# live layout check of the SDRAM inject word (the header).
qom_set() {  # path property json-value
    printf '{"execute": "qom-set", "arguments": {"path": "%s", "property": "%s", "value": %s}}' \
        "$1" "$2" "$3"
}
XP_CMD='{"execute": "human-monitor-command", "arguments": {"command-line": "xp /4wx 0x4008337C"}}'
SDRAM_INJECT="$(qom_set /machine/stixctrl/edac upset-inject \
    '"sdram,0x4008337C,0x3"')"

# The connect group (before any send) arms at the connect.
CONNECT_ARGS=(
    --expect "0x5A7:5:1:002200000008000000@60"
    --expect "0x5A7:5:1:14a3@60"
    --forbid "0x5A1:1:2"
    --forbid "0x5A1:1:8"
    --forbid "0x5A7:5:3:9434"
    --forbid "0x5A7:5:3:9449"
)
# The layout group and the inject group. In the negative the two
# second-boot expectations attach to the layout group (there is no
# inject group): the probe must then fail with exactly those two
# unmatched (the WANT list).
SDRAM_ARGS=(
    --send-at 110 --qmp-cmd "$XP_CMD"
)
if [ "$NEGATIVE" -eq 1 ]; then
    SDRAM_ARGS+=(
        --expect "0x5A7:5:1:002200000008000000@90"
        --expect "0x5A7:5:1:14a3@90"
    )
else
    SDRAM_ARGS+=(
        --send-at 112 --qmp-cmd "$SDRAM_INJECT"
        --expect "0x5A7:5:1:002200000008000000@90"
        --expect "0x5A7:5:1:14a3@90"
    )
fi

PROBE_ARGS=(
    --port "$TMTC_PORT" --seconds "$DURATION" --window 20
    --qmp "$QMP_SOCK"
    "${CONNECT_ARGS[@]}"
    "${SDRAM_ARGS[@]}"
    --hold
)

QEMU_ARGS=(
    -M leon3_generic -m 64M
    -display none -monitor none
    "${STIX_SLOT_QEMU_ARGS[@]}"
    -global stix-edac.edac-at-boot=all-on
    -qmp "unix:$QMP_SOCK,server=on,wait=off"
    -serial "file:$RUN_DIR/uart.edac.raw"
    -kernel "$ASW_ELF"
)
# NOTE: no -no-reboot — the watchdog reset must re-boot the guest (the
# only STIX gate that omits it; see the header).

MODE="positive"
[ "$NEGATIVE" -eq 1 ] && MODE="negative"

if [ "$DRY_RUN" -eq 1 ]; then
    echo "DRY RUN — the commands edac-smoke would run ($MODE):"
    printf '(cd %s && timeout %s %s ' "$RUN_DIR" "$((DURATION + 30))" \
        "$QEMU_BIN"
    printf '%q ' "${QEMU_ARGS[@]}"
    printf '> qemu-stdout.edac.log 2>&1)\n'
    printf 'python3 -u %s ' "$PROBE"
    printf '%q ' "${PROBE_ARGS[@]}"
    printf '\n'
    exit 0
fi

[ -f "$ASW_ELF" ] || {
    echo "SKIP: ASW ELF absent ($ASW_ELF); set STIX_ASW_ELF to enable"
    exit 77
}

if ss -ltn | grep -q ":${TMTC_PORT} "; then
    echo "TMTC port ${TMTC_PORT} busy — another QEMU instance running?${STIX_SLOT_TAG}"
    exit 2
fi

mkdir -p "$RUN_DIR"
cd "$RUN_DIR"
# Private flash: a fresh sparse copy of the master every run (slot mode;
# legacy runs inside a dir that already holds one). The guard inside
# stix_flash_copy refuses shared dirs strictly.
stix_flash_private
if [ ! -f flash.bin ]; then
    # legacy with no local flash: a fresh empty one (fault-smoke's
    # pattern; the SAFE boot expectations hold on a fresh flash too)
    truncate -s 17716740096 flash.bin
fi
[ -f flash.bin ] || { echo "flash.bin missing — run capture-golden.sh first"; exit 2; }
# Stale outputs from a previous run must never satisfy this gate; the
# dump is appended to, so it must start empty.
rm -f uart.edac.raw uart.edac.txt edac.probe.log \
      qemu-stdout.edac.log qmp.sock

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

# The probe starts first: it retries the TMTC connect until QEMU binds
# the port, and connects to the QMP socket at the first --qmp-cmd (@110).
timeout "$((DURATION + 30))" python3 -u "$PROBE" "${PROBE_ARGS[@]}" \
    > edac.probe.log 2>&1 &
PROBE_PID=$!

timeout "$((DURATION + 30))" "$QEMU_BIN" "${QEMU_ARGS[@]}" \
    > qemu-stdout.edac.log 2>&1 &
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

# On every path the log tail(s) come BEFORE the verdict, so the verdict
# line is always the last thing the gate prints.
if [ "$QEMU_ALIVE" -ne 0 ]; then
    tail -5 qemu-stdout.edac.log
    echo "EDAC-SMOKE: ERROR (QEMU died or never started;" \
        "qemu rc=$QEMU_RC)${STIX_SLOT_TAG}"
    exit 2
fi
if [ "$PROBE_RC" -eq 124 ]; then
    tail -5 edac.probe.log
    echo "EDAC-SMOKE: ERROR (the probe hit its own timeout)${STIX_SLOT_TAG}"
    exit 2
fi
if [ "$PROBE_RC" -eq 2 ]; then
    tail -5 edac.probe.log
    echo "EDAC-SMOKE: ERROR (the probe rejected its arguments)${STIX_SLOT_TAG}"
    exit 2
fi
if grep -q "QMP ERROR: cannot connect" edac.probe.log 2>/dev/null; then
    tail -5 edac.probe.log
    echo "EDAC-SMOKE: ERROR (no QMP connection; QEMU rc=$QEMU_RC)${STIX_SLOT_TAG}"
    exit 2
fi
if grep -q "could not connect" edac.probe.log 2>/dev/null; then
    tail -5 edac.probe.log
    echo "EDAC-SMOKE: ERROR (the probe never connected;" \
        "QEMU rc=$QEMU_RC)${STIX_SLOT_TAG}"
    exit 2
fi

# Every check below runs and records its name in FAILED, so the report
# is complete and --negative can compare the set.
FAILED=()

# 1. the probe (TM expectations and forbids)
if [ "$PROBE_RC" -ne 0 ]; then
    WHY="$(grep -m1 -o 'FORBIDDEN TM: .*' edac.probe.log \
        | cut -d' ' -f3-)" || true
    if [ -n "$WHY" ]; then
        WHY="forbidden: $WHY"
    else
        WHY="$(grep -o 'EXPECTATION NOT MATCHED: .*' edac.probe.log \
            | cut -d' ' -f4- | paste -sd' ')" || true
        if [ -n "$WHY" ]; then
            WHY="not matched: $WHY"
        elif grep -q '^CONNECTION CLOSED' edac.probe.log; then
            WHY="connection closed"
        else
            WHY="probe rc=$PROBE_RC; see edac.probe.log"
        fi
    fi
    tail -5 edac.probe.log
    echo "CHECK probe: FAIL ($WHY)"
    FAILED+=(probe)
else
    echo "CHECK probe: ok (every TM expectation matched, no forbid fired)"
fi

# 2. the SDRAM inject word's layout (the xp return in the probe log).
# The HMP xp format (monitor/hmp-cmds.c memory_dump) is exact: an 8-digit
# address, then " 0x%08x" per 32-bit word, lowercase.
LAYOUT_LINE="$(grep -m1 '4008337c: 0x' edac.probe.log || true)"
case "$LAYOUT_LINE" in
    *"4008337c: 0x00000fff 0x00000003 "*)
        echo "CHECK layout: ok (hkLineLimits[7] = {min 0, max 0x0fff," \
            "count 3} at 0x4008337C, live)"
        ;;
    *)
        echo "CHECK layout: FAIL (the xp dump at 0x4008337C is not" \
            "hkLineLimits[7] = 0x00000fff 0x00000003; got:" \
            "${LAYOUT_LINE:-nothing})"
        FAILED+=(layout)
        ;;
esac

# 3. the UART base (both modes), NULs stripped
tr -d '\000' < uart.edac.raw > uart.edac.txt
UART_BASE_OK=1
if ! grep -q "init: SAFE mode" uart.edac.txt; then
    echo "CHECK uart: the UART has no 'init: SAFE mode' banner"
    UART_BASE_OK=0
fi
if grep -Eq 'storing new context[01] failed' uart.edac.txt; then
    grep -Em1 'storing new context[01] failed' uart.edac.txt
    echo "CHECK uart: a flash context write failed"
    UART_BASE_OK=0
fi
if [ "$UART_BASE_OK" -eq 1 ]; then
    echo "CHECK uart: ok (SAFE banner, no context failure)"
else
    FAILED+=(uart)
fi

# 5. the trap and the reboot (the SDRAM part): exactly one trap print
# (WDT.c:135/:151), one-shot (no boot loop), and the SAFE banner twice
# (boot 1 + boot 2).
N_SAFE="$(grep -c 'init: SAFE mode' uart.edac.txt || true)"
N_TRAP="$(grep -c 'Unexpected trap (0x09)' uart.edac.txt || true)"
N_DAE="$(grep -c 'data_access_exception' uart.edac.txt || true)"
echo "UART: $N_SAFE 'init: SAFE mode' banners (want 2: boot 1 + boot 2)," \
    "$N_TRAP 'Unexpected trap (0x09)' and $N_DAE 'data_access_exception'" \
    "lines (want 1 each)"
if [ "$N_SAFE" -eq 2 ] && [ "$N_TRAP" -eq 1 ] && [ "$N_DAE" -eq 1 ]; then
    echo "CHECK uart-trap: ok (one trap, one reboot)"
else
    echo "CHECK uart-trap: FAIL (see the counts above)"
    FAILED+=(uart-trap)
fi

GOT="${FAILED[*]:-}"
pass_cleanup() {
    stix_slot_cleanup 0 flash.bin
}
if [ "$NEGATIVE" -eq 1 ]; then
    # PASS only on the planned failure: the probe rc 1 with exactly the
    # two second-boot expectations unmatched (the WANT list, fault-smoke's
    # discipline), nothing forbidden, no QMP error, and exactly the
    # probe (that unmatched pair) and uart-trap checks failed
    # (detector-smoke's discipline).
    # both second-boot expectations share the @90 window, so they expire
    # in the same poll and the probe reports both (it fails at the first
    # expired deadline and lists only what has expired by then)
    WANT="0x5A7:5:1:002200000008000000@90 0x5A7:5:1:14a3@90"
    MISSED="$(grep -o 'EXPECTATION NOT MATCHED: .*' edac.probe.log \
        | cut -d' ' -f4- | sort | paste -sd' ')" || true
    if [ "$PROBE_RC" -eq 1 ] && [ "$MISSED" = "$WANT" ] && \
            [ "$GOT" = "probe uart-trap" ] && \
            ! grep -q 'FORBIDDEN TM: \|QMP ERROR: ' edac.probe.log; then
        echo "expected failure seen: not matched: $MISSED; failed checks:" \
            "$GOT"
        pass_cleanup
        echo "EDAC-SMOKE (negative): PASS${STIX_SLOT_TAG}"
        exit 0
    fi
    tail -5 edac.probe.log
    if [ "$PROBE_RC" -eq 0 ]; then
        echo "EDAC-SMOKE (negative): FAIL (the probe passed: the gate" \
            "did not detect the missing reaction)${STIX_SLOT_TAG}"
    else
        echo "EDAC-SMOKE (negative): FAIL (unmatched: '${MISSED}', want" \
            "'$WANT'; failed checks: '${GOT}', want" \
            "'probe uart-trap')${STIX_SLOT_TAG}"
    fi
    exit 1
fi

if [ "$PROBE_RC" -ne 0 ] || [ -n "$GOT" ]; then
    echo "EDAC-SMOKE: FAIL (failed checks: '${GOT:-probe}')${STIX_SLOT_TAG}"
    exit 1
fi
pass_cleanup
echo "EDAC-SMOKE: PASS${STIX_SLOT_TAG}"
exit 0
