# Review: STIX IDPU QEMU Port — Design

**Reviewed:** `2026-11-09-stix-qemu-port-design.md` (draft)
**Date:** 2026-11-09
**Method:** every factual claim I could check was checked against
`origin/stix-emul-qemu-v5.2.0` (tip `93a9824b2d`), the `v5.2.0` tag, and the
local `master` checkout (v6.2.0-rc4). Things I could *not* verify (no v11 tags
locally, no network) are marked as such.

## Verdict

The plan is sound in shape: baseline first, like-for-like port, tests proven on
the old build before they judge the new one. Most of the "baseline truth" is
accurate. But two of the four "load-bearing" shared-file diffs are in fact
overridden on the documented run path, and the plan misses the upstream
`-kernel` bootloader stub, which is the biggest boot-path risk. It also misses
a few things that will break the golden-log and qtest approaches as written
(device debug prints mixed into the UART stream, asserts in the device models,
files written to the working directory, a fixed TCP port). Fix those before
writing the implementation plan.

## What checks out

- Tip `93a9824b2d`, 22 STIX commits + 1 merge over `v5.2.0`, no unrelated
  patches. Diffstat matches (16 files, ~3.1k lines).
- APBUART reset `control = 0xf`, PnP byte-access hack (`min_access_size = 1`
  plus a shift), `CPU_CLK` 40→20 MHz, `main_cpu_reset` psret/psref/wim block.
- SCET-set write path: `assert(0)` on `0xC8`/`0xCC` in `grlib_stixctrl.c`.
- `flash.bin` opened from CWD via `fopen`, created if missing.
- TMTC server: `"STIX_CONTROL"` defined, never compared. `accept()` runs in a
  pthread, so it does not block boot (good for the smoke test).
- WD compiled under `CONFIG_GRLIB` but commented out of `leon3.c`.
- `ASW_em.elf` exists at the stated path (5.4 MB, 2026-02-09).

## Critical — fix in the plan before building on it

### C1. The upstream `-kernel` bootloader stub overrides two "load-bearing" diffs

`leon3.c` has had `write_bootloader()` since 2019 (`dbed0d2d2a`), so it is in
the 5.2 baseline too. With `-kernel` and no `u-boot.bin` (the historical
recipe), QEMU writes a small stub into PROM, sets `reset_info->entry = 0`,
and starts the CPU at address 0. The stub writes UART control `= 3`, sets the
GPTIMER scaler to `39`, then jumps to the ELF entry. As a result:

- **The psret/psref/wim patch never runs.** It is guarded by
  `if (env->pc)`, and `pc` is 0 (PROM) on this path. The code exists but
  does nothing for `-kernel` boots.
- **The APBUART `control = 0xf` reset value is overwritten by the stub's
  `3` (RE|TE, interrupts off) before the guest's first instruction.** The
  guest never sees `0xf` on this path. Only qtest (no kernel, so no stub)
  can observe it.
- **The stub hard-codes the scaler for 40 MHz (`40 - 1`)**, but the STIX
  machine runs GPTIMER at 20 MHz. If the RTEMS 4.10 LEON3 BSP trusts the
  bootloader's scaler (as it normally trusts mkprom), guest time runs at
  half speed. This happens in both old and new builds, so it is not a
  regression. But it is the kind of thing that changes silently if upstream
  ever derived the scaler from `CPU_CLK`.

**Ask:** reword "load-bearing" to "carry faithfully, but inert on the
`-kernel` path". Add an explicit task: *diff `write_bootloader()` and the
`-kernel` reset flow between v5.2.0 and v11.1.x* (the 2024 SMP work touched
CPU start-up). This is the most likely place for an FSW-observable change the
plan doesn't list. The `-d in_asm` output of the first ~20 instructions is a
cheap extra golden artifact that catches it.

### C2. The golden UART log is mixed with QEMU's own debug prints

Every STIX device calls `qemu_printf` on almost every register access
(`CTRL | Read requested ...`, `SPW | Write completed ...`, flash file
messages, TMTC "Waiting for TC client..."). With `-nographic`, guest UART
output and these prints share stdout. CTRL reads also print SCET values taken
from the virtual clock, which change between runs.

**Ask:** capture the guest UART on its own channel
(`-serial file:uart.log` or `-chardev file,...`, plus `-display none
-monitor none`) and keep QEMU stdout/stderr as a second artifact. The
normalizer then only has to handle guest content. Keep the device-print
stream too: it is a free register-access trace of the old model, and arguably
a better regression signal than the trace points the plan wants.

### C3. Determinism isn't addressed

The normalizer is expected to absorb "racy lines". But guest timing depends
on host speed unless `-icount` is used, and CTRL derives registers from
`QEMU_CLOCK_VIRTUAL`. **Ask:** decide up front whether golden runs use
`-icount shift=N,sleep=off` (deterministic, slightly different timing
behavior) and use the same flags for old and new. Without this, "matches the
golden baseline" will be flaky in ways that are hard to tell apart from
regressions.

## Important — the test design needs adjusting

### I1. Device models `assert()` on access patterns a generic qtest will hit

- `grlib_stixflash_read`: `assert(addr == 0)` and
  `assert(write_counter == 0)`. Reading any register other than offset 0
  aborts QEMU.
- `grlib_stixflash_write`: an exact RAM-addr → page-addr → cmd/sta write
  order is enforced by asserts. Any other order, or an unknown offset,
  aborts.
- `grlib_stixctrl_read/write`: index `reg[addr/4]` with no bounds check
  against the MMIO window. A sweep of the 0x1000 window could read or write
  past the struct. That is undefined behavior, so it can differ between the
  old and new builds and between compilers.

