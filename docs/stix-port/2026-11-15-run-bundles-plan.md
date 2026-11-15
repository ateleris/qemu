# Run bundles: record -> replay -> compare, staged — plan (2026-11-15)

> For agentic workers: implement task by task; steps use `- [ ]`.
> Workers cannot build or run anything: write code + self-test cases,
> the reviewer runs them and commits.

**Goal:** every platform run becomes a bundle (a manifest, the derived
decisions and the run dir it already is), the Runs board lists and
opens them, and the record/replay/compare formats are fixed now so S2
(replay on guest time) and S3 (compare two runs) only add consumers.

**Architecture:** a new `stixplat/bundle.py` owns the manifest, the
input classifier and the decisions extractor (both fed in record
order with per-run state); the extractor
runs as a record-log listener appending `decisions.jsonl` next to
`log.jsonl` in the run dir (the run dir *is* the bundle: no copies).
API additions (`GET /runs...`, verbs `run.baseline[.clear]`,
`run.open`/`run.close`) and one new tab in `ui/index.html`. Replay
(S2) and compare (S3) are separate modules that consume only what S1
writes.

**Spec:** owner decision 2026-11-15: "Record -> replay -> compare,
staged. S1 = every platform run writes a bundle and the Runs board
lists them; S2 = replay a bundle on guest time with a chosen ELF/QEMU;
S3 = compare two runs (decisions first, numbers within tolerances;
first divergence marked on the Timeline two-run overlay)." Design
notes: `2026-11-14-timeline-design.md` "Later" (a bundle = manifest
(ELF + sha256, QEMU commit + argv, platform config, knob values, flash
start state by reference) + inputs stamped on guest time + outputs +
decisions; "wall-paced GS TCs" not replayable; "log.jsonl and the
scenarios are most of the input record already"). Mockup board
`Runs.dc.html` (list + "what the bundle holds" + replay + compare).
House style: `2026-11-14-timeline-plan.md`.

## Key facts (verified by reading; UNVERIFIED marked)

Run dir and retention
- Every run has a run dir: interactive `run_dir.path` (config.py:28),
  run mode `<retention.run_root>/runs/<stamp>-<name>[.<variant>]/`
  (runmode.py:190, the UTC-stamp prefix makes it unique,
  runmode.py:146). Run-mode dirs carry the retention marker
  `.stixplat-run` (retention.py:21; written "running" at
  runmode.py:192, passed/failed at finish_run retention.py:119-128);
  retention deletes only marker-carrying dirs (retention.py:113) and
  only under `runs/`.
- `fresh = true` removes only the controller's named files
  (rundir.py:30): flash.bin, uart.raw, qemu.log, log.jsonl, tm.jsonl —
  a manifest.json or decisions.jsonl would survive, so the bundle
  writer must truncate its own files at start.
- log.jsonl opens in append mode (log.py:88): a reused interactive
  dir's log holds the earlier sessions too. The manifest carries the
  session's `start_seq` (the controller.start record's seq) and S2
  slices the log from it.
- Retention sizes are real blocks (retention.py:29-36); the flash is
  sparse (16.5 GB apparent, platform.md "The run dir"); `keep_flash`
  drops a passed run's flash (retention.py:119-128).
- The interactive run dir is reused across sessions; the flock
  (rundir.py:59-72) is the only thing stopping a second controller.

Record log and records
- Record schema v1 (log.py:24-36); `emit` calls listeners after
  releasing `self.cond` (log.py:114-116), each in try/except that
  logs once and never re-raises (log.py:134-165); a listener must not
  take `state.lock` or emit. `ingest` (view-run) feeds pre-built
  records through the same listeners (log.py:119-132).
- Rotation: 50 MB, 5 files, gzipped log.jsonl.N.gz (log.py:71,
  log.py:167-183). The `controller.start` record (config dump,
  core.py:93-95) can rotate away — the manifest must carry the config
  itself.
- The actual QEMU argv is already logged in the power "on" record
  (power.py:140 `_record("on", pid=..., argv=argv)`), built by
  qemu.build_argv (qemu.py:36-73); the boot record carries `t0`
  (power.py:176-178).
- The ELF sha256 is computed once at start (core.py:380-390);
  `status()["setup"]` exposes binary/elf/icount/pace (core.py:392-404).

Inputs (all already stamped on guest time in log.jsonl)
- Every verb goes through the dispatcher and logs `cmd` + `result`
  (dispatch.py:100-125), so operator/scenario actions are cmd records
  with `data.args`. TC bytes: `tc` records carry hex, service,
  subtype, source, delivered (relay.py:173-198; real GS TCs: source
  "tc", path "tc.gs"; `tc.gs.send`: source "scn", plat.py:418;
  `tc.platform.send`: source "ctl", plat.py:395-397; TC(9,129):
  source "ctl", path "platform.timesync", timesync.py:201 — but that
  record carries only seq/coarse and a `raw` boolean, no TC bytes).
  Real GS TCs arrive on UDP (relay.py:149-164) and their tc records
  carry `sim_est: true` while RUNNING — `emit` stamps the clock's
  est (log.py:96-105); the `sim_est=True` at relay.py:101-103 is the
  TM emit. Wall-paced, the design's non-replayable case.
