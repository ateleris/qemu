# SPDX-License-Identifier: GPL-2.0-or-later
"""`stixplat.py --self-test`: offline, no QEMU (platform plan PL-3).

Fake binaries (tests/fake_qemu.py) stand in for QEMU; fake filesystems
and clocks for the rest. Every case prints one line; the last line is
"SELF-TEST: PASS" or "SELF-TEST: FAIL".
"""

import gzip
import http.client
import json
import os
import re
import shutil
import socket
import sys
import tempfile
import threading
import time

from . import api as api_mod
from . import config as config_mod
from . import core as core_mod
from . import log as log_mod
from . import probe_lib
from . import qemu as qemu_mod
from . import retention
from . import rundir
from .dispatch import VerbError
from .qmp import QmpConnection, QmpError, parse_address
from .simclock import SimClock

HERE = os.path.dirname(os.path.abspath(__file__))
FAKE = os.path.normpath(os.path.join(HERE, "..", "tests", "fake_qemu.py"))
DOCS = os.path.normpath(os.path.join(HERE, "..", "..", "..", "..", "docs",
                                     "stix-port", "platform.md"))


class Fail(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Fail(msg)


def raises(exc, fn, *a, **kw):
    try:
        fn(*a, **kw)
    except exc as e:
        return e
    raise Fail("%s not raised by %s" % (exc.__name__, fn.__name__))


def wait_for(cond, timeout=5.0, what="condition"):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if cond():
            return
        time.sleep(0.02)
    raise Fail("timed out waiting for " + what)


class Env:
    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix="stixplat-selftest-")
        self.bins = {}
        for v in ("new", "old", "noquit"):
            p = os.path.join(self.tmp, "qemu-%s" % v)
            with open(p, "w") as f:
                f.write('#!/bin/sh\nexec "%s" "%s" --variant %s "$@"\n'
                        % (sys.executable, FAKE, v))
            os.chmod(p, 0o755)
            self.bins[v] = p
        self.n = 0

    def run_dir(self):
        self.n += 1
        d = os.path.join(self.tmp, "run%d" % self.n)
        os.makedirs(d)
        return d

    def cfg(self, **over):
        o = {"qemu": {"binary": self.bins["new"], "elf": "/dev/null"},
             "power": {"off_time_s": 0.2, "quit_timeout_s": 2.0},
             "clock": {"sample_hz": 50.0, "state_hz": 10.0}}
        for k, v in over.items():
            o.setdefault(k, {}).update(v)
        return config_mod.load(overrides=o)

    def controller(self, **over):
        c = core_mod.Controller(self.cfg(**over), run_dir=self.run_dir())
        c.start(serve=False)
        return c

    def close(self):
        shutil.rmtree(self.tmp, ignore_errors=True)


# -- the cases ------------------------------------------------------------
def t_config(env):
    c = config_mod.load()
    check(c["qemu"]["on_guest_reset"] == "warm", "default policy")
    check(c["qemu"]["obt_epoch"] & 63 << 16 == 0, "epoch 6-LSB aligned")
    c = config_mod.load(text='mode = "interactive"\n[ports]\napi = 21520\n'
                        '[qemu]\non_guest_reset = "halt"  # comment\n')
    check(c["ports"]["api"] == 21520 and
          c["qemu"]["on_guest_reset"] == "halt", "toml override")
    for bad in ('nosuch = 1\n', '[ports]\napi = "x"\n',
                '[qemu]\non_guest_reset = "never"\n', '[ports]\napi = 0\n',
                '[power]\ncycle_boots = 1\n', 'mode = \n',
                '[qemu]\nobt_epoch = 281474976710656\n'):
        raises(config_mod.ConfigError, config_mod.load, text=bad)
    c = config_mod.load(text="[power]\noff_time_s = 3\n")
    check(c["power"]["off_time_s"] == 3.0, "int for a float key")


