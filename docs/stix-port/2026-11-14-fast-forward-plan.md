# Fast-forward (FF) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Get to "an hour of observation" or "a full ROTB / archive" without waiting for it, without changing what the FSW does.

**Architecture:** Three independent levers. (1) Pacing: `-icount shift=N,sleep=off` skips guest idle time instead of waiting it out. (2) A detector-model ROTB burst: one runtime QOM write appends N frames at once. (3) A flash library in the platform controller: save the flash state of a run and start a later run from it (the FSW rebuilds its file-system state from flash at boot).

**Tech Stack:** QEMU C (hw/sparc/stix_det.c, grlib_stixctrl.c), qtest (tests/qtest/stix.c), bash (scripts/stix/rotb-fill-run.sh), Python 3 stdlib (scripts/stix/platform/stixplat).

**Spec:** the user's request and the mockup board "8 · Setup" (canvas https://claude.ai/artifact/V9rbLbVVcfwFJjnoiKciqJ), discussed in chat 2026-11-14; the measured speeds are in this plan's Background. The user said "now let's implement this" for all three levers.

**Worktree:** /home/laszlo/dev/qemu-stix-rotb, branch stix-uprev-rotb (equal to stix-emul-qemu-uprev 542168f4b6 at the start).

## Background (measured, this host)

Wall seconds per guest second, from the run.json/fill.csv of past runs:
icount 3 = 1.40, icount 6 = 1.04, icount 7 = 0.35, icount 8 = 0.27.
At icount 3 the guest is busy all the time, so the host runs ≈ 125 M
guest instr/s ÷ 1.4 ≈ 89 M instr/s. At icount 7 a fully busy guest would
need 7.8 / 89 ≈ 0.09 wall s per guest s; the measured 0.35 is the idle
time waited out in real time (QEMU's default icount `sleep=on`).

FSW facts the burst must respect (STIX-FSW 128f30b0):
- ArchiveManager.c:125-195: the first chunk after start is thrown away; a
  chunk's archived duration = (its SCET end − the previous chunk's SCET
  end + 50 ms) / 100 ms, as u16. SCETs that go backwards wrap that u16.
- RotAccuCompactor.c:149-152: the block SCET is also the key of the RCR
  history lookup (RateCtrl_getRCRatTime).

## Global Constraints

- No FSW edits. No AI attribution in commits.
- Runtime QOM properties follow the det-* pattern in stix_det.c (object_class_property_add on /machine/stixctrl, description ends "(runtime)").
- Platform API v1: additions only; new verbs go in verbs.py VERBS and get a self-test check.
- rundir.py's "The controller never writes flash.bin itself" changes: the controller may now replace flash.bin while power is off (flash load only). Update that docstring.
- Sparse copies only (`cp --sparse=always`); C: is the disk limit.
- Repo text cites FSW code only, never the confidential docs.

## Review Focus

1. Burst SCETs must not go backwards against live chunks already in the ring or archived. A burst right after a live chunk gets near-zero durations; the next live chunk after the burst must still have a SCET later than the last burst frame. Task 2 test `det-rotb-burst` checks monotonic SCETs across live → burst → live.
2. Burst into a nearly full ring with drop-new: stop at the first chunk that does not fit, and report how many went in (not an error). Task 2 test checks `appended < requested` and `stop=ring-full`.
3. `pace=fast` with icount off is meaningless (`sleep=off` needs icount): reject it in both the run script and the platform config. Tasks 1 and 3 test the rejection.
4. Flash save while QEMU is running and writing: only allowed when power is off or the sim is paused (the device fflush()es each page before its completion IRQ, grlib_stixflash.c:535, so a paused file is consistent). Task 4 test checks the refusal while running.
5. Flash library names from the API: reject path tricks (`../x`, `/abs`, empty, > 64 chars); only `[A-Za-z0-9._-]{1,64}`, not starting with `.`. Task 4 test checks them.

---

### Task 1: Run-script pacing (`--pace wall|fast`)

**Files:**
- Modify: `scripts/stix/rotb-fill-run.sh` (usage block near :36-80, option parse near :115, validation near :184-189, QEMU_ARGS near :401-403, run.json near :450)

**Interfaces:**
- Produces: `--pace wall|fast` (default `wall`); run.json key `"pace": "wall"|"fast"`; rotb-fill-report.py `RUN_PARAM_KEYS` gains `"pace"` and the matrix shows it in a `pace` column (width 4, `wall`/`fast`, `-` for old runs).

