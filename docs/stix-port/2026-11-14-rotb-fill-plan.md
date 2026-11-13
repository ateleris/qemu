# ROTB fill attribution experiment — plan (2026-11-14)

Status: DRAFT plan, not executed. Attribution experiment: a 2x2 matrix
{CPU speed} x {flash latency} with an integration-time x flare sweep,
estimating (not proving) why the ROTB fills at the 0.1 s minimum.

## Goal

Flight shows NOMINAL at 0.5 s integration is safe, 0.3 s ok, 0.1 s
(`NOMINAL_ACCU_MIN_INTEGRATION`, `ParamMgmtDef.h:382`) is not: too many
RAW_COUNTS files. Hypothesis: file writing / fresh-position search too
slow, so the rotating buffer (ROTB) fills until it overflows (FSW
`RotAccumulator.c:66-173` drains; on overrun it skips ~60 s, FDIR-REQ 051,
`:50-64,104-142`). Deliverable: an **estimate, not flight fidelity** — the
fill curve over time, attributing the fill to CPU-too-slow vs
flash/FS-waits via a 2x2 emulator-configuration matrix.

## Non-goals

- No FSW edits: `/home/laszlo/dev/STIX-FSW` is read-only; the parameter is
  set over TM, emulator config over QEMU args / QMP.
- No flight-cause claim. Caveat wording for all output: **"emulator shows
  FSW behaviour for our input; supports a mechanism, does not prove the
  flight cause."** Flight flash wear and true NAND/search timings are not
  reproduced.
- No golden-baseline change: icount stays out of the
  `baseline/run-flags.txt` rules; matrix arms are run-scoped.
- No flash-damage/EDAC work (other plans); no FSW fix design. Deliverable:
  measurement + CSV/plots + a one-page attribution read-out.

## Recon facts (verified 2026-11-13; FSW paths under `/home/laszlo/dev/STIX-FSW/ASW/ASW/src/`)

1. **Integration-time parameter.** `SWStatus/ParamMgmtDef.h:382-384`:
   `NOMINAL_ACCU_MIN_INTEGRATION` default 9, `..._MAX_INTEGRATION` 199,
   `..._MAX_COUNTS` 1800. The FSW programs 9/199/1800 at NOMINAL entry; the
   detector latches `fsmIntTime` in 0.1 s ticks minus 1 and swaps when
   `t >= IntTimeMin + 1` (or the 1800-event cap first), so param N gives
   (N+1) x 0.1 s: 0.5 s -> N=4, 0.3 s -> 2, 0.2 s -> 1, 0.1 s -> 0
   (`accepted-diffs.md:3288-3296`).
2. **Runtime setting.** Params are set by TC(237,18) (index u16 BE + value),
   read back by TC(237,19) -> TM(237,20) carrying the value
   (`2026-11-13-aspect-plan.md:299,804`; that gate caught a wrong index).
   Indices are ELF-dependent — PFM param block (`ParamMgmtDef.h:18-65`) vs
   AX block (:67+) shifts the ordinal — so the aspect-plan's AX-build
   values 312 (0x0138) / 313 (0x0139) (index = 315 + line - 385) must be
   recounted for the ELF that runs (RF-1). `ParamMgmt_setParamValue` only
   stores; the detector is programmed from the params at NOMINAL entry
   (`SWStatus/OpModeMgmt.c:625-631`) -> send params, then (re)enter
   NOMINAL. No script sends these two params yet; builders to copy:
   `aspect-smoke.sh:337` / `elut-upload.py:191`.
3. **Flash latency model.** `hw/sparc/grlib_stixflash.c:1723` `latency`
   default **on**; `:1725-1727` tread-us 130 / tprog-us 300 / tbers-us 1500
   (datasheet-typical; accepted-diffs §42, `accepted-diffs.md:4320-4339`).
   Off = `-global grlib-stixflash.latency=off` (pfm-boot-smoke.sh:162).
   Per-op QOM counters `ops-read/prog/erase/abort`
   (`grlib_stixflash.c:1749-1756`); traces `stixflash_op_start` (with
   delay_us) / `_done` / `_abort` / `_busy_reject` (`trace-events:33-36`).
