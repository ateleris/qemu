# STIX v11.1 branch review: findings and TDD fix plan

**Branch:** `stix-emul-qemu-v11.1` (tip `f5420ac48e`, 21 STIX commits on QEMU v11.1.3)
**Reviewed:** 2026-11-09
**Audience:** planner and implementer. Each task is self-contained: problem,
evidence, root cause, the failing test to write first, implementation notes,
and acceptance criteria.

---

## 1. Summary

The port is faithful to the repaired 2022 v5.2 baseline. It builds cleanly,
the 4 STIX qtests pass, and the flight software (`ASW_em.elf`, FSW v1.86)
boots with a UART log identical to the golden log, with telemetry flowing on
TCP 21000.

It is **not stable for long runs**, and parts of the test tooling can report
success without testing anything:

| ID | Priority | Problem | Inherited from 5.2? |
|---|---|---|---|
| T1 | P0 | QEMU aborts at ~135 s in every FSW run (SCET-set `assert(0)`) | yes |
| T2 | P0 | `boot-smoke.sh` gives a false PASS with a relative binary path | no (new tooling) |
| T3 | P1 | `make check` writes a 16.5 GB `flash.bin` into `build/` and fights over port 21000 | partly |
| T4 | P1 | STIX qtest fails if any STIX QEMU is running; dead code in the test | no |
| T5 | P2 | Flash addressing: 32-bit overflow aliases chips, MCM ignored, `ftell` truncation, broken files from interrupted creation | yes |
| T6 | P3 | Per-access stdout tracing; blocking TM `send()` on the vCPU thread; unsynchronised socket global | yes |
| T7 | P1 | Docs contain wrong statements (assert "only under icount", smoke command) | no |

"Inherited" means the old build behaves the same. Fixing an inherited
problem is a **deliberate deviation from the baseline** and must be
recorded in `docs/stix-port/accepted-diffs.md` (see section 5).

---

## 2. How the review was done (reproduction)

All commands from the repo root, uid 1000 (TM port 21000).

```bash
# Build
make -C build -j$(nproc)

# Device qtests: 4/4 pass
QTEST_QEMU_BINARY=$PWD/build/qemu-system-sparc build/tests/qtest/stix

# Boot smoke + TM. NOTE: absolute path required (see T2)
bash scripts/stix/boot-smoke.sh $PWD/build/qemu-system-sparc --with-tm
#   -> BOOT SMOKE: PASS, TM CHECK: PASS (2m17s)

# Long run, no TM client: aborts
cd /tmp/stix-baseline-run && timeout 300 $OLDPWD/build/qemu-system-sparc \
  -M leon3_generic -m 64M -no-reboot -display none -monitor none \
  -serial file:/tmp/uart.log \
  -kernel /mnt/c/Users/laszl/Development/stix_asw_src/ASW/ASW/bin/ASW_em.elf
#   -> rc=134 after 135 s:
#   grlib_stixctrl.c:97: grlib_stixctrl_write: Assertion `0' failed.
#   Old 5.2 build (/tmp/qemu-stix-baseline/...): identical abort at 136 s.
```

---

## 3. TDD ground rules for every task

1. **Red:** write the test first, run it, and **see it fail for the expected
   reason** (paste the failure output into the commit message or PR).
2. **Green:** make the smallest change that makes it pass.
3. **Refactor** only with the tests green.
4. After each task, run the full regression gate (section 6). No task is
   done while any gate item is red.
5. One task per commit, prefix `stix:`. If the task deviates from the
   baseline, the same commit updates `accepted-diffs.md`.
6. Do not delete or recreate `/tmp/stix-baseline-run/flash.bin` (16.5 GB,
   ~2.5 min to create). Tests that need a flash file use a temporary one.

---

## 4. Tasks

### T1 (P0): QEMU aborts when the FSW sets the SCET clock

**Problem.** Around guest time 00:01:00 (~135 s wall clock) the FSW writes
the CTRL long-timer register `longTimerSec` (offset `0xC8`). The write
handler hits `assert(0)` and QEMU dies with SIGABRT (exit 134). This happens
on **every** normal run, without `-icount`. The golden capture and smoke test
stop at 90 s, so they never reach it.

**Evidence.** Last QEMU output before the abort:

```
CTRL | Read requested: addr = 0xc8, regval = 0x7c
CTRL | Read requested: addr = 0xcc, regval = 0x3168
CTRL | Read requested: addr = 0xc8, regval = 0x7c
qemu-system-sparc: ../hw/sparc/grlib_stixctrl.c:97: grlib_stixctrl_write: Assertion `0' failed.
```

