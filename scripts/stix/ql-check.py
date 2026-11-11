#!/usr/bin/env python3
"""Decode and check the STIX quick-look TM (TM(21,6) SSID 30-34).

Input: a dump file as tmtc-probe.py --dump-apid 0x5DC:FILE writes it:
one whole CCSDS packet per line, hex, WITHOUT the 4-byte SpW header,
so the SSID is byte 16. Blank lines and lines starting with '#' are
ignored.

Decode (detector plan "QL TM decode for the gates"; TM_21.c:315-647,
ICD Tables 103-107):
- APID 0x5DC, PUS TM(21,6), SSID at CCSDS byte 16, no CRC/PEC. All
  offsets below are relative to the SSID byte, big-endian.
- Segmentation (CCSDS byte 2 bits 7:6, TMData.h:89-92): 3 standalone,
  1 FIRST, 0 CONTINUATION, 2 LAST. A product is one standalone packet or
  FIRST, CONT..., LAST. Every segment is a complete packet with its own
  header (TMForm_nextSegmentationGroupingFlagQL), so the samples of a
  product are the concatenation of its packets' samples. A LAST with
  zero samples is the empty termination packet and is tolerated.
- Compression S0, schema byte 0 S KKK MMM, default 0x2B (S0, K5, M3,
  CompressionAlgorithms.c:18-90): V < 16 -> C = V; else shift right
  until V < 16, e = shifts + 1, C = (V & 7) + (e << 3). Decode (M = 3):
  C < 16 -> V = C; else V >= (8 + (C & 7)) << ((C >> 3) - 1), a lower
  bound (truncating), the true V < (9 + (C & 7)) << ((C >> 3) - 1).
- SSID 30 (light curves): 0 SSID, 1 coarse u32, 5 fine u16, 7 int
  time u16, 9 detector mask u32 (subcol bits), 13 pixel mask u16 (bit p
  = pix p), 15 LC SKM, 16 TA SKM << 1 | upper edge, 17 lower edge mask
  u32, 21 E; per band u16 N + N bytes; u16 N + N trigger bytes; u16 N +
  N RCR bytes.
- SSID 31 (background): 0 SSID, 1, 5, 7 int time, 9 BG SKM << 1 |
  upper edge, 10 lower edge mask u32, 14 TA SKM, 15 E; per band u16 N +
  N bytes; u16 N + N trigger bytes.
- SSID 32 (spectra): 0 SSID, 1, 5, 7 int time, 9 spectrum SKM, 10 TA
  SKM, 11 pixel mask u16, 13 N; then N samples of 35 B: subcol, 32
  compressed counts (ebin 0..31), trigger, integration offset.
- SSID 33 (variance): 0 SSID, 1, 5, 7 int time, 9 samples per value,
  10 detector mask u32 (subcol bits), 14 energy mask u32 (ebin bits),
  18 pixel mask u16, 20 SKM, 21 N, 23 N bytes.
- SSID 34 (flare flag): 0 SSID, 1, 5, 7 deltaTimeSeconds (the FSW
  name, TM_21.c:624; 0.1 s units, 39), 9 N, then N triplets: status,
  Z, Y (int8 arcmin).
- Sequence counts (CCSDS bytes 2-3, 14 bits): one counter for the whole
  APID (TM_21.c:780, TMForm_SequenceCounters.APID_93_12), shared by
  every QL SSID, so the segments of one product interleave with other
  SSIDs' packets and are NOT contiguous among themselves. Checked: the
  dump as a whole is contiguous (each packet = previous + 1 mod 2^14;
  a gap is a lost packet), and within a FIRST..LAST group the counts
  strictly increase.

--expect-zero-flush SECONDS is the detector-smoke check (tiers 0+1,
the sky is zero): exactly one product per SSID 30-34, default header
fields, every count, trigger, RCR, variance and flare byte 0, and the
sample counts of a NOMINAL window of SECONDS: SSID 30, 33, 34 about
SECONDS/4, SSID 31 about SECONDS/8 (each +-2), SSID 32 exactly one
dump of 32 samples whose subcol bytes are 0..31 once each.

Usage:
  ql-check.py DUMP [--samples] [--expect-zero-flush SECONDS]
  ql-check.py --self-test
Exit codes: 0 decoded (and the check passed), 1 a decode error or a
failed check, 2 bad usage or an unreadable file. stdlib only.
"""
import argparse
import sys

