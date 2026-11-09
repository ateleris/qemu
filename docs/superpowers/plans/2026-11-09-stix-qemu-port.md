# STIX IDPU QEMU Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the STIX IDPU emulation from QEMU v5.2.0 to v11.1.x on branch `stix-emul-qemu-v11.1`, with golden baseline artifacts captured from the old build *before* any port work, and a three-layer test suite (build gate, device qtests, boot smoke) that passes against the old build first and must stay green through the port.

**Architecture:** Baseline-first port. The old branch (`93a9824b2d`) is rebuilt in an era-correct Docker container from a throwaway git worktree; golden artifacts (UART log on a dedicated serial channel, QEMU device-print stream, trace, `in_asm` head) are committed to the branch. Device qtests are written in the new v11.1 tree and validated against the *old* binary via `QTEST_QEMU_BINARY` before the devices are ported; after the port the same tests must pass against the new binary. The port itself is a like-for-like commit series.

**Tech Stack:** QEMU 11.1.x (meson/ninja, GCC 13), Docker (`ubuntu:20.04`) for the old build, libqtest C tests, bash + Python for capture/normalize/smoke scripts.

**Spec:** `docs/superpowers/specs/2026-11-09-stix-qemu-port-design.md` (read it first; this plan argues from it. The review at `docs/superpowers/specs/2026-11-09-stix-qemu-port-design-review.md` is folded into the spec.)

## Global Constraints

