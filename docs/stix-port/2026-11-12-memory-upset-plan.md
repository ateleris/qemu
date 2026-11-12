# Memory upsets (SEU/DEU/MBU) in the STIX memories, with EDAC: plan

Status: draft 2026-11-12, revised the same day after the independent
plan review (`.review-mem.txt`, resolutions at the end). Base: track
worktree `~/dev/qemu-stix-mem`
(branch `stix-uprev-mem`, from `083d3d4365`, the same tip as
`stix-uprev-flashdmg`). The user wants single-bit (SEU) and
double/multi-bit (DEU/MBU) upsets injected into the instrument's
memories, with the EDAC machinery the flight software can observe. The
scope is fixed by the user (2026-11-12):

- **IN:** (1) the processor SDRAM (FTSDCTRL16 EDAC, `E_EDAC_MODULE_SDRAM`,
  reboot mask 0x2); (2) the FPGA "CACHE" SRAM, 2 MB
  (`STIXCTRL_SRAM_SIZE`: QL accumulators, ELUT/TLUT; CacheCmdSta
  EDACON/EDACRESET, `E_EDAC_MODULE_CACHE`, reboot mask 0x4); (3) the
  rotating-buffer SDRAM (RotbConfig EDACON, RotbCmdSta EDACRESET,
  `E_EDAC_MODULE_ROTBUF`); IRQ_STIXCTRL_EDAC (6) is shared by (2) and
  (3) (`StixIdpu.h:1547`); (4) the BRAM: FPGA block RAM that survives an
  IDPU restart (FSW `MemoryMgmt/MemBram.c`, `MemBram.h`), **no EDAC** —
  an upset there is silent corruption the FSW sees on the next boot.
- **OUT:** EEPROM/PROM (the boot image is immutable, no upsets; its EDAC
  register behaviour stays exactly as it is today). The NAND flash is a
  separate track (`~/dev/qemu-stix-flash`, branch `stix-uprev-flashdmg`,
  plan `docs/stix-port/2026-11-12-flash-damage-plan.md` there) —
  referenced only in "Merge order and overlap".

Ownership follows the platform split (attenuator plan status block): the
platform track owns the fault-ID namespace and the catalogue; this track
owns the model and its QOM surface under `/machine/stixctrl/edac`.

Sources:
- the model: `hw/sparc/grlib_stixsdram.c` (90 lines),
  `leon3.c`, `grlib_stixctrl.c`, `stix_det.[ch]`, `StixIdpu.h`,
  `tests/qtest/stix.c`, `scripts/stix/*`;
- a read-only pass over the FSW, `~/dev/STIX-FSW/ASW`. Paths are
  relative to it: **BSW** = `BSW_ASW/src/BSW/...` (so `EDAC.c` below
  means `BSW_ASW/src/BSW/EDAC/EDAC.c`), **ASW** = `ASW/src/...`. The
  worktree sandbox limits Bash to the repo, so files are read by path;
  the two greps the draft owed are resolved (corrections 1-2, Review
  resolutions);
- the attenuator plan (`~/dev/qemu-stix-att/docs/stix-port/
  2026-11-11-attenuator-plan.md`, read in full) for structure, the
  truth/presented/forcing layering and the as-built qtest pattern;
- the platform plan (`~/dev/qemu-stix-platform/docs/stix-port/
  2026-11-11-platform-plan.md`): D-PLo (hw-state tree), D-PLp (truth vs
  telemetry), D-PLq (the catalogue) and
  `scripts/stix/platform/catalogue/faults-v1.json` (action kinds; the
  reserved attenuator block shows the "qom on /machine/stixctrl*"
  convention).

Fact tags (as the attenuator plan):
- **[C]** read in code (FSW or model), with file:line;
- **[E]** read in the gate ELF (`nm`) — not needed so far;
- **[G]** seen in a gate artefact;
- **[D]** a document says it (code wins);
- **[I]** inferred — a named knob or a single constant, cheap to change;
- **[A]** an assumption the user has to accept;
- **[L]** to be checked live before it becomes a gate expectation.

## Goal

1. One mechanism injects bit upsets into the four in-scope memories and
   the EDAC hardware reacts the way the ICD/FSW expects:
   - **SEU (single bit, correctable):** the read data is corrected, the
     per-module SEC counter counts, and the FSW's periodic SEU report
     and scrub-rate algorithm see the counts;
   - **DEU/MBU (uncorrectable):** SDRAM raises the SPARC
     data-access-error trap (0x09) that ends in a watchdog reboot;
     CACHE and ROTBUF latch DEDETECTED, pulse the shared EDAC IRQ 6 and
     return corrupted data; the FSW counts them into SID2;
   - **BRAM:** no EDAC — the flip is returned raw, with no status and no
     event, until the FSW reads the corrupted record after the next
     boot.
2. Three layers over QMP `qom-get`/`qom-set` on
   `/machine/stixctrl/edac`: **truth** (the host RAM bytes, flipped),
   **presented** (what the EDAC logic returns and reports), and the
   injection itself (a physical event, not a register forcing).
3. With nothing injected, the model is byte-invisible: the goldens and
   every existing gate stay unchanged (M-1's declared boot-print line
   excepted, see Goldens).
4. A qtest per commit (in the existing `stix` binary), and an optional
   live `edac-smoke.sh` gate; the platform catalogue gets one entry per
   memory/kind.

## Corrections to the brief

These change what the track can deliver. Read them first.

1. **BRAM persistence is already modelled, and it answers the brief's
   question.** The stixctrl realize maps the LVAR window as a 0x400-byte
   plain RAM that covers LVAR0-3 *and* the BRAM areas behind them — the
   plan comment names both `WDT UNCHANGED_STORAGE 0xE02000A8` and
   `MemBram 64 B ending 0xE0200400` (`grlib_stixctrl.c:1051-1056`), and
   device reset deliberately does not clear it ("plain RAM, zeroed at
   power-on only … the FSW's BRAM keeps the reboot-persistent state",
   `grlib_stixctrl.c:1117-1120`). A fresh QEMU process zeroes it
   (`memory_region_init_ram`, `:1059-1060`) — exactly the "not reset
   after reboot, only after power cycle" contract the FSW documents
   (`commonFSW.h:228`). So a BRAM upset only needs a host-byte flip; the
   reset/power-cycle semantics come for free. What the FSW *does* with a
   corrupted record is now answered: the ASW-visible BRAM reader is the
   WDT boot record — `WDT_callAfterReset` reads `UNCHANGED_STORAGE`
   0xE02000A8 at **every** boot (`Watchdog.c:54` → `WDT.c:239-263`) —
   while `MemBram.c`/`MemBram.h` are setters only (`MemBram.h:10-11`),
   so the 0xE02003C0 struct is a SuSW-facing record no ASW code reads
   back. The FSW-visible BRAM outcome therefore lands on the WDT area
   (M-5); the MemBram record row reports nothing.
2. **Only the SDRAM has a reboot path.** For CACHE and ROTBUF the EDAC
   ISR counts and resets, and that is all (`EDAC.c:62-83`,
   `FDIR/EDAC.c:110-135`); nobody reboots. The reboot happens for
   SDRAM because an uncorrectable SDRAM error is an AHB error, which
   becomes SPARC trap 0x09, whose handler forces the watchdog
   (`WDT.c:131-134` comment, `:151`, `:182`). The `MemBram_
   setEdacRebootModule` mask (0x2 SDRAM / 0x4 CACHE, `MemBram.h:7-8`)
   is written by someone I could not find in ASW (the caller grep stays
   owed — the `MemBram_setOperationMode` writers are
   `OpModeMgmt.c:703,954` — but it cannot change this verdict) — the brief's
   "who decides to reboot on a DE" is answered as: **the trap handler,
   unconditionally, for SDRAM only**.
3. **Nothing traps on a bad access today, and the FSW's trap handler
   reads an AHBSTAT register the emulator does not map.** With the MMU
   off (LEON3 resets MMU-off, `target/sparc/cpu.c:77`; ASW runs
   MMU-off) an unassigned access **silently completes**:
   `sparc_raise_mmu_fault` raises only with MMU_E
   (`target/sparc/ldst_helper.c:469`). So the HWSelftest trap selftest
   (`HWSelftest.c:277-282`, a TC-triggered store to 0xF0000000) does
   **not** trap today, and `storeBadTrapInfo`'s read of 0x80000F00
   (`WDT.c:119`) returns ~0 harmlessly. D-Ud therefore raises trap
   0x09 from the overlay itself and must not touch the generic
   unassigned-access behaviour; the HWSelftest trap test is a separate,
   optional item (M-3).
4. **SEUs are invisible in short gates by default.** The periodic SEU
   report (TM(5,2) **0x542F**) runs every `E_PARAM_ID_EDAC_
   SEU_REPORT_PERIOD_S` seconds, default **1000** (`ParamMgmtDef.h:195`,
   `FDIR/EDAC.c:148,164`). A live SEU gate must shorten that parameter
   or run for minutes (Q4). The DE paths (trap, IRQ, SID2 counters) are
   immediate and gateable.
5. **The scrub-rate algorithm is counter-driven, not time-driven.** It
   reads the SEC counters once per report period, compares against
   nominal/high thresholds (SDRAM 14/43, CACHE 25/54 per second,
   `ParamMgmtDef.h:196-199`) and can send TM(5,1) **0x0030** with
   `[module, newRate, oldRate, counts, threshold, period]`
   (`FDIR/EDAC.c:193-281`, `TMForm.c:570-587`). The hardware SCRUBRATE
   field itself is only stored/read (`EDAC.c:189-237`); nothing in the
   FSW observes a scrub *happening*. So the model stores SCRUBRATE and
   does not simulate a scrub pass (D-Ui).
6. **On the gate (AX) ELF the EDAC machinery runs, with EDACON at the
   reset values.** `EDAC_init` runs from `FDIR_init` (`main.c:154-156`,
   `FDIR.c:372-374`) on every build, the EDAC IRQ is caught
   (`EDAC.c:100-105`), and SID2 is real — so the CACHE/ROTBUF DE
   counters (SID2 status1) and the SDRAM trap/reboot are visible on AX,
   unlike the attenuator. Nothing in ASW ever sets an EDACON bit: the
   SDRAM enable was removed (`RamUtils.c:54`, `#if 0 removed on request
   2016-06-21`), TC(236) is off-only for SDRAM (`HWConfig.c:77-82`),
   `EDAC_init` only resets (`FDIR/EDAC.c:91-92`) and the boot bring-up
   is commented out (`main.c:205-222`). So the gate ELF boots with
   EDACON at the hardware reset values — per STIX-ICD-0026: CacheCmdSta
   EdacOn resets to **1** (txt:2119), RotbConfig/Sdram:Edac EdacOn to 0
   (txt:2202, txt:3249; RP-0067: SDRAM EDAC disabled by default) — and
   whether SuSW enables any of them before hand-over is user question
   Q1 (the model supports both, D-Uk). M-1 fixes the model's
   cacheCmdSta reset to the ICD value (today it reads 0 — an M-1 fix
   with a declared boot-golden impact). `EDAC_init` does write the
   EDACRST/EDACRESET strobes to all four modules (`FDIR/EDAC.c:90-92`,
   `EDAC.c:283-313`) — the trace golden shows those writes (M-1
   checks, Q2).

