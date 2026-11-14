# SPDX-License-Identifier: GPL-2.0-or-later
"""The IDB-driven HK decoder (timeline plan A2): the TM(3,25) SID 1/2
fields of the ground station's stix_idb_tlm.json, so an HK report can
be turned into {code: value} series.

Pinned semantics (timeline plan, "Pinned semantics"):
- `byte_offset` is relative to the application data (the SID byte is
  byte 0, SP[16] of the space packet). Evidence: the RB level NIXD0501
  -- TM_3.c packs it into status4 bits 31..29, app byte 62 bits 7..5 --
  and the JSON says byte_offset 62 for it.
- `bit_offset` counts from the MSB of the field read big-endian; a
  field may cross bytes, so ceil((bit_offset + bit_length) / 8) bytes
  are read at byte_offset and the value is the top bit_length of them.
  Evidence: the RB level again (byte 62, bit_offset 0, bit_length 3 =
  the top bits, so 0b000_00101 is level 0), and the mode NIXD0023
  (byte 1, bit_offset 4, bit_length 4), which is decode.py's
  `app[1] & 0x0F`, the low nibble.

Calibration (HKC): the JSON's top-level "calibrations" table maps a
field's "calibration" id to one of
- polynomial: coefficients [c0, c1, ...] -> value = sum c_i * raw^i
  (CPU load CIX00041TM is [0, 4]);
- interpolation: unit + points [[raw, eng], ...] sorted by raw (the
  engineering values may descend) -> linear between the points, clamped
  outside the range;
- text: text_ranges [[lo, hi, text], ...] -> the text with
  lo <= raw <= hi, else None.
Field.cal says which ("poly"|"interp"|"text", None = raw) and
Field.unit is the field's unit or the calibration's. A field whose
calibration id is unknown, of an unknown type or a broken entry stays
raw: load() then emits one `timeline.idb` record listing them, and
never fails the load. [timeline] idb_sqlite stays reserved for curves
the JSON does not carry.
"""

import json
import math
import os
import re

CODE_RE = re.compile(r"\(([A-Z]+\d+)\)\s*$")

# (bytes, signed) of the plain (non-bit-field) data types
_TYPES = {"int8": (1, True), "uint8": (1, False),
          "int16": (2, True), "uint16": (2, False),
          "int32": (4, True), "uint32": (4, False),
          "int64": (8, True), "uint64": (8, False)}

# Readable ids for the common fields (timeline plan A2): a pinned code,
# searched in SID 2 first (the full HK, the one with the RB level), then
# SID 1. The trigger accumulator med/max are NIX00072/NIX00073.
ALIASES = {
    "hk.cpu_load": {"code": "NIXD0002"},
    "hk.rb_level": {"code": "NIXD0501"},
    "hk.trig_med": {"code": "NIX00072"},
    "hk.trig_max": {"code": "NIX00073"},
    "hk.archive_usage": {"code": "NIXD0003"},
    "hk.mode": {"code": "NIXD0023"},
}


class IdbError(ValueError):
    pass


class Field:
    """One HK field: `code` (the IDB parameter code from the name's
    "(NIX.....)"), `name`, `unit` (the JSON's, or the calibration's when
    the field has none of its own), `cal` (None|"poly"|"interp"|"text":
    None while the field has no usable calibration in the JSON's table),
    `raw` (True when cal is None: show the raw value and say "raw"
    instead of the unit) and `labels` (a text calibration's texts, in
    text_ranges order)."""

    __slots__ = ("code", "name", "unit", "cal", "raw", "labels")

    def __init__(self, code, name, unit, cal=None, labels=None):
        self.code = code
        self.name = name
        self.unit = unit
        self.cal = cal
        self.raw = cal is None
        self.labels = list(labels) if labels else []


