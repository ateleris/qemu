# PL-11 remainder plan (2026-11-17)

Status: PLAN ONLY (nothing built). Refreshes
`2026-11-13-pl11-surfacing-plan.md` (owner-approved 2026-11-12/13) for
the commits it left undone: PL-11d..h. PL-11a, b, c are DONE
(`git log --grep PL-11`: a630933646, e61cb5c5e5, 44259441e6). Since
then the stimulus/spectra/single-events tracks (2026-11-15-*.md), the
run-bundle/replay/compare tracks and the UI port (U1-U3) changed the
platform a lot; every claim below was re-read in this tree on
2026-11-17.

Biggest change of premise: the old plan's O1 put every control verb
under `instrument.<unit>.<action>`. The owner then approved the
stimulus track, which shipped control verbs named `det.*`
(`stixplat/verbs.py:7-8`). Section 3 proposes names for the remaining
verbs following that precedent, as an open question.

## 1. What the 2026-11-13 plan gets wrong today

- **O1 / m8 / D-PSc's single `instrument.*` namespace** — superseded:
  the approved stimulus track ships `det.tape.*`, `det.align.set`,
  `det.adc.set`, `det.rotb.burst` (verbs.py:7-8; plat.py:300-337,
  447-465, 566-579). O1's `stim.det.* → instrument.detector.*` mapping
  never happened.
- **Coverage-matrix rows 6, 8, 13** ("attenuator/EDAC/det+rotb watch
  invisible") — built in PL-11b/c; the rows' "platform today" cites
  (state.py:31-75, core.py:25/178-180) are stale.
- **Row 5 (the O2 cold-cycle bug)** — fixed in PL-11a
  (faults.py:287-320, called from plat.py:229 and :235).
- **"EDAC seeded burst is debug.qmp-only today"** — now wrong in the
  other direction: debug.qmp refuses `/machine/stixctrl/edac` (exact
  match: core.py:30 + the 403 check at :282-284), so the burst has no
  surface at all until PL-11f.
- **Stale line numbers throughout** — the C and Python files moved:
  selftest.py:597-602 → 669-674 (debug.qmp 403 matrix), selftest.py:
  2316-2317 → 2500-2511 (the qom-path pin), grlib_stixctrl.c:533-577
  → 650-694 (HK name table), stix_det.c grew to 4986 lines (every det
  cite), state.py:94-101 → 177-184 (clear_hw), plat.py:120-133 →
  220-248 (_on_power), the verbs-table check selftest.py:2708 →
  3903-3921.
- **The fake** — m2's "fake still serves schema 1" is stale (it serves
  schema 5, fake_qemu.py:216) and m11's SETTABLE gap is closed
  (fake_qemu.py:43-46); the fake still has no flash device at all
  (only the flash.bin file creation, fake_qemu.py:143-148).
- **The UI** — U1-U3 re-laid it: the Faults tab now has category
  filter chips and a "what it does" column that still renders only
  `class, mode on target (changes, persistence)`
  (index.html:1516-1517); the forcing select still lists raw leaf
  paths (index.html:1729-1736); the state table columns are
  leaf/truth/presented/reported/status/changed (index.html:736-741).
- **platform.md cites** — the stale "attenuator's fault IDs are
  reserved in the catalogue's comments" sentence (old m10) is now at
  platform.md:431; the normative verbs table is platform.md:181-229
  (`<!-- verbs:begin -->`), the debug.qmp row :228, the state-leaf
  namespace table :162-171.
- **PL-11e's letter** listed `[qemu]` keys for all three replay knobs;
  only `det-replay-file` is startup-only (grlib_stixctrl.c:1798, the
  realize-time scan stix_det.c:4956-4972); `det-replay-trigger` and
  `det-replay-offset-ms` are runtime knobs that survive a device reset
  (stix_det.c:4563-4566, 4571-4577). Open question O-N2.

## 2. Status: the five commits and the coverage matrix

### Per-commit status

| commit | exists today (file:line) | missing |
|---|---|---|
| PL-11d flash watch + debug.qmp | nothing. No flash leaves (`leaves_from_hw`, state.py:47-158, maps no flash section); poll_state gets only the two hw-states (core.py:349-350); DEBUG_PATHS is still the two top paths (core.py:30) and the self-test 403 matrix still refuses `/machine/stixflash` (selftest.py:669-674) | all of PL-11d |
| PL-11e det control + `[qemu]` replay key | watch done (PL-11c leaves, state.py:96-121); tape/align/adc/burst control done (verbs.py:7-8; plat.py:300-337, 447-465, 566-579). No scene verb (det-source/-flare/-quiet-cps/-flare-cps have no platform surface; the only route today is debug.qmp on /machine/stixctrl), no inject verb (det-inject, stix_det.c:4207-4286), no replay start/stop verb (det-replay-start, stix_det.c:4435-4469), no `[qemu] det_replay_file` (config.py:29-43; qemu.py:56-77 passes only the flash/spw globals plus extra_args) | scene/inject/replay verbs, the config key, the UI scene+replay panel |
| PL-11f att cmdsta + EDAC control | nothing. `force.cmdsta-set`/`-clear` exist device-side only (stix_att.c:1102-1112) — absent from verbs.py and the fake's SETTABLE (fake_qemu.py:43-46); `edac-enable` (stix_edac.c:1410-1415), `upset-clear` (:1444-1447), `random-seed`/`random-count` (:1460-1473) likewise | all four verbs + fake support |
| PL-11g flash_damage rows | nothing. ACTIONS ends at `qom` (faults.py:48-49); REAPPLY_KINDS is `("qom",)` with the "flash_damage joins in PL-11g" note (:50-55); the D-X5 IDs are comment-only (faults-v1.json:17-31); the qom kind still refuses /machine/stixflash (faults.py:59-60, pinned selftest.py:2500-2511) | the kind, its validate branch, the three rows, the reapply "already active" tolerance, the fake flash stub |
| PL-11h labels + UI | nothing. No `label`/`description` fields (REQUIRED faults.py:65-67; no row carries them); preconditions/expected_detection are served but never rendered (index.html:1516-1517); no HK channel-name table in Python (names only in the C comments, grlib_stixctrl.c:650-694) | all of PL-11h |

