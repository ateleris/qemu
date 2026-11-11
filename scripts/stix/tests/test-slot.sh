#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Tests for scripts/stix/lib/slot.sh (gate-ports plan G-1). Plain bash;
# every case uses a temporary lock dir via STIX_SLOT_LOCK_DIR, so the
# real /tmp/stix-qtest-*.lock files are never touched, and a fake master
# flash, so the shared flash.bin files are never read.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
LIB="$REPO_ROOT/scripts/stix/lib/slot.sh"
PASS=0
FAIL=0
UIDB=$((20000 + $(id -u)))

check() { # name status detail: status 0 is a pass
    if [ "$2" -eq 0 ]; then
        echo "ok   - $1"
        PASS=$((PASS + 1))
    else
        echo "FAIL - $1: $3"
        FAIL=$((FAIL + 1))
    fi
}

LOCKS="$(mktemp -d)"
WORK="$(mktemp -d)"
HOLDERS=()
cleanup() {
    local p
    for p in "${HOLDERS[@]}"; do kill -9 "$p" 2>/dev/null || true; done
    rm -rf "$LOCKS" "$WORK"
}
trap cleanup EXIT

lockf() { # k -> lock file of slot k in the test lock dir
    echo "$LOCKS/stix-qtest-$((UIDB + 100 * $1)).lock"
}

# Run a snippet with the helper sourced, in a clean environment.
helper() { # snippet [env...]
    local snip="$1"
    shift
    env -u STIX_PORT_BASE -u STIX_SLOTS -u STIX_GS_TM_PORT \
        -u STIX_GS_UDP_TM_PORT -u STIX_GS_UDP_TC_PORT \
        STIX_SLOT_LOCK_DIR="$LOCKS" "$@" \
        bash -c "set -euo pipefail; . '$LIB'; $snip" 2>&1
}

hold() { # k: an outside flock holds slot k until release_all
    # -o: the lock stays with flock itself, so killing it frees the lock
    flock -o "$(lockf "$1")" sleep 600 > /dev/null 2>&1 < /dev/null &
    HOLDERS+=($!)
    local i
    for i in $(seq 50); do
        flock -n "$(lockf "$1")" true || return 0
        sleep 0.1
    done
}

is_held() { # k
    ! flock -n "$(lockf "$1")" true
}

release_all() {
    local p
    for p in "${HOLDERS[@]}"; do kill -9 "$p" 2>/dev/null || true; done
    for p in "${HOLDERS[@]}"; do wait "$p" 2>/dev/null || true; done
    HOLDERS=()
}

# --- unset: legacy, base 20000+uid, no lock, nothing printed ---
out="$(helper 'stix_slot_setup
    echo "$TMTC_PORT $STIX_SLOT_MODE [$STIX_SLOT_TAG]" \
        "${#STIX_SLOT_QEMU_ARGS[@]} $(stix_slot_dir /tmp/x)"
    ls "$STIX_SLOT_LOCK_DIR" | wc -l')"
[ "$out" = "$(printf '%s 0 [] 0 /tmp/x\n0' "$UIDB")" ]
check "unset: legacy port, no lock, no output" $? "$out"

# --- auto never takes slot 0, free or held ---
B1=$((UIDB + 100))
out="$(helper 'stix_slot_setup; echo "$STIX_SLOT $STIX_PORT_BASE $TMTC_PORT"' \
    STIX_PORT_BASE=auto)"
[ "$out" = "$(printf 'slot: 1 base %s\n1 %s %s' "$B1" "$B1" "$B1")" ]
check "auto, slot 0 free: takes slot 1, not 0" $? "$out"
hold 0
out="$(helper 'stix_slot_setup; echo "$STIX_SLOT"' STIX_PORT_BASE=auto)"
[ "$(echo "$out" | tail -1)" = 1 ]
check "auto, slot 0 held: slot 1" $? "$out"
release_all

# --- auto with slot 1 held by an outside flock -> slot 2 ---
hold 1
out="$(helper 'stix_slot_setup
    echo "$STIX_SLOT $TMTC_PORT ${STIX_SLOT_QEMU_ARGS[*]}"' \
    STIX_PORT_BASE=auto)"
B2=$((UIDB + 200))
[ "$(echo "$out" | tail -1)" = \
    "2 $B2 -global grlib-stixspw.tmtc-port=$B2" ]
check "auto, slot 1 held: slot 2 and its -global" $? "$out"

