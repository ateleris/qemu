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
                     [--expect-forced-abort URN ...]
                     [--expect-forced-complete URN ...]
                     [--expect-tiny URN ...] [--expect-epoch2 URN ...]
                     [--expect-collapse URN ...] [--expect-content-jump]
                     [--summing N] [--averaging A] [--samples]
  aspect-check.py --self-test
The modes are the D-ASh signatures of the aspect-smoke.sh legs (aspect
plan AS-5, docs/stix-port/2026-11-13-aspect-plan.md); every SSID 42
product must be claimed by exactly one mode. --expect-ideal is the
D-ASh row 1 shape (default URNs "1 0"; --summing/--averaging default
to 16). --expect-forced-abort is row 2 (--jitter): one product per
urn, a forced mid-series packet, the series ends early (start span
below ABORT_SPAN_MS). --expect-forced-complete is row 3
(--step-gap): exactly one forced mid-series packet and the full
archive delivered (span above ABORT_SPAN_MS). --expect-tiny is rows
4/7 epoch 1 (--drift, --step-1031): the corrupted-estimate packets -
summingFpga 15 and averagingTc 64 are pinned by the mechanism, every
packet carries at most 2 samples (the leading 0-sample forced packet
allowed, plan MINOR 3), and there are tens of them. --expect-epoch2
is row 7's second epoch (summing 16, averaging 64, one small
product). --expect-collapse is row 6 (--avg): the series ends early
and the TM values step down ~8x at the AvgSize change.
--expect-content-jump is row 5 (--drop, with --expect-ideal): the
time chain is continuous but the ramp's A0 values jump once. The
first live runs are [L]: the modes assert direction and rough
magnitude and print ASPECT-CHECK NOTE lines with the observed
numbers so the reviewer can pin them.
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

# the AS-5 leg signatures ([L] rough magnitudes, D-ASh rows 2-7): a
# completing product spans the ~250 s archive, an aborted one ends at
# the knob stamp (~150 s) minus the NOMINAL start (~72 s)
ABORT_SPAN_MS = 120000
# row 4/7: inc = 15 is deterministic (the truncated pair delta 1005..1007
# < 1008, Timing.c:395-399 + AspectCtrl.c:219-233), averaging 64 is the
# TC(238,1) binwidth; ~100 gap-forced packets of 0/1 samples
TINY_SUMMING, TINY_AVERAGING = 15, 64
TINY_MIN_PACKETS, TINY_MAX_PACKETS = 30, 130
TINY_MAX_SAMPLES = 2
# row 7 second epoch: all records post-step and consistent (inc = 16)
EPOCH2_SUMMING, EPOCH2_AVERAGING = 16, 64
# --avg (Amendment B2): the estimate from file 1 (inc 16) applied to the
# Avg2 epoch 2 (NOMINAL again @340): packet 0 is epoch 1's open packet,
# then one gap-forced 0/1-sample packet per Avg2 record
STALE_SUMMING, STALE_AVERAGING = 16, 64
STALE_FIRST_MIN = 100           # epoch 1's open packet (~235 samples)
STALE_MIN_FORCED = 50           # of the ~100 the budget allows
STALE_EPOCH2_MS = 330000        # epoch 2 starts at the @340 NOMINAL
# row 5/6 payload events on diode A0 (minimal-signed diffs mod 2**16):
# the ramp advances 16 or 256 per sample; a 2-block content loss is
# ~+3072 (folded), the Avg16->Avg2 change drops the values ~8x
JUMP_MIN, JUMP_MAX = 1024, 8192
COLLAPSE_STEP, COLLAPSE_MAX_POST = 2048, 128


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


# --- the AS-5 leg signatures (D-ASh rows 2-7) ---------------------------
#
# All [L]-loose: direction and rough magnitude, with NOTE lines carrying
# the observed numbers. Shared helpers first.

def one_product_each(products, urns, what):
    """Exactly one product per requested urn. Returns (fails, products
    by urn)."""
    fails = []
    by_urn = {}
    for prod in products:
        urn = prod["packets"][0]["header"]["user_request_number"]
        by_urn.setdefault(urn, []).append(prod)
    for urn in urns:
        n = len(by_urn.get(urn, []))
        if n != 1:
            fails.append(f"{what}: user_request_number {urn}: {n} "
                         f"product(s), want 1")
    return fails, by_urn


