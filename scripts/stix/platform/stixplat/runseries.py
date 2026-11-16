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
