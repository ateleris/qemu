# SPDX-License-Identifier: GPL-2.0-or-later
"""The scenario runner (platform plan PL-5; survey §3.3, F' await/assert).

A scenario is JSON: setup actions, timed steps (a verb to run and the
checks it arms), teardown actions, and variants (the ESA FDIR test
classes: nominal, fault without recovery, fault with recovery) that
filter the steps by glob.

- **Time is sim time.** `at` is seconds from T0 (the boot: the sim time
  right after a `platform.power.boot` step, else the end of the setup);
  `after` is seconds from the start of the previous step that runs in
  this variant. Every timeout and window is sim time too, so a pause
  stops them.
- **Checks arm at their step** (before its verb runs, so its response
  counts) and are unordered within their window:
  `await_tm` (the probe grammar `APID:T:S[:hex]@timeout_s`),
  `await_event` (EID + parameter prefix), `await_uart` (regex),
  `await_state` (path + predicate), `expect_cuc` (the first TM of an
  APID against the OBT), `assert_no_tm` / `assert_no_event` (a forbid,
  armed until the end or for `for_s`), `assert_silence` (no TM unit in
  a window). `assert_consistent` arrives in PL-8: until then it is a
  scenario error, never a silent pass.
- **Verdicts:** each check and step is pass, fail, timeout or error,
  with the seq range as evidence, logged as `expect` records. The run
  is error if a step errs, fail if one fails or times out, else pass;
  exit codes 0 pass, 1 fail, 2 scenario error, 3 infrastructure.

The Runner talks to the world through a small ctx (now, last_seq,
records, command, state, read_obt, emit, sleep), so the self-test can
drive it with a scripted clock and record stream.
"""

import copy
import fnmatch
import json
import os
import re
import threading
import time

from . import probe_lib
from .dispatch import VerbError
from .qmp import QmpError

API = 1
NS = 1_000_000_000
DEFAULT_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "scenarios"))
EXIT = {"pass": 0, "fail": 1, "error": 2, "infra": 3}
NAME_RE = re.compile(r"^[A-Za-z0-9_.-]+$")
HEX_EID = re.compile(r"^[0-9a-fA-F]{4}$")
HEX = re.compile(r"^([0-9a-fA-F]{2})*$")
TOP_KEYS = {"api", "name", "description", "seed", "duration_s", "variants",
            "config", "variant_config", "setup", "steps", "teardown"}
STEP_KEYS = {"at", "after", "name", "note", "do", "args", "variants",
             "await_tm", "await_event", "await_uart", "await_state",
             "assert_no_tm", "assert_no_event", "assert_silence",
             "assert_consistent", "expect_cuc"}
CHECK_KEYS = ("await_tm", "await_event", "await_uart", "await_state",
              "assert_no_tm", "assert_no_event", "assert_silence",
              "expect_cuc")
CUC_KEYS = {"apid", "within_s", "outside_s", "timeout_s", "mod_s",
            "offset_s"}
# run mode's wall-clock ceiling: 3 x duration_s plus this (review S3)
BOOT_SLACK_S = 120
PREDICATES = ("eq", "ne", "lt", "gt", "within", "in_set", "changed",
              "stable_for")
# what a scenario may change in the controller config for its run
CONFIG_ALLOWED = {
    "qemu": {"timecode_source", "obt_epoch", "obt_continuity",
             "on_guest_reset"},
    "keepalive": None, "timesync": None,
    "gs": {"tc_during_link_down", "hold_max", "emulation"},
    "power": {"off_time_s"}, "clock": {"snapshot_s"}}


class ScenarioError(Exception):
    """A malformed scenario or a wrong variant: exit 2."""


def _need(cond, msg):
    if not cond:
        raise ScenarioError(msg)


def _num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def load(path):
    """A scenario file; whole-line "#" comments (the SPDX tag) are
    dropped, as in the fault catalogue and QEMU's QAPI files."""
    try:
        with open(path) as f:
            text = "".join(ln if not ln.lstrip().startswith("#") else "\n"
                           for ln in f)
        obj = json.loads(text, parse_constant=_no_constant)
    except (OSError, ValueError) as e:
        raise ScenarioError("scenario %s: %s" % (path, e))
    return validate(obj)