ARGV_GOLDEN = [
    "/q/qemu-system-sparc", "-M", "leon3_generic", "-m", "64M", "-S",
    "-display", "none", "-monitor", "none",
    "-serial", "file:/r/uart.raw",
    "-qmp", "unix:/r/qmp.sock,server=on,wait=off",
    "-global", "grlib-stixflash.flash-file=/r/flash.bin",
    "-global", "grlib-stixflash.inverted-storage=on",
    "-global", "grlib-stixspw.tmtc-server=off",
    "-global", "grlib-stixspw.udp-tm-host=127.0.0.1",
    "-global", "grlib-stixspw.udp-tm-port=21511",
    "-global", "grlib-stixspw.udp-tc-port=21512",
    "-global", "grlib-stixspw.udp-spw-header=on",
    "-global", "grlib-stixspw.timecode-source=obt",
    "-global", "grlib-stixspw.obt-epoch=17592186044416",
    "-action", "reboot=reset",
    "-trace", "stixspw_timecode_emit", "-trace", "stixspw_rx_deliver",
    "-trace", "stixspw_timecode_manual",
    "-trace", "stixspw_timecode_once_drop", "-trace", "stixspw_obt_set",
    "-kernel", "/e/asw.elf"]


def t_argv(env):
    c = config_mod.load(overrides={"qemu": {"binary": "/q/qemu-system-sparc",
                                            "elf": "/e/asw.elf"}})
    argv = qemu_mod.build_argv(c, "/r")
    check(argv == ARGV_GOLDEN, "argv differs from the golden:\n%s"
          % "\n".join(argv))
    check(qemu_mod.reset_args("cold") == ["-no-reboot"], "cold args")
    check(qemu_mod.reset_args("halt") ==
          ["-action", "reboot=shutdown,shutdown=pause"], "halt args")


class FakeFs(rundir.RealFs):
    """A filesystem without sparse files (or without hole punching)."""

    def __init__(self, sparse=True, punch=True):
        self.sparse, self.punch = sparse, punch
        self.files = {}

    def create_sparse(self, path, size):
        self.files[path] = size

    def write_block(self, path, offset, data):
        pass

    def blocks_bytes(self, path):
        return self.files[path] if not self.sparse else 4096

    def punch_hole(self, path, offset, length):
        if not self.punch:
            raise OSError(95, "Operation not supported")

    def unlink(self, path):
        del self.files[path]

    def exists(self, path):
        return path in self.files


def t_rundir(env):
    for bad in ("/tmp/stix-uprev-run", "/tmp/stix-baseline-run",
                "/tmp/stix-plat-run", "/tmp/stix-uprev-run/"):
        raises(rundir.RunDirError, rundir.guard, bad)
    link = os.path.join(env.tmp, "sneaky")
    os.symlink("/tmp/stix-uprev-run", link)
    raises(rundir.RunDirError, rundir.guard, link)
    rundir.guard("/tmp/stix-plat/core-smoke")
    d = env.run_dir()
    rundir.preflight(d)                       # the real FS: sparse
    check(not os.path.exists(os.path.join(d, rundir.PROBE_NAME)),
          "probe file left behind")
    for fs, what in ((FakeFs(sparse=False), "allocated"),
                     (FakeFs(punch=False), "punch hole")):
        e = raises(rundir.RunDirError, rundir.preflight, d, fs)
        check("does not support sparse" in str(e) and what in str(e),
              "preflight message: %s" % e)
        check(not fs.files, "fake probe file left behind")
    fs = FakeFs(sparse=False)
    fs.files[os.path.join(d, "flash.bin")] = 17 << 30
    raises(rundir.RunDirError, rundir.sparse_check, d, fs)
    for n in ("flash.bin", "uart.raw", "keep.me"):
        open(os.path.join(d, n), "w").close()
    removed = rundir.prepare(d, fresh=True)
    check(sorted(removed) == ["flash.bin", "uart.raw"] and
          os.path.exists(os.path.join(d, "keep.me")), "fresh removed %s"
          % removed)


