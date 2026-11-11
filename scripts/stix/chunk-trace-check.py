#!/usr/bin/env python3
"""Check the detector model's ROTB science chunks in a QEMU trace.

detector-smoke (detector plan D-5) runs QEMU with -trace events=FILE for,
among others, these events (hw/sparc/trace-events), so the lines read

  <time> stixdet_scene scene source=S flare=F quiet_cps=Q flare_cps=R
      (per fpga_det) at vclock_ns=V
  <time> stixdet_nominal_swap nominal swap: nom_bank B finished, nom_bank
      A active, int_ticks=T sumcnt=N eacc_events=E at vclock_ns=V
  <time> stixdet_chunk chunk: nom_bank B, 49 blocks, scet 0xC:0xF,
      int_ticks=T eacc_events=E rotb_count=K at vclock_ns=V
  <time> stixdet_chunk_drop chunk dropped (WHY): nom_bank B, int_ticks=T
      rotb_count=K at vclock_ns=V
  <time> stixctrl_rotb_cmd rotb cmd=C rd=.. wr=.. count=.. full=..

Every time this script compares is the guest's virtual clock (vclock_ns,
the QEMU_CLOCK_VIRTUAL stamp the model writes into each line) or a tick
count; the <time> stamps (-msg timestamp=on, wall clock) are never used,
since WSL's timesyncd steps the wall clock mid-run.

An integration of T ticks (100 ms each) ends at V; it ran over
[V - T * 0.1 s, V]. The flare windows are the spans in which the last
stixdet_scene line says flare=1. An integration is quiet when it
overlaps no flare window, a flare integration when it lies inside one.

The positive check (all must hold):
- at least one chunk; every chunk has 49 blocks; no stixdet_chunk_drop
  line (every nominal swap made a chunk: nothing dropped for a full
  ring, a disabled ROTB or TESTMODE);
- every quiet integration took exactly --quiet-ticks ticks (200: the
  FSW's max time 199, ParamMgmtDef.h:382-384; SUMCNT stays below 1800),
  and there are at least --min-quiet of them;
- consecutive chunks are the later one's int_ticks apart in virtual
  time, within CADENCE_TOL_NS (a tenth of a tick: the D-1 tick timer
  re-arms from its callback, so a tick lasts 100 ms plus the callback's
  lateness, about 0.7 us in the TCG runs; a lost or extra tick is 100
  ms off and still fails);
- the FSW drained the ring: GETBLOCK commands (cmd=1) >= 49 x (chunks
  - 1).
--flare adds: at least --min-flare flare integrations, each exactly
--flare-ticks ticks (10: the min time 9, the 1800 events come within a
tick). It is a separate gate check (chunk-flare), so the flare negative
(no flip) fails only it.

--expect-none is the detector-smoke --no-rotb check (det-rotb=off): at
least --min-swaps nominal swaps and no chunk at all; every drop line
says det-rotb-off.

Usage:
  chunk-trace-check.py TRACE [--min-quiet N] [--quiet-ticks T] [--flare
      [--min-flare N] [--flare-ticks T]] [--expect-none [--min-swaps N]]
  chunk-trace-check.py --self-test
Exit codes: 0 the check passed, 1 it failed, 2 the trace is missing or
unreadable (or bad usage). stdlib only.
"""
import argparse
import re
import sys

CHUNK_BLOCKS = 49
TICK_NS = 100_000_000
CADENCE_TOL_NS = TICK_NS // 10

SCENE = re.compile(r"^\S+ stixdet_scene scene source=\S+ flare=(\d) "
                   r".* at vclock_ns=(-?\d+)")
SWAP = re.compile(r"^\S+ stixdet_nominal_swap nominal swap: .*"
                  r"int_ticks=(\d+) sumcnt=(\d+) eacc_events=(\d+) "
                  r"at vclock_ns=(-?\d+)")
CHUNK = re.compile(r"^\S+ stixdet_chunk chunk: nom_bank (\d+), (\d+) "
                   r"blocks, scet 0x([0-9a-f]+):0x([0-9a-f]+), "
                   r"int_ticks=(\d+) eacc_events=(\d+) rotb_count=(\d+) "
                   r"at vclock_ns=(-?\d+)")
DROP = re.compile(r"^\S+ stixdet_chunk_drop chunk dropped \(([^)]+)\): "
                  r"nom_bank (\d+), int_ticks=(\d+) rotb_count=(\d+) "
                  r"at vclock_ns=(-?\d+)")