def _no_constant(name):
    raise ValueError("%s is not JSON" % name)


def _config_ok(over, where):
    _need(isinstance(over, dict), "%s is not an object" % where)
    for sec, keys in over.items():
        _need(sec in CONFIG_ALLOWED and isinstance(keys, dict),
              "%s.%s may not be set by a scenario" % (where, sec))
        allowed = CONFIG_ALLOWED[sec]
        for k in keys:
            _need(allowed is None or k in allowed,
                  "%s.%s.%s may not be set by a scenario" % (where, sec, k))


def _timeout(d, where):
    t = d.get("timeout_s")
    _need(_num(t) and t > 0, "%s needs timeout_s > 0" % where)
    return float(t)


def _tm_spec(spec, where, forbid=False):
    p = probe_lib.load()
    try:
        x = (p.Forbid if forbid else p.Expect)(spec)
    except ValueError as e:
        raise ScenarioError("%s: %s" % (where, e))
    # the probe's forbid @SECONDS window (A-4) is not the runner's: a
    # scenario forbid is windowed by {tm, for_s}, and the runner would
    # silently forbid for the whole run
    _need(not (forbid and x.window is not None),
          "%s: a forbid takes no @window; use {tm, for_s}" % where)


def _actions(lst, where):
    _need(isinstance(lst, list), "%s is not a list" % where)
    for i, a in enumerate(lst):
        _need(isinstance(a, dict) and set(a) <= {"do", "args"} and
              isinstance(a.get("do"), str), "%s[%d] is {do, args}"
              % (where, i))
        _need(not a["do"].startswith("scenario."), "%s[%d]: a scenario "
              "cannot run scenario verbs" % (where, i))
        _need(isinstance(a.get("args", {}), dict), "%s[%d].args is not "
              "an object" % (where, i))


def validate(obj):
    """The normalized scenario, or ScenarioError naming the problem."""
    _need(isinstance(obj, dict), "a scenario is a JSON object")
    _need(obj.get("api") == API, "scenario: \"api\": 1 is required")
    unknown = set(obj) - TOP_KEYS
    _need(not unknown, "unknown scenario keys %s" % sorted(unknown))
    _need(isinstance(obj.get("name"), str) and NAME_RE.match(obj["name"]),
          "name must match %s" % NAME_RE.pattern)
    _need(_num(obj.get("duration_s")) and obj["duration_s"] > 0,
          "duration_s must be > 0")
    sc = copy.deepcopy(obj)
    declared = "variants" in sc
    vs = sc.setdefault("variants", ["default"])
    _need(isinstance(vs, list) and vs and
          all(isinstance(v, str) and NAME_RE.match(v) for v in vs) and
          len(set(vs)) == len(vs), "variants must be distinct names")
    sc["variants_declared"] = declared
    _need(sc.get("seed") is None or (isinstance(sc["seed"], int) and
                                     not isinstance(sc["seed"], bool)),
          "seed is an integer")
    _config_ok(sc.setdefault("config", {}), "config")
    vc = sc.setdefault("variant_config", {})
    _need(isinstance(vc, dict), "variant_config is not an object")
    for v, over in vc.items():
        _need(v in vs, "variant_config.%s is not a variant" % v)
        _config_ok(over, "variant_config.%s" % v)
    for k in ("setup", "teardown"):
        _actions(sc.setdefault(k, []), k)
    steps = sc.get("steps")
    _need(isinstance(steps, list) and steps, "steps is a non-empty list")
    for i, st in enumerate(steps):
        _step(st, "steps[%d]" % i, vs)
    for v in vs:
        _need(any(k in st for st in steps if runs_in(st, v)
                  for k in CHECK_KEYS),
              "variant %r has no check: it could only pass vacuously" % v)
    return sc


def check_verbs(sc, known):
    """ScenarioError naming the first action whose verb is not in
    @known (the registry, verbs.VERBS, or a live dispatcher's)."""
    for a in sc["setup"] + sc["steps"] + sc["teardown"]:
        _need("do" not in a or a["do"] in known, "%s: no verb %r"
              % (sc["name"], a.get("do")))


