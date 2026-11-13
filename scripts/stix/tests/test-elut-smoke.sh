#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Tests for scripts/stix/elut-smoke.sh (ELUT plan E-2). Nothing boots:
# QEMU is tests/fake-qemu.sh (it records its argv, CWD and the inode of
# ./flash.bin) and the probe is tests/fake-probe.py. The CSV is
# synthetic (no flight data, D-Ed), a decoy file stands in for the
# master flash (never wired in: the gate boots a fresh flash), the
# slot locks live in a temp dir and the disk guard reads a fake df: the
# real locks, the shared flash.bin files and the real ports are never
# touched (stub-bin/ss reports no listener).
#
# Cases: the SKIP 77 paths; the run-dir guard (--check-guard) in legacy
# and slot mode; --dry-run in legacy and slot mode, positive and
# negative (QEMU line, ports, the TC sequence, absolute pmemsave paths,
# the run-2 forbids, the E-4 replay leg's -global/-trace/QMP wiring and
# its absence in --negative); argument errors; the E-4 checker's
# self-test; and fake-QEMU runs (fake-probe.py with FAKE_PMEMSAVE_CSV
# writes the pmemsave dumps, fake-qemu.sh mode elut writes the golden
# UART and the canned D-7 traces): a run-1 UART failure still stops
# before run 2; the full positive gate PASSES in legacy and slot mode
# (fresh private sparse flash, the .dssevs written by the gate, run 2's
# replay argv, the E-4 verdict REPLAY-CHECK: PASS, slot-mode PASS
# cleanup); with FAKE_QEMU_ELUT_TRACE=vacuous (no replay start) and
# =fallback (a fallback line) the gate FAILs in run 2 at the E-4
# verdict.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
S="$REPO_ROOT/scripts/stix"
GATE="$S/elut-smoke.sh"
FAKE="$S/tests/fake-qemu.sh"
FPROBE="$S/tests/fake-probe.py"
export PATH="$S/tests/stub-bin:$PATH"
UIDB=$((20000 + $(id -u)))
B1=$((UIDB + 100))
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
printf '#!/bin/sh\necho "Avail"\necho 99999999\n' > "$W/df-ok"
chmod +x "$W/df-ok"
mkdir -p "$W/locks"
unset STIX_PORT_BASE STIX_SLOTS STIX_ELUT_RUN_DIR STIX_TMTC_PROBE \
    STIX_UPREV_DIR
export STIX_ASW_ELF="$W/dummy.elf"
export STIX_SLOT_LOCK_DIR="$W/locks"
export STIX_DF_CMD="$W/df-ok"

