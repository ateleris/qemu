# Flash damage plan: bad blocks, broken cells and corrupted files as the FSW filesystem sees them

Status: X-1 to X-4 implemented 2026-11-12 (see their "As built"
notes); the rest is
draft, 2026-11-12, revised the same day after the independent
review (`.review-flashdmg.txt`, all 8 findings resolved — see "Review
resolutions") and the unit-test framework cross-check; unreviewed.
Base: track worktree
`~/dev/qemu-stix-flash` (branch `stix-uprev-flashdmg`, HEAD `083d3d4365`).
That base already carries the flash-latency work (D-F1..D-F3 issue/complete
split, `latency` on by default, accepted-diffs §42), so every flash op
completes from `stixflash_complete()` with one status update and one IRQ —
the natural place to attach damage. The track follows the platform and
attenuator conventions: the **platform track owns the fault-ID namespace
and the catalogue** (D-PLq), this track owns **the device side and the
offline tool**.

Sources:
- the model: `hw/sparc/grlib_stixflash.c` (998 lines), `hw/sparc/trace-events`,
  `tests/qtest/stix.c`, `scripts/stix/lib/{paths,slot}.sh`,
  `scripts/stix/boot-smoke.sh`, `scripts/stix/platform/stixplat/qemu.py`,
  `scripts/stix/platform/catalogue/faults-v1.json`;
- a read-only pass over the FSW, `~/dev/STIX-FSW/ASW`. Paths are relative
  to it: **BSW** = `BSW_ASW/src/BSW/...`, **ASW** = `ASW/src/...`;
- the platform plan (`docs/stix-port/2026-11-11-platform-plan.md`):
  D-PLf, D-PLk, D-PLo (as amended), D-PLq, R-b; the attenuator plan
  (`~/dev/qemu-stix-att/docs/stix-port/2026-11-11-attenuator-plan.md`):
  D-Ag/D-Ah conventions, A-1's QOM-name and as-built findings;
- the gate artefacts in `docs/stix-port/baseline/`;
- the FS unit-test framework `~/dev/STIX-FSW-UnitTest-Framework`
  (read-only; the former FSW filesystem dev repo: criterion tests, the
  `Flash.c` fake with `Flash_Curr_Img_DirectRead/Write`, its own
  `MemFilesystem.c`) — used for **scenarios and addressing only**; its
  `MemFilesystem.c` has drifted from the FSW (~561 diff lines), so the
  FSW tree wins on semantics (see "Scenario catalogue");
- the TM/TC ICD export
  `~/dev/STIX-TESTING/documentation/STIX-ICD-0812-ESC_I4R8draft_TMTC_ICD.txt`,
  cited as **ICD:line** (TC(238,5)/(238,6) live in its §7.3).

Fact tags:
- **[C]** read in code (FSW or model), with file:line;
- **[D]** read in a sibling plan or catalogue (code wins on conflict);
- **[G]** seen in a gate artefact;
- **[I]** inferred; a wrong guess is one constant or one knob;
- **[A]** an assumption the user has to accept; nothing is gated on one;
- **[L]** to be verified live before it becomes a gate expectation.

## Review resolutions

Every finding of the independent review was re-verified against the FSW
code before editing; all eight were accepted.

1. **Partition of the factory-bad-block example** — confirmed:
   `TMMGMT` = `TMBUFFER` = control blocks 9-11 = **blocks 604-783** with
   `LIMIT_FS_SIZE=32` (`MemFilesystem.c:46-48, 123-124`), block 70 is
   CB0 = **CONTEXT0** (`MemFilesystem.c:43, 107-108`). Fixed to block
   610 in the catalogue row, the outcome row and X-4; X-4's erase-fail
   gate now names block and partition.
2. **Two-blocks outcome row** — confirmed: `File_write` burns ≤ 2 blocks
   and returns `E_FILE_RES_ERROR` (`File.c:549-601`); the
   `MemFilesystem_write` retry (`MemFilesystem.c:722-754`) then finds a
   healthy block and succeeds. `FSW_ERROR_FILE_CORRUPTED` needs **4**
   damaged free blocks (2 per `File_write` × `MEM_FILESYSTEM_MAX_RETRIES`
   = 2). Row and gate math fixed.
3. **O6** — confirmed answered: `#define MEM_FILESYSTEM_MAX_RETRIES 2`
   (`MemFilesystem.h:25`, with the amplification rationale in the
   comment above it). O6 removed; arithmetic updated.
4. **Commit-order defect** — confirmed (plan-internal): X-1's
   `flash-dmg-marker-plain` now hand-writes the marker bytes; the tool
   only exists from X-3 on.
5. **0x543A wrong-address kind** — built as the tool mode
   `corrupt-file --mode address` (D-X2/D-X4), with its catalogue and
   outcome rows and an X-4 smoke check. The requirement citations are
   taken from the review (STIX-TN-0024-FHNW.txt §3.5.5.5 txt:2364-2366,
   :2400-2403; STIX-TN-0062 §4.6.8.6.5 txt:3528-3530 — the TN exports
   were not reachable from this worktree, the FSW side is verified:
   detection at `File.c:691-699`). One honest correction to the
   review's wording: the read path I traced emits **0x543B**
   (`MemFilesystem.c:589-592`); `TMForm_inconsistent_flash_address`
   (0x543A, `TMForm.c:678-686`) has no sender on that path in
   File.c/MemFilesystem.c/Flash.c (doc-vs-code 7) — the probe run
   decides which event actually appears.
6. **Catalogue contradiction** — confirmed; the offline rows are now
   marked setup-only in the table, consistent with D-X5.
7. **Sidecar vs reserved blocks** — confirmed unstated; D-X3 now says
   the sidecar refuses blocks < 64 at realize, no override.
8. **TcService238.c** — verified:
   `~/dev/STIX-FSW/ASW/ASW/src/TcTm/TcServices/TcService238.c`, TC(238,6)
   exactly at :169-183; the TC(238,5) delete loop at :56-166 closes with
   `TMForm_sendExecutionCompletedReport` (:209), so the TM(1,7)/(1,8)
   row stands. Unit layouts: `TCData.h:429-431` (TC(238,6)),
   `:422-427` (TC(238,5)); the ICD agrees (§7.3, Table 190/191/192).

### Review of X-1 (2026-11-12, post-build)

The X-1 build review (run `glm-x1-review`) reported 6 findings; every
FSW citation re-verified against the FSW tree before editing. 1, 2 and
4 are plan corrections applied above (correction 1, the outcome rows,
X-4/X-4b); 3, 5 and 6 are device fixes in the X-1 working tree.

1. **erase-fail on a free erased block is dormant for `File_write`** —
   confirmed: `write1` erases only when a page of the target block
   reads non-0xFF (`File.c:449-470`) and `MemFilesystem_write` does no
   pre-erase (`MemFilesystem.c:705-757`), so a write to a free erased
   block lands on the damaged block and succeeds. The free-block
   outcome rows use program-fail; erase-fail is exercised where an
   erase is really attempted: `File_remove` (`File.c:768`),
   `File_freeBlock` (`File.c:810`), TC(238,6) tryEraseBB
   (`TcService238.c:169-183`), a free-but-dirty block. X-4's check
   part uses program-fail (bad_after=1 kept).
2. **whole-block `stuck` at boot never yields 0x543B** — confirmed:
   the block mask covers the spare and the `File_init` scan
   (`File.c:318-339`) runs before the context read
   (`MemFilesystem.c:1029-1039`), so value 0 → marker 0xFE → BAD and
   value 1 → looks FREE; the file is "not found" (0x8030/0x5439).
   Outcome rows and X-4b corrected; 0x543B via stuck needs X-2 runtime
   injection on a catalogued file, page/byte scope, not the spare.
3. **only the first stuck entry per page applied** — confirmed; the
   mask now applies every matching stuck entry in list order (the
   later entry wins on the same bit); qtest `flash-dmg-stuck-multi`
   pins two stuck-1 bits on one page.
4. **program-fail on pages 0/1 defeats the grown-bad-block marker** —
   confirmed: the marker program targets the block's pages 0 and 1
   (`File.c:589-598`); documented in correction 1 and the D-X2 row;
   the tool/gates inject program-fail on pages ≥ 2 when persistence is
   the point.
5. **seek/tell failure reported as the sidecar cap** — fixed: the
   loader now reports "cannot determine the size of …" (with the
   strerror) separately from the 1 MiB cap.
6. **realize-failure cleanup asymmetry** — fixed: a failed sidecar
   load closes the backing file and NULLs it before realize returns,
   mirroring unrealize.

## Goal

1. The NAND model can be damaged in the ways real NAND fails, and the
   FSW's filesystem sees the consequences:
   - **factory bad blocks** (the spare-area marker the boot scan reads);
   - **grown bad blocks** (erase/program failures the FSW itself reacts
     to by marking the block bad);
   - **broken/stuck cells** (bits that read a stuck value, 0 or 1,
     regardless of what was programmed) and read-disturb-style bit
     flips;
   - **corrupted stored files** addressed by their FSW filesystem name,
     not by block numbers, so a test never needs to know the layout.
2. Every fault has a stated FSW-visible outcome (EIDs, TM(238,7) counts,
   boot 0x0032 payload), marked [L] until a live run confirms it.
3. **With no damage configured, nothing changes**: same status bits, same
   timing, same traces, byte-identical goldens (see "Goldens").
4. Two injection paths with one damage model:
   - an **offline tool** that damages a flash image before boot
     (persistent, stored in the image);
   - **runtime QOM properties** on the flash device (controller-driven,
     until cleared), plus a small **sidecar map** for damage that must
     exist from boot.
5. A new gate `scripts/stix/flash-damage-smoke.sh` proves 1-2 live, with
   a `--negative` mode that proves the gate bites.

## Corrections to the brief

These change what the track can deliver. Read them first.

1. **The FSW writes the grown-bad-block marker itself.** After a failed
   erase or program inside `File_write`, the FSW erases the block and
   programs `badBlockMarker = 0` into the first two pages
   (`File.c:582-598` [C]). So the device never needs to mark anything:
   injecting an erase/program failure makes the FSW produce a persistent,
   boot-surviving bad block in the image on its own. Factory bad blocks
   are the same marker, pre-written by the tool. One mechanism, two
   origins. One bound, from the X-1 review: the marker program targets
   the block's **pages 0 and 1** (`File.c:589-598`), so a program-fail
   injected there defeats the marker — entries on both pages leave no
   marker at all (the next boot scan reclassifies the block FREE) and a
   single one leaves only the other marker. The tool and the gates
   therefore inject program-fail on pages ≥ 2 when the persistent
   marker is the point (X-4); erase-fail needs no such rule (the FSW
   marks without checking the erase result, `File.c:592`).
2. **No device-side "read failure" kind.** A read that completes with
   FAILED hits `MemFilesystem`'s `Read_page` error path, which does
   `ASSERT(0)` ("Flash operation failed, unexpected status",
   `MemFilesystem.c:1223-1228` [C]) — on a build with asserts live that
   stops the software, which is not the outcome a filesystem test wants.
   Real NAND reads do not fail; they return wrong data. "Whole block
   unreadable" is therefore expressed as a **stuck-at-0 mask over the
   whole block**, which the FSW handles cleanly: BCH declares every page
   uncorrectable, `File_read` returns `E_FILE_RES_ERROR`, and TM(5,2)
   **0x543B** carries the page address (`File.c:671-678`,
   `MemFilesystem.c:589-592`, `TMForm.c:689-692` [C]).
3. **There is no ECC in the device.** The ECC is guest software: BCH
   over 528-byte sub-blocks, 8 per page, corrects ≥5 and detects ≥13 bit
   flips, 12 code bytes per sub-block in the spare area
   (`File.h:106-122` [C]; `MemoryMgmt/BCH.c`). The model stores raw
   pages (`grlib_stixflash.c:376-476`) and must keep doing so: damage is
   plain bits, and all correction behaviour comes from the FSW for free.
   The `rotbCmdSta`/cache EDAC machinery (`MemFilesystem` SEU counters
   aside) does not touch the NAND path.
4. **The model's bad-block-marker comment is stale for the default
   storage mode.** `grlib_stixflash.c:446` says "a hole would read 0x00
   = bad-block marker on the next boot scan" — true only with
   `inverted-storage=off`. With the default inversion on, a file hole
   (byte 0x00) reads as guest 0xFF, an erased page (`:195-206` [C]). The
   tool must apply the inversion; the qtest that pins the marker
   behaviour must run in both modes. The qtest comment at
   `tests/qtest/stix.c:404-405` repeats the stale wording.
5. **The AX gate ELF sees the damage.** The gate ELF has the full
   filesystem (it scans 64 control blocks, `LIMIT_FS_SIZE=32`, and
   writes context on NOMINAL entry — flash-latency plan "Corrections"
   and `MemFilesystem.c:46-52` [C]). Bad blocks in the FS partitions and
   corrupted files are FSW-visible on the gate ELF; only the ASW-image
   area (blocks < 64, `MemFilesystem.c:43-44` [C]) is not, and the tool
   refuses it by default. No PFM ELF is needed for the first gate
   (O1 asks whether a PFM variant is wanted).

## Key facts the plan rests on

### Geometry and page layout [C]

- 4096 data + 128 spare bytes per page, 64 pages per block, 4096 blocks
  per chip, 8 chips per MCM, 2 MCMs; the model mirrors the FSW constants
  (`FilesystemDefinition.h:15-26`, `grlib_stixflash.c:29-40`).
- The FSW's linear page address splits as mcm = `addr>>21`, chip =
  `(addr>>18)&7`, page = `addr & 0x3FFFF` (`Flash.c:70-73`); the model's
  file offset is `((mcm*8 + chip)*262144 + page) * 4224`
  (`grlib_stixflash.c:188-193`). Chip 0/MCM 0 page P sits at file
  offset P·4224.
- Spare layout (`File_auxPageData_s`, packed, 128 B): `badBlockMarker`
  at 0 (bad iff ≠ 0xFF), `emptyPageMarker` at 1 (0xFF empty, 0xFE in
  use), `fileName[10]` at 2..11, `pageAddress` u32 at 12,
  `logicalWriteTime` u64 at 16, `fileSizeInBytes` u32 at 24,
  `ECC[8]×12` at 28..123, filler 124..127 (`File.h:128-140`).
- File name: 10 bytes — [0] type, [1] spare, [2..5] SCET coarse,
  [6..7] SCET fine, [8..9] duration (`FilesystemDefinition.h:58-73`).
  Types: 1 ASPECT, 2 RAW_COUNTS, 3 TMMGMT, 6 FLARE, 7 USER_REQUESTS,
  8/9 CONTEXT0/1, 10 LUT (`:80-92`). Partitions live on control blocks
  starting at block 64 per flash (`MemFilesystem.c:43-52, 107-144`).

### The bad-block scan and the boot reports [C]

- `File_init` reads pages `page0+1` then `page0` of every block and
  marks the block `E_FILE_BLOCK_STAT_BAD` when either spare
  `badBlockMarker` ≠ 0xFF (`File.c:318-339`); the marker is spare byte 0
  of the first or second page (the K9F8G08X0M rule, quoted there).
- First page empty (`emptyPageMarker == 0xFF`) → free; first-page header
  BCH-uncorrectable → `E_FILE_BLOCK_STAT_ERROR`, "pretend this file
  doesn't exist" (`File.c:342-346, 355-376`).
- Block stats are global counters: `Flash_badBlocks` ("broken forever"),
  `Flash_errorBlocks` ("can be emptied and reused"), `Flash_usedBlocks`
  (`Flash.h:176-178`); transitions are counted in
  `updateStatsBlockTransition` (`File.c:109-147`).
- The init task reads both context files first, then scans all
  partitions, then sends TM(5,1) **0x0032** `Filesystem initialisation
  finished` with `{status u8, used u16, bad u16, error u16, free u16}`
  (`MemFilesystem.c:1018-1070`, `TMForm.c:631-641`; the golden payload
  on a fresh AX flash is `0032 01 0002 0000 0000 fffe` [G]).

### Write, erase and the grown-bad-block path [C]

- `File_write` → `write1`: erase if needed, then program page by page.
  Any `Write_page`/`Erase_block` failure → `File_write` marks the block
  bad (erase; program marker 0 into pages 0 and 1) and retries on
  another block, at most `File_CONFIG_MAX_BB` = 2 times, then returns
  `E_FILE_RES_ERROR` (`File.c:427-542, 549-601`; `File.h:87-102`).
  `MemFilesystem_write` retries the whole write `MEM_FILESYSTEM_MAX_RETRIES`
  = 2 times before returning `FSW_ERROR_FILE_CORRUPTED`
  (`MemFilesystem.c:722-754`; `MemFilesystem.h:25`). One `File_write`
  call burns ≤ 2 blocks and the retry picks fresh free blocks, so
  **4** damaged free blocks are needed for `FSW_ERROR_FILE_CORRUPTED`
  (2 per `File_write` × `MEM_FILESYSTEM_MAX_RETRIES`).
- The FSW marks bad **without checking the erase result**
  (`(void)Erase_block`, `File.c:592-598`), so a block injected
  erase-fail is marked bad even though the erase failed; with inversion
  the model's clean page overwrite makes the marker land anyway (the
  model does not model program-on-non-erased). Documented below.
