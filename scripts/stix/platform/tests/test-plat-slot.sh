#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Offline test of the platform gates' slot use (plat-slot.sh, §41):
# every gate's --dry-run, in legacy mode, prints its historic fixed ports
# and run dir; with STIX_PORT_BASE=1 and =2 every port lies in that
# slot's band B+1..B+99 (never B+0, the TMTC TCP port), the five are
# distinct, and the run dir has the -s<k> suffix. --dry-run only peeks:
# it takes no lock, so it also works while the slot is held. Nothing
# boots; a temp lock dir and a fake df keep it hermetic.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
export STIX_SLOT_LOCK_DIR="$T" STIX_DF_CMD="echo 999999999"
export STIX_ASW_ELF=/dev/null
QEMU=/bin/true
fail=0

field() { sed -n "s/.* $1=\([^ ]*\).*/\1/p" <<< " $2"; }

check() { # gate legacy-line slot
    local g="$1" want="$2" k out b p ports
    out="$(env -u STIX_PORT_BASE "$HERE/$g.sh" --dry-run "$QEMU")"
    if [ "$out" != "$want" ]; then
        echo "FAIL $g legacy: got '$out', want '$want'"
        fail=1
    fi
    for k in 1 2; do
        out="$(STIX_PORT_BASE=$k "$HERE/$g.sh" --dry-run "$QEMU" | tail -1)"
        b=$((20000 + $(id -u) + 100 * k))
        ports=()
        for p in api qemu_tm qemu_tc gs_tc sink; do
            ports+=("$(field "$p" "$out")")
        done
        for p in "${ports[@]}"; do
            if [ -z "$p" ] || [ "$p" -le "$b" ] || [ "$p" -gt $((b + 99)) ]
            then
                echo "FAIL $g slot $k: port '$p' outside" \
                    "$((b + 1))..$((b + 99)): $out"
                fail=1
            fi
        done
        if [ "$(printf '%s\n' "${ports[@]}" | sort -u | wc -l)" -ne 5 ]; then
            echo "FAIL $g slot $k: ports not distinct: $out"
            fail=1
        fi
        if [ "$(field run "$out")" != "/tmp/stix-plat/$g-s$k" ] ||
                [ "$(field slot "$out")" != "$k" ]; then
            echo "FAIL $g slot $k: run dir or slot: $out"
            fail=1
        fi
    done
    [ "$fail" -ne 0 ] || echo "ok   $g: legacy ports kept, slots 1/2 in band"
}

legacy() { # gate api tm tc gs sink
    echo "slot=0 run=/tmp/stix-plat/$1 api=$2 qemu_tm=$3 qemu_tc=$4" \
        "gs_tc=$5 sink=$6"
}
check core-smoke "$(legacy core-smoke 21520 21521 21522 21523 21526)"
check link-smoke "$(legacy link-smoke 21525 21526 21527 21528 21531)"
check run-smoke "$(legacy run-smoke 21535 21536 21537 21538 21541)"
if [ "$fail" -eq 0 ]; then echo "PLAT-SLOT: PASS"; else echo "PLAT-SLOT: FAIL"; fi
exit "$fail"
