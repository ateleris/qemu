#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Flight energy LUT (ELUT) upload tool for the STIX model (ELUT plan E-1,
docs/stix-port/2026-11-11-elut-plan.md). Standard library only.

encode      turns a flight ELUT CSV into the TC(237,7) x 96 + TC(237,8)
            sequence, as probe arguments (one per line, for mapfile -t)
            or as hex units (one per line).
check-dump  checks a QMP pmemsave dump of the FPGA ELUT (12 x 32 x 4096
            bytes at 0xE0000000) against the CSV, through the FSW's
            fpga_det -> subcol mapping; exit 0 pass, 1 mismatch, 2
            environment or sensitivity failure.
self-test   runs the built-in tests (no file needed).

Index spaces (plan "Index spaces"); every name says which one it is in:
  subcol          ASW sub-collimator = ICD detector index, 0-31; the CSV
                  column "Detector" (0-based; GSW stx_read_elut.pro) and
                  the first index of the FSW RAM table ELUT[subcol][pix][33]
  det_id_1based   the TC(237,7) "Detector ID" byte = subcol + 1, 1-32
  fpga_det        FPGA detector, 0-31; dimension 1 of the FPGA ELUT
  pix             Caliste pixel, 0-11
  ebin            science energy bin, 0-31 (32 = unassigned)
  adc             raw 12-bit ADC value, 0-4095

The FSW apply loop (LUTMgmt.c applyELUT) writes the FPGA ELUT
val[pix][fpga_det][adc] from RAM row ELUT[subcol][pix] with
subcol = utils_subcolIdFromFpgaId[fpga_det]. That mapping is NOT the
identity: only fpga_det 0, 1 and 4 map to themselves.

Boundaries (D-Eb, user decision 2026-11-11): B[0] = 0, B[1..31] = the
CSV's 31 ADC edges, B[32] = 0. So ebin 0 covers adc 0..e[0]-1, ebin 31
covers e[30]..4095, and no adc is left unassigned.

