# Platform timeline view — design (2026-11-14)

Status: approved by the owner 2026-11-14 ("go ahead with all that you can"); plan `2026-11-14-timeline-plan.md`. Builds on the platform controller
(`docs/stix-port/platform.md`, plan `2026-11-11-platform-plan.md`) and
its PL-11 surfacing work.

## Intent

The owner wants one place to watch the emulated STIX system over time
and pair cause with effect: "when I inject an error, does the rotating
buffer fill up faster?" Today the facts exist but are scattered: the
controller's state tree (truth), the TM the FSW sends (what STIX
reports), the record log (what the operator and the scenarios did), and
for the ROTB experiments separate files (`fill.csv`, `tm.log`). The
ground station has its own HK dashboard; this view does not replace it.
What is new here is truth, telemetry and actions on **one clock** in
**one plot stack**.

## Goals

1. A **Timeline** tab in the platform web UI that plots chosen series
   over time, live (streaming) or over a window (last 1/10/60 min, or
   the whole run).
2. Series from three sources, all on the bench clock (`sim_time`):
   - **truth**: any state-tree leaf with a numeric value, including the
     volatile ones (ROTB count, rd/wr), via a sampler;
   - **reported**: HK SID 1 and SID 2 fields decoded with the IDB JSON
     (CPU load, archive memory usage, ROTB fill level, trigger median /
     max, mode, power status bits, …), and any other numeric HK field
     by name;
   - **markers**: fault inject/clear, forcings, TCs sent (service,
     subtype, and for TC(237,18) the parameter index and value),
     TM(5,x) events (EID and name), mode changes, power/boot, scenario
     steps. Markers are vertical lines across every plot, with a hover
     label.
3. **Views**: a named JSON file listing the panels (which series in
   which panel), the window, and which marker kinds to show. The page
   lists the saved views, loads one, lets the owner change it, and saves
   it under a name. A handful of starter views ship with the platform.
4. **"Show me everything"**: a view that is generated, one panel per
   leaf group, every marker kind on.
5. **Past runs**: the controller can open a finished run's `log.jsonl`
   read-only and serve it through the same page, so a run can be looked
   at later with the same views.

## Non-goals

- Real hardware (AX/SIIS) data. Emulator only for now (owner,
  2026-11-14).
- Replacing the ground station's HK pages or its archive.
- Comparing two runs in one plot (a later step; the data format allows
  it).
- Per-partition archive fill. HK reports one archive usage value
  (`NIXD0003`); per-partition truth would need a flash filesystem
  reader in the controller. Listed under "later".

## Anti-goals (failure even if it works)

- The page becomes a source of truth: something visible only in the
  browser, lost on reload. Everything plotted comes from the log or the
  state API; views live on the server.
- The sampler slows the guest or floods the log so that runs change
  behaviour or the run dir grows without bound.
- A chart library from a CDN, `innerHTML`, inline handlers: the page's
  CSP and self-test rules stay as they are.
- Breaking API v1: additions only.

## Constraints (from the existing platform)

- `index.html` is one file, plain JS, no build, no CDN, strict CSP
  (script/style by hash, `connect-src 'self'`). Plots are drawn on
  `<canvas>` (or inline SVG) by our own code.
- The controller polls the hw-state over QMP; under `-icount` the guest
  clock does not advance during a QMP round trip, so a sampler does not
  perturb guest time, but it costs wall CPU.
- Change records skip volatile leaves (`state.py` `VOLATILE`); the
  10 s snapshots are too coarse for a fill curve.
- The controller's decoder is deliberately small (D-PLa: operator
  decoding belongs to the GS). PL-8 already planned an IDB-driven
  decoder for truth-vs-telemetry; this design brings its decoding part
  forward and uses it for the plots. The IDB JSON is the GS's
  `definitions/telemetry/stix_idb_tlm.json` (optional `/idb` mount
  in the container; a config key natively).

## Approaches considered

- **A. Timeline tab inside the platform UI (chosen by the owner).**
  Live, same clock as the faults and TCs, nothing duplicated. Costs: a
  sampler, the IDB decoder, hand-written plots.
