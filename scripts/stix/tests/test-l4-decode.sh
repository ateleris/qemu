#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Tests for scripts/stix/l4-decode.py (L4 plan D-L4b,
# docs/stix-port/2026-11-13-l4-replay-plan.md).  Host only: no QEMU,
# no flash, and no bundle files (the gh1061 evidence is read-only and
# stays outside the repo; the SSID-24 wire format follows the FSW
# cites in the tool's docstring, TM_21.c:23, :37-43, :930-936).
#
# Cases: the tool's self-test; golden packets assembled here from the
# SSID-24 layout (independent of the tool's own test builder) through
# parse and report --request A (expected set vs a hand-written truth
# file, TRUTH round math); report --request B ghost rules (primary,
# rel-times-only named, no with the byte offset); the 0x543F event
# count; malformed-line and usage rejections.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TOOL="$REPO_ROOT/scripts/stix/l4-decode.py"
PASS=0
FAIL=0
TMP="$(mktemp -d /tmp/test-l4-decode.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

report() { # name ok|<failure detail>
    if [ "$2" = "ok" ]; then
        echo "ok   - $1"
        PASS=$((PASS + 1))
    else
        echo "FAIL - $1: $2"
        FAIL=$((FAIL + 1))
    fi
}

# Golden fixtures, assembled here from the layout only: one whole
# CCSDS packet per line (the probe --dump-apid framing, SSID at byte
# 16).  Req 42 carries two structures (S1, S2); req 43 carries S2
# alone -- the byte-identical "ghost" of 42's last structure -- under
# its own start time 500; the third line is an event packet (skipped).
python3 - "$TMP" <<'EOF'
import struct
import sys

tmp = sys.argv[1]

def l4_line(req, psc, start, structs):
    d = bytearray([24])
    d += struct.pack(">HHI", 91, psc, req)     # PID, PSC, request
    d += bytes([43, 43])                       # comp schema, trigger
    d += struct.pack(">IH", start, 0)          # start coarse, fine
    d += struct.pack(">H", len(structs))
    for pixel, det, rcr, ebin, samples, closing in structs:
        d += struct.pack(">HIBHH", pixel, det, rcr, ebin, len(samples))
        for rel, trig, counts in samples:
            d += struct.pack(">I", rel)[1:]
            d += bytes([trig, len(counts)]) + bytes(counts)
        d += struct.pack(">H", closing)
    head = struct.pack(">HHH", 0x0DBC, 0xC000, len(d) + 9)
    pus = bytes([0x10, 21, 6, 0]) + struct.pack(">IH", start + 7, 0)
    return (head + pus + bytes(d)).hex()

def ev_line(eid, ptype=5, sub=2, params=b""):
    app = struct.pack(">H", eid) + params
    head = struct.pack(">HHH", 0x0DA7, 0xC000, len(app) + 9)
    pus = bytes([0x10, ptype, sub, 0]) + struct.pack(">IH", 1000, 0)
    return (head + pus + app).hex()

S1 = (0, 0xFFFFFFFF, 0, 0x03E1,
      [(3, 7, [1, 2]), (53, 7, [3, 4]), (103, 7, [5, 6])], 20)
S2 = (0xFFF, 0x0FFFFFFF, 2, 0x3E0, [(203, 1, [9] * 16)], 50)
S2_NO = (0xFFF, 0x0FFFFFFF, 2, 0x3E0, [(203, 1, [9] * 15 + [8])], 50)
S2_REL = (0xFFF, 0x0FFFFFFF, 2, 0x3E0, [(204, 1, [9] * 16)], 50)

with open(tmp + "/cap-good.asc", "w") as f:
    f.write(l4_line(42, 0xC001, 100, [S1, S2]) + "\n")
    f.write(l4_line(43, 0xC002, 500, [S2]) + "\n")
    f.write(ev_line(0x543F, params=b"\x00\x01") + "\n")
with open(tmp + "/cap-ghost-no.asc", "w") as f:
    f.write(l4_line(42, 0xC001, 100, [S1, S2]) + "\n")
    f.write(l4_line(43, 0xC002, 500, [S2_NO]) + "\n")