# the synthetic CSV (the self-test's pattern)
python3 - "$S/elut-upload.py" "$W/synth.csv" <<'EOF'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("elut_upload", sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
with open(sys.argv[2], "w", newline="") as f:
    f.write(m.synth_csv_text(m.synth_rows()))
EOF
export STIX_ELUT_CSV="$W/synth.csv"

# a decoy master file: 8 MiB apparent, 16 KiB of data. The gate never
# copies the master (it boots a fresh sparse flash), so the file is only
# an inode the booted flash.bin must not have.
M="$W/master/flash.bin"
mkdir -p "$W/master"
truncate -s 8M "$M"
dd if=/dev/urandom of="$M" bs=4k count=4 seek=100 conv=notrunc status=none
MINO="$(stat -c %i "$M")"

gate() { # env-assignments... -- args...: rc in $rc, output in $out
    local envs=()
    while [ "$1" != -- ]; do envs+=("$1"); shift; done
    shift
    out="$(env "${envs[@]}" timeout 60 bash "$GATE" "$FAKE" "$@" 2>&1)"
    rc=$?
}

# ---- SKIP 77
gate STIX_ELUT_CSV= -- --dry-run
[ "$rc" -eq 77 ] && grep -q '^SKIP: STIX_ELUT_CSV unset' <<< "$out"
check "SKIP 77 without STIX_ELUT_CSV" $? "rc=$rc $out"
gate STIX_ELUT_CSV="$W/none.csv" --
[ "$rc" -eq 77 ] &&
    grep -q '^SKIP: STIX_ELUT_CSV not a readable file' <<< "$out"
check "SKIP 77 with an unreadable CSV" $? "rc=$rc $out"
gate STIX_ASW_ELF="$W/none.elf" --
[ "$rc" -eq 77 ] && grep -q '^SKIP: ASW ELF absent' <<< "$out"
check "SKIP 77 without the ELF" $? "rc=$rc $out"

# ---- the run-dir guard
gate -- --check-guard
[ "$rc" -eq 0 ] && grep -q "^GUARD OK: run dir /tmp/stix-elut-run " <<< "$out"
check "guard: legacy default" $? "rc=$rc $out"
gate STIX_PORT_BASE=1 -- --check-guard
[ "$rc" -eq 0 ] &&
    grep -q "^GUARD OK: run dir /tmp/stix-elut-run-s1 " <<< "$out"
check "guard: slot 1 suffix" $? "rc=$rc $out"
gate -- --check-guard --negative
[ "$rc" -eq 0 ] &&
    grep -q "^GUARD OK: run dir /tmp/stix-elut-run-neg " <<< "$out"
check "guard: --negative suffix" $? "rc=$rc $out"
UP="$HOME/dev/stix-state/uprev-run"
for d in /tmp/stix-uprev-run /tmp/stix-uprev-run/ /tmp/stix-uprev-run/x \
         /tmp/stix-baseline-run//. /tmp/stix-gs-run /tmp/stix-gs-run/x \
         "$UP" "$UP/" "$UP/x" /tmp / /tmp/../tmp; do
    gate STIX_ELUT_RUN_DIR="$d" -- --check-guard
    [ "$rc" -eq 2 ] && grep -q 'refusing run dir' <<< "$out"
    check "guard: refuses $d" $? "rc=$rc $out"
done
ln -s /tmp/stix-uprev-run "$W/link"
ln -s "$UP" "$W/ulink"
for l in "$W/link" "$W/ulink"; do
    gate STIX_ELUT_RUN_DIR="$l" -- --check-guard
    [ "$rc" -eq 2 ] && grep -q 'refusing run dir' <<< "$out"
    check "guard: refuses a symlink to the uprev dir ($l)" $? "rc=$rc $out"
done

# ---- argument errors
gate -- --bogus
[ "$rc" -eq 2 ]
check "unknown argument -> 2" $? "rc=$rc"
gate -- --negative --save-template "$W/t.bin"
[ "$rc" -eq 2 ] && grep -q 'needs a positive run' <<< "$out"
check "--save-template refused with --negative" $? "rc=$rc $out"
gate -- --save-template
[ "$rc" -eq 2 ]
check "--save-template without PATH -> 2" $? "rc=$rc"

# ---- --dry-run
gate STIX_ELUT_RUN_DIR="$W/dr" -- --dry-run
DRY="$out"
[ "$rc" -eq 0 ]
check "dry-run legacy rc 0" $? "rc=$rc $out"
r1="$(sed -n '/^# run 1/,/^# run 2/p' <<< "$DRY")"
r2="$(sed -n '/^# run 2/,$p' <<< "$DRY")"
# printf %q escapes the spaces of a hex unit
q() { sed 's/ /\\ /g' <<< "$1"; }
HDR="--send-hex $(q "40 02 00 00 1D AC")"
n7="$(grep -oF -- "$(q "00 D7 19 ED 07")" <<< "$r1" | wc -l)"
[ "$n7" -eq 96 ]
check "dry-run: 96 TC(237,7) in run 1" $? "n=$n7"
grep -qF -- "--send-at 127 $HDR\\ $(q "C0 70 00 08 19 ED 08 00 03 00 01")" \
    <<< "$r1" &&
    grep -q -- '0x5A1:1:7:1DACC070@38' <<< "$r1"
check "dry-run: TC(237,8) LUT ID 1 at 127, TM(1,7)@38" $? \
    "$(grep -o -- '--send-at 12[0-9][^-]*' <<< "$r1")"
! grep -q -- 'ED\\ 07\|ED\\ 08' <<< "$r2"
check "dry-run: no TC(237,7)/(237,8) in run 2" $? ""
grep -q -- "--expect 0x5A7:5:3:8030@50" <<< "$r1" &&
    grep -q -- "--forbid 0x5A7:5:3:8030" <<< "$r2" &&
    ! grep -q -- "--expect 0x5A7:5:3:8030" <<< "$r2"
check "dry-run: 8030 expected in run 1, forbidden in run 2" $? ""
for n in r1-pre r1-apply r1-nominal; do
    grep -q "\"filename\\\\\": \\\\\"$W/dr/elut-dump.$n.bin" <<< "$r1" ||
        grep -qF "$W/dr/elut-dump.$n.bin" <<< "$r1"
    check "dry-run: run 1 pmemsave $n, absolute path" $? ""
done
for n in r2-pre r2-nominal; do
    grep -qF "$W/dr/elut-dump.$n.bin" <<< "$r2"
    check "dry-run: run 2 pmemsave $n, absolute path" $? ""
done
grep -qF -- "--port $UIDB " <<< "$r1" && ! grep -q 'tmtc-port' <<< "$DRY" &&
    grep -qF -- "-qmp unix:$W/dr/qmp.sock\\,server=on\\,wait=off" <<< "$r1" &&
    grep -qF -- "-serial file:$W/dr/uart.elut2.raw" <<< "$r2"
check "dry-run legacy: port 20000+uid, no -global, QMP and UART paths" $? ""
grep -q 'B0=0 B32=0' <<< "$DRY" && grep -q "sha256 " <<< "$DRY"
check "dry-run: the CSV record line" $? ""
for g in "0x5A1:1:1:1DACC001@12" "0x5A1:1:7:1DACC002@8" \
         "0x5A1:1:7:1DACC003@45" "0x5A1:1:7:1DACC06F@5"; do
    grep -qF -- "$g" <<< "$r1"
    check "dry-run: window $g" $? ""
done
grep -qF -- "--forbid 0x5A7:5:2:542c03" <<< "$r2" &&
    grep -qF -- "--expect 0x5A7:5:2:542c01@45" <<< "$r2" &&
    grep -qF -- "--expect 0x5A7:5:2:542c01@45" <<< "$r1" &&
    grep -qF -- "--forbid 0x5A1:1:8" <<< "$r2"
check "dry-run: 542c03 forbidden, 542c01 expected, TM(1,8) forbidden" $? ""

gate STIX_ELUT_RUN_DIR="$W/dr" -- --dry-run --negative
[ "$rc" -eq 0 ] && ! grep -q -- 'ED\\ 08' <<< "$out" &&
    [ "$(grep -o -- 'ED\\ 07' <<< "$out" | wc -l)" -eq 96 ] &&
    grep -qF "$W/dr-neg/elut-dump.r1-apply.bin" <<< "$out"
check "dry-run --negative: no TC(237,8), 96 TC(237,7), -neg dir" $? "rc=$rc"
! grep -q 'det-replay-file\|stixdet_replay\|stixdet_elut_fallback\|dump-apid\|replay-dss' <<< "$out"
check "dry-run --negative: no E-4 replay wiring" $? ""

# the E-4 wiring in the positive dry-run's run 2
CONF2="$(python3 - "$S/elut-upload.py" <<'EOF'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("elut_upload", sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
print(m.unit_hex(m.tc_unit(4, 236, 1, bytes([0x04, 0x00]))))
EOF
)"
[ "$CONF2" = "40 02 00 00 1D AC C0 04 00 07 19 EC 01 00 04 00 3D 0D" ]
check "TC_CONF2 (seq 4) rebuilds with elut-upload.py" $? "$CONF2"
r2q="$(sed -n '/^# run 2/,$p' <<< "$DRY" | sed -n '/(cd /,/qemu-stdout/p')"
r2p="$(sed -n '/^# run 2/,$p' <<< "$DRY" | sed -n '/python3/,$p')"
grep -qF -- "-global grlib-stixctrl.det-replay-file=$W/dr/dssevs.elut2" <<< "$r2q" &&
    grep -qF -- "-trace stixdet_elut_fallback" <<< "$r2q" &&
    grep -qF -- "-trace stixdet_replay_scan" <<< "$r2q" &&
    grep -qF -- "-trace stixdet_replay_state" <<< "$r2q" &&
    grep -qF -- "-trace stixspw_rx_truncated" <<< "$r2q"
