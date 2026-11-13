# SPDX-License-Identifier: GPL-2.0-or-later
"""The platform module (platform plan PL-4, D-PLk): the SpW link, the
time codes, the OBT, TC(9,129), TC(20,128), forcing, faults, and the
TM relay + TC proxy, behind the platform.*, force.*, fault.* and tc.*
verbs.

What a device reset clears (link-down, adc-inject) is re-applied after
a warm guest reset and before `cont` after a cold power-on (D-PLf); so
are the active device-state faults (PL-11a, D-PSa).
"""

import json
import threading

from . import faults as faults_mod
from . import tcbuild
from .dispatch import VerbError
from .force import Forcing
from .keepalive import KeepAlive
from .qmp import QmpError
from .relay import Relay
from .timesync import TimeSync

STIXSPW = "/machine/stixspw"


class Platform:
    def __init__(self, ctl):
        self.ctl = ctl
        cfg = ctl.cfg
        self.cfg = cfg
        self.log = ctl.log
        self.clock = ctl.clock
        self.power = ctl.power
        self.lock = threading.Lock()
        self.link_down = False          # the controller's truth
        self.timecodes = None           # None = QEMU's default
        self.seq14 = 0                  # source 14 on APID 0x5AC
        self.seq_gs = 0                 # GS emulation (source 121)
        self.relay = Relay(cfg, ctl.run_dir, self.log, self.clock,
                           self.gs_link_ok)
        self.keepalive = KeepAlive(cfg, self.clock, self.log,
                                   self.relay.send_unit, self.can_send)
        self.timesync = TimeSync(self.clock, self.log, self.read_obt,
                                 self.relay.send_unit, self.can_send,
                                 self._next_seq14,
                                 autostart=cfg["timesync"]["autostart"])
        self.forcing = Forcing(self.log, self.qmp)
        self.faults = faults_mod.Faults(
            faults_mod.load(cfg["faults"]["catalogue"] or None),
            self.log, self.clock, self._fault_actions())
        self.relay.listeners += [self.keepalive.on_tm, self.timesync.on_tm]
        ctl.trace_filters.append(self.keepalive.trace_filter)
        self.power.hooks.append(self._on_power)

    # -- helpers --------------------------------------------------------------
    def qmp(self):
        q = self.power.qmp
        return q if q is not None and self.power.qmp_alive() else None

    def _q(self):
        q = self.qmp()
        if q is None:
            raise VerbError(409, "STIX is off")
        return q

    def _qom_set(self, prop, value):
        try:
            self._q().qom_set(STIXSPW, prop, value)
        except QmpError as e:
            raise VerbError(409, "%s: %s" % (prop, e))

    def _next_seq14(self):
        with self.lock:
            s = self.seq14
            self.seq14 = (self.seq14 + 1) & 0x3FFF
            return s

    def can_send(self):
        """None, or why a platform TC cannot go out now."""
        if self.power.state != "running":
            return "STIX is %s" % self.power.state
        if self.link_down:
            return "the SpW link is down"
        return None

    def gs_link_ok(self):
        if self.power.state not in ("running", "paused"):
            return "STIX is %s" % self.power.state
        if self.link_down:
            return "the SpW link is down"
        return None

    def read_obt(self, cached=False):
        """(OBT, bench time of the reading); one hw-state read gives the
        OBT and the vclock atomically."""
        if cached and self.power.last_obt is not None:
            return self.power.last_obt
        hw = json.loads(self._q().qom_get(STIXSPW, "hw-state"))
        self.clock.sample(hw["vclock_ns"])
        at = self.clock.exact_sim()
        if hw.get("obt") is None:
            raise VerbError(409, "no OBT (time codes have no source)")
        return hw["obt"], at

    def _platform_state(self):
        """The controller-owned platform leaves; also refreshed by every
        state poll, so scenarios can await the counters (PL-5)."""
        ka = self.keepalive.status()
        last = self.timesync.status()["last"]
        self.ctl.state_platform({
            "platform.spw.link": "down" if self.link_down else "up",
            "platform.keepalive.enabled": ka["enabled"],
            "platform.keepalive.period_s": ka["period_s"],
            "platform.keepalive.sent": ka["sent"],
            "platform.keepalive.suppressed": ka["suppressed"],
            "platform.timesync.verdict": last["verdict"] if last else
            "none"})

    # -- power hooks (D-PLk on_power) -----------------------------------------
    def _on_power(self, event, power):
        if event == "spawned":
            # before cont: switches set now apply from the first
            # instruction
            if self.link_down:
                self._q().qom_set(STIXSPW, "link-down", True)
            if self.timecodes is not None:
                self._q().qom_set(STIXSPW, "timecodes", self.timecodes)
            self.forcing.reapply("cold power-on")
            self.faults.reapply("cold power-on")
        elif event == "guest_reset":
            # the device reset cleared link-down and adc-inject
            if self.link_down:
                self._q().qom_set(STIXSPW, "link-down", True)
            self.forcing.reapply("warm guest reset")
            self.faults.reapply("warm guest reset")
            self.keepalive.on_boot()
            self.timesync.on_boot()
        elif event == "boot":
            self.relay.drop_held("boot")
            self.keepalive.on_boot()
            self.timesync.on_boot()
        elif event == "off":
            self.relay.drop_held("off")
            self.keepalive.on_off()
            self.timesync.on_off()

    # -- verbs ----------------------------------------------------------------
    def register(self, d):
        d.add("platform.spw.link", self.v_link,
              args={"state": {"type": "string", "required": True,
                              "enum": ["up", "down"]}},
              doc="the SpW link (QEMU link-down); GS TCs while down are "
                  "dropped and logged")
        d.add("platform.timecode.enable", self.v_tc_enable,
              args={"enabled": {"type": "boolean", "required": True}},
              doc="the periodic time codes")
        d.add("platform.timecode.once", self.v_tc_once,
              doc="one correct code at the next rollover")
        d.add("platform.timecode.inject", self.v_tc_inject,
              args={"timecnt": {"type": "integer", "required": True,
                                "min": 0, "max": 63}},
              doc="one code now with this TIMECNT (a wrong-value tool)")
        d.add("platform.obt.set", self.v_obt_set,
              args={"coarse": {"type": "integer", "required": True,
                               "min": 0, "max": 0xFFFFFFFF},
                    "fine": {"type": "integer", "min": 0, "max": 0xFFFF,
                             "default": 0}},
              doc="jump the platform OBT (obt mode only)")
        d.add("platform.timesync.send", self.v_timesync,
              args={"coarse": {"type": ["string", "integer"],
                               "default": "auto"}},
              doc="TC(9,129) timed to the next code (D-PLd); 409 while "
                  "one is pending",
              locked=False)
        d.add("platform.keepalive.set", self.v_keepalive,
              args={"enabled": {"type": "boolean"},
                    "period_s": {"type": "number", "min": 0.125,
                                 "max": 3600}},
              doc="the TC(20,128) keep-alive (bench-time paced)")
        d.add("force.set", self.v_force_set,
              args={"path": {"type": "string", "required": True},
                    "value": {"type": "integer", "required": True}},
              doc="force a presented value (instrument.hk_adc.<ch>)")
        d.add("force.clear", self.v_force_clear,
              args={"path": {"type": "string", "required": True}},
              doc="clear a forcing")
        d.add("fault.inject", self.faults.inject,
              args={"id": {"type": "string", "required": True},
                    "params": {"type": "object"},
                    "trigger": {"type": "object"},
                    "seed": {"type": "integer", "min": 0}},
              doc="inject a catalogue fault (logged with its seed)")
        d.add("fault.clear", self.faults.clear,
              args={"id": {"type": "string", "required": True}},
              doc="clear an active fault")
        d.add("tc.platform.send", self.v_tc_platform,
              args={"hex": {"type": "string", "required": True}},
              doc="a raw platform TC (source 14, services 9 and 20 only)")
        d.add("tc.gs.send", self.v_tc_gs,
              args={"service": {"type": "integer", "required": True,
                                "min": 0, "max": 255},
                    "subtype": {"type": "integer", "required": True,
                                "min": 0, "max": 255},
                    "data": {"type": "string", "default": ""},
                    "ack": {"type": "integer", "min": 0, "max": 15,
                            "default": 9}},
              doc="GS emulation: an operator TC from source 121 "
                  "(allow_gs_emulation)",
              enabled=self._gs_enabled)

    def v_link(self, state):
        down = state == "down"
        if self.qmp() is not None:
            self._qom_set("link-down", down)
        with self.lock:
            self.link_down = down
        self._platform_state()
        if not down:
            self.relay.release_held()
        return {"link": state}

    def v_tc_enable(self, enabled):
        if self.qmp() is not None:
            self._qom_set("timecodes", enabled)
        self.timecodes = enabled
        return {"enabled": enabled}

    def v_tc_once(self):
        self._qom_set("timecode-once", True)
        return {}

    def v_tc_inject(self, timecnt):
        self._qom_set("timecode-inject", timecnt)
        return {"timecnt": timecnt}

    def v_obt_set(self, coarse, fine=0):
        obt = (coarse << 16) | fine
        self._qom_set("obt", obt)
        return {"obt": obt}

    def v_timesync(self, coarse="auto"):
        if isinstance(coarse, str) and coarse != "auto":
            raise VerbError(400, "coarse is \"auto\" or an integer")
        return self.timesync.send(coarse)

    def v_keepalive(self, enabled=None, period_s=None):
        r = self.keepalive.set(enabled, period_s)
        self._platform_state()
        return r

    def v_force_set(self, path, value):
        return self.forcing.set(path, value)

    def v_force_clear(self, path):
        return self.forcing.clear(path)

    def v_tc_platform(self, hex):
        try:
            sp = bytes.fromhex(hex)
        except ValueError:
            raise VerbError(400, "hex is not hexadecimal")
        if len(sp) < 10 or not sp[0] & 0x10:
            raise VerbError(400, "not a TC space packet")
        svc, sub, src = sp[7], sp[8], sp[9]
        if svc not in tcbuild.PLATFORM_SERVICES:
            raise VerbError(403, "service %d: platform TCs are services 9 "
                            "and 20 only; operator TCs belong to the GS"
                            % svc)
        if src != tcbuild.SOURCE_PLATFORM:
            raise VerbError(403, "source %d: platform TCs use source 14"
                            % src)
        why = self.can_send()
        if why is not None:
            raise VerbError(409, why)
        if (svc, sub) == (9, 129):
            # SF3: pending exactly like timesync.send (409 while busy)
            self.timesync.send_raw(sp)
        else:
            self.relay.send_unit(tcbuild.unit(sp))
        p = tcbuild.parse_tc(sp)
        self.log.emit("ctl", "tc", "tc.platform.send",
                      {"hex": sp.hex(), "service": svc, "subtype": sub,
                       "source": src, "crc_ok": p[6] if p else False})
        return {"service": svc, "subtype": sub, "len": len(sp)}

    def _gs_enabled(self):
        mode = self.cfg["gs"]["emulation"]
        if mode == "on" or (mode == "auto" and self.cfg["mode"] == "run"):
            return None
        return ("tc.gs.send needs [gs] emulation = \"on\" (default: on in "
                "run mode only)")

    def v_tc_gs(self, service, subtype, data="", ack=9):
        try:
            payload = bytes.fromhex(data)
        except ValueError:
            raise VerbError(400, "data is not hexadecimal")
        with self.lock:
            seq = self.seq_gs
            self.seq_gs = (self.seq_gs + 1) & 0x3FFF
        sp = tcbuild.tc_packet(tcbuild.APID_TC, seq, service, subtype,
                               payload, ack=ack,
                               source=self.cfg["gs"]["emulation_source"])
        ok = self.relay.gs_tc(sp, source="scn", path="tc.gs.send")
        return {"seq": seq, "delivered": ok, "label": "GS-EMULATION"}

    # -- fault actions (D-PLq) ------------------------------------------------
    def _fault_actions(self):
        """{kind: (apply -> prior, undo(prior))}: undo puts back what the
        apply replaced (SF4), so a fault on a link the operator had
        already taken down leaves it down when cleared."""
        def link(entry, p):
            prior = self.link_down
            self.v_link("down")
            return prior

        def link_undo(entry, p, prior):
            self.v_link("down" if prior else "up")

        def tc_off(entry, p):
            prior = self.timecodes
            self.v_tc_enable(False)
            return prior

        def tc_off_undo(entry, p, prior):
            # None: the operator never set it, so QEMU's default (on)
            self.v_tc_enable(True if prior is None else prior)
            self.timecodes = prior

        def tc_inject(entry, p):
            self.v_tc_inject(p["timecnt"])

        def obt_jump(entry, p):
            obt, _ = self.read_obt()
            self._qom_set("obt", (obt + p["delta_s"] * 65536)
                          & 0xFFFFFFFFFFFF)

        def ka_off(entry, p):
            prior = self.keepalive.enabled
            self.v_keepalive(enabled=False)
            return prior

        def ka_undo(entry, p, prior):
            self.v_keepalive(enabled=prior)

        def adc_path(entry):
            return "instrument.hk_adc.%d" % entry["action"]["ch"]

        def adc(entry, p):
            prior = self.forcing.status().get(adc_path(entry))
            self.forcing.set(adc_path(entry), p["value"],
                             why="fault " + entry["id"])
            return prior

        def adc_undo(entry, p, prior):
            why = "fault " + entry["id"] + " cleared"
            if prior is None:
                self.forcing.clear(adc_path(entry), why=why)
            else:
                self.forcing.set(adc_path(entry), prior, why=why)

        def qom(entry, p):
            a = entry["action"]
            if not faults_mod.qom_path_ok(a["path"]):
                raise VerbError(403, "qom path %r is not a STIX device"
                                % a["path"])
            q = self._q()
            prior = None
            if "clear" not in a:
                prior = q.qom_get(a["path"], a["property"])
            q.qom_set(a["path"], a["property"],
                      p[a["param"]] if "param" in a else a["value"])
            return prior

        def qom_undo(entry, p, prior):
            a = entry["action"]
            self._q().qom_set(a["path"], a["property"],
                              a["clear"] if "clear" in a else prior)

        def nothing(entry, p, prior=None):
            pass

        return {"link_down": (link, link_undo),
                "timecodes_off": (tc_off, tc_off_undo),
                "timecode_inject": (tc_inject, nothing),
                "obt_jump": (obt_jump, nothing),
                "keepalive_off": (ka_off, ka_undo),
                "adc": (adc, adc_undo),
                "qom": (qom, qom_undo)}

    # -- read side ------------------------------------------------------------
    def status(self):
        return {"link": "down" if self.link_down else "up",
                "timecodes": self.timecodes,
                "keepalive": self.keepalive.status(),
                "timesync": self.timesync.status(),
                "tm_count": self.relay.tm_count,
                "held_tcs": len(self.relay.held)}

    def close(self):
        self.relay.close()
