#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""ROTB fill drive (RF-1 + RF-3, rotb-fill plan
docs/stix-port/2026-11-14-rotb-fill-plan.md).

The TC/TM counterpart of scripts/stix/rotb-fill-run.sh: it connects to
the model's TMTC port, fires the schedule the wrapper wrote, samples
the guest clock and the ROTB fill level, and records everything the
RF-4 extraction needs.

Guest-clock schedule (Amendment A: tmtc-probe's --send-at is wall
time, which breaks under icount): the drive polls the virtual clock
over QMP,  qom-get /machine/stixctrl hw-state  ->  JSON string with
"vclock_ns" (grlib_stixctrl.c:1608), and a schedule item fires when
the guest seconds since the FIRST sample (the run's t0) reach its
"t".  The run ends at --guest-seconds GUEST seconds.  Under icount the
guest clock is disanchored from the wall clock in both directions, so
nothing here keys on wall time except the connect retry and the
stdout stamps.

Files, all under --out DIR:
  fill.csv     one row per sample: wall_mono, vclock_ns, rotb.count,
               rotb.full, rotb.rd, rotb.wr, rotb.capacity.  The rotb
               cells come from the hw-state "rotb" section; a binary
               without that section (or with renamed fields) leaves
               the cells empty, never fails the run.
  tm.log       one line per framed TM unit:
               "vclock_ns apid type subtype datahex" (vclock = the
               last sampled clock; datahex = the application data).
  events.log   one line per send/mark/qmp with the vclock, plus the
               first hw-state's schema and rotb section verbatim.

TC builders (RF-1): tc_param_set(index, value) / tc_param_get(index)
build TC(237,18)/(237,19) on APID 0x1DAC exactly like aspect-smoke.sh's
tc(): SpW header 40 02 00 00, CCSDS primary header 1D AC C0 seq,
length = len(app) + 5, PUS secondary header 19 <svc> <sub> 00,
application data, CRC-16/CCITT-FALSE over the CCSDS packet.  A set's
application data is index u16 BE + sub-id u8 0 + value i32 BE (the
aspect anchor: index 477, value 100 -> 01DD0000000064); a get's is
index u16 BE + sub-id u8 0.  The answer TM(237,20) arrives on APID
0x5A8 carrying index + sub-id + value (param_readback_expect() builds
the 0x5A8:237:20:... expectation hex).  The AX gate ELF's indices
(Amendment A, O-R1) are 312/313/314 for NOMINAL_ACCU_MIN_INTEGRATION /
MAX_INTEGRATION / MAX_COUNTS.

--emit-param-set INDEX VALUE SEQ / --emit-param-get INDEX SEQ print
one full TC hex and exit (rotb-fill-run.sh builds its schedule JSON
with these, so the builder lives here only).

Schedule (--schedule FILE): a JSON list of items, each
  {"t": GUEST_S, "tc": HEX}      a TC unit, sent on the TMTC socket
  {"t": GUEST_S, "qmp": {...}}   one QMP command (e.g. a qom-set)
  {"t": GUEST_S, "mark": TEXT}   an events.log marker only
Items fire in list order when their t is reached; equal t keeps list
order (the wrapper puts the det-flare-cps qom-set before det-flare
true at the same t).

--expect APID:TYPE:SUBTYPE[:DATAHEX] (repeatable) tracks the framed TM
stream with tmtc-probe's Expect; the run exits 1 at the end when any
expectation was never seen.  The verdict line is always last:
"DRIVE: PASS|FAIL (...)".

--self-test runs the host-side unit tests (schedule parsing, the CSV
writer, the TC builders against the nominal-smoke/detector-smoke full
units and the aspect-smoke 01DD0000000064 payload) and exits.

Exit codes: 0 pass, 1 fail (an expectation never seen, a QMP error,
the connection closed early), 2 bad use.
"""
import argparse
import importlib.util
import json
import os
import socket
import sys
import tempfile
import time

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "tmtc_probe", os.path.join(_here, "tmtc-probe.py"))
probe = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(probe)

# === TC builders (RF-1) ===

# Ported from aspect-smoke.sh's tc_units() python block (which ports
# nominal-smoke/elut-smoke): SpW unit prefix 40 02 00 00, then the
# CCSDS packet — primary header, PUS secondary header 19 <svc> <sub>
# 00, application data, CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF,
# no reflection) over the CCSDS packet.  elut-upload.py's
# crc16_ccitt_false/tc_unit are the same code path; this file cannot
# import elut-upload.py (no hyphen-safe loader benefit over ~20 lines)
# so the two functions are ported and anchored below.
SPW_HDR_HEX = "40020000"
APID_TC = 0x1DAC          # the TC APID word 1DAC (TM APID 0x5AC)
DFH_FLAGS = 0x19          # PUS data field header: both acks requested
SVC_MODE = 0xEC           # TC(236,x) mode control
SVC_PARAM = 0xED          # TC(237,x) parameter management
SUB_CONF = 1              # TC(236,1) 04 00 CONF / 05 00 NOMINAL
SUB_S20 = 6               # TC(236,6) 07 04 00 00 = S20 keep-alive 0
SUB_PARAM_SET = 18        # TC(237,18) set a parameter
SUB_PARAM_GET = 19        # TC(237,19) read a parameter back
PARAM_TM_TYPE = 237       # the read-back TM(237,20)
PARAM_TM_SUBTYPE = 20
PARAM_READBACK_APID = 0x5A8   # TM(237,20) and TM(238,3) ride 0x5A8

# The AX gate ELF's parameter indices (Amendment A, O-R1 resolution:
# the matrix runs on the AX build; the PFM block would shift the
# ordinals).  Still gated by the TC(237,19) read-back.
AX_MIN_INTEGRATION = 312   # NOMINAL_ACCU_MIN_INTEGRATION
AX_MAX_INTEGRATION = 313   # NOMINAL_ACCU_MAX_INTEGRATION
AX_MAX_COUNTS = 314        # NOMINAL_ACCU_MAX_COUNTS

# the known full units the builder must reproduce byte for byte
# (nominal-smoke.sh seq 1-3, detector-smoke.sh seq 5; the same four
# anchor aspect-smoke.sh's builder)
TC_ANCHORS = (
    ("400200001DACC001000719EC010004004AC1", 1, SVC_MODE, SUB_CONF,
     bytes([4, 0])),
    ("400200001DACC002000919EC060007040000AFBA", 2, SVC_MODE, SUB_S20,
     bytes([7, 4, 0, 0])),
    ("400200001DACC003000719EC01000500BF97", 3, SVC_MODE, SUB_CONF,
     bytes([5, 0])),
    ("400200001DACC005000719EC01000400D62E", 5, SVC_MODE, SUB_CONF,
     bytes([4, 0])),
)


def crc16_ccitt_false(data):
    """CRC-16/CCITT-FALSE over @data (poly 0x1021, init 0xFFFF)."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


def tc_unit(seq, service, subtype, app):
    """One full TC unit as an uppercase hex string (aspect-smoke's
    tc()): SpW header + CCSDS packet + CRC.  @seq is the 14-bit
    sequence count; the TM(1,1)/TM(1,7) acks echo it as 1DACC0<seq>."""
    if not 0 <= seq <= 0x3FFF:
        raise ValueError("seq 0x%X outside 14 bits" % seq)
    dfh = bytes([DFH_FLAGS, service, subtype, 0x00])
    length = len(dfh) + len(app) + 2 - 1   # CCSDS: data field bytes - 1
    sp = bytes([APID_TC >> 8, APID_TC & 0xFF,
                0xC0 | (seq >> 8), seq & 0xFF,
                length >> 8, length & 0xFF]) + dfh + bytes(app)
    crc = crc16_ccitt_false(sp)
    return SPW_HDR_HEX + (sp + bytes([crc >> 8, crc & 0xFF])).hex().upper()


# one sequence counter for the default-seq builders; rotb-fill-run.sh
# passes explicit seqs (its ack expectations echo them), so this only
# serves ad-hoc interactive use
_seq = [0]


def tc_reset_seq():
    _seq[0] = 0


def next_seq():
    _seq[0] = (_seq[0] % 0x3FFF) + 1
    return _seq[0]


def param_set_app(index, value):
    """TC(237,18) application data: index u16 BE, sub-id u8 0, value
    i32 BE (TcService237.c:66-94; the aspect anchor 477/100 ->
    01DD0000000064)."""
    if not 0 <= index <= 0xFFFF:
        raise ValueError("param index %d outside u16" % index)
    return index.to_bytes(2, "big") + bytes([0]) + \
        (value & 0xFFFFFFFF).to_bytes(4, "big")


def param_get_app(index):
    """TC(237,19) application data: index u16 BE, sub-id u8 0 (the
    aspect bytes 01 DD 00)."""
    if not 0 <= index <= 0xFFFF:
        raise ValueError("param index %d outside u16" % index)
    return index.to_bytes(2, "big") + bytes([0])


def tc_param_set(index, value, seq=None):
    """The full TC(237,18) hex setting @index to @value (i32 BE)."""
    return tc_unit(seq if seq is not None else next_seq(),
                   SVC_PARAM, SUB_PARAM_SET, param_set_app(index, value))


def tc_param_get(index, seq=None):
    """The full TC(237,19) hex reading @index back (TM(237,20))."""
    return tc_unit(seq if seq is not None else next_seq(),
                   SVC_PARAM, SUB_PARAM_GET, param_get_app(index))


def param_readback_expect(index, value):
    """The --expect hex of the TM(237,20) read-back of @index/@value:
    APID:TYPE:SUBTYPE:DATAHEX with DATAHEX = index u16 BE + sub-id 00
    + value i32 BE (aspect-smoke expects 0x5A8:237:20:01DD0000000064
    for the 477/100 set)."""
    return "0x%X:%d:%d:%s" % (PARAM_READBACK_APID, PARAM_TM_TYPE,
                              PARAM_TM_SUBTYPE,
                              param_set_app(index, value).hex())


# === schedule ===

def validate_schedule(items):
    """Validate the parsed schedule list, returning it sorted by t
    (stable, so equal-t items keep list order — the det-flare-cps
    qom-set must precede det-flare true at the same t).  Raises
    ValueError with an item-indexed message on anything malformed."""
    if not isinstance(items, list):
        raise ValueError("schedule must be a JSON list of items")
    out = []
    for i, item in enumerate(items):
        where = "schedule item %d" % i
        if not isinstance(item, dict):
            raise ValueError("%s is not an object" % where)
        t = item.get("t")
        if not isinstance(t, (int, float)) or isinstance(t, bool) \
                or t < 0:
            raise ValueError("%s needs a numeric t >= 0" % where)
        kinds = [k for k in ("tc", "qmp", "mark") if k in item]
        if len(kinds) != 1:
            raise ValueError("%s needs exactly one of tc/qmp/mark "
                             "(got: %s)" % (where, kinds or "none"))
        kind = kinds[0]
        if kind == "tc":
            if not isinstance(item["tc"], str):
                raise ValueError("%s: tc must be a hex string" % where)
            try:
                bytes.fromhex(item["tc"])
            except ValueError:
                raise ValueError("%s: tc is not hex: %r"
                                 % (where, item["tc"][:32]))
        elif kind == "qmp":
            cmd = item["qmp"]
            if not isinstance(cmd, dict) or "execute" not in cmd:
                raise ValueError("%s: qmp needs a JSON object with an "
                                 "\"execute\" key" % where)
        elif not isinstance(item["mark"], str):
            raise ValueError("%s: mark must be a string" % where)
        out.append({"t": float(t), "kind": kind,
                    "value": item[kind], "sent": False})
    out.sort(key=lambda it: it["t"])
    return out


def load_schedule(path):
    """Read and validate the schedule file (JSON list)."""
    try:
        with open(path) as f:
            items = json.load(f)
    except OSError as e:
        raise ValueError("cannot read schedule %s: %s" % (path, e))
    except ValueError as e:
        raise ValueError("schedule %s is not JSON: %s" % (path, e))
    return validate_schedule(items)


# === hw-state + CSV ===

HW_STATE_PATH = "/machine/stixctrl"
HW_STATE_PROP = "hw-state"
QOM_GET_HW_STATE = {"execute": "qom-get",
                    "arguments": {"path": HW_STATE_PATH,
                                  "property": HW_STATE_PROP}}
# the hw-state keys the fill.csv rotb cells come from; a binary without
# the "rotb" section (or with extra/renamed fields) leaves cells empty
ROTB_KEYS = ("count", "full", "rd", "wr", "capacity")


def parse_hw_state(reply):
    """(vclock_ns, rotb) from a qom-get hw-state reply.  The getter is
    a string property (grlib_stixctrl.c:1608), so the reply is the
    state's JSON text; @rotb is the "rotb" section's dict or None.
    Never raises on a missing section — only on a state without
    vclock_ns (that is unusable for the schedule)."""
    state = json.loads(reply) if isinstance(reply, str) else reply
    if not isinstance(state, dict) or "vclock_ns" not in state:
        raise ValueError("hw-state without vclock_ns: %r"
                         % (reply if not isinstance(reply, str)
                            else reply[:120]))
    vclock = int(state["vclock_ns"])
    rotb = state.get("rotb")
    return vclock, rotb if isinstance(rotb, dict) else None


def rotb_cell(rotb, key):
    """One CSV cell for @key from the rotb section: the value, or ""
    when the section/field is missing or of an unexpected type."""
    if rotb is None:
        return ""
    v = rotb.get(key)
    if isinstance(v, bool):
        return "1" if v else "0"
    if isinstance(v, (int, float)):
        return str(v)
    return ""


CSV_HEADER = ("wall_mono,vclock_ns,rotb.count,rotb.full,rotb.rd,"
              "rotb.wr,rotb.capacity")


def write_csv_row(f, wall_mono, vclock, rotb):
    """One fill.csv row: the clock pair plus the five rotb cells
    (empty when the hw-state has no usable rotb section)."""
    cells = [rotb_cell(rotb, k) for k in ROTB_KEYS]
    f.write("%.3f,%d,%s\n" % (wall_mono, vclock, ",".join(cells)))
    f.flush()


# === drive ===

class DriveFail(Exception):
    """A run-ending failure (QMP error, closed TMTC, unusable clock)."""


def connect_tmtc(port, timeout_s=30.0):
    """Connect to the model's TMTC port and send the 12-byte
    STIX_CONTROL handshake (D-K), retrying until @timeout_s (QEMU
    binds the port after the drive starts, as in tmtc-probe/pfm)."""
    deadline = time.monotonic() + timeout_s
    while True:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=2)
            s.sendall(b"STIX_CONTROL")
            s.settimeout(0.1)
            return s
        except OSError as e:
            if time.monotonic() >= deadline:
                raise DriveFail("could not connect to 127.0.0.1:%d: %s"
                                % (port, e))
            time.sleep(0.05)