## Key facts the plan rests on

### Register maps (the model's `StixIdpu.h`) [C]
- Bases: stixctrl APB 0x80000500, SDRAM controller APB 0x80000C00
  (`StixIdpu.h:223-227`). AHBSTAT (FSW-side only) 0x80000F00
  (`WDT.c:80`).
- **SDRAM FTSDCTRL16** (`ftSdCtrl16Regs_s`, `StixIdpu.h:1587-1591`):
  configStatus 0x80000C00, **edac 0x80000C04** with DEDETECTED 31,
  EDACON 12, EDACRST 13 (strobe), SCRUBRATE 9:8 (off/slow/med/fast),
  SECCOUNT 7:0 (`StixIdpu.h:539-562`).
- **CACHE CacheCmdSta** 0x80000550: OPMODE 0 (0 = cache mode),
  SCRUBRATE 5:4, SECCOUNT 15:8, EDACON 16, EDACRESET 17 (strobe),
  DEDETECTED 18 (`StixIdpu.h:742-771`).
- **ROTB RotbCmdSta** 0x80000554: COMMAND 1:0, EDACRESET 2 (strobe),
  DEDETECTED 4, READY 3, FULL 5, BLOCKCOUNT 23:7, SECCOUNT 31:24;
  **RotbConfig** 0x80000558: EDACON 12, TESTMODE 13, REFRESHRATE 11:4,
  ENABLE 31 (`StixIdpu.h:1188-1232`, ICD txt:2213; the draft said
  22:6/19:12 — wrong; the model uses the macros and is right).
- **IRQ_STIXCTRL_EDAC = 6**, shared: "Errors (DED) in Detector's Cache
  Memory or in Rotating Buffer's SDRAM memory. Cache EDAC interrupt is
  reported if OpMode of Cache memory is set to 0" (`StixIdpu.h:1547`).
- The FSW view of memory: SDRAM 0x40000000-0x43300000
  (`commonFSW.h:206-207`), SRAM/CACHE 0xE0000000 +2 MB with the ELUT
  window 0xE0000000-0xE017FFFF, LACC0-3 at 0xE0180000+0x6000-strides,
  TLUT0/1 at 0xE01B8000/0xE01B9000 (`StixIdpu.h:252-298`), LVAR0-3
  0xE0200000+0x100 strides (`:302-320`), BRAM struct at 0xE02003C0
  (`commonFSW.h:228`).

### The FSW's EDAC machinery [C]
- **Modules** EEPROM/SDRAM/CACHE/ROTBUF (`EDAC.h:59-67`); the
  function/module matrix (`EDAC.h:29-38`): scrub rate only SDRAM+CACHE;
  DEDETECTED readable for SDRAM/CACHE/ROTBUF. The callback contract:
  called inside the ISR, then the module is reset (`EDAC.h:153-163`).
- **One ISR for IRQ 6** (`EDAC.c:51-87`): checks
  `EDAC_getDeDetected(CACHE)` then `(ROTBUF)`, calls
  `EDAC_callback(module)` and `EDAC_reset(module)` per hit, then clears
  the IRQMP pending bit. The comment records that SDRAM has no IRQ —
  "an AHB error (which leads to TRAP) is reported" (`:69-76`).
- **EDAC_callback** (`FDIR/EDAC.c:110-135`): CACHE →
  `cacheDoubleErrorHKcounter++`; ROTBUF → counter +
  `rotbDoubleErrorFlag = TRUE`; SDRAM → nothing (comment).
- **EDAC_init** (`FDIR/EDAC.c:79-101`): `EDAC_reset` for all four
  modules, then `EDAC_enableInterrupt(TRUE)`.
- **Periodic SEU report** (`FDIR/EDAC.c:140-190`): every
  `E_PARAM_ID_EDAC_SEU_REPORT_PERIOD_S`, for every *enabled* module
  (`EDAC_isEnabled` gates it, `:155-158`): read SEC count
  (`EDAC_getSecCount`, `EDAC.c:240-262` — register fields above), if
  non-zero: put it in `optionalParam[module]`, `EDAC_reset(module)`, and
  send **TM(5,2) 0x542F** with the collected counts (`:186-189`, 4
  params, index 0=EEPROM..3=ROTBUF via `:178`).
- **Scrub-rate algorithm** (`FDIR/EDAC.c:193-281`): only CACHE/SDRAM;
  rate off → return; `counts/period_s` ≥ high threshold → MAX, ≥ nom →
  NORMAL, else SLOW; on change `EDAC_setScrubbingParam` + **TM(5,1)
  0x0030** (`TMForm.c:570-587`).
- **EDAC_setEnable/isEnabled** are register bit pokes (`EDAC.c:109-186`);
  **TC(236) config**: `E_IDPU_PARAM_CONFIG_ENABLE_EDAC` turns SDRAM EDAC
  *off only*, toggles CACHE (0x4) via `QLAccudDblBuffs_EDAC_on` and
  ROTBUF (0x8) via `RotAccumulator_EDAC_on`, and stores the param masked
  to 0xC (`HWConfig.c:74-113`); scrub rates are TCs 5/7
  (`HWConfig.c:115-119,125-129`, `HWConfig.h:29-34`).
- **CACHE EDAC on** (`QLAccuDblBuffs.c:340-367`): reset + enable, then
  clears QL, nominal accumulators, TLUT and ELUT — "TLUT and ELUT need
  to be reapplied after enabling edac!" (`:362`).
- **ROTB EDAC on** (`RotAccumulator.c:233-301`): TM(5,1) **0x142F** at
  start, **0x1430** at end; test mode, clear, reset, enable, then
  writes zeros through all `ROTB_MAX_BLOCKS` blocks.
- **ROTB DEU consumer** (`RotAccumulator.c:144-154,176-194`): the
  accumulator task reads the block descriptor, then checks
  `EDAC_getRotbufDEUFlag()`; on DEU it advances the pointer and skips
  the block ("skip the probably broken cell").
- **ROTB selftest** (`HWSelftest.c:481-576`): enables ROTBUF EDAC
  (`:499`), fills 128-word blocks in test mode, reads them back and
  compares — a DEU would show up as an inconsistent-data failure.
- **Boot-time EDAC print** (`main.c:120-125`) reads
  `EDAC_isEnabled` for all four modules (`EDAC.c:166-186`, live
  register reads); the ROTB EDAC task start and the CACHE `EDAC_on`
  call at boot are commented out (`main.c:205-222`). With M-1's EdacOn
  reset fix the print's CACHE line changes 0→1 — the declared
  boot-golden impact.
- Defaults: `CONFIG_ENABLED_EDAC` 0xF, `CONFIG_SCRUB_SDRAM` 1 (slow),
  `CONFIG_SCRUB_CACHE` 0 (off) (`ParamMgmtDef.h:165-167`).

### The reboot/trap path (SDRAM DE) [C]
- `main.c:80` installs `WDT_catchUnexpectedTraps` early (`WDT.c:195-221`
  catches traps 1..255 except the RTEMS-owned ones).
- `unexpected_trap_handler` (`WDT.c:128-193`): stores bad-trap info via
  `storeBadTrapInfo` (`:86-126`), which writes PSR/trap/PC/nPC as
  16-bit halves into BRAM at `UNCHANGED_STORAGE` 0xE02000A8 (`:74`) —
  "not used part of LVAR0 in BRAM" (`:68`), STIX-TN-0612 — and for
  trap 0x09 overwrites the nPC halves with the **AHBSTAT** word at
  0x80000F00 (`:116-123`, per GRLIB-AN-0004). Then it forces the
  watchdog (`:182`) — the model turns a watchdog FORCE into
  `qemu_system_reset_request` (`grlib_stixctrl.c:281-287`).
- After reset, `WDT_callAfterReset` (`WDT.c:239-276`, called from
  `Watchdog_init`, `Watchdog.c:54`) reads the BRAM record: BAD_TRAP
  magic → `WDT_RESET_TRAP` plus trapPSR/TBR/PC/PCn (`:255-263`), reads
  the SRAM selftest result from the same area (`:251-252`), then clears
  the record. `WDT_resetInfo` is where a boot report *could* carry the
  reset reason — no TM formatting of it was found in the ASW files read
  (the review sweep agrees).
- The reset source bits come from `STIXCTRL_REGISTERS->stixStatus`
  (`WDT.c:241`); the model implements the watchdog → reset path but I
  did not verify which stixStatus reset bits it sets (M-3 checks live
  [L]).

### HK / TM observables [C]
- **SID2 status1** carries the per-HK-cycle DE counters: "repurpose the
  status1 6 bits … 3 bits of rotating buffer double errors, and 3 bits
  of sram errors … reset every cycle. Ceiled at 3 bits max value"
  (`HKAcquire.c:274-286`): rotbuf count in bits 5:3, SRAM (CACHE) count
  in bits 2:0, via `EDAC_getResetDeDetected` (`FDIR/EDAC.c:283-297`,
  which also zeroes the counters). This is the AX-visible DE evidence.
- **TM(5,2) 0x542F** periodic SEU counts and **TM(5,1) 0x0030** scrub
  rate change (above).
- **TM(5,1) 0x142F/0x1430** wrap the ROTB EDAC enable
  (`RotAccumulator.c:236,299`).
- No named HK field for the SEC counters; they reach the ground only
  via 0x542F.

### The emulator today [C]
- **SDRAM controller** `grlib_stixsdram.c` is a 90-line store-and-return
  stub born from the INIT-bit repair (header comment `:1-13`); both
  registers are plain storage (`:35-49`). The SDRAM *data* is
  `machine->ram` at 0x40000000 (`leon3.c:58`, `:344-353`) — plain host
  RAM.
- **stixctrl** regions (leon3.c:447-482): APB regs (IRQ 6 already wired
  to IRQMP, `:476-477`), the 512 B ROTB block window, and four plain
  RAM AHB windows: `sram` 2 MB (CACHE: ELUT/LACC/TLUT), `lvar` 0x400
  (LVAR + BRAM), `ltrg`, `etrg` (`grlib_stixctrl.c:1051-1073`); the
  lvar/sram contents survive system reset (`:1117-1120`).
