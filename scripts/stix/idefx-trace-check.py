#!/usr/bin/env python3
"""Count the FSW's ASIC verification runs in a stixdet_idefx_cmd trace.

A successful ASIC verification is silent (ENABLE_TRACE_DETFDIR 0,
configFSW.h:89), so detector-smoke takes its evidence from the model
trace. QEMU runs with -msg timestamp=on and
-trace enable=stixdet_idefx_cmd,file=TRACE, so every line reads

  <UTC ISO time> stixdet_idefx_cmd IDeF-X grp G word 0xWWWWW
      delivered_grp_mask 0xMMMM response 0xRRRR

(accepted-diffs §32; G is GrpAddr, the word cmd << 16 | data). A read
address block is word & 0xFC07F == 0x84000 with reg = (word >> 7) & 0x7F
in 1..12 (Detectors.c:817). Delivered read address blocks are clustered
by a gap > 5 s; each cluster is one verification run
(XrayDetFDIR.c:140-300, 596-690) and must be exactly 384 = 12 registers
x 32 ASICs: for every register, every grp 0..15 (grp = fpga_det / 2)
exactly twice (asic 0 and 1). Every run must lie in the NOMINAL window
[T_nom - 2 s, T_exit + 2 s]. At least --min-runs runs are required, and
an undelivered read address block is a failure.

Clocks. T_nom / T_exit are the probe's CLOCK_MONOTONIC stamps ([m=...])
of 0021 04 05 / 0021 05 04; the trace's stamps are wall-clock (QEMU's
-msg timestamp=on), and the wall clock can be stepped mid-run (WSL's
timesyncd). The gate therefore samples (realtime, monotonic) before and
after the run (--clock R0 M0 R1 M1), and the window is placed on the
wall clock as R0 + (m - M0). That holds only while the wall clock kept
pace: when (R1 - R0) / (M1 - M0) differs from 1 by more than 0.5 %, the
result is INCONCLUSIVE (exit 3) — never a pass, never a model failure.
Within 0.5 % a 300 s run drifts at most 1.5 s, inside the 2 s slack;
the 5 s clustering gap is between runs about 30 s apart. A step that is
undone before the end sample is not seen; the monotonic NOMINAL window
the ql check uses does not depend on this at all.

--expect-none is the detector-smoke --negative check (det-idefx=off
keeps the registers as plain storage, so no stixdet_idefx_cmd fires):
the trace must exist and hold no IDeF-X command word at all, hence 0
runs and 0 undelivered blocks. It uses no time, so no clock either.

Usage:
  idefx-trace-check.py TRACE --clock R0 M0 R1 M1 [--t-nom-m M]
      [--t-exit-m M] [--min-runs N]
  idefx-trace-check.py TRACE --expect-none
  idefx-trace-check.py --self-test
Exit codes: 0 the check passed, 1 it failed, 2 the trace is missing or
unreadable (or bad usage), 3 INCONCLUSIVE (the wall clock was stepped).
stdlib only.
"""
import argparse
import datetime
import re
import sys

NUM_GRP = 16
NUM_REG = 12
RUN_BLOCKS = NUM_REG * NUM_GRP * 2   # 384
GAP_S = 5.0
WINDOW_SLACK_S = 2.0
CLOCK_TOLERANCE = 0.005   # wall/monotonic rate, 0.5 %

LINE = re.compile(r"^(\S+) stixdet_idefx_cmd IDeF-X grp (\d+) word "
                  r"0x([0-9a-f]+) delivered_grp_mask 0x([0-9a-f]+)")


def clock_step(r0, m0, r1, m1):
    """None when the wall clock kept pace with the monotonic clock over
    the run (within CLOCK_TOLERANCE), else the INCONCLUSIVE reason."""
    if m1 <= m0:
        return f"monotonic clock did not advance ({m0} -> {m1})"
    ratio = (r1 - r0) / (m1 - m0)
    if abs(ratio - 1.0) > CLOCK_TOLERANCE:
        return (f"clock step: the wall clock advanced {r1 - r0:.1f} s over "
                f"{m1 - m0:.1f} s monotonic, ratio {ratio:.3f}")
    return None


def to_wall(m, r0, m0):
    """A monotonic stamp on the wall clock of the run start."""
    return r0 + (m - m0)