def log_event(ev, vclock, text):
    ev.write("%d %s\n" % (vclock, text))
    ev.flush()


def fire_due(schedule, guest, vclock, sock, qmp, ev):
    """Send every not-yet-sent item whose t the guest clock passed,
    in list order; each send goes to events.log with the vclock."""
    for it in schedule:
        if it["sent"] or guest < it["t"]:
            continue
        it["sent"] = True
        stamp = "guest=%.3f" % guest
        if it["kind"] == "tc":
            sock.sendall(bytes.fromhex(it["value"]))
            log_event(ev, vclock, "send tc %s %s" % (it["value"], stamp))
            print("SEND tc %s %s" % (it["value"][:48], stamp), flush=True)
        elif it["kind"] == "qmp":
            ret = qmp.execute(it["value"])
            log_event(ev, vclock, "send qmp %s -> %s %s"
                      % (json.dumps(it["value"]), json.dumps(ret), stamp))
            print("SEND qmp %s -> %s %s"
                  % (json.dumps(it["value"])[:64], json.dumps(ret)[:64],
                     stamp), flush=True)
        else:
            log_event(ev, vclock, "mark %s %s" % (it["value"], stamp))
            print("MARK %s %s" % (it["value"], stamp), flush=True)


def handle_tm(data, pending, vclock, tm, expects):
    """Frame one received chunk; every whole unit goes to tm.log and
    against the expectations.  Returns (newly satisfied specs, the
    number of whole units the chunk carried)."""
    pending += data
    units, _ = probe.frame_units(pending)
    newly = []
    for unit in units:
        fields = probe.unit_fields(unit)
        if fields is None:
            continue
        apid, ptype, sub, app = fields
        tm.write("%d %d %d %d %s\n" % (vclock, apid, ptype, sub,
                                       app.hex()))
        tm.flush()
        for e in expects:
            if not e.ok and e.matches(unit):
                e.ok = True
                newly.append(e)
    return newly, len(units)