GETBLOCK = re.compile(r"^\S+ stixctrl_rotb_cmd rotb cmd=1 ")


def parse(lines):
    t = {"flare": [], "swaps": [], "chunks": [], "drops": [], "getblock": 0}
    for line in lines:
        m = SCENE.match(line)
        if m:
            t["flare"].append((int(m.group(2)), m.group(1) == "1"))
            continue
        m = SWAP.match(line)
        if m:
            t["swaps"].append({"ticks": int(m.group(1)),
                               "sumcnt": int(m.group(2)),
                               "vclock": int(m.group(4))})
            continue
        m = CHUNK.match(line)
        if m:
            t["chunks"].append({"blocks": int(m.group(2)),
                                "ticks": int(m.group(5)),
                                "eacc": int(m.group(6)),
                                "vclock": int(m.group(8))})
            continue
        m = DROP.match(line)
        if m:
            t["drops"].append({"why": m.group(1), "ticks": int(m.group(3)),
                               "vclock": int(m.group(5))})
            continue
        if GETBLOCK.match(line):
            t["getblock"] += 1
    return t


def flare_windows(scene):
    """[(on, off)] in vclock ns from the scene lines (off = None: open)"""
    out, on = [], None
    for vclock, flare in scene:
        if flare and on is None:
            on = vclock
        elif not flare and on is not None:
            out.append((on, vclock))
            on = None
    if on is not None:
        out.append((on, None))
    return out


def classify(chunk, windows):
    end = chunk["vclock"]
    start = end - chunk["ticks"] * TICK_NS
    for on, off in windows:
        hi = float("inf") if off is None else off
        if start >= on and end <= hi:
            return "flare"
        if start < hi and end > on:
            return "mixed"
    return "quiet"


def analyse(t, min_quiet=5, quiet_ticks=200):
    fails = []
    chunks = t["chunks"]
    windows = flare_windows(t["flare"])
    kinds = [classify(c, windows) for c in chunks]
    quiet = [c for c, k in zip(chunks, kinds) if k == "quiet"]
    if not chunks:
        fails.append(f"no chunk ({len(t['swaps'])} nominal swaps)")
    bad = [c for c in chunks if c["blocks"] != CHUNK_BLOCKS]
    if bad:
        fails.append(f"{len(bad)} chunks without {CHUNK_BLOCKS} blocks")
    if t["drops"]:
        whys = sorted({d["why"] for d in t["drops"]})
        fails.append(f"{len(t['drops'])} chunks dropped ({', '.join(whys)})")
    if len(t["swaps"]) != len(chunks) + len(t["drops"]):
        fails.append(f"{len(t['swaps'])} nominal swaps but {len(chunks)} "
                     f"chunks and {len(t['drops'])} drops")
    off = [c["ticks"] for c in quiet if c["ticks"] != quiet_ticks]
    if off:
        fails.append(f"quiet integrations of {sorted(set(off))} ticks, "
                     f"want {quiet_ticks}")
    if len(quiet) < min_quiet:
        fails.append(f"{len(quiet)} quiet chunks, want >= {min_quiet}")
    for a, b in zip(chunks, chunks[1:]):
        if abs(b["vclock"] - a["vclock"] - b["ticks"] * TICK_NS) > \
                CADENCE_TOL_NS:
            fails.append(f"chunks at vclock {a['vclock']} and "
                         f"{b['vclock']} are not {b['ticks']} ticks apart")
            break
    want = CHUNK_BLOCKS * max(len(chunks) - 1, 0)
    if t["getblock"] < want:
        fails.append(f"{t['getblock']} GETBLOCK, want >= {want} = "
                     f"49 x (chunks - 1): the FSW did not drain the ring")
    summary = {"chunks": len(chunks), "quiet": len(quiet),
               "flare": kinds.count("flare"), "mixed": kinds.count("mixed"),
               "swaps": len(t["swaps"]), "drops": len(t["drops"]),
               "getblock": t["getblock"], "windows": len(windows),
               "quiet_ticks": sorted({c["ticks"] for c in quiet}),
               "flare_ticks": sorted({c["ticks"] for c, k in
                                      zip(chunks, kinds) if k == "flare"})}
    return summary, fails


