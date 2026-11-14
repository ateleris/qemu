# SPDX-License-Identifier: GPL-2.0-or-later
"""`stixplat.py --self-test`: offline, no QEMU (platform plan PL-3).

Fake binaries (tests/fake_qemu.py) stand in for QEMU; fake filesystems
and clocks for the rest. Every case prints one line; the last line is
"SELF-TEST: PASS" or "SELF-TEST: FAIL".
"""

import gzip
import hashlib
import http.client
import json
import os
import random
import re
import shutil
import socket
import sys
import tempfile
import threading
import time
import types

from . import api as api_mod
from . import config as config_mod
from . import decode
from . import faults as faults_mod
from . import idb as idb_mod
from . import keepalive as keepalive_mod
from . import series as series_mod
from . import tcbuild
from . import timeline as timeline_mod
from . import timesync
from . import viewrun as viewrun_mod
from . import views as views_mod
from . import core as core_mod
from . import log as log_mod
from . import probe_lib
from . import qemu as qemu_mod
from . import relay as relay_mod
from . import retention
from . import scenario
from . import rundir
from . import state as state_mod
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
    # FF plan Task 3: the pacing keys (0/wall = today's behaviour;
    # fast is sleep=off, so it needs icount > 0)
    c = config_mod.load()
    check(c["qemu"]["icount_shift"] == 0 and c["qemu"]["pace"] == "wall",
          "no icount by default")
    for bad in ('[qemu]\nicount_shift = 16\n',
                '[qemu]\nicount_shift = -1\n',
                '[qemu]\npace = "slow"\n',
                '[qemu]\nicount_shift = 0\npace = "fast"\n'):
        raises(config_mod.ConfigError, config_mod.load, text=bad)
    c = config_mod.load(text='[qemu]\nicount_shift = 7\npace = "fast"\n')
    check(c["qemu"]["icount_shift"] == 7 and c["qemu"]["pace"] == "fast",
          "a fast profile needs icount > 0")
    # A1: the timeline sampler keys (-1 = by mode, 0 = off, 0.5..60)
    c = config_mod.load()
    check(c["timeline"]["sample_s"] == -1.0 and
          c["timeline"]["idb_json"].startswith("~/dev/") and
          c["timeline"]["views_dir"] == "",
          "the timeline defaults")
    for bad in ('[timeline]\nsample_s = 0.4\n',
                '[timeline]\nsample_s = 61\n',
                '[timeline]\nsample_s = -2\n',
                '[timeline]\nsample_s = "1"\n',
                '[timeline]\nsample_paths = [1]\n'):
        raises(config_mod.ConfigError, config_mod.load, text=bad)
    c = config_mod.load(text='[timeline]\nsample_s = 0.5\n')
    check(c["timeline"]["sample_s"] == 0.5, "the sampler floor 0.5 s")


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
    # the pacing keys (FF plan Task 3): -icount only when shift > 0,
    # +sleep=off for fast
    for icount, pace, want in ((0, "wall", None), (7, "wall", "shift=7"),
                               (7, "fast", "shift=7,sleep=off")):
        c = config_mod.load(overrides={"qemu": {
            "binary": "/q/qemu-system-sparc", "elf": "/e/asw.elf",
            "icount_shift": icount, "pace": pace}})
        argv = qemu_mod.build_argv(c, "/r")
        got = [argv[i + 1] for i, a in enumerate(argv) if a == "-icount"]
        check(got == ([] if want is None else [want]),
              "icount %d %s: %r" % (icount, pace, got))


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
    # the persistent master (paths.sh: STIX_UPREV_DIR); none of these
    # paths needs to exist, guard matches on the realpath
    master = rundir.SHARED[0]
    for bad in (master, master + "/", master + "/sub", master + "-s1",
                "/tmp/stix-uprev-run", "/tmp/stix-baseline-run",
                "/tmp/stix-plat-run", "/tmp/stix-uprev-run/"):
        raises(rundir.RunDirError, rundir.guard, bad)
    link = os.path.join(env.tmp, "sneaky")
    os.symlink("/tmp/stix-uprev-run", link)
    raises(rundir.RunDirError, rundir.guard, link)
    mlink = os.path.join(env.tmp, "masterlink")
    os.symlink(master, mlink)             # dangling if master is absent
    raises(rundir.RunDirError, rundir.guard, mlink)
    for bad in ("/tmp/stix-uprev-run-s1", "/tmp/stix-uprev-run-s12/x",
                "/tmp/stix-uprev-run/sub/dir", "/tmp/stix-baseline-run/a",
                "/", "/tmp/stix-d4-run-s3"):
        raises(rundir.RunDirError, rundir.guard, bad)
    deep = os.path.join(env.tmp, "deeplink")
    os.symlink("/tmp/stix-uprev-run-s2/inner", deep)
    raises(rundir.RunDirError, rundir.guard, deep)
    # "*" must not cross "/": these are not gate run dirs
    for good in ("/tmp/stix-plat/core-smoke", "/tmp/stix-plat/x-run",
                 "/tmp/stix-plat/a/b-run-s1", "/tmp/stix-uprev-runner",
                 master + "ner"):        # .../uprev-runner next to it
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


# -- PL-5: the scenario runner against a scripted world ------------------
class FakeWorld:
    """A scripted sim clock and record stream for the Runner (the ctx
    protocol of scenario.py). @script: [(sim_s, fn(world))]. A pause
    [(sim_s, ticks)] freezes sim time for that many sleeps."""

    def __init__(self, script=(), pauses=(), on_cmd=None):
        self.t = 0
        self.recs = []
        self.cmds = []
        self.states = {}
        self.script = sorted(script, key=lambda x: x[0])
        self.pauses = dict(pauses)
        self.paused = 0
        self.on_cmd = on_cmd or {}
        self.epoch = 0x10000000 << 16
        self.obt_skew = 0
        self.power = "running"
        self.wall = 0.0

    def now(self):
        return self.t

    def mono(self):
        return self.wall

    def power_state(self):
        return self.power

    def sleep(self, s):
        self.wall += s
        if self.paused:
            self.paused -= 1
            return
        self.t += int(s * NS)
        while self.script and self.script[0][0] * NS <= self.t:
            self.script.pop(0)[1](self)
        for at in list(self.pauses):
            if at * NS <= self.t:
                self.paused = self.pauses.pop(at)

    def rec(self, kind, path, data):
        r = {"seq": len(self.recs) + 1, "sim_time": self.t, "kind": kind,
             "path": path, "source": "tm", "data": data}
        self.recs.append(r)
        return r

    def tm(self, apid, ptype, sub, app, cuc=None):
        u = _tm_unit(apid, ptype, sub, app,
                     cuc=self.obt() if cuc is None else cuc)
        return self.rec("tm", "tm.0x%03x" % apid,
                        {"hex": u[4:].hex(), "decoded": decode.decode(u)})

    def event(self, eid, params=b""):
        return self.tm(0x5A7, 5, 1, eid.to_bytes(2, "big") + params)

    def obt(self):
        return self.epoch + self.t * 65536 // NS + self.obt_skew

    # the ctx protocol
    def last_seq(self):
        return len(self.recs)

    def records(self, since):
        return [r for r in self.recs if r["seq"] > since]

    def command(self, verb, args):
        self.cmds.append((self.t / NS, verb, args))
        fn = self.on_cmd.get(verb)
        return fn(self, args) if fn else {}

    def state(self, path):
        if path not in self.states:
            return None
        return {"truth": self.states[path], "presented": self.states[path]}

    def read_obt(self):
        return self.obt(), self.t

    def emit(self, kind, path, data):
        return self.rec(kind, path, data)


def _sc(steps, variants=None, duration_s=30, **top):
    d = {"api": 1, "name": "t", "duration_s": duration_s,
         "setup": [], "steps": steps, "teardown": []}
    if variants:
        d["variants"] = variants
    d.update(top)
    return scenario.validate(d)


def _run(sc, world, variant=None):
    r = scenario.Runner(sc, variant, world, tick_s=0.1)
    return r, r.run()


def _verdicts(res):
    return [st["verdict"] for st in res["steps"]]


# split so checkpatch does not take it for this file's tag
SPDX = "SPDX-License-" "Identifier:"


def t_scn_validate(env):
    """PL-5: scenario errors are ScenarioError (exit 2), never a pass."""
    good = {"api": 1, "name": "ok", "duration_s": 10,
            "steps": [{"at": 0, "await_tm": ["0x5E5:3:25@5"]}]}
    scenario.validate(good)
    for bad in ({}, dict(good, api=2), dict(good, name="a b"),
                dict(good, duration_s=0), dict(good, nosuch=1),
                dict(good, steps=[{"at": 0, "nosuch": 1}]),
                dict(good, steps=[{"await_tm": ["0x5E5:3:25@5"]}]),
                dict(good, steps=[{"at": 0, "after": 1}]),
                dict(good, steps=[{"at": 0, "await_tm": ["0x5E5:3:25"]}]),
                dict(good, steps=[{"at": 0, "await_tm": ["junk@5"]}]),
                dict(good, steps=[{"at": 0, "assert_no_tm": ["0x5A1:1:8@3"]}]),
                dict(good, steps=[{"at": 0, "await_event": [{"eid": "14a"}]}]),
                dict(good, steps=[{"at": 0, "await_uart": [
                    {"re": "(", "timeout_s": 1}]}]),
                dict(good, steps=[{"at": 0, "await_state": [
                    {"path": "x", "timeout_s": 1}]}]),
                dict(good, steps=[{"at": 0, "await_state": [
                    {"path": "x", "eq": 1, "gt": 0, "timeout_s": 1}]}]),
                dict(good, steps=[{"at": 0, "assert_consistent": ["idpu.*"]}]),
                dict(good, steps=[{"at": 0, "variants": ["x"]}]),
                dict(good, variants=["a", "a"]),
                dict(good, config={"qemu": {"binary": "/bin/sh"}}),
                dict(good, variant_config={"nope": {}}),
                dict(good, steps=[{"at": 0, "do": "x", "args": []}])):
        e = raises(scenario.ScenarioError, scenario.validate, bad)
        check(scenario.EXIT["error"] == 2, "exit 2")
    e = raises(scenario.ScenarioError, scenario.validate,
               dict(good, steps=[{"at": 0, "assert_consistent": ["a"]}]))
    check("PL-8" in str(e), "assert_consistent names PL-8: %s" % e)
    # whole-line "#" comments (the SPDX tag) as in the fault catalogue
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "c.json")
        with open(p, "w") as f:
            f.write("# " + SPDX + " GPL-2.0-or-later\n" +
                    json.dumps(good) + "\n  # trailing\n")
        check(scenario.load(p)["name"] == "ok", "commented scenario loads")
        with open(p, "w") as f:
            f.write(json.dumps(dict(good, description="a # b"), indent=1)
                    .replace('"description"', '\n  # c\n  "description"')
                    + "\n")
        check(scenario.load(p)["description"] == "a # b",
              "a '#' inside a value round-trips")
    # the shipped scenarios: all valid, each with its SPDX line, and
    # their config overrides merge
    shipped = sorted(f for f in os.listdir(scenario.DEFAULT_DIR)
                     if f.endswith(".json"))
    check({"gate-spw-link.json", "gate-keepalive.json", "gate-timesync.json",
           "gate-negative.json"} <= set(shipped), "gates: %r" % shipped)
    for f in shipped:
        p = os.path.join(scenario.DEFAULT_DIR, f)
        with open(p) as fh:
            check(fh.readline().startswith("# " + SPDX),
                  "%s: SPDX first" % f)
        sc = scenario.load(p)
        for v in sc["variants"]:
            config_mod.merged(config_mod.load(), sc["config"],
                              sc["variant_config"].get(v, {}))


def t_scn_runner(env):
    """PL-5: awaits are unordered within their window and armed at their
    step; a late match is a timeout; forbids arm at their step; silence
    windows; the verdict and the evidence."""
    w = FakeWorld(script=[
        (1, lambda w: w.event(0x1429)),         # before step 2 arms
        (3, lambda w: w.event(0x142D, b"\x00")),
        (4, lambda w: w.event(0x14A3)),
        (12, lambda w: w.event(0x5435, b"\x48")),
        (16, lambda w: w.tm(0x5E5, 3, 25, b"\x04"))])
    sc = _sc([
        {"at": 0, "do": "platform.power.boot",
         "await_event": [{"eid": "14a3", "timeout_s": 6},
                         {"eid": "142d", "data": "00", "timeout_s": 6}]},
        {"at": 2, "assert_no_event": [{"eid": "1429"}],
         "await_event": [{"eid": "5435", "data": "48", "timeout_s": 5}]},
        {"at": 4, "assert_silence": {"after_s": 1, "for_s": 6}},
        {"at": 13, "assert_no_tm": ["0x5E5:3:25"]},
        {"at": 13, "assert_no_tm": [{"tm": "0x5E5:3:25", "for_s": 2}]}],
        duration_s=20)
    r, res = _run(sc, w)
    check(_verdicts(res) == ["pass", "timeout", "pass", "fail", "pass"],
          "verdicts %r" % _verdicts(res))
    check(res["verdict"] == "fail" and scenario.exit_code(res) == 1,
          "overall %r" % res["verdict"])
    st = res["steps"][0]["checks"][0]
    check(st["evidence"]["matched_seq"] is not None and
          st["evidence"]["seq_from"] <= st["evidence"]["matched_seq"],
          "evidence %r" % st)
    check(w.cmds[0][1] == "platform.power.boot", "do ran")
    exp = [x for x in w.recs if x["kind"] == "expect"]
    check(len(exp) >= 5 and all(x["path"].startswith("scenario.t")
                                for x in exp), "expect records")
    # a record fetched in the same tick as the deadline, but stamped
    # after it: too late for an await, outside a forbid's for_s
    w = FakeWorld(script=[(5.05, lambda w: w.event(0x14A3)),
                          (6.05, lambda w: w.event(0x1429))])
    r, res = _run(_sc([{"at": 0, "await_event": [{"eid": "14a3",
                                                  "timeout_s": 5}]},
                       {"at": 4, "assert_no_event": [{"eid": "1429",
                                                      "for_s": 2}]}],
                      duration_s=8), w)
    check(_verdicts(res) == ["timeout", "pass"],
          "stamped past the deadline %r" % _verdicts(res))
    # a silence broken
    w = FakeWorld(script=[(5, lambda w: w.tm(0x5E5, 3, 25, b"\x04"))])
    r, res = _run(_sc([{"at": 0, "assert_silence": {"after_s": 2,
                                                    "for_s": 5}}]), w)
    check(_verdicts(res) == ["fail"], "silence broken %r" % _verdicts(res))
    # an await still open at the end is a timeout, never a pass
    w = FakeWorld()
    r, res = _run(_sc([{"at": 0, "await_tm": ["0x5E5:3:25@50"]}],
                      duration_s=5), w)
    check(_verdicts(res) == ["timeout"], "open at the end %r" % res)


def t_scn_variants(env):
    """PL-5: steps filtered by variant globs; T0 is the boot; `after`."""
    sc = _sc([{"at": 0, "do": "platform.power.boot",
               "assert_no_tm": ["0x5A1:1:8"]},
              {"at": 5, "do": "fault.inject", "variants": ["fault_*"],
               "args": {"id": "x"}},
              {"after": 2, "do": "fault.clear",
               "variants": ["fault_with_recovery"], "args": {"id": "x"}}],
             variants=["nominal", "fault_no_recovery",
                       "fault_with_recovery"], duration_s=10)
    runs = {}
    for v in sc["variants"]:
        w = FakeWorld(on_cmd={"platform.power.boot":
                              lambda w, a: w.sleep(1.5)})
        _run(sc, w, v)
        runs[v] = [(round(t, 1), verb) for t, verb, _ in w.cmds]
    check(runs["nominal"] == [(0.0, "platform.power.boot")], "%r" % runs)
    check([v for _, v in runs["fault_no_recovery"]] ==
          ["platform.power.boot", "fault.inject"] and
          runs["fault_no_recovery"][1][0] == 6.5,
          "at is from T0, the boot (1.5 s): %r" % runs)
    check(runs["fault_with_recovery"][2] == (8.5, "fault.clear"),
          "after: 2 s after the previous step %r" % runs)
    raises(scenario.ScenarioError, scenario.Runner, sc, "other",
           FakeWorld())


