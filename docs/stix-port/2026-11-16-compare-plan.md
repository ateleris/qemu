# Run bundles Stage S3: compare two runs — plan (2026-11-16)

> **PLAN ONLY — not approved.** Nobody implements until the owner
> signs off. For agentic workers (once approved): implement task by
> task; steps use `- [ ]`. Workers cannot build or run anything: write
> code + self-test cases, the reviewer runs them and commits. This plan
> expands the S3 outline of `2026-11-15-run-bundles-plan.md` (Task R7)
> under the owner review fixes recorded there: mode rows compare
> `detail.params` hex in v1; no overrun EID — the overrun rows derive
> from the `hk_step` NIXD0501 steps and the `instrument.rotb.*` /
> `as.dropped` series; the Compare panel renders
> Check | Baseline | Replay | Result. Where the code differs from the
> outline, the code wins — see "Deviations".

**Goal:** compare two finished run bundles — the baseline and an "other"
run (in practice the S2 replay of it) — decisions first (pass 1), series
numbers within tolerances (pass 2), and report the first divergence so
the Timeline two-run overlay can mark it. The compare consumes only
what S1/S2 already write (`manifest.json`, `decisions.jsonl`,
`log.jsonl`), writes nothing into either run dir, and is driven from the
Runs board's Compare panel, a verb, and a CLI flag.