- **Register hooks** in the APB handler: 0x54 rotbCmdSta and 0x88
  hkCmdSta are composed live on read (`:807-825`); 0x54 writes are
  masked to bits 2:0 with the comment "BLOCKCOUNT, SECCOUNT, READY,
  FULL and DEDETECTED are read-only status, composed in the read
  handler" (`:983-1003`) — the DE/SEC composition anticipated but not
  yet written; `stixctrl_rotb_cmdsta` composes READY/FULL/BLOCKCOUNT
  from live state (`:307-328`). cacheCmdSta (0x50) and rotbConfig
  (0x58) are plain storage; the FSW's EDAC_reset ROTBUF strobe dance
  writes `~(1u<<2)` = 0xFFFFFFFB, which today lands as COMMAND=3
  RESETFIFO (harmless, per the comment).
- **stix_det** writes QL/EACC/LVAR data into those RAM windows from the
  host side via `memory_region_get_ram_ptr` + `set_dirty`
  (`stix_det.c:22-24`, LACC offsets `:61-75`, the QL swap writes
  `:1298-1315`) — a flipped accumulator word is overwritten at the next
  QL swap, so CACHE upsets in LACC are visible to the FSW only between
  swaps (or forever in ELUT/TLUT, which the FSW applies and the FSM
  reads).
- **Injection conventions** to follow: P4-2's `adc-inject[N]` setter
  with range check + trace + comparator sample (`grlib_stixctrl.c:1155-1201`),
  PL-2's read-only JSON `hw-state` on a fixed QOM path
  (`:1203-1240+`, D-PLo), the attenuator's child-object QOM face and the
  "faults survive device reset" persistence debate (attenuator D-Ag/D-Ah
  and its accepted-diffs note), and the platform catalogue's action
  kinds (`faults-v1.json`: `link_down`, `adc`, `timecode_inject`, … —
  new faults arrive as **qom** actions on `/machine/stixctrl*`).
- There is no existing memory-upset code (the only "SEU" hits are the
  detector ASIC test pulses, `stix_det.c:66-68`).

## Decisions (D-U)

