# Detector stimulus: per-detector/per-pixel rates, the mix tape, ELUT alignment; spectra and single events staged — plan (2026-11-15)

> For agentic workers: implement task by task; steps use `- [ ]`.
> Workers cannot build or run anything: write code + self-test cases,
> the reviewer runs them and commits. S1, the alignment track and the
> ADC-faithful sky mode (stage C) are written in full; S2/S3 are
> outlined with their wire interfaces fixed (the keV ↔ adc law is the
> stage-C calibration, decisions D1/D5).

**Goal:** the Detectors → Stimulus board gets per-detector and per-pixel
rate stimulus driven by JSON "mix tapes" played on guest time, and —
opt-in, first-class — ELUT alignment between the flight software's
loaded ELUT and the detector simulation, so ADC-domain counts land in
the intended science bins whatever ELUT the FSW loaded. An optional
ADC-faithful sky mode (`det-adc-mode`, default off, stage C) draws the
synthetic sky and the ROTB chunk fill through the loaded ELUT, so a
misaligned ELUT miscounts the sky exactly as on hardware.

**Architecture:** the tape engine and the per-(fpga_det, pix) rate state
live in `stix_det.c` on the existing 100 ms tick (absolute virtual-clock
deadlines, the D-Dj store-then-apply rule), so steps are guest-time
exact and icount/pace-fast safe; the platform controller owns tape
files, verbs and markers (`stixplat/tapes.py`, verbs `det.tape.*`, a new
`stim` record kind). Alignment is three pieces: (b) a derive tool that
writes an ELUT CSV from the sim's per-detector baseline law for the
existing `elut-upload.py` chain, (c) an alignment classification + report
in the model exposed through hw-state and a gate, and (a) a per-step
`align` option that converts science-space (ebin/keV) stimuli to ADC
through the preimage of the loaded ELUT (consumed by S2/S3); stage C
folds the drawn sky through the loaded ELUT with a cached per-row
bin-mass map (default off).

**Spec/inputs (owner decisions, 2026-11-15, verbatim):**

- "All of it, staged": Stage S1 = per-detector and per-pixel rates + the
  mix tape (JSON steps on guest time, a background layer under the
  steps, each step start a Timeline marker, tapes saved next to the
  saved views); S2 = spectra (line at ADC, flat, from file); S3 = single
  events as in the DSS tool (det(22, 0, 1000) x N, wait 20 us). Each
  stage gets its own gate.
- "Concerning the detector simulation, what would be good is if there
  was a way that you could support alignment. It doesn't have to be
  always, but that if I wanted to, that you could support alignment
  between the ELUT in the flight software and also the detector
  simulation. Because right now, as I've seen in the test I did for the
  rotating fill level, if the flight software doesn't have the properly
  configured ELUT that matches the DSS, I don't count properly."

