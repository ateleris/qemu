#!/usr/bin/env python3
"""Normalize a STIX boot log for golden comparison.

Rules (only these):
  - strip an optional leading CR and the guest timestamp prefix
    (HH:MM:SS.mmm followed by whitespace)
  - collapse 3+ consecutive blank lines to one

Usage: normalize-log.py <in.log> <out.log>
"""
import argparse
import re
import sys

TS = re.compile(r"^\r?(\d{2}):(\d{2}):(\d{2})\.(\d{3})\s+")


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
        if stripped == "":
            blanks += 1
            if blanks >= 3:
                continue
        else:
            blanks = 0
        out.append(stripped)

    with open(args.outfile, "w") as f:
        f.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
