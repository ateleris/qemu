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
from . import decode
from . import faults as faults_mod
from . import keepalive as keepalive_mod
from . import tcbuild
from . import timesync
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

NS = 1_000_000_000


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


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Env:
    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix="stixplat-selftest-")
        self.bins = {}
        for v in ("new", "old", "noquit", "noqmp", "badstate"):
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
        return config_mod.load(overrides=self.overrides(**over))

    def overrides(self, **over):
        o = {"qemu": {"binary": self.bins["new"], "elf": "/dev/null"},
             "power": {"off_time_s": 0.2, "quit_timeout_s": 2.0},
             "clock": {"sample_hz": 50.0, "state_hz": 10.0},
             # private UDP ports: never the interactive or gate ones
             "ports": {"qemu_tm": free_udp_port(),
                       "qemu_tc": free_udp_port(),
                       "gs_tc": free_udp_port()},
             "gs": {"tm_dests": ["127.0.0.1:%d" % free_udp_port()]}}
        for k, v in over.items():
            o.setdefault(k, {}).update(v)
        return o

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
    check(c["keepalive"]["period_s"] == 0.125,
          "the keep-alive default is 125 ms (user decision): %r"
          % c["keepalive"]["period_s"])
    c = config_mod.load(text='mode = "interactive"\n[ports]\napi = 21520\n'
                        '[qemu]\non_guest_reset = "halt"  # comment\n')
    check(c["ports"]["api"] == 21520 and
          c["qemu"]["on_guest_reset"] == "halt", "toml override")
    for bad in ('nosuch = 1\n', '[ports]\napi = "x"\n',
                '[qemu]\non_guest_reset = "never"\n', '[ports]\napi = 0\n',
                '[power]\ncycle_boots = 1\n', 'mode = \n',
                '[qemu]\nobt_epoch = 281474976710656\n',
                # coarse bit 31 is the CUC "unsynchronised" flag
                '[qemu]\nobt_epoch = %d\n' % (0x80000000 << 16)):
        raises(config_mod.ConfigError, config_mod.load, text=bad)
    # S6: a non-loopback bind needs [api] container = true (PL-7)
    raises(config_mod.ConfigError, config_mod.load,
           text='[api]\nbind = "0.0.0.0"\n')
    c = config_mod.load(text='[api]\nbind = "0.0.0.0"\ncontainer = true\n')
    check(c["api"]["bind"] == "0.0.0.0", "container bind")
    for b in ("127.0.0.1", "::1", "localhost"):
        config_mod.load(overrides={"api": {"bind": b}})
    # a comma would split QEMU's -chardev / -global option strings
    for bad in ('[run_dir]\npath = "/tmp/stix-plat/a,b"\n',
                '[qemu]\nelf = "/x/a,append=off.elf"\n',
                '[qemu]\nbinary = "/x/q,emu"\n'):
        raises(config_mod.ConfigError, config_mod.load, text=bad)
    raises(rundir.RunDirError, core_mod.Controller, config_mod.load(),
           run_dir=os.path.join(env.tmp, "x,y"))
    c = config_mod.load(text="[power]\noff_time_s = 3\n")
    check(c["power"]["off_time_s"] == 3.0, "int for a float key")