The CSV is referenced by path only (--csv or STIX_ELUT_CSV); its rows are
never copied anywhere. The record line on stderr gives its path, sha256,
row count and value ranges.
"""
import argparse
import csv
import hashlib
import io
import os
import sys

N_SUBCOL = 32
N_PIX = 12
N_EDGES = 31
N_BOUNDARIES = 33           # B[0..32] per (subcol, pix), commonFSW.h
N_EBIN = 32                 # ebin 0..31; 32 = unassigned
N_ADC = 4096
DUMP_SIZE = N_PIX * N_SUBCOL * N_ADC    # 1 572 864 bytes
ROWS_PER_TC = 4             # PIX00479; N = 5 fails the FSW's static check
N_ELUT_TCS = N_SUBCOL * N_PIX // ROWS_PER_TC    # 96
LUT_TYPE_ELUT = 3           # E_LUTMGMT_ELUT, LUTMgmt.h
SPW_HDR = bytes([0x40, 0x02, 0x00, 0x00])
APID_TC_WORD = 0x1DAC       # TC, secondary header, APID 0x5AC
DFH_FLAGS = 0x19            # PUS data field header byte: both acks
ELUT_STRUCT_LEN = 52

# utils.c:113-119 (utils_subcolIdFromFpgaId): index fpga_det -> subcol.
# applyELUT (LUTMgmt.c:870, UTILS_FPGAID2SUBCOLID) reads the RAM row of
# this subcol for FPGA detector fpga_det.
SUBCOL_FROM_FPGA_DET = [
    0, 1, 5, 6, 4, 10, 11, 12,
    13, 14, 9, 15, 7, 8, 2, 3,
    30, 31, 25, 26, 21, 27, 19, 20,
    17, 18, 16, 22, 23, 24, 28, 29,
]
# utils.c:121-127 (utils_fpgaIdFromSubcolId): index subcol -> fpga_det,
# the inverse; equal to dete_data[].indexFPGA (utils.c:74-107), which is
# what UTILS_SUBCOLID2FPGAID reads (utils.h:116).
FPGA_DET_FROM_SUBCOL = [
    0, 1, 14, 15, 4, 2, 3, 12,
    13, 10, 5, 6, 7, 8, 9, 11,
    26, 24, 25, 22, 23, 20, 27, 28,
    29, 18, 19, 21, 30, 31, 16, 17,
]
IDENTITY = list(range(N_SUBCOL))


class CsvError(ValueError):
    """A CSV that is not a flight ELUT table; the text names the CSV line
    and the index space."""


# ---------------------------------------------------------------- CSV

def parse_csv_text(text, name="<csv>"):
    """Parse a flight ELUT CSV given as text; returns
    rows[(subcol, pix)] = [e0..e30] (adc edges).

    The header is the first row whose first cell, stripped and
    case-folded, starts with "offset"; the lines before it are free
    text. Columns are read by position: offset, gain, Pixel (pix),
    Detector (subcol, 0-based), 31 ADC edges. Exactly 384 data rows,
    each (subcol, pix) once, integer edges strictly increasing in
    1..4095. Raises CsvError naming the CSV line and the index space."""
    reader = csv.reader(io.StringIO(text, newline=""))
    header_line = None
    rows = {}
    n_data = 0
    for cells in reader:
        line = reader.line_num
        if header_line is None:
            if cells and cells[0].strip().casefold().startswith("offset"):
                header_line = line
                hdr = [c.strip().casefold() for c in cells]
                if len(hdr) != 4 + N_EDGES:
                    raise CsvError("%s: CSV line %d: header has %d cells, "
                                   "want %d" % (name, line, len(hdr),
                                                4 + N_EDGES))
                for col, want in ((2, "pixel"), (3, "detector")):
                    if want not in hdr[col]:
                        raise CsvError("%s: CSV line %d: header col %d must "
                                       "contain '%s' (got %r)"
                                       % (name, line, col, want, cells[col]))
                for col in range(4, 4 + N_EDGES):
                    if "adc edge" not in hdr[col]:
                        raise CsvError("%s: CSV line %d: header col %d must "
                                       "contain 'adc edge' (got %r)"
                                       % (name, line, col, cells[col]))
            continue
        if not cells or all(not c.strip() for c in cells):
            continue
        where = "%s: CSV line %d" % (name, line)
        if len(cells) != 4 + N_EDGES:
            raise CsvError("%s: %d cells, want %d"
                           % (where, len(cells), 4 + N_EDGES))
        try:
            float(cells[0])
            float(cells[1])
            pix = int(cells[2])
            subcol = int(cells[3])
        except ValueError:
            raise CsvError("%s: offset/gain/Pixel/Detector not numeric"
                           % where)
        if not 0 <= pix < N_PIX or not 0 <= subcol < N_SUBCOL:
            raise CsvError("%s: pix %d / subcol %d outside 0..%d / 0..%d"
                           % (where, pix, subcol, N_PIX - 1, N_SUBCOL - 1))
        where = "%s: subcol %d pix %d" % (where, subcol, pix)
        try:
            edges = [int(c) for c in cells[4:]]
        except ValueError:
            raise CsvError("%s: an ADC edge is not an integer" % where)
        for k, e in enumerate(edges):
            if not 1 <= e <= N_ADC - 1:
                raise CsvError("%s: edge %d (%d) outside 1..%d"
                               % (where, k, e, N_ADC - 1))
            if k and e <= edges[k - 1]:
                raise CsvError("%s: edge %d (%d) not > edge %d (%d)"
                               % (where, k, e, k - 1, edges[k - 1]))
        if (subcol, pix) in rows:
            raise CsvError("%s: duplicate (subcol %d, pix %d)"
                           % (where, subcol, pix))
        rows[(subcol, pix)] = edges
        n_data += 1
    if header_line is None:
        raise CsvError("%s: no header row (first cell 'Offset ...')" % name)
    if n_data != N_SUBCOL * N_PIX:
        raise CsvError("%s: %d data rows, want %d"
                       % (name, n_data, N_SUBCOL * N_PIX))
    return rows


def parse_csv_file(path):
    with open(path, newline="") as f:
        return parse_csv_text(f.read(), path)


def boundaries(edges):
    """B[0..32] for one (subcol, pix) row: B0 = 0, B[1..31] = e[0..30],
    B32 = 0 (D-Eb)."""
    if len(edges) != N_EDGES:
        raise ValueError("want %d edges, got %d" % (N_EDGES, len(edges)))
    return [0] + list(edges) + [0]


# ---------------------------------------------------------- TC encoding

def crc16_ccitt_false(data):
    """CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


