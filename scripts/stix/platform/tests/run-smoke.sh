#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# run-smoke: the live gate of mode = "run" (PL-5). Ports: QEMU TM/TC
# 21536/21537, GS TC 21538, relayed TM to 21541 (nobody listens); run root
# /tmp/stix-plat/run-smoke. Checks:
#   1. run mode over the gate scenarios (gate-spw-link x3, gate-keepalive,
#      gate-timesync x3) exits 0 and every run's report says pass
#   2. gate-negative alone exits 1, with exactly one failed check: the
#      await_event 142d of the step before the clear
# STIX_RUN_SMOKE_LONG=1 adds the optional gate-timecode-loss to 1.
#
# Usage: run-smoke.sh <qemu-binary>   (STIX_ASW_ELF as in boot-smoke.sh)
# Run it under flock /tmp/stix-qtest-21000.lock like every QEMU gate.
# Exit codes: 0 pass (or SKIP), 1 a check failed, 2 environment problem.
set -euo pipefail

QEMU_BIN="$(realpath -e "${1:?usage: run-smoke.sh <qemu-binary>}")" || exit 2
ASW_ELF="${STIX_ASW_ELF:-/tmp/stix-baseline-run/ASW_em_baseline.elf}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT=/tmp/stix-plat/run-smoke

[ -f "$ASW_ELF" ] || { echo "SKIP: ASW ELF absent ($ASW_ELF)"; exit 0; }
for p in 21536 21537 21538 21541; do
    if ss -lntu | grep -q ":$p "; then echo "port $p busy"; exit 2; fi
done

mkdir -p "$ROOT"
CFG="$ROOT/run-smoke.toml"
cat > "$CFG" <<EOT
mode = "run"
[qemu]
binary = "$QEMU_BIN"
elf = "$ASW_ELF"
[ports]
api = 21535
qemu_tm = 21536
qemu_tc = 21537
gs_tc = 21538
[gs]
tm_dests = ["127.0.0.1:21541"]
[retention]
run_root = "$ROOT"
keep_flash = "never"
EOT

PLAT="$HERE/../stixplat.py"
GATES=(--scenario gate-spw-link.json --scenario gate-keepalive.json
       --scenario gate-timesync.json)
if [ "${STIX_RUN_SMOKE_LONG:-0}" = 1 ]; then
    GATES+=(--scenario gate-timecode-loss.json)
fi

# the newest report dir, or "" (no failing pipeline under set -e)
latest() {
    local d
    d="$(ls -1d "$ROOT"/reports/*/ 2>/dev/null | sort | tail -n 1)" || true
    printf '%s' "$d"
}

fail=0
rc=0
python3 "$PLAT" --config "$CFG" "${GATES[@]}" || rc=$?
REP1="$(latest)"
[ -n "$REP1" ] || { echo "FAIL gates: no report (exit $rc)"; exit 1; }
python3 - "$REP1" "$rc" <<'PYEOF' || fail=1
import json, sys
s = json.load(open(sys.argv[1] + "summary.json"))
bad = [r["name"] for r in s["runs"] if r["verdict"] != "pass"]
ok = sys.argv[2] == "0" and s["exit"] == 0 and len(s["runs"]) >= 7 \
    and not bad
print("%s gates: exit %s, %d runs, not passed: %s"
      % ("ok  " if ok else "FAIL", sys.argv[2], len(s["runs"]), bad))
sys.exit(0 if ok else 1)
PYEOF

rc=0
python3 "$PLAT" --config "$CFG" --scenario gate-negative.json || rc=$?
REP2="$(latest)"
if [ -z "$REP2" ] || [ "$REP2" = "$REP1" ]; then
    echo "FAIL negative: no new report (exit $rc)"
    exit 1
fi
python3 - "$REP2" "$rc" <<'PYEOF' || fail=1
import json, sys
r = json.load(open(sys.argv[1] + "gate-negative.json"))
failed = [(s["name"], c["kind"], c["spec"]) for s in r["steps"]
          for c in s["checks"] if c["verdict"] != "pass"]
want = failed == [("142D before the clear (must time out)", "await_event",
                   {"eid": "142d", "data": "00", "timeout_s": 5})]
ok = sys.argv[2] == "1" and r["verdict"] == "fail" and want
print("%s negative: exit %s, verdict %s, unmatched %r"
      % ("ok  " if ok else "FAIL", sys.argv[2], r["verdict"], failed))
sys.exit(0 if ok else 1)
PYEOF

if [ "$fail" = 0 ]; then echo "RUN-SMOKE: PASS"; else echo "RUN-SMOKE: FAIL"; fi
exit "$fail"