def signed_diffs(series):
    """Consecutive differences folded into [-32768, 32768): the ramp
    lives mod 2**16 and the folding keeps one big step one big step."""
    return [((b - a + 32768) % 65536) - 32768
            for a, b in zip(series, series[1:])]


def a0_series(prod):
    """The product's diode A0 values, packet by packet."""
    return [s[0] for p in prod["packets"] for s in p["samples"]]


def span_ms(prod):
    hs = prod["packets"][0]["header"], prod["packets"][-1]["header"]
    return (scet_to_ms(hs[1]["coarse"], hs[1]["fine"]) -
            scet_to_ms(hs[0]["coarse"], hs[0]["fine"]))


def start_deltas(prod):
    hs = [p["header"] for p in prod["packets"]]
    return [scet_to_ms(h["coarse"], h["fine"]) -
            scet_to_ms(g["coarse"], g["fine"])
            for g, h in zip(hs, hs[1:])]


def check_headers(prod, urn, summing, averaging, what):
    fails = []
    for k, p in enumerate(prod["packets"]):
        h = p["header"]
        if h["summingFpga"] != summing:
            fails.append(f"{what} urn {urn} packet {k}: summingFpga "
                         f"{h['summingFpga']}, want {summing}")
        if h["averagingTc"] != averaging:
            fails.append(f"{what} urn {urn} packet {k}: averagingTc "
                         f"{h['averagingTc']}, want {averaging}")
    return fails


def check_sgf_ends(prod, urn, what):
    pkts = prod["packets"]
    fails = []
    if pkts[0]["seg"] != SEG_FIRST:
        fails.append(f"{what} urn {urn}: first packet SGF "
                     f"{SEG_NAMES[pkts[0]['seg']]}, want FIRST")
    if pkts[-1]["seg"] != SEG_LAST:
        fails.append(f"{what} urn {urn}: last packet SGF "
                     f"{SEG_NAMES[pkts[-1]['seg']]}, want LAST")
    return fails


def note(what, urn, text):
    print(f"ASPECT-CHECK NOTE: {what} urn {urn}: {text}")


def check_forced_abort(products, urns, summing, averaging):
    """Row 2 (--jitter): the request hits a gap with budget 1 left
    (forced packet) and a second one with none left (forced final +
    FSW_ERROR): one short-lived product per urn with a forced mid-series
    packet."""
    what = "forced-abort"
    fails, by_urn = one_product_each(products, urns, what)
    for urn in urns:
        if urn not in by_urn:
            continue
        prod = by_urn[urn][0]
        pkts = prod["packets"]
        if len(pkts) < 2:
            fails.append(f"{what} urn {urn}: {len(pkts)} packet(s), "
                         f"want a forced packet and the error's final one")
            continue
        fails += check_sgf_ends(prod, urn, what)
        fails += check_headers(prod, urn, summing, averaging, what)
        shorts = [k for k, p in enumerate(pkts[:-1])
                  if len(p["samples"]) < MAX_SAMPLES]
        if not shorts:
            fails.append(f"{what} urn {urn}: no forced mid-series packet "
                         f"(all non-final packets full)")
        span = span_ms(prod)
        if span >= ABORT_SPAN_MS:
            fails.append(f"{what} urn {urn}: the product spans {span} ms, "
                         f"want an early end (< {ABORT_SPAN_MS})")
        note(what, urn, f"{len(pkts)} packet(s) with "
             f"{[len(p['samples']) for p in pkts]} sample(s), start "
             f"deltas {start_deltas(prod)} ms, span {span} ms")
    return fails


def check_forced_complete(products, urns, summing, averaging):
    """Row 3 (--step-gap): one gap event consumes the budget, the
    request completes: the full archive with exactly one forced
    mid-series packet."""
    what = "forced-complete"
    fails, by_urn = one_product_each(products, urns, what)
    for urn in urns:
        if urn not in by_urn:
            continue
        prod = by_urn[urn][0]
        pkts = prod["packets"]
        if len(pkts) < 2:
            fails.append(f"{what} urn {urn}: {len(pkts)} packet(s), want "
                         f"the forced one and the rest of the archive")
            continue
        fails += check_sgf_ends(prod, urn, what)
        fails += check_headers(prod, urn, summing, averaging, what)
        shorts = [k for k, p in enumerate(pkts[:-1])
                  if len(p["samples"]) < MAX_SAMPLES]
        if len(shorts) != 1:
            fails.append(f"{what} urn {urn}: {len(shorts)} forced "
                         f"mid-series packet(s), want exactly 1")
        if len(pkts[-1]["samples"]) >= MAX_SAMPLES:
            fails.append(f"{what} urn {urn}: the last packet is full, "
                         f"want a short one")
        span = span_ms(prod)
        if span <= ABORT_SPAN_MS:
            fails.append(f"{what} urn {urn}: the product spans {span} ms, "
                         f"want the whole archive (> {ABORT_SPAN_MS})")
        note(what, urn, f"{len(pkts)} packet(s) with "
             f"{[len(p['samples']) for p in pkts]} sample(s), start "
             f"deltas {start_deltas(prod)} ms, span {span} ms")
    return fails


