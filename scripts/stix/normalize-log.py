#!/usr/bin/env python3
"""Normalize a STIX boot log for golden comparison.

Rules (only these):
  - strip an optional leading CR and the guest timestamp prefix
    (HH:MM:SS.mmm followed by whitespace)
  - on the FSW version banner line ("STIX ASW_v2, ver. ..."), replace the
    bracketed build stamp with a placeholder — it changes with every
    rebuild and carries no behavioral information
  - collapse 3+ consecutive blank lines to one

Usage: normalize-log.py <in.log> <out.log>
"""
import argparse
import re
import sys

TS = re.compile(r"^\r?(\d{2}):(\d{2}):(\d{2})\.(\d{3})\s+")
BANNER = re.compile(r"^(.*STIX ASW.*ver\..*)\[.*\]\s*$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("infile")
    ap.add_argument("outfile")
    args = ap.parse_args()

    with open(args.infile, "r", errors="replace") as f:
        lines = f.read().splitlines()

    out = []
    blanks = 0
    for line in lines:
        stripped = TS.sub("", line)
        stripped = BANNER.sub(r"\1[<build>]", stripped)
        if stripped == "":
            blanks += 1
            if blanks >= 2:  # 3+ consecutive blanks collapse to one
                continue
        else:
            blanks = 0
        out.append(stripped)

    with open(args.outfile, "w") as f:
        f.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