def wall_ceiling(sc):
    """Run mode's wall-clock limit for one variant (monotonic seconds)."""
    return 3 * sc["duration_s"] + BOOT_SLACK_S


def _step(st, where, variants):
    _need(isinstance(st, dict), "%s is not an object" % where)
    unknown = set(st) - STEP_KEYS
    _need(not unknown, "%s: unknown keys %s" % (where, sorted(unknown)))
    _need(("at" in st) != ("after" in st), "%s needs exactly one of at, "
          "after" % where)
    t = st.get("at", st.get("after"))
    _need(_num(t) and t >= 0, "%s: at/after must be >= 0" % where)
    if "do" in st:
        _need(isinstance(st["do"], str), "%s.do is a verb name" % where)
        _need(not st["do"].startswith("scenario."), "%s: a scenario cannot "
              "run scenario verbs" % where)
        _need(isinstance(st.get("args", {}), dict), "%s.args is not an "
              "object" % where)
    if "variants" in st:
        g = st["variants"]
        _need(isinstance(g, list) and g and
              all(isinstance(x, str) for x in g), "%s.variants is a list "
              "of globs" % where)
        _need(any(fnmatch.fnmatchcase(v, x) for v in variants for x in g),
              "%s.variants %r match no variant of %r" % (where, g,
                                                         variants))
    if "assert_consistent" in st:
        raise ScenarioError("%s: assert_consistent arrives in PL-8 (the "
                            "truth-vs-telemetry decoder); until then it is "
                            "an error, never a silent pass" % where)
    for key in ("await_tm", "await_event", "await_uart", "await_state",
                "assert_no_tm", "assert_no_event", "expect_cuc"):
        if key in st:
            _need(isinstance(st[key], list) and st[key], "%s.%s is a "
                  "non-empty list" % (where, key))
    for i, s in enumerate(st.get("await_tm", [])):
        w = "%s.await_tm[%d]" % (where, i)
        if isinstance(s, str):
            _need("@" in s, "%s: %r needs @timeout_s" % (w, s))
            _tm_spec(s, w)
        else:
            _need(isinstance(s, dict) and isinstance(s.get("tm"), str),
                  "%s is a spec or {tm, timeout_s}" % w)
            _timeout(s, w)
            _tm_spec(s["tm"], w)
    for i, e in enumerate(st.get("await_event", [])):
        w = "%s.await_event[%d]" % (where, i)
        _event(e, w)
        _timeout(e, w)
    for i, e in enumerate(st.get("assert_no_event", [])):
        w = "%s.assert_no_event[%d]" % (where, i)
        _event(e, w)
        _for(e, w)
    for i, s in enumerate(st.get("assert_no_tm", [])):
        w = "%s.assert_no_tm[%d]" % (where, i)
        if isinstance(s, str):
            _tm_spec(s, w, forbid=True)
        else:
            _need(isinstance(s, dict) and isinstance(s.get("tm"), str),
                  "%s is a spec or {tm, for_s}" % w)
            _tm_spec(s["tm"], w, forbid=True)
            _for(s, w)
    for i, u in enumerate(st.get("await_uart", [])):
        w = "%s.await_uart[%d]" % (where, i)
        _need(isinstance(u, dict) and isinstance(u.get("re"), str),
              "%s needs re" % w)
        try:
            re.compile(u["re"])
        except re.error as e:
            raise ScenarioError("%s: %s" % (w, e))
        _timeout(u, w)
    for i, a in enumerate(st.get("await_state", [])):
        w = "%s.await_state[%d]" % (where, i)
        _need(isinstance(a, dict) and isinstance(a.get("path"), str),
              "%s needs path" % w)
        preds = [p for p in PREDICATES if p in a]
        _need(len(preds) == 1, "%s needs exactly one of %s" % (w,
                                                               PREDICATES))
        _need(a.get("layer", "truth") in ("truth", "presented"),
              "%s.layer is truth or presented" % w)
        p = preds[0]
        if p in ("lt", "gt", "stable_for"):
            _need(_num(a[p]), "%s.%s is a number" % (w, p))
        elif p == "within":
            _need(isinstance(a[p], list) and len(a[p]) == 2 and
                  all(_num(x) for x in a[p]), "%s.within is [value, tol]"
                  % w)
        elif p == "in_set":
            _need(isinstance(a[p], list), "%s.in_set is a list" % w)
        elif p == "changed":
            _need(a[p] is True, "%s.changed is true" % w)
        _timeout(a, w)
    for i, c in enumerate(st.get("expect_cuc", [])):
        w = "%s.expect_cuc[%d]" % (where, i)
        _need(isinstance(c, dict) and isinstance(c.get("apid"), str),
              "%s needs apid" % w)
        _need(set(c) <= CUC_KEYS, "%s: unknown keys %s"
              % (w, sorted(set(c) - CUC_KEYS)))
        _need("mod_s" not in c or (_num(c["mod_s"]) and c["mod_s"] > 0),
              "%s: mod_s > 0" % w)
        _need("offset_s" not in c or _num(c["offset_s"]),
              "%s: offset_s is a number" % w)
        try:
            int(c["apid"], 0)
        except ValueError:
            raise ScenarioError("%s: apid %r" % (w, c["apid"]))
        _need(("within_s" in c) != ("outside_s" in c), "%s needs exactly "
              "one of within_s, outside_s" % w)
        _need(_num(c.get("within_s", c.get("outside_s"))), "%s: a number"
              % w)
        _timeout(c, w)
    if "assert_silence" in st:
        s = st["assert_silence"]
        _need(isinstance(s, dict) and set(s) == {"after_s", "for_s"} and
              _num(s["after_s"]) and _num(s["for_s"]) and s["for_s"] > 0
              and s["after_s"] >= 0, "%s.assert_silence is {after_s, "
              "for_s}" % where)


