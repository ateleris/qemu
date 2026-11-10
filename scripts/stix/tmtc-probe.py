#!/usr/bin/env python3
"""Probe the STIX model's TMTC TCP server for telemetry, and gate TC ingress.

Connects to the model's TMTC port immediately after QEMU start, sends the
12-byte "STIX_CONTROL" handshake token (stripped by the model per D-K),
then reads the TM stream.

Framing (D-K, since P2-6 both directions): a unit is
[4-byte SpW header][CCSDS Space Packet], back to back, with no length
prefix; the unit length is 4 + 7 + BE16(bytes 8:10) (the CCSDS packet
length field at SP[4:6]), and header byte 1 must be 0x02 — anything
else drops one byte (framing resync). TM arrives as one unit per
packet; TC leaves as one unit per packet.

TC ingress gate (--send-hex with --expect): raw TC units are sent on
the same connection at their --send-at offsets, and the framed TM
stream is matched against the expectations. An expectation keys on
APID and PUS type/subtype (SP[7], SP[8] — correction 5), plus an
optional prefix of the application data (SP[10:]); expectations attach
to the most recent --send-hex. The probe exits 0 only when every
expectation matched within --window seconds of its send.

Usage:
  tmtc-probe.py [--port PORT] [--seconds N]
      logs raw chunks (prints 'NO TM OBSERVED', exits 1 if none)
  tmtc-probe.py [--port P] [--seconds N] --window W
      [--send-at S] --send-hex HEX [--expect APID:TYPE:SUBTYPE[:DATAHEX]]...
      TC ingress gate
  tmtc-probe.py --self-test
      runs the framer self-test and exits 0/1
"""
import argparse
import os
import socket
import sys
import time

# D-K: the 4-byte SpW header plus the 6 CCSDS primary-header bytes
# before the length field; a unit is 11 + BE16(unit[8:10]) long and can
# never exceed 4 + 65542 bytes.
TC_HDR_LEN = 10
TC_UNIT_MAX = 4 + 65542


def frame_units(stream):
    """Split the byte stream on the CCSDS length; return whole units.

    Mutates @stream, leaving unconsumed bytes (a partial unit) in it.
    Drops one byte whenever header byte 1 is not 0x02 (D-K resync).
    Returns (units, resyncs).
    """
    units = []
    resyncs = 0
    while True:
        if len(stream) < TC_HDR_LEN:
            break
        if stream[1] != 0x02:
            del stream[0]
            resyncs += 1
            continue
        need = 11 + ((stream[8] << 8) | stream[9])
        if need > TC_UNIT_MAX:
            del stream[0]
            resyncs += 1
            continue
        if len(stream) < need:
            break
        units.append(bytes(stream[:need]))
        del stream[:need]
    return units, resyncs


def unit_fields(unit):
    """(apid, PUS type, PUS subtype, application data) of a unit, or
    None when it is too short for the PUS fields.

    The data-field header length follows the packet type bit (SP[0]
    bit 4): a TC carries the 4-byte header and its application data
    starts at SP[10]; a TM carries the 10-byte TM data field header
    (TMData.h:423) and its application data starts at TMData_DATA_FIELD
    = SP[16] (TMData.h:56, TM_1.c:29-30). The probe matches the model's
    TM stream, so expectations see the TM offset.
    """
    sp = unit[4:]
    if len(sp) < 9:
        return None
    apid = ((sp[0] << 8) | sp[1]) & 0x7FF
    off = 10 if sp[0] & 0x10 else 16
    return apid, sp[7], sp[8], sp[off:]


class Expectation:
    """Base for every expectation kind. Everything run_gate's verdict
    loop reads (window, ok, __str__) lives here, so a new kind cannot
    miss it (P2-7 review B1). An optional "@SECONDS" suffix tightens
    one expectation's window; the group's --window remains the
    default."""

    def __init__(self, spec):
        self.full = spec
        self.window = None
        body = spec
        if "@" in spec:
            body, w = spec.rsplit("@", 1)
            self.window = float(w)
        self._parse(body)
        self.ok = False

    def _parse(self, body):
        raise NotImplementedError

    def matches(self, unit):
        raise NotImplementedError

    def __str__(self):
        return self.full


