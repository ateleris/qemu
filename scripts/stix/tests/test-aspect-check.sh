#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Host test for scripts/stix/aspect-check.py (aspect plan AS-4/AS-5):
# the self-test, then synthetic SSID 42 dumps — the good ideal dump
# (both request paths of D-ASi) and one dump per failure signature
# (summingFpga, averagingTc, a missing and an unexpected request path, a
# mid-series short packet, no full packet, a start-time step, the
# product and SGF shapes, the decode errors, --expect-none), then the
# AS-5 leg signatures (D-ASh rows 2-7: --expect-forced-abort/
# -forced-complete/-tiny/-epoch2/-collapse/-content-jump, each passing
# on its good shape and failing on its signature's absence). The dumps
# are built with the checker's own packet builders, so the test pins the
# CLI behaviour (exit codes, verdict lines), the self-test the parsing.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CHECK="$REPO_ROOT/scripts/stix/aspect-check.py"
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

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

# the synthetic dumps (aspect-check.py's builders; a decode failure in
# the builder itself would show as a missing dump below)
python3 - "$CHECK" "$TMP" <<'PY'
import importlib.util
import sys

spec = importlib.util.spec_from_file_location("aspect_check", sys.argv[1])
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
out = sys.argv[2]


def write(name, pkts):
    with open(f"{out}/{name}", "w") as f:
        for p in pkts:
            f.write(p.hex() + "\n")


def write_lines(name, lines):
    with open(f"{out}/{name}", "w") as f:
        f.write("\n".join(lines) + "\n")


def ql(seq, n=1):
    return [mod.build_packet(mod.ql34_body(n), seq=seq)]


# the good ideal dump: both request paths, QL-flush lines between
write("good.dump",
      ql(0, 2) + mod.burst(1, 120000, [510, 510, 3], seq=1) +
      ql(4) + mod.burst(0, 200000, [510, 2], seq=5))
# one failure signature per dump
write("summing.dump", mod.burst(1, 0, [510, 3], seq=0, summing=15))
write("averaging.dump", mod.burst(1, 0, [510, 3], seq=0, averaging=64))
write("no-urn0.dump", mod.burst(1, 0, [510, 3], seq=0))
write("extra-urn.dump", mod.burst(1, 0, [510, 3], seq=0) +
      mod.burst(7, 9000, [510, 3], seq=2))
write("short-mid.dump", mod.burst(1, 0, [509, 510, 3], seq=0))
write("no-full.dump", mod.burst(1, 0, [510, 510], seq=0))
write("step.dump", mod.burst(1, 0, [510, 510, 3], seq=0, jump=(1, 40)))
write("slack.dump", mod.burst(1, 0, [510, 510, 3], seq=0, jump=(1, 2)))
write("one-packet.dump", mod.burst(1, 0, [510], seq=0))
write("term.dump", mod.burst(1, 0, [510, 0], seq=0))
write("cont.dump", [mod.build_packet(
    mod.ssid42_body(*mod.ms_to_scet(0), 1, 16, 16, []), mod.SEG_CONT)])
write("firstfirst.dump", [
    mod.build_packet(mod.ssid42_body(*mod.ms_to_scet(0), 1, 16, 16, []),
                     mod.SEG_FIRST, 5),
    mod.build_packet(mod.ssid42_body(*mod.ms_to_scet(0), 1, 16, 16, []),
                     mod.SEG_FIRST, 6)])
write("never-closed.dump", [mod.build_packet(
    mod.ssid42_body(*mod.ms_to_scet(0), 1, 16, 16, []), mod.SEG_FIRST, 5)])
write("apidgap.dump", ql(3) + ql(5))
pkt = mod.build_packet(mod.ssid42_body(*mod.ms_to_scet(0), 1, 16, 16,
                                       [[1, 2, 3, 4]] * 2))
trunc = bytearray(pkt[:-2])
trunc[5] -= 2                       # the CCSDS length stays consistent
write_lines("trunc.dump", [bytes(trunc).hex()])
write("too-many.dump", [mod.build_packet(
    mod.ssid42_body(*mod.ms_to_scet(0), 1, 16, 16,
                    [[0] * 4] * (mod.MAX_SAMPLES + 1)))])
wrong = bytearray(pkt)
wrong[1] = 0xA7
write("wrong-apid.dump", [bytes(wrong)])
tcp = bytearray(pkt)
tcp[0] |= 0x10
write("tc-bit.dump", [bytes(tcp)])
write_lines("nothex.dump", ["zz"])
write("none-good.dump", ql(0))

# the AS-5 leg signatures (D-ASh rows 2-7); the first live runs are [L],
# so the good dumps assert direction/rough magnitude, not pinned counts
# row 2 (--jitter): FIRST(15) + LAST(0) one block apart
write("fa-good.dump", mod.burst_strided(1, 0, [15, 0], seq=0, stride=1008))
write("fa-full.dump", mod.burst(1, 0, [510, 3], seq=0))
write("fa-span.dump", mod.burst_strided(1, 0, [15, 0], seq=0,
                                        stride=130560))
