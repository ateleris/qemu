# SPDX-License-Identifier: GPL-2.0-or-later
"""The live UI check (PL-6): the page a running controller serves at /
against the vocabulary it serves at /api/v1/verbs. core-smoke and
link-smoke run it (`stixplat.py --check-ui URL`) on the same server
their API checks use, so the gates cover the server that serves the
page."""

import base64
import hashlib
import html.parser
import json
import re
import urllib.error
import urllib.request

from . import api


class _Verbs(html.parser.HTMLParser):
    def __init__(self):
        super().__init__()
        self.verbs = set()

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if "data-verb" in a:
            self.verbs.add(a["data-verb"])


def _get(url):
    req = urllib.request.Request(url)
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def check(base):
    """(ok, why) for the controller at @base (http://host:port)."""
    st, hd, body = _get(base + "/")
    if st != 200:
        return False, "GET / answered %d" % st
    if hd.get("Content-Type") != "text/html; charset=utf-8":
        return False, "GET / is %r" % hd.get("Content-Type")
    page = body.decode("utf-8")
    pol = {x.split()[0]: set(x.split()[1:]) for x in
           (hd.get("Content-Security-Policy") or "").split(";") if x.strip()}
    for tag, key in (("script", "script-src"), ("style", "style-src")):
        # computed here, not with api.page_csp: an independent check
        want = {"'sha256-%s'" % base64.b64encode(hashlib.sha256(
            m.encode()).digest()).decode() for m in
            re.findall(r"<%s>(.*?)</%s>" % (tag, tag), page, re.S)}
        if not want or pol.get(key) != want:
            return False, "the CSP %s does not match the page" % key
    if pol.get("default-src") != {"'none'"} or \
            pol.get("connect-src") != {"'self'"}:
        return False, "the CSP is not default-src 'none', connect-src 'self'"
    p = _Verbs()
    p.feed(page)
    st, _, body = _get(base + api.PREFIX + "/verbs")
    if st != 200:
        return False, "GET /verbs answered %d" % st
    served = {v["verb"] for v in json.loads(body)["verbs"]}
    unknown = sorted(p.verbs - served)
    if not p.verbs or unknown:
        return False, "data-verb not served: %r" % unknown
    return True, "%d data-verb, all served; CSP matches" % len(p.verbs)
