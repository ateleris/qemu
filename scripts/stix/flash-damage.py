#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline NAND flash damage tool (flash damage plan D-X4, commit X-3).

Damages a QEMU stixflash backing image the way real NAND fails, exactly as
the FSW filesystem (~/dev/STIX-FSW/ASW) sees it:

  bad-block     factory bad-block marker 0 into the spares of pages 0/1
                (the FSW boot scan reads them, BSW/Flash/File.c:318-339)
  corrupt-file  locate a stored file by its 10-byte name and flip a
                deterministic bit pattern (header / data), or rewrite the
                spare pageAddress with a re-encoded BCH(13,7) header
                sub-block (address)
  show          print the file's block and pages (truth read-back)
  device        write a flash-damage sidecar for the device (X-1/X-2
                grammar, grlib_stixflash.c stixflash_dmg_entry)

Layout agreed byte-for-byte with the device and the FSW:
  page 4224 B = 4096 data + 128 spare (FilesystemDefinition.h:15-17,
  grlib_stixflash.c:33-35); 64 pages per block (:18, :36), 4096 blocks per
  chip (:21, :39), 8 chips per MCM (:23, :41), 2 MCMs (:25, :43). The FSW's
  linear page address splits mcm/chip/page (BSW/Flash/Flash.c:70-73); the
  backing-file offset is ((mcm*8 + chip)*262144 + page_in_chip) * 4224
  (grlib_stixflash.c:246-251). Spare layout = File_auxPageData_s
  (BSW/include/File.h:128-140), all multi-byte fields big-endian as the
  LEON stores them (File.c:497-502 swaps on little-endian test hosts).

