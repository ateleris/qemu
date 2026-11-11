# SPDX-License-Identifier: GPL-2.0-or-later
"""The one command vocabulary (platform plan D-PLn): REST
`POST /api/v1/cmd/<verb>` and scenario steps go through this
dispatcher, with the same argument validation and the same `cmd` and
`result` records.
"""

import threading


class VerbError(Exception):
    """@status is the HTTP status (400 bad args, 403 refused, 404 no such
    verb, 409 wrong state, 500 failure)."""

    def __init__(self, status, msg):
        super().__init__(msg)
        self.status = status


ARG_TYPES = {"string": str, "integer": int, "number": (int, float),
             "boolean": bool, "object": dict}


def check_args(schema, args):
    """Validate @args against @schema {name: {type, required, enum, min,
    max, default}}; unknown args are refused. Returns the args with the
    defaults filled in."""
    if not isinstance(args, dict):
        raise VerbError(400, "args must be a JSON object")
    for k in args:
        if k not in schema:
            raise VerbError(400, "unknown argument %r" % k)
    out = {}
    for name, spec in schema.items():
        if name not in args:
            if spec.get("required"):
                raise VerbError(400, "missing argument %r" % name)
            if "default" in spec:
                out[name] = spec["default"]
            continue
        v = args[name]
        t = ARG_TYPES[spec["type"]]
        if isinstance(v, bool) and spec["type"] != "boolean":
            raise VerbError(400, "%s: expected %s" % (name, spec["type"]))
        if not isinstance(v, t):
            raise VerbError(400, "%s: expected %s" % (name, spec["type"]))
        if "enum" in spec and v not in spec["enum"]:
            raise VerbError(400, "%s: not one of %s"
                            % (name, "|".join(map(str, spec["enum"]))))
        if "min" in spec and v < spec["min"]:
            raise VerbError(400, "%s: below %s" % (name, spec["min"]))
        if "max" in spec and v > spec["max"]:
            raise VerbError(400, "%s: above %s" % (name, spec["max"]))
        out[name] = v
    return out


class Dispatcher:
    def __init__(self, log):
        self.log = log
        self.verbs = {}
        self.lock = threading.Lock()   # one command at a time

    def add(self, name, handler, args=None, doc="", enabled=None):
        """@enabled() -> None or a reason string why the verb is refused
        (403) in this configuration."""
        self.verbs[name] = {"handler": handler, "args": args or {},
                            "doc": doc, "enabled": enabled}

    def describe(self):
        """GET /api/v1/verbs: the vocabulary with arg schemas."""
        return [{"verb": n, "doc": v["doc"], "args": v["args"],
                 "enabled": v["enabled"] is None or v["enabled"]() is None}
                for n, v in sorted(self.verbs.items())]

    def run(self, verb, args, source="api"):
        """Validate, log `cmd`, run, log `result`. Returns the result
        dict; raises VerbError (also logged)."""
        v = self.verbs.get(verb)
        if v is None:
            raise VerbError(404, "no such verb %r" % verb)
        try:
            args = check_args(v["args"], args if args is not None else {})
        except VerbError as e:
            self.log.emit(source, "result", verb,
                          {"ok": False, "status": e.status,
                           "error": str(e), "args": args})
            raise
        cmd = self.log.emit(source, "cmd", verb, {"args": args})
        try:
            if v["enabled"] is not None:
                why = v["enabled"]()
                if why is not None:
                    raise VerbError(403, why)
            with self.lock:
                res = v["handler"](**args)
        except VerbError as e:
            self.log.emit(source, "result", verb,
                          {"ok": False, "status": e.status,
                           "error": str(e), "cmd_seq": cmd.seq})
            raise
        except Exception as e:  # an infrastructure failure
            self.log.emit(source, "result", verb,
                          {"ok": False, "status": 500,
                           "error": "%s: %s" % (type(e).__name__, e),
                           "cmd_seq": cmd.seq})
            raise VerbError(500, "%s: %s" % (type(e).__name__, e))
        res = res if res is not None else {}
        self.log.emit(source, "result", verb,
                      {"ok": True, "cmd_seq": cmd.seq, "result": res})
        return res
