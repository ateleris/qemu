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

Calibration: TODO, raw-only for now. The JSON carries a calibration id
per field and a "calibrations" table, and [timeline] idb_sqlite would
carry the curves too; until one is applied Field.raw stays True and a
plot shows the raw value as "raw" instead of the unit.
"""

import json
import os
import re

CODE_RE = re.compile(r"\(([A-Z]+\d+)\)\s*$")

# (bytes, signed) of the plain (non-bit-field) data types
_TYPES = {"int8": (1, True), "uint8": (1, False),
          "int16": (2, True), "uint16": (2, False),
          "int32": (4, True), "uint32": (4, False),
          "int64": (8, True), "uint64": (8, False)}

# Readable ids for the common fields (timeline plan A2): a pinned code,
# or the first SID 2/1 field whose name has all the substrings (SID 2
# first: the full HK, the one with the RB level). No HK field name has
# "rigger" today, so hk.trig_* are absent until the IDB has one.
ALIASES = {
    "hk.cpu_load": {"code": "NIXD0002"},
    "hk.rb_level": {"code": "NIXD0501"},
    "hk.trig_med": {"name": ("rigger", "med")},
    "hk.trig_max": {"name": ("rigger", "max")},
    "hk.archive_usage": {"code": "NIXD0003"},
    "hk.mode": {"code": "NIXD0023"},
}


class IdbError(ValueError):
    pass


class Field:
    """One HK field: `code` (the IDB parameter code from the name's
    "(NIX.....)"), `name`, `unit` (the JSON's, None when absent) and
    `raw` (True while no calibration is applied: show the raw value and
    say "raw" instead of the unit)."""

    __slots__ = ("code", "name", "unit", "raw")

    def __init__(self, code, name, unit, raw=True):
        self.code = code
        self.name = name
        self.unit = unit
        self.raw = raw


class Idb:
    """The TM(3,25) SID 1/2 catalogue and decoder of one IDB JSON."""

    def __init__(self, sids, aliases):
        self._sids = sids          # sid -> [(field, off, nbytes, shift,
        #                             mask, signed)]
        self.aliases = aliases     # alias -> (sid, code)

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


def load(json_path, sqlite_path="", log=None):
    """The Idb of one stix_idb_tlm.json, or None when @json_path is
    empty or the file is missing or broken (then one `ctl log` record
    `timeline.idb` with the reason, when @log is given). @sqlite_path
    is reserved for the calibration curves (TODO: raw-only)."""
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
    return idb


def _build(data):
    if not isinstance(data, dict) or \
            not isinstance(data.get("packets"), list):
        raise IdbError("no packets table")
    if data.get("byte_order") not in (None, "big"):
        raise IdbError("byte_order %r is not big-endian"
                       % data.get("byte_order"))
    sids = {}
    for pkt in data["packets"]:
        if not isinstance(pkt, dict) or \
                (pkt.get("service_type"), pkt.get("service_subtype"),
                 pkt.get("pi1_value")) not in ((3, 25, 1), (3, 25, 2)):
            continue                # not an HK report (stix_54101/54102)
        fields = []
        specs = pkt.get("fields")
        for spec in specs if isinstance(specs, list) else ():
            layout = _field(spec)
            if layout is not None:
                fields.append(layout)
        sids[pkt["pi1_value"]] = fields
    if 1 not in sids and 2 not in sids:
        raise IdbError("no TM(3,25) SID 1/2 packet (stix_54101/54102)")
    return Idb(sids, _resolve_aliases(sids))


def _field(spec):
    """(field, byte_offset, nbytes, shift, mask, signed) of one JSON
    field: None when it is not usable (no "(NIX.....)" code, or an
    unknown data_type name); IdbError when it is malformed (a non-
    string name, or a coded field without byte_offset/data_type), so
    load() reports the file broken instead of raising. The bit form
    reads ceil((bit_offset + bit_length) / 8) bytes (see the module
    docstring)."""
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
    size, signed = _TYPES[dtype]
    field = Field(m.group(1), name, spec.get("unit"))
    bo, bl = spec.get("bit_offset"), spec.get("bit_length")
    if isinstance(bo, int) and isinstance(bl, int) and bl > 0:
        nbytes = (bo + bl + 7) // 8
        return (field, off, nbytes, nbytes * 8 - bo - bl,
                (1 << bl) - 1, False)
    return (field, off, size, 0, (1 << (size * 8)) - 1, signed)


def _resolve_aliases(sids):
    """alias -> (sid, code): the first match in SID order 2, 1."""
    out = {}
    for alias, spec in ALIASES.items():
        for sid in (2, 1):
            for f, *_ in sids.get(sid, ()):
                if "code" in spec:
                    hit = f.code == spec["code"]
                else:
                    hit = all(w in f.name.lower() for w in spec["name"])
                if hit:
                    out[alias] = (sid, f.code)
                    break
            if alias in out:
                break
    return out