# Phase 1 acceptance checklist

2026-11-10. Template: `2026-11-10-phase1-detailed-plan.md` §7. Corrections:
`2026-11-10-phase1-plan-corrections.md` (they win over the plan where the two differ).

Every item gives the command to re-run, or the file to re-read, together with the expected
result. The items marked [x] were checked by the orchestrator in this session. The
accepting reviewer re-runs them.

## State

- [x] **uprev tip** = `1b14485480`. Check with
  `git -C /home/laszlo/dev/qemu-stix-uprev log --oneline 723622bedb..stix-emul-qemu-uprev`.
  - **Picks from v11.1, in order:**
    - `f8df2e8d91` … `3e97493fce` (the v11.1 line, rebased);
    - `8a69240620`, the soak gate.
  - **Phase 1 commits:**

    | Step | Commit |
    |---|---|
    | B1 | `2730537d7d` |
    | B2 | `a39152d73a` |
    | D-I | `21f898484f` |
    | R3 | `5cda8ac82e` |
    | A1 | `47fa3a7232` |
    | A2 | `2dd19d4a9e` |
    | docs (GS) | `688510df3b` |
    | A3 | `1b14485480` |

- [x] **Worktrees and branches** (`git worktree list`):
  - `qemu-stix-flash` [stix-uprev-flash @ 21f898484f]
  - `qemu-stix-r3` [stix-uprev-r3 @ 5cda8ac82e]
  - `qemu-stix-rotb` [stix-uprev-rotb @ 1b14485480]
  - `qemu-stix-uprev` [stix-emul-qemu-uprev @ 1b14485480]
  - `/tmp/qemu-stix-baseline`, the 5.2 baseline build, not ours.
- [x] **Backup ref unchanged.** `git rev-parse stix-emul-qemu-uprev-backup-20261110` → `1f42ecb32e…`.
- [x] **Main tree unchanged.** `git -C /home/laszlo/dev/qemu status --short` shows the same as at
  session start:
  ```
   M hw/sparc/grlib_stixctrl.c
   M hw/sparc/trace-events
   M tests/qtest/stix.c
  ?? docs/stix-port/2026-11-10-r3-watchdog-status.md
  ```
- [x] **No AI trailers.** `git log --format=%B 723622bedb..stix-emul-qemu-uprev | grep -ci co-authored` → `0`.
- [x] **Baseline `flash.bin` untouched.** `stat -c '%s %y' /tmp/stix-baseline-run/flash.bin` →
  `17716740096 2026-11-09 15:30:10.455927715 +0100`.

## Per commit

Each commit message carries its own "Red -> green:" lines. Read them with `git show -s <sha>`.

- [x] **`2730537d7d` B1: flash controller error contract.**
  - Red: `flash-error-paths` hits SIGABRT at `assert(write_counter == 1)`, `grlib_stixflash.c:240`, on its first
    out-of-order PAGEAD write.
  - Green: 6/6 steps pass.
  - Files: flash.c, trace-events, stix.c, accepted-diffs.md (all within the zone).
  - accepted-diffs **§7**.
- [x] **`a39152d73a` B2: flash addressing.**
  - Red: `flash-addressing` fails in three ways:
    - the chip-1 write lands at the raw-register offset;
    - an MCM1 write aliases onto MCM0;
    - an erase of page 10 wipes pages 10–73 (memcmp 85).
  - Green: 5/5 steps pass.
  - Files: flash.c, stix.c, accepted-diffs.md.
  - accepted-diffs **§8**. Correction 1B-1 (page 10) is applied.
- [x] **`21f898484f` D-I: sparse, inverted flash storage.**
  - Red: `flash-storage` fails because the binary rejects the unknown property `inverted-storage`.
    Because the B2 binary would write 16.5 GB, the missing-file case was left out of the red run.
  - Red: `flash` reads 0x00 from a hole.
  - Red: `flash-addressing` fails its host-view memcmp.
  - Green: all three tests pass.
  - accepted-diffs **§9**.
- [x] **`5cda8ac82e` R3: CTRL watchdog (0xD4) and the regenerated golden.**
  - Red: `ctrl-watchdog` reads COUNT 0 after RELOAD; it expects 63.
  - Green: the full command set passes, including the 105 ms tick, the missed feed resetting at about 6.6 s, and FORCE.
  - Golden regenerated in `docs/stix-port/baseline/` from the normal ELF. `run-flags.txt` records sha256 `6bf0af10…`.
  - accepted-diffs **§10**.
