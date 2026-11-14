# SPDX-License-Identifier: GPL-2.0-or-later
"""The series index (timeline plan A4): the plottable history of a run,
fed by a record-log listener (one feed per record, after the ring
append, never under the log lock) and served over /api/v1/series.

Series ids:
- `truth:<path>` and `presented:<path>` — state leaves, from `sample`
  records (the sampler's ticks) and `state` records (changes and
  snapshots, with the leaf unit when a record carries one);
  `presented:<path>` only where presented differs from truth;
- `hk:<sid>:<code>` — a decoded HK field of a `tm` record with
  data.hk (timeline A2), plus the readable aliases of idb.ALIASES
  (hk.cpu_load, hk.rb_level, ...) when an Idb is loaded; with
  data.hk_cal (HKC) a poly/interp field feeds the calibrated number
  (the unit) and a text field an enum indexed by the text_ranges
  order (the labels), so the axis order is stable; the catalogue entry
  says `cal`: "poly"|"interp"|"text"|"raw";
- marker kinds `fault`, `force`, `tc`, `event`, `mode`, `power`,
  `scenario` — one point per record, the label is the hover text
  (`TC(237,18) param 312 = 0`, `EID 0x0021 mode change`, ...);
  `scenario` = log/expect records whose path starts with `scenario.`.

Values: bool as 0/1 (kind "bool"), int/float as-is (kind "number"),
str as the index in the order first seen (kind "enum", `values` lists
the strings; an HK text calibration seeds them with the labels). A
record that carries a value of another kind than the series is
skipped. Points beyond max_points_per_series drop the oldest
(catalogue notes "truncated_before"); query() decimates a window with
more than max_points points into equal-time buckets, each bucket
returning its first t and the last/min/max value, so spikes survive.
"""

import threading

MARKERS = ("fault", "force", "tc", "event", "mode", "power", "scenario")
LABEL_CAP = 240          # a bucket's joined marker labels


class _Series:
    __slots__ = ("id", "label", "unit", "source", "kind", "values",
                 "points", "truncated_before", "cal")

    def __init__(self, sid, label, unit, source, kind):
        self.id = sid
        self.label = label
        self.unit = unit
        self.source = source
        self.kind = kind
        self.values = []            # enum strings: first-seen order, or a
        #                             text calibration's text_ranges order
        self.points = []            # (t, value) ascending, markers str
        self.truncated_before = None
        self.cal = None             # hk: "poly"|"interp"|"text"|"raw"