- Old-branch baseline commit: `93a9824b2d` (empirical: if it doesn't boot, walk back to `bf501dd0f9`; record which one in `docs/stix-port/baseline/README.md`).
- Golden runs use `-icount shift=6,sleep=off` — **same flags for old and new builds**; if the baseline capture shows shift=6 is unusable (too slow/fast for the ASW boot), fix a different N once, record it in `docs/stix-port/baseline/run-flags.txt`, and use it everywhere after.
- Guest UART is always captured on its own channel (`-serial file:...`), never mixed with QEMU stdout.
- Every emulator run (golden, smoke, qtest) executes in a scratch working directory with pinned flash state — `grlib_stixflash.c` creates/persists `flash.bin` in its CWD.
- `-Werror` clean on GCC 13; STIX source fixes go in their own commits, never mixed into like-for-like commits.
- TMTC fixed TCP port 2221 (old build); the boot smoke script must check the port is free before starting.
- The FSW tree `/mnt/c/Users/laszl/Development/stix_asw_src` is **strictly read-only** — read `ASW/BSW_ASW/**` and `ASW/ASW/src/**` for driver behavior, never write there; builds of FSW code happen only on a copy outside that tree.
- Git commits carry **no AI attribution lines** (user rule; overrides any default).
- Before any `git push`, run `git fetch origin` first (stale-checkout rule).
- Known-dead at baseline, do not chase as regressions: SPW TC injection (RX callback stubbed), SCET-set writes at CTRL `0xC8`/`0xCC` (`assert(0)`), CTRL IRQ wiring index-0 bug (only `irq_detectors` connected, last-wins to the ROTB CPU IRQ line).
- `origin` carries no v10/v11 tags — fetch upstream (`https://github.com/qemu/qemu.git` as remote `upstream`) to get v11.1.x.

## Review Focus

1. **Flash write-order deviation aborts QEMU** (`assert(addr == 0)`, strict RAM→page→cmd ordering, `assert(0)` on unknown offsets) — a person poking flash registers in any other order expects an error message, not a dead emulator. Pinned by Task 6's protocol test (exact driver order) and Task 3's documented abort list.
2. **Nondeterministic golden compare** (host-speed timing or missing `-icount`) — a person re-running the smoke test expects a stable PASS, not flakes. Pinned by Task 9: smoke script asserts the flags file matches `-icount shift=N,sleep=off` and fails otherwise.
3. **TMTC port 2221 occupied** — the device logs a bind failure but boot continues, so a TM check would silently talk to the wrong instance. Pinned by Task 9: smoke script checks the port is free and aborts if not.
4. **PnP sub-word reads after dropping the STIX byte-access hack** — if upstream `.valid`/`.impl` coverage is assumed but wrong, drvmgr-visible PnP words double-shift and device discovery breaks. Pinned by Task 12's byte/half/word PnP equivalence qtest, run against both builds.
5. **Upstream bootloader/CPU-start changes** (`write_bootloader`, 2024 SMP rework) — a person expecting identical `-kernel` boot expects the same first instructions and UART setup. Pinned by Task 12's `write_bootloader` diff + Task 3's `-d in_asm` head artifact compared in Task 13.

---

### Task 1: Branch and docs commit

**Files:**
- Create: `docs/superpowers/specs/2026-11-09-stix-qemu-port-design.md` (exists, untracked)
- Create: `docs/superpowers/specs/2026-11-09-stix-qemu-port-design-review.md` (exists, untracked)
- Create: `docs/superpowers/plans/2026-11-09-stix-qemu-port.md` (this file)

**Interfaces:**
- Produces: branch `stix-emul-qemu-v11.1` at v11.1.3 + docs commit; all later tasks work on this branch.

- [ ] **Step 1: Fetch upstream v11.1.3**

```bash
cd /home/laszlo/dev/qemu
git remote add upstream https://github.com/qemu/qemu.git 2>/dev/null || true
git fetch upstream v11.1.3 --no-tags
```

If v11.1.3 does not exist upstream, list `git ls-remote upstream 'refs/tags/v11.*'` and use the newest v11.1.x; record the actual tag in the commit message.

- [ ] **Step 2: Create the branch off v11.1.3**

```bash
git switch -c stix-emul-qemu-v11.1 FETCH_HEAD
```

- [ ] **Step 3: Commit the three docs**

```bash
git add docs/superpowers/specs/2026-11-09-stix-qemu-port-design.md \
        docs/superpowers/specs/2026-11-09-stix-qemu-port-design-review.md \
        docs/superpowers/plans/2026-11-09-stix-qemu-port.md
git commit -m "docs: STIX port design, review, and implementation plan"
```

- [ ] **Step 4: Verify**

Run: `git log --oneline -2 && git describe --always`
Expected: docs commit on top of v11.1.3.

---

### Task 2: Old-baseline Docker build

**Files:**
- Create: `scripts/stix/baseline/Dockerfile`
- Create: `scripts/stix/baseline/build-old.sh`
- Create (throwaway, not committed): worktree at `/tmp/qemu-stix-baseline`

**Interfaces:**
- Produces: `/tmp/qemu-stix-baseline/build/qemu-system-sparc` — the old-build binary every later task compares against. Docker image `qemu52-baseline` (tag), reproducible from the committed scripts.

- [ ] **Step 1: Create the throwaway worktree at the baseline commit**

```bash
cd /home/laszlo/dev/qemu
git worktree add /tmp/qemu-stix-baseline 93a9824b2d
```

- [ ] **Step 2: Write `scripts/stix/baseline/Dockerfile`**

```dockerfile
FROM ubuntu:20.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential ninja-build python3 git libglib2.0-dev libpixman-1-dev \
    libfdt-dev flex bison && rm -rf /var/lib/apt/lists/*
```

- [ ] **Step 3: Write `scripts/stix/baseline/build-old.sh`**

Bash script, `set -euo pipefail`, no arguments:
1. `docker build -t qemu52-baseline scripts/stix/baseline/`
2. `docker run --rm -v /tmp/qemu-stix-baseline:/src -w /src qemu52-baseline bash -c 'git submodule update --init meson && mkdir -p build && cd build && ../configure --target-list=sparc-softmmu --disable-slirp --disable-capstone --disable-docs --disable-fdt --disable-guest-agent && make -j"$(nproc)" qemu-system-sparc'`
3. Print the binary path and `/tmp/qemu-stix-baseline/build/sparc-softmmu/qemu-system-sparc --version` (QEMU 5.2 keeps the binary under `build/sparc-softmmu/`).

- [ ] **Step 4: Run the build**

Run: `bash scripts/stix/baseline/build-old.sh`
Expected: ends with `QEMU emulator version 5.2.0 (...)`. If GCC emits hard errors (not just warnings), STOP and report — this decides the empirical baseline question (walk back to `bf501dd0f9` per Global Constraints, re-running Task 2 for that commit).

- [ ] **Step 5: Smoke the binary — does the current ELF boot at all?**

```bash
mkdir -p /tmp/stix-baseline-run && cd /tmp/stix-baseline-run
timeout 60 /tmp/qemu-stix-baseline/build/sparc-softmmu/qemu-system-sparc \
  -M leon3_generic -m 64M -no-reboot -nographic \
  -kernel /mnt/c/Users/laszl/Development/stix_asw_src/ASW/ASW/bin/ASW_em.elf \
  2>&1 | head -30
```
Expected: the STIX banner `STIX ASW_v2, ver.` appears within 60 s. If not, try one `History/` ELF per the spec's fallback order before anything else. **If the current ELF and every `History/` ELF fail to boot, STOP and ask Laszlo** (spec: the fallback chain ends in stop-and-ask — "baseline = whatever the new build does" is circular and has no regression value).

- [ ] **Step 6: Commit**

```bash
cd /home/laszlo/dev/qemu
git add scripts/stix/baseline/
git commit -m "stix: reproducible Docker build for the v5.2 baseline"
```

---

### Task 3: Golden artifact capture

**Files:**
- Create: `scripts/stix/baseline/capture-golden.sh`
- Create: `scripts/stix/normalize-log.py`
- Create: `docs/stix-port/baseline/README.md`, `run-flags.txt`
- Create (artifacts): `docs/stix-port/baseline/uart.golden.log`, `qemu-stdout.golden.log`, `trace.golden.log`, `asm-head.golden.log`

**Interfaces:**
- Consumes: old binary from Task 2.
- Produces: the golden artifacts every later diff runs against; `normalize-log.py` interface used by Task 9/13: `normalize-log.py <in.log> <out.log>` — strips leading `HH:MM:SS.mmm` timestamp prefixes, collapses 3+ blank lines to one, nothing else. `run-flags.txt` holds the exact qemu invocation.

- [ ] **Step 1: Write `scripts/stix/baseline/capture-golden.sh`**

Bash, `set -euo pipefail`, args: `<qemu-binary> <out-dir>`. Body:
1. `PORT_CHECK`: `ss -ltn | grep -q ':2221 ' && { echo "port 2221 busy"; exit 1; }`
2. Scratch run dir via `mktemp -d`; `cd` into it.
3. Run with `timeout 300`:
   `"$QEMU" -M leon3_generic -m 64M -no-reboot -display none -monitor none \`
   `  -serial file:uart.log -kernel "$ASW_ELF" -icount shift=6,sleep=off \`
   `  --trace 'irqmp_*' --trace 'apb_pnp*' -d in_asm -D qemu-debug.log`
   (stdout of the script run = device-print stream; save it: `> qemu-stdout.log 2>&1` on the timeout invocation.)
4. Post-process: `head -200 qemu-debug.log > asm-head.log`; filter trace to `trace.golden.log` (the `-D` file holds trace when `-d` is also used — if trace lands in `qemu-debug.log`, split by prefix; record which in `run-flags.txt`).
5. Assert `grep -q 'STIX ASW_v2' uart.log` — capture fails loudly otherwise.
6. Copy `uart.log qemu-stdout.log trace.golden.log asm-head.log` to `<out-dir>`, write `run-flags.txt` with the full command line.

- [ ] **Step 2: Write `scripts/stix/normalize-log.py`**

Python 3, argparse, two positional args. Rules (only these): strip a leading `^\d{2}:\d{2}:\d{2}\.\d{3} ` timestamp; collapse 3+ consecutive blank lines to one; write result. ~30 lines.

- [ ] **Step 3: Capture against the old build**

Run: `bash scripts/stix/baseline/capture-golden.sh /tmp/qemu-stix-baseline/build/sparc-softmmu/qemu-system-sparc docs/stix-port/baseline`
Expected: exit 0; `uart.golden.log` contains the banner and the `init:` sequence ending past `do: boot report`.

- [ ] **Step 4: Write `docs/stix-port/baseline/README.md`**

Contents: which commit the baseline is, the artifact list, the run flags, the *documented non-issues* (psret patch inert, UART control 3-not-0xf, scaler 39-vs-20MHz, CTRL wiring bug, flash abort paths — from the spec's Baseline truth), and which fallback ELFs exist.

- [ ] **Step 5: Commit**

```bash
git add scripts/stix/ docs/stix-port/
git commit -m "stix: golden baseline artifacts and capture/normalize tooling"
```

---

### Task 4: New-tree build gate

**Files:**
- Create (throwaway): build dir `/home/laszlo/dev/qemu/build`

**Interfaces:**
- Produces: green v11.1.3 build incl. qtest infra; `build/tests/qtest/stix` target name used by Tasks 5–8 and 13.

- [ ] **Step 1: Install build prerequisites**

```bash
pip3 install --user meson ninja  # or apt equivalents if preferred
```
Verify: `meson --version` (QEMU 11.1 needs ≥ 1.1) and `ninja --version`.

- [ ] **Step 2: Configure and build**

```bash
cd /home/laszlo/dev/qemu
mkdir -p build && cd build
../configure --target-list=sparc-softmmu --disable-docs --enable-werror
make -j16
```
Expected: completes with no errors (GCC 13, `-Werror` on). This is the baseline proof that v11.1.3 itself is healthy before any STIX code lands.

---

### Task 5: qtest skeleton + PnP pin

**Files:**
- Create: `tests/qtest/stix.c`
- Modify: `tests/qtest/meson.build` (add to `qtests_sparc`)

**Interfaces:**
- Produces: test binary `build/tests/qtest/qemu-sparc-softmmu/stix`; helper `static void stix_pnp_check(qtest_state, base, word)` used by Task 12. Test names: `/sparc/stix/pnp`.
- Run mechanism for ALL qtest tasks: from a scratch dir, `QTEST_QEMU_BINARY=<old-binary> build/tests/qtest/qemu-sparc-softmmu/stix` (g_test runs sequentially — no port collisions).

- [ ] **Step 1: Register the test in `tests/qtest/meson.build`**

Add `qtests_sparc += {'stix': None}` style entry per the file's existing pattern (find the sparc section; the exact edit is one line plus nothing else).

- [ ] **Step 2: Write the PnP probe-and-pin test in `tests/qtest/stix.c`**

Skeleton per neighboring qtests (e.g. `tests/qtest/prom-env-test.c` for machine args):
- `g_test_init` + `qtest_start("-M leon3_generic")` (no `-kernel`).
- Probe first (throwaway run): `readl(0x800FF000 + 4*i)` for the first ~40 words, print them, and also read the STIX device entries — find the three FTMCTRL-tagged words at/near the CTRL (`0x80000500`), flash (`0x80000800`), SPW (`0x80000A00`) entries by matching against upstream entries (UART/IRQMP/timer at `0x80000100/200/300`).
- Pin: assert the exact 32-bit words for the three STIX entries **as the old build returns them** (values discovered in the probe run, pasted as literals into the assertions with a comment `/* pinned from old build <commit> */`), plus one upstream entry (UART) as a sanity anchor.

- [ ] **Step 3: Build and run against the OLD binary**

```bash
cd build && ninja qemu-sparc-softmmu/stix && cd /tmp && mkdir -p stix-qtest && cd stix-qtest
QTEST_QEMU_BINARY=/tmp/qemu-stix-baseline/build/sparc-softmmu/qemu-system-sparc \
  /home/laszlo/dev/qemu/build/tests/qtest/qemu-sparc-softmmu/stix
```
Expected: `/sparc/stix/pnp` PASS.

- [ ] **Step 4: Commit**

```bash
git add tests/qtest/stix.c tests/qtest/meson.build
git commit -m "stix: qtest skeleton with PnP entry pinning"
```

---

### Task 6: qtest — flash protocol

**Files:**
- Modify: `tests/qtest/stix.c` (add `/sparc/stix/flash`)

**Interfaces:**
- Consumes: flash base `0x80000800`; the exact register write sequence mirrored from the ASW driver (read `ASW/BSW_ASW/**/Flash.c` in the FSW tree — read-only — to extract the RAM-addr → page-addr → cmd/sta register offsets and values; the device asserts otherwise).
- Produces: test name `/sparc/stix/flash`.

- [ ] **Step 1: Extract the driver sequence**

Read the BSW flash driver in the FSW tree; record the register offsets/values it writes for a read-page and an erase-page operation. (Read-only recon; paste the sequence into the test as constants.)

- [ ] **Step 2: Write the test**

Happy path only, in exact driver order: write sequence for one page read → `readl(0x80000800)` returns the expected data word → assert. Then the same for erase (with the erase-complete IRQ observed via the IRQMP pending register at `0x80000200+0` before/after, per the review's IRQMP advice — no `irq_intercept`).
Deliberately NO negative-path tests (unknown offset, wrong order) — those paths are `assert(0)` aborts by design; abort behavior is documented in `docs/stix-port/baseline/README.md`, not tested.

- [ ] **Step 3: Run against the OLD binary** (same mechanism as Task 5)
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add tests/qtest/stix.c
git commit -m "stix: qtest for flash protocol happy path"
```

---

### Task 7: qtest — SPW TX descriptor path

**Files:**
- Modify: `tests/qtest/stix.c` (add `/sparc/stix/spw-tx`)

**Interfaces:**
- Consumes: SPW base `0x80000A00`; descriptor structs (`spwTXdsc_s`: ctrl/haddr/dlen/daddr) from `grlib_stixspw.c`.
- Produces: test name `/sparc/stix/spw-tx`.

- [ ] **Step 1: Read the TX flow in `grlib_stixspw.c` and write the test**

Decision rule (from the design): prefer register-only assertions — memwrite a TX descriptor into guest RAM via `qtest_memwrite`, poke the SPW control register to start transmission, assert descriptor-completion observable (EN bit cleared / IRQMP pending). If, and only if, the device stalls without a connected TCP client, open a localhost socket inside the test before starting TX (old build binds 2221; sequential runs make that safe — note this constraint in a test comment).
No TC-injection tests (RX path is stubbed at baseline — Global Constraints).

- [ ] **Step 2: Run against the OLD binary**
Expected: PASS.

- [ ] **Step 3: Commit**

```bash
git add tests/qtest/stix.c
git commit -m "stix: qtest for SPW TX descriptor path"
```

---

### Task 8: qtest — CTRL registers

**Files:**
- Modify: `tests/qtest/stix.c` (add `/sparc/stix/ctrl`)

**Interfaces:**
- Consumes: CTRL base `0x80000500`; register layout from `stixCtrlRegs_s` in `grlib_stixctrl.c`.
- Produces: test name `/sparc/stix/ctrl`.

- [ ] **Step 1: Write the test**

No `-kernel` → virtual clock is nearly idle → `longTimerSec`/`longTimerSubSec` reads are stable: assert the exact values the old build returns (pin from a probe run, same method as Task 5). Assert a write to a benign register reads back. Deliberately NO write to `0xC8`/`0xCC` (assert-abort path, documented not tested). CTRL IRQ wiring is NOT runtime-observable — it is pinned by the source comment in Task 11, not by a test.

- [ ] **Step 2: Run against the OLD binary**
Expected: PASS.

- [ ] **Step 3: Commit**

```bash
git add tests/qtest/stix.c
git commit -m "stix: qtest for CTRL registers"
```

---

### Task 9: Boot smoke script (old-build gate)

**Files:**
- Create: `scripts/stix/boot-smoke.sh`

**Interfaces:**
- Consumes: `capture-golden.sh` flags, `normalize-log.py`, golden artifacts, old binary.
- Produces: the acceptance gate reused in Task 13 against the new build. Interface: `boot-smoke.sh <qemu-binary>` — exit 0 = normalized UART matches golden; exit 1 = diff (prints it); exit 2 = environment problem (port busy, flag drift); exit 0 with SKIP notice if `STIX_ASW_ELF` is unset.

- [ ] **Step 1: Write the script**

Bash, `set -euo pipefail`:
1. If `${STIX_ASW_ELF:-}` unset → print SKIP, exit 0.
2. `ss -ltn | grep -q ':2221 '` → abort exit 2 with message.
3. Flag-drift tripwire: `grep -q -- '-icount shift=6,sleep=off' docs/stix-port/baseline/run-flags.txt || { echo "run-flags.txt drifted from the pinned -icount"; exit 2; }` (Review Focus #2 — this check is what makes golden compares stable; update both files together if N ever changes).
4. Scratch dir; run exactly the Task 3 invocation (flags hardcoded here, matching `run-flags.txt` per the tripwire above).
5. Normalize both logs; `diff -u`; on difference print it and exit 1.

- [ ] **Step 2: Run against the OLD binary**
Run: `STIX_ASW_ELF=/mnt/c/Users/laszl/Development/stix_asw_src/ASW/ASW/bin/ASW_em.elf bash scripts/stix/boot-smoke.sh /tmp/qemu-stix-baseline/build/sparc-softmmu/qemu-system-sparc`
Expected: exit 0 (self-compare). This closes the baseline phase — the hard gate opens.

- [ ] **Step 3: Commit**

```bash
git add scripts/stix/boot-smoke.sh
git commit -m "stix: boot smoke script, baseline gate green"
```

---

### Task 10: Port groundwork — headers, build wiring

**Files:**
- Create: `hw/sparc/StixIdpu.h` (from old branch, verbatim)
- Create: `include/hw/sparc/grlib_stix.h` — declares `TYPE_GRLIB_STIXCTRL`, `TYPE_GRLIB_STIXFLASH`, `TYPE_GRLIB_STIXSPW`, `TYPE_GRLIB_STIXWD`
- Modify: `hw/sparc/meson.build` (add the four device files + tmtc_io_server.c when they land)
- Modify: `hw/sparc/Kconfig` (verify STIX files build under `CONFIG_GRLIB`; add a select only if the build says otherwise)

**Interfaces:**
- Produces: `TYPE_GRLIB_STIX*` macros consumed by Task 11/12; `StixIdpu.h` include path used by every device file.

- [ ] **Step 1: Copy `StixIdpu.h` from the old branch**

```bash
git show 93a9824b2d:hw/sparc/StixIdpu.h > hw/sparc/StixIdpu.h
```

- [ ] **Step 2: Write `include/hw/sparc/grlib_stix.h`** — the four `TYPE_*` defines + `OBJECT_DECLARE_SIMPLE_TYPE` usage stays in the .c files as upstream style dictates; header holds only the `TYPE_` macros (single STIX header, documented as STIX-local, not for upstream).

- [ ] **Step 3: Build**

Run: `cd build && ninja qemu-sparc-softmmu`
Expected: green (headers alone change nothing yet).

- [ ] **Step 4: Commit**

```bash
git add hw/sparc/StixIdpu.h include/hw/sparc/grlib_stix.h hw/sparc/meson.build hw/sparc/Kconfig
git commit -m "stix: IDPU memory map and device type declarations"
```

---

### Task 11: Port flash device

**Files:**
- Create: `hw/sparc/grlib_stixflash.c` (from old branch)
- Modify: `include/hw/sparc/grlib_stix.h` if realized types need it
- Modify: `hw/sparc/meson.build`

**Interfaces:**
- Produces: `TYPE_GRLIB_STIXFLASH` realized device; `flash-file` qdev property (default `"flash.bin"` — the sanctioned non-guest-visible deviation from like-for-like, per spec).

- [ ] **Step 1: Port the file**

`git show 93a9824b2d:hw/sparc/grlib_stixflash.c > hw/sparc/grlib_stixflash.c`, then fix for v11.1: `DEVICE_NATIVE_ENDIAN` → `DEVICE_BIG_ENDIAN`; add the `flash-file` property (DEFINE_PROP_STRING, default `"flash.bin"`, used for the fopen path); adjust include paths per current tree. Keep all asserts and behavior identical — the qtest pins them.

- [ ] **Step 2: Build**

Run: `cd build && ninja qemu-sparc-softmmu`
Expected: green under `-Werror`. If STIX-source warnings appear, fix them in this commit only if they are in this file; otherwise they wait for the -Werror sweep in Task 12.

- [ ] **Step 3: qtests still pass against the OLD binary** (they must — devices aren't instantiated yet)
Run: Task 5 mechanism. Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add hw/sparc/grlib_stixflash.c hw/sparc/meson.build include/hw/sparc/grlib_stix.h
git commit -m "stix: port flash device to QEMU 11.1"
```

---

### Task 12: Port SPW + TMTC server, CTRL, WD; leon3.c wiring; shared diffs

The heart of the port. One task because the pieces share `leon3.c` and are only meaningfully testable together; commits inside stay one-per-concern.

**Files:**
- Create: `hw/sparc/grlib_stixspw.c`, `hw/sparc/tmtc_io_server.c`, `hw/sparc/tmtc_io_server.h`, `hw/sparc/grlib_stixctrl.c`, `hw/sparc/grlib_stixwd.c` (all from old branch)
- Modify: `hw/sparc/leon3.c` (device instantiation, `CPU_CLK` 40→20 MHz, psret/psref/wim patch adapted to the new CPU-start flow, PnP entries — with the five `sysbus_connect_irq(dev, 0, ...)` calls kept **as found**, plus a comment: `/* Deliberately preserved baseline bug: all five CTRL outputs connect via line 0, last-wins to ROTB. See docs/stix-port/baseline/README.md */`)
- Modify: `hw/char/grlib_apbuart.c` (reset control `= 0xf` carried over)
- Modify: `hw/misc/grlib_ahb_apb_pnp.c` (byte-access hack: keep or drop per Step 3)
- Modify: `hw/sparc/grlib_stixspw.c` → `tmtc-port` qdev property (default `2221`)

**Interfaces:**
- Consumes: `TYPE_GRLIB_STIX*` (Task 10), qtest suite (Tasks 5–8), golden artifacts (Task 3).
- Produces: the complete STIX machine; `boot-smoke.sh <new-binary>` as the acceptance interface for Task 13.

- [ ] **Step 1: Port SPW + TMTC server** (one commit)
  - Files from old branch; fix `qemu_printf(stderr, ...)` misuse and any other `-Werror` issues **in a separate preceding commit** (`fixup: stix tmtc/spw printf misuse` — like-for-like commits stay readable).
  - `DEVICE_BIG_ENDIAN` for SPW ops; add `tmtc-port` property (default `2221`), threaded through `init_tmtc_server`.
  - Build green; qtests vs old binary PASS.

- [ ] **Step 2: Port CTRL + WD** (one commit)
  - CTRL as-is (assert path and live timer-read behavior preserved); WD file compiles but stays **commented out** of `leon3.c` exactly as at baseline.
  - Build green; qtests vs old binary PASS.

- [ ] **Step 3: Port leon3.c wiring + shared diffs** (one commit each)
  - `leon3.c`: reapply the instantiation block (CTRL five IRQ lines *as found*, flash, SPW), `CPU_CLK` 20 MHz, PnP entries (`GRLIB_FTMCTRL`, version 0, exact words), and the psret/psref/wim semantics (`if (env->pc) { psret=1; psref=1; wim=2; }`) adapted to wherever v11.1 sets the CPU start state (`leon3_start_cpu` flow — read the current file first).
  - `grlib_apbuart.c`: reset control `= 0xf`.
  - PnP byte-hack decision: read v11.1's `grlib_ahb_apb_pnp.c` `.valid`/`.impl` — if sub-word access is now handled upstream, **drop the STIX hack**; if not, port it. Either way, extend `tests/qtest/stix.c` with byte/half/word reads of known PnP entries and require PASS against **both** old and new binaries (Task 5 mechanism, then default binary).
  - `write_bootloader()` diff (spec C1): `git diff 93a9824b2d v11.1.3 -- hw/sparc/leon3.c` focused on the bootloader/reset path; if anything FSW-visible changed, record it in `docs/stix-port/accepted-diffs.md`.
  - Build green; qtests vs old PASS; qtests vs new (default `QTEST_QEMU_BINARY` → new binary) PASS.

- [ ] **Step 4: Commit series**

```bash
git add -A hw/sparc hw/char/grlib_apbuart.c hw/misc/grlib_ahb_apb_pnp.c tests/qtest/stix.c
git commit -m "stix: port SPW/TMTC, CTRL, WD; wire leon3 machine; shared diffs"
```
(one commit per sub-step as noted above, in order)

---

### Task 13: Acceptance — new build passes the baseline gates

**Files:**
- Create: `docs/stix-port/accepted-diffs.md` (only if differences exist)

**Interfaces:**
- Consumes: everything.
- Produces: the accepted-port state; the interface `boot-smoke.sh build/sparc-softmmu/qemu-system-sparc` for future regression runs.

- [ ] **Step 1: Full build gate**

Run: `cd build && make` (with `-Werror` already configured in Task 4)
Expected: green.

- [ ] **Step 2: qtest suite against the NEW binary**

Run: scratch dir; default `QTEST_QEMU_BINARY` (new binary): `build/tests/qtest/qemu-sparc-softmmu/stix`
Expected: all four tests PASS.

- [ ] **Step 3: Boot smoke against the NEW binary**

Run: `STIX_ASW_ELF=... bash scripts/stix/boot-smoke.sh build/sparc-softmmu/qemu-system-sparc`
Expected: exit 0. On diff: analyze each difference; FSW-relevant ones go into `docs/stix-port/accepted-diffs.md` with a one-line why; accepted only by Laszlo (per spec). Re-run until exit 0 or Laszlo accepts the documented diff.

- [ ] **Step 4: `-d in_asm` head comparison** (Review Focus #5)

Capture `asm-head.log` from the new build with capture-golden.sh flags and diff against `asm-head.golden.log`. Differences here are bootloader/CPU-start changes — enumerate, don't silently accept.

- [ ] **Step 5: Commit**

```bash
git add docs/stix-port/
git commit -m "stix: acceptance run against golden baseline"
```

---

### Task 14: TM probe (settles the console-only question) and push

**Files:**
- Create: `scripts/stix/tmtc-probe.py`
- Create: `docs/stix-port/baseline/tm.golden.log` (only if TM observed)

**Interfaces:**
- Produces: definitive answer to "does the old model emit TM at all" (Laszlo recalls console-only — the probe verifies); pushed branch. TM-out itself is preserved for the future ground-station attachment (out of scope here).

- [ ] **Step 1: Write `tmtc-probe.py`**

Python 3, stdlib only: connect `localhost:2221` immediately after QEMU start, send the 12-byte token `b"STIX_CONTROL"` (vestigial, sent for fidelity), then read the socket for N seconds (default 120, argparse); each read: 4-byte TSIM header (length prefix) + payload; log to stdout as hex lines with arrival timestamps. Tolerate server-side disconnect and empty reads without error.

- [ ] **Step 2: Probe the OLD build — expected finding: no TM**

Run qemu per `run-flags.txt` with the probe attached from the start, let the boot run past `do: boot report` plus the HK period. **Expected outcome per Laszlo's recollection: zero TM bytes** (console-only model). No TM → record that in `docs/stix-port/baseline/README.md` ("TM-out code present but never exercised end-to-end; ground-station attachment is a future project"), skip any smoke extension. TM observed → save as `tm.golden.log`, extend `boot-smoke.sh` with a `--with-tm` mode asserting non-empty TM, and note the recollection was wrong.

- [ ] **Step 3: Commit**

```bash
git add scripts/stix/tmtc-probe.py docs/stix-port/
git commit -m "stix: TM probe results (console-only baseline confirmed)"
```

- [ ] **Step 4: Fetch, then push**

```bash
git fetch origin
git push -u origin stix-emul-qemu-v11.1
```