- Faults: `fault` records with action `inject` (resolved params +
  seed but no trigger, faults.py:281-285), `armed` (the only record
  carrying the at_sim trigger, faults.py:238-240; the `inject`
  follows at fire time), `clear`, `reapply`
  (re-applied automatically at power cycles, faults.py:287-299) and
  `error`. Forcings: `force` records action `set`/`clear`
  (force.py:52-66) and `force.reapply` (force.py:75-76).

Outputs already on disk per run dir: log.jsonl, tm.jsonl
(relay.py:104-108), uart.raw (qemu.py:46-48), qemu.log.

Decision sources already decoded in records
- TM(5,x) events: `tm` records with `data.decoded.type == 5`, EID +
  name (decode.py:53-56); the platform EID table is decode.py:19-26
  (0x0021 mode change, 0x142E keepalive lapse, 0x1429, 0x542B, 0x14A3
  FDIR). Overrun/skip EIDs: none — the FSW event enum (TMData.h:
  245-364) has no ROTB-overrun/skip EID; an overrun sets flags and
  silently skips ~60 s of data (RotAccumulator.c:51-64, 104-141) and
  HK carries only the 3-bit RB level (HKAcquire.c:151-152, status4
  bits 31..29); the only skip EIDs are the user-request/aspect ones
  0x543F/0x5440/0x5446 (SciReport path, accepted-diffs §48). S3
  derives the overrun rows from hk_step/series data instead.
- HK fields: `tm` records carry `data.hk = {code: value}` when the
  IDB JSON is loaded (relay.py:116-124); RB level NIXD0501 (SID 2)
  and mode NIXD0023 per the timeline plan's pinned rules
  (2026-11-14-timeline-plan.md:124-128). TM(1,x) acks carry
  `data.decoded.tc = {packet_id, apid, seq}` + fid (decode.py:45-52).
  Keep-alive TCs log only summaries with `data.summary = true`, no
  hex (keepalive.py:178-189) — the ack rule must key on tc records
  that carry `hex`, or 8 Hz acks flood the decisions.

Conventions to reuse
- Views precedent: tmp + os.replace writes (views.py:114-136), the
  name rule checked before any file access (views.py:24, 36-41).
  Flash library: sparse `cp --sparse=always`, refused over the sparse
  limit (flashlib.py:116-139); record fields flashlib.py:63-77 (no
  content sha today).
- API: additive v1 GET routes in api._get (api.py:177-226); verbs are
  registered in core._register (core.py:99-138) **and** listed in
  verbs.VERBS (the registry the self-test and run mode check), and
  must be added to platform.md's verbs table (the self-test's
  "verbs == docs table").
- `--view-run DIR [--port N]` (default 8081) serves a run dir
  read-only through the same API/page, `view.*` verbs only
  (viewrun.py:62-94, 162-166, 185-209); it takes the run's config
  from its controller.start record (viewrun.py:133-151).
- Page rules: tab buttons `data-tab` (index.html:160-171), no
  innerHTML, no inline handlers/styles, every `data-verb` served.
- Self-test: offline, fake QEMU, private UDP ports from
  `free_udp_port` (selftest.py:91-124); cases in `CASES`
  (selftest.py:4555-4634).

## Global constraints

- Stdlib only; no new third-party import anywhere.
- API v1 additive only; no record kind/topic added (decisions are not
  records — a separate file — so `log.KINDS` is untouched).
- Listener rule as in the timeline plan: collect under the lock,
  call after release, try/except per listener, never emit, never
  `state.lock`.
- The flash is never copied and never materialised: hashing flash.bin
  is sparse-aware (FIEMAP extents + zeros for holes) and only at an
  explicit flash-library save (power off/paused), never at controller
  start or close.
