#!/usr/bin/env python3
"""Decode and check the STIX aspect-burst TM (TM(21,6) SSID 42).

Input: a dump file as tmtc-probe.py --dump-apid 0x5DC:FILE writes it:
one whole CCSDS packet per line, hex, WITHOUT the 4-byte SpW header,
so the SSID is byte 16. Blank lines and lines starting with '#' are
ignored.

Decode (aspect plan D-ASi/D-ASh; TM_21.c:647-687, TMData.h:593-596 and
:906-931): APID 0x5DC (PID_93 auxiliary science, category 12), PUS
TM(21,6), SSID 42 at CCSDS byte 16 (TMData_DATA_FIELD, TMData.h:56), no
CRC. After the SSID byte, big-endian: SCET coarse u32, SCET fine u16,
user_request_number u32, averagingTc u16, summingFpga u8,
numberOfDynamicStructures u16, then 8 B per aspect sample = diode A0,
A1, B0, B1 (u16 each). A packet carries at most 510 samples
(TMDATA_SOURCE_DATA_SIZE 4096 minus the 16-byte SSID42 overhead =
4080 B, TMData.h:548, :595-596). Segmentation flags in CCSDS byte 2
bits 7:6: 3 standalone, 1 FIRST, 0 CONTINUATION, 2 LAST; a burst answer
is one product = FIRST, CONT..., LAST (AspectCtrl.c:89, :300-303,
:450-457). Every 0x5DC packet shares one sequence counter with the QL
flush SSIDs (TMForm_createHeadersType_21_6_APID93, TM_21.c:780-799), so
the dump as a whole must count up by one (a gap is a lost packet); the
QL SSID 30-34 lines are only keyed here, not parsed (ql-check.py owns
them).

The two request paths aspect-smoke.sh fires (aspect plan D-ASi):
TC(238,1) processing level 5 (SciReport.c:586-602 ->
AspectCtrl_report, AspectCtrl.c:71) answers with user_request_number =
the TC's, while TC(238,10) (TcService238.c:190-202 ->
AspectCtrl_tcReportBurstData, AspectCtrl.c:495-510, processed by
AspectCtrl_doTask :519-534) always answers with user_request_number 0
(AspectCtrl.c:107 with :530). The ideal leg is D-ASh row 1: exactly one
product per path, summingFpga = the FSW's once-per-request block
estimate (floor(pair delta / 63) = 16 for the default AVGSIZE 4,
AspectCtrl.c:219-233), averagingTc = the TC's time_binwidth, every
non-final packet of exactly 510 samples with a short (or empty
termination) last one, SGF FIRST -> CONT -> ... -> LAST, and packet
start times 510 * averagingTc * summingFpga ms apart (each archived row
advances the time by the estimate, AspectCtrl.c:364, and a sample is
emitted every averagingTc rows, :367-372). The start times are
Timing_MsToSCET of the accumulated estimate (AspectCtrl.c:409-411), so
the whole-ms view has a +-1 ms truncation slack per packet
(Timing_ToMs, Timing.c:395-399; Timing_MsToSCET, :401-410): +-3 ms on a
step. --expect-none is the negative leg (as-enable=false): no SSID 42
packet at all (the request answers EID 0x543F, SciReport.c:535-544, and
TM(1,8) FID_48435, AspectCtrl.c:531).

Usage:
  aspect-check.py DUMP [--expect-ideal [URN ...]] [--expect-none]
                     [--summing N] [--averaging A] [--samples]
  aspect-check.py --self-test
--expect-ideal defaults to URNs "1 0" (the TC(238,1) request number
then the TC(238,10) path); --summing/--averaging default to 16 (the
ideal leg's N and time_binwidth; the AS-5 legs pass their own).
Exit codes: 0 decoded (and the check passed), 1 a decode error or a
failed check, 2 bad usage or an unreadable file. stdlib only.
"""
import argparse
import io
import sys

