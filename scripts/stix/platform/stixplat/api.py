# SPDX-License-Identifier: GPL-2.0-or-later
"""REST + SSE (platform plan D-PLn) behind the D-PLj request filter.

No authentication, so: localhost only; a Host header other than
localhost, 127.0.0.1 or [::1] (with an optional port) is refused (DNS
rebinding); every state change is a POST with application/json AND
`X-Stix-Platform: 1` (CSRF: a form or a simple cross-site request can
set neither); no CORS headers; SSE is GET-only and read-only.

The web UI (PL-6) is one static file served at `/`, behind the same
filter. Its Content-Security-Policy allows exactly its own inline script
and style (by hash), and network access to this origin only, so neither
an injected script nor an external resource can load.
"""

import base64
import hashlib
import json
import os
import re
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from .dispatch import VerbError

PREFIX = "/api/v1"
UI_PAGE = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "ui", "index.html"))
HOST_RE = re.compile(r"^(localhost|127\.0\.0\.1|\[::1\])(:\d{1,5})?$")
SSE_KEEPALIVE_S = 15.0


def filter_request(method, headers):
    """D-PLj. Returns None (allowed) or (status, reason)."""
    host = headers.get("Host")
    if host is None or not HOST_RE.match(host.strip().lower()):
        return 403, "Host %r is not localhost" % host
    if method not in ("GET", "POST"):
        return 405, "method not allowed"
    if method == "POST":
        ctype = (headers.get("Content-Type") or "").split(";")[0].strip()
        if ctype.lower() != "application/json":
            return 415, "POST needs Content-Type: application/json"
        if headers.get("X-Stix-Platform") != "1":
            return 403, "POST needs X-Stix-Platform: 1"
    return None


def _hashes(page, tag):
    return " ".join("'sha256-%s'" % base64.b64encode(
        hashlib.sha256(m.encode()).digest()).decode()
        for m in re.findall(r"<%s>(.*?)</%s>" % (tag, tag), page, re.S))


def page_csp(page):
    """The CSP for @page: its inline <script> and <style> blocks by
    hash, fetch/EventSource to this origin, nothing else."""
    return ("default-src 'none'; script-src %s; style-src %s; "
            "connect-src 'self'; img-src 'self' data:; base-uri 'none'; "
            "form-action 'none'; frame-ancestors 'none'"
            % (_hashes(page, "script"), _hashes(page, "style")))


def scenario_list(ctl):
    """GET /api/v1/scenarios: the scenario dir's files; a broken one is
    listed with its error (the UI shows it, scenario.run refuses it)."""
    from . import scenario as scn
    d = ctl.scenario_dir()
    out = []
    try:
        names = sorted(f for f in os.listdir(d) if f.endswith(".json"))
    except OSError as e:
        return {"dir": d, "scenarios": [], "error": str(e)}
    for f in names:
        try:
            sc = scn.load(os.path.join(d, f))
        except scn.ScenarioError as e:
            out.append({"file": f, "error": str(e)})
            continue
        out.append({"file": f, "name": sc["name"],
                    "description": sc.get("description", ""),
                    "duration_s": sc["duration_s"],
                    "variants": sc["variants"],
                    "variants_declared": sc["variants_declared"]})
    return {"dir": d, "scenarios": out}


def sse_frame(rec):
    """One SSE event: id = seq, event = topic, data = the record line
    (the same bytes as the log.jsonl line)."""
    return ("id: %d\nevent: %s\ndata: %s\n\n"
            % (rec.seq, rec.topic, rec.line)).encode()


def _no_constant(name):
    raise ValueError("%s is not JSON" % name)


def content_length(headers):
    """The body length; None for a malformed or negative header."""
    v = headers.get("Content-Length")
    if v is None:
        return 0
    v = v.strip()
    if not v.isdigit():
        return None
    return int(v)


