#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read-only reader of the RAW_COUNTS archive in a stixflash image
(L4 plan amendment A, commit 68e6cfa27d): builds the #1061 replay truth
file from the collect run's flash - the same archive the request runs
will read - never from TM, and not from the UART (the rcr / Flush DET
stream prints are TRACE calls, compiled out in these builds).

It imports the layout and IO code of scripts/stix/flash-damage.py as a
module (geometry, spare layout, addressing, BCH, FlashImage); nothing
is copied, nothing is written.

Steps (the FSW's own view of the image):
  1. every FS-scanned block's page 0 spare (File.c:328, :342, :387;
     initial control blocks MemFilesystem.c:495-500) names at most one
     file: in use, not bad-block-marked, type RAW_COUNTS (enum value 2,
     FilesystemDefinition.h:81-92); the 10-byte name = type, spare,
     coarse u32 BE, fine u16 BE, duration u16 BE
     (FilesystemDefinition.h:64-73);
  2. a file's pages are read sequentially from page 0 while their
     spares say in use (File.c:621-651; the spare pageAddress is a
     redundant check, File.c:691-699, not re-verified here);
  3. the page bodies are concatenated and trimmed to the spare's
     fileSizeInBytes (File.c:490; contiguous, last page zero-padded,
     File.c:478-484);
  4. the chunks are walked from data offset 4 (the 4 pre-chunk bytes
     are the FSW's own valid-length cross-check, ArchiveManager.c:47-48):
     per chunk the 64-B header (sci_utils.h:8-14) gives the timestamp
     coarse u32 BE + fine u16 BE, duration u16 BE, detector mask u32 BE,
     RCR u16 BE, 16x3-B triggers, and the size u16 BE at 62 counts the
     whole chunk incl. its header (RotAccuCompactor.c:169-173); the
     compacted datums follow from 64.  The FSW overwrites each stored
     timestamp with the previous chunk's end (ArchiveManager.c:133,168,
     182-185), so the header IS the archived chunk's SCET; coarse is
     masked 0x7FFFFFFF (removeTimeSyncBit) and ms = coarse*1000 +
     fine*1000//65536 (Timing_ToMs truncation, Timing.c:395-399);
  5. the truth file is emitted in l4-decode.py's format, files sorted
     by their name SCET (a file's name SCET is its first chunk's
     timestamp, ArchiveManager.c:158-159).

--trace <collect.trace> runs the exact cross-check of the amendment:
archived chunk k's ms == the k-th stixdet_chunk trace line's
"scet 0xC:0xF" ms, and archived = model - 1. The FSW drops the first
chunk of each flush epoch (ArchiveManager.c:138-144) and stores every
chunk with the PREVIOUS chunk's end (:133,168,182-185); the model's
chunk k carries chunk k's integration end, so the k-th archived record
holds trace line k's SCET and the last model SCET is never archived
(verified on a live 600 s collect: 604 of 604 equal at k, 0 at k+1).
No stixdet_chunk_drop line may appear.  --uart <uart.collect.txt> asserts exactly one
"Skipped first Det data chunk" line (the one chunk-path TRACE_ERROR
that survives in these builds).  Any violation exits non-zero with the
reason on stderr.

--self-test builds a small synthetic image in a temp dir with the
imported writer/layout helpers (files of varying chunk size, a padded
last page, a foreign type, a bad-block-marked file) and exercises the
reader, the summary and the failure paths.

