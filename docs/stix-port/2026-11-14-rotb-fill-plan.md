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

**RF-2 early shakedown (2026-11-13, AX gate ELF ASW_em_baseline.elf,
fresh flash, 90 s wall, no probe).** `-icount shift=6` and `shift=7` both
boot to `init: SAFE mode` and `delete init` with the usual fresh-flash LUT
errors only; no abort. Guest time of `delete init`: shift 6 = 2.645 s,
shift 7 = 3.095 s, **no icount = 3.368 s** (a gate run, uprev-run-s1).
So without icount the guest is not "infinitely fast": the virtual clock
follows the host and TCG speed + host load set the work per guest second.
**Design change:** every cell runs under icount; the CPU axis becomes the
shift: **3 (~125 M insn/s, ~6-8x LEON: "fast CPU")** vs **6 / 7 (~LEON
bracket)**. M1..M4 keep their meaning with "no icount" read as "shift 3".
All runs are then deterministic in guest time once the drive is
event-driven (RF-3).

## Amendment A review (GLM, 2026-11-13) — resolutions

- **BLOCKER (RF-2b stop marker) fixed:** "Filesystem full, removing 32
  oldest files" is a TRACE in ArchiveManager.c, compiled out
  (`ArchiveManager.c:20-23`, `ENABLE_TRACE_ARCHIVE_MANAGER` 0 at
  `configFSW.h:99`; DEBUG_PRINT revives only TRACE_ERROR,
  `commonFSW.h:349-356`). RF-2b keys on the TRACE_ERROR
  `"Partition of type %u is full"` (`MemFilesystem.c:734`), printed once
  per failing write.
- **Partition 0 corrected:** bounds are inclusive
  (`MemFilesystem.c:1006`, `File.c:201`); LIMIT_FS_SIZE=32 -> 64 CBs,
  fixed partitions 14 CBs -> partition 0 = CBs 12..61 = **50 CBs = 3000
  files = 750 MiB**, full in ~25 min at 2 files/s. (Full flight FS: 1076
  CBs = 64,560 files.)
- **Verified:** one file = one block, RAW_COUNTS limit 256 KiB
  (`MemStream.h:16`, `MemStream.c:214-217`); the delete-32 path runs in
  the drain task and erases synchronously (`File.c:758-772` ->
  `MemFilesystem.c:1094-1114`); the N-oldest scan is RAM-only (fileCatalog),
  O(3000), << 1 ms.
- **CPU cost per page not in the flash knobs:** `BCH_encode` over each
  page plus a 4 KiB copy (`File.c:476-486,511-518`) — LEON CPU time that
  does not scale with tread/tprog. The break-even statement is "at icount
  shift s, with the per-page CPU cost included".

## Amendment B (2026-11-13): ROTB overflow semantics

**Owner input:** the FPGA ROTB is a ring with a write pointer running
ahead and a read pointer the FSW sees through one fixed 512 B window
(GETBLOCK = return-and-advance). When the write pointer catches the read
pointer, "some events and IRQs happen, but it is not really tested".

**The model today is drop-new:** a nominal chunk that does not fit is
dropped whole and IRQ 7 pulses once (`grlib_stixctrl.c:429-432`,
`stixdet_chunk_drop`); a TESTMODE PUTBLOCK into a full ring is dropped
(D-F, accepted-diffs). The FSW, however, is written for
**overwrite-oldest**: `RotAccuCompactor.c:81-85` resets on block 0 "in
case ROTBUF overflow occured, start with 1st block again, and forget
interrupted one", and a block-number mismatch drops the frame until the
next block 0 (`:90-97`) — the torn-frame case only an overwriting writer
produces. The real behaviour is unconfirmed.

**Change (QEMU, RF-0):** a `grlib-stixctrl` property
`rotb-overflow = drop-new | overwrite-oldest` (default drop-new: no
golden or gate changes). overwrite-oldest: a block written into a full
ring overwrites the oldest block, the read pointer advances with it
(count stays at capacity, FULL stays set), IRQ 7 pulses once per
overwritten block; partial chunks are possible (torn frames at the read
side). New trace `stixctrl_rotb_overwrite(rd, count)`. qtests: per-block
overwrite, rd advance, IRQ count, window shows the new oldest block.
**Experiment:** every cell that reaches overflow is run under both
policies; the read-out reports what the FSW does after the first overrun
under each: the FDIR-051 skip (`RotAccumulator.c:104-142`: it reads
`blocksCount` once, then GETBLOCKs until a block SCET > start + 60 s —
under overwrite the ring refills during the skip), torn-frame drops
(UART "packet sequence error", `RotAccuCompactor.c:94`), and whether the
drain recovers or re-overruns in a loop. This is the untested regime the
owner named; the emulator shows FSW behaviour for each assumed HW policy,
it does not decide which policy the FPGA implements.