Python 3 stdlib only. With the default inverted storage the file stores
every byte XOR 0xFF (grlib_stixflash.c:253-264): a hole (file 0x00) is
erased flash. The tool applies the inversion itself; --plain disables it.
Writes are in-place seek+write of the touched bytes only, never past EOF
(the file size IS the flash capacity, grlib_stixflash.c:230-237), and the
allocation is re-checked against the 64 MiB sparse cap after every edit
(scripts/stix/lib/slot.sh:277,305-310).
"""

import argparse
import json
import os
import shutil
import sys
import tempfile
import time

# ---- geometry: FilesystemDefinition.h:15-26 / grlib_stixflash.c:33-44 ----
PAGE_DATA_SIZE = 4096
PAGE_HEADER_SIZE = 128
PAGE_SIZE = PAGE_DATA_SIZE + PAGE_HEADER_SIZE          # 4224
PAGES_PER_BLOCK = 64
BLOCK_DATA_SIZE = PAGE_DATA_SIZE * PAGES_PER_BLOCK     # 262144
BLOCKS_PER_DEVICE = 4096
DEVICES_PER_MCM = 8
NUM_MCMS = 2
BLOCKS_PER_MCM = DEVICES_PER_MCM * BLOCKS_PER_DEVICE   # 32768
TOTAL_BLOCKS = NUM_MCMS * BLOCKS_PER_MCM               # 65536
TOTAL_PAGES = TOTAL_BLOCKS * PAGES_PER_BLOCK           # 4194304

# spare area = File_auxPageData_s (File.h:128-140), packed, offsets in the
# 128-byte header that follows the 4096 data bytes of a page
AUX_BAD_BLOCK_MARKER = 0     # bad iff != 0xFF (File.c:328)
AUX_EMPTY_PAGE_MARKER = 1    # 0xFF empty, 0xFE in use (File.c:64-65)
AUX_FILE_NAME = 2            # 10 bytes (FilesystemDefinition.h:58)
AUX_PAGE_ADDRESS = 12        # u32, linear page (File.c:492)
AUX_WRITE_TIME = 16          # u64 (File.c:491)
AUX_FILE_SIZE = 24           # u32 (File.c:490)
AUX_ECC = 28                 # ECCblock[8] x 12 B -> 28..123 (File.h:114-117)
AUX_FILLER = 124             # 4 bytes, 0xFF (File.c:494)
FN_LENGTH = 10               # File_CONFIG_FN_LENGTH (FilesystemDefinition.h:58)

# BCH over 528-byte sub-blocks, 8 per page (File.h:114-117)
FILE_ECC_SUBBLOCKS = 8
FILE_ECC_SUBBLOCK_SIZE = PAGE_SIZE // FILE_ECC_SUBBLOCKS      # 528
FILE_ECC_BCH_SIZE = 12
FILE_ECC_BCH_CORRECTIONS = 7

EPM_EMPTY = 0xFF                                             # File.c:64
EPM_IN_USE = 0xFE                                            # File.c:65

# FS-scanned block ranges. Control blocks start at block 64 per flash
# (MemFilesystem.c:43-44); the gates build with LIMIT_FS_SIZE=32, i.e. 32
# control blocks of FS_CONFIG_MAX_FILES=60 files each (MemFilesystem.c:46-48,
# FilesystemDefinition.h:48); FLASH B starts at FLASH_TOTAL_BLOCKS/2
# (MemFilesystem.c:494-500). FS A = blocks 64..1983, FS B = 32832..34751.
FS_CONFIG_MAX_FILES = 60
LIMIT_FS_SIZE = 32
FS_A = (64, 64 + LIMIT_FS_SIZE * FS_CONFIG_MAX_FILES - 1)
FS_B = (TOTAL_BLOCKS // 2 + 64,
        TOTAL_BLOCKS // 2 + 64 + LIMIT_FS_SIZE * FS_CONFIG_MAX_FILES - 1)
FS_RANGES = (FS_A, FS_B)

# blocks < 64 are the ASW image area: no damage by default (D-X2/D-X3;
# grlib_stixflash.c:145, MemFilesystem.c:43)
RESERVED_BLOCKS = 64

# sparse allocation cap, STIX_FLASH_MAX_ALLOC_KB (slot.sh:277,305-310)
CAP_KB_DEFAULT = 65536

# file types: the enum values of MemFilesystem_filetype_e
# (FilesystemDefinition.h:81-92), which are sequential from FIRST = 0.
# The header's per-line comments ("6 for FLARE", "8 for CONTEXT", ...)
# do not match the enum; the master's stored context names (type bytes
# 0x06 / 0x07) confirm the enum values.
FILE_TYPES = {
    "aspect": 1, "raw-counts": 2, "tmmgmt": 3, "flare": 4,
    "user-requests": 5, "context0": 6, "context1": 7, "lut": 8,
}


class ToolError(Exception):
    """A refusal or a failure to find something; exit status 1."""


# ---- addressing (D-X2): linear page <-> mcm/chip/page ----------------------

def page_split(lin):
    """FSW linear page -> (mcm, chip, page in chip), Flash.c:70-73."""
    return ((lin >> 21) & 1, (lin >> 18) & 7, lin & 0x3FFFF)


def page_join(mcm, chip, page_in_chip):
    """Inverse of page_split; the PAGEAD register value, Flash.c:81-84."""
    return (mcm << 21) | (chip << 18) | (page_in_chip & 0x3FFFF)


def file_offset(mcm, chip, page_in_chip):
    """Backing-file offset of a page, grlib_stixflash.c:246-251."""
    return (((mcm * DEVICES_PER_MCM + chip)
             * (BLOCKS_PER_DEVICE * PAGES_PER_BLOCK))
            + (page_in_chip & 0x3FFFF)) * PAGE_SIZE


def block_split(lin_block):
    """Linear block -> (mcm, chip, block in chip)."""
    return (lin_block // BLOCKS_PER_MCM,
            (lin_block // BLOCKS_PER_DEVICE) % DEVICES_PER_MCM,
            lin_block % BLOCKS_PER_DEVICE)


def block_join(mcm, chip, block_in_chip):
    return mcm * BLOCKS_PER_MCM + chip * BLOCKS_PER_DEVICE + block_in_chip


def block_of_page(lin_page):
    """The containing linear block (the device's reserved rule,
    grlib_stixflash.c:1243-1246)."""
    return lin_page // PAGES_PER_BLOCK


# ---- file names (FilesystemDefinition.h:58-73, FilesystemAPI.c:77-96) ------

def build_name(ftype, spare=0, coarse=0, fine=0, duration=0):
    """The 10-byte name as the FSW's FScreateFilename stores it: type,
    spare, coarse u32 BE, fine u16 BE, duration u16 BE; the time-sync bit
    (bit 7 of the coarse MSB) is stripped like FSwrite does
    (FilesystemAPI.c:22-27, 38)."""
    if not 0 <= ftype <= 0xFF or not 0 <= spare <= 0xFF \
            or not 0 <= coarse <= 0xFFFFFFFF or not 0 <= fine <= 0xFFFF \
            or not 0 <= duration <= 0xFFFF:
        raise ToolError("file name field out of range")
    coarse &= 0x7FFFFFFF            # removeTimeSyncBit
    return bytes([ftype, spare,
                  (coarse >> 24) & 0xFF, (coarse >> 16) & 0xFF,
                  (coarse >> 8) & 0xFF, coarse & 0xFF,
                  (fine >> 8) & 0xFF, fine & 0xFF,
                  (duration >> 8) & 0xFF, duration & 0xFF])


def name_hex(name):
    return "0x" + name.hex()


# ---- BCH(13,7), the FSW's ECC over each 528-byte sub-block -----------------
#
# The FSW uses the Parrot/Linux binary BCH library with the parameters
# frozen at compile time: m = 13, t = FILE_ECC_BCH_CORRECTIONS = 7
# (MemoryMgmt/BCH.c:130-132, File.h:117), 12 ECC bytes per sub-block
# (BCH.c:1408, DIV_ROUND_UP(13*7, 8); File.h:116 FILE_ECC_BCH_SIZE).
# File_write encodes all 8 sub-blocks of every page with the aux ECC area
# zeroed (File.c:504-518); the boot scan and File_read decode sub-block 7
# respectively every sub-block the same way (File.c:355-376, 658-685).
#
# Bits are processed MSB-first ("input data is read in big-endian format",
# BCH.c:309-310; the explicit endian swap makes that true on little-endian
# test hosts too) and the ECC bytes are packed MSB-first from the parity
# register (store_ecc8, BCH.c:214-232). In codeword terms the received
# buffer is one polynomial: data bit string index s (0-based, MSB of byte 0
# first) has degree N-1-s with N = 8*len + ecc_bits, and the parity string
# index s' has degree ecc_bits-1-s'.

BCH_M = 13
BCH_T = FILE_ECC_BCH_CORRECTIONS
BCH_N = (1 << BCH_M) - 1                       # 8191, BCH.c:138
# default primitive polynomial for m = 13: prim_poly_tab[m - 5] (BCH.c:1365,
# 1368-1371, 1399-1400) -> 0x201b
BCH_PRIM_POLY = 0x201b

_A_POW = None
_A_LOG = None
_GENPOLY = None          # g(X) as an int, bit k = coefficient of X^k
_GEN_DEG = None          # ecc_bits = deg g (BCH.c:1328)


def _gf_init():
    """build_gf_tables (BCH.c:1153-1176): alpha = 2 in GF(2^13) mod 0x201b."""
    global _A_POW, _A_LOG
    if _A_POW is not None:
        return
    a_pow = [0] * (BCH_N + 1)
    a_log = [0] * (BCH_N + 1)
    x = 1
    for i in range(BCH_N):
        a_pow[i] = x
        a_log[x] = i
        x <<= 1
        if x & (1 << BCH_M):
            x ^= BCH_PRIM_POLY
    a_pow[BCH_N] = 1                     # BCH.c:1172
    _A_POW, _A_LOG = a_pow, a_log


def _gf_mul(a, b):
    """GF(2^13) multiply via log tables (BCH.c:406-410)."""
    if a == 0 or b == 0:
        return 0
    s = _A_LOG[a] + _A_LOG[b]
    return _A_POW[s if s < BCH_N else s - BCH_N]   # mod_s, BCH.c:347-353


def _bch_init():
    """Compute g(X) = the product of (X + alpha^i) over the union of the
    cyclotomic cosets of 1, 3, .., 2t-1 (compute_generator_polynomial,
    BCH.c:1273-1335). The product has binary coefficients; deg g = 91 here
    (7 cosets x 13 elements), so ecc_bits = 91 and ecc_bytes = 12."""
    global _GENPOLY, _GEN_DEG
    if _GENPOLY is not None:
        return
    _gf_init()
    # roots[j] = 1 iff j is in the union of cosets (BCH.c:1293-1299)
    roots = [0] * (BCH_N + 1)
    for i in range(BCH_T):
        r = 2 * i + 1
        for _ in range(BCH_M):
            roots[r] = 1
            r = 2 * r if 2 * r < BCH_N else 2 * r - BCH_N   # mod_s
    # multiply g by (X + alpha^i) for every root exponent, low-order
    # coefficients first, exactly as BCH.c:1300-1314
    c = [1]
    deg = 0
    for i in range(BCH_N):
        if not roots[i]:
            continue
        r = _A_POW[i]
        c.append(1)                       # g->c[g->deg + 1] = 1
        for j in range(deg, 0, -1):
            c[j] = _gf_mul(c[j], r) ^ c[j - 1]
        c[0] = _gf_mul(c[0], r)
        deg += 1
    if any(v not in (0, 1) for v in c):
        raise AssertionError("BCH generator polynomial is not binary")
    _GEN_DEG = deg
    _GENPOLY = sum(v << j for j, v in enumerate(c))


def bch_ecc_bits():
    _bch_init()
    return _GEN_DEG


def _poly_eval(coefs, x):
    """Evaluate a GF(2^m) polynomial (coefs[j] = coeff of X^j) at x."""
    acc = 0
    for j in range(len(coefs) - 1, -1, -1):
        acc = _gf_mul(acc, x) ^ coefs[j]
    return acc


def _bch_encode_std(data):
    """The 12 parity bytes of one sub-block: parity = data(X) * X^ecc_bits
    mod g(X). Bit-serial division, MSB-first; the ecc_bits zero bits fed
    after the data are the X^ecc_bits shift of the systematic code, the
    bit-level equivalent of the C's per-byte step (each data byte enters
    the register at X^ecc_bits, encode_bch BCH.c:176-330 with the mod8_tab
    precomputed (p(X).X^(8b+deg g)) mod g, BCH.c:1181-1210). --self-test
    cross-checks this against a direct big-int division."""
    _bch_init()
    reg = 0
    top = 1 << _GEN_DEG
    for byte in data:
        for k in range(7, -1, -1):
            reg = (reg << 1) | ((byte >> k) & 1)
            if reg & top:
                reg ^= _GENPOLY
    for _ in range(_GEN_DEG):            # the * X^ecc_bits of the codeword
        reg <<= 1
        if reg & top:
            reg ^= _GENPOLY
    out = bytearray(FILE_ECC_BCH_SIZE)
    for s in range(_GEN_DEG):
        if (reg >> (_GEN_DEG - 1 - s)) & 1:
            out[s >> 3] |= 1 << (7 - (s & 7))
    return bytes(out)


def word_swap(data):
    """Reverse the bytes of every 32-bit word (len(data) % 4 == 0)."""
    return b"".join(bytes(data[i:i + 4])[::-1] for i in range(0, len(data), 4))


def bch_encode(data):
    """The 12 ECC bytes the FLIGHT FSW stores for one sub-block. encode_bch
    byte-swaps every aligned 32-bit word unconditionally (BCH.c:300-310, the
    Linux cpu_to_be32 hard-coded for a little-endian host); on the
    big-endian LEON that swap reverses each word, so the flight code word
    is the standard BCH of word_swap(data), not of data (FSW finding F19).
    Pinned by the master flash's stored header ECC and by a LEON-faithful
    build of BCH.c (tests/bch13t7-leon-vector.txt). A sub-block is 528 B
    and 4-aligned in the page buffer, so encode_bch always takes the
    aligned path."""
    return _bch_encode_std(word_swap(data))


def bch_decode(data, recv_ecc):
    """What the FLIGHT BCH_decode does with one sub-block: (ok, fixes,
    data_fixes). ok/fixes are those of the swapped stream (the error count
    does not depend on the byte order). data_fixes are the (byte, mask)
    flips the FSW APPLIES to the unswapped buffer: BCH_decode uses errloc
    as _data[errloc / 8] (BCH.c:1480-1486), a swapped-stream index, so a
    real error in byte b is "corrected" in byte b ^ 3, same bit (F19). The
    flips are therefore NOT the fix: apply them to model the flight result,
    which keeps the original error and adds a mirrored one."""
    return _bch_decode_std(word_swap(data), recv_ecc)


def _parity_by_division(data):
    """The same parity by plain big-int carry-less division of
    data(X) * X^ecc_bits by g(X) - an implementation independent of the
    LFSR above, used by --self-test to pin it."""
    _bch_init()
    reg = int.from_bytes(data, "big") << _GEN_DEG
    g = _GENPOLY
    shift = reg.bit_length() - (_GEN_DEG + 1)
    while shift >= 0:
        if reg >> (shift + _GEN_DEG) & 1:
            reg ^= g << shift
        shift -= 1
    # the deg-g parity bits sit MSB-aligned in the ECC bytes, the pad
    # bits at the end (store_ecc8, BCH.c:214-232)
    reg <<= FILE_ECC_BCH_SIZE * 8 - _GEN_DEG
    return reg.to_bytes(FILE_ECC_BCH_SIZE, "big")


def _bch_decode_std(data, recv_ecc):
    """Mirror of BCH_decode (BCH.c:1464-1488) + decode_bch
    (BCH.c:1089-1148): re-encode the data, XOR with the stored ECC and
    locate the error positions. Returns (ok, fixes, data_fixes) where ok is
    False when the FSW would declare the sub-block uncorrectable, fixes is
    the number of located errors and data_fixes lists (byte, mask) inside
    the data region (errors in the ECC region itself are counted but not
    applied, BCH.c:1481-1486)."""
    _bch_init()
    calc = int.from_bytes(_bch_encode_std(data), "big")
    recv = int.from_bytes(recv_ecc, "big")
    # the parity bits are MSB-aligned in the ECC bytes: drop the trailing
    # pad bits (not code bits, BCH.c:455-458); bit s of the result is
    # then the coefficient of X^s of E(X) mod g(X)
    diff = (calc ^ recv) >> (FILE_ECC_BCH_SIZE * 8 - _GEN_DEG)
    if diff == 0:
        return True, 0, []
    # syndromes S_j = E(alpha^j) of the error pattern, j = 1..2t: the diff
    # word's bit s has codeword degree s, and data errors
    # are folded into the same word because parity(recv) ^ recv_ecc = E mod g
    # (decode_bch computes exactly this word, BCH.c:1102-1124, and
    # compute_syndromes evaluates it at alpha^j, BCH.c:446-477)
    pos = [s for s in range(_GEN_DEG) if (diff >> s) & 1]
    syn = [0] * (2 * BCH_T)
    for j in range(1, 2 * BCH_T + 1):
        acc = 0
        for q in pos:
            acc ^= _A_POW[(j * q) % BCH_N]
        syn[j - 1] = acc
    deg, elp = _berlekamp_massey(syn)
    if deg < 0 or deg > BCH_T:
        return False, 0, []
    roots = _chien_roots(elp, deg)
    if len(roots) != deg:
        return False, 0, []              # decode_bch:1130-1133
    nbits = len(data) * 8 + _GEN_DEG     # decode_bch:1136
    fixes = []
    for p in roots:
        if p >= nbits:
            return False, 0, []          # decode_bch:1137-1141
        s_idx = nbits - 1 - p            # decode_bch:1142
        if s_idx < len(data) * 8:
            fixes.append((s_idx >> 3, 1 << (7 - (s_idx & 7))))
    return True, len(roots), fixes


def _berlekamp_massey(syn):
    """Verbatim port of compute_error_locator_polynomial (BCH.c:484-533),
    over GF(2^13) with the t = 7 iteration and degree guard. Returns
    (deg, elp) with elp[j] = coefficient of X^j, or (-1, None). The
    coefficient buffer is larger than the C's GF_POLY_SZ(2t) so a
    transient index can never raise; the C's bound is deg <= 2t."""
    t = BCH_T
    elp = [0] * (3 * t + 8)
    pelp = [0] * (3 * t + 8)
    elp_deg = 0
    pelp_deg = 0
    elp[0] = 1
    pelp[0] = 1
    pd = 1
    d = syn[0]
    pp = -1
    i = 0
    while i < t and elp_deg <= t:
        if d:
            k = 2 * i - pp
            elp_copy = elp[:]
            old_deg = elp_deg          # gf_poly_copy carries elp's old degree
            tmp = _A_LOG[d] + BCH_N - _A_LOG[pd]
            for j in range(pelp_deg + 1):
                if pelp[j]:
                    elp[j + k] ^= _A_POW[(tmp + _A_LOG[pelp[j]]) % BCH_N]
            tmp2 = pelp_deg + k
            if tmp2 > elp_deg:
                elp_deg = tmp2
                pelp = elp_copy
                pelp_deg = old_deg
                pd = d
                pp = 2 * i
        if i < t - 1:
            d = syn[2 * i + 2]
            for j in range(1, elp_deg + 1):
                d ^= _gf_mul(elp[j], syn[2 * i + 2 - j])
        i += 1
    return (-1, None) if elp_deg > t else (elp_deg, elp)


