#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Offline test of run-container.sh (PL-7): a `docker` stub first on PATH
# records its argv. Checks the plan's static verification (nothing is
# built or run):
#   run: the argv has the 127.0.0.1 publishing, --add-host, the named
#     volume stix-run, the :ro /elf /config /scenarios mounts, the image
#     last, and the optional /idb only with --idb-dir; --config adds the
#     controller arg; --volume replaces the named volume
#   a /mnt/c volume is refused with exit 2 before docker is called
#   build: docker build -f Dockerfile -t stix-platform <repo root>, once;
#     the prune hygiene is printed, not run
#   --dry-run prints the same argv and calls no docker
#   the Dockerfile pins both FROM by digest, copies only
#     build/qemu-system-sparc (no make install), and the dockerignore
#     has **/flash.bin but no *.bin (pc-bios must stay); hadolint when
#     available; bash -n on the script
# Exit codes: 0 pass, 1 a check failed (2: a setup problem here).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLAT="$(cd "$HERE/.." && pwd)"
SCRIPT="$PLAT/run-container.sh"
DOCKERFILE="$PLAT/Dockerfile"
IGNORE="$PLAT/Dockerfile.dockerignore"
REPO="$(cd "$HERE/../../../.." && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
STUB="$T/stub"
mkdir -p "$STUB" "$T/elf" "$T/conf" "$T/scen" "$T/idb"
: > "$T/conf/run-gates.toml"    # the --config case needs the file
: > "$T/conf/interactive.toml"  # the default config
cat > "$STUB/docker" <<EOF
#!/usr/bin/env bash
printf '%s\n' "\$*" >> "$T/docker.log"
EOF
chmod +x "$STUB/docker"
export STIXPLAT_ELF_DIR="$T/elf" STIXPLAT_CONF_DIR="$T/conf" \
    STIXPLAT_SCEN_DIR="$T/scen" STIXPLAT_IDB_DIR=

fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }
run() { PATH="$STUB:$PATH" "$SCRIPT" "$@"; }    # never runs a real docker

check_run_argv() { # <argv line> <want_idb: yes|no>
    local line="$1" want_idb="$2"
    case "$line" in
    "run --rm --name stix-platform "*) ;;
    *) bad "$line: argv does not start with run --rm --name"; return ;;
    esac
    for want in "-p 127.0.0.1:8080:8080" "-p 127.0.0.1:21502:21502/udp" \
                "--add-host=host.docker.internal:host-gateway" \
                "-v stix-run:/var/lib/stix" \
                "-v $T/elf:/elf:ro" "-v $T/conf:/config:ro" \
                "-v $T/scen:/scenarios:ro"; do
        case " $line " in
        *" $want "*) ;;
        *) bad "argv lacks $want: $line" ;;
        esac
    done
    case "$line" in
    *":/idb:ro"*) [ "$want_idb" = yes ] || bad "argv mounts /idb unasked: $line" ;;
    *) [ "$want_idb" = no ] || bad "argv lacks the /idb mount: $line" ;;
    esac
    # the default: the interactive sample (the image CMD's
    # /config/platform.toml is not shipped)
    case "$line" in
    *" stix-platform --config /config/interactive.toml") ;;
    *) bad "the default does not end with the image and the interactive sample: $line" ;;
    esac
}

# -- run: the argv, with and without the stub ------------------------------
out="$(run --dry-run)"
case "$out" in
"docker "*) check_run_argv "${out#docker }" no ;;
*) bad "--dry-run: output does not start with docker: $out" ;;
esac
if [ -f "$T/docker.log" ]; then
    bad "--dry-run called docker"
fi
[ "$fail" -eq 0 ] && ok "--dry-run: the full docker argv, no docker called"

: > "$T/docker.log"
run > "$T/run.out"
line="$(head -1 "$T/docker.log")"
[ "$(wc -l < "$T/docker.log")" -eq 1 ] || bad "docker called more than once"
check_run_argv "$line" no
[ "$fail" -eq 0 ] && ok "run: the stub saw the plan's argv, image last"

: > "$T/docker.log"
run --idb-dir "$T/idb" > /dev/null
check_run_argv "$(head -1 "$T/docker.log")" yes
[ "$fail" -eq 0 ] && ok "run: --idb-dir adds -v ...:/idb:ro"

: > "$T/docker.log"
run --config run-gates.toml > /dev/null
line="$(head -1 "$T/docker.log")"
case "$line" in
*" stix-platform --config /config/run-gates.toml") ;;
*) bad "--config: argv ends with $line" ;;
esac
[ "$fail" -eq 0 ] && ok "run: --config overrides the image CMD"

: > "$T/docker.log"
run --volume "$T/vol" > /dev/null
line="$(head -1 "$T/docker.log")"
case " $line " in
*" -v $T/vol:/var/lib/stix "*) ;;
*) bad "--volume: argv lacks -v $T/vol:/var/lib/stix: $line" ;;
esac
case " $line " in
*" -v stix-run:"*) bad "--volume still names the volume: $line" ;;
esac
[ "$fail" -eq 0 ] && ok "run: --volume replaces the named volume"