### Coverage matrix (re-verified 2026-11-17)

capability | emulator knob (file:line) | platform today (file:line) | gap
---|---|---|---
process power/boot/pause | spawn `-S`, stop/cont (power.py) | platform.power.*, sim.* (core.py:123-134) | none
SpW link, time codes, OBT, TC(9,129)/TC(20,128) | link-down/timecodes/obt (grlib_stixspw.c) | plat.py:252-292, 401-429 | none
HK ADC forcing | adc-inject[0..127] (grlib_stixctrl.c:1859-1869) | force.py:38-67, re-applied cold+warm (force.py:69-76 via plat.py:228/:234) | none
faults (v1 catalogue) | device hooks | 25 rows: 8 platform/supply + 9 attenuator + 8 memory (faults-v1.json:35-308) | none for those rows
cold-cycle persistence of QOM faults | attenuator faults survive reset (stix_att.c:504-512); flash damage is runtime state | PL-11a built: Faults.reapply (faults.py:287-320), REAPPLY_KINDS (:55) | closed; flash rows join in PL-11g
attenuator watch | `hw-state` "attenuator" (stix_att.c:966-998, wired grlib_stixctrl.c:1727) | PL-11b built (state.py:131-142) | none (debug.qmp to the child stays refused until PL-11d)
attenuator control | faults-active/fault-clear/force.cmdsta-set/-clear (stix_att.c:1092-1112) | 9 catalogue rows ride the qom action (faults-v1.json:108-205; plat.py:751-767) | cmdsta forcing has no verb (PL-11f)
EDAC watch | `hw-state` "edac", poisons + per-mem counters (stix_edac.c:714-760) | PL-11b built (state.py:143-157) | none
EDAC control | upset-inject/upset-clear/upsets-active/edac-enable/edac-at-boot (stix_edac.c:1399-1452) | 8 `memory.*` rows inject via qom (faults-v1.json:217-308); clear dead for them (:206-216; the 409 pinned at selftest.py:2542-2544) | no edac-enable, no upset-clear verb (PL-11f)
EDAC seeded burst | random-seed (rw) / random-count (write-only) (stix_edac.c:1460-1473) | nothing (debug.qmp refuses the edac path, core.py:30) | gets a verb (PL-11f)
detector scene/inject | det-source/-flare/-quiet-cps/-flare-cps, det-inject (stix_det.c:4526-4559; setters :4066-4094, :4207-4286) | watch leaves only (state.py:98-101); control only via debug.qmp on /machine/stixctrl | scene + inject verbs (PL-11e)
DSS replay | det-replay-file startup-only (grlib_stixctrl.c:1798, scan stix_det.c:4956-4972); trigger/-offset-ms/-start/-status runtime (stix_det.c:4560-4596, start :4435-4469) | watch done (state.py:104-110); no config key, no start/stop control | PL-11e (file key + start/stop; O-N2)
det/rotb watch | hw-state schema 5 (grlib_stixctrl.c:1607), det accessor stix_det.c:4772-4847, rotb section :1716-1724 | PL-11c built (state.py:96-125), incl. the stimulus track's tape/align/adc blocks | none
flash watch | ops-read/prog/erase/abort + damage-list (grlib_stixflash.c:1749-1762) at the fixed path /machine/stixflash (leon3.c:516-519) | nothing; debug.qmp refuses the path (core.py:30; selftest.py:669-674) | PL-11d
flash damage control | damage-add/damage-remove/allow-reserved (grlib_stixflash.c:1767-1770, add :1547-1575, remove :1585-1621, allow-reserved :1717); offline tool scripts/stix/flash-damage.py | D-X5 IDs comment-only (faults-v1.json:17-31); the qom kind refuses the path (faults.py:59-60, pin selftest.py:2500-2511) | PL-11g
HK names/labels | channel names only in the C comments over stixctrl_hk_adc_table (grlib_stixctrl.c:650-694) | UI renders class/mode text (index.html:1516-1517) and raw forcing paths (:1729-1736); no label/description fields | PL-11h
realise-time config knobs | att-*/det-seed/-model/-idefx/-rotb, hk-adc/powerstat/hk-prot, rotb-blocks, flash latency/timings/allow-reserved, edac-at-boot (grlib_stixctrl.c:1771-1815; stix_edac.c:1399-1403) | `[qemu] extra_args` only (config.py:39, qemu.py:77); `edac-at-boot` also runtime-settable, deliberately no verb | accepted remainder (Later), unchanged
SCET inject, hk-prot line faults, rail cut, watchdog expire, ELUT read-back, qtest-only rx-inject | none built | — | unchanged MISSING (out of PL-11)
PL-8 `reported`, PL-10 gdb | — | leaf reported=None (state.py:38-44) | unchanged, other tracks