def run_drive(args):
    """The live run; returns the process exit code (0/1)."""
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    try:
        schedule = load_schedule(args.schedule)
    except ValueError as e:
        print("bad arguments: %s" % e)
        return 2
    late = [it for it in schedule if it["t"] >= args.guest_seconds]
    if late:
        print("WARNING: %d schedule item(s) fire at or after the run's"
              " end (%.1f guest s) and will never fire: %s"
              % (len(late), args.guest_seconds,
                 ["t=%.1f %s" % (it["t"], it["value"])
                  for it in late][:4]), flush=True)
    expects = []
    for spec in args.expect:
        try:
            expects.append(probe.Expect(spec))
        except ValueError as e:
            print("bad arguments: --expect %s: %s" % (spec, e))
            return 2
    with open(os.path.join(out, "fill.csv"), "w") as csv, \
            open(os.path.join(out, "tm.log"), "w") as tm, \
            open(os.path.join(out, "events.log"), "w") as ev:
        csv.write(CSV_HEADER + "\n")
        ev.write("# rotb-fill-drive guest_seconds=%s sample_every=%s\n"
                 % (args.guest_seconds, args.sample_every))
        ev.write("# schedule %s (%d items)\n"
                 % (os.path.abspath(args.schedule), len(schedule)))
        try:
            sock = connect_tmtc(args.port)
        except DriveFail as e:
            print("DRIVE: FAIL (%s)" % e)
            return 1
        qmp = probe.QmpClient(args.qmp, timeout=5.0)
        print("# connected to 127.0.0.1:%d [m=%.3f]"
              % (args.port, time.monotonic()), flush=True)
        v0 = None
        last_row = None
        pending = bytearray()
        tm_units = 0
        fail = None
        while fail is None:
            try:
                vclock, rotb = parse_hw_state(qmp.execute(QOM_GET_HW_STATE))
            except probe.QmpError as e:
                fail = "QMP ERROR: %s" % e
                break
            except ValueError as e:
                fail = "hw-state unusable: %s" % e
                break
            if v0 is None:
                v0 = vclock
                ev.write("# hw-state vclock_ns=%d rotb=%s\n"
                         % (vclock, json.dumps(rotb)))
                ev.flush()
                write_csv_row(csv, time.monotonic(), vclock, rotb)
                last_row = vclock
                print("SAMPLE t0 vclock_ns=%d rotb=%s"
                      % (vclock, rotb_cell(rotb, "count") or "n/a"),
                      flush=True)
            guest = (vclock - v0) / 1e9
            if vclock - last_row >= args.sample_every * 1e9:
                write_csv_row(csv, time.monotonic(), vclock, rotb)
                last_row = vclock
                print("SAMPLE guest=%.3f vclock_ns=%d rotb.count=%s"
                      % (guest, vclock, rotb_cell(rotb, "count") or "?"),
                      flush=True)
            try:
                fire_due(schedule, guest, vclock, sock, qmp, ev)
            except probe.QmpError as e:
                fail = "QMP ERROR: %s" % e
                break
            except OSError as e:
                fail = "send failed: %s" % e
                break
            try:
                data = sock.recv(65536)
            except socket.timeout:
                data = None
            except OSError as e:
                fail = "TMTC socket error: %s" % e
                break
            if data:
                newly, n_units = handle_tm(data, pending, vclock, tm,
                                           expects)
                tm_units += n_units
                for e in newly:
                    print("EXPECT OK: %s" % e, flush=True)
            elif data is not None:
                fail = ("the TMTC connection closed at guest=%.3f"
                        % guest)
                break
            if guest >= args.guest_seconds:
                write_csv_row(csv, time.monotonic(), vclock, rotb)
                break
        sock.close()
        qmp.close()
        if fail is None:
            print("RUN guest=%.3f vclock_ns=%d samples_done" %
                  ((vclock - v0) / 1e9, vclock), flush=True)
        missing = [str(e) for e in expects if not e.ok]
        for spec in missing:
            print("EXPECT NEVER SEEN: %s" % spec, flush=True)
        if fail is not None:
            print("DRIVE: FAIL (%s)" % fail)
            return 1
        if missing:
            print("DRIVE: FAIL (%d of %d expectation(s) never seen)"
                  % (len(missing), len(expects)))
            return 1
        print("DRIVE: PASS (guest %.1f s, %d tm units, fill.csv/tm.log/"
              "events.log in %s)" % ((vclock - v0) / 1e9, tm_units, out))
        return 0