The last UART lines are the once-per-second
`Watchdog *E invalid number of cycles after reset: exp=63 hw=0`
(61 of them). The watchdog device is not instantiated; this may be related
to what makes the FSW set the clock but is not confirmed.

**Root cause** (`hw/sparc/grlib_stixctrl.c`):
- L94–97: `if (addr == 0xC8 || addr == 0xCC) { assert(0); ... }`.
- The code after the assert is also wrong and must not just be enabled:
  - L108: `time_correction_ms = time_correction_s - (tc/1000)*1000` is
    always 0, so writing seconds drops the current sub-second part.
  - The `0xCC` branch (L112) keeps only the whole-second part of the
    *correction*, not the current *seconds value*. The result depends on
    the previous correction instead of the current clock.
  - Mixed `uint64_t` / `int64_t` arithmetic: a correction that makes "now"
    negative wraps to a huge value and trips the `assert` in `msToSCET`.
  - `longTimerSubSec` has a reserved/control field in bits 31:16
    (`STIXCTRL_LONGTIMERSUBSEC_RESERVED`); only bits 15:0 are sub-seconds
    (1/65536 s).

**Step 0 (investigation, before the test).** Find out what value the FSW
writes and in which order (0xC8 only? 0xC8 then 0xCC?). Suggested: add a
trace event `stixctrl_scet_write(uint32_t addr, uint32_t value)` (see T6)
and boot with `-trace stixctrl_scet_write`, temporarily replacing the
assert with a log + return. Record the findings in this task's PR. If the
FSW source is available, also find the call site (look for writes to
`longTimerSec` in `ASW/src`, likely time-sync / timecode handling).

**Open decision for the planner (D1):** what "set the clock" means.
Recommended semantics, matching a free-running hardware counter:
- Write `0xC8` = V: seconds become V, the sub-second count continues.
- Write `0xCC` = F: sub-seconds become `F & 0xFFFF`, seconds unchanged.
  Bits 31:16 are stored but do not affect the time.
- Reads continue to return the corrected virtual time.

**Red tests** (add to `tests/qtest/stix.c`, new function `test_ctrl_scet`,
registered as `/sparc/stix/ctrl-scet`). Use `qtest_clock_step()` to move
virtual time; under `-accel qtest` the virtual clock moves only when the test
steps it.

1. *No abort:* `ctrl_write(qts, 0xC8, 1000)`. Currently the QEMU child
   aborts and libqtest fails. This is the red you must see.
2. *Seconds set:* after writing 1000, `ctrl_read(0xC8) == 1000`.
3. *Clock runs:* `qtest_clock_step(qts, 1500 * SCALE_MS)`, then
   `0xC8 == 1001` and `0xCC & 0xFFFF` is within ±1 of `0x8000`.
4. *Sub-seconds set:* write `0xCC = 0x4000`, then `0xC8` is unchanged and
   `0xCC & 0xFFFF` is within ±1 of `0x4000`.
5. *Seconds write keeps sub-seconds:* step to a known fraction, write
   `0xC8 = 5`, then the fraction is unchanged (this pins the L108 bug).
6. *Large epoch value:* write `0xC8 = 0x30000000` (realistic SCET), step
   1 s, then `0xC8 == 0x30000001` (no `msToSCET` assert, no wrap).
7. *Set backwards:* write 1000, then write 10, then `0xC8 == 10` (signed
   correction).

