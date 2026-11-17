#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A fake qemu-system-sparc for the controller's self-test (no QEMU).

  fake_qemu.py [--variant new|old|noquit] <qemu args>

-M none -qmp stdio: the binary-guard probe; `old` answers
qom-list-properties for grlib-stixflash WITHOUT inverted-storage.
Otherwise it serves QMP on the -qmp unix socket like a frozen (-S)
leon3: cont/stop/quit, the RESUME/STOP/SHUTDOWN/RESET events, qom-get
vclock-ns, obt and both hw-state JSONs (the ctrl one carries the
attenuator, edac, det and rotb sections, PL-11b/PL-11c, with the det
section's tape/align/adc stimulus blocks), qom-set obt, the upset verbs
on the EDAC (they move the hw-state's poisons and counters), the
det-rotb-burst pair (the FF plan's burst), the det tape/align/adc QOM
pairs (the stimulus plan T3/C2), the det scene/inject/replay knobs
(PL-11e: the hw-state's det section echoes the stored scene; a
replay-dss source or a replay start is refused with the device's
no-table text), the flash device at /machine/stixflash
(the five watch props, the damage-add/remove verbs with the device's
duplicate and reserved-block refusals, and the x-fake-flash-op counter
bumps, PL-11d), the PL-11f knobs (the
attenuator's force.cmdsta-set/-clear move the hw-state's presented
cmdsta, upset-clear and random-count move the poisons, edac-enable and
random-seed are stored like the device's), memsave (the fsw-params RAM
snapshot: zeros into the asked file, in the cwd), and the test-only
x-fake-guest-reset, x-fake-tape-state, x-fake-align-state and
x-fake-adc-state, which behave per -no-reboot / -action like QEMU.
`noquit` ignores quit (the controller must escalate to SIGTERM);
`noqmp` accepts the QMP connection and never answers; `badstate`
serves a hw-state without its keys; `noflash` refuses every flash
qom-get (the poller's flash-error tolerance).
"""

import json
import os
import socket
import sys
import threading
import time

# the attenuator 0x5C readback after reset: POSITIONAB (bit 10, out)
# | OPERATIONSTATUS (bit 8, set by stix_att.c reset) = 0x500
ATT_CMDSTA_RESET = 0x500


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
            "upset-inject", "upset-clear", "det-tape-load", "det-tape-play",
            "det-align", "det-adc-mode",
            # PL-11e: the scene, the one-shot deposit and the replay
            # knobs on /machine/stixctrl
            "det-source", "det-flare", "det-quiet-cps", "det-flare-cps",
            "det-inject", "det-replay-start", "det-replay-trigger",
            "det-replay-offset-ms",
            # PL-11f: the attenuator cmdsta forcing and the EDAC knobs
            "force.cmdsta-set", "force.cmdsta-clear", "edac-enable",
            "random-seed", "random-count") + ATTENUATOR + \
    tuple("adc-inject[%d]" % ch for ch in range(128))
# read-back before the first set: the booleans are false, the offset 0
ATT_DEFAULT = {"attenuator.current.offset": 0}
# the EDAC device (the memory.* catalogue rows' target); its upset
# verbs move the hw-state's edac section, like the device's
EDAC = "/machine/stixctrl/edac"
# the attenuator (PL-11f): the cmdsta forcing props live here
ATT = "/machine/stixctrl/attenuator"
# the controller device (the det.* scene knobs are on its class)
CTRL = "/machine/stixctrl"
# the flash device (PL-11d): the five watch props, the damage verbs
FLASH = "/machine/stixflash"


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
        # the stimulus plan (T3/T4/C2): the tape playback, the ELUT
        # alignment report and the ADC-sky knob, as the det section of
        # the hw-state carries them at their defaults; the tape QOM
        # trio stores and reports like the device's store-then-apply
        self.tape = {"state": "idle", "name": "", "step": 0, "n_steps": 0,
                     "label": "", "kind": "", "loop": False, "align": False,
                     "spec": "", "dropped": 0}
        self.tape_json = None
        self.align = {"mode": "off", "rows": 384, "fallback": 0,
                      "honoured": 0, "mismatch_cells": 0,
                      "unassigned_cells": 0, "calibration": "none",
                      "preimage_miss": 0}
        self.adc = {"mode": False, "kev_full": 160, "base_source": "zero",
                    "map_rows": 0}
        # the EDAC state the hw-state section shows (stix_edac.c:
        # 714-759): no poisons armed, the counters at zero. The bram
        # has no EDAC: no counters and its upsets arm no poison (M-5),
        # so it never appears here
        self.poisons = []
        self.edac_counters = {m: {"injected": 0, "corrected": 0,
                                  "detected": 0}
                              for m in ("sdram", "cache", "rotbuf")}
        # PL-11f: the cmdsta forcing masks (stix_att.c:874-905, applied
        # to the readback as (value | set) & ~clear) and the burst seed
        # (0 = off, survives a reset like the device's). The real device
        # clears the cmdsta force on reset; this fake keeps all props
        # across a reset (see guest_reset)
        self.cmdsta_force = {"set": 0, "clear": 0}
        self.random_seed = 0
        # the flash device (PL-11d): the op counters move only through
        # x-fake-flash-op (D-F9); the damage list holds the parsed
        # runtime specs (damage-add/damage-remove) and survives a guest
        # reset like the device's runtime state
        self.flash_ops = {"ops-read": 0, "ops-prog": 0, "ops-erase": 0,
                          "ops-abort": 0}
        self.flash_damage = []
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

    def tape_status(self):
        t = self.tape
        return ("state=%s,name=%s,step=%d/%d,label=%s,step_left_ticks=0,"
                "loop=%s,align=%s,dropped=%d"
                % (t["state"], t["name"], t["step"], t["n_steps"],
                   t["label"], "true" if t["loop"] else "false",
                   "true" if t["align"] else "false", t["dropped"]))

    def align_status(self):
        a = self.align
        return ("mode=%s,rows=%d,fallback=%d,honoured=%d,"
                "mismatch_cells=%d,unassigned_cells=%d,calibration=%s,"
                "preimage_miss=%d"
                % (a["mode"], a["rows"], a["fallback"], a["honoured"],
                   a["mismatch_cells"], a["unassigned_cells"],
                   a["calibration"], a["preimage_miss"]))

    def adc_status(self):
        a = self.adc
        return ("mode=%s,kev_full=%d,base_source=%s,map_rows=%d"
                % ("on" if a["mode"] else "off", a["kev_full"],
                   a["base_source"], a["map_rows"]))

    def hw_ctrl(self):
        v = self.vclock()
        return {"schema": 5, "vclock_ns": v,
                "model": {"hk_adc": True, "powerstat": True,
                          "hk_prot": True, "det_model": True},
                "supplies": {"enables": 0, "powerstat": 0, "lines": {
                    n: {"enabled": False, "good": False}
                    for n in ("lv", "hv01", "hv17", "spw0", "spw1")}},
                # the one served HK channel is a named one (PL-11h, R10):
                # ch 64 (ASP_PHOTOA0_V), whose C-table nominal is 100
                "hk_adc": {"64": {"truth": 100, "presented": 100,
                                  "injected": False, "last": None}},
                # line 1 watches ch 0 (stixctrl_hk_prot_ch), the device's
                # mapping; the fake never samples, so this is inert data
                "hk_prot": {"irqen": 0, "protstat": 0, "hotsample": 0,
                            "lines": [{"line": 1, "ch": 0, "bit": 0,
                                       "threshold": 0, "prot_en": False}]},
                "watchdog": {"running": False, "enabled": False,
                             "count": 0, "reload": 0},
                "scet": {"coarse": v // 1_000_000_000,
                         "fine": (v % 1_000_000_000) * 65536
                         // 1_000_000_000, "unsynced": True},
                # the detector (stix_det.c, PL-11c): the scene as the
                # det.* knobs left it (the device defaults otherwise),
                # the FSM off, no replay table loaded (idle), quarter
                # power off. PL-11e: the scene echoes the stored props
                "det": {"quarter_on": [False] * 4,
                        "source": self.props.get((CTRL, "det-source"),
                                                 "synthetic"),
                        "flare": self.props.get((CTRL, "det-flare"),
                                                False),
                        "quiet_cps": self.props.get(
                            (CTRL, "det-quiet-cps"), 20),
                        "flare_cps": self.props.get(
                            (CTRL, "det-flare-cps"), 2000),
                        "ql_bank": 0, "fsm": {"enable": False},
                        "replay": {"state": "idle", "pos_ns": 0,
                                   "events": 0, "dropped": 0,
                                   "unsupported": 0, "table": "",
                                   "count": 0, "duration_ns": 0},
                        "tape": dict(self.tape),
                        "align": dict(self.align),
                        "adc": dict(self.adc)},
                # the ROTB ring (grlib_stixctrl.c, PL-11c): empty and
                # disabled, the default capacity
                "rotb": {"count": 0, "full": False, "enable": False,
                         "rd": 0, "wr": 0, "capacity": 104448},
                # the attenuator section (stix_att.c:966-998), idle at
                # "out": AB actuates at OUT (stix_att.c:291-293), so the
                # presented cmdsta carries the AB bit (bit 10,
                # StixIdpu.h:722); att_c is HK ch 8's idle adc. The
                # cmdsta forcing applies to the readback as
                # (value | set) & ~clear (stix_att.c:344-350)
                "attenuator": {
                    "truth": {"position": "out", "position_ppm": 0,
                              "sensor_ab": True, "sensor_bc": False,
                              "motor1_driving": False,
                              "motor2_driving": False, "att_c": 20,
                              "moves": 0, "last_end": "none",
                              "last_end_ns": 0},
                    "presented": {"sensor_ab": True, "sensor_bc": False,
                                  "cmdsta": (ATT_CMDSTA_RESET
                                             | self.cmdsta_force["set"])
                                  & ~self.cmdsta_force["clear"] & 0xFFF,
                                  "att_c": 20},
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
        the poisons list; the refusals are the device's texts."""
        if spec == "all":
            self.poisons = []
            return
        try:
            i = int(spec, 0)
        except ValueError:
            raise KeyError("upset-clear: expected \"all\" or an index "
                           "into upsets-active, not \"%s\"" % spec)
        if i < 0 or i >= len(self.poisons):
            raise KeyError("upset-clear: no upset %d armed (upsets-active "
                           "holds %d)" % (i, len(self.poisons)))
        del self.poisons[i]

    # the device's draw PRNG, xorshift32 13/17/5 (stix_edac.c:1122-1131)
    @staticmethod
    def _xorshift(state):
        x = state
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        return x

    def upset_burst(self, count):
        """random-count (stix_edac.c:1133-1199): count one-bit upsets
        from self.random_seed, 0 injects nothing and a 0 seed is off
        (the device's texts). The fake does NOT model the real draw —
        the device picks the memory, word and bit uniformly among the
        wired EDAC memories and skips already poisoned words (the qtest
        edac-random pins it); it only needs count plausible poison
        records, drawn deterministically from the seed over
        sdram/cache/rotbuf so the leaves move the same way every run."""
        if count == 0:
            raise KeyError("random-count: 0 injects nothing")
        if self.random_seed == 0:
            raise KeyError("random-count: random-seed is 0 (off): set a "
                           "nonzero seed first")
        if count > 256 - len(self.poisons):
            raise KeyError("random-count: %d upsets do not fit the poison "
                           "table (%d free of %d)"
                           % (count, 256 - len(self.poisons), 256))
        # one-bit mask, so every record is a seu; the addresses are the
        # modules' plausible window starts, the rotbuf one split into
        # the block:offset pair the records carry
        base = {"sdram": 0x40000000, "cache": 0xE0180000,
                "rotbuf": 0x00000000}
        state = self.random_seed
        for _ in range(count):
            state = self._xorshift(state)
            mem = ("sdram", "cache", "rotbuf")[state % 3]
            state = self._xorshift(state)
            word = state % 4096
            state = self._xorshift(state)
            mask = 1 << (state % 32)
            rec = {"memory": mem, "mask": mask, "mode": "seu",
                   "persist": False}
            if mem == "rotbuf":
                # "<block>:<offset>": the block index into the ring and
                # the 4-aligned byte offset in the block (M-4)
                rec["block"] = word // 64
                rec["offset"] = (word % 64) * 4
                rec["address"] = rec["block"] * 0x200 + rec["offset"]
            else:
                rec["address"] = base[mem] + word * 4
            self.poisons.append(rec)
            self.edac_counters[mem]["injected"] += 1

    def flash_spec(self, value):
        """One damage spec string into a canonical record — the device's
        grammar (stixflash_dmg_entry, grlib_stixflash.c:1094-1298) as
        far as the fake needs it: the linear block/page form only."""
        try:
            e = json.loads(value)
        except ValueError:
            raise KeyError("stixflash: not JSON: %s" % value)
        if not isinstance(e, dict):
            raise KeyError("stixflash: the spec is not a JSON object")
        if set(e) - {"kind", "block", "page", "bit", "value"}:
            raise KeyError("stixflash: unknown key in the damage spec")
        kind = e.get("kind")
        if kind == "stuck0":        # the device's alias for value 0
            if "value" in e:
                raise KeyError("stixflash: 'value' with the 'stuck0' "
                               "alias; use kind \"stuck\"")
            kind, e = "stuck", dict(e, value=0)
        if kind not in ("erase-fail", "program-fail", "stuck"):
            raise KeyError("stixflash: bad kind %r" % kind)
        if kind == "stuck":
            if e.get("value") not in (0, 1):
                raise KeyError("stixflash: stuck needs a value 0 or 1")
        elif "value" in e:
            raise KeyError("stixflash: 'value' is only valid with kind "
                           "\"stuck\"")
        has_block = "block" in e
        has_page = "page" in e
        if has_block == has_page:   # exactly one of them
            raise KeyError("stixflash: needs a 'block' or a 'page'")
        if kind == "erase-fail" and has_page:
            raise KeyError("stixflash: erase-fail is a block kind")
        if kind == "program-fail" and has_block:
            raise KeyError("stixflash: program-fail is a page kind")
        rec = {"kind": kind}
        if has_block:
            rec["block"] = e["block"]
        else:
            rec["page"] = e["page"]
        if kind == "stuck":
            if has_page:
                try:
                    byte, bit = str(e["bit"]).split(".")
                    byte, bit = int(byte), int(bit)
                except (KeyError, ValueError, AttributeError):
                    raise KeyError("stixflash: stuck needs a bit as "
                                   "'byte.bit'")
                if not (0 <= byte < 4096 and 0 <= bit < 8):
                    raise KeyError("stixflash: bit out of range")
                rec["byte"], rec["bit"] = byte, bit
            elif "bit" in e:
                raise KeyError("stixflash: block-scope stuck takes no "
                               "'bit'")
            rec["value"] = e["value"]
        return rec

    def flash_add(self, value):
        """damage-add (grlib_stixflash.c:1547-1575): one runtime entry;
        a spec that is already active is rejected with the device's
        exact text (PL-11g's reapply tolerance matches on it), and so
        is the reserved ASW image area (:1245-1257 — the controller
        never sets allow-reserved)."""
        rec = self.flash_spec(value)
        blk = rec["block"] if "block" in rec else rec["page"] // 64
        if blk < 64:
            raise KeyError("stixflash: damage-add: block %d is in the "
                           "reserved ASW image area (blocks < 64)" % blk)
        if rec in self.flash_damage:
            raise KeyError("stixflash: damage-add: the spec is already "
                           "active")
        self.flash_damage.append(rec)

    def flash_remove(self, value):
        """damage-remove (grlib_stixflash.c:1585-1621): "all" or the
        exact spec; a spec that names nothing errors and changes
        nothing."""
        if value == "all":
            self.flash_damage = []
            return
        rec = self.flash_spec(value)
        if rec not in self.flash_damage:
            raise KeyError("stixflash: damage-remove: no such runtime "
                           "damage entry")
        self.flash_damage.remove(rec)

    def flash_list(self):
        """The damage-list string in the device's line format
        (stixflash_damage_list_get, grlib_stixflash.c:1429-1478); the
        fake has no sidecar rows, so src is always runtime."""
        rows = []
        for d in self.flash_damage:
            if d["kind"] == "erase-fail":
                rows.append("src=runtime kind=erase-fail block=%d"
                            % d["block"])
            elif d["kind"] == "program-fail":
                rows.append("src=runtime kind=program-fail page=%d"
                            % d["page"])
            elif "page" in d:
                rows.append("src=runtime kind=stuck value=%d page=%d "
                            "bit=%d.%d" % (d["value"], d["page"],
                                           d["byte"], d["bit"]))
            else:
                rows.append("src=runtime kind=stuck value=%d block=%d"
                            % (d["value"], d["block"]))
        return "\n".join(rows)

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
            if self.variant == "noflash" and a["path"] == FLASH:
                raise KeyError("Property '%s' not found" % prop)
            if prop == "vclock-ns":
                return self.vclock()
            if prop == "obt":
                return self.obt()
            if prop == "hw-state" and self.variant == "badstate":
                return '{"schema": 1, "vclock_ns": 5}'   # keys missing
            if prop == "hw-state":
                return json.dumps(self.hw_ctrl() if "ctrl" in a["path"]
                                  else self.hw_spw())
            if prop == "det-tape-status":
                return self.tape_status()
            if prop == "det-align-status":
                return self.align_status()
            if prop == "det-adc-status":
                return self.adc_status()
            if a["path"] == FLASH and prop in self.flash_ops:
                return self.flash_ops[prop]
            if a["path"] == FLASH and prop == "damage-list":
                return self.flash_list()
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
            if a["path"] == FLASH and prop in ("damage-add",
                                               "damage-remove"):
                # the flash device's runtime injection verbs (X-2,
                # write-only: no getter, so qom-get of them fails)
                if prop == "damage-add":
                    self.flash_add(a["value"])
                else:
                    self.flash_remove(a["value"])
                self.set_log.append((a["path"], prop, a["value"]))
                return {}
            if prop in SETTABLE:
                if a["path"] == EDAC and prop == "upset-inject":
                    self.upset_inject(a["value"])
                elif a["path"] == EDAC and prop == "upset-clear":
                    self.upset_clear(a["value"])
                elif a["path"] == ATT and prop in ("force.cmdsta-set",
                                                   "force.cmdsta-clear"):
                    # stix_att.c:886-908: a uint32, bits 11:5 only (the
                    # device's text otherwise); the masks apply to the
                    # readback as (value | set) & ~clear
                    v = a["value"]
                    if not isinstance(v, int) or isinstance(v, bool) or \
                            not 0 <= v <= 0xFFFFFFFF:
                        raise KeyError("%s: not a uint32: %r" % (prop, v))
                    if v & ~0xFE0:
                        raise KeyError("%s: 0x%x has bits outside 11:5"
                                       % (prop, v))
                    self.cmdsta_force["set" if prop.endswith("set")
                                      else "clear"] = v
                elif a["path"] == EDAC and prop == "edac-enable":
                    # stix_edac.c:1302-1352: "all-on" or a 3-bit mask
                    # string; only sets, never clears (the fake keeps no
                    # Edacon state: no hw-state field shows it)
                    s = a["value"]
                    if s != "all-on":
                        try:
                            m = int(s, 0)
                        except (TypeError, ValueError):
                            m = -1
                        if not 0 <= m <= 7:
                            raise KeyError(
                                "edac-enable: expected all-on or a 3-bit "
                                "mask (1 = sdram, 2 = cache, 4 = rotb), "
                                "not %s" % s)
                        if m == 0:
                            raise KeyError("edac-enable: an empty mask "
                                           "enables nothing")
                elif a["path"] == EDAC and prop == "random-seed":
                    # plain state, 0 = off (stix_edac.c:1460-1464)
                    v = a["value"]
                    if not isinstance(v, int) or isinstance(v, bool) or \
                            not 0 <= v <= 0xFFFFFFFF:
                        raise KeyError("random-seed: not a uint32: %r" % v)
                    self.random_seed = v
                elif a["path"] == EDAC and prop == "random-count":
                    self.upset_burst(a["value"])
                elif prop == "det-tape-load":
                    try:
                        tape = json.loads(a["value"])
                    except ValueError:
                        raise KeyError("det-tape-load: bad tape JSON")
                    self.tape_json = tape
                    steps = tape.get("steps") or []
                    self.tape.update({"state": "ready",
                                      "name": tape.get("name", ""),
                                      "step": 0, "n_steps": len(steps),
                                      "label": "", "kind": "",
                                      "loop": bool(tape.get("loop", False)),
                                      "align": bool(tape.get("align",
                                                             False))})
                elif prop == "det-tape-play":
                    if a["value"]:
                        if self.tape_json is None:
                            raise KeyError("det-tape-play: no tape")
                        steps = self.tape_json.get("steps") or []
                        self.tape.update({"state": "playing", "step": 1,
                                          "label": steps[0].get("label", "")
                                          if steps else "",
                                          "kind": steps[0].get("kind", "")
                                          if steps else "",
                                          "dropped": 0})  # the play start
                    elif self.tape["state"] == "playing":
                        self.tape["state"] = "stopped"
                elif prop == "det-align":
                    self.align["mode"] = "on" if a["value"] else "off"
                elif prop == "det-adc-mode":
                    self.adc["mode"] = bool(a["value"])
                elif prop == "det-source":
                    # the device refuses replay-dss without a table
                    # (stix_det.c:4078-4082); the fake never has one
                    if a["value"] == "replay-dss":
                        raise KeyError("det-source: replay-dss needs a "
                                       "replay table (set det-replay-file "
                                       "at startup)")
                elif prop == "det-replay-start":
                    # ditto for a start (stix_det.c:4446-4450, the
                    # device's text); a stop (false) is stored
                    if a["value"]:
                        raise KeyError("det-replay-start: no replay "
                                       "table (set det-replay-file at "
                                       "startup)")
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
        if ex == "x-fake-tape-state":
            # the tape_markers rig: move the hw-state's det.tape section
            # like a playing tape would (the model's own tick rules are
            # the real device's, qtest-covered)
            st = a.get("state")
            if not isinstance(st, dict):
                raise KeyError("x-fake-tape-state needs {state: {...}}")
            self.tape.update(st)
            return {}
        if ex == "x-fake-adc-state":
            # S2 (tapes_spec_play): the det.adc section (kev_full = K)
            st = a.get("state")
            if not isinstance(st, dict):
                raise KeyError("x-fake-adc-state needs {state: {...}}")
            self.adc.update(st)
            return {}
        if ex == "x-fake-align-state":
            # S2 (align_leaves): move the det.align section (the
            # preimage_miss gauge the model's spectrum build sets)
            st = a.get("state")
            if not isinstance(st, dict):
                raise KeyError("x-fake-align-state needs {state: {...}}")
            self.align.update(st)
            return {}
        if ex == "x-fake-guest-reset":
            threading.Timer(0.05, self.guest_reset).start()
            return {}
        if ex == "x-fake-flash-op":
            # the flash op counters move only here (the real device's
            # move in its MMIO handler, D-F9)
            op = a.get("op")
            if op not in ("read", "prog", "erase", "abort"):
                raise KeyError("x-fake-flash-op needs op "
                               "read|prog|erase|abort")
            self.flash_ops["ops-" + op] += 1
            return {}
        raise KeyError("The command %s has not been found" % ex)

    def guest_reset(self):
        # this fake keeps all props across the reset; the real device
        # drops link-down/adc-inject (stix_att.c:504-512 keeps only the
        # attenuator's faults), and the controller re-applies those.
        # The flash damage is runtime state and survives too
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
