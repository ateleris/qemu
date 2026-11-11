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
# In slot mode the helper exports STIX_SLOT, STIX_PORT_BASE (= B_k) and
# TMTC_PORT, prints one "slot: k base B" line, and keeps the lock fd open
# for the caller's lifetime (inherited by QEMU and the probe, so the
# kernel frees it when the last of them exits, SIGKILL included).
#
# Test hooks: STIX_SLOT_LOCK_DIR (default /tmp), STIX_FLASH_MASTER
# (default /tmp/stix-uprev-run/flash.bin), STIX_FLASH_MAX_ALLOC_KB
# (65536), STIX_CP_CMD (default "cp --sparse=always"), STIX_DF_CMD (the
# df command whose last line's last field is the free KiB).

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

# Refuse any slot count whose bands would overlap the GS ports.
stix_slot_check_gs() { # highest slot index in use
    local k b p
    for ((k = 1; k <= $1; k++)); do
        b="$(stix_slot_base "$k")"
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

# The number of held slot locks in 0..STIX_SLOTS other than our own, read
# from /proc/locks without taking (or probing) any lock.
stix_slot_busy() {
    local n=0 k f id t
    local held
    held=" $(awk '$2 == "FLOCK" { printf "%s ", $6 }' /proc/locks 2>/dev/null)"
    for ((k = 0; k <= ${STIX_SLOTS:-3}; k++)); do
        [ "$k" -ne "$STIX_SLOT" ] || [ "$STIX_SLOT_MODE" -eq 0 ] || continue
        f="$(stix_slot_lock "$k")"
        [ -e "$f" ] || continue
        # /proc/locks names a file as MAJOR:MINOR:INODE, the device
        # numbers in %02x (fs/locks.c); stat %d is the encoded st_dev
        read -r -a t <<< "$(stat -L -c '%d %i' "$f" 2>/dev/null)" || continue
        [ "${#t[@]}" -eq 2 ] || continue
        id="$(printf '%02x:%02x:%s' $(((t[0] >> 8) & 0xfff)) \
            $(((t[0] & 0xff) | ((t[0] >> 12) & 0xfff00))) "${t[1]}")"
        case "$held" in *" $id "*) n=$((n + 1)) ;; esac
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
    if [ -n "${STIX_DF_CMD:-}" ]; then
        df_cmd="$STIX_DF_CMD"
    elif [ -d /mnt/c ]; then
        df_cmd="df --output=avail /mnt/c"
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
stix_slot_setup() {
    local req="${STIX_PORT_BASE:-}" n="${STIX_SLOTS:-3}" k waited
    stix_disk_guard
    if [ -z "$req" ]; then
        TMTC_PORT=$((20000 + $(id -u)))
        return 0
    fi
    case "$n" in
        '' | *[!0-9]* | 0)
            echo "slot: STIX_SLOTS must be a positive integer (got: $n)"
            exit 2 ;;
    esac
    if [ "$req" = auto ]; then
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
            sleep 1
            waited=$((waited + 1))
        done
    else
        case "$req" in
            '' | *[!0-9]* | 0*)
                echo "slot: STIX_PORT_BASE must be unset, 'auto' or a slot" \
                    "number >= 1 (got: $req); slot 0 is legacy"
                exit 2 ;;
        esac
        k="$req"
        stix_slot_check_gs "$k" || exit 2
        if ! stix_slot_try "$k"; then
            echo "slot: slot $k is held ($(stix_slot_lock "$k"))"
            exit 2
        fi
    fi
    STIX_SLOT="$k"
    STIX_SLOT_MODE=1
    STIX_PORT_BASE="$(stix_slot_base "$k")"
    TMTC_PORT="$STIX_PORT_BASE"
    export STIX_SLOT STIX_PORT_BASE TMTC_PORT
    STIX_SLOT_QEMU_ARGS=(-global "grlib-stixspw.tmtc-port=$TMTC_PORT")
    # D-G6: appended to every verdict line in slot mode
    STIX_SLOT_TAG=" slot=$k busy=$(stix_slot_busy)"
    STIX_SLOT_TAG="$STIX_SLOT_TAG load=$(cut -d' ' -f1 /proc/loadavg)"
    echo "slot: $k base $STIX_PORT_BASE"
}

# The run dir for this mode: unchanged in legacy, <dir>-s<k> in slot mode.
stix_slot_dir() { # dir
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

# Refresh ./flash.bin as a private sparse copy of the master. The master
# must exist (a missing one would make QEMU create a fresh flash and take
# the first-boot write path). The copy must stay sparse: allocated <
# STIX_FLASH_MAX_ALLOC_KB and apparent == master's.
stix_flash_copy() {
    local master="${STIX_FLASH_MASTER:-/tmp/stix-uprev-run/flash.bin}"
    local max_kb="${STIX_FLASH_MAX_ALLOC_KB:-65536}" alloc size msize
    if [ ! -f "$master" ]; then
        echo "slot: the master flash $master is missing;" \
            "run capture-golden.sh first"
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