- [x] **`47fa3a7232` A1: CTRL IRQ lines.**
  - Red: `ctrl-irq-lines`: `sysbus-irq[0]` points to `unnamed-gpio-in[7]`, and outputs 1–4 are unconnected.
  - Green: outputs 0–4 connect to IRQMP lines 3–7.
  - Files: leon3.c, stix.c, accepted-diffs.md, baseline README.
  - accepted-diffs **§11**.
- [x] **`2dd19d4a9e` A2: ROTB ring.**
  - Red tests: `rotb-fifo`, `rotb-window`, `rotb-full-irq`, `rotb-selftest-shape` (a bounded loop with no hang),
    `rotb-strobe` and `rotb-reset`. Each red reason is in the commit message.
  - `ctrl-rotb` passes unchanged.
  - Green: 18/18.
  - accepted-diffs **§12**.
  - The commit message says boot-smoke and soak "must confirm". The results they confirmed are recorded under Gates.
- [x] **`1b14485480` A3: AHB windows.**
  - Red: `ahb-windows` reads 0 after writing 0x10101010 to 0xE0000000. `bram-persist` reads 0 after the reset.
  - Green: 20/20.
  - Files: ctrl.c, leon3.c, stix.c, accepted-diffs.md.
  - accepted-diffs **§13**.
- [x] **Red runs reproduced independently.** For every commit, the orchestrator rebuilt with the device files from
  HEAD~ and got the same first failure as the worker.

## Gates (commands to re-run)

```bash
W=/home/laszlo/dev/qemu-stix-uprev
export PATH=/home/laszlo/venvs/qemu-build/bin:$PATH
make -C $W/build -j8 qemu-system-sparc tests/qtest/stix   # -Werror build
```

- [x] **Build:** clean with `-Werror`, binary built at 1b14485480.
- [x] **Full suite, under the lock:** 20/20 ok. Command:
  `flock /tmp/stix-qtest-21000.lock env QTEST_QEMU_BINARY=$W/build/qemu-system-sparc $W/build/tests/qtest/stix --tap`.
  The tests are:
  - pnp, flash, flash-error-paths, flash-addressing, flash-storage
  - spw-tx, ctrl, ctrl-scet, ctrl-rotb, gptimer-prescaler, ctrl-watchdog, ctrl-irq-lines
  - rotb-fifo, rotb-window, rotb-full-irq, rotb-selftest-shape, rotb-strobe, rotb-reset
  - ahb-windows, bram-persist
- [x] **Targeted runs:** each new test passes on its own with `-p /sparc/stix/<t>`.
- [x] **5.2 wrapper.** Run it from `/tmp/stix-uprev-qtest-scratch`, which holds the 1 MB 0xFF `flash.bin`, one test at a time:
  `-p /sparc-52-wrapper.sh/sparc/stix/<t>`. The test path takes its prefix from the binary name, so `-p /sparc/stix/<t>`
  matches nothing.
  - **Expected green:** pnp, flash, spw-tx, ctrl.
  - **Expected red:** all 12 new tests, plus the pin fixes ctrl-scet, ctrl-rotb, gptimer-prescaler and ctrl-watchdog.
  - **Actual:** exactly as expected.

    | Test | First failure on 5.2 |
    |---|---|
    | flash-error-paths | 5.2 aborts at `grlib_stixflash.c:238` |
    | flash-addressing | 5.2 aborts at `grlib_stixflash.c:185` |
    | flash-storage | `stat(path)` fails |
    | ctrl-scet | 5.2 aborts at `grlib_stixctrl.c:97` |
    | ctrl-rotb | 0x54 reads 0xffffff… |
    | gptimer-prescaler | the reload value differs |
    | ctrl-watchdog | COUNT is 0, not 63 |
    | ctrl-irq-lines | CTRL path not found (5.2 QOM layout) |
    | rotb-*, ahb-windows, bram-persist | no `rotb-blocks` property; they fail at the probe, before the behavior check |

  - Disk delta: 0 MB. The aborted tests leave `/tmp/stix-flash-*` behind; they were removed afterwards.
- [x] **Boot-smoke:** PASS against the R3 golden after A2 and again after A3. No new UART line.
  - ELF: `/tmp/stix-baseline-run/ASW_em_baseline.elf` (sha256 6bf0af10…).
  - Run dir: `/tmp/stix-uprev-run`, with a sparse D-I `flash.bin` of about 16 KB used.
  - Command: `STIX_RUN_DIR=/tmp/stix-uprev-run bash $W/scripts/stix/boot-smoke.sh $W/build/qemu-system-sparc`.
- [x] **Soak** (`scripts/stix/soak.sh`, 60 s TM slices). Every run passed:

  | After | Length | TM chunks per slice |
  |---|---|---|
  | R3 | 600 s | 56/56 |
  | A2 | 300 s | 56/55 |
  | A3 | 300 s | 54/56 |

  The UART was silent after boot each time.

