#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""timeline_smoke: the live gate of the timeline (plan C2), API only
(urllib, no browser, no writes). Against a running controller or a
--view-run server (--base, default http://127.0.0.1:8080): load every
saved view plus the generated "everything" and assert each panel
series has points over the view's window (live trails 10 min like the
page). With no hk ids in the catalogue at all (the IDB JSON is absent,
review focus 1) the hk series are reported instead of failed.

Usage: python3 timeline_smoke.py [--base URL]
Exit codes: 0 pass, 1 a view's series failed, 2 the server is wrong.
"""
import argparse
import json
import sys
import urllib.error
import urllib.parse
import urllib.request

WINDOWS_NS = {"live": 600e9, "1m": 60e9, "10m": 600e9, "60m": 3600e9}


def get(base, path):
    req = urllib.request.Request(base + "/api/v1" + path)
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read().decode())


def main():
    ap = argparse.ArgumentParser(
        description="load every view and check its series have points")
    ap.add_argument("--base", default="http://127.0.0.1:8080")
    args = ap.parse_args()
    base = args.base.rstrip("/")
    try:
        cat = get(base, "/series/catalogue")
        views = get(base, "/views")["views"]
        now = get(base, "/time")["sim_time"]
    except (urllib.error.URLError, ValueError, KeyError) as e:
        print("ENV: %s: %s" % (type(e).__name__, e))
        return 2
    has_hk = any(e["id"].startswith("hk") for e in cat)
    if not has_hk:
        print("note: no hk ids in the catalogue: HK series need the "
              "IDB JSON")
    todo = [("everything", get(base, "/views/everything"))]
    for name in views:
        if name != "everything":
            todo.append((name, get(base, "/views/" + name)))
    failed = 0
    for name, view in todo:
        t_from = t_to = None
        w = view.get("window", "run")
        if w in WINDOWS_NS and now is not None:
            t_from = max(0, int(now - WINDOWS_NS[w]))
            t_to = int(now)
        ids = sorted({s for p in view["panels"] for s in p["series"]})
        if not ids:
            print("%s: no series" % name)
            continue
        q = [("ids", i) for i in ids]
        q += [("max_points", "1000")]
        if t_from is not None:
            q += [("from", str(t_from)), ("to", str(t_to))]
        res = get(base, "/series?" + urllib.parse.urlencode(q))
        unknown = set(res.get("unknown") or [])
        missing = []
        for sid in ids:
            if (res.get(sid) or {}).get("t"):
                continue
            if sid.startswith("hk") and not has_hk:
                print("%s: %s: no points (no IDB JSON)" % (name, sid))
            else:
                missing.append(sid)
        for sid in missing:
            print("FAIL %s: %s has no points%s"
                  % (name, sid, " (unknown id)" if sid in unknown else ""))
        print("%s: %d/%d series have points"
              % (name, len(ids) - len(missing), len(ids)))
        failed += len(missing)
    print("TIMELINE SMOKE: %s" % ("PASS" if not failed else "FAIL"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
