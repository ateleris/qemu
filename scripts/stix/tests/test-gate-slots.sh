#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Tests for the gate scripts' slot mode (gate-ports plan G-2). Nothing
# boots: QEMU is tests/fake-qemu.sh (it records its argv, CWD and the
# inode of ./flash.bin) and the TM probe is tests/fake-probe.py (it
# records its argv). nominal- and fault-smoke are checked via --dry-run.
#
# Legacy mode (STIX_PORT_BASE unset) must stay byte-identical: each gate
# is also run from the pre-change scripts at STIX_LEGACY_REF (default
# 93438dfa3b, the gate-ports base), extracted with git archive, and the
# QEMU argv, probe argv and output are compared. Without that commit the
# identity cases are skipped (reported, not failed). Bump the ref when a
# later commit changes a gate's legacy command line on purpose.
#
# Slot mode uses STIX_PORT_BASE=1 with a temporary lock dir and a fake
# master flash; the real locks and the shared flash.bin files are never
# touched. The legacy cases check port 20000+uid with ss like the real
# gates, so run this under /tmp/stix-qtest-21000.lock.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
S="$REPO_ROOT/scripts/stix"
FAKE="$S/tests/fake-qemu.sh"
FPROBE="$S/tests/fake-probe.py"
REF="${STIX_LEGACY_REF:-93438dfa3b}"
UIDB=$((20000 + $(id -u)))
B1=$((UIDB + 100))
PASS=0
FAIL=0
SKIP=0

check() { # name status detail: status 0 is a pass
    if [ "$2" -eq 0 ]; then
        echo "ok   - $1"
        PASS=$((PASS + 1))
    else
        echo "FAIL - $1: $3"
        FAIL=$((FAIL + 1))
    fi
}

W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
chmod +x "$FAKE" 2>/dev/null || true
echo "dummy elf" > "$W/dummy.elf"
export STIX_ASW_ELF="$W/dummy.elf"
unset STIX_PORT_BASE STIX_SLOTS STIX_RUN_DIR STIX_TMTC_PROBE

OLD=""
if git -C "$REPO_ROOT" cat-file -e "$REF^{commit}" 2>/dev/null; then
    OLD="$W/old"
    mkdir -p "$OLD"
    git -C "$REPO_ROOT" archive "$REF" scripts/stix docs/stix-port/baseline \
        | tar -x -C "$OLD"
else
    echo "skip - legacy identity: $REF not in this repository"
fi

fresh() { # name -> a fresh run dir with an empty flash.bin
    rm -rf "${W:?}/$1"
    mkdir -p "$W/$1"
    : > "$W/$1/flash.bin"
    echo "$W/$1"
}

# The fake master: 8 MiB apparent, 16 KiB of data.
M="$W/master/flash.bin"
mkdir -p "$W/master"
truncate -s 8M "$M"
dd if=/dev/urandom of="$M" bs=4k count=4 seek=100 conv=notrunc status=none
MSTAT="$(stat -c '%Y %i %b' "$M")"
MINO="$(stat -c %i "$M")"

slotenv() { # cmd...: run in slot 1 with the test lock dir and master
    mkdir -p "$W/locks"
    env STIX_PORT_BASE=1 STIX_SLOT_LOCK_DIR="$W/locks" \
        STIX_FLASH_MASTER="$M" "$@"
}

