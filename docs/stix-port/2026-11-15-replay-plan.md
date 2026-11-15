# Run bundles Stage S2: replay a bundle on guest time — plan (2026-11-15)

> For agentic workers: implement task by task; steps use `- [ ]`.
> Workers cannot build or run anything: write code + self-test cases,
> the reviewer runs them and commits. This plan expands the S2 outline
> of `2026-11-15-run-bundles-plan.md` ("S2 — replay a bundle on guest
> time", Task R6); the fixed formats (the manifest, `classify`,
> `decide`, decisions.jsonl, the per-kind re-feed rules, `replay.skipped`,
> `--allow-wall`, the output dir shape) are not reopened here. Where the
> code differs from the outline, the code wins — see "Deviations".

**Goal:** take a finished run bundle (its manifest + log slice) and run
it again on guest time: a fresh controller re-feeds every recorded
input — verbs through a real Dispatcher, TCs as the recorded bytes, the
null-hex TC(9,129) via `platform.timesync.send`, faults with the
recorded params + seed + trigger, forcings — each at its recorded
`sim_time` via `clock.at()`, so bench-time deadlines fire only while
RUNNING exactly as the sampler and the keep-alive do. Inputs that
cannot be re-fed (real GS TCs — wall-paced; TCs that never reached the
FSW; excluded verbs) are skipped and logged. The replay writes its own
bundle (a fresh run dir, manifest `kind "replay"`, decisions.jsonl from
the same Writer), so S3 compares replay vs origin with no new code.

**Architecture:** one new module `stixplat/replay.py` with three parts:
the input loader (the origin's log slice from the manifest's
`start_offset`/`log_rotations` through `bundle.classify`, tagged with
the origin's power phase), the scheduler (off/frozen-phase inputs
dispatched when due, running/paused-phase inputs armed once on
`clock.at`; one dispatch queue thread so a slow verb never blocks the
poller that fires the deadlines), and the re-feed table (one function
per input kind). `stixplat.py` gains `--replay BUNDLE [--elf]
[--qemu-binary] [--port] [--allow-wall]`. Two small additive edits to
`bundle.py`: the stimulus/param verbs' classification gap closes, and
the `Writer` takes an `extra` dict for the replay manifest's
`of`/`elf`/`qemu-binary`. No new record kinds, topics or sources; no
new config keys; no QEMU or model change.

