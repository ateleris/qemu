# Web UI: rest of the mockup (Track D) — plan (2026-11-14)

Owner go: "go ahead with all that you can" (2026-11-14), scope = the
mockup boards that need no emulator or FSW change. Mockup:
https://claude.ai/artifact/V9rbLbVVcfwFJjnoiKciqJ boards 2, 3, 5, 6.
Page rules as in `2026-11-14-timeline-plan.md` (one file, no innerHTML,
no inline handlers, no style=, textContent only, every data-verb
served). API v1 additions only; no new verbs needed.

Out of scope (owner questions): FSW parameter table (board 2d),
detector stimulus (2c), run bundles (7), the "Reported in TM" column
beyond what data.hk gives.

## D1 Faults & forcings (board 3)
- Catalogue grouped by category (the id's first segment: spw/timecode
  -> Link, supplies, attenuator, memory, flash; other -> Other) with
  filter chips "All N", "<Category> N"; table columns Fault, What it
  does (description), Parameters (inputs as today), State
  (idle/armed at T/active since T), Inject: "Now" and "At…".
- "At…": an input for guest seconds, absolute (`120`) or relative
  (`+30`), converted to `trigger {"at": "at_sim", "sim_time": ns}`
  using the controller's /time sim_time; armed faults show their time
  and can be cleared (disarm = fault.clear, as faults.py does).
- "Active now" list: active and armed faults with since/at time and
  params, Clear per row, "Clear all" (fault.clear for each, with one
  confirm).
- Forcings: list of active forcings with Clear per row (force.clear
  {path}); the set form stays.

## D2 Platform tab and header (board 2)
- "Faults here" box on the Platform tab: catalogue entries whose
  category is Link or Supplies, each with Now (fault.inject).
- A global Pause/Resume button in the header (sim.pause/sim.resume,
  the label follows status.time.running).

## D3 Logs (board 5)
- Record-type chips: tm, tc, fault, state, sample, heartbeat, uart,
  cmd, log (heartbeat = tm records whose decoded APID is 0x5E5; tm
  then excludes them); sample and heartbeat off by default.
- A text search over the rendered record lines (client side, case-
  insensitive, applies to new lines too).

## D4 State (board 6)
- Leaves grouped by parent path (collapsible groups, all open by
  default), columns Leaf, Truth, Presented to FSW, Reported in TM (the
  latest decoded HK value whose alias/code is mapped in a small table
  in the page: instrument.rotb.count -> hk.rb_level only; else "–"),
  Status, Changed (t_sim as guest s).
- Filters: "Only differences" (presented != truth), "Only faulted"
  (fault_ids non-empty or status != ok), plus the existing text filter.
- Click a numeric/bool/enum row: adds truth:<path> to the Timeline's
  expanded panel (or a new panel) and switches to the Timeline tab.

## Tests
- selftest page rules: ids exist, tabs, data-verbs served (existing);
  new case "UI: mockup port (D)": the faults catalogue container, the
  category chip container, the active list, the header pause button
  (data-verb sim.pause and sim.resume), the log search input and the
  state filters exist.
- Reviewer: browser pass on the real ASW (schedule a fault +10 s, see
  it armed then active, clear all; logs search; state only-differences
  with a forcing; click-to-timeline).
