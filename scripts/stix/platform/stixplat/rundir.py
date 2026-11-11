# SPDX-License-Identifier: GPL-2.0-or-later
"""The run dir (platform plan D-PLl): the guard, the sparse preflight,
the second-line sparse check and `fresh`.

The controller never writes flash.bin itself; QEMU makes it.
"""

import ctypes
import ctypes.util
import fnmatch
import os

GIB = 1 << 30
# the gates' own run dirs and the reference flashes
FORBIDDEN = ("/tmp/stix-*-run", "/tmp/stix-baseline-run")
PROBE_NAME = ".stixplat-sparse-probe"
# the files `fresh = true` removes (its own named files only)
FRESH_FILES = ("flash.bin", "uart.raw", "qemu.log", "log.jsonl", "tm.jsonl")
FALLOC_FL_KEEP_SIZE = 0x01
FALLOC_FL_PUNCH_HOLE = 0x02


class RunDirError(Exception):
    pass


def guard(path):
    """Refuse the gates' run dirs (after resolving symlinks)."""
    real = os.path.realpath(path)
    for pat in FORBIDDEN:
        if fnmatch.fnmatchcase(real.rstrip("/"), pat) or \
                fnmatch.fnmatchcase(path.rstrip("/"), pat):
            raise RunDirError("run dir %s is reserved (%s); the controller "
                              "never uses a gate's run dir" % (path, pat))
    return real


class RealFs:
    """The few filesystem calls the preflight needs (a fake replaces it
    in the self-test)."""

    def create_sparse(self, path, size):
        with open(path, "wb") as f:
            f.truncate(size)

    def write_block(self, path, offset, data):
        with open(path, "r+b") as f:
            f.seek(offset)
            f.write(data)
            f.flush()
            os.fsync(f.fileno())

    def blocks_bytes(self, path):
        return os.stat(path).st_blocks * 512

    def punch_hole(self, path, offset, length):
        libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
        libc.fallocate.argtypes = (ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int64, ctypes.c_int64)
        fd = os.open(path, os.O_RDWR)
        try:
            rc = libc.fallocate(fd, FALLOC_FL_PUNCH_HOLE |
                                FALLOC_FL_KEEP_SIZE, offset, length)
            if rc != 0:
                e = ctypes.get_errno()
                raise OSError(e, os.strerror(e))
        finally:
            os.close(fd)

    def unlink(self, path):
        os.unlink(path)

    def exists(self, path):
        return os.path.exists(path)


def preflight(run_dir, fs=None):
    """Before the first spawn: the run dir must support sparse files and
    hole punching, and an existing flash.bin must be sparse."""
    fs = fs or RealFs()
    probe = os.path.join(run_dir, PROBE_NAME)
    why = "run dir does not support sparse files or hole punching"
    try:
        fs.create_sparse(probe, GIB)
        if fs.blocks_bytes(probe) >= 16 * 1024 * 1024:
            raise RunDirError("%s (%s: a 1 GiB truncate allocated %d "
                              "bytes)" % (why, run_dir,
                                          fs.blocks_bytes(probe)))
        fs.write_block(probe, 0, b"\xa5" * 4096)
        try:
            fs.punch_hole(probe, 0, 4096)
        except OSError as e:
            raise RunDirError("%s (%s: punch hole: %s)" % (why, run_dir, e))
    except OSError as e:
        raise RunDirError("%s (%s: %s)" % (why, run_dir, e))
    finally:
        try:
            if fs.exists(probe):
                fs.unlink(probe)
        except OSError:
            pass
    sparse_check(run_dir, fs, when="existing")


def sparse_check(run_dir, fs=None, when="after power-on"):
    """flash.bin must stay under 1 GiB of real blocks."""
    fs = fs or RealFs()
    flash = os.path.join(run_dir, "flash.bin")
    if fs.exists(flash):
        used = fs.blocks_bytes(flash)
        if used >= GIB:
            raise RunDirError("%s flash.bin is not sparse: %d bytes of "
                              "blocks (limit 1 GiB)" % (when, used))
        return used
    return None


def prepare(run_dir, fresh):
    """Create the run dir; with @fresh remove only our named files."""
    os.makedirs(run_dir, exist_ok=True)
    removed = []
    if fresh:
        for name in FRESH_FILES:
            p = os.path.join(run_dir, name)
            if os.path.exists(p):
                os.unlink(p)
                removed.append(name)
    return removed