- [ ] **Step 1:** Add the option. `fast` adds `,sleep=off` to the `-icount shift=$ICOUNT` argument. `--pace fast` with `--icount 0` exits 2 with `--pace fast needs --icount > 0 (sleep=off is an icount option)`. Usage text: one line saying fast skips guest idle time and leaves the guest's own timing unchanged.
- [ ] **Step 2:** Verify the rejection: `scripts/stix/rotb-fill-run.sh --icount 0 --pace fast --out /tmp/x` → exit 2 and the message, before anything is launched. `--pace slow` → exit 2.
- [ ] **Step 3:** rotb-fill-report.py: add `pace` to RUN_PARAM_KEYS and the matrix column; extend the self-test's run.json with `"pace": "fast"` and assert the matrix row contains `fast`. Run `python3 scripts/stix/rotb-fill-report.py --self-test` → PASS.
- [ ] **Step 4:** Commit `stix: rotb-fill-run --pace fast (icount sleep=off)`.

### Task 2: Detector-model ROTB burst (`det-rotb-burst`)

**Files:**
- Modify: `hw/sparc/stix_det.c` (state use in stix_det_nominal_swap :1861-1894, class init :2353ff), `hw/sparc/stix_det.h` (StixDetState fields), `hw/sparc/trace-events`
- Test: `tests/qtest/stix.c` (new `test_det_rotb_burst`, registered next to `/sparc/stix/det-rotb-hw-state`)

**Interfaces:**
- Produces (runtime QOM on /machine/stixctrl):
  - `det-rotb-burst` (uint32, write-only): append this many frames now.
  - `det-rotb-burst-status` (str, read-only): `requested=N,appended=M,stop=done|ring-full|rotb-disabled|rotb-testmode,rotb_count=C` of the last burst, `requested=0,...` before any.
  - trace `stixdet_rotb_burst(uint32_t requested, uint32_t appended, const char *stop, uint32_t rotb_count, int64_t vclock_ns)`.

Semantics (decided):
- A burst frame is a copy of the **last live chunk** (`d->chunk` as built by stix_det_chunk_build), so its content is real detector-model output. The sky PRNG and the integration banks are not touched.
- SCETs: the model keeps the last live chunk's SCET (`last_chunk_coarse`, `last_chunk_fine`, `last_chunk_valid`, set in stix_det_nominal_swap after an APPENDED or FULL chunk build). Burst frame k = 1..N gets SCET = last + (now − last) · k / N, computed on the 48-bit coarse:fine value (fine = 1/65536 s); the UNSYNCED bit 31 of coarse comes from the current SCET register. All 49 blocks of a frame carry it (bytes +2..+7). After the burst, last = now. Then the next live chunk is later than every burst frame.
- Rejected with error_setg (nothing appended): N = 0; N > STIXCTRL_ROTB_MAX_BLOCKS / STIX_DET_CHUNK_BLOCKS (2131); det-rotb off; no live chunk since the last device reset (`"det-rotb-burst repeats the last live frame: none yet"`).
- Not an error: stop at the first append that is not APPENDED; the status and trace say how many went in and why.
- Device reset clears last_chunk_valid and the status.
- Archive caveat (property description and docs): burst frames get near-zero archived durations (ArchiveManager.c:168).

- [ ] **Step 1: Write the failing qtest `test_det_rotb_burst`**, modelled on `test_det_rotb_hw_state` (same machine set-up to enable the ROTB and run the nominal FSM to the first chunk). Assertions:
  - before any chunk: `qom-set det-rotb-burst 5` returns an error containing `none yet`;
  - after one live chunk (rotb count = 49): set 10 → status `requested=10,appended=10,stop=done,rotb_count=539`; the ring holds 11 chunks;
  - SCETs read from block 0 of each chunk in ring order (coarse:fine as u48) strictly increase across live, the 10 burst frames, and the next live chunk;
  - every burst frame's bytes outside +2..+7 of each block equal the live chunk's;
  - set 0 and set 2132 return errors;
  - fill the ring to within 3 chunks of capacity (rotb-blocks property small, e.g. 49 * 20), set 10 → `appended=3,stop=ring-full`.
