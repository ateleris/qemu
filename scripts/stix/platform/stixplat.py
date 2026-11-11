#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The STIX platform controller: the spacecraft side of the test bench.

  stixplat.py --config FILE [--run-dir DIR] [--api-port N]
  stixplat.py --config FILE --scenario NAME [--scenario ..] [--variant G]
  stixplat.py --self-test [CASE]

mode = "run" (or --scenario, which implies it) runs the scenarios each
in its own QEMU and exits; --run-dir then sets retention.run_root.
See docs/stix-port/platform.md. Stdlib only (Python >= 3.11).
Exit codes: 0 ok (run mode: all passed), 1 a scenario failed, 2 a
config or scenario error, 3 an infrastructure failure.
"""

import argparse
import os
import signal
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from stixplat import config as config_mod  # noqa: E402
from stixplat import core, rundir           # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--config", help="the TOML config")
    ap.add_argument("--run-dir", help="override run_dir.path")
    ap.add_argument("--api-port", type=int, help="override ports.api")
    ap.add_argument("--scenario", action="append", metavar="NAME",
                    help="run mode: a scenario file (in run.scenario_dir "
                    "unless absolute); repeatable")
    ap.add_argument("--variant", action="append", metavar="GLOB",
                    help="run mode: only these variants; repeatable")
    ap.add_argument("--self-test", nargs="?", const="", metavar="CASE",
                    help="run the offline self-test (optionally one case)")
    ap.add_argument("--check-ui", metavar="URL",
                    help="check the UI a running controller serves at URL "
                    "(http://127.0.0.1:PORT) against its verbs; exit 1 on "
                    "a mismatch")
    args = ap.parse_args()
    if args.check_ui:
        from stixplat import uicheck
        ok, why = uicheck.check(args.check_ui.rstrip("/"))
        print(("ok   " if ok else "FAIL ") + "ui: " + why)
        return 0 if ok else 1
    if args.self_test is not None:
        from stixplat import selftest
        return selftest.main(args.self_test or None)
    over = {}
    if args.api_port is not None:
        over["ports"] = {"api": args.api_port}
    try:
        cfg = config_mod.load(args.config, overrides=over)
        if args.scenario:
            cfg = config_mod.merged(cfg, {"mode": "run", "run": {
                "scenarios": args.scenario,
                "variants": args.variant or []}})
        if cfg["mode"] == "run" and args.run_dir:
            cfg = config_mod.merged(cfg, {"retention": {
                "run_root": args.run_dir}})
    except (config_mod.ConfigError, OSError) as e:
        print("stixplat: %s" % e, file=sys.stderr)
        return 2
    if cfg["mode"] == "run":
        from stixplat import runmode
        return runmode.run_all(cfg, out=lambda t: print(t, flush=True))
    try:
        ctl = core.Controller(cfg, run_dir=args.run_dir)
    except rundir.RunDirError as e:
        print("stixplat: %s" % e, file=sys.stderr)
        return 2
    done = threading.Event()
    signal.signal(signal.SIGTERM, lambda *a: done.set())
    signal.signal(signal.SIGINT, lambda *a: done.set())
    try:
        ctl.start()
    except OSError as e:
        print("stixplat: API: %s" % e, file=sys.stderr)
        ctl.shutdown("api bind failed")
        return 3
    print("stixplat: API on http://%s:%d/api/v1, run dir %s"
          % (cfg["api"]["bind"], ctl.server.server_address[1], ctl.run_dir),
          flush=True)
    while not done.wait(0.5):
        pass
    ctl.shutdown("signal")
    return 0


if __name__ == "__main__":
    sys.exit(main())
