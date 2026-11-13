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
# 9c7bd11322, the gate-ports base after D-4), extracted with git archive,
# and the QEMU argv, probe argv and output are compared (detector-smoke's
# per-mode run dir suffix mapped back). Without that commit the
# identity cases are skipped (reported, not failed). Bump the ref when a
# later commit changes a gate's legacy command line on purpose.
#
# Slot mode uses STIX_PORT_BASE=1 with a temporary lock dir and a fake
# master flash; the real locks and the shared flash.bin files are never
# touched. The legacy cases run the real gates' ss port check against a
# stub (stub-bin/ss), so no 21000 lock is needed.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
S="$REPO_ROOT/scripts/stix"
FAKE="$S/tests/fake-qemu.sh"
FPROBE="$S/tests/fake-probe.py"
# Review S5: the gates' "port busy" check uses ss; a stub reports no
# listener, so these fake-QEMU cases pass while another track's real QEMU
# holds 20000+uid (nothing here binds a port).
export PATH="$REPO_ROOT/scripts/stix/tests/stub-bin:$PATH"
REF="${STIX_LEGACY_REF:-9c7bd11322}"
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
    # Intended change: the boot golden itself moved on (cache EDAC
    # enabled, E_EDAC_MODULE_CACHE = 1); the legacy case compares the
    # scripts, so both sides check against today's golden.
    cp "$REPO_ROOT/docs/stix-port/baseline/uart.golden.log" \
        "$OLD/docs/stix-port/baseline/uart.golden.log"
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
    # Intended changes: the master/legacy run dir moved out of /tmp
    # (lib/paths.sh); for detector-smoke also the per-mode run dir (-neg)
    # and the trace events file (the FSM strobes mark NOMINAL in the trace)
    n="$(bash "$S/$sc" "$FAKE" "$@" 2>&1 |
        sed -e "s#$HOME/dev/stix-state/uprev-run#/tmp/stix-uprev-run#g" \
            -e 's#\(/tmp/stix-detector-run\)-neg\([/ ,]\)#\1\2#g' \
            -e 's#events=[^ ]*/idefx\.events\\,#enable=stixdet_idefx_cmd\\,#')"
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
    legacy_dry detector-smoke.sh --dry-run
    legacy_dry detector-smoke.sh --negative --dry-run
    legacy_dry detector-smoke.sh --check-guard

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
has_global() { # argv-file qemu-starts: each start has tmtc-port AND tm-hold
    [ "$(grep -cx "grlib-stixspw.tmtc-port=$B1" "$1")" -eq "$2" ] &&
        [ "$(grep -cx "grlib-stixspw.tm-hold=on" "$1")" -eq "$2" ] &&
        [ "$(grep -cx -- "-global" "$1")" -eq "$((2 * $2))" ]
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
out="$(slotenv STIX_DETECTOR_RUN_DIR="$W/det" bash "$S/detector-smoke.sh" \
    "$FAKE" --dry-run 2>&1)"
echo "$out" | grep -q "^(cd $W/det-s1 && timeout " &&
    echo "$out" | grep -q -- "-monitor none -global grlib-stixspw.tmtc-port=$B1 " &&
    echo "$out" | grep -q -- "unix:$W/det-s1/qmp.sock" &&
    echo "$out" | grep -q "^python3 -u .* --port $B1 "
check "slot: detector-smoke.sh --dry-run (-s1 dir, -global, --port)" $? "$out"
out="$(slotenv STIX_FAULT_RUN_DIR="$W/flt" bash "$S/fault-smoke.sh" \
    "$FAKE" --dry-run 2>&1)"
echo "$out" | grep -q -- "-qmp unix:$W/flt-s1/qmp.sock" &&
    echo "$out" | grep -q -- "--qmp $W/flt-s1/qmp.sock "
check "slot: fault-smoke QMP socket in the -s1 dir" $? "$out"
out="$(slotenv STIX_NOMINAL_RUN_DIR=/tmp/stix-uprev-run/x \
    bash "$S/nominal-smoke.sh" "$FAKE" --check-guard 2>&1)"
[ $? -eq 2 ]
check "slot: nominal run-dir guard still refuses the shared dir" $? "$out"
out="$(slotenv STIX_NOMINAL_RUN_DIR="$HOME/dev/stix-state/uprev-run/x" \
    bash "$S/nominal-smoke.sh" "$FAKE" --check-guard 2>&1)"
[ $? -eq 2 ]
check "slot: nominal run-dir guard refuses the persistent master dir" $? "$out"

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

# B2: the gates refuse a run dir that resolves into a shared flash dir
# (synthetic shared dirs; a symlinked dir), before QEMU or any write
mkdir -p "$W/frozen" "$W/uprevm"
ln -sfn "$W/frozen" "$W/frz"
for sc in boot-smoke.sh soak.sh tc-smoke.sh; do
    : > "$W/frz.argv"
    out="$(env STIX_SHARED_DIRS="$W/uprevm $W/frozen" STIX_RUN_DIR="$W/frz" \
        FAKE_QEMU_ARGV_FILE="$W/frz.argv" STIX_TMTC_PROBE="$FPROBE" \
        bash "$S/$sc" "$FAKE" 2>&1)"
    rc=$?
    [ "$rc" -eq 2 ] && [ ! -s "$W/frz.argv" ] && [ -z "$(ls -A "$W/frozen")" ] &&
        echo "$out" | grep -q "inside the shared flash dir"
    check "B2: $sc refuses a run dir resolving into the frozen dir" $? \
        "rc=$rc: $out"
done
ln -sfn "$W/uprevm" "$W/upl-s1"
: > "$W/upl.argv"
out="$(slotenv STIX_SHARED_DIRS="$W/uprevm $W/frozen" STIX_RUN_DIR="$W/upl" \
    FAKE_QEMU_ARGV_FILE="$W/upl.argv" bash "$S/boot-smoke.sh" "$FAKE" 2>&1)"
rc=$?
[ "$rc" -eq 2 ] && [ ! -s "$W/upl.argv" ] && [ -z "$(ls -A "$W/uprevm")" ]
check "B2: slot mode refuses a -s1 dir symlinked to the uprev dir" $? \
    "rc=$rc: $out"

# S1: a gate that deletes flash.bin pre-creates it sparse right after, so
# no QEMU build ever creates (or fills) one itself
for sc in nominal-smoke.sh fault-smoke.sh detector-smoke.sh; do
    awk '/^rm -f flash.bin/ { r = 1 }
         r && !/\\$/ { getline nx; print nx; exit }' "$S/$sc" |
        grep -qx 'truncate -s 17716740096 flash.bin'
    check "S1: $sc pre-creates a sparse flash.bin after its rm" $? \
        "$(grep -n -A3 '^rm -f flash.bin' "$S/$sc")"
done

# NIT: dry-run / check-guard / ELF-absent SKIP boot nothing, so they take
# no slot lock, never wait for one and skip the disk guard
GS="${STIX_GATES_DIR:-$S}"
printf '#!/bin/sh\necho Avail\necho 1024\n' > "$W/df-low"
chmod +x "$W/df-low"
mkdir -p "$W/locks"
PEEKH=()
for k in 1 2 3; do
    flock -o "$W/locks/stix-qtest-$((UIDB + 100 * k)).lock" sleep 300 \
        > /dev/null 2>&1 < /dev/null &
    PEEKH+=($!)
done
sleep 0.3
for sc in nominal-smoke.sh fault-smoke.sh detector-smoke.sh; do
    for a in --dry-run --check-guard; do
        t0=$SECONDS
        out="$(env STIX_PORT_BASE=auto STIX_SLOT_LOCK_DIR="$W/locks" \
            STIX_SLOT_WAIT=20 STIX_DF_CMD="$W/df-low" \
            STIX_NOMINAL_RUN_DIR="$W/pk" STIX_FAULT_RUN_DIR="$W/pk" \
            STIX_DETECTOR_RUN_DIR="$W/pk" \
            timeout 60 bash "$GS/$sc" "$FAKE" "$a" 2>&1)"
        rc=$?
        [ "$rc" -eq 0 ] && [ $((SECONDS - t0)) -lt 10 ]
        check "peek: $sc $a, all slots held, low disk: no wait, exit 0" $? \
            "rc=$rc t=$((SECONDS - t0)): $out"
    done
done
t0=$SECONDS
out="$(env STIX_PORT_BASE=auto STIX_SLOT_LOCK_DIR="$W/locks" \
    STIX_SLOT_WAIT=20 STIX_DF_CMD="$W/df-low" STIX_RUN_DIR="$W/pk" \
    timeout 60 bash "$GS/tc-smoke.sh" "$FAKE" --dry-run 2>&1)"
rc=$?
[ "$rc" -eq 0 ] && [ $((SECONDS - t0)) -lt 10 ]
check "peek: tc-smoke.sh --dry-run, all slots held, low disk: no wait" $? \
    "rc=$rc t=$((SECONDS - t0)): $out"
t0=$SECONDS
out="$(env STIX_PORT_BASE=auto STIX_SLOT_LOCK_DIR="$W/locks" \
    STIX_SLOT_WAIT=20 STIX_DF_CMD="$W/df-low" \
    STIX_NOMINAL_RUN_DIR="$W/pk" STIX_ASW_ELF="$W/no-such.elf" \
    timeout 60 bash "$GS/nominal-smoke.sh" "$FAKE" 2>&1)"
rc=$?
[ "$rc" -eq 0 ] && [ $((SECONDS - t0)) -lt 10 ] && echo "$out" | grep -q SKIP
check "peek: nominal-smoke ELF-absent SKIP: no wait, no disk guard" $? \
    "rc=$rc t=$((SECONDS - t0)): $out"
out="$(env STIX_PORT_BASE=auto STIX_SLOT_LOCK_DIR="$W/locks" \
    STIX_SLOT_WAIT=1 STIX_DF_CMD="$W/df-low" STIX_NOMINAL_RUN_DIR="$W/pk" \
    timeout 60 bash "$GS/nominal-smoke.sh" "$FAKE" 2>&1)"
rc=$?
[ "$rc" -eq 2 ] && echo "$out" | grep -q "disk guard"
check "peek: a real nominal run still runs the disk guard" $? "rc=$rc: $out"
for p in "${PEEKH[@]}"; do kill -9 "$p" 2>/dev/null; wait "$p" 2>/dev/null; done

# --- detector-smoke: per-mode run dirs, slot args in every mode (G-2b) ---
# each mode has its own run dir (-neg, -flare, -flareneg, -norotb), legacy
# and slot
for m in "--negative:-neg" "--flare:-flare" "--flare --negative:-flareneg" \
    "--no-rotb:-norotb" ":"; do
    args="${m%%:*}"
    sfx="${m#*:}"
    # shellcheck disable=SC2086 # $args is a list of flags
    out="$(bash "$S/detector-smoke.sh" "$FAKE" $args --dry-run 2>&1)"
    echo "$out" | grep -q "^(cd /tmp/stix-detector-run$sfx && timeout "
    check "detector legacy '${args:-positive}': /tmp/stix-detector-run$sfx" \
        $? "$out"
    # shellcheck disable=SC2086
    out="$(slotenv STIX_DETECTOR_RUN_DIR="$W/det" bash "$S/detector-smoke.sh" \
        "$FAKE" $args --dry-run 2>&1)"
    echo "$out" | grep -q "^(cd $W/det$sfx-s1 && timeout " &&
        echo "$out" | grep -q -- "-global grlib-stixspw.tmtc-port=$B1 " &&
        echo "$out" | grep -q -- "unix:$W/det$sfx-s1/qmp.sock" &&
        echo "$out" | grep -q "^python3 -u .* --port $B1 "
    check "detector slot '${args:-positive}': -s1 dir, -global, --port" $? \
        "$out"
done
out="$(slotenv STIX_DETECTOR_RUN_DIR="$W/det" bash "$S/detector-smoke.sh" \
    "$FAKE" --flare --dry-run 2>&1)"
echo "$out" | grep -q -- "--qmp $W/det-flare-s1/qmp.sock "
check "detector slot --flare: the probe's QMP socket is the slot's" $? "$out"
for bad in "--no-rotb --flare" "--negative --no-rotb"; do
    # shellcheck disable=SC2086 # $bad is a list of flags
    out="$(bash "$S/detector-smoke.sh" "$FAKE" $bad --dry-run 2>&1)"
    rc=$?
    [ "$rc" -eq 2 ] && echo "$out" | grep -q "combines with neither"
    check "detector: '$bad' is refused" $? "rc=$rc: $out"
done
out="$(bash "$S/detector-smoke.sh" "$FAKE" --no-rotb --dry-run 2>&1)"
echo "$out" | grep -q -- "-global grlib-stixctrl.det-rotb=off" &&
    echo "$out" | grep -q -- "-global grlib-stixctrl.det-source=zero"
check "detector --no-rotb: the zero sky with det-rotb=off" $? "$out"

# a fake run to the verdict: canned probe lines (T_nom = T_exit, no TM
# failure) and a one-packet QL dump that the real ql-check rejects
{
    echo '%T EXPECT OK: 0x5A7:5:1:00210405@10 [m=1000.000]'
    echo '%T EXPECT OK: 0x5A7:5:1:00210504@10 [m=1000.000]'
} > "$W/det.lines"
echo 0ddcc000001a101506000000000000002200000001000000270002000000000000 \
    > "$W/det.dump"
detrun() { # args...: a fake detector-smoke run in slot 1
    slotenv STIX_DETECTOR_RUN_DIR="$W/dr" STIX_TMTC_PROBE="$FPROBE" \
        FAKE_PROBE_LINES="$W/det.lines" FAKE_PROBE_DUMP="$W/det.dump" \
        FAKE_QEMU_RUN_SECONDS=10 bash "$S/detector-smoke.sh" "$FAKE" "$@" 2>&1
}
out="$(detrun --negative)"
rc=$?
last="$(echo "$out" | tail -1)"
[ "$rc" -eq 1 ] &&
    echo "$last" | grep -q "^DETECTOR-SMOKE (negative): FAIL .*slot=1 busy=" &&
    echo "$last" |
        grep -Eq "ql: SSID 30: 0 products, want 1 \(\+[0-9]+ more\)" &&
    echo "$out" | grep -q "^CHECK ql: FAIL (SSID 30: 0 products, want 1" &&
    [ -e "$W/dr-neg-s1/flash.bin" ] && [ -e "$W/dr-neg-s1/ql.check.log" ]
check "detector FAIL: slot tag, the ql mismatch in the verdict, dir kept" $? \
    "rc=$rc: $out"
echo first > "$W/dr-neg-s1/marker"
out="$(detrun --negative)"
rc=$?
moved="$(ls -d "$W"/dr-neg-s1.fail-* 2>/dev/null | head -1)"
[ "$rc" -eq 1 ] && [ -n "$moved" ] && [ "$(cat "$moved/marker")" = first ] &&
    [ ! -e "$W/dr-neg-s1/marker" ] &&
    echo "$out" | grep -q "kept the unfinished run's dir as $moved"
check "detector: a failed run's dir is moved aside, never reused" $? \
    "rc=$rc moved=$moved: $out"
out="$(detrun --flare)"
[ ! -e "$W/dr-neg-s1/ql.contrast.log" ] && [ -e "$W/dr-flare-s1/ql.check.log" ]
check "detector: --flare runs in its own dir, not the negative's" $? \
    "$(ls "$W/dr-neg-s1" "$W/dr-flare-s1")"
out="$(STIX_DETECTOR_RUN_DIR="$W/drl" STIX_TMTC_PROBE="$FPROBE" \
    FAKE_PROBE_LINES="$W/det.lines" FAKE_PROBE_DUMP="$W/det.dump" \
    FAKE_QEMU_RUN_SECONDS=10 bash "$S/detector-smoke.sh" "$FAKE" 2>&1)"
last="$(echo "$out" | tail -1)"
echo "$last" |
    grep -q "^DETECTOR-SMOKE: FAIL (failed checks: ql .*; ql: SSID" &&
    ! echo "$last" | grep -q "slot="
check "detector legacy FAIL: the ql mismatch named, no slot tag" $? "$out"

# --- clock: gate timing is monotonic; a wall-clock step is INCONCLUSIVE ---
# The probe tags each event with CLOCK_MONOTONIC ([m=...]). Here the wall
# HH:MM:SS of T_nom and T_exit are equal (a wall clock stepped back by
# 145 s), the monotonic stamps 145 s apart: the window is 145 s, and the
# flare switch offsets come from the QMP lines' monotonic stamps.
{
    echo '%T EXPECT OK: 0x5A7:5:1:00210405@10 (unit len=25) [m=1000.000]'
    q='%T >> qmp {"execute": "qom-set"} -> return {}'
    echo "$q at +135s det-flare [m=1062.000]"
    echo "$q at +195s det-flare [m=1122.000]"
    echo '%T EXPECT OK: 0x5A7:5:1:00210504@10 (unit len=25) [m=1145.000]'
} > "$W/clk.lines"
clockrun() { # clock-pairs-file args...: a fake slot-1 run, faked clocks
    local pairs="$1"
    shift
    # the fake clock prints line n of the pairs file on its n-th call
    {
        echo '#!/bin/sh'
        echo "n=\$(( \$(cat $pairs.n 2>/dev/null || echo 0) + 1 ))"
        echo "echo \$n > $pairs.n"
        echo "sed -n \"\${n}p\" $pairs"
    } > "$W/clock-cmd"
    chmod +x "$W/clock-cmd"
    rm -f "$pairs.n"
    slotenv STIX_DETECTOR_RUN_DIR="$W/ck" STIX_TMTC_PROBE="$FPROBE" \
        FAKE_PROBE_LINES="$W/clk.lines" FAKE_PROBE_DUMP="$W/det.dump" \
        STIX_CLOCK_CMD="$W/clock-cmd" FAKE_QEMU_RUN_SECONDS=10 \
        bash "$S/detector-smoke.sh" "$FAKE" "$@" 2>&1
}
printf '1700000000.000 900.000\n1700000300.000 1200.000\n' > "$W/clk-ok"
printf '1700000000.000 900.000\n1700000225.000 1200.000\n' > "$W/clk-step"
out="$(clockrun "$W/clk-ok" --flare)"
echo "$out" | grep -q "^NOMINAL window: .* 145.0 s (monotonic)" &&
    echo "$out" |
        grep -q "^flare switch: on at T_nom +62.0 s, off at T_nom +122.0 s"
check "clock: the NOMINAL window and flare offsets are monotonic" $? "$out"
# the same fake run with a steady clock and with a 0.75 wall/monotonic
# step: the same verdict line (the slot tag's load aside), only the
# reported ratio differs
for mode in "" --negative --flare; do
    # shellcheck disable=SC2086 # an optional mode flag
    o1="$(clockrun "$W/clk-ok" $mode)"
    r1=$?
    # shellcheck disable=SC2086
    o2="$(clockrun "$W/clk-step" $mode)"
    r2=$?
    v1="$(echo "$o1" | tail -1 | sed 's/ load=.*//')"
    v2="$(echo "$o2" | tail -1 | sed 's/ load=.*//')"
    [ "$r1" -eq "$r2" ] && [ "$v1" = "$v2" ] &&
        echo "$o1" | grep -q "^clock: wall/monotonic over the run 1.000 " &&
        echo "$o2" | grep -q "^clock: wall/monotonic over the run 0.750 "
    check "clock: a 0.75 wall-clock step leaves '${mode:-positive}' unchanged" \
        $? "rc $r1/$r2: $v1 | $v2"
done

echo "----------------------------------------"
echo "pass=$PASS fail=$FAIL"
[ "$FAIL" -eq 0 ]