**RF-0 as built (2026-11-13).** `grlib-stixctrl.rotb-overflow =
drop-new | overwrite-oldest` (default drop-new, byte-identical), read at
realize (a set after realize is rejected); accepted-diffs §49. Review
(GLM): 2 major fixed — a TESTMODE window store into a full ring now lands
at wr under overwrite-oldest (no stale block resurrected), and the
detector append path has its own qtest (`det-chunk-overwrite`: two
49-block chunks into a 60-block ring -> 38 overwrites, window = the first
chunk's block 38; `det-chunk-full` pins drop-new's whole-chunk drop);
minors fixed: the overwrite trace carries the dropped block's rd, IRQMP
latching noted (per-block count only in the trace). Suite 196/196; the
overwrite tests are red on the pre-RF-0 binary.

## Amendment C (2026-11-14): AX calibration, skip accounting, knobs

### C.1 AX reference run (owner, 2026-11-14)

The owner ran the AX (LEON3FT 20 MHz, 1 kB I/D caches) with an
optimized master build (`128f30b0`, no `DEBUG_PRINT`, watchdog off; our
copy `~/dev/stix-elf/ASW_em_prof.elf`), DSS high load (HK trigger
median ~159k per QL accumulation), MIN = MAX = 0 (0.1 s), RCR off.

| time | HK RB level | note |
|---|---|---|
| 10:09 | – | NOMINAL, GRMON attached (profile off) |
| 10:26 | 5 | |
| 10:32 | 6 | then 5/6 for ~20 min, CPU 92 % |
| 10:53 | 5 | GRMON detached |
| 10:55:40 | 7 | first overrun (no TM(5,x): the FSW emits none) |
| 10:57:45 | 6 | |
| 10:59:34 | 7 | second overrun |
| 11:00:39 | 5 | |
| 11:01:38 | 6 | |

Reading: GRMON was not the cause. Two overruns 234 s apart with a
60 s skip (~29,400 blocks at 490 b/s) in between give a net deficit of
~126 b/s, i.e. ~364 b/s processed, **~7.4 frames/s**. The 20 min hover
before suggests the DSS load is not constant; treat 7.4 fr/s as
±15 %. GRMON profiles (same build, 0.1 s): idle 28 %, compaction 20 %,
`encode_bch` 19 %, memcpy 7 %.

### C.2 Correction: the report's drain counts the skip

`drain` is the rd-pointer advance per `fill.csv` interval, and the
overrun skip (`RotAccumulator.c:116-133`) advances rd by GETBLOCK
without processing. In `rf-i8-t0-drop` (icount 8, gate ELF, 0.1 s)
the 18 skips account for ~530k of the 715k "drained" blocks: the
reported 431 b/s drain is mostly skipped data. The processed drain
before the first overrun is ~164 b/s (the 104,448-block ring filled in
320 s at 490 b/s inflow), ~3.3 fr/s. The fill traces show full-size skips
(29.0k–29.7k blocks, sometimes split over two samples), i.e. the skip
works as coded.

**RF-9 (report).** `rotb-fill-report.py` splits the drain:
- a **skip interval** is a `fill.csv` interval whose drain rate exceeds
  `SKIP_RATE_BPS` (default 5000 b/s, ~10× the max inflow); adjacent
  skip intervals coalesce into one **skip event**;
- per event: start s, blocks, equivalent seconds of data
  (blocks / mean inflow), count before and after;
- `drain.processed_*` = drain minus skip-event blocks (the bit of
  processing inside a skip interval is lost; documented in DEFS);
- `skips.refill_s`: from each event's end to the next event's start;
  `skips.first_s`: first event;
- the old `drain.*` keys keep their meaning (rd advance); `deficit`
  gets a `processed` variant.
