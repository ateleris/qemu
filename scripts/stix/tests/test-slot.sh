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
out="$(helper 'stix_slot_setup
    echo "$STIX_SLOT $STIX_QTEST_PORT_BASE $TMTC_PORT $STIX_PORT_BASE"' \
    STIX_PORT_BASE=auto)"
[ "$out" = "$(printf 'slot: 1 base %s\n1 %s %s auto' "$B1" "$B1" "$B1")" ]
check "auto, slot 0 free: slot 1, not 0; the selector is kept" $? "$out"
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
    "2 $B2 -global grlib-stixspw.tmtc-port=$B2 -global grlib-stixspw.tm-hold=on" ]
check "auto, slot 1 held: slot 2 and its -globals" $? "$out"

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
# a band end past 65535 (a high uid): stix_slot_base is overridden
out="$(helper 'stix_slot_base() { echo $((65450 + 100 * ($1 - 1))); }
    stix_slot_setup; echo REACHED' STIX_PORT_BASE=1)"
rc=$?
[ "$rc" -eq 2 ] && echo "$out" | grep -q "passes port 65535" &&
    ! echo "$out" | grep -q REACHED
check "a slot band ending past 65535: exit 2" $? "rc=$rc: $out"
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

# --- review B1/B2: every guard runs before any byte is written ---
# a recording cp: proves whether a copy was attempted at all
printf '#!/bin/sh\ntouch %s/cp.called\ncp --sparse=always "$1" "$2"\n' \
    "$WORK" > "$WORK/cp-rec"
chmod +x "$WORK/cp-rec"
mkdir -p "$WORK/empty"
nowrite() { # dir: no copy attempted and the dir still empty
    [ ! -e "$WORK/cp.called" ] && [ -z "$(ls -A "$1")" ]
}
rm -f "$WORK/cp.called"
out="$(cd "$WORK/empty" && helper 'stix_slot_setup > /dev/null
    stix_flash_private' STIX_PORT_BASE=1 STIX_CP_CMD="$WORK/cp-rec" \
    STIX_FLASH_MASTER="$WORK/master/dense.bin" STIX_FLASH_MAX_ALLOC_KB=1024)"
rc=$?
[ "$rc" -eq 2 ] && nowrite "$WORK/empty" &&
    echo "$out" | grep -q "refusing to copy the master .*(a dense flash)"
check "B1: a dense master is refused before any copy (nothing written)" $? \
    "rc=$rc: $out; left: $(ls -A "$WORK/empty" "$WORK/cp.called" 2>&1)"
for d in /mnt /proc; do
    rm -f "$WORK/cp.called"
    out="$(cd "$d" && helper 'stix_slot_setup > /dev/null; stix_flash_copy' \
        STIX_CP_CMD="$WORK/cp-rec" STIX_FLASH_MASTER="$M")"
    rc=$?
    [ "$rc" -eq 2 ] && [ ! -e "$WORK/cp.called" ] &&
        echo "$out" | grep -q "refusing the flash copy into $d"
    check "B1: a copy into $d (/mnt or a non-ext filesystem) is refused" $? \
        "rc=$rc: $out"
done
# B2: ./flash.bin is the master (a symlinked -s<k> dir, a symlinked file)
ln -sfn "$WORK/master" "$WORK/sym-s1"
out="$(cd "$WORK/sym-s1" && helper 'stix_slot_setup > /dev/null
    stix_flash_private' STIX_PORT_BASE=1 STIX_FLASH_MASTER="$M")"
rc=$?
[ "$rc" -eq 2 ] && [ "$(stat -c '%Y %i %b %s' "$M")" = "$before" ] &&
    echo "$out" | grep -q "is the master"
check "B2: a symlinked -s<k> dir onto the master's dir is refused" $? \
    "rc=$rc: $out"
mkdir -p "$WORK/symf"
ln -sf "$M" "$WORK/symf/flash.bin"
out="$(cd "$WORK/symf" && helper 'stix_slot_setup > /dev/null
    stix_flash_private' STIX_PORT_BASE=1 STIX_FLASH_MASTER="$M")"
rc=$?
[ "$rc" -eq 2 ] && [ "$(stat -c '%Y %i %b %s' "$M")" = "$before" ]
check "B2: a flash.bin symlinked to the master is refused" $? "rc=$rc: $out"
# B2: the realpath guard on the shared dirs (synthetic, via STIX_SHARED_DIRS)
SH="$WORK/master $WORK/frozen"
mkdir -p "$WORK/frozen"
out="$(cd "$WORK/sym-s1" && helper 'stix_slot_setup > /dev/null
    stix_flash_private' STIX_PORT_BASE=1 STIX_FLASH_MASTER="$M" \
    STIX_SHARED_DIRS="$SH")"
