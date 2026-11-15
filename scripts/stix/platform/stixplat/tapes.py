# SPDX-License-Identifier: GPL-2.0-or-later
"""The detector mix tapes (stimulus plan T3): JSON files in
[timeline] tapes_dir (default <retention.run_root>/tapes — next to the
views dir, outside every run dir), played on the model's guest-time
tick through det.tape.play (det-tape-load + det-tape-play).

A tape file is the model's schema (T1) and is validated here first, so
a bad tape is a 400 before any QMP; the model remains the authority.
A name is 1..64 of A-Za-z0-9._- not starting with a dot (the views'
rule), checked before any file access; the tape's own "name" field is
the display name the model reports (spaces allowed there, 1..64).

Masks are fpga_det/pix indexed; subcol-keyed steps are refused. The
background layer is unmasked by definition — it fills all 384
(fpga_det, pix) pairs outside the active step's masks.

S2 (spectra plan U3): a rate step's spec is "flat" (the quiet template)
or exactly one of line_adc / line_kev / flat_kev / file, the model's
rule table mirrored here as the validator of record. Spectrum CSVs live
in <tapes_dir>/spectra (filled by hand); a saved tape names one by its
bare file name, and det.tape.play reads and parses it (a 400 before any
QMP) and hands the model its absolute path. keV values >= the current K
(det-cal-kev-full) are a warning, never a refusal (decision Q7).
"""

import copy
import json
import math
import os
import re

from .dispatch import VerbError
from .views import NAME_RE

MAX_STEPS = 64
MAX_BYTES = 64 * 1024
MAX_NAME = 64
KINDS = ("rate", "replay", "off", "single")
MAX_CPS = 1000000
MAX_FOR_S = 86400.0
MIN_FOR_S = 0.1

# S2: the spectrum spec (the model's stix_det_spec_parse rule table)
SPEC_KEYS = ("line_adc", "line_kev", "flat_kev", "file")
MAX_SPEC_ROWS = 4096
MAX_SPEC_FILE = 256 * 1024
KEV_FULL_MAX = 4096          # det-cal-kev-full's range cap
KEV_FULL_DEFAULT = 160       # the model's default K
MAX_SPEC_PATH = 4095
_CSV_NUM = re.compile(r"[0-9]+(\.[0-9]+)?([eE][-+]?[0-9]+)?")
_CSV_INT = re.compile(r"[0-9]+")

# utils.c:113-119 (utils_subcolIdFromFpgaId): fpga_det -> subcol, the
# TM's numbering — the same FSW-sourced table elut-upload.py carries
# (one per language; the self-test pins this copy and the UI's against
# it).
SUBCOL_FROM_FPGA_DET = [
    0, 1, 5, 6, 4, 10, 11, 12,
    13, 14, 9, 15, 7, 8, 2, 3,
    30, 31, 25, 26, 21, 27, 19, 20,
    17, 18, 16, 22, 23, 24, 28, 29,
]


def tapes_dir(cfg):
    return cfg["timeline"]["tapes_dir"] or \
        os.path.join(cfg["retention"]["run_root"], "tapes")


def spectra_dir(cfg):
    """S2: the spectrum CSVs, next to the tapes (filled by hand)."""
    return os.path.join(tapes_dir(cfg), "spectra")


def check_spectrum_name(name, where="spectrum"):
    """The platform's file rule (a stricter subset of the model's any
    path): a bare <NAME_RE>.csv in spectra_dir, no '/', checked before
    any file access."""
    if not isinstance(name, str) or "/" in name or \
            not name.endswith(".csv") or not NAME_RE.match(name[:-4]):
        raise VerbError(400, "%s: file %r is not a bare <name>.csv in the "
                        "spectra dir (1..64 of A-Za-z0-9._- not starting "
                        "with '.', then .csv)" % (where, name))


def check_name(name):
    """A tape file name; VerbError(400) for anything else, before any
    file access (the name becomes a file name)."""
    if not isinstance(name, str) or not NAME_RE.match(name):
        raise VerbError(400, "tape name %r: 1..64 of A-Za-z0-9._- and "
                        "not starting with '.'" % (name,))


