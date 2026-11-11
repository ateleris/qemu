#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A fake qemu-system-sparc for the controller's self-test (no QEMU).

  fake_qemu.py [--variant new|old|noquit] <qemu args>

-M none -qmp stdio: the binary-guard probe; `old` answers
qom-list-properties for grlib-stixflash WITHOUT inverted-storage.
Otherwise it serves QMP on the -qmp unix socket like a frozen (-S)
leon3: cont/stop/quit, the RESUME/STOP/SHUTDOWN/RESET events, qom-get
vclock-ns, obt and both hw-state JSONs, qom-set obt, and the test-only
x-fake-guest-reset, which behaves per -no-reboot / -action like QEMU.
`noquit` ignores quit (the controller must escalate to SIGTERM).
"""

import json
import os
import socket
import sys
import threading
import time

PROPS_NEW = ["flash-file", "inverted-storage"]
PROPS_OLD = ["flash-file"]


def greeting():
    return {"QMP": {"version": {"qemu": {"major": 11, "minor": 1,
                                         "micro": 0}, "package": "fake"},
                    "capabilities": []}}


def guard_mode(variant):
    out = sys.stdout
    out.write(json.dumps(greeting()) + "\n")
    out.flush()
    for line in sys.stdin:
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        ex = msg.get("execute")
        if ex == "qom-list-properties":
            names = PROPS_OLD if variant == "old" else PROPS_NEW
            ret = [{"name": n, "type": "str"} for n in names]
        elif ex == "quit":
            out.write(json.dumps({"return": {}}) + "\n")
            out.flush()
            return 0
        else:
            ret = {}
        out.write(json.dumps({"return": ret}) + "\n")
        out.flush()
    return 0


class Fake:
    def __init__(self, args, variant):
        self.variant = variant
        self.args = args
        self.running = False
        self.vbase = 0
        self.mono0 = None
        self.uart = None
        self.booted = False
        self.conn = None
        self.wlock = threading.Lock()
        self.epoch = 0x10000000 << 16
        self.obt_base = None
        self.obt_vbase = 0
        self.no_reboot = "-no-reboot" in args
        self.halt = "reboot=shutdown,shutdown=pause" in args
        self.qmp_path = None
        for i, a in enumerate(args):
            nxt = args[i + 1] if i + 1 < len(args) else ""
            if a == "-qmp" and nxt.startswith("unix:"):
                self.qmp_path = nxt[5:].split(",")[0]
            if a == "-serial" and nxt.startswith("file:"):
                self.uart = nxt[5:]
            if a == "-global" and nxt.startswith(
                    "grlib-stixflash.flash-file="):
                flash = nxt.split("=", 1)[1]
                if not os.path.exists(flash):
                    with open(flash, "wb") as f:
                        f.truncate(1 << 30)
            if a == "-global" and nxt.startswith("grlib-stixspw.obt-epoch="):
                self.epoch = int(nxt.split("=", 1)[1])

    def vclock(self):
        if self.running:
            return self.vbase + int((time.monotonic() - self.mono0) * 1e9)
        return self.vbase

    def obt(self):
        v = self.vclock()
        base = self.epoch if self.obt_base is None else self.obt_base
        return base + (v - self.obt_vbase) * 65536 // 1_000_000_000

    def send(self, obj):
        with self.wlock:
            if self.conn is not None:
                try:
                    self.conn.sendall(json.dumps(obj).encode() + b"\n")
                except OSError:
                    pass

    def event(self, name, data=None):
        t = time.time()
        self.send({"event": name, "data": data or {},
                   "timestamp": {"seconds": int(t),
                                 "microseconds": int(t % 1 * 1e6)}})

    def set_running(self, on):
        if on and not self.running:
            self.mono0 = time.monotonic()
            self.running = True
            self.event("RESUME")
        elif not on and self.running:
            self.vbase = self.vclock()
            self.running = False
            self.event("STOP")

    def hw_ctrl(self):
        v = self.vclock()
        return {"schema": 1, "vclock_ns": v,
                "model": {"hk_adc": True, "powerstat": True,
                          "hk_prot": True, "det_model": True},
                "supplies": {"enables": 0, "powerstat": 0, "lines": {
                    n: {"enabled": False, "good": False}
                    for n in ("lv", "hv01", "hv17", "spw0", "spw1")}},
                "hk_adc": {"5": {"truth": 100, "presented": 100,
                                 "injected": False, "last": None}},
                "hk_prot": {"irqen": 0, "protstat": 0, "hotsample": 0,
                            "lines": [{"line": 1, "ch": 5, "bit": 0,
                                       "threshold": 0, "prot_en": False}]},
                "watchdog": {"running": False, "count": 0, "reload": 0},
                "scet": {"coarse": v // 1_000_000_000,
                         "fine": (v % 1_000_000_000) * 65536
                         // 1_000_000_000, "unsynced": True},
                "det": {"quarter_on": [False] * 4}}

    def hw_spw(self):
        return {"schema": 1, "vclock_ns": self.vclock(), "ls": 5,
                "link_down": False, "status": 0,
                "control": {"raw": 0, "tr": False, "tq": False,
                            "ie": False, "li": False},
                "timecode": {"source": "obt", "enabled": True,
                             "once_pending": False, "last_timecnt": None,
                             "last_vclock_ns": None,
                             "next_due_vclock_ns": None},
                "obt": self.obt(), "obt_kind": "platform", "rx_queued": 0,
                "counters": {"tm_dropped": 0, "rx_dropped": 0,
                             "tc_resync": 0, "tmtc_connected": False}}

    def command(self, msg):
        ex = msg.get("execute")
        a = msg.get("arguments", {})
        if ex == "qmp_capabilities" or ex == "query-status":
            return {"status": "running" if self.running else "paused",
                    "running": self.running} if ex == "query-status" else {}
        if ex == "cont":
            self.set_running(True)
            if not self.booted and self.uart:
                self.booted = True
                with open(self.uart, "ab") as f:
                    f.write(b"STIX ASW fake ver. 1.0\r\n")
                sys.stderr.write("%d@%.6f:stixspw_timecode_emit time code "
                                 "emitted: coarse=0x10000000 timecnt=0\n"
                                 % (os.getpid(), time.time()))
                sys.stderr.flush()
            return {}
        if ex == "stop":
            self.set_running(False)
            return {}
        if ex == "quit":
            if self.variant == "noquit":
                return {}
            raise SystemExit(0)
        if ex == "qom-get":
            prop = a.get("property")
            if prop == "vclock-ns":
                return self.vclock()
            if prop == "obt":
                return self.obt()
            if prop == "hw-state":
                return json.dumps(self.hw_ctrl() if "ctrl" in a["path"]
                                  else self.hw_spw())
            raise KeyError("Property '%s' not found" % prop)
        if ex == "qom-set":
            if a.get("property") == "obt":
                self.obt_base = a["value"]
                self.obt_vbase = self.vclock()
                return {}
            raise KeyError("Property '%s' not found" % a.get("property"))
        if ex == "x-fake-guest-reset":
            threading.Timer(0.05, self.guest_reset).start()
            return {}
        raise KeyError("The command %s has not been found" % ex)

    def guest_reset(self):
        if self.no_reboot:
            self.event("SHUTDOWN", {"guest": True, "reason": "guest-reset"})
            os._exit(0)
        elif self.halt:
            self.event("SHUTDOWN", {"guest": True, "reason": "guest-reset"})
            self.set_running(False)
        else:
            self.event("RESET", {"guest": True, "reason": "guest-reset"})

    def serve(self):
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(self.qmp_path)
        srv.listen(1)
        while True:
            conn, _ = srv.accept()
            self.conn = conn
            self.send(greeting())
            buf = b""
            while True:
                chunk = conn.recv(65536)
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if not line.strip():
                        continue
                    msg = json.loads(line)
                    try:
                        ret = self.command(msg)
                        rep = {"return": ret}
                    except KeyError as e:
                        rep = {"error": {"class": "GenericError",
                                         "desc": str(e.args[0])}}
                    except SystemExit:
                        self.send({"return": {}, "id": msg.get("id")})
                        self.event("SHUTDOWN", {"guest": False,
                                                "reason": "host-qmp-quit"})
                        conn.close()
                        os._exit(0)
                    if "id" in msg:
                        rep["id"] = msg["id"]
                    self.send(rep)
            with self.wlock:
                self.conn = None


def main():
    args = sys.argv[1:]
    variant = "new"
    if args[:1] == ["--variant"]:
        variant, args = args[1], args[2:]
    if "-M" in args and args[args.index("-M") + 1] == "none":
        return guard_mode(variant)
    Fake(args, variant).serve()
    return 0


if __name__ == "__main__":
    sys.exit(main())