- Test configs never touch the demo controller's ports 8080, 21502,
  21511 (21512 is QEMU's TC bind): self-tests use `free_udp_port()`,
  API ports in the 18081+ band.
- Retention still deletes only marker-carrying dirs; the one policy
  change is "the baseline run is never deleted" (accepted-diffs).
- No FSW edits; nothing copied from ~/dev/stix-doc-notes/sources.
- `python3 stixplat.py --self-test` stays PASS; every task adds its
  own `check(...)` cases.

## Approaches considered

- **A. Bundle = the run dir + two derived files (chosen).** The log,
  TM, UART, argv and flash state are already there; retention and
  `--view-run` keep working unchanged; S2 re-feeds straight from
  log.jsonl. Cost: an interactive session's bundle is overwritten by
  the next session in the same dir — accepted and documented: the
  writer truncates its files at start (R2), the manifest's
  `start_seq` scopes the log slice (R1/R6), platform.md carries the
  caveat (R5); run mode keeps every session, retention keeps the
  last N.
- **B. A separate bundle store** (`<run_root>/bundles/<id>/` with
  copied or hardlinked logs): self-contained exports, but duplicates
  data on a nearly full C: drive and drifts from the run dir.
  Rejected for S1; an "Export" can tar the run dir later.
- **C. Derive everything at request time from log.jsonl only** (no
  manifest, no decisions file): zero disk cost, but controller.start
  can rotate away, every board/compare call re-parses the whole log,
  and a reused interactive dir keeps no predecessor. Rejected: the
  manifest is the durable, self-describing artifact.

## Review focus

1. A reused interactive run dir must never mix sessions: the writer
   truncates decisions.jsonl and rewrites manifest.json at start,
   before anything reads them (test in R2).
2. The decisions writer is a log listener under concurrent emits
   (HTTP threads, poller, relay): line-atomic appends under its own
   lock, never a record emitted, never a raise into emit (test in R2,
   the t_series_concurrent pattern).
3. Sparse flash hashing: `sparse_sha256` must read only mapped
   extents and equal the dense sha256; a materialising implementation
   would fill the disk (test in R2).
4. `/runs/<run_id>` ids come from a URL: validate against a name rule
   before any filesystem access, as views.py does (test in R3).
5. The baseline exemption must not break retention's budget: `plan()`
   keeps the baseline run like a running one, and a baseline whose
   dir disappeared is shown as missing, never fatal (test in R3).
   The baseline's size still counts toward max_total_mb; past it
   `plan()` warns and keeps the baseline (test in R3).

---

## Track S1 — every run writes a bundle; the Runs board

### Task R1: bundle.py — manifest shape, input classifier, decisions extractor

**Files:** create `scripts/stix/platform/stixplat/bundle.py`; modify
`config.py` (DEFAULTS `bundle`), `selftest.py`.

**Interfaces — fixed now (S2/S3 consume them):**
- `BUNDLE_VERSION = 1`; `MANIFEST = "manifest.json"`;
  `DECISIONS = "decisions.jsonl"`; `BASELINE = ".stixplat-baseline"`.
- Manifest JSON (written by R2, shape frozen here):
  `{"v": 1, "kind": "run"|"replay", "run_id", "dir", "mode",
  "scenario": {file,name,variant}|null, "started_utc", "start_seq",
  "ended_utc",
  "open": bool, "t0": ns|null, "final_sim_time": ns, "final_seq": N,
  "length_s": s|null (last sim_time - t0), "fsw": {elf, sha256},
  "qemu": {binary, sha256|null, commit|null, dirty|null, describe|null,
  argv: [...]}, "config": {…the controller.start config…},
  "flash": {"start": {source: "library"|"fresh"|"carried", name|null,
  sha256|null, record|null}, "end": {present, allocated_kib}},
  "outputs": {log_records, log_bytes, tm_lines|null, tm_bytes|null,
  uart_bytes}, "inputs": {counts: {verb: {name: n}, tc, fault, force},
  not_replayable}, "decisions": {counts: {event, ack, hk_step}}}`.
  Readers key on `"v"` and ignore unknown fields (the D-PLn rule).
- `run_id(basename, started_utc)`: run-mode basenames already start
  with a `%Y%m%dT%H%M%SZ` stamp -> the basename; otherwise
  `<basename>-<started_utc compact>` (an interactive dir is reused).
- `git_info()` -> `{"commit", "dirty", "describe"}` or None:
  `git -C <repo-root> rev-parse HEAD` and `describe --always
  --dirty=.dirty`; repo root = 4 `dirname`s up from bundle.py; any
  failure (no git, foreign binary) -> None, never fatal.
- `classify(obj, state) -> input dict | None` (fed in record order
  with a per-run `state`; the S2 contract):
  cmd records whose verb is in `REPLAY_VERBS` = {sim.pause,
  sim.resume, log.mark, platform.spw.link, platform.timecode.enable,
  platform.timecode.once, platform.timecode.inject, platform.obt.set,
  platform.keepalive.set, det.rotb.burst, platform.power.on,
  platform.power.boot, platform.power.off, platform.power.cycle} ->
  `{"input": "verb", "verb", "args", "replay": "guest"}`;
  tc records carrying `hex` -> `{"input": "tc", "hex", "replay":
  "guest"|"wall"|"no"}` ("wall" when source == "tc" — a real GS TC,
  why "wall-paced GS TC"; "no" when delivered is false, why "never
  reached the FSW"); the `platform.timesync` tc record (no hex:
  seq/coarse + a `raw` boolean, timesync.py:198-201) -> `{"input":
  "tc", "hex": null, "service": 9, "subtype": 129, "seq", "coarse",
  "replay": "guest"}` — a manual mid-run sync is a real input (its
  TM(1,7) ack matches no hex-carrying tc record, so it yields no
  `ack` decision: documented); fault records: `state` holds the
  armed faults by id — an `armed` record stores {params, seed,
  trigger} and maps to None; the matching `inject` (same id) ->
  `{"input": "fault", "id", "params", "seed", "trigger", "replay":
  "guest"}` (the trigger survives — an at_sim fault replays as an
  at_sim fault); an `inject` with no armed twin is an immediate
  inject, trigger `{"at": "now"}`; action `clear` -> fault.clear
  input; actions `reapply|error` -> None (automatic / failure);
  force records action
  `set`/`clear` -> `{"input": "force", "path", "value", "replay":
  "guest"}`, path `force.reapply` -> None. Verbs excluded as
  themselves (their canonical input is another record, or they must
  not re-run): fault.*, force.*, tc.platform.send, tc.gs.send,
  platform.timesync.send (covered by their fault/force/tc records),
  scenario.run/abort (already expanded into their own cmd records),
  platform.flash.* (flash state is by reference), debug.qmp, view.*
  -> `{"input": "verb", "replay": "no", why}` so they count in
  `not_replayable`. `result` records -> None.
- `decide(obj, state) -> [decision]` (a stateful accumulator fed in
  record order — the ack rule needs the run's input-TC key set in
  `state`; the S3 contract); a decision is
  `{"v": 1, "seq", "sim_time", "boot_id", "kind", "key", "class",
  "detail"}`; kinds: `event` (tm, decoded.type == 5: key `"0x%04X"`,
  class from `EID_CLASSES = {0x0021: "mode", 0x142E: "keepalive",
  0x1429: "timesync", 0x542B: "timesync", 0x14A3: "fdir"}` else
  "other", detail {name, params}); `ack` (tm, decoded.type == 1, only
  when (apid, seq) matches a tc record carrying `hex` seen in this
  run — a bounded set of the last 512 input-TC keys; key
  `"0x%04X/%d"`, detail {subtype, fid}); `hk_step` (tm with `data.hk`,
  for codes in `[bundle] hk_codes`, when the value changed since the
  previous HK report of that (sid, code): key `"hk:<sid>:<code>"` —
  the series id space — detail {value, previous}). Heartbeats
  (APID 0x5E5) and HK reports without data.hk produce nothing.
- Config `[bundle]`: `enabled` (bool, true), `hk_codes` (list of
  str, `["NIXD0501", "NIXD0023"]`). `config._check` validates the
  types. (No `overrun_eids`: the FSW has no overrun EID — S3 derives
  the overrun rows from hk_step/series data.)

- [ ] Self-test `bundle_decisions`: feed synthetic records to
  `decide`: tm decoded {type: 5, eid: 0x21, params: "02"} -> exactly
  one decision, kind "event", class "mode", key "0x0021"; eid 0x542B
  -> class "timesync"; tm with data.hk {"NIXD0501": 3} then
  {"NIXD0501": 5} -> exactly one `hk_step` (detail value 5, previous
  3); tm decoded {type: 1, tc: {packet_id: 0x15A0, apid: 0x5A1,
  seq: 7}, subtype: 7} after a tc record whose hex has that
  (apid, seq) -> one `ack`; the same ack without a matching tc
  record -> nothing; an APID 0x5E5 record -> nothing.
- [ ] Self-test `bundle_inputs`: cmd sim.pause -> verb/guest;
  cmd scenario.run -> replay "no" with a reason; tc source "tc"
  delivered true -> "wall"; the same with delivered false -> "no";
  a `platform.timesync` tc record -> a tc input with hex null and
  the recorded seq/coarse; fault action "reapply" -> None; an
  `armed` record then its `inject` -> one fault input carrying
  params, seed and the at_sim trigger verbatim; an `inject` with no
  armed twin -> trigger {"at": "now"}; action "inject" with params
  {"delta_s": 4} and seed 99 -> the fault input carries both
  verbatim; force action "set" value 123 -> force input; cmd
  platform.flash.load -> replay "no".
- [ ] Self-test `bundle_run_id`: run_id("20261115T101112Z-gate-x",
  ...) == the basename; run_id("interactive", t) ends with the
  compact stamp.

### Task R2: the writer — the Bundle as a log listener + lifecycle

**Files:** `bundle.py` (class `Writer`); modify `core.py` (own and
close it), `flashlib.py` (`sparse_sha256` + `sha256` in the save
record), `runmode.py` (verdict into the manifest), `selftest.py`.

- [ ] `Writer(cfg, run_dir, log, mode, scenario=None)`: at
  `__init__` — before the controller's next emit can matter —
  capture `flash_existed_at_start` (os.path.exists(run_dir/flash.bin)),
  truncate decisions.jsonl (open "w"), and write manifest.json
  (tmp + os.replace, the views.py pattern) with `open: true` and
  every start field it can know (config via the same `_public(cfg)`
  dict core.py:95 logs; fsw elf + sha256; qemu binary + binary
  sha256 + git_info; argv null until the power record).
  `on_record` (the log listener) classifies via `classify(obj,
  state)`, extracts via `decide(obj, state)`, and appends one json
  line per decision to decisions.jsonl under its own lock, in seq
  order: the listener call order can invert adjacent seqs across
  emitting threads (log.py:114-116 releases `cond` before the
  calls), so decisions wait in a small per-seq buffer until every
  lower seq has flushed; whatever is left flushes in seq order at
  close — never emits, never takes state.lock. It also tracks for
  the close write: the first power record with `argv` (power.py:140),
  the boot record's `t0`, the last sim_time/seq, the input counts,
  the platform.flash.load *result* record (its `result` field
  carries the library entry with the new sha256, dispatch.py:122-124),
  and the input-TC (apid, seq) key set for `decide`.
- [ ] `Writer.close()`: `open: false`, ended_utc, final_sim_time,
  final_seq, length_s, qemu.argv from the power record, and flash
  provenance — `source: "library"` (name + the entry record with its
  sha256, taken from the load's result record) when a
  platform.flash.load cmd precedes the first
  power record; else `"carried"` when flash_existed_at_start; else
  `"fresh"`. `flash.end` = {present, allocated_kib} from os.stat()
  st_blocks — never hashed. outputs counts: log records seen,
  tm.jsonl/uart.raw sizes via os.stat (absent -> null). Rewrite
  manifest.json atomically; idempotent, safe to call twice.
- [ ] `core.Controller`: create the Writer right after the
  `controller.start` emit (core.py:93-95), passing that record's seq
  as the manifest's `start_seq`, when `cfg["bundle"]["enabled"]`;
  `shutdown()` closes it immediately before `log.close()`
  (core.py:473, after plat.close at 472) so the post-controller.stop
  power-off and plat.close decisions land.
- [ ] `flashlib.sparse_sha256(path)`: a FIEMAP walk (fcntl.ioctl +
  a ctypes struct — Linux-only like rundir's fallocate), hashing
  mapped extents and zeros for holes; `save()` adds `sha256` to the
  record (additive; readers ignore unknown fields). I/O is bounded
  by the sparse limit (real blocks <= power.sparse_limit_mb = 1 GiB);
  CPU is not (holes hash as zeros: the full image, ~10 s per save).
- [ ] `runmode._batch` calls `bundle.set_verdict(run_dir, verdict)`
  after `retention.finish_run` (runmode.py:200) so a finished
  run-mode manifest carries `"verdict"`.
- [ ] Self-test `bundle_writer_lifecycle` (Env, fake QEMU): a
  controller with `timeline.sample_s = 0.5` and
  `[gs] emulation = "on"`; power on, boot, one
  `fault.inject {"id": "timecode.missing"}`, one `tc.gs.send
  {service: 237, subtype: 18, data: "01383c00000000"}` (its tc
  record's param is {index: 312, sub: 60, value: 0},
  relay.py:250-260), then — the fake QEMU never sends TM — a
  synthetic tm record via `log.ingest` whose decoded.tc matches that
  tc record's (apid, seq), power off, shutdown.
  Assert manifest.json: v == 1, run_id == basename + "-" + the
  compact started_utc (the Env dir's name is not a stamp — R1's
  rule), fsw.sha256 == hashlib.sha256(b"").hexdigest()
  (e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855),
  qemu.argv[0] endswith qemu-new, inputs.counts.fault == 1,
  inputs.counts.tc == 1, decisions.counts.ack == 1,
  flash.start.source == "fresh", open == false. Then a second
  controller in the SAME run dir: a new started_utc and start_seq,
  the old decisions gone (truncated), and
  `retention.read_marker` still None.
- [ ] Self-test `bundle_writer_rotation`: `log.max_log_mb` tiny so
  the log rotates mid-run; the manifest still carries the full
  config (controller.start rotated away) and decisions.jsonl has
  every decision with strictly increasing seqs (no gaps).
- [ ] Self-test `bundle_writer_concurrent`: 4 threads emit 500
  cmd/tm/fault records through one RecordLog with the writer
  attached; afterwards every decisions.jsonl line json.loads alone
  (no interleaved writes) and their seqs are strictly increasing
  (the Writer's per-seq buffer orders what the listener rule leaves
  unordered).
- [ ] Self-test `bundle_flashlib_sha`: a 64 MiB sparse file with two
  4 KiB blocks 32 MiB apart -> sparse_sha256(f) == the dense
  hashlib.sha256, and it returns in < 2 s (the timing guard is the
  "only extents read" check, not an implementation detail).
- [ ] Self-test `bundle_runmode_verdict`: a one-variant run via
  `runmode.run_one` on a tiny scenario -> the run dir's manifest has
  `"verdict": "pass"` after finish_run.

### Task R3: runs API + baseline + the in-run viewer

**Files:** `bundle.py` (`listing`, `detail`, `baseline_*`,
`Viewers`); modify `api.py` (3 GET routes), `core.py` (4 verbs),
`verbs.py` (VERBS), `selftest.py`.

- [ ] `listing(cfg, live_dir=None)`: scan the live dir (when it has
  a manifest) plus `<retention.run_root>/runs/*/manifest.json`;
  returns `{"root": run_root, "runs": [entry...]}` sorted by
  started_utc, newest first; entry = {run_id, dir, live, open,
  started_utc, ended_utc, verdict, mode, scenario, name (the
  scenario name or run_id), elf, elf_sha256, qemu_describe,
  qemu_dirty, icount_shift, pace, length_s, baseline, inputs,
  decisions, error?} — a broken manifest becomes an entry with
  `error`. Dirs without manifest.json are not listed. `baseline`
  reads the BASELINE marker.
- [ ] `detail(cfg, run_id)` and `read_manifest(cfg, run_id)`: the
  run_id must match `^[A-Za-z0-9][A-Za-z0-9.@_-]{0,127}$` ->
  VerbError 400 before any filesystem access (the views.py:36-41
  pattern); unknown -> 404. detail adds `files` sizes, `baseline`
  and the retention marker status.
- [ ] Baseline: `baseline_set/clear` — set writes the BASELINE
  marker (`{"name", "set_utc"}`) into the target run dir and removes
  the previous holder's; unknown run_id -> 404. `retention.plan`
  treats a dir holding BASELINE like a running run (never doomed;
  one clause next to retention.py:79-90). Its size still counts
  toward max_total_mb: past the budget `plan()` logs a warning
  naming the baseline dir — counted and warned, never deleted.
- [ ] `Viewers`: at most one open read-only viewer;
  `open(ctl, run_id, port=None)` builds `viewrun_mod.ViewRun(dir)`
  and `api_mod.make_server(vr, "127.0.0.1", port)` — the exact
  --view-run objects (viewrun.py:185-200) — on a free TCP port and
  returns {"url", "port"}; a second open -> 409; `close()` shuts the
  server down. ViewRun's 403-for-non-view.* rule applies unchanged.
- [ ] API: `GET /api/v1/runs`, `GET /api/v1/runs/<run_id>`,
  `GET /api/v1/runs/<run_id>/manifest` (the raw manifest.json bytes).
  The handler passes `bundle_mod.listing(ctl.cfg, getattr(ctl,
  "run_dir", None))`, so a --view-run server lists other bundles too
  (no viewrun.py change needed).
- [ ] Verbs (core._register **and** `verbs.VERBS`, verbs.py:6-16 —
  the self-test asserts `set(dispatcher.verbs) == VERBS`
  (selftest.py:2013-2015) and run mode validates scenarios against
  it (runmode.py:130); the platform.md table is R5):
  `run.baseline {name}` (string, required), `run.baseline.clear {}`,
  `run.open {name, port?}` (1024..65535, default a free port),
  `run.close {port?}` — all 403 in a --view-run server by the
  existing rule (viewrun.py:162-166).
- [ ] Self-test `bundle_api`: a finished synthetic bundle under a
  fake run root: /runs lists it with the right elf_sha256 and
  icount_shift; /runs/<id> 200 with files.log_bytes ==
  os.path.getsize(log.jsonl); /runs/<id>/manifest == the file's
  bytes; ids "../etc" and "a/b" -> 400; an unknown id -> 404; the
  live controller's own dir appears with live: true.
- [ ] Self-test `bundle_baseline`: set -> /runs shows baseline: true
  on that row only and the marker file exists in that dir; clear ->
  gone; `retention.plan` with fake sizes keeps the baseline dir past
  max_total_mb with a warning that names it; set on an unknown id ->
  404.
- [ ] Self-test `bundle_run_open`: run.open on a finished dir -> 200
  with a URL; GET <url>/api/v1/status says mode "view" and view_run
  == the dir; a second run.open -> 409; run.close -> the port no
  longer accepts; nothing written into the run dir (the `_dir_state`
  pattern of the view-run test).

### Task R4: the Runs board tab

**Files:** `ui/index.html`, `stixplat/selftest.py`.

(The mockup's "Bundle this run" / "Import…" buttons, Runs.dc.html:
48-49, are not in S1: the run dir is the bundle and there is no
import format.)

- [ ] Tab button `data-tab="runs"` between State and Setup
  (index.html:160-171: ...State, **Runs**, Setup) and a
  `data-tab-panel="runs"` section: a filter input (client-side,
  case-insensitive over name/ELF/QEMU/verdict), a Refresh button, and
  a table built with createElement/textContent (the faults-table
  pattern) with the mockup's columns: Bundle (name + live/open
  badges + a baseline mark), Recorded (started_utc), FSW ELF (name +
  the first 8 hex of the sha256), QEMU (describe, "+ dirty" when
  dirty), icount, Length, Tags (verdict, scenario, error) — plus
  per-row actions: baseline (`data-verb="run.baseline"`, the row's
  run_id), open (`data-verb="run.open"`; on success
  `window.open(url, "_blank", "noopener")`), and one header-level
  close-viewer button (`data-verb="run.close"`) shown while a viewer
  is open. A row with `error` renders the error text instead of
  actions. The board fetches `GET /api/v1/runs` on tab switch and
  every 5 s while visible (the /status poll discipline).
- [ ] No new CSP needs: connect-src 'self' already covers /runs;
  window.open is plain JS; text only via textContent.
- [ ] Self-test `UI: runs board (S1)` (page rules, no browser): the
  tab button and panel exist; `#runs-filter` and `#runs-table`
  exist; the page's data-verb set gains run.baseline, run.open,
  run.close and every data-verb is still a served verb (the existing
  every-verb-served walk); the existing no-innerHTML / no inline
  handler / no style attribute checks cover the new markup once the
  page is parsed.

### Task R5: docs + live check

**Files:** `docs/stix-port/platform.md`,
`docs/stix-port/accepted-diffs.md`, `selftest.py`.

- [ ] platform.md: a "Run bundles" section (what a bundle is, the
  manifest fields, decisions.jsonl, the interactive-dir-is-
  overwritten caveat, baseline + retention, run.open limits); 4 new
  rows in the verbs table between the `<!-- verbs:begin/end -->`
  markers (platform.md:167-199) — `t_verbs_doc` enforces exact
  equality; a `/runs` row in the API endpoint table; a paragraph in
  the run-dir section.
- [ ] accepted-diffs.md: the new numbered sections listed below.
- [ ] Reviewer live check (never ports 8080/21502/21511/21512): a
  smoke config with `[ports] api = 18081` and free 215xx slots,
  `[run_dir] path = "/tmp/stix-plat/bundle-smoke"`, the fake QEMU
  wrapper from tests/; on, boot, ~30 s, one fault, off. Then the
  Runs tab at :18081 lists the bundle (ELF sha, the fake binary in
  argv, icount, length ~30 s), `open` serves it read-only on a free
  port, and manifest.json + decisions.jsonl exist in the run dir.

## S2 — replay a bundle on guest time (outlined; formats fixed by R1/R2)

### Task R6: replay.py + `stixplat.py --replay BUNDLE [--elf] [--qemu-binary] [--port]`

**Files:** create `stixplat/replay.py`; modify `stixplat.py` (the
flag), `selftest.py`, platform.md.

- Input: `bundle.inputs(run_dir)` — log.jsonl from the manifest's
  `start_offset` byte on (implementation finding: seqs restart at 1 per
  session, so `start_seq` cannot scope it) (a reused interactive dir's log.jsonl also holds
  earlier sessions, log.py:88) passed through `classify`, ordered by
  seq, each `{seq, sim_time, boot_id,
  input, verb|hex|id+params+trigger+seed|path+value, replay, why}`;
  plus the origin manifest (ELF/QEMU defaults, config).
- Re-feed rule per kind (fixed now): verb inputs are re-dispatched
  through a real Controller's Dispatcher with the recorded `args` at
  the recorded `sim_time` via `clock.at()` — bench-time deadlines
  fire only while RUNNING (simclock.py:144-158), so inputs land on
  guest time exactly as the sampler and keep-alive do. tc inputs are
  re-sent as the recorded `hex` through `Relay.gs_tc` (the link-down
  policy re-applies; the recorded bytes keep the TC seq, so TM(1,x)
  ack matching reproduces). fault inputs re-run `fault.inject` with
  the recorded resolved params + seed + trigger (an at_sim trigger
  re-arms and fires via `clock.at()`; an immediate trigger injects
  now; the armed echo is consumed by classify and never re-fed;
  reapply is never re-fed — the power cycle re-applies by itself).
  A tc input with hex null (the manual TC(9,129)) re-runs
  `platform.timesync.send {coarse: <recorded>}`; its ack produces no
  decision in either run. force inputs re-run
  `force.set`/`force.clear`. Power verbs are re-dispatched; the
  off-time stays wall-paced by design (D-PLr), and the replay's own
  bench clock accumulates off-times the same way.
- `replay: "wall"` inputs (real GS TCs) and `replay: "no"` inputs
  are skipped, each logged as a `log` record `replay.skipped
  {seq, why}`; `--allow-wall` suppresses the refusal, never converts
  the input.
- Output: a fresh run dir
  `<run_root>/runs/<stamp>-replay-<orig-name>/` whose manifest is
  `{"kind": "replay", "of": <orig run_id>, "elf", "qemu-binary", ...}`
  in the same shape, with a decisions.jsonl produced by the same
  Writer — so S3 compares replay vs origin with no new code.
- [ ] Self-test: replay a synthetic bundle with the fake QEMU; the
  replay's decisions carry the same fault/event sequence; every
  skipped input is listed with its why; a bundle containing a wall
  TC refuses without `--allow-wall`.

## S3 — compare two runs (outlined; formats fixed by R1/R2)

### Task R7: compare.py + the Timeline two-run overlay

**Files:** create `stixplat/compare.py`; modify `ui/index.html`,
`selftest.py`, platform.md.

- `compare(baseline_dir, other_dir, tol=None)`: load both
  decisions.jsonl (derive with `decide` from log.jsonl when missing
  — one accumulator fed in record order; pre-S1 runs still
  comparable); align at the first common decision
  (one-sided leading rows are skipped); pass 1 — decisions: the same
  (kind, key) sequence, times within tol (default 1.0 s for
  event/mode/ack; 1.0 s and an exact value match for hk_step); pass
  2 — numbers: series points within per-id tolerances (`[compare]
  tol_number`: default 0.05 relative + 1.0 absolute).
- Output: `compare.jsonl` (one row per check: `{check, a, b, result:
  same|differs|only-baseline|only-other, tol, first: bool}`) plus a
  summary `{"verdict", "first_divergence": {kind, key, t_a, t_b}}`;
  the Runs board's Compare panel reads it, and the Timeline overlay
  draws the second run's series dashed with the first divergence as
  a marker.
- Mode rows compare the recorded `detail.params` hex in v1
  (decided: the 0x0021 param layout stays undecoded until needed).
  The mockup's "First overrun (HK level 7)" and "Overruns in the
  run" rows derive from the hk_step NIXD0501 steps and the
  instrument.rotb.*/as.dropped series — there is no overrun EID;
  "Processed drain" is a series row (pass 2). The Compare panel
  renders Check | Baseline | Replay | Result (Runs.dc.html:116).
- [ ] Self-test: two synthetic bundles differing only in one event's
  time beyond tol -> verdict "differs" and first_divergence names
  that row; identical bundles -> "same".

## Merge order

R1 -> R2 -> R3 (each: self-test PASS; R2 and R3 also `gates2.sh`
since core.py/api.py change, as in the timeline plan's merge order)
-> R4 -> R5 -> gate -> ff. R6/R7 are separate future worktrees;
nothing in S1 may need changing when they land — that is the point
of fixing the formats now.

## Accepted-diffs / docs updates

- accepted-diffs.md, continuing after §50: §51 "Run bundles:
  retention keeps the baseline run" (controller-side policy, no
  upstream analogue; the baseline's size is counted and a
  past-budget `plan()` warns but keeps it); §52 "flashlib records
  gain sha256" (additive field; sparse-aware FIEMAP hashing; readers
  ignore unknown fields); §53 "the controller may serve one
  read-only viewer of another run dir in-process" (run.open;
  127.0.0.1 bind, one slot, --view-run's 403 rules unchanged).
- platform.md: the verbs table rows (run.baseline,
  run.baseline.clear, run.open, run.close), the "Run bundles"
  section, the /runs endpoints, the retention paragraph.

## Open questions — answered 2026-11-15

1. Overrun/skip EIDs: none exist (see Key facts) — the EID promise
   is dropped; S3 derives the overrun rows from hk_step/series data.
   `[bundle] overrun_eids` is gone.
2. Interactive overwrite: accepted (Approach A stands). The rule is
   documented: the writer truncates its files at start, the
   manifest's `start_seq` scopes the log slice, platform.md carries
   the caveat. A new run_id per controller start was rejected: it
   would need a per-session run dir (a copy or a redirect of
   `run_dir.path`) for no gain; run mode already keeps every
   session.
3. run.open: the in-process viewer ships as specified (R3's
   `Viewers` is exactly the --view-run pair, viewrun.py:185-200).
4. Retention keeps the baseline past `max_total_mb`, but its size
   is counted and a past-budget `plan()` warns naming the dir.
5. Mode-change compare: `detail.params` hex in v1.

## Plan review (2026-11-15)

Applied against the code at c0f5083a5d; every cited claim
re-verified by reading before editing. No finding was rejected.

- B1 (fixed): `classify` is now fed in record order with per-run
  state and pairs `armed` -> `inject` by id, so the fault input
  carries the at_sim trigger (the `inject` record itself has none,
  faults.py:281-285 vs 238-240). `bundle_inputs` covers the armed/
  inject pair and the immediate inject; R6 re-feeds the trigger.
- B2 (fixed): R3 now also edits `verbs.py` (VERBS) and cites the two
  checks that fail without it (selftest.py:2013-2015,
  runmode.py:130).
- SH3 (fixed): the `platform.timesync` tc record (no hex,
  timesync.py:198-201) is classified as an input (hex null,
  seq/coarse); S2 re-runs `platform.timesync.send {coarse}`; the
  manual sync's unmatched TM(1,7) ack (no `ack` decision) is
  documented as a loss.
- SH4 (fixed): the lifecycle test feeds a synthetic TM(1,x) via
  `log.ingest` (the fake QEMU never sends TM) and asserts
  `decisions.counts.ack == 1` instead of a vacuous key check; the
  run_id assertion now follows R1's rule for a non-stamp Env dir.
- SH5 (fixed): the TC(237,18) example hex is now
  "01383c00000000" = {index 312, sub 60, value 0} (relay.py:250-260;
  the design's "param 312", timeline-design.md:241).
- SH6 (fixed): the Writer buffers decisions per seq and flushes in
  order (log.py:114-116 releases `cond` before the listener calls),
  so the concurrent test keeps its strict-ordering assertion without
  extending the listener contract.
- SH7 (fixed): the overrun/skip EID claim replaced with the verified
  FSW facts (TMData.h:245-364, RotAccumulator.c, HKAcquire.c:151-152);
  `overrun_eids` config dropped; S3 derives the rows from hk_step/
  series data (owner Q1).
- N8 (fixed): sim_est citation corrected — log.py:96-105 stamps the
  clock's est; relay.py:101-103 is the TM emit.
- N9 (fixed): classify/decide described as stateful accumulators fed
  in record order (also R7's re-derive).
- N10 (fixed): the library entry comes from the load's result record
  (dispatch.py:122-124); Writer.close sits immediately before
  log.close() (core.py:473, after plat.close).
- Also, for Q2's soundness: the manifest gains `start_seq` and
  `bundle.inputs` slices log.jsonl from it (log.jsonl opens in
  append mode, log.py:88, so a reused interactive dir holds earlier
  sessions); mockup details inlined where cheap (compare-panel
  columns, out-of-scope buttons).

Nothing left open for the owner; the reviewer still owns all
builds/tests/commits.