def _int_in(v, lo, hi, what, where):
    if isinstance(v, bool) or not isinstance(v, int) or not lo <= v <= hi:
        raise VerbError(400, "%s: %s %r is not an integer %d..%d"
                        % (where, what, v, lo, hi))


def _num_in(v, lo, hi, what, where):
    if isinstance(v, bool) or not isinstance(v, (int, float)) or \
            not lo <= v <= hi:
        raise VerbError(400, "%s: %s %r is not a number %s..%s"
                        % (where, what, v, lo, hi))


def _is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def _spec(step, kind, where):
    """S2: the step's spec and align against the model's rule table;
    VerbError(400) naming the step."""
    spec = step.get("spec", "flat")
    align = step.get("align")
    if align is not None and not isinstance(align, bool):
        raise VerbError(400, "%s: align %r is not a boolean" % (where, align))
    if spec == "flat":
        if align is True:
            raise VerbError(400, "%s: align applies to keV specs (line_kev, "
                            "flat_kev, a kev,weight file)" % where)
        return
    if not isinstance(spec, dict) or len(spec) != 1:
        raise VerbError(400, "%s: spec %r must be \"flat\" or an object "
                        "with exactly one key of %s"
                        % (where, spec, ", ".join(SPEC_KEYS)))
    if kind != "rate":
        raise VerbError(400, "%s: a non-flat spec is only valid on kind "
                        "rate" % where)
    (key, v), = spec.items()
    if key == "line_adc":
        if isinstance(v, bool) or not isinstance(v, int) or \
                not 0 <= v <= 4095:
            raise VerbError(400, "%s: line_adc %r is not an integer in "
                            "0..4095" % (where, v))
    elif key == "line_kev":
        if not _is_num(v) or not math.isfinite(v) or v < 0:
            raise VerbError(400, "%s: line_kev %r is not a finite number "
                            ">= 0 (keV)" % (where, v))
    elif key == "flat_kev":
        if not isinstance(v, list) or len(v) != 2 or \
                not all(_is_num(k) and math.isfinite(k) for k in v) or \
                not 0 <= v[0] < v[1]:
            raise VerbError(400, "%s: flat_kev %r is not [k1, k2] with "
                            "0 <= k1 < k2 (keV)" % (where, v))
    elif key == "file":
        if not isinstance(v, str) or not 1 <= len(v.encode()) <= \
                MAX_SPEC_PATH or "," in v:
            raise VerbError(400, "%s: file %r is not a path of 1..%d bytes "
                            "without a comma" % (where, v, MAX_SPEC_PATH))
        check_spectrum_name(v, where)
    else:
        raise VerbError(400, "%s: spec key %r is not one of %s"
                        % (where, key, ", ".join(SPEC_KEYS)))
    if align is True and key == "line_adc":
        raise VerbError(400, "%s: align applies to keV specs (line_kev, "
                        "flat_kev, a kev,weight file)" % where)


