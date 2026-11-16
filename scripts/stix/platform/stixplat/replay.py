# SPDX-License-Identifier: GPL-2.0-or-later
"""Run bundles S2: replay a bundle on guest time (2026-11-15 replay
plan). P1 is the input loader: the origin's log slice, in record
order, through bundle.classify, every classified input tagged with its
seq, sim_time, boot_id and the origin's power phase. The origin is
read-only: the loader opens its files, never writes. P2 adds the
scheduler, the re-feed table and run() around it, P3 the CLI.
"""

import gzip
import json
import os
import queue
import re
import shutil
import signal
import socket
import threading
import time

from . import bundle as bundle_mod
from . import config as config_mod
from . import core as core_mod
from . import flashlib as flashlib_mod
from . import log as log_mod
from . import tapes as tapes_mod
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

# the verbs that move the power state the held inputs wait for
_STATE_VERBS = ("platform.power.", "sim.pause", "sim.resume")

# a Unix socket path holds at most 107 bytes (sun_path is 108 with the
# NUL); QEMU's QMP socket lives at <run dir>/qmp.sock
_SUN_PATH_MAX = 107


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

    Off/frozen/paused-phase inputs are held at the head, in order,
    until the replay's power state matches the phase; an off-phase
    input also waits for clock.now() to pass its sim_time (bench time
    runs on the wall clock while off). Frozen and paused inputs wait
    for the state only (a plan deviation): bench time stands still in
    both, at a value set by the replay's own launch or pause latency,
    so it may never reach the origin's sim_time; and clock.at never
    fires while paused, so an armed sim.resume would never fire.
    Running-phase inputs are armed
    exactly once on clock.at; a deadline fired more than _LATE_TOL_NS
    past its sim_time gets one replay.late record first (owner
    question 1a). Inputs
    with replay != "guest" never dispatch: one replay.skipped as the
    cursor passes them. done is set once every input was dispatched or
    skipped and every armed one has fired. A failed power or pause
    verb sets aborted and stops the scheduler: the held inputs after it
    wait for a state that will not come.
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
        self.aborted = None     # why a failed state verb ended the replay
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
        """Dispatch once the power state matches the phase (and, while
        off, once due); hold at the head otherwise (order kept: the
        power verb queued just before changes the state)."""
        while not self._stop.is_set():
            due = inp["phase"] != "off" or \
                self.ctl.clock.now()[0] >= inp["sim_time"]
            if due and self.ctl.power.state == inp["phase"]:
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
        if not ok and inp.get("input") == "verb" and \
                inp.get("verb", "").startswith(_STATE_VERBS):
            # every later held input waits for the state this verb
            # should have set: abort instead of holding forever
            self.aborted = "%s (seq %d) failed" % (inp["verb"], inp["seq"])
            self.stop()

    def _maybe_done(self):
        with self._lock:
            if self._stop.is_set() or not self._walked or self._armed \
                    or self._queue.unfinished_tasks:
                return
            self.done.set()


# -- run(): the whole replay (P2) ------------------------------------------

def _free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]
    finally:
        s.close()


def _free_tcp_port():
    """R6: the API port when none was given (a 0 is refused by
    config._check, so the replay probes one)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]
    finally:
        s.close()


def _read_manifest(bundle_dir):
    path = os.path.join(bundle_dir, bundle_mod.MANIFEST)
    try:
        with open(path, encoding="utf-8") as f:
            m = json.load(f)
    except (OSError, ValueError):
        raise ReplayError("no run bundle at %r" % (bundle_dir,))
    if not isinstance(m, dict):
        raise ReplayError("no run bundle at %r" % (bundle_dir,))
    return m


def _check_wall(inputs, allow_wall):
    """No wall TC is ever converted: the refusal only lifts."""
    walls = [i for i in inputs if i.get("replay") == "wall"]
    if walls and not allow_wall:
        raise ReplayError("%d wall-paced GS TC(s) (first seq %d): "
                          "replaying omits them — pass --allow-wall to "
                          "accept that"
                          % (len(walls), walls[0]["seq"]))


def _check_idb(origin_cfg, no_idb):
    """R3: the IDB is a by-reference input; a missing file refuses
    unless @no_idb (the manifest then records idb "absent")."""
    if no_idb:
        return
    tl = origin_cfg.get("timeline") or {}
    for key in ("idb_json", "idb_sqlite"):
        v = tl.get(key)
        if not v:
            continue
        path = os.path.expanduser(v)
        if not os.path.isfile(path):
            raise ReplayError("the recorded %s %s is missing on this "
                              "host: pass --no-idb to run without it "
                              "(the replay manifest then records idb "
                              "\"absent\")" % (key, path))


def _check_flash(manifest, cfg):
    """The origin's flash start by reference: 'carried' refuses (owner
    question 4a); a 'library' entry must exist under the local run
    root's flash-library."""
    start = ((manifest.get("flash") or {}).get("start")) or {}
    src = start.get("source")
    if src == "carried":
        raise ReplayError("flash start is 'carried': no library entry "
                          "to load (see platform.md)")
    if src == "library":
        name = start.get("name")
        lib = flashlib_mod.library_dir(cfg)
        rec = start.get("record") or {}
        if not os.path.isfile(os.path.join(lib, "%s.bin" % name)) or \
                not os.path.isfile(os.path.join(lib, "%s.json" % name)):
            raise ReplayError("no flash library entry %r under %s (the "
                              "origin recorded it from %s)"
                              % (name, lib, rec.get("source_run_dir")))


