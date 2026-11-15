# Detector stimulus: per-detector/per-pixel rates, the mix tape, ELUT alignment; spectra and single events staged — plan (2026-11-15)

> For agentic workers: implement task by task; steps use `- [ ]`.
> Workers cannot build or run anything: write code + self-test cases,
> the reviewer runs them and commits. S1 and the alignment track are
> written in full; S2/S3 are outlined with their interfaces fixed now.

**Goal:** the Detectors → Stimulus board gets per-detector and per-pixel
rate stimulus driven by JSON "mix tapes" played on guest time, and —
opt-in, first-class — ELUT alignment between the flight software's
loaded ELUT and the detector simulation, so ADC-domain counts land in
the intended science bins whatever ELUT the FSW loaded.

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
through the preimage of the loaded ELUT (consumed by S2/S3).

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

Other inputs: mockup board `Stimulus.dc.html` (Target chips All 32 /
Q1–Q4 / DG1–DG4 / pick on map; pixel select; kind buttons Rate / Single
events / DSS sequence; cps + spectrum + duration; the mix tape with a
background row, a step row, progress, saved-tapes select, Loop, Play,
Stop, a step table with move up/down/remove; footnote "Steps run one
after another; the background layer runs under all of them. Every step
start is a marker on the Timeline. A tape is a JSON file next to the
saved views"; the "Emulator support today vs needed" box). Docs:
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
DSS = fpga_det, FSW/sub-collimator = subcol. **UNVERIFIED:** the
DG1–DG4 → fpga_det grouping (front-end detector groups; not in
`stix_det.c`'s table set) — open question 1.

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
- The synthetic sky stays an ebin-space source (§34 deviation stands);
  alignment never changes its default path, only opt-in per tape.
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
   translates Q1-Q4/DG/pixel picks into masks). Chosen.
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

## Review focus (each pinned to a task's test)

1. **Step timing is tick-quantized and guest-time.** A step boundary
   lands on a 100 ms tick; the tape does not advance while FSM Enable =
   0 (SAFE freezes it) or while paused. Pinned: `det-tape-rate`
   (ClearEnable mid-tape → no step progress; resume continues).
2. **Background vs step precedence, and `off`.** A step wins on its
   masks; an `off` step (all 32) must zero even the background layer.
   Pinned: `det-tape-rate` bank 3 all-zero assertion.
3. **Index-space mistakes.** The tape masks are `fpga_det`-indexed; the
   TM shows `subcol`. Pinned: the `--tape` gate's SSID 32 check (non-zero
   exactly at `SUBCOL_FROM_FPGA_DET[f]` for the masked f) and
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
   counts. Pinned: `test-elut-derive.sh` case 4.

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
    "loop": <bool, default false>,
    "background": <step-like rate, no for_s>,
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
  build (S2/S3)"** so S1 never half-implements them. Every error names
  the step index. Masks are `fpga_det`/`pix` indexed; `subcol`-keyed
  steps are refused (`"subcol_mask" is not a tape field; use
  fpga_det_mask (dss_det = fpga_det)`).
- `det-tape-play` (write-only bool): true starts at step 0 on the next
  tick (store-then-apply, D-Dj); false stops and reverts the rates to
  the scene knobs at the next tick. Refused (error) while playing
  already / not loaded.
- `det-tape-status` (read-only str, one line, atomic):
  `state=idle|ready|playing|done|stopped,name=...,step=K/N,
  label=...,step_left_ticks=...,loop=...,align=...`.
- Tape state in `StixDetState` (`stix_det.h`): loaded steps (owned
  copy), play request, current step index, tick countdown. A device
  reset stops playback and keeps the tape (state `ready`), like the
  scene; a new `det-tape-load` while playing stops first.
