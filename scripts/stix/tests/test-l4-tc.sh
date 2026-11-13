#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Tests for scripts/stix/l4-tc.py (L4 plan D-L4a,
# docs/stix-port/2026-11-13-l4-replay-plan.md). Host only: no QEMU, no
# flash, and no bundle files (the gh1061 evidence is read-only and
# stays outside the repo; the 611/711 vector values are transcribed
# from the plan). The golden units are assembled independently here,
# like test-elut-upload.sh's GOLD.
#
# Cases: the tool's self-test; golden TC(238,1) 611/711 and TC(237,13)
# units (frame + app data + CRC, one per CLI hex line); the
# probe-args layout; the FSW-mirror rejections (TM_238.c:69-75 box
# checks, field ranges, 14-bit seq).
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TOOL="$REPO_ROOT/scripts/stix/l4-tc.py"
PASS=0
FAIL=0

report() { # name ok|<failure detail>
    if [ "$2" = "ok" ]; then
        echo "ok   - $1"
        PASS=$((PASS + 1))
    else
        echo "FAIL - $1: $2"
        FAIL=$((FAIL + 1))
    fi
}

# Golden TC(238,1) unit for req/seq/start/box, assembled here from the
# layout only (TM_238.c:22-58 field order, big-endian).
golden_req() { # req seq start tmin tmax tunit emin emax eunit1
    python3 - "$@" <<'EOF'
import sys
v = [int(x, 0) for x in sys.argv[1:]]
req, seq, start, tmin, tmax, tunit, emin, emax, eunit1 = v

def crc(data):
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else c << 1
            c &= 0xFFFF
    return c

box = (tmin.to_bytes(4, "big")[1:] + tmax.to_bytes(4, "big")[1:]
       + tunit.to_bytes(2, "big")
       + ((emin << 10) | (emax << 5) | eunit1).to_bytes(2, "big"))
app = (req.to_bytes(4, "big") + bytes([4]) + start.to_bytes(4, "big")
       + bytes(2) + b"\xff\xff\xff\xff" + bytes([1]) + box)
pkt = (bytes([0x1D, 0xAC, 0xC0 | (seq >> 8), seq & 0xFF,
              0, len(app) + 5]) + bytes([0x19, 238, 1, 0]) + app)
print(" ".join("%02X" % b for b in
               bytes([0x40, 2, 0, 0]) + pkt
               + crc(pkt).to_bytes(2, "big")))
EOF
}

# Golden TC(237,13) unit for a pixel mask, seq 0x200 (the collect
# line's other values are baked in).
golden_flare() { # pixel-mask
    python3 - "$1" <<'EOF'
import sys
mask = int(sys.argv[1], 0)

def crc(data):
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else c << 1
            c &= 0xFFFF
    return c

app = (bytes.fromhex("0004003100000001ffffffffffffffff")
       + bytes([0, 0, 0]) + bytes(12)     # sel start/stop, imaging det
       + bytes([0, 0, (mask >> 8) & 0x0F, mask & 0xFF]) + bytes(4))
pkt = (bytes([0x1D, 0xAC, 0xC2, 0x00, 0x00, len(app) + 5])
       + bytes([0x19, 237, 13, 0]) + app)
print(" ".join("%02X" % b for b in
               bytes([0x40, 2, 0, 0]) + pkt
               + crc(pkt).to_bytes(2, "big")))
EOF
}

# 1. the tool's self-test
out="$(python3 "$TOOL" self-test 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && grep -q '^SELF-TEST: PASS' <<< "$out"; then
    report "self-test" ok
else
    report "self-test" "rc=$rc: $(tail -3 <<< "$out" | tr '\n' ' ')"
fi

# 2. golden 611 (the capture's GREEN full-window request) and 711
GOLD_611="$(golden_req 611 0x201 595147086 0 90000 1 0 31 1)"
GOLD_711="$(golden_req 711 0x204 595147086 0 90000 1 0 31 1)"
hex611="$(python3 "$TOOL" request --req 611 --start 595147086 \
    --box 0,90000,1,0,31,1 --seq 0x201 --emit hex 2> /dev/null)"