def _event(e, w):
    _need(isinstance(e, dict) and isinstance(e.get("eid"), str) and
          HEX_EID.match(e["eid"]), "%s: eid is 4 hex digits" % w)
    _need(isinstance(e.get("data", ""), str) and
          HEX.match(e.get("data", "")), "%s: data is hex bytes" % w)


def _for(d, w):
    if "for_s" in d:
        _need(_num(d["for_s"]) and d["for_s"] > 0, "%s: for_s > 0" % w)


def runs_in(step, variant):
    g = step.get("variants")
    return g is None or any(fnmatch.fnmatchcase(variant, x) for x in g)


def exit_code(res):
    if res.get("infra"):
        return EXIT["infra"]
    return EXIT[{"pass": "pass", "fail": "fail"}.get(res["verdict"],
                                                     "error")]


# -- the checks ------------------------------------------------------------
def _unit(rec):
    return b"\x00\x00\x00\x00" + bytes.fromhex(rec["data"]["hex"])


class Check:
    kind = "?"

    def __init__(self, spec, ctx):
        self.spec = spec
        self.ctx = ctx
        self.verdict = None
        self.why = None
        self.ev = {"seq_from": None, "seq_to": None, "matched_seq": None}

    def arm(self, now, seq):
        self.start = now
        self.ev["seq_from"] = seq

    def final(self, verdict, why=None, seq=None, matched=None):
        if self.verdict is None:
            self.verdict = verdict
            self.why = why
            self.ev["seq_to"] = seq
            self.ev["matched_seq"] = matched

    def feed(self, rec, now):
        pass

    def tick(self, now, seq):
        pass

    def finish(self, aborted, seq):
        self.final("error", aborted or "the scenario ended first", seq)

    def report(self):
        return {"kind": self.kind, "spec": self.spec,
                "verdict": self.verdict, "why": self.why,
                "evidence": dict(self.ev)}


class Await(Check):
    """Pass on the first matching record before the deadline."""

    def __init__(self, spec, ctx, timeout_s):
        super().__init__(spec, ctx)
        self.timeout = int(timeout_s * NS)

    def match(self, rec):
        raise NotImplementedError

    def feed(self, rec, now):
        if rec["sim_time"] <= self.start + self.timeout and self.match(rec):
            self.final("pass", None, rec["seq"], rec["seq"])

    def tick(self, now, seq):
        if now > self.start + self.timeout:
            self.final("timeout", "no match in %.1f s" % (self.timeout / NS),
                       seq)

    def finish(self, aborted, seq):
        self.final("error" if aborted else "timeout",
                   aborted or "the scenario ended before a match", seq)