## 3. Naming for the remaining verbs

Prefixes in `VERBS` today (verbs.py:6-20): `platform.*`, `sim.*`,
`log.*`, `scenario.*`, `view.*`, `run.*`, `force.*`, `fault.*`,
`det.*`, `tc.*`, `param.*`, `debug.qmp`. Free: `att.*`, `memory.*`
(or `mem.*`), `flash.*` (`platform.flash.*` is taken — the flash
*library* save/load/list, plat.py:272-281 — so a bare `flash.*` verb
would read as the library; flash control stays fault rows anyway).
State leaves keep their existing prefixes (`instrument.*`, built in
PL-11b/c; the new flash watch leaves keep the old D-PSb choice
`flash.*` at the root).

Two options for the remaining control verbs:

| today's knob | option A: det.*-style short prefixes (recommended) | option B: old O1 `instrument.<unit>.<action>` |
|---|---|---|
| det-source/-flare/-quiet-cps/-flare-cps (stix_det.c:4526-4552) | `det.scene.set {source?, flare?, quiet_cps?, flare_cps?}` | `instrument.detector.scene {...}` |
| det-inject (stix_det.c:4553-4559) | `det.inject {fpga_det, pix, ebin, count}` | `instrument.detector.inject {...}` |
| det-replay-start (stix_det.c:4578-4583) | `det.replay.start` / `det.replay.stop` | `instrument.detector.replay {start}` |
| det-replay-trigger/-offset-ms (stix_det.c:4560-4577) | `det.replay.config {trigger?, offset_ms?}` (O-N2) | (startup keys only, per the old letter) |
| force.cmdsta-set/-clear (stix_att.c:1102-1112) | `att.cmdsta.set {value}` / `att.cmdsta.clear {value}` | `instrument.attenuator.cmdsta[.clear] {...}` |
| edac-enable (stix_edac.c:1410-1415) | `memory.edac.enable {module, on}` | `instrument.memory.edac.enable {...}` |
| upset-clear (stix_edac.c:1444-1447) | `memory.upset.clear {id}` | `instrument.memory.upset.clear {...}` |
| random-seed/random-count (stix_edac.c:1460-1473) | `memory.upset.burst {seed?, count}` | `instrument.memory.upset.burst {...}` |

Recommendation: **option A**, because the owner approved the stimulus
track's `det.*` verbs after O1, so the short-prefix style is the
established precedent; `det.tape.play`/`det.tape.stop` also settles the
start/stop shape. `att.*`/`memory.*` mirror it and collide with
nothing. Open question O-N1 records the choice.

## 4. Tasks

Commit order stays d → e → f → g → h (d first: the fake flash stub
gives g its device; e and f are independent of each other). Every
commit runs the full existing set: `stixplat.py --self-test`,
core-smoke, link-smoke (`--check-ui`), run-smoke, and the STIX smokes
its files touch (`boot-smoke`, `nominal-smoke`, `fault-smoke`,
`att-smoke`, `edac-smoke`, `detector-smoke`, `flash-damage-smoke`,
`align-smoke`, `tc-smoke` — all under `scripts/stix/`; the platform
ones under `scripts/stix/platform/tests/`). No commit below touches C,
so no golden is regenerated and no qtest pin moves.

### PL-11d: watch the flash + open the debug.qmp hatch (no emulator change)

- **Leaves.** `poll_state` (core.py:341-368) additionally qom-gets
  `ops-read`/`ops-prog`/`ops-erase`/`ops-abort` and `damage-list` from
  `/machine/stixflash` (read-only props, grlib_stixflash.c:1749-1762)
  into `flash.ops.read|prog|erase|abort` and `flash.damage` leaves.
  Each get is individually fault-tolerant in the same style as the
  hw-state try (core.py:348-354): a QMP error on one flash property is
  a `poller.error`, never a dead poll. The counters move only on flash
  ops and `damage-list` only on add/remove — no VOLATILE entries
  (state.py:19-35). `damage-list` is the device's rendered string
  (grlib_stixflash.c:1429-1478, rows like
  `src=runtime kind=stuck value=1 page=… bit=…`); parse it into
  `{src, kind, block|page, bit?, value}` records (O-N5) so the UI and
  compare can show rows, and keep the raw string as a second leaf
  `flash.damage.raw` for greppability.
