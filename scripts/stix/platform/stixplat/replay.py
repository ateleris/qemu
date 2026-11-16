# SPDX-License-Identifier: GPL-2.0-or-later
"""Run bundles S2: replay a bundle on guest time (2026-11-15 replay
plan). P1 is the input loader: the origin's log slice, in record
order, through bundle.classify, every classified input tagged with its
seq, sim_time, boot_id and the origin's power phase. The origin is
read-only: the loader opens its files, never writes. P2 adds the
scheduler and the re-feed table around it, P3 the CLI.
"""

import gzip
import json
import os
import queue
import re
import threading

from . import bundle as bundle_mod
from . import log as log_mod
from .dispatch import VerbError

EXIT = {"ok": 0, "refused": 2, "infra": 3}

_PART_RE = re.compile(r"log\.jsonl\.(\d+)\.gz")


class ReplayError(Exception):
    """A refusal in the pre-flight: exit 2."""


def _parts(run_dir):
    """The log parts of a run dir, oldest first (the viewrun.parts
    order, kept local so the loader never pulls in the view stack)."""
    nums = []
    try:
        for fn in os.listdir(run_dir):
            m = _PART_RE.fullmatch(fn)
            if m:
                nums.append(int(m.group(1)))
    except OSError:
        pass
    return [os.path.join(run_dir, "log.jsonl.%d.gz" % n)
            for n in sorted(nums, reverse=True)] + \
        [os.path.join(run_dir, "log.jsonl")]


def _slice(run_dir, manifest):
    """(paths, offset): the session's slice, oldest first —
    log.jsonl.<log_rotations>.gz from start_offset, then every newer
    part, then log.jsonl."""
    rot = manifest.get("log_rotations") or 0
    paths = _parts(run_dir)
    if rot > 0:
        first = os.path.join(run_dir, "log.jsonl.%d.gz" % rot)
        if first not in paths:
            raise ReplayError("the bundle's log slice has rotated away "
                              "(log_rotations %d)" % rot)
        paths = paths[paths.index(first):]
    else:
        if not os.path.isfile(paths[-1]):
            raise ReplayError("the bundle has no log.jsonl in %s"
                              % run_dir)
        paths = paths[-1:]
    return paths, manifest.get("start_offset") or 0


def _lines(paths, offset):
    """The slice's lines; the offset (a file size, so a line boundary)
    skips the whole lines before the session's first record."""
    for i, path in enumerate(paths):
        skip = offset if i == 0 else 0
        opener = gzip.open if path.endswith(".gz") else open
        with opener(path, "rb") as f:
            for line in f:
                if skip >= len(line):
                    skip -= len(line)
                    continue
                yield line


def _phase_next(phase, obj):
    """The power phase after one record: a power event moves it
    (on -> frozen, boot -> running, off -> off), so does the
    sim.pause / sim.resume cmd (R8: a QMP STOP or HALT leaves no
    record and no input, so placement stays correct)."""
    kind = obj.get("kind")
    if kind == "power":
        ev = (obj.get("data") or {}).get("event")
        if ev == "on":
            return "frozen"
        if ev == "boot":
            return "running"
        if ev == "off":
            return "off"
    if kind == "cmd":
        if obj.get("path") == "sim.pause":
            return "paused"
        if obj.get("path") == "sim.resume":
            return "running"
    return phase


def load_inputs(run_dir, manifest):
    """[(input dict)] in seq order: the manifest's log slice
    (log.jsonl.<log_rotations>.gz from start_offset, then the newer
    parts, then log.jsonl — viewrun.parts' order), each record fed
    through bundle.classify(obj, bundle.new_state(hk_codes)) with the
    origin manifest config's [bundle] hk_codes; each classified input
    gains seq, sim_time, boot_id (from its record) and phase
    ("off"|"frozen"|"running"|"paused" — the origin's power state at
    that record). Raises ReplayError when the manifest is still open,
    has no t0, or the slice's first part is gone (rotated past
    [log] max_files - 1). Broken lines are skipped (the viewrun._stream
    rule). Returns every classified input, the replay != "guest" ones
    included (the scheduler emits their replay.skipped)."""
    if not isinstance(manifest, dict):
        raise ReplayError("no run bundle manifest at %r" % (run_dir,))
    if manifest.get("open"):
        raise ReplayError("the bundle is still open")
    if manifest.get("t0") is None:
        raise ReplayError("the bundle has no boot record (t0)")
    cfg = manifest.get("config")
    cfg = cfg if isinstance(cfg, dict) else {}
    bun = cfg.get("bundle")
    bun = bun if isinstance(bun, dict) else {}
    state = bundle_mod.new_state(bun.get("hk_codes") or ())
    out = []
    phase = "off"
    for line in _lines(*_slice(run_dir, manifest)):
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
            log_mod.check_record(obj)
        except ValueError:
            continue
        inp = bundle_mod.classify(obj, state)
        if inp is not None:
            if "seq" in inp:    # the null-hex TC(9,129)'s own TC seq
                inp["tc_seq"] = inp.pop("seq")
            inp["seq"] = obj["seq"]     # the record's seq
            inp["sim_time"] = obj["sim_time"]
            inp["boot_id"] = obj["boot_id"]
            inp["phase"] = phase
            out.append(inp)
        phase = _phase_next(phase, obj)
    return out