**Architecture:** one new module `stixplat/compare.py` with four parts:
the loaders (a bundle's decisions — `decisions.jsonl` or re-derived from
the log slice through `bundle.decide`), the pure
`compare(baseline_dir, other_dir, tol)`, the artifact writer
(`<retention.run_root>/compares/<a>-vs-<b>.{jsonl,json}`), and the
per-run series reader the overlay needs (a `SeriesIndex` rebuilt from
the run's log slice, cached). Surfaces: a `[compare]` config section;
the verb `run.compare {a, b}`; the CLI `--compare BASELINE OTHER`;
`GET /runs/<run_id>/series` for the overlay's second series. The board's
Compare panel and the Timeline overlay are two separate UI tasks.

**Spec:** owner decision 2026-11-15 (run bundles): "S3 = compare two
runs (decisions first, numbers within tolerances; first divergence
marked on the Timeline two-run overlay)". Outline + fixed formats:
`2026-11-15-run-bundles-plan.md` R1/R2 (formats) and R7 (the outline
expanded here). S2 as built: platform.md "Replaying a bundle (S2)";
its live check gave the same 7 decisions with a max |sim_time delta| of
0.44 s. House style: `2026-11-15-replay-plan.md`.

## Key facts

Read in this worktree; every claim re-verified by reading before
writing this plan. UNVERIFIED marked.

The decisions format (bundle.py — S3's pass-1 input)
- `decide(obj, state)` (bundle.py:217-275) emits
  `{"v": 1, "seq", "sim_time", "boot_id", "kind", "key", "class",
  "detail"}`; kinds `event` (detail `{name, params}` — params the
  undecoded hex string, e.g. `"02"` for 0x0021, t_bundle_decisions
  selftest.py:6145-6154), `ack` (detail `{subtype, fid}`, key
  `"0x%04X/%d"`), `hk_step` (detail `{value, previous}`, key
  `"hk:<sid>:<code>"` — the series id space, bundle.py:270-273).
  `EID_CLASSES` (bundle.py:79-80). The ack rule keys on tc records that
  carry hex (bundle.py:226-235).
- decisions.jsonl lines are strictly increasing in seq: the Writer
  buffers per seq and flushes in order (bundle.py:432-448), and close
  merges stragglers with a sorted rewrite (bundle.py:508-549). A
  compare may therefore rely on seq-ordered lines.
- The manifest carries `t0` (the *last* boot's bench time — the Writer
  overwrites `_t0` on every boot record, bundle.py:400-402;
  power.py:171-182 sets `t0 = clock.exact_sim()` at boot),
  `start_offset` (bundle.py:467), `log_rotations` (:468), the full
  launch `config` (:476), `open`, `run_id`, `kind` ("run"|"replay"),
  and on a replay `of`/`elf`/`qemu-binary` (replay.py:644-647).
- sim_time is the bench clock: base + vclock, and the bench runs on the
  wall clock while OFF (simclock.py:105-119). A run's sim_time includes
  its pre-boot off-time, so a replay's absolute sim_time carries its own
  launch latency (QEMU spawn, the off-phase holds) as a systematic
  offset against the origin's.
- Run-id resolution for the surfaces: `check_run_id` 400s before any
  filesystem access (bundle.py:572-577), `_resolve` 404s unknown
  (bundle.py:652-663); `listing`/`_entry` (bundle.py:597-649) feed the
  board — entries carry no `kind`/`of` today.

The slice reader and where series live
- `replay._slice`/`_lines` (replay.py:54-85) yield the session's
  records (start_offset inside `log.jsonl.<log_rotations>.gz`, then the
  newer parts, then log.jsonl); `load_inputs` (replay.py:110-155) feeds
  them through `classify` with per-run state. This iteration is the
  re-derive path's reader (extracted as `replay.iter_records`, C1).
- There is no persisted series file: points live in an in-memory
  `SeriesIndex` per controller (core.py:80-83), rebuilt for a finished
  run by streaming its log parts (the ViewRun pattern,
  viewrun.py:120-150); `GET /api/v1/series` answers queries against
  that index (api.py:199-204, 262-280). Pass 2 and the overlay must
  rebuild an index per run the same way. `query(ids, None, None, n)`
  with a huge n returns the raw (undecimated) arrays — the 10..5000
  bound lives only in the API layer (api.py:277-278).
- Series ids pass 2 and the overlay need exist as state leaves sampled
  by default: `instrument.rotb.*` (config.py:86; state.py:34-35, :125),
  `instrument.detector.tape.dropped` (state.py:33; the sample path
  `instrument.detector.tape.*`, config.py:88), and `hk:2:NIXD0501` from
  tm records with `data.hk` (series.py:157-188; the alias `hk.rb_level`
  only when an IDB is loaded).
- The R7 outline's `as.dropped` names no code path — the drop gauges
  are `instrument.detector.tape.dropped` /
  `instrument.detector.replay.dropped` (state.py:28-33). The mockup
  `Runs.dc.html` is not in the repo (UNVERIFIED which series its
  "Processed drain" row plots).
- `[bundle]` config lives in DEFAULTS (config.py:101-103) with its
  `_check` clauses (config.py:239-242); `[compare]` joins them (C1).

The S2 producer and its measured jitter
- A replay is S3's main "other" run: `replay.run` writes a fresh bundle
  (`kind "replay"`, `of`, `elf`, `qemu-binary`; replay.py:640-647)
  whose decisions come from the same Writer every controller attaches
  (core.py:105-116). The origin is read-only throughout
  (t_replay_run, selftest.py:7327, :7371).
- Live check (platform.md:709-717): origin vs replay — the same 7
  decisions in the same (kind, key) order, max |sim_time delta| 0.44 s,
  measured on **absolute** sim_times of zipped pairs, so the figure
  includes the replay's boot-latency offset (see Approaches 1).

Self-test harness to reuse: `Env` (selftest.py:97-138), the record
builder `_rec` (selftest.py:6132-6137), the synthetic origin
`_replay_origin` (selftest.py:6801-6870), `_dir_state`
(selftest.py:6041-6047), `_http`/`_post` (selftest.py:3612, :5934),
`CASES` (selftest.py:7625-7754), the UI walks (`_UiParser`
selftest.py:3935; `UI_RUNS_VERBS`/`UI_RUNS_IDS` selftest.py:3840-3842;
`UI_ENDPOINTS` selftest.py:3845-3847), the verbs-registry equality
check (selftest.py:2034-2036) and `t_verbs_doc` (selftest.py:3788).

UNVERIFIED
- The mockup rows "First overrun (HK level 7)", "Overruns in the run"
  and "Processed drain": `Runs.dc.html` is not in the repo; the mapping
  "overrun == RB level 7" and the series behind "Processed drain" rest
  on the outline's inline note only
  (2026-11-15-run-bundles-plan.md:594-597).
- The in-run decision jitter with the boot offset removed: the 0.44 s
  is one measured run on absolute times, not a bound for the aligned
  metric (the anchor subtraction can at most double a delta:
  |(a_i−a_0)−(b_i−b_0)| ≤ |a_i−b_i| + |a_0−b_0|).
- `as.dropped`: no such series id exists in code (see above).

## Global constraints

Inherited from the run-bundles and replay plans (unchanged): stdlib
only; API v1 additive only; **no new record kind, topic or source**
(the compare writes no records at all; `run.compare` is a verb whose
cmd/result records classify as excluded — one `_EXCLUDED_VERB_WHY`
entry, bundle.py:49-76); the listener rule untouched; the flash is
never read; test/self-test configs never touch ports 8080, 21502,
21511, 21512 (API ports in the 18081+ band); no FSW edits;
`python3 stixplat.py --self-test` stays PASS and every task adds its
own `check(...)` cases. Plus, for S3:

- **Both input bundles are read-only.** The compare opens the two
  manifests, decisions files and logs, and writes nothing into either
  run dir — the artifact goes under `<retention.run_root>/compares/`
  (pinned by `_dir_state` in the verb and CLI tests, the
  t_replay_run pattern).
- **GET stays read-only.** The compare itself is a POST verb plus the
  CLI; `GET /runs/<run_id>/series` only reads and rebuilds an
  in-memory index.
- **A comparison never stops at a refusal of one side.** A missing
  decisions.jsonl re-derives (below); only a missing/unresolvable
  bundle (no manifest *and* no log, a rotated-away slice with no
  decisions to fall back on, a bad or unknown id) refuses — and so
  does a bundle whose manifest is still `open` (a growing decisions
  file would make the verdict a moving snapshot; the replay refuses
  the same way, replay.py:125-126).

## Approaches considered

1. **The time base.** (i) Absolute sim_time — what the S2 live check
   measured: simple, but every delta carries each run's pre-boot bench
   time (the replay's launch latency) as a systematic offset.
   (ii) Relative to each manifest's `t0`: removes the boot offset, but
   `t0` is the *last* boot's time (bundle.py:400-402) and is null
   without a boot (pre-S1 runs have no manifest at all).
   (iii) Relative to each run's first common decision (chosen): one
   global shift per side, defined by the pass-1 anchor itself, needs no
   boot and no manifest; the S2-measured 0.44 s bounds the unaligned
   deltas it is computed from, and the aligned residual is bounded by
   twice that (the UNVERIFIED bullet below) — still inside
   `tol_event_s = 1.0` with margin.
2. **Pairing after the anchor.** (i) Positional (chosen — keeps the
   "same (kind, key) sequence" property the outline asks for; seq order
   is the tie-break). (ii) A (kind, key) merge join: keys repeat
   (acks repeat, several hk_step codes interleave), so a join needs a
   tie-break policy that seq order already answers better.
3. **Pass 2 granularity.** (i) Index-wise value comparison with a count
   slack, times ignored (chosen — smallest viable, robust to sample-tick
   jitter). (ii) A nearest-time join within a sample period (robust to
   a dropped sample, more code). (iii) Summary statistics only
   (min/max/last) — not point-wise, weaker than the outline's words.
4. **Where the compare writes.** (i) Into a run dir — rejected (both
   runs read-only). (ii) `<run_root>/compares/` (chosen). (iii) No
   artifact at all — loses the fixed output format the outline froze.
5. **Invocation.** (i) A GET endpoint only — GET-with-writes breaks the
   house GET rule (api.py:1-13). (ii) A POST verb + CLI (chosen). (iii)
   A verb only — the reviewer's gates and the live check want a CLI.

## Review focus

1. Both dirs stay byte-identical across a compare: the `_dir_state` pin
   in the verb test and the CLI test; the artifact lands under
   `<run_root>/compares/` (tests in C2).
2. The first-divergence report is stable: exactly one row carries
   `first: true`, and `first_divergence` names the first
   differs/only-* row in pass-1 emitted order (test in C1).
3. The re-derive path must not read a foreign session: a missing
   decisions.jsonl in a reused interactive dir re-derives from the
   manifest's `start_offset`/`log_rotations` slice — the same scoping
   as the replay — never from the whole log (test in C1 with a
   two-session log).
4. The series cache never serves stale points: keyed by the log's size
   and mtime_ns, at most two entries (test in C3).
5. The UI page rules hold over the new markup: no innerHTML, no inline
   handlers/styles, every `data-verb` served — the existing walks
   cover it once the page is parsed (C4/C5).

---

## Task C1: compare.py — the pure compare, the loaders, [compare]

**Files:** create `scripts/stix/platform/stixplat/compare.py`; modify
`stixplat/config.py` (DEFAULTS `[compare]`, `_check`), `stixplat/
replay.py` (extract `iter_records`), `stixplat/selftest.py`.

**Interfaces — produces:**

- config: `DEFAULTS["compare"] = {"tol_event_s": 1.0,
  "tol_number_rel": 0.05, "tol_number_abs": 1.0,
  "series": ["hk:2:NIXD0501", "truth:instrument.rotb.count",
             "truth:instrument.detector.tape.dropped"]}`.
  `config._check` validates: `tol_event_s` a float with
  0 < x <= 3600; `tol_number_rel` 0 <= x < 1; `tol_number_abs` >= 0;
  `series` a list of strings. (The default series list: the three ids
  the mockup's rows need — raw ids, no IDB dependency; the hk id
  resolves only when the run had an IDB, otherwise the row is
  only-*.)
- `replay.iter_records(run_dir, manifest)` — extracted from
  `load_inputs`' loop (replay.py:136-154): the `_slice`/`_lines`
  iteration yielding parsed, `check_record`-validated record dicts;
  `manifest=None` means the whole log in the viewrun.parts order,
  offset 0. `load_inputs` keeps its behaviour (it now consumes
  `iter_records`); `ReplayError` unchanged.
- `compare.CompareError(Exception)` — a refusal (no bundle at a dir);
  the surfaces map it to exit 2 / 404.
- `load_decisions(run_dir, manifest) -> [decision]`:
  `decisions.jsonl` when present (every line `json.loads`-ed alone,
  broken lines skipped, the viewrun._stream rule viewrun.py:135-140);
  otherwise re-derived: every record of `iter_records(run_dir,
  manifest)` through `bundle.decide(obj, bundle.new_state(hk_codes))`
  with `hk_codes` from the manifest config's `[bundle]` (empty when
  there is no manifest) — the S1 extractor is stateful and must be fed
  in record order exactly as the Writer did (bundle.py:384-420). A
  bundle with decisions but an unreadable log still compares: only the
  re-derive path requires the log (an unreadable log with no
  decisions.jsonl is a refusal, Global constraints).
- `compare(baseline_dir, other_dir, tol=None) -> {"summary", "rows"}` —
  pure, no writes; `tol=None` uses the config defaults as a module
  constant. `tol` = `{"tol_event_s", "tol_number_rel",
  "tol_number_abs", "series"}`. Refusals (`CompareError`): a dir with
  neither manifest nor log, a slice rotated away with no decisions to
  fall back on, a manifest still `open` (the message names the side).
- **Alignment (the anchor rule).** Load both decision lists in seq
  order. Skip decisions from either list until the two heads share the
  same `(kind, key)`; every skipped row becomes a row with result
  `only-baseline`/`only-other` (nothing is dropped silently). The
  first head match is the **anchor**; each side's relative time is
  `t_rel(d) = d["sim_time"] − anchor["sim_time"]` (ns, per side). The
  summary records `align: {"t_a", "t_b"}` — the anchor's absolute
  sim_times, the shift the Timeline overlay applies.
- **Pass 1 (decisions), from the anchor, positionally:**
  - heads with the same `(kind, key)` → one row, check
    `"<class> <key>"` for events (e.g. `mode 0x0021`),
    `"<kind> <key>"` otherwise; a/b = the relative times in s
    (rounded to ms); result `same` iff the details are equal exactly
    (event `{name, params}` — the owner's hex rule for mode rows;
    ack `{subtype, fid}`; hk_step `{value, previous}`) **and**
    `|t_rel_a − t_rel_b| <= tol_event_s`; else `differs`.
  - heads differing in `(kind, key)` → one `differs` row (check
    `"pair <i>: <a kind/key> vs <b kind/key>"`), `first_divergence`
    from the baseline side's decision, and pass 1 stops: everything
    after a key mismatch would pair garbage. The unpaired remainders
    are counted in the summary (`a_tail`, `b_tail`), not enumerated.
  - the first `differs`/only-* row in emitted order carries
    `first: true` (exactly one row); `first_divergence` =
    `{kind, key, t_a, t_b}` — absolute sim_times, `t_b` null for an
    only-* row.
- **Derived rows** (from the full lists, independent of pass 1's
  stopping): `First overrun (hk level 7)` — per side the first
  `hk_step` with key `hk:2:NIXD0501` and `detail.value == 7` → a/b the
  relative times (null when that side has none), result `same` iff
  both present and within `tol_event_s`, else only-*;
  `Overruns in the run` — the counts of such steps per side, result
  `same` iff equal, tol null.
- **Pass 2 (numbers), one row per id in `tol["series"]`:** both sides'
  points from a `SeriesIndex` built by streaming that run's log (the
  same builder C3 serves); a `marker`-kind id is skipped and noted in
  `summary.counts.series_skipped`; an id with points on one side only
  is a only-* row. Otherwise the **value arrays** are compared
  index-wise: a pair matches iff
  `|v_a − v_b| <= tol_number_abs + tol_number_rel * max(|v_a|, |v_b|)`;
  the point counts must match within a slack of `2 + 5%`. Times are
  not compared in pass 2 (sample-tick jitter; the aligned overlay shows
  them). a/b = the point counts; the first mismatching index lands in
  `summary.first_series_divergence = {check, index, t_a, v_a, v_b}`.
- **Summary:** `{"verdict": "same"|"differs"` (differs when any row is
  `differs` or only-*), `"first_divergence"`, 
  `"first_series_divergence"`, `"align"`,
  `"counts": {pairs, a_tail, b_tail, series, series_skipped}}`.
- **Rows:** `{check, a, b, result, tol, first}` exactly as R7 froze
  (a/b numbers or null; tol the time or number tolerance).

- [ ] Self-test `compare_core` (RED: the module does not exist).
  Hand-written bundles under `env.tmp` — a minimal manifest
  `{"v": 1, "run_id", "open": false, "t0", "length_s",
  "start_offset": 0, "log_rotations": 0, "config": {"bundle":
  {"hk_codes": [...]}}}`, `decisions.jsonl` lines built with the
  `_rec` shapes of `t_bundle_decisions`, and a `log.jsonl` holding the
  deciding records for the re-derive/series paths:
  identical bundles → verdict `same`, no differs row;
  one event's sim_time moved 2 s → `differs` and `first_divergence` is
  `{kind: "event", key: "0x0021", ...}` with that row `first`;
  a mode event with params `"02"` vs `"03"` (times equal) → `differs`
  (the hex rule);
  an `hk_step` value 5 vs 6 → `differs` (exact value) even within
  `tol_event_s`;
  a leading decision only on the baseline (a 0x142E keepalive lapse
  before boot) → one `only-baseline` row, pairing starts at the next
  common key, `align` is that anchor;
  heads that never match → `differs`, both tails counted, exactly one
  `first` row;
  a missing decisions.jsonl → the re-derived list equals the
  Writer-produced one (compare a `t_bundle_writer_lifecycle`-style
  bundle against a copy whose decisions.jsonl was deleted);
  a two-session log with a manifest scoped by `start_offset` → the
  re-derive sees only this session's decisions;
  no manifest at all (log only) → the whole log is the slice, the
  verdict is unchanged;
  the overrun rows: an NIXD0501 step to 7 on one side only →
  `only-*`; steps on both sides at aligned times → `same`;
  a series id in `tol["series"]` with one perturbed point → `differs`
  + `first_series_divergence`;
  a manifest with `"open": true` → `CompareError` naming the side;
  `_dir_state` unchanged on both dirs after every call.
- [ ] Implement.
- [ ] `cd scripts/stix/platform && python3 stixplat.py --self-test`
  → PASS.
- [ ] Commit.

**Verification:** `compare_core` PASS; `t_bundle_*`/`t_replay_*`
unchanged (the `iter_records` extraction keeps `load_inputs`
behaviour).

## Task C2: the surfaces — run.compare, the artifact, --compare

**Files:** `stixplat/compare.py` (`run_compare`, the artifact writer,
`EXIT`); modify `stixplat/core.py` (`_register`), `stixplat/verbs.py`
(VERBS), `stixplat/bundle.py` (`_EXCLUDED_VERB_WHY`),
`stixplat.py` (`--compare`), `docs/stix-port/platform.md` (the
verbs-table row only — `t_verbs_doc` needs it in the same commit),
`stixplat/selftest.py`.

**Interfaces — produces:**

- `compare.EXIT = {"ok": 0, "refused": 2, "infra": 3}` (the runmode
  convention, runmode.py:13-14).
- `run_compare(cfg, a_id, b_id, live_dir=None, out=print) -> dict`:
  `bundle.check_run_id` + `bundle._resolve` both ids (400/404, the
  name rule before any filesystem access), `compare(...)` with
  `cfg["compare"]`, then the artifact:
  `<cfg["retention"]["run_root"]>/compares/<a>-vs-<b>.jsonl` (one row
  per line) and `.json` (`{"summary": ..., "rows": [...]}` — one file
  a human or the board can read back), both written tmp + os.replace
  (the `_write_json` pattern, bundle.py:279-284). Never into either
  run dir. Returns `{"summary", "rows", "artifact"}`.
- Verb `run.compare {a, b}` (strings, required) in `core._register`
  **and** `verbs.VERBS` (the registry the self-test checks against a
  live dispatcher, selftest.py:2034-2036, and run mode validates
  scenarios against) and one `_EXCLUDED_VERB_WHY` entry
  `"run.compare": "controller management, not a run input"`
  (bundle.py:49-76). 403 in a `--view-run` server by the existing
  rule (viewrun.py:186-190).
- CLI: `stixplat.py --config FILE --compare BASELINE OTHER`
  (`nargs=2`); cannot combine with `--scenario`, `--variant`,
  `--view-run`, `--replay`, `--self-test`, `--check-ui` (the
  `ap.error` rule, stixplat.py:76-88). Prints the verdict, the
  per-check rows and the artifact path; exit 0 on a completed compare
  whichever the verdict (owner question 1), 2 refused (a dir with no
  manifest and no log — `CompareError`), 3 infra.

- [ ] Self-test `compare_verb` (RED): two hand-written bundles
  registered under a fake run root (the `t_bundle_api` pattern,
  selftest.py:6589-6656): `run.compare {a, b}` → 200 with the pair's
  rows and summary; the artifact files exist under
  `<run_root>/compares/` with those rows; `_dir_state` of both run
  dirs identical before/after; an unknown id → 404; an `a` with a
  slash → 400; the verb is in `verbs_mod.VERBS` and served; a
  `run.compare` cmd record classifies `replay: "no"` with the new why.
- [ ] Self-test `compare_cli` (RED): `compare.run_compare` against the
  same fixture (no controller needed): a healthy pair → 0 and the
  printed verdict; a perturbed copy with verdict "differs" → still 0
  (owner question 1a); a nonexistent dir → 2 with a message naming the
  path.
- [ ] Implement.
- [ ] `python3 stixplat.py --self-test` → PASS (the platform.md
  verbs-table row goes in with the verb, or `t_verbs_doc` fails).
- [ ] Commit.

**Verification:** `compare_verb` and `compare_cli` PASS;
`t_bundle_api`, `t_replay_cli` and `t_verbs_doc` PASS.

## Task C3: the overlay's data path — GET /runs/<run_id>/series

The Timeline overlay must draw the *other* run's series inside the
page the controller already serves (`connect-src 'self'` — a
`run.open` viewer on another port is a different origin and the page
cannot fetch it). So the controller itself serves a finished run's
series on its own origin, read-only.

**Files:** `stixplat/compare.py` (`SeriesCache`);
modify `stixplat/api.py` (the route; the `/series` param parsing
shared), `stixplat/core.py` (one attribute), `stixplat/selftest.py`.

**Interfaces — produces:**

- `compare.SeriesCache`: `{run_id: (key, SeriesIndex)}` with at most
  two entries; the key is the resolved dir's `log.jsonl` + gz parts'
  `(total size, max mtime_ns)` — a changed log rebuilds. Methods:
  `get(cfg, run_id, live_dir=None)` (resolve via `bundle._resolve`,
  build with the same `_series_index(run_dir, manifest)` builder C1's
  pass 2 uses: every `replay.iter_records` record — manifest or
  whole-log — fed into `series_mod.SeriesIndex()`, the
  viewrun._stream pattern without the record log) and
  `query(run_id, ids, t_from, t_to, max_points)` answering exactly
  like `SeriesIndex.query` (series.py:300-314).
- `core.Controller.__init__`: `self.run_series_cache =
  compare_mod.SeriesCache()` next to `self.viewers` (core.py:71). A
  `--view-run` server has none: the handler falls back to an uncached
  build per request (`getattr(ctl, "run_series_cache", None)`;
  noted in the docs).
- api.py: in the `/runs/` branch (api.py:217-227), before the
  `/manifest` suffix check, `rest.endswith("/series")` →
  `GET /api/v1/runs/<run_id>/series?ids=..&ids=..&from=&to=&max_points=`
  with the same param rules as `/series` (ids required and non-empty,
  max_points 10..5000 — extract the parsing of api.py:262-280 into a
  helper both routes share). Unknown run id → 404; bad ids → 400 (the
  same messages). Works on a `--view-run` server too (GET).

- [ ] Self-test `compare_series_api` (RED): a finished synthetic
  bundle with `sample` records (the `t_view_run` pattern,
  selftest.py:6066-6094): `GET /api/v1/runs/<id>/series?ids=
  truth:instrument.rotb.count` → the same body shape as `/series`
  with all points; an unknown run id → 404; an empty `ids=` → 400;
  appending a record to the run's log (bumping mtime) then re-querying
  shows the new point (the cache rebuilt); `_dir_state` of the run
  dir unchanged throughout.
- [ ] Implement.
- [ ] `python3 stixplat.py --self-test` → PASS.
- [ ] Commit.

**Verification:** `compare_series_api` PASS; `t_view_run`,
`t_series_api_*` unchanged.

## Task C4: the Runs board Compare panel (UI 1)

**Files:** `stixplat/ui/index.html`; `stixplat/selftest.py`. (UI is
its own task, per the S3 brief; it needs C2's verb and nothing else.)

- [ ] `bundle._entry` gains `"kind": m.get("kind")` and
  `"of": m.get("of")` (bundle.py:611-624, additive — readers ignore
  unknown fields), so the board can label a replay row: the tags cell
  shows `replay of <of>` when `kind == "replay"`.
- [ ] Per-row action `compare` (`data-verb="run.compare"`,
  `data-args = {a: <the baseline row's run_id>, b: <this row's
  run_id>}`), rendered only while some row is the baseline and the row
  itself is not it; with no baseline the actions cell keeps
  baseline/open and `#runs-note` explains "mark a baseline first
  (run.baseline)". The response renders into a results section under
  the table: a caption `<a> vs <b> · verdict`, the count line
  (pairs, tails, series), and the four-column table
  **Check | Baseline | Replay | Result** — the owner-fixed layout
  (Runs.dc.html:116) — built with createElement/textContent (the
  runs-row pattern, index.html:1185-1235). Result cells get a class
  (`same` muted, `differs` the error colour, only-* the warning
  colour) — CSS classes, never inline styles; the `first` row is
  bolded via a class; the first-divergence line under the table reads
  `event 0x0021 at +0.00 s vs +0.31 s`. A "show on Timeline" button
  (plain JS, no data-verb) switches to `#timeline` with the pair
  stored for C5's overlay (`TL.overlayPend = {a, b}`).
- [ ] No new CSP needs; no new GET endpoint used by the panel.
- [ ] Self-test `UI: compare panel (S3)` (RED; the `t_ui_runs`
  pattern, selftest.py:7586-7622, no browser): the page's data-verb
  set gains `run.compare` and every data-verb is still a served verb;
  the new ids (`runs-compare`, `runs-compare-table`) exist;
  `UI_RUNS_VERBS` gains `run.compare`; the no-innerHTML / no inline
  handler / no style-attribute walks still pass; the page still names
  no new GET beyond `/runs` (the panel uses the verb response).
- [ ] `python3 stixplat.py --self-test` → PASS.
- [ ] Commit.

**Verification:** the new UI case PASS; `t_ui_runs`, `t_ui_page`,
`t_ui_tabs` unchanged-PASS.

## Task C5: the Timeline two-run overlay (UI 2)

**Files:** `stixplat/ui/index.html`; `stixplat/selftest.py`. (Needs
C3's endpoint and C2's verb response for the shift.)

- [ ] A Timeline control next to the view/window selectors:
  `<select id="tl-overlay">` — `none` plus one option per
  `GET /runs` entry (`<name>`; a replay row `↻ <name>`), fetched on
  first Timeline visit and on the reload button. The selected run's
  series are fetched once per (run, ids) change with
  `GET runs/<id>/series?ids=…&max_points=2000` (the `tlQuery`
  discipline: a request counter supersedes stale answers,
  index.html:2558-2591) into `TL.overlay = {runId, data, shift,
  divergence}`.
- [ ] The shift: `shift = align.t_a − align.t_b` (ns) taken from the
  run.compare result the page holds (`TL.overlayPend` from C4's
  "show on Timeline", or the panel re-POSTs `run.compare {a, b}` when
  the user picks an overlay without one; in a `--view-run` server that
  POST is a 403 — toast it and overlay unshifted, `shift = 0`). The
  second run's points draw at `t + shift` so the two runs' anchors
  coincide.
- [ ] Drawing: `tlSeries` gains a style parameter — overlay points
  draw dashed (`setLineDash([4, 3])`) in the same palette colour, in
  every panel whose series the overlay shares (the smallest change to
  the per-series renderer, index.html:2919; the hover ignores overlay
  points in v1). The first divergence is a vertical line across the
  panels at `first_divergence.t_a` (or
  `first_series_divergence.t_a`), in the fault colour, with a
  "first divergence" tick label in the axis legend line (the
  `tlMarks` loop, index.html:3046-3054).
- [ ] The overlay clears when the select returns to `none` or the
  panel loses the series; `tlDraw`/`tlTrimData` untouched otherwise.
- [ ] Self-test `UI: timeline overlay (S3)` (RED; the `t_ui_timeline`
  pattern, selftest.py:4105-4139): the id `tl-overlay` exists; the
  page's JS names the `runs/` series path (a string check, like the
  board verb checks); `UI_TIMELINE_IDS` gains `tl-overlay`; the
  existing page walks still pass.
- [ ] `python3 stixplat.py --self-test` → PASS.
- [ ] Commit.

**Verification:** the new UI case PASS; `t_ui_timeline`,
`t_ui_tabs` unchanged-PASS.

## Task C6: docs + the reviewer live check

**Files:** modify `docs/stix-port/platform.md`. **No new
accepted-diffs section** — S3 adds no QEMU or model behaviour and no
existing controller guarantee (the S2 precedent, replay plan P4); the
rows join the existing verbs/API tables.

- [ ] platform.md:
  - A "Comparing two runs (S3)" subsection under Run bundles: the two
    passes and their tolerances (the `[compare]` keys); the alignment
    rule (anchor at the first common `(kind, key)`; times relative to
    each side's anchor; why — it removes the replay's launch-latency
    offset that the S2 live check's 0.44 s still contains); the
    derived overrun rows (from the `hk_step` NIXD0501 == 7 steps and
    the `instrument.rotb.*` / dropped-gauge series — the mockup's
    naming; the level semantics are owner question 4); `run.compare`
    and the artifact path; `--compare` with its exit codes;
    `GET /runs/<run_id>/series`; the read-only guarantee; the limits
    (pass 2 compares value arrays index-wise with a count slack, times
    not compared; `seq`/`boot_id` never compared; pass 1 stops at the
    first key mismatch and counts the tails; a missing
    decisions.jsonl is re-derived from the manifest's log slice).
  - The verbs table row for `run.compare` (between the
    `<!-- verbs:begin/end -->` markers, `t_verbs_doc` exact equality,
    selftest.py:3788) landed with C2; this task adds the prose, the
    API endpoint table row for `GET /runs/<run_id>/series`, and the
    live check.
  - The Runs board paragraph (index.html's tab description at
    platform.md:1117 area) gains the Compare panel sentence.
- [ ] Reviewer live check (never ports 8080/21502/21511/21512;
  reuses the S2 recipe, platform.md:709-717):
  1. Record + replay exactly the S2 live-check run
     (`fault.inject {id: obt.jump, params: {delta_s: 4}}`,
     `tc.gs.send` of TC(237,18), ~30 s, icount off, `pace = "wall"`,
     API 18081/18082, run roots under /tmp/stix-plat).
  2. Compare baseline vs replay — the board's compare button or
     `--compare`: expect verdict **"same"** (the same 7 (kind, key)
     decisions; the aligned deltas well inside `tol_event_s = 1.0` —
     the S2 measurement 0.44 s was on absolute times and includes the
     offset the anchor removes).
  3. Perturb: `cp -a` the origin dir to a scratch copy, edit the
     copy's log.jsonl fault record `delta_s 4 → 40` (a string replace)
     and delete the copy's decisions.jsonl (forcing the re-derive);
     replay the copy; compare copy-replay vs the origin → verdict
     **"differs"** with a non-null `first_divergence` whose (kind,
     key) sits after the fault's seq (the longer OBT jump moves the
     first post-fault decision).
  4. The board: the Compare panel renders the rows; the Timeline
     overlay draws the replay dashed over the baseline with the
     divergence marker at the reported row.
- [ ] Commit.

**Verification:** the reviewer runs steps 1-4 and notes the measured
aligned deltas in the PR description (the plan's `tol_event_s = 1.0`
is informed by the S2 measurement, not a bound — UNVERIFIED until this
run).

## Merge order

C1 → C2 → C3 → C4 → C5 → C6. Each task: its new self-test cases RED on
the parent (the module/verb/endpoint missing), the whole self-test
green after. C2 (core.py/verbs.py) and C3 (api.py) also run
`gates2.sh`, like R2/R3 (the run-bundles plan's merge order). C4 lands
without C5 (the panel works, no overlay); C5 needs C3. The
verbs-table row for `run.compare` lands with C2 (`t_verbs_doc`), the
prose with C6.

## Deviations from the R7 outline (deliberate)

- **D-a.** "Align at the first common decision" is sharpened into the
  anchor rule: leading one-sided rows are reported as only-* rows, not
  dropped silently, and the time base is each side's anchor sim_time
  (the outline left the time base open; Approaches 1).
- **D-b.** Pass 1 stops pairing at the first `(kind, key)` mismatch;
  the remainders are tail counts in the summary. The outline's
  row-per-check stream would pair garbage after a key mismatch.
- **D-c.** The summary gains `align` and `first_series_divergence`
  (readers ignore unknown keys; the overlay needs the shift, the
  panel the series marker). The frozen row shape
  `{check, a, b, result, tol, first}` is unchanged.
- **D-d.** The outline's `as.dropped` series is not a code path; the
  drop gauges are `instrument.detector.tape.dropped` /
  `instrument.detector.replay.dropped` (state.py:28-33) — the default
  `[compare] series` uses the real ids.
- **D-e.** The artifact lives under `<run_root>/compares/` (the
  outline says only "compare.jsonl"; both runs stay read-only, so no
  run dir can hold it).
- **D-f.** Pass 2 compares value arrays index-wise with a count
  slack; times not compared (the outline's "series points within
  per-id tolerances" left the pairing open; Approaches 3).
- **D-g.** The re-derive reads the log through
  `replay.iter_records` — extracted from `load_inputs` so the slice
  scoping (`start_offset`/`log_rotations`) is shared, not duplicated.

## Open questions for the owner

1. **`--compare`'s exit code when the verdict is "differs".**
   (a) 0 either way — the verdict is data, like the replay's "no fail
   verdict" (recommended; a gate can add `--fail-on-differs` later
   without a format change); (b) exit 1 on differs (run mode's
   "a scenario failed" convention, runmode.py:13-14) — friendlier to
   CI, but a compare that *reports* a difference has not failed.
2. **The artifact's lifetime.** (a) One file pair per comparison,
   overwritten on each compare of the same pair —
   `<run_root>/compares/<a>-vs-<b>.{jsonl,json}`, latest wins
   (recommended — the path is derivable from the two run ids, no dir
   growth, the board always reads the current verdict); (b) a stamped
   subdir per compare (full history, but the board must resolve the
   newest and the dir grows with every click).
3. **Pass 2's pairing.** (a) Index-wise value comparison with a count
   slack of `2 + 5%`, times ignored (recommended — smallest viable,
   robust to sample-tick jitter; a dropped mid-run sample flags the
   row `differs`, an accepted v1 limit stated in platform.md);
   (b) a nearest-time join within a sample period (robust to dropped
   samples, meaningfully more code and a new tolerance knob).
4. **The overrun rows' level.** The mockup names "HK level 7"; the FSW
   constant behind it is not verifiable from this repo (the
   RotAccumulator.c/HKAcquire.c facts in the S1 plan's Key facts were
   verified elsewhere; this plan may not open that tree). (a) Ship v1
   with `detail.value == 7` exactly as the outline inlines, the row
   named after the mockup, and revisit if the owner corrects it
   (recommended — the rows are derived, not stored; a correction is a
   one-line change); (b) hold the two overrun rows out of v1 until the
   owner confirms the level/flag semantics from TMData.h.
5. **The Compare panel's second column header** stays "Replay"
   (Runs.dc.html:116) even when the compared run is a plain run, not a
   replay. (a) Keep the owner-fixed four columns and let the caption
   name the two run ids (recommended — the layout is the owner's
   decision); (b) relabel the column to "Other" when
   `kind != "replay"` (honest, but departs from the fixed mockup).

## Plan review (2026-11-16, GLM + reviewer; these override the text above)

- **R1 (major) — pairing by key within a window, not by position.**
  Pass 1's positional pairing stops at the first `(kind, key)`
  mismatch, and the anchor rule has no advance policy when the lists
  hold each other's keys (a=[A,B], b=[B,A]). Near-simultaneous
  decisions can swap order between runs (the S2 live check had 0x0032
  and 0x8030 both ~0.10 s late), and keys repeat (two `ack
  0x05AC/0`, per-code `hk_step`s). Replace both rules:
  - anchor = the earliest baseline decision whose `(kind, key)`
    occurs in the other list; the other side's anchor is its *first*
    occurrence of that key;
  - pass 1 walks the baseline in order and matches each decision to
    the earliest unmatched other decision with the same `(kind, key)`
    whose anchor-relative time is within `tol_event_s` — `same` when
    the details are equal, `differs` when not; a baseline decision with
    no match is `only-baseline`; unmatched other decisions are
    `only-other`, emitted in time order. Repeated keys pair in order.
    No early stop; D-b is withdrawn, `a_tail`/`b_tail` dropped.
  - `first_divergence` = the earliest (anchor-relative) non-`same` row.
  Self-tests: a swapped near-simultaneous pair → `same`; a repeated
  ack key pairs in order; a=[A,B] vs b=[B,A] beyond tol → defined
  rows.
- **R2 (minor) — pre-anchor one-sided rows** count toward the verdict
  and can be `first` (owner question 6).
- **R3 (minor)** — decisions.jsonl seqs are non-decreasing, not
  strictly increasing (one HK report changing two watched codes writes
  two `hk_step` lines with one seq, bundle.py:261-274). Nothing may
  rely on strictness.
- **R4 (minor) — empty and enum series.** An id with no points on
  either side, and an enum-kind id (first-seen indices,
  series.py:255-266), produce no row; they are counted in
  `summary.counts.series_skipped` like markers. C6's live check sets
  `[timeline] sample_s` so pass 2 has data.
- **R5 (minor)** — platform.md limits: one global anchor shift cannot
  absorb differing wall-paced off-times (D-PLr), so decisions after a
  power cycle may drift beyond `tol_event_s` with no real divergence.
- **R6 (minor)** — the overlay is offered only when the page shows the
  compare's baseline (`run.open` of it, or `--view-run`); on the live
  controller's Timeline the button is absent.
- **R7 (minor)** — D-h: the Compare panel renders the `run.compare`
  response; the artifact is for later reading (S3 tooling, CI), not
  the panel's source.
- **R8/R9 (notes)** — `--compare A B` and `run.compare` take run ids;
  a still-open bundle is a 409 on the verb (exit 2 on the CLI), and
  the panel does not offer compare for an open run (the live row).
- **R10 (note) — split.** Pass 2 and the series-index builder move from
  C1 to C3 (which owns the series path); C1 is decisions only.
- **R11 (note) — defer `SeriesCache`.** v1 builds the series index per
  request (the `--view-run` fallback already does).
- **R13 (note)** — C6 step 3's perturbation edit must match the log's
  compact separators (`"delta_s":4` → `"delta_s":40`).

Open question 6 (from R2): **pre-anchor one-sided decisions.**
(a) Report them as only-* rows that count toward the verdict and may
be `first` (recommended — nothing is hidden; a replay that misses a
leading decision really differs); (b) report them but exclude them from
the verdict and from `first` (friendlier when the baseline started its
bench earlier, but a real early divergence would then hide).
