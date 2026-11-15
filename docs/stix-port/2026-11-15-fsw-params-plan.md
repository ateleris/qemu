# FSW parameters table (IDPU & FSW tab) — implementation plan (2026-11-15)

> For agentic workers: implement task by task; steps use `- [ ]`.
> Workers cannot build or run anything: write code + self-test cases,
> the reviewer runs them and commits.

**Goal:** the IDPU & FSW tab gets the mockup's FSW parameters table —
defaults and names read from the running FSW ELF, live values from
guest RAM, per-row set/read-back over TC(237,18)/(237,19), and an
"Applies" column with FSW-evidenced semantics.

**Architecture:** one new module `stixplat/params.py` (stdlib-only
ELF32/DWARF reader + the table cache + the read-back listener), owned
by `Platform` like keepalive/timesync; five new verbs behind the
existing operator-TC gate; one new record kind `param` that the series
index turns into `param:<index>` series; the IDPU tab panel in
`ui/index.html`. No emulator (C) change, no config change, no qtest
impact.

**Spec / inputs:**
- Owner decision (2026-11-15): "parameter DEFAULTS come from the
  RUNNING ELF, not from the FSW source, not from a boot-time
  read-back, not from a curated file."
- Mockup `Idpu.dc.html` (approved visually): status line "N parameters
  differ from default" (:61); "Only non-default" filter (:68); "Read
  all back" and "Reset all…" buttons (:70-71); note "Set sends
  TC(237,18) and reads back with TC(237,19). Highlighted rows differ
  from the ELF default. 'Applies' says when the FSW picks the value
  up." (:73); columns Index, Name, Default, Current, New value,
  Applies, Set/Reset (:77), rows grouped under a group-name row
  (:79-95, example rows :117-128).
- Docs: `docs/stix-port/platform.md` (API v1, verbs table is
  normative), `2026-11-14-timeline-plan.md` (record/listener/series
  conventions), `accepted-diffs.md` (P-N sections for controller
  features).

## Key facts

ELF facts below are owner-verified (2026-11-15); this session could
not run `nm`/`readelf` (sandbox), so task T7 re-verifies them live.

- FSW keeps one static table: `ParamMgmt_parameters[E_PARAM_ID_MAX]`
  (ParamMgmt.c:22-27) of 16-byte rows
  `{const uint32_t defaultValue; uint32_t value; minValue; maxValue}`
  (ParamMgmt.h:37-43). `value` starts == default (the C initialiser,
  ParamMgmt.c:25), so the ELF *file image* of `.data` already holds
  defaults/min/max — no guest needed for the Default column.
- ELFs in `~/dev/stix-elf`: `ASW_em_baseline.elf` symbol
  `ParamMgmt_parameters.145875` at 0x40080908, size 7712 (= 482 rows);
  `ASW_em_prof.elf` `.142237` at 0x40059a68, size 7728 (= 483);
  `ASW_pfm_v186.elf` `.142194` at 0x400597a8, size 7712. The symbol is
  LOCAL with a per-build numeric suffix (hence prefix matching). All
  three carry `.debug_info`, so the `E_PARAM_ID_*` names are
  recoverable from the DWARF enum.
- The IDB cannot supply names: the GS command JSON has TC(237,18) /
  TC(237,19) as bare `Parameter ID - ASW (PIX00016)` u16 + `subID`
  u8 + `data` u32 with no per-id enumeration
  (stix_idb_cmd_5ac.json:8574-8615). FSW `ParamMgmtDef.h` has 530
  `PARAM_INT32(` lines with `#ifdef HW_VERSION_*` variants
  (:18-114), i.e. source order/count differ per build — not
  authoritative.
- TC(237,18) app data `[u16 id][u8 sub][u32 value]` BE
  (TCData.h:103-106); sub 0/1/2 = value/min/max
  (TcService237.c:66-94). TC(237,19) `[u16 id][u8 sub]`
  (TcService237.c:97-143) answers TM(237,20) `[u16 id][u8 sub][u32
  value]` BE (TM_237.c:29-48) on APID `(90 << 4) | 8` = 0x5A8
  (TMData.h:63, :79, :442). The controller already labels TC(237,18)
  TCs `data.param {index, sub, value}` (relay.py:243-246, 250-260;
  selftest `t_tc_237_18_label`).
