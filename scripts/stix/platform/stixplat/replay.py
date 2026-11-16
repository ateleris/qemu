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
import re

from . import bundle as bundle_mod
from . import log as log_mod

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