def _step(step, i):
    """One step of the model's schema (T1); VerbError(400) naming the
    step index. single steps parse here (the wire interface is fixed);
    whether the build supports them is the model's call at load. S2:
    the spec rule table is the model's, mirrored (an align: true on an
    adc,weight file is caught at resolve, once the CSV is read)."""
    where = "step %d" % i
    if not isinstance(step, dict):
        raise VerbError(400, "%s is not an object" % where)
    if "subcol_mask" in step:
        raise VerbError(400, "%s: \"subcol_mask\" is not a tape field; "
                        "use fpga_det_mask (dss_det = fpga_det)" % where)
    kind = step.get("kind")
    if kind not in KINDS:
        raise VerbError(400, "%s: unknown kind %r (one of %s)"
                        % (where, kind, "|".join(KINDS)))
    _num_in(step.get("for_s"), MIN_FOR_S, MAX_FOR_S, "for_s", where)
    if step.get("label") is not None:
        _text(step["label"], "%s: label" % where)
    _spec(step, kind, where)
    if kind in ("rate", "off"):
        _int_in(step.get("fpga_det_mask"), 0, 0xFFFFFFFF,
                "fpga_det_mask", where)
        _int_in(step.get("pix_mask", 0xFFF), 0, 0xFFF, "pix_mask", where)
    if kind == "rate":
        _int_in(step.get("cps"), 0, MAX_CPS, "cps", where)
    elif kind == "replay":
        if not isinstance(step.get("file"), str) or not step["file"]:
            raise VerbError(400, "%s: replay needs its file" % where)
        _int_in(step.get("offset_ms", 0), 0, 1 << 31, "offset_ms", where)
    elif kind == "single":                  # S3's fixed shape
        _int_in(step.get("fpga_det"), 0, 31, "fpga_det", where)
        _int_in(step.get("pix"), 0, 11, "pix", where)
        _int_in(step.get("n"), 1, 1000000, "n", where)
        _int_in(step.get("wait_us", 20), 1, 10000, "wait_us", where)
        if step.get("adc") is None and step.get("ebin") is None:
            raise VerbError(400, "%s: single needs adc or ebin" % where)
        if step.get("adc") is not None:
            _int_in(step.get("adc"), 0, 4095, "adc", where)
        else:
            _int_in(step.get("ebin"), 0, 31, "ebin", where)


def _background(bg):
    """The background layer: a rate layer, no for_s, unmasked."""
    if not isinstance(bg, dict):
        raise VerbError(400, "background is not an object")
    if "for_s" in bg or "fpga_det_mask" in bg or "pix_mask" in bg:
        raise VerbError(400, "background: no for_s and no masks (it is "
                        "unmasked and fills the whole tape)")
    if bg.get("kind", "rate") != "rate":
        raise VerbError(400, "background: kind %r is not a rate layer"
                        % (bg.get("kind"),))
    _int_in(bg.get("cps", 0), 0, MAX_CPS, "cps", "background")


def _text(v, what):
    """The model's name/label rule (stix_det_tape_line_text): 1..MAX_NAME
    bytes, no comma (the status line separator), no control char."""
    if not isinstance(v, str) or not 1 <= len(v.encode()) <= MAX_NAME:
        raise VerbError(400, "%s %r: a string of 1..%d bytes"
                        % (what, v, MAX_NAME))
    if any(c == "," or ord(c) < 0x20 or ord(c) == 0x7F for c in v):
        raise VerbError(400, "%s %r holds a comma or a control character"
                        % (what, v))


