# Handoff: STIX QEMU port — Task B complete, FSW stall blocks the soak gate

> **Superseded 2026-11-10:** there is no FSW stall. The soak gate sampled
> the UART only after QEMU had been killed, so it failed every run; the
> "freeze at FSW ~4.8 min" was the 600 s kill seen at FSW time (~0.48x
> wall, itself a GPTIMER prescaler defect). See
> `2026-11-09-task-b-stall-analysis.md`. The text below is kept as the
> record of the investigation.

Written 2026-11-09 for a fresh session taking over the analysis. Read this
with `2026-11-09-task-b-soak-investigation.md` (the narrative) and the SDD
ledger `.superpowers/sdd/2026-11-09-branch-review-tdd-fixes/progress.md`
(the raw run log).

## The situation in one paragraph

The STIX IDPU QEMU port (branch `stix-emul-qemu-v11.1`) is mid-way through
the branch-review TDD fix plan. Task B (= review finding T1, SCET set
semantics) is **functionally complete**: the model now implements the HSWICD
§4.51 two-register commit protocol with the UNSYNCED status flag, plus two
baseline-inherited model defects found along the way (ROTB strobe/status
leak, ROTB READY emulation). qtest suite 6/6, boot smoke PASS against the
golden. **Nothing is committed yet** — the working tree holds all of it,
prepared for a 4-commit split. The blocker: the 600 s soak gate cannot pass
reliably because the FSW **stalls completely at ~FSW 4.8 min**
(nondeterministic 235–291 s), on both FSW binaries tested, with and without
a TM client. The model work is provably correct; the stall looks
FSW-internal, probably triggered by the emulated environment (unmodeled
watchdog device → per-second error print + event TM, the busiest task in
SAFE mode, on a path real hardware never executes).

## Decision on the table (Laszlo has not chosen yet)

- **A. Scope the gate to the T1 objective** — soak asserts the FSW
  survives past FSW ~70 s (the old abort point) instead of 600 s liveness;
  document the stall as out of scope; commit Task B today.
- **B. DISABLE_WATCHDOG causal experiment** — one FSW rebuild without the
  per-second watchdog path; if the soak passes, the race is localized and
  the gate gets a stable configuration.
- **C. Hunt the race with the traced build** — instruments exist (see
  below); does not change the port's correctness.

## State of the working tree (branch stix-emul-qemu-v11.1, HEAD 03cb668cdf)

Uncommitted, planned as 4 commits:

1. `stix: implement SCET set semantics per HSWICD commit protocol (T1)` —
   `hw/sparc/grlib_stixctrl.c` (time_correction_ns, staged_secs/
   unsynced_flag, nsToSCET, 0xC8/0xCC commit protocol, UNSYNCED readback,
   reset), `hw/sparc/trace-events`, `tests/qtest/stix.c` (ctrl_sec/ctrl_flag
   helpers + test_ctrl_scet), `docs/stix-port/accepted-diffs.md` §4.
   **The tree is currently in exactly this state** (the ROTB content is
   temporarily removed for the split).
2. `stix: ROTB strobe/status semantics (EDACRESET mask + READY emulation)` —
   re-apply: grlib_stixctrl.c 0x54 write mask (`value &= 0x7`) + 0x58
   READY-from-ENABLE block; stix.c test_ctrl_rotb + registration;
   accepted-diffs §5. All exact edit strings are recoverable from
   `docs/stix-port/2026-11-09-task-b-soak-investigation.md` + the ledger.
3. `stix: soak gate on UART heartbeat with TM probe; normalize banner
   stamp` — `scripts/stix/soak.sh` (probe attached, UART heartbeat
   criterion, T-150/T-30 sampling), `scripts/stix/normalize-log.py`
   (banner `[<build>]` rule).
4. The two docs: `2026-11-09-task-b-soak-investigation.md` +
   `2026-11-09-task-b-stall-handoff.md` (this file).

Verification protocol per commit: rebuild (`ninja -C build
qemu-system-sparc tests/qtest/stix`), run the qtest suite from a scratch
dir (see below) — expect 5/5 for state 1, 6/6 for state 2. Full suite:
`cd /tmp/stix-qtest-scratch && QTEST_QEMU_BINARY=/home/laszlo/dev/qemu/build/qemu-system-sparc
/home/laszlo/dev/qemu/build/tests/qtest/stix --tap`.

## Evidence table (the stall)

| # | Config | Build | Outcome |
|---|--------|-------|---------|
| 1 | no client | Feb 9 v1 | stall, UART frozen at FSW 00:04:49 (289 s), 288 watchdog prints |
| 2 | no client | Feb 9 v1 | stall at FSW 00:01:04 (64 s) — this was the ROTB defect, fixed since |
| 3 | no client | v1 | stall at FSW 00:04:49 |
| 4 | probe attached | v1 | stall at FSW 00:04:50 (290 s) — client does NOT prevent it |
| 5 | probe attached | v1 | healthy to kill at FSW 291 s (dodged) |
| 6 | no client, monitor attached | v1 (20:37 rebuild v2) | healthy to FSW 873 s (dodged) |
| 7 | probe attached | v2 | stall at FSW 290 s — v2 does NOT dodge reliably |

