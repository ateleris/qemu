# SPDX-License-Identifier: GPL-2.0-or-later
"""The FSW parameters table (docs/stix-port/2026-11-15-fsw-params-plan.md):
names, defaults and min/max from the running FSW's ELF file image,
live values from guest RAM (one QMP `memsave` per refresh) and
per-row read-back over TC(237,19) -> TM(237,20). Stdlib only: the
controller image has no binutils (no readelf/nm).

The ELF is the single authority (owner decision 2026-11-15): the FSW
source and the IDB are never read at runtime (the IDB's TC(237,18)/
(237,19) JSON carries bare u16 ids with no enumeration, and
ParamMgmtDef.h's PARAM_INT32 macro order does not match a build's
table rows). A broken, stripped or ambiguous ELF degrades: an empty
table plus warnings and one `params.elf` log record, never an
exception.

"Applies" says when the FSW picks a value up. The labels are static,
from owner-verified FSW greps (2026-11-15):
- on set: ATT_*_PWM_LEVEL and ENABLED_SSID_TRANSFFER_TO_SSMM_MASK
  (ParamMgmt.c:101-112), CONFIG_CURRENT_{T1,T2,ELUT,SKY}_LUT_ID
  (ParamMgmt.c:109, LUTMgmt.c:477-510); every set also updates the HK
  monitoring (ParamMgmt.c:99);
- next NOMINAL entry: the config block in setMode (OpModeMgmt.c:
  501-568, the NOMINAL_* masks :625-637) and RCR_* via
  QLAccuDblBuffs_init -> RateCtrl_init (OpModeMgmt.c:645,
  QLAccuDblBuffs.c:103, RateCtrl.c:667-675);
- next calibration start: CALIB_* (TC(237,3) sets and restarts,
  TcService237.c:146-167; the TPG reads CALIB_QUIET_TIME when applied,
  XRayADCsCtrl.c:2596);
- at boot/INIT: CONFIG_SCRUB_SDRAM (OpModeMgmt.c:1071) and
  ASPECT_SUMMED_VALUES (OpModeMgmt.c:528 on the NOMINAL entry and
  :1067 at boot);
- everything else: unknown.
Row notes (the two nuances): TC(237,13) applies NOMINAL_ACCU_MIN/
MAX_INTEGRATION, NOMINAL_ACCU_MAX_COUNTS and NOMINAL_SUMEMASK/
SUMDMASK immediately (TcService237.c:329-341), and TC(237,10) applies
RCR_* immediately (RateCtrl.c:1304); a bare TC(237,18) does not. At
boot the FSW may overlay flash-context values onto the table
(ContextMgmt.c:2341-2344), so value != default without any operator
set is legitimate.
"""

import os
import re
import struct
import threading

from . import tcbuild
from .dispatch import VerbError
from .qmp import QmpError

SYM_RE = re.compile(r"ParamMgmt_parameters(?:\.\d+)?$")
RAM_LO, RAM_HI = 0x40000000, 0x48000000
MAX_ROWS = 8192
RAM_NAME = "params-ram.bin"     # the memsave file, in the run dir
PARAM_TM_APID = 0x5A8           # TM(237,20) (TMData.h:63, :79, :442)
READ_TIMEOUT_S = 2.0
VERIFY_TIMEOUT_S = 2.0           # TC(237,18) -> TM(1,7) | TM(1,8)
# TM(1,8) failure ids a param set meets (TMData.h FID_*; the FSW only
# accepts TC(237,18) outside SAFE: TCVerifDynamic.c:172)
FID_NAMES = {48301: "wrong operation mode"}
READ_PROGRESS = 32              # a params.read_all progress record every n

# Exact names before prefixes; the CONFIG_ prefix must come after
# CONFIG_CURRENT_ and CONFIG_SCRUB_SDRAM.
APPLIES_EXACT = (
    ("ASPECT_SUMMED_VALUES", "on NOMINAL entry and at boot"),
    ("CONFIG_SCRUB_SDRAM", "at boot"),
)
APPLIES_PREFIX = (
    ("ENABLED_SSID_", "on set"),
    ("CONFIG_CURRENT_", "on set"),
    ("CALIB_", "next calibration start"),
    ("NOMINAL_", "next NOMINAL entry"),
    ("CONFIG_", "next NOMINAL entry"),
    ("ASIC_", "next NOMINAL entry"),
    ("RCR_", "next NOMINAL entry"),
    ("IMAGING_", "next NOMINAL entry"),
    ("SPECTRO_", "next NOMINAL entry"),
    ("FLARE_SEL_", "next NOMINAL entry"),
)
APPLIES_DEFAULT = "unknown"
NOTE_237_13 = "TC(237,13) applies this immediately (TcService237.c:329-341)"
NOTE_237_10 = "TC(237,10) applies this immediately (RateCtrl.c:1304)"
IMMEDIATE_237_13 = ("NOMINAL_ACCU_MIN_INTEGRATION",
                    "NOMINAL_ACCU_MAX_INTEGRATION",
                    "NOMINAL_ACCU_MAX_COUNTS",
                    "NOMINAL_SUMEMASK", "NOMINAL_SUMDMASK")