ASP_APID = 0x5DC
SSID_OFF = 16          # TMData_DATA_FIELD (TMData.h:56)
SSID42 = 42            # TMData_SSID42 (TMData.h:593)
SAMPLE_SIZE = 8        # TMData_SSID42_MAX_SAMPLE_SIZE_UNIT (TMData.h:596)
MAX_SAMPLES = 510      # 4096 - 16 overhead = 4080 dynamic bytes (TMData.h:595)
SEG_CONT, SEG_FIRST, SEG_LAST, SEG_STANDALONE = 0, 1, 2, 3
SEG_NAMES = {SEG_CONT: "CONT", SEG_FIRST: "FIRST", SEG_LAST: "LAST",
             SEG_STANDALONE: "STANDALONE"}
SEQ_MOD = 1 << 14      # CCSDS source sequence count
STEP_TOL_MS = 3        # whole-ms truncation slack of two start times


class DecodeError(Exception):
    pass


# --- SCET <-> ms (Timing.c:395-399, :401-410) --------------------------

def scet_to_ms(coarse, fine):
    """Timing_ToMs: whole ms, truncating."""
    return coarse * 1000 + (fine * 1000) // 65536


def ms_to_scet(ms):
    """Timing_MsToSCET: exact for whole ms."""
    coarse = ms // 1000
    return coarse, (ms - coarse * 1000) * 65536 // 1000


# --- packet parsing -----------------------------------------------------

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


def parse_packet(pkt, where):
    """One CCSDS packet -> dict. SSID 42 is parsed fully; the other
    TM(21,6) SSIDs on the same APID (the QL flush 30-34) are only
    keyed (the shared sequence counter still covers them)."""
    if len(pkt) < SSID_OFF + 1:
        raise DecodeError(f"{where}: {len(pkt)} bytes, too short for an "
                          f"SSID at byte {SSID_OFF}")
    apid = ((pkt[0] << 8) | pkt[1]) & 0x7FF
    if apid != ASP_APID:
        raise DecodeError(f"{where}: APID 0x{apid:03X}, want 0x{ASP_APID:03X}")
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
    if ssid != SSID42:
        return {"ssid": ssid, "seg": seg, "seq": seq, "where": where}
    r = Reader(pkt[SSID_OFF + 1:], f"{where} SSID 42")
    header = {"coarse": r.u32("SCET coarse"), "fine": r.u16("SCET fine"),
              "user_request_number": r.u32("user_request_number"),
              "averagingTc": r.u16("averagingTc"),
              "summingFpga": r.u8("summingFpga"),
              "n": r.u16("numberOfDynamicStructures")}
    if header["n"] > MAX_SAMPLES:
        raise DecodeError(f"{where}: numberOfDynamicStructures "
                          f"{header['n']} > {MAX_SAMPLES}")
    samples = []
    for s in range(header["n"]):
        raw = r.take(SAMPLE_SIZE, f"sample {s}")
        samples.append([int.from_bytes(raw[2 * k:2 * k + 2], "big")
                        for k in range(4)])
    r.done()
    return {"ssid": ssid, "seg": seg, "seq": seq, "header": header,
            "samples": samples, "where": where}


def reassemble(packets):
    """The SSID 42 packets in dump order -> [product]; a product is
    FIRST, CONT..., LAST (or one STANDALONE). Returns (products,
    n_ql_lines)."""
    products = []
    open_ = None
    n_ql = 0
    for p in packets:
        if p["ssid"] != SSID42:
            n_ql += 1
            continue
        if p["seg"] in (SEG_STANDALONE, SEG_FIRST):
            if open_ is not None:
                raise DecodeError(f"{p['where']}: SSID 42 "
                                  f"{SEG_NAMES[p['seg']]} while a product "
                                  f"from {open_['packets'][0]['where']} is "
                                  f"still open")
            open_ = {"packets": [p]}
            if p["seg"] == SEG_STANDALONE:
                products.append(open_)
                open_ = None
            continue
        if open_ is None:
            raise DecodeError(f"{p['where']}: SSID 42 {SEG_NAMES[p['seg']]} "
                              f"without a FIRST")
        step = (p["seq"] - open_["packets"][-1]["seq"]) % SEQ_MOD
        if not 0 < step < SEQ_MOD // 2:
            raise DecodeError(f"{p['where']}: SSID 42 {SEG_NAMES[p['seg']]} "
                              f"seq {p['seq']} does not follow seq "
                              f"{open_['packets'][-1]['seq']} of its group")
        open_["packets"].append(p)
        if p["seg"] == SEG_LAST:
            products.append(open_)
            open_ = None
    if open_ is not None:
        raise DecodeError(f"SSID 42: product from "
                          f"{open_['packets'][0]['where']} never closed "
                          f"(no LAST)")
    return products, n_ql


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