def check_tiny(products, urns):
    """Rows 4/7 epoch 1 (--drift, --step-1031): the corrupted estimate
    inc = 15 gaps at every block; with averaging 64 and the budget of
    100 that is ~100 forced packets of 0/1 samples, then FSW_ERROR. The
    leading 0-sample forced packet is the plan's MINOR 3."""
    what = "tiny"
    fails, by_urn = one_product_each(products, urns, what)
    for urn in urns:
        if urn not in by_urn:
            continue
        prod = by_urn[urn][0]
        pkts = prod["packets"]
        if not TINY_MIN_PACKETS <= len(pkts) <= TINY_MAX_PACKETS:
            fails.append(f"{what} urn {urn}: {len(pkts)} packet(s), want "
                         f"the ~100 gap-forced ones "
                         f"({TINY_MIN_PACKETS}..{TINY_MAX_PACKETS})")
        big = [k for k, p in enumerate(pkts)
               if len(p["samples"]) > TINY_MAX_SAMPLES]
        if big:
            fails.append(f"{what} urn {urn}: packet(s) {big} carry more "
                         f"than {TINY_MAX_SAMPLES} sample(s)")
        fails += check_headers(prod, urn, TINY_SUMMING, TINY_AVERAGING,
                               what)
        counts = [len(p["samples"]) for p in pkts]
        note(what, urn, f"{len(pkts)} packet(s): "
             f"{counts.count(0)} x 0-sample, "
             f"{counts.count(1)} x 1-sample, max {max(counts) if counts else 0}")
    return fails


def scet_ms_of(pkt):
    h = pkt["header"]
    return scet_to_ms(h["coarse"], h["fine"])


def check_stale(products, urns):
    """Amendment B2 (--avg): one request over two epochs, Avg16 then
    Avg2. The once-per-request estimate (file 1's first pair: inc 16)
    is stale for every Avg2 record (126 ms vs 1008 ms): packet 0 is
    epoch 1's open packet, forced at the epoch boundary; then a gap at
    every epoch-2 record forces 0/1-sample packets until the budget
    (100) is gone -> FSW_ERROR (0x5440, asserted by the probe)."""
    what = "stale"
    fails, by_urn = one_product_each(products, urns, what)
    for urn in urns:
        if urn not in by_urn:
            continue
        prod = by_urn[urn][0]
        pkts = prod["packets"]
        if len(pkts) < 1 + STALE_MIN_FORCED:
            fails.append(f"{what} urn {urn}: {len(pkts)} packet(s), want "
                         f"epoch 1's packet + >= {STALE_MIN_FORCED} "
                         f"gap-forced ones")
            continue
        first = pkts[0]
        if len(first["samples"]) <= STALE_FIRST_MIN:
            fails.append(f"{what} urn {urn}: packet 0 has "
                         f"{len(first['samples'])} sample(s), want epoch "
                         f"1's open packet (> {STALE_FIRST_MIN})")
        if scet_ms_of(first) >= STALE_EPOCH2_MS:
            fails.append(f"{what} urn {urn}: packet 0 starts at "
                         f"{scet_ms_of(first)} ms, want epoch 1 "
                         f"(< {STALE_EPOCH2_MS})")
        rest = pkts[1:]
        big = [k + 1 for k, p in enumerate(rest)
               if len(p["samples"]) > TINY_MAX_SAMPLES]
        if big:
            fails.append(f"{what} urn {urn}: forced packet(s) {big[:5]} "
                         f"carry more than {TINY_MAX_SAMPLES} sample(s)")
        early = [k + 1 for k, p in enumerate(rest)
                 if scet_ms_of(p) < STALE_EPOCH2_MS]
        if early:
            fails.append(f"{what} urn {urn}: forced packet(s) {early[:5]} "
                         f"start before epoch 2 ({STALE_EPOCH2_MS} ms)")
        fails += check_headers(prod, urn, STALE_SUMMING, STALE_AVERAGING,
                               what)
        counts = [len(p["samples"]) for p in rest]
        note(what, urn, f"packet 0: {len(first['samples'])} samples at "
             f"{scet_ms_of(first)} ms; {len(rest)} forced packet(s) from "
             f"{scet_ms_of(rest[0])} ms: {counts.count(0)} x 0-sample, "
             f"{counts.count(1)} x 1-sample")
    return fails