# ---------------- legacy identity ----------------
# old vs new run in a fresh dir each: the flash inode legitimately differs
strip_ino() { sed -i '/^flash-inode: /d' "$@"; }
legacy_dry() { # script args...: old vs new --dry-run output
    local sc="$1" o n
    shift
    o="$(bash "$OLD/scripts/stix/$sc" "$FAKE" "$@" 2>&1 | sed "s#$OLD#$REPO_ROOT#g")"
    n="$(bash "$S/$sc" "$FAKE" "$@" 2>&1)"
    [ "$o" = "$n" ] && [ -n "$n" ]
    check "legacy: $sc $* identical to $REF" $? \
        "$(diff <(echo "$o") <(echo "$n") | head -5)"
}
if [ -n "$OLD" ]; then
    legacy_dry tc-smoke.sh --dry-run
    legacy_dry nominal-smoke.sh --dry-run
    legacy_dry nominal-smoke.sh --negative --dry-run
    legacy_dry nominal-smoke.sh --check-guard
    legacy_dry fault-smoke.sh --dry-run
    legacy_dry fault-smoke.sh --negative --dry-run
    legacy_dry fault-smoke.sh --check-guard

    # boot-smoke: the same fake QEMU argv and the same output
    for v in old new; do
        if [ "$v" = old ]; then sc="$OLD/scripts/stix"; else sc="$S"; fi
        d="$(fresh boot-legacy)"
        env STIX_RUN_DIR="$d" STIX_SMOKE_SECONDS=2 FAKE_QEMU_RUN_SECONDS=1 \
            FAKE_QEMU_ARGV_FILE="$W/boot.$v.argv" \
            bash "$sc/boot-smoke.sh" "$FAKE" > "$W/boot.$v.out" 2>&1
        echo "rc=$?" >> "$W/boot.$v.out"
    done
    strip_ino "$W/boot.old.argv" "$W/boot.new.argv"
    cmp -s "$W/boot.old.argv" "$W/boot.new.argv" &&
        cmp -s "$W/boot.old.out" "$W/boot.new.out" &&
        grep -q "BOOT SMOKE: PASS" "$W/boot.new.out"
    check "legacy: boot-smoke argv and output identical to $REF" $? \
        "$(diff "$W/boot.old.argv" "$W/boot.new.argv"; \
           diff "$W/boot.old.out" "$W/boot.new.out")"

    # soak: the same QEMU and probe argv, the same verdict (counts masked)
    for v in old new; do
        if [ "$v" = old ]; then sc="$OLD/scripts/stix"; else sc="$S"; fi
        d="$(fresh soak-legacy)"
        env STIX_RUN_DIR="$d" STIX_SOAK_WINDOW=4 STIX_SOAK_SLICE=2 \
            STIX_SOAK_MARGIN=2 STIX_TMTC_PROBE="$FPROBE" FAKE_PROBE_SECONDS=30 \
            FAKE_QEMU_MODE=heartbeat FAKE_QEMU_RUN_SECONDS=60 \
            FAKE_QEMU_ARGV_FILE="$W/soak.$v.argv" \
            FAKE_PROBE_ARGV_FILE="$W/soak.$v.pargv" \
            bash "$sc/soak.sh" "$FAKE" 10 2>&1 | sed 's/[0-9][0-9]*/N/g' \
            > "$W/soak.$v.out"
    done
    strip_ino "$W/soak.old.argv" "$W/soak.new.argv"
    cmp -s "$W/soak.old.argv" "$W/soak.new.argv" &&
        cmp -s "$W/soak.old.pargv" "$W/soak.new.pargv" &&
        cmp -s "$W/soak.old.out" "$W/soak.new.out" &&
        grep -q "SOAK: PASS" "$W/soak.new.out"
    check "legacy: soak argv, probe argv and verdict identical to $REF" $? \
        "$(diff "$W/soak.old.argv" "$W/soak.new.argv"; \
           diff "$W/soak.old.pargv" "$W/soak.new.pargv"; \
           diff "$W/soak.old.out" "$W/soak.new.out")"

    # capture-golden: the same recorded flags
    for v in old new; do
        if [ "$v" = old ]; then sc="$OLD/scripts/stix"; else sc="$S"; fi
        d="$(fresh cap-legacy)"
        env STIX_RUN_DIR="$d" FAKE_QEMU_RUN_SECONDS=1 \
            FAKE_QEMU_ARGV_FILE="$W/cap.$v.argv" \
            bash "$sc/baseline/capture-golden.sh" "$FAKE" "$W/cap.$v" \
            > /dev/null 2>&1
        echo "rc=$?" >> "$W/cap.$v.argv"
    done
    strip_ino "$W/cap.old.argv" "$W/cap.new.argv"
    cmp -s "$W/cap.old/run-flags.txt" "$W/cap.new/run-flags.txt" &&
        cmp -s "$W/cap.old.argv" "$W/cap.new.argv"
    check "legacy: capture-golden argv and run-flags identical to $REF" $? \
        "$(diff "$W/cap.old/run-flags.txt" "$W/cap.new/run-flags.txt"; \
           diff "$W/cap.old.argv" "$W/cap.new.argv")"
fi

# tc-smoke legacy real run (the pre-change script has no probe hook, so
# its argv is checked against the dry-run's command lines instead)
d="$(fresh tc-legacy)"
out="$(env STIX_RUN_DIR="$d" STIX_TMTC_PROBE="$FPROBE" \
    FAKE_QEMU_ARGV_FILE="$W/tc.argv" FAKE_PROBE_ARGV_FILE="$W/tc.pargv" \
    bash "$S/tc-smoke.sh" "$FAKE" 2>&1)"
rc=$?
want="argv:
-M
leon3_generic
-m
64M
-no-reboot
-display
none
-monitor
none
-serial
file:$d/uart.tc.raw
-kernel
$W/dummy.elf
cwd: $d
flash-inode: $(stat -c %i "$d/flash.bin")
end"
[ "$rc" -eq 0 ] && [ "$out" = "TC-SMOKE: PASS" ] &&
    [ "$(cat "$W/tc.argv")" = "$want" ] &&
    [ "$(sed -n 1,2p "$W/tc.pargv" | paste -sd' ')" = "--port $UIDB" ]