with open(tmp + "/cap-ghost-rel.asc", "w") as f:
    f.write(l4_line(42, 0xC001, 100, [S1, S2]) + "\n")
    f.write(l4_line(43, 0xC002, 500, [S2_REL]) + "\n")
with open(tmp + "/truth-good", "w") as f:
    f.write("# truth\n"
            "chunk 100.3 0\nchunk 105.3 0\nchunk 110.3 0\n"
            "chunk 120.3 0\n"
            "file RAW_COUNTS_a 100.3 3\nfile RAW_COUNTS_b 110.3 1\n")
with open(tmp + "/truth-long", "w") as f:
    # one chunk the TM does not carry: the #1061 loss signature
    f.write("chunk 100.3 0\nchunk 105.3 0\nchunk 110.3 0\n"
            "chunk 115.3 0\nchunk 120.3 0\n"
            "file RAW_COUNTS_a 100.3 3\nfile RAW_COUNTS_b 110.3 2\n")
with open(tmp + "/truth-short", "w") as f:
    f.write("chunk 100.3 0\nchunk 105.3 0\nchunk 110.3 0\n"
            "file RAW_COUNTS_a 100.3 2\nfile RAW_COUNTS_b 110.3 1\n")
with open(tmp + "/events", "w") as f:
    f.write(ev_line(0x543F, params=b"\x04\x01") + "\n")
    f.write(ev_line(0x14A3, ptype=5, sub=1) + "\n")
with open(tmp + "/cap-bad.asc", "w") as f:
    f.write("0DBC000100\n")            # 5 B: shorter than a header
with open(tmp + "/cap-cut.asc", "w") as f:
    good = l4_line(42, 0xC001, 100, [S1, S2])
    f.write(good[:-4] + "\n")          # closing offset chopped
EOF

# 1. the tool's self-test
out="$(python3 "$TOOL" self-test 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && grep -q '^SELF-TEST: PASS' <<< "$out"; then
    report "self-test" ok
else
    report "self-test" "rc=$rc: $(tail -3 <<< "$out" | tr '\n' ' ')"
fi

# 2. parse: the golden capture's table, hand-derived from the layout
out="$(python3 "$TOOL" parse --capture "$TMP/cap-good.asc" 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && grep -qF "lines=3 packets=2 skipped=1" <<< "$out"; then
    report "parse capture line" ok
else
    report "parse capture line" "rc=$rc: $(head -1 <<< "$out")"
fi
want="REQUEST 42: packets=1 samples=4 rel=3..203 dups=0"
want="$want rcr=[0,2] ebins=[0x3e0,0x3e1] pixel=[0x0,0xfff]"
want="$want closing=[20,50]"
if grep -qF "$want" <<< "$out"; then
    report "parse REQUEST 42" ok
else
    report "parse REQUEST 42" "want: $want"
fi
want="REQUEST 43: packets=1 samples=1 rel=203..203 dups=0"
want="$want rcr=[2] ebins=[0x3e0] pixel=[0xfff] closing=[50]"
if grep -qF "$want" <<< "$out"; then
    report "parse REQUEST 43" ok
else
    report "parse REQUEST 43" "want: $want"
fi

# 3. report A: expected set vs the truth file (complete and short)
out="$(python3 "$TOOL" report --capture "$TMP/cap-good.asc" \
    --request A --truth "$TMP/truth-good" 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && grep -qF "REQUEST 42 (A)" <<< "$out" && \
        grep -qF "EXPECTED-SET: PASS missing=0 extra=0" <<< "$out" && \
        grep -qF "(expected=4 decoded=4)" <<< "$out"; then
    report "report A complete" ok
else
    report "report A complete" "rc=$rc: $(tail -2 <<< "$out" | tr '\n' ' ')"
fi
if grep -qF "lastround=4 red-round=5" <<< "$out"; then
    report "TRUTH round math" ok
else
    report "TRUTH round math" "$(grep TRUTH <<< "$out")"