QL_APID = 0x5DC
SSID_OFF = 16          # TMData_DATA_FIELD (TMData.h:56)
SEG_CONT, SEG_FIRST, SEG_LAST, SEG_STANDALONE = 0, 1, 2, 3
SEG_NAMES = {SEG_CONT: "CONT", SEG_FIRST: "FIRST", SEG_LAST: "LAST",
             SEG_STANDALONE: "STANDALONE"}
SSID_NAMES = {30: "light curves", 31: "background", 32: "spectra",
              33: "variance", 34: "flare flag"}
SKM_DEFAULT = 0x2B     # S0, K5, M3 (ParamMgmtDef.h:256-279, 335)
NUM_EBIN = 32          # NUMBER_OF_SCIENCE_CHANNELS
NUM_SUBCOL = 32
SSID32_SAMPLE = 35     # subcol + 32 ebin counts + trigger + offset
SEQ_MOD = 1 << 14      # CCSDS source sequence count


class DecodeError(Exception):
    pass


# --- S0 compression (CompressionAlgorithms.c:45-90, M = 3) ------------

def s0_compress(value, m=3):
    """The FSW S0 compressor for S = 0 (truncating, monotone)."""
    if value < (1 << (m + 1)):
        return value
    shifts = 0
    while value >= (1 << (m + 1)):
        value >>= 1
        shifts += 1
    e = shifts + 1
    c = (value & ((1 << m) - 1)) + (e << m)
    return c if c <= 0xFF else 0xFF


def s0_decode(c, m=3):
    """(lower bound, exclusive upper bound) of the value behind @c."""
    if c < (1 << (m + 1)):
        return c, c + 1
    e = c >> m
    mant = c & ((1 << m) - 1)
    return ((1 << m) + mant) << (e - 1), ((1 << m) + mant + 1) << (e - 1)


# --- packet parsing ---------------------------------------------------

class Reader:
    def __init__(self, data, what):
        self.d = data
        self.i = 0
        self.what = what

    def take(self, n, field):
        if self.i + n > len(self.d):
            raise DecodeError(f"{self.what}: truncated at {field} "
                              f"(rel. offset {self.i}, need {n}, "
                              f"have {len(self.d) - self.i})")
        b = self.d[self.i:self.i + n]
        self.i += n
        return b

    def u8(self, f):
        return self.take(1, f)[0]

    def u16(self, f):
        return int.from_bytes(self.take(2, f), "big")

    def u32(self, f):
        return int.from_bytes(self.take(4, f), "big")

    def done(self):
        if self.i != len(self.d):
            raise DecodeError(f"{self.what}: {len(self.d) - self.i} "
                              f"trailing bytes after rel. offset {self.i}")


def parse_counted(r, field, n_expected=None):
    n = r.u16(field + " N")
    if n_expected is not None and n != n_expected:
        raise DecodeError(f"{r.what}: {field} N = {n}, the first block "
                          f"said {n_expected}")
    return n, list(r.take(n, field))


def parse_ssid30(r):
    h = {"coarse": r.u32("coarse"), "fine": r.u16("fine"),
         "int_time": r.u16("int time"),
         "det_mask_subcol": r.u32("detector mask"),
         "pix_mask": r.u16("pixel mask"), "lc_skm": r.u8("LC SKM")}
    b = r.u8("TA SKM | upper edge")
    h["ta_skm"], h["upper_edge"] = b >> 1, b & 1
    h["lower_edge_mask_ebin"] = r.u32("lower edge mask")
    h["n_bands"] = r.u8("E")
    bands = []
    n = None
    for k in range(h["n_bands"]):
        n, vals = parse_counted(r, f"band {k}", n)
        bands.append(vals)
    n, trig = parse_counted(r, "trigger", n)
    n, rcr = parse_counted(r, "RCR", n)
    r.done()
    n = n or 0
    samples = [{"lc_band": [bands[k][s] for k in range(h["n_bands"])],
                "trigger": trig[s], "rcr": rcr[s]} for s in range(n)]
    return h, samples