4. **Flare injection.** Runtime QOM props on `/machine/stixctrl`:
   `det-flare` (bool), `det-quiet-cps`, `det-flare-cps`
   (`hw/sparc/stix_det.c:2345-2364`), driven by `qmp qom-set`
   (detector-smoke.sh:63-66; defaults 20 quiet / 2000 flare cps per
   fpga_det). `stixdet_scene` (`trace-events:63`) logs the live cps.
5. **ROTB in the model.** A nominal swap pushes one chunk of
   `STIX_DET_CHUNK_BLOCKS` = 49 blocks (`stix_det.c:1716,1877-1886`); ring =
   104448 blocks x 512 B (`StixIdpu.h:29`,
   `2026-11-10-instrument-mock-plan.md:60`) = **~2131 chunks** full. Qdev
   prop `det-rotb` (default on; off disables chunk generation,
   `hw/sparc/grlib_stixctrl.c:1651`). Traces: `stixdet_chunk` (n_blocks,
   int_ticks, eacc_events, **rotb_count**, vclock_ns, `trace-events:66`),
   `stixdet_chunk_drop` (`:67`), `stixctrl_rotb_cmd` (rd/wr/count/full,
   `:44`), `stixctrl_rotb_overflow` (`:45`).
6. **FSW drain and overrun.** `XrayDataRTProc/RotAccumulator.c:66-173`
   `doTask` drains block by block; `:50-64` `rot_buf_overrun()` (FDIR-REQ
   051) arms a skip; `:104-142` skips until SCET > start + 60 s (`:117`);
   UART witness `TRACE_ERROR("RotBuf overrun occured")` at `:107` (spelling
   as in code).
7. **Files per second.** `ArchiveManager_addDetectorData` appends compacted
   chunks to the open RAW_COUNTS file, closes it when block-full
   (`2026-11-13-l4-replay-plan.md:196-199` -> `ArchiveManager.c:151-152`);
   the first chunk of each flush epoch is dropped (ArchiveManager.c:138-144).
   Dense 2000-cps chunks close a file every ~7 chunks (l4-replay-plan.md:339)
   -> files/s = chunk_rate / chunks-per-file, both measured. The per-file
   UART TRACE is compiled out (l4-replay-plan.md:400, amendment A) -> file
   truth from `scripts/stix/l4-flash-truth.py` on a snapshot (`:1-39`,
   emits `file <name> <scet> <chunks>`).
8. **icount.** `baseline/run-flags.txt:5` pins "icount: none";
   `capture-golden.sh:12-13` justifies it with an `assert(0)` in the CTRL
   SCET-set path — stale: that path is now the two-register commit, no
   assert (`hw/sparc/grlib_stixctrl.c:1070-1092`; it fired on every run and
   was fixed by review T1, `2026-11-09-branch-review-findings.md:453`).
   icount is untested on this branch -> RF-2. LEON = 20 MHz
   (`baseline/README.md:404-405`) -> `-icount shift=6` = one insn / 64 ns =
   ~15.6 MIPS = 0.78x LEON (shift 5 would overshoot at 31 MIPS).
9. **Run plumbing + probe.** No script accepts arbitrary extra QEMU args
   (each builds its own QEMU_ARGS, e.g. `l4-replay.sh:296-305`; slot.sh
   sets its own `STIX_SLOT_QEMU_ARGS`, `lib/slot.sh:220,26`) -> RF-3 adds a
   wrapper with an env passthrough. Template = `l4-replay.sh` phase 1
   (fresh flash, CONF@50, params, S20=0@60, NOMINAL@70, det-flare@75,
   closing CONF, one `-trace events=...,file=...`,
   `l4-replay.sh:7-15,292-305`) + nominal-smoke.sh's fresh-flash guard
   (`nominal-smoke.sh:3-6,52-56`: any NOMINAL entry writes a flash context
   — never reuse a flash.bin) + soak.sh TM-flow liveness (`soak.sh:6-14`);
   FSW uptime ran ~0.48x wall before the GPTIMER prescaler fix
   (accepted-diffs §6, 3e97493fce, on main): task periods, incl. the
   drain task, now run at 1x; flash.bin sparse (D-I,
   `capture-golden.sh:16-21`). Probe `tmtc-probe.py` (`:15-58`):
   `--send-hex/--send-at`, `--expect/--forbid`, `--qmp-cmd`, `--dump-apid`,
   `--hold`.

## Design

