#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""TM/TC driver of pfm-boot-smoke.sh (flash latency plan F-5).

The PFM flight build boots far slower than the AX gate ELF (its
filesystem scan reads 130,800 pages, about 60 s of guest time with the
latency model on), and the TC20 boot timer fires a fixed time after FDIR
is on. So, unlike tmtc-probe.py, whose sends sit at fixed offsets from
the connect, this driver anchors the TC schedule on TM arrivals
(tmtc-probe.py's --send-at cannot express that, and a probe restart per
phase would drop the connection between phases):

  connect group (offsets from the connect, CLOCK_MONOTONIC; measured
  on the release ELF with latency on: 0x0022 at +0.4 s, 0x14A3 at
  +16 s, 0x142E at +32 s, FS init still running at +46 s — FDIR and
  the TC20 timer are NOT gated by FS init):
    TM(5,1) 0022... (0x0022 Application SW boot success) by @20, and
                    before 0x0032 (the witness that the FS task yields)
    TM(5,1) 14a3    (0x14A3 FDIR protection enabled) by @40
    TM(5,1) 142e    (0x142E Preparing for shutdown, the TC20 boot
                    timer) by 0x14A3 + 25 s, capped @160
    TM(5,1) 0032 01 (0x0032 Filesystem initialisation finished) by @150
    TM(5,3) 8030    (0x8030 Context file(s) not found, fresh flash) @150
  NOMINAL (unless --no-nominal), anchored on the LATER of 0x142E and
  0x0032 (the mode change waits on the filesystem/context):
    CONF at max(0x142E, 0x0032) + 5 s, S20=0 at CONF + 10 s, NOMINAL at
    CONF + 20 s
    CONF:  TM(1,1), TM(5,1) 0021 02 04, TM(1,7)    by CONF + 10 s
    S20:   TM(1,1), TM(1,7)                        by S20 + 10 s
    NOM:   TM(1,1), TM(5,1) 0021 04 05, TM(1,7), SID2 0x5A4 with the
           mode nibble 5                            by NOMINAL + 5 s
    forbidden from CONF on: 0021 05 02, 0021 04 02, any TM(1,8)
  forbidden from the connect on: TM(5,3) 8021 (0x8021 Watchdog failure
    detected)
  liveness: from 0x0032 to the end, at least one HK TM(3,25) on 0x5E5
    per 2 s on average.
  The deadline checks run on every tick before any send is scheduled,
  so an expired boot window is reported even while CONF is still
  waiting for its anchor.

The TC bytes are nominal-smoke.sh's. Every TM line carries its offset
from the connect and the [m=...] monotonic stamp, and goes to stdout
(HK included, so the log is a complete record). "PFM 0x0032 at +S s"
marks FS init; one verdict line, "DRIVE: PASS" or "DRIVE: FAIL: <why>",
comes last. Exit 0 / 1, 2 on bad use.