def check_contiguous(packets):
    """All 0x5DC packets share one counter (TM_21.c:780-799): the dump
    must count up by one, or a packet was lost."""
    for prev, p in zip(packets, packets[1:]):
        if p["seq"] != (prev["seq"] + 1) % SEQ_MOD:
            raise DecodeError(f"{p['where']}: seq {p['seq']} after seq "
                              f"{prev['seq']} ({prev['where']}): a gap "
                              f"in the APID's sequence counts")


def decode_lines(lines):
    pkts = read_dump(lines)
    check_contiguous(pkts)
    products, n_ql = reassemble(pkts)
    return products, n_ql


# --- reporting ----------------------------------------------------------

def fmt_scet(h):
    return f"{scet_to_ms(h['coarse'], h['fine'])} ms"


def report(products, n_ql, show_samples, out):
    for j, prod in enumerate(products):
        urn = prod["packets"][0]["header"]["user_request_number"]
        segs = ",".join(SEG_NAMES[p["seg"]] for p in prod["packets"])
        counts = " ".join(str(len(p["samples"])) for p in prod["packets"])
        h0 = prod["packets"][0]["header"]
        h1 = prod["packets"][-1]["header"]
        print(f"SSID 42 product {j}: urn {urn}, "
              f"{len(prod['packets'])} packet(s) [{segs}], "
              f"{sum(len(p['samples']) for p in prod['packets'])} samples "
              f"({counts}), summing {h0['summingFpga']}, averaging "
              f"{h0['averagingTc']}, start {fmt_scet(h0)}..{fmt_scet(h1)}",
              file=out)
        if show_samples:
            for p in prod["packets"]:
                for i, smp in enumerate(p["samples"]):
                    print(f"  {p['where']} sample {i}: " +
                          " ".join(f"{v:04x}" for v in smp), file=out)
    print(f"{n_ql} QL-flush packet(s) (SSID 30-34) ignored", file=out)


# --- the D-ASh row 1 (ideal) check --------------------------------------