def analyse_flare(t, min_flare=40, flare_ticks=10):
    fails = []
    windows = flare_windows(t["flare"])
    flare = [c for c in t["chunks"] if classify(c, windows) == "flare"]
    if not windows:
        fails.append("no flare window (no stixdet_scene flare=1)")
    off = [c["ticks"] for c in flare if c["ticks"] != flare_ticks]
    if off:
        fails.append(f"flare integrations of {sorted(set(off))} ticks, "
                     f"want {flare_ticks}")
    if len(flare) < min_flare:
        fails.append(f"{len(flare)} flare chunks, want >= {min_flare}")
    return fails


def check_none(t, min_swaps=3):
    fails = []
    if t["chunks"]:
        fails.append(f"want no chunk: {len(t['chunks'])} chunks")
    if len(t["swaps"]) < min_swaps:
        fails.append(f"{len(t['swaps'])} nominal swaps, want >= "
                     f"{min_swaps} (the swap stays with det-rotb=off)")
    whys = sorted({d["why"] for d in t["drops"]} - {"det-rotb-off"})
    if whys:
        fails.append(f"drops other than det-rotb-off: {', '.join(whys)}")
    return fails


# --- self-test ---------------------------------------------------------

def scene(v, flare):
    return (f"2026-11-11T10:00:00.000000Z stixdet_scene scene "
            f"source=synthetic flare={int(flare)} quiet_cps=20 "
            f"flare_cps=2000 (per fpga_det) at vclock_ns={v}")


def swap(v, ticks):
    return (f"2026-11-11T10:00:00.000000Z stixdet_nominal_swap nominal "
            f"swap: nom_bank 0 finished, nom_bank 1 active, "
            f"int_ticks={ticks} sumcnt=0 eacc_events=0 at vclock_ns={v}")


def chunk(v, ticks, blocks=CHUNK_BLOCKS):
    return (f"2026-11-11T10:00:00.000000Z stixdet_chunk chunk: nom_bank 0, "
            f"{blocks} blocks, scet 0x00000064:0x0000, int_ticks={ticks} "
            f"eacc_events=0 rotb_count=49 at vclock_ns={v}")


def drop(v, ticks, why):
    return (f"2026-11-11T10:00:00.000000Z stixdet_chunk_drop chunk dropped "
            f"({why}): nom_bank 0, int_ticks={ticks} rotb_count=0 at "
            f"vclock_ns={v}")


def getblocks(n):
    return ["2026-11-11T10:00:00.000000Z stixctrl_rotb_cmd rotb cmd=1 "
            "rd=1 wr=49 count=48 full=0"] * n


def run(spec, t0=100 * 10**9, flare_on=None, flare_off=None, rotb=True,
        drain=True, blocks=CHUNK_BLOCKS):
    """A trace from @spec, a list of integration lengths in ticks; the
    flare switch at vclock @flare_on / @flare_off (ns)."""
    out = [scene(0, False)]
    v = t0
    events = []
    for ticks in spec:
        v += ticks * TICK_NS
        events.append((v, ticks))
    marks = []
    if flare_on is not None:
        marks.append((flare_on, True))
    if flare_off is not None:
        marks.append((flare_off, False))
    lines = []
    for v, ticks in events:
        lines.append((v, 1, swap(v, ticks)))
        if rotb:
            lines.append((v, 2, chunk(v, ticks, blocks)))
        else:
            lines.append((v, 2, drop(v, ticks, "det-rotb-off")))
    for v, flare in marks:
        lines.append((v, 0, scene(v, flare)))
    for _, _, x in sorted(lines):
        out.append(x)
        if drain and x.startswith("2026") and " stixdet_chunk " in x:
            out += getblocks(CHUNK_BLOCKS)
    return out