def t_scn_pause_abort(env):
    """PL-5: timeouts are sim time (a pause inside one does not expire
    it); abort ends the run with errors; a failing do is an error."""
    w = FakeWorld(script=[(6, lambda w: w.event(0x14A3))],
                  pauses=[(3, 200)])        # 200 ticks = 20 s of wall
    r, res = _run(_sc([{"at": 0, "await_event": [{"eid": "14a3",
                                                  "timeout_s": 4}]}],
                      duration_s=8), w)
    check(_verdicts(res) == ["timeout"], "6 s sim > 4 s: %r" % res)
    w = FakeWorld(script=[(3.5, lambda w: w.event(0x14A3))],
                  pauses=[(3, 200)])
    r, res = _run(_sc([{"at": 0, "await_event": [{"eid": "14a3",
                                                  "timeout_s": 4}]}],
                      duration_s=8), w)
    check(_verdicts(res) == ["pass"], "the pause is not sim time %r" % res)
    holder = {}
    w = FakeWorld(script=[(2, lambda w: holder["r"].abort("test"))])
    sc = _sc([{"at": 0, "await_tm": ["0x5E5:3:25@9"]},
              {"at": 5, "do": "fault.inject", "args": {"id": "x"}}],
             teardown=[{"do": "platform.power.off"}], duration_s=10)
    r = scenario.Runner(sc, None, w, tick_s=0.1)
    holder["r"] = r
    res = r.run()
    check(res["verdict"] == "error" and _verdicts(res) == ["error",
                                                           "error"] and
          res["aborted"] == "test", "abort %r" % res)
    check([v for _, v, _ in w.cmds] == ["platform.power.off"],
          "teardown after abort, no fault.inject %r" % w.cmds)

    def boom(w, a):
        raise VerbError(409, "STIX is off")
    for power, verdict, code in (("running", "fail", 1),
                                 ("off", "error", 3)):
        w = FakeWorld(on_cmd={"fault.inject": boom})
        w.power = power
        r, res = _run(_sc([{"at": 1, "do": "fault.inject",
                            "args": {"id": "x"},
                            "await_tm": ["0x5E5:3:25@1"]}],
                          duration_s=3), w)
        check(_verdicts(res) == [verdict] and "STIX is off" in
              res["steps"][0]["why"] and scenario.exit_code(res) == code,
              "N1: a 409 with power %s: %r" % (power, res))
    w = FakeWorld(on_cmd={"fault.inject": lambda w, a: (_ for _ in ()).throw(
        VerbError(404, "no fault"))})
    r, res = _run(_sc([{"at": 1, "do": "fault.inject", "args": {"id": "x"},
                        "await_tm": ["0x5E5:3:25@1"]}], duration_s=3), w)
    check(scenario.exit_code(res) == 2, "a 404 stays a scenario error")

    def infra(w, a):
        raise VerbError(500, "QMP gone")
    w = FakeWorld(on_cmd={"fault.inject": infra})
    r, res = _run(_sc([{"at": 1, "do": "fault.inject",
                        "args": {"id": "x"},
                        "await_tm": ["0x5E5:3:25@1"]}], duration_s=3), w)
    check(scenario.exit_code(res) == 3, "a 5xx is infra %r" % res)
    for status, code in ((409, 3), (400, 2)):
        def refuse(w, a, status=status):
            raise VerbError(status, "refused")
        w = FakeWorld(on_cmd={"platform.power.on": refuse})
        r, res = _run(_sc([{"at": 0, "await_tm": ["0x5E5:3:25@1"]}],
                          setup=[{"do": "platform.power.on"}],
                          duration_s=2), w)
        check(scenario.exit_code(res) == code and
              _verdicts(res) == ["error"],
              "a %d in the setup exits %d: %r" % (status, code, res))


def t_scn_state_cuc(env):
    """PL-5: await_state predicates and expect_cuc."""
    def setter(path, v):
        return lambda w: w.states.__setitem__(path, v)
    w = FakeWorld(script=[(1, setter("a", "down")), (1, setter("n", 3)),
                          (2, setter("n", 7)), (3, setter("n", 7.05)),
                          (4, setter("s", 1)), (4.5, setter("s", 2))])
    w.states.update({"s": 0, "c": 5})
    sc = _sc([{"at": 0, "await_state": [
        {"path": "a", "eq": "down", "timeout_s": 3},
        {"path": "a", "ne": "up", "timeout_s": 3},
        {"path": "n", "gt": 5, "timeout_s": 3},
        {"path": "n", "lt": 4, "timeout_s": 3},
        {"path": "n", "within": [7, 0.1], "timeout_s": 4},
        {"path": "a", "in_set": ["down", "x"], "timeout_s": 3},
        {"path": "s", "changed": True, "timeout_s": 6},
        {"path": "s", "stable_for": 2, "timeout_s": 8}]},
        {"at": 0, "await_state": [{"path": "c", "changed": True,
                                   "timeout_s": 2}]},
        {"at": 3.5, "await_state": [{"path": "s", "stable_for": 3,
                                     "timeout_s": 3}]}], duration_s=9)
    r, res = _run(sc, w)
    check(_verdicts(res) == ["pass", "timeout", "timeout"],
          "state %r" % [[c["verdict"] for c in st["checks"]]
                        for st in res["steps"]])
    w = FakeWorld(script=[(1, lambda w: w.tm(0x5E5, 3, 25, b"\x04")),
                          (3, lambda w: setattr(w, "obt_skew", 40 << 16)),
                          (4, lambda w: w.tm(0x5E5, 3, 25, b"\x04",
                                             cuc=w.obt() - (40 << 16)))])
    sc = _sc([{"at": 0, "expect_cuc": [{"apid": "0x5E5", "within_s": 0.3,
                                        "timeout_s": 2}]},
              {"at": 2, "expect_cuc": [{"apid": "0x5E5", "outside_s": 10,
                                        "timeout_s": 3}]},
              {"at": 2, "expect_cuc": [{"apid": "0x5E5", "within_s": 0.3,
                                        "timeout_s": 3}]}], duration_s=6)
    r, res = _run(sc, w)
    check(_verdicts(res) == ["pass", "pass", "fail"], "cuc %r" % res)


SCN_PASS = {"api": 1, "name": "fake-pass", "duration_s": 3,
            "setup": [{"do": "platform.power.on"}],
            "steps": [{"at": 0, "do": "platform.power.boot",
                       "await_uart": [{"re": "STIX ASW", "timeout_s": 2}],
                       "await_state": [{"path": "platform.power",
                                        "eq": "running", "timeout_s": 2}]}],
            "teardown": [{"do": "platform.power.off"}]}
SCN_FAIL = {"api": 1, "name": "fake-fail", "duration_s": 2,
            "variants": ["nominal", "fault_x"],
            "setup": [{"do": "platform.power.on"}],
            "steps": [{"at": 0, "do": "platform.power.boot",
                       "await_state": [{"path": "platform.power",
                                        "eq": "running", "timeout_s": 1}]},
                      {"at": 0.2, "variants": ["fault_*"],
                       "await_tm": ["0x5A7:5:1:142d@1"]}],
            "teardown": [{"do": "platform.power.off"}]}
SCN_LONG = {"api": 1, "name": "fake-long", "duration_s": 60,
            "setup": [{"do": "platform.power.on"}],
            "steps": [{"at": 0, "do": "platform.power.boot",
                       "await_tm": ["0x5A7:5:1:142d@50"]}],
            "teardown": [{"do": "platform.power.off"}]}


def _scn_dir(env):
    d = os.path.join(env.tmp, "scn-%d" % env.n)
    os.makedirs(d, exist_ok=True)
    for sc in (SCN_PASS, SCN_FAIL, SCN_LONG):
        with open(os.path.join(d, sc["name"] + ".json"), "w") as f:
            json.dump(sc, f)
    with open(os.path.join(d, "broken.json"), "w") as f:
        json.dump(dict(SCN_PASS, name="broken", steps=[
            {"at": 0, "assert_consistent": ["idpu.*"]}]), f)
    with open(os.path.join(d, "nested.json"), "w") as f:
        json.dump(dict(SCN_PASS, name="nested", steps=[
            {"at": 0, "do": "scenario.run", "args": {"file": "x"}}]), f)
    return d


def t_scn_verbs(env):
    """PL-5: scenario.run / scenario.abort on a live controller (the
    fake QEMU): one at a time, files from the scenario dir only."""
    d = _scn_dir(env)
    ctl = env.controller(run={"scenario_dir": d})
    try:
        cmd = ctl.command
        r = cmd("scenario.run", {"file": "fake-pass.json"})
        check(r["started"] and r["scenario"] == "fake-pass", "%r" % r)
        e = raises(VerbError, cmd, "scenario.run", {"file": "fake-pass.json"})
        check(e.status == 409, "a second scenario: %s" % e)
        wait_for(lambda: (ctl.status()["scenario"] or {}).get("phase") ==
                 "done", timeout=15, what="the scenario")
        st = ctl.status()["scenario"]
        check(st["verdict"] == "pass", "verdict %r" % st)
        check(ctl.power.state == "off", "teardown powered off")
        ends = [x.obj["data"] for x in ctl.log.since(0)
                if x.kind == "expect" and
                x.obj["path"] == "scenario.fake-pass" and
                x.obj["data"].get("phase") == "end"]
        check(ends and ends[0]["verdict"] == "pass", "end record %r" % ends)
        for f, status in (("../x.json", 403), ("/etc/passwd", 403),
                          ("nosuch.json", 404), ("broken.json", 400),
                          ("nested.json", 400)):
            e = raises(VerbError, cmd, "scenario.run", {"file": f})
            check(e.status == status, "%s: %s" % (f, e))
        e = raises(VerbError, cmd, "scenario.run",
                   {"file": "fake-fail.json"})
        check(e.status == 400 and "variant" in str(e), "variant needed")
        e = raises(VerbError, cmd, "scenario.abort", {})
        check(e.status == 409, "nothing to abort")
        cmd("scenario.run", {"file": "fake-long.json"})
        wait_for(lambda: ctl.power.state == "running", timeout=10,
                 what="the long scenario's boot")
        cmd("scenario.abort", {})
        wait_for(lambda: ctl.status()["scenario"]["phase"] == "done",
                 timeout=15, what="the abort")
        check(ctl.status()["scenario"]["verdict"] == "error" and
              ctl.power.state == "off", "abort: %r"
              % ctl.status()["scenario"])
    finally:
        ctl.shutdown()


def t_platform_leaves(env):
    """PL-5: the platform counters scenarios await on are state leaves,
    refreshed by the poller (volatile: no change record per TC20)."""
    ctl = env.controller(keepalive={"autostart": "boot"},
                         timesync={"autostart": "off"})
    try:
        ctl.command("platform.power.on", {})
        ctl.command("platform.power.boot", {})
        wait_for(lambda: (ctl.state.get("platform.keepalive.sent") or
                          {}).get("truth", 0) > 2, what="sent leaf")
        ctl.command("platform.spw.link", {"state": "down"})
        wait_for(lambda: (ctl.state.get("platform.keepalive.suppressed") or
                          {}).get("truth", 0) > 2, what="suppressed leaf")
        ch = [r for r in ctl.log.since(0) if r.kind == "state" and
              "platform.keepalive.sent" in json.dumps(r.obj["data"]) and
              not r.obj["data"].get("snapshot")]
        check(not ch, "change records for a volatile counter: %d" % len(ch))
        check(ctl.state.get("platform.timesync.verdict") is not None,
              "the verdict leaf exists")
    finally:
        ctl.shutdown()


def t_runmode(env):
    """PL-5: mode = "run": each scenario variant in its own controller,
    QEMU and run dir; reports, summary.json, the log slice; exit 0/1/2."""
    from . import runmode
    d = _scn_dir(env)
    root = os.path.join(env.tmp, "runs-root-%d" % env.n)

    def cfg(scns, **over):
        o = env.overrides(**over)
        o["mode"] = "run"
        o.setdefault("run", {}).update(scenario_dir=d, scenarios=scns)
        o["retention"] = {"run_root": root, "min_free_mb": 1}
        return config_mod.load(overrides=o)
    out = []
    rc = runmode.run_all(cfg(["fake-pass.json", "fake-fail.json"]),
                         out=out.append)
    check(rc == 1, "a failing variant: exit %d (%r)" % (rc, out))
    reps = sorted(os.listdir(os.path.join(root, "reports")))
    check(len(reps) == 1, "one report dir %r" % reps)
    rdir = os.path.join(root, "reports", reps[0])
    with open(os.path.join(rdir, "summary.json")) as f:
        summ = json.load(f)
    got = [(x["scenario"], x["variant"], x["verdict"]) for x in summ["runs"]]
    check(got == [("fake-pass", "default", "pass"),
                  ("fake-fail", "nominal", "pass"),
                  ("fake-fail", "fault_x", "fail")] and summ["exit"] == 1,
          "summary %r" % got)
    for x in summ["runs"]:
        check(os.path.exists(os.path.join(rdir, x["report"])), "report")
        with open(os.path.join(rdir, x["slice"])) as f:
            sl = [json.loads(ln) for ln in f]
        check(sl and sl[0]["seq"] > x["seq_from"] and
              sl[-1]["seq"] <= x["seq_to"], "the slice %r" % x)
        check(retention.read_marker(x["run_dir"])["status"] ==
              ("passed" if x["verdict"] == "pass" else "failed"),
              "marker %s" % x["run_dir"])
    with open(os.path.join(rdir, "summary.txt")) as f:
        txt = f.read()
    check("FAIL" in txt and "fake-fail.fault_x" in txt, "text %r" % txt)
    check(runmode.run_all(cfg(["fake-pass.json"]), out=out.append) == 0,
          "all passed: 0")
    n = len(os.listdir(os.path.join(root, "runs")))
    check(runmode.run_all(cfg(["fake-pass.json", "broken.json"]),
                          out=out.append) == 2 and
          len(os.listdir(os.path.join(root, "runs"))) == n,
          "a scenario error: 2, before anything runs")
    check(runmode.run_all(cfg(["fake-pass.json"],
                              qemu={"binary": env.bins["old"]}),
                          out=out.append) == 3, "a binary refused: 3")
    o = env.overrides()
    o["mode"] = "run"
    o["run"] = {"scenario_dir": d, "scenarios": ["fake-fail.json"],
                "variants": ["fault_*"]}
    o["retention"] = {"run_root": root, "min_free_mb": 1}
    out2 = []
    check(runmode.run_all(config_mod.load(overrides=o), out=out2.append)
          == 1 and sum("fake-fail" in x for x in out2) == 1,
          "the variant filter: %r" % out2)