# Rows the FSW itself rewrites at runtime: the per-source TC sequence
# counters (TCData.c:197-218 picks the SSC_for_* row per TC source and
# writes the accepted count; TCBuffer.c:297 LAST_ACCEPTED_SSC). They
# differ from the default by design; resetting them can make the FSW
# refuse the next TC as a sequence error, so they never count as
# "differ" and Reset all skips them (a single explicit param.set still
# works).
FSW_MAINTAINED_RE = re.compile(r"SSC_for_\w+|LAST_ACCEPTED_SSC")


def fsw_maintained(name):
    return bool(name) and FSW_MAINTAINED_RE.fullmatch(name) is not None


def applies_for(name):
    """(label, note) of one parameter name: exact names, then the
    prefixes (ATT only with its PWM_LEVEL suffix), else unknown."""
    for n, label in APPLIES_EXACT:
        if name == n:
            return label, None
    if name.startswith("ATT_") and name.endswith("_PWM_LEVEL"):
        return "on set", None
    for pre, label in APPLIES_PREFIX:
        if name.startswith(pre):
            return label, _applies_note(name)
    return APPLIES_DEFAULT, None


def _applies_note(name):
    if name in IMMEDIATE_237_13:
        return NOTE_237_13
    if name.startswith("RCR_"):
        return NOTE_237_10
    return None


class ElfError(ValueError):
    pass


class DwarfError(ValueError):
    pass


