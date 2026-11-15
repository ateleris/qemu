# SPDX-License-Identifier: GPL-2.0-or-later
"""Run bundles (2026-11-15 run-bundles plan, S1): a run dir plus its
manifest.json and decisions.jsonl — the record -> replay -> compare
formats, fixed now so S2 (replay) and S3 (compare) only add consumers.

The run dir IS the bundle: nothing is copied. The Writer is a
record-log listener appending one decision per line to decisions.jsonl
next to log.jsonl and rewriting manifest.json at close (tmp +
os.replace, the views.py pattern). classify() and decide() are stateful
accumulators fed in record order: S2 re-feeds classify() on the
recorded log, S3 re-derives with decide() alone.
"""

import hashlib
import json
import os
import re
import subprocess
import threading

from . import log as log_mod
from .dispatch import VerbError

BUNDLE_VERSION = 1
MANIFEST = "manifest.json"
DECISIONS = "decisions.jsonl"
BASELINE = ".stixplat-baseline"

# a run_id from the API: checked before any filesystem access (the
# views.py name rule)
RUN_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9.@_-]{0,127}$")
# run-mode dir names already start with this stamp: their run_id is the
# basename (an interactive dir is reused, so its start is stamped on)
STAMP_RE = re.compile(r"^\d{8}T\d{6}Z")

# the verbs S2 re-dispatches as themselves; every other verb a cmd can
# name is excluded below (its canonical input is another record, or it
# must not re-run) and only counts in not_replayable
REPLAY_VERBS = frozenset({
    "sim.pause", "sim.resume", "log.mark", "platform.spw.link",
    "platform.timecode.enable", "platform.timecode.once",
    "platform.timecode.inject", "platform.obt.set",
    "platform.keepalive.set", "det.rotb.burst", "platform.power.on",
    "platform.power.boot", "platform.power.off", "platform.power.cycle"})

_EXCLUDED_VERB_WHY = {
    "fault.inject": "the fault record is the input",
    "fault.clear": "the fault record is the input",
    "force.set": "the force record is the input",
    "force.clear": "the force record is the input",
    "tc.platform.send": "the tc record is the input",
    "tc.gs.send": "the tc record is the input",
    "platform.timesync.send": "the tc record is the input",
    "scenario.run": "already expanded into its own records",
    "scenario.abort": "already expanded into its own records",
    "platform.flash.list": "a read-only listing",
    "platform.flash.load": "flash state is by reference",
    "platform.flash.save": "flash state is by reference",
    "debug.qmp": "a dev tool, never re-run",
    "view.save": "views are not run inputs",
    "view.delete": "views are not run inputs",
}

# the platform-relevant EIDs (decode.py) and their compare classes
EID_CLASSES = {0x0021: "mode", 0x142E: "keepalive", 0x1429: "timesync",
               0x542B: "timesync", 0x14A3: "fdir"}

# the last 512 input-TC (apid, seq) keys an ack may match (8 Hz acks
# against a bounded set)
TC_KEY_LIMIT = 512
# decisions leave the writer's buffer once the run has moved this far
# past them (the listener rule can invert adjacent seqs across emitting
# threads; a window of 1024 orders what inverted, keeps the buffer
# small, and decisions land on disk during the run)
FLUSH_WINDOW = 1024


def _compact(stamp):
    return re.sub(r"[^0-9A-Za-z]", "", stamp or "")


def run_id(basename, started_utc):
    """The bundle id: run-mode basenames already start with their UTC
    stamp -> the basename; an interactive dir is reused across
    sessions, so its start is stamped on."""
    if STAMP_RE.match(basename or ""):
        return basename
    return "%s-%s" % (basename, _compact(started_utc))


def _controller_root():
    return os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))


