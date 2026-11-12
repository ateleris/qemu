#!/usr/bin/env bash
# FLASH-DAMAGE smoke: the flash-damage gate (flash damage plan X-4/X-4b,
# docs/stix-port/2026-11-12-flash-damage-plan.md). Boots the STIX model on
# a private copy of the master flash (slot mode only) and proves the
# FSW-visible outcomes of NAND damage. The master already carries the two
# context files (correction of the 2026-11-12 live finding: a plain boot
# on a file-LESS flash creates them unconditionally — ContextMgmt_init's
# else branch, ContextMgmt.c:1977-1982 — and sends 0x8030 (1,1), seen on
# fresh flashes; the master's copies show NO 0x8030 and 0x0032
# used=0002 = exactly those two files, so the golden capture's own boot
# stored them and every later boot loads them, ContextMgmt.c:1892-1903).
# The seed therefore does not create the files, it CHANGES a context
# parameter so the NOMINAL-entry store re-writes them. Every part that
# needs stored files first runs a SEED, discovers its files on the
# seeded flash and boots the run under test on that same flash. Three
# parts by default; a seeded part is two QEMU runs (seed + the run):
#
#   seed (the shared seed_part helper; one QEMU run in the part's dir):
#     fresh private copy of the master (stix_flash_private), boot, drive
#     CONF -> S20=0 -> TC(236,6) param -> NOMINAL. The param TC sets
#     E_PARAM_ID_CONFIG_SPW_THRESHOLD_CURRENT 3826 -> 4095 (TC(236,6)
#     IDPU param 6, HWConfig.c:121-123; in the context via
#     ContextMgmt_c2ptable, ContextMgmt.c:1176; default/range
#     ParamMgmtDef.h:186), so at NOMINAL entry the deinit's CRC gate
#     (ContextMgmt.c:2075-2083) fires and store_new_context re-writes
#     both files onto FRESH blocks (File_findFreeBlock starts after the
#     last written one, File.c:159-163). TC_S20 stays: it pins the TC20
#     keep-alive reaction off for the long NOMINAL wait (nominal-smoke's
#     reason); against the stored context it is a CRC no-op — the stored
#     ReactionToS20 is already 0 (the S20 TC completed live with
#     TM(1,7), so the master's context was written by an earlier S20=0
#     run; that no-op is exactly why the first seed attempt stored
#     nothing). Then wait for the context write: CONTEXT1's
#     logicalWriteTime (read-only show on the live private flash) above
#     the pristine copy's AND stable for 8 s [L] (du is no signal: the
#     re-store frees the old blocks, so the allocation stays ~16 KB
#     before and after — the live misread of the first seed run), then quit
#     QEMU cleanly over QMP (`quit`) so the file is complete on disk.
#     TM(5,3) 0x8030 "Context file(s) not found" (1,1) marks the
#     boot-path re-create (ContextMgmt.c:1982, TMForm.c:651-658) and is
#     reported when seen (NOT expected on the master's copies — the
#     files are found at boot); the NOMINAL-entry store emits no TM
#     (ContextMgmt.c:2065-2090), so the logicalWriteTime poll is the wait condition.
#     The discovery then finds the files by name with the tool's
#     read-only show ON THE SEEDED FLASH — the name's spare byte is
#     the build's CONTEXT_VERSION (ContextMgmt.h:11 says 35; the gate
#     ELF stores 0x22), so show matches it with --spare any (the
#     tool's default spare is 0; a bare
#     "show --type context0" finds nothing even when the file is
#     there — the live misdiagnosis). The LUT leg: when no LUT file
#     exists after the seed (expected on the gate ELF), it is skipped
#     loudly, as before.
#
#   boot (offline tool, edits the seeded flash BEFORE QEMU):
#     bad-block --block 610 (TMMGMT, blocks 604-783) + corrupt-file
#     --mode header on a LUT file (when one exists). No context damage
#     here: a context read error at boot is fatal (F20, the addr and
#     stuck parts). Expect (order free):
#       TM(5,2) 0x5439 <lutType u8> <lutId u16> for the LUT (only when a
#       LUT file exists after the seed; on AX a missing LUT file alone
#       sends none — the golden UART has the four "could not find LUT"
#       lines and no 0x5439) [L]
#       TM(5,1) 0x0032 {status 01, used 0002, bad 0001, error 0001|0000}
#       [L; used = 0002: the master's two context files themselves
#       ([G] live: 0x0032 used=0002 at boot, no 0x8030; CONTEXT_LENGTH
#       4094 fits one block, ContextMgmt.h:12, FilesystemAPI.c:36); the
#       seed's re-store frees the old blocks and writes fresh ones, so
#       the count stays 2; free unpinned; error=0001 only with the LUT
#       damaged (its block used->error); the probe run freezes the
#       seeded counts]
#   check (device, runtime, over QMP; one QEMU run, NO -no-reboot; the
#     flash is the part's seeded one):
#     boot; damage-add program-fail on page 0 of the first free block
#     after the context files (show-derived on the seeded flash), read
#     back with qom-get damage-list; page 0 because a context file is one
#     page and the store programs nothing else (the plan's page 2 never
#     fired live); CONF -> S20=0 ->
#     TC(236,6) param 4094 -> NOMINAL (the param TC differs from the
#     seeded context's 4095, so the run's NOMINAL-entry store fires —
#     OpModeMgmt.c:317, ContextMgmt.c:2075-2083 — and that store burns
#     the block: File_write marks it bad, retries elsewhere);
#     TC(238,6) {0, 3839, 0} (TCData.h:429-431,
#     badBlockHandling=0) -> TM(238,7) on APID 0x5A8 (PID_90, category
#     diagnostics, TM_238.c:151-180), bad_after=1 [L, from the probe log];
#     then a guest reset in-process (QMP system_reset — the same
#     qemu_system_reset_request the watchdog/FORCE path makes; the entry
#     survives it, X-2) and the second boot's 0x0032 shows
#     bad = boot-1's bad + 1 [L; the plan's "reboot -> 0x0032 bad=1"
#     persistence point, D-X1. badBlockHandling=0 because with 1 the
#     check erases the grown-bad block, which a program-fail-only block
#     survives, so it heals before the reset — seen live]
#   addr (offline tool on the seeded flash): corrupt-file --mode address
#     on CONTEXT1 page 0 (pageAddress + 1, header BCH re-encoded in the
#     FLIGHT form, F19). The boot scan accepts the block; the context
#     read reaches the address check (File.c:694-698) and its 0x543B
#     report hits the uncreated TMBuffer: FSW finding F20 — INIT waits
#     for the context reads (MemFilesystem.c:542-544, :1038-1051) before
#     TMBuffer_init (main.c:136-140). Expect (live, [G]): QEMU exits by
#     itself (-no-reboot), the UART has "address corrupt", "ASSERT
#     FAILED: ...TMBuffer.c" and no "init: TM buffer". On hardware the
#     reset repeats every boot (a reboot loop).
#   stuck (offline tool on the seeded flash; = the X-4b "uncorrectable"
#     flavour's boot):
#     corrupt-file --mode data --bits 8 on CONTEXT1 page 0 sub-block 0;
#     the same F20 crash with "Unrecoverable error in file" (File.c:673)
#
# X-4b flavours (--flavour; default: none of them runs):
#   correctable     corrupt-file --bits N (STIX_FLASHDMG_CORRECTABLE_BITS,
#                   default 3; 5 = the remove-and-rewrite variant) on a
#                   TMMGMT file (TMBuffer.c:715 naming): both boots (one
#                   QEMU run, in-process reset) are clean (no 0x543B,
#                   forbidden) and the "BCH corrected N SEUs" TRACE of the
#                   restore read is on the UART [L]. The FSW deletes
#                   restored TMBuffer backups once the queues drain
#                   (TMBuffer.c:891-896), so the reboot leg only re-reads
#                   the file if it survived; at bits 5 the file must move
#                   to a fresh block or be gone (deleted, the rewrite
#                   unobservable) — never stay put [L]. SKIP (77) when no
#                   TMMGMT file exists — discovered on a fresh private
#                   copy, NOT seeded: the seed cannot create one
#                   (TMBuffer writes those only during a link outage).
#   uncorrectable   the stuck part's boot (the plan reuses it; seeded
#                   like it).
#   broken          bad-block --fs-range (the framework's BBrokenFlash):
#                   needs no stored files and runs unseeded. 0x0032
#                   {01, 0000, 0f00, 0000, f100} [L], 0x8030 (1,1)
#                   [L] and 0x5439 [L].
#   full            program-fail on the FIRST pages of the two free
#                   blocks after the context files (show-derived on the
#                   seeded flash); the run's param TC (4094 vs the seeded
#                   4095) makes the NOMINAL-entry store fire and it
#                   burns both (one File_write, review finding 2) and
#                   the MemFilesystem retry succeeds; TM(238,7)
#                   bad_after=2 [L]. The plan's filled/FS_FULL leg needs
#                   soak-scale writes and is NOT gated here.
#
# --negative runs the boot part UNDAMAGED in <base>-boot-neg (seeded
# like it) and passes only when the probe fails with exactly the boot
# part's damage expectations unmatched and nothing else (fault-smoke's
# "expected failure seen").
#
# Every part keeps its own private flash and run dir
# <base>-<part>[-neg]-s<k>: seed_part creates the flash there,
# discover_files shows it and the run under test boots it;
# boot_and_probe never re-copies a seeded flash. The master
# ($STIX_FLASH_MASTER) is only the copy's source (it carries the two
# context files — see above), so no show ever needs to run on it.
# Every [L] payload is confirmed by a live probe
# run before it freezes; the page addresses come from show on the
# seeded flash at run time, the rest are the plan's numbers.
#
# QMP read-back of damage-list greps the probe log's "-> return" line
# (this worktree's tmtc-probe.py has no --expect-return yet; that lands
# with the attenuator probe and can replace the grep).
#
# Usage: flash-damage-smoke.sh <qemu-binary> [--negative]
#          [--flavour correctable|uncorrectable|broken|full]
#          [--dry-run] [--check-guard]
#   STIX_ASW_ELF                    the flavour ELF; absent: SKIP, exit 77
#   STIX_FLASHDMG_RUN_DIR           run-dir base (default
#                                   /tmp/stix-flashdmg-run); parts append
#                                   -<part>[-neg], slot mode -s<k>
#   STIX_FLASHDMG_BOOT_SECONDS      boot part (default 75)
#   STIX_FLASHDMG_CHECK_SECONDS     check part (default 240)
#   STIX_FLASHDMG_STUCK_SECONDS     stuck part (default 90)
#   STIX_FLASHDMG_FLAVOUR_SECONDS   flavours (default 150)
#   STIX_FLASHDMG_SEED_SECONDS      the seed's store wait, >= 120
#                                   (default 360)
#   STIX_FLASHDMG_CORRECTABLE_BITS  the correctable flavour's --bits
#                                   (default 3; 5 = the rewrite variant)
#   STIX_PORT_BASE                  REQUIRED for real runs (slot mode:
#                                   the private flash copy); --dry-run and
#                                   --check-guard work without it
#   STIX_FLASH_MASTER               the private copy's source (slot.sh);
#                                   it carries the master's context files
#                                   (see the header), so the stored files
#                                   are discovered on the seeded flash,
#                                   never here
#   --dry-run prints the parts' seed, damage and QEMU/probe command
#             lines, boots nothing and calls no show (the seeded files
#             exist only after a seed ran).
#   --check-guard only runs the run-dir guards and exits 0/2.
#
# Exit codes: 0 pass (or dry-run / check-guard ok), 1 a failed
#             expectation or a wrong failure, 2 environment problem
#             (a failed seed or tool step included), 77 SKIP (no ELF;
#             the correctable flavour without its target file).
set -euo pipefail

