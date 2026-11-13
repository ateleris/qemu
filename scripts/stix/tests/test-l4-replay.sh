#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Tests for scripts/stix/l4-replay.sh (the #1061 L4 replay wrapper,
# L4-3, plan amendment A).  Nothing boots: QEMU is tests/fake-qemu.sh
# (FAKE_QEMU_MODE=l4 leaves the collect boot's small synthetic flash
# image — 13 RAW_COUNTS files, 25 archived chunks, built with l4-flash
# -truth.py's builder — plus the banner and the one "Skipped first Det
# data chunk" TRACE_ERROR on the UART, and the stixdet_chunk trace at
# SCET 1000001+i; the rcr/flush UART prints are compiled out in these
# builds; the red/green boots get only the boot banner) and the probe
# is tests/fake-probe.py (FAKE_PROBE_DUMP_MAP feeds each run dir its
# own canned 0x5BC/0x5A7 dumps).  The canned dumps are built with
# l4-decode.py's packet builder from the fake collection's known
# truth: 26 model chunks at SCET 1000001+i, archived[k] = model[k] (the
# last model SCET is never archived), the flash holds a 1-chunk file then 2-chunk files -> 25
# archived chunks, 13 files
# (1 + 12 x 2), 13 % 3 = 1 file in the last search round: lastround 2,
# red-round 3; GREEN A carries all 25 samples (rel 0..240 step 10), RED
# A the last 3 (the carried-over sample 220 + the last file's 2), the
# 712 packet is the
# byte-identical ghost, the events dump holds the two boot events and
# one TM(5,2) 0x543F.  The sample bytes are a function of rel, so the
# red/green identity check is meaningful.  The slot locks live in a
# temp dir, the disk guard reads a fake df, the snapshot copies work on
# the wrapper's own sparse flash files (the shared master flashes are
# never touched; stub-bin/ss reports no listener, nothing binds a
# port).
#
# Cases: the SKIP 77 paths (RED/GREEN ELF unset or missing, also with
# --dry-run); the run-dir guard (--check-guard) in legacy and slot mode
# with its refusals; argument errors; --dry-run in legacy and slot mode
# (the four phases' command lines: no boot-TM expects anywhere, the
# collect mode TCs and the QMP flare switch, the request runs' dumps,
# forbids and acks and no NOMINAL TC, the phase-4 decode lines,
# --collect-only stops after phase 2); the fake full run PASSES with
# the documented verdict lines (red 7 = red-round, ghost primary,
# green 25 complete, B silent, identity ok), record.txt carries the
# ELFs' md5+sha256, the PASS cleanup drops the working flashes but
# keeps the snapshot, three boots happened, each request boot had its
# own flash copy; a --collect-only fake run stops after the truth
# gate; a ghost-less RED dump and a wrong RED sample count FAIL with
# the right check names; a FAIL leaves .unfinished and the next run
# moves the dir aside.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
S="$REPO_ROOT/scripts/stix"
GATE="$S/l4-replay.sh"
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
echo "dummy elf" > "$W/red.elf"
cp "$W/red.elf" "$W/green.elf"
printf '#!/bin/sh\necho "Avail"\necho 99999999\n' > "$W/df-ok"
chmod +x "$W/df-ok"
mkdir -p "$W/locks"
unset STIX_PORT_BASE STIX_SLOTS STIX_L4_RUN_DIR STIX_TMTC_PROBE \
    STIX_L4_RED_ELF STIX_L4_GREEN_ELF STIX_L4_COLLECT_ELF STIX_UPREV_DIR \
    STIX_L4_COLLECT_SECONDS STIX_L4_REQ_B_AT STIX_L4_REQ_SECONDS
export STIX_SLOT_LOCK_DIR="$W/locks"
export STIX_DF_CMD="$W/df-ok"

# ---- the canned dumps (l4-decode.py's builder, the fake truth)
python3 - "$S/l4-decode.py" "$W" <<'EOF'
import importlib.util
import os
import sys

