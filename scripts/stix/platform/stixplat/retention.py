# SPDX-License-Identifier: GPL-2.0-or-later
"""Run-dir retention (platform plan D-PLl, D-PLu: the disk budget).

Run mode (PL-5) writes each scenario run under
<run_root>/runs/<UTC stamp>-<scenario>[.<variant>]/ with a marker file
`.stixplat-run` ({"status": "running"|"passed"|"failed",
"ended": <unix s>}). Only dirs that carry the marker are ever deleted;
the interactive run dir has none and is never cleaned.

Policy: keep the last `keep_runs` runs and the failed runs of the last
`keep_failed_days`; delete the rest. Then, while the run root is over
`max_total_mb`, delete the oldest passed run, then the oldest failed
one. A running run is never deleted. Sizes are real blocks
(st_blocks), so a sparse flash counts what it really costs.
"""

import json
import os
import shutil

MARKER = ".stixplat-run"


class RetentionError(Exception):
    pass


def du_bytes(path):
    total = 0
    for root, dirs, files in os.walk(path):
        for n in files + dirs:
            try:
                total += os.lstat(os.path.join(root, n)).st_blocks * 512
            except OSError:
                pass
    return total


def read_marker(run):
    try:
        with open(os.path.join(run, MARKER)) as f:
            m = json.load(f)
        return m if isinstance(m, dict) else {"status": "unknown"}
    except (OSError, ValueError):
        return None


def write_marker(run, status, ended=None):
    with open(os.path.join(run, MARKER), "w") as f:
        json.dump({"status": status, "ended": ended}, f)


def list_runs(run_root):
    """[(name, path, marker)] sorted oldest first (names are UTC
    stamps); dirs without the marker are left out."""
    runs_dir = os.path.join(run_root, "runs")
    out = []
    if not os.path.isdir(runs_dir):
        return out
    for name in sorted(os.listdir(runs_dir)):
        p = os.path.join(runs_dir, name)
        if os.path.isdir(p) and not os.path.islink(p):
            m = read_marker(p)
            if m is not None:
                out.append((name, p, m))
    return out


def plan(run_root, rc, now, sizes=None):
    """The deletions, as [(path, reason)], without deleting anything.
    @sizes maps path -> bytes (the self-test); default du."""
    runs = list_runs(run_root)
    size = (lambda p: sizes[p]) if sizes is not None else du_bytes
    keep = set(p for _, p, _ in runs[-rc["keep_runs"]:]) \
        if rc["keep_runs"] > 0 else set()
    horizon = now - rc["keep_failed_days"] * 86400
    doomed = []
    alive = []
    for name, p, m in runs:
        st = m.get("status")
        if st == "running":
            alive.append((name, p, m))
            continue
        if p in keep:
            alive.append((name, p, m))
            continue
        if st == "failed" and (m.get("ended") or 0) >= horizon:
            alive.append((name, p, m))
            continue
        doomed.append((p, "beyond keep_runs/keep_failed_days"))
    cap = rc["max_total_mb"] * 1024 * 1024
    total = sum(size(p) for _, p, _ in alive)
    for want in ("passed", "failed"):
        for name, p, m in list(alive):
            if total <= cap:
                break
            if m.get("status") != want:
                continue
            doomed.append((p, "over max_total_mb"))
            alive.remove((name, p, m))
            total -= size(p)
    return doomed


def cleanup(run_root, rc, now, dry_run=False, emit=None, sizes=None):
    """Apply plan(); each deletion is reported through @emit(path,
    reason, dry_run). Returns the plan."""
    doomed = plan(run_root, rc, now, sizes=sizes)
    for p, why in doomed:
        if emit is not None:
            emit(p, why, dry_run)
        if not dry_run:
            if read_marker(p) is None:      # never without the marker
                continue
            shutil.rmtree(p)
    return doomed


def finish_run(run, passed, keep_flash, now):
    """The end of a scenario run: the marker, and the flash per
    keep_flash ("failed": keep it only for a failed run)."""
    write_marker(run, "passed" if passed else "failed", ended=now)
    flash = os.path.join(run, "flash.bin")
    drop = keep_flash == "never" or (keep_flash == "failed" and passed)
    if drop and os.path.exists(flash):
        os.unlink(flash)
        return True
    return False


def check_free(run_root, min_free_mb, statvfs=os.statvfs):
    """Refuse a run when the free space under the run root is below
    min_free_mb (measured inside the volume)."""
    st = statvfs(run_root)
    free = st.f_bavail * st.f_frsize
    if free < min_free_mb * 1024 * 1024:
        raise RetentionError("free space under %s is %d MB, below "
                             "min_free_mb %d" % (run_root, free >> 20,
                                                 min_free_mb))
    return free