def tc_unit(seq, service, subtype, app):
    """One TC unit: SpW header + CCSDS primary header + PUS data field
    header + app data + CRC-16/CCITT-FALSE over the CCSDS packet."""
    if not 0 <= seq <= 0x3FFF:
        raise ValueError("seq 0x%X outside 14 bits" % seq)
    dfh = bytes([DFH_FLAGS, service, subtype, 0x00])
    length = len(dfh) + len(app) + 2 - 1   # CCSDS: data field bytes - 1
    sp = bytes([APID_TC_WORD >> 8, APID_TC_WORD & 0xFF,
                0xC0 | (seq >> 8), seq & 0xFF,
                length >> 8, length & 0xFF]) + dfh + bytes(app)
    crc = crc16_ccitt_false(sp)
    return SPW_HDR + sp + bytes([crc >> 8, crc & 0xFF])


def elut_struct(det_id_1based, pix, b):
    """The 52-byte TC(237,7) ELUT structure (TCData.h:190-198): the two
    ID bytes, B[0..31] as 16 packed 12-bit BE pairs, B[32] in the high
    12 bits of a u16 (low nibble 0)."""
    if len(b) != N_BOUNDARIES or any(not 0 <= x < N_ADC for x in b):
        raise ValueError("B[] needs %d 12-bit values" % N_BOUNDARIES)
    out = bytearray([det_id_1based, pix])
    for k in range(16):
        v = (b[2 * k] << 12) | b[2 * k + 1]
        out += bytes([v >> 16, (v >> 8) & 0xFF, v & 0xFF])
    out += bytes([b[32] >> 4, (b[32] & 0x0F) << 4])
    return bytes(out)


def elut_units(rows, first_seq, rows_per_tc=ROWS_PER_TC):
    """The 96 TC(237,7) units, rows in (subcol, pix) order, seq
    first_seq + k for unit k."""
    if not 1 <= rows_per_tc <= ROWS_PER_TC:
        raise ValueError("N = %d rows per TC(237,7); the FSW takes 1..%d"
                         % (rows_per_tc, ROWS_PER_TC))
    keys = sorted(rows)
    units = []
    for k in range(0, len(keys), rows_per_tc):
        chunk = keys[k:k + rows_per_tc]
        app = bytes([LUT_TYPE_ELUT, len(chunk)])
        for (subcol, pix) in chunk:
            app += elut_struct(subcol + 1, pix, boundaries(rows[(subcol, pix)]))
        units.append(tc_unit(first_seq + len(units), 237, 7, app))
    return units


def apply_unit(lut_id, seq):
    """TC(237,8): app data 03 | lut_id u16 (TCData.h:225-226)."""
    if not 0 <= lut_id <= 0xFFFF:
        raise ValueError("lut_id %d outside 0..65535" % lut_id)
    return tc_unit(seq, 237, 8, bytes([LUT_TYPE_ELUT, lut_id >> 8,
                                       lut_id & 0xFF]))


def ack_data(seq):
    """The TM(1,x) data prefix for TC seq: the TC's first header words."""
    return "%04X%04X" % (APID_TC_WORD, 0xC000 | seq)


def unit_hex(unit):
    return " ".join("%02X" % b for b in unit)


def fmt_t(t):
    s = "%.2f" % t
    return s.rstrip("0").rstrip(".")


# ------------------------------------------- the FSW side, modelled

def fsw_process_elut(app, ram):
    """A Python copy of processELUT (LUTMgmt.c:1024-1067) on the app
    data of a TC(237,7): fills ram[(subcol, pix)] = B[0..32]. Returns
    False where the FSW returns FSW_ERROR (TM(1,8) FID 48457), with the
    rows before the bad one already written, like the FSW."""
    n = app[1]
    for i in range(n):
        base = 2 + i * ELUT_STRUCT_LEN
        # uint32_t detectorId = byte - 1: wraps to 0xFFFFFFFF for 0
        subcol = (app[base] - 1) & 0xFFFFFFFF
        pix = app[base + 1]
        if subcol >= N_SUBCOL or pix >= N_PIX:
            return False
        e = base + 2
        b = [0] * N_BOUNDARIES
        for k in range(16):
            b[2 * k] = ((app[e + 3 * k] << 8) | app[e + 3 * k + 1]) >> 4
            b[2 * k + 1] = ((app[e + 3 * k + 1] << 8)
                            | app[e + 3 * k + 2]) & 0xFFF
        b[32] = ((app[e + 48] << 8) | app[e + 49]) >> 4
        ram[(subcol, pix)] = b
    return True