def _chien_roots(elp, deg):
    """Roots alpha^-p of the error locator, p = the codeword degree
    position. The FSW factors the polynomial with the Berlekamp Trace
    algorithm (find_poly_roots, BCH.c:977-1010); a Chien search over the
    n = 8191 field elements finds the same root set."""
    roots = []
    for p in range(BCH_N):
        x = _A_POW[BCH_N - p]            # alpha^-p (a_pow[BCH_N] == 1)
        acc = 0
        for j in range(deg, -1, -1):
            acc = _gf_mul(acc, x) ^ elp[j]
        if acc == 0:
            roots.append(p)
    return roots


# ---- the flash image --------------------------------------------------------

class FlashImage:
    """In-place access to a stixflash backing file. Reads and writes are
    byte-precise seek+write calls (a 16.5 GB image is never rewritten);
    every write is checked against EOF (the file size IS the capacity,
    grlib_stixflash.c:230-237) and against the sparse allocation cap
    (slot.sh:305-310). Guest bytes are converted to/from stored bytes with
    the XOR inversion when inverted storage is on (grlib_stixflash.c:253-264).
    Past-EOF reads return erased flash (0xFF guest), like the device
    (grlib_stixflash.c:448-450)."""

    def __init__(self, path, inverted):
        self.path = path
        self.inverted = inverted
        try:
            self.f = open(path, "r+b")
        except OSError as e:
            raise ToolError("cannot open flash file %s: %s" % (path, e))
        self.size = os.fstat(self.f.fileno()).st_size

    def close(self):
        self.f.close()

    def alloc_kb(self):
        """du -k equivalent: allocated 1K blocks, rounded up (slot.sh:305)."""
        st = os.fstat(self.f.fileno())
        return (st.st_blocks * 512 + 1023) // 1024

    def to_guest(self, raw):
        if self.inverted:
            return bytes(b ^ 0xFF for b in raw)
        return raw

    def to_stored(self, guest):
        if self.inverted:
            return bytes(b ^ 0xFF for b in guest)
        return guest

    def read_guest(self, off, n):
        """Guest bytes at off; a missing tail (past EOF, or a hole) reads
        as erased 0xFF."""
        self.f.seek(off)
        raw = self.f.read(n)
        guest = bytearray(b"\xFF" * n)
        guest[:len(raw)] = self.to_guest(raw)
        return bytes(guest)

    def read_raw(self, off, n):
        """Stored bytes at off; returns b"" for a read entirely past EOF
        and short reads at the boundary (holes read as 0x00 when inverted)."""
        self.f.seek(off)
        return self.f.read(n)

    def write_guest(self, off, guest):
        """Seek+write of exactly the touched bytes; never extends the file
        (D-X6/D-H)."""
        if off < 0 or off + len(guest) > self.size:
            raise ToolError(
                "write of %d bytes at offset %d would go past EOF "
                "(file size %d); the flash file is never grown"
                % (len(guest), off, self.size))
        self.f.seek(off)
        self.f.write(self.to_stored(guest))
        self.f.flush()
        check_cap(self)


def open_image(args):
    refuse_shared_dir(args.flash, allow=args.allow_shared)
    img = FlashImage(args.flash, inverted=not args.plain)
    try:
        check_cap(img)      # pre-check, like stix_flash_copy's master check
    except ToolError:
        img.close()
        raise
    return img


# ---- the guards --------------------------------------------------------------

def _shared_dirs():
    """stix_run_dir_guard's list (slot.sh:238-260, 34-35): STIX_SHARED_DIRS
    or $STIX_UPREV_DIR plus $STIX_LEGACY_DIRS, with paths.sh:24-28
    defaults."""
    env = os.environ.get("STIX_SHARED_DIRS")
    if env:
        raw = env.split()
    else:
        state = os.environ.get("STIX_STATE_DIR",
                               os.path.join(os.path.expanduser("~"),
                                            "dev", "stix-state"))
        uprev = os.environ.get("STIX_UPREV_DIR",
                               os.path.join(state, "uprev-run"))
        legacy = os.environ.get("STIX_LEGACY_DIRS",
                                "/tmp/stix-uprev-run /tmp/stix-baseline-run")
        raw = [uprev] + legacy.split()
    out = []
    for d in raw:
        d = os.path.realpath(os.path.expandvars(os.path.expanduser(d)))
        if d and d not in out:
            out.append(d)
    return out


def refuse_shared_dir(path, allow=False):
    """Refuse a flash file at or inside a shared master dir (slot.sh:253-258);
    the tool only ever damages private copies."""
    real = os.path.realpath(path)
    for shared in _shared_dirs():
        if real == shared or real.startswith(shared + os.sep):
            if allow:
                print("flash-damage: WARNING: %s is inside the shared flash "
                      "dir %s (--allow-shared given)" % (path, shared))
                return
            raise ToolError(
                "refusing %s: it is at or inside the shared flash dir %s "
                "(--allow-shared overrides)" % (path, shared))


def cap_limit_kb():
    return int(os.environ.get("STIX_FLASH_MAX_ALLOC_KB", CAP_KB_DEFAULT))


def check_cap(img):
    """Re-check the allocation after every edit (D-X4)."""
    alloc = img.alloc_kb()
    limit = cap_limit_kb()
    if alloc >= limit:
        raise ToolError(
            "flash file %s allocates %d KiB, sparse cap is %d KiB "
            "(STIX_FLASH_MAX_ALLOC_KB); refusing to grow it further"
            % (img.path, alloc, limit))


def check_reserved(lin_block, allow):
    """blocks < 64 are the ASW image area, refused by default (D-X2/D-X3)."""
    if lin_block < RESERVED_BLOCKS and not allow:
        raise ToolError(
            "block %d is in the reserved ASW image area (blocks < %d); "
            "--allow-reserved overrides" % (lin_block, RESERVED_BLOCKS))


# ---- the file-name scan -------------------------------------------------------

def iter_fs_blocks():
    """All FS-scanned blocks, A then B (D-X4 --fs-range)."""
    for lo, hi in FS_RANGES:
        for b in range(lo, hi + 1):
            yield b


def block_page0_offset(lin_block):
    mcm, chip, bic = block_split(lin_block)
    return file_offset(mcm, chip, bic * PAGES_PER_BLOCK)


def _name_matches(stored, name, any_spare):
    if any_spare:
        return stored[0] == name[0] and stored[2:] == name[2:]
    return stored == name


def scan_for_name(img, name, any_spare=False):
    """Locate a stored file by its 10-byte name: a chunked spare-area scan
    of the FS regions, page 0 of every block (the FSW catalogs one file per
    block from the first page, File.c:318-404). Past-EOF blocks read as
    erased and never match. Returns the list of matching linear blocks."""
    hits = []
    for b in iter_fs_blocks():
        off = block_page0_offset(b)
        if off >= img.size:
            continue                       # past EOF: erased (D-H)
        raw = img.read_raw(off + PAGE_DATA_SIZE, PAGE_HEADER_SIZE)
        if len(raw) < PAGE_HEADER_SIZE:
            continue                       # short read at the EOF boundary
        if img.inverted and not any(raw):
            continue                       # a hole is an erased page
        aux = img.to_guest(raw)
        if aux[AUX_EMPTY_PAGE_MARKER] != EPM_IN_USE:
            continue                       # not a stored file's first page
        if aux[AUX_BAD_BLOCK_MARKER] != 0xFF:
            continue                       # the boot scan marks it BAD
        stored = bytes(aux[AUX_FILE_NAME:AUX_FILE_NAME + FN_LENGTH])
        if _name_matches(stored, name, any_spare):
            if any_spare:
                print("flash-damage: block %d holds %s" % (b, name_hex(stored)))
            hits.append(b)
    return hits


# ---- page-level helpers -------------------------------------------------------

def page_offset(lin_block, page_in_block):
    mcm, chip, bic = block_split(lin_block)
    return file_offset(mcm, chip, bic * PAGES_PER_BLOCK + page_in_block)


def linear_page0(lin_block):
    mcm, chip, bic = block_split(lin_block)
    return page_join(mcm, chip, bic * PAGES_PER_BLOCK)


def aux_bytes(page):
    """The 128-byte File_auxPageData_s of a guest page image."""
    return page[PAGE_DATA_SIZE:]


def ecc_zero_span():
    """(offset, length) of the aux ECC area inside a page (File.h:137,
    AUX_ECC 28 .. 28 + 8*12 - 1 = 123), in page coordinates."""
    return PAGE_DATA_SIZE + AUX_ECC, FILE_ECC_SUBBLOCKS * FILE_ECC_BCH_SIZE


def subblock_with_ecc_zeroed(page, sub):
    """The 528-byte sub-block buffer as the FSW hands it to BCH_encode /
    BCH_decode: the aux ECC area is zeroed so the ECC cannot cover itself
    (File.c:495/504-514 encode, File.c:660-662 decode). Only sub-block 7
    (page bytes 3696..4223) contains the aux area."""
    buf = bytearray(page[sub * FILE_ECC_SUBBLOCK_SIZE:
                         (sub + 1) * FILE_ECC_SUBBLOCK_SIZE])
    if sub == FILE_ECC_SUBBLOCKS - 1:
        off, ln = ecc_zero_span()
        base = sub * FILE_ECC_SUBBLOCK_SIZE
        buf[off - base:off - base + ln] = b"\x00" * ln
    return bytes(buf)


def stored_ecc(page, sub):
    aux = aux_bytes(page)
    o = AUX_ECC + sub * FILE_ECC_BCH_SIZE
    return bytes(aux[o:o + FILE_ECC_BCH_SIZE])


def bit_flips(start, count):
    """A deterministic flip pattern: bit-string indices [start, start+count)
    of a sub-block -> (byte in sub-block, mask). MSB-first, the codeword bit
    order of the FSW's BCH."""
    flips = []
    for s in range(start, start + count):
        flips.append((s >> 3, 1 << (7 - (s & 7))))
    return flips


def apply_flips(buf, flips):
    out = bytearray(buf)
    for byte, mask in flips:
        out[byte] ^= mask
    return bytes(out)


# ---- bad-block (D-X2/D-X4) -----------------------------------------------------

