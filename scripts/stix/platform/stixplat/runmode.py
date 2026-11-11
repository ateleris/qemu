# SPDX-License-Identifier: GPL-2.0-or-later
"""mode = "run" (platform plan D-PLh, PL-5): the batch runner.

Every scenario is loaded and checked first, so a scenario error (exit
2) runs nothing. Then each scenario variant runs in its own
controller, QEMU process and fresh run dir with a fresh flash:
<run_root>/runs/<stamp>-<name>[.<variant>]/, which carries the
retention marker (passed/failed; a passed run's flash is dropped per
retention.keep_flash). The reports go to <run_root>/reports/<stamp>/:
<name>[.<variant>].json (the Runner result), .jsonl (the log slice of
the run), summary.json and summary.txt. Retention cleans up at the end.

Exit codes: 0 all passed, 1 a scenario failed, 2 a config or scenario
error, 3 an infrastructure failure (the worst one wins).

Each variant has a wall-clock ceiling (scenario.wall_ceiling, measured
with time.monotonic: the WSL wall clock steps). Whatever goes wrong in a
run, the bench is powered off and the controller closed before the next
one; SIGTERM aborts the running variant as infra and starts no other.
"""

import json
import os
import signal
import threading
import time

from . import config as config_mod
from . import core
from . import retention
from . import rundir
from . import scenario as scn
from . import verbs as verbs_mod


def _resolve(cfg, name):
    base = cfg["run"]["scenario_dir"] or scn.DEFAULT_DIR
    return name if os.path.isabs(name) else os.path.join(base, name)


def _selected(sc, globs):
    import fnmatch
    vs = sc["variants"]
    if not globs:
        return vs
    return [v for v in vs if any(fnmatch.fnmatchcase(v, g) for g in globs)]


def _slice(run_dir, lo, hi, dest):
    n = 0
    with open(dest, "w") as out:
        for name in sorted((f for f in os.listdir(run_dir)
                            if f.startswith("log.jsonl") and
                            not f.endswith(".gz")),
                           key=lambda f: (f != "log.jsonl", f),
                           reverse=True):
            with open(os.path.join(run_dir, name)) as f:
                for line in f:
                    try:
                        seq = json.loads(line)["seq"]
                    except (ValueError, KeyError):
                        continue
                    if lo < seq <= hi:
                        out.write(line)
                        n += 1
    return n


def _failed(sc, variant, infra, why, seq_to=0):
    return {"scenario": sc["name"], "variant": variant, "verdict": "error",
            "infra": infra, "steps": [], "why": why, "seq_from": 0,
            "seq_to": seq_to}


def run_one(cfg, sc, variant, run_dir, term=None):
    """One scenario variant in its own controller; the Runner result,
    with infra set when the controller or the bench failed. The bench is
    off and the controller closed on return, and also when an exception
    (KeyboardInterrupt included) leaves. @term: the SIGTERM state."""
    term = term if term is not None else {}
    over = [sc["config"], sc["variant_config"].get(variant, {}),
            {"run_dir": {"path": run_dir, "fresh": True}}]
    rcfg = config_mod.merged(cfg, *over)
    try:
        ctl = core.Controller(rcfg, run_dir=run_dir)
    except Exception as e:
        return _failed(sc, variant, True, "controller: %s: %s"
                       % (type(e).__name__, e))
    res = None
    try:
        ctl.start(serve=cfg["run"]["serve_api"])
        runner = scn.Runner(sc, variant, scn.CtlCtx(ctl),
                            wall_s=scn.wall_ceiling(sc))
        ctl.runner = runner
        term["runner"] = runner
        if term.get("sig"):
            runner.abort("SIGTERM", infra=True)
        res = runner.run()
    except scn.ScenarioError as e:
        res = _failed(sc, variant, False, str(e), ctl.log.seq)
    except Exception as e:      # a bug or a QMP failure: never the batch
        res = _failed(sc, variant, True, "%s: %s" % (type(e).__name__, e),
                      ctl.log.seq)
    finally:
        term["runner"] = None
        try:
            ctl.shutdown("scenario %s done" % sc["name"])
        except Exception as e:
            if res is not None:
                res["infra"] = True
                res["why"] = "shutdown: %s: %s" % (type(e).__name__, e)
    return res


