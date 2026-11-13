#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""TC(238,1)/TC(237,13) builders for the L4 request replay (GH #1061,
L4 plan D-L4a, docs/stix-port/2026-11-13-l4-replay-plan.md). Standard
library only.

request    build one TC(238,1) "Select data for download" unit
           (ZIX38001): the replay's request A (whole collection) and
           B (empty window after it)
flare      build the TC(237,13) "Flare processing parameters" unit
           (ZIX37013): fills the L4 spectrogram pixel mask
self-test  run the built-in tests (no files needed)

TC(238,1) application data, big-endian (TM_238.c:22-58, TCData.h:
407-420, IDB ZIX38001 stix_idb_cmd_5ac.json:8617-8703): req u32,
level u8, start s u32, subsec u16, detector mask u32, n boxes u8,
then per box 10 B: tmin u24, tmax u24, tunit u16, packed energy u16
= emin(5) | emax(5) | energy-unit-1(5) in bits 14..10, 9..5, 4..0
(bit 15 spare).  The two energy fields are off-by-1 on the wire
(SciReport.c:567-573): wire emax is the last INCLUSIVE level (the
FSW does e_max++), wire energy-unit-1 is the levels per bin MINUS 1
(e_binwidth++).  The capture request (gh1061
tcsend_238_gh1061_cnatural.txt) sends PIX00200 0, PIX00201 31,
PIX00202 1 -> emin 0, 32 exclusive levels, bins of 2 -> energy word
0x03E1, the energyBinning the bundle's TM(21,6) captures decode
(21-B samples).  Reading PIX00202 as a plain ediv builds 0x3E0 and
is wrong (plan review MAJOR, resolved 2026-11-13).

TC(237,13) application data, 39 B big-endian (TCData.h:369-387,
TcService237.c:323-392): min/max accu duration u16/u16, max counts
u32, sumEmask u32, sumDmask u32, sel period u8, sel latency u8, sel
enabled in bit 7 of byte 18, sel start/stop u32, imaging det mask
u32, trim N/F u8, then byte 33 = spare(2) | imaging-bckg (bit 5) |
spectro-bckg (bit 4) | pixel mask[11:8], byte 34 = pixel mask[7:0],
spectro det mask u32.  The FSW stores GET16(byte 33) & 0xFFF as
E_PARAM_ID_SPECTRO_PIXE_MASK (TcService237.c:383-384); L4Report.c:201
copies it into every TM(21,6) structure at request time.

`flare` defaults are the gh1061 collect line (collect_L4_synthetic.tcl
ZIX37013) with the pixel mask raised from 0 to 0xFFF (TEST_PLAN §8
caveat: the capture's 0 left every energy byte zero; 0xFFF needs no
re-collection).  The self-test vectors 611/711 are the capture's
requests, their bytes derived from TM_238.c:22-58 plus the IDB field
list; the seqs are the captures' TM(21,6) PSCs (0xC201, 0xC204).

Units: SpW header + CCSDS + PUS + CRC-16/CCITT-FALSE, built by the
platform builder (stixplat/tcbuild.py under scripts/stix/platform/).
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "platform"))
from stixplat import tcbuild  # noqa: E402

SPW_HDR = tcbuild.SPW_HDR
APID_TC_WORD = 0x1DAC        # CCSDS word 0, APID 0x5AC (tcbuild.APID_TC)
BOX_SIZE = 10                # TCDATA_238_1_BOX_SIZE (TCData.h:414)
FLARE_APP_SIZE = 39          # TC(237,13) app data bytes


def _range(name, value, bits):
    hi = (1 << bits) - 1
    if not 0 <= value <= hi:
        raise ValueError("%s %d outside 0..%d" % (name, value, hi))
    return value


def energy_word(emin, emax, eunit1):
    """The box's packed energy u16 (TM_238.c:51-58): emin in bits
    14..10, emax in 9..5, energy-unit-1 in 4..0, bit 15 spare."""
    _range("emin", emin, 5)
    _range("emax", emax, 5)
    _range("energy-unit-1", eunit1, 5)
    return (emin << 10) | (emax << 5) | eunit1


