# SPDX-License-Identifier: GPL-2.0-or-later
"""The fault catalogue (platform plan D-PLq), separate from forcing.

A named failure records the *intent*: an id from the JSON catalogue,
its resolved params and seed, and its expected detection, as one
`fault` record per inject and per clear. The action itself reuses the
platform switches and the forcing hooks, or (`qom`) sets a property of
a STIX device, for device-level faults such as the attenuator's.

What a fault changes (the attenuator plan's amendment to D-PLo, PL-4
review SF8): a fault that models a physical failure may change *truth*
(a dead motor stops the mechanism; a disconnected link carries
nothing), a signal fault changes *presented* only (a sensor reads
wrong). Each entry declares it as `changes: truth|presented`; either
way the item shows as `fault` through `fault_ids` (D-PLp), never as a
UI-made truth change.

Lifecycle (PL-4 review SF4):
- everything is validated before anything is marked;
- `at_sim` arms the fault; `fault.clear` disarms an armed fault (also a
  non-reversible one, which has not happened yet);
- an apply that fails at its trigger (in the poller) is a `fault`
  record with the error, and leaves nothing armed or active;
- `until_cleared` and `until_write` faults are *active* until cleared;
  `transient` and `permanent` ones are one-shot events, never active,
  and may be injected again;
- apply returns the state it replaced, and undo restores exactly that.
"""

import json
import os
import random
import re
import threading

from .dispatch import VerbError

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CATALOGUE = os.path.normpath(os.path.join(
    HERE, "..", "catalogue", "faults-v1.json"))
CLASSES = ("component", "signal", "link", "memory", "timing")
PERSISTENCE = ("transient", "until_cleared", "permanent", "until_write")
LASTING = ("until_cleared", "until_write")
ACTIONS = ("link_down", "timecodes_off", "timecode_inject", "obt_jump",
           "keepalive_off", "adc", "qom")
CHANGES = ("truth", "presented")
# the qom action reaches the STIX devices only (stixctrl and its
# sub-models such as the attenuator, and stixspw)
QOM_PATH = re.compile(r"^/machine/(stixctrl[A-Za-z0-9_-]*"
                      r"(/[A-Za-z0-9_-]+)*|stixspw)$")
# the qom value rides verbatim: an integer for the forcing-style
# properties, a string for the EDAC upset verbs (upset-inject, M-6a of
# the memory-upset plan)
TRIGGERS = ("now", "at_sim")
REQUIRED = ("id", "class", "changes", "target", "mode", "params",
            "persistence", "reversible", "action", "preconditions",
            "expected_detection")
ADC_TARGET = re.compile(r"^instrument\.hk_adc\.(\d+)$")
HK_CHANNELS = 128


class CatalogueError(Exception):
    pass


def _need(cond, msg):
    if not cond:
        raise CatalogueError(msg)