def parse_ssid31(r):
    h = {"coarse": r.u32("coarse"), "fine": r.u16("fine"),
         "int_time": r.u16("int time")}
    b = r.u8("BG SKM | upper edge")
    h["bg_skm"], h["upper_edge"] = b >> 1, b & 1
    h["lower_edge_mask_ebin"] = r.u32("lower edge mask")
    h["ta_skm"] = r.u8("TA SKM")
    h["n_bands"] = r.u8("E")
    bands = []
    n = None
    for k in range(h["n_bands"]):
        n, vals = parse_counted(r, f"band {k}", n)
        bands.append(vals)
    n, trig = parse_counted(r, "trigger", n)
    r.done()
    samples = [{"bg_band": [bands[k][s] for k in range(h["n_bands"])],
                "trigger": trig[s]} for s in range(n)]
    return h, samples


def parse_ssid32(r):
    h = {"coarse": r.u32("coarse"), "fine": r.u16("fine"),
         "int_time": r.u16("int time"), "sp_skm": r.u8("spectrum SKM"),
         "ta_skm": r.u8("TA SKM"), "pix_mask": r.u16("pixel mask")}
    n = r.u16("N")
    samples = []
    for s in range(n):
        raw = r.take(SSID32_SAMPLE, f"sample {s}")
        samples.append({"subcol": raw[0], "ebin": list(raw[1:33]),
                        "trigger": raw[33], "int_offset": raw[34]})
    r.done()
    return h, samples


def parse_ssid33(r):
    h = {"coarse": r.u32("coarse"), "fine": r.u16("fine"),
         "int_time": r.u16("int time"),
         "samples_per_value": r.u8("samples per value"),
         "det_mask_subcol": r.u32("detector mask"),
         "energy_mask_ebin": r.u32("energy mask"),
         "pix_mask": r.u16("pixel mask"), "skm": r.u8("SKM")}
    n, vals = parse_counted(r, "variance")
    r.done()
    return h, [{"variance": v} for v in vals]


def parse_ssid34(r):
    # offset 7: the FSW's deltaTimeSeconds (TM_21.c:624), in 0.1 s units
    h = {"coarse": r.u32("coarse"), "fine": r.u16("fine"),
         "delta_time_seconds": r.u16("deltaTimeSeconds")}
    n = r.u16("N")
    samples = []
    for s in range(n):
        st, z, y = r.take(3, f"triplet {s}")
        samples.append({"status": st, "z": z - 256 if z > 127 else z,
                        "y": y - 256 if y > 127 else y})
    r.done()
    return h, samples


PARSERS = {30: parse_ssid30, 31: parse_ssid31, 32: parse_ssid32,
           33: parse_ssid33, 34: parse_ssid34}


def parse_packet(pkt, where):
    """One CCSDS packet -> dict(ssid, seg, seq, header, samples)."""
    if len(pkt) < SSID_OFF + 1:
        raise DecodeError(f"{where}: {len(pkt)} bytes, too short for an "
                          f"SSID at byte {SSID_OFF}")
    apid = ((pkt[0] << 8) | pkt[1]) & 0x7FF
    if apid != QL_APID:
        raise DecodeError(f"{where}: APID 0x{apid:03X}, want 0x{QL_APID:03X}")
    if pkt[0] & 0x10:
        raise DecodeError(f"{where}: a TC packet (type bit set)")
    plen = (pkt[4] << 8) | pkt[5]
    if len(pkt) != plen + 7:
        raise DecodeError(f"{where}: {len(pkt)} bytes, the CCSDS length "
                          f"field says {plen + 7}")
    if (pkt[7], pkt[8]) != (21, 6):
        raise DecodeError(f"{where}: TM({pkt[7]},{pkt[8]}), want TM(21,6)")
    seg = pkt[2] >> 6
    seq = ((pkt[2] & 0x3F) << 8) | pkt[3]
    ssid = pkt[SSID_OFF]
    if ssid not in PARSERS:
        raise DecodeError(f"{where}: SSID {ssid} is not a QL SSID 30-34")
    r = Reader(pkt[SSID_OFF + 1:], f"{where} SSID {ssid}")
    header, samples = PARSERS[ssid](r)
    return {"ssid": ssid, "seg": seg, "seq": seq, "header": header,
            "samples": samples, "where": where}


