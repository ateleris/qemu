# Platform track PL-11: surfacing every emulator knob in the controller

Status: draft, 2026-11-13; reviewed by GLM 2026-11-12 — 4 MAJOR + 13
MINOR findings, all resolved (see Review resolutions; every finding was
re-verified against the tree before the fix). Base: track worktree
`~/dev/qemu-stix-platform` (branch `stix-uprev-pl11`, integration tip
`a5dc6d5c11`), which already carries the attenuator (A-1..A-5), flash
damage (X-1..X-5), memory upsets (M-1..M-7) and detector (D-1..D-7)
tracks. This plan does not build anything itself; it plans PL-11.

Sources:
- the GLM review of this plan, `~/dev/stix-state/tools/glm/
  pl11-plan-review.md` (2026-11-12; resolved — see Review resolutions);
- the GLM audit `~/dev/stix-state/tools/glm/plat-audit-result.md`,
  written at `083d3d4365` (before att/flash/mem/det merged); every row
  has been re-verified against this tree — see the matrix, and "What
  the old audit got wrong" at the end;
- this repo: `scripts/stix/platform/`, `docs/stix-port/platform.md`,
  `hw/sparc/` ([C] = read in code, [M] = read in this repo, [D] = read
  in a sibling plan, [A] = an assumption to accept, [L] = verify live
  before it becomes a gate expectation);
- the platform plan (`2026-11-11-platform-plan.md`: D-PLf re-apply,
  D-PLo state tree, D-PLq catalogue, D-PLn API stability), the flash
  damage plan (D-X5, decision O2), the attenuator plan (D-Ag/D-Ah),
  the memory-upset plan (M-6a/M-7), the detector plan (D-PLm reqs).

## Review resolutions (GLM review 2026-11-12)

Every finding was re-verified against `a5dc6d5c11` before the fix.
finding → where it is resolved:

| finding | resolution |
|---|---|
| M1 reapply poisons `prior`; warm reset; flash "already active" | D-PSa rewritten: reapply never re-captures `prior`, emits its own `action: "reapply"` record (it does not call `_apply`), is device-idempotent on warm reset, and a flash row whose spec is already active is logged `already active`, not an error. New warm-reset-then-clear gate in PL-11a. |
| M2 flash rows not expressible with the `qom` action | D-PSe rewritten: a dedicated `flash_damage` action kind (write-only `damage-add` with a JSON spec built from the entry, undo = `damage-remove` with the same spec). `QOM_PATH` is NOT widened; `faults.load()` validation stays strict (params integer-only, spec shape checked). PL-11g gates `fault.clear` of a flash row, not only the inject. |
| M3 key fact 1 inverted | Fact 1 corrected: `Faults.active` persists across power cycles, the device comes back clean — that mismatch is the O2 bug and why reapply is driven from `active`. |
| M4 seeded burst has no surface | Matrix row added (random-seed/random-count, stix_edac.c:1460-1473); PL-11f gains `instrument.memory.upset.burst {seed?, count}`. |
| m1 two self-test negatives | The debug.qmp `/machine/stixflash` refusal (selftest.py:597-602) flips in PL-11d — named in its gate. The qom-path refusal (selftest.py:2316-2317) deliberately stays: with the `flash_damage` kind, `qom` still must not reach `/machine/stixflash`; the negative now pins that split. |
| m2 schema migration surface | Corrected: the consumers are the qtest pin (stix.c:9048) and the docs only; the fake still serves schema 1 (fake_qemu.py:143). PL-11c bumps 3 → 4 (3 arrives from the parallel watchdog fix, `watchdog.enabled`) and updates the qtest pin, fake_qemu and platform.md in one commit. |
| m3 source enum | PL-11e: zero \| synthetic \| replay-dss (stix_det.c:945-950, description :2341-2344); "sky" removed. |
| m4 `clear_hw` has no `flash.` prefix | PL-11d adds it (state.py:94-101). |
| m5 leaf-list omissions | PL-11b maps `truth.last_end_ns` (stix_att.c:985); PL-11c maps the FSM enable and the status line's table/count/duration_ns (stix_det.c:2259-2268); `instrument.memory.bram` maps no leaf (no EDAC counters for bram, stix_edac.c:747-750) — stated. |
| m6 "one-shot transient" imprecise | Fact 1/D-PSa say "one-shot (`transient` or `permanent`, faults-v1.json:289-308)"; the LASTING rule is what excludes them from reapply. |
| m7 cold policy double hook | One sentence in D-PSa (RESET fires on the dying process, power.py:344-351, then `spawned` on the fresh one, :359-370); harmless under the keep-`prior` rule. |
| m8 mixed verb trees | Resolved by O1: one control namespace `instrument.<unit>.<action>`. |
| m9 :1944 citation | D-PSg corrected: det-replay-file is a DEFINE_PROP_STRING silently inert after realise (grlib_stixctrl.c:1560-1565); stix_det.c:1942-1945 is det-source refusing replay-dss when no table was scanned. |
| m10 HK names range + stale platform.md | D-PSf cites the full table (grlib_stixctrl.c:533-577); the stale "reserved in the catalogue's comments" sentence (platform.md:346-347) is fixed in PL-11g's platform.md edit. |
| m11 fake must accept attenuator qom-sets | PL-11a extends SETTABLE (fake_qemu.py:27-29) — named in the commit. |
| m12 "one qom-set" overclaims | D-PSc says "one verb, minimal qom-sets" (scene: up to 4; burst: 1-2). |
| m13 config-knob remainder | Matrix row added: the realise-time knobs are `[qemu] extra_args`-only (config.py:37, qemu.py:51-60); `edac-at-boot` is additionally runtime-settable (stix_edac.c:1399-1405) with deliberately no verb in PL-11 (boot-time semantics; noted as Later). |

