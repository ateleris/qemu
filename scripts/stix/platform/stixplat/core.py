# SPDX-License-Identifier: GPL-2.0-or-later
"""The controller core: the record log, the bench clock, the power
model, the state tree, the dispatcher and the poller, wired together
(platform plan PL-3).
"""

import hashlib
import json
import os
import re
import threading
import time

from . import bundle as bundle_mod
from . import log as log_mod
from . import power as power_mod
from . import qemu as qemu_mod
from . import rundir
from . import scenario as scenario_mod
from . import series as series_mod
from . import timeline as timeline_mod
from . import views as views_mod
from .dispatch import Dispatcher, VerbError
from .qmp import QmpError
from .simclock import SimClock
from .state import StateTree

TRACE_NAME_RE = re.compile(r"^(?:\d+@\d+\.\d+:)?(\w+) ")
DEBUG_QMP = ("qom-get", "qom-set", "query-status")
# D-PSd: the five fixed paths debug.qmp may touch (exact match, no
# children)
DEBUG_PATHS = ("/machine/stixspw", "/machine/stixctrl",
               "/machine/stixctrl/attenuator", "/machine/stixctrl/edac",
               "/machine/stixflash")


class Controller:
    def __init__(self, cfg, run_dir=None, launcher=None, mono_fn=None,
                 sleep=time.sleep, scenario=None, bundle_kind=None,
                 bundle_extra=None):
        self.cfg = cfg
        self.elf_sha256 = self._elf_sha256()
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
        self.power.on_snapshot = self._event_snapshot
        self.dispatcher = Dispatcher(self.log)
        self.stopping = threading.Event()
        self.poller = None
        self.last_state_poll = 0.0
        self.last_poll_error = None
        self._poll_errored = False   # an error was seen in this poll
        self.debug_used = False
        self.snap_lock = threading.Lock()
        self.last_sparse = time.monotonic()
        self.last_snapshot = None   # bench time of the last snapshot
        self.server = None
        self.runner = None
        self.scn_thread = None      # only for scenario.run (interactive)
        self.scn_lock = threading.Lock()
        self.viewers = bundle_mod.Viewers()   # run.open/run.close (S1)
        self.bundle = None          # the run-bundle writer (S1)
        self._register()
        from .plat import Platform
        self.trace_filters = []     # fn(name, line) -> True drops it
        self.plat = Platform(self)
        self.plat.register(self.dispatcher)
        self.state.fault_ids = self.plat.faults.active_targets
        self.params = self.plat.params   # GET /params (fsw-params plan)
        # A4: the series index, fed by a record-log listener (called
        # after the ring append, outside the log lock)
        self.series = series_mod.SeriesIndex(idb=self.plat.relay.idb)
        self.log.listeners.append(self.series.on_record)
        # A1: the truth sampler; -1 means by mode (1 s interactive, off
        # in run mode unless the scenario or config sets it)
        tl = cfg["timeline"]
        ss = tl["sample_s"]
        if ss < 0:
            ss = 1.0 if cfg["mode"] == "interactive" else 0.0
        self.sampler = (timeline_mod.Sampler(self.clock, self.state,
                                             self.log, tl["sample_paths"],
                                             ss)
                        if ss > 0 else None)
        if self.sampler is not None:
            self.sampler.start()
            self.power.hooks.append(self._sampler_power)
        views_mod.ensure_starters(cfg)      # A5: only when missing
        start_rec = self.log.emit(
            "ctl", "log", "controller.start",
            {"run_dir": self.run_dir, "fresh_removed": self.removed,
             "config": _public(cfg),
             # elf_sha256 rides next to the config so --view-run can
             # replay the FSW parameters table from the same ELF
             "elf_sha256": self.elf_sha256})
        # S1: the bundle writer owns manifest.json + decisions.jsonl in
        # the run dir; its start_seq is this record's (S2 slices the log
        # from it). Created before anything else can emit a record the
        # writer would miss.
        if cfg["bundle"]["enabled"]:
            # S2: the replay passes bundle_kind/bundle_extra for its
            # manifest (kind "replay", of/elf/qemu-binary)
            self.bundle = bundle_mod.Writer(
                cfg, self.run_dir, self.log, cfg["mode"],
                scenario=scenario, start_seq=start_rec.seq,
                kind=bundle_kind or "run", extra=bundle_extra)
            self.log.listeners.append(self.bundle.on_record)
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
        d.add("scenario.run", self._scenario_run,
              args={"file": {"type": "string", "required": True},
                    "variant": {"type": "string"}},
              doc="run a scenario from the scenario dir (one at a time)")
        d.add("scenario.abort", self._scenario_abort,
              doc="abort the running scenario (its teardown still runs)")
        d.add("view.save", self._view_save,
              args={"name": {"type": "string", "required": True},
                    "view": {"type": "object", "required": True}},
              doc="save a timeline view (name, window, markers, panels) "
                  "as <name>.json in the views dir; 409 for the "
                  "generated \"everything\"")
        d.add("view.delete", self._view_delete,
              args={"name": {"type": "string", "required": True}},
              doc="delete a saved timeline view (409 for \"everything\")")
        d.add("run.baseline", self._run_baseline,
              args={"name": {"type": "string", "required": True}},
              doc="mark that run bundle (its run dir) as the comparison "
                  "baseline; the previous holder's mark moves")
        d.add("run.baseline.clear", self._run_baseline_clear,
              doc="remove the baseline mark")
        d.add("run.open", self._run_open,
              args={"name": {"type": "string", "required": True},
                    "port": {"type": "integer", "min": 1024, "max": 65535}},
              doc="serve a finished run bundle read-only (the --view-run "
                  "pair) on 127.0.0.1; one slot, 409 while one is open; "
                  "the answer is {url, port}")
        d.add("run.close", self._run_close,
              args={"port": {"type": "integer", "min": 1024, "max": 65535}},
              doc="close the open run viewer (409 when none)")
        d.add("run.compare", self._run_compare,
              args={"a": {"type": "string", "required": True},
                    "b": {"type": "string", "required": True}},
              doc="compare two finished run bundles by run id (R8): the "
                  "decisions aligned at the first common (kind, key) "
                  "plus the derived rows; the answer is {summary, rows, "
                  "artifact} with the artifact under "
                  "<retention.run_root>/compares (404 unknown, 409 a "
                  "bundle still open)")
        d.add("debug.qmp", self._debug_qmp,
              args={"command": {"type": "string", "required": True,
                                "enum": list(DEBUG_QMP)},
                    "arguments": {"type": "object"}},
              doc="a raw QMP command (dev and gates only)",
              enabled=self._debug_enabled)

    # -- scenarios (PL-5) --
    def scenario_dir(self):
        return os.path.realpath(self.cfg["run"]["scenario_dir"] or
                                scenario_mod.DEFAULT_DIR)

    def _scenario_run(self, file, variant=None):
        base = self.scenario_dir()
        path = os.path.realpath(os.path.join(base, file))
        if os.path.commonpath([base, path]) != base:
            raise VerbError(403, "scenarios are read from %s only" % base)
        if not os.path.isfile(path):
            raise VerbError(404, "no scenario %s" % file)
        try:
            sc = scenario_mod.load(path)
            runner = scenario_mod.Runner(sc, variant,
                                         scenario_mod.CtlCtx(self))
        except scenario_mod.ScenarioError as e:
            raise VerbError(400, str(e))
        with self.scn_lock:
            if self.runner is not None and \
                    self.runner.state["phase"] != "done":
                raise VerbError(409, "scenario %s is running"
                                % self.runner.state["name"])
            self.runner = runner
        self.scn_thread = threading.Thread(
            target=self._scenario_thread, args=(runner,), name="scenario",
            daemon=True)
        self.scn_thread.start()
        return {"started": True, "scenario": sc["name"],
                "variant": runner.variant}

    def _scenario_thread(self, runner):
        try:
            runner.result = runner.run()
        except Exception as e:      # never a silent pass
            runner.state.update(phase="done", verdict="error")
            self.log.emit("scn", "log", "scenario.error",
                          {"error": "%s: %s" % (type(e).__name__, e)})

    def _scenario_abort(self):
        with self.scn_lock:
            r = self.runner
        if r is None or r.state["phase"] == "done":
            raise VerbError(409, "no scenario is running")
        r.abort("scenario.abort")
        return {"aborting": r.state["name"]}

    # -- views (timeline plan A5) --
    def _view_save(self, name, view):
        return views_mod.save(self.cfg, name, view)

    def _view_delete(self, name):
        views_mod.delete(self.cfg, name)
        return {"deleted": name}

    # -- run bundles (S1) --
    def _run_baseline(self, name):
        return bundle_mod.baseline_set(self.cfg, name,
                                       live_dir=self.run_dir)

    def _run_baseline_clear(self):
        return bundle_mod.baseline_clear(self.cfg, live_dir=self.run_dir)

    def _run_open(self, name, port=None):
        return self.viewers.open(self, name, port)

    def _run_close(self, port=None):
        return self.viewers.close(port)

    def _run_compare(self, a, b):
        from . import compare as compare_mod   # lazy: compare -> replay
        return compare_mod.run_compare(self.cfg, a, b,
                                       live_dir=self.run_dir, out=None)

    def command(self, verb, args, source="api"):
        res = self.dispatcher.run(verb, args, source=source)
        self.poll_state()      # a state record right after every command
        return res

    def _mark(self, text, data=None):
        rec = self.log.emit("api", "log", "log.mark",
                            {"text": text, "data": data or {}})
        return {"seq": rec.seq}

    def _debug_enabled(self):
        dbg = self.cfg["debug"]
        if not dbg["raw_qmp"]:
            return "debug.qmp needs [debug] raw_qmp = true"
        if self.cfg["mode"] == "run" and not dbg["allow_in_run"]:
            return ("debug.qmp is refused in run mode unless the scenario "
                    "or gate sets [debug] allow_in_run = true")
        return None

    def _debug_qmp(self, command, arguments=None):
        arguments = arguments or {}
        if command in ("qom-get", "qom-set"):
            path = arguments.get("path")
            if path not in DEBUG_PATHS:
                raise VerbError(403, "debug.qmp path %r is not one of %s"
                                % (path, ", ".join(DEBUG_PATHS)))
        q = self.power.qmp
        if q is None:
            raise VerbError(409, "power is off")
        self.debug_used = True          # sticky: reports print it
        try:
            ret = q.execute(command, arguments or None)
        except QmpError as e:
            self.log.emit("api", "debug", "debug.qmp",
                          {"command": command, "arguments": arguments,
                           "error": str(e)})
            raise VerbError(400, str(e))
        self.log.emit("api", "debug", "debug.qmp",
                      {"command": command, "arguments": arguments,
                       "return": ret})
        return {"return": ret}

    # -- QEMU output --
    def _qemu_line(self, kind, text):
        if kind == "uart":
            self.log.emit("fsw", "uart", "uart", {"line": text})
        elif kind == "trace":
            m = TRACE_NAME_RE.match(text)
            name = m.group(1) if m else "?"
            for f in self.trace_filters:
                if f(name, text):
                    return
            self.log.emit("qemu", "log", "trace." + name, {"line": text})
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

    def state_platform(self, values):
        self._emit_changes(self.state.set_platform(values,
                                                   self.clock.now()[0]))

    def _emit_changes(self, changes):
        for path, (old, new) in sorted(changes.items()):
            self.log.emit("ctl", "state", path, {"old": old, "new": new})

    def _sampler_power(self, event, power):
        """A1: bench-time ticks never fire while off or paused, so the
        sampler only restarts at a boot or a guest reset."""
        if event in ("boot", "guest_reset"):
            self.sampler.on_boot()
        elif event == "off":
            self.sampler.on_off()

    # -- polling (D-PLr, D-PLo) --
    def poll_state(self, snapshot=True):
        """Sample both hw-states and the flash device's watch props;
        never raises (S3): a failure is one poller.error record
        (repeats of the same error are not logged)."""
        q = self.power.qmp
        if q is None or not self.power.qmp_alive():
            return
        self.last_state_poll = time.monotonic()
        self._poll_errored = False
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
        # PL-11d (R4): the flash watch props in their own block, after
        # the hw-state update: one bad get is a "flash" poller.error and
        # never skips or undoes the hw-state leaves above
        flash = {}
        flash_err = None
        for prop in ("ops-read", "ops-prog", "ops-erase", "ops-abort",
                     "damage-list"):
            try:
                flash[prop] = q.qom_get(qemu_mod.STIXFLASH, prop)
            except (QmpError, ValueError, KeyError) as e:
                flash_err = flash_err or e
        if flash_err is not None:
            # only the first one per poll: five different messages
            # would defeat the repeat check and log every poll
            self._poll_error("flash", flash_err)
        if flash:
            try:
                self._emit_changes(self.state.update_flash(flash, sim))
            except Exception as e:
                self._poll_error("flash", e)
        self.plat._platform_state()
        if snapshot:
            self._maybe_snapshot(sim)
        self._poll_ok()

    def _poll_error(self, where, e):
        self._poll_errored = True   # no poller.recovered in this poll
        msg = "%s: %s: %s" % (where, type(e).__name__, e)
        if msg != self.last_poll_error:
            self.last_poll_error = msg
            self.log.emit("ctl", "log", "poller.error", {"error": msg})

    def _poll_ok(self):
        # recovered only when the last error did not recur this poll
        if self.last_poll_error is not None and not self._poll_errored:
            self.log.emit("ctl", "log", "poller.recovered",
                          {"after": self.last_poll_error})
            self.last_poll_error = None
        self._poll_errored = False

    def _maybe_snapshot(self, sim):
        """A periodic snapshot every snapshot_s of bench time, so the log
        alone can rebuild the volatile leaves that change records skip.
        The poller and commands both poll: one lock makes the decision,
        so an interval gets one snapshot."""
        every = int(self.cfg["clock"]["snapshot_s"] * 1e9)
        if every <= 0:
            return
        with self.snap_lock:
            if self.last_snapshot is not None and \
                    sim - self.last_snapshot < every:
                return
            self.last_snapshot = sim
            self._emit_snapshot("periodic")

    def _event_snapshot(self, reason):
        """At boot, at pause and just before power-off (Power calls it,
        with the QMP connection still up)."""
        self.poll_state(snapshot=False)
        with self.snap_lock:
            self.last_snapshot = self.clock.exact_sim()
            self._emit_snapshot(reason)

    def _emit_snapshot(self, reason):
        with self.state.lock:
            leaves = {p: {"truth": lf["truth"], "presented": lf["presented"]}
                      for p, lf in sorted(self.state.leaves.items())}
        # kind state, path = the state root (""), data.snapshot = true
        self.log.emit("ctl", "state", "", {"snapshot": True,
                                           "reason": reason,
                                           "leaves": leaves})

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
    def _elf_sha256(self):
        """The FSW ELF's digest, computed once at start (the Setup tab);
        None if the file is unreadable."""
        h = hashlib.sha256()
        try:
            with open(self.cfg["qemu"]["elf"], "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    h.update(chunk)
        except OSError:
            return None
        return h.hexdigest()

    def setup(self):
        """The launch configuration, read-only (a change is a restart);
        status()["setup"] (timeline plan B2, additive)."""
        q = self.cfg["qemu"]
        # [timeline] sample_s comes with Track A; None until then
        return {"binary": q["binary"], "elf": q["elf"],
                "elf_sha256": self.elf_sha256,
                "icount_shift": q["icount_shift"], "pace": q["pace"],
                "memory": q["memory"],
                "timecode_source": q["timecode_source"],
                "obt_continuity": q["obt_continuity"],
                "on_guest_reset": q["on_guest_reset"],
                "sample_s": self.cfg.get("timeline", {}).get("sample_s")}

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
                "debug_used": self.debug_used,
                "platform": self.plat.status(),
                "setup": self.setup(),
                "faults": self.plat.faults.status(),
                "forcings": self.plat.forcing.status(),
                "scenario": None if self.runner is None else
                dict(self.runner.state)}

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
        r = self.runner
        if r is not None and r.state["phase"] != "done":
            r.abort("controller shutdown")
            if self.scn_thread is not None:
                self.scn_thread.join(timeout=30)
        if self.power.state != power_mod.OFF:
            try:
                self.power.off()
            except Exception as e:      # the close below must still run
                self.log.emit("ctl", "log", "controller.stop.power",
                              {"error": "%s: %s" % (type(e).__name__, e)})
        self.stopping.set()
        if self.server is not None:
            self.server.shutdown()
            self.server.server_close()
        if self.poller is not None:
            self.poller.join(timeout=2)
        if self.sampler is not None:
            self.sampler.stop()
        self.plat.close()
        if self.bundle is not None:
            # before log.close(): the post-controller.stop power-off and
            # plat.close decisions still land in decisions.jsonl
            self.bundle.close()
        self.log.close()
        if self.lock_fd is not None:
            os.close(self.lock_fd)
            self.lock_fd = None


def _public(cfg):
    return {k: v for k, v in cfg.items() if not k.startswith("_")}