def check_ideal(products, urns, summing, averaging):
    """Signature 1: one product per request path, full packets of
    MAX_SAMPLES with a short last one, FIRST -> CONT -> ... -> LAST,
    the headers right, and the packet start times stepping a full
    packet. Returns a list of failures."""
    fails = []
    by_urn = {}
    for prod in products:
        urn = prod["packets"][0]["header"]["user_request_number"]
        by_urn.setdefault(urn, []).append(prod)
    for urn in urns:
        n = len(by_urn.get(urn, []))
        if n != 1:
            fails.append(f"user_request_number {urn}: {n} product(s), "
                         f"want 1")
    extra = [u for u in sorted(by_urn) if u not in urns]
    if extra:
        fails.append(f"unexpected user_request_number(s) {extra} "
                     f"({sum(len(by_urn[u]) for u in extra)} product(s))")
    for prod in products:
        urn = prod["packets"][0]["header"]["user_request_number"]
        pkts = prod["packets"]
        last = len(pkts) - 1
        if len(pkts) == 1:
            fails.append(f"urn {urn}: one {SEG_NAMES[pkts[0]['seg']]} "
                         f"packet, want FIRST..LAST")
            continue
        if pkts[0]["seg"] != SEG_FIRST:
            fails.append(f"urn {urn}: first packet SGF "
                         f"{SEG_NAMES[pkts[0]['seg']]}, want FIRST")
        if pkts[last]["seg"] != SEG_LAST:
            fails.append(f"urn {urn}: last packet SGF "
                         f"{SEG_NAMES[pkts[last]['seg']]}, want LAST")
        for k in range(1, last):
            if pkts[k]["seg"] != SEG_CONT:
                fails.append(f"urn {urn}: packet {k} SGF "
                             f"{SEG_NAMES[pkts[k]['seg']]}, want CONT")
        full = 0
        for k, p in enumerate(pkts):
            n = len(p["samples"])
            h = p["header"]
            if h["summingFpga"] != summing:
                fails.append(f"urn {urn} packet {k}: summingFpga "
                             f"{h['summingFpga']}, want {summing}")
            if h["averagingTc"] != averaging:
                fails.append(f"urn {urn} packet {k}: averagingTc "
                             f"{h['averagingTc']}, want {averaging}")
            if k == last:
                if n >= MAX_SAMPLES:
                    fails.append(f"urn {urn}: the last packet has {n} "
                                 f"samples, want a short one")
            else:
                if n != MAX_SAMPLES:
                    fails.append(f"urn {urn}: packet {k} of {len(pkts)} "
                                 f"has {n} samples, want {MAX_SAMPLES}")
                else:
                    full += 1
            if k:
                prev = pkts[k - 1]["header"]
                delta = (scet_to_ms(h["coarse"], h["fine"]) -
                         scet_to_ms(prev["coarse"], prev["fine"]))
                # the step follows the packets' own summingFpga claim, so
                # a wrong estimate fails once in the header check, not
                # twice
                want = MAX_SAMPLES * h["averagingTc"] * h["summingFpga"]
                if abs(delta - want) > STEP_TOL_MS:
                    fails.append(f"urn {urn}: packet {k} starts {delta} ms "
                                 f"after packet {k - 1}, want {want} "
                                 f"+-{STEP_TOL_MS} ms")
        if not full:
            fails.append(f"urn {urn}: no full {MAX_SAMPLES}-sample packet")
    return fails


def check_none(products):
    """The negative leg (as-enable=false): no SSID 42 packet at all."""
    if products:
        n = sum(len(p["packets"]) for p in products)
        return [f"{n} SSID 42 packet(s) in the dump, want 0"]
    return []


# --- self-test ----------------------------------------------------------

def be(v, n):
    return v.to_bytes(n, "big")


def build_packet(ssid_body, seg=SEG_STANDALONE, seq=0):
    """A TM(21,6) CCSDS packet around @ssid_body (SSID byte first):
    APID 0x5DC, the 10-byte TM data field header, no CRC."""
    dfh = bytes([0x10, 21, 6, 0]) + bytes(6)   # PUS v1, svc, sub, dst, time
    body = dfh + ssid_body
    plen = len(body) - 1
    hdr = bytes([0x08 | (ASP_APID >> 8), ASP_APID & 0xFF,
                 (seg << 6) | (seq >> 8), seq & 0xFF,
                 plen >> 8, plen & 0xFF])
    return hdr + body


def ssid42_body(coarse, fine, urn, averaging, summing, samples):
    """The SSID 42 payload of one packet (TM_21.c:663-678): the header
    fields and @samples of 4 diode u16 BE each."""
    b = bytes([SSID42]) + be(coarse, 4) + be(fine, 2) + be(urn, 4) + \
        be(averaging, 2) + bytes([summing]) + be(len(samples), 2)
    for smp in samples:
        for v in smp:
            b += be(v, 2)
    return b


def ql34_body(n=1):
    """A stand-in QL-flush line (SSID 34 shape): keyed, never parsed
    here; its body only has to match the CCSDS length field."""
    return bytes([34]) + be(1, 4) + be(0, 2) + be(39, 2) + be(n, 2) + \
        bytes(3 * n)