def t_scn_review(env):
    """PL-5 review: an unexpected power-off is infra (S1); the wall-clock
    ceiling (S3); a variant without checks is an error (S5); seed is no
    bool (N7); expect_cuc mod_s/offset_s (S6); a QMP error reading the
    OBT is the check's error, not a crash (B1)."""
    from .qmp import QmpError

    def died(w):
        w.power = "off"
        w.rec("power", "platform.power", {"event": "off", "state": "off",
                                           "why": "QEMU exited unexpectedly"})

    def off_cmd(w, a):
        if w.power == "off":
            raise VerbError(409, "power is off (needs running)")
        w.power = "off"
        w.rec("power", "platform.power", {"event": "off", "state": "off",
                                           "why": "power off"})
        return {}
    w = FakeWorld(script=[(2, died)],
                  on_cmd={"platform.power.off": off_cmd})
    sc = _sc([{"at": 0, "await_tm": ["0x5E5:3:25@20"]}],
             teardown=[{"do": "platform.power.off"}], duration_s=30)
    r, res = _run(sc, w)
    check(res["infra"] and scenario.exit_code(res) == 3 and
          "unexpectedly" in (res["aborted"] or "") and w.t < 3 * NS and
          "teardown_error" not in res,
          "S1: QEMU died: %r (t %.1f)" % (res, w.t / NS))
    # a power-off a step's own verb caused is not one
    w = FakeWorld(on_cmd={"platform.power.off": off_cmd})
    sc = _sc([{"at": 1, "do": "platform.power.off",
               "await_state": [{"path": "p", "eq": 1, "timeout_s": 1}]},
              {"at": 2, "await_tm": ["0x5E5:3:25@1"]}], duration_s=4)
    w.states["p"] = 1
    r, res = _run(sc, w)
    check(not res["infra"] and res["aborted"] is None and
          _verdicts(res) == ["pass", "timeout"], "S1: an own off %r" % res)
    # S3: the sim clock stands still, the wall clock does not
    w = FakeWorld(pauses=[(1, 10 ** 6)])
    r = scenario.Runner(_sc([{"at": 0, "await_tm": ["0x5E5:3:25@5"]}],
                            duration_s=10), None, w, tick_s=0.1,
                        wall_s=20)
    res = r.run()
    check(scenario.exit_code(res) == 3 and "wall-clock" in res["aborted"]
          and 20 <= w.wall < 21, "S3: %r (wall %.1f)" % (res, w.wall))
    check(scenario.wall_ceiling(_sc([{"at": 0, "await_tm": [
        "0x5E5:3:25@5"]}], duration_s=100)) ==
        3 * 100 + scenario.BOOT_SLACK_S, "3 x duration_s + boot slack")
    # S5 and N7
    for bad in ([{"at": 0, "do": "log.mark", "args": {"text": "x"}}],
                [{"at": 0, "variants": ["b"],
                  "await_tm": ["0x5E5:3:25@5"]}]):
        e = raises(scenario.ScenarioError, _sc, bad, variants=["a", "b"])
        check("no check" in str(e) and "'a'" in str(e), "S5: %s" % e)
    raises(scenario.ScenarioError, _sc, [{"at": 0, "await_tm": [
        "0x5E5:3:25@5"]}], seed=True)
    _sc([{"at": 0, "await_tm": ["0x5E5:3:25@5"]}], seed=3)
    # S6: the CUC against the OBT modulo 64 s
    w = FakeWorld(script=[
        (1, lambda w: w.tm(0x5E5, 3, 25, b"\x04",
                           cuc=w.obt() + (32 << 16) + (64 << 20))),
        (3, lambda w: w.tm(0x5E5, 3, 25, b"\x04",
                           cuc=w.obt() + 0x1000)),
        (5, lambda w: w.tm(0x5E5, 3, 25, b"\x04",
                           cuc=w.obt() + (63 << 16) + 0xF000))])
    sc = _sc([{"at": 0, "expect_cuc": [{"apid": "0x5E5", "mod_s": 64,
                                        "offset_s": 32, "within_s": 0.3,
                                        "timeout_s": 2}]},
              {"at": 2, "expect_cuc": [{"apid": "0x5E5", "mod_s": 64,
                                        "offset_s": 32, "within_s": 0.3,
                                        "timeout_s": 2}]},
              {"at": 4, "expect_cuc": [{"apid": "0x5E5", "mod_s": 64,
                                        "within_s": 0.3,
                                        "timeout_s": 2}]}], duration_s=7)
    r, res = _run(sc, w)
    diffs = [st["checks"][0]["evidence"].get("diff_s")
             for st in res["steps"]]
    check(_verdicts(res) == ["pass", "fail", "pass"] and
          abs(diffs[2] + 0.0623) < 0.01,
          "S6: mod 64 %r %r" % (_verdicts(res), diffs))
    for bad in ({"mod_s": 0}, {"mod_s": True}, {"offset_s": "1"},
                {"mod": 64}):
        spec = {"apid": "0x5E5", "within_s": 0.3, "timeout_s": 1}
        spec.update(bad)
        if spec.get("within_s") is None:
            spec.pop("within_s")
        raises(scenario.ScenarioError, _sc, [{"at": 0, "expect_cuc":
                                              [spec]}])
    # B1: QmpError from the OBT read

    def no_obt():
        raise QmpError("QMP closed")
    w = FakeWorld(script=[(1, lambda w: w.tm(0x5E5, 3, 25, b"\x04"))])
    w.read_obt = no_obt
    r, res = _run(_sc([{"at": 0, "expect_cuc": [
        {"apid": "0x5E5", "within_s": 0.3, "timeout_s": 2}]}],
        duration_s=3), w)
    check(_verdicts(res) == ["error"] and "QMP closed" in
          res["steps"][0]["why"], "B1: QmpError %r" % res)


def t_runmode_review(env):
    """PL-5 review: a crashing Runner is infra and the bench still goes
    off (B1); KeyboardInterrupt too; an unknown verb is exit 2 before
    anything runs and a ScenarioError in run_one exit 2 (S4); SIGTERM
    aborts as infra (N6); the verb registry is the dispatcher's."""
    import signal
    from . import runmode, verbs as verbs_mod
    from .qmp import QmpError
    d = _scn_dir(env)
    root = os.path.join(env.tmp, "runs-root-r%d" % env.n)

    def cfg(scns, **over):
        o = env.overrides(**over)
        o["mode"] = "run"
        o.setdefault("run", {}).update(scenario_dir=d, scenarios=scns)
        o["retention"] = {"run_root": root, "min_free_mb": 1}
        return config_mod.load(overrides=o)
    sc = scenario.load(os.path.join(d, "fake-pass.json"))
    real = scenario.Runner.run
    for exc in (QmpError("boom"), RuntimeError("bug")):
        def bad(self, exc=exc):
            self.ctx.command("platform.power.on", {})
            raise exc
        scenario.Runner.run = bad
        rd = env.run_dir()
        try:
            res = runmode.run_one(cfg([]), sc, "default", rd)
        finally:
            scenario.Runner.run = real
        check(scenario.exit_code(res) == 3 and str(exc) in res["why"],
              "B1: %r" % res)
        ctl = core_mod.Controller(cfg([]), run_dir=rd)   # the lock is free
        ctl.shutdown()

    def intr(self):
        self.ctx.command("platform.power.on", {})
        raise KeyboardInterrupt
    scenario.Runner.run = intr
    rd = env.run_dir()
    try:
        raises(KeyboardInterrupt, runmode.run_one, cfg([]), sc, "default",
               rd)
    finally:
        scenario.Runner.run = real
    ctl = core_mod.Controller(cfg([]), run_dir=rd)
    ctl.shutdown()
    # S4
    with open(os.path.join(d, "typo.json"), "w") as f:
        json.dump(dict(SCN_PASS, name="typo", setup=[
            {"do": "platform.power.onn"}]), f)
    os.makedirs(os.path.join(root, "runs"), exist_ok=True)
    n = len(os.listdir(os.path.join(root, "runs")))
    out = []
    check(runmode.run_all(cfg(["fake-pass.json", "typo.json"]),
                          out=out.append) == 2 and
          len(os.listdir(os.path.join(root, "runs"))) == n and
          any("platform.power.onn" in x for x in out),
          "S4: an unknown verb: %r" % out)
    res = runmode.run_one(cfg([]), sc, "nope", env.run_dir())
    check(scenario.exit_code(res) == 2 and not res.get("infra"),
          "S4: a ScenarioError in run_one %r" % res)
    ctl = env.controller()
    try:
        check(set(ctl.dispatcher.verbs) == verbs_mod.VERBS,
              "the registry: %r" % (set(ctl.dispatcher.verbs) ^
                                    verbs_mod.VERBS))
    finally:
        ctl.shutdown()
    # N6: SIGTERM mid-run
    t = threading.Timer(2.0, os.kill, (os.getpid(), signal.SIGTERM))
    t.start()
    t0 = time.monotonic()
    out = []
    rc = runmode.run_all(cfg(["fake-long.json", "fake-pass.json"]),
                         out=out.append)
    t.join()
    check(rc == 3 and time.monotonic() - t0 < 20 and
          not any(x.startswith("PASS fake-pass") for x in out) and
          signal.getsignal(signal.SIGTERM) == signal.SIG_DFL,
          "N6: SIGTERM: rc %d %r" % (rc, out))


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

        def tm_lines():
            with open(os.path.join(ctl.run_dir, "tm.jsonl")) as f:
                return [json.loads(x) for x in f if x.endswith("\n")]
        # the count goes up before the line is written: wait for it
        wait_for(lambda: len(tm_lines()) >= 5, what="5 tm.jsonl lines")
        lines = tm_lines()
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
    # the memory-upset entries (M-6a) ride the qom action as strings:
    # a one-shot event (no undo: upset-clear is a different property),
    # and fault.clear refuses
    ctl = env.controller()
    try:
        ctl.command("platform.power.on", {})
        ctl.command("platform.power.boot", {})
        ctl.command("fault.inject", {"id": "memory.cache.seu"})
        sets = ctl.power.qmp.execute("x-fake-set-log")
        check(sets[-1] == ["/machine/stixctrl/edac", "upset-inject",
                           "cache,0xE0180040,0x1"],
              "memory.cache.seu applies %r" % sets[-1])
        e = raises(VerbError, ctl.command, "fault.clear",
                   {"id": "memory.cache.seu"})
        check(e.status == 409, "a transient memory fault clears to 409")
    finally:
        ctl.shutdown()


def t_fault_reapply(env):
    """PL-11a (D-PSa): a power cycle re-applies the active device-state
    faults (qom) and keeps the prior from the first apply; one-shots and
    the controller-side kinds are not re-applied; a failed re-apply is
    logged and never raises (S3)."""
    clk, f, calls, fail, recs = _fault_rig()
    f.inject("attenuator.current.offset", params={"offset": 800})
    f.inject("memory.cache.seu")
    f.inject("spw.link.disconnect")
    n = len(calls)
    f.reapply("cold power-on")
    check(calls[n:] == [("apply", "attenuator.current.offset")],
          "only the qom fault re-applied: %r" % calls[n:])
    check(f.active["attenuator.current.offset"]["prior"] ==
          "prior-attenuator.current.offset", "the stored prior is kept")
    r = [x for x in recs() if x["action"] == "reapply"]
    check(len(r) == 1 and r[0]["why"] == "cold power-on" and
          r[0]["params"] == {"offset": 800}, "reapply record %r" % r)
    f.clear("attenuator.current.offset")
    check(calls[-1] == ("undo", "attenuator.current.offset",
                        "prior-attenuator.current.offset"),
          "clear after reapply undoes with the first prior: %r"
          % (calls[-1],))
    # a failed re-apply: logged, the fault stays active, no raise
    f.inject("attenuator.current.offset", params={"offset": 800})
    fail["v"] = True
    f.reapply("warm guest reset")
    last = recs()[-1]
    check(last["action"] == "reapply-error" and
          "STIX is off" in last["error"], "reapply-error record %r" % last)
    check("attenuator.current.offset" in f.active, "still active")
    fail["v"] = False
    # against the fake: the re-apply qom-set lands after spawn and
    # before cont, the one-shot memory upset does not come back, and
    # the clear after both cycles writes the pre-fault value
    ATT_SET = ["/machine/stixctrl/attenuator", "attenuator.current.offset"]
    ctl = env.controller()
    try:
        cmd = ctl.command
        cmd("platform.power.on", {})
        cmd("platform.power.boot", {})
        cmd("fault.inject", {"id": "attenuator.current.offset",
                             "params": {"offset": 800}})
        cmd("fault.inject", {"id": "memory.cache.seu"})
        sets = ctl.power.qmp.execute("x-fake-set-log")
        check(sets[-1] == ["/machine/stixctrl/edac", "upset-inject",
                           "cache,0xE0180040,0x1"] and
              ATT_SET + [800] in sets, "first inject: %r" % sets)
        cmd("platform.power.off", {})
        cmd("platform.power.on", {})
        sets = ctl.power.qmp.execute("x-fake-set-log")
        check(sets == [ATT_SET + [800]],
              "re-applied after spawn, before cont, the transient "
              "memory row is not: %r" % sets)

        def reapplies():
            return [(x.obj["path"], x.obj["data"])
                    for x in ctl.log.since(0) if x.kind == "fault"
                    and x.obj["data"].get("action") == "reapply"]
        rr = reapplies()
        check([(p, d["why"], d["params"]) for p, d in rr] ==
              [("fault.attenuator.current.offset", "cold power-on",
                {"offset": 800})], "the cold reapply record: %r" % (rr,))
        e = raises(VerbError, cmd, "fault.inject",
                   {"id": "attenuator.current.offset",
                    "params": {"offset": 1}})
        check(e.status == 409, "the second inject still 409s")
        cmd("platform.power.boot", {})
        sets = ctl.power.qmp.execute("x-fake-set-log")
        check(sets == [ATT_SET + [800], ["cont"]],
              "the re-apply qom-set landed before cont: %r" % sets)
        ctl.power.qmp.execute("x-fake-guest-reset")
        wait_for(lambda: len(reapplies()) == 2, what="the warm reapply")
        check(reapplies()[-1][1]["why"] == "warm guest reset",
              "the warm reapply record: %r" % (reapplies()[-1],))
        cmd("fault.clear", {"id": "attenuator.current.offset"})
        sets = ctl.power.qmp.execute("x-fake-set-log")
        check(sets[-1] == ATT_SET + [0] and
              all(x[1:2] != ["upset-inject"] for x in sets),
              "the clear writes the pre-fault value, not the faulted "
              "one: %r" % sets[-1])
    finally:
        ctl.shutdown()