**Implementation notes.**
- Keep `time_correction` as `int64_t` and do all arithmetic in `int64_t`
  ms. Suggested shape:
  ```c
  int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
  int64_t cur = now - s->time_correction;        /* current SCET in ms */
  int64_t sec = cur / 1000, frac_ms = cur % 1000;
  /* 0xC8: */ s->time_correction = now - ((int64_t)value * 1000 + frac_ms);
  /* 0xCC: */ s->time_correction = now - (sec * 1000 + toMs(0, value & 0xFFFF));
  ```
- Millisecond resolution loses precision (1 ms ≈ 65.5 sub-second ticks).
  The ±1-tick test tolerance only holds if the write/read path keeps more
  precision. Either switch the model to `QEMU_CLOCK_VIRTUAL` in ns, or
  widen the tolerance to ±66 ticks and document it. **Recommendation:**
  switch to ns. It is a small change and avoids drift on repeated syncs.
- Remove the `assert(0)` and the `qemu_printf`s in that block.
- Still store the raw written value in `reg` so the existing store-and-read
  behaviour of other offsets is unchanged.

**Acceptance criteria.**
- All 7 tests pass and the existing `/sparc/stix/ctrl` still passes.
- New long-run check (see T2: `scripts/stix/soak.sh`): the FSW runs for
  **10 minutes** with no abort, and UART output keeps growing in the last
  minute.
- Boot smoke is still PASS (the boot sequence ends at ~2.9 s, before any
  SCET write).
- `accepted-diffs.md` gets a new entry: "SCET set implemented; baseline
  aborted at ~135 s".

---

### T2 (P0): `boot-smoke.sh` false PASS; add a long-run soak

**Problem.** `bash scripts/stix/boot-smoke.sh build/qemu-system-sparc`, the
form documented in the README, prints `BOOT SMOKE: PASS` without running
QEMU:
- L22 checks `-x "$QEMU_BIN"` **before** `cd "$RUN_DIR"` (L37).
- After the `cd`, the relative path no longer resolves, and `timeout` fails
  with "No such file or directory".
- `|| true` (L43) hides the failure.
- The stale `uart.smoke.raw` from a previous run is still in
  `/tmp/stix-baseline-run`, gets normalised, and matches the golden log.
- The same pattern repeats in the TM section (L77–80), which then fails with
  a misleading "could not connect".

Observed: a "PASS" in 40 s total; `qemu-stdout.smoke.log` contained only
`timeout: failed to run command 'build/qemu-system-sparc'`.

**Red tests.** New file `scripts/stix/tests/test-boot-smoke.sh` (plain bash,
exit non-zero on the first failure). It needs the script to be testable
without the real FSW, so first add two env overrides to `boot-smoke.sh`:
`STIX_RUN_DIR` (default `/tmp/stix-baseline-run`) and the existing
`STIX_ASW_ELF`. Each case runs in a fresh `mktemp -d` run directory with an
empty `flash.bin` and a dummy ELF file.

Fake QEMU: `scripts/stix/tests/fake-qemu.sh`. It parses `-serial file:X`
and, depending on `FAKE_QEMU_MODE`, either copies
`docs/stix-port/baseline/uart.golden.log` to X (`good`), writes nothing
(`silent`), or exits 1 immediately (`crash`).

| Case | Setup | Expected |
|---|---|---|
| a | relative path to fake QEMU, mode `good`, called from a different CWD | PASS, exit 0 |
| b | mode `silent`, stale golden-matching `uart.smoke.raw` already in run dir | FAIL, exit ≠ 0 |
| c | mode `crash` | FAIL, exit 2 |
| d | non-existent binary path | exit 2 |
| e | fake QEMU that runs past the timeout (killed by `timeout`, rc 124) with good output | PASS (124 is the normal end of a timed run) |

Today, cases a, b and c are red.

**Implementation notes.**
- `QEMU_BIN="$(realpath -e "$1")" || exit 2` before the `cd`.
- `rm -f uart.smoke.raw uart.smoke.norm uart.smoke.boot qemu-stdout.smoke.log`
  before the run.
