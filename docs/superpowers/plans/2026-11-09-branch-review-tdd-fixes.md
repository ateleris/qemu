# Branch Review TDD Fixes — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (native, as the partner chose for the port). Steps use checkbox (`- [ ]`) syntax.

**Goal:** Fix the seven findings from the branch review (`docs/stix-port/2026-11-09-branch-review-findings.md`) with strict test-driven development, in the review's section-7 order, with one commit per task.

**Architecture:** Each task is red→green→gate: write the review's red tests first, RUN them and show the failure for the stated reason, make the minimal fix, then run the full regression gate. The review document is the per-task test specification; this plan encodes the five decisions, the order, the gates, and the discipline — it does not re-transcribe the tests.

**Tech Stack:** QEMU 11.1 device code, libqtest, bash test scripts, Python probe.

**Spec:** `docs/stix-port/2026-11-09-branch-review-findings.md` (the binding authority for per-task tests and acceptance criteria), plus the decisions below (confirmed by Laszlo 2026-11-09).

## Global Constraints

- **Decisions (confirmed):** D1 counter semantics + ns resolution (SCET: 0xC8 sets seconds/keeps sub-seconds; 0xCC sets sub-seconds/keeps seconds; ns-precision internal clock). D2 lazy flash file (0xFF past EOF; pad on far writes; no pre-fill ever). D3 TM server off under qtest unless `tmtc-port` set; bind failure → `error_setg` realize error, never `exit(1)`. D4 flash addressing items 1–4 fixed now; guest-reachable asserts NOT converted (baseline-preserved). D5 trace events replace stdout prints; stdout golden reference regenerated from a `-trace` run.
- **TDD:** red test first, failure shown for the stated reason, then minimal fix, then the full gate (review §6). No task done while any gate item is red.
- **One commit per task**, message prefix `stix:`. Any baseline deviation updates `docs/stix-port/accepted-diffs.md` in the SAME commit (expected: T1, T3, T5, T6.1).
- **`/tmp/stix-baseline-run/flash.bin` (16.5 GB) is never deleted or recreated.** Tests use temp flash files and temp run dirs.
- **Absolute paths to `boot-smoke.sh` until T2 lands** (relative path = false PASS, reproduced 2026-11-09).
- **Stop-and-ask:** if T1's Step 0 investigation contradicts D1 semantics.
- The old-build comparison through the wrapper is expected to FAIL for deviation tests (T1's SCET tests, T3's lazy-file test, T5's addressing tests) — the gate lists them explicitly as "expected red on 5.2", everything else must be green.

## Review Focus

1. A green gate that never actually ran QEMU (T2's false PASS) — every "PASS" must be traceable to a fresh QEMU run (stale-output deletion, exit-code capture, missing-file failure).
2. The SCET semantics vs the FSW's real write pattern (T1 Step 0) — a contradiction stops the task.
3. The 16.5 GB flash.bin must never be touched by any test run — every new test uses temp files.
4. Old-build (5.2) runs through the wrapper must keep passing for everything NOT a deliberate deviation.
5. Each accepted-diffs entry must say what changed, why, and how it was verified.

---

### Task A (=T2): Trustworthy boot smoke + soak script

**Files:** Modify `scripts/stix/boot-smoke.sh`; Create `scripts/stix/soak.sh`, `scripts/stix/tests/test-boot-smoke.sh`, `scripts/stix/tests/fake-qemu.sh`. See review §T2 for the red-test table (cases a–e) and implementation notes (`realpath -e` before the cd, stale-output deletion, exit-code capture with rc==124 accepted, missing/empty uart.smoke.raw → fail, `STIX_SMOKE_SECONDS`/`STIX_RUN_DIR` overrides, same handling in the TM section).

- [ ] Step 1: Write `fake-qemu.sh` + the smoke-test script with cases a–e
- [ ] Step 2: Run — cases a, b, c RED for the stated reasons (false PASS / stale reuse / hidden crash); paste output
- [ ] Step 3: Fix `boot-smoke.sh` per the implementation notes
- [ ] Step 4: Cases a–e green; the exact README command (relative path) now genuinely runs QEMU
- [ ] Step 5: Write `soak.sh`; run it — **RED**: abort at ~135 s (this is T1's red evidence too)
- [ ] Step 6: Full gate (§6) — smoke+TM green with ABSOLUTE path; soak red is expected and ledgered
- [ ] Step 7: Commit `stix: harden boot-smoke against false PASS; add soak script`

### Task B (=T1): SCET set semantics (D1) — Step 0 investigation first

**Files:** Modify `hw/sparc/grlib_stixctrl.c`, `hw/sparc/trace-events` (new), `tests/qtest/stix.c`; update `docs/stix-port/accepted-diffs.md`.

- [ ] Step 0: **Investigation (no behavior change):** add `stixctrl_scet_write` trace event; temporarily replace `assert(0)` with log+return; boot with `-trace stixctrl_scet_write` past 135 s; record what the FSW writes (0xC8 only? order? values?). **If it contradicts D1 semantics: STOP and ask Laszlo.**
- [ ] Step 1: Write the 7 red tests (`test_ctrl_scet`, review §T1) — run, show failures (test 1 = the abort itself)
- [ ] Step 2: Implement per review notes: int64_t/ns arithmetic (`qemu_clock_get_ns`), correction-based seconds/subsec writes, `longTimerSubSec` bits 31:16 reserved, remove assert(0) + prints in that block; keep raw-value storage for other offsets
- [ ] Step 3: 7 tests green + existing `/sparc/stix/ctrl` green; **expected red on 5.2** (deviation — wrapper run lists it)
- [ ] Step 4: `soak.sh` 600 s GREEN (was red in Task A); boot smoke still PASS
- [ ] Step 5: accepted-diffs entry; commit `stix: implement SCET set semantics (baseline aborted at ~135 s)`

### Task C (=T3+T4): Hermetic make-check + qtest isolation (D2, D3)

**Files:** Modify `hw/sparc/grlib_stixflash.c` (lazy file, 64-bit offsets where convenient, delete the fill loop + `while(f==NULL)` loop), `hw/sparc/tmtc_io_server.c` (+`.h`) and `hw/sparc/grlib_stixspw.c` (qtest-gated server start, `error_setg` on bind failure), `tests/qtest/stix.c` (`/sparc/stix/flash-lazy` test, remove the dead `zeros_ff` loop, empty temp file suffices), Create `scripts/stix/tests/test-make-check-hermetic.sh`.

- [ ] Step 1: Red tests 1–4 (review §T3: qom-test hermetic; port-held; lazy-file page 1000 read/write/pad; truncated file) + T4's port-held suite run — show failures
- [ ] Step 2: Implement D2 lazy file + D3 qtest gating + realize error
- [ ] Step 3: All green; `make check-qtest-sparc` green with no large file in `build/`; **expected red on 5.2** for the lazy-file test (deviation)
- [ ] Step 4: Boot smoke + soak still green with the existing persistent flash.bin
- [ ] Step 5: accepted-diffs entry (lazy file; TM-off-under-qtest); commit `stix: hermetic flash (lazy file) and TM server under qtest`

### Task D (=T7): Documentation corrections

**Files:** `docs/stix-port/baseline/README.md` (SCET "implemented"; absolute-path note removed after T2; soak as step 6), `README-STIX.md` (serial recommendation obsolete after T6.1 — recheck at execution time; remove "aborts at ~2 min" once T1 lands), note on commit 78e7dd8782's hermeticity scope.

- [ ] Step 1: Apply the review §T7 table against the post-T1/T3 state
- [ ] Step 2: Gate green; commit `stix: documentation corrections from branch review`

### Task E (=T5): Flash addressing (D4: items 1–4)

**Files:** Modify `hw/sparc/grlib_stixflash.c` (`flash_offset()` helper per review: chip stride 2^18 pages, MCM × devices-per-MCM, 64-bit `fseeko`/`ftello` or `pread`/`pwrite`, fflush after writes), `tests/qtest/stix.c` (4 red tests from review §T5), `docs/stix-port/accepted-diffs.md`.

- [ ] Step 1: Red tests 1–4 (chip-1 aliasing, MCM separation, past-512MB read, chip-0 offset compatibility) — show failures
- [ ] Step 2: Implement the offset helper + 64-bit IO
- [ ] Step 3: Tests green; existing flash qtest green; **expected red on 5.2** (deviation); boot smoke + soak green with the persistent flash.bin
- [ ] Step 4: accepted-diffs entry (chip-0/MCM-0 offsets unchanged — existing data preserved); commit `stix: fix flash addressing (64-bit offsets, chip stride, MCM)`

### Task F (=T6): Trace events + TM robustness (D5)

**Files:** Modify `hw/sparc/grlib_stixctrl.c`, `hw/sparc/grlib_stixspw.c` (qemu_printf → trace events), `hw/sparc/trace-events`, `hw/sparc/tmtc_io_server.c` (nonblocking send + drop counter, `qatomic_*`/mutex for clientSocket, `qemu_thread_create`, `qemu_socket`/`qemu_accept`), scripts (`scripts/stix/tests/` stdout-cleanliness test), **regenerate `docs/stix-port/baseline/qemu-stdout.golden.log` from a `-trace 'stix*'` run**, update baseline README.

- [ ] Step 1: Red test: boot `-nographic` 10 s → stdout must contain no `SPW |`/`CTRL |` lines (red today)
- [ ] Step 2: Convert prints to trace events (D5); regenerate the stdout golden reference with `-trace 'stix*'`; update README run rules
- [ ] Step 3: TM robustness: `MSG_DONTWAIT|MSG_NOSIGNAL` + drop counter; synchronized clientSocket; qemu_thread_create/socket wrappers (review §T6.2–4; the never-reading-client test as the gate)
- [ ] Step 4: Full gate green incl. boot smoke + soak; commit `stix: trace events for register access; TM send hardening`

## Success criteria

- All seven review findings closed; every task's red→green evidence in its commit message; gate green after every task; soak 600 s green; accepted-diffs carries every deviation.
