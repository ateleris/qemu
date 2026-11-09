# STIX IDPU QEMU Port — Design

**Date:** 2026-11-09
**Status:** draft for discussion
**Scope:** port branch `stix-emul-qemu-v5.2.0` (QEMU v5.2.0, last commit 2022-02-07) to a current QEMU release, with a regression safety net established before the port begins.

## Intent and why

The STIX IDPU emulation (custom GRLIB devices on the `leon3_generic` machine)
lets STIX flight software (`ASW_em.elf`, RTEMS 4.10, LEON3) run without the AX
hardware at FHNW. The emulation has been dormant for ~4.5 years on QEMU 5.2:

- QEMU 5.2 no longer builds with current GCC (13.x on Ubuntu 24.04) without
  patching — any revival requires porting work regardless of target version.
- Upstream LEON3/GRLIB support has moved toward the real flight silicon:
  AdaCore/Vortex landed IRQMP multicore + multiprocessor status register modelled
  from the GR712RC manual (the IDPU's part family) in Jan 2024, plus years of
  GRLIB fixes (APB PnP access widths, IRQ ack, big-endian access markings).
- Every further year of delay widens the port.

**Goal:** a `stix-emul-qemu-v11.1` branch on `ateleris/qemu` (origin), where the
ASW ELF boots and behaves like the old model, proven by tests captured *before*
the port.

## Baseline truth (what we verified)

- `ASW_em.elf` exists at `/mnt/c/Users/laszl/Development/stix_asw_src/ASW/ASW/bin/`
  (rebuilt 2026-02-09). RTEMS 4.10, LEON3 BSP, uses drvmgr (device discovery via
  AMBA PnP — this is why the model's `grlib_apb_pnp_add_entry` patches matter).
- Old model's last known state, per Laszlo: **boots the ELF and produces boot
  messages**; whether end-to-end TM worked is not remembered. The golden
  reference is therefore the UART boot log, not a full TM exchange.
- No old build exists anymore (`~/qemu-stix` is gone). Docker (29.8) works on
  this machine; host GCC is 13.3 (Ubuntu 24.04), 16 cores.
- `origin` is `https://github.com/ateleris/qemu.git`; it may not carry the
  v11.1 tags — fetch from an upstream mirror if needed.
- The v5.2 model's components (all in `hw/sparc/`, ~3.1k lines, verified
  against `origin/stix-emul-qemu-v5.2.0` tip `93a9824b2d` — vanilla v5.2.0 +
  22 STIX-only commits, no unrelated patches): `StixIdpu.h` (generated memory
  map, 1.7k lines), `grlib_stixflash.c` (working), `grlib_stixspw.c`
  (**TX working; TC-receive path is a printf stub at the tip — TC in cannot
  reach the guest**), `tmtc_io_server.c` (TCP TM/TC bridge; the
  `"STIX_CONTROL"` token is defined but never checked — **vestigial**, any
  client can connect), `grlib_stixctrl.c` (**more live than "WIP"**: every
  register read updates the longTimer registers from the virtual clock, and
  a write to the SCET-set registers `0xC8`/`0xCC` hits `assert(0)` — a live
  crash path), `grlib_stixwd.c` (wired in but commented out of `leon3.c`),
  plus diffs to `leon3.c`, `grlib.h`, APBUART/IRQMP/PnP.
- **Shared-file diffs to carry faithfully — but inert or overwritten on the
  documented `-kernel` path** (verified at v5.2.0): the PROM bootloader stub
  (`write_bootloader`, present since 2019) runs when `-kernel` is given with
  no `u-boot.bin`: it sets `reset_info->entry = LEON3_PROM_OFFSET` (0), so
  `main_cpu_reset`'s psret/psref/wim patch (`if (env->pc)`) never executes;
  the stub writes UART control `= 3` (RE|TE, IRQs off) at `0x80000108`,
  overwriting the APBUART reset value `0xf` before the guest's first
  instruction, and a GPTIMER scaler of `39` (hardcoded for 40 MHz — the
  STIX machine runs 20 MHz; RTEMS's LEON3 BSP likely reprograms the
  prescaler at boot — confirm from the golden log). The APBUART `0xf` reset
  value and the psret patch remain observable to qtest (no kernel → no
  stub). **Port task: diff `write_bootloader()` and the `-kernel` reset flow
  between v5.2.0 and v11.1.x** — the 2024 SMP work touched CPU start-up;
  this is the most likely FSW-observable change the plan wouldn't otherwise
  list. (Stub addresses `0x80000108`/`0x8000030x` match the STIX map's
  UART/TIMER bases — verified.)
- **Known-dead paths at the baseline tip** (absence in the port is not a
  regression): SPW TC injection, SCET-set register writes (they `assert(0)`).
- **CTRL IRQ wiring is wrong at baseline** (verified): `leon3.c` connects
  all five CTRL outputs with `sysbus_connect_irq(dev, 0, ...)` — line 0
  (`irq_detectors`) is the only connected output, last-wins to the ROTB CPU
  IRQ. CTRL raises no IRQs today, so it is latent; a like-for-like port
  keeps it, and a qtest asserts the wiring *as found* (check v11.1 qdev
  tolerates the repeated reconnects).
- **All three STIX PnP entries report as GAISLER/FTMCTRL (0x054), version
  0** (verified) — drvmgr sees three FTMCTRLs. Preserve exactly; the PnP
  qtest asserts the exact words, including upstream's AHB/APB entries.
- **Device abort paths** (next to known-dead; verified): flash read asserts
  `addr == 0` and write asserts a strict RAM-addr → page-addr → cmd/sta
  ordering (`assert(0)` on anything else) — a generic register sweep would
  abort QEMU. CTRL read/write indexes `reg[addr/4]` unbounded, but the MMIO
  region is sized `sizeof(stixCtrlRegs_s)`, so out-of-window accesses are
  unmapped rather than UB (weaker than the review's framing; verify at
  implementation). qtest inventory is therefore derived from what the ASW
  driver actually accesses, not from `StixIdpu.h` wholesale; each assert is
  classified behavior-to-preserve or bug-to-fix-in-a-labeled-commit.
- **Device debug prints share the guest UART channel** (verified): every
  STIX device `qemu_printf`s on register accesses, mixed into `-nographic`
  stdout with guest serial output. Golden capture splits the channels (see
  Goals); the device-print stream is kept as a free register-access trace —
  a stronger regression signal than trace points.
- **Flash persistence:** `grlib_stixflash.c` opens `flash.bin` from the QEMU
  process working directory via raw `fopen`; if absent it silently creates
  an empty all-`0xFF` image and then persists every guest write across runs.
  **Every emulator run must therefore use a scratch working directory with a
  pinned flash state**, or the golden log encodes cross-run contamination.
- **Flash content is constructible, not precious:** `FlashASW/src/main.c`
  (FSW tree) is a host-side tool that generates flash images with CRC-16
  using the BSW's `Flash.h`. No populated `flash.bin` was found on this
  machine (searched `~/dev`, `~/Development`, maxdepth 4).
- **Kconfig:** already works — STIX files build under `CONFIG_GRLIB`
  (`LEON3` selects GRLIB); no extra wiring needed.
- **Historical run recipe (from the old branch's README.rst):**
  `qemu-system-sparc -s -S -no-reboot -nographic -M leon3_generic -m 64M
  -kernel ASW_em.elf` (drop `-S` for automation), trace variant
  `--trace "*irqmp_*"`, gdb via `sparc-rtems4.10-gdb` + `target remote :1234`.
  Treated as the authoritative historical invocation.
- **Fallback ELFs exist:** `ASW/ASW/bin/History/` holds older builds
  (Feb 2024 era) if the current `ASW_em.elf` (2026-02-09) fails to boot on
  the old model.
- **No TM was ever received, per Laszlo's recollection:** the old model
  printed to console only; no TMTC client was attached end-to-end in 2022.
  The TM-out TCP server code exists and is intact (builder-verified), but
  the baseline probe should *expect* no TM. A future project — **attaching
  the ground station over TCP/IP to receive packets** — depends on TM-out
  surviving the port; it is explicitly out of scope here.
- **TMTC protocol (verified in both trees):** QEMU listens on TCP **2221**
  (`IO_TCP_SERVER_LISTENING_PORT`); client sends the 12-byte handshake token
  `"STIX_CONTROL"`; packets are framed with a 4-byte TSIM header (length
  prefix, `TSIM_HEADER_SIZE=4`); TM returns over the same connection via
  `send_tm_packet()`. Identical constants in the FSW tree's
  `TSIM_IO/ASW_UT/src_io/ioTcpServer/ioTcpServer.c` — one protocol, two
  implementations.
- **Boot log shape (from real capture `ASW/202601121224-terminal.log`,
  FSW v1.86):** version banner `STIX ASW_v2, ver. ...`, RTEMS config dump,
  EDAC config, then the `init:` sequence (ParamMgmt, ADCs, MemCommon,
  TM buffer, ContextMgmt, Timing, TC buffer, FDIR, MODE, HKReport,
  SPW control), `do: selftest`, `do: boot report`, `do: enable HK IRQ`,
  task inits. Known-expected errors under emulation: `*E reference voltage
  ... out of range` (no real ADC). The golden-log normalizer must whitelist
  these, not fail on any `*E` line.

## Goals

1. **Baseline first — with a hard gate: no port work until the baseline
   artifacts are committed.** Rebuild the old branch in an era-correct
   Docker container (host `--disable-werror` fallback is weaker than it
   looks: Ubuntu 24.04's Python 3.12 lacks `distutils`, which the 5.2 build
   needs — so Docker is the primary path), boot `ASW_em.elf`, capture golden
   artifacts:
   - **Guest UART on its own channel** (primary golden reference):
     `-serial file:uart.log -display none -monitor none`, so QEMU's own
     debug prints don't mix into the golden stream
   - **QEMU stdout/stderr as second artifact** — the STIX devices'
     `qemu_printf` register-access prints (a free register-access trace of
     the old model)
   - boot-time `--trace` output for **irqmp/apbuart/pnp events only** (the
     old branch defines no STIX trace points) — secondary reference
   - `-d in_asm` output of the first ~20 guest instructions — cheap
     tripwire for bootloader/CPU-start differences (C1)
   - best-effort: TM output over the TMTC TCP server from a scripted client
     (stretch — only if the old model actually produces TM)
   - **All golden runs use `-icount shift=N,sleep=off` with the same flags
     for old and new builds**, so guest timing is deterministic rather than
     host-speed dependent; N is fixed once at baseline capture. Without
     this, "matches the baseline" is flaky in ways indistinguishable from
     regressions.
   - Every run executes in a scratch working directory with a pinned flash
     state (`flash.bin` is created/persisted in the CWD).
2. **Port.** New branch `stix-emul-qemu-v11.1` based on QEMU v11.1.x
   (current stable as of 2026-11: 11.1.3 — verified via qemu.org; re-check
   after fetching upstream). Like-for-like port: preserve observable
   behavior, change only what compilation/modernization of the *base*
   requires. Commit as a reviewable series (one commit per concern);
   `-Werror`-driven STIX source fixes (e.g. the `qemu_printf(stderr, ...)`
   misuse in `grlib_stixspw.c`) go in their own commits so like-for-like
   commits stay readable. Two accepted, non-guest-visible deviations from
   strict like-for-like: a `flash-file` qdev property (default `"flash.bin"`)
   and a TMTC listen-port property (default `2221`), for hermetic test runs.
3. **Tests, three layers:**
   - **Build gate:** full build on host GCC 13 with `-Werror` (QEMU default);
     no warning suppressions in STIX code.
   - **Device regression (in-tree `tests/qtest`):** inventory derived from
     what the **ASW driver actually accesses** (not `StixIdpu.h` wholesale —
     flash's `assert`s abort on generic sweeps; abort paths are documented
     next to the known-dead paths, each assert classified
     behavior-to-preserve vs bug-to-fix-in-labeled-commit). Covers: PnP
     entries (exact words, incl. upstream AHB/APB entries — drvmgr depends
     on them; byte/half/word access equivalence), CTRL wiring *as found*
     (index-0 bug), flash read/write protocol, SPW TX descriptor path, WD if
     enabled. **Old-build validation runs the qtest binary built once from
     the new tree with `QTEST_QEMU_BINARY=<old qemu-system-sparc>`** — do
     not back-port tests into the 5.2 tree. IRQ checks prefer reading the
     IRQMP pending register over `irq_intercept_in` on CPU lines (simpler,
     identical on both builds). If upstream's PnP `.valid`/`.impl` now
     covers sub-word access, **drop the STIX byte-access hack** and prove
     equivalence with the PnP qtest on both builds; keeping both would
     double-shift.
   - **Boot smoke (script, in-tree, e.g. `scripts/stix/`):** run
     `qemu-system-sparc -M leon3_generic -m 64M -no-reboot -kernel
     ASW_em.elf` (historical recipe; drop `-s -S` for automation), capture
     UART via the split channels above, assert normalized boot log matches
     golden baseline (or an accepted diff), fail on timeout/fatal. Checks
     the TMTC port is free before starting (the device binds fixed 2221 at
     realize; bind failure is logged, not fatal, and would make a TMTC test
     talk to the wrong instance). Skips gracefully when the ELF or host
     prerequisites are absent (ELF lives on a Windows mount; not
     CI-portable).
4. **Deliverable:** branch pushed to `ateleris/qemu`, plus a committed
   `Dockerfile` + script reproducing the old-baseline build so the
   comparison stays reproducible. The design document and this review are
   committed as the first commit of `stix-emul-qemu-v11.1` so the reasoning
   travels with the branch.

## Non-goals (this round)

- Finishing or enabling the WIP watchdog/CTRL functionality — port them as
  they are (WD stays commented out of `leon3.c` unless trivial to leave
  equivalent).
- Rewriting `tmtc_io_server.c` onto the chardev socket frontend, QOM/style
  cleanups beyond compilation requirements — logged as follow-ups.
- CI pipeline setup; upstream submission of STIX devices.
- Ground-station TCP attachment (Laszlo's follow-up goal, ground station
  provided later): the port must preserve TM-out (TMTC server + `tmtc-port`
  property) but builds no integration for it.
- SMP/multicore (RTEMS 4.10 is uniprocessor; old model was single-CPU).
- Any STIX-GSW / STIX-TESTING repo changes.

## Anti-goals (what would make this a failure)

- A port that compiles but silently changes FSW-observable behavior
  (IRQ semantics, timer behavior, PnP entries, register maps). Mitigation:
  baseline-first golden diff + qtest.
- Tests written only against the new code that would also pass a broken
  port (no old-run validation). Mitigation: run the whole suite against the
  old build first; tests must pass there before the port is judged.

## Constraints

- Host: WSL2, Ubuntu 24.04, GCC 13.3; meson/ninja must be installed (pip).
- Old QEMU 5.2 toolchain: Docker `ubuntu:20.04` with era packages, or
  host build with `--disable-werror` fallback (documented in the
  Dockerfile/repro script whichever path is used).
- `ASW_em.elf` path is outside the repo; boot tests parameterize the path
  (env var, e.g. `STIX_ASW_ELF`) and skip when unset.
- Golden-log comparison must normalize volatile content (timestamps,
  pointers/addresses, ordering racy lines) — the diff tooling defines the
  accepted-difference format explicitly.
- The FSW tree (`/mnt/c/Users/laszl/Development/stix_asw_src`, private repo
  `i4Ds/STIX-FSW`) is **strictly read-only reference** per Laszlo: no edits
  there, and its own ground rules apply (no reading stored git credentials;
  `ASW/cmdvs/` is hardware-campaign material).

## Decisions made (do not re-litigate without reason)

| Decision | Value | Reason |
|---|---|---|
| Target version | QEMU 11.1.x (latest stable) | no LTS exists; latest stable maximizes runway |
| Port style | like-for-like | minimal behavioral delta = meaningful regression comparison |
| Branch | `stix-emul-qemu-v11.1`, new, off v11.1.3 tag | old branch preserved as historical baseline; **origin carries no v11 tags — fetch upstream mirror first** |
| Baseline commit | empirical: tip `93a9824b2d` first; if it doesn't boot, walk back (`bf501dd0f9` next) — **confirmed by Laszlo 2026-11-09** | the build answers what memory can't |
| Known-dead paths | documented in baseline notes: SPW TC-in stubbed, SCET-set `assert(0)` | port must not chase their absence as regressions |
| Flash baseline | empty auto-created image; every run in a scratch dir with pinned flash state — **confirmed by Laszlo 2026-11-09** | no populated `flash.bin` found; FlashASW tool can generate content later if golden boot shows flash dependence |
| Trace baseline | capture-what-exists (irqmp/apbuart/pnp); new STIX trace points allowed in port but never golden-asserted | old branch has no STIX trace events to validate against |
| ELF fallback order | current `ASW_em.elf` → `History/` ELFs → FSW **copy** outside the read-only tree + README's `TMBuffer_init` tweak → if all fail: **stop and ask** (defining the baseline as "current ELF on the new build" is circular — no regression reference remains) | rebuild touches a copy, never the read-only FSW tree |
| Test homes | qtest in-tree; boot smoke as repo script | qtest is QEMU-native/hermetic; boot test depends on machine-local ELF |
| TMTC round-trip test | downgraded to a **probe, not a test**: old model was console-only per Laszlo (2026-11-09); probe settles it definitively and records the finding | ground-station TCP attachment is a named follow-up, out of scope |

## Left to the builder

- Exact Docker image + package set for the 5.2 build; the host
  `--disable-werror` fallback path.
- Cherry-pick vs. manual re-apply per file; conflict strategy for:
  `leon3.c` (machine->ram/memdev, new bootloader/CPU-start, load_elf
  signature), `grlib.h` (split per-peripheral since Jan 2024 — STIX device
  declarations need new headers), include-path moves (`hw/irq.h`,
  `hw/qdev-properties.h`, `hw/sysbus.h` → `hw/core/`, Nov 2025),
  `DEVICE_NATIVE_ENDIAN` → `DEVICE_BIG_ENDIAN` for SPARC peripherals,
  `DEFINE_PROP_END_OF_LIST` removal, `class_init` const-data signature.
  (The header-move list is treated as hints, not facts: re-verify against
  the fetched v11.1 tree. The Nov 2025 `hw/irq.h`/`hw/qdev-properties.h`/
  `hw/sysbus.h` → `hw/core/` moves were verified via upstream git history,
  but exact final paths can shift.)
- qtest case inventory per device (register reset values, RW semantics,
  IRQ lines, PnP entries) from `StixIdpu.h` and the old code.
- Normalization rules for the golden-log diff (which lines are volatile).

## Success criteria

1. Old-baseline reproduction: committed Dockerfile + script boots the old
   branch and captures the golden artifacts without manual fiddling.
2. Port branch: ASW ELF boots on v11.1; normalized boot log matches the
   golden baseline, or every difference is enumerated in
   `docs/stix-port/accepted-diffs.md` and accepted by Laszlo.
3. qtest suite green on the port; also run against the old build where
   applicable to prove the tests themselves are meaningful.
4. Clean build with `-Werror` on GCC 13; no STIX-code warning suppressions.
5. Series pushed to `ateleris/qemu` as `stix-emul-qemu-v11.1`.
