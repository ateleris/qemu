# SPDX-License-Identifier: GPL-2.0-or-later
"""The platform module (platform plan PL-4, D-PLk): the SpW link, the
time codes, the OBT, TC(9,129), TC(20,128), forcing, faults, the FSW
parameters (fsw-params plan), and the TM relay + TC proxy, behind the
platform.*, force.*, fault.*, tc.* and param.* verbs.

What a device reset clears (link-down, adc-inject) is re-applied after
a warm guest reset and before `cont` after a cold power-on (D-PLf); so
are the active device-state faults (PL-11a, D-PSa).
"""

import hashlib
import json
import threading
import time

from . import faults as faults_mod
from . import flashlib
from . import fswmode as fswmode_mod
from . import idb as idb_mod
from . import params as params_mod
from . import tcbuild
from . import tapes as tapes_mod
from .dispatch import VerbError
from .force import Forcing
from .keepalive import KeepAlive
from .qmp import QmpError
from .relay import Relay
from .timesync import TimeSync

STIXSPW = "/machine/stixspw"
STIXCTRL = "/machine/stixctrl"
STIXATT = STIXCTRL + "/attenuator"      # PL-11f: the cmdsta forcing
STIXEDAC = STIXCTRL + "/edac"           # PL-11f: the EDAC knobs
STIXFLASH = "/machine/stixflash"        # PL-11g: the damage verbs


def split_kv(line):
    """The device status strings' key=value comma splitter (the
    det-rotb-burst, det-tape-status, det-align-status and
    det-adc-status lines)."""
    out = {}
    for kv in line.split(","):
        k, _, v = kv.partition("=")
        out[k.strip()] = v.strip()
    return out