- Semantics: the step clock advances one tick per tick **only while
  FSM Enable = 1** (the tick's own gate); at a step start the model
  traces `stixdet_tape_step(index, kind, label, vclock_ns)` and applies
  the step (T2); `replay` steps start/stop the D-7 engine at step
  start/end (source switches to/from replay-dss for the step's
  duration; refused at load when no table was scanned at realize);
  `loop=true` restarts at step 0 after the last step.

- [ ] qtest `det-tape-props`: every rejection above (bad JSON, > 64
  steps, > 64 KiB, `for_s` 0, unknown kind, `pix_mask` > 0xFFF,
  `cps` > 1000000, `spec` `{"line_adc": 1000}` in S1, `subcol_mask`,
  play without a tape, replay step with no realized table); a rejected
  load leaves the previous tape intact (`det-tape-status` unchanged).
- [ ] qtest `det-tape-status`: idle → ready → playing (step=1/N) →
  done; `loop=true` wraps; play=false mid-step → stopped; a device
  reset → ready with the tape kept; the status line parses with the
  `plat.py` key=value splitter.
- [ ] qtest `det-tape-replay`: a tape `[rate 1 s, replay for_s 2
  offset_ms 0, off 1 s]` over a synthetic `.dssevs` (the
  `det-replay-decode` generator): during the replay step
  `det-replay-status` reads `running` and the LACC bank gets the
  events; before/after it reads `stopped`/`armed` and no replay counts
  land; the step end stops the replay.
- [ ] qtest `det-tape-guard`: with steps changing under `clock_step`,
  every CTRL MMIO path still answers (the `det-chunk-guard` pattern).

**Verification:** `scripts/stix/qt.sh all` — e.g. 96 + 4 of 96 + 4 ok
(the exact count grows by the four new tests); each new test RED on the
parent binary (unknown property `det-tape-load`).

### Task T2: per-pair rate application (model)

**Files:** modify `hw/sparc/stix_det.c` (sky tick + step application),
`hw/sparc/stix_det.h`, `tests/qtest/stix.c`.

**Interfaces — produces:**
- While a tape plays, the sky tick's per-pair rate for (fpga_det, pix)
  is: the active step's `cps` if the pair is inside the step's
  `fpga_det_mask`/`pix_mask`, else the background's `cps`; `kind off`
  contributes 0 on its masks (an all-32 off step silences the
  background too). The rate still flows through the existing exposure +
  template (`pix_share`, `ebin_share`) and the per-tick pix gate — a
  step rate on a gated-off pix produces nothing, like a real event.
- Rate steps require `det-source=synthetic` at play (the play setter
  refuses otherwise, naming the source); the scene's `det-flare` and
  the two `*-cps` knobs are ignored while playing and resume on stop.
- The nominal integration / SUMCNT / LVAR read the same per-pair rate
  (one code path: the rate lookup feeds `stix_det_sky_tick`'s existing
  accumulators).

- [ ] qtest `det-tape-rate`: QlIntTime 9 (1 s periods),
  `det-quiet-cps=0`, background `{all, all pix, cps 100}`, steps
  `[{rate, for_s 2, fpga_det_mask 0x00FF00FF (Q1+Q3), cps 20000}, {off,
  for_s 1}]`. After the two step-1 swaps: the LACC sum over pix×ebin of
  fpga_det 0 is 20000 ± 5σ (σ≈141), fpga_det 16 likewise, fpga_det 2 is
  100 ± 5σ (background); after the step-2 swap every one of the 384
  cells is exactly 0 (off beats the background); `det-tape-play=false`
  then gives an all-zero bank (revert to quiet-cps 0). ClearEnable
  mid-step freezes the step countdown (`det-tape-status`
  `step_left_ticks` unchanged across a paused second).
- [ ] qtest `det-tape-pixmask`: same shape with step `pix_mask 0xF00`
  (small pix 8-11 only): large-pix cells zero, small-pix cells
  ≈ 0.22/8.88 of the rate (the template shares still apply).
- [ ] qtest `det-tape-seed`: the same seed and tape give identical
  banks (`det-counts-seed` pattern; icount determinism in guest time).

**Verification:** `scripts/stix/qt.sh all`; `--tape` gate (T6) for the
FSW-visible end.

### Task T3: platform tapes — files, verbs, API

**Files:** create `scripts/stix/platform/stixplat/tapes.py`; modify
`stixplat/verbs.py` (+`det.tape.save|delete|play|stop`),
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
n_steps,label,loop}` and `det.align` → `instrument.detector.align.*`),
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
  `Stimulus.dc.html`): Target chips (All 32, Q1-Q4, DG1-DG4 once open
  question 1 is answered, pick-on-map rows from the existing detector
  map), pixel select, "cps per detector" + duration inputs, Send now /
  Add to tape (S1: rate steps only; the Spectrum and Single-events
  controls render disabled with the mockup's "Emulator support today"
  wording), and the Mix tape panel: saved-tapes select (GET
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

- [ ] `--tape`: zero-sky base like the D-3 modes, QlIntTime default;
  @75 QMP loads the gate's tape (background quiet 20 cps all 32; steps:
  `rate` Q1+Q3 2000 cps for 60 s → `off` 30 s), `det-tape-play true`;
  @215 CONF as usual. New check `tape`: `ql-check.py
  --ssid32-subcols 0,1,5,6,4,10,11,12,30,31,25,26,21,27,19,20` — in the
  one SSID 32 dump, non-zero counts exactly at those `subcol`
  (`SUBCOL_FROM_FPGA_DET[f]` for the masked `f`), zero at the other 16,
  and the SSID 30 flare-window band-0 median ≥ 8 compressed steps above
  the quiet median (the `ql-contrast` rule) with the off-window samples
  at the quiet floor. `--tape --negative` plays only the `off` step and
  fails exactly `tape`. Expected line
  `DETECTOR-SMOKE (tape): PASS`; SKIP 77 without the ELF (house
  convention).
- [ ] `gate-det-tape.json` (platform scenario, run mode): power on/boot,
  `det.tape.save`, `det.tape.play`, `await_state`
  `instrument.detector.tape.step` eq 2 within 70 s, `det.tape.stop`,
  `await_state` `.tape.state` eq `stopped`; a `stim` marker in the log
  (the scenario's `expect` records are the verdict).

**Verification:** `STIX_PORT_BASE=auto scripts/stix/detector-smoke.sh
build/qemu-system-sparc --tape` → `DETECTOR-SMOKE (tape): PASS`; the
scenario via run mode (`tests/run-smoke.sh` extended) → pass.

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
  whenever `det-align` is on **or** the source is replay-dss; it emits
  one summary trace `stixdet_align(rows, fallback, honoured,
  mismatch_cells, vclock_ns)` per classification (closes the E-4
  vacuity gap: the classification itself becomes visible).
- `det-align-status` (read-only str):
  `mode=off|on,rows=384,fallback=F,honoured=H,mismatch_cells=M,
  unassigned_cells=U,calibration=none|dss-linear`.
  - `mismatch_cells`: over honoured (non-degenerate) rows only, the
    count of adc whose honoured ebin differs from the sim calibration's
    linear bin (base per dss_det from a loaded replay table; without
    one, `calibration=none` and `mismatch_cells=0`). For the derived
    CSV of A2 this is 0 by construction; for a flight ELUT it is large
    and is the "the ELUT does not match the DSS" signal.
  - `unassigned_cells`: adc with ELUT value 32 over honoured rows.
- hw-state `det.align`: `{mode, rows, fallback, honoured,
  mismatch_cells, calibration}` → `instrument.detector.align.*` leaves
  (T4). Pure reads (the PL-2 contract, `grlib_stixctrl.c:1628-1635`).
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
`docs/stix-port/ground-station.md` (ELUT section: the derive recipe).

**Interfaces — produces:**
- `derive --dssevs PATH --out CSV` (reads only the 0xE setup words;
  the file stays where it is) or `--base JSON` (32 numbers keyed by
  `dss_det`); `--self-test`.
- The sim calibration is the D-7 linear law per detector: bin width
  `W = 4096 − base`, edge `e[k] = base + round((k+1)·W/32)`, k = 0..30,
  written for every `(subcol = SUBCOL_FROM_FPGA_DET[f], pix)` row
  (imported from `elut-upload.py`; one FSW-sourced table per language).
  Validation: `0 ≤ base ≤ 3800` per detector (else fewer than 31
  distinct edges; the error names the `dss_det`), edges strictly
  increasing in 1..4095. Output header matches the flight CSV shape
  the `elut-upload.py` parser guards.
- **The round-trip theorem the tool self-tests:** with `B0 = B32 = 0`
  (D-Eb unchanged) and `base ≤ 3800`, the honoured binning of the
  derived ELUT equals the D-7 fallback binning for **every** adc
  (rounding error ≤ 0.5 keeps `floor` on the right side of each edge
  since `16/W < 1`). Known, documented nuance: adc ≤ base bins to
  `ebin` 0 under B0 = 0 while the fallback drops it — ebin 0 is outside
  the LC bands (1-27) and the QLDetPerf mask (1-30); recorded, not
  engineered around (open question 4).
- Chain: `elut-derive.py derive` → `elut-upload.py encode --csv` →
  probe → TC(237,8) → `elut-upload.py check-dump --csv` PASS.

- [ ] Self-test (in the tool): (1) the round-trip theorem exhaustively
  for bases {0, 1800, 2000, 2020, 3800} × all 4096 adc (fallback ==
  honoured, via the E-1 oracle replay); (2) base 3801 and a negative
  base are refused, naming the dss_det; (3) the CSV re-parses with
  `elut-upload.parse_csv_text` → 384 rows, each (subcol, pix) once;
  (4) sensitivity: with baselines `1800 + 10·dss_det` the D-Eg check
  distinguishes identity vs wrong-direction mappings (exact differing
  row counts asserted, so `check-dump` exits 0/1 not 2); (5) an
  `encode --emit hex` unit round-trips through a Python copy of
  `processELUT` (the E-1 self-test 2 pattern).
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
  `floor(250·32/2296) = 3`; derived edges `e[2] = 2015, e[3] = 2087`).
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
  did not move when the ELUT arrived).
- [ ] `--negative`: skip the upload → leg 2 must fail exactly the
  `align-leg2` check (fallback stays 384) and nothing else; `SKIP` 77
  without the ELF.
- [ ] Host-only parts (`--dry-run`, `--check-guard`, the derive/encode
  pipeline) run without QEMU in `tests/test-align-smoke.sh` (fake-QEMU
  argv pattern of `test-elut-smoke.sh`).

**Verification:** `STIX_PORT_BASE=auto scripts/stix/align-smoke.sh
build/qemu-system-sparc` → `ALIGN-SMOKE: PASS` (about 7 min);
`--negative` → PASS (failed as planned).

## Stage S2 — spectra (outlined; interfaces fixed now)

### Task U1: spectrum specs + the adc-domain sky path

**Files:** `hw/sparc/stix_det.c/.h`, `trace-events`
(`stixdet_spectrum`), `tests/qtest/stix.c`,
`scripts/stix/platform/stixplat/tapes.py` (validation),
`ui/index.html` (the Spectrum select of the mockup), `selftest.py`.

**Interfaces (fixed):** step `spec` ∈ `"flat"` |
`{"line_adc": <adc 0..4095>}` | `{"line_kev": <keV>}` |
`{"flat_kev": [k1, k2]}` | `{"file": <path>}`; per-step
`"align": true|false` overriding the tape's `"align"` (default false).
Semantics: adc/keV/file specs make the sky tick draw per-event adc
values from the spectrum and bin them exactly like the replay
(`stix_det_replay_elut_ebin` / fallback, shared helper); a keV spec
maps keV → target ebin via the sim calibration, then — with `align`
on — to adc via `stix_det_align_adc` (T1/A1's fixed helper) so the
ELUT lookup returns the intended bin; with `align` off it bins
directly in ebin space (today's §34 rule). `{"file"}` CSV format
(`adc,cps` vs `kev,cps`, header line) is settled with the owner before
U1 lands (open question 3). `det-align-status` gains
`preimage_miss=N`. Traces `stixdet_spectrum(step, spec, vclock_ns)`.

- [ ] qtests: `det-spec-line` (line_adc lands in the ELUT's ebin for
  that adc; align preimage: a keV line with align on lands at the
  intended ebin on a derived flash and at the raw-calibration bin with
  align off); `det-spec-file` (a two-column CSV weights the draw);
  `det-spec-props` (bad spec values rejected at load).
- [ ] U2 gate: `detector-smoke.sh --spectrum` on the A3-derived flash:
  a line step whose keV maps to `ebin` 12 → SSID 32 peaks at byte 13 of
  the masked subcols; `--spectrum --negative` (align off) fails exactly
  the `spectrum-bin` check. Expected `DETECTOR-SMOKE (spectrum): PASS`.

## Stage S3 — single events (outlined; interfaces fixed now)

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
1.25× triggers all apply, §36/§37 rules). Trace
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
wants the fix before the tape. Each task: qtests RED on the parent,
suite green after; each stage: its own gate (`--tape`/scenario,
`align-smoke.sh`, `--spectrum`, `--single`); UART goldens unchanged
everywhere (nothing new at boot).

## Accepted-diffs / docs updates

- New **§51 "Detector stimulus: per-pair rates and the mix tape"**
  (baseline: one uniform rate, no tape): the tape schema, tick rules,
  step/background precedence, the replay-step constraint
  (`det-replay-file` is realize-time), markers, gate record.
- New **§52 "ELUT alignment (baseline: none)"**: the three pieces
  ((a)/(b)/(c)), the derived-CSV round-trip theorem and its B0
  nuance, the classification exposure, `align-smoke` gate record, and
  the statement that the ebin-space sky still bypasses the ELUT
  (amends §34's "No ELUT" bullet with "the tape's `align` option is
  the opt-in exception for science-space steps").
- Amend **§37/§40** with one line each: the classification now also
  runs under `det-align` and traces `stixdet_align` (the E-4 vacuity
  note gains its per-classification summary).
- Amend **§38** (known limits): drop "the optional `--replay` gate is
  not built" once T6/A3 land? — no: §38's replay-gate bullet is about
  `detector-smoke --replay` and stays; add the tape/align limits
  (SAFE freezes playback, sub-baseline adc bins to 0, DG mapping).
- `platform.md`: verb-table rows (`det.tape.*`, `det.align.set`),
  state-tree leaves (`instrument.detector.tape.*`, `.align.*`), a
  "Tapes" section next to "Timeline", `[timeline] tapes_dir` in the
  config table, `stim` in the record kinds/topics.
- `ground-station.md`: "Detector scene" gains the tape recipe and the
  alignment recipe (derive → upload → align-status); the ELUT section
  gains the derive sub-recipe and the check-dump sensitivity caveat.

## Open questions for the owner (do not block)

1. **DG1–DG4 mapping.** The mockup's target chips need DG → fpga_det.
   Proposal: DG_n = front-end detectors `fe_sc` 8(n−1)+1 .. 8n mapped
   through `utils_oldSubcolIdToFpgaId`; UNVERIFIED against the FSW —
   confirm or give the table; until then the board ships All/Q1-Q4/
   pick-on-map and greys DG out.
2. **Tape playback outside NOMINAL.** The step clock rides the FSM
   tick, so SAFE/paused freezes a tape. Proposal: keep (tapes are
   NOMINAL test drivers); a "run anyway" mode would need a second
   timer and is not planned.
3. **Spectrum "from file" format.** Proposal: a two-column CSV with a
   first line `unit=adc` or `unit=kev`, rows `value,cps`, stdlib
   parsing; confirm or name the DSS tool's export format to match.
4. **Sub-baseline adc.** With the derived ELUT and B0 = 0 (D-Eb), adc
   ≤ the DSS baseline bins to `ebin` 0 where the DSS would drop it.
   Proposal: keep B0 = 0 and document; if you want the drop, we add a
   per-row `--b0` to `elut-upload.py` (additive, default unchanged).
5. **Uniform baselines.** If your real `.dssevs` has one baseline for
   all 32 detectors, the derived CSV's rows are identical and
   `check-dump`'s D-Eg sensitivity check exits 2 by design; the gate
   uses distinct baselines. Proposal: accept, and verify such runs via
   `det-align-status` + SSID 32 instead of `check-dump`.
