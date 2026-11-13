#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""E-4 replay verdict for elut-smoke run 2 (ELUT plan E-4,
docs/stix-port/2026-11-11-elut-plan.md "no fallback on a real ELUT").
Standard library only.

Run 2 of elut-smoke (positive) boots the seeded flash with
-global grlib-stixctrl.det-replay-file=<run dir>/dssevs.elut2 (a tiny
synthetic .dssevs the gate wrote itself), traces stixdet_elut_fallback,
stixdet_replay_scan and stixdet_replay_state, and sets
det-source=replay-dss by QMP before the NOMINAL entry. This checker
turns the run's artefacts into the E-4 verdict. All rules must hold:

1. scan      the QEMU log has a stixdet_replay_scan "replay pre-scan"
             line naming the run's .dssevs file: the table was decoded
             at realize (det-replay-file is realize-time, it cannot be
             set by QMP);
2. start     the log has a stixdet_replay_state
             "replay state=start (nominal)" line: the replay started at
             the SetEnable 0->1 edge of the NOMINAL entry, and
             stix_det.c classifies all 384 ELUT rows on that same edge,
             just before the start. This is the witness that the rows
             WERE classified while det-source=replay-dss - without it a
             bare "no fallback" grep would pass vacuously (the plan's
             vacuity trap; D-7 has no per-classification summary trace
             yet, so the start line plus rule 5 carry the proof);
3. fallback  the log has ZERO stixdet_elut_fallback "ELUT fallback:"
             lines: no (fpga_det, pix) row was degenerate at any
             classification, so the honoured ELUT branch is in use;
4. fed       (with --count) the log has a stixdet_replay_state
             "state=eof" line whose events_fed equals the number of
             events the gate wrote into the .dssevs file: every
             synthetic event was fed, not only some of them;
5. ssid32    (with --ssid32-dump) the QL spectra dump holds exactly one
             SSID 32 product of 32 samples, subcol 0..31 once each, and
             the replay's counts are exactly where the honoured branch
             puts them: the sample whose detector byte is
             SUBCOL_FROM_FPGA_DET[fpga_det] has a non-zero count byte at
             the ebin the CSV gives adc for (pix, fpga_det) through
             applyELUT's loop; every other sample is all zero; the
             target's byte at the linear-fallback ebin
             (adc - base) * 32 / (4096 - base) is 0 when that ebin
             exists and differs (with the gate's file, adc <= base, so
             the fallback would have dropped the events entirely).

Exit codes: 0 pass, 1 a failed check, 2 bad usage or an unreadable
input. --self-test runs the built-in cases (good, vacuous, a fallback
line, an unfed event, an empty and an absent dump, fallback-binned
counts, a stray count, two dumps).

Usage:
  elut-replay-check.py --qemu-log LOG --dssevs FILE --csv CSV
                       --adc N --fpga-det N --pix N --base N
                       [--count N] [--ssid32-dump DUMP]
  elut-replay-check.py --self-test