QEMU_BIN_ARG="${1:?usage: flash-damage-smoke.sh <qemu-binary> \
[--negative] [--flavour correctable|uncorrectable|broken|full] \
[--dry-run] [--check-guard]}"
shift
NEGATIVE=0
DRY_RUN=0
CHECK_GUARD=0
FLAVOUR=""
while [ $# -gt 0 ]; do
    case "$1" in
        --negative)
            [ "$NEGATIVE" -eq 0 ] || { echo "--negative given twice"; exit 2; }
            NEGATIVE=1 ;;
        --flavour)
            [ -z "$FLAVOUR" ] || { echo "--flavour given twice"; exit 2; }
            [ $# -ge 2 ] || { echo "--flavour needs a value"; exit 2; }
            FLAVOUR="$2"
            shift ;;
        --flavour=*)
            [ -z "$FLAVOUR" ] || { echo "--flavour given twice"; exit 2; }
            FLAVOUR="${1#--flavour=}" ;;
        --dry-run)
            [ "$DRY_RUN" -eq 0 ] || { echo "--dry-run given twice"; exit 2; }
            DRY_RUN=1 ;;
        --check-guard)
            [ "$CHECK_GUARD" -eq 0 ] || {
                echo "--check-guard given twice"; exit 2; }
            CHECK_GUARD=1 ;;
        *) echo "unknown argument: $1"; exit 2 ;;
    esac
    shift
done
case "$FLAVOUR" in
    "" | correctable | uncorrectable | broken | full) ;;
    *) echo "unknown flavour: $FLAVOUR (correctable|uncorrectable|broken|full)"
       exit 2 ;;
esac
if [ "$NEGATIVE" -eq 1 ] && [ -n "$FLAVOUR" ]; then
    echo "--negative runs the boot part only; drop --flavour"
    exit 2
fi

. "$(dirname "${BASH_SOURCE[0]}")/lib/paths.sh"
ASW_ELF="${STIX_ASW_ELF:-$STIX_DEFAULT_ELF}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOL="$REPO_ROOT/scripts/stix/flash-damage.py"
PROBE="$REPO_ROOT/scripts/stix/tmtc-probe.py"
BASE_DIR="${STIX_FLASHDMG_RUN_DIR:-/tmp/stix-flashdmg-run}"
BOOT_SECS="${STIX_FLASHDMG_BOOT_SECONDS:-75}"
CHECK_SECS="${STIX_FLASHDMG_CHECK_SECONDS:-240}"
STUCK_SECS="${STIX_FLASHDMG_STUCK_SECONDS:-90}"
FLAV_SECS="${STIX_FLASHDMG_FLAVOUR_SECONDS:-150}"
SEED_SECS="${STIX_FLASHDMG_SEED_SECONDS:-360}"
CORR_BITS="${STIX_FLASHDMG_CORRECTABLE_BITS:-3}"
# Slot mode (STIX_PORT_BASE, scripts/stix/lib/slot.sh): the slot's port
# and run dir <dir>-s<k>; unset = legacy, 20000+uid and the dir as is.
. "$REPO_ROOT/scripts/stix/lib/slot.sh"
# dry-run, check-guard and the ELF-absent SKIP boot nothing: they peek at
# the slot (no lock, no wait, no disk guard)
if [ "$DRY_RUN" -eq 1 ] || [ "$CHECK_GUARD" -eq 1 ] ||
        [ ! -f "$ASW_ELF" ]; then
    stix_slot_setup peek
else
    stix_slot_setup
fi