# -- the re-feed table (P2): one function per input kind -----------------

_WALL_WHY = "a real GS TC: wall-paced, not replayable"

# a clock.at deadline fires only once a vclock sample (20 Hz) has passed
# it, so every fire is a little past its sim_time; only a delay beyond
# this is a replay.late (5 sample periods)
_LATE_TOL_NS = 250 * 10**6


def _note(ctl, path, data):
    """One replay marker: a kind "log" record, source "ctl" (the
    controller's own records; classify returns None for kind "log", so
    a marker is never an input or a decision)."""
    ctl.log.emit("ctl", "log", path, data)


def _failed(ctl, inp, e):
    """One replay.failed record for a refused re-feed; never raises."""
    _note(ctl, "replay.failed",
          {"seq": inp["seq"], "status": e.status, "error": str(e)})
    return False


def _feed_verb(ctl, inp):
    """The recorded verb with its recorded args, through the real
    Dispatcher (the path a scenario step takes)."""
    try:
        ctl.command(inp["verb"], inp["args"], source="scn")
    except VerbError as e:
        return _failed(ctl, inp, e)
    return True


def _feed_tc(ctl, inp):
    """The recorded TC bytes via the relay (source "scn", never "tc" —
    that is the real-GS marker classify keys on), or the null-hex
    TC(9,129) as platform.timesync.send with the recorded coarse."""
    try:
        if inp["hex"] is None:
            ctl.command("platform.timesync.send",
                        {"coarse": inp["coarse"]}, source="scn")
        else:
            # delivered false (the link-down policy re-applied) is the
            # origin's own outcome: not a failure
            ctl.plat.relay.gs_tc(bytes.fromhex(inp["hex"]),
                                 source="scn", path="tc.replay")
    except VerbError as e:
        return _failed(ctl, inp, e)
    return True


def _feed_fault(ctl, inp):
    """The recorded params + seed + trigger verbatim; a clear input
    re-runs fault.clear."""
    try:
        if inp.get("action") == "clear":
            ctl.command("fault.clear", {"id": inp["id"]}, source="scn")
        else:
            ctl.command("fault.inject",
                        {"id": inp["id"], "params": inp["params"],
                         "seed": inp["seed"], "trigger": inp["trigger"]},
                        source="scn")
    except VerbError as e:
        return _failed(ctl, inp, e)
    return True


def _feed_force(ctl, inp):
    """A value present is force.set, absent force.clear."""
    try:
        if "value" in inp:
            ctl.command("force.set", {"path": inp["path"],
                                      "value": inp["value"]},
                        source="scn")
        else:
            ctl.command("force.clear", {"path": inp["path"]},
                        source="scn")
    except VerbError as e:
        return _failed(ctl, inp, e)
    return True


_FEED = {"verb": _feed_verb, "tc": _feed_tc, "fault": _feed_fault,
         "force": _feed_force}


# -- the scheduler (P2) ---------------------------------------------------

def _skip_why(inp):
    """replay.skipped's why: the input's own, or the wall rule's."""
    if inp.get("why"):
        return inp["why"]
    return _WALL_WHY if inp.get("replay") == "wall" else "no replay rule"