def t_binary_guard(env):
    ctl = core_mod.Controller(env.cfg(qemu={"binary": env.bins["old"]}),
                              run_dir=env.run_dir())
    try:
        e = raises(VerbError, ctl.command, "platform.power.on", {})
        check(e.status == 409 and "predates 21f898484f" in str(e),
              "old binary: %s" % e)
        check(not os.path.exists(os.path.join(ctl.run_dir, "flash.bin")),
              "the refused binary created flash.bin")
        check(ctl.power.state == "off", "state after a refusal")
    finally:
        ctl.shutdown()
    ok, detail, cached = qemu_mod.probe_binary(env.bins["new"])
    check(ok and not cached, "new binary accepted (%s)" % detail)
    ok, detail, cached = qemu_mod.probe_binary(env.bins["new"])
    check(ok and cached, "probe result cached")


def t_retention(env):
    root = os.path.join(env.tmp, "rroot")
    now = 1_800_000_000
    day = 86400
    sizes = {}
    runs = [("20261101T000000Z-a", "passed", now - 10 * day, 100),
            ("20261102T000000Z-b", "failed", now - 9 * day, 100),
            ("20261103T000000Z-c", "failed", now - 2 * day, 100),
            ("20261104T000000Z-d", "passed", now - 2 * day, 300),
            ("20261105T000000Z-e", "passed", now - day, 300),
            ("20261106T000000Z-f", "running", None, 50),
            ("20261107T000000Z-g", "passed", now, 300)]
    for name, st, ended, mb in runs:
        p = os.path.join(root, "runs", name)
        os.makedirs(p)
        retention.write_marker(p, st, ended)
        open(os.path.join(p, "flash.bin"), "w").close()
        sizes[p] = mb << 20
    alien = os.path.join(root, "runs", "20261100T000000Z-alien")
    os.makedirs(alien)                        # no marker: never touched
    P = lambda n: os.path.join(root, "runs", n)
    rc = {"keep_runs": 3, "keep_failed_days": 7, "max_total_mb": 2048,
          "min_free_mb": 1, "keep_flash": "failed"}
    got = [p for p, _ in retention.plan(root, rc, now, sizes)]
    check(got == [P("20261101T000000Z-a"), P("20261102T000000Z-b"),
                  P("20261104T000000Z-d")], "keep_runs/failed_days: %s"
          % got)
    rc2 = dict(rc, max_total_mb=500)
    got = [p for p, _ in retention.plan(root, rc2, now, sizes)]
    check(got[:3] == [P("20261101T000000Z-a"), P("20261102T000000Z-b"),
                      P("20261104T000000Z-d")] and
          got[3:] == [P("20261105T000000Z-e")], "max_total_mb: %s" % got)
    seen = []
    retention.cleanup(root, rc, now, dry_run=True, sizes=sizes,
                      emit=lambda p, w, d: seen.append((p, d)))
    check(len(seen) == 3 and all(d for _, d in seen) and
          all(os.path.isdir(p) for p, _ in seen), "a dry run deleted")
    retention.cleanup(root, rc, now, sizes=sizes)
    check(not os.path.exists(P("20261101T000000Z-a")) and
          os.path.isdir(alien) and os.path.isdir(P("20261106T000000Z-f")),
          "cleanup deleted the wrong dirs")
    check(retention.finish_run(P("20261107T000000Z-g"), True, "failed",
                               now), "passed run keeps its flash")
    check(not retention.finish_run(P("20261103T000000Z-c"), False,
                                   "failed", now), "failed run lost flash")

    class Vfs:
        f_bavail, f_frsize = 100, 1 << 20
    raises(retention.RetentionError, retention.check_free, root, 1024,
           statvfs=lambda p: Vfs)
    retention.check_free(root, 50, statvfs=lambda p: Vfs)


