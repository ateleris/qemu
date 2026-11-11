# SPDX-License-Identifier: GPL-2.0-or-later
"""The fault catalogue (platform plan D-PLq), separate from forcing.

A named failure records the *intent*: an id from the JSON catalogue,
its resolved params and seed, and its expected detection, as one
`fault` record per inject and per clear. The action itself reuses the
platform switches and the forcing hooks; a fault changes `presented`
only.
"""

import json
import os
import random
import threading

from .dispatch import VerbError

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CATALOGUE = os.path.normpath(os.path.join(
    HERE, "..", "catalogue", "faults-v1.json"))
CLASSES = ("component", "signal", "link", "memory", "timing")
PERSISTENCE = ("transient", "until_cleared", "permanent", "until_write")
ACTIONS = ("link_down", "timecodes_off", "timecode_inject", "obt_jump",
           "keepalive_off", "adc")
TRIGGERS = ("now", "at_sim")
REQUIRED = ("id", "class", "target", "mode", "params", "persistence",
            "reversible", "action", "preconditions", "expected_detection")


class CatalogueError(Exception):
    pass


def validate(cat):
    """The schema check of a catalogue (start-up and self-test)."""
    if not isinstance(cat, dict) or cat.get("api") != 1:
        raise CatalogueError("catalogue: \"api\": 1 is required")
    seen = set()
    for f in cat.get("faults", []):
        for k in REQUIRED:
            if k not in f:
                raise CatalogueError("fault %r lacks %r" % (f.get("id"), k))
        fid = f["id"]
        if fid in seen:
            raise CatalogueError("duplicate fault id %r" % fid)
        seen.add(fid)
        if f["class"] not in CLASSES:
            raise CatalogueError("%s: class %r" % (fid, f["class"]))
        if f["persistence"] not in PERSISTENCE:
            raise CatalogueError("%s: persistence %r"
                                 % (fid, f["persistence"]))
        if f["action"].get("kind") not in ACTIONS:
            raise CatalogueError("%s: action %r" % (fid, f["action"]))
        for name, p in f["params"].items():
            if p.get("type") != "integer" or "default" not in p:
                raise CatalogueError("%s: param %s needs type integer and "
                                     "a default" % (fid, name))
        for d in f["expected_detection"]:
            if "tm" not in d or "within_s" not in d:
                raise CatalogueError("%s: expected_detection needs tm and "
                                     "within_s" % fid)
    return cat


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
        """@actions: {kind: (apply(entry, params), undo(entry, params))}."""
        self.cat = {f["id"]: f for f in cat["faults"]}
        self.log = log
        self.clock = clock
        self.actions = actions
        self.active = {}
        self.lock = threading.Lock()

    def catalogue(self):
        return {"catalogue": list(self.cat.values()),
                "active": self.status()}

    def _params(self, entry, given):
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

    def inject(self, id, params=None, trigger=None, seed=None):
        entry = self.cat.get(id)
        if entry is None:
            raise VerbError(404, "no fault %r in the catalogue" % id)
        params = self._params(entry, params or {})
        trigger = trigger or {"at": "now"}
        if trigger.get("at", "now") not in TRIGGERS:
            raise VerbError(400, "trigger.at is now or at_sim")
        if seed is None:
            seed = random.SystemRandom().randrange(1 << 31)
        with self.lock:
            if id in self.active:
                raise VerbError(409, "fault %s is already active" % id)
            if entry["persistence"] != "transient":
                self.active[id] = {"params": params, "seed": seed,
                                   "pending": True}
        if trigger.get("at") == "at_sim":
            t = trigger.get("sim_time")
            if not isinstance(t, int):
                raise VerbError(400, "trigger.sim_time (ns) is required")
            self.clock.at(t, lambda: self._apply(entry, params, seed))
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

    def _apply(self, entry, params, seed):
        apply_fn, _ = self.actions[entry["action"]["kind"]]
        apply_fn(entry, params)
        with self.lock:
            if entry["id"] in self.active:
                self.active[entry["id"]]["pending"] = False
        self.log.emit("api", "fault", "fault." + entry["id"],
                      {"action": "inject", "target": entry["target"],
                       "mode": entry["mode"], "params": params,
                       "seed": seed,
                       "expected_detection": entry["expected_detection"]})

    def clear(self, id):
        entry = self.cat.get(id)
        if entry is None:
            raise VerbError(404, "no fault %r in the catalogue" % id)
        if not entry["reversible"]:
            raise VerbError(409, "fault %s is not reversible" % id)
        with self.lock:
            st = self.active.pop(id, None)
        if st is None:
            raise VerbError(409, "fault %s is not active" % id)
        _, undo = self.actions[entry["action"]["kind"]]
        undo(entry, st["params"])
        self.log.emit("api", "fault", "fault." + id,
                      {"action": "clear", "params": st["params"],
                       "seed": st["seed"]})
        return {"id": id}

    def status(self):
        with self.lock:
            return {k: dict(v) for k, v in self.active.items()}

    def active_targets(self):
        """{target path: [fault ids]} for the state tree (D-PLo)."""
        with self.lock:
            out = {}
            for fid in self.active:
                out.setdefault(self.cat[fid]["target"], []).append(fid)
            return out