- [ ] **Step 2:** Build and run: `ninja -C build tests/qtest/stix && QTEST_QEMU_BINARY=build/qemu-system-sparc build/tests/qtest/stix -p /sparc/sparc/stix/det-rotb-burst` → FAIL (property not found).
- [ ] **Step 3:** Implement the two properties, the state fields and the trace event as specified. Keep the burst in a function `static void stix_det_rotb_burst(StixDetState *d, uint32_t n, Error **errp)`.
- [ ] **Step 4:** Run the test → PASS; run the whole suite `STIX_PORT_BASE=auto STIX_SLOTS=4 scripts/stix/qt.sh --no-build all` → all pass.
- [ ] **Step 5:** Commit `stix: det-rotb-burst appends N copies of the last live frame`.

### Task 3: Platform pacing and the burst verb

**Files:**
- Modify: `scripts/stix/platform/stixplat/config.py` (DEFAULTS "qemu", CHOICES, validation), `stixplat/qemu.py` (build_argv), `stixplat/plat.py` (verb registration, next to force.set :181), `stixplat/verbs.py`, `stixplat/selftest.py`, `docs/stix-port/platform.md`

**Interfaces:**
- Consumes: Task 2's `det-rotb-burst` / `det-rotb-burst-status`.
- Produces:
  - config `qemu.icount_shift` (int 0..15, default 0 = no icount, today's behaviour) and `qemu.pace` ("wall" | "fast", default "wall"); config load fails on `pace = "fast"` with `icount_shift = 0`.
  - build_argv adds `-icount shift=N` (+ `,sleep=off` for fast) when icount_shift > 0.
  - verb `det.rotb.burst {"frames": N}` → qom-set, then returns the parsed status `{"requested":N,"appended":M,"stop":S,"rotb_count":C}`; a QOM error becomes a VerbError with its text. Power must be on.

- [ ] **Step 1:** Self-test cases first: argv for (0,"wall") has no `-icount`; (7,"wall") has `-icount shift=7`; (7,"fast") has `-icount shift=7,sleep=off`; config (0,"fast") raises the config error; `det.rotb.burst` is in VERBS and registered; with power off it raises VerbError. Run `python3 stixplat.py --self-test` → FAIL.
- [ ] **Step 2:** Implement; run the self-test → PASS.
- [ ] **Step 3:** platform.md: the two config keys, the verb, and the burst caveats (copies of the last live frame, near-zero archived durations).
- [ ] **Step 4:** Commit `stix: platform icount/pace config and det.rotb.burst`.

### Task 4: Platform flash library

**Files:**
- Create: `scripts/stix/platform/stixplat/flashlib.py`
- Modify: `stixplat/plat.py` (verbs), `stixplat/verbs.py`, `stixplat/rundir.py` (docstring :5), `stixplat/selftest.py`, `docs/stix-port/platform.md`

**Interfaces:**
- Produces:
  - `flashlib.library_dir(cfg) -> str` = `<retention.run_root>/flash-library`.
  - `flashlib.check_name(name: str) -> None` raises ValueError unless `^[A-Za-z0-9_-][A-Za-z0-9._-]{0,63}$`.
  - `flashlib.save(cfg, run_dir, name, meta: dict) -> dict`: sparse copy `run_dir/flash.bin` → `<lib>/<name>.bin`, writes `<lib>/<name>.json` = meta + `{"name", "created_utc", "allocated_kib", "source_run_dir"}`; refuses an existing name (`exists`); returns the json dict.
  - `flashlib.load(cfg, run_dir, name) -> dict`: sparse copy `<lib>/<name>.bin` → `run_dir/flash.bin` (replacing it); returns the json dict.
  - `flashlib.listing(cfg) -> list[dict]`: the json dicts, sorted by name.
  - verbs: `platform.flash.save {"name"}` (power off, or on and paused; meta = ELF path + sha256, QEMU binary, sim_time, boot_id), `platform.flash.load {"name"}` (power off only), `platform.flash.list {}` (always).
- Copy command: `cp --sparse=always`, timeout 120 s, as rotb-fill-run.sh does; afterwards the existing sparse check of rundir.py (≤ power.sparse_limit_mb allocated) applies to the copy, and a too-dense copy is deleted and the verb fails.

- [ ] **Step 1:** Self-test cases first (temp dirs, a small sparse file standing in for flash.bin): save → list shows it with allocated_kib; save the same name again → `exists`; load replaces run_dir/flash.bin with identical bytes; names `../x`, `/abs`, `''`, `.hidden`, 65 chars → ValueError; save while running and not paused → VerbError; load while power on → VerbError. Run the self-test → FAIL.
- [ ] **Step 2:** Implement flashlib.py and the verbs; run → PASS.
- [ ] **Step 3:** platform.md: the library, the verbs, the "fill on a fast profile, save, load on the calibrated one" workflow, and why it is consistent (FSW rebuilds FS state at boot; the device fflush()es each page). rundir.py docstring updated.
- [ ] **Step 4:** Commit `stix: platform flash library (save/load/list)`.

### Task 5: A/B measurement and docs (orchestrator, not a worker)

- [ ] **Step 1:** Two runs on the prof ELF, `--ticks 0 --max-ticks 0 --cps 20000 --rcr off --icount 7 --guest-seconds 900`, one `--pace wall`, one `--pace fast`, in parallel slots.
- [ ] **Step 2:** Pass if, on the fast run: processed fr/s within 3 % of the paced run, the skip count equal or ±1, the refill intervals within 5 s; report the wall s per guest s of both.
- [ ] **Step 3:** One burst check on the platform: power on, NOMINAL, `det.rotb.burst 2000`, and watch the HK RB level go to 6–7 and drain; UART has no new error lines other than the known overrun ones.
- [ ] **Step 4:** Results into this plan (a Results section) and the ROTB fill plan's knobs table; memory note.
- [ ] **Step 5:** Gate (`gates2.sh /home/laszlo/dev/qemu-stix-rotb ff -j 4`), ff-merge, post-merge steps.

## Out of scope

- The Setup tab UI itself: it waits for the UI mockup review and the timeline design; the config keys and verbs here are what it will call.
- Writing archive files without the FSW (would need an FSW-format file-system writer with the word-swapped BCH ECC).
- Full QEMU snapshots (only grlib-stixflash has vmstate).

## Amendment A (plan review, GLM, 2026-11-14)

These override the tasks above where they differ.

- A1 (MAJOR, Task 2 SCET): mask UNSYNCED bit 31 off both coarse values
  before the u48 (coarse << 16 | fine) arithmetic, then OR in the current
  register's flag. If now <= last (the guest stepped SCET back via the
  time correction, grlib_stixctrl.c:257-268, or the sim is paused so the
  vclock did not move), delta = 0: every burst frame carries `last`.
  The qtest asserts strictly increasing SCETs in the normal case and
  non-decreasing in a paused-clock case (burst twice without stepping
  the clock). The property description mentions the paused case.