**2x2 attribution matrix.** Each run boots a fresh sparse flash.bin, sets
the integration params, enters NOMINAL, turns the flare on and holds:

| cell | CPU | flash latency | reads as |
|------|-----|---------------|----------|
| M1 | no icount (fast vs LEON) | off | baseline: neither cost modelled; expect no fill (if it fills, the cause is outside both axes, e.g. the drain task period) |
| M2 | no icount | on | **fill here (and not in M1) => flash/FS waits alone** |
| M3 | icount shift=6 (~LEON) | off | **fill here (and not in M1) => CPU work alone** (compaction, file search) |
| M4 | icount shift=6 | on | the realistic combination; fill only here => the interaction |

M0 (anchor) is not a separate cell: the first M2 run at ticks 4 / 2000 cps is the
anchor (files produced, extraction works). Comparing time-to-first-overrun
and peak fill across M1..M4 per (ticks, cps) attributes the fill; report
"both" when M2 and M3 each fill.

Note on "fast CPU": without icount the virtual clock follows the host
clock, so TCG speed is not literally infinite; M1 measures how much fill
remains with neither modelled cost, and that is the floor the other cells
are read against.

**Sweep.** Per cell: ticks {0, 2} (0.1/0.3 s) x cps {2000, 20000} = 4 runs;
extend to ticks {1, 4} (0.2/0.5 s) on the cells that discriminate. The chunk
rate is event-capped (`MAX_COUNTS`=1800, `ParamMgmtDef.h:384`) — at 20000
cps the cap can fire before the min time, so the effective chunk rate is
**measured** from `stixdet_chunk` int_ticks/eacc_events, never assumed.
Controls: optional M5 = M1 + `det-rotb=off` (no chunks at all; calibrates
the trace pipeline overhead).

**Extraction (existing traces only).** Events file with selected events
(l4-replay.sh:293-304 pattern, one `-trace`; exclude `stixctrl_io_*`):
fill from `stixdet_chunk` (`rotb_count` vs `vclock_ns`) and
`stixdet_chunk_drop` (`trace-events:66-67`); drain from `stixctrl_rotb_cmd`
(`:44`); overflow from the first `stixctrl_rotb_overflow` (`:45`) + first
UART "RotBuf overrun occured" -> **time-to-first-overrun** (guest s); flash
ops/s from `stixflash_op_start` (delay_us, `:33`) / `_done`, totals
cross-checked with `qom-get ops-read/prog/erase/abort`; files/s from
`l4-flash-truth.py` on the post-run snapshot (files, chunks/file) times the
measured chunk rate. Per run: `fill.csv`, `summary.json` (time-to-first-
overrun, files/s, flash ops/s, chunk rate), a fill plot; one matrix table
across runs.

## Phases

### Phase 1 — offline prep (no QEMU)

- **RF-1 Param indices + TC payloads.** Recount the TC(237,18) indices of
  `NOMINAL_ACCU_MIN/MAX_INTEGRATION` for the chosen ELF (default
  `ASW_pfm_v186.elf`: the PFM block `ParamMgmtDef.h:18-65` is compiled in,
  the AX-build values 312/313 shift by the entry-count difference); encode
  set payloads (index u16 BE + value i32 BE) + TC(237,19) read-backs.
  - Files: new `scripts/stix/rotb-fill-drive.py` (copy `tc()` from
    aspect-smoke.sh:337 or `tc_unit` from elut-upload.py:191).
  - Acceptance: a known payload (aspect budget 100 -> index 477 `01 DD`,
    `01 DD 00 00 00 00 64`) reproduced byte for byte; dry-run prints the
    schedule. Host test: round-trip the builder vs that anchor.