def stale_packets(urn, seq=0, first=235, forced=99, t0=76000,
                  t2=341000, step=126):
    """One --avg product: epoch 1's open packet, then @forced 1-sample
    packets @step ms apart from @t2 (summing 16, averaging 64)."""
    counts = [first] + [1] * forced
    pkts = []
    for k, n in enumerate(counts):
        t = t0 if k == 0 else t2 + (k - 1) * step
        samples = [[(i + k) & 0xFFFF, 0, 0, 0] for i in range(n)]
        seg = SEG_FIRST if k == 0 else (
            SEG_LAST if k == len(counts) - 1 else SEG_CONT)
        c, f = ms_to_scet(t)
        pkts.append(build_packet(ssid42_body(
            c, f, urn, STALE_AVERAGING, STALE_SUMMING, samples), seg, seq))
        seq = (seq + 1) % SEQ_MOD
    return pkts


def check_epoch2(products, urns):
    """Row 7's second epoch: the discriminator. All records post-step
    and consistent: inc = 16, one small product."""
    what = "epoch2"
    fails, by_urn = one_product_each(products, urns, what)
    for urn in urns:
        if urn not in by_urn:
            continue
        prod = by_urn[urn][0]
        pkts = prod["packets"]
        if not 1 <= len(pkts) <= 2:
            fails.append(f"{what} urn {urn}: {len(pkts)} packet(s), "
                         f"want 1 (or 2)")
        total = sum(len(p["samples"]) for p in pkts)
        if not 2 <= total <= MAX_SAMPLES:
            fails.append(f"{what} urn {urn}: {total} sample(s), want the "
                         f"small second epoch (2..{MAX_SAMPLES})")
        for k, p in enumerate(pkts):
            if len(p["samples"]) > MAX_SAMPLES:
                fails.append(f"{what} urn {urn}: packet {k} over "
                             f"{MAX_SAMPLES} samples")
        fails += check_headers(prod, urn, EPOCH2_SUMMING, EPOCH2_AVERAGING,
                               what)
        note(what, urn, f"{len(pkts)} packet(s) with "
             f"{[len(p['samples']) for p in pkts]} sample(s), span "
             f"{span_ms(prod)} ms")
    return fails


def check_collapse(products, urns, summing, averaging):
    """Row 6 (--avg): the once-per-request estimate stays 16, so every
    post-change block gaps; budget 1 aborts at the second one. The
    values step down ~8x (N=2 sums divided by the still-16 averaging)."""
    what = "collapse"
    fails, by_urn = one_product_each(products, urns, what)
    for urn in urns:
        if urn not in by_urn:
            continue
        prod = by_urn[urn][0]
        pkts = prod["packets"]
        if len(pkts) < 2:
            fails.append(f"{what} urn {urn}: {len(pkts)} packet(s), want "
                         f"the pre-change data and the error's final one")
            continue
        fails += check_sgf_ends(prod, urn, what)
        fails += check_headers(prod, urn, summing, averaging, what)
        if len(pkts[-1]["samples"]) >= MAX_SAMPLES:
            fails.append(f"{what} urn {urn}: the last packet is full, "
                         f"want a short one")
        span = span_ms(prod)
        if span >= ABORT_SPAN_MS:
            fails.append(f"{what} urn {urn}: the product spans {span} ms, "
                         f"want an early end (< {ABORT_SPAN_MS})")
        series = a0_series(prod)
        dds = signed_diffs(series)
        runs = []
        i = 0
        while i < len(dds):
            if dds[i] <= -JUMP_MIN:
                j = i
                while j < len(dds) and dds[j] <= -JUMP_MIN:
                    j += 1
                runs.append((i, j))
                i = j
            else:
                i += 1
        if len(runs) != 1 or runs[0][1] - runs[0][0] > 2:
            fails.append(f"{what} urn {urn}: {len(runs)} value step(s) "
                         f"down, want exactly 1 (spanning <= 2 samples)")
        else:
            lo, hi = runs[0]
            drop = sum(dds[lo:hi])
            if drop > -COLLAPSE_STEP:
                fails.append(f"{what} urn {urn}: the value step is {drop}, "
                             f"want <= {-COLLAPSE_STEP} (the ~8x "
                             f"N=2/averaging-16 collapse)")
            late = [d for d in dds[hi:] if abs(d) > COLLAPSE_MAX_POST]
            if late:
                fails.append(f"{what} urn {urn}: after the step the sample "
                             f"rhythm jumps {late}, want |d| <= "
                             f"{COLLAPSE_MAX_POST}")
            if lo < 10:
                fails.append(f"{what} urn {urn}: only {lo} sample(s) "
                             f"before the step, want >= 10")
            note(what, urn, f"step {dds[lo:hi]} at sample {lo}, "
                 f"{len(series)} sample(s), span {span} ms")
    return fails


