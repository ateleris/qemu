# SPDX-License-Identifier: GPL-2.0-or-later
"""The controller config (platform plan D-PLh): TOML, hand-edited.

Every key has a default; an unknown key or a wrong type is an error
(exit 2), so a typo never silently falls back to a default.
"""

import copy
import os
import tomllib


def _default_elf():
    """The gate ELF, the same default scripts/stix/lib/paths.sh derives
    (a /tmp path does not survive a WSL restart)."""
    elf = os.environ.get("STIX_DEFAULT_ELF")
    if elf:
        return os.path.expanduser(elf)
    elf_dir = os.environ.get("STIX_ELF_DIR")
    if elf_dir:
        return os.path.join(os.path.expanduser(elf_dir),
                            "ASW_em_baseline.elf")
    return os.path.expanduser("~/dev/stix-elf/ASW_em_baseline.elf")


DEFAULTS = {
    "mode": "interactive",          # "interactive" | "run" (run: PL-5)
    "run_dir": {"path": "/tmp/stix-plat/interactive", "fresh": False},
    "qemu": {
        "binary": "build/qemu-system-sparc",
        "elf": _default_elf(),
        "timecode_source": "obt",    # D-PLb: "obt" | "scet"
        "obt_epoch": 0x10000000 << 16,
        "obt_continuity": "aligned",  # "aligned" | "exact"
        "on_guest_reset": "warm",    # "warm" | "cold" | "halt"
        "icount_shift": 0,           # 0 = no icount (today's behaviour)
        "pace": "wall",              # "wall" | "fast" (-icount sleep=off)
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
    # PL-5: scenarios (scenario.run reads only from scenario_dir; "" =
    # scripts/stix/platform/scenarios). mode = "run" runs `scenarios`
    # (each listed variant, or those matching a `variants` glob) each
    # in its own controller, QEMU and run dir under retention.run_root
    "run": {"scenario_dir": "", "scenarios": [], "variants": [],
            "serve_api": False},
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
    # A1: the truth sampler; sample_s -1 = by mode (1 s interactive,
    # 0 in run mode), 0 = off, else the period in seconds (0.5..60)
    "timeline": {"sample_s": -1.0,
                 "sample_paths": [
                     "instrument.rotb.*", "instrument.detector.*cps",
                     "instrument.detector.flare",
                     "instrument.detector.tape.*",
                     "instrument.detector.align.*",
                     "instrument.detector.adc.*",
                     "idpu.watchdog.count",
                     "idpu.spw.counters.*", "instrument.memory.*",
                     "platform.keepalive.*", "platform.timesync.verdict",
                     "platform.power", "idpu.spw.link_down"],
                 # A2: "" would disable; views_dir "" = <run_root>/views,
                 # tapes_dir "" = <run_root>/tapes (the stimulus plan)
                 "idb_json": "~/dev/ATS-GroundStation-stix/definitions/"
                             "telemetry/stix_idb_tlm.json",
                 "idb_sqlite": "", "views_dir": "", "tapes_dir": ""},
    "log": {"max_log_mb": 50, "max_log_files": 5, "ring": 20000},
    # run bundles (2026-11-15 run-bundles plan): the manifest +
    # decisions writer, and the HK fields that yield hk_step decisions
    "bundle": {"enabled": True, "hk_codes": ["NIXD0501", "NIXD0023"]},
    # S3: comparing two run bundles (2026-11-16 compare plan); the
    # series list is pass 2's (C3)
    "compare": {"tol_event_s": 1.0, "tol_number_rel": 0.05,
                "tol_number_abs": 1.0,
                "series": ["hk:2:NIXD0501",
                           "truth:instrument.rotb.count",
                           "truth:instrument.detector.tape.dropped"]},
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
    ("qemu", "pace"): ("wall", "fast"),
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
    if cfg["qemu"]["obt_epoch"] >> 47:
        raise ConfigError("qemu.obt_epoch sets coarse bit 31, the CUC's "
                          "\"unsynchronised\" flag")
    if not 0 <= cfg["qemu"]["icount_shift"] <= 15:
        raise ConfigError("qemu.icount_shift is 0..15 (0 = no icount)")
    if cfg["qemu"]["pace"] == "fast" and cfg["qemu"]["icount_shift"] == 0:
        raise ConfigError("qemu.pace = \"fast\" needs qemu.icount_shift "
                          "> 0 (sleep=off is an icount option)")
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
    for k in ("scenarios", "variants"):
        if not all(isinstance(x, str) for x in cfg["run"][k]):
            raise ConfigError("run.%s is a list of strings" % k)
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
    ss = cfg["timeline"]["sample_s"]
    if ss != -1.0 and ss != 0.0 and not 0.5 <= ss <= 60:
        raise ConfigError("timeline.sample_s is -1 (by mode), 0 (off) "
                          "or 0.5..60")
    if not all(isinstance(x, str) for x in cfg["timeline"]["sample_paths"]):
        raise ConfigError("timeline.sample_paths is a list of globs")
    if not isinstance(cfg["bundle"]["enabled"], bool):
        raise ConfigError("bundle.enabled is true or false")
    if not all(isinstance(x, str) for x in cfg["bundle"]["hk_codes"]):
        raise ConfigError("bundle.hk_codes is a list of strings")
    c = cfg["compare"]
    if not 0 < c["tol_event_s"] <= 3600:
        raise ConfigError("compare.tol_event_s is 0 < x <= 3600")
    if not 0 <= c["tol_number_rel"] < 1:
        raise ConfigError("compare.tol_number_rel is 0 <= x < 1")
    if c["tol_number_abs"] < 0:
        raise ConfigError("compare.tol_number_abs is >= 0")
    if not all(isinstance(x, str) for x in c["series"]):
        raise ConfigError("compare.series is a list of strings")


def merged(cfg, *overs):
    """@cfg with each override merged in and checked again (a scenario's
    config for its run)."""
    out = copy.deepcopy({k: v for k, v in cfg.items() if k != "_path"})
    for o in overs:
        if o:
            _merge(out, o, "")
    _check(out)
    return out


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