rc=$?
[ "$rc" -eq 2 ] && echo "$out" | grep -q "inside the shared flash dir"
check "B2: a dir resolving into the master's dir is refused (realpath)" $? \
    "rc=$rc: $out"
ln -sfn "$WORK/frozen" "$WORK/frz"
out="$(helper "stix_run_dir_guard '$WORK/frz/sub'; echo REACHED" \
    STIX_SHARED_DIRS="$SH")"
rc=$?
[ "$rc" -eq 2 ] && ! echo "$out" | grep -q REACHED
check "B2: legacy: below the frozen dir (via a symlink) is refused" $? \
    "rc=$rc: $out"
out="$(helper "stix_run_dir_guard '$WORK/master'; echo REACHED" \
    STIX_SHARED_DIRS="$SH")"
[ "$out" = REACHED ]
check "B2: legacy: the uprev master's dir stays allowed for boot gates" $? \
    "$out"
out="$(helper "stix_run_dir_guard '$WORK/master' strict; echo REACHED" \
    STIX_SHARED_DIRS="$SH")"
rc=$?
[ "$rc" -eq 2 ]
check "B2: strict: the uprev master's dir is refused" $? "rc=$rc: $out"
out="$(helper "stix_slot_setup > /dev/null
    stix_run_dir_guard '$WORK/master-s1'; echo REACHED" \
    STIX_PORT_BASE=1 STIX_SHARED_DIRS="$SH")"
[ "$out" = REACHED ]
check "B2: slot mode: <master dir>-s1 is its own dir, allowed" $? "$out"
[ "$(stat -c '%Y %i %b %s' "$M")" = "$before" ]
check "B1/B2: the synthetic master is untouched" $? "$(stat "$M")"


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

# --- review NITs: peek mode, the /mnt/c mountpoint, stix_slot_dir / ---
# peek (dry-run / check-guard / SKIP paths): no lock, no wait, no guard
hold 1
out="$(helper 'stix_slot_setup peek; echo "$STIX_SLOT $TMTC_PORT"' \
    STIX_PORT_BASE=auto STIX_DF_CMD="$WORK/df-low")"
rc=$?
[ "$rc" -eq 0 ] && [ "$(echo "$out" | tail -1)" = "2 $((UIDB + 200))" ] &&
    ! is_held 2
check "peek: auto skips a held slot, takes no lock, no disk guard" $? \
    "rc=$rc: $out"
hold 2
hold 3
t0=$SECONDS
out="$(helper 'stix_slot_setup peek; echo "$STIX_SLOT"' \
    STIX_PORT_BASE=auto STIX_SLOT_WAIT=30)"
rc=$?
[ "$rc" -eq 0 ] && [ $((SECONDS - t0)) -lt 5 ] &&
    [ "$(echo "$out" | tail -1)" = 1 ]
check "peek: all slots held => no wait (slot 1 shown)" $? "rc=$rc: $out"
out="$(helper 'stix_slot_setup peek; echo "$STIX_SLOT"' STIX_PORT_BASE=3)"
rc=$?
[ "$rc" -eq 0 ] && [ "$(echo "$out" | tail -1)" = 3 ]
check "peek: a held slot number is not refused" $? "rc=$rc: $out"
release_all
out="$(helper 'stix_slot_setup peek; echo REACHED' STIX_DF_CMD="$WORK/df-low")"
[ "$(echo "$out" | tail -1)" = REACHED ]
check "peek: legacy, no disk guard" $? "$out"
out="$(helper 'stix_slot_setup peek' STIX_PORT_BASE=auto STIX_SLOTS=9)"
rc=$?
[ "$rc" -eq 2 ]
check "peek: still refuses a bad STIX_SLOTS" $? "rc=$rc: $out"
# the disk guard refuses an unmounted /mnt/c instead of measuring /
mkdir -p "$WORK/not-a-mount"
out="$(helper 'stix_disk_guard; echo REACHED' STIX_MNT_C="$WORK/not-a-mount")"
rc=$?
[ "$rc" -eq 2 ] && echo "$out" | grep -q "not a mountpoint"
check "disk guard: /mnt/c present but not mounted => exit 2" $? \
    "rc=$rc: $out"
out="$(helper 'stix_disk_guard; echo REACHED' STIX_MNT_C="$WORK/no-such")"
[ "$out" = REACHED ]
check "disk guard: no /mnt/c at all (not WSL) => measures /tmp" $? "$out"
for d in / //; do
    out="$(helper "stix_slot_setup > /dev/null; stix_slot_dir '$d'
        echo REACHED" STIX_PORT_BASE=1)"
    rc=$?
    [ "$rc" -eq 2 ] && ! echo "$out" | grep -q REACHED
    check "stix_slot_dir '$d' is refused" $? "rc=$rc: $out"
done