class elf32:
    """The minimal 32-bit big-endian ELF reader (params.elf32.read):
    the section headers, the .symtab/.strtab symbols and the PT_LOAD
    program headers of the file image. Anything else (64-bit, little
    endian, truncated) is an ElfError."""

    EH_FMT = ">3I6H"                # e_phoff, e_shoff, e_flags, then the
    SH_FMT = ">10I"                 # 6 u16s (e_ehsize..e_shstrndx). SHDR:
    SYM_FMT = ">IIIBBH"             # st_name, value, size, info, other, ndx
    PH_FMT = ">8I"                  # p_type, offset, vaddr, paddr, filesz,
    #                                 memsz, flags, align
    PT_LOAD = 1
    SHT_SYMTAB = 2

    @staticmethod
    def read(path):
        """{symbols, loads, sections, warnings, data}: the symbols are
        (name, value, size, info), the loads the PT_LOAD program
        headers, data the raw file bytes the DWARF reader walks."""
        with open(path, "rb") as f:
            data = f.read()
        if len(data) < 52 or data[:4] != b"\x7fELF":
            raise ElfError("no ELF header")
        if data[4] != 1:
            raise ElfError("not a 32-bit ELF (class %r)" % (data[4],))
        if data[5] != 2:
            raise ElfError("not big-endian (data %r)" % (data[5],))
        (e_phoff, e_shoff, _, _, e_phentsize, e_phnum,
         e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from(
            elf32.EH_FMT, data, 28)
        warnings = []
        sections = []
        names = b""
        if e_shoff and e_shentsize >= 40 and \
                e_shoff + e_shnum * e_shentsize <= len(data):
            raw = [struct.unpack_from(elf32.SH_FMT, data,
                                      e_shoff + i * e_shentsize)
                   for i in range(e_shnum)]
            if e_shstrndx < len(raw):
                s = raw[e_shstrndx]     # name, type, flags, addr, offset,
                names = data[s[4]:s[4] + s[5]]   # size, ...
            for sh_name, sh_type, _, sh_addr, sh_offset, sh_size, \
                    sh_link, _, _, sh_entsize in raw:
                sections.append({"name": _cstr(names, sh_name),
                                 "type": sh_type, "addr": sh_addr,
                                 "offset": sh_offset, "size": sh_size,
                                 "link": sh_link, "entsize": sh_entsize})
        else:
            warnings.append("no readable section headers")
        symbols = []
        symtab = next((s for s in sections
                       if s["type"] == elf32.SHT_SYMTAB), None)
        if symtab is None:
            warnings.append("no .symtab: names and defaults unavailable")
        else:
            strtab = sections[symtab["link"]] if \
                symtab["link"] < len(sections) else None
            strs = _span(data, strtab) if strtab else b""
            entsize = symtab["entsize"] or struct.calcsize(elf32.SYM_FMT)
            for off in range(symtab["offset"],
                             symtab["offset"] + symtab["size"] -
                             entsize + 1, entsize):
                st_name, st_value, st_size, st_info, _, st_shndx = \
                    struct.unpack_from(elf32.SYM_FMT, data, off)
                if st_shndx == 0:       # UNDEF
                    continue
                symbols.append((_cstr(strs, st_name), st_value, st_size,
                                st_info))
        loads = []
        if e_phoff and e_phentsize >= 32:
            for i in range(e_phnum):
                p = struct.unpack_from(elf32.PH_FMT, data,
                                       e_phoff + i * e_phentsize)
                if p[0] == elf32.PT_LOAD:
                    loads.append({"offset": p[1], "vaddr": p[2],
                                  "filesz": p[4], "memsz": p[5]})
        return {"symbols": symbols, "loads": loads, "sections": sections,
                "warnings": warnings, "data": data}


def _span(data, sec):
    """A section's bytes (bounds-checked)."""
    return data[sec["offset"]:sec["offset"] + sec["size"]]


def _cstr(blob, off):
    if off < 0 or off >= len(blob):
        return ""
    end = blob.find(b"\x00", off)
    if end < 0:
        end = len(blob)
    return blob[off:end].decode("utf-8", "replace")


def find_table(elf):
    """{vaddr, size, count, symbol} of the parameter table, or None:
    symbols matching ParamMgmt_parameters(.N)? (LOCAL or GLOBAL bind),
    a 16-byte row multiple, 1..8192 rows, in RAM. Several candidates
    keep the size-valid ones, warn and pick the largest."""
    hits = []
    for name, value, size, info in elf["symbols"]:
        if not SYM_RE.fullmatch(name) or (info >> 4) not in (0, 1):
            continue
        if size % 16:
            elf["warnings"].append("symbol %s: size %d is not a 16-byte "
                                   "row multiple" % (name, size))
            continue
        count = size // 16
        if not 1 <= count <= MAX_ROWS:
            # the plan's fixture table has 4 rows, so the floor is one
            # row (16 bytes), not 16 rows
            elf["warnings"].append("symbol %s: %d rows is outside "
                                   "1..%d" % (name, count, MAX_ROWS))
            continue
        if not RAM_LO <= value <= RAM_HI:
            elf["warnings"].append("symbol %s at 0x%08x is outside RAM"
                                   % (name, value))
            continue
        hits.append((name, value, size, count))
    if not hits:
        if not any("symtab" in w for w in elf["warnings"]):
            elf["warnings"].append("no ParamMgmt_parameters symbol in "
                                   "the ELF")
        return None
    if len(hits) > 1:
        elf["warnings"].append("%d ParamMgmt_parameters symbols (%s): "
                               "kept the largest" %
                               (len(hits), ", ".join(sorted(
                                   "%s@0x%08x" % (h[0], h[1])
                                   for h in hits))))
        hits.sort(key=lambda h: h[2], reverse=True)
    name, value, size, count = hits[0]
    return {"vaddr": value, "size": size, "count": count, "symbol": name}


def rows(elf, table):
    """(default, value, min, max) per row, from the PT_LOAD covering
    the table's virtual range -- the initial file image, so value ==
    default here. No covering load (BSS) is an ElfError."""
    vaddr, size = table["vaddr"], table["size"]
    for p in elf["loads"]:
        if p["vaddr"] <= vaddr and vaddr + size <= p["vaddr"] + p["filesz"]:
            off = p["offset"] + (vaddr - p["vaddr"])
            return [struct.unpack_from(">4I", elf["data"], off + 16 * i)
                    for i in range(table["count"])]
    raise ElfError("no PT_LOAD covers 0x%08x..0x%08x (BSS?)"
                   % (vaddr, vaddr + size - 1))


class dwarf:
    """The minimal DWARF reader (params.dwarf.enum_names): just enough
    of .debug_abbrev/.debug_info to find the E_PARAM_ID_* enumeration.
    An unsupported form is a DwarfError naming the form, never a wrong
    name."""

    TAG_ENUM = 0x04
    TAG_ENUMERATOR = 0x28
    AT_NAME = 0x03
    AT_CONST_VALUE = 0x1c
    # form code -> fixed size; the variable ones (string, block*,
    # sdata, udata, exprloc, indirect, implicit_const, flag_present)
    # are handled inline below
    FORM_SIZE = {0x01: 4,                 # addr
                 0x05: 2, 0x06: 4, 0x07: 8, 0x0b: 1, 0x28: 16,  # data2..16
                 0x0c: 1,                 # flag
                 0x0e: 4,                 # strp
                 0x10: 4, 0x11: 1, 0x12: 2, 0x13: 4, 0x14: 8,   # ref_addr,
                 0x17: 4,                 #   ref1/2/4/8, sec_offset
                 0x20: 8}                 # ref_sig8

    @staticmethod
    def enum_names(elf):
        """{value: "E_PARAM_ID_NAME"} of the non-empty enumeration_type
        whose enumerators are all E_PARAM_ID_* (the sentinels
        E_PARAM_ID_NONE/E_PARAM_ID_MAX ride along; the caller attaches
        names only for index < count)."""
        sec = {s["name"]: s for s in elf["sections"]}
        info, abbrev = sec.get(".debug_info"), sec.get(".debug_abbrev")
        strs = sec.get(".debug_str")
        if info is None or info["size"] == 0:
            raise DwarfError("no .debug_info")
        if abbrev is None:
            raise DwarfError("no .debug_abbrev")
        data = elf["data"]
        out = {}
        pos, end = info["offset"], info["offset"] + info["size"]
        while pos + 4 <= end:
            (unit_len,) = struct.unpack_from(">I", data, pos)
            cu_end = pos + 4 + unit_len
            if cu_end > end:
                break
            (version,) = struct.unpack_from(">H", data, pos + 4)
            if version in (2, 3, 4):
                abbrev_off, addr_size = struct.unpack_from(">IB", data,
                                                           pos + 6)
                p = pos + 11
            elif version == 5:
                # unit_length, version, unit_type, address_size,
                # abbrev_offset (compilation units; a type unit's
                # signature misparses into the outer handler's warning)
                addr_size = data[pos + 7]
                (abbrev_off,) = struct.unpack_from(">I", data, pos + 8)
                p = pos + 12
            else:
                raise DwarfError("unsupported DWARF version %d" % version)
            decls = dwarf._abbrevs(data, abbrev["offset"] + abbrev_off)
            for names in dwarf._walk(data, p, cu_end, decls, strs,
                                     addr_size):
                if len(names) > len(out):
                    out = names
            pos = cu_end
        if not out:
            raise DwarfError("no E_PARAM_ID_* enumeration in .debug_info")
        return out

    @staticmethod
    def _abbrevs(data, pos):
        """code -> (tag, has_children, [(attr, form, implicit)]) of one
        abbreviation table."""
        out = {}
        while pos < len(data):
            code, pos = _uleb(data, pos)
            if code == 0:
                break
            tag, pos = _uleb(data, pos)
            has_children = data[pos]
            pos += 1
            attrs = []
            while True:
                attr, pos = _uleb(data, pos)
                form, pos = _uleb(data, pos)
                implicit = None
                if form == 0x21:        # implicit_const: the value is
                    implicit, pos = _sleb(data, pos)   # in the abbrev
                if attr == 0 and form == 0:
                    break
                attrs.append((attr, form, implicit))
            out[code] = (tag, has_children, attrs)
        return out

    @staticmethod
    def _walk(data, pos, end, decls, strs, addr_size):
        """The matching enumeration_types of one CU: [{value: name}]
        for every non-empty enum whose enumerators are all
        E_PARAM_ID_*."""
        out = []
        stack = []                  # the open enum being filled
        while pos < end:
            code, pos = _uleb(data, pos)
            if code == 0:               # null: end of the siblings
                if stack:
                    names = stack.pop()
                    if names:
                        out.append(names)
                continue
            if code not in decls:
                raise DwarfError("unknown abbrev code %d" % code)
            tag, has_children, attrs = decls[code]
            values = {}
            for attr, form, implicit in attrs:
                v, pos = dwarf._form(data, pos, form, strs, addr_size,
                                     implicit)
                if attr == dwarf.AT_NAME:
                    values["name"] = v
                elif attr == dwarf.AT_CONST_VALUE:
                    values["value"] = v
            if tag == dwarf.TAG_ENUM:
                stack.append({})
            elif tag == dwarf.TAG_ENUMERATOR and stack:
                name, value = values.get("name"), values.get("value")
                if isinstance(name, str) and isinstance(value, int) and \
                        not isinstance(value, bool):
                    stack[-1][value] = name
            if not has_children and tag == dwarf.TAG_ENUM and stack:
                names = stack.pop()
                if names:
                    out.append(names)
        while stack:                    # a truncated CU
            names = stack.pop()
            if names:
                out.append(names)
        return [names for names in out
                if all(isinstance(n, str) and n.startswith("E_PARAM_ID_")
                       for n in names.values())]

    @staticmethod
    def _form(data, pos, form, strs, addr_size, implicit):
        """(value, next pos) of one attribute value; an unsupported
        form is a DwarfError naming it."""
        if form == 0x08:                # string
            end = data.find(b"\x00", pos)
            return data[pos:end].decode("utf-8", "replace"), end + 1
        if form == 0x09:                # block
            n, pos = _uleb(data, pos)
            return data[pos:pos + n], pos + n
        if form == 0x03:                # block2
            (n,) = struct.unpack_from(">H", data, pos)
            return data[pos + 2:pos + 2 + n], pos + 2 + n
        if form == 0x04:                # block4
            (n,) = struct.unpack_from(">I", data, pos)
            return data[pos + 4:pos + 4 + n], pos + 4 + n
        if form == 0x0a:                # block1
            n = data[pos]
            return data[pos + 1:pos + 1 + n], pos + 1 + n
        if form == 0x18:                # exprloc
            n, pos = _uleb(data, pos)
            return data[pos:pos + n], pos + n
        if form == 0x0d:                # sdata
            return _sleb(data, pos)
        if form == 0x0f:                # udata
            return _uleb(data, pos)
        if form == 0x19:                # flag_present
            return True, pos
        if form == 0x21:                # implicit_const
            return implicit, pos
        if form == 0x16:                # indirect
            form, pos = _uleb(data, pos)
            return dwarf._form(data, pos, form, strs, addr_size, implicit)
        if form == 0x0e:                # strp: an offset into .debug_str
            (off,) = struct.unpack_from(">I", data, pos)
            if strs is None:
                raise DwarfError("DW_FORM_strp without .debug_str")
            return _cstr(data[strs["offset"]:strs["offset"] +
                              strs["size"]], off), pos + 4
        size = dwarf.FORM_SIZE.get(form)
        if size is None:
            raise DwarfError("unsupported DWARF form 0x%02x" % form)
        raw = data[pos:pos + size]
        if len(raw) < size:
            raise DwarfError("truncated DWARF form 0x%02x" % form)
        if form == 0x0c:                # flag
            return bool(raw[0]), pos + size
        return int.from_bytes(raw, "big"), pos + size


def _uleb(buf, pos):
    out = shift = 0
    while True:
        b = buf[pos]
        pos += 1
        out |= (b & 0x7F) << shift
        if not b & 0x80:
            return out, pos
        shift += 7


def _sleb(buf, pos):
    out = shift = 0
    while True:
        b = buf[pos]
        pos += 1
        out |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            if b & 0x40 and shift < 64:
                out -= 1 << shift
            return out, pos


class ParamsTable:
    """The static table plus the live values learned later. @rows is a
    list of dicts the controller and the replay update in place
    (value/readback and their bench times); count, names, defaults and
    min/max never change. @warnings is the reason list /params serves."""

    def __init__(self, count, rows, warnings, symbol=None, vaddr=None):
        self.count = count
        self.rows = rows
        self.warnings = list(warnings)
        self.symbol = symbol
        self.vaddr = vaddr
        self.at_sim = None      # bench time of the last RAM snapshot

    def differ(self):
        return sum(1 for r in self.rows
                   if r["value"] is not None and r["value"] != r["default"]
                   and not r.get("maintained"))

    def summary(self):
        return {"count": self.count, "differ": self.differ(),
                "warnings": list(self.warnings)}

    def snapshot(self, elf, elf_sha256):
        return {"elf": elf, "elf_sha256": elf_sha256,
                "symbol": dict(self.symbol) if self.symbol else None,
                "count": self.count, "differ": self.differ(),
                "warnings": list(self.warnings), "at_sim": self.at_sim,
                "params": [dict(r) for r in self.rows]}


def load(path, sha256=None, log=None):
    """The ParamsTable of one FSW ELF, or an empty table with the
    reason as a warning (and one `params.elf` log record, when @log is
    given) when the file is missing or broken. Caching is the
    caller's: the controller caches by the pair (path, sha256), so a
    failed load is negative-cached and the 1 s status poll never
    re-parses. Nothing a broken file does escapes as an exception."""
    if not path:
        return _broken(None, "no ELF configured", log)
    try:
        return _parse(path, log)
    except (OSError, ValueError, IndexError, struct.error) as e:
        return _broken(path, "%s: %s" % (type(e).__name__, e), log)


def _parse(path, log):
    elf = elf32.read(path)
    tab = find_table(elf)                 # appends its reasons to
    warnings = list(elf["warnings"])      # elf["warnings"]: copy after
    if tab is None:
        return _broken(path, "no usable ParamMgmt_parameters symbol",
                       log, warnings)
    try:
        raw = rows(elf, tab)
    except (OSError, ValueError, IndexError, struct.error) as e:
        return _broken(path, "%s: %s" % (type(e).__name__, e), log,
                       warnings)
    names = {}
    try:
        names = dwarf.enum_names(elf)
    except (OSError, ValueError, IndexError, struct.error) as e:
        warnings.append("parameter names unavailable: %s" % e)
    by_name = {n: v for v, n in names.items()}
    if by_name.get("E_PARAM_ID_MAX") not in (None, tab["count"]):
        warnings.append("E_PARAM_ID_MAX = %d but the table has %d rows"
                        % (by_name["E_PARAM_ID_MAX"], tab["count"]))
    inv = [i for i, (_, _, lo, hi) in enumerate(raw) if lo > hi]
    if inv:
        warnings.append("min > max in row(s) %s" %
                        ", ".join("#%d" % i for i in inv))
    out = [i for i, (d, _, lo, hi) in enumerate(raw)
           if lo <= hi and not lo <= d <= hi]
    if out:
        warnings.append("default outside [min, max] in row(s) %s" %
                        ", ".join("#%d" % i for i in out))
    table = []
    for i, (d, _, lo, hi) in enumerate(raw):
        full = names.get(i)
        if isinstance(full, str) and full.startswith("E_PARAM_ID_"):
            full = full[len("E_PARAM_ID_"):]
        label, note = (applies_for(full) if full
                       else (APPLIES_DEFAULT, None))
        table.append({"index": i,
                      "name": full if full else "param %d" % i,
                      "default": d, "value": None, "value_at": None,
                      "readback": None, "readback_at": None,
                      "min": lo, "max": hi,
                      "applies": label, "applies_note": note,
                      "maintained": fsw_maintained(full)})
    return ParamsTable(tab["count"], table, warnings,
                       symbol={"name": tab["symbol"],
                               "address": tab["vaddr"],
                               "size": tab["size"]}, vaddr=tab["vaddr"])


def _broken(path, why, log, warnings=None):
    warnings = list(warnings) if warnings else []
    warnings.append(why)
    if log is not None:
        log.emit("ctl", "log", "params.elf", {"elf": path, "error": why})
    return ParamsTable(0, [], warnings)


def replay(recs, elf, elf_sha256):
    """A finished run's ParamsTable: the ELF's static table with the
    run's `param` records replayed on top (how "ram" fills value,
    "readback" readback; the bench time is the record's sim_time). An
    absent or unreadable ELF degrades exactly as live: an empty table
    plus warnings, the records nothing to attach to."""
    tab = load(elf, elf_sha256)
    by = {r["index"]: r for r in tab.rows}
    for rec in recs:
        d = rec.get("data") or {}
        row = by.get(d.get("index"))
        if row is None:
            continue
        at = rec.get("sim_time")
        if d.get("how") == "readback":
            row["readback"], row["readback_at"] = d.get("new"), at
        else:
            row["value"], row["value_at"] = d.get("new"), at
            if at is not None and (tab.at_sim is None or at > tab.at_sim):
                tab.at_sim = at
    return tab


class Params:
    """The controller's parameters facade (owned by Platform like
    keepalive/timesync): the cached table, the RAM refresh and the
    read-back verbs. @sha256 is the controller's per-start ELF digest
    (None for an unreadable ELF), the second half of the cache key;
    @next_seq hands out the GS-emulation TC sequence counts (the same
    counter tc.gs.send uses, under the platform lock) and @link_ok()
    says why a GS TC cannot go out now (None when it can)."""

    def __init__(self, power, relay, log, clock, cfg, sha256,
                 next_seq, link_ok):
        self.power = power
        self.relay = relay
        self.log = log
        self.clock = clock
        self.cfg = cfg
        self.sha256 = sha256
        self.next_seq = next_seq
        self.link_ok = link_ok
        self.lock = threading.Lock()
        self._key = None
        self._table = None
        self._tm_lock = threading.Lock()
        self._answers = {}          # (index, sub) -> (value, at_sim)
        self._pending = None        # the (index, sub) a verb waits for
        self._event = threading.Event()
        self._read_lock = threading.Lock()
        self._worker = threading.Lock()
        self._verif = {}            # tc seq -> {"event", "result"}

    # -- the table ---------------------------------------------------------
    def table(self):
        """The table of this controller's ELF, loaded once (the cache
        key is the (path, sha256) pair; a failed load is cached too)."""
        with self.lock:
            key = (self.cfg["qemu"]["elf"], self.sha256)
            if self._key != key:
                self._key = key
                self._table = load(key[0], key[1], log=self.log)
            return self._table

    def summary(self):
        return self.table().summary()

    def snapshot(self):
        t = self.table()
        return t.snapshot(self.cfg["qemu"]["elf"], self.sha256)

    def _record(self, row, how, old, new):
        self.log.emit("ctl", "param", "param.%d" % row["index"],
                      {"index": row["index"], "name": row["name"],
                       "how": how, "old": old, "new": new})

    # -- Current: guest RAM (param.refresh) --------------------------------
    def refresh(self):
        """One QMP memsave of the table's virtual range into the run
        dir (a relative filename, QEMU's cwd), then the value per row.
        Paused is fine (no TC, no guest); off is a 409."""
        if self.power.state == "off" or self.power.qmp is None or \
                not self.power.qmp_alive():
            raise VerbError(409, "STIX is off")
        t = self.table()
        if t.count == 0:
            raise VerbError(409, "no parameter table: %s" %
                            (t.warnings[0] if t.warnings else
                             "the ELF carries none"))
        try:
            self.power.qmp.execute("memsave", {
                "val": t.vaddr, "size": 16 * t.count,
                "filename": RAM_NAME})   # cpu-index omitted: CPU 0
        except QmpError as e:
            raise VerbError(409, "memsave: %s" % e)
        try:
            with open(os.path.join(self.power.run_dir, RAM_NAME), "rb") as f:
                blob = f.read()
        except OSError as e:
            raise VerbError(500, "%s: %s" % (RAM_NAME, e))
        self.apply_ram(blob, self.clock.exact_sim())
        return self.snapshot()

    def apply_ram(self, blob, at_sim):
        """The RAM image (16-byte rows, value at row+4, big-endian):
        diff against the cache, one `param` record per changed row."""
        t = self.table()
        changes = []
        n = min(t.count, len(blob) // 16)
        with self.lock:
            for i in range(n):
                row = t.rows[i]
                new = int.from_bytes(blob[16 * i + 4:16 * i + 8], "big")
                old = row["value"] if row["value"] is not None \
                    else row["default"]
                if new != old:
                    changes.append((row, old, new))
                row["value"], row["value_at"] = new, at_sim
            t.at_sim = at_sim
        for row, old, new in changes:
            self._record(row, "ram", old, new)
        return len(changes)

    # -- the TM(237,20) listener -------------------------------------------
    def on_tm(self, unit, dec):
        if isinstance(dec, dict) and dec.get("type") == 1 and \
                dec.get("subtype") in (2, 7, 8) and \
                isinstance(dec.get("tc"), dict) and \
                dec["tc"].get("apid") == tcbuild.APID_TC:
            self._on_verif(dec)
            return
        if not isinstance(dec, dict) or dec.get("apid") != PARAM_TM_APID \
                or dec.get("type") != 237 or dec.get("subtype") != 20:
            return
        p = dec.get("param")
        if not isinstance(p, dict) or "index" not in p:
            return
        key = (p.get("index"), p.get("sub"))
        at = self.clock.exact_sim()
        with self._tm_lock:
            self._answers[key] = (p.get("value"), at)
            if self._pending == key:
                self._pending = None
                self._event.set()

    def _on_verif(self, dec):
        """TM(1,7) completes, TM(1,2)/TM(1,8) fail a pending param.set."""
        with self._tm_lock:
            slot = self._verif.get(dec["tc"].get("seq"))
            if slot is None or slot["result"] is not None:
                return
            if dec.get("subtype") == 7:
                slot["result"] = {"verified": "completed"}
            else:
                fid = dec.get("fid")
                slot["result"] = {"verified": "failed",
                                  "tm": "TM(1,%d)" % dec["subtype"],
                                  "fid": fid,
                                  "reason": FID_NAMES.get(fid)}
            slot["event"].set()

    # -- read-back (param.read, param.read_all) ----------------------------
    def _one(self, index, sub):
        """One TC(237,19) and the matching TM(237,20) that arrives
        after it (the pending slot is marked before the send, so a
        repeat read cannot match the previous answer)."""
        t = self.table()
        if t.count == 0:
            raise VerbError(409, "no parameter table: %s" %
                            (t.warnings[0] if t.warnings else
                             "the ELF carries none"))
        if not isinstance(index, int) or not 0 <= index < t.count:
            raise VerbError(400, "param index %r is outside the table "
                            "(0..%d)" % (index, t.count - 1))
        why = self.link_ok()
        if why is not None:
            raise VerbError(409, why)
        with self._read_lock:
            seq = self.next_seq()
            with self._tm_lock:
                self._pending = (index, sub)
                self._event.clear()
            sp = tcbuild.tc_packet(tcbuild.APID_TC, seq, 237, 19,
                                   struct.pack(">HB", index, sub),
                                   source=self.cfg["gs"]
                                   ["emulation_source"])
            try:
                ok = self.relay.gs_tc(sp, source="api", path="param.read")
            except BaseException:
                with self._tm_lock:
                    self._pending = None
                raise
            if not ok:
                with self._tm_lock:
                    self._pending = None
                raise VerbError(409, self.link_ok() or
                                "the read request was held or dropped")
            if not self._event.wait(READ_TIMEOUT_S):
                with self._tm_lock:
                    self._pending = None
                raise VerbError(500, "no TM(237,20) for param %d" % index)
            with self._tm_lock:
                ans = self._answers.get((index, sub))
                self._pending = None
        if ans is None:
            raise VerbError(500, "no TM(237,20) for param %d" % index)
        value, at = ans
        self._readback(index, value, at)
        return {"index": index, "sub": sub, "value": value, "at_sim": at}

    def _readback(self, index, value, at):
        with self.lock:
            t = self._table
            if t is None or index >= t.count:
                return
            row = t.rows[index]
            old = row["readback"]
            row["readback"], row["readback_at"] = value, at
        if old != value:
            self._record(row, "readback", old, value)

    def read(self, index, sub=0):
        return self._one(index, sub)

    def read_all(self, only_changed=True):
        t = self.table()
        if t.count == 0:
            raise VerbError(409, "no parameter table: %s" %
                            (t.warnings[0] if t.warnings else
                             "the ELF carries none"))
        if not self._worker.acquire(blocking=False):
            raise VerbError(409, "another param.read_all or "
                            "param.reset_all is running")
        try:
            targets = [r for r in t.rows if not only_changed or
                       (r["value"] is not None and
                        r["value"] != r["default"])]
            read = failed = 0
            for n, row in enumerate(targets, 1):
                try:
                    self._one(row["index"], 0)
                    read += 1
                except VerbError:
                    failed += 1
                if n % READ_PROGRESS == 0:
                    self.log.emit("ctl", "log", "params.read_all",
                                  {"done": n, "total": len(targets),
                                   "read": read, "failed": failed})
            return {"read": read, "failed": failed,
                    "only_changed": only_changed}
        finally:
            self._worker.release()

    # -- set / reset (TC(237,18)) ------------------------------------------
    def set(self, index, value, sub=0):
        t = self.table()
        if t.count == 0:
            raise VerbError(409, "no parameter table: %s" %
                            (t.warnings[0] if t.warnings else
                             "the ELF carries none"))
        if not isinstance(index, int) or not 0 <= index < t.count:
            raise VerbError(400, "param index %r is outside the table "
                            "(0..%d)" % (index, t.count - 1))
        seq = self.next_seq()
        sp = tcbuild.tc_packet(tcbuild.APID_TC, seq, 237, 18,
                               struct.pack(">HBI", index, sub, value),
                               source=self.cfg["gs"]["emulation_source"])
        slot = {"event": threading.Event(), "result": None}
        with self._tm_lock:
            self._verif[seq] = slot
        try:
            ok = self.relay.gs_tc(sp, source="api", path="param.set")
            out = {"index": index, "sub": sub, "value": value,
                   "delivered": ok}
            if not ok:
                out["why"] = self.link_ok() or \
                    "the TC was held or dropped by the link policy"
                return out
            # the FSW's verdict: a set it refuses (e.g. in SAFE, FID
            # 48301) must not read as done
            if slot["event"].wait(VERIFY_TIMEOUT_S):
                out.update(slot["result"])
            else:
                out["verified"] = "timeout"
            return out
        finally:
            with self._tm_lock:
                self._verif.pop(seq, None)

    def reset_all(self, only_changed=True):
        t = self.table()
        if t.count == 0:
            raise VerbError(409, "no parameter table: %s" %
                            (t.warnings[0] if t.warnings else
                             "the ELF carries none"))
        if not self._worker.acquire(blocking=False):
            raise VerbError(409, "another param.read_all or "
                            "param.reset_all is running")
        try:
            targets = [r for r in t.rows if r["value"] is not None and
                       not r.get("maintained") and
                       (not only_changed or r["value"] != r["default"])]
            reset = failed = 0
            first_failure = None
            for n, row in enumerate(targets, 1):
                r = self.set(row["index"], row["default"])
                if r.get("verified") == "completed":
                    reset += 1
                else:
                    failed += 1
                    if first_failure is None:
                        first_failure = dict(r, name=row["name"])
                if n % READ_PROGRESS == 0:
                    self.log.emit("ctl", "log", "params.reset_all",
                                  {"done": n, "total": len(targets),
                                   "reset": reset, "failed": failed})
            out = {"reset": reset, "failed": failed}
            if first_failure is not None:
                out["first_failure"] = first_failure
            return out
        finally:
            self._worker.release()