def t_att_edac_leaves(env):
    """PL-11b (D-PSb): the stixctrl hw-state's attenuator and edac
    sections map to instrument.attenuator.* / instrument.memory.*
    leaves; a poison arm and its clear are change records, two stable
    reads are not."""
    ctl = env.controller()
    try:
        cmd = ctl.command
        cmd("platform.power.on", {})
        cmd("platform.power.boot", {})
        wait_for(lambda: ctl.state.get("instrument.attenuator") is not None,
                 what="the attenuator leaves")
        # the leaf paths and values are the fake's idle attenuator
        # (stix_att.c:966-998 at reset: parked at "out", AB actuates at
        # OUT so the presented cmdsta carries the AB bit, no fault)
        t = ctl.state.get("instrument.attenuator.truth")
        check(t["position"]["truth"] == "out" and
              t["position_ppm"]["truth"] == 0 and
              t["sensor_ab"]["truth"] is True and
              t["sensor_bc"]["truth"] is False and
              t["motor1_driving"]["truth"] is False and
              t["motor2_driving"]["truth"] is False and
              t["att_c"]["truth"] == 20 and t["moves"]["truth"] == 0 and
              t["last_end"]["truth"] == "none" and
              t["last_end_ns"]["truth"] == 0,
              "the attenuator truth leaves: %r" % t)
        p = ctl.state.get("instrument.attenuator.presented")
        check(p["sensor_ab"]["truth"] is True and
              p["sensor_bc"]["truth"] is False and
              p["cmdsta"]["truth"] == 1024 and
              p["att_c"]["truth"] == 20,
              "the attenuator presented leaves: %r" % p)
        check(ctl.state.get("instrument.attenuator.fsw_accesses")
              ["truth"] == 0 and
              ctl.state.get("instrument.attenuator.faults_active")
              ["truth"] == "",
              "fsw_accesses and faults_active")
        for mem in ("cache", "sdram", "rotbuf"):
            for k in ("injected", "corrected", "detected"):
                lf = ctl.state.get("instrument.memory.%s.%s" % (mem, k))
                check(lf is not None and lf["truth"] == 0,
                      "instrument.memory.%s.%s at zero: %r" % (mem, k, lf))
        check(ctl.state.get("instrument.memory.poisons")["truth"] == [],
              "no poisons armed")
        check(ctl.state.get("instrument.memory.bram") is None,
              "the bram maps no leaf (no EDAC counters)")

        def changes():
            return [x for x in ctl.log.since(0) if x.kind == "state" and
                    x.obj["path"].startswith(("instrument.attenuator",
                                              "instrument.memory"))]
        seen = len(changes())
        time.sleep(0.3)                # several state polls, all stable
        check(len(changes()) == seen,
              "no change record for the stable reads: %r"
              % changes()[seen:])
        # an active fault tags its target's leaves (state.py fault_ids)
        cmd("fault.inject", {"id": "attenuator.sensor_ab.stuck_open"})
        check(ctl.state.get(
            "instrument.attenuator.truth.sensor_ab")["fault_ids"] ==
            ["attenuator.sensor_ab.stuck_open"], "the fault tag")
        # arm a poison through the catalogue row: the counter and the
        # poisons leaf move, each with one change record
        cmd("fault.inject", {"id": "memory.cache.seu"})
        wait_for(lambda: ctl.state.get(
                     "instrument.memory.cache.injected")["truth"] == 1,
                 what="the injected counter")
        wait_for(lambda: len(ctl.state.get(
                     "instrument.memory.poisons")["truth"]) == 1,
                 what="the poison record")
        rec = ctl.state.get("instrument.memory.poisons")["truth"][0]
        check(rec == {"id": 0, "memory": "cache", "address": 0xE0180040,
                      "mask": 1, "mode": "seu", "persist": False},
              "the poison record mirrors the device: %r" % rec)
        # every memory row is transient/permanent: a one-shot event,
        # never active (faults.py:25), so it tags no leaf — the counter
        # and the poisons leaf are the record
        check(not ctl.state.get(
            "instrument.memory.cache.injected").get("fault_ids"),
            "a one-shot memory fault tags no leaf")
        ch = [x.obj["data"] for x in changes()
              if x.obj["path"] == "instrument.memory.poisons"]
        check(len(ch) == 2 and ch[0]["old"] is None and          # creation
              ch[1]["old"]["truth"] == [] and
              len(ch[1]["new"]["truth"]) == 1,
              "creation, then one change record on the arm: %r" % ch)
        # the clear: the device's own upset-clear (no platform verb
        # until PL-11f), driven straight on the fake's QMP
        ctl.power.qmp.execute("qom-set", {
            "path": "/machine/stixctrl/edac",
            "property": "upset-clear", "value": "all"})
        wait_for(lambda: ctl.state.get(
                     "instrument.memory.poisons")["truth"] == [],
                 what="the poison cleared")
        ch = [x.obj["data"] for x in changes()
              if x.obj["path"] == "instrument.memory.poisons"]
        check(len(ch) == 3 and ch[2]["old"]["truth"] and
              ch[2]["new"]["truth"] == [],
              "one change record on the clear: %r" % ch)
        check(ctl.state.get("instrument.memory.cache.injected")
              ["truth"] == 1,
              "the counter stays (only the poison is retired)")
    finally:
        ctl.shutdown()


def t_det_rotb_leaves(env):
    """PL-11c (D-PSb): the stixctrl hw-state's det (scene, FSM, replay)
    and rotb sections map to instrument.detector.* and instrument.rotb.*
    leaves; the volatile ones (replay.pos_ns, rotb count/rd/wr) flood no
    change record, two stable reads add none."""
    ctl = env.controller()
    try:
        cmd = ctl.command
        cmd("platform.power.on", {})
        cmd("platform.power.boot", {})
        wait_for(lambda: ctl.state.get("instrument.detector") is not None,
                 what="the detector leaves")
        # the leaf paths and values are the fake's idle det section (the
        # stix_det scene defaults, FSM off, no replay table)
        d = ctl.state.get("instrument.detector")
        check(d["source"]["truth"] == "synthetic" and
              d["flare"]["truth"] is False and
              d["quiet_cps"]["truth"] == 20 and
              d["flare_cps"]["truth"] == 2000 and
              d["ql_bank"]["truth"] == 0 and
              d["fsm"]["enable"]["truth"] is False,
              "the detector scene/FSM leaves: %r" % d)
        rep = d["replay"]
        check(rep["state"]["truth"] == "idle" and
              rep["pos_ns"]["truth"] == 0 and
              rep["events"]["truth"] == 0 and
              rep["dropped"]["truth"] == 0 and
              rep["unsupported"]["truth"] == 0 and
              rep["table"]["truth"] == "" and
              rep["count"]["truth"] == 0 and
              rep["duration_ns"]["truth"] == 0,
              "the replay leaves at boot: %r" % rep)
        for q in range(4):
            lf = ctl.state.get("instrument.detector.quarter.%d" % q)
            check(lf is not None and lf["truth"] is False and
                  lf["index_space"] == "quarter",
                  "quarter.%d keeps its path and space: %r" % (q, lf))
        # the ROTB ring: empty, disabled, the default capacity
        for k, v in (("count", 0), ("full", False), ("enable", False),
                     ("rd", 0), ("wr", 0), ("capacity", 104448)):
            lf = ctl.state.get("instrument.rotb." + k)
            check(lf is not None and lf["truth"] == v,
                  "instrument.rotb.%s == %r: %r" % (k, v, lf))

        def changes():
            return [x for x in ctl.log.since(0) if x.kind == "state" and
                    x.obj["path"].startswith(("instrument.detector",
                                              "instrument.rotb"))]
        seen = len(changes())
        time.sleep(0.3)                # several state polls, all stable
        check(len(changes()) == seen,
              "no change record for the stable reads (and the volatile "
              "ones flood none): %r" % changes()[seen:])
    finally:
        ctl.shutdown()


def t_det_burst(env):
    """FF plan Task 3: the det.rotb.burst verb over the fake's
    det-rotb-burst QOM pair: power off refuses, the device's refusal
    text becomes the 409, the status string is parsed."""
    from . import verbs as verbs_mod
    check("det.rotb.burst" in verbs_mod.VERBS, "in the registry")
    ctl = env.controller()
    try:
        check("det.rotb.burst" in ctl.dispatcher.verbs, "registered")
        e = raises(VerbError, ctl.command, "det.rotb.burst", {"frames": 5})
        check(e.status == 409 and "off" in str(e), "power must be on: %s"
              % e)
        ctl.command("platform.power.on", {})
        ctl.command("platform.power.boot", {})
        e = raises(VerbError, ctl.command, "det.rotb.burst", {"frames": 0})
        check(e.status == 409 and "det-rotb-burst" in str(e),
              "the device's refusal: %s" % e)
        r = ctl.command("det.rotb.burst", {"frames": 10})
        check(r == {"requested": 10, "appended": 10, "stop": "done",
                    "rotb_count": 490}, "the parsed status: %r" % r)
    finally:
        ctl.shutdown()


def t_flashlib(env):
    """FF plan Task 4: the flash library — save/list/load on temp dirs
    with a small sparse file standing in for flash.bin, the name rules,
    the too-dense copy refusal, and the verbs' power-state rules."""
    from . import flashlib
    root = os.path.join(env.tmp, "libroot-%d" % env.n)
    cfg = env.cfg(retention={"run_root": root})
    lib = flashlib.library_dir(cfg)
    check(lib == os.path.join(root, "flash-library"),
          "the library dir: %s" % lib)
    rd = env.run_dir()
    flash = os.path.join(rd, "flash.bin")
    with open(flash, "wb") as f:
        f.write(b"\xa5" * 8192)
        f.truncate(4 << 20)
    rec = flashlib.save(cfg, rd, "run-a", {"elf": "/e/asw.elf"})
    check(rec["name"] == "run-a" and rec["allocated_kib"] <= 64 and
          rec["source_run_dir"] == rd and rec["elf"] == "/e/asw.elf" and
          rec["created_utc"].endswith("Z"), "the save record: %r" % rec)
    lst = flashlib.listing(cfg)
    check(len(lst) == 1 and lst[0]["name"] == "run-a" and
          lst[0]["allocated_kib"] == rec["allocated_kib"],
          "the listing shows it: %r" % lst)
    e = raises(FileExistsError, flashlib.save, cfg, rd, "run-a", {})
    check("exists" in str(e), "a second save of a name: %s" % e)
    # load replaces run_dir/flash.bin with identical bytes
    os.unlink(flash)
    check(flashlib.load(cfg, rd, "run-a") == rec, "load's record")
    with open(flash, "rb") as f:
        check(f.read(8192) == b"\xa5" * 8192 and
              f.seek(0, 2) == 4 << 20, "the loaded flash.bin bytes")
    # the name rules (review focus 5)
    for bad in ("../x", "/abs", "", ".hidden", "a" * 65, "a/b", "a b",
                "a,b"):
        raises(ValueError, flashlib.check_name, bad)
    for good in ("a", "X-9._y", "a" * 64):
        flashlib.check_name(good)
    # a too-dense copy is refused and deleted (A6)
    cfg1 = env.cfg(retention={"run_root": root},
                   power={"sparse_limit_mb": 1})
    rd1 = env.run_dir()
    with open(os.path.join(rd1, "flash.bin"), "wb") as f:
        f.write(os.urandom(2 << 20))
        f.truncate(4 << 20)
    e = raises(flashlib.FlashLibError, flashlib.save, cfg1, rd1, "dense",
               {})
    check("not sparse" in str(e), "the dense copy: %s" % e)
    check(not os.path.exists(os.path.join(lib, "dense.bin")),
          "the dense copy is deleted")
    # the verbs' power-state rules
    ctl = env.controller(retention={"run_root": root})
    try:
        cmd = ctl.command
        r = cmd("platform.flash.list", {})
        check(r["dir"] == lib and
              [x["name"] for x in r["library"]] == ["run-a"],
              "flash.list: %r" % r)
        e = raises(VerbError, cmd, "platform.flash.save", {"name": "nope"})
        check(e.status == 409, "no flash.bin yet: %s" % e)
        cmd("platform.power.on", {})
        cmd("platform.power.boot", {})
        e = raises(VerbError, cmd, "platform.flash.save", {"name": "x1"})
        check(e.status == 409 and "paused" in str(e),
              "save while running: %s" % e)
        e = raises(VerbError, cmd, "platform.flash.load",
                   {"name": "run-a"})
        check(e.status == 409, "load while on: %s" % e)
        e = raises(VerbError, cmd, "platform.flash.save",
                   {"name": "../evil"})
        check(e.status == 400, "a bad name: %s" % e)
        cmd("sim.pause", {})
        r = cmd("platform.flash.save", {"name": "paused-save"})
        check(r["name"] == "paused-save" and r["allocated_kib"] <= 64 and
              r["elf"] == "/dev/null" and len(r["elf_sha256"]) == 64 and
              r["qemu"] == env.bins["new"] and "sim_time" in r and
              r["boot_id"] == 1, "a save while paused: %r" % r)
        cmd("platform.power.off", {})
        r = cmd("platform.flash.load", {"name": "paused-save"})
        check(r["name"] == "paused-save", "the load after off: %r" % r)
        e = raises(VerbError, cmd, "platform.flash.load",
                   {"name": "no-such"})
        check(e.status == 404, "no such flash: %s" % e)
    finally:
        ctl.shutdown()


def t_faults(env):
    """D-PLq: the catalogue validates; inject/clear are fault records with
    seeds; forcing is re-applied after a warm reset."""
    cat = faults_mod.load()
    check(len(cat["faults"]) >= 16, "v1 catalogue + the memory upsets")
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


def t_gate_slots(env):
    """§41: the platform gates' ports come from the slot band (their
    --dry-run, a temp lock dir and a fake df; nothing boots)."""
    import subprocess
    t = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                     "tests", "test-plat-slot.sh")
    env_ = {k: v for k, v in os.environ.items() if k != "STIX_PORT_BASE"}
    r = subprocess.run(["bash", t], capture_output=True, text=True,
                       timeout=60, env=env_)
    check(r.returncode == 0 and "PLAT-SLOT: PASS" in r.stdout,
          "test-plat-slot.sh: %s%s" % (r.stdout, r.stderr))


UI_VERBS = {"platform.power.on", "platform.power.boot", "platform.power.off",
            "platform.power.cycle", "sim.pause", "sim.resume",
            "platform.spw.link", "platform.timecode.enable",
            "platform.timecode.once", "platform.timecode.inject",
            "platform.timesync.send", "platform.keepalive.set",
            "platform.obt.set", "fault.inject", "fault.clear", "force.set",
            "force.clear", "scenario.run", "scenario.abort"}
# the verbs the tabbed page (timeline plan B1/B2) adds to UI_VERBS
UI_VERBS_ADDED = {"tc.gs.send", "det.rotb.burst", "platform.flash.save",
                  "platform.flash.load", "platform.flash.list"}
UI_TABS = ("overview", "platform", "idpu", "detectors", "faults",
           "scenarios", "logs", "state", "setup", "timeline")
UI_ENDPOINTS = {"status", "time", "faults", "verbs", "state", "log",
                "stream", "scenarios", "cmd", "series", "views"}
# timeline plan C1: the ids the plotter panel must have
UI_TIMELINE_IDS = {"tl-view", "tl-window", "tl-search", "tl-hits",
                   "tl-add", "tl-save", "tl-save-name", "tl-delete",
                   "tl-note", "tl-panels", "tl-hover", "tl-plot",
                   "tl-canvas", "tl-reload", "tl-add-panel"}


def _csp_hashes(page, tag):
    import base64
    import hashlib
    return {"'sha256-%s'" % base64.b64encode(hashlib.sha256(
        m.encode()).digest()).decode()
        for m in re.findall(r"<%s>(.*?)</%s>" % (tag, tag), page, re.S)}


def t_ui_server(env):
    """PL-6: the page at / (and /index.html) behind the same Host filter,
    with a CSP that allows exactly its own inline script and style
    (hashes) and the same origin; GET /api/v1/scenarios lists the
    scenario dir (a broken file is listed with its error)."""
    d = _scn_dir(env)
    ctl = env.controller(run={"scenario_dir": d})
    srv = api_mod.make_server(ctl, "127.0.0.1", 0)
    port = srv.server_address[1]
    try:
        with open(api_mod.UI_PAGE, encoding="utf-8") as f:
            page = f.read()
        for path in ("/", "/index.html"):
            st, hd, body = _http(port, "GET", path)
            check(st == 200 and body.decode() == page, "GET %s: %d" %
                  (path, st))
            check(hd["Content-Type"] == "text/html; charset=utf-8" and
                  hd["X-Content-Type-Options"] == "nosniff" and
                  hd["X-Frame-Options"] == "DENY" and
                  hd["Cache-Control"] == "no-store", "headers %r" % hd)
            csp = hd["Content-Security-Policy"]
            pol = {x.split()[0]: set(x.split()[1:])
                   for x in csp.split(";") if x.strip()}
            check(pol["default-src"] == {"'none'"} and
                  pol["connect-src"] == {"'self'"} and
                  pol["frame-ancestors"] == {"'none'"} and
                  pol["script-src"] == _csp_hashes(page, "script") and
                  pol["style-src"] == _csp_hashes(page, "style") and
                  pol["script-src"] and pol["style-src"], "CSP %r" % csp)
        st, _, _ = _http(port, "GET", "/", headers={"Host": "evil.example"})
        check(st == 403, "the page behind the Host filter: %d" % st)
        st, _, _ = _http(port, "GET", "/favicon.ico")
        check(st == 204, "no favicon, no error: %d" % st)
        st, _, _ = _http(port, "GET", "/ui/../core.py")
        check(st == 404, "no other file is served: %d" % st)
        st, _, body = _http(port, "GET", "/api/v1/scenarios")
        lst = json.loads(body)["scenarios"]
        by = {x["file"]: x for x in lst}
        check(st == 200 and by["fake-fail.json"]["variants"] ==
              ["nominal", "fault_x"] and by["fake-pass.json"]["name"] ==
              "fake-pass" and "PL-8" in by["broken.json"]["error"] and
              sorted(by) == sorted(f for f in os.listdir(d)
                                   if f.endswith(".json")),
              "scenarios %r" % lst)
    finally:
        srv.shutdown()
        srv.server_close()
        ctl.shutdown()