def self_test():
    s = 10**9
    quiet7 = [200] * 7
    # zero sky: 7 quiet chunks of 200 ticks, drained
    sm, f = analyse(parse(run(quiet7)))
    assert f == [] and sm["chunks"] == 7 and sm["quiet"] == 7, (sm, f)
    # a min-time quiet integration (wrong parameters) fails
    _, f = analyse(parse(run([200, 200, 10, 200, 200, 200])))
    assert f == ["quiet integrations of [10] ticks, want 200"], f
    # too few
    _, f = analyse(parse(run([200] * 4)))
    assert f == ["4 quiet chunks, want >= 5"], f
    # a dropped chunk, a short chunk
    tr = run(quiet7)
    tr.insert(5, drop(999 * s, 200, "ring-full"))
    _, f = analyse(parse(tr))
    assert any("dropped (ring-full)" in x for x in f), f
    _, f = analyse(parse(run(quiet7, blocks=48)))
    assert f == ["7 chunks without 49 blocks"], f
    # not drained
    _, f = analyse(parse(run(quiet7, drain=False)))
    assert len(f) == 1 and "0 GETBLOCK, want >= 294" in f[0], f
    # a swap without a chunk line
    tr = run(quiet7)
    tr.insert(3, swap(55 * s, 200))
    _, f = analyse(parse(tr))
    assert any("8 nominal swaps but 7 chunks" in x for x in f), f
    # chunks not at the integration cadence
    tr = [x.replace("vclock_ns=160000000000", "vclock_ns=160100000000")
          for x in run(quiet7)]
    _, f = analyse(parse(tr))
    assert any("not 200 ticks apart" in x for x in f), f
    # a late timer callback (141 us over 200 ticks, as seen live) is fine
    tr = [x.replace("vclock_ns=160000000000", "vclock_ns=160000141553")
          for x in run(quiet7)]
    _, f = analyse(parse(tr))
    assert not f, f
    # the flare: 2 quiet, a mixed one, 54 flare of 10 ticks, one mixed,
    # then a quiet one after the switch-off
    spec = [200, 200, 200] + [10] * 55 + [200]
    on = 100 * s + 45 * s
    off = 100 * s + 60 * s + 55 * s - 5 * TICK_NS
    t = parse(run(spec, flare_on=on, flare_off=off))
    sm, f = analyse(t, min_quiet=2)
    assert f == [] and sm["quiet"] == 3 and sm["flare"] == 54 and \
        sm["mixed"] == 2, (sm, f)
    assert analyse_flare(t) == []
    # the flare negative: no flip, so every chunk is quiet; no flare chunk
    t = parse(run(quiet7))
    assert analyse(t, min_quiet=2)[1] == []
    assert analyse_flare(t) == ["no flare window (no stixdet_scene "
                                "flare=1)", "0 flare chunks, want >= 40"]
    # a flare integration that hit the max time fails chunk-flare
    spec = [200, 200, 200] + [10] * 20 + [200] + [10] * 30 + [200]
    t = parse(run(spec, flare_on=on, flare_off=100 * s + 60 * s + 72 * s))
    assert any("flare integrations of [200]" in x for x in analyse_flare(t))
    # --expect-none (det-rotb=off)
    t = parse(run(quiet7, rotb=False))
    assert check_none(t) == []
    assert analyse(t)[1] != []
    assert check_none(parse(run(quiet7))) == ["want no chunk: 7 chunks"]
    assert check_none(parse(run([200, 200], rotb=False))) == [
        "2 nominal swaps, want >= 3 (the swap stays with det-rotb=off)"]
    print("CHUNK-TRACE-CHECK SELF-TEST: PASS")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Check the ROTB science chunks in a detector trace")
    ap.add_argument("trace", nargs="?")
    ap.add_argument("--min-quiet", type=int, default=5)
    ap.add_argument("--quiet-ticks", type=int, default=200)
    ap.add_argument("--flare", action="store_true",
                    help="check the flare integrations (chunk-flare)")
    ap.add_argument("--min-flare", type=int, default=40)
    ap.add_argument("--flare-ticks", type=int, default=10)
    ap.add_argument("--expect-none", action="store_true",
                    help="require swaps but no chunk (det-rotb=off)")
    ap.add_argument("--min-swaps", type=int, default=3)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.trace:
        ap.error("TRACE is required")
    try:
        with open(args.trace) as fh:
            t = parse(fh)
    except OSError as e:
        print(f"CHUNK-TRACE-CHECK: ERROR (cannot read {args.trace}: {e})")
        return 2
    sm, fails = analyse(t, args.min_quiet, args.quiet_ticks)
    print(f"ROTB chunks: {sm['chunks']} ({sm['quiet']} quiet of ticks "
          f"{sm['quiet_ticks']}, {sm['flare']} flare of ticks "
          f"{sm['flare_ticks']}, {sm['mixed']} across a switch) from "
          f"{sm['swaps']} nominal swaps, {sm['drops']} dropped, "
          f"{sm['getblock']} GETBLOCK, {sm['windows']} flare window(s)")
    if args.expect_none:
        fails = check_none(t, args.min_swaps)
    elif args.flare:
        fails = analyse_flare(t, args.min_flare, args.flare_ticks)
    for x in fails:
        print(f"  {x}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