- Where the FSW picks values up ("Applies"):
  - on set: ATT_NOMINAL/MAX_PWM_LEVEL (ParamMgmt.c:101-107),
    ENABLED_SSID_TRANSFFER_TO_SSMM_MASK (ParamMgmt.c:111-112),
    CONFIG_CURRENT_{T1,T2,ELUT,SKY}_LUT_ID (ParamMgmt.c:109,
    LUTMgmt.c:477-510); every set also updates HK monitoring
    (ParamMgmt.c:99).
  - next NOMINAL entry: the big config block inside `setMode`
    (OpModeMgmt.c:501-568 detector/ASIC/aspect/attenuator/ASW config,
    :625-637 NOMINAL_* masks) and RCR_* via `QLAccuDblBuffs_init` →
    `RateCtrl_init` (OpModeMgmt.c:645, QLAccuDblBuffs.c:103,
    RateCtrl.c:667-675); nuance: TC(237,10) applies RCR immediately
    (RateCtrl.c:1304), a bare TC(237,18) does not.
  - next calibration start: CALIB_* (TC(237,3) sets+restarts,
    TcService237.c:146-167; TPG reads CALIB_QUIET_TIME when applied,
    XRayADCsCtrl.c:2596).
  - at boot/INIT: ASPECT_SUMMED_VALUES, CONFIG_SCRUB_SDRAM
    (OpModeMgmt.c:1067, :1071).
  - everything else: unknown.
- At boot the FSW may overlay flash-context values onto the table
  (ContextMgmt.c:2341-2344), so `value != default` without any
  operator set is legitimate — the table shows values, the UI does
  not claim who changed them.
- Platform conventions to reuse: verbs are registered in
  `Platform.register` (plat.py:160-237) with the operator-TC gate
  `_gs_enabled` (plat.py:400-405); the relay exposes a TM listener
  list `fn(unit, dec)` (relay.py:59, 109-114); `tcbuild.tc_packet`
  builds TCs (tcbuild.py:29-40); QEMU runs with cwd = run dir
  (qemu.py:196-199), so QMP `memsave` can write a relative filename
  into the run dir; `core._elf_sha256` (core.py:380-391) is computed
  at start; record kinds/topics in log.py:26-36; series feed rules in
  series.py:79-130; `GET /api/v1/verbs` must equal the platform.md
  table (`t_verbs_doc`); every `data-verb` must be served (uicheck.py:
  39-68). The container final stage has no binutils — only
  python3-minimal (Dockerfile:44-46) — so `readelf` is unavailable
  where the controller runs: the ELF parsing is stdlib-only Python.

## Global constraints

- Stdlib only (Python ≥ 3.11); no new config keys; API v1 additive
  only (new verbs, kind `param`, topic `param`, GET `/params`).
- The ELF is the single authority: names, defaults, min/max, count.
  Never read the FSW source or a curated file at runtime; the FSW
  greps in this plan are only evidence for the static "Applies"
  labels.
- `index.html`: one file, no build, no `innerHTML`, no inline
  handlers/styles, textContent only; every `data-verb` served.
- Self-test `cd scripts/stix/platform && python3 stixplat.py
  --self-test` stays PASS; every task adds `check(...)` cases in
  `stixplat/selftest.py`.
- A broken/missing ELF must degrade, never crash the controller: one
  `params.elf` log record with the reason, `/params` answers with
  `params: []` and `warnings`.
- No FSW edits; nothing copied from `~/dev/stix-doc-notes/sources`
  (not readable in this session; nothing depends on it).
- RAM reads are a snapshot of a running guest: one `memsave` per
  refresh, torn u32s accepted; never poll faster than the UI's 2 s.

## Approaches considered

1. **Defaults from the ELF file image (chosen):** parse `.symtab` +
   PT_LOAD in the ELF file, read the initial table bytes. Exact, per
   build, works with the guest off, needs no curated mirror.
   Trade-off: a small ELF parser to own (~150 lines) instead of
   calling `readelf` — but `readelf` does not exist in the controller
   image (Dockerfile:44-46).
2. Defaults from `ParamMgmtDef.h` in the FSW checkout: rejected —
   not present in the container, may not match the running ELF
   (owner explicitly excluded it).
