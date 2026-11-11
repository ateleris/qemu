# shellcheck shell=bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The platform gates' ports and run dir (gate-ports §41, lib/slot.sh).
# Sourced by core-smoke.sh, link-smoke.sh and run-smoke.sh, never run.
#
# plat_slot_setup GATE LEGACY_BASE [peek]
#   STIX_PORT_BASE unset: legacy (slot 0). Ports LEGACY_BASE+0.. (the
#     fixed ports these gates always used), run dir /tmp/stix-plat/GATE,
#     the caller holds /tmp/stix-qtest-21000.lock.
#   auto or k >= 1: the slot's band B = 20000 + uid + 100k. The platform
#     QEMU runs with tmtc-server=off, so B+0 (the TMTC TCP port) stays
#     unused; its UDP TM/TC take the band's reserved B+1/B+2, the
#     controller's API B+3, the GS TC port B+4 and the fake GS or TM sink
#     B+5. The run dir is /tmp/stix-plat/GATE-s<k>; the slot lock is held
#     until the gate exits.
# It sets PLAT_RUN, PLAT_API, PLAT_QEMU_TM, PLAT_QEMU_TC, PLAT_GS_TC and
# PLAT_SINK, and exports them (link-smoke's Python reads them).
#
# The platform gates never boot the shared master: the controller runs
# each boot on the run dir's own fresh flash.bin, which QEMU makes
# sparse after the controller's binary guard (D-PLl), so there is no
# flash to copy (stix_flash_private is not used).

# shellcheck source=../../lib/slot.sh
. "$(dirname "${BASH_SOURCE[0]}")/../../lib/slot.sh"

plat_slot_setup() { # gate legacy_base [peek]
    local gate="$1" legacy="$2" b
    stix_slot_setup ${3:+"$3"}
    if [ "$STIX_SLOT_MODE" -eq 1 ]; then
        b="$TMTC_PORT"
        PLAT_QEMU_TM=$((b + 1))
        PLAT_QEMU_TC=$((b + 2))
        PLAT_API=$((b + 3))
        PLAT_GS_TC=$((b + 4))
        PLAT_SINK=$((b + 5))
    else
        PLAT_API=$legacy
        PLAT_QEMU_TM=$((legacy + 1))
        PLAT_QEMU_TC=$((legacy + 2))
        PLAT_GS_TC=$((legacy + 3))
        PLAT_SINK=$((legacy + 6))
    fi
    PLAT_RUN="$(stix_slot_dir "/tmp/stix-plat/$gate")"
    export PLAT_RUN PLAT_API PLAT_QEMU_TM PLAT_QEMU_TC PLAT_GS_TC PLAT_SINK
}

# exit 2 if one of the gate's ports is already bound
plat_ports_free() {
    local p
    for p in "$PLAT_API" "$PLAT_QEMU_TM" "$PLAT_QEMU_TC" "$PLAT_GS_TC" \
             "$PLAT_SINK"; do
        if ss -lntu | grep -q ":$p "; then
            echo "port $p busy"
            exit 2
        fi
    done
}

# --dry-run: what the gate would use, nothing started
plat_print_ports() {
    echo "slot=$STIX_SLOT run=$PLAT_RUN api=$PLAT_API qemu_tm=$PLAT_QEMU_TM" \
        "qemu_tc=$PLAT_QEMU_TC gs_tc=$PLAT_GS_TC sink=$PLAT_SINK"
}