- **clear_hw.** `state.clear_hw` (state.py:177-184) gains the `flash.`
  prefix, so the leaves drop at power off like the instrument ones
  (old m4).
- **debug.qmp.** `DEBUG_PATHS` (core.py:30) per D-PSd becomes the five
  fixed paths: `/machine/stixspw`, `/machine/stixctrl`,
  `/machine/stixctrl/attenuator`, `/machine/stixctrl/edac`,
  `/machine/stixflash`. Exact match stays (no children, no patterns —
  a stray `/machine/` path keeps 403ing); same `[debug] raw_qmp` gate,
  run-mode gate and sticky taint (core.py:269-299).
- **platform.md.** The leaf namespace table (:162-171) gains the
  `flash.*` row; the `debug.qmp` verb row (:228) says the five paths.
- **Fake.** fake_qemu.py has no flash device (it only creates
  flash.bin, :143-148, and its qom-get falls through to KeyError,
  :389-393): add a stub that serves the five properties, stores
  `damage-add`/`damage-remove` (reject a duplicate spec with the
  device's exact text — PL-11g's gate needs it), renders `damage-list`
  in the device's line format, and bumps `ops-*` only when told (an
  `x-fake-flash-op` test command).
- **Self-test.** The 403 matrix (selftest.py:669-674) flips:
  `/machine/stixflash` moves from the refused list to an accepted
  qom-get; `/machine`, `/objects/x` and the `/machine/stixspw/../
  stixflash` traversal form stay 403, as do the four other paths'
  children (e.g. `/machine/stixctrl/edac` is now accepted,
  `/machine/stixspw/sub` still 403). Plus: leaf shapes/values from the
  fake, the ops counter moving on `x-fake-flash-op`, damage add/remove
  reflected in `flash.damage`, `clear_hw` dropping `flash.*`, taint
  unchanged.
- **Gates:** self-test; core-smoke + link-smoke `--check-ui` (uicheck
  needs no change — no new data-verbs); run-smoke unchanged.

### PL-11e: control the detector scene, inject and replay

Names per option A (O-N1); each verb lands with its `d.add`
(plat.py, next to the det verbs at :300-337), verbs.py entry,
platform.md `verbs:begin` row (selftest.py:3903-3921 pins the
equality) and fake support (SETTABLE, fake_qemu.py:43-46).

