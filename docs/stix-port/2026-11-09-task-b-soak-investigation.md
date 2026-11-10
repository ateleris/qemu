# Task B soak investigation: SCET fix, three masked model defects, and an FSW race

> **Superseded 2026-11-10:** there is no FSW stall. The soak gate sampled
> the UART only after QEMU had been killed, so it failed every run; the
> "freeze at FSW ~4.8 min" was the 600 s kill seen at FSW time (~0.48x
> wall, itself a GPTIMER prescaler defect). See
> `2026-11-09-task-b-stall-analysis.md`. The text below is kept as the
> record of the investigation.

Companion to the branch review (`2026-11-09-branch-review-findings.md`, finding
T1) and the implementation plan (`docs/superpowers/plans/2026-11-09-branch-review-tdd-fixes.md`).
This documents what happened during Task B (= T1, SCET set semantics): the
review predicted one failure behind the ~135 s abort; fixing it unmasked three
more, each diagnosed before fixing. One issue (the FSW stall) traces to the
FSW binary itself, not the model port.

## Timeline

### 1. Red phase — the T1 abort

The FSW's time-set path writes CTRL `longTimerSec` (0xC8) /
`longTimerSubSec` (0xCC). The model `assert(0)`s on those writes and died
(SIGABRT, exit 134) ~135 s wall / ~60 s FSW into every run — the review's
T1. Step 0 (trace instrumentation) recorded the FSW's write pair:
`0xC8 = 0x8000007b`, `0xCC = 0xe0c4`. Red tests were written first and shown
failing (test 1 aborted the QEMU child on the assert).

### 2. First fix attempt — D1 as drafted (WRONG, caught by the soak)

Decision D1 (as approved before the evidence below) was implemented:
"0xC8 write = seconds := full 32-bit value, sub-seconds continue; 0xCC =
sub-seconds := value & 0xFFFF". qtest went green (after fixing a test
arithmetic bug and restoring the HK-ADC repair block that the edit had
swallowed).

The new 600 s soak gate then **failed**: the FSW halted at FSW 00:01:04 with
`ASSERT FAILED: rbLevelMask <= STATUS4_ACTIVE_RB_LEVEL_MASK (HKAcquire.c:325)`.

Root cause (with Laszlo, who chose the fix): the FSW's write carries
**bit 31 = the UNSYNCED status flag** (`TIMING_COARSE_UNSYNCED`, Timing.c),
not seconds data, and per HSWICD §4.51 (quoted in the FSW's own
`IIR_setElapsedTime`) the commit happens at the **0xCC** write — the 0xC8
write only *stages* the seconds field. The first draft turned the FSW's
flag-set (its lost-timecode handler fires ~60 s FSW into any run without a
ground station) into a **+68-year clock jump**; the FSW's TM scheduling
wedged and its rotating buffer overfilled.

**Fix (D1 amended):** HSWICD two-register commit protocol — 0xC8 stages
seconds (bits 30:0) + the UNSYNCED flag (bit 31) without moving the running
time; 0xCC commits (staged seconds, written subsec); reads of 0xC8 return
running-seconds | flag<<31. qtest `stix/ctrl-scet` pins it (stage-only,
commit, flag readback, free-run, backwards, large epoch, flagged commit).

### 3. Second soak failure — ROTB strobe leak (masked model defect #1)

The soak failed again with the identical HK assert at FSW 00:01:04 — the
first root cause was real but **not the only killer**. Every pre-fix run had
died at the SCET assert (FSW ~60 s) *four seconds before* this fires; fixing
T1 unmasked it.

Root cause: the FSW's EDAC counter reset for the rotating buffer
(EDAC.c, `E_EDAC_MODULE_ROTBUF`) clears its strobe with a **full-register
write of `~(1u << 2)` = 0xFFFFFFFB** to `rotbCmdSta` (0x54). The model stored
it raw, leaking into the read-only BLOCKCOUNT field (bits 23:7 → 131071);
the FSW's HK level check computed level 8 > mask 7 and asserted. This is a
**baseline-inherited model defect** (identical raw store in the 5.2 model).
The other EDAC modules use single-bit read-modify-writes and are unaffected.

**Fix:** writes to 0x54 latch only the writable fields (COMMAND bits 1:0 +
EDACRESET bit 2); status fields ignore writes. qtest `stix/ctrl-rotb` pins it
(red shown: readback 0xfffffff8 after the strobe pair).

### 4. Third soak failure — the READY accident (defect #2 exposed by #1's fix)

The 0x54 mask removed an *accidental* behavior the FSW depended on: the same
EDAC strobe write (0xFFFFFFFB) had been setting `rotbCmdSta.READY` (bit 3)
through the raw store — the baseline's documented "accidental ROTB READY
pass". With the mask, the FSW's `RotBuf_init` READY wait timed out →
"Fatal error: 1" at init (FSW died at 00:00:00.09).