def box_bytes(tmin, tmax, tunit, emin, emax, eunit1):
    """One 10-byte box (TCData.h:414-420) with the FSW's own checks
    (TM_238.c:69-75, FID 48435) on the wire values."""
    _range("tmin", tmin, 24)
    _range("tmax", tmax, 24)
    _range("tunit", tunit, 16)
    if tmin >= tmax:
        raise ValueError("tmin %d >= tmax %d (TM_238.c:69-75)"
                         % (tmin, tmax))
    if tunit == 0:
        raise ValueError("tunit 0 (TM_238.c:69-75)")
    if emin > emax:
        raise ValueError("emin %d > emax %d (TM_238.c:69-75)"
                         % (emin, emax))
    return (struct.pack(">I", tmin)[1:] + struct.pack(">I", tmax)[1:]
            + struct.pack(">H", tunit)
            + struct.pack(">H", energy_word(emin, emax, eunit1)))


def request_app(req, level, start, subsec, det_mask, boxes):
    """The TC(238,1) application data, TM_238.c:22-58 field order."""
    _range("req", req, 32)
    _range("level", level, 8)
    _range("start", start, 32)
    _range("subsec", subsec, 16)
    _range("detector mask", det_mask, 32)
    if not boxes:
        raise ValueError("no --box: a request needs at least one box")
    if len(boxes) > 255:
        raise ValueError("%d boxes: the wire u8 takes at most 255"
                         % len(boxes))
    app = struct.pack(">IBIHI", req, level, start, subsec, det_mask)
    app += bytes((len(boxes),))
    for box in boxes:
        app += box_bytes(*box)
    return app


def request_unit(req, level, start, subsec, det_mask, boxes, seq):
    """One TC(238,1) SpW unit: header + CCSDS + PUS + app + CRC."""
    app = request_app(req, level, start, subsec, det_mask, boxes)
    return SPW_HDR + tcbuild.tc_packet(tcbuild.APID_TC, seq, 238, 1, app)


def flare_app(min_dur, max_dur, max_counts, sum_emask, sum_dmask,
              sel_period, sel_latency, sel_enabled, sel_start,
              sel_stop, image_det_mask, trim_n, trim_f, image_bckg,
              spectro_bckg, pixel_mask, spectro_det_mask):
    """The 39-byte TC(237,13) application data (TCData.h:369-387,
    TcService237.c:323-392)."""
    _range("min dur", min_dur, 16)
    _range("max dur", max_dur, 16)
    _range("max counts", max_counts, 32)
    _range("sumEmask", sum_emask, 32)
    _range("sumDmask", sum_dmask, 32)
    _range("sel period", sel_period, 8)
    _range("sel latency", sel_latency, 8)
    _range("sel enabled", sel_enabled, 1)
    _range("sel start", sel_start, 32)
    _range("sel stop", sel_stop, 32)
    _range("imaging det mask", image_det_mask, 32)
    _range("trim N", trim_n, 8)
    _range("trim F", trim_f, 8)
    _range("imaging bckg", image_bckg, 1)
    _range("spectro bckg", spectro_bckg, 1)
    _range("pixel mask", pixel_mask, 12)
    _range("spectro det mask", spectro_det_mask, 32)
    app = struct.pack(">HHIII", min_dur, max_dur, max_counts,
                      sum_emask, sum_dmask)
    app += bytes((sel_period, sel_latency, (sel_enabled & 1) << 7))
    app += struct.pack(">III", sel_start, sel_stop, image_det_mask)
    app += bytes((trim_n, trim_f,
                  (image_bckg << 5) | (spectro_bckg << 4)
                  | (pixel_mask >> 8), pixel_mask & 0xFF))
    app += struct.pack(">I", spectro_det_mask)
    return app