## Goal

Every capability the emulator has on this tree must be **controllable
and watchable from the platform controller** — verbs, fault-catalogue
rows, state-tree leaves, the UI — in one consistent way: one watch
idiom (hw-state → state leaves), one control idiom (verbs and
catalogue rows over existing QOM properties), one label idiom
(catalogue `description`, leaf `label`). Today four whole tracks landed
in the emulator with little or no platform surface: the attenuator is
invisible, the flash is unwatchable, the detector has no verbs, and a
cold power cycle leaves the catalogue's active list (and the UI) saying
the faults are still armed while the device runs clean — the flash
plan's O2 requires the opposite (2026-11-12-flash-damage-plan.md:1128-1150).

## Coverage matrix (re-verified at a5dc6d5c11)

capability | emulator knob (file:line) | platform today (file:line) | gap
---|---|---|---
process power/boot/pause | spawn `-S`, stop/cont (power.py) | platform.power.*, sim.* (platform.md:153-158) | none
SpW link, time codes, OBT, TC(9,129)/TC(20,128) | link-down/timecodes/obt (grlib_stixspw.c) | plat.py:146-207 | none
HK ADC forcing | adc-inject[0..127] (grlib_stixctrl.c:1612-1622) | force.set/clear (force.py:45-67), re-applied cold+warm (plat.py:120-133, force.py:69-76) | none
faults (v1 catalogue) | device hooks | 8 v1 rows + 9 attenuator rows + 8 memory rows active (faults-v1.json) | none for those rows
**cold-cycle persistence of QOM faults** | attenuator faults survive device reset (stix_att.c:505-512); flash damage is runtime state | plat.py:120-133 re-applies switches + forcing **only**, never `faults.active` [C] | **BUG: O2 violated** (PL-11a)
attenuator watch | `hw-state` "attenuator" section (stix_att.c:966-998, wired grlib_stixctrl.c:1532-1533); truth.*/presented.* QOM props (stix_att.c:1002-1107) | state.py:31-75 maps neither; debug.qmp refuses `/machine/stixctrl/attenuator` (core.py:25,178-180) | invisible: no leaves, no escape hatch (PL-11b, PL-11d)
attenuator control | faults-active/fault-clear/force.cmdsta-set/-clear (stix_att.c:1080-1107) | 9 catalogue rows ride the qom action (faults-v1.json:108-205; QOM_PATH allows it, faults.py:49-50) | cmdsta forcing has no verb (PL-11f)
EDAC watch | `hw-state` "edac" section, poisons + per-mem counters (stix_edac.c:714-759, schema 2 at grlib_stixctrl.c:1433,1535-1537) | state.py maps neither | invisible (PL-11b)
EDAC control | upset-inject/upset-clear/upsets-active/edac-enable/edac-at-boot (stix_edac.c:1399-1444) | 8 `memory.*` rows inject via qom (faults-v1.json:217-308); clear is dead for them ("clear" 0) | no edac-enable, no upset-clear verb → adopted upsets not reversible (PL-11f)
EDAC seeded burst | random-seed (read-write, stix_edac.c:1460-1464), random-count (write-only verb, :1465-1473) | nothing; not expressible as a catalogue row ("one action sets one property", faults-v1.json:214-216) | debug.qmp-only today; gets a verb (PL-11f)
detector scene/inject | det-source/det-flare/det-quiet-cps/det-flare-cps (stix_det.c:2338-2363), det-inject (:2036) | no verb (plat.py:146-207); only debug.qmp qom-set /machine/stixctrl | no platform control (PL-11e)
DSS replay | det-replay-file (DEFINE_PROP_STRING, grlib_stixctrl.c:1560-1565, scanned at realise stix_det.c:1316+; silently inert after realise)/-trigger/-offset-ms/-start, det-replay-status (stix_det.c:2153,2191,2228-2271,2278-2317,2396) | nothing; no config key | no control, no watch; the file is startup-only by design (PL-11e; decision O2)
detector/ROTB watch | hw-state det = quarter_on only (grlib_stixctrl.c:1522-1530); rotb_count/rotb_full (:87-90,337-341) not exported; D-PLm req 8 accessor still pending | state.py:72-74 quarter leaves only | det FSM/replay + rotb not watchable (PL-11c)
flash watch | ops-read/prog/erase/abort, damage-list (grlib_stixflash.c:1749-1769) at fixed path /machine/stixflash (leon3.c:517-518) | no hw-state, no leaves; debug.qmp refuses the path (core.py:25) | unwatchable (PL-11d)
flash damage control | damage-add/damage-remove, allow-reserved (grlib_stixflash.c:1761-1769,1717); offline tool scripts/stix/flash-damage.py (X-3) | D-X5 IDs only a comment reservation (faults-v1.json:17-31); QOM_PATH refuses /machine/stixflash (faults.py:49-50,113-116) | no injectable rows (PL-11g; a dedicated `flash_damage` action kind, D-PSe)
HK names/labels | channel names only in C comments (grlib_stixctrl.c:533-577, table + per-entry comments) | UI shows raw paths, "class, mode on target (...)" (index.html:437-438,487-492) | no description field, preconditions/expected_detection never rendered (PL-11h)
realise-time config knobs | att-initial/-travel-ms/-ramp-ms/-debounce-ms/-timeout-finishes, det-seed/-model/-idefx/-rotb, hk-adc/powerstat/hk-prot, rotb-blocks, flash latency/timings/allow-reserved/flash-damage-map, edac-at-boot (grlib_stixctrl.c:1543-1577, stix_edac.c:1399-1405) | `[qemu] extra_args` only (config.py:37, qemu.py:51-60) | accepted remainder: config-file knobs, not verbs; `edac-at-boot` is also runtime-settable (:1399-1405) and stays debug.qmp-only in PL-11 (boot-time semantics; a verb is Later)
SCET inject, hk-prot line faults, rail cut, watchdog expire, ELUT read-back, qtest-only rx-inject | none built | — | unchanged PARTIAL/MISSING (out of PL-11; extension roadmap)
PL-8 `reported`, PL-10 gdb | — | state.py:28 reported=None; no gdb gate | unchanged, other tracks

