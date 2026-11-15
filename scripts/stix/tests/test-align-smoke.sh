#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Host-only tests for scripts/stix/align-smoke.sh (stimulus plan A3/C3).
# Nothing boots: the ELF is a dummy file for the SKIP path, the slot
# locks live in a temp dir and the disk guard reads a fake df (stub-bin/ss
# reports no listener), and every run dir is a temp dir.
#
# Cases: the SKIP 77 path; the run-dir guard (--check-guard) in legacy
# and slot mode with the -neg / -adc / -adc-neg suffixes and the shared
# dir refusals; --dry-run for the align legs, --adc and the negatives
# (the -global det-replay-file and the six -trace flags on the QEMU
# line; the qom-sets, the qom-gets with --expect-return, the upload
# groups and the pmemsave on the probe line; no upload wiring in the
# negative; the four ADC boots with det-quiet-cps/det-adc-mode on the
# right legs); and the derive/encode pipeline the dry-run runs: the
# synthetic .dssevs, the derived CSV re-parsing through elut-upload.py
# (384 rows), 96 TC(237,7) + TC(237,8) in the probe line.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
S="$REPO_ROOT/scripts/stix"
GATE="$S/align-smoke.sh"
TOOL="$S/elut-upload.py"
PASS=0
FAIL=0

check() { # name status detail: status 0 is a pass
    if [ "$2" -eq 0 ]; then
        echo "ok   - $1"
        PASS=$((PASS + 1))
    else
        echo "FAIL - $1: $3"
        FAIL=$((FAIL + 1))
    fi
}

W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
echo "dummy elf" > "$W/dummy.elf"
printf '#!/bin/sh\n' > "$W/qemu"
chmod +x "$W/qemu"
printf '#!/bin/sh\necho "Avail"\necho 99999999\n' > "$W/df-ok"
chmod +x "$W/df-ok"
mkdir -p "$W/locks"
unset STIX_PORT_BASE STIX_SLOTS STIX_ALIGN_RUN_DIR STIX_TMTC_PROBE \
    STIX_UPREV_DIR
export STIX_ASW_ELF="$W/dummy.elf"
export STIX_SLOT_LOCK_DIR="$W/locks"
export STIX_DF_CMD="$W/df-ok"
q() { sed 's/[ ,]/\\&/g' <<< "$1"; }

gate() { # env-assignments... -- args...: rc in $rc, output in $out
    local envs=()
    while [ "$1" != -- ]; do envs+=("$1"); shift; done
    shift
    out="$(env "${envs[@]}" timeout 120 bash "$GATE" "$W/qemu" "$@" 2>&1)"
    rc=$?
}

# ---- SKIP 77 (the ELF is checked after the guard)
gate STIX_ASW_ELF="$W/none.elf" --
[ "$rc" -eq 77 ] && grep -q '^SKIP: ASW ELF absent' <<< "$out"
check "SKIP 77 without the ELF" $? "rc=$rc $out"

# ---- the run-dir guard
gate -- --check-guard
[ "$rc" -eq 0 ] && grep -q "^GUARD OK: run dir /tmp/stix-align-run " \
    <<< "$out"
check "guard: legacy default" $? "rc=$rc $out"
gate -- --check-guard --negative
[ "$rc" -eq 0 ] && grep -q "^GUARD OK: run dir /tmp/stix-align-run-neg " \
    <<< "$out"
check "guard: --negative suffix" $? "rc=$rc $out"
gate -- --check-guard --adc
[ "$rc" -eq 0 ] && grep -q "^GUARD OK: run dir /tmp/stix-align-run-adc " \
    <<< "$out"
check "guard: --adc suffix" $? "rc=$rc $out"
gate -- --check-guard --adc --negative
[ "$rc" -eq 0 ] &&
    grep -q "^GUARD OK: run dir /tmp/stix-align-run-adc-neg " <<< "$out"