def apply_row(b, prior=0):
    """One (subcol, pix) RAM row through applyELUT's boundary loop
    (LUTMgmt.c:878-930), literally: iBoundary 0..33 in order, a later
    write wins; a boundary value 0 as an upper limit means 4096.
    Returns val[adc] for adc 0..4095 (prior where nothing is written)."""
    val = bytearray([prior]) * N_ADC
    for i_boundary in range(N_BOUNDARIES + 1):     # 0..33
        if i_boundary == 0:
            ebin, start, stop = N_EBIN, 0, b[0]
        elif i_boundary == N_BOUNDARIES:
            ebin, start, stop = N_EBIN, b[32] or N_ADC, N_ADC
        else:
            ebin = i_boundary - 1
            start, stop = b[ebin], b[ebin + 1] or N_ADC
        if stop > start:
            val[start:stop] = bytes([ebin]) * (stop - start)
    return bytes(val)


def fpga_elut(rows, subcol_from_fpga_det, b_of=None, layout="pix_fpga"):
    """The FPGA ELUT bytes val[pix][fpga_det][adc] the FSW writes for
    CSV rows[(subcol, pix)], reading RAM row subcol_from_fpga_det
    [fpga_det] for FPGA detector fpga_det. b_of(edges) builds B[] (the
    shared-builder trap swaps it); layout "fpga_pix" lays the table out
    [fpga_det][pix][adc] instead (the axis trap)."""
    b_of = b_of or boundaries
    cache = {}
    val = bytearray(DUMP_SIZE)
    for fpga_det in range(N_SUBCOL):
        subcol = subcol_from_fpga_det[fpga_det]
        for pix in range(N_PIX):
            key = (subcol, pix)
            if key not in cache:
                cache[key] = apply_row(b_of(rows[key]))
            if layout == "pix_fpga":
                off = (pix * N_SUBCOL + fpga_det) * N_ADC
            else:
                off = (fpga_det * N_PIX + pix) * N_ADC
            val[off:off + N_ADC] = cache[key]
    return bytes(val)


def oracle_failures(rows, dump):
    """D-Ef's independent oracle, from the CSV edges directly (no B[],
    no loop replay): for each (pix, fpga_det), e = the CSV row of
    subcol = SUBCOL_FROM_FPGA_DET[fpga_det]: val[0] == 0,
    val[e[k]-1] == k and val[e[k]] == k+1 for k = 0..30, and
    val[4095] == 31. Returns a list of (pix, fpga_det, subcol, adc,
    want_ebin, got_ebin)."""
    bad = []
    for pix in range(N_PIX):
        for fpga_det in range(N_SUBCOL):
            subcol = SUBCOL_FROM_FPGA_DET[fpga_det]
            e = rows[(subcol, pix)]
            off = (pix * N_SUBCOL + fpga_det) * N_ADC
            probes = [(0, 0), (N_ADC - 1, N_EBIN - 1)]
            for k in range(N_EDGES):
                probes += [(e[k] - 1, k), (e[k], k + 1)]
            for adc, want in probes:
                if dump[off + adc] != want:
                    bad.append((pix, fpga_det, subcol, adc, want,
                                dump[off + adc]))
    return bad