## Key facts the plan rests on

1. **The re-apply hook exists and is narrow, and the fault intent
   outlives the device [C].** `Platform._on_power` (plat.py:120-133)
   re-applies link-down, timecodes and `forcing.reapply` at `spawned`
   (before `cont`) and at `guest_reset`. `forcing.reapply`
   (force.py:69-76) covers only adc-inject — which is also what the
   `adc` fault kind rides (plat.py:344-358). Faults whose action is
   `qom` (all 9 attenuator rows, all 8 memory rows, the future flash
   rows) are applied once (faults.py:261-271) and never re-applied.
   Crucially the two sides disagree after a power cycle in opposite
   directions: `Faults.active` is controller-side and **survives** (a
   plain dict; nothing at `off`/`spawned` clears it, faults.py:158-314,
   plat.py:120-143), while the device comes back **clean** (a cold
   cycle is a QEMU respawn: power.py:184-209, :84-121 — all QOM state
   is lost). So after a cold cycle the catalogue's active list and the
   UI (GET /faults, `renderFaultState`) still say the faults are armed
   while the bench runs clean — the O2 bug
   (2026-11-12-flash-damage-plan.md:1128-1133), and exactly why
   re-apply is driven from `active`. The one-shot memory rows are 6
   `transient` + 2 `permanent` (`memory.bram.flip/.wdt`,
   faults-v1.json:289-308) and must **not** be re-applied (the
   obt.jump precedent) — the `LASTING` rule (faults.py:42-43) already
   excludes both; the attenuator rows are `until_cleared` and must.
2. **The watch path is two JSON getters [C].** `poll_state` qom-gets
   both `hw-state`s (core.py:229-256) and `leaves_from_hw` flattens
   them (state.py:31-75). The stixctrl hw-state is at schema 2 with
   `attenuator` (grlib_stixctrl.c:1532-1533 → stix_att.c:966-998) and
   `edac` (stix_edac.c:714-759) sections already in; `det` holds only
   `quarter_on` (:1522-1530) and there is no `rotb` or flash section.
   The flash device exposes read-only ops counters and the damage list
   as plain QOM properties at the fixed path `/machine/stixflash`
   (grlib_stixflash.c:1749-1769; leon3.c:517-518), so the controller
   can watch it without any emulator change.
3. **The control path is QOM class properties [C].** The detector
   scene/inject/replay knobs are class properties of stixctrl
   (stix_det_class_init, grlib_stixctrl.c:1623-1624); the attenuator's
   fault and force knobs are class properties of the child
   (stix_att.c:1080-1107); the EDAC verbs are object properties of
   `/machine/stixctrl/edac` (stix_edac.c:1399-1444). The generic `qom`
   fault action already reaches stixctrl children (faults.py:49-50) and
   stays that strict — `/machine/stixflash` does not match the pattern
   (:113-116) and must not: the flash rows get their own `flash_damage`
   action kind instead (D-PSe). `debug.qmp`'s whitelist is the two top
   paths only (core.py:25, :178-180).
4. **The verb contract is strict [M].** The verbs table in
   platform.md:150-176 is normative; the self-test checks
   `GET /verbs` against it (selftest.py:2708) and `stixplat.py
   --check-ui` checks every `data-verb` against the served vocabulary
   (uicheck.py:59-68); run mode refuses unknown verbs via verbs.py:6-13.
   Every new verb therefore lands together with its table row,
   verbs.py entry, fake-QEMU support and self-test checks.
5. **The UI is a pure API client [M].** Fault rows are generated from
   `GET /faults` (index.html:433-458) and render only
   "class, mode on target (changes, persistence)" (:437-438) —
   `preconditions` and `expected_detection` are in the catalogue and
   served but never shown. The forcing select lists raw leaf paths
   (:487-492); the state table has path/truth/presented/status/unit/
   faults columns (:498-517). `leaves_from_hw` decides what the UI can
   show, so watch work is leaf work by construction.