check "guard: --adc --negative suffix" $? "rc=$rc $out"
gate STIX_PORT_BASE=1 -- --check-guard
[ "$rc" -eq 0 ] && grep -q "^GUARD OK: run dir /tmp/stix-align-run-s1 " \
    <<< "$out"
check "guard: slot 1 suffix" $? "rc=$rc $out"
UP="$HOME/dev/stix-state/uprev-run"
for d in /tmp/stix-uprev-run /tmp/stix-uprev-run/x /tmp/stix-baseline-run \
         /tmp/stix-gs-run /tmp/stix-gs-run/x "$UP" "$UP/x" / /tmp/../tmp; do
    gate STIX_ALIGN_RUN_DIR="$d" -- --check-guard
    [ "$rc" -eq 2 ] && grep -q 'refusing run dir' <<< "$out"
    check "guard: refuses $d" $? "rc=$rc $out"
done
gate -- --bogus
[ "$rc" -eq 2 ]
check "unknown argument -> 2" $? "rc=$rc"

# ---- --dry-run, align legs
gate STIX_ALIGN_RUN_DIR="$W/dr" -- --dry-run
DRY="$out"
[ "$rc" -eq 0 ]
check "dry-run legacy rc 0" $? "rc=$rc $out"
qemu_line="$(sed -n '/^(cd /p' <<< "$DRY")"
probe_line="$(sed -n '/^python3 -u /p' <<< "$DRY")"
grep -qF -- "grlib-stixctrl.det-replay-file=$W/dr/dssevs.align" \
    <<< "$qemu_line"
check "dry-run: the realize-time replay file" $? ""
for t in stixdet_align stixdet_elut_fallback stixdet_replay_scan \
         stixdet_replay_state stixdet_chunk; do
    grep -qF -- "-trace $t" <<< "$qemu_line"
    check "dry-run: -trace $t" $? ""
done
n7="$(grep -oF -- "$(q '00 D7 19 ED 07')" <<< "$probe_line" | wc -l)"
[ "$n7" -eq 96 ]
check "dry-run: 96 TC(237,7) upload groups" $? "n=$n7"
grep -qF -- "$(q '19 ED 08 00 03 00 01')" <<< "$probe_line"
check "dry-run: the TC(237,8) apply unit" $? ""
grep -qF -- 'det-align' <<< "$probe_line"
check "dry-run: the det-align qom-set" $? ""
grep -qF -- 'replay-dss' <<< "$probe_line"
check "dry-run: det-source=replay-dss" $? ""
[ "$(grep -oF -- 'det-align-status' <<< "$probe_line" | wc -l)" -eq 2 ]
check "dry-run: two det-align-status qom-gets" $? ""
grep -qF -- "$(q 'fallback=384,honoured=0,mismatch_cells=0')" \
    <<< "$probe_line"
check "dry-run: the leg-1 --expect-return line" $? ""
grep -qF -- 'pmemsave' <<< "$probe_line" &&
    grep -qF "$W/dr/elut-dump.leg2.bin" <<< "$probe_line"