class Idb:
    """The TM(3,25) SID 1/2 catalogue and decoder of one IDB JSON."""

    def __init__(self, sids, aliases, cals, uncal):
        self._sids = sids          # sid -> [(field, off, nbytes, shift,
        #                             mask, signed)]
        self.aliases = aliases     # alias -> (sid, code)
        self._cals = cals          # (sid, code) -> (kind, payload)
        self.uncal = uncal         # fields that stayed raw, for the log

    def hk_fields(self, sid):
        """The fields of one SID, in the JSON's order."""
        return [f for f, *_ in self._sids.get(sid, ())]

    def decode_hk(self, sid, app):
        """{code: value} of every field that fits into the app data."""
        out = {}
        for f, off, nbytes, shift, mask, signed in self._sids.get(sid, ()):
            if off + nbytes > len(app):
                continue
            v = (int.from_bytes(app[off:off + nbytes], "big") >> shift) \
                & mask
            if signed and v & ((mask + 1) >> 1):
                v -= mask + 1
            out[f.code] = v
        return out

    def calibrate(self, sid, code, raw):
        """The calibrated value of one decoded field: the number of a
        poly/interp calibration, the text of a text one, or the raw
        value when the field has no calibration."""
        cal = self._cals.get((sid, code))
        if cal is None:
            return raw
        kind, payload = cal
        if kind == "poly":
            out = 0
            for c in reversed(payload):
                out = out * raw + c
            return out
        if kind == "interp":
            return _interp(payload, raw)
        for lo, hi, text in payload:
            if lo <= raw <= hi:
                return text
        return None


def _interp(points, raw):
    """Linear between the [raw, eng] points (sorted by raw), clamped
    outside the range."""
    if raw <= points[0][0]:
        return points[0][1]
    if raw >= points[-1][0]:
        return points[-1][1]
    for (r0, e0), (r1, e1) in zip(points, points[1:]):
        if raw <= r1:
            f = (raw - r0) / (r1 - r0) if r1 > r0 else 0.0
            return e0 + (e1 - e0) * f
    return points[-1][1]


def load(json_path, sqlite_path="", log=None):
    """The Idb of one stix_idb_tlm.json, or None when @json_path is
    empty or the file is missing or broken (then one `ctl log` record
    `timeline.idb` with the reason, when @log is given). Fields with an
    unknown or broken calibration stay raw: one further `timeline.idb`
    record lists them. @sqlite_path is reserved for curves the JSON
    does not carry."""
    path = os.path.expanduser(json_path or "")
    if not path:
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        idb = _build(data)
    except (OSError, ValueError) as e:
        if log is not None:
            log.emit("ctl", "log", "timeline.idb",
                     {"idb_json": path, "error": "%s: %s"
                      % (type(e).__name__, e)})
        return None
    if idb.uncal and log is not None:
        log.emit("ctl", "log", "timeline.idb",
                 {"idb_json": path, "uncalibrated": idb.uncal})
    return idb


def _build(data):
    if not isinstance(data, dict) or \
            not isinstance(data.get("packets"), list):
        raise IdbError("no packets table")
    if data.get("byte_order") not in (None, "big"):
        raise IdbError("byte_order %r is not big-endian"
                       % data.get("byte_order"))
    cals = data.get("calibrations")
    cals = cals if isinstance(cals, dict) else {}
    sids = {}
    cal_of = {}                 # (sid, code) -> (kind, payload)
    uncal = []
    for pkt in data["packets"]:
        if not isinstance(pkt, dict) or \
                (pkt.get("service_type"), pkt.get("service_subtype"),
                 pkt.get("pi1_value")) not in ((3, 25, 1), (3, 25, 2)):
            continue                # not an HK report (stix_54101/54102)
        fields = []
        specs = pkt.get("fields")
        for spec in specs if isinstance(specs, list) else ():
            got = _field(spec, cals)
            if got is None:
                continue
            layout, why = got
            fields.append(layout[:6])
            if why is not None:
                uncal.append("SID %d %s: %s"
                             % (pkt["pi1_value"], layout[0].code, why))
            elif layout[6] is not None:
                cal_of[(pkt["pi1_value"], layout[0].code)] = layout[6]
        sids[pkt["pi1_value"]] = fields
    if 1 not in sids and 2 not in sids:
        raise IdbError("no TM(3,25) SID 1/2 packet (stix_54101/54102)")
    return Idb(sids, _resolve_aliases(sids), cal_of, uncal)


