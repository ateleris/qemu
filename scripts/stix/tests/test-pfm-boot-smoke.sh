#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Offline checks of pfm-boot-smoke.sh (flash latency plan F-5). The
# driver's rule self-test; the windows re-judged from canned driver logs
# (pfm-boot-drive.py --replay): the measured release-like timeline
# (0x14A3 @16, 0x142E @32, 0x0032 @70) must PASS with CONF sent at 75;
# 0x142E 26 s after 0x14A3 must FAIL, 24 s must PASS; 0x0032 missing by
# @150 must FAIL naming 0x0032 with no CONF sent; 0x0032 before the
# boot report 0x0022 must FAIL. The pre-flight SKIP and AX-ELF refusal
# (a stub nm provides the moveStartCoarseT symbol); the --dry-run
# command lines in legacy and slot mode; and fake-harness runs in slot 1
# (fresh sparse flash, the 0x0032 offset in the verdict, cleanup on
# PASS) plus the fake negative (latency=off in the real QEMU argv).
# Nothing here boots the real QEMU. Exit 0 when every check passes.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
S="$REPO_ROOT/scripts/stix"
GATE="$S/pfm-boot-smoke.sh"
DRIVE="$S/pfm-boot-drive.py"
FAKE="$S/tests/fake-qemu.sh"
UIDB=$((20000 + $(id -u)))
B1=$((UIDB + 100))
fails=0

check() { # name expected-rc actual-rc
    if [ "$2" = "$3" ]; then
        echo "ok: $1"
    else
        echo "FAIL: $1 (exit $3, want $2)"
        fails=$((fails + 1))
    fi
}

grep_check() { # name pattern text [absent]
    if printf '%s\n' "$3" | grep -qE -- "$2"; then
        [ "${4:-}" != absent ] && echo "ok: $1" && return
    else
        [ "${4:-}" = absent ] && echo "ok: $1" && return
    fi
    echo "FAIL: $1"
    fails=$((fails + 1))
}

[ -x "$GATE" ] || { echo "FAIL: $GATE missing or not executable"; exit 1; }

W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
chmod +x "$FAKE" 2>/dev/null || true
echo "dummy pfm elf" > "$W/asw_pfm.elf"     # no UART banner: release
echo "dummy ax elf" > "$W/asw_ax.elf"

# stubs: df for the slot disk guard, nm for the AX marker, ss so the
# port check sees no listener, and a driver stand-in that passes with a
# measured 0x0032 offset
printf '#!/bin/sh\necho Avail\necho 999999999\n' > "$W/df-ok"
chmod +x "$W/df-ok"
mkdir -p "$W/bin"
cat > "$W/bin/nm" <<'EOF'
#!/bin/sh
# the AX gate ELF's symbol (the att plan's refusal rule)
for f in "$@"; do
    case "${f##*/}" in asw_ax.elf) echo "00012340 g moveStartCoarseT" ;;
    esac
