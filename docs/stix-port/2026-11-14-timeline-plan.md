# Timeline view + tabbed page — implementation plan (2026-11-14)

> For agentic workers: implement task by task; steps use `- [ ]`.
> Workers cannot build or run anything: write code + self-test cases,
> the reviewer runs them and commits.

**Goal:** the platform web UI gets the mockup's tab structure (Overview,
subsystem tabs, Setup) and a Timeline tab that plots truth, decoded HK
and markers on one bench clock, live or from a finished run.

**Architecture:** Python additions in `scripts/stix/platform/stixplat`
(sampler, IDB HK decoder, series index, views, read-only run viewer),
all API v1 additive; the one-file page `ui/index.html` gets a tab bar
and a canvas plotter written by us.

**Spec:** `docs/stix-port/2026-11-14-timeline-design.md` (approved by
the owner 2026-11-14, "go ahead with all that you can"), its "Calls
made" section included. Mockup boards 1, 2, 2d, 4, 8 of
https://claude.ai/artifact/V9rbLbVVcfwFJjnoiKciqJ for the layout.

## Global constraints

- `index.html`: one file, plain JS, no build, no CDN, no `innerHTML`,
  no inline handlers, no `style=` attributes; bench text only as
  `textContent`; every `data-verb` must be served (uicheck.py).
- API v1 additions only; no existing field, path or verb changes.
- Record schema v1: new kind `sample` and topic `sample` are additions
  to `log.KINDS` / `log.TOPICS`; `check_record` keeps working.
- Self-test: `cd scripts/stix/platform && python3 stixplat.py --self-test`
  must stay PASS; every task adds its own `check(...)` cases in
  `stixplat/selftest.py`.
- No FSW edits; no file under ~/dev/stix-doc-notes/sources cited.
- Sampler must not change guest behaviour: it reads the state tree the
  poller already fills; it issues no QMP of its own.

## Review focus

1. A run without the IDB JSON: HK series absent, page says so, nothing
   raises (test in A2).
2. A long run (log rotated into `log.jsonl.N.gz`): `/series` over
   "whole run" still answers, bounded memory (test in A4).
3. Enum/bool leaves (link up/down, timesync verdict) plotted as steps
   with the string in the hover, not dropped (test in A1/A4).
4. View names from the API are file names: path traversal, empty,
   overlong, non-ASCII refused with 400 before any file access (A5).
5. `--view-run` refuses every non-`view.*` verb with 403 and appends
   nothing to the run's log (A6).

---

## Track A — controller (worktree qemu-stix-pl11, branch stix-uprev-pl11)

### Task A1: sampler

**Files:** create `stixplat/timeline.py`; modify `config.py`
(DEFAULTS `timeline`), `log.py` (KINDS += "sample", TOPICS += "sample",
`_TOPIC` maps sample->sample), `core.py` (start/stop the sampler),
`selftest.py`.

**Interfaces — produces:**
- config `[timeline]`: `sample_s` (float, default `None` = 1.0 in
  interactive, 0 in run mode; 0 = off; range 0 or 0.1..60),
  `sample_paths` (list of fnmatch globs, default
  `["instrument.rotb.*", "instrument.detector.*cps",
  "instrument.detector.flare", "idpu.watchdog.count",
  "idpu.spw.counters.*", "instrument.memory.*", "platform.keepalive.*",
  "platform.timesync.verdict", "platform.power", "idpu.spw.link_down"]`),
  `idb_json` (str, default
  `~/dev/ATS-GroundStation-stix/definitions/telemetry/stix_idb_tlm.json`),
  `idb_sqlite` (str, default ""), `views_dir` (str, default "" =
  `<retention.run_root>/views`).
- `class Sampler(clock, state, log, paths, period_s)` with `start()`,
  `stop()`; on each bench-time tick (via `clock.after`, like keepalive)
  emits one record `("ctl", "sample", "", {path: value})` holding
  `truth` per matched leaf, plus `"<path>@presented": value` only
  where presented != truth. Values: int/float as-is, bool as-is,
  str as-is, others skipped. No record when power is off / tree empty.

