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
# the run-2 forbids); argument errors; and a fake-QEMU run in legacy and
# slot mode, which must boot a fresh private sparse flash in the run dir
# (never the master) with the right argv and then FAIL (the fake probe sends
# nothing and fake QEMU dumps nothing).
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

gate STIX_PORT_BASE=1 STIX_ELUT_RUN_DIR="$W/dr" -- --dry-run
[ "$rc" -eq 0 ] &&
    grep -qF -- "-global grlib-stixspw.tmtc-port=$B1" <<< "$out" &&
    grep -qF -- "--port $B1 " <<< "$out" &&
    grep -qF "$W/dr-s1/elut-dump.r2-nominal.bin" <<< "$out" &&
    [ ! -e "$W/locks/stix-qtest-$B1.lock" ]
check "dry-run slot 1: -global, --port B1, -s1 dir, no lock taken" $? "rc=$rc"

# ---- a fake-QEMU run, legacy and slot mode
fake_run() { # label env... : boots fake QEMU twice at most
    local label="$1" dir
    shift
    : > "$W/qargv.$label"
    : > "$W/pargv.$label"
    gate "$@" STIX_TMTC_PROBE="$FPROBE" FAKE_PROBE_SECONDS=1 \
        FAKE_QEMU_RUN_SECONDS=20 FAKE_QEMU_ARGV_FILE="$W/qargv.$label" \
        FAKE_PROBE_ARGV_FILE="$W/pargv.$label" --
}
fake_run legacy STIX_ELUT_RUN_DIR="$W/fl"
[ "$rc" -eq 1 ] && grep -q "^ELUT-SMOKE: FAIL (run 1 UART: 0 'problem erasing" \
    <<< "$out"
check "fake legacy: FAIL at the run-1 UART check, rc 1" $? \
    "rc=$rc $(tail -3 <<< "$out")"
FINO="$(sed -n 's/^flash-inode: //p' "$W/qargv.legacy" | head -1)"
[ -n "$FINO" ] && [ "$FINO" != none ] && [ "$FINO" != "$MINO" ] &&
    [ "$(stat -c %s "$W/fl/flash.bin")" -eq 17716740096 ] &&
    [ "$(du -k "$W/fl/flash.bin" | cut -f1)" -eq 0 ]
check "fake legacy: QEMU boots a fresh, empty sparse flash.bin" $? \
    "inode $FINO master $MINO"
grep -qx "cwd: $W/fl" "$W/qargv.legacy" &&
    grep -qx -- "file:$W/fl/uart.elut1.raw" "$W/qargv.legacy" &&
    grep -qx -- "unix:$W/fl/qmp.sock,server=on,wait=off" "$W/qargv.legacy" &&
    grep -qx -- "stixspw_rx_truncated" "$W/qargv.legacy" &&
    ! grep -q tmtc-port "$W/qargv.legacy" &&
    grep -qx -- "$UIDB" "$W/pargv.legacy"
check "fake legacy: QEMU argv, cwd and probe port" $? \
    "$(head -30 "$W/qargv.legacy" | tr '\n' ' ')"
[ "$(grep -c '^argv:' "$W/qargv.legacy")" -eq 1 ]
check "fake legacy: run 2 never starts after a run-1 FAIL" $? ""
[ -e "$W/fl/flash.bin" ]
check "fake legacy: a FAIL keeps the flash" $? ""

fake_run slot STIX_PORT_BASE=1 STIX_ELUT_RUN_DIR="$W/fl"
[ "$rc" -eq 1 ] &&
    grep -q "^ELUT-SMOKE: FAIL (run 1 UART: .* slot=1 busy=" <<< "$out"
check "fake slot: FAIL with the slot tag, rc 1" $? \
    "rc=$rc $(tail -2 <<< "$out")"
grep -qx "cwd: $W/fl-s1" "$W/qargv.slot" &&
    grep -qx -- "grlib-stixspw.tmtc-port=$B1" "$W/qargv.slot" &&
    grep -qx -- "$B1" "$W/pargv.slot" &&
    [ "$(sed -n 's/^flash-inode: //p' "$W/qargv.slot")" != "$MINO" ] &&
    [ "$(stat -c %s "$W/fl-s1/flash.bin")" -eq 17716740096 ]
check "fake slot: -s1 dir, -global port, probe port, private flash" $? \
    "$(tr '\n' ' ' < "$W/qargv.slot" | cut -c1-300)"

echo "test-elut-smoke: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
