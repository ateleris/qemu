#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Tests for scripts/stix/elut-derive.py (stimulus plan A2). Host only:
# no QEMU, no flash, and no flight CSV (D-Ed): every .dssevs here is
# synthetic, written to a temp dir with the gate's baseline pattern
# 1800 + 10 * dss_det, and every CSV is derived into the same temp dir.
#
# Cases: the tool's own self-test (the round-trip theorem, the W >= 32
# bound, the sensitivity predictions, the --b0 and --skip-sensitivity
# rules); derive from a synthetic .dssevs and from --base, the CSV
# re-parsing through elut-upload.py (384 rows); the refusals (a missing
# 0xE word, a bad --base key, a non-integer base, base 4065); --b0's
# offset column carrying the baseline into the encode; the gate's
# round trip: a synthetic dump built from the derived CSV the way
# test-elut-upload.sh does passes check-dump (exit 0), the identity
# mapping fails it with the predicted 348 differing rows (exit 1), and
# a uniform-baseline CSV needs --skip-sensitivity to be dump-checkable
# (exit 2 without the flag, 0/1 with it).
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
DERIVE="$REPO_ROOT/scripts/stix/elut-derive.py"
TOOL="$REPO_ROOT/scripts/stix/elut-upload.py"
PASS=0
FAIL=0
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

report() { # name ok|<failure detail>
    if [ "$2" = "ok" ]; then
        echo "ok   - $1"
        PASS=$((PASS + 1))
    else
        echo "FAIL - $1: $2"
        FAIL=$((FAIL + 1))
    fi
}

# a synthetic .dssevs: one 0xE setup word per dss_det (base = 1800 +
# 10*d), then n events at adc on fpga_det 0, pix 0, one per 100 ms tick
# (2442 x 2047 + 1226 = 5e6 units of 20 ns of padding per event)
dssevs() { # path n [skip-det]
    python3 - "$@" <<'EOF'
import struct, sys
path, n = sys.argv[1], int(sys.argv[2])
skip = int(sys.argv[3]) if len(sys.argv) > 3 else -1
with open(path, "wb") as f:
    for d in range(32):
        if d == skip:
            continue
        f.write(struct.pack(">I", ((1800 + 10 * d) << 20)
                            | (0xE << 16) | (d << 11)))
    for _ in range(n):
        f.write(struct.pack(">I", 2050 << 20))
        for _ in range(2442):
            f.write(struct.pack(">I", (0xF << 16) | 2047))
        f.write(struct.pack(">I", (0xF << 16) | 1226))
EOF
}

# 1. the tool's self-test
out="$(python3 "$DERIVE" --self-test 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && grep -q '^DERIVE SELF-TEST: PASS' <<< "$out"; then
    report "self-test" ok
else
    report "self-test" "rc=$rc: $(tail -3 <<< "$out" | tr '\n' ' ')"
fi

# 2. derive from a synthetic .dssevs: the CSV parses (384 rows), the
# record line names the file and the D-Eb default
dssevs "$T/s.dssevs" 4
out="$(python3 "$DERIVE" derive --dssevs "$T/s.dssevs" \
    --out "$T/derived.csv" 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && grep -q 'rows 384' <<< "$out" &&
        grep -q 'offset 0 (D-Eb)' <<< "$out"; then
    report "derive --dssevs" ok
else
    report "derive --dssevs" "rc=$rc: $out"
fi
n="$(python3 "$TOOL" check-dump --csv "$T/derived.csv" --dump /dev/null \
    2>&1 | grep -c 'want 1572864' || true)"
if [ "$n" -eq 1 ]; then
    report "derived CSV parses with elut-upload.py (384 rows)" ok
else
    report "derived CSV parses with elut-upload.py" "check-dump: $n"
fi

# 3. derive --base: all 32 keys required, unknown keys refused
BASE0="$(python3 -c 'print("{" + ", ".join(chr(34) + str(i) + chr(34) +
                                     ": 0" for i in range(32)) + "}")')"
out="$(python3 "$DERIVE" derive --base "$BASE0" --out "$T/base0.csv" 2>&1)"
[ $? -eq 0 ] && report "derive --base (32 keys)" ok ||
    report "derive --base (32 keys)" "$out"
BAD="$(python3 -c 'print("{" + ", ".join(chr(34) + str(i) + chr(34) +
                                        ": 0" for i in range(31)) + "}")')"
err="$(python3 "$DERIVE" derive --base "$BAD" --out "$T/x.csv" 2>&1)"
[ $? -eq 2 ] && grep -q 'dss_det 31' <<< "$err" &&
    report "derive --base: a missing key refused" ok ||
    report "derive --base: a missing key refused" "$err"
UNK="$(python3 -c 'print("{" + ", ".join(chr(34) + str(i) + chr(34) +
                                        ": 0" for i in range(33)) + "}")')"
err="$(python3 "$DERIVE" derive --base "$UNK" --out "$T/x.csv" 2>&1)"
[ $? -eq 2 ] && grep -q 'unknown dss_det' <<< "$err" &&
    report "derive --base: an unknown key refused" ok ||
    report "derive --base: an unknown key refused" "$err"
HI="$(python3 -c 'print("{" + ", ".join(chr(34) + str(i) + chr(34) +
     ": " + ("4065" if i == 31 else "0") for i in range(32)) + "}")')"
err="$(python3 "$DERIVE" derive --base "$HI" --out "$T/x.csv" 2>&1)"
[ $? -eq 2 ] && grep -q 'dss_det 31' <<< "$err" &&
    report "derive --base: base 4065 (W < 32) refused" ok ||
    report "derive --base: base 4065 (W < 32) refused" "$err"

