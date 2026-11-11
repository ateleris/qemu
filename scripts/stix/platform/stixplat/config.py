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
        "gs_tc": 21502,     # the controller takes GS TCs here (D-PLg)
    },
    # D-PLg/D-PLa: the GS side of the relay and the proxy
    "gs": {"tm_dests": ["127.0.0.1:21501"], "bind": "127.0.0.1",
           "tc_during_link_down": "drop",   # "drop" | "hold"
           # "hold": at most this many TCs (the FSW queues 64), SF5
           "hold_max": 32,
           "emulation": "auto",    # tc.gs.send: "auto" (run mode) | on | off
           "emulation_source": 121},
    # D-PLe: a TC(20,128) every 125 ms (user decision), configurable
    "keepalive": {"enabled": True, "period_s": 0.125,
                  "autostart": "first_tm",    # "first_tm" | "boot" | "off"
                  # one summary record per N sends (80 = 10 s at 125 ms)
                  "summary_every": 80},
    # B2: the automatic TC(9,129) once per boot (user decision:
    # "automatic, configurable"); "first_tm" | "boot" | "off"
    "timesync": {"autostart": "first_tm"},
    "faults": {"catalogue": ""},              # "" = the v1 catalogue
    # bind: loopback only, unless container = true (PL-7: inside a
    # container that is published on 127.0.0.1 only, D-PLj)
    "api": {"bind": "127.0.0.1", "container": False},
    "power": {"off_time_s": 2.0, "cycle_boots": True,
              "quit_timeout_s": 5.0, "qmp_timeout_s": 5.0,
              "kill_grace_s": 2.0,
              # D-PLl second line, repeated while on (PL-3 review S7)
              "sparse_check_s": 30.0, "sparse_limit_mb": 1024},
    "clock": {"sample_hz": 20.0, "state_hz": 2.0,
              "snapshot_s": 10.0},  # a full state record (0 = off)
    "log": {"max_log_mb": 50, "max_log_files": 5, "ring": 20000},
    "retention": {"run_root": "/tmp/stix-plat/runs-root", "keep_runs": 10,
                  "keep_failed_days": 7, "max_total_mb": 2048,
                  "min_free_mb": 1024, "keep_flash": "failed"},
    # debug.qmp: dev and gate use only; in mode = "run" also needs
    # allow_in_run (a scenario or gate sets it explicitly)
    "debug": {"raw_qmp": False, "allow_in_run": False},
}

CHOICES = {
    ("mode",): ("interactive", "run"),
    ("qemu", "timecode_source"): ("obt", "scet"),
    ("qemu", "obt_continuity"): ("aligned", "exact"),
    ("qemu", "on_guest_reset"): ("warm", "cold", "halt"),
    ("retention", "keep_flash"): ("failed", "never", "always"),
    ("gs", "tc_during_link_down"): ("drop", "hold"),
    ("gs", "emulation"): ("auto", "on", "off"),
    ("keepalive", "autostart"): ("first_tm", "boot", "off"),
    ("timesync", "autostart"): ("first_tm", "boot", "off"),
}


LOOPBACK = ("127.0.0.1", "::1", "localhost")


def parse_dest(d):
    """"host:port" or "[v6]:port" -> (host, port); ConfigError else."""
    d = str(d)
    if d.startswith("["):
        host, sep, port = d[1:].partition("]:")
        if not sep or not host:
            raise ConfigError("%r is not [ipv6]:port" % d)
    else:
        host, _, port = d.rpartition(":")
        if ":" in host:
            raise ConfigError("%r: an IPv6 host needs brackets, "
                              "[::1]:port" % d)
    if not host or not port.isdigit() or not 0 < int(port) < 65536:
        raise ConfigError("%r is not host:port" % d)
    return host, int(port)


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
    # SF6: the GS side has no authentication either (D-PLj)
    open_ok = cfg["api"]["container"]
    for d in cfg["gs"]["tm_dests"]:
        host, _ = parse_dest(d)
        if host not in LOOPBACK and not open_ok:
            raise ConfigError("gs.tm_dests %r is not loopback; only a "
                              "container ([api] container = true) may "
                              "send TM off the host" % d)
    if cfg["gs"]["bind"] not in LOOPBACK and not open_ok:
        raise ConfigError("gs.bind = %r is not loopback; only a container "
                          "([api] container = true) may take GS TCs off "
                          "the host" % cfg["gs"]["bind"])
    src = cfg["gs"]["emulation_source"]
    if not 0 <= src <= 255 or src == 14:
        raise ConfigError("gs.emulation_source = %r: 0..255 and not 14, "
                          "which is the platform's own source" % src)
    if not 0.125 <= cfg["keepalive"]["period_s"] <= 3600:
        raise ConfigError("keepalive.period_s is 0.125..3600")
    if not 1 <= cfg["gs"]["hold_max"] <= 64:
        raise ConfigError("gs.hold_max is 1..64 (the FSW queues 64 TCs)")
    if cfg["keepalive"]["summary_every"] < 1:
        raise ConfigError("keepalive.summary_every must be >= 1")
    if cfg["clock"]["snapshot_s"] < 0:
        raise ConfigError("clock.snapshot_s < 0")
    if cfg["api"]["bind"] not in LOOPBACK and not cfg["api"]["container"]:
        raise ConfigError("api.bind = %r is not loopback; there is no "
                          "authentication (D-PLj). Only a container "
                          "published on 127.0.0.1 may bind it, with "
                          "[api] container = true" % cfg["api"]["bind"])
    for key, v in (("run_dir.path", cfg["run_dir"]["path"]),
                   ("qemu.elf", cfg["qemu"]["elf"]),
                   ("qemu.binary", cfg["qemu"]["binary"])):
        if "," in v:
            raise ConfigError("%s %r has a comma, which would split "
                              "QEMU's option strings" % (key, v))
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