- Capture the exit code instead of `|| true`:
  `rc=0; timeout ... || rc=$?`. Accept `rc == 124` (time limit reached).
  Any other non-zero value means QEMU failed: print the tail of
  `qemu-stdout.smoke.log` and exit 2.
- Fail if `uart.smoke.raw` is missing or empty.
- Allow `DURATION` to be overridden (`STIX_SMOKE_SECONDS`) so tests run fast.
- Apply the same handling to the TM section.

**Add a soak script** `scripts/stix/soak.sh <qemu-binary> [seconds=600]`:
it boots the FSW with the same flags and exits non-zero if QEMU exits
before the time limit (anything other than rc 124) or if the UART log did
not grow in the last 60 s. It is the acceptance check for T1 and catches
any future steady-state crash. **This must be red before T1 lands**
(abort at ~135 s) and green after.

**Acceptance criteria.** All cases in the table pass; the README command
works with both relative and absolute paths; `soak.sh` exists and is listed
in the regression gate.

---

### T3 (P1): `make check` creates a 16.5 GB `flash.bin` and collides on port 21000

**Problem.** Upstream generic qtests start every sparc machine, including
`leon3_generic`: `qom-test`, `cdrom-test`, `test-hmp`. Under qtest the
"missing BIOS" error is skipped (`hw/sparc/leon3.c:378`), so the STIX
devices are realized:
- `grlib-stixflash` sees no `flash.bin` in CWD (meson runs tests from
  `build/`) and fills a 16.5 GB file with 0xFF, ~2.5 min, during realize.
  Measured: `qom-test -p /sparc/qom/leon3_generic` had written 3.9 GB after
  30 s and had not finished.
- `grlib-stixspw` binds 127.0.0.1:21000 and calls `exit(1)` if bind fails
  (`tmtc_io_server.c`, `init_tmtc_server`). Parallel qtest processes fight
  over the port.
- An interrupted creation leaves a truncated `flash.bin` that the next start
  accepts (`fopen("r+b")` succeeds). Later accesses past the truncated end
  hit `assert(false)` (see T5).

**Open decision for the planner (D2), flash backing when no file is
configured.** Options:
- (a) **Lazy file (recommended):** never pre-fill. Treat bytes past EOF as
  erased (0xFF) on read. On write/erase past EOF, first pad from EOF to the
  target offset with 0xFF, then write. Compatible with the existing
  `/tmp/stix-baseline-run/flash.bin`. Startup becomes instant, and an
  interrupted creation is no longer possible. Cost: the first write far into
  the address space pays a one-time pad.
- (b) Skip flash file creation when `qtest_enabled()` and no `flash-file`
  was given (no backing, reads return 0xFF, writes ignored with a log).
  Smaller change, but it is a qtest-only code path.
- (c) Both.

**Open decision (D3), TM server under qtest.** Recommended: when
`qtest_enabled()` and `tmtc-port` was not set explicitly, do not start the
TM server. Separately, turn the bind failure into a proper realize error
(`error_setg(errp, ...)` and return) instead of `exit(1)` from inside the
device.

**Red tests.**
1. Script test `scripts/stix/tests/test-make-check-hermetic.sh`: in a
   `mktemp -d` CWD, run
   `QTEST_QEMU_BINARY=<abs build>/qemu-system-sparc timeout 60 <abs build>/tests/qtest/qom-test -p /sparc/qom/leon3_generic`.
   Expect exit 0 in < 20 s, and no file in the CWD larger than 1 MB.
   Today it times out with a multi-GB `flash.bin`.
2. Same test, but with a listener already bound to `127.0.0.1:20000+uid`
   while it runs. Expect exit 0. Today: "Bind failed", abort.