class Expect(Expectation):
    def _parse(self, body):
        parts = body.split(":", 3)
        if len(parts) < 3:
            raise ValueError("expect needs APID:TYPE:SUBTYPE[:DATAHEX][@S]")
        self.apid = int(parts[0], 0)
        self.ptype = int(parts[1], 0)
        self.subtype = int(parts[2], 0)
        self.data = bytes.fromhex(parts[3]) if len(parts) > 3 else None

    def matches(self, unit):
        fields = unit_fields(unit)
        if fields is None:
            return False
        apid, ptype, subtype, app = fields
        if (apid, ptype, subtype) != (self.apid, self.ptype, self.subtype):
            return False
        return self.data is None or app[:len(self.data)] == self.data


class CucExpect(Expectation):
    """The SCET the FSW carries as CUC time in the PUS TM data field
    header: coarse = SP[10:14] big-endian (unit bytes 14:18 — the unit
    starts with the 4-byte SpW header), fine = SP[14:16]. Confirmed on
    real heartbeats, whose coarse ticks +1 per second. Matches a TM
    unit with @apid whose coarse hex starts with @prefix — '1000000'
    matches 0x10000000..0x1000000F, 'a few seconds after the set'."""

    def _parse(self, body):
        apid, prefix = body.split(":", 1)
        self.apid = int(apid, 0)
        self.prefix = prefix.lower()

    def matches(self, unit):
        fields = unit_fields(unit)
        if fields is None:
            return False
        if fields[0] != self.apid:
            return False
        return unit[14:18].hex().startswith(self.prefix)


class OpAction(argparse.Action):
    """Record --send-at/--send-hex/--expect in command-line order, so
    --send-at applies to the following --send-hex and --expect attaches
    to the most recent --send-hex."""

    def __call__(self, parser, namespace, values, option_string=None):
        ops = getattr(namespace, "ops", None) or []
        ops.append((option_string, values))
        namespace.ops = ops


def build_groups(args):
    """Group the ordered options into send groups (+ a connect-time
    group for expectations given before any --send-hex)."""
    groups = [{"at": None, "hex": None, "expects": []}]
    current_at = 20.0
    for op, val in args.ops:
        if op == "--send-at":
            current_at = float(val)
        elif op == "--send-hex":
            groups.append({"at": current_at, "hex": val,
                           "expects": [], "sent": False, "t_sent": None})
        elif op == "--expect":
            groups[-1]["expects"].append(Expect(val))
        elif op == "--expect-cuc":
            groups[-1]["expects"].append(CucExpect(val))
    return groups


def setup_mode(args):
    """Plain mode (no gate options) gets one connect-time group and
    gate=False; gate mode returns the send groups."""
    groups = build_groups(args)
    gate = any(g["hex"] is not None for g in groups) or \
        any(g["expects"] for g in groups)
    return groups, gate


def make_parser():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int,
                    default=20000 + os.getuid(),  # IO_TCP_SERVER_BASE_PORT + getuid()
                    help="TMTC server port (default: 20000+getuid)")
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--window", type=float, default=10,
                    help="per-send expectation window, seconds")
    ap.add_argument("--send-at", action=OpAction, metavar="SECONDS",
                    help="offset for the following --send-hex, seconds "
                         "(default 20)")
    ap.add_argument("--send-hex", action=OpAction, metavar="HEX",
                    help="a TC unit to send (repeatable)")
    ap.add_argument("--expect", action=OpAction,
                    metavar="APID:TYPE:SUBTYPE[:DATAHEX][@SECONDS]",
                    help="a TM expectation, keyed on APID and PUS "
                         "type/subtype plus an optional application-data "
                         "prefix and an optional per-expectation window "
                         "(repeatable)")
    ap.add_argument("--expect-cuc", action=OpAction,
                    metavar="APID:HEXPREFIX",
                    help="a TM expectation on the CUC coarse time in the "
                         "TM data field header (SP[10:14]), hex-prefix "
                         "match (repeatable)")
    ap.add_argument("--self-test", action="store_true",
                    help="run the framer self-test and exit")
    # B1 of the P2-6 review: ops must exist even when no gate option is
    # given (soak.sh and boot-smoke.sh run the plain mode).
    ap.set_defaults(ops=[])
    return ap


