# SPDX-License-Identifier: GPL-2.0-or-later
"""The power model (platform plan D-PLf).

on    = spawn QEMU frozen (-S): realized and reset, no instruction run;
boot  = QMP cont (its bench time is T0 for scenarios);
off   = QMP quit, then SIGTERM, then SIGKILL; the flash stays;
cycle = off, the off-time (wall clock), on, and boot if cycle_boots.

Guest resets follow `on_guest_reset`: warm (in-process, RESET event),
cold (SHUTDOWN + exit, then a cold restart: not hardware-like) or halt
(the process stays paused for inspection).

QMP events arrive on the QMP reader thread; they are handed to an
event worker, because handling one may issue QMP commands, which the
reader thread itself must never wait for.
"""

import json
import os
import queue
import threading
import time

from . import qemu as qemu_mod
from . import rundir
from .dispatch import VerbError
from .qmp import QmpConnection, QmpError

OFF, FROZEN, RUNNING, PAUSED, HALTED = ("off", "frozen", "running",
                                        "paused", "halted")
CUC_PER_S = 65536


class Power:
    def __init__(self, cfg, run_dir, log, clock, launcher=None,
                 sleep=time.sleep):
        self.cfg = cfg
        self.run_dir = run_dir
        self.log = log
        self.clock = clock
        self.launcher = launcher or qemu_mod.QemuProcess
        self.sleep = sleep
        self.lock = threading.RLock()
        self.state = OFF
        self.proc = None
        self.qmp = None
        self.pid = None
        self.preflight_done = False
        self.stopping = False
        self.hooks = []            # on_power(event, power) (D-PLk)
        self.on_line = None        # (kind, text) from the QEMU readers
        self.on_change = None      # () after any power state change
        self.last_obt = None       # (obt, bench time of the sample)
        self.t0 = None             # bench time of the last boot
        self.events = queue.Queue()
        self.worker = threading.Thread(target=self._event_loop,
                                       name="power-events", daemon=True)
        self.worker.start()
        self.gen = 0               # the process generation
        self.lock_fd = None        # the run-dir lock, inherited by QEMU

    # -- helpers ------------------------------------------------------------
    def _record(self, event, **data):
        data["event"] = event
        data["state"] = self.state
        self.log.emit("ctl", "power", "platform.power", data)

    def _set(self, state):
        self.state = state
        if self.on_change is not None:
            self.on_change()

    def _need(self, *states):
        if self.state not in states:
            raise VerbError(409, "power is %s (needs %s)"
                            % (self.state, " or ".join(states)))

    def qmp_alive(self):
        q = self.qmp
        return q is not None and not q.closed.is_set()

    # -- on / boot / off / cycle -------------------------------------------
    def on(self):
        with self.lock:
            self._need(OFF)
            q = self.cfg["qemu"]
            if not self.preflight_done:
                try:
                    rundir.preflight(self.run_dir)
                except rundir.RunDirError as e:
                    raise VerbError(409, str(e))
                self.preflight_done = True
            try:
                detail, cached = qemu_mod.check_binary(q["binary"],
                                                       self.run_dir)
            except qemu_mod.BinaryError as e:
                self.log.emit("ctl", "log", "platform.power.guard",
                              {"ok": False, "error": str(e)})
                raise VerbError(409, str(e))
            self.log.emit("ctl", "log", "platform.power.guard",
                          {"ok": True, "detail": detail, "cached": cached,
                           "binary": q["binary"]})
            sock = os.path.join(self.run_dir, "qmp.sock")
            if os.path.exists(sock):
                os.unlink(sock)
            argv = qemu_mod.build_argv(self.cfg, self.run_dir)
            self.gen += 1
            gen = self.gen
            self.proc = self.launcher(argv, self.run_dir, self._line,
                                      keep_fds=() if self.lock_fd is None
                                      else (self.lock_fd,))
            self.pid = self.proc.spawn()
            try:
                return self._on_spawned(gen, sock, argv)
            except BaseException:
                # whatever failed, the child must not outlive the call
                if self.proc is not None:
                    self.stopping = True
                    self._reap("power on failed")
                raise

    def _on_spawned(self, gen, sock, argv):
        """The rest of on(); any exception reaps the child."""
        self.stopping = False
        self.qmp = QmpConnection(sock, timeout=self.cfg["power"]
                                 ["qmp_timeout_s"],
                                 on_event=lambda m, g=gen:
                                 self.events.put((g, m)))
        try:
            self.qmp.connect(retry_s=10.0)
        except QmpError as e:
            raise VerbError(500, "QEMU QMP: %s" % e)
        try:
            os.chmod(sock, 0o600)
        except OSError:
            pass
        self.clock.process_started()
        self._set(FROZEN)
        self._record("on", pid=self.pid, argv=argv)
        try:
            used = rundir.sparse_check(self.run_dir,
                                       limit=self.sparse_limit())
        except rundir.RunDirError as e:
            self._off_locked("sparse check")
            raise VerbError(500, str(e)) from None
        self.log.emit("ctl", "log", "platform.power.sparse",
                      {"flash_blocks_bytes": used})
        self._obt_continuity()
        for h in self.hooks:
            h("spawned", self)
        return {"pid": self.pid, "boot_id": self.clock.boot_id}

    def sparse_limit(self):
        return self.cfg["power"]["sparse_limit_mb"] << 20

    def sparse_poll(self):
        """The periodic sparse check (poller): over the limit, power off
        and say why."""
        with self.lock:
            if self.state == OFF:
                return
            try:
                rundir.sparse_check(self.run_dir, when="while on",
                                    limit=self.sparse_limit())
            except rundir.RunDirError as e:
                self.log.emit("ctl", "log", "platform.power.sparse",
                              {"ok": False, "error": str(e)})
                self._off_locked("sparse check")

    def boot(self):
        with self.lock:
            self._need(FROZEN)
            self.qmp.execute("cont")
            self.clock.resumed()
            self.t0 = self.clock.exact_sim()
            self._set(RUNNING)
            self._record("boot", t0=self.t0)
            return {"t0": self.t0, "boot_id": self.clock.boot_id}

    def off(self):
        with self.lock:
            self._need(FROZEN, RUNNING, PAUSED, HALTED)
            return self._off_locked("power off")

    def _off_locked(self, why):
        self.stopping = True
        self._sample_final()
        method = "quit"
        try:
            self.qmp.execute("quit", timeout=2.0)
        except QmpError:
            pass
        rc = self.proc.wait(self.cfg["power"]["quit_timeout_s"])
        if rc is None:
            method = "sigterm"
            self.proc.terminate()
            rc = self.proc.wait(5.0)
        if rc is None:
            method = "sigkill"
            self.proc.kill()
            rc = self.proc.wait(5.0)
        self._reap(why, method=method, rc=rc)
        return {"method": method, "rc": rc}

    def _sample_final(self):
        """Before quit: the last vclock and OBT (D-PLf OBT continuity)."""
        try:
            v = self.qmp.qom_get(qemu_mod.STIXSPW, "vclock-ns", timeout=2)
            self.clock.sample(v)
            obt = self.qmp.qom_get(qemu_mod.STIXSPW, "obt", timeout=2)
            self.last_obt = (obt, self.clock.exact_sim())
        except QmpError:
            pass

    def _reap(self, why, method=None, rc=None):
        if self.qmp is not None:
            self.qmp.close()
        if self.proc is not None:
            if rc is None:
                rc = self.proc.wait(0.5)
            if rc is None:
                # never return with a live child: terminate, then kill
                method = (method + "+" if method else "") + "sigterm"
                self.proc.terminate()
                rc = self.proc.wait(self.cfg["power"]["kill_grace_s"])
            if rc is None:
                method += "+sigkill"
                self.proc.kill()
                rc = self.proc.wait(5.0)
            self.proc.finish()
        self.qmp = None
        self.proc = None
        self.clock.process_ended()
        self._set(OFF)
        self._record("off", why=why, method=method, rc=rc)

    def cycle(self, off_time_s=None, boot=None):
        off_time_s = self.cfg["power"]["off_time_s"] if off_time_s is None \
            else off_time_s
        boot = self.cfg["power"]["cycle_boots"] if boot is None else boot
        with self.lock:
            if self.state != OFF:
                self.off()
            self._record("cycle_off_time", off_time_s=off_time_s)
            self.sleep(off_time_s)
            self.on()
            if boot:
                self.boot()
            return {"boot_id": self.clock.boot_id, "booted": boot}

    def pause(self):
        with self.lock:
            self._need(RUNNING)
            self.qmp.execute("stop")
            self.clock.stopped()
            self._set(PAUSED)
            return {"sim_time": self.clock.exact_sim()}

    def resume(self):
        with self.lock:
            self._need(PAUSED)
            self.qmp.execute("cont")
            self.clock.resumed()
            self._set(RUNNING)
            return {"sim_time": self.clock.exact_sim()}

    # -- OBT continuity (D-PLf, D-PLb) --------------------------------------
    def _obt_continuity(self):
        if self.cfg["qemu"]["timecode_source"] != "obt" or \
                self.last_obt is None:
            return
        obt, at_sim = self.last_obt
        now = self.clock.exact_sim()
        cont = obt + (now - at_sim) * CUC_PER_S // 1_000_000_000
        new = cont
        mode = self.cfg["qemu"]["obt_continuity"]
        if mode == "aligned":
            ctrl = self.qmp.qom_get(qemu_mod.STIXCTRL, "hw-state")
            scet = json.loads(ctrl)["scet"]
            coarse = (cont >> 16) & ~63 | (scet["coarse"] & 63)
            new = (coarse << 16) | scet["fine"]
            if new < cont:
                new += 64 << 16
        self.qmp.qom_set(qemu_mod.STIXSPW, "obt", new)
        self.log.emit("ctl", "log", "platform.obt",
                      {"why": "continuity", "mode": mode, "last": obt,
                       "continued": cont, "set": new,
                       "jump_cuc": new - cont})

    # -- QEMU readers and QMP events ----------------------------------------
    def _line(self, kind, text):
        if self.on_line is not None:
            self.on_line(kind, text)

    def _event_loop(self):
        while True:
            gen, msg = self.events.get()
            try:
                self._event(gen, msg)
            except Exception as e:  # keep the worker alive, but say so
                self.log.emit("ctl", "log", "platform.power.event_error",
                              {"error": "%s: %s" % (type(e).__name__, e),
                               "event": msg.get("event")})

    def _event(self, gen, msg):
        ev = msg.get("event")
        data = msg.get("data", {})
        self.log.emit("qemu", "log", "qmp.event." + ev.lower(),
                      {"data": data})
        with self.lock:
            if gen != self.gen or self.proc is None:
                return
            policy = self.cfg["qemu"]["on_guest_reset"]
            if ev == "STOP":
                self.clock.stopped()
                if self.state == RUNNING:
                    self._set(PAUSED)
            elif ev == "RESUME":
                self.clock.resumed()
                if self.state in (PAUSED, FROZEN):
                    self._set(RUNNING)
            elif ev == "RESET":
                self.clock.guest_reset()
                self._record("guest_reset", policy="warm" if
                             data.get("guest") else "host",
                             reason=data.get("reason"),
                             boot_id=self.clock.boot_id)
                for h in self.hooks:
                    h("guest_reset", self)
            elif ev == "SHUTDOWN" and not self.stopping:
                reason = data.get("reason")
                if reason == "guest-reset" and policy == "halt":
                    self._set(HALTED)
                    self._record("guest_reset", policy="halt",
                                 reason=reason, note="halted for "
                                 "inspection")
                elif reason == "guest-reset" and policy == "cold":
                    self.stopping = True
                    rc = self.proc.wait(5.0)
                    if rc is None:
                        self.proc.kill()
                        rc = self.proc.wait(5.0)
                    self._reap("guest reset", method="exit", rc=rc)
                    self._record("guest_reset", policy="cold",
                                 reason=reason,
                                 note="cold restart (not hardware-like)")
                    self.on()
                    self.boot()
                else:
                    self.stopping = True
                    rc = self.proc.wait(5.0)
                    self._reap("QEMU shut down (%s)" % reason, rc=rc)

    def watch(self):
        """Called periodically: notice a process that died on its own."""
        with self.lock:
            if self.proc is not None and not self.stopping and \
                    not self.proc.alive():
                self.stopping = True
                self._reap("QEMU exited unexpectedly",
                           rc=self.proc.wait(0.1))
