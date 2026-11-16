# SPDX-License-Identifier: GPL-2.0-or-later
"""Run bundles S3: compare two finished run bundles (2026-11-16
compare plan): the decisions — the plan review's R1 pairing — plus the
derived overrun rows from the hk_step decisions, and the surfaces (C2):
the run.compare verb, the artifact pair and the --compare CLI. Both
bundles are read-only and compare() is pure. Pass 2 (the series
numbers, R10) comes with C3.
"""

import json
import os

import copy

from . import bundle as bundle_mod
from . import config as config_mod
from . import replay as replay_mod
from .dispatch import VerbError

# tol=None: the [compare] defaults (the series ids are pass 2's, read
# by C3)
DEFAULT_TOL = copy.deepcopy(config_mod.DEFAULTS["compare"])

# the derived overrun rows' decision (the mockup's "HK level 7";
# owner question 4: ship v1 with 7, revisit if corrected)
_OVERRUN_KEY = "hk:2:NIXD0501"
_OVERRUN_VALUE = 7


class CompareError(Exception):
    """A refusal (the message names the side): no bundle at a dir, a
    slice rotated away with no decisions to fall back on, a manifest
    still open. The surfaces map it to exit 2 / 404."""


# -- the loaders -------------------------------------------------------------

def _read_manifest(run_dir):
    """The bundle's manifest dict, or None (no or broken manifest.json)."""
    try:
        with open(os.path.join(run_dir, bundle_mod.MANIFEST),
                  encoding="utf-8") as f:
            m = json.load(f)
    except (OSError, ValueError):
        return None
    return m if isinstance(m, dict) else None


def _has_log(run_dir):
    """log.jsonl or any rotated part is there."""
    if os.path.isfile(os.path.join(run_dir, "log.jsonl")):
        return True
    try:
        names = os.listdir(run_dir)
    except OSError:
        return False
    return any(replay_mod._PART_RE.fullmatch(n) for n in names)


def load_decisions(run_dir, manifest):
    """The bundle's decisions: decisions.jsonl when present (every line
    json.loads-ed alone, broken lines skipped — the viewrun._stream
    rule), otherwise re-derived: every record of
    replay.iter_records(run_dir, manifest) through bundle.decide fed in
    record order, with the manifest config's [bundle] hk_codes (the
    config default when the manifest records none, e.g. a log-only
    bundle: the codes every run records unless configured otherwise;
    the S1 extractor is stateful). Raises
    CompareError when the re-derive needs the log and the slice is
    gone (compare() names the side); a bundle with decisions but an
    unreadable log still compares."""
    try:
        with open(os.path.join(run_dir, bundle_mod.DECISIONS),
                  encoding="utf-8") as f:
            lines = f.readlines()
    except OSError:
        lines = None
    if lines is not None:
        out = []
        for line in lines:
            line = line.strip()
            if not line:
                continue
            try:
                out.append(json.loads(line))
            except ValueError:
                continue
        return out
    cfg = (manifest or {}).get("config")
    cfg = cfg if isinstance(cfg, dict) else {}
    bun = cfg.get("bundle")
    bun = bun if isinstance(bun, dict) else {}
    codes = bun.get("hk_codes")
    if codes is None:
        codes = config_mod.DEFAULTS["bundle"]["hk_codes"]
    state = bundle_mod.new_state(codes)
    out = []
    try:
        for obj in replay_mod.iter_records(run_dir, manifest):
            out.extend(bundle_mod.decide(obj, state))
    except replay_mod.ReplayError as e:
        raise CompareError(str(e))
    return out


# -- pass 1: the decisions ---------------------------------------------------

def _s(rel):
    """An anchor-relative time in s, rounded to ms; None stays None."""
    return None if rel is None else round(rel / 1e9, 3)


def _check_of(d):
    """"<class> <key>" for events (e.g. "mode 0x0021"), "<kind> <key>"
    otherwise (the decision's class is None there)."""
    return "%s %s" % (d.get("class") or d["kind"], d["key"])