def check_dump_bytes(rows, dump, expect_constant=None, out=print,
                     b_of=None):
    """check-dump on bytes; returns the exit code (0, 1 or 2).

    D-Eg first: the checker must be able to tell the true mapping from
    the identity and the wrong-direction table on THIS CSV, with exactly
    12 x #{fpga_det : table differs} differing rows each; else exit 2.
    Then the dump: a constant dump is named as such (exit 0 only under a
    matching expect_constant); otherwise the loop replay (D-Ef, all
    1 572 864 bytes) and the independent oracle must both pass.
    b_of is a self-test hook: the B[] builder of the replay (never of
    the oracle), to prove that the oracle catches a builder bug the
    replay shares."""
    if len(dump) != DUMP_SIZE:
        out("check-dump: ERROR: dump is %d bytes, want %d (12 pix x 32 "
            "fpga_det x 4096 adc)" % (len(dump), DUMP_SIZE))
        return 2
    want = fpga_elut(rows, SUBCOL_FROM_FPGA_DET, b_of=b_of)
    counts = []
    for name, table in (("identity", IDENTITY),
                        ("wrong-direction", FPGA_DET_FROM_SUBCOL)):
        predicted = {(p, f) for p in range(N_PIX) for f in range(N_SUBCOL)
                     if table[f] != SUBCOL_FROM_FPGA_DET[f]}
        got = rows_differing(fpga_elut(rows, table, b_of=b_of), want)
        if got != predicted:
            out("check-dump: ERROR: sensitivity: the %s mapping differs "
                "in %d rows, predicted %d: checker cannot detect a "
                "mapping bug on this CSV" % (name, len(got), len(predicted)))
            return 2
        counts.append("%s %d rows" % (name, len(got)))
    out("sensitivity: " + ", ".join(counts) + " (as predicted)")

    first = dump[0]
    if dump.count(first) == DUMP_SIZE:
        if expect_constant is not None:
            if first == expect_constant:
                out("check-dump: PASS (constant %d, as expected)" % first)
                return 0
            out("check-dump: FAIL (constant %d, expected constant %d)"
                % (first, expect_constant))
            return 1
        out("check-dump: FAIL (the dump is constant %d: no ELUT applied)"
            % first)
        return 1
    if expect_constant is not None:
        out("check-dump: FAIL (not constant; expected constant %d)"
            % expect_constant)
        return 1

    per_row = {}
    shown = []
    if dump != want:
        for pix in range(N_PIX):
            for fpga_det in range(N_SUBCOL):
                off = (pix * N_SUBCOL + fpga_det) * N_ADC
                a, b = dump[off:off + N_ADC], want[off:off + N_ADC]
                if a == b:
                    continue
                subcol = SUBCOL_FROM_FPGA_DET[fpga_det]
                for adc in range(N_ADC):
                    if a[adc] != b[adc]:
                        per_row[(pix, fpga_det)] = \
                            per_row.get((pix, fpga_det), 0) + 1
                        if len(shown) < 20:
                            shown.append((pix, fpga_det, subcol, adc,
                                          b[adc], a[adc]))
    oracle = oracle_failures(rows, dump)
    if not per_row and not oracle:
        out("replay: PASS (all %d bytes)" % DUMP_SIZE)
        out("oracle: PASS (%d rows x %d probes)"
            % (N_PIX * N_SUBCOL, 2 + 2 * N_EDGES))
        out("check-dump: PASS")
        return 0
    if per_row:
        out("replay: FAIL: bad rows %d, bad bytes %d"
            % (len(per_row), sum(per_row.values())))
        out("  pix fpga_det subcol adc want_ebin got_ebin")
        for m in shown:
            out("  %3d %8d %6d %4d %9d %8d" % m)
        out("  per (pix, fpga_det): " + " ".join(
            "%d,%d:%d" % (p, f, n) for (p, f), n in sorted(per_row.items())))
    else:
        out("replay: PASS (all %d bytes)" % DUMP_SIZE)
    if oracle:
        out("oracle: FAIL: %d probes, first: pix %d fpga_det %d subcol %d "
            "adc %d want_ebin %d got_ebin %d" % ((len(oracle),) + oracle[0]))
    else:
        out("oracle: PASS")
    out("check-dump: FAIL")
    return 1


# ------------------------------------------------------------ self-test

def synth_rows():
    """The self-test's CSV rows: edge k of (subcol, pix) is
    100 + 60k + (12 subcol + pix), so every row is unique and strictly
    increasing (max edge 2283)."""
    return {(subcol, pix): [100 + 60 * k + (12 * subcol + pix)
                            for k in range(N_EDGES)]
            for subcol in range(N_SUBCOL) for pix in range(N_PIX)}


def synth_csv_text(rows, eol="\r\n"):
    hdr = ["Offset (ADC)", "Gain (keV/ADC)", "Pixel", "Detector"] + [
        "ADC Edge #%d - %d keV" % (k, k + 4) for k in range(N_EDGES)]
    lines = ["Created from synthetic", " Channel energy edges, synthetic",
             ",".join(hdr)]
    for (subcol, pix) in sorted(rows):
        lines.append(",".join(["900.5", "0.11", str(pix), str(subcol)]
                              + [str(x) for x in rows[(subcol, pix)]]))
    return eol.join(lines) + eol


def rows_differing(a, b):
    """The set of (pix, fpga_det) rows where FPGA tables a and b differ."""
    return {(p, f) for p in range(N_PIX) for f in range(N_SUBCOL)
            if a[(p * N_SUBCOL + f) * N_ADC:(p * N_SUBCOL + f + 1) * N_ADC]
            != b[(p * N_SUBCOL + f) * N_ADC:(p * N_SUBCOL + f + 1) * N_ADC]}