- [ ] Self-test `sampler_record_shape`: fake StateTree with
  `instrument.rotb.count=5`, `instrument.rotb.full=False`,
  `platform.timesync.verdict` (truth "ok", a string); assert one `sample` record per tick, keys exactly the matched
  paths, `check_record` passes, topic == "sample".
- [ ] Self-test `sampler_off_in_run_mode`: mode run + sample_s unset
  -> no Sampler started; mode run + sample_s 2.0 -> started.
- [ ] Implement; config `_check` validates the range and types.

### Task A2: IDB HK decoder

**Files:** create `stixplat/idb.py`; modify `decode.py` (optional hook),
the TM relay path that logs `tm` records (find where `decode.decode`
result goes into the `tm` record data, `relay.py`/`plat.py`), `selftest.py`.

**Interfaces — produces:**
- `idb.load(json_path, sqlite_path="") -> Idb | None` (None + one
  `log` record `timeline.idb` with the reason when missing/broken).
- `Idb.hk_fields(sid) -> [Field]`, `Field(code, name, unit, raw: bool)`;
  `code` is the IDB parameter code from the name's `(NIX.....)`.
- `Idb.decode_hk(sid, app: bytes) -> {code: value}`.
- `tm` records of APID 0x5A4 TM(3,25) SID 1/2 carry
  `data.hk = {code: value}` when an Idb is loaded.
- Aliases (readable ids) in `idb.ALIASES`: `hk.cpu_load`,
  `hk.rb_level` (NIXD0501), `hk.trig_med`, `hk.trig_max`,
  `hk.archive_usage` (NIXD0003), `hk.mode` — resolve the codes for
  CPU load and the trigger fields from the JSON by name.

**Pinned semantics to establish first:** the JSON's `byte_offset` base
(packet start, data field, or app data) and its bit numbering
(`bit_offset` 0 = MSB or LSB). Decide them from the known RB level:
FSW `TM_3.c` packs it into status4 bits 31..29 at app byte 62 bits 7..5
while the JSON says byte 62, bit_offset 0, bit_length 3 — so bit_offset
counts from the MSB and byte_offset is relative to the app data
(SP[16]). Confirm with one more field (mode, NIXD0001?) against
`decode.py`'s `app[1] & 0x0F`. Write both rules in the module
docstring with the evidence.

- [ ] Self-test `idb_rb_level`: a synthetic SID 2 app buffer with
  byte 62 = 0b101_00000 -> `decode_hk(2, app)["NIXD0501"] == 5`.
- [ ] Self-test `idb_mode_matches_minimal_decoder`: random app bytes,
  the IDB mode field == `app[1] & 0x0F` (or the documented relation).
- [ ] Self-test `idb_missing`: `idb.load("/nonexistent")` is None, the
  relay logs `tm` records without `hk`, no exception.
- [ ] Self-test `idb_real_json` (skips with a PASS note if the GS repo
  is absent): load the real JSON, SID 1 and SID 2 have > 20 fields,
  every alias resolves.
- [ ] Calibration from sqlite: optional; if `idb_sqlite` set and the
  curve is a linear/polynomial in the IDB, apply it and drop "raw";
  otherwise unit reads "raw". (Builder may ship raw-only and leave a
  TODO in the docstring if the sqlite schema is unclear.)

### Task A3: TC(237,18) labels

**Files:** where `tc` records are emitted for `tc.gs.send` and relayed
GS TCs (`plat.py v_tc_gs`, `relay.py`); `selftest.py`.

- [ ] For service 237 subtype 18 add `data.param = {"index": u16,
  "sub": u8, "value": u32}` decoded from the app data (PIX00016 u16,
  PIX00017 u8, PIX00018 u32, big-endian; several sets if N > 1 —
  read the TC layout in FSW `TC_237.c` via
  `git -C /home/laszlo/dev/STIX-FSW grep -n` to confirm the count
  field). Self-test `tc_237_18_label` with a built unit -> 
  `param == {"index": 312, "sub": 0, "value": 0}`.

### Task A4: series index + API

**Files:** create `stixplat/series.py`; modify `api.py` (GET routes),
`core.py` (own a `SeriesIndex`, feed it from `log.emit` via a listener
list added to `RecordLog`: `log.listeners.append(fn)` called after the
ring append, outside nothing else — keep it cheap), `selftest.py`.

