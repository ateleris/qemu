#!/usr/bin/env bash
# Tests for scripts/stix/boot-smoke.sh (review finding T2: the smoke could
# report PASS without running QEMU). Plain bash; exits non-zero on the first
# failure. Each case runs in a fresh temp run directory with an empty
# flash.bin and a dummy ELF — the real 16.5 GB flash.bin is never touched.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SMOKE="$REPO_ROOT/scripts/stix/boot-smoke.sh"
FAKE="$REPO_ROOT/scripts/stix/tests/fake-qemu.sh"
FAKE_ABS="$(cd "$(dirname "$FAKE")" && pwd)/$(basename "$FAKE")"
# Review S5: the gates' "port busy" check uses ss; a stub reports no
# listener, so these fake-QEMU cases pass while another track's real QEMU
# holds 20000+uid (nothing here binds a port).
export PATH="$REPO_ROOT/scripts/stix/tests/stub-bin:$PATH"
PASS=0
FAIL=0

report() { # name expected outcome
    local name="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        echo "ok   - $name ($actual)"
        PASS=$((PASS + 1))
    else
        echo "FAIL - $name: expected $expected, got $actual"
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

chmod +x "$FAKE" 2>/dev/null || true

# --- case a: relative path valid in the caller's CWD (repo root), run dir
# elsewhere — pins the README usage; the script must resolve the binary
# BEFORE cd-ing to the run directory ---
d="$(fresh_run_dir)"
out="$(cd "$REPO_ROOT" && STIX_RUN_DIR="$d" STIX_ASW_ELF="$d/dummy.elf" \
    STIX_SMOKE_SECONDS=2 FAKE_QEMU_MODE=good \
    bash "$SMOKE" scripts/stix/tests/fake-qemu.sh 2>&1)"
rc=$?
case_rc="$rc"
if echo "$out" | grep -q "BOOT SMOKE: PASS" && [ "$case_rc" -eq 0 ]; then
    report "case a: relative path, good fake => real PASS" "ok" "ok"
else
    report "case a: relative path, good fake => real PASS" "ok" "FAIL ($case_rc): $out"
fi
rm -rf "$d"

# --- case b: silent QEMU with a stale golden-matching uart.smoke.raw ---
d="$(fresh_run_dir)"
cp "$REPO_ROOT/docs/stix-port/baseline/uart.golden.log" "$d/uart.smoke.raw"
out="$(STIX_RUN_DIR="$d" STIX_ASW_ELF="$d/dummy.elf" \
    STIX_SMOKE_SECONDS=2 FAKE_QEMU_MODE=silent \
    bash "$SMOKE" "$FAKE_ABS" 2>&1)"
rc=$?
if [ "$rc" -ne 0 ]; then
    report "case b: silent QEMU + stale output => FAIL" "ok" "ok"
else
    report "case b: silent QEMU + stale output => FAIL" "ok" "FAIL (rc=0): $out"
fi
rm -rf "$d"

# --- case c: crashing QEMU ---
d="$(fresh_run_dir)"
out="$(STIX_RUN_DIR="$d" STIX_ASW_ELF="$d/dummy.elf" \
    STIX_SMOKE_SECONDS=2 FAKE_QEMU_MODE=crash \
    bash "$SMOKE" "$FAKE_ABS" 2>&1)"
rc=$?
if [ "$rc" -eq 2 ]; then
    report "case c: crashing QEMU => exit 2" "ok" "ok"
else
    report "case c: crashing QEMU => exit 2" "ok" "FAIL (rc=$rc): $out"
fi
rm -rf "$d"

# --- case d: non-existent binary ---
d="$(fresh_run_dir)"
out="$(STIX_RUN_DIR="$d" STIX_ASW_ELF="$d/dummy.elf" \
    bash "$SMOKE" /nonexistent/qemu-system-sparc 2>&1)"
rc=$?
if [ "$rc" -eq 2 ]; then
    report "case d: missing binary => exit 2" "ok" "ok"
else
    report "case d: missing binary => exit 2" "ok" "FAIL (rc=$rc): $out"
fi
rm -rf "$d"

# --- case e: QEMU killed by the timeout (rc 124) with good output => PASS ---
d="$(fresh_run_dir)"
out="$(STIX_RUN_DIR="$d" STIX_ASW_ELF="$d/dummy.elf" \
    STIX_SMOKE_SECONDS=2 FAKE_QEMU_MODE=good FAKE_QEMU_RUN_SECONDS=30 \
    bash "$SMOKE" "$FAKE_ABS" 2>&1)"
rc=$?
if echo "$out" | grep -q "BOOT SMOKE: PASS" && [ "$rc" -eq 0 ]; then
    report "case e: timeout-killed QEMU with good output => PASS" "ok" "ok"
else
    report "case e: timeout-killed QEMU with good output => PASS" "ok" "FAIL ($rc): $out"
fi
rm -rf "$d"

echo "----------------------------------------"
echo "pass=$PASS fail=$FAIL"
[ "$FAIL" -eq 0 ]