- **`det.scene.set {source?, flare?, quiet_cps?, flare_cps?}`** —
  qom-sets only the given args (up to 4 sets; D-PSc's "one verb,
  minimal sets", m12), on `/machine/stixctrl` via `_qom_set`
  (plat.py:107-111, the path parameter is already there). Validation:
  source enum `zero|synthetic|replay-dss` (stix_det.c:4529-4532),
  cps 0..1000000 (stix_det.c:993-995); the device's own refusals
  surface as 409 — `replay-dss needs a replay table (set
  det-replay-file at startup)` (stix_det.c:4078-4082).
- **`det.inject {fpga_det, pix, ebin, count}`** — one qom-set of
  `det-inject` with `fpga_det=F,pix=P,ebin=B,count=N`
  (stix_det.c:4207-4286); validate controller-side: fpga_det 0..31,
  pix 0..11, ebin 0..31, count >= 1 (count=0 is a device error,
  :4276-4278). Index spaces named per the stimulus track's rule.
- **`det.replay.start` / `det.replay.stop`** — qom-set
  `det-replay-start` true/false (stix_det.c:4435-4469; a start needs
  `det-source=replay-dss`, :4451-4454, and a table, :4446-4450; a stop
  applies at once). Scenarios `await_state` on
  `instrument.detector.replay.state` (state.py:104-110).
- **`det.replay.config {trigger?, offset_ms?}`** (O-N2) — qom-sets
  `det-replay-trigger` (`nominal|qmp`) and `det-replay-offset-ms`
  (uint64; the device rejects past 2^32 ticks, stix_det.c:4354-4358).
  1-2 sets.
- **`[qemu] det_replay_file`** (O2, D-PSg) — a string, default ""
  (config.py qemu table :29-43); when set, build_argv (qemu.py:36-77)
  appends `-global grlib-stixctrl.det-replay-file=<path>` next to the
  other `-global`s (:56-65); made absolute like qemu.elf (QEMU's cwd
  is the run dir, qemu.py:201-204). A bad file fails QEMU's realize
  (stix_det.c:4956-4972), so power.on reports the exit — document that
  in platform.md's config table row.
- **UI** (the mockup's scene + DSS-replay panels, stimulus plan lines
  84-86, not built yet): a scene section on the Detectors tab —
  source select, flare checkbox, quiet/flare cps inputs, apply
  (`data-verb="det.scene.set"`); replay start/stop buttons and the
  `instrument.detector.replay.*` readouts (the state table already
  shows them; add the two buttons `det.replay.start`/`det.replay.stop`
  and a trigger/offset row). Every new data-verb must be served —
  uicheck enforces it (uicheck.py:26-27, 67-68).
- **Fake.** SETTABLE += `det-source`, `det-flare`, `det-quiet-cps`,
  `det-flare-cps`, `det-inject`, `det-replay-start`,
  `det-replay-trigger`, `det-replay-offset-ms`; the hw-state `det`
  section (fake_qemu.py:235-244) echoes the stored scene and flips
  `replay.state` on start/stop (with a no-table refusal mirroring the
  device's error text).
- **Self-test.** Arg validation (the enum, cps bounds, inject bounds,
  offset cap), the partial-set rule (only given args are set — assert
  the fake's set log), scene.set → the leaves move, inject → the
  leaf/SSID path stays as today's qtest covers the real device, replay
  start without a table → 409, table/fake/verbs equality.
- **Smoke gate.** Extend the shipped detector scenario: a
  `gate-det-scene` variant on `gate-det-tape.json` (or a sibling
  file — implementer's choice, one scenario) that sets
  scene=synthetic with a raised flare cps, awaits the QL counts move,
  injects one det.inject deposit and asserts the leaf; registers in
  run-gates.toml:32-39 and run-smoke.sh:61-65. `detector-smoke.sh`
  (which drives QMP directly) stays unchanged — no C change.

### PL-11f: control the attenuator and the EDAC

- **`att.cmdsta.set {value}` / `att.cmdsta.clear {value}`** — qom-set
  `force.cmdsta-set` / `force.cmdsta-clear` on
  `/machine/stixctrl/attenuator` (stix_att.c:1102-1112): bits 11:5
  forced to 1 (resp. 0) in the 0x5C readback; validate value 0..0xFFF.
  The bench's only attenuator *command* path that needs no PFM ELF
  (the TC(236,4/5) route is FSW-owned — decision O3 stands). The
  device clears the force on reset (stix_att.c:1106, 1111-1112); the
  controller deliberately does **not** re-apply it across cycles
  (O-N4) — unlike adc-inject, no D-PLf decision covers it and the
  device's own rule is "clears on reset".
- **`memory.edac.enable {module, on}`** — O4 verbatim: module
  `all|sdram|cache|rotbuf` → qom-set `edac-enable` `"all-on"` / `1` /
  `2` / `4` on `/machine/stixctrl/edac` (stix_edac.c:1410-1415). The
  property only sets, never clears (the setter ORs the EdacOn bits,
  :1285-1352), so `on: false` is a controller-side 400 and never
  reaches the device; the device's own refusals — an empty mask
  (:1328-1330) or an unwired register owner (:1334-1340) — surface as
  the verb's 409. No leaf shows Edacon (the hw-state carries poisons
  and counters only, stix_edac.c:744-756): the verb's effect is
  observable through `memory.sdram.seu`'s precondition going
  satisfiable — the live part stays [L].
- **`memory.upset.clear {id}`** — `"all"` or an index into
  `instrument.memory.poisons` → qom-set `upset-clear`
  (stix_edac.c:1444-1447); 409 while STIX is off. Completes the memory
  rows, whose catalogue `clear` is deliberately dead
  (faults-v1.json:206-216; the 409 pinned at selftest.py:2542-2544
  stays — it pins fault.clear, not the verb).
- **`memory.upset.burst {seed?, count}`** — `seed` → qom-set
  `random-seed` (0 = off, survives reset, stix_edac.c:1460-1464),
  `count` → qom-set `random-count` (write-only, :1465-1473); one verb,
  1-2 sets. Stays catalogue-less ("one action sets one property",
  faults-v1.json:214-216); debug.qmp keeps its (now widened) hatch.
- **Fake.** SETTABLE += `force.cmdsta-set`, `force.cmdsta-clear`,
  `edac-enable`, `random-seed`, `random-count`; the fake's poisons
  react to `random-count` only if it draws (simplest: the fake models
  the burst as `count` poison records drawn deterministically from the
  seed over {sdram, cache, rotbuf} — enough for the leaves to move;
  the real draw rules are qtest-covered).
- **Self-test.** Paths and recorded sets, 409s when off, the
  `on: false` 400, `memory.upset.clear` all/index/out-of-range, the
  burst's leaves moving, table/fake/verbs equality.
- **Smoke gate.** Extend `gate-fault-reapply.json` (run-smoke already
  runs it, run-smoke.sh:61-65) with a variant that
  `memory.edac.enable`s sdram, injects `memory.sdram.seu`, awaits the
  counter leaf, clears via the new verb, then a fixed-seed
  `memory.upset.burst` and asserts `instrument.memory.poisons` gains
  rows — the [L] live leg the owner runs once.
- **platform.md.** Four verb rows + a sentence that cmdsta forces and
  Edacon are not re-applied across cycles (cmdsta clears by design;
  Edacon the FSW or a reset owns).

### PL-11g: adopt the D-X5 flash fault rows (the `flash_damage` action kind)

- **faults.py.** `ACTIONS` += `"flash_damage"` (:48-49);
  `REAPPLY_KINDS` += `"flash_damage"` (:55 — the comment at :50-54
  already promises this). A strict `flash_damage` branch in
  `validate()` per D-PSe: `path == "/machine/stixflash"`, the spec's
  shape (allowed keys, kind enum erase-fail | program-fail | stuck —
  the device's parser grlib_stixflash.c:1507-1538 is the truth, the
  `stuck0` alias is accepted device-side), `stuck` requires bit+value,
  and every `{"param": name}` reference exists and is integer-typed
  (the params rule :112-115 unchanged). `QOM_PATH` (:59-60) is NOT
  widened; the qom kind still refuses `/machine/stixflash`
  (selftest.py:2500-2511 stays as the pin, D-PSe).
- **plat.py `_fault_actions`** (:697-778): `flash_damage` apply builds
  the spec JSON string from the entry's spec + resolved params and
  qom-sets write-only `damage-add` (no getter exists, :1767-1768 — no
  prior read, `"clear"` unused); undo rebuilds the same string and
  qom-sets `damage-remove` (the device's exact-spec removal,
  :1585-1621).
- **faults-v1.json.** `flash.block.erase_fail`,
  `flash.page.program_fail`, `flash.cell.stuck` become real rows
  (class memory, changes truth, until_cleared, reversible; params
  block|page (+bit, value) in the FSW-linear index space — the
  damage-list getter's linearisation, grlib_stixflash.c:1441-1445).
  The reservation comment (:17-31) shrinks to the three offline rows
  (:28-31).
- **Reapply tolerance** (owner decision 2026-11-13): D-PSa's reapply
  re-runs apply; on a warm reset the device rejects the duplicate with
  exactly `stixflash: damage-add: the spec is already active`
  (grlib_stixflash.c:1560-1565) — the reapply path matches that error
  text and logs the row as `already active` (not a reapply-error, not
  a damage-list pre-check); any other damage-add error at reapply is a
  `reapply-error` and never wedges power-on. On a cold cycle the fresh
  process's list is empty and the add just lands — O2 closed
  end-to-end.
- **Fake.** The PL-11d flash stub gains the add/remove/duplicate/
  list behaviour above; the fake's RESET keeps runtime damage (the
  real device's list is plain runtime state — it survives a guest
  reset and dies with the process), so the warm-then-clear gate is
  meaningful.
- **Gates.** The catalogue rules: `stixplat.py --self-test` green with
  validate negatives (a `/machine/stixflash` row with `kind: qom`
  still refused; a malformed spec refused; a non-integer param
  refused); `fault.clear` of a flash row runs damage-remove and the
  `flash.damage` leaf loses the row (not only the inject); the
  gate-fault-reapply scenario extended with `flash.cell.stuck`
  (inject, cold cycle → reapply, clear); a reserved-block inject
  without `allow-reserved` fails at the device with nothing armed
  (grlib_stixflash.c:1717 — the controller never sets it, D-PSe);
  `flash-damage-smoke.sh` unchanged (offline tool); platform.md's
  stale :431 sentence fixed and the catalogue section (:380-435)
  documents the kind and rows.

### PL-11h: labels, descriptions and the UI

- **Catalogue.** Optional `label` (short, the row title) and
  `description` (one sentence) — validated as strings when present
  (D-PSf), `REQUIRED` unchanged (faults.py:65-67). The shipped rows
  get one-line descriptions; the attenuator rows' PFM-ELF caveat moves
  into `description` and stays as `preconditions`
  (faults-v1.json:116-117 and friends).
- **HK channel names.** One Python table (new `stixplat/hknames.py` or
  a state.py constant) transcribed from the C comments over
  `stixctrl_hk_adc_table` (grlib_stixctrl.c:650-694: channels 0-4, 8,
  12, 16, 20, 24, 28, 32-36, 40, 44, 48, 52, 56, 60, 64-79 — the
  table cites its source). Served as `label` on the
  `instrument.hk_adc.N` leaves (state.py:71-74; an additive leaf-dict
  key — clients ignore unknown fields, platform.md:233-240). The
  self-test asserts the table covers exactly the channels the hw-state
  can serve (grlib_stixctrl.c:1637-1644 emits a channel when its
  table entry is non-zero or an injection is active), so a
  transcription slip cannot pass silently (O6).
- **UI** (index.html): the fault row shows `description` when present
  (else today's class/mode line), then `preconditions` and
  `expected_detection` as list items (template :648-662, renderer
  :1502-1555); the forcing select shows `ch 8 — ATT_C` style labels
  (:1729-1736); the State/Detector tables show the label as the leaf
  cell's title. No new data-verbs.
- **Gates.** The catalogue rules as PL-11g (validate negatives for a
  non-string label/description); self-test UI checks (the CSP is a
  hash of the served file — any index.html edit must keep uicheck's
  CSP check green, uicheck.py:67-68); `--check-ui` on core-smoke and
  link-smoke; platform.md's catalogue (:380-435) and web-UI sections
  updated.

## 5. Global constraints that still hold

- **No golden regeneration on C-touching commits** — moot for d..h as
  planned (none touches C), but the rule stands if one grows a C hunk.
- **Additive under API v1** (platform.md:231-240): new verbs, fields,
  kinds and topics are fine; no renames, no removals. The
  `instrument.detector.quarter.*` paths stay as built; the leaf-dict
  gains only the additive `label` key (PL-11h).
- **One verb, minimal qom-sets** (D-PSc as corrected by m12): scene
  up to 4, burst 1-2, everything else 1.
- **Strict validation**: every catalogue change keeps `faults.load()` +
  `validate()` green and integer-only params (faults.py:81-144); every
  new verb validates args before any QMP.
- **The four-place verb landing**: `d.add` in plat.py, verbs.py VERBS,
  a platform.md `verbs:begin` row (t_verbs_doc pins the equality,
  selftest.py:3903-3921), and fake support in the same commit; any UI
  button goes through `data-verb` and uicheck (uicheck.py:26-27, 67-68).
- **Fake drift**: every leaf/verb commit extends fake_qemu.py and the
  self-test expectations together, or the gates lie.
- **Scope guard** (unchanged): SCET inject, hk-prot line faults, rail
  cut, watchdog expire, ELUT read-back and runtime replay-file load
  stay out; `edac-at-boot` keeps "no verb" (boot-time semantics,
  Later).

## 6. D-PS decisions: kept, changed, dropped

| decision | status |
|---|---|
| D-PSa re-apply rule | built (PL-11a); PL-11g adds `flash_damage` to REAPPLY_KINDS with the "already active" tolerance |
| D-PSb one watch idiom | kept; the flash part (watch via the QOM props, no flash hw-state section — that stays an emulator-side follow-up) is exactly PL-11d |
| D-PSc control idiom | kept except the namespace sentence (O1 superseded by the det.* precedent, O-N1); `_qom_set(path=…)` already exists (plat.py:107-111) |
| D-PSd debug.qmp whitelist | kept as decided (five fixed paths); note the implementation is exact-match, so "children of a listed path allowed" from the old text is wrong — children stay refused |
| D-PSe flash_damage kind | kept verbatim (PL-11g); the reapply tolerance note already sits in faults.py:50-55 |
| D-PSf labels | kept (PL-11h); O6's "C comments are the source" stands |
| D-PSg replay file startup-only | kept, narrowed: only the file is a `[qemu]` key; trigger/offset are runtime knobs and ride `det.replay.config` (O-N2) |
| O1 verb namespace | superseded for the detector by the shipped `det.*` verbs; O-N1 asks whether the rest follows |
| O2 replay file | kept (startup config only) |
| O3 attenuator control | kept (cmdsta forcing only; labelled TC(236,4/5) steps stay a later track) |
| O4 EDAC enable | kept: one verb, only-sets semantics, module mask bits 1/2/4, `all` → "all-on" (stix_edac.c:1285-1352) |
| O5 re-apply opt-out | built (PL-11a); no per-entry opt-out |
| O6 HK labels | kept (PL-11h) |

## 7. Open questions for the owner

- **O-N1 verb namespace.** Option A: follow the shipped `det.*`
  precedent — `det.scene.set`, `det.inject`, `det.replay.start/stop/
  config`, `att.cmdsta.set/.clear`, `memory.edac.enable`,
  `memory.upset.clear/.burst` (section 3). Option B: the old O1
  `instrument.<unit>.<action>`. **Recommend A** (the owner approved
  `det.*` after O1; consistent, shorter, nothing collides).
- **O-N2 replay config surface.** (a) `[qemu] det_replay_file` only +
  `det.replay.config {trigger?, offset_ms?}` for the two runtime
  knobs; (b) the old letter: all three as `[qemu]` keys. **Recommend
  (a)** — trigger/offset are runtime device knobs that survive a
  reset (stix_det.c:4563-4566, 4571-4577); only the file needs the
  spawn line.
- **O-N3 det.inject.** Keep the raw one-shot deposit verb, or drop it
  (the Stimulus board's "Send now" already covers one-shot stimulus
  through a one-step tape, index.html:2257-2281)? **Recommend keep** —
  it is the only direct accumulator deposit with no tape file, it was
  in the approved PL-11e scope, and it is one thin qom-set.
- **O-N4 cmdsta re-apply.** Re-apply `att.cmdsta.*` across power
  cycles like adc-inject, or leave it (the device clears the force on
  reset by design, stix_att.c:1106/1111-1112)? **Recommend leave** —
  document "clears on reset" in platform.md; no D-PLf change.
- **O-N5 flash.damage leaf shape.** (a) parsed records
  (`{src, kind, block|page, bit?, value}`) + a raw-string leaf; (b)
  raw string only. **Recommend (a)** — the UI and compare want rows;
  the parser is 20 lines over a stable format (grlib_stixflash.c:
  1450-1475).

## 8. Review focus (the 5 places most likely to be wrong)

1. **PL-11d's poll integration.** poll_state runs at `state_hz` and
   must never die on the new device: the fake answers unknown
   qom-gets with KeyError (fake_qemu.py:389-393), so a missing stub
   or a non-tolerant get turns every poll into a `poller.error` and
   silently breaks dozens of unrelated self-test cases. Each flash
   get needs the core.py:348-354 treatment, and the fake needs the
   stub in the same commit.
2. **PL-11g's duplicate-spec tolerance.** The warm-reset gate is only
   honest if the fake reproduces the device's exact error text
   (`the spec is already active`, grlib_stixflash.c:1562-1563) and the
   reapply path matches on it; otherwise the gate logs reapply-error
   and passes while the real device path is untested. The owner
   decision pins textual matching — no damage-list pre-check.
3. **PL-11f's module/bit map and the only-sets rule.** `rotbuf` (the
   verb arg and device grammar) vs the leaf `instrument.memory.rotbuf`
   vs mask bit 4; `on: false` must 400 at the verb and never reach the
   device; "all" maps to the string "all-on", not a mask. A wrong map
   enables the wrong module silently.
4. **The four-place verb landing.** A verb missing from any of
   verbs.py (run mode refuses it), the platform.md verbs:begin block
   (t_verbs_doc compares exact sets, selftest.py:3903-3921), the fake
   SETTABLE, or the UI's data-verbs (uicheck) fails only in the
   reviewer's gate run — check all four per verb before handing over.
5. **PL-11h's additive leaf key.** `label` rides every leaf dict:
   snapshot records carry whole leaves (core.py:405-412), the compare
   series rows read leaf values, and the UI's `isLeaf` sniffs the
   shape (index.html:1701-1704). The key must be set only on
   `instrument.hk_adc.*` leaves, never change `truth`/`presented`
   typing, and the hknames table must cover exactly the servable
   channels (grlib_stixctrl.c:1637-1644) or the count self-test is
   vacuous.

## 9. Plan review (2026-11-17) — resolutions (these override the task text above)

Independent review; the four MAJORs were re-checked against the code by
the integrator. No BLOCKER.

| # | finding | resolution |
|---|---|---|
| R1 MAJOR | cmdsta bounds: the device refuses any bit outside 11:5 (`STIX_ATT_FORCE_MASK 0xFE0`, "has bits outside 11:5", stix_att.c:875, 902-903) | `att.cmdsta.set/.clear` validate `value & ~0xFE0 == 0` controller-side (400), not 0..0xFFF |
| R2 MAJOR | stuck grammar: `value` (0/1) is required for every stuck; `bit` is a STRING `"byte.bit"` required for page scope and forbidden for block scope; erase-fail is block-only, program-fail page-only (grlib_stixflash.c:1094-1298, kind checks ~1259-1266, bit ~1277-1295) | the `flash_damage` validate branch mirrors exactly these rules (scope from block vs page key; stuck ⇒ value; page stuck ⇒ bit; block stuck ⇒ no bit; erase-fail ⇒ block; program-fail ⇒ page). The device's parser (stixflash_dmg_entry) is the truth, cite it |
| R3 MAJOR | integer-only params cannot express `"byte.bit"` | the spec leaf `bit` takes `{"byte": <int or param>, "bit": <int or param>}`; apply composes the device string `"<byte>.<bit>"`; validate bounds byte < page size and bit 0..7 when fixed. Params stay integer-only (faults.py:112-115 unchanged). `flash.cell.stuck` is page scope with params page, byte, bit, value |
| R4 MAJOR | PL-11d poll error rule contradicted its cite (core.py:351-352 returns silently on QmpError) | the flash gets run in their own block AFTER the hw-state update: each get catches QmpError/ValueError and calls `_poll_error("flash", e)` (deduped, core.py:370-374), then the poll continues; a flash error never skips the hw-state leaves. A missing QMP (`qmp_alive` false) still returns early as today |
| R5 | snapshots copy only {truth, presented} (core.py:407-408) | claim corrected: `label` never rides snapshots; add platform.md:155-157 (leaf shape sentence) to PL-11h's edit list |
| R6 | CSP cite | CSP hashes are uicheck.py:47-58 / api.py:52-64 and recompute at serve time; the gate that bites is the data-verb check |
| R7 | flash knob cite | flash latency/timings/allow-reserved are grlib_stixflash.c:1709-1728 |
| R8 | parser cites | spec grammar = stixflash_dmg_entry 1094-1298 (enum text 1140-1142); damage-list renderer 1450-1475 prints `bit` as `byte.bit` — the parsed `flash.damage` record carries `bit: "byte.bit"` (string) |
| R9 | PL-11d 403 matrix wording | accepted (exact): the five paths. Refused: `/machine`, `/objects/x`, `/machine/stixspw/../stixflash`, any child such as `/machine/stixspw/sub` or `/machine/stixflash/x` |
| R10 | hk names vs the fake's ch 5 | the names table covers the C table's named channels; the self-test checks (a) every channel the REAL table can serve (non-zero entries in grlib_stixctrl.c:650-694, parsed from the C file at test time) has a name, and (b) the fake serves only named channels — the fake's ch 5 moves to a named channel |
| R11 | edac-enable is a string property | send `"all-on"`, `"1"`, `"2"`, `"4"` as strings |
| R12 | `memory.*` verb prefix shares a word with fault IDs and leaves | verbs use `mem.*` (`mem.edac.enable`, `mem.upset.clear`, `mem.upset.burst`), see O-N1 |

O-N1 updated recommendation: option A with `mem.*` for the memory verbs.