class _UiParser(__import__("html.parser").parser.HTMLParser):
    def __init__(self):
        super().__init__()
        self.verbs = []
        self.attrs = []
        self.scripts = []
        self.tabs = []          # data-tab inside #tabbar
        self.panels = []
        self._bar = False
        self._in = None
        self.rows = []          # (verb, input names) of each ctl button
        self._ctl = []          # [depth, names, buttons] of open .ctl
        self._depth = 0

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        self.attrs.append((tag, a))
        if tag == "nav" and a.get("id") == "tabbar":
            self._bar = True
        if self._bar and "data-tab" in a:
            self.tabs.append(a["data-tab"])
        if "data-tab-panel" in a:
            self.panels.append(a["data-tab-panel"])
        if tag == "div":
            self._depth += 1
            if "ctl" in (a.get("class") or "").split():
                self._ctl.append([self._depth, [], []])
        if self._ctl and a.get("name") and tag in ("input", "select"):
            self._ctl[-1][1].append(a["name"])
        if self._ctl and "data-verb" in a and "data-args" not in a:
            self._ctl[-1][2].append((a["data-verb"], a.get("data-only")))
        if "data-verb" in a:
            self.verbs.append((a["data-verb"], a.get("data-args")))
        if tag in ("script", "style"):
            self._in = tag

    def handle_endtag(self, tag):
        self._in = None
        if tag == "nav":
            self._bar = False
        if tag == "div":
            if self._ctl and self._ctl[-1][0] == self._depth:
                _, names, buttons = self._ctl.pop()
                for verb, only in buttons:
                    keep = None if only is None else \
                        {x for x in only.split(",") if x}
                    self.rows.append((verb, [n for n in names if keep is
                                             None or n in keep]))
            self._depth -= 1

    def handle_data(self, data):
        if self._in == "script":
            self.scripts.append(data)


def t_ui_page(env):
    """PL-6 gate (no browser): every data-verb in index.html exists in
    GET /api/v1/verbs, with data-args a JSON object of known args; the
    panels cover the plan's verbs; no browser storage, no external URL,
    no inline handlers or style attributes (the CSP would block them),
    no innerHTML (TM and UART text is untrusted); every API path the
    script names is a served endpoint; every literal $(...) in the
    script is a defined id."""
    ctl = env.controller()
    srv = api_mod.make_server(ctl, "127.0.0.1", 0)
    port = srv.server_address[1]
    try:
        st, _, body = _http(port, "GET", "/api/v1/verbs")
        verbs = {v["verb"]: set(v["args"]) for v in
                 json.loads(body)["verbs"]}
        st, _, page = _http(port, "GET", "/")
        page = page.decode()
    finally:
        srv.shutdown()
        srv.server_close()
        ctl.shutdown()
    p = _UiParser()
    p.feed(page)
    used = {v for v, _ in p.verbs}
    check(used and used <= set(verbs), "unknown data-verb %r"
          % (used - set(verbs)))
    check(UI_VERBS <= used, "panels miss %r" % (UI_VERBS - used))
    for v, a in p.verbs:
        if a is None:
            continue
        try:
            args = json.loads(a)
        except ValueError:
            raise Fail("data-args of %s is not JSON: %r" % (v, a))
        check(isinstance(args, dict) and set(args) <= verbs[v],
              "data-args of %s: %r" % (v, a))
    low = page.lower()
    for bad in ("localstorage", "sessionstorage", "indexeddb",
                "document.cookie", "innerhtml", "outerhtml",
                "insertadjacenthtml", "document.write", "eval(",
                "new function"):
        check(bad not in low, "the page uses %s" % bad)
    check(not re.search(r"(https?:)?//[a-z0-9.-]+\.[a-z]{2,}", low) and
          "http:" not in low and "https:" not in low,
          "an external URL in the page")
    for tag, a in p.attrs:
        on = [k for k in a if k.startswith("on")]
        check(not on and "style" not in a, "<%s %r>: inline handler or "
              "style attribute" % (tag, a))
        for k in ("src", "href", "action"):
            check(not a.get(k) or a[k].startswith("#"),
                  "<%s %s=%r>: a resource" % (tag, k, a.get(k)))
    # a button without data-args posts the named inputs of its .ctl row
    # (less a data-only list): those names must be args of its verb
    for verb, names in p.rows:
        top = {n.split(".")[0] for n in names}
        check(top <= verbs[verb], "%s posts %r, not all args of it"
              % (verb, sorted(top)))
    js = "\n".join(p.scripts)
    # every request is getJSON("<endpoint>...") or API + "<endpoint>...",
    # with API the v1 prefix; nothing else may name a URL
    check(re.search(r'const API = "/api/v1/";', js) and
          js.count("/api/v1") == 1, "one API prefix")
    node = shutil.which("node") or shutil.which("nodejs")
    if node:            # the script at least parses (no browser needed)
        import subprocess
        with tempfile.NamedTemporaryFile("w", suffix=".js",
                                         delete=False) as f:
            f.write(js)
        try:
            r = subprocess.run([node, "--check", f.name],
                               capture_output=True, text=True, timeout=30)
        finally:
            os.unlink(f.name)
        check(r.returncode == 0, "the page's script: %s" % r.stderr)
    paths = set(re.findall(r"""(?:getJSON\(|API \+ )["']([a-z]+)""", js))
    check(len(re.findall(r"\bfetch\(|EventSource\(", js)) ==
          len(re.findall(r"\bfetch\(API \+|EventSource\(API \+", js)),
          "a fetch or EventSource not on API")
    check(paths and paths <= UI_ENDPOINTS and
          {"status", "stream", "cmd", "verbs", "faults", "scenarios",
           "state"} <= paths, "API paths %r" % paths)
    # every literal $() names an id defined in the page
    refs = set(re.findall(r"""\$\(["']([^"']+)["']\)""", js))
    ids = {a["id"] for _, a in p.attrs if "id" in a}
    check(refs and refs <= ids, "$() ids %r, page ids %r"
          % (sorted(refs), sorted(ids)))


def t_ui_tabs(env):
    """Timeline plan B1: the page is a tab bar over data-tab-panel
    sections (state in the location hash, default #overview): the tab
    bar's tabs and the panels match one to one, the pre-tab data-verb
    set is still all there, and the new verbs are used."""
    ctl = env.controller()
    srv = api_mod.make_server(ctl, "127.0.0.1", 0)
    port = srv.server_address[1]
    try:
        st, _, body = _http(port, "GET", "/")
        check(st == 200, "GET /: %d" % st)
    finally:
        srv.shutdown()
        srv.server_close()
        ctl.shutdown()
    p = _UiParser()
    p.feed(body.decode())
    check(sorted(p.tabs) == sorted(UI_TABS), "tabs %r" % sorted(p.tabs))
    check(sorted(p.panels) == sorted(UI_TABS), "panels %r" % sorted(p.panels))
    used = {v for v, _ in p.verbs}
    check(UI_VERBS <= used, "the pre-tab data-verbs lost %r"
          % (UI_VERBS - used))
    check(UI_VERBS_ADDED <= used, "the new data-verbs missing %r"
          % (UI_VERBS_ADDED - used))


def t_ui_timeline(env):
    """Timeline plan C1: the timeline panel is the plotter, not the
    placeholder: the canvas container and the control ids are there,
    the script reads /series and /views, and the page rules still hold
    (no style attribute, no inline handler, no innerHTML)."""
    ctl = env.controller()
    srv = api_mod.make_server(ctl, "127.0.0.1", 0)
    port = srv.server_address[1]
    try:
        st, _, body = _http(port, "GET", "/")
        check(st == 200, "GET /: %d" % st)
    finally:
        srv.shutdown()
        srv.server_close()
        ctl.shutdown()
    page = body.decode()
    check("Track C" not in page, "the placeholder is still there")
    p = _UiParser()
    p.feed(page)
    ids = {a["id"] for _, a in p.attrs if "id" in a}
    check(UI_TIMELINE_IDS <= ids, "timeline ids missing %r"
          % sorted(UI_TIMELINE_IDS - ids))
    plot = re.search(r'id="tl-plot"[^>]*>\s*<canvas id="tl-canvas"',
                     page)
    check(plot is not None, "the canvas is not in the plot container")
    js = "\n".join(p.scripts)
    paths = set(re.findall(r"""(?:getJSON\(|API \+ )["']([a-z]+)""", js))
    check({"series", "views"} <= paths, "the script reads %r" % paths)
    for tag, a in p.attrs:
        check("style" not in a and not [k for k in a if k.startswith("on")],
              "<%s %r>: style attribute or inline handler" % (tag, a))
    for bad in ("innerhtml", "outerhtml", "insertadjacenthtml"):
        check(bad not in page.lower(), "the page uses %s" % bad)


SETUP_KEYS = {"binary", "elf", "elf_sha256", "icount_shift", "pace",
              "memory", "timecode_source", "obt_continuity",
              "on_guest_reset", "sample_s"}


def t_setup_status(env):
    """Timeline plan B2: status()["setup"] lists the launch
    configuration (additive, read-only); elf_sha256 is the ELF's
    digest, or None when unreadable."""
    ctl = env.controller(qemu={"icount_shift": 7, "pace": "fast"})
    try:
        s = ctl.status()["setup"]
        check(set(s) == SETUP_KEYS, "setup keys %r" % sorted(s))
        check(s["icount_shift"] == 7 and s["pace"] == "fast",
              "setup mirrors the qemu config: %r" % s)
        check(s["elf_sha256"] == hashlib.sha256(b"").hexdigest(),
              "elf_sha256 of the /dev/null ELF")
        q = ctl.cfg["qemu"]
        check(s["binary"] == q["binary"] and s["elf"] == q["elf"] and
              s["memory"] == q["memory"] and
              s["timecode_source"] == q["timecode_source"] and
              s["obt_continuity"] == q["obt_continuity"] and
              s["on_guest_reset"] == q["on_guest_reset"],
              "setup == the qemu config: %r" % s)
        # None until Track A's [timeline] config lands (then -1.0)
        check(s["sample_s"] is None or s["sample_s"] == -1.0,
              "sample_s %r" % s["sample_s"])
    finally:
        ctl.shutdown()
    ctl = env.controller(qemu={"elf": "/nonexistent/no-such.elf"})
    try:
        check(ctl.status()["setup"]["elf_sha256"] is None,
              "elf_sha256 None when unreadable")
    finally:
        ctl.shutdown()


def t_ui_check(env):
    """PL-6: uicheck (used by core-smoke and link-smoke against the live
    server): ok on the served page; a page naming an unknown verb, a CSP
    that does not cover the page's script, and a server without the page
    each fail with the reason."""
    from . import uicheck
    ctl = env.controller()
    srv = api_mod.make_server(ctl, "127.0.0.1", 0)
    base = "http://127.0.0.1:%d" % srv.server_address[1]
    real_page, real_csp = api_mod.UI_PAGE, api_mod.page_csp
    try:
        ok, why = uicheck.check(base)
        check(ok and "data-verb" in why, "the served page: %s" % why)
        with open(real_page, encoding="utf-8") as f:
            page = f.read()
        bad = os.path.join(env.tmp, "bad-%d.html" % env.n)
        with open(bad, "w", encoding="utf-8") as f:
            f.write(page.replace('data-verb="sim.pause"',
                                 'data-verb="sim.nosuch"'))
        api_mod.UI_PAGE = bad
        ok, why = uicheck.check(base)
        check(not ok and "sim.nosuch" in why, "unknown verb: %s" % why)
        api_mod.UI_PAGE = real_page
        api_mod.page_csp = lambda p: "default-src 'none'"
        ok, why = uicheck.check(base)
        check(not ok and "CSP" in why, "CSP: %s" % why)
        api_mod.page_csp = real_csp
        api_mod.UI_PAGE = os.path.join(env.tmp, "missing.html")
        ok, why = uicheck.check(base)
        check(not ok and "500" in why, "no page: %s" % why)
    finally:
        api_mod.UI_PAGE, api_mod.page_csp = real_page, real_csp
        srv.shutdown()
        srv.server_close()
        ctl.shutdown()


def t_probe_lib(env):
    p = probe_lib.load()
    units = list(p.frame_units(bytes.fromhex("fe0201") + b"\x00" * 6))
    check(isinstance(units, list), "probe frame_units")


# -- timeline (A1 sampler, A2 IDB HK decode, A3 TC(237,18) label) ---------
def _sampler_rig(paths, leaves):
    t = [0.0]
    clk = SimClock(mono_fn=lambda: t[0])
    clk.process_started()
    clk.resumed()
    clk.sample(0)
    st = state_mod.StateTree()
    with st.lock:
        st.leaves.update(leaves)
    lg = log_mod.RecordLog(None, clk.now)
    s = timeline_mod.Sampler(clk, st, lg, paths, 1.0)
    return clk, st, lg, s, t


def _sampler_tick(clk, t):
    t[0] += 1.0
    clk.sample(int(t[0] * NS))
    clk.run_due()


def _sample_recs(lg):
    return [r for r in lg.since(0) if r.kind == "sample"]


def t_sampler_record_shape(env):
    """A1: one `sample` record per bench-time tick, keys exactly the
    matched paths (+ "<path>@presented" only where presented differs),
    bools and strings as-is, None and containers skipped, nothing while
    off or unmatched."""
    clk, st, lg, s, t = _sampler_rig(
        ["instrument.rotb.*", "platform.timesync.verdict"],
        {"instrument.rotb.count": state_mod.leaf(5),
         "instrument.rotb.full": state_mod.leaf(False),
         "instrument.rotb.spare": state_mod.leaf(None),
         "instrument.rotb.frames": state_mod.leaf([1, 2]),
         "platform.timesync.verdict": state_mod.leaf("ok"),
         "instrument.hk_adc.0": state_mod.leaf(100)})  # not matched
    s.start()
    _sampler_tick(clk, t)
    recs = _sample_recs(lg)
    check(len(recs) == 1, "one record per tick: %d" % len(recs))
    r = recs[0]
    log_mod.check_record(r.obj)
    check(r.topic == "sample" and r.obj["source"] == "ctl" and
          r.obj["path"] == "", "the record shape: %r" % r.obj)
    check(set(r.obj["data"]) == {"instrument.rotb.count",
                                 "instrument.rotb.full",
                                 "platform.timesync.verdict"},
          "exactly the matched leaves: %r" % sorted(r.obj["data"]))
    check("instrument.rotb.spare" not in r.obj["data"] and
          "instrument.rotb.frames" not in r.obj["data"],
          "a None and a list leaf are absent: %r" % sorted(r.obj["data"]))
    check(r.obj["data"]["instrument.rotb.count"] == 5 and
          r.obj["data"]["instrument.rotb.full"] is False and
          r.obj["data"]["platform.timesync.verdict"] == "ok",
          "values as-is: %r" % r.obj["data"])
    # presented != truth adds "<path>@presented"
    with st.lock:
        st.leaves["instrument.rotb.count"]["presented"] = 7
    _sampler_tick(clk, t)
    recs = _sample_recs(lg)
    check(set(recs[-1].obj["data"]) == {"instrument.rotb.count",
                                        "instrument.rotb.full",
                                        "platform.timesync.verdict",
                                        "instrument.rotb.count@presented"}
          and recs[-1].obj["data"]["instrument.rotb.count@presented"] == 7,
          "the presented delta: %r" % sorted(recs[-1].obj["data"]))
    # no record while off (deadlines do not fire) or with nothing matched
    n = len(recs)
    clk.process_ended()
    _sampler_tick(clk, t)
    check(len(_sample_recs(lg)) == n, "no record while off")
    clk.process_started()
    clk.resumed()
    with st.lock:
        st.leaves.clear()
    _sampler_tick(clk, t)
    check(len(_sample_recs(lg)) == n, "no record with an empty tree")