# -- refusals: exit 2, docker never called ---------------------------------
refused() { # grep-pattern args...
    local pat="$1" rc=0
    shift
    : > "$T/docker.log"
    run "$@" > "$T/out" 2> "$T/err" || rc=$?
    if [ "$rc" -ne 2 ]; then
        bad "$*: exit $rc, want 2"
    elif [ -s "$T/docker.log" ]; then
        bad "$*: docker was called"
    elif ! grep -q "$pat" "$T/err"; then
        bad "$*: the refusal does not name it (want '$pat')"
    fi
}
refused /mnt/c --volume /mnt/c/stix-run
[ "$fail" -eq 0 ] && ok "refusal: a /mnt/c volume, exit 2, no docker call"
refused /nonexistent-dir-xyz --elf-dir /nonexistent-dir-xyz
[ "$fail" -eq 0 ] && ok "refusal: a missing bind dir, exit 2"

# -- build ------------------------------------------------------------------
: > "$T/docker.log"
run build > "$T/build.out"
line="$(head -1 "$T/docker.log")"
[ "$(wc -l < "$T/docker.log")" -eq 1 ] || bad "build: docker called more than once"
case "$line" in
"build -f $DOCKERFILE -t stix-platform $REPO") ;;
*) bad "build: argv is '$line'" ;;
esac
grep -q "docker builder prune -f" "$T/build.out" \
    || bad "build: the prune hygiene is not printed"
grep -q "docker image prune" "$T/build.out" \
    || bad "build: the image prune hint is not printed"
[ "$fail" -eq 0 ] && ok "build: one docker build of the repo root; hygiene only printed"
: > "$T/docker.log"
run build --dry-run > "$T/build.out"
[ -s "$T/docker.log" ] && bad "build --dry-run called docker"
case "$(head -1 "$T/build.out")" in
docker*" build -f $DOCKERFILE "*) ;;
*) bad "build --dry-run: output is '$(head -1 "$T/build.out")'" ;;
esac
[ "$fail" -eq 0 ] && ok "build --dry-run: the build argv printed, no docker called"

# -- the Dockerfile's static checks (the plan's no-build verification) ------
n="$(grep -c '^FROM ubuntu:24.04@sha256:' "$DOCKERFILE" || true)"
[ "$n" -eq 2 ] || bad "the Dockerfile has $n pinned FROM lines, want 2"
grep -q '^COPY --from=build /src/build/qemu-system-sparc /opt/qemu/bin/' \
    "$DOCKERFILE" || bad "the Dockerfile does not copy qemu-system-sparc alone"
if grep -v '^[[:space:]]*#' "$DOCKERFILE" | grep -qi "make install"; then
    bad "the Dockerfile runs make install"
fi
grep -q '^CMD \["--config", "/config/platform.toml"\]' "$DOCKERFILE" \
    || bad "the Dockerfile CMD is not --config /config/platform.toml"
[ "$fail" -eq 0 ] && ok "Dockerfile: both FROM digest-pinned, no make install"
# the probe: stixplat/probe_lib.py loads <stixplat>/../../tmtc-probe.py,
# so it must sit next to the platform dir's parent (/opt/stix/)
PLAT_DST="$(sed -n 's#^COPY scripts/stix/platform/ \(/[^ ]*\)/*$#\1#p' "$DOCKERFILE")"
PROBE_DST="$(sed -n 's#^COPY scripts/stix/tmtc-probe.py \(/[^ ]*\)$#\1#p' "$DOCKERFILE")"
if [ -n "$PLAT_DST" ] && [ "$PROBE_DST" = "$(dirname "$PLAT_DST")/tmtc-probe.py" ]; then
    ok "Dockerfile: the probe at $PROBE_DST, where probe_lib looks"
else
    bad "Dockerfile: the probe at '$PROBE_DST', probe_lib looks in" \
        "'$(dirname "${PLAT_DST:-?}")/tmtc-probe.py'"
fi
for want in "^\.git$" "^build\*/$" "^\*\*/flash\.bin$"; do
    grep -q "$want" "$IGNORE" || bad "the dockerignore lacks $want"
done
if grep -qx '\*.bin' "$IGNORE"; then
    bad "the dockerignore excludes *.bin (pc-bios must stay)"
fi
[ "$fail" -eq 0 ] && ok "dockerignore: flash.bin and build* out, pc-bios kept"
bash -n "$SCRIPT" || bad "bash -n: run-container.sh does not parse"
[ "$fail" -eq 0 ] && ok "bash -n: run-container.sh parses"
if command -v hadolint > /dev/null 2>&1; then
    if hadolint "$DOCKERFILE"; then
        ok "hadolint: clean"
    else
        bad "hadolint: findings above"
    fi
else
    echo "ok   hadolint absent: the Dockerfile goes to review unlinted"
fi

if [ "$fail" -eq 0 ]; then
    echo "RUN-CONTAINER: PASS"
else
    echo "RUN-CONTAINER: FAIL"
fi
exit "$fail"
