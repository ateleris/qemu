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

TC ingress gate (--send-hex/--expect/--forbid): raw TC units are sent
on the same connection at their --send-at offsets, and the framed TM
stream is matched against the expectations. An expectation keys on
APID and PUS type/subtype (SP[7], SP[8] — correction 5), plus an
optional prefix of the application data (SP[10:]); expectations and
forbids attach to the most recent --send-hex, or to a connect-time
group when given before any --send-hex. Expectations match in ANY
order within their window; the run passes when every expectation
matched within --window seconds of its send — immediately, or at the
--seconds deadline under --hold. Gate mode without --hold exits on the
last match, so anything after it (a mode change, a fallback) is not
watched — that is why plain probing near a mode change once looked
like "TM stopped". A --forbid is the negation: a matching unit fails
the run with "FORBIDDEN TM: ...". It arms at its group's send (the
connect group arms at connect); a match before its group's send is
ignored. Since A-4 an optional @SECONDS suffix disarms the forbid
that many seconds after its group's send; without it the forbid stays
armed for the rest of the run.

QMP groups (--qmp PATH, --qmp-cmd JSON; P4-3): PATH is a unix socket or,
since PL-2b, tcp:HOST:PORT (IPv6 bracketed). A send group can be a
QMP command instead of a TC unit, e.g. a qom-set that injects a fault.
It takes the preceding --send-at, collects the expectations and
forbids that follow, and drains the TM already buffered before it
runs, exactly like a --send-hex. The probe connects to the QMP socket
at the first due --qmp-cmd (it starts before QEMU), does the greeting
and qmp_capabilities, and for each command skips async events until
the "return". An "error" reply, a closed socket or no reply within 5 s
fails the run with "QMP ERROR: ...".

--expect-return VALUE (A-4) attaches to the latest --qmp-cmd group (a
qom-get) and fails the run at once with "RETURN MISMATCH: ..." unless
the reply equals VALUE (VALUE is JSON when it parses — numbers, bools,
null — else the raw string, so a qom-get of a string property takes
the bare word). On a match the run continues and the group prints
"RETURN OK".

Full-packet dump (--dump-apid APID:FILE; detector plan D-3): the TM
lines above show at most 48 bytes of a unit. A dump APPENDS every framed
TM unit of that APID to FILE as one whole CCSDS packet per line, in hex,
WITHOUT the 4-byte SpW header (so a TM's SSID / first application byte
is at byte 16), flushed per line. It works in both modes and changes
nothing else; repeatable for different APIDs (one file each).

Usage:
  tmtc-probe.py [--port PORT] [--seconds N]
      logs raw chunks (prints 'NO TM OBSERVED', exits 1 if none)
  tmtc-probe.py [--port P] [--seconds N] --window W [--hold] [--qmp PATH]
      [--send-at S] (--send-hex HEX | --qmp-cmd JSON)
      [--expect APID:TYPE:SUBTYPE[:DATAHEX]]...
      [--expect-return VALUE]
      [--forbid APID:TYPE:SUBTYPE[:DATAHEX][@SECONDS]]...
      TC ingress gate
  either mode also takes [--dump-apid APID:FILE]...
  tmtc-probe.py --self-test
      runs the framer self-test and exits 0/1