def op_bad_block(img, blocks, allow_reserved, fs_range=False):
    """badBlockMarker = 0 into the spare of pages 0 and 1 (the grown-bad-block
    marker the FSW itself programs, File.c:585-598; the boot scan reads
    page0+1 then page0 and marks the block BAD, File.c:318-339)."""
    for b in blocks:
        base = block_page0_offset(b)
        if base >= img.size:
            if fs_range:
                continue               # past EOF: nothing stored there
            raise ToolError("block %d starts at offset %d, past EOF "
                            "(file size %d)" % (b, base, img.size))
        check_reserved(b, allow_reserved)
        for p in (0, 1):
            off = base + p * PAGE_SIZE + PAGE_DATA_SIZE + AUX_BAD_BLOCK_MARKER
            # one byte per page: the rest of the page stays erased
            img.write_guest(off, b"\x00")
        print("flash-damage: block %d: bad-block marker 0 written into the "
              "spares of pages 0 and 1" % b)


# ---- corrupt-file (D-X2/D-X4, review finding 5) --------------------------------

def locate_file(img, name, any_spare=False):
    hits = scan_for_name(img, name, any_spare)
    if not hits:
        raise ToolError("file %s not found in the FS ranges (blocks "
                        "%d-%d and %d-%d)" % (name_hex(name),
                                              FS_A[0], FS_A[1], FS_B[0], FS_B[1]))
    if len(hits) > 1:
        raise ToolError("file %s found on %d blocks (%s); refusing to "
                        "guess" % (name_hex(name), len(hits),
                                   ", ".join(str(b) for b in hits)))
    return hits[0]


def check_file_page(img, lin_block, page_in_block):
    """The page must be a stored, consistent page of the file (in use, on
    the block the FSW would read, File.c:651-699)."""
    off = page_offset(lin_block, page_in_block)
    page = img.read_guest(off, PAGE_SIZE)
    aux = aux_bytes(page)
    if aux[AUX_EMPTY_PAGE_MARKER] != EPM_IN_USE:
        raise ToolError("page %d of block %d is not in use "
                        "(emptyPageMarker 0x%02X); the file does not span it"
                        % (page_in_block, lin_block, aux[AUX_EMPTY_PAGE_MARKER]))
    expected = linear_page0(lin_block) + page_in_block
    got = int.from_bytes(aux[AUX_PAGE_ADDRESS:AUX_PAGE_ADDRESS + 4], "big")
    if got != expected:
        raise ToolError("page %d of block %d carries pageAddress %d, "
                        "expected %d; the page is inconsistent already"
                        % (page_in_block, lin_block, got, expected))
    return off, page


def flip_bytes_of_page(flips, sub):
    """The page offsets a sub-block flip pattern touches, as (page offset in
    the sub-block's frame)."""
    base = sub * FILE_ECC_SUBBLOCK_SIZE
    return [(base + b, m) for b, m in flips]


def write_page_flips(img, off, page, flips_abs):
    """Write only the flipped bytes back (one seek+write per touched byte);
    several flips in one byte accumulate."""
    masks = {}
    for byte, mask in flips_abs:
        masks[byte] = masks.get(byte, 0) ^ mask
    for byte in sorted(masks):
        img.write_guest(off + byte, bytes([page[byte] ^ masks[byte]]))
    return sorted(masks)


def corrupt_header(img, lin_block, bits):
    """Uncorrectable first-page header: the boot scan's header check
    (BCH_decode of sub-block 7 of page 0, File.c:355-376) must fail, so the
    file "does not exist" (E_FILE_BLOCK_STAT_ERROR, File.c:369-375). Any
    error pattern of weight > t that is not within distance t of a lighter
    codeword fails; the candidate patterns are swept deterministically and
    each one is verified uncorrectable with the tool's own BCH decoder
    before anything is written."""
    if bits < FILE_ECC_BCH_CORRECTIONS + 1:
        raise ToolError("--bits for --mode header must be > %d "
                        "(BCH corrects up to %d per sub-block)"
                        % (FILE_ECC_BCH_CORRECTIONS, FILE_ECC_BCH_CORRECTIONS))
    off, page = check_file_page(img, lin_block, 0)
    sub = FILE_ECC_SUBBLOCKS - 1
    ecc7 = stored_ecc(page, sub)
    buf = subblock_with_ecc_zeroed(page, sub)
    chosen = None
    start = 0
    tries = 0
    while (start + bits <= FILE_ECC_SUBBLOCK_SIZE * 8) and tries < 16:
        flips = bit_flips(start, bits)
        tries += 1
        ok, _, _ = bch_decode(apply_flips(buf, flips), ecc7)
        if not ok:
            chosen = flips
            break
        start += 8
    if chosen is None:
        raise ToolError("no deterministic %d-flip pattern in header "
                        "sub-block 7 failed the BCH check; not writing"
                        % bits)
    touched = write_page_flips(img, off, page, flip_bytes_of_page(chosen, sub))
    print("flash-damage: block %d page 0: %d bits flipped in header "
          "sub-block 7 (bytes %s); the boot scan's header check now fails"
          % (lin_block, bits,
             " ".join(str(b) for b in touched)))


def corrupt_data(img, lin_block, page_in_block, sub, bits):
    """Flip --bits bits in one 528-byte sub-block of one page: <= 7 flips
    take the BCH-corrected read path ("BCH corrected N SEUs",
    MemFilesystem.c:604-606), 5..7 additionally the remove-and-rewrite
    (MemFilesystem.c:608-654), > 7 make the page uncorrectable ->
    E_FILE_RES_ERROR -> TM(5,2) 0x543B (File.c:665-685,
    MemFilesystem.c:589-592)."""
    if not 1 <= bits <= 64:
        raise ToolError("--bits must be 1..64")
    off, page = check_file_page(img, lin_block, page_in_block)
    ecc = stored_ecc(page, sub)
    buf = subblock_with_ecc_zeroed(page, sub)
    if bits <= FILE_ECC_BCH_CORRECTIONS:
        flips = bit_flips(0, bits)          # correctable: always, BCH bound
        ok, fixes, _ = bch_decode(apply_flips(buf, flips), ecc)
        if not ok or fixes != bits:
            raise ToolError("internal: %d flips did not decode as %d "
                            "corrections" % (bits, bits))
    else:
        flips = None
        start = 0
        tries = 0
        while (start + bits <= FILE_ECC_SUBBLOCK_SIZE * 8) and tries < 16:
            cand = bit_flips(start, bits)
            tries += 1
            ok, _, _ = bch_decode(apply_flips(buf, cand), ecc)
            if not ok:
                flips = cand
                break
            start += 8
        if flips is None:
            raise ToolError("no deterministic %d-flip pattern in sub-block "
                            "%d failed the BCH check; not writing"
                            % (bits, sub))
    touched = write_page_flips(img, off, page, flip_bytes_of_page(flips, sub))
    print("flash-damage: block %d page %d: %d bits flipped in sub-block %d "
          "(bytes %s)" % (lin_block, page_in_block, bits, sub,
                          " ".join(str(b) for b in touched)))


def corrupt_address(img, lin_block, pages, to_page, allow_reserved):
    """wrong-address (review finding 5, D-X2): rewrite the spare pageAddress
    u32 (aux offset 12, File.h:133) of the chosen pages to a wrong but valid
    page address and re-encode sub-block 7's BCH (the ECC[7] bytes at aux
    112..123), so the read passes BCH (File.c:665-685) and reaches the FSW's
    address check instead (File.c:691-699) -> E_FILE_RES_ERROR, reported as
    0x543B on the traced read path (MemFilesystem.c:589-592)."""
    lin0 = linear_page0(lin_block)
    for p in pages:
        off, page = check_file_page(img, lin_block, p)
        aux = bytearray(aux_bytes(page))
        expected = lin0 + p
        wrong = to_page if to_page is not None else expected + 1
        if wrong == expected:
            raise ToolError("--to %d is the page's current address" % wrong)
        if not 0 <= wrong < TOTAL_PAGES:
            raise ToolError("--to %d is not a valid page (0..%d)"
                            % (wrong, TOTAL_PAGES - 1))
        check_reserved(block_of_page(wrong), allow_reserved)
        aux[AUX_PAGE_ADDRESS:AUX_PAGE_ADDRESS + 4] = wrong.to_bytes(4, "big")
        new_page = page[:PAGE_DATA_SIZE] + bytes(aux)
        buf = subblock_with_ecc_zeroed(new_page, FILE_ECC_SUBBLOCKS - 1)
        ecc7 = bch_encode(buf)
        ok, fixes, _ = bch_decode(buf, ecc7)
        if not ok or fixes != 0:
            raise ToolError("internal: re-encoded header sub-block does "
                            "not decode clean")
        img.write_guest(off + PAGE_DATA_SIZE + AUX_PAGE_ADDRESS,
                        wrong.to_bytes(4, "big"))
        img.write_guest(off + PAGE_DATA_SIZE + AUX_ECC
                        + (FILE_ECC_SUBBLOCKS - 1) * FILE_ECC_BCH_SIZE, ecc7)
        print("flash-damage: block %d page %d: spare pageAddress %d -> %d, "
              "header sub-block BCH re-encoded"
              % (lin_block, p, expected, wrong))


# ---- show (D-X4) -----------------------------------------------------------

def in_use_pages(img, lin_block):
    """The pages of the block whose aux says "in use" (emptyPageMarker 0xFE,
    File.c:65) -- the pages the file's write really programmed
    (File.c:476-525)."""
    pages = []
    for p in range(PAGES_PER_BLOCK):
        off = page_offset(lin_block, p)
        if off >= img.size:
            break
        if aux_bytes(img.read_guest(off, PAGE_SIZE))[
                AUX_EMPTY_PAGE_MARKER] == EPM_IN_USE:
            pages.append(p)
    return pages


def op_show(img, name, any_spare=False):
    lin_block = locate_file(img, name, any_spare)
    mcm, chip, bic = block_split(lin_block)
    lin0 = linear_page0(lin_block)
    print("file %s: block %d (mcm %d chip %d block %d), pages:"
          % (name_hex(name), lin_block, mcm, chip, bic))
    n_pages = 0
    for p in range(PAGES_PER_BLOCK):
        off = page_offset(lin_block, p)
        if off >= img.size:
            break
        aux = aux_bytes(img.read_guest(off, PAGE_SIZE))
        if aux[AUX_EMPTY_PAGE_MARKER] == EPM_EMPTY:
            continue
        n_pages += 1
        page_addr = int.from_bytes(aux[AUX_PAGE_ADDRESS:
                                       AUX_PAGE_ADDRESS + 4], "big")
        size = int.from_bytes(aux[AUX_FILE_SIZE:AUX_FILE_SIZE + 4], "big")
        wtime = int.from_bytes(aux[AUX_WRITE_TIME:AUX_WRITE_TIME + 8], "big")
        expected = lin0 + p
        mark = "" if page_addr == expected else "  WRONG (expected %d)" % expected
        print("  page %d (linear %d): pageAddress %d%s, fileSize %d, "
              "logicalWriteTime %d" % (p, expected, page_addr, mark,
                                       size, wtime))
    if not n_pages:
        print("  (no in-use pages)")


