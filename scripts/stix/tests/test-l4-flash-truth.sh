#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Tests for scripts/stix/l4-flash-truth.py (the collect-flash truth
# reader, L4 plan amendment A).  Host only: no QEMU, no bundle files;
# every case runs against synthetic images the tool's own builder
# writes into a temp dir (flash-damage.py's layout/IO code does the
# storage, inverted by default).
#
# Cases: the tool's --self-test; a built image read through the CLI
# (the FLASH-TRUTH summary, 7 chunk lines, 4 file lines sorted by name
# SCET, --out); --trace with the exact model (archived = model - 1)
# and its failures (model without the dropped first chunk, a SCET
# mismatch, a stixdet_chunk_drop line); --uart with exactly one
# 'Skipped first Det data chunk' and its failures (0 and 2); the loud
# failures (no RAW_COUNTS files, a zeroed chunk size field,
# fileSizeInBytes past the file's pages).
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TOOL="$REPO_ROOT/scripts/stix/l4-flash-truth.py"
FD="$REPO_ROOT/scripts/stix/flash-damage.py"
PASS=0
FAIL=0
TMP="$(mktemp -d /tmp/test-l4-flash-truth.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

report() { # name ok|<failure detail>
    if [ "$2" = "ok" ]; then
        echo "ok   - $1"
        PASS=$((PASS + 1))
    else
        echo "FAIL - $1: $2"
        FAIL=$((FAIL + 1))
    fi
}

# 1. the tool's self-test (builds its own images)
out="$(python3 "$TOOL" --self-test 2>&1)"
rc=$?
[ "$rc" -eq 0 ] && grep -q '^self-test: OK' <<< "$out"
report "tool self-test" \
    "$([ "$rc" -eq 0 ] && grep -q '^self-test: OK' <<< "$out" && echo ok ||
        echo "rc=$rc: $out")"

# the image for the CLI cases (the self-test's builder)
python3 - "$TOOL" "$TMP" <<'EOF'
import importlib.util
import os
import sys

spec = importlib.util.spec_from_file_location("l4ft", sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
m.build_selftest_image(os.path.join(sys.argv[2], "flash.bin"))
EOF
rc=$?
[ "$rc" -eq 0 ]
report "builder: the CLI image written" "$([ "$rc" -eq 0 ] && echo ok ||
    echo "rc=$rc")"
FLASH="$TMP/flash.bin"

# 2. the CLI: summary, chunk/file lines, files sorted by name SCET
out="$(python3 "$TOOL" "$FLASH" 2>&1)"
rc=$?
[ "$rc" -eq 0 ] &&
    grep -q '^FLASH-TRUTH: files=4 chunks=7 rcrmax=3' <<< "$out" &&
    [ "$(grep -c '^chunk ' <<< "$out")" -eq 7 ] &&
    [ "$(grep -c '^file ' <<< "$out")" -eq 4 ] &&
    [ "$(awk '$1 == "file" { print $3 }' <<< "$out" | head -1)" = \
        "1000005.000" ] &&
    [ "$(awk '$1 == "file" { print $3 }' <<< "$out" | tail -1)" = \
        "1000030.000" ] &&
    grep -q '^chunk 1000030.000 3$' <<< "$out" &&
    grep -q '^file 0x02' <<< "$out"
report "CLI: summary and sorted file order" \
    "$([ "$rc" -eq 0 ] && echo ok || echo "rc=$rc: $out")"

# 3. --out writes the truth file
python3 "$TOOL" "$FLASH" --out "$TMP/truth.txt" > /dev/null 2>&1
rc=$?
head -1 "$TMP/truth.txt" 2>/dev/null |
    grep -q '^# l4-flash-truth: 4 RAW_COUNTS files, 7 archived chunks'
report "--out: the truth file written" \
    "$([ "$rc" -eq 0 ] && echo ok || echo "rc=$rc")"

# 4. --trace: the exact model (archived = model - 1) passes
python3 - "$TMP" <<'EOF'
import sys

# the 7 archived chunks + the last model chunk, never archived
ms = [1000005, 1000010, 1000011, 1000012, 1000020, 1000030, 1000031,
      1000040]