def git_info(where=None):
    """The git info of the tree holding @where (a file or directory;
    default the controller's own tree), or None (no git, a foreign
    binary): never fatal, never more than two fast calls. The QEMU
    binary's provenance is its own tree's (a build/ dir inside a
    worktree), not the controller's: they are often different
    worktrees."""
    root = _controller_root() if where is None else \
        (where if os.path.isdir(where) else os.path.dirname(where))
    try:
        head = subprocess.run(["git", "-C", root, "rev-parse", "HEAD"],
                              capture_output=True, text=True, timeout=5)
        desc = subprocess.run(
            ["git", "-C", root, "describe", "--always", "--dirty=.dirty"],
            capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.SubprocessError):
        return None
    if head.returncode != 0 or desc.returncode != 0:
        return None
    describe = desc.stdout.strip()
    return {"commit": head.stdout.strip(),
            "dirty": describe.endswith(".dirty"), "describe": describe}


def new_state(hk_codes=()):
    """The per-run accumulator state fed to classify() and decide()
    (the S2/S3 contract: both are fed in record order)."""
    return {"armed": {}, "tc_keys": {}, "hk_last": {},
            "hk_codes": list(hk_codes)}


def _classify_fault(obj, data, state):
    fid = (obj.get("path") or "")[len("fault."):]
    action = data.get("action")
    if action == "armed":
        # the only record carrying the at_sim trigger; its inject
        # follows at fire time and inherits params/seed/trigger
        state["armed"][fid] = {"params": data.get("params"),
                               "seed": data.get("seed"),
                               "trigger": data.get("trigger")}
        return None
    if action == "inject":
        armed = state["armed"].pop(fid, None)
        if armed is not None:
            params, seed, trigger = (armed["params"], armed["seed"],
                                     armed["trigger"])
        else:                       # an immediate inject
            params, seed, trigger = (data.get("params"),
                                     data.get("seed"), {"at": "now"})
        return {"input": "fault", "id": fid, "params": params,
                "seed": seed, "trigger": trigger, "replay": "guest"}
    if action in ("clear", "disarm"):
        # disarm: the armed fault never fired, the clear still replays
        return {"input": "fault", "id": fid, "action": "clear",
                "replay": "guest"}
    return None     # reapply / reapply-error / error: automatic or failed


def classify(obj, state):
    """One record -> its input dict, or None (the S2 contract, fed in
    record order). Faults arm through @state so an inject carries its
    at_sim trigger; the manual TC(9,129) has no hex, its coarse is the
    input (its ack matches no hex-carrying tc and yields no ack
    decision: documented loss)."""
    kind = obj.get("kind")
    data = obj.get("data") or {}
    if kind == "cmd":
        verb = obj.get("path") or ""
        inp = {"input": "verb", "verb": verb,
               "args": data.get("args") or {}}
        if verb in REPLAY_VERBS:
            inp["replay"] = "guest"
            return inp
        inp["replay"] = "no"
        inp["why"] = _EXCLUDED_VERB_WHY.get(verb, "no replay rule")
        return inp
    if kind == "tc":
        if obj.get("path") == "platform.timesync" and "hex" not in data:
            return {"input": "tc", "hex": None, "service": 9,
                    "subtype": 129, "seq": data.get("seq"),
                    "coarse": data.get("coarse"), "replay": "guest"}
        if "hex" not in data:
            return None     # keep-alive summaries, held drops, ...
        out = {"input": "tc", "hex": data["hex"]}
        if not data.get("delivered"):
            out["replay"] = "no"
            out["why"] = "never reached the FSW"
        elif data.get("source") == "tc":
            out["replay"] = "wall"      # a real GS TC: wall-paced
        else:
            out["replay"] = "guest"
        return out
    if kind == "fault":
        return _classify_fault(obj, data, state)
    if kind == "force":
        if obj.get("path") == "force.reapply":
            return None     # automatic: the power hooks re-apply
        out = {"input": "force", "path": obj.get("path"),
               "replay": "guest"}
        if "value" in data:
            out["value"] = data["value"]
        return out
    return None


