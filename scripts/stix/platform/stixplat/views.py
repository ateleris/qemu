# SPDX-License-Identifier: GPL-2.0-or-later
"""Timeline views (timeline plan A5): named JSON files in
[timeline] views_dir (default <retention.run_root>/views — outside
every run dir, never touched by run_dir.fresh), the starter views
copied in only when missing, and the generated "everything" view.

A view is {"name", "window": "live"|"1m"|"10m"|"60m"|"run",
"markers": [marker kinds], "panels": [{"title", "series": [ids]}]}.
A name is 1..64 of A-Za-z0-9._- not starting with a dot, and is checked
before any file access (the name becomes a file name). "everything" is
generated from the series catalogue (one panel per leaf group, every
marker kind on) and cannot be saved over or deleted.
"""

import json
import os
import re
import shutil

from .dispatch import VerbError
from .series import MARKERS

# like the flash library's names: no "", no "/", no "..", no hidden
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$")
EVERYTHING = "everything"
WINDOWS = ("live", "1m", "10m", "60m", "run")
STARTERS_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "views"))


def views_dir(cfg):
    return cfg["timeline"]["views_dir"] or \
        os.path.join(cfg["retention"]["run_root"], "views")


def check_name(name):
    """A view name; VerbError(400) for anything else, before any file
    access."""
    if not isinstance(name, str) or not NAME_RE.match(name):
        raise VerbError(400, "view name %r: 1..64 of A-Za-z0-9._- and "
                        "not starting with '.'" % (name,))


def validate(view):
    """The view object; VerbError(400) with the reason."""
    if not isinstance(view, dict):
        raise VerbError(400, "a view is a JSON object")
    check_name(view.get("name"))
    if view.get("window") not in WINDOWS:
        raise VerbError(400, "view window %r is not one of %s"
                        % (view.get("window"), "|".join(WINDOWS)))
    markers = view.get("markers")
    if not isinstance(markers, list) or \
            not all(isinstance(m, str) for m in markers) or \
            len(set(markers)) != len(markers) or \
            not all(m in MARKERS for m in markers):
        raise VerbError(400, "view markers is a list of %s"
                        % "|".join(MARKERS))
    panels = view.get("panels")
    if not isinstance(panels, list):
        raise VerbError(400, "view panels is a list")
    for p in panels:
        if not isinstance(p, dict) or not isinstance(p.get("title"), str) \
                or not p["title"] or \
                not isinstance(p.get("series"), list) or \
                not all(isinstance(s, str) and s for s in p["series"]):
            raise VerbError(400, "a view panel is {title, series}: %r"
                            % (p,))
    return view


def ensure_starters(cfg):
    """The starter views in the views dir, each only when missing (a
    deleted one comes back at the next start; a changed one is kept)."""
    d = views_dir(cfg)
    if not os.path.isdir(STARTERS_DIR):
        return d
    os.makedirs(d, exist_ok=True)
    for fn in sorted(os.listdir(STARTERS_DIR)):
        if fn.endswith(".json") and not os.path.exists(os.path.join(d, fn)):
            shutil.copyfile(os.path.join(STARTERS_DIR, fn),
                            os.path.join(d, fn))
    return d


def listing(cfg):
    """GET /api/v1/views: the saved view names and the dir."""
    d = views_dir(cfg)
    names = []
    try:
        for fn in os.listdir(d):
            name = fn[:-len(".json")] if fn.endswith(".json") else ""
            if NAME_RE.match(name):
                names.append(name)
    except OSError:
        pass
    return {"views": sorted(names), "dir": d}


def read(cfg, name):
    """GET /api/v1/views/<name>: the view, validated."""
    check_name(name)                # before any file access
    path = os.path.join(views_dir(cfg), name + ".json")
    try:
        with open(path, encoding="utf-8") as f:
            view = json.load(f)
    except FileNotFoundError:
        raise VerbError(404, "no view %r in %s" % (name, views_dir(cfg)))
    except (OSError, ValueError) as e:
        raise VerbError(400, "view %s: %s" % (name, e))
    return validate(view)


def save(cfg, name, view):
    """view.save {name, view}: the file <name>.json (an existing one is
    replaced, tmp + os.replace); "everything" is generated and cannot
    be saved over."""
    check_name(name)
    if name == EVERYTHING:
        raise VerbError(409, '"everything" is generated; save the view '
                        'under another name')
    validate(view)
    view = dict(view)
    view["name"] = name
    d = views_dir(cfg)
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, name + ".json")
    tmp = path + ".tmp"
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(view, f, indent=1, sort_keys=True)
            f.write("\n")
        os.replace(tmp, path)       # a reader sees old or new, not half
    except OSError as e:
        raise VerbError(500, "view %s: %s" % (name, e))
    return view


def delete(cfg, name):
    """view.delete {name}; 404 unknown, 409 "everything"."""
    check_name(name)
    if name == EVERYTHING:
        raise VerbError(409, '"everything" is generated; nothing to '
                        'delete')
    path = os.path.join(views_dir(cfg), name + ".json")
    if not os.path.isfile(path):
        raise VerbError(404, "no view %r in %s" % (name, views_dir(cfg)))
    try:
        os.unlink(path)
    except OSError as e:
        raise VerbError(500, "view %s: %s" % (name, e))


def everything(catalogue):
    """The generated view: one panel per leaf group (a state path's
    parent node, at most two levels; all HK fields together) from the series catalogue,
    every marker kind on, the whole run."""
    groups = {}
    for e in catalogue:
        sid = e["id"]
        if sid.startswith(("truth:", "presented:")):
            # the parent node, at most two levels: platform.power ->
            # platform, instrument.rotb.count -> instrument.rotb
            parts = sid.split(":", 1)[1].split(".")
            group = ".".join(parts[:max(1, min(2, len(parts) - 1))])
        elif sid.startswith("hk:") or sid.startswith("hk."):
            group = "hk"
        else:
            continue
        groups.setdefault(group, []).append(sid)
    return {"name": EVERYTHING, "window": "run",
            "markers": list(MARKERS),
            "panels": [{"title": g, "series": sorted(ids)}
                       for g, ids in sorted(groups.items())]}