- **RF-2 icount shakedown.** One 90 s boot run with `-icount shift=6`
  (latency default on); one more with `latency=off`.
  - Files: none (manual invocations, documented in RF-3's wrapper).
  - Acceptance: no SIGABRT (timeout kill only), no
    `stixctrl_watchdog_reset`, NOMINAL TM `0021 04 05`, clean UART;
    if icount still wedges -> Risks fallback. Host test: grep UART +
    trace for expected/forbidden lines.

### Phase 2 — harness

- **RF-3 Wrapper `scripts/stix/rotb-fill-run.sh`.** Args: cell (latency
  on/off x icount on/off), ticks, cps, seconds (default 600), out dir.
  Fresh sparse flash per run in its own run dir (guard from
  nominal-smoke.sh:52-56 — refuse `/tmp/stix-uprev-run`/`-baseline-run`);
  QEMU args as l4-replay.sh:296-305 + cell extras (`-icount shift=6`
  and/or `-global grlib-stixflash.latency=off`) + `STIX_ROTB_QEMU_EXTRA`
  env passthrough; probe schedule = l4 phase 1 (CONF@50, params @65 with
  read-back gate, S20=0@60, NOMINAL@70, det-flare-cps qom-set @75, closing
  CONF) + soak-style liveness (soak.sh:6-14); sparse-copy snapshot after
  QEMU exits.
  - Files: new `scripts/stix/rotb-fill-run.sh` (self-contained).
  - Acceptance: `--dry-run` prints QEMU + probe lines; the M0 anchor
    (600 s) yields >= 5 RAW_COUNTS files (flash truth), TM(237,20)
    read-backs match both params, UART shows "Configuring nominal flare
    parameters" (OpModeMgmt.c:628). Host test: `bash -n` + guard check.
- **RF-4 Extraction `scripts/stix/rotb-fill-report.py`.** Trace + UART +
  flash snapshot (imports l4-flash-truth.py) -> fill.csv / summary.json /
  fill.png; `--matrix` over run dirs (parsing precedent:
  chunk-trace-check.py:1-20).
  - Acceptance: on M0, CSV rows monotone in time and `stixdet_chunk` scet
    ms match the flash-truth chunks (amendment-A cross-check,
    l4-flash-truth.py:41-45); flash-op totals match the `qom-get`
    counters. Host test: run on M0; a synthetic trace snippet yields the
    exact expected first-overflow second.

### Phase 3 — matrix + sweep runs

- **RF-5 Core matrix.** M1-M4 x ticks {0, 2} x cps {2000, 20000} = 16
  runs, sequential or on 3 gate-port slots (accepted-diffs §41,
  `accepted-diffs.md:4053`); PASS deletes the run dir, FAIL keeps it
  (pfm-boot-smoke.sh:58-59); archive trace + CSV + JSON + plot.
  - Acceptance: 16/16 complete with liveness; every summary.json has chunk
    rate, files/s, flash ops/s, time-to-first-overrun (or "no overflow").
    Host test: `--matrix` table; spot-check two cells vs raw traces.
- **RF-6 Sweep extension + controls.** Ticks {1, 4} on the two
  discriminating cells (4-8 runs); optional M5 (`det-rotb=off`) and a
  0.2 s run where the 0.1/0.3 boundary is not bracketed.
  - Acceptance: per cell, the boundary integration time (fastest still
    safe) bracketed and stated. Host test: as RF-5.
- **RF-7 (optional) CPU-time attribution.** Only if RF-5 is ambiguous:
  build the unmodified `contrib/plugins/hotblocks.c` (`make -C
  contrib/plugins`), run M4, map per-PC instruction counts to FSW symbols
  with `nm` (`MemFilesystem_*`, flash driver, archive/file search,
  `RotAccuCompactor_*`). contrib/plugins has hotblocks/uftrace but nothing
  resolving SPARC ELF symbols — the map is a post-process, not a new
  plugin; a sampling plugin only if TB granularity proves too coarse.
  - Files: new `scripts/stix/hotblocks-symbols.py` (post-process only).
  - Acceptance: top-20 symbol table for one M4 run (compaction vs
    filesystem share). Host test: maps a known address into symbol range.

### Phase 4 — read-out

- **RF-8 Results note.** One page: per-cell time-to-first-overrun and
  files/s vs integration time; attribution verdict (flash waits vs CPU
  work vs interaction) with the Non-goals caveat; CSVs + plots referenced.
  - Files: `docs/stix-port/2026-11-15-rotb-fill-results.md` (written only
    at RF-8).
  - Acceptance: every number reproducible from a summary.json by path.
    Host test: re-run `--matrix`, table matches the note.

## Run budget

- Wall time: 600 s guest per run; ~11 min per no-icount run (FSW at 1x
  since the prescaler fix), 30 min budgeted for icount runs (CPU-bound). Core =
  16 matrix runs + 2 shakedown = 18 runs ~ 6 h sequential, ~2.5 h on 3
  gate-port slots; RF-6 adds 5-9 runs (+2-3 h).
- Disk: C: has ~48 GB free but the WSL vhdx never shrinks. flash.bin sparse (D-I, capture-golden.sh:16-21) — only
  written pages cost; snapshot = sparse copy (l4-replay.sh:38-39); traces
  < ~10 MB/run (selected events); delete run dirs on PASS, archive
  trace+CSV+JSON+plot (< ~25 MB/run); check `df` per batch; never write
  into `/tmp/stix-baseline-run` / `/tmp/stix-uprev-run` (RF-3 guard).

## Risks / open questions

- **icount stability.** Untested since T1; RF-2 gates M3/M4. Fallback:
  shift=7, else drop the CPU axis (M2 vs M1 + RF-7 histogram).
- **Param index.** 312/313 are AX-build values (Recon 2); the read-back
  gate fails loudly on a wrong index; RF-1 recounts first.
- **Event-capped chunk rate.** At 20000 cps the 1800-event cap can beat
  int-time — measure the rate from `stixdet_chunk`, never infer it.
- **Saw-tooth fill.** After the first overrun the FSW skips ~60 s
  (RotAccumulator.c:104-142); mark skip windows on plots (UART "occured",
  :107); time-to-first-overrun stays well defined.
- **Off-by-one.** First chunk of each flush epoch dropped, chunks store the
  previous chunk's end (ArchiveManager.c:133,138-144;
  l4-flash-truth.py:32-36) — archived = model - 1 per epoch; RF-4 pins it.
