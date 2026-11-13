#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Tests for scripts/stix/elut-upload.py (ELUT plan E-1,
# docs/stix-port/2026-11-11-elut-plan.md). Host only: no QEMU, no flash,
# and no flight CSV (D-Ed): every CSV here is synthetic, written to a
# temp dir with the self-test's edge pattern 100 + 60k + (12*subcol + pix).
#
# Cases: the tool's own self-test; the CSV parser on good and bad files
# (LF, CRLF, a leading-space preamble line, a wrong header name, 34
# columns, 383 rows, a duplicate (subcol, pix) pair, a non-monotone row,
# edges 0 and 4096), each with its error text; a golden first TC(237,7)
# unit; the probe-args layout; check-dump's exit codes on a correct, a
# constant and a wrong-size dump; the Python index tables pinned against
# the model's transcription in hw/sparc/stix_det.c (E-4); and, when
# STIX_FSW_SRC points at the FSW's ASW/ASW/src (read-only), the same
# tables against utils.c.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
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

# synth CSV variant -> $T/<variant>.csv. Variants change one thing each.
synth() { # variant
    python3 - "$1" "$T/$1.csv" <<'EOF'
import sys
variant, path = sys.argv[1], sys.argv[2]
eol = "\r\n" if variant != "lf" else "\n"
pre = ["Created from synthetic.fits", " Channel energy edges, synthetic"]
hdr = ["Offset (ADC)", "Gain (keV/ADC)", "Pixel", "Detector"] + [
    "ADC Edge #%d - %d keV" % (k, k + 4) for k in range(31)]
if variant == "badheader":
    hdr[3] = "Det"
if variant == "cols34":
    hdr = hdr[:34]
lines = pre + [",".join(hdr)]
for subcol in range(32):
    for pix in range(12):
        e = [100 + 60 * k + (12 * subcol + pix) for k in range(31)]
        if variant == "dup" and (subcol, pix) == (3, 4):
            pix_cell = 5
        else:
            pix_cell = pix
        if variant == "nonmono" and (subcol, pix) == (1, 5):
            e[12] = e[11]
        if variant == "edge0" and (subcol, pix) == (0, 0):
            e[0] = 0
        if variant == "edge4096" and (subcol, pix) == (31, 11):
            e[30] = 4096
        row = ["900.5", "0.11", str(pix_cell), str(subcol)]
        row += [str(x) for x in e]
        if variant == "cols34":
            row = row[:34]
        lines.append(",".join(row))
if variant == "rows383":
    lines = lines[:-1]
with open(path, "w", newline="") as f:
    f.write(eol.join(lines) + eol)
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

# 2. good CSVs parse (encode --emit hex: 96 TC(237,7) + 1 TC(237,8))
for v in lf crlf; do
    synth "$v"
    n="$(python3 "$TOOL" encode --csv "$T/$v.csv" --emit hex 2> "$T/$v.err" |
        wc -l)"
    if [ "$n" -eq 97 ] && grep -q 'B0=0 B32=0' "$T/$v.err"; then
        report "parse $v" ok
    else
        report "parse $v" "units=$n; $(cat "$T/$v.err")"
    fi
done
# the crlf variant has the leading-space preamble line, like the flight file
if grep -q '^ Channel' "$T/crlf.csv"; then
    report "leading-space preamble present" ok
else
    report "leading-space preamble present" "synth lost it"
fi

# 3. bad CSVs: rc 2 and the error names the line and index space
bad() { # variant expected-text
    synth "$1"
    local err rc
    err="$(python3 "$TOOL" encode --csv "$T/$1.csv" --emit hex \
        2>&1 > /dev/null)"
    rc=$?
    if [ "$rc" -eq 2 ] && grep -qF -- "$2" <<< "$err"; then
        report "reject $1" ok
    else
        report "reject $1" "rc=$rc, want 2 and '$2'; got: $err"
    fi
}
bad badheader "col 3 must contain 'detector'"
bad cols34 "header has 34 cells, want 35"
bad rows383 "383 data rows, want 384"
bad dup "duplicate (subcol 3, pix 5)"
bad nonmono "CSV line 21: subcol 1 pix 5: edge 12 (777) not > edge 11 (777)"
bad edge0 "CSV line 4: subcol 0 pix 0: edge 0 (0) outside 1..4095"
bad edge4096 "subcol 31 pix 11: edge 30 (4096) outside 1..4095"
err="$(python3 "$TOOL" encode --csv "$T/nonexistent.csv" --emit hex 2>&1)"
[ $? -eq 2 ] && report "reject missing file" ok ||
    report "reject missing file" "$err"
err="$(env -u STIX_ELUT_CSV python3 "$TOOL" encode --emit hex 2>&1)"
[ $? -eq 2 ] && grep -q 'STIX_ELUT_CSV' <<< "$err" &&
    report "no --csv and no STIX_ELUT_CSV" ok ||
    report "no --csv and no STIX_ELUT_CSV" "$err"
