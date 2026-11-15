# Detector stimulus Stage S2: spectra (line at ADC, line/flat in keV, from file) — plan (2026-11-15)

> For agentic workers: implement task by task; steps use `- [ ]`.
> Workers cannot build or run anything: write code + self-test cases,
> the reviewer runs them and commits. This plan expands the S2 outline
> of `2026-11-15-stimulus-plan.md` ("Stage S2", Task U1); the outline's
> wire interfaces are fixed and are not reopened here (spec shapes,
> per-step `align`, the D4 CSV format, the D5 keV law, `preimage_miss`
> in `det-align-status`, the trace name `stixdet_spectrum`).

**Goal:** a tape `rate` step can carry a spectrum instead of the quiet
template: a line at an ADC value, a line or a flat band in keV, or a
weighted spectrum read from a two-column CSV. The spectrum is an
ADC-domain stimulus: it is binned through the ELUT the FSW loaded,
exactly as DSS replay events are, so a misaligned ELUT mis-bins it as
on hardware. A keV spectrum can opt into `align` (per step, overriding
the tape), which converts it through the preimage of the loaded ELUT
so it lands in the intended science bin whatever ELUT is loaded.

**Architecture:** everything stays in `stix_det.c` on the 100 ms tick
(S1's engine, D-Dj store-then-apply, under the tick's
`mem_reentrancy_guard`). At `det-tape-load` the parser turns each
`spec` into an owned `StixDetSpec` (a CSV file is read and parsed
there, once, host-side; the tape keeps the parsed points). While a
spectrum step runs, a **per-spec bin-mass row** `spec_row[fpga_det][pix]
[ebin]` (the fraction of the step's counts the loaded ELUT row puts in
each ebin) is built at the step start and rebuilt at each QL swap, by
one shared adc → ebin helper (the replay's honoured/fallback lookup,
factored out). The sky tick adds the step's counts for its pairs into
a destination-ebin exposure (`spec_expo`, `nom_spec_expo`) instead of
the template exposure; the draw adds one Poisson draw per cell only
for banks that hold spectrum exposure, after C1's fold (spectrum
counts are already ELUT-binned and never fold twice). The platform
(`tapes.py`) mirrors the model's rules as the validator of record,
keeps spectrum CSVs in a `spectra/` dir next to the tapes and resolves
them to absolute paths at play; the UI's Spectrum control is enabled.
The FSW-visible end is a new `detector-smoke.sh --spectrum` leg.

**Spec/inputs:** the S2 outline (stimulus plan "Stage S2", Task U1),
decisions D1 (ADC-faithful sky), D4 (CSV format), D5 (the keV law);
as built: `accepted-diffs.md` §54 (S1), §55 (alignment), §56 (ADC
sky). Mapping to the outline: the outline's U1 is U1–U4 here, its U2
gate is U5.

## Key facts

Read in this worktree unless marked UNVERIFIED.

- **The parser today.** `stix_det_tape_parse_step`
  (`hw/sparc/stix_det.c:2651`) refuses any `spec` other than the string
  `"flat"` with "spec values other than \"flat\" are unsupported in this
  build (S2/S3)" (`:2730-2737`), kind `single` with "unsupported in this
  build (S2/S3)" (`:2680-2683`), and unknown step fields against the
  `known[]` list `kind, for_s, label, fpga_det_mask, pix_mask, cps,
  spec, file, offset_ms` (`:2778-2781`) — so a per-step `"align"` is
  refused today as an unknown field. The tape-level `align` is parsed
  and stored (`tape_align`, `:2853`, `:2925`) and reported in
  `det-tape-status`, but nothing consumes it yet.
- **"flat" is the S1 template.** A rate step without `spec` (or with
  `"flat"`) flows through the quiet profile's `pix_share`/`ebin_share`
  template (`stix_det_sky_tick`, `:1205`; template `:1004-1043`: 10 %
  over ebin 1-11, 15 % over 12-30, lines at 18 (45 %), 19 (10 %), 27
  (20 %)). The wire name stays `"flat"` (S1, fixed); the UI labels it
  "template".
- **The ELUT lookup.** Honoured row: `stix_det_replay_elut_ebin`
  (`:1500-1508`) reads `val[pix][fpga_det][adc]` from the SRAM ram
  pointer, 32 = unassigned (dropped by the caller). Degenerate row:
  `stix_det_replay_fallback_ebin` (`:1516-1525`), `adc <= base`
  dropped by the caller; both branches are chosen inline in
  `stix_det_replay_tick` (`:1903-1917`) from `replay_row_fallback`,
  which only `stix_det_replay_classify` (`:1588`) refreshes, at the
  points `stix_det_classify_wanted` (`:1531-1535`) allows.
- **The calibration (D5, C1).** `stix_det_cal_base` (`:1542`, the
  replay table's 0xE baseline or 0), `stix_det_cal_ebin` (`:1556`,
  the D-7 grid), `det-cal-kev-full` `K` (default 160, `adc_kev_full`).
  keV → ebin `floor(32·keV/K)`; `line_kev` adc `base + round(gain·keV)`,
  `gain = (4096 − base)/K` (D5, fixed).
- **The preimage helper** `stix_det_align_adc(d, fpga_det, pix, ebin)`
  (`:1654-1683`, declared `stix_det.h:401`): the midpoint of the
  longest run of adc mapping to `ebin`, −1 for a degenerate row or no
  preimage. Not called yet.
- **The C1 fold.** `stix_det_adc_map_rebuild` (`:1716`) and
  `stix_det_adc_fold_share` (`:1782`) fold the *template* mass;
  `stix_det_draw_acc` (`:2002-2058`) splits each drawn `n_sky` by the
  map and adds `inj` after the fold. `stix_det_ql_swap` (`:221-249`)
  classifies (and rebuilds the map) before `stix_det_sky_materialize`.
- **The pix gate.** `stix_det_pix_gate` (`:1152-1166`): a pix counts
  only while its R3 threshold is < 63. In qtests `idefx_power_all`
  leaves the defaults (62, all 12 pix open). With the FSW, after
  quarter power-on each detector's R3 is rewritten so only pix 0, 3,
  6, 7, 8 are below 63: the model emits (4 large + 1 small)/8.88 =
  0.475 of the configured cps. This is FSW configuration, not a model
  bug.