def flare_unit(min_dur, max_dur, max_counts, sum_emask, sum_dmask,
               sel_period, sel_latency, sel_enabled, sel_start,
               sel_stop, image_det_mask, trim_n, trim_f, image_bckg,
               spectro_bckg, pixel_mask, spectro_det_mask, seq):
    """One TC(237,13) SpW unit."""
    app = flare_app(min_dur, max_dur, max_counts, sum_emask, sum_dmask,
                    sel_period, sel_latency, sel_enabled, sel_start,
                    sel_stop, image_det_mask, trim_n, trim_f,
                    image_bckg, spectro_bckg, pixel_mask,
                    spectro_det_mask)
    return SPW_HDR + tcbuild.tc_packet(tcbuild.APID_TC, seq, 237, 13, app)


def unit_hex(unit):
    return " ".join("%02X" % b for b in unit)


def fmt_t(t):
    s = "%.2f" % t
    return s.rstrip("0").rstrip(".")


def ack_data(seq):
    """The TM(1,x) data prefix for TC seq: the TC's first header words."""
    return "%04X%04X" % (APID_TC_WORD, 0xC000 | seq)


def probe_args(unit, seq, t0, ack_window, done_window):
    """The tmtc-probe.py arguments to send one unit and collect both
    acks (the elut-upload.py encode pattern)."""
    a = ack_data(seq)
    return ["--send-at", fmt_t(t0), "--send-hex", unit_hex(unit),
            "--expect", "0x5A1:1:1:%s@%s" % (a, fmt_t(ack_window)),
            "--expect", "0x5A1:1:7:%s@%s" % (a, fmt_t(done_window))]