def self_test():
    failures = []

    def check(name, cond, detail=""):
        print("%s - %s%s" % ("ok  " if cond else "FAIL", name,
                             "" if cond else ": " + detail))
        if not cond:
            failures.append(name)

    def run(name, fn):
        try:
            fn()
        except Exception as e:  # a stub or a crash is a failure, not a stop
            check(name, False, "%s: %s" % (type(e).__name__, e))

    rows = synth_rows()
    quiet = []

    def capture(*a):
        quiet.append(" ".join(str(x) for x in a))

    # 1. CRC: the four nominal-smoke units rebuild byte for byte
    def t1():
        want = {
            (1, 236, 1, "0400"):
                "40 02 00 00 1D AC C0 01 00 07 19 EC 01 00 04 00 4A C1",
            (2, 236, 6, "07040000"):
                "40 02 00 00 1D AC C0 02 00 09 19 EC 06 00 07 04 00 00 AF BA",
            (3, 236, 1, "0500"):
                "40 02 00 00 1D AC C0 03 00 07 19 EC 01 00 05 00 BF 97",
            (4, 17, 1, ""):
                "40 02 00 00 1D AC C0 04 00 05 19 11 01 00 16 B8",
        }
        for (seq, svc, sub, app), hexs in want.items():
            got = unit_hex(tc_unit(seq, svc, sub, bytes.fromhex(app)))
            check("1 crc: TC(%d,%d) seq %d" % (svc, sub, seq), got == hexs,
                  got)
    run("1 crc", t1)

    # 2. round trip through the processELUT copy
    def t2():
        units = elut_units(rows, 0x010)
        check("2 96 units", len(units) == N_ELUT_TCS, str(len(units)))
        ram = {}
        ok = True
        for k, u in enumerate(units):
            sp = u[4:]
            ok &= u[:4] == SPW_HDR and len(u) == 226
            ok &= (sp[4] << 8 | sp[5]) == 0x00D7
            ok &= (sp[0] << 8 | sp[1]) == APID_TC_WORD
            ok &= (sp[2] << 8 | sp[3]) == 0xC000 | (0x010 + k)
            ok &= sp[6:10] == bytes([DFH_FLAGS, 237, 7, 0])
            ok &= crc16_ccitt_false(sp[:-2]) == (sp[-2] << 8 | sp[-1])
            app = sp[10:-2]
            ok &= app[0] == LUT_TYPE_ELUT and app[1] == ROWS_PER_TC
            ok &= len(app) == 2 + app[1] * ELUT_STRUCT_LEN
            for i in range(app[1]):    # spare nibble of the B32 word
                ok &= app[2 + i * ELUT_STRUCT_LEN + 51] & 0x0F == 0
            ok &= fsw_process_elut(app, ram)
        check("2 unit headers, length 0x00D7, CRC, N <= 4, spare 0", ok)
        check("2 round trip: ram[(det_id_1based - 1, pix)] = B[0..32]",
              ram == {key: boundaries(e) for key, e in rows.items()})
        try:
            elut_units(rows, 0x010, rows_per_tc=5)
            check("2 N = 5 refused", False, "accepted")
        except ValueError:
            check("2 N = 5 refused", True)
    run("2 round trip", t2)

    # 3. off-by-one trap: det_id_1based = subcol decodes to the wrong row
    def t3():
        b = boundaries(rows[(5, 3)])
        app = bytes([LUT_TYPE_ELUT, 1]) + elut_struct(5, 3, b)
        ram = {}
        check("3 det_id_1based = subcol 5 lands on subcol 4",
              fsw_process_elut(app, ram) and list(ram) == [(4, 3)])
        app = bytes([LUT_TYPE_ELUT, 1]) + elut_struct(0, 3, b)
        check("3 det_id_1based 0 hits the range check (uint32 wrap)",
              not fsw_process_elut(app, {}))
    run("3 off-by-one", t3)

    good = None

    # 4/5. identity and wrong-direction dumps fail with exact row counts
    def t45():
        nonlocal good
        good = fpga_elut(rows, SUBCOL_FROM_FPGA_DET)
        del quiet[:]
        check("4 correct dump passes",
              check_dump_bytes(rows, good, out=capture) == 0,
              " | ".join(quiet[-3:]))
        for name, table, n_bad in (("4 identity", IDENTITY, 29),
                                   ("5 wrong direction",
                                    FPGA_DET_FROM_SUBCOL, 23)):
            dump = fpga_elut(rows, table)
            del quiet[:]
            rc = check_dump_bytes(rows, dump, out=capture)
            nrows = len(rows_differing(dump, good))
            check("%s dump fails with %d x 12 bad rows" % (name, n_bad),
                  rc == 1 and nrows == n_bad * N_PIX
                  and any("bad rows %d" % (n_bad * N_PIX) in s
                          for s in quiet),
                  "rc=%d rows=%d" % (rc, nrows))
    run("4/5 mapping traps", t45)

    # 6. axis trap
    def t6():
        dump = fpga_elut(rows, SUBCOL_FROM_FPGA_DET, layout="fpga_pix")
        check("6 [fpga_det][pix][adc] dump fails",
              check_dump_bytes(rows, dump, out=capture) == 1)
    run("6 axis", t6)

    # 7. shared-builder trap: a B[] with no B0 shift, built the same way
    # for the dump and for the checker's replay
    def t7():
        # the plan's form, B[c] = e[c], B31 = B32 = 0: B31 = 0 lets ebin
        # 31 claim [0, 4096) last, so the FSW loop gives all 31
        def shifted(e):
            return list(e) + [0, 0]
        dump = fpga_elut(rows, SUBCOL_FROM_FPGA_DET, b_of=shifted)
        del quiet[:]
        # (with b_of=shifted the sensitivity step itself exits 2: every
        # mapping gives the same all-31 table)
        rc = check_dump_bytes(rows, dump, out=capture)
        check("7 B[c] = e[c], B31 = B32 = 0 degenerates to constant 31",
              rc == 1 and dump == bytes([31]) * DUMP_SIZE
              and any("constant 31" in s for s in quiet))

        # a non-degenerate one: B[c] = e[c], B31 = 4095, B32 = 0
        def shifted_capped(e):
            return list(e) + [N_ADC - 1, 0]
        dump = fpga_elut(rows, SUBCOL_FROM_FPGA_DET, b_of=shifted_capped)
        del quiet[:]
        rc = check_dump_bytes(rows, dump, out=capture, b_of=shifted_capped)
        check("7 the replay of the same shifted B[] agrees",
              any(s.startswith("replay: PASS") for s in quiet),
              " | ".join(quiet[-3:]))
        check("7 the independent oracle fails it",
              rc == 1 and any(s.startswith("oracle: FAIL") for s in quiet),
              " | ".join(quiet[-3:]))
    run("7 shared builder", t7)

    # 8. constant dumps
    def t8():
        for v in (0, 31):
            dump = bytes([v]) * DUMP_SIZE
            del quiet[:]
            rc = check_dump_bytes(rows, dump, out=capture)
            check("8 all-%d fails, reported constant" % v,
                  rc == 1 and any("constant %d" % v in s for s in quiet),
                  " | ".join(quiet[-2:]))
            check("8 all-%d --expect-constant %d -> 0" % (v, v),
                  check_dump_bytes(rows, dump, v, out=capture) == 0)
            w = 31 - v
            check("8 all-%d --expect-constant %d -> 1" % (v, w),
                  check_dump_bytes(rows, dump, w, out=capture) == 1)
        check("8 short dump -> 2",
              check_dump_bytes(rows, b"\0" * 100, out=capture) == 2)
    run("8 constants", t8)

    # 9. the tables
    def t9():
        s2f, f2s = SUBCOL_FROM_FPGA_DET, FPGA_DET_FROM_SUBCOL
        check("9 permutations", sorted(s2f) == IDENTITY
              and sorted(f2s) == IDENTITY)
        check("9 inverse", all(f2s[s2f[f]] == f for f in IDENTITY)
              and all(s2f[f2s[s]] == s for s in IDENTITY))
        check("9 fixed points {0, 1, 4}",
              [f for f in IDENTITY if s2f[f] == f] == [0, 1, 4])
        agree = [f for f in IDENTITY if f2s[f] == s2f[f]]
        check("9 wrong direction agrees at {0,1,4,7,8,12,13,18,25}",
              agree == [0, 1, 4, 7, 8, 12, 13, 18, 25], str(agree))
    run("9 tables", t9)

    # 10. the apply loop, from e[]
    def t10():
        val = apply_row([0] * N_BOUNDARIES, prior=7)
        check("10 all-zero row gives all 31", val == bytes([31]) * N_ADC)
        e = rows[(9, 7)]
        val = apply_row(boundaries(e))
        check("10 ebin 0 at adc e0-1, 1 at e0",
              val[e[0] - 1] == 0 and val[e[0]] == 1)
        check("10 ebin 31 at e30 and at 4095",
              val[e[30]] == 31 and val[4095] == 31)
        check("10 no 32", 32 not in val)
    run("10 apply loop", t10)

    # the parser on the synthetic text, both line ends
    def t_parse():
        for eol in ("\r\n", "\n"):
            got = parse_csv_text(synth_csv_text(rows, eol))
            check("csv parse %r" % eol, got == rows)
    run("csv parse", t_parse)

    if failures:
        print("SELF-TEST: FAIL (%d: %s)" % (len(failures),
                                            ", ".join(failures)))
        return 1
    print("SELF-TEST: PASS")
    return 0