def validate(cat):
    """The schema check of a catalogue (start-up and self-test); every
    problem, also a wrong type, is a CatalogueError naming the entry."""
    _need(isinstance(cat, dict) and cat.get("api") == 1,
          "catalogue: \"api\": 1 is required")
    faults = cat.get("faults", [])
    _need(isinstance(faults, list), "catalogue: \"faults\" is not a list")
    seen = set()
    for i, f in enumerate(faults):
        _need(isinstance(f, dict), "fault #%d is not an object" % i)
        for k in REQUIRED:
            _need(k in f, "fault %r lacks %r" % (f.get("id"), k))
        fid = f["id"]
        _need(isinstance(fid, str) and fid, "fault #%d: id is not a "
              "string" % i)
        _need(fid not in seen, "duplicate fault id %r" % fid)
        seen.add(fid)
        _need(f["class"] in CLASSES, "%s: class %r" % (fid, f["class"]))
        _need(f["changes"] in CHANGES, "%s: changes %r is not truth or "
              "presented" % (fid, f["changes"]))
        _need(f["persistence"] in PERSISTENCE, "%s: persistence %r"
              % (fid, f["persistence"]))
        _need(isinstance(f["reversible"], bool), "%s: reversible is not "
              "a boolean" % fid)
        _need(isinstance(f["target"], str), "%s: target is not a string"
              % fid)
        act = f["action"]
        _need(isinstance(act, dict) and act.get("kind") in ACTIONS,
              "%s: action %r" % (fid, act))
        _need(isinstance(f["params"], dict), "%s: params is not an object"
              % fid)
        for name, p in f["params"].items():
            _need(isinstance(p, dict) and p.get("type") == "integer" and
                  isinstance(p.get("default"), int), "%s: param %s needs "
                  "type integer and an integer default" % (fid, name))
        _need(isinstance(f["preconditions"], list), "%s: preconditions "
              "is not a list" % fid)
        _need(isinstance(f["expected_detection"], list), "%s: "
              "expected_detection is not a list" % fid)
        for d in f["expected_detection"]:
            _need(isinstance(d, dict) and "tm" in d and "within_s" in d,
                  "%s: expected_detection needs tm and within_s" % fid)
        if act["kind"] == "qom":
            _need(qom_path_ok(act.get("path")), "%s: qom path %r is not "
                  "a STIX device (/machine/stixctrl*, /machine/stixspw)"
                  % (fid, act.get("path")))
            _need(isinstance(act.get("property"), str) and
                  act["property"], "%s: qom property is not a string" % fid)
            _need(act.get("param") in f["params"] or
                  (isinstance(act.get("value"), (int, str)) and
                   "param" not in act),
                  "%s: qom needs a param of the entry or a value" % fid)
            _need(isinstance(act.get("clear", 0), int), "%s: qom clear "
                  "is not an integer" % fid)
        if act["kind"] == "adc":
            ch = act.get("ch")
            _need(isinstance(ch, int) and 0 <= ch < HK_CHANNELS,
                  "%s: action ch %r is not 0..%d" % (fid, ch,
                                                     HK_CHANNELS - 1))
            m = ADC_TARGET.match(f["target"])
            _need(m and int(m.group(1)) == ch, "%s: target %r is not "
                  "instrument.hk_adc.%d (the action's ch)"
                  % (fid, f["target"], ch))
    return cat


def qom_path_ok(path):
    return isinstance(path, str) and QOM_PATH.match(path) is not None


def load(path=None):
    """A JSON catalogue; whole-line "#" comments (the SPDX tag) are
    dropped, as in QEMU's QAPI files."""
    with open(path or DEFAULT_CATALOGUE) as f:
        text = "".join(ln if not ln.lstrip().startswith("#") else "\n"
                       for ln in f)
        try:
            cat = json.loads(text, parse_constant=_no_constant)
        except ValueError as e:
            raise CatalogueError("catalogue %s: %s" % (path, e))
    return validate(cat)


def _no_constant(name):
    raise ValueError("%s is not JSON" % name)