3. qtest in `stix.c` (`/sparc/stix/flash-lazy`): with `flash-file` pointing
   to a **zero-length** temp file, page-read page 1000 and check RAM is all
   0xFF. Then page-write page 1000 with a pattern, page-read it back, and
   check that page 999 still reads 0xFF (padding is 0xFF, not 0x00).
   Today this hits `assert(false)` (length check).
4. Same as 3 against a **truncated** file (e.g. 3 pages long): reading
   page 10 returns 0xFF instead of aborting.

**Implementation notes.**
- Option (a): replace the `length < position + PAGE + 1` asserts in the
  read path with "short read → fill remainder with 0xFF". In the
  write/erase path, pad with 0xFF to `position` when
  `position > file length`.
- Use `off_t` / `ftello` / `fseeko` (or `pread`/`pwrite` on a fd) for all
  offsets. This overlaps with T5; do T5's offset type change here if it is
  convenient.
- `init_and_load_flash_file`: on a missing file, create it empty
  (`"w+b"`). Delete the 16.5 GB fill loop and the `while (f == NULL)` loop
  (an infinite loop if the re-open keeps failing).
- Run the **full** sparc qtest suite afterwards:
  `make -C build check-qtest-sparc`. It must finish without creating a
  large file in `build/`.

**Acceptance criteria.** Red tests 1–4 pass; `make check-qtest-sparc` is
green and leaves no `flash.bin` larger than 1 MB in `build/`; the FSW boot
smoke and soak still pass with the existing persistent `flash.bin`;
`accepted-diffs.md` documents the lazy-file behaviour.

---

### T4 (P1): STIX qtest not isolated from a running emulator; dead code

**Problem.**
- `tests/qtest/stix.c:105` starts QEMU without `tmtc-port`, so it binds
  21000. If a developer has the emulator running, the whole suite aborts.
  Verified: with 21000 held, `stix` exits rc -6 ("Bind failed... error: 98").
- `tests/qtest/stix.c:91–95`: a loop declaring an unused `zeros_ff` array
  (a no-op leftover).
- The temp flash file is filled with 1 MB of 0xFF by hand. After T3 option
  (a) an empty file is enough.

**Red test.** Wrap the existing suite in a script (or extend
`test-make-check-hermetic.sh` from T3): hold `127.0.0.1:20000+uid`, run
`build/tests/qtest/stix`, and expect 4/4 (plus new tests) ok. Red today.

**Implementation notes.** If D3 is accepted, the qtest needs nothing extra.
Otherwise, pick a free port (bind port 0 on a throwaway socket, read the
port, close it) and pass `-global grlib-stixspw.tmtc-port=<port>`. Remove
the dead loop. Note: the 5.2 wrapper
(`scripts/stix/baseline/qemu-system-sparc-52-wrapper.sh`) strips unknown
`-global` arguments, so any new `-global` must be added to its strip list.

**Acceptance criteria.** The suite passes with port 21000 occupied, and it
still passes against the old build through the wrapper.

---

### T5 (P2): Flash addressing (inherited, latent data corruption)

These do not show up in the 90 s runs (the FSW did not modify `flash.bin`
during them; mtime unchanged). They will show up once the FSW stores data
beyond chip 0 / the first 512 MB.

**Problems** (`hw/sparc/grlib_stixflash.c`):
1. **32-bit offset overflow:** `uint32_t position_in_file = page_address * FLASH_PAGE_SIZE`
   (L129, L152, L183). `page_address` carries the chip number at bit 20
   (`FTNANDCTRL_PAGEADDRESS_CHIPADDRESS`). Chip 1 page 0:
   `(1<<20) * 4224 mod 2^32 = 134,217,728`. That is 128 MB into chip 0, and
   not page-aligned (it straddles chip-0 pages 31775/31776). Writes to
   chip ≥ 1 corrupt chip 0.
2. **Layout:** even in 64 bits, `page_address * PAGE_SIZE` uses a chip
   stride of 2^20 pages, but a chip has 2^18 pages (4096 blocks × 64).
   Chip 7 would map to ~31 GB, past the 16.5 GB file.
