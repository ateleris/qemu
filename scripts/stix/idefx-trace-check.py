#!/usr/bin/env python3
"""Count the FSW's ASIC verification runs in a stixdet_idefx_cmd trace.

A successful ASIC verification is silent (ENABLE_TRACE_DETFDIR 0,
configFSW.h:89), so detector-smoke takes its evidence from the model
trace. QEMU runs with -msg timestamp=on and -trace events=FILE,file=TRACE
for two events, so the lines read

  <UTC ISO time> stixdet_idefx_cmd IDeF-X grp G word 0xWWWWW
      delivered_grp_mask 0xMMMM response 0xRRRR
  <UTC ISO time> stixdet_fsm_strobe fsmCmd strobes=0xS enable=E ...

(accepted-diffs §32; G is GrpAddr, the word cmd << 16 | data; E is the
FSM Enable bit after the fsmCmd write). A read address block is
word & 0xFC07F == 0x84000 with reg = (word >> 7) & 0x7F in 1..12
(Detectors.c:817). Delivered read address blocks form verification runs
(XrayDetFDIR.c:140-300, 596-690): a run ends at an FSM strobe line or a
gap > 5 s between two blocks. Each run must be exactly 384 = 12
registers x 32 ASICs: for every register, every grp 0..15 (grp =
fpga_det / 2) exactly twice (asic 0 and 1). At least --min-runs runs
are required, and an undelivered read address block is a failure.

The NOMINAL window is taken from the trace itself, by ORDER, not by
time: it opens at the FSM strobe that turns Enable on (SetEnable at the
NOMINAL entry) and closes at the next strobe that turns it off
(ClearEnable at the exit). Every run must lie inside one such window.
No clock is compared with another: the trace's wall-clock stamps (QEMU
-msg timestamp=on) can be stepped mid-run (WSL's timesyncd steps the
wall clock back every poll), so they serve only for the gap rule above,
which a step cannot break (runs last ~0.05 s and are ~30 s apart; a
backward step inside a run gives a negative gap, which joins). Measured
on 2026-11-11: SetEnable (strobes 0x1, enable=1) precedes the first run,
only QlClear strobes (enable=1) come between runs, and ClearEnable
(0x2, enable=0) follows the last.

--expect-none is the detector-smoke --negative check (det-idefx=off
keeps the registers as plain storage, so no stixdet_idefx_cmd fires):
the trace must exist and hold no IDeF-X command word at all, hence 0
runs and 0 undelivered blocks. FSM strobe lines are allowed.

Usage:
  idefx-trace-check.py TRACE [--min-runs N] [--expect-none]
  idefx-trace-check.py --self-test
Exit codes: 0 the check passed, 1 it failed, 2 the trace is missing or
unreadable (or bad usage). stdlib only.
"""
import argparse
import datetime
import re
import sys

NUM_GRP = 16
NUM_REG = 12
RUN_BLOCKS = NUM_REG * NUM_GRP * 2   # 384
GAP_S = 5.0

LINE = re.compile(r"^(\S+) stixdet_idefx_cmd IDeF-X grp (\d+) word "
                  r"0x([0-9a-f]+) delivered_grp_mask 0x([0-9a-f]+)")
FSM = re.compile(r"^(\S+) stixdet_fsm_strobe fsmCmd strobes=0x([0-9a-f]+) "
                 r"enable=(\d)")


def stamp(iso_time):
    return datetime.datetime.fromisoformat(
        iso_time.replace("Z", "+00:00")).timestamp()