def t_log(env):
    d = env.run_dir()
    path = os.path.join(d, "log.jsonl")
    clk = SimClock()
    lg = log_mod.RecordLog(path, clk.now, ring=50, max_bytes=4000,
                           max_files=3)
    for i in range(120):
        lg.emit("ctl", "log", "t.%d" % i, {"i": i, "pad": "x" * 40})
    raises(ValueError, lg.emit, "nobody", "log", "x")
    raises(ValueError, lg.emit, "ctl", "nokind", "x")
    lg.close()
    rot = log_mod.rotated_names(path, 3)
    check(all(os.path.exists(p) for p in rot), "rotated files %s" % rot)
    check(not os.path.exists(path + ".3.gz"), "max_files exceeded")
    lines = []
    for p in reversed(rot):
        with gzip.open(p, "rt") as f:
            lines += f.read().splitlines()
    with open(path) as f:
        tail = f.read().splitlines()
    lines += tail
    seqs = []
    for ln in lines:
        obj = json.loads(ln)
        log_mod.check_record(obj)
        seqs.append(obj["seq"])
    check(all(b == a + 1 for a, b in zip(seqs, seqs[1:])),
          "seq not strictly increasing by 1")
    check(seqs[-1] == 120, "last seq")
    ring = lg.since(0)
    check(len(ring) == 50 and [r.line for r in ring[-len(tail):]] == tail,
          "the ring holds the same bytes as log.jsonl")
    check(lg.oldest_seq() == 71, "ring oldest")
    check(log_mod.topic_of("power") == "state" and
          log_mod.topic_of("result") == "cmd" and
          log_mod.topic_of("force") == "fault" and
          log_mod.topic_of("debug") == "log" and
          log_mod.topic_of("uart") == "uart", "kind->topic")


def t_qmp(env):
    check(parse_address("tcp:[::1]:4444") == ("tcp", ("::1", 4444)) and
          parse_address("tcp:localhost:1") == ("tcp", ("localhost", 1)) and
          parse_address("/r/qmp.sock") == ("unix", "/r/qmp.sock"),
          "parse_address")
    for bad in ("tcp:::1:4", "tcp:h:0", "tcp:h:x", "tcp:[::1]"):
        raises(QmpError, parse_address, bad)
    a, b = socket.socketpair()
    script = []

    def server():
        f = b.makefile("rwb")
        f.write(b'{"QMP": {"version": {}, "capabilities": []}}\n')
        f.flush()
        for line in f:
            msg = json.loads(line)
            script.append(msg)
            ex, mid = msg["execute"], msg["id"]
            if ex == "qmp_capabilities":
                f.write(b'{"return": {}, "id": %d}\n' % mid)
            elif ex == "evs":
                # events between replies, a stale reply, then ours
                f.write(b'{"event": "STOP", "data": {}}\n')
                f.write(b'{"return": "stale", "id": 999}\n')
                f.write(b'{"event": "RESUME", "data": {}}\n')
                f.write(b'{"return": 7, "id": %d}\n' % mid)
            elif ex == "bad":
                f.write(b'{"error": {"class": "GenericError", "desc": '
                        b'"nope"}, "id": %d}\n' % mid)
            elif ex == "slow":
                pass                        # never answered
            elif ex == "bye":
                f.close()
                b.close()
                return
            f.flush()
    t = threading.Thread(target=server, daemon=True)
    t.start()
    got = []
    q = QmpConnection(sock=a, timeout=1.0, on_event=got.append)
    q.connect()
    check(q.execute("evs") == 7, "reply after events")
    check([m["event"] for m in got] == ["STOP", "RESUME"] and
          q.events.qsize() == 2, "events queued and called back")
    e = raises(QmpError, q.execute, "bad")
    check(e.klass == "GenericError" and "nope" in str(e), "error reply")
    t0 = time.monotonic()
    raises(QmpError, q.execute, "slow", timeout=0.3)
    check(time.monotonic() - t0 < 1.0, "timeout honoured")
    check(q.rtt_min is not None and q.rtt_max >= q.rtt_min, "rtt stats")
    raises(QmpError, q.execute, "bye", timeout=1.0)
    raises(QmpError, q.execute, "after-close", timeout=0.5)
    q.close()