# --- all auto slots held -> bounded wait, then exit 2 ---
hold 2
hold 3
t0=$SECONDS
out="$(helper 'stix_slot_setup' STIX_PORT_BASE=auto STIX_SLOT_WAIT=2)"
rc=$?
el=$((SECONDS - t0))
[ "$rc" -eq 2 ] && [ "$el" -ge 2 ] && [ "$el" -le 6 ] &&
    echo "$out" | grep -q "all 3 slots held"
check "auto, all slots held: bounded wait then exit 2" $? \
    "rc=$rc el=$el: $out"

# --- a fourth run waits and gets the slot that frees ---
helper 'stix_slot_setup; echo "got $STIX_SLOT"' STIX_PORT_BASE=auto \
    STIX_SLOT_WAIT=20 > "$WORK/waiter.out" &
wpid=$!
sleep 2
kill -9 "${HOLDERS[1]}" 2>/dev/null
wait "${HOLDERS[1]}" 2>/dev/null
wait "$wpid"
rc=$?
[ "$rc" -eq 0 ] && grep -q "got 2" "$WORK/waiter.out"
check "auto: a waiting run takes the slot that frees" $? \
    "rc=$rc: $(cat "$WORK/waiter.out")"
release_all

# --- a number with its lock held -> exit 2; 0 and junk -> exit 2 ---
hold 2
out="$(helper 'stix_slot_setup' STIX_PORT_BASE=2)"
rc=$?
[ "$rc" -eq 2 ] && echo "$out" | grep -q "slot 2 is held"
check "number 2, held: exit 2" $? "rc=$rc: $out"
out="$(helper 'stix_slot_setup; echo "$STIX_SLOT"' STIX_PORT_BASE=3)"
[ "$(echo "$out" | tail -1)" = 3 ]
check "number 3, free: slot 3" $? "$out"
release_all
for bad in 0 00 x 1x -1; do
    out="$(helper 'stix_slot_setup' STIX_PORT_BASE="$bad")"
    rc=$?
    [ "$rc" -eq 2 ]
    check "number '$bad': exit 2" $? "rc=$rc: $out"
done

# --- STIX_SLOTS overlapping the GS ports -> exit 2 ---
if [ "$UIDB" -eq 21000 ]; then
    out="$(helper 'stix_slot_setup; echo "$STIX_SLOT"' \
        STIX_PORT_BASE=auto STIX_SLOTS=4)"
    [ "$(echo "$out" | tail -1)" = 1 ]
    check "STIX_SLOTS=4 at uid 1000: allowed" $? "$out"
fi
out="$(helper 'stix_slot_setup' STIX_PORT_BASE=auto \
    STIX_SLOTS=$(( (21500 - UIDB) / 100 + 1 )))"
rc=$?
[ "$rc" -eq 2 ] && echo "$out" | grep -q "overlaps the GS port"
check "STIX_SLOTS reaching 21500: exit 2" $? "rc=$rc: $out"
out="$(helper 'stix_slot_setup' STIX_PORT_BASE=auto STIX_SLOTS=2 \
    STIX_GS_UDP_TC_PORT=$((UIDB + 250)))"
rc=$?
[ "$rc" -eq 2 ] && echo "$out" | grep -q "overlaps the GS port $((UIDB + 250))"
check "STIX_GS_UDP_TC_PORT inside slot 2: exit 2" $? "rc=$rc: $out"
out="$(helper 'stix_slot_setup' STIX_PORT_BASE=1 \
    STIX_GS_TM_PORT=$((UIDB + 199)))"
rc=$?
[ "$rc" -eq 2 ]
check "STIX_GS_TM_PORT inside slot 1 (number): exit 2" $? "rc=$rc: $out"
out="$(helper 'stix_slot_setup' STIX_PORT_BASE=auto STIX_SLOTS=0)"
rc=$?
[ "$rc" -eq 2 ]
check "STIX_SLOTS=0: exit 2" $? "rc=$rc: $out"

# --- SIGKILL of a childless holder frees the lock; a child keeps it ---
env STIX_SLOT_LOCK_DIR="$LOCKS" STIX_PORT_BASE=1 bash -c \
    ". '$LIB'; stix_slot_setup > /dev/null; exec sleep 600" \
    > /dev/null 2>&1 &
p=$!
sleep 1
if is_held 1; then
    kill -9 "$p"
    wait "$p" 2>/dev/null
    sleep 0.3
    ! is_held 1
    check "SIGKILL, childless holder: lock freed" $? "still held"
else
    check "SIGKILL, childless holder: lock freed" 1 "never taken"