**Spec:** owner decision 2026-11-15 (run bundles): "S2 = replay a
bundle on guest time with a chosen ELF/QEMU". Plan + formats:
`2026-11-15-run-bundles-plan.md` (R1/R2 fixed the formats; R6 is the
outline expanded here; its "Open questions — answered" and "Plan
review" applied). Design notes: `2026-11-14-timeline-design.md`
"Later" (a bundle = manifest + inputs stamped on guest time + outputs
+ decisions; "wall-paced GS TCs" not replayable). As built:
`accepted-diffs.md` §51-§53 (S1). House style:
`2026-11-15-spectra-plan.md`.

## Key facts

Read in this worktree; every claim re-verified by reading before
writing this plan. UNVERIFIED marked.

The classifier and its gap (bundle.py)
- `bundle.classify(obj, state)` exists (bundle.py:158) and returns the
  outline's input dicts, fed in record order with a per-run state
  (`new_state`, :124): verb inputs `{"input": "verb", "verb", "args",
  "replay"}` (:166-175), tc inputs `{"input": "tc", "hex", "replay"|`
  `"why"}` (:176-191), fault inputs via the armed/inject pairing
  (`_classify_fault`, :131-155: an `inject` inherits the `armed`
  record's params/seed/trigger; an orphan inject gets trigger
  `{"at": "now"}`; `clear`/`disarm` → a clear input; `reapply`/`error`
  → None), force inputs `{"input": "force", "path", "value"?,
  "replay"}` (:194-201; `force.reapply` → None). `decide` (:205) is
  the S3 extractor; the replay never calls it (its own Writer does).
- `REPLAY_VERBS` (:39-44) holds the 14 outline verbs, **not** the
  stimulus verbs: `det.tape.save/play/stop/delete`, `det.align.set`,
  `det.adc.set` are in neither `REPLAY_VERBS` nor `_EXCLUDED_VERB_WHY`
  (:46-66), so today they classify as `replay: "no"` with why
  `"no replay rule"` (:174). Same for the five `param.*` verbs —
  although their TCs ARE recorded as inputs (`param.set`/`param.read`
  send through `relay.gs_tc` with source "api", params.py:799, :884,
  so the tc record carries hex and classifies `guest`). This gap is
  S2's to close (P1).
- `bundle.inputs(run_dir)` does **not exist** (the outline's R6 input
  name has no code counterpart; R1 shipped the Writer-side counting
  instead). The manifest carries the slice fields the loader needs:
  `start_offset` (the byte size of log.jsonl when the session opened,
  bundle.py:451, log.py:91) and `log_rotations` (:452, log.py:94),
  with the documented slice order "log.jsonl.<log_rotations>.gz from
  start_offset, then every newer part, then log.jsonl"
  (bundle.py:448-452; the parts listing is viewrun.parts,
  viewrun.py:41-54). Seqs restart at 1 per session (log.py:108,
  `self.seq` starts 0), so `start_seq` alone cannot scope a reused
  dir — the offset is the scope.
- `Writer.__init__(cfg, run_dir, log, mode, scenario=None,
  start_seq=0, kind="run")` (bundle.py:329-331): `kind` lands in the
  manifest (:444) but there is no `of`/`elf`/`qemu-binary` — the
  replay adds them via a new optional `extra` kwarg (P1; readers key
  on `"v"` and ignore unknown fields, the D-PLn rule). The Writer is
  attached by every controller when `[bundle] enabled`
  (core.py:108-112) and closed before `log.close()` (core.py:519-523),
  so a replay controller produces decisions.jsonl with zero
  replay-specific code.
- `run_id(basename, started_utc)` (bundle.py:86): a stamp-prefixed
  basename is its own run_id (`STAMP_RE`, :34); `RUN_ID_RE` (:31) caps
  an id at 128 chars — the replay dir name must respect that.
- `set_verdict(run_dir, verdict)` (bundle.py:535) is the existing
  manifest-patch precedent (runmode.py:207).

Guest time and the re-feed paths
- `SimClock.at(sim_ns, cb)` (:132) heaps the deadline; `run_due()`
  (:144-158) fires deadlines **only while RUNNING** and only once a
  real vclock sample has passed them (:150-153) — the property the
  re-feed relies on. `now()` (:105) advances on the wall clock while
  OFF (the off-time folds in, :38-58), so bench time moves even with
  the power off; a replay's end condition on bench time cannot stall.
  `run_due` runs on the poller thread (core.py:420), so a `clock.at`
  callback must not run a slow verb inline (the dispatch queue, P2).
- Verb inputs re-dispatch through a real controller's dispatcher:
  `ctl.command(verb, args, source="scn")` (core.py:241) validates,
  logs `cmd` + `result`, and polls state — the same path a scenario
  step takes. `Dispatcher.run` refuses unknown verbs 404 and bad args
  400 (dispatch.py:88-99); a 409 (wrong power state, a sync in
  flight) is the re-feed's divergence signal, not a crash.
- TC inputs re-send the recorded hex through `Relay.gs_tc(sp,
  source=, path=)` (relay.py:173-198): the link-down policy
  re-applies (drop/hold, :177-195) and the record carries the hex +
  parsed (apid, seq, service, subtype, source, delivered). The source
  must NOT be `"tc"` (that is the real-GS marker `classify` keys on,
  bundle.py:187) — the replay uses `source="scn"`, `path="tc.replay"`.
  The recorded bytes keep the TC seq, so TM(1,x) ack matching
  reproduces (`decide`'s key set is fed from tc records with hex,
  bundle.py:214-223).
- The null-hex TC(9,129) (classify :177-180: `hex: None`, the
  recorded `seq`/`coarse`) re-runs `platform.timesync.send {coarse:
  <recorded>}` (plat.py:423; timesync.send accepts an explicit coarse
  and warns when its 6 LSBs miss, timesync.py:133-145). Its ack
  matches no hex-carrying tc → no ack decision in either run
  (documented loss, R1).
- Fault inputs re-run `fault.inject {id, params, seed, trigger}`
  (faults.py:212-248): the recorded resolved params + seed + trigger
  verbatim; an `at_sim` trigger re-arms and fires via `clock.at`
  (faults.py:235-237; the armed record :238-240; the inject record
  :281-285 carries no trigger); an immediate trigger injects now;
  `seed` is accepted as given (:218, min 0). A clear input re-runs
  `fault.clear {id}` (:322). `reapply` is never re-fed — the power
  hooks re-apply by themselves (plat.py:215-240).
- Force inputs: `value` present → `force.set {path, value}`
  (force.py:45), absent → `force.clear {path}` (:57).
- Power verbs re-dispatch as themselves; `cycle`'s off-time stays
  wall-paced by design (D-PLr — power.py:253 sleeps the recorded
  `off_time_s`), and the replay's own bench clock folds its off-times
  in the same way (simclock.py:38-58), so sim_times re-align within
  wall jitter.
- The keep-alive is NOT an input (summaries carry no hex, classify
  returns None, bundle.py:181-182): the replay's own keep-alive
  reproduces it from the recorded config. The automatic TC(9,129) IS
  recorded as an input (timesync.py:198-201) — so the replay must set
  `[timesync] autostart = "off"` or it would double-send (P2, D-e).

Bundle shape, dirs, files
- A closed manifest carries `t0` (power.boot's bench time,
  power.py:176-178), `final_sim_time`, `length_s` (bundle.py:441-442,
  455-457), `open` and the full launch `config` (`_public(cfg)`,
  :275-276 — including `ports`, `gs`, `timesync`, `qemu`, `mode`).
- Run dirs: run mode writes `<run_root>/runs/<stamp>-<name>/` with the
  retention marker (runmode.py:195-197); retention deletes only
  marker-carrying dirs (retention.py:113, :126) — a replay dir
  without a marker is never auto-deleted. The baseline is never
  doomed (retention.py:85, :101, :106-112).
- The flash start state is by reference: `flash.start.source` is
  `"library"` (name + the record with its sha256, bundle.py:469-473),
  `"carried"` (the origin dir reused a flash) or `"fresh"`.
  `platform.flash.load {name}` sparse-copies from
  `<run_root>/flash-library` and 404s an unknown name (plat.py:614-624,
  flashlib.py:189-209); the library is under the *replay's* run_root
  (flashlib.py:135). A passed run-mode origin may have had its
  flash.bin dropped (`keep_flash = "failed"`, retention.py:132-141),
  so a "carried" origin's flash may be gone.
- `det.tape.play {name}` reads `<tapes_dir>/<name>.json` — a shared
  dir outside every run dir (tapes.py:70-72), validated 400/404
  before any QMP (plat.py:523-542). A missing tape file is a
  verb-time 400.
- `run_dir.fresh` removes only the controller's named files
  (rundir.py:30); the replay's fresh dir starts clean. The run-dir
  flock (rundir.py:59-72) and guard (:39) apply to the replay's
  controller like any other.
- Exit codes: run mode's convention is 0 ok / 1 fail / 2 config
  error / 3 infra (runmode.py:13-14); the replay has no "fail"
  verdict — 0 completed, 2 refused, 3 infra.
- Self-test harness: `Env` (fake QEMU wrappers, private UDP ports,
  selftest.py:96-134; `free_udp_port` :88), the `_rec` record builder
  (:6117), the bundle lifecycle test's controller + `log.ingest`
  pattern (t_bundle_writer_lifecycle, :6317-6401), `CASES` (:6816),
  the `_dir_state` helper (used at :6744).

UNVERIFIED
- The live-check tolerances below (±2 s on event times) are
  expectations from the timing model (20 Hz sampling, QMP and UDP
  latency), not measurements; the first live run pins them.
- The collision behaviour when an operator forces the demo ports
  (8080/21502/21511) on a replay whose recorded config also names
  them: the plan re-draws the UDP ports (P2, D-f) and defaults the
  API port to a free one (P3), but no test exercises a forced
  collision.

## Global constraints

Inherited from the run-bundles plan (unchanged): stdlib only; API v1
additive only; **no new record kind, topic or source** (the re-fed
records are the existing `cmd`/`result`/`tc`/`fault`/`force` kinds via
the existing sources; the replay's own markers are `kind "log"`
records on paths `replay.*`); the listener rule untouched (the replay
adds no listener — its Writer is the S1 one); the flash is never
materialised (reproducing the flash start goes through
`platform.flash.load`'s sparse copy or a refusal — never a plain
copy, never a dense read); test/self-test configs never touch ports
8080, 21502, 21511, 21512 (API ports in the 18081+ band, UDP ports
from `free_udp_port`); no FSW edits; `python3 stixplat.py --self-test`
stays PASS and every task adds its own `check(...)` cases. Plus, for
S2:

- **The origin bundle is read-only.** The replay opens the origin's
  manifest and log, writes nothing into the origin dir, and never
  touches its marker or baseline (pinned by the `_dir_state`
  pattern).
- **Guest-time scheduling only.** A running/paused-phase input is
  dispatched exclusively through `clock.at` (fired only while
  RUNNING, simclock.py:150); nothing is fed "as soon as possible"
  while the bench is off or paused.
- **One input, one record.** Every skipped input produces exactly one
  `replay.skipped` log record; every failed dispatch exactly one
  `replay.failed`; a late dispatch exactly one `replay.late`. None of
  them is an input or a decision, and none is re-classified by the
  replay's own Writer (kind "log" classifies to None).
- **No wall TC is ever converted.** `--allow-wall` only lifts the
  pre-flight refusal; a wall input is skipped either way.

## Approaches considered

1. **Scheduling the re-feed.** (i) Feed everything immediately in seq
   order at replay start: rejected — the guest-time placement is the
   point (an `at_sim` fault or `keepalive.set` fed early lands at the
   wrong guest time). (ii) Sleep on the wall clock until each input's
   wall due time: rejected — wall ≠ bench; a pause would desync the
   whole schedule. (iii) Phase-split: off/frozen-phase inputs
   dispatched when due on the bench clock (they are pre-boot /
   between-boots actions), running/paused-phase inputs armed once on
   `clock.at` (chosen — simclock's RUNNING-only deadlines place them
   exactly where the sampler and keep-alive live, and bench time
   advances while off, so nothing stalls).
2. **Where the loader lives.** (i) `bundle.inputs(run_dir)` as the
   outline named it: rejected — bundle.py stays the S1 formats module
   and the name has no code counterpart (Key facts); the loader is a
   consumer. (ii) `replay.load_inputs` (chosen).
3. **The wall refusal.** (i) Skip wall TCs silently: rejected — a
   replay missing real GS TCs reads as a platform bug. (ii) Refuse
   unless `--allow-wall` (chosen, the outline's rule). (iii) An
   interactive prompt: rejected — run mode and gates are
   non-interactive.
4. **The replay's own bundle.** (i) Append into the origin's dir:
   rejected — mutates a read-only input and breaks the S3 pairing.
   (ii) A fresh dir + the standard Writer (chosen — S3 consumes two
   existing files with no new code).
5. **"Carried" flash.** (i) Sparse-copy the origin's flash.bin into
   the replay dir (rejected for v1 — a new copy path outside the
   library's record/sha discipline; open question 4). (ii) Refuse
   with guidance (chosen).

## Review focus (five input classes, each pinned to a test)

1. **Verb inputs** (the 14 + 4 re-fed verbs with recorded args at the
   recorded sim_time; excluded verbs and `param.*` never re-run):
   pinned by `replay_loader` (the ordered input sequence with exact
   dicts and phases) and `replay_run` (the replay's `cmd` records
   carry the recorded args; `det.tape.save` appears only as
   `replay.skipped`).
2. **TC inputs** (recorded hex re-sent via `gs_tc` source "scn" path
   "tc.replay"; hex null → `platform.timesync.send {coarse}`; wall
   and undelivered skipped with their whys): pinned by
   `replay_loader` (the three tc shapes incl. hex null),
   `replay_dispatch` (the mapping, offline), `replay_run` (the
   replay's tc record hex equals the recorded hex; the skipped
   lines) and `replay_cli` (the wall refusal without `--allow-wall`).
3. **Fault inputs** (armed→inject pairing, the recorded params + seed
   + trigger verbatim, clear re-fed, reapply/error never): pinned by
   `replay_loader` (the armed twin's trigger survives; the orphan
   inject's `{"at": "now"}`) and `replay_run` (the replay's fault
   record carries the same params/seed).
4. **Force inputs** (value present → set, absent → clear, reapply
   never): pinned by `replay_dispatch` and `replay_run`.
5. **File-referencing inputs and the log slice** (start_offset/
   log_rotations across a rotation; `det.tape.play`'s tape file; the
   flash start library/carried/fresh; a missing file or slice):
   pinned by `replay_loader_rotated` (the slice survives a rotation;
   a missing part refuses) and `replay_cli` (missing tape file,
   missing library entry, carried flash, missing slice → exit 2
   before any controller exists).

---

## Task P1: the classifier gap + the Writer's `extra`; the input loader

**Files:** modify `scripts/stix/platform/stixplat/bundle.py`; create
`scripts/stix/platform/stixplat/replay.py`; modify
`stixplat/selftest.py`.

**Interfaces — produces:**

- `bundle.py` (additive only; the S1 formats unchanged):
  - `REPLAY_VERBS` gains `"det.tape.play"`, `"det.tape.stop"`,
    `"det.align.set"`, `"det.adc.set"` (self-contained, guest-time
    verbs; the play re-reads its tape file at re-feed time).
  - `_EXCLUDED_VERB_WHY` gains: `"det.tape.save"`: "the tape file is
    by reference"; `"det.tape.delete"`: "would delete a file a play
    needs"; `"param.refresh"`: "a read-only RAM snapshot";
    `"param.read"`, `"param.read_all"`, `"param.set"`,
    `"param.reset_all"`: "the tc record is the input" (one entry per
    verb).
  - `Writer.__init__(..., kind="run", extra=None)`; `_manifest`
    merges `self._extra` first so the fixed keys always win
    (`out = dict(self._extra or {}); out.update(<fixed fields>)`).
- `replay.py`:
  ```python
  EXIT = {"ok": 0, "refused": 2, "infra": 3}

  class ReplayError(Exception):
      """A refusal in the pre-flight: exit 2."""

  def load_inputs(run_dir, manifest):
      """[(input dict)] in seq order: the manifest's log slice
      (log.jsonl.<log_rotations>.gz from start_offset, then the newer
      parts, then log.jsonl — viewrun.parts' order), each record fed
      through bundle.classify(obj, bundle.new_state(hk_codes)) with
      the origin manifest config's [bundle] hk_codes; each classified
      input gains seq, sim_time, boot_id (from its record) and phase
      ("off"|"frozen"|"running"|"paused" — the origin's power state at
      that record, tracked from the power records (event on ->
      frozen, boot -> running, off -> off) and the sim.pause /
      sim.resume cmd records). Raises ReplayError when the slice's
      first part is gone (rotated past [log] max_files - 1). Skips
      broken lines (the viewrun._stream rule, viewrun.py:132-140).
      Returns every classified input, the replay != "guest" ones
      included (the scheduler emits their replay.skipped)."""
  ```
  Loader only, no I/O beyond reading the origin dir; classify's state
  is local to the call (the armed/inject pairing needs it).

- [ ] Self-test `replay_loader` (RED: the module and the classifier
  entries do not exist). Front part, no controller (the
  `bundle_mod.classify` pattern of `t_bundle_inputs`): `cmd
  det.tape.play {"name": "t"}` → `replay: "guest"`; `cmd
  det.tape.save {"name": "t", "tape": {}}` → `replay: "no"`, why
  "the tape file is by reference"; `cmd det.tape.delete` → why
  "would delete a file a play needs"; `cmd param.set {"index": 1,
  "value": 2}` → why "the tc record is the input"; `cmd
  param.refresh` → why "a read-only RAM snapshot"; `cmd det.adc.set
  {"enabled": true}` and `cmd det.align.set {"enabled": false}` →
  guest. Main part, a real bundle (the `t_bundle_writer_lifecycle`
  pattern, Env + fake QEMU, `timeline={"sample_s": 0.5}`,
  `gs={"emulation": "on"}`): on; boot; `det.tape.save` +
  `det.tape.play` of a one-step tape (the `t_tapes_crud` save
  pattern); `fault.inject {"id": "obt.jump", "params": {"delta_s": 4},
  "seed": 99}`; `tc.gs.send {"service": 237, "subtype": 18, "data":
  "01383c00000000"}`; a synthetic null-hex tc record via `log.ingest`
  (path "platform.timesync", data `{service: 9, subtype: 129, seq: 5,
  coarse: 100, raw: false}`); a wall TC via
  `ctl.plat.relay.gs_tc(tcbuild.tc_packet(tcbuild.APID_TC, 3, 17, 1,
  b"\x02", ack=9, source=121))` (the default source "tc");
  `platform.spw.link {"state": "down"}`, a `tc.gs.send` (delivered
  false), link up; off; shutdown. Then `load_inputs(d, m)` and
  assert the classified inputs in seq order with their phases:
  power.on (off), power.boot (frozen), det.tape.save (no, its why),
  det.tape.play (running, guest), fault obt.jump (running: params
  {"delta_s": 4}, seed 99, trigger {"at": "now"}), tc guest hex
  (running), tc hex None seq 5 coarse 100 (running), tc wall
  (running, no why), spw.link (running), tc undelivered (running, why
  "never reached the FSW"), spw.link (running), power.off (running).
- [ ] Self-test `replay_loader_rotated` (RED): the
  `t_bundle_writer_rotation` config (`log={"max_log_mb": 1,
  "max_log_files": 5}`, `timeline={"sample_s": 0}`), spin + a few
  inputs (power.on/boot/off), shutdown (the log rotated,
  `log.jsonl.1.gz` exists); `load_inputs` still yields the session's
  inputs (the slice starts inside the gz part); then a doctored
  manifest with `"log_rotations": 9` → `raises(ReplayError)`; a
  manifest with `"open": true` → `raises(ReplayError)`; a manifest
  with `"t0": null` → `raises(ReplayError)` ("no boot record").
- [ ] Implement the bundle.py entries, the `extra` kwarg, and
  `replay.load_inputs` (+ `ReplayError`, `EXIT`).
- [ ] `cd scripts/stix/platform && python3 stixplat.py --self-test`
  → PASS (the existing `bundle_inputs` case keeps passing: no input
  shape changed, only set membership and new whys).
- [ ] Commit.

**Verification:** the two new cases PASS; `t_bundle_inputs` and
`t_bundle_writer_lifecycle` unchanged-PASS.

## Task P2: the scheduler, the re-feed, the replay's bundle

**Files:** `stixplat/replay.py`; modify `stixplat/selftest.py`.

**Interfaces — produces:**

- ```python
  def _feed_verb(ctl, inp): ...
  #   ctl.command(inp["verb"], inp["args"], source="scn")
  def _feed_tc(ctl, inp): ...
  #   hex:    ctl.plat.relay.gs_tc(bytes.fromhex(inp["hex"]),
  #                              source="scn", path="tc.replay")
  #   hex None: ctl.command("platform.timesync.send",
  #                         {"coarse": inp["coarse"]}, source="scn")
  def _feed_fault(ctl, inp): ...
  #   action "clear": fault.clear {"id": inp["id"]}; else
  #   fault.inject {"id", "params", "seed", "trigger"} verbatim
  def _feed_force(ctl, inp): ...
  #   "value" in inp: force.set {"path", "value"}; else
  #   force.clear {"path": inp["path"]}
  _FEED = {"verb": _feed_verb, "tc": _feed_tc, "fault": _feed_fault,
           "force": _feed_force}
  ```
  Every `_feed_*` wraps its call in `try/except VerbError` and, on
  failure, emits one `log` record `replay.failed {seq, status,
  error}` and returns (never raises into the clock callback, never
  aborts the replay). A tc feed that comes back `delivered: false`
  (the link-down policy re-applied) is not a failure — the origin
  recorded the same policy outcome.
- ```python
  def run(cfg, bundle_dir, elf=None, qemu_binary=None, port=None,
          allow_wall=False, on_controller=None, out=print,
          now=time.time):
      """One replay; returns EXIT["ok"|"refused"|"infra"], never
      raises. Refusals (exit 2, ReplayError inside): no/broken
      manifest; open manifest; t0 null; the log slice rotated away; a
      wall input and not allow_wall (the message names --allow-wall
      and counts the wall inputs); flash.start.source "library" and
      the entry missing from <retention.run_root>/flash-library (the
      message names the origin record's source_run_dir); source
      "carried" (always, open question 4); a det.tape.play input
      whose tape file is missing from tapes_dir. SIGTERM/SIGINT stop
      early (exit 0; the Writer's close still runs)."""
  ```
  Body (the order is the contract):
  1. read + validate the origin manifest; `load_inputs`; the
     file/flash pre-checks above (nothing created yet);
  2. the replay config:
     `rcfg = config_mod.merged(origin_manifest["config"], over)` with
     `over` = `{run_dir: {path: <new dir>, fresh: true}, timesync:
     {autostart: "off"}, ports: {api: port or a free one,
     qemu_tm/qemu_tc/gs_tc: a free UDP port each}}` plus
     `qemu.elf`/`qemu.binary` when the CLI overrides them, and
     `retention.run_root` from the *local* cfg (the replay dir and
     the flash library live under the local run root, not the
     origin's). The recorded `mode` stays (so `[gs] emulation =
     "auto"` resolves as recorded, plat.py:665-670); `gs.tm_dests`
     stays (open question 5).
  3. the replay dir `<local run_root>/runs/<stamp>-replay-<origin
     run_id>/` (`stamp` = the UTC time in runmode.py:150's format;
     the name truncated to 128 chars so `RUN_ID_RE` holds);
  4. `ctl = core.Controller(rcfg, run_dir=<the new dir>)` — its
     Writer writes the manifest with `kind "replay"` and `extra =
     {"of": <origin run_id>, "elf": <rcfg qemu elf>, "qemu-binary":
     <rcfg qemu binary>}`; any exception here → `EXIT["infra"]`
     (the binary guard, the flock, a bad config);
  5. flash start: source `"library"` →
     `ctl.command("platform.flash.load", {"name": ...},
     source="scn")` now (power off; a failure here is
     `EXIT["infra"]` — the pre-check passed, so this is a real
     error); `"fresh"` → nothing;
  6. `ctl.start(serve=True)`; `on_controller(ctl)` when given; the
     scheduler thread (below);
  7. the end condition: after the last input is dispatched, wait
     until `clock.now()[0] - t0 >= origin length_s * 1e9` (bench
     time; the origin's `length_s` = last sim_time − t0,
     bundle.py:441-442) — decisions keep arriving after the last
     input and S3 compares the whole sequence; then one `log` record
     `replay.done {inputs, dispatched, skipped, failed, late}` and
     `ctl.shutdown("replay done")`.
  - **The scheduler** (one daemon thread + one dispatch queue): wake
    ≤ 50 ms (the `clock.wake_in` cap); a cursor walks the inputs in
    seq order. Phase `off`/`frozen`: dispatch when
    `clock.now()[0] >= sim_time` **and** the replay's power state
    matches the phase (`off` → power off, `frozen` → frozen); when
    due but the state does not match, hold at the head of the queue
    (in order; the following power verb changes the state). Phase
    `running`/`paused`: armed exactly once via `clock.at(sim_time,
    release)` — `release` puts the input on the dispatch queue and
    the queue thread runs `_FEED`; the clock fires only while
    RUNNING, so a pause holds the schedule, and an input whose
    `sim_time` is already past when RUNNING begins fires on the
    first `run_due` after it (in due order), each such dispatch
    preceded by one `replay.late {seq, by_s}` log record (by_s = the
    dispatch bench time − sim_time, in ms). Skipped inputs
    (`replay != "guest"`) never dispatch: as the cursor passes them,
    one `log` record `replay.skipped {seq, why}` — `why` = the
    input's own `why`, or `"a real GS TC: wall-paced, not
    replayable"` for `replay: "wall"`.
- The replay's own manifest and decisions come from the controller's
  Writer (core.py:108-112): `inputs.counts` counts what the replay
  really re-fed (the wall/undelivered/excluded inputs are absent),
  `decisions.counts` the acks/events/hk steps its TM produced, `kind`
  "replay", `of`/`elf`/`qemu-binary` from `extra`. S3 (R7) consumes
  origin vs replay with no new code.

- [ ] Self-test `replay_dispatch` (RED; offline, stub objects): a
  stub `ctl` recording `(verb, args)` pairs and a stub relay
  recording `(sp, source, path)`: `_feed_tc` with the recorded hex
  calls the relay with exactly those bytes, source "scn", path
  "tc.replay"; `_feed_tc` with `hex: None` calls
  `command("platform.timesync.send", {"coarse": 100})`; `_feed_fault`
  inject passes id/params/seed/trigger verbatim, a clear input calls
  `fault.clear {"id": ...}`; `_feed_force` with and without
  `value`; `_feed_verb` passes args through with source "scn".
- [ ] Self-test `replay_run` (RED): replay the `replay_loader`
  origin with `allow_wall=True` → returns 0. Assert the replay's
  manifest: `kind == "replay"`, `of == <origin run_id>`, `elf ==
  "/dev/null"` (the Env ELF), `qemu-binary` endswith "qemu-new",
  `inputs.counts.fault == 1`, `inputs.counts.tc == 1` (the guest
  one; the wall and undelivered ones skipped),
  `inputs.counts.verb` holds platform.power.on/boot/off 1 each and
  platform.spw.link 2, `flash.start.source == "fresh"`, `open ==
  false`. Assert the replay's log holds, in seq order: `cmd
  platform.power.on`, `cmd platform.power.boot`, `cmd det.tape.play`
  (args name equal), a `fault` record action "inject" with the same
  params/seed, a `tc` record path "tc.replay" whose `hex` equals the
  recorded hex, `log` records `replay.skipped` for the tape.save cmd
  (its own why), the undelivered tc ("never reached the FSW") and
  the wall tc ("a real GS TC: wall-paced, not replayable"), `cmd
  platform.spw.link` ×2, `cmd platform.power.off`. Ack parity: the
  origin's `t_bundle_writer_lifecycle`-style synthetic TM(1,7) ack is
  reproduced in the replay via the `on_controller(ctl)` hook (the
  test's hook spawns a watcher thread that waits for the replay's
  `tc.replay` record and ingests the matching ack) — then the
  replay's decisions.jsonl holds exactly one `ack` line whose `key`
  equals the origin's ack key (the recorded bytes carry the seq).
  The origin dir is unchanged: `_dir_state(origin)` identical
  before/after.
- [ ] Implement the scheduler, `_FEED`, the flash pre-step, the end
  rule, `on_controller`.
- [ ] `python3 stixplat.py --self-test` → PASS.
- [ ] Commit.

**Verification:** `replay_dispatch` and `replay_run` PASS;
`t_bundle_*` unchanged.

## Task P3: the CLI — `--replay` with its refusals

**Files:** modify `scripts/stix/platform/stixplat.py`;
`stixplat/selftest.py`.

**Interfaces — produces:**

- `stixplat.py`: `--replay DIR`, plus new `--elf PATH`,
  `--qemu-binary PATH`, `--allow-wall`, and the existing `--port`
  generalised ("with --view-run or --replay: the API port",
  stixplat.py:55-56). `--replay` cannot combine with `--scenario`,
  `--variant`, `--view-run`, `--self-test` or `--check-ui`
  (`ap.error`, exit 2, the :52-54 rule). The local config still comes
  from `--config` (it supplies `retention.run_root` and the API bind;
  everything else on the replay bench comes from the origin
  manifest). `main` returns `replay_mod.run(cfg, args.replay,
  elf=args.elf, qemu_binary=args.qemu_binary, port=args.port,
  allow_wall=args.allow_wall)` — 0/2/3 per the run-mode convention
  (runmode.py:13-14). Usage line:
  `stixplat.py --config FILE --replay DIR [--elf] [--qemu-binary]
  [--port N] [--allow-wall]`.
- The refusal messages (each names the fix): "no run bundle at %r";
  "the bundle is still open"; "the bundle has no boot record (t0)";
  "the bundle's log slice has rotated away (log_rotations %d)";
  "%d wall-paced GS TC(s) (first seq %d): replaying omits them — pass
  --allow-wall to accept that"; "flash start is 'carried': no library
  entry to load (see platform.md)"; "no flash library entry %r under
  %s (the origin recorded it from %s)"; "tape file %r is missing from
  %s".

- [ ] Self-test `replay_cli` (RED): `replay.run` against crafted dirs
  (a manifest + a fake log; no controller needed — every refusal
  fires before the controller exists): each refusal returns
  `EXIT["refused"]` and its message names the fix (the wall one only
  without `allow_wall`); a healthy tiny bundle with
  `allow_wall=True` gets past the pre-flight — the test then stops it
  by making the controller construction fail (a run-dir path with a
  comma, refused by `rundir.guard`) and asserts `EXIT["infra"]`,
  pinning the 2/3 split.
- [ ] Implement the argparse block and the `main` wiring.
- [ ] `python3 stixplat.py --self-test` → PASS (no new verbs —
  `t_verbs_doc` untouched).
- [ ] Commit.

**Verification:** `replay_cli` PASS; `python3 stixplat.py --help`
shows the four new flags with their combination rules.

## Task P4: docs + the live check (reviewer)

**Files:** modify `docs/stix-port/platform.md`. **No new
accepted-diffs section**: S2 changes no QEMU or model behaviour and no
existing controller guarantee — it adds a module, a CLI flag, four
`REPLAY_VERBS` members and a handful of excluded-verb whys, all
documented in platform.md; §51-§53 exist because they changed a
behaviour or a guarantee, this does not.

- [ ] platform.md:
  - Usage: the `--replay` line with its flags and exit codes.
  - A "Replaying a bundle (S2)" subsection under Run bundles: what is
    re-fed per kind (verbs incl. the stimulus four; TC bytes via
    `tc.replay`; the null-hex TC(9,129) via `platform.timesync.send`;
    faults with params+seed+trigger; forces), what is skipped
    (wall/undelivered/excluded/`param.*` — each a `replay.skipped`
    record) and the `--allow-wall` rule; the phase-split scheduler in
    two sentences (off/frozen dispatched when due, running/paused on
    `clock.at`; a late input fires at the first RUNNING sample with a
    `replay.late` record); the replay dir
    `<run_root>/runs/<stamp>-replay-<origin run_id>/` (no retention
    marker — never auto-deleted, like an interactive dir), its
    manifest `kind "replay"` + `of`/`elf`/`qemu-binary`, and that S3
    compares the two decisions.jsonl files; the end rule (the
    origin's `length_s` of bench time after the last input); the
    refusals incl. the "carried" flash; the recorded-config rules
    (timesync autostart off, UDP ports re-drawn, the API on `--port`
    or a free one, the recorded `mode` kept); the determinism limits
    (below).
- [ ] Reviewer live check (never ports 8080/21502/21511/21512):
  1. Record a short run with a real QEMU (icount off, the default
     `pace = "wall"`): a smoke config with `[ports] api = 18081` and
     free 215xx slots, `[retention] run_root = "/tmp/stix-plat/rpl"`,
     `run_dir.path = "/tmp/stix-plat/rpl-live"`, the real ASW ELF;
     start it, then over the API: on, boot, `fault.inject {"id":
     "obt.jump", "params": {"delta_s": 4}}`, `tc.gs.send {"service":
     237, "subtype": 18, "data": "01383c00000000"}`, wait ~30 s, off;
     Ctrl-C the controller.
  2. Replay: `python3 scripts/stix/platform/stixplat.py --config
     replay.toml --replay /tmp/stix-plat/rpl/runs/<id> --port 18082`
     (replay.toml: the same run_root, nothing else). Expect exit 0.
  3. "Same decisions" means: the same `(kind, key)` sequence in the
     two `decisions.jsonl` files — every `ack` key equal (the
     recorded hex carries the TC seq), every `hk_step` `detail`
     (value, previous) exactly equal, and every `event`/`ack`
     `sim_time` within ~2 s of its origin's (wall-paced jitter: the
     20 Hz sampling, QMP and UDP latencies; the origin's own
     turn-to-turn times jitter by the same amount). NOT compared:
     absolute sim_time, seq, boot_id. Check:
     `python3 -c "import json,sys;a=[json.loads(l) for l in
     open(sys.argv[1]) if l.strip()];b=[json.loads(l) for l in
     open(sys.argv[2]) if l.strip()];print([(d['kind'],d['key']) for d
     in a]==[(d['kind'],d['key']) for d in b]);print([(d['kind'],
     d['key'],round((y['sim_time']-x['sim_time'])/1e9,2)) for x,y in
     zip(a,b)])" <origin>/decisions.jsonl <replay>/decisions.jsonl`
  4. Limits, stated in the doc: keep the recorded icount/pace (the
     replay inherits them; a `pace = "fast"` origin is replayed fast
     and its time-sync inputs may `late_apply`, as platform.md warns —
     record the live-check run with icount off); fault seeds are
     re-fed verbatim so seeded fault noise reproduces; the model's
     sky RNG is guest-time driven but the live check uses no tape, so
     the compared decisions are FSW reactions, not model draws; the
     power-off times stay wall-paced (D-PLr) and are the main source
     of the ±2 s.
- [ ] Commit.

**Verification:** the reviewer runs steps 1-3 and reads the printed
`True` plus the per-decision deltas; the first live run's measured
max delta is noted in the PR description (the plan's ±2 s is an
expectation, UNVERIFIED until then).

## Merge order

P1 → P2 → P3 → P4. Each task: its new self-test cases RED on the
parent (the module or flag missing), the whole self-test green after.
P3 touches `stixplat.py` only (no core/api change — no `gates2.sh`
needed, unlike R2/R3). Nothing in S1's formats changes; the S3 task
(R7) is untouched and needs no update.

## Deviations from the S2 outline (deliberate)

- **D-a.** The outline's input interface `bundle.inputs(run_dir)` has
  no code counterpart (R1 shipped the Writer-side counting and the
  `start_offset`/`log_rotations` manifest fields instead,
  bundle.py:451-452); the loader is `replay.load_inputs(run_dir,
  manifest)`.
- **D-b.** The replay manifest's `of`/`elf`/`qemu-binary` ride a new
  optional `Writer(extra=...)` kwarg (the outline fixed the fields
  but no mechanism; readers ignore unknown fields).
- **D-c.** The classifier gap is closed here: `det.tape.play/stop`,
  `det.align.set`, `det.adc.set` join `REPLAY_VERBS`; `det.tape.save/
  delete` and the five `param.*` verbs get their exclusion whys (the
  outline's fixed verb sets predate the stimulus and fsw-params
  verbs).
- **D-d.** A `carried` flash start refuses (the outline is silent);
  open question 4.
- **D-e.** The replay forces `[timesync] autostart = "off"`: the
  automatic TC(9,129) is itself a recorded input and would be sent
  twice otherwise (once by the replay's own autostart, once re-fed).
- **D-f.** The replay re-draws the three UDP ports with free ones
  (transport only — the recorded wall TCs are skipped and the GS
  emulation TCs originate in the controller); the recorded `mode` is
  kept so `[gs] emulation = "auto"` resolves as recorded.
- **D-g.** The loader returns *all* classified inputs, including the
  `replay: "no"|"wall"` ones (the outline's input shape has no such
  rule): the scheduler needs them in sequence to emit their
  `replay.skipped {seq, why}` records at the right position.

## Open questions for the owner

1. **Inputs whose `sim_time` is already past when the replay reaches
   RUNNING** (the replay's boot was slower than the origin's, or its
   off-time ran long). (a) Dispatch at the first opportunity in due
   order, each with a `replay.late {seq, by_s}` record (recommended —
   `clock.at`'s own semantics, no input lost, the delay visible in
   the log and countable in `replay.done`); (b) skip past-due inputs
   as `replay.skipped` (loses real inputs — a replay missing inputs
   reads as a platform bug); (c) refuse the replay up front when any
   input would land late (too brittle: ordinary boot-latency jitter
   trips it). The tasks implement (a).
2. **When a replay stops.** (a) Last input, then the origin's
   `length_s` of bench time, then `replay.done` + shutdown
   (recommended — decisions keep arriving after the last input and S3
   compares the whole sequence; bench time advances even while off,
   so the condition cannot stall); (b) stop right after the last
   input's `result` record (shorter, but the tail events/acks the
   origin recorded are lost); (c) additionally a `--duration S`
   override (can be added later without a format change). The tasks
   implement (a).
3. **`det.tape.save` during a replay.** (a) Excluded — the tapes dir
   is shared state outside every run dir; the play input re-reads the
   file at re-feed time, and re-saving could overwrite a tape the
   operator edited since (recommended, D-c); (b) re-feed it (a replay
   stays self-contained even after the tape file was deleted, but it
   silently rewrites shared files).
4. **A `carried` flash start.** (a) Refuse with guidance (recommended
   for v1 — no new copy path outside the library's record/sha
   discipline; the workaround is one `platform.flash.save` +
   re-record, and a passed run-mode origin often has no flash.bin
   left anyway, retention.py:132-141); (b) sparse-copy the origin's
   flash.bin into the replay dir before power-on (flashlib's `_copy`
   + sparse_check), recording `flash.start: {source: "carried-copy",
   of: <origin>}` — makes every bundle replayable, at the cost of a
   second copier.
5. **`gs.tm_dests` during a replay.** (a) Keep the recorded
   destinations (recommended — replaying into the same GS is a
   legitimate comparison, and a sendto to a GS that is not listening
   is already silent, relay.py:86-89); (b) strip them (a replay never
   feeds a GS — but then a GS-side comparison needs a hand-edited
   config).
6. **A missing IDB on the replay host** (`timeline.idb_json` /
   `idb_sqlite` from the recorded config; `idb.load` returns None,
   idb.py:148-171, so no `data.hk` and no `hk_step` decision).
   (a) Refuse at pre-flight (exit 2, naming the path) unless
   `--no-idb` is given, which records `idb: "absent"` in the replay
   manifest so S3 knows the hk_step rows are not comparable
   (recommended — never a silent loss); (b) warn only (a
   `replay.skipped`-grade log record) and run.

## Plan review (2026-11-15, GLM, findings folded in — these override the text above)

- R1 (minor): citation drift — "seq starts 0" is `log.py:79/:107`; the
  retention marker check is `retention.py:126` (doom at `:94/:103`).
- R2 (major): the replay's own manifest counts come from `classify` on
  the replay's own log (`bundle.py:397-399, 409-417`). The re-feed's
  `fault.inject`, `platform.timesync.send`, `force.*` and
  `platform.flash.load` go through `ctl.command` and appear there as
  excluded verb inputs, and the re-fed null-hex TC(9,129) is a new tc
  record. `replay_run` asserts key-scoped counts instead:
  `counts.tc == 2` for that fixture, `counts.verb["platform.power.on"]
  == 1`, and the expected excluded-verb entries listed explicitly; P2's
  "the wall/undelivered/excluded inputs are absent" is reworded to "the
  origin's skipped inputs are not re-fed (their `replay.skipped`
  records are the evidence)". Optionally (implementer's call, say
  which): tag re-feed commands `source: "replay"` so the classifier can
  report them separately — only if the Writer already keys on source.
- R3 (major): the IDB is a by-reference input; P2's pre-flight checks it
  next to the tape/flash files (owner question 6; the tasks implement
  6a: refuse unless `--no-idb`, manifest `idb: "absent"`).
- R4 (minor): `det.tape.play`'s pre-flight also resolves each step's
  `spec.file` CSV (`tapes.resolve`, plat.py:527-532); a missing
  spectrum is an exit-2 refusal, not a `replay.failed`.
- R5 (minor): `views_dir` and `tapes_dir` are forced to the replay's
  local run root like `retention.run_root`, so `ensure_starters`
  (core.py:96) never writes the origin's dirs. The tape pre-flight
  reads the ORIGIN's tapes dir (the recorded path) and copies the
  played tapes (and their spectra) into the replay's tapes dir, so the
  replay is self-contained and the origin's dirs stay read-only.
- R6 (minor): a free API port is probed (`free_udp_port`-style TCP
  probe); `ports.api = 0` is refused by `config._check`
  (config.py:177-179).
- R7 (minor): `replay_run`'s ack parity ingests the matching TM(1,7)
  into the origin before its power-off (the t_bundle_writer_lifecycle
  pattern, selftest.py:6333-6348), then compares the keys.
- R8 (note): the loader's phase tracker reads only power/sim.pause
  records; a QMP STOP (power.py:336-339) or HALT (power.py:355) leaves
  no input, and placement stays correct because deadlines are
  RUNNING-only.
- R9 (note): "same decisions" stays scoped as P4 says: hk_step values
  depend on the sky RNG when a tape plays, so the live check plays no
  tape. ±2 s stays UNVERIFIED until the first live run.