class SeriesIndex:
    def __init__(self, max_points_per_series=200_000, idb=None):
        self.cap = max_points_per_series
        self.lock = threading.Lock()
        self._series = {}           # id -> _Series
        self._fields = {}           # (sid, code) -> Field, for labels
        self._alias_of = {}         # (sid, code) -> alias id
        if idb is not None:
            self.set_idb(idb)

    def set_idb(self, idb):
        """The hk field labels and aliases, also when the Idb becomes
        known only after the first records (a view-run takes its config
        from the controller.start record mid-stream)."""
        if idb is None:
            return
        for sid in (1, 2):
            for f in idb.hk_fields(sid):
                self._fields[(sid, f.code)] = f
        for alias, (sid, code) in idb.aliases.items():
            self._alias_of[(sid, code)] = alias

    # -- the listener ----------------------------------------------------------
    def on_record(self, rec):
        """The record-log listener form: a log.Record; its dict feeds."""
        self.feed(rec.obj)

    def feed(self, obj):
        """One record (the dict, not the Record) under the index lock,
        so catalogue()/query() never see a series mid-change; never
        touches state.lock, never emits; a failure dies in the log's
        listener guard."""
        with self.lock:
            self._feed(obj)

    def _feed(self, obj):
        kind = obj.get("kind")
        t = obj.get("sim_time") or 0
        data = obj.get("data") or {}
        if kind == "sample":
            for k, v in data.items():
                if k.endswith("@presented"):
                    self._leaf(k[:-len("@presented")], True, t, v)
                else:
                    self._leaf(k, False, t, v)
        elif kind == "state":
            path = obj.get("path") or ""
            if data.get("snapshot"):
                for p, lf in (data.get("leaves") or {}).items():
                    if not isinstance(lf, dict):
                        continue
                    u = lf.get("unit")
                    self._leaf(p, False, t, lf.get("truth"), u)
                    if lf.get("presented") != lf.get("truth"):
                        self._leaf(p, True, t, lf.get("presented"), u)
            else:
                new = data.get("new")
                if isinstance(new, dict):
                    u = new.get("unit")
                    self._leaf(path, False, t, new.get("truth"), u)
                    if new.get("presented") != new.get("truth"):
                        self._leaf(path, True, t, new.get("presented"), u)
        elif kind == "tm":
            self._tm(t, data)
        elif kind == "tc" and obj.get("path") == "platform.keepalive":
            pass    # the TC(20,128) summaries every 10 s: noise, not events
        elif kind in ("fault", "force", "tc", "power"):
            self._marker(kind, t, obj, data)
        elif kind in ("log", "expect") and \
                str(obj.get("path") or "").startswith("scenario."):
            label = obj["path"]
            v = data.get("verdict")
            if isinstance(v, str):
                label += " " + v
            self._add_marker("scenario", t, label)

    def _leaf(self, path, presented, t, v, unit=None):
        sid = ("presented:" if presented else "truth:") + path
        if isinstance(v, bool):
            self._add_num(sid, path, unit, "state", "bool", t, int(v))
        elif isinstance(v, (int, float)):
            self._add_num(sid, path, unit, "state", "number", t, v)
        elif isinstance(v, str):
            self._add_enum(sid, path, "state", t, v, unit)

    def _tm(self, t, data):
        dec = data.get("decoded") or {}
        hk = data.get("hk")
        if isinstance(hk, dict) and dec.get("sid") in (1, 2):
            sid = dec["sid"]
            hkc = data.get("hk_cal")
            cal = hkc if isinstance(hkc, dict) else {}
            for code, v in hk.items():
                if isinstance(v, bool) or not isinstance(v, (int, float)):
                    continue
                f = self._fields.get((sid, code))
                label = code if f is None else f.name
                ids = ["hk:%d:%s" % (sid, code)]
                alias = self._alias_of.get((sid, code))
                if alias is not None:
                    ids.append(alias)
                if f is not None and f.cal == "text" and code in cal:
                    cv = cal[code]
                    if not isinstance(cv, str):
                        continue    # raw in no text range: nothing to plot
                    for i in ids:
                        self._add_hk_text(i, label, t, cv, f.labels, "text")
                    continue
                if code in cal and not isinstance(cal[code], bool) and \
                        isinstance(cal[code], (int, float)):
                    for i in ids:
                        self._add_num(i, label, f.unit if f else None,
                                      "hk", "number", t, cal[code],
                                      f.cal if f else None)
                    continue
                unit = "raw" if f is not None else None
                for i in ids:
                    self._add_num(i, label, unit, "hk", "number", t, v,
                                  "raw" if f is not None else None)
        if dec.get("type") == 5 and "eid" in dec:
            label = "EID 0x%04X" % dec["eid"]
            if dec.get("eid_name"):
                label += " " + dec["eid_name"]
            self._add_marker("event", t, label)
            if dec["eid"] == 0x21:      # mode change: its own kind too
                self._add_marker("mode", t, label)

    def _marker(self, kind, t, obj, data):
        path = str(obj.get("path") or "")
        if kind == "fault":
            short = path[len("fault."):] if path.startswith("fault.") \
                else path
            label = "fault %s %s" % (data.get("action") or "?", short or path)
        elif kind == "force":
            act = data.get("action") or "force"
            label = "force %s %s" % (act, path)
            if act == "set" and "value" in data:
                label += " = %s" % (data["value"],)
        elif kind == "tc":
            label = "TC(%s,%s)" % (data.get("service"),
                                   data.get("subtype"))
            p = data.get("param")
            if isinstance(p, dict) and "index" in p:
                label += " param %s = %s" % (p.get("index"), p.get("value"))
        else:                           # power
            label = "power %s (%s)" % (data.get("event") or "?",
                                       data.get("state") or "?")
        self._add_marker(kind, t, label)

    def _add_num(self, sid, label, unit, source, kind, t, v, cal=None):
        s = self._series.get(sid)
        if s is None:
            s = _Series(sid, label, unit, source, kind)
            s.cal = cal
            self._series[sid] = s
        elif s.kind != kind:
            return                      # one kind per series
        if s.unit is None and unit is not None:
            s.unit = unit               # e.g. a snapshot after a sample
        self._keep(s, t, v)

    def _add_hk_text(self, sid, label, t, v, labels, cal):
        """A text-calibrated HK point: the value is the text's index in
        the ranges order (@labels), not first-seen, so the axis order is
        the text_ranges order."""
        s = self._series.get(sid)
        if s is None:
            s = _Series(sid, label, None, "hk", "enum")
            s.values = list(labels)
            s.cal = cal
            self._series[sid] = s
        if s.kind != "enum" or not isinstance(v, str):
            return
        if v not in s.values:
            s.values.append(v)          # a text outside the labels
        self._keep(s, t, s.values.index(v))

    def _add_enum(self, sid, label, source, t, v, unit=None):
        s = self._series.get(sid)
        if s is None:
            s = _Series(sid, label, unit, source, "enum")
            self._series[sid] = s
        if s.kind != "enum" or not isinstance(v, str):
            return
        if s.unit is None and unit is not None:
            s.unit = unit               # e.g. a snapshot after a sample
        if v not in s.values:
            s.values.append(v)
        self._keep(s, t, s.values.index(v))

    def _add_marker(self, kind, t, label):
        s = self._series.get(kind)
        if s is None:
            s = _Series(kind, kind, None, "log", "marker")
            self._series[kind] = s
        self._keep(s, t, label)

    def _keep(self, s, t, v):
        s.points.append((t, v))
        if len(s.points) > self.cap:
            del s.points[:len(s.points) - self.cap]
            s.truncated_before = s.points[0][0]

    # -- reads -------------------------------------------------------------------
    def catalogue(self):
        """Every series: {id, label, unit, source, kind} (+ `values` for
        an enum, `truncated_before` when the cap dropped points)."""
        with self.lock:
            out = []
            for sid in sorted(self._series):
                s = self._series[sid]
                e = {"id": sid, "label": s.label, "unit": s.unit,
                     "source": s.source, "kind": s.kind}
                if s.cal is not None:
                    e["cal"] = s.cal
                if s.kind == "enum":
                    e["values"] = list(s.values)
                if s.truncated_before is not None:
                    e["truncated_before"] = s.truncated_before
                out.append(e)
            return out

    def query(self, ids, t_from=None, t_to=None, max_points=1000):
        """{id: {t, v, min?, max?, labels?}} for each id, plus
        "unknown": the ids no series carries (their bodies are empty)."""
        out = {}
        unknown = []
        with self.lock:
            for sid in ids:
                s = self._series.get(sid)
                if s is None:
                    unknown.append(sid)
                    out[sid] = {"t": [], "v": []}
                else:
                    out[sid] = self._one(s, t_from, t_to, max_points)
            out["unknown"] = unknown
        return out

    def _one(self, s, lo, hi, n):
        pts = [p for p in s.points
               if (lo is None or p[0] >= lo) and (hi is None or p[0] <= hi)]
        if s.kind == "marker":
            if len(pts) > n:
                pts = _buckets(pts, n, labels=True)
            return {"t": [p[0] for p in pts],
                    "labels": [p[1] for p in pts]}
        if s.kind in ("bool", "enum"):
            pts = _changes(pts)
        if len(pts) > n:
            b = _buckets(pts, n)
            out = {"t": [x[0] for x in b], "v": [x[1] for x in b],
                   "min": [x[2] for x in b], "max": [x[3] for x in b]}
        else:
            out = {"t": [p[0] for p in pts], "v": [p[1] for p in pts]}
        if s.kind == "enum":
            out["labels"] = list(s.values)
        return out


