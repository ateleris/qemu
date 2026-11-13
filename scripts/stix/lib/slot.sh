# shellcheck shell=bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Slot helper for the STIX gate scripts (gate-ports plan D-G1..D-G7,
# docs/stix-port/2026-11-11-gateports-plan.md). Sourced, never run.
#
# A slot is one lock file, one port band and one run-dir suffix; holding
# the slot lock owns all three. Slot k (k >= 0) uses the band
#   B_k = 20000 + uid + 100*k .. B_k + 99
# and the lock <STIX_SLOT_LOCK_DIR>/stix-qtest-<B_k>.lock. Within a band,
# B+0 is the TMTC TCP port, B+1/B+2 are reserved for UDP TM/TC and
# B+3..B+99 are for the qtest suite's QEMU-bound ports.
#
# STIX_PORT_BASE selects the mode:
#   unset/empty  legacy (slot 0): port 20000+uid, no lock taken (the
#                caller holds /tmp/stix-qtest-21000.lock by convention),
#                nothing printed, nothing exported, no run-dir suffix
#   auto         the first free slot in 1..STIX_SLOTS (default 3), polled
#                once a second for at most STIX_SLOT_WAIT seconds (1800)
#   k >= 1       exactly slot k; exit 2 if its lock is held
# Slot 0 is never taken here: it is legacy, manual and 5.2-wrapper use.
#
# In slot mode the helper exports STIX_SLOT, TMTC_PORT (= B_k) and
# STIX_QTEST_PORT_BASE (= B_k, read by tests/qtest/stix.c for its port
# band); STIX_PORT_BASE keeps the caller's selector (auto or k). It also
# sets STIX_SLOT_QEMU_ARGS (the slot's -global tmtc-port plus the P0b
# tm-hold) for the caller's QEMU command line, and prints one
# "slot: k base B" line, and keeps the lock fd open
# for the caller's lifetime (inherited by QEMU and the probe, so the
# kernel frees it when the last of them exits, SIGKILL included).
#
# Test hooks: STIX_SLOT_LOCK_DIR (default /tmp), STIX_FLASH_MASTER
# (default $STIX_UPREV_DIR/flash.bin, see lib/paths.sh), STIX_FLASH_MAX_ALLOC_KB
# (65536), STIX_CP_CMD (default "cp --sparse=always"), STIX_DF_CMD (the
# df command whose last line's last field is the free KiB),
# STIX_SHARED_DIRS (default "$STIX_UPREV_DIR $STIX_LEGACY_DIRS": the uprev
# master's dir first, then the old /tmp master dir and the frozen baseline's).

. "$(dirname "${BASH_SOURCE[0]}")/paths.sh"

STIX_SLOT=0
STIX_SLOT_MODE=0
STIX_SLOT_TAG=""
STIX_SLOT_QEMU_ARGS=()
STIX_SLOT_FD=""

stix_slot_base() { # k -> B_k
    echo $((20000 + $(id -u) + 100 * $1))
}

stix_slot_lock() { # k -> lock file path
    echo "${STIX_SLOT_LOCK_DIR:-/tmp}/stix-qtest-$(stix_slot_base "$1").lock"
}

# Refuse any slot count whose bands would overlap the GS ports or pass
# port 65535.
stix_slot_check_gs() { # highest slot index in use
    local k b p
    for ((k = 1; k <= $1; k++)); do
        b="$(stix_slot_base "$k")"
        if [ $((b + 99)) -gt 65535 ]; then
            echo "slot: slot $k band $b..$((b + 99)) passes port 65535"
            return 2
        fi
        for p in "${STIX_GS_TM_PORT:-21500}" "${STIX_GS_UDP_TM_PORT:-21501}" \
                 "${STIX_GS_UDP_TC_PORT:-21502}"; do
            if [ "$p" -ge "$b" ] && [ "$p" -le $((b + 99)) ]; then
                echo "slot: slot $k band $b..$((b + 99)) overlaps the GS" \
                    "port $p; lower STIX_SLOTS or move the STIX_GS_* ports"
                return 2
            fi
        done
    done
}