**Interfaces — produces:**
- `SeriesIndex(max_points_per_series=200_000)`, `feed(rec_obj)`,
  `catalogue() -> [{"id", "label", "unit", "source", "kind":
  "number"|"bool"|"enum"|"marker", "values": [enum strings]?}]`,
  `query(ids, t_from, t_to, max_points) -> {id: {"t": [...], "v":
  [...], "min": [...]?, "max": [...]?, "labels": [...]?}}`.
- Ids: `truth:<path>`, `presented:<path>` (from `sample` and `state`
  records), `hk:<sid>:<code>` + aliases (from `tm` records with
  `data.hk`), markers `fault`, `force`, `tc`, `event`, `mode`,
  `power`, `scenario` (from fault/force/tc/tm(5,x)/power/expect
  records; marker points carry `label` text, e.g. `TC(237,18) param
  312 = 0`, `EID 0x0021 mode change`).
- Decimation: when a window has > max_points samples, buckets of equal
  time, each bucket returns first t, and min/max/last; step series
  keep every change point up to max_points then bucket.
- Enum: value -> index in order first seen; `labels` lists strings.
- Over-cap: oldest points dropped per series; `catalogue` notes
  `"truncated_before": t`.
- `GET /api/v1/series/catalogue`, `GET /api/v1/series?ids=..&ids=..
  &from=&to=&max_points=` (ids repeated; max_points 10..5000, default
  1000; 400 on bad input; unknown id -> empty series, listed in
  `"unknown"`).

- [ ] Self-tests: `series_decimate_keeps_spikes` (10000 points, one
  spike, max_points 100 -> the spike value is in `max`),
  `series_enum_steps` (verdict none->ok->late_apply -> v [0,1,2],
  labels), `series_markers` (fault inject + tc 237,18 -> marker labels),
  `series_cap` (cap 1000, feed 1500 -> 1000 kept, truncated_before set),
  `series_api_bad_input` (max_points 0 -> 400), `series_api_ids_repeat` (two `ids=` params -> both served; api `_csv` is not used for ids).

### Task A5: views

**Files:** create `scripts/stix/platform/views/{rotb-fill-vs-faults,
cpu-and-hk,memory-upsets,power-and-link}.json`; `stixplat/views.py`;
`api.py`; verbs registered in `core.py` (`view.save`, `view.delete`);
`selftest.py`.

**Interfaces — produces:**
- View JSON: `{"name", "window": "live"|"1m"|"10m"|"60m"|"run",
  "markers": [kinds], "panels": [{"title", "series": [ids]}]}`.
- `views.validate(view) -> view` (raises VerbError 400), name rule
  `^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$` checked before any file access.
- Starter views copied into views_dir only when missing.
- "everything" is generated (`GET /api/v1/views/everything`): one panel
  per leaf group (path up to the second dot) from the catalogue, all
  markers; it cannot be saved over or deleted (409).
- `GET /api/v1/views` -> `{"views": [names], "dir": d}`;
  `GET /api/v1/views/<name>`; verbs `view.save {name, view}`,
  `view.delete {name}`.

- [ ] Self-tests: round trip, `../x` and `a/b` and "" -> 400, starter
  copy-only-when-missing, "everything" undeletable.

### Task A6: `--view-run`

**Files:** `stixplat.py` (flag `--view-run DIR [--port N]`, default
8081), `stixplat/viewrun.py`, `selftest.py`, `docs/stix-port/platform.md`
(new "Timeline" section: sampler, series, views, view-run, IDB).

- [ ] Reads `log.jsonl.N.gz` (oldest first) then `log.jsonl` into a
  SeriesIndex and a RecordLog with no file (ring sized to hold them or
  the last 20000); serves the same API + page; `/status` has
  `"view_run": DIR`; every verb except `view.*` -> 403; nothing
  written into DIR.
- [ ] Self-tests on a synthetic run dir with one rotated part.

## Track B — page tabs + Setup (worktree qemu-stix-ui, branch stix-uprev-ui; parallel to Track A)

### Task B1: tab bar

**Files:** `ui/index.html`, `stixplat/selftest.py` (page rules), maybe
`uicheck.py`.