- B. A separate offline viewer over run dirs. Not live; duplicates the
  platform's data knowledge.
- C. Generated end-of-run reports (PNG/HTML). Cheapest, not live, not
  interactive; a small form exists (`fill.png`).

## Design

### Data: one series model

A **series** is `(id, label, unit, points[(sim_time_ns, value)])`. Ids:

- `truth:<leaf path>` and `presented:<leaf path>` — state leaves;
- `hk:<sid>:<field name>` — a decoded HK field, the IDB field name
  (for example `hk:2:RotBuff fill level (NIXD0501)`); a short alias
  table gives the common ones readable ids (`hk.cpu_load`,
  `hk.rb_level`, `hk.trig_med`, `hk.trig_max`, `hk.archive_usage`);
- marker kinds: `fault`, `force`, `tc`, `event`, `mode`, `power`,
  `scenario`.

### Controller additions

1. **Sampler.** Config `[timeline] sample_s` (default 1.0, 0 = off)
   and `sample_paths` (glob list, default the volatile numeric leaves
   plus `instrument.rotb.*`). Each tick logs **one** record
   `kind = "sample"`, `path = ""`, `data = {path: value, ...}`. Topic
   `sample`. Size: ~20 leaves × 1/s ≈ 3 MB/h of JSONL; the run dir's
   retention rules apply.
2. **HK decode.** `decode.py` gains an IDB-driven field decoder for
   TM(3,25) SID 1/2 (byte offset, type, bit offset/width from the
   JSON). The `tm` record of an HK unit carries `data.hk = {field:
   value}`. Without the IDB JSON the old minimal decode stays and the
   HK series are absent (the page says so).
3. **TC labels.** The `tc` records already exist; the param index /
   value of TC(237,18) is added to their `data` when the controller
   builds or relays it.
4. **API (additive, v1):**
   - `GET /api/v1/series?ids=a,b&from=<ns>&to=<ns>&max_points=N` —
     points from the record ring / log, decimated server-side
     (min/max per bucket, so spikes survive);
   - `GET /api/v1/series/catalogue` — every series id available now
     (leaves, decoded HK fields, marker kinds) with unit and source;
   - `GET /api/v1/views`, `GET /api/v1/views/<name>`,
     `POST /api/v1/cmd/view.save {name, view}` and `view.delete` —
     views as JSON files in `[timeline] views_dir` (default
     `<run root>/views`, starter views copied in at first start from
     `scripts/stix/platform/views/`);
   - live updates: the existing SSE stream with topics
     `sample,tm,fault,cmd,state`.
5. **Past runs.** `stixplat.py --view-run <run dir>`: a read-only
   controller (no QEMU, every `cmd` verb refused except `view.*`) that
   serves that run's `log.jsonl` through the same API and page.

### Page

- A tab bar: the existing panels, and **Timeline**.
- Timeline: a view picker (saved views + "everything"), a window picker
  (live / 1 / 10 / 60 min / whole run), a series picker fed by
  `/series/catalogue` (search by name), panels stacked with a shared
  time axis, markers across all panels, hover readout of every series
  at the cursor time, zoom by drag, "save view as".
- Plots on canvas, step lines for discrete values (levels, mode), lines
  for continuous ones, a second y-axis per panel when units differ.

### Starter views

- **ROTB fill vs faults:** `truth:instrument.rotb.count`,
  `hk.rb_level`, `hk.cpu_load`, `truth:instrument.detector.flare_cps`;
  markers fault, tc, event.
- **CPU and HK:** `hk.cpu_load`, `hk.archive_usage`, `hk.trig_med`,
  `hk.trig_max`, `hk.mode`; markers tc, event, mode.
- **Memory upsets:** `truth:instrument.memory.*` counters; markers
  fault, event.
- **Power and link:** supplies, SpW, time sync; markers power, fault.

### Tests

