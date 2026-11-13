#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
r"""ROTB fill report (rotb-fill plan RF-4,
docs/stix-port/2026-11-14-rotb-fill-plan.md).

Reads one run dir (rotb-fill-run.sh's layout):

  run.json         the cell parameters the wrapper recorded; tolerated
                   keys: cell, ticks, cps, icount_shift, icount, latency,
                   seconds, rotb_overflow, elf, capacity (unknown extras
                   are kept verbatim; a missing file is tolerated)
  fill.csv         the wrapper's ROTB samples, header
                   wall_mono,vclock_ns,count,full,rd,wr,capacity
                   (vclock_ns is the guest's virtual clock; rd/wr/count
                   are FIFO block pointers/levels, capacity the ring
                   size in blocks, full the FULL flag)
  trace.log        the QEMU -trace output (one -trace events=...,file=...
                   and -msg timestamp=on); lines look like

                     2026-11-13T10:00:00.000001Z stixdet_chunk chunk: nom_bank 0, 49 blocks, scet 0x00000064:0x0000, int_ticks=0 eacc_events=1800 rotb_count=98 at vclock_ns=100000000000

                   with the leading \S+ stamp the host wall clock, which
                   is never used as a metric (chunk-trace-check.py's
                   rule; WSL's timesyncd steps the wall clock mid-run)
  uart.raw         the raw serial output (no vclock; only counts and the
                   first-occurrence order are derived from it)
  events.log       the -trace events filter / drive log (only counted)
  flash-truth.txt  optional l4-flash-truth.py output; its "file " lines
                   are the RAW_COUNTS archive files

and writes into the dir: summary.json and fill.png (matplotlib only;
skipped with a note when matplotlib is not importable).

Metric definitions (the same text ships inside summary.json "defs"):

  window   NOMINAL window = first stixdet_chunk/stixdet_chunk_drop
           vclock to the later of the last such vclock and the last
           fill.csv sample.
  inflow   blocks per vclock second from stixdet_chunk n_blocks; each
           stixdet_chunk_drop counts as CHUNK_BLOCKS = 49 blocks (the
           drop trace carries no size; a nominal swap always pushes a
           49-block chunk, stix_det.c). stixctrl_rotb_overwrite lines
           are NOT added: under overwrite-oldest the chunk line already
           carries the full n_blocks including the overwritten blocks.
  drain    blocks per fill.csv sample interval: (rd1 - rd0 + capacity)
           % capacity. Under rotb-overflow=overwrite-oldest the rd
           pointer also advances per evicted block (grlib_stixctrl.c),
           so the drain then counts evictions + GETBLOCKs; the deficit
           stays consistent because inflow contains those blocks too.
  deficit  inflow - drain, per fill.csv interval (blocks) and as means
           over the window (blocks/s).
  fill     peak = max fill.csv count / capacity; first_full_s = first
           sample with count >= capacity (or full set).
  overflow stixctrl_rotb_overflow / stixctrl_rotb_overwrite carry no
           vclock: the reported time is the midpoint of the vclock
           bracket between the neighbouring vclock-bearing trace lines
           (the self-test pins an exact bracket); count = line count.
  flash    op kind by cmd: 0 reset, 1 erase, 2 read, 3 program, 4
           fsm-access (grlib_stixflash.c); ops/s over the window with
           each stixflash_op_start placed at its bracket midpoint;
           busy time = sum of delay_us over stixflash_op_start lines.
  files    files_per_s = (number of "file " lines in flash-truth.txt) /
           window seconds; null when the truth file is absent (a flash
           program burst is not a file).

Usage:
  rotb-fill-report.py RUN_DIR [RUN_DIR...]
  rotb-fill-report.py --matrix RUN_DIR...
  rotb-fill-report.py --self-test
Exit codes: 0 ok, 1 a --matrix row was unreadable (rows still printed),
2 an input error (missing/unreadable trace.log, bad run dir). stdlib
only; matplotlib is optional.
"""

import argparse
import bisect
import csv
import json
import os
import re
import shutil
import sys
import tempfile
import time

NS = 1_000_000_000
CHUNK_BLOCKS = 49          # STIX_DET_CHUNK_BLOCKS (stix_det.c:1716)
TRACE_TS = r"^\S+ "        # the host stamp, never used as a metric

CHUNK = re.compile(TRACE_TS + r"stixdet_chunk chunk: nom_bank (\d+), "
                   r"(\d+) blocks, scet 0x([0-9a-f]+):0x([0-9a-f]+), "
                   r"int_ticks=(\d+) eacc_events=(\d+) rotb_count=(\d+) "
                   r"at vclock_ns=(-?\d+)")
DROP = re.compile(TRACE_TS + r"stixdet_chunk_drop chunk dropped \(([^)]+)\)"
                  r": nom_bank (\d+), int_ticks=(\d+) rotb_count=(\d+) "
                  r"at vclock_ns=(-?\d+)")
OVERFLOW = re.compile(TRACE_TS + r"stixctrl_rotb_overflow rotb overflow: "
                      r"block dropped, count=(\d+)")
OVERWRITE = re.compile(TRACE_TS + r"stixctrl_rotb_overwrite rotb overwrite:"
                       r" dropped block rd=(\d+) count=(\d+)")
FLASH_START = re.compile(TRACE_TS + r"stixflash_op_start op start "
                         r"cmd=(\d+) mcm=(\d+) chip=(\d+) "
                         r"page=0x[0-9a-f]+ delay_us=(\d+)")