- **D-Ua: one unit, `hw/sparc/stix_edac.[ch]`, child object
  TYPE_STIX_EDAC at `/machine/stixctrl/edac`** (created with
  `object_initialize_child` in the stixctrl instance_init, like the
  attenuator's D-Aa pattern). It owns:
  - the **poison table** (a sparse set of `(memory, address, bitmask,
    mode, flags)` records);
  - the **SEC counters and DEDETECTED latches** for the three EDAC
    memories (the registers *compose* them on read; the register owners
    keep EDACON/EDACRST/EDACRESET/SCRUBRATE as stored bits);
  - the **overlay MemoryRegions** for SDRAM and CACHE (D-Uc);
  - the **`edac-at-boot` property** (D-Uk), applied at reset;
  - links set at realize: `stixctrl` (SRAM/LVAR windows, rotb ring,
    IRQ 6 line, cacheCmdSta/rotbCmdSta state) and `sdram-ram` (the
    machine RAM MemoryRegion; leon3.c passes it where it maps the
    device, like the scet-source link, `leon3.c:506-508`).
  The stixsdram device gains ~10 hook lines (compose SECCOUNT/DEDETECTED
  on read, strobe handling) and grlib_stixctrl.c ~15 (the 0x50/0x54
  composition and one call in the rotb window handler); the hooks stay
  in their own hunks, away from the det/att blocks.
- **D-Ub: layering.** **Truth** is the host RAM bytes — the flip is
  real data corruption. **Presented** is what the EDAC logic returns:
  with EDACON=1 a SEU read returns the *corrected* word and counts, a
  DE read returns the *flipped* word and flags. There is no forcing
  layer and no `presented.*` property tree: the injection is a physical
  event, not a register forcing; the observable state is the registers
  themselves plus the read-only `upsets-active` (D-Ug). This follows the
  platform's "changes: truth" class (a physical failure), not
  "presented".
- **D-Uc: the data mechanism is a sparse page overlay, not a check on
  every access.** For SDRAM and CACHE: on the first poison in an
  aligned 4 KB page, add a priority-1 subregion over that page whose
  read/write handlers consult the poison table; remove it when the page
  empties. Reads: SEU → fetch the clean word from the backing RAM
  (`memory_region_get_ram_ptr`), return it, SEC++, and by default write
  the corrected word back and retire the poison (D-Uh); DE → return the
  flipped word, latch DEDETECTED, pulse IRQ 6 (CACHE) or raise the trap
  (SDRAM, D-Ud). Writes to a poisoned word clear that poison (a rewrite
  fixes the cell). Options considered and rejected:
  - *check on every access* (a hook in the RAM dispatch): a per-access
    cost on all 53 MB — rejected, the hot path must cost zero with no
    injection;
  - *status/IRQ only, no data flip*: the FSW's ROTB selftest and the
    pattern tests compare data; a status-only model would be caught as
    inconsistent — rejected;
  - *flip host bytes in place*: works for BRAM (no EDAC, silent — that
    is exactly what D-Uh does for lvar) but not for EDAC memories,
    where "corrected on read" must leave the flip armed until the
    correction is observed; the overlay keeps the flip and the
    correction separate.
  TCG effects: adding/removing a subregion flushes TLBs once per
  inject/clear, not per access; the overlaid page itself becomes MMIO
  (slow path) for its lifetime, which only matters for the few poisoned
  pages. For **ROTBUF** there is no direct guest-mapped data window —
  the FSW reads blocks through the 512 B window whose handler already
  exists (`grlib_stixctrl.c:431-463`), so a `stix_edac_rotb_check()`
  call in that handler (and in `stixctrl_rotb_append` for FPGA-written
  blocks) covers it; no overlay. For **BRAM** the flip is applied
  directly to the lvar host buffer + `set_dirty` (like stix_det's
  writes) — silent by construction. Two rules close the gaps the
  review found in this mechanism:
  - **Host-side writers go through the poison-clear rule.** stix_det
    materializes LACC/LVAR host-side via `memory_region_get_ram_ptr` +
    `set_dirty` (`stix_det.c:22-33`, the QL swap `:1304-1315`),
    bypassing any overlay: a poison on a LACC word would survive the
    swap and corrupt the *new* count on every read, with fresh SEC
    counts each time — unphysical (the FPGA rewrite cures the cell).
    The materialization therefore calls a
    `stix_edac_host_write(memory, off, len)` hook that retires every
    poison it overwrites (the same rule as a guest write); qtest
    `edac-cache-swap-poison` pins it.
  - **All guest access sizes are forwarded.** The FSW reads u16 LACC
    words, u8 ELUT bytes and 32-bit words elsewhere, so the overlay
    handlers accept 1/2/4-byte accesses. Sub-word write semantics on a
    poisoned word: any write that touches any byte of the word clears
    the whole poison — the EDAC word is the unit and the hardware
    read-modify-write rewrites all 32 bits.
- **D-Ud: SDRAM DEU/MBU raises the trap the FSW already handles —
  from the overlay itself.** Nothing traps today (correction 3): the
  generic unassigned-access path raises only with the MMU enabled
  (`ldst_helper.c:469`) and is not touched. The overlay's read handler
  raises the SPARC data-access exception **TT_DFAULT = 0x09** itself
  (`cpu_raise_exception_ra(env, TT_DFAULT, retaddr)` /
  `cpu_loop_exit_restore` from the overlay handler, the
  `sparc_raise_mmu_fault` pattern) — a device-initiated trap from a
  mapped address. A minimal **AHBSTAT window** at 0x80000F00 is added
  in the same commit: the FSW reads exactly one 32-bit word there, the
  failing address (`WDT.c:117-123`; the full GRLIB-AN-0004 layout
  stays [I]), so `storeBadTrapInfo` records the real failing address
  and the next boot's BRAM record is meaningful. The FSW then forces
  the watchdog itself; the model's existing watchdog-FORCE →
  `qemu_system_reset_request` path (`grlib_stixctrl.c:281-287`)
  completes the reboot. The model does not reboot on its own. The
  DEDETECTED/SECCOUNT bits of the SDRAM edac register still latch/count
  (register-visible, even though the ASW reads them nowhere — Doc vs
  code 1). If the raise hits a wall the review did not foresee, the
  fallback stays status-only SDRAM DE (DEDETECTED + SECCOUNT, no
  reboot) with the trap as A-later.
  **User decision (2026-11-12): a DEU may lead to a trap on purpose.**
  One-shot stays the default, and the trap must be reachable from a
  test without rebuilding the image. It is reached through EDACON=1
  in two ways: `edac-at-boot=all-on` at reset (D-Uk), or at run time
  through the `edac-enable` QOM write on `/machine/stixctrl/edac` (a
  mask like `edac-at-boot`, applied as if software had written the
  EdacOn bits; M-3). There is deliberately no "trap with EDACON=0" knob:
  with EDAC off the controller detects nothing, so that trap would be
  unphysical. The flipped word is returned raw (the D-Ue row).
- **D-Ue: DE/SEU status and IRQ semantics.**
  - CACHE: DEDETECTED (cacheCmdSta bit 18) latches, IRQ 6 pulses, and
    the corrupted word is returned — **only when EDACON=1 and
    cacheCmdSta OPMODE=0** (cache mode), per the register comment
    (`StixIdpu.h:1547`; Q3 checks the FSW's setSRAMModeToCache call
    site). SEU: corrected word, SECCOUNT (bits 15:8) ++.
  - ROTBUF: DEDETECTED (rotbCmdSta bit 4) latches, IRQ 6 pulses, when
    rotbConfig EDACON=1; SEU: corrected, SECCOUNT (bits 31:24) ++. The
    FSW's EDACRESET strobe (`EDAC.c:304-307`, the `~(1u<<2)` write)
    clears the latch and the counter — the existing 0x54 write filter
    (`grlib_stixctrl.c:993-996`) is extended to treat bit 2 as the
    strobe, exactly as its comment anticipates.
  - SDRAM: EDACRST strobe (bit 13 set then cleared, `EDAC.c:294-297`)
    clears DEDETECTED + SECCOUNT; EDACON (bit 12) gates everything
    (D-Uf).
  - All SEC counters saturate at 255 (8-bit fields) [I — wrapping is
    equally plausible; the change is one constant]. EDAC_init's
    strobe writes clear them at boot, as the FSW expects
    (`FDIR/EDAC.c:90-92`).
- **D-Uf: EDACON=0 means the flip is returned raw.** With the module's
  EDAC off, reads return the flipped data, no status, no counter, no
  IRQ, no trap (an uncaught corrupt read — the point of the TC that
  disables SDRAM EDAC, `HWConfig.c:78-82`). BRAM has no EDAC at all, so
  it is always in this mode. Note nothing in ASW sets EDACON
  (correction 6): the boot-time state comes from the reset values plus
  D-Uk's `edac-at-boot`.
- **D-Ug: the injection interface (the contract for the platform
  track).** All on `/machine/stixctrl/edac`:
  - `upset-inject` (write, string):
    `"<memory>,<address>,<bitmask>[,<mode>][,<persist>]"` with memory ∈
    {`sdram`,`cache`,`rotbuf`,`bram`}; address is the guest byte
    address (4-aligned) except rotbuf, which takes `"<block>:<offset>"`
    (block index into the ring, offset 0..511); bitmask is the XOR mask
    (bit count 1 = SEU, ≥2 = DEU/MBU; `mode` may override the derived
    seu|de); `persist` keeps the poison after its first effect (D-Uh).
    Rejected sets (unknown memory, unaligned address, bitmask 0,
    bitmask outside the 32-bit word, rotbuf block out of range) fail
    with an error and change nothing, like the adc-inject setter.
  - `upset-clear` (write): an index into `upsets-active`, or `"all"`.
  - `upsets-active` (read-only string, JSON): one record per armed
    poison plus per-memory counters `injected`, `corrected`,
    `detected`.
  - `random-seed` (uint32, default 0 = off) + `random-count` (uint32):
    injects `count` upsets deterministically — xorshift32 from the
    seed, uniform over the memory's extent, one bit each, skipping
    addresses already poisoned; the exact sequence is pinned by a qtest
    (the attenuator's determinism lesson, D-Ai).
  - **hw-state**: the stixctrl `hw-state` object (D-PLo) gains an `edac`
    section (same JSON as `upsets-active`) through one accessor
    `stix_edac_hw_state(QDict *)`.
  - trace: `stix_edac_inject(memory, addr, mask, mode)` /
    `stix_edac_effect(memory, addr, kind)` / `stix_edac_clear(...)`,
    emitted from the QOM setters and from actual intercepted accesses
    only — never from a getter (D-Ai's rule).
- **D-Uh: persistence and reset.** The flip lives in the host RAM bytes
  or the poison table: **upsets survive a system reset** (a reboot does
  not heal a memory cell) and vanish at power cycle (a fresh QEMU). SEU
  poisons self-retire on the first corrected read (the model writes the
  corrected word back, like real EDAC write-back) unless `persist` is
  set; **DE poisons default to one-shot** — the poison retires after
  the detected read, so an SDRAM DEU does not turn into a guaranteed
  boot loop; `persist=1` models a stuck cell (boot-loop risk documented
  in Risks). BRAM flips persist until a power cycle; nothing ever
  reports them. This differs from the Phase-4 "injections clear with
  the device" rule (D-P4b) and matches the attenuator's physical-fault
  persistence (D-Ag); the accepted-diff section records the difference.
- **D-Ui: scrubbing.** SCRUBRATE (edac 9:8, cacheCmdSta 5:4) is stored
  and readable so `EDAC_get/setScrubbingParam` and the TCs work; the
  model runs **no timed scrub pass** — the FSW's rate algorithm works
  off the SEC counters and never observes a scrub directly (correction
  5). A scrub that auto-clears armed SEUs could be added later behind a
  property if a gate ever needs it (A-later).
- **D-Uj: tests and order.** Tests live in `tests/qtest/stix.c` behind
  a `stix_edac_register()` block before `main()` (the attenuator as-built
  pattern; no new binary, no meson change). Order by FSW test value:
  1. **CACHE** — the richest observable chain (IRQ 6 → ISR → SID2
     bits, SEC → 0x542F, ELUT/TLUT/LACC corruption into science TM),
     and it works on AX **unconditionally**: cacheCmdSta EdacOn resets
     to 1 (ICD txt:2119, M-1), so no enable step is needed;
  2. **SDRAM** — the trap → watchdog → reboot → BRAM-record path, the
     highest-drama FDIR behaviour, needs the AHBSTAT + trap mechanics
     (M-3's risk) and EDACON=1 for any live gate (D-Uk);
  3. **ROTBUF** — the DEU block-skip path and the rotbCmdSta bits,
     EDACON=1 for any live gate (D-Uk);
  4. **BRAM** — silent corruption; the FSW-visible outcome is the WDT
     boot record (answered, correction 1), so the commit is not gated
     on a grep any more.
  LEON3 register-file/cache parity (trap 0x20) is out of scope.
- **D-Uk: the boot-time EDACON hand-over state (`edac-at-boot`).**
  The SuSW (not in this repo, and not run by the emulator's `-kernel`
  boot stub) is what may enable EDAC before the ASW starts; the ASW
  itself never sets EDACON (correction 6). The model therefore has one
  property on the edac unit, `edac-at-boot`:
  - `hw-reset` (default): the ICD reset values — SDRAM EDACON 0
    (txt:3249), CACHE EDACON 1 (txt:2119), ROTB EDACON 0 (txt:2202) —
    applied at every device reset (power-on and each watchdog reboot,
    like an FPGA re-loading its defaults), via a `qemu_register_reset`
    hook in the edac unit's realize so it runs after the register
    owners' resets;
  - `all-on`: all three EDACON bits set at every reset, emulating a
    SuSW that enabled EDAC before hand-over;
  - a 3-bit mask (sdram/cache/rotb) for a single memory.
  The EDAC model only ever reacts to the *live* EDACON bit (D-Uf); the
  property only decides what the bit reads at hand-over. Until Q1 is
  answered, live SDRAM/ROTB DE gates run `all-on` and say so; CACHE
  needs no property (its ICD reset value is already on).

## Fault catalogue

The IDs below are **proposed to the platform track** (D-PLq); as with
the attenuator, a rename is one string per property. Every fault is
persistent until cleared (a rewrite of the address, `upset-clear`, or —
for SEU — the first corrected read), survives device reset, and is
reversible. `changes: truth` in every row: these are physical failures.

| ID | class | mode / params (inactive) | effect | expected detection (first signal) |
|---|---|---|---|---|
| `memory.cache.seu` | physical | address, bitmask=1 bit (persist off) | corrected on read; cacheCmdSta SECCOUNT++ | TM(5,2) 0x542F after the report period (Q4); science TM corrupted if in ELUT/TLUT/LACC [L] |
| `memory.cache.ded` | physical | address, bitmask ≥2 bits (persist off) | flipped data returned; DEDETECTED; IRQ 6 | SID2 status1 bits 2:0 next HK cycle; 0x542F never (DEs are not SECs) |
| `memory.sdram.seu` | physical | address, 1 bit | corrected on read; edac SECCOUNT++ — needs EDACON=1 (SuSW hand-over or `edac-at-boot`, D-Uk) | TM(5,2) 0x542F after the period (Q4) |
| `memory.sdram.ded` | physical | address, ≥2 bits | trap 0x09 → watchdog reboot (one-shot default) — needs EDACON=1 (D-Uk); with EDACON=0 (the reset default) the flip is returned raw | UART "Unexpected trap (0x09)" then reboot; boot report 0x0022; BRAM trap record |
| `memory.rotbuf.seu` | physical | `<block>:<offset>`, 1 bit | corrected on read; rotbCmdSta SECCOUNT++ — needs EDACON=1 (D-Uk) | TM(5,2) 0x542F after the period (Q4) |
| `memory.rotbuf.ded` | physical | `<block>:<offset>`, ≥2 bits | flipped block data; DEDETECTED; IRQ 6; FSW skips the block — needs EDACON=1 (D-Uk) | SID2 status1 bits 5:3; one block of science lost [L] |
| `memory.bram.flip` | physical | address (0xE0200000+), bitmask | silent; raw flipped bytes from the next boot | **none** for the MemBram record (0xE02003C0): SuSW-only record, no ASW reader (`MemBram.h:10-11`) |
| `memory.bram.wdt` | physical | address in WDT `UNCHANGED_STORAGE` 0xE02000A8, bitmask | silent; corrupts the boot record read every boot (`Watchdog.c:54`, `WDT.c:239-263`) | garbage `sramTestResult` in the boot data or a spurious `WDT_RESET_TRAP` (`WDT.c:252,255-263`) [L]; the FSW clears the record after reading, so one boot only |

A `memory.random.burst` convenience entry (seeded N upsets across
sdram/cache) is proposed with M-6; the platform track may fold it into
a scenario step instead (D-Ug's `random-seed`/`random-count` makes the
device side enough).

## FSW-visible outcome per fault

Setup: gate ELF (AX), defaults. Everything marked [L] must be confirmed
by a live probe run before it becomes a gate expectation (the
attenuator A-4 pattern). "SID2 s1" = TM(3,25) SID2 `status1`
(`HKAcquire.c:286`).

| fault | immediate FSW reaction | later / periodic | notes |
|---|---|---|---|
| none | boot, 0x14A3 at ~15 s | — | the reference |
| `memory.cache.seu` ×1 in a LACC word | none — unconditional: cacheCmdSta EdacOn resets to 1 (M-1) | 0x542F `[0, 1, 0, 0]` (EEPROM,SDRAM,CACHE,ROTBUF) after the period (Q4); EDAC_init's reset clears it at next boot | if in ELUT/TLUT: corrupted detector formatting until the LUTs are reapplied [L]; in LACC: only until the next QL swap — whose rewrite clears the poison (D-Uc host-write rule) |
| `memory.cache.ded` ×1 | IRQ 6 → `EDAC_callback(CACHE)`; DEDETECTED latches until `EDAC_reset` (EDAC_init clears it at boot) — unconditional at boot (EdacOn reset 1; OPMODE resets 0, accepted diff) | SID2 s1 bits 2:0 = 1 on the next HK, then reset every cycle | the ISR's `EDAC_reset` strobe clears the latch within microseconds of the IRQ |
| `memory.cache.ded` burst ≥14/s for one period | 0x0030 rate change to NORMAL/MAX (`FDIR/EDAC.c:244-262`) | scrub rate visible in cacheCmdSta 5:4 | needs the SEU counters? No — the algorithm uses `EDAC_getSecCount`… which counts SEUs; a pure-DE burst does **not** move it. Rate changes need SEUs (correction: the DE path never triggers 0x0030), and TC 5/7 must set the hardware scrub rate first (see the 0x0030 note) |
| `memory.sdram.seu` ×1 | none — needs EDACON=1 (SuSW hand-over or `edac-at-boot=all-on`, D-Uk; ASW never enables it) | 0x542F `[0, 1, 0, 0]` after the period (Q4) | the FSW never reboots on SEUs |
| `memory.sdram.ded` ×1 (one-shot) | needs EDACON=1 (D-Uk); then trap 0x09: UART `Unexpected trap (0x09) … data_access_exception`, BRAM record with the AHBSTAT address, watchdog FORCE → reset | next boot: 0x0022 boot report; `WDT_RESET_TRAP` recorded in BRAM (nothing in ASW formats the record into TM — old Q1 sweep); the SDRAM DEDETECTED bit latched (read by nobody) | with `persist=1` this is a boot loop — the gate uses one-shot |
| `memory.sdram.ded` with EDACON=0 — **the reset default** (`TC(236) E_IDPU_PARAM_CONFIG_ENABLE_EDAC` = 0 can also turn it off) | **no trap** — the flipped word is returned raw | the FSW's own pattern tests can catch it (`RamUtils_memoryTest`, `HWSelftest.c:399-464`) → selftest-fail report 0x9437 | the TC disables SDRAM EDAC *off only* (`HWConfig.c:78-82`); this is the row a gate gets with no `edac-at-boot` |
| `memory.rotbuf.seu` | none — needs EDACON=1 (D-Uk) | 0x542F `[0, 0, 0, 1]` after the period (Q4) | — |
| `memory.rotbuf.ded` | needs EDACON=1 (D-Uk); then IRQ 6 → `EDAC_callback(ROTBUF)` → flag; the accumulator task skips exactly the block (`RotAccumulator.c:150-154`); DEDETECTED cleared by the ISR's reset | SID2 s1 bits 5:3 = 1 next HK | in NOMINAL only (`RotAccumulator.c:69-72`); ROTB EDAC on path wraps 0x142F/0x1430 on this build? — it is called from TC config (`HWConfig.c:106-110`), not boot (`main.c:205-216`) |
| `memory.bram.flip` in the MemBram record (`EDAC_reboot_module`, 0xE02003C3) | none — silent | **none: SuSW-only record** — `MemBram.h:10-11` is setters only; no ASW code reads it back | survives system reset; dies at power cycle |
| `memory.bram.wdt` in WDT `UNCHANGED_STORAGE` (0xE02000A8+): the `sramTestResult` halves (ptr[10]/ptr[11]) or the BAD_TRAP magic | none — silent | the next boot's `WDT_callAfterReset` (every boot, `Watchdog.c:54`) reads it: garbage `sramTestResult` (`WDT.c:252`) or a spurious `WDT_RESET_TRAP` with fake PSR/PC (`WDT.c:255-263`) [L — what the boot report then carries is the owed probe]; the FSW clears the record after reading, so exactly one boot is affected | survives system reset; dies at power cycle |

**Note on 0x0030 (scrub rate change):** it is driven by the SEC counts
only; a DEU burst cannot cause it. The algorithm also returns while the
*hardware* scrub rate is OFF (`FDIR/EDAC.c:230-234`), and ASW never sets
the hardware rate at boot (`RamUtils.c:54` `#if 0`; `HWConfig.c:115-129`
is TC-only) — so a gate that wants 0x0030 needs TC 5/7 (scrub rate)
first, then ≥14 `memory.*.seu` per second for one period (the seeded
burst mode, D-Ug) plus the report period shortened (Q4).

## Commits

Each commit is test-first: the new qtests fail against the previous
commit, then the code makes them pass. Only the qtests marked **RED**
fail *informatively* (they pin an existing behaviour that must change);
the rest are red-by-construction — they exercise a property, region or
hook that does not exist before their commit — so they are not tagged
per-test. The per-commit gate
set: the suite (`build/tests/qtest/stix`), checkpatch, the probe
self-test, boot-smoke (**UART identical**; M-1's declared boot-print
line excepted), and a manual trace-golden
diff (no `stix_edac_*` lines). The full set (soak 300 s, tc-smoke,
nominal-smoke ±, fault-smoke ±, detector-smoke ±) runs at M-6 and at
the merge.

### M-1: EDAC register semantics and reset state (no data upsets)
- `stix_edac.[ch]` skeleton: the QOM child, the counter/latch state,
  realize/reset, the `edac-at-boot` property (D-Uk); composition hooks:
  stixsdram `edac` reg (SECCOUNT 7:0,
  DEDETECTED 31 composed; EDACON/EDACRST/SCRUBRATE stored; the EDACRST
  strobe clears), stixctrl cacheCmdSta (SECCOUNT 15:8, DEDETECTED 18;
  EDACRESET strobe bit 17 clears) and `stixctrl_rotb_cmdsta`
  (SECCOUNT 31:24, DEDETECTED 4 composed; the 0x54 write filter treats
  bit 2 as the strobe per its own comment). Writes to the composed
  fields are dropped. **Reset-value fix:** cacheCmdSta takes its ICD
  reset value at device reset — EdacOn (bit 16) = **1**
  (STIX-ICD-0026 txt:2119); the model reads 0 today (plain storage,
  zeroed only at power-on). OpMode and ScrubRate keep reset 0 (the ICD
  says 1 and 3, txt:2124/txt:2122) — an accepted diff (M-7): OPMODE=0
  is what keeps the CACHE DE→IRQ path live without FSW setup (the
  `EDAC.c:69-76` HW comment says SRAM boots as main memory and ASW
  switches it to interrupt), and ScrubRate=0 keeps the scrub
  algorithm's OFF early-return (`FDIR/EDAC.c:230-234`). Traces: none
  added in this commit.
- qtests (`stix_edac_register()` block):
  - `edac-reg-reset`: edac 0x80000C04 and rotbCmdSta 0x80000554 read 0
    at reset; cacheCmdSta 0x80000550 reads 0x00010000 (EdacOn=1).
    **RED** — without the reset-value fix bit 16 reads 0.
  - `edac-reg-readonly`: **RED** — write 0xFF to the SECCOUNT/DEDETECTED
    fields of all three registers; with the change they read 0
    (composed), without it the raw stored bits come back.
  - `edac-boot-state`: the `edac-at-boot` property — the default
    `hw-reset` matches `edac-reg-reset`; `all-on` sets the
    SDRAM+CACHE+ROTB EDACON bits at every reset (the EDACON=1 the live
    SDRAM/ROTB DE gates need); a mask string covers single memories.
  - `edac-strobe`: EDACRST/EDACRESET set-then-clear leaves
    EDACON/SCRUBRATE intact and (with a forced latch via the unit's
    test-only counter property, if needed) clears DEDETECTED/SECCOUNT.
  - `edac-rotb-strobe-quirk`: the FSW's `~(1u<<2)` EDAC_reset dance on
    0x54 still lands as RESETFIFO and clears nothing it should not.
- **Gate impact:** the boot UART golden changes on exactly one line —
  the boot print reads `EDAC_isEnabled` live for all four modules
  (`main.c:122-125`, `EDAC.c:166-186`), so `E_EDAC_MODULE_CACHE = 0`
  becomes `= 1`; the trace golden changes too if it logs MMIO read
  *values* of 0x550 (Q2 decides). Regenerate the goldens; the M-7
  accepted-diffs entry records why. Everything else is byte-identical:
  the composed fields read 0, and the `FDIR/EDAC.c` report/scrub loops
  are silent with count 0 and rate OFF. The manual trace-diff expects
  the EDAC_init strobes on 0xC04/0x550/0x554 only — the EEPROM strobe
  to 0x80000900 is dropped because that APB bank is unmapped in
  leon3.c (only 0x500, 0x800-0x8FF and 0xC00 are mapped there), so
  `EDAC_isEnabled(EEPROM)` reads 0 and 0x542F's EEPROM slot stays 0
  (`EDAC.c:290-291`, `:170-171`).
- **As built (M-1):**
  - `stix_edac.[ch]` as planned: the QOM child TYPE_STIX_EDAC at
    `/machine/stixctrl/edac` (`object_initialize_child` in the
    stixctrl instance_init, realized with its parent), the SEC
    counters/DeDetected latches, the `edac-at-boot` property and the
    composition hooks in stixsdram (edac 0xC04), grlib_stixctrl
    (cacheCmdSta 0x50, the `stixctrl_rotb_cmdsta` composition and the
    0x54 write filter now treating bit 2 as the strobe). No traces
    added. Not a sysbus device: its reset is a `qemu_register_reset`
    hook (registered at realize) that clears the counters.
  - **Deviation from D-Uk's mechanism, same contract:** the EDACON
    hand-over state is applied by the *register owners'* device resets
    (stixctrl for cacheCmdSta/rotbConfig, stixsdram for the edac
    register), not by the unit's `qemu_register_reset` hook. In this
    tree a hook registered during a device's realize runs *before* the
    sysbus devices reset (the bus joins the root reset container only
    at the end of `machine_run_board_init`, `machine.c:1771`), so the
    owners would have clobbered the property's EDACON bits (visible
    with a mask that turns CACHE off). The unit's hook keeps the
    counter/latch clearing, where the order does not matter.
  - grlib_stixsdram gained its first device reset: the edac register
    takes its ICD reset (EdacOn 0, ScrubRate 0, txt:3243-3253) at every
    device reset plus the D-Uk EDACON state; configStatus is untouched
    (the INIT repair owns it). The stixsdram device reaches the unit
    through a strong `edac` link set by leon3.c (the scet-source
    pattern); with no unit linked the register keeps the composed
    semantics with zero counters (`stix_edac_*` with `e == NULL`), with
    the ICD reset.
  - The strobe qtests force the latches through three test-only unit
    properties `test-latch-{sdram,cache,rotb}` (bit 31 = DeDetected,
    7:0 = SecCount); they stay for M-2's qtests.
  - `edac-at-boot` is settable both over qom-set and as
    `-global stix-edac.edac-at-boot=...` (dynamic object properties are
    applied by the -global machinery at instance post-init, before the
    first reset) — the clean recipe for the M-6 live SDRAM/ROTB gates
    that need EDACON on from boot; pinned by `edac-boot-state`.
  - Q2 resolved by the committed trace golden: it logs MMIO read
    values and stored write values, so M-1 changes it beyond the UART
    line — the seven 0x50 lines (the boot print's read returns
    0x10000; the `RamUtils_setSRAMModeToCache` RMW carries it; the
    `EDAC_init` strobe pair reads/writes 0x10000/0x30000 → the stored
    word never shows the strobe bit) and the first 0x54 write (0x4
    traces as stored 0x0, COMMAND only). The 0xC04 accesses are
    invisible (the sdram device has no trace events). The UART golden
    line 19 is updated; the trace golden is a manual reference (baseline
    README) and is not recaptured (see accepted-diffs), the 8-line delta
    is listed there.

### M-2: CACHE (FPGA SRAM) upsets
- The overlay machinery (D-Uc) on `stixctrl->sram`, the EDACON+OPMODE
  gate, SEU correct-on-read + SEC, DE flip + DEDETECTED + IRQ 6 pulse
  (D-Ue), the poison table, `upset-inject`/`upset-clear`/
  `upsets-active` (D-Ug) — CACHE only. The overlay handlers forward all
  1/2/4-byte accesses and clear a poison on any write touching its word
  (D-Uc). stix_det's host-side LACC writes call the poison-clear hook
  (`stix_edac_host_write`), so a QL swap retires LACC poisons. Trace
  events per D-Ug. No EDACON setup is needed in any test: cacheCmdSta
  EdacOn resets to 1 (M-1).
- qtests:
  - `edac-cache-seu`: the guest (qtest `outl`/`inl`) writes a pattern to
    a LACC address, `upset-inject cache,<addr>,0x1`; read back returns
    the **original** pattern, SECCOUNT = 1; the poison retired; a second
    read counts no more.
  - `edac-cache-seu-raw`: same with cacheCmdSta EDACON=0: read back
    returns the **flipped** word, SECCOUNT stays 0.
  - `edac-cache-ded`: 2-bit flip, EDACON=1, OPMODE=0: read returns
    flipped, DEDETECTED=1, IRQ 6 observed on the IRQ line; the FSW's
    EDACRESET write sequence (set bit 17, clear it) clears DEDETECTED.
  - `edac-cache-opmode`: same flip with OPMODE=1: no DEDETECTED, no IRQ
    (the flip is still returned — the register comment gates the IRQ,
    not the data [I]).
  - `edac-cache-tlut`: flip a word in the ELUT window, read it raw
    (no EDAC-on precondition beyond EDACON), verify the flip + the SEU
    correction path.
  - `edac-cache-persist`: `persist` mode: repeated reads keep counting
    SECCOUNT (saturating at 255).
  - `edac-inject-reject`: unknown memory, unaligned address, mask 0,
    mask >32 bits → rejected, state unchanged.
  - `edac-cache-subword`: u16 LACC-style and u8 ELUT-style accesses
    across a poisoned word — reads see the flip at every width; a write
    touching any byte of the word clears the whole poison (D-Uc).
  - `edac-cache-swap-poison`: poison a LACC word, run a QL swap
    (det-source set, virtual clock across the 100 ms tick), read the
    rewritten word → clean, poison retired, a further read counts no
    SEC — pins the stix_det host-write hook (D-Uc).
- **Gate impact:** none uninjected. `fsw-accesses`-style counters are
  not needed: the FSW reads these memories on AX.
- **As built (M-2):**
  - The poison table and the overlays live in the EDAC unit and are
    generic per module (the `StixEdacMem`/`StixEdacPoison` records are
    keyed by the module enum); M-2 links only CACHE, through
    `stix_edac_cache_link()` at the stixctrl realize (the stored
    cacheCmdSta word as the EDACON/OPMODE gate source, the `sram` MR as
    the backing RAM, the sysbus IRQ slot that pulses line 6). M-3 adds
    an `stix_edac_sdram_link()` on the same structures.
  - **The flip lives in the poison table, not the backing bytes**: an
    overlay read fetches the clean word from the backing RAM and XORs
    the mask. D-Uh's "write the corrected word back" is therefore
    implicit in retiring the SEU poison — there is nothing to write
    back. A D-Uc host-write rule consequence: stix_det's materialization
    calls `stixctrl_edac_sram_write` (a thin wrapper around
    `stix_edac_host_write`; the wrapper exists because the STIXCTRL
    struct is private to grlib_stixctrl.c), which retires every poison
    the write's byte range covers. It covers the **LACC and EACC**
    banks (both sit in the sram region and are FPGA-rewritten at their
    swaps: bank zeroing at fresh enable and the materialization at the
    QL/nominal swap); LTRG/LVAR writes are in other windows and are not
    touched (lvar is M-5 BRAM anyway).
  - **Retirement rules:** a one-shot DE retires on the first *detected*
    read (EDACON=1 and OPMODE=0); with EDACON=0 or OPMODE=1 the flip is
    returned raw and the poison **stays armed** (nothing corrected the
    cell — "the detected read" is what retires, not any read). An SEU
    poison retires on the first read with EDACON=1 (corrected + SEC)
    and stays armed under EDACON=0. `persist` keeps it armed through
    everything. The overlay is added on the first poison in a 4 KB page
    and removed when the page empties; a removed overlay struct is
    parked on a freelist and reused, not freed — an SEU retire happens
    inside its own read dispatch, so freeing there would be a
    use-after-free.
  - **The OPMODE gate is the DE status/IRQ gate, not the data gate**
    (per `edac-cache-opmode` and the `StixIdpu.h:1547` comment): with
    OPMODE=1 the DE flip is still returned. The SEU path gates on
    EDACON only ([I] — the plan's qtests never read an SEU with
    OPMODE=1; one constant to change if Q3 says otherwise).
  - `upset-clear` index semantics: each record in `upsets-active` ids
    itself (`"id"` = the 0-based position in the armed list, the same
    order the JSON lists); `upset-clear` accepts that number or "all".
  - `upset-inject` in M-2 parses `sdram`/`rotbuf`/`bram` as known
    memories but refuses them as "not supported yet" (a caller is never
    left with a silently unarmed upset). A re-inject of an already
    poisoned word replaces its record (mask/mode/persist) and counts a
    fresh `injected`. Other grammar decisions: `mode` ∈ {seu, de}
    (overrides the popcount derivation), `persist` ∈ {0, 1}, numbers in
    the C `strtoull` base-0 syntax (0x hex or decimal); every rejection
    names the offending field and changes nothing.
  - `upsets-active` shape: `{"poisons": [{"id", "memory", "address",
    "mask", "mode", "persist"}...], "counters": {<memory>:
    {"injected", "corrected", "detected"}}}` — the writer's unpretty
    form (`": "`/`", "` separators), integers for address/mask. The
    counters are host-side statistics that
    survive reset (the poisons do too, D-Uh; the M-1 reset hook keeps
    clearing only the SEC/DE latches).
  - Trace kinds in `stix_edac_effect`: "sec", "ded", "raw" (flip
    returned without EDAC), "write-clear" (a guest write cures the
    cell), "host-clear" (the stix_det hook) — D-Ug's three-event set,
    with the effect kind naming the interceptor. The host-write hook
    traces although it is neither a QOM verb nor a guest access: the
    swap cures are exactly what the trace golden for a campaign needs;
    nothing traces from a getter.
  - The nine M-2 qtests are registered in `stix_edac_register()`; every
    one is red-by-construction on M-1 (no verbs, no overlay) and says
    so in its one-line comment. No FSW-visible behaviour changed
    uninjected (the overlays exist only under an armed poison; the
    stix_det hook early-returns on an empty table), so no
    accepted-diffs entry is owed from this commit.
  - Orchestrator changes after the implementation and its review:
    (a) a sub-word store to a poisoned word is a read-modify-write of
    the EDAC word: the untouched bytes keep the flip (EDACON = 0, or a
    DE: re-encoded with fresh check bits, the corruption becomes the
    cell's content), except an SEU with EDACON = 1, which the RMW's
    read half corrects (no SEC counted; the FSW never observes an RMW);
    (b) a parked overlay keeps its initialized MR and is reused (the MR
    is initialized once per struct); (c) a re-inject of an armed word
    replaces its record without counting a new armed poison, and is
    allowed with a full table; (d) the DE gate is per module
    (`stix_edac_de_gate`: CACHE adds the OPMODE half), ready for M-3.
    Known bounded window (review, for M-6 live campaigns): after an
    inject over QMP a running vCPU may still hit a stale RAM TLB entry
    for a few TBs (the flush is async), so the first read can be clean
    and the poison stays armed until the next read through the overlay.

### M-3: SDRAM upsets and the reboot path
- The overlay on machine RAM (`sdram-ram` link), the device-initiated
  trap raise (D-Ud: `cpu_raise_exception_ra(env, TT_DFAULT, retaddr)`
  from the overlay handler; the generic unassigned-access path in
  `ldst_helper.c` is not touched), the AHBSTAT window at 0x80000F00
  (the one failing-address word), SDRAM EDACON/EDACRST gating. The
  live-FSW variants need SDRAM EDACON=1: run with
  `edac-at-boot=all-on` (D-Uk); SuSW does not (Q1, user). M-3 adds
  the run-time `edac-enable` write (D-Ud user decision), so a campaign
  can arm the DE → trap path mid-run. Unlike `test-latch-*` it is not
  qtest-only: it is a platform fault verb, and it only sets the EdacOn
  bits a software write could set. qtest `edac-sdram-ded-trap-runtime`:
  boot with hw-reset, DE read → raw word, no trap; `edac-enable` sdram;
  DE read → trap 0x09.
- Optional, and separate from this track: making the HWSelftest trap
  selftest actually trap (`HWSelftest.c:277-282` stores to 0xF0000000,
  silent today — correction 3). Out of scope unless it falls out of the
  M-3 work for free; it would change the trap selftest's gate
  behaviour, so it must not ride along silently.
- qtests:
  - `edac-sdram-seu`: write/read a pattern at 0x40001000 with
    EDACON=1 → corrected + SECCOUNT=1; EDACRST clears.
  - `edac-sdram-seu-raw`: EDACON=0 → flipped word returned.
  - `edac-sdram-ded-ahbstat`: DE flip; a guest read faults; read
    AHBSTAT 0x80000F00 (from the *next* context — via the qtest
    `qtest_inl` after the reset, or a unit-level accessor [L]) shows
    the failing address. The qtest pins that the fault is a trap 0x09,
    not a silent return.
  - `edac-sdram-reboot`: **live-FSW variant** [L]: with the gate ELF,
    inject a one-shot DE into a data address the FSW touches, expect
    the UART trap line, a watchdog reset, and a fresh 0x0022 boot
    report; then confirm the model's stixStatus reset bits (the recon
    stays [L]). If the suite cannot run the FSW in-qtest, this moves into
    the M-6 `edac-smoke.sh` and the qtest keeps the unit-level part.
  - `edac-sdram-persist`: `persist=1` DE → the reboot loop reproducible
    (bounded: two boots then stop), demonstrating the stuck-cell mode.
- **Gate impact:** the AHBSTAT window is a new region — the goldens
  never touch it (the FSW reads it only inside a trap handler; the gate
  runs have no traps). Soak/boot gates unchanged.

### M-4: ROTBUF upsets
- `stix_edac_rotb_check()` in the rotb window read handler and in
  `stixctrl_rotb_append` (FPGA-side blocks), block:offset addressing,
  EDACRESET strobe clearing (extends M-1's filter). Live gates need
  rotbConfig EDACON=1 (`edac-at-boot=all-on`, D-Uk); the qtests set the
  bit directly.
- qtests:
  - `edac-rotb-seu`: fill a block via the window (test mode), SEU the
    block's word 0, read → corrected + SECCOUNT.
  - `edac-rotb-ded`: 2-bit flip → flipped data + DEDETECTED + IRQ 6;
    EDACRESET clears.
  - `edac-rotb-fpga-block`: poison a block, then `stixctrl_rotb_append`
    writes it — the FPGA write clears the poison (a rewritten cell is
    fixed), matching D-Uc's write rule.
- **Gate impact:** none uninjected (the composition is M-1).

### M-5: BRAM silent flips
- The lvar flip helper (host byte flip + `set_dirty`), addressed in the
  `upset-inject` grammar as memory `bram`. No status, no counters. The
  FSW-visible outcome is the WDT persistent area, not the MemBram
  struct: `WDT_callAfterReset` reads `UNCHANGED_STORAGE` 0xE02000A8 at
  every boot (`Watchdog.c:54`, `WDT.c:239-263`), so a flip there
  corrupts `sramTestResult` (`WDT.c:252`) or can forge the BAD_TRAP
  magic into a spurious `WDT_RESET_TRAP` (`WDT.c:255-263`), while the
  MemBram record at 0xE02003C0 has no ASW reader at all (setters only,
  `MemBram.h:10-11`) — none: SuSW-only record.
- qtests:
  - `edac-bram-wdt`: flip a byte in the WDT UNCHANGED_STORAGE area
    (e.g. the magic halves at 0xE02000A8); `inl` reads the flipped
    byte; `system_reset` keeps it flipped so the next boot's
    `WDT_callAfterReset` sees it; a fresh QEMU instance (a second
    `stix_qtest_new_extra`) reads 0 — the power-cycle contract.
  - `edac-bram-flip`: the same contract on the MemBram record
    (0xE02003C3, `EDAC_reboot_module`) — mechanism regression, no FSW
    outcome (SuSW-only record).
- The exact boot-report consequence of a corrupted WDT record stays [L]
  (nothing I found formats `WDT_resetInfo` into TM); the mechanism and
  the read-every-boot consumer are pinned by code.
- **Gate impact:** none uninjected.

### M-6: seeded random, hw-state, catalogue and gate
- `random-seed`/`random-count` (D-Ug), the `hw-state.edac` accessor,
  the platform catalogue entries (the table above, `action: {kind:
  "qom", target: "/machine/stixctrl/edac", property: "upset-inject"}`),
  probe recipes in `ground-station.md`, and an optional
  `scripts/stix/edac-smoke.sh` (the fault-smoke pattern): AX-visible
  steps only — a `memory.cache.ded` injection with SID2 status1 bits
  2:0 = 1 expected on the next HK (unconditional, EdacOn reset 1), and
  a `memory.sdram.ded` with the trap line + reboot + 0x0022, run with
  `edac-at-boot=all-on` (D-Uk). The 0x542F SEU step is included only if
  Q4 lands a shorter report period.
- qtests: `edac-random` (the pinned xorshift sequence), `edac-hw-state`
  (the JSON section, two reads equal with no virtual time — D-PLo's
  purity rule).
- **Gates: the full set**, plus edac-smoke ± if it exists.

### M-7: docs
- `accepted-diffs.md`: heading **"Accepted diff: memory upsets and
  EDAC"**, no § number (numbered at the merge), covering: the overlay
  design, the one-shot/persist semantics, the OPMODE gate, the AHBSTAT
  window, the persistence difference from D-P4b, the cacheCmdSta
  reset-value change (EdacOn 1 per ICD txt:2119, with its boot-golden
  line; OpMode/ScrubRate deliberately keep reset 0 against ICD
  txt:2124/txt:2122, per the `EDAC.c:69-76` HW comment), and the
  `edac-at-boot` property standing in for the unknown SuSW hand-over
  state (Q1).
- `ground-station.md`: the injection recipes (`-S` + qom-set + `cont`
  for an upset present from boot), the expected events per fault.
- `baseline/README.md`: the suite recipe is unchanged (same binary);
  a note that `edac-smoke.sh` exists if M-6 shipped it.
- This plan's "As built" notes per commit.

## Goldens and gate impact

- **With no injection the model is byte-invisible from M-2 on.** All
  composed fields (SECCOUNT, DEDETECTED) read 0, the overlays do not
  exist, the AHBSTAT window is unread, and no trace fires: the UART
  golden and the trace golden must be byte-identical (per-commit
  manual diff, as the attenuator does). The one declared exception is
  M-1's cacheCmdSta EdacOn reset fix — the boot print's CACHE line
  (M-1). If any other golden changes, stop —
  the FSW must be touching an EDAC register in a way M-1's readback
  composition alters (Q2).
- The existing gates are unchanged: boot-smoke, soak, tc-smoke,
  nominal-smoke, fault-smoke, detector-smoke — except M-1's declared
  one-line UART golden change (the boot EDAC print). EDAC_init's boot
  strobes and the boot-time `EDAC_isEnabled` reads keep behaving as
  today, with cacheCmdSta's reset bit now set.
- The suite keeps one binary (`stix`); the recipe in
  `baseline/README.md` step 3 is unchanged.
- **New: edac-smoke.sh** (optional, M-6), fault-smoke pattern, private
  flash via the slot library rules, AX-visible steps only; the
  report-per-fault table rows graduate from [L] to gate expectations
  only after a live probe run confirms them.

## Merge order and overlap

- **Order:** this track is independent of the flash track's *model*
  but lands on the same base commit (`083d3d4365`); rebase onto the
  integration tip (which should include the platform PL-2 hw-state
  split and, if merged before M-2, the attenuator's hooks). The
  catalogue entries in M-6 follow the platform track adopting the IDs
  (the attenuator's D-PLq precedent).
- Shared files:
  - `grlib_stixctrl.c`: ~15 hook lines (0x50/0x54 composition, the
    rotb check call, the child init, the hw-state line) in their own
    hunks, away from the det (0x00-0x4C) and att (0x5C/0x60) blocks;
  - `grlib_stixsdram.c`: ~10 lines in its own hunks;
  - `leon3.c`: one realize/link block after the stixctrl block
    (`:447-482`), touching nothing else;
  - `hw/sparc/trace-events`: a new `# stix_edac.c` block placed where
    neither the det nor the att tracks append;
  - `tests/qtest/stix.c`: one `stix_edac_register()` block before
    `main()` (conflicts with the attenuator's block are one-line);
  - `scripts/stix/platform/catalogue/faults-v1.json`: appended entries,
    after the platform adopts the IDs;
  - `accepted-diffs.md`, `ground-station.md`, `baseline/README.md`:
    appended sections.
- The flash track (branch `stix-uprev-flashdmg`, plan
  `docs/stix-port/2026-11-12-flash-damage-plan.md` in
  `~/dev/qemu-stix-flash` — not yet visible to me, so its file list is
  [L]) touches the NAND model and the filesystem; the only likely
  overlap is `tests/qtest/stix.c` and the trace-events file. This track
  does not reference flash behaviour anywhere else.

## Risks and checks

- **Raising trap 0x09 from a device read [L] — the headline risk.**
  There is **no positive control**: with the MMU off an unassigned
  access silently completes (`ldst_helper.c:469` raises only with
  MMU_E; LEON3 resets MMU-off, `cpu.c:77`), so the FSW's trap selftest
  does not trap today (`HWSelftest.c:277-282`) and cannot serve as the
  control. M-3 must prove the device-initiated raise from scratch
  (`cpu_raise_exception_ra(env, TT_DFAULT, retaddr)` from the overlay
  handler, the `sparc_raise_mmu_fault` pattern) — timebox it and fall
  back to status-only SDRAM DE (DEDETECTED + SECCOUNT, no reboot) with
  the trap as A-later. The generic unassigned-access behaviour stays
  untouched; making the HWSelftest selftest trap is a separate optional
  item (M-3).
- **The composed registers change values only when latched.** The FSW
  never writes SECCOUNT/DEDETECTED (`EDAC.c` writes strobes, EDACON and
  SCRUBRATE only), so dropping those writes cannot break readback
  [C]. The M-1 RED test pins it.
- **IRQ 6 sharing.** CACHE and ROTBUF both pulse the same line; the
  FSW's ISR checks both modules and clears the pending bit
  (`EDAC.c:62-86`), so two latched DEs are handled by one IRQ. The
  model must not lose a second pulse while the first is pending — the
  qtest pins the both-latched case.
- **Boot loop with `persist` DE in SDRAM** — realistic but deadly for
  gates; the default is one-shot (D-Uh) and the risk is documented in
  the catalogue row. `edac-smoke` never uses persist.
- **The 0x542F period (1000 s default)** makes live SEU gates long;
  gates either shorten the parameter (Q4) or stay at the DE paths.
  The qtest-level SEU checks are the primary SEU evidence.
- **Determinism.** Explicit injections are exact; the seeded mode is
  xorshift32 and qtest-pinned; no host randomness, no timers added (the
  unit has no timer, BH or DMA — the attenuator D-Ai re-entrancy rules
  apply verbatim; the overlay handlers run under the BQL and never call
  `address_space_*`).
- **The trace golden.** `stix_edac_*` matches `-trace 'stix*'`; the
  unit must not trace at realize, at reset, or in a getter (D-Ug).
- **AX visibility is good but not total:** DE counters (SID2) and the
  SDRAM trap work on AX; the SEU report needs the period parameter;
  LACC corruption is masked by the next QL swap — science-TM corruption
  checks target ELUT/TLUT [L].
- **The flash rule:** no script touches the shared flash images; the
  qtest keeps using temp flash files (`stix_qtest_new_extra`,
  `stix.c:133-153`).

## Doc vs code

Code wins in each case.

1. **`EDAC.h:34` Table.1** says `EDAC_getDeDetected` is supported for
   SDRAM, but the only caller checks CACHE/ROTBUF (`EDAC.c:62-83`) and
   `FDIR/EDAC.c:131-134` says SDRAM reports via trap. ✔ code: the model
   latches the SDRAM DEDETECTED bit anyway (register-visible, harmless),
   and the reboot comes from the trap, not the bit.
2. **`StixIdpu.h:1547`** gates the CACHE EDAC IRQ on Cache OpMode = 0;
   the FSW's `RamUtils_setSRAMModeToCache` clears OPMODE
   (`RamUtils.c:81-84`). ✔ code (D-Ue); the call site is Q3.
3. **`MemBram.c:5`** says BRAM "holds data over FPGA reboot and
   reinitialization"; the emulator already implements that contract
   (`grlib_stixctrl.c:1117-1120`). ✔ both.
4. **`WDT.c:68`** says the trap record lives in "not used 16 words of
   table LVAR0", but 0xE02000A8 is past LVAR0's 0xA0 span
   (`StixIdpu.h:302-304`) — it is in the gap behind LVAR0. ✔ code
   (0xE02000A8); the emulator's 0x400 lvar window covers it
   (`grlib_stixctrl.c:1053-1056`).
5. **The BRAM struct size.** `MemBram.c:23-59` lays out 68 bytes at
   0xE02003C0, which would end at 0xE0200404 — 4 bytes past the
   emulator's 0x400 window (the comment rounds to "64 B"). The fields
   this track targets (`EDAC_reboot_module` at +3, `lastOperationMode`
   at +6) are inside. ✔ no action; noted.
6. **`EDAC_reset(ROTBUF)` writes `~(1u << 2)`** (`EDAC.c:305-306`) —
   with plain storage that would be garbage; the model's 0x54 filter
   already handles it and M-1 keeps the behaviour
   (`grlib_stixctrl.c:983-1003`). ✔ code.
7. **Boot EDAC bring-up.** `main.c:205-222` has the ROTB EDAC task and
   the CACHE `EDAC_on` commented out; `ParamMgmtDef.h:167` defaults
   `CONFIG_ENABLED_EDAC` to 0xF — and nobody applies it to the EDACON
   bits (correction 6). The model does not depend on the FSW (it only
   reacts to register writes); the boot-time EDACON state is the reset
   values plus D-Uk's `edac-at-boot`. ✔ code.

## Review resolutions

The independent review of 2026-11-12 (`.review-mem.txt`) was verified
against the code before acting. All ten findings were accepted, none
overruled; per finding:

1. **No trap on unassigned access** (was the M-3 risk premise).
   Verified: `ldst_helper.c:469` raises only with MMU_E, `cpu.c:77`
   resets MMU-off, `HWSelftest.c:277-282` cannot trap today. Old Q4 →
   no. Corrections 3 and Risks rewritten; D-Ud now raises TT_DFAULT
   from the overlay itself, keeps the status-only fallback, and leaves
   the generic unassigned behaviour alone.
2. **Nothing in ASW sets SDRAM EDACON.** Verified: `RamUtils.c:54`
   `#if 0`, off-only TC `HWConfig.c:77-82`, reset-only `EDAC_init`,
   commented boot bring-up `main.c:205-222`. Correction 6 rewritten
   around the ICD reset values (CACHE on, txt:2119; SDRAM/ROTB off,
   txt:2202/txt:3249); D-Uk adds `edac-at-boot`; the SDRAM/ROTB outcome
   rows are marked "needs EDACON=1"; CACHE is the unconditional target;
   M-1 fixes cacheCmdSta's EdacOn reset (0 today) with its boot-golden
   impact declared; the SuSW hand-over state is new Q1.
3. **stix_det bypasses the overlay.** Verified: `stix_det.c:22-33`,
   `:1304-1315`. D-Uc gains the `stix_edac_host_write` poison-clear
   hook; M-2 gains `edac-cache-swap-poison`.
4. **BRAM: reduce, don't defer.** Verified: `MemBram.h:10-11` setters
   only; `WDT_callAfterReset` reads UNCHANGED_STORAGE at every boot
   (`Watchdog.c:54`, `WDT.c:239-263`). M-5 recast onto the WDT area;
   the MemBram record row says "none: SuSW-only record"; new
   `memory.bram.wdt` catalogue row; M-5 is no longer gated on a grep.
5. **Register map fixes.** BLOCKCOUNT 23:7 and REFRESHRATE 11:4 per
   `StixIdpu.h:1188-1189,1222-1223` and ICD txt:2213; the plan text was
   wrong, the model is right.
6. **0x0030 note extended.** The algorithm returns while the hardware
   rate is OFF (`FDIR/EDAC.c:230-234`) and ASW never sets the hardware
   rate at boot; the gate needs TC 5/7 first (outcome-table note).
7. **M-1 trace-diff expectation.** The EEPROM APB bank (0x80000900) is
   unmapped in leon3.c, so `EDAC_init`'s EEPROM strobe is dropped and
   `EDAC_isEnabled(EEPROM)` reads 0; M-1 states the expected strobes
   (0xC04/0x550/0x554) and why 0x542F's EEPROM slot stays 0.
8. **Overlay access sizes.** D-Uc: forward 1/2/4-byte accesses; a write
   touching any byte of a poisoned word clears the poison (EDAC-word
   read-modify-write semantics); `edac-cache-subword` qtest in M-2.
9. **Saturation is [I]; OPMODE reset is an accepted diff.** D-Ue tags
   saturation [I]; M-1/M-7 record the OpMode/ScrubRate reset divergence
   (ICD txt:2124/txt:2122 vs model 0, per the `EDAC.c:69-76` HW
   comment) instead of adopting the ICD values.
10. **RED discipline.** One statement in Commits: only the tagged
    qtests fail informatively; the rest are red-by-construction.

The only wording adjustment is finding 7's "EEPROM's 0x542F slot 0",
stated as: 0x542F's EEPROM slot is always 0 because
`EDAC_isEnabled(EEPROM)` reads the unmapped 0x80000900.

## Open questions for the user

> **Orchestrator answers (2026-11-12), from the code:**
> - the trace/boot golden question: the boot golden is the normalized UART log
>   and includes the EDAC configuration print (`main.c:122-125`), so M-1's
>   CacheCmdSta EdacOn reset fix changes exactly that line (declared in M-1);
> - the setSRAMModeToCache question: `MemCommon_init` (`main.c:136`) calls
>   `RamUtils_setSRAMModeToCache` at boot (`MemCommon.c:75-78`, "changes SRAM
>   EDAC uncorrectable error handling from Traps to callbacks"), before any
>   task starts;
> - the 0x542F period question: the period is the parameter
>   `EDAC_SEU_REPORT_PERIOD_S` (`ParamMgmtDef.h:195`, 1000 s, 0..0xFFFF); M-6
>   checks which TC sets parameters and uses it in edac-smoke.
> Only the SuSW hand-over (Q1) and the DE default (Q5) stay with the user.

1. **Q1: does SuSW enable the EDACON bits before handing over to the
   ASW?** SuSW is not in this repo and the emulator's `-kernel` boot
   stub does not run it. The ICD reset values put CACHE EDAC on and
   SDRAM/ROTB off at hand-over (correction 6) and the ASW never
   enables SDRAM/ROTB — so Q1 decides whether live SDRAM/ROTB gates
   should default to `edac-at-boot=hw-reset` or `all-on` (D-Uk). Until
   answered, the SDRAM/ROTB live gates run `all-on` and say so.
2. **Q2: the trace golden** — does it contain EDAC_init's strobe
   writes to 0x80000C04 / 0x80000550 / 0x80000554 (expected; the
   EEPROM strobe to 0x80000900 is dropped, that bank is unmapped), and
   does it log MMIO read *values*? If it logs values, M-1's EdacOn
   reset fix changes the 0x550 read lines too. A manual diff in M-1
   decides.
3. **Q3: is `RamUtils_setSRAMModeToCache` called before CACHE EDAC is
   enabled at boot** (the OPMODE=0 gate)? One grep.
4. **Q4: can the gate ELF's `E_PARAM_ID_EDAC_SEU_REPORT_PERIOD_S` be
   shortened by TC** (for a live 0x542F gate), or should SEU gates stay
   at qtest level?
5. **Q5: DE persistence default.** This plan defaults DE poisons to
   one-shot (a transient upset) to avoid guaranteed boot loops — the
   recommendation, reinforced by the review: the SDRAM DE path is
   EDACON-gated and thus not the everyday case; `persist=1` models a
   stuck cell. Confirm that split, or say whether SDRAM DEs should
   persist by default (physically truer, gate-hostile).

Answered by the 2026-11-12 review (kept for the record):

- **Old Q1, the BRAM readers — answered.** The WDT `UNCHANGED_STORAGE`
  record is read at every boot (`Watchdog.c:54`, `WDT.c:239-263`); the
  MemBram struct has no ASW reader (setters only, `MemBram.h:10-11`;
  `MemBram_setOperationMode` writers `OpModeMgmt.c:703,954`). The
  `MemBram_setEdacRebootModule` caller grep is still owed but cannot
  change the verdict; M-5 is no longer gated on it.
- **Old Q2 — answered: nobody.** No ASW path applies
  `CONFIG_ENABLED_EDAC` to EDACON (correction 6); the gate ELF boots
  with EDACON at the reset values.
- **Old Q4 — answered: no.** An unassigned access with the MMU off is
  silent (`ldst_helper.c:469`, `cpu.c:77`); correction 3 rewritten.
- **Old Q5 — answered: one word.** The FSW reads exactly one 32-bit
  word at 0x80000F00 (`WDT.c:117-123`); the full AN-0004 layout stays
  [I].

The brief's "find the FSW's EDAC reboot path end to end" is closed:
trap → `unexpected_trap_handler` → `storeBadTrapInfo` (BRAM record +
AHBSTAT word) → watchdog FORCE → reset → `WDT_callAfterReset` reads
the record (corrections 1-3). The only unknown left is SuSW (Q1).

## Out of scope

- **EEPROM/PROM**: the boot image is immutable (user decision); no
  upsets; `FTEEPROMCTRL_STATUS` EDACON/EDACRESET/ERRORCOUNT keep their
  plain-storage behaviour unchanged (the FSW's `EDAC_init` strobe
  writes keep landing in plain storage, as today).
- **NAND flash damage**: the flash track (`stix-uprev-flashdmg`);
  referenced only in Merge order.
- LEON3 register-file/cache parity (trap 0x20) and instruction-cache
  upsets.
- A timed scrub pass (D-Ui); migration/vmstate of the poison table
  (the gates never migrate); SuSW-only behaviour beyond the shared
  tree; the detector ASIC SEU machinery (`stix_det.c` test pulses —
  different fault class, already modelled).
- The platform controller itself (its catalogue/UI), and any PFM-ELF
  work: everything here is gate-ELF-visible.