# 4. a .dssevs missing a 0xE word is refused, naming the dss_det
dssevs "$T/gap.dssevs" 1 7
err="$(python3 "$DERIVE" derive --dssevs "$T/gap.dssevs" \
    --out "$T/x.csv" 2>&1)"
[ $? -eq 2 ] && grep -q 'dss_det 7' <<< "$err" &&
    report "a missing 0xE word refused" ok ||
    report "a missing 0xE word refused" "$err"

# 5. --b0: the offset column carries the baseline; the encode honours
# it (the B[0] byte), the default stays byte-identical to D-Eb
python3 "$DERIVE" derive --dssevs "$T/s.dssevs" --b0 \
    --out "$T/b0.csv" > /dev/null 2>&1
python3 - "$TOOL" "$T/derived.csv" "$T/b0.csv" > "$T/b0.check" <<'EOF'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("elut_upload", sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
poffs = {}
plain = m.parse_csv_file(sys.argv[2], offsets=poffs)
offs = {}
b0 = m.parse_csv_file(sys.argv[3], offsets=offs)
# the plain CSV carries no honoured B0; the --b0 one an integral
# per-row baseline (keyed by dss_det, so compare per (subcol, pix) row)
ok = m.b0_of_offsets(poffs) is None
b0_of = m.b0_of_offsets(offs)
ok = ok and b0_of is not None and len(b0_of) == 384 and \
    all(b0_of[k] == int(offs[k]) for k in b0_of) and \
    len(set(b0_of.values())) == 32
u_plain = m.elut_units(plain, 0x010)
u_b0 = m.elut_units(b0, 0x010, b0_of=m.b0_lookup(b0_of))
ok = ok and u_plain != u_b0
ram = {}
for u in u_b0:
    m.fsw_process_elut(u[4:][10:-2], ram)
ok = ok and all(ram[k][0] == b0_of[k] for k in ram)
print("OK" if ok else "BAD")
EOF
[ "$(cat "$T/b0.check")" = OK ] &&
    report "--b0: the offset column reaches the encode as B[0]" ok ||
    report "--b0: the offset column reaches the encode as B[0]" \
        "$(cat "$T/b0.check")"
out="$(python3 "$TOOL" encode --csv "$T/derived.csv" --emit hex \
    2>&1 >/dev/null)"
grep -q 'B0=0 B32=0' <<< "$out" &&
    report "without --b0 the record keeps B0=0 (D-Eb)" ok ||
    report "without --b0 the record keeps B0=0 (D-Eb)" "$out"

# 6. the gate's round trip: the derived CSV's dump passes check-dump;
# the identity mapping fails it with the predicted 348 rows; a
# uniform-baseline CSV needs --skip-sensitivity
python3 "$DERIVE" derive --base "$(python3 -c 'print("{" + ", ".join(
    chr(34) + str(i) + chr(34) + ": 1800" for i in range(32)) + "}")')" \
    --out "$T/uniform.csv" > /dev/null 2>&1
python3 - "$TOOL" "$T/derived.csv" "$T/good.bin" "$T/ident.bin" \
    "$T/uniform.csv" "$T/uniform.bin" <<'EOF'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("elut_upload", sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
rows = m.parse_csv_file(sys.argv[2])
with open(sys.argv[3], "wb") as f:
    f.write(m.fpga_elut(rows, m.SUBCOL_FROM_FPGA_DET))
with open(sys.argv[4], "wb") as f:
    f.write(m.fpga_elut(rows, m.IDENTITY))
urows = m.parse_csv_file(sys.argv[5])
with open(sys.argv[6], "wb") as f:
    f.write(m.fpga_elut(urows, m.SUBCOL_FROM_FPGA_DET))
EOF
dump_rc() { # want name csv args...
    local want="$1" name="$2" csv="$3" rc out
    shift 3
    out="$(python3 "$TOOL" check-dump --csv "$csv" "$@" 2>&1)"
    rc=$?
    if [ "$rc" -eq "$want" ] && ! grep -q Traceback <<< "$out"; then
        report "check-dump $name" ok
    else
        report "check-dump $name" "rc=$rc want $want: $(tail -2 <<< "$out")"
    fi
}
dump_rc 0 "derived CSV, correct dump" "$T/derived.csv" --dump "$T/good.bin"
dump_rc 1 "derived CSV, identity-mapping dump" "$T/derived.csv" \
    --dump "$T/ident.bin"
out="$(python3 "$TOOL" check-dump --csv "$T/derived.csv" \
    --dump "$T/ident.bin" 2>&1)"
grep -q 'bad rows 348' <<< "$out" &&
    report "the identity dump fails with the predicted 348 rows" ok ||
    report "the identity dump fails with the predicted 348 rows" "$out"
dump_rc 2 "uniform baselines: the D-Eg trap" "$T/uniform.csv" \
    --dump "$T/uniform.bin"
dump_rc 0 "uniform baselines: --skip-sensitivity passes" "$T/uniform.csv" \
    --dump "$T/uniform.bin" --skip-sensitivity
out="$(python3 "$TOOL" check-dump --csv "$T/uniform.csv" \
    --dump "$T/uniform.bin" --skip-sensitivity 2>&1)"
grep -q 'skipped (--skip-sensitivity' <<< "$out" &&
    report "--skip-sensitivity says so on the record" ok ||
    report "--skip-sensitivity says so on the record" "$out"

echo "test-elut-derive: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