BASE_DIR="$(realpath -m -- "$BASE_DIR")"
SHARED_UPREV="$(realpath -m -- "$STIX_UPREV_DIR")"
SHARED_OLD="$(realpath -m -- /tmp/stix-uprev-run)"
SHARED_BASE="$(realpath -m -- /tmp/stix-baseline-run)"
guard_dir() {  # dir: refuse the shared flash dirs, their parents, and /
    local d="$1" shared
    d="$(realpath -m -- "$d")"
    if [ "$d" = "/" ]; then
        echo "refusing run dir $1 (it is the root directory)"
        exit 2
    fi
    for shared in "$SHARED_UPREV" "$SHARED_OLD" "$SHARED_BASE"; do
        case "$d" in
            "$shared" | "$shared"/*)
                echo "refusing run dir $1 (it is $shared or inside it;" \
                    "the shared flashes must stay untouched)"
                exit 2 ;;
        esac
        case "$shared" in
            "$d"/*)
                echo "refusing run dir $1 (it is a parent of the shared" \
                    "flash dir $shared)"
                exit 2 ;;
        esac
    done
}
# every part's dir, guarded: the parts boot in their own dirs so one
# part's evidence never overwrites another's
for part in boot boot-neg check stuck corr corr2 brk full; do
    guard_dir "$BASE_DIR-$part"
done

if [ "$CHECK_GUARD" -eq 1 ]; then
    echo "GUARD OK: run dirs under $BASE_DIR (canonical," \
        "<part>[-neg][, slot mode -s<k>])"
    exit 0
fi

QEMU_BIN="$(realpath -e "$QEMU_BIN_ARG")" || {
    echo "qemu binary not found: $QEMU_BIN_ARG"; exit 2; }
[ -x "$QEMU_BIN" ] || { echo "not executable: $QEMU_BIN"; exit 2; }

for v in "$BOOT_SECS" "$CHECK_SECS" "$STUCK_SECS" "$FLAV_SECS" "$SEED_SECS"; do
    case "$v" in
        '' | *[!0-9]*)
            echo "STIX_FLASHDMG_*_SECONDS must be whole seconds (got: $v)"
            exit 2
            ;;
    esac
done
if [ "$SEED_SECS" -lt 120 ]; then
    echo "STIX_FLASHDMG_SEED_SECONDS must be >= 120 (the mode drive" \
        "alone ends at +80 s; got: $SEED_SECS)"
    exit 2
fi
case "$CORR_BITS" in
    3 | 5) ;;
    *) echo "STIX_FLASHDMG_CORRECTABLE_BITS must be 3 or 5 (got: $CORR_BITS)"
       exit 2 ;;
esac

[ -f "$ASW_ELF" ] || {
    echo "SKIP: ASW ELF absent ($ASW_ELF); set STIX_ASW_ELF to enable"
    exit 77
}

# Real runs need slot mode: the offline damage edits a private copy of
# the master (stix_flash_private), and legacy mode has no private copy.
if [ "$DRY_RUN" -eq 0 ] && [ "$STIX_SLOT_MODE" -eq 0 ]; then
    echo "flash-damage-smoke needs slot mode (STIX_PORT_BASE=auto or a" \
        "slot number): the offline damage must edit a private copy of" \
        "the master flash (stix_flash_private), never the master itself"
    exit 2
fi

command -v python3 > /dev/null || { echo "python3 is required"; exit 2; }
MASTER="${STIX_FLASH_MASTER:-$STIX_UPREV_DIR/flash.bin}"
[ -f "$MASTER" ] || {
    echo "the master flash $MASTER is missing; run capture-golden.sh first"
    exit 2
}

# ---- TC units ----------------------------------------------------------
# TC_CONF/TC_S20/TC_NOM are nominal-smoke.sh's bytes verbatim (the FSW
# accepts them live). The param TC and TC(238,6) are built here with the
# same CRC-16/CCITT-FALSE routine as att-smoke.sh.
#
# The param TC is TC(236,6) HWConfig_config (TcService236.c:108-115):
# app data {subsystem u8 @0, param u8 @+1, value u16 BE @+2}
# (TCData.h:142-144). It sets E_PARAM_ID_CONFIG_SPW_THRESHOLD_CURRENT
# (IDPU=1, param 6, HWConfig.h:11+31) — the SpW overcurrent trip
# threshold, in the context via ContextMgmt_c2ptable
# (ContextMgmt.c:1176), allowed in CONF (TCVerifDynamic.c:66, value
# range 0..0xFFF, TCVerifDynamic.c:361+651-667). Default 3826, max
# 0xFFF (ParamMgmtDef.h:186). Raising it is harmless: the SW re-arm
# (OpModeMgmt.c:308) and the HW comparator only trip LATER; no mode
# change, no HK-report limit. The seed sets 4095 (0xFFF) so its
# NOMINAL-entry deinit store fires against the master's stored 3826;
# the check/full runs set 4094 (0xFFE) so THEIR store fires against the
# seeded 4095 (that store is what burns the injected blocks).
TC_CONF="40 02 00 00 1D AC C0 01 00 07 19 EC 01 00 04 00 4A C1"
TC_S20="40 02 00 00 1D AC C0 02 00 09 19 EC 06 00 07 04 00 00 AF BA"
TC_NOM="40 02 00 00 1D AC C0 03 00 07 19 EC 01 00 05 00 BF 97"
eval "$(python3 <<'PYEOF'
def crc16(sp):
    crc = 0xFFFF
    for b in sp:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc

def unit(seq, svc, sub, app):
    # DFH: PUS v1, ack 9 (acceptance + completion), service, subtype,
    # source 0; length = DFH + CRC + app - 1
    sp = bytes([0x1D, 0xAC, 0xC0 | (seq >> 8), seq & 0xFF,
                0, len(app) + 5, 0x19, svc, sub, 0]) + bytes(app)
    c = crc16(sp)
    return "40 02 00 00 " + " ".join("%02X" % b for b in sp) + \
        " %02X %02X" % (c >> 8, c & 0xFF)

# seq 4: the TM(1,1) ack 1DACC004 is distinct from the CONF/S20/NOM ones
# badBlockHandling 0: with 1 the check erases the grown-bad block, and a
# program-fail-only block erases fine, so it heals before the reset (live:
# bad_after=1, then 0x0032 bad=0 on boot 2, block 67 all 0xFF)
print('TC_238_6="%s"' % unit(4, 0xEE, 6, [0x00, 0x00, 0x0E, 0xFF, 0x00]))
# the param TCs: the seed's seq 4 acks 1DACC004 (the seed sends no
# TC_238_6); the runs' seq 5 acks 1DACC005, keeping TC_238_6's seq 4.
# No sequence-count ordering is checked (TCVerifStatic.c:106 verifies
# only the flags), so seed order C001,C002,C004,C003 is fine.
print('TC_236_6_SEED="%s"' % unit(4, 0xEC, 6, [0x01, 0x06, 0x0F, 0xFF]))
print('TC_236_6_RUN="%s"' % unit(5, 0xEC, 6, [0x01, 0x06, 0x0F, 0xFE]))
# TC(236,1) mode changes {mode u8, 0}: TC_NOM's own builder check, then
# NOMINAL -> SAFE -> MAINTENANCE for the check part's TC(238,6), which is
# MAINTENANCE-only (TCVerifDynamic.c:97, else TM(1,8) 48301 "wrong
# operation mode"); MAINTENANCE is entered only from SAFE
# (OpModeMgmt.c:86-95)
print('TC_NOM_BUILT="%s"' % unit(3, 0xEC, 1, [0x05, 0x00]))
print('TC_SAFE="%s"' % unit(6, 0xEC, 1, [0x02, 0x00]))
print('TC_MAINT="%s"' % unit(7, 0xEC, 1, [0x03, 0x00]))
PYEOF
)"
case "$TC_238_6" in
    "40 02 00 00 1D AC C0 04 00 0A 19 EE 06 00 00 00 0E FF 00 "*) ;;
    *) echo "flash-damage-smoke: the TC builder does not reproduce the" \
           "TC(238,6) {0, 3839, 0} unit ($TC_238_6)"
       exit 2 ;;
esac
[ "$TC_NOM_BUILT" = "$TC_NOM" ] || {
    echo "flash-damage-smoke: the TC builder does not reproduce TC_NOM" \
        "($TC_NOM_BUILT vs $TC_NOM)"
    exit 2
}
case "$TC_236_6_SEED" in
    "40 02 00 00 1D AC C0 04 00 09 19 EC 06 00 01 06 0F FF "*) ;;
    *) echo "flash-damage-smoke: the TC builder does not reproduce the" \
           "TC(236,6) {1, 6, 0xFFF} seed unit ($TC_236_6_SEED)"
       exit 2 ;;
esac
case "$TC_236_6_RUN" in
    "40 02 00 00 1D AC C0 05 00 09 19 EC 06 00 01 06 0F FE "*) ;;
    *) echo "flash-damage-smoke: the TC builder does not reproduce the" \
           "TC(236,6) {1, 6, 0xFFE} run unit ($TC_236_6_RUN)"
       exit 2 ;;
esac

# ---- discovery: read-only show on the SEEDED flash, per part -----------
# The master carries the two context files (see the header), but every
# seeded part shows ITS OWN ./flash.bin after seed_part — never the
# master. The tool refuses shared dirs; the part dirs are private, so
# no --allow-shared is needed anywhere anymore.
show_file() {  # show args... -> stdout; ./flash.bin in the part dir
    python3 "$TOOL" show "$@" ./flash.bin
}
block_of_show() {  # show stdout -> the file's linear block
    sed -n 's/^file 0x[0-9a-fA-F]*: block \([0-9]*\) .*/\1/p'
}
page0_of_show() {  # show stdout -> page 0's linear page
    sed -n 's/^  page 0 (linear \([0-9]*\)).*/\1/p'
}

discover_files() {  # tag: show the seeded ./flash.bin (runs in its dir)
    local tag="$1" out
    # the context files: type 6/7 (the enum values; the header's comments
    # say 8/9), duration 0xFFFF, spare = the build's CONTEXT_VERSION, hence
    # --spare any (ContextMgmt.h:11, MemFilesystem.c:1032)
    CTX0_BLOCK="$(show_file --type context0 --spare any --duration 0xffff \
        | block_of_show)" || {
        echo "FLASH-DAMAGE-SMOKE: ERROR (the seeded flash in $PWD has no" \
            "CONTEXT0: show failed — its output above tells what the FS" \
            "ranges hold; the seed's store did not land as assumed)" \
            "${STIX_SLOT_TAG}"
        exit 2
    }
    CTX1_SHOW="$(show_file --type context1 --spare any --duration 0xffff)" || {
        echo "FLASH-DAMAGE-SMOKE: ERROR (the seeded flash in $PWD has no" \
            "CONTEXT1: show failed — its output above tells)${STIX_SLOT_TAG}"
        exit 2
    }
    CTX1_BLOCK="$(printf '%s\n' "$CTX1_SHOW" | block_of_show)"
    CTX1_PAGE0="$(printf '%s\n' "$CTX1_SHOW" | page0_of_show)"
    if [ -z "$CTX0_BLOCK" ] || [ -z "$CTX1_BLOCK" ] || [ -z "$CTX1_PAGE0" ]; then
        echo "FLASH-DAMAGE-SMOKE: ERROR (cannot parse the show output for" \
            "the context files (blocks '$CTX0_BLOCK'/'$CTX1_BLOCK', page 0" \
            "'$CTX1_PAGE0') — the seeded flash's layout or the tool" \
            "changed)${STIX_SLOT_TAG}"
        exit 2
    fi

    # a LUT file, if the seed left one: the boot loads types 1..4 with the
    # id from the context (0 on the gate ELF) and copies 0..3 in the
    # fine-low byte (LUTMgmt.c:1111-1124)
    LUT_FOUND=0
    LUT_TYPE=""
    LUT_ARGS=""
    LUT_BLOCK=""
    for t in 1 2 3 4; do
        for f in 0 1 2 3; do
            if out="$(show_file --type lut --spare any --coarse 0 --fine "$f" \
                    --duration "$t" 2>/dev/null)"; then
                LUT_FOUND=1
                LUT_TYPE="$t"
                LUT_ARGS="--type lut --spare any --coarse 0 --fine $f --duration $t"
                LUT_BLOCK="$(printf '%s\n' "$out" | block_of_show)"
                break 2
            fi
        done
    done
    if [ "$LUT_FOUND" -eq 1 ]; then
        echo "discovery ($tag): LUT file found: $LUT_ARGS (block $LUT_BLOCK)"
    else
        echo "discovery ($tag): no LUT file after the seed (expected on" \
            "the gate ELF); the boot part skips the 0x5439 leg and pins" \
            "0x0032 error=0000"
    fi

    # the check/full injection block: File_findFreeBlock continues after the
    # context file's node 0 (the plan, File.c:163), so the first block after
    # CONTEXT0's — skipping the other context's block — is where the next
    # context store draws from; it must stay inside CB0 (blocks 64..123,
    # CONTEXT0's partition; review resolution 1)
    INJ_BLOCK=$((CTX0_BLOCK + 1))
    while [ "$INJ_BLOCK" -eq "$CTX0_BLOCK" ] || [ "$INJ_BLOCK" -eq "$CTX1_BLOCK" ]; do
        INJ_BLOCK=$((INJ_BLOCK + 1))
    done
    if [ "$INJ_BLOCK" -gt 123 ]; then
        echo "FLASH-DAMAGE-SMOKE: ERROR (the computed injection block" \
            "$INJ_BLOCK is outside CB0 (64..123); the seeded flash's" \
            "context layout is not what the plan expects — inspect with" \
            "show)${STIX_SLOT_TAG}"
        exit 2
    fi
    # check part: page 0 of the block. A context file is one page (4094 B),
    # so the store programs page 0 only; page 2 (the plan's choice, to
    # keep the marker pages 0/1 clean) is never programmed and its
    # program-fail never fires (live: the store landed on block 67 page 0,
    # 0x0032 bad stayed 0). The FSW marks a failed block in pages 0 AND 1,
    # and the scan checks both, so the failing page-0 marker write still
    # leaves page 1's marker.
    CHECK_PAGE=$((INJ_BLOCK * 64))
    # full flavour: the FIRST pages of the two consecutive free blocks (the
    # plan's X-4b full text; the counts are RAM-live there, no reboot leg)
    FULL_B1="$INJ_BLOCK"
    FULL_B2=$((INJ_BLOCK + 1))
    while [ "$FULL_B2" -eq "$CTX0_BLOCK" ] || [ "$FULL_B2" -eq "$CTX1_BLOCK" ]; do
        FULL_B2=$((FULL_B2 + 1))
    done
    if [ "$FULL_B2" -gt 123 ]; then
        echo "FLASH-DAMAGE-SMOKE: ERROR (the full flavour's second block" \
            "$FULL_B2 is outside CB0)${STIX_SLOT_TAG}"
        exit 2
    fi
    FULL_P1=$((FULL_B1 * 64))
    FULL_P2=$((FULL_B2 * 64))
    qmp_args
    echo "discovery ($tag): CONTEXT0 block $CTX0_BLOCK, CONTEXT1 block" \
        "$CTX1_BLOCK (page 0 linear $CTX1_PAGE0); injection block" \
        "$INJ_BLOCK (check page $CHECK_PAGE = page 0, the page a one-page" \
        "context store programs)"

    # ---- the boot part's expectations (shared with --negative) ----
    # The context-file damage is NOT here: a context read error at boot
    # is fatal (FSW finding F20, the addr and stuck parts).
    # [L] payloads: used = 0002 (the master's two context files — the seed's re-store
    # replaces them block-for-block), bad = the boot part's factory-bad
    # block 610, error = 0001 only with the LUT damaged; the probe run
    # freezes the seeded counts.
    if [ "$LUT_FOUND" -eq 1 ]; then
        # [L] 0x5439 LUT not found; payload {lutType u8, lutId u16}
        EXP_5439="0x5A7:5:2:$(printf '5439%02x0000' "$LUT_TYPE")@60"
        EXP_0032="0x5A7:5:1:003201000200010001@60"
    else
        EXP_5439=""
        EXP_0032="0x5A7:5:1:003201000200010000@60"
    fi
    WANT="$(printf '%s\n' ${EXP_5439:+"$EXP_5439"} "$EXP_0032" \
        | sort | paste -sd' ')"
}