def parse_last_id(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


class Handler(BaseHTTPRequestHandler):
    server_version = "stixplat/1"
    protocol_version = "HTTP/1.1"
    ctl = None   # set by make_server

    def log_message(self, fmt, *args):
        pass  # the record log is the log

    # -- plumbing -------------------------------------------------------------
    def _send(self, status, body, ctype="application/json", extra=()):
        if not isinstance(body, bytes):
            body = (json.dumps(body, separators=(",", ":")) + "\n").encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _err(self, status, msg):
        self._send(status, {"ok": False, "status": status, "error": msg})

    def _gate(self, method):
        bad = filter_request(method, self.headers)
        if bad is not None:
            # drain a body so the connection stays sane
            n = content_length(self.headers)
            if n is not None and 0 < n <= 1 << 20:
                self.rfile.read(n)
            self._err(*bad)
            return False
        return True

    def do_PUT(self):
        if self._gate("PUT"):
            self._err(405, "method not allowed")

    do_DELETE = do_PATCH = do_OPTIONS = do_HEAD = do_PUT

    # -- GET ------------------------------------------------------------------
    def do_GET(self):
        if not self._gate("GET"):
            return
        url = urllib.parse.urlsplit(self.path)
        q = urllib.parse.parse_qs(url.query)
        path = url.path.rstrip("/")
        ctl = self.ctl
        if path in ("", "/index.html"):
            return self._page()
        if path == "/favicon.ico":          # none: no console error
            return self._send(204, b"", "image/x-icon")
        if path == PREFIX + "/scenarios":
            return self._send(200, scenario_list(ctl))
        if path == PREFIX + "/status":
            return self._send(200, ctl.status())
        if path == PREFIX + "/time":
            return self._send(200, ctl.time_info())
        if path == PREFIX + "/faults":
            return self._send(200, ctl.plat.faults.catalogue())
        if path == PREFIX + "/verbs":
            return self._send(200, {"verbs": ctl.dispatcher.describe()})
        if path == PREFIX + "/state" or path.startswith(PREFIX + "/state/"):
            sub = path[len(PREFIX + "/state"):].lstrip("/") or None
            tree = ctl.state.get(sub)
            if tree is None:
                return self._err(404, "no state at %r" % sub)
            return self._send(200, tree)
        if path == PREFIX + "/log":
            since = parse_last_id((q.get("since") or ["0"])[0])
            if since is None:
                return self._err(400, "since must be an integer")
            kinds = _csv(q.get("kinds"))
            recs = ctl.log.since(since, kinds=kinds)
            body = "".join(r.line + "\n" for r in recs).encode()
            return self._send(200, body, "application/x-ndjson")
        if path == PREFIX + "/stream":
            return self._stream(q)
        return self._err(404, "no such endpoint")

    def _page(self):
        try:
            with open(UI_PAGE, encoding="utf-8") as f:
                page = f.read()
        except OSError as e:
            return self._err(500, "the UI page: %s" % e)
        self._send(200, page.encode(), "text/html; charset=utf-8",
                   (("Content-Security-Policy", page_csp(page)),
                    ("X-Frame-Options", "DENY"),
                    ("Referrer-Policy", "no-referrer")))

    def _stream(self, q):
        topics = _csv(q.get("topics"))
        last = parse_last_id(self.headers.get("Last-Event-ID"))
        if last is None:
            last = parse_last_id((q.get("since") or [None])[0])
        log = self.ctl.log
        if last is None:
            last = log.seq      # live from now
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        try:
            self.wfile.write(b"retry: 1000\n\n")
            self.wfile.flush()
            while not self.ctl.stopping.is_set():
                # on every round (S4): records the ring dropped before
                # this client read them are announced, never skipped
                # silently. since() returns a contiguous tail taken under
                # one lock, so its first seq tells the gap atomically.
                recs = log.since(last)
                if recs and recs[0].seq > last + 1:
                    self.wfile.write(("event: gap\ndata: %s\n\n"
                                      % json.dumps({"from": last + 1,
                                                    "to": recs[0].seq - 1}))
                                     .encode())
                for r in recs:
                    last = r.seq
                    if topics is None or r.topic in topics:
                        self.wfile.write(sse_frame(r))
                if recs:
                    self.wfile.flush()
                    continue
                if log.wait(last, SSE_KEEPALIVE_S) == last:
                    self.wfile.write(b": ka\n\n")
                    self.wfile.flush()
                if log.closed:
                    break
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    # -- POST -----------------------------------------------------------------
    def do_POST(self):
        if not self._gate("POST"):
            return
        url = urllib.parse.urlsplit(self.path)
        path = url.path.rstrip("/")
        n = content_length(self.headers)
        if n is None:
            self.close_connection = True
            return self._err(400, "bad Content-Length")
        if n > 1 << 20:
            return self._err(413, "body too large")
        raw = self.rfile.read(n) if n else b"{}"
        if not path.startswith(PREFIX + "/cmd/"):
            return self._err(404, "no such endpoint")
        verb = path[len(PREFIX + "/cmd/"):]
        try:
            args = json.loads(raw.decode() or "{}",
                              parse_constant=_no_constant)
        except ValueError as e:
            return self._err(400, "bad JSON: %s" % e)
        try:
            res = self.ctl.command(verb, args, source="api")
        except VerbError as e:
            return self._err(e.status, str(e))
        except Exception as e:   # S3: a 500 and a record, never a hang-up
            err = "%s: %s" % (type(e).__name__, e)
            self.ctl.log.emit("api", "log", "api.error",
                              {"verb": verb, "error": err})
            return self._err(500, err)
        self._send(200, {"ok": True, "verb": verb, "result": res,
                         "seq": self.ctl.log.seq})


def _csv(vals):
    if not vals:
        return None
    out = set()
    for v in vals:
        out.update(x for x in v.split(",") if x)
    return out or None


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def make_server(ctl, host, port):
    handler = type("BoundHandler", (Handler,), {"ctl": ctl})
    srv = Server((host, port), handler)
    t = threading.Thread(target=srv.serve_forever, name="api",
                         daemon=True)
    t.start()
    return srv