def t_simclock(env):
    t = [100.0]
    clk = SimClock(mono_fn=lambda: t[0])
    t[0] = 101.0
    clk.process_started()                      # 1 s of initial off-time
    check(clk.now()[:3] == (1_000_000_000, 1, 0), "power-on")
    clk.resumed()
    fired = []
    clk.at(clk.exact_sim() + 2_000_000_000, lambda: fired.append("a"))
    t[0] = 102.5
    check(clk.now()[3] is True, "an estimate while running")
    clk.sample(1_500_000_000)
    check(clk.run_due() == [] and not fired, "early")
    clk.stopped()                              # STOP
    t[0] = 200.0                               # long pause
    clk.sample(1_500_000_000)
    check(clk.now()[0] == 2_500_000_000, "frozen while paused")
    check(clk.run_due() == [], "no firing while paused")
    clk.sample(5_000_000_000)                  # even a late sample
    check(clk.run_due() == [] and not fired, "no firing while paused (2)")
    clk.resumed()
    t[0] = 200.1
    clk.sample(5_100_000_000)
    check(clk.run_due() and fired == ["a"], "fires after RESUME")
    before = clk.now()[0]
    clk.guest_reset()                          # warm: same process
    check(clk.boot_id == 2 and clk.now()[0] >= before, "warm reset")
    t[0] = 201.0
    clk.process_ended(final_vclock=6_000_000_000)
    s_off = clk.now()[0]
    check(s_off == 7_000_000_000, "end of process: %d" % s_off)
    t[0] = 203.0                               # 2 s off (wall clock)
    check(clk.now()[0] == 9_000_000_000, "off-time on the wall clock")
    clk.process_started()
    check(clk.now()[:3] == (9_000_000_000, 3, 0),
          "continuous across a power cycle: %r" % (clk.now(),))
    clk.resumed()
    clk.sample(0)
    t[0] = 203.0
    clk.sample(100)
    clk.sample(50)                             # a sample never goes back
    check(clk.now()[0] == 9_000_000_100, "monotone")