# ---- QMP command strings (rebuilt per part from the seeded layout) -----
qom_flash_set() {  # property value-json
    printf '{"execute": "qom-set", "arguments": {"path": "%s", "property": "%s", "value": %s}}' \
        /machine/stixflash "$1" "$2"
}
spec_value() {  # linear page -> the X-2 JSON object string (quoted)
    local inner
    inner="$(printf '{"kind": "program-fail", "page": %d}' "$1")"
    inner="${inner//\"/\\\"}"
    printf '"%s"' "$inner"
}
qmp_args() {  # rebuild the runtime damage-add strings from the pages
    QMP_ADD_CHECK="$(qom_flash_set damage-add "$(spec_value "$CHECK_PAGE")")"
    QMP_LIST='{"execute": "qom-get", "arguments": {"path": "/machine/stixflash", "property": "damage-list"}}'
    QMP_ADD_FULL1="$(qom_flash_set damage-add "$(spec_value "$FULL_P1")")"
    QMP_ADD_FULL2="$(qom_flash_set damage-add "$(spec_value "$FULL_P2")")"
}
QMP_RESET='{"execute": "system_reset"}'

boot_args() {  # the boot part; the negative reuses it undamaged
    # EXP_5439/EXP_0032/WANT come from discover_files on the
    # part's seeded flash (or from the dry-run preamble)
    PROBE_ARGS=(
        --port "$TMTC_PORT" --seconds "$BOOT_SECS" --window 20
        --qmp "$QMP_SOCK"
        --expect "0x5A7:5:1:14a3@30"
    )
    if [ "$LUT_FOUND" -eq 1 ]; then
        PROBE_ARGS+=(--expect "$EXP_5439")   # [L] lutType/lutId
    fi
    PROBE_ARGS+=(
        --expect "$EXP_0032"          # [L] used/bad/error: the seeded counts
        --hold
    )
}

check_args() {  # the check part; one QEMU run, in-process reset
    PROBE_ARGS=(
        --port "$TMTC_PORT" --seconds "$CHECK_SECS" --window 20
        --qmp "$QMP_SOCK"
        --expect "0x5A7:5:1:14a3@30"
        --expect "0x5A7:5:1:0032@50"  # boot-1's 0x0032 (the bad baseline)
        --send-at 35 --qmp-cmd "$QMP_ADD_CHECK"
        --send-at 36 --qmp-cmd "$QMP_LIST"   # read-back: check_damage_list
        --send-at 40 --send-hex "$TC_CONF"
        --expect "0x5A1:1:1:1DACC001@10"
        --expect "0x5A7:5:1:00210204@10"
        --send-at 45 --send-hex "$TC_S20"
        --expect "0x5A1:1:1:1DACC002@10"
        --expect "0x5A1:1:7:1DACC002@10"
        --send-at 50 --send-hex "$TC_236_6_RUN"
        --expect "0x5A1:1:1:1DACC005@10"
        --expect "0x5A1:1:7:1DACC005@10"
        --send-at 55 --send-hex "$TC_NOM"
        --expect "0x5A1:1:1:1DACC003@20"
        --expect "0x5A7:5:1:00210405@20"  # NOMINAL entry: the store runs
                                          # here (param 4094 vs seeded 4095)
        --send-at 95 --send-hex "$TC_SAFE"
        --expect "0x5A1:1:1:1DACC006@10"
        --expect "0x5A7:5:1:00210502@10"
        --send-at 100 --send-hex "$TC_MAINT"
        --expect "0x5A1:1:1:1DACC007@10"
        --expect "0x5A7:5:1:00210203@10"
        --send-at 105 --send-hex "$TC_238_6"   # MAINTENANCE-only
        --expect "0x5A1:1:1:1DACC004@40"
        --expect "0x5A1:1:7:1DACC004@40"
        --send-at 145 --qmp-cmd "$QMP_RESET"
        --expect "0x5A7:5:1:003201@90"    # [L] boot 2 after the reset
        --hold
    )
}


corr_args() {  # the correctable flavour; one QEMU run, in-process reset
    PROBE_ARGS=(
        --port "$TMTC_PORT" --seconds "$FLAV_SECS" --window 20
        --qmp "$QMP_SOCK"
        --expect "0x5A7:5:1:14a3@30"
        --forbid "0x5A7:5:2:543b"     # the corrected read must not fail
        --send-at 40 --qmp-cmd "$QMP_RESET"
        --expect "0x5A7:5:1:14a3@60"  # boot 2 after the reset
        --hold
    )
}

brk_args() {  # the broken-flash flavour (BBrokenFlash)
    PROBE_ARGS=(
        --port "$TMTC_PORT" --seconds "$FLAV_SECS" --window 20
        --qmp "$QMP_SOCK"
        --expect "0x5A7:5:3:80300101@90"                  # [L] (1,1)
        --expect "0x5A7:5:2:5439@90"                      # [L] any payload
        --expect "0x5A7:5:1:00320100000f000000f100@90"    # [L] the plan's row
        --hold
    )
}

full_args() {  # the full-FS flavour's gateable leg (bad-block growth)
    PROBE_ARGS=(
        --port "$TMTC_PORT" --seconds "$FLAV_SECS" --window 20
        --qmp "$QMP_SOCK"
        --expect "0x5A7:5:1:14a3@30"
        --expect "0x5A7:5:1:0032@50"
        --send-at 35 --qmp-cmd "$QMP_ADD_FULL1"
        --send-at 36 --qmp-cmd "$QMP_ADD_FULL2"
        --send-at 40 --send-hex "$TC_CONF"
        --expect "0x5A1:1:1:1DACC001@10"
        --expect "0x5A7:5:1:00210204@10"
        --send-at 45 --send-hex "$TC_S20"
        --expect "0x5A1:1:1:1DACC002@10"
        --expect "0x5A1:1:7:1DACC002@10"
        --send-at 50 --send-hex "$TC_236_6_RUN"
        --expect "0x5A1:1:1:1DACC005@10"
        --expect "0x5A1:1:7:1DACC005@10"
        --send-at 55 --send-hex "$TC_NOM"
        --expect "0x5A1:1:1:1DACC003@20"
        --expect "0x5A7:5:1:00210405@20"
        --send-at 95 --send-hex "$TC_238_6"
        --expect "0x5A1:1:1:1DACC004@40"
        --expect "0x5A1:1:7:1DACC004@40"
        --hold
    )
}

run_tool() {  # offline damage on the private copy; a tool refusal is
              # an environment problem
    echo "+ flash-damage.py $*"
    python3 "$TOOL" "$@" || {
        echo "FLASH-DAMAGE-SMOKE: ERROR (flash-damage.py failed on" \
            "$*; the seeded flash may lack the expected file)${STIX_SLOT_TAG}"
        exit 2
    }
}

offline_damage() {  # tag: edit ./flash.bin OFFLINE, before QEMU
    case "$1" in
        boot)
            run_tool bad-block --block 610 ./flash.bin
            if [ "$LUT_FOUND" -eq 1 ]; then
                run_tool corrupt-file $LUT_ARGS --mode header ./flash.bin
            fi
            ;;
        addr)
            run_tool corrupt-file --type context1 --spare any \
                --duration 0xffff --mode address --page 0 ./flash.bin
            ;;
        stuck)
            run_tool corrupt-file --type context1 --spare any \
                --duration 0xffff --mode data --bits 8 --page 0 \
                --sub-block 0 ./flash.bin
            ;;
        corr)
            run_tool corrupt-file $TMMGMT_ARGS --mode data \
                --bits "$CORR_BITS" --page 0 --sub-block 0 ./flash.bin
            ;;
        brk)
            run_tool bad-block --fs-range ./flash.bin
            ;;
    esac
}

