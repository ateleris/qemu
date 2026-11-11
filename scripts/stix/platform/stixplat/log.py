# SPDX-License-Identifier: GPL-2.0-or-later
"""The record log (platform plan D-PLn, D-PLs): one schema for the SSE
events and log.jsonl.

Every record is
  {"v":1,"seq":N,"sim_time":<ns>,"boot_id":B,"vclock_ns":<ns|null>,
   "wall_time":"...Z","source":"ctl|qemu|fsw|tm|tc|scn|api",
   "kind":"cmd|result|state|power|tm|tc|fault|force|expect|mismatch|
           uart|log|debug","path":"<state path or verb>","data":{...}}
plus "sim_est": true when the controller could not sample the time
exactly. The serialised line is built once; the same bytes go to
log.jsonl, the in-memory ring and every SSE subscriber.
"""

import collections
import datetime
import gzip
import json
import os
import shutil
import threading

SCHEMA_VERSION = 1
SOURCES = ("ctl", "qemu", "fsw", "tm", "tc", "scn", "api")
KINDS = ("cmd", "result", "state", "power", "tm", "tc", "fault", "force",
         "expect", "mismatch", "uart", "log", "debug")
FIELDS = ("v", "seq", "sim_time", "boot_id", "vclock_ns", "wall_time",
          "source", "kind", "path", "data")

# D-PLn kind -> SSE topic
_TOPIC = {"cmd": "cmd", "result": "cmd", "state": "state",
          "power": "state", "fault": "fault", "force": "fault",
          "debug": "log"}
TOPICS = ("cmd", "state", "tm", "tc", "fault", "expect", "mismatch",
          "uart", "log")


def topic_of(kind):
    return _TOPIC.get(kind, kind)


def wall_now():
    return datetime.datetime.now(datetime.timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%S.%fZ")


class Record:
    """A record and its exact serialised line (no trailing newline)."""

    __slots__ = ("seq", "kind", "topic", "line", "obj")

    def __init__(self, obj):
        self.obj = obj
        self.seq = obj["seq"]
        self.kind = obj["kind"]
        self.topic = topic_of(obj["kind"])
        # strict JSON: NaN/Infinity would break every other reader
        self.line = json.dumps(obj, separators=(",", ":"), sort_keys=False,
                               allow_nan=False)


class RecordLog:
    """Append-only record stream: log.jsonl (rotated), a ring for the
    REST slice and SSE resume, and blocking reads for SSE clients.

    @time_fn returns (sim_time, boot_id, vclock_ns or None, sim_est).
    """

    def __init__(self, path=None, time_fn=None, ring=20000,
                 max_bytes=50 * 1024 * 1024, max_files=5):
        self.path = path
        self.time_fn = time_fn or (lambda: (0, 0, None, True))
        self.ring_size = ring
        self.max_bytes = max_bytes
        self.max_files = max_files
        self.ring = collections.deque()   # contiguous seqs, oldest left
        self.seq = 0
        self.cond = threading.Condition()
        self.fh = None
        self.closed = False
        if path is not None:
            self.fh = open(path, "a", encoding="utf-8")

    def emit(self, source, kind, path, data=None, sim_est=None):
        if source not in SOURCES:
            raise ValueError("bad record source %r" % source)
        if kind not in KINDS:
            raise ValueError("bad record kind %r" % kind)
        with self.cond:
            sim, boot_id, vclock, est = self.time_fn()
            self.seq += 1
            obj = {"v": SCHEMA_VERSION, "seq": self.seq,
                   "sim_time": int(sim), "boot_id": int(boot_id),
                   "vclock_ns": None if vclock is None else int(vclock),
                   "wall_time": wall_now(), "source": source,
                   "kind": kind, "path": path,
                   "data": data if data is not None else {}}
            if sim_est if sim_est is not None else est:
                obj["sim_est"] = True
            rec = Record(obj)
            if self.fh is not None:
                self.fh.write(rec.line + "\n")
                self.fh.flush()
                self._maybe_rotate()
            self.ring.append(rec)
            while len(self.ring) > self.ring_size:
                self.ring.popleft()
            self.cond.notify_all()
            return rec

    def _maybe_rotate(self):
        """D-PLl: rotate at max_bytes; the rotated part is gzipped and at
        most max_files files (current + rotated) are kept."""
        if self.max_bytes <= 0 or self.fh.tell() < self.max_bytes:
            return
        self.fh.close()
        rotated = rotated_names(self.path, self.max_files)
        # shift .N.gz -> .N+1.gz, dropping the oldest
        for i in range(len(rotated) - 1, 0, -1):
            if os.path.exists(rotated[i - 1]):
                os.replace(rotated[i - 1], rotated[i])
        if rotated:
            with open(self.path, "rb") as src, \
                    gzip.open(rotated[0], "wb") as dst:
                shutil.copyfileobj(src, dst)
        os.unlink(self.path)
        self.fh = open(self.path, "a", encoding="utf-8")

    def since(self, seq, kinds=None, topics=None):
        """The records after @seq still in the ring (filtered)."""
        with self.cond:
            # seqs are contiguous: walk back from the newest only as far
            # as needed (no full scan under the lock)
            out = []
            for r in reversed(self.ring):
                if r.seq <= seq:
                    break
                out.append(r)
        out.reverse()
        return [r for r in out if (kinds is None or r.kind in kinds) and
                (topics is None or r.topic in topics)]

    def oldest_seq(self):
        with self.cond:
            return self.ring[0].seq if self.ring else self.seq + 1

    def wait(self, seq, timeout):
        """Block until a record after @seq exists (or timeout/close)."""
        with self.cond:
            if self.seq <= seq and not self.closed:
                self.cond.wait(timeout)
            return self.seq

    def close(self):
        with self.cond:
            self.closed = True
            self.cond.notify_all()
            if self.fh is not None:
                self.fh.close()
                self.fh = None


def rotated_names(path, max_files):
    """log.jsonl.1.gz .. log.jsonl.<max_files-1>.gz"""
    return ["%s.%d.gz" % (path, i) for i in range(1, max(1, max_files))]


def check_record(obj):
    """The schema check of one record (self-test and readers)."""
    for f in FIELDS:
        if f not in obj:
            raise ValueError("record without %r: %r" % (f, obj))
    if obj["v"] != SCHEMA_VERSION:
        raise ValueError("record version %r" % obj["v"])
    if obj["source"] not in SOURCES or obj["kind"] not in KINDS:
        raise ValueError("record source/kind %r/%r" % (obj["source"],
                                                       obj["kind"]))
    if not isinstance(obj["data"], dict):
        raise ValueError("record data is not an object")
    if not obj["wall_time"].endswith("Z"):
        raise ValueError("wall_time without Z")
