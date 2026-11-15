#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A fake qemu-system-sparc for the controller's self-test (no QEMU).

  fake_qemu.py [--variant new|old|noquit] <qemu args>

-M none -qmp stdio: the binary-guard probe; `old` answers
qom-list-properties for grlib-stixflash WITHOUT inverted-storage.
Otherwise it serves QMP on the -qmp unix socket like a frozen (-S)
leon3: cont/stop/quit, the RESUME/STOP/SHUTDOWN/RESET events, qom-get
vclock-ns, obt and both hw-state JSONs (the ctrl one carries the
attenuator, edac, det and rotb sections, PL-11b/PL-11c), qom-set obt,
the upset verbs on the EDAC (they move the hw-state's poisons and
counters), the det-rotb-burst pair (the FF plan's burst), memsave (the
fsw-params RAM snapshot: zeros into the asked file, in the cwd), and the
test-only x-fake-guest-reset, which behaves per -no-reboot / -action
like QEMU.
`noquit` ignores quit (the controller must escalate to SIGTERM);
`noqmp` accepts the QMP connection and never answers; `badstate`
serves a hw-state without its keys.
"""

import json
import os
import socket
import sys
import threading
import time

PROPS_NEW = ["flash-file", "inverted-storage"]
PROPS_OLD = ["flash-file"]
# the attenuator fault properties (the catalogue's qom rows on
# /machine/stixctrl/attenuator); the fake stores them like the device
ATTENUATOR = ("attenuator.sensor_ab.stuck_open",
              "attenuator.sensor_ab.stuck_closed",
              "attenuator.sensor_bc.stuck_open",
              "attenuator.sensor_bc.stuck_closed",
              "attenuator.motor1.dead", "attenuator.motor2.dead",
              "attenuator.mechanism.jammed", "attenuator.current.zero",
              "attenuator.current.offset")
SETTABLE = ("link-down", "timecodes", "timecode-once", "timecode-inject",
            "upset-inject", "upset-clear") + ATTENUATOR + \
    tuple("adc-inject[%d]" % ch for ch in range(128))
# read-back before the first set: the booleans are false, the offset 0
ATT_DEFAULT = {"attenuator.current.offset": 0}
# the EDAC device (the memory.* catalogue rows' target); its upset
# verbs move the hw-state's edac section, like the device's
EDAC = "/machine/stixctrl/edac"


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
        self.uart_append = False
        self.props = {}
        # det-rotb-burst (FF plan): the fake answers like the device's
        # runtime QOM on an empty ring (Task 2's qtest covers the real
        # one); the status starts "requested=0" before any burst
        self.props[("/machine/stixctrl", "det-rotb-burst-status")] = \
            "requested=0,appended=0,stop=done,rotb_count=0"
        self.rotb_blocks = 0
        self.set_log = []
        # the EDAC state the hw-state section shows (stix_edac.c:
        # 714-759): no poisons armed, the counters at zero. The bram
        # has no EDAC: no counters and its upsets arm no poison (M-5),
        # so it never appears here
        self.poisons = []
        self.edac_counters = {m: {"injected": 0, "corrected": 0,
                                  "detected": 0}
                              for m in ("sdram", "cache", "rotbuf")}
        for i, a in enumerate(args):
            nxt = args[i + 1] if i + 1 < len(args) else ""
            if a == "-qmp" and nxt.startswith("unix:"):
                self.qmp_path = nxt[5:].split(",")[0]
            if a == "-serial" and nxt.startswith("file:"):
                self.uart = nxt[5:]
                self.uart_append = False
            if a == "-chardev" and nxt.startswith("file,"):
                opts = dict(o.split("=", 1) for o in nxt.split(",")[1:])
                self.uart = opts["path"]
                self.uart_append = opts.get("append") == "on"
            if a == "-global" and nxt.startswith(
                    "grlib-stixflash.flash-file="):
                flash = nxt.split("=", 1)[1]
                if not os.path.exists(flash):
                    with open(flash, "wb") as f:
                        f.truncate(1 << 30)
            if a == "-global" and nxt.startswith("grlib-stixspw.obt-epoch="):
                self.epoch = int(nxt.split("=", 1)[1])

    def open_uart(self):
        """Like QEMU's file chardev: O_TRUNC unless append=on."""
        if self.uart:
            open(self.uart, "ab" if self.uart_append else "wb").close()

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
        return {"schema": 5, "vclock_ns": v,
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
                "watchdog": {"running": False, "enabled": False,
                             "count": 0, "reload": 0},
                "scet": {"coarse": v // 1_000_000_000,
                         "fine": (v % 1_000_000_000) * 65536
                         // 1_000_000_000, "unsynced": True},
                # the detector (stix_det.c, PL-11c): the scene defaults
                # (stix_det_instance_init), the FSM off, no replay table
                # loaded (idle), quarter power off
                "det": {"quarter_on": [False] * 4, "source": "synthetic",
                        "flare": False, "quiet_cps": 20, "flare_cps": 2000,
                        "ql_bank": 0, "fsm": {"enable": False},
                        "replay": {"state": "idle", "pos_ns": 0,
                                   "events": 0, "dropped": 0,
                                   "unsupported": 0, "table": "",
                                   "count": 0, "duration_ns": 0}},
                # the ROTB ring (grlib_stixctrl.c, PL-11c): empty and
                # disabled, the default capacity
                "rotb": {"count": 0, "full": False, "enable": False,
                         "rd": 0, "wr": 0, "capacity": 104448},
                # the attenuator section (stix_att.c:966-998), idle at
                # "out": AB actuates at OUT (stix_att.c:291-293), so the
                # presented cmdsta carries the AB bit (bit 10,
                # StixIdpu.h:722); att_c is HK ch 8's idle adc
                "attenuator": {
                    "truth": {"position": "out", "position_ppm": 0,
                              "sensor_ab": True, "sensor_bc": False,
                              "motor1_driving": False,
                              "motor2_driving": False, "att_c": 20,
                              "moves": 0, "last_end": "none",
                              "last_end_ns": 0},
                    "presented": {"sensor_ab": True, "sensor_bc": False,
                                  "cmdsta": 1024, "att_c": 20},
                    "fsw_accesses": 0, "faults_active": ""},
                "edac": self.hw_edac(),
                # the aspect generator (AS-2), powered off
                "as": {"asaon": False, "asbon": False, "avgsize": 0,
                       "n": 1, "pattern": "ramp", "sun": 0,
                       "rate_ppm": 0, "enable": True, "tick": False,
                       "block_seq": 0, "rows": 0, "pushed": 0,
                       "dropped": 0, "rotb_full": 0, "flip_coarse": 0,
                       "flip_fine": 0, "ql": [0, 0, 0, 0]}}

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

    def hw_edac(self):
        """The edac section (stix_edac.c:714-759): the armed poisons as
        records — the ids are positions in the list, renumbered by the
        next read after a clear, like the device's — plus the
        per-memory counters."""
        poisons = []
        for i, p in enumerate(self.poisons):
            rec = {"id": i, "memory": p["memory"], "address": p["address"],
                   "mask": p["mask"], "mode": p["mode"],
                   "persist": p["persist"]}
            if "block" in p:
                rec["block"] = p["block"]
                rec["offset"] = p["offset"]
            poisons.append(rec)
        return {"poisons": poisons,
                "counters": {m: dict(c)
                             for m, c in self.edac_counters.items()}}

    def upset_inject(self, spec):
        """The device's upset-inject (stix_edac.c:874-1031), as far as
        the hw-state shows it: a poison record and the injected
        counter. bram is silent — no record, no counters (stix_edac.c:
        887-891); corrected/detected move only when the FSW actually
        reads the poisoned word, which the fake does not model."""
        f = spec.split(",")
        if f[0] == "bram":
            return
        if f[0] == "rotbuf":          # "<block>:<offset>" (M-4)
            block, off = f[1].split(":")
            rec = {"memory": "rotbuf", "block": int(block, 0),
                   "offset": int(off, 0),
                   "address": int(block, 0) * 0x200 + int(off, 0)}
        else:
            rec = {"memory": f[0], "address": int(f[1], 0)}
        rec["mask"] = int(f[2], 0)
        # bit count 1 = seu, >= 2 = de; the mode field overrides
        rec["mode"] = "de" if bin(rec["mask"]).count("1") != 1 else "seu"
        if len(f) > 3 and f[3] in ("seu", "de"):
            rec["mode"] = f[3]
        rec["persist"] = len(f) > 4 and f[4] == "1"
        for i, p in enumerate(self.poisons):
            if p["memory"] == rec["memory"] and \
                    p["address"] == rec["address"]:
                self.poisons[i] = rec      # re-arm: replace, no re-count
                return
        self.poisons.append(rec)
        self.edac_counters[f[0]]["injected"] += 1

    def upset_clear(self, spec):
        """upset-clear (stix_edac.c:1044-1091): "all", or an index into
        the poisons list."""
        if spec == "all":
            self.poisons = []
            return
        try:
            i = int(spec, 0)
        except ValueError:
            raise KeyError("expected \"all\" or an index, not %r" % spec)
        if i < 0 or i >= len(self.poisons):
            raise KeyError("no upset %d armed (%d)" % (i, len(self.poisons)))
        del self.poisons[i]

    def command(self, msg):
        ex = msg.get("execute")
        a = msg.get("arguments", {})
        if ex == "qmp_capabilities" or ex == "query-status":
            return {"status": "running" if self.running else "paused",
                    "running": self.running} if ex == "query-status" else {}
        if ex == "cont":
            self.set_log.append(["cont"])   # ordering marker for the gates
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
            if prop == "hw-state" and self.variant == "badstate":
                return '{"schema": 1, "vclock_ns": 5}'   # keys missing
            if prop == "hw-state":
                return json.dumps(self.hw_ctrl() if "ctrl" in a["path"]
                                  else self.hw_spw())
            if (a["path"], prop) in self.props:
                return self.props[(a["path"], prop)]
            if prop in ATTENUATOR:
                return ATT_DEFAULT.get(prop, False)
            raise KeyError("Property '%s' not found" % prop)
        if ex == "qom-set":
            prop = a.get("property")
            if prop == "obt":
                self.obt_base = a["value"]
                self.obt_vbase = self.vclock()
                return {}
            if prop == "det-rotb-burst":
                n = a["value"]
                if not isinstance(n, int) or n < 1 or n > 2131:
                    raise KeyError("det-rotb-burst needs 1..2131 frames, "
                                   "got %r" % n)
                self.rotb_blocks += n * 49
                self.props[(a["path"], prop)] = n
                self.props[(a["path"], "det-rotb-burst-status")] = \
                    "requested=%d,appended=%d,stop=done,rotb_count=%d" \
                    % (n, n, self.rotb_blocks)
                self.set_log.append((a["path"], prop, n))
                return {}
            if prop in SETTABLE:
                if a["path"] == EDAC and prop == "upset-inject":
                    self.upset_inject(a["value"])
                elif a["path"] == EDAC and prop == "upset-clear":
                    self.upset_clear(a["value"])
                # a plain store; the self-test reads it back
                self.props[(a["path"], prop)] = a["value"]
                self.set_log.append((a["path"], prop, a["value"]))
                return {}
            raise KeyError("Property '%s' not found" % prop)
        if ex == "x-fake-set-log":
            return [list(x) for x in self.set_log]
        if ex == "memsave":
            # the real memsave writes @size bytes of guest memory at
            # virtual @val into @filename, relative to the cwd (the run
            # dir). The fake writes zeros: the rows whose ELF default
            # is non-zero then differ on the controller's next refresh.
            with open(a.get("filename") or "params-ram.bin", "wb") as f:
                f.write(bytes(int(a.get("size") or 0)))
            return {}
        if ex == "x-fake-guest-reset":
            threading.Timer(0.05, self.guest_reset).start()
            return {}
        raise KeyError("The command %s has not been found" % ex)

    def guest_reset(self):
        # this fake keeps all props across the reset; the real device
        # drops link-down/adc-inject (stix_att.c:504-512 keeps only the
        # attenuator's faults), and the controller re-applies those
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
            if self.variant == "noqmp":
                time.sleep(3600)     # accepts, never greets, never answers
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
    f = Fake(args, variant)
    f.open_uart()
    f.serve()
    return 0


if __name__ == "__main__":
    sys.exit(main())