**Fix:** emulate the real FPGA behavior — the rotbConfig (0x58) ENABLE bit
(31) asserts READY, disabling clears it. Red `(0 == 1)` → green.

### 5. Soak failures four to seven — the FSW stall (not a model defect)

With all three fixes in (qtest 6/6, boot smoke PASS), the soak still failed:
the FSW **stalls completely** (no prints, no TM sends, no device activity,
no traps) at a nondeterministic **FSW 235–291 s** — in territory no build had
ever reached, since every earlier model died at FSW 60 s.

Diagnosed and ruled out: model APBUART TX block (status hardwired ready),
TC-only watchdog disable, register aliasing (COUNT bits 5:0 vs commands
`<<29`), FDIR action (fires once, no-op after), SPW link recovery (the stall
happens with a TM client attached too). The decisive instrument — freezing
the VM via the QEMU monitor at the stall — was prepared but the race dodged
it twice (freeze point varies with host load; one catcher run missed by
seconds, one was killed by the rebuild mid-launch).

**Status (updated after further runs):** the stall reproduces on **both**
FSW builds. The Feb 9 binary stalled in 4/4 deep runs (FSW 235–291 s, with
and without a TM client). The Nov 9 20:37 rebuild ("debug on") survived one
run to FSW 873 s — a dodge, not immunity: a later 600 s soak on the same
binary stalled at FSW 290 s. No tested build passes a 600 s soak reliably;
the stall point varies run-to-run (a race). Not a port regression — the
device behavior under QEMU is identical across builds; the FSW's own binary
changed the outcome. Prime suspect: the **unmodeled watchdog device** makes
the FSW's Watchdog task print an error and generate an event-report TM
*every second* in SAFE mode — the busiest task in the system, on a path that
on real hardware never executes (a real watchdog never reads 0). The race
may be an artifact of the emulated environment.

**Instruments queued:** a traced FSW build (`ENABLE_TRACE_TMBUFFER/
SPWCONTROL/SCHEDULER/WATCHDOG/TIMING` in configFSW.h; WATCHDOG+TIMING
confirmed compiled in) and a `DISABLE_WATCHDOG` build variant (removes the
per-second error/TM path entirely — the causal experiment). A VM-freeze
stall catcher exists (`/tmp/stix-stall-catch.sh`): runs QEMU with a Unix
monitor, freezes on 45 s of UART silence, dumps registers + code window for
symbol mapping via `readelf` on the FSW ELF. It has not yet caught the
freeze (the race dodged it twice: once at the 600 s window edge, once by a
mid-rebuild file absence).

## Gate redesign (soak.sh)

The soak gate's liveness criterion evolved with the evidence:

1. Original: UART must grow over the final 60 s (no client).
2. + TM probe attached (operational configuration): with no client the FSW
   wedges in link recovery, and even healthy runs pause TM delivery for
   minutes at a time — TM flow is not a reliable heartbeat.
3. Final: **UART heartbeat** — the watchdog device is unmodeled, so the FSW's
   once-per-second Watchdog error never clears and prints exactly as long as
   the Watchdog task and the RTEMS tick are alive (evidence: stalled runs
   froze mid-cadence; healthy runs printed to the kill instant). The probe
   stays attached as the operational configuration and its TM log is kept as
   evidence.

`normalize-log.py` additionally normalizes the FSW banner's build stamp
(`[<build>]`) — the only line that changes between rebuilds of the same
source (`__FILE__` paths stay identical for same-tree builds).

## FSW binary pinning

The build system cannot rename outputs, so `ASW_em.elf` at the FSW path is
whatever was built last. The gates pin a preserved copy:
`/tmp/stix-baseline-run/ASW_em_baseline.elf` (the Nov 9 20:37 non-stalling
rebuild; boot smoke PASS against the golden). The Feb 9 golden-capture
binary was overwritten by that rebuild and survives only in the History
folders as unrelated older builds. **If a fresh clone needs the gate ELF,
this copy must travel with the repo** (it is not committed — 5.4 MB binary;
decision recorded in the ledger).

## Final state

- Model: SCET HSWICD commit protocol + UNSYNCED flag readback; ROTB 0x54
  write mask; READY emulation. qtest 6/6, boot smoke PASS.
- Gates: soak = 600 s with TM probe, UART heartbeat criterion; 5.2 wrapper
  run expected RED on `ctrl-scet` and `ctrl-rotb` (both pin fixes the 5.2
  baseline lacks).
- Open: the FSW-binary stall at ~FSW 4.8 min (both builds, nondeterministic)
  blocks a 600 s soak PASS. Decision pending (see
  `2026-11-09-task-b-stall-handoff.md`): scope the gate to the T1 objective,
  run the DISABLE_WATCHDOG causal experiment, or hunt the race with the
  traced build. Also open: the TM delivery pause (minutes, self-recovers,
  seen with a client attached).