class AwaitTm(Await):
    kind = "await_tm"

    def __init__(self, spec, ctx):
        if isinstance(spec, str):
            body, t = spec.rsplit("@", 1)
            super().__init__(spec, ctx, float(t))
        else:
            body = spec["tm"]
            super().__init__(spec, ctx, spec["timeout_s"])
        self.exp = probe_lib.load().Expect(body)

    def match(self, rec):
        return rec["kind"] == "tm" and self.exp.matches(_unit(rec))


def _event_match(rec, eid, data):
    if rec["kind"] != "tm":
        return False
    d = rec["data"].get("decoded") or {}
    return d.get("apid") == 0x5A7 and d.get("type") == 5 and \
        d.get("eid") == eid and d.get("params", "").startswith(data)


class AwaitEvent(Await):
    kind = "await_event"

    def __init__(self, spec, ctx):
        super().__init__(spec, ctx, spec["timeout_s"])
        self.eid = int(spec["eid"], 16)
        self.data = spec.get("data", "").lower()

    def match(self, rec):
        return _event_match(rec, self.eid, self.data)


class AwaitUart(Await):
    kind = "await_uart"

    def __init__(self, spec, ctx):
        super().__init__(spec, ctx, spec["timeout_s"])
        self.re = re.compile(spec["re"])

    def match(self, rec):
        return rec["kind"] == "uart" and \
            self.re.search(rec["data"].get("line", "")) is not None


class AwaitState(Await):
    kind = "await_state"

    def __init__(self, spec, ctx):
        super().__init__(spec, ctx, spec["timeout_s"])
        self.pred = next(p for p in PREDICATES if p in spec)
        self.arg = spec[self.pred]
        self.layer = spec.get("layer", "truth")
        self.base = None
        self.since = None

    def feed(self, rec, now):
        pass                        # a state check polls, see tick

    def _value(self):
        leaf = self.ctx.state(self.spec["path"])
        return None if leaf is None else (True, leaf.get(self.layer))

    def arm(self, now, seq):
        super().arm(now, seq)
        v = self._value()
        self.base = v
        self.since = now

    def tick(self, now, seq):
        if self.verdict is not None:
            return
        v = self._value()
        if v is not None and self._holds(v[1], now, v):
            self.final("pass", None, seq)
            return
        super().tick(now, seq)

    def _holds(self, x, now, v):
        p, a = self.pred, self.arg
        try:
            if p == "eq":
                return x == a
            if p == "ne":
                return x != a
            if p == "lt":
                return x is not None and x < a
            if p == "gt":
                return x is not None and x > a
            if p == "within":
                return x is not None and abs(x - a[0]) <= a[1]
            if p == "in_set":
                return x in a
            if p == "changed":
                return self.base is not None and v != self.base
            if p == "stable_for":
                if v != self.base:
                    self.base, self.since = v, now
                    return False
                return now - self.since >= a * NS
        except TypeError:
            return False
        return False


class ExpectCuc(Await):
    """The first TM of the APID after arming: its 48-bit CUC against the
    platform OBT at its arrival."""
    kind = "expect_cuc"

    def __init__(self, spec, ctx):
        super().__init__(spec, ctx, spec["timeout_s"])
        self.apid = int(spec["apid"], 0)

    def feed(self, rec, now):
        if self.verdict is not None or rec["kind"] != "tm":
            return
        d = rec["data"].get("decoded") or {}
        if d.get("apid") != self.apid or "cuc" not in d:
            return
        if rec["sim_time"] > self.start + self.timeout:
            return
        try:
            obt, at = self.ctx.read_obt()
        except (VerbError, OSError, QmpError) as e:
            self.final("error", "no OBT: %s" % e, rec["seq"], rec["seq"])
            return
        obt_rec = obt + (rec["sim_time"] - at) * 65536 // NS
        diff = (d["cuc"] - obt_rec) / 65536 - self.spec.get("offset_s", 0)
        m = self.spec.get("mod_s")
        if m:
            diff = (diff + m / 2) % m - m / 2     # in [-m/2, m/2)
        self.ev["diff_s"] = diff
        if "within_s" in self.spec:
            ok = abs(diff) <= self.spec["within_s"] and not d["unsynced"]
        else:
            ok = abs(diff) >= self.spec["outside_s"]
        self.final("pass" if ok else "fail", None if ok else
                   "the CUC is %.3f s from the OBT" % diff, rec["seq"],
                   rec["seq"])