- **Status strings.** `det-align-status` (`:3019-3035`) prints
  `mode=` from the `det-align` knob only (not the tape's `align`). The
  qtest `det-align-status` compares four whole lines
  (`tests/qtest/stix.c:9998-10037`); `align-smoke.sh` matches its
  `STATUS_*` lines as substrings (`grep -qF`, `:203-208`, `:1056`),
  so appending a key keeps it green. `plat.py` `split_kv` (`:34-42`)
  splits on commas; `v_align_set` (`plat.py:534-540`) casts a fixed key
  list.
- **Platform.** `tapes.py` accepts any dict `spec` and any per-step
  `align` bool (`:93-106`); the self-test pins that S2 shapes parse
  (`selftest.py:2810-2815`). `state.py:111-116` maps every key of the
  hw-state `det.tape`/`det.align`/`det.adc` objects to leaves, so new
  keys become leaves without code. The Stimulus board's Spectrum chip
  is `disabled` (`ui/index.html:479-480`); `stimSettings` shows only
  "· spec" (`:1931-1934`).
- **Gates.** `detector-smoke.sh` is one boot with a fixed timeline
  (CONF @50, S20 @60, verification period @62, NOMINAL @70, CONF @215;
  `:410-466`); `--tape` loads/plays at @75 and uses
  `--expect-tape-flush`; it has no ELUT-upload chain. `align-smoke.sh`
  has it: `encode_args` (`:290-305`), the synthetic 32-setup-word
  `.dssevs` (`write_adc_dssevs`, `:279-288`), the base-0 wrong CSV
  (`:310-317`, `:600`), and the UART rule after an applied TC(237,8)
  (exactly 4 "problem erasing LUT type=3," lines, `uart_ok`,
  `:437-460`). `ql-check.py --ssid32-byte SUBCOL EBIN` takes the
  0-based ebin (`:101-106`, `:738-764`).
- **The base-0 derived ELUT** (C3 leg 4's `wrong-base0.csv`): every row
  bins `adc → floor(adc/128)` (edges `128·(k+1)`, the honoured oracle
  puts `adc = e[k]` in bin k+1; B0 = 0 so adc 0..127 → ebin 0).

## Global constraints

Inherited from the stimulus plan (unchanged): no FSW edits (STIX-FSW
read-only); every new name carries its index space (`fpga_det`,
`subcol`, `pix`, `ebin`, `adc`; `keV` for energies); API v1 additive
only; UI rules (one `ui/index.html`, no innerHTML / inline handlers /
inline styles, verbs via `data-verb`); guest-time scheduling only;
QOM setters store fields only and the tick applies them; goldens
unchanged (every new field defaults off/absent; nothing new at boot);
`python3 stixplat.py --self-test` stays PASS and every task adds
`check(...)` cases. Plus, for S2:

- **Guest memory only under the tick guard.** The spectrum row build
  reads the ELUT SRAM only inside the tick (step start in
  `stix_det_tape_tick`, the swap) under the `mem_reentrancy_guard`,
  never from QMP; hw-state and status getters are pure reads (PL-2).
  The CSV read is host file I/O in the `det-tape-load` setter only
  (like the realize-time `det-replay-file`), never in the tick.
- **Default-off is bit-identical.** A tape whose steps have no spectrum
  (`spec` absent or `"flat"`) makes no extra RNG draws and no new
  writes: `det-tape-seed`, `det-adc-off` and every S1/C1 test keep
  their values. The extra Poisson draw exists only for banks with
  spectrum exposure.
- **No double fold.** Spectrum counts are ELUT-binned at build time;
  they never pass C1's map (with `det-adc-mode` on, only the template
  mass folds).
- **One rounding rule.** "round" in D5 is `floor(x + 0.5)` (half up) in
  C and in any Python mirror — never Python's `round()` (half-even).
- **Lesson 1 (byte vs ebin).** `ql-check --ssid32-byte` and the new
  `--ssid32-peak` take the 0-based ebin; "byte N" of an SSID 32 sample
  is ebin N−1 (byte 0 is the detector byte). This plan writes ebins
  only: the outline's "peaks at byte 13" is **ebin 12**
  (`--ssid32-byte SC 12`, `--ssid32-peak SC:12`).
- **Lesson 2 (status mode).** `det-align-status` reads `mode=on` only
  when the `det-align` knob is set; the per-step/tape `align` does not
  change it. The S2 gate does not set `det-align`, so its expected
  line starts `mode=off`. Every expected status string in this plan
  states the knobs of its leg.
- **Lesson 3 (non-vacuous negatives).** Every `--negative` must fail on
  the intended evidence: the spectrum negative fails exactly
  `spectrum-bin` while `spectrum-line` (the line exists) and the
  negative-only `spectrum-arrival` (it arrived at the predicted
  unaligned ebin) pass. Every qtest "exactly 0" assertion is paired
  with an arrival assertion elsewhere.
- **Lesson 4 (the quiet template).** The spectrum gate and the spectrum
  qtests silence the template: `det-quiet-cps=0` and tape background
  `cps 0`; unmasked subcols are then asserted exactly zero.
- **Lesson 5 (pixels, relative thresholds).** With the FSW only pix 0,
  3, 6, 7, 8 count (0.475 of the configured cps). Gate checks are per
  `subcol` sample sums and relative (the peak ebin ≥ 8 compressed steps
  above every other ebin of the same sample; unmasked exactly zero),
  never absolute from cps × time, and never assume 12 pixels; step
  `pix_mask` stays 0xFFF in the gate. qtests run with all 12 pix open
  (`idefx_power_all`) and say so; their ±5σ Poisson bounds are
  model-internal, not FSW expectations.
- **Lesson 6 (evidence).** Gate checks log the bytes and status lines
  they decide on (`ql-check` prints every inspected sample's ebin
  bytes and argmax; the gate echoes the status return and the trace
  lines). A failed live run is investigated, not re-run.

## Approaches considered

1. **Where the spectrum binning happens.** (i) Per-event adc draws in
   the tick: rejected (20000 cps × 384 pairs, a changed RNG stream —
   stimulus plan approach 4(i)). (ii) A per-spec bin-mass row built at
   step start and each swap, mass accumulated per tick in destination-
   ebin space (recommended; the outline's "per-spec bin-mass map, one
   row"). (iii) Accumulate in source (adc) space and fold at the draw
   with a row rebuilt before `materialize` (C1-like pickup): needs a
   per-step exposure flush whenever a step changes mid-period;
   rejected for complexity (open question 5).
2. **ELUT pickup.** With (ii) the row is the ELUT as read at the step
   start and at each QL swap; a mid-period TC(237,8) rewrite reaches
   the bank after the next swap — the bank closed by the first swap
   after the rewrite still shows the old binning, as in the replay's
   `det-replay-elut` bank-1 precedent (§37), deliberately unlike C1.
   Cost: a line spec is 384 lookups per build; a band/file spec at
   most 384 × 4096 adc per build, per swap — the C1 bound, zero when
   no spectrum step runs.
3. **The gate's ELUT.** On a derived (aligned) ELUT, or on a fresh
   flash (degenerate rows → fallback = calibration), `align` is a
   no-op, so an `align`-off negative cannot fail. The gate therefore
   uploads C3's deliberately wrong base-0 ELUT while the calibration
   baselines are `1800 + 10·d` (open question 2).

## Review focus (five input classes, each pinned to a test)

1. **Spec values and the keV law** (adc vs keV vs ebin, D5 rounding,
   the per-step `align` override): pinned by `det-spec-line` — exact
   destination ebins per `fpga_det` (12 / 21 / 12 for one keV value
   under three row states) and the gate's `spectrum-bin` +
   `spectrum-arrival` (ebin 12 aligned, ebin 21 unaligned).
2. **ELUT row states** (honoured, degenerate, unassigned/below-base
   drops, no preimage): pinned by `det-spec-line` — the dropped pairs'
   totals are exactly 0 *and* the same step's other pairs arrive at
   their bin; `preimage_miss=12` for the one row without ebin 12.
3. **Host file content** (the CSV: header, grammar, limits, CRLF,
   duplicates, file changed after load): pinned by `det-spec-file` and
   `det-spec-props`, with the same conformance vectors in the
   platform's `tapes_spec_csv` (the validator of record).
4. **Mode combinations** (`spec` absent vs `"flat"`, `det-adc-mode`
   on, tape vs step `align`): pinned by `det-spec-flat-ident` (bit-
   identical banks, the RNG stream untouched before the first spectrum
   bank) and `det-spec-line`'s `adc_mode` run (no double fold).
5. **Time and context** (step boundaries, ELUT rewrite mid-step,
   ClearEnable, steps under `clock_step`): pinned by `det-spec-rebuild`
   (the closed bank keeps the old bin, the next bank moves) and
   `det-spec-guard` (every CTRL MMIO path answers).

---

## Task U1: spec parse, the CSV read, status and hw-state (model)

**Files:** modify `hw/sparc/stix_det.c`, `hw/sparc/stix_det.h`,
`hw/sparc/trace-events`, `tests/qtest/stix.c`.

**Interfaces — produces:**

- `stix_det.h`:
  ```c
  typedef enum StixDetSpecKind {
      STIX_DET_SPEC_FLAT,         /* the quiet template (S1 "flat") */
      STIX_DET_SPEC_LINE_ADC,
      STIX_DET_SPEC_LINE_KEV,
      STIX_DET_SPEC_FLAT_KEV,
      STIX_DET_SPEC_FILE_ADC,     /* CSV header adc,weight */
      STIX_DET_SPEC_FILE_KEV,     /* CSV header kev,weight */
      STIX_DET_SPEC__MAX,
  } StixDetSpecKind;

  typedef struct StixDetSpecPoint {
      double value;               /* adc (integer) or keV */
      double weight;              /* >= 0, relative */
  } StixDetSpecPoint;

  typedef struct StixDetSpec {
      uint8_t kind;               /* StixDetSpecKind */
      bool align;                 /* effective: the step's, else the tape's */
      double v0, v1;              /* line: v0; flat_kev: [v0, v1) keV */
      StixDetSpecPoint *pts;      /* file: owned copy, 1..4096 points */
      uint32_t n_pts;
      char *desc;                 /* trace/hw-state text, see below */
  } StixDetSpec;

  #define STIX_DET_SPEC_MAX_POINTS 4096u
  #define STIX_DET_SPEC_MAX_FILE (256u * 1024u)
  ```
  `StixDetTapeStep` gains `StixDetSpec spec;` (zero = FLAT).
- `stix_det.c` (static):
  ```c
  static bool stix_det_spec_parse(StixDetState *d, QDict *step,
                                  bool tape_align, StixDetTapeStep *st,
                                  uint32_t idx, Error **errp);
  static bool stix_det_spec_file_load(const char *path, uint32_t kev_full,
                                      StixDetSpec *sp, uint32_t idx,
                                      Error **errp);
  static void stix_det_spec_free(StixDetSpec *sp);
  ```
  called from `stix_det_tape_parse_step` in place of today's refusal;
  `stix_det_spec_free` from `stix_det_tape_clear` and from the load
  setter's error unwind (which today frees only labels, `:2909-2911`).
- `known[]` gains `"align"`. The `single` refusal text becomes
  "kind \"single\" is unsupported in this build (S3)".
- **The rules (the model and `tapes.py` implement this one table; every
  error names the step index, file errors also the path and line):**

  | field | rule |
  |---|---|
  | `spec` | absent or `"flat"` (any kind, as S1), or an object with exactly one key of `line_adc`, `line_kev`, `flat_kev`, `file`; a non-flat spec only on kind `rate` |
  | `line_adc` | JSON integer 0..4095 (`1600.0` refused, as `qnum_get_try_int`) |
  | `line_kev` | JSON number, `0 <= v < K` (K = `det-cal-kev-full` at load) |
  | `flat_kev` | array of exactly two numbers, `0 <= k1 < k2 <= K` |
  | `file` | string 1..4095 bytes; read at load (below) |
  | step `align` | optional bool, default = the tape's `align`; an explicit `true` only on `line_kev`, `flat_kev` or a `kev,weight` file ("align applies to keV specs"); the tape-level default is ignored for adc and flat specs |
  | CSV size | ≤ 256 KiB (`STIX_DET_SPEC_MAX_FILE`), checked before reading |
  | CSV header | the first line exactly `adc,weight` or `kev,weight` |
  | CSV rows | 1..4096 lines `value,weight`; no spaces; an optional `\r` before `\n`; the final newline optional; no blank or comment lines |
  | numbers | `[0-9]+(\.[0-9]+)?([eE][-+]?[0-9]+)?`, finite (no sign, no `inf`/`nan`, no `0x`, no `_`) |
  | adc value | `[0-9]+`, 0..4095 |
  | keV value | a number, `< K` |
  | weights | numbers ≥ 0, sum > 0; repeated values are summed |

  The file is read once at load into `pts`; a later change or deletion
  of the file does not affect the loaded tape. Relative paths resolve
  against QEMU's working directory (the platform always passes absolute
  paths, U3).
- `desc` (the trace and hw-state text, no comma): `flat`,
  `line_adc=1600`, `line_kev=62`, `line_kev=62 align`,
  `flat_kev=40..60 align`, `file=<basename> unit=adc rows=2` (keV
  printed with `%g`; `align` appended when effective).
- `det-align-status` gains a trailing `,preimage_miss=N` (the outline's
  fixed field; defined in U2; 0 until a build counts). hw-state
  `det.align` gains `preimage_miss`; `det.tape` gains `spec` (the
  running step's `desc`, `""` when no tape). Both additive.
- Trace (fixed name), `hw/sparc/trace-events`:
  `stixdet_spectrum(uint32_t step, const char *spec, int64_t vclock_ns)
  "spectrum step %u: %s at vclock_ns=%" PRId64` (emitted by U2).

Steps:

- [ ] qtest `det-spec-props` (RED on the parent: every S2 shape is
  refused there). Each rejection is a failed `det-tape-load` and leaves
  the previous tape intact (`det-tape-status` unchanged). Vectors
  (shared with U3's self-test, same order):
  - spec: `{"line": 1}`; `{"line_adc": 1600, "line_kev": 62}`; `{}`;
    `"line"`; `[1]`; a `line_adc` spec on an `off` step;
  - `line_adc`: 4096, −1, 1600.0, `"1600"`;
  - `line_kev`: −1, 160 (= K), `"62"`; after `det-cal-kev-full=100`,
    120;
  - `flat_kev`: `[60, 40]`, `[40]`, `[40, 161]`, `[-1, 5]`;
  - `align`: `"yes"`; `true` with `line_adc`; `true` with an
    `adc,weight` file; `true` with `"flat"`;
  - file: `""`; a missing path (the error names it); a 262145-byte
    file; header `energy,weight`; a first line `1600,1`; header only
    (0 rows); 4097 rows; `1600,1,2`; `1600, 1`; `1600,nan`;
    `1600,inf`; `1600,1e999`; `0x10,1`; `+5,1`; `1600,-1`; all
    weights 0; `4096,1` (adc file); `160,1` (kev file, K 160); a blank
    line between rows.
  Accepted (each loads, `state=ready`): `"flat"`; `{"line_adc": 0}`;
  `{"line_adc": 4095}`; `{"line_kev": 0}`; `{"line_kev": 159.9}`;
  `{"flat_kev": [0, 160]}`; a CRLF `kev,weight` file; a file without a
  final newline; a step `align: true` on `line_kev` with tape `align`
  false; tape `align: true` with a `line_adc` step (ignored there).
- [ ] Update `det-tape-props`: its "S2 spec" case
  (`{"line_adc": 1000}`, `tests/qtest/stix.c:9539-9542`) is now
  accepted — change it to `{"line_adc": 4096}` (still refused, for its
  range); the `single` case stays refused.
- [ ] Update `det-align-status`: the four expected lines gain
  `,preimage_miss=0` at the end; add one assertion that the line
  splits into exactly 8 `key=value` fields.
- [ ] Implement the types, the parser, the CSV reader
  (`g_stat` size check, `g_file_get_contents`, a hand-written line
  scanner implementing the grammar, `g_ascii_strtod` only on
  grammar-checked text, `isfinite`), `desc`, the freeing paths, the
  status/hw-state fields.
- [ ] Run `scripts/stix/qt.sh all`: the new and updated tests pass,
  the rest unchanged.
- [ ] Commit.

**Verification:** `scripts/stix/qt.sh all` — all ok; `det-spec-props`
RED on the parent binary.

## Task U2: the per-spec row, the sky path, the draw (model)

**Files:** modify `hw/sparc/stix_det.c`, `hw/sparc/stix_det.h`,
`tests/qtest/stix.c`.

**Interfaces — produces:**

- The shared lookup, used by the replay tick (refactor, behaviour
  identical — pinned by the `det-replay-*` tests) and the spectrum
  build:
  ```c
  /* the ebin the loaded ELUT gives (fpga_det, pix, adc): honoured row
   * val (32 -> -1, unassigned); degenerate row the D-7 fallback
   * (adc <= base -> -1) */
  static int stix_det_adc_ebin(StixDetState *d, uint32_t fpga_det,
                               uint32_t pix, uint32_t adc);
  /* D5: base + floor(gain * kev + 0.5), gain = (4096 - base) / K;
   * -1 when the result is past 4095 (an ADC overflow: dropped) */
  static int stix_det_kev_adc(const StixDetState *d, uint32_t fpga_det,
                              double kev);
  /* D5: floor(32 * kev / K), clamped to 0..31 */
  static uint32_t stix_det_kev_ebin(const StixDetState *d, double kev);
  static bool stix_det_spec_active(const StixDetState *d);
  static void stix_det_spec_build(StixDetState *d);
  ```
- `StixDetState` gains
  `double spec_row[STIX_DET_N_FPGA_DET][STIX_DET_N_PIX_ACC][STIX_DET_N_EBIN];`
  `double spec_expo[...same...]`, `double nom_spec_expo[...same...]`,
  `bool spec_expo_any, nom_spec_expo_any`, `uint32_t
  spec_preimage_miss` (~300 KB; the C1 map is 3 MB).
- **The build** (`stix_det_spec_build`, one pass over the 384 pairs of
  the running step's masks; `spec_row` = 0 outside them). Per pair the
  spec becomes weighted adc points, each point's weight going to
  `stix_det_adc_ebin` of its adc (a −1 drops it), normalized by the
  spec's total weight:
  - `line_adc A`: one point at A.
  - `line_kev E`, align off: one point at `stix_det_kev_adc(E)`.
  - `flat_kev [k1, k2)`, align off: adc `a` weighted by the overlap of
    `[a, a+1)` with `[base + gain·k1, base + gain·k2)`.
  - file: each point as `line_adc`/`line_kev`, weighted.
  - **align on** (keV specs, honoured row): the keV mass goes to ebins
    by the D5 law (`line_kev`, file points: `stix_det_kev_ebin`;
    `flat_kev`: ebin e gets the overlap of `[k1, k2)` with
    `[e·K/32, (e+1)·K/32)`), then each ebin e with mass takes the adc
    `stix_det_align_adc(d, fpga_det, pix, e)` and lands at
    `stix_det_adc_ebin` of it (= e). −1 on a honoured row is a
    preimage miss: that mass is dropped and `spec_preimage_miss`
    counts one per (pair, ebin). On a degenerate row align is a no-op
    (the fallback is the calibration grid): the align-off path runs.
  - `spec_preimage_miss` is a gauge: reset at each build, so
    `det-align-status` reports the running step's last build (0 for
    a step without an aligned spec).
  - Built at the step start (`stix_det_tape_apply_step`: classify
    first, then build) and at the start of `stix_det_ql_swap` while a
    spectrum step runs (after the classification, before
    `materialize`; the rebuild affects the next period's ticks only —
    approach 2). Each build traces `stixdet_spectrum(step, desc,
    vclock_ns)`.
  - `stix_det_classify_wanted` gains `|| stix_det_spec_active(d)` (a
    playing tape whose running step has a non-flat spec), so the row
    states are fresh without `det-align`.
- **The sky tick.** For a pair inside a spectrum step's masks (and
  through the pix gate, as every pair): `c = tick_counts ·
  pix_share[pix]`; `spec_expo[f][p][t] += c · spec_row[f][p][t]` for
  every t (and `nom_spec_expo` likewise inside SUMDMASK, else
  `nom_trg_extra_grp += c · Σ_t spec_row`); SUMCNT/LVAR lambdas take
  `c · Σ_{t in mask} spec_row[f][p][t]` (destination ebins: no C1
  fold); nothing is added to `expo`/`nom_expo` for that pair. Set the
  `*_any` flag on the first add.
- **The draw.** `stix_det_draw_acc` gains
  `const double (*spec)[STIX_DET_N_PIX_ACC][STIX_DET_N_EBIN]` and
  `bool spec_any`; inside the existing loop, after the sky draw and
  its fold and before `inj`: when `spec_any`,
  `n_spec = stix_det_poisson(d, spec[fpga_det][pix][ebin])`,
  `cell[ebin] += n_spec`, counted in `pair_total` (LTRG 1.25× as for
  the sky). The expo-clear helpers clear the spectrum exposure and its
  flag. With `spec_any` false the loop is unchanged.

Steps (all qtests: QlIntTime 9 → 1 s periods, `det-quiet-cps=0`,
tape background `cps 0`, `idefx_power_all` (all 12 pix open), play on
the tick after a swap as `det-tape-rate` does, each spectrum step
`for_s 1` = one whole period, cps 12000 on `fpga_det_mask 0x7`, a
replay table from `det_adc_file_baselines(&f, 1800)` (base 1800, K
160, gain 14.35), and three row states written with `qtest_memwrite`
to all 12 pix: `fpga_det` 0 the wrong row `adc/128`
(`det_adc_wrong_row`), `fpga_det` 1 untouched (degenerate → fallback),
`fpga_det` 2 the "no-12" row (`adc/128`, value 12 replaced by 13)):

- [ ] qtest `det-spec-line`, one tape of five steps, each bank the
  swap just closed (Σ = over pix; "≈ N" = `sky_assert_poisson` ±5σ;
  "0" = exactly 0):

  | step | spec | `fpga_det` 0 (adc/128) | 1 (degenerate) | 2 (no-12) |
  |---|---|---|---|---|
  | 1 | `line_adc 1600` | ebin 12 ≈ 12000, rest 0 | total 0 (1600 ≤ base) | ebin 13 ≈ 12000 |
  | 2 | `line_kev 62` | ebin 21 ≈ 12000 (adc 2690) | ebin 12 ≈ 12000 | ebin 21 ≈ 12000 |
  | 3 | `line_kev 62`, step `align: true` | ebin 12 ≈ 12000 (preimage 1600) | ebin 12 ≈ 12000 | total 0 (miss) |
  | 4 | `flat_kev [40, 60]`, `align: true` | ebins 8..11 each ≈ 3000, rest 0 | ebins 8..11 each ≈ 3000 | ebins 8..11 each ≈ 3000 |
  | 5 | `off` | all 0 | all 0 | all 0 |

  `det-align-status` read during step 3 ends `preimage_miss=12` (12
  pix of `fpga_det` 2) and starts `mode=off`; during step 2 and step 4
  `preimage_miss=0`. A `-trace stixdet_spectrum -D` log holds
  `line_adc=1600`, `line_kev=62` (without ` align`), `line_kev=62
  align` and `flat_kev=40..60 align` (`det7_count_str` ≥ 1 each). A
  second run of step 1 alone with the tape's `"adc_mode": true`:
  `fpga_det` 0 ebin 12 ≈ 12000 and ebins 20/21 exactly 0 (a double
  fold through the C1 map would move the base-1800 ebin-12 range
  [2661, 2733) to 20/21).
- [ ] qtest `det-spec-file`, CSVs in a temp dir, on `fpga_det` 0 (the
  wrong row): (a) `adc,weight` / `1600,3` / `2200,1` → ebin 12 ≈ 9000,
  ebin 17 ≈ 3000 (2200/128), rest 0 — the file is `unlink`ed after the
  load and before the play (the parsed copy plays); (b) CRLF
  `kev,weight` / `62,1` / `121,1` with step `align: true` → ebins 12
  and 24 each ≈ 6000; (c) `adc,weight` / `1600,1` / `1600,1` /
  `2200,2` (repeats summed) → ebins 12 and 17 each ≈ 6000. The trace
  holds `file=<basename> unit=adc rows=2`.
- [ ] qtest `det-spec-flat-ident`: same seed, three runs of three 1 s
  periods: A a rate step with no `spec`, B the same with
  `"spec": "flat"` → banks 1-3 bit-identical (A = B); C = A's step
  for periods 1-2 then a `line_adc 1600` step → banks 1 and 2
  bit-identical to A (the extra draws start only in the first bank
  with spectrum exposure), bank 3 differs.
- [ ] qtest `det-spec-rebuild`: `line_adc 1600` on `fpga_det` 0 for
  `for_s 3`; after the first swap and 5 ticks the test rewrites
  `fpga_det` 0's 12 rows to `min(adc/128 + 1, 31)` → the bank closed by
  the first swap after the rewrite: ebin 12 ≈ 12000, ebin 13 exactly 0
  (the row is the ELUT read at the step start/swap — the replay's
  bank-1 precedent); the next bank: ebin 13 ≈ 12000, ebin 12 exactly
  0. A ClearEnable mid-step freezes `step_left_ticks` and no spectrum
  exposure accrues (the bank after resume is still one period's
  worth: ≈ 12000).
- [ ] qtest `det-spec-guard`: the `det-tape-guard` pattern with a
  `line_adc` and a file step changing under `clock_step` — every CTRL
  MMIO path still answers.
- [ ] Implement; the replay tick switches to `stix_det_adc_ebin`.
- [ ] `scripts/stix/qt.sh all`: all ok, including `det-tape-seed`,
  `det-adc-off`, `det-adc-wrong`, `det-replay-elut` unchanged.
- [ ] Commit.

**Verification:** `scripts/stix/qt.sh all`; each new test RED on the
U1 binary (spectrum steps load but put nothing anywhere); `git grep -c
stixdet_spectrum hw/sparc/trace-events` = 1.

## Task U3: the platform — validator of record, spectra dir, play, leaves

**Files:** modify `scripts/stix/platform/stixplat/tapes.py`,
`stixplat/plat.py` (`v_tape_play`, `v_align_set`),
`stixplat/selftest.py`, `docs/stix-port/platform.md`.

**Interfaces — produces:**

- `tapes.py`:
  ```python
  SPEC_KEYS = ("line_adc", "line_kev", "flat_kev", "file")
  MAX_SPEC_ROWS = 4096
  MAX_SPEC_FILE = 256 * 1024
  KEV_FULL_MAX = 4096          # det-cal-kev-full's range cap
  def spectra_dir(cfg): ...    # <tapes_dir>/spectra
  def parse_spectrum_csv(text, kev_full, where): ...  # -> (unit, [(v, w)])
  def resolve(cfg, tape, kev_full): ...  # copy, file -> absolute path
  ```
  `_step` implements U1's rule table exactly (same messages' key words:
  "align applies to keV specs", "is not an integer", the step index).
  At save, keV bounds use `KEV_FULL_MAX` (the model's K is not known
  offline); `resolve` re-checks with the live K.
- Platform file rule (a stricter subset of the model's): a saved
  tape's `spec.file` is a bare name `<NAME_RE>.csv` in
  `spectra_dir(cfg)` (no `/`, checked before any file access). The
  model accepts any path; only the platform restricts.
- `GET /api/v1/tapes` gains `"spectra": [names]` (the `.csv` files in
  the spectra dir; additive).
- `det.tape.play`: `tapes.read` → `resolve(cfg, tape, K)` with `K` from
  the leaf `instrument.detector.adc.kev_full` (160 when absent) — reads
  and parses each referenced CSV with `parse_spectrum_csv` (a bad or
  missing file is a 400 before any QMP) and passes the absolute path
  to `det-tape-load`. The saved file is never rewritten.
- `v_align_set` casts gain `"preimage_miss": int`. The leaves
  `instrument.detector.align.preimage_miss` and
  `instrument.detector.tape.spec` appear through `state.py`'s
  pass-through (no code); the sampler globs already match them.

Steps:

- [ ] Self-test `tapes_spec`: U1's vector list (the JSON ones) through
  `tapes.validate` — each rejected vector a 400 naming the step index,
  each accepted vector validates; replace the "S2/S3 shapes parse" case
  (`selftest.py:2810-2815`) with these.
- [ ] Self-test `tapes_spec_csv`: U1's file vectors through
  `parse_spectrum_csv` (same order, same verdicts), plus the accepted
  CRLF / no-final-newline / repeated-value files; `1e999` and `nan`
  refused although Python's `float` accepts them.
- [ ] Self-test `tapes_spec_play` (fake-QMP verb pattern): a tape
  referencing `s.csv` → the `det-tape-load` JSON carries
  `<spectra_dir>/s.csv` absolute; a missing `s.csv` → 400 and no QMP
  call; `line_kev 150` with the leaf K = 100 → 400 before QMP;
  `file: "../x.csv"` → 400 at save.
- [ ] Self-test `align_leaves`: a fake hw-state with
  `det.align.preimage_miss` 3 and `det.tape.spec "line_adc=1600"` →
  both leaves; `det.align.set` returns `preimage_miss` 3; the
  `sampler_paths` case still passes (no new glob).
- [ ] Implement; `platform.md`: the spectra dir, the `spectra` listing
  field, the file rule, the two leaves.
- [ ] `cd scripts/stix/platform && python3 stixplat.py --self-test` →
  `SELF-TEST: PASS`.
- [ ] Commit.

## Task U4: the Spectrum control (UI)

**Files:** modify `scripts/stix/platform/ui/index.html`,
`stixplat/selftest.py`.

- [ ] The Spectrum chip is enabled (no `disabled`, the "arrive in S2"
  title gone); next to the rate input a `select id="stim-spec"` with
  options `template (flat)` / `line at ADC…` / `line at keV…` /
  `flat keV band…` / `from file…`, and the inputs `stim-spec-adc`
  (int 0..4095), `stim-spec-kev` (number), `stim-spec-k1`/`k2`
  (numbers), `stim-spec-file` (a select filled from the listing's
  `spectra`), shown per option (`hidden`, no inline style), plus an
  `align` checkbox `stim-spec-align` enabled only for the keV options.
  "Add to tape" / "Send now" write the step's `spec` and, when
  checked, `"align": true`.
- [ ] `stimSettings` shows the spec: `2000 cps · line_adc 1600`,
  `· line_kev 62 (align)`, `· flat_kev 40..60`, `· file s.csv`; the
  progress readout shows `instrument.detector.tape.spec`; the align
  box shows `instrument.detector.align.preimage_miss`.
- [ ] Update the blue box: spectra are supported; single events
  remain (S3).
- [ ] Self-test "UI: stimulus board" (`t_ui_stimulus`): the spectrum
  select with its 5 options, the four inputs, the align checkbox and
  the file select exist; the Spectrum chip has no `disabled`; page
  rules still pass.
- [ ] `python3 stixplat.py --self-test` → PASS; the reviewer's browser
  pass (platform.md checklist) adds: compose a `line_kev` step with
  align, save, reload, the table shows it.
- [ ] Commit.

## Task U5: the S2 gate — `detector-smoke.sh --spectrum`

**Files:** modify `scripts/stix/ql-check.py`,
`scripts/stix/detector-smoke.sh`.

**Interfaces — produces:**

- `ql-check.py`:
  - `--ssid32-peak MAP [MARGIN]`, `MAP` = `SUBCOL:EBIN[,SUBCOL:EBIN…]`
    (0-based ebin, lesson 1), `MARGIN` default 8: in the last SSID 32
    product, each listed sample's byte at EBIN is non-zero and ≥
    MARGIN compressed steps above every other ebin byte (0..31) of the
    same sample. Prints each listed sample's 32 bytes, its argmax and
    top two bytes. `def check_ssid32_peak(products, want, margin=8,
    out=None)`.
  - `--ssid32-line SUBCOLS [MARGIN]`: every listed sample has one
    dominant ebin (its top byte ≥ MARGIN steps above its second), every
    other sample is all-zero over ebins 0..31. Prints the same.
    `def check_ssid32_line(products, masked, margin=8, out=None)`.
- `detector-smoke.sh --spectrum [--negative]` (combines with neither
  `--flare`, `--tape` nor `--no-rotb`; run-dir suffix `-spec` /
  `-specneg`; verdict tag `DETECTOR-SMOKE (spectrum)` /
  `DETECTOR-SMOKE (spectrum negative)`). Copies from `align-smoke.sh`
  (with a "keep in step with align-smoke.sh" comment, no refactor of
  align-smoke): `write_adc_dssevs` (32 0xE words, base `1800 + 10·d`,
  no events), the BASE0 JSON, `encode_args`, and the applied-UART rule
  as a new check `uart-elut` (exactly 4 "problem erasing LUT type=3,"
  lines, none of align-smoke's forbidden LUT lines).

Steps:

- [ ] `ql-check.py --self-test` cases (the `build_packet`/`ssid32_body`
  pattern): peak at ebin 12 with every other byte 0 → pass; peak 0x40
  with a neighbour 0x39 → fail (margin); peak at 21 asked at 12 →
  fail naming argmax 21; an all-zero listed sample → fail;
  `--ssid32-line` with one unmasked sample holding a non-zero byte →
  fail; a listed sample with two equal top bytes → fail.
- [ ] The `--spectrum` timeline (seconds from the TMTC connect; the
  default mode's times after @62 shift by 85 s):
  - QEMU: `-global grlib-stixctrl.det-replay-file=<run>/dssevs.spec`,
    `det-replay-trigger=qmp` (the replay engine stays idle),
    `det-quiet-cps=0`; the trace events list adds `stixdet_spectrum`,
    `stixdet_tape_step`, `stixdet_align`.
  - host, before the boot: `elut-derive.py derive --base "$BASE0"
    --out <run>/wrong-base0.csv`; `encode_args` with `--t0 64` (96 ×
    TC(237,7) at 64 + 0.75 k, TC(237,8) at ≈141, each TM(1,1)/TM(1,7)
    as emitted).
  - @50 CONF, @60 S20 = 0, @62 the verification period (as today),
    @64.. the upload, @155 NOMINAL (T_nom, its TM(5,1) within 10 s).
  - @160 QMP `det-tape-load` + `det-tape-play true`, the tape
    `{"api": 1, "name": "gate-det-spectrum", "align": false,
    "background": {"cps": 0}, "steps": [`
    `{"kind": "rate", "label": "kev Q1", "for_s": 12, "cps": 2000,
    "fpga_det_mask": 255, "spec": {"line_kev": 62}, "align": true},`
    `{"kind": "rate", "label": "adc Q3", "for_s": 12, "cps": 2000,
    "fpga_det_mask": 16711680, "spec": {"line_adc": 1600}},`
    `{"kind": "off", "for_s": 30, "fpga_det_mask": 4294967295}]}`;
    `--negative` drops step 0's `"align": true` (tape `align` false:
    the override is what the positive exercises).
  - @180 `qom-get det-align-status` (during step 1).
  - @300 CONF (T_exit) + the five-product flush within 15 s; hold to
    the default duration + 85 s.
- [ ] Checks (each echoes what it decided on):
  - base: probe, UART (incl. `uart-elut`), `trace-verif`, `uart-chunk`,
    and the flush with `--expect-tape-flush (T_exit − T_nom)` in both
    legs; the D-5 chunk checks are skipped as in the tape modes.
  - `spectrum-status`: the @180 return starts `mode=off` (lesson 2:
    `det-align` is not set) and contains `fallback=0,honoured=384`,
    `calibration=dss-linear` and `preimage_miss=0` (the base-0 row
    has every ebin).
  - `spectrum-trace`: `stixdet_spectrum` lines for `step 0: line_kev=62
    align` (negative: `step 0: line_kev=62` and no `align` on step 0)
    and `step 1: line_adc=1600`, both after the enabling
    `stixdet_fsm_strobe` in trace order.
  - `spectrum-line`: `--ssid32-line
    0,1,5,6,4,10,11,12,30,31,25,26,21,27,19,20 8` (the 16 masked
    subcols = `SUBCOL_FROM_FPGA_DET[f]` for `f` 0-7, 16-23 have a line;
    the other 16 are exactly zero — the template and background are
    silenced, lesson 4).
  - `spectrum-bin`: `--ssid32-peak
    0:12,1:12,5:12,6:12,4:12,10:12,11:12,12:12 8` — Q1's aligned keV
    line at ebin 12 (`floor(32·62/160) = 12`; preimage 1600 in the
    base-0 row).
  - `spectrum-adc`: `--ssid32-peak
    30:12,31:12,25:12,26:12,21:12,27:12,19:12,20:12 8` — Q3's
    `line_adc 1600` at ebin 12 (`floor(1600/128)`), align-independent.
  - `--negative` only, `spectrum-arrival`: `--ssid32-peak
    0:21,1:21,5:21,6:21,4:21,10:21,11:21,12:21 8` — unaligned, `adc =
    b + floor((4096 − b)·62/160 + 0.5)` for `b = 1800 + 10·f`: 2690,
    2696, 2702, 2708, 2714, 2720, 2726, 2733 for `f` 0..7, all
    `floor(adc/128) = 21`.
  - Verdicts: positive PASS when nothing fails; `--negative` PASS only
    when the failed set is exactly `spectrum-bin` (so `spectrum-line`,
    `spectrum-adc`, `spectrum-status`, `spectrum-trace` and
    `spectrum-arrival` passed), with the line `DETECTOR-SMOKE (spectrum
    negative): PASS (failed exactly as planned: the unaligned keV line
    arrived at ebin 21, not 12; everything else passed)`.
- [ ] Known first-run risks (investigate, do not re-run, lesson 6):
  `--expect-tape-flush` wants a non-zero byte in SSID 30/31/32/33 —
  with only ebins 12 and 21 lit, SSID 33 (the FSW's variance energy
  mask) or SSID 31 could be zero by design; the gate logs
  `ql.check.log` and the reviewer decides from the FSW's masks.
- [ ] `--dry-run --spectrum` prints the QEMU and probe lines (the 96
  upload sends and the shifted times visible); the reviewer runs both
  legs.
- [ ] Commit.

**gates2 legs** (`gates2.sh` is outside the repo; UNVERIFIED form —
copy the existing `detector-smoke --tape` / `--tape --negative`
entries and change only the leg name, the flags and the expected
line):

```
add_leg det-spectrum     "scripts/stix/detector-smoke.sh build/qemu-system-sparc --spectrum"            "DETECTOR-SMOKE (spectrum): PASS"
add_leg det-spectrum-neg "scripts/stix/detector-smoke.sh build/qemu-system-sparc --spectrum --negative" "DETECTOR-SMOKE (spectrum negative): PASS"
```

Each leg ≈ 5.5 min (the default 240 s + 85 s of upload), one slot,
`STIX_PORT_BASE=auto` as the other legs; same group as `det-tape`.

**Verification:** `python3 scripts/stix/ql-check.py --self-test` → PASS;
`STIX_PORT_BASE=auto scripts/stix/detector-smoke.sh
build/qemu-system-sparc --spectrum` → `DETECTOR-SMOKE (spectrum):
PASS`; `--spectrum --negative` → `DETECTOR-SMOKE (spectrum negative):
PASS`; SKIP 77 without the ELF.

## Task U6: accepted-diffs and docs

**Files:** modify `docs/stix-port/accepted-diffs.md`,
`docs/stix-port/ground-station.md`, `docs/stix-port/platform.md` (if
U3 did not cover it).

- [ ] New **§57 "Detector stimulus S2: spectra (baseline: the quiet
  template only)"**: the spec shapes and the rule table (U1); the CSV
  format (D4) and its limits; the keV law (D5) with the half-up
  rounding and the ADC-overflow drop; the per-spec row (approach 1)
  and its pickup points — step start and QL swap, the closed bank
  keeps the old binning (the replay's bank-1 precedent, unlike §56);
  cost; spectrum counts are ELUT-binned once and never pass §56's
  fold; the RNG stream changes only for banks with spectrum exposure;
  `align` (step overrides tape, keV specs only, degenerate rows
  no-op), the preimage miss (dropped, `preimage_miss` gauge); the pix
  gate (with the FSW only pix 0, 3, 6, 7, 8 count — FSW
  configuration); the platform's stricter file rule; the gate record
  (both legs, the measured peak bytes from the first live run).
- [ ] Amend **§54** (one paragraph): `spec` values other than `"flat"`
  are now accepted on rate steps (§57); per-step `align` is a known
  field; only `single` is refused ("unsupported in this build (S3)").
- [ ] Amend **§55** (one paragraph): piece (a) is consumed by §57;
  `det-align-status` gains `preimage_miss=N` (the line format grows by
  one trailing key); the classification also runs while a spectrum
  step plays (without `det-align`, so `mode=off` there).
- [ ] Amend **§34**'s "No ELUT" bullet (as §55 already does): "…and the
  tape's spectrum steps of §57, which always bin through the loaded
  ELUT".
- [ ] `ground-station.md` "Detector scene": the spectrum recipe
  (a `line_kev` step with `align`, the spectra dir, reading
  `preimage_miss`).
- [ ] Commit.

## Merge order and gates

U1 → U2 → (U3, U4 parallel; U4 needs U3's listing field) → U5 → U6.
Each model task: new qtests RED on its parent, the suite green after;
U3/U4: self-test PASS; U5: both legs PASS plus a re-run of the
existing `align-smoke.sh` (A3 + `--adc`) and `detector-smoke.sh
--tape` legs (their status substrings and tape schema must still
hold). UART goldens unchanged (nothing new at boot).

## Open questions for the owner

1. **Where the S2 gate lives.** (a) `detector-smoke.sh --spectrum` with
   align-smoke's upload helpers copied (≈30 lines; the D-3 base checks
   come for free; the outline's choice); (b) the same, but the helpers
   moved to `scripts/stix/lib/elut-legs.sh` and sourced by both (one
   copy, but touches a validated gate); (c) `align-smoke.sh
   --spectrum` (all machinery there, but none of detector-smoke's
   product checks). **Recommendation: (a)** now; (b) as a later
   clean-up if a third gate needs the chain.
2. **The gate's ELUT.** The outline says "on the A3-derived flash",
   but on a derived (aligned) ELUT `align` changes nothing, so the
   align-off negative could not fail. (a) Upload C3's base-0 wrong
   ELUT while the calibration is `1800 + 10·d` (this plan); (b) the
   derived ELUT with no negative (weaker: `align` unproven end to
   end); (c) a real flight ELUT CSV (not in the repo; D-Ed).
   **Recommendation: (a).**
3. **What a preimage miss does with the mass.** (a) Drop it and count
   `preimage_miss` (this plan: the intended bin is unreachable, and the
   loss is visible); (b) fall back to the unaligned calibration adc
   (counts appear, in the wrong bin); (c) use the nearest ebin that has
   a preimage. **Recommendation: (a)** — (b)/(c) hide exactly the
   misalignment the option exists to expose.
4. **Spectrum files on the platform.** (a) Bare names in
   `<tapes_dir>/spectra/`, dropped there by hand, listed in `GET
   /api/v1/tapes` and resolved to absolute paths at play (this plan);
   (b) also an upload verb from the UI (new verb, file-size handling);
   (c) points inline in the tape JSON (no file; the 64 KiB tape cap
   bounds it, but D4 fixed the file form). **Recommendation: (a)**,
   (b) later if wanted.
5. **ELUT pickup inside a running step.** (a) The row is read at the
   step start and each QL swap; the bank closed by the first swap
   after a rewrite keeps the old binning (replay precedent; simple,
   bounded cost); (b) C1-like: accumulate in adc space and fold at the
   draw so that bank already uses the new ELUT (needs a per-step
   exposure flush at mid-period step changes). **Recommendation:
   (a)**; TC(237,8) during a spectrum step is not a planned use.
6. **The `"flat"` name.** S1 fixed `"flat"` = the quiet template
   (lines at 18/19/27), which reads oddly next to `flat_kev`. (a) Keep
   the wire name and label it "template (flat)" in the UI (this plan);
   (b) add an alias `"template"` (additive) and keep `"flat"`.
   **Recommendation: (a)**; (b) only if users trip over it.