3. Current values by TC(237,19) read-back of all rows (chosen only as
   an on-demand action): correct but 482 round trips and
   link-dependent; guest RAM via QMP `memsave` (chosen for Current)
   is one command and works paused. Both are shown so a mismatch is
   visible.
4. Names from DWARF (chosen) vs from the IDB (impossible, verified)
   vs "param N" only (kept as the degraded fallback with a warning).

## Review focus

1. A missing, stripped or ambiguous symbol must degrade to warnings +
   an empty table, never an exception in the API (T1:
   `params_elf_symbol_rules`).
2. Unsupported DWARF forms must fall back to `param <idx>` names with
   a named warning, never silently wrong names (T1:
   `params_dwarf_missing`; T7 spot-checks real names).
3. Read-back must not hang when the TC never leaves the bench: link
   down / power off → immediate 409 with the relay's drop reason;
   timeout → 500 after 2 s (T4: `params_readback_dropped`).
4. Verbs table sync: `t_verbs_doc` fails unless platform.md gains the
   five verbs in the same task as `verbs.py` (T6), and `--check-ui`
   fails unless the UI verbs land with the registrations (T6 gate).
5. `memsave` semantics: relative filename resolves in the run dir
   (QEMU cwd, qemu.py:196-199), cpu index 0, 409 when power is off —
   pinned by T3's fake-QMP case.

## Tasks

### Task T1: ELF/DWARF reader + the static table

**Files:** create `stixplat/params.py`; modify `stixplat/selftest.py`.

- [ ] `elf32.read(path)` → `{symbols, loads, sections, warnings}`:
  32-bit big-endian ELF, section headers, `.symtab`/`.strtab`,
  program headers. Refuse non-BE/non-32-bit with `ElfError`.
- [ ] `find_table(elf)` → `{vaddr, size, count, symbol, warnings}`:
  symbols matching `^ParamMgmt_parameters(\.\d+)?$`, LOCAL or GLOBAL,
  `st_size % 16 == 0`, `16 <= count <= 8192`, vaddr in
  0x40000000..0x48000000; several candidates → keep the
  size-valid ones, list all in `warnings`, pick the largest.
- [ ] `rows(elf, table)` → per row `(default, value, min, max)` read
  from the PT_LOAD covering vaddr (error when it does not, = BSS):
  the initial image, i.e. `value == default` here.
- [ ] `dwarf.enum_names(elf)` → `{index: "PARAM_NAME"}`: minimal
  .debug_abbrev/.debug_info walk, take the `DW_TAG_enumeration_type`
  whose enumerators are all `E_PARAM_ID_*`; forms supported: addr,
  block*, data1/2/4/8/16, sdata, udata, strp, string, flag(_present),
  ref*, sec_offset, exprloc, implicit_const; anything else →
  `DwarfError` naming the form. Cross-check: enumerator `E_PARAM_ID_MAX`
  value vs symbol count; count of rows with `min > max` or default
  outside `[min, max]` → warnings, never failures.
- [ ] `load(path, sha256) -> ParamsTable` with `count`, `rows` (list
  of dicts `index, name` (E_PARAM_ID_ prefix stripped, else
  `"param <idx>"`), `default, value=None, value_at=None, readback=None,
  readback_at=None, applies`), `warnings`, `at_sim=None`; caches by
  sha256.
- [ ] The seed `applies` map (name prefix → label), ordered, from the
  Key facts: ATT_*_PWM_LEVEL / ENABLED_SSID_* / CONFIG_CURRENT_* →
  `"on set"`; CONFIG_SCRUB_SDRAM → `"at boot"`; CALIB_* →
  `"next calibration start"`; NOMINAL_/CONFIG_/ASIC_/ASPECT_/RCR_/
  IMAGING_/SPECTRO_/FLARE_SEL_ → `"next NOMINAL entry"`; else
  `"unknown"`. Evidence cites in the module docstring.
- [ ] Self-tests: `params_elf_parse` (a `_param_elf()` fixture builder
  writing a synthetic 32-bit BE ELF with a PT_LOAD at 0x40080908, one
  LOCAL `ParamMgmt_parameters.145875`, st_size 4×16, and a DWARF enum
  with 5 enumerators incl. `E_PARAM_ID_MAX`=4; assert count 4, names,
  default/min/max of row 1, one min>max row → warning, table loads);
  `params_elf_symbol_rules` (no symtab; size % 16 != 0; two
  candidates → warning + largest kept; vaddr outside RAM → no table);
  `params_dwarf_missing` (same ELF without .debug_* → names
  `"param 0..3"`, `warnings` names the reason, defaults still served).