def run_all(cfg, out=print, now=time.time):
    """The batch; returns the exit code. @now only names the report
    dir; no duration is measured with it."""
    names = cfg["run"]["scenarios"]
    if not names:
        out("stixplat: mode = \"run\" needs run.scenarios")
        return scn.EXIT["error"]
    try:
        scs = [scn.load(_resolve(cfg, n)) for n in names]
        plan = [(sc, v) for sc in scs
                for v in _selected(sc, cfg["run"]["variants"])]
        if not plan:
            raise scn.ScenarioError("run.variants %r selects nothing"
                                    % cfg["run"]["variants"])
        for sc in scs:
            scn.check_verbs(sc, verbs_mod.VERBS)
            config_mod.merged(cfg, sc["config"])
            for over in sc["variant_config"].values():
                config_mod.merged(cfg, sc["config"], over)
    except (scn.ScenarioError, config_mod.ConfigError) as e:
        out("stixplat: %s" % e)
        return scn.EXIT["error"]
    rc = cfg["retention"]
    root = rc["run_root"]
    try:
        root = rundir.guard(root)
        os.makedirs(os.path.join(root, "runs"), exist_ok=True)
        retention.check_free(root, rc["min_free_mb"])
    except (rundir.RunDirError, retention.RetentionError, OSError) as e:
        out("stixplat: %s" % e)
        return scn.EXIT["infra"]
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime(now()))
    rdir = os.path.join(root, "reports", stamp)
    os.makedirs(rdir, exist_ok=True)
    runs = []
    term = {"runner": None, "sig": False}

    def on_term(signum, frame):
        if term["sig"]:
            raise KeyboardInterrupt     # a second SIGTERM: stop now
        term["sig"] = True
        r = term["runner"]
        if r is not None:
            r.abort("SIGTERM", infra=True)
    main = threading.current_thread() is threading.main_thread()
    old = signal.signal(signal.SIGTERM, on_term) if main else None
    try:
        worst = _batch(cfg, plan, root, rdir, stamp, now, out, term, runs)
    finally:
        if main:
            signal.signal(signal.SIGTERM, old)
    summary = {"api": 1, "stamp": stamp, "exit": worst, "runs": runs}
    with open(os.path.join(rdir, "summary.json"), "w") as f:
        json.dump(summary, f, indent=1)
    with open(os.path.join(rdir, "summary.txt"), "w") as f:
        for r in runs:
            f.write(r["line"] + "\n")
        f.write("exit %d (%d runs)\n" % (worst, len(runs)))
    try:
        retention.cleanup(root, rc, now(), emit=lambda p, why, dry:
                          out("retention: removed %s (%s)" % (p, why)))
    except OSError as e:
        out("retention: %s" % e)
    out("reports: %s" % rdir)
    return worst


def _batch(cfg, plan, root, rdir, stamp, now, out, term, runs):
    rc = cfg["retention"]
    worst = 0
    for sc, v in plan:
        name = sc["name"] + ("." + v if sc["variants_declared"] else "")
        if term["sig"]:
            out("stixplat: SIGTERM: %s and later not run" % name)
            return max(worst, scn.EXIT["infra"])
        run_dir = os.path.join(root, "runs", "%s-%s" % (stamp, name))
        os.makedirs(run_dir, exist_ok=True)
        retention.write_marker(run_dir, "running")
        res = run_one(cfg, sc, v, run_dir, term)
        code = scn.exit_code(res)
        worst = max(worst, code)
        with open(os.path.join(rdir, name + ".json"), "w") as f:
            json.dump(res, f, indent=1)
        n = _slice(run_dir, res.get("seq_from", 0), res.get("seq_to", 0),
                   os.path.join(rdir, name + ".jsonl"))
        retention.finish_run(run_dir, res["verdict"] == "pass",
                             rc["keep_flash"], now())
        bad = [s for s in res["steps"] if s["verdict"] != "pass"]
        line = "%-4s %s  %s" % (
            {"pass": "PASS", "fail": "FAIL"}.get(res["verdict"], "ERR"),
            name, (res.get("aborted") if res.get("infra") and
                   res.get("aborted") else bad[0]["why"] if bad else
                   res.get("why") or res.get("setup_error") or ""))
        out(line)
        runs.append({"scenario": sc["name"], "variant": v, "name": name,
                     "verdict": res["verdict"], "exit": code,
                     "report": name + ".json", "slice": name + ".jsonl",
                     "records": n, "run_dir": run_dir,
                     "seq_from": res.get("seq_from", 0),
                     "seq_to": res.get("seq_to", 0),
                     "steps": [s["verdict"] for s in res["steps"]],
                     "line": line})
    return worst
