# SPDX-License-Identifier: GPL-2.0-or-later
"""The fault catalogue (platform plan D-PLq), separate from forcing.

A named failure records the *intent*: an id from the JSON catalogue,
its resolved params and seed, and its expected detection, as one
`fault` record per inject and per clear. The action itself reuses the
platform switches and the forcing hooks, or (`qom`) sets a property of
a STIX device, for device-level faults such as the attenuator's, or
(`flash_damage`, PL-11g) drives the flash device's write-only
damage-add/damage-remove with the row's spec.

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
- apply returns the state it replaced, and undo restores exactly that;
- after a power cycle the active device-state faults are re-applied
  (PL-11a, D-PSa), with the prior of the first apply kept;
- `status()` reports an active lasting fault's `since_sim`, the bench
  time it turned active (UI plan D1).
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
           "keepalive_off", "adc", "qom", "flash_damage")
# the kinds reapply covers (D-PSa): device state the power cycle can
# lose. The controller-side kinds (link, timecodes, keepalive) and adc
# keep their own paths. flash_damage (PL-11g): a warm reset keeps the
# runtime damage, so the re-add is refused with FLASH_ALREADY_ACTIVE
# and the row logs as already-active, not a reapply-error
REAPPLY_KINDS = ("qom", "flash_damage")
CHANGES = ("truth", "presented")
# the qom action reaches the STIX devices only (stixctrl and its
# sub-models such as the attenuator, and stixspw)
QOM_PATH = re.compile(r"^/machine/(stixctrl[A-Za-z0-9_-]*"
                      r"(/[A-Za-z0-9_-]+)*|stixspw)$")
# the flash_damage action (PL-11g, D-PSe): the device's spec grammar is
# stixflash_dmg_entry (grlib_stixflash.c:1094-1298) — the linear block/
# page form only (the fake refuses mcm/chip too); the geometry is the
# device's (grlib_stixflash.c:33-43). Blocks < 64 are the reserved ASW
# image area (grlib_stixflash.c:145) and the controller never sets
# allow-reserved. The qom kind above still refuses the flash path.
FLASH_PATH = "/machine/stixflash"
FLASH_KEYS = ("kind", "block", "page", "bit", "value")
FLASH_KINDS = ("erase-fail", "program-fail", "stuck", "stuck0")
FLASH_PAGE_SIZE = 4224          # 4096 data + 128 header bytes
FLASH_PAGES_PER_BLOCK = 64
FLASH_BLOCKS = 65536            # 2 mcms * 8 devices * 4096 blocks
FLASH_PAGES = FLASH_BLOCKS * FLASH_PAGES_PER_BLOCK
# the device's exact duplicate-spec refusal (grlib_stixflash.c:1562):
# reapply matches on it (the owner decision, D-PSa)
FLASH_ALREADY_ACTIVE = "stixflash: damage-add: the spec is already active"
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
        # PL-11h (D-PSf): optional title and one-sentence description
        for k in ("label", "description"):
            _need(k not in f or (isinstance(f[k], str) and f[k]),
                  "%s: %s must be a non-empty string when present"
                  % (fid, k))
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
        if act["kind"] == "flash_damage":
            _need(act.get("path") == FLASH_PATH, "%s: flash_damage path "
                  "%r is not %r" % (fid, act.get("path"), FLASH_PATH))
            flash_spec_ok(fid, act.get("spec"), f["params"])
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


def _flash_leaf(fid, name, leaf, params, bounds):
    """One spec leaf: a fixed int within @bounds, or {"param": name} of
    an existing param (its integer typing is the params rule above)."""
    if isinstance(leaf, dict):
        _need(set(leaf) == {"param"} and isinstance(leaf["param"], str) and
              leaf["param"] in params, "%s: spec %s must reference an "
              "existing param as {\"param\": name}, got %r"
              % (fid, name, leaf))
        return
    _need(isinstance(leaf, int) and not isinstance(leaf, bool) and
          bounds[0] <= leaf <= bounds[1], "%s: spec %s is not an integer "
          "in %d..%d (or a param reference), got %r"
          % (fid, name, bounds[0], bounds[1], leaf))


def flash_spec_ok(fid, spec, params):
    """The strict shape check of a flash_damage spec (D-PSe, R2): the
    device's keys and kind enum, exactly one of block/page (the scope),
    erase-fail block-only, program-fail page-only, stuck with value
    (0/1; the stuck0 alias fixes it and takes none), a page-scope stuck
    with a {byte, bit} bit and a block-scope one without, and the fixed
    values within the device's geometry."""
    _need(isinstance(spec, dict), "%s: flash_damage spec is not an "
          "object" % fid)
    extra = sorted(set(spec) - set(FLASH_KEYS))
    _need(not extra, "%s: flash_damage spec has unknown keys %r"
          % (fid, extra))
    _need(spec.get("kind") in FLASH_KINDS, "%s: flash_damage kind %r is "
          "not one of %s" % (fid, spec.get("kind"), "|".join(FLASH_KINDS)))
    kind = spec["kind"]
    has_block, has_page = "block" in spec, "page" in spec
    _need(has_block != has_page, "%s: flash_damage needs exactly one of "
          "'block' and 'page' (the scope)" % fid)
    if kind == "erase-fail":
        _need(has_block, "%s: erase-fail is a block kind" % fid)
    elif kind == "program-fail":
        _need(has_page, "%s: program-fail is a page kind" % fid)
    else:                           # stuck / stuck0
        if kind == "stuck":
            _need("value" in spec, "%s: stuck needs 'value'" % fid)
            _flash_leaf(fid, "value", spec["value"], params, (0, 1))
        else:
            _need("value" not in spec, "%s: 'value' with the 'stuck0' "
                  "alias; use kind \"stuck\" with \"value\": 0" % fid)
        if has_page:
            bit = spec.get("bit")
            _need(isinstance(bit, dict) and set(bit) == {"byte", "bit"},
                  "%s: page-scope stuck needs 'bit' as "
                  "{\"byte\": ..., \"bit\": ...}" % fid)
            _flash_leaf(fid, "bit.byte", bit["byte"], params,
                        (0, FLASH_PAGE_SIZE - 1))
            _flash_leaf(fid, "bit.bit", bit["bit"], params, (0, 7))
        else:
            _need("bit" not in spec,
                  "%s: block-scope stuck takes no 'bit'" % fid)
    if kind in ("erase-fail", "program-fail"):
        _need("bit" not in spec and "value" not in spec,
              "%s: 'bit' and 'value' are stuck-only" % fid)
    if has_block:
        _flash_leaf(fid, "block", spec["block"], params,
                    (0, FLASH_BLOCKS - 1))
    else:
        _flash_leaf(fid, "page", spec["page"], params,
                    (0, FLASH_PAGES - 1))