Self-test: a synthetic fill.csv with two skips (one split over two
samples) checks the counts and the coalescing.

### C.3 Calibration matrix RF-10 (all 0.1 s fixed, RCR off, cps 20000, 1800 guest s)

| cell | ELF | icount | flash | question |
|---|---|---|---|---|
| cal-p-i7 | prof | 7 | fresh | does 128 ns/insn match the AX 7.4 fr/s? |
| cal-p-i8 | prof | 8 | fresh | bracket |
| cal-b-i7 | gate (DEBUG_PRINT) | 7 | fresh | cost of the debug build (owner: the level never fell in debug-build tests) |
| cal-p-i7-aged | prof | 7 | aged (RF-2b) | archive full: deletes + erases |

Prediction from RF-8 (1.04 M insns/frame, prof ELF, 0.1 s, cps 20000):
icount 7 -> ~7.5 fr/s, icount 8 -> ~3.8 fr/s, before flash waits.

### C.4 Knobs: what exists, what each calibrates, what is missing

Emulator (exists):

| knob | where | calibrates |
|---|---|---|
| `-icount shift=N` | run `--icount` | CPU speed (2^N ns/insn); the AX has cache-miss-heavy OS paths, so one shift fits one workload |
| `tread-us`/`tprog-us`/`tbers-us` | `grlib-stixflash`, run `--latency-scale` | chip busy times (130/300/1500 µs, datasheet family [A]) |
| `latency` | `grlib-stixflash` | flash async completion on/off |
| `rotb-overflow` | `grlib-stixctrl` | drop-new / overwrite-oldest (Amendment B) |
| start flash | run `--flash` | archive state (fresh vs aged/full) |
| `det-flare-cps` | QMP | load |

FSW references for the flash side (i4Ds/STIX-FSW issues, AX, 2019):
#700 (Release build, no BCH: 0.1 s 100 % CPU, 0.2 s 80 %, 0.3 s 60 %,
0.4 s 48 %; at 92 % CPU a 256 KiB archive file write takes 22–23 s and
write + 32 erases 45 s; BCH adds 1.2 s per 256 KiB file), #626 (FS init
0.29 s per control block), #288 (0.6 s per integration at 0.1 s, 2019,
flash-bound). These are FSW-level durations (CPU + flash), not chip
timings: they are acceptance targets for the emulator at a given
icount, not new device parameters. Chip timings stay as they are unless
a run shows the flash, not the CPU, sets the pace.

Missing in the emulator, candidates (only if a run needs them):
- a per-op extra flash delay (controller overhead) — no evidence yet;
- a DSS load schedule (cps over time) to reproduce a non-constant load
  like the AX hover — `det-flare-cps` can already be stepped by QMP.

FSW behaviour that could be parameters (proposal for the owner, no FSW
edits): the overrun skip length is hard-coded to 60 s
(`RotAccumulator.c:117`, the code cites STIX-FDIR-REQ 051, which allows
a user parameter); the drain task wakes every 1000 ms
(`RTEMSExtra/TaskManager.c:86-89`, `ROTB_TIMER_PERIOD_MS`); an overrun raises no TM(5,x) event, only the
latched HK RB level 7 (`HKAcquire.c:323`). The emulator can show what
each change would do once the owner wants it tried.

### C.5 Review of Amendment C (GLM, 2026-11-14) — resolutions

- **M1 threshold vs processing rate** (at icount 3 processing reaches
  ~5.9k b/s): the threshold is `max(SKIP_RATE_BPS, 10 × median
  processing rate of the unflagged intervals)`, and the skip-event count
  is cross-checked against the UART "RotBuf overrun occured" count (when
  the ELF prints it) and the overflow trace; a mismatch is a note.
- **M2 dilution by long intervals**: flag on
  `blocks > threshold × min(dt, 1 s)`.
- m3 split fragments below the threshold count as processing; m4
  `processed_*` is clean under drop-new only (overwrite-oldest also
  subtracts the `stixctrl_rotb_overwrite` count); m5 a skip ≥ capacity
  is invisible to rd deltas (cross-check catches it): all three go into
  DEFS.
- m6 the processed mean uses the same overlap weighting as the drain
  mean (skip blocks accumulated in the interval loop).
- m8/m10 wording and line numbers fixed above; skip events carry their
  bracketing sample times; the last event's `refill_s` is null.