def check_content_jump(products):
    """Row 5 (--drop): no time gap at all (the archive's rewrite chain),
    the request completes - the only signature is the ramp content
    jumping once (two lost blocks, folded into +-3072 on A0)."""
    what = "content-jump"
    fails = []
    for prod in products:
        urn = prod["packets"][0]["header"]["user_request_number"]
        series = a0_series(prod)
        dds = signed_diffs(series)
        runs = []
        i = 0
        while i < len(dds):
            if abs(dds[i]) >= JUMP_MIN:
                j = i
                while j < len(dds) and abs(dds[j]) >= JUMP_MIN:
                    j += 1
                runs.append((i, j))
                i = j
            else:
                i += 1
        if len(runs) != 1 or runs[0][1] - runs[0][0] > 2:
            fails.append(f"{what} urn {urn}: {len(runs)} content jump(s), "
                         f"want exactly 1 (spanning <= 2 samples)")
            continue
        lo, hi = runs[0]
        jump = sum(dds[lo:hi])
        if not JUMP_MIN <= abs(jump) <= JUMP_MAX:
            fails.append(f"{what} urn {urn}: the content jump is {jump}, "
                         f"want a 2-block loss (|{JUMP_MIN}..{JUMP_MAX}|)")
        note(what, urn, f"jump {dds[lo:hi]} at sample {lo}, "
             f"{len(series)} sample(s)")
    return fails


def check_claimed(products, claimed):
    """Every SSID 42 product must be claimed by exactly one mode."""
    fails = []
    for prod in products:
        urn = prod["packets"][0]["header"]["user_request_number"]
        if urn not in claimed:
            fails.append(f"user_request_number {urn}: unclaimed product "
                         f"(no --expect-* mode asked for it)")
    return fails


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


def burst_strided(urn, t0_ms, counts, seq=0, stride=None, summing=16,
                  averaging=16):
    """burst() with a per-packet start stride (the gap-forced packets
    follow the raw record times, not a full packet)."""
    stride = stride if stride is not None \
        else MAX_SAMPLES * averaging * summing
    pkts = []
    t = t0_ms
    for k, n in enumerate(counts):
        samples = [[(i + k) & 0xFFFF, 0, 0, 0] for i in range(n)]
        seg = SEG_STANDALONE if len(counts) == 1 else (
            SEG_FIRST if k == 0 else
            SEG_LAST if k == len(counts) - 1 else SEG_CONT)
        c, f = ms_to_scet(t)
        pkts.append(build_packet(
            ssid42_body(c, f, urn, averaging, summing, samples), seg, seq))
        seq = (seq + 1) % SEQ_MOD
        t += stride
    return pkts