# ---- device (D-X3/X-2: the sidecar grammar) -----------------------------------

SPEC_KEYS = ("kind", "block", "page", "mcm", "chip", "bit", "value")
SPEC_KINDS = ("erase-fail", "program-fail", "stuck", "stuck0")


def validate_spec(spec):
    """Mirror of the device's stixflash_dmg_entry (grlib_stixflash.c:1092-1296)
    used before the sidecar is written, so the tool never emits a map the
    device would reject at realize. Same reserved-block rule as the sidecar:
    containing linear block >= 64, no override (D-X3)."""
    for k in spec:
        if k not in SPEC_KEYS:
            raise ToolError("unknown sidecar key '%s'" % k)
    kind = spec.get("kind")
    if kind not in SPEC_KINDS:
        raise ToolError("'kind' must be one of %s" % ", ".join(SPEC_KINDS))
    is_stuck = kind in ("stuck", "stuck0")
    if kind == "stuck0" and "value" in spec:
        raise ToolError("'value' with the 'stuck0' alias; use kind 'stuck' "
                        "with value 0")
    if is_stuck and kind != "stuck0":
        if spec.get("value") not in (0, 1):
            raise ToolError("'value' must be 0 or 1")
    if not is_stuck and "value" in spec:
        raise ToolError("'value' is only valid with kind 'stuck'")
    has_mcm = "mcm" in spec
    has_chip = "chip" in spec
    if has_mcm != has_chip:
        raise ToolError("'mcm' and 'chip' go together")
    if has_mcm:
        if not 0 <= spec["mcm"] < NUM_MCMS or not 0 <= spec["chip"] < DEVICES_PER_MCM:
            raise ToolError("'mcm' must be 0..%d and 'chip' 0..%d"
                            % (NUM_MCMS - 1, DEVICES_PER_MCM - 1))
    has_block = "block" in spec
    has_page = "page" in spec
    lin_block = None
    page_scope = None
    if has_block and has_page:
        if not has_mcm:
            raise ToolError("'block' and 'page' together need 'mcm' and "
                            "'chip' (the framework form)")
        if not 0 <= spec["block"] < BLOCKS_PER_DEVICE \
                or not 0 <= spec["page"] < PAGES_PER_BLOCK:
            raise ToolError("framework form: 'block' 0..%d, 'page' 0..%d"
                            % (BLOCKS_PER_DEVICE - 1, PAGES_PER_BLOCK - 1))
        lin_block = block_join(spec["mcm"], spec["chip"], spec["block"])
        page_scope = True
    elif has_block:
        if has_mcm:
            if not 0 <= spec["block"] < BLOCKS_PER_DEVICE:
                raise ToolError("'block' with 'mcm'/'chip' is the block in "
                                "the chip, 0..%d" % (BLOCKS_PER_DEVICE - 1))
            lin_block = block_join(spec["mcm"], spec["chip"], spec["block"])
        else:
            if not 0 <= spec["block"] < TOTAL_BLOCKS:
                raise ToolError("linear 'block' must be 0..%d"
                                % (TOTAL_BLOCKS - 1))
            lin_block = spec["block"]
        page_scope = False
    elif has_page:
        if has_mcm:
            if not 0 <= spec["page"] < BLOCKS_PER_DEVICE * PAGES_PER_BLOCK:
                raise ToolError("'page' with 'mcm'/'chip' is the page in the "
                                "chip, 0..%d"
                                % (BLOCKS_PER_DEVICE * PAGES_PER_BLOCK - 1))
        else:
            if not 0 <= spec["page"] < TOTAL_PAGES:
                raise ToolError("linear 'page' must be 0..%d"
                                % (TOTAL_PAGES - 1))
        page_scope = True
    else:
        raise ToolError("needs a 'block' or a 'page'")
    if lin_block is None:
        if has_mcm:
            lin_block = block_join(spec["mcm"], spec["chip"],
                                   spec["page"] // PAGES_PER_BLOCK)
        else:
            lin_block = block_of_page(spec["page"])
    check_reserved(lin_block, False)     # the sidecar has no override (D-X3)
    if kind == "erase-fail" and page_scope:
        raise ToolError("erase-fail is a block kind")
    if kind == "program-fail" and not page_scope:
        raise ToolError("program-fail is a page kind")
    if is_stuck and page_scope and kind != "stuck0":
        bit = spec.get("bit")
        if not isinstance(bit, str):
            raise ToolError("stuck needs a bit as 'byte.bit'")
        parts = bit.split(".")
        if len(parts) != 2 or not all(p.isdigit() for p in parts):
            raise ToolError("stuck needs a bit as 'byte.bit'")
        byte, bitn = int(parts[0]), int(parts[1])
        if byte >= PAGE_SIZE or bitn >= 8:
            raise ToolError("stuck bit must be byte < %d and bit < 8"
                            % PAGE_SIZE)
    if is_stuck and not page_scope and "bit" in spec:
        raise ToolError("block-scope stuck takes no 'bit'")
    return lin_block


def op_device(args):
    """Write the sidecar JSON {"v":1,"damage":[...]} next to the flash
    (D-X3), with the X-1/X-2 entry grammar (grlib_stixflash.c:1102-1104
    key list; kind stuck carries value 0|1, stuck0 = the value-0 alias
    without a value)."""
    spec = {"kind": args.kind}
    if args.mcm is not None:
        spec["mcm"] = args.mcm
        spec["chip"] = args.chip
    if args.block is not None:
        spec["block"] = args.block
    if args.page is not None:
        spec["page"] = args.page
    if args.bit is not None:
        if args.offset is not None:
            raise ToolError("--bit and --offset are two spellings of the "
                            "same thing; give one")
        spec["bit"] = args.bit
    elif args.offset is not None:
        if args.bitn is None:
            raise ToolError("--offset needs the bit inside the byte: --bitn "
                            "0..7 (the framework's offset=<o> has no sidecar "
                            "key; it resolves to the bit 'offset.bit')")
        spec["bit"] = "%d.%d" % (args.offset, args.bitn)
    elif args.bitn is not None:
        raise ToolError("--bitn only together with --offset")
    if args.value is not None:
        spec["value"] = args.value
    validate_spec(spec)
    doc = {"v": 1, "damage": [spec]}
    if os.path.exists(args.out) and not args.force:
        raise ToolError("refusing to overwrite %s (--force replaces it)"
                        % args.out)
    with open(args.out, "w") as f:
        f.write(json.dumps(doc) + "\n")
    print("flash-damage: wrote %s: %s" % (args.out, json.dumps(doc)))
    return 0


# ---- --self-test ------------------------------------------------------------

SELFTEST_BLOCKS = 72      # covers FS A blocks 64..71
SELFTEST_SIZE = SELFTEST_BLOCKS * PAGE_SIZE * PAGES_PER_BLOCK

_ST_CHECKS = [0]


def _st(cond, msg):
    if not cond:
        raise _SelfTestFailure(msg)
    _ST_CHECKS[0] += 1


class _SelfTestFailure(Exception):
    pass


def _st_expect_rc(argv, want_rc, msg):
    try:
        rc = main(argv)
    except SystemExit as e:                  # argparse usage errors exit 2
        rc = e.code if isinstance(e.code, int) else 1
    _st(rc == want_rc, "%s (rc %r, want %r): %s"
        % (argv[:3], rc, want_rc, msg))


def _make_fsw_page(name, lin_page, file_size, write_time, pattern):
    """One page exactly as File_write's write1 programs it (File.c:472-525):
    data = file bytes zero-padded to 4096, aux = marker 0xFF / empty 0xFE /
    name / pageAddress / logicalWriteTime / fileSizeInBytes / ECC / filler
    0xFF, multi-byte fields big-endian (File.c:497-502 swaps on LE test
    hosts = the LEON stores big-endian), ECC of all 8 sub-blocks computed
    with the aux ECC area zeroed (File.c:495, 504-518)."""
    page = bytearray(PAGE_SIZE)
    n = min(file_size, PAGE_DATA_SIZE)
    page[:n] = pattern[:n]
    aux = bytearray(PAGE_HEADER_SIZE)
    aux[AUX_BAD_BLOCK_MARKER] = 0xFF
    aux[AUX_EMPTY_PAGE_MARKER] = EPM_IN_USE
    aux[AUX_FILE_NAME:AUX_FILE_NAME + FN_LENGTH] = name
    aux[AUX_PAGE_ADDRESS:AUX_PAGE_ADDRESS + 4] = lin_page.to_bytes(4, "big")
    aux[AUX_WRITE_TIME:AUX_WRITE_TIME + 8] = write_time.to_bytes(8, "big")
    aux[AUX_FILE_SIZE:AUX_FILE_SIZE + 4] = file_size.to_bytes(4, "big")
    aux[AUX_FILLER:AUX_FILLER + 4] = b"\xFF" * 4
    page[PAGE_DATA_SIZE:] = aux
    for sub in range(FILE_ECC_SUBBLOCKS):
        buf = subblock_with_ecc_zeroed(bytes(page), sub)
        ecc = bch_encode(buf)
        o = PAGE_DATA_SIZE + AUX_ECC + sub * FILE_ECC_BCH_SIZE
        page[o:o + FILE_ECC_BCH_SIZE] = ecc
    return bytes(page)


def _st_write_file(img, lin_block, name, file_size, write_time, pattern):
    page = _make_fsw_page(name, linear_page0(lin_block), file_size,
                          write_time, pattern)
    img.write_guest(page_offset(lin_block, 0), page)


def _st_bch_section():
    """The BCH(13,7) algebra, the encoder cross-check, the decode round
    trips and the reference-vector pin."""
    _bch_init()
    _st(_GEN_DEG == 91,
        "BCH(13,7) generator degree is %d, want 91 (cosets of 1,3,..,13 x 13)"
        % _GEN_DEG)
    # g(alpha^i) == 0 for every coset member: g IS the product of the
    # minimal polynomials (BCH.c:1293-1314), the anchor for everything below
    coset = [0] * (BCH_N + 1)
    for i in range(BCH_T):
        r = 2 * i + 1
        for _ in range(BCH_M):
            coset[r] = 1
            r = 2 * r if 2 * r < BCH_N else 2 * r - BCH_N
    gcoefs = [(_GENPOLY >> j) & 1 for j in range(_GEN_DEG + 1)]
    for i in range(BCH_N):
        if coset[i]:
            _st(_poly_eval(gcoefs, _A_POW[i]) == 0,
                "generator polynomial has root alpha^%d" % i)
    # the bit-serial LFSR encoder must agree with a direct carry-less
    # division of data(X) * X^ecc_bits by g(X)
    patterns = [b"\x00" * FILE_ECC_SUBBLOCK_SIZE,
                b"\xFF" * FILE_ECC_SUBBLOCK_SIZE,
                b"\xA5" * FILE_ECC_SUBBLOCK_SIZE,
                bytes(i & 0xFF for i in range(FILE_ECC_SUBBLOCK_SIZE)),
                bytes((i * 7 + 3) & 0xFF
                      for i in range(FILE_ECC_SUBBLOCK_SIZE))]
    for p in patterns:
        _st(_bch_encode_std(p) == _parity_by_division(p),
            "encoder matches the independent polynomial division")
    # decode (the standard core): 1..7 flips are corrected at exactly the
    # flipped bits (BCH bound, d_min >= 2t+1); a single flipped ECC byte is
    # located too. The flight wrapper's mirrored fixes: _st_leon_vectors.
    data = patterns[4]
    ecc = _bch_encode_std(data)
    for k in (1, 2, 5, 7):
        flips = bit_flips(0, k)
        ok, fixes, data_fixes = _bch_decode_std(apply_flips(data, flips), ecc)
        _st(ok and fixes == k, "%d data flips decode with %d fixes" % (k, k))
        _st(sorted(data_fixes) == sorted(flips),
            "%d fixes land on the flipped bits" % k)
        ok, fixes, _ = bch_decode(apply_flips(data, flips), bch_encode(data))
        _st(ok and fixes == k,
            "flight: %d data flips are counted as %d fixes" % (k, k))
    bad_ecc = bytes([ecc[0] ^ 0x80]) + ecc[1:]
    ok, fixes, data_fixes = _bch_decode_std(data, bad_ecc)
    _st(ok and fixes == 1 and data_fixes == [],
        "one flipped ECC bit is located and counted, the data untouched")
    # reference vector (D-X4): pinned when the fixture is committed;
    # generated once by the reviewer against the framework's BCH.c
    fixture = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "tests", "bch13t7-reference-vector.txt")
    if not os.path.exists(fixture):
        print("self-test: note: %s is not committed yet; run "
              "`flash-damage.py --print-vector`, cross-check the output "
              "against the unit-test framework's BCH.c and commit it to "
              "pin the encoder" % fixture)
        return
    vecs = _parse_vectors(fixture)
    _st(len(vecs) >= 1, "reference vector file has at least one vector")
    for name, data_v, ecc_v in vecs:
        _st(len(data_v) == FILE_ECC_SUBBLOCK_SIZE
            and len(ecc_v) == FILE_ECC_BCH_SIZE,
            "vector '%s' has 528 data and 12 ecc bytes" % name)
        _st(_bch_encode_std(data_v) == ecc_v,
            "vector '%s' pins the encoder" % name)
        ok, fixes, _ = _bch_decode_std(data_v, ecc_v)
        _st(ok and fixes == 0, "vector '%s' decodes clean" % name)
        bad = bytes([ecc_v[0] ^ 0x01]) + ecc_v[1:]
        ok, fixes, _ = _bch_decode_std(data_v, bad)
        _st(ok and fixes == 1,
            "vector '%s': a wrong ECC byte is caught, not ignored" % name)