def _decisions(a, b, tol):
    """Pass 1 with the R1 pairing: the anchor is the earliest baseline
    decision whose (kind, key) occurs in the other list (the other
    side's anchor is its first occurrence of that key); the baseline is
    then walked in order, each decision matched to the earliest
    unmatched other decision with the same (kind, key) whose
    anchor-relative time is within tol_event_s — same when the details
    are equal, differs when not; no match is only-baseline, and the
    unmatched other decisions are only-other. No early stop. Returns
    (rows, first_divergence, align, pairs)."""
    tol_ns = tol["tol_event_s"] * 1e9
    tol_s = tol["tol_event_s"]
    bkeys = {(d["kind"], d["key"]) for d in b}
    ai = next((i for i, d in enumerate(a)
               if (d["kind"], d["key"]) in bkeys), None)
    if ai is None:                  # no common key: no anchor, shift 0
        ta0 = tb0 = 0
        pairs, a_only = [], list(range(len(a)))
        b_only = list(range(len(b)))
    else:
        ta0 = a[ai]["sim_time"]
        bj = next(j for j, o in enumerate(b)
                  if (o["kind"], o["key"]) ==
                  (a[ai]["kind"], a[ai]["key"]))
        tb0 = b[bj]["sim_time"]
        used = set()
        pairs, a_only = [], []
        # the whole baseline, the pre-anchor decisions too (negative
        # relative times): a leading one-sided decision is an
        # only-baseline row that counts (owner decision 6)
        for i in range(len(a)):
            d = a[i]
            ra = d["sim_time"] - ta0
            match = next((j for j, o in enumerate(b)
                          if j not in used and
                          (o["kind"], o["key"]) ==
                          (d["kind"], d["key"]) and
                          abs(ra - (o["sim_time"] - tb0)) <= tol_ns),
                         None)
            if match is None:
                a_only.append(i)
            else:
                used.add(match)
                pairs.append((i, match))
        b_only = [j for j in range(len(b)) if j not in used]
    rows = []           # (rel_ns, group, n, row, first-divergence datum)

    def add(rel, group, n, row, fd):
        rows.append((rel, group, n, row, fd))

    for n, i in enumerate(a_only):
        d = a[i]
        rel = d["sim_time"] - ta0
        add(rel, 0, n,
            {"check": _check_of(d), "a": _s(rel), "b": None,
             "result": "only-baseline", "tol": tol_s, "first": False},
            {"kind": d["kind"], "key": d["key"],
             "t_a": d["sim_time"], "t_b": None})
    for n, (i, j) in enumerate(pairs):
        d, o = a[i], b[j]
        rel = d["sim_time"] - ta0
        add(rel, 1, n,
            {"check": _check_of(d), "a": _s(rel),
             "b": _s(o["sim_time"] - tb0),
             "result": "same" if d["detail"] == o["detail"]
             else "differs", "tol": tol_s, "first": False},
            {"kind": d["kind"], "key": d["key"],
             "t_a": d["sim_time"], "t_b": o["sim_time"]})
    for n, j in enumerate(b_only):
        o = b[j]
        rel = o["sim_time"] - tb0
        add(rel, 2, n,
            {"check": _check_of(o), "a": None, "b": _s(rel),
             "result": "only-other", "tol": tol_s, "first": False},
            {"kind": o["kind"], "key": o["key"], "t_a": None,
             "t_b": o["sim_time"]})
    rows.sort(key=lambda x: x[:3])      # anchor-relative time order
    out = [r[3] for r in rows]
    fd = None
    for _, _, _, row, f in rows:        # the earliest non-same row
        if row["result"] != "same":
            fd = f
            row["first"] = True         # exactly one
            break
    out += _overruns(a, b, ta0, tb0, tol)
    align = {"t_a": a[ai]["sim_time"] if ai is not None else None,
             "t_b": tb0 if ai is not None else None}
    return out, fd, align, len(pairs)


def _is_overrun(d):
    return d["kind"] == "hk_step" and d["key"] == _OVERRUN_KEY and \
        d["detail"].get("value") == _OVERRUN_VALUE


def _overruns(a, b, ta0, tb0, tol):
    """The derived rows (from the full lists, independent of pass 1):
    the first hk_step to NIXD0501 == 7 per side (same iff both present
    and within tol_event_s, only-* when one side has none, differs when
    both are present but beyond tol; no row when neither has one), and
    the counts of such steps."""
    da = next((d for d in a if _is_overrun(d)), None)
    db = next((d for d in b if _is_overrun(d)), None)
    out = []
    if da is not None or db is not None:
        ra = None if da is None else da["sim_time"] - ta0
        rb = None if db is None else db["sim_time"] - tb0
        if da is None:
            res = "only-other"
        elif db is None:
            res = "only-baseline"
        elif abs(ra - rb) <= tol["tol_event_s"] * 1e9:
            res = "same"
        else:
            res = "differs"
        out.append({"check": "First overrun (hk level 7)",
                    "a": _s(ra), "b": _s(rb), "result": res,
                    "tol": tol["tol_event_s"], "first": False})
    na = sum(1 for d in a if _is_overrun(d))
    nb = sum(1 for d in b if _is_overrun(d))
    out.append({"check": "Overruns in the run", "a": na, "b": nb,
                "result": "same" if na == nb else "differs",
                "tol": None, "first": False})
    return out


# -- the pure compare ---------------------------------------------------------