def analyse(lines, t_nom=None, t_exit=None, min_runs=3):
    """Returns (summary dict, list of failures) for the positive check."""
    words = 0
    reads = []          # (epoch, reg, grp) of delivered read addr blocks
    undelivered = 0
    for line in lines:
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
        ts = datetime.datetime.fromisoformat(
            m.group(1).replace("Z", "+00:00")).timestamp()
        reads.append((ts, reg, int(m.group(2))))
    runs = []
    for r in reads:
        if not runs or r[0] - runs[-1][-1][0] > GAP_S:
            runs.append([])
        runs[-1].append(r)
    fails = []
    if undelivered:
        fails.append(f"{undelivered} undelivered read address blocks")
    desc = []
    for i, run in enumerate(runs):
        start, end = run[0][0], run[-1][0]
        rel = f"+{start - t_nom:.1f} s" if t_nom is not None else "?"
        desc.append(f"{len(run)} at T_nom {rel} ({end - start:.2f} s long)")
        bad = []
        for reg in range(1, NUM_REG + 1):
            for grp in range(NUM_GRP):
                n = sum(1 for _, r, g in run if r == reg and g == grp)
                if n != 2:
                    bad.append(f"R{reg} grp {grp} x{n}")
        stray = [g for _, _, g in run if g >= NUM_GRP]
        if len(run) != RUN_BLOCKS or bad or stray:
            fails.append(f"run {i}: {len(run)} read address blocks, want "
                         f"{RUN_BLOCKS} = every reg 1..12 x every grp "
                         f"0..15 twice" +
                         (f" ({', '.join(bad[:4])}"
                          f"{', ...' if len(bad) > 4 else ''})" if bad
                          else ""))
        if t_nom is None or t_exit is None:
            fails.append(f"run {i}: no NOMINAL window to place it in")
        elif start < t_nom - WINDOW_SLACK_S or \
                end > t_exit + WINDOW_SLACK_S:
            fails.append(f"run {i}: outside the NOMINAL window")
    if len(runs) < min_runs:
        fails.append(f"{len(runs)} verification runs, want >= {min_runs}")
    return {"words": words, "runs": len(runs), "undelivered": undelivered,
            "desc": desc}, fails


def check_none(summary):
    fails = []
    if summary["words"] or summary["runs"] or summary["undelivered"]:
        fails.append(f"want an empty trace (no IDeF-X command word): "
                     f"{summary['words']} command words, "
                     f"{summary['runs']} runs, {summary['undelivered']} "
                     f"undelivered read address blocks")
    return fails


# --- self-test ---------------------------------------------------------

def synth_run(t0, skip=None, undelivered=None, extra_grp=None):
    """The trace lines of one verification run starting at epoch t0: for
    each reg, each grp, two read address blocks (asic 0, 1), each after
    a SCINITSEQ word, with a data block after it."""
    out = []
    t = t0
    for reg in range(1, NUM_REG + 1):
        for grp in range(NUM_GRP):
            for asic in (0, 1):
                g = extra_grp if (extra_grp is not None and reg == 5 and
                                  grp == 3) else grp
                if skip == (reg, grp, asic):
                    continue
                mask = 0 if undelivered == (reg, grp, asic) else 1 << g
                iso = datetime.datetime.fromtimestamp(
                    t, datetime.timezone.utc).strftime(
                        "%Y-%m-%dT%H:%M:%S.%fZ")
                for w in (0xBFD30 | asic << 6, 0x84000 | reg << 7, 0x80000):
                    out.append(f"{iso} stixdet_idefx_cmd IDeF-X grp {g} "
                               f"word 0x{w:05x} delivered_grp_mask "
                               f"0x{mask:04x} response 0x0000")
                t += 0.0001
    return out