### Task T2: controller wiring, GET /params, records, series

**Files:** `stixplat/params.py`, `stixplat/plat.py` (own a `Params`,
`register`, `status()` gains `"params"`, additive), `stixplat/core.py`
(expose `ctl.params`), `stixplat/api.py` (GET route), `stixplat/log.py`
(KINDS += `"param"`, TOPICS += `"param"`), `stixplat/series.py`
(`param` records → series), `stixplat/viewrun.py` (replay), `selftest.py`.

- [ ] `class Params(power, relay, log, clock, cfg)`: `table()` (lazy
  `params.load(cfg["qemu"]["elf"], ctl.elf_sha256)`, reloaded never —
  the sha256 is per controller), `snapshot()` → the JSON below.
- [ ] `GET /api/v1/params` → `{"elf", "elf_sha256", "symbol": {name,
  address, size}, "count", "differ", "warnings", "at_sim", "params":
  [row...]}` (`differ` = rows with `value != default`, `value != None`).
- [ ] `status()["params"]` = `{"count", "differ", "warnings"}` (the
  mockup's "N parameters differ" line).
- [ ] Changes (from refresh/read-back, T3/T4) emit one record per row:
  kind `"param"`, path `"param.<index>"`, data `{"index", "name",
  "how": "ram"|"readback", "old", "new"}`.
- [ ] series `_feed`: kind `"param"` → num series `param:<index>`,
  label = name, source `"param"`.
- [ ] `params.replay(recs) -> ParamsTable`: rebuild value/readback
  from `param` records (for `--view-run`); viewrun serves GET /params
  from it.
- [ ] Self-tests: `params_api` (controller with the fixture ELF path
  via `env.controller(qemu={"elf": ...})`: shape, count, differ 0,
  table empty warnings path when the ELF is junk → `params: []`,
  `warnings` non-empty, no exception; status summary present);
  `params_series` (feed a `param` record → `param:<index>` in
  catalogue with the name as label); `params_viewrun` (replay of two
  records → values visible, defaults absent).

### Task T3: Current = guest RAM (param.refresh)

**Files:** `stixplat/params.py`, `selftest.py`.

- [ ] Verb `param.refresh` (locked=True): power off → 409; else QMP
  `memsave {val: vaddr, size, filename: "params-ram.bin", cpu: 0}`
  (the file lands in the run dir, qemu.py:196-199), parse count ×
  4 u32 BE at row+4 (`value`), diff vs the cache, emit `param`
  records (`how: "ram"`), set `value`/`value_at`, return the full
  `snapshot()`. QmpError → VerbError(409, message).
- [ ] Self-test `params_refresh_ram`: a fake QMP (socketpair, as
  `t_qmp`) that answers `memsave` by writing the run dir file with a
  fixture image where rows 1 and 2 differ from the initial image →
  two `param` records with old/new, `differ` 2, `at_sim` set; second
  call with the same image → no records, `differ` unchanged; power
  off → 409.

### Task T4: TM(237,20) decode + read-back verbs

**Files:** `stixplat/decode.py`, `stixplat/params.py`,
`stixplat/plat.py` (relay listener), `selftest.py`.

- [ ] `decode.decode`: type 237, subtype 20, app ≥ 7 →
  `d["param"] = {"index": u16, "sub": u8, "value": u32}` BE (mirror of
  the TC(237,18) label; every `tm` record then carries it in
  `decoded`).
- [ ] `Params` registers a relay listener: APID 0x5A8, subtype 20 →
  store `(index, sub) -> (value, t)` under a small lock and set an
  event.
- [ ] Verb `param.read` `{index, sub: 0|1|2 (default 0)}` (gate
  `_gs_enabled` like `tc.gs.send`): index out of the table → 400; no
  table → 409; send TC(237,19) `[u16 id][u8 sub]` via
  `relay.gs_tc(sp, source="api", path="param.read")`; `gs_tc` returned
  False (dropped/held) → 409 with its why; else wait ≤ 2 s for the
  matching TM(237,20) → `{"index", "sub", "value", "at_sim"}`;
  timeout → VerbError(500, "no TM(237,20) for param <index>"). The
  stored answer also updates the row (`readback`, `readback_at`,
  `param` record `how: "readback"`).
- [ ] Verb `param.read_all` `{only_changed: false}` (gate as above,
  locked=False, one worker at a time → 409): sequential `read` of all
  rows (or value != default); progress log record `params.read_all`
  every 32; result `{"read", "failed", "only_changed"}`.
- [ ] Self-tests: `tc_237_20_label` (a relayed TM unit built by
  `_tm_unit(0x5A8, 237, 20, app)` → the tm record's `decoded.param`
  == `{"index": 312, "sub": 0, "value": 9}`; short app → no `param`);
  `params_readback` (fake relay: the verb's TC record shows service
  237 subtype 19, then `relay.handle_tm(...)` delivers the answer →
  value returned, row updated; wrong index → 400); 
  `params_readback_dropped` (link down → 409 with the drop reason, no
  wait: assert the elapsed time < 0.5 s).

### Task T5: param.set and param.reset_all

**Files:** `stixplat/params.py`, `stixplat/plat.py` (`register`),
`stixplat/verbs.py`, `selftest.py`.

- [ ] Verb `param.set` `{index, value (0..0xFFFFFFFF), sub (0|1|2,
  default 0)}` (gate `_gs_enabled`): send TC(237,18)
  `[u16 id][u8 sub][u32 value]` via `relay.gs_tc(..., path="param.set")`;
  return `{"index", "sub", "value", "delivered", "why"?}`; dropped →
  `delivered: false` with the reason (still a 200 — the refusal is
  the answer, like `tc.gs.send`). The TC(237,18) label and the
  FSW's TM(1,7/1,8) arrive through the existing records.
- [ ] Verb `param.reset_all` `{only_changed: true}` (gate, locked=False,
  409 while a worker runs): for every row with `value != default`
  (value known), send TC(237,18) with `default`; progress + result
  `{"reset", "failed"}`; no table → 409.
- [ ] `verbs.py` VERBS += the five `param.*` names.
- [ ] Self-tests: `params_set` (fixture controller: cmd + tc records
  with `data.param == {"index": 1, "sub": 0, "value": 7}`, delivered
  true with the fake relay; sub 1/2 accepted; index out of range 400;
  gate off → 403 with the emulation text); `params_reset_all` (two
  rows differ → two TC(237,18) records with the default values;
  second concurrent call → 409; zero differing → `reset: 0`).

### Task T6: verbs table + docs

**Files:** `docs/stix-port/platform.md`, `docs/stix-port/
accepted-diffs.md`.

- [ ] platform.md: five rows in the normative verbs table (between
  the `verbs:begin/end` markers); the kind list in "Records" gains
  `param` and the topic note; a new "FSW parameters" section after
  Timeline: where defaults/names come from (the running ELF, symbol
  prefix + DWARF enum, warnings on mismatch), Current (RAM snapshot
  via `memsave`) vs read-back (TC(237,19) → TM(237,20)), the Applies
  labels and their FSW evidence, the flash-context note (value may
  differ from default at boot without an operator set), the degraded
  modes.
- [ ] accepted-diffs.md: new section "P-2. FSW parameters table:
  defaults, names and Applies from the running ELF (baseline: none)"
  — what the controller reads from the ELF file and guest RAM, and
  that TC(237,18)/(237,19) remain the only write/read-back path.
- [ ] Self-test `t_verbs_doc` passes unchanged (it compares against
  the table); run `python3 stixplat.py --self-test verbs`.

### Task T7: real-ELF selftest + live smoke

**Files:** `stixplat/selftest.py`, create `scripts/stix/platform/
tests/params_smoke.py`.

- [ ] Self-test `params_real_elf` (skipped with a note when
  `~/dev/stix-elf/ASW_em_baseline.elf` is absent): count == 482,
  symbol address 0x40080908; the row whose name is
  `NOMINAL_ACCU_MIN_INTEGRATION` has default 9, min 0, max 0xFFFF;
  `RCR_ENABLED` default 1; names unique; ParamMgmtDef.h cross-anchors
  (ParamMgmtDef.h:343, :382-383, :386). Same assertions against
  `ASW_em_prof.elf` when present (count 483) — counts may differ per
  build, per-ELF data must not.
- [ ] `tests/params_smoke.py` (API-only, reviewer-run against a live
  controller, like `timeline_smoke.py`): GET /params (count > 0, no
  fatal warnings), `param.refresh` → differ count, `param.set` on a
  harmless row (value = default ± 1 within min/max, else the row the
  script picks) → TM(1,7) in the log, `param.refresh` shows the
  change, `param.read` returns the same value as RAM within one
  refresh, `param.read_all {only_changed: true}` completes, 
  `param.reset_all` restores, `differ` back to 0.
- [ ] Reviewer live check (first run on the real ELF): confirm the
  symbol facts above (`nm` size == count×16), that the DWARF names
  spot-check (≥ 10 names match `ParamMgmtDef.h` order), and that
  `memsave` sees the table (a `param.set` shows up in the next
  refresh).

### Task T8: the UI table (IDPU tab)

**Files:** `scripts/stix/platform/ui/index.html`, `stixplat/selftest.py`,
`stixplat/uicheck.py` (no change expected).

- [ ] New section on the `idpu` tab, mockup `Idpu.dc.html`: header
  line "N parameters differ from default" (from `status.params`),
  find input (index or name substring, client-side), "Only
  non-default" checkbox, "Read all back" (`param.read_all
  {only_changed: false}`), "Reset all…" with `data-confirm`
  (`param.reset_all`); grouped rows (group = the name's first `_`
  token, header row per group, ELF index order kept); columns Index,
  Name, Default, Current (RAM value, `·` + read-back age when
  present), New value (number input), Applies, Set (`param.set`) and
  Reset (`param.set` with the row's default); rows where value !=
  default get the highlight class, the input gets `min`/`max`
  hints. Refresh: `param.refresh` on tab open, every 2 s while the
  tab is visible, and after every set/reset/read-all; a refusal
  (gate off / power off) shows the toast text, not a dead table.
- [ ] Page plumbing: the SSE `topics` list (index.html:2412) and
  `TOPIC` map (:2374) gain `"param"`; a Logs chip for `param`
  (default off, like sample); `onRecord` appends param records to
  the events log.
- [ ] Self-test `t_ui_params`: ids `params`, `params-find`,
  `params-nondef`, `params-readall`, `params-reset`,
  `params-body` present; `UI_PARAMS_VERBS = {param.refresh,
  param.set, param.read_all, param.reset_all}` ⊆ used data-verbs;
  the page rules hold (no style/inline handlers/innerHTML — extend
  the existing loops); `UI_ENDPOINTS` += `"params"`.

## Merge order

T1 → T2 → T3 → T4 → T5 → T6 (docs with the verbs) → T7 → T8.
Gate after T5/T6 and at the end: `python3 stixplat.py --self-test`,
then `tests/core-smoke.sh` and `tests/link-smoke.sh` (they run
`--check-ui`, which fails if the page advertises an unserved verb).
No qtest run needed (no C change), but the reviewer decides.

## Accepted-diffs / docs updates

- accepted-diffs.md: new **P-2** (controller feature; baseline: no
  parameter table, no ELF/DWARF reader in the controller).
- platform.md: "FSW parameters" section, verbs table +5 rows,
  Records kind list + `param`.
- No ground-station.md change (the GS keeps relaying the same TCs;
  the IDB gains nothing — no parameter names exist there).

## Open questions for the owner (do not block)

1. **Gate for the write verbs:** `param.set`/`read*`/`reset_all`
   reuse the `tc.gs.send` gate, so in the default interactive config
   (`[gs] emulation = "auto"`) they are refused until emulation is
   on. Proposed answer: keep one operator-TC gate (no second
   switch), and if the bench finds this annoying, set
   `[gs] emulation = "on"` in the interactive sample config — your
   call, one line.
2. **"Read all back" scope:** the mockup button reads everything
   (482 round trips, ~seconds under icount). Proposed answer: the
   button sends `only_changed: false` as drawn, the verb offers
   `only_changed: true` for the common case; acceptable?
3. **Applies wording:** the labels above are short forms of the FSW
   evidence ("next NOMINAL entry", "on set", "next calibration
   start", "at boot", "unknown"); the RCR nuance (TC(237,10)
   applies immediately) goes in the row tooltip. Proposed answer:
   ship these five labels; extend per group only when the FSW grep
   evidence exists (T7 step).