def burst(urn, t0_ms, counts, seq=0, summing=16, averaging=16, jump=None):
    """The packets of one burst product: @counts samples per packet,
    packet k starting t0 + k * 510 * averaging * summing ms (with @jump = (k, ms)
    the starts from packet k on shifted, the #1058-shaped start-time
    step). The samples follow a known ramp: sample i of packet k is
    (i + k, 2i, 3i, 4i) & 0xFFFF."""
    pkts = []
    t = t0_ms
    for k, n in enumerate(counts):
        samples = [[(i + k) & 0xFFFF, (2 * i) & 0xFFFF, (3 * i) & 0xFFFF,
                    (4 * i) & 0xFFFF] for i in range(n)]
        seg = SEG_STANDALONE if len(counts) == 1 else (
            SEG_FIRST if k == 0 else
            SEG_LAST if k == len(counts) - 1 else SEG_CONT)
        c, f = ms_to_scet(t)
        pkts.append(build_packet(
            ssid42_body(c, f, urn, averaging, summing, samples), seg, seq))
        seq = (seq + 1) % SEQ_MOD
        t += MAX_SAMPLES * averaging * summing
        if jump and k + 1 >= jump[0]:
            t += jump[1]
    return pkts


def lines_of(pkts):
    return [p.hex() for p in pkts]


def must_fail(lines, what, needle):
    try:
        decode_lines(lines)
    except DecodeError as e:
        assert needle in str(e), f"{what}: wrong error: {e}"
        return
    raise AssertionError(f"{what}: decoded, want a DecodeError")