- `Write_page`/`Erase_block` wrap the driver with 2 retries, a flash
  reset + 20 ms wait on timeout, and on `E_FLASH_RES_ERROR_WRITE` a
  reset + retry (`MemFilesystem.c:1073-1119, 1236-1318`); a reset
  failure sends TM(5,3) **0x944C** `Flash_reset error` with the chip
  mask (`MemFilesystem.c:208-218`, `TMForm.c:564-567`,
  `TMData.h:360`).
- The driver turns status FAILED into `E_FLASH_RES_ERROR_WRITE` for
  erase and program in the ISR (`Flash.c:150-167`); the model sets
  FAILED|READY with one IRQ at completion
  (`grlib_stixflash.c:239-258, 486-516`).

### Read and the corrupted-file path [C]

- `File_read` walks the file's pages: `Read_page` fail →
  `E_FILE_RES_ERROR` with the page address (`File.c:651-656`); per
  528-byte sub-block BCH, uncorrectable → `E_FILE_RES_ERROR`
  (`:665-685`); spare marker ≠ 0xFF or `pageAddress` mismatch →
  `E_FILE_RES_ERROR` (`:689-699`); file-size or write-time inconsistency
  across pages → `E_FILE_RES_ERROR` (`:706-719`).
- `MemFilesystem_read` maps `E_FILE_RES_ERROR` → TM(5,2) **0x543B**
  `flash page CRC check failed` with the page address (when
  `generateTMs`), and returns `FSW_ERROR_FILE_CORRUPTED`
  (`MemFilesystem.c:585-602`, `TMForm.c:689-692`, `TMData.h:299`).
- Corrected reads are counted: "BCH corrected N SEUs in this file";
  when the worst block had ≥ `E_PARAM_ID_FS_REWRITE_FILES_AFTER_N_SEUS`
  corrections (default 4, `ParamMgmtDef.h:538` [D]) the file is removed
  and rewritten to a fresh block (`MemFilesystem.c:604-655`).
