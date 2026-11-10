#!/usr/bin/env bash
# Tests for scripts/stix/soak.sh. The first version sampled the UART log
# only after QEMU had been killed by its timeout, so both samples read the
# same closed file and every run "FAILED — heartbeat stopped": the FSW
# "stall at FSW ~4.8 min" was the 600 s kill landing at FSW time 0.48x
# wall (see docs/stix-port/2026-11-09-task-b-stall-analysis.md). These
# cases pin that liveness is sampled while QEMU is still running.
#
# Plain bash; each case uses a fresh temp run dir with an empty flash.bin
# and a dummy ELF — the real 16.5 GB flash.bin is never touched. Short
# windows via STIX_SOAK_WINDOW / STIX_SOAK_MARGIN.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SOAK="$REPO_ROOT/scripts/stix/soak.sh"
FAKE="$REPO_ROOT/scripts/stix/tests/fake-qemu.sh"
PASS=0
FAIL=0

report() { # name ok|<failure detail>
    if [ "$2" = "ok" ]; then
        echo "ok   - $1"
        PASS=$((PASS + 1))
    else
        echo "FAIL - $1: $2"
        FAIL=$((FAIL + 1))
    fi
}

fresh_run_dir() {
    local d
    d="$(mktemp -d)"
    : > "$d/flash.bin"
    echo "dummy elf" > "$d/dummy.elf"
    echo "$d"
}

run_soak() { # dir total [env...]  -> sets out, rc, elapsed
    local d="$1" total="$2"; shift 2
    local t0=$SECONDS
    out="$(env STIX_RUN_DIR="$d" STIX_ASW_ELF="$d/dummy.elf" \
        STIX_SOAK_WINDOW=4 STIX_SOAK_MARGIN=2 "$@" \
        bash "$SOAK" "$FAKE" "$total" 2>&1)"
    rc=$?
    elapsed=$((SECONDS - t0))
}

chmod +x "$FAKE" 2>/dev/null || true

# --- case a: guest prints until the timeout kills it => PASS, and the
# script ends with the window (not a full window after QEMU died) ---
d="$(fresh_run_dir)"
run_soak "$d" 10 FAKE_QEMU_MODE=heartbeat FAKE_QEMU_RUN_SECONDS=60
if [ "$rc" -eq 0 ] && echo "$out" | grep -q "SOAK: PASS" && [ "$elapsed" -le 14 ]; then
    report "case a: live guest to the timeout => PASS within the window" ok
else
    report "case a: live guest to the timeout => PASS within the window" \
        "rc=$rc elapsed=${elapsed}s: $out"
fi
rm -rf "$d"

# --- case b: guest hangs (QEMU alive, UART silent) => exit 1 ---
d="$(fresh_run_dir)"
run_soak "$d" 10 FAKE_QEMU_MODE=freeze FAKE_QEMU_FREEZE_AFTER=2 FAKE_QEMU_RUN_SECONDS=60
if [ "$rc" -eq 1 ] && echo "$out" | grep -q "heartbeat stopped"; then
    report "case b: hung guest => FAIL exit 1" ok
else
    report "case b: hung guest => FAIL exit 1" "rc=$rc: $out"
fi
rm -rf "$d"

# --- case c: QEMU crashes => exit 2, without sitting out the window ---
d="$(fresh_run_dir)"
run_soak "$d" 10 FAKE_QEMU_MODE=crash
if [ "$rc" -eq 2 ] && [ "$elapsed" -le 5 ]; then
    report "case c: crashing QEMU => exit 2 promptly" ok
else
    report "case c: crashing QEMU => exit 2 promptly" "rc=$rc elapsed=${elapsed}s: $out"
fi
rm -rf "$d"

# --- case d: QEMU exits cleanly before the window ends => exit 2 (a
# -no-reboot guest reset/halt is not a healthy soak) ---
d="$(fresh_run_dir)"
run_soak "$d" 10 FAKE_QEMU_MODE=heartbeat FAKE_QEMU_RUN_SECONDS=3
if [ "$rc" -eq 2 ]; then
    report "case d: early clean QEMU exit => exit 2" ok
else
    report "case d: early clean QEMU exit => exit 2" "rc=$rc: $out"
fi
rm -rf "$d"

# --- case e: window does not fit in the run => exit 2 up front ---
d="$(fresh_run_dir)"
run_soak "$d" 5 FAKE_QEMU_MODE=heartbeat
if [ "$rc" -eq 2 ] && [ "$elapsed" -le 2 ]; then
    report "case e: window longer than the run => exit 2" ok
else
    report "case e: window longer than the run => exit 2" "rc=$rc elapsed=${elapsed}s: $out"
fi
rm -rf "$d"

# --- case f: healthy run where the TM probe has already exited at the end
# (what a real probe does when QEMU's socket closes) => still exit 0. The
# first real PASS printed "SOAK: PASS" and exited 1: the EXIT trap's kill
# of the gone probe failed under set -e. ---
d="$(fresh_run_dir)"
printf 'print("# connected (fake probe)")\nprint("# total 37 bytes")\n' > "$d/fake-probe.py"
run_soak "$d" 10 FAKE_QEMU_MODE=heartbeat FAKE_QEMU_RUN_SECONDS=60 STIX_TMTC_PROBE="$d/fake-probe.py"
if [ "$rc" -eq 0 ] && echo "$out" | grep -q "SOAK: PASS.*37 bytes"; then
    report "case f: probe gone at the end => PASS exit 0" ok
else
    report "case f: probe gone at the end => PASS exit 0" "rc=$rc: $out"
fi
rm -rf "$d"

echo "----------------------------------------"
echo "pass=$PASS fail=$FAIL"
[ "$FAIL" -eq 0 ]