PROBE_PID=""
QEMU_PID=""
cleanup() {
    # `|| true`: under set -e a failed kill (process already gone) would
    # otherwise become the script's exit status
    if [ -n "$QEMU_PID" ]; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if [ -n "$PROBE_PID" ]; then
        kill "$PROBE_PID" 2>/dev/null || true
        wait "$PROBE_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

REBOOTABLE=0   # 1: no -no-reboot (the guest reset happens in-process)

check_qmp_sock() {  # the QMP unix socket path must stay under 108 bytes
    if [ "${#QMP_SOCK}" -ge 108 ]; then
        echo "refusing run dir $PART_DIR (the QMP socket path is" \
            "${#QMP_SOCK} bytes; a unix socket path must stay under 108)"
        exit 2
    fi
}

boot_and_probe() {  # tag argfn secs [seeded]: one boot in <BASE_DIR>-<tag>
    local tag="$1" argfn="$2" secs="$3" seeded="${4:-0}"
    PART_DIR="$(stix_slot_dir "$BASE_DIR-$tag")"
    guard_dir "$PART_DIR"
    mkdir -p "$PART_DIR"
    cd "$PART_DIR"
    QMP_SOCK="$PART_DIR/qmp.sock"
    check_qmp_sock
    if [ "$seeded" = seeded ]; then
        # the seed left ./flash.bin with its stored files: keep it, the
        # discovery already showed it and offline_damage edits it below
        [ -f flash.bin ] || {
            echo "FLASH-DAMAGE-SMOKE: ERROR (the seeded flash.bin is gone" \
                "from $PART_DIR)${STIX_SLOT_TAG}"
            exit 2
        }
        rm -f "uart.$tag.raw" "uart.$tag.txt" "$tag.probe.log" \
            "qemu-stdout.$tag.log" qmp.sock
    else
        rm -f flash.bin "uart.$tag.raw" "uart.$tag.txt" "$tag.probe.log" \
            "qemu-stdout.$tag.log" qmp.sock
        stix_flash_private
        [ -f flash.bin ] || {
            echo "flash.bin missing — the master flash copy failed"
            exit 2
        }
    fi
    offline_damage "$tag"
    QEMU_RUN_ARGS=(-M leon3_generic -m 64M -display none -monitor none)
    [ "$REBOOTABLE" -eq 1 ] || QEMU_RUN_ARGS+=(-no-reboot)
    "$argfn"
    PROBE_PID=""
    QEMU_PID=""
    timeout "$((secs + 30))" python3 -u "$PROBE" "${PROBE_ARGS[@]}" \
        > "$tag.probe.log" 2>&1 &
    PROBE_PID=$!
    timeout "$((secs + 30))" "$QEMU_BIN" \
        "${QEMU_RUN_ARGS[@]}" \
        "${STIX_SLOT_QEMU_ARGS[@]}" \
        -qmp "unix:$QMP_SOCK,server=on,wait=off" \
        -serial "file:$PART_DIR/uart.$tag.raw" \
        -kernel "$ASW_ELF" > "qemu-stdout.$tag.log" 2>&1 &
    QEMU_PID=$!
    set +e
    wait "$PROBE_PID"
    PROBE_RC=$?
    PROBE_PID=""
    kill -0 "$QEMU_PID" 2>/dev/null
    QEMU_ALIVE=$?
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null
    QEMU_RC=$?
    QEMU_PID=""
    set -e
    # On every path the log tail comes BEFORE the verdict.
    if [ "$QEMU_ALIVE" -ne 0 ]; then
        tail -5 "qemu-stdout.$tag.log"
        echo "FLASH-DAMAGE-SMOKE: ERROR (QEMU died or never started in" \
            "$tag; qemu rc=$QEMU_RC)${STIX_SLOT_TAG}"
        exit 2
    fi
    if [ "$PROBE_RC" -eq 124 ]; then
        tail -5 "$tag.probe.log"
        echo "FLASH-DAMAGE-SMOKE: ERROR (the $tag probe hit its own" \
            "timeout)${STIX_SLOT_TAG}"
        exit 2
    fi
    if [ "$PROBE_RC" -eq 2 ]; then
        tail -5 "$tag.probe.log"
        echo "FLASH-DAMAGE-SMOKE: ERROR (the $tag probe rejected its" \
            "arguments)${STIX_SLOT_TAG}"
        exit 2
    fi
    if grep -q "could not connect" "$tag.probe.log" 2>/dev/null; then
        tail -5 "$tag.probe.log"
        echo "FLASH-DAMAGE-SMOKE: ERROR (the $tag probe never connected;" \
            "QEMU rc=$QEMU_RC)${STIX_SLOT_TAG}"
        exit 2
    fi
}

why_failed() {  # probe log -> the failure reason, by kind
    local why
    why="$(grep -m1 -o 'FORBIDDEN TM: .*' "$1" | cut -d' ' -f3-)" || true
    if [ -n "$why" ]; then echo "forbidden: $why"; return; fi
    why="$(grep -m1 -o 'QMP ERROR: .*' "$1" | cut -d' ' -f3-)" || true
    if [ -n "$why" ]; then echo "qmp error: $why"; return; fi
    why="$(grep -o 'EXPECTATION NOT MATCHED: .*' "$1" \
        | cut -d' ' -f4- | paste -sd' ')" || true
    if [ -n "$why" ]; then echo "not matched: $why"; return; fi
    if grep -q '^CONNECTION CLOSED' "$1"; then
        echo "connection closed"; return
    fi
    echo "probe rc=$PROBE_RC; see the part's probe log"
}

uart_ok() {  # tag: the boot banner, NULs stripped
    tr -d '\000' < "uart.$1.raw" > "uart.$1.txt"
    grep -q "init: SAFE mode" "uart.$1.txt"
}

tm_apps() {  # log apid type subtype -> the app-data hex of each match
    awk -v pat="TM apid=0x$2 type=$3 subtype=$4 " '
        index($0, pat) {
            for (i = 1; i <= NF; i++)
                if ($i ~ /^data=[0-9a-f]+$/) print substr($i, 46)
        }' "$1"
}
# the probe's data= field is the whole unit from byte 0; TM application
# data starts at unit byte 20 (4 SpW + 6 SP + 10 TM data field header,
# TMData.h:56) = hex char 41 of the unit = char 46 of "data=<hex>"

check_damage_list() {  # page...: the injection(s) in the QMP read-back
    local p want=0
    for p in "$@"; do
        if grep -qF "src=runtime kind=program-fail page=$p" \
                check.probe.log; then
            want=$((want + 1))
        fi
    done
    [ "$want" -eq "$#" ] || {
        echo "FLASH-DAMAGE-SMOKE: FAIL (check part: damage-list read-back" \
            "shows $want of $# injected specs)${STIX_SLOT_TAG}"
        exit 1
    }
    echo "CHECK damage-list: ok ($# spec(s) active)"
}

check_tm2387() {  # log want-bad-after: the FS check's ground readout [L]
    local ba
    ba="$(tm_apps "$1" 5A8 238 7 | tail -1 | cut -c25-28)"
    if [ -z "$ba" ]; then
        echo "FLASH-DAMAGE-SMOKE: FAIL (no TM(238,7) on 0x5A8 in $1)" \
            "${STIX_SLOT_TAG}"
        exit 1
    fi
    if [ "$ba" != "$2" ]; then
        echo "FLASH-DAMAGE-SMOKE: FAIL (TM(238,7) bad_after=$ba, want $2)" \
            "${STIX_SLOT_TAG}"
        exit 1
    fi
    echo "CHECK tm2387: ok (bad_after=$ba)"
}

check_0032_delta() {  # log: the second boot's bad = the first's + 1 [L]
    local b1 b2
    b1="$(tm_apps "$1" 5A7 5 1 | grep '^0032' | head -1 | cut -c11-14)" || true
    b2="$(tm_apps "$1" 5A7 5 1 | grep '^0032' | tail -1 | cut -c11-14)" || true
    if [ -z "$b1" ] || [ -z "$b2" ]; then
        echo "FLASH-DAMAGE-SMOKE: FAIL (0x0032 payload(s) missing in $1)" \
            "${STIX_SLOT_TAG}"
        exit 1
    fi
    if [ $((16#$b2)) -ne $((16#$b1 + 1)) ]; then
        echo "FLASH-DAMAGE-SMOKE: FAIL (0x0032 bad after the reset is" \
            "$((16#$b2)), want $((16#$b1 + 1)); the grown marker did not" \
            "survive to the second boot)${STIX_SLOT_TAG}"
        exit 1
    fi
    echo "CHECK 0032-persistence: ok (bad $((16#$b1)) -> $((16#$b2)))"
}

# ---- the seed (the shared helper; every seeded part calls seed_part) ----
seed_args() {  # the seed run's probe args (constants, no show data)
    local win=$((SEED_SECS - 5)) nom_win=$((SEED_SECS - 75))
    SEED_PROBE_ARGS=(
        --port "$TMTC_PORT" --seconds "$SEED_SECS" --window 20
        --expect "0x5A7:5:1:14a3@$win"
        --expect "0x5A7:5:1:0032@$win"         # FS init finished (scan end)
        --send-at 50 --send-hex "$TC_CONF"     # nominal-smoke's drive
        --expect "0x5A1:1:1:1DACC001@10"
        --expect "0x5A7:5:1:00210204@10"
        --send-at 60 --send-hex "$TC_S20"
        --expect "0x5A1:1:1:1DACC002@10"
        --send-at 65 --send-hex "$TC_236_6_SEED"  # the store driver: SPW
                                                  # threshold 3826 -> 4095
        --expect "0x5A1:1:1:1DACC004@10"
        --expect "0x5A1:1:7:1DACC004@10"
        --send-at 70 --send-hex "$TC_NOM"
        --expect "0x5A1:1:1:1DACC003@10"
        --expect "0x5A7:5:1:00210405@$nom_win"  # NOMINAL entry: the store
        --hold
    )
}

qmp_quit() {  # sock: negotiate, then `quit`, so QEMU flushes and exits
    python3 - "$1" <<'PYEOF'
import json, socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(10)
s.connect(sys.argv[1])
f = s.makefile("rw", newline="\n")
json.loads(f.readline())                                   # the greeting
f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush()
json.loads(f.readline())
f.write(json.dumps({"execute": "quit"}) + "\n"); f.flush()
PYEOF
}

ctx1_lwt() {  # ./flash.bin -> CONTEXT1 page 0's logicalWriteTime, or 0
    python3 "$TOOL" show --type context1 --spare any --duration 0xffff \
        ./flash.bin 2>/dev/null \
        | sed -n 's/^  page 0 (linear [0-9]*):.*logicalWriteTime \([0-9]*\).*/\1/p' \
        | head -1 | grep . || echo 0
}

wait_seed_store() {  # log pristine-lwt: 0 = the store landed and settled
    # [L] the wait condition: CONTEXT1's logicalWriteTime (read with the
    # tool's read-only show on the live private flash) above the pristine
    # copy's AND stable for 4 polls (8 s). CONTEXT1 is written after
    # CONTEXT0 (ContextMgmt.c:2065-2090), so its new name marks the end of
    # the store. The file's allocation (du) is NOT a signal: the re-store
    # programs fresh blocks but frees the old ones (File.c:159-163), and
    # the model's erase deallocates them, so du stays put (live: 16 KiB
    # before and after a landed store). The store cannot run before the
    # async FS init task releases mutexAPI (MemFilesystem.c:1018-1070), so
    # there is no fixed offset to sleep to; the NOMINAL-entry store emits
    # no TM. The first 45 polls (~90 s) are a floor so the
    # CONF->S20->param->NOMINAL drive (last TC at +70) always goes out
    # before the quit.
    local log="$1" pristine="$2" i=0 lwt stable=0 last=0
    local polls=$((SEED_SECS / 2))
    local min_i=$(( polls > 45 ? 45 : polls - 1 ))
    while [ "$i" -lt "$polls" ]; do
        sleep 2
        kill -0 "$QEMU_PID" 2>/dev/null || return 1
        lwt="$(ctx1_lwt)"
        if [ "$lwt" -gt "$pristine" ] && [ "$i" -ge "$min_i" ]; then
            if [ "$lwt" -eq "$last" ]; then
                stable=$((stable + 1))
                if [ "$stable" -ge 4 ]; then
                    SEED_LWT="$lwt"
                    return 0
                fi
            else
                stable=0
            fi
        fi
        last="$lwt"
        i=$((i + 1))
    done
    return 1
}

seed_part() {  # tag: fresh private flash + boot + the mode drive + the
               # context store, then QMP quit; one QEMU run in
               # <BASE_DIR>-<tag>; the flash is left for the discovery
               # and the run under test
    local tag="$1" pristine_lwt
    PART_DIR="$(stix_slot_dir "$BASE_DIR-$tag")"
    guard_dir "$PART_DIR"
    mkdir -p "$PART_DIR"
    cd "$PART_DIR"
    QMP_SOCK="$PART_DIR/qmp.sock"
    check_qmp_sock
    rm -f flash.bin uart.seed.raw uart.seed.txt seed.probe.log \
        qemu-stdout.seed.log qmp.sock
    stix_flash_private
    [ -f flash.bin ] || {
        echo "flash.bin missing — the master flash copy failed"
        exit 2
    }
    pristine_lwt="$(ctx1_lwt)"
    [ "$pristine_lwt" -gt 0 ] || {
        echo "FLASH-DAMAGE-SMOKE: ERROR (the private flash copy has no" \
            "CONTEXT1 — the master is expected to carry both context" \
            "files)${STIX_SLOT_TAG}"
        exit 2
    }
    seed_args
    PROBE_PID=""
    QEMU_PID=""
    timeout "$((SEED_SECS + 30))" python3 -u "$PROBE" \
        "${SEED_PROBE_ARGS[@]}" > seed.probe.log 2>&1 &
    PROBE_PID=$!
    timeout "$((SEED_SECS + 30))" "$QEMU_BIN" \
        -M leon3_generic -m 64M -no-reboot -display none -monitor none \
        "${STIX_SLOT_QEMU_ARGS[@]}" \
        -qmp "unix:$QMP_SOCK,server=on,wait=off" \
        -serial "file:$PART_DIR/uart.seed.raw" \
        -kernel "$ASW_ELF" > qemu-stdout.seed.log 2>&1 &
    QEMU_PID=$!
    if ! wait_seed_store seed.probe.log "$pristine_lwt"; then
        tail -5 seed.probe.log
        tail -5 qemu-stdout.seed.log
        echo "FLASH-DAMAGE-SMOKE: ERROR (the seed's context store never" \
            "landed in $tag: CONTEXT1 logicalWriteTime stayed at" \
            "${pristine_lwt} or QEMU" \
            "died; SEED_SECONDS=$SEED_SECS)${STIX_SLOT_TAG}"
        exit 2
    fi
    if ! qmp_quit "$QMP_SOCK"; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
        QEMU_PID=""
        tail -5 qemu-stdout.seed.log
        echo "FLASH-DAMAGE-SMOKE: ERROR (the seed's QMP quit failed" \
            "in $tag)${STIX_SLOT_TAG}"
        exit 2
    fi
    set +e
    wait "$QEMU_PID"
    QEMU_RC=$?
    wait "$PROBE_PID"
    PROBE_RC=$?
    QEMU_PID=""
    PROBE_PID=""
    set -e
    if [ "$QEMU_RC" -ne 0 ]; then
        tail -5 qemu-stdout.seed.log
        echo "FLASH-DAMAGE-SMOKE: ERROR (the seed's QEMU did not quit" \
            "cleanly in $tag; qemu rc=$QEMU_RC)${STIX_SLOT_TAG}"
        exit 2
    fi
    tr -d '\000' < uart.seed.raw > uart.seed.txt
    grep -q "init: SAFE mode" uart.seed.txt || {
        echo "FLASH-DAMAGE-SMOKE: ERROR (the seed's UART has no 'init:" \
            "SAFE mode' banner)${STIX_SLOT_TAG}"
        exit 1
    }
    if grep -Eq 'storing new context[01] failed' uart.seed.txt; then
        grep -Em1 'storing new context[01] failed' uart.seed.txt
        echo "FLASH-DAMAGE-SMOKE: ERROR (the seed's context write failed" \
            "on the UART)${STIX_SLOT_TAG}"
        exit 1
    fi
    if [ -n "$(tm_apps seed.probe.log 5A7 5 3 \
            | grep '^80300101' || true)" ]; then
        echo "seed ($tag): 0x8030 (1,1) seen (the boot-path re-create," \
            "ContextMgmt.c:1982 + TMForm.c:651-658; not expected on the" \
            "master's copies — its context files are found at boot)"
    else
        echo "seed ($tag): no 0x8030 (expected: the master's context" \
            "files are found at boot, ContextMgmt.c:1892-1903; the" \
            "seed's store is the NOMINAL-entry deinit, which emits no" \
            "TM — OpModeMgmt.c:317, ContextMgmt.c:2065-2090)"
    fi
    echo "seed ($tag): ok (CONTEXT1 logicalWriteTime ${pristine_lwt} ->" \
        "${SEED_LWT}, QEMU quit cleanly)${STIX_SLOT_TAG}"
}

dry_run_part() {  # tag secs argfn needs-seed: the part's commands
    local dir needs_seed="${4:-0}"
    dir="$(stix_slot_dir "$BASE_DIR-$1")"
    QMP_SOCK="$dir/qmp.sock"   # the argfns embed it in the probe line
    if [ "$needs_seed" -eq 1 ]; then
        dry_seed_lines "$dir"
    fi
    dry_damage_lines "$1"
    "$3"
    printf '(cd %s && timeout %s %s -M leon3_generic -m 64M%s\
 -display none -monitor none%s -qmp unix:%s/qmp.sock,server=on,wait=off\
 -serial file:%s/uart.%s.raw -kernel %s > qemu-stdout.%s.log 2>&1)\n' \
        "$dir" "$(($2 + 30))" "$QEMU_BIN" \
        "$([ "$REBOOTABLE" -eq 1 ] || printf ' -no-reboot')" \
        "${STIX_SLOT_QEMU_ARGS[*]:+ ${STIX_SLOT_QEMU_ARGS[*]}}" \
        "$dir" "$dir" "$1" "$ASW_ELF" "$1"
    printf 'python3 -u %s ' "$PROBE"
    printf '%q ' "${PROBE_ARGS[@]}"
    printf '\n'
}

dry_crash_part() {  # tag secs what: a crash part's commands (no probe)
    local dir
    dir="$(stix_slot_dir "$BASE_DIR-$1")"
    dry_seed_lines "$dir"
    dry_damage_lines "$1"
    printf '(cd %s && timeout %s %s -M leon3_generic -m 64M -no-reboot\
 -display none -monitor none%s -qmp unix:%s/qmp.sock,server=on,wait=off\
 -serial file:%s/uart.%s.raw -kernel %s > qemu-stdout.%s.log 2>&1)\n' \
        "$dir" "$(($2 + 30))" "$QEMU_BIN" \
        "${STIX_SLOT_QEMU_ARGS[*]:+ ${STIX_SLOT_QEMU_ARGS[*]}}" \
        "$dir" "$dir" "$1" "$ASW_ELF" "$1"
    echo "# F20: QEMU must exit by itself within $2 s; the UART must show" \
        "'$3', 'ASSERT FAILED: ...TMBuffer.c' (0x543B in its backtrace when one is printed) and no" \
        "'init: TM buffer'"
}

dry_seed_lines() {  # dir: the seed's QEMU + probe + discovery commands
    local dir="$1"
    seed_args
    echo "# seed: fresh private flash in $dir, boot +" \
        "CONF->S20->TC(236,6) param->NOMINAL," \
        "then wait for the store (CONTEXT1 logicalWriteTime above the pristine copy AND" \
        "stable for 8 s; 0x8030 (1,1) reported when seen) and QMP quit"
    printf '(cd %s && timeout %s %s -M leon3_generic -m 64M -no-reboot\
 -display none -monitor none%s -qmp unix:%s/qmp.sock,server=on,wait=off\
 -serial file:%s/uart.seed.raw -kernel %s > qemu-stdout.seed.log 2>&1)\n' \
        "$dir" "$((SEED_SECS + 30))" "$QEMU_BIN" \
        "${STIX_SLOT_QEMU_ARGS[*]:+ ${STIX_SLOT_QEMU_ARGS[*]}}" \
        "$dir" "$dir" "$ASW_ELF"
    printf 'python3 -u %s ' "$PROBE"
    printf '%q ' "${SEED_PROBE_ARGS[@]}"
    printf '\n'
    printf 'python3 %s show --type context0 --spare any --duration 0xffff %s/flash.bin\n' \
        "$TOOL" "$dir"
    printf 'python3 %s show --type context1 --spare any --duration 0xffff %s/flash.bin\n' \
        "$TOOL" "$dir"
    echo "# (the LUT/TMMGMT discovery and the injection pages come from" \
        "show on the seeded flash; the dry run calls no show)"
}

dry_damage_lines() {  # tag: the offline tool calls (constants only)
    case "$1" in
        boot)
            echo "+ flash-damage.py bad-block --block 610 ./flash.bin"
            echo "# (when the seeded flash has a LUT file:" \
                "flash-damage.py corrupt-file <lut name args> --mode" \
                "header ./flash.bin)"
            ;;
        addr)
            echo "+ flash-damage.py corrupt-file --type context1 --spare" \
                "any --duration 0xffff --mode address --page 0 ./flash.bin"
            ;;
        stuck)
            echo "+ flash-damage.py corrupt-file --type context1 --spare" \
                "any --duration 0xffff --mode data --bits 8 --page 0" \
                "--sub-block 0 ./flash.bin"
            ;;
        corr)
            echo "# discovery: flash-damage.py show --type tmmgmt --spare P" \
                "--coarse I --fine 0 --duration V ./flash.bin (P/I/V =" \
                "0..3/0..1/0..1; SKIP 77 when absent)"
            echo "+ flash-damage.py corrupt-file --type tmmgmt <tmmgmt" \
                "name args> --mode data --bits $CORR_BITS --page 0" \
                "--sub-block 0 ./flash.bin"
            ;;
        brk)
            echo "+ flash-damage.py bad-block --fs-range ./flash.bin"
            ;;
        check|full)
            echo "# no offline damage: the damage goes in at runtime over" \
                "QMP (the pages come from show on the seeded flash)"
            ;;
        boot-neg)
            echo "# no damage: the negative boots the seeded flash undamaged"
            ;;
    esac
}