# --- scripts/stix/qt.sh with a fake build and a fake suite ---
QT="$REPO_ROOT/scripts/stix/qt.sh"
# the fake suite reports what it sees: its slot base, whether that slot's
# lock (slot 0's outside a slot) is held, the QEMU binary and the CWD
{
    echo '#!/bin/sh'
    echo "l=\"$LOCKS/stix-qtest-\${STIX_QTEST_PORT_BASE:-$UIDB}.lock\""
    echo 'if flock -n "$l" true; then held=free; else held=held; fi'
    echo 'echo "ok 1 base=${STIX_QTEST_PORT_BASE:-unset} lock=$held" \'
    echo '    "qemu=$(basename "$QTEST_QEMU_BINARY") cwd=$(pwd) args=$*"'
    echo '[ ! -e flash.bin ] || echo "ok 2 flash=$(stat -c %s flash.bin)"'
} > "$WORK/fake-suite"
chmod +x "$WORK/fake-suite"
qt() { # args... -> qt.sh with the fake build and suite
    env -u STIX_PORT_BASE STIX_SLOT_LOCK_DIR="$LOCKS" \
        STIX_QT_MAKE="${STIX_QT_MAKE:-true}" \
        STIX_QT_SUITE="$WORK/fake-suite" STIX_FLASH_MASTER="$M" \
        STIX_QT_BASELINE_DIR="${STIX_QT_BASELINE_DIR:-$WORK/qt-base}" \
        bash "$QT" "$@" 2>&1
}
out="$(qt)"
rc=$?
[ "$rc" -eq 0 ] && echo "$out" | grep -q "^slot: 1 base $B1\$" &&
    echo "$out" | grep -q "^ok 1 base=$B1 lock=held qemu=qemu-system-sparc " &&
    echo "$out" | grep -q "^exit=0 slot=1 busy=0 load="
check "qt.sh: the suite runs in slot 1, STIX_QTEST_PORT_BASE exported" $? \
    "rc=$rc: $out"
out="$(qt flash-error-paths)"
echo "$out" | grep -q "args=--tap -p /sparc/sparc/stix/flash-error-paths\$" &&
    echo "$out" | grep -q "lock=held"
check "qt.sh <test>: one test, also under a slot" $? "$out"
hold 1
hold 2
hold 3
out="$(STIX_SLOT_WAIT=1 qt)"
rc=$?
[ "$rc" -eq 2 ] && ! echo "$out" | grep -q "^ok"
check "qt.sh: no free slot => exit 2, the suite does not run" $? \
    "rc=$rc: $out"
release_all
out="$(qt --baseline)"
rc=$?
[ "$rc" -eq 0 ] &&
    echo "$out" | grep -q "^slot: 0 base $UIDB (5.2 baseline)" &&
    echo "$out" | grep -q "^ok 1 base=unset lock=held" &&
    echo "$out" | grep -q "qemu=qemu-system-sparc-52-wrapper.sh" &&
    echo "$out" | grep -q "cwd=$WORK/qt-base " &&
    echo "$out" | grep -q "^ok 2 flash=8388608\$" &&
    [ "$(du -k "$WORK/qt-base/flash.bin" | cut -f1)" -lt 1024 ]
check "qt.sh --baseline: slot 0, the wrapper, a sparse flash in its dir" $? \
    "rc=$rc: $out"
# review S2: the 5.2 model reads the inverted master as all-0x00 flash, so
# --baseline runs only the non-flash filter unless --full is given
echo "$out" | grep -q \
    "args=--tap -p /sparc-52-wrapper.sh/sparc/stix/tmtc-hermetic\$"
check "S2: qt.sh --baseline runs only the non-flash filter by default" $? \
    "$out"
out="$(qt --baseline --full)"
rc=$?
[ "$rc" -eq 0 ] && echo "$out" | grep -q "^ok 1 .* args=--tap\$"
check "S2: qt.sh --baseline --full runs the whole suite" $? "rc=$rc: $out"
out="$(qt --full)"
rc=$?
[ "$rc" -eq 2 ] && ! echo "$out" | grep -q "^ok"
check "S2: --full without --baseline => exit 2" $? "rc=$rc: $out"
out="$(STIX_QT_BASELINE_DIR="$WORK/master" qt --baseline)"
rc=$?
[ "$rc" -eq 2 ] && ! echo "$out" | grep -q "^ok" &&
    [ "$(stat -c '%Y %i %b %s' "$M")" = "$before" ]
check "B2: qt.sh --baseline in the master's dir is refused" $? \
    "rc=$rc: $out"
out="$(STIX_QT_MAKE=false qt)"
rc=$?
[ "$rc" -eq 3 ] && echo "$out" | grep -q "BUILD FAILED"
check "qt.sh: a failed build => exit 3" $? "rc=$rc: $out"

echo "----------------------------------------"
echo "pass=$PASS fail=$FAIL"
[ "$FAIL" -eq 0 ]