class Forbid(Check):
    """Fail on a matching TM after arming, until the end or for_s."""

    def __init__(self, spec, ctx, match, for_s):
        super().__init__(spec, ctx)
        self.match = match
        self.for_ns = None if for_s is None else int(for_s * NS)

    def feed(self, rec, now):
        if self.for_ns is not None and \
                rec["sim_time"] > self.start + self.for_ns:
            return
        if self.match(rec):
            self.final("fail", "a forbidden TM", rec["seq"], rec["seq"])

    def tick(self, now, seq):
        if self.for_ns is not None and now > self.start + self.for_ns:
            self.final("pass", None, seq)

    def finish(self, aborted, seq):
        self.final("error" if aborted else "pass", aborted, seq)


class Silence(Check):
    kind = "assert_silence"

    def arm(self, now, seq):
        super().arm(now, seq)
        self.lo = now + int(self.spec["after_s"] * NS)
        self.hi = self.lo + int(self.spec["for_s"] * NS)

    def feed(self, rec, now):
        if rec["kind"] == "tm" and self.lo <= rec["sim_time"] <= self.hi:
            self.final("fail", "a TM in the silence window", rec["seq"],
                       rec["seq"])

    def tick(self, now, seq):
        if now > self.hi:
            self.final("pass", None, seq)


def _checks(st, ctx):
    p = probe_lib.load()
    out = []
    for s in st.get("await_tm", []):
        out.append(AwaitTm(s, ctx))
    for s in st.get("await_event", []):
        out.append(AwaitEvent(s, ctx))
    for s in st.get("await_uart", []):
        out.append(AwaitUart(s, ctx))
    for s in st.get("await_state", []):
        out.append(AwaitState(s, ctx))
    for s in st.get("expect_cuc", []):
        out.append(ExpectCuc(s, ctx))
    for s in st.get("assert_no_tm", []):
        spec = s if isinstance(s, str) else s["tm"]
        f = p.Forbid(spec)
        c = Forbid(s, ctx, lambda rec, f=f: rec["kind"] == "tm" and
                   f.matches(_unit(rec)),
                   None if isinstance(s, str) else s.get("for_s"))
        c.kind = "assert_no_tm"
        out.append(c)
    for s in st.get("assert_no_event", []):
        eid, data = int(s["eid"], 16), s.get("data", "").lower()
        c = Forbid(s, ctx, lambda rec, e=eid, d=data:
                   _event_match(rec, e, d), s.get("for_s"))
        c.kind = "assert_no_event"
        out.append(c)
    if "assert_silence" in st:
        out.append(Silence(st["assert_silence"], ctx))
    return out


