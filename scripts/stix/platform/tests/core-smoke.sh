#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# core-smoke: the live gate of the platform controller core (PL-3).
# curl only (no browser); the controller on 127.0.0.1:21520, QEMU UDP on
# 21521/21522, run dir /tmp/stix-plat/core-smoke (a fresh sparse flash
# made by QEMU). Checks, in order:
#   power on: frozen, flash.bin sparse; an SSE client collects records
#   timecodes=false set while frozen (raw QMP via debug.qmp) survives cont
#   boot: the UART banner within 20 s, as uart records
#   pause 10 s, resume: sim_time advanced < 0.5 s across the pause
#   off (same flash inode), on, boot, cycle: boot_id increments and
#     sim_time is continuous (it grows by at least the off-time)
#   after the cycle the time codes run again (the positive control)
#   the UART banner is logged for every boot, also after the cycle
#   SIGTERM: the controller powers QEMU off with quit, rc 0
#   the SSE records equal the log.jsonl lines (same seqs, same bytes)
#
# Usage: core-smoke.sh <qemu-binary>   (STIX_ASW_ELF as in boot-smoke.sh)
# Run it under flock /tmp/stix-qtest-21000.lock like every QEMU gate.
# Exit codes: 0 pass (or SKIP), 1 a check failed, 2 environment problem.
set -euo pipefail

QEMU_BIN="$(realpath -e "${1:?usage: core-smoke.sh <qemu-binary>}")" || exit 2
ASW_ELF="${STIX_ASW_ELF:-/tmp/stix-baseline-run/ASW_em_baseline.elf}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CTL="$HERE/../stixplat.py"
RUN=/tmp/stix-plat/core-smoke
PORT=21520
API="http://127.0.0.1:$PORT/api/v1"

[ -f "$ASW_ELF" ] || { echo "SKIP: ASW ELF absent ($ASW_ELF)"; exit 0; }
for p in 21520 21521 21522; do
    if ss -lntu | grep -q ":$p "; then echo "port $p busy"; exit 2; fi
done

mkdir -p "$RUN"
cat > "$RUN/core-smoke.toml" <<EOF
mode = "interactive"
[run_dir]
path = "$RUN"
fresh = true
[qemu]
binary = "$QEMU_BIN"
elf = "$ASW_ELF"
[ports]
api = $PORT
qemu_tm = 21521
qemu_tc = 21522
[power]
off_time_s = 2.0
[debug]
raw_qmp = true
EOF

CTL_PID=
SSE_PID=
cleanup() {
    [ -n "$SSE_PID" ] && kill "$SSE_PID" 2>/dev/null || true
    [ -n "$CTL_PID" ] && kill "$CTL_PID" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

fail() { echo "CORE-SMOKE: FAIL: $*"; exit 1; }
ok() { echo "ok   $*"; }
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
    echo "$r"
}
blocks() { echo $(( $(stat -c %b "$RUN/flash.bin") * 512 )); }
banners_in_boot() {
    get "log?since=0&kinds=uart" | python3 -c "
import json,re,sys
print(sum(1 for l in sys.stdin
          if re.search(r'STIX ASW.*ver\.', json.loads(l)['data']['line'])
          and json.loads(l)['boot_id'] == $1))"
}
emits_in_boot() {
    get "log?since=0&kinds=log" | python3 -c "
import json,sys
print(sum(1 for l in sys.stdin
          if json.loads(l)['path'] == 'trace.stixspw_timecode_emit'
          and json.loads(l)['boot_id'] == $1))"
}

python3 "$CTL" --config "$RUN/core-smoke.toml" > "$RUN/controller.out" 2>&1 &
CTL_PID=$!
for _ in $(seq 50); do
    get status > /dev/null 2>&1 && break
    sleep 0.1
done
get status > /dev/null || fail "the API did not come up"

curl -sN -H 'Last-Event-ID: 0' "$API/stream" > "$RUN/sse.out" &
SSE_PID=$!

# -- power on: frozen, sparse flash ------------------------------------
okpost platform.power.on > /dev/null
[ "$(get status | jq_ 'd["power"]')" = frozen ] || fail "not frozen"
[ -f "$RUN/flash.bin" ] || fail "no flash.bin"
[ "$(blocks)" -lt $((1 << 30)) ] || fail "flash.bin not sparse"
INO="$(stat -c %i "$RUN/flash.bin")"
ok "power on: frozen, flash.bin sparse ($(blocks) bytes)"

# -- timecodes=false while frozen survives cont --------------------------
SPW='"path":"/machine/stixspw","property":"timecodes"'
okpost debug.qmp "{\"command\":\"qom-set\",\"arguments\":{$SPW,\"value\":false}}" \
    > /dev/null

# -- boot: the banner within 20 s ----------------------------------------
okpost platform.power.boot > /dev/null
for _ in $(seq 200); do
    [ "$(banners_in_boot 1)" -ge 1 ] && break
    sleep 0.1