"""
import argparse
import importlib.util
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
N_ADC = 4096
N_EBIN = 32


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def tools():
    """(elut-upload, ql-check) modules from this directory."""
    return (load("elut_upload", os.path.join(HERE, "elut-upload.py")),
            load("ql_check", os.path.join(HERE, "ql-check.py")))


def fallback_ebin(adc, base):
    """stix_det.c stix_det_replay_fallback_ebin for one event; None when
    the fallback drops it (adc at or below the file's 0xE baseline)."""
    if adc <= base:
        return None
    return min((adc - base) * N_EBIN // (N_ADC - base), N_EBIN - 1)


def check_log(text, dssevs, count=None):
    """Rules 1-4 on the QEMU log; returns the failure messages."""
    lines = text.splitlines()
    fails = []
    if not any("stixdet_replay_scan" in l and dssevs in l for l in lines):
        fails.append("no stixdet_replay_scan pre-scan line for %s: the"
                     " replay table was never loaded" % dssevs)
    if not any("replay state=start (nominal)" in l for l in lines):
        fails.append('no stixdet_replay_state "state=start (nominal)"'
                     " line: the replay never started at the SetEnable"
                     " edge, so the 384 ELUT rows were never classified"
                     " (det-source=replay-dss missing or the trigger did"
                     " not fire); a no-fallback grep would be vacuous")
    fb = [l.strip() for l in lines if "ELUT fallback:" in l]
    if fb:
        fails.append("%d stixdet_elut_fallback lines, first: %s"
                     % (len(fb), fb[0]))
    if count is not None:
        fed = None
        for l in lines:
            if "stixdet_replay_state" in l and "state=eof" in l:
                m = re.search(r"events_fed=(\d+)", l)
                if m:
                    fed = int(m.group(1))
                    break
        if fed is None:
            fails.append("no stixdet_replay_state state=eof line: the"
                         " replay never reached the end of the file")
        elif fed != count:
            fails.append("the eof trace says events_fed=%d, want %d:"
                         " the .dssevs held more events than the sink"
                         " saw" % (fed, count))
    return fails


def check_ssid32(dump_path, subcol, ecsv, efb):
    """Rule 5 on the SSID 32 dump; returns the failure messages."""
    _, qlc = tools()
    fails = []
    try:
        with open(dump_path) as f:
            pkts = qlc.read_dump(f)
    except OSError as e:
        return ["the SSID 32 dump %s is unreadable: %s" % (dump_path, e)]
    except qlc.DecodeError as e:
        return ["the SSID 32 dump does not decode: %s" % e]
    try:
        qlc.check_contiguous(pkts)
        prods = qlc.reassemble(pkts).get(32, [])
    except qlc.DecodeError as e:
        return ["the SSID 32 dump does not reassemble: %s" % e]
    if len(prods) != 1:
        return ["SSID 32: %d products, want 1 (the one dump of the"
                " NOMINAL-exit flush)" % len(prods)]
    smp = prods[0]["samples"]
    subcols = sorted(s["subcol"] for s in smp)
    if subcols != list(range(N_EBIN)):
        return ["SSID 32: subcol bytes %s..., want 0..31 once each"
                % subcols[:4]]
    tgt = [s for s in smp if s["subcol"] == subcol][0]
    if tgt["ebin"][ecsv] < 1:
        fails.append("SSID 32: subcol %d has no count at the honoured"
                     " ebin %d (byte 0x%02x): the replay events did not"
                     " land where the ELUT puts adc" % (subcol, ecsv,
                                                        tgt["ebin"][ecsv]))
    for s in smp:
        if s["subcol"] == subcol:
            continue
        if any(s["ebin"]):
            nz = [j for j, v in enumerate(s["ebin"]) if v]
            fails.append("SSID 32: subcol %d holds counts outside the"
                         " replay (ebins %s): another source than the"
                         " synthetic replay fed the spectra"
                         % (s["subcol"], nz))
            break
    if efb is not None and efb != ecsv and tgt["ebin"][efb]:
        fails.append("SSID 32: subcol %d holds 0x%02x at the linear"
                     " fallback ebin %d: the ELUT row was treated as"
                     " degenerate" % (subcol, tgt["ebin"][efb], efb))
    other = [j for j, v in enumerate(tgt["ebin"])
             if v and j != ecsv and j != efb]
    if other:
        fails.append("SSID 32: subcol %d holds counts at ebin(s) %s"
                     " besides the honoured one: not the expected"
                     " events at one adc" % (subcol, other))
    return fails


def self_test():
    """The built-in cases; prints ok/FAIL lines, returns the rc."""
    failures = []

    def check(name, cond, detail=""):
        print("%s - %s" % ("ok  " if cond else "FAIL", name))
        if not cond:
            print("      %s" % detail)
            failures.append(name)

    elut, qlc = tools()
    with tempfile.TemporaryDirectory() as d:
        csv = os.path.join(d, "synth.csv")
        with open(csv, "w", newline="") as f:
            f.write(elut.synth_csv_text(elut.synth_rows()))
        rows = elut.parse_csv_file(csv)
        subcol = elut.SUBCOL_FROM_FPGA_DET[0]
        ecsv = elut.apply_row(elut.boundaries(rows[(subcol, 0)]))[2000]
        dssevs = os.path.join(d, "dssevs.elut2")
        scan = ("stixdet_replay_scan replay pre-scan '%s': 5 words,"
                " 4 events (fpga_det = dss_det), duration 0 ns,"
                " scanned in 0 ns" % dssevs)
        start = ("stixdet_replay_state replay state=start (nominal):"
                 " t_tick=0 cursor=0 events_fed=0")
        eof = ("stixdet_replay_state replay state=eof (end of file):"
               " t_tick=1 cursor=4 events_fed=4")
        fbline = ("stixdet_elut_fallback ELUT fallback: fpga_det 0 pix 0"
                  " degenerate row (constant 0), linear binning in use")

        def dump(path, spectra):
            with open(path, "w") as f:
                f.write(qlc.build_packet(qlc.ssid32_body(spectra)).hex()
                        + "\n")

        def zero_spectra():
            return [(s, [0] * N_EBIN) for s in range(N_EBIN)]

        good_log = "\n".join([scan, start, eof]) + "\n"
        log = os.path.join(d, "qemu.log")
        with open(log, "w") as f:
            f.write(good_log)
        d32 = os.path.join(d, "ql32.hex")
        sp = zero_spectra()
        sp[subcol][1][ecsv] = 4
        dump(d32, sp)
        fails = check_log(good_log, dssevs, 4) + \
            check_ssid32(d32, subcol, ecsv, fallback_ebin(2000, 2000))
        check("good log and dump pass", not fails, " | ".join(fails))

        with open(log, "w") as f:
            f.write(scan + "\n")          # vacuous: scan only
        fails = check_log(open(log).read(), dssevs, 4)
        check("vacuous run (no start line) fails", len(fails) == 2 and
              "never classified" in fails[0] and "never reached" in
              fails[1], " | ".join(fails))

        with open(log, "w") as f:
            f.write("\n".join([scan, start, fbline]) + "\n")
        fails = check_log(open(log).read(), dssevs, 4)
        check("a fallback line fails", len(fails) == 2 and
              any("stixdet_elut_fallback" in f for f in fails),
              " | ".join(fails))

        with open(log, "w") as f:
            f.write(start + "\n")
        fails = check_log(open(log).read(), dssevs, 4)
        check("a missing pre-scan fails", len(fails) == 2 and
              "never loaded" in fails[0], " | ".join(fails))

        eof3 = ("stixdet_replay_state replay state=eof (end of file):"
                " t_tick=1 cursor=3 events_fed=3")
        with open(log, "w") as f:
            f.write("\n".join([scan, start, eof3]) + "\n")
        fails = check_log(open(log).read(), dssevs, 4)
        check("an unfed event fails the eof count", len(fails) == 1 and
              "events_fed=3, want 4" in fails[0], " | ".join(fails))

        empty = os.path.join(d, "ql32empty.hex")
        open(empty, "w").close()
        fails = check_ssid32(empty, subcol, ecsv, None)
        check("an empty SSID 32 dump fails", len(fails) == 1 and
              "0 products" in fails[0], " | ".join(fails))
        fails = check_ssid32(os.path.join(d, "ql32absent.hex"), subcol,
                             ecsv, None)
        check("an absent SSID 32 dump fails", len(fails) == 1 and
              "unreadable" in fails[0], " | ".join(fails))

        # counts binned by the fallback (base 0: efb 15) instead of the
        # honoured ebin 31
        efb = fallback_ebin(2000, 0)
        sp = zero_spectra()
        sp[subcol][1][efb] = 4
        d32 = os.path.join(d, "ql32fb.hex")
        dump(d32, sp)
        fails = check_ssid32(d32, subcol, ecsv, efb)
        check("fallback-binned counts fail", len(fails) == 2 and
              any("honoured ebin" in f for f in fails) and
              any("fallback ebin" in f for f in fails),
              " | ".join(fails))

        sp = zero_spectra()
        sp[subcol][1][ecsv] = 4
        sp[7][1][20] = 1
        d32 = os.path.join(d, "ql32stray.hex")
        dump(d32, sp)
        fails = check_ssid32(d32, subcol, ecsv, None)
        check("a stray count in another sample fails",
              len(fails) == 1 and "another source" in fails[0],
              " | ".join(fails))

        d32 = os.path.join(d, "ql32two.hex")
        with open(d32, "w") as f:
            f.write(qlc.build_packet(qlc.ssid32_body(zero_spectra())).hex()
                    + "\n")
            f.write(qlc.build_packet(
                qlc.ssid32_body(zero_spectra()), seq=1).hex() + "\n")
        fails = check_ssid32(d32, subcol, ecsv, None)
        check("two SSID 32 products fail", len(fails) == 1 and
              "want 1" in fails[0], " | ".join(fails))

    if failures:
        print("SELF-TEST: FAIL (%d: %s)" % (len(failures),
                                            ", ".join(failures)))
        return 1
    print("SELF-TEST: PASS")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--qemu-log", help="QEMU's log of run 2 (the traces)")
    ap.add_argument("--dssevs", help="the run's synthetic .dssevs file")
    ap.add_argument("--ssid32-dump", help="the 0x5DC dump of run 2")
    ap.add_argument("--csv", help="the flight ELUT CSV")
    ap.add_argument("--adc", type=int, help="the events' adc (0..4095)")
    ap.add_argument("--fpga-det", type=int, help="the events' fpga_det")
    ap.add_argument("--pix", type=int, help="the events' pix")
    ap.add_argument("--base", type=int, help="the file's 0xE baseline")
    ap.add_argument("--count", type=int,
                    help="the .dssevs event count the gate wrote")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    missing = [k for k, v in (("--qemu-log", args.qemu_log),
                              ("--dssevs", args.dssevs),
                              ("--csv", args.csv)) if not v]
    if missing:
        print("elut-replay-check: %s required (or --self-test)"
              % ", ".join(missing), file=sys.stderr)
        return 2
    for name, v, hi in (("adc", args.adc, N_ADC - 1),
                        ("fpga-det", args.fpga_det, 31),
                        ("pix", args.pix, 11),
                        ("base", args.base, N_ADC - 1)):
        if v is None or not 0 <= v <= hi:
            print("elut-replay-check: --%s must be 0..%d" % (name, hi),
                  file=sys.stderr)
            return 2
    if args.count is not None and args.count < 1:
        print("elut-replay-check: --count must be >= 1", file=sys.stderr)
        return 2
    try:
        with open(args.qemu_log, errors="replace") as f:
            text = f.read()
    except OSError as e:
        print("elut-replay-check: cannot read %s: %s"
              % (args.qemu_log, e), file=sys.stderr)
        return 2
    elut, _ = tools()
    try:
        rows = elut.parse_csv_file(args.csv)
    except (OSError, elut.CsvError) as e:
        print("elut-replay-check: the CSV failed: %s" % e, file=sys.stderr)
        return 2
    subcol = elut.SUBCOL_FROM_FPGA_DET[args.fpga_det]
    ecsv = elut.apply_row(
        elut.boundaries(rows[(subcol, args.pix)]))[args.adc]
    efb = fallback_ebin(args.adc, args.base)

    fails = check_log(text, args.dssevs, args.count)
    if args.ssid32_dump:
        fails += check_ssid32(args.ssid32_dump, subcol, ecsv, efb)
    print("REPLAY-CHECK: honoured ebin %d at subcol %d (adc %d, pix %d,"
          " fpga_det %d, base %d, events %s, fallback %s)"
          % (ecsv, subcol, args.adc, args.pix, args.fpga_det, args.base,
             args.count if args.count is not None else "n/a",
             "drops adc <= base" if efb is None else "ebin %d" % efb))
    for f in fails:
        print("REPLAY-CHECK: FAIL: %s" % f)
    if fails:
        print("REPLAY-CHECK: FAIL (%d check(s))" % len(fails))
        return 1
    print("REPLAY-CHECK: PASS (scan, start (nominal), 0 fallback lines%s%s)"
          % (", eof events_fed=%d" % args.count
             if args.count is not None else "",
             ", SSID 32 at the honoured ebin" if args.ssid32_dump else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
