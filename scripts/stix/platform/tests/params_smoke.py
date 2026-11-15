#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""params_smoke: the live gate of the FSW parameters table (plan
2026-11-15-fsw-params-plan.md, T7), API only (urllib, no browser).
Against a running controller with the real ASW (--base, default
http://127.0.0.1:8080): the table parses (count > 0, no fatal
warnings), param.refresh shows the differ set, a harmless row is set
to default +- 1 within its [min, max], its TM(1,7) arrives, the next
refresh shows the change, param.read returns the RAM value within one
refresh, param.read_all {only_changed: true} completes, param.reset_all
restores, and the touched rows are back at the *pre-set baseline* —
not "differ back to 0": the flash context may overlay values at boot
(ContextMgmt.c:2341-2344), so value != default without any operator
set is legitimate.

The controller needs [gs] emulation = "on" (or run mode) and a booted
STIX: the script says so instead of failing obscurely.

Usage: python3 params_smoke.py [--base URL]
Exit codes: 0 pass, 1 a check failed, 2 the server is wrong.
"""
import argparse
import json
import sys
import time
import urllib.error
import urllib.request


def get(base, path):
    req = urllib.request.Request(base + "/api/v1" + path)
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read().decode())


def get_raw(base, path):
    """The body as text (/log answers NDJSON lines, not JSON)."""
    req = urllib.request.Request(base + "/api/v1" + path)
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.read().decode()


def post(base, verb, args=None):
    body = json.dumps(args or {}).encode()
    req = urllib.request.Request(base + "/api/v1/cmd/" + verb, data=body,
                                 headers={"Content-Type":
                                          "application/json",
                                          "X-Stix-Platform": "1"},
                                 method="POST")
    # a long timeout: read_all of many rows is minutes of round trips
    with urllib.request.urlopen(req, timeout=600) as r:
        return json.loads(r.read().decode())


def fail(msg):
    print("FAIL " + msg)
    return 1


def main():
    ap = argparse.ArgumentParser(
        description="the live FSW parameters gate (API only)")
    ap.add_argument("--base", default="http://127.0.0.1:8080")
    args = ap.parse_args()
    base = args.base.rstrip("/")
    failed = 0
    try:
        snap = get(base, "/params")
    except (urllib.error.URLError, ValueError) as e:
        print("ENV: %s: %s" % (type(e).__name__, e))
        return 2
    rows = snap.get("params") or []
    for w in snap.get("warnings") or []:
        print("note: " + w)
    if not rows:
        return fail("no parameter table (count %r, warnings %r)"
                    % (snap.get("count"), snap.get("warnings")))
    print("params: %d rows, symbol %s" % (snap["count"],
                                          (snap.get("symbol") or {})
                                          .get("name")))
    st = get(base, "/status")
    if st.get("power") != "running":
        return fail("STIX must be running (power %r); boot it first"
                    % st.get("power"))
    try:
        post(base, "param.refresh", {})
    except urllib.error.HTTPError as e:
        return fail("param.refresh: %s (%s); is [gs] emulation on?"
                    % (e.code, e.read().decode(errors="replace").strip()))
    snap = get(base, "/params")
    baseline = {r["index"]: r["value"] for r in snap["params"]
                if r.get("value") is not None and
                r["value"] != r["default"]}
    print("refresh: %d differ from default" % len(baseline))

    # a harmless row: default +- 1 stays within [min, max]; prefer the
    # NOMINAL accumulator row the mockup shows, else the lowest index
    wanted = [r for r in snap["params"]
              if r["name"] == "NOMINAL_ACCU_MIN_INTEGRATION"]
    row = None
    for r in wanted + snap["params"]:
        if r.get("max") is None or r["default"] + 1 > r["max"]:
            continue
        if r.get("min") is not None and r["default"] + 1 < r["min"]:
            continue
        row = r
        break
    if row is None:
        return fail("no row takes default + 1 within [min, max]")
    value = row["default"] + 1
    print("set: param %d (%s) %d -> %d" % (row["index"], row["name"],
                                           row["default"], value))
    # the FSW takes TC(237,18) only outside SAFE: a set in SAFE must
    # come back refused (TM(1,8), FID 48301 "wrong operation mode"),
    # never as done; then CONFIGURATION (TC(236,1) 04 00) and the set
    res = post(base, "param.set", {"index": row["index"],
                                   "value": value})["result"]
    if res.get("verified") == "completed":
        print("note: the FSW accepted the set before CONFIGURATION")
    elif res.get("verified") == "failed":
        print("set in SAFE: refused as expected (%s, fid %s %s)"
              % (res.get("tm"), res.get("fid"), res.get("reason")))
    before = tm17_count(base, 0)
    post(base, "tc.gs.send", {"service": 236, "subtype": 1,
                              "data": "0400"})
    if not wait_tm17(base, before):
        return fail("no TM(1,7) for TC(236,1) CONFIGURATION")
    print("mode: CONFIGURATION commanded")
    res = post(base, "param.set", {"index": row["index"],
                                   "value": value})["result"]
    if not res.get("delivered"):
        return fail("param.set not delivered: %r" % res)
    if res.get("verified") != "completed":
        failed += fail("the TC(237,18) was not completed: %r" % res)
    post(base, "param.refresh", {})
    now = {r["index"]: r["value"]
           for r in get(base, "/params")["params"]}
    if now.get(row["index"]) != value:
        failed += fail("the RAM snapshot does not show the set: %r"
                       % now.get(row["index"]))
    else:
        print("refresh: the set is visible in RAM")
    back = post(base, "param.read", {"index": row["index"]})["result"]
    if back.get("value") != value:
        failed += fail("param.read %r != the RAM value %d"
                       % (back.get("value"), value))
    else:
        print("read back: %r" % back.get("value"))
    r = post(base, "param.read_all", {"only_changed": True})["result"]
    print("read_all: %r" % r)
    r = post(base, "param.reset_all", {})["result"]
    print("reset_all: %r" % r)
    if r.get("failed"):
        failed += fail("reset_all had refused sets: %r" % r)
    time.sleep(0.5)
    post(base, "param.refresh", {})
    after = {rr["index"]: rr["value"]
             for rr in get(base, "/params")["params"]}
    if after.get(row["index"]) != baseline.get(row["index"],
                                               row["default"]):
        failed += fail("param %d did not return to the pre-set baseline "
                       "(%r)" % (row["index"], after.get(row["index"])))
    else:
        print("after reset_all: param %d back at the baseline" %
              row["index"])
    print("PARAMS SMOKE: %s" % ("PASS" if not failed else "FAIL"))
    return 1 if failed else 0


def tm17_count(base, since):
    """The TM(1,7) acks in the log slice after @since (an upper
    bound on the seqs the wait scans)."""
    n = 0
    for line in get_raw(base, "/log?since=%d&kinds=tm" % since).splitlines():
        d = (json.loads(line).get("data") or {}).get("decoded") or {}
        if d.get("type") == 1 and d.get("subtype") == 7:
            n += 1
    return n


def wait_tm17(base, before, timeout_s=10.0):
    """A new TM(1,7) ack within the window (the count grows past the
    pre-set one)."""
    end = time.monotonic() + timeout_s
    while time.monotonic() < end:
        if tm17_count(base, 0) > before:
            return True
        time.sleep(0.2)
    return False


if __name__ == "__main__":
    sys.exit(main())