check "dry-run: the leg-2 ELUT pmemsave" $? ""
grep -q '^# ELUT CSV ' <<< "$DRY" && grep -q 'B0=0 B32=0' <<< "$DRY"
check "dry-run: the CSV record line" $? ""
[ -f "$W/dr/derived.csv" ] && [ -f "$W/dr/dssevs.align" ]
check "dry-run: the derive ran (derived.csv, dssevs.align)" $? ""
python3 - "$TOOL" "$W/dr/derived.csv" <<'EOF' > "$W/parse.check" 2>&1
import importlib.util, sys
spec = importlib.util.spec_from_file_location("elut_upload", sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
rows = m.parse_csv_file(sys.argv[2])
ok = len(rows) == 384
# rows hold edges: edge 0 = base + ceil(W / 32), W = 4096 - base, with
# the gate's base 1800 + 10 d per dss_det (= fpga_det, the replay map)
e0 = {m.SUBCOL_FROM_FPGA_DET.index(key[0]): rows[key][0] for key in rows}
ok = ok and len(e0) == 32 and all(
    e0[d] == 1800 + 10 * d + -(-(4096 - 1800 - 10 * d) // 32) for d in e0)
print("OK" if ok else "BAD")
EOF
[ "$(cat "$W/parse.check")" = OK ] &&
    check "the derived CSV re-parses with the gate's baselines" 0 "" ||
    check "the derived CSV re-parses with the gate's baselines" 1 \
        "$(cat "$W/parse.check")"

# ---- --dry-run --negative: no upload wiring
gate STIX_ALIGN_RUN_DIR="$W/dr" -- --dry-run --negative
[ "$rc" -eq 0 ]
check "dry-run --negative rc 0" $? "rc=$rc"
probe_line="$(sed -n '/^python3 -u /p' <<< "$out")"
! grep -qF -- '19 ED 07' <<< "$probe_line" &&
    ! grep -qF -- 'pmemsave' <<< "$probe_line" &&
    ! grep -q '^# ELUT CSV' <<< "$out"
check "dry-run --negative: no upload groups, pmemsave or record" $? ""
grep -qF -- "$(q 'fallback=384,honoured=0,mismatch_cells=0')" \
    <<< "$probe_line"
check "dry-run --negative: the leg-1 --expect-return kept" $? ""

# ---- --dry-run --adc: four boots, the right -globals per leg
gate STIX_ALIGN_RUN_DIR="$W/dra" -- --dry-run --adc
[ "$rc" -eq 0 ]
check "dry-run --adc rc 0" $? "rc=$rc $out"
[ "$(grep -c '^# boot ' <<< "$out")" -eq 4 ]
check "dry-run --adc: four boots" $? "$(grep -c '^# boot ' <<< "$out")"
[ "$(grep -c 'det-adc-mode=true' <<< "$out")" -eq 3 ]
check "dry-run --adc: det-adc-mode on the three mode-on boots" $? \
    "$(grep -c 'det-adc-mode=true' <<< "$out")"
[ "$(grep -c 'det-quiet-cps=200' <<< "$out")" -eq 4 ]
check "dry-run --adc: quiet cps 200 on all four boots" $? \
    "$(grep -c 'det-quiet-cps=200' <<< "$out")"
grep -qF -- 'dssevs.adc' <<< "$out" && ! grep -qF -- 'dssevs.align' \
    <<< "$out"
check "dry-run --adc: the 32-setup-word .dssevs" $? ""
grep -qF -- 'wrong-base0.csv' <<< "$out" &&
    grep -qF -- 'aligned.csv' <<< "$out"
check "dry-run --adc: the base-0 and aligned CSVs" $? ""
n7="$(grep -oF -- "$(q '00 D7 19 ED 07')" <<< "$out" | wc -l)"
[ "$n7" -eq 288 ]
check "dry-run --adc: 3 x 96 upload groups (leg 4, leg 5 twice)" $? \
    "n=$n7"

# ---- --dry-run --adc --negative: only leg 4's upload remains
gate STIX_ALIGN_RUN_DIR="$W/dra" -- --dry-run --adc --negative
[ "$rc" -eq 0 ]
n7="$(grep -oF -- "$(q '00 D7 19 ED 07')" <<< "$out" | wc -l)"
[ "$n7" -eq 96 ]
check "dry-run --adc --negative: only leg 4's 96 upload groups" $? \
    "n=$n7"

# ---- slot mode dry-run: the -global port and the -s1 dir
gate STIX_PORT_BASE=1 STIX_ALIGN_RUN_DIR="$W/dr" -- --dry-run
[ "$rc" -eq 0 ] &&
    grep -qF -- "grlib-stixspw.tmtc-port=$((20000 + $(id -u) + 100))" \
        <<< "$out" &&
    grep -qF "$W/dr-s1/dssevs.align" <<< "$out"
check "dry-run slot 1: the port -global and the -s1 dir" $? ""

echo "test-align-smoke: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