check "legacy: tc-smoke real run, unchanged argv and verdict" $? \
    "rc=$rc out=$out $(diff <(echo "$want") "$W/tc.argv")"

# boot-smoke --with-tm legacy: both QEMU runs without -global, the probe
# on 20000+uid
d="$(fresh boot-tm-legacy)"
out="$(env STIX_RUN_DIR="$d" STIX_SMOKE_SECONDS=2 FAKE_QEMU_RUN_SECONDS=2 \
    STIX_TMTC_PROBE="$FPROBE" FAKE_QEMU_ARGV_FILE="$W/btm.argv" \
    FAKE_PROBE_ARGV_FILE="$W/btm.pargv" \
    bash "$S/boot-smoke.sh" "$FAKE" --with-tm 2>&1)"
rc=$?
[ "$rc" -eq 0 ] && [ "$(grep -c '^argv:' "$W/btm.argv")" -eq 2 ] &&
    ! grep -q -- '-global' "$W/btm.argv" &&
    [ "$(sed -n 1,2p "$W/btm.pargv" | paste -sd' ')" = "--port $UIDB" ] &&
    [ "$out" = "BOOT SMOKE: PASS (normalized UART matches golden)
TM CHECK: PASS (telemetry observed on port $UIDB)" ] &&
    [ -e "$d/flash.bin" ]
check "legacy: boot-smoke --with-tm, no -global, flash kept" $? \
    "rc=$rc out=$out"

# ---------------- slot mode ----------------
# the private flash: a copy in <dir>-s1, never the master
flash_ok() { # argv-file dir
    [ "$(stat -c '%Y %i %b' "$M")" = "$MSTAT" ] &&
        grep -q "^cwd: $2\$" "$1" &&
        ! grep -q "^cwd: $2-" "$1" &&
        ! grep -q "^flash-inode: none\$" "$1" &&
        ! grep -q "^flash-inode: $MINO\$" "$1"
}
has_global() { # argv-file count
    [ "$(grep -cx "grlib-stixspw.tmtc-port=$B1" "$1")" -eq "$2" ] &&
        [ "$(grep -cx -- "-global" "$1")" -eq "$2" ]
}

# boot-smoke --with-tm
rm -rf "$W/boot" "$W/boot-s1"
out="$(slotenv STIX_RUN_DIR="$W/boot" STIX_SMOKE_SECONDS=2 \
    FAKE_QEMU_RUN_SECONDS=2 STIX_TMTC_PROBE="$FPROBE" \
    FAKE_QEMU_ARGV_FILE="$W/boot.s.argv" FAKE_PROBE_ARGV_FILE="$W/boot.s.pargv" \
    bash "$S/boot-smoke.sh" "$FAKE" --with-tm 2>&1)"
rc=$?
[ "$rc" -eq 0 ] && has_global "$W/boot.s.argv" 2 &&
    flash_ok "$W/boot.s.argv" "$W/boot-s1" &&
    [ "$(sed -n 1,2p "$W/boot.s.pargv" | paste -sd' ')" = "--port $B1" ] &&
    echo "$out" | grep -q "^slot: 1 base $B1\$" &&
    echo "$out" | grep -q "^BOOT SMOKE: PASS .* slot=1 busy=0 load=" &&
    echo "$out" | grep -q "^TM CHECK: PASS (telemetry observed on port $B1) slot=1" &&
    [ ! -e "$W/boot-s1/flash.bin" ] && [ ! -e "$W/boot/flash.bin" ]
check "slot: boot-smoke --with-tm (port, -s1 dir, private flash, cleanup)" \
    $? "rc=$rc out=$out argv=$(cat "$W/boot.s.argv")"

# soak
rm -rf "$W/soak" "$W/soak-s1"
out="$(slotenv STIX_RUN_DIR="$W/soak" STIX_SOAK_WINDOW=4 STIX_SOAK_SLICE=2 \
    STIX_SOAK_MARGIN=2 STIX_TMTC_PROBE="$FPROBE" FAKE_PROBE_SECONDS=30 \
    FAKE_QEMU_MODE=heartbeat FAKE_QEMU_RUN_SECONDS=60 \
    FAKE_QEMU_ARGV_FILE="$W/soak.s.argv" FAKE_PROBE_ARGV_FILE="$W/soak.s.pargv" \
    bash "$S/soak.sh" "$FAKE" 10 2>&1)"
rc=$?
[ "$rc" -eq 0 ] && has_global "$W/soak.s.argv" 1 &&
    flash_ok "$W/soak.s.argv" "$W/soak-s1" &&
    [ "$(sed -n 1,2p "$W/soak.s.pargv" | paste -sd' ')" = "--port $B1" ] &&
    echo "$out" | grep -q "^SOAK: PASS .* slot=1 busy=0 load=" &&
    [ ! -e "$W/soak-s1/flash.bin" ]
check "slot: soak (port, -s1 dir, private flash, cleanup)" $? \
    "rc=$rc out=$out"

# tc-smoke
rm -rf "$W/tc" "$W/tc-s1"
out="$(slotenv STIX_RUN_DIR="$W/tc" STIX_TMTC_PROBE="$FPROBE" \
    FAKE_QEMU_ARGV_FILE="$W/tc.s.argv" FAKE_PROBE_ARGV_FILE="$W/tc.s.pargv" \
    bash "$S/tc-smoke.sh" "$FAKE" 2>&1)"
rc=$?
[ "$rc" -eq 0 ] && has_global "$W/tc.s.argv" 1 &&
    flash_ok "$W/tc.s.argv" "$W/tc-s1" &&
    [ "$(sed -n 1,2p "$W/tc.s.pargv" | paste -sd' ')" = "--port $B1" ] &&
    echo "$out" | grep -q "^TC-SMOKE: PASS slot=1 busy=0 load=" &&
    [ ! -e "$W/tc-s1/flash.bin" ]
check "slot: tc-smoke (port, -s1 dir, private flash, cleanup)" $? \
    "rc=$rc out=$out"

# a FAIL keeps the slot dir's flash: a crashing QEMU
rm -rf "$W/tcf" "$W/tcf-s1"
out="$(slotenv STIX_RUN_DIR="$W/tcf" STIX_TMTC_PROBE="$FPROBE" \
    FAKE_QEMU_MODE=crash bash "$S/tc-smoke.sh" "$FAKE" 2>&1)"
rc=$?
[ "$rc" -eq 2 ] && [ -e "$W/tcf-s1/flash.bin" ] &&
    echo "$out" | grep -q "^TC-SMOKE: ERROR .* slot=1 busy=0 load="
check "slot: tc-smoke ERROR keeps the flash, verdict tagged" $? \
    "rc=$rc out=$out"

# nominal / fault: the dry-run lines
dry_ok() { # script dir args...
    local sc="$1" dir="$2" out
    shift 2
    out="$(slotenv "$@" bash "$S/$sc" "$FAKE" --dry-run 2>&1)"
    echo "$out" | grep -q "^(cd $dir-s1 && timeout " &&
        echo "$out" | grep -q -- "-monitor none -global grlib-stixspw.tmtc-port=$B1 " &&
        echo "$out" | grep -q "file:$dir-s1/uart\." &&
        echo "$out" | grep -q "^python3 -u .* --port $B1 "
    check "slot: $sc --dry-run (-s1 dir, -global, --port)" $? "$out"
}
dry_ok nominal-smoke.sh "$W/nom" STIX_NOMINAL_RUN_DIR="$W/nom"
dry_ok fault-smoke.sh "$W/flt" STIX_FAULT_RUN_DIR="$W/flt"
out="$(slotenv STIX_FAULT_RUN_DIR="$W/flt" bash "$S/fault-smoke.sh" \
    "$FAKE" --dry-run 2>&1)"
echo "$out" | grep -q -- "-qmp unix:$W/flt-s1/qmp.sock" &&
    echo "$out" | grep -q -- "--qmp $W/flt-s1/qmp.sock "
check "slot: fault-smoke QMP socket in the -s1 dir" $? "$out"
out="$(slotenv STIX_NOMINAL_RUN_DIR=/tmp/stix-uprev-run/x \
    bash "$S/nominal-smoke.sh" "$FAKE" --check-guard 2>&1)"
[ $? -eq 2 ]
check "slot: nominal run-dir guard still refuses the shared dir" $? "$out"

# capture-golden refuses slot mode, before any QEMU
: > "$W/cap.s.argv"
out="$(slotenv STIX_RUN_DIR="$W/cap-s" FAKE_QEMU_ARGV_FILE="$W/cap.s.argv" \
    bash "$S/baseline/capture-golden.sh" "$FAKE" "$W/cap.s" 2>&1)"
rc=$?
[ "$rc" -eq 2 ] && [ ! -s "$W/cap.s.argv" ] && [ ! -e "$W/cap-s" ]
check "slot: capture-golden refuses slot mode" $? "rc=$rc out=$out"

# a missing master: boot-smoke exits 2 before QEMU
: > "$W/miss.argv"
out="$(slotenv STIX_RUN_DIR="$W/miss" STIX_FLASH_MASTER="$W/none.bin" \
    FAKE_QEMU_ARGV_FILE="$W/miss.argv" bash "$S/boot-smoke.sh" "$FAKE" 2>&1)"
rc=$?
[ "$rc" -eq 2 ] && [ ! -s "$W/miss.argv" ]
check "slot: missing master => exit 2, no QEMU" $? "rc=$rc out=$out"

echo "----------------------------------------"
echo "pass=$PASS fail=$FAIL"
[ "$FAIL" -eq 0 ]