def ts():
    return time.strftime("%H:%M:%S")


def verdict(groups, group_window, now, t0):
    """The run_gate verdict computation: (all_matched, failed).

    Factored out so the self-test can run it over a mixed group (the
    P2-7 review B1): every attribute the loop reads — window, ok,
    __str__ — must exist on every expectation kind.
    """
    all_matched = True
    failed = []
    for g in groups:
        if g["hex"] is not None and not g["sent"]:
            all_matched = False
        # m2 of the P2-6 review: the connect-time group binds to t0
        # with its own window
        start = g["t_sent"] if g["hex"] is not None else t0
        ready = g["sent"] if g["hex"] is not None else True
        for e in g["expects"]:
            if not e.ok:
                all_matched = False
                win = e.window if e.window is not None else group_window
                if ready and now > start + win:
                    failed.append(e)
    return all_matched, failed


def run_gate(sock, args, groups):
    """The --send-hex/--expect mode: send at the offsets, match the
    framed TM stream. Returns 0 only when every expectation matched
    within --window of its send."""
    t0 = time.monotonic()
    pending = bytearray()
    total = 0
    sock.settimeout(0.5)
    while True:
        now = time.monotonic()
        for g in groups:
            if g["hex"] is not None and not g["sent"] and now - t0 >= g["at"]:
                payload = bytes.fromhex(g["hex"])
                sock.sendall(payload)
                g["sent"] = True
                g["t_sent"] = time.monotonic()
                print(f"{ts()} >> sent {len(payload)}B "
                      f"({g['hex'][:32]}...) at +{g['at']:.0f}s", flush=True)
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            chunk = b""
        except OSError:
            chunk = b""
        if chunk:
            total += len(chunk)
            pending += chunk
            print(f"{ts()} +{len(chunk):4d}B {chunk[:48].hex()}", flush=True)
            units, _ = frame_units(pending)
            for unit in units:
                fields = unit_fields(unit)
                if fields is None:
                    continue
                apid, ptype, subtype, _app = fields
                print(f"{ts()} TM apid=0x{apid:03X} type={ptype} "
                      f"subtype={subtype} len={len(unit)} "
                      f"data={unit[:48].hex()}", flush=True)
                for g in groups:
                    if g["hex"] is not None and not g["sent"]:
                        continue  # m3: its TC has not gone out yet
                    for e in g["expects"]:
                        if not e.ok and e.matches(unit):
                            e.ok = True
                            print(f"{ts()} EXPECT OK: {e} "
                                  f"(unit len={len(unit)})", flush=True)
        # verdicts: all matched -> pass; any deadline passed with an
        # unmatched expectation -> fail
        all_matched, failed = verdict(groups, args.window, now, t0)
        if all_matched:
            print(f"# all expectations matched (total {total} bytes)")
            return 0
        if failed:
            for e in failed:
                print(f"EXPECTATION NOT MATCHED: {e}")
            print(f"# total {total} bytes")
            return 1
        if now > t0 + args.seconds:
            print("EXPECTATION TIMEOUT: not all matched within --seconds")
            for g in groups:
                for e in g["expects"]:
                    if not e.ok:
                        print(f"EXPECTATION NOT MATCHED: {e}")
            return 1
        time.sleep(0.05)