def self_test():
    t_nom = 1_700_000_000.0
    t_exit = t_nom + 140
    ok3 = synth_run(t_nom + 20) + synth_run(t_nom + 50) + \
        synth_run(t_nom + 80)
    s, f = analyse(ok3, t_nom, t_exit)
    assert f == [] and s["runs"] == 3, (s, f)
    assert s["words"] == 3 * RUN_BLOCKS * 3
    # 383 blocks
    s, f = analyse(synth_run(t_nom + 20, skip=(7, 9, 1)) +
                   synth_run(t_nom + 50) + synth_run(t_nom + 80),
                   t_nom, t_exit)
    assert len(f) == 1 and "run 0: 383" in f[0] and "R7 grp 9 x1" in f[0], f
    # an undelivered block
    s, f = analyse(synth_run(t_nom + 20, undelivered=(1, 0, 0)) +
                   synth_run(t_nom + 50) + synth_run(t_nom + 80),
                   t_nom, t_exit)
    assert any("1 undelivered" in x for x in f), f
    # a run outside the window
    s, f = analyse(ok3 + synth_run(t_exit + 30), t_nom, t_exit)
    assert f == ["run 3: outside the NOMINAL window"], f
    # one grp read 32 times for a register (grp 3's R5 reads go to grp 7)
    s, f = analyse(synth_run(t_nom + 20, extra_grp=7) +
                   synth_run(t_nom + 50) + synth_run(t_nom + 80),
                   t_nom, t_exit)
    assert len(f) == 1 and "R5 grp 3 x0" in f[0] and \
        "R5 grp 7 x4" in f[0], f
    # two runs only
    s, f = analyse(ok3[:2 * RUN_BLOCKS * 3], t_nom, t_exit)
    assert f == ["2 verification runs, want >= 3"], f
    # --expect-none: empty passes, anything else fails
    s, _ = analyse([], t_nom, t_exit)
    assert check_none(s) == []
    s, _ = analyse(ok3[:3], t_nom, t_exit)        # one SCINITSEQ word
    assert len(check_none(s)) == 1
    # clocks: monotonic event stamps placed on the wall clock
    m0, r0 = 5000.0, t_nom - 100.0          # the run started 100 s early
    assert to_wall(m0 + 100.0, r0, m0) == t_nom
    assert clock_step(r0, m0, r0 + 300.0, m0 + 300.0) is None
    assert clock_step(r0, m0, r0 + 298.8, m0 + 300.0) is None   # 0.4 %
    # a wall clock stepped back: 225 s wall over 300 s monotonic (0.75)
    why = clock_step(r0, m0, r0 + 225.0, m0 + 300.0)
    assert why is not None and "ratio 0.750" in why, why
    assert clock_step(r0, m0, r0 + 301.8, m0 + 300.0) is not None  # 0.6 %
    # the same trace, the window from monotonic stamps: the verdict does
    # not change however the probe's wall HH:MM:SS read
    s, f = analyse(ok3, to_wall(m0 + 100.0, r0, m0),
                   to_wall(m0 + 240.0, r0, m0))
    assert f == [] and s["runs"] == 3, f
    # main(): a step => exit 3 before any verdict; --expect-none is immune
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".trace") as tf:
        tf.write("\n".join(ok3) + "\n")
        tf.flush()
        clk = [str(v) for v in (r0, m0, r0 + 225.0, m0 + 300.0)]
        assert main([tf.name, "--clock", *clk, "--t-nom-m", str(m0 + 100),
                     "--t-exit-m", str(m0 + 240)]) == 3
        clk[2] = str(r0 + 300.0)
        assert main([tf.name, "--clock", *clk, "--t-nom-m", str(m0 + 100),
                     "--t-exit-m", str(m0 + 240)]) == 0
    with tempfile.NamedTemporaryFile("w", suffix=".trace") as tf:
        assert main([tf.name, "--expect-none"]) == 0
    print("IDEFX-TRACE-CHECK SELF-TEST: PASS")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Count ASIC verification runs in an IDeF-X trace")
    ap.add_argument("trace", nargs="?")
    ap.add_argument("--clock", nargs=4, type=float,
                    metavar=("R0", "M0", "R1", "M1"),
                    help="(realtime, monotonic) before and after the run")
    ap.add_argument("--t-nom-m", type=float,
                    help="the probe's monotonic stamp of 0021 04 05")
    ap.add_argument("--t-exit-m", type=float,
                    help="the probe's monotonic stamp of 0021 05 04")
    ap.add_argument("--min-runs", type=int, default=3)
    ap.add_argument("--expect-none", action="store_true",
                    help="require an empty trace (det-idefx=off)")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.trace:
        ap.error("TRACE is required")
    if not args.expect_none and args.clock is None:
        ap.error("--clock R0 M0 R1 M1 is required (the window check)")
    try:
        with open(args.trace) as fh:
            lines = fh.readlines()
    except OSError as e:
        print(f"IDEFX-TRACE-CHECK: ERROR (cannot read {args.trace}: {e})")
        return 2
    t_nom = t_exit = None
    if not args.expect_none:
        r0, m0, r1, m1 = args.clock
        why = clock_step(r0, m0, r1, m1)
        if why is not None:
            print(f"IDEFX-TRACE-CHECK: INCONCLUSIVE ({why})")
            return 3
        if args.t_nom_m is not None:
            t_nom = to_wall(args.t_nom_m, r0, m0)
        if args.t_exit_m is not None:
            t_exit = to_wall(args.t_exit_m, r0, m0)
    summary, fails = analyse(lines, t_nom, t_exit, args.min_runs)
    print(f"ASIC verification runs (delivered read address blocks): "
          f"{summary['runs']}"
          + (": " + "; ".join(summary["desc"]) if summary["desc"] else "")
          + f"; {summary['words']} IDeF-X command words,"
            f" {summary['undelivered']} undelivered read address blocks")
    if args.expect_none:
        fails = check_none(summary)
    for x in fails:
        print(f"  {x}")
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