Symptoms: ALL FSW activity stops (UART prints, TM sends, HK polls); no
trap, no assert, QEMU itself keeps running until killed; the guest CPU
stops touching devices (model stdout ends within seconds of the freeze);
freeze point varies 235–291 s FSW. TM flow (with client) pauses 2–6 min
near the window end even in healthy runs, then resumes — separate,
non-fatal observation.

Ruled out: model APBUART TX block (status hardwired TX-ready), TC-driven
watchdog disable (no ground station), register aliasing (watchdog COUNT is
bits 5:0, FSW commands write `<<29`), FDIR action (fires once, no-op),
SPW-link-recovery-only (stalls with a client too).

## Environment map

- Repo: `/home/laszlo/dev/qemu` (branch `stix-emul-qemu-v11.1`), build in
  `build/`, build env: `export PATH=/home/laszlo/venvs/qemu-build/bin:$PATH`.
- Run dir: `/tmp/stix-baseline-run/` — contains the **16.5 GB flash.bin
  (NEVER delete or recreate it — standing instruction from Laszlo)**, the
  golden artifacts, and the preserved gate ELF
  `ASW_em_baseline.elf` (the Nov 9 20:37 v2 rebuild; boot smoke PASS).
- FSW tree: `/mnt/c/Users/laszl/Development/stix_asw_src/` — **read-only,
  never modify** (Laszlo's rule; memory: stix_asw_src_readonly). The ELF at
  `ASW/ASW/bin/ASW_em.elf` is currently the TRACED build (v3, Nov 9 20:44,
  WATCHDOG+TIMING traces compiled in; the build system cannot rename
  outputs, so every rebuild overwrites that path). `ASW_em.map` travels
  with it. Older builds: `bin/History/`.
- FSW sources referenced in this investigation: `ASW/src/HK/HKAcquire.c`
  (rbLevel assert, line ~325), `ASW/src/SpWControl/SpwControl.c`
  (transmitPacket, link FDIR), `ASW/src/Timing/Timing.c`
  (TIMING_COARSE_UNSYNCED = bit 31, lost-timecode timer ~60 s),
  `BSW_ASW/src/BSW/WDT/WDT.c` + `ASW/src/HWControl/Watchdog.c`
  (feed cycle, FDIR delay 4000 ms, `DISABLE_WATCHDOG` ifdef),
  `BSW_ASW/src/BSW/EDAC/EDAC.c` (ROTB strobe, ~line 305),
  `BSW_ASW/src/BSW/Rotbuf/RotBuf.c`, `ASW/src/configFSW.h` (trace switches).
- Gate scripts: `scripts/stix/boot-smoke.sh`, `scripts/stix/soak.sh` (both
  accept `STIX_ASW_ELF`/`STIX_RUN_DIR`/`STIX_SMOKE_SECONDS`),
  `scripts/stix/normalize-log.py`, `scripts/stix/tmtc-probe.py`,
  `scripts/stix/tests/{fake-qemu.sh,test-boot-smoke.sh}`.
- Stall catcher: `/tmp/stix-stall-catch.sh` (runs QEMU with
  `-monitor unix:/tmp/stix-baseline-run/mon.sock`, freezes on 45 s of UART
  silence, dumps `info registers` + code window + stack to
  `/tmp/stix-baseline-run/stall-report.txt`; maps PC → symbols with
  `readelf -s` on the FSW ELF).
- Baseline build (5.2 model) for wrapper runs:
  `/tmp/qemu-stix-baseline/build/qemu-system-sparc` — run the current
  qtest suite against it with `QTEST_QEMU_BINARY` pointed there;
  **expected RED** on `ctrl-scet` (5.2 asserts on the SCET write) and
  `ctrl-rotb` (pins the strobe fix 5.2 lacks); pnp/flash/spw-tx/ctrl
  expected green. This run has NOT happened yet.
- SDD ledger (append-only log of every ruling):
  `.superpowers/sdd/2026-11-09-branch-review-tdd-fixes/progress.md`.
- Driving docs: `docs/stix-port/2026-11-09-branch-review-findings.md`
  (the review; Task B = T1),
  `docs/superpowers/plans/2026-11-09-branch-review-tdd-fixes.md` (the plan;
  remaining tasks after B: C=make-check hermeticity, D=doc fixes,
  E=flash addressing, F=stdout tracing/TM hardening).

## Guardrails (Laszlo's standing instructions)

1. Never delete/recreate `/tmp/stix-baseline-run/flash.bin` (16.5 GB,
   ~2.5 min to recreate). Tests use temp flash files and temp run dirs.
2. FSW tree is read-only — recon only.
3. One commit per task, `stix:` prefix; deviations from the 2022 baseline
   update `docs/stix-port/accepted-diffs.md` in the same commit.
4. TDD: red tests first, shown failing, then the fix, then the full gate.
5. If a long-running test fails, pause and synthesize for Laszlo instead of
   auto-relaunching.
6. Laszlo builds the FSW on request; the traced build and a
   DISABLE_WATCHDOG variant are available on ask.