def t_power(env):
    ctl = env.controller()
    try:
        cmd = ctl.command
        e = raises(VerbError, cmd, "platform.power.boot", {})
        check(e.status == 409, "boot while off")
        r = cmd("platform.power.on", {})
        check(ctl.power.state == "frozen" and r["boot_id"] == 1, "on")
        flash = os.path.join(ctl.run_dir, "flash.bin")
        ino = os.stat(flash).st_ino
        check(os.stat(os.path.join(ctl.run_dir, "qmp.sock")).st_mode
              & 0o777 == 0o600, "qmp.sock mode")
        raises(VerbError, cmd, "platform.power.on", {})
        cmd("platform.power.boot", {})
        check(ctl.power.state == "running", "boot")
        wait_for(lambda: any(r.kind == "uart" and "STIX ASW" in r.line
                             for r in ctl.log.since(0)), what="uart record")
        wait_for(lambda: any(r.obj["path"] == "trace.stixspw_timecode_emit"
                             for r in ctl.log.since(0)), what="trace record")
        cmd("sim.pause", {})
        s1 = ctl.clock.now()[0]
        time.sleep(0.3)
        check(ctl.clock.now()[0] - s1 < 50_000_000, "paused clock moved")
        cmd("sim.resume", {})
        wait_for(lambda: ctl.state.get("platform.obt") is not None,
                 what="state poll")
        check(ctl.state.get("instrument.hk_adc.5")["truth"] == 100,
              "state leaf")
        snaps = [x.obj["data"]["leaves"] for x in ctl.log.since(0)
                 if x.kind == "state" and x.obj["path"] == "snapshot"]
        check(snaps and "idpu.scet.coarse" in snaps[0] and
              "platform.obt" in snaps[0], "state snapshot record")
        dq = [x for x in ctl.log.since(0) if x.obj["path"] == "debug.qmp"]
        check(not dq, "debug.qmp records before any call")
        sim_before = ctl.clock.now()[0]
        r = cmd("platform.power.cycle", {"off_time_s": 0.2})
        check(r["boot_id"] == 2 and ctl.power.state == "running", "cycle")
        check(os.stat(flash).st_ino == ino, "flash inode changed")
        check(ctl.clock.now()[0] >= sim_before + 200_000_000,
              "bench time continuous across the cycle")
        obt_logs = [x.obj["data"] for x in ctl.log.since(0)
                    if x.obj["path"] == "platform.obt"]
        check(obt_logs and obt_logs[-1]["mode"] == "aligned" and
              (obt_logs[-1]["set"] >> 16) & 63 == 0 and
              obt_logs[-1]["set"] >= obt_logs[-1]["continued"],
              "OBT continuity: %r" % obt_logs)
        r = cmd("platform.power.off", {})
        check(r["method"] == "quit" and ctl.power.state == "off", "off")
        check(ctl.state.get("idpu") is None, "idpu state after off")
        raises(VerbError, cmd, "platform.power.off", {})
        raises(VerbError, cmd, "sim.pause", {})
        e = raises(VerbError, cmd, "debug.qmp", {"command": "query-status"})
        check(e.status == 403, "debug.qmp is gated")
        e = raises(VerbError, cmd, "platform.power.cycle", {"bogus": 1})
        check(e.status == 400, "unknown arg")
        e = raises(VerbError, cmd, "no.such", {})
        check(e.status == 404, "unknown verb")
        recs = [x.obj for x in ctl.log.since(0)]
        res = [x for x in recs if x["kind"] == "result"]
        check(res and all("ok" in x["data"] for x in res), "result records")
    finally:
        ctl.shutdown()
    # quit ignored: SIGTERM
    ctl = env.controller(qemu={"binary": env.bins["noquit"]},
                         power={"quit_timeout_s": 0.3})
    try:
        ctl.command("platform.power.on", {})
        r = ctl.command("platform.power.off", {})
        check(r["method"] == "sigterm", "escalation: %r" % r)
    finally:
        ctl.shutdown()
    # debug.qmp enabled
    ctl = env.controller(debug={"raw_qmp": True})
    try:
        ctl.command("platform.power.on", {})
        r = ctl.command("debug.qmp", {"command": "qom-set", "arguments": {
            "path": "/machine/stixspw", "property": "obt",
            "value": 5 << 16}})
        r = ctl.command("debug.qmp", {"command": "qom-get", "arguments": {
            "path": "/machine/stixspw", "property": "obt"}})
        check(r["return"] == 5 << 16, "debug.qmp round trip")
        dq = [x.obj for x in ctl.log.since(0) if x.obj["path"] == "debug.qmp"]
        check([x["kind"] for x in dq] == ["cmd", "result"] * 2 and
              dq[0]["data"]["args"]["command"] == "qom-set",
              "every debug.qmp call is logged: %r" % dq)
        e = raises(VerbError, ctl.command, "debug.qmp", {"command": "quit"})
        check(e.status == 400, "debug.qmp allowlist")
    finally:
        ctl.shutdown()


def _power_recs(ctl, event):
    return [r.obj["data"] for r in ctl.log.since(0)
            if r.kind == "power" and r.obj["data"].get("event") == event]


def t_guest_reset(env):
    for policy in ("warm", "cold", "halt"):
        ctl = env.controller(qemu={"on_guest_reset": policy})
        try:
            ctl.command("platform.power.on", {})
            ctl.command("platform.power.boot", {})
            pid = ctl.power.pid
            ctl.power.qmp.execute("x-fake-guest-reset")
            wait_for(lambda: _power_recs(ctl, "guest_reset"),
                     what=policy + " guest reset record")
            rec = _power_recs(ctl, "guest_reset")[-1]
            check(rec["policy"] == policy, "%s: %r" % (policy, rec))
            if policy == "warm":
                check(ctl.power.pid == pid and ctl.clock.boot_id == 2 and
                      ctl.power.state == "running", "warm")
            elif policy == "cold":
                wait_for(lambda: ctl.power.state == "running" and
                         ctl.power.pid != pid, what="cold restart")
                check("not hardware-like" in rec["note"] and
                      ctl.clock.boot_id == 2, "cold: %r" % rec)
            else:
                wait_for(lambda: ctl.power.state == "halted", what="halt")
                check(ctl.power.proc.alive(), "halt keeps the process")
        finally:
            ctl.shutdown()