spec = importlib.util.spec_from_file_location("l4dec", sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
w = sys.argv[2]
# the fake collection: 26 model chunks at SCET 1000001+i, 25 archived
# (a 1-chunk file then 2-chunk files) -> 13 files, red-round 3
rels = [10 * j for j in range(25)]        # A start = 1000001


def samples(rs):
    # trig and the 16 count bytes are a function of rel, so a decoder
    # that pairs the wrong samples fails the identity check
    return [(r, r % 256, [(r + j) % 251 + 1 for j in range(16)])
            for r in rs]


green = m._build_line(611, 0xC204, 1000001, 0,
                      [(0x0FFF, 0xFFFFFFFF, 0, 0x03E1,
                        samples(rels), 10)], word23=0x4000) + "\n"
red = m._build_line(711, 0xC204, 1000001, 0,
                    [(0x0FFF, 0xFFFFFFFF, 0, 0x03E1,
                      samples(rels[-3:]), 10)]) + "\n"
ghost = m._build_line(712, 0xC205, 1000625, 0,
                      [(0x0FFF, 0xFFFFFFFF, 0, 0x03E1,
                        samples(rels[-3:]), 10)]) + "\n"
redshort = (m._build_line(711, 0xC204, 1000001, 0,
                      [(0x0FFF, 0xFFFFFFFF, 0, 0x03E1,
                        samples(rels[-2:]), 10)]) + "\n"
        + m._build_line(712, 0xC205, 1000625, 0,
                        [(0x0FFF, 0xFFFFFFFF, 0, 0x03E1,
                          samples(rels[-2:]), 10)]) + "\n")
events = "\n".join([m._build_event(0x0022, b"\x00\x00\x00\x08\x00\x00",
                                   sub=1),
                    m._build_event(0x14A3, sub=1),
                    m._build_event(0x543F, b"\x04\x01\x00\x01")]) + "\n"
for name, text in [("green.hex", green),
                   ("red.hex", red + ghost),
                   ("red-noghost.hex", red),
                   ("redshort.hex", redshort),
                   ("events.hex", events)]:
    with open(os.path.join(w, name), "w") as f:
        f.write(text)
print("canned dumps: 25 green samples, 7 red + ghost 712, 1 x 0x543F")
EOF

dump_map() { # run-dir -> the per-run-dir dump map for the fake runs
    echo "$1/red/l4.dump.hex=$W/red.hex,$1/red/events.dump.hex=$W/events.hex,$1/green/l4.dump.hex=$W/green.hex,$1/green/events.dump.hex=$W/events.hex"
}

gate() { # env-assignments... -- args...: rc in $rc, output in $out
    local envs=()
    while [ "$1" != -- ]; do envs+=("$1"); shift; done
    shift
    out="$(env "${envs[@]}" timeout 60 bash "$GATE" "$FAKE" "$@" 2>&1)"
    rc=$?
}

# ---- SKIP 77
gate STIX_L4_RED_ELF= --
[ "$rc" -eq 77 ] && grep -q '^SKIP: STIX_L4_RED_ELF unset' <<< "$out"
check "SKIP 77 without STIX_L4_RED_ELF" $? "rc=$rc $out"
gate STIX_L4_RED_ELF="$W/none.elf" STIX_L4_GREEN_ELF="$W/green.elf" --
[ "$rc" -eq 77 ] && grep -q '^SKIP: STIX_L4_RED_ELF not a file' <<< "$out"
check "SKIP 77 with a missing RED ELF" $? "rc=$rc $out"
gate STIX_L4_RED_ELF="$W/red.elf" STIX_L4_GREEN_ELF= --
[ "$rc" -eq 77 ] && grep -q '^SKIP: STIX_L4_GREEN_ELF unset' <<< "$out"
check "SKIP 77 without STIX_L4_GREEN_ELF" $? "rc=$rc $out"
gate STIX_L4_RED_ELF="$W/red.elf" STIX_L4_GREEN_ELF="$W/none.elf" --
[ "$rc" -eq 77 ] && grep -q '^SKIP: STIX_L4_GREEN_ELF not a file' <<< "$out"
check "SKIP 77 with a missing GREEN ELF" $? "rc=$rc $out"
gate STIX_L4_RED_ELF="$W/none.elf" -- --dry-run
[ "$rc" -eq 77 ] && grep -q '^SKIP:' <<< "$out"
check "SKIP 77 also with --dry-run" $? "rc=$rc $out"

# ---- the run-dir guard
gate STIX_L4_RED_ELF="$W/red.elf" STIX_L4_GREEN_ELF="$W/green.elf" \
    STIX_L4_RUN_DIR="$W/run" -- --check-guard
[ "$rc" -eq 0 ] &&
    grep -q "^GUARD OK: run dir $W/run (canonical)" <<< "$out" &&
    grep -q "GUARD OK: collect $W/run/collect, red $W/run/red" <<< "$out"
check "guard: legacy subdirs" $? "rc=$rc $out"
gate STIX_PORT_BASE=1 STIX_L4_RUN_DIR="$W/run" -- --check-guard
[ "$rc" -eq 0 ] &&
    grep -q "^GUARD OK: run dir $W/run-s1 (canonical)" <<< "$out"
check "guard: slot 1 suffix" $? "rc=$rc $out"
UP="$HOME/dev/stix-state/uprev-run"
for d in /tmp/stix-uprev-run /tmp/stix-uprev-run/ /tmp/stix-uprev-run/x \
         /tmp/stix-baseline-run//. /tmp/stix-gs-run "$UP" "$UP/x" /tmp / \
         /tmp/../tmp; do
    gate STIX_L4_RUN_DIR="$d" -- --check-guard
    [ "$rc" -eq 2 ] && grep -q 'refusing run dir' <<< "$out"
    check "guard: refuses $d" $? "rc=$rc $out"
done
ln -s /tmp/stix-uprev-run "$W/link"
gate STIX_L4_RUN_DIR="$W/link" -- --check-guard
[ "$rc" -eq 2 ] && grep -q 'refusing run dir' <<< "$out"
check "guard: refuses a symlink to the uprev dir" $? "rc=$rc $out"

# ---- argument errors
gate --
[ "$rc" -eq 77 ] && grep -q '^SKIP: STIX_L4_RED_ELF unset' <<< "$out"
check "bare invocation (the fake stands in for qemu) SKIPs" $? "rc=$rc $out"
gate -- --bogus
[ "$rc" -eq 2 ] && grep -q 'unknown argument' <<< "$out"
check "unknown argument -> 2" $? "rc=$rc $out"
gate STIX_L4_RED_ELF="$W/red.elf" STIX_L4_GREEN_ELF="$W/green.elf" \
    STIX_L4_REQ_B_AT=50 -- --dry-run
[ "$rc" -eq 2 ] && grep -q 'STIX_L4_REQ_B_AT must be > 100' <<< "$out"
check "REQ_B_AT <= 100 -> 2" $? "rc=$rc $out"
gate STIX_L4_RED_ELF="$W/red.elf" STIX_L4_GREEN_ELF="$W/green.elf" \
    STIX_L4_COLLECT_SECONDS=abc -- --dry-run
[ "$rc" -eq 2 ] && grep -q 'whole seconds' <<< "$out"
check "non-numeric collect seconds -> 2" $? "rc=$rc $out"

# ---- --dry-run
DENV=(STIX_L4_RED_ELF="$W/red.elf" STIX_L4_GREEN_ELF="$W/green.elf"
      STIX_L4_RUN_DIR="$W/dr")
gate "${DENV[@]}" -- --dry-run
[ "$rc" -eq 0 ] && grep -q '^DRY RUN' <<< "$out"
check "dry-run legacy rc 0" $? "rc=$rc $out"
# no boot-TM expectations anywhere (advisory only)
! grep -q '0x5A7:5:3:8030' <<< "$out" &&
    ! grep -q '002200000008' <<< "$out" && ! grep -qi '14a3' <<< "$out"
check "dry-run: no boot-TM expects (advisory only)" $? ""
# phase 1: the collect boot (printf %q escapes spaces in a hex unit
# as '\ '; fragments below avoid the %q-ambiguous chars = and ,)
grep -qF -- "-serial file:$W/dr/collect/uart.collect.raw" <<< "$out" &&
    grep -qF -- "qmp.sock" <<< "$out" &&
    grep -qF -- "trace.events" <<< "$out" &&
    grep -qF -- "collect.trace" <<< "$out" &&
    grep -qF -- "stixdet_chunk_drop" <<< "$out" &&
    grep -qF -- "stixspw_rx_truncated" <<< "$out"
check "dry-run collect: serial, QMP socket, the trace events" $? ""
grep -qF -- "--send-at 50 --send-hex 40" <<< "$out" &&
    grep -qF -- '00\ 04\ 00\ 4A\ C1' <<< "$out" &&
    grep -qF -- "--expect 0x5A7:5:1:00210204@12" <<< "$out" &&
    grep -qF -- "--send-at 70 --send-hex 40" <<< "$out" &&
    grep -qF -- '01\ 00\ 05\ 00\ BF\ 97' <<< "$out" &&
    grep -qF -- "--expect 0x5A7:5:1:00210405@12" <<< "$out" &&
    grep -qF -- "--forbid 0x5A7:5:1:00210502" <<< "$out"
check "dry-run collect: mode TCs with acks and forbids" $? ""
grep -qF -- 'det-flare' <<< "$out" && grep -qF -- 'send-at 75' <<< "$out"
check "dry-run collect: the QMP flare switch at 75" $? ""
# phase 3: the request runs (scoped to the red section for the
# no-NOMINAL check)
grep -qF -- "--dump-apid 0x5BC:$W/dr/red/l4.dump.hex" <<< "$out" &&
    grep -qF -- "--dump-apid 0x5A7:$W/dr/red/events.dump.hex" <<< "$out" &&
    grep -qF -- "--dump-apid 0x5BC:$W/dr/green/l4.dump.hex" <<< "$out"
check "dry-run requests: the 0x5BC/0x5A7 dumps per build" $? ""
REDSEC="$(sed -n '/^# request run red/,/^# request run green/p' <<< "$out")"
grep -qF -- "--send-at 65 --send-hex 40" <<< "$REDSEC" &&
    grep -qF -- '19\ ED\ 0D\ 00' <<< "$REDSEC" &&
    grep -qF -- "--expect 0x5A1:1:1:1DACC003@10" <<< "$REDSEC"
check "dry-run requests: the flare TC (seq 3) at 65" $? ""
grep -qF -- "--send-at 70 --send-hex 40" <<< "$REDSEC" &&
    grep -qF -- '19\ EE\ 01\ 00' <<< "$REDSEC" &&
    grep -qF -- "--expect 0x5A1:1:7:1DACC004@45" <<< "$REDSEC" &&
    grep -qF -- "--send-at 600 --send-hex 40" <<< "$REDSEC" &&
    grep -qF -- "--expect 0x5A1:1:7:1DACC005@45" <<< "$REDSEC"
check "dry-run requests: A @70 and B @600 with the acks" $? ""
grep -qF -- "--forbid 0x5A7:5:1:142e" <<< "$REDSEC" &&
    grep -qF -- "--forbid 0x5A7:5:1:00210402" <<< "$REDSEC"
check "dry-run requests: the 142E/0021 forbids" $? ""
! grep -qF -- 'BF\ 97' <<< "$REDSEC"
check "dry-run requests: no NOMINAL TC (CONFIGURATION only)" $? ""
grep -qF -- "sparse copy of $W/dr/collect/flash-collect.bin" <<< "$REDSEC"
check "dry-run requests: the snapshot copy is named" $? ""
grep -qF -- "l4-decode.py report --capture $W/dr/red/l4.dump.hex --request A" \
    <<< "$out" &&
    grep -qF -- "--request B" <<< "$out" &&
    grep -qF -- "--truth $W/dr/truth.txt" <<< "$out"
check "dry-run phase 4: the decode lines" $? ""
gate "${DENV[@]}" -- --dry-run --collect-only
[ "$rc" -eq 0 ] && ! grep -q 'phase 3' <<< "$out" &&
    ! grep -q 'l4-decode.py report' <<< "$out"
check "dry-run --collect-only: phases 1-2 only" $? "rc=$rc"
gate STIX_PORT_BASE=1 "${DENV[@]}" -- --dry-run
[ "$rc" -eq 0 ] &&
    grep -qF -- "-global grlib-stixspw.tmtc-port=$B1" <<< "$out" &&
    grep -qF -- "--port $B1 " <<< "$out" &&
    grep -qF -- "$W/dr-s1/red/l4.dump.hex" <<< "$out" &&
    [ ! -e "$W/locks/stix-qtest-$B1.lock" ]
check "dry-run slot 1: -global port, --port B1, -s1 dirs, no lock" $? \
    "rc=$rc"

# ---- the fake full run: PASS with the documented verdict lines
fake_run() { # label run-dir dump-map [env...] -- [gate-args...]
    local label="$1" rundir="$2" map="$3"; shift 3
    local envs=() gargs=() in_g=0
    while [ $# -gt 0 ]; do
        if [ "$1" = -- ]; then in_g=1; shift; continue; fi
        if [ "$in_g" -eq 0 ]; then envs+=("$1"); else gargs+=("$1"); fi
        shift
    done
    : > "$W/qargv.$label"
    : > "$W/pargv.$label"
    out="$(env "${envs[@]}" STIX_L4_RUN_DIR="$rundir" \
        STIX_TMTC_PROBE="$FPROBE" \
        STIX_L4_RED_ELF="$W/red.elf" STIX_L4_GREEN_ELF="$W/green.elf" \
        FAKE_QEMU_MODE=l4 FAKE_PROBE_SECONDS=1 FAKE_QEMU_RUN_SECONDS=2 \
        FAKE_QEMU_ARGV_FILE="$W/qargv.$label" \
        FAKE_PROBE_ARGV_FILE="$W/pargv.$label" \
        FAKE_PROBE_DUMP_MAP="$map" \
        timeout 120 bash "$GATE" "$FAKE" \
        ${gargs[@]+"${gargs[@]}"} 2>&1)"
    rc=$?
}
fake_run full "$W/run" "$(dump_map "$W/run")"
[ "$rc" -eq 0 ] && grep -q '^L4-REPLAY: PASS' <<< "$out"
check "fake legacy: the full run PASSES" $? "rc=$rc $(tail -5 <<< "$out")"
grep -q 'CHECK red-a-samples: ok (3 = red-round 3' <<< "$out" &&
    grep -q 'CHECK red-b-ghost: ok' <<< "$out" &&
    grep -q 'CHECK green-a-samples: ok (25' <<< "$out" &&
    grep -q 'CHECK green-b-silent: ok' <<< "$out" &&
    grep -q 'CHECK red-identity: ok (3 samples' <<< "$out"
check "fake legacy: the Goal-2 check lines" $? "$(grep 'CHECK' <<< "$out" |
    tr '\n' ' ')"
grep -q '^TRUTH GATE: chunks=25 files=13 lastround=2 red-round=3 rcrmax=0' \
    <<< "$out"
check "fake legacy: the truth gate numbers" $? \
    "$(grep 'TRUTH GATE' <<< "$out")"
grep -q '^FLASH-TRUTH: files=13 chunks=25 rcrmax=0' <<< "$out"
check "fake legacy: the flash truth reader's summary" $? \
    "$(grep 'FLASH-TRUTH' <<< "$out")"
[ -f "$W/run/truth.txt" ] &&
    [ "$(grep -c '^chunk ' "$W/run/truth.txt")" -eq 25 ] &&
    [ "$(grep -c '^file ' "$W/run/truth.txt")" -eq 13 ] &&
    [ "$(grep -c '^file 0x02' "$W/run/truth.txt")" -eq 13 ] &&
    [ "$(head -1 "$W/run/truth.txt")" = \
        "# l4-flash-truth: 13 RAW_COUNTS files, 25 archived chunks read from $W/run/collect/flash.bin" ]
check "fake legacy: truth.txt in the l4-decode format (on-flash names)" $? ""
! grep -q 'rcr =' "$W/run/collect/uart.collect.txt"
check "fake legacy: no rcr print on the collect UART (compiled out)" $? ""
grep -q 'md5:' "$W/run/record.txt" && grep -q 'sha256:' "$W/run/record.txt" &&
    [ "$(grep -c '^elf .* md5:' "$W/run/record.txt")" -eq 3 ]
check "fake legacy: record.txt holds the three ELFs' md5+sha256" $? ""
[ -f "$W/run/collect/flash-collect.bin" ] &&
    [ ! -e "$W/run/collect/flash.bin" ] &&
    [ ! -e "$W/run/red/flash.bin" ] && [ ! -e "$W/run/green/flash.bin" ]
check "fake legacy: PASS keeps the snapshot, drops the working flashes" \
    $? ""
[ -f "$W/run/red/l4.dump.hex" ] && [ -f "$W/run/green/A.report.log" ] &&
    [ ! -e "$W/run/.unfinished" ]
check "fake legacy: the run dir keeps dumps, reports, no .unfinished" \
    $? ""
[ "$(grep -c '^argv:' "$W/qargv.full")" -eq 3 ]
check "fake legacy: three boots" $? ""
grep -q 'stixdet_chunk' "$W/run/collect/trace.events" &&
    grep -q 'stixspw_rx_truncated' "$W/run/collect/trace.events" &&
    grep -qF -- "det-flare" "$W/pargv.full"
check "fake legacy: the collect trace events and the flare QMP" $? ""
FINO_RED="$(awk '/^argv:/{n++} n==2 && /^flash-inode:/{print}' "$W/qargv.full")"
FINO_GREEN="$(awk '/^argv:/{n++} n==3 && /^flash-inode:/{print}' "$W/qargv.full")"
[ "$FINO_RED" != "flash-inode: none" ] && [ "$FINO_RED" != "$FINO_GREEN" ]
check "fake legacy: each request boot had its own flash copy" $? \
    "$FINO_RED vs $FINO_GREEN"
# the argv file holds one argument per line
grep -x -A1 -- "--send-at" "$W/pargv.full" | grep -qx 600 &&
    grep -qF -- "0x5BC:$W/run/red/l4.dump.hex" "$W/pargv.full"
check "fake legacy: the red probe sent B at 600 and dumped 0x5BC" $? ""

# ---- --collect-only: the truth gate without the request runs
fake_run collectonly "$W/run-co" "$(dump_map "$W/run-co")" -- --collect-only
[ "$rc" -eq 0 ] &&
    grep -q '^L4-REPLAY (collect-only): PASS' <<< "$out" &&
    [ -f "$W/run-co/truth.txt" ] && [ ! -e "$W/run-co/red/l4.dump.hex" ]
check "fake --collect-only: truth gate PASS, no request runs" $? \
    "rc=$rc $(tail -3 <<< "$out")"

# ---- the fake's own guard: a chunk count off the canned truth must
#      stop the fake collect boot, never reach a PASS
fake_run badn "$W/run-badn" "$(dump_map "$W/run-badn")" \
    FAKE_QEMU_L4_CHUNKS=30 -- --collect-only
[ "$rc" -ne 0 ] && ! grep -q 'PASS' <<< "$out"
check "fake guard: FAKE_QEMU_L4_CHUNKS=30 -> no PASS, rc != 0" $? \
    "rc=$rc $(tail -3 <<< "$out")"

# ---- the negatives: a ghost-less RED and a wrong RED count
fake_run ng2 "$W/run-ng2" \
    "$W/run-ng2/red/l4.dump.hex=$W/red-noghost.hex,$W/run-ng2/red/events.dump.hex=$W/events.hex,$W/run-ng2/green/l4.dump.hex=$W/green.hex,$W/run-ng2/green/events.dump.hex=$W/events.hex"
[ "$rc" -eq 1 ] &&
    grep -q '^L4-REPLAY: FAIL (failed checks: red-b-ghost' <<< "$out"
check "fake ghost-less RED FAILs red-b-ghost, rc 1" $? \
    "rc=$rc $(grep '^L4-REPLAY' <<< "$out")"
[ -e "$W/run-ng2/.unfinished" ]
check "fake FAIL leaves .unfinished" $? ""
fake_run resume "$W/run-ng2" "$(dump_map "$W/run-ng2")"
grep -q "kept the unfinished run's dir as" <<< "$out"
check "fake rerun moves the failed dir aside" $? "rc=$rc"
fake_run ng3 "$W/run-ng3" \
    "$W/run-ng3/red/l4.dump.hex=$W/redshort.hex,$W/run-ng3/red/events.dump.hex=$W/events.hex,$W/run-ng3/green/l4.dump.hex=$W/green.hex,$W/run-ng3/green/events.dump.hex=$W/events.hex"
[ "$rc" -eq 1 ] &&
    grep -q '^L4-REPLAY: FAIL (failed checks: red-a-samples' <<< "$out"
check "fake wrong RED count FAILs red-a-samples, rc 1" $? \
    "rc=$rc $(grep '^L4-REPLAY' <<< "$out")"

# ---- a slot-mode PASS with the slot tag
fake_run slot "$W/run-slot" "$(dump_map "$W/run-slot-s1")" STIX_PORT_BASE=1
[ "$rc" -eq 0 ] && grep -q '^L4-REPLAY: PASS.* slot=1 busy=' <<< "$out"
check "fake slot: PASS with the slot tag, rc 0" $? \
    "rc=$rc $(tail -2 <<< "$out")"
grep -qF -- "grlib-stixspw.tmtc-port=$B1" "$W/qargv.slot" &&
    grep -qF -- "$W/run-slot-s1/red/l4.dump.hex" "$W/pargv.slot"
check "fake slot: -s1 run dirs and the slot port" $? ""

echo "test-l4-replay: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