def _check_tapes(inputs, origin_cfg):
    """R4/R5: every det.tape.play input's tape file must exist in the
    origin's recorded tapes dir, its spec.file CSVs included; returns
    [(name, tape)] for the copy into the replay's tapes dir."""
    d = tapes_mod.tapes_dir(origin_cfg)
    played = []
    seen = set()
    for inp in inputs:
        if inp.get("input") != "verb" or \
                inp.get("verb") != "det.tape.play":
            continue
        name = (inp.get("args") or {}).get("name")
        if not isinstance(name, str) or name in seen:
            continue
        seen.add(name)
        path = os.path.join(d, "%s.json" % name)
        if not os.path.isfile(path):
            raise ReplayError("tape file %r is missing from %s"
                              % (os.path.basename(path), d))
        try:
            with open(path, encoding="utf-8") as f:
                tape = json.load(f)
            tape = tapes_mod.validate(tape)
            # R4: the spectrum CSVs resolve at pre-flight, not at
            # re-feed time (a missing one is a refusal, not a
            # replay.failed)
            tapes_mod.resolve(origin_cfg, tape,
                              tapes_mod.KEV_FULL_DEFAULT)
        except (VerbError, ValueError, OSError) as e:
            raise ReplayError("tape %s: %s" % (name, e))
        played.append((name, tape))
    return played


def _copy_tapes(played, origin_cfg, rcfg):
    """R5: the played tapes and their spectra copied from the origin's
    recorded tapes dir into the replay's, so the replay is
    self-contained and the origin's dirs stay read-only."""
    if not played:
        return
    src_dir = tapes_mod.tapes_dir(origin_cfg)
    dst_dir = tapes_mod.tapes_dir(rcfg)
    os.makedirs(dst_dir, exist_ok=True)
    for name, tape in played:
        pairs = [(os.path.join(src_dir, "%s.json" % name),
                  os.path.join(dst_dir, "%s.json" % name))]
        for step in tape.get("steps") or []:
            spec = step.get("spec") if isinstance(step, dict) else None
            if isinstance(spec, dict) and isinstance(spec.get("file"),
                                                     str):
                pairs.append((os.path.join(tapes_mod.spectra_dir(
                                              origin_cfg),
                                            spec["file"]),
                              os.path.join(tapes_mod.spectra_dir(rcfg),
                                           spec["file"])))
        os.makedirs(tapes_mod.spectra_dir(rcfg), exist_ok=True)
        for src, dst in pairs:
            if os.path.realpath(src) == os.path.realpath(dst):
                continue
            shutil.copyfile(src, dst)


def run(cfg, bundle_dir, elf=None, qemu_binary=None, port=None,
        allow_wall=False, no_idb=False, on_controller=None, out=print,
        now=time.time):
    """One replay of the finished bundle at @bundle_dir, on guest time;
    returns EXIT["ok"|"refused"|"infra"], never raises. Refusals
    (ReplayError inside, exit 2): no or broken manifest, still open, no
    t0, the log slice rotated away, a wall input without allow_wall, a
    missing recorded IDB without no_idb, a carried flash start, a
    missing flash-library entry, a missing tape or spectrum file.
    Controller construction or start failures are exit 3. SIGTERM and
    SIGINT stop early (exit 0; the Writer's close still runs); the
    handlers are installed only from the main thread and restored on
    return."""
    stop = threading.Event()

    def on_signal(signum, frame):
        stop.set()

    main = threading.current_thread() is threading.main_thread()
    old = None
    if main:
        old = (signal.signal(signal.SIGTERM, on_signal),
               signal.signal(signal.SIGINT, on_signal))
    try:
        return _run(cfg, bundle_dir, elf, qemu_binary, port, allow_wall,
                    no_idb, on_controller, out, now, stop)
    except ReplayError as e:
        out("stixplat: %s" % e)
        return EXIT["refused"]
    except Exception as e:
        out("stixplat: %s: %s" % (type(e).__name__, e))
        return EXIT["infra"]
    finally:
        if main:
            signal.signal(signal.SIGTERM, old[0])
            signal.signal(signal.SIGINT, old[1])