Python 3 stdlib only.
"""

import argparse
import os
import re
import shutil
import sys
import tempfile
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
FD_PATH = os.path.join(_HERE, "flash-damage.py")

_FD = None


def _fd():
    """flash-damage.py imported once as a module."""
    global _FD
    if _FD is None:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "stix_flash_damage", FD_PATH)
        _FD = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(_FD)
    return _FD


CHUNK_HDR = 64                    # sci_utils.h:8-14
CHUNK_SIZE_OFF = 62               # size u16 BE, whole chunk incl. header
FILE_TYPE_RAW_COUNTS = 2          # MemFilesystem_filetype_e (h:81-92)
WALK_START = 4                    # ArchiveManager.c:47-48

TRACE_CHUNK = re.compile(r"^\S+ stixdet_chunk chunk: nom_bank \d+, "
                         r"\d+ blocks, scet 0x([0-9a-f]+):0x([0-9a-f]+), ")


class TruthError(Exception):
    """A violation: loud exit 1 with this reason."""


class _Args:
    """The attrs fd.open_image needs."""

    def __init__(self, flash, plain, allow_shared):
        self.flash = flash
        self.plain = plain
        self.allow_shared = allow_shared


def scet_ms(coarse, fine):
    """ms from a SCET coarse/fine pair, the sync bit (bit 31) stripped
    on both sides (removeTimeSyncBit; Timing_ToMs, Timing.c:395-399)."""
    return (coarse & 0x7FFFFFFF) * 1000 + fine * 1000 // 0x10000


# ------------------------------------------------------------- the reader

def _file_data(img, fd, lin_block, size):
    """A file's data: the in-use pages' 4096-B bodies concatenated and
    trimmed to the spare's fileSizeInBytes (File.c:478-490)."""
    out = bytearray()
    for p in range(fd.PAGES_PER_BLOCK):
        off = fd.page_offset(lin_block, p)
        if off >= img.size:
            break
        page = img.read_guest(off, fd.PAGE_SIZE)
        if page[fd.PAGE_DATA_SIZE + fd.AUX_EMPTY_PAGE_MARKER] != fd.EPM_IN_USE:
            break
        out += page[:fd.PAGE_DATA_SIZE]
        if len(out) >= size:
            break
    if len(out) < size:
        raise TruthError(
            "block %d: its pages hold %d data bytes but the spare says "
            "fileSizeInBytes %d" % (lin_block, len(out), size))
    return bytes(out[:size])


def _walk_chunks(data, chunks, lin_block):
    """Append (ms, rcr) per chunk; walk from data offset 4
    (ArchiveManager.c:47-48), each size u16 BE at 62 counting the whole
    chunk incl. its 64-B header (RotAccuCompactor.c:169-173).  Returns
    the chunk count."""
    off = WALK_START
    n = 0
    while off < len(data):
        if off + CHUNK_HDR > len(data):
            raise TruthError(
                "block %d: %d trailing byte(s) after chunk %d (offset %d), "
                "not a whole 64-B header"
                % (lin_block, len(data) - off, n, off))
        size = int.from_bytes(
            data[off + CHUNK_SIZE_OFF:off + CHUNK_SIZE_OFF + 2], "big")
        if size < CHUNK_HDR:
            raise TruthError(
                "block %d chunk %d at offset %d: size field %d is smaller "
                "than the 64-B header" % (lin_block, n, off, size))
        if off + size > len(data):
            raise TruthError(
                "block %d chunk %d at offset %d: size %d runs %d byte(s) "
                "past fileSizeInBytes"
                % (lin_block, n, off, size, off + size - len(data)))
        coarse = int.from_bytes(data[off:off + 4], "big")
        fine = int.from_bytes(data[off + 4:off + 6], "big")
        rcr = int.from_bytes(data[off + 12:off + 14], "big")
        chunks.append((scet_ms(coarse, fine), rcr))
        off += size
        n += 1
    return n