def decide(obj, state):
    """One record -> [decision] (the S3 contract, fed in record order).
    A decision is {v, seq, sim_time, boot_id, kind, key, class, detail}:
    event (TM(5,x)), ack (TM(1,x) whose (apid, seq) matches a tc record
    carrying hex seen earlier in this run), hk_step (a watched HK field
    that changed since the previous report of the same (sid, code)).
    Heartbeats and HK reports without data.hk produce nothing."""
    kind = obj.get("kind")
    data = obj.get("data") or {}
    if kind == "tc":
        # the ack rule's input-TC key set: bounded to the last 512
        if "hex" in data:
            apid, seq = data.get("apid"), data.get("seq")
            if isinstance(apid, int) and isinstance(seq, int) and \
                    not isinstance(apid, bool):
                keys = state["tc_keys"]
                keys[(apid, seq)] = True
                while len(keys) > TC_KEY_LIMIT:
                    keys.pop(next(iter(keys)))
        return []
    if kind != "tm":
        return []
    dec = data.get("decoded")
    if not isinstance(dec, dict) or dec.get("heartbeat"):
        return []
    base = {"v": 1, "seq": obj.get("seq"),
            "sim_time": obj.get("sim_time"), "boot_id": obj.get("boot_id")}
    out = []
    if dec.get("type") == 1 and isinstance(dec.get("tc"), dict):
        tc = dec["tc"]
        apid, seq = tc.get("apid"), tc.get("seq")
        if isinstance(apid, int) and isinstance(seq, int) and \
                (apid, seq) in state["tc_keys"]:
            out.append(dict(base, kind="ack",
                            key="0x%04X/%d" % (apid, seq),
                            detail={"subtype": dec.get("subtype"),
                                    "fid": dec.get("fid")}))
            out[-1]["class"] = None
    elif dec.get("type") == 5 and "eid" in dec:
        eid = dec["eid"]
        out.append(dict(base, kind="event", key="0x%04X" % eid,
                        detail={"name": dec.get("eid_name"),
                                "params": dec.get("params")}))
        out[-1]["class"] = EID_CLASSES.get(eid, "other")
    hk = data.get("hk")
    sid = dec.get("sid")
    if isinstance(hk, dict) and sid in (1, 2):
        for code in state["hk_codes"]:
            if code not in hk:
                continue
            v = hk[code]
            prev = state["hk_last"].get((sid, code))
            state["hk_last"][(sid, code)] = v
            if prev is not None and v != prev:
                out.append(dict(base, kind="hk_step",
                                key="hk:%d:%s" % (sid, code),
                                detail={"value": v, "previous": prev}))
                out[-1]["class"] = None
    return out


# -- the manifest helpers ---------------------------------------------------
def _write_json(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=1, sort_keys=True)
        f.write("\n")
    os.replace(tmp, path)       # a reader sees old or new, not half


def _public(cfg):
    return {k: v for k, v in cfg.items() if not k.startswith("_")}


def _sha256_file(path):
    """The digest of a small file (the ELF, the QEMU binary); None when
    unreadable. Never the flash: that one is sparse_sha256 (flashlib),
    only at an explicit flash-library save."""
    h = hashlib.sha256()
    try:
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
    except OSError:
        return None
    return h.hexdigest()


def _qemu_info(binary, argv):
    none = {"commit": None, "dirty": None, "describe": None}
    real = os.path.realpath(binary) if binary else None
    g = (git_info(real) if real and os.path.exists(real) else None) or none
    c = git_info() or none
    return {"binary": binary, "sha256": _sha256_file(binary),
            "commit": g["commit"], "dirty": g["dirty"],
            "describe": g["describe"], "argv": argv,
            # the controller's own tree (stixplat), for the record
            "platform": {"commit": c["commit"], "dirty": c["dirty"],
                         "describe": c["describe"]}}