def _http(port, method, path, body=None, headers=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    h = {"Host": "127.0.0.1:%d" % port}
    h.update(headers or {})
    c.request(method, path, body=body, headers=h)
    r = c.getresponse()
    data = r.read()
    c.close()
    return r.status, dict(r.getheaders()), data


def t_filter(env):
    f = api_mod.filter_request
    ok = {"Host": "localhost:8080"}
    post = {"Host": "127.0.0.1", "Content-Type": "application/json",
            "X-Stix-Platform": "1"}
    check(f("GET", ok) is None and f("GET", {"Host": "[::1]:1"}) is None,
          "allowed hosts")
    check(f("POST", post) is None, "a good POST")
    for m, h, st in (("GET", {"Host": "evil.example"}, 403),
                     ("GET", {"Host": "localhost.evil.example"}, 403),
                     ("GET", {"Host": "127.0.0.1.nip.io:80"}, 403),
                     ("GET", {}, 403),
                     ("POST", dict(post, **{"X-Stix-Platform": "0"}), 403),
                     ("POST", {"Host": "localhost",
                               "Content-Type": "application/json"}, 403),
                     ("POST", dict(post, **{"Content-Type":
                                            "text/plain"}), 415),
                     ("PUT", ok, 405)):
        r = f(m, h)
        check(r is not None and r[0] == st, "%s %r -> %r" % (m, h, r))
    ctl = env.controller()
    srv = api_mod.make_server(ctl, "127.0.0.1", 0)
    port = srv.server_address[1]
    try:
        st, hd, _ = _http(port, "GET", "/api/v1/status",
                          headers={"Host": "evil.example"})
        check(st == 403, "live bad Host")
        st, hd, body = _http(port, "GET", "/api/v1/status")
        check(st == 200 and json.loads(body)["power"] == "off", "status")
        check(not any(k.lower().startswith("access-control") for k in hd),
              "CORS header present")
        st, _, _ = _http(port, "POST", "/api/v1/cmd/log.mark",
                         body=b'{"text": "x"}',
                         headers={"Content-Type": "text/plain"})
        check(st == 415, "live POST without JSON type")
        st, _, _ = _http(port, "POST", "/api/v1/cmd/log.mark",
                         body=b'{"text": "x"}',
                         headers={"Content-Type": "application/json"})
        check(st == 403, "live POST without X-Stix-Platform")
        st, _, body = _http(port, "POST", "/api/v1/cmd/log.mark",
                            body=b'{"text": "hello"}', headers=post)
        check(st == 200 and json.loads(body)["ok"], "live POST")
        st, _, _ = _http(port, "DELETE", "/api/v1/status")
        check(st == 405, "DELETE")
        st, _, body = _http(port, "GET", "/api/v1/log?since=0&kinds=log")
        lines = body.decode().splitlines()
        check(st == 200 and lines and all(json.loads(x)["kind"] == "log"
                                          for x in lines), "log slice")
        st, _, body = _http(port, "GET", "/api/v1/time")
        check(st == 200 and json.loads(body)["power"] == "off", "time")
        st, _, _ = _http(port, "GET", "/api/v1/state/no.such")
        check(st == 404, "state 404")
    finally:
        srv.shutdown()
        srv.server_close()
        ctl.shutdown()


def _sse_read(port, headers, n_events, path="/api/v1/stream"):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    req = "GET %s HTTP/1.1\r\nHost: localhost\r\n" % path
    for k, v in headers.items():
        req += "%s: %s\r\n" % (k, v)
    s.sendall((req + "\r\n").encode())
    buf = b""
    events = []
    end = time.monotonic() + 5
    while len(events) < n_events and time.monotonic() < end:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
        head, _, rest = buf.partition(b"\r\n\r\n")
        if not rest:
            continue
        events = [e for e in rest.split(b"\n\n")[:-1]
                  if e.startswith(b"id:") or e.startswith(b"event: gap")]
    s.close()
    return head, events


def t_sse(env):
    ctl = env.controller()
    srv = api_mod.make_server(ctl, "127.0.0.1", 0)
    port = srv.server_address[1]
    try:
        for i in range(5):
            ctl.command("log.mark", {"text": "m%d" % i})
        head, evs = _sse_read(port, {"Last-Event-ID": "0"}, 4,
                              "/api/v1/stream?topics=log")
        check(b"text/event-stream" in head, "SSE content type")
        lines = {r.seq: r.line for r in ctl.log.since(0)}
        for e in evs:
            m = re.match(rb"id: (\d+)\nevent: (\w+)\ndata: (.*)$", e, re.S)
            check(m is not None, "SSE frame %r" % e)
            seq = int(m.group(1))
            check(m.group(2) == b"log" and
                  m.group(3).decode() == lines[seq],
                  "SSE data equals the log line")
        mid = int(re.match(rb"id: (\d+)", evs[1]).group(1))
        head, evs2 = _sse_read(port, {"Last-Event-ID": str(mid)}, 2,
                               "/api/v1/stream?topics=log")
        first = int(re.match(rb"id: (\d+)", evs2[0]).group(1))
        check(first > mid and first == int(re.match(
            rb"id: (\d+)", evs[2]).group(1)), "Last-Event-ID resume")
        ctl.log.ring_size = 3
        for i in range(10):
            ctl.log.emit("ctl", "log", "fill", {})
        head, evs3 = _sse_read(port, {"Last-Event-ID": "1"}, 1)
        check(evs3 and evs3[0].startswith(b"event: gap"), "gap event")
    finally:
        srv.shutdown()
        srv.server_close()
        ctl.shutdown()


def t_verbs_doc(env):
    with open(DOCS) as f:
        text = f.read()
    m = re.search(r"<!-- verbs:begin -->(.*?)<!-- verbs:end -->", text, re.S)
    check(m is not None, "no verbs table in %s" % DOCS)
    doc = {}
    for row in m.group(1).splitlines():
        cells = [c.strip() for c in row.strip().strip("|").split("|")]
        vm = re.fullmatch(r"`([\w.]+)`", cells[0]) if cells else None
        if vm:
            doc[vm.group(1)] = sorted(re.findall(r"`(\w+)`", cells[1]))
    ctl = env.controller()
    try:
        api = {v["verb"]: sorted(v["args"]) for v in
               ctl.dispatcher.describe()}
    finally:
        ctl.shutdown()
    check(api == doc, "GET /verbs differs from the docs table:\napi %r\n"
          "doc %r" % (api, doc))


def t_probe_lib(env):
    p = probe_lib.load()
    units = list(p.frame_units(bytes.fromhex("fe0201") + b"\x00" * 6))
    check(isinstance(units, list), "probe frame_units")


CASES = [("config", t_config), ("argv golden", t_argv),
         ("run-dir guard + sparse preflight", t_rundir),
         ("binary guard", t_binary_guard), ("retention", t_retention),
         ("record log + rotation", t_log), ("QmpConnection", t_qmp),
         ("SimClock", t_simclock), ("power state machine", t_power),
         ("guest-reset policies", t_guest_reset),
         ("D-PLj filter", t_filter), ("SSE + Last-Event-ID", t_sse),
         ("verbs == docs table", t_verbs_doc), ("probe_lib", t_probe_lib)]


def main(only=None):
    env = Env()
    failed = 0
    try:
        for name, fn in CASES:
            if only and only not in name:
                continue
            t0 = time.monotonic()
            try:
                fn(env)
                print("ok   %-34s %.2fs" % (name, time.monotonic() - t0))
            except Exception as e:
                failed += 1
                print("FAIL %-34s %s: %s" % (name, type(e).__name__, e))
            sys.stdout.flush()
    finally:
        env.close()
    print("SELF-TEST: %s" % ("PASS" if not failed else "FAIL"))
    return 1 if failed else 0