def validate(tape):
    """The tape object against the model's schema (T1); VerbError(400)
    with the reason (the step index for a bad step)."""
    if not isinstance(tape, dict):
        raise VerbError(400, "a tape is a JSON object")
    if tape.get("api") != 1:
        raise VerbError(400, "tape api %r is not 1" % (tape.get("api"),))
    _text(tape.get("name"), "tape name")
    for k in ("align", "adc_mode", "loop"):
        if k in tape and not isinstance(tape[k], bool):
            raise VerbError(400, "tape %s %r is not a boolean" % (k, tape[k]))
    if "background" in tape:
        _background(tape["background"])
    steps = tape.get("steps", [])
    if not isinstance(steps, list):
        raise VerbError(400, "tape steps is a list")
    if len(steps) > MAX_STEPS:
        raise VerbError(400, "tape has %d steps, at most %d"
                        % (len(steps), MAX_STEPS))
    for i, step in enumerate(steps):
        _step(step, i)
    if len(json.dumps(tape).encode()) > MAX_BYTES:
        raise VerbError(400, "tape is over %d KiB" % (MAX_BYTES // 1024))
    return tape


def listing(cfg):
    """GET /api/v1/tapes: the saved tape names and the dir; S2 adds
    "spectra", the .csv files of the spectra dir (additive)."""
    d = tapes_dir(cfg)
    names = []
    try:
        for fn in os.listdir(d):
            name = fn[:-len(".json")] if fn.endswith(".json") else ""
            if NAME_RE.match(name):
                names.append(name)
    except OSError:
        pass
    spectra = []
    try:
        for fn in os.listdir(spectra_dir(cfg)):
            if fn.endswith(".csv") and NAME_RE.match(fn[:-4]):
                spectra.append(fn)
    except OSError:
        pass
    return {"tapes": sorted(names), "dir": d, "spectra": sorted(spectra)}


# -- S2: spectrum CSVs, keV warnings, resolve at play --------------------------

def parse_spectrum_csv(text, kev_full, where):
    """A spectrum CSV (D4), the model's stix_det_spec_file_load grammar
    exactly: <= 256 KiB; the first line exactly "adc,weight" or
    "kev,weight"; then 1..4096 lines "value,weight", no spaces, an
    optional \\r before \\n, the final newline optional, no blank lines;
    numbers [0-9]+(.[0-9]+)?([eE][-+]?[0-9]+)? and finite (no sign, no
    inf/nan — although Python's float() takes them); adc values
    integers 0..4095; weights >= 0 with a sum > 0; repeated values are
    summed. kev_full is not a bound (decision Q7: K is read at the
    model's row build). Returns (unit, [(value, weight)]) sorted by
    value; VerbError(400) naming @where and the line."""
    del kev_full                         # Q7: no K bound at load
    if len(text.encode("utf-8", "surrogateescape")) > MAX_SPEC_FILE:
        raise VerbError(400, "%s: the file is past the %d KiB cap"
                        % (where, MAX_SPEC_FILE // 1024))
    segs = text.split("\n")
    if len(segs) > 1 and segs[-1] == "":
        segs.pop()                       # the final newline
        terminated = len(segs)
    else:
        terminated = len(segs) - 1       # the last line has no newline
    unit = None
    pts = {}
    rows = 0
    for n, line in enumerate(segs):
        if n < terminated and line.endswith("\r"):
            line = line[:-1]
        lineno = n + 1
        if n == 0:
            if line not in ("adc,weight", "kev,weight"):
                raise VerbError(400, "%s line 1: the header must be "
                                "exactly \"adc,weight\" or \"kev,weight\""
                                % where)
            unit = line[:3]
            continue
        if rows == MAX_SPEC_ROWS:
            raise VerbError(400, "%s line %d: more than %d data rows"
                            % (where, lineno, MAX_SPEC_ROWS))
        val, sep, wt = line.partition(",")
        num = _CSV_NUM if unit == "kev" else _CSV_INT
        ok = bool(sep) and num.fullmatch(val) and _CSV_NUM.fullmatch(wt)
        if ok:
            v, w = float(val), float(wt)
            ok = math.isfinite(v) and math.isfinite(w) and \
                (unit == "kev" or v <= 4095)
        if not ok:
            raise VerbError(400, "%s line %d: not \"%s,weight\" (%s, weight "
                            "a number >= 0; no spaces, no blank lines)"
                            % (where, lineno, unit, "keV a number"
                               if unit == "kev" else
                               "adc an integer 0..4095"))
        if unit == "adc":
            v = int(val)
        pts[v] = pts.get(v, 0.0) + w
        rows += 1
    if not rows:
        raise VerbError(400, "%s: no data rows (1..%d)"
                        % (where, MAX_SPEC_ROWS))
    total = sum(pts.values())
    if not total > 0:
        raise VerbError(400, "%s: the weights sum to 0" % where)
    if not math.isfinite(total):
        raise VerbError(400, "%s: the weights sum is not finite" % where)
    return unit, sorted(pts.items())


def kev_warnings(tape, kev_full):
    """Q7: the line_kev / flat_kev values at or past the current K (the
    mass there is dropped as an ADC overflow) — advice, never a
    refusal. Files are checked at resolve, once read."""
    out = []
    for i, step in enumerate(tape.get("steps") or []):
        spec = step.get("spec") if isinstance(step, dict) else None
        if not isinstance(spec, dict):
            continue
        if "line_kev" in spec and _is_num(spec["line_kev"]) and \
                spec["line_kev"] >= kev_full:
            out.append("step %d: line_kev %g >= K %d keV: the line is "
                       "dropped (ADC overflow)" % (i, spec["line_kev"],
                                                  kev_full))
        fk = spec.get("flat_kev")
        if isinstance(fk, list) and len(fk) == 2 and _is_num(fk[1]) and \
                fk[1] > kev_full:
            out.append("step %d: flat_kev %g..%g reaches past K %d keV: "
                       "the part >= K is dropped (ADC overflow)"
                       % (i, fk[0], fk[1], kev_full))
    return out


def resolve(cfg, tape, kev_full, warnings=None):
    """det.tape.play: a copy of the (validated) tape whose spectrum
    files are read and parsed here (a bad or missing file is a 400
    before any QMP) and named by their absolute path; an explicit
    align: true on an adc,weight file is refused now that the unit is
    known; keV >= K points add to @warnings. The saved file is never
    rewritten."""
    out = copy.deepcopy(tape)
    d = spectra_dir(cfg)
    for i, step in enumerate(out.get("steps") or []):
        spec = step.get("spec")
        if not isinstance(spec, dict) or "file" not in spec:
            continue
        where = "step %d: spectrum %s" % (i, spec["file"])
        check_spectrum_name(spec["file"], "step %d" % i)
        path = os.path.abspath(os.path.join(d, spec["file"]))
        if "," in path:
            raise VerbError(400, "%s: the path %s holds a comma (the "
                            "model refuses it)" % (where, path))
        try:
            if os.stat(path).st_size > MAX_SPEC_FILE:
                raise VerbError(400, "%s: the file is past the %d KiB cap"
                                % (where, MAX_SPEC_FILE // 1024))
            with open(path, "rb") as f:
                raw = f.read(MAX_SPEC_FILE + 1)
        except OSError as e:
            raise VerbError(400, "%s: %s" % (where, e))
        try:
            text = raw.decode("ascii")
        except UnicodeDecodeError:
            raise VerbError(400, "%s: not ASCII text" % where)
        unit, pts = parse_spectrum_csv(text, kev_full, where)
        if step.get("align") is True and unit == "adc":
            raise VerbError(400, "step %d: align applies to keV specs "
                            "(line_kev, flat_kev, a kev,weight file): %s "
                            "is adc,weight" % (i, spec["file"]))
        if unit == "kev" and warnings is not None:
            over = [v for v, _ in pts if v >= kev_full]
            if over:
                warnings.append("step %d: %s has %d keV value(s) >= K %d "
                                "keV: dropped (ADC overflow)"
                                % (i, spec["file"], len(over), kev_full))
        step["spec"] = {"file": path}
    return out


def read(cfg, name):
    """GET /api/v1/tapes/<name>: the tape, validated."""
    check_name(name)                # before any file access
    path = os.path.join(tapes_dir(cfg), name + ".json")
    try:
        with open(path, encoding="utf-8") as f:
            tape = json.load(f)
    except FileNotFoundError:
        raise VerbError(404, "no tape %r in %s" % (name, tapes_dir(cfg)))
    except (OSError, ValueError) as e:
        raise VerbError(400, "tape %s: %s" % (name, e))
    return validate(tape)


def save(cfg, name, tape):
    """det.tape.save {name, tape}: the file <name>.json (an existing one
    is replaced, tmp + os.replace); a tape without its own name takes
    the file name."""
    check_name(name)
    if not isinstance(tape, dict):
        raise VerbError(400, "a tape is a JSON object")
    if "name" not in tape:
        tape = dict(tape)
        tape["name"] = name
    validate(tape)
    d = tapes_dir(cfg)
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, name + ".json")
    tmp = path + ".tmp"
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(tape, f, indent=1, sort_keys=True)
            f.write("\n")
        os.replace(tmp, path)       # a reader sees old or new, not half
    except OSError as e:
        raise VerbError(500, "tape %s: %s" % (name, e))
    return tape


def delete(cfg, name):
    """det.tape.delete {name}; 404 unknown."""
    check_name(name)
    path = os.path.join(tapes_dir(cfg), name + ".json")
    if not os.path.isfile(path):
        raise VerbError(404, "no tape %r in %s" % (name, tapes_dir(cfg)))
    try:
        os.unlink(path)
    except OSError as e:
        raise VerbError(500, "tape %s: %s" % (name, e))