def _st_leon_vectors():
    """F19: the flight encoder (word-swapped) against a LEON-faithful build
    of BCH.c and a header sub-block the AX gate ELF wrote, and the flight
    decoder's mirrored miscorrection."""
    fixture = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "tests", "bch13t7-leon-vector.txt")
    vecs = _parse_vectors(fixture)
    _st(len(vecs) >= 2, "LEON vector file has the emulated and live vectors")
    for name, data_v, ecc_v in vecs:
        _st(bch_encode(data_v) == ecc_v,
            "LEON vector '%s' pins the flight encoder" % name)
        ok, fixes, _ = bch_decode(data_v, ecc_v)
        _st(ok and fixes == 0, "LEON vector '%s' decodes clean" % name)
    name, data_v, ecc_v = vecs[0]
    _st(_bch_encode_std(data_v) != ecc_v,
        "LEON vector '%s' differs from the standard (x86) ECC" % name)
    for b, bit in ((0, 0), (1, 5), (4, 7), (523, 2)):
        flipped = bytearray(data_v)
        flipped[b] ^= 1 << bit
        ok, fixes, dfx = bch_decode(bytes(flipped), ecc_v)
        _st(ok and fixes == 1 and dfx == [(b ^ 3, 1 << bit)],
            "flight decode of a flip at %d.%d returns ok with the mirrored "
            "fix %d.%d (F19)" % (b, bit, b ^ 3, bit))


def _parse_vectors(path):
    vecs = []
    name = None
    data = None
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("vector:"):
                name = line.split(":", 1)[1].strip()
            elif line.startswith("data:"):
                data = bytes.fromhex(line.split(":", 1)[1].strip())
            elif line.startswith("ecc:"):
                vecs.append((name, data,
                             bytes.fromhex(line.split(":", 1)[1].strip())))
                name = None
                data = None
    return vecs


def _st_image_checks(img, mode_name):
    """Everything that runs against one synthetic image (inverted and
    plain)."""
    name_a = build_name(FILE_TYPES["tmmgmt"], 0, 0x91223344, 0x5678, 0x64)
    name_b = build_name(FILE_TYPES["lut"], 7, 0, 0, 0xFFFF)
    _st(name_a == bytes.fromhex("03001122334456780064"),
        "name A: type 3, sync bit stripped, coarse/fine/duration big-endian")
    pat_a = bytes((i * 13 + 5) & 0xFF for i in range(PAGE_DATA_SIZE))
    pat_b = bytes((i * 29 + 11) & 0xFF for i in range(PAGE_DATA_SIZE))
    _st_write_file(img, 70, name_a, 4094, 1, pat_a)
    _st_write_file(img, 71, name_b, 100, 2, pat_b)

    # the file-name scan (D-X4)
    _st(scan_for_name(img, name_a) == [70], "scan finds file A on block 70")
    _st(scan_for_name(img, name_b) == [71], "scan finds file B on block 71")
    _st(scan_for_name(img, build_name(FILE_TYPES["lut"], 7, 0, 0, 0xFFFE))
        == [], "a wrong duration byte does not match")

    # bad-block marker, heal-free, and the inversion negative
    before = img.alloc_kb()
    op_bad_block(img, [65], False)
    raw0 = img.read_raw(block_page0_offset(65) + PAGE_DATA_SIZE
                        + AUX_BAD_BLOCK_MARKER, 1)
    _st(img.to_guest(raw0) == b"\x00",
        "%s: the marker reads 0 in the guest view" % mode_name)
    # the negative: with the OPPOSITE storage convention the marker byte
    # would read 0xFF = good, so a wrong inversion must fail this check
    wrong = raw0 if img.inverted else bytes([raw0[0] ^ 0xFF])
    _st(wrong != b"\x00",
        "%s: with the wrong inversion the marker is not visible" % mode_name)
    _st(img.alloc_kb() - before <= 8,
        "marker writes allocate at most the two touched pages")
    page0 = img.read_guest(page_offset(65, 0), PAGE_SIZE)
    _st(page0[:PAGE_DATA_SIZE] == b"\xFF" * PAGE_DATA_SIZE
        and page0[PAGE_DATA_SIZE + AUX_EMPTY_PAGE_MARKER] == 0xFF,
        "the rest of page 0 stays erased (heal-free)")
    page1 = img.read_guest(page_offset(65, 1), PAGE_SIZE)
    _st(page1[PAGE_DATA_SIZE + AUX_BAD_BLOCK_MARKER] == 0x00,
        "page 1 carries the marker too")

    # corrupt-file --mode data: 3 flips = the corrected read path
    off_b, page_b = check_file_page(img, 71, 0)
    corrupt_data(img, 71, 0, 0, 3)
    page_b2 = img.read_guest(off_b, PAGE_SIZE)
    diff = [(i, page_b[i] ^ page_b2[i]) for i in range(PAGE_SIZE)
            if page_b[i] != page_b2[i]]
    _st(diff == [(0, 0xE0)],
        "data corruption flips exactly the 3 chosen bits of sub-block 0")
    ok, fixes, data_fixes = bch_decode(subblock_with_ecc_zeroed(page_b2, 0),
                                       stored_ecc(page_b2, 0))
    _st(ok and fixes == 3
        and sorted(data_fixes) == sorted((b ^ 3, m)
                                         for b, m in bit_flips(0, 3)),
        "the flight decode counts 3 fixes (BCH corrected 3 SEUs) but "
        "applies them to the mirrored byte 3 (F19)")
    # 8 flips in sub-block 1: uncorrectable, the 0x543B path
    corrupt_data(img, 71, 0, 1, 8)
    page_b3 = img.read_guest(off_b, PAGE_SIZE)
    ok, _, _ = bch_decode(subblock_with_ecc_zeroed(page_b3, 1),
                          stored_ecc(page_b3, 1))
    _st(not ok, "8 flips make sub-block 1 uncorrectable (0x543B on read)")

    # corrupt-file --mode address on file A: pageAddress rewritten, header
    # sub-block re-encoded, everything else byte-identical
    off_a, page_a = check_file_page(img, 70, 0)
    corrupt_address(img, 70, [0], None, False)
    page_a2 = img.read_guest(off_a, PAGE_SIZE)
    got = int.from_bytes(aux_bytes(page_a2)[AUX_PAGE_ADDRESS:
                                            AUX_PAGE_ADDRESS + 4], "big")
    _st(got == linear_page0(70) + 1,
        "spare pageAddress rewritten to the wrong but valid next page")
    ok, fixes, _ = bch_decode(subblock_with_ecc_zeroed(page_a2, 7),
                              stored_ecc(page_a2, 7))
    _st(ok and fixes == 0,
        "the re-encoded header sub-block decodes clean (the read reaches "
        "the FSW address check, File.c:691-699, not the ECC)")
    diff = [i for i in range(PAGE_SIZE) if page_a[i] != page_a2[i]]
    ecc7_lo = PAGE_DATA_SIZE + AUX_ECC + 7 * FILE_ECC_BCH_SIZE
    _st(all(PAGE_DATA_SIZE + AUX_PAGE_ADDRESS <= i
            < PAGE_DATA_SIZE + AUX_PAGE_ADDRESS + 4
            or ecc7_lo <= i < ecc7_lo + FILE_ECC_BCH_SIZE for i in diff),
        "only pageAddress and the 12 ECC[7] bytes changed")
    _st(scan_for_name(img, name_a) == [70],
        "the scan still finds the file (name and markers untouched)")

    # corrupt-file --mode header on file B: the first page header stops
    # decoding (run after the data flips; the header flips hit the data
    # tail of sub-block 7, not the aux, so the scan still lists the block)
    corrupt_header(img, 71, 8)
    page_b4 = img.read_guest(off_b, PAGE_SIZE)
    ok, _, _ = bch_decode(subblock_with_ecc_zeroed(page_b4, 7),
                          stored_ecc(page_b4, 7))
    _st(not ok, "the header sub-block no longer decodes (file 'does not "
                "exist' at boot, File.c:369-375)")
    _st(aux_bytes(page_b4)[AUX_FILE_NAME:AUX_FILE_NAME + FN_LENGTH] == name_b,
        "the name bytes survive (truth read-back for show)")