check "dry-run run 2: -global det-replay-file and the 4 traces" $? \
    "$(grep -o -- '-trace [a-z_]*' <<< "$r2q" | tr '\n' ' ')"
grep -qF -- "--send-at 5 --qmp-cmd" <<< "$r2p" &&
    grep -qF -- "det-source" <<< "$r2p" &&
    grep -qF -- "replay-dss" <<< "$r2p"
check "dry-run run 2: qom-set det-source=replay-dss at @5" $? ""
grep -qF -- "--dump-apid 0x5DC:$W/dr/ql.dump.elut2.hex" <<< "$r2p" &&
    grep -qF -- "--send-at 115 --send-hex $(q "$CONF2")" <<< "$r2p" &&
    grep -qF -- "0x5A7:5:1:00210504@10" <<< "$r2p" &&
    grep -qF -- "--expect 0x5DC:21:6:1e@15" <<< "$r2p" &&
    grep -qF -- "--expect 0x5DC:21:6:22@15" <<< "$r2p"
check "dry-run run 2: 0x5DC dump, TC(236,1) at 115, exit-flush expects" $? ""
! grep -q 'det-replay-file\|replay-dss' <<< "$r1" &&
    ! grep -q 'stixdet_elut_fallback' <<< "$r1"
check "dry-run run 1: no replay wiring" $? ""