- A2 (Task 2 docs): under rotb-overflow=overwrite-oldest append never
  returns FULL, so a burst overwrites the oldest blocks instead of
  stopping; say so in the property description and platform.md.
  Task 5 Step 3 runs with drop-new.
- A3 (Task 2 test): model the qtest on test_det_chunk_format
  (tests/qtest/stix.c:7724), not test_det_rotb_hw_state: rotb_qtest_new(N),
  rotb_cfg(true, false), sky_start_nom, det_step_ticks, and read chunks
  with nom_read_chunk (:7640). Add two small helpers: a qom-get returning
  a string, and a qom-set returning the QMP error desc (pattern:
  flash_dmg_set, :13098). Test path: `-p /sparc/stix/det-rotb-burst`.
- A4 (Task 1 Step 2): the script needs the positional QEMU binary and
  --ticks: `scripts/stix/rotb-fill-run.sh build/qemu-system-sparc --ticks 1
  --icount 0 --pace fast --out /tmp/x` → exit 2 with the pace message.
  The pace check goes in the :182-209 validation block.
- A5 (Task 1 run.json): keep `"icount": "shift=N"` unchanged; sleep=off
  lives only in the new `"pace"` key (the matrix truncates icount).
- A6 (Task 4 sparse check): rundir.sparse_check is hard-wired to
  run_dir/flash.bin; flashlib does its own allocated-blocks check
  (os.stat st_blocks * 512) against cfg["power"]["sparse_limit_mb"].
- A7 (Task 4 meta): ELF sha256 = hashlib over cfg["qemu"]["elf"];
  sim_time = self.clock.exact_sim(); boot_id = self.clock.boot_id;
  paused = self.power.state == "paused".
- A8 (Task 3 verb): det-* live on /machine/stixctrl; add a STIXCTRL path
  constant next to STIXSPW (plat.py:24) and reuse the `_q()`/`_qom_set`
  VerbError pattern (plat.py:61-71).

