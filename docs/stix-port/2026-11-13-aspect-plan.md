# Aspect system emulation: photodiode ROTB blocks, archives and aspect-burst reports: plan

Status: draft 2026-11-13, revised the same day after the independent plan
review (`~/dev/stix-state/tools/glm/asp-plan-review.md`; resolutions at
the end, "Review resolutions"). Base: track worktree
`~/dev/qemu-stix-aspect` (branch `stix-uprev-asp`, at the main-line tip
`49d36da19d`). The user wants a new emulator track "aspect system"
(prefix AS-): the model generates the Aspect System photodiode data so
the real flight ASW archives it and answers aspect-burst requests, with
timing knobs to test hypotheses about three open FSW GitHub issues
(#1026 data gaps, #1031 ~99 one-sample packets then EID 0x5440, #1058
time jumps 510 samples apart at packet boundaries).

Sources:
- the model (`hw/sparc/grlib_stixctrl.c`, `hw/sparc/stix_det.[ch]`,
  `hw/sparc/StixIdpu.h`, `tests/qtest/stix.c`, `scripts/stix/*`,
  `docs/stix-port/accepted-diffs.md`);
- a read-only pass over the FSW, `~/dev/STIX-FSW/ASW` (master
  `128f30b0`). Paths below are relative to that root; note the layout:
  **ASW** sources are `ASW/src/...` (= `~/dev/STIX-FSW/ASW/ASW/src/...`),
  **BSW** sources are `BSW_ASW/src/BSW/...` (per the `ASW/Makefile`
  `SRCS`/`BSRCS` prefixes); `Aspect.c` means
  `BSW_ASW/src/BSW/Aspect/Aspect.c`;
- the FPGA documents, **as distilled in the user's brief** — tagged
  **[F]** and treated as authoritative (the source documents were
  explicitly not to be looked for);
- the detector plan (`2026-11-11-detector-plan.md`, D-* decisions and
  the D-Dg chunk machinery) and the memory-upset plan
  (`2026-11-12-memory-upset-plan.md`) for structure and conventions;
- no smoke sends a science data request today: the only service-238 TC
  in `scripts/stix/` is flash-damage-smoke's TC(238,6) filesystem check
  (`flash-damage-smoke.sh:82-84,382`) — not a data request.

Fact tags:
- **[C]** read in code (FSW or model), with file:line;
- **[F]** the user's distilled FPGA fact (authoritative; no source
  document in the repo);
- **[I]** inferred — a model choice, cheap to change;
- **[A]** an assumption the user has to accept; nothing is gated on
  an [A];
- **[L]** to be verified live before it becomes a gate expectation.

## Goal

1. The emulator generates aspect photodiode data exactly as the FPGA
   would: channels A and B (diodes A0, A1, B0, B1), a fixed 1 kHz
   sample tick, N = 1, 2, 4, 8, 16 consecutive samples summed per row
   (`asCmdSta` AVGSIZE), a 63-row double buffer that flips into **one**
   512-byte ROTB block per 63·N ms (63 ms .. 1008 ms), descriptor
   0x4000, LongTimer timestamp taken at the flip. The FSW routes the
   block into its flash archive and answers aspect-burst requests with
   TM(21,6) SSID42.
2. Timing knobs, each defaulting to ideal hardware, so one smoke can
   run the experiments: perfect (control), single-block timestamp
   jitter, a persistent timestamp step (single gap, and — the review's
   hypothesis — a step corrupting the once-per-request increment
   estimate), diode-tick/LongTimer clock-rate error, missing blocks,
   and an AvgSize change mid-run. Each knob is a QOM property settable
   at startup (`-global`) and at runtime (`qom-set`), with a hw-state
   section and trace events, following the detector scene pattern.
3. A qtest per commit, and a live `aspect-smoke.sh` that boots the gate
   ELF, drives NOMINAL long enough for archive files, sends the
   aspect-burst request, and decodes the SSID42 packets with a new
   `aspect-check.py` (the `ql-check.py` pattern: self-test, then
   per-experiment signatures).
4. **The caveat that governs every conclusion drawn here: the emulator
   shows what the real FSW does with OUR modelled aspect input. A
   reproduced flight signature supports a mechanism; it does not prove
   the flight cause.** The GitHub-issue legs record what the FSW does
   with each knob; matching the issue's symptoms is evidence, not
   proof.
5. Known probe connect race: TM sent before the probe connects is
   dropped, and the planned `tm-hold` fix is a separate track. The
   smoke therefore must not depend on boot-time TM: its sync points are
   the responses to its own TCs (TM(1,1)/TM(1,7) acks, TM(5,1)
   0021 04 05) and the probe's monotonic `--send-at` stamps, never
   0x0022/0x14A3 timing.

## Corrections to the brief

1. **There are two request paths, not one.** The brief asked to find
   how a ground request for aspect-burst data is made. Both are found:
   - **TC(238,1)** "select data for download" with **processing level
     5 = aspect** (queued by `UserSelector.c:21-62`), then **TC(238,2)**
     which reports the list as TM(238,3) (`UserSelectionList.c:168-196`);
     the request is actually processed by the Background task every
     **30 s** (`background.c:63` period 300 × 100 ms, `:174-178`,
     `SciReport.c:345-393`). `SciReport_processBox` routes level 5 to
     `AspectCtrl_report(from, to, averaging, request_number)` with
     `averaging` = the box's `u16_time_binwidth`
     (`SciReport.c:586-603`). Failures here are the **EID 0x5440**
     TM(5,2) the issues quote ("error during user request box
     processing", `TMData.h:304`, sent at `SciReport.c:467-474`), plus
     EID 0x543F "no BSD data found" for an empty range
     (`SciReport.c:535-544`) and 0x5446 for a decode error
     (`SciReport.c:377`).
   - **TC(238,10)**, the dedicated aspect burst TC
     (`TcService238.c:190-202` → `AspectCtrl_tcReportBurstData`,
     `AspectCtrl.c:495-510`; payload per `TCData.h:433-435`: u32 start
     sec, u32 end sec, u16 averaging) → Background event →
     `AspectCtrl_doTask` (`:519-534`); its failure report is **not**
     0x5440 but **TM(1,8) FID_48435** "no selected data (scientific +
     aspect) found" (`TMData.h:214`, sent at `AspectCtrl.c:531`), and
     a second concurrent request is rejected with FID_48463
     (`TcService238.c:192-197`, `TMData.h:242`). The user_request_number
     is always 0 on this path.
   The issues' EID 0x5440 tells us the failing flight requests went
   through the TC(238,1)+TC(238,2) path; the smoke exercises both.
2. **The SSMM transfer mask needs nothing**: the default
   `ENABLED_SSID_TRANSFFER_TO_SSMM_MASK` 0x1DF1F1F
   (`ParamMgmtDef.h:388`) already sets SSID42's bit
   (SCIENCE_TRANSFER_MASK_SSID42 0x00800000, `commonFSW.h:225`); the
   SSID42 check is `TMBuffer.c:285-311` (the SSID42 line is `:309`).
   No TC(21,1) is needed.
3. **The brief's "Aspect HK on-demand channels exist" is narrower than
   it looks**: what AspectCtrl actually consumes at runtime is
   `asQlData[0..3]` via a 4 s timer (`AspectCtrl.c:42-55`,
   `Aspect.c:127-133`) summed into the HK aspect fields
   (`AspectCtrl_getAndResetHK`, `:462-477`). The photodiode
   voltage/temperature HK channels stay out of scope as the brief says.
4. **The gate ELF prints and asserts for real.** The gate ELF is a
   DEBUG_PRINT build (`commonFSW.h:336-378`): `TRACE_ERROR` and
   `ASSERT` are **live** (only `TRACE` is #undef'd per module,
   `AspectCtrl.c:24-27`), and `ASSERT` failure hangs the emulator
   (`debug_assert` → `1/assert_breakIntoDebugger` loop,
   `commonFSW.h:358-365`). Consequences, both handled below:
   - "Skipped first Aspect data chunk" (`ArchiveManager.c:219`, a
     TRACE_ERROR) prints once per archive epoch as soon as aspect
     blocks flow — `detector-smoke.sh`'s `uart-chunk` check ("Skipped
     first Det data chunk" exactly once, **no `*E` line from the
     archive**, `detector-smoke.sh:93-100`) breaks at AS-2 and gets a
     planned whitelist (AS-2).
   - `AspectCtrl_report`'s ASSERTs (`AspectCtrl.c:174, 180, 219,
     229-230, 370, 437`) must stay unreachable: every knob bound in
     D-ASd is chosen so the increment estimate stays in 1..16 and the
     record deltas stay positive (the bound derivations are in D-ASd).
   The AX-only "NOT ZERO" check inside the sample loop
   (`AspectCtrl.c:354-362`) **is** compiled out — it is double-gated by
   `#ifdef ENABLE_TRACE_ASPECT` (`configFSW.h:101` sets 0) — so our
   non-zero pattern draws no UART complaint.