- Consumers of a corrupted/missing file:
  - **context**: missing at boot → the FSW runs on default parameters,
    recreates both files, and sends TM(5,3) **0x8030** `No CONTEXT file
    was found in file system` with `{newSpare0, newSpare1}`
    (`ContextMgmt.c:1890-1912, 1929-1984`, `TMForm.c:651-658`,
    `TMData.h:318`); a header-corrupt context file is "not found" (the
    catalog entry never exists), a data-corrupt one gives 0x543B and
    then the same fallback [I → L].
  - **LUT**: not found → TM(5,2) **0x5439** `LUT not found` and the FSW
    falls back to defaults (the AX UART shows the four `LUTMgmt` lines)
    (`TMForm.c:669-676`, `TMData.h:297`; fallback [I → L]).
  - **pageAddress inconsistency** (a page written to the wrong place:
    the spare `pageAddress` ≠ the physical page, checked at
    `File.c:691-699`) → `E_FILE_RES_ERROR`; the traced read path sends
    TM(5,2) **0x543B** `flash page CRC check failed`
    (`MemFilesystem.c:589-592`). **0x543A** `inconsistent flash address`
    (`TMForm_inconsistent_flash_address`, `TMForm.c:678-686`) is the
    TN-required "error message" for this aspect but has no sender on the
    flash path I traced (doc-vs-code 7); the probe run decides which
    event appears. Targeted by the tool's `corrupt-file --mode address`
    (D-X4).

### TC(238,6) / TM(238,7): the filesystem check [C]

- TC(238,6) `{startFile u16, endFile u16, badBlockHandling u8}` (1 =
  try to erase bad blocks) runs an async check over the file range and
  sends its own TM(1,7)/(1,8); the scan erases corrupted files
  (`FSW_ERROR_FILE_CORRUPTED` → erase), frees error blocks
  (`File_freeBlock`), and with `tryEraseBB` tries to erase bad blocks
  (`~/dev/STIX-FSW/ASW/ASW/src/TcTm/TcServices/TcService238.c:169-183`,
  `MemFilesystem.c:791-992`, `:900-948`). The unit layout is
  `TCData.h:429-431` (start at application-data offset 0, end at +2,
  badBlockHandling at +4) and is confirmed by the TM/TC ICD §7.3.6,
  Table 192 (ICD:23981-24078; `badBlockHandling` "0 = do not handle bad
  blocks, 1 = try erasing bad blocks", with the warning "DO NOT SET TO 1
  UNLESS UNDERSTOOD WHY", ICD:24066-24078).
- The result is TM(238,7) with 13 × u16: start/end file, start/end
  block, number of files, and bad/error/free/filled before and after
  (`TM_238.c:122-149`), on APID PID_90/category diagnostics
  (`:151-180`). This is the ground-visible block-count readout.

### The model today [C]

- Commands RESET 0 / ERASE 1 / READ 2 / WRITE 3, status bits FAILED 24,
  BUSY 4, READY 30, WRPROT 31 (`grlib_stixflash.c:57-66`); issue latches
  an op record, completion executes the file I/O, sets the status and
  pulses the IRQ once (`:486-516`, `:579-676`).
- The backing file is sparse and inverted by default (file 0x00 = guest
  0xFF); erase punches holes; the file size is the flash capacity and is
  never extended (`:164-206, 290-374, 428-476`).
- QOM: qdev `flash-file`, `inverted-storage`, `latency`, `tread-us`,
  `tprog-us`, `tbers-us` (`:932-944`); read-only counters `ops-read/
  prog/erase/abort` (`:961-973`); the device reset handler drops the
  pending op and clears cmd_sta/`write_counter`, and touches nothing
  else (`:919-929`); vmstate is deliberately unmigratable (`:955-958`).
- Trace events live in the `# grlib_stixflash.c` block
  (`trace-events:23-36`); the golden capture runs `-trace 'stix*'`
  (`baseline/run-flags.txt:4` [G]) so new `stixflash_*` events must not
  fire when no damage is configured.
- The qtests use the 1 MB sparse temp flash helper
  `stix_qtest_new_extra` (`tests/qtest/stix.c:133-149`) and assert
  FAILED|READY + one IRQ on error paths (`:330-417`).

### Flash images: how they are produced and shared [C]

- The master flash is `$STIX_UPREV_DIR/flash.bin` =
  `~/dev/stix-state/uprev-run/flash.bin` (`lib/paths.sh:24-28`).
  Gates boot a **private sparse copy** (`stix_flash_copy`,
  `lib/slot.sh:275-330`): `cp --sparse=always`, the copy's allocation
  must stay under `STIX_FLASH_MAX_ALLOC_KB` (default **64 MiB**), the
  filesystem must keep holes (ext2/3/4, tmpfs, btrfs, xfs), and the
  shared dirs are refused as run dirs (`stix_run_dir_guard`,
  `:238-260`). The platform's run dirs are covered by the same rules
  plus its own binary/sparse preflight (`stixplat/qemu.py:51-53,
  108-109` [D]).

## Decisions (D-X)

- **D-X1: two layers, one damage model; persistence split.**
  - **Data-side damage lives in the image**: factory bad-block markers
    and corrupted file bytes are written into `flash.bin` by the offline
    tool (X-3). It survives warm reset, cold power cycle and QEMU exit,
    because it is the stored data. No device state involved.
  - **Device-side damage lives in the model**: erase-fail, program-fail
    and stuck-at masks change device behaviour, not stored data. They
    come from a sidecar map (from boot) or runtime QOM (until cleared).
    They are lost at QEMU exit; the *consequences* the FSW wrote into
    the image (grown bad-block markers, rewritten files) persist, which
    is exactly the factory→grown lifecycle of real NAND.
  - Runtime device faults survive a guest/system reset (the reset
    handler is untouched), and are re-applied after a cold power cycle
    by the platform controller's `on_power` re-apply, like `link-down`
    (D-PLf) [D]. Catalogue persistence: `until_cleared`, `reversible`.
  - Volatile-only (lost at exit) is the rejected alternative: the
    attenuator faults can be re-applied by the controller, but a flash
    test that needs "the FSW has marked 3 blocks bad across a reboot"
    would need the sidecar anyway; the image is the honest store.
- **D-X2: the v1 fault kinds and their device semantics.**
  | kind | scope | semantics |
  |---|---|---|
  | `bad-block` (offline only) | block | tool writes marker 0 into the spare of pages 0 and 1; the FSW's scan sees a bad block. The device never writes markers itself. |
  | `erase-fail` | block | every erase op on the block completes FAILED\|READY (+IRQ), nothing erased. The FSW's marking erase also fails (`(void)`, `File.c:592`), the marker program still lands (nuance, documented). |
  | `program-fail` | page | every program op on the page completes FAILED\|READY, nothing written. The FSW's grown-bad-block marker program targets the block's pages 0 and 1 (`File.c:589-598`), so an injection there defeats the marker (correction 1) — the tool/gates use pages ≥ 2 when persistence is the point. |
  | `stuck` | page or block | reads return the true page with chosen bits forced to their stuck value 0 or 1 (O4: both polarities; `stuck0` stays as an alias for value 0): `page=<p> bit=<byte>.<bit>` plus `value=<0|1>` for single-bit flips (BCH-correctable), `block=<b>` plus `value=<0|1>` (bit 0 of every byte) for a whole-block unreadable. Applied after the file read, before the DMA, every read — so it also corrupts a freshly programmed page, like a real stuck cell. |
  - Deferred, not built: probabilistic wear (fails the Nth op — O3
    resolved: planned later, see "Later"), `read-fail` (correction 2),
    multi-plane quirks.
  - **Offline, built in v1 after the review (finding 5):**
    `wrong-address` — the TN-required "data block written at the wrong
    address" aspect (STIX-TN-0024 §3.5.5.5, txt:2364-2366 and
    :2400-2403; STIX-TN-0062 §4.6.8.6.5, txt:3528-3530: an error message
    shall be sent). The tool rewrites the spare `pageAddress` u32
    (aux offset 12, inside sub-block 7) of chosen pages of a stored
    file to a wrong but valid page address **and re-encodes sub-block
    7's BCH** (the `ECC[7]` bytes at aux 112..123), so the read passes
    BCH and reaches the FSW's address check (`File.c:691-699`) instead
    of failing ECC first. FDIR-required, hence not deferred any more.
  - Addressing grammar, shared by sidecar, QOM and tool — two forms:
    - `block=<linear-block>` or `mcm=<m>,chip=<c>,page=<p>` (FSW linear
      page = `mcm<<21 | chip<<18 | page`); and, for one-to-one replay of
      unit-test scenarios, the framework's
      `mcm=<m>,chip=<c>,block=<b>,page=<p>[,offset=<o>]` form (the
      argument list of `Flash_Curr_Img_DirectWrite`,
      `Flash.c:280-310`): linear page =
      `mcm<<21 | chip<<18 | (block*64 + page)`, offset = the byte offset
      inside the 4224-B page. The framework's fake names the second
      index `device`; the FSW's name `chip` is used here. Mapping to the
      model: linear page × 4224 = the backing-file offset
      (`grlib_stixflash.c:188-193`), every byte written XOR 0xFF under
      the default `inverted-storage=on`. The framework's *image format*
      itself differs (allocation table + appended pages,
      `Flash.c:144-156`) — only the addressing is shared.
    - Blocks < 64 are refused by the tool by default (the ASW image
      area; `--allow-reserved` overrides).
- **D-X3: the device surface and the sidecar.**
  - qdev `flash-damage-map` (string path, default **""** = no sidecar,
    no change): parsed at realize. Sidecar = JSON, one small file next
    to the flash, e.g. `<run dir>/flash.bin.dmg`:
    `{"v":1,"damage":[{"kind":"erase-fail","block":610}]}`. A missing
    file is silence; a malformed file fails realize (`error_setg`
    naming the line) — loud, like `tread-us` validation. The sidecar
    applies the same reserved-block rule as the tool: a spec with
    block < 64 fails realize, and there is **no override** — the
    ASW-image area is not FSW-visible on the gate ELF, so nothing needs
    to inject there; a qtest that wants the reserved area uses the
    runtime property instead.
  - Runtime QOM on `/machine/stixflash`: `damage-add` (write str: a
    spec as in D-X2), `damage-remove` (write str: the exact spec or
    `all`), `damage-list` (read-only str: the active specs, sidecar
    entries included, `src=map|runtime` — X-2 renamed the X-1
    `src=file` to `src=map`). Dashed names, one property
    per verb — no dotted-ID property zoo, because flash faults carry
    block parameters that an ID cannot hold (the attenuator's one-
    property-per-ID needs no params; here the spec is the ID).
  - Entries apply at `stixflash_complete()` only: an erase/program
    still takes its full latency, then fails — the FSW sees a normal
    slow op that failed, exactly like hardware. `stuck` applies at
    read completion after the file read and before the DMA.
  - Traces: `stixflash_damage_hit(kind, page_address, cmd_sta)` and
    `stixflash_damage_load(n, path)`; both silent with no damage.
  - Rejected: a `qom`-per-block property set (unbounded names), a QMP
    command (needs generic `qapi/` changes, against the platform's
    rule), and teaching the device the filesystem format (layering).