class Scheduler:
    """Re-feeds the inputs on guest time, in seq order: a daemon cursor
    thread (waking at most every 50 ms, the clock.wake_in cap) and one
    dispatch-queue thread running _FEED, so a slow verb never blocks
    the poller that fires the clock.at deadlines.

    Off/frozen/paused-phase inputs dispatch when due (clock.now() past
    sim_time) and the replay's power state matches the phase, else they
    hold at the head in order. Paused joins off/frozen (a plan
    deviation): clock.at never fires while paused, so an armed
    sim.resume would never fire; guest time is frozen in a pause, so
    the state match alone places it. Running-phase inputs are armed
    exactly once on clock.at; a deadline fired more than _LATE_TOL_NS
    past its sim_time gets one replay.late record first (owner
    question 1a). Inputs
    with replay != "guest" never dispatch: one replay.skipped as the
    cursor passes them. done is set once every input was dispatched or
    skipped and every armed one has fired.
    """

    def __init__(self, ctl, inputs):
        self.ctl = ctl
        self.inputs = inputs
        self.counts = {"inputs": len(inputs), "dispatched": 0,
                       "skipped": 0, "failed": 0, "late": 0}
        self.done = threading.Event()
        self._queue = queue.Queue()
        self._armed = set()     # seqs armed, not yet released
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._walked = False    # the cursor passed every input
        self._cursor = threading.Thread(target=self._walk,
                                        name="replay-cursor", daemon=True)
        self._worker = threading.Thread(target=self._work,
                                        name="replay-dispatch",
                                        daemon=True)

    def start(self):
        self._cursor.start()
        self._worker.start()

    def stop(self):
        """Stop both threads; done stays unset (the replay is ending)."""
        self._stop.set()
        self._queue.put(None)

    # -- the cursor thread -------------------------------------------------
    def _walk(self):
        for inp in self.inputs:
            if self._stop.is_set():
                return
            if inp.get("replay") != "guest":
                _note(self.ctl, "replay.skipped",
                      {"seq": inp["seq"], "why": _skip_why(inp)})
                with self._lock:
                    self.counts["skipped"] += 1
                self._maybe_done()
            elif inp["phase"] in ("off", "frozen", "paused"):
                if not self._hold(inp):
                    return
            else:
                self._arm(inp)
        with self._lock:
            self._walked = True
        self._maybe_done()

    def _hold(self, inp):
        """Dispatch when due and the power state matches the phase;
        hold at the head otherwise (order kept: the power verb queued
        just before changes the state)."""
        while not self._stop.is_set():
            if self.ctl.clock.now()[0] >= inp["sim_time"] and \
                    self.ctl.power.state == inp["phase"]:
                self._queue.put(inp)
                return True
            self._stop.wait(self.ctl.clock.wake_in(0.05))
        return False

    def _arm(self, inp):
        with self._lock:
            self._armed.add(inp["seq"])
        self.ctl.clock.at(inp["sim_time"],
                          lambda: self._release(inp))

    def _release(self, inp):
        """The clock.at callback (the poller thread): queue the feed; a
        deadline the bench reached only later notes the delay first."""
        try:
            now = self.ctl.clock.now()[0]
            if now - inp["sim_time"] > _LATE_TOL_NS:
                _note(self.ctl, "replay.late",
                      {"seq": inp["seq"],
                       "by_s": round((now - inp["sim_time"]) / 1e9, 3)})
                with self._lock:
                    self.counts["late"] += 1
            self._queue.put(inp)
        except Exception:       # never into the clock callback
            pass
        finally:
            with self._lock:
                self._armed.discard(inp["seq"])
            self._maybe_done()

    # -- the dispatch-queue thread -----------------------------------------
    def _work(self):
        while True:
            inp = self._queue.get()
            try:
                if inp is None:     # the stop sentinel
                    return
                self._feed_one(inp)
            finally:
                self._queue.task_done()
                self._maybe_done()

    def _feed_one(self, inp):
        try:
            ok = _FEED[inp["input"]](self.ctl, inp)
        except Exception as e:  # not a VerbError: still one record,
            _note(self.ctl, "replay.failed",   # never a dead queue
                  {"seq": inp["seq"], "status": 0,
                   "error": "%s: %s" % (type(e).__name__, e)})
            ok = False
        with self._lock:
            self.counts["dispatched" if ok else "failed"] += 1

    def _maybe_done(self):
        with self._lock:
            if self._stop.is_set() or not self._walked or self._armed \
                    or self._queue.unfinished_tasks:
                return
            self.done.set()