def compare(baseline_dir, other_dir, tol=None):
    """Compare two finished run bundles: the decisions first (pass 1,
    _decisions) plus the derived overrun rows. tol = the [compare]
    dict; None uses the defaults. Both dirs are read-only. Raises
    CompareError (the message names the side): a dir with neither
    manifest nor log, a slice rotated away with no decisions to fall
    back on, a manifest still open. Pass 2 (the series numbers) comes
    with C3."""
    t = dict(DEFAULT_TOL)
    if tol:
        t.update(tol)
        t["series"] = list(t["series"])
    sides = []
    for side, d in (("baseline", baseline_dir), ("other", other_dir)):
        m = _read_manifest(d)
        if m is None and not _has_log(d):
            raise CompareError("%s: no run bundle at %r" % (side, d))
        if m is not None and m.get("open"):
            # a growing decisions file would make the verdict a moving
            # snapshot; the replay refuses the same way (CompareOpen:
            # 409 on the verb, R8)
            raise CompareOpen("%s: the run bundle at %r is still open"
                              % (side, d))
        try:
            sides.append(load_decisions(d, m))
        except CompareError as e:
            raise CompareError("%s: %s" % (side, e))
    rows, fd, align, pairs = _decisions(sides[0], sides[1], t)
    verdict = "same"
    for row in rows:                    # only-* counts too (owner 6)
        if row["result"] != "same":
            verdict = "differs"
            break
    return {"summary": {"verdict": verdict, "first_divergence": fd,
                        "align": align, "counts": {"pairs": pairs}},
            "rows": rows}


# -- the surfaces (C2) -------------------------------------------------------
class CompareOpen(CompareError):
    """The refusal for a manifest still open: 409 on the verb (wrong
    state, R8), exit 2 on the CLI like every other refusal."""


EXIT = {"ok": 0, "refused": 2, "infra": 3}   # the runmode convention


def _write_artifact(root, a_id, b_id, res):
    """The artifact pair (owner question 2: one pair per run pair,
    latest wins): <root>/compares/<a>-vs-<b>.jsonl (one row per line)
    and .json ({"summary", "rows"} — what a human or the board reads
    back). Both tmp + os.replace (the bundle._write_json pattern);
    never into either run dir. Returns the .json path."""
    d = os.path.join(root, "compares")
    os.makedirs(d, exist_ok=True)
    base = os.path.join(d, "%s-vs-%s" % (a_id, b_id))
    tmp = base + ".jsonl.tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        for row in res["rows"]:
            f.write(json.dumps(row, separators=(",", ":")) + "\n")
    os.replace(tmp, base + ".jsonl")
    tmp = base + ".json.tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump({"summary": res["summary"], "rows": res["rows"]},
                  f, indent=1, sort_keys=True)
        f.write("\n")
    os.replace(tmp, base + ".json")
    return base + ".json"


def run_compare(cfg, a_id, b_id, live_dir=None, out=print):
    """The compare of two finished run bundles, by run id (R8): the name
    rule and the resolve first (400/404, before any filesystem access),
    then compare() with cfg["compare"] — a CompareError is a 404 (no
    bundle at the dir, or its slice rotated away with no decisions to
    fall back on), a CompareOpen a 409 (the verb's wrong-state refusal;
    the CLI maps both to exit 2) — then the artifact pair. Returns
    {summary, rows, artifact} (the .json path)."""
    for rid in (a_id, b_id):
        bundle_mod.check_run_id(rid)
    da = bundle_mod._resolve(cfg, a_id, live_dir)
    db = bundle_mod._resolve(cfg, b_id, live_dir)
    try:
        res = compare(da, db, cfg["compare"])
    except CompareError as e:
        raise VerbError(409 if isinstance(e, CompareOpen) else 404,
                        str(e))
    artifact = _write_artifact(cfg["retention"]["run_root"], a_id, b_id,
                               res)
    if out:
        out("stixplat: compare %s vs %s: %s"
            % (a_id, b_id, res["summary"]["verdict"]))
    return {"summary": res["summary"], "rows": res["rows"],
            "artifact": artifact}


def cli(cfg, ids, out=print):
    """--compare BASELINE OTHER (the two run ids): exit 0 on a completed
    compare whichever the verdict (the verdict is data, owner question
    1), 2 refused (an unknown id, a bundle still open or unusable), 3
    infra. Prints the verdict, the rows and the artifact path."""
    try:
        res = run_compare(cfg, ids[0], ids[1], out=out)
    except VerbError as e:
        out("stixplat: %s" % e)
        return EXIT["refused"]
    except Exception as e:
        out("stixplat: %s: %s" % (type(e).__name__, e))
        return EXIT["infra"]
    for row in res["rows"]:
        out("  %s: a=%s b=%s -> %s%s"
            % (row["check"], row["a"], row["b"], row["result"],
               " (first)" if row["first"] else ""))
    out("stixplat: artifact %s" % res["artifact"])
    return EXIT["ok"]