- **D-X4: the offline tool, `scripts/stix/flash-damage.py`** (stdlib
  only, like the rest of `scripts/stix`):
  - `bad-block --block N` (or the framework form
    `--mcm M --chip C --block B`) — marker 0 into the spares of pages
    0/1; `--fs-range` applies it to every FS-scanned block (A: linear
    blocks 64-1983, B: 32832-34751 with `LIMIT_FS_SIZE=32`), for the
    broken-flash scenario of the unit-test framework's
    `Initialization.BBrokenFlash`;
  - `corrupt-file --type T [--spare S --coarse C --fine F] --mode
    header|data|address [--bits N]` — locates the file by its 10-byte
    name (chunked spare-area scan of the FS regions) and flips a
    deterministic pattern: `header` (uncorrectable first-page header →
    file "does not exist" at boot) or `data` (>7 flips in one 528-B
    sub-block → 0x543B on read); `--bits` small (≤4) produces the
    BCH-corrected path, 5..7 additionally the
    remove-and-rewrite path (`MemFilesystem.c:604-655`); `address`
    (review finding 5) rewrites the spare `pageAddress` of the page(s)
    and **re-encodes sub-block 7's BCH**, which needs a small BCH(13,7)
    encoder in the tool — the parameters are `m=13, t=7`, 12 ECC bytes
    (`BCH.c:130-132` in both the FSW and the framework;
    `File.h:114-117`); the self-test pins the encoder against a
    reference vector produced once with the framework's `BCH.c`
    (generated by the reviewer, committed as a fixture);
  - `show --type T ...` — prints the file's block/pages (truth
    read-back), for scenarios that then inject runtime faults there;
  - `device --kind K [--value 0|1] ... --out flash.bin.dmg` — writes
    the sidecar (kind `stuck` carries `value`, `stuck0` = the value-0
    alias);
  - addressing: every block/page argument accepts both the linear form
    (`block=<linear>` / `page=<linear>`) and the framework's
    `mcm/chip/block/page/offset` form (D-X2), so a unit-test scenario
    can be replayed one-to-one;
  - it applies the inversion (default `inverted-storage=on`; `--plain`
    for off), never writes past EOF (the file size is the capacity,
    D-H), re-checks `du` against the 64 MiB cap after every edit, and
    refuses the shared master dirs from `stix_run_dir_guard`'s list
    unless `--allow-shared` is given. Scan cost on the 16.5 GB sparse
    image is a sequential hole-friendly read of the FS region only
    (~0.5 GB AX, minutes worst case on PFM) [I].
