# SPDX-License-Identifier: GPL-2.0-or-later
"""S3 (compare plan C3, review R10/R11): a run bundle's SeriesIndex,
rebuilt per request from the run's log slice — the data path behind
GET /runs/<run_id>/series and the compare's pass 2. No cache: a query
answers from the log as it is now, and nothing is written into the run
dir.

With a manifest the slice is that session's (start_offset inside
log.jsonl.<log_rotations>.gz, then the newer parts, then log.jsonl —
the replay loader's scoping); without one the whole log, all parts
oldest first. Broken lines are skipped (the viewrun._stream rule).
"""

import json
import os

from . import bundle as bundle_mod
from . import config as config_mod
from . import log as log_mod
from . import replay as replay_mod
from . import series as series_mod


def load_manifest(run_dir):
    """The run dir's manifest dict, or None when there is none (a
    log-only bundle, or a broken manifest)."""
    try:
        with open(os.path.join(run_dir, bundle_mod.MANIFEST),
                  encoding="utf-8") as f:
            m = json.load(f)
    except (OSError, ValueError):
        return None
    return m if isinstance(m, dict) else None


def series_index(run_dir, manifest=None):
    """A SeriesIndex fed with every record of the run's log slice, in
    record order (the loader's read, the viewrun feed without the
    record log). Raises replay.ReplayError when there is no readable
    log: a manifest whose slice rotated away or whose log.jsonl is
    gone, or no log at all."""
    if manifest is not None and not isinstance(manifest, dict):
        raise replay_mod.ReplayError("no run bundle manifest at %r"
                                     % (run_dir,))
    if manifest is None:
        paths = [p for p in replay_mod._parts(run_dir)
                 if os.path.isfile(p)]
        if not paths:
            raise replay_mod.ReplayError("no log.jsonl in %s" % run_dir)
        offset = 0
    else:
        paths, offset = replay_mod._slice(run_dir, manifest)
    idx = series_mod.SeriesIndex()
    for line in replay_mod._lines(paths, offset):
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
            log_mod.check_record(obj)
        except ValueError:
            continue
        idx.feed(obj)
    return idx


# pass 2 (compare plan C1/R10; owner decision 3): the value-compare
# rules — index-wise, counts within a slack of 2 + 5 % of the larger,
# times ignored; R4: marker/enum ids and ids with no points anywhere
# are skipped
_SKIP_KINDS = ("marker", "enum")
_COUNT_BASE, _COUNT_REL = 2, 0.05
_RAW_POINTS = 1 << 30           # query() n: no decimation, raw arrays


def _num_match(va, vb, tol_abs, tol_rel):
    return abs(va - vb) <= tol_abs + tol_rel * max(abs(va), abs(vb))


OBT_ID = "truth:platform.obt"      # coarse << 16 | fine, 2^-16 s ticks
OBT_CHECK = "OBT offset vs sim time (s)"


def _obt_offset(q):
    """(last - first OBT) - (last - first sample time), in seconds:
    the net jump the run applied to the OBT (an obt.jump fault, a time
    sync). None with fewer than two samples."""
    t, v = q["t"], q["v"]
    if len(v) < 2:
        return None
    return (v[-1] - v[0]) / 65536.0 - (t[-1] - t[0]) / 1e9


def obt_row(qa, qb, tol_s):
    """The derived OBT row: the series value compare cannot see an OBT
    jump (the relative tolerance on a 2^44-sized counter is days), so
    the net offsets compare with an absolute tol_event_s. No row when
    neither side has two OBT samples."""
    oa, ob = _obt_offset(qa), _obt_offset(qb)
    if oa is None and ob is None:
        return None
    if oa is None:
        res = "only-other"
    elif ob is None:
        res = "only-baseline"
    else:
        res = "same" if abs(oa - ob) <= tol_s else "differs"
    r3 = lambda x: None if x is None else round(x, 3)
    return {"check": OBT_CHECK, "a": r3(oa), "b": r3(ob), "result": res,
            "tol": tol_s, "first": False}


def series_rows(a_dir, b_dir, tol):
    """Pass 2 (the series numbers): both sides' index from
    series_index(d, load_manifest(d)); one row per id in tol["series"]
    with points on at least one side — check the series id, a/b the
    point counts, tol {"rel", "abs"}, first False. The value arrays
    compare index-wise up to the shorter length, a pair matching within
    tol_number_abs + tol_number_rel * max(|va|, |vb|); the counts must
    match within 2 + 5 % of the larger count, else the row differs.
    Times are not compared. A marker or enum id, and an id with no
    points anywhere, produce no row and count in series_skipped (R4).
    Returns (rows, info), info = {"series": <ids compared>,
    "series_skipped", "first_series_divergence": {check, index, t_a,
    v_a, v_b} or None}. Raises replay.ReplayError when a side has no
    readable log. A derived OBT row (obt_row) is appended after the
    series rows; it is not counted in "series"."""
    tol_rel = tol["tol_number_rel"]
    tol_abs = tol["tol_number_abs"]
    ids = list(tol["series"])
    idx_a = series_index(a_dir, load_manifest(a_dir))
    idx_b = series_index(b_dir, load_manifest(b_dir))
    qa = idx_a.query(ids + [OBT_ID], None, None, _RAW_POINTS)
    qb = idx_b.query(ids + [OBT_ID], None, None, _RAW_POINTS)
    kind_a = {e["id"]: e["kind"] for e in idx_a.catalogue()}
    kind_b = {e["id"]: e["kind"] for e in idx_b.catalogue()}
    rows, compared, skipped, first = [], [], 0, None
    for sid in ids:
        if kind_a.get(sid) in _SKIP_KINDS or kind_b.get(sid) in _SKIP_KINDS:
            skipped += 1
            continue
        ta, va = qa[sid]["t"], qa[sid]["v"]
        vb = qb[sid]["v"]
        na, nb = len(va), len(vb)
        if na == 0 and nb == 0:
            skipped += 1            # no points anywhere (R4)
            continue
        compared.append(sid)
        if na == 0 or nb == 0:
            rows.append({"check": sid, "a": na, "b": nb,
                         "result": "only-baseline" if nb == 0
                         else "only-other",
                         "tol": {"rel": tol_rel, "abs": tol_abs},
                         "first": False})
            continue
        res = "same"
        bad = None
        if abs(na - nb) > _COUNT_BASE + _COUNT_REL * max(na, nb):
            res = "differs"
        else:
            for i in range(min(na, nb)):
                if not _num_match(va[i], vb[i], tol_abs, tol_rel):
                    res = "differs"
                    bad = {"check": sid, "index": i, "t_a": ta[i],
                           "v_a": va[i], "v_b": vb[i]}
                    break
        rows.append({"check": sid, "a": na, "b": nb, "result": res,
                     "tol": {"rel": tol_rel, "abs": tol_abs},
                     "first": False})
        if bad is not None and first is None:
            first = bad
    orow = obt_row(qa[OBT_ID], qb[OBT_ID], tol.get(
        "tol_event_s", config_mod.DEFAULTS["compare"]["tol_event_s"]))
    if orow is not None:
        rows.append(orow)
    return rows, {"series": compared, "series_skipped": skipped,
                  "first_series_divergence": first}
