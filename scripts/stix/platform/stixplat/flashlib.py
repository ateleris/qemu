# SPDX-License-Identifier: GPL-2.0-or-later
"""The flash library (FF plan Task 4): named saves of a run's flash.bin
under <retention.run_root>/flash-library, so a later run starts from an
earlier one's flash state (the FSW rebuilds its file-system state from
flash at boot).

Every copy is sparse (`cp --sparse=always`, the rotb-fill-run.sh
pattern): the flash is 16.5 GB apparent and a materialised copy fills
the host disk (the WSL VHDX does not shrink, D-PLu). A copy whose real
blocks reach power.sparse_limit_mb is deleted and refused.
"""

import json
import os
import re
import subprocess
import time

from . import rundir

# 1..64 of A-Za-z0-9._-, not starting with a dot: no "", no "/", no
# "..", no hidden name
NAME_RE = re.compile(r"^[A-Za-z0-9_-][A-Za-z0-9._-]{0,63}$")
COPY_TIMEOUT_S = 120


class FlashLibError(Exception):
    pass


def library_dir(cfg):
    return os.path.join(cfg["retention"]["run_root"], "flash-library")


def check_name(name):
    """A library name; anything else raises ValueError."""
    if not isinstance(name, str) or not NAME_RE.match(name):
        raise ValueError("flash name %r: 1..64 of A-Za-z0-9._- and not "
                         "starting with '.'" % name)


def save(cfg, run_dir, name, meta):
    """Sparse-copy run_dir/flash.bin to <lib>/<name>.bin and write the
    record <lib>/<name>.json (meta plus name, created_utc,
    allocated_kib, source_run_dir). An existing name is refused."""
    check_name(name)
    src = os.path.join(run_dir, "flash.bin")
    if not os.path.isfile(src):
        raise FlashLibError("no flash.bin in %s (power on once first)"
                            % run_dir)
    lib = library_dir(cfg)
    os.makedirs(lib, exist_ok=True)
    bin_path = os.path.join(lib, name + ".bin")
    json_path = os.path.join(lib, name + ".json")
    if os.path.exists(bin_path) or os.path.exists(json_path):
        raise FileExistsError("%s already exists in %s" % (name, lib))
    _copy(src, bin_path)
    try:
        _check_copy(cfg, bin_path, src)
    except FlashLibError:
        _unlink(bin_path)
        raise
    rec = dict(meta)
    rec["name"] = name
    rec["created_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                       time.gmtime())
    rec["allocated_kib"] = os.stat(bin_path).st_blocks * 512 // 1024
    rec["source_run_dir"] = run_dir
    try:
        with open(json_path, "w") as f:
            json.dump(rec, f, indent=1, sort_keys=True)
            f.write("\n")
    except OSError:
        _unlink(json_path)
        _unlink(bin_path)
        raise
    return rec


def load(cfg, run_dir, name):
    """Sparse-copy <lib>/<name>.bin over run_dir/flash.bin (the power
    is off: QEMU must not have the file open); returns the record."""
    check_name(name)
    lib = library_dir(cfg)
    bin_path = os.path.join(lib, name + ".bin")
    json_path = os.path.join(lib, name + ".json")
    if not os.path.isfile(bin_path) or not os.path.isfile(json_path):
        raise FileNotFoundError("no flash %r in %s" % (name, lib))
    with open(json_path) as f:
        rec = json.load(f)
    dst = os.path.join(run_dir, "flash.bin")
    _copy(bin_path, dst)
    # the same sparse check as after a power-on applies to the copy
    try:
        rundir.sparse_check(run_dir, when="flash load",
                            limit=cfg["power"]["sparse_limit_mb"] << 20)
    except rundir.RunDirError as e:
        _unlink(dst)
        raise FlashLibError(str(e)) from None
    return rec


def listing(cfg):
    """The records, sorted by name (an empty or absent library is [])."""
    lib = library_dir(cfg)
    if not os.path.isdir(lib):
        return []
    recs = []
    for fn in sorted(os.listdir(lib)):
        if fn.endswith(".json"):
            with open(os.path.join(lib, fn)) as f:
                recs.append(json.load(f))
    return sorted(recs, key=lambda r: r.get("name", ""))


def _copy(src, dst):
    """cp --sparse=always (rotb-fill-run.sh's pattern), bounded in
    time; a failed or timed-out copy leaves nothing behind."""
    try:
        subprocess.run(["cp", "--sparse=always", "--", src, dst],
                       timeout=COPY_TIMEOUT_S, check=True)
    except (OSError, subprocess.SubprocessError) as e:
        _unlink(dst)
        raise FlashLibError("copy %s -> %s: %s" % (src, dst, e)) from None


def _check_copy(cfg, path, src):
    """The library copy holds the source's bytes and stays under the
    sparse limit (its own allocated-blocks check: rundir's is
    hard-wired to run_dir/flash.bin)."""
    st = os.stat(path)
    if st.st_size != os.stat(src).st_size:
        raise FlashLibError("the copy of %s has %d bytes, the source %d"
                            % (src, st.st_size, os.stat(src).st_size))
    limit = cfg["power"]["sparse_limit_mb"] << 20
    used = st.st_blocks * 512
    if used >= limit:
        raise FlashLibError("%s is not sparse: %d bytes of blocks "
                            "(limit %d)" % (path, used, limit))


def _unlink(path):
    try:
        os.unlink(path)
    except OSError:
        pass