if [ "$DRY_RUN" -eq 1 ]; then
    # The seeded files exist only after a seed ran: the show-derived
    # payloads print as prefix expectations and the injection pages are
    # the plan's provisional ones (block 65 = the first free block when
    # CONTEXT0 lands on block 64); the real run recomputes all of it
    # from show on the seeded flash.
    EXP_5439=""
    EXP_0032="0x5A7:5:1:0032@60"      # + used/bad/error, from show
    LUT_FOUND=0
    INJ_BLOCK=65; CHECK_PAGE=4160; FULL_B1=65; FULL_B2=66
    FULL_P1=4160; FULL_P2=4224
    qmp_args
    echo "DRY RUN — the commands flash-damage-smoke would run" \
        "(negative=$NEGATIVE, flavour=${FLAVOUR:-none}):"
    if [ "$NEGATIVE" -eq 1 ]; then
        dry_run_part boot-neg "$BOOT_SECS" boot_args 1
    elif [ -z "$FLAVOUR" ]; then
        REBOOTABLE=0
        dry_run_part boot "$BOOT_SECS" boot_args 1
        REBOOTABLE=1
        dry_run_part check "$CHECK_SECS" check_args 1
        REBOOTABLE=0
        dry_crash_part addr "$STUCK_SECS" "address corrupt"
        dry_crash_part stuck "$STUCK_SECS" "Unrecoverable error in file"
    else
        case "$FLAVOUR" in
            correctable)
                REBOOTABLE=1
                dry_run_part corr "$FLAV_SECS" corr_args 0 ;;
            uncorrectable)
                REBOOTABLE=0
                dry_crash_part stuck "$STUCK_SECS" \
                    "Unrecoverable error in file" ;;
            broken)
                REBOOTABLE=0
                dry_run_part brk "$FLAV_SECS" brk_args 0 ;;
            full)
                REBOOTABLE=0
                dry_run_part full "$FLAV_SECS" full_args 1 ;;
        esac
    fi
    exit 0