class Platform:
    def __init__(self, ctl):
        self.ctl = ctl
        cfg = ctl.cfg
        self.cfg = cfg
        self.run_dir = ctl.run_dir
        self.log = ctl.log
        self.clock = ctl.clock
        self.power = ctl.power
        self.lock = threading.Lock()
        self.link_down = False          # the controller's truth
        self.timecodes = None           # None = QEMU's default
        self.seq14 = 0                  # source 14 on APID 0x5AC
        self.seq_gs = 0                 # GS emulation (source 121)
        self._seq14_sent = {}           # seq -> monotonic send time
        self._tape_seen = False         # the tape step leaf seen (T4)
        self._last_step = None          # the tape step last seen (T4)
        # A2: the HK decoder is given to the relay before its threads
        # start; None (with one timeline.idb record) when the IDB JSON
        # is missing or broken
        self.relay = Relay(cfg, ctl.run_dir, self.log, self.clock,
                           self.gs_link_ok,
                           idb_mod.load(cfg["timeline"]["idb_json"],
                                        cfg["timeline"]["idb_sqlite"],
                                        self.log))
        self.keepalive = KeepAlive(cfg, self.clock, self.log,
                                   self.relay.send_unit, self.can_send)
        self.timesync = TimeSync(self.clock, self.log, self.read_obt,
                                 self.relay.send_unit, self.can_send,
                                 self._next_seq14,
                                 autostart=cfg["timesync"]["autostart"])
        # the FSW parameters (fsw-params plan T2): same seq_gs counter
        # and GS link rule as tc.gs.send
        self.params = params_mod.Params(
            self.power, self.relay, self.log, self.clock, cfg,
            ctl.elf_sha256, next_seq=self._next_seq_gs,
            link_ok=self.gs_link_ok, platform_seq_recent=self.seq14_recent)
        self.forcing = Forcing(self.log, self.qmp)
        self.faults = faults_mod.Faults(
            faults_mod.load(cfg["faults"]["catalogue"] or None),
            self.log, self.clock, self._fault_actions())
        # U2: the FSW operating mode leaf (fsw.mode), from the same TM
        # listeners as keepalive/timesync/params
        self.fsw_mode = fswmode_mod.FswMode(ctl.state, self.clock,
                                            ctl.state_platform)
        self.relay.listeners += [self.keepalive.on_tm, self.timesync.on_tm,
                                 self.params.on_tm, self.fsw_mode.on_tm]
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

    def _qom_set(self, prop, value, path=STIXSPW):
        try:
            self._q().qom_set(path, prop, value)
        except QmpError as e:
            raise VerbError(409, "%s: %s" % (prop, e))

    def _next_seq14(self):
        with self.lock:
            s = self.seq14
            self.seq14 = (self.seq14 + 1) & 0x3FFF
            self._seq14_sent[s] = time.monotonic()
            if len(self._seq14_sent) > 256:     # keep the recent ones
                cut = time.monotonic() - 60.0
                self._seq14_sent = {k: v for k, v in
                                    self._seq14_sent.items() if v >= cut}
            return s

    def seq14_recent(self, seq, window_s):
        """True when a platform TC (source 14, e.g. timesync's
        TC(9,129)) took @seq within @window_s: it shares APID 0x5AC with
        the GS counter, and a TM(1,x) echoes only the 14-bit seq."""
        with self.lock:
            t = self._seq14_sent.get(seq)
        return t is not None and time.monotonic() - t <= window_s

    def _next_seq_gs(self):
        with self.lock:
            s = self.seq_gs
            self.seq_gs = (self.seq_gs + 1) & 0x3FFF
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
        self._stim_step()

    def _stim_step(self):
        """T4: one `stim` record per instrument.detector.tape.step
        change (the poller refreshes the leaves; the play/stop verbs
        call their own after their QOM writes). The leaf's creation —
        at boot, or again after a power-off cleared the tree — is no
        record, and neither is a step that holds."""
        lf = self.ctl.state.get("instrument.detector.tape.step")
        if lf is None:
            self._tape_seen = False
            self._last_step = None
            return
        step = lf["truth"]
        if not self._tape_seen:
            self._tape_seen = True
            self._last_step = step
            return
        if step == self._last_step:
            return
        self._last_step = step

        def leaf(p):
            x = self.ctl.state.get("instrument.detector.tape." + p)
            return x["truth"] if x else None
        self.log.emit("ctl", "stim", "instrument.detector.tape.step",
                      {"name": leaf("name"), "step": step,
                       "n_steps": leaf("n_steps"), "label": leaf("label"),
                       "kind": leaf("kind")})

    def _stim_emit(self, status, kind=""):
        """The play/stop verbs' own stim record, from the status they
        just parsed (the leaves catch up at the next poll)."""
        self._tape_seen = True
        self._last_step = status.get("step")
        self.log.emit("ctl", "stim", "instrument.detector.tape.step",
                      {"name": status.get("name"), "step":
                       status.get("step"), "n_steps":
                       status.get("n_steps"), "label":
                       status.get("label"), "kind": kind})

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
            self.fsw_mode.reset()       # a new boot_id
        elif event == "boot":
            self.relay.drop_held("boot")
            self.keepalive.on_boot()
            self.timesync.on_boot()
            self.fsw_mode.reset()
        elif event == "off":
            self.relay.drop_held("off")
            self.keepalive.on_off()
            self.timesync.on_off()
            self.fsw_mode.reset()

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
        d.add("platform.flash.save", self.v_flash_save,
              args={"name": {"type": "string", "required": True}},
              doc="save flash.bin into the flash library under this "
                  "name (a sparse copy; power off or paused)")
        d.add("platform.flash.load", self.v_flash_load,
              args={"name": {"type": "string", "required": True}},
              doc="replace flash.bin with that library entry (a sparse "
                  "copy; power off only)")
        d.add("platform.flash.list", self.v_flash_list,
              doc="the flash library's records, sorted by name")
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
        d.add("det.rotb.burst", self.v_det_rotb_burst,
              args={"frames": {"type": "integer", "required": True}},
              doc="append that many copies of the last live detector "
                  "frame into the ROTB ring now; returns the parsed "
                  "burst status (409 while STIX is off or the device "
                  "refuses: no live chunk, 0 or over the chunk count)")
        d.add("det.tape.save", self.v_tape_save,
              args={"name": {"type": "string", "required": True},
                    "tape": {"type": "object", "required": True}},
              doc="save a mix tape as <name>.json in the tapes dir "
                  "([timeline] tapes_dir, default next to the views); "
                  "the model's schema, validated here first (400 with "
                  "the step index)")
        d.add("det.tape.delete", self.v_tape_delete,
              args={"name": {"type": "string", "required": True}},
              doc="delete a saved mix tape (404 unknown)")
        d.add("det.tape.play", self.v_tape_play,
              args={"name": {"type": "string", "required": True},
                    "loop": {"type": "boolean"}},
              doc="load a saved mix tape into the model and start it on "
                  "the guest-time tick (det-tape-load then "
                  "det-tape-play); loop overrides the tape's own; the "
                  "answer is the parsed det-tape-status (409 while STIX "
                  "is off or the model refuses)")
        d.add("det.tape.stop", self.v_tape_stop,
              doc="stop the playing tape (det-tape-play false); the "
                  "rates revert to the scene knobs; the answer is the "
                  "parsed det-tape-status")
        d.add("det.align.set", self.v_align_set,
              args={"enabled": {"type": "boolean", "required": True}},
              doc="the ELUT-alignment bench knob (det-align) + the "
                  "parsed det-align-status report; a misaligned loaded "
                  "ELUT then shows in mismatch_cells")
        d.add("det.adc.set", self.v_adc_set,
              args={"enabled": {"type": "boolean", "required": True}},
              doc="the ADC-faithful sky knob (det-adc-mode, default "
                  "off) + the parsed det-adc-status; when on, the "
                  "synthetic sky folds through the loaded FPGA ELUT")
        d.add("det.scene.set", self.v_det_scene_set,
              args={"source": {"type": "string",
                               "enum": ["zero", "synthetic",
                                        "replay-dss"]},
                    "flare": {"type": "boolean"},
                    "quiet_cps": {"type": "integer", "min": 0,
                                  "max": 1000000},
                    "flare_cps": {"type": "integer", "min": 0,
                                  "max": 1000000}},
              doc="the synthetic sky's scene knobs (det-source, "
                  "det-flare, det-quiet-cps, det-flare-cps on "
                  "/machine/stixctrl); at least one argument, and only "
                  "the given ones are written; the device's refusal "
                  "(replay-dss without a replay table) is the 409")
        d.add("det.inject", self.v_det_inject,
              args={"fpga_det": {"type": "integer", "required": True,
                                 "min": 0, "max": 31},
                    "pix": {"type": "integer", "required": True,
                            "min": 0, "max": 11},
                    "ebin": {"type": "integer", "required": True,
                             "min": 0, "max": 31},
                    "count": {"type": "integer", "required": True,
                              "min": 1}},
              doc="one count deposit into the accumulator event sink "
                  "(det-inject, write-only): one qom-set with the "
                  "device's fpga_det=F,pix=P,ebin=B,count=N string; "
                  "fpga_det is DSS 0..31, pix 0..11, ebin 0..31, count "
                  ">= 1; the counts land in the finished QL bank's LACC "
                  "and in EACC at the next nominal swap")
        d.add("det.replay.start", self.v_det_replay_start,
              doc="start a loaded .dssevs replay now (det-replay-start "
                  "true; the device refuses without a table or without "
                  "det-source=replay-dss); the start applies at the "
                  "next 100 ms tick")
        d.add("det.replay.stop", self.v_det_replay_stop,
              doc="stop a running replay now (det-replay-start false, "
                  "applied at once)")
        d.add("det.replay.config", self.v_det_replay_config,
              args={"trigger": {"type": "string",
                                "enum": ["nominal", "qmp"]},
                    "offset_ms": {"type": "integer", "min": 0,
                                  "max": 429496729599}},
              doc="the replay's scene knobs (det-replay-trigger "
                  "nominal|qmp, det-replay-offset-ms); at least one "
                  "argument, only the given ones are written; both "
                  "survive a device reset, the offset is capped at "
                  "2^32 ticks of replay time")
        d.add("att.cmdsta.set", self.v_att_cmdsta_set,
              args={"value": {"type": "integer", "required": True,
                              "min": 0}},
              doc="force bits 11:5 of the attenuator's 0x5C readback to 1 "
                  "(force.cmdsta-set on /machine/stixctrl/attenuator); "
                  "the value is masked to 0xfe0 (400 naming the mask "
                  "otherwise); the device clears the force on reset and "
                  "the controller does not re-apply it across cycles")
        d.add("att.cmdsta.clear", self.v_att_cmdsta_clear,
              args={"value": {"type": "integer", "required": True,
                              "min": 0}},
              doc="force bits 11:5 of the attenuator's 0x5C readback to 0 "
                  "(force.cmdsta-clear); a bit in both masks reads 0 (the "
                  "clear wins); clears on reset, not re-applied")
        d.add("mem.edac.enable", self.v_edac_enable,
              args={"module": {"type": "string", "required": True,
                               "enum": ["all", "sdram", "cache", "rotbuf"]},
                    "on": {"type": "boolean", "default": True}},
              doc="set the EdacOn bits at run time (edac-enable on "
                  "/machine/stixctrl/edac): all sends the string "
                  "\"all-on\", sdram/cache/rotbuf the masks \"1\"/\"2\"/"
                  "\"4\"; the property only sets, never clears, so on is "
                  "true (a false is a 400 that never reaches the device); "
                  "the device's refusals are the 409; Edacon is not "
                  "re-applied across cycles (the FSW or a reset owns the "
                  "off path)")
        d.add("mem.upset.clear", self.v_upset_clear,
              args={"id": {"type": ["string", "integer"], "required": True,
                           "min": 0}},
              doc="clear armed upsets (upset-clear on the edac): \"all\" "
                  "or an index into instrument.memory.poisons; 409 while "
                  "STIX is off or the index is not armed")
        d.add("mem.upset.burst", self.v_upset_burst,
              args={"seed": {"type": "integer", "min": 0,
                             "max": 0xFFFFFFFF},
                    "count": {"type": "integer", "required": True,
                              "min": 1, "max": 0xFFFFFFFF}},
              doc="the seeded random upset burst (random-seed, then "
                  "random-count, on the edac): count one-bit upsets drawn "
                  "from the seed (0 = off, survives a reset); a burst "
                  "without seed uses the device's stored seed and is a "
                  "409 while it is 0")
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
              enabled=self._gs_enabled("tc.gs.send"))
        # the FSW parameters (fsw-params plan T3-T5): the TC-carrying
        # verbs sit behind the same operator-TC gate as tc.gs.send
        d.add("param.refresh", self.params.refresh,
              doc="a RAM snapshot of the parameters' current values "
                  "(one QMP memsave into the run dir); 409 while STIX "
                  "is off")
        d.add("param.read", self.params.read,
              args={"index": {"type": "integer", "required": True,
                              "min": 0, "max": 65535},
                    "sub": {"type": "integer", "min": 0, "max": 2,
                            "default": 0}},
              doc="read one parameter back with TC(237,19) and wait up "
                  "to 2 s for its TM(237,20); sub 0 = value, 1 = min, "
                  "2 = max",
              enabled=self._gs_enabled("param.read"))
        d.add("param.read_all", self.params.read_all,
              args={"only_changed": {"type": "boolean",
                                     "default": True}},
              doc="read rows back one by one: only the rows whose "
                  "value differs from the ELF default unless "
                  "only_changed is false; progress every 32",
              enabled=self._gs_enabled("param.read_all"), locked=False)
        d.add("param.set", self.params.set,
              args={"index": {"type": "integer", "required": True,
                              "min": 0, "max": 65535},
                    "value": {"type": "integer", "required": True,
                              "min": 0, "max": 0xFFFFFFFF},
                    "sub": {"type": "integer", "min": 0, "max": 2,
                            "default": 0}},
              doc="set a parameter with TC(237,18); delivered false "
                  "with the reason when the TC cannot go out",
              enabled=self._gs_enabled("param.set"))
        d.add("param.reset_all", self.params.reset_all,
              args={"only_changed": {"type": "boolean",
                                     "default": True}},
              doc="TC(237,18) the ELF default of every row whose known "
                  "value differs (all known rows when only_changed is "
                  "false)",
              enabled=self._gs_enabled("param.reset_all"), locked=False)

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

    def v_det_rotb_burst(self, frames):
        """FF plan: the det-rotb-burst runtime QOM write (on
        /machine/stixctrl): append @frames copies of the last live
        chunk now. The device's error text (0, an oversize count, no
        live chunk since reset) becomes the 409; the status string is
        the answer."""
        self._qom_set("det-rotb-burst", frames, STIXCTRL)
        try:
            status = self._q().qom_get(STIXCTRL, "det-rotb-burst-status")
        except QmpError as e:
            raise VerbError(409, "det-rotb-burst-status: %s" % e)
        out = split_kv(status)
        try:
            return {"requested": int(out["requested"]),
                    "appended": int(out["appended"]),
                    "stop": out["stop"],
                    "rotb_count": int(out["rotb_count"])}
        except (KeyError, ValueError):
            raise VerbError(500, "det-rotb-burst-status %r" % status)

    # -- the detector tape and ELUT alignment (stimulus plan T3, A1, C2)
    def _det_status(self, prop, casts):
        """A read-only det status string, parsed (409 off, 500 a line
        that does not parse)."""
        try:
            line = self._q().qom_get(STIXCTRL, prop)
        except QmpError as e:
            raise VerbError(409, "%s: %s" % (prop, e))
        out = split_kv(line)
        try:
            return {k: cast(out[k]) for k, cast in casts.items()}
        except (KeyError, ValueError):
            raise VerbError(500, "%s %r" % (prop, line))

    @staticmethod
    def _tape_status_casts():
        return {"state": str, "name": str, "label": str,
                "step_left_ticks": int,
                # dropped rides since S3 (V3): the pix-gate drop gauge
                "dropped": int,
                "loop": lambda v: v == "true",
                "align": lambda v: v == "true"}

    def _tape_status(self):
        """det-tape-status, with step=K/N split into step and n_steps.
        An older model's line without dropped still parses (V3)."""
        try:
            line = self._q().qom_get(STIXCTRL, "det-tape-status")
        except QmpError as e:
            raise VerbError(409, "det-tape-status: %s" % e)
        out = split_kv(line)
        casts = self._tape_status_casts()
        try:
            r = {k: cast(out[k]) for k, cast in casts.items()
                 if k != "dropped" or "dropped" in out}
            step, _, n = out["step"].partition("/")
            r["step"] = int(step)
            r["n_steps"] = int(n)
            return r
        except (KeyError, ValueError):
            raise VerbError(500, "det-tape-status %r" % line)

    def _kev_full(self):
        """S2: the current K, the leaf instrument.detector.adc.kev_full
        (the model's default 160 when absent)."""
        lf = self.ctl.state.get("instrument.detector.adc.kev_full")
        v = lf.get("truth") if isinstance(lf, dict) else None
        if isinstance(v, bool) or not isinstance(v, int) or \
                not 1 <= v <= tapes_mod.KEV_FULL_MAX:
            return tapes_mod.KEV_FULL_DEFAULT
        return v

    def v_tape_save(self, name, tape):
        """Save the tape as <name>.json in the tapes dir (the model's
        schema, validated client-side first). S2: keV values >= the
        current K come back as "warnings" (additive; not a refusal)."""
        r = tapes_mod.save(self.cfg, name, tape)
        warnings = tapes_mod.kev_warnings(r, self._kev_full())
        return dict(r, warnings=warnings) if warnings else r

    def v_tape_delete(self, name):
        tapes_mod.delete(self.cfg, name)
        return {"deleted": name}

    def v_tape_play(self, name, loop=None):
        """Load the saved tape into the model and start it (the loop
        argument overrides the tape's own, as in the schema). The read
        and the validation come before any QMP."""
        tape = tapes_mod.read(self.cfg, name)       # 400/404 first
        # S2: the spectrum files read and parsed, absolute paths (400
        # before any QMP); keV >= K is a warning
        kev_full = self._kev_full()
        warnings = tapes_mod.kev_warnings(tape, kev_full)
        tape = tapes_mod.resolve(self.cfg, tape, kev_full, warnings)
        if loop is not None:
            tape = dict(tape)
            tape["loop"] = loop
        self._qom_set("det-tape-load", json.dumps(tape), STIXCTRL)
        self._qom_set("det-tape-play", True, STIXCTRL)
        r = self._tape_status()
        self._stim_emit(r, self._tape_step_kind(tape, r["step"]))
        if warnings:
            r["warnings"] = warnings
        return r

    def v_tape_stop(self):
        self._qom_set("det-tape-play", False, STIXCTRL)
        r = self._tape_status()
        self._stim_emit(r)
        return r

    @staticmethod
    def _tape_step_kind(tape, step):
        """The kind of the step the status points at (1-based)."""
        steps = tape.get("steps") or []
        if isinstance(step, int) and 1 <= step <= len(steps):
            return steps[step - 1].get("kind")
        return ""

    def v_align_set(self, enabled):
        """A1's verb: the det-align bench knob + the parsed report."""
        self._qom_set("det-align", enabled, STIXCTRL)
        return self._det_status("det-align-status", {
            "mode": str, "rows": int, "fallback": int, "honoured": int,
            "mismatch_cells": int, "unassigned_cells": int,
            "calibration": str, "preimage_miss": int})

    def v_adc_set(self, enabled):
        """C2's verb: the det-adc-mode bench knob + the parsed status."""
        self._qom_set("det-adc-mode", enabled, STIXCTRL)
        return self._det_status("det-adc-status", {
            "mode": str, "kev_full": int, "base_source": str,
            "map_rows": int})

    # -- PL-11e: the scene, the deposit and the replay knobs (all on
    # /machine/stixctrl; validation before any QMP, the device's own
    # refusals surface as the 409 from _qom_set)
    def v_det_scene_set(self, source=None, flare=None, quiet_cps=None,
                        flare_cps=None):
        """The scene knobs of the synthetic sky (D-Df). One verb, only
        the given knobs are qom-set (D-PSc's minimal sets, m12); the
        leaves follow at the next poll."""
        given = {"source": source, "flare": flare, "quiet_cps": quiet_cps,
                 "flare_cps": flare_cps}
        if all(v is None for v in given.values()):
            raise VerbError(400, "det.scene.set: give at least one of "
                            "source, flare, quiet_cps, flare_cps")
        for prop, v in (("det-source", source), ("det-flare", flare),
                        ("det-quiet-cps", quiet_cps),
                        ("det-flare-cps", flare_cps)):
            if v is not None:
                self._qom_set(prop, v, STIXCTRL)
        return {k: v for k, v in given.items() if v is not None}

    def v_det_inject(self, fpga_det, pix, ebin, count):
        """D-6: the one-shot deposit, write-only det-inject. The bounds
        are the device's (stix_det.c:4237-4278: fpga_det < 32, pix < 12,
        ebin < 32, count 0 adds no counts) and sit in the verb's arg
        schema; the deposit lands at the next swaps."""
        self._qom_set("det-inject",
                      "fpga_det=%d,pix=%d,ebin=%d,count=%d"
                      % (fpga_det, pix, ebin, count), STIXCTRL)
        return {"fpga_det": fpga_det, "pix": pix, "ebin": ebin,
                "count": count}

    def v_det_replay_start(self):
        """D-7: det-replay-start true. The device refuses a start
        without a table or without det-source=replay-dss; a start is
        tick-deferred, so the replay.state leaf follows at a poll."""
        self._qom_set("det-replay-start", True, STIXCTRL)
        return {"started": True}

    def v_det_replay_stop(self):
        """D-7: det-replay-start false; a stop applies at once."""
        self._qom_set("det-replay-start", False, STIXCTRL)
        return {"started": False}

    def v_det_replay_config(self, trigger=None, offset_ms=None):
        """The replay's two runtime knobs (O-N2a: they ride this verb,
        only the file is a [qemu] key). The offset cap is the feeder's
        (stix_det.c:4353-4358: past 2^32 100 ms ticks is rejected)."""
        if trigger is None and offset_ms is None:
            raise VerbError(400, "det.replay.config: give trigger or "
                            "offset_ms")
        if trigger is not None:
            self._qom_set("det-replay-trigger", trigger, STIXCTRL)
        if offset_ms is not None:
            self._qom_set("det-replay-offset-ms", offset_ms, STIXCTRL)
        out = {}
        if trigger is not None:
            out["trigger"] = trigger
        if offset_ms is not None:
            out["offset_ms"] = offset_ms
        return out

    # -- PL-11f: the attenuator cmdsta forcing and the EDAC knobs
    # (validation before any QMP; the device's own refusals surface as
    # the 409 from _qom_set)
    @staticmethod
    def _cmdsta_check(value):
        """The device's rule (stix_att.c:874-905): bits 11:5 only, the
        mask named in the 400."""
        if value & ~0xFE0:
            raise VerbError(400, "value 0x%x has bits outside 11:5 "
                            "(the force mask is 0xfe0)" % value)
        return value

    def _att_cmdsta(self, value, prop):
        self._qom_set(prop, self._cmdsta_check(value), STIXATT)
        return {"value": value}

    def v_att_cmdsta_set(self, value):
        return self._att_cmdsta(value, "force.cmdsta-set")

    def v_att_cmdsta_clear(self, value):
        return self._att_cmdsta(value, "force.cmdsta-clear")

    def v_edac_enable(self, module, on=True):
        """O4 verbatim: edac-enable is a string property and its setter
        only ORs the EdacOn bits in (stix_edac.c:1285-1352), so on must
        be true and never reaches the device as false."""
        if not on:
            raise VerbError(400, "on must be true: edac-enable only "
                            "sets, it never clears (the off path is the "
                            "FSW's own TC(236) write or a reset)")
        enable = {"all": "all-on", "sdram": "1", "cache": "2",
                  "rotbuf": "4"}[module]
        self._qom_set("edac-enable", enable, STIXEDAC)
        return {"module": module, "on": True}

    def v_upset_clear(self, id):
        """upset-clear takes the device's string: "all" or the index
        (stix_edac.c:1044-1091). Off, or an index that is not armed, is
        the 409."""
        if isinstance(id, str) and id != "all":
            raise VerbError(400, "id is \"all\" or an index into "
                            "instrument.memory.poisons")
        self._qom_set("upset-clear", str(id), STIXEDAC)
        return {"cleared": id}

    def v_upset_burst(self, count, seed=None):
        """The seeded burst (stix_edac.c:1133-1199): the seed is plain
        state (0 = off), the count is the write-only verb that fires the
        draw; seed first, then count."""
        if seed is not None:
            self._qom_set("random-seed", seed, STIXEDAC)
        self._qom_set("random-count", count, STIXEDAC)
        return {"count": count, "seed": seed}

    @staticmethod
    def _flash_name(name):
        """A bad library name is the caller's error (400), whatever the
        power state."""
        try:
            flashlib.check_name(name)
        except ValueError as e:
            raise VerbError(400, str(e))

    def v_flash_save(self, name):
        """FF plan: save flash.bin into the flash library. Only while
        the FSW cannot be writing it: power off, or on and paused (the
        device fflush()es each page before its completion IRQ, so a
        paused flash file is consistent)."""
        self._flash_name(name)
        if self.power.state not in ("off", "paused"):
            raise VerbError(409, "flash save needs power off or paused "
                            "(STIX is %s and may be writing the flash)"
                            % self.power.state)
        q = self.cfg["qemu"]
        h = hashlib.sha256()
        try:
            with open(q["elf"], "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    h.update(chunk)
        except OSError as e:
            raise VerbError(409, "cannot hash %s: %s" % (q["elf"], e))
        meta = {"elf": q["elf"], "elf_sha256": h.hexdigest(),
                "qemu": q["binary"], "sim_time": self.clock.exact_sim(),
                "boot_id": self.clock.boot_id}
        try:
            return flashlib.save(self.cfg, self.run_dir, name, meta)
        except FileExistsError as e:
            raise VerbError(409, str(e))
        except ValueError as e:
            raise VerbError(400, str(e))
        except flashlib.FlashLibError as e:
            raise VerbError(409, str(e))
        except OSError as e:
            raise VerbError(500, str(e))

    def v_flash_load(self, name):
        """FF plan: replace flash.bin from the flash library; power off
        only (QEMU must not have the file open)."""
        self._flash_name(name)
        if self.power.state != "off":
            raise VerbError(409, "flash load needs power off (STIX is "
                            "%s)" % self.power.state)
        try:
            return flashlib.load(self.cfg, self.run_dir, name)
        except FileNotFoundError as e:
            raise VerbError(404, str(e))
        except ValueError as e:
            raise VerbError(400, str(e))
        except flashlib.FlashLibError as e:
            raise VerbError(409, str(e))
        except OSError as e:
            raise VerbError(500, str(e))

    def v_flash_list(self):
        return {"dir": flashlib.library_dir(self.cfg),
                "library": flashlib.listing(self.cfg)}

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

    def _gs_enabled(self, verb):
        """The enabled() gate of the operator-TC verbs: the refusal
        reason names the verb being refused."""
        def check():
            mode = self.cfg["gs"]["emulation"]
            if mode == "on" or (mode == "auto" and self.cfg["mode"] == "run"):
                return None
            return ('%s needs [gs] emulation = "on" (default: on in run '
                    "mode only)" % verb)
        return check

    def v_tc_gs(self, service, subtype, data="", ack=9):
        try:
            payload = bytes.fromhex(data)
        except ValueError:
            raise VerbError(400, "data is not hexadecimal")
        seq = self._next_seq_gs()
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

        def flash_damage(entry, p):
            # PL-11g (D-PSe): the spec resolved against the params, JSON
            # encoded as the device's damage-add takes it (one sidecar
            # entry as a JSON object string, grlib_stixflash.c:
            # 1507-1538). Write-only (no getter exists, :1767-1768): no
            # prior read — the spec string is the undo anchor. The
            # device's refusals (a duplicate at a warm reset, the
            # reserved blocks) surface as the 409.
            spec = json.dumps(
                faults_mod.flash_build(entry["action"]["spec"], p))
            self._qom_set("damage-add", spec, STIXFLASH)
            return spec

        def flash_damage_undo(entry, p, prior):
            # the device's exact-spec removal (grlib_stixflash.c:
            # 1585-1621): the same string, rebuilt from the entry
            spec = json.dumps(
                faults_mod.flash_build(entry["action"]["spec"], p))
            self._qom_set("damage-remove", spec, STIXFLASH)

        def nothing(entry, p, prior=None):
            pass

        return {"link_down": (link, link_undo),
                "timecodes_off": (tc_off, tc_off_undo),
                "timecode_inject": (tc_inject, nothing),
                "obt_jump": (obt_jump, nothing),
                "keepalive_off": (ka_off, ka_undo),
                "adc": (adc, adc_undo),
                "qom": (qom, qom_undo),
                "flash_damage": (flash_damage, flash_damage_undo)}

    # -- read side ------------------------------------------------------------
    def status(self):
        return {"link": "down" if self.link_down else "up",
                "timecodes": self.timecodes,
                "keepalive": self.keepalive.status(),
                "timesync": self.timesync.status(),
                "tm_count": self.relay.tm_count,
                "held_tcs": len(self.relay.held),
                "params": self.params.summary()}

    def close(self):
        self.relay.close()