# === self-test ===

def tm_unit(apid, ptype, sub, app):
    """A framed TM unit, as the model sends it (pfm-boot-drive's
    helper): SpW header + CCSDS + 10-byte TM data field header, so the
    application data sits at SP[16] (probe.unit_fields' TM offset)."""
    sp = bytes([apid >> 8, apid & 0xFF, 0xC0, 0, 0, 0]) \
        + bytes([0x10, ptype, sub]) + bytes(7) + app
    n = len(sp) - 7
    sp = sp[:4] + bytes([n >> 8, n & 0xFF]) + sp[6:]
    return b"\x49\x02\x00\x00" + sp


def self_test():
    bad = 0

    def check(name, ok, detail=""):
        nonlocal bad
        bad += not ok
        print("%s: %s%s" % ("ok" if ok else "FAIL", name,
                            (": " + detail) if detail and not ok else ""))

    # 1. the CRC against the standard check value
    check("crc16 check value", crc16_ccitt_false(b"123456789") == 0x29B1)

    # 2. the tc() path reproduces the four known full units
    for known, seq, svc, sub, app in TC_ANCHORS:
        got = tc_unit(seq, svc, sub, app)
        check("tc anchor seq %d" % seq, got == known,
              "got %s want %s" % (got, known))

    # 3. the RF-1 payload anchor: index 477, value 100 must reproduce
    #    aspect-smoke's 01DD0000000064 inside the TC (app data at hex
    #    [28:42]: 4 SpW + 4 CCSDS hdr + 2 length + 4 PUS header), with
    #    the same length field and PUS secondary header that smoke's
    #    TC(237,18) carries; the CRC bytes are anchored by 2 above.
    u = tc_param_set(477, 100, seq=9)
    check("237,18 length field", u[16:20] == "000C", u)
    check("237,18 PUS header", u[20:28] == "19ED1200", u)
    check("aspect payload 01DD0000000064", u[28:42] == "01DD0000000064",
          u)
    #    the framing around it, byte for byte, minus the CRC
    check("237,18 frame", u[:28] == "400200001DACC009000C19ED1200"
          and len(u) == 46, u)

    # 4. the read-back TC(237,19): app 01DD00, len 8
    u = tc_param_get(477, seq=10)
    check("237,19 length field", u[16:20] == "0008", u)
    check("237,19 PUS header", u[20:28] == "19ED1300", u)
    check("237,19 app 01DD00", u[28:34] == "01DD00", u)

    # 5. default sequence handling: an auto-incrementing 14-bit count
    tc_reset_seq()
    seqs = [tc_param_set(1, 1)[12:16], tc_param_get(2)[12:16],
            tc_param_set(3, 4)[12:16]]
    check("default seq 1,2,3", seqs == ["C001", "C002", "C003"], seqs)

    # 6. the read-back expectation hex, matched end to end through the
    #    probe's Expect over a real TM(237,20) unit on 0x5A8
    e = param_readback_expect(AX_MIN_INTEGRATION, 4)
    check("readback expect hex", e == "0x5A8:237:20:01380000000004", e)
    good = tm_unit(PARAM_READBACK_APID, PARAM_TM_TYPE, PARAM_TM_SUBTYPE,
                   bytes.fromhex("01380000000004") + b"\x00\x00")
    check("readback expect matches TM(237,20)",
          probe.Expect(e).matches(good))
    check("readback expect rejects a wrong value",
          not probe.Expect(e).matches(
              tm_unit(PARAM_READBACK_APID, PARAM_TM_TYPE,
                      PARAM_TM_SUBTYPE,
                      bytes.fromhex("01380000000009") + b"\x00\x00")))

    # 7. schedule parsing: sorting is stable (equal t keeps list
    #    order), kinds are normalised, sent starts False
    sched = validate_schedule([
        {"t": 60, "tc": "40"},
        {"t": 55, "mark": "params"},
        {"t": 75, "qmp": {"execute": "qom-set",
                          "arguments": {"path": "/machine/stixctrl",
                                        "property": "det-flare-cps",
                                        "value": 2000}}},
        {"t": 75, "qmp": {"execute": "qom-set",
                          "arguments": {"path": "/machine/stixctrl",
                                        "property": "det-flare",
                                        "value": True}}},
        {"t": 0, "mark": "start"},
    ])
    check("schedule length", len(sched) == 5)
    check("schedule stable sort",
          [(it["t"], it["kind"]) for it in sched] ==
          [(0.0, "mark"), (55.0, "mark"), (60.0, "tc"),
           (75.0, "qmp"), (75.0, "qmp")],
          str([(it["t"], it["kind"]) for it in sched]))
    check("equal t keeps list order (cps before flare)",
          sched[3]["value"]["arguments"]["property"] == "det-flare-cps"
          and sched[4]["value"]["arguments"]["property"] == "det-flare")
    check("schedule not sent", all(not it["sent"] for it in sched))
    for bad_items, why in (
            ({"t": 1, "tc": "40"}, "a JSON object, not a list"),
            ([{"tc": "40"}], "missing t"),
            ([{"t": -1, "tc": "40"}], "negative t"),
            ([{"t": 1}], "no kind"),
            ([{"t": 1, "tc": "40", "mark": "x"}], "two kinds"),
            ([{"t": 1, "tc": "zz"}], "bad tc hex"),
            ([{"t": 1, "qmp": {"arguments": {}}}], "qmp without execute"),
            ([{"t": 1, "mark": 5}], "non-string mark"),
            ([["t", 1]], "a list item, not an object")):
        try:
            validate_schedule(bad_items)
            check("schedule rejects %s" % why, False)
        except ValueError:
            check("schedule rejects %s" % why, True)

    # 8. the CSV writer: header, empty rotb cells, filled cells (full
    #    as 1/0), extra fields ignored
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "fill.csv")
        with open(path, "w") as f:
            f.write(CSV_HEADER + "\n")
            write_csv_row(f, 12.5, 1234, None)
            write_csv_row(f, 13.5, 2234,
                          {"count": 2131, "full": True, "rd": 5,
                           "wr": 49, "capacity": 104448,
                           "unexpected": "ignored"})
        lines = open(path).read().splitlines()
        check("csv header", lines[0] == CSV_HEADER, lines[0])
        check("csv row without rotb",
              lines[1] == "12.500,1234,,,,,", lines[1])
        check("csv row with rotb",
              lines[2] == "13.500,2234,2131,1,5,49,104448", lines[2])

    # 9. hw-state parsing: a string reply (the str getter), a missing
    #    rotb section -> None, a missing vclock -> ValueError
    v, r = parse_hw_state('{"schema": 4, "vclock_ns": 1000}')
    check("hw-state vclock", v == 1000 and r is None)
    v, r = parse_hw_state('{"vclock_ns": 2, "rotb": {"count": 3}}')
    check("hw-state rotb", v == 2 and r == {"count": 3})
    try:
        parse_hw_state('{"schema": 4}')
        check("hw-state without vclock raises", False)
    except ValueError:
        check("hw-state without vclock raises", True)

    print("SELF-TEST: " + ("PASS" if bad == 0 else "FAIL (%d)" % bad))
    return 1 if bad else 0


