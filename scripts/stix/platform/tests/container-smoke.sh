#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# container-smoke: the live gate of the platform image (PL-7). core-smoke
# starts its own native controller, so it cannot point at a container;
# this runs the same API checks against the interactive container that
# run-container.sh starts, plus the container's own:
#   docker port: only 127.0.0.1 addresses
#   the UI this server serves passes stixplat.py --check-ui
#   boot: the UART banner within 20 s, as a uart record
#   pause 10 s, resume: sim_time advanced < 0.5 s across the pause
#   cycle: boot_id increments, sim_time continuous, running again;
#     the banner is logged in both boots
#   docker stop (SIGTERM through tini): the container exits 0, and the
#     run dir's log.jsonl ends with a clean quit record
#   flash.bin in the volume allocates < 100 MB
#
# Usage: container-smoke.sh [--image NAME]   (default stix-platform)
# Uses the container name stix-plat-smoke and the volume
# stix-plat-smoke (removed first and at the end); needs 127.0.0.1:8080
# and 127.0.0.1:21502/udp free (no interactive controller running).
# Exit codes: 0 pass, 1 a check failed, 2 environment problem.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLAT="$(cd "$HERE/.." && pwd)"
CTL="$PLAT/stixplat.py"
IMAGE=stix-platform
[ "${1:-}" = --image ] && IMAGE="${2:?--image needs a value}"
NAME=stix-plat-smoke
VOL=stix-plat-smoke
PORT=8080
API="http://127.0.0.1:$PORT/api/v1"

fail() { echo "CONTAINER-SMOKE: FAIL: $*"; exit 1; }
ok() { echo "ok   $*"; }
command -v docker > /dev/null || { echo "CONTAINER-SMOKE: no docker"; exit 2; }
docker image inspect "$IMAGE" > /dev/null 2>&1 ||
    { echo "CONTAINER-SMOKE: no image $IMAGE (run-container.sh build)"; exit 2; }
if curl -s -o /dev/null "http://127.0.0.1:$PORT/"; then
    echo "CONTAINER-SMOKE: 127.0.0.1:$PORT is in use (a controller running?)"
    exit 2
fi

cleanup() {
    docker rm -f "$NAME" > /dev/null 2>&1 || true
    docker volume rm "$VOL" > /dev/null 2>&1 || true
}
cleanup
trap cleanup EXIT

post() {
    local body='{}'
    [ $# -ge 2 ] && body="$2"
    curl -sS -X POST -H 'Content-Type: application/json' \
         -H 'X-Stix-Platform: 1' -d "$body" "$API/cmd/$1"
}
get() { curl -sS "$API/$1"; }
jq_() { python3 -c "import json,sys; d=json.load(sys.stdin); print($1)"; }
okpost() {
    local r
    r="$(post "$@")"
    [ "$(echo "$r" | jq_ 'd["ok"]')" = True ] || fail "$1: $r"
}
banners_in_boot() {
    get "log?since=0&kinds=uart" | python3 -c "
import json,re,sys
print(sum(1 for l in sys.stdin
          if re.search(r'STIX ASW.*ver\.', json.loads(l)['data']['line'])
          and json.loads(l)['boot_id'] == $1))"
}
in_vol() {
    docker run --rm -v "$VOL":/var/lib/stix --entrypoint sh "$IMAGE" -c "$1"
}

# -- start: the interactive sample, detached -------------------------------
"$PLAT/run-container.sh" --image "$IMAGE" --name "$NAME" --volume-name "$VOL" \
    --detach > /dev/null || fail "run-container.sh could not start $NAME"
for _ in $(seq 100); do
    curl -s -o /dev/null "$API/status" && break
    sleep 0.2
done
[ "$(get status | jq_ 'd["mode"]')" = interactive ] ||
    fail "no interactive controller on $API: $(docker logs "$NAME" 2>&1 | tail -3)"
ok "the interactive container answers on $API"

# -- docker port: 127.0.0.1 only -------------------------------------------
PORTS="$(docker port "$NAME")"
echo "$PORTS" | grep -vq '127\.0\.0\.1:' &&
    fail "a port is published beyond 127.0.0.1: $PORTS"
[ "$(echo "$PORTS" | wc -l)" -eq 2 ] || fail "expected 2 published ports: $PORTS"
ok "docker port: $(echo "$PORTS" | tr '\n' ' ')"

# -- the UI ----------------------------------------------------------------
UI="$(python3 "$CTL" --check-ui "http://127.0.0.1:$PORT")" || fail "$UI"
echo "$UI"

# -- boot: the banner within 20 s (the sample starts powered off) ---------
okpost platform.power.on
okpost platform.power.boot
B1="$(get status | jq_ 'd["boot_id"]')"
for _ in $(seq 200); do
    [ "$(banners_in_boot "$B1")" -ge 1 ] && break
    sleep 0.1
done
[ "$(banners_in_boot "$B1")" -ge 1 ] || fail "no UART banner within 20 s"
ok "boot $B1: UART banner as a uart record"

# -- pause / resume --------------------------------------------------------
sleep 5
S1="$(get time | jq_ 'd["sim_time"]')"
okpost sim.pause
sleep 10
okpost sim.resume
S2="$(get time | jq_ 'd["sim_time"]')"
[ $((S2 - S1)) -lt 500000000 ] || fail "sim_time moved $((S2 - S1)) ns in a pause"
ok "pause 10 s: sim_time advanced $((S2 - S1)) ns"

# -- cycle -----------------------------------------------------------------
T1="$(get time | jq_ 'd["sim_time"]')"
okpost platform.power.cycle
B2="$(get status | jq_ 'd["boot_id"]')"
T2="$(get time | jq_ 'd["sim_time"]')"
[ "$B2" = $((B1 + 1)) ] || fail "boot_id $B1 -> $B2 across a cycle"
[ "$T2" -ge $((T1 + 2000000000)) ] || fail "sim_time $T1 -> $T2 not continuous"
[ "$(get status | jq_ 'd["power"]')" = running ] || fail "not running after cycle"
for _ in $(seq 200); do
    [ "$(banners_in_boot "$B2")" -ge 1 ] && break
    sleep 0.1
done
for b in "$B1" "$B2"; do
    [ "$(banners_in_boot "$b")" -ge 1 ] || fail "no UART banner in boot $b"
done
ok "cycle: boot_id $B1 -> $B2, sim_time $T1 -> $T2, banner in both boots"

# -- docker stop: SIGTERM through tini --------------------------------------
docker stop -t 15 "$NAME" > /dev/null || fail "docker stop failed"
RC="$(docker inspect -f '{{.State.ExitCode}}' "$NAME" 2>/dev/null || echo gone)"
# run-container.sh starts with --rm: the container is gone after a clean
# stop, so its rc comes from the log record instead
LAST="$(in_vol 'tail -5 /var/lib/stix/log.jsonl' 2>/dev/null)"
echo "$LAST" | grep '"kind":"power"' | grep -q '"method":"quit","rc":0' ||
    fail "no clean quit record at SIGTERM (container rc $RC): $LAST"
ok "docker stop: QEMU quit cleanly (rc 0) through tini"

# -- the flash: sparse in the volume ----------------------------------------
KB="$(in_vol 'du -k /var/lib/stix/flash.bin | cut -f1')"
[ -n "$KB" ] && [ "$KB" -lt 102400 ] || fail "flash.bin allocates ${KB:-?} KiB"
ok "flash.bin allocates $KB KiB in the volume (< 100 MB)"

echo "CONTAINER-SMOKE: PASS"