FLASH_DONE = re.compile(TRACE_TS + r"stixflash_op_done op done cmd=(\d+) ")
FLASH_ABORT = re.compile(TRACE_TS + r"stixflash_op_abort op aborted by "
                         r"RESET cmd=(\d+)")

# grlib_stixflash.c:61-65
FLASH_CMD_NAMES = {0: "reset", 1: "erase", 2: "read", 3: "prog",
                   4: "fsm-access"}

UART_MARKS = ("RotBuf overrun occured",     # RotAccumulator.c:107 (sic)
              "packet sequence error",      # RotAccuCompactor.c:94
              "Partition of type")          # MemFilesystem.c:734

RUN_PARAM_KEYS = ("cell", "ticks", "cps", "icount_shift", "icount",
                  "latency", "seconds", "rotb_overflow", "elf", "capacity")

DEFS = {
    "window": "NOMINAL window = first stixdet_chunk/stixdet_chunk_drop "
              "vclock to the later of the last such vclock and the last "
              "fill.csv sample",
    "inflow": "blocks per vclock second from stixdet_chunk n_blocks; "
              "each stixdet_chunk_drop counts as CHUNK_BLOCKS=49 (no "
              "size in the drop trace); overwrites NOT added (the chunk "
              "line already includes them under overwrite-oldest)",
    "drain": "(rd1-rd0+capacity)%capacity per fill.csv sample interval; "
             "under overwrite-oldest rd also advances per eviction, so "
             "drain = GETBLOCKs + evictions then",
    "deficit": "inflow - drain per fill.csv interval, and means over "
               "the window in blocks/s",
    "fill": "peak = max(fill.csv count)/capacity; first_full_s = first "
            "sample with count >= capacity or full set",
    "overflow": "rotb_overflow/rotb_overwrite traces carry no vclock: "
                "time = midpoint of the vclock bracket between the "
                "neighbouring vclock-bearing lines",
    "flash": "kind by cmd 0 reset/1 erase/2 read/3 prog/4 fsm-access; "
             "ops/s over the window, each op_start at its bracket "
             "midpoint; busy = sum of stixflash_op_start delay_us",
    "files": "files_per_s = count of 'file ' lines in flash-truth.txt / "
             "window seconds; null without the truth file (a flash "
             "program burst is not a file)",
}


class ReportError(Exception):
    """A run dir cannot be reported: loud exit 2 with this reason."""


def scet_ms(coarse, fine):
    """ms from a SCET coarse/fine pair, sync bit stripped
    (Timing_ToMs, the l4-flash-truth.py rule)."""
    return (coarse & 0x7FFFFFFF) * 1000 + fine * 1000 // 0x10000


def read_run_json(run_dir):
    """The run.json params ({} with a note when absent/unreadable)."""
    path = os.path.join(run_dir, "run.json")
    try:
        with open(path) as fh:
            data = json.load(fh)
    except FileNotFoundError:
        return {}, ["run.json absent: params unknown"]
    except (OSError, ValueError) as e:
        return {}, ["run.json unreadable (%s): params unknown" % e]
    if not isinstance(data, dict):
        return {}, ["run.json is not an object: params unknown"]
    # a nested "params" object is merged over the top-level keys
    merged = dict(data)
    inner = data.get("params")
    if isinstance(inner, dict):
        merged.update(inner)
    merged.pop("params", None)
    return merged, []


def parse_fill_csv(path):
    """fill.csv -> rows of dicts sorted by vclock_ns (torn rows dropped)."""
    rows = []
    with open(path, newline="") as fh:
        for row in csv.DictReader(fh):
            # the drive writes "rotb.count" etc.; accept bare names too
            row = {k.split(".", 1)[1] if k and k.startswith("rotb.")
                   else k: v for k, v in row.items()}
            try:
                rows.append({
                    "wall_mono": float(row["wall_mono"]),
                    "vclock_ns": int(row["vclock_ns"]),
                    "count": int(row["count"]),
                    "full": str(row["full"]).strip().lower()
                            in ("1", "true", "yes"),
                    "rd": int(row["rd"]),
                    "wr": int(row["wr"]),
                    "capacity": int(row["capacity"]),
                })
            except (KeyError, TypeError, ValueError):
                continue        # a header line or a torn last sample
    rows.sort(key=lambda r: r["vclock_ns"])
    return rows