--replay LOG re-judges such a log (this exact line format) through the
same checks: the TC sends are re-scheduled by the checker at the same
offsets, so the log's SEND lines carry no information and are ignored.
"""
import argparse
import importlib.util
import os
import re
import socket
import sys
import time

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "tmtc_probe", os.path.join(_here, "tmtc-probe.py"))
probe = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(probe)

TC_CONF = "40 02 00 00 1D AC C0 01 00 07 19 EC 01 00 04 00 4A C1"
TC_S20 = "40 02 00 00 1D AC C0 02 00 09 19 EC 06 00 07 04 00 00 AF BA"
TC_NOM = "40 02 00 00 1D AC C0 03 00 07 19 EC 01 00 05 00 BF 97"

BOOT_BY = 20.0
FDIR_BY = 40.0
TC20_AFTER_FDIR = 25.0
TC20_CAP = 160.0
FS_BY = 150.0
CTX_BY = 150.0
CONF_AFTER_ANCHOR = 5.0
S20_AFTER_CONF = 10.0
NOM_AFTER_CONF = 20.0
CONF_WINDOW = 10.0
S20_WINDOW = 10.0
NOM_WINDOW = 5.0


def spec(apid, ptype, sub, data=""):
    return (apid, ptype, sub, bytes.fromhex(data))


def matches(s, fields):
    apid, ptype, sub, app = fields
    return (apid, ptype, sub) == s[:3] and app[:len(s[3])] == s[3]


EV_BOOT = spec(0x5A7, 5, 1, "002200000008000000")
EV_FS = spec(0x5A7, 5, 1, "003201")
EV_CTX = spec(0x5A7, 5, 3, "8030")
EV_FDIR = spec(0x5A7, 5, 1, "14a3")
EV_TC20 = spec(0x5A7, 5, 1, "142e")
EV_HK = spec(0x5E5, 3, 25)
FORBID_ALWAYS = [("TM(5,3) 0x8021 (Watchdog failure detected)",
                  spec(0x5A7, 5, 3, "8021"))]
FORBID_NOMINAL = [
    ("0021 05 02", spec(0x5A7, 5, 1, "00210502")),
    ("0021 04 02", spec(0x5A7, 5, 1, "00210402")),
    ("TM(1,8)", spec(0x5A1, 1, 8)),
]
GROUPS = {
    "CONF": (TC_CONF, CONF_WINDOW, [
        ("TM(1,1) CONF", spec(0x5A1, 1, 1, "1DACC001")),
        ("0021 02 04", spec(0x5A7, 5, 1, "00210204")),
        ("TM(1,7) CONF", spec(0x5A1, 1, 7, "1DACC001")),
    ]),
    "S20": (TC_S20, S20_WINDOW, [
        ("TM(1,1) S20", spec(0x5A1, 1, 1, "1DACC002")),
        ("TM(1,7) S20", spec(0x5A1, 1, 7, "1DACC002")),
    ]),
    "NOMINAL": (TC_NOM, NOM_WINDOW, [
        ("TM(1,1) NOMINAL", spec(0x5A1, 1, 1, "1DACC003")),
        ("0021 04 05", spec(0x5A7, 5, 1, "00210405")),
        ("TM(1,7) NOMINAL", spec(0x5A1, 1, 7, "1DACC003")),
        ("SID2 mode 5", spec(0x5A4, 3, 25, "0215")),
    ]),
}


class Checker:
    """The gate's rules over (offset, TM fields) events, no I/O.

    feed() takes one TM unit's fields at offset t (seconds since the
    connect); tick(t) checks the deadlines and returns the TC sends that
    are due as (group, hex) pairs; finish(t) applies the end-of-run
    checks. self.fail is None or the first failure."""

    def __init__(self, nominal=True):
        self.nominal = nominal
        self.seen = {}
        self.hk_after_fs = 0
        self.fail = None
        self.sends = {}      # group -> planned offset
        self.sent = {}       # group -> actual offset
        self.got = {g: set() for g in GROUPS}

    def _fail(self, why):
        if self.fail is None:
            self.fail = why

    def _plan(self):
        """CONF at max(0x142E, 0x0032) + 5 s: the mode change waits on
        the filesystem/context, so both must be in (either order)."""
        if "fs" in self.seen and "tc20" in self.seen:
            conf = max(self.seen["fs"], self.seen["tc20"]) \
                + CONF_AFTER_ANCHOR
            self.sends = {"CONF": conf,
                          "S20": conf + S20_AFTER_CONF,
                          "NOMINAL": conf + NOM_AFTER_CONF}

    def feed(self, t, fields):
        for name, s in FORBID_ALWAYS:
            if matches(s, fields):
                self._fail(f"forbidden {name} at +{t:.1f} s")
        if "CONF" in self.sent:
            for name, s in FORBID_NOMINAL:
                if matches(s, fields):
                    self._fail(f"forbidden {name} at +{t:.1f} s")
        for key, s in (("boot", EV_BOOT), ("fs", EV_FS), ("ctx", EV_CTX),
                       ("fdir", EV_FDIR), ("tc20", EV_TC20)):
            if key not in self.seen and matches(s, fields):
                self.seen[key] = t
                if key == "fs" and "boot" not in self.seen:
                    self._fail("0x0032 (Filesystem initialisation"
                               " finished) arrived before the boot report"
                               " 0x0022: FILE did not yield during the"
                               " scan")
                if self.nominal:
                    self._plan()
        if "fs" in self.seen and matches(EV_HK, fields):
            self.hk_after_fs += 1
        for g, (_, _, exps) in GROUPS.items():
            if g in self.sent:
                for name, s in exps:
                    if matches(s, fields):
                        self.got[g].add(name)

    def tick(self, t):
        s = self.seen
        if "boot" not in s and t > BOOT_BY:
            self._fail(f"no boot report 0x0022 (Application SW boot"
                       f" success) by @{BOOT_BY:.0f}")
        if "fs" not in s and t > FS_BY:
            self._fail(f"no 0x0032 (Filesystem initialisation finished)"
                       f" by @{FS_BY:.0f}")
        if "ctx" not in s and t > CTX_BY:
            self._fail(f"no 0x8030 (Context file(s) not found) by"
                       f" @{CTX_BY:.0f}")
        if "fdir" not in s and t > FDIR_BY:
            self._fail(f"no 0x14A3 (FDIR protection enabled) by"
                       f" @{FDIR_BY:.0f}")
        if "tc20" not in s:
            rel = s["fdir"] + TC20_AFTER_FDIR if "fdir" in s else TC20_CAP
            if t > min(rel, TC20_CAP):
                if rel >= TC20_CAP:
                    self._fail(f"no 0x142E (Preparing for shutdown) by"
                               f" @{TC20_CAP:.0f}")
                else:
                    self._fail(f"no 0x142E (Preparing for shutdown) by"
                               f" 0x14A3 + {TC20_AFTER_FDIR:.0f} s")
        for g, (_, window, exps) in GROUPS.items():
            if g in self.sent and t > self.sent[g] + window:
                missing = [n for n, _ in exps if n not in self.got[g]]
                if missing:
                    self._fail(f"{g}: no {', '.join(missing)} within"
                               f" {window:.0f} s")
        due = []
        if self.fail is None:
            for g in ("CONF", "S20", "NOMINAL"):
                if g in self.sends and g not in self.sent \
                        and t >= self.sends[g]:
                    self.sent[g] = t
                    due.append((g, GROUPS[g][0]))
        return due

    def finish(self, t):
        self.tick(t)
        if self.nominal and "NOMINAL" not in self.sent:
            self._fail("the run ended before NOMINAL was commanded")
        if "fs" in self.seen:
            span = t - self.seen["fs"]
            need = int(span // 2)
            if self.hk_after_fs < need:
                self._fail(f"HK liveness: {self.hk_after_fs} TM(3,25) on"
                           f" 0x5E5 in {span:.0f} s after 0x0032"
                           f" (need {need})")


def tm_line(t, fields):
    apid, ptype, sub, app = fields
    return (f"TM +{t:7.2f} s 0x{apid:03X} ({ptype},{sub})"
            f" {app[:12].hex()} [m={time.monotonic():.3f}]")


def drive_line(chk, t, fields):
    """Print the TM, then the FS-init mark; feed the checker."""
    print(tm_line(t, fields), flush=True)
    had_fs = "fs" in chk.seen
    chk.feed(t, fields)
    if not had_fs and "fs" in chk.seen:
        print(f"PFM 0x0032 at +{t:.1f} s", flush=True)


def run(sock, seconds, nominal):
    chk = Checker(nominal)
    t0 = time.monotonic()
    buf = bytearray()
    sock.settimeout(0.2)
    while True:
        now = time.monotonic()
        t = now - t0
        if t >= seconds:
            break
        try:
            data = sock.recv(65536)
            if not data:
                chk._fail(f"the TMTC connection closed at +{t:.1f} s")
                break
            buf.extend(data)
        except socket.timeout:
            pass
        units, _ = probe.frame_units(buf)
        for u in units:
            fields = probe.unit_fields(u)
            if fields is None:
                continue
            drive_line(chk, time.monotonic() - t0, fields)
        for g, hx in chk.tick(time.monotonic() - t0):
            sock.sendall(bytes.fromhex(hx))
            print(f"SEND {g} at +{time.monotonic() - t0:.2f} s"
                  f" [m={time.monotonic():.3f}]", flush=True)
        if chk.fail is not None:
            break
    if chk.fail is None:
        chk.finish(time.monotonic() - t0)
    return chk


TM_LINE = re.compile(
    r"^TM \+\s*([0-9.]+) s 0x([0-9A-Fa-f]+) \((\d+),(\d+)\) ?([0-9a-f]*)")


def replay(lines, seconds, nominal):
    """Judge a driver log (run()'s TM lines) through the same Checker.

    tick() runs on a 0.5 s grid, as in the self-test, so a deadline one
    tick past its window is caught even when the log's next event is far
    away. The log's SEND lines are ignored (the checker re-schedules the
    sends at the same offsets); its TM lines are the record judged."""
    chk = Checker(nominal)
    ev = []
    for line in lines:
        m = TM_LINE.match(line)
        if not m:
            continue
        app = bytes.fromhex(m.group(5)) if m.group(5) else b""
        ev.append((float(m.group(1)),
                   (int(m.group(2), 16), int(m.group(3)), int(m.group(4)),
                    app)))
    ev.sort(key=lambda e: e[0])
    t = 0.0
    while t <= seconds and chk.fail is None:
        while ev and ev[0][0] <= t:
            drive_line(chk, *ev.pop(0))
        for g, _ in chk.tick(t):
            # the send the checker schedules, echoed as the live driver
            # would print it
            print(f"SEND {g} at +{t:.2f} s", flush=True)
        t = round(t + 0.5, 1)
    if chk.fail is None:
        chk.finish(seconds)
    return chk


def tm_unit(apid, ptype, sub, app):
    """A framed TM unit, as the model sends it (self-test helper)."""
    sp = bytes([apid >> 8, apid & 0xFF, 0xC0, 0, 0, 0]) \
        + bytes([0x10, ptype, sub]) + bytes(7) + app
    n = len(sp) - 7
    sp = sp[:4] + bytes([n >> 8, n & 0xFF]) + sp[6:]
    return b"\x00\x02\x00\x00" + sp


def self_test():
    def fields(s):
        u = tm_unit(s[0], s[1], s[2], s[3] + b"\x00\x00")
        units, _ = probe.frame_units(bytearray(u))
        return probe.unit_fields(units[0])

    def replay(events, end, nominal=True, mute=()):
        """events: (t, spec); sends are answered by the matching group's
        expectations one second later"""
        chk = Checker(nominal)
        ev = sorted(events, key=lambda e: e[0])
        t = 0.0
        while t <= end and chk.fail is None:
            while ev and ev[0][0] <= t:
                chk.feed(ev[0][0], fields(ev.pop(0)[1]))
            for g, _ in chk.tick(t):
                for n, s in GROUPS[g][2]:
                    if n not in mute:
                        ev.append((t + 1.0, s))
                ev.sort(key=lambda e: e[0])
            t = round(t + 0.5, 1)
        if chk.fail is None:
            chk.finish(end)
        return chk

    def boot(fs=70.0, boot_t=1.0, fdir=16.0, tc20=32.0, extra=()):
        # the measured release-ELF order: FDIR and the TC20 timer are
        # NOT gated by FS init (0x14A3 at +16 s, 0x142E at +32 s, FS
        # init at +70 s)
        ev = [(boot_t, EV_BOOT), (fdir, EV_FDIR), (tc20, EV_TC20),
              (fs, EV_FS), (fs, EV_CTX)]
        t = fs + 1
        while t < 240:
            ev.append((t, EV_HK))
            t += 1.0
        return ev + list(extra)

    cases = [
        ("pass", boot(), 240, True, None),
        ("pass --no-nominal", boot(), 240, False, None),
        ("no boot report", [e for e in boot() if e[1] is not EV_BOOT], 240,
         True, "no boot report 0x0022"),
        ("0x0032 before the boot report", boot(fs=10.0, boot_t=15.0), 240,
         True, "0x0032 (Filesystem initialisation finished) arrived"
               " before the boot report 0x0022"),
        ("boot report late", boot(boot_t=25.0), 240, True,
         "no boot report 0x0022"),
        ("fs too late", boot(fs=155.0), 240, True,
         "no 0x0032 (Filesystem initialisation finished) by @150"),
        ("no 14a3", [e for e in boot() if e[1] is not EV_FDIR], 240, True,
         "no 0x14A3 (FDIR protection enabled) by @40"),
        ("no 8030", [e for e in boot() if e[1] is not EV_CTX], 240, True,
         "no 0x8030"),
        ("8021", boot(extra=[(100.0, spec(0x5A7, 5, 3, "8021"))]), 240,
         True, "forbidden TM(5,3) 0x8021"),
        ("fallback", boot(extra=[(100.0, spec(0x5A7, 5, 1, "00210502"))]),
         240, True, "forbidden 0021 05 02"),
        ("no 142e", [e for e in boot() if e[1] is not EV_TC20], 240, True,
         "no 0x142E"),
        # No "142e cap" case: 0x14A3 must come by @40, so 0x142E is due
        # by @65 and the @160 cap is only a backstop that cannot fire.
        ("hk stops", [e for e in boot() if not (e[1] is EV_HK and
                                                e[0] > 70)], 240, True,
         "HK liveness"),
    ]
    bad = 0
    cases += [
        ("NOMINAL not reached", boot(), 240, True,
         "NOMINAL: no 0021 04 05 within 5 s", ("0021 04 05",)),
        ("CONF not acked", boot(), 240, True,
         "CONF: no TM(1,7) CONF within 10 s", ("TM(1,7) CONF",)),
    ]
    for case in cases:
        name, ev, end, nominal, want = case[:5]
        chk = replay(ev, end, nominal, case[5] if len(case) > 5 else ())
        ok = chk.fail is None if want is None else \
            (chk.fail is not None and want in chk.fail)
        if name == "pass" and ok:
            # CONF anchored on max(0x142E, 0x0032) = 70 -> +5 s
            ok = set(chk.sent) == {"CONF", "S20", "NOMINAL"} and \
                abs(chk.sent["CONF"] - 75.0) < 0.6
        print(f"{'ok' if ok else 'FAIL'}: {name}: {chk.fail}")
        bad += not ok
    print("SELF-TEST: " + ("PASS" if bad == 0 else f"FAIL ({bad})"))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=20000 + os.getuid())
    ap.add_argument("--seconds", type=float, default=240.0)
    ap.add_argument("--no-nominal", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--replay", metavar="LOG",
                    help="judge a driver log instead of connecting")
    args = ap.parse_args()
    if args.self_test:
        return self_test()

    if args.replay:
        with open(args.replay) as f:
            chk = replay(f, args.seconds, not args.no_nominal)
        if chk.fail is None:
            print("DRIVE: PASS")
            return 0
        print(f"DRIVE: FAIL: {chk.fail}")
        return 1

    deadline = time.monotonic() + 30
    sock = None
    while sock is None and time.monotonic() < deadline:
        try:
            sock = socket.create_connection(("127.0.0.1", args.port),
                                            timeout=2)
        except OSError:
            time.sleep(0.5)
    if sock is None:
        print(f"DRIVE: FAIL: could not connect to 127.0.0.1:{args.port}")
        return 1
    sock.sendall(b"STIX_CONTROL")  # the 12-byte handshake (D-K)
    print(f"# connected to 127.0.0.1:{args.port} [m={time.monotonic():.3f}]",
          flush=True)
    chk = run(sock, args.seconds, not args.no_nominal)
    sock.close()
    if chk.fail is None:
        print("DRIVE: PASS")
        return 0
    print(f"DRIVE: FAIL: {chk.fail}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