class Faults:
    def __init__(self, cat, log, clock, actions):
        """@actions: {kind: (apply(entry, params) -> prior state,
        undo(entry, params, prior))}."""
        self.cat = {f["id"]: f for f in cat["faults"]}
        self.log = log
        self.clock = clock
        self.actions = actions
        self.active = {}            # lasting faults, applied
        self.armed = {}             # at_sim faults, not yet applied
        self.token = 0
        self.lock = threading.Lock()

    def catalogue(self):
        return {"catalogue": list(self.cat.values()),
                "active": self.status()}

    def _params(self, entry, given):
        if not isinstance(given, dict):
            raise VerbError(400, "params must be an object")
        out = {}
        for k in given:
            if k not in entry["params"]:
                raise VerbError(400, "%s has no param %r" % (entry["id"], k))
        for name, spec in entry["params"].items():
            v = given.get(name, spec["default"])
            if isinstance(v, bool) or not isinstance(v, int) or \
                    not spec.get("min", v) <= v <= spec.get("max", v):
                raise VerbError(400, "%s: %s = %r is out of range"
                                % (entry["id"], name, v))
            out[name] = v
        return out

    def _trigger(self, trigger):
        trigger = trigger or {"at": "now"}
        if not isinstance(trigger, dict) or \
                trigger.get("at", "now") not in TRIGGERS:
            raise VerbError(400, "trigger.at is now or at_sim")
        if trigger.get("at") == "at_sim":
            t = trigger.get("sim_time")
            if isinstance(t, bool) or not isinstance(t, int) or t < 0:
                raise VerbError(400, "trigger.sim_time (ns) is required")
        return trigger

    def inject(self, id, params=None, trigger=None, seed=None):
        entry = self.cat.get(id)
        if entry is None:
            raise VerbError(404, "no fault %r in the catalogue" % id)
        params = self._params(entry, params or {})
        trigger = self._trigger(trigger)
        if seed is None:
            seed = random.SystemRandom().randrange(1 << 31)
        lasting = entry["persistence"] in LASTING
        with self.lock:
            if id in self.armed:
                raise VerbError(409, "fault %s is already armed" % id)
            if lasting and id in self.active:
                raise VerbError(409, "fault %s is already active" % id)
            if trigger.get("at") == "at_sim":
                self.token += 1
                tok = self.token
                self.armed[id] = {"params": params, "seed": seed,
                                  "trigger": trigger, "token": tok}
            elif lasting:
                # held while applying, so a concurrent inject gets 409
                self.active[id] = {"params": params, "seed": seed,
                                   "pending": True}
        if trigger.get("at") == "at_sim":
            self.clock.at(trigger["sim_time"],
                          lambda: self._fire(entry, params, seed, tok))
            self.log.emit("api", "fault", "fault." + id,
                          {"action": "armed", "params": params,
                           "seed": seed, "trigger": trigger})
            return {"id": id, "armed": True, "seed": seed}
        try:
            self._apply(entry, params, seed)
        except Exception:
            with self.lock:
                self.active.pop(id, None)
            raise
        return {"id": id, "params": params, "seed": seed}

    def _fire(self, entry, params, seed, tok):
        """The at_sim trigger (the poller thread)."""
        fid = entry["id"]
        with self.lock:
            a = self.armed.get(fid)
            if a is None or a["token"] != tok:
                return              # disarmed (or re-armed)
            del self.armed[fid]
            if entry["persistence"] in LASTING:
                self.active[fid] = {"params": params, "seed": seed,
                                    "pending": True}
        try:
            self._apply(entry, params, seed)
        except Exception as e:
            with self.lock:
                self.active.pop(fid, None)
            self.log.emit("ctl", "fault", "fault." + fid,
                          {"action": "error", "params": params,
                           "seed": seed, "error": "%s: %s"
                           % (type(e).__name__, e)})

    def _apply(self, entry, params, seed):
        apply_fn, _ = self.actions[entry["action"]["kind"]]
        prior = apply_fn(entry, params)
        with self.lock:
            if entry["id"] in self.active:
                self.active[entry["id"]].update(pending=False, prior=prior)
        self.log.emit("api", "fault", "fault." + entry["id"],
                      {"action": "inject", "target": entry["target"],
                       "mode": entry["mode"], "params": params,
                       "seed": seed, "persistence": entry["persistence"],
                       "expected_detection": entry["expected_detection"]})

    def clear(self, id):
        entry = self.cat.get(id)
        if entry is None:
            raise VerbError(404, "no fault %r in the catalogue" % id)
        with self.lock:
            armed = self.armed.pop(id, None)
        if armed is not None:
            self.log.emit("api", "fault", "fault." + id,
                          {"action": "disarm", "params": armed["params"],
                           "seed": armed["seed"]})
            return {"id": id, "disarmed": True}
        if entry["persistence"] not in LASTING or not entry["reversible"]:
            raise VerbError(409, "fault %s is a one-shot event or not "
                            "reversible: nothing to clear" % id)
        with self.lock:
            st = self.active.get(id)
            if st is None or st.get("pending"):
                raise VerbError(409, "fault %s is not active" % id)
            del self.active[id]
        _, undo = self.actions[entry["action"]["kind"]]
        undo(entry, st["params"], st.get("prior"))
        self.log.emit("api", "fault", "fault." + id,
                      {"action": "clear", "params": st["params"],
                       "seed": st["seed"]})
        return {"id": id}

    def status(self):
        with self.lock:
            out = {k: {kk: vv for kk, vv in v.items() if kk != "prior"}
                   for k, v in self.active.items()}
            for k, v in self.armed.items():
                out[k] = {"params": v["params"], "seed": v["seed"],
                          "pending": True, "trigger": v["trigger"]}
            return out

    def active_targets(self):
        """{target path: [fault ids]} for the state tree (D-PLo)."""
        with self.lock:
            out = {}
            for fid in self.active:
                out.setdefault(self.cat[fid]["target"], []).append(fid)
            return out
