# Task B stall analysis: there is no FSW stall (gate artifact) + two model defects

Written 2026-11-09 as the answer to `2026-11-09-task-b-stall-handoff.md`
("FSW halts at a nondeterministic ~FSW 4.8 min"). Read-only recon of the FSW
tree; no FSW or working-tree changes.

## 1. Verdict

**The "stall" is a measurement artifact of `scripts/stix/soak.sh`, not FSW
behaviour.** The FSW was alive at the end of every run in the evidence
table. Options A/B/C in the handoff all assume a real stall, so none of them
is needed in the form described.

Two *real* model defects surfaced along the way. Both make the emulated
environment differ from hardware and are worth fixing (section 4).

## 2. Why the gate always fails

`soak.sh` (both the committed `03cb668cdf` version and the working-tree
version) runs QEMU **in the foreground**:

```bash
timeout "$SECONDS_TOTAL" "$QEMU_BIN" ... -serial file:uart.soak.raw ... || rc=$?
# ...only reached after QEMU has been killed...
sleep "$((SECONDS_TOTAL - 150))"     # working tree; committed: no sleep
SIZE_NOW=$(stat -c %s uart.soak.raw)
sleep 120                            # committed: sleep 60
SIZE_END=$(stat -c %s uart.soak.raw)
[ "$SIZE_END" -le "$SIZE_NOW" ] && FAIL "heartbeat stopped"
```

Both samples are taken **after** QEMU exited, on a closed file, so
`SIZE_END == SIZE_NOW` always holds and the gate cannot pass. The same holds
for the earlier TM-flow variant of the gate (also sampled after exit).

The "freeze point" is simply where `timeout` killed QEMU, expressed in FSW
time:

| Run | Wall window | Last FSW timestamp | FSW / wall |
|---|---|---|---|
| latest soak (working tree) | 600 s | 00:04:50 (290 s) | 0.483 |
| stall-catcher (`/tmp/stix-stall-catch.sh`, 1800 s timeout, never froze) | 1800 s | 00:14:33 (873 s) | 0.485 |
| evidence-table runs 1, 3, 4, 5, 7 | 600 s | 287–291 s | ~0.48 |

The "nondeterministic 235–291 s" spread is host load changing how much FSW
time fits into 600 wall seconds. "Model stdout ends within seconds of the
freeze" and "QEMU keeps running until killed" are both what a kill at the
window edge looks like. In the latest soak, `uart.soak.raw`,
`qemu-stdout.soak.log` and `tm.soak-probe.log` all have the same mtime
(21:48:47): the UART was being written in the last second of the run.

**Direct demonstration:** `soak.sh <qemu> 160` (window ends at ~FSW 75 s,
long before any claimed stall) — see section 6 for the result.

## 3. The ~0.48 ratio is itself a model defect (GPTIMER prescaler)

FSW uptime advances at ~half the speed of the QEMU virtual clock:

- The FSW expects the GPTIMER prescaler to be programmed by the boot PROM:
  `BSW_ASW/src/BSW/include/IIR.h:64`
  `#define IIR_GPT_PRESCALER 19  /*GJ it was 40*/`, which means
  20 MHz / (19+1) = 1 MHz timer tick. No FSW code writes the prescaler
  (`IIR_initSystemClocks` only clears timer 1 control).
- QEMU's PROM stub writes **39** (`hw/sparc/leon3.c:132`,
  `gen_store_u32(p, 0x80000304, 39)`). That is correct for upstream's
  40 MHz, but the port set `CPU_CLK` to 20 MHz (`leon3.c:54`) and kept the
  stub's 39. The timer tick is 0.5 MHz, so RTEMS uptime runs at 0.5×.
- `accepted-diffs.md` §1 says "the RTEMS LEON3 BSP reprograms the prescaler
  at boot". **This is wrong** for this FSW (measured ratio 0.48; no write in
  the source).