"Reset values for all registers" as a qtest inventory will crash or produce
garbage. **Ask:** derive the qtest inventory from what the *ASW driver
actually does*, not from `StixIdpu.h` wholesale, and list the abort paths
next to the "known-dead paths". Then decide case by case whether an assert
is behavior to preserve (like-for-like) or a bug to fix in a separate,
labeled commit.

### I2. STIX CTRL IRQ wiring is wrong in the baseline. Decide whether to keep it

`leon3.c` calls `sysbus_connect_irq(dev, 0, ...)` five times with index **0**
each time. The last call wins, so CTRL's `irq_detectors` output goes to CPU
IRQ **7** (ROTB) and outputs 1–4 are unconnected. CTRL never raises an IRQ
today, so this is latent. **Ask:** record it in the baseline notes. A
like-for-like port keeps it, and a qtest that asserts "CTRL IRQ wiring as
found" stops someone from fixing it silently later. Also check that
reconnecting the same GPIO doesn't hit a new assertion in v11.1's qdev code
(I haven't verified this).

### I3. The PnP entries are strange. Pin them exactly

CTRL, flash and SPW are all registered as `GAISLER / GRLIB_FTMCTRL (0x054)`,
version 0, with the IRQ field taken from their IRQ macro. drvmgr will see
three FTMCTRLs. That is the behavior to preserve, so the PnP qtest should
assert these exact words (including the AHB/APB entries upstream adds), not
"plausible" values.

### I4. Upstream PnP access-width changes can conflict with the STIX hack

The plan lists upstream's "APB PnP access widths" fix as a benefit. The STIX
hack (byte reads by shift, `min_access_size = 1`) does the same job and will
conflict. If upstream now handles sub-word access through `.valid`/`.impl`,
**drop the STIX hack and prove the result is the same with a qtest doing
byte, half-word and word reads of known PnP entries, run against both
builds.** Keeping both would double-shift.

### I5. qtest and smoke runs aren't hermetic

- `flash.bin` lands in the CWD of whatever spawns QEMU. For qtest that is
  the build dir, and it persists between runs. The plan's scratch-dir rule
  only covers "emulator runs". **Ask:** extend it to qtest (chdir to a
  `g_dir_make_tmp` before `qtest_init`), or add a `flash-file` property that
  defaults to `"flash.bin"`. The default keeps old behavior; the property
  isn't guest-visible.
- The TMTC server binds fixed port **2221** at SPW realize, in every QEMU
  instance including qtest. Parallel runs, such as the boot smoke test next
  to a developer's QEMU, will fail to bind. That failure is logged but not
  fatal, and the TMTC test then talks to the wrong instance. **Ask:** the
  boot/TMTC scripts should check that the port is free, or the port gets a
  property with the same default.

### I6. Running "the whole suite against the old build" needs a concrete mechanism

The qtest sources will live in the v11.1 tree. The old tree has a different
meson/libqtest. The practical route is to build the qtest binary once from
the new tree and run it with `QTEST_QEMU_BINARY=<old qemu-system-sparc>`.
The qtest wire protocol has been stable enough across these versions for
plain `readl`/`writel`/`irq_intercept` tests. Say so in the plan, so the
builder doesn't try to back-port tests into 5.2. For IRQ checks, prefer
reading the IRQMP pending register over `irq_intercept_in` on CPU lines:
it's simpler and works the same on both builds.

## Minor

- **`-Werror` gate will need STIX source fixes**: e.g.
  `qemu_printf(stderr, "%s: Not valid packet received\n")` in
  `grlib_stixspw.c` passes a `FILE *` as the format and is missing an
  argument. That's allowed ("compilation requires"), but put such fixes in
  their own commit so the like-for-like commits stay readable.
- **The ELF fallback's last resort is circular.** "Baseline = current ELF's
  behavior *on the new build*" means there is no regression reference at
  all. Make that a stop-and-ask point, not a fallback.
- **Success criterion 2's escape hatch** ("or every difference is
  enumerated and explained") is reasonable, but say who accepts the
  differences (Laszlo) and where the accepted diff file lives in the repo.
- **Unverified here:** that v11.1.3 is the current stable tag, and the
  "Nov 2025 `hw/irq.h` → `hw/core/`" header move. Neither tag nor commit is
  reachable from this checkout. Re-check both after fetching upstream; the
  header list in "Left to the builder" should be treated as hints, not facts.
- **Docker build of 5.2:** `ubuntu:20.04` is the right first choice. A
  modern host also breaks on the Python side (3.12 has no `distutils`), not
  only GCC 13, so the `--disable-werror` host fallback is weaker than the
  plan implies.
- **The design doc itself** is untracked on `master` (v6.2.0-rc4) in this
  checkout. Commit it, plus this review, as the first commit of
  `stix-emul-qemu-v11.1` so the reasoning travels with the branch.
- **Missing sequencing:** the doc is a design, not an implementation plan.
  The next step should be a phased plan with a hard gate: *no port work
  until the baseline artifacts (UART log, device-print log, `in_asm` head,
  PnP dump) are committed.*

## Suggested additions to "Baseline truth"

1. With `-kernel` and no BIOS, the CPU starts in the PROM bootloader stub.
   The psret patch is inert and UART control ends up `3`, not `0xf` (C1).
2. CTRL IRQ index-0 wiring bug: only `irq_detectors` is connected, to CPU
   IRQ 7 (I2).
3. Flash/CTRL abort and undefined-behavior paths (I1).
4. All three STIX PnP entries report as FTMCTRL (I3).