def a0_ramp_packets(urn, series, splits, seq=0, summing=16, averaging=16,
                    stride=None):
    """One product whose A0 follows @series (split into packets of
    @splits sample counts; the other diodes 0), packet starts advancing
    @stride (default: a full packet)."""
    stride = stride if stride is not None \
        else MAX_SAMPLES * averaging * summing
    pkts = []
    t = 0
    pos = 0
    for k, n in enumerate(splits):
        samples = []
        for i in range(n):
            v = series[pos + i] if pos + i < len(series) else 0
            samples.append([v, 0, 0, 0])
        pos += n
        seg = SEG_STANDALONE if len(splits) == 1 else (
            SEG_FIRST if k == 0 else
            SEG_LAST if k == len(splits) - 1 else SEG_CONT)
        c, f = ms_to_scet(t)
        pkts.append(build_packet(
            ssid42_body(c, f, urn, averaging, summing, samples), seg, seq))
        seq = (seq + 1) % SEQ_MOD
        t += stride
    return pkts


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

    # --- the AS-5 leg signatures (D-ASh rows 2-7) -----------------------

    # row 2, forced-abort: FIRST(15) + LAST(0) one block apart
    good = burst_strided(1, 0, [15, 0], seq=0, stride=1008)
    prods, _ = decode_lines(lines_of(good))
    assert check_forced_abort(prods, [1], 16, 16) == [], \
        check_forced_abort(prods, [1], 16, 16)
    # ... a missing urn, a full-packet shape and a too-long span fail
    f = check_forced_abort([], [1], 16, 16)
    assert f == ["forced-abort: user_request_number 1: 0 product(s), "
                 "want 1"], f
    prods, _ = decode_lines(lines_of(burst(1, 0, [510, 3], seq=0)))
    f = check_forced_abort(prods, [1], 16, 16)
    assert len(f) == 2 and "no forced mid-series packet" in f[0] and \
        "spans" in f[1], f
    prods, _ = decode_lines(lines_of(
        burst_strided(1, 0, [15, 0], seq=0, stride=130560)))
    f = check_forced_abort(prods, [1], 16, 16)
    # 130560 ms reads back as 130559: ms -> SCET floors the 1/65536 s
    # fine count and Timing_ToMs floors again (the live runs show it too)
    assert len(f) == 1 and "the product spans 130559 ms" in f[0], f

    # row 3, forced-complete: one forced packet, the archive delivered
    good = burst(1, 0, [15, 510, 130], seq=0)
    prods, _ = decode_lines(lines_of(good))
    assert check_forced_complete(prods, [1], 16, 16) == [], \
        check_forced_complete(prods, [1], 16, 16)
    # ... two forced packets fail, an aborted span fails
    prods, _ = decode_lines(lines_of(burst(1, 0, [15, 510, 17, 130],
                                           seq=0)))
    f = check_forced_complete(prods, [1], 16, 16)
    assert len(f) == 1 and "want exactly 1" in f[0], f
    prods, _ = decode_lines(lines_of(
        burst_strided(1, 0, [15, 3], seq=0, stride=1008)))
    f = check_forced_complete(prods, [1], 16, 16)
    assert any("spans 1007 ms" in x for x in f), f

    # rows 4/7, tiny: ~100 forced packets of 0/1 samples, summing 15
    good = burst_strided(1, 0, [0] + [1] * 99, seq=0, stride=1008,
                         summing=15, averaging=64)
    prods, _ = decode_lines(lines_of(good))
    assert check_tiny(prods, [1]) == [], check_tiny(prods, [1])
    # ... too few packets, a 3-sample packet and a wrong estimate fail
    prods, _ = decode_lines(lines_of(
        burst_strided(1, 0, [0] + [1] * 9, seq=0, stride=1008,
                      summing=15, averaging=64)))
    f = check_tiny(prods, [1])
    assert len(f) == 1 and "10 packet(s), want" in f[0], f
    prods, _ = decode_lines(lines_of(
        burst_strided(1, 0, [0] + [1] * 39 + [3] + [1] * 59, seq=0,
                      stride=1008, summing=15, averaging=64)))
    f = check_tiny(prods, [1])
    assert len(f) == 1 and "carry more than 2 sample(s)" in f[0], f
    prods, _ = decode_lines(lines_of(
        burst_strided(1, 0, [0] + [1] * 99, seq=0, stride=1008,
                      summing=16, averaging=64)))
    f = check_tiny(prods, [1])
    assert len(f) == 100 and all("summingFpga 16, want 15" in x
                                 for x in f), f[0]

    # row 7 epoch 2: one small consistent product, summing 16
    good = burst_strided(2, 0, [62], seq=0, summing=16, averaging=64)
    prods, _ = decode_lines(lines_of(good))
    assert check_epoch2(prods, [2]) == [], check_epoch2(prods, [2])
    prods, _ = decode_lines(lines_of(
        burst_strided(2, 0, [62], seq=0, summing=15, averaging=64)))
    f = check_epoch2(prods, [2])
    assert len(f) == 1 and "summingFpga 15, want 16" in f[0], f
    f = check_epoch2([], [2])
    assert f == ["epoch2: user_request_number 2: 0 product(s), want 1"], f

    # row 6, collapse: the ~8x value step down, then a small rhythm
    pre = [16 * (300 + i) for i in range(60)]           # ~16*s rhythm
    post = [2 * (360 + i) for i in range(12)]           # ~2*s: /8
    series = pre + post
    good = a0_ramp_packets(1, series, [36, 36, 0], seq=0, stride=1008)
    prods, _ = decode_lines(lines_of(good))
    f = check_collapse(prods, [1], 16, 16)
    assert f == [], f
    # ... no step: the collapse must fail
    flat = [16 * (900 + i) for i in range(72)]
    prods, _ = decode_lines(lines_of(
        a0_ramp_packets(1, flat, [36, 36, 0], seq=0, stride=1008)))
    f = check_collapse(prods, [1], 16, 16)
    assert len(f) == 1 and "want exactly 1" in f[0], f
    # ... a step without the post-step rhythm (a one-off blip) fails
    blip = pre + [pre[-1] // 8] + [16 * (900 + i) for i in range(11)]
    prods, _ = decode_lines(lines_of(
        a0_ramp_packets(1, blip, [36, 35, 1], seq=0, stride=1008)))
    f = check_collapse(prods, [1], 16, 16)
    assert any("rhythm jumps" in x for x in f), f

    # row 5, content-jump: one ~+3072 step in an otherwise smooth A0
    smooth = [(64 * (i // 63) + i % 63) * 16 for i in range(600)]
    jumped = smooth[:300] + [v + 3072 for v in smooth[300:]]
    good = a0_ramp_packets(1, jumped, [510, 90], seq=0)
    prods, _ = decode_lines(lines_of(good))
    assert check_content_jump(prods) == [], check_content_jump(prods)
    # ... a smooth series has no jump; two jumps fail
    prods, _ = decode_lines(lines_of(a0_ramp_packets(1, smooth,
                                                     [510, 90], seq=0)))
    f = check_content_jump(prods)
    assert len(f) == 1 and "0 content jump(s), want exactly 1" in f[0], f
    twice = smooth[:300] + [v + 3072 for v in smooth[300:450]] + \
        [v + 6144 for v in smooth[450:]]
    prods, _ = decode_lines(lines_of(a0_ramp_packets(1, twice, [510, 90],
                                                     seq=0)))
    f = check_content_jump(prods)
    assert len(f) == 1 and "2 content jump(s)" in f[0], f

    # Amendment B2, stale: epoch 1's packet + 99 forced Avg2 packets
    prods, _ = decode_lines(lines_of(stale_packets(1)))
    assert check_stale(prods, [1]) == [], check_stale(prods, [1])
    # ... too few forced packets, a small packet 0, a forced packet in
    # epoch 1 and a wrong averaging each fail
    prods, _ = decode_lines(lines_of(stale_packets(1, forced=20)))
    f = check_stale(prods, [1])
    assert len(f) == 1 and "21 packet(s)" in f[0], f
    prods, _ = decode_lines(lines_of(stale_packets(1, first=40)))
    f = check_stale(prods, [1])
    assert len(f) == 1 and "packet 0 has 40 sample(s)" in f[0], f
    prods, _ = decode_lines(lines_of(stale_packets(1, t2=300000)))
    f = check_stale(prods, [1])
    assert len(f) == 1 and "start before epoch 2" in f[0], f
    tiny_hdr = burst_strided(1, 0, [235] + [1] * 60, seq=0, stride=126,
                             summing=16, averaging=16)
    prods, _ = decode_lines(lines_of(tiny_hdr))
    f = check_stale(prods, [1])
    assert any("averagingTc 16, want 64" in x for x in f), f

    # the modes claim their urns: an unclaimed product fails
    # (the second burst continues the APID's sequence count)
    prods, _ = decode_lines(lines_of(good) + lines_of(
        burst(7, 9000, [510, 3], seq=len(good))))
    f = check_claimed(prods, {1: "content-jump"})
    assert len(f) == 1 and "unclaimed product" in f[0], f

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
    ap.add_argument("--expect-forced-abort", type=int, nargs="+",
                    metavar="URN",
                    help="D-ASh row 2 (--jitter): one product per urn, a "
                         "forced mid-series packet, the series ends early")
    ap.add_argument("--expect-forced-complete", type=int, nargs="+",
                    metavar="URN",
                    help="D-ASh row 3 (--step-gap): exactly one forced "
                         "mid-series packet, the archive fully delivered")
    ap.add_argument("--expect-stale", type=int, nargs="+", metavar="URN",
                    help="Amendment B2 (--avg): epoch 1's packet, then "
                         ">= 50 gap-forced 0/1-sample packets in the Avg2 "
                         "epoch 2, summing 16, averaging 64")
    ap.add_argument("--expect-tiny", type=int, nargs="+", metavar="URN",
                    help="D-ASh rows 4/7 epoch 1 (--drift, --step-1031): "
                         "the corrupted-estimate packets, summing 15 and "
                         "averaging 64, <= 2 samples each, tens of them")
    ap.add_argument("--expect-epoch2", type=int, nargs="+", metavar="URN",
                    help="D-ASh row 7's second epoch (--step-1031): one "
                         "small consistent product, summing 16, "
                         "averaging 64")
    ap.add_argument("--expect-collapse", type=int, nargs="+",
                    metavar="URN",
                    help="D-ASh row 6 (--avg): the series ends early and "
                         "the values step down ~8x at the AvgSize change")
    ap.add_argument("--expect-content-jump", action="store_true",
                    help="D-ASh row 5 (--drop, with --expect-ideal): the "
                         "ramp's A0 values jump once, times continuous")
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
    if args.expect_content_jump and args.expect_ideal is None:
        ap.error("--expect-content-jump claims no product: give it with "
                 "--expect-ideal")
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
    claimed = {}
    if args.expect_ideal is not None:
        fails += check_ideal(products, urns, args.summing, args.averaging)
        passed.append(f"ideal: one product per urn {urns}, "
                      f"{MAX_SAMPLES}-sample packets, "
                      f"summing {args.summing}, averaging {args.averaging}")
        for u in urns:
            claimed[u] = "ideal"
    if args.expect_none:
        fails += check_none(products)
        passed.append("none: no SSID 42 packet")
    if args.expect_forced_abort is not None:
        fails += check_forced_abort(products, args.expect_forced_abort,
                                    args.summing, args.averaging)
        passed.append(f"forced-abort: urns {args.expect_forced_abort} "
                      f"end early with a forced packet")
        for u in args.expect_forced_abort:
            claimed[u] = "forced-abort"
    if args.expect_forced_complete is not None:
        fails += check_forced_complete(products,
                                       args.expect_forced_complete,
                                       args.summing, args.averaging)
        passed.append(f"forced-complete: urns "
                      f"{args.expect_forced_complete} deliver the "
                      f"archive with one forced packet")
        for u in args.expect_forced_complete:
            claimed[u] = "forced-complete"
    if args.expect_tiny is not None:
        fails += check_tiny(products, args.expect_tiny)
        passed.append(f"tiny: urns {args.expect_tiny} gap-forced "
                      f"0/1-sample packets, summing {TINY_SUMMING}, "
                      f"averaging {TINY_AVERAGING}")
        for u in args.expect_tiny:
            claimed[u] = "tiny"
    if args.expect_stale is not None:
        fails += check_stale(products, args.expect_stale)
        passed.append(f"stale: urns {args.expect_stale} epoch 1's packet "
                      f"then gap-forced Avg2 packets, summing "
                      f"{STALE_SUMMING}, averaging {STALE_AVERAGING}")
        for u in args.expect_stale:
            claimed[u] = "stale"
    if args.expect_epoch2 is not None:
        fails += check_epoch2(products, args.expect_epoch2)
        passed.append(f"epoch2: urn {args.expect_epoch2} one small "
                      f"consistent product, summing {EPOCH2_SUMMING}, "
                      f"averaging {EPOCH2_AVERAGING}")
        for u in args.expect_epoch2:
            claimed[u] = "epoch2"
    if args.expect_collapse is not None:
        fails += check_collapse(products, args.expect_collapse,
                                args.summing, args.averaging)
        passed.append(f"collapse: urns {args.expect_collapse} end early "
                      f"with the ~8x value step")
        for u in args.expect_collapse:
            claimed[u] = "collapse"
    if args.expect_content_jump:
        fails += check_content_jump(products)
        passed.append("content-jump: one ramp jump, times continuous")
    if not passed:
        return 0
    fails += check_claimed(products, claimed)
    for x in fails:
        print(f"ASPECT-CHECK MISMATCH: {x}")
    if fails:
        print(f"ASPECT-CHECK: FAIL ({len(fails)} mismatches)")
        return 1
    print(f"ASPECT-CHECK: PASS ({'; '.join(passed)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())