def self_test():
    """Fully offline; builds small synthetic sparse images and exercises
    marker/flip/scan/heal-free behaviour, the inversion, the EOF and cap
    guards, the shared-dir refusal, the file-name scan, both addressing
    forms, the address mode end-to-end and the device grammar."""
    t0 = time.time()
    _ST_CHECKS[0] = 0
    _st_bch_section()
    _st_leon_vectors()

    tmp = tempfile.mkdtemp(prefix="stix-flashdmg-selftest-")
    try:
        inv_path = os.path.join(tmp, "flash-inverted.bin")
        pln_path = os.path.join(tmp, "flash-plain.bin")
        # inverted: sparse (holes are erased flash, D-I)
        with open(inv_path, "wb") as f:
            f.truncate(SELFTEST_SIZE)
        # plain: erased pages are 0xFF bytes on disk (the device writes
        # them out in plain mode, grlib_stixflash.c:1000-1009)
        with open(pln_path, "wb") as f:
            f.truncate(SELFTEST_SIZE)
            f.write(b"\xFF" * SELFTEST_SIZE)

        for path, mode_name in ((inv_path, "inverted"), (pln_path, "plain")):
            img = FlashImage(path, inverted=(mode_name == "inverted"))
            try:
                _st_image_checks(img, mode_name)
            finally:
                img.close()

        # both addressing forms must select the same byte (D-X4): the same
        # marker written once per form on two identical fresh images gives
        # byte-identical files
        p1 = os.path.join(tmp, "form-linear.bin")
        p2 = os.path.join(tmp, "form-framework.bin")
        for p in (p1, p2):
            with open(p, "wb") as f:
                f.truncate(SELFTEST_SIZE)
        _st_expect_rc(["bad-block", "--block", "66", p1], 0, "linear form")
        _st_expect_rc(["bad-block", "--mcm", "0", "--chip", "0",
                       "--block", "66", p2], 0, "framework form")
        with open(p1, "rb") as f1, open(p2, "rb") as f2:
            _st(f1.read() == f2.read(),
                "linear and mcm/chip/block forms write identical bytes")

        # EOF guard: block 40000 starts far past this image's EOF
        _st_expect_rc(["bad-block", "--block", "40000", inv_path], 1,
                      "writes past EOF are refused")
        _st(os.path.getsize(inv_path) == SELFTEST_SIZE,
            "the refused write did not grow the file")

        # reserved ASW image area (blocks < 64)
        _st_expect_rc(["bad-block", "--block", "10", inv_path], 1,
                      "block < 64 refused without --allow-reserved")
        _st_expect_rc(["bad-block", "--block", "10", "--allow-reserved",
                       inv_path], 0, "--allow-reserved lifts the rule")

        # allocation cap (STIX_FLASH_MAX_ALLOC_KB, slot.sh:277)
        old_cap = os.environ.get("STIX_FLASH_MAX_ALLOC_KB")
        os.environ["STIX_FLASH_MAX_ALLOC_KB"] = "1"
        try:
            _st_expect_rc(["bad-block", "--block", "66", inv_path], 1,
                          "the sparse cap is enforced")
        finally:
            if old_cap is None:
                del os.environ["STIX_FLASH_MAX_ALLOC_KB"]
            else:
                os.environ["STIX_FLASH_MAX_ALLOC_KB"] = old_cap

        # shared-dir refusal (slot.sh:238-260)
        shared = os.path.join(tmp, "shared")
        os.makedirs(shared)
        in_shared = os.path.join(shared, "flash.bin")
        with open(in_shared, "wb") as f:
            f.truncate(SELFTEST_SIZE)
        old_shared = os.environ.get("STIX_SHARED_DIRS")
        os.environ["STIX_SHARED_DIRS"] = shared
        try:
            _st_expect_rc(["bad-block", "--block", "66", in_shared], 1,
                          "a flash file in a shared dir is refused")
            _st_expect_rc(["bad-block", "--block", "66", "--allow-shared",
                           in_shared], 0, "--allow-shared overrides")
        finally:
            if old_shared is None:
                del os.environ["STIX_SHARED_DIRS"]
            else:
                os.environ["STIX_SHARED_DIRS"] = old_shared

        # --fs-range: markers land on every FS-scanned block that exists,
        # past-EOF blocks are skipped (the framework's BBrokenFlash shape)
        _st_expect_rc(["bad-block", "--fs-range", inv_path], 0, "--fs-range")
        img = FlashImage(inv_path, inverted=True)
        try:
            for b in list(iter_fs_blocks()):
                off = block_page0_offset(b)
                if off >= img.size:
                    break
                aux = img.read_guest(off + PAGE_DATA_SIZE, PAGE_HEADER_SIZE)
                _st(aux[AUX_BAD_BLOCK_MARKER] == 0x00,
                    "fs-range marked block %d" % b)
        finally:
            img.close()

        # the device subcommand: the X-1/X-2 sidecar grammar (O4)
        dev = os.path.join(tmp, "flash.bin.dmg")
        cases = [
            (["--kind", "erase-fail", "--block", "610"],
             {"kind": "erase-fail", "block": 610}),
            (["--kind", "program-fail", "--page", "4483"],
             {"kind": "program-fail", "page": 4483}),
            (["--kind", "stuck", "--mcm", "0", "--chip", "0", "--block",
              "70", "--page", "3", "--offset", "4100", "--bitn", "2",
              "--value", "1"],
             {"kind": "stuck", "mcm": 0, "chip": 0, "block": 70, "page": 3,
              "bit": "4100.2", "value": 1}),
            (["--kind", "stuck", "--page", "4483", "--bit", "100.7",
              "--value", "0"],
             {"kind": "stuck", "page": 4483, "bit": "100.7", "value": 0}),
            (["--kind", "stuck0", "--mcm", "1", "--chip", "0", "--block",
              "64"],
             {"kind": "stuck0", "mcm": 1, "chip": 0, "block": 64}),
        ]
        for i, (extra, want) in enumerate(cases):
            out = dev if i == 0 else "%s.%d" % (dev, i)
            _st_expect_rc(["device"] + extra + ["--out", out], 0,
                          "device emits %s" % want["kind"])
            with open(out) as f:
                doc = json.load(f)
            _st(doc == {"v": 1, "damage": [want]},
                "sidecar %d matches the device grammar" % i)
            _st(validate_spec(doc["damage"][0]) >= RESERVED_BLOCKS,
                "sidecar %d passes the device's own validation" % i)
        bad_device = [
            ["--kind", "stuck", "--page", "4483", "--value", "0"],
            ["--kind", "stuck0", "--block", "610", "--value", "0"],
            ["--kind", "erase-fail", "--page", "4483"],
            ["--kind", "program-fail", "--block", "610"],
            ["--kind", "erase-fail", "--block", "10"],
            ["--kind", "stuck", "--page", "4483", "--bit", "5000.0",
             "--value", "1"],
            ["--kind", "stuck", "--page", "4483", "--bit", "0.8",
             "--value", "1"],
            ["--kind", "stuck", "--page", "4483", "--bit", "0.0",
             "--value", "2"],
        ]
        for extra in bad_device:
            _st_expect_rc(["device"] + extra + ["--out", dev], 1,
                          "device refuses %s" % " ".join(extra))
        # an unknown kind is refused by argparse itself (exit 2)
        _st_expect_rc(["device", "--kind", "weird", "--block", "610",
                       "--out", dev], 2, "device refuses an unknown kind")
        _st_expect_rc(["device", "--kind", "erase-fail", "--block", "610",
                       "--out", dev], 1, "an existing sidecar is protected")
        _st_expect_rc(["device", "--kind", "erase-fail", "--block", "610",
                       "--out", dev, "--force"], 0, "--force replaces it")

        # show on the damaged inverted image (truth read-back after all the
        # damage above: the marker of the fs-range step marks the blocks)
        _st_expect_rc(["show", "--type", "lut", "--spare", "7", "--duration",
                       "0xFFFF", inv_path], 1,
                      "show reports a missing file with rc 1")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("self-test: OK (%d checks, %.2f s)"
          % (_ST_CHECKS[0], time.time() - t0))
    return 0


# ---- --print-vector ---------------------------------------------------------

def print_vector():
    """Emit the BCH(13,7) reference vector for
    scripts/stix/tests/bch13t7-reference-vector.txt (D-X4). The reviewer
    regenerates/cross-checks it once against the unit-test framework's
    BCH.c (BCH_init(); BCH_encode(buf, 528, ecc, 12)) and commits it; the
    self-test pins the encoder against the committed file."""
    _bch_init()
    vectors = [
        ("a5-fill", b"\xA5" * FILE_ECC_SUBBLOCK_SIZE),
        ("ramp7", bytes((i * 7 + 3) & 0xFF
                        for i in range(FILE_ECC_SUBBLOCK_SIZE))),
    ]
    print("# BCH(13,7) reference vectors for scripts/stix/flash-damage.py")
    print("# --self-test (D-X4). Generated by this tool's encoder, a port of")
    print("# the FSW's ASW/src/MemoryMgmt/BCH.c: m=13, t=7, primitive")
    print("# polynomial 0x201b (prim_poly_tab[8], BCH.c:1368-1371), 12 ECC")
    print("# bytes per 528-byte sub-block, MSB-first bit order (BCH.c:309-")
    print("# 310, store_ecc8 BCH.c:214-232). Reviewer: regenerate or")
    print("# cross-check once with the framework's BCH.c, then commit.")
    for name, data in vectors:
        print("vector: %s" % name)
        print("data: %s" % data.hex())
        print("ecc: %s" % bch_encode(data).hex())
    return 0