- **Trace volume.** One `-trace` only (a second resets the file,
  l4-replay.sh:293-295); selected events, `stixctrl_io_*` excluded; thin
  `stixctrl_rotb_cmd` in the events file if it floods.
- **Host load** moves the no-icount virtual clock vs guest progress
  (M1/M2 are host-speed dependent) -> run M1/M2 on idle slots and repeat
  one cell to measure spread; icount cells are deterministic in guest
  time.
- **Open: which ELF.** Default `ASW_pfm_v186.elf`; if M0 produces no
  RAW_COUNTS files, fall back to the L4 collect ELF (STIX_L4_COLLECT_ELF,
  l4-replay.sh:7-9) and record the swap.
- **Open: pre-NOMINAL TCs.** If the release ELF's NOMINAL needs more setup
  than the l4 phase-1 schedule, pfm-boot-smoke's default (non
  `--no-nominal`) drive is the reference; review at RF-3 dry-run.
## Amendment A (2026-11-13): review resolutions, rates in blocks, archive state

**Review (GLM, 2026-11-13) verified:** the drain chain is synchronous
(`RotAccumulator.c:162` -> `RotAccuCompactor.c:266` -> `ArchiveManager.c:59`
`FSwrite` -> `FilesystemAPI.c:40-42` -> `MemFilesystem.c:1178-1205` blocks
the caller on `rtems_event_receive`), so latency=on stalls the ROTB task
(prio 100, period 1000 ms, `configFSW.h:278,347`); without icount the
virtual clock follows host time; only `stix_det.c:1326,1424` use REALTIME
(replay scan, unused here).

**Rates in blocks (user correction).** One integration = one detector
frame = 49 ROTB blocks (`RotAccumulator.h:42` LAST_DETECTOR_BLOCK_NUMBER
48). Inflow: 0.1 s -> 490 blocks/s, 0.2 s -> 245, 0.3 s -> 163, 0.5 s ->
98. Ring 104448 blocks = 213 s of 0.1 s inflow with the drain stopped. The
ROTB fills at (inflow - drain); a drain at 80 % overflows in ~18 min at
0.1 s. All reports state blocks/s in, blocks/s drained (from
`stixctrl_rotb_cmd` GETBLOCK count per second) and the deficit.

**Archive state is a primary axis (B1 resolution).** In flight the
archive partition (partition 0, `MemFilesystem.c:132-140`: all control
blocks between TMBUFFER and CONTEXT1, ~1079 CBs x 60 = ~64.7k one-block
files, ~16 GB) is full; each write that hits FSW_ERROR_FILE_OUT_OF_SPACE
runs, inside the drain task, `FSdeleteOldestFilesMultipleFileTypes(..., 32)`
(`ArchiveManager.c:61-77`, `FilesystemAPI.c:215-262`: N-oldest scan +
32 erases) and the retry. A fresh flash never takes that path. Axis:
archive {fresh, full}. How "full" is produced is an owner decision
(Open O-R1).

