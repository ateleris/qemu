#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# run-container.sh: the only documented way to run the PL-7 container
# (docs/stix-port/platform.md, "Usage"). It builds the image and runs it
# with the API (8080/tcp) and the GS TC port (21502/udp) published on
# 127.0.0.1 only (D-PLj), the named volume stix-run at /var/lib/stix,
# and read-only /elf, /config, /scenarios and, with --idb-dir, /idb.
#
# Usage:
#   run-container.sh [--dry-run] [build [-- docker-build-arg...]]
#       [--volume DIR] [--elf-dir DIR] [--conf-dir DIR] [--scen-dir DIR]
#       [--idb-dir DIR] [--config FILE.toml] [--name NAME] [--image NAME]
#       [--volume-name NAME] [--detach]
#       [-- ARGS...]                      # extra controller args
#
# Without --config it runs the interactive sample (interactive.toml in
# the config dir); another config in that dir with --config, e.g.
#   run-container.sh --config run-gates.toml
# --volume-name NAME uses another named volume, --detach runs it in the
# background (docker run -d; stop it with docker stop).
#
# The flash volume is the named volume stix-run, or a Linux bind mount
# (--volume DIR). Never /mnt/c (drvfs keeps no sparse holes): the
# controller's sparse check refuses it, and this script refuses it
# before docker runs.
#
# Exit codes: 0 ok (dry-run: printed the argv), 1 docker failed,
# 2 usage or environment problem (docker missing, a dir missing, a
# /mnt/c volume).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/paths.sh
. "$HERE/../lib/paths.sh"    # STIX_ELF_DIR (the ELF dir default)

DRY=0
IMAGE=stix-platform
NAME=stix-platform
VOLUME=             # empty: the named volume (VOLNAME)
VOLNAME=stix-run    # --volume-name: another named volume
DETACH=0            # --detach: docker run -d
ELF_DIR="${STIXPLAT_ELF_DIR:-$STIX_ELF_DIR}"
CONF_DIR="${STIXPLAT_CONF_DIR:-$HERE}"          # the samples live here
SCEN_DIR="${STIXPLAT_SCEN_DIR:-$HERE/scenarios}"  # the gate scenarios
IDB_DIR="${STIXPLAT_IDB_DIR:-}"
CONFIG=interactive.toml   # a file in CONF_DIR (the image CMD names
                          # /config/platform.toml, which the samples lack)
ACTION=run
declare -a EXTRA=()   # after -- : docker build args, or controller args

usage() {
    # the leading comment block (the shebang dropped, the "# " stripped)
    awk 'NR == 1 { next } /^#/ { print; next } { exit }' \
        "$HERE/run-container.sh" | sed 's/^# \{0,1\}//'
}

die() {
    echo "run-container.sh: $*" >&2
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
    --dry-run) DRY=1 ;;
    --image) IMAGE="${2:?--image needs a value}"; shift ;;
    --name) NAME="${2:?--name needs a value}"; shift ;;
    --volume) VOLUME="${2:?--volume needs a value}"; shift ;;
    --volume-name) VOLNAME="${2:?--volume-name needs a value}"; shift ;;
    --detach) DETACH=1 ;;
    --elf-dir) ELF_DIR="${2:?--elf-dir needs a value}"; shift ;;
    --conf-dir) CONF_DIR="${2:?--conf-dir needs a value}"; shift ;;
    --scen-dir) SCEN_DIR="${2:?--scen-dir needs a value}"; shift ;;
    --idb-dir) IDB_DIR="${2:?--idb-dir needs a value}"; shift ;;
    --config) CONFIG="${2:?--config needs a value}"; shift ;;
    build) ACTION=build ;;
    --) shift; EXTRA=("$@"); break ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; die "unknown option $1" ;;
    esac
    shift
done

# -- checks ----------------------------------------------------------------

if [ "$VOLUME" != "" ]; then
    case "$VOLUME" in
    /*) ;;
    *) die "--volume $VOLUME must be an absolute path (a bind mount)" ;;
    esac
    case "$VOLUME" in
    /mnt/*) die "volume $VOLUME is under /mnt (e.g. /mnt/c, drvfs): it \
keeps no sparse holes and the controller's sparse check would refuse \
it; use the named volume (default) or a Linux ext4/xfs bind mount" ;;
    esac
fi
if [ "$ACTION" = build ]; then
    [ -f "$HERE/Dockerfile" ] || die "missing $HERE/Dockerfile"
else
    if [ "$DRY" = 0 ] && ! command -v docker > /dev/null 2>&1; then
        die "docker not found on PATH"
    fi
    for d in "$ELF_DIR" "$CONF_DIR" "$SCEN_DIR"; do
        case "$d" in /*) ;; *) die "bind dir $d must be absolute" ;; esac
        [ -d "$d" ] || die "bind dir $d does not exist"
    done
    [ "$IDB_DIR" = "" ] || [ -d "$IDB_DIR" ] \
        || die "--idb-dir $IDB_DIR does not exist"
    [ "$CONFIG" = "" ] || [ -f "$CONF_DIR/$CONFIG" ] \
        || die "no config $CONF_DIR/$CONFIG"
    for d in "$ELF_DIR" "$CONF_DIR" "$SCEN_DIR" "$IDB_DIR"; do
        [ -n "$d" ] || continue
        case "$d" in
        /mnt/*) echo "warning: bind dir $d is under /mnt (drvfs): \
reads are slow" >&2 ;;
        esac
    done
fi

# -- the argvs -------------------------------------------------------------

print_argv() {
    printf ' %q' "$@"
    printf '\n'
}

if [ "$ACTION" = build ]; then
    ROOT="$(cd "$HERE/../../.." && pwd)"
    set -- build -f "$HERE/Dockerfile" -t "$IMAGE"
    [ ${#EXTRA[@]} -eq 0 ] || set -- "$@" "${EXTRA[@]}"
    set -- "$@" "$ROOT"
    if [ "$DRY" = 1 ]; then
        printf 'docker'; print_argv "$@"
        exit 0
    fi
    docker "$@"
    # D-PLu post-build hygiene: printed, never run without asking.
    cat <<'EOT'
Post-build hygiene (run these only if you choose to):
  docker builder prune -f      # the cached builder layers
  docker image prune -f
  The VHDXs never shrink on their own (D-PLu): on Windows, wsl
  --shutdown, then Optimize-VHD or "diskpart compact vdisk" on the
  Docker Desktop and WSL VHDXs, or the freed space stays used.
EOT
    exit 0
fi

set -- run --rm --name "$NAME"
[ "$DETACH" = 0 ] || set -- "$@" -d
set -- "$@" -p 127.0.0.1:8080:8080 -p 127.0.0.1:21502:21502/udp
set -- "$@" --add-host=host.docker.internal:host-gateway
set -- "$@" -v "${VOLUME:-$VOLNAME}:/var/lib/stix"
set -- "$@" -v "$ELF_DIR:/elf:ro" -v "$CONF_DIR:/config:ro" \
            -v "$SCEN_DIR:/scenarios:ro"
[ "$IDB_DIR" = "" ] || set -- "$@" -v "$IDB_DIR:/idb:ro"
set -- "$@" "$IMAGE"
[ "$CONFIG" = "" ] || set -- "$@" --config "/config/$CONFIG"
[ ${#EXTRA[@]} -eq 0 ] || set -- "$@" "${EXTRA[@]}"

if [ "$DRY" = 1 ]; then
    printf 'docker'; print_argv "$@"
    exit 0
fi
exec docker "$@"