## Results (2026-11-14)

Implemented: Task 1 42f0b4921a, Task 2 e15c422d71, Task 3 8440c6475a,
Task 4 0b73915f6e (+ b7212c5cc0: a bad library name is a 400 before
the power-state check). Gate `ff` 7/7, suite all pass (with
/sparc/sparc/stix/det-rotb-burst; note the qtest path carries the
arch prefix, Amendment A3 had it without), platform self-test PASS.

**Pacing A/B** (prof ELF, --ticks 0 --max-ticks 0 --cps 20000 --rcr off
--icount 7, 1800 guest s; paced = rf-cal-p-i7-s1, fast =
rf-ff-p-i7-fast-s1):

| | paced | fast |
|---|---|---|
| processed frames/s | 7.32 | 7.40 |
| overrun skips | 4 (first 892 s) | 4 (first 899 s) |
| refill between skips | 232.7, 231.8, 227.6 s | 232.1, 233.2, 227.5 s |
| wall s per guest s | 0.352 | 0.282 |

Same behaviour (well inside the 3 % / ±1 / 5 s criteria), but only
20 % faster, not the ≈ 0.1 wall s per guest s estimated from the
icount 3 runs: at this load little guest time is idle. What else sets
the pace (probably host-side device-model work per guest second) is
not profiled yet; a host profile is the next speed lever.

**Burst on the FSW** (prof ELF, --ticks 9 (1 s integration) --cps 20000
--icount 7 --pace fast, 1200 guest s, rf-ff-burst-s2; burst via a second
QMP socket at guest 302 s): `requested=2000,appended=2000,stop=done,
rotb_count=98050`. HK RB level 0 → 6, then 5, 3, 2, 1, 0; no overrun.
The FSW drained at ≈ 300-390 blocks/s while the backlog lasted (the
same processing as at 0.1 s), then back to the 49 b/s inflow. The prof
ELF prints nothing on the UART, so the "no new error lines" check is
empty for it.

## Task 6 (added 2026-11-14 after the host profile): idle warp in the vCPU thread

**Why:** with no perf on WSL2, a /proc sample of a fast-paced run
(icount 7, 0.1 s, 20000 cps) showed the vCPU thread at 36 % user, 39 %
kernel and ~25 % blocked, the main loop at 25 % and the monitor
iothread at 13 %, with thousands of 8-byte eventfd reads/writes per
second and ~4000 voluntary context switches per second. Clock reads
are vDSO (49 ns), no page faults; the GPTIMER is the 200 Hz RTEMS tick
(reload 0x1387). Upstream's sleep=off warp runs in the main loop, so
every idle period (the RTEMS idle power-down) costs a vCPU -> main loop
-> vCPU round trip.

**Change:** accepted-diffs §49 (icount_warp_idle_vcpu + the bounded
loop in rr_cpu_thread_fn). No-op for sleep=on, replay, qtest.

**Spike results** (prof ELF, icount 7, 20000 cps, --pace fast):

| cell | unpatched | spike | final |
|---|---|---|---|
| 0.1 s, 1800 guest s, NOMINAL wall/guest | 0.277 | 0.253 | 0.257 |
| 0.1 s skips / refill | 4 / 232.1, 233.2, 227.5 s | 4 / 232.4, 230.7, 226.3 s | 4 / 232.1, 230.7, 227.1 s |
| 0.1 s processed | | | 7.34 fr/s |
| 1 s, 600 guest s, NOMINAL wall/guest | 0.368 | 0.052 | 0.054 |
| 1 s drain / flash ops/s / processed | 49.9 / 14.34 / | 49.9 / 14.32 / | 49.9 / 14.32 / 1.02 fr/s |

"final" is the reviewed code (two review rounds; an exit_request
check and a per-warp clock notify each cost the 1 s case back to
0.17-0.22 and were taken out). Guest-side numbers (skips, refill,
drain, flash ops) match the unpatched binary within run noise.

**Platform:** at ~20x real time the controller's bench-time timing
cannot keep the time-sync margin; gate-spw-link fails 3/3 under
patched + fast at its time-sync step (`late_apply`) and passes under
patched + wall and unpatched + fast (~2.7x). A platform limit,
documented in platform.md (Pacing): timing-sensitive scenarios run
with pace = wall.

The busy 0.1 s case keeps most of its overhead; where it goes is still
open (the kernel share under load is not the idle round trip).