6. **Catalogue rules [M].** `faults.load()` + `validate()` is the
   schema (faults.py:55-134): `REQUIRED` keys are fixed; params are
   integer-typed with integer defaults (:102-105); the `qom` clear must
   be an int (:123-124); every problem is a `CatalogueError`. Extra
   keys are tolerated today only because nothing checks for them — an
   optional `label`/`description` must be *validated* (string when
   present), not just ignored, and the new `flash_damage` kind gets its
   own strict branch (D-PSe). Any catalogue change keeps
   `faults.load()` working and `python3 stixplat.py --self-test` green;
   that is a gate on every catalogue-touching commit.
7. **The fake must mirror the real [M].** fake_qemu.py (scripts/stix/
   platform/tests/fake_qemu.py) serves a hw-state stub (`det` only,
   :158, still schema 1, :143) and records qom-sets only for
   properties in its SETTABLE tuple (:27-29 — the attenuator's fault
   properties are rejected today); every leaf or verb commit extends
   the fake and the self-test expectations in the same commit.

## Decisions (D-PS)

- **D-PSa — re-apply rule.** At `spawned` (before `cont`) and at
  `guest_reset`, `Faults.reapply(why)` makes the device match every
  active fault whose persistence is in `LASTING`, for the device-state
  kinds (`qom`, `flash_damage`). Reapply is **not** `_apply` again: it
  re-runs the action's apply function but never touches the stored
  `prior` — `prior` is the undo anchor captured at the first apply and
  stays the clean pre-fault value even though the property read back
  after a reset holds the fault value (plat.py:366-371); otherwise
  `qom_undo` would "clear" the fault by writing the fault back. It
  emits its own record with `action: "reapply"` (never
  `action: "inject"`, faults.py:267-271); a QMP failure is logged as
  `action: "reapply-error"` and never fails the power-on (S3, like the
  poller). Warm reset is device-idempotent: the attenuator faults
  survive reset (stix_att.c:504-512) so re-setting the property is a
  no-op, and flash damage survives reset too, where a blind damage-add
  would error "the spec is already active" (grlib_stixflash.c:1560-1565)
  — the `flash_damage` reapply tolerates exactly that device error and
  logs the row as `already active` (the device is the canonical
  comparator, stixflash_dmg_equal :1499-1505). One-shots (`transient`
  or `permanent` — the 6+2 memory rows, faults-v1.json:289-308) are not
  re-applied (the LASTING rule excludes them); `adc`-kind faults stay
  covered by `forcing.reapply`; controller-side kinds (link, timecodes,
  keepalive) keep today's path. Under `on_guest_reset = cold` the hooks
  fire twice (RESET on the dying process, power.py:344-351, then
  `spawned` on the fresh one, :359-370) — harmless here, and another
  reason reapply only re-sets and never re-captures.
- **D-PSb — one watch idiom.** Everything watchable goes through
  hw-state → state leaves; no separate per-device getters. The
  attenuator and EDAC sections are mapped as-is (no emulator change);
  det/rotb get one read-only accessor each in the emulator (the
  D-PLm req 8 shape); the flash is watched by the controller reading
  the existing flash QOM properties (no flash hw-state yet — a flash
  hw-state section is the emulator-side follow-up, not PL-11).
  state.py maps sections with `.get`, so older-schema fakes keep
  working; the hw-state schema is at 2, goes to 3 with the parallel
  watchdog fix (`watchdog.enabled`) and to 4 when det/rotb land in
  PL-11c (the M-6a precedent, grlib_stixctrl.c:1433). Replay position
  and other move-every-tick leaves join `VOLATILE` (state.py:16-20).
- **D-PSc — one control idiom, one verb namespace (O1).** Control
  verbs wrap existing QOM properties one-to-one and stay thin: validate,
  minimal qom-sets, log, return — one verb may issue several sets
  (`instrument.detector.scene` sets up to 4 properties, the burst verb
  1-2), so the rule is "one verb, minimal sets", not "one qom-set". No
  new device knobs except the read-only det/rotb accessor of D-PSb.
  `plat.py`'s `_qom_set` is generalised to take a device path. Every
  control verb lives in one namespace `instrument.<unit>.<action>`
  (detector, attenuator, memory, flash); state leaves keep their
  existing prefixes. Verbs are additive under API v1 (D-PLn): each
  lands with its normative table row (platform.md:150-176), verbs.py
  entry and `--check-ui` coverage.
- **D-PSd — debug.qmp whitelist.** `DEBUG_PATHS` becomes the five
  fixed STIX device paths: `/machine/stixspw`, `/machine/stixctrl`,
  `/machine/stixctrl/attenuator`, `/machine/stixctrl/edac`,
  `/machine/stixflash` (children of a listed path allowed, anything
  else still 403). It stays a dev/gate escape hatch: same
  `[debug] raw_qmp` gate, same sticky taint (platform.md:175).