**Budget (datasheet timings).** One file = one flash block = 64 pages of
4 KiB (`FilesystemDefinition.h:15-19`); `write1` reads every target page
before programming it (`File.c:453-462`): 64 x 130 us + 64 x 300 us ~ 28 ms
per file; ~1.4-2 files/s at 0.1 s -> 40-56 ms/s; delete-32 burst ~50 ms +
scan per 32 files. Flash waits alone use ~5 % of the 1 s drain budget, so
the latency axis gains a **scale sweep** (tread/tprog/tbers x {1, 3, 10,
30}) on M2: the result is the break-even flash slowdown per integration
time ("the FS breaks the ROTB at 0.1 s if a program takes > X us").

**Wall vs guest clock (review M1/M2).** tmtc-probe `--send-at` is wall
time (`tmtc-probe.py:725`). Under icount the schedule must be guest-time
anchored: RF-3 sends each TC after a TM anchor (event-driven), not at a
wall offset, and `--seconds` becomes a guest-time duration read from the
`stixdet_chunk` vclock. RF-2 runs the full drive (params + read-back gate
+ NOMINAL) under icount, not only the boot.

**Minor resolutions.** CPI: shift=6 is 15.6 M insn/s; LEON3 at 20 MHz
runs ~1.3-2 CPI (10-15 MIPS) -> RF-2 adds shift=7 (7.8 MIPS) as the
bracket, and states the CPI basis. The anchor run is an M1 (fast, latency
off) run, not the first M2 run. "Deterministic in guest time" holds only
after the event-driven drive. Trace citations are `hw/sparc/trace-events`.

**Run length.** 15-20 min guest per run (a small deficit needs minutes).

**Open O-R1 (owner): how to get a full archive.**
- (a) Fill it with the FSW in the emulator (latency off, 0.1 s, flare):
  ~64.7k files at <= 2 files/s = 9+ h guest, and ~16 GB really written
  into the sparse flash.bin (the WSL vhdx grows and never shrinks; C: has
  ~48 GB). Faithful, slow, disk-heavy.
- (b) A build of the unmodified FSW with `-DLIMIT_FS_SIZE=<n>`
  (`MemFilesystem.c:46-48`, an existing define): e.g. n=20 -> partition 0
  holds ~14 CBs x 60 = ~840 files, full in ~10 min, a few hundred MB.
  Same search/delete code, smaller search space (the free-block search is
  bitmap-based, `File.c:156-204`; the N-oldest scan is O(CBs) and is
  under-costed -> report it). Needs the owner's go (an FSW build variant).
- (c) An offline writer of synthetic files into flash.bin (file header
  per `File.c:41-54` + the word-swapped BCH ECC): most work, same disk
  cost as (a).
Recommendation: (b) for the matrix, plus one (a) aging run later only if
(b) shows the delete path matters.

**O-R1 resolved (owner, 2026-11-13): option (b), and no new build is
needed.** Every AX build, including our AX gate ELF and the #1061 builds,
already uses `-DLIMIT_FS_SIZE=32` (`2026-11-11-flash-latency-plan.md:166`,
`tasks.vs.json:14,26,50`): 64 control blocks, partition 0 = ~53 CBs x 60
= ~3180 files (~800 MB), full in ~30-35 min guest at 0.1 s + flare. The
matrix therefore runs on the **AX gate ELF** (the bench configuration the
owner's flight-comparable experience comes from), not on
`ASW_pfm_v186.elf` (1090 CBs). New task **RF-2b aging run**: one M1 run
(fast, latency off) at 0.1 s + flare until the first "Filesystem full,
removing 32 oldest files" (UART, `ArchiveManager.c:63`; a TRACE, so
check it reaches the AX gate ELF's UART — DEBUG_PRINT is on) plus ~5 min;
sparse-copy the flash.bin as `aged-flash.bin` (archived, read-only); every
"full" run starts from a sparse copy of it. Recon 2 (param indices) uses
the AX block: 312/313 hold for the AX gate ELF, still gated by the
TC(237,19) read-back.