# 0 if slot k's lock is held by anyone, read from /proc/locks without
# taking (or probing) the lock.
stix_slot_held() { # k
    local f id t held
    f="$(stix_slot_lock "$1")"
    [ -e "$f" ] || return 1
    held=" $(awk '$2 == "FLOCK" { printf "%s ", $6 }' /proc/locks 2>/dev/null)"
    # /proc/locks names a file as MAJOR:MINOR:INODE, the device numbers in
    # %02x (fs/locks.c); stat %d is the encoded st_dev
    read -r -a t <<< "$(stat -L -c '%d %i' "$f" 2>/dev/null)" || return 1
    [ "${#t[@]}" -eq 2 ] || return 1
    id="$(printf '%02x:%02x:%s' $(((t[0] >> 8) & 0xfff)) \
        $(((t[0] & 0xff) | ((t[0] >> 12) & 0xfff00))) "${t[1]}")"
    case "$held" in *" $id "*) return 0 ;; esac
    # stix_slot_try's lock is taken by flock(1), which exits at once: the
    # kernel then reports the owner as pid 0 and leaves the lock out of
    # /proc/locks, but the holder's fdinfo still lists it
    grep -qs -- " $id " /proc/[0-9]*/fdinfo/* 2>/dev/null
}

# The number of held slot locks in 0..STIX_SLOTS other than our own.
stix_slot_busy() {
    local n=0 k
    for ((k = 0; k <= ${STIX_SLOTS:-3}; k++)); do
        [ "$k" -ne "$STIX_SLOT" ] || [ "$STIX_SLOT_MODE" -eq 0 ] || continue
        if stix_slot_held "$k"; then
            n=$((n + 1))
        fi
    done
    echo "$n"
}

stix_slot_try() { # k -> 0 if its lock is now held on STIX_SLOT_FD
    local f fd
    f="$(stix_slot_lock "$1")"
    exec {fd}>>"$f" || return 1
    if flock -n "$fd"; then
        STIX_SLOT_FD="$fd"
        return 0
    fi
    exec {fd}>&-
    return 1
}

# The free space guard (D-G7): the real limit is the Windows host C:.
stix_disk_guard() {
    local min_gb="${STIX_DISK_MIN_GB:-3}" df_cmd avail
    local mnt="${STIX_MNT_C:-/mnt/c}"
    if [ -n "${STIX_DF_CMD:-}" ]; then
        df_cmd="$STIX_DF_CMD"
    elif [ -e "$mnt" ]; then
        # an unmounted /mnt/c would measure / (the VHDX), not C:
        if ! mountpoint -q "$mnt"; then
            echo "disk guard: $mnt is not a mountpoint; cannot see the" \
                "Windows disk's free space"
            exit 2
        fi
        df_cmd="df --output=avail $mnt"
    else
        df_cmd="df --output=avail /tmp"
    fi
    avail="$($df_cmd 2>/dev/null | tail -1 | awk '{ print $NF }')"
    case "$avail" in
        '' | *[!0-9]*)
            echo "disk guard: cannot read the free space ($df_cmd)"
            exit 2 ;;
    esac
    if [ "$avail" -lt $((min_gb * 1024 * 1024)) ]; then
        echo "disk guard: only $((avail / 1024)) MiB free ($df_cmd), below" \
            "STIX_DISK_MIN_GB=$min_gb GiB; refusing to start QEMU"
        exit 2
    fi
}

# Parse STIX_PORT_BASE, take the slot and set TMTC_PORT. Runs the disk
# guard in every mode. Exits 2 on any refusal.
#
# "peek" is for paths that boot nothing (--dry-run, --check-guard, the
# ELF-absent SKIP): it sets the same variables but takes no lock, never
# waits and skips the disk guard. auto shows the first slot not held
# right now (slot 1 when all are), a number is shown even when held.
stix_slot_setup() { # [peek]
    local req="${STIX_PORT_BASE:-}" n="${STIX_SLOTS:-3}" k waited
    local peek=0
    [ "${1:-}" != peek ] || peek=1
    [ "$peek" -eq 1 ] || stix_disk_guard
    if [ -z "$req" ]; then
        TMTC_PORT=$((20000 + $(id -u)))
        return 0
    fi
    case "$n" in
        '' | *[!0-9]* | 0)
            echo "slot: STIX_SLOTS must be a positive integer (got: $n)"
            exit 2 ;;
    esac
    if [ "$req" = auto ] && [ "$peek" -eq 1 ]; then
        stix_slot_check_gs "$n" || exit 2
        for ((k = 1; k <= n; k++)); do
            stix_slot_held "$k" || break
        done
        [ "$k" -le "$n" ] || k=1
    elif [ "$req" = auto ]; then
        stix_slot_check_gs "$n" || exit 2
        waited=0
        while :; do
            for ((k = 1; k <= n; k++)); do
                if stix_slot_try "$k"; then
                    break 2
                fi
            done
            if [ "$waited" -ge "${STIX_SLOT_WAIT:-1800}" ]; then
                echo "slot: all $n slots held for ${waited}s" \
                    "(locks $(stix_slot_lock 1) .. $(stix_slot_lock "$n"))"
                exit 2
            fi
            # a wait is visible, not only a longer run time (gate V6)
            [ "$waited" -ne 0 ] || echo "slot: all $n busy, waiting up to" \
                "${STIX_SLOT_WAIT:-1800}s for a free one"
            sleep 1
            waited=$((waited + 1))
        done
        [ "$waited" -eq 0 ] || echo "slot: waited ${waited}s"
    else
        case "$req" in
            '' | *[!0-9]* | 0*)
                echo "slot: STIX_PORT_BASE must be unset, 'auto' or a slot" \
                    "number >= 1 (got: $req); slot 0 is legacy"
                exit 2 ;;
        esac
        k="$req"
        stix_slot_check_gs "$k" || exit 2
        if [ "$peek" -eq 0 ] && ! stix_slot_try "$k"; then
            echo "slot: slot $k is held ($(stix_slot_lock "$k"))"
            exit 2
        fi
    fi
    STIX_SLOT="$k"
    STIX_SLOT_MODE=1
    TMTC_PORT="$(stix_slot_base "$k")"
    STIX_QTEST_PORT_BASE="$TMTC_PORT"
    export STIX_SLOT TMTC_PORT STIX_QTEST_PORT_BASE
    # P0b (gate speed-up plan): every slot-mode boot holds TM until the
    # first probe connect, so the boot TM can never race the probe.
    # Legacy mode boots nothing gated; the array stays empty there.
    STIX_SLOT_QEMU_ARGS=(-global "grlib-stixspw.tmtc-port=$TMTC_PORT" \
                         -global "grlib-stixspw.tm-hold=on")
    # D-G6: appended to every verdict line in slot mode
    STIX_SLOT_TAG=" slot=$k busy=$(stix_slot_busy)"
    STIX_SLOT_TAG="$STIX_SLOT_TAG load=$(cut -d' ' -f1 /proc/loadavg)"
    echo "slot: $k base $TMTC_PORT"
}

# The run dir for this mode: unchanged in legacy, <dir>-s<k> in slot mode.
stix_slot_dir() { # dir
    if [ "$(realpath -m -- "$1")" = / ]; then
        echo "slot: refusing run dir / ($1)" >&2
        exit 2
    fi
    if [ "$STIX_SLOT_MODE" -eq 1 ]; then
        echo "${1%/}-s$STIX_SLOT"
    else
        echo "$1"
    fi
}

# Slot mode only: refresh ./flash.bin as a private copy of the master.
stix_flash_private() {
    [ "$STIX_SLOT_MODE" -eq 1 ] || return 0
    stix_flash_copy
}

# Refuse a run dir that is, or is inside, a shared flash dir (after
# resolving symlinks): the frozen baseline dir always, the uprev master's
# dir in slot mode or when "strict" is given. Legacy boot gates run in the
# uprev dir by design; slot mode and copies never do. Also refuses /.
stix_run_dir_guard() { # dir [strict]
    local d shared first=1
    d="$(realpath -m -- "$1")"
    if [ "$d" = / ]; then
        echo "slot: refusing run dir / ($1)"
        exit 2
    fi
    for shared in ${STIX_SHARED_DIRS:-$STIX_UPREV_DIR $STIX_LEGACY_DIRS}; do
        if [ "$first" -eq 1 ] && [ "$STIX_SLOT_MODE" -eq 0 ] &&
                [ "${2:-}" != strict ]; then
            first=0
            continue
        fi
        first=0
        shared="$(realpath -m -- "$shared")"
        case "$d" in
            "$shared" | "$shared"/*)
                echo "slot: refusing run dir $1 (resolves to $d, at or" \
                    "inside the shared flash dir $shared)"
                exit 2 ;;
        esac
    done
}

# Refresh ./flash.bin as a private sparse copy of the master. Every guard
# runs before anything is written:
# - the master must exist (a missing one would make QEMU create a fresh
#   flash and take the first-boot write path);
# - the CWD must not be a shared flash dir, and ./flash.bin must not be
#   the master itself (a symlinked dir or file);
# - the CWD must be a local Linux filesystem that keeps holes (not under
#   /mnt, i.e. not the Windows disk);
# - the master's own allocation must be below STIX_FLASH_MAX_ALLOC_KB, so
#   a dense master (the frozen baseline is 16.5 GB allocated) is never
#   copied.
# After the copy, the copy must be sparse (allocated below the cap) and
# its apparent size the master's: a second line of defence.
stix_flash_copy() {
    local master="${STIX_FLASH_MASTER:-$STIX_UPREV_DIR/flash.bin}"
    local max_kb="${STIX_FLASH_MAX_ALLOC_KB:-65536}" alloc size msize
    local here fstype malloc
    if [ ! -f "$master" ]; then
        echo "slot: the master flash $master is missing;" \
            "run capture-golden.sh first"
        exit 2
    fi
    stix_run_dir_guard . strict
    if [ -e flash.bin ] && [ flash.bin -ef "$master" ]; then
        echo "slot: refusing the flash copy: ./flash.bin is the master" \
            "$master itself"
        exit 2
    fi
    here="$(realpath .)"
    case "$here" in
        /mnt | /mnt/*)
            echo "slot: refusing the flash copy into $here (under /mnt:" \
                "the Windows disk keeps no holes)"
            exit 2 ;;
    esac
    fstype="$(stat -f -c %T .)"
    case "$fstype" in
        ext2/ext3 | ext4 | tmpfs | btrfs | xfs) ;;
        *)
            echo "slot: refusing the flash copy into $here (filesystem" \
                "$fstype; only ext2/3/4, tmpfs, btrfs or xfs keep holes)"
            exit 2 ;;
    esac
    malloc="$(du -kL -- "$master" | cut -f1)"
    if [ "$malloc" -ge "$max_kb" ]; then
        echo "slot: refusing to copy the master $master: it allocates" \
            "${malloc} KiB, limit ${max_kb} KiB (a dense flash)"
        exit 2
    fi
    stix_disk_guard
    rm -f flash.bin
    # shellcheck disable=SC2086 # STIX_CP_CMD is a command line
    if ! flock -s -w 30 "$(dirname "$master")/.flash.lock" timeout 30 \
            ${STIX_CP_CMD:-cp --sparse=always} "$master" flash.bin; then
        rm -f flash.bin
        echo "slot: copying the master flash $master failed or timed" \
            "out"
        exit 2
    fi
    alloc="$(du -k flash.bin | cut -f1)"
    size="$(stat -c %s flash.bin)"
    msize="$(stat -L -c %s "$master")"
    if [ "$size" -ne "$msize" ] || [ "$alloc" -ge "$max_kb" ]; then
        rm -f flash.bin
        echo "slot: refusing the flash copy (allocated ${alloc} KiB, limit" \
            "${max_kb} KiB; apparent $size B, master $msize B)"
        exit 2
    fi
}

# Slot mode only, after a PASS (rc 0): remove the large files of the run
# (flash.bin, trace logs); a FAIL/ERROR keeps everything for diagnosis.
stix_slot_cleanup() { # rc file...
    local rc="$1"
    shift
    [ "$STIX_SLOT_MODE" -eq 1 ] && [ "$rc" -eq 0 ] || return 0
    rm -f -- "$@"
}