def self_test():
    # SCET <-> ms: MsToSCET is exact, the way back can lose 1 ms
    c, f = ms_to_scet(128160)
    assert (c, f) == (128, 10485), (c, f)
    assert scet_to_ms(c, f) == 128159
    assert scet_to_ms(1, 65) == 1000 + 0        # 65 * 1000 // 65536 = 0
    assert scet_to_ms(0, 65535) == 999

    # one standalone packet: the fields and samples round-trip
    pkt = build_packet(ssid42_body(*ms_to_scet(120000), 1, 16, 16,
                                  [[0x123, 0x456, 0x789, 0xABC]]))
    assert pkt[SSID_OFF] == 42 and pkt[SSID_OFF + 1:SSID_OFF + 5] == be(120, 4)
    prods, n_ql = decode_lines([pkt.hex()])
    assert n_ql == 0 and len(prods) == 1
    p = prods[0]["packets"][0]
    h = p["header"]
    assert h == {"coarse": 120, "fine": 0, "user_request_number": 1,
                 "averagingTc": 16, "summingFpga": 16, "n": 1}, h
    assert p["samples"] == [[0x123, 0x456, 0x789, 0xABC]]

    # the ideal dump: two products (urn 1, urn 0) with QL lines between,
    # full 510-sample packets and short last ones, 130560 ms steps
    good = (lines_of([build_packet(ql34_body(2), seq=0)]) +
            lines_of(burst(1, 120000, [510, 510, 3], seq=1)) +
            lines_of([build_packet(ql34_body(1), seq=4)]) +
            lines_of(burst(0, 200000, [510, 2], seq=5)))
    prods, n_ql = decode_lines(good)
    assert n_ql == 2 and len(prods) == 2
    assert [p["packets"][0]["header"]["user_request_number"]
            for p in prods] == [1, 0]
    assert [len(p["samples"]) for p in prods[0]["packets"]] == [510, 510, 3]
    assert [len(p["samples"]) for p in prods[1]["packets"]] == [510, 2]
    assert check_ideal(prods, [1, 0], 16, 16) == []
    assert check_none(prods) != []
    # ... and a report line for each product
    buf = io.StringIO()
    report(prods, n_ql, False, buf)
    rep = buf.getvalue().splitlines()
    assert len(rep) == 3 and "urn 1" in rep[0] and "8160" not in rep[0], rep

    # the empty termination (a burst ending on a packet boundary):
    # FIRST 510 + empty LAST is a clean product
    prods, _ = decode_lines(lines_of(burst(1, 0, [510, 0], seq=0)))
    assert check_ideal(prods, [1], 16, 16) == [], \
        check_ideal(prods, [1], 16, 16)

    # each failure signature:
    # summingFpga off (the FSW estimated a different N): one header
    # mismatch per packet; the step check follows the packet's own claim
    prods, _ = decode_lines(lines_of(burst(1, 0, [510, 3], seq=0,
                                           summing=15)))
    f = check_ideal(prods, [1], 16, 16)
    assert len(f) == 2 and all("summingFpga 15, want 16" in x for x in f), f
    # averagingTc off (the TC binwidth not echoed)
    prods, _ = decode_lines(lines_of(burst(1, 0, [510, 3], seq=0,
                                           averaging=64)))
    f = check_ideal(prods, [1], 16, 16)
    assert len(f) == 2 and all("averagingTc 64, want 16" in x for x in f), f
    # a request path missing / an unexpected path
    prods, _ = decode_lines(lines_of(burst(1, 0, [510, 3], seq=0)))
    f = check_ideal(prods, [1, 0], 16, 16)
    assert f == ["user_request_number 0: 0 product(s), want 1"], f
    prods, _ = decode_lines(lines_of(burst(1, 0, [510, 3], seq=0)) +
                            lines_of(burst(7, 9000, [510, 3], seq=2)))
    f = check_ideal(prods, [1], 16, 16)
    assert len(f) == 1 and "unexpected user_request_number(s) [7]" in f[0], f
    # a mid-series short packet (the forced-send shape of D-ASh row 2)
    prods, _ = decode_lines(lines_of(burst(1, 0, [509, 510, 3], seq=0)))
    f = check_ideal(prods, [1], 16, 16)
    assert len(f) == 1 and "packet 0 of 3 has 509 samples" in f[0], f
    # no full packet at all
    prods, _ = decode_lines(lines_of(burst(1, 0, [510, 510], seq=0)))
    f = check_ideal(prods, [1], 16, 16)
    assert len(f) == 1 and "the last packet has 510 samples" in f[0], f
    # a start-time step (the #1058 signature): +40 ms from packet 1 on,
    # so the steps into packet 1 AND packet 2 both fail
    prods, _ = decode_lines(lines_of(burst(1, 0, [510, 510, 3], seq=0,
                                           jump=(1, 40))))
    f = check_ideal(prods, [1], 16, 16)
    assert len(f) == 2 and "after packet 0, want 130560" in f[0] and \
        "after packet 1, want 130560" in f[1], f
    # ... but the +-3 ms truncation slack passes
    prods, _ = decode_lines(lines_of(burst(1, 0, [510, 510, 3], seq=0,
                                           jump=(1, 2))))
    assert check_ideal(prods, [1], 16, 16) == []
    # a burst that fits one packet (wrong shape for the ideal leg)
    prods, _ = decode_lines(lines_of(burst(1, 0, [510], seq=0)))
    f = check_ideal(prods, [1], 16, 16)
    assert len(f) == 1 and "one STANDALONE packet" in f[0], f
    # SGF shapes
    must_fail([build_packet(ssid42_body(*ms_to_scet(0), 1, 16, 16, []),
                            SEG_CONT).hex()],
              "CONT without FIRST", "without a FIRST")
    must_fail([build_packet(ssid42_body(*ms_to_scet(0), 1, 16, 16, []),
                            SEG_FIRST, 5).hex(),
               build_packet(ssid42_body(*ms_to_scet(0), 1, 16, 16, []),
                            SEG_FIRST, 6).hex()],
              "FIRST twice", "still open")
    must_fail([build_packet(ssid42_body(*ms_to_scet(0), 1, 16, 16, []),
                            SEG_FIRST, 5).hex()],
              "FIRST never closed", "never closed")
    # a seq-count gap inside a product: a group counting backwards (the
    # whole-dump contiguity check would fire first, so reassemble is
    # exercised directly, as ql-check.py does)
    back = [parse_packet(burst(1, 0, [510, 3], seq=9)[0], "p9"),
            parse_packet(build_packet(
                ssid42_body(*ms_to_scet(8160), 1, 16, 16, []),
                SEG_CONT, 7), "p7")]
    try:
        reassemble(back)
    except DecodeError as e:
        assert "does not follow" in str(e), e
    else:
        raise AssertionError("a group counting backwards was accepted")
    # a truncated sample array (the CCSDS length still consistent)
    pkt = build_packet(ssid42_body(*ms_to_scet(0), 1, 16, 16,
                                   [[1, 2, 3, 4]] * 2))
    trunc = bytearray(pkt[:-2])
    trunc[5] -= 2
    must_fail([trunc.hex()], "truncated samples", "truncated")
    # too many samples for one packet
    must_fail([build_packet(
        ssid42_body(*ms_to_scet(0), 1, 16, 16,
                   [[0] * 4] * (MAX_SAMPLES + 1))).hex()],
        "511 samples in one packet", "numberOfDynamicStructures")
    # dump hygiene: wrong APID, a TC packet, not hex, a seq gap
    wrong = bytearray(build_packet(ssid42_body(*ms_to_scet(0), 1, 16, 16,
                                               [])))
    wrong[1] = 0xA7
    must_fail([wrong.hex()], "wrong APID", "APID 0x5A7")
    tc_pkt = bytearray(build_packet(ssid42_body(*ms_to_scet(0), 1, 16, 16,
                                                [])))
    tc_pkt[0] |= 0x10
    must_fail([tc_pkt.hex()], "TC type bit", "a TC packet")
    must_fail(["zz"], "not hex", "not hex")
    must_fail([build_packet(ql34_body(1), seq=3).hex(),
               build_packet(ql34_body(1), seq=5).hex()],
              "a lost QL packet breaks the counter", "a gap in the APID")

    # --expect-none: only QL lines pass, one SSID 42 packet fails
    prods, n_ql = decode_lines(lines_of([build_packet(ql34_body(1), seq=0)]))
    assert n_ql == 1 and check_none(prods) == []
    prods, _ = decode_lines(lines_of(burst(0, 0, [3], seq=0)))
    assert check_none(prods) == ["1 SSID 42 packet(s) in the dump, want 0"]

    print("ASPECT-CHECK SELF-TEST: PASS")
    return 0