- The self-test needs a capacity ≥ 10^4 (or an injectable threshold):
  the synthetic ring grows to 104448.

### C.6 RF-10 results (2026-11-14, 1800 guest s each, drop-new)

Processed drain from fill.csv (skips removed with the C.5 rule; script
in the session scratchpad, RF-9 makes it the report's):

| cell | first full | processed before / after | skips (blocks each) | refill |
|---|---|---|---|---|
| cal-p-i7 | 780 s | 7.3 / 7.4 fr/s | 4 (29.8k–30.0k) | 228–233 s |
| cal-p-i8 | 329 s | 3.5 / 3.6 fr/s | 15 (27.9k–29.7k) | 89–95 s |
| cal-b-i7 | 697 s | 6.9 / 7.0 fr/s | 5 (29.6k–30.0k) | 203–206 s |
| cal-p-i7-aged | 766 s | 7.2 / 7.3 fr/s | 5 (27.8k–30.0k) | 222–226 s |
| AX (C.1) | – | ~7.4 fr/s | – | 234 s |

- **icount 7 with the prof ELF matches the AX**: refill 228–233 s vs
  the AX's 234 s. icount 8 is 2× too slow (the RF-8 instruction model
  predicted both: 7.5 and 3.8 fr/s).
- **The debug build costs ~5 %** (6.9 vs 7.3 fr/s at icount 7). In the
  emulator that is not enough to explain a level that never falls;
  the owner's 2022 builds differ in more than `DEBUG_PRINT`.
- **The aged archive costs ~1 %** although it ran 1,122 block erases
  (vs 2 on fresh flash): at 1.5 ms chip time each the erases are not
  what limits the drain. #700's 45 s "write + 32 erases" is FSW time in
  the archive path, which the emulator executes as code; it does not
  show up in the ROTB drain at this load.
- The skip removes the full 60 s (~29.4k blocks at 490 b/s) every
  time; the FSW then refills at the same processed rate, so at 0.1 s
  the system overruns in a stable cycle (AX and emulator alike).

### Amendment C.7 (2026-11-14): fast-forward knobs

Added by the fast-forward plan (2026-11-14-fast-forward-plan.md):
`rotb-fill-run --pace wall|fast` (icount sleep=off; same FSW behaviour,
0.35 -> 0.28 wall s per guest s at icount 7 / 0.1 s / 20000 cps),
the runtime `det-rotb-burst` / `det-rotb-burst-status` on
/machine/stixctrl (N copies of the last live frame, SCETs spread
between the last live chunk and now), and the platform's
`qemu.icount_shift` / `qemu.pace` config, `det.rotb.burst` and the
flash library (`platform.flash.save|load|list`).

### Amendment C.8 (2026-11-14): count-rate sweep, debug ELF at 0.1 s

ASW_em_baseline.elf (DEBUG_PRINT), --ticks 0 --max-ticks 0 --rcr off
--icount 7 --pace fast, 1800 guest s (rf-cps-b-i7-c{2000,5000,10000};
20000 = rf-cal-b-i7-s3). Bytes/frame = stixflash cmd=3 page programs x
4096 / processed frames.

| cps (per fpga_det) | peak fill | skips | processed fr/s | B/frame |
|---|---|---|---|---|
| 2000 | 0.02 | 0 | 10.00 | 8212 |
| 5000 | 0.15 | 0 | 9.83 | 13813 |
| 10000 | 1.00 | 2 | 8.26 | 18466 |
| 20000 | 1.00 | 5 | 6.99 | 22809 |

A debug build at 0.1 s keeps up below ~5000-10000 cps and overruns
above, so whether a test overruns depends on the source rate. The two
saturated points give the emulator's cost ≈ 27 ms/frame + 5.1 µs per
compacted byte (debug ELF, icount 7); the 2022 AX fit
([[reference-ax-rotb-tests]] in the session notes) was ≈ 94 ms + 8.5
µs/B on a 2022 build with 25-49 KB frames. The icount 7 match to the
AX (234 s cycle, C.6) therefore holds for that one load point; the
fixed vs per-byte split is not confirmed (the live run's bytes/frame
are unknown). Emulator shows FSW behaviour for our input; supports a
mechanism, does not prove the flight cause.