# -- the runner ------------------------------------------------------------
class Runner:
    def __init__(self, sc, variant, ctx, tick_s=0.05, wall_s=None):
        if variant is None:
            _need(not sc["variants_declared"], "%s has variants %r: pick "
                  "one" % (sc["name"], sc["variants"]))
            variant = sc["variants"][0]
        _need(variant in sc["variants"], "%s has no variant %r (%r)"
              % (sc["name"], variant, sc["variants"]))
        verbs = getattr(ctx, "verbs", None)
        if verbs is not None:
            check_verbs(sc, verbs())
        self.sc = sc
        self.variant = variant
        self.ctx = ctx
        self.tick_s = tick_s
        self.wall_s = wall_s
        self.mono = getattr(ctx, "mono", time.monotonic)
        self.aborted = None
        self.infra = None           # why the bench (not the FSW) failed
        self.verb_spans = []        # (seq before, seq after) of each verb
        self.steps = [dict(st, index=i) for i, st in enumerate(sc["steps"])
                      if runs_in(st, variant)]
        self.path = "scenario.%s" % sc["name"]
        self.lock = threading.Lock()
        self.state = {"name": sc["name"], "variant": variant,
                      "phase": "new", "step": None}

    def abort(self, why="aborted", infra=False):
        why = why or "aborted"
        if infra and self.infra is None:
            self.infra = why
        if self.aborted is None:
            self.aborted = why

    def _power(self):
        fn = getattr(self.ctx, "power_state", None)
        return fn() if fn is not None else None

    def _emit(self, kind, data, path=None):
        d = {"scenario": self.sc["name"], "variant": self.variant}
        d.update(data)
        self.ctx.emit(kind, path or self.path, d)

    def _do(self, verb, args, setup=False):
        """None, or (verdict, why, infra) of a failed verb. A 5xx is an
        infrastructure failure; in the setup a 409 is too (the bench
        could not be brought up: a refused binary, a run-dir check). In
        a step a 409 is infra when the bench is off, else the step's
        fail (the state the scenario expected is not there, review N1);
        400/403/404 stay the scenario's own error."""
        before = self.ctx.last_seq()
        try:
            self.ctx.command(verb, args or {})
        except VerbError as e:
            why = "%s: %s (%d)" % (verb, e, e.status)
            if e.status >= 500 or (setup and e.status == 409):
                return "error", why, True
            if e.status == 409:
                if self._power() == "off":
                    return "error", why, True
                return "fail", why, False
            return "error", why, False
        finally:
            self.verb_spans.append((before, self.ctx.last_seq()))
        return None

    def _own(self, seq):
        return any(lo < seq <= hi for lo, hi in self.verb_spans)

    def run(self):
        ctx, sc = self.ctx, self.sc
        res = {"api": API, "scenario": sc["name"], "variant": self.variant,
               "seed": sc.get("seed"), "verdict": None, "aborted": None,
               "infra": False, "steps": [], "setup_error": None}
        self.seq = ctx.last_seq()
        self.mono0 = self.mono()
        res["seq_from"] = self.seq
        self._emit("log", {"phase": "start"})
        self.state["phase"] = "setup"
        for a in sc["setup"]:
            err = self._do(a["do"], a.get("args"), setup=True)
            if err:
                res["setup_error"] = err[1]
                res["infra"] = res["infra"] or err[2]
                break
        self.t0 = ctx.now()
        open_checks = []
        reports = {st["index"]: {"index": st["index"],
                                 "name": st.get("name"),
                                 "do": st.get("do"), "verdict": None,
                                 "why": None, "started_sim": None,
                                 "checks": []} for st in self.steps}
        step_checks = {}
        nxt = 0
        prev_start = self.t0
        self.state["phase"] = "steps"
        while res["setup_error"] is None:
            if self.wall_s is not None and \
                    self.mono() - self.mono0 > self.wall_s:
                self.abort("the wall-clock ceiling of %.0f s passed (the "
                           "sim clock stalled?)" % self.wall_s, infra=True)
            if self.aborted:
                break
            now = ctx.now()
            self._drain(open_checks, now)
            for c in open_checks:
                c.tick(now, self.seq)
            # start due steps, in order
            while nxt < len(self.steps):
                st = self.steps[nxt]
                due = (self.t0 + int(st["at"] * NS) if "at" in st else
                       prev_start + int(st["after"] * NS))
                if now < due:
                    break
                nxt += 1
                prev_start = now
                rep = reports[st["index"]]
                rep["started_sim"] = now - self.t0
                self.state["step"] = st["index"]
                self._drain(open_checks, now)
                cs = _checks(st, ctx)
                for c in cs:
                    c.arm(now, self.seq)
                step_checks[st["index"]] = cs
                open_checks += cs
                if "do" in st:
                    err = self._do(st["do"], st.get("args"))
                    if err:
                        rep["verdict"], rep["why"] = err[0], err[1]
                        res["infra"] = res["infra"] or err[2]
                        for c in cs:
                            c.final("error", "the step's verb failed",
                                    self.seq)
                        if err[2]:
                            self.abort(err[1], infra=True)
                    elif st["do"] == "platform.power.boot":
                        self.t0 = ctx.now()
                        prev_start = self.t0
                        rep["started_sim"] = 0
                now = ctx.now()
            open_checks = [c for c in open_checks if c.verdict is None]
            self._close_steps(reports, step_checks)
            end = self.t0 + int(sc["duration_s"] * NS)
            if now >= end or (nxt == len(self.steps) and not open_checks):
                break
            ctx.sleep(self.tick_s)
        # the end: open checks, steps never started
        self._drain(open_checks, ctx.now())
        for c in open_checks:
            if c.verdict is None:
                c.finish(self.aborted, self.seq)
        for st in self.steps[nxt:]:
            rep = reports[st["index"]]
            rep["verdict"] = "error"
            rep["why"] = (self.aborted if self.aborted else
                          res["setup_error"] or
                          "not reached before duration_s")
        self._close_steps(reports, step_checks)
        self.state["phase"] = "teardown"
        for a in sc["teardown"]:
            err = self._do(a["do"], a.get("args"))
            if err and a["do"] == "platform.power.off" and \
                    self._power() == "off":
                continue            # already off (QEMU died): fine
            if err:
                res["teardown_error"] = err[1]
                res["infra"] = res["infra"] or err[2]
        res["aborted"] = self.aborted
        res["infra"] = res["infra"] or self.infra is not None
        res["steps"] = [reports[st["index"]] for st in self.steps]
        for rep in res["steps"]:
            rep.pop("_closed", None)
        vs = [r["verdict"] for r in res["steps"]]
        if res["setup_error"] or res.get("teardown_error") or \
                "error" in vs or self.aborted:
            res["verdict"] = "error"
        elif "fail" in vs or "timeout" in vs:
            res["verdict"] = "fail"
        else:
            res["verdict"] = "pass"
        res["t0"] = self.t0
        res["seq_to"] = ctx.last_seq()
        self._emit("expect", {"phase": "end", "verdict": res["verdict"],
                              "aborted": self.aborted,
                              "seq_from": res["seq_from"],
                              "steps": vs})
        self.state["phase"] = "done"
        self.state["verdict"] = res["verdict"]
        return res

    def _drain(self, open_checks, now):
        for rec in self.ctx.records(self.seq):
            self.seq = max(self.seq, rec["seq"])
            if rec["kind"] == "power" and \
                    rec["data"].get("event") == "off" and \
                    not self._own(rec["seq"]):
                # no step's verb did this: QEMU exited (review S1)
                self.abort("power off that no step caused: %s"
                           % rec["data"].get("why"), infra=True)
            for c in open_checks:
                if c.verdict is None:
                    c.feed(rec, now)

    def _close_steps(self, reports, step_checks):
        for idx, cs in step_checks.items():
            rep = reports[idx]
            if rep.get("_closed") or any(c.verdict is None for c in cs):
                continue
            rep["_closed"] = True
            rep["checks"] = [c.report() for c in cs]
            for c in cs:
                self._emit("expect", dict(c.report(), step=idx),
                           "%s.s%d" % (self.path, idx))
            if rep["verdict"] is None:
                vs = [c.verdict for c in cs]
                rep["verdict"] = ("fail" if "fail" in vs else
                                  "timeout" if "timeout" in vs else
                                  "error" if "error" in vs else "pass")
                bad = [c for c in cs if c.verdict != "pass"]
                if bad:
                    rep["why"] = "%s %s: %s" % (bad[0].kind,
                                                json.dumps(bad[0].spec),
                                                bad[0].why)
            self._emit("expect", {"step": idx, "verdict": rep["verdict"],
                                  "why": rep["why"]},
                       "%s.s%d" % (self.path, idx))


class CtlCtx:
    """The Runner's ctx on a live controller."""

    def __init__(self, ctl):
        self.ctl = ctl

    def now(self):
        return self.ctl.clock.exact_sim()

    def last_seq(self):
        return self.ctl.log.seq

    def records(self, since):
        return [r.obj for r in self.ctl.log.since(since)]

    def command(self, verb, args):
        return self.ctl.command(verb, args, source="scn")

    def state(self, path):
        v = self.ctl.state.get(path)
        return v if isinstance(v, dict) and "truth" in v else None

    def read_obt(self):
        return self.ctl.plat.read_obt()

    def emit(self, kind, path, data):
        return self.ctl.log.emit("scn", kind, path, data)

    def verbs(self):
        return set(self.ctl.dispatcher.verbs)

    def power_state(self):
        return self.ctl.power.state

    def sleep(self, s):
        time.sleep(s)
