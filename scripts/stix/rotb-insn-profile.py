#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
r"""Guest instructions per function per detector frame (ROTB plan RF-7).

Two rotb-fill-run.sh runs of the same cell that differ only in
--guest-seconds, each with the hotblocks TCG plugin:

  STIX_ROTB_QEMU_EXTRA="-plugin <build>/contrib/plugins/libhotblocks.so,\
limit=0,inline=on -d plugin -D <out>/hotblocks.log"

Boot, CONF and NOMINAL entry are identical in both, so the difference
long - short is the steady NOMINAL window alone.  Per translation block
the plugin gives the start pc, the instructions in it and its exec
count; insns = icount * ecount, attributed to the ELF function whose
[start, start+size) holds the pc (host nm -S).  Frames in the window =
the difference of the stixdet_chunk lines; compacted bytes = the
difference of flash page programs (stixflash_op_start cmd=3) x 4096,
so a partially written last file is the error bar.

Under -icount these are guest instruction counts, not time: the AX's
time per instruction comes from comparing them with GRMON/AX timing of
the same ELF.  Busy-wait loops count too (they are instructions).

  rotb-insn-profile.py SHORT_DIR LONG_DIR --elf ELF [--top N] [--json F]
  rotb-insn-profile.py --self-test
"""

import argparse
import bisect
import json
import os
import re
import subprocess
import sys
import tempfile

PAGE_BYTES = 4096
HB_LINE = re.compile(r"^0x([0-9a-f]+), (\d+), (\d+), (\d+)$")
CHUNK = re.compile(r"stixdet_chunk chunk: ")
PROG = re.compile(r"stixflash_op_start op start cmd=3 ")

# coarse groups by function name (first match wins); the rest is "other"
GROUPS = (
    ("bch", re.compile(r"BCH", re.I)),
    ("rotb_compaction", re.compile(r"RotAccu|Compactor|RotBuf", re.I)),
    ("archive_stream", re.compile(r"Archive|MemStream|Stream", re.I)),
    ("filesystem", re.compile(r"Filesystem|^FS|File_|_page|_block|"
                              r"MemFilesystem|Flash", re.I)),
    ("ql_rcr", re.compile(r"^QL|RateCtrl|FlareDet|FlareLoc|BGMon|"
                          r"LightCurve|Variance|Spectra", re.I)),
    ("libc_mem", re.compile(r"^(memcpy|memset|memmove|memcmp|_?_?mem)")),
    ("rtems", re.compile(r"^(_Thread|_CPU|_ISR|_Scheduler|_Watchdog|"
                         r"_Chain|_Objects|_Event|_Semaphore|_CORE|"
                         r"_TOD|_Timer|_Heap|_Protected|_User|_API|"
                         r"rtems_|_RTEMS|_Rate|_Message|bsp_|"
                         r"_Context)", re.I)),
)


class ProfileError(Exception):
    pass


def group_of(name):
    for g, rx in GROUPS:
        if rx.search(name):
            return g
    return "other"