done
[ "$(banners_in_boot 1)" -ge 1 ] || fail "no UART banner within 20 s"
ok "boot: UART banner as a uart record"

# -- pause / resume --------------------------------------------------------
sleep 5
S1="$(get time | jq_ 'd["sim_time"]')"
okpost sim.pause > /dev/null
sleep 10
okpost sim.resume > /dev/null
S2="$(get time | jq_ 'd["sim_time"]')"
[ $((S2 - S1)) -lt 500000000 ] || fail "sim_time moved $((S2 - S1)) ns in a pause"
ok "pause 10 s: sim_time advanced $((S2 - S1)) ns"

sleep 10
TC="$(okpost debug.qmp "{\"command\":\"qom-get\",\"arguments\":{$SPW}}" |
      jq_ 'd["result"]["return"]')"
[ "$TC" = False ] || fail "timecodes is $TC after cont"
TR="$(get state/idpu.spw.control.tr | jq_ 'd["truth"]')"
[ "$TR" = True ] || fail "control.TR is $TR: the check below would be vacuous"
N1="$(emits_in_boot 1)"
[ "$N1" = 0 ] || fail "$N1 time codes emitted with timecodes=false"
ok "timecodes=false set while frozen survived cont (TR=1, 0 codes in ~25 s)"

# -- off, on, boot, cycle --------------------------------------------------
okpost platform.power.off | grep -q '"method":"quit"' || fail "off not by quit"
[ "$(stat -c %i "$RUN/flash.bin")" = "$INO" ] || fail "flash inode changed"
okpost platform.power.on > /dev/null
okpost platform.power.boot > /dev/null
B1="$(get status | jq_ 'd["boot_id"]')"
T1="$(get time | jq_ 'd["sim_time"]')"
okpost platform.power.cycle > /dev/null
B2="$(get status | jq_ 'd["boot_id"]')"
T2="$(get time | jq_ 'd["sim_time"]')"
[ "$B2" = $((B1 + 1)) ] || fail "boot_id $B1 -> $B2 across a cycle"
[ "$T2" -ge $((T1 + 2000000000)) ] || fail "sim_time $T1 -> $T2 not continuous"
[ "$(stat -c %i "$RUN/flash.bin")" = "$INO" ] || fail "flash inode changed"
[ "$(get status | jq_ 'd["power"]')" = running ] || fail "not running after cycle"
ok "off/on/boot/cycle: boot_id $B1 -> $B2, sim_time $T1 -> $T2, same inode"

for _ in $(seq 300); do
    [ "$(emits_in_boot "$B2")" -ge 2 ] && break
    sleep 0.1
done
[ "$(emits_in_boot "$B2")" -ge 2 ] || fail "no time codes after the cycle"
ok "time codes run again after the cycle (positive control)"
for b in "$B1" "$B2"; do
    [ "$(banners_in_boot "$b")" -ge 1 ] || fail "no UART banner in boot $b"
done
ok "UART banner logged in boots $B1 and $B2 too (append=on)"

QPID="$(get status | jq_ 'd["pid"]')"

# -- SIGTERM ----------------------------------------------------------------
sleep 1
kill -TERM "$CTL_PID"
for _ in $(seq 100); do
    kill -0 "$CTL_PID" 2>/dev/null || break
    sleep 0.1
done
kill -0 "$CTL_PID" 2>/dev/null && fail "the controller ignored SIGTERM"
wait "$CTL_PID" || fail "controller rc $?"
CTL_PID=
kill -0 "$QPID" 2>/dev/null && fail "QEMU $QPID survived the controller"
tail -5 "$RUN/log.jsonl" | grep '"kind":"power"' | grep -q '"method":"quit","rc":0' \
    || fail "no clean quit record at SIGTERM"
ok "SIGTERM: QEMU quit cleanly (rc 0)"

# -- SSE == log.jsonl -------------------------------------------------------
kill "$SSE_PID" 2>/dev/null || true
wait "$SSE_PID" 2>/dev/null || true
SSE_PID=
python3 - "$RUN/sse.out" "$RUN/log.jsonl" <<'EOF' || fail "SSE differs from log.jsonl"
import json, sys
sse = {}
for block in open(sys.argv[1]).read().split("\n\n"):
    lines = block.split("\n")
    if not lines[0].startswith("id: "):
        continue
    sse[int(lines[0][4:])] = lines[2][len("data: "):]
log = {json.loads(l)["seq"]: l for l in open(sys.argv[2]).read().splitlines()}
lo, hi = min(sse), max(sse)
want = {s: l for s, l in log.items() if lo <= s <= hi}
assert lo == 1, "SSE did not start at seq 1 (%d)" % lo
assert sse == want, "differ at %s" % sorted(set(sse) ^ set(want))[:5]
assert hi >= max(log) - 10, "SSE stopped early (%d of %d)" % (hi, max(log))
print("ok   SSE: %d records equal log.jsonl seqs %d..%d byte for byte"
      % (len(sse), lo, hi))
EOF

echo "CORE-SMOKE: PASS"