def _num(x):
    """A finite JSON number (a bool is not one; NaN/Infinity parse to
    non-finite floats and must not reach a calibration)."""
    if isinstance(x, bool):
        return False
    if isinstance(x, int):
        return abs(x) < 1e300        # bounded: float arithmetic stays sane
    return isinstance(x, float) and math.isfinite(x)


def _calibration(cid, cals):
    """(unit, cal, labels, why) of one calibration id: @cal is
    ("poly", coefficients) | ("interp", points) | ("text", ranges), or
    None with @why set when the id is unknown, the type is unknown or
    the entry is broken (the field then stays raw)."""
    c = cals.get(cid) if isinstance(cid, str) else None
    if not isinstance(c, dict):
        return None, None, None, "no calibration %r in the table" % (cid,)
    typ = c.get("type")
    if typ == "polynomial":
        co = c.get("coefficients")
        if isinstance(co, list) and co and all(_num(x) for x in co):
            return c.get("unit"), ("poly", co), None, None
        return None, None, None, "broken coefficients"
    if typ == "interpolation":
        pts = c.get("points")
        if isinstance(pts, list) and pts and all(
                isinstance(p, (list, tuple)) and len(p) == 2 and
                _num(p[0]) and _num(p[1]) for p in pts):
            return c.get("unit"), ("interp", sorted(pts)), None, None
        return None, None, None, "broken points"
    if typ == "text":
        tr = c.get("text_ranges")
        if isinstance(tr, list) and tr and all(
                isinstance(r, (list, tuple)) and len(r) == 3 and
                _num(r[0]) and _num(r[1]) and r[0] <= r[1] and
                isinstance(r[2], str) for r in tr) and \
                len({r[2] for r in tr}) == len(tr):
            return (c.get("unit"), ("text", [tuple(r) for r in tr]),
                    [r[2] for r in tr], None)
        return None, None, None, "broken text_ranges"
    return None, None, None, "unknown calibration type %r" % (typ,)


def _field(spec, cals):
    """((field, byte_offset, nbytes, shift, mask, signed, cal), why) of
    one JSON field: (None, None) when it is not usable (no "(NIX.....)"
    code, or an unknown data_type name); IdbError when it is malformed
    (a non-string name, or a coded field without byte_offset/
    data_type), so load() reports the file broken instead of raising.
    @why is set when the field's calibration is unusable (the field
    stays raw). The bit form reads ceil((bit_offset + bit_length) / 8)
    bytes (see the module docstring)."""
    if not isinstance(spec, dict):
        return None
    name = spec.get("name")
    if not isinstance(name, str):
        raise IdbError("a field name is %r, not a string" % (name,))
    m = CODE_RE.search(name)
    if m is None:
        return None
    off = spec.get("byte_offset")
    if not isinstance(off, int) or off < 0:
        raise IdbError("%s: byte_offset %r" % (name, off))
    dtype = spec.get("data_type")
    if not isinstance(dtype, str):
        raise IdbError("%s: data_type %r" % (name, dtype))
    if dtype not in _TYPES:
        return None
    cid = spec.get("calibration")
    if cid is None:
        unit, cal, labels, why = None, None, None, None
    else:
        unit, cal, labels, why = _calibration(cid, cals)
    fu = spec.get("unit") or None
    field = Field(m.group(1), name, fu or unit or None,
                  cal[0] if cal else None, labels)
    bo, bl = spec.get("bit_offset"), spec.get("bit_length")
    if isinstance(bo, int) and isinstance(bl, int) and bl > 0:
        nbytes = (bo + bl + 7) // 8
        return ((field, off, nbytes, nbytes * 8 - bo - bl,
                 (1 << bl) - 1, False, cal), why)
    size, signed = _TYPES[dtype]
    return ((field, off, size, 0, (1 << (size * 8)) - 1, signed,
             cal), why)


def _resolve_aliases(sids):
    """alias -> (sid, code): the pinned code, SID 2 first, then SID 1."""
    out = {}
    for alias, spec in ALIASES.items():
        code = spec["code"]
        for sid in (2, 1):
            if any(f.code == code for f, *_ in sids.get(sid, ())):
                out[alias] = (sid, code)
                break
    return out