- **D-X5: the platform contract.**
  - Catalogue entries in `faults-v1.json` (a new reserved block next to
    the attenuator's, same format [D]): `flash.block.erase_fail`,
    `flash.page.program_fail`, `flash.cell.stuck` — class `memory`,
    `changes: "truth"` (the stored data or the op result really
    changes; the D-PLo amendment), `persistence: "until_cleared"`,
    params `{block|page, bit}` with the FSW-linear index space named,
    `action: {"kind":"qom","property":"damage-add","clear":
    "damage-remove"}` (the D-PLq SF8 qom action, with the spec as the
    value).
  - Offline damage is **setup, not a runtime fault**: the catalogue
    gets no runtime entry; scenarios express it as a pre-boot step (a
    new platform hook verb `flash.damage`, in the `hooks.*` family of
    D-PLk) that runs the tool against the run dir's fresh flash before
    power-on. `expected_detection` for the offline rows lives in the
    scenario steps (0x0032 payload, 0x543B), not in the catalogue. The
    fault catalogue below therefore lists the offline IDs as
    **setup-only** rows (expressed via the `flash.damage` hook, no
    `action`), which resolves the contradiction the review flagged
    (finding 6).
  - Read-back: `damage-list` is the controller's truth; a flash section
    in `hw-state` (D-PLo) is **not** built in v1 (the properties are
    enough; noted as a follow-up).
- **D-X6: zero overhead, determinism, sparse rule.**
  - With no sidecar and no runtime entries the hot paths gain one
    `damage == NULL` check per op; no trace fires; status, timing and
    IRQ behaviour are unchanged; goldens stay byte-identical.
  - All damage is deterministic: no RNG, no counters, no seeds; a
    `clock_step` script replays bit-exactly. The catalogue's `seed`
    field stays unused (as the attenuator's noise modes are).
  - Damage never grows the backing file: writes stay inside EOF, the
    tool re-checks the allocation cap, the sidecar is KBs. A damaged
    flash stays a legal `stix_flash_copy` source (sparse, under 64 MiB).
- **D-X7: ownership.** This track owns the NAND damage model, the tool
  and the `flash.*` catalogue IDs. The **memory-upset track**
  (`stix-uprev-mem`, the platform plan's R-a: RAM/SRAM/BRAM EDAC,
  0x542F, the SDRAM DEU trap) owns semiconductor-memory upset outside
  the NAND; the platform plan's R-b ("SEU/DEU injection in flash",
  live single-bit flips) is **folded into this track** — a live flip is
  `stuck page=<p> bit=<b> value=<0|1>`, and R-b's "0x543B / rewrite"
  outcomes are
  exactly this plan's outcome table. No second flash-fault track is
  started.

## Fault catalogue

All entries are `until_cleared` and reversible unless stated; the tool's
offline rows are permanent in the image (remove them by re-flashing a
fresh copy, never by editing the shared master).

| ID | layer | mode / param | effect | source |
|---|---|---|---|---|
| `flash.block.bad_factory` | offline (image), **setup-only** | block ≥ 64 | boot scan counts it bad; never allocated | new (platform to adopt) |
| `flash.block.erase_fail` | device | block | erase ops on it fail (FAILED\|READY); FSW marks it bad and moves on | new |
| `flash.page.program_fail` | device | page | program ops fail; FSW marks the block bad | new |
| `flash.cell.stuck` | device | page + bit, or block; stuck value 0 or 1 | reads return the bits at their stuck value; small = corrected/rewrite path; whole block: BAD or "not found" at boot (the scan reads the masked spare first), 0x543B only via runtime injection on a catalogued file (page/byte scope, not the spare) | new; subsumes the platform plan's R-b flash SEU |
| `flash.file.corrupt` | offline (image), **setup-only** | file name (type/spare/coarse/fine), header or data, bit count | header: file vanishes at boot (0x8030/0x5439 + defaults); data: 0x543B, or the corrected→rewrite path at ≤7 bits (rewrite from 5) | new |
| `flash.file.wrong_address` | offline (image), **setup-only** | file name, page(s), wrong spare `pageAddress` (BCH re-encoded) | read reaches the FSW's address check (`File.c:691-699`); error TM with the page address (0x543B on the traced path; 0x543A is the TN-named event — doc-vs-code 7) | new; FDIR-required (TN-0024 §3.5.5.5, TN-0062 §4.6.8.6.5) |

## FSW-visible outcome per fault

Setup per row: fresh private flash, AX gate ELF, default latency.
Everything here is [L] until a live run confirms it. "boot" = the
0x0032 payload fields `{status, used, bad, error, free}`.

| fault | when | FSW reaction |
|---|---|---|
| none | — | `0032 01 0002 0000 0000 fffe` (fresh flash) [G] |
| `bad_factory` block 610 (a TMMGMT-partition block: TMBUFFER = CBs 9-11 = blocks 604-783) | boot | 0x0032 bad=1, free−1; TC(238,6) → TM(238,7) bad_before=1; `tryEraseBB` erase fails or succeeds without changing the count |
| `program_fail` on a page of a free block, then a context write | runtime | the write fails there, the FSW marks the block bad and retries on another block; TC(238,6) shows bad_after=1 (the FSW's own marker, persistent across reboot: 0x0032 bad=1 after a reset); keep the FSW's marker pages 0/1 un-injected (correction 1) |
| `program_fail` on 2 consecutive free blocks | runtime | one `File_write` burns both and returns `E_FILE_RES_ERROR`; the `MemFilesystem_write` retry picks fresh free blocks and **succeeds** — TC(238,6) shows bad_after=2, the write completes [I → L] |
| `program_fail` on 4 free blocks of one partition | runtime | both `File_write` calls (2 × `File_CONFIG_MAX_BB`) exhaust their blocks → `FSW_ERROR_FILE_CORRUPTED` (`MemFilesystem.c:740-741`) [I → L: no TM unless a caller sends one] |
| `erase_fail` on a free-but-dirty block, then a write | runtime | `write1` really erases (a page of the block reads non-0xFF, `File.c:449-470`), the erase fails → block marked bad, write retried elsewhere; on a free **erased** block erase-fail is dormant: no erase is attempted, the write lands on the damaged block and succeeds (review of X-1, finding 1) |
| `erase_fail` on a used block + TC(238,5) free-files (the ICD's "Free data in the filesystem", Table 190) | runtime | `File_remove` erase fails → `FSW_ERROR_FILE_FLASH_IO`, TM(1,8) for the TC (`TcService238.c:56-166, 209`); file survives |
| `stuck` 1 bit (value 0 or 1) in a stored file's page | read | "BCH corrected 1 SEUs"; ≥4 corrections in the block → the file is removed and rewritten (old block freed; `File_rewriteAlmostCorruptedFiles` [I → L: no TM]) |
| `stuck` >7 bits in one sub-block | read | 0x543B `flash page CRC check failed` with the page address; file unreadable |
| `stuck` whole block at 0 (context file) | boot | the mask covers the spare and the `File_init` scan runs before the context read (`MemFilesystem.c:1027-1039`): the marker reads 0xFE → BAD (0x0032 bad=1); the context file is "not found" → 0x8030 `(1,1)` and default parameters, both context files recreated; **no 0x543B** (0x543B via stuck needs X-2 runtime injection on a catalogued file, page/byte scope, not the spare) |
| `stuck` whole block at 1 (context file) | boot | marker reads 0xFF, `emptyPageMarker` reads 0xFF → the block looks **FREE** (an in-use block silently reusable): the context file is "not found" → 0x8030 `(1,1)` + defaults; bad stays 0 |
| `stuck` whole block at 0 (LUT file) | boot | the scan marks the block BAD (marker 0xFE, as above); the LUT file is "not found" → 0x5439 `LUT not found` + the four `LUTMgmt` UART lines; **no 0x543B** |
| `wrong-address` on a data page of CONTEXT1 | boot | the init context read (`MemFilesystem.c:1027-1042`, `generateTMs` still TRUE) hits `File.c:691-699` → 0x543B with the page address; CONTEXT0 still found, so **no** 0x8030; 0x0032 unchanged (the block stays `E_FILE_BLOCK_STAT_FILE`) [I → L] |
| `file.corrupt header` (context) | boot | the file is `E_FILE_BLOCK_STAT_ERROR` ("doesn't exist", error=1 in 0x0032); 0x8030 `(1,1)`; defaults; TC(238,6) frees the block (error before → free after) |
| `file.corrupt header` (LUT) | boot | error=1 in 0x0032; 0x5439; TC(238,6) frees it |
| `file.corrupt data` (HK/TM buffer file) | read | 0x543B; the reader's own handling [I → L] |
| `erase_fail` on the block the FSW is marking bad | runtime | the marker program lands anyway (the model overwrites pages cleanly); the block is counted bad; on real NAND it could stay unmarked — accepted diff, documented |

## Scenario catalogue (from the unit-test framework)

Source: `~/dev/STIX-FSW-UnitTest-Framework` (read-only), suite/test and
`file:line` as in that repo. The framework's `Flash.c` fake stores pages
behind an allocation table (`Flash.c:144-156, 280-310`), so its images
are **not** interchangeable with the emulator's linear sparse image —
what transfers is the *scenarios* and the *addressing* (D-X2/D-X4),
never the semantics: the framework's `MemFilesystem.c` has drifted from
the FSW (~561 diff lines), and its own tests are not all self-consistent
(`FileAlloc.AFill` "fills" TMMGMT with 480 successful writes on what is
a 3-CB = 180-block partition, one file per block). The FSW tree wins
wherever they disagree.

| suite/test | what it does | maps to the emulator? |
|---|---|---|
| `FileCorruption.ABitflips` (`TestFileCorruption.c:8`) | 300 TMMGMT files, 0-4 flips per 528-B sub-block (≤ `FILE_ECC_BCH_CORRECTIONS`), every `FSread` returns byte-identical data | yes — X-4b scenario 1 (`corrupt-file --bits ≤4` / single-bit `stuck`): read succeeds silently, "BCH corrected N SEUs" TRACE only, no TM; reboot leg re-reads corrected (the image keeps the damage) |
| `FileCorruption.BNoBitflips` (`:90`) | same loop, no damage (control) | yes — the `--negative` undamaged baseline of X-4 |
| `FileCorruption.C1BitflipsPre/PostRestart` (`:121-178`) | bitflips across a process restart via a log file; **commented out** in the framework | the shape (damage, restart, re-check) is X-4b scenario 1's reboot leg; the emulator gets image persistence for free (D-X1) |
| `Initialization.AEmptyFlash` (`TestInitialization.c:11`, `.disabled`) | empty image, `FSInit` completes | already covered: the fresh-flash golden `0032 01 0002 0000 0000 fffe` [G]; no new scenario |
| `Initialization.BBrokenFlash` (`:28`, `.disabled`) | zeroes the first 32 spare bytes of pages 0 and 1 of **every** block → the boot scan marks all blocks bad | yes — X-4b scenario 3 ("broken flash"): tool `bad-block --fs-range` (marker byte 0 suffices — the FSW checks only `badBlockMarker`, `File.c:328`); outcome 0x0032 with bad=3840, 0x8030, 0x5439 [L] |
| `Initialization.CFullEmptyFlash` (`:54`, `.disabled`) | writes 0xFF markers (a no-op on erased flash) | control for the row above; not mapped |
| `Partitions.FilePartitions30MB` (`TestPartitions.c:16`) | fills CONTEXT0/USER_REQ/FLARE/CONTEXT1 with 120 files → `FSW_ERROR_FILE_OUT_OF_SPACE` | partially — pins the quota arithmetic (120 blocks per 30 MB partition) that X-4b scenario 4's full-FS expectation uses; the filling itself is left to the FSW's own writes |
| `Partitions.FilePartitions45MB` (`:36`) | LUT: 180 files, holes via deletes, refill, partial reads | the same scan/deletion paths the tool's `corrupt-file` exercises; covered by X-4's boot part, no separate scenario |
| `Partitions.DDeleteOldestFilesOnFullFS` (`:72`), `E` (`:109`), `G` (`:152`), `H` (`:166`) | delete-oldest semantics on full partitions | no — needs direct FS API calls (`FSdeleteOldestFiles`); informs what the FSW does autonomously when an archive partition fills |
| `Partitions.AWriteMultipleFilesWithStream` (`:7`), `FMultiplePartitionsRangeSearch` (`:133`) | streamed writes; cross-CB range search + looped delete | no — direct FS API calls; nothing device-side to damage |
| `FileAlloc.AFill`/`BFillAndWrap`/`CSimpleWearLeveling`/`D1`/`D2` (`TestFileAlloc.c:7-158`) | allocation order, fill-and-wrap, wear levelling; D1+D2 restart an image with `Flash_initImg(..., false)` + `FSInit` | D1/D2's image-reuse-across-restart pattern is what every X-4 reboot leg does; A/B/C are FS-internal (allocation order), not mapped. AFill's 480-file expectation contradicts its own partition table — drift, see above |
| `FileRanges.*` (`TestFileRanges.c:12-102`) | file-range search semantics; `E` (`:91`) reads a real AX flash dump (`flash_ax_test_data2.fs`) | no — FS API only; `E` is further evidence the image formats differ |
| `FileManipulation.A-L` (`TestFileManipulation.c:7-129`) | CRUD on single files, name collisions, spare-type sensitivity | covered by X-3's `--self-test` (the tool's scan must respect the same 10-byte-name/`emptyPageMarker` invariants); no emulator scenario |

## Commits

Each commit is test-first: the qtests are written and seen RED against
the previous commit, then the code makes them pass. Per-commit gate set:
the suite (`make check-qtest-sparc`, stix only), checkpatch, boot-smoke
(**golden identical, required**), and `scripts/stix/flash-damage.py
--self-test` once it exists. The full set (soak, tc/nominal/fault/
detector/att smokes) at X-4/X-4b and before the merge.

### X-1: device damage model + sidecar (from boot)

- `grlib_stixflash.c`: the damage list (kind, value, mcm/chip/page
  scope, bit), `flash-damage-map` qdev (sidecar parse at realize, D-X3),
  `damage-list`, the three hit points in `stixflash_complete()`
  (erase-fail, program-fail, stuck-before-DMA), traces
  `stixflash_damage_load/_hit`. The reset handler is untouched; the
  sidecar list survives it.
- qtests (RED: `flash-damage-map` and `damage-list` are unknown
  properties before the commit — `qtest_initf` fails / QMP errors):
  - `flash-dmg-erase-fail`: sidecar with one block; erase → FAILED|READY
    after the full `tbers-us` (with `latency=on` explicitly), one IRQ,
    file bytes unchanged; other blocks unaffected;
  - `flash-dmg-program-fail`: program → FAILED|READY, file unchanged;
  - `flash-dmg-stuck0`: bit and block forms, both at value 0; the read
    returns the true page with the bit(s) cleared; a freshly
    programmed page also reads masked;
  - `flash-dmg-stuck1` (O4): the value-1 mirror — an erased page
    (0xFF) is unaffected, a programmed 0 on the bit reads 1;
  - `flash-dmg-marker-plain`: in `inverted-storage=off` mode, a
    hand-written marker in the qtest (0x00 at page offset 4096 — the
    tool does not exist yet at X-1, review finding 4) is read as 0x00,
    and in inverted mode the qtest's pre-inverted byte (0xFF) reads as
    0x00 — pins the inversion rule (correction 4);
  - `flash-dmg-quiet`: no map → `damage-list` empty and the four
    existing flash tests' behaviour unchanged (status, counters, no
    `stixflash_damage_*` trace lines);
  - `flash-dmg-map-parse`: malformed sidecar fails realize (subprocess
    test, like `flash-create-error`); missing file is fine.
- Gates: the per-commit set; boot-smoke golden must not move (no gate
  passes a map).

- **As built (2026-11-12, commit X-1).** Built as specified, with these
  details the plan left open:
  - Entry struct: `{kind, mcm, chip, page, block_scope, byte, bit,
    value}` — after the parse every entry is canonical (mcm/chip + page
    inside the chip); erase-fail stores the block's base page and
    `block_scope`, program-fail the exact page, stuck either plus the
    masked byte, bit and stuck value (O4: 0 or 1). The whole-block
    stuck form is "bit 0 of every byte", so it stores no byte.
  - Sidecar grammar (JSON keys, one entry each): `"block"` (linear
    block), `"page"` (linear page), or `"mcm"+"chip"` with `"page"`
    (chip-relative), `"block"` (block in chip) or `"block"+"page"` (the
    framework form). `"bit"` and `"value"` are stuck-only: `"byte.bit"`
    with byte < 4224, bit < 8; `"value"` 0 or 1 (O4), given with kind
    `"stuck"` — `"stuck0"` stays as an alias for value 0 and takes no
    `"value"`. The framework's `offset=<o>` has no sidecar key —
    the tool (X-3) resolves it to a bit before writing the file.
    Unknown keys are rejected. The v1 example of D-X3 parses as is.
  - The reserved-block rule is applied to the **containing linear
    block** of every kind (page scopes included), with no override.
  - `damage-list` rows: `src=file kind=erase-fail block=<linear>`,
    `src=file kind=program-fail page=<linear>`,
    `src=file kind=stuck value=<0|1> page=<linear> bit=<byte>.<bit>`,
    `src=file kind=stuck value=<0|1> block=<linear>` — FSW-linear
    index space, one row per entry, `\n`-separated, empty string when
    clean.
  - Traces: `stixflash_damage_hit` fires after `stixflash_op_done`
    (one per hit, with the final cmd_sta — FAILED|READY for the two
    failing kinds, READY for stuck; the hit's kind string is
    stuck0|stuck1 by value); `stixflash_damage_load(n, path)`
    fires once after a successful parse, n = 0 for an empty `"damage"`
    array. Both silent with no sidecar or a missing file. A
    damage-induced failure also goes through the normal
    `stixflash_error why=erase-fail|program-fail` line, like every
    other failure reason.
  - Realize order: the sidecar is parsed after the backing file opened
    and before the op timer is created; a missing file is silence
    (ENOENT only — any other open error fails realize), a sidecar over
    1 MiB fails realize, and the read is a plain fread (no mmap).
  - The read was split into `flash_read_file` (file stage, unchanged
    behaviour) and `flash_read_dma` (the D-F2/D-F3 DMA); stuck applies
    between the two. The reset handler is untouched; unrealize frees
    the list.
  - The qtests live behind one `stix_flashdmg_register()` call from
    `main()` (the attenuator's `stix_att_register()` pattern), with a
    dedicated 32 MB sparse flash per test (block 70 / page 5000 live
    above the reserved 64 blocks). `flash-dmg-marker-plain` hand-writes
    the marker bytes in both storage modes (F4). The two stale marker
    comments (correction 4 / doc-vs-code 5) are fixed. The
    `flash-dmg-quiet` command line enables `stixflash_op_done` so its
    -D log has a positive control.
  - RED expectations: the seven tests assert `flash-damage-map` /
    `damage-list` behaviour that a pre-X-1 binary cannot show (unknown
    -global property aborts QEMU at startup; the `damage-list` qom-get
    errors), so each can fail; `flash-dmg-marker-plain` fails if the
    inversion rule breaks; `flash-dmg-stuck1` (O4) is RED on a pre-O4
    binary — kind `stuck` fails the parse.

### X-2: runtime injection + platform contract

- `damage-add` / `damage-remove` (validation: kind known, block ≥ 0
  (≥ 64 unless `--allow-reserved` equivalent `allow-reserved` bool
  property, default false), specs reversible), `damage-list` gains the
  `src=runtime` rows; a rejected set errors and changes nothing
  (D-Ag's rule). Trace `stixflash_damage_hit` only.
- `faults-v1.json`: the reserved `flash.*` block (D-X5), comment-only
  until the platform adopts the IDs.
- qtests (RED: the properties are unknown): `flash-dmg-runtime-add`
  (inject → erase fails → remove → erase succeeds), `flash-dmg-reset`
  (a `system_reset` keeps the entries; the pending-op rules of D-F4
  still hold), `flash-dmg-reject` (unknown kind, block < 64, bad spec,
  remove of an unknown spec, remove `all`).
- Gates: the per-commit set; the probe self-test only otherwise.

- **As built (2026-11-12, commit X-2).** Built as specified, with these
  details the plan left open:
  - One parser: X-1's per-entry parse `stixflash_dmg_entry` gained an
    `allow_reserved` argument and is the only entry grammar — the
    sidecar passes `allow_reserved=false` always (D-X3, no override),
    `damage-add` passes the qdev `allow-reserved` property.
    `damage-remove` parses its spec with the rule lifted (removing
    injects nothing, so a removal must never be blocked by it). The
    string→JSON-object step is generic `qobject_from_json`; there is
    no second grammar.
  - `damage-add` value = one sidecar `"damage"` entry as a JSON object
    string. A spec that is already active (map or runtime) is rejected
    like an invalid one ("the spec is already active"), so the list
    keeps one row per active spec and the exact-spec remove is
    unambiguous. Runtime entries append after the sidecar's, in add
    order.
  - `damage-remove` value = `all` or the exact spec. Matching is by
    the canonical record (kind, scope, mcm/chip/page, byte/bit/value),
    so any written form of the same spec (linear or mcm/chip,
    `stuck` or the `stuck0` alias) removes it. Only `src=runtime` rows
    are removable: `all` = every runtime row, the sidecar rows stay
    (boot configuration, D-X3; they only go away by rebooting QEMU
    without the sidecar). Removing a spec that is not there errors and
    changes nothing (D-Ag); `all` is idempotent (with no runtime rows
    it succeeds and changes nothing). The list is capped at 65536
    entries (map + runtime); a damage-add beyond errors.
  - damage-list rows are `src=map|runtime ...`; the X-1 `src=file` was
    renamed to `src=map` (X-2 brief, D-X3 updated) and the five X-1
    damage-list assertions were updated with it. Row order: map rows first
    (sidecar load order), then runtime rows in add order.
  - An entry added or removed while an op is pending takes effect at
    that op's completion (the hit points run there, D-X3): an add can
    fail the op in flight, a remove cannot recall a hit that already
    ran. Both verbs run in the main loop like the completion timer, so
    the parse-and-append needs no locking. Stated at the verbs.
  - Runtime entries survive `system_reset` (D-X1: they model broken
    cells); the reset handler is untouched. D-F4 interaction, stated
    at the reset handler and pinned in `flash-dmg-reset`: the reset
    drops a pending op before completion, so an aborted op can never
    take a damage hit (no IRQ, no status, nothing committed); the
    guest re-issues the op after the reset and the damage applies to
    that one.
  - `faults-v1.json`: the reservation is whole-line `#` comments next
    to the attenuator's. The loader has no "reserved/disabled row"
    form — `validate` requires every real row to be complete and every
    real row is injectable and listed by `catalogue()` — so comments
    are the only non-injectable form, which is how the attenuator IDs
    are reserved. The three device rows name the qom action on
    `/machine/stixflash`, which the platform's `QOM_PATH` pattern
    (`stixplat/faults.py`, stixctrl*/stixspw only) does not reach yet:
    adopting them needs a platform amendment, hence comment-only
    (D-X5). The three offline rows are noted as setup-only with no
    action.
  - No new traces: `stixflash_damage_hit` fires for runtime hits like
    for sidecar hits (one line per hit), add/remove are silent.
    `flash-dmg-runtime-add` pins exactly one hit line for the failed
    runtime erase and none after the remove.
  - qtests: `flash-dmg-runtime-add` (inject → full-latency erase
    fails, nothing erased, one hit → remove the identical spec →
    erase succeeds), `flash-dmg-reset` (system_reset during a pending
    erase of the undamaged block 71 drops it per D-F4; the entry
    survives and still fails block 70 with the full latency and one
    IRQ), `flash-dmg-reject` (unknown kind, reserved block with
    `allow-reserved` off, no scope, non-JSON, unknown key, duplicate
    add, remove of an unknown spec, remove of the sidecar's own spec,
    each leaving damage-list unchanged;
    then `all` removing only the runtime row, and a second `all`
    succeeding with nothing to remove (idempotent); then the
    `allow-reserved=on` positive with the same instance pattern).
    Each is RED on a pre-X-2 binary: the verbs are unknown properties,
    the first `qom-set` errors and the leading assert aborts. O4's
    both polarities work at runtime for free through the shared
    parser (pinned for the sidecar in X-1's stuck0/stuck1).
  - Not implemented (the plan's "Later"): per-entry persistence
    (O2 — the controller re-applies after a cold power cycle, no
    device-side per-entry flag) and wear/presets (O3 — the explicit
    per-entry list only).

### X-3: the offline tool

- `scripts/stix/flash-damage.py` with the D-X4 subcommands and
  `--self-test` (offline, no QEMU): a small synthetic image (a few
  blocks, both storage modes) exercises marker/flip/scan/heal-free
  behaviour, the inversion, the EOF and cap guards, the shared-dir
  refusal, and the file-name scan; the self-test includes its own
  negatives (a wrong inversion must fail it). It also covers the
  `address` mode end-to-end (rewrite `pageAddress`, re-encode sub-block
  7, then decode with the same BCH to prove the page still reads) and
  pins the BCH(13,7) encoder against the committed reference vector
  (D-X4); both addressing forms must select the same byte, and the
  `device` subcommand emits the O4 stuck grammar (kind `stuck` with
  `value` 0|1, `stuck0` = the value-0 alias).
- RED: the self-test fails before the tool exists (CI runs it once the
  file lands; the commit's first build shows it green only with the
  tool).
- Gates: the per-commit set (no C change).

- **As built (2026-11-12, commit X-3).** `scripts/stix/flash-damage.py`
  (stdlib only) with `bad-block`, `corrupt-file`
  (`header|data|address`), `show`, `device` and `--self-test` (two
  synthetic sparse images, 197 checks, < 1 s, its own negatives), as
  D-X4 says. Notes:
  - The BCH(13,7) encoder and decoder were cross-checked against the
    FSW's own `ASW/src/MemoryMgmt/BCH.c`, compiled standalone on the
    host: the encoder is byte-identical on five patterns, and
    `BCH_decode` vs the tool agree on 1/7 data flips (corrected), 8
    flips (uncorrectable), one ECC-bit flip and 6 data + 1 ECC flips.
    The two reference vectors are committed in
    `scripts/stix/tests/bch13t7-reference-vector.txt` and pinned by
    the self-test. Review fixes before the commit: the parity bits are
    MSB-aligned in the 12 ECC bytes (the 5 pad bits trail), which the
    self-test's independent division and the decoder's syndrome step
    had wrong.
  - `corrupt-file --mode header|data` verifies its deterministic flip
    pattern with the in-tool decoder before writing (> 7 flips can
    miscorrect instead of failing; the tool never writes a pattern
    that would take the corrected path when an uncorrectable one is
    asked for).
  - FS B spans blocks 32832-34751 (`MemFilesystem.c:494-500`; both
    partitions are 1920 blocks, matching the scenario's bad = 3840);
    the earlier 34671 in this plan was wrong.
  - Marker writes are single-byte (aux byte 0 of pages 0/1), not the
    FSW's full-page program: the same guest view, sparser on disk.

### X-4: `scripts/stix/flash-damage-smoke.sh`

- On the gate conventions (`--dry-run`, `--check-guard`, slot library,
  private flash, SKIP = exit 77, SKIP listed separately [D]):
  - **boot part** (tool, offline, before QEMU): fresh private flash;
    `bad-block --block 610` (TMMGMT, blocks 604-783) + `corrupt-file`
    on a LUT + `corrupt-file --mode address` on CONTEXT1's first data
    page; expect the 0x543B for the CONTEXT1 read (the init task reads
    the contexts first, `MemFilesystem.c:1027-1042`), 0x5439 for the LUT
    (on AX too — await it; the AX boot itself has no 0x8030), and
    0x0032 with bad=1 and error=1, then hold;
  - **check part** (device, runtime, over QMP): fresh flash; boot;
    `damage-add program-fail` on page 4162 (block 65 in **CONTEXT0**,
    the partition the next context write draws from:
    `File_findFreeBlock` continues after `mostRecentlyWrittenBlock` =
    the context file's node 0, `File.c:163` — the exact block and a
    page the write really reaches are confirmed by the probe run before
    the expectation freezes [L]; page ≥ 2 of the block keeps the FSW's
    marker pages 0/1 clean, correction 1; erase-fail would not fire on
    a free erased block — no erase is attempted before the write,
    review of X-1, finding 1); trigger a context write (CONF entry,
    nominal-smoke's TC bytes); TC(238,6) `{0, 3839, 1}` (start/end file
    u16 + badBlockHandling u8; layout per ICD:23981-24078 and
    `TCData.h:429-431`, built with nominal-smoke's TC helpers) →
    TM(238,7) bad_after=1; reboot (watchdog or FORCE) → 0x0032 bad=1
    (persistence, the D-X1 point);
  - **stuck part**: `corrupt-file --mode data --bits 8` → 0x543B with
    the page address from the tool's `show`;
  - `--negative`: run the boot part undamaged — the bad=1/error=1
    expectations must fail (proves the gate is not vacuous), exactly
    one named expectation each.
- Every [L] payload is confirmed by a probe run before it becomes an
  expectation (the Phases 3/4 pattern).
- Gates: the full set plus flash-damage-smoke ±.
- **As built (2026-11-12, commit X-4).** The live probe runs changed
  the design in seven places; all of them are pinned live:
  1. **File types and the spare byte.** The tool's type codes were the
     header comments (context0 8, ...); the enum is sequential, so
     context0 = 6, context1 = 7, lut = 8, flare = 4, user-requests = 5
     (FSW finding F18). Context names carry the build's CONTEXT_VERSION
     in the spare byte (0x22 on the AX gate ELF, 35 in the source), so
     `show`/`corrupt-file` take `--spare any`.
  2. **Flight BCH (FSW finding F19).** `encode_bch` byte-swaps every
     32-bit word unconditionally (`BCH.c:300-310`), so on the LEON the
     stored ECC is the standard BCH of the word-swapped sub-block, and
     `BCH_decode` applies its fixes at swapped-stream indices: a real
     error in byte b is "corrected" in byte b^3 (ok, fixes = 1, two
     wrong bytes). Pinned by the master flash's stored header ECC and a
     LEON-faithful build of `BCH.c` (`tests/bch13t7-leon-vector.txt`).
     The tool's `bch_encode`/`bch_decode` are the flight forms; the
     standard core stays pinned by the x86 vector. Consequence for the
     outcome rows: a "correctable" flip is NOT corrected on flight — the
     page reads back with the original error plus the mirrored one, and
     the >= 4-corrections rewrite path stores the miscorrected data with
     a fresh, valid ECC. The X-4b correctable flavour's expectations
     need re-deriving from this before it graduates.
  3. **Seed wait.** The NOMINAL-entry re-store frees the old blocks and
     the model's erase deallocates them, so the file's allocation (du)
     stays put; the seed waits on CONTEXT1's logicalWriteTime instead
     (2 -> 4 live).
  4. **Boot-time context damage is fatal (FSW finding F20)**, so the
     boot part carries only the bad-block and the LUT legs (0x0032
     bad=1), and the address and uncorrectable damage are two parts of
     their own, `addr` and `stuck`. INIT waits for the FILE task's
     context reads (`MemFilesystem.c:542-544`, `:1038-1051`) before
     `TMBuffer_init` (`main.c:136-140`); the read error's 0x543B report
     hits the uncreated TMBuffer semaphores -> ASSERT (`TMBuffer.c:276`)
     -> Halting -> trap -> reset, every boot while the file is damaged.
     Pinned: QEMU exits by itself under -no-reboot, the UART has the
     read-error line ("address corrupt" / "Unrecoverable error in
     file"), the TMBuffer ASSERT and no "init: TM buffer"; the ASSERT's
     backtrace (0x543B in some frame) is printed only sometimes and is
     reported, not pinned. The plan's "0x543B on the CONTEXT1 read" is
     therefore unobservable as TM on any boot.
  5. **Check page.** A context file is one page, so the store programs
     page 0 only: the injection is page 0 of the first free CB0 block
     (block 67 live), not page 2 (which never fired). The FSW marks the
     failed block in pages 0 and 1 and the scan reads both.
  6. **TC(238,6) is MAINTENANCE-only** (`TCVerifDynamic.c:97`; TM(1,8)
     48301 "wrong operation mode" live in NOMINAL), and MAINTENANCE is
     entered only from SAFE (`OpModeMgmt.c:86-95`): the check part goes
     NOMINAL -> SAFE -> MAINTENANCE first.
  7. **badBlockHandling = 0.** With 1 the check erases the grown-bad
     block; a program-fail-only block erases fine, so it healed before
     the reset (live: bad_after=1, then 0x0032 bad=0, block 67 all
     0xFF). With 0: bad_after=1 and the second boot's 0x0032 bad 0 -> 1.
  The QMP `damage-list` read-back is now actually requested (qom-get at
  +36 s; the first version grepped for a read-back it never asked for).

### X-4b: framework-derived scenarios

The four scenarios the unit-test framework motivates (see the scenario
catalogue), as optional flavours of flash-damage-smoke (one QEMU run
each; "uncorrectable" reuses the stuck part's boot):

- **correctable** (`FileCorruption.ABitflips`): `corrupt-file --bits 3`
  on a TMMGMT file → reads succeed before **and** after a reboot, no
  TM, the "BCH corrected N SEUs" TRACE on the UART; `--bits 5` variant
  additionally exercises the silent remove-and-rewrite
  (`MemFilesystem.c:604-655`) — after the reboot `show` finds the file
  on a fresh block [I → L];
- **uncorrectable**: the stuck part's `--bits 8` → expected 0x543B
  (BCH `t = FILE_ECC_BCH_CORRECTIONS = 7` corrects ≤ 7 per sub-block;
  beyond that `BCH_decode` fails or miscorrects — which of the two the
  probe shows decides the expectation, `File.h:106-122`) [I → L]. The
  device-kind route to 0x543B is X-2 runtime injection of ≥ 8
  page-scope stuck entries in one sub-block of an already-catalogued
  file; a whole-block stuck at boot never reaches a read — the init
  scan classifies the block from the masked spare first (value 0 →
  BAD, value 1 → FREE, the file is "not found", review of X-1
  finding 2);
- **broken flash** (`Initialization.BBrokenFlash`): `bad-block
  --fs-range` → 0x0032 with bad=3840 (0x0F00), used=0, free=61696
  (0xF100 — the free count spans the whole device, `MemFilesystem.c:1064`),
  plus 0x8030 `(1,1)` (both context blocks bad) and 0x5439 (LUT) [L];
  marking the *whole* device would wrap the u16 bad count (65536 & 0xFFFF
  = 0) — the scenario stays inside the FS-scanned blocks;
- **full FS with grown bad blocks** (`Partitions.FilePartitions30MB`
  arithmetic): soak archive writes with program-fail injected on the
  first page of free blocks of the written partition (erase-fail is
  dormant on free erased blocks — `write1` erases only non-0xFF
  blocks, review of X-1 finding 1); TM(238,7) shows bad growing 2 per
  exhausted write, and the partition finally reports filled/`FS_FULL`.
  Nuance from the framework's quota tests: once the partition is full
  `File_write` returns `FS_FULL` **before** burning blocks
  (`File.c:553-554`), so grown bad blocks only accumulate while a free
  block remains [I → L].
- X-4b lands with X-4; each flavour is optional and probed (payload
  confirmed live) before its expectation freezes. Gates: the full set
  plus flash-damage-smoke ± (the run flavours when exercised).

### X-5: docs

- `accepted-diffs.md`: **"## 43. Flash damage injection (baseline: the
  NAND is always perfect)"** — the model semantics (the stuck kind's
  value 0|1 per O4, the marking-erase nuance), the sidecar/grammar (both
  addressing forms, the framework mapping), the tool (incl. the
  BCH(13,7) encoder for the `address` mode), the catalogue mapping.
- `ground-station.md` "Fault injection": the flash subsection (QOM
  recipes, the tool recipes, the expected EIDs, the `-S` + `damage-add`
  + `cont` pattern for damage from boot).
- `baseline/README.md`: the suite line gains flash-damage-smoke.
- This plan's "As built".

## Goldens and gate impact

- **All three goldens stay byte-identical in every commit.** No gate or
  golden runs with damage configured: the sidecar default is off, the
  tool only ever touches private copies, and the new traces
  (`stixflash_damage_*`) never fire without damage. X-1 re-runs
  boot-smoke and diffs `qemu-stdout` manually (no `stixflash_damage_`
  lines expected); if anything differs, stop and report.
- `uart.golden.log`/`tm.golden.log` are not regenerated; the boot cut
  (`boot-smoke.sh:89-93`) is untouched.
- **New gate**: flash-damage-smoke (±, ≈3 QEMU runs of ~60 s each plus
  tool time for the main parts; each X-4b flavour is one more run; AX
  ELF only in v1 — the PFM variant is O1).

## Merge order and overlap

- This branch already sits on the flash-latency base; X-1..X-5 land on
  `stix-uprev-flashdmg` and rebase onto the integration tip as usual.
- **Memory-upset track** (`stix-uprev-mem`, currently at the same HEAD
  `083d3d4365`): no shared device file (it works in `grlib_stixctrl.c`
  EDAC counters per R-a [D]); shared: `accepted-diffs.md` numbering
  (this plan takes §43; the next track takes the next free number at
  its merge), `tests/qtest/stix.c` appends (each track behind one
  `stix_*_register()` call, as the attenuator landed), and the EID
  naming conventions. R-b (flash SEU) is dropped from the mem track's
  scope in favour of this plan (D-X7) — to be confirmed with that
  track's owner (O6).
- **Attenuator track** (`stix-uprev-att`): `faults-v1.json` is the only
  shared file — the flash block is a separate hunk next to the
  attenuator's reservation. No `grlib_stixctrl.c` or `stix_att.c`
  overlap.
- **Platform track**: consumes, does not block. The `qom` action exists
  (SF8 [D]); the `hooks.*` pre-boot step (D-X5) is a small ask to
  schedule with that track; nothing in X-1..X-3 waits for it.

## Risks and checks

- **`File_CONFIG_MAX_BB` amplification**: one erase-fail injection can
  make the FSW mark up to 2 blocks bad per `File_write` call, and
  `MemFilesystem_write` retries the whole write twice
  (`MEM_FILESYSTEM_MAX_RETRIES = 2`, `MemFilesystem.h:25`) — so up to
  **4** blocks per file write, and `FSW_ERROR_FILE_CORRUPTED` only
  after 4 damaged free blocks. A long scenario against a nearly-full
  partition could burn many blocks; once the partition is full
  `File_write` returns `FS_FULL` before burning anything
  (`File.c:553-554`). The gate injects once and checks once; scenarios
  are advised to target free blocks.
- **The marking erase also fails** under erase-fail (correction 1):
  accepted, documented; the qtest `flash-dmg-erase-fail` pins that the
  marker program still lands.
- **generateTMs is FALSE inside the TC(238,6) scan reads**
  (`MemFilesystem.c:900-902`): corrupted files found by the scan erase
  silently (no 0x543B). Outcome rows that need 0x543B damage files
  outside the scanned range or read them at boot.
- **ASSERT(0) paths**: `Read_page`'s unexpected-status path
  (`MemFilesystem.c:1227`) is unreachable by design (no read-fail
  kind); `File_read`'s `ASSERT(page->aux.badBlockMarker == 0xff)`
  (`File.c:689`) fires if a marker flips *after* the catalog was built
  but before the read — not producible in v1 (no runtime marker
  writes); noted so nobody adds one.
- **Scan cost** (the tool on a populated 16.5 GB image): bounded by the
  FS region and the hole-friendly chunked read [I]; measured in X-3's
  self-test notes.
- **Determinism vs the sparse file**: a damaged page turns holes into
  allocated blocks (4 KiB each); the cap check catches a runaway; the
  boot scan of a file with many factory bad blocks is *faster* (2 reads
  per block either way, no change).
- **The `-trace 'stix*'` golden rule** (D-X6): checked per commit.

## Doc vs code

Code wins in each case.

1. `Flash.h:33-39` says bad blocks must never be erased or written;
   `File_write`'s own marking erases and writes them (`File.c:592-598`).
   ✔ code: the model allows erasing any block; the FSW does what it
   does.
2. `File.h:198-205` documents `E_FILE_RES_ERROR_FLASH/_PROGRAM`; the
   enum has only `FS_FULL/ERROR/ERROR_FILE_NAME` (`File.h:164-168`).
   ✔ code.
3. `File.c:37-55`'s spare-layout comment (CRC16 at D+2, size at D+32)
   is stale against `File_auxPageData_s` (`File.h:128-140`).
   ✔ struct.
4. `TMForm_sendFilesystemInitFinished`'s parameter is named
   `in_totalFiles` but receives `Flash_usedBlocks`
   (`MemFilesystem.c:1064`). ✔ code (the golden's `0002`).
5. `grlib_stixflash.c:446` (and the qtest comment at
   `tests/qtest/stix.c:404-405`) call a hole "0x00 = bad-block marker";
   with the default inversion a hole reads 0xFF. ✔ code (both storage
   modes pinned by `flash-dmg-marker-plain`); the comments are
   corrected in X-1.
6. `FilesystemDefinition.h` and the model both derive 16.5 GB from
   4096+128/64/4096/8/2 — the FSW's "1'107'296'256" byte figure for one
   device is 1,056 MiB rounded oddly (`:22`); the model's
   `FLASH_DEVICE_SIZE` is the same product. ✔ consistent; nothing to
   do.
7. The TNs name the "data block written at the wrong address" aspect
   and require an error message; the FSW's detection is the spare
   `pageAddress` check (`File.c:691-699`), but the read path I traced
   reports it as **0x543B** (`MemFilesystem.c:589-592`),
   `TMForm_inconsistent_flash_address` (0x543A, `TMForm.c:678-686`)
   notwithstanding — no sender for it exists in File.c, MemFilesystem.c
   or Flash.c. ✔ code (0x543B on the traced path); the X-4 boot part
   expects whichever event the probe run shows [L].

## Later (planned, not in X-1..X-5)

Resolved out of O2 and O3; nothing here gates X-1..X-5.

- **Per-entry persistence (O2).** The platform controller re-applies
  runtime damage after a cold power cycle (default, D-X1). A later
  per-campaign setting marks, per damage entry, "pre-programmed /
  persistent" (reloaded at every boot) vs "transient" (gone at the
  next cold cycle): one field in the sidecar plus the controller's
  re-apply rule.
- **Wear and pre-broken presets (O3).** A deterministic wear/aging
  kind (fail the Nth erase/program of a block) and "permanently broken
  before the run" presets for campaigns.

## Open questions for the user

- **O1.** ~~Which ELF should flash-damage-smoke target by default — the
  AX gate ELF (proposed; the damage is fully visible) or a PFM ELF, or
  both (PFM as an opt-in flavour like pfm-boot-smoke)?~~ — answered
  2026-11-12: the AX gate ELF by default; a PFM flavour is opt-in
  (like pfm-boot-smoke).
- **O2.** ~~Should runtime damage survive a cold power cycle via the
  controller's re-apply (proposed, D-X1), or is per-process lifetime
  wanted for some scenarios?~~ — answered 2026-11-12: the controller
  re-applies runtime damage after a cold power cycle (default, D-X1);
  a per-campaign per-entry "persistent vs transient" setting is
  planned later (see "Later (planned, not in X-1..X-5)").
- **O3.** ~~Is a deterministic `wear` kind (fails the Nth erase/program
  of a block) wanted in v1, or is the explicit per-block list enough?~~
  — answered 2026-11-12: no wear kind in v1, the explicit per-block
  list is enough; wear/aging and "permanently broken before the run"
  presets are planned later (see "Later (planned, not in X-1..X-5)").
- **O4.** ~~Confirm the stuck-cell polarity assumption: a stuck cell
  only reads 0 (never 1) [A]. If the FPGA reports otherwise the
  `stuck0` kind generalises to a mask.~~ — answered 2026-11-12: stuck
  cells have BOTH polarities; the kind is now `stuck` with `value`
  0 or 1 (the sidecar keeps `stuck0` as an alias for value 0); a stuck
  bit reads its stuck value on every read, the stored file is
  untouched.
- **O5.** ~~The TC(238,6) unit bytes~~ — answered from the documents:
  the layout is `{startFile u16, endFile u16, badBlockHandling u8}` per
  the TM/TC ICD §7.3.6 Table 192 (ICD:23981-24078) and `TCData.h:429-431`;
  X-4 builds `{0, 3839, 1}` with nominal-smoke's TC helpers (the ICD's
  "DO NOT SET TO 1 UNLESS UNDERSTOOD WHY" note is why the gate uses
  badBlockHandling=1 only where the check intends it). Remaining: a live
  probe run confirms the TM(238,7) payload and ack behaviour before
  X-4's expectations freeze; the FID_48447 range error
  (`MemFilesystem.c:801`) is the fallback matcher.
- **O6.** ~~R-b (flash SEU) moves under this plan (D-X7)~~ — confirmed
  2026-11-12 by the orchestrator, who runs both tracks: the memory-upset
  plan (branch stix-uprev-mem) excludes the NAND flash.
- **O7.** Any override of D-X1..D-X7.

## Out of scope

- A filesystem-aware runtime interface (file names in the device); the
  tool owns names, the device owns blocks/pages.
- Wear levelling/page reallocation inside the device; program-disturb
  simulation (partial-page programming); multi-plane and SLC-cache
  behaviours of real parts.
- Probabilistic/noisy fault modes and `savevm` checkpoints of damaged
  state (R-r3 needs vmstate, which this device deliberately lacks).
- The platform controller's `flash.damage` hook implementation and UI
  (that track); a `view.flash` browser (R-c); the SDRAM/BRAM EDAC
  faults (memory-upset track).
