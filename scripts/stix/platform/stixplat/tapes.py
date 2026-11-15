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
"""

import json
import os

from .dispatch import VerbError
from .views import NAME_RE

MAX_STEPS = 64
MAX_BYTES = 64 * 1024
MAX_NAME = 64
KINDS = ("rate", "replay", "off", "single")
MAX_CPS = 1000000
MAX_FOR_S = 86400.0
MIN_FOR_S = 0.1

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


def _step(step, i):
    """One step of the model's schema (T1); VerbError(400) naming the
    step index. single steps and the S2 spec shapes parse here (the
    wire interface is fixed); whether the build supports them is the
    model's call at load."""
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
    label = step.get("label")
    if label is not None and (not isinstance(label, str) or
                              not 1 <= len(label) <= MAX_NAME):
        raise VerbError(400, "%s: label %r is not a string of 1..%d"
                        % (where, label, MAX_NAME))
    align = step.get("align")
    if align is not None and not isinstance(align, bool):
        raise VerbError(400, "%s: align %r is not a boolean" % (where, align))
    if kind in ("rate", "off"):
        _int_in(step.get("fpga_det_mask"), 0, 0xFFFFFFFF,
                "fpga_det_mask", where)
        _int_in(step.get("pix_mask", 0xFFF), 0, 0xFFF, "pix_mask", where)
    if kind == "rate":
        _int_in(step.get("cps"), 0, MAX_CPS, "cps", where)
        spec = step.get("spec", "flat")
        if isinstance(spec, str):
            if spec != "flat":
                raise VerbError(400, "%s: spec %r" % (where, spec))
        elif not isinstance(spec, dict):
            raise VerbError(400, "%s: spec %r is not \"flat\" or an object"
                            % (where, spec))
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


def validate(tape):
    """The tape object against the model's schema (T1); VerbError(400)
    with the reason (the step index for a bad step)."""
    if not isinstance(tape, dict):
        raise VerbError(400, "a tape is a JSON object")
    if tape.get("api") != 1:
        raise VerbError(400, "tape api %r is not 1" % (tape.get("api"),))
    name = tape.get("name")
    if not isinstance(name, str) or not 1 <= len(name) <= MAX_NAME:
        raise VerbError(400, "tape name %r: a string of 1..%d"
                        % (name, MAX_NAME))
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
    """GET /api/v1/tapes: the saved tape names and the dir."""
    d = tapes_dir(cfg)
    names = []
    try:
        for fn in os.listdir(d):
            name = fn[:-len(".json")] if fn.endswith(".json") else ""
            if NAME_RE.match(name):
                names.append(name)
    except OSError:
        pass
    return {"tapes": sorted(names), "dir": d}


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