- [ ] Tabs (buttons with `data-tab`, sections with `data-tab-panel`;
  state in `location.hash`, default `#overview`): **Overview**,
  **Platform** (power/sim, SpW + time codes, time sync/keep-alive/OBT),
  **IDPU & FSW** (TC send, "any verb", UART), **Detectors** (new, B3),
  **Faults & forcings**, **Scenarios**, **Logs** (events), **State**,
  **Setup** (B2), **Timeline** (placeholder text until Track C).
  Every existing section moves unchanged into one tab; no control lost
  (self-test: the set of `data-verb` values before == after, plus the
  new ones).
- [ ] Overview: the header chips stay global; the Overview panel shows
  per-subsystem cards (Platform, IDPU & FSW, Detectors, Faults) each
  with a health word from existing state (power, link, mode,
  active faults count, ROTB fill truth %) and a "open" button
  switching tab; plus the last 10 events (TM(5,x)) list.

### Task B2: Setup tab

**Files:** `core.py` (`status()["setup"]`, additive), `ui/index.html`,
`selftest.py`, `docs/stix-port/platform.md`.

- [ ] `status()["setup"] = {"binary", "elf", "elf_sha256" (computed
  once at start, None if unreadable), "icount_shift", "pace",
  "memory", "timecode_source", "obt_continuity", "on_guest_reset",
  "sample_s"}`.
- [ ] The tab shows them in "what it means" words (board 8): icount 0
  = "host speed, not timed"; 7 = "AX-calibrated (ROTB 0.1 s overrun
  cycle ~230 s, matches the AX)"; 8 = "slower than the AX"; 3 =
  "fast functional"; pace wall/fast with the measured speeds table
  from `2026-11-14-fast-forward-plan.md` Task 6 and the pace=fast
  limit from platform.md (Pacing). Read-only: changing them is a
  restart (`next launch`), shown as text, no controls.
- [ ] Skip ahead: `det.rotb.burst` (frames input 1..104448/49),
  `platform.flash.save {name}`, `platform.flash.load {name}`,
  `platform.flash.list` result shown as a table (refresh button).

### Task B3: Detectors tab

- [ ] Shows `instrument.detector.*` (source, flare, quiet_cps,
  flare_cps, quarter.0..3, fsm.*, replay.*) and `instrument.rotb.*`
  with a fill bar (count / capacity) from `/state` + SSE `state`
  topic; burst control duplicated here. No new verbs (stimulus
  controls need detector-model work: open question).

## Track C — Timeline page (after A and B are merged; worktree qemu-stix-ui rebased)

### Task C1: plotter + Timeline tab

**Files:** `ui/index.html`, `selftest.py` (page rules).

- [ ] View picker (`/views` + "everything"), window picker (live, 1, 10,
  60 min, whole run), series picker (search over `/series/catalogue`),
  panels stacked on one time axis, canvas drawing by our own code:
  lines for numbers, steps for bool/enum/levels, min/max band when
  decimated, a second y-axis per panel when units differ, markers as
  vertical lines across all panels coloured by kind with hover label,
  hover readout of every series at the cursor, drag to zoom,
  double-click to reset, "save view as" (`view.save`), delete.
- [ ] Live: SSE topics `sample,tm,tc,cmd,fault,state,expect`; appends
  points client-side from records (same rules as series.py) and
  re-queries `/series` on zoom or window change.
- [ ] Page says "HK series need the IDB JSON" when the catalogue has
  no `hk:` ids.

### Task C2: smoke

- [ ] Live smoke with the real ASW (reviewer runs): interactive
  controller on free ports, boot, 120 s; every starter view loads and
  each listed series has points (script `scripts/stix/platform/tests/
  timeline_smoke.py` using only the API); `--view-run` on that run dir
  serves the same.

## Merge order

A and B in parallel → gate each (self-test, qt suite unaffected,
`gates2.sh` since core.py changes) → ff A, rebase B, ff B → C on top →
gate → ff. Post-merge steps per memory.

## Open questions for the owner (do not block)

1. FSW parameter table (board 2d): needs the parameter defaults
   (from the FSW tables or the IDB?) and a read-back path.
2. Detector stimulus (board 2c): per-detector/pixel rates, spectra,
   single events need detector-model work: plan it?
3. Run bundles (board 7): its own design.