def _run(cfg, bundle_dir, elf, qemu_binary, port, allow_wall, no_idb,
         on_controller, out, now, stop):
    # 1. the origin: its manifest, its inputs, its by-reference files
    #    (nothing created yet)
    m = _read_manifest(bundle_dir)
    origin_cfg = m.get("config")
    if not isinstance(origin_cfg, dict):
        raise ReplayError("no run bundle at %r (no recorded config)"
                          % (bundle_dir,))
    inputs = load_inputs(bundle_dir, m)
    _check_wall(inputs, allow_wall)
    _check_idb(origin_cfg, no_idb)
    _check_flash(m, cfg)
    played = _check_tapes(inputs, origin_cfg)
    # 2. the replay config: the recorded config with the replay's own
    #    bench bits; the recorded mode and gs.tm_dests stay
    root = cfg["retention"]["run_root"]
    over = {"timesync": {"autostart": "off"},   # D-e: the automatic
            # TC(9,129) is itself a recorded input
            "ports": {"api": port if port is not None
                      else _free_tcp_port(),
                      "qemu_tm": _free_udp_port(),
                      "qemu_tc": _free_udp_port(),
                      "gs_tc": _free_udp_port()},
            "retention": {"run_root": root},
            "timeline": {"views_dir": os.path.join(root, "views"),
                         "tapes_dir": os.path.join(root, "tapes")}}
    if elf:
        over["qemu"] = {"elf": elf}
    if qemu_binary:
        over.setdefault("qemu", {})["binary"] = qemu_binary
    # 3. the replay dir <run_root>/runs/<stamp>-replay-<origin id>,
    #    capped to RUN_ID_RE's 128 characters and so that
    #    <dir>/qmp.sock fits a Unix socket path (the manifest's "of"
    #    keeps the full origin id)
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime(now()))
    head = "%s-replay-" % stamp
    parent = os.path.join(os.path.abspath(root), "runs")
    # <parent>/<name>/qmp.sock
    room = min(128, _SUN_PATH_MAX - len(parent) - len("/") -
               len("/qmp.sock"))
    if room < len(head) + 8:
        raise ReplayError("run_root %r is too long for the replay dir's "
                          "QMP socket (Unix socket paths hold %d bytes): "
                          "use a shorter [retention] run_root"
                          % (root, _SUN_PATH_MAX))
    name = (head + (m.get("run_id") or
                    os.path.basename(bundle_dir)))[:room]
    over["run_dir"] = {"path": os.path.join(parent, name),
                       "fresh": True}
    rcfg = config_mod.merged(origin_cfg, over)
    _copy_tapes(played, origin_cfg, rcfg)
    # the replay manifest's of/elf/qemu-binary (D-b); R3's no_idb marks
    # the absent IDB so S3 knows the hk_step rows are not comparable
    extra = {"of": m.get("run_id"), "elf": rcfg["qemu"]["elf"],
             "qemu-binary": rcfg["qemu"]["binary"]}
    if no_idb:
        extra["idb"] = "absent"
    # 4. the controller; its Writer writes kind "replay" and the extra
    ctl = None
    try:
        try:
            ctl = core_mod.Controller(rcfg, run_dir=over["run_dir"]["path"],
                                      bundle_kind="replay",
                                      bundle_extra=extra)
        except Exception as e:
            out("stixplat: controller: %s: %s" % (type(e).__name__, e))
            return EXIT["infra"]
        # 5. the flash start, power off (the pre-check passed, so a
        #    failure here is a real error, exit 3)
        start = ((m.get("flash") or {}).get("start")) or {}
        if start.get("source") == "library":
            try:
                ctl.command("platform.flash.load",
                            {"name": start.get("name")}, source="scn")
            except Exception as e:
                out("stixplat: flash load: %s: %s"
                    % (type(e).__name__, e))
                ctl.shutdown("replay flash load failed")
                return EXIT["infra"]
        # 6. serve, the caller's hook, then the scheduler
        try:
            ctl.start(serve=True)
        except Exception as e:
            out("stixplat: start: %s: %s" % (type(e).__name__, e))
            ctl.shutdown("replay start failed")
            return EXIT["infra"]
        if on_controller is not None:
            on_controller(ctl)
        sched = Scheduler(ctl, inputs)
        sched.start()
        # 7. the end rule: every input dispatched, then the origin's
        #    length_s of bench time (bench time advances while off), one
        #    replay.done with the scheduler's counts, the shutdown
        while not sched.done.wait(0.2):
            if sched.aborted:
                out("stixplat: replay aborted: %s" % sched.aborted)
                _note(ctl, "replay.done",
                      dict(sched.counts, aborted=sched.aborted))
                ctl.shutdown("replay aborted")
                return EXIT["infra"]
            if stop.is_set():
                sched.stop()
                ctl.shutdown("replay stopped")
                return EXIT["ok"]
        end = (m.get("t0") or 0) + (m.get("length_s") or 0.0) * 1e9
        while ctl.clock.now()[0] < end and not stop.is_set():
            time.sleep(ctl.clock.wake_in(0.05))
        _note(ctl, "replay.done", dict(sched.counts))
        ctl.shutdown("replay stopped" if stop.is_set() else "replay done")
        return EXIT["ok"]
    except Exception as e:
        out("stixplat: %s: %s" % (type(e).__name__, e))
        return EXIT["infra"]
    finally:
        if ctl is not None and not ctl.stopping.is_set():
            try:
                ctl.shutdown("replay error")
            except Exception:
                pass