"""
import argparse
import json
import os
import re
import socket
import sys
import threading
import time

# D-K: the 4-byte SpW header plus the 6 CCSDS primary-header bytes
# before the length field; a unit is 11 + BE16(unit[8:10]) long and can
# never exceed 4 + 65542 bytes.
TC_HDR_LEN = 10
TC_UNIT_MAX = 4 + 65542


class PacketDump:
    """--dump-apid APID:FILE (D-3): append each framed TM unit of APID
    to FILE as its CCSDS packet (unit[4:], no SpW header) in hex, one
    per line, flushed per line so a killed probe keeps what it saw."""

    def __init__(self, specs, opener=open):
        self.files = {}
        for spec in specs or []:
            apid_s, sep, path = spec.partition(":")
            if not sep or not path:
                raise ValueError("--dump-apid wants APID:FILE: %s" % spec)
            try:
                apid = int(apid_s, 0)
            except ValueError:
                raise ValueError("--dump-apid: bad APID in %s" % spec)
            if not 0 <= apid <= 0x7FF:
                raise ValueError("--dump-apid: APID out of range: %s" % spec)
            if apid in self.files:
                raise ValueError("--dump-apid: APID 0x%03X given twice" % apid)
            self.files[apid] = opener(path, "a")

    def write(self, unit):
        """Dump @unit when its APID is dumped; returns True if written."""
        sp = unit[4:]
        if len(sp) < 6:
            return False
        f = self.files.get(((sp[0] << 8) | sp[1]) & 0x7FF)
        if f is None:
            return False
        f.write(sp.hex() + "\n")
        f.flush()
        return True

    def close(self):
        for f in self.files.values():
            f.close()
        self.files = {}


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


class Forbid(Expect):
    """The negation of an Expect (--forbid, P3-2): a matching TM unit
    FAILS the run instead of satisfying it. Parsing and matching are
    Expect's. A forbid arms at its group's send — or at the connect for
    the connect-time group — and stays armed for the whole run, unless
    an @SECONDS suffix (A-4) disarms it that many seconds after its
    group's send: a match after the window is not evidence (the gates
    forbid a TM only while the condition under test can produce it).
    run_gate sets armed_at; the self-test sets it by hand."""

    def __init__(self, spec):
        super().__init__(spec)
        self.armed_at = None


class OpAction(argparse.Action):
    """Record --send-at/--send-hex/--qmp-cmd/--expect/--expect-return in
    command-line order, so --send-at applies to the following --send-hex
    or --qmp-cmd and --expect/--expect-return attach to the most recent
    of them."""

    def __call__(self, parser, namespace, values, option_string=None):
        ops = getattr(namespace, "ops", None) or []
        ops.append((option_string, values))
        namespace.ops = ops


def is_send(g):
    """A send group (a TC or a QMP command, sent at g["at"]) versus the
    connect-time group, which has no send offset (P4-3)."""
    return g["at"] is not None


def parse_qmp_cmd(text):
    """--qmp-cmd JSON: one QMP command object with an "execute" key,
    validated when the groups are built so a typo fails before any
    boot (ValueError, like a bad --expect)."""
    try:
        cmd = json.loads(text)
    except ValueError as e:
        raise ValueError("--qmp-cmd is not JSON: %s (%s)" % (text, e))
    if not isinstance(cmd, dict) or "execute" not in cmd:
        raise ValueError("--qmp-cmd needs a JSON object with an "
                         "\"execute\" key: %s" % text)
    return cmd


def parse_return_value(text):
    """--expect-return VALUE (A-4): JSON when it parses (a qom-get
    returns numbers, bools and null), else the raw string (a qom-get of
    a string property is compared with the bare word)."""
    try:
        return json.loads(text)
    except ValueError:
        return text


class QmpError(Exception):
    """A QMP failure: an error reply, a closed socket or a timeout."""


def qmp_address(path):
    """The kind and address of a --qmp value (PL-2b): ("unix", path), or
    ("tcp", (host, port)) for tcp:HOST:PORT, where an IPv6 HOST is
    bracketed (tcp:[::1]:PORT) and PORT is 1..65535 (platform plan
    D-PLj; the callers keep HOST to the loopback)."""
    if not path.startswith("tcp:"):
        return "unix", path
    rest = path[4:]
    if rest.startswith("["):
        host, sep, tail = rest[1:].partition("]")
        if not sep or not tail.startswith(":"):
            host = ""
        port = tail[1:]
    else:
        host, sep, port = rest.rpartition(":")
        if ":" in host:  # an unbracketed IPv6 address is ambiguous
            host = ""
    if not host or not port.isdigit() or not 0 < int(port) < 65536:
        raise QmpError("bad QMP address %r: want tcp:HOST:PORT or "
                       "tcp:[IPV6]:PORT" % path)
    return "tcp", (host, int(port))


class QmpClient:
    """A minimal synchronous QMP client (P4-3).

    QMP is newline-delimited JSON: the server greets with {"QMP": ...},
    the client must send {"execute": "qmp_capabilities"} before any
    other command, and every command gets exactly one {"return": ...}
    or {"error": ...}. Asynchronous {"event": ...} messages can come at
    any time — before the greeting reply, between commands — and are
    skipped (printed). The connection is made lazily, on the first
    execute(), because the probe starts before QEMU. @sock (an already
    connected socket) is for the self-test; @timeout bounds each
    command's wait for its reply (and the connect retries).
    """

    def __init__(self, path=None, sock=None, timeout=5.0):
        self.path = path
        self.sock = sock
        self.timeout = timeout
        self.buf = bytearray()
        self.ready = False

    def _connect(self):
        deadline = time.monotonic() + self.timeout
        if self.sock is None:
            kind, addr = qmp_address(self.path)
            while True:
                try:
                    if kind == "tcp":
                        # every address family the host resolves to
                        s = socket.create_connection(
                            addr, timeout=max(0.1, deadline -
                                              time.monotonic()))
                    else:
                        s = socket.socket(socket.AF_UNIX,
                                          socket.SOCK_STREAM)
                        try:
                            s.connect(addr)
                        except OSError:
                            s.close()
                            raise
                    self.sock = s
                    break
                except OSError as e:
                    if time.monotonic() >= deadline:
                        raise QmpError("cannot connect to %s: %s"
                                       % (self.path, e))
                    time.sleep(0.2)
        greeting = self._read_msg(deadline)
        if "QMP" not in greeting:
            raise QmpError("no QMP greeting: %s" % json.dumps(greeting))
        self.ready = True
        self.execute({"execute": "qmp_capabilities"})

    def _read_msg(self, deadline):
        """The next JSON message (one line), events included."""
        while True:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                line = bytes(self.buf[:nl]).strip()
                del self.buf[:nl + 1]
                if not line:
                    continue
                try:
                    return json.loads(line)
                except ValueError:
                    raise QmpError("bad JSON from QMP: %r" % line[:200])
            left = deadline - time.monotonic()
            if left <= 0:
                raise QmpError("no reply within %.1f s" % self.timeout)
            self.sock.settimeout(left)
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                raise QmpError("no reply within %.1f s" % self.timeout)
            except OSError as e:
                raise QmpError("socket error: %s" % e)
            if not chunk:
                raise QmpError("connection closed")
            self.buf += chunk

    def execute(self, cmd):
        """Send one command; return its "return" value, skipping events.
        Raises QmpError on an "error" reply, a close or a timeout."""
        if not self.ready:
            self._connect()
        try:
            self.sock.sendall(json.dumps(cmd).encode() + b"\n")
        except OSError as e:
            raise QmpError("send failed: %s" % e)
        deadline = time.monotonic() + self.timeout
        while True:
            msg = self._read_msg(deadline)
            if "event" in msg:
                print(f"{ts()} QMP event {msg.get('event')} (skipped) "
                      f"{mono()}",
                      flush=True)
                continue
            if "return" in msg:
                return msg["return"]
            if "error" in msg:
                err = msg["error"]
                raise QmpError("%s: %s (command %s)" % (
                    err.get("class", "?"), err.get("desc", "?"),
                    json.dumps(cmd)))
            raise QmpError("unexpected QMP message: %s" % json.dumps(msg))

    def close(self):
        if self.sock is not None:
            self.sock.close()
            self.sock = None


def build_groups(args):
    """Group the ordered options into send groups (+ a connect-time
    group for expectations and forbids given before any send). A send
    group is a --send-hex TC unit or a --qmp-cmd QMP command (P4-3);
    both take the preceding --send-at and collect the expectations and
    forbids that follow."""
    groups = [{"at": None, "hex": None, "expects": [], "forbids": []}]
    current_at = 20.0
    for op, val in args.ops:
        if op == "--send-at":
            current_at = float(val)
        elif op == "--send-hex":
            groups.append({"at": current_at, "hex": val, "qmp": None,
                           "expects": [], "forbids": [], "sent": False,
                           "t_sent": None})
        elif op == "--qmp-cmd":
            groups.append({"at": current_at, "hex": None,
                           "qmp": parse_qmp_cmd(val), "expects": [],
                           "forbids": [], "sent": False, "t_sent": None})
        elif op == "--expect":
            groups[-1]["expects"].append(Expect(val))
        elif op == "--expect-cuc":
            groups[-1]["expects"].append(CucExpect(val))
        elif op == "--expect-return":
            g = groups[-1]
            if not is_send(g) or g.get("qmp") is None:
                raise ValueError("--expect-return attaches to the latest "
                                 "--qmp-cmd group (a qom-get), not to %s"
                                 % ("a --send-hex group"
                                    if is_send(g) else "no send group yet"))
            g["expect_return"] = (val, parse_return_value(val))
        elif op == "--forbid":
            groups[-1]["forbids"].append(Forbid(val))
    return groups


def setup_mode(args):
    """Plain mode (no gate options) gets one connect-time group and
    gate=False; gate mode returns the send groups."""
    groups = build_groups(args)
    gate = any(is_send(g) for g in groups) or \
        any(g["expects"] for g in groups) or \
        any(g["forbids"] for g in groups)
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
    ap.add_argument("--expect-return", action=OpAction, metavar="VALUE",
                    help="the latest --qmp-cmd group (a qom-get) must "
                         "return exactly VALUE, else the run fails at "
                         "once with RETURN MISMATCH (A-4). VALUE is JSON "
                         "when it parses, else the raw string "
                         "(repeatable, one per --qmp-cmd)")
    ap.add_argument("--forbid", action=OpAction,
                    metavar="APID:TYPE:SUBTYPE[:DATAHEX][@SECONDS]",
                    help="the negation of --expect: a matching TM unit "
                         "fails the run. Attaches to the most recent "
                         "--send-hex (or the connect group) and arms at "
                         "that group's send; a match before the send is "
                         "ignored. An optional @SECONDS suffix disarms "
                         "the forbid that many seconds after its group's "
                         "send (A-4); without it the forbid stays armed "
                         "for the run (repeatable)")
    ap.add_argument("--qmp", metavar="PATH",
                    help="QEMU QMP unix socket, or tcp:HOST:PORT "
                         "(tcp:[::1]:PORT for IPv6), for the --qmp-cmd "
                         "groups (connected at the first due --qmp-cmd)")
    ap.add_argument("--qmp-cmd", action=OpAction, metavar="JSON",
                    help="a QMP command object executed at the preceding "
                         "--send-at, e.g. '{\"execute\": \"qom-set\", "
                         "\"arguments\": {...}}'. A send group like "
                         "--send-hex: the expectations and forbids that "
                         "follow attach to it. An error reply, a closed "
                         "socket or no reply within 5 s fails the run "
                         "with 'QMP ERROR: ...' (repeatable, needs --qmp)")
    ap.add_argument("--hold", action="store_true",
                    help="gate mode: keep listening until --seconds even "
                         "after every expectation matched, so a later "
                         "mode change cannot be missed (without it the "
                         "run exits on the last match)")
    ap.add_argument("--dump-apid", action="append", default=[],
                    metavar="APID:FILE",
                    help="append every TM unit of APID to FILE as a "
                         "whole CCSDS packet (no SpW header), hex, one "
                         "per line (repeatable, one file per APID)")
    ap.add_argument("--self-test", action="store_true",
                    help="run the framer self-test and exit")
    # B1 of the P2-6 review: ops must exist even when no gate option is
    # given (soak.sh and boot-smoke.sh run the plain mode).
    ap.set_defaults(ops=[])
    return ap


def ts():
    return time.strftime("%H:%M:%S")


def mono():
    """The event's CLOCK_MONOTONIC stamp, appended to every event line as
    [m=SECONDS]. Gates time windows from these, never from the wall-clock
    HH:MM:SS: the wall clock can be stepped mid-run (WSL's timesyncd
    pulls it back to the Windows time), the monotonic clock, which QEMU's
    virtual time follows, cannot. The value is system-wide, so a gate can
    relate it to its own time.monotonic() samples."""
    return f"[m={time.monotonic():.3f}]"


def verdict(groups, group_window, now, t0):
    """The run_gate verdict computation: (all_matched, failed).

    Factored out so the self-test can run it over a mixed group (the
    P2-7 review B1): every attribute the loop reads — window, ok,
    __str__ — must exist on every expectation kind. Forbids are not
    verdict state: they are checked per unit in match_unit.
    """
    all_matched = True
    failed = []
    for g in groups:
        if is_send(g) and not g["sent"]:
            all_matched = False
        # m2 of the P2-6 review: the connect-time group binds to t0
        # with its own window
        start = g["t_sent"] if is_send(g) else t0
        ready = g["sent"] if is_send(g) else True
        for e in g["expects"]:
            if not e.ok:
                all_matched = False
                win = e.window if e.window is not None else group_window
                if ready and now > start + win:
                    failed.append(e)
    return all_matched, failed


def gate_done(all_matched, has_failed, hold, now, t0, seconds):
    """The run_gate exit rule, factored for the self-test (P3-2).

    Returns 0 (pass), 1 (fail) or None (keep running). A run passes
    when every expectation matched — immediately, or at the --seconds
    deadline under --hold, which keeps watching the stream after the
    last match so a later mode change cannot slip through unseen. It
    fails as soon as an expectation's window expired (has_failed). A
    forbidden unit fails the run at match time (match_unit), hold or
    not.
    """
    if has_failed:
        return 1
    if not hold and all_matched:
        return 0
    if now >= t0 + seconds:
        return 0 if all_matched else 1
    return None


def match_unit(unit, groups, now=None):
    """One framed TM unit against every ARMED group: expectations it
    satisfies are marked ok and returned (newly_ok); the first matching
    forbid is returned (forbidden) and the caller fails the run. A
    group arms when its TC has been sent; the connect-time group is
    armed from the start, so its forbids cover the whole run. A forbid
    with an @SECONDS window is skipped once @now is past armed_at plus
    the window (A-4); run_gate sets armed_at to the group's send time
    (the connect for the connect group). Factored out of run_gate for
    the self-test (P3-2)."""
    if now is None:
        now = time.monotonic()
    fields = unit_fields(unit)
    if fields is None:
        return [], None
    newly_ok = []
    for g in groups:
        if is_send(g) and not g["sent"]:
            continue  # m3/P3-2: its TC has not gone out yet; not armed
        for e in g["expects"]:
            if not e.ok and e.matches(unit):
                e.ok = True
                newly_ok.append(e)
    for g in groups:
        if is_send(g) and not g["sent"]:
            continue
        for f in g["forbids"]:
            if f.window is not None and f.armed_at is not None \
                    and now > f.armed_at + f.window:
                continue  # A-4: the @SECONDS window disarmed it
            if f.matches(unit):
                return newly_ok, f
    return newly_ok, None


def handle_chunk(chunk, groups, pending, dump=None):
    """Frame and match one received chunk: extends @pending, prints the
    raw line, the TM lines and the newly satisfied expectations, and
    returns the first forbidden unit the chunk carried (or None).
    Factored so the send-side drain (review m1) shares the matcher.
    @dump (a PacketDump) gets every framed unit before it is matched."""
    pending += chunk
    print(f"{ts()} +{len(chunk):4d}B {chunk[:48].hex()} {mono()}",
          flush=True)
    units, _ = frame_units(pending)
    for unit in units:
        if dump is not None:
            dump.write(unit)
        fields = unit_fields(unit)
        if fields is None:
            continue
        apid, ptype, subtype, _app = fields
        print(f"{ts()} TM apid=0x{apid:03X} type={ptype} "
              f"subtype={subtype} len={len(unit)} "
              f"data={unit[:48].hex()} {mono()}", flush=True)
        newly_ok, forbidden = match_unit(unit, groups)
        for e in newly_ok:
            print(f"{ts()} EXPECT OK: {e} "
                  f"(unit len={len(unit)}) {mono()}", flush=True)
        if forbidden is not None:
            return forbidden
    return None


def run_plain(sock, deadline, dump=None):
    """The plain mode: log raw chunks until @deadline or a close; with a
    @dump (--dump-apid), also frame the stream and dump the units.
    Returns 1 when no TM arrived (prints 'NO TM OBSERVED'), else 0."""
    saw_any = False
    total = 0
    pending = bytearray()
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
        print(f"{ts()} +{len(chunk):4d}B {chunk[:48].hex()} {mono()}")
        if dump is not None and dump.files:
            pending += chunk
            for unit in frame_units(pending)[0]:
                dump.write(unit)
    if not saw_any:
        print("NO TM OBSERVED")
        return 1
    print(f"# total {total} bytes")
    return 0


def run_gate(sock, args, groups, qmp=None, dump=None):
    """The --send-hex/--qmp-cmd/--expect/--forbid mode: send at the
    offsets, match the framed TM stream. Returns 0 when every
    expectation matched within its window — immediately, or at
    --seconds under --hold; a forbidden unit, an expired expectation, a
    QMP failure or (under --hold) a closed connection returns 1. @qmp
    is the QmpClient the --qmp-cmd groups use; @dump the PacketDump."""
    t0 = time.monotonic()
    # a forbid arms at its group's send; the connect group's at the
    # connect (A-4 @SECONDS windows count from that instant)
    for g in groups:
        for f in g["forbids"]:
            if f.armed_at is None:
                f.armed_at = t0
    pending = bytearray()
    total = 0
    held = False
    sock.settimeout(0.5)
    while True:
        now = time.monotonic()
        for g in groups:
            if is_send(g) and not g["sent"] and now - t0 >= g["at"]:
                # review m1: drain what is already buffered BEFORE
                # marking the group sent — units that arrived earlier
                # are matched with the group still unarmed, so they
                # cannot trip its forbids
                forbidden = None
                while forbidden is None:
                    try:
                        chunk = sock.recv(4096, socket.MSG_DONTWAIT)
                    except OSError:
                        break
                    if not chunk:
                        break
                    total += len(chunk)
                    forbidden = handle_chunk(chunk, groups, pending, dump)
                if forbidden is not None:
                    print(f"FORBIDDEN TM: {forbidden}", flush=True)
                    print(f"# total {total} bytes")
                    return 1
                if g.get("qmp") is not None:
                    # P4-3: a QMP group; any failure ends the run
                    try:
                        ret = qmp.execute(g["qmp"])
                    except QmpError as e:
                        print(f"QMP ERROR: {e}", flush=True)
                        print(f"# total {total} bytes")
                        return 1
                    g["sent"] = True
                    g["t_sent"] = time.monotonic()
                    for f in g["forbids"]:
                        f.armed_at = g["t_sent"]
                    print(f"{ts()} >> qmp {json.dumps(g['qmp'])} -> "
                          f"return {json.dumps(ret)} at +{g['at']:.0f}s "
                          f"{mono()}", flush=True)
                    er = g.get("expect_return")
                    if er is not None:
                        if ret != er[1]:
                            print(f"RETURN MISMATCH: want {er[0]}, got "
                                  f"{json.dumps(ret)} "
                                  f"({json.dumps(g['qmp'])})", flush=True)
                            print(f"# total {total} bytes")
                            return 1
                        print(f"{ts()} RETURN OK: {er[0]} {mono()}",
                              flush=True)
                    continue
                payload = bytes.fromhex(g["hex"])
                sock.sendall(payload)
                g["sent"] = True
                g["t_sent"] = time.monotonic()
                for f in g["forbids"]:
                    f.armed_at = g["t_sent"]
                print(f"{ts()} >> sent {len(payload)}B "
                      f"({g['hex'][:32]}...) at +{g['at']:.0f}s {mono()}",
                      flush=True)
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            chunk = None
        except OSError:
            chunk = None
        if chunk:
            total += len(chunk)
            forbidden = handle_chunk(chunk, groups, pending, dump)
            if forbidden is not None:
                print(f"FORBIDDEN TM: {forbidden}", flush=True)
                print(f"# total {total} bytes")
                return 1
        elif chunk is not None and args.hold:
            # review m3: a successful recv returning b"" is a closed
            # connection. Under --hold that must fail the run — the
            # verdict at the deadline would otherwise silently pass on
            # a dead stream. WITHOUT --hold the pre-P3-2 behaviour
            # stays (spin until the deadline): tc-smoke's runs never
            # see a mid-run close, and its runs end on the last match.
            print("CONNECTION CLOSED", flush=True)
            print(f"# total {total} bytes")
            return 1
        # verdicts: all matched -> pass (at once, or at the deadline
        # under --hold); any deadline passed with an unmatched
        # expectation -> fail
        all_matched, failed = verdict(groups, args.window, now, t0)
        rc = gate_done(all_matched, bool(failed), args.hold, now, t0,
                       args.seconds)
        if rc == 0:
            print(f"# all expectations matched (total {total} bytes)")
            return 0
        if rc == 1:
            if failed:
                for e in failed:
                    print(f"EXPECTATION NOT MATCHED: {e}")
            else:
                print("EXPECTATION TIMEOUT: not all matched within "
                      "--seconds")
                for g in groups:
                    for e in g["expects"]:
                        if not e.ok:
                            print(f"EXPECTATION NOT MATCHED: {e}")
            print(f"# total {total} bytes")
            return 1
        if all_matched and args.hold and not held:
            held = True
            print(f"# all expectations matched; holding until +"
                  f"{args.seconds:.0f}s (--hold)", flush=True)
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
    assert groups == [{"at": None, "hex": None, "expects": [],
                       "forbids": []}]

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

    # ---- P3-2: --hold and --forbid (the NOMINAL gate) ----
    # A forbid reuses Expect's parsing and matching; since A-4 an
    # @SECONDS suffix is its disarm window (the window cases follow in
    # the A-4 block)
    assert Forbid("0x5A7:5:1:00210402@30").window == 30.0
    fb = Forbid("0x5A7:5:1:00210402")
    assert fb.apid == 0x5A7 and fb.ptype == 5 and fb.subtype == 1
    assert fb.data == bytes.fromhex("00210402")
    assert str(fb) == "0x5A7:5:1:00210402"
    # a data-less forbid matches on APID/type/subtype alone (tm18 is
    # the tm17 TM(1,7) unit with the subtype byte changed to 8)
    tm18 = bytearray(tm17)
    tm18[12] = 0x08
    assert Forbid("0x5A1:1:8").matches(bytes(tm18))
    assert not Forbid("0x5A1:1:8").matches(tm17)

    # hand-built TM(5,1) 0021 events on 0x5A7 (data field 10 05 01 00 +
    # 6 B SCET + EID + params <old> <new> <reason>; recon-phase3 §4)
    def tm51_unit(seq, eid, params):
        data = bytes.fromhex("10050100") + bytes(6) + \
            bytes.fromhex(eid) + bytes.fromhex(params)
        sp = bytes.fromhex("0DA7") + bytes([0xC0, seq]) + \
            (len(data) - 1).to_bytes(2, "big") + data
        return bytes.fromhex("49020000") + sp

    ev_nom_to_safe = tm51_unit(1, "0021", "050202")
    ev_conf_to_safe = tm51_unit(2, "0021", "040202")
    assert Expect("0x5A7:5:1:00210502").matches(ev_nom_to_safe)
    assert Expect("0x5A7:5:1:00210402").matches(ev_conf_to_safe)
    assert not Expect("0x5A7:5:1:00210502").matches(ev_conf_to_safe)

    # --forbid is an ordered op: it attaches to the most recent
    # --send-hex, or to the connect group when it comes before any;
    # --forbid alone is gate mode; --hold parses
    p = make_parser().parse_args([
        "--forbid", "0x5A7:5:1:00210402",
        "--send-at", "50", "--send-hex", "40", "--expect", "0x5A1:1:1",
        "--forbid", "0x5A7:5:1:00210502",
        "--hold",
    ])
    assert p.hold is True
    groups, gate = setup_mode(p)
    assert gate is True
    assert [len(g["forbids"]) for g in groups] == [1, 1], \
        [len(g["forbids"]) for g in groups]
    p2 = make_parser().parse_args(["--forbid", "0x5A1:1:8"])
    groups, gate = setup_mode(p2)
    assert gate is True
    assert len(groups[0]["forbids"]) == 1

    # the factored per-unit match step (what run_gate's read loop
    # calls), over the two groups
    gconn = {"hex": None, "at": None, "sent": False, "t_sent": None,
             "expects": [], "forbids": [Forbid("0x5A7:5:1:00210402")]}
    gsend = {"hex": "40", "at": 50.0, "sent": False, "t_sent": None,
             "expects": [Expect("0x5A1:1:1")],
             "forbids": [Forbid("0x5A7:5:1:00210502")]}
    groups = [gconn, gsend]
    # before the send, only the connect group is armed: the NOM->SAFE
    # event is ignored (the send group's forbid is not armed yet)
    ok, forb = match_unit(ev_nom_to_safe, groups)
    assert ok == [] and forb is None
    # the CONF->SAFE event matches the CONNECT group's armed forbid
    ok, forb = match_unit(ev_conf_to_safe, groups)
    assert ok == [] and forb is not None
    assert str(forb) == "0x5A7:5:1:00210402"
    # after the send group armed, its own forbid fires; an expectation
    # the same poll satisfies is still returned as newly ok
    gsend["sent"] = True
    gsend["t_sent"] = 50.0
    ok, forb = match_unit(tm11, groups)
    assert ok == [gsend["expects"][0]] and forb is None
    ok, forb = match_unit(ev_nom_to_safe, groups)
    assert ok == [] and forb is not None
    assert str(forb) == "0x5A7:5:1:00210502"

    # the exit rule (what run_gate applies per poll): hold keeps the
    # run alive after the last match, until the deadline
    assert gate_done(True, False, False, 30.0, 0.0, 60.0) == 0
    assert gate_done(False, False, False, 30.0, 0.0, 60.0) is None
    assert gate_done(False, True, False, 30.0, 0.0, 60.0) == 1
    assert gate_done(True, False, True, 30.0, 0.0, 60.0) is None
    assert gate_done(True, False, True, 59.9, 0.0, 60.0) is None
    assert gate_done(True, False, True, 60.0, 0.0, 60.0) == 0
    assert gate_done(False, False, True, 60.0, 0.0, 60.0) == 1
    # and a forbid fails a hold run even with everything matched:
    # gate_done says "keep running", match_unit still reports the unit
    gsend["expects"][0].ok = True
    allm, failed = verdict(groups, 20.0, 55.0, 0.0)
    assert allm is True and failed == []
    assert gate_done(allm, bool(failed), True, 55.0, 0.0, 60.0) is None
    ok, forb = match_unit(ev_nom_to_safe, groups)
    assert forb is not None  # -> run_gate returns 1 with FORBIDDEN TM

    # ---- A-4: a forbid @SECONDS window disarms after its group's send ----
    fbw = Forbid("0x5A7:5:1:00210402@5")
    assert fbw.window == 5.0 and fbw.data == bytes.fromhex("00210402")
    assert fbw.apid == 0x5A7 and fbw.ptype == 5 and fbw.subtype == 1
    assert str(fbw) == "0x5A7:5:1:00210402@5"  # the @ shows in failures
    assert Forbid("0x5A7:5:1:00210402").window is None
    assert Forbid("0x5A1:1:8@0.2").window == 0.2
    # the disarm is checked against armed_at (the group's send time,
    # which run_gate sets; here by hand) and a now past it
    gwin = {"hex": None, "at": None, "sent": False, "t_sent": None,
            "expects": [], "forbids": [Forbid("0x5A7:5:1:00210402@5")]}
    gwin["forbids"][0].armed_at = 100.0
    ok, forb = match_unit(ev_conf_to_safe, [gwin], now=104.0)
    assert ok == [] and forb is not None
    ok, forb = match_unit(ev_conf_to_safe, [gwin], now=105.0)
    assert ok == [] and forb is not None    # the window ends AT 105.0
    ok, forb = match_unit(ev_conf_to_safe, [gwin], now=105.5)
    assert ok == [] and forb is None        # disarmed
    # a send group's window counts from ITS send, not the connect
    gconn = {"hex": None, "at": None, "sent": False, "t_sent": None,
             "expects": [], "forbids": []}
    gsend = {"hex": "40", "at": 50.0, "sent": True, "t_sent": 100.0,
             "expects": [], "forbids": [Forbid("0x5A7:5:1:00210502@3")]}
    gsend["forbids"][0].armed_at = 100.0
    ok, forb = match_unit(ev_nom_to_safe, [gconn, gsend], now=102.9)
    assert forb is not None
    ok, forb = match_unit(ev_nom_to_safe, [gconn, gsend], now=103.1)
    assert forb is None
    # without a @suffix the forbid stays armed for the whole run (P3-2)
    gsend["forbids"] = [Forbid("0x5A7:5:1:00210502")]
    ok, forb = match_unit(ev_nom_to_safe, [gsend], now=1e9)
    assert forb is not None

    # review m2: run_gate itself, over a socketpair (the peer plays the
    # model: it reads the probe's TC byte, then writes TM units). Each
    # case stays under ~2 s.
    def gate_in_thread(sock, pargs, pgroups, qmp=None):
        box = {}

        def target():
            box["rc"] = run_gate(sock, pargs, pgroups, qmp)

        th = threading.Thread(target=target)
        th.start()
        return th, box

    gate_args = ["--hold", "--seconds", "1", "--window", "1",
                 "--send-at", "0", "--send-hex", "40",
                 "--expect", "0x5A1:1:1"]

    # a. a matching unit: rc 0, but only AT the --seconds deadline
    #    (--hold keeps watching after the last match)
    pargs = make_parser().parse_args(gate_args)
    groups, gate = setup_mode(pargs)
    a, b = socket.socketpair()
    t_start = time.monotonic()
    th, box = gate_in_thread(a, pargs, groups)
    assert b.recv(1) == b"@"            # the probe's TC byte
    b.sendall(tm11)                     # the matching unit
    th.join(timeout=3)
    elapsed = time.monotonic() - t_start
    assert not th.is_alive()
    assert box["rc"] == 0
    assert 0.95 <= elapsed <= 2.0, elapsed
    b.close()

    # b. a forbidden unit written AFTER the expectation matched: rc 1
    pargs = make_parser().parse_args(
        gate_args + ["--forbid", "0x5A7:5:1:00210502"])
    groups, gate = setup_mode(pargs)
    a, b = socket.socketpair()
    th, box = gate_in_thread(a, pargs, groups)
    assert b.recv(1) == b"@"
    b.sendall(tm11 + ev_nom_to_safe)    # match, then the forbidden unit
    th.join(timeout=3)
    assert not th.is_alive()
    assert box["rc"] == 1
    b.close()

    # c. the peer closes while --hold is on: rc 1 (review m3)
    pargs = make_parser().parse_args(gate_args)
    groups, gate = setup_mode(pargs)
    a, b = socket.socketpair()
    th, box = gate_in_thread(a, pargs, groups)
    assert b.recv(1) == b"@"
    b.sendall(tm11)
    b.close()                           # close under --hold
    th.join(timeout=3)
    assert not th.is_alive()
    assert box["rc"] == 1

    # d. A-4: a forbidden unit INSIDE a 0.2 s window fails the run (like
    #    case b), the same unit after the window is ignored: rc 0 at the
    #    --seconds deadline
    pargs = make_parser().parse_args(
        gate_args + ["--forbid", "0x5A7:5:1:00210502@0.2"])
    groups, gate = setup_mode(pargs)
    a, b = socket.socketpair()
    th, box = gate_in_thread(a, pargs, groups)
    assert b.recv(1) == b"@"
    time.sleep(0.5)                     # past the 0.2 s window
    b.sendall(tm11 + ev_nom_to_safe)    # the match, then the disarmed unit
    th.join(timeout=3)
    assert not th.is_alive()
    assert box["rc"] == 0
    b.close()

    # ---- P4-3: QMP groups ----
    # --qmp-cmd is a send group like --send-hex: it takes the preceding
    # --send-at and collects the following expects/forbids; a bad JSON
    # or a missing "execute" is rejected when the groups are built.
    qset = ('{"execute": "qom-set", "arguments": {"path": '
            '"/machine/stixctrl", "property": "adc-inject[24]", '
            '"value": 1800}}')
    p = make_parser().parse_args([
        "--qmp", "/x/qmp.sock",
        "--expect", "0x5A7:5:1:14a3",
        "--send-at", "25", "--send-hex", "40", "--expect", "0x5A1:1:7",
        "--send-at", "30", "--qmp-cmd", qset,
        "--expect", "0x5A7:5:3:9449", "--forbid", "0x5A1:1:8",
    ])
    groups, gate = setup_mode(p)
    assert gate is True and p.qmp == "/x/qmp.sock"
    assert [g["at"] for g in groups] == [None, 25.0, 30.0]
    assert groups[1]["qmp"] is None and groups[1]["hex"] == "40"
    assert groups[2]["hex"] is None
    assert groups[2]["qmp"]["arguments"]["value"] == 1800
    assert [len(g["expects"]) for g in groups] == [1, 1, 1]
    assert [len(g["forbids"]) for g in groups] == [0, 0, 1]
    # a QMP-only gate is a gate
    groups, gate = setup_mode(make_parser().parse_args(
        ["--qmp", "/x", "--qmp-cmd", qset]))
    assert gate is True and is_send(groups[1])
    for bad in ('{"execute": ', '["qom-set"]', '{"arguments": {}}'):
        try:
            setup_mode(make_parser().parse_args(["--qmp-cmd", bad]))
            raise AssertionError("bad --qmp-cmd accepted: %s" % bad)
        except ValueError:
            pass

    greet = (b'{"QMP": {"version": {"qemu": {"major": 11}}, '
             b'"capabilities": ["oob"]}}\r\n')
    ok_ret = b'{"return": {}}\r\n'

    def fake_qmp(peer, script):
        """Play QEMU's side over @peer: greet, then for each step read
        one command line and answer with @reply bytes (None: close)."""
        def target():
            f = peer.makefile("rb")
            peer.sendall(greet)
            for reply in script:
                line = f.readline()
                if not line:
                    return
                box.setdefault("cmds", []).append(json.loads(line))
                if reply is None:
                    # shutdown, not close: the makefile keeps the fd
                    # open, and the client must see EOF
                    peer.shutdown(socket.SHUT_RDWR)
                    return
                peer.sendall(reply)
            box["done"] = True
        box = {}
        th = threading.Thread(target=target)
        th.start()
        return th, box

    def qmp_case(script, timeout=1.0):
        a, b = socket.socketpair()
        th, box = fake_qmp(b, script)
        c = QmpClient(sock=a, timeout=timeout)
        t_start = time.monotonic()
        try:
            ret = c.execute(json.loads(qset))
            err = None
        except QmpError as e:
            ret, err = None, str(e)
        elapsed = time.monotonic() - t_start
        th.join(timeout=3)
        assert not th.is_alive()
        assert elapsed < 2.0, elapsed
        a.close()
        b.close()
        return ret, err, box

    # q1. success: greeting, qmp_capabilities first, then the command
    ret, err, box = qmp_case([ok_ret, b'{"return": {"x": 1}}\r\n'])
    assert err is None and ret == {"x": 1}, (ret, err)
    assert [m["execute"] for m in box["cmds"]] == \
        ["qmp_capabilities", "qom-set"]
    # q2. an error reply raises, with the QMP class and desc
    ret, err, box = qmp_case([ok_ret, (
        b'{"error": {"class": "GenericError", "desc": "Property '
        b'adc-inject[24] value 4096 out of range"}}\r\n')])
    assert ret is None and "GenericError" in err and "4096" in err, err
    # q3. async events before the return (one split across two writes
    #     would be the same bytes on the stream) are skipped
    ev = (b'{"timestamp": {"seconds": 1, "microseconds": 2}, '
          b'"event": "RTC_CHANGE", "data": {"offset": 0}}\r\n')
    ret, err, box = qmp_case([ev + ok_ret, ev + ev + ok_ret])
    assert err is None and ret == {}, (ret, err)
    # q4. the socket closes instead of answering
    ret, err, box = qmp_case([ok_ret, None])
    assert ret is None and "closed" in err, err
    # q5. no reply at all: the (shortened) timeout fires
    ret, err, box = qmp_case([ok_ret, b""], timeout=0.4)
    assert ret is None and "no reply" in err, err

    # q6. run_gate with a QMP group: the command goes out at its
    #     offset, its expectation arms then and matches the TM unit
    #     the TMTC peer writes afterwards; rc 0 at the deadline
    qargs = ["--hold", "--seconds", "1", "--window", "1",
             "--send-at", "0", "--qmp-cmd", qset,
             "--expect", "0x5A1:1:1"]
    pargs = make_parser().parse_args(qargs)
    groups, gate = setup_mode(pargs)
    qa, qb = socket.socketpair()
    qth, qbox = fake_qmp(qb, [ok_ret, ok_ret])
    a, b = socket.socketpair()
    th, box = gate_in_thread(a, pargs, groups,
                             QmpClient(sock=qa, timeout=1.0))
    qth.join(timeout=3)
    b.sendall(tm11)
    th.join(timeout=3)
    assert not th.is_alive() and qbox.get("done")
    assert box["rc"] == 0
    for s_ in (a, b, qa, qb):
        s_.close()

    # q7. run_gate with an error reply: rc 1 at once (QMP ERROR)
    pargs = make_parser().parse_args(qargs)
    groups, gate = setup_mode(pargs)
    qa, qb = socket.socketpair()
    qth, qbox = fake_qmp(qb, [ok_ret, (
        b'{"error": {"class": "GenericError", "desc": "nope"}}\r\n')])
    a, b = socket.socketpair()
    t_start = time.monotonic()
    th, box = gate_in_thread(a, pargs, groups,
                             QmpClient(sock=qa, timeout=1.0))
    th.join(timeout=3)
    qth.join(timeout=3)
    assert not th.is_alive()
    assert box["rc"] == 1
    assert time.monotonic() - t_start < 0.9  # before the deadline
    for s_ in (a, b, qa, qb):
        s_.close()

    # ---- A-4: --expect-return, a qom-get group's return must equal ----
    # VALUE is JSON when it parses, else the raw string; the option
    # attaches to the latest --qmp-cmd group and is rejected elsewhere.
    qget_n = ('{"execute": "qom-get", "arguments": {"path": '
              '"/machine/stixctrl/attenuator", "property": '
              '"fsw-accesses"}}')
    qget_s = ('{"execute": "qom-get", "arguments": {"path": '
              '"/machine/stixctrl/attenuator", "property": '
              '"truth.position"}}')
    p = make_parser().parse_args([
        "--qmp", "/x/qmp.sock",
        "--send-at", "45", "--qmp-cmd", qget_n, "--expect-return", "0",
        "--send-at", "46", "--qmp-cmd", qget_s, "--expect-return", "out",
    ])
    groups, gate = setup_mode(p)
    assert gate is True
    assert groups[1]["expect_return"] == ("0", 0)
    assert groups[2]["expect_return"] == ("out", "out")
    assert groups[0].get("expect_return") is None
    # JSON values: numbers, bools; a string property takes the bare word
    assert parse_return_value("1280") == 1280
    assert parse_return_value("true") is True
    assert parse_return_value("out") == "out"
    assert parse_return_value('{"a": 1}') == {"a": 1}
    # attached to the connect group or to a --send-hex group: rejected
    for bad in (["--expect-return", "0"],
                ["--send-at", "5", "--send-hex", "40",
                 "--expect-return", "0"]):
        try:
            setup_mode(make_parser().parse_args(bad))
            raise AssertionError("--expect-return accepted: %s" % bad)
        except ValueError:
            pass
    # r1. the return equals: RETURN OK, the run passes at the deadline
    pargs = make_parser().parse_args(
        ["--hold", "--seconds", "1", "--window", "1",
         "--send-at", "0", "--qmp-cmd", qget_n, "--expect-return", "0"])
    groups, gate = setup_mode(pargs)
    qa, qb = socket.socketpair()
    qth, qbox = fake_qmp(qb, [ok_ret, b'{"return": 0}\r\n'])
    a, b = socket.socketpair()
    th, box = gate_in_thread(a, pargs, groups,
                             QmpClient(sock=qa, timeout=1.0))
    th.join(timeout=3)
    qth.join(timeout=3)
    assert not th.is_alive() and qbox.get("done") and box["rc"] == 0
    for s_ in (a, b, qa, qb):
        s_.close()
    # r2. a wrong return fails the run at once, before the deadline
    pargs = make_parser().parse_args(
        ["--hold", "--seconds", "5", "--window", "1",
         "--send-at", "0", "--qmp-cmd", qget_n, "--expect-return", "0"])
    groups, gate = setup_mode(pargs)
    qa, qb = socket.socketpair()
    qth, qbox = fake_qmp(qb, [ok_ret, b'{"return": 3}\r\n'])
    a, b = socket.socketpair()
    t_start = time.monotonic()
    th, box = gate_in_thread(a, pargs, groups,
                             QmpClient(sock=qa, timeout=1.0))
    th.join(timeout=3)
    qth.join(timeout=3)
    assert not th.is_alive() and box["rc"] == 1
    assert time.monotonic() - t_start < 0.9
    for s_ in (a, b, qa, qb):
        s_.close()

    # D-3 --dump-apid: whole packets of the dumped APID only, without
    # the SpW header, one hex line each, across split chunks, in both
    # handle_chunk (gate) use and the parser
    import io

    class KeepOpen(io.StringIO):
        def close(self):
            pass

    sinks = {}

    def opener(path, mode):
        assert mode == "a"
        sinks[path] = KeepOpen()
        return sinks[path]

    pargs = make_parser().parse_args(
        ["--dump-apid", "0x5DC:ql.hex", "--dump-apid", "1441:ack.hex"])
    dump = PacketDump(pargs.dump_apid, opener)
    assert sorted(dump.files) == [0x5A1, 0x5DC]
    # a 68-byte QL TM(21,6) unit (longer than the 48-byte log lines):
    # 64-byte CCSDS packet, length field 64 - 7 = 57
    ql_sp = bytes([0x0D, 0xDC, 0xC0, 0x07, 0x00, 57, 0x10, 21, 6, 0]) + \
        bytes(6) + bytes([0x1E]) + bytes(range(47))
    ql_unit = bytes([0x49, 0x02, 0x00, 0x00]) + ql_sp
    assert frame_units(bytearray(ql_unit))[0] == [ql_unit]
    pend = bytearray()
    stream = tm172 + ql_unit + tm11
    # split inside the QL unit: the dump sees it only once it is whole
    handle_chunk(stream[:30], [], pend, dump)
    assert sinks["ql.hex"].getvalue() == ""
    handle_chunk(stream[30:], [], pend, dump)
    ql_lines = sinks["ql.hex"].getvalue().splitlines()
    assert ql_lines == [ql_sp.hex()], ql_lines
    assert bytes.fromhex(ql_lines[0])[16] == 0x1E   # SSID at byte 16
    assert sinks["ack.hex"].getvalue().splitlines() == [tm11[4:].hex()]
    assert dump.write(tm172) is False               # 0x5A7: not dumped
    dump.close()
    for bad in ("0x5DC", "0x5DC:", "zz:f", "0x800:f"):
        try:
            PacketDump([bad], opener)
        except ValueError:
            continue
        raise AssertionError(f"--dump-apid {bad} accepted")
    # plain mode (run_plain) dumps too: the same stream over a
    # socketpair (two writes, cut inside the QL unit; recv may join
    # them), then a close ends the run
    sinks.clear()
    dump = PacketDump(["0x5DC:plain.hex"], opener)
    a, b = socket.socketpair()
    a.settimeout(0.2)
    b.sendall(stream[:30])
    b.sendall(stream[30:])
    b.close()
    rc = run_plain(a, time.monotonic() + 2.0, dump)
    a.close()
    assert rc == 0
    assert sinks["plain.hex"].getvalue().splitlines() == [ql_sp.hex()]
    dump.close()
    try:
        PacketDump(["0x5DC:a", "1500:b"], opener)
    except ValueError:
        pass
    else:
        raise AssertionError("--dump-apid: a duplicate APID accepted")

    # event stamps are CLOCK_MONOTONIC, not the (steppable) wall clock
    m1 = mono()
    assert re.fullmatch(r"\[m=\d+\.\d{3}\]", m1), m1
    assert abs(float(m1[3:-1]) - time.monotonic()) < 1.0, m1

    # q8. PL-2b: QMP over TCP. --qmp tcp:HOST:PORT connects with
    #     socket.create_connection (any address family) to a real
    #     listener on an ephemeral port; the same greeting/capabilities/
    #     command exchange as the unix form
    def tcp_case(family, bind_host, client_fmt):
        lst = socket.socket(family, socket.SOCK_STREAM)
        lst.bind((bind_host, 0))
        lst.listen(1)
        lst.settimeout(3)  # a client that never connects fails, not hangs
        tport = lst.getsockname()[1]
        tbox = {}

        def tcp_accept():
            try:
                peer, _ = lst.accept()
            except socket.timeout:
                return
            th_, b_ = fake_qmp(peer, [ok_ret, b'{"return": {"tcp": 1}}\r\n'])
            th_.join(timeout=3)
            tbox.update(b_)
            peer.close()
        tth = threading.Thread(target=tcp_accept)
        tth.start()
        c = QmpClient(client_fmt % tport, timeout=1.0)
        try:
            ret = c.execute(json.loads(qset))
        finally:
            c.close()
            tth.join(timeout=4)
            lst.close()
        assert not tth.is_alive() and ret == {"tcp": 1}, ret
        assert [m["execute"] for m in tbox["cmds"]] == \
            ["qmp_capabilities", "qom-set"]

    tcp_case(socket.AF_INET, "127.0.0.1", "tcp:127.0.0.1:%d")
    # q8b. "localhost" may resolve to ::1 first; create_connection tries
    #      every address, so an IPv4-only listener is still reached
    tcp_case(socket.AF_INET, "127.0.0.1", "tcp:localhost:%d")
    # q8c. a bracketed IPv6 loopback (skipped when the host has no ::1)
    if socket.has_ipv6:
        try:
            probe6 = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
            probe6.bind(("::1", 0))
            probe6.close()
            have6 = True
        except OSError:
            have6 = False
        if have6:
            tcp_case(socket.AF_INET6, "::1", "tcp:[::1]:%d")
        else:
            print("self-test: no IPv6 loopback, q8c skipped")
    # q9. the address forms: a unix path stays a path; tcp needs a host
    #     (an IPv6 one in brackets) and a port 1..65535
    assert qmp_address("/x/qmp.sock") == ("unix", "/x/qmp.sock")
    assert qmp_address("tcp:127.0.0.1:4444") == ("tcp", ("127.0.0.1", 4444))
    assert qmp_address("tcp:localhost:4444") == ("tcp", ("localhost", 4444))
    assert qmp_address("tcp:[::1]:4444") == ("tcp", ("::1", 4444))
    for bad in ("tcp:127.0.0.1", "tcp::4444", "tcp:127.0.0.1:x",
                "tcp:127.0.0.1:70000", "tcp:127.0.0.1:0", "tcp:::1:4444",
                "tcp:[::1:4444", "tcp:[]:4444", "tcp:[::1]x:4444"):
        try:
            qmp_address(bad)
            raise AssertionError("bad QMP address accepted: %s" % bad)
        except QmpError:
            pass

    print("SELF-TEST: PASS")
    return 0


def main():
    args = make_parser().parse_args()

    if args.self_test:
        return self_test()

    try:
        groups, gate = setup_mode(args)
        dump = PacketDump(args.dump_apid)
    except ValueError as e:
        print(f"bad arguments: {e}")
        return 2
    except OSError as e:
        print(f"bad arguments: --dump-apid: {e}")
        return 2
    qmp = None
    if any(g.get("qmp") is not None for g in groups):
        if not args.qmp:
            print("bad arguments: --qmp-cmd needs --qmp PATH")
            return 2
        try:
            qmp_address(args.qmp)
        except QmpError as e:
            print("bad arguments: --qmp: %s" % e)
            return 2
        qmp = QmpClient(args.qmp)

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
        return run_gate(sock, args, groups, qmp, dump)

    return run_plain(sock, deadline, dump)


if __name__ == "__main__":
    sys.exit(main())