def _changes(pts):
    """A step series: the first point and every value change."""
    out = []
    last = object()
    for p in pts:
        if p[1] != last:
            out.append(p)
            last = p[1]
    return out


def _buckets(pts, n, labels=False):
    """Equal-time buckets of @pts (t ascending): per bucket the first t
    and the last value with its min/max, or the labels joined."""
    if n < 1:
        n = 1
    t0, t1 = pts[0][0], pts[-1][0]
    if t1 <= t0:
        if labels:
            return [(t0, _short("; ".join(p[1] for p in pts)))]
        vs = [p[1] for p in pts]
        return [(t0, vs[-1], min(vs), max(vs))]
    groups = {}
    for p in pts:
        i = (p[0] - t0) * n // (t1 - t0)
        groups.setdefault(n - 1 if i >= n else i, []).append(p)
    out = []
    for i in sorted(groups):
        g = groups[i]
        if labels:
            labs = []
            for _, lab in g:
                if lab not in labs:
                    labs.append(lab)
            out.append((g[0][0], _short("; ".join(labs))))
        else:
            vs = [p[1] for p in g]
            out.append((g[0][0], vs[-1], min(vs), max(vs)))
    return out


def _short(text):
    if len(text) <= LABEL_CAP:
        return text
    return text[:LABEL_CAP - 1] + "…"