fi
env STIX_SLOT_LOCK_DIR="$LOCKS" STIX_PORT_BASE=1 bash -c \
    ". '$LIB'; stix_slot_setup > /dev/null
    sleep 600 & echo \$! > '$WORK/child'; wait" > /dev/null 2>&1 &
p=$!
sleep 1
kill -9 "$p"
wait "$p" 2>/dev/null
sleep 0.3
child="$(cat "$WORK/child")"
if is_held 1; then
    kill -9 "$child"
    sleep 0.3
    ! is_held 1
    check "SIGKILL, a surviving child keeps the lock" $? \
        "held after the child died"
else
    kill -9 "$child" 2>/dev/null
    check "SIGKILL, a surviving child keeps the lock" 1 \
        "freed while the child ran"
fi

# --- busy= counts held slots without taking any lock ---
hold 0
hold 2
out="$(helper 'stix_slot_setup > /dev/null; echo "$STIX_SLOT_TAG"' \
    STIX_PORT_BASE=auto)"
case "$out" in
    " slot=1 busy=2 load="[0-9]*) st=0 ;;
    *) st=1 ;;
esac
check "busy= counts slots 0 and 2 for slot 1" $st "$out"
# no flock on any slot lock while counting: a fake flock on PATH records
# every call, and the free slot 3 stays free
mkdir -p "$WORK/bin"
printf '#!/bin/sh\necho "$@" >> %s/flock.calls\nexit 0\n' "$WORK" \
    > "$WORK/bin/flock"
chmod +x "$WORK/bin/flock"
: > "$WORK/flock.calls"
out="$(helper 'STIX_SLOT=1; STIX_SLOT_MODE=1; stix_slot_busy' \
    PATH="$WORK/bin:$PATH")"
[ "$out" = 2 ] && [ ! -s "$WORK/flock.calls" ] && ! is_held 3
check "stix_slot_busy takes no lock (slot 3 stays free)" $? \
    "out=$out calls=$(cat "$WORK/flock.calls")"
release_all

# --- the sparse-copy guard (a fake 8 MiB master with 16 KiB of data) ---
M="$WORK/master/flash.bin"
mkdir -p "$WORK/master" "$WORK/run"
truncate -s 8M "$M"
dd if=/dev/urandom of="$M" bs=4k count=4 seek=100 conv=notrunc status=none
before="$(stat -c '%Y %i %b %s' "$M")"
flash() { # snippet [env...]: in the run dir, slot 1, master $M
    local snip="$1"
    shift
    (cd "$WORK/run" && helper "stix_slot_setup > /dev/null; $snip" \
        STIX_PORT_BASE=1 STIX_FLASH_MASTER="$M" "$@")
}
out="$(flash 'stix_flash_private; du -k flash.bin | cut -f1
    stat -c %s flash.bin' STIX_FLASH_MAX_ALLOC_KB=1024)"
rc=$?
alloc="$(echo "$out" | sed -n 1p)"
size="$(echo "$out" | sed -n 2p)"
[ "$rc" -eq 0 ] && [ "$size" = 8388608 ] && [ "$alloc" -lt 1024 ] &&
    cmp -s "$M" "$WORK/run/flash.bin" &&
    [ "$(stat -c '%Y %i %b %s' "$M")" = "$before" ]
check "flash copy: sparse, identical, master untouched" $? \
    "rc=$rc out=$out"
out="$(flash 'stix_flash_private' STIX_FLASH_MAX_ALLOC_KB=1024 \
    STIX_CP_CMD="cp --sparse=never")"
rc=$?
[ "$rc" -eq 2 ] && [ ! -e "$WORK/run/flash.bin" ] &&
    echo "$out" | grep -q "refusing the flash copy"
check "flash copy: a non-sparse copy is refused and removed" $? \
    "rc=$rc: $out"
dd if=/dev/urandom of="$WORK/master/dense.bin" bs=1M count=2 status=none
out="$(flash 'stix_flash_private' STIX_FLASH_MAX_ALLOC_KB=1024 \
    STIX_FLASH_MASTER="$WORK/master/dense.bin")"
rc=$?
[ "$rc" -eq 2 ] && [ ! -e "$WORK/run/flash.bin" ]
check "flash copy: the allocation limit is enforced" $? "rc=$rc: $out"
printf '#!/bin/sh\ncp --sparse=always "$1" "$2" && truncate -s 4M "$2"\n' \
    > "$WORK/cp-short"