ARGV_GOLDEN = [
    "/q/qemu-system-sparc", "-M", "leon3_generic", "-m", "64M", "-S",
    "-display", "none", "-monitor", "none",
    "-chardev", "file,id=stixuart,path=/r/uart.raw,append=on",
    "-serial", "chardev:stixuart",
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
    for bad in ("/tmp/stix-uprev-run-s1", "/tmp/stix-uprev-run-s12/x",
                "/tmp/stix-uprev-run/sub/dir", "/tmp/stix-baseline-run/a",
                "/", "/tmp/stix-d4-run-s3"):
        raises(rundir.RunDirError, rundir.guard, bad)
    deep = os.path.join(env.tmp, "deeplink")
    os.symlink("/tmp/stix-uprev-run-s2/inner", deep)
    raises(rundir.RunDirError, rundir.guard, deep)
    # "*" must not cross "/": these are not gate run dirs
    for good in ("/tmp/stix-plat/core-smoke", "/tmp/stix-plat/x-run",
                 "/tmp/stix-plat/a/b-run-s1", "/tmp/stix-uprev-runner"):
        rundir.guard(good)
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
    clk.correct(4_000_000_000)                 # the real vclock is lower
    check(clk.exact_sim() == 5_000_000_000 and
          clk.now()[0] == 5_000_000_000, "correct() lowers the exact time")
    clk.correct(1_000_000_000)
    check(clk.now()[0] == 5_000_000_000, "reported time never goes back")
    clk.correct(5_000_000_000)
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
        real = ctl.power.qmp.qom_get("/machine/stixspw", "vclock-ns")
        check(ctl.clock.vclock == real, "the clock after pause is the real "
              "vclock (%r != %r)" % (ctl.clock.vclock, real))
        s1 = ctl.clock.now()[0]
        time.sleep(0.3)
        check(ctl.clock.now()[0] - s1 < 50_000_000, "paused clock moved")
        cmd("sim.resume", {})
        wait_for(lambda: ctl.state.get("platform.obt") is not None,
                 what="state poll")
        check(ctl.state.get("instrument.hk_adc.5")["truth"] == 100,
              "state leaf")
        snaps = [x.obj["data"] for x in ctl.log.since(0)
                 if x.kind == "state" and x.obj["data"].get("snapshot")]
        check(snaps and all(x["path"] == "" for x in
                            (r.obj for r in ctl.log.since(0)
                             if r.obj["data"].get("snapshot") is True)) and
              "idpu.scet.coarse" in snaps[-1]["leaves"] and
              "platform.obt" in snaps[-1]["leaves"], "state snapshot record")
        reasons = [x["reason"] for x in snaps]
        check("boot" in reasons and "pause" in reasons,
              "snapshots at boot and pause: %r" % reasons)
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
        last = [x.obj for x in ctl.log.since(0)
                if x.obj["data"].get("snapshot") is True][-1]
        offrec = [x.obj for x in ctl.log.since(0) if x.kind == "power" and
                  x.obj["data"].get("event") == "off"][-1]
        check(last["data"]["reason"] == "off" and
              last["seq"] < offrec["seq"] and
              "idpu.scet.coarse" in last["data"]["leaves"],
              "a snapshot just before power-off")
        check(ctl.state.get("idpu") is None, "idpu state after off")
        raises(VerbError, cmd, "platform.power.off", {})
        raises(VerbError, cmd, "sim.pause", {})
        e = raises(VerbError, cmd, "debug.qmp", {"command": "query-status"})
        check(e.status == 403, "debug.qmp is gated")
        e = raises(VerbError, cmd, "platform.power.cycle", {"bogus": 1})
        check(e.status == 400, "unknown arg")
        tail = [x.obj for x in ctl.log.since(0)][-2:]
        check([x["kind"] for x in tail] == ["cmd", "result"] and
              tail[0]["data"]["args"] == {"bogus": 1} and
              tail[1]["data"]["cmd_seq"] == tail[0]["seq"],
              "a refused command logs cmd then result: %r" % tail)
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
        check([x["kind"] for x in dq] == ["cmd", "debug", "result"] * 2 and
              dq[0]["data"]["args"]["command"] == "qom-set" and
              dq[1]["data"]["arguments"]["property"] == "obt",
              "every debug.qmp call is a debug record: %r"
              % [x["kind"] for x in dq])
        check(ctl.status()["debug_used"] is True, "the taint flag")
        e = raises(VerbError, ctl.command, "debug.qmp", {"command": "quit"})
        check(e.status == 400, "debug.qmp allowlist")
        for path in ("/machine", "/machine/stixflash", "/objects/x",
                     "/machine/stixspw/../stixflash"):
            e = raises(VerbError, ctl.command, "debug.qmp", {
                "command": "qom-get", "arguments": {
                    "path": path, "property": "x"}})
            check(e.status == 403, "debug.qmp path %s: %s" % (path, e))
    finally:
        ctl.shutdown()
    ctl = env.controller(debug={"raw_qmp": True})
    try:
        check(ctl.status()["debug_used"] is False, "taint before use")
    finally:
        ctl.shutdown()
    ctl = env.controller(debug={"raw_qmp": True})
    ctl.cfg["mode"] = "run"
    try:
        ctl.command("platform.power.on", {})
        e = raises(VerbError, ctl.command, "debug.qmp",
                   {"command": "query-status"})
        check(e.status == 403 and "run" in str(e), "run mode: %s" % e)
        ctl.cfg["debug"]["allow_in_run"] = True
        ctl.command("debug.qmp", {"command": "query-status"})
    finally:
        ctl.shutdown()


def t_uart_cycle(env):
    """B1: the banner of every boot reaches the log (QEMU's file chardev
    truncates without append=on)."""
    ctl = env.controller()
    try:
        def banners():
            return sorted({r.obj["boot_id"] for r in ctl.log.since(0)
                           if r.kind == "uart" and "STIX ASW" in r.line})
        ctl.command("platform.power.on", {})
        ctl.command("platform.power.boot", {})
        wait_for(lambda: banners() == [1], what="banner of boot 1")
        ctl.command("platform.power.cycle", {"off_time_s": 0.1})
        wait_for(lambda: banners() == [1, 2], what="banner of boot 2")
        # a reader that meets a truncated file starts over at 0
        uart = os.path.join(ctl.run_dir, "uart.raw")
        open(uart, "wb").close()
        with open(uart, "ab") as f:
            f.write(b"STIX ASW after truncation ver. 9\r\n")
        wait_for(lambda: any("after truncation" in r.line
                             for r in ctl.log.since(0)
                             if r.kind == "uart"), what="truncated reread")
    finally:
        ctl.shutdown()


def t_run_lock(env):
    """S2: one controller per run dir; QEMU inherits the lock and dies
    with its parent (PDEATHSIG), also when spawned from a short-lived
    (HTTP request) thread."""
    d = env.run_dir()
    cfg = env.cfg()
    ctl = core_mod.Controller(cfg, run_dir=d)
    try:
        e = raises(rundir.RunDirError, core_mod.Controller, cfg, run_dir=d)
        check("in use" in str(e), "second controller: %s" % e)
        res = {}
        t = threading.Thread(target=lambda: res.update(
            ctl.command("platform.power.on", {})))
        t.start()
        t.join()
        time.sleep(0.5)          # the spawning thread is gone by now
        check(ctl.power.proc.alive(), "QEMU died with its spawning thread")
        lock = os.path.join(d, rundir.LOCK_NAME)
        ino = os.stat(lock).st_ino
        with open("/proc/locks") as f:
            locks = f.read()
        check(sum(":%d " % ino in ln for ln in locks.splitlines()) >= 1,
              "run-dir lock not held")
        fds = os.listdir("/proc/%d/fd" % res["pid"])
        check(any(os.path.realpath("/proc/%d/fd/%s" % (res["pid"], fd))
                  == os.path.realpath(lock) for fd in fds),
              "QEMU does not hold the run-dir lock")
    finally:
        ctl.shutdown()
    ctl = core_mod.Controller(cfg, run_dir=d)    # free again
    ctl.shutdown()
    # the parent dies hard: its QEMU follows (PDEATHSIG)
    d2 = env.run_dir()
    code = ("import sys,json; sys.path.insert(0,%r)\n"
            "from stixplat import core, config\n"
            "c = core.Controller(config.load(overrides=json.loads(%r)),"
            " run_dir=%r)\n"
            "print(c.command('platform.power.on', {})['pid'], flush=True)\n"
            "import os; os._exit(0)\n"
            % (os.path.dirname(HERE), json.dumps(env.overrides()), d2))
    import subprocess
    out = subprocess.run([sys.executable, "-c", code], capture_output=True,
                         timeout=30).stdout.decode().split()
    check(out, "the child controller printed no pid")
    pid = int(out[-1])
    wait_for(lambda: not os.path.exists("/proc/%d" % pid) or
             open("/proc/%d/stat" % pid).read().rsplit(")", 1)[1].split()[0]
             == "Z", what="QEMU to die with its parent (PDEATHSIG)")


def t_poller_robust(env):
    """S3: a malformed hw-state and a QEMU crash never kill the poller,
    and a command never ends in a dropped connection."""
    ctl = env.controller(qemu={"binary": env.bins["badstate"]})
    srv = api_mod.make_server(ctl, "127.0.0.1", 0)
    port = srv.server_address[1]
    post = {"Content-Type": "application/json", "X-Stix-Platform": "1"}
    try:
        ctl.command("platform.power.on", {})
        ctl.command("platform.power.boot", {})
        wait_for(lambda: any(r.obj["path"] == "poller.error"
                             for r in ctl.log.since(0)), what="poller.error")
        check(ctl.poller.is_alive(), "the poller died on a bad hw-state")
        n = sum(r.obj["path"] == "poller.error" for r in ctl.log.since(0))
        time.sleep(0.5)
        n2 = sum(r.obj["path"] == "poller.error" for r in ctl.log.since(0))
        check(n2 == n, "poller.error floods the log (%d -> %d)" % (n, n2))
        st, _, body = _http(port, "POST", "/api/v1/cmd/log.mark",
                            body=b'{"text": "x"}', headers=post)
        check(st == 200, "a command with a bad state poll: %d %r"
              % (st, body))
        os.kill(ctl.power.pid, 9)                       # a crash
        wait_for(lambda: ctl.power.state == "off", what="state off")
        check(any(r.kind == "power" and "unexpectedly" in
                  str(r.obj["data"].get("why")) for r in ctl.log.since(0)),
              "no record of the crash")
        time.sleep(0.3)
        check(ctl.poller.is_alive(), "the poller died with QEMU")
        ctl.cfg["qemu"]["binary"] = env.bins["new"]
        ctl.command("platform.power.on", {})
        ctl.command("platform.power.boot", {})
        wait_for(lambda: ctl.state.get("platform.obt") is not None,
                 what="polling resumes")

        def boom(**kw):
            raise RuntimeError("internal")
        ctl.dispatcher.add("x.boom", boom)
        st, _, body = _http(port, "POST", "/api/v1/cmd/x.boom", body=b"{}",
                            headers=post)
        check(st == 500 and json.loads(body)["status"] == 500,
              "internal error -> 500 JSON")
        orig = ctl.dispatcher.run
        ctl.dispatcher.run = lambda *a, **k: 1 / 0
        st, _, body = _http(port, "POST", "/api/v1/cmd/log.mark",
                            body=b'{"text": "y"}', headers=post)
        ctl.dispatcher.run = orig
        check(st == 500 and any(r.obj["path"] == "api.error"
                                for r in ctl.log.since(0)),
              "an error outside the dispatcher -> 500 + record")
    finally:
        srv.shutdown()
        srv.server_close()
        ctl.shutdown()


def t_sparse_poll(env):
    """S7: the poller re-checks the flash; above the limit it powers off
    and logs why."""
    ctl = env.controller(power={"sparse_check_s": 0.2,
                                "sparse_limit_mb": 1})
    try:
        ctl.command("platform.power.on", {})
        ctl.command("platform.power.boot", {})
        time.sleep(0.5)
        check(ctl.power.state == "running", "a sparse flash powered off")
        with open(os.path.join(ctl.run_dir, "flash.bin"), "r+b") as f:
            f.write(os.urandom(2 << 20))       # 2 MB of real blocks
            f.flush()
            os.fsync(f.fileno())
        wait_for(lambda: ctl.power.state == "off", what="sparse power-off")
        why = [r.obj["data"] for r in ctl.log.since(0)
               if r.obj["path"] == "platform.power.sparse" and
               not r.obj["data"].get("ok", True)]
        check(why and "not sparse" in why[-1]["error"], "sparse record")
    finally:
        ctl.shutdown()


def t_snapshot_race(env):
    """Q2: the poller and commands share one snapshot decision, so a
    periodic snapshot is logged once per interval."""
    ctl = env.controller(clock={"snapshot_s": 0.2, "state_hz": 50.0,
                                "sample_hz": 100.0})
    try:
        ctl.command("platform.power.on", {})
        ctl.command("platform.power.boot", {})
        stop = time.monotonic() + 1.5

        def hammer():
            while time.monotonic() < stop:
                ctl.poll_state()
        ts = [threading.Thread(target=hammer) for _ in range(4)]
        for t in ts:
            t.start()
        for t in ts:
            t.join()
        sims = [r.obj["sim_time"] for r in ctl.log.since(0)
                if r.obj["data"].get("reason") == "periodic"]
        check(len(sims) >= 3, "periodic snapshots: %r" % sims)
        gaps = [b - a for a, b in zip(sims, sims[1:])]
        check(min(gaps) >= 150_000_000, "duplicate snapshots: %r" % gaps)
    finally:
        ctl.shutdown()


SMOKES = [os.path.normpath(os.path.join(HERE, "..", "..", n)) for n in
          ("tc-smoke.sh", "nominal-smoke.sh", "fault-smoke.sh")]
SMOKE_TC_RE = re.compile(r'^(TC\w*)="((?:[0-9A-Fa-f]{2} ?)+)"', re.M)


def t_tcbuild(env):
    """PL-4: the CRC and the builder reproduce every TC unit of the
    smokes byte for byte (a corrupted-CRC unit must not reproduce)."""
    n = bad = 0
    for path in SMOKES:
        with open(path) as f:
            for name, hx in SMOKE_TC_RE.findall(f.read()):
                u = bytes.fromhex(hx)
                sp = u[4:]
                apid, seq, svc, sub, src, ack, ok = tcbuild.parse_tc(sp)
                rebuilt = tcbuild.unit(tcbuild.tc_packet(
                    apid, seq, svc, sub, sp[10:-2], ack=ack, source=src))
                if "BAD" in name:
                    check(not ok and rebuilt != u, "%s reproduced" % name)
                    bad += 1
                else:
                    check(ok and rebuilt == u, "%s: %s != %s"
                          % (name, rebuilt.hex(), u.hex()))
                    n += 1
    check(n >= 10 and bad == 1, "smoke TCs found: %d good, %d bad"
          % (n, bad))
    tc9 = tcbuild.tc9_129(2, 0x10000000, source=0)
    check(tc9[-2:] == b"\xfa\x63", "the tc-smoke TC(9,129) CRC FA 63")
    k = tcbuild.tc20_128(tcbuild.keepalive_seq(1_250_000_000))
    check(len(k) == 248 and k[4:6] == b"\x00\xf1" and
          (k[0] << 8 | k[1]) & 0x7FF == 0x31C and k[9] == 14 and
          k[6] & 0xF == 0 and tcbuild.parse_tc(k)[1] == 10 and
          tcbuild.parse_tc(k)[6], "TC(20,128) layout %s" % k[:12].hex())
    check(tcbuild.keepalive_seq(2048 * 1_000_000_000) == 0 and
          tcbuild.keepalive_seq(2047 * 1_000_000_000 + 999_999_999)
          == 16383, "8 Hz count wraps at 2^14")


def t_decode(env):
    """PL-4: the decoder on the probe self-test's real FSW units."""
    tm11 = bytes.fromhex("490200000da1c000000d100101000000001481af1dacc000")
    tm12 = bytes.fromhex("490200000da1c00200151001020000000014832a1dacc001"
                         "0002110112346f1f")
    hb = bytes.fromhex("490200000de5c00e001a1003190e10000003a053040000"
                       "0010000000000000000000000000")
    ev = bytes.fromhex("490200000da7c001000e10050100000000000000"
                       "142e")
    d = decode.decode(tm11)
    check(d["apid"] == 0x5A1 and (d["type"], d["subtype"]) == (1, 1) and
          d["tc"] == {"packet_id": 0x1DAC, "apid": 0x5AC, "seq": 0},
          "TM(1,1): %r" % d)
    d = decode.decode(tm12)
    check(d["subtype"] == 2 and d["fid"] == 2 and d["tc"]["seq"] == 1,
          "TM(1,2): %r" % d)
    d = decode.decode(hb)
    check(d.get("heartbeat") and d["cuc"] >> 16 == 0x10000003 and
          not d["unsynced"], "heartbeat: %r" % d)
    d = decode.decode(ev)
    check(d["eid"] == 0x142E and "30 s" in d["eid_name"], "event %r" % d)
    sid2 = bytes.fromhex("490200000da4c001000a1003190e00000000000002"
                         "15")
    sid2 = sid2[:8] + (len(sid2) - 11).to_bytes(2, "big") + sid2[10:]
    d = decode.decode(sid2)
    check(d["sid"] == 2 and d["mode"] == 5, "SID 2 mode %r" % d)
    check(decode.decode(bytes.fromhex("40020000" + "1dacc000000519110100"
                                      "d77e")) is None, "a TC is no TM")


def t_truncated(env):
    """NIT: truncated units never raise in the parser or the decoder:
    they decode as far as they go (None when there is no header)."""
    tc = tcbuild.tc9_129(3, 0x10000000)
    for n in range(len(tc)):
        p = tcbuild.parse_tc(tc[:n])
        check(p is None or not p[6], "a %d-byte TC parsed with a good CRC"
              % n)
    units = [bytes.fromhex(h) for h in (
        "490200000da1c00200151001020000000014832a1dacc0010002110112346f1f",
        "490200000de5c000001a1003190e000000013d1204000000010000000000000000"
        "00000000",
        "490200000da7c005000910110200000000148220")]
    units.append(_tm_unit(0x5A1, 1, 8, bytes.fromhex("1dacc0010002")))
    units.append(_tm_unit(0x5A7, 5, 1, bytes.fromhex("142e")))
    units.append(_tm_unit(0x5A4, 3, 25, bytes.fromhex("0215")))
    for u in units:
        for n in range(len(u)):
            d = decode.decode(u[:n])
            check(d is None if n < 20 else isinstance(d, dict),
                  "%d of %d bytes: %r" % (n, len(u), d))
            if d is not None:
                decode.describe(d)
    d = decode.decode(units[3][:-1])
    check("fid" not in d and d["tc"]["seq"] == 1, "TM(1,8) cut in the FID "
          "%r" % d)
    d = decode.decode(units[4][:-1])
    check("eid" not in d, "TM(5,1) cut in the EID %r" % d)
    d = decode.decode(units[5][:-1])
    check(d["sid"] == 2 and "mode" not in d, "SID 2 cut before the mode")


def t_timesync_window(env):
    """D-PLd: the window logic against a scripted OBT and bench time."""
    check(timesync.plan_send((0x10000005 << 16) | 0x0000) ==
          (True, 0x10000006, 0), "a fresh second: send coarse + 1 now")
    check(timesync.plan_send((0x10000005 << 16) | 0x7FFF)[0],
          "just under half a second gone: still now")
    now, tgt, wait = timesync.plan_send((0x10000005 << 16) | 0xC000)
    check(not now and abs(wait - 300_000_000) < 1_000_000,
          "late in the second: wait for the rollover + 50 ms (%r)" % wait)
    rig = _TsRig((0x10000005 << 16) | 0xC000)

    def advance():
        time.sleep(0.05)
        rig.t[0] = 0.35
        rig.obt["v"] = (0x10000006 << 16) | 0x0CCC  # 50 ms past rollover
        rig.clk.sample(350_000_000)
        rig.clk.run_due()
    threading.Thread(target=advance).start()
    r = rig.ts.send()
    check(r["waited"] and r["coarse"] == 0x10000007 and len(rig.sent) == 1,
          "after the wait: the new coarse + 1 (%r)" % r)
    check(tcbuild.parse_tc(rig.sent[0][4:])[2:4] == (9, 129), "a TC(9,129)")
    e = raises(VerbError, rig.ts.send)
    check(e.status == 409 and "pending" in str(e), "R2: one in flight")
    rig.clk.sample(3_400_000_000)
    rig.clk.run_due()
    check(rig.ts.busy() is None and rig.ts.history[-1]["verdict"] ==
          "timeout", "3 s without TM(1,7): timeout")
    rig.obt["v"] = (0x10000010 << 16) | 0x1000
    r = rig.ts.send(coarse=0x10000020)
    check(r["warning"] and "TIMECNT" in r["warning"], "explicit coarse warn")
    rig.ack(r["seq"], 8, fid=48035)
    check(rig.ts.history[-1]["verdict"] == "fail" and "48035" in
          rig.ts.history[-1]["why"], "TM(1,8) 48035 fails")


class _TsRig:
    """A TimeSync with a scripted OBT and bench time, and helpers that
    feed it real-shaped TM and trace records."""

    def __init__(self, obt):
        self.t = [0.0]
        self.clk = SimClock(mono_fn=lambda: self.t[0])
        self.clk.process_started()
        self.clk.resumed()
        self.clk.sample(0)
        self.sent = []
        self.obt = {"v": obt}
        self.lg = log_mod.RecordLog(None, self.clk.now)
        self.ts = timesync.TimeSync(
            self.clk, self.lg,
            lambda cached=False: (self.obt["v"], self.clk.exact_sim()),
            self.sent.append, lambda: None, iter(range(99)).__next__)

    def trace(self, name, fmt, *a):
        self.lg.emit("qemu", "log", "trace." + name,
                     {"line": "1234@1762886400.123456:%s %s" % (name,
                                                                fmt % a)})

    def rx(self, n):
        self.trace("stixspw_rx_deliver",
                   "RX unit delivered: len=%u ctrl=0x%x sel=%u "
                   "vclock_ns=%d", n, 0x8000 | n, 3, 5)

    def emit(self, coarse):
        self.trace("stixspw_timecode_emit",
                   "time code emitted: coarse=0x%08x timecnt=%u", coarse,
                   coarse & 63)

    def ack(self, seq, sub, fid=None, dest=14):
        app = bytes.fromhex("1dac") + (0xC000 | seq).to_bytes(2, "big")
        if fid is not None:
            app += fid.to_bytes(2, "big")
        u = _tm_unit(0x5A1, 1, sub, app, dest=dest)
        self.ts.on_tm(u, decode.decode(u))

    def heartbeat(self, cuc):
        u = _tm_unit(0x5E5, 3, 25, b"\x04", cuc=cuc, dest=14)
        self.ts.on_tm(u, decode.decode(u))

    def sync(self, ordering="ok", tc20s=2, hb_off_s=0.0):
        """A full sync: the trace records in the given order, TM(1,7),
        then a heartbeat hb_off_s away from the OBT."""
        self.obt["v"] += 20 << 16
        self.obt["v"] &= ~0xFFFF
        r = self.ts.send()
        for _ in range(tc20s):
            self.rx(252)                    # TC20s interleave
        if ordering == "ok":
            self.rx(22)
            self.emit(r["coarse"])
        else:
            self.emit(r["coarse"])
            self.rx(22)
        self.rx(252)
        self.ack(r["seq"], 7)
        self.heartbeat(self.obt["v"] + int(hb_off_s * 65536))
        return r, self.ts.history[-1]


def t_timesync_verdicts(env):
    """SF1/SF2/SF7: the exact verdicts. Real-shaped trace records with
    interleaved TC20 RX deliveries; a 1 s-off CUC is late_apply; an ack
    to another destination is not ours."""
    rig = _TsRig(0x10000000 << 16)
    r, v = rig.sync()
    check(v["verdict"] == "ok" and v["ordering"] == "ok" and
          abs(v["diff_s"]) < 0.01, "a clean sync: %r" % v)
    r, v = rig.sync(ordering="late")
    check(v["verdict"] == "late_apply" and v["ordering"] == "late",
          "the TC9 RX after the applying emit (TC20 RX before it): %r" % v)
    r, v = rig.sync(hb_off_s=-1.0)
    check(v["verdict"] == "late_apply" and "1 s" in v["why"],
          "a CUC 1 s behind is late_apply: %r" % v)
    r, v = rig.sync(hb_off_s=0.4)
    check(v["verdict"] == "fail", "0.4 s off fails: %r" % v)
    r, v = rig.sync(hb_off_s=0.2)
    check(v["verdict"] == "ok", "0.2 s off is within 0.25 s: %r" % v)
    # an ack with dest != 14 (another source's TC with the same seq)
    rig.obt["v"] += 20 << 16
    rig.obt["v"] &= ~0xFFFF
    r = rig.ts.send()
    rig.ack(r["seq"], 7, dest=0)
    check(rig.ts.busy() is not None, "a dest 0 TM(1,7) cleared the sync")
    rig.ack(r["seq"], 8, fid=48035, dest=0)
    check(rig.ts.busy() is not None, "a dest 0 TM(1,8) failed the sync")
    rig.ack(r["seq"], 7)
    check(rig.ts.busy() is None, "the dest 14 TM(1,7) is ours")


def t_timesync_reserve(env):
    """NITs: the wait re-checks the 0.5 s rule, and a sync being prepared
    already counts as pending (the verb runs outside the dispatcher
    lock)."""
    rig = _TsRig((0x10000005 << 16) | 0xC000)
    errs = []

    def second():
        time.sleep(0.03)
        try:
            rig.ts.send()
        except VerbError as e:
            errs.append(e)

    def advance():
        time.sleep(0.08)
        # the OBT jumped while waiting: 0.9 s into the second again
        rig.t[0] = 0.35
        rig.obt["v"] = (0x10000009 << 16) | 0xE666
        rig.clk.sample(350_000_000)
        rig.clk.run_due()
        time.sleep(0.05)
        rig.t[0] = 0.6
        rig.obt["v"] = (0x1000000A << 16) | 0x0CCC
        rig.clk.sample(600_000_000)
        rig.clk.run_due()
    ths = [threading.Thread(target=f) for f in (second, advance)]
    for th in ths:
        th.start()
    r = rig.ts.send()
    for th in ths:
        th.join()
    check(r["coarse"] == 0x1000000B, "the re-check after an OBT jump "
          "waited for the next rollover: 0x%08X" % r["coarse"])
    check(len(errs) == 1 and errs[0].status == 409,
          "a send while one is being prepared: %r" % errs)


def _tm_unit(apid, ptype, sub, app, cuc=0, seq=0, dest=0):
    body = bytes((0x10, ptype, sub, dest)) + cuc.to_bytes(6, "big") + app
    sp = (0x0800 | apid).to_bytes(2, "big") + \
        (0xC000 | seq).to_bytes(2, "big") + \
        (len(body) - 1).to_bytes(2, "big") + body
    return bytes((0x49, 0x02, 0, 0)) + sp


def t_keepalive(env):
    """D-PLe: bench-paced, 8 Hz count, suppressed while it cannot send,
    started by the first TM."""
    t = [0.0]
    clk = SimClock(mono_fn=lambda: t[0])
    clk.process_started()
    clk.resumed()
    clk.sample(0)
    sent = []
    why = {"v": None}
    cfg = config_mod.load()
    lg = log_mod.RecordLog(None, clk.now)
    ka = keepalive_mod.KeepAlive(cfg, clk, lg, sent.append,
                                 lambda: why["v"])
    ka.on_boot()

    def run_to(ns):
        while clk.exact_sim() < ns:
            clk.sample(clk.vclock + 5_000_000)
            clk.run_due()
    run_to(3_000_000_000)
    check(not sent, "nothing before the first TM")
    ka.on_tm(None, None)
    run_to(6_000_000_000 + 1_000_000)
    check(len(sent) == 24, "one per 125 ms (the default): %d" % len(sent))
    seqs = [tcbuild.parse_tc(u[4:])[1] for u in sent]
    check(seqs == list(range(25, 49)), "8 Hz count, one step per TC20 %r"
          % seqs)
    clk.stopped()
    for _ in range(50):
        clk.sample(clk.vclock)
        clk.run_due()
    check(len(sent) == 24, "no keep-alive while paused")
    clk.resumed()
    why["v"] = "the SpW link is down"
    run_to(9_000_000_000 + 1_000_000)
    check(len(sent) == 24 and ka.suppressed == 24, "suppressed %d"
          % ka.suppressed)
    why["v"] = None
    ka.set(enabled=False)
    run_to(12_000_000_000)
    check(len(sent) == 24, "disabled")


def _ka_rig(**kcfg):
    t = [0.0]
    clk = SimClock(mono_fn=lambda: t[0])
    clk.process_started()
    clk.resumed()
    clk.sample(0)
    sent = []
    cfg = config_mod.load(overrides={"keepalive": kcfg} if kcfg else None)
    lg = log_mod.RecordLog(None, clk.now)
    ka = keepalive_mod.KeepAlive(cfg, clk, lg, lambda u: sent.append(u),
                                 lambda: None)
    return clk, lg, ka, sent


def _ka_recs(lg):
    return [r.obj["data"] for r in lg.since(0)
            if r.kind == "tc" and r.obj["path"] == "platform.keepalive"]


def t_keepalive_drift(env):
    """SF9: re-armed from the previous deadline, so irregular samples
    (70 ms here, the poller runs at 20 Hz) do not stretch the period."""
    clk, lg, ka, sent = _ka_rig(autostart="boot")
    ka.on_boot()
    while clk.exact_sim() < 10 * NS:
        clk.sample(clk.vclock + 70_000_000)
        clk.run_due()
    check(79 <= len(sent) <= 80, "80 TC20 in 10 s at 125 ms, got %d"
          % len(sent))
    # a stall (a slow host) is not caught up with a burst
    n = len(sent)
    clk.sample(clk.vclock + 2 * NS)
    clk.run_due()
    check(len(sent) == n + 1, "a 2 s stall gave %d TCs" % (len(sent) - n))
    check(ka.status()["missed"] >= 15, "missed slots counted %r"
          % ka.status())


def t_keepalive_log(env):
    """SF9: aggregate logging, one summary every N sends plus the first
    and the last of each run and any failure, never one per TC."""
    clk, lg, ka, sent = _ka_rig(autostart="boot", summary_every=80)
    ka.on_boot()
    while clk.exact_sim() < 30 * NS + NS // 100:
        clk.sample(clk.vclock + 5_000_000)
        clk.run_due()
    recs = _ka_recs(lg)
    check(len(sent) == 240, "240 sent: %d" % len(sent))
    check(all(r.get("summary") for r in recs), "a per-TC record: %r"
          % [r for r in recs if not r.get("summary")][:1])
    check([r["reason"] for r in recs] == ["first", "every", "every",
                                          "every"],
          "summaries %r" % [r["reason"] for r in recs])
    check(recs[1]["sent"] == 80 and recs[3]["sent"] == 240 and
          recs[3]["last_seq"] == tcbuild.parse_tc(sent[-1][4:])[1],
          "summary counts %r" % recs[3])
    ka.set(enabled=False)
    recs = _ka_recs(lg)
    check(recs[-1]["reason"] == "last" and recs[-1]["sent"] == 240,
          "the last of the run %r" % recs[-1])
    # a failure is logged at once
    clk2, lg2, ka2, _ = _ka_rig(autostart="boot")

    def broken(u):
        raise OSError("ENETUNREACH")
    ka2.send_unit = broken
    ka2.on_boot()
    for _ in range(40):
        clk2.sample(clk2.vclock + 5_000_000)
        clk2.run_due()
    fails = [r for r in _ka_recs(lg2) if r["reason"] == "failure"]
    check(len(fails) == 1 and "ENETUNREACH" in fails[0]["error"],
          "one failure record per failing run %r" % _ka_recs(lg2))


def t_keepalive_trace(env):
    """SF9: the RX-delivery trace of each TC20 is not logged (only
    counted); any other unit's is."""
    ctl = env.controller(keepalive={"autostart": "boot"})
    try:
        ka = ctl.plat.keepalive
        line = ("stixspw_rx_deliver RX unit delivered: len=%d ctrl=0x10f8 "
                "sel=3 vclock_ns=5")
        with ka.lock:
            ka.outstanding = 1
        ctl._qemu_line("trace", line % 252)
        ctl._qemu_line("trace", line % 252)     # no TC20 outstanding
        ctl._qemu_line("trace", line % 22)
        lens = [r.obj["data"]["line"] for r in ctl.log.since(0)
                if r.obj["path"] == "trace.stixspw_rx_deliver"]
        check(len(lens) == 2 and "len=22" in lens[1],
              "logged rx_deliver %r" % lens)
        check(ka.status()["rx_deliver"] == 1, "counted %r" % ka.status())
    finally:
        ctl.shutdown()


def _tc9s(sock, wait=1.5):
    """The TC(9,129) units that reach QEMU's TC port within @wait s."""
    out = []
    end = time.monotonic() + wait
    while time.monotonic() < end:
        sock.settimeout(max(0.01, end - time.monotonic()))
        try:
            u = sock.recvfrom(4096)[0]
        except socket.timeout:
            break
        p = tcbuild.parse_tc(u[4:])
        if p and p[2:4] == (9, 129):
            out.append(u)
    return out


def t_timesync_auto(env):
    """B2: the automatic time sync, once per boot (also after a guest
    reset), logged; autostart = first_tm | boot | off."""
    for mode in ("first_tm", "boot", "off"):
        ctl = env.controller(timesync={"autostart": mode},
                             keepalive={"enabled": False})
        qemu_tc = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        qemu_tc.bind(("127.0.0.1", ctl.cfg["ports"]["qemu_tc"]))
        snd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        tm = ("127.0.0.1", ctl.cfg["ports"]["qemu_tm"])
        try:
            ctl.command("platform.power.on", {})
            ctl.command("platform.power.boot", {})
            got = _tc9s(qemu_tc, 0.8)
            check(len(got) == (1 if mode == "boot" else 0),
                  "%s: %d syncs at boot before any TM" % (mode, len(got)))
            for i in range(3):
                snd.sendto(_tm_unit(0x5A7, 5, 1, b"\x00\x22", seq=i), tm)
            got = _tc9s(qemu_tc)
            check(len(got) == (1 if mode == "first_tm" else 0),
                  "%s: %d syncs after the first TMs" % (mode, len(got)))
            autos = [r.obj["data"] for r in ctl.log.since(0)
                     if r.obj["path"] == "platform.timesync.auto"]
            check(len(autos) == (0 if mode == "off" else 1) and
                  all(a["trigger"] == mode for a in autos),
                  "%s: auto records %r" % (mode, autos))
            if mode == "off":
                continue
            # free the slot (no FSW here to answer), then a warm reset
            ts = ctl.plat.timesync
            with ts.lock:
                if ts.pending:
                    ts._verdict(ts.pending, "timeout", "the test")
            ctl.power.qmp.execute("x-fake-guest-reset")
            wait_for(lambda: _power_recs(ctl, "guest_reset"), what="reset")
            snd.sendto(_tm_unit(0x5A7, 5, 1, b"\x00\x22", seq=9), tm)
            got = _tc9s(qemu_tc)
            check(len(got) == 1, "%s: %d syncs after a guest reset"
                  % (mode, len(got)))
        finally:
            qemu_tc.close()
            snd.close()
            ctl.shutdown()


def t_relay(env):
    """D-PLg: the relay is byte-identical, one tm.jsonl line per TM; the
    proxy adds the SpW header; a GS TC while the link is down is dropped
    and logged (or held)."""
    ctl = env.controller(gs={"tc_during_link_down": "drop"},
                         timesync={"autostart": "off"})
    gs = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    gs.bind(("127.0.0.1", 0))
    gs.settimeout(2)
    qemu_tc = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    qemu_tc.bind(("127.0.0.1", ctl.cfg["ports"]["qemu_tc"]))
    qemu_tc.settimeout(2)
    ctl.plat.relay.dests = [gs.getsockname()]
    snd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        ctl.command("platform.power.on", {})
        ctl.command("platform.power.boot", {})
        units = [_tm_unit(0x5E5, 3, 25, b"\x04" + bytes(i), seq=i)
                 for i in range(5)]
        for u in units:
            snd.sendto(u, ("127.0.0.1", ctl.cfg["ports"]["qemu_tm"]))
        got = [gs.recvfrom(4096)[0] for _ in units]
        check(got == [u[4:] for u in units], "relay not byte-identical")
        wait_for(lambda: ctl.plat.relay.tm_count == 5, what="5 TMs")
        with open(os.path.join(ctl.run_dir, "tm.jsonl")) as f:
            lines = [json.loads(x) for x in f]
        check([x["hex"] for x in lines] == [u[4:].hex() for u in units],
              "tm.jsonl")
        check(sum(r.kind == "tm" for r in ctl.log.since(0)) == 5,
              "tm records")
        tc = tcbuild.tc_packet(0x5AC, 3, 17, 1, source=0)
        snd.sendto(tc, ("127.0.0.1", ctl.cfg["ports"]["gs_tc"]))
        check(qemu_tc.recvfrom(4096)[0] == tcbuild.unit(tc), "proxy")
        ctl.command("platform.spw.link", {"state": "down"})
        snd.sendto(tc, ("127.0.0.1", ctl.cfg["ports"]["gs_tc"]))
        wait_for(lambda: any(r.kind == "tc" and r.obj["data"].get("dropped")
                             for r in ctl.log.since(0)), what="drop record")
        qemu_tc.settimeout(0.3)
        raises(socket.timeout, qemu_tc.recvfrom, 4096)
        q = ctl.power.qmp
        sets = q.execute("x-fake-set-log")
        check(["/machine/stixspw", "link-down", True] in sets,
              "link-down set in QEMU: %r" % sets)
        e = raises(VerbError, ctl.command, "platform.timesync.send", {})
        check(e.status == 409, "time sync while the link is down")
        ctl.plat.relay.policy = "hold"
        snd.sendto(tc, ("127.0.0.1", ctl.cfg["ports"]["gs_tc"]))
        wait_for(lambda: ctl.plat.relay.held, what="held")
        ctl.command("platform.spw.link", {"state": "up"})
        check(qemu_tc.recvfrom(4096)[0] == tcbuild.unit(tc), "held TC")
    finally:
        for sk in (gs, qemu_tc, snd):
            sk.close()
        ctl.shutdown()


def t_gs_security(env):
    """SF6: the GS side is loopback-only unless in a container; the GS
    emulation never poses as the platform (source 14); a GS TC claiming
    source 14 is refused and logged; [::1] works for TM and TC."""
    bad = ['[gs]\nbind = "0.0.0.0"\n',
           '[gs]\ntm_dests = ["10.0.0.5:21501"]\n',
           '[gs]\ntm_dests = ["127.0.0.1:21501", "gs.example:21501"]\n',
           '[gs]\nemulation_source = 14\n',
           '[gs]\nemulation_source = 256\n',
           '[gs]\ntm_dests = ["::1:21501"]\n',
           '[gs]\ntm_dests = ["[::1:21501"]\n']
    for text in bad:
        raises(config_mod.ConfigError, config_mod.load, text=text)
    for text in ('[gs]\nbind = "0.0.0.0"\ntm_dests = ["10.0.0.5:21501"]\n'
                 '[api]\ncontainer = true\n',
                 '[gs]\nbind = "::1"\ntm_dests = ["[::1]:21501", '
                 '"localhost:21501"]\n'):
        config_mod.load(text=text)
    gs6 = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    gs6.bind(("::1", 0))
    gs6.settimeout(2)
    ctl = env.controller(gs={"tm_dests": ["[::1]:%d"
                                          % gs6.getsockname()[1]],
                             "bind": "::1"},
                         timesync={"autostart": "off"})
    qemu_tc = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    qemu_tc.bind(("127.0.0.1", ctl.cfg["ports"]["qemu_tc"]))
    qemu_tc.settimeout(2)
    snd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        ctl.command("platform.power.on", {})
        ctl.command("platform.power.boot", {})
        u = _tm_unit(0x5E5, 3, 25, b"\x04")
        snd.sendto(u, ("127.0.0.1", ctl.cfg["ports"]["qemu_tm"]))
        check(gs6.recvfrom(4096)[0] == u[4:], "TM to [::1]")
        tc14 = tcbuild.tc_packet(0x5AC, 5, 9, 129, bytes(6), source=14)
        gs6.sendto(tc14, ("::1", ctl.cfg["ports"]["gs_tc"]))
        wait_for(lambda: [r for r in ctl.log.since(0)
                          if r.obj["path"] == "tc.refused"],
                 what="the tc.refused record")
        ok_tc = tcbuild.tc_packet(0x5AC, 6, 17, 1, source=0)
        gs6.sendto(ok_tc, ("::1", ctl.cfg["ports"]["gs_tc"]))
        check(qemu_tc.recvfrom(4096)[0] == tcbuild.unit(ok_tc),
              "a GS TC over [::1] (and not the source-14 one first)")
        rec = [r.obj["data"] for r in ctl.log.since(0)
               if r.obj["path"] == "tc.refused"][0]
        check(rec["source"] == 14 and not rec["delivered"], "refused %r"
              % rec)
    finally:
        for sk in (gs6, qemu_tc, snd):
            sk.close()
        ctl.shutdown()


def t_hold_queue(env):
    """SF5: the hold queue is capped (below the FSW's 64-unit queue),
    with every drop logged, and is dropped (logged) at power off and at
    boot, so stale TCs never reach a new boot."""
    ctl = env.controller(gs={"tc_during_link_down": "hold", "hold_max": 3},
                         timesync={"autostart": "off"})
    snd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    gs_tc = ("127.0.0.1", ctl.cfg["ports"]["gs_tc"])
    try:
        check(config_mod.load()["gs"]["hold_max"] == 32, "default 32")
        raises(config_mod.ConfigError, config_mod.load,
               text="[gs]\nhold_max = 65\n")
        raises(config_mod.ConfigError, config_mod.load,
               text="[gs]\nhold_max = 0\n")
        ctl.command("platform.power.on", {})
        ctl.command("platform.power.boot", {})
        ctl.command("platform.spw.link", {"state": "down"})
        for i in range(5):
            snd.sendto(tcbuild.tc_packet(0x5AC, i, 17, 1), gs_tc)

        def gs_recs():
            return [r.obj["data"] for r in ctl.log.since(0)
                    if r.kind == "tc" and r.obj["path"] == "tc.gs"]
        wait_for(lambda: len(gs_recs()) == 5, what="5 tc records")
        held = [d for d in gs_recs() if d.get("held")]
        full = [d for d in gs_recs() if d.get("dropped")]
        check(len(ctl.plat.relay.held) == 3 and len(held) == 3 and
              len(full) == 2 and all("full" in d["why"] for d in full),
              "cap 3: %d held, %r" % (len(ctl.plat.relay.held), full))
        ctl.command("platform.power.off", {})
        flush = [r.obj["data"] for r in ctl.log.since(0)
                 if r.obj["path"] == "tc.gs.held"]
        check(not ctl.plat.relay.held and len(flush) == 1 and
              flush[0]["dropped"] == 3 and flush[0]["why"] == "off",
              "dropped at off: %r" % flush)
        # TCs held while off are dropped at the boot
        snd.sendto(tcbuild.tc_packet(0x5AC, 9, 17, 1), gs_tc)
        wait_for(lambda: len(ctl.plat.relay.held) == 1, what="held off")
        ctl.command("platform.power.on", {})
        ctl.command("platform.power.boot", {})
        flush = [r.obj["data"] for r in ctl.log.since(0)
                 if r.obj["path"] == "tc.gs.held"]
        check(not ctl.plat.relay.held and flush[-1]["why"] == "boot" and
              flush[-1]["seqs"] == [9], "dropped at boot: %r" % flush)
    finally:
        snd.close()
        ctl.shutdown()


def t_platform_tcs(env):
    """D-PLa + R2: platform TCs are services 9 and 20 from source 14;
    TC(9,129) waits while a sync is pending; tc.gs.send is gated."""
    ctl = env.controller()
    qemu_tc = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    qemu_tc.bind(("127.0.0.1", ctl.cfg["ports"]["qemu_tc"]))
    qemu_tc.settimeout(2)
    try:
        cmd = ctl.command
        e = raises(VerbError, cmd, "tc.platform.send", {
            "hex": tcbuild.tc9_129(0, 0x10000000).hex()})
        check(e.status == 409, "off: 409")
        cmd("platform.power.on", {})
        cmd("platform.power.boot", {})
        for svc, sub, src, st in ((17, 1, 14, 403), (236, 6, 14, 403),
                                  (9, 129, 0, 403), (20, 128, 121, 403)):
            pkt = tcbuild.tc_packet(0x5AC, 1, svc, sub, source=src)
            e = raises(VerbError, cmd, "tc.platform.send",
                       {"hex": pkt.hex()})
            check(e.status == st, "TC(%d,%d) src %d: %s" % (svc, sub, src,
                                                            e))
        k = tcbuild.tc20_128(1)
        cmd("tc.platform.send", {"hex": k.hex()})
        check(qemu_tc.recvfrom(4096)[0] == tcbuild.unit(k), "TC20 sent")
        r = cmd("platform.timesync.send", {})
        qemu_tc.recvfrom(4096)
        e = raises(VerbError, cmd, "tc.platform.send",
                   {"hex": tcbuild.tc9_129(8, r["coarse"]).hex()})
        check(e.status == 409 and "pending" in str(e),
              "R2: raw TC(9,129) while a sync is pending: %s" % e)
        e = raises(VerbError, cmd, "platform.timesync.send", {})
        check(e.status == 409, "second timesync.send: 409")
        # SF3: a raw TC(9,129) is a sync in flight like timesync.send
        ts = ctl.plat.timesync
        with ts.lock:
            ts._verdict(ts.pending, "timeout", "the test clears it")
        bad = bytearray(tcbuild.tc9_129(7, 0x10000000))
        bad[-1] ^= 0xFF                 # a malformed (bad CRC) variant
        cmd("tc.platform.send", {"hex": bytes(bad).hex()})
        qemu_tc.recvfrom(4096)
        check(ts.busy() is not None and ts.pending["seq"] == 7 and
              ts.pending["raw"], "the raw TC(9,129) is pending: %r"
              % ts.pending)
        e = raises(VerbError, cmd, "platform.timesync.send", {})
        check(e.status == 409, "timesync.send after a raw TC(9,129): %s"
              % e)
        check(ctl.dispatcher.verbs["platform.timesync.send"]["locked"] is
              False, "timesync.send holds the dispatcher lock")
        e = raises(VerbError, cmd, "tc.gs.send", {"service": 17,
                                                   "subtype": 1})
        check(e.status == 403, "tc.gs.send gated in interactive mode")
        ctl.cfg["gs"]["emulation"] = "on"
        r = cmd("tc.gs.send", {"service": 17, "subtype": 1})
        u = qemu_tc.recvfrom(4096)[0]
        check(r["label"] == "GS-EMULATION" and u[4 + 9] == 121 and
              tcbuild.parse_tc(u[4:])[6], "GS emulation from 121")
    finally:
        qemu_tc.close()
        ctl.shutdown()


def _fault_rig():
    t = [0.0]
    clk = SimClock(mono_fn=lambda: t[0])
    clk.process_started()
    clk.resumed()
    clk.sample(0)
    lg = log_mod.RecordLog(None, clk.now)
    calls = []
    fail = {"v": False}

    def apply(entry, p):
        if fail["v"]:
            raise VerbError(409, "STIX is off")
        calls.append(("apply", entry["id"]))
        return "prior-" + entry["id"]

    def undo(entry, p, prior):
        calls.append(("undo", entry["id"], prior))
    acts = {k: (apply, undo) for k in faults_mod.ACTIONS}
    f = faults_mod.Faults(faults_mod.load(), lg, clk, acts)

    def recs():
        return [r.obj["data"] for r in lg.since(0) if r.kind == "fault"]
    return clk, f, calls, fail, recs


def t_fault_lifecycle(env):
    """SF4: validate before active; an armed fault can be disarmed; an
    apply failure at its trigger is logged and leaves nothing pending;
    permanent and transient faults are one-shot events; undo gets the
    state saved at apply."""
    clk, f, calls, fail, recs = _fault_rig()
    e = raises(VerbError, f.inject, "spw.link.disconnect",
               trigger={"at": "at_sim"})
    check(e.status == 400 and f.status() == {}, "a bad trigger left %r"
          % f.status())
    f.inject("spw.link.disconnect")         # not 409: nothing was left
    check(calls == [("apply", "spw.link.disconnect")], "applied %r" % calls)
    f.clear("spw.link.disconnect")
    check(calls[-1] == ("undo", "spw.link.disconnect",
                        "prior-spw.link.disconnect"),
          "undo with the saved prior state %r" % (calls[-1],))
    # armed, then disarmed by clear
    r = f.inject("keepalive.missing",
                 trigger={"at": "at_sim", "sim_time": 2 * NS})
    check(r["armed"] and f.status()["keepalive.missing"]["pending"],
          "armed %r" % f.status())
    f.clear("keepalive.missing")
    clk.sample(3 * NS)
    clk.run_due()
    check(("apply", "keepalive.missing") not in calls and
          f.status() == {}, "a disarmed fault applied: %r" % calls)
    check(recs()[-1]["action"] == "disarm", "disarm record %r" % recs()[-1])
    # an apply failure at the trigger (the poller thread)
    fail["v"] = True
    f.inject("timecode.missing",
             trigger={"at": "at_sim", "sim_time": 4 * NS})
    clk.sample(5 * NS)
    clk.run_due()
    last = recs()[-1]
    check(last["action"] == "error" and "STIX is off" in last["error"] and
          f.status() == {}, "apply failure %r / %r" % (last, f.status()))
    fail["v"] = False
    # permanent and transient faults are events: not active, repeatable
    for fid in ("obt.jump", "obt.jump", "timecode.wrong_value"):
        f.inject(fid)
    check(f.status() == {} and f.active_targets() == {},
          "one-shot faults listed as active %r" % f.status())
    e = raises(VerbError, f.clear, "obt.jump")
    check(e.status == 409, "clear of a one-shot fault")
    # a non-reversible fault armed in the future can still be disarmed
    f.inject("obt.jump", trigger={"at": "at_sim", "sim_time": 9 * NS})
    f.clear("obt.jump")
    n = calls.count(("apply", "obt.jump"))
    clk.sample(10 * NS)
    clk.run_due()
    check(calls.count(("apply", "obt.jump")) == n, "disarmed obt.jump ran")


QOM_FAULT = {
    "id": "test.qom.adc7", "class": "signal", "changes": "presented",
    "target": "instrument.hk_adc.7", "mode": "stuck_at",
    "params": {"value": {"type": "integer", "min": 0, "max": 4095,
                         "default": 100}},
    "persistence": "until_cleared", "reversible": True,
    "action": {"kind": "qom", "path": "/machine/stixctrl",
               "property": "adc-inject[7]", "param": "value",
               "clear": 0xFFFFFFFF},
    "preconditions": [], "expected_detection": []}


def t_fault_model(env):
    """SF8: every entry says whether it changes truth or presented (the
    attenuator amendment), and the generic qom action reaches only the
    STIX devices."""
    cat = faults_mod.load()
    check(all(f["changes"] in ("truth", "presented")
              for f in cat["faults"]), "changes on every entry")
    byid = {f["id"]: f for f in cat["faults"]}
    check(byid["spw.link.disconnect"]["changes"] == "truth" and
          byid["thermal.psu.overtemp"]["changes"] == "presented",
          "a link fault is truth, a sensor reading presented")
    base = {k: v for k, v in cat["faults"][0].items() if k != "changes"}
    for broken in (base, dict(base, changes="both")):
        raises(faults_mod.CatalogueError, faults_mod.validate,
               {"api": 1, "faults": [broken]})
    faults_mod.validate({"api": 1, "faults": [QOM_FAULT]})
    for act in ({"kind": "qom", "path": "/machine/stixflash",
                 "property": "x", "param": "value"},
                {"kind": "qom", "path": "/machine/peripheral/stixctrl",
                 "property": "x", "param": "value"},
                {"kind": "qom", "path": "/machine/stixctrl/../unattached",
                 "property": "x", "param": "value"},
                {"kind": "qom", "path": "/machine/stixctrl",
                 "property": 3, "param": "value"},
                {"kind": "qom", "path": "/machine/stixctrl",
                 "property": "x", "param": "nosuch"}):
        raises(faults_mod.CatalogueError, faults_mod.validate,
               {"api": 1, "faults": [dict(QOM_FAULT, action=act)]})
    faults_mod.validate({"api": 1, "faults": [dict(QOM_FAULT, action=dict(
        QOM_FAULT["action"], path="/machine/stixctrl-att"))]})
    path = os.path.join(env.tmp, "cat-qom.json")
    with open(path, "w") as f:
        json.dump({"api": 1, "faults": [QOM_FAULT]}, f)
    ctl = env.controller(faults={"catalogue": path})
    try:
        ctl.command("platform.power.on", {})
        ctl.command("platform.power.boot", {})
        ctl.command("fault.inject", {"id": "test.qom.adc7",
                                     "params": {"value": 321}})
        ctl.command("fault.clear", {"id": "test.qom.adc7"})
        sets = ctl.power.qmp.execute("x-fake-set-log")
        check(sets[-2:] == [["/machine/stixctrl", "adc-inject[7]", 321],
                            ["/machine/stixctrl", "adc-inject[7]",
                             0xFFFFFFFF]], "qom fault sets %r" % sets[-2:])
    finally:
        ctl.shutdown()


def t_faults(env):
    """D-PLq: the catalogue validates; inject/clear are fault records with
    seeds; forcing is re-applied after a warm reset."""
    cat = faults_mod.load()
    check(len(cat["faults"]) >= 8, "v1 catalogue")
    for broken in ({"api": 1, "faults": {}},
                   {"api": 1, "faults": ["x"]},
                   {"api": 1, "faults": [dict(cat["faults"][0],
                                              params=[])]},
                   {"api": 1, "faults": [dict(cat["faults"][0],
                                              action="link_down")]},
                   {"api": 1, "faults": [dict(cat["faults"][0],
                                              expected_detection={})]},
                   {"api": 1, "faults": [dict(cat["faults"][5],
                                              target="instrument.hk_adc.5")
                                         ]},
                   {"api": 1, "faults": [dict(cat["faults"][5],
                                              action={"kind": "adc",
                                                      "ch": 200},
                                              target="instrument.hk_adc.200")
                                         ]},
                   {"api": 2, "faults": []},
                   {"api": 1, "faults": [{"id": "x"}]},
                   {"api": 1, "faults": [dict(cat["faults"][0],
                                              **{"class": "nope"})]},
                   {"api": 1, "faults": [cat["faults"][0],
                                         cat["faults"][0]]}):
        raises(faults_mod.CatalogueError, faults_mod.validate, broken)
    ctl = env.controller()
    try:
        cmd = ctl.command
        cmd("platform.power.on", {})
        cmd("platform.power.boot", {})
        # SF4: undo restores the operator's state from before the fault
        cmd("platform.spw.link", {"state": "down"})
        cmd("platform.timecode.enable", {"enabled": False})
        for fid in ("spw.link.disconnect", "timecode.missing"):
            cmd("fault.inject", {"id": fid})
            cmd("fault.clear", {"id": fid})
        check(ctl.plat.link_down and ctl.plat.timecodes is False,
              "undo forced the operator's down/off back to up/on")
        cmd("platform.spw.link", {"state": "up"})
        cmd("platform.timecode.enable", {"enabled": True})
        r = cmd("fault.inject", {"id": "thermal.psu.overtemp",
                                 "params": {"value": 3900}, "seed": 7})
        check(r["seed"] == 7, "seed")
        recs = [x.obj for x in ctl.log.since(0) if x.kind == "fault"]
        check(recs[-1]["data"]["action"] == "inject" and
              recs[-1]["data"]["seed"] == 7 and
              recs[-1]["data"]["params"] == {"value": 3900}, "fault rec")
        sets = ctl.power.qmp.execute("x-fake-set-log")
        check(["/machine/stixctrl", "adc-inject[40]", 3900] in sets,
              "adc-inject[40] set: %r" % sets)
        e = raises(VerbError, cmd, "fault.inject",
                   {"id": "thermal.psu.overtemp"})
        check(e.status == 409, "double inject")
        e = raises(VerbError, cmd, "fault.inject",
                   {"id": "thermal.psu.overtemp.x"})
        check(e.status == 404, "unknown id")
        e = raises(VerbError, cmd, "fault.inject",
                   {"id": "obt.jump", "params": {"delta_s": 10 ** 9}})
        check(e.status == 400, "param range")
        r = cmd("fault.inject", {"id": "spw.link.disconnect"})
        check(r["seed"] is not None and ctl.plat.link_down, "link fault")
        # a warm guest reset clears adc-inject and link-down in QEMU:
        # both are re-applied
        ctl.power.qmp.execute("x-fake-guest-reset")
        wait_for(lambda: _power_recs(ctl, "guest_reset"), what="reset")
        wait_for(lambda: sum(x == ["/machine/stixctrl", "adc-inject[40]",
                                   3900] for x in
                             ctl.power.qmp.execute("x-fake-set-log")) == 2,
                 what="adc re-applied after the warm reset")
        cmd("fault.clear", {"id": "spw.link.disconnect"})
        cmd("fault.clear", {"id": "thermal.psu.overtemp"})
        sets = ctl.power.qmp.execute("x-fake-set-log")
        check(sets[-1] == ["/machine/stixctrl", "adc-inject[40]",
                           0xFFFFFFFF] and not ctl.plat.link_down,
              "cleared: %r" % sets[-2:])
        e = raises(VerbError, cmd, "fault.clear", {"id": "obt.jump"})
        check(e.status == 409, "not reversible")
        st, = [ctl.status()["faults"]]
        check(st == {}, "no active faults: %r" % st)
        # forcing directly
        cmd("force.set", {"path": "instrument.hk_adc.24", "value": 1800})
        e = raises(VerbError, cmd, "force.set",
                   {"path": "instrument.hk_adc.999", "value": 1})
        check(e.status == 400, "bad force path")
        check(ctl.status()["forcings"] == {"instrument.hk_adc.24": 1800},
              "forcings in status")
    finally:
        ctl.shutdown()


def _fake_children():
    """Our live child fake QEMUs (pids)."""
    out = []
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        try:
            with open("/proc/%s/stat" % d) as f:
                st = f.read().rsplit(")", 1)[1].split()
            with open("/proc/%s/cmdline" % d, "rb") as f:
                cmd = f.read()
        except OSError:
            continue
        if int(st[1]) == os.getpid() and st[0] != "Z" and \
                b"fake_qemu.py" in cmd:
            out.append(int(d))
    return out


def t_no_orphan(env):
    """B2: a QEMU that never answers QMP is killed, never orphaned, and
    the next power-on spawns exactly one process."""
    ctl = env.controller(qemu={"binary": env.bins["noqmp"]},
                         power={"qmp_timeout_s": 0.5})
    try:
        e = raises(VerbError, ctl.command, "platform.power.on", {})
        check(e.status == 500 and ctl.power.state == "off",
              "a mute QEMU: %s" % e)
        check(_fake_children() == [], "a live QEMU after the failed on: %r"
              % _fake_children())
        ctl.cfg["qemu"]["binary"] = env.bins["new"]
        ctl.command("platform.power.on", {})
        kids = _fake_children()
        check(kids == [ctl.power.pid], "exactly one QEMU: %r" % kids)
    finally:
        ctl.shutdown()
    check(_fake_children() == [], "a live QEMU after shutdown")


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
        for body in (b'{"text": "x", "data": {"a": NaN}}',
                     b'{"text": "x", "data": {"a": Infinity}}',
                     b'{"text": "x", "data": {"a": -Infinity}}'):
            st, _, b2 = _http(port, "POST", "/api/v1/cmd/log.mark",
                              body=body, headers=post)
            check(st == 400, "NaN/Infinity accepted: %d %r" % (st, b2))
        raises(ValueError, ctl.log.emit, "api", "log", "x",
               {"v": float("nan")})
        for cl in ("-5", "abc", "1e3"):
            st = _raw_post(port, cl)
            check(st == 400, "Content-Length %r -> %r" % (cl, st))
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


def _raw_post(port, content_length):
    """A POST with a hand-written Content-Length; the status (or None
    for a dropped connection)."""
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall(("POST /api/v1/cmd/log.mark HTTP/1.1\r\nHost: localhost\r\n"
               "Content-Type: application/json\r\nX-Stix-Platform: 1\r\n"
               "Content-Length: %s\r\nConnection: close\r\n\r\n{}"
               % content_length).encode())
    try:
        data = s.recv(4096)
    except OSError:
        data = b""
    s.close()
    m = re.match(rb"HTTP/1\.[01] (\d+)", data)
    return int(m.group(1)) if m else None


def _sse_read(port, headers, n_events, path="/api/v1/stream",
              after_connect=None):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    req = "GET %s HTTP/1.1\r\nHost: localhost\r\n" % path
    for k, v in headers.items():
        req += "%s: %s\r\n" % (k, v)
    s.sendall((req + "\r\n").encode())
    buf = b""
    events = []
    end = time.monotonic() + 5
    while len(events) < n_events and time.monotonic() < end:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        head, _, rest = buf.partition(b"\r\n\r\n")
        if not rest:
            continue
        if after_connect is not None:
            time.sleep(0.2)        # the handler is waiting for records
            after_connect()
            after_connect = None
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

        def burst():
            # more records than the ring holds arrive while the stream
            # waits: the live stream must say so (S4)
            with ctl.log.cond:
                for i in range(10):
                    ctl.log.emit("ctl", "log", "burst", {})
        head, evs4 = _sse_read(port, {}, 4, after_connect=burst)
        check(evs4 and evs4[0].startswith(b"event: gap"),
              "mid-stream gap event: %r" % evs4[:2])
        ids = [int(re.match(rb"id: (\d+)", e).group(1)) for e in evs4[1:]]
        check(ids == list(range(ids[0], ids[0] + len(ids))) and
              ids[-1] == ctl.log.seq, "after the gap: %r" % ids)
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
         ("uart across a power cycle", t_uart_cycle),
         ("no orphaned QEMU", t_no_orphan),
         ("run-dir lock + PDEATHSIG", t_run_lock),
         ("poller + command robustness", t_poller_robust),
         ("periodic sparse check", t_sparse_poll),
         ("snapshot race", t_snapshot_race),
         ("tcbuild: CRC + smoke TCs", t_tcbuild),
         ("decoder on real units", t_decode),
         ("truncated TC/TM input", t_truncated),
         ("time sync window (D-PLd)", t_timesync_window),
         ("time sync verdicts", t_timesync_verdicts),
         ("time sync reserve + re-check", t_timesync_reserve),
         ("time sync autostart (B2)", t_timesync_auto),
         ("keep-alive pacing", t_keepalive),
         ("keep-alive: no drift", t_keepalive_drift),
         ("keep-alive: aggregate log", t_keepalive_log),
         ("keep-alive: rx trace filter", t_keepalive_trace),
         ("TM relay + TC proxy", t_relay),
         ("hold queue (SF5)", t_hold_queue),
         ("GS side security (SF6)", t_gs_security),
         ("platform TCs (9/20, R2)", t_platform_tcs),
         ("fault lifecycle (SF4)", t_fault_lifecycle),
         ("fault catalogue + forcing", t_faults),
         ("fault model: truth|presented, qom", t_fault_model),
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