fi

if ss -ltn | grep -q ":${TMTC_PORT} "; then
    echo "TMTC port ${TMTC_PORT} busy — another QEMU instance" \
        "running?${STIX_SLOT_TAG}"
    exit 2
fi

crash_part() {  # tag secs what: a seeded boot whose context damage must
    # crash the ASW at boot (FSW finding F20): FSInit makes INIT wait for
    # the FILE task's context reads (MemFilesystem.c:542-544, :1038-1051),
    # all before TMBuffer_init (main.c:136-140); the read error's 0x543B
    # report (MemFilesystem.c:589-591) hits the uncreated TMBuffer
    # semaphores -> ASSERT (TMBuffer.c:276, arg2 = the event ID) ->
    # Halting -> trap -> reset, which -no-reboot turns into a QEMU exit.
    # Pinned: QEMU exits by itself within secs, the UART has the FSW's
    # read-error line ($3), the TMBuffer ASSERT, and no
    # "init: TM buffer" (the crash precedes it).
    local tag="$1" secs="$2" what="$3" i=0 u
    PART_DIR="$(stix_slot_dir "$BASE_DIR-$tag")"
    guard_dir "$PART_DIR"
    cd "$PART_DIR"
    [ -f flash.bin ] || {
        echo "FLASH-DAMAGE-SMOKE: ERROR (the seeded flash.bin is gone" \
            "from $PART_DIR)${STIX_SLOT_TAG}"
        exit 2
    }
    rm -f "uart.$tag.raw" "uart.$tag.txt" "qemu-stdout.$tag.log" qmp.sock
    offline_damage "$tag"
    QMP_SOCK="$PART_DIR/qmp.sock"
    QEMU_PID=""
    timeout "$((secs + 30))" "$QEMU_BIN" \
        -M leon3_generic -m 64M -no-reboot -display none -monitor none \
        "${STIX_SLOT_QEMU_ARGS[@]}" \
        -qmp "unix:$QMP_SOCK,server=on,wait=off" \
        -serial "file:$PART_DIR/uart.$tag.raw" \
        -kernel "$ASW_ELF" > "qemu-stdout.$tag.log" 2>&1 &
    QEMU_PID=$!
    while [ "$i" -lt "$secs" ] && kill -0 "$QEMU_PID" 2>/dev/null; do
        sleep 1
        i=$((i + 1))
    done
    if kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
        QEMU_PID=""
        tr -d '\000' < "uart.$tag.raw" > "uart.$tag.txt"
        tail -5 "uart.$tag.txt"
        echo "FLASH-DAMAGE-SMOKE: FAIL ($tag: the ASW was still running" \
            "after ${secs} s — the F20 boot crash did not happen)" \
            "${STIX_SLOT_TAG}"
        exit 1
    fi
    set +e
    wait "$QEMU_PID"
    QEMU_RC=$?
    set -e
    QEMU_PID=""
    tr -d '\000' < "uart.$tag.raw" > "uart.$tag.txt"
    u="uart.$tag.txt"
    if ! grep -q "$what" "$u" \
            || ! grep -q "ASSERT FAILED: .*TMBuffer.c" "$u" \
            || grep -q "init: TM buffer" "$u"; then
        tail -12 "$u"
        echo "FLASH-DAMAGE-SMOKE: FAIL ($tag: QEMU exited (rc $QEMU_RC)" \
            "but the UART is not the F20 crash: want '$what', the" \
            "TMBuffer ASSERT, no 'init: TM buffer')${STIX_SLOT_TAG}"
        exit 1
    fi
    # the ASSERT's backtrace (with 0x543B in some frame) is printed only
    # sometimes — none at all in one live run, arg2 of frame 1 or arg1
    # next to the page address in others — so it is reported, not pinned
    local bt="no backtrace printed"
    grep -Eq "arg[0-9]=0x0000543B" "$u" && bt="0x543B in the backtrace"
    echo "CHECK $tag: F20 boot crash ($(grep -m1 -o "$what" "$u");" \
        "ASSERT TMBuffer ($bt) before 'init: TM buffer'; QEMU" \
        "exited rc $QEMU_RC)"
}

fail_part() {  # tag: the log tail then the verdict
    tail -5 "$1.probe.log"
    echo "FLASH-DAMAGE-SMOKE: FAIL ($1: $(why_failed "$1.probe.log")" \
        ")${STIX_SLOT_TAG}"
    exit 1
}