def self_test():
    """Framer and expectation vectors; exits 0 on success."""
    tc0 = bytes.fromhex("40 02 00 00 1D AC C0 00 00 05 19 11 01 00 D7 7E")
    tc1 = bytes.fromhex("40 02 00 00 1D AC C0 01 00 05 19 11 01 00 6F 1F")
    tc2 = bytes.fromhex("40 02 00 00 1D AC C0 02 00 05 19 11 01 00 B7 9D")
    # TM(1,1) as the FSW prefixes it: 49 02 00 00 + SP (pktLen 7)
    tm11 = bytes.fromhex("49 02 00 00 05 A1 C0 00 00 07 19 01 01 00 "
                         "1D AC C0 00".replace(" ", ""))

    stream = bytearray()
    for chunk in (tc0[:1], tc0[1:6], tc0[6:], tc0[:4], tc0[4:]):
        stream += chunk
    units, resyncs = frame_units(stream)
    assert units == [tc0, tc0], f"split-assembly: {units}"
    assert resyncs == 0

    stream = bytearray(tc1 + b"\xee" + tc2)
    units, resyncs = frame_units(stream)
    assert units == [tc1, tc2], f"garbage byte: {units}"
    assert resyncs == 1, resyncs

    stream = bytearray(b"\x00\x11\x22" + tc0 + b"\xff\xee\xdd" + tc1)
    units, resyncs = frame_units(stream)
    assert units == [tc0, tc1], f"garbage head: {units}"
    assert resyncs == 6, resyncs  # one drop per garbage byte

    # B2 of the P2-6 review: the four real FSW TM units, full
    # [SpW hdr][CCSDS] units as on the wire (from the reviewer's
    # tc-smoke run). TM application data starts at SP[16].
    tm11 = bytes.fromhex(
        "490200000da1c000000d100101000000001481af1dacc000")
    tm172 = bytes.fromhex("490200000da7c005000910110200000000148220")
    tm17 = bytes.fromhex(
        "490200000da1c001000d1001070000000014825c1dacc000")
    tm12 = bytes.fromhex(
        "490200000da1c00200151001020000000014832a1dacc0010002110112346f1f")
    stream = bytearray(tm11 + tm172 + tm17 + tm12)
    units, resyncs = frame_units(stream)
    assert units == [tm11, tm172, tm17, tm12], f"TM frames: {units}"
    assert resyncs == 0
    assert Expect("0x5A1:1:1:1DACC000").matches(tm11)
    assert Expect("0x5A7:17:2").matches(tm172)
    assert Expect("0x5A1:1:7:1DACC000").matches(tm17)
    assert Expect("0x5A1:1:2:1DACC0010002").matches(tm12)
    # a wrong application-data prefix must not match
    assert not Expect("0x5A1:1:1:1DACC001").matches(tm11)
    assert not Expect("0x5A1:1:2:1DACC0010003").matches(tm12)

    # the @SECONDS suffix overrides one expectation's window
    ewin = Expect("0x5A1:1:7:1DACC002@3")
    assert ewin.window == 3.0
    assert ewin.data == bytes.fromhex("1DACC002")
    assert Expect("0x5A7:17:2").window is None
    # __str__ keeps the @ so failure messages show the tightened window
    assert str(ewin) == "0x5A1:1:7:1DACC002@3"

    # B1 of the P2-7 review: the actual verdict computation over a
    # MIXED group (Expect, Expect@window, CucExpect) — every attribute
    # the loop reads must exist on every kind, before and after the
    # send.
    mixed = {"hex": "40", "at": 20.0, "sent": False, "t_sent": None,
             "expects": [Expect("0x5A1:1:1:1DACC000"),
                         Expect("0x5A1:1:7@3"),
                         CucExpect("0x5E5:1000000")]}
    groups = [{"hex": None, "at": None, "expects": []}, mixed]
    # before the send: nothing can be failed (the group is not ready)
    allm, failed = verdict(groups, 10.0, 21.0, 0.0)
    assert allm is False and failed == []
    mixed["sent"] = True
    mixed["t_sent"] = 20.0
    allm, failed = verdict(groups, 10.0, 21.0, 0.0)
    assert allm is False and failed == []
    mixed["expects"][0].ok = True
    mixed["expects"][2].ok = True
    # the @3 expectation is still pending at +1.5 s: no failure yet
    allm, failed = verdict(groups, 10.0, 21.5, 0.0)
    assert allm is False and failed == []
    # at +4.5 s its own 3 s window has expired even though the group
    # window is 10 s
    allm, failed = verdict(groups, 10.0, 24.5, 0.0)
    assert allm is False and len(failed) == 1
    assert failed[0].window == 3.0 and str(failed[0]) == "0x5A1:1:7@3"
    mixed["expects"][1].ok = True
    allm, failed = verdict(groups, 10.0, 24.5, 0.0)
    assert allm is True and failed == []
    # a connect-time expectation fails on its own window from t0 (m2)
    conn = {"hex": None, "at": None, "sent": False, "t_sent": None,
            "expects": [Expect("0x5A7:17:2")]}
    allm, failed = verdict([conn], 10.0, 11.0, 0.0)
    assert allm is False and failed == [conn["expects"][0]]

    # the CUC coarse the FSW carries in the TM data field header
    # (SP[10:14]), from the reviewer's real heartbeat stream (APID
    # 0x5E5, type 3 subtype 25): the coarse ticks +1 per second
    hb = bytes.fromhex(
        "490200000de5c000001a1003190e000000013d120400000001"
        "000000000000000000000000")
    hb_late = bytes.fromhex(
        "490200000de5c00e001a1003190e00000010a0530400000010"
        "000000000000000000000000")
    assert hb[14:18].hex() == "00000001" and hb_late[14:18].hex() == "00000010"
    e = CucExpect("0x5E5:1000000")   # 0x10000000 + a few seconds
    assert not e.matches(hb) and not e.matches(hb_late)
    # the same heartbeat after the TC(9,129) set: coarse 0x10000003
    hb_set = bytes.fromhex(
        "490200000de5c00e001a1003190e" + "10000003" +
        "a0530400000010000000000000000000000000")
    assert hb_set[14:18].hex() == "10000003"
    assert e.matches(hb_set)
    assert not CucExpect("0x5E7:1000000").matches(hb_set)

    # B1 of the P2-6 review: the plain mode (soak.sh, boot-smoke.sh)
    # parses and sets up without gate options and never gates.
    plain = make_parser().parse_args(["--port", "21000", "--seconds", "30"])
    groups, gate = setup_mode(plain)
    assert gate is False
    assert groups == [{"at": None, "hex": None, "expects": []}]

    # M1 of the P2-6 review: --send-at is an ordered op, so two sends
    # keep their own offsets.
    two = make_parser().parse_args([
        "--send-at", "20", "--send-hex", tc0.hex(), "--expect", "0x5A1:1:1",
        "--send-at", "32", "--send-hex", tc1.hex(), "--expect", "0x5A7:17:2",
    ])
    groups, gate = setup_mode(two)
    sends = [g for g in groups if g["hex"] is not None]
    assert gate is True
    assert [g["at"] for g in sends] == [20.0, 32.0], \
        [g["at"] for g in sends]
    assert [len(g["expects"]) for g in sends] == [1, 1]

    print("SELF-TEST: PASS")
    return 0


def main():
    args = make_parser().parse_args()

    if args.self_test:
        return self_test()

    groups, gate = setup_mode(args)

    deadline = time.monotonic() + args.seconds
    # Retry connect for a few seconds in case QEMU is still binding.
    sock = None
    while time.monotonic() < deadline and sock is None:
        try:
            sock = socket.create_connection(("127.0.0.1", args.port), timeout=2)
        except OSError:
            time.sleep(0.5)
    if sock is None:
        print(f"NO TM OBSERVED: could not connect to 127.0.0.1:{args.port}")
        return 1

    sock.sendall(b"STIX_CONTROL")  # 12-byte handshake token (stripped)
    print(f"# connected to 127.0.0.1:{args.port}, listening {args.seconds}s",
          flush=True)
    sock.settimeout(1.0)

    if gate:
        return run_gate(sock, args, groups)

    saw_any = False
    total = 0
    while time.monotonic() < deadline:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        except OSError:
            break  # server closed
        if not chunk:
            break
        saw_any = True
        total += len(chunk)
        print(f"{ts()} +{len(chunk):4d}B {chunk[:48].hex()}")
    if not saw_any:
        print("NO TM OBSERVED")
        return 1
    print(f"# total {total} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