# ------------------------------------------------------------ self-test

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

    def crc_nibbles(data, crc=0xFFFF):
        # test-only CRC-16/CCITT-FALSE, nibble-wise: a second,
        # independent implementation, so the trailer checks below do
        # not lean on tcbuild.crc16 alone; t1 pins both to absolute
        # gate bytes.
        for byte in data:
            for nib in (byte >> 4, byte & 0x0F):
                crc ^= nib << 12
                for _ in range(4):
                    crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 \
                        else crc << 1
                    crc &= 0xFFFF
        return crc

    # 1. the shared platform frames: the four precomputed nominal-smoke
    #    units rebuild byte for byte, CRCs from nominal-smoke.sh
    def t1():
        want = {
            (1, 236, 1, "0400"):
                "40 02 00 00 1D AC C0 01 00 07 19 EC 01 00 04 00 4A C1",
            (2, 236, 6, "07040000"):
                "40 02 00 00 1D AC C0 02 00 09 19 EC 06 00 07 04 00 00"
                " AF BA",
            (3, 236, 1, "0500"):
                "40 02 00 00 1D AC C0 03 00 07 19 EC 01 00 05 00 BF 97",
            (4, 17, 1, ""):
                "40 02 00 00 1D AC C0 04 00 05 19 11 01 00 16 B8",
        }
        for (seq, svc, sub, app), hexs in want.items():
            unit = SPW_HDR + tcbuild.tc_packet(tcbuild.APID_TC, seq, svc,
                                               sub, bytes.fromhex(app))
            got = unit_hex(unit)
            check("1 frame TC(%d,%d) seq %d" % (svc, sub, seq),
                  got == hexs, got)
            check("1 crc nibble-wise TC(%d,%d) seq %d" % (svc, sub, seq),
                  crc_nibbles(unit[4:-2])
                  == ((unit[-2] << 8) | unit[-1]))
    run("1 frames", t1)

    # 2. the energy-word trap (plan review MAJOR)
    def t2():
        check("2 wire emin 0, emax 31, eunit1 1 -> 0x03E1",
              energy_word(0, 31, 1) == 0x03E1,
              "0x%04X" % energy_word(0, 31, 1))
        check("2 FSW energyBinning of the incremented values agrees "
              "(L4Report.c:187-189)",
              ((0 << 10) | (31 << 5) | 1) == 0x03E1)
        check("2 the plain-ediv misreading (wire 0) builds 0x3E0, not "
              "the capture's 0x03E1",
              ((0 << 10) | (31 << 5) | 0) == 0x3E0
              and energy_word(0, 31, 1) != 0x3E0)
    run("2 energy word", t2)

    # 3. vector 611: the capture's GREEN full-window request
    #    (tcsend_238_gh1061_cnatural.txt:16; the capture's TM(21,6)
    #    PSC 0xC201 pins seq 0x201)
    def t3():
        unit = request_unit(611, 4, 595147086, 0, 0xFFFFFFFF,
                            [(0, 90000, 1, 0, 31, 1)], 0x201)
        frame = "40020000" "1dac" "c201" "001f" "19ee" "0100"
        app = ("00000263" "04" "2379394e" "0000" "ffffffff" "01"
               "000000" "015f90" "0001" "03e1")
        check("3 611 frame", unit[:14].hex() == frame, unit[:14].hex())
        check("3 611 app data byte for byte", unit[14:-2].hex() == app,
              unit[14:-2].hex())
        check("3 611 CRC trailer",
              ((unit[-2] << 8) | unit[-1]) == crc_nibbles(unit[4:-2]),
              "%04X" % ((unit[-2] << 8) | unit[-1]))
        check("3 611 box energy word bytes 03 E1",
              unit[14 + 24:14 + 26] == b"\x03\xe1")
    run("3 vector 611", t3)

    # 4. vector 711: the RED request, same bytes except req 0x02C7 and
    #    the capture's PSC 0xC204 -> seq 0x204
    def t4():
        u611 = request_unit(611, 4, 595147086, 0, 0xFFFFFFFF,
                            [(0, 90000, 1, 0, 31, 1)], 0x201)
        u711 = request_unit(711, 4, 595147086, 0, 0xFFFFFFFF,
                            [(0, 90000, 1, 0, 31, 1)], 0x204)
        check("4 711 seq word C2 04", u711[6:8] == b"\xc2\x04",
              u711[6:8].hex())
        check("4 711 req 00 00 02 C7", u711[14:18] == b"\x00\x00\x02\xc7",
              u711[14:18].hex())
        check("4 711 app after the req = 611's", u711[18:-2] == u611[18:-2])
        check("4 711 CRC trailer",
              ((u711[-2] << 8) | u711[-1]) == crc_nibbles(u711[4:-2]))
    run("4 vector 711", t4)

    # 5. flare: the collect line's values, pixel mask 0xFFF (byte 33
    #    low nibble + byte 34), flags at bits 5/4 (TcService237.c:376,
    #    :380); the capture's mask 0 gives bytes 33..34 = 00 00
    def t5():
        app = flare_app(4, 49, 1, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 0,
                        0, 0, 0, 0, 0, 0, 0, 0xFFF, 0)
        want = ("00040031" "00000001" "ffffffff" "ffffffff" "00" "00"
                "00" "00000000" "00000000" "00000000" "00" "00" "0f"
                "ff" "00000000")
        check("5 flare app 39 bytes", len(app) == FLARE_APP_SIZE,
              str(len(app)))
        check("5 flare app byte for byte", app.hex() == want, app.hex())
        zero = flare_app(4, 49, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                         0, 0)
        check("5 pixel mask 0 (the capture line) -> bytes 33..34 zero",
              zero[33:35] == b"\x00\x00", zero[33:35].hex())
        flags = flare_app(4, 49, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                          0xFFF, 0)
        check("5 bckg flags in byte 33 bits 5/4", flags[33] == 0x3F,
              "%02X" % flags[33])
        unit = flare_unit(4, 49, 1, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 0,
                          0, 0, 0, 0, 0, 0, 0, 0xFFF, 0, 0x200)
        check("5 flare frame seq 0x200, length 0x2C",
              unit[:14].hex() == "40020000" "1dac" "c200" "002c"
              "19ed" "0d00", unit[:14].hex())
        check("5 flare CRC trailer",
              ((unit[-2] << 8) | unit[-1]) == crc_nibbles(unit[4:-2]))
    run("5 flare", t5)

    # 6. the FSW's own static checks are mirrored (TM_238.c:69-75)
    def t6():
        bad_boxes = [
            (90000, 0, 1, 0, 31, 1),               # tmin >= tmax
            (0, 90000, 0, 0, 31, 1),               # tunit 0
            (0, 90000, 1, 31, 0, 1),               # emin > emax
            (0x1000000, 0x1000001, 1, 0, 31, 1),   # beyond u24
            (0, 90000, 1, 32, 31, 1),              # emin beyond 5 bits
        ]
        for box in bad_boxes:
            try:
                request_app(611, 4, 595147086, 0, 0xFFFFFFFF, [box])
                check("6 box %r refused" % (box,), False, "accepted")
            except ValueError:
                check("6 box %r refused" % (box,), True)
        try:
            request_app(611, 4, 595147086, 0, 0xFFFFFFFF, [])
            check("6 empty request refused", False, "accepted")
        except ValueError:
            check("6 empty request refused", True)
        try:
            flare_app(4, 49, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                      0x1000, 0)
            check("6 pixel mask 0x1000 refused", False, "accepted")
        except ValueError:
            check("6 pixel mask 0x1000 refused", True)
        try:
            request_unit(611, 4, 595147086, 0, 0xFFFFFFFF,
                         [(0, 90000, 1, 0, 31, 1)], 0x4000)
            check("6 seq 0x4000 refused (14 bits)", False, "accepted")
        except ValueError:
            check("6 seq 0x4000 refused (14 bits)", True)
    run("6 static checks", t6)

    # 7. the probe-args emission and the ack prefix
    def t7():
        unit = request_unit(611, 4, 595147086, 0, 0xFFFFFFFF,
                            [(0, 90000, 1, 0, 31, 1)], 0x201)
        a = probe_args(unit, 0x201, 70, 5, 5)
        check("7 probe-args shape",
              a[:2] == ["--send-at", "70"] and a[2] == "--send-hex"
              and a[4:6] == ["--expect", "0x5A1:1:1:1DACC201@5"]
              and a[6:8] == ["--expect", "0x5A1:1:7:1DACC201@5"],
              str(a))
    run("7 probe-args", t7)

    if failures:
        print("SELF-TEST: FAIL (%d: %s)" % (len(failures),
                                            ", ".join(failures)))
        return 1
    print("SELF-TEST: PASS")
    return 0