gate STIX_PORT_BASE=1 STIX_ELUT_RUN_DIR="$W/dr" -- --dry-run
[ "$rc" -eq 0 ] &&
    grep -qF -- "-global grlib-stixspw.tmtc-port=$B1" <<< "$out" &&
    grep -qF -- "--port $B1 " <<< "$out" &&
    grep -qF "$W/dr-s1/elut-dump.r2-nominal.bin" <<< "$out" &&
    [ ! -e "$W/locks/stix-qtest-$B1.lock" ]
check "dry-run slot 1: -global, --port B1, -s1 dir, no lock taken" $? "rc=$rc"

# ---- the E-4 checker's self-test (the verdict rules: good, vacuous,
# fallback line, fallback-binned counts, a stray count, two dumps)
out="$(python3 "$S/elut-replay-check.py" --self-test 2>&1)"
if grep -q '^SELF-TEST: PASS' <<< "$out"; then
    rc=0
else
    rc=1
fi
check "elut-replay-check self-test" "$rc" "$(tail -3 <<< "$out")"

# ---- fake-QEMU runs: the E-4 verdicts and the full positive pass.
# fake-probe.py (FAKE_PMEMSAVE_CSV) writes the pmemsave dumps; the
# canned SSID 32 dump of the exit flush holds the replay's 4 counts at
# the CSV's honoured ebin in subcol 0 only.
python3 - "$S/ql-check.py" "$S/elut-upload.py" "$W/synth.csv" \
    "$W/ql32.hex" "$W/ql32wrong.hex" <<'EOF'
import importlib.util, sys
def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m
q = load("ql_check", sys.argv[1])
m = load("elut_upload", sys.argv[2])
rows = m.parse_csv_file(sys.argv[3])
subcol = m.SUBCOL_FROM_FPGA_DET[0]
ebin = m.apply_row(m.boundaries(rows[(subcol, 0)]))[2000]
spectra = [(s, [4 if (s == subcol and e == ebin) else 0
                for e in range(32)]) for s in range(32)]
with open(sys.argv[4], "w") as f:
    f.write(q.build_packet(q.ssid32_body(spectra)).hex() + "\n")
# the rule-4 decoy: the counts at a wrong ebin (20 != the honoured one)
spectra_wrong = [(s, [4 if (s == subcol and e == 20) else 0
                      for e in range(32)]) for s in range(32)]
with open(sys.argv[5], "w") as f:
    f.write(q.build_packet(q.ssid32_body(spectra_wrong)).hex() + "\n")
print("canned ssid32 dump: subcol %d ebin %d" % (subcol, ebin))
EOF

fake_run() { # label env...; set FAKE_PROBE_DUMP_FILE before the call to
             # swap the canned SSID 32 dump
    local label="$1"; shift
    : > "$W/qargv.$label"
    : > "$W/pargv.$label"
    gate "$@" STIX_TMTC_PROBE="$FPROBE" FAKE_PROBE_SECONDS=1 \
        FAKE_QEMU_RUN_SECONDS=20 FAKE_PMEMSAVE_CSV="$W/synth.csv" \
        FAKE_QEMU_ARGV_FILE="$W/qargv.$label" \
        FAKE_PROBE_ARGV_FILE="$W/pargv.$label" \
        FAKE_PROBE_DUMP="${FAKE_PROBE_DUMP_FILE:-$W/ql32.hex}" --
}
r2argv() { # file -> the second boot's argv lines
    awk 'seen { print } /^end$/{ seen = 1 }' "$1"
}