# row 3 (--step-gap): one forced packet, the archive delivered
write("fc-good.dump", mod.burst(1, 0, [15, 510, 130], seq=0))
write("fc-two.dump", mod.burst(1, 0, [15, 510, 17, 130], seq=0))
# rows 4/7 epoch 1 (--drift, --step-1031): ~100 forced 0/1-sample
# packets, summing 15, averaging 64, a leading 0-sample one
write("tiny-good.dump", mod.burst_strided(1, 0, [0] + [1] * 99, seq=0,
                                          stride=1008, summing=15,
                                          averaging=64))
write("tiny-few.dump", mod.burst_strided(1, 0, [0] + [1] * 9, seq=0,
                                         stride=1008, summing=15,
                                         averaging=64))
write("tiny-big.dump", mod.burst_strided(
    1, 0, [0] + [1] * 39 + [3] + [1] * 59, seq=0, stride=1008,
    summing=15, averaging=64))
write("tiny-sum.dump", mod.burst_strided(1, 0, [0] + [1] * 99, seq=0,
                                         stride=1008, summing=16,
                                         averaging=64))
# row 7 epoch 2: one small consistent product
write("e2-good.dump", mod.burst_strided(2, 0, [62], seq=0, summing=16,
                                        averaging=64))
write("e2-sum.dump", mod.burst_strided(2, 0, [62], seq=0, summing=15,
                                       averaging=64))
# row 6 (--avg): the ~8x value step down, then a small rhythm
pre = [16 * (300 + i) for i in range(60)]
post = [2 * (360 + i) for i in range(12)]
write("col-good.dump", mod.a0_ramp_packets(1, pre + post, [36, 36, 0],
                                           seq=0, stride=1008))
write("col-flat.dump", mod.a0_ramp_packets(
    1, [16 * (900 + i) for i in range(72)], [36, 36, 0], seq=0,
    stride=1008))