def read_symbols(elf):
    """[(start, end, name)] sorted, the function symbols with a size."""
    try:
        out = subprocess.run(["nm", "-S", "-n", "--defined-only", elf],
                             capture_output=True, text=True,
                             check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        raise ProfileError("nm %s: %s" % (elf, e))
    syms = []
    for line in out.splitlines():
        f = line.split()
        if len(f) == 4 and f[2] in "tTwW":
            start, size = int(f[0], 16), int(f[1], 16)
            if size:
                syms.append((start, start + size, f[3]))
    syms.sort()
    return syms


def read_hotblocks(path):
    """{pc: insns executed} summed over the TBs starting at pc."""
    out = {}
    n = 0
    with open(path, errors="replace") as fh:
        for line in fh:
            m = HB_LINE.match(line.strip())
            if m:
                pc = int(m.group(1), 16)
                out[pc] = out.get(pc, 0) + int(m.group(3)) * int(m.group(4))
                n += 1
    if not n:
        raise ProfileError("%s: no hotblocks rows (plugin output "
                           "missing?)" % path)
    return out


def count_trace(path):
    chunks = progs = 0
    with open(path, errors="replace") as fh:
        for line in fh:
            if CHUNK.search(line):
                chunks += 1
            elif PROG.search(line):
                progs += 1
    return chunks, progs


def run_files(d):
    hb = os.path.join(d, "hotblocks.log")
    tr = os.path.join(d, "trace.log")
    for p in (hb, tr):
        if not os.path.isfile(p):
            raise ProfileError("%s missing" % p)
    return hb, tr


def attribute(pc_insns, syms):
    starts = [s[0] for s in syms]
    by_fn = {}
    for pc, n in pc_insns.items():
        i = bisect.bisect_right(starts, pc) - 1
        name = (syms[i][2] if i >= 0 and pc < syms[i][1]
                else "?0x%08x" % pc)
        by_fn[name] = by_fn.get(name, 0) + n
    return by_fn


def profile(short_dir, long_dir, elf, top=40):
    syms = read_symbols(elf)
    hb_s, tr_s = run_files(short_dir)
    hb_l, tr_l = run_files(long_dir)
    a, b = read_hotblocks(hb_s), read_hotblocks(hb_l)
    diff = {pc: b.get(pc, 0) - a.get(pc, 0) for pc in set(a) | set(b)}
    neg = sum(-v for v in diff.values() if v < 0)
    by_fn = attribute({pc: v for pc, v in diff.items() if v > 0}, syms)
    (ca, pa), (cb, pb) = count_trace(tr_s), count_trace(tr_l)
    frames, progs = cb - ca, pb - pa
    if frames <= 0:
        raise ProfileError("no frames in the window (chunks %d -> %d)"
                           % (ca, cb))
    total = sum(by_fn.values())
    nbytes = progs * PAGE_BYTES
    groups = {}
    for fn, n in by_fn.items():
        g = group_of(fn)
        groups[g] = groups.get(g, 0) + n
    fns = sorted(by_fn.items(), key=lambda kv: -kv[1])
    return {
        "short": short_dir, "long": long_dir, "elf": elf,
        "frames": frames, "page_programs": progs,
        "compacted_bytes": nbytes,
        "bytes_per_frame": nbytes / frames,
        "insns_total": total,
        "insns_per_frame": total / frames,
        "negative_insns_dropped": neg,
        "groups_per_frame": {g: n / frames for g, n in
                             sorted(groups.items(), key=lambda kv: -kv[1])},
        "top_per_frame": [{"fn": fn, "group": group_of(fn),
                           "insns_per_frame": n / frames,
                           "share": n / total}
                          for fn, n in fns[:top]],
    }


def print_profile(p):
    print("window: %d frames, %d page programs (%.0f B/frame), "
          "%.3g insns/frame" % (p["frames"], p["page_programs"],
                                p["bytes_per_frame"], p["insns_per_frame"]))
    if p["negative_insns_dropped"]:
        print("note: %d insns had long < short (dropped)"
              % p["negative_insns_dropped"])
    print("\n%-18s %14s %7s" % ("group", "insns/frame", "share"))
    for g, n in p["groups_per_frame"].items():
        print("%-18s %14.0f %6.1f%%" % (g, n, 100 * n / p["insns_per_frame"]))
    print("\n%-44s %-16s %14s %7s" % ("function", "group", "insns/frame",
                                      "share"))
    for r in p["top_per_frame"]:
        print("%-44s %-16s %14.0f %6.1f%%" % (r["fn"][:44], r["group"],
                                              r["insns_per_frame"],
                                              100 * r["share"]))


def self_test():
    ok = 0

    def chk(c, what):
        nonlocal ok
        if not c:
            raise AssertionError(what)
        ok += 1
    tmp = tempfile.mkdtemp(prefix="rotb-insn-profile-st-")
    try:
        syms = [(0x1000, 0x1100, "BCH_encode"),
                (0x2000, 0x2400, "RotAccuCompactor_processDetectorBlock"),
                (0x3000, 0x3010, "_Thread_Dispatch")]
        chk(attribute({0x1000: 10, 0x10ff: 5, 0x1100: 7, 0x2010: 3}, syms)
            == {"BCH_encode": 15, "?0x00001100": 7,
                "RotAccuCompactor_processDetectorBlock": 3}, "attribute")
        chk(group_of("BCH_encode") == "bch" and
            group_of("RotAccuCompactor_x") == "rotb_compaction" and
            group_of("memcpy") == "libc_mem" and
            group_of("_Thread_Dispatch") == "rtems" and
            group_of("main") == "other", "groups")
        hb = os.path.join(tmp, "hb.log")
        with open(hb, "w") as fh:
            fh.write("collected 2 entries in the hash table\n"
                     "pc, tcount, icount, ecount\n"
                     "0x0000000000001000, 1, 4, 10\n"
                     "0x0000000000001000, 2, 3, 2\n"
                     "0x0000000000002000, 1, 5, 1\n")
        chk(read_hotblocks(hb) == {0x1000: 46, 0x2000: 5}, "hotblocks")
        tr = os.path.join(tmp, "trace.log")
        with open(tr, "w") as fh:
            fh.write("t stixdet_chunk chunk: nom_bank 0, 49 blocks\n"
                     "t stixflash_op_start op start cmd=3 mcm=0\n"
                     "t stixflash_op_start op start cmd=2 mcm=0\n"
                     "t stixdet_chunk chunk: nom_bank 1, 49 blocks\n")
        chk(count_trace(tr) == (2, 1), "count_trace")
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)
    print("ROTB-INSN-PROFILE SELF-TEST: PASS (%d checks)" % ok)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("short", nargs="?")
    ap.add_argument("long", nargs="?")
    ap.add_argument("--elf")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--json")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not (a.short and a.long and a.elf):
        ap.error("SHORT_DIR LONG_DIR --elf ELF required")
    try:
        p = profile(a.short, a.long, a.elf, a.top)
    except ProfileError as e:
        print("rotb-insn-profile: %s" % e, file=sys.stderr)
        return 2
    print_profile(p)
    if a.json:
        with open(a.json, "w") as fh:
            json.dump(p, fh, indent=1)
            fh.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