def reassemble(packets):
    """Group packets per SSID into products (FIRST..LAST or STANDALONE).

    Returns {ssid: [product]}; a product is {"packets": [...],
    "samples": [...], "header": first packet's header,
    "termination": True when it ended in an empty LAST}."""
    products = {}
    open_ = {}
    for p in packets:
        s, seg = p["ssid"], p["seg"]
        cur = open_.get(s)
        if seg == SEG_STANDALONE or seg == SEG_FIRST:
            if cur is not None:
                raise DecodeError(f"{p['where']}: SSID {s} "
                                  f"{SEG_NAMES[seg]} while a product "
                                  f"from {cur['packets'][0]['where']} "
                                  f"is still open")
            prod = {"packets": [p], "samples": list(p["samples"]),
                    "header": p["header"], "termination": False}
            if seg == SEG_STANDALONE:
                products.setdefault(s, []).append(prod)
            else:
                open_[s] = prod
            continue
        if cur is None:
            raise DecodeError(f"{p['where']}: SSID {s} {SEG_NAMES[seg]} "
                              f"without a FIRST")
        step = (p["seq"] - cur["packets"][-1]["seq"]) % SEQ_MOD
        if not 0 < step < SEQ_MOD // 2:
            raise DecodeError(f"{p['where']}: SSID {s} {SEG_NAMES[seg]} "
                              f"seq {p['seq']} does not follow seq "
                              f"{cur['packets'][-1]['seq']} of its group")
        cur["packets"].append(p)
        cur["samples"].extend(p["samples"])
        if seg == SEG_LAST:
            cur["termination"] = len(p["samples"]) == 0
            products.setdefault(s, []).append(cur)
            del open_[s]
    for s, cur in open_.items():
        raise DecodeError(f"SSID {s}: product from "
                          f"{cur['packets'][0]['where']} never closed "
                          f"(no LAST)")
    return products