done
EOF
chmod +x "$W/bin/nm"
cat > "$W/fake-drive.py" <<'EOF'
# stands in for pfm-boot-drive.py: PASS, with a measured 0x0032 offset
import os
with open(os.environ["FAKE_DRIVE_RECORD"], "w") as f:
    f.write("cwd: %s\n" % os.getcwd())
    st = os.stat("flash.bin")   # the gate must have pre-created it
    f.write("flash-size: %d\n" % st.st_size)
    f.write("flash-alloc-kb: %d\n" % (st.st_blocks * 512 // 1024))
print("PFM 0x0032 at +62.0 s")
print("DRIVE: PASS")
EOF
cat > "$W/fake-drive-fail.py" <<'EOF'
# stands in for pfm-boot-drive.py: FAIL late
print("DRIVE: FAIL: no 0x0032 (Filesystem initialisation finished) by @150")
raise SystemExit(1)
EOF
PATH="$S/tests/stub-bin:$PATH"
export PATH

slotenv() { # cmd...: run in slot 1 with the test lock dir and df stub
    mkdir -p "$W/locks"
    env STIX_PORT_BASE=1 STIX_SLOT_LOCK_DIR="$W/locks" \
        STIX_DF_CMD="$W/df-ok" "$@"
}

# ---------------- the driver's rules ----------------

python3 "$DRIVE" --self-test > /dev/null
check "driver self-test" 0 $?

gen_log() { # file boot fs fdir tc20  (tc20 empty = never arrives; fs
            # may be past @150 to model "FS init still running")
    local f=$1 boot=$2 fs=$3 fdir=$4 tc20=$5 t anchor conf nom
    : > "$f"
    [ -z "$boot" ] ||
        echo "TM +$boot s 0x5A7 (5,1) 002200000008000000 [m=1000.000]" >> "$f"
    [ -z "$fs" ] || {
        echo "TM +$fs s 0x5A7 (5,1) 003201 [m=1000.000]" >> "$f"
        echo "TM +$fs s 0x5A7 (5,3) 8030 [m=1000.000]" >> "$f"
    }
    echo "TM +$fdir s 0x5A7 (5,1) 14a3 [m=1000.000]" >> "$f"
    if [ -n "$tc20" ]; then
        echo "TM +$tc20 s 0x5A7 (5,1) 142e [m=1000.000]" >> "$f"
        # CONF at max(0x142E, 0x0032) + 5 s, as the checker plans it
        if [ -z "$fs" ]; then anchor=$tc20; else
            if [ "$fs" -gt "$tc20" ]; then anchor=$fs; else anchor=$tc20; fi
        fi
        echo "SEND CONF at +$((anchor + 5)) s [m=1000.000]" >> "$f"
        conf=$((anchor + 6))
        echo "TM +$conf s 0x5A1 (1,1) 1dacc001 [m=1000.000]" >> "$f"
        echo "TM +$conf s 0x5A7 (5,1) 00210204 [m=1000.000]" >> "$f"
        echo "TM +$conf s 0x5A1 (1,7) 1dacc001 [m=1000.000]" >> "$f"
        echo "SEND S20 at +$((anchor + 15)) s [m=1000.000]" >> "$f"
        echo "TM +$((conf + 10)) s 0x5A1 (1,1) 1dacc002 [m=1000.000]" >> "$f"
        echo "TM +$((conf + 10)) s 0x5A1 (1,7) 1dacc002 [m=1000.000]" >> "$f"
        nom=$((anchor + 25))
        echo "SEND NOMINAL at +$nom s [m=1000.000]" >> "$f"
        echo "TM +$((nom + 1)) s 0x5A1 (1,1) 1dacc003 [m=1000.000]" >> "$f"
        echo "TM +$((nom + 1)) s 0x5A7 (5,1) 00210405 [m=1000.000]" >> "$f"
        echo "TM +$((nom + 1)) s 0x5A1 (1,7) 1dacc003 [m=1000.000]" >> "$f"
        # SID2 every 64 s: SID2_AFTER_NOM (default 1) places the first
        # one after NOMINAL; empty = it never comes
        [ -z "${SID2_AFTER_NOM-1}" ] || echo "TM +$((nom + ${SID2_AFTER_NOM:-1})) s 0x5A4 (3,25) 0215 [m=1000.000]" >> "$f"
    fi
    [ -z "$fs" ] && t=0 || t=$fs
    while [ "$t" -le 240 ]; do
        echo "TM +$t s 0x5E5 (3,25) 021500 [m=1000.000]" >> "$f"
        t=$((t + 1))
    done
}

# the measured release-ELF order: FDIR @16 and the TC20 timer @32 are
# NOT gated by FS init; FS init at 70 (debug: ~62 s UART time)
gen_log "$W/releaselike.log" 1 70 16 32
out="$(python3 "$DRIVE" --replay "$W/releaselike.log" --seconds 240 2>&1)"
check "replay: release-like timeline passes" 0 $?
grep_check "replay: release-like sends CONF at 75" \
    "^SEND CONF at \+75\.00 s$" "$out"
grep_check "replay: release-like prints the 0x0032 offset" \
    "^PFM 0x0032 at \+70\.0 s$" "$out"
grep_check "replay: release-like DRIVE: PASS last" "^DRIVE: PASS$" "$out"

# the measured PFM case: SID2 43 s after NOMINAL (64 s period) passes
SID2_AFTER_NOM=43 gen_log "$W/sid2late.log" 1 70 16 32
out="$(python3 "$DRIVE" --replay "$W/sid2late.log" --seconds 240 2>&1)"
check "replay: SID2 43 s after NOMINAL passes" 0 $?
grep_check "replay: SID2 43 s DRIVE: PASS last" "^DRIVE: PASS$" "$out"

SID2_AFTER_NOM= gen_log "$W/nosid2.log" 1 70 16 32
out="$(python3 "$DRIVE" --replay "$W/nosid2.log" --seconds 240 2>&1)"
check "replay: no SID2 after NOMINAL fails" 1 $?
grep_check "replay: no SID2 names the 70 s window" \
    "NOMINAL: no SID2 mode 5 within 70 s" "$out"

gen_log "$W/late26.log" 1 70 16 $((16 + 26))
out="$(python3 "$DRIVE" --replay "$W/late26.log" --seconds 240 2>&1)"
check "replay: 0x142E 26 s after 0x14A3 fails" 1 $?
grep_check "replay: 26 s names the window" \
    "no 0x142E \(Preparing for shutdown\) by 0x14A3 \+ 25 s" "$out"

gen_log "$W/late24.log" 1 70 16 $((16 + 24))
out="$(python3 "$DRIVE" --replay "$W/late24.log" --seconds 240 2>&1)"
check "replay: 0x142E 24 s after 0x14A3 passes" 0 $?
grep_check "replay: 24 s prints the 0x0032 offset" \
    "^PFM 0x0032 at \+70\.0 s$" "$out"
grep_check "replay: 24 s DRIVE: PASS last" "^DRIVE: PASS$" "$out"

gen_log "$W/nofs.log" 1 170 16 32
out="$(python3 "$DRIVE" --replay "$W/nofs.log" --seconds 240 2>&1)"
check "replay: 0x0032 missing by @150 fails (before any CONF)" 1 $?
grep_check "replay: missing 0x0032 names its window" \
    "no 0x0032 \(Filesystem initialisation finished\) by @150" "$out"
grep_check "replay: no CONF without 0x0032" "SEND CONF" "$out" absent

gen_log "$W/order.log" 15 10 13 ""
out="$(python3 "$DRIVE" --replay "$W/order.log" --seconds 240 2>&1)"
check "replay: 0x0032 before 0x0022 fails" 1 $?
grep_check "replay: ordering names both events" \
    "0x0032 .* arrived before the boot report 0x0022" "$out"

# ---------------- the gate's pre-flight ----------------

STIX_PFM_ELF=/nonexistent/pfm.elf "$GATE" "$FAKE" > /dev/null 2>&1
check "absent ELF is SKIP 77" 77 $?

out="$(PATH="$W/bin:$PATH" STIX_PFM_ELF="$W/asw_ax.elf" \
    "$GATE" "$FAKE" --dry-run 2>&1)"
check "AX gate ELF is refused" 2 $?
grep_check "AX refusal names the symbol" "moveStartCoarseT" "$out"

out="$(slotenv STIX_PFM_ELF="$W/asw_pfm.elf" \
    STIX_PFM_RUN_DIR="$HOME/dev/stix-state/uprev-run/x" \
    "$GATE" "$FAKE" --dry-run 2>&1)"
check "the persistent master's dir is refused as a run dir" 2 $?
grep_check "refusal says why" "inside the shared flash dir" "$out"

# ---------------- --dry-run command lines ----------------

out="$(STIX_PFM_ELF="$W/asw_pfm.elf" "$GATE" "$FAKE" --dry-run 2>&1)"
check "dry-run (legacy)" 0 $?
grep_check "dry-run: legacy run dir" \
    "^\(cd /tmp/stix-pfm-run && timeout 270 " "$out"
grep_check "dry-run: legacy port" " --port $UIDB " "$out"
grep_check "dry-run: -no-reboot" " -no-reboot " "$out"
grep_check "dry-run: watchdog trace" \
    "enable=stixctrl_watchdog_\*,file=/tmp/stix-pfm-run/wdg.trace" "$out"
grep_check "dry-run: serial" "file:/tmp/stix-pfm-run/uart\.pfm\.raw" "$out"
grep_check "dry-run: kernel" " -kernel $W/asw_pfm\.elf" "$out"
grep_check "dry-run: shipped latency default" "latency=" "$out" absent
grep_check "dry-run: driver 240 s" " --port $UIDB --seconds 240" "$out"

out="$(slotenv STIX_PFM_ELF="$W/asw_pfm.elf" STIX_PFM_RUN_DIR="$W/pfm" \
    "$GATE" "$FAKE" --dry-run 2>&1)"
check "dry-run (slot 1)" 0 $?
grep_check "dry-run slot: base line" "^slot: 1 base $B1$" "$out"
grep_check "dry-run slot: -s1 run dir" "^\(cd $W/pfm-s1 && timeout 270 " \
    "$out"
grep_check "dry-run slot: tmtc port global" \
    "-global grlib-stixspw.tmtc-port=$B1 " "$out"
grep_check "dry-run slot: trace in the slot dir" \
    "file=$W/pfm-s1/wdg\.trace" "$out"
grep_check "dry-run slot: driver port" " --port $B1 --seconds 240" "$out"

out="$(slotenv STIX_PFM_ELF="$W/asw_pfm.elf" STIX_PFM_RUN_DIR="$W/pfm" \
    "$GATE" "$FAKE" --dry-run --negative 2>&1)"
check "dry-run --negative (slot 1)" 0 $?
grep_check "negative: latency off" "grlib-stixflash.latency=off" "$out"
grep_check "negative: 30 s" "timeout 30 " "$out"
grep_check "negative: -neg run dir" "cd $W/pfm-neg-s1" "$out"
grep_check "negative: no driver" "pfm-boot-drive" "$out" absent

out="$(STIX_PFM_ELF="$W/asw_pfm.elf" "$GATE" "$FAKE" --dry-run \
    --no-nominal 2>&1)"
check "dry-run --no-nominal" 0 $?
grep_check "no-nominal reaches the driver" \
    "python3 -u .* --no-nominal > pfm\.drive\.log" "$out"

# ---------------- fake-harness runs, slot 1 ----------------
# each expectation is its own check with the actual value in the FAIL
# detail (nothing here is a compound chain)

# positive: fresh sparse flash seen by the stand-in driver, the offset in
# the verdict, the run dir removed on PASS
out="$(slotenv STIX_PFM_ELF="$W/asw_pfm.elf" STIX_PFM_RUN_DIR="$W/pfrun" \
    STIX_PFM_DRIVE="$W/fake-drive.py" FAKE_DRIVE_RECORD="$W/pos.record" \
    FAKE_QEMU_MODE=pfm FAKE_QEMU_RUN_SECONDS=3 \
    FAKE_QEMU_ARGV_FILE="$W/pos.argv" \
    bash "$GATE" "$FAKE" 2>&1)"
rc=$?
check "fake positive: rc" 0 "$rc"
grep_check "fake positive: slot line" "^slot: 1 base $B1$" "$out"
grep_check "fake positive: verdict with the 0x0032 offset" \
    "^PFM-BOOT-SMOKE: PASS \(release, 0x0032 at \+62\.0 s\) slot=1 " "$out"
check "fake positive: the driver ran in the gate's -s1 dir" \
    "$W/pfrun-s1" "$(sed -n 's/^cwd: //p' "$W/pos.record")"
check "fake positive: fresh flash full size" 17716740096 \
    "$(sed -n 's/^flash-size: //p' "$W/pos.record")"
alloc="$(sed -n 's/^flash-alloc-kb: //p' "$W/pos.record")"
if [ -n "$alloc" ] && [ "$alloc" -lt 65536 ]; then st=ok; else st="alloc=$alloc"; fi
check "fake positive: fresh flash is sparse (< 65536 KB allocated)" ok "$st"
grep_check "fake positive: -kernel in the QEMU argv" "^-kernel$" \
    "$(cat "$W/pos.argv")"
grep_check "fake positive: no latency flag in the QEMU argv" "latency" \
    "$(cat "$W/pos.argv")" absent
grep_check "fake positive: the flash existed for QEMU" \
    "^flash-inode: none$" "$(cat "$W/pos.argv")" absent
check "fake positive: the run dir is removed on PASS" "gone" \
    "$([ -e "$W/pfrun-s1" ] && echo there || echo gone)"

# a FAIL keeps the run dir for diagnosis
out="$(slotenv STIX_PFM_ELF="$W/asw_pfm.elf" STIX_PFM_RUN_DIR="$W/pffail" \
    STIX_PFM_DRIVE="$W/fake-drive-fail.py" FAKE_QEMU_MODE=pfm \
    FAKE_QEMU_RUN_SECONDS=3 bash "$GATE" "$FAKE" 2>&1)"
rc=$?
check "fake FAIL: rc" 1 "$rc"
grep_check "fake FAIL: verdict names the reason" \
    "^PFM-BOOT-SMOKE: FAIL \(release: no 0x0032 " "$out"
check "fake FAIL: the run dir is kept" "there" \
    "$([ -e "$W/pffail-s1" ] && echo there || echo gone)"

# negative: latency=off in the real argv, the watchdog reset, no driver
out="$(slotenv STIX_PFM_ELF="$W/asw_pfm.elf" STIX_PFM_RUN_DIR="$W/pfneg" \
    FAKE_QEMU_MODE=pfm-reset FAKE_QEMU_ARGV_FILE="$W/neg.argv" \
    bash "$GATE" "$FAKE" --negative 2>&1)"
rc=$?
check "fake negative: rc" 0 "$rc"
grep_check "fake negative: latency=off in the QEMU argv" \
    "^grlib-stixflash.latency=off$" "$(cat "$W/neg.argv")"
grep_check "fake negative: verdict" \
    "^PFM-BOOT-SMOKE \(negative\): PASS \(release\) slot=1 " "$out"
check "fake negative: the -neg run dir is removed on PASS" "gone" \
    "$([ -e "$W/pfneg-neg-s1" ] && echo there || echo gone)"

[ "$fails" -eq 0 ] && echo "TEST-PFM-BOOT-SMOKE: PASS" && exit 0
echo "TEST-PFM-BOOT-SMOKE: FAIL ($fails)"
exit 1
