#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# run-smoke: the live gate of mode = "run" (PL-5). Ports and run root
# from plat-slot.sh: legacy (STIX_PORT_BASE unset) QEMU TM/TC
# 21536/21537, GS TC 21538, relayed TM to 21541 (nobody listens), run
# root /tmp/stix-plat/run-smoke; in slot k the slot's band and
# /tmp/stix-plat/run-smoke-s<k>. Checks:
#   1. run mode over the gate scenarios (gate-spw-link x3, gate-keepalive,
#      gate-timesync x3) exits 0 and every run's report says pass
#   2. gate-negative alone exits 1, with exactly one failed check: the
#      await_event 142d of the step before the clear
# STIX_RUN_SMOKE_LONG=1 adds the optional gate-timecode-loss to 1.
#
# Usage: run-smoke.sh [--dry-run] <qemu-binary>   (STIX_ASW_ELF as in
# boot-smoke.sh). Legacy: run it under flock /tmp/stix-qtest-21000.lock;
# STIX_PORT_BASE=auto|k takes a slot (lib/slot.sh). --dry-run prints the
# ports and run root and starts nothing.
# Exit codes: 0 pass (or SKIP), 1 a check failed, 2 environment problem.
set -euo pipefail

DRY=0
if [ "${1:-}" = --dry-run ]; then DRY=1; shift; fi
USAGE="usage: run-smoke.sh [--dry-run] <qemu-binary>"
QEMU_BIN="$(realpath -e "${1:?$USAGE}")" || exit 2
ASW_ELF="${STIX_ASW_ELF:-/tmp/stix-baseline-run/ASW_em_baseline.elf}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=plat-slot.sh
. "$HERE/plat-slot.sh"
if [ "$DRY" = 1 ] || [ ! -f "$ASW_ELF" ]; then
    plat_slot_setup run-smoke 21535 peek
    [ "$DRY" = 0 ] || { plat_print_ports; exit 0; }
    echo "SKIP: ASW ELF absent ($ASW_ELF)"
    exit 0
fi
plat_slot_setup run-smoke 21535
ROOT="$PLAT_RUN"
plat_ports_free

mkdir -p "$ROOT"
CFG="$ROOT/run-smoke.toml"
cat > "$CFG" <<EOT
mode = "run"
[qemu]
binary = "$QEMU_BIN"
elf = "$ASW_ELF"
[ports]
api = $PLAT_API
qemu_tm = $PLAT_QEMU_TM
qemu_tc = $PLAT_QEMU_TC
gs_tc = $PLAT_GS_TC
[gs]
tm_dests = ["127.0.0.1:$PLAT_SINK"]
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

if [ "$fail" = 0 ]; then echo "RUN-SMOKE: PASS${STIX_SLOT_TAG}"
else echo "RUN-SMOKE: FAIL${STIX_SLOT_TAG}"; fi
exit "$fail"