# row 5 (--drop): one ~+3072 content jump in an otherwise smooth A0
smooth = [(64 * (i // 63) + i % 63) * 16 for i in range(600)]
jumped = smooth[:300] + [v + 3072 for v in smooth[300:]]
write("jump-good.dump", mod.a0_ramp_packets(1, jumped, [510, 90], seq=0))
write("jump-none.dump", mod.a0_ramp_packets(1, smooth, [510, 90], seq=0))
write("unclaimed.dump", mod.burst_strided(1, 0, [15, 0], seq=0,
                                          stride=1008) +
      mod.burst(7, 9000, [510, 3], seq=2))
PY

run() { # dump args...: runs the checker on $TMP/<dump>, sets OUT/RC
    OUT="$(python3 "$CHECK" "$TMP/$1" "${@:2}" 2>&1)"
    RC=$?
}

# 1. the self-test
OUT="$(python3 "$CHECK" --self-test 2>&1)"
check "self-test passes" $? "$OUT"

# 2. the good dump: the ideal signature on both paths (explicit and
# default URNs), and the verdict line
run good.dump --expect-ideal 1 0
[ "$RC" -eq 0 ] && echo "$OUT" | grep -q "^ASPECT-CHECK: PASS"
check "good: ideal, both paths, PASS" $? "$OUT"
run good.dump --expect-ideal
[ "$RC" -eq 0 ] && echo "$OUT" | grep -q "^ASPECT-CHECK: PASS"
check "good: ideal with the default URNs" $? "$OUT"
echo "$OUT" | grep -q "urn 1"
check "good: the report names urn 1" $? "$OUT"

# 3. each failure signature fails with its needle (a check mismatch or
# a decode error; the verdict prefix differs)
fail() { # dump args... needle name
    local n=$#
    local needle="${@:n-1:1}" name="${@:n:1}"
    run "$1" "${@:2:n-3}" # the checker flags only, not needle and name
    if [ "$RC" -eq 1 ] && echo "$OUT" | grep -q -- "$needle"; then
        check "$name" 0
    else
        check "$name" 1 "rc=$RC: $OUT"
    fi
}
fail summing.dump --expect-ideal "summingFpga 15, want 16" \
    "summingFpga 15 fails"
fail averaging.dump --expect-ideal "averagingTc 64, want 16" \
    "averagingTc 64 fails"
fail no-urn0.dump --expect-ideal "user_request_number 0: 0 product" \
    "a missing request path fails"
fail extra-urn.dump --expect-ideal "unexpected user_request_number" \
    "an unexpected request path fails"
fail short-mid.dump --expect-ideal "has 509 samples" \
    "a mid-series short packet fails"
fail no-full.dump --expect-ideal "the last packet has 510 samples" \
    "no short last packet fails"
fail step.dump --expect-ideal "want 130560" \
    "a start-time step fails"
fail one-packet.dump --expect-ideal "one STANDALONE packet" \
    "a one-packet burst fails"
fail cont.dump --expect-ideal "without a FIRST" \
    "CONT without FIRST fails (decode)"
fail firstfirst.dump --expect-ideal "still open" \
    "FIRST while open fails (decode)"
fail never-closed.dump --expect-ideal "never closed" \
    "FIRST without LAST fails (decode)"
fail apidgap.dump --expect-ideal "a gap in the APID" \
    "a lost QL packet fails (decode)"
fail trunc.dump --expect-ideal "truncated" \
    "a truncated sample array fails (decode)"
fail too-many.dump --expect-ideal "numberOfDynamicStructures" \
    "more than 510 samples fails (decode)"
fail wrong-apid.dump --expect-ideal "APID 0x5A7" \
    "a wrong APID fails (decode)"
fail tc-bit.dump --expect-ideal "a TC packet" \
    "a TC packet fails (decode)"
fail nothex.dump --expect-ideal "not hex" \
    "a non-hex line fails (decode)"

# 4. the shapes that pass: the +-3 ms slack and the empty termination
run slack.dump --expect-ideal
check "the +-3 ms truncation slack passes" "$?" "$OUT"
run term.dump --expect-ideal
check "an empty termination LAST passes" "$?" "$OUT"

# 5. --expect-none: only QL lines pass, one SSID 42 packet fails
run none-good.dump --expect-none
check "none: only QL-flush lines pass" "$?" "$OUT"
run good.dump --expect-none
[ "$RC" -eq 1 ] &&
    echo "$OUT" | grep -q "SSID 42 packet(s) in the dump, want 0"
check "none: SSID 42 present fails" $? "$OUT"

# 6. bad usage: both checks at once, no dump, a missing file
run good.dump --expect-ideal --expect-none
[ "$RC" -eq 2 ]
check "ideal + none together: exit 2" $? "rc=$RC: $OUT"
OUT="$(python3 "$CHECK" 2>&1)"
[ "$?" -eq 2 ]
check "no dump: exit 2" $? "$OUT"
OUT="$(python3 "$CHECK" "$TMP/absent.dump" --expect-none 2>&1)"
[ "$?" -eq 2 ] && echo "$OUT" | grep -q "ASPECT-CHECK: ERROR"
check "a missing dump: exit 2 and an ERROR line" $? "$OUT"

# 7. --samples prints the diode values
run good.dump --expect-ideal --samples
echo "$OUT" | grep -q "sample 0: 0000 0000 0000 0000"
check "--samples prints the diode values" $? "$OUT"

# 8. the AS-5 leg signatures (D-ASh rows 2-7): the good shape passes,
# each failure signature fails with its needle, and the NOTE lines
# carry the observed numbers
run fa-good.dump --expect-forced-abort 1
[ "$RC" -eq 0 ] && echo "$OUT" | grep -q "^ASPECT-CHECK: PASS" &&
    echo "$OUT" | grep -q "^ASPECT-CHECK NOTE: forced-abort urn 1:"
check "fa-good: forced-abort PASS with a NOTE" $? "$OUT"
fail fa-full.dump --expect-forced-abort 1 "no forced mid-series packet" \
    "a full-packet abort shape fails"
fail fa-span.dump --expect-forced-abort 1 "the product spans 130559 ms" \
    "a too-long abort span fails"
run fc-good.dump --expect-forced-complete 1
check "fc-good: forced-complete PASS" "$?" "$OUT"
fail fc-two.dump --expect-forced-complete 1 "want exactly 1" \
    "two forced packets fail"
run tiny-good.dump --expect-tiny 1
[ "$RC" -eq 0 ] && echo "$OUT" | grep -q \
    "NOTE: tiny urn 1: 100 packet(s): 1 x 0-sample, 99 x 1-sample"
check "tiny-good: tiny PASS with the leading 0-sample packet" $? "$OUT"
fail tiny-few.dump --expect-tiny 1 "10 packet(s), want" \
    "too few forced packets fails"
fail tiny-big.dump --expect-tiny 1 "carry more than 2 sample(s)" \
    "a 3-sample forced packet fails"
fail tiny-sum.dump --expect-tiny 1 "summingFpga 16, want 15" \
    "a clean estimate fails the tiny check"
run e2-good.dump --expect-epoch2 2
check "e2-good: epoch2 PASS" "$?" "$OUT"
fail e2-sum.dump --expect-epoch2 2 "summingFpga 15, want 16" \
    "a corrupted epoch-2 estimate fails"
run col-good.dump --expect-collapse 1
[ "$RC" -eq 0 ] && echo "$OUT" | grep -q \
    "NOTE: collapse urn 1: step"
check "col-good: collapse PASS with a NOTE" $? "$OUT"
fail col-flat.dump --expect-collapse 1 "want exactly 1" \
    "a flat series fails the collapse"
run jump-good.dump --expect-ideal 1 --expect-content-jump
[ "$RC" -eq 0 ] && echo "$OUT" | grep -q \
    "NOTE: content-jump urn 1: jump"
check "jump-good: ideal + content-jump PASS" $? "$OUT"
fail jump-none.dump --expect-ideal 1 --expect-content-jump \
    "0 content jump(s), want exactly 1" "a smooth series fails the jump"
fail unclaimed.dump --expect-forced-abort 1 0 "unclaimed product" \
    "an unclaimed urn fails"

echo "----------------------------------------"
echo "pass=$PASS fail=$FAIL"
[ "$FAIL" -eq 0 ]