if [ "$NEGATIVE" -eq 1 ]; then
    seed_part boot-neg
    discover_files boot-neg
    boot_and_probe boot-neg boot_args "$BOOT_SECS" seeded
    if [ "$PROBE_RC" -eq 1 ]; then
        MISSED="$(grep -o 'EXPECTATION NOT MATCHED: .*' boot-neg.probe.log \
            | cut -d' ' -f4- | sort | paste -sd' ')" || true
        if [ "$MISSED" = "$WANT" ] && \
                grep -q 'EXPECT OK: 0x5A7:5:1:14a3' boot-neg.probe.log && \
                ! grep -q 'FORBIDDEN TM: \|QMP ERROR: \|RETURN MISMATCH' \
                    boot-neg.probe.log && \
                uart_ok boot-neg; then
            tail -5 boot-neg.probe.log
            echo "expected failure seen: not matched: $MISSED"
            stix_slot_cleanup 0 flash.bin
            echo "FLASH-DAMAGE-SMOKE (negative): PASS (boot part" \
                "undamaged: the damage expectations are not vacuous)" \
                "${STIX_SLOT_TAG}"
            exit 0
        fi
    fi
    tail -5 boot-neg.probe.log
    if [ "$PROBE_RC" -eq 0 ]; then
        echo "FLASH-DAMAGE-SMOKE (negative): FAIL (the probe passed: the" \
            "gate did not detect the missing damage)${STIX_SLOT_TAG}"
    else
        echo "FLASH-DAMAGE-SMOKE (negative): FAIL (wrong failure: " \
            "$(why_failed boot-neg.probe.log))${STIX_SLOT_TAG}"
    fi
    exit 1
fi

if [ -z "$FLAVOUR" ]; then
    # ---- boot part ----
    seed_part boot
    discover_files boot
    boot_and_probe boot boot_args "$BOOT_SECS" seeded
    if [ "$PROBE_RC" -ne 0 ]; then
        fail_part boot
    fi
    uart_ok boot || {
        echo "FLASH-DAMAGE-SMOKE: FAIL (the boot part's UART has no" \
            "'init: SAFE mode' banner)${STIX_SLOT_TAG}"
        exit 1
    }
    stix_slot_cleanup 0 flash.bin
    echo "boot part: PASS"
    # ---- check part ----
    REBOOTABLE=1
    seed_part check
    discover_files check
    boot_and_probe check check_args "$CHECK_SECS" seeded
    if [ "$PROBE_RC" -ne 0 ]; then
        fail_part check
    fi
    check_damage_list "$CHECK_PAGE"
    check_tm2387 check.probe.log "0001"        # [L] the plan's bad_after=1
    check_0032_delta check.probe.log           # [L] persistence, D-X1
    uart_ok check || {
        echo "FLASH-DAMAGE-SMOKE: FAIL (the check part's UART has no" \
            "'init: SAFE mode' banner)${STIX_SLOT_TAG}"
        exit 1
    }
    stix_slot_cleanup 0 flash.bin
    echo "check part: PASS"
    # ---- addr part (wrong spare pageAddress on CONTEXT1, F20) ----
    REBOOTABLE=0
    seed_part addr
    discover_files addr
    crash_part addr "$STUCK_SECS" "address corrupt"
    stix_slot_cleanup 0 flash.bin
    echo "addr part: PASS"
    # ---- stuck part (= the uncorrectable boot, F20) ----
    seed_part stuck
    discover_files stuck
    crash_part stuck "$STUCK_SECS" "Unrecoverable error in file"
    stix_slot_cleanup 0 flash.bin
    echo "stuck part: PASS"
    echo "FLASH-DAMAGE-SMOKE: PASS (boot part), PASS (check part), PASS" \
        "(addr part), PASS (stuck part)${STIX_SLOT_TAG}"
    exit 0
fi

case "$FLAVOUR" in
    correctable)
        # the TMMGMT file must exist (TMBuffer backups); discover it on a
        # fresh private copy — NOT seeded: the seed cannot create one
        # (TMBuffer writes those only during a link outage, TMBuffer.c:715
        # naming). SKIP loudly when absent.
        PART_DIR="$(stix_slot_dir "$BASE_DIR-corr")"
        guard_dir "$PART_DIR"
        mkdir -p "$PART_DIR"
        cd "$PART_DIR"
        rm -f flash.bin
        stix_flash_private
        [ -f flash.bin ] || {
            echo "flash.bin missing — the master flash copy failed"
            exit 2
        }
        TMMGMT_ARGS=""
        TMMGMT_BLOCK=""
        for p in 0 1 2 3; do
            for i in 0 1; do
                for v in 0 1; do
                    if out="$(show_file --type tmmgmt --spare "$p" \
                            --coarse "$i" --fine 0 --duration "$v" \
                            2>/dev/null)"; then
                        TMMGMT_ARGS="--type tmmgmt --spare $p --coarse $i --fine 0 --duration $v"
                        TMMGMT_BLOCK="$(printf '%s\n' "$out" \
                            | block_of_show)"
                        break 3
                    fi
                done
            done
        done
        if [ -z "$TMMGMT_ARGS" ]; then
            echo "SKIP: no TMMGMT file on the fresh private copy of" \
                "$MASTER (and the seed cannot create one: TMBuffer" \
                "writes those only during a link outage); the" \
                "correctable flavour needs one (seed the master via" \
                "STIX_FLASH_MASTER)"
            exit 77
        fi
        echo "discovery (corr): TMMGMT file: $TMMGMT_ARGS (block $TMMGMT_BLOCK)"
        REBOOTABLE=1
        boot_and_probe corr corr_args "$FLAV_SECS" seeded
        if [ "$PROBE_RC" -ne 0 ]; then
            fail_part corr
        fi
        if ! grep -q "BCH corrected" uart.corr.txt; then
            echo "FLASH-DAMAGE-SMOKE: FAIL (correctable: no 'BCH" \
                "corrected' TRACE on the UART)${STIX_SLOT_TAG}"
            exit 1
        fi
        echo "CHECK bch-trace: ok ($(grep -m1 'BCH corrected' \
            uart.corr.txt | sed 's/^[[:space:]]*//'))"
        if [ "$CORR_BITS" -eq 5 ]; then
            # [L] the silent remove-and-rewrite (the old block had >= 4
            # corrections): the file lands on a fresh block — or is gone,
            # if the FSW deleted the restored backup after the queues
            # drained (TMBuffer.c:891-896); it must never stay put
            NEW_BLOCK="$(python3 "$TOOL" show $TMMGMT_ARGS ./flash.bin \
                | block_of_show)" || NEW_BLOCK=""
            if [ -z "$NEW_BLOCK" ]; then
                echo "CHECK rewrite: the file is gone (deleted after the" \
                    "restore drain); the rewrite is unobservable, the" \
                    "boot-1 'BCH corrected' line stands"
            elif [ "$NEW_BLOCK" = "$TMMGMT_BLOCK" ]; then
                echo "FLASH-DAMAGE-SMOKE: FAIL (correctable bits 5: the" \
                    "file is still on block $TMMGMT_BLOCK, want a fresh" \
                    "block after the rewrite)${STIX_SLOT_TAG}"
                exit 1
            else
                echo "CHECK rewrite: ok (block $TMMGMT_BLOCK -> $NEW_BLOCK)"
            fi
        fi
        stix_slot_cleanup 0 flash.bin
        echo "FLASH-DAMAGE-SMOKE: PASS (flavour correctable, bits" \
            "$CORR_BITS)${STIX_SLOT_TAG}"
        exit 0
        ;;
    uncorrectable)
        seed_part stuck
        discover_files stuck
        crash_part stuck "$STUCK_SECS" "Unrecoverable error in file"
        stix_slot_cleanup 0 flash.bin
        echo "FLASH-DAMAGE-SMOKE: PASS (flavour uncorrectable)" \
            "${STIX_SLOT_TAG}"
        exit 0
        ;;
    broken)
        boot_and_probe brk brk_args "$FLAV_SECS"
        if [ "$PROBE_RC" -ne 0 ]; then
            fail_part brk
        fi
        uart_ok brk || {
            echo "FLASH-DAMAGE-SMOKE: FAIL (the broken part's UART has" \
                "no 'init: SAFE mode' banner)${STIX_SLOT_TAG}"
            exit 1
        }
        stix_slot_cleanup 0 flash.bin
        echo "FLASH-DAMAGE-SMOKE: PASS (flavour broken)${STIX_SLOT_TAG}"
        exit 0
        ;;
    full)
        seed_part full
        discover_files full
        boot_and_probe full full_args "$FLAV_SECS" seeded
        if [ "$PROBE_RC" -ne 0 ]; then
            fail_part full
        fi
        check_damage_list_full() {
            local want=0 p
            for p in "$FULL_P1" "$FULL_P2"; do
                grep -qF "src=runtime kind=program-fail page=$p" \
                    full.probe.log && want=$((want + 1))
            done
            [ "$want" -eq 2 ] || {
                echo "FLASH-DAMAGE-SMOKE: FAIL (full: damage-list" \
                    "read-back shows $want of 2 specs)${STIX_SLOT_TAG}"
                exit 1
            }
            echo "CHECK damage-list: ok (2 specs active)"
        }
        check_damage_list_full
        check_tm2387 full.probe.log "0002"     # [L] one File_write, 2 burns
        uart_ok full || {
            echo "FLASH-DAMAGE-SMOKE: FAIL (the full part's UART has no" \
                "'init: SAFE mode' banner)${STIX_SLOT_TAG}"
            exit 1
        }
        stix_slot_cleanup 0 flash.bin
        echo "FLASH-DAMAGE-SMOKE: PASS (flavour full)${STIX_SLOT_TAG}"
        exit 0
        ;;
esac