def analyse(lines, min_runs=3):
    """Returns (summary dict, list of failures) for the positive check."""
    words = 0
    undelivered = 0
    runs = []           # each: {"reads": [(t, reg, grp)], "in_nominal"}
    windows = 0         # NOMINAL windows opened (Enable 0 -> 1)
    enable = False
    open_run = None
    for line in lines:
        f = FSM.match(line)
        if f:
            open_run = None                 # a strobe ends a run
            now = f.group(3) == "1"
            if now and not enable:
                windows += 1
            enable = now
            continue
        m = LINE.match(line)
        if not m:
            continue
        words += 1
        word = int(m.group(3), 16)
        if word & 0xFC07F != 0x84000:
            continue
        reg = (word >> 7) & 0x7F
        if not 1 <= reg <= NUM_REG:
            continue
        if int(m.group(4), 16) == 0:
            undelivered += 1
            continue
        r = (stamp(m.group(1)), reg, int(m.group(2)))
        if open_run is None or r[0] - open_run["reads"][-1][0] > GAP_S:
            open_run = {"reads": [], "in_nominal": enable}
            runs.append(open_run)
        open_run["reads"].append(r)
        open_run["in_nominal"] = open_run["in_nominal"] and enable
    fails = []
    if undelivered:
        fails.append(f"{undelivered} undelivered read address blocks")
    desc = []
    for i, run in enumerate(runs):
        rd = run["reads"]
        desc.append(f"{len(rd)}" + ("" if run["in_nominal"]
                                    else " outside NOMINAL"))
        bad = []
        for reg in range(1, NUM_REG + 1):
            for grp in range(NUM_GRP):
                n = sum(1 for _, r, g in rd if r == reg and g == grp)
                if n != 2:
                    bad.append(f"R{reg} grp {grp} x{n}")
        stray = [g for _, _, g in rd if g >= NUM_GRP]
        if len(rd) != RUN_BLOCKS or bad or stray:
            fails.append(f"run {i}: {len(rd)} read address blocks, want "
                         f"{RUN_BLOCKS} = every reg 1..12 x every grp "
                         f"0..15 twice" +
                         (f" ({', '.join(bad[:4])}"
                          f"{', ...' if len(bad) > 4 else ''})" if bad
                          else ""))
        if windows == 0:
            fails.append(f"run {i}: no NOMINAL window to place it in "
                         f"(no FSM SetEnable in the trace)")
        elif not run["in_nominal"]:
            fails.append(f"run {i}: outside the NOMINAL window (FSM "
                         f"Enable off)")
    if len(runs) < min_runs:
        fails.append(f"{len(runs)} verification runs, want >= {min_runs}")
    return {"words": words, "runs": len(runs), "undelivered": undelivered,
            "windows": windows, "desc": desc}, fails


def check_none(summary):
    fails = []
    if summary["words"] or summary["runs"] or summary["undelivered"]:
        fails.append(f"want an empty trace (no IDeF-X command word): "
                     f"{summary['words']} command words, "
                     f"{summary['runs']} runs, {summary['undelivered']} "
                     f"undelivered read address blocks")
    return fails


# --- self-test ---------------------------------------------------------

