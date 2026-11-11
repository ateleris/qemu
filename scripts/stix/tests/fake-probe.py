# SPDX-License-Identifier: GPL-2.0-or-later
"""Fake tmtc-probe.py for the gate-script tests (STIX_TMTC_PROBE).

Appends its argv (one per line, then "end") to FAKE_PROBE_ARGV_FILE when
that is set, prints "# connected", then one chunk line in the probe's
format ("HH:MM:SS +NNNNB <hex>", the header byte 0x49) every 0.5 s.
It exits 0 after FAKE_PROBE_SECONDS (default 2) and prints the probe's
"# total" summary on exit or on SIGTERM.
"""
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
end = time.monotonic() + float(os.environ.get("FAKE_PROBE_SECONDS", "2"))
while time.monotonic() < end:
    print(f"{time.strftime('%H:%M:%S')} +{37:4d}B 49" + "ab" * 36,
          flush=True)
    time.sleep(0.5)
done()