def _flash_end(run_dir):
    """flash.end: {present, allocated_kib} from st_blocks — never
    hashed, never materialised."""
    try:
        st = os.stat(os.path.join(run_dir, "flash.bin"))
    except OSError:
        return {"present": False, "allocated_kib": 0}
    return {"present": True, "allocated_kib": st.st_blocks * 512 // 1024}


class Writer:
    """The bundle writer (R2): a record-log listener owning
    decisions.jsonl (one decision per line, appended under its own
    lock) and manifest.json (rewritten atomically at close).

    The listener rule: never emits, never takes state.lock; a failure
    dies in the log's listener guard. Listener calls can invert
    adjacent seqs across emitting threads (the log releases its cond
    before the calls), so decisions wait in a small per-seq buffer
    until the run has moved a window past them, and whatever is left
    flushes in seq order at close.
    """

    def __init__(self, cfg, run_dir, log, mode, scenario=None,
                 start_seq=0, kind="run"):
        self.cfg = cfg
        self.run_dir = run_dir
        self.log = log
        self.mode = mode
        self.scenario = scenario
        self.kind = kind
        self.lock = threading.Lock()
        self.state = new_state(cfg["bundle"]["hk_codes"])
        self._start_seq = start_seq
        self._flash_existed = os.path.exists(
            os.path.join(run_dir, "flash.bin"))
        self._dec_path = os.path.join(run_dir, DECISIONS)
        # truncate: a reused interactive dir (or fresh = true, which
        # leaves unknown files alone) must never mix sessions
        self._dec_fh = open(self._dec_path, "w", encoding="utf-8")
        with log.cond:
            # every record emitted after this point reaches us (the log
            # appends listeners under its cond); earlier ones are below
            # the first seq we can see
            self._flushed = log.seq
        self._pending = {}          # seq -> [decision, ...]
        self._high = self._flushed  # the highest seq seen
        self._stragglers = []       # arrived below the flushed seqs
        self._counts = {"verb": {}, "tc": 0, "fault": 0, "force": 0}
        self._not_replayable = 0
        self._dec_counts = {"event": 0, "ack": 0, "hk_step": 0}
        self._records = 0
        self._argv = None
        self._t0 = None
        self._last_sim = 0
        self._last_seq = start_seq
        self._flash_load = None     # the library entry before power-on
        self._powered_on = False
        self._closed = False
        self.started_utc = log_mod.wall_now()
        self.run_id = run_id(os.path.basename(run_dir), self.started_utc)
        _write_json(os.path.join(run_dir, MANIFEST),
                    self._manifest(open_=True))

    # -- the listener -------------------------------------------------------
    def on_record(self, rec):
        obj = rec.obj
        with self.lock:
            if self._closed:
                return
            self._records += 1
            self._last_sim = obj.get("sim_time") or 0
            self._last_seq = obj.get("seq") or self._last_seq
            data = obj.get("data") or {}
            kind = obj.get("kind")
            if kind == "power":
                event = data.get("event")
                if event == "on":
                    self._powered_on = True
                    if self._argv is None and \
                            isinstance(data.get("argv"), list):
                        self._argv = data["argv"]
                elif event == "boot" and "t0" in data:
                    self._t0 = data.get("t0")
            elif kind == "result" and \
                    obj.get("path") == "platform.flash.load" and \
                    not self._powered_on and \
                    isinstance(data.get("result"), dict):
                # the flash library entry the run started from: its
                # result record carries name and (since R2) sha256
                self._flash_load = data["result"]
            inp = classify(obj, self.state)
            if inp is not None:
                self._count(inp)
            for d in decide(obj, self.state):
                self._dec_counts[d["kind"]] = \
                    self._dec_counts.get(d["kind"], 0) + 1
                self._pending.setdefault(d["seq"], []).append(d)
            seq = obj.get("seq")
            if isinstance(seq, int) and seq > self._high:
                self._high = seq
            self._flush()

    def _count(self, inp):
        how = inp["input"]
        if how == "verb":
            self._counts["verb"][inp["verb"]] = \
                self._counts["verb"].get(inp["verb"], 0) + 1
        else:
            self._counts[how] = self._counts.get(how, 0) + 1
        if inp.get("replay") == "no":
            self._not_replayable += 1

    def _flush(self):
        """Write the decisions the run has moved a window past; called
        under the lock, so the lines are atomic and ascending. A
        straggler below the flushed seqs waits for the close merge."""
        bound = self._high - FLUSH_WINDOW
        while self._pending:
            s = min(self._pending)
            if s <= self._flushed:
                self._stragglers += self._pending.pop(s)
                continue
            if s > bound:
                break
            for d in self._pending.pop(s):
                self._dec_fh.write(json.dumps(
                    d, separators=(",", ":"), allow_nan=False) + "\n")
            self._flushed = s
            self._dec_fh.flush()

    # -- the manifest ---------------------------------------------------------
    def _manifest(self, open_):
        q = self.cfg["qemu"]
        length = None
        if not open_ and self._t0 is not None:
            length = (self._last_sim - self._t0) / 1e9
        return {
            "v": BUNDLE_VERSION, "kind": self.kind, "run_id": self.run_id,
            "dir": self.run_dir, "mode": self.mode,
            "scenario": self.scenario, "started_utc": self.started_utc,
            "start_seq": self._start_seq,
            # seqs restart per session; S2 slices log.jsonl from here
            "start_offset": getattr(self.log, "start_offset", 0),
            "ended_utc": None if open_ else log_mod.wall_now(),
            "open": open_, "t0": self._t0,
            "final_sim_time": None if open_ else self._last_sim,
            "final_seq": None if open_ else self._last_seq,
            "length_s": length,
            "fsw": {"elf": q["elf"], "sha256": _sha256_file(q["elf"])},
            "qemu": _qemu_info(q["binary"], self._argv),
            "config": _public(self.cfg),
            "flash": {"start": None if open_ else self._flash_start(),
                      "end": None if open_ else _flash_end(self.run_dir)},
            "outputs": None if open_ else self._outputs(),
            "inputs": {"counts": self._counts,
                       "not_replayable": self._not_replayable},
            "decisions": {"counts": self._dec_counts},
        }

    def _flash_start(self):
        if self._flash_load is not None:
            rec = self._flash_load
            return {"source": "library", "name": rec.get("name"),
                    "sha256": rec.get("sha256"), "record": rec}
        if self._flash_existed:
            return {"source": "carried", "name": None, "sha256": None,
                    "record": None}
        return {"source": "fresh", "name": None, "sha256": None,
                "record": None}

    def _outputs(self):
        def sz(name):
            try:
                return os.path.getsize(os.path.join(self.run_dir, name))
            except OSError:
                return None
        return {"log_records": self._records, "log_bytes": sz("log.jsonl"),
                # tm.jsonl has no cheap line count here; the bytes say it
                "tm_lines": None, "tm_bytes": sz("tm.jsonl"),
                "uart_bytes": sz("uart.raw")}

    def close(self):
        """The final manifest; idempotent, safe to call twice. Leftover
        buffered decisions flush in seq order; a straggler that arrives
        below what was already written is merged (the file is rewritten
        sorted) so the line seqs stay strictly increasing."""
        with self.lock:
            if self._closed:
                return
            self._closed = True
            rows = self._stragglers + [d for s in sorted(self._pending)
                                       for d in self._pending[s]]
            self._pending.clear()
            self._stragglers = []
            merged = False
            if rows and rows[0]["seq"] <= self._flushed:
                self._merge_leftovers(rows)   # rewrote the file already
                merged = True
            else:
                for d in rows:
                    self._dec_fh.write(json.dumps(
                        d, separators=(",", ":"), allow_nan=False) + "\n")
            if not merged:
                self._dec_fh.flush()
            self._dec_fh.close()
            _write_json(os.path.join(self.run_dir, MANIFEST),
                        self._manifest(open_=False))

    def _merge_leftovers(self, rows):
        """A straggler below the flushed seqs: read, merge, rewrite
        sorted, so the line seqs stay strictly increasing."""
        self._dec_fh.flush()
        with open(self._dec_path, encoding="utf-8") as f:
            allrows = [json.loads(x) for x in f if x.strip()]
        allrows += rows
        allrows.sort(key=lambda d: d["seq"])
        self._dec_fh.close()
        tmp = self._dec_path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            for d in allrows:
                f.write(json.dumps(d, separators=(",", ":"),
                                   allow_nan=False) + "\n")
        os.replace(tmp, self._dec_path)


def set_verdict(run_dir, verdict):
    """runmode._batch, after retention.finish_run: the finished
    run-mode manifest carries the scenario's verdict. A missing or
    broken manifest (the writer disabled) is left alone."""
    path = os.path.join(run_dir, MANIFEST)
    try:
        with open(path, encoding="utf-8") as f:
            m = json.load(f)
    except (OSError, ValueError):
        return
    if not isinstance(m, dict) or m.get("verdict") == verdict:
        return
    m["verdict"] = verdict
    try:
        _write_json(path, m)
    except OSError:
        pass


# -- the runs board API (R3) -------------------------------------------------
def check_run_id(run_id_):
    """A run_id from a URL: 400 before any filesystem access (the
    views.py name-rule pattern)."""
    if not isinstance(run_id_, str) or not RUN_ID_RE.match(run_id_):
        raise VerbError(400, "run id %r: 1..128 of A-Za-z0-9.@_-"
                        % (run_id_,))


def is_baseline(d):
    return os.path.isfile(os.path.join(d, BASELINE))


def _candidate_dirs(cfg, live_dir=None):
    out = []
    if live_dir:
        out.append(live_dir)
    runs_dir = os.path.join(cfg["retention"]["run_root"], "runs")
    try:
        names = sorted(os.listdir(runs_dir))
    except OSError:
        names = []
    out += [os.path.join(runs_dir, n) for n in names]
    return out


def _entry(d, live):
    try:
        with open(os.path.join(d, MANIFEST), encoding="utf-8") as f:
            m = json.load(f)
        if not isinstance(m, dict):
            raise ValueError("not a JSON object")
    except (OSError, ValueError) as e:
        return {"run_id": os.path.basename(d), "dir": d, "live": live,
                "baseline": is_baseline(d), "error": str(e)}
    scn = m.get("scenario") if isinstance(m.get("scenario"), dict) else {}
    q = m.get("qemu") if isinstance(m.get("qemu"), dict) else {}
    fsw = m.get("fsw") if isinstance(m.get("fsw"), dict) else {}
    cfg = m.get("config") if isinstance(m.get("config"), dict) else {}
    qc = cfg.get("qemu") if isinstance(cfg.get("qemu"), dict) else {}
    return {"run_id": m.get("run_id") or os.path.basename(d), "dir": d,
            "live": live, "open": m.get("open") is True,
            "started_utc": m.get("started_utc"),
            "ended_utc": m.get("ended_utc"), "verdict": m.get("verdict"),
            "mode": m.get("mode"), "scenario": m.get("scenario"),
            "name": scn.get("name") or m.get("run_id") or
            os.path.basename(d),
            "elf": fsw.get("elf"), "elf_sha256": fsw.get("sha256"),
            "qemu_describe": q.get("describe"),
            "qemu_dirty": q.get("dirty"),
            "icount_shift": qc.get("icount_shift"),
            "pace": qc.get("pace"), "length_s": m.get("length_s"),
            "baseline": is_baseline(d), "inputs": m.get("inputs"),
            "decisions": m.get("decisions")}


def listing(cfg, live_dir=None):
    """GET /runs: the bundles with a manifest.json under
    <retention.run_root>/runs plus the live dir (when it has one),
    sorted by started_utc, newest first. Dirs without a manifest are
    not listed; a broken manifest is an entry with error."""
    root = cfg["retention"]["run_root"]
    dirs = []
    if live_dir and os.path.isfile(os.path.join(live_dir, MANIFEST)):
        dirs.append(live_dir)
    runs_dir = os.path.join(root, "runs")
    try:
        names = sorted(os.listdir(runs_dir))
    except OSError:
        names = []
    for name in names:
        p = os.path.join(runs_dir, name)
        if os.path.isdir(p) and not os.path.islink(p) and \
                os.path.isfile(os.path.join(p, MANIFEST)):
            dirs.append(p)
    live = os.path.abspath(live_dir) if live_dir else None
    out = [_entry(d, os.path.abspath(d) == live) for d in dirs]
    out.sort(key=lambda e: e.get("started_utc") or "", reverse=True)
    return {"root": root, "runs": out}


def _resolve(cfg, run_id_, live_dir=None):
    """The bundle dir whose manifest carries that run_id; 404 unknown
    (the name rule ran before this opened anything)."""
    for d in _candidate_dirs(cfg, live_dir):
        try:
            with open(os.path.join(d, MANIFEST), encoding="utf-8") as f:
                m = json.load(f)
        except (OSError, ValueError):
            continue
        if isinstance(m, dict) and m.get("run_id") == run_id_:
            return d
    raise VerbError(404, "no run bundle %r" % run_id_)


def read_manifest(cfg, run_id_, live_dir=None):
    check_run_id(run_id_)
    d = _resolve(cfg, run_id_, live_dir)
    with open(os.path.join(d, MANIFEST), encoding="utf-8") as f:
        return json.load(f)


def manifest_bytes(cfg, run_id_, live_dir=None):
    """GET /runs/<run_id>/manifest: the raw manifest.json bytes."""
    check_run_id(run_id_)
    d = _resolve(cfg, run_id_, live_dir)
    with open(os.path.join(d, MANIFEST), "rb") as f:
        return f.read()


def detail(cfg, run_id_, live_dir=None):
    """GET /runs/<run_id>: the manifest plus the file sizes, the
    baseline mark and the retention marker status."""
    from . import retention
    check_run_id(run_id_)
    d = _resolve(cfg, run_id_, live_dir)
    with open(os.path.join(d, MANIFEST), encoding="utf-8") as f:
        m = json.load(f)

    def sz(name):
        try:
            return os.path.getsize(os.path.join(d, name))
        except OSError:
            return None
    m["dir"] = d
    m["baseline"] = is_baseline(d)
    m["marker"] = retention.read_marker(d)
    m["files"] = {"log_bytes": sz("log.jsonl"), "tm_bytes": sz("tm.jsonl"),
                  "uart_bytes": sz("uart.raw"),
                  "decisions_bytes": sz(DECISIONS)}
    return m


def baseline_set(cfg, run_id_, live_dir=None):
    """run.baseline: mark that bundle's dir; the previous holder's mark
    is removed. 404 unknown."""
    check_run_id(run_id_)
    d = _resolve(cfg, run_id_, live_dir)
    for other in _candidate_dirs(cfg, live_dir):
        if other != d:
            _clear_marker(other)
    _write_json(os.path.join(d, BASELINE),
                {"name": run_id_, "set_utc": log_mod.wall_now()})
    return {"name": run_id_, "dir": d}


def baseline_clear(cfg, live_dir=None):
    """run.baseline.clear: remove the mark wherever it is (there is at
    most one)."""
    cleared = None
    for d in _candidate_dirs(cfg, live_dir):
        if _clear_marker(d):
            cleared = os.path.basename(d)
    return {"cleared": cleared}


def _clear_marker(d):
    try:
        os.unlink(os.path.join(d, BASELINE))
        return True
    except OSError:
        return False


class Viewers:
    """At most one open read-only viewer of another run dir (R3): the
    exact --view-run objects (a ViewRun served by api.make_server on
    127.0.0.1), in-process. ViewRun's 403-for-non-view.* rule applies
    unchanged."""

    def __init__(self):
        self.lock = threading.Lock()
        self.viewer = None          # (server, ViewRun)

    def open(self, ctl, run_id_, port=None):
        with self.lock:
            if self.viewer is not None:
                raise VerbError(409, "a run viewer is already open on "
                                "port %d (run.close first)"
                                % self.viewer[0].server_address[1])
            d = _resolve(ctl.cfg, run_id_, getattr(ctl, "run_dir", None))
            from . import api as api_mod
            from . import viewrun as viewrun_mod
            vr = viewrun_mod.ViewRun(d)
            srv = api_mod.make_server(vr, "127.0.0.1",
                                      0 if port is None else port)
            self.viewer = (srv, vr)
            return {"url": "http://127.0.0.1:%d" % srv.server_address[1],
                    "port": srv.server_address[1]}

    def close(self, port=None):
        with self.lock:
            viewer = self.viewer
            if viewer is None:
                raise VerbError(409, "no run viewer is open")
            srv = viewer[0]
            if port is not None and port != srv.server_address[1]:
                raise VerbError(409, "the open viewer is on port %d"
                                % srv.server_address[1])
            self.viewer = None
        viewer[1].stopping.set()
        srv.shutdown()
        srv.server_close()
        return {"closed": srv.server_address[1]}