fi
out="$(python3 "$TOOL" report --capture "$TMP/cap-good.asc" \
    --request A --truth "$TMP/truth-short" 2>&1)"
out="$(python3 "$TOOL" report --capture "$TMP/cap-good.asc" \
    --request A --truth "$TMP/truth-long" 2>&1)"
if grep -qF "EXPECTED-SET: FAIL missing=1 extra=0" <<< "$out" && \
        grep -qF "(expected=5 decoded=4)" <<< "$out"; then
    report "report A missing chunk (loss)" ok
else
    report "report A missing chunk (loss)" "$(grep EXPECTED <<< "$out")"
fi
# truth one chunk short of the capture: the decoded sample without a
# truth chunk is an extra (data the truth does not account for)
out="$(python3 "$TOOL" report --capture "$TMP/cap-good.asc" \
    --request A --truth "$TMP/truth-short" 2>&1)"
if grep -qF "EXPECTED-SET: FAIL missing=0 extra=1" <<< "$out" && \
        grep -qF "(expected=3 decoded=4)" <<< "$out"; then
    report "report A truth short: extra sample" ok
else
    report "report A truth short: extra sample" "$(grep EXPECTED <<< "$out")"
fi

# 4. report B: the ghost rules of D-L4b
out="$(python3 "$TOOL" report --capture "$TMP/cap-good.asc" \
    --request B --req-number 43 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && grep -qF "REQUEST 43 (B)" <<< "$out" && \
        grep -qF "GHOST: primary - 1 B structure(s)" <<< "$out" && \
        grep -qF "byte-identical to A's last (req 42, 34 B)" <<< "$out"; then
    report "ghost primary" ok
else
    report "ghost primary" "rc=$rc: $(grep GHOST <<< "$out")"
fi
out="$(python3 "$TOOL" report --capture "$TMP/cap-ghost-rel.asc" \
    --request B --req-number 43 2>&1)"
if grep -qF "GHOST: rel-times-only" <<< "$out" && \
        grep -qF "B sample 0 rel 204 vs 203" <<< "$out"; then
    report "ghost rel-times-only named" ok
else
    report "ghost rel-times-only named" "$(grep GHOST <<< "$out")"
fi
out="$(python3 "$TOOL" report --capture "$TMP/cap-ghost-no.asc" \
    --request B --req-number 43 2>&1)"
if grep -qF "GHOST: no - first difference" <<< "$out" && \
        grep -qF "structure byte 31: 08 vs 09" <<< "$out"; then
    report "ghost no, offset named" ok
else
    report "ghost no, offset named" "$(grep GHOST <<< "$out")"
fi

# 5. the 0x543F event count
out="$(python3 "$TOOL" report --capture "$TMP/cap-good.asc" \
    --request A --events "$TMP/events" 2>&1)"
if grep -qF "packets=2 tm52=1 eid543f=1" <<< "$out"; then
    report "event 0x543F count" ok
else
    report "event 0x543F count" "$(grep EVENTS <<< "$out")"
fi

# 6. malformed input fails loudly (rc 1, the line named)
err="$(python3 "$TOOL" parse --capture "$TMP/cap-bad.asc" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] && grep -q "shorter than a CCSDS header" <<< "$err"; then
    report "short line refused" ok
else
    report "short line refused" "rc=$rc: $err"
fi
err="$(python3 "$TOOL" parse --capture "$TMP/cap-cut.asc" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] && grep -q "truncated packet" <<< "$err"; then
    report "cut packet refused" ok
else
    report "cut packet refused" "rc=$rc: $err"
fi

# 7. usage: --request B needs --req-number (rc 2)
err="$(python3 "$TOOL" report --capture "$TMP/cap-good.asc" \
    --request B 2>&1)"
rc=$?
if [ "$rc" -eq 2 ] && grep -q -- "--req-number" <<< "$err"; then
    report "B without req-number refused" ok
else
    report "B without req-number refused" "rc=$rc: $err"
fi

echo "test-l4-decode: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]