Second pass (2026-11-15, recorded as decisions D1–D7 in "Owner
decisions"): the ADC-faithful sky is approved as an optional mode
(default off; the synthetic sky and the ROTB chunk path draw ADC
values through a per-pixel simulation calibration, then bin through
the loaded FPGA ELUT); DG1–DG4 are dropped from the board.

Other inputs: the mockup boards (scratchpad copies outside the repo:
`/tmp/claude-1000/-home-laszlo-dev-qemu/4272697f-b4b3-4e8f-ab5f-
3dbd52a58b2f/scratchpad/stix-mock/project/Stimulus.dc.html` and
`Detectors.dc.html`; the details the board needs are inlined here).
Stimulus page: header readout "now playing: tape 'flare on Q1+Q3' ·
step 2 of 4"; §1 Target chips All 32 / Q1–Q4 / DG1–DG4 / "Pick on
map…" with "Selected: 16 detectors (Q1 + Q3, DSS numbering)", pixel
select "all 12 / large only (0–7) / small only (8–11) / one pixel…";
§2 segmented control Rate / Single events / DSS sequence, inputs
"cps per detector" (20000), "Spectrum" ("line at ADC…" / "flat" /
"from file…"), "Duration" ("120 s"), the mono hint "single events, as
in the DSS tool: det(22, 0, 1000) × N, wait 20 us", buttons Send now /
Add to tape; §3 Mix tape: saved-tapes select ("flare on Q1+Q3",
"quiet day", "ROTB stress 0.1 s"), Save as…, Loop, Play, Stop; the
tape timeline's background row ("all 32 · 2,000 cps · whole tape") and
step row (widths ∝ for_s: "1 quiet" 60 s, 2,000 cps flat, all 32 →
"2 flare Q1+Q3" 120 s, 20,000 cps · line ADC 1000, playing → "3
replay" 30 s, DSS sequence, "Xrate-1.dssevs" → "4 off" 30 s, 0 cps),
progress "02:05 / 04:00", the step table (# / Kind / Target / Settings
/ For, move up/down/remove), the footnote "Steps run one after
another; the background layer runs under all of them. Every step start
is a marker on the Timeline. A tape is a JSON file next to the saved
views", and the blue box "Emulator support today vs needed": "Today:
one quiet and one flare rate for all detectors, and DSS sequence
replay. Needed for this page: per-detector and per-pixel rates,
spectra, and single events — detector-model work, planned
separately." Detectors page: the Status/Stimulus sub-view toggle, the
numbering toggle DSS / FSW / sub-collimator over the map
(`Detectors.dc.html:114-118`; DSS = fpga_det, FSW/sub-collimator =
subcol), the scene panel (Source synthetic/zero/replay-dss, quiet/
flare cps, Flare on, Apply / Apply at…), the DSS replay panel (file
select, Start/Stop, "idle · events 0 · dropped 0 · unsupported 0"),
the detector map whose pair tiles repeat a per-quarter group digit 1-4
(detector 22's side panel reads "Detector group DG3" — mockup-only
labels; the board drops DG, decision D2), and the pixel panel "pixel /
ASIC channel (DSS numbering)" with "0/26"-style labels. Docs:
`2026-11-11-detector-plan.md` (index spaces, D-Df/D-Dk),
`2026-11-11-elut-plan.md` (E-1/E-2/E-4), `accepted-diffs.md` §34–§38,
§40, `platform.md` (state tree, verbs, timeline),
`ground-station.md` (Detector scene + ELUT sections),
`2026-11-14-rotb-fill-plan.md` Amendment C (icount 7 calibration),
`2026-11-14-timeline-plan.md` (house style, sampler/series/views).

## Key facts

Every claim below was read in this worktree unless marked UNVERIFIED.

**The model today.**

- Scene knobs are runtime class properties on `/machine/stixctrl` added
  by `stix_det_class_init` (`hw/sparc/stix_det.c:2470-2568`); qdev
  properties `det-model/det-idefx/det-seed/det-rotb` at
  `hw/sparc/grlib_stixctrl.c:1783-1792`. Setters store fields only and
  the next 100 ms tick applies them (D-Dj).
- The tick runs only while FSM Enable = 1, engages the controller's
  `mem_reentrancy_guard` for its whole body, and calls sky, replay and
  inject in that order (`stix_det.c:230-259`).
- The synthetic sky accumulates exposure per (fpga_det, pix, profile)
  (`stix_det.h:197`) and is generated **directly in ebin space with no
  ELUT** — a deliberate recorded deviation
  (`accepted-diffs.md:3119-3124`). The per-tick rate is one global
  `quiet_cps`/`flare_cps` per fpga_det with fixed `pix_share` weights
  (`stix_det.c:1153-1201`; template §34, `accepted-diffs.md:3106-3112`).
- The event sink `stix_det_add_counts(fpga_det, pix, ebin, n)`
  (`stix_det.c:1222-1252`) honours the per-tick pix gate
  (`stix_det_pix_gate`, `stix_det.c:1112-1126`: grp reads out, R3
  threshold < 63), SUMDMASK (EACC), SUMCNT's SUMDMASK×SUMEMASK share
  and LVAR's masks; deposits land in LACC at the next QL swap and in
  EACC — the next ROTB chunk — at the next nominal swap.
- **ROTB science chunks (tier 3):** the chunk's EACC is materialized at
  the nominal swap by `stix_det_draw_acc` from `nom_expo` plus the
  inject deposits (`stix_det.c:1627-1661`; §35,
  `accepted-diffs.md:3338-3426`) — i.e. the same ebin-space mix as the
  sky; replay events and det-inject reach it through `add_counts`'
  `inj_nom` (`stix_det.c:1236-1244`). There is no ADC content in any
  accumulator: alignment can only change *which ebin* a source lands
  in, never the chunk layout.
- **DSS replay:** `.dssevs` pre-scanned at realize into
  `{t_tick, fpga_det, pix, adc}`; `fpga_det = dss_det` (identity); each
  event's ebin is looked up in the FPGA ELUT `val[pix][fpga_det][adc]`
  at 0xE0000000 (`stix_det.c:1298-1304, 1434-1441`), with the
  **degenerate-row linear fallback**
  `ebin = (adc − base)·32/(4096 − base)` (base = the file's 0xE
  baseline per dss_det) traced as `stixdet_elut_fallback`
  (`stix_det.c:1443-1489`), classified at each SetEnable 0→1 edge and
  each QL swap **only while `det-source=replay-dss`**
  (`stix_det.c:1556-1608`, edge hook at `:304-305`). §37,
  `accepted-diffs.md:3845-4038`; §40 E-4 addendum
  `accepted-diffs.md:3616-3637`.
- hw-state: `stixctrl_get_hw_state` builds the JSON (`schema` 5,
  `grlib_stixctrl.c:1593-1739`) and calls `stix_det_hw_state(&s->det,
  det)` (`grlib_stixctrl.c:1708`); the det section carries
  source/flare/cps/ql_bank/fsm/replay (`stix_det.c:2652-2680`).
- Replay knobs precedent: `det-replay-*` properties
  (`stix_det.c:2508-2544`); `det-replay-file` is a qdev
  `DEFINE_PROP_STRING` (`grlib_stixctrl.c:1798`), scanned at realize
  (`stix_det.c:1316-1427`) — a runtime tape cannot load a new replay
  file.
- `det-rotb-burst` (the fill-level knob the owner used) is the
  write-only QOM + read-only status-string pair
  (`stix_det.c:2545-2567`); the platform verb parses the status line
  (`scripts/stix/platform/stixplat/plat.py:285-306`).

**ELUT tooling.**

- `scripts/stix/elut-upload.py`: `encode` (96 × TC(237,7) + TC(237,8)),
  `check-dump` (loop replay + independent oracle + D-Eg sensitivity
  self-check that exits 2 when the CSV cannot detect a mapping bug),
  `self-test`; the FSW-sourced mapping table `SUBCOL_FROM_FPGA_DET`
  (`elut-upload.py:63-68`), inverse `FPGA_DET_FROM_SUBCOL` (:72-77);
  dump layout `val[pix][fpga_det][adc]` = 1 572 864 B (:51). B0 = 0,
  B32 = 0 fixed (D-Eb, §40). `SUBCOL_FROM_FPGA_DET[22] = 19`,
  `[5] = 10`, fixed points {0, 1, 4}.
- elut-smoke's run-2 replay leg (E-4) already proves
  honoured-vs-fallback end to end with a gate-written synthetic
  `.dssevs` and `elut-replay-check.py` (§40,
  `accepted-diffs.md:3616-3637`) — the pattern A3 reuses.

**Platform controller.**

- Verb registry `VERBS` frozenset (`stixplat/verbs.py:6-16`), handlers
  registered with `d.add(...)` in `plat.py:185-237`; API v1 is
  additive-only (`platform.md` "Versioning").
- Record kinds `KINDS` (`stixplat/log.py:26-27`), topic map `_TOPIC`
  and `TOPICS` (`log.py:32-36`); timeline marker kinds
  `MARKERS = ("fault","force","tc","event","mode","power","scenario")`
  (`stixplat/series.py:35`).
- Sampler default paths include `instrument.detector.*cps` and `.flare`
  (`stixplat/config.py:85-90`); `views_dir` default "" =
  `<retention.run_root>/views` (`config.py:94`, `stixplat/views.py:31-33`)
  — "next to the saved views" = a sibling `tapes` dir.
- State-tree leaves are built from the hw-state JSON in
  `stixplat/state.py:43-140` (det section at :92-106); volatile leaves
  listed at :19-31. Leaf shape `{truth, presented, reported, unit,
  index_space, t_sim, t_changed, status, fault_ids}` (`state.py:34-40`).
- Self-test: `check(cond, msg)` (`stixplat/selftest.py:61`), run with
  `python3 stixplat.py --self-test`; page rules (every `data-verb`
  served, no innerHTML/inline handlers/styles) already enforced.

**Index spaces** (master table: `ground-station.md` "Detector scene";
ELUT subset `ground-station.md:741-748`): `fpga_det` 0-31 (= `dss_det`,
identity), `subcol` 0-31 (ICD number − 1, the TM index), `pix` 0-11,
`ebin` 0-31 (32 = unassigned), `adc` 0-4095, `grp` = fpga_det/2,
`quarter` = grp/4. Q1+Q3 = grp 0-3, 8-11 = fpga_det 0-7, 16-23 = mask
0x00FF00FF (16 detectors, the mockup's example). The UI numbering
toggle (DSS / FSW / sub-collimator, `Detectors.dc.html:114-118`) maps
DSS = fpga_det, FSW/sub-collimator = subcol. DG1–DG4: the mockup's
front-end group chips have no FSW concept behind them (the only 4×8
grouping is the quarters) — the board drops them (decision D2).

**Performance context.** icount 7 + the AX-like CPU is the calibrated
bench (Amendment C.6, `2026-11-14-rotb-fill-plan.md:569-596`); at
`pace = fast` the controller's timing granularity is ~1 s of guest time
(`platform.md` "Fast-forward") — the reason the tape engine must not be
controller-scheduled.

## Global constraints

- No FSW edits; `/home/laszlo/dev/STIX-FSW` read-only.
- Every new property, tape field, trace argument, state leaf and test
  name carries its index space (`fpga_det`, `subcol`, `pix`, `ebin`,
  `adc`); a bare "det"/"pixel" is not allowed (detector plan rule).
- API v1 additive only: new verbs, kinds, topics, fields; no rename.
- UI rules: one file `ui/index.html`, no innerHTML, no inline
  handlers/styles, verbs via `data-verb`, every verb served (uicheck).
- The tape engine runs inside the existing tick under the
  `mem_reentrancy_guard` (`stix_det.c:233-238`); QOM setters store
  fields only; no MMIO dispatch from timer or QMP context.
- Guest-time scheduling only: no wall clock in model or gate timing
  (the §41 monotonic-stamp rule for gates).
- The synthetic sky stays an ebin-space source by default (§34
  deviation stands); alignment never changes its default path, only
  opt-in per tape. The ADC-faithful sky (stage C, decision D1) is
  opt-in and default off: no existing gate or golden changes.
- Tapes and derived CSVs live in run dirs / the tapes dir; the user's
  `.dssevs` is never copied into the repo; CSVs are referenced by path
  (D-Ed).
- Goldens unchanged: every new property defaults off/empty; nothing new
  happens at boot without `-global`/QMP.
- Self-test stays PASS: `cd scripts/stix/platform && python3
  stixplat.py --self-test`; every task adds `check(...)` cases.

## Approaches considered

1. **Where the tape engine lives.** (i) Controller-side: schedule
   `qom-set`s of the existing scene knobs at bench-time deadlines like
   the keep-alive. Rejected: at `pace = fast` (~20x) one 20 Hz poll is
   ~1 s of guest time (`platform.md` "Fast-forward"), so 0.1 s steps
   (the mockup's "ROTB stress 0.1 s" preset) are impossible, per-pixel
   rates are not expressible with today's knobs, and playback would not
   be icount-deterministic. (ii) Model-side on the 100 ms tick
   (recommended): absolute vclock deadlines, exactly D-Dd's tick rules;
   the platform only loads/plays/stops. Chosen.
2. **How a step targets detectors.** (i) A full 384-entry per-pair rate
   table per step: exact but makes tapes huge and the UI unusable.
   (ii) `fpga_det_mask` + `pix_mask` + one cps per step, with the
   background layer filling unmasked pairs (recommended; the UI
   translates Q1-Q4/pixel picks into masks). Chosen.
3. **Which alignment pieces.** (a) only — model converts science-space
   stimuli through the inverse loaded ELUT: helps only *specified*
   stimuli, does nothing for the replay, and silently mis-bins if used
   alone with a wrong ELUT. (b) only — fix the FSW's ELUT: right fix for
   the replay, but nothing tells the user *when* it is wrong. (c) only —
   report without a remedy. **Recommendation: (b) + (c) always available
   (tools + report, works with today's replay), plus (a) as the
   per-step/tape `align` option consumed by S2/S3 ADC-domain stimuli.**
   Knobs: model `det-align` (bool scene knob, default off) +
   `det-align-status` (read-only str) + hw-state `det.align`; tape
   fields `"align"` (tape-level default) and per-step science-space
   `spec`/`ebin`; platform verb `det.align.set {enabled}`; gate
   `align-smoke.sh`. The ebin-space sky stays as recorded in §34.
4. **How the ADC-faithful sky draws (decision D1, stage C).** (i) A
   per-event adc draw in the tick: rejected — the tick's cost is per
   (fpga_det, pix, profile) exposure, and 20000 cps over 384 pairs is
   ~2 M draws per guest second, with a changed RNG stream. (ii) A
   cached per-row bin-mass map: the tick keeps its ebin-space exposure
   and per-ebin Poisson draws; at the draw each ebin's mass folds
   through the loaded ELUT row by a precomputed 32×32 fraction table,
   rebuilt at each classification point while the mode is on (ELUT
   writes are undetectable — no write hook — so "once per change" is
   not implementable; one full 384 × 4096 pass, host ~ms),
   integer largest-remainder splits, no new RNG. Chosen. (iii)
   Poisson per destination ebin at the swap: rejected — it doubles the
   draws and loses the mode-off = mode-on total invariant the qtests
   pin.

## Review focus (each pinned to a task's test)

1. **Step timing is tick-quantized and guest-time.** A step boundary
   lands on a 100 ms tick; the tape does not advance while FSM Enable =
   0 (SAFE freezes it) or while paused. Pinned: `det-tape-rate`
   (ClearEnable mid-tape → no step progress; resume continues).
2. **Background vs step precedence, and `off`.** A step wins on its
   masks; an `off` step (all 32) must zero even the background layer.
   Pinned: `det-tape-rate` bank 3 all-zero assertion.
3. **Index-space mistakes.** The tape masks are `fpga_det`-indexed; the
   TM shows `subcol`. Pinned: the `--tape` gate's SSID 32 check (elevated
   exactly at `SUBCOL_FROM_FPGA_DET[f]` for the masked f, the
   background floor elsewhere) and
   `tapes_numbering` (the page's DSS/FSW table equals
   `elut-upload.py`'s).
4. **Alignment vacuity.** A fallback=0 report is only proof if the
   classification actually ran and the counts really crossed the ELUT.
   Pinned: `align-smoke.sh` leg 2 asserts `stixdet_align` present,
   `stixdet_elut_fallback` absent, check-dump PASS, **and** the SSID 32
   ebin — three independent witnesses (the E-4 lesson, §40).
5. **check-dump's sensitivity trap on derived CSVs.** Rows identical
   across subcol make D-Eg exit 2 ("cannot detect a mapping bug"). The
   gate's synthetic file therefore uses distinct per-detector baselines,
   and `elut-derive.py --self-test` pins the predicted differing-row
   counts. Real uniform-baseline files are checked with the new
   explicit `check-dump --skip-sensitivity` (decision D7). Pinned:
   `test-elut-derive.sh` case 4.
6. **ADC mode default-off is load-bearing.** §34 and every gate assume
   the sky bypasses the ELUT; the mode must add no RNG draws and no
   folds when off. Pinned: `det-adc-off` (the `det-counts-seed`
   stimulus, same bank values; knob changes do not perturb it).
7. **The bin-mass map tracks the ELUT SRAM.** A stale map after a
   TC(237,8) apply would silently misbin — the bug class the mode
   exists to expose. Pinned: `det-adc-map-invalidate` (row rewritten
   mid-run → the bank closed by the first swap after it moves; the
   rebuild runs at the start of `ql_swap`, before `materialize`) and
   C3's aligned leg (mismatch → 0 after derive/upload).
8. **The map build stays in established contexts.** Guest-memory reads
   via `memory_region_get_ram_ptr` only where the classification
   already reads (SetEnable edge, QL swap, the replay tick's
   `replay_class_pending` case — raised by the det-source setter,
   not by a TC(237,8) apply) under the tick's `mem_reentrancy_guard`;
   integer splits and the existing PRNG only, so icount and
   wall-clock behaviour are unchanged. Pinned: `det-adc-guard` (the
   `det-tape-guard` pattern with the mode on) and C3's twice-run
   aligned leg (byte-identical SSID 32 dumps).

---

## Stage S1 — rates and the mix tape

### Task T1: tape state, parser, properties (model)

**Files:** modify `hw/sparc/stix_det.c`, `hw/sparc/stix_det.h`,
`hw/sparc/trace-events` (add `stixdet_tape_load`, `stixdet_tape_step`),
`tests/qtest/stix.c`.

**Interfaces — produces:**
- `det-tape-load` (write-only str, JSON) on `/machine/stixctrl`, added
  in `stix_det_class_init` like the scene knobs. Tape JSON:
  `{"api": 1, "name": <str 1..64>, "align": <bool, default false>,
    "adc_mode": <bool, optional, default = the det-adc-mode knob>,
    "loop": <bool, default false>,
    "background": <rate layer, no for_s, unmasked>,
    "steps": [<step>, ...]}` with ≤ 64 steps, ≤ 64 KiB total.
  Step: `{"kind": "rate"|"replay"|"off"|"single", "for_s": <number
  0.1..86400>, "label": <str ≤ 64, optional>,
  "fpga_det_mask": <u32, required for rate/off>,
  "pix_mask": <0..0xFFF, default 0xFFF>,
  "cps": <0..1000000, rate>, "spec": <"flat" in S1>,
  "file"/"offset_ms" (replay: must match the realized
  det-replay-file), "n"/"wait_us"/"adc"/"ebin" (single, S3)}`.
  Kinds `single` and spec values other than `"flat"` parse (the shape
  is fixed now) but are **rejected at load with "unsupported in this
  build (S2/S3)"** so S1 never half-implements them; so is
  `"adc_mode": true` ("unsupported in this build (ADC sky is stage
  C)") until C1 lands. Every error names
  the step index. Masks are `fpga_det`/`pix` indexed; `subcol`-keyed
  steps are refused (`"subcol_mask" is not a tape field; use
  fpga_det_mask (dss_det = fpga_det)`). The background is unmasked by
  definition — it fills all 384 (fpga_det, pix) pairs outside the
  active step's masks; `fpga_det_mask`/`pix_mask` on the background
  are refused.
- `det-tape-play` (write-only bool): true starts at step 0 on the next
  tick (store-then-apply, D-Dj); false stops and reverts the rates to
  the scene knobs at the next tick, and so does `done` (the last step
  ended without loop). Refused (error) while playing
  already / not loaded.
- `det-tape-status` (read-only str, one line, atomic):
  `state=idle|ready|playing|done|stopped,name=...,step=K/N,
  label=...,step_left_ticks=...,loop=...,align=...`.
- Tape state in `StixDetState` (`stix_det.h`): loaded steps (owned
  copy), play request, current step index, tick countdown. A device
  reset stops playback and keeps the tape (state `ready`), like the
  scene; a new `det-tape-load` while playing stops first.
- hw-state `det.tape`: `{state, name, step, n_steps, label, kind,
  loop, align}` next to the scene keys in `stix_det_hw_state`
  (`stix_det.c:2652-2674`) — T4 maps it to leaves and the `stim`
  record needs `kind`.
- Semantics: the step clock advances one tick per tick **only while
  FSM Enable = 1** (the tick's own gate); at a step start the model
  traces `stixdet_tape_step(index, kind, label, vclock_ns)` and applies
  the step (T2); `replay` steps start/stop the D-7 engine at step
  start/end (source switches to/from replay-dss for the step's
  duration; refused at load when no table was scanned at realize, or
  unless `det-replay-trigger=qmp` — the default nominal trigger starts
  the replay at the SetEnable edge regardless of the tape,
  `stix_det.c:1546-1549`; replay steps are model-only in S1, no UI
  control yet — the board's DSS-sequence button is wired in S3/V1);
  `loop=true` restarts at step 0 after the last step.

- [ ] qtest `det-tape-props`: every rejection above (bad JSON, > 64
  steps, > 64 KiB, `for_s` 0, unknown kind, `pix_mask` > 0xFFF,
  `cps` > 1000000, `spec` `{"line_adc": 1000}` in S1, `adc_mode` true
  in S1, `subcol_mask`,
  a masked background, play without a tape, replay step with no
  realized table or without trigger=qmp); a rejected
  load leaves the previous tape intact (`det-tape-status` unchanged).
- [ ] qtest `det-tape-status`: idle → ready → playing (step=1/N) →
  done; `loop=true` wraps; play=false mid-step → stopped; a device
  reset → ready with the tape kept; the status line parses with the
  `plat.py` key=value splitter.
- [ ] qtest `det-tape-replay`: a tape `[rate 1 s, replay for_s 2
  offset_ms 0, off 1 s]` over a synthetic `.dssevs` (the
  `det-replay-decode` generator), with `det-replay-trigger=qmp` set
  first (a nominal trigger starts the replay at NOMINAL entry, before
  the tape reaches the step): during the replay step
  `det-replay-status` reads `running` and the LACC bank gets the
  events; before/after it reads `stopped`/`armed` and no replay counts
  land; the step end stops the replay.
- [ ] qtest `det-tape-guard`: with steps changing under `clock_step`,
  every CTRL MMIO path still answers (the `det-chunk-guard` pattern).

**Verification:** `scripts/stix/qt.sh all` — e.g. 207 + 4 of 207 + 4 ok
(the suite is 207 qtest cases today; the exact count grows by the four
new tests); each new test RED on the
parent binary (unknown property `det-tape-load`).

### Task T2: per-pair rate application (model)

**Files:** modify `hw/sparc/stix_det.c` (sky tick + step application),
`hw/sparc/stix_det.h`, `tests/qtest/stix.c`.

**Interfaces — produces:**
- While a tape plays, the sky tick's per-pair rate for (fpga_det, pix)
  is: the active step's `cps` if the pair is inside the step's
  `fpga_det_mask`/`pix_mask`, else the background's `cps` (the
  background is unmasked, T1); `kind off`
  contributes 0 on its masks (an all-32 off step silences the
  background too). The rate still flows through the existing exposure +
  template (`pix_share`, `ebin_share`) and the per-tick pix gate — a
  step rate on a gated-off pix produces nothing, like a real event.
- Rate steps require `det-source=synthetic` at play (the play setter
  refuses otherwise, naming the source); the scene's `det-flare` and
  the two `*-cps` knobs are ignored while playing and resume on stop
  or done (T1).
- The nominal integration / SUMCNT / LVAR read the same per-pair rate
  (one code path: the rate lookup feeds `stix_det_sky_tick`'s existing
  accumulators).

- [ ] qtest `det-tape-rate`: QlIntTime 9 (1 s periods),
  `det-quiet-cps=0`, background cps 100 (unmasked), steps
  `[{rate, for_s 2, fpga_det_mask 0x00FF00FF (Q1+Q3), cps 20000}, {off,
  for_s 1}]`. `det-tape-play` is issued on the tick right after a QL
  swap (the hw-state bank flip marks it), so the for_s 2 step covers
  exactly two whole periods, the `off` swap closes a pure step-1 bank,
  and every assertion reads the bank the swap just closed — a play
  landing mid-period would mix background into the first closed bank
  (≈ 18000, tens of σ off). After the two step-1 swaps: the LACC sum
  over pix×ebin of
  fpga_det 0 is 20000 ± 5σ (σ≈141), fpga_det 16 likewise, fpga_det 2 is
  100 ± 5σ (background); after the step-2 swap every one of the 384
  cells is exactly 0 (off beats the background); `det-tape-play=false`
  then gives an all-zero bank (revert to quiet-cps 0). ClearEnable
  mid-step freezes the step countdown (`det-tape-status`
  `step_left_ticks` unchanged across a paused second).
- [ ] qtest `det-tape-pixmask`: same shape with background cps 0 (so
  "zero" is exact — a filled background would put its counts in the
  large pix too) and step `pix_mask 0xF00`
  (small pix 8-11 only): large-pix cells exactly 0, small-pix cells
  ≈ 0.22/8.88 of the rate (the template shares still apply).
- [ ] qtest `det-tape-seed`: the same seed and tape give identical
  banks (`det-counts-seed` pattern; icount determinism in guest time).

**Verification:** `scripts/stix/qt.sh all`; `--tape` gate (T6) for the
FSW-visible end.

### Task T3: platform tapes — files, verbs, API

**Files:** create `scripts/stix/platform/stixplat/tapes.py`; modify
`stixplat/verbs.py` (+`det.tape.save|delete|play|stop`,
`det.align.set`),
`stixplat/plat.py` (handlers, `d.add` entries), `stixplat/api.py`
(`GET /api/v1/tapes`, `GET /api/v1/tapes/<name>`), `stixplat/config.py`
(`[timeline] tapes_dir`, default "" = `<retention.run_root>/tapes`),
`stixplat/selftest.py`, `docs/stix-port/platform.md`.

**Interfaces — produces:**
- Tape file JSON = the model's schema (T1) and is validated
  client-side first (kinds, ranges, masks, ≤ 64 steps) so a bad tape is
  a 400 before any QMP; the model remains the authority.
- `det.tape.save {name, tape}` → `<tapes_dir>/<name>.json` (tmp +
  os.replace); name rules = views' `NAME_RE` (1..64 of A-Za-z0-9._-,
  no leading dot) checked before any file access (`views.py:24,36-41`
  pattern). `det.tape.delete {name}` (404 unknown). `GET /api/v1/tapes`
  → `{"tapes": [names], "dir": d}`; `GET /api/v1/tapes/<name>`.
- `det.tape.play {name, loop}`: read the file, `qom-set
  /machine/stixctrl det-tape-load <json>` then `det-tape-play true`
  (`loop` is in the JSON; the verb argument overrides); returns the
  parsed `det-tape-status` dict; 409 while STIX is off or QMP errors
  (the `v_det_rotb_burst` pattern, `plat.py:285-306`).
  `det.tape.stop {}`: `det-tape-play false` + parsed status.
- `det.align.set {enabled}` (A1's verb, registered here): `qom-set
  det-align` + parsed `det-align-status`.

- [ ] Self-test `tapes_crud`: save/list/read/delete round trip;
  `../x`, ``, 65 chars, `.hidden` → 400 before any file access; an
  invalid tape (unknown kind, `for_s` 0) → 400 with the step index.
- [ ] Self-test `tapes_play_stop` (the fake-QMP verb pattern): play
  issues load-then-play in order and returns the parsed status keys
  (`state`, `step`, `name`); stop → `det-tape-play false`; 409 while
  off.
- [ ] Self-test `tapes_numbering`: the DSS/FSW numbering table the UI
  uses equals `elut-upload.py`'s `SUBCOL_FROM_FPGA_DET` (import; the
  one FSW-sourced table per language, pinned).

**Verification:** `cd scripts/stix/platform && python3 stixplat.py
--self-test` → ends `SELF-TEST: PASS`.

### Task T4: markers, sampler paths, state leaves

**Files:** modify `stixplat/log.py` (`KINDS += "stim"`, `TOPICS +=
"stim"`), `stixplat/series.py` (`MARKERS += "stim"`), `stixplat/state.py`
(map hw-state `det.tape` → `instrument.detector.tape.{state,name,step,
n_steps,label,kind,loop}` and `det.align` → `instrument.detector.align.*`),
`stixplat/plat.py` (log one `stim` record when
`instrument.detector.tape.step` changes, and on play/stop verbs),
`stixplat/config.py` (sample_paths += `"instrument.detector.tape.*"`,
`"instrument.detector.align.*"`), `stixplat/selftest.py`.

**Interfaces — produces:**
- `stim` record: `{"kind": "stim", "path":
  "instrument.detector.tape.step", "data": {"name", "step", "n_steps",
  "label", "kind"}}`; series marker kind `stim` with hover label
  `tape '<name>' step <k>/<N>: <label>` (the mockup's "Every step start
  is a marker on the Timeline").
- The UI's SSE listener list gains `stim` (`ui/index.html` topic list,
  Timeline page).

- [ ] Self-test `tape_markers`: feed a poll where `det.tape.step`
  changes 1 → 2 → exactly one `stim` record, `check_record` passes, the
  series catalogue lists marker kind `stim`; no record when the step
  holds.
- [ ] Self-test `sampler_paths`: the two new globs match the new leaves
  and nothing else new.

**Verification:** `python3 stixplat.py --self-test` → PASS.

### Task T5: the Stimulus board (UI)

**Files:** modify `scripts/stix/platform/ui/index.html`,
`stixplat/selftest.py` (page rules case), `stixplat/uicheck.py` if the
verb list needs the new verbs.

- [ ] Detectors tab gains the Status/Stimulus sub-view (mockup
  `Stimulus.dc.html`): Target chips (All 32, Q1-Q4, pick-on-map rows
  from the existing detector map; DG1-DG4 are not rendered at all —
  dropped, decision D2), pixel
  select ("all 12 / large only (0–7) / small only (8–11) / one
  pixel…"), "cps per detector" + duration inputs, Send now /
  Add to tape (S1: rate steps only; the Spectrum, Single-events and
  DSS-sequence controls render disabled with the mockup's "Emulator
  support today" wording — the DSS-sequence button is wired in S3,
  V1), and the Mix tape panel: saved-tapes select (GET
  /api/v1/tapes), Save as… (`det.tape.save`), Loop checkbox, Play/Stop
  (`det.tape.play`/`det.tape.stop`), the background row, the step row
  (widths ∝ `for_s`), the progress readout from
  `instrument.detector.tape.*`, the step table with move up/down/
  remove (client-side re-order, saved back with `det.tape.save`), and
  the alignment box (A1): align checkbox (`det.align.set`) + the
  `instrument.detector.align.*` readout.
- [ ] Self-test case "UI: stimulus board": the tape select, loop
  checkbox, play/stop buttons (`data-verb det.tape.play/stop`), the
  step table container and the align toggle exist; every `data-verb`
  served; no innerHTML/inline handlers/styles (existing suite).

**Verification:** `python3 stixplat.py --self-test` → PASS; reviewer's
browser pass per platform.md's manual checklist.

### Task T6: S1 gate

**Files:** modify `scripts/stix/detector-smoke.sh` (new `--tape` mode),
`scripts/stix/ql-check.py` (`--ssid32-subcols` check), create
`scripts/stix/platform/scenarios/gate-det-tape.json`.

- [ ] `--tape`: synthetic zero-rate base — `det-source=synthetic`
  with `det-quiet-cps=0` (the play setter refuses any other source,
  T2, so the D-3 `det-source=zero` base cannot be used; quiet-cps 0
  keeps the zero-sky contract: the only sky is the tape's),
  QlIntTime default; @75 QMP loads the gate's
  tape (background 20 cps on all 32; steps: `rate` Q1+Q3 2000 cps for
  60 s → `off` 30 s), `det-tape-play true` (play lands inside the
  T_nom ± 10 s window, so the rate step covers the first 8 QL periods
  that SSID 32 sums); @215 CONF as usual. The base flush check is a
  new `ql-check.py --expect-tape-flush (T_exit − T_nom)`: the D-3
  product set, cadences and header defaults, content asserted
  non-zero only — `--expect-zero-flush` would fail by design (the
  20 cps background runs from play until the `off` step) and
  `--expect-sky-flush`'s line-bin and ebin asserts are the flare
  template's, not the tape's. New check `tape`: `ql-check.py
  --ssid32-subcols 0,1,5,6,4,10,11,12,30,31,25,26,21,27,19,20` — in the
  one SSID 32 dump, every masked `subcol` (`SUBCOL_FROM_FPGA_DET[f]`
  for the masked `f`) has byte 4 (ebin 3, a flat-share ebin of the
  quiet template, 0.10/11) ≥ 0x30 (2000 cps × 32 s ≈ 580 counts), and
  every unmasked subcol stays at the background floor, byte 4 < 0x10
  (20 cps × 32 s ≈ 6 counts, the ql-inject bound — not zero: the
  background fills all 32). Plus the SSID 30 rule, part of the same
  `tape` check (one verdict — pinned here because
  `check_lc_contrast`'s quiet-before-switch window does not exist
  (play ≈ T_nom leaves no samples before the switch)):
  the rate step's band-0 median ≥ 8 compressed steps above the `off`
  step's band-0 median, and the `off`-window samples are exactly 0
  (the `off` step beats the background end to end through the FSW).
  `--tape --negative` plays only the `off` step and
  fails exactly `tape` (no elevated subcol, and the SSID 30 rule
  fails too with no rate step — which is why it lives inside `tape`).
  Expected line
  `DETECTOR-SMOKE (tape): PASS`; SKIP 77 without the ELF (house
  convention).
- [ ] `gate-det-tape.json` (platform scenario, run mode): power on/boot,
  `tc.gs.send` TC(236,1) 05 00 with `await_tm` `0x021:4:5` (NOMINAL —
  the tick, and so the tape, only runs while FSM Enable = 1
  (`stix_det.c:230-259`), and scenarios boot to SAFE:
  `gate-spw-link.json` never enters NOMINAL), `det.tape.save`,
  `det.tape.play`, `await_state`
  `instrument.detector.tape.step` eq 2 within 70 s, `det.tape.stop`,
  `await_state` `.tape.state` eq `stopped`; a `stim` marker in the log
  (the scenario's `expect` records are the verdict).

**Verification:** `STIX_PORT_BASE=auto scripts/stix/detector-smoke.sh
build/qemu-system-sparc --tape` → `DETECTOR-SMOKE (tape): PASS`; the
scenario via run mode (`scripts/stix/platform/tests/run-smoke.sh`
extended) → pass.

## Alignment track (the owner's requirement)

### Task A1: `det-align` + classification report (model)

**Files:** modify `hw/sparc/stix_det.c`, `hw/sparc/stix_det.h`,
`hw/sparc/trace-events` (add `stixdet_align`), `tests/qtest/stix.c`.

**Interfaces — produces:**
- `det-align` (bool scene knob on `/machine/stixctrl`, default false,
  survives a device reset — it describes the bench intent, like
  `det-source`).
- The ELUT row classification (today only while `det-source=replay-dss`,
  `stix_det.c:1556-1571`) runs at the SetEnable edge and each QL swap
  whenever `det-align` is on, `det-adc-mode` is on (C1), or the source
  is replay-dss; it emits
  one summary trace `stixdet_align(rows, fallback, honoured,
  mismatch_cells, vclock_ns)` per classification (closes the E-4
  vacuity gap: the classification itself becomes visible).
- `det-align-status` (read-only str):
  `mode=off|on,rows=384,fallback=F,honoured=H,mismatch_cells=M,
  unassigned_cells=U,calibration=none|dss-linear`.
  - `mismatch_cells`: over honoured (non-degenerate) rows only, the
    count of adc whose honoured ebin differs from the sim calibration's
    linear bin (base per dss_det from a loaded replay table; without
    one, `calibration=none` and `mismatch_cells=0` — `none` marks the
    comparison as not computed, not a zero-mismatch verdict against a
    base-0 calibration, which C1's fold still runs without a table;
    read `calibration` first). For the derived
    CSV of A2 this is 0 by construction; for a flight ELUT it is large
    and is the "the ELUT does not match the DSS" signal.
  - `unassigned_cells`: adc with ELUT value 32 over honoured rows.
- hw-state `det.align`: `{mode, rows, fallback, honoured,
  mismatch_cells, unassigned_cells, calibration}` →
  `instrument.detector.align.*` leaves (T4). Pure reads (the PL-2
  contract, `stix_det.c:2647-2650`).
- The stix_det.h helper **fixed now for S2/S3**:
  `int stix_det_align_adc(StixDetState *d, uint32_t fpga_det,
  uint32_t pix, uint32_t ebin)` — an adc whose loaded ELUT row
  `(pix, fpga_det)` maps to `ebin` (midpoint of the run), or −1 when
  the row is degenerate (caller uses the sim calibration) or no adc
  maps to it (caller counts a `preimage_miss`). Not called before S2.

- [ ] qtest `det-align-status`: align on, fresh flash, after NOMINAL
  entry `rows=384,fallback=384,honoured=0,mismatch_cells=0`; write one
  honoured row (the `det-replay-elut` SRAM-write pattern) → next swap
  `fallback=383,honoured=1`; align off (default) → `mode=off` and no
  `stixdet_align` line in a `-D` log after a swap (the vacuity guard);
  with align on and no replay table the classification still runs
  (today it would not).
- [ ] Suite stays green: `det-replay-elut` (classification semantics
  unchanged when align is off).

**Verification:** `scripts/stix/qt.sh all`; then `git grep -c
stixdet_align hw/sparc/trace-events` ≥ 1.

### Task A2: `elut-derive.py` — the FSW follows the sim (b)

**Files:** create `scripts/stix/elut-derive.py` (stdlib only),
`scripts/stix/tests/test-elut-derive.sh`; modify
`scripts/stix/elut-upload.py` (two additive changes, below),
`docs/stix-port/ground-station.md` (ELUT section: the derive recipe).

**Interfaces — produces:**
- `derive --dssevs PATH --out CSV` (reads only the 0xE setup words;
  the file stays where it is) or `--base JSON` (32 numbers keyed by
  `dss_det`); `--self-test`.
- `derive --b0[=B]` (decision D6, additive): writes B into the CSV's
  offset column — with no value, each row's own DSS baseline. Default:
  the flag absent, offset column 0, output byte-identical to today
  (D-Eb stands). `elut-upload.py` then reads the CSV's offset column
  for B[0] when it is non-zero (today it hardwires B[0] = 0; with 0
  the encoded bytes are unchanged), so sub-baseline adc becomes
  unassigned — dropped, like the DSS — instead of ebin 0.
- `check-dump --skip-sensitivity` (decision D7): skips the D-Eg
  sensitivity self-check (the exit-2 path) for uniform-baseline files
  and says so on the record line; without the flag, behaviour is
  unchanged.
- The sim calibration is the D-7 linear law per detector: bin width
  `W = 4096 − base`, edge `e[k] = base + ceil((k+1)·W/32)`, k = 0..30,
  written for every `(subcol = SUBCOL_FROM_FPGA_DET[f], pix)` row
  (imported from `elut-upload.py`; one FSW-sourced table per language).
  Validation: `0 ≤ base ≤ 4064` per detector — the real bound is
  `W ≥ 32`: below it the ceil edges are not distinct, above it `e[30]`
  passes 4095; the error names the `dss_det`. Edges strictly
  increasing in 1..4095. With `--dssevs`, every `dss_det` 0..31 must
  have exactly one 0xE setup word — a missing one is an error naming
  the `dss_det` (it would otherwise silently derive base 0). Output
  header matches the flight CSV shape
  the `elut-upload.py` parser guards.
- **The round-trip theorem the tool self-tests:** with `B0 = B32 = 0`
  (D-Eb unchanged), edges on the fallback's transition points
  `e[k] = base + ceil((k+1)·W/32)` and `W ≥ 32`, the honoured binning
  of the derived ELUT equals the D-7 fallback binning for **every**
  adc > base (at each edge `floor(ceil((k+1)W/32)·32/W) = k+1` since
  `32/W ≤ 1`, and between two edges both sides stay in bin k+1; the
  honoured oracle puts `adc = e[k]` in bin k+1, `elut-upload.py:341`).
  Round edges would break it: `e[k] = base + round((k+1)·W/32)` puts
  `adc = e[k]` in bin k+1 honoured but in `floor` k whenever the
  rounding went down (base 1800: `e[2] = 2015` → honoured 3, fallback
  `floor(215·32/2296) = 2`). Known, documented nuance: adc ≤ base bins
  to `ebin` 0 under B0 = 0 while the fallback drops it — ebin 0 is
  outside the LC bands (1-27) and the QLDetPerf mask (1-30); resolved
  additively by D6's `--b0` (default stays B0 = 0).
- Chain: `elut-derive.py derive` → `elut-upload.py encode --csv` →
  probe → TC(237,8) → `elut-upload.py check-dump --csv` PASS.

- [ ] Self-test (in the tool): (1) the round-trip theorem exhaustively
  for bases {0, 1800, 2000, 2020, 3800} × all 4096 adc (fallback ==
  honoured, via the E-1 oracle replay); (2) base 4065 (W < 32) and a
  negative base are refused, naming the dss_det; (3) the CSV re-parses with
  `elut-upload.parse_csv_text` → 384 rows, each (subcol, pix) once;
  (4) sensitivity: with baselines `1800 + 10·dss_det` the D-Eg check
  distinguishes identity vs wrong-direction mappings (exact differing
  row counts asserted, so `check-dump` exits 0/1 not 2); (5) an
  `encode --emit hex` unit round-trips through a Python copy of
  `processELUT` (the E-1 self-test 2 pattern); (6) `--b0`: with base b
  the oracle marks adc ≤ b unassigned and the encoded B[0] byte
  carries b, while without the flag the output is byte-identical to
  the D-Eb encoding; (7) `--skip-sensitivity` on an identity-row dump
  exits 0/1 (not 2) and prints the skip (D7).
- [ ] `test-elut-derive.sh` (bash, temp dir): runs the self-test; builds
  a synthetic ELUT dump from the derived CSV the way
  `test-elut-upload.sh` does and asserts `check-dump` exit 0; flips the
  mapping to identity → exit 1 with the predicted row count.

**Verification:** `python3 scripts/stix/elut-derive.py --self-test` →
`DERIVE SELF-TEST: PASS`; `bash scripts/stix/tests/test-elut-derive.sh`
→ all cases ok.

### Task A3: the alignment gate (c) — `align-smoke.sh`

**Files:** create `scripts/stix/align-smoke.sh` (the elut-smoke
skeleton: guards, slot library, `--dry-run`, `--check-guard`,
`--negative`), modify `scripts/stix/ql-check.py` if the SSID 32 check
needs a `--ebin` variant, `docs/stix-port/ground-station.md`.

- [ ] Fresh flash; the gate writes its own synthetic `.dssevs` in the
  run dir (never a user file): one 0xE word per `dss_det` with
  `base = 1800 + 10·d` (distinct, so D-Eg stays meaningful), then
  ~6200 events at 1 per 100 ms tick on `fpga_det` 0, `pix` 0,
  `adc` 2050 (both binnings put 2050 in `ebin` 3: fallback
  `floor(250·32/2296) = 3`; derived edges `e[2] = 2016, e[3] = 2087`).
  `-global grlib-stixctrl.det-replay-file=` (realize-time) and the
  `stixdet_align` / `stixdet_elut_fallback` / `stixdet_replay_*`
  traces.
- [ ] Leg 1: connect group; CONF @35; QMP `det-align=true` @45,
  `det-source=replay-dss` @50; NOMINAL @55 (T_nom). @120: `qom-get
  det-align-status` via the probe's `--qmp-cmd --expect-return` = the
  line with `fallback=384,honoured=0,mismatch_cells=0`; the
  `stixdet_elut_fallback` trace present; CONF @130 (QL flush).
  `ql-check.py`: SSID 32 shows the counts at `subcol` 0 byte 4
  (`ebin` 3 + 1) ≥ 0x30 (S0 of ≥ ~256 events; ~320 fall in the first
  8 periods at 10 events/s), other samples' byte 4 < 0x10.
- [ ] Leg 2 (the alignment): CONF @140; host: `elut-derive.py derive
  --dssevs <synthetic> --out <run>/derived.csv`; `elut-upload.py encode`
  → 96 × TC(237,7) @150 + 0.75 k (each TM(1,1)/TM(1,7)@5), TC(237,8)
  @230; NOMINAL @240; @300 `qom-get det-align-status` =
  `fallback=0,honoured=384,mismatch_cells=0` and `calibration=
  dss-linear`; `stixdet_elut_fallback` **absent** in this leg's trace,
  `stixdet_align` present; `pmemsave` @310 → `check-dump --csv
  derived.csv` exit 0; CONF @330; SSID 32 again at `subcol` 0 byte 4
  with the **same ebin 3** (the alignment proof: the observable bin
  did not move when the ELUT arrived). The next ROTB chunk after the
  upload (traced `stixdet_chunk`) decodes its EACC cell (pix 0,
  fpga_det 0) to `ebin` 3 as well (the `det-inject-chunk` decode) —
  the replay counts reach EACC through `add_counts`' `inj_nom`, so the
  owner's fill-level path (the chunk, not just the QL dump) is checked
  at the aligned bin too.
- [ ] `--negative`: skip the upload → leg 2 must fail exactly the
  `align-leg2` check (fallback stays 384) and nothing else; `SKIP` 77
  without the ELF.
- [ ] Host-only parts (`--dry-run`, `--check-guard`, the derive/encode
  pipeline) run without QEMU in `tests/test-align-smoke.sh` (fake-QEMU
  argv pattern of `test-elut-smoke.sh`).

**Verification:** `STIX_PORT_BASE=auto scripts/stix/align-smoke.sh
build/qemu-system-sparc` → `ALIGN-SMOKE: PASS` (about 7 min);
`--negative` → PASS (failed as planned).

## Stage C — the ADC-faithful sky mode (decision D1)

When the mode is on (`det-adc-mode`, or a tape's `adc_mode` field),
the drawn sky — the synthetic background and the tape's rate steps,
both profiles, LACC and the nominal EACC — is generated through the
loaded FPGA ELUT exactly like replay events: the tick keeps its
ebin-space exposure, and at each draw the per-ebin mass is folded
through the row's ELUT via a cached bin-mass map. A misaligned ELUT
then miscounts the sky exactly as it miscounts the replay (the AX
rotating-fill symptom, reproduced on demand); A1's report shows where
and A2's derive + upload fixes it. Default off: §34, every gate and
golden unchanged.

### Task C1: calibration, bin-mass map, the ADC sky path (model)

**Files:** modify `hw/sparc/stix_det.c`, `hw/sparc/stix_det.h`,
`hw/sparc/trace-events` (add `stixdet_adc_map`), `tests/qtest/stix.c`.

**Interfaces — produces:**
- `det-adc-mode` (bool scene knob on `/machine/stixctrl`, default
  false, survives a device reset — bench intent, like `det-source`).
  Effective while the synthetic sky draws (`det-source=synthetic`);
  inert otherwise. A tape's `adc_mode` field (T1) overrides the knob
  while the tape plays and reverts on stop/done.
- `det-cal-kev-full` (uint32 scene knob, default 160, range 1..4096):
  the keV full scale `K` the calibration maps to adc 4096 — a
  documented sim anchor, not instrument truth. A change re-arms the
  map rebuild.
- **The sim calibration (the law, fixed here):** per (dss_det, pix),
  linear `adc(keV) = base + gain·keV` with `base` = the dss_det's 0xE
  baseline from the realized replay table (0 without one — the status
  reports `base_source=zero`) and `gain = (4096 − base)/K`, identical
  across pix by default. The ebin grid it defines: ebin e covers adc
  `[E(e), E(e+1))` with `E(e) = base + ceil(e·W/32)`, `W = 4096 −
  base` — exactly the D-7 fallback grid and exactly the edges
  `elut-derive.py` writes, so the derived ELUT and the fallback agree
  with the calibration by construction (D1). In keV, ebin e ↔
  `[e·K/32, (e+1)·K/32)`; S2's keV specs convert through it (D5).
- **The bin-mass map** (`stix_det.h`): per (fpga_det, pix) a
  `double map[STIX_DET_N_EBIN][STIX_DET_N_EBIN]` + a valid flag;
  `map[s][t]` = the fraction of ebin s's mass the row puts in ebin t —
  built by one walk over the row's 4096 ELUT bytes (honoured row:
  destination `val[adc]`, 32 drops; degenerate row: the fallback's
  bin, adc ≤ base dropped), from the SRAM ram pointer
  (`memory_region_get_ram_ptr`, the `stix_det_replay_classify`
  pattern). Rebuilt at the SetEnable 0→1 edge and at the START of
  `stix_det_ql_swap` — BEFORE `stix_det_sky_materialize` (`:200` vs
  the classify at `:213-215`) — plus when a `det-cal-kev-full` change (or
  the mode turning on) arms it; never per tick, never per event.
  There is no write hook: a TC(237,8) apply is a guest CPU store into
  plain RAM (`memory_region_init_ram`, `grlib_stixctrl.c:1330` — no
  ops, no notifier) and notifies the model of nothing;
  `replay_class_pending` is not the signal (only the det-source
  setter raises it, `stix_det.c:2037-2040`). Pickup is purely the
  swap/edge rebuild, like the replay classification. The
  before-`materialize` placement is load-bearing: the bank closed by
  the first swap after a mid-period rewrite must already fold with
  the new map — unlike the replay's event-time lookup, where that
  bank still shows the old binning (the `det-replay-elut` bank-1
  precedent, `tests/qtest/stix.c:8935-8941`). **Cost:** while the
  mode is on, one full 384 × 4096 walk (~1.6 M byte reads, host ~ms)
  per rebuild point — per QL swap/edge, not per detected change
  (changes are undetectable); the walk is always 4096 bytes/row, more
  than the classify, which early-exits per honoured row
  (`stix_det.c:1482-1487`). Zero while the mode is off; nothing at
  draw time beyond the fold itself.
- **The draw fold:** `stix_det_draw_acc` keeps its per-ebin Poisson
  draws (same loop order, same RNG stream shape); with the mode on,
  each drawn count n is split over the map row by largest remainder —
  deterministic integer arithmetic, no new RNG — into the cell's
  destination ebins, for LACC and, through the same function, the
  nominal EACC chunk. The per-tick SUMCNT/LVAR lambdas use the
  map-folded SUMEMASK/VAREMASK shares (cached per profile with the
  map; the masks still read live; a 32×32 dot product per profile per
  tick). Totals per (fpga_det, pix) are unchanged by the mode except
  where the row itself drops: a honoured row's 32-valued (unassigned)
  cells, and — on a degenerate row — the fallback's adc ≤ base drop
  (`stix_det.c:1597-1599`), which loses a 1/len₀ sliver of ebin 0
  (adc = base = E(0) sits inside sim ebin 0's interval; 1/72 of it at
  base 1800, 1/128 at base 0) — benign, ebin 0 is outside the LC
  bands and the QLDetPerf mask (A2's note). Otherwise only the ebin
  distribution moves. Deposits (`det-inject`), replay events
  and S3 `single` steps are science-space and stay out of the fold.
- Trace `stixdet_adc_map(rows, nonidentity_rows, vclock_ns)` once per
  rebuild. hw-state `det.adc`: `{mode, kev_full, base_source:
  replay|zero, map_rows}` — pure reads (PL-2): `map_rows` counts the
  built rows and must not trigger the rebuild (QMP context is outside
  the tick's `mem_reentrancy_guard`); mapped to leaves in C2.

Determinism, guard, guest memory: every new draw reuses the
xorshift64*/Poisson helpers in the established call order and the
splits are integer arithmetic, so runs stay icount-stable and the
mode-off path consumes no RNG at all; the map build reads the ELUT
SRAM only where the classification already reads it, under the tick's
`mem_reentrancy_guard` (`stix_det.c:233-238`), with no MMIO dispatch
and no wall clock.

- [ ] qtest `det-adc-off`: mode off (default) gives the
  `det-counts-seed` stimulus the bank values that test asserts
  (bit-identical: no RNG draws, no folds); setting and changing
  `det-cal-kev-full` does not perturb them.
- [ ] qtest `det-adc-derived`: mode on, the test writes every ELUT
  row from the calibration's edges (the A2 law) → every LACC and EACC
  cell equals the mode-off run exactly (same seed, reset between):
  the round trip is mass-preserving and bin-exact, stronger than
  "within statistics".
- [ ] qtest `det-adc-wrong`: mode on, one row deliberately wrong (the
  base-0 edge grid while the file's baseline is 1800 — shifted: the
  drawn ebin-18 adc range, 3092–3163, lands in ebin 24) → the
  target-ebin cell ≈ 0 and the mass arrives at ebin 24 (assert the
  arrival, not only the loss — a mass-losing fold must fail),
  `det-align-status.mismatch_cells ≥ 1`, and the next ROTB chunk's
  EACC cell shows the same misbin (the `det-inject-chunk` decode); a
  `det-inject` deposit made during the run lands at its requested
  ebin bit-exact — the fold splits only the Poisson part, `draw_acc`
  adds `inj` after the draw (`stix_det.c:1648-1649`), so deposits
  stay science-space.
- [ ] qtest `det-adc-map-invalidate`: mode on with a matching row; a
  mid-run rewrite of that row (the `det-replay-elut` SRAM-write
  pattern) moves the bank closed by the FIRST swap after the rewrite
  (the rebuild runs at the start of `ql_swap`, before `materialize` —
  deliberately unlike the replay's bank-1 precedent,
  `tests/qtest/stix.c:8935-8941`), and rewriting back moves the next
  bank back.
- [ ] qtest `det-adc-props`: `det-cal-kev-full` 0 refused; the tape
  `adc_mode` field overrides the knob while playing and reverts on
  stop (the `det-tape-rate` banks witness it); the status line
  `mode=off|on,kev_full=...,base_source=...,map_rows=...` parses with
  the `plat.py` splitter.
- [ ] qtest `det-adc-guard`: with the mode on and steps changing under
  `clock_step`, every CTRL MMIO path still answers (the
  `det-tape-guard` pattern).

**Verification:** `scripts/stix/qt.sh all` — the suite grows by these
six; each RED on the parent binary (unknown property `det-adc-mode`).

### Task C2: ADC mode on the platform (verb, leaves, sampler, UI)

**Files:** modify `stixplat/verbs.py` (+`det.adc.set`),
`stixplat/plat.py` (handler, `d.add` entry), `stixplat/state.py`
(`det.adc` → `instrument.detector.adc.{mode,kev_full,base_source,
map_rows}`), `stixplat/config.py` (sample_paths +=
`"instrument.detector.adc.*"`), `stixplat/selftest.py`,
`ui/index.html`, `docs/stix-port/platform.md`.

- [ ] Self-test `adc_verb` (the fake-QMP verb pattern): `det.adc.set
  {enabled}` → `qom-set det-adc-mode` + the parsed status; 409 while
  STIX is off.
- [ ] Self-test `adc_leaves`: the four leaves exist with the right
  shapes; the sampler glob matches them and nothing else new (the
  `sampler_paths` pattern).
- [ ] UI: an "ADC sky" checkbox in the Mix tape panel next to the
  align box (writes the tape's `adc_mode` field on save) and the
  `instrument.detector.adc.mode` readout; the page-rules suite still
  passes.

**Verification:** `python3 stixplat.py --self-test` → PASS.

### Task C3: the ADC gate legs (align-smoke `--adc`)

**Files:** modify `scripts/stix/align-smoke.sh` (three legs on the A3
skeleton), `scripts/stix/ql-check.py` (the SSID 32 check gains an
`--expect-ebin-byte` parameter if the byte-19 comparison needs it),
`scripts/stix/tests/test-align-smoke.sh` (host-only parts),
`docs/stix-port/ground-station.md` (the ADC recipe).

- [ ] Leg 3 `adc-reference`: fresh flash (the A3 distinct baselines),
  `det-source=synthetic`, quiet cps 200, mode off; CONF @T; SSID 32's
  masked subcol byte 19 (ebin 18 + 1, the quiet template's 45 % line)
  captured as the reference bound, and the next ROTB chunk's EACC
  cell (pix 0, fpga_det 0) decoded at ebin 18.
- [ ] Leg 4 `adc-mismatch`: the same, mode on, plus a deliberately
  wrong ELUT uploaded first (the CSV derived from base 0 — shifted:
  the drawn ebin-18 adc range, 3092–3163 for base 1800, lands in
  ebin 24 under the base-0 row); `det-align-status` has
  `mismatch_cells ≥ 1` and `calibration=dss-linear`; SSID 32 byte 19
  ≤ 1/8 of the leg-3 reference (as built: the 128-adc base-0 bin
  keeps the flat floor, so not < 0x10) while the total still flows (the counts are in the wrong
  bins — the AX miscount, end to end); the counts arrive at ebin 24:
  SSID 32 byte 25 (ebin 24) within 5σ of the leg-3 byte-19 reference
  (the whole line moved, not lost — a mass-losing fold must fail)
  and the chunk's EACC cell decodes at ebin 24, not merely "not at
  ebin 18".
- [ ] Leg 5 `adc-aligned`: the same as leg 4 but
  `elut-derive.py derive` + `elut-upload.py encode` (the A3 leg-2
  chain) first: `mismatch_cells = 0`; SSID 32 byte 19 within 5σ of
  the leg-3 reference; the chunk's EACC cell back at ebin 18. Run
  twice: the two SSID 32 dumps byte-identical (guest-time
  determinism of the fold). `--negative`: skip the upload → leg 5
  fails exactly `adc-aligned` (as built: the status stays unaligned;
  byte 19 stays AT the reference — the degenerate rows keep the mass
  at its own bin); SKIP 77 without
  the ELF.
- [ ] Host-only parts (`--dry-run`, `--check-guard`, the
  derive/encode pipeline with the base-0 CSV) run without QEMU in
  `test-align-smoke.sh`.

**Verification:** `STIX_PORT_BASE=auto scripts/stix/align-smoke.sh
build/qemu-system-sparc --adc` → `ALIGN-SMOKE (adc): PASS` (about
+5 min over A3's 7); `--negative` → PASS (failed as planned).

## Stage S2 — spectra (outlined; wire interfaces fixed — the keV law is the stage-C calibration, decision D5)

### Task U1: spectrum specs + the adc-domain sky path

**Files:** `hw/sparc/stix_det.c/.h`, `trace-events`
(`stixdet_spectrum`), `tests/qtest/stix.c`,
`scripts/stix/platform/stixplat/tapes.py` (validation),
`ui/index.html` (the Spectrum select of the mockup), `selftest.py`.

**Interfaces (fixed):** step `spec` ∈ `"flat"` |
`{"line_adc": <adc 0..4095>}` | `{"line_kev": <keV>}` |
`{"flat_kev": [k1, k2]}` | `{"file": <path>}`; per-step
`"align": true|false` overriding the tape's `"align"` (default false).
Semantics: adc/keV/file specs fold the step's exposure through the
loaded ELUT exactly like the replay path
(`stix_det_replay_elut_ebin` / fallback, shared helper) — as a
per-spec bin-mass map (C1's mechanism, one row), never per-event
draws: a `line_adc`/`line_kev` spec is a one-adc map, a file/flat_kev
spec a 32-column row of the same shape. The keV law is decided (D5):
keV → ebin `floor(32·keV/K)` with `K` = `det-cal-kev-full`; a
`line_kev` draws at adc `base + round(gain·keV)`; no load-time
rejection remains for keV specs. `{"file"}` format (decision D4): a
two-column CSV, first line `adc,weight` or `kev,weight`, rows
`value,weight`, stdlib parsing; weights are relative shares. With
`align` on, science-space specs still convert through the preimage
`stix_det_align_adc` (T1/A1's fixed helper) so the ELUT lookup
returns the intended bin. `det-align-status` gains
`preimage_miss=N`. Traces `stixdet_spectrum(step, spec, vclock_ns)`.

- [ ] qtests: `det-spec-line` (line_adc lands in the ELUT's ebin for
  that adc; line_kev via the D5 law; the align-preimage leg as
  written); `det-spec-file` (a two-column CSV weights the draw);
  `det-spec-props` (bad spec values rejected at load).
- [ ] U2 gate: `detector-smoke.sh --spectrum` on the A3-derived flash:
  a line step at the adc whose ebin is 12 → SSID 32 peaks at byte 13 of
  the masked subcols (a keV-flavoured variant rides the same leg);
  `--spectrum --negative` (align off) fails exactly
  the `spectrum-bin` check. Expected `DETECTOR-SMOKE (spectrum): PASS`.

## Stage S3 — single events (outlined; wire interfaces fixed)

### Task V1: the `single` step kind

**Files:** `hw/sparc/stix_det.c/.h`, `trace-events`
(`stixdet_single`), `tests/qtest/stix.c`, `stixplat/tapes.py`,
`ui/index.html`, `selftest.py`.

**Interfaces (fixed):** step
`{"kind": "single", "for_s": ..., "fpga_det": F, "pix": P,
"adc": A, "n": <1..1000000>, "wait_us": <1..10000, default 20>}`
(DSS-faithful: `det(F, P, A) × N, wait W us`, `fpga_det = dss_det`) or
`"ebin": B` in place of `adc` (then `align` applies as in S2). The
model spreads the n events over `n·wait_us` of replay-style time
within the step (tick-quantized; at DSS rates n·20 µs ≤ one 100 ms
tick for n ≤ 5000 — longer bursts span ticks), feeding
`stix_det_add_counts` (so the pix gate, SUMDMASK, SUMCNT, LVAR and the
1.25× triggers all apply, §36/§37 rules). The board's "DSS sequence"
control is wired here (S1 ships it disabled, T5) — it composes
`single` steps as in the DSS tool (`det(22, 0, 1000) × N, wait 20 us`,
the mockup's mono hint). Trace
`stixdet_single(fpga_det, pix, adc, ebin, n, vclock_ns)`.

- [ ] qtests: `det-single-land` (det(22, 0, 2200) × 1000 → LACC cell
  (pix 0, fpga_det 22, ebin per ELUT) holds the counts, neighbours
  zero — the `det-inject-lacc` pattern), `det-single-gated`
  (TH-63 pix drops them, counted in the status),
  `det-single-props` (`n` 0, `wait_us` 0, adc > 4095 refused).
- [ ] V2 gate: `detector-smoke.sh --single` on the derived flash:
  SSID 32 sample `subcol` 19 (= `SUBCOL_FROM_FPGA_DET[22]`) byte 3
  ≥ 0x3F (S0 of 1000); the next ROTB chunk's EACC cell
  (pix 0, fpga_det 22, ebin 2) holds the counts (the
  `det-inject-chunk` decode); LTRG grp 11 ≈ 1.25×. Expected
  `DETECTOR-SMOKE (single): PASS`.

## Merge order and gates

T1 → T2 → (T3, T4 parallel) → T5 → T6; A1 → A2 → A3 independent of
T* (touches classification + tools only) — align first if the owner
wants the fix before the tape; C1 → C2 → C3 after A1 (C1 reuses the
classification hooks and the hw-state pattern; C3 reuses A2's
derive/encode chain and the align-smoke skeleton). Each task: qtests
RED on the parent, suite green after; each stage: its own gate
(`--tape`/scenario, `align-smoke.sh` incl. its `--adc` legs,
`--spectrum`, `--single`); UART goldens unchanged everywhere (nothing
new at boot; `det-adc-mode` defaults off).

## Accepted-diffs / docs updates

- New **§51 "Detector stimulus: per-pair rates and the mix tape"**
  (baseline: one uniform rate, no tape): the tape schema (incl. the
  `adc_mode` field), tick rules, step/background precedence, the
  replay-step constraint (`det-replay-file` is realize-time),
  markers, gate record.
- New **§52 "ELUT alignment (baseline: none)"**: the three pieces
  ((a)/(b)/(c)), the derived-CSV round-trip theorem and its B0
  nuance (resolved additively by D6's `--b0`), the classification
  exposure, `align-smoke` gate record, and the statement that the
  ebin-space sky still bypasses the ELUT (amends §34's "No ELUT"
  bullet with "except the tape's `align` option for science-space
  steps and the `det-adc-mode` sky of §53").
- New **§53 "ADC-faithful sky mode (baseline: off)"** (decision D1):
  the calibration law (base/gain per dss_det/pix, `K`), the cached
  per-row bin-mass map and its cost (one full 384 × 4096 pass per
  classification point — per QL swap/edge — while the mode is on;
  ELUT writes are undetectable, zero per event), the draw fold
  (largest-remainder splits, no new RNG), the EACC chunk path
  through the same fold, the off-bit-identical invariant, and the
  AX-miscount reproduction (C3's legs).
- Amend **§37/§40** with one line each: the classification now also
  runs under `det-align` and `det-adc-mode` and traces
  `stixdet_align` (the E-4 vacuity note gains its
  per-classification summary); §40 gains the additive `--b0` (D6,
  amends D-Eb) and `check-dump --skip-sensitivity` (D7).
- Amend **§38** (known limits): drop "the optional `--replay` gate is
  not built" once T6/A3 land? — no: §38's replay-gate bullet is about
  `detector-smoke --replay` and stays; add the tape/align limits
  (SAFE freezes playback, sub-baseline adc bins to 0 unless `--b0`).
- `platform.md`: verb-table rows (`det.tape.*`, `det.align.set`,
  `det.adc.set`), state-tree leaves (`instrument.detector.tape.*`,
  `.align.*`, `.adc.*`), a "Tapes" section next to "Timeline",
  `[timeline] tapes_dir` in the config table, `stim` in the record
  kinds/topics.
- `ground-station.md`: "Detector scene" gains the tape recipe and the
  alignment recipe (derive → upload → align-status); the ELUT section
  gains the derive sub-recipe, the `--b0`/`--skip-sensitivity` flags
  and the check-dump sensitivity caveat, plus the ADC-mode recipe
  (`det-adc-mode` → wrong-ELUT miscount → derive/upload fix).

## Owner decisions (2026-11-15, second pass — all resolved)

Numbered D1–D7, in the order of the first pass's open questions 1–7.
D1–D2 are the owner's answers; D3, D4, D6, D7 are orchestrator calls
recorded here as decided, with reasons; D5 follows from D1.

- **D1 — ADC-faithful sky: YES, as an optional mode, default off
  (owner).** The synthetic sky and the ROTB chunk (EACC) path draw
  ADC values through a per-pixel simulation calibration, then bin
  through the FPGA ELUT the FSW loaded (`val[pix][fpga_det][adc]` at
  0xE0000000), so a misaligned ELUT miscounts exactly as on hardware;
  the alignment report (c) shows where and `elut-derive` +
  `elut-upload` (b) fixes it. Default off, so §34 and every existing
  gate/golden stay unchanged. Tasks C1–C3; deviation section §53.
- **D2 — DG1–DG4: dropped from the board (owner).** No DG buttons and
  no DG → fpga_det mapping question: the FSW has no DG concept and
  the mockup's group table is not in it. The chips do not render
  (not even greyed out); targeting is All 32 / Q1–Q4 / pick-on-map.
- **D3 — playback outside NOMINAL stays frozen (orchestrator).** The
  step clock rides the FSM tick, which only runs while Enable = 1;
  tapes are NOMINAL test drivers, and a "run anyway" mode would need
  a second timer for no user. SAFE/paused freezes playback, as
  written in T1/T6.
- **D4 — S2 spectrum file format: two-column CSV with a header line
  (orchestrator).** First line `adc,weight` or `kev,weight`, rows
  `value,weight`; the unit is explicit per file and stdlib-parseable;
  keV weights convert through the D5 calibration. U1 implements it.
- **D5 — the keV ↔ adc calibration (follows from D1).** The ADC
  mode's calibration is the single keV law for the sky and the S2
  keV specs: linear `adc = base + gain·keV` per (dss_det, pix),
  base from the 0xE word, `gain = (4096 − base)/K`, `K` =
  `det-cal-kev-full` (default 160 keV full scale, a documented sim
  anchor). Defaults match the D-7 law, so `elut-derive` and the
  fallback agree (C1).
- **D6 — sub-baseline adc: the additive `--b0` option on
  `elut-derive` (orchestrator).** Absent by default → B0 = 0, output
  byte-identical to today (D-Eb stands); with `--b0` the CSV's offset
  column carries the baseline and `elut-upload.py` honours it
  additively, so sub-baseline adc is unassigned — dropped, like the
  DSS — instead of ebin 0. Amends D-Eb additively.
- **D7 — uniform baselines: `check-dump --skip-sensitivity`
  (orchestrator).** Identical rows make D-Eg exit 2 by design; the
  explicit, documented flag skips that self-check (and says so), so a
  real uniform-baseline run can still be dump-checked. The gates keep
  distinct baselines; such runs can also be verified via
  `det-align-status` + SSID 32 as before.

Nothing remains blocked on the owner. Optional, not scheduled: real
per-detector/pixel baseline-gain tables, if a DSS export ever
provides them — they would become `det-cal-*` overrides.

## Plan review (2026-11-15)

Applied by a worker edit pass over this file only. Worktree cites were
re-verified in place (`stix_det.c`, `elut-upload.py`, `ql-check.py`,
`detector-smoke.sh`, `grlib_stixctrl.c`, `plat.py`, scenario JSON);
the FSW facts (`utils.c`, no DG concept) are taken from the review
(that tree is outside this working directory). The blocker arithmetic
was re-done by hand: oracle `val[e[k]] == k+1` (`elut-upload.py:341`),
fallback `floor` (`stix_det.c:1456-1457`), base 1800 → `e[2] = 2015`
honoured 3 vs fallback 2. Dispositions:

- B1 (A2 theorem false for round edges) — **fixed**: edges are now the
  fallback's transition points `e[k] = base + ceil((k+1)·W/32)`;
  theorem, validation bound and A3's `e[2]` updated (2016); adc 2050 →
  ebin 3 still holds for both binnings.
- B2 (T6 `--tape` check self-contradictory) — **fixed**: base flush
  named (`--expect-tape-flush`; `--expect-zero-flush` would fail with
  the background), SSID 32 pinned to byte 4 ≥ 0x30 masked / < 0x10
  unmasked (quiet-template share 0.10/11 → ≈ 580 vs ≈ 6 counts in the
  32 s window; the ql-inject bound style, not zero), and the SSID 30
  rule redefined on the off window (`check_lc_contrast`'s
  quiet-before-switch window cannot exist with play ≈ T_nom).
- B3 (`gate-det-tape.json` unreachable) — **fixed**: NOMINAL entry step
  added (`tc.gs.send` TC(236,1) 05 00 + `await_tm` 0x021:4:5).
- 4 (replay steps vs trigger) — **fixed**: a replay step now requires
  `det-replay-trigger=qmp` (refused at load otherwise); qtest updated.
- 5 (`det.tape` hw-state orphan) — **fixed**: assigned to T1, incl.
  `kind`; T4's leaf list gains `kind`.
- 6 (`done` rates) — **fixed**: `done` reverts to the scene knobs
  (owner call).
- 7 (ROTB path unverified) — **fixed**: A3 leg 2 decodes the next
  chunk's EACC cell to ebin 3 (the `det-inject-chunk` pattern).
- 8 (replay-step scope) — **fixed** per owner call: replay steps stay
  model-only in S1; the board's DSS-sequence button is wired in S3
  (V1) and renders disabled in S1 (T5).
- 9 (T2 phase-fragile) — **fixed**: play is pinned to the tick right
  after a QL swap; assertions read the bank the swap just closed.
- 10 (pixmask vs background) — **fixed**: that qtest now runs with
  background cps 0 and asserts large-pix cells exactly 0.
- 11 (keV calibration undefined) — **fixed** per owner call: open
  question 5; keV specs parse but are refused at load until the owner
  names the law; "interfaces fixed now" softened to wire interfaces.
- 12 (base-cap rationale) — **fixed**: real bound `W ≥ 32`
  (`base ≤ 4064`); self-test refusal case is now 4065.
- 13 (test count) — **fixed**: 207 + 4.
- 14 (PL-2 cite) — **fixed**: `stix_det.c:2647-2650`.
- 15 (`unassigned_cells`) — **fixed** in the hw-state `det.align` field
  list.
- 16 (run-smoke path) — **fixed**: `scripts/stix/platform/tests/
  run-smoke.sh`.
- 17 (verbs.py delta) — **fixed**: `det.align.set` added to T3 Files.
- 18 (mockups outside the repo) — **fixed**: needed details inlined in
  "Other inputs" with the scratchpad paths.
- 19 (`--dssevs` 0xE completeness) — **fixed**: missing 0xE word per
  dss_det is now a named error.
- 20 (background masks) — **fixed**: the background is unmasked by
  definition; masked backgrounds are refused (T1 schema, T2, qtest).
- Review's "open questions the code answers": OQ1 folded into open
  question 2 (DG greyed out, fe_sc proposal dropped); OQ4 already
  recorded as open question 6's nuance; OQ5 stands as open question 7.
  No finding rejected. New owner items: open question 1 (ADC-faithful
  sky, first, with the task outline) and open question 5 (keV law).

## Second pass (2026-11-15, owner answers applied)

Edit pass over this file only; no code touched. The owner answered
D1 (ADC-faithful sky, YES as an optional mode) and D2 (DG dropped);
the orchestrator recorded D3/D4/D6/D7, and D5 follows from D1.
Changes: the outline of open question 1 became stage C (C1–C3, full
S1-level tasks with the bin-mass-map performance design, the
determinism/guard/guest-memory rules, and the off-bit-identical /
derived-equal / wrong-ELUT qtests and gate legs); the DG chips are
gone (T5, the mockup notes, the index-space paragraph); the tape
schema gains `adc_mode` (rejected at load until C1, T1); U1's keV and
file-format blockers are resolved (D4/D5) and its spectra fold
per-spec maps, not per-event draws; A1's classification also runs
under `det-adc-mode`; A2 gains `--b0` (D6) and `check-dump
--skip-sensitivity` (D7); review focus gained items 6–8; merge order
and the accepted-diffs names (§53 new) updated; the open-questions
section became the decisions record. No first-pass finding was
reverted.

## Re-review (2026-11-15)

Second review pass: no blockers; all SHOULD-FIX items and nits 5–9
applied in this file only (no code touched). Every corrected claim was
re-verified against the worktree first (`replay_class_pending` set
only by the det-source setter, `stix_det.c:2037-2040`; the ELUT SRAM
is plain RAM, `grlib_stixctrl.c:1330`; `materialize` `:200` vs the
swap classify `:213-215`; the classify early-exit `:1482-1487`, the
fallback drop `:1597-1599`, the `inj` sum `:1648-1649`; the bank-1
precedent `tests/qtest/stix.c:8935-8941`; PL-2 `:2647-2650`).
Dispositions:

- SF1 (C1 map invalidation: wrong signal + ordering) — **fixed**: the
  "TC(237,8) apply raises `replay_class_pending`" claim removed (the
  apply is a guest store into plain RAM and notifies nothing; the
  pending flag belongs to the det-source setter); the rebuild is now
  pinned to the START of `ql_swap`, before `materialize`, so the bank
  closed by the first swap after a mid-period rewrite folds with the
  new map — noted as deliberately unlike the replay's bank-1
  precedent; `det-adc-map-invalidate` pins that exact swap; review
  focus 7 aligned.
- SF2 (C1 cost claim unimplementable) — **fixed**: the honest bound
  is one full 384 × 4096 walk per classification point (per QL
  swap/edge) while the mode is on — changes are undetectable — and
  the walk is always 4096 bytes/row (the classify early-exits);
  ~ms host per swap, zero when off. Approach 4(ii) and the §53
  sketch aligned.
- SF3 (totals invariant overbroad) — **fixed**: qualified — unchanged
  except where the row itself drops (32-valued cells; the degenerate
  fallback's adc ≤ base, a 1/len₀ sliver of ebin 0: 1/72 at base
  1800, 1/128 at base 0), benign per A2's note.
- SF4 (T6 base contradicts T2) — **fixed**: the `--tape` base is
  `det-source=synthetic` with `det-quiet-cps=0` (the play setter
  refuses `zero`).
- N5 (deposits unpinned) — **fixed**: `det-adc-wrong` now asserts a
  `det-inject` deposit lands at its requested ebin bit-exact while
  the sky folds (`draw_acc` adds `inj` after the draw,
  `stix_det.c:1648-1649`). While editing, the bullet's "the mass in
  lower ebins" was corrected to "the mass arrives at ebin 24": the
  base-0 row maps the drawn ebin-18 adc range 3092–3163 to val 24
  (re-derived; same shift C3 leg 4 already recorded) — an extra fix
  beyond the findings, flagged here.
- N6 (C3 leg 4 mass-loss passes) — **fixed**: leg 4 also asserts
  arrival at ebin 24 (SSID 32 byte 25 within 5σ of the leg-3
  reference; the chunk cell at ebin 24).
- N7 (hw-state rebuild) — **fixed**: `map_rows` counts built rows and
  must not trigger the rebuild (PL-2; QMP context is unguarded).
- N8 (T6 `--negative` scope) — **fixed**: the SSID 30 rule is now
  stated as part of the `tape` check (one verdict), so the negative
  — which fails it too, having no rate step — fails exactly `tape`.
- N9 (calibration=none) — **fixed**: A1 states `none` marks the
  comparison as not computed, not a zero-mismatch verdict (C1's fold
  calibration still runs at base 0 without a table).

Nothing rejected; the review's "verified sound" list needed no plan
change. Reviewer runs stay per the task Verification lines
(`scripts/stix/qt.sh all`, `elut-derive.py --self-test` and the gates
post-implementation); the reviewer ran none, and neither did this
edit pass.