- Self-test (offline): sampler record shape and the VOLATILE leaves
  covered; the IDB decoder against a recorded real SID 2 unit (values
  cross-checked with `TM_3.c` packing, including bit fields such as
  the RB level at app byte 62 bits 7..5 — status4 bits 31..29);
  `/series` decimation keeps min/max; views round-trip; `--view-run`
  refuses control verbs; the page rules (CSP, no innerHTML, every
  `data-verb` served) still pass.
- Live smoke: a short run with the fake QEMU and one with the real
  ASW: the starter views load and every series has points.

## Later

- Per-partition archive fill from flash truth (needs a filesystem
  reader).
- Two-run overlay (same view on two run dirs, aligned at NOMINAL).
- ROTB/calibration runs as platform scenarios, so they appear here
  without the separate driver.
- Run bundles (user request on the mockups, 2026-11-14; its own design
  when started): a bundle = manifest (ELF + sha256, QEMU commit +
  argv, platform config, knob values, flash start state by reference)
  + every input stamped on guest time (TCs, operator commands, faults,
  forcings, stimulus steps) + outputs (TM, UART, samples) + the
  decisions derived from them (mode changes, TM(5,x) events, HK level
  steps, overruns/skips). Replay re-feeds the inputs on guest time with
  a chosen ELF/QEMU; QEMU's own record/replay is tied to one binary
  and cannot compare builds. Compare checks the decisions first, then
  numbers within tolerances, and marks the first divergence on the
  two-run overlay. log.jsonl and the scenarios are most of the input
  record already. Mockup: board 7 · Runs.

## Left to the builder

- Canvas vs SVG, the decimation bucket count, colours.
- Exact sampler default glob.
- How the record ring and the on-disk log split the work for long
  windows (the ring holds the last 2000 records today; long windows
  read `log.jsonl`).

## Calls made after the builder check (GLM, 2026-11-14)

All mine, for the owner to check; none changes what the view shows.

1. **IDB source.** Native default
   `~/dev/ATS-GroundStation-stix/definitions/telemetry/stix_idb_tlm.json`
   (config `[timeline] idb_json`; container: the `/idb` mount). Fields
   carry `byte_offset`, `data_type`, `bit_offset`/`bit_length`,
   sometimes `unit` and a `calibration` **id only** (for example CPU
   load: byte 51, bits 0..4, unit %, `CIX00041TM`; RB level: byte 62,
   bits 0..2, `CAAT0046TM`). The curves are not in the JSON: they come
   from the IDB sqlite (`[timeline] idb_sqlite`, optional); without it
   a series shows raw values and says "raw" in its unit.
2. **Series ids key on the parameter code**: `hk:2:NIXD0501`, label
   from the IDB name (names contain commas, so names never go in ids).
   The query form is `ids=` repeated, not comma-joined.
3. **Views live outside every run dir**: `<retention.run_root>/views`,
   shared by all runs, never touched by `run_dir.fresh`. Starter views
   are copied in only when missing (a deleted starter comes back at the
   next start; a changed one is kept). `--view-run` serves the same
   views dir.
4. **`--view-run DIR [--port N]`** opens the run's log read-only: no
   records appended, its own seq space, reads the rotated
   `log.jsonl.N.gz` parts too; `/stream` sends the history and then
   stays idle. Default port 8081, so it can run next to a live
   controller on 8080.
5. **Sampler default**: on (1 s) in interactive mode, off in run mode
   unless the scenario or config sets `[timeline] sample_s`; gate
   evidence logs stay as they are.
6. **Non-numeric leaves**: booleans plot as 0/1 steps; enum strings
   (link up/down, timesync verdict) as labelled steps (the value's
   index in the order seen, with the string in the hover). A `sample`
   record carries `truth`, and `presented` only where it differs.
7. **Live updates**: the page subscribes to every topic it draws
   (`sample, tm, tc, cmd, fault, state, expect, power`).
8. **TC(237,18) label**: ZIX37018 is PIX00016 index u16, PIX00017
   sub-id u8, PIX00018 value u32: the marker reads
   `param 312 = 0`.
