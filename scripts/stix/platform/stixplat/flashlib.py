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

import ctypes
import fcntl
import hashlib
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

# -- sparse hashing (S1, run bundles) ---------------------------------------
# FS_IOC_FIEMAP = _IOWR('f', 43, struct fiemap); Linux-only, like
# rundir's fallocate
_IOC_READ = 2
_IOC_WRITE = 1


class _FiemapHeader(ctypes.Structure):
    _fields_ = [("fm_start", ctypes.c_uint64),
                ("fm_length", ctypes.c_uint64),
                ("fm_flags", ctypes.c_uint32),
                ("fm_mapped_extents", ctypes.c_uint32),
                ("fm_extent_count", ctypes.c_uint32),
                ("fm_reserved", ctypes.c_uint32)]


class _FiemapExtent(ctypes.Structure):
    _fields_ = [("fe_logical", ctypes.c_uint64),
                ("fe_physical", ctypes.c_uint64),
                ("fe_length", ctypes.c_uint64),
                ("fe_reserved", ctypes.c_uint64 * 2),
                ("fe_flags", ctypes.c_uint32),
                ("fe_reserved2", ctypes.c_uint32 * 3)]


FIEMAP_IOCTL = ((_IOC_READ | _IOC_WRITE) << 30) | \
    (ctypes.sizeof(_FiemapHeader) << 16) | (ord("f") << 8) | 43
FIEMAP_EXTENT_LAST = 0x0001
_EXTENTS_PER_CALL = 256


def _fiemap_extents(f, size):
    """[(logical offset, length)] of the mapped extents below @size (the
    caller's one stat); OSError where FIEMAP is not supported."""
    out = []
    start = 0
    while start < size:
        n = _EXTENTS_PER_CALL
        buf = (ctypes.c_char * (ctypes.sizeof(_FiemapHeader) +
                                n * ctypes.sizeof(_FiemapExtent)))()
        hdr = ctypes.cast(buf, ctypes.POINTER(_FiemapHeader)).contents
        hdr.fm_start = start
        hdr.fm_length = size - start
        hdr.fm_extent_count = n
        fcntl.ioctl(f.fileno(), FIEMAP_IOCTL, buf, True)
        base = ctypes.sizeof(_FiemapHeader)
        esz = ctypes.sizeof(_FiemapExtent)
        last = False
        for i in range(hdr.fm_mapped_extents):
            e = _FiemapExtent.from_buffer(buf, base + i * esz)
            if e.fe_length:
                out.append((e.fe_logical, e.fe_length))
            if e.fe_flags & FIEMAP_EXTENT_LAST:
                last = True
        if last or not hdr.fm_mapped_extents or not out:
            break
        start = out[-1][0] + out[-1][1]
    return out


def _hash_zeros(h, n):
    zeros = bytes(1 << 20)
    while n > 0:
        c = min(n, len(zeros))
        h.update(zeros[:c])
        n -= c


def sparse_sha256(path):
    """The sha256 of a sparse file reading only its mapped extents
    (zeros for the holes): the flash is 16.5 GB apparent and a
    materialising walk would fill the disk. Falls back to a dense read
    where FIEMAP is not available."""
    with open(path, "rb") as f:
        try:
            size = os.fstat(f.fileno()).st_size    # once, for both
            extents = _fiemap_extents(f, size)
        except OSError:
            extents = None
        h = hashlib.sha256()
        if extents is None:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
            return h.hexdigest()
        pos = 0
        for off, ln in extents:
            if off > pos:
                _hash_zeros(h, off - pos)
            f.seek(off)
            left = ln
            while left > 0:
                chunk = f.read(min(1 << 20, left))
                if not chunk:
                    break
                h.update(chunk)
                left -= len(chunk)
            pos = off + ln
        if pos < size:
            _hash_zeros(h, size - pos)
        return h.hexdigest()


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
    try:
        rec["sha256"] = sparse_sha256(src)
    except OSError as e:
        _unlink(bin_path)
        raise FlashLibError("hash %s: %s" % (src, e)) from None
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