# === CLI ===

def make_parser():
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=20000 + os.getuid(),
                    help="TMTC server port (default: 20000+getuid)")
    ap.add_argument("--qmp", metavar="tcp:HOST:PORT",
                    help="QEMU QMP socket (tcp:HOST:PORT, IPv6 host "
                         "bracketed) for the guest clock: qom-get "
                         "/machine/stixctrl hw-state")
    ap.add_argument("--schedule", metavar="FILE",
                    help="schedule JSON: a list of {t, tc|qmp|mark} "
                         "items, t in guest seconds")
    ap.add_argument("--guest-seconds", type=float, default=None,
                    metavar="N", help="end of run, in GUEST seconds")
    ap.add_argument("--sample-every", type=float, default=1.0,
                    metavar="S",
                    help="one fill.csv row every S guest seconds "
                         "(default 1.0)")
    ap.add_argument("--out", metavar="DIR",
                    help="output dir for fill.csv, tm.log, events.log")
    ap.add_argument("--expect", action="append", default=[],
                    metavar="APID:TYPE:SUBTYPE[:DATAHEX]",
                    help="TM the run must have seen by the end "
                         "(repeatable; exit 1 listing any never seen)")
    ap.add_argument("--emit-param-set", nargs=3, type=int,
                    metavar=("INDEX", "VALUE", "SEQ"),
                    help="print the full TC(237,18) hex and exit "
                         "(rotb-fill-run.sh builds its schedule with "
                         "this)")
    ap.add_argument("--emit-param-get", nargs=2, type=int,
                    metavar=("INDEX", "SEQ"),
                    help="print the full TC(237,19) hex and exit")
    ap.add_argument("--self-test", action="store_true",
                    help="run the host-side unit tests and exit")
    return ap


def main():
    args = make_parser().parse_args()
    if args.self_test:
        return self_test()
    try:
        if args.emit_param_set is not None:
            index, value, seq = args.emit_param_set
            print(tc_param_set(index, value, seq))
            return 0
        if args.emit_param_get is not None:
            index, seq = args.emit_param_get
            print(tc_param_get(index, seq))
            return 0
    except ValueError as e:
        print("bad arguments: %s" % e)
        return 2
    missing = [flag for flag, val in
               (("--qmp", args.qmp), ("--schedule", args.schedule),
                ("--guest-seconds", args.guest_seconds),
                ("--out", args.out)) if val is None]
    if missing:
        print("bad arguments: a live run needs %s (or --self-test / "
              "--emit-param-*)" % ", ".join(missing))
        return 2
    if args.sample_every <= 0 or args.guest_seconds <= 0:
        print("bad arguments: --sample-every and --guest-seconds must "
              "be > 0")
        return 2
    return run_drive(args)


if __name__ == "__main__":
    sys.exit(main())