# a run-1 UART failure still stops before run 2 (mode good: the golden
# has no "problem erasing LUT" lines)
fake_run norun FAKE_QEMU_MODE=good STIX_ELUT_RUN_DIR="$W/fl"
[ "$rc" -eq 1 ] &&
    grep -q "^ELUT-SMOKE: FAIL (run 1 UART: 0 'problem erasing" <<< "$out"
check "fake run-1 FAIL stops at the UART check, rc 1" $? \
    "rc=$rc $(tail -3 <<< "$out")"
[ "$(grep -c '^argv:' "$W/qargv.norun")" -eq 1 ]
check "fake run-1 FAIL: run 2 never starts" $? ""
[ -e "$W/fl/flash.bin" ]
check "fake run-1 FAIL keeps the flash" $? ""

# the full positive pass: mode elut with the good traces
fake_run good FAKE_QEMU_MODE=elut STIX_ELUT_RUN_DIR="$W/fl"
[ "$rc" -eq 0 ] && grep -q "ELUT-SMOKE: PASS" <<< "$out" &&
    grep -q "REPLAY-CHECK: PASS" <<< "$out"
check "fake legacy: the full positive gate PASSES with the E-4 verdict" $? \
    "rc=$rc $(tail -8 <<< "$out")"
FINO="$(sed -n 's/^flash-inode: //p' "$W/qargv.good" | head -1)"
[ -n "$FINO" ] && [ "$FINO" != none ] && [ "$FINO" != "$MINO" ] &&
    [ "$(stat -c %s "$W/fl/flash.bin")" -eq 17716740096 ] &&
    [ "$(du -k "$W/fl/flash.bin" | cut -f1)" -eq 0 ]
check "fake legacy: QEMU boots a fresh, empty sparse flash.bin" $? \
    "inode $FINO master $MINO"
grep -qx "cwd: $W/fl" "$W/qargv.good" &&
    grep -qx -- "file:$W/fl/uart.elut1.raw" "$W/qargv.good" &&
    grep -qx -- "unix:$W/fl/qmp.sock,server=on,wait=off" "$W/qargv.good" &&
    grep -qx -- "stixspw_rx_truncated" "$W/qargv.good" &&
    ! grep -q tmtc-port "$W/qargv.good" &&
    grep -qx -- "$UIDB" "$W/pargv.good"
check "fake legacy: QEMU argv, cwd and probe port (run 1)" $? \
    "$(head -30 "$W/qargv.good" | tr '\n' ' ')"
r2="$(r2argv "$W/qargv.good")"
grep -qx -- "file:$W/fl/uart.elut2.raw" <<< "$r2" &&
    grep -qx -- "grlib-stixctrl.det-replay-file=$W/fl/dssevs.elut2" <<< "$r2" &&
    grep -qx -- "stixdet_elut_fallback" <<< "$r2" &&
    grep -qx -- "stixdet_replay_scan" <<< "$r2" &&
    grep -qx -- "stixdet_replay_state" <<< "$r2"
check "fake legacy: run 2 boots with the replay -global and traces" $? \
    "$(tr '\n' ' ' <<< "$r2" | cut -c1-200)"
[ "$(grep -c '^argv:' "$W/qargv.good")" -eq 2 ]
check "fake legacy: two boots" $? ""
grep -q "REPLAY-CHECK: honoured ebin" <<< "$out"
check "fake legacy: the checker prints the honoured ebin" $? ""
r2p="$(r2argv "$W/pargv.good")"
grep -qx -- "0x5DC:$W/fl/ql.dump.elut2.hex" <<< "$r2p" &&
    grep -qF -- "det-source" <<< "$r2p" &&
    grep -qF -- '"replay-dss"' <<< "$r2p" &&
    grep -qx -- "$CONF2" <<< "$r2p"