3. **MCM ignored:** `cmd_sta` bit 3 (`mcm`) is decoded but never used, so
   MCM 0 and MCM 1 alias.
4. **`ftell` truncation:** `uint32_t length = ftell(f)` (L124, L151) turns
   16.5 GB into 512 MB. Any page past ~127k hits `assert(false)`, and QEMU
   aborts. (The write path L182 uses `uint64_t`, which is inconsistent.)
5. **Asserts reachable by the guest** (L213, L216, L227, L240, L248, L253,
   L296): any unexpected register access order aborts QEMU.
6. Erase (L129) uses `(i + page_address)`. Confirm with the FSW driver that
   `page_address` for erase is the first page of the block.

**Open decision (D4).** Fix 1–4 now (a deviation from baseline, but chip
0/MCM 0 offsets stay identical so existing data is preserved), or defer
until the ground-station work. **Recommendation:** fix 1–4 now, because
silent corruption is worse than a crash. Treat 5 as optional: replace
asserts with `qemu_log_mask(LOG_GUEST_ERROR, ...)` and set the FAILED status
bit.

**Red tests** (qtest, temp flash file; with T3(a) a small file is fine):
1. Write a pattern to chip 1 page 0. Then read chip 0 pages 31775 and 31776
   and check both are still 0xFF. Red today (aliasing).
2. Write pattern A to MCM 0 chip 0 page 5 and pattern B to MCM 1 chip 0
   page 5. Read both back and check A ≠ B is preserved. Red today.
3. Read chip 0 page 200000 (offset ~845 MB, past the 512 MB truncation)
   and check 0xFF with no abort. Red today.
4. Regression: chip 0 MCM 0 page N is still at file offset `N * 4224`.
   Write via QEMU, then check the file contents directly from the test.
   Pins compatibility with the existing `flash.bin`.

**Implementation notes.** A single helper:

```c
static int64_t flash_offset(STIXFLASH *s, uint32_t page_address, uint32_t mcm)
{
    uint64_t chip = (page_address >> FTNANDCTRL_PAGEADDRESS_CHIPADDRESS)
                    & FTNANDCTRL_PAGEADDRESS_CHIPADDRESS_MASK;
    uint64_t page = page_address & FTNANDCTRL_PAGEADDRESS_PAGEADDRESS_MASK;
    uint64_t dev  = mcm * FLASH_DEVICES_PER_MCM + chip;
    return (dev * FLASH_BLOCKS_PER_DEVICE * FLASH_PAGES_PER_BLOCK + page)
           * FLASH_PAGE_SIZE;
}
```

Check: chip is 3 bits (0–7) and there are 8 devices per MCM, so the maximum
offset is exactly `FLASH_TOTAL_STORAGE`. Use 64-bit file offsets everywhere
(`fseeko`/`ftello` or `pread`/`pwrite`). Consider `fflush` after each
write/erase so a QEMU kill does not lose the last page.

**Acceptance criteria.** Tests 1–4 pass; the existing flash qtest passes;
boot smoke and soak pass with the existing persistent `flash.bin`;
`accepted-diffs.md` updated.

---

### T6 (P3): Diagnostics and TM robustness (inherited)

1. **Per-access tracing to stdout.** `qemu_printf` on every SPW/CTRL
   register access (~1 MB per 90 s). With the README's `-nographic` quick
   start, the guest console is interleaved with thousands of trace lines.
   Convert to QEMU trace events (`hw/sparc/trace-events`). They are off by
   default and enabled with `-trace 'stix*'`.
   **Conflict (D5):** `docs/stix-port/baseline/qemu-stdout.golden.log` is a
   manual reference based on this stdout stream. Either keep a
   compatibility mode or regenerate the reference with
   `-trace 'stix*'` and update `baseline/README.md`.
   *Test:* boot with `-nographic` for 10 s and check that stdout contains
   only the UART output (no `SPW |` / `CTRL |` lines).
