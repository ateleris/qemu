#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""TM(21,6) SSID-24 L4 spectrogram decoder for the GH #1061 replay
(L4 plan D-L4b, docs/stix-port/2026-11-13-l4-replay-plan.md).
Standard library only.

parse      decode a 0x5BC dump (or a bundle .asc capture): one report
           line per request number found
report     the wrapper's mode: one request (A = the data request, B =
           the empty-window request) with the comparison rules of
           D-L4b: expected-set vs the truth file, the ghost check,
           sample identity vs a second capture, the 0x543F count
self-test  run the built-in tests (no files needed)

Input lines are whole CCSDS packets in hex: the tmtc-probe.py
--dump-apid framing (unit[4:], SSID at byte 16), byte-compatible with
the bundle's .asc captures (verify_l4_indep.py:24 reads the payload
from byte 16).  A line that is not APID 0x5BC TM(21,6) SSID-24 is
skipped; a malformed 0x5BC/SSID-24 line is an error (exit 1), never a
silent gap.

Wire format: PUS data field at byte 16 (TMData.h:56), then
TM21_HEADER_SIZE 19 B of science header (TM_21.c:23): SSID u8 = 24,
PID u16 = 91, PSC u16 (the TC's sequence control), request number
u32, compression schema count u8, trigger u8, start coarse u32 + fine
u16, number of structures u16.  Each L4 structure (fill_report_L4,
TM_21.c:930-936): pixelMask u16, detector mask u32, rcr u8,
energyBinning u16, n_spectra u16, the sample bytes, closing time
offset u16.  Each sample: rel time u24, compressed trigger u8, number
of energies u8, one count byte per energy bin.  rel = ((chunk_ms -
start_ms) + 50) / 100, 24-bit (TM_21.c:37-43), where ms = coarse *
1000 + fine * 1000 / 65536 (Timing_ToMs, Timing.c:395-399); the
packet's start time is the request's (SciReport.c:441).

Truth file (written by l4-replay.sh phase 2, L4-3; read here): '#'
comments, blank lines, then per archived chunk 'chunk <SCET s> <rcr>'
(in collection order; SCET decimal seconds, 0.1 s resolution) and per
RAW_COUNTS file 'file <name> <SCET s> <chunks>' (informational; its
chunk total must match the chunk lines).  The expected set for
request A is one rel per chunk line (the wrapper sends A with start =
the truth's first chunk and a box covering the whole collection, so
every chunk contributes exactly one sample: chunks exceed the 0.1-s
bin, tunit 1); chunks before the request start are excluded.  The
TRUTH line also reports last3files and red-round = last3files + 1:
the "15 = 14 + 1" RED signature (TEST_PLAN §8) the wrapper checks the
RED totals against.

Report lines (L4-3's wrapper greps these):
  CAPTURE <file>: lines=<n> packets=<p> skipped=<s>
  TRUTH <file>: chunks=<c> files=<f> last3files=<k> red-round=<k1>
      rcr=[..]
  REQUEST <req> (A|B): packets=<p> samples=<s> rel=<min>..<max>
      dups=<d> rcr=[..] ebins=[..] pixel=[..] closing=[..]
  EXPECTED-SET: PASS|FAIL missing=<m> extra=<e> (expected=<n>
      decoded=<d>) | skipped (<reason>)
  SAMPLE-IDENTITY vs <file>: matched=<m> samediff=<d> absent=<a>
      (of <n>)
  GHOST: primary|rel-times-only|no|none|skipped - <detail>
  EVENTS <file>: packets=<p> tm52=<t> eid543f=<g>
Exit codes: 0 = decoded (data verdicts are the printed lines, the
wrapper's Goal-2 rules decide), 1 = malformed input, 2 = usage.

Vectors: the self-test's request-711/712 packets are assembled from
the bundle capture results/20261110_red3_711-712.asc:1-2 (distinctive
bytes transcribed, zero runs explicit; the ghost line's structure is
byte-identical to line 1's); the synthetic GREEN packets follow the
documented decode (README.md:34-40, TEST_PLAN §8,
docs/GH1061_L4_override_INVESTIGATION.md §"Hardware confirmation":
1799 samples rel 3..89903 delta 50, RED 15, ghost, 0 for the empty
window), and their first packet's prefix must equal capture line 1 of
results/20261110_green3_611-612.asc through sample 0.
"""
import argparse
import struct
import sys

APID_L4 = 0x5BC          # TM(21,6), PID 91 category 12 (TM_21.c:737-765)
APID_EVENTS = 0x5A7      # TM(5,x) event reports
SSID_L4 = 24
PUS_DATA_FIELD = 16      # TMData_DATA_FIELD (TMData.h:56)
TM21_HDR = 19            # TM21_HEADER_SIZE (TM_21.c:23)
STRUCT_HDR = 11          # fill_report_L4 fixed part (TM_21.c:930-936)
SAMPLE_HDR = 5           # rel u24 + trigger u8 + n_energies u8
REL_MASK = 0xFFFFFF      # the rel field is 24 bits (TM_21.c:42)
EID_NO_DATA = 0x543F     # TM(5,2) no data found (SciReport.c:535-544)


class DecodeError(Exception):
    """A capture, truth or events file is malformed."""


class Sample:
    def __init__(self, rel, trig, counts):
        self.rel = rel
        self.trig = trig
        self.counts = counts


class Structure:
    """One L4 structure; raw = its bytes pixelMask..closing offset,
    masked = raw with every sample's rel-time bytes zeroed (the
    D-L4b ghost fallback compares masked bytes)."""

    def __init__(self, pixel, det, rcr, ebin, samples, closing, raw):
        self.pixel = pixel
        self.det = det
        self.rcr = rcr
        self.ebin = ebin
        self.samples = samples
        self.closing = closing
        self.raw = raw
        self.masked = _zero_rels(raw)


class Packet:
    def __init__(self, lineno, psc, req, comp, trig, coarse, fine,
                 nstruct, structures):
        self.lineno = lineno
        self.psc = psc
        self.req = req
        self.comp = comp
        self.trig = trig
        self.start_coarse = coarse
        self.start_fine = fine
        self.nstruct = nstruct
        self.structures = structures

    def start_ms(self):
        # Timing_ToMs (Timing.c:395-399)
        return self.start_coarse * 1000 + self.start_fine * 1000 // 0x10000


def _u16(b, o):
    return (b[o] << 8) | b[o + 1]


def _u24(b, o):
    return (b[o] << 16) | (b[o + 1] << 8) | b[o + 2]


def _u32(b, o):
    return (b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]


def _zero_rels(raw):
    out = bytearray(raw)
    off = STRUCT_HDR
    end = len(raw) - 2                       # the closing offset
    while off + SAMPLE_HDR <= end:
        neng = raw[off + 4]
        out[off:off + 3] = b"\x00\x00\x00"
        off += SAMPLE_HDR + neng
    return bytes(out)


def parse_l4_line(line, lineno):
    """One dump line -> Packet, or None to skip (not APID 0x5BC
    TM(21,6) SSID-24).  DecodeError on a malformed one."""
    try:
        raw = bytes.fromhex(line.strip())
    except ValueError:
        raise DecodeError("line %d: not hex" % lineno)
    if len(raw) < 6:
        raise DecodeError("line %d: shorter than a CCSDS header" % lineno)
    if (_u16(raw, 0) & 0x7FF) != APID_L4 or raw[0] & 0x10:
        return None                          # not a 0x5BC TM packet
    want = _u16(raw, 4) + 7
    if len(raw) != want:
        raise DecodeError("line %d: truncated packet: %d B, length "
                          "field says %d" % (lineno, len(raw), want))
    if raw[7] != 21 or raw[8] != 6:
        return None                          # 0x5BC but not TM(21,6)
    d = raw[PUS_DATA_FIELD:]
    if len(d) < TM21_HDR:
        raise DecodeError("line %d: SSID-24 science header truncated"
                          % lineno)
    if d[0] != SSID_L4:
        return None                          # not an SSID-24 packet
    nstruct = _u16(d, 17)
    off = TM21_HDR
    structures = []
    for si in range(nstruct):
        if off + STRUCT_HDR > len(d):
            raise DecodeError("line %d: structure %d header truncated"
                              % (lineno, si))
        sbegin = off
        pixel = _u16(d, off)
        det = _u32(d, off + 2)
        rcr = d[off + 6]
        ebin = _u16(d, off + 7)
        nspec = _u16(d, off + 9)
        off += STRUCT_HDR
        samples = []
        for k in range(nspec):
            if off + SAMPLE_HDR > len(d):
                raise DecodeError("line %d: structure %d sample %d "
                                  "truncated" % (lineno, si, k))
            neng = d[off + 4]
            if off + SAMPLE_HDR + neng > len(d):
                raise DecodeError("line %d: structure %d sample %d: "
                                  "%d energy bytes truncated"
                                  % (lineno, si, k, neng))
            samples.append(Sample(_u24(d, off), d[off + 3],
                                  d[off + 5:off + 5 + neng]))
            off += SAMPLE_HDR + neng
        if off + 2 > len(d):
            raise DecodeError("line %d: structure %d closing offset "
                              "truncated" % (lineno, si))
        closing = _u16(d, off)
        off += 2
        structures.append(Structure(pixel, det, rcr, ebin, samples,
                                    closing, d[sbegin:off]))
    if off != len(d):
        raise DecodeError("line %d: %d trailing bytes after %d "
                          "structure(s)"
                          % (lineno, len(d) - off, nstruct))
    return Packet(lineno, _u16(d, 3), _u32(d, 5), d[9], d[10],
                  _u32(d, 11), _u16(d, 15), nstruct, structures)


def parse_capture(path):
    """-> (packets, data lines, skipped lines)."""
    packets = []
    lines = skipped = 0
    try:
        f = open(path)
    except OSError as e:
        raise DecodeError("cannot read %s: %s" % (path, e))
    with f:
        for lineno, line in enumerate(f, 1):
            if not line.strip():
                continue
            lines += 1
            pkt = parse_l4_line(line, lineno)
            if pkt is None:
                skipped += 1
            else:
                packets.append(pkt)
    return packets, lines, skipped


def rel_of(start_ms, chunk_ms):
    # (delta + 50) / 100, 24-bit (TM_21.c:37-43)
    return (((chunk_ms - start_ms) + 50) // 100) & REL_MASK


class TruthChunk:
    def __init__(self, ms, rcr):
        self.ms = ms
        self.rcr = rcr


class TruthFileRec:
    def __init__(self, name, ms, nchunks):
        self.name = name
        self.ms = ms
        self.nchunks = nchunks


class Truth:
    def __init__(self, chunks, files):
        self.chunks = chunks
        self.files = files

    def last3files(self):
        return sum(f.nchunks for f in self.files[-3:])

    def red_round(self):
        """The RED survivors: the last search round's chunks (the
        wrapped builds search 3 files per round) plus the one
        carried-over time-bin sample (TEST_PLAN §8)."""
        if len(self.files) < 2:
            return None
        return self.last3files() + 1


def _scet_ms(text):
    return int(round(float(text) * 1000))


def read_truth_lines(lines):
    chunks = []
    files = []
    for ln, line in enumerate(lines, 1):
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        t = s.split()
        try:
            if t[0] == "chunk" and len(t) == 3:
                chunks.append(TruthChunk(_scet_ms(t[1]), int(t[2])))
            elif t[0] == "file" and len(t) == 4:
                files.append(TruthFileRec(t[1], _scet_ms(t[2]),
                                          int(t[3])))
            else:
                raise ValueError("want 'chunk <scet> <rcr>' or "
                                 "'file <name> <scet> <chunks>'")
        except ValueError as e:
            raise DecodeError("truth line %d: %s (%s)" % (ln, s, e))
    if files and sum(f.nchunks for f in files) != len(chunks):
        raise DecodeError("truth: the file lines hold %d chunks but "
                          "there are %d chunk lines"
                          % (sum(f.nchunks for f in files), len(chunks)))
    return Truth(chunks, files)


def read_truth(path):
    try:
        f = open(path)
    except OSError as e:
        raise DecodeError("cannot read %s: %s" % (path, e))
    with f:
        return read_truth_lines(f)


def count_event_lines(lines):
    """TM(5,x) event lines -> counts; the EID is the first app-data
    word (byte 16, the probe's 0x5A7:5:2:543F... prefix matching)."""
    res = {"packets": 0, "tm52": 0, "eid543f": 0, "skipped": 0}
    for lineno, line in enumerate(lines, 1):
        if not line.strip():
            continue
        try:
            raw = bytes.fromhex(line.strip())
        except ValueError:
            raise DecodeError("line %d: not hex" % lineno)
        if len(raw) < 6:
            raise DecodeError("line %d: shorter than a CCSDS header"
                              % lineno)
        if (_u16(raw, 0) & 0x7FF) != APID_EVENTS or raw[0] & 0x10:
            res["skipped"] += 1
            continue
        if len(raw) != _u16(raw, 4) + 7:
            raise DecodeError("line %d: truncated packet" % lineno)
        res["packets"] += 1
        if raw[7] == 5 and raw[8] == 2:
            res["tm52"] += 1
            if raw[PUS_DATA_FIELD:PUS_DATA_FIELD + 2] == b"\x54\x3F":
                res["eid543f"] += 1
    return res


def count_events(path):
    try:
        f = open(path)
    except OSError as e:
        raise DecodeError("cannot read %s: %s" % (path, e))
    with f:
        return count_event_lines(f)


# ------------------------------------------------------ comparison rules

def analyze_a(packets, req_no, truth=None, against=None, start_ms=None):
    """Request A: the numbers, the expected-set compare vs the truth
    (D-L4b) and the sample identity vs a second capture (Goal-2)."""
    pk = [p for p in packets if p.req == req_no]
    samples = [s for p in pk for st in p.structures for s in st.samples]
    rels = [s.rel for s in samples]
    res = {"req": req_no, "packets": len(pk), "samples": len(samples),
           "rel_min": min(rels) if rels else None,
           "rel_max": max(rels) if rels else None,
           "dups": len(rels) - len(set(rels)),
           "rcrs": sorted({st.rcr for p in pk for st in p.structures}),
           "ebins": sorted({st.ebin for p in pk for st in p.structures}),
           "pixels": sorted({st.pixel for p in pk for st in p.structures}),
           "closings": sorted({st.closing for p in pk
                               for st in p.structures})}
    if truth is not None:
        st = pk[0].start_ms() if pk else start_ms
        if st is None:
            res["expected"] = None
        else:
            exp = {rel_of(st, c.ms) for c in truth.chunks
                   if c.ms + 50 >= st}
            got = set(rels)
            missing = len(exp - got)
            extra = len(got - exp)
            res["expected"] = {"n": len(exp), "missing": missing,
                               "extra": extra,
                               "ok": (missing == 0 and extra == 0
                                      and res["dups"] == 0)}
    if against is not None:
        amap = {}
        for p in against:
            for st2 in p.structures:
                for s in st2.samples:
                    amap[s.rel] = (s.trig, s.counts)
        matched = samediff = absent = 0
        for s in samples:
            other = amap.get(s.rel)
            if other is None:
                absent += 1
            elif other == (s.trig, s.counts):
                matched += 1
            else:
                samediff += 1
        res["identity"] = {"matched": matched, "samediff": samediff,
                           "absent": absent}
    return res


def analyze_b(packets, req_no):
    """Request B: the numbers and the D-L4b ghost check against the
    last structure of the other request in the same capture."""
    pk = [p for p in packets if p.req == req_no]
    b_structs = [st for p in pk for st in p.structures]
    a_last = None
    a_req = None
    for p in packets:
        if p.req == req_no:
            continue
        if p.structures:
            a_last = p.structures[-1]
            a_req = p.req
    samples = [s for st in b_structs for s in st.samples]
    rels = [s.rel for s in samples]
    res = {"req": req_no, "packets": len(pk), "samples": len(samples),
           "rel_min": min(rels) if rels else None,
           "rel_max": max(rels) if rels else None,
           "dups": len(rels) - len(set(rels)),
           "rcrs": sorted({st.rcr for st in b_structs}),
           "ebins": sorted({st.ebin for st in b_structs}),
           "pixels": sorted({st.pixel for st in b_structs}),
           "closings": sorted({st.closing for st in b_structs})}
    res["ghost"], res["ghost_detail"] = \
        ghost_verdict(b_structs, a_last, a_req)
    return res


def ghost_verdict(b_structs, a_last, a_req):
    """D-L4b: primary = every B structure byte-identical to A's last,
    including the rel-time bytes and the closing offset; fallback =
    identical except the rel-time bytes (probable ghost, named, but
    the PASS verdict still requires the primary)."""
    if not b_structs:
        return "none", "0 B structures"
    if a_last is None:
        return "skipped", "no other request's structures in the capture"
    if all(st.raw == a_last.raw for st in b_structs):
        return ("primary",
                "%d B structure(s) byte-identical to A's last (req %s, "
                "%d B)" % (len(b_structs), a_req, len(a_last.raw)))
    if all(st.masked == a_last.masked for st in b_structs):
        diffs = []
        for st in b_structs:
            for j, (s, o) in enumerate(zip(st.samples, a_last.samples)):
                if s.rel != o.rel:
                    diffs.append("B sample %d rel %d vs %d"
                                 % (j, s.rel, o.rel))
        return ("rel-times-only",
                "identical except rel-time bytes (probable ghost; the "
                "PASS verdict needs the primary): %s"
                % "; ".join(diffs[:4]))
    st = b_structs[0]
    if len(st.raw) != len(a_last.raw):
        return ("no", "B structure is %d B, A's last is %d B"
                % (len(st.raw), len(a_last.raw)))
    for i, (x, y) in enumerate(zip(st.raw, a_last.raw)):
        if x != y:
            return ("no", "first difference at structure byte %d: "
                    "%02X vs %02X" % (i, x, y))
    return "no", "structures differ"


# -------------------------------------------------------------- printing

def _fmt_set(vals, fmt):
    return "[" + ",".join(fmt % v for v in vals) + "]"


def _fmt_range(res):
    if res["samples"] == 0:
        return "-..-"
    return "%d..%d" % (res["rel_min"], res["rel_max"])


def request_line(res, letter=None):
    lbl = "REQUEST %d" % res["req"]
    if letter:
        lbl += " (%s)" % letter
    return ("%s: packets=%d samples=%d rel=%s dups=%d rcr=%s ebins=%s "
            "pixel=%s closing=%s"
            % (lbl, res["packets"], res["samples"], _fmt_range(res),
               res["dups"], _fmt_set(res["rcrs"], "%d"),
               _fmt_set(res["ebins"], "0x%x"),
               _fmt_set(res["pixels"], "0x%x"),
               _fmt_set(res["closings"], "%d")))


def expected_line(exp):
    if exp is None:
        return "EXPECTED-SET: skipped (no packets and no --start)"
    return "%s missing=%d extra=%d (expected=%d decoded=%d)" \
        % ("EXPECTED-SET: PASS" if exp["ok"] else "EXPECTED-SET: FAIL",
           exp["missing"], exp["extra"], exp["n"],
           exp["n"] - exp["missing"] + exp["extra"])


def identity_line(res, path):
    ident = res["identity"]
    return ("SAMPLE-IDENTITY vs %s: matched=%d samediff=%d absent=%d "
            "(of %d)" % (path, ident["matched"], ident["samediff"],
                         ident["absent"], res["samples"]))


def truth_line(truth, path):
    rr = truth.red_round()
    rcrs = sorted({c.rcr for c in truth.chunks})
    return ("TRUTH %s: chunks=%d files=%d last3files=%d red-round=%s "
            "rcr=%s"
            % (path, len(truth.chunks), len(truth.files),
               truth.last3files() if truth.files else 0,
               rr if rr is not None else "n/a",
               _fmt_set(rcrs, "%d")))


def _req_counts(packets, req):
    pk = [p for p in packets if p.req == req]
    return (len(pk),
            sum(len(st.samples) for p in pk for st in p.structures))


def _print_others(packets, req):
    for other in sorted({p.req for p in packets} - {req}):
        n, s = _req_counts(packets, other)
        print("REQ %d: packets=%d samples=%d" % (other, n, s))


# ------------------------------------------------------------- self-test

def _build_line(req, psc, start_coarse, start_fine, structs,
                word23=0xC000, ts=(0, 0), comp=43, trig=43):
    """One whole-CCSDS-hex line, the dump framing (test-only)."""
    d = bytearray()
    d.append(SSID_L4)
    d += struct.pack(">HHIBBIHH", 91, psc, req, comp, trig,
                     start_coarse, start_fine, len(structs))
    for pixel, det, rcr, ebin, samples, closing in structs:
        d += struct.pack(">HIBHH", pixel, det, rcr, ebin, len(samples))
        for rel, strig, counts in samples:
            d += struct.pack(">I", rel)[1:]
            d += bytes((strig, len(counts))) + bytes(counts)
        d += struct.pack(">H", closing)
    head = struct.pack(">HHH", 0x0DBC, word23, len(d) + 9)
    pus = bytes((0x10, 21, 6, 0)) + struct.pack(">IH", ts[0], ts[1])
    return (head + pus + bytes(d)).hex()


def _build_event(eid, params=b"", ptype=5, sub=2, ts=(0, 0)):
    app = struct.pack(">H", eid) + params
    head = struct.pack(">HHH", 0x0D00 | APID_EVENTS, 0xC000,
                       len(app) + 9)
    pus = bytes((0x10, ptype, sub, 0)) + struct.pack(">IH", ts[0], ts[1])
    return (head + pus + app).hex()


# The bundle's RED capture (results/20261110_red3_711-712.asc), lines
# 1-2: request 711's only packet and the 712 ghost.  Each is one whole
# CCSDS packet, 363 B = 726 hex chars: 19-B science header, one 328-B
# structure (15 x 21-B samples, closing offset 50).  The distinctive
# bytes below are transcribed from the capture; the zero runs are
# written explicitly (all count bytes are 0: pixel mask 0, TEST_PLAN
# §8 caveat).  Line 2 is byte-identical to line 1 from the start-fine
# field on, under 712's own start time 595160000 (the D2 ghost).
_RED_711_HEAD = ("0DBCC000016410150600237997C28428"
                 "18005BC204000002C72B2B2379394E")
_RED_712_HEAD = ("0DBCC0010164101506002379986F8F96"
                 "18005BC208000002C82B2B23796BC0")
_RED_RELS = ("015C73", "015CA5", "015CD7", "015D09", "015D3B",
             "015D6D", "015D9F", "015DD1", "015E03", "015E35",
             "015E67", "015E99", "015ECB", "015EFD", "015F2F")
RED_711 = (_RED_711_HEAD + "0000" + "0001"      # fine, nstruct
           + "0000" + "FFFFFFFF"                # pixel, detector
           + "00" + "03E1" + "000F"             # rcr, ebin, nspec
           + "".join(r + "00" + "10" + "0" * 32 for r in _RED_RELS)
           + "0032")                            # closing offset 50
RED_712 = _RED_712_HEAD + RED_711[len(_RED_711_HEAD):]

# The bundle's GREEN capture line 1 (results/20261110_green3_611-612.
# asc:1) through sample 0, 67 B: the synthetic packet 0 must rebuild
# these bytes (same field walk, nspec 0x00C1 = 193, rel 3).
GREEN_611_PREFIX = (
    "0DBC40000FFE101506002379893D6C0A18005BC201000002632B2B2379394E"
    + "000000010000" + "FFFFFFFF" + "00" + "03E1" + "00C1"
    + "000003" + "0010" + "0" * 32)


def _green_lines():
    """The GREEN capture's documented shape: 10 packets, 9 x 193 + 62
    samples, rel 3..89903 step 50, req 611 PSC 0xC201, start
    595147086 (tcsend_238_gh1061_cnatural.txt:16), ebin 0x03E1, rcr 0,
    closing 50; packet 0 carries the capture's own TM seq/time."""
    lines = []
    n = [193] * 9 + [62]
    done = 0
    for i, nspec in enumerate(n):
        samples = [(3 + 50 * (done + j), 0, bytes(16))
                   for j in range(nspec)]
        done += nspec
        kw = {}
        if i == 0:
            kw = dict(word23=0x4000, ts=(0x2379893D, 0x6C0A))
        lines.append(_build_line(611, 0xC201, 595147086, 0,
                                 [(0, 0xFFFFFFFF, 0, 0x03E1, samples,
                                   50)], **kw))
    return lines


def _truth_lines():
    """The collection's documented shape (TEST_PLAN §8): 1799 chunks
    at 5 s from SCET 595147086.3, RCR 0; 359 files of 5 chunks + 1 of
    4 = 360 files."""
    out = ["# synthetic truth, TEST_PLAN §8 shape"]
    for i in range(1799):
        ms = 595147086300 + 5000 * i
        out.append("chunk %d.%03d 0" % (ms // 1000, ms % 1000))
    done = 0
    for fi in range(360):
        nchunks = 4 if fi == 359 else 5
        ms = 595147086300 + 5000 * done
        out.append("file RAW_COUNTS_%d %d.%03d %d"
                   % (fi + 1, ms // 1000, ms % 1000, nchunks))
        done += nchunks
    return out


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
        except Exception as e:  # a crash is a failure, not a stop
            check(name, False, "%s: %s" % (type(e).__name__, e))

    # 1. the capture-derived RED vectors parse to the documented
    #    numbers (README.md:34-40, TEST_PLAN §8)
    def t1():
        check("1 711 is the capture's 363-B packet",
              len(RED_711) == 2 * 363, str(len(RED_711)))
        p711 = parse_l4_line(RED_711, 1)
        check("1 711 req/PSC/start", (p711.req, p711.psc) == (711, 0xC204)
              and p711.start_coarse == 595147086
              and p711.start_fine == 0,
              "req %s psc %#x start %s" % (p711.req, p711.psc,
                                           p711.start_coarse))
        check("1 711 one structure", p711.nstruct == 1)
        st = p711.structures[0]
        check("1 711 structure fields",
              (st.pixel, st.det, st.rcr, st.ebin, st.closing)
              == (0, 0xFFFFFFFF, 0, 0x03E1, 50),
              "%s" % [(hex(st.pixel), hex(st.det), st.rcr, hex(st.ebin),
                       st.closing)])
        rels = [s.rel for s in st.samples]
        check("1 711 15 samples rel 89203..89903 step 50",
              len(rels) == 15 and rels[0] == 89203 and rels[-1] == 89903
              and all(rels[i + 1] - rels[i] == 50 for i in range(14)),
              "%d %s..%s" % (len(rels), rels[:1], rels[-1:]))
        check("1 711 21-B samples, zero counts (pixel mask 0)",
              all(len(s.counts) == 16 and s.trig == 0
                  and s.counts == bytes(16) for s in st.samples))
        check("1 711 structure 328 B",
              len(st.raw) == STRUCT_HDR + 15 * 21 + 2, str(len(st.raw)))
    run("1 RED 711 vector", t1)

    # 2. the ghost: 712's structure is byte-identical to 711's, under
    #    712's own start time 595160000 (TEST_PLAN §8, D-L4b primary)
    def t2():
        check("2 712 is the capture's 363-B packet",
              len(RED_712) == 2 * 363, str(len(RED_712)))
        p711 = parse_l4_line(RED_711, 1)
        p712 = parse_l4_line(RED_712, 2)
        check("2 712 req/PSC/start", (p712.req, p712.psc) == (712, 0xC208)
              and p712.start_coarse == 595160000)
        check("2 structure byte-identical (primary)",
              p712.structures[0].raw == p711.structures[0].raw)
        verdict, _ = ghost_verdict(p712.structures,
                                   p711.structures[0], 711)
        check("2 ghost verdict primary", verdict == "primary", verdict)
    run("2 RED 712 ghost vector", t2)

    # 3. the synthetic GREEN packet 0 rebuilds the capture's line-1
    #    prefix (67 B, through sample 0)
    def t3():
        line = _green_lines()[0]
        check("3 prefix excerpt is 67 B",
              len(GREEN_611_PREFIX) == 2 * 67, str(len(GREEN_611_PREFIX)))
        check("3 GREEN packet 0 prefix == capture line 1",
              line[:len(GREEN_611_PREFIX)].upper() == GREEN_611_PREFIX,
              line[:80])
        check("3 GREEN packet 0 is the capture's 4095-B packet",
              len(line) == 2 * (0x0FFE + 7), str(len(line)))
        pkt = parse_l4_line(line, 1)
        check("3 GREEN packet 0 parses to 193 samples, first rel 3",
              pkt.req == 611 and len(pkt.structures[0].samples) == 193
              and pkt.structures[0].samples[0].rel == 3)
        lines = _green_lines()
        total = sum(len(st.samples) for ln in lines
                    for st in parse_l4_line(ln, 1).structures)
        check("3 GREEN 10 packets, 1799 samples", total == 1799,
              str(total))
    run("3 GREEN shape", t3)

    # 4. the documented numbers end-to-end: expected-set, ghost,
    #    sample identity, events (README.md:34-40)
    def t4():
        green = [parse_l4_line(ln, i + 1)
                 for i, ln in enumerate(_green_lines())]
        red = [parse_l4_line(RED_711, 1), parse_l4_line(RED_712, 2)]
        truth = read_truth_lines(_truth_lines())
        a_green = analyze_a(green, 611, truth, red)
        exp = a_green["expected"]
        check("4 GREEN 611: 1799 samples, set complete",
              a_green["samples"] == 1799 and exp["ok"]
              and exp["missing"] == 0 and exp["extra"] == 0,
              str(exp))
        ident = a_green["identity"]
        check("4 GREEN vs RED identity: 15 matched, 0 diff",
              ident["matched"] == 15 and ident["samediff"] == 0,
              str(ident))
        a_red = analyze_a(red, 711, truth, green)
        exp = a_red["expected"]
        check("4 RED 711: 15 samples, 1784 missing, none extra",
              a_red["samples"] == 15 and exp["missing"] == 1784
              and exp["extra"] == 0 and not exp["ok"], str(exp))
        check("4 truth: 1799 chunks, 360 files, last3 14, round 15",
              (len(truth.chunks), len(truth.files),
               truth.last3files(), truth.red_round())
              == (1799, 360, 14, 15))
        b_red = analyze_b(red, 712)
        check("4 RED 712 ghost primary", b_red["ghost"] == "primary",
              b_red["ghost_detail"])
        b_green = analyze_b(green, 612)
        check("4 GREEN 612: 0 packets, no ghost",
              b_green["packets"] == 0 and b_green["ghost"] == "none",
              b_green["ghost_detail"])
        got = count_event_lines([_build_event(EID_NO_DATA,
                                              b"\x04\x01\x00\x01"),
                                 _build_event(0x14A3, b"",
                                              ptype=5, sub=1)])
        check("4 events: 1 x TM(5,2) 0x543F",
              got == {"packets": 2, "tm52": 1, "eid543f": 1,
                      "skipped": 0}, str(got))
    run("4 documented numbers", t4)

    # 5. ghost fallback and rejection: rel-times-only is named, a
    #    count-byte difference is "no" with the offset
    def t5():
        p711 = parse_l4_line(RED_711, 1)
        st = p711.structures[0]

        def rebuilt(samples):
            return _build_line(712, 0xC208, 595160000, 0,
                               [(st.pixel, st.det, st.rcr, st.ebin,
                                 samples, st.closing)])
        rel = [(s.rel, s.trig, s.counts) for s in st.samples]
        v, _ = ghost_verdict(parse_l4_line(rebuilt(rel), 3).structures,
                             st, 711)
        check("5 rebuilt 712 still primary", v == "primary", v)
        relshift = [(s.rel + 1, s.trig, s.counts) for s in st.samples]
        pkt = parse_l4_line(rebuilt(relshift), 3)
        v, detail = ghost_verdict(pkt.structures, st, 711)
        check("5 rel-times-only named", v == "rel-times-only"
              and "rel 89204 vs 89203" in detail, "%s: %s" % (v, detail))
        badcount = [(s.rel, s.trig, s.counts) for s in st.samples]
        badcount[0] = (badcount[0][0], 0, b"\x01" + bytes(15))
        pkt = parse_l4_line(rebuilt(badcount), 3)
        v, detail = ghost_verdict(pkt.structures, st, 711)
        check("5 count-byte difference rejected with the offset",
              v == "no" and "byte 16" in detail, "%s: %s" % (v, detail))
        shorter = [(s.rel, s.trig, s.counts[:-1]) for s in st.samples]
        pkt = parse_l4_line(rebuilt(shorter), 3)
        v, detail = ghost_verdict(pkt.structures, st, 711)
        check("5 length mismatch rejected", v == "no"
              and "313 B, A's last is 328 B" in detail,
              "%s: %s" % (v, detail))
    run("5 ghost fallback", t5)

    # 6. the rel-time formula: rounding at the 50-ms boundary, 24-bit
    def t6():
        check("6 +0.3 s -> rel 3", rel_of(1000000, 1000300) == 3)
        check("6 +250 ms -> 3, +249 -> 2, +50 -> 1, +49 -> 0",
              rel_of(0, 250) == 3 and rel_of(0, 249) == 2
              and rel_of(0, 50) == 1 and rel_of(0, 49) == 0)
        check("6 24-bit wrap (TM_21.c:42)",
              rel_of(0, 0x1000000 * 100) == 0)
    run("6 rel formula", t6)

    # 7. skip rules and loud failures
    def t7():
        line = _build_line(611, 0xC201, 595147086, 0,
                           [(0, 0xFFFFFFFF, 0, 0x03E1, [], 50)])
        check("7 non-0x5BC line skipped",
              parse_l4_line(_build_event(0x543F), 1) is None)
        other = bytearray(bytes.fromhex(line))
        other[PUS_DATA_FIELD] = 25          # SSID 25
        check("7 non-SSID-24 0x5BC line skipped",
              parse_l4_line(other.hex(), 1) is None)
        for name, bad in [("truncated", line[:-4]),
                          ("not hex", line[:-1] + "zz"),
                          ("short", "0DBC0001")]:
            try:
                parse_l4_line(bad, 1)
                check("7 %s line refused" % name, False, "accepted")
            except DecodeError:
                check("7 %s line refused" % name, True)
        short_struct = _build_line(
            611, 0xC201, 0, 0,
            [(0, 0xFFFFFFFF, 0, 0x03E1, [(1, 0, bytes(16))], 50)])
        # a length-consistent lie: nspec (chars 88..91) says 2, only
        # 1 sample is present -- the walk must fail, not the framing
        bad = short_struct[:88] + "0002" + short_struct[92:]
        try:
            parse_l4_line(bad, 1)
            check("7 truncated structure refused", False, "accepted")
        except DecodeError as e:
            check("7 truncated structure refused",
                  "sample 1 truncated" in str(e), str(e))
    run("7 skip and reject", t7)

    # 8. the truth reader: shapes, round math, loud errors
    def t8():
        t = read_truth_lines(["# c", "",
                              "chunk 100.3 0", "chunk 105.3 0",
                              "chunk 110.3 0", "chunk 120.3 0",
                              "file RAW_COUNTS_a 100.3 3",
                              "file RAW_COUNTS_b 110.3 1"])
        check("8 chunks and files",
              [c.ms for c in t.chunks] == [100300, 105300, 110300,
                                           120300]
              and [f.name for f in t.files] == ["RAW_COUNTS_a",
                                                "RAW_COUNTS_b"])
        check("8 last3files 4, red-round 5",
              t.last3files() == 4 and t.red_round() == 5)
        try:
            read_truth_lines(["chunk 100.3"])
            check("8 short truth line refused", False, "accepted")
        except DecodeError:
            check("8 short truth line refused", True)
        try:
            read_truth_lines(["chunk 100.3 0", "file a 100.3 2"])
            check("8 file/chunk mismatch refused", False, "accepted")
        except DecodeError:
            check("8 file/chunk mismatch refused", True)
        check("8 single file: no red-round",
              read_truth_lines(["chunk 1.0 0", "file a 1.0 1"])
              .red_round() is None)
    run("8 truth reader", t8)

    if failures:
        print("SELF-TEST: FAIL (%d: %s)" % (len(failures),
                                            ", ".join(failures)))
        return 1
    print("SELF-TEST: PASS")
    return 0


# ----------------------------------------------------------------- main

def _int(s):
    return int(s, 0)


def _float(s):
    return float(s)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    rep = sub.add_parser("report", help="the wrapper's A/B report")
    rep.add_argument("--capture", required=True,
                     help="0x5BC dump (whole-CCSDS-hex lines)")
    rep.add_argument("--request", choices=("A", "B"), required=True,
                     help="A = the data request, B = the empty-window "
                          "request")
    rep.add_argument("--req-number", type=_int,
                     help="the request number (A: default the first "
                          "seen; B: required)")
    rep.add_argument("--truth",
                     help="truth file (A mode: the expected set)")
    rep.add_argument("--events", help="0x5A7 event dump (0x543F count)")
    rep.add_argument("--against",
                     help="a second capture for the sample-identity "
                          "check (Goal-2: RED's samples vs GREEN's)")
    rep.add_argument("--start", type=_float,
                     help="request start SCET s, when the capture has "
                          "no A packets")
    par = sub.add_parser("parse", help="decode a capture")
    par.add_argument("--capture", required=True)
    sub.add_parser("self-test", help="run the built-in tests")
    args = ap.parse_args(argv)

    try:
        if args.cmd == "self-test":
            return self_test()
        packets, lines, skipped = parse_capture(args.capture)
        print("CAPTURE %s: lines=%d packets=%d skipped=%d"
              % (args.capture, lines, len(packets), skipped))
        if args.cmd == "parse":
            seen = []
            for p in packets:
                if p.req not in seen:
                    seen.append(p.req)
            for req in seen:
                print(request_line(analyze_a(packets, req)))
            return 0
        truth = read_truth(args.truth) if args.truth else None
        if truth:
            print(truth_line(truth, args.truth))
        if args.request == "A":
            req = args.req_number
            if req is None:
                if not packets:
                    ap.error("--request A: no packets in the capture "
                             "and no --req-number")
                req = packets[0].req
            against = parse_capture(args.against)[0] \
                if args.against else None
            res = analyze_a(packets, req, truth, against,
                            args.start * 1000 if args.start else None)
            print(request_line(res, "A"))
            if truth:
                print(expected_line(res["expected"]))
            if args.against:
                print(identity_line(res, args.against))
            _print_others(packets, req)
        else:
            if args.req_number is None:
                ap.error("--request B needs --req-number")
            res = analyze_b(packets, args.req_number)
            print(request_line(res, "B"))
            print("GHOST: %s - %s" % (res["ghost"], res["ghost_detail"]))
            _print_others(packets, args.req_number)
        if args.events:
            ev = count_events(args.events)
            print("EVENTS %s: packets=%d tm52=%d eid543f=%d"
                  % (args.events, ev["packets"], ev["tm52"],
                     ev["eid543f"]))
        return 0
    except DecodeError as e:
        print("l4-decode: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