chmod +x "$WORK/cp-short"
out="$(flash 'stix_flash_private' STIX_CP_CMD="$WORK/cp-short")"
rc=$?
[ "$rc" -eq 2 ] && [ ! -e "$WORK/run/flash.bin" ]
check "flash copy: the apparent size must equal the master's" $? \
    "rc=$rc: $out"
out="$(flash 'stix_flash_private' \
    STIX_FLASH_MASTER="$WORK/master/none.bin")"
rc=$?
[ "$rc" -eq 2 ] && echo "$out" | grep -q "missing"
check "flash copy: a missing master => exit 2" $? "rc=$rc: $out"
echo legacy > "$WORK/run/flash.bin"
out="$(cd "$WORK/run" && helper \
    'stix_slot_setup; stix_flash_private; cat flash.bin' \
    STIX_FLASH_MASTER="$M")"
[ "$out" = legacy ]
check "flash copy: legacy mode leaves flash.bin alone" $? "$out"
rm -f "$WORK/run/flash.bin"

# --- the disk guard (STIX_DF_CMD faked; df prints KiB) ---
printf '#!/bin/sh\necho Avail\necho 2097152\n' > "$WORK/df-low"
printf '#!/bin/sh\necho Avail\necho 20971520\n' > "$WORK/df-high"
chmod +x "$WORK/df-low" "$WORK/df-high"
out="$(helper 'stix_slot_setup; echo REACHED' STIX_DF_CMD="$WORK/df-low")"
rc=$?
[ "$rc" -eq 2 ] && ! echo "$out" | grep -q REACHED &&
    echo "$out" | grep -q "disk guard"
check "disk guard: 2 GiB free, legacy => exit 2 before anything" $? \
    "rc=$rc: $out"
out="$(helper 'stix_slot_setup; echo REACHED' STIX_DF_CMD="$WORK/df-low" \
    STIX_PORT_BASE=auto)"
rc=$?
[ "$rc" -eq 2 ] && ! echo "$out" | grep -q REACHED && ! is_held 1
check "disk guard: 2 GiB free, auto => exit 2, no slot kept" $? \
    "rc=$rc: $out"
out="$(flash "STIX_DF_CMD='$WORK/df-low'; stix_flash_private; echo REACHED" \
    STIX_DF_CMD="$WORK/df-high")"
rc=$?
[ "$rc" -eq 2 ] && [ ! -e "$WORK/run/flash.bin" ]
check "disk guard: checked again before the flash copy" $? "rc=$rc: $out"
out="$(helper 'stix_slot_setup; echo REACHED' STIX_DF_CMD="$WORK/df-high")"
[ "$out" = REACHED ]
check "disk guard: 20 GiB free => proceed" $? "$out"
out="$(helper 'stix_slot_setup; echo REACHED' STIX_DF_CMD="$WORK/df-high" \
    STIX_DISK_MIN_GB=30)"
rc=$?
[ "$rc" -eq 2 ]
check "disk guard: STIX_DISK_MIN_GB raises the floor" $? "rc=$rc: $out"

# --- the run dir per mode ---
out="$(helper 'stix_slot_setup > /dev/null
    stix_slot_dir /tmp/stix-fault-run/' STIX_PORT_BASE=2)"
[ "$out" = /tmp/stix-fault-run-s2 ]
check "slot dir: <dir>-s<k>" $? "$out"

# --- cleanup on PASS removes the big files; FAIL keeps them ---
mk() {
    : > "$WORK/run/flash.bin"
    : > "$WORK/run/trace.log"
    : > "$WORK/run/verdict.log"
}
mk
flash 'stix_slot_cleanup 0 flash.bin trace.log' > /dev/null
[ ! -e "$WORK/run/flash.bin" ] && [ ! -e "$WORK/run/trace.log" ] &&
    [ -e "$WORK/run/verdict.log" ]
check "cleanup: PASS removes flash.bin and the trace log only" $? \
    "$(ls "$WORK/run")"
mk
flash 'stix_slot_cleanup 1 flash.bin trace.log' > /dev/null
[ -e "$WORK/run/flash.bin" ] && [ -e "$WORK/run/trace.log" ]
check "cleanup: FAIL keeps everything" $? "$(ls "$WORK/run")"
(cd "$WORK/run" &&
    helper 'stix_slot_setup; stix_slot_cleanup 0 flash.bin' > /dev/null)
[ -e "$WORK/run/flash.bin" ]
check "cleanup: a legacy PASS keeps everything" $? "$(ls "$WORK/run")"

echo "----------------------------------------"
echo "pass=$PASS fail=$FAIL"
[ "$FAIL" -eq 0 ]