- **D-PSe — flash faults: a dedicated `flash_damage` action kind.**
  When adopted (PL-11g), the three D-X5 device rows become real
  catalogue entries with `"action": {"kind": "flash_damage", "path":
  "/machine/stixflash", "spec": {...}}`; the fault IDs are the platform
  API, so no separate `flash.damage.*` verbs. The spec is the sidecar
  entry grammar as a JSON object (kind erase-fail | program-fail |
  stuck | stuck0; block or page or mcm/chip addressing; bit+value
  stuck-only — grlib_stixflash.c:1083-1144) whose leaf values are
  either fixed integers or `{"param": "<name>"}` references into the
  entry's params. Apply builds the spec string with the resolved
  params and qom-sets write-only `damage-add` (no getter exists,
  :1767-1768, so there is no prior read — `"clear"` is not used);
  undo rebuilds the same spec string from the stored params and
  qom-sets `damage-remove`, the device's exact-spec removal
  (:1585-1619). Validation stays strict (fact 6): a new branch checks
  path == /machine/stixflash, the spec's shape (allowed keys, kind
  enum, stuck needs bit+value), and that every referenced param exists
  and is integer-typed (:102-105 unchanged) — the API never sees raw
  JSON. `QOM_PATH` is NOT widened and the qom kind still refuses
  `/machine/stixflash` (selftest.py:2316-2317 stays green as the pin).
  The offline rows (bad_factory, file.corrupt, wrong_address) stay
  comment-reserved: they are pre-boot tool work (the planned `hooks.*`
  verb), not injectable faults (D-X5).
- **D-PSf — labels.** The catalogue gains optional `label` (short,
  for the row title) and `description` (one sentence); validated as
  strings when present, `REQUIRED` unchanged. The controller gains one
  HK channel-name table (transcribed from the C comments over the
  full table, grlib_stixctrl.c:533-577 — [A] the C comments are the
  source of truth, decision O6) held in one Python table with a
  self-test that its channel count matches the served leaves, served as
  `label` on `instrument.hk_adc.N` leaves and shown in the forcing
  select; the UI renders description, preconditions and
  expected_detection.
- **D-PSg — replay file stays startup-only in PL-11.** The
  `.dssevs` table is pre-scanned at realize (stix_det.c:1316-1423);
  det-replay-file is a DEFINE_PROP_STRING (grlib_stixctrl.c:1560-1565)
  and is silently inert after realize — it is not a refusal but a
  no-op; what errors at runtime is det-source refusing `replay-dss`
  when no table was scanned (stix_det.c:1942-1945). PL-11 surfaces it
  as `[qemu]` config keys + `det-replay-start` control +
  `det-replay-status` watch; runtime (re)load is an emulator item for
  later (decision O2: out of scope).

## User decisions (O1-O6, 2026-11-12)

| Q | decision |
|---|---|
| O1 verb namespace | One control namespace `instrument.<unit>.<action>` for the detector, attenuator, memory and flash verbs: `stim.det.*` becomes `instrument.detector.*`; state leaves keep their existing prefixes; `state.clear_hw` gains the `flash.` prefix. |
| O2 replay file | Startup config only (`[qemu] det_replay_file`); runtime load stays out of scope (D-PSg). |
| O3 attenuator control | cmdsta forcing is enough in this track; labelled TC(236,4/5) helper steps are a later track. |
| O4 EDAC enable | One verb `instrument.memory.edac.enable {module, on}` mirroring the QOM `edac-enable` property semantics exactly: module `all` → "all-on", one module → its bit (1 = sdram, 2 = cache, 4 = rotb); `on: false` refused (the property only sets, never clears, stix_edac.c:1412-1415); the device's own refusals (empty mask, unwired owner, :1315-1329+) surface as the verb's error. |
| O5 re-apply opt-out | Cold-cycle re-apply is default-on for every LASTING device-state fault, no per-entry opt-out in PL-11; the flash plan's persistent/transient field is noted as Later. |
| O6 HK labels | From the C comments (grlib_stixctrl.c:533-577), held in one Python table with a self-test that the channel count matches; the C comment stays the source (D-PSf). |

## Owner decisions on the revision (2026-11-13)
- Flash reapply tolerance: match the device's distinct error text "the
  spec is already active" (grlib_stixflash.c:1562); no textual
  damage-list pre-check. Any other damage-add error at reapply is
  logged as a failed reapply (S3), never wedges power-on.
- platform.md schema prose: the PL-11c implementer greps platform.md
  for every hw-state schema mention and updates all of them.

## Commits

| Commit | One line | New gates |
|---|---|---|
| PL-11a | cold+warm re-apply of device-state faults, prior kept (the O2 bug) | self-test fake-QEMU re-apply check (cold + warm-then-clear); run-smoke `gate-fault-reapply` variant |
| PL-11b | watch: attenuator + EDAC state leaves from the existing hw-state | self-test leaf shapes; core-smoke `--check-ui` |
| PL-11c | watch: det + rotb hw-state sections (read-only accessor, schema 3 → 4) | qtest hw-state (purity + fields, schema pin 4); boot/nominal/fault smokes unchanged |
| PL-11d | watch: flash ops/damage leaves; debug.qmp whitelist (D-PSd) | self-test fake flash + debug.qmp matrix (the /machine/stixflash refusal flips); core/link-smoke |
| PL-11e | control: `instrument.detector.*` (scene, inject, replay) + `[qemu]` replay keys | self-test verb/table/fake; run-smoke detector scenario |
| PL-11f | control: attenuator cmdsta + EDAC enable/upset-clear/upset-burst verbs | self-test; run-smoke variants on the new leaves |
| PL-11g | control: D-X5 flash fault rows via the `flash_damage` action kind | catalogue gates; fault-reapply scenario extended with `flash.cell.stuck` (inject + clear) |
| PL-11h | UI/catalogue labels: description, preconditions, expected_detection, HK names | catalogue gates; self-test UI checks; `--check-ui` |

