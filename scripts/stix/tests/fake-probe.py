# SPDX-License-Identifier: GPL-2.0-or-later
"""Fake tmtc-probe.py for the gate-script tests (STIX_TMTC_PROBE).

Appends its argv (one per line, then "end") to FAKE_PROBE_ARGV_FILE when
that is set, prints "# connected", then one chunk line in the probe's
format ("HH:MM:SS +NNNNB <hex>", the header byte 0x49) every 0.5 s.
It exits 0 after FAKE_PROBE_SECONDS (default 2) and prints the probe's
"# total" summary on exit or on SIGTERM.

FAKE_PROBE_LINES names a file whose lines are printed right after the
connect line, "%T" replaced by the current HH:MM:SS (canned EXPECT OK
lines). FAKE_PROBE_DUMP names a file copied to the path of the probe's
--dump-apid APID:PATH argument (a canned packet dump).

FAKE_PMEMSAVE_CSV, when set, makes every --qmp-cmd pmemsave write its
target file: a *-pre* dump of the requested size filled with zeros,
anything else the correct FPGA ELUT table built from that CSV with
elut-upload.py (the check-dump oracle's own bytes).
"""
import importlib.util
import json
import os
import signal
import sys
import time

argv_file = os.environ.get("FAKE_PROBE_ARGV_FILE")
if argv_file:
    with open(argv_file, "a") as f:
        f.write("".join(a + "\n" for a in sys.argv[1:]) + "end\n")


def done(*_):
    print("# total 37 bytes", flush=True)
    os._exit(0)


signal.signal(signal.SIGTERM, done)
print("# connected (fake probe)", flush=True)
lines_file = os.environ.get("FAKE_PROBE_LINES")
if lines_file:
    with open(lines_file) as f:
        for line in f:
            print(line.rstrip("\n").replace("%T", time.strftime("%H:%M:%S")),
                  flush=True)
dump_file = os.environ.get("FAKE_PROBE_DUMP")
if dump_file and "--dump-apid" in sys.argv:
    dump_path = sys.argv[sys.argv.index("--dump-apid") + 1].split(":", 1)[1]
    with open(dump_file) as src, open(dump_path, "a") as dst:
        dst.write(src.read())
pm_csv = os.environ.get("FAKE_PMEMSAVE_CSV")
if pm_csv:
    spec = importlib.util.spec_from_file_location(
        "elut_upload",
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                     "elut-upload.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    rows = m.parse_csv_file(pm_csv)
    table = m.fpga_elut(rows, m.SUBCOL_FROM_FPGA_DET)
    for i, a in enumerate(sys.argv[:-1]):
        if a != "--qmp-cmd":
            continue
        try:
            cmd = json.loads(sys.argv[i + 1])
        except ValueError:
            continue
        if cmd.get("execute") != "pmemsave":
            continue
        fn = cmd["arguments"]["filename"]
        size = int(cmd["arguments"]["size"])
        data = table if "-pre" not in os.path.basename(fn) else bytes(size)
        with open(fn, "wb") as dst:
            dst.write(data[:size])
end = time.monotonic() + float(os.environ.get("FAKE_PROBE_SECONDS", "2"))
while time.monotonic() < end:
    print(f"{time.strftime('%H:%M:%S')} +{37:4d}B 49" + "ab" * 36,
          flush=True)
    time.sleep(0.5)
done()