# ---- command line -----------------------------------------------------------

def _hex_int(text):
    return int(text, 0)


def _spare_arg(text):
    """--spare: a byte, or "any" (match the name ignoring the spare byte;
    the FSW's context names carry CONTEXT_VERSION there, which differs
    between builds: the gate ELF stores 0x22, the source says 35)."""
    if text == "any":
        return None
    return int(text, 0)


def _file_type(text):
    if text in FILE_TYPES:
        return FILE_TYPES[text]
    try:
        value = int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError("unknown file type %r" % text)
    if value not in FILE_TYPES.values():
        raise argparse.ArgumentTypeError("file type %d does not exist "
                                         "(FilesystemDefinition.h:80-92)"
                                         % value)
    return value


def build_parser():
    p = argparse.ArgumentParser(
        prog="flash-damage.py",
        description="Offline NAND flash damage for stixflash images "
                    "(flash damage plan D-X4). Damages a private flash copy "
                    "in place; refuses shared master dirs, the reserved ASW "
                    "image area (blocks < 64), writes past EOF and a "
                    "runaway sparse allocation.")
    p.add_argument("--self-test", action="store_true",
                   help="run the offline self-test (no QEMU, no image)")
    p.add_argument("--print-vector", action="store_true",
                   help="print the BCH(13,7) reference vector for "
                        "scripts/stix/tests/bch13t7-reference-vector.txt")
    sub = p.add_subparsers(dest="cmd")

    def add_image_flags(sp, allow_reserved):
        sp.add_argument("--plain", action="store_true",
                        help="the image is stored un-inverted "
                             "(inverted-storage=off)")
        sp.add_argument("--allow-shared", action="store_true",
                        help="allow a flash file inside the shared master "
                             "dirs (slot.sh stix_run_dir_guard's list)")
        if allow_reserved:
            sp.add_argument("--allow-reserved", action="store_true",
                            help="allow blocks < 64 (the ASW image area)")

    bb = sub.add_parser("bad-block", help="factory bad-block marker 0 into "
                                         "the spares of pages 0 and 1")
    bb.add_argument("--block", type=int,
                    help="linear block, or the block in the chip with "
                         "--mcm/--chip")
    bb.add_argument("--mcm", type=int)
    bb.add_argument("--chip", type=int)
    bb.add_argument("--fs-range", action="store_true",
                    help="mark every FS-scanned block (A: %d-%d, B: %d-%d)"
                         % (FS_A[0], FS_A[1], FS_B[0], FS_B[1]))
    add_image_flags(bb, True)
    bb.add_argument("flash")

    cf = sub.add_parser("corrupt-file",
                        help="locate a stored file by name and damage it")
    cf.add_argument("--type", dest="ftype", type=_file_type, required=True,
                    help="file type: %s or the number"
                         % "|".join(FILE_TYPES))
    cf.add_argument("--spare", type=_spare_arg, default=0,
                    help="name byte 1 (context files: CONTEXT_VERSION=35, "
                         "ContextMgmt.h:11)")
    cf.add_argument("--coarse", type=_hex_int, default=0,
                    help="SCET coarse, u32 big-endian, name bytes 2..5")
    cf.add_argument("--fine", type=_hex_int, default=0,
                    help="SCET fine, u16 big-endian, name bytes 6..7")
    cf.add_argument("--duration", type=_hex_int, default=0,
                    help="duration, u16 big-endian, name bytes 8..9 "
                         "(context files: 0xFFFF, MemFilesystem.c:1032)")
    cf.add_argument("--mode", choices=("header", "data", "address"),
                    required=True,
                    help="header: uncorrectable first-page header; data: "
                         "flips in one sub-block; address: wrong spare "
                         "pageAddress with re-encoded header BCH")
    cf.add_argument("--bits", type=int,
                    help="flips for header/data (default 8; <= 7 = the "
                         "corrected path, > 7 = uncorrectable)")
    cf.add_argument("--page", type=int,
                    help="page in the block (default 0)")
    cf.add_argument("--all-pages", action="store_true",
                    help="address mode: every in-use page of the file")
    cf.add_argument("--sub-block", type=int,
                    help="data mode: the 528-byte sub-block (default 0)")
    cf.add_argument("--to", type=_hex_int,
                    help="address mode: the wrong pageAddress to write "
                         "(default: the page's real address + 1)")
    add_image_flags(cf, True)
    cf.add_argument("flash")

    sh = sub.add_parser("show", help="print the file's block and pages "
                                     "(truth read-back)")
    sh.add_argument("--type", dest="ftype", type=_file_type, required=True)
    sh.add_argument("--spare", type=_spare_arg, default=0,
                    help='a byte, or "any" to ignore the spare byte')
    sh.add_argument("--coarse", type=_hex_int, default=0)
    sh.add_argument("--fine", type=_hex_int, default=0)
    sh.add_argument("--duration", type=_hex_int, default=0)
    add_image_flags(sh, False)
    sh.add_argument("flash")

    dv = sub.add_parser("device",
                        help="write a flash-damage sidecar for the device "
                             "(X-1/X-2 grammar)")
    dv.add_argument("--kind", required=True, choices=SPEC_KINDS,
                    help="stuck0 is the value-0 alias and takes no --value")
    dv.add_argument("--block", type=int,
                    help="linear block, or the block in the chip with "
                         "--mcm/--chip")
    dv.add_argument("--page", type=int,
                    help="linear page, or the page in the chip "
                         "(--mcm/--chip) or in the block (framework form)")
    dv.add_argument("--mcm", type=int)
    dv.add_argument("--chip", type=int)
    dv.add_argument("--bit", help="stuck, page scope: 'byte.bit', byte < 4224")
    dv.add_argument("--offset", type=int,
                    help="framework form: the byte offset in the page; "
                         "resolves to the sidecar's 'offset.bit' with --bitn")
    dv.add_argument("--bitn", type=int, help="bit inside the --offset byte")
    dv.add_argument("--value", type=int, help="stuck value, 0 or 1")
    dv.add_argument("--out", default="flash.bin.dmg")
    dv.add_argument("--force", action="store_true",
                    help="replace an existing sidecar")
    return p


def cmd_bad_block(args):
    if args.fs_range != (args.block is None):
        raise ToolError("bad-block needs --block N (or --mcm M --chip C "
                        "--block B) or --fs-range, not both")
    if (args.mcm is None) != (args.chip is None):
        raise ToolError("--mcm and --chip go together")
    if args.mcm is not None:
        if not 0 <= args.mcm < NUM_MCMS or not 0 <= args.chip < DEVICES_PER_MCM:
            raise ToolError("--mcm must be 0..%d, --chip 0..%d"
                            % (NUM_MCMS - 1, DEVICES_PER_MCM - 1))
        if not 0 <= args.block < BLOCKS_PER_DEVICE:
            raise ToolError("--block with --mcm/--chip is the block in the "
                            "chip, 0..%d" % (BLOCKS_PER_DEVICE - 1))
        lin = block_join(args.mcm, args.chip, args.block)
    else:
        lin = args.block
    img = open_image(args)
    try:
        if args.fs_range:
            op_bad_block(img, list(iter_fs_blocks()), args.allow_reserved,
                         fs_range=True)
        else:
            if not 0 <= lin < TOTAL_BLOCKS:
                raise ToolError("linear block must be 0..%d"
                                % (TOTAL_BLOCKS - 1))
            op_bad_block(img, [lin], args.allow_reserved)
    finally:
        img.close()
    return 0


def cmd_corrupt_file(args):
    any_spare = args.spare is None
    name = build_name(args.ftype, 0 if any_spare else args.spare,
                      args.coarse, args.fine, args.duration)
    if args.mode == "header":
        if (args.page is not None or args.all_pages
                or args.sub_block is not None or args.to is not None):
            raise ToolError("--mode header works on page 0's header "
                            "sub-block only")
    elif args.mode == "data":
        if args.all_pages or args.to is not None:
            raise ToolError("--all-pages/--to are address-mode options")
    else:
        if args.bits is not None or args.sub_block is not None:
            raise ToolError("--bits/--sub-block are header/data options")
        if args.all_pages and args.page is not None:
            raise ToolError("--all-pages and --page are mutually exclusive")
    img = open_image(args)
    try:
        lin_block = locate_file(img, name, any_spare)
        if args.mode == "header":
            corrupt_header(img, lin_block,
                           8 if args.bits is None else args.bits)
        elif args.mode == "data":
            page = 0 if args.page is None else args.page
            sub = 0 if args.sub_block is None else args.sub_block
            if not 0 <= page < PAGES_PER_BLOCK:
                raise ToolError("--page must be 0..%d" % (PAGES_PER_BLOCK - 1))
            if not 0 <= sub < FILE_ECC_SUBBLOCKS:
                raise ToolError("--sub-block must be 0..%d"
                                % (FILE_ECC_SUBBLOCKS - 1))
            corrupt_data(img, lin_block, page, sub,
                         8 if args.bits is None else args.bits)
        else:
            if args.all_pages:
                pages = in_use_pages(img, lin_block)
                if not pages:
                    raise ToolError("file %s has no in-use pages on block %d"
                                    % (name_hex(name), lin_block))
            else:
                pages = [0 if args.page is None else args.page]
            corrupt_address(img, lin_block, pages, args.to,
                            args.allow_reserved)
    finally:
        img.close()
    return 0


def cmd_show(args):
    any_spare = args.spare is None
    name = build_name(args.ftype, 0 if any_spare else args.spare,
                      args.coarse, args.fine, args.duration)
    img = open_image(args)
    try:
        op_show(img, name, any_spare)
    finally:
        img.close()
    return 0


def main(argv):
    args = build_parser().parse_args(argv)
    try:
        if args.self_test:
            return self_test()
        if args.print_vector:
            return print_vector()
        if args.cmd == "bad-block":
            return cmd_bad_block(args)
        if args.cmd == "corrupt-file":
            return cmd_corrupt_file(args)
        if args.cmd == "show":
            return cmd_show(args)
        if args.cmd == "device":
            return op_device(args)
        build_parser().print_help()
        return 2
    except ToolError as e:
        print("flash-damage: %s" % e, file=sys.stderr)
        return 1
    except _SelfTestFailure as e:
        print("self-test: FAIL: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

