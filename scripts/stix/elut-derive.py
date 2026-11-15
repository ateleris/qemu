#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Derive a flight-shaped ELUT CSV from the detector simulation's
per-detector baseline law (stimulus plan A2,
docs/stix-port/2026-11-15-stimulus-plan.md), for the existing
elut-upload.py chain. Standard library only.

derive      writes the CSV: one row per (subcol, pix), all 12 pix of a
            subcol carrying the same 31 ADC edges, the sim calibration
            of that dss_det. The edges are the linear fallback's
            transition points, so a derived ELUT, once uploaded, bins
            exactly like the model's fallback (the round-trip theorem
            below) and det-align-status reads mismatch_cells = 0.
check-dump  see elut-upload.py; --skip-sensitivity skips the mapping
            sensitivity self-check for uniform-baseline CSVs (D7).
self-test   runs the built-in tests (no file needed).

Sources of the per-dss_det baselines (the 0xE setup words' adc value):
  --dssevs PATH  a DSS .dssevs file; every dss_det 0..31 must have
                 exactly one 0xE word (a missing one is an error naming
                 the dss_det - it would otherwise silently derive
                 base 0). The file is read by path only and stays where
                 it is; nothing derived from a user file is copied into
                 the repo.
  --base JSON    the 32 baselines keyed by dss_det, e.g.
                 '{"0": 1800, "1": 1810, ...}' (all 32 keys required).

The sim calibration is the D-7 linear law per dss_det: bin width
W = 4096 - base, edge e[k] = base + ceil((k+1) * W / 32), k = 0..30.
Validation: 0 <= base <= 4064 per detector - the real bound is W >= 32:
below it the ceil edges are not distinct, above it e[30] passes 4095;
the error names the dss_det.

--b0[=B] (decision D6, additive): writes B into the CSV's offset
column; with no value, each row's own DSS baseline. Default: the flag
absent, offset column 0, output byte-identical to the D-Eb encoding
(B0 = 0). elut-upload.py honours a non-zero *integral* offset column as
B[0], so sub-baseline adc becomes unassigned - dropped, like the DSS -
instead of ebin 0. The flight CSVs carry a fractional scale offset
there (e.g. 900.5), which stays ignored: D-Eb stands for them.

**The round-trip theorem the tool self-tests:** with B0 = B32 = 0 (D-Eb
unchanged), edges on the fallback's transition points
e[k] = base + ceil((k+1) * W / 32) and W >= 32, the honoured binning of
the derived ELUT equals the D-7 fallback binning for every adc > base
(at each edge floor(ceil((k+1)W/32) * 32/W) = k+1 since 32/W <= 1, and
between two edges both sides stay in bin k+1; the honoured oracle puts
adc = e[k] in bin k+1, elut-upload.py apply_row). Round edges would
break it: e[k] = base + round((k+1) * W / 32) puts adc = e[k] in bin
k+1 honoured but in floor k whenever the rounding went down (base 1800:
e[2] = 2015 -> honoured 3, fallback floor(215 * 32 / 2296) = 2).
Known, documented nuance: adc <= base bins to ebin 0 under B0 = 0 while
the fallback drops it - ebin 0 is outside the LC bands (1-27) and the
QLDetPerf mask (1-30); with --b0 the FSW loop marks adc < B0 unassigned
(the single adc == B0 lands in ebin 0, the half-open boundary rule).

Chain: elut-derive.py derive -> elut-upload.py encode --csv -> probe ->
TC(237,8) -> elut-upload.py check-dump --csv PASS.

Usage:
  elut-derive.py derive (--dssevs PATH | --base JSON) --out CSV
              [--b0[=B]]
  elut-derive.py --self-test
Exit codes: 0 ok, 1 a failed self-test, 2 bad usage or a refused
input. stdlib only.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
N_DET = 32
N_PIX = 12
N_EDGES = 31
N_EBIN = 32
N_ADC = 4096
# the real baseline bound: W = 4096 - base >= 32 distinct edges
BASE_MAX = N_ADC - 1 - (N_EBIN - 1)      # 4064
DSSEVS_TYPE_SETUP = 0xE


def load_elut_upload():
    spec = importlib.util.spec_from_file_location(
        "elut_upload", os.path.join(HERE, "elut-upload.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class DeriveError(ValueError):
    """A refused input; the text names the dss_det or the file."""


# ------------------------------------------------------------- the law

def derive_edges(base, dss_det):
    """The 31 ADC edges of dss_det's sim calibration (see the module
    docstring). Raises DeriveError naming the dss_det."""
    if isinstance(base, bool) or not isinstance(base, int):
        raise DeriveError("dss_det %d: base %r is not an integer"
                          % (dss_det, base))
    if base < 0:
        raise DeriveError("dss_det %d: base %d is negative" % (dss_det, base))
    if base > BASE_MAX:
        raise DeriveError("dss_det %d: base %d gives a bin width W = %d "
                          "below 32: the ceil edges would not be distinct"
                          % (dss_det, base, N_ADC - base))
    w = N_ADC - base
    edges = [base + -(-(k + 1) * w // N_EBIN) for k in range(N_EDGES)]
    for k in range(N_EDGES):
        if not 1 <= edges[k] <= N_ADC - 1:
            raise DeriveError("dss_det %d: edge %d (%d) outside 1..%d"
                              % (dss_det, k, edges[k], N_ADC - 1))
        if k and edges[k] <= edges[k - 1]:
            raise DeriveError("dss_det %d: edge %d (%d) not > edge %d (%d)"
                              % (dss_det, k, edges[k], k - 1, edges[k - 1]))
    return edges


def fallback_ebin(adc, base):
    """The D-7 linear fallback binning of one adc (stix_det.c
    stix_det_replay_fallback_ebin); None when the fallback drops the adc
    (at or below the baseline)."""
    if adc <= base:
        return None
    return min((adc - base) * N_EBIN // (N_ADC - base), N_EBIN - 1)


# -------------------------------------------------------------- inputs

def read_bases_dssevs(path):
    """The per-dss_det baselines of a .dssevs file: its 0xE setup words.
    Every dss_det 0..31 needs exactly one 0xE word."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        raise DeriveError("cannot read %s: %s" % (path, e))
    if len(data) % 4:
        raise DeriveError("%s: %d bytes, not a multiple of 4"
                          % (path, len(data)))
    bases = {}
    for off in range(0, len(data), 4):
        word = struct.unpack(">I", data[off:off + 4])[0]
        if (word >> 16) & 0xF != DSSEVS_TYPE_SETUP:
            continue                   # events and padding: not a baseline
        dss_det = (word >> 11) & 0x1F
        base = word >> 20
        if dss_det in bases:
            raise DeriveError("%s: dss_det %d has a second 0xE setup word "
                              "(base %d, first %d)"
                              % (path, dss_det, base, bases[dss_det]))
        bases[dss_det] = base
    missing = [d for d in range(N_DET) if d not in bases]
    if missing:
        raise DeriveError("%s: no 0xE setup word for dss_det %d (it would "
                          "silently derive base 0)" % (path, missing[0]))
    return bases


def bases_from_json(text):
    """--base JSON: the 32 baselines keyed by dss_det ("0".."31")."""
    try:
        obj = json.loads(text)
    except ValueError as e:
        raise DeriveError("--base is not JSON: %s" % e)
    if not isinstance(obj, dict):
        raise DeriveError("--base is not a JSON object keyed by dss_det")
    bases = {}
    for d in range(N_DET):
        key = str(d)
        if key not in obj:
            raise DeriveError("--base: no entry for dss_det %d" % d)
        bases[d] = obj[key]
    extra = sorted(set(obj) - {str(d) for d in range(N_DET)},
                   key=lambda s: (len(s), s))
    if extra:
        raise DeriveError("--base: unknown dss_det key(s) %s" % extra[:4])
    for d, base in bases.items():
        if isinstance(base, bool) or not isinstance(base, int):
            raise DeriveError("--base: dss_det %d: %r is not an integer"
                              % (d, base))
        derive_edges(base, d)          # the range check, naming the dss_det
    return bases


# --------------------------------------------------------------- output

def derive_rows(bases, b0_mode=None, b0_value=None):
    """The CSV rows and the offset column: rows[(subcol, pix)] = edges
    for every (subcol = SUBCOL_FROM_FPGA_DET[fpga_det], pix); the offset
    column is 0, or B, or each row's own DSS baseline (--b0 with no
    value). gains[(subcol, pix)] is the calibration's keV/adc
    (K / W, K = 160 keV full scale), for the CSV's gain column."""
    elut = load_elut_upload()
    rows = {}
    offsets = {}
    gains = {}
    for fpga_det in range(N_DET):
        subcol = elut.SUBCOL_FROM_FPGA_DET[fpga_det]
        edges = derive_edges(bases[fpga_det], fpga_det)
        gain = 160.0 / (N_ADC - bases[fpga_det])
        for pix in range(N_PIX):
            rows[(subcol, pix)] = edges
            gains[(subcol, pix)] = gain
            if b0_mode == "value":
                offsets[(subcol, pix)] = b0_value
            elif b0_mode == "per-row":
                offsets[(subcol, pix)] = bases[fpga_det]
            else:
                offsets[(subcol, pix)] = 0
    return rows, offsets, gains


def csv_text(rows, offsets, gains, source):
    """The flight CSV shape elut-upload.py's parser guards: a preamble
    line, the 35-cell header, 384 data rows (offset, gain, Pixel,
    Detector, 31 edges)."""
    hdr = ["Offset (ADC)", "Gain (keV/ADC)", "Pixel", "Detector"] + [
        "ADC Edge #%d - %d keV" % (k, k + 5) for k in range(N_EDGES)]
    lines = ["Derived by elut-derive.py from %s" % source,
             " Sim calibration: the D-7 linear law per dss_det "
             "(e[k] = base + ceil((k+1)*W/32))", ",".join(hdr)]
    for (subcol, pix) in sorted(rows):
        lines.append(",".join([str(offsets[(subcol, pix)]),
                               "%.6f" % gains[(subcol, pix)],
                               str(pix), str(subcol)]
                              + [str(x) for x in rows[(subcol, pix)]]))
    return "\n".join(lines) + "\n"


def write_csv(path, text):
    try:
        with open(path, "w", newline="") as f:
            f.write(text)
    except OSError as e:
        raise DeriveError("cannot write %s: %s" % (path, e))


# ------------------------------------------------------------ self-test

def self_test():
    failures = []
    elut = load_elut_upload()

    def check(name, cond, detail=""):
        print("%s - %s" % ("ok  " if cond else "FAIL", name))
        if not cond:
            print("      %s" % detail)
            failures.append(name)

    # 1. the round-trip theorem, exhaustively for five bases x all adc:
    #    the honoured binning (the E-1 apply_row replay of the derived
    #    boundaries) equals the D-7 fallback for every adc > base, and
    #    bins to ebin 0 at or below it (B0 = 0, the documented nuance)
    for base in (0, 1800, 2000, 2020, 3800):
        edges = derive_edges(base, 0)
        val = elut.apply_row(elut.boundaries(edges))
        bad = [adc for adc in range(N_ADC)
               if val[adc] != (fallback_ebin(adc, base)
                               if adc > base else 0)]
        check("1 round trip base %d: fallback == honoured (adc > base)"
              % base, not bad,
              "%d adc differ, first %r" % (len(bad), bad[:3]))

    # 2. the baseline bound: W < 32 (base 4065) and a negative base are
    #    refused, naming the dss_det
    for base in (4065, -1):
        try:
            derive_edges(base, 7)
            check("2 base %d refused" % base, False, "accepted")
        except DeriveError as e:
            check("2 base %d refused, naming the dss_det" % base,
                  "dss_det 7" in str(e), str(e))

    # 3. the CSV re-parses with elut-upload.parse_csv_text: 384 rows,
    #    each (subcol, pix) once
    bases = {d: 1800 + 10 * d for d in range(N_DET)}
    rows, offsets, gains = derive_rows(bases)
    text = csv_text(rows, offsets, gains, "self-test")
    try:
        got = elut.parse_csv_text(text)
        check("3 the CSV re-parses: 384 rows, each (subcol, pix) once",
              len(got) == N_DET * N_PIX and got == rows,
              "%d rows" % len(got))
    except elut.CsvError as e:
        check("3 the CSV re-parses", False, str(e))

    # 4. sensitivity: with baselines 1800 + 10*dss_det the D-Eg check
    #    distinguishes identity vs wrong-direction (exact differing row
    #    counts), so check-dump exits 0/1, never 2
    good = elut.fpga_elut(rows, elut.SUBCOL_FROM_FPGA_DET)
    for name, table, n_det in (("identity", elut.IDENTITY, 29),
                               ("wrong-direction",
                                elut.FPGA_DET_FROM_SUBCOL, 23)):
        dump = elut.fpga_elut(rows, table)
        n_rows = len(elut.rows_differing(dump, good))
        check("4 %s mapping: %d x 12 differing rows (as predicted)"
              % (name, n_det), n_rows == n_det * N_PIX,
              "%d rows" % n_rows)
    quiet = []

    def capture(*a):
        quiet.append(" ".join(str(x) for x in a))

    check("4 the correct dump passes (exit 0, not 2)",
          elut.check_dump_bytes(rows, good, out=capture) == 0,
          " | ".join(quiet[-2:]))
    check("4 the sensitivity line ran (not skipped)",
          any(s.startswith("sensitivity: identity 348") for s in quiet))

    # 5. an encode --emit hex unit round-trips through the processELUT
    #    copy (the E-1 self-test 2 pattern)
    units = elut.elut_units(rows, 0x010)
    check("5 96 TC(237,7) units", len(units) == elut.N_ELUT_TCS,
          str(len(units)))
    ram = {}
    ok = True
    for u in units:
        sp = u[4:]
        app = sp[10:-2]
        ok &= elut.fsw_process_elut(app, ram)
    check("5 the units round-trip through processELUT",
          ok and ram == {key: elut.boundaries(e)
                         for key, e in rows.items()})

    # 6. --b0: with base b the FSW loop marks adc < b unassigned (32)
    #    and the encoded B[0] byte carries b; without the flag the
    #    output is byte-identical to the D-Eb encoding (the single
    #    adc == b lands in ebin 0: the half-open boundary rule)
    b = 1800
    edges = derive_edges(b, 0)
    val = elut.apply_row(elut.boundaries(edges, b0=b))
    check("6 --b0: adc < b unassigned",
          all(val[adc] == elut.N_EBIN for adc in range(b)))
    check("6 --b0: adc == b in ebin 0 (the boundary point)", val[b] == 0)
    check("6 --b0: adc > b falls back",
          all(val[adc] == fallback_ebin(adc, b)
              for adc in range(b + 1, N_ADC)))
    app = bytes([elut.LUT_TYPE_ELUT, 1]) + elut.elut_struct(
        1, 0, elut.boundaries(edges, b0=b))
    ram = {}
    elut.fsw_process_elut(app, ram)
    check("6 the encoded B[0] byte carries b", ram[(0, 0)][0] == b)
    r0, o0, _ = derive_rows(bases)
    rb, ob, _ = derive_rows(bases, b0_mode="per-row")
    plain = elut.elut_units(r0, 0x010)
    shifted = elut.elut_units(rb, 0x010, b0_of=lambda k: ob[k])
    with_off = elut.elut_units(r0, 0x010, b0_of=lambda k: 0)
    check("6 without the flag the encoding is byte-identical to D-Eb",
          plain == with_off)
    check("6 with --b0 the encoding differs (B[0] carried)",
          plain != shifted)

    # 7. --skip-sensitivity (D7): on a uniform-baseline CSV (all rows
    #    identical) the D-Eg check would exit 2; with the flag it exits
    #    0/1 and says so
    ub = {d: 1800 for d in range(N_DET)}
    urows, _, _ = derive_rows(ub)
    good_u = elut.fpga_elut(urows, elut.SUBCOL_FROM_FPGA_DET)
    # a wrong dump: one byte of (pix 0, fpga_det 0) flipped (with
    # uniform rows an identity-mapping dump is byte-identical to the
    # correct one, so a flipped byte stands in for the exit-1 case)
    wrong_u = bytearray(good_u)
    wrong_u[(0 * N_DET + 0) * N_ADC + 5] = 17
    del quiet[:]
    rc = elut.check_dump_bytes(urows, good_u, out=capture)
    check("7 uniform baselines: the D-Eg trap exits 2",
          rc == 2 and any("cannot detect a mapping bug" in s
                          for s in quiet), " | ".join(quiet[-2:]))
    del quiet[:]
    rc = elut.check_dump_bytes(urows, good_u, out=capture,
                               skip_sensitivity=True)
    check("7 --skip-sensitivity: the correct dump exits 0 and says so",
          rc == 0 and any("skipped" in s for s in quiet),
          " | ".join(quiet[-3:]))
    del quiet[:]
    rc = elut.check_dump_bytes(urows, bytes(wrong_u), out=capture,
                               skip_sensitivity=True)
    check("7 --skip-sensitivity: a wrong dump exits 1 (not 2)",
          rc == 1, " | ".join(quiet[-3:]))

    # the baselines of a synthetic .dssevs: 32 setup words, one per
    # dss_det; a missing word is an error naming the dss_det
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "s.dssevs")
        with open(p, "wb") as f:
            for det in range(N_DET):
                f.write(struct.pack(">I", ((1800 + 10 * det) << 20)
                                    | (DSSEVS_TYPE_SETUP << 16)
                                    | (det << 11)))
            f.write(struct.pack(">I", 2050 << 20))     # one event word
        got = read_bases_dssevs(p)
        check("dssevs bases read, one per dss_det",
              got == {det: 1800 + 10 * det for det in range(N_DET)})
        with open(p, "wb") as f:
            for det in range(N_DET - 1):
                f.write(struct.pack(">I", ((1800 + 10 * det) << 20)
                                    | (DSSEVS_TYPE_SETUP << 16)
                                    | (det << 11)))
        try:
            read_bases_dssevs(p)
            check("a missing 0xE word is refused", False, "accepted")
        except DeriveError as e:
            check("a missing 0xE word is refused, naming the dss_det",
                  "dss_det 31" in str(e), str(e))

    if failures:
        print("DERIVE SELF-TEST: FAIL (%d: %s)"
              % (len(failures), ", ".join(failures)))
        return 1
    print("DERIVE SELF-TEST: PASS")
    return 0


# ----------------------------------------------------------------- main

def parse_b0(spec):
    """--b0[=B]: None (absent), "per-row" (no value) or an int 0..4095."""
    if spec is None:
        return (None, None)
    if spec == "":
        return ("per-row", None)
    try:
        b = int(spec, 0)
    except ValueError:
        raise DeriveError("--b0: %r is not an integer" % spec)
    if not 0 <= b <= N_ADC - 1:
        raise DeriveError("--b0: %d outside 0..%d" % (b, N_ADC - 1))
    return ("value", b)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--self-test", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    der = sub.add_parser("derive")
    src = der.add_mutually_exclusive_group(required=True)
    src.add_argument("--dssevs", metavar="PATH",
                     help="read the baselines from the file's 0xE words")
    src.add_argument("--base", metavar="JSON",
                     help="the 32 baselines keyed by dss_det")
    der.add_argument("--out", required=True, metavar="CSV")
    der.add_argument("--b0", nargs="?", const="", default=None,
                     metavar="B",
                     help="write B into the offset column (no value: "
                          "each row's own DSS baseline); default 0 "
                          "(D-Eb)")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if args.cmd != "derive":
        ap.error("a command is required (derive, or --self-test)")
    try:
        if args.dssevs:
            bases = read_bases_dssevs(args.dssevs)
            source = args.dssevs
        else:
            bases = bases_from_json(args.base)
            source = "--base"
        b0_mode, b0_value = parse_b0(args.b0)
        rows, offsets, gains = derive_rows(bases, b0_mode, b0_value)
        text = csv_text(rows, offsets, gains, source)
        write_csv(args.out, text)
    except DeriveError as e:
        print("elut-derive: %s" % e, file=sys.stderr)
        return 2
    lo, hi = min(bases.values()), max(bases.values())
    b0s = sorted(set(offsets.values()))
    print("derived CSV %s sha256 %s rows %d base %d..%d offset %s"
          % (args.out, hashlib.sha256(text.encode()).hexdigest(),
             len(rows), lo, hi,
             "0 (D-Eb)" if b0s == [0] else "per-row baseline"
             if b0_mode == "per-row" else str(b0s[0])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