Why it matters beyond cosmetics: the FSW uses **two clocks**.
`Timing_getMainTime()` is RTEMS uptime (`Timing.c:330`), and SCET is read
from the CTRL long timer, which the model advances at the QEMU virtual rate.
On hardware both run at the same rate; under the model SCET runs 2× faster
than uptime. Everything that mixes them sees a time base no real IDPU
produces: time sync, lost-timecode handling (the ~60 s timer that triggered
the T1 SCET write), TM scheduling, and HK/TM cadences (the golden "37 B HK
every ~2 s" is very likely a 1 s FSW period at half speed). It also halves
the FSW depth reached in any wall-clock window.

## 4. What can safely be made more robust / mocked

Ranked by value and how safely each can be made. "Safe" means the change
moves the model *toward* documented hardware behaviour (FSW source or ICD as
reference), is pinned by a qtest, and any visible change is recorded in
`accepted-diffs.md`.

### R1 — Fix `soak.sh` (gate; blocks Task B) — safe, no model change

- Run QEMU in the background (`timeout ... & QPID=$!`), take the heartbeat
  samples **while it runs** (at T-150 and T-30), then `wait $QPID` and apply
  the rc check (124 = normal end).
- Fail if QEMU is not alive at the second sample.
- Remove the duplicated final "SOAK: PASS" echo (the second one mislabels
  UART bytes as "TM flowed ... probe-log bytes").
- Update the script header: the "no-client link recovery wedges the FSW"
  rationale came from the misread runs; it is unproven.
- **Red test first:** extend `scripts/stix/tests/` with a fake QEMU
  (`fake-qemu.sh` mode `heartbeat`: appends a line to the `-serial file:`
  every second until killed). Today `soak.sh fake 160` reports FAIL, which
  is the red. After the fix: PASS. Add a mode `freeze` (prints for 20 s, then
  sleeps), expected FAIL with exit 1.

### R2 — GPTIMER prescaler from `CPU_CLK` — safe, one line, high value

- `leon3.c:132`: `gen_store_u32(p, 0x80000304, CPU_CLK / 1000000 - 1)`,
  which gives 19 at 20 MHz. This matches `IIR_GPT_PRESCALER` and the real
  boot PROM.
- **Red test:** qtest is not enough (the stub only runs on the `-kernel`
  path). Use the boot smoke: assert that the FSW timestamp of the N-th
  watchdog line ≈ N s of wall time ±10 %, or a qtest-free script test that
  boots the ELF for 30 s wall time and checks that the last UART timestamp
  is ≥ 25 s. Today: ~14 s (red).
- Consequences to record in `accepted-diffs.md`: FSW cadences double in wall
  terms; the golden boot sequence (timestamps stripped by `normalize-log.py`)
  should be unchanged. Verify. `tm.golden.log` cadence changes; regenerate
  it. Fix the wrong §1 sentence.

### R3 — Model the STIXCTRL watchdog (0xD4) — safe if pinned, removes the per-second error path

Semantics fully specified by the FSW (`BSW_ASW/src/BSW/WDT/WDT.c` header
comment + functions, bit fields in `StixIdpu.h`):

| Field | Bits | Model behaviour |
|---|---|---|
| COMMAND | 31:29 | write-only action |
| RELOADVAL | 16:11 | latched by CHANGE (7) |
| COUNT | 5:0 | read: cycles to reset |

| Command | Value | Effect |
|---|---|---|
| RELOAD | 1 | COUNT := RELOADVAL; **also enables counting** (WDT.c comment) |
| FORCE / FORCE1 / FORCE2 | 2/3/4 | IDPU reset (`qemu_system_reset_request`) |
| DISABLE | 5 | stop counting (COUNT frozen) |
| ENABLE | 6 | resume counting |
| CHANGE | 7 | RELOADVAL := bits 16:11; does not touch COUNT |
| SAFE | 0 | no-op |

Counting: COUNT decrements once per 105 ms (`WATCHDOG_CYCLE_DURATION`,
Watchdog.c) on the virtual clock; at 0 with counting enabled, request a
reset.

Why it is safe and useful:
- After the FSW's `RELOAD`, COUNT reads back 63, so `Watchdog_feed()`'s check
  at Watchdog.c:197 passes. The per-second `*E invalid number of cycles`
  print and the 4 s FDIR path disappear. That path never runs on hardware,
  and today it is the busiest code path in SAFE mode.
- A real hang now becomes a **reset**. With `-no-reboot`, QEMU exits before
  the time limit and the soak reports it as a crash, instead of an ambiguous
  silent UART. This makes every future liveness gate stronger.
- Note: the FSW ELFs under test are built with `-DDISABLE_WATCHDOG`
  (verified: the `"incorrect cycles remaining"` string is absent from both
  `ASW_em_baseline.elf` and `ASW_em.elf`). Yet `Watchdog_feed()`'s RELOAD
  re-enables counting on hardware semantics, so the mock will count. That is
  correct hardware behaviour, and the FSW feeds every 1 s against a 6.6 s
  timeout.
- Open point: the WDT.c comment gives 3.28 ms/unit ("0..1023 → 3.28 ms to
  3.35 s") while the FSW uses 105 ms/cycle with a 6-bit field. Use the FSW's
  105 ms (it is what the FSW's own check assumes) and note the discrepancy.

Red tests (qtest `/sparc/stix/ctrl-watchdog`):
1. CHANGE(63), then RELOAD: COUNT reads 63 (today: 0, red).
2. Step 1 s: COUNT = 63 − 9 (±1).
3. DISABLE, step 1 s: COUNT unchanged.
4. ENABLE / RELOAD resume counting.
5. CHANGE does not alter the current COUNT.
6. Counting to 0 produces a reset: under qtest, listen for the QMP `RESET`
   event, or `SHUTDOWN` with `-no-reboot`.
7. FORCE produces the same reset.

Consequences: the steady-state UART becomes quiet (no per-second line), and
the golden boot sequence loses the `00:00:00.018 Watchdog *E` line, so it
must be regenerated. **The soak heartbeat must then move off the UART** (R4).
Do R3 after R1 and R2.

### R4 — A heartbeat that does not depend on an error print

After R3, the UART no longer prints every second. Choose one:
- TM flow with the probe attached (HK TM is periodic). Gate on "≥ 1 HK
  packet in each 60 s slice of the last 3 min". Record the TM pause
  observation first (below).
- Model-side: a trace event or QMP query counting CTRL HK-ADC accesses
  (the HK task polls periodically).

### R5 — Already planned in the review (unchanged, still valid)

T3 (lazy flash file / qtest hermeticity), T4, T5 (flash addressing), T6
(trace events, non-blocking TM send, socket races). None of them relate to
the "stall".

## 5. Recommendation on A / B / C

- **Do R1 first, then re-run the 600 s soak.** Expected result: PASS with the
  current tree (the FSW is alive to the kill in every log examined). This
  unblocks the Task B split **without** re-scoping the gate (A is
  unnecessary) and without an FSW rebuild (B).
- **Do not run B as designed.** Both ELFs are *already* `DISABLE_WATCHDOG`
  builds, and in `Watchdog.c` that ifdef only removes the first check
  (`incorrect cycles remaining`, line ~185). The per-second print at line
  199 and the FDIR path are outside the ifdef. The experiment would not
  remove the suspected path and would not test the hypothesis.
- **Do not hunt the race (C).** There is no race to catch: the catcher ran
  1800 s without a freeze because the FSW never froze. More traced builds
  and catcher runs would spend hours on the misreading.
- **Do not narrow the gate to "survives past FSW 70 s" (A).** That would
  ship a gate that cannot detect a steady-state death and would encode a
  non-existent FSW bug in the docs.
- **Correct the record:** the handoff, the investigation narrative (§5,
  "Gate redesign"), the ledger entries for soak runs 4–7, and the `soak.sh`
  header attribute a wedge to the FSW / SPW link recovery. Mark these as
  superseded by this analysis. Do not rewrite them; history stays readable.
- The "TM pauses 2–6 min then resumes" observation is **unverified** and may
  partly be the same artifact: the probe keeps logging for +10 s after QEMU
  is killed. Re-check it with the fixed gate before investigating.
- R2 and R3 are real fidelity fixes. Do them as their own TDD tasks after
  Task B is committed (R3 touches `grlib_stixctrl.c`, the same file as the
  Task B split, so it must not go in before the split lands).

## 6. Demonstration result

Run 2026-11-09 23:00, working-tree `soak.sh`, current build,
`STIX_ASW_ELF=/tmp/stix-baseline-run/ASW_em_baseline.elf`, window 160 s:

```
SOAK: FAIL — UART heartbeat stopped in the final ~2 min (16195 -> 16195 bytes)
UART tail: ... 00:01:17.250  Watchdog *E invalid number of cycles ...
real 4m48.956s   exit=1
```

- The script ran 289 s for a 160 s window: it slept 10 s + 120 s **after**
  QEMU was killed. During those sleeps, `pgrep` showed `soak.sh` alive and
  no `qemu-system-sparc` process.
- The UART mtime (23:01:31) equals the kill instant, and the last FSW
  timestamp is 77 s (ratio 0.48). The FSW printed every second up to the kill.
- FSW 77 s is far before any claimed stall point (235–291 s). The "FAIL"
  is therefore produced by the gate alone. **The gate cannot pass for any
  window length.**