def flash_build(spec, params):
    """A validated spec resolved against the fault's params (D-PSe):
    {"param": name} leaves become the values and the bit composes to
    the device's "byte.bit" string — the JSON object damage-add takes
    (grlib_stixflash.c:1507-1538)."""
    def val(v):
        return params[v["param"]] if isinstance(v, dict) else v
    out = dict(spec)
    if "bit" in out:
        out["bit"] = "%d.%d" % (val(spec["bit"]["byte"]),
                                val(spec["bit"]["bit"]))
    for k in ("block", "page", "value"):
        if k in out:
            out[k] = val(out[k])
    return out


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
                # since_sim: the bench time the fault became active —
                # an inject now, or the at_sim trigger firing (UI D1)
                self.active[entry["id"]].update(
                    pending=False, prior=prior,
                    since_sim=self.clock.now()[0])
        self.log.emit("api", "fault", "fault." + entry["id"],
                      {"action": "inject", "target": entry["target"],
                       "mode": entry["mode"], "params": params,
                       "seed": seed, "persistence": entry["persistence"],
                       "expected_detection": entry["expected_detection"]})

    def reapply(self, why):
        """Power-cycle re-apply (PL-11a, D-PSa): the device lost what it
        does not keep (a cold cycle is a new process; a warm reset keeps
        only what the device itself keeps), so the apply function of
        every active LASTING device-state fault runs again. Not _apply:
        the stored prior stays the one captured at the first apply
        (fault.clear must write the pre-fault value, not the faulted
        one) and the record says action reapply. A failed re-apply is
        logged (S3) and never fails the power-on; one-shots are never
        active and the other kinds have their own re-apply paths."""
        with self.lock:
            items = [(fid, st) for fid, st in self.active.items()
                     if not st.get("pending")]
        for fid, st in items:
            entry = self.cat[fid]
            if entry["persistence"] not in LASTING or \
                    entry["action"]["kind"] not in REAPPLY_KINDS:
                continue
            with self.lock:
                if self.active.get(fid) is not st:
                    continue        # cleared while we were walking
            try:
                apply_fn, _ = self.actions[entry["action"]["kind"]]
                apply_fn(entry, st["params"])
            except Exception as e:
                # PL-11g (the owner decision, D-PSa): a warm reset keeps
                # the flash device's runtime damage, so the re-add is
                # refused with its exact duplicate text — the row is
                # fine, log it as already-active, not an error
                if entry["action"]["kind"] == "flash_damage" and \
                        FLASH_ALREADY_ACTIVE in str(e):
                    self.log.emit("ctl", "fault", "fault." + fid,
                                  {"action": "already-active", "why": why,
                                   "params": st["params"],
                                   "seed": st["seed"]})
                    continue
                self.log.emit("ctl", "fault", "fault." + fid,
                              {"action": "reapply-error", "why": why,
                               "params": st["params"], "seed": st["seed"],
                               "error": "%s: %s"
                               % (type(e).__name__, e)})
                continue
            self.log.emit("ctl", "fault", "fault." + fid,
                          {"action": "reapply", "why": why,
                           "params": st["params"], "seed": st["seed"]})

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
