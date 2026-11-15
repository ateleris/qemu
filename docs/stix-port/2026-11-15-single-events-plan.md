# Detector stimulus Stage S3: single events (the `single` step kind) — plan (2026-11-15)

> For agentic workers: implement task by task; steps use `- [ ]`.
> Workers cannot build or run anything: write code + self-test cases,
> the reviewer runs them and commits. This plan expands the S3 outline
> of `2026-11-15-stimulus-plan.md` ("Stage S3", Tasks V1 + V2 gate); the
> outline's wire interfaces are fixed and are not reopened here (the
> step shape `{"kind":"single","for_s","fpga_det","pix","adc"|"ebin",
> "n" 1..1000000,"wait_us" 1..10000 default 20}`, `fpga_det = dss_det`,
> the event schedule feeding `stix_det_add_counts`, the trace name
> `stixdet_single`, `ebin` + `align` as in S2, the board's DSS-sequence
> control wired here).

**Goal:** a tape step can be a DSS-faithful single-event burst:
`det(F, P, A) × N, wait W us` — N events at one adc on one
(`fpga_det`, `pix`) pair, spread over `n·wait_us` of tick-quantized
guest time — or the same with an explicit science `ebin` instead of the
adc. The events flow through the D-Df event sink `stix_det_add_counts`,
so the per-tick pix gate, SUMDMASK, SUMCNT, LVAR and the 1.25×
LTRG/ETRG rules all apply exactly as for a real event. An adc burst is
binned through the loaded ELUT exactly as DSS replay events are; an
`ebin` burst with `align` goes through the preimage of the loaded ELUT
(S2 semantics, including the `preimage_miss` drop). The Stimulus
board's "Single events" chip and the "DSS sequence" composer are
enabled. The FSW-visible end is a new `detector-smoke.sh --single`
leg.