def read_dump(lines):
    pkts = []
    for n, line in enumerate(lines, 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        try:
            raw = bytes.fromhex(line)
        except ValueError:
            raise DecodeError(f"line {n}: not hex")
        pkts.append(parse_packet(raw, f"line {n}"))
    return pkts


# --- reporting --------------------------------------------------------

def fmt_header(h):
    out = []
    for k, v in h.items():
        if k in ("coarse", "fine"):
            continue
        if "mask" in k:
            out.append(f"{k}=0x{v:0{8 if v > 0xFFFF else 4}X}")
        elif "skm" in k:
            out.append(f"{k}=0x{v:02X}")
        else:
            out.append(f"{k}={v}")
    return " ".join(out)


def sample_values(ssid, smp):
    """Every QL value byte of one sample, for the zero check."""
    if ssid == 30:
        return smp["lc_band"] + [smp["trigger"], smp["rcr"]]
    if ssid == 31:
        return smp["bg_band"] + [smp["trigger"]]
    if ssid == 32:
        return smp["ebin"] + [smp["trigger"]]
    if ssid == 33:
        return [smp["variance"]]
    return [smp["status"], smp["z"], smp["y"]]


def fmt_sample(ssid, i, smp):
    if ssid == 30:
        lc = " ".join(f"{c:02x}" for c in smp["lc_band"])
        return (f"  sample {i}: lc_band[] = {lc} trigger {smp['trigger']:02x}"
                f" rcr {smp['rcr']}")
    if ssid == 31:
        bg = " ".join(f"{c:02x}" for c in smp["bg_band"])
        return f"  sample {i}: bg_band[] = {bg} trigger {smp['trigger']:02x}"
    if ssid == 32:
        sp = "".join(f"{c:02x}" for c in smp["ebin"])
        return (f"  sample {i}: subcol {smp['subcol']} ebin[0..31] = {sp}"
                f" trigger {smp['trigger']:02x} int_offset "
                f"{smp['int_offset']}")
    if ssid == 33:
        lo, hi = s0_decode(smp["variance"])
        return f"  value {i}: {smp['variance']:02x} (>= {lo}, < {hi})"
    return (f"  period {i}: status 0x{smp['status']:02x} z {smp['z']} "
            f"y {smp['y']}")


def report(products, show_samples, out):
    for ssid in sorted(products):
        for j, prod in enumerate(products[ssid]):
            segs = ",".join(SEG_NAMES[p["seg"]] for p in prod["packets"])
            nz = sum(1 for smp in prod["samples"]
                     for v in sample_values(ssid, smp) if v)
            print(f"SSID {ssid} ({SSID_NAMES[ssid]}) product {j}: "
                  f"{len(prod['packets'])} packet(s) [{segs}], "
                  f"{len(prod['samples'])} samples, "
                  f"{nz} nonzero value bytes; "
                  f"{fmt_header(prod['header'])}", file=out)
            if show_samples:
                for i, smp in enumerate(prod["samples"]):
                    print(fmt_sample(ssid, i, smp), file=out)


# --- the detector-smoke zero-flush check -------------------------------

ZERO_HEADERS = {
    30: {"int_time": 39, "det_mask_subcol": 0xFFFFFCFF, "pix_mask": 0x0FFF,
         "lc_skm": SKM_DEFAULT, "ta_skm": SKM_DEFAULT, "upper_edge": 0,
         "lower_edge_mask_ebin": 0x10821082, "n_bands": 5},
    31: {"int_time": 79, "bg_skm": SKM_DEFAULT, "upper_edge": 0,
         "lower_edge_mask_ebin": 0x10821082, "ta_skm": SKM_DEFAULT,
         "n_bands": 5},
    32: {"int_time": 319, "sp_skm": SKM_DEFAULT, "ta_skm": SKM_DEFAULT,
         "pix_mask": 0x0FFF},
    33: {"int_time": 39, "samples_per_value": 40,
         "det_mask_subcol": 0xFFFFFCFF, "energy_mask_ebin": 0x7FFE0000,
         "pix_mask": 0x0FFF, "skm": SKM_DEFAULT},
    34: {"delta_time_seconds": 39},
}
CADENCE_S = {30: 4, 31: 8, 33: 4, 34: 4}
SAMPLE_TOL = 2


def check_zero_flush(products, seconds):
    """The detector-smoke flush check; returns a list of failures."""
    fails = []
    for ssid in range(30, 35):
        prods = products.get(ssid, [])
        if len(prods) != 1:
            fails.append(f"SSID {ssid}: {len(prods)} products, want 1")
            continue
        prod = prods[0]
        for p in prod["packets"]:
            for k, want in ZERO_HEADERS[ssid].items():
                got = p["header"][k]
                if got != want:
                    fails.append(f"SSID {ssid} {p['where']}: {k} = "
                                 f"0x{got:X}, want 0x{want:X}")
        smp = prod["samples"]
        nz = [(i, v) for i, s in enumerate(smp)
              for v in sample_values(ssid, s) if v]
        if nz:
            fails.append(f"SSID {ssid}: {len(nz)} nonzero value bytes "
                         f"(first in sample {nz[0][0]}: 0x{nz[0][1]:02x})")
        if ssid == 32:
            subcols = sorted(s["subcol"] for s in smp)
            if subcols != list(range(NUM_SUBCOL)):
                fails.append(f"SSID 32: subcol bytes {subcols}, want "
                             f"0..31 once each (exactly one dump)")
            continue
        want = seconds / CADENCE_S[ssid]
        if abs(len(smp) - want) > SAMPLE_TOL:
            fails.append(f"SSID {ssid}: {len(smp)} samples, want "
                         f"{want:.1f} +-{SAMPLE_TOL} ({seconds} s / "
                         f"{CADENCE_S[ssid]} s)")
    return fails


# --- self-test ---------------------------------------------------------

def build_packet(ssid_body, seg=SEG_STANDALONE, seq=0):
    """A QL TM(21,6) CCSDS packet around @ssid_body (SSID byte first):
    APID 0x5DC, the 10-byte TM data field header, no CRC."""
    dfh = bytes([0x10, 21, 6, 0]) + bytes(6)   # PUS v1, svc, sub, dst, time
    body = dfh + ssid_body
    plen = len(body) - 1
    hdr = bytes([0x08 | (QL_APID >> 8), QL_APID & 0xFF,
                 (seg << 6) | (seq >> 8), seq & 0xFF,
                 plen >> 8, plen & 0xFF])
    return hdr + body


def be(v, n):
    return v.to_bytes(n, "big")


def ssid30_body(samples, det_mask=0xFFFFFCFF, int_time=39):
    """@samples: list of (lc_band[5], trigger, rcr)."""
    n = len(samples)
    b = bytes([30]) + be(0x1234, 4) + be(0x56, 2) + be(int_time, 2)
    b += be(det_mask, 4) + be(0x0FFF, 2) + bytes([0x2B, 0x2B << 1])
    b += be(0x10821082, 4) + bytes([5])
    for k in range(5):
        b += be(n, 2) + bytes(s[0][k] for s in samples)
    b += be(n, 2) + bytes(s[1] for s in samples)
    b += be(n, 2) + bytes(s[2] for s in samples)
    return b


def ssid31_body(n):
    b = bytes([31]) + be(1, 4) + be(0, 2) + be(79, 2) + bytes([0x2B << 1])
    b += be(0x10821082, 4) + bytes([0x2B, 5])
    for _ in range(5):
        b += be(n, 2) + bytes(n)
    return b + be(n, 2) + bytes(n)


def ssid32_body(spectra):
    """@spectra: list of (subcol, [32 ebin bytes])."""
    b = bytes([32]) + be(1, 4) + be(0, 2) + be(319, 2) + bytes([0x2B, 0x2B])
    b += be(0x0FFF, 2) + be(len(spectra), 2)
    for subcol, ebins in spectra:
        b += bytes([subcol]) + bytes(ebins) + bytes([0, 7])
    return b


def ssid33_body(n):
    b = bytes([33]) + be(1, 4) + be(0, 2) + be(39, 2) + bytes([40])
    b += be(0xFFFFFCFF, 4) + be(0x7FFE0000, 4) + be(0x0FFF, 2)
    return b + bytes([0x2B]) + be(n, 2) + bytes(n)


def ssid34_body(n):
    return bytes([34]) + be(1, 4) + be(0, 2) + be(39, 2) + be(n, 2) + \
        bytes(3 * n)


def check_contiguous(packets):
    """The APID counter is shared by all QL SSIDs: consecutive dump lines
    must count up by one (mod 2^14), or a packet was lost."""
    for prev, p in zip(packets, packets[1:]):
        if p["seq"] != (prev["seq"] + 1) % SEQ_MOD:
            raise DecodeError(f"{p['where']}: seq {p['seq']} after seq "
                              f"{prev['seq']} ({prev['where']}): a gap "
                              f"in the APID's sequence counts")


def decode_lines(lines):
    pkts = read_dump(lines)
    check_contiguous(pkts)
    return reassemble(pkts)


def must_fail(lines, what, needle):
    try:
        decode_lines(lines)
    except DecodeError as e:
        assert needle in str(e), f"{what}: wrong error: {e}"
        return
    raise AssertionError(f"{what}: decoded, want a DecodeError")


def self_test():
    # S0 vectors (plan "Compression"; CompressionAlgorithms.c)
    for v, c in ((0, 0), (15, 0x0F), (16, 0x10), (100, 0x24),
                 (1000, 0x3F), (2160, 0x48), (10000, 0x59),
                 (65535, 0x6F)):
        assert s0_compress(v) == c, f"S0({v}) = {s0_compress(v):#x}"
        lo, hi = s0_decode(c)
        assert lo <= v < hi, f"decode {c:#x}: {lo}..{hi} vs {v}"
    assert s0_decode(0x48) == (2048, 2304)
    prev = 0
    for v in range(0, 70000, 7):  # monotone
        c = s0_compress(v)
        assert c >= prev
        prev = c

    # the worked example: 1 count in every LACC cell for one QL period;
    # band 0 = ebin 1-6 x 12 pix x 30 subcol (no CFL, no BKG) = 2160
    band0 = 6 * 12 * (NUM_SUBCOL - 2)
    assert band0 == 2160
    lc = [s0_compress(band0), s0_compress(5 * 12 * 30), 0, 0, 0]
    assert lc[0] == 0x48
    pkt = build_packet(ssid30_body([(lc, 0x10, 0)]))
    assert pkt[SSID_OFF] == 30 and pkt[SSID_OFF + 22:SSID_OFF + 24] == \
        b"\x00\x01" and pkt[SSID_OFF + 24] == 0x48
    prods = decode_lines([pkt.hex()])
    smp = prods[30][0]["samples"]
    assert len(smp) == 1 and smp[0]["lc_band"][0] == 0x48
    assert s0_decode(smp[0]["lc_band"][0])[0] == 2048
    h = prods[30][0]["header"]
    assert h["det_mask_subcol"] == 0xFFFFFCFF and h["pix_mask"] == 0x0FFF
    assert h["lower_edge_mask_ebin"] == 0x10821082 and h["n_bands"] == 5

    # SSID 30-34 parsing, one standalone packet each (a zero sky)
    secs = 140
    n4, n8 = secs // 4, secs // 8
    zero_lines = [
        build_packet(ssid30_body([([0] * 5, 0, 0)] * n4)).hex(),
        build_packet(ssid31_body(n8), seq=1).hex(),
        build_packet(ssid32_body([(sc, [0] * 32)
                                  for sc in range(32)]), seq=2).hex(),
        build_packet(ssid33_body(n4), seq=3).hex(),
        build_packet(ssid34_body(n4), seq=4).hex(),
    ]
    prods = decode_lines(["# comment", ""] + zero_lines)
    assert sorted(prods) == [30, 31, 32, 33, 34]
    assert [len(prods[s][0]["samples"]) for s in range(30, 35)] == \
        [n4, n8, 32, n4, n4]
    assert prods[33][0]["header"]["samples_per_value"] == 40
    assert prods[31][0]["header"]["int_time"] == 79
    assert check_zero_flush(prods, secs) == []

    # SSID 32 sample layout: subcol, ebin 0..31, trigger, offset
    sp = [0] * 32
    sp[7] = s0_compress(100)
    prods = decode_lines([build_packet(ssid32_body([(14, sp)])).hex()])
    s32 = prods[32][0]["samples"][0]
    assert s32["subcol"] == 14 and s32["ebin"][7] == 0x24
    assert s32["trigger"] == 0 and s32["int_offset"] == 7

    # segment reassembly: FIRST, CONT, LAST (+ samples concatenated),
    # then FIRST + an empty termination LAST, interleaved with SSID 34
    a = [([1, 0, 0, 0, 0], 0, 0)] * 3
    seg_lines = [
        build_packet(ssid30_body(a), SEG_FIRST, 10).hex(),
        build_packet(ssid34_body(2), SEG_STANDALONE, 11).hex(),
        build_packet(ssid30_body(a[:2]), SEG_CONT, 12).hex(),
        build_packet(ssid30_body(a[:1]), SEG_LAST, 13).hex(),
        build_packet(ssid33_body(4), SEG_FIRST, 14).hex(),
        build_packet(ssid33_body(0), SEG_LAST, 15).hex(),
    ]
    prods = decode_lines(seg_lines)
    p30 = prods[30][0]
    assert len(p30["packets"]) == 3 and len(p30["samples"]) == 6
    assert not p30["termination"]
    p33 = prods[33][0]
    assert len(p33["samples"]) == 4 and p33["termination"]
    assert len(prods[34][0]["samples"]) == 2

    # known-bad inputs must fail
    good30 = build_packet(ssid30_body(a))
    must_fail([good30.hex() + "00"], "trailing byte", "CCSDS length")
    trunc = bytearray(good30[:-1])
    trunc[5] -= 1                       # consistent CCSDS length
    must_fail([trunc.hex()], "truncated RCR", "truncated")
    must_fail([build_packet(ssid30_body(a), SEG_CONT).hex()],
              "CONT without FIRST", "without a FIRST")
    must_fail([build_packet(ssid30_body(a), SEG_FIRST).hex()],
              "FIRST never closed", "never closed")
    must_fail([build_packet(ssid30_body(a), SEG_FIRST, 0).hex(),
               build_packet(ssid30_body(a), SEG_FIRST, 1).hex()],
              "FIRST twice", "still open")
    # sequence counts: a gap in the dump (a lost packet) fails, a wrap
    # 16383 -> 0 does not; within a group the counts must increase
    must_fail([build_packet(ssid30_body(a), SEG_FIRST, 7).hex(),
               build_packet(ssid30_body(a), SEG_LAST, 9).hex()],
              "seq gap", "a gap in the APID")
    prods = decode_lines([build_packet(ssid30_body(a), SEG_FIRST,
                                       SEQ_MOD - 1).hex(),
                          build_packet(ssid30_body(a), SEG_LAST, 0).hex()])
    assert len(prods[30][0]["samples"]) == 6
    back = [parse_packet(build_packet(ssid30_body(a), seg, q), f"p{q}")
            for seg, q in ((SEG_FIRST, 5), (SEG_LAST, 4))]
    try:
        reassemble(back)
    except DecodeError as e:
        assert "does not follow" in str(e), e
    else:
        raise AssertionError("a group counting backwards was accepted")
    bad_n = bytearray(good30)
    bad_n[SSID_OFF + 22 + 2 + 3 + 1] = 2   # band-1 N = 2, band 0 said 3
    must_fail([bad_n.hex()], "band N mismatch", "first block said")
    wrong_apid = bytearray(good30)
    wrong_apid[1] = 0xA7
    must_fail([wrong_apid.hex()], "wrong APID", "APID 0x5A7")
    must_fail(["zz"], "not hex", "not hex")
    # ... and the zero-flush check must catch content and count errors
    hot = list(zero_lines)
    hot[0] = build_packet(ssid30_body([([0x48, 0, 0, 0, 0], 0, 0)] +
                                      [([0] * 5, 0, 0)] * (n4 - 1))).hex()
    f = check_zero_flush(decode_lines(hot), secs)
    assert len(f) == 1 and "nonzero" in f[0], f
    f = check_zero_flush(decode_lines(zero_lines[1:]), secs)
    assert f == ["SSID 30: 0 products, want 1"], f
    f = check_zero_flush(decode_lines(zero_lines), secs + 40)
    assert len(f) == 4 and all("samples, want" in x for x in f), f
    dup32 = list(zero_lines)
    dup32[2] = build_packet(ssid32_body([(sc % 31, [0] * 32)
                                         for sc in range(32)]),
                            seq=2).hex()
    f = check_zero_flush(decode_lines(dup32), secs)
    assert len(f) == 1 and "once each" in f[0], f
    off = list(zero_lines)
    off[0] = build_packet(ssid30_body([([0] * 5, 0, 0)] * n4,
                                      det_mask=0xFFFFFFFF)).hex()
    f = check_zero_flush(decode_lines(off), secs)
    assert len(f) == 1 and "det_mask_subcol" in f[0], f

    print("QL-CHECK SELF-TEST: PASS")
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="Decode/check STIX QL TM(21,6) SSID 30-34 dumps")
    ap.add_argument("dump", nargs="?",
                    help="hex dump, one CCSDS packet per line")
    ap.add_argument("--samples", action="store_true",
                    help="print every sample's values")
    ap.add_argument("--expect-zero-flush", type=float, metavar="SECONDS",
                    help="check the NOMINAL-exit flush of a zero sky "
                         "after a NOMINAL window of SECONDS")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.dump:
        ap.error("a dump file is required")
    try:
        with open(args.dump) as f:
            lines = f.readlines()
    except OSError as e:
        print(f"QL-CHECK: ERROR (cannot read {args.dump}: {e})")
        return 2
    try:
        products = decode_lines(lines)
    except DecodeError as e:
        print(f"QL-CHECK: FAIL (decode: {e})")
        return 1
    report(products, args.samples, sys.stdout)
    if args.expect_zero_flush is None:
        return 0
    fails = check_zero_flush(products, args.expect_zero_flush)
    for x in fails:
        print(f"QL-CHECK MISMATCH: {x}")
    if fails:
        print(f"QL-CHECK: FAIL ({len(fails)} mismatches)")
        return 1
    print("QL-CHECK: PASS (zero-sky flush, all five SSIDs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