def parse_trace(path):
    """trace.log -> event dicts; non-vclock events get vc_lo/vc_hi, the
    vclock bracket between the neighbouring vclock-bearing lines."""
    ev = []
    with open(path, errors="replace") as fh:
        for i, line in enumerate(fh):
            m = CHUNK.match(line)
            if m:
                ev.append({"n": i, "k": "chunk", "vc": int(m.group(8)),
                           "blocks": int(m.group(2)),
                           "rotb_count": int(m.group(7)),
                           "ticks": int(m.group(5)),
                           "eacc": int(m.group(6)),
                           "scet_ms": scet_ms(int(m.group(3), 16),
                                              int(m.group(4), 16))})
                continue
            m = DROP.match(line)
            if m:
                ev.append({"n": i, "k": "drop", "vc": int(m.group(5)),
                           "why": m.group(1),
                           "rotb_count": int(m.group(4)),
                           "ticks": int(m.group(3))})
                continue
            m = OVERFLOW.match(line)
            if m:
                ev.append({"n": i, "k": "overflow", "vc": None,
                           "count": int(m.group(1))})
                continue
            m = OVERWRITE.match(line)
            if m:
                ev.append({"n": i, "k": "overwrite", "vc": None,
                           "rd": int(m.group(1)),
                           "count": int(m.group(2))})
                continue
            m = FLASH_START.match(line)
            if m:
                ev.append({"n": i, "k": "flash_start", "vc": None,
                           "cmd": int(m.group(1)),
                           "delay_us": int(m.group(4))})
                continue
            if FLASH_DONE.match(line):
                ev.append({"n": i, "k": "flash_done", "vc": None})
                continue
            if FLASH_ABORT.match(line):
                ev.append({"n": i, "k": "flash_abort", "vc": None})
    # bracket every non-vclock line: lo = last vclock at or before it,
    # hi = first vclock after it (file order == vclock order: the trace
    # lines are stamped as they are printed)
    marks_n = [e["n"] for e in ev if e["vc"] is not None]
    marks_v = [e["vc"] for e in ev if e["vc"] is not None]
    for e in ev:
        if e["vc"] is not None:
            e["vc_lo"] = e["vc_hi"] = e["vc"]
            continue
        j = bisect.bisect_right(marks_n, e["n"])
        e["vc_lo"] = marks_v[j - 1] if j > 0 else None
        e["vc_hi"] = marks_v[j] if j < len(marks_v) else None
    return ev


def parse_uart(path):
    """uart.raw -> {counts: {mark: {count, first_line}},
    first_occurrence_order: [marks by first line]}."""
    with open(path, "rb") as fh:
        text = fh.read().decode("utf-8", "replace")
    lines = text.splitlines()
    counts = {}
    for mark in UART_MARKS:
        first = next((i for i, ln in enumerate(lines) if mark in ln), None)
        counts[mark] = {
            "count": sum(ln.count(mark) for ln in lines),
            "first_line": first,
        }
    order = sorted((m for m in UART_MARKS
                    if counts[m]["first_line"] is not None),
                   key=lambda m: counts[m]["first_line"])
    return {"counts": counts, "first_occurrence_order": order}


def parse_events_log(path):
    """events.log -> {"lines": n, "json_lines": n, "nominal_lines": n}."""
    out = {"lines": 0, "json_lines": 0, "nominal_lines": 0}
    with open(path, errors="replace") as fh:
        for line in fh:
            out["lines"] += 1
            low = line.lower()
            if "nominal" in low:
                out["nominal_lines"] += 1
            try:
                json.loads(line)
            except ValueError:
                continue
            out["json_lines"] += 1
    return out


def count_flash_truth_files(path):
    """The "file " lines in a flash-truth.txt (None when absent)."""
    n = 0
    with open(path, errors="replace") as fh:
        for line in fh:
            if line.startswith("file "):
                n += 1
    return n


def _bracket_secs(e):
    """A non-vclock event's time: the bracket in seconds and its midpoint."""
    lo = None if e["vc_lo"] is None else e["vc_lo"] / NS
    hi = None if e["vc_hi"] is None else e["vc_hi"] / NS
    mid = None if lo is None or hi is None else (lo + hi) / 2
    return {"vclock_s": mid, "bracket_s": [lo, hi]}