check "fake legacy: run 2 probe args (dump, qom-set, TC_CONF2)" $? \
    "$(tr '\n' ' ' <<< "$r2p" | cut -c1-200)"

# the E-4 verdicts: a vacuous run (no start line) and a fallback line
fake_run vacuous FAKE_QEMU_MODE=elut FAKE_QEMU_ELUT_TRACE=vacuous \
    STIX_ELUT_RUN_DIR="$W/fl"
[ "$rc" -eq 1 ] && grep -q "^ELUT-SMOKE: FAIL (run 2 replay:" <<< "$out" &&
    grep -q "never classified" <<< "$out"
check "fake vacuous run FAILs the E-4 verdict, rc 1" $? \
    "rc=$rc $(tail -4 <<< "$out")"
[ "$(grep -c '^argv:' "$W/qargv.vacuous")" -eq 2 ] &&
    [ "$(stat -c %s "$W/fl/dssevs.elut2")" -eq 20 ]
check "fake vacuous: run 2 started, the gate wrote the 20-byte .dssevs" \
    $? ""
fake_run fbline FAKE_QEMU_MODE=elut FAKE_QEMU_ELUT_TRACE=fallback \
    STIX_ELUT_RUN_DIR="$W/fl"
[ "$rc" -eq 1 ] && grep -q "^ELUT-SMOKE: FAIL (run 2 replay:" <<< "$out" &&
    grep -q "stixdet_elut_fallback lines" <<< "$out"
check "fake fallback line FAILs the E-4 verdict, rc 1" $? \
    "rc=$rc $(tail -4 <<< "$out")"

# rule 4: the dump carries the counts at a wrong ebin
FAKE_PROBE_DUMP_FILE="$W/ql32wrong.hex" \
    fake_run wrongeb FAKE_QEMU_MODE=elut STIX_ELUT_RUN_DIR="$W/fl"
[ "$rc" -eq 1 ] && grep -q "^ELUT-SMOKE: FAIL (run 2 replay:" <<< "$out" &&
    grep -q "no count at the honoured ebin" <<< "$out"
check "fake wrong-ebin dump FAILs the E-4 verdict (rule 4), rc 1" $? \
    "rc=$rc $(tail -4 <<< "$out")"

# a slot-mode FAIL: the slot tag on the FAIL line
fake_run slotfail FAKE_QEMU_MODE=elut FAKE_QEMU_ELUT_TRACE=vacuous \
    STIX_PORT_BASE=1 STIX_ELUT_RUN_DIR="$W/fl"
[ "$rc" -eq 1 ] &&
    grep -q "^ELUT-SMOKE: FAIL (run 2 replay:.* slot=1 busy=" <<< "$out"
check "fake slot FAIL carries the slot tag, rc 1" $? \
    "rc=$rc $(tail -4 <<< "$out")"

# slot mode: a full PASS with the slot tag and the PASS cleanup
fake_run slot FAKE_QEMU_MODE=elut STIX_PORT_BASE=1 \
    STIX_ELUT_RUN_DIR="$W/fl"
[ "$rc" -eq 0 ] && grep -q "ELUT-SMOKE: PASS.* slot=1 busy=" <<< "$out"
check "fake slot: PASS with the slot tag, rc 0" $? \
    "rc=$rc $(tail -2 <<< "$out")"
grep -qx "cwd: $W/fl-s1" "$W/qargv.slot" &&
    grep -qx -- "grlib-stixspw.tmtc-port=$B1" "$W/qargv.slot" &&
    grep -qx -- "$B1" "$W/pargv.slot" &&
    [ "$(sed -n 's/^flash-inode: //p' "$W/qargv.slot" | head -1)" != none ] &&
    [ "$(sed -n 's/^flash-inode: //p' "$W/qargv.slot" | head -1)" != "$MINO" ]
# (the full-size sparse check lives in the legacy good case: slot mode
# deletes flash.bin on PASS, so it is gone by the time we look)
check "fake slot: -s1 dir, -global port, probe port, fresh private flash" \
    $? "$(tr '\n' ' ' < "$W/qargv.slot" | cut -c1-300)"
[ ! -e "$W/fl-s1/flash.bin" ]
check "fake slot: the PASS cleanup deleted the flash" $? ""

echo "test-elut-smoke: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
