# SPDX-License-Identifier: GPL-2.0-or-later
"""The controller config (platform plan D-PLh): TOML, hand-edited.

Every key has a default; an unknown key or a wrong type is an error
(exit 2), so a typo never silently falls back to a default.
"""

import copy
import os
import tomllib

DEFAULTS = {
    "mode": "interactive",          # "interactive" | "run" (run: PL-5)
    "run_dir": {"path": "/tmp/stix-plat/interactive", "fresh": False},
    "qemu": {
        "binary": "build/qemu-system-sparc",
        "elf": "/tmp/stix-baseline-run/ASW_em_baseline.elf",
        "timecode_source": "obt",    # D-PLb: "obt" | "scet"
        "obt_epoch": 0x10000000 << 16,
        "obt_continuity": "aligned",  # "aligned" | "exact"
        "on_guest_reset": "warm",    # "warm" | "cold" | "halt"
        "memory": "64M",
        "extra_args": [],
        "traces": ["stixspw_timecode_emit", "stixspw_rx_deliver",
                   "stixspw_timecode_manual", "stixspw_timecode_once_drop",
                   "stixspw_obt_set"],
    },
    "ports": {
        "api": 8080,
        "qemu_tm": 21511,   # QEMU sends TM here (the controller binds it)
        "qemu_tc": 21512,   # QEMU binds TC here (127.0.0.1)
    },
    "api": {"bind": "127.0.0.1"},
    "power": {"off_time_s": 2.0, "cycle_boots": True,
              "quit_timeout_s": 5.0},
    "clock": {"sample_hz": 20.0, "state_hz": 2.0},
    "log": {"max_log_mb": 50, "max_log_files": 5, "ring": 20000},
    "retention": {"run_root": "/tmp/stix-plat/runs-root", "keep_runs": 10,
                  "keep_failed_days": 7, "max_total_mb": 2048,
                  "min_free_mb": 1024, "keep_flash": "failed"},
    "debug": {"raw_qmp": False},     # debug.qmp: dev and gate use only
}

CHOICES = {
    ("mode",): ("interactive", "run"),
    ("qemu", "timecode_source"): ("obt", "scet"),
    ("qemu", "obt_continuity"): ("aligned", "exact"),
    ("qemu", "on_guest_reset"): ("warm", "cold", "halt"),
    ("retention", "keep_flash"): ("failed", "never", "always"),
}


class ConfigError(Exception):
    pass


def _merge(base, over, where):
    for k, v in over.items():
        if k not in base:
            raise ConfigError("unknown config key %s%s" % (where, k))
        b = base[k]
        if isinstance(b, dict):
            if not isinstance(v, dict):
                raise ConfigError("%s%s must be a table" % (where, k))
            _merge(b, v, "%s%s." % (where, k))
            continue
        ok = (isinstance(v, bool) == isinstance(b, bool) and
              (isinstance(v, type(b)) or
               (isinstance(b, float) and isinstance(v, int))))
        if not ok:
            raise ConfigError("%s%s: expected %s, got %r"
                              % (where, k, type(b).__name__, v))
        base[k] = float(v) if isinstance(b, float) else v


def _check(cfg):
    for path, allowed in CHOICES.items():
        v = cfg
        for p in path:
            v = v[p]
        if v not in allowed:
            raise ConfigError("%s = %r is not one of %s"
                              % (".".join(path), v, "|".join(allowed)))
    for k, p in cfg["ports"].items():
        if not 0 < p < 65536:
            raise ConfigError("ports.%s = %r is out of range" % (k, p))
    if not 0 <= cfg["qemu"]["obt_epoch"] < (1 << 48):
        raise ConfigError("qemu.obt_epoch is wider than the 48-bit CUC")
    if cfg["power"]["off_time_s"] < 0:
        raise ConfigError("power.off_time_s < 0")
    for k in ("sample_hz", "state_hz"):
        if cfg["clock"][k] <= 0:
            raise ConfigError("clock.%s must be > 0" % k)


def load(path=None, text=None, overrides=None):
    """The merged config. @text (TOML) is for the self-test."""
    cfg = copy.deepcopy(DEFAULTS)
    if path is not None:
        with open(path, "rb") as f:
            text = f.read().decode()
    if text is not None:
        try:
            data = tomllib.loads(text)
        except tomllib.TOMLDecodeError as e:
            raise ConfigError("config: %s" % e)
        _merge(cfg, data, "")
    if overrides:
        _merge(cfg, overrides, "")
    _check(cfg)
    if path is not None:
        cfg["_path"] = os.path.abspath(path)
    return cfg