def compute(td, fill, uart, events, n_files):
    """All metrics: the summary dict minus notes/defs/params."""
    # ---- the NOMINAL window: first chunk/drop vclock to the end
    vc_all = [e["vc"] for e in td if e["k"] in ("chunk", "drop")]
    start = min(vc_all) if vc_all else None
    last_trace = max(vc_all) if vc_all else None
    last_fill = fill[-1]["vclock_ns"] if fill else None
    ends = [x for x in (last_trace, last_fill) if x is not None]
    end = max(ends) if ends else None
    window = (start, end) if start is not None and end is not None \
        and end > start else None
    window_s = None if window is None else (window[1] - window[0]) / NS

    # ---- inflow: chunk n_blocks + drops (CHUNK_BLOCKS each) per vclock
    # second; overwrites are not added (see DEFS["inflow"])
    chunk_count = drop_count = 0
    chunk_blocks = drop_blocks = 0
    drop_by_why = {}
    peak_rotb = 0
    bins = {}
    cd = []                       # (vclock, blocks) in time order
    for e in td:
        if e["k"] == "chunk":
            blocks = e["blocks"]
            chunk_count += 1
            chunk_blocks += blocks
            peak_rotb = max(peak_rotb, e["rotb_count"])
        elif e["k"] == "drop":
            blocks = CHUNK_BLOCKS          # the drop trace has no size
            drop_count += 1
            drop_blocks += blocks
            drop_by_why[e["why"]] = drop_by_why.get(e["why"], 0) + 1
            peak_rotb = max(peak_rotb, e["rotb_count"])
        else:
            continue
        bins[e["vc"] // NS] = bins.get(e["vc"] // NS, 0) + blocks
        cd.append((e["vc"], blocks))
    cd.sort()
    inflow_total = chunk_blocks + drop_blocks
    inflow_ts = None
    if window is not None:
        inflow_ts = [{"s": s, "blocks": bins.get(s, 0)}
                     for s in range(window[0] // NS, -(-window[1] // NS))]
    inflow_mean = None if window_s is None else inflow_total / window_s

    # ---- drain and deficit per fill.csv sample interval
    drain_series = []
    deficit_series = []
    drain_total = 0               # blocks, all intervals
    drain_window_blocks = 0.0     # overlap-fraction weighted, for the mean
    j = 0                         # pointer into cd
    for a, b in zip(fill, fill[1:]):
        dt = b["vclock_ns"] - a["vclock_ns"]
        cap = b["capacity"] if b["capacity"] > 0 else a["capacity"]
        if dt <= 0 or not cap:
            continue
        d = (b["rd"] - a["rd"] + cap) % cap
        t0, t1 = a["vclock_ns"], b["vclock_ns"]
        drain_total += d
        drain_series.append({"s": t0 / NS, "dt_s": dt / NS, "blocks": d,
                             "rate_blocks_per_s": d / (dt / NS)})
        infl = 0
        while j < len(cd) and cd[j][0] < t1:
            if cd[j][0] >= t0:
                infl += cd[j][1]
            j += 1
        deficit_series.append({"s": t0 / NS, "dt_s": dt / NS,
                               "inflow_blocks": infl, "drained_blocks": d,
                               "deficit_blocks": infl - d})
        if window is not None:
            ov = min(t1, window[1]) - max(t0, window[0])
            if ov > 0:
                drain_window_blocks += d * ov / dt
    drain_mean = None if window_s is None \
        else drain_window_blocks / window_s
    deficit_mean = None if (inflow_mean is None or drain_mean is None) \
        else inflow_mean - drain_mean

    # ---- fill level and overflow events
    cap = next((r["capacity"] for r in fill if r["capacity"] > 0), None)
    peak = None
    if fill and cap:
        pr = max(fill, key=lambda r: r["count"])
        peak = {"frac": pr["count"] / cap, "count": pr["count"],
                "vclock_s": pr["vclock_ns"] / NS}
    first_full = None
    full_samples = 0
    for r in fill:
        if r["full"] or (cap is not None and r["count"] >= cap):
            full_samples += 1
            if first_full is None:
                first_full = r["vclock_ns"] / NS
    ovf = [e for e in td if e["k"] == "overflow"]
    ovw = [e for e in td if e["k"] == "overwrite"]

    # ---- flash ops (each op_start at its bracket midpoint)
    kinds = {}
    kinds_in_window = {}
    done_count = abort_count = 0
    busy_total_us = 0
    for e in td:
        if e["k"] == "flash_start":
            kind = FLASH_CMD_NAMES.get(e["cmd"], "cmd%d" % e["cmd"])
            k = kinds.setdefault(kind, {"ops": 0, "busy_us": 0})
            k["ops"] += 1
            k["busy_us"] += e["delay_us"]
            busy_total_us += e["delay_us"]
            if window is not None:
                mid = _bracket_secs(e)["vclock_s"]
                if mid is not None and window[0] <= mid * NS < window[1]:
                    kinds_in_window[kind] = kinds_in_window.get(kind, 0) + 1
        elif e["k"] == "flash_done":
            done_count += 1
        elif e["k"] == "flash_abort":
            abort_count += 1
    flash_ops_per_s = None
    if window_s:
        flash_ops_per_s = {k: kinds_in_window.get(k, 0) / window_s
                           for k in kinds}

    # ---- files
    files_per_s = None
    if n_files is not None and window_s:
        files_per_s = n_files / window_s

    return {
        "window": {"nominal_start_s": None if start is None
                   else start / NS,
                   "end_s": None if end is None else end / NS,
                   "duration_s": window_s},
        "inflow": {"mean_blocks_per_s": inflow_mean,
                   "total_blocks": inflow_total,
                   "chunk_count": chunk_count,
                   "chunk_blocks": chunk_blocks,
                   "drop_count": drop_count,
                   "drop_blocks": drop_blocks,
                   "series": inflow_ts},
        "drain": {"mean_blocks_per_s": drain_mean,
                  "total_blocks": drain_total,
                  "series": drain_series},
        "deficit": {"mean_blocks_per_s": deficit_mean,
                    "series": deficit_series},
        "fill": {"peak": peak, "first_full_s": first_full,
                 "full_samples": full_samples,
                 "capacity_blocks": cap,
                 "peak_rotb_count": peak_rotb},
        "overflow": {"chunk_drop_count": drop_count,
                     "chunk_drop_by_why": drop_by_why,
                     "rotb_overflow_count": len(ovf),
                     "first_rotb_overflow": _bracket_secs(ovf[0])
                     if ovf else None,
                     "overwrite_count": len(ovw),
                     "first_overwrite": _bracket_secs(ovw[0])
                     if ovw else None},
        "uart": uart,
        "events_log": events,
        "flash": {"ops_by_kind": {k: v["ops"] for k, v in kinds.items()},
                  "busy_us_by_kind": {k: v["busy_us"]
                                      for k, v in kinds.items()},
                  "busy_us_total": busy_total_us,
                  "ops_done": done_count,
                  "ops_aborted": abort_count,
                  "ops_per_s_in_window_by_kind": flash_ops_per_s},
        "files": {"n_files": n_files, "files_per_s": files_per_s},
    }


def make_plot(path, sm, fill, td):
    """fill.png via matplotlib; -> None or a skip note string."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as e:                     # ImportError and friends
        return "skipped: matplotlib not importable (%s)" % e
    cap = sm["fill"]["capacity_blocks"]
    w = sm["window"]

    fig, (ax1, ax2) = plt.subplots(
        2, 1, figsize=(10, 7), sharex=True,
        gridspec_kw={"height_ratios": [1, 1]})
    fig.suptitle("ROTB fill: %s" % os.path.basename(
        os.path.dirname(os.path.abspath(path))) or path)

    # top: the fill level
    if fill and cap:
        xs = [r["vclock_ns"] / NS for r in fill]
        ys = [min(r["count"] / cap, 1.5) for r in fill]
        ax1.plot(xs, ys, ".-", color="#1f77b4", lw=1, ms=3,
                 label="count/capacity (fill.csv)")
        ax1.axhline(1.0, color="k", lw=0.8, ls=":")
    drops = [e for e in td if e["k"] == "drop"]
    if drops and cap:
        ax1.plot([e["vc"] / NS for e in drops],
                 [min(e["rotb_count"] / cap, 1.5) for e in drops],
                 "x", color="#d62728", label="chunk_drop")
    for ov, color, lab in (
            (sm["overflow"]["first_rotb_overflow"], "#ff7f0e",
             "first rotb_overflow"),
            (sm["overflow"]["first_overwrite"], "#9467bd",
             "first rotb_overwrite")):
        if ov and ov["vclock_s"] is not None:
            ax1.axvline(ov["vclock_s"], color=color, lw=1, ls="--",
                        label=lab)
        elif ov and ov["bracket_s"][0] is not None:
            ax1.axvline(ov["bracket_s"][0], color=color, lw=1, ls="--",
                        label=lab + " (>=)")
    if sm["fill"]["first_full_s"] is not None:
        ax1.axvline(sm["fill"]["first_full_s"], color="g", lw=1,
                    label="first full")
    ax1.set_ylabel("fill (count/capacity)")
    ax1.set_ylim(bottom=0)
    ax1.legend(fontsize=7, loc="upper left")

    # bottom: rates
    inf = sm["inflow"]["series"]
    if inf:
        ax2.step([x["s"] for x in inf], [x["blocks"] for x in inf],
                 where="post", color="#1f77b4", label="inflow (blocks/s)")
    dr = sm["drain"]["series"]
    if dr:
        ax2.plot([x["s"] for x in dr], [x["rate_blocks_per_s"] for x in dr],
                 ".-", color="#2ca02c", lw=1, ms=3,
                 label="drain (blocks/s)")
    dfc = sm["deficit"]["series"]
    if dfc:
        ax2.plot([x["s"] for x in dfc],
                 [x["deficit_blocks"] / x["dt_s"] for x in dfc],
                 ".-", color="#d62728", lw=1, ms=3,
                 label="deficit (blocks/s)")
    ax2.axhline(0.0, color="k", lw=0.8, ls=":")
    if w["nominal_start_s"] is not None and w["end_s"] is not None:
        ax2.axvspan(w["nominal_start_s"], w["end_s"],
                    color="#1f77b4", alpha=0.06)
    ax2.set_xlabel("vclock (s)")
    ax2.set_ylabel("blocks/s")
    ax2.legend(fontsize=7, loc="upper left")

    fig.tight_layout(rect=(0, 0, 1, 0.97))
    fig.savefig(path, dpi=120)
    plt.close(fig)
    return None


def _fmt(x, nd=1, scale=1.0):
    return "-" if x is None else ("%.*f" % (nd, x * scale))


def report(run_dir, plot=True):
    """Report one run dir: writes summary.json (and fill.png unless
    plot=False); returns the summary dict."""
    if not os.path.isdir(run_dir):
        raise ReportError("not a directory")
    trace = os.path.join(run_dir, "trace.log")
    if not os.path.isfile(trace):
        raise ReportError("no trace.log (the QEMU -trace output)")
    notes = []

    params, pnotes = read_run_json(run_dir)
    notes += pnotes

    fill_path = os.path.join(run_dir, "fill.csv")
    fill = []
    if os.path.isfile(fill_path):
        try:
            fill = parse_fill_csv(fill_path)
            if not fill:
                notes.append("fill.csv has no usable rows")
        except OSError as e:
            notes.append("fill.csv unreadable (%s)" % e)
    else:
        notes.append("fill.csv absent: no drain/deficit/fill samples")

    uart = None
    uart_path = os.path.join(run_dir, "uart.raw")
    if os.path.isfile(uart_path):
        try:
            uart = parse_uart(uart_path)
        except OSError as e:
            notes.append("uart.raw unreadable (%s)" % e)
    else:
        notes.append("uart.raw absent: no UART counts")

    events = None
    events_path = os.path.join(run_dir, "events.log")
    if os.path.isfile(events_path):
        try:
            events = parse_events_log(events_path)
        except OSError as e:
            notes.append("events.log unreadable (%s)" % e)
    else:
        notes.append("events.log absent")

    truth_path = os.path.join(run_dir, "flash-truth.txt")
    n_files = None
    if os.path.isfile(truth_path):
        try:
            n_files = count_flash_truth_files(truth_path)
        except OSError as e:
            notes.append("flash-truth.txt unreadable (%s)" % e)
    else:
        notes.append("flash-truth.txt absent: files_per_s is null "
                     "(run l4-flash-truth.py on the snapshot to get it)")

    try:
        td = parse_trace(trace)
    except OSError as e:
        raise ReportError("cannot read %s: %s" % (trace, e))
    sm = compute(td, fill, uart, events, n_files)
    sm["run"] = os.path.basename(os.path.normpath(run_dir)) or run_dir
    sm["params"] = {k: params[k] for k in RUN_PARAM_KEYS if k in params}
    sm["params"].update({k: v for k, v in params.items()
                         if k not in sm["params"]})
    sm["generated_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                        time.gmtime())
    sm["defs"] = DEFS
    sm["notes"] = notes

    out_path = os.path.join(run_dir, "summary.json")
    plot_note = None
    if plot:
        # the plot needs the metrics only, so it runs before the one
        # summary.json write (the file always carries the "plot" key)
        try:
            plot_note = make_plot(os.path.join(run_dir, "fill.png"),
                                  sm, fill, td)
        except Exception as e:                 # a plot must not kill the report
            plot_note = "failed: %s" % e
    sm["plot"] = "not requested" if not plot else (plot_note or "fill.png")
    if plot_note:
        notes.append("fill.png %s" % plot_note)
    with open(out_path, "w") as fh:
        json.dump(sm, fh, indent=1)
        fh.write("\n")

    print("ROTB-FILL: run=%s window=%s..%ss inflow=%s b/s drain=%s b/s "
          "deficit=%s b/s peak=%s first_full=%ss first_overflow=%s "
          "drops=%d overwrites=%d flash_busy=%sms files=%s -> %s"
          % (sm["run"],
             _fmt(sm["window"]["nominal_start_s"]),
             _fmt(sm["window"]["end_s"]),
             _fmt(sm["inflow"]["mean_blocks_per_s"]),
             _fmt(sm["drain"]["mean_blocks_per_s"]),
             _fmt(sm["deficit"]["mean_blocks_per_s"]),
             _fmt(sm["fill"]["peak"]["frac"] if sm["fill"]["peak"]
                  else None, 2),
             _fmt(sm["fill"]["first_full_s"]),
             _fmt(sm["overflow"]["first_rotb_overflow"]["vclock_s"]
                  if sm["overflow"]["first_rotb_overflow"] else None),
             sm["overflow"]["chunk_drop_count"],
             sm["overflow"]["overwrite_count"],
             _fmt(sm["flash"]["busy_us_total"] / 1000.0),
             _fmt(sm["files"]["n_files"], 0),
             out_path))
    return sm


def _pnum(x, nd=1):
    return "-" if x is None else "%.*f" % (nd, x)


def _param_lat(v):
    if isinstance(v, bool):
        return "on" if v else "off"
    if isinstance(v, str):
        return v[:4]
    return "-"


def _matrix_row(d, sm):
    p = sm.get("params") or {}
    peak = sm.get("fill", {}).get("peak")
    ovf = sm.get("overflow", {}).get("first_rotb_overflow")
    icnt = p.get("icount_shift", p.get("icount"))
    icnt = "-" if icnt is None else str(icnt)
    fops = sm.get("flash", {}).get("ops_per_s_in_window_by_kind")
    fops = sum(fops.values()) if fops else None
    return ("%-22s %-4s %-5s %-6s %-4s %-4s | %-5s %-8s %-8s | %-8s %-8s "
            "%-8s | %-5s %-4s | %-7s %-7s"
            % (d[:22] or ".",
               str(p.get("cell", "-"))[:4],
               _pnum(p.get("ticks"), 0) if p.get("ticks") is not None
               else "-",
               _pnum(p.get("cps"), 0) if p.get("cps") is not None else "-",
               icnt[:4], _param_lat(p.get("latency")),
               _pnum(peak["frac"], 2) if peak else "-",
               _pnum(sm.get("fill", {}).get("first_full_s")),
               _pnum(ovf["vclock_s"]) if ovf else "-",
               _pnum(sm.get("inflow", {}).get("mean_blocks_per_s")),
               _pnum(sm.get("drain", {}).get("mean_blocks_per_s")),
               _pnum(sm.get("deficit", {}).get("mean_blocks_per_s")),
               str(sm.get("overflow", {}).get("chunk_drop_count", "-")),
               str(sm.get("overflow", {}).get("overwrite_count", "-")),
               _pnum(fops, 2),
               _pnum(sm.get("files", {}).get("files_per_s"), 2)))


_MATRIX_HDR = ("%-22s %-4s %-5s %-6s %-4s %-4s | %-5s %-8s %-8s | %-8s "
               "%-8s %-8s | %-5s %-4s | %-7s %-7s"
               % ("run", "cell", "tick", "cps", "icnt", "lat",
                  "peak", "full_s", "ovf_s", "in_b/s", "dr_b/s",
                  "df_b/s", "drop", "ovw", "fop/s", "file/s"))


def matrix(run_dirs):
    """--matrix: one table row per run dir from its summary.json."""
    rc = 0
    print(_MATRIX_HDR)
    for d in run_dirs:
        path = os.path.join(d, "summary.json")
        try:
            with open(path) as fh:
                sm = json.load(fh)
        except (OSError, ValueError) as e:
            print("%-22s no/bad summary.json (%s)" % (d[:22], e),
                  file=sys.stderr)
            rc = 1
            continue
        print(_matrix_row(d, sm))
    return rc


def _st_write(path, text):
    with open(path, "w") as fh:
        fh.write(text)


def self_test():
    """Offline: a synthetic run dir in a temp dir -> exact expected
    inflow, drain (including a wrap) and first-overflow time.

    Scenario: capacity 100 blocks; 9 chunks of 49 at vclock s=100..105
    and 107..109, one chunk_drop (ring-full) at s=106; the rotb_overflow
    trace between the chunks at s=102 and s=103 (bracket [102, 103]);
    a rotb_overwrite after the last chunk (no later vclock: open
    bracket); an erase op before the first chunk and read/prog ops
    inside the window; fill.csv samples at s=100..110 with rd advancing
    30 blocks/s and wrapping twice; UART with all three marks.
    """
    ts = "2026-11-13T10:00:00.000000Z"

    def chunk(s, rotb):
        return ("%s stixdet_chunk chunk: nom_bank 0, 49 blocks, "
                "scet 0x000186a0:0x0000, int_ticks=0 eacc_events=1800 "
                "rotb_count=%d at vclock_ns=%d\n" % (ts, rotb, s * NS))

    trace = (
        "%s stixflash_op_start op start cmd=1 mcm=0 chip=0 page=0x10 "
        "delay_us=1500\n" % ts
        + "%s stixflash_op_done op done cmd=1 page_address=0x10 "
          "cmd_sta=0x21\n" % ts
        + chunk(100, 49) + chunk(101, 98) + chunk(102, 98)
        + "%s stixctrl_rotb_overflow rotb overflow: block dropped, "
          "count=100\n" % ts
        + chunk(103, 98)
        + "%s stixflash_op_start op start cmd=2 mcm=0 chip=0 page=0x10 "
          "delay_us=130\n" % ts
        + "%s stixflash_op_done op done cmd=2 page_address=0x10 "
          "cmd_sta=0x21\n" % ts
        + "%s stixflash_op_start op start cmd=2 mcm=0 chip=0 page=0x11 "
          "delay_us=130\n" % ts
        + "%s stixflash_op_done op done cmd=2 page_address=0x11 "
          "cmd_sta=0x21\n" % ts
        + chunk(104, 98) + chunk(105, 98)
        + "%s stixdet_chunk_drop chunk dropped (ring-full): nom_bank 0, "
          "int_ticks=0 rotb_count=98 at vclock_ns=%d\n" % (ts, 106 * NS)
        + chunk(107, 98) + chunk(108, 98)
        + "%s stixflash_op_start op start cmd=3 mcm=0 chip=0 page=0x20 "
          "delay_us=300\n" % ts
        + "%s stixflash_op_done op done cmd=3 page_address=0x20 "
          "cmd_sta=0x21\n" % ts
        + chunk(109, 98)
        + "%s stixctrl_rotb_overwrite rotb overwrite: dropped block "
          "rd=7 count=100\n" % ts)

    fill_rows = []
    counts = {100: 0, 101: 49}
    counts.update({s: 100 for s in range(102, 111)})
    for i, s in enumerate(range(100, 111)):
        fill_rows.append("%d,%d,%d,%d,%d,%d,100"
                         % (i, s * NS, counts[s],
                            1 if counts[s] >= 100 else 0,
                            (30 * i) % 100, (30 * i + counts[s]) % 100))
    fill_csv = ("wall_mono,vclock_ns,count,full,rd,wr,capacity\n"
                + "\n".join(fill_rows) + "\n")

    uart = ("init: SAFE mode\n"
            "00:01:16.0  RotAccuCompactor  *E packet sequence error in "
            "ROTB frame\n"
            "00:01:17.0  RotAccumulator   *E RotBuf overrun occured\n"
            "00:01:18.0  RotAccuCompactor  *E packet sequence error "
            "again\n"
            "00:01:19.0  MemFilesystem     *E Partition of type 0 is "
            "full\n")

    events = ('{"t_wall": 50.0, "event": "conf"}\n'
              '{"t_wall": 70.0, "event": "nominal", "vclock_ns": '
              '100000000000}\n'
              'garbage line\n')

    truth = ("# l4-flash-truth: 3 RAW_COUNTS files, 7 archived chunks\n"
             "file 010000640000 1000000.000 3\n"
             "file 010000650000 1000010.000 2\n"
             "file 010000660000 1000020.000 2\n")

    run_json = json.dumps({"cell": "M2", "ticks": 0, "cps": 2000,
                           "icount_shift": 6, "latency": True,
                           "seconds": 600, "rotb_overflow": "drop-new"})

    tmp = tempfile.mkdtemp(prefix="stix-rotb-fill-report-selftest-")
    try:
        _st_write(os.path.join(tmp, "trace.log"), trace)
        _st_write(os.path.join(tmp, "fill.csv"), fill_csv)
        _st_write(os.path.join(tmp, "uart.raw"), uart)
        _st_write(os.path.join(tmp, "events.log"), events)
        _st_write(os.path.join(tmp, "flash-truth.txt"), truth)
        _st_write(os.path.join(tmp, "run.json"), run_json)
        sm = report(tmp, plot=False)

        ok = 0

        def chk(cond, what):
            nonlocal ok
            if not cond:
                raise AssertionError(what)
            ok += 1

        chk(sm["window"] == {"nominal_start_s": 100.0, "end_s": 110.0,
                             "duration_s": 10.0}, "window %r" % sm["window"])
        inf = sm["inflow"]
        chk(inf["mean_blocks_per_s"] == 49.0, "inflow mean")
        chk(inf["total_blocks"] == 490 and inf["chunk_blocks"] == 441
            and inf["chunk_count"] == 9 and inf["drop_count"] == 1
            and inf["drop_blocks"] == 49, "inflow totals %r" % inf)
        chk(len(inf["series"]) == 10 and inf["series"][0]["s"] == 100.0
            and all(b["blocks"] == 49 for b in inf["series"]),
            "inflow series %r" % inf["series"][:2])
        dr = sm["drain"]
        chk(dr["mean_blocks_per_s"] == 30.0 and dr["total_blocks"] == 300,
            "drain mean/total %r" % ((dr["mean_blocks_per_s"],
                                      dr["total_blocks"]),))
        chk(len(dr["series"]) == 10
            and all(x["blocks"] == 30 for x in dr["series"]),
            "drain series (the wrap: rd 90 -> 20 must be +30) %r"
            % dr["series"][3])
        chk(sm["deficit"]["mean_blocks_per_s"] == 19.0
            and all(x["deficit_blocks"] == 19 and x["inflow_blocks"] == 49
                    and x["drained_blocks"] == 30
                    for x in sm["deficit"]["series"]),
            "deficit %r" % sm["deficit"]["mean_blocks_per_s"])
        fl = sm["fill"]
        chk(fl["peak"] == {"frac": 1.0, "count": 100, "vclock_s": 102.0},
            "peak %r" % fl["peak"])
        chk(fl["first_full_s"] == 102.0 and fl["full_samples"] == 9
            and fl["capacity_blocks"] == 100 and fl["peak_rotb_count"] == 98,
            "fill %r" % fl)
        ov = sm["overflow"]
        chk(ov["chunk_drop_count"] == 1
            and ov["chunk_drop_by_why"] == {"ring-full": 1},
            "drop count %r" % ov)
        chk(ov["rotb_overflow_count"] == 1
            and ov["first_rotb_overflow"] ==
            {"vclock_s": 102.5, "bracket_s": [102.0, 103.0]},
            "first overflow %r" % ov["first_rotb_overflow"])
        chk(ov["overwrite_count"] == 1
            and ov["first_overwrite"] ==
            {"vclock_s": None, "bracket_s": [109.0, None]},
            "first overwrite (open bracket) %r" % ov["first_overwrite"])
        u = sm["uart"]
        chk(u["counts"]["RotBuf overrun occured"] ==
            {"count": 1, "first_line": 2}, "uart overrun %r" % u)
        chk(u["counts"]["packet sequence error"] ==
            {"count": 2, "first_line": 1}, "uart seq %r" % u)
        chk(u["counts"]["Partition of type"] ==
            {"count": 1, "first_line": 4}, "uart partition %r" % u)
        chk(u["first_occurrence_order"] == ["packet sequence error",
                                            "RotBuf overrun occured",
                                            "Partition of type"],
            "uart order %r" % u["first_occurrence_order"])
        chk(sm["events_log"] == {"lines": 3, "json_lines": 2,
                                 "nominal_lines": 1}, "events %r"
            % sm["events_log"])
        fl2 = sm["flash"]
        chk(fl2["ops_by_kind"] == {"erase": 1, "read": 2, "prog": 1}
            and fl2["busy_us_by_kind"] == {"erase": 1500, "read": 260,
                                           "prog": 300}
            and fl2["busy_us_total"] == 2060 and fl2["ops_done"] == 4
            and fl2["ops_aborted"] == 0, "flash %r" % fl2)
        ops = fl2["ops_per_s_in_window_by_kind"]
        chk(ops["erase"] == 0.0 and round(ops["read"], 6) == 0.2
            and round(ops["prog"], 6) == 0.1, "flash ops/s %r" % ops)
        chk(sm["files"] == {"n_files": 3, "files_per_s": 0.3},
            "files %r" % sm["files"])
        chk(sm["params"] == {"cell": "M2", "ticks": 0, "cps": 2000,
                             "icount_shift": 6, "latency": True,
                             "seconds": 600, "rotb_overflow": "drop-new"},
            "params %r" % sm["params"])
        with open(os.path.join(tmp, "summary.json")) as fh:
            sm2 = json.load(fh)
        chk(sm2["inflow"]["mean_blocks_per_s"] == 49.0
            and sm2["defs"] == DEFS and sm2["plot"] == "not requested",
            "summary.json round-trip")
        print("ROTB-FILL-REPORT SELF-TEST: PASS (%d checks)" % ok)
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="rotb-fill-report.py",
        description="ROTB fill report (rotb-fill plan RF-4): summary.json "
                    "+ fill.png per run dir")
    ap.add_argument("dirs", nargs="*", metavar="RUN_DIR",
                    help="run dirs (run.json, fill.csv, trace.log, "
                         "uart.raw, events.log)")
    ap.add_argument("--matrix", action="store_true",
                    help="one table row per run dir from summary.json")
    ap.add_argument("--self-test", action="store_true",
                    help="offline self-test on a synthetic run dir")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.dirs:
        ap.error("RUN_DIR required (or --self-test)")
    if args.matrix:
        return matrix(args.dirs)
    rc = 0
    for d in args.dirs:
        try:
            report(d)
        except ReportError as e:
            print("rotb-fill-report: %s: %s" % (d, e), file=sys.stderr)
            rc = 2
    return rc


if __name__ == "__main__":
    sys.exit(main())
