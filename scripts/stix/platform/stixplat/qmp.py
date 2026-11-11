# SPDX-License-Identifier: GPL-2.0-or-later
"""The controller's QMP client (platform plan D-PLi): a reader thread,
an event queue (and an optional event callback), one command at a time
matched by "id", and the usual greeting + qmp_capabilities handshake.

QEMU allows one QMP client and the controller holds it; external tools
use the REST API. Each command's round trip is measured (D-PLr: QMP is
one serial channel, a slow command delays a clock sample).
"""

import json
import queue
import socket
import threading
import time


class QmpError(Exception):
    """A QMP "error" reply, a closed connection or a timeout."""

    def __init__(self, msg, klass=None):
        super().__init__(msg)
        self.klass = klass


def parse_address(addr):
    """("unix", path) or ("tcp", (host, port)); tcp:[v6]:port allowed."""
    if not addr.startswith("tcp:"):
        return "unix", addr
    rest = addr[4:]
    if rest.startswith("["):
        host, sep, tail = rest[1:].partition("]")
        port = tail[1:] if sep and tail.startswith(":") else ""
    else:
        host, _, port = rest.rpartition(":")
        if ":" in host:
            host = ""
    if not host or not port.isdigit() or not 0 < int(port) < 65536:
        raise QmpError("bad QMP address %r" % addr)
    return "tcp", (host, int(port))


class QmpConnection:
    """@sock: an already connected socket (the self-test's fake QMP).
    @on_event(msg) is called from the reader thread for every event;
    events are also queued (self.events)."""

    def __init__(self, address=None, sock=None, timeout=5.0,
                 on_event=None):
        self.address = address
        self.sock = sock
        self.timeout = timeout
        self.on_event = on_event
        self.events = queue.Queue()
        self.replies = queue.Queue()
        self.lock = threading.Lock()  # one command at a time
        self.next_id = 1
        self.reader = None
        self.closed = threading.Event()
        self.rtt_min = None
        self.rtt_max = None
        self.rtt_last = None

    # -- connection -------------------------------------------------------
    def connect(self, retry_s=None):
        """Connect (retrying for @retry_s while QEMU starts), read the
        greeting and negotiate capabilities."""
        deadline = time.monotonic() + (self.timeout if retry_s is None
                                       else retry_s)
        if self.sock is None:
            kind, addr = parse_address(self.address)
            while True:
                try:
                    if kind == "tcp":
                        s = socket.create_connection(addr, timeout=1.0)
                    else:
                        s = socket.socket(socket.AF_UNIX,
                                          socket.SOCK_STREAM)
                        try:
                            s.connect(addr)
                        except OSError:
                            s.close()
                            raise
                    self.sock = s
                    break
                except OSError as e:
                    if time.monotonic() >= deadline:
                        raise QmpError("cannot connect to %s: %s"
                                       % (self.address, e))
                    time.sleep(0.05)
        self.sock.settimeout(None)
        self.reader = threading.Thread(target=self._read_loop,
                                       name="qmp-reader", daemon=True)
        self.reader.start()
        greeting = self._get_reply(time.monotonic() + self.timeout)
        if "QMP" not in greeting:
            raise QmpError("no QMP greeting: %s" % json.dumps(greeting))
        self.execute("qmp_capabilities")

    def _read_loop(self):
        buf = bytearray()
        try:
            while True:
                chunk = self.sock.recv(65536)
                if not chunk:
                    break
                buf += chunk
                while True:
                    nl = buf.find(b"\n")
                    if nl < 0:
                        break
                    line = bytes(buf[:nl]).strip()
                    del buf[:nl + 1]
                    if not line:
                        continue
                    try:
                        msg = json.loads(line)
                    except ValueError:
                        continue
                    if "event" in msg:
                        self.events.put(msg)
                        if self.on_event is not None:
                            try:
                                self.on_event(msg)
                            except Exception:  # never kill the reader
                                pass
                    else:
                        self.replies.put(msg)
        except OSError:
            pass
        self.closed.set()
        self.replies.put(None)

    def _get_reply(self, deadline):
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise QmpError("no QMP reply within %.1f s" % self.timeout)
            try:
                msg = self.replies.get(timeout=left)
            except queue.Empty:
                raise QmpError("no QMP reply within %.1f s" % self.timeout)
            if msg is None:
                self.replies.put(None)  # stays closed for later callers
                raise QmpError("QMP connection closed")
            return msg

    # -- commands ---------------------------------------------------------
    def execute(self, command, arguments=None, timeout=None):
        """Run one command; return its "return" value. Replies are
        matched by "id"; an unmatched reply (a late one after a timeout)
        is dropped."""
        with self.lock:
            cid = self.next_id
            self.next_id += 1
            msg = {"execute": command, "id": cid}
            if arguments is not None:
                msg["arguments"] = arguments
            t0 = time.monotonic()
            try:
                self.sock.sendall(json.dumps(msg).encode() + b"\n")
            except OSError as e:
                raise QmpError("QMP send failed: %s" % e)
            deadline = t0 + (self.timeout if timeout is None else timeout)
            while True:
                rep = self._get_reply(deadline)
                if rep.get("id") != cid and "id" in rep:
                    continue  # a stale reply
                rtt = time.monotonic() - t0
                self.rtt_last = rtt
                self.rtt_min = rtt if self.rtt_min is None else \
                    min(self.rtt_min, rtt)
                self.rtt_max = rtt if self.rtt_max is None else \
                    max(self.rtt_max, rtt)
                if "error" in rep:
                    err = rep["error"]
                    raise QmpError("%s: %s" % (err.get("class", "?"),
                                               err.get("desc", "?")),
                                   klass=err.get("class"))
                if "return" in rep:
                    return rep["return"]
                raise QmpError("unexpected QMP message: %s"
                               % json.dumps(rep))

    def qom_get(self, path, prop, timeout=None):
        return self.execute("qom-get", {"path": path, "property": prop},
                            timeout=timeout)

    def qom_set(self, path, prop, value, timeout=None):
        return self.execute("qom-set", {"path": path, "property": prop,
                                        "value": value}, timeout=timeout)

    def close(self):
        if self.sock is not None:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.sock.close()
            self.sock = None
        if self.reader is not None:
            self.reader.join(timeout=2)
