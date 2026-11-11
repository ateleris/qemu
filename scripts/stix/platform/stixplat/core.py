# SPDX-License-Identifier: GPL-2.0-or-later
"""The controller core: the record log, the bench clock, the power
model, the state tree, the dispatcher and the poller, wired together
(platform plan PL-3).
"""

import json
import os
import re
import threading
import time

from . import log as log_mod
from . import power as power_mod
from . import qemu as qemu_mod
from . import rundir
from .dispatch import Dispatcher, VerbError
from .qmp import QmpError
from .simclock import SimClock
from .state import StateTree

TRACE_NAME_RE = re.compile(r"^(?:\d+@\d+\.\d+:)?(\w+) ")
DEBUG_QMP = ("qom-get", "qom-set", "query-status")


class Controller:
    def __init__(self, cfg, run_dir=None, launcher=None, mono_fn=None,
                 sleep=time.sleep):
        self.cfg = cfg
        self.run_dir = rundir.guard(run_dir or cfg["run_dir"]["path"])
        os.makedirs(self.run_dir, exist_ok=True)
        self.lock_fd = rundir.lock(self.run_dir)
        self.removed = rundir.prepare(self.run_dir,
                                      cfg["run_dir"]["fresh"])
        self.clock = SimClock(mono_fn) if mono_fn else SimClock()
        lc = cfg["log"]
        self.log = log_mod.RecordLog(
            os.path.join(self.run_dir, "log.jsonl"), self.clock.now,
            ring=lc["ring"], max_bytes=lc["max_log_mb"] * 1024 * 1024,
            max_files=lc["max_log_files"])
        self.state = StateTree()
        self.power = power_mod.Power(cfg, self.run_dir, self.log,
                                     self.clock, launcher=launcher,
                                     sleep=sleep)
        self.power.lock_fd = self.lock_fd
        self.power.on_line = self._qemu_line
        self.power.on_change = self._power_changed
        self.dispatcher = Dispatcher(self.log)
        self.stopping = threading.Event()
        self.poller = None
        self.last_state_poll = 0.0
        self.last_poll_error = None
        self.last_sparse = time.monotonic()
        self.last_snapshot = None   # bench time of the last snapshot
        self.server = None
        self._register()
        self.log.emit("ctl", "log", "controller.start",
                      {"run_dir": self.run_dir, "fresh_removed":
                       self.removed, "config": _public(cfg)})
        self._power_changed()

    # -- verbs --
    def _register(self):
        d = self.dispatcher
        p = self.power
        d.add("platform.power.on", p.on,
              doc="spawn QEMU frozen (-S); no instruction runs")
        d.add("platform.power.boot", p.boot, doc="QMP cont; bench T0")
        d.add("platform.power.off", p.off,
              doc="QMP quit, then SIGTERM, then SIGKILL")
        d.add("platform.power.cycle", p.cycle,
              args={"off_time_s": {"type": "number", "min": 0,
                                   "max": 3600},
                    "boot": {"type": "boolean"}},
              doc="off, the off-time on the wall clock, on, boot")
        d.add("sim.pause", p.pause, doc="QMP stop: all time freezes")
        d.add("sim.resume", p.resume, doc="QMP cont")
        d.add("log.mark", self._mark,
              args={"text": {"type": "string", "required": True},
                    "data": {"type": "object"}},
              doc="a marker record in the log")
        d.add("debug.qmp", self._debug_qmp,
              args={"command": {"type": "string", "required": True,
                                "enum": list(DEBUG_QMP)},
                    "arguments": {"type": "object"}},
              doc="a raw QMP command (dev and gates only)",
              enabled=lambda: None if self.cfg["debug"]["raw_qmp"]
              else "debug.qmp needs [debug] raw_qmp = true")

    def command(self, verb, args, source="api"):
        res = self.dispatcher.run(verb, args, source=source)
        self.poll_state()      # a state record right after every command
        return res

    def _mark(self, text, data=None):
        rec = self.log.emit("api", "log", "log.mark",
                            {"text": text, "data": data or {}})
        return {"seq": rec.seq}

    def _debug_qmp(self, command, arguments=None):
        q = self.power.qmp
        if q is None:
            raise VerbError(409, "power is off")
        try:
            return {"return": q.execute(command, arguments)}
        except QmpError as e:
            raise VerbError(400, str(e))

    # -- QEMU output --
    def _qemu_line(self, kind, text):
        if kind == "uart":
            self.log.emit("fsw", "uart", "uart", {"line": text})
        elif kind == "trace":
            m = TRACE_NAME_RE.match(text)
            self.log.emit("qemu", "log", "trace." + (m.group(1) if m
                                                     else "?"),
                          {"line": text})
        else:
            self.log.emit("qemu", "log", "qemu.stderr", {"line": text})

    def _power_changed(self):
        changes = self.state.set_platform(
            {"platform.power": self.power.state,
             "platform.boot_id": self.clock.boot_id},
            self.clock.now()[0])
        if self.power.state == power_mod.OFF:
            self.state.clear_hw()
        self._emit_changes(changes)

    def _emit_changes(self, changes):
        for path, (old, new) in sorted(changes.items()):
            self.log.emit("ctl", "state", path, {"old": old, "new": new})

    # -- polling (D-PLr, D-PLo) --
    def poll_state(self):
        """Sample both hw-states; never raises (S3): a failure is one
        poller.error record (repeats of the same error are not logged)."""
        q = self.power.qmp
        if q is None or not self.power.qmp_alive():
            return
        self.last_state_poll = time.monotonic()
        try:
            ctrl = json.loads(q.qom_get(qemu_mod.STIXCTRL, "hw-state"))
            spw = json.loads(q.qom_get(qemu_mod.STIXSPW, "hw-state"))
        except (QmpError, AttributeError, OSError):
            return              # QEMU going away: power.watch() reports it
        except ValueError as e:
            return self._poll_error("state", e)
        try:
            self.clock.sample(spw["vclock_ns"])
            sim = self.clock.exact_sim()
            if spw.get("obt_kind") == "platform" and \
                    spw.get("obt") is not None:
                self.power.last_obt = (spw["obt"], sim)
            changes = self.state.update_hw(ctrl, spw, sim)
        except Exception as e:
            return self._poll_error("state", e)
        self._emit_changes(changes)
        self._maybe_snapshot(sim)
        self._poll_ok()

    def _poll_error(self, where, e):
        msg = "%s: %s: %s" % (where, type(e).__name__, e)
        if msg != self.last_poll_error:
            self.last_poll_error = msg
            self.log.emit("ctl", "log", "poller.error", {"error": msg})

    def _poll_ok(self):
        if self.last_poll_error is not None:
            self.log.emit("ctl", "log", "poller.recovered",
                          {"after": self.last_poll_error})
            self.last_poll_error = None

    def _maybe_snapshot(self, sim):
        """A full `state` record (path "snapshot") every snapshot_s of
        bench time, so the log alone can rebuild the volatile leaves
        that change records skip."""
        every = int(self.cfg["clock"]["snapshot_s"] * 1e9)
        if every <= 0 or (self.last_snapshot is not None and
                          sim - self.last_snapshot < every):
            return
        self.last_snapshot = sim
        with self.state.lock:
            leaves = {p: {"truth": lf["truth"], "presented": lf["presented"]}
                      for p, lf in sorted(self.state.leaves.items())}
        self.log.emit("ctl", "state", "snapshot", {"leaves": leaves})

    def _poll_loop(self):
        period = 1.0 / self.cfg["clock"]["sample_hz"]
        state_period = 1.0 / self.cfg["clock"]["state_hz"]
        while not self.stopping.is_set():
            time.sleep(self.clock.wake_in(period))
            try:
                self._poll_once(state_period)
            except Exception as e:      # S3: the poller never dies
                self._poll_error("loop", e)
                time.sleep(period)

    def _poll_once(self, state_period):
        self.power.watch()
        every = self.cfg["power"]["sparse_check_s"]
        if every > 0 and time.monotonic() - self.last_sparse >= every:
            self.last_sparse = time.monotonic()
            self.power.sparse_poll()
        q = self.power.qmp
        if q is None or not self.power.qmp_alive():
            return
        try:
            self.clock.sample(q.qom_get(qemu_mod.STIXSPW, "vclock-ns"))
        except (QmpError, AttributeError, OSError):
            return
        self.clock.run_due()
        if self.power.state != power_mod.OFF and \
                time.monotonic() - self.last_state_poll >= state_period:
            self.poll_state()

    # -- read endpoints --
    def time_info(self):
        sim, boot_id, vclock, est = self.clock.now()
        obt = self.state.get("platform.obt")
        return {"sim_time": sim, "sim_est": est, "boot_id": boot_id,
                "vclock_ns": vclock, "wall_time": log_mod.wall_now(),
                "power": self.power.state,
                "running": self.power.state == power_mod.RUNNING,
                "obt": obt["truth"] if obt else None,
                "obt_kind": (self.state.get("platform.obt_kind") or
                             {}).get("truth"),
                "t0": self.power.t0}

    def status(self):
        q = self.power.qmp
        return {"api": 1, "mode": self.cfg["mode"],
                "power": self.power.state, "pid": self.power.pid
                if self.power.state != power_mod.OFF else None,
                "boot_id": self.clock.boot_id, "run_dir": self.run_dir,
                "time": self.time_info(), "seq": self.log.seq,
                "timecode_source": self.cfg["qemu"]["timecode_source"],
                "on_guest_reset": self.cfg["qemu"]["on_guest_reset"],
                "qmp_rtt_s": None if q is None else
                {"min": q.rtt_min, "max": q.rtt_max, "last": q.rtt_last},
                "faults": [], "forcings": [], "scenario": None}

    # -- lifecycle --
    def start(self, serve=True):
        self.poller = threading.Thread(target=self._poll_loop,
                                       name="poller", daemon=True)
        self.poller.start()
        if serve:
            from .api import make_server
            self.server = make_server(self, self.cfg["api"]["bind"],
                                      self.cfg["ports"]["api"])
            self.log.emit("ctl", "log", "api.listen",
                          {"bind": self.cfg["api"]["bind"],
                           "port": self.server.server_address[1]})

    def shutdown(self, why="shutdown"):
        """Power off QEMU cleanly (if on) and close the log."""
        self.log.emit("ctl", "log", "controller.stop", {"why": why})
        if self.power.state != power_mod.OFF:
            try:
                self.power.off()
            except VerbError:
                pass
        self.stopping.set()
        if self.server is not None:
            self.server.shutdown()
            self.server.server_close()
        if self.poller is not None:
            self.poller.join(timeout=2)
        self.log.close()
        if self.lock_fd is not None:
            os.close(self.lock_fd)
            self.lock_fd = None


def _public(cfg):
    return {k: v for k, v in cfg.items() if not k.startswith("_")}