# ----------------------------------------------------------------- main

def _int(s):
    return int(s, 0)


def parse_box(text):
    parts = text.split(",")
    if len(parts) != 6:
        raise ValueError("--box wants TMIN,TMAX,TUNIT,EMIN,EMAX,EUNIT1")
    return tuple(_int(p) for p in parts)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    req = sub.add_parser("request", help="build one TC(238,1) unit")
    req.add_argument("--req", type=_int, required=True,
                     help="unique request number (PIX00076)")
    req.add_argument("--level", type=_int, default=4,
                     help="data processing level (PIX00070; 4 = L4)")
    req.add_argument("--start", type=_int, required=True,
                     help="start time SCET coarse, s (PIX00072)")
    req.add_argument("--subsec", type=_int, default=0,
                     help="start time sub-seconds (PIX00073)")
    req.add_argument("--det-mask", type=_int, default=0xFFFFFFFF,
                     help="detector mask (PIX00248)")
    req.add_argument("--box", action="append", default=[],
                     metavar="TMIN,TMAX,TUNIT,EMIN,EMAX,EUNIT1",
                     help="one box (PIX00077..PIX00202), repeatable; "
                          "EMAX and EUNIT1 are off-by-1 on the wire, "
                          "see the module docstring")
    req.add_argument("--seq", type=_int, default=0x201,
                     help="TC sequence count, 14 bit (default 0x201 = "
                          "the capture's 611 PSC 0xC201)")
    req.add_argument("--t0", type=float, default=0.0)
    req.add_argument("--ack-window", type=float, default=5.0)
    req.add_argument("--done-window", type=float, default=5.0)
    req.add_argument("--emit", choices=("probe-args", "hex"),
                     default="probe-args")
    fl = sub.add_parser("flare", help="build one TC(237,13) unit")
    fl.add_argument("--min-dur", type=_int, default=4,
                    help="PIX00447, the collect line's value")
    fl.add_argument("--max-dur", type=_int, default=49,
                    help="PIX00448, the collect line's value")
    fl.add_argument("--max-counts", type=_int, default=1,
                    help="PIX00449, the collect line's value")
    fl.add_argument("--sum-emask", type=_int, default=0xFFFFFFFF,
                    help="PIX00450, the collect line's value")
    fl.add_argument("--sum-dmask", type=_int, default=0xFFFFFFFF,
                    help="PIX00451, the collect line's value")
    fl.add_argument("--sel-period", type=_int, default=0,
                    help="PIX00452, the collect line's value")
    fl.add_argument("--sel-latency", type=_int, default=0,
                    help="PIX00453, the collect line's value")
    fl.add_argument("--sel-enabled", type=_int, default=0,
                    help="PIX00454, the collect line's value")
    fl.add_argument("--sel-start", type=_int, default=0,
                    help="PIX00455, the collect line's value")
    fl.add_argument("--sel-stop", type=_int, default=0,
                    help="PIX00456, the collect line's value")
    fl.add_argument("--image-det-mask", type=_int, default=0,
                    help="PIX00457, the collect line's value")
    fl.add_argument("--trim-n", type=_int, default=0,
                    help="PIX00458, the collect line's value")
    fl.add_argument("--trim-f", type=_int, default=0,
                    help="PIX00459, the collect line's value")
    fl.add_argument("--image-bckg", type=_int, default=0,
                    help="PIX00460, the collect line's value")
    fl.add_argument("--spectro-bckg", type=_int, default=0,
                    help="PIX00461, the collect line's value")
    fl.add_argument("--pixel-mask", type=_int, default=0xFFF,
                    help="PIX00462 spectrogram pixel mask: 0xFFF per "
                         "the TEST_PLAN §8 caveat (the capture's 0 "
                         "left every energy byte zero)")
    fl.add_argument("--spectro-det-mask", type=_int, default=0,
                    help="PIX00463, the collect line's value")
    fl.add_argument("--seq", type=_int, default=0x200,
                    help="TC sequence count, 14 bit")
    fl.add_argument("--t0", type=float, default=0.0)
    fl.add_argument("--ack-window", type=float, default=5.0)
    fl.add_argument("--done-window", type=float, default=5.0)
    fl.add_argument("--emit", choices=("probe-args", "hex"),
                    default="probe-args")
    sub.add_parser("self-test", help="run the built-in tests")
    args = ap.parse_args(argv)

    if args.cmd == "self-test":
        return self_test()
    try:
        if args.cmd == "request":
            unit = request_unit(args.req, args.level, args.start,
                                args.subsec, args.det_mask,
                                [parse_box(b) for b in args.box],
                                args.seq)
        else:
            unit = flare_unit(args.min_dur, args.max_dur,
                              args.max_counts, args.sum_emask,
                              args.sum_dmask, args.sel_period,
                              args.sel_latency, args.sel_enabled,
                              args.sel_start, args.sel_stop,
                              args.image_det_mask, args.trim_n,
                              args.trim_f, args.image_bckg,
                              args.spectro_bckg, args.pixel_mask,
                              args.spectro_det_mask, args.seq)
    except ValueError as e:
        print("l4-tc: %s" % e, file=sys.stderr)
        return 2
    if args.emit == "hex":
        print(unit_hex(unit))
        return 0
    print("\n".join(probe_args(unit, args.seq, args.t0,
                               args.ack_window, args.done_window)))
    return 0


if __name__ == "__main__":
    sys.exit(main())