**Architecture:** everything stays in `stix_det.c` on the 100 ms tick
(S1's engine). At `det-tape-load` the parser validates the single
shape (one new branch of `stix_det_tape_parse_step`, the model's rule
table below) and stores the target and the schedule in the step. While
a single step plays, a per-tick feed — a new `stix_det_single_tick`
called from `stix_det_tick` right after `stix_det_tape_tick` — adds the
events whose time falls in this tick's 100 ms window, in one
`stix_det_add_counts` call per tick (all events of a step share the
pair; the sink applies the pix gate, SUMDMASK/SUMCNT/LVAR and the
nominal/extra-trigger rules). The pix-gated share is counted in a new
`dropped` gauge reported as a trailing field of `det-tape-status` (and
as hw-state `det.tape.dropped`). A single step contributes no sky rate
(its implicit masks are the one pair, its cps 0), draws no RNG, and
touches no guest memory outside the tick. The platform
(`tapes.py`) already parses the shape and mirrors the model's rules as
the validator of record; the UI's two disabled chips are enabled. The
FSW-visible end is `detector-smoke.sh --single`.

**Spec/inputs:** the S3 outline (stimulus plan "Stage S3", Tasks V1 +
V2 gate), lessons 1-7 below, F6 (STIX-FSW #1069, accepted-diffs);
as built: `accepted-diffs.md` §54 (S1), §55 (alignment), §56 (ADC
sky), §57 (spectra). Mapping to the outline: the outline's V1 is
V1–V3 here, its V2 gate is V4.

## Key facts

Read in this worktree unless marked UNVERIFIED.

- **The parser today.** `stix_det_tape_parse_step`
  (`hw/sparc/stix_det.c:3348`) refuses kind `single` with "kind
  \"single\" is unsupported in this build (S3)" (`:3376-3379`); the
  kind enum already carries `STIX_DET_TAPE_SINGLE`
  (`stix_det.h:111-117`). The unknown-field check (`:3464-3483`) is one
  shared `known[]` list (`kind, for_s, label, fpga_det_mask, pix_mask,
  cps, spec, file, offset_ms, align`), so the single fields need their
  own per-kind accepted set. Integer fields parse through
  `stix_det_tape_uint` (`:2946`, JSON-int + range, `1600.0` refused);
  `for_s` through `stix_det_tape_num` (`:2924`, 0.1..86400,
  tick-quantized by `stix_det_tape_step_ticks` (`:2747`, 10 ticks/s).
  The spec parse (`stix_det_spec_parse`, called last, `:3484-3489`)
  refuses a non-flat spec on a non-rate kind (S2's rule table).
- **The tick and the step clock.** One tick is 100 ms
  (`STIX_DET_TICK_NS`, `:61`); the order inside `stix_det_tick`
  (`:274-310`) is tape → sky → replay → inject, all under the tick's
  `mem_reentrancy_guard`. A step starts at a tick boundary
  (`stix_det_tape_apply_step`, `:2816-2841`, called from
  `stix_det_tape_tick` `:2851-2896`); `spec_preimage_miss` is reset
  there at every step start (`:2823`). SAFE/ClearEnable freezes the
  step clock (the tick only runs while Enable = 1, D3).
- **The event sink.** `stix_det_add_counts` (`:1350-1380`): the
  per-tick pix gate first (`stix_det_pix_gate` `:1168-1182` — a grp
  that reads out and R3 < 63; a gated-off pix drops the deposit,
  `:1361-1363`), then `inj_ql`/`inj_nom` (SUMDMASK `:1365`), SUMCNT
  (SUMEMASK, `:1367-1370`), ETRG-only for masked-out fpga_det
  (`nom_trg_extra_grp`, `:1372`), LVAR (VARDMASK × VARPMASK ×
  VAREMASK, `:1374-1379`). The injected counts reach LACC at the next
  QL swap and EACC at the next nominal swap, and are counted in the
  1.25× triggers: LTRG/ETRG per grp = `(5·n + 2) / 4` clamped at 24
  bits (`:2389-2392`; the det-inject-chunk precedent,
  `tests/qtest/stix.c:8404-8436`).
- **The adc → ebin lookup.** `stix_det_adc_ebin` (`:1634-1647`): a
  honoured row's `val` (32 = unassigned → −1); a degenerate row's D-7
  fallback `(adc − base)·32/(4096 − base)` with `adc <= base` → −1
  (`stix_det_replay_fallback_ebin` `:1578-1586`); `base` is the
  realized replay table's 0xE baseline, 0 without one
  (`stix_det_cal_base` `:1604-1607`). The replay tick's order is gate
  first, then the lookup, ELUT drops silent (`:2215-2231`) — the
  single feed copies it. The preimage helper `stix_det_align_adc`
  (`:1773`, declared `stix_det.h:460`): −1 for a degenerate row or no
  adc mapping to the ebin; on a degenerate row align is a no-op (S2).
- **Status strings.** `det-tape-status` (`:3680`) prints
  `state=...,name=...,step=K/N,label=...,step_left_ticks=...,loop=...,
  align=...`; the hw-state `det.tape` object (`:4581-4608`) mirrors
  it. Exact-match consumers of the line today: the five literal
  expected lines of qtest `det-tape-status`
  (`tests/qtest/stix.c:9627-9684`) and `det-spec-props`'
  status0/keeper pair (`:10638-10645`, self-consistent); `plat.py`
  `_tape_status_casts` (`:473-478`) casts a fixed key list and
  ignores extra keys; `fake_qemu.py:383` serves a canned line; the
  smoke gates grep substrings (`grep -qF`, tolerant).
  `det-align-status` ends `,preimage_miss=N` since S2 (lesson 2).
- **The single shape in the platform.** `tapes.py` `_step`
  (`:190-200`) already parses it: `fpga_det` 0..31, `pix` 0..11,
  `n` 1..1000000, `wait_us` 1..10000 default 20, `adc` 0..4095 or
  `ebin` 0..31, exactly one — matching the outline's ranges
  (verified). `selftest.py:2810-2815` pins that the shape parses; the
  model is the authority at load.
- **Index spaces (lesson 1).** `SUBCOL_FROM_FPGA_DET[22] = 19`
  (verified: `scripts/stix/elut-upload.py:68-73`,
  `stixplat/tapes.py:57-62` — two FSW-sourced copies pinned equal by
  the self-test). `ql-check.py --ssid32-byte SUBCOL EBIN` takes the
  0-based ebin (`:101-106`); an SSID 32 sample is the subcol byte,
  then ebins 0..31, trigger, offset (`:155`, `:270-279`), so "byte 3"
  is **ebin 2**. The S0 compressor (`:165-175`):
  `s0_compress(1000) = 0x3F` (decodes to [960, 1024)), `s0_compress(500) = 0x37`
  (ql-check.py:165-184; §36's 250-count deposit measured 0x2F).
- **The gate's ELUT (lesson 1 continued).** The outline's gate ebin is
  ebin 2 for `det(22, 0, 2200)`: that holds **only** for the
  degenerate-row state with the gate's dssevs baselines
  `1800 + 10·d` (align-smoke's law, already copied into
  detector-smoke by U5) — fpga_det 22 → base 2020,
  `(2200 − 2020)·32 / (4096 − 2020) = 2.77` → ebin 2. Under the S2
  gate's base-0 uploaded ELUT the same adc is ebin 17, and under the
  qtest helper `det_adc_file_baselines(&f, 1800)` (all bases 1800,
  `stix.c:10093-10101`) it is ebin 5 — each leg below pins its own
  base and ebin explicitly.
- **The checks available.** `ql-check.py --ssid32-inject SUBCOL EBIN
  MIN_SPIKE MAX_QUIET` (`:617-654`): in the one SSID 32 dump the
  target sample's byte at EBIN ≥ MIN_SPIKE and every other sample's
  byte at EBIN < MAX_QUIET, the rest of the target's bytes only
  logged against its subcol ± 1 neighbours — the single-gate byte
  check is this check with new numbers, no new ql-check code. It
  requires exactly one SSID 32 product. `check_tape_flush`
  (`:672-693`) asserts at least one non-zero value byte per SSID
  30/31/32/33, so a fully silent sky fails it by design (lesson 4).
  SSID 32's dump sums QlIntTime 319 + 1 = 320 ticks = 32 s (`:474`).
  The ROTB chunk payload is not TM-visible in the gate scripts; the
  chunk's total is: the `stixdet_chunk` trace line carries
  `eacc_events` (§35).
- **F6 (STIX-FSW #1069, accepted-diffs :3227-3267).** The FSW's
  NOMINAL R3 write leaves only pix 0, 3, 6, 7, 8 below 63 — the
  model's pix gate passes 0.475 of a 12-pix template. FSW
  configuration; the model is not changed (do not "fix" F6). In
  qtests `idefx_power_all` leaves all 12 pix open; an R3 = 63 for one
  pix is written with `idefx_write_reg(qts, 9, 0, 3, r3, sizeof(r3))`
  (`stix.c:7205`, the det-counts-r3 pattern).
- **The quiet template (lesson 4).** The template has 10 % over
  ebins 1-11 and lines at 18/19/27; a 20 cps background over a 32 s
  dump adds ≈ 3 counts at ebin 2 of any subcol (0.475 pix weight ×
  0.10/11 share) — S0 ≤ 0x03, far under both the 0x3F floor and the
  0x10 quiet bound.

## Global constraints

Inherited from the stimulus plan (unchanged): no FSW edits
(STIX-FSW read-only); every new name carries its index space
(`fpga_det`, `pix`, `ebin`, `adc`; `subcol` only where the TM
numbering is meant, `fpga_det = dss_det`); API v1 additive only; UI
rules (one `ui/index.html`, no innerHTML / inline handlers / inline
styles, verbs via `data-verb`); guest-time scheduling only; QOM
setters store fields only and the tick applies them;
`python3 stixplat.py --self-test` stays PASS and every task adds
`check(...)` cases. Plus, for S3:

- **Lesson 1 (byte vs ebin, verified).** `ql-check --ssid32-*` takes
  the 0-based ebin; "byte N" of an SSID 32 sample is ebin N−1 (byte 0
  is the detector byte). This plan writes ebins only. The outline's
  "subcol 19 = SUBCOL_FROM_FPGA_DET[22]" is verified (two pinned
  copies); its "ebin 2" is verified **for the gate's ELUT state
  only** (degenerate rows + the `1800 + 10·d` dssevs baselines); the
  qtest legs use base 1800 and ebin 5, stated per assertion.
- **Lesson 2 (status fields).** `det-align-status` ends
  `,preimage_miss=N` since S2; any exact-match status string in this
  plan includes it. S3 adds a trailing `,dropped=N` to
  `det-tape-status` (the running step's pix-gate drops, a gauge reset
  at every step start, 0 unless a single step ran); every
  exact-match consumer is updated in the same task: the five literal
  lines of `det-tape-status` (`stix.c:9627-9684`), `det-spec-props`'s
  pair (`:10638-10645`, re-verified), `plat.py`
  `_tape_status_casts` gains `"dropped": int`, `fake_qemu.py`'s
  canned line, and §54's reported-field list (V5). The smoke gates'
  `grep -qF` consumers are substring-based and need no change.
- **Lesson 3 (non-vacuous negatives).** `--single --negative` fails
  exactly `single-byte single-chunk` (the gated pix's counts reach
  neither the SSID 32 byte nor the chunk total) while
  `single-status` (`dropped=1000`), `single-trace`, the flush and
  the base checks pass. Every qtest "exactly 0" cell is paired with
  an arrival assertion elsewhere.
- **Lesson 4 (the quiet template).** The gate keeps
  `det-quiet-cps=0` and plays a 20 cps background (as `--tape`):
  `--expect-tape-flush` needs SSID 31 (BG `fpga_det` 8/9) and SSID 33
  non-zero, which a one-pair burst never lights. The background's
  ebin-2 share on any sample is ≈ 3 counts (S0 ≤ 0x03) against the
  0x3F floor and is stated in the check's comment. The qtests use
  background 0 and `det-quiet-cps=0`, so their cells are exact.
- **Lesson 5 (pixels, F6).** With the FSW only pix 0, 3, 6, 7, 8
  count in NOMINAL. The gate's positive uses pix 0; the negative
  uses pix 1 (R3 = 63 there). `det-single-gated` sets the threshold
  itself with `idefx_write_reg` and runs outside the FSW (all 12 pix
  open otherwise). Nothing is changed about F6.
- **Lesson 6 (evidence, relative thresholds).** A single step draws
  no RNG: its counts are exact, and so is the compressed byte —
  `s0_compress(1000) = 0x3F` — but the gate's floor is `0x37` = S0 of
  `n/2` (robust to the background's ≈ 3 counts; the measured byte,
  expected 0x3F, is logged). `single-chunk`'s window
  `[1000, 1250]` derives from n and the background's worst nominal
  integration (bounded and logged). Every check logs the bytes,
  status lines and trace lines it decides on; a failed live run is
  investigated, not re-run.
- **Lesson 7 (goldens).** A UART golden must not change: nothing new
  at boot (the single kind parses at `det-tape-load` only; the new
  status field reads `dropped=0` whenever no single step ran;
  `det-tape-seed`, `det-adc-off` and the S1/S2/C1 qtests keep their
  values — a single step consumes no RNG draws and adds no
  exposure).
- **Guest memory only under the tick guard.** The feed runs inside
  `stix_det_tick` under the guard and touches model RAM only
  (`inj_*` via the sink); no guest memory from QOM setters or status
  getters (PL-2, as S2).

## Approaches considered

1. **Where the schedule lives.** (i) Per-event timer callbacks:
   rejected (1e6 timers; not tick-quantized, not deterministic in
   the same stream). (ii) A per-tick feed driven by the existing
   100 ms tick, events due by `k·wait_us` in the tick's window, one
   `stix_det_add_counts` per tick (recommended; the replay's
   `t_tick` batching precedent, `:2215-2231`). (iii) Feeding from the
   sky tick with a synthetic cps: rejected (the pix gate and the
   template shares would distort a one-pair burst; the sink is the
   outline's fixed interface).
2. **One sink call per tick vs per event.** Per tick (recommended):
   the sink applies the gate/SUMDMASK/SUMCNT/LVAR rules to the whole
   batch identically (all events share the pair and the ebin), and
   1e6 calls per step would dominate the tick. The trace records the
   batch, so the per-event granularity survives in the log via the
   schedule law.
3. **The gated count's home.** (i) A new read-only property:
   rejected (a second status line for one gauge). (ii) The trace
   only: rejected (the outline says "counted in a status"). (iii) A
   trailing `,dropped=N` on `det-tape-status` + hw-state
   `det.tape.dropped` (recommended; the S2 `preimage_miss`
   precedent, lesson 2).
4. **The gate's EACC and LTRG evidence.** The chunk payload is not
   TM-visible in the gate scripts, so the outline's "EACC cell holds
   the counts" is asserted exactly in qtests (`det-single-land`'s
   chunk leg) and as the `stixdet_chunk` trace's `eacc_events`
   window in the gate; LTRG is an AHB SRAM (not a TM field), so its
   1.25× rule is asserted exactly in `det-single-land` (LTRG grp 11
   = 1250) and the gate logs the target sample's SSID 30 trigger
   byte for the record only (deviations D-a/D-b; open question 3).

## Review focus (five input classes, each pinned to a test)

1. **Step values and ranges** (the parse table: ranges, the adc/ebin
   exclusivity, the `wait_us` default, integer vs number, the
   coverage rule, `spec`/`align` on single): pinned by
   `det-single-props` — every rejection a failed `det-tape-load`
   that leaves the previous tape intact, every acceptance loading to
   `state=ready`; the same vectors in the platform's `tapes_single`
   self-test (the validator of record).
2. **The schedule across ticks** (tick quantization, the start
   tick's first window, bursts spanning ticks, the last window,
   loop/stop): pinned by `det-single-land` — the trace's per-tick
   batch sizes (5000 × 4 for n 20000 wait 20) sum to n and the bank
   holds exactly n; the start-tick rule is pinned by the n 1000 leg
   landing whole.
3. **The sink semantics** (pix gate, ELUT drops vs gate drops,
   SUMDMASK/SUMCNT/LVAR, the 1.25× LTRG/ETRG, EACC): pinned by
   `det-single-land` (LACC cell + neighbours + LTRG 1250 + the chunk
   leg) and `det-single-gated` (R3 = 63 drops the batch, `dropped`
   counts it, the trace still shows the schedule).
4. **Mode combinations** (`ebin` direct vs `ebin` + `align` on the
   three row states, single under a background, single + `spec`
   refused, the RNG stream untouched): pinned by
   `det-single-land`'s ebin legs (`preimage_miss=1` on the no-12
   row, direct landing on the wrong row) and `det-single-guard`'s
   bank-identity check.
5. **Time and context** (ClearEnable mid-burst freezes the schedule,
   steps under `clock_step`, stop/loop mid-burst): pinned by
   `det-single-guard` (the det-tape-guard pattern; every CTRL MMIO
   path still answers; after resume the remaining events land, total
   still n).

---

## Task V1: the single parse, the status field, hw-state (model)

**Files:** modify `hw/sparc/stix_det.c`, `hw/sparc/stix_det.h`,
`tests/qtest/stix.c`.

**Interfaces — produces:**

- `stix_det.h`: `StixDetTapeStep` gains
  ```c
  uint32_t fpga_det;          /* single: the target (dss_det = fpga_det) */
  uint32_t pix;               /* single: the target pix */
  uint32_t n;                 /* single: 1..1000000 events */
  uint32_t wait_us;           /* single: 1..10000, default 20 */
  int32_t adc;                /* single: -1 when absent */
  int32_t ebin;               /* single: -1 when absent */
  ```
  (`STIX_DET_TAPE_SINGLE` exists already, `stix_det.h:115`).
- `stix_det.c` (static):
  ```c
  static bool stix_det_single_parse(StixDetState *d, QDict *obj,
                                    bool tape_align, StixDetTapeStep *st,
                                    uint32_t idx, Error **errp);
  ```
  called from the `single` branch of `stix_det_tape_parse_step`
  in place of today's refusal (`:3376-3379`). The unknown-field check
  becomes per-kind: the single branch accepts
  `fpga_det, pix, adc, ebin, n, wait_us` on top of the common set, so
  a rate step carrying `n` stays refused.
- **The rules (the model and `tapes.py` implement this one table;
  every error names the step index):**

  | field | rule |
  |---|---|
  | `fpga_det` | required, JSON integer 0..31 (`dss_det = fpga_det`) |
  | `pix` | required, JSON integer 0..11 |
  | `adc` / `ebin` | exactly one required: `adc` a JSON integer 0..4095, `ebin` a JSON integer 0..31; both present or both absent refused |
  | `n` | required, JSON integer 1..1000000 |
  | `wait_us` | optional, JSON integer 1..10000, default 20 |
  | `for_s` | the common 0.1..86400 rule **plus** the coverage rule against the quantized duration: `n·wait_us ≤ stix_det_tape_step_ticks(for_s)·100000` (error "for_s %g s (%u ticks) does not cover n·wait_us = %g s"; orchestrator call, open question 1) |
  | `spec` | refused on single (any value, `"flat"` included: "a single step takes no spec") |
  | `align` | optional bool, default the tape's; an explicit `true` only with `ebin` ("align applies to ebin singles") |
  | `label` | the common rule (≤ 64 bytes, no comma) |

  The effective `align` is stored on the step (a bool like S2's
  `sp->align`; a degenerate row makes it a no-op at feed time, as in
  S2).
- `StixDetState` gains `uint32_t single_next, single_dropped;` (the
  schedule cursor and the pix-gate drop gauge; `single_next` ≤ 1e6
  fits). `single_next` resets at every step start in
  `stix_det_tape_apply_step` (next to the `spec_preimage_miss`
  reset, `:2823`); `single_dropped` resets only when playback starts
  (`det-tape-play true` applied at the tick, and at a loop restart),
  so it sums the whole play and a status read after the burst's step
  still holds it (plan review M2).
- `det-tape-status` gains a trailing `,dropped=%u` (formatter
  `:3680`); hw-state `det.tape` gains `dropped` (`:4581-4608`). Both
  additive (lesson 2's consumer list).
- The kind-name table (`:2738-2744`) reports `single`.

Steps:

- [ ] qtest `det-single-props` (RED on the parent: every single
  shape is refused there). Each rejection is a failed
  `det-tape-load` and leaves the previous tape intact
  (`det-tape-status` unchanged). Vectors (shared with V3's
  self-test, same order):
  - shape: `fpga_det` 32; `fpga_det` missing; `pix` 12; `pix`
    missing; `n` 0; `n` 1000001; `n` missing; `n` 1000.0;
    `wait_us` 0; `wait_us` 10001; `wait_us` 20.0; `adc` 4096;
    `adc` −1; `ebin` 32; `ebin` −1; `adc` + `ebin` both; neither;
    `adc` 1600.0; `spec: "flat"`; `spec: {"line_adc": 1600}`;
    `align: true` with `adc`; `align: "yes"`;
  - coverage: `n` 1000000, `wait_us` 10000, `for_s` 0.1 (needs
    10000 s); `n` 6000, `wait_us` 20, `for_s` 1 (needs 1.2 s);
    `n` 6000, `wait_us` 20, `for_s` 0.12 (rounds to 1 tick, needs 2);
  - accepted (each loads, `state=ready`): the outline's mono hint
    `{"fpga_det": 22, "pix": 0, "adc": 1000, "n": 1000}`;
    `n` 1000000 with `for_s` 10000, `wait_us` 10000; `wait_us`
    absent (default 20); `{"ebin": 7}` alone; `{"ebin": 31}`;
    `adc` 0; `adc` 4095; `align: true` with `ebin`; `align: false`
    with `adc`; tape `align: true` with an `adc` single (ignored
    there).
- [ ] Update `det-tape-props`: its "S3 kind" case
  (`stix.c:9553-9557`, the refused `single`) is now accepted —
  replace it with a still-refused single vector (`"n": 0`).
- [ ] Update the five literal expected lines of `det-tape-status`
  (`stix.c:9627-9684`) to end `,dropped=0`; re-verify
  `det-spec-props`' pair (`:10638-10645`).
- [ ] Implement the types, the branch, the per-kind known-field
  check, the coverage rule, the status/hw-state fields, the resets.
- [ ] Run `scripts/stix/qt.sh all`: the new and updated tests pass,
  the rest unchanged.
- [ ] Commit.

**Verification:** `scripts/stix/qt.sh all` — all ok;
`det-single-props` RED on the parent binary.

## Task V2: the schedule and the feed (model)

**Files:** modify `hw/sparc/stix_det.c`, `hw/sparc/trace-events`,
`tests/qtest/stix.c`.

**Interfaces — produces:**

- `trace-events`:
  `stixdet_single(uint32_t fpga_det, uint32_t pix, int32_t adc,
  int32_t ebin, uint32_t n, int64_t vclock_ns) "single burst:
  fpga_det %u pix %u adc %d ebin %d n %u at vclock_ns=%" PRId64`
  (fixed name; `adc`/`ebin` are −1 when absent/dropped).
- `stix_det.c` (static):
  ```c
  static void stix_det_single_tick(StixDetState *d);
  ```
  called from `stix_det_tick` right after `stix_det_tape_tick`
  (`:289`), so the step's start tick feeds its first window. Return
  immediately unless a tape is playing and the running step's kind
  is single.
- **The schedule law (one rule, deterministic):** event k (0-based)
  of the running step is due at `t_k = k·wait_us` µs after the
  step's start tick. Tick j of the step (j = 0 is the start tick)
  feeds every due k with `j·100000 ≤ k·wait_us < (j+1)·100000` (µs)
  in ONE `stix_det_add_counts(d, fpga_det, pix, ebin, batch)` call,
  where `batch` is the number of such k and the cursor
  `single_next` advances past them. Consequences (each pinned by a
  test): with `wait_us 20`, one 100 ms tick holds ≤ 5000 events
  (the outline's bound); n 1000 lands whole in the start tick;
  n 20000 spans ticks 0..3 with batches 5000 each; the largest
  possible burst (n 1e6, wait_us 1) spans 10 ticks. The cursor also
  caps at the step's `n`.
- **The per-tick resolution (the replay's order, `:2215-2231`):**
  1. the pix gate first: a gated-off pix drops the whole batch —
     `single_dropped += batch`, and the batch is still traced (n =
     batch, `ebin` −1);
  2. an `adc` step: `ebin = stix_det_adc_ebin(d, fpga_det, pix,
     adc)` — −1 (unassigned 32, or `adc <= base` on a degenerate
     row) drops the batch's mass silently, like the replay (not
     counted in `dropped`; the missing mass is the evidence);
  3. an `ebin` step with align off: `ebin` is the step's — a direct
     deposit at the science bin (the det-inject semantics), whatever
     ELUT is loaded;
  4. an `ebin` step with align on: on a honoured row,
     `adc = stix_det_align_adc(d, fpga_det, pix, ebin)`; −1 is a
     preimage miss — the batch is dropped and `spec_preimage_miss`
     counts one (the S2 gauge, reported by `det-align-status`; NOT
     counted in `dropped`); the landed ebin is `stix_det_adc_ebin`
     of that adc (= the step's ebin). On a degenerate row align is
     a no-op: rule 3 runs.
  5. trace one `stixdet_single` line per tick with the batch's n and
     the resolved ebin (−1 when the gate, the ELUT or a preimage
     miss dropped it); the sum of n over the step equals the step's
     `n` always (drops change where the mass went, not the
     schedule).
- A single step contributes no sky rate: at parse its implicit
  masks are stored as `fpga_det_mask = 1u << fpga_det`,
  `pix_mask = 1u << pix`, `cps = 0`, so `stix_det_sky_tick` needs no
  change (the step's masks zero its pair, the background still
  fills the other pairs, S1 precedence). A single step draws no RNG.
- Stop/done/loop/step-change ends the feed (the cursor belongs to
  the running step; the resets of V1 cover it). Belt and braces:
  the feed checks the running step's kind every tick.

Steps (all qtests: `det-quiet-cps=0`, tape background `cps 0`,
`idefx_power_all` (all 12 pix open), a replay table from
`det_adc_file_baselines(&f, 1800)` (all bases 1800 — so
`det(22, 0, 2200)` is **ebin 5** here: `(2200−1800)·32/2296 = 5.57`;
the gate leg's base 2020/ebin 2 is a different setup, see V4), play
on the tick after a swap as `det-tape-rate` does):

- [ ] qtest `det-single-land` (RED on the V1 binary: the parse
  accepts but nothing feeds), one tape of four steps on the
  `det_spec_qtest` row states (`fpga_det` 0 the wrong `adc/128` row,
  1 degenerate, 2 the no-12 row):
  - step 1 `{"fpga_det": 22, "pix": 0, "adc": 2200, "n": 1000,
    "for_s": 0.2}`: the bank the step's swap closed holds LACC
    (pix 0, fpga_det 22, ebin 5) = 1000 (the `det-inject-lacc`
    decode, raw offset `(0·32+22)·32+5` big-endian u16), every
    neighbour (F ± 1, P ± 1, B ± 1) and the whole bank total stay at
    0 / 1000; LTRG grp 11 = `(5·1000+2)/4` = **1250** exactly, every
    other grp 0; the trace holds exactly one `stixdet_single` line
    (n 1000, ebin 5) — one batch, the start tick's window.
  - step 2 `{"fpga_det": 22, "pix": 0, "adc": 2200, "n": 20000,
    "wait_us": 20, "for_s": 0.5}`: the trace holds four lines with
    n 5000 each; the bank holds 20000 at the same cell; LTRG grp 11
    = 25000 (4 × (5·5000+2)/4; the test pins a QL period that holds
    all four burst ticks, `det_start(qts, 39)` as det-inject-chunk).
  - step 3 `{"fpga_det": 2, "pix": 0, "ebin": 7, "n": 1000, "for_s": 0.2}`
    (align off, the wrong row): the bank holds ebin 7 = n (a direct
    science-space deposit — the det-inject semantics; with
    `det-adc-mode` on it still lands at 7, single counts never
    fold).
  - step 4 `{"fpga_det": 2, "pix": 0, "ebin": 12, "n": 1000,
    "align": true, "for_s": 0.2}` (the no-12 row: no adc maps to ebin 12): nothing
    lands, `det-align-status` ends `preimage_miss=1` and starts
    `mode=off` (lesson 2); the trace line carries ebin −1, n = the
    step's n.
  - a chunk leg on step 1's setup (`rotb_cfg` + `sky_start_nom`, the
    `det-inject-chunk` pattern): the next chunk's EACC (0, 22, 5) =
    1000, ETRG grp 11 = 1250, the EACC bank zero after the read;
    with fpga_det 22 masked out of SUMDMASK the deposit is
    ETRG-only (the D-Df rule, `:1365-1373`).
  - a mode leg: step 1 with the tape's `"adc_mode": true` — the
    bank is unchanged (the deposit is added after the fold; no
    double fold).
- [ ] qtest `det-single-gated` (RED on the V1 binary): as step 1 but
  with R3 = 63 written for pix 1's `asic_ch` (15,
  `utils_pixelId2channelId[1]`) on fpga_det 22's grp 11
  (`sky_r3_wire(r3, 15, 63)` then
  `idefx_write_reg(qts, 11, 0, 3, r3, sizeof(r3))` — grp 11, not
  det-inject-gated's grp 9) and the step on pix 1: the bank total is
  exactly 0, `det-tape-status` (read while the step runs) ends
  `dropped=<n>`, the trace still holds the batch line with ebin −1
  (the schedule ran; the gate dropped the mass). A control run with
  R3 = 62 for the same pix lands its counts (paired arrival,
  lesson 3).
- [ ] qtest `det-single-guard`: the `det-tape-guard` pattern with a
  single step changing under `clock_step` — every CTRL MMIO path
  still answers; plus a ClearEnable mid-burst (n 20000): the
  schedule freezes with the step clock (`step_left_ticks` holds, no
  `stixdet_single` lines while frozen), and after resume the
  remaining events land — the final bank total is still exactly n;
  and a three-run seed check: the banks around a single step are
  bit-identical across runs with and without the step (a single
  step consumes no RNG — the `det-spec-flat-ident` pattern).
- [ ] Implement the feed, the trace, the resets' interaction with
  stop/loop.
- [ ] `scripts/stix/qt.sh all`: all ok, including `det-tape-seed`,
  `det-adc-off`, `det-spec-*`, `det-replay-*` unchanged.
- [ ] Commit.

**Verification:** `scripts/stix/qt.sh all`; each V2 test RED on the
V1 binary (the tape loads and plays, no counts land);
`git grep -c stixdet_single hw/sparc/trace-events` = 1.

## Task V3: the platform — validator of record, UI, self-test

**Files:** modify `scripts/stix/platform/stixplat/tapes.py`,
`stixplat/plat.py` (`_tape_status_casts`), `stixplat/selftest.py`,
`stixplat/tests/fake_qemu.py` (the canned `det-tape-status` line),
`ui/index.html`, `docs/stix-port/platform.md`.

**Interfaces — produces:**

- `tapes.py` `_step`'s `single` branch implements V1's rule table
  exactly (it already has the ranges, `:190-200`); it gains:
  - the coverage rule: `for_s * 1e6 >= n * wait_us`, else 400
    "step %d: for_s %g s does not cover n·wait_us = %g s";
  - `spec` present on a single step → 400 ("a single step takes no
    spec");
  - `align: true` with `adc` → 400 ("align applies to ebin
    singles"); `align` absent/false/true-with-ebin pass through.
  Same message key words as the model; the step index in every
  error.
- `plat.py` `_tape_status_casts` gains `"dropped": int` (additive;
  extra keys in the line are already ignored, `:486-493`).
- `fake_qemu.py`'s canned `det-tape-status` gains `dropped=0`
  (faithfulness).
- `ui/index.html`:
  - the "Single events" chip is enabled (no `disabled`, the "arrive
    in stage S3" title gone); next to the rate/spectrum inputs it
    reveals `stim-single-adc` (int 0..4095), `stim-single-ebin`
    (int 0..31; the two swap by a small `stim-single-mode` select
    adc/ebin, one `hidden`), `stim-single-n` (int 1..1000000,
    default 1000), `stim-single-wait` (int 1..10000, default 20);
    "Add to tape" / "Send now" emit `{"kind": "single", "for_s":
    <duration, raised to cover n·wait_us>, "fpga_det": <the selected
    detector>, "pix": <the one pixel>, "adc"|"ebin", "n",
    "wait_us"}` — a multi-detector target or an "all 12" pix select
    is refused with a toast (a single targets one pair, the DSS
    `det()` form).
  - the "DSS sequence" chip is enabled: it toggles a
    `stim-dss-text` textarea whose lines are
    `det(F, P, A) x N, wait W us` (W optional, default 20 — the
    mockup's mono hint is the placeholder); its "Add to tape" parses
    each line into one single step (`for_s` = the coverage, raised
    to 0.1 s), toasts the line number on a parse error, and refuses
    more than 64 steps.
  - `stimSettings` shows a single step as `det(22, 0, 2200) x 1000
    · 20 us`; the progress readout shows `det.tape.dropped` while a
    single step plays (the leaf arrives through `state.py`'s
    pass-through, no code).
  - the blue box: single events and DSS sequences are supported; the
    stimulus board is complete.
- `platform.md`: the leaf (`det.tape.dropped`), the single recipe (a
  one-pair burst lands where the loaded ELUT puts the adc; an ebin
  single with align drops on a preimage miss), the DSS text grammar.

Steps:

- [ ] Self-test `tapes_single`: V1's vector list through
  `tapes.validate` — each rejected vector a 400 naming the step
  index (the coverage rule's message key words included), each
  accepted vector validates; a single step with a `file` spec → 400.
- [ ] Self-test `tapes_status_dropped`: a fake hw-state with
  `det.tape.dropped` 1000 → the leaf; `_tape_status` casts
  `dropped` 1000 from a line that carries it and ignores one that
  does not (backwards tolerance); the `sampler_paths` case still
  passes (no new glob).
- [ ] Self-test "UI: stimulus board" (`t_ui_stimulus`): the two
  chips have no `disabled`; the six new ids exist; `stimSettings`
  renders `det(22, 0, 2200) x 1000 · 20 us` for a single step; the
  page rules (no inline handlers/styles, verbs served) still pass.
- [ ] Implement; `platform.md` as above.
- [ ] `cd scripts/stix/platform && python3 stixplat.py --self-test`
  → `SELF-TEST: PASS`.
- [ ] Commit.

**Verification:** the self-test PASS; the reviewer's browser pass
(platform.md checklist) adds: compose a `det(22, 0, 2200) x 1000`
line via the DSS textarea, save, reload, the table shows the single
step; the progress readout shows `dropped`.

## Task V4: the S3 gate — `detector-smoke.sh --single`

**Files:** modify `scripts/stix/detector-smoke.sh` (ql-check.py needs
no change: `--ssid32-inject` is the byte check).

**Interfaces — produces:**

- `detector-smoke.sh --single [--negative]` (combines with none of
  `--flare`, `--tape`, `--no-rotb`, `--spectrum`; run-dir suffix
  `-single` / `-singleneg`; verdict tag `DETECTOR-SMOKE (single)` /
  `DETECTOR-SMOKE (single negative)`).
- The base is the `--tape` one (no 85 s upload shift — no ELUT is
  uploaded): QEMU with
  `-global grlib-stixctrl.det-replay-file=<run>/dssevs.spec`
  (the 32 0xE setup words, base `1800 + 10·d`, no events —
  `write_adc_dssevs`, already in detector-smoke from U5),
  `det-replay-trigger=qmp` (the replay engine stays idle; it exists
  only to give the calibration baselines — without it every base is
  0 and the gate's adc would bin to ebin 17, lesson 1),
  `det-quiet-cps=0`; the trace events list adds `stixdet_single`,
  `stixdet_tape_step`, `stixdet_chunk`.
- **The timeline** (seconds from the TMTC connect; the default
  mode's times, no shift, `AT_CONF2=215`, probe `DURATION` 240 s):
  @50 CONF, @60 S20 = 0, @62 the
  verification period, @70 NOMINAL (T_nom ∈ [70, 80]), @76 QMP `det-tape-load` +
  `det-tape-play true` (inside the T_nom ± 10 s window, so the burst
  is inside the first 8 QL periods the SSID 32 dump sums):
  `{"api": 1, "name": "gate-det-single", "background": {"cps": 20},
  "steps": [`
  `{"kind": "single", "label": "mono", "for_s": 4, "fpga_det": 22,
  "pix": 0, "adc": 2200, "n": 1000, "wait_us": 20},`
  `{"kind": "off", "for_s": 30, "fpga_det_mask": 4294967295}]}`.
  The 20 cps background exists for the flush evidence only (lesson
  4: `--expect-tape-flush` needs SSID 31 and 33 non-zero; a one-pair
  burst never lights them); its ebin-2 share on any sample is ≈ 3
  counts (S0 ≤ 0x03), stated in the check's comment. @100 QMP
  `qom-get det-tape-status` (T_nom ≤ 80, so by @100 the 4 s burst
  step has run and the 30 s off step is playing: `step=2/2`;
  `dropped` sums the whole play). @215 CONF (T_exit) + the
  five-product flush; the probe ends at the default 240 s.
- **Checks** (each echoes what it decided on):
  - base: probe, UART, `trace-verif`, `uart-chunk`, and the flush
    with `--expect-tape-flush (T_exit − T_nom)`; the D-5 chunk
    checks are skipped as in the tape modes.
  - `single-status`: the @100 return ends `dropped=0` (negative:
    `dropped=1000`) and holds `state=playing`, `step=2/2`; echoed.
  - `single-trace`: the trace file holds `stixdet_tape_step 0
    kind=single` after the enabling `stixdet_fsm_strobe`, and the
    `stixdet_single` lines after it sum to n = 1000 with
    `fpga_det 22 pix 0 adc 2200` and ebin 2 (negative: ebin −1; the
    sum is still 1000).
  - `single-byte`: `ql-check.py --ssid32-inject 19 2 0x37 0x10` on
    the exit dump — subcol 19 = `SUBCOL_FROM_FPGA_DET[22]`
    (verified, lesson 1), ebin 2 (byte 3; the degenerate-row
    fallback with base 2020 puts adc 2200 there:
    `floor(180·32/2076) = 2`), the floor 0x37 = S0 of n/2 (the
    exact expected byte is 0x3F = S0 of 1000, logged), every other
    sample's ebin-2 byte < 0x10 (the background floor, ≈ 3 counts).
  - `single-chunk`: in the trace, the first `stixdet_chunk` line
    after the last `stixdet_single` line has
    `1000 <= eacc_events <= 1250` (the deposit is exact; the +250
    headroom is the 20 cps background's worst nominal integration ≈
    0.475·20 cps·20 s·1.3, the bound and the measured value
    logged). This is the TM-visible stand-in for the outline's EACC
    cell (deviation D-b; the cell itself is asserted in
    `det-single-land`'s chunk leg).
  - for the record only (printed, not asserted): the target
    sample's SSID 30 trigger byte against its subcol ± 1 neighbours
    (LTRG is an AHB SRAM, not a TM field — deviation D-a; the 1.25×
    rule is asserted exactly in `det-single-land`).
  - Verdicts: positive PASS when nothing fails; `--negative` PASS
    only when the failed set is exactly `single-byte single-chunk`
    (so `single-status` with `dropped=1000`, `single-trace`, the
    flush and the base checks passed), with the line
    `DETECTOR-SMOKE (single negative): PASS (failed exactly as
    planned: the gated pix 1 burst reached neither the SSID 32 byte
    nor the chunk total; the status counted 1000 dropped)`.
- **The negative:** the same tape with `"pix": 1` (R3 = 63 there
  under F6 — only pix 0, 3, 6, 7, 8 pass the model's gate in
  NOMINAL; lesson 5): the schedule runs (the trace sums 1000), the
  gate drops every event (`dropped=1000`), and nothing reaches
  ebin 2 or the chunk — the intended evidence, never vacuous
  (lesson 3).
- `--dry-run --single` prints the QEMU and probe lines (the @76 tape
  JSON and the @100 status read visible); the reviewer runs both
  legs.

Steps:

- [ ] The header comment: the `--single` block after `--spectrum`'s
  (the timeline, the background rationale, the checks, the
  negative's expected failed set); `--single` combines with none of
  the other modes; the run-dir suffixes.
- [ ] Implement the mode: the flag parsing, the run-dir suffix, the
  QEMU args (dssevs, trigger, quiet-cps, traces), the @76/@78 QMP
  group, the checks, the verdict tags. Reuse `write_adc_dssevs`
  unchanged; no `elut-derive`/`encode_args` calls.
- [ ] `--dry-run --single` prints as specified.
- [ ] Commit.

**gates2 legs** (`gates2.sh` is outside the repo; the form of its
`det` group's entries, cf. the S2 plan's `detspec` lines; the verdict
is the script's exit code; 300 s = the 240 s default run + margin —
no upload shift in this mode):

```
add_leg detsingle    det      300 1 'STIX_DETECTOR_RUN_DIR=/tmp/stix-$TAG-detsingle' \
    'scripts/stix/detector-smoke.sh build/qemu-system-sparc --single'
add_leg detsingleneg det      300 1 'STIX_DETECTOR_RUN_DIR=/tmp/stix-$TAG-detsingleneg' \
    'scripts/stix/detector-smoke.sh build/qemu-system-sparc --single --negative'
```

**Verification:** `STIX_PORT_BASE=auto scripts/stix/detector-smoke.sh
build/qemu-system-sparc --single` → `DETECTOR-SMOKE (single): PASS`;
`--single --negative` → `DETECTOR-SMOKE (single negative): PASS`;
`--dry-run --single` prints the plan above; SKIP without the ELF; a
re-run of the `--tape` and `--spectrum` legs still passes (the
shared code paths did not change).

## Task V5: accepted-diffs and docs

**Files:** modify `docs/stix-port/accepted-diffs.md`,
`docs/stix-port/ground-station.md`.

- [ ] New **§58 "Detector stimulus S3: single events (baseline:
  none)"**: the step shape and the rule table (V1), the coverage
  rule and its decision, the schedule law (event k at k·wait_us, the
  per-tick window, ≤ 5000 events per 20 µs tick, the
  trace-per-batch rule), the sink semantics (gate first, then the
  adc→ebin lookup / the ebin rules; the pix-gate drops counted in
  the `dropped` gauge, ELUT drops silent as in the replay, the align
  preimage miss in `preimage_miss`), `det-tape-status`'s
  `,dropped=N` and the hw-state leaf, no sky rate / no RNG / no
  double fold, the pix gate (F6 as §57 states it), the platform's
  rules and the UI's two controls, the gate record (both legs, the
  measured byte and `eacc_events` from the first live run), and the
  deviations D-a/D-b.
- [ ] Amend **§54** (one paragraph): kind `single` is accepted
  (§58); the refusal text is gone; `det-tape-status`'s
  reported-field list gains `dropped`.
- [ ] Amend **§57** (one paragraph): the S2 rule table's "a non-flat
  spec is valid on kind `rate` only" now also names single's no-spec
  rule; the `preimage_miss` gauge also counts an aligned ebin
  single's misses (one per batch); the `dropped` gauge is the
  pix-gate counterpart of the replay's `dropped`.
- [ ] `ground-station.md` "Detector scene": the single/DSS recipe (a
  one-pair burst, where it lands, reading `dropped`).
- [ ] Commit.

**Verification:** the reviewer reads §58 against the built
behaviour; no doc build exists.

## Merge order and gates

V1 → V2 → (V3 parallel from V1; V4 needs V2) → V5. Each model task:
new qtests RED on its parent, the suite green after; V3: self-test
PASS; V4: both legs PASS plus a re-run of the `--tape` and
`--spectrum` legs (their status substrings and schema must still
hold). UART goldens unchanged (nothing new at boot;
`det-tape-status`'s new field reads `dropped=0` without a single
step).

## Deviations from the S3 outline (deliberate)

- **D-a.** The outline's "LTRG grp 11 ≈ 1.25×" gate evidence is
  asserted exactly in `det-single-land` (1250, zero elsewhere) and
  in the gate only logged: LTRG is an AHB SRAM region, not a TM
  field, and the SSID 30 trigger byte's grp mapping is not
  established from the code (open question 3).
- **D-b.** The outline's "the next ROTB chunk's EACC cell holds the
  counts" is asserted cell-exactly in `det-single-land`'s chunk leg;
  in the gate the chunk payload is not TM-visible, so the evidence
  is the `stixdet_chunk` trace's `eacc_events` window (V4), and the
  outline's SSID 32 byte check is `--ssid32-inject 19 2 0x37 0x10`
  (the D-6 check reused with new numbers).
- **D-c.** The coverage rule (`for_s ≥ n·wait_us`, refused at load)
  is an orchestrator call the outline is silent on (open question
  1); the model and `tapes.py` change together if the owner flips
  it.
- **D-d.** The outline's "spreading n events over n·wait_us of
  replay-style time" is realized as one sink call per tick batch,
  not per event (approach 2) — the schedule law and the trace make
  the per-event timing recoverable, and the sink's rules are
  batch-invariant here (one pair, one ebin per batch).

## Open questions for the owner

1. **`for_s` shorter than `n·wait_us`.** (a) Refuse at load (the
   coverage rule; recommended — no silent loss, the model and
   `tapes.py` stay one table, and the UI composes `for_s` from the
   schedule anyway); (b) truncate at the step end (DSS-faithful "the
   step ends, the burst dies with it" — but a truncated burst is
   easy to misread in the TM); (c) extend the step to cover the
   burst (`for_s` becomes a lower bound — surprises the step
   table). The tasks implement (a); flipping changes V1's table,
   `det-single-props`, `tapes_single` and the UI composer together.
2. **What the DSS-sequence control composes.** (a) One single step
   per `det(F, P, A) x N[, wait W]` line, appended to the tape,
   `for_s` auto-covered (recommended — the DSS tool's sequence is
   exactly a list of such bursts, and the tape is the model's
   schedule); (b) a fixed mono preset
   (`det(22, 0, 1000) × 1000, wait 20 us`) as a one-button "send
   now" — smaller, but not a sequence; (c) a full DSS `.dssevs`-file
   import — out of scope here (the replay step already plays
   files). The tasks implement (a).
3. **The gate's LTRG evidence.** (a) Keep the 1.25× rule qtest-only
   and log the SSID 30 trigger byte for the record (recommended —
   no new probe surface, and the rule is already pinned exactly);
   (b) assert the SSID 30 trigger byte of the target sample once
   the sample ↔ grp mapping is confirmed from the FSW code; (c) add
   a probe-side LTRG SRAM read (a QMP `human-monitor-command` `xp` —
   a new probe capability for one number).
4. **Does `dropped` also count the ELUT drops (unassigned 32,
   `adc <= base`)?** (a) No — pix-gate drops only, parity with the
   replay's `dropped`, and the ELUT drop stays visible as missing
   mass in the trace's ebin −1 (recommended; a mixed gauge would
   blur the F6 evidence); (b) yes, with the trace distinguishing
   the two (a second trace field or a second gauge).

## Plan review (2026-11-15, GLM, findings folded in)

- B1 (blocker): the unshifted mode's CONF is @215 with a 240 s probe
  (`detector-smoke.sh:422-428`); the V4 timeline now says so (was
  "@300 CONF … hold 240 s"). The gates2 legs keep 300 s as margin.
- M2: T_nom is only bounded to [70, 80]; the @78 status read could see
  `state=ready`. Fixed by reading @100 and resetting `single_dropped`
  at play start, not at step start (the burst stays DSS-faithful:
  1000 × 20 µs in one tick).
- M3: S0 constants corrected (`s0(1000) = 0x3F`, `s0(500) = 0x37`);
  the byte floor is 0x37.
- M4: the coverage rule is checked against the quantized tick count
  (`stix_det_tape_step_ticks` rounds to nearest); the UI/DSS composer
  sets `for_s = max(0.1, ceil(n·wait_us / 100000) / 10)`.
- M5: V1/V3 must carve single out of S2's "align applies to keV specs"
  refusal (`stix_det_spec_parse` `stix_det.c:3309-3314`, run after the
  single branch; `tapes.py _spec` `:118-121`): on a single step `align`
  is decided by V1's table, and `spec` is refused before `_spec` runs.
- M6: det-single-gated writes grp 11 / asic_ch 15 (fixed above).
- m7: det-single-land steps 3/4 gain `n`; step 2 pins the QL period.
- m8: the SSID 32 window: the live records (D-3's exit dump, §36's
  T_nom+5..15 deposit seen at exit) say the exit dump holds one
  offset-0 sample per subcol summing from T_nom; statically
  `QLSpectra.c:127-181` could restart accumulation, which would make
  `--ssid32-inject` (exactly one product) fail. The live gate is the
  evidence; a failure there is investigated against that code, not
  re-timed blindly.
- m9: the kind-name table is `stix_det.c:2731-2736` (already reports
  `single`); `tapes.py` must refuse adc+ebin both present (today it
  ignores the second); `spec` on single is refused before `_spec`;
  `det.tape.dropped` goes into `state.py`'s VOLATILE set so the live
  readout does not lag.
