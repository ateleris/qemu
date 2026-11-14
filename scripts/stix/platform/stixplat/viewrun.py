# SPDX-License-Identifier: GPL-2.0-or-later
"""--view-run (timeline plan A6): a finished run's log, read-only,
through the same API and page as a live controller.

The run dir's log.jsonl.N.gz parts (oldest first, the highest N) and
then log.jsonl are streamed line by line into a RecordLog without a
file (the run's own seq space, nothing appended to the run dir; the
ring keeps the newest 20000 records) and into a SeriesIndex, which
sees every record. Served on --port (default 8081, next to a live
controller). Every verb except view.* is refused (403); the views dir
and the IDB come from the run's own controller.start record, taken
while streaming, so the same views apply.
"""

import gzip
import json
import os
import re
import signal
import sys
import threading
import types

from . import api as api_mod
from . import config as config_mod
from . import idb as idb_mod
from . import log as log_mod
from . import series as series_mod
from . import views as views_mod
from .dispatch import Dispatcher, VerbError

PART_RE = re.compile(r"log\.jsonl\.(\d+)\.gz")


class ViewRunError(Exception):
    pass


def parts(run_dir):
    """The log parts of a run dir, oldest first: log.jsonl.<N>.gz by
    falling N, then log.jsonl."""
    nums = []
    try:
        for fn in os.listdir(run_dir):
            m = PART_RE.fullmatch(fn)
            if m:
                nums.append(int(m.group(1)))
    except OSError:
        pass
    return [os.path.join(run_dir, "log.jsonl.%d.gz" % n)
            for n in sorted(nums, reverse=True)] + \
        [os.path.join(run_dir, "log.jsonl")]


class _NoState:
    """No live state tree: /state answers 404."""

    def get(self, sub=None):
        return None


class ViewRun:
    """The controller-shaped object api.make_server serves."""

    def __init__(self, run_dir):
        self.run_dir = os.path.realpath(run_dir)
        self.stopping = threading.Event()
        self.skipped = 0
        self.cfg = None             # from controller.start, mid-stream
        self.state = _NoState()
        self.plat = types.SimpleNamespace(faults=types.SimpleNamespace(
            catalogue=lambda: {"catalogue": [], "active": {}}))
        self.series = series_mod.SeriesIndex()
        self._last_sim = 0
        self._last_boot = 0
        self.log = log_mod.RecordLog(
            None, lambda: (self._last_sim, self._last_boot, None, True),
            ring=20000)
        self.log.listeners.append(self.series.on_record)
        self._stream()
        if self.cfg is None:        # no controller.start record
            self.cfg = config_mod.load()
            self.series.set_idb(self._idb())
        self.dispatcher = Dispatcher(self.log)
        self.dispatcher.add("view.save", self._view_save,
                            args={"name": {"type": "string",
                                           "required": True},
                                  "view": {"type": "object",
                                           "required": True}},
                            doc="save a timeline view in the views dir")
        self.dispatcher.add("view.delete", self._view_delete,
                            args={"name": {"type": "string",
                                           "required": True}},
                            doc="delete a saved timeline view")

    def _idb(self):
        """The run's IDB, for the hk series labels; None is fine (the
        labels are then the codes) and no record is emitted."""
        return idb_mod.load(self.cfg["timeline"]["idb_json"],
                            self.cfg["timeline"]["idb_sqlite"])

    # -- streaming the log -----------------------------------------------------
    def _stream(self):
        """The log parts, oldest first, line by line into ingest; the
        ring drops the oldest, the series index still sees every
        record. A truncated or broken line is skipped, not fatal; no
        log file at all is a ViewRunError."""
        found = False
        for path in parts(self.run_dir):
            opener = gzip.open if path.endswith(".gz") else open
            try:
                with opener(path, "rt", encoding="utf-8") as f:
                    found = True
                    for line in f:
                        line = line.strip()
                        if not line:
                            continue
                        try:
                            obj = json.loads(line)
                            log_mod.check_record(obj)
                        except ValueError:
                            self.skipped += 1
                            continue
                        self._take_cfg(obj)
                        self._last_sim = obj["sim_time"]
                        self._last_boot = obj["boot_id"]
                        self.log.ingest(obj)
            except OSError:
                continue
        if not found:
            raise ViewRunError("no log.jsonl in %s" % self.run_dir)

    def _take_cfg(self, obj):
        """The run's own [timeline] and [retention] config, from its
        controller.start record (the first one), so the views dir and
        the IDB are known mid-stream; the defaults without one (or a
        config a newer controller would refuse)."""
        if self.cfg is not None or obj.get("kind") != "log" or \
                obj.get("path") != "controller.start":
            return
        cfg = (obj.get("data") or {}).get("config")
        if isinstance(cfg, dict):
            over = {k: cfg[k] for k in ("timeline", "retention")
                    if isinstance(cfg.get(k), dict)}
            try:
                self.cfg = config_mod.merged(config_mod.load(), over)
            except config_mod.ConfigError:
                pass
        if self.cfg is None:
            self.cfg = config_mod.load()
        self.series.set_idb(self._idb())

    # -- the only allowed verbs -----------------------------------------------
    def _view_save(self, name, view):
        return views_mod.save(self.cfg, name, view)

    def _view_delete(self, name):
        views_mod.delete(self.cfg, name)
        return {"deleted": name}

    # -- the controller surface the API reads ---------------------------------
    def command(self, verb, args, source="api"):
        if not verb.startswith("view."):
            raise VerbError(403, "view-run is read-only: %r is refused "
                            "(view.* only)" % verb)
        return self.dispatcher.run(verb, args, source=source)

    def scenario_dir(self):
        return os.path.join(self.run_dir, "scenarios")

    def time_info(self):
        return {"sim_time": self._last_sim, "sim_est": False,
                "boot_id": self._last_boot, "vclock_ns": None,
                "wall_time": log_mod.wall_now(), "power": "off",
                "running": False, "obt": None, "obt_kind": None,
                "t0": None}

    def status(self):
        return {"api": 1, "mode": "view", "view_run": self.run_dir,
                "run_dir": self.run_dir, "power": "off",
                "boot_id": self._last_boot, "seq": self.log.seq,
                "time": self.time_info()}


def main(run_dir, port=8081):
    try:
        vr = ViewRun(run_dir)
    except ViewRunError as e:
        print("stixplat: %s" % e, file=sys.stderr)
        return 2
    if vr.skipped:
        print("stixplat: view-run: %d broken log lines skipped"
              % vr.skipped, file=sys.stderr)
    try:
        srv = api_mod.make_server(vr, "127.0.0.1", port)
    except OSError as e:
        print("stixplat: API: %s" % e, file=sys.stderr)
        return 3
    print("stixplat: view-run %s on http://127.0.0.1:%d/api/v1"
          % (vr.run_dir, srv.server_address[1]), flush=True)
    done = threading.Event()
    signal.signal(signal.SIGTERM, lambda *a: done.set())
    signal.signal(signal.SIGINT, lambda *a: done.set())
    while not done.wait(0.5):
        pass
    vr.stopping.set()
    srv.shutdown()
    srv.server_close()
    return 0