err="$(python3 "$TOOL" encode --csv "$T/lf.csv" --lut-id 65536 --emit hex 2>&1)"
[ $? -eq 2 ] && report "reject --lut-id 65536" ok ||
    report "reject --lut-id 65536" "$err"

# 4. golden first unit of the synthetic CSV: subcol 0 pix 0..3, seq 0x010.
# det_id_1based 01, pix 00, B = 0, 100, 160, ..., 1900, 0 packed 12-bit BE.
GOLD="$(python3 - <<'EOF'
b = [0] + [100 + 60 * k for k in range(31)] + [0]
def st(det_id_1based, pix, b):
    out = bytes([det_id_1based, pix])
    for k in range(16):
        v = (b[2 * k] << 12) | b[2 * k + 1]
        out += bytes([v >> 16, (v >> 8) & 0xFF, v & 0xFF])
    return out + bytes([b[32] >> 4, (b[32] & 0xF) << 4])
app = bytes([3, 4])
for pix in range(4):
    app += st(1, pix, [x + pix if 0 < i < 32 else x for i, x in enumerate(b)])
print(app.hex())
EOF
)"
first="$(python3 "$TOOL" encode --csv "$T/lf.csv" --emit hex 2> /dev/null |
    head -1)"
first="${first// /}"
first="${first,,}"
# 40020000 1DAC C010 00D7 19 ED 07 00 | app | CRC
if [ "${first:0:28}" = "400200001dacc01000d719ed0700" ] &&
        [ "${first:28:$((${#first} - 32))}" = "$GOLD" ] &&
        [ "${#first}" -eq 452 ]; then
    report "golden first unit" ok
else
    report "golden first unit" "got ${first:0:60}... len ${#first}"
fi

APPLY_HEX="$(python3 "$TOOL" encode --csv "$T/lf.csv" --lut-id 1 \
    --first-seq 0x010 --emit hex 2> /dev/null | tail -1)"
# 5. probe args: 96 x (send-at, send-hex, 2 expects) + TC(237,8) group
mapfile -t A < <(python3 "$TOOL" encode --csv "$T/lf.csv" --lut-id 1 \
    --first-seq 0x010 --t0 50 --pace 0.75 --emit probe-args 2> /dev/null)
if [ "${#A[@]}" -eq $((97 * 8)) ] &&
        [ "${A[0]}" = "--send-at" ] && [ "${A[1]}" = "50" ] &&
        [ "${A[4]}" = "--expect" ] && [ "${A[5]}" = "0x5A1:1:1:1DACC010@5" ] &&
        [ "${A[7]}" = "0x5A1:1:7:1DACC010@5" ] &&
        [ "${A[$((95 * 8 + 1))]}" = "121.25" ] &&
        [ "${A[$((96 * 8 + 1))]}" = "127" ] &&
        [ "${A[$((96 * 8 + 3))]}" = "$APPLY_HEX" ] &&
        [ "${APPLY_HEX:0:50}" = \
            "40 02 00 00 1D AC C0 70 00 08 19 ED 08 00 03 00 01" ] &&
        [ "${A[$((96 * 8 + 7))]}" = "0x5A1:1:7:1DACC070@38" ]; then
    report "probe-args layout" ok
else
    report "probe-args layout" "n=${#A[@]} [0..7]=${A[*]:0:8} last=${A[*]: -8}"
fi

# --no-apply (elut-smoke --negative): the 96 TC(237,7) groups only
mapfile -t A < <(python3 "$TOOL" encode --csv "$T/lf.csv" --no-apply \
    --emit probe-args 2> /dev/null)
nh="$(python3 "$TOOL" encode --csv "$T/lf.csv" --no-apply --emit hex \
    2> /dev/null | wc -l)"
if [ "${#A[@]}" -eq $((96 * 8)) ] && [ "$nh" -eq 96 ] &&
        ! printf '%s\n' "${A[@]}" | grep -q 'ED 08'; then
    report "--no-apply leaves out TC(237,8)" ok
else
    report "--no-apply leaves out TC(237,8)" "args=${#A[@]} hex=$nh"
fi