## Decisions and deviations

- [x] **D-F** (IRQ 7 only for a PUTBLOCK into a full ring, which is dropped): `grlib_stixctrl.c:238-249`. Window writes
  while full or outside TESTMODE are ignored without an IRQ: `:286-287`.
- [x] **D-G** (the windows survive system reset, while the ROTB pointers clear and its content is kept):
  realize `:458`, reset `:512-516`.
- [x] **D-H** (EOF rule):
  - reads past EOF return 0xFF, erases succeed without change, writes set FAILED and never extend the file;
  - helper `flash_page_beyond_eof` at `grlib_stixflash.c:127`, used at `:272/:312/:358`;
  - `flash_fail` at `:159-166`.
- [x] **D-I** (sparse, inverted storage; the user chose option 1):
  - property `inverted-storage` (default on), at `grlib_stixflash.c:650`;
  - `inverted-storage=off` keeps plain legacy files usable;
  - a missing file is created by `ftruncate`.
- [x] **Corrections addendum:**
  - 1B items 1–4: applied as written.
  - 1A items 1–3 and 5–12: applied as written.
  - **1A item 4 was rejected.** The validator proposed an IRQ per rejected window write. D-F counts rejected
    PUTBLOCKs, and the FSW fills a block with byte and word stores, so an IRQ per write would mean hundreds per block.
- [x] **Deviations from the plan:**
  - **Integration order.** R3 and the soak gate went directly onto uprev, not through cherry-picks from v11.1
    (correction 1A-1).
  - **D-I is a new decision.** It came out of the full-size file problem, after the user approved option 1.
  - **The flash track grew to three commits** (B1, B2, D-I).
  - **A3 error handling.** The orchestrator replaced the worker's `if (*errp)` checks with one
    `if (!memory_region_init_ram(...) || ...) return;`, the QEMU bool-return idiom.
  - **A2 commit message.** It still says the gates "must confirm", because the message edit did not apply. The
    results are recorded here, and the history was not rewritten.
  - **Implementation review.** The orchestrator did it: diff review, independent red/green re-runs, and gates.
    Plan §6.2 asked for a separate reviewer subagent with `drafts/…-review.md` files; those files were not written.
  - **`688510df3b`** (ground-station docs and scripts) sits between A2 and A3 and touches only docs/ and scripts/.

## Open items handed back

1. **HK ADC model gap.** The HK ADC reads 0, so the HKMonitor line checks fail (`HKMonitor.c:489/730` →
   `FDIR.c:1274`), and the FSW can emit TM(5,4) power-down events. This is not a Phase 1 regression; it belongs to the mock roadmap.
2. **Watchdog RELOAD while DISABLEd.** We load COUNT and run it. The FSW never does this, and hardware behavior is
   unconfirmed (accepted-diffs §10).
3. **BRAM low-16-bit storage is not modeled** (accepted-diffs §13). This is a fidelity gap.
4. **qtest temp files leak on failure.** Failing tests leave `/tmp/stix-flash-*` behind. Cleanup is
   `rm -rf /tmp/stix-flash-*`. A g_test cleanup hook would fix it.
5. **Ground station** (branch `stix-qemu-integration`, `ATS-GroundStation-stix`):
   - The HTML escaping fix `8990d41` also applies to GS `main`. Decide whether to cherry-pick it.
   - The definition store re-reads its file for every packet.
   - Do bit fields, calibrations, the PUS key and the serializer guard go to `main`?
6. **Phase 2 TC inputs.** The SpW RX path is a stub. TC needs:
   - a `stix_pus` serializer;
   - a CRC-16/CCITT-FALSE trailer;
   - time sync, because the CUC "not synchronised" bit is set.

## Cleanup commands (run only after acceptance)

```bash
git -C /home/laszlo/dev/qemu-stix-uprev worktree remove /home/laszlo/dev/qemu-stix-flash
git -C /home/laszlo/dev/qemu-stix-uprev branch -d stix-uprev-flash
git -C /home/laszlo/dev/qemu-stix-uprev worktree remove /home/laszlo/dev/qemu-stix-rotb
git -C /home/laszlo/dev/qemu-stix-uprev branch -d stix-uprev-rotb
# qemu-stix-r3: keep it until the GS docs point at the uprev build; then remove it the same way
rm -rf /tmp/stix-uprev-qtest-scratch /tmp/stix-flash-*
# keep /tmp/stix-uprev-run and /tmp/stix-baseline-run (persistent flash images)
```
