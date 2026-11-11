#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The STIX platform controller: the spacecraft side of the test bench.

  stixplat.py --config FILE [--run-dir DIR] [--api-port N]
  stixplat.py --self-test [CASE]

See docs/stix-port/platform.md. Stdlib only (Python >= 3.11).
Exit codes: 0 ok, 2 config error, 3 infrastructure failure.
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
    ap.add_argument("--self-test", nargs="?", const="", metavar="CASE",
                    help="run the offline self-test (optionally one case)")
    args = ap.parse_args()
    if args.self_test is not None:
        from stixplat import selftest
        return selftest.main(args.self_test or None)
    over = {}
    if args.api_port is not None:
        over["ports"] = {"api": args.api_port}
    try:
        cfg = config_mod.load(args.config, overrides=over)
    except (config_mod.ConfigError, OSError) as e:
        print("stixplat: %s" % e, file=sys.stderr)
        return 2
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