def iso(t):
    return datetime.datetime.fromtimestamp(
        t, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def fsm(t, strobes, enable):
    return (f"{iso(t)} stixdet_fsm_strobe fsmCmd strobes=0x{strobes:x} "
            f"enable={enable} active_ql_bank=0 ql_ready=0")


def synth_run(t0, skip=None, undelivered=None, extra_grp=None,
              step_at=None, step_s=0.0):
    """The trace lines of one verification run starting at epoch t0: for
    each reg, each grp, two read address blocks (asic 0, 1), each after
    a SCINITSEQ word, with a data block after it. From register @step_at
    on, the stamps are shifted by @step_s (a wall-clock step mid-run)."""
    out = []
    t = t0
    for reg in range(1, NUM_REG + 1):
        if reg == step_at:
            t += step_s
        for grp in range(NUM_GRP):
            for asic in (0, 1):
                g = extra_grp if (extra_grp is not None and reg == 5 and
                                  grp == 3) else grp
                if skip == (reg, grp, asic):
                    continue
                mask = 0 if undelivered == (reg, grp, asic) else 1 << g
                for w in (0xBFD30 | asic << 6, 0x84000 | reg << 7, 0x80000):
                    out.append(f"{iso(t)} stixdet_idefx_cmd IDeF-X grp {g} "
                               f"word 0x{w:05x} delivered_grp_mask "
                               f"0x{mask:04x} response 0x0000")
                t += 0.0001
    return out


def nominal(runs, t0=1_700_000_000.0, step=None):
    """ClearEnable, SetEnable, then per run a QlClear strobe and the run
    30 s apart, then ClearEnable. With @step = (after_run, seconds) the
    wall stamps jump by @seconds after that run (a stepped wall clock)."""
    out = [fsm(t0, 0x2, 0), fsm(t0 + 1, 0x1, 1)]
    shift = 0.0
    for i, run in enumerate(runs):
        t = t0 + 20 + 30 * i + shift
        out += [fsm(t - 0.5, 0x80, 1)] + run(t)
        if step is not None and step[0] == i:
            shift += step[1]
    out.append(fsm(t0 + 20 + 30 * len(runs) + shift, 0x2, 0))
    return out


def self_test():
    ok = [synth_run] * 3
    s, f = analyse(nominal(ok))
    assert f == [] and s["runs"] == 3 and s["windows"] == 1, (s, f)
    assert s["words"] == 3 * RUN_BLOCKS * 3
    # 383 blocks
    s, f = analyse(nominal([lambda t: synth_run(t, skip=(7, 9, 1))] +
                           ok[1:]))
    assert len(f) == 1 and "run 0: 383" in f[0] and "R7 grp 9 x1" in f[0], f
    # an undelivered block
    s, f = analyse(nominal([lambda t: synth_run(t, undelivered=(1, 0, 0))] +
                           ok[1:]))
    assert any("1 undelivered" in x for x in f), f
    # a run after ClearEnable, and a run before SetEnable: outside
    tr = nominal(ok)
    s, f = analyse(tr + synth_run(1_700_000_200.0))
    assert f == ["run 3: outside the NOMINAL window (FSM Enable off)"], f
    s, f = analyse(synth_run(1_699_999_990.0) + tr)
    assert f == ["run 0: outside the NOMINAL window (FSM Enable off)"], f
    # no SetEnable at all: Enable reads 0 on every strobe
    s, f = analyse([x.replace("enable=1", "enable=0") for x in tr])
    assert any("no FSM SetEnable" in x for x in f), f
    # one grp read 32 times for a register (grp 3's R5 reads go to grp 7)
    s, f = analyse(nominal([lambda t: synth_run(t, extra_grp=7)] + ok[1:]))
    assert len(f) == 1 and "R5 grp 3 x0" in f[0] and \
        "R5 grp 7 x4" in f[0], f
    # two runs only
    s, f = analyse(nominal(ok[:2]))
    assert f == ["2 verification runs, want >= 3"], f
    # a stepped wall clock: back 25 s or forward 40 s between runs, or
    # back 2.5 s (a timesyncd step) inside a run: the verdict is the same,
    # because the window is placed by order and the gap rule is relative
    for step in ((0, -25.0), (1, 40.0)):
        s, f = analyse(nominal(ok, step=step))
        assert f == [] and s["runs"] == 3, (step, f)
    s, f = analyse(nominal([lambda t: synth_run(t, step_at=6, step_s=-2.5)] +
                           ok[1:]))
    assert f == [] and s["runs"] == 3, f
    # --expect-none: empty (FSM strobes allowed) passes, anything else not
    s, _ = analyse([fsm(1.0, 0x1, 1), fsm(2.0, 0x2, 0)])
    assert check_none(s) == []
    s, _ = analyse(synth_run(1_700_000_000.0)[:1])   # one SCINITSEQ word
    assert len(check_none(s)) == 1
    print("IDEFX-TRACE-CHECK SELF-TEST: PASS")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Count ASIC verification runs in an IDeF-X trace")
    ap.add_argument("trace", nargs="?")
    ap.add_argument("--min-runs", type=int, default=3)
    ap.add_argument("--expect-none", action="store_true",
                    help="require an empty trace (det-idefx=off)")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.trace:
        ap.error("TRACE is required")
    try:
        with open(args.trace) as fh:
            lines = fh.readlines()
    except OSError as e:
        print(f"IDEFX-TRACE-CHECK: ERROR (cannot read {args.trace}: {e})")
        return 2
    summary, fails = analyse(lines, args.min_runs)
    print(f"ASIC verification runs (delivered read address blocks): "
          f"{summary['runs']}"
          + (" (" + ", ".join(summary["desc"]) + ")" if summary["desc"]
             else "")
          + f" in {summary['windows']} NOMINAL window(s);"
            f" {summary['words']} IDeF-X command words,"
            f" {summary['undelivered']} undelivered read address blocks")
    if args.expect_none:
        fails = check_none(summary)
    for x in fails:
        print(f"  {x}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