def read_flash(path, plain=False, allow_shared=False):
    """-> (chunks, files): chunks = [(ms, rcr)] in archive order;
    files = [(name hex, first-chunk ms, nchunks)] sorted by name SCET."""
    fd = _fd()
    img = fd.open_image(_Args(path, plain, allow_shared))
    try:
        found = []
        for b in fd.iter_fs_blocks():
            off = fd.block_page0_offset(b)
            if off >= img.size:
                continue                       # past EOF: erased
            aux = img.read_guest(off + fd.PAGE_DATA_SIZE, fd.PAGE_HEADER_SIZE)
            if aux[fd.AUX_EMPTY_PAGE_MARKER] != fd.EPM_IN_USE:
                continue                       # no file catalogued here
            if aux[fd.AUX_BAD_BLOCK_MARKER] != 0xFF:
                continue                       # the boot scan marks it BAD
            if aux[fd.AUX_FILE_NAME] != FILE_TYPE_RAW_COUNTS:
                continue
            name = bytes(aux[fd.AUX_FILE_NAME:fd.AUX_FILE_NAME + fd.FN_LENGTH])
            size = int.from_bytes(aux[fd.AUX_FILE_SIZE:fd.AUX_FILE_SIZE + 4],
                                  "big")
            found.append((b, name, size))
        # the name carries the first chunk's SCET (ArchiveManager.c:158-159)
        found.sort(key=lambda t: scet_ms(int.from_bytes(t[1][2:6], "big"),
                                         int.from_bytes(t[1][6:8], "big")))
        chunks = []
        files = []
        for b, name, size in found:
            data = _file_data(img, fd, b, size)
            # the two redundant lengths agree (ArchiveManager.c:47-48)
            if int.from_bytes(data[:4], "big") != size:
                raise TruthError(
                    "block %d: data bytes 0..3 say %d, the spare's "
                    "fileSizeInBytes says %d"
                    % (b, int.from_bytes(data[:4], "big"), size))
            first = len(chunks)
            n = _walk_chunks(data, chunks, b)
            name_ms = scet_ms(int.from_bytes(name[2:6], "big"),
                              int.from_bytes(name[6:8], "big"))
            # the name carries the first stored chunk's SCET
            # (ArchiveManager.c:158-159 vs :182-183)
            if n and chunks[first][0] != name_ms:
                raise TruthError(
                    "block %d: file name SCET %d.%03d s != its first "
                    "chunk's %d.%03d s" % (b, name_ms // 1000,
                                           name_ms % 1000,
                                           chunks[first][0] // 1000,
                                           chunks[first][0] % 1000))
            files.append((fd.name_hex(name), name_ms, n))
        return chunks, files
    finally:
        img.close()