def line(c, i):
    return ("2026-11-13T10:00:00.000000Z stixdet_chunk chunk: nom_bank 0, "
            "49 blocks, scet 0x%08x:0x0000, int_ticks=10 eacc_events=1800 "
            "rotb_count=%d at vclock_ns=%d\n" % (c, i, i * 1000000000))


for name, lines in [("trace.good", ms),
                    ("trace.short", ms[:-1]),
                    ("trace.swap", ms[:2] + [1000099] + ms[3:])]:
    with open("%s/%s" % (sys.argv[1], name), "w") as f:
        f.writelines(line(c, i) for i, c in enumerate(lines))
with open(sys.argv[1] + "/trace.drop", "w") as f:
    f.writelines(line(c, i) for i, c in enumerate(ms))
    f.write("2026-11-13T10:00:01.000000Z stixdet_chunk_drop chunk: "
            "dropped\n")
EOF
out="$(python3 "$TOOL" "$FLASH" --trace "$TMP/trace.good" 2>&1)"
rc=$?
[ "$rc" -eq 0 ] && grep -q '^FLASH-TRUTH: files=4 chunks=7 rcrmax=3' <<< "$out"
report "--trace: the exact model passes" \
    "$([ "$rc" -eq 0 ] && echo ok || echo "rc=$rc: $out")"

# 5. --trace failures
python3 "$TOOL" "$FLASH" --trace "$TMP/trace.short" > /dev/null 2>&1
rc=$?
[ "$rc" -eq 1 ]
report "--trace: model without the never-archived last chunk -> 1" \
    "$([ "$rc" -eq 1 ] && echo ok || echo "rc=$rc")"
out="$(python3 "$TOOL" "$FLASH" --trace "$TMP/trace.swap" 2>&1)"
rc=$?
[ "$rc" -eq 1 ] && grep -q 'archived chunk 2 SCET 1000011.000 s != model' <<< "$out"
report "--trace: a SCET mismatch is named" \
    "$([ "$rc" -eq 1 ] && echo ok || echo "rc=$rc: $out")"
out="$(python3 "$TOOL" "$FLASH" --trace "$TMP/trace.drop" 2>&1)"
rc=$?
[ "$rc" -eq 1 ] && grep -q 'stixdet_chunk_drop line(s) in the trace' <<< "$out"
report "--trace: a chunk-drop line -> 1" \
    "$([ "$rc" -eq 1 ] && echo ok || echo "rc=$rc: $out")"

# 6. --uart: exactly one 'Skipped first Det data chunk'
printf '00:00:00.150  Main             init: SAFE mode\n00:01:16.0  ArchiveManager   *E Skipped first Det data chunk\n' \
    > "$TMP/uart.good"
printf '00:00:00.150  Main             init: SAFE mode\n' > "$TMP/uart.none"
{ cat "$TMP/uart.good"; cat "$TMP/uart.good"; } > "$TMP/uart.two"
python3 "$TOOL" "$FLASH" --trace "$TMP/trace.good" --uart "$TMP/uart.good" \
    > /dev/null 2>&1
rc=$?
[ "$rc" -eq 0 ]
report "--uart: one skipped-first line passes" \
    "$([ "$rc" -eq 0 ] && echo ok || echo "rc=$rc")"
python3 "$TOOL" "$FLASH" --trace "$TMP/trace.good" --uart "$TMP/uart.none" \
    > /dev/null 2>&1
rc=$?
[ "$rc" -eq 1 ]
report "--uart: no skipped-first line -> 1" \
    "$([ "$rc" -eq 1 ] && echo ok || echo "rc=$rc")"
out="$(python3 "$TOOL" "$FLASH" --trace "$TMP/trace.good" \
    --uart "$TMP/uart.two" 2>&1)"
rc=$?
[ "$rc" -eq 1 ] && grep -q "found 2" <<< "$out"
report "--uart: two skipped-first lines -> 1, named" \
    "$([ "$rc" -eq 1 ] && echo ok || echo "rc=$rc: $out")"

# 7. no RAW_COUNTS files -> 1
truncate -s "$((8 * 64 * 4224))" "$TMP/empty.bin"
out="$(python3 "$TOOL" "$TMP/empty.bin" 2>&1)"
rc=$?
[ "$rc" -eq 1 ] && grep -q 'no RAW_COUNTS files found' <<< "$out"
report "empty image -> 1, named" \
    "$([ "$rc" -eq 1 ] && echo ok || echo "rc=$rc: $out")"