Every commit additionally runs the full existing set (`boot-smoke`,
`nominal-smoke`, `fault-smoke`, `att-smoke`, `edac-smoke`,
`detector-smoke`, `flash-damage-smoke`, the platform smokes,
`stixplat.py --self-test`); C-touching commits never regenerate a
golden.

### PL-11a: the cold-cycle bug first

- `Faults.reapply(why)` in faults.py per D-PSa: a separate path from
  `_apply` that re-runs the action's apply function, **keeps the stored
  `prior` from the first apply** and logs `action: "reapply"`;
  called from `plat._on_power` in both the `spawned` and the
  `guest_reset` branches (plat.py:120-133). The warm branch is not
  just "cheap and symmetric": the emulator keeps attenuator faults at
  reset (stix_att.c:504-512) so the re-set is a device no-op, and the
  keep-`prior` rule is what makes the subsequent `fault.clear` still
  write the clean pre-fault value instead of the faulted one.
- The fake gains the attenuator fault properties in SETTABLE
  (fake_qemu.py:27-29 — they are rejected today), and its guest reset
  keeps properties (as the real device keeps the attenuator's), so the
  warm gate below is meaningful.
- Gate (self-test, fake QEMU): inject `attenuator.current.offset`
  (qom, until_cleared, works on AX), power off, power on; assert a
  `fault.attenuator.current.offset` record with `action: "reapply"`
  and the recorded qom-set of the offset property after spawn and
  before `cont`; assert the second inject still 409s; assert a
  `transient` memory row is *not* re-applied. Warm gate: inject →
  `x-fake-guest-reset` (the RESET event) → `fault.clear` → the
  recorded clear qom-set writes the clear value, **not** the faulted
  current value (prior kept; the M1 regression). Gate (run-smoke): a
  scenario variant injects the same fault, checks
  `instrument.hk_adc.8` presented, `power.cycle`s, and re-checks the
  leaf — the O2 acceptance, live.
- Risks: an apply that fails at re-apply must not wedge power-on (S3
  logging, D-PSa); ordering before `cont` (the link-down precedent,
  platform-plan.md:1641-1643) [L] — the run-smoke variant proves it.

### PL-11b: watch the attenuator and the EDAC (no emulator change)

- `leaves_from_hw` maps `attenuator` → `instrument.attenuator.*`
  (truth.position/position_ppm/sensor_ab/sensor_bc/motor1_driving/
  motor2_driving/att_c/moves/last_end/last_end_ns, presented.sensor_ab/
  sensor_bc/cmdsta/att_c, fsw_accesses, faults_active) and `edac` →
  `instrument.memory.<cache|sdram|rotbuf>.{injected,corrected,detected}`
  + `instrument.memory.poisons` (the armed-upset records). Both with
  `.get` so an old-schema fake still loads; fake_qemu.py's hw-state gains
  both sections (mirroring stix_att.c:966-998 / stix_edac.c:714-759).
  Stated omission: `instrument.memory.bram` maps no leaf — the EDAC
  hw-state has no bram counters (stix_edac.c:747-750), so the
  `memory.bram.*` faults can never tag one.
- The state table and the diff highlight pick the leaves up for free
  (index.html:498-517); `fault_ids` already tags targets
  (state.py:129-133), so an active attenuator fault shows on its leaf.
- Gate: self-test (leaf paths/values from the fake, change records on
  poison arm/clear, no change record for stable reads); core-smoke
  with `--check-ui`; the catalogue untouched.

### PL-11c: watch the detector and the ROTB (small emulator change)

- One read-only accessor per D-PLm req 8: `stix_det` fills the hw-state
  `det` QDict with source, flare, quiet_cps, flare_cps, QL bank in
  use, the FSM enable, and the replay fields the status string already
  carries (state, pos_ns, events, dropped, unsupported, plus
  table/count/duration_ns — parsed once in C, stix_det.c:2259-2268);
  grlib_stixctrl adds a `rotb` section (count, full, enable — the
  state behind rotbCmdSta, grlib_stixctrl.c:337-341). Pure getters:
  two reads with no access in between are equal (the D-Ai rule).
- Schema: the parallel watchdog fix bumps the ctrl hw-state to 3
  (`watchdog.enabled`); this commit bumps it **3 → 4** and updates
  every consumer in the same commit — the qtest pin (stix.c:9048-9051,
  which pins 3 after the watchdog merge) and fake_qemu.py (its
  `hw_ctrl` serves 3 after that merge, 4 here; it sat at 1 through
  M-6a, fake_qemu.py:143). The controller itself never reads the
  version — its tolerance comes from `.get` mapping, not the number.
- `leaves_from_hw` maps `instrument.detector.*` (incl. `fsm.enable`
  and the replay fields) and `instrument.rotb.*`;
  `detector.replay.pos_ns` joins VOLATILE; quarter_on keeps its path
  (no rename — API v1 stability).
- Gate: qtest extension (fields, purity, schema pin 4); boot/nominal/
  fault/att/edac/detector smokes unchanged; self-test leaves;
  platform.md's hw-state schema mention updated.

### PL-11d: watch the flash + open the debug.qmp hatch

- `poll_state` additionally qom-gets the flash's `ops-read/prog/
  erase/abort` and `damage-list` (grlib_stixflash.c:1749-1769) into
  `flash.ops.*` and `flash.damage` leaves (damage-list parsed; the
  counters are normal leaves — they move only on flash ops). One more
  small JSON at ≤ 2 Hz. `state.clear_hw` gains the `flash.` prefix
  (state.py:94-101), so the new leaves are dropped at power off like
  the instrument ones (m4).
- `DEBUG_PATHS` per D-PSd; the platform.md `debug.qmp` row says the
  five paths. The self-test's debug.qmp refusal of
  `/machine/stixflash` flips in this commit (selftest.py:597-602
  drops it from the 403 list); the taint/gate assertions stay.
- Gate: self-test (fake flash device, whitelist accept/refuse matrix
  with the flip named, taint unchanged, clear_hw drops flash.*);
  core-smoke + link-smoke `--check-ui`.

### PL-11e: control the detector stimulus

- Verbs (D-PSc + O1, each with table row + verbs.py + fake support):
  `instrument.detector.scene {source?, flare?, quiet_cps?, flare_cps?}`
  (sets only the given args — up to 4 qom-sets; source enum
  zero|synthetic|replay-dss, stix_det.c:945-950, description
  :2341-2344), `instrument.detector.inject {fpga_det, pix, ebin,
  count}` (det-inject grammar, stix_det.c:2036-2150),
  `instrument.detector.replay {start}` (bool → det-replay-start,
  :2278-2317). Watch comes from PL-11c's leaves (`await_state` on
  `instrument.detector.replay.state` in scenarios).
- Config keys `[qemu] det_replay_file / det_replay_trigger /
  det_replay_offset_ms` passed at spawn (D-PSg, decision O2); the
  wrong-key guard of config.py covers typos.
- Gate: self-test (arg validation incl. the source enum, table/fake/
  verbs.py equality); a run-smoke scenario: scene=synthetic →
  `await_state` QL counts move; inject → SSID32/leaf; replay (small
  committed .dssevs fixture) → start → state running → ended;
  `--check-ui`.

### PL-11f: control the attenuator and the EDAC

- `instrument.attenuator.cmdsta {value}` / `instrument.attenuator.
  cmdsta.clear` → force.cmdsta-set / force.cmdsta-clear
  (stix_att.c:1102-1107): the bench's only attenuator *command* path
  that does not need a PFM ELF (the TC(236,4/5) route is FSW-owned,
  decision O3; the 9 catalogue fault rows already exist).
- `instrument.memory.edac.enable {module, on}` per O4 → edac-enable
  (stix_edac.c:1410): `module: all|sdram|cache|rotbuf`, mapped to
  "all-on" or the 1/2/4 bit; `on: false` refused (the property only
  sets, never clears, :1412-1415); the device's refusals (empty mask,
  unwired owner, :1315-1329+) surface as the verb's error.
- `instrument.memory.upset.clear {id}` ("all" or an index into the
  poisons leaves) → upset-clear (:1444): completes the memory rows,
  whose catalogue `clear` is deliberately dead (faults-v1.json:206-216).
- `instrument.memory.upset.burst {seed?, count}` (decision on M4):
  `seed` → qom-set random-seed (0 = off, survives reset,
  stix_edac.c:1460-1464), `count` → qom-set random-count (write-only
  verb, :1465-1473); one verb, 1-2 qom-sets (m12). The burst stays
  catalogue-less ("one action sets one property", faults-v1.json:
  214-216) — the verb is its platform surface, the debug.qmp escape
  hatch remains.
- Gate: self-test (paths, 409s when off, the `on: false` refusal);
  run-smoke variants: cmdsta force →
  `instrument.attenuator.presented.cmdsta` leaf; edac-enable →
  `memory.sdram.seu` detection precondition satisfied live [L]; burst
  with a fixed seed → the poisons leaves gain rows [L].

### PL-11g: adopt the flash damage fault rows (D-X5)

- faults-v1.json: `flash.block.erase_fail`, `flash.page.program_fail`,
  `flash.cell.stuck` become real rows (class memory, changes truth,
  until_cleared, reversible; params block|page (+bit, value) in the
  FSW-linear index space) with the new `action kind flash_damage`
  (D-PSe): apply = `damage-add` with the JSON spec string built from
  the entry's spec + resolved params, clear = `damage-remove` with the
  same spec (the device's exact-spec removal, grlib_stixflash.c:
  1585-1619). `ACTIONS` gains the kind; `QOM_PATH` is untouched and
  the qom path still refuses `/machine/stixflash` (selftest.py:
  2316-2317 stays as the pin). The reservation comment shrinks to the
  three offline rows.
- Validation: the strict `flash_damage` branch of D-PSe (spec shape,
  integer params, no raw JSON from the API); `faults.load()` keeps
  working on the shipped and every test catalogue.
- Because the rows are LASTING + device-state, PL-11a re-applies them
  across a cold cycle for free (damage-add re-issued — the fresh
  process's list is empty) and skips a warm reset gracefully (the
  device's "already active" error logged as `already active`, not a
  reapply-error) — O2 closed end-to-end.
- Gate: **the catalogue rules** (`python3 stixplat.py --self-test`
  green, including validate negatives: a `/machine/stixflash` row
  with `kind: qom` still refused, a malformed spec refused, a
  reserved-block inject without `allow-reserved` fails at the device
  with nothing armed); **fault.clear of a flash row** runs
  damage-remove and the damage leaf loses the row (not only the
  inject); the PL-11a scenario extended with `flash.cell.stuck`
  (inject, cold cycle, reapply, clear); `flash-damage-smoke`
  unchanged; platform.md's stale "attenuator's fault IDs are reserved
  in the catalogue's comments" sentence fixed (platform.md:346-347,
  m10).

### PL-11h: labels, descriptions and the UI

- Catalogue: optional `label`/`description`, validated (D-PSf); the
  shipped rows get one-line descriptions (the attenuator rows'
  preconditions already carry the PFM-ELF caveat — it moves into
  `description` + stays as `preconditions`).
- Controller: the HK channel-name table served as `label` on
  `instrument.hk_adc.N` leaves (source: the C comments over the full
  table, grlib_stixctrl.c:533-577; [A]/O6 accepted as truth) — one
  Python table with a self-test that its channel count matches the
  served leaves, so a transcription slip cannot pass silently.
- UI (index.html): the fault row shows `description` when present
  (else today's class/mode line), then preconditions and
  expected_detection as list items (:433-458); the forcing select
  shows "ch 8 — ATT_C" style labels (:487-492); the state table gains
  the label column (:498-517). No new data-verbs.
- Gate: **the catalogue rules as PL-11g**; self-test UI checks
  (CSP unchanged, every data-verb still served); `--check-ui` on both
  smokes; platform.md (catalogue + web UI sections) updated.

## Risks and checks

- **Fake drift**: every leaf/verb commit must extend fake_qemu.py and
  the self-test expectations in the same commit, or the gates lie
  (fact 7). PL-11a's fake work is two-sided: the attenuator properties
  join SETTABLE (:27-29) and the hw-state stub later grows the
  attenuator/edac/det/rotb/flash data. The self-test's fake-vs-table
  equality catches most of it.
- **Schema bumps**: the ctrl hw-state goes 2 → 3 (parallel watchdog
  fix, `watchdog.enabled`) and 3 → 4 in PL-11c (det/rotb). The
  consumers to update in the PL-11c commit are the qtest pin
  (stix.c:9048-9051) and fake_qemu (serves 3 after the watchdog
  merge); the controller's tolerance comes from `.get` mapping, not
  the version, so a mixed build never KeyErrors; the smokes stay
  green across both bumps.
- **Re-apply ordering and undo**: properties set after `cont` would
  miss the first instructions [L]; the run-smoke variant checks a
  presented value right after boot. Reapply never re-captures `prior`
  (D-PSa) — the keep-`prior` rule is what keeps `fault.clear` writing
  the clean value after any cycle; the PL-11a warm gate pins it.
- **debug.qmp widening** is dev/gate-only: same raw_qmp gate, same
  run-mode gate, same sticky taint; the whitelist stays a fixed tuple,
  not a pattern, so a stray `/machine/` path keeps 403ing.
- **Labels go stale**: the HK names are a transcription of C comments;
  the table cites its source and the qtest hw-state could carry the
  name per channel later (not PL-11).
- **Attenuator outcomes on TM**: the fault rows' expected_detection
  stays empty until a live PFM run (their plan's A-later caveat);
  PL-11 gates them on state leaves, not TM.
- **Scope guard**: SCET inject, hk-prot line faults, rail cut, watchdog
  expire, ELUT read-back and runtime replay-file load stay out (the
  coverage matrix marks them); taking them in would double the track.
- **Open questions**: none — all six were decided on 2026-11-12, see
  the User decisions table.

## What the old audit got wrong (written at 083d3d4365)

- **Row 19 (DSS replay)**: "no det-replay-status property … offset s
  not ms" — now built: `det-replay-status` (stix_det.c:2228-2271,
  2396) and `det-replay-offset-ms` (:2191), trigger modes (:2153);
  only "file init-time only" and "no loop" still hold.
- **Row 23 (flash faults) MISSING** — built: X-1..X-5 landed the
  damage list, damage-add/remove, the sidecar and the offline tool
  (grlib_stixflash.c:110-126, 1761-1769).
- **Row 24 (SEU/DEU) MISSING** — built: M-1..M-7 landed the EDAC unit
  with the upset machinery, hw-state section and catalogue rows
  (stix_edac.c; hw-state schema 2).
- **Row 25 (attenuator) "A-3 not built"** — built: the A-3 fault
  properties, faults-active/fault-clear and force.cmdsta
  (stix_att.c:1080-1107), plus 9 active catalogue rows.
- **Row 25/26 "truth unreachable"** — still true via debug.qmp, but
  the catalogue's qom action already reaches the attenuator child, so
  the faults are injectable today; only the *watch* side is missing.
- Stale line numbers throughout (plat.py, faults-v1.json) and the
  "att tree"/"det7" premise — all four tracks are merged at
  `a5dc6d5c11`; rows 4, 9, 12-14, 16, 20-22, 28, 29 re-verified
  unchanged.