2. **Blocking TM send on the vCPU thread.** `send()` in `send_tm_packet` is
   called with the BQL held. A client that stops reading eventually blocks
   the whole guest. At ~37 B / 2 s this takes a long time, but a ground
   station could stall. Use `MSG_DONTWAIT | MSG_NOSIGNAL` and drop and
   count packets on `EAGAIN`, or move to QEMU's chardev/`QIOChannel`.
   *Test:* a Python client that connects and never reads, with the
   socket's receive buffer at its minimum. The guest UART log must keep
   growing for 5 min.
3. **Unsynchronised globals.** `clientSocket` is written by the accept
   thread (including `close()` + `-1`) and read by the vCPU thread, so a
   close/reuse race can send TM to an unrelated fd. Use `qatomic_*` plus a
   mutex around close/send, or the QIOChannel route above. Also: raw
   `pthread_create` → `qemu_thread_create`; sockets without `CLOEXEC`
   (use `qemu_socket()` / `qemu_accept()`).
4. **Single-instance globals.** The TMTC server state is file-global, so
   only one SPW device per process works. This is acceptable today (one
   per machine) but worth an `assert`-free check in realize.

---

### T7 (P1): Documentation corrections

| File | Fix |
|---|---|
| `docs/stix-port/baseline/README.md`, "Run rules" | The SCET `assert(0)` is hit on **normal** runs at ~135 s, not only under `-icount`. After T1, rewrite as "implemented". |
| same, "Running the regression suite" step 4 | Until T2 lands, the binary path must be absolute. Afterwards, add `soak.sh` as step 6. |
| `README-STIX.md` "Quick start" | `-nographic` mixes device traces into the console. Recommend `-serial stdio -display none -monitor none` or a file serial until T6.1 lands. Add "known: aborts at ~2 min" until T1 lands. |
| `README-STIX.md` "Known limitations" | Add flash addressing (T5) if deferred. |
| commit `78e7dd8782` "hermetic qtests" | Only `stix.c` is hermetic (flash file). Generic qtests and the TM port are not (T3/T4). Note it in docs; don't rewrite history. |

---

## 5. Decisions needed before implementation

| ID | Question | Recommendation |
|---|---|---|
| D1 | SCET set semantics (T1) | Seconds write keeps the sub-seconds, sub-seconds write keeps the seconds, ns resolution |
| D2 | Flash backing when no file is given (T3) | (a) lazy file, 0xFF beyond EOF |
| D3 | TM server under qtest (T3/T4) | Off under qtest unless `tmtc-port` is set; bind failure → realize error |
| D4 | Fix inherited flash addressing now? (T5) | Yes, items 1–4 |
| D5 | Trace events vs the stdout golden reference (T6) | Trace events; regenerate the reference |

Every deviation from the baseline (T1, T3a, T5, T6.1) gets an entry in
`docs/stix-port/accepted-diffs.md`: what changed, why, and how it was
verified.

## 6. Regression gate (run after every task)

```bash
make -C build -j$(nproc)                                      # no warnings
QTEST_QEMU_BINARY=$PWD/build/qemu-system-sparc build/tests/qtest/stix
make -C build check-qtest-sparc                               # after T3
bash scripts/stix/tests/test-boot-smoke.sh                    # after T2
bash scripts/stix/tests/test-make-check-hermetic.sh           # after T3
bash scripts/stix/boot-smoke.sh build/qemu-system-sparc --with-tm
bash scripts/stix/soak.sh build/qemu-system-sparc 600         # red until T1
# Baseline comparison (device tests against the old build, through the wrapper):
QTEST_QEMU_BINARY=$PWD/scripts/stix/baseline/qemu-system-sparc-52-wrapper.sh \
    build/tests/qtest/stix   # new tests for deviations are expected to fail here; list them
```

## 7. Suggested order

1. **T2** (trustworthy smoke + soak script, so the soak is red).
2. **T1** (soak goes green).
3. **T3** together with **T4** (same root: devices realized in generic qtests).
4. **T7** (docs, now that behaviour is settled).
5. **T5**, then **T6**, after decisions D4/D5.