def t_sampler_off_in_run_mode(env):
    """A1: sample_s unset means 1 s in interactive and no sampler in run
    mode; an explicit value overrides it."""
    cfg = config_mod.load(overrides=dict(env.overrides(), mode="run"))
    check(cfg["timeline"]["sample_s"] == -1.0, "the sentinel default")
    ctl = core_mod.Controller(cfg, run_dir=env.run_dir())
    try:
        check(ctl.sampler is None, "run mode without sample_s: no sampler")
    finally:
        ctl.shutdown()
    cfg = config_mod.load(overrides=dict(env.overrides(), mode="run",
                                         timeline={"sample_s": 2.0}))
    ctl = core_mod.Controller(cfg, run_dir=env.run_dir())
    try:
        check(ctl.sampler is not None and ctl.sampler.period_ns == 2 * NS,
              "run mode with sample_s = 2.0")
    finally:
        ctl.shutdown()
    ctl = env.controller()
    try:
        check(ctl.sampler is not None and ctl.sampler.period_ns == NS,
              "interactive: 1 s")
    finally:
        ctl.shutdown()


_IDB_SYNTH = {
    "byte_order": "big",
    "calibrations": {
        "CIX00041TM": {"type": "polynomial", "coefficients": [0, 4]},
        "CIXP0999TM": {"type": "interpolation", "unit": "degC",
                       "points": [[10, 90], [1010, 40], [2010, -10]]},
        "CIXT0999TM": {"type": "text",
                       "text_ranges": [[2, 2, "SAFE"], [5, 5, "NOMINAL"]]}},
    "packets": [
        {"id": "stix_54101", "service_type": 3, "service_subtype": 25,
         "pi1_value": 1,
         "fields": [
             {"name": "SID (NIX00020)", "byte_offset": 0,
              "data_type": "uint8"},
             {"name": "Instrument mode (NIXD0023)", "byte_offset": 1,
              "data_type": "uint8", "bit_offset": 4, "bit_length": 4},
             {"name": "CPU load (NIXD0002)", "byte_offset": 15,
              "data_type": "uint8", "bit_offset": 0, "bit_length": 5,
              "unit": "%", "calibration": "CIX00041TM"}]},
        {"id": "stix_54102", "service_type": 3, "service_subtype": 25,
         "pi1_value": 2,
         "fields": [
             {"name": "SID (NIX00020)", "byte_offset": 0,
              "data_type": "uint8"},
             {"name": "Instrument mode (NIXD0023)", "byte_offset": 1,
              "data_type": "uint8", "bit_offset": 4, "bit_length": 4,
              "calibration": "CIXT0999TM"},
             {"name": "HK_DPU_PCB_T (NIXD0025)", "byte_offset": 2,
              "data_type": "uint16", "bit_offset": 0, "bit_length": 12,
              "calibration": "CIXP0999TM"},
             {"name": "Archive Memory usage (NIXD0003)", "byte_offset": 51,
              "data_type": "uint8", "bit_offset": 7, "bit_length": 8},
             {"name": "RotBuff fill level (NIXD0501)", "byte_offset": 62,
              "data_type": "uint8", "bit_offset": 0, "bit_length": 3},
             {"name": "Med value of trig acc (NIX00072)", "byte_offset": 66,
              "data_type": "uint32", "bit_offset": 0, "bit_length": 24}]}]}


def _idb_synth(env):
    p = os.path.join(env.tmp, "idb-synth.json")
    with open(p, "w") as f:
        json.dump(_IDB_SYNTH, f)
    return p


def t_idb_rb_level(env):
    """A2 pinned semantics: byte_offset is relative to the app data and
    bit_offset counts from the MSB."""
    idb = idb_mod.load(_idb_synth(env))
    check(idb is not None, "the synthetic IDB loads")
    app = bytes(62) + bytes((0b101_00000,))
    check(idb.decode_hk(2, app)["NIXD0501"] == 5, "0b101_00000 -> 5")
    app = bytes(62) + bytes((0b000_00101,))
    check(idb.decode_hk(2, app)["NIXD0501"] == 0,
          "0b000_00101 -> 0 (MSB numbering, not LSB)")


def t_idb_cross_byte(env):
    """A2: a field may cross bytes (NIXD0003: byte 51, bit_offset 7,
    bit_length 8 -> bytes 51..52)."""
    idb = idb_mod.load(_idb_synth(env))
    app = bytes(51) + bytes((0x01, 0xFE))
    check(idb.decode_hk(2, app)["NIXD0003"] == 0xFF, "0x01,0xFE -> 0xFF")


def t_idb_mode_matches_minimal_decoder(env):
    """A2: the IDB mode field (NIXD0023, byte 1, bit_offset 4,
    bit_length 4) is decode.py's app[1] & 0x0F."""
    idb = idb_mod.load(_idb_synth(env))
    for _ in range(64):
        app = bytes(random.randrange(256) for _ in range(24))
        check(idb.decode_hk(1, app)["NIXD0023"] == app[1] & 0x0F,
              "mode of %s != app[1] & 0x0F" % app.hex())


def t_idb_missing(env):
    """A2: a missing IDB JSON is None with one timeline.idb record; a
    malformed field is the same (never a TypeError/KeyError out of
    load); the relay still logs tm records, without hk."""
    clk = SimClock()
    lg = log_mod.RecordLog(None, clk.now)
    missing = os.path.join(env.tmp, "no-idb.json")
    check(idb_mod.load(missing) is None, "load returns None")
    check(idb_mod.load(missing, log=lg) is None, "load returns None")
    recs = [r for r in lg.since(0) if r.obj["path"] == "timeline.idb"]
    check(len(recs) == 1 and "No such file" in recs[0].obj["data"]["error"],
          "one timeline.idb record: %r" % [r.obj["data"] for r in recs])
    # a malformed field makes the file broken: None + the record
    for i, fld in enumerate((
            {"name": 5, "byte_offset": 0, "data_type": "uint8"},
            {"name": "SID (NIX00020)", "data_type": "uint8"},
            {"name": "SID (NIX00020)", "byte_offset": 0})):
        p = os.path.join(env.tmp, "idb-bad-%d.json" % i)
        with open(p, "w") as f:
            json.dump({"byte_order": "big", "packets": [
                {"id": "stix_54102", "service_type": 3,
                 "service_subtype": 25, "pi1_value": 2,
                 "fields": [fld]}]}, f)
        lgb = log_mod.RecordLog(None, clk.now)
        check(idb_mod.load(p, log=lgb) is None, "a bad field: %r" % fld)
        recs = [r for r in lgb.since(0)
                if r.obj["path"] == "timeline.idb"]
        check(len(recs) == 1, "one timeline.idb record for %r: %r"
              % (fld, [r.obj["data"] for r in recs]))
    ctl = env.controller(timeline={"idb_json": missing})
    snd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        u = _tm_unit(0x5A4, 3, 25, bytes((2, 5)) + bytes(90))
        snd.sendto(u, ("127.0.0.1", ctl.cfg["ports"]["qemu_tm"]))
        wait_for(lambda: any(r.kind == "tm" for r in ctl.log.since(0)),
                 what="the tm record")
        rec = [r for r in ctl.log.since(0) if r.kind == "tm"][-1]
        check("hk" not in rec.obj["data"],
              "no hk without the IDB: %r" % sorted(rec.obj["data"]))
        check(rec.obj["data"]["decoded"]["sid"] == 2, "the SID 2 report")
    finally:
        snd.close()
        ctl.shutdown()


def t_idb_relay_hk(env):
    """A2: with the synthetic IDB, a TM(3,25) SID 2 report's tm record
    carries data.hk; app byte 62 = 0b101_00000 -> NIXD0501 == 5."""
    ctl = env.controller(timeline={"idb_json": _idb_synth(env)})
    snd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        app = bytes((2,)) + bytes(61) + bytes((0b101_00000,))
        u = _tm_unit(0x5A4, 3, 25, app)
        snd.sendto(u, ("127.0.0.1", ctl.cfg["ports"]["qemu_tm"]))
        wait_for(lambda: any(r.kind == "tm" for r in ctl.log.since(0)),
                 what="the tm record")
        rec = [r for r in ctl.log.since(0) if r.kind == "tm"][-1]
        check(rec.obj["data"]["decoded"]["sid"] == 2, "the SID 2 report")
        check(rec.obj["data"].get("hk", {}).get("NIXD0501") == 5,
              "hk attached with the RB level: %r"
              % rec.obj["data"].get("hk"))
    finally:
        snd.close()
        ctl.shutdown()


def t_idb_calibration(env):
    """HKC: the synthetic JSON's poly/interp/text calibrations: the
    values, the tm record's hk raw + hk_cal, the series catalogue
    kinds/units/values, and a broken or unknown calibration leaves the
    field raw with one timeline.idb record."""
    idb = idb_mod.load(_idb_synth(env))
    check(idb is not None, "the synthetic IDB loads")
    check(idb.calibrate(1, "NIXD0002", 5) == 20, "poly [0, 4]: 5 -> 20")
    check(idb.calibrate(2, "NIXD0025", 510) == 65,
          "interp between the points: 510 -> 65")
    check(idb.calibrate(2, "NIXD0025", 5) == 90 and
          idb.calibrate(2, "NIXD0025", 3000) == -10,
          "interp clamps outside the range")
    check(idb.calibrate(2, "NIXD0023", 2) == "SAFE",
          "text: raw 2 -> its text")
    check(idb.calibrate(2, "NIXD0023", 3) is None,
          "text: raw 3 is in no range -> None")
    check(idb.calibrate(2, "NIX00072", 9) == 9,
          "no calibration: the raw value")
    f = {x.code: x for x in idb.hk_fields(2)}
    check(f["NIXD0025"].cal == "interp" and f["NIXD0025"].unit == "degC"
          and not f["NIXD0025"].raw, "the interp field, the cal's unit")
    check(f["NIXD0023"].cal == "text" and
          f["NIXD0023"].labels == ["SAFE", "NOMINAL"], "the text field")
    check(f["NIX00072"].cal is None and f["NIX00072"].raw,
          "the raw field")
    # the relay record: data.hk stays the wire truth, data.hk_cal is
    # the calibrated one (SID byte 2, mode 2, PCB temp 510, RB 5)
    ctl = env.controller(timeline={"idb_json": _idb_synth(env)})
    snd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        app = bytes((2, 0x02, 0x1F, 0xE0)) + bytes(58) + \
            bytes((0b101_00000,))
        snd.sendto(_tm_unit(0x5A4, 3, 25, app),
                   ("127.0.0.1", ctl.cfg["ports"]["qemu_tm"]))
        wait_for(lambda: any(r.kind == "tm" for r in ctl.log.since(0)),
                 what="the tm record")
        rec = [r for r in ctl.log.since(0) if r.kind == "tm"][-1]
        d = rec.obj["data"]
        check(d["hk"]["NIXD0501"] == 5 and d["hk"]["NIXD0025"] == 510,
              "hk stays raw: %r" % d.get("hk"))
        check(d.get("hk_cal") == {"NIXD0023": "SAFE", "NIXD0025": 65},
              "hk_cal: %r" % d.get("hk_cal"))
    finally:
        snd.close()
        ctl.shutdown()
    # the series: poly/interp numbers, the text enum indexed in the
    # text_ranges order (NOMINAL fed first, still index 1), aliases
    # mirror it, raw fields say "raw"
    si = series_mod.SeriesIndex(idb=idb)
    si.feed(_srec("tm", 100, data={
        "decoded": {"sid": 2},
        "hk": {"NIXD0023": 5, "NIXD0025": 510, "NIX00072": 9},
        "hk_cal": {"NIXD0023": "NOMINAL", "NIXD0025": 65}}))
    si.feed(_srec("tm", 110, data={
        "decoded": {"sid": 2}, "hk": {"NIXD0023": 2},
        "hk_cal": {"NIXD0023": "SAFE"}}))
    si.feed(_srec("tm", 200, data={
        "decoded": {"sid": 1}, "hk": {"NIXD0002": 5},
        "hk_cal": {"NIXD0002": 20}}))
    cat = {e["id"]: e for e in si.catalogue()}
    m = cat["hk:2:NIXD0023"]
    check(m["kind"] == "enum" and m["cal"] == "text" and
          m["values"] == ["SAFE", "NOMINAL"], "the text series: %r" % m)
    check(cat["hk.mode"]["kind"] == "enum" and
          cat["hk.mode"]["cal"] == "text" and
          cat["hk.mode"]["values"] == ["SAFE", "NOMINAL"],
          "the alias mirrors the text series")
    q = si.query(["hk:2:NIXD0023", "hk.mode"])
    check(q["hk:2:NIXD0023"]["t"] == [100, 110] and
          q["hk:2:NIXD0023"]["v"] == [1, 0] and
          q["hk:2:NIXD0023"]["labels"] == ["SAFE", "NOMINAL"],
          "the enum index is the label's, not first-seen: %r"
          % q["hk:2:NIXD0023"])
    m = cat["hk:2:NIXD0025"]
    check(m["kind"] == "number" and m["unit"] == "degC" and
          m["cal"] == "interp", "the interp series: %r" % m)
    m = cat["hk:2:NIX00072"]
    check(m["kind"] == "number" and m["unit"] == "raw" and
          m["cal"] == "raw", "the raw series: %r" % m)
    m = cat["hk.cpu_load"]
    check(m["kind"] == "number" and m["unit"] == "%" and
          m["cal"] == "poly" and
          si.query(["hk.cpu_load"])["hk.cpu_load"]["v"] == [20],
          "the alias carries the calibrated number: %r" % m)
    # a record without hk_cal (an old log): the raw fallback
    si2 = series_mod.SeriesIndex(idb=idb)
    si2.feed(_srec("tm", 5, data={"decoded": {"sid": 2},
                                  "hk": {"NIXD0023": 3}}))
    m = {e["id"]: e for e in si2.catalogue()}["hk:2:NIXD0023"]
    check(m["kind"] == "number" and m["unit"] == "raw" and
          m["cal"] == "raw", "without hk_cal: raw: %r" % m)
    # an unknown type or a broken entry: the field stays raw, one
    # timeline.idb record lists them, the load never fails
    clk = SimClock()
    lg = log_mod.RecordLog(None, clk.now)
    p = os.path.join(env.tmp, "idb-bad-cal.json")
    with open(p, "w") as f:
        json.dump({"byte_order": "big", "calibrations": {
            "CIXX0001TM": {"type": "magic"},
            "CIXX0002TM": {"type": "polynomial", "coefficients": "no"},
            "CIXX0003TM": {"type": "interpolation", "points": [[1]]},
            "CIXX0004TM": {"type": "text", "text_ranges": [[0]]},
            "CIXX0006TM": {"type": "polynomial",
                           "coefficients": [0, float("nan")]},
            "CIXX0007TM": {"type": "text", "text_ranges": [[5, 1, "x"]]},
            "CIXX0008TM": {"type": "text",
                           "text_ranges": [[0, 0, "x"], [1, 1, "x"]]}},
            "packets": [{"id": "stix_54102", "service_type": 3,
                         "service_subtype": 25, "pi1_value": 2,
                         "fields": [
                             {"name": "a (NIXD0600)", "byte_offset": 0,
                              "data_type": "uint8",
                              "calibration": "CIXX0001TM"},
                             {"name": "b (NIXD0601)", "byte_offset": 1,
                              "data_type": "uint8",
                              "calibration": "CIXX0002TM"},
                             {"name": "c (NIXD0602)", "byte_offset": 2,
                              "data_type": "uint8",
                              "calibration": "CIXX0003TM"},
                             {"name": "d (NIXD0603)", "byte_offset": 3,
                              "data_type": "uint8",
                              "calibration": "CIXX0004TM"},
                             {"name": "e (NIXD0604)", "byte_offset": 4,
                              "data_type": "uint8",
                              "calibration": "CIXX0005TM"},
                             {"name": "f (NIXD0605)", "byte_offset": 5,
                              "data_type": "uint8",
                              "calibration": "CIXX0006TM"},
                             {"name": "g (NIXD0606)", "byte_offset": 6,
                              "data_type": "uint8",
                              "calibration": "CIXX0007TM"},
                             {"name": "h (NIXD0607)", "byte_offset": 7,
                              "data_type": "uint8",
                              "calibration": "CIXX0008TM"}]}]}, f)
    idb = idb_mod.load(p, log=lg)
    check(idb is not None, "a broken calibration never fails the load")
    check(all(x.raw for x in idb.hk_fields(2)),
          "all eight stay raw (NaN, lo > hi, duplicate texts included)")
    recs = [r for r in lg.since(0) if r.obj["path"] == "timeline.idb"]
    check(len(recs) == 1 and
          len(recs[0].obj["data"].get("uncalibrated") or ()) == 8,
          "one record lists them: %r" % [r.obj["data"] for r in recs])
    # finite coefficients can still overflow to inf: hk_cal drops the
    # value instead of killing the relay (records are allow_nan=False)
    p = os.path.join(env.tmp, "idb-inf-cal.json")
    with open(p, "w") as f:
        json.dump({"byte_order": "big", "calibrations": {
            "CIXX0009TM": {"type": "polynomial",
                           "coefficients": [0, 0, 0, 0, 1e299]},
            "CIXX0010TM": {"type": "polynomial", "coefficients": [1, 2]}},
            "packets": [{"id": "stix_54102", "service_type": 3,
                         "service_subtype": 25, "pi1_value": 2,
                         "fields": [
                             {"name": "i (NIXD0608)", "byte_offset": 0,
                              "data_type": "uint8",
                              "calibration": "CIXX0009TM"},
                             {"name": "j (NIXD0609)", "byte_offset": 1,
                              "data_type": "uint8",
                              "calibration": "CIXX0010TM"}]}]}, f)
    idb = idb_mod.load(p)
    stub = types.SimpleNamespace(idb=idb)
    got = relay_mod.Relay._hk_cal(stub, {"sid": 2},
                                  {"NIXD0608": 255, "NIXD0609": 3})
    check(got == {"NIXD0609": 7}, "the inf value is dropped: %r" % got)
    json.dumps(got, allow_nan=False)