def render_truth(chunks, files, src):
    """The truth text in l4-decode.py's format (SCETs ms-exact)."""
    lines = ["# l4-flash-truth: %d RAW_COUNTS files, %d archived chunks "
             "read from %s" % (len(files), len(chunks), src),
             "# chunk '<SCET s.mmm> <rcr>' in archive order; file "
             "'<name> <first-chunk SCET s.mmm> <chunks>', files sorted "
             "by name SCET",
             "# SCETs ms-exact: coarse masked 0x7FFFFFFF, "
             "fine*1000//65536 (Timing_ToMs, Timing.c:395-399)"]
    for ms, rcr in chunks:
        lines.append("chunk %d.%03d %d" % (ms // 1000, ms % 1000, rcr))
    for name, ms, n in files:
        lines.append("file %s %d.%03d %d" % (name, ms // 1000, ms % 1000, n))
    return "\n".join(lines) + "\n"


# ----------------------------------------------------- the cross-checks

def read_trace_model(path):
    """The model chunk SCETs (ms) from the QEMU stixdet_chunk trace, in
    collection order, plus the stixdet_chunk_drop count."""
    model = []
    drops = 0
    with open(path, errors="replace") as f:
        for line in f:
            m = TRACE_CHUNK.match(line)
            if m:
                model.append(scet_ms(int(m.group(1), 16),
                                     int(m.group(2), 16)))
            elif " stixdet_chunk_drop " in line:
                drops += 1
    return model, drops


def check_trace(chunks, model, drops):
    """archived == model - 1 and archived[k].ms == model[k].ms
    (ArchiveManager.c:133,168,182-185 store each chunk with the previous
    chunk's end, which is model chunk k's SCET; the last model SCET is
    never archived)."""
    errs = []
    if drops:
        errs.append("%d stixdet_chunk_drop line(s) in the trace (want 0)"
                    % drops)
    if not model:
        errs.append("no stixdet_chunk lines in the trace")
    if not chunks:
        errs.append("no archived chunks read from the flash")
    if len(model) != len(chunks) + 1:
        errs.append("%d model chunks vs %d archived (want model - 1: the "
                    "first chunk is dropped at NOMINAL entry, "
                    "ArchiveManager.c:138-144)" % (len(model), len(chunks)))
    else:
        for k, (ms, _rcr) in enumerate(chunks):
            if ms != model[k]:
                errs.append(
                    "archived chunk %d SCET %d.%03d s != model chunk %d "
                    "%d.%03d s (the stored timestamp is the previous "
                    "chunk's end, ArchiveManager.c:133,168,182-185)"
                    % (k, ms // 1000, ms % 1000, k,
                       model[k] // 1000, model[k] % 1000))
                break
    return errs


def check_uart(path):
    """Exactly one 'Skipped first Det data chunk'
    (ArchiveManager.c:138-144)."""
    n = 0
    with open(path, errors="replace") as f:
        for line in f:
            if "Skipped first Det data chunk" in line:
                n += 1
    if n != 1:
        raise TruthError(
            "found %d 'Skipped first Det data chunk' line(s) in %s (want "
            "exactly 1, ArchiveManager.c:138-144)" % (n, path))


# ------------------------------------------------- the synthetic builder

def chunk_bytes(coarse, fine, rcr, datums):
    """One 64-B chunk header (sci_utils.h:8-14) + the compacted datums;
    the size field at 62 counts the whole chunk
    (RotAccuCompactor.c:169-173)."""
    if not 0 <= coarse <= 0xFFFFFFFF or not 0 <= fine <= 0xFFFF:
        raise TruthError("chunk timestamp field out of range")
    if not 0 <= rcr <= 0xFFFF or len(datums) > 0xFFFF - CHUNK_HDR:
        raise TruthError("chunk rcr/datums out of range")
    hdr = bytearray(CHUNK_HDR)
    hdr[0:4] = coarse.to_bytes(4, "big")
    hdr[4:6] = fine.to_bytes(2, "big")
    hdr[6:8] = (1).to_bytes(2, "big")            # duration 0.1 s
    hdr[8:12] = (0xFFFFFFFF).to_bytes(4, "big")  # detector mask
    hdr[12:14] = rcr.to_bytes(2, "big")
    hdr[CHUNK_SIZE_OFF:CHUNK_SIZE_OFF + 2] = \
        (CHUNK_HDR + len(datums)).to_bytes(2, "big")
    return bytes(hdr) + bytes(datums)


def write_file(img, fd, lin_block, name, data, write_time=1):
    """One stored file on one block: a page per 4096 B, aux per
    File_write's write1 (File.c:472-525), the ECC of all 8 sub-blocks
    computed with the aux ECC area zeroed (File.c:495, 504-518)."""
    npages = max(1, -(-len(data) // fd.PAGE_DATA_SIZE))
    if npages > fd.PAGES_PER_BLOCK:
        raise TruthError("file of %d bytes does not fit one block"
                         % len(data))
    for p in range(npages):
        body = data[p * fd.PAGE_DATA_SIZE:(p + 1) * fd.PAGE_DATA_SIZE]
        page = bytearray(fd.PAGE_SIZE)   # the tail stays 0x00 (File.c:478-484)
        page[:len(body)] = body
        aux = bytearray(fd.PAGE_HEADER_SIZE)
        aux[fd.AUX_BAD_BLOCK_MARKER] = 0xFF
        aux[fd.AUX_EMPTY_PAGE_MARKER] = fd.EPM_IN_USE
        aux[fd.AUX_FILE_NAME:fd.AUX_FILE_NAME + fd.FN_LENGTH] = name
        lin = fd.linear_page0(lin_block) + p
        aux[fd.AUX_PAGE_ADDRESS:fd.AUX_PAGE_ADDRESS + 4] = \
            lin.to_bytes(4, "big")
        aux[fd.AUX_WRITE_TIME:fd.AUX_WRITE_TIME + 8] = \
            write_time.to_bytes(8, "big")
        aux[fd.AUX_FILE_SIZE:fd.AUX_FILE_SIZE + 4] = \
            len(data).to_bytes(4, "big")
        aux[fd.AUX_FILLER:fd.AUX_FILLER + 4] = b"\xFF" * 4
        page[fd.PAGE_DATA_SIZE:] = aux
        for sub in range(fd.FILE_ECC_SUBBLOCKS):
            buf = fd.subblock_with_ecc_zeroed(bytes(page), sub)
            o = fd.PAGE_DATA_SIZE + fd.AUX_ECC + sub * fd.FILE_ECC_BCH_SIZE
            page[o:o + fd.FILE_ECC_BCH_SIZE] = fd.bch_encode(buf)
        img.write_guest(fd.page_offset(lin_block, p), bytes(page))


def write_raw_counts(img, fd, lin_block, coarse0, fine0, chunks,
                     write_time=1):
    """A RAW_COUNTS file from (coarse, fine, rcr, datums) tuples: the 4
    pre-chunk bytes hold the file size, u32 BE (FlushStream,
    ArchiveManager.c:47-48) and the name carries the first chunk's SCET
    (ArchiveManager.c:158-159; the name duration is informational
    here)."""
    body = b"".join(chunk_bytes(c, f, r, d) for c, f, r, d in chunks)
    data = (4 + len(body)).to_bytes(4, "big") + body
    name = fd.build_name(fd.FILE_TYPES["raw-counts"], 0, coarse0, fine0,
                         len(chunks))
    write_file(img, fd, lin_block, name, data, write_time)
    return name


def build_selftest_image(path, inverted=True):
    """The self-test's image: four RAW_COUNTS files (varying chunk
    sizes incl. a zero-datum chunk, one multi-page file with a padded
    last page, one non-zero rcr tag), a foreign-type file, a
    bad-block-marked RAW_COUNTS file and an erased block.  Returns the
    file list as the reader must see it (name hex, ms, nchunks)."""
    fd = _fd()
    with open(path, "wb") as f:
        f.truncate((fd.FS_A[0] + 16) * fd.PAGES_PER_BLOCK * fd.PAGE_SIZE)
    img = fd.FlashImage(path, inverted=inverted)
    try:
        def rc(block, coarse0, fine0, chs):
            name = write_raw_counts(img, fd, block, coarse0, fine0, chs)
            return (fd.name_hex(name), scet_ms(coarse0, fine0), len(chs))
        out = [rc(67, 1000005, 0, [(1000005, 0, 0, b"\x66" * 10)]),
               rc(64, 1000010, 0, [(1000010, 0, 0, b"\x11" * 8),
                                   (1000011, 0, 0, b""),
                                   (1000012, 0, 0, b"\x22" * 5)]),
               rc(65, 1000020, 0, [(1000020, 0, 0, b"\x33" * 30)]),
               # 4339 B of data: two pages, the last padded
               rc(66, 1000030, 0, [(1000030, 0, 3, b"\x44" * 4200),
                                   (1000031, 0, 0, b"\x55" * 7)])]
        write_file(img, fd, 68,
                   fd.build_name(fd.FILE_TYPES["tmmgmt"], 0, 9, 0, 1),
                   b"\x00" * 8)          # a foreign type: skipped
        rc(69, 1000040, 0, [(1000040, 0, 0, b"")])
        img.write_guest(fd.block_page0_offset(69) + fd.PAGE_DATA_SIZE
                        + fd.AUX_BAD_BLOCK_MARKER, b"\x00")
        return out                        # block 70 stays erased
    finally:
        img.close()


def synthetic_collect_flash(path, nmodel=26, coarse0=1000000):
    """The fake collect run's flash (tests/fake-qemu.sh): nmodel model
    chunks at 1 s steps, the first dropped by the FSW, the rest
    archived as a 1-chunk file then 2-chunk files (13 files / 25 chunks
    for nmodel 26).  Returns (nfiles, nchunks)."""
    fd = _fd()
    img = fd.FlashImage(path, inverted=True)
    try:
        archived = [(coarse0 + k, 0, 0, bytes(8)) for k in range(1, nmodel)]
        files = 0
        i = 0
        while i < len(archived):
            take = 1 if files == 0 else 2
            grp = archived[i:i + take]
            write_raw_counts(img, fd, fd.FS_A[0] + files,
                             grp[0][0], grp[0][1], grp)
            files += 1
            i += take
        return files, len(archived)
    finally:
        img.close()


# ------------------------------------------------------------- self-test

_ST = [0]
_LAST_ERR = [""]
_LAST_SUMMARY = [""]


def _st(cond, msg):
    if not cond:
        raise _SelfTestFailure(msg)
    _ST[0] += 1


class _SelfTestFailure(Exception):
    pass


def _st_rc(argv, want_rc, msg, want_msg=None):
    try:
        rc = main(argv)
    except SystemExit as e:               # argparse usage errors exit 2
        rc = e.code if isinstance(e.code, int) else 1
    _st(rc == want_rc, "%s (rc %r, want %r): %s" % (argv, rc, want_rc, msg))
    if want_msg is not None:
        _st(want_msg in _LAST_ERR[0],
            "%s: the reason %r is not in %r" % (argv, want_msg, _LAST_ERR[0]))


def _trace_lines(ms_list):
    out = []
    for i, ms in enumerate(ms_list):
        # coarse s + the smallest fine whose truncated ms is exact
        coarse, fine = ms // 1000, -(-(ms % 1000) * 65536 // 1000)
        assert scet_ms(coarse, fine) == ms
        out.append("2026-11-13T10:00:00.000000Z stixdet_chunk chunk: "
                   "nom_bank 0, 49 blocks, scet 0x%08x:0x%04x, "
                   "int_ticks=10 eacc_events=1800 rotb_count=%d at "
                   "vclock_ns=%d\n" % (coarse, fine, i, i * 1000000000))
    return out


def self_test():
    """Offline: a synthetic image in a temp dir, then the reader, the
    truth format, the trace/uart cross-checks and the failure paths."""
    t0 = time.time()
    _ST[0] = 0
    fd = _fd()
    tmp = tempfile.mkdtemp(prefix="stix-l4-flash-truth-selftest-")
    try:
        path = os.path.join(tmp, "flash.bin")
        files = build_selftest_image(path)
        _st([f[2] for f in files] == [1, 3, 1, 2],
            "the builder wrote 1/3/1/2-chunk files")
        chunks, files2 = read_flash(path)
        _st(files2 == files, "the reader sees the builder's files: %s"
            % (files2,))
        _st([(ms // 1000, rcr) for ms, rcr in chunks]
            == [(1000005, 0), (1000010, 0), (1000011, 0), (1000012, 0),
                (1000020, 0), (1000030, 3), (1000031, 0)],
            "7 chunks ms-exact, sorted by name SCET, the rcr tag read: %s"
            % (chunks,))
        text = render_truth(chunks, files2, path)
        _st(text.startswith("# l4-flash-truth: 4 RAW_COUNTS files, 7 "
                            "archived chunks"),
            "the truth header line: %r" % text.splitlines()[0])
        _st("chunk 1000030.000 3\n" in text
            and "chunk 1000031.000 0\n" in text,
            "the chunk lines are ms-exact with the rcr tag")
        _st(text.count("\nfile ") == 4
            and "file %s 1000005.000 1\n" % files[0][0] in text,
            "the file lines carry the on-flash name and chunk count")

        # the trace cross-check: model = archived + the dropped first
        archived = [ms for ms, _ in chunks]
        good = os.path.join(tmp, "trace.good")
        with open(good, "w") as f:
            f.writelines(_trace_lines(archived + [1000099000]))
        _st_rc([path, "--trace", good], 0, "the exact model passes")
        short = os.path.join(tmp, "trace.short")
        with open(short, "w") as f:
            f.writelines(_trace_lines(archived))
        _st_rc([path, "--trace", short], 1,
               "model - 1 violated when nothing was dropped",
               want_msg="want model - 1")
        swap = os.path.join(tmp, "trace.swap")
        ms = archived + [1000099000]
        ms[2] = 1000099000
        with open(swap, "w") as f:
            f.writelines(_trace_lines(ms))
        _st_rc([path, "--trace", swap], 1, "a SCET mismatch is named",
               want_msg="archived chunk 2 SCET 1000011.000 s != model")
        drop = os.path.join(tmp, "trace.drop")
        with open(drop, "w") as f:
            f.writelines(_trace_lines(archived + [1000099000]))
            f.write("2026-11-13T10:00:01.000000Z stixdet_chunk_drop "
                    "chunk: dropped\n")
        _st_rc([path, "--trace", drop], 1, "a chunk-drop line fails",
               want_msg="stixdet_chunk_drop")

        # the UART cross-check
        uart = os.path.join(tmp, "uart.txt")
        with open(uart, "w") as f:
            f.write("00:00:00.150  Main             init: SAFE mode\n"
                    "00:01:16.0  ArchiveManager   *E Skipped first Det "
                    "data chunk\n")
        _st_rc([path, "--trace", good, "--uart", uart], 0,
               "one skipped-first line passes")
        with open(uart, "w") as f:
            f.write("00:00:00.150  Main             init: SAFE mode\n")
        _st_rc([path, "--trace", good, "--uart", uart], 1,
               "no skipped-first line fails", want_msg="want exactly 1")
        with open(uart, "a") as f:
            f.write("00:01:16.0  ArchiveManager   *E Skipped first Det "
                    "data chunk\n"
                    "00:01:16.5  ArchiveManager   *E Skipped first Det "
                    "data chunk\n")
        _st_rc([path, "--trace", good, "--uart", uart], 1,
               "two skipped-first lines fail", want_msg="found 2")

        # an image without RAW_COUNTS files fails loudly
        empty = os.path.join(tmp, "empty.bin")
        with open(empty, "wb") as f:
            f.truncate(8 * fd.PAGES_PER_BLOCK * fd.PAGE_SIZE)
        _st_rc([empty], 1, "an image without RAW_COUNTS files fails",
               want_msg="no RAW_COUNTS files")

        # a zeroed chunk size field is refused (block 64, chunk 0)
        corrupt = os.path.join(tmp, "corrupt.bin")
        build_selftest_image(corrupt)
        img = fd.FlashImage(corrupt, inverted=True)
        try:
            img.write_guest(fd.block_page0_offset(64) + WALK_START
                            + CHUNK_SIZE_OFF, b"\x00\x00")
        finally:
            img.close()
        _st_rc([corrupt], 1, "a zeroed chunk size fails",
               want_msg="size field 0")

        # fileSizeInBytes past the file's pages is refused (block 65)
        big = os.path.join(tmp, "big.bin")
        build_selftest_image(big)
        img = fd.FlashImage(big, inverted=True)
        try:
            off = fd.block_page0_offset(65)
            page = img.read_guest(off, fd.PAGE_SIZE)
            aux = bytearray(page[fd.PAGE_DATA_SIZE:])
            aux[fd.AUX_FILE_SIZE:fd.AUX_FILE_SIZE + 4] = \
                (1 << 24).to_bytes(4, "big")
            img.write_guest(off, page[:fd.PAGE_DATA_SIZE] + bytes(aux))
        finally:
            img.close()
        _st_rc([big], 1, "fileSizeInBytes past the pages fails",
               want_msg="pages hold")

        # the out file and the summary line
        outp = os.path.join(tmp, "truth.txt")
        _st_rc([path, "--out", outp], 0, "--out writes the truth file")
        with open(outp) as f:
            first = f.readline().rstrip("\n")
        _st(first.startswith("# l4-flash-truth: 4 RAW_COUNTS files, 7 "
                            "archived chunks"),
            "the out file's header: %r" % first)
        _st(_LAST_SUMMARY[0] == "FLASH-TRUTH: files=4 chunks=7 rcrmax=3",
            "the summary line: %r" % _LAST_SUMMARY[0])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("self-test: OK (%d checks, %.2f s)" % (_ST[0], time.time() - t0))
    return 0


# ----------------------------------------------------------------- main

def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="l4-flash-truth.py",
        description="Read-only truth reader for a stixflash image's "
                    "RAW_COUNTS archive (L4 plan amendment A); imports "
                    "flash-damage.py's layout/IO code.")
    ap.add_argument("flash", nargs="?",
                    help="the stixflash backing image (read-only)")
    ap.add_argument("--plain", action="store_true",
                    help="the image is stored un-inverted "
                         "(inverted-storage=off)")
    ap.add_argument("--allow-shared", action="store_true",
                    help="allow a flash file inside the shared master "
                         "dirs (slot.sh stix_run_dir_guard's list)")
    ap.add_argument("--out", metavar="FILE",
                    help="write the truth file here (default stdout)")
    ap.add_argument("--trace", metavar="COLLECT.TRACE",
                    help="cross-check: archived[k] ms == the k-th "
                         "stixdet_chunk line's scet ms, archived = "
                         "model - 1, no chunk-drop line")
    ap.add_argument("--uart", metavar="UART.COLLECT.TXT",
                    help="assert exactly one 'Skipped first Det data "
                         "chunk' line")
    ap.add_argument("--self-test", action="store_true",
                    help="run the offline self-test (no image needed)")
    args = ap.parse_args(argv)
    try:
        if args.self_test:
            return self_test()
        if not args.flash:
            ap.error("need a stixflash image (or --self-test)")
        chunks, files = read_flash(args.flash, plain=args.plain,
                                   allow_shared=args.allow_shared)
        if not files:
            fd = _fd()
            raise TruthError(
                "no RAW_COUNTS files found in %s (FS blocks %d-%d and "
                "%d-%d)" % (args.flash, fd.FS_A[0], fd.FS_A[1],
                            fd.FS_B[0], fd.FS_B[1]))
        errs = []
        if args.trace:
            try:
                model, drops = read_trace_model(args.trace)
            except OSError as e:
                errs.append("cannot read %s: %s" % (args.trace, e))
            else:
                errs += check_trace(chunks, model, drops)
        if args.uart:
            try:
                check_uart(args.uart)
            except OSError as e:
                errs.append("cannot read %s: %s" % (args.uart, e))
        if errs:
            raise TruthError("; ".join(errs))
        text = render_truth(chunks, files, args.flash)
        if args.out:
            with open(args.out, "w") as f:
                f.write(text)
        else:
            sys.stdout.write(text)
        _LAST_SUMMARY[0] = "FLASH-TRUTH: files=%d chunks=%d rcrmax=%d" \
            % (len(files), len(chunks),
               max((r for _ms, r in chunks), default=0))
        print(_LAST_SUMMARY[0])
        return 0
    except TruthError as e:
        _LAST_ERR[0] = str(e)
        print("l4-flash-truth: %s" % e, file=sys.stderr)
        return 1
    except _fd().ToolError as e:
        _LAST_ERR[0] = str(e)
        print("l4-flash-truth: %s" % e, file=sys.stderr)
        return 1
    except OSError as e:
        _LAST_ERR[0] = str(e)
        print("l4-flash-truth: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