# ----------------------------------------------------------------- main

def csv_path(args):
    p = args.csv or os.environ.get("STIX_ELUT_CSV")
    if not p:
        raise CsvError("no CSV: give --csv PATH or set STIX_ELUT_CSV")
    return p


def record_line(path, rows, lut_id):
    with open(path, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    e0 = [e[0] for e in rows.values()]
    e30 = [e[30] for e in rows.values()]
    return ("ELUT CSV %s sha256 %s rows %d edges %d..%d (e0 %d..%d, "
            "e30 %d..%d) lut_id %d B0=0 B32=0 (user decision 2026-11-11)"
            % (path, digest, len(rows), min(e0), max(e30), min(e0),
               max(e0), min(e30), max(e30), lut_id))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    enc = sub.add_parser("encode")
    enc.add_argument("--csv")
    enc.add_argument("--lut-id", type=lambda s: int(s, 0), default=1)
    enc.add_argument("--first-seq", type=lambda s: int(s, 0), default=0x010)
    enc.add_argument("--t0", type=float, default=50.0)
    enc.add_argument("--pace", type=float, default=0.75)
    enc.add_argument("--ack-window", type=float, default=5.0)
    enc.add_argument("--apply-window", type=float, default=38.0)
    enc.add_argument("--emit", choices=("probe-args", "hex"),
                     default="probe-args")
    enc.add_argument("--no-apply", action="store_true",
                     help="leave out TC(237,8) (elut-smoke --negative)")
    chk = sub.add_parser("check-dump")
    chk.add_argument("--csv")
    chk.add_argument("--dump", required=True)
    chk.add_argument("--expect-constant", type=int, choices=(0, 31))
    sub.add_parser("self-test")
    args = ap.parse_args(argv)

    if args.cmd == "self-test":
        return self_test()
    try:
        path = csv_path(args)
        rows = parse_csv_file(path)
    except (OSError, CsvError) as e:
        print("elut-upload: %s" % e, file=sys.stderr)
        return 2
    if args.cmd == "check-dump":
        try:
            with open(args.dump, "rb") as f:
                dump = f.read()
        except OSError as e:
            print("elut-upload: %s" % e, file=sys.stderr)
            return 2
        return check_dump_bytes(rows, dump, args.expect_constant)

    if not 0 <= args.lut_id <= 0xFFFF:
        print("elut-upload: --lut-id %d outside 0..65535" % args.lut_id,
              file=sys.stderr)
        return 2
    if not 0 <= args.first_seq <= 0x3FFF - N_ELUT_TCS:
        print("elut-upload: --first-seq 0x%X: seqs 0x%X..0x%X must fit in "
              "14 bits" % (args.first_seq, args.first_seq,
                           args.first_seq + N_ELUT_TCS), file=sys.stderr)
        return 2
    if args.pace <= 0 or args.t0 < 0:
        print("elut-upload: --t0 must be >= 0 and --pace > 0",
              file=sys.stderr)
        return 2
    print(record_line(path, rows, args.lut_id), file=sys.stderr)
    units = elut_units(rows, args.first_seq)
    seq_apply = args.first_seq + N_ELUT_TCS
    apply = apply_unit(args.lut_id, seq_apply)
    if args.no_apply:
        print("no TC(237,8): the upload is not applied or stored",
              file=sys.stderr)
    if args.emit == "hex":
        for u in units + ([] if args.no_apply else [apply]):
            print(unit_hex(u))
        return 0
    out = []
    for k, u in enumerate(units):
        a = ack_data(args.first_seq + k)
        out += ["--send-at", fmt_t(args.t0 + args.pace * k),
                "--send-hex", unit_hex(u),
                "--expect", "0x5A1:1:1:%s@%s" % (a, fmt_t(args.ack_window)),
                "--expect", "0x5A1:1:7:%s@%s" % (a, fmt_t(args.ack_window))]
    a = ack_data(seq_apply)
    if not args.no_apply:
        out += ["--send-at", fmt_t(args.t0 + args.pace * N_ELUT_TCS + 5),
                "--send-hex", unit_hex(apply),
                "--expect", "0x5A1:1:1:%s@%s" % (a, fmt_t(args.ack_window)),
                "--expect",
                "0x5A1:1:7:%s@%s" % (a, fmt_t(args.apply_window))]
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