def t_idb_real_json(env):
    """A2: the ground station's real JSON (skipped when the GS repo is
    absent): SID 1 has > 30 and SID 2 > 80 fields, the aliases resolve,
    every coded field with a known data_type decodes, the mode field is
    the minimal decoder's, the JSON's calibrations apply."""
    path = os.path.expanduser(config_mod.DEFAULTS["timeline"]["idb_json"])
    if not os.path.exists(path):
        print("     (skipped: no IDB JSON at %s)" % path)
        return
    idb = idb_mod.load(path)
    check(idb is not None, "the real IDB loads")
    f1, f2 = idb.hk_fields(1), idb.hk_fields(2)
    check(len(f1) > 30, "SID 1 has %d fields" % len(f1))
    check(len(f2) > 80, "SID 2 has %d fields" % len(f2))
    for alias, (sid, code) in sorted(idb.aliases.items()):
        check(code in [f.code for f in idb.hk_fields(sid)],
              "alias %s -> (%d, %s) is in the catalogue" % (alias, sid,
                                                            code))
    check(idb.aliases.get("hk.rb_level") == (2, "NIXD0501") and
          idb.aliases.get("hk.archive_usage") == (2, "NIXD0003") and
          idb.aliases.get("hk.cpu_load") == (2, "NIXD0002") and
          idb.aliases.get("hk.mode") == (2, "NIXD0023") and
          idb.aliases.get("hk.trig_med") == (2, "NIX00072") and
          idb.aliases.get("hk.trig_max") == (2, "NIX00073"),
          "the pinned aliases: %r" % idb.aliases)
    # decoded fields per SID == the JSON's coded fields with a known
    # data_type; the skipped data_types are named in the failure text
    with open(path, encoding="utf-8") as f:
        raw = json.load(f)
    for sid in (1, 2):
        flds = [x for pkt in raw["packets"] if isinstance(pkt, dict) and
                (pkt.get("service_type"), pkt.get("service_subtype"),
                 pkt.get("pi1_value")) == (3, 25, sid)
                for x in (pkt.get("fields") or []) if isinstance(x, dict)]
        coded = [x for x in flds
                 if idb_mod.CODE_RE.search(x.get("name") or "")]
        known = [x for x in coded if x.get("data_type") in idb_mod._TYPES]
        unk = sorted({x["data_type"] for x in coded
                      if isinstance(x.get("data_type"), str) and
                      x["data_type"] not in idb_mod._TYPES})
        check(len(idb.hk_fields(sid)) == len(known),
              "SID %d: %d decoded != %d coded with a known data_type "
              "(skipped data_types: %r)" % (sid, len(idb.hk_fields(sid)),
                                            len(known), unk))
    check(all(f.raw == (f.cal is None) and
              f.cal in (None, "poly", "interp", "text")
              for f in f1 + f2), "raw iff no usable calibration")
    app = bytes(random.randrange(256) for _ in range(96))
    check(idb.decode_hk(1, app)["NIXD0023"] == app[1] & 0x0F,
          "the real mode field is the minimal decoder's")


def t_idb_real_calibrations(env):
    """HKC: the real JSON's calibrations (skipped when the GS repo is
    absent): mode 5 is NOMINAL, the CPU load scales by 4, the RB level
    7 is the overflow text, the temperatures clamp below their first
    point."""
    path = os.path.expanduser(config_mod.DEFAULTS["timeline"]["idb_json"])
    if not os.path.exists(path):
        print("     (skipped: no IDB JSON at %s)" % path)
        return
    idb = idb_mod.load(path)
    check(idb is not None, "the real IDB loads")
    check(idb.calibrate(2, "NIXD0023", 5) == "NOMINAL",
          "mode raw 5 -> NOMINAL")
    check(idb.calibrate(2, "NIXD0002", 3) == 12, "CPU load raw 3 -> 12")
    check(idb.calibrate(2, "NIXD0501", 7) == "Overflow occur",
          "RB level raw 7 -> the overflow text")
    check(idb.calibrate(2, "NIXD0025", 100) == 99.99,
          "an interpolation clamps below its first point")
    f = {x.code: x for x in idb.hk_fields(2)}
    check(f["NIXD0023"].cal == "text" and
          f["NIXD0023"].labels.index("NOMINAL") == 5, "the mode labels")
    check(f["NIXD0002"].cal == "poly" and f["NIXD0002"].unit == "%" and
          not f["NIXD0002"].raw, "the CPU load poly keeps its unit")
    check(f["NIXD0501"].cal == "text" and
          f["NIXD0501"].labels[7] == "Overflow occur", "the RB labels")
    check(f["NIX00072"].cal is None and f["NIX00072"].raw,
          "the trigger med has no calibration")


def t_tc_237_18_label(env):
    """A3: a TC(237,18) record carries data.param {index, sub, value}
    (PIX00016/17/18, big-endian, TcService237.c:66-94); dropped or not,
    and no label for other subtypes or too-short app data."""
    ctl = env.controller()
    try:
        sp = tcbuild.tc_packet(0x5AC, 1, 237, 18,
                               bytes.fromhex("01380000000000"))
        ctl.plat.relay.gs_tc(sp)    # power off: dropped, still logged
        rec = [r for r in ctl.log.since(0) if r.kind == "tc"][-1]
        check(rec.obj["data"]["param"] == {"index": 312, "sub": 0,
                                           "value": 0},
              "the label: %r" % rec.obj["data"])
        sp = tcbuild.tc_packet(0x5AC, 2, 237, 18, b"\x01\x38")
        ctl.plat.relay.gs_tc(sp)
        rec = [r for r in ctl.log.since(0) if r.kind == "tc"][-1]
        check("param" not in rec.obj["data"], "no param below 7 app bytes")
        sp = tcbuild.tc_packet(0x5AC, 3, 237, 19, bytes(7))
        ctl.plat.relay.gs_tc(sp)
        rec = [r for r in ctl.log.since(0) if r.kind == "tc"][-1]
        check("param" not in rec.obj["data"], "TC(237,19) is not labelled")
    finally:
        ctl.shutdown()


# -- timeline series, views, view-run (A4, A5, A6) -------------------------
def _srec(kind, t, path="", data=None):
    """A minimal record dict for SeriesIndex.feed."""
    return {"kind": kind, "sim_time": t, "path": path,
            "data": data if data is not None else {}}


def t_series_decimate_keeps_spikes(env):
    """A4: a window with more points than max_points is decimated into
    equal-time buckets, each with first t and last/min/max, so a one-
    point spike survives in max."""
    si = series_mod.SeriesIndex()
    t0 = 1000 * NS
    for i in range(10000):
        si.feed(_srec("sample", t0 + i * NS,
                      data={"x": 99.0 if i == 5000 else 1.0}))
    s = si.query(["truth:x"], max_points=100)["truth:x"]
    check(len(s["t"]) <= 100 and "min" in s and "max" in s,
          "decimated into buckets: %d" % len(s["t"]))
    check(99.0 in s["max"] and 99.0 not in s["min"],
          "the spike survives in max: %r" % s["max"][:4])
    # a window that fits is served as-is, without min/max
    s = si.query(["truth:x"], t_from=t0, t_to=t0 + 5 * NS,
                 max_points=100)["truth:x"]
    check("min" not in s and s["v"] == [1.0] * 6, "small window: %r" % s)


def t_series_enum_steps(env):
    """A4: an enum leaf (the timesync verdict) plots as labelled steps,
    the value's index in the order first seen; repeats add no step."""
    si = series_mod.SeriesIndex()
    p = "platform.timesync.verdict"
    for i, v in enumerate(("none", "ok", "late_apply")):
        si.feed(_srec("sample", 10 + i, data={p: v}))
    si.feed(_srec("sample", 20, data={p: "late_apply"}))    # no change
    cat = {e["id"]: e for e in si.catalogue()}["truth:" + p]
    check(cat["kind"] == "enum" and cat["values"] ==
          ["none", "ok", "late_apply"], "the catalogue: %r" % cat)
    s = si.query(["truth:" + p], max_points=1000)["truth:" + p]
    check(s["t"] == [10, 11, 12] and s["v"] == [0, 1, 2] and
          s["labels"] == ["none", "ok", "late_apply"],
          "labelled steps: %r" % s)


def t_series_markers(env):
    """A4: fault inject, TC(237,18) and a TM(5,x) EID carry marker
    labels; a mode change is both an event and a mode marker."""
    ctl = env.controller()
    snd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        ctl.command("fault.inject", {"id": "spw.link.disconnect"})
        sp = tcbuild.tc_packet(0x5AC, 1, 237, 18,
                               bytes.fromhex("01380000000000"))
        ctl.plat.relay.gs_tc(sp)
        snd.sendto(_tm_unit(0x5A7, 5, 1, bytes.fromhex("0021")),
                   ("127.0.0.1", ctl.cfg["ports"]["qemu_tm"]))
        wait_for(lambda: any(r.kind == "tm" for r in ctl.log.since(0)),
                 what="the event TM")
        q = ctl.series.query(["fault", "tc", "event", "mode"],
                             max_points=100)
        check(q["unknown"] == [], "all four markers exist: %r" % q["unknown"])
        check("fault inject spw.link.disconnect" in q["fault"]["labels"],
              "the fault label: %r" % q["fault"]["labels"])
        check("TC(237,18) param 312 = 0" in q["tc"]["labels"],
              "the TC label: %r" % q["tc"]["labels"])
        check("EID 0x0021 mode change" in q["event"]["labels"] and
              "EID 0x0021 mode change" in q["mode"]["labels"],
              "the event and mode labels: %r / %r"
              % (q["event"]["labels"], q["mode"]["labels"]))
    finally:
        snd.close()
        ctl.shutdown()
    # keep-alive summaries (tc records on platform.keepalive, every 10 s)
    # are not TC markers; a real TC is
    si = series_mod.SeriesIndex()
    si.feed(_srec("tc", 1000, "platform.keepalive",
                  {"service": 20, "subtype": 128}))
    si.feed(_srec("tc", 2000, "platform.timesync",
                  {"service": 9, "subtype": 129}))
    q = si.query(["tc"], max_points=100)
    check(q["tc"]["t"] == [2000], "keep-alive is no TC marker: %r" % q["tc"])


def t_series_cap(env):
    """A4: over-cap drops the oldest points per series and the
    catalogue notes truncated_before."""
    si = series_mod.SeriesIndex(max_points_per_series=1000)
    for i in range(1500):
        si.feed(_srec("sample", 1000 + i, data={"x": i}))
    cat = {e["id"]: e for e in si.catalogue()}["truth:x"]
    check(cat["truncated_before"] == 1500, "truncated_before: %r" % cat)
    s = si.query(["truth:x"], max_points=5000)["truth:x"]
    check(len(s["t"]) == 1000 and s["t"][0] == 1500 and
          s["v"][0] == 500 and s["v"][-1] == 1499,
          "the last 1000 kept: %r" % s["v"][:2])


def t_series_concurrent(env):
    """A4: one thread feeds 20000 records over many new ids while
    another calls catalogue()/query() in a loop; the index lock keeps
    the reads consistent, no exception."""
    si = series_mod.SeriesIndex()
    stop = threading.Event()
    err = []

    def feed():
        try:
            for i in range(20000):
                si.feed(_srec("sample", i, data={"x.%d" % (i % 50): i}))
        except Exception as e:
            err.append(e)
        finally:
            stop.set()

    th = threading.Thread(target=feed)
    th.start()
    while not stop.is_set():
        si.catalogue()
        si.query(["truth:x.0", "truth:x.1", "nope"], max_points=100)
    th.join()
    check(not err, "no exception out of feed: %r" % err[:1])
    ids = {e["id"] for e in si.catalogue()}
    check(len(ids) == 50 and si.query(sorted(ids))["unknown"] == [],
          "all 50 series there: %d" % len(ids))


def _series_server(env):
    ctl = env.controller()
    srv = api_mod.make_server(ctl, "127.0.0.1", 0)
    return ctl, srv, srv.server_address[1]


def t_series_api_bad_input(env):
    """A4: 400 on bad input: no ids, an empty ids= value, max_points
    out of 10..5000 or not an integer, a bad from/to."""
    ctl, srv, port = _series_server(env)
    try:
        for q in ("", "?ids=", "?ids=&ids=truth:x",
                  "?ids=truth:x&max_points=0",
                  "?ids=truth:x&max_points=5001",
                  "?ids=truth:x&max_points=x", "?ids=truth:x&from=abc"):
            st, _, body = _http(port, "GET", "/api/v1/series" + q)
            check(st == 400, "%r -> %d (%r)" % (q, st, body[:80]))
        st, _, _ = _http(port, "GET",
                         "/api/v1/series?ids=truth:x&max_points=10")
        check(st == 200, "max_points 10 is served: %d" % st)
    finally:
        srv.shutdown()
        srv.server_close()
        ctl.shutdown()


def t_series_api_ids_repeat(env):
    """A4: the ids are repeated ids= query parameters, each one id (the
    api _csv helper is not used, so no comma splitting); an unknown id
    is an empty series listed in "unknown"."""
    ctl, srv, port = _series_server(env)
    try:
        ctl.log.emit("ctl", "sample", "", {"a": 1, "b": 2})
        st, _, body = _http(port, "GET",
                            "/api/v1/series?ids=truth:a&ids=truth:b")
        d = json.loads(body)
        check(st == 200 and sorted(d) == ["truth:a", "truth:b", "unknown"]
              and d["unknown"] == [], "both served: %r" % sorted(d))
        check(d["truth:a"]["v"] == [1] and d["truth:b"]["v"] == [2],
              "the values: %r" % d)
        st, _, body = _http(port, "GET",
                            "/api/v1/series?ids=truth:a,truth:b")
        d = json.loads(body)
        check(d["unknown"] == ["truth:a,truth:b"],
              "one ids= value is one id, no comma split: %r" % d["unknown"])
        st, _, body = _http(port, "GET", "/api/v1/series/catalogue")
        cat = json.loads(body)
        check(st == 200 and "truth:a" in [e["id"] for e in cat],
              "the catalogue: %r" % [e["id"] for e in cat][:4])
    finally:
        srv.shutdown()
        srv.server_close()
        ctl.shutdown()