hex711="$(python3 "$TOOL" request --req 711 --start 595147086 \
    --box 0,90000,1,0,31,1 --seq 0x204 --emit hex 2> /dev/null)"
if [ "$hex611" = "$GOLD_611" ]; then
    report "golden 611 unit" ok
else
    report "golden 611 unit" "got: $hex611"
fi
if [ "$hex711" = "$GOLD_711" ]; then
    report "golden 711 unit" ok
else
    report "golden 711 unit" "got: $hex711"
fi
# the two capture requests: same length, different frame and app bytes
if [ "${#GOLD_611}" -eq "${#GOLD_711}" ] && [ "$GOLD_611" != "$GOLD_711" ];
then
    report "711/611 same length, different bytes" ok
else
    report "711/611 same length, different bytes" \
        "len ${#GOLD_611}/${#GOLD_711}"
fi

# 3. golden flare units: the collect line, pixel masks 0xFFF and 0
GOLD_FLARE="$(golden_flare 0xFFF)"
hexfl="$(python3 "$TOOL" flare --emit hex 2> /dev/null)"
if [ "$hexfl" = "$GOLD_FLARE" ]; then
    report "golden flare unit (pixel mask 0xFFF)" ok
else
    report "golden flare unit (pixel mask 0xFFF)" "got: $hexfl"
fi
GOLD_FLARE0="$(golden_flare 0)"
hexfl0="$(python3 "$TOOL" flare --pixel-mask 0 --emit hex 2> /dev/null)"
if [ "$hexfl0" = "$GOLD_FLARE0" ]; then
    report "flare --pixel-mask 0 (the capture line)" ok
else
    report "flare --pixel-mask 0 (the capture line)" "got: $hexfl0"
fi

# 4. probe-args layout: one send + both acks, ack prefix 1DACC201
mapfile -t A < <(python3 "$TOOL" request --req 611 --start 595147086 \
    --box 0,90000,1,0,31,1 --seq 0x201 --t0 70 --emit probe-args \
    2> /dev/null)
if [ "${#A[@]}" -eq 8 ] && [ "${A[0]}" = "--send-at" ] &&
        [ "${A[1]}" = "70" ] && [ "${A[2]}" = "--send-hex" ] &&
        [ "${A[3]}" = "$GOLD_611" ] &&
        [ "${A[4]}" = "--expect" ] &&
        [ "${A[5]}" = "0x5A1:1:1:1DACC201@5" ] &&
        [ "${A[6]}" = "--expect" ] &&
        [ "${A[7]}" = "0x5A1:1:7:1DACC201@5" ]; then
    report "probe-args layout" ok
else
    report "probe-args layout" "n=${#A[@]}: ${A[*]:0:8}"
fi

# 5. rejections: rc 2, the error names the field
bad() { # name want-text args...
    local name="$1" want="$2" err rc
    shift 2
    err="$(python3 "$TOOL" "$@" --emit hex 2>&1 > /dev/null)"
    rc=$?
    if [ "$rc" -eq 2 ] && grep -qF -- "$want" <<< "$err"; then
        report "reject $name" ok
    else
        report "reject $name" "rc=$rc, want 2 and '$want'; got: $err"
    fi
}
bad "tmin >= tmax" "tmin 90000 >= tmax 0" \
    request --req 611 --start 595147086 --box 90000,0,1,0,31,1
bad "tunit 0" "tunit 0" \
    request --req 611 --start 595147086 --box 0,90000,0,0,31,1
bad "emin > emax" "emin 31 > emax 0" \
    request --req 611 --start 595147086 --box 0,90000,1,31,0,1
bad "tmin beyond u24" "tmin 16777216 outside 0..16777215" \
    request --req 611 --start 595147086 --box 16777216,16777217,1,0,31,1
bad "no --box" "at least one box" \
    request --req 611 --start 595147086
bad "pixel mask" "pixel mask 4096 outside 0..4095" \
    flare --pixel-mask 4096
bad "seq not 14-bit" "not 14-bit" \
    request --req 611 --start 595147086 --seq 16384 \
    --box 0,90000,1,0,31,1

echo "test-l4-tc: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]