def int_arg(s):
    """int() accepting 0x hex."""
    return int(s, 0)


def main():
    ap = argparse.ArgumentParser(
        description="Decode/check STIX aspect-burst TM(21,6) SSID 42 dumps")
    ap.add_argument("dump", nargs="?",
                    help="hex dump, one CCSDS packet per line")
    ap.add_argument("--expect-ideal", type=int, nargs="*", metavar="URN",
                    help="check the D-ASh row 1 signature: one product per "
                         "request path (default URNs: 1 0 = the TC(238,1) "
                         "request number and the TC(238,10) path)")
    ap.add_argument("--expect-none", action="store_true",
                    help="check the negative leg: no SSID 42 packet at all")
    ap.add_argument("--summing", type=int_arg, default=16, metavar="N",
                    help="the expected summingFpga (default 16)")
    ap.add_argument("--averaging", type=int_arg, default=16, metavar="A",
                    help="the expected averagingTc (default 16)")
    ap.add_argument("--samples", action="store_true",
                    help="print every sample's diode values")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if args.expect_ideal is not None and args.expect_none:
        ap.error("--expect-ideal and --expect-none are exclusive")
    if not args.dump:
        ap.error("a dump file is required")
    try:
        with open(args.dump) as f:
            lines = f.readlines()
    except OSError as e:
        print(f"ASPECT-CHECK: ERROR (cannot read {args.dump}: {e})")
        return 2
    try:
        products, n_ql = decode_lines(lines)
    except DecodeError as e:
        print(f"ASPECT-CHECK: FAIL (decode: {e})")
        return 1
    report(products, n_ql, args.samples, sys.stdout)
    urns = args.expect_ideal if args.expect_ideal else [1, 0]
    fails, passed = [], []
    if args.expect_ideal is not None:
        fails += check_ideal(products, urns, args.summing, args.averaging)
        passed.append(f"ideal: one product per urn {urns}, "
                      f"{MAX_SAMPLES}-sample packets, "
                      f"summing {args.summing}, averaging {args.averaging}")
    if args.expect_none:
        fails += check_none(products)
        passed.append("none: no SSID 42 packet")
    if not passed:
        return 0
    for x in fails:
        print(f"ASPECT-CHECK MISMATCH: {x}")
    if fails:
        print(f"ASPECT-CHECK: FAIL ({len(fails)} mismatches)")
        return 1
    print(f"ASPECT-CHECK: PASS ({'; '.join(passed)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())