def t_log_listeners(env):
    """The A4 listener rule: called after the ring append and after the
    log lock is released, each in try/except, never raised, and the
    others still run (also for the error record itself, without
    recursion); a failing listener is one log.listener_error record,
    again only after it succeeded once."""
    lg = log_mod.RecordLog(None, None)
    seen = []
    fail = [True]

    def ok(rec):
        seen.append(rec.seq)
        check(lg.cond.acquire(blocking=False),
              "the listener ran under the log lock")
        lg.cond.release()

    def bad(rec):
        if fail[0]:
            raise RuntimeError("boom")

    lg.listeners += [ok, bad]
    rec = lg.emit("ctl", "log", "x", {"i": 1})
    errs = [r for r in lg.since(0) if r.obj["path"] == "log.listener_error"]
    check(len(errs) == 1 and
          errs[0].obj["data"]["error"] == "RuntimeError: boom" and
          errs[0].obj["data"]["listener"].endswith("bad"),
          "one record for the failing listener: %r"
          % [r.obj["data"] for r in errs])
    check(seen == [rec.seq, errs[0].seq],
          "both records reached the listener, the error round did not "
          "recurse: %r" % seen)
    lg.emit("ctl", "log", "y")
    errs = [r for r in lg.since(0) if r.obj["path"] == "log.listener_error"]
    check(len(errs) == 1, "still failing: reported once, not per call")
    fail[0] = False
    lg.emit("ctl", "log", "z")          # bad succeeds: re-armed
    fail[0] = True
    lg.emit("ctl", "log", "v")
    errs = [r for r in lg.since(0) if r.obj["path"] == "log.listener_error"]
    check(len(errs) == 2, "re-armed after a success: %d" % len(errs))
    check(lg.emit("ctl", "log", "end") is not None, "emit never raises")


def _post():
    return {"Content-Type": "application/json", "X-Stix-Platform": "1"}


def t_views(env):
    """A5: the name rule before any file access, view validation, the
    starter views copied only when missing, "everything" generated and
    undeletable, and view.save/view.delete round-trip."""
    d = os.path.join(env.tmp, "views-root")
    vdir = os.path.join(d, "views")
    ctl = env.controller(retention={"run_root": d})
    srv = api_mod.make_server(ctl, "127.0.0.1", 0)
    port = srv.server_address[1]
    good = {"name": "mine", "window": "10m", "markers": ["tc"],
            "panels": [{"title": "p", "series": ["truth:x"]}]}
    try:
        for bad in ("../x", "a/b", "", ".hidden", "x" * 65, "a b", 5):
            e = raises(VerbError, views_mod.check_name, bad)
            check(e.status == 400, "a bad name is 400: %r" % (bad,))
        for ok in ("a", "Z9._-", "x" * 64):
            views_mod.check_name(ok)
        check(views_mod.validate(dict(good)) == good, "a good view")
        for bad in ({}, dict(good, window="1h"), dict(good, markers=["x"]),
                    dict(good, markers="tc"), dict(good, panels={}),
                    dict(good, panels=[{"title": "p"}]),
                    dict(good, panels=[{"title": "", "series": []}]),
                    dict(good, name="../x")):
            raises(VerbError, views_mod.validate, bad)
        # the starters, copied only when missing
        st, _, body = _http(port, "GET", "/api/v1/views")
        lst = json.loads(body)
        check(st == 200 and lst["dir"] == vdir and lst["views"] and
              sorted(lst["views"]) == sorted(f[:-len(".json")]
                                             for f in os.listdir(vdir)),
              "the views dir: %r" % lst)
        check(len(lst["views"]) == 4, "the four starters: %r" % lst["views"])
        for name in lst["views"]:
            with open(os.path.join(vdir, name + ".json")) as f:
                views_mod.validate(json.load(f))
        keep, drop = lst["views"][0], lst["views"][1]
        changed = '{"name": "%s", "window": "1m", "markers": [], ' \
                  '"panels": []}' % keep
        with open(os.path.join(vdir, keep + ".json"), "w") as f:
            f.write(changed)
        os.unlink(os.path.join(vdir, drop + ".json"))
        ctl2 = env.controller(retention={"run_root": d})
        try:
            with open(os.path.join(vdir, drop + ".json")) as f:
                json.load(f)
            with open(os.path.join(vdir, keep + ".json")) as f:
                check(f.read() == changed, "a changed starter is kept")
        finally:
            ctl2.shutdown()
        # "everything": generated, before <name>, undeletable
        st, _, body = _http(port, "GET", "/api/v1/views/everything")
        ev = json.loads(body)
        check(st == 200 and ev["name"] == "everything" and
              ev["window"] == "run" and
              ev["markers"] == list(views_mod.MARKERS) and
              any(p["title"] == "platform" and
                  "truth:platform.power" in p["series"]
                  for p in ev["panels"]),
              "everything is generated: %r" % ev)
        st, _, body = _http(port, "POST", "/api/v1/cmd/view.save",
                            body=json.dumps({"name": "mine",
                                             "view": good}).encode(),
                            headers=_post())
        check(st == 200 and os.path.isfile(os.path.join(vdir, "mine.json")),
              "view.save: %d %r" % (st, body[:80]))
        st, _, body = _http(port, "GET", "/api/v1/views/mine")
        check(st == 200 and json.loads(body)["name"] == "mine",
              "read back: %d" % st)
        st, _, _ = _http(port, "GET", "/api/v1/views/no-such")
        check(st == 404, "an unknown view is 404")
        with open(os.path.join(vdir, "broken.json"), "w") as f:
            f.write('{"name": "broken", ')
        st, _, _ = _http(port, "GET", "/api/v1/views/broken")
        check(st == 400, "a corrupt view file is 400")
        os.unlink(os.path.join(vdir, "broken.json"))
        st, _, _ = _http(port, "POST", "/api/v1/cmd/view.save",
                         body=json.dumps({"name": "everything",
                                          "view": good}).encode(),
                         headers=_post())
        check(st == 409, "everything cannot be saved over")
        st, _, _ = _http(port, "POST", "/api/v1/cmd/view.delete",
                         body=b'{"name": "everything"}', headers=_post())
        check(st == 409, "everything cannot be deleted")
        st, _, _ = _http(port, "POST", "/api/v1/cmd/view.save",
                         body=b'{"name": "../x", "view": {}}',
                         headers=_post())
        check(st == 400, "a bad name is refused")
        st, _, _ = _http(port, "GET", "/api/v1/views/%2e%2e%2fx")
        check(st == 400, "a bad name in the path too")
        check(not os.path.exists(os.path.join(d, "x.json")),
              "nothing written outside the views dir")
        st, _, _ = _http(port, "POST", "/api/v1/cmd/view.delete",
                         body=b'{"name": "mine"}', headers=_post())
        check(st == 200, "deleted")
        st, _, _ = _http(port, "POST", "/api/v1/cmd/view.delete",
                         body=b'{"name": "mine"}', headers=_post())
        check(st == 404, "gone")
    finally:
        srv.shutdown()
        srv.server_close()
        ctl.shutdown()


def _dir_state(d):
    out = []
    for root, _, files in os.walk(d):
        for n in files:
            p = os.path.join(root, n)
            out.append((os.path.relpath(p, d), os.stat(p).st_size))
    return sorted(out)


def t_view_run(env):
    """A6: a finished run's log (one rotated part) read-only: /status
    says view_run, the rotated part is read oldest first, every verb
    but view.* is a 403, the page and the views dir are served, and
    nothing is written into the run dir."""
    d = os.path.realpath(os.path.join(env.tmp, "run-view"))
    os.makedirs(d)
    vdir = os.path.join(env.tmp, "views-view")
    t = [0.0]
    lg = log_mod.RecordLog(None, lambda: (int(t[0]), 0, None, True))
    lg.emit("ctl", "log", "controller.start",
            {"run_dir": d, "config": {
                "timeline": {"views_dir": vdir, "idb_json": "",
                             "idb_sqlite": "", "sample_paths": [],
                             "sample_s": 0.0},
                "retention": {"run_root": env.tmp}}})
    for i in range(60):
        t[0] += 1.0
        lg.emit("ctl", "sample", "", {"instrument.rotb.count": i})
    lg.emit("ctl", "fault", "fault.spw.link.disconnect",
            {"action": "inject", "params": {}})
    recs = lg.since(0)
    lg.close()
    with gzip.open(os.path.join(d, "log.jsonl.1.gz"), "wt") as f:
        for r in recs[:21]:         # the oldest part, controller.start in
            f.write(r.line + "\n")
    with open(os.path.join(d, "log.jsonl"), "w") as f:
        for r in recs[21:]:
            f.write(r.line + "\n")
    before = _dir_state(d)
    vr = viewrun_mod.ViewRun(d)
    srv = api_mod.make_server(vr, "127.0.0.1", 0)
    port = srv.server_address[1]
    try:
        st, _, body = _http(port, "GET", "/api/v1/status")
        s = json.loads(body)
        check(st == 200 and s["view_run"] == d and s["mode"] == "view",
              "status: %r" % s)
        st, _, body = _http(port, "GET",
                            "/api/v1/series?ids=truth:instrument.rotb.count")
        q = json.loads(body)
        srs = q["truth:instrument.rotb.count"]
        check(st == 200 and q["unknown"] == [] and srs["v"][0] == 0 and
              srs["v"][-1] == 59 and len(srs["v"]) == 60,
              "the whole run, oldest part first: %r" % srs["v"][:2])
        st, _, body = _http(port, "GET", "/api/v1/series?ids=fault")
        check(json.loads(body)["fault"]["labels"] ==
              ["fault inject spw.link.disconnect"], "the marker")
        st, _, _ = _http(port, "POST", "/api/v1/cmd/log.mark",
                         body=b'{"text": "x"}', headers=_post())
        check(st == 403, "log.mark refused")
        st, _, _ = _http(port, "POST", "/api/v1/cmd/platform.power.on",
                         body=b"{}", headers=_post())
        check(st == 403, "power.on refused")
        view = {"name": "from-view-run", "window": "run", "markers": ["tc"],
                "panels": [{"title": "p", "series": ["truth:x"]}]}
        st, _, body = _http(port, "POST", "/api/v1/cmd/view.save",
                            body=json.dumps({"name": "from-view-run",
                                             "view": view}).encode(),
                            headers=_post())
        check(st == 200 and
              os.path.isfile(os.path.join(vdir, "from-view-run.json")),
              "view.save works: %d %r" % (st, body[:80]))
        st, _, _ = _http(port, "POST", "/api/v1/cmd/view.delete",
                         body=b'{"name": "from-view-run"}', headers=_post())
        check(st == 200, "view.delete works in a view-run")
        st, _, _ = _http(port, "POST", "/api/v1/cmd/view.delete",
                         body=b'{"name": "from-view-run"}', headers=_post())
        check(st == 404, "an unknown view is 404")
        st, _, body = _http(port, "GET", "/api/v1/views")
        check(json.loads(body)["dir"] == vdir, "the run's own views dir")
        st, _, _ = _http(port, "GET", "/")
        check(st == 200, "the page")
    finally:
        srv.shutdown()
        srv.server_close()
    check(_dir_state(d) == before, "nothing written into the run dir")
    raises(viewrun_mod.ViewRunError, viewrun_mod.ViewRun,
           os.path.join(env.tmp, "no-such-run"))


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
         ("IDB: RB level (A2)", t_idb_rb_level),
         ("IDB: cross-byte field (A2)", t_idb_cross_byte),
         ("IDB: mode == minimal decoder (A2)",
          t_idb_mode_matches_minimal_decoder),
         ("IDB: missing JSON (A2)", t_idb_missing),
         ("IDB: relay attaches hk (A2)", t_idb_relay_hk),
         ("IDB: calibration (HKC)", t_idb_calibration),
         ("IDB: real JSON (A2)", t_idb_real_json),
         ("IDB: real calibrations (HKC)", t_idb_real_calibrations),
         ("truncated TC/TM input", t_truncated),
         ("time sync window (D-PLd)", t_timesync_window),
         ("time sync verdicts", t_timesync_verdicts),
         ("time sync reserve + re-check", t_timesync_reserve),
         ("time sync autostart (B2)", t_timesync_auto),
         ("keep-alive pacing", t_keepalive),
         ("keep-alive: no drift", t_keepalive_drift),
         ("keep-alive: aggregate log", t_keepalive_log),
         ("keep-alive: rx trace filter", t_keepalive_trace),
         ("sampler record shape (A1)", t_sampler_record_shape),
         ("sampler by mode (A1)", t_sampler_off_in_run_mode),
         ("scenario: validation", t_scn_validate),
         ("scenario: runner", t_scn_runner),
         ("scenario: variants, T0, after", t_scn_variants),
         ("scenario: pause, abort, errors", t_scn_pause_abort),
         ("scenario: state and CUC", t_scn_state_cuc),
         ("scenario verbs", t_scn_verbs),
         ("scenario: review fixes", t_scn_review),
         ("run mode: review fixes", t_runmode_review),
         ("platform state leaves", t_platform_leaves),
         ("run mode", t_runmode),
         ("TM relay + TC proxy", t_relay),
         ("hold queue (SF5)", t_hold_queue),
         ("GS side security (SF6)", t_gs_security),
         ("platform TCs (9/20, R2)", t_platform_tcs),
         ("TC(237,18) label (A3)", t_tc_237_18_label),
         ("record log: listeners (A4)", t_log_listeners),
         ("series: decimation keeps spikes (A4)",
          t_series_decimate_keeps_spikes),
         ("series: enum steps (A4)", t_series_enum_steps),
         ("series: markers (A4)", t_series_markers),
         ("series: cap (A4)", t_series_cap),
         ("series: concurrent feed and read (A4)", t_series_concurrent),
         ("series API: bad input (A4)", t_series_api_bad_input),
         ("series API: repeated ids (A4)", t_series_api_ids_repeat),
         ("views (A5)", t_views),
         ("view-run (A6)", t_view_run),
         ("fault lifecycle (SF4)", t_fault_lifecycle),
         ("fault catalogue + forcing", t_faults),
         ("fault model: truth|presented, qom", t_fault_model),
         ("fault re-apply across a power cycle (PL-11a)", t_fault_reapply),
         ("attenuator + EDAC state leaves (PL-11b)", t_att_edac_leaves),
         ("detector + ROTB state leaves (PL-11c)", t_det_rotb_leaves),
         ("detector burst verb (FF)", t_det_burst),
         ("flash library (FF)", t_flashlib),
         ("guest-reset policies", t_guest_reset),
         ("D-PLj filter", t_filter), ("SSE + Last-Event-ID", t_sse),
         ("verbs == docs table", t_verbs_doc), ("probe_lib", t_probe_lib),
         ("gate ports from the slot band", t_gate_slots),
         ("UI: served at /, CSP, scenarios", t_ui_server),
         ("UI: the page (no browser)", t_ui_page),
         ("UI: tabs (timeline B1)", t_ui_tabs),
         ("UI: timeline (C1)", t_ui_timeline),
         ("setup status (timeline B2)", t_setup_status),
         ("UI: the live check (smokes)", t_ui_check)]


def main(only=None):
    env = Env()
    failed = ran = 0
    try:
        for name, fn in CASES:
            if only and only not in name:
                continue
            ran += 1
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
    if not ran:
        failed = 1
        print("FAIL no case matches %r" % only)
    print("SELF-TEST: %s" % ("PASS" if not failed else "FAIL"))
    return 1 if failed else 0