## Key facts the plan rests on

### The aspect hardware (all [F], from the user's distilled brief)
- Two ADC channels A and B, two diodes each (A0, A1, B0, B1). Channel
  enable = `asCmdSta` bit 0 (A) / bit 1 (B); AVGSIZE = bits 10..8:
  0..4 → N = 1, 2, 4, 8, 16 summed ADC samples. Each diode is sampled
  every 1 ms (a fixed 1 kHz tick); N consecutive 12-bit samples are
  summed into one 16-bit value (non-overlapping windows), so one aspect
  row covers exactly N ms.
- After power-on of a channel: up to 1 ms to turn on, readout starts
  after another 1 ms. A channel that is off writes 0 for its two
  diodes.
- Double buffer of 63 rows × 4 words (A0, A1, B0, B1 per row). When 63
  rows are full the pages flip and the full page is copied to the
  rotating buffer as **one 512-byte block** (an aspect chunk is always
  1 block; it is never interleaved inside a 49-block detector chunk).
  One block every 63·N ms.
- Block layout, big-endian: descriptor u16 (bits 15..14 = 01 aspect,
  bits 13..7 = 0, bits 6..0 block number = 0), timestamp 6 bytes =
  32-bit seconds + 16-bit subseconds of the LongTimer, then 504 data
  bytes: row r at offset 8·r holds A0, A1, B0, B1 (u16 each); row r
  covers time t0 + r·N ms. The timestamp is sampled at the END of the
  integration of the block (the flip).
- `AsQlData[0..3]` (0x6C..0x7B) hold the very last accumulation per
  diode (16-bit, upper bits 0).
- Aspect HK on-demand channels exist in the HK scan; out of scope
  unless trivial.

### The FSW side [C]
- **Registers.** `asCmdSta` 0x68, `asQlData[4]` 0x6C-0x7B
  (`StixIdpu.h:1643-1644`). `Aspect_setPower` clears/sets bits 0/1
  (`Aspect.c:80-96`), `Aspect_setSampleCountPerSum` validates 0..4 and
  writes AVGSIZE (`:108-118`), `Aspect_getQLData` reads the four
  16-bit fields (`:127-133`). The model's own bit macros already exist
  (`StixIdpu.h:667-693`: ASAON 0, ASBON 1, AVGSIZE 10:8 with values
  AVG1..AVG16 0..4; ASQLDATA_DATA 15:0).
- **Power-on and configure.** Boot/init:
  `aspConfig(E_ASP_PARAM_CONFIG_SUMMED_VALUES, ASPECT_SUMMED_VALUES)`
  then `HWConfig_aspectPowerOn(A|B)` (`OpModeMgmt.c:1066-1068`).
  SAFE entry: aspect power on (`:229-230`). CONF entry: power from
  `E_PARAM_ID_ASPECT_HALVES_POWER` via `Aspect_setPower`
  (`:381-384`). NOMINAL entry: power on again (`:680`) and
  `AspectCtrl_configure()` re-applies the AVGSIZE param to the
  register (`OpModeMgmt.c:641`, `AspectCtrl.c:536-548`). MAINTENANCE
  powers the channels off (`:239`). `aspConfig` writes the register
  and stores the param (`HWConfig.c:190-207`).
- **Params.** `ASPECT_SUMMED_VALUES` default 4 (=
  1<<4 = 16 samples summed; `ParamMgmtDef.h:210`),
  `NUMBER_ASPECT_GAPS_ALLOWED` default 1 (`:547`), SSMM mask default
  0x1DF1F1F (`:388`).
- **ROTB routing.** `RotAccumulator_doTask` runs only in NOMINAL
  (`RotAccumulator.c:69-72`), reads blocks through the FIFO window and
  dispatches on the descriptor type (`:146-166`):
  `ROTBUF_DESCRIPTOR_ASPECT_DATA` = 0x4000 →
  `AspectCtrl_processRotB` (`RotAccumulator.h:16-22`,
  `AspectCtrl.c:481-484` → `ArchiveManager_addAspectData`). Block
  constants `RotAccumulator.h:24-31`: 504 B data, 6 B time, 63 samples
  per block (`ROTBUF_ASPECT_SAMPLES_PER_BLOCK 0x3F`), data offset 8.
- **Archive.** `ArchiveManager_addAspectData`
  (`ArchiveManager.c:201-267`): the **first chunk after a flush is
  skipped** ("Skipped first Aspect data chunk", `:217-223`) because the
  row period is unknown; the block timestamp is the integration end
  with the de-sync bit masked (`:214`); each stored record rewrites the
  block start time to the **previous archived block's** raw end
  (`:243-245`), appends 6 B time + 504 B data (`:248`), and advances
  the chain to the current block's raw end (`:265-266`). **This
  time-rewrite chain is the spine of every timing experiment**: a block
  that never reaches the archive (dropped) leaves no time gap at all —
  consecutive archived records still differ by 63·N ms — and a one-block
  timestamp error shows up **twice** (entry and exit of the affected
  record). When the stream cannot take another 510 B the file is
  flushed (`:231-232`, `flushAspStream :92-104`) as
  `FScreateFilename(E_MEMFILESYSTEM_FILETYPE_ASPECT = 1, start SCET,
  duration s)` — filename bytes `[0]=type, [2..5]=SCET coarse,
  [6..7]=SCET fine, [8..9]=duration`
  (`FilesystemDefinition.h:64-73, 83`). One file = one flash block,
  256 KiB with a 4 B size header (`ArchiveManager.h:11`,
  `FilesystemDefinition.h:19`), i.e. ~514 aspect records per file.
  **Files are closed at exactly two mode changes**: SAFE entry
  ("save last detector and aspect data", `OpModeMgmt.c:165-166`) and
  NOMINAL→CONF (`:292-294`) — **not** at CONF→NOMINAL. So the smoke's
  SAFE-era, CONF-era and NOMINAL-era records land in **one** file,
  written at the NOMINAL exit; the file's first two records are the
  first blocks after the SAFE-entry flush (~boot), whatever the
  request window is.
- **The burst report** (`AspectCtrl_report`, `AspectCtrl.c:71-459`):
  finds aspect files by SCET range (`FSgetFilenamesOfFilesInRange`,
  `:142`; filenames carry start+duration, `:149-160`); the file payload
  is self-describing (4 B size header, then 6 B time + 504 B data
  records, `:173-180`); the **per-sample increment is estimated once
  per request** from the first two block timestamps of the first file
  that has two blocks in it (`:197-237`; asserted 1..16 ms
  `:229-230`; kept across file boundaries, `:196`), the gap check
  compares each block's raw timestamp against the previous block's
  estimated end (timestamp + 63·increment) and fires on a difference
  **> 5 ms** (`:279-291`); with gaps budget left it forces a packet
  send and continues, decrementing the budget (`:317-342`); with none
  left it forces a final packet and **returns FSW_ERROR**
  (`:297-315`); a full packet flushes at
  `TMData_SSID42_MAX_DYNAMIC_DATA_SIZE` = 4096 − 16 = 4080 B = **510
  samples** (`:396-417`, `TMData.h:593-596`, `TMDATA_SOURCE_DATA_SIZE
  4096 :548`); after a full-packet send the packet start time is the
  **accumulated estimate** `Timing_MsToSCET(aspectCurrentTimeMs)`
  (`:409-411`), while after a gap-forced send it is the freshly-read
  **raw record timestamp** of the block after the gap (`:332-334`).
  The sample-averaging state (`u32_average_counter`,
  `averagedAspectDataBuffer`) is **not** reset by a forced send
  (`:317-342` resets only the TM counters) — with TC averaging 64 and
  N=16 one completed output sample spans ~1.01 blocks, so a forced
  packet carries ~1 sample. A SAFE-mode entry aborts the report with
  FSW_ERROR_ABORT → EID 0x5445, any other error → EID 0x5440
  (`SciReport.c:461-477`).
- **TM(21,6) SSID42 wire format** (`TM_21.c:647-687`): APID 0x5DC
  (PID_93 auxiliary science, category 12, `TMData.h:66,83`); after the
  SSID byte: SCET coarse u32, SCET fine u16, user_request_number u32,
  averagingTc u16, summingFpga u8, numberOfDynamicStructures u16, then
  8 B per sample = A0, A1, B0, B1 u16 BE
  (`TMData.h:906-931`). Segmentation flags FIRST/CONT/LAST/STANDALONE
  per packet; no CRC.