# 8/9. the corrupted-image failures (fresh images)
python3 - "$TOOL" "$FD" "$TMP" <<'EOF'
import importlib.util
import sys


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


l4ft = load(sys.argv[1], "l4ft")
fd = load(sys.argv[2], "fd")
tmp = sys.argv[3]
# a zeroed chunk size field (block 64, chunk 0 at data offset 4)
l4ft.build_selftest_image(tmp + "/corrupt.bin")
img = fd.FlashImage(tmp + "/corrupt.bin", inverted=True)
img.write_guest(fd.block_page0_offset(64) + 4 + 62, b"\x00\x00")
img.close()
# fileSizeInBytes past the file's pages (block 65)
l4ft.build_selftest_image(tmp + "/big.bin")
img = fd.FlashImage(tmp + "/big.bin", inverted=True)
off = fd.block_page0_offset(65)
page = img.read_guest(off, fd.PAGE_SIZE)
aux = bytearray(page[fd.PAGE_DATA_SIZE:])
aux[fd.AUX_FILE_SIZE:fd.AUX_FILE_SIZE + 4] = (1 << 24).to_bytes(4, "big")
img.write_guest(off, page[:fd.PAGE_DATA_SIZE] + bytes(aux))
img.close()
EOF
rc=$?
[ "$rc" -eq 0 ]
report "corrupt images built" "$([ "$rc" -eq 0 ] && echo ok || echo "rc=$rc")"
out="$(python3 "$TOOL" "$TMP/corrupt.bin" 2>&1)"
rc=$?
[ "$rc" -eq 1 ] && grep -q 'size field 0 is smaller than the 64-B header' <<< "$out"
report "zeroed chunk size -> 1, named" \
    "$([ "$rc" -eq 1 ] && echo ok || echo "rc=$rc: $out")"
out="$(python3 "$TOOL" "$TMP/big.bin" 2>&1)"
rc=$?
[ "$rc" -eq 1 ] && grep -q 'pages hold' <<< "$out"
report "fileSizeInBytes past the pages -> 1, named" \
    "$([ "$rc" -eq 1 ] && echo ok || echo "rc=$rc: $out")"

# 7. the remaining structural failures: a chunk size past the file end,
#    the two redundant lengths disagreeing, a name SCET != first chunk
python3 - "$TOOL" "$TMP" <<'EOF'
import importlib.util
import os
import shutil
import sys

spec = importlib.util.spec_from_file_location("l4ft", sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
fd = m._fd()
src = os.path.join(sys.argv[2], "flash.bin")


def patch(name, rel, raw):
    """Copy the CLI image and overwrite data bytes of block 65's file
    (one 30-datum chunk) at offset rel of its page-0 data."""
    dst = os.path.join(sys.argv[2], name)
    shutil.copyfile(src, dst)
    img = fd.FlashImage(dst, inverted=True)
    try:
        img.write_guest(fd.block_page0_offset(65) + rel, raw)
    finally:
        img.close()


patch("runs.bin", 4 + m.CHUNK_SIZE_OFF, b"\xff\xff")   # chunk 0 size
patch("len.bin", 0, b"\x00\x00\x00\x05")              # data bytes 0..3
patch("name.bin", 4, (1000021).to_bytes(4, "big"))     # chunk 0 coarse
EOF
rc=$?
[ "$rc" -eq 0 ]
report "structural images built" "$([ "$rc" -eq 0 ] && echo ok ||
    echo "rc=$rc")"
for c in "runs.bin|runs .* past fileSizeInBytes|chunk size 0xFFFF" \
         "len.bin|data bytes 0..3 say 5|size word != spare size" \
         "name.bin|file name SCET 1000020.000 s != its first chunk|name SCET != chunk 0"; do
    IFS='|' read -r img pat what <<< "$c"
    out="$(python3 "$TOOL" "$TMP/$img" 2>&1)"
    rc=$?
    [ "$rc" -eq 1 ] && grep -q -- "$pat" <<< "$out"
    report "$what -> 1, named" \
        "$([ "$rc" -eq 1 ] && grep -q -- "$pat" <<< "$out" && echo ok ||
            echo "rc=$rc: $out")"
done

echo "test-l4-flash-truth: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]