# 6. check-dump exit codes
python3 - "$TOOL" "$T/lf.csv" "$T/good.bin" <<'EOF'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("elut_upload", sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
rows = m.parse_csv_file(sys.argv[2])
with open(sys.argv[3], "wb") as f:
    f.write(m.fpga_elut(rows, m.SUBCOL_FROM_FPGA_DET))
EOF
dump_rc() { # expected-rc name args...
    local want="$1" name="$2" out rc
    shift 2
    out="$(python3 "$TOOL" check-dump --csv "$T/lf.csv" "$@" 2>&1)"
    rc=$?
    # a crash (rc 1 from Python) must not count as a mismatch
    if [ "$rc" -eq "$want" ] && ! grep -q Traceback <<< "$out"; then
        report "check-dump $name" ok
    else
        report "check-dump $name" "rc=$rc want $want: $(tail -2 <<< "$out")"
    fi
}
head -c 1572864 /dev/zero > "$T/zero.bin"
head -c 1572864 /dev/zero | tr '\0' '\037' > "$T/c31.bin"
head -c 1000 /dev/zero > "$T/short.bin"
dump_rc 0 "correct dump" --dump "$T/good.bin"
dump_rc 1 "all-0 dump" --dump "$T/zero.bin"
dump_rc 0 "all-0 --expect-constant 0" --dump "$T/zero.bin" --expect-constant 0
dump_rc 1 "all-31 --expect-constant 0" --dump "$T/c31.bin" --expect-constant 0
dump_rc 0 "all-31 --expect-constant 31" --dump "$T/c31.bin" --expect-constant 31
dump_rc 1 "correct --expect-constant 0" --dump "$T/good.bin" --expect-constant 0
dump_rc 2 "short dump" --dump "$T/short.bin"
dump_rc 2 "missing dump" --dump "$T/none.bin"
out="$(python3 "$TOOL" check-dump --csv "$T/lf.csv" --dump "$T/good.bin" 2>&1)"
grep -q 'sensitivity: identity 348 rows, wrong-direction 276 rows' <<< "$out" &&
    report "check-dump prints the sensitivity line" ok ||
    report "check-dump prints the sensitivity line" "$out"

# 7. the FSW-source cross-check (read-only, optional)
if [ -z "${STIX_FSW_SRC:-}" ]; then
    echo "SKIP (fsw-src): STIX_FSW_SRC unset (point it at STIX-FSW/ASW/ASW/src)"
elif [ ! -f "$STIX_FSW_SRC/utils.c" ]; then
    echo "SKIP (fsw-src): no utils.c under $STIX_FSW_SRC"
else
    out="$(python3 - "$TOOL" "$STIX_FSW_SRC/utils.c" <<'EOF' 2>&1
import importlib.util, re, sys
spec = importlib.util.spec_from_file_location("elut_upload", sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
src = open(sys.argv[2]).read()
def table(name):
    g = re.search(name + r"\s*\[[^]]*\]\s*=\s*\{([^}]*)\}", src)
    return [int(x) for x in re.findall(r"\d+", g.group(1))]
sub_from_fpga = table("utils_subcolIdFromFpgaId")
fpga_from_sub = table("utils_fpgaIdFromSubcolId")
body = re.search(r"dete_data\s*\[\s*\]\s*=\s*\{(.*?)\n\};", src, re.S).group(1)
dete = [re.findall(r"0x[0-9A-Fa-f]+|\d+", r) for r in
        re.findall(r"\{([^}]*)\}", body)]
# utils_dete_data_s: indexSC, numberSC, maskSC, indexFPGA, ... (utils.h)
index_fpga_from_subcol = [0] * 32
for r in dete:
    index_fpga_from_subcol[int(r[0])] = int(r[3])
bad = []
if sub_from_fpga != m.SUBCOL_FROM_FPGA_DET:
    bad.append("utils_subcolIdFromFpgaId")
if fpga_from_sub != m.FPGA_DET_FROM_SUBCOL:
    bad.append("utils_fpgaIdFromSubcolId")
if len(dete) != 32 or index_fpga_from_subcol != m.FPGA_DET_FROM_SUBCOL:
    bad.append("dete_data[].indexFPGA")
print("MISMATCH " + " ".join(bad) if bad else "MATCH")
EOF
)"
    [ "$out" = MATCH ] && report "fsw-src tables match utils.c" ok ||
        report "fsw-src tables match utils.c" "$out"
fi

# 8. the model table pin (E-4): the Python copies match stix_det.c's
# transcriptions of the FSW tables (hw/sparc/stix_det.c, from DP D-2 on)
out="$(python3 - "$TOOL" "$REPO_ROOT/hw/sparc/stix_det.c" <<'EOF' 2>&1
import importlib.util, re, sys
spec = importlib.util.spec_from_file_location("elut_upload", sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
src = open(sys.argv[2]).read()
def table(name):
    g = re.search(name + r"\s*\[[^]]*\]\s*=\s*\{([^}]*)\}", src)
    return [int(x) for x in re.findall(r"\d+", g.group(1))]
bad = []
if table("stix_det_subcol_from_fpga_det") != m.SUBCOL_FROM_FPGA_DET:
    bad.append("stix_det_subcol_from_fpga_det")
if table("stix_det_fpga_det_from_subcol") != m.FPGA_DET_FROM_SUBCOL:
    bad.append("stix_det_fpga_det_from_subcol")
print("MISMATCH " + " ".join(bad) if bad else "MATCH")
EOF
)"
[ "$out" = MATCH ] && report "model tables match stix_det.c" ok ||
    report "model tables match stix_det.c" "$out"

echo "test-elut-upload: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