- **Timing facts for the knobs.** `Timing_ToMs(coarse, fine)`
  **truncates** each timestamp to whole ms: `coarse·1000 +
  (fine·1000)/0x10000` (`Timing.c:395-399`), fine in 1/65536 s units;
  the FSW's `(delta + 5) / 10` arithmetic (`AspectCtrl.c:215-225`) is
  a no-op on already-whole-ms inputs. The emulator's block period is
  therefore visible to the FSW only through the truncated pair delta
  (⌊P⌋ or ⌊P⌋+1, phase-dependent) and the integer estimate
  `inc = floor(delta_ms / 63)` — the per-block residual is
  `|63·inc − P|` ms with `P = 63·N·(1+eps)`.
- **AvgSize change mid-run.** TC(236,6) with system 3
  (`E_SYSTEM_ASPECT`, `HWConfig.h:13`), parameter 1
  (`E_ASP_PARAM_CONFIG_SUMMED_VALUES`, `HWConfig.h:48`) writes the
  register at once (`HWConfig.c:1005-1008`); nothing flushes the
  archive, so one file then contains mixed row periods. The FSW's
  comment says a changed sum config "will have a new file" only
  because NOMINAL re-entry flushes (`AspectCtrl.c:192`).
- **Parameter TCs and indices.** ASW parameters are set by **TC(237,18)**
  (load parameter): u16 param ID, u8 sub-ID (0 = set value), u32 value
  (`TCData.h:103-106`, `TcService237.c:66-94`), and read back by
  **TC(237,19)** → **TM(237,20)** carrying (paramId u16, subId u8,
  value u32) on APID 0x5A8 (`TcService237.c:97-144`,
  `TM_237.c:30-48` with the PID_90/CAT_DIAGNOSTICS header builder
  `:51-79`). The `E_PARAM_ID_*` enum is generated 1:1 from the
  `ParamMgmtDef.h` list order (`ParamMgmt.h:25-34`), **skipping the
  `#ifdef`'d-out HW blocks**: the PFM block (`:18-65`) is compiled out
  in the gate ELF (AX build), the AX block (`:67-113`, 46 entries) is
  in. Counting the AX list from the known anchor NOMINAL_QL_PERIOD =
  315 (`ParamMgmtDef.h:385`, per the detector plan; the anchor
  cross-check also gives ASPECT_SUMMED_VALUES = 140 at `:210`):
  index(line) = 315 + (line − 385), so **NUMBER_ASPECT_GAPS_ALLOWED
  (:547) = 477 = 0x01DD** — index 476 (`:546`) is
  ATT_AUTONOMY_MOTOR_USE and a TC with it is rejected by the range
  check [I — line-count, anchor-consistent; the smoke reads the
  parameter back so a wrong index fails loudly]. So
  `NUMBER_ASPECT_GAPS_ALLOWED = 100` is TC(237,18) app data
  `01 DD 00 00 00 00 64`, immediately followed by the read-back
  TC(237,19) `01 DD 00` → TM(237,20) `01 DD 00 00 00 00 64` — the
  flight raised this budget by TC (GitHub #1026 comments), which the
  #1031 legs must reproduce.
- **FSW constants that disagree with the hardware fact** [C]:
  `AspectCtrl.h:12` defines `ASPECT_SAMPLE_RATE 1024 // This is the
  hardware default and cannot be changed.` while the [F] hardware fact
  (and every D-ASh arithmetic below) is a 1 ms tick (1 kHz). The model
  keeps **1 ms** ([F] wins; the comment is unused by the code — the
  FSW never divides by it), recorded here and in accepted-diffs §46.
- **TM(238,3)** (the user-request list report the smoke waits for) is
  APID **0x5A8** (PID_90, CAT_DIAGNOSTICS; `TM_238.c:116` with the
  header builder `:151-162`).

### The emulator today [C]
- `asCmdSta` (0x68) and `asQlData` (0x6C-0x7B) are **plain storage**:
  the APB write dispatch falls through to the stored word
  (`grlib_stixctrl.c:1137`), reads return it (`:943-948`); no model,
  no timer, no reset. `accepted-diffs.md` §38 records "No aspect ROTB
  blocks" (plan D-Dg; §35 builds only 0x8000 chunks).
- The ROTB ring and the append path to reuse: ring fields
  `grlib_stixctrl.c:87-96`, `stixctrl_rotb_push` `:375-381`,
  `stixctrl_rotb_append` `:391-428` (only with rotbConfig ENABLE=1 ∧
  TESTMODE=0; whole-append-or-drop with one IRQ 7 pulse on full),
  `stixctrl_rotb_cmdsta` composed on read `:349-367`. Ring size
  default 104448 blocks (`StixIdpu.h:187`, `grlib_stixctrl.c:1573`) ≈
  2131 detector chunks — an aspect block per 63..1008 ms cannot fill it.
- The patterns to copy from the detector model (`stix_det.c`):
  a `QEMU_CLOCK_VIRTUAL` timer with absolute deadlines
  (`:146-148, :256-258`, `timer_new_ns :2570`, `STIX_DET_TICK_NS
  100 ms :49`); the chunk build writes descriptor + SCET big-endian
  with `stw_be_p`/`stl_be_p` (`stix_det_chunk_build :1822-1860`, the
  SCET from `stixctrl_scet_reg` `:1878`); host writes via
  `memory_region_get_ram_ptr` + `set_dirty`; the re-entrancy rules of
  D-Dj (timer callbacks hold the BQL, no MMIO dispatch from timer
  context — detector chunks and aspect blocks can never interleave
  mid-chunk).
- **The LongTimer** is composed live on read from
  `qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) − time_correction_ns`
  (`grlib_stixctrl.c:888-896`), subseconds in 1/65536 s
  (`STIX_SUBSEC_PER_SEC 65536`, `:199`); a TC(9,129) SCET commit
  shifts `time_correction_ns` (`:1042-1062`), so a mid-run time jump
  moves all future block timestamps — same as the detector's chunk
  timestamps, and same as the FSW expects (it masks the UNSYNCED bit,
  `ArchiveManager.c:214`).
- hw-state getter `stixctrl_get_hw_state` (`grlib_stixctrl.c:1447+`,
  D-PLo) builds the JSON string; the aspect section is one new
  subsection. Runtime QOM property setters with validation + trace are
  the detector scene pattern (`stix_det.c:1917+`).
- Tests: one qtest binary (`tests/qtest/stix.c`); detector tests are
  registered inline in `main()`, newer tracks add a `stix_*_register()`
  block called from `main()` (`stix_edac_register` `:14454`, called at
  `:14627`). The aspect tests get `stix_aspect_register()`.
- `tmtc-probe.py` already has `--dump-apid APID:FILE` (whole CCSDS
  packets, SSID at byte 16) and `--send-at/--send-hex/--qmp`;
  `scripts/stix/lib/slot.sh` and `lib/paths.sh` (`STIX_DEFAULT_ELF`)
  are the smoke infrastructure, as used by `detector-smoke.sh`.
- Wall-clock rule: no gate timing compares across clocks; the smoke
  uses the probe's CLOCK_MONOTONIC stamps (WSL steps the wall clock;
  the detector plan's D-3 lesson).

## Decisions (D-AS)

- **D-ASa: one unit, `hw/sparc/stix_aspect.[ch]`, `StixAspectState`
  embedded in `STIXCTRL`** (the D-Da pattern). It owns the tick timer,
  the row/buffer state, the knobs and the traces; `grlib_stixctrl.c`
  delegates 0x68-0x7B and calls its realize/reset. Not a sysbus
  device. Init-time property `as-model` (bool, default on): off keeps
  today's plain storage for 0x68-0x7B, forever, for A/B comparison and
  rollback.
- **D-ASb: sampling model.** A 1 ms `QEMU_CLOCK_VIRTUAL` timer with an
  absolute deadline (the stix_det tick pattern), running whenever
  `as-model` is on. Each tick every **enabled** channel draws one
  12-bit sample from the pattern; N consecutive samples are summed
  (mod 65536) into the row. AVGSIZE is **latched at row start** (a
  mid-row TC change applies from the next row — the same latch rule as
  QlIntTime, D-Dd). After every 63rd row the pages flip:
  - the full page becomes **one** 512 B block,
  - the timestamp = `stixctrl_scet_reg` (the LongTimer) sampled at the
    flip, descriptor = 0x4000 | 0,
  - the block is appended with `stixctrl_rotb_append(s, blk, 1, …)` —
    so it obeys the same ENABLE ∧ ¬TESTMODE gating, the drop-new rule
    and the IRQ 7 pulse as the detector chunks, and it lands **between**
    detector chunks, never inside one (BQL-serialized timer callbacks,
    D-Dj).
  A channel that is off contributes 0 for its two diodes; the block is
  still produced ([F] "a channel that is off writes 0", which implies
  the block exists). The [F] power-on delay (≤1 ms + 1 ms) is **not**
  modelled — it is invisible at 63 ms block granularity; recorded as
  an accepted diff.
- **D-ASc: the data pattern.** `as-pattern` (str, runtime): `zero` |
  `ramp` (default `ramp`), plus `as-sun` (uint16, default 0) added to
  every diode ("sun" offset, clamped into 12 bits before the sum).
  `ramp` makes every 12-bit sample value
  `base[d] + ((block_seq & 0x3F) << 6 | row) & 0xFFF` with per-diode
  bases 0x000, 0x100, 0x200, 0x300 — so a decoder can verify sample
  order, diode assignment and loss from two consecutive rows alone,
  and the row value is `N × (sample + sun) & 0xFFFF`, which makes an
  AvgSize change visible in the TM values (D-ASh row 6). The FSW
  divides by the TC `averaging`; the smoke's averaging defaults to N
  and is overridden to 64 on the #1031-shaped legs (rows 4/7).
- **D-ASd: the timing knobs (the experiments), all runtime properties
  on `/machine/stixctrl`, validated + traced, applied at the next
  tick/flip. Bounds first, because the gate ELF's ASSERTs are live
  (Correction 4). The FSW's estimate-pair delta is the difference of
  two **whole-ms-truncated** timestamps (`Timing_ToMs`,
  `Timing.c:395-399`), so for a nominal pair period P the delta can be
  ⌊P⌋ or ⌊P⌋+1 (phase-dependent, ±1 ms on top of the fine
  quantization), and `inc = floor(delta / 63)` must stay in 1..16 or
  `AspectCtrl_report`'s ASSERTs hang (`:229-230`; the delta ASSERT
  `:219` needs delta > 0). **The setters therefore validate N-aware,
  against the latched AVGSIZE at qom-set time**: a (rate, step,
  jitter) combination is rejected unless, for the latched N,
  `⌈63·N·(1+eps′)⌉ − 1 ≥ 63` and `⌊63·N·(1+eps′)⌋ + 1 ≤ 1070` for
  every eps′ the knob can produce at the estimate pair (eps′ = the
  drift, or the pair-period change a ±|step|/±|jitter| implies).**
  - `as-rate-ppm` (int32, default 0, **bounded to ±5000, and ≥ 0 at
    N = 1**): the diode tick period becomes 1 ms / (1 + ppm·1e-6) in
    virtual time while the LongTimer keeps 1 s per 1e9 ns, so the
    block period **measured in LongTimer time** is 63·N·(1+ppm·1e-6)
    ms. Positive = tick fast = blocks too long; negative = too short.
    At N=1 **any** negative ppm can truncate the pair delta to 62 →
    inc = 0 → ASSERT hang (the old "safe to −7937 ppm" was wrong: it
    ignored the ToMs truncation), so the validator rejects ppm < 0
    while N = 1 is latched; at N ≥ 4 the ±5000 ppm range is safe
    (worst-case delta 1002..1013 ≫ 63).
  - `as-jitter-next-ms` (write-only int32, default 0, **bounded to
    ±32**): added to the timestamp of the next flipped block only,
    then resets to 0 (a one-block error; it produces **two** gap
    events, entry and exit — D-ASh row 2). At N=16 the pair delta
    stays in [976, 1040] ms → inc ∈ {15, 16}; at N=1 the same
    validator rule applies (a ±32 jitter on a 63 ms pair can give
    delta 30 → inc 0, so the validator rejects it unless N ≥ 4).
  - `as-step-next-ms` (write-only int32, default 0, **bounded to
    ±32**): a **persistent** timestamp step — all blocks from the
    armed flip on get ±d ms (a biased FPGA epoch). One gap event only
    (the boundary), unless it corrupts the once-per-request increment
    estimate (D-ASh row 7, the #1031 hypothesis). The ±32 ms bound
    keeps the N=16 estimate delta in [976, 1040] ms → inc ∈ {15, 16};
    at smaller latched N the validator rule above decides.
    Arming:
    - `as-step-at-block` (write-only uint32, default 0): 0 = the next
      flip; otherwise the `block_seq` (model-wide flip counter, in the
      hw-state and the `stixasp_flip` trace) whose flip starts the
      step — the exact placement the step legs need.
  - `as-drop-next` (write-only uint32, default 0): the next k flipped
    blocks are **not appended** (the double buffer keeps running).
    Because of the archive's time-rewrite chain this leaves **no time
    gap** — its only signature is the payload content jump (D-ASh row
    5).
  - `as-enable` (bool, default true): the generation switch; false =
    no blocks at all (the negative leg).
  All are settable at startup with `-global grlib-stixctrl.as-*` and
  at runtime with `qom-set`, like `det-flare`.
- **D-ASe: asQlData.** Updated at the end of every row accumulation
  (the [F] "very last accumulation per diode": the last completed
  N-sum), upper 16 bits forced 0 on read. The FSW's 4 s HK timer
  (AspectCtrl.c:42-55) sums these into HK.
- **D-ASf: reset.** Device reset (with `as-model` on) stops the tick,
  clears the row/buffer state, the counters and `asQlData`, and writes
  `asCmdSta = 0` (channels off, AVGSIZE 0) — a watchdog reboot cuts the
  aspect supply, and the FSW re-powers and re-configures at every
  boot/SAFE/NOMINAL entry ([C] above). The scene knobs (`as-pattern`,
  `as-sun`, `as-rate-ppm`, `as-enable`) **survive** a reset (they
  describe the Sun, D-Db's rule); the one-shot knobs are state, not
  scene, and reset. `as-model=off` keeps today's no-reset behaviour.
- **D-ASg: observability.**
  - hw-state gains an `as` section through one
    `stix_aspect_hw_state(QDict *)` accessor: channels on/off, AVGSIZE
    (raw + derived N), rate-ppm, pattern, sun, enable, block_seq, rows
    in the open page, counters `pushed`/`dropped`/`jittered`/
    `stepped`/`dropped-knobs`/`rotb-full`, last flip SCET
    (coarse/fine), last QL values. The CTRL hw-state **schema bumps
    3 → 4** (`grlib_stixctrl.c:1461`, §10 precedent); note PL-11c on
    the platform track also bumps 3 → 4 — **whichever track merges
    second takes the next free number (5)** and rebases its note.
  - trace events, emitted only from actual actions (never getters):
    `stixasp_scene(source-pattern, sun, rate_ppm, enable)`,
    `stixasp_flip(block_seq, n_ms, coarse, fine)`,
    `stixasp_push(block_seq, result_name, rotb_count)`,
    `stixasp_drop(block_seq, reason)` (reason: knob or rotb-full),
    `stixasp_jitter(block_seq, delta_ms)`,
    `stixasp_step(block_seq, delta_ms)`.
- **D-ASh: the experiments and their expected FSW signatures** (the
  heart of the track; all FSW-side behaviour [C] from AspectCtrl.c and
  ArchiveManager.c, all expected TM evidence [L] until AS-5 runs).
  **The drift arithmetic, pinned** (re-review MAJOR D): the estimate
  pair delta is the difference of two whole-ms-truncated timestamps
  (`Timing_ToMs`, `Timing.c:395-399`: `coarse·1000 +
  fine·1000/65536` truncated), so the FSW's
  `deltaTms = (deltaTmsExtra + 5) / 10` rounding is a no-op on
  already-whole-ms inputs and
  **inc = floor((⌊t2⌋ms − ⌊t1⌋ms) / 63)** (`AspectCtrl.c:219-225`);
  the truncated delta is ⌊P⌋ or ⌊P⌋+1 for a nominal pair period
  P = 63·N·(1+eps), phase-dependent:
  - **N=16, negative**: the cliff sits at about **−30 ppm** — any eps
    < 0 makes P < 1008, so the truncated delta drops to 1007 →
    inc = **15** → residual ≈ **61 ms** → a gap at every block; for
    |eps| just below 0 the drop is phase-dependent (a ±30 ppm band
    where it depends on the sub-ms phase of the estimate pair), and
    leg 4's −2000 ppm (delta 1005..1006, residual ≈ 61 ms) is
    robust regardless of phase;
  - **N=16, positive**: inc stays 16 up to +6.2 %; the gap trips only
    when `P − 1008 > 5` ms, i.e. above about **+4960 ppm** (±30 ppm,
    residual = P − 1008);
  - **N=1**: even +10000 ppm does not trip (residual 0.63 ms), and
    the negative side can hang on the FSW's inc ≥ 1 ASSERT for any
    eps < 0 (D-ASd's N-aware validator keeps it out).
  Leg 3 pins **N=16**.

  | # | knob(s) (one leg of `aspect-smoke.sh`) | what the FSW sees | expected signature | issue |
  |---|---|---|---|---|
  | 1 | defaults (control) | blocks every 63·N ms, timestamps exactly 63·N ms apart | continuous SSID42 series: packets of 510 samples (last one short), SGF FIRST→CONT→…→LAST, `summingFpga = N`, packet start times 510·averaging·N ms apart (corrected at AS-4: one sample per `averaging` rows, AspectCtrl.c:364/:372), no 0x543F/0x5440 | control |
  | 2 | `as-jitter-next-ms=+6` mid-NOMINAL, budget 1 (default) | one archived record is +6 ms | **two** gap events (entry +6 and exit −6 of the affected record): the first forces a packet send (budget 1→0), the second hits the no-budget branch → forced final packet + **FSW_ERROR** → EID 0x5440 (user path) / TM(1,8) FID_48435 (TC 238,10 path) | #1058 (time jumps at packet boundaries, error shape) |
  | 3 | `as-step-next-ms=+6` (`as-step-at-block` mid-window), budget 1 | every record from the step on is +6 ms | **one** gap event at the step boundary (entry only — the offset persists, there is no exit): forced packet send, budget consumed, request **completes**; the packet start times jump by the step exactly there | #1058 (single-gap control) |
  | 4 | `as-rate-ppm=−2000` at N=16 (drift leg), budget 100 + averaging 64 (TCs) | inc = 15, residual ≈ 61 ms per block | a gap at **every** block → forced packets of ~1 completed sample each (averaging 64 spans ~1 block and is not reset at a forced send) → after ~100 gaps the no-budget branch → **EID 0x5440** after a leading 0-sample packet and ~98-99 one-sample packets (the first forced send fires before 64 samples have accumulated: 63 < 64) | #1031 (mechanism A: tick drift) |
  | 5 | `as-drop-next=2` | two blocks missing from the archive | **no time gap at all** (the time-rewrite chain keeps consecutive records 63·N ms apart); the request completes control-like; the only signature is the **payload content jump** (the ramp's block_seq skips 2 → decoded rows jump); assert **no 0x5440** and the content jump | #1026 (real data gaps: loss without a time gap) |
  | 6 | TC(236,6) `03 01 00 01` mid-NOMINAL (Avg16→Avg2) | block period drops 1008→126 ms mid-file; rows become N=2 sums | the once-per-request increment estimate is stale: per-block residual \|63·16 − 126\| = 882 ms > 5 → gap at every post-change block → forced sends → EID 0x5440 (budget 1) or the row-4 packet pattern (budget 100); TM values change to N=2 sums **divided by the still-16 averaging** (≈ v/8) | #1031/#1058 (estimate-once) |
  | 7 | **HYPOTHESIS** (`--step-1031`): `as-step-next-ms=−1` armed at the **first archived flip's own block_seq** (so the estimate pair straddles the step); budget 100 + averaging 64 | the estimate pair straddles a −1 ms step: truncated delta 1007 → **inc = 15** | deterministic for that epoch: every following block shows a ~61 ms gap → a 0-sample packet, ~98-99 one-sample packets → **EID 0x5440**; a re-request over the same epoch fails the same way (the records are fixed on flash). Mechanism: the estimate is taken once per request from the first two records of the first file in range (`AspectCtrl.c:196-237`), **before** the start-time skip loop (`:247-262`), and persists across files (`:196`) | #1031 (mechanism B: estimate corruption) — see the falsifiers below |

  Row 7 has a **built-in control leg** (`--step-1031-control`): the
  same −1 ms step placed **later** in the file (mid-window) leaves the
  estimate pair clean (inc = 16) and shows a **single** gap event (the
  step's entry boundary; the 1 ms is below the 5 ms gap threshold, so
  with −1 ms even that boundary is silent — the control asserts a
  clean completion and no per-block failure). A forward step of ≥ +5 ms
  placed anywhere gives one gap only (row 3's shape).
  **Epoch discriminator (re-review MAJOR E)**: because the estimate
  persists across files and is taken before the start-time skip, a
  request whose *window* starts after the step pair still reads the
  same file and fails — the working discriminator is a **later file**:
  the `--step-1031` leg returns to NOMINAL after the @320 flush (a
  second ~65 s epoch, D-ASi), whose records are all post-step and
  consistent; a request over the second file's range estimates
  inc = 16 and succeeds, matching #1031's "neighbouring epochs
  succeed". **What would falsify the row-7 hypothesis** (record in
  accepted-diffs §46): (i) the request completes despite the corrupted
  estimate pair (would mean the FSW re-estimates per file or the
  arithmetic differs from `AspectCtrl.c:219-225`); (ii) the failure
  pattern is not 0-sample/one-sample packets + 0x5440 (e.g.
  multi-sample forced packets); (iii) a forward step of the same size
  also produces per-block failures (would break the truncation
  asymmetry the mechanism rests on); (iv) the epoch-specific behaviour
  fails (the re-request over the same epoch succeeds, or the
  second-epoch request fails). As everywhere: a reproduced signature
  supports the mechanism, it does not prove the flight cause (Goal 4).
- **D-ASi: gates.** A new `scripts/stix/aspect-smoke.sh` (built like
  `detector-smoke.sh`: fresh flash run dir `STIX_ASPECT_RUN_DIR` with
  the slot library, `--hold/--dry-run/--check-guard/--negative`, a QMP
  socket, `-trace stixasp_*` plus the chunk traces into its own file,
  probe `--dump-apid 0x5DC:asp.dump.hex`), and a decoder
  `scripts/stix/aspect-check.py` (the `ql-check.py` pattern: its own
  self-test on hand-built packets, then SSID42 decode: SGF, start
  SCET, user_request_number, averagingTc, summingFpga, per-packet
  sample counts, per-sample diode values). It asserts, per leg, the
  D-ASh signature, plus:
  - timeline (seconds from connect; sync only on own-TC responses,
    Goal 5): @50 TC(236,1) `04 00` (CONF); @70 TC(236,1) `05 00`
    (NOMINAL) → TM(5,1) `0021 04 05` = T_nom; NOMINAL runs to @320
    (~250 s ≈ 248 aspect blocks at N=16, no full stream yet); @320
    TC(236,1) `04 00` — the NOMINAL→CONF flush (`OpModeMgmt.c:292-294`)
    writes the **one** aspect file (it holds the SAFE-, CONF- and
    NOMINAL-era records, Key facts "Archive"); @330 TC(237,18)
    `01 DD 00 00 00 00 64` (NUMBER_ASPECT_GAPS_ALLOWED = 100, only on
    the #1031-shaped legs) immediately followed by the read-back
    TC(237,19) `01 DD 00` → TM(237,20) on 0x5A8 must carry value 100 —
    a wrong param index fails loudly here instead of silently leaving
    the budget at 1; @340 TC(238,1) level-5 request for
    [T_nom−10, T_exit] with `time_binwidth` = the leg's averaging
    (16, or 64 on the #1031-shaped legs); @350 TC(238,2) → TM(238,3)
    on APID 0x5A8; the Background task processes it within ≤ 30 s +
    processing → SSID42 packets by ~T+45; @T+50 TC(238,10) for the
    same window (the second path; expect the same data again, request
    number 0); on `--step-1031` only, @335 TC(236,1) `05 00` again (a
    second ~65 s epoch, all records post-step), @400 TC(236,1)
    `04 00` (second file via the NOMINAL→CONF flush), @410 a second
    TC(238,1)+TC(238,2) pair with the window inside the second file —
    the epoch discriminator (D-ASh row 7); hold to +480.
  - UART checks: `init: SAFE mode` present; none of "packet sequence
    error", "UNKNOWN RB-BLOCK", "TOO MANY RB-BLOCKS", "RotBuf overrun"
    (the aspect blocks must not confuse the detector compactor);
    **exactly one "Skipped first Aspect data chunk" line** per archive
    epoch (one flush cycle with data → one skip), and no other `*E`
    line on the ideal leg.
  - legs (each a separate run; knobs via QMP at known stamps, the
    affected block identified from the `stixasp_*` trace):
    `--ideal` (row 1), `--jitter` (row 2), `--step-gap` (row 3),
    `--drift` (row 4: N=16 via the default, −2000 ppm, budget 100 +
    averaging 64), `--drop` (row 5), `--avg` (row 6: default budget 1
    and averaging 16, expect EID 0x5440 at the second post-change
    gap), `--step-1031` and `--step-1031-control` (row 7: −1 ms armed
    at `as-step-at-block` = the block_seq of the **first `stixasp_push`
    after the epoch's skip** — the first archived flip itself, no +1;
    read from the ideal leg's trace — vs the same step mid-window;
    `--step-1031` additionally runs the second epoch and its request),
    and `--negative` =
    `-global grlib-stixctrl.as-enable=false` (or `as-model=off`): no
    blocks, no aspect file, the request answers EID 0x543F (no data)
    and TC(238,10) answers TM(1,8) FID_48435.
  - wall-clock rule: every check uses probe monotonic stamps or trace
    (virtual) time, never the wall clock.
- **D-ASj: order of work** — register semantics first (the FSW writes
  them at boot), then the generator, then the knobs, then the live
  gate, then the experiment legs, then docs. Each commit's qtests are
  red on the parent binary at their first new assertion.

## Commits

Each commit is test-first; per-commit gates: the qtest suite
(`build/tests/qtest/stix`), checkpatch, boot-smoke (UART identical),
probe self-test, and a manual trace-golden diff (no `stixasp_*` lines
uninjected). The full set (soak, tc/nominal/detector-smoke ±) runs at
AS-5 and at the merge.

### AS-1: asCmdSta/asQlData semantics and reset (no data yet)
- 0x68: ASAON/ASBON (bits 0/1) and AVGSIZE (10:8) stored, read back;
  reserved bits (2..7, 11..31) read 0 (writes dropped, per
  `StixIdpu.h:681-685`). 0x6C-0x7B: the four QL words, upper 16 bits
  composed to 0 on read. Explicit device reset: `asCmdSta = 0`,
  `asQlData = 0`, unit state cleared (D-ASf); `as-model=off` keeps
  today's plain storage and no reset.
- qtests (`stix_aspect_register()`): `as-reg-reset` (0x68/0x6C read 0
  after reset), `as-rw` (enable/AVGSIZE round-trip, reserved bits
  dropped), `as-ql-mask` (a stored 0xFFFF0000 in a QL word reads 0),
  `as-model-off` (raw store, no reset).
- Gate impact: none expected — the FSW writes 0x68 before it ever
  reads it, and the boot print does not touch it [L].

### AS-2: the generator core (tick, rows, flip, push, QL, pattern, rate)
- The tick timer, the row accumulator with the AVGSIZE latch, the
  63-row double buffer, the flip, the block build (descriptor 0x4000,
  LongTimer at the flip, 504 B of 4×u16 BE rows), the
  `stixctrl_rotb_append(…, 1, …)` push with ENABLE/TESTMODE gating,
  channel-off zeros, `asQlData` per row (D-ASe), `as-pattern`/`as-sun`
  (D-ASc), `as-rate-ppm` (D-ASd), the traces and the hw-state `as`
  section (D-ASg), reset integration (D-ASf).
- qtests: `as-block-format` (byte-for-byte: **the block is exactly 512
  B**; descriptor = 0x4000 with **bits 13..7 = 0 and block number bits
  6..0 = 0**; timestamp monotonic and equal to the LongTimer at the
  flip; row r at 8r; A0/A1/B0/B1 order; values = N×(pattern+sun) mod
  65536), `as-timing` (for
  N = 1, 2, 4, 8, 16 the LongTimer delta between consecutive flips is
  63·N ms; `as-rate-ppm=1000000` gives 126·N ms), `as-avgsize-latch`
  (a mid-row AVGSIZE change applies from the next row/block),
  `as-channel-off` (one channel off → its two diodes 0, the other
  pair patterned), `as-ql-data` (the last row's values in 0x6C-0x7B),
  `as-push-gating` (rotbConfig TESTMODE=1 or ENABLE=0 → no push, the
  page still flips), `as-hw-state` (the JSON section), `as-reset`
  (system_reset stops the tick, clears state, keeps the scene knobs).
- Gate impact: none uninjected at boot — in SAFE the blocks pile into
  the ring (the FSW drains only in NOMINAL) and the UART goldens are
  untouched [L]. **But any gate that reaches NOMINAL now prints the
  FSW's "Skipped first Aspect data chunk" TRACE_ERROR once per archive
  epoch** (Correction 4), so AS-2 also updates `detector-smoke.sh`'s
  `uart-chunk` check (`:93-100`): the "no `*E` line from the archive"
  rule learns to whitelist exactly that line (counted: one per flush
  cycle that carried aspect blocks), while still failing on every
  other `*E`; `--negative`'s expectation shifts accordingly. The
  detector chunk checks themselves are untouched. accepted-diffs §46
  records the not-modelled 2 ms power-on delay and the
  ASPECT_SAMPLE_RATE 1024 vs 1 ms discrepancy.

### AS-3: the experiment knobs
- `as-jitter-next-ms`, `as-step-next-ms` (+ `as-step-at-block`) and
  `as-drop-next` (D-ASd), their flip-path integration and traces.
- qtests: `as-jitter` (one flip's timestamp shifted by the knob, the
  next unshifted, the knob back to 0), `as-step` (all flips from the
  armed `as-step-at-block` on shifted, earlier ones not; a next-flip
  arming with 0), `as-drop` (k blocks missing from the ring, the
  double buffer unaffected, the archived time chain continuous), and
  `as-knob-reject` (the N-aware validator: ppm < 0 with N = 1 latched,
  ppm beyond ±5000, jitter/step beyond ±32 ms or a combination whose
  worst-case estimate delta leaves inc outside 1..16, jitter/drop
  while `as-enable=false` → all rejected, state unchanged — the
  adc-inject setter pattern).
- Gate impact: none uninjected (defaults 0).

### AS-4: the live gate (ideal leg + negative)
- `scripts/stix/aspect-check.py` (self-test + SSID42 decode) and
  `scripts/stix/aspect-smoke.sh` with the D-ASi timeline, the ideal
  leg asserting signature 1 on both request paths, and `--negative`
  (`as-enable=false`) asserting 0x543F / TM(1,8) FID_48435 and no
  aspect TM at all.
- Before the first live run: confirm the gate ELF reaches NOMINAL with
  aspect archiving enabled (`AspectCtrl_configure` at NOMINAL entry,
  `OpModeMgmt.c:641` — expected, [L]), and the TM(238,3) APID for the
  list-report wait [L].
- Gates: the full set + aspect-smoke positive and negative.
- accepted-diffs §46 (first part).

### AS-5: the experiment legs
- `aspect-smoke.sh --jitter/--step-gap/--drift/--drop/--avg/
  --step-1031/--step-1031-control` (D-ASh rows 2-7), each asserting
  its signature; the #1031-shaped legs (`--drift`, `--step-1031`) set
  NUMBER_ASPECT_GAPS_ALLOWED = 100 by TC(237,18) `01 DD 00 00 00 00 64`
  with the TC(237,19)/TM(237,20) read-back check, and request with
  averaging 64 via TC(238,1) level 5 (the path that produces EID
  0x5440); `--step-1031` adds the second epoch (D-ASi). aspect-check.py
  allows a leading 0-sample forced packet on the #1031-shaped legs
  (MINOR: 63 < 64 samples at the first gap). The first live runs are
  [L] by definition — the exact packet sample counts and the trip
  behaviour are recorded against the D-ASh expectations (the drift
  trip points are pinned there, so the leg asserts direction and rough
  magnitude, not a guessed threshold).
- The full gate set at the end; the results (signatures observed,
  which knob reproduces which issue's symptoms) are folded into
  accepted-diffs §46 and, if the user wants, into the GitHub issues
  as emulator evidence — always with the Goal-4 caveat.

### AS-6: docs
- accepted-diffs §46 completed (the §38 "No aspect ROTB blocks" bullet
  is replaced by a pointer to §46), `ground-station.md` gains an
  "Aspect scene" section (knobs, experiments, how to decode SSID42),
  this plan's status line updated.

## Risks

- **The FSW's gap arithmetic is integer and once-per-request**
  (increment estimated from the first two archived records with
  `(delta+5)/10` rounding and floor division, u24 relative box
  times): the trip points are pinned in D-ASh, but a leg can still be
  off by one block at the boundary; AS-5 records first, asserts
  second.
- **The #1031 mechanism depends on the flight configuration being
  reproducible here**: budget 100 (TC(237,18), per the #1026
  comments) and averaging 64 via TC(238,1). If the live legs with
  that configuration still do not produce the ~99 one-sample packets
  + 0x5440, the hypothesis space is narrowed but the flight cause
  stays open (Goal 4) — the recorded observables are the evidence.
- **The `--step-1031` placement is the fragile part**: the step must
  be armed at the **first archived flip's own block_seq** (the first
  `stixasp_push` after the epoch's skip), so the estimate pair spans
  the skipped flip's raw end and the first archived flip's (stepped)
  raw end; armed one flip later the pair stays clean and the leg
  degrades to the control signature. The `as-step-at-block` knob makes
  it deterministic given the block_seq from the trace; a placement
  miss degrades to the control signature (single gap / silent), which
  the leg detects from the trace and reports as a placement failure,
  not as a falsification.
- **Request processing time**: the FSW's burst loop runs per sample in
  the Background task; a 250 s window at N=16 with averaging 64 is
  ~250 output samples — small, but the 30 s tick plus processing must
  fit the smoke's hold windows; the TC(238,10) leg runs second, after
  the user-request leg has drained.
- **The probe connect race** (Goal 5): the smoke design avoids
  boot-TM, but if `tm-hold` lands first the boot-group asserts can be
  tightened; until then 0x0022/0x14A3 are recorded, never awaited.
- **Ring interaction**: aspect blocks interleaved between detector
  chunks are safe by descriptor dispatch ([C] `RotAccumulator.c:146-166`),
  and the BQL rule makes mid-chunk interleave impossible; the UART
  checks in AS-4 pin it live.
- **`asCmdSta` reset to 0 on watchdog reboot** is a model choice [I]:
  the FSW re-powers immediately (init/SAFE entry), so it is invisible
  unless a gate reboots mid-NOMINAL — the memory-upset reboot gates
  will show it first [L].

## Open questions

1. Do real blocks flow when **both** channels are off (and in
   MAINTENANCE, where the FSW cuts the power)? The model produces
   zero-diode blocks whenever `as-model` is on (D-ASb). If the FPGA
   gates the readout on channel power entirely, only the `as-enable`
   knob's meaning changes — a one-line model change, but the negative
   leg's expected TM is identical either way. User decision wanted.
2. The pinned drift trip points (D-ASh, re-review MAJOR D: with
   Timing_ToMs truncation the negative cliff is ≈ −30 ppm at N=16,
   phase-dependent in a ±30 ppm band; ≈ +4960 ppm on the positive
   side; nothing at N=1) are derived from `AspectCtrl.c:219-225` and
   `Timing.c:395-399`; the AS-5 live run confirms them before they
   become hard gate expectations [L].
3. Whether the flight #1031 requests used TC(238,1)+TC(238,2) (the
   EID 0x5440 says yes) with averaging 64 and the gap budget raised
   to 100 by TC — the budget/averaging figures come from the GitHub
   #1026 comments [A, user-stated]; the legs make both explicit so a
   wrong guess is visible in the observables.
4. Does the gate ELF (STIX_DEFAULT_ELF) archive aspect data in the
   modes it reaches — expected yes (it enters NOMINAL in
   detector-smoke; aspect archiving is unconditional on blocks
   arriving) [L at AS-4].
5. Aspect HK on-demand channels (photodiode voltages, temperatures,
   references in the HK scan): out of scope unless trivial (the
   brief's own caveat); none of the experiments needs them.

## Review resolutions (2026-11-13, review `asp-plan-review.md`)

| item | verdict | resolution |
|---|---|---|
| BLOCKER 1 (rows 2/4 contradict the archive time-rewrite chain, ArchiveManager.c:243-245/:265-266) | accepted | D-ASh rewritten: a dropped block leaves **no** time gap (row 5: assert no 0x5440 + payload content jump); a one-block jitter gives **two** gap events and, with budget 1, ends in FSW_ERROR/0x5440 (row 2); the new persistent `as-step-next-ms` knob gives the single-gap #1058 leg (row 3) |
| MAJOR 2 (one-sample packets unreachable) | **rejected, with correction** | the reviewer assumed budget 1 and averaging 1; in flight the budget was raised to 100 by TC (#1026 comments) and with averaging 64 the per-block average spans ~1 block and is not reset at a forced send (`AspectCtrl.c:317-342`) — ~99 one-sample packets then 0x5440 are reachable. Rows 4 and 7 and the legs are built on that configuration (TC(237,18), averaging 64; the TC payload was corrected to `01 DD` by the re-review, below) |
| MAJOR 3 (DEBUG_PRINT: live TRACE_ERRORs and hanging ASSERTs) | accepted | Correction 4 rewritten (`commonFSW.h:336-378`); knob bounds derived in D-ASd so no ASSERT can trip; AS-2 plans the `detector-smoke.sh` `uart-chunk` whitelist for the "Skipped first Aspect data chunk" line; aspect-smoke expects exactly one skip line per epoch |
| MAJOR 4 (pin the drift trip points) | accepted | D-ASh states the increment formula with the N=16 thresholds and leg 4 pins N=16 at −2000 ppm (superseded by the re-review's truncation-aware numbers, below) |
| MINOR: hw-state schema bump | accepted | D-ASg: 3 → 4 (`grlib_stixctrl.c:1461`), with the PL-11c coordination note (second merger takes 5) |
| MINOR: ASPECT_SAMPLE_RATE 1024 vs 1 ms | accepted | recorded as a Key-fact bullet (`AspectCtrl.h:12`); the model keeps the [F] 1 ms tick |
| MINOR: :332-334 is the raw post-gap timestamp, not the accumulated estimate | accepted | Key facts fixed (`:409-411` accumulated at a full-packet send, `:332-334` raw at a gap-forced send) |
| MINOR: ABORT → 0x5445 | accepted | Key facts now state FSW_ERROR_ABORT → EID 0x5445, other errors → 0x5440 (`SciReport.c:461-477`) |
| MINOR: row 4 self-contradiction; row 5 value arithmetic | accepted | both rows rewritten (row 5 is now the drop leg; the AvgSize leg is row 6 with the /16 averaging note) |
| MINOR: TM(238,3) APID | accepted | resolved: 0x5A8 (`TM_238.c:116`, `:151-162`); open question removed |
| MINOR: qtest asserts (512 B, descriptor bits) | accepted | added to `as-block-format` (AS-2) |
| MINOR: param indices | accepted | resolved by count from the 315 anchor: ASPECT_SUMMED_VALUES = 140, NUMBER_ASPECT_GAPS_ALLOWED 477 (corrected by the re-review, below) [I]; open question removed |

## Re-review resolutions (2026-11-13, review `asp-plan-rereview.md`; Claude verified A and the Timing.c:395-399 truncation behind C/D)

| item | verdict | resolution |
|---|---|---|
| BLOCKER A (budget TC sets the wrong parameter: :546 is ATT_AUTONOMY_MOTOR_USE, :547 = 477) | accepted | index recounted with the `#ifdef` blocks: the PFM block (`ParamMgmtDef.h:18-65`) is compiled out in the AX gate ELF, the AX block (`:67-113`, 46 entries) is in; index(:547) = 315 + 162 = **477 = 0x01DD** everywhere (Key facts, D-ASi @330, AS-5, resolutions). New run-time check: after TC(237,18) the smoke sends TC(237,19) `01 DD 00` and asserts TM(237,20) (APID 0x5A8, `TcService237.c:97-144`, `TM_237.c:30-48`) carries value 100 — a wrong index fails loudly |
| BLOCKER B (`--step-1031` placement off by one → degrades to the control) | accepted | D-ASi: `as-step-at-block` = the block_seq of the **first `stixasp_push` after the epoch's skip** (the first archived flip itself, no +1); the Risk text now says "the skipped flip and the first archived flip" |
| MAJOR C (bounds not ASSERT-safe "for every N"; ToMs truncation) | accepted | D-ASd rewritten: the pair delta is ⌊P⌋ or ⌊P⌋+1 (whole-ms truncation, `Timing.c:395-399`); the setters validate N-aware against the latched AVGSIZE (`⌈P′⌉−1 ≥ 63`, `⌊P′⌋+1 ≤ 1070`); ppm < 0 is rejected at N=1 (the old −7937 ppm figure was wrong); ±32 step/jitter claims are scoped to N=16 with the validator covering smaller N; `as-knob-reject` qtest updated |
| MAJOR D (−496 ppm cliff wrong; +4960 stands) | accepted | D-ASh/open question 2: `inc = floor((⌊t2⌋ms − ⌊t1⌋ms)/63)` (the `(delta+5)/10` rounding is a no-op on whole-ms inputs); negative cliff ≈ **−30 ppm** (phase-dependent ±30 ppm band), positive ≈ +4960 ppm (±30 ppm); leg 4's −2000 ppm is robust (residual ≈ 61 ms regardless of phase) |
| MAJOR E (row 7 discriminator contradicts the estimate-once code) | accepted | the window-start discriminator dropped (the estimate is taken before the skip loop and persists across files, `AspectCtrl.c:196-237` vs `:247-262`); replaced by the **second epoch**: `--step-1031` returns to NOMINAL after the @320 flush (~65 s), flushes a second (post-step, consistent) file and requests it — ~1 min of extra smoke time, within the decision's budget |
| MINOR 1 (averaging wording) | accepted | D-ASc: "averaging defaults to N, overridden to 64 on the #1031-shaped legs" |
| MINOR 2 (--avg budget/averaging unpinned) | accepted | D-ASi: `--avg` pins the default budget 1 and averaging 16, expecting 0x5440 at the second post-change gap |
| MINOR 3 (first forced packet carries 0 samples at averaging 64) | accepted | rows 4/7 and AS-5 note the leading 0-sample packet (63 < 64, counter not reset); aspect-check.py allows it |

## Amendment B (2026-11-13, the first live AS-5 runs)

Seven legs passed live (ideal, negative, jitter, step-gap, drift, drop,
step-1031-control; commit `1a936defa9`). Three plan statements did not
survive the live runs.

**B1, row 5 (`--drop`) — corrected, done in AS-5.** The archive stores
each aspect block with the *previous* block's end (ArchiveManager.c, the
"override the time of the last ending chunk" step after the
`aspBlockSkipedFirst` branch). Dropped blocks never reach the archive,
so the first record after the drop still reads contiguous, and the next
one carries the post-drop end: the gap is delayed by one record, not
removed. Live: one gap event, forced packet, the request completes
(packets 303/510/143, the forced start delta ≈ 2 blocks longer than
`--step-gap`'s). The leg now asserts the forced-complete signature;
"no time gap at all" in D-ASd and D-ASh row 5 is superseded.

**B2, row 6 (`--avg`) — the TC is mode-gated.** TC(236,6) is applicable
only in MAINTENANCE and CONFIGURATION (TCVerifDynamic.c:66); sent at @150
in NOMINAL it gets TM(1,8) FID_48301 (wrong operation mode, param 5 =
NOMINAL). So an AvgSize change *inside* one NOMINAL epoch cannot happen
by TC, and the once-per-request estimate cannot go stale within an
epoch. The realistic hazard is a request spanning **two epochs with
different AvgSize**. Redesign (`--avg`):
- epoch 1 as now (Avg16, NOMINAL @70 → CONF @320, file 1);
- @325 TC(236,6) `03 01 00 01` (Avg2) in CONF → TM(1,1)/TM(1,7);
- @330 NOMINAL (epoch 2, blocks every 63 × 2 ms = 126 ms), @390 CONF
  (file 2 flushed);
- budget 100 (TC(237,18)/(237,19) as on `--drift`) and a level-5
  TC(238,1) over the whole archive with averaging 64, urn 1, @400;
  TC(238,2) @402;
- expectation [L]: the estimate comes from file 1's first pair (inc 16,
  1008 ms); the epoch boundary is one gap event (budget 100 → 99); then
  every Avg2 record differs from the estimate by 882 ms → a gap per
  record → the row-4 pattern (~99 forced 0/1-sample packets) →
  EID 0x5440 with the 45 s window widened to 90 s. Asserted: 0x5440,
  ≥ 50 forced packets, and the forced packets' start times inside
  epoch 2 (direction + rough magnitude; the exact count is recorded).
- the leg's default `STIX_ASPECT_SECONDS` is 540 (request @400, the
  0x5440 window of 90 s ends @490, plus margin).
- The TC(238,10) path is dropped on this leg (one request is enough to
  show the mechanism; it keeps the run inside one Background slot).

**B3, `--step-1031` — the epoch-1 request is sent inside epoch 2.** The
D-ASi order (@335 NOMINAL again, @340 the epoch-1 request) put the
request into NOMINAL; it got TM(1,1)/TM(1,7) but no answer within its
45 s window, which ended the run at ≈ +381 s. The only documented
NOMINAL skip is a flare in progress (SciReport.c:354-358); whether the
emulated detector trips the flare flag in epoch 2 is **open (O-B1)** and
not needed for the leg. Reorder so each request is answered in CONF:
- @320 CONF (file 1), @330/@331 budget 100 + read-back, @340 TC(238,1)
  urn 1 averaging 64, @342 TC(238,2); EID 0x5440 within 60 s;
- @400 NOMINAL (epoch 2, −1 ms step still armed at the first archived
  block of epoch 1 only), @460 CONF (file 2), @470 TC(238,1) urn 2 with
  the window start at a fixed 405 s (these legs never set the time, so
  SCET counts from boot: the ideal leg's first archived block after the
  @70 NOMINAL is stamped ≈ 76 s; 405 s lies after the @400 entry and
  before epoch 2's first archived record), @472 TC(238,2); the urn-2
  answer within 60 s; forbids 0x543F/0x5440/0x5446 after @470;
- the TC(238,10) path dropped on this leg as on `--avg`;
- the leg's default `STIX_ASPECT_SECONDS` 560.

**B4, probe windows.** A missed expectation ends the whole probe run, so
every request-answer window on the #1031-shaped legs is ≥ 60 s (two
Background slots of 30 s, background.c:63).

**Open O-B1.** Why the request sent at +340 in epoch-2 NOMINAL was not
served for 41 s (≥ one Background slot). Candidates: the flare flag
(SciReport.c:354), or the slot phase plus Background load in NOMINAL.
A dedicated run would tell; not a gate item.

### Amendment B review resolutions (GLM review 2026-11-13, all accepted)

- **BLOCKER (mode race):** the NOMINAL→CONF transition commits the mode
  only after the archive flush (OpModeMgmt.c:287-302, :702), so a
  TC(236,6) 5 s after CONF may still see NOMINAL. `--avg` timeline:
  @320 CONF, **@332** TC(236,6) `03 01 00 01` with
  `--forbid 0x5A1:1:8:<its seq>` (a rejection names FID_48301 instead of
  timing out), @334/@335 budget 100 + read-back, **@340** NOMINAL
  (epoch 2), **@400** CONF (file 2), **@410** TC(238,1) urn 1
  averaging 64, @412 TC(238,2), EID 0x5440 within 90 s, default
  `STIX_ASPECT_SECONDS` **550**.
- **MAJOR (first forced packet):** the boundary's forced packet is
  epoch 1's open packet (~235 samples, start ≈ 76 s), so a new check
  mode `--expect-stale` (not `--expect-collapse`, which encodes the dead
  single-epoch design) asserts: one product per urn; packet 0 large
  (> 100 samples) starting in epoch 1; ≥ 50 further forced packets of
  0/1 samples whose starts lie in epoch 2 (≥ 330 s); every header
  summing 16 / averaging 64 (the stale-estimate evidence); EID 0x5440
  in the probe. Exact counts are printed in a NOTE ([L]).
- **MAJOR (step persistence):** the model's step stays in force from
  `as-step-at-block` on (stix_aspect.c:160-161, :184-186), so epoch 2's
  flips are shifted too. Harmless (uniform −1 ms keeps epoch-2 deltas at
  1008 ms → inc 16, the urn-2 request succeeds). The B3 text "epoch 1
  only" is superseded; no disarm needed.
- **MINOR (window start):** 405 s is after file 1's end (~325 s) and
  before file 2's end (~460 s); whether it precedes epoch 2's first
  record does not matter (the skip loop, AspectCtrl.c:247-262, handles
  a start inside file 2; the estimate pair is independent of the
  window). Expected urn-2 samples ≈ 48-52 (recorded, not asserted).
- **MINOR (code):** `--avg` and `--step-1031` expect 2 skip lines of
  each kind (two epochs) and **one** gap-terminate line (the TC(238,10)
  path is dropped on both); the request-answer windows become 60/90 s;
  per-leg default `STIX_ASPECT_SECONDS` (avg 550, step-1031 560); the
  ≥ 450 floor applies to the legs that keep the TC(238,10) group.
- **O-B1 (plausible, not verified):** one request per 30 s Background
  tick (background.c:63, SciReport.c:361-392) at Background priority 150
  under the NOMINAL real-time tasks, plus the full archive walk and ~101
  forced TM sends, easily exceeds 41 s; the flare gate is the only
  documented *skip*. A discriminating run (TC(238,10) bypasses the tick)
  is noted for later, not a gate item.
