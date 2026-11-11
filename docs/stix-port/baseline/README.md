# STIX Golden Baseline

**Current golden (since R3, 2026-11-10):** captured from the uprev model
(`stix-emul-qemu-uprev`) on the **normal** FSW ELF `ASW_em_baseline.elf`
(sha256 in `run-flags.txt`), with a sparse, inverted flash file (D-I). It
differs from the original v5.2 golden only by the removed per-second
watchdog error (`accepted-diffs.md` §10). The history below describes the
original v5.2 baseline that the port was validated against.

**Original baseline model:** branch `stix-emul-qemu-v5.2.0`, tip `93a9824b2d` ("trying to
implement WD and CTRL", 2022-02-07) **plus two model repairs** (worktree branch
`stix-emul-repairs`):

- `23fb928739` — SDRAM controller register stub (`grlib_stixsdram.c` at
  `0x80000C00`): without it, `RamUtils_initSDRAM()` polls a controller the
  model never mapped, times out, and `MemCommon_init()` fails into the
  `TMBuffer.c:276` assert.
- `0df394b42b` — HK ADC READY-on-write (`grlib_stixctrl.c`): without it the
  FSW polls `hkAdc` bit 7 forever (18.5M reads observed).

Both repairs enter the v11.1 port series as its first labeled commits.

**Baseline guest:** current `ASW_em.elf` (FSW v1.86, built 2026-02-09). It
boots fully on the repaired model: banner → RTEMS/EDAC config → ParamMgmt →
MemCommon → selftest → boot report → FDIR/MODE/HKReport/SPW → tasks →
`init: SAFE mode` at ~2.9 s → steady state (SPW polling, HK cycles). No
2021/2022-era ELF exists anywhere on this machine; per the stop-and-ask
ruling with Laszlo (2026-11-09) the baseline target is the CURRENT ELF on the
REPAIRED old model.

## Artifacts

| File | Content |
|---|---|
| `uart.golden.log` | guest UART (own channel, `-serial file:`) |
| `qemu-stdout.golden.log` | the STIX `stix*` trace-event stream (`-trace 'stix*'` on stderr, merged) plus the one-off flash realize reports — the register-access trace |
| `asm-head.golden.log` | first 200 translated guest blocks (`-d in_asm`) — bootloader/CPU-start tripwire |
| `run-flags.txt` | the exact invocation |

## Run rules (do not break these)

- **No `-icount`.** Under icount the WDT path reaches the CTRL SCET-set
  registers (`0xC8`/`0xCC`) and hits `assert(0)` in `grlib_stixctrl.c`
  (QEMU SIGABRT, exit 134). Determinism is provided by content stability
  instead — verified: two 90 s runs produce identical normalized content.
- **Run directory: `/tmp/stix-uprev-run/`** (`STIX_RUN_DIR`, the default of
  all three scripts). Its `flash.bin` is 17.7 GB apparent but sparse and
  inverted (D-I, `accepted-diffs.md` §9): the model creates it instantly if
  it is missing, and it costs only the pages the FSW writes (about 16 KB
  after a boot). Keep it between runs, because the golden assumes a flash
  the FSW has already booted on.
- **`/tmp/stix-baseline-run/` is the frozen v11.1 reference.** It holds a
  plain 16.5 GB `flash.bin`. Never run QEMU there, and never delete or
  copy that file: the host is a WSL VM on a nearly full Windows disk. A
  plain file used with the new model reads all 0x00 (all blocks bad); use
  `-global grlib-stixflash.inverted-storage=off` for it.
- **The ELF.** The gate scripts (`boot-smoke.sh`, `soak.sh`,
  `capture-golden.sh`) default to the normal ELF the golden was captured
  from, `/tmp/stix-baseline-run/ASW_em_baseline.elf` (reading it is fine).
  - `STIX_ASW_ELF` overrides it.
  - Don't point it at `stix_asw_src/.../ASW_em.elf` for a gate. That file is
    whatever was built last on Windows. On 2026-11-10 that was a
    `LIMIT_FS_SIZE=32` build without `DEBUG_PRINT`, which has no UART output
    at all, so boot-smoke fails with "no UART output".
- The TMTC server binds `IO_TCP_SERVER_BASE_PORT + getuid()` = **21000**
  (uid 1000), not the 2221 constant in `tmtc_io_server.h`.
- Since P2-1 (`accepted-diffs.md` §14) the ported model starts **no TM
  server when the qtest harness drives QEMU**
  (`grlib-stixspw.tmtc-server=auto`), so the qtest suite needs no port
  lock against itself — except `/sparc/stix/tmtc-hermetic`, which holds
  20000+uid in LISTEN while it runs. So don't run the qtests at the same
  moment as a boot gate or a live-FSW QEMU. The boot gates
  (`boot-smoke.sh`, `soak.sh`, `tc-smoke.sh`) and the 5.2-wrapper run
  (step 3) still use the lock: the 5.2 device always binds 21000 and has
  no `tmtc-server` property.
- Capture uses `-display none -monitor none -serial file:` so guest UART and
  QEMU prints never mix.

## Running the regression suite

Everything needed to re-validate the port from the committed scripts and
docs alone (paths assume this repo; the ASW ELF path is overridden with
`STIX_ASW_ELF`):

```bash
# 1. Build the ported tree (GCC >= 7.5-style toolchain, meson >= 1.1, ninja)
../configure --target-list=sparc-softmmu --disable-docs --enable-werror && make -j

# 2. Device regression suite against the NEW build (from any CWD - the
#    qtest passes -global grlib-stixflash.flash-file=<temp file> so no
#    flash.bin is created in the CWD):
QTEST_QEMU_BINARY=$PWD/build/qemu-system-sparc build/tests/qtest/stix

# 3. Device regression suite against the OLD repaired 5.2 baseline (the
#    wrapper bridges v11.1 libqtest args to 5.2 and strips the flash-file
#    override, which the 5.2 device lacks):
git worktree add /tmp/qemu-stix-baseline stix-emul-repairs   # see build-old.sh
bash scripts/stix/baseline/build-old.sh                       # ~5 min, Docker
QTEST_QEMU_BINARY=$PWD/scripts/stix/baseline/qemu-system-sparc-52-wrapper.sh \
    build/tests/qtest/stix

# 4. Boot smoke against either build (needs the ASW ELF; skips politely
#    without it; compares the boot sequence against the golden):
STIX_ASW_ELF=/path/to/ASW_em.elf bash scripts/stix/boot-smoke.sh \
    build/qemu-system-sparc          # add --with-tm for the TM check

# 5. TM probe (D-K framing since P2-6: [4-byte SpW header][CCSDS packet],
#    split on the CCSDS length; the raw chunk lines are still printed):
python3 scripts/stix/tmtc-probe.py --seconds 60

# 6. TC end-to-end gate (P2-6, needs the ASW ELF; boots the model, sends
#    TC(17,1) seq 0 at ~20 s and expects TM(1,1), TM(17,2), TM(1,7)
#    within 10 s, then a corrupt-CRC TC and expects TM(1,2)):
STIX_ASW_ELF=/path/to/ASW_em.elf bash scripts/stix/tc-smoke.sh \
    build/qemu-system-sparc          # add --dry-run to only print

# 7. Detector gate (detector plan D-3, accepted-diffs §33; needs the ELF,
#    a fresh run dir of its own and the 21000 flock; ~4.5 min):
STIX_DETECTOR_RUN_DIR=/tmp/stix-detector-run \
    flock /tmp/stix-qtest-21000.lock bash scripts/stix/detector-smoke.sh \
    build/qemu-system-sparc          # add --negative, --dry-run
# its stdlib checkers, also usable on their own:
python3 scripts/stix/ql-check.py --self-test          # QL TM(21,6) decode
python3 scripts/stix/idefx-trace-check.py --self-test # verification runs
```

Notes:
- `flash.bin` lives in `/tmp/stix-uprev-run/` (sparse, see the run rules).
  Gate scripts run there and never delete it. The qtests use their own temp
  files.
- Long gate: `STIX_ASW_ELF=... bash scripts/stix/soak.sh build/qemu-system-sparc 600`.
  Liveness is TM flow: at least one TM chunk in every 60 s slice of the
  window. UART growth is only reported, because with the watchdog modeled
  the normal ELF goes quiet after boot.
- TC gate: `STIX_ASW_ELF=... bash scripts/stix/tc-smoke.sh build/qemu-system-sparc`.
  **PASS** means the model accepted the ground's TC(17,1) over TCP, the
  FSW verified and executed it, and — within `STIX_TC_WINDOW` (10 s) of
  the send — TM(1,1) arrived on APID 0x5A1 with application data
  `1DAC C000`, TM(17,2) on 0x5A7, and TM(1,7) on 0x5A1 with `1DAC C000`;
  afterwards a TC(17,1) with a corrupted CRC trailer was rejected with
  TM(1,2) on 0x5A1. Expectations key on APID and PUS type/subtype
  (correction 5), never on the 0x49 SpW header byte (findings F1). QEMU
  is stopped when the probe finishes; `flash.bin` stays in place.
  `--dry-run` prints the commands without booting.
- Detector gate: `detector-smoke.sh` (see step 7). **PASS** means the
  FSW went SAFE -> CONF -> NOMINAL -> CONF with the ASIC verification
  period at 30 s, no verification/SEU/noisy-detector/fallback TM fired,
  the NOMINAL-exit flush carried all five QL SSIDs 30-34 with the
  zero-sky content (`ql-check.py` on the probe's `--dump-apid` file),
  the UART had no IDeF-X, context or ROTB error, and the trace showed
  >= 3 verification runs of exactly 384 read address blocks
  (`idefx-trace-check.py`). `--negative` (`det-idefx=off`) passes only
  when exactly the UART IDeF-X check and the trace check fail, the
  trace being present and empty.
- **Gate exit codes:**

  | Code | Meaning |
  |---|---|
  | 0 | PASS (also `--dry-run` / `--check-guard` done) |
  | 1 | FAIL: an expectation, a forbid, a closed connection or a post-run check |
  | 2 | environment problem (port busy, QEMU died, bad arguments, refused run dir) |
  | 77 | SKIP: the ASW ELF is absent. **The convention for new gates** (`detector-smoke.sh` and later), as automake/meson treat 77. |

  Legacy: `boot-smoke.sh`, `soak.sh`, `tc-smoke.sh`, `nominal-smoke.sh`
  and `fault-smoke.sh` still exit 0 on SKIP (they print `SKIP:`); they
  keep that for now. The checkers `ql-check.py` and
  `idefx-trace-check.py` exit 0 pass, 1 fail, 2 unreadable input.
- The wrapper locates the old binary via `STIX_OLD_QEMU`
  (default `/tmp/qemu-stix-baseline/build/sparc-softmmu/qemu-system-sparc`).
- `qemu-stdout.golden.log` (the `stix*` trace-event stream, captured with
  `-trace 'stix*'` — accepted-diffs §15) is a committed reference for
  MANUAL comparison - no automated gate diffs it yet; it is the
  designated second net for ground-station-era work.

## Watching a run (console, TM, model events)

The guest console (FSW UART) always goes to a plain file, so any run can
be watched live from a second terminal. The FSW pads with NUL bytes, so
strip them:

```bash
tail -f /tmp/stix-uprev-run/uart.soak.raw | tr -d '\000'   # FSW console
tail -f /tmp/stix-uprev-run/tm.soak-probe.log              # TM chunks as they arrive
```

From Windows the same files are at `\\wsl$\<distro>\tmp\stix-uprev-run\`.

| Run | FSW console (UART) | QEMU device prints / `stix*` traces | TM probe log |
|---|---|---|---|
| `soak.sh` | `uart.soak.raw` | `qemu-stdout.soak.log` | `tm.soak-probe.log` |
| `boot-smoke.sh` | `uart.smoke.raw` (+ `.norm`) | `qemu-stdout.smoke.log` | `tm.smoke.log` (`--with-tm`) |
| `tc-smoke.sh` | `uart.tc.raw` | `qemu-stdout.tc.log` | `tc.smoke.probe.log` (framed units + expectations) |
| `nominal-smoke.sh` | `uart.nominal.raw` (+ `.txt`) | `qemu-stdout.nominal.log` | `nominal.probe.log` |
| `detector-smoke.sh` | `uart.detector.raw` (+ `.txt`) | `qemu-stdout.detector.log`; `idefx.trace` (timestamped `stixdet_idefx_cmd`) | `detector.probe.log`; `ql.dump.hex` (whole 0x5DC packets), `ql.check.log` (decoded QL TM) |
| `capture-golden.sh` | `uart.golden.raw` | `qemu-stdout.golden.log` | — |

All of these are in the run directory. Each run overwrites its own files.

**NOMINAL experiments need a fresh run dir.** Entering NOMINAL — by any
route, scripted or manual — makes the FSW write a flash context
(`ContextMgmt_deinit`), and every later boot **on that `flash.bin`**
starts from it (the TC20 reaction stays disabled; the other context
parameters come from it too). Never point a NOMINAL session at
`/tmp/stix-uprev-run` or `/tmp/stix-baseline-run`; use an empty
directory of its own (`STIX_NOMINAL_RUN_DIR` for `nominal-smoke.sh`,
which enforces this, or an empty dir with its own `flash.bin` for a
manual run). Do not reuse another experiment's run dir either:
the p3probe4 experiment silently booted from p3probe3's context, and
its boot looked cleaner than a fresh flash's for that reason alone.

**Interactive run, console in your terminal.** Only do this while no
other QEMU is running, because the TM server binds port 21000. Ctrl-C ends
the run:

```bash
cd /tmp/stix-uprev-run && /path/to/build/qemu-system-sparc \
    -M leon3_generic -m 64M -no-reboot -display none -monitor none \
    -serial stdio -kernel /tmp/stix-baseline-run/ASW_em_baseline.elf
```

- Since P2-2 the model prints **no device activity by default** (the only
  stderr lines left are one-off reports: the flash "missing, creating a
  new one" info line, the flash host-I/O error and QEMU's own
  error/warning reports). Add `-trace 'stix*'` to get the old
  register-access output back on stderr: every SPW and CTRL MMIO access,
  the flash file/size and TMTC server lines, plus the flash errors
  (`stixflash_error`), watchdog commands and resets, and SCET writes. Use
  `-D <file>` to send them to a file.
- `python3 scripts/stix/tmtc-probe.py --seconds 60` in a second terminal
  shows the TM stream.

**What a healthy run looks like (normal ELF):**
- About 100 UART lines of boot, up to `init: SAFE mode`, then the console
  goes **quiet**. This is correct: the once-per-second
  `Watchdog *E invalid number of cycles` error disappeared with the
  watchdog model (R3).
- The expected steady-state error `LUTMgmt *E could not find LUT` is
  part of the golden. The `HKAcquire *E reference voltage` lines are
  gone since P3-1 (`accepted-diffs.md` §22): the ADC now returns the
  nominal values and the boot self-test passes.
- TM keeps ticking at about one packet per second (FSW time runs ~0.5–0.9×
  wall time).

**Warning signs:**
- `FS *E Write error`: flash writes are failing. Check the trace for
  `stixflash_error ... why=beyond-backing-file`, which means the flash file
  is too short or isn't the sparse default.
- QEMU exits early under `-no-reboot`: a guest reset. If
  `stixctrl_watchdog_reset` is in the trace, the watchdog expired because
  the FSW didn't feed it for ~6.6 s.

## Known non-issues (absence in the port is NOT a regression)

- `main_cpu_reset` psret/psref/wim patch is inert on the `-kernel` path
  (PROM bootloader stub sets `entry = 0`; verified in v5.2 source and in
  `-d in_asm` output).
- APBUART reset value `0xf` is overwritten by the stub's `3` before the
  guest's first instruction; the stub hardcodes GPTIMER scaler 39 (40 MHz)
  while the machine runs 20 MHz.
- CTRL IRQ wiring: **fixed** (see `accepted-diffs.md` §11) — the baseline
  connected all five CTRL outputs via `sysbus_connect_irq(dev, 0, ...)`
  (last-wins to the ROTB CPU IRQ line); they now reach IRQMP lines
  3/4/5/6/7. Still no CTRL IRQ source until the ROTB path lands.
- ROTB init passes accidentally: an early BSP write of `~0x4` to `rotbCmdSta`
  leaves READY (bit 3) set, so `RotBuf_init`'s first poll read succeeds.
  `rotbConfig` ends at `0x900` (ENABLE bit 31 never set; the test-mode branch
  runs because the EDAC param is 0 on the emulator).
- EDAC configuration prints all-zero on the emulator (hardware shows
  EEPROM/CACHE = 1).
- Expected steady-state guest error: `LUTMgmt *E could not find LUT`
  (empty flash) — part of the golden content, not failures. The
  `HKAcquire *E reference voltage` errors left with P3-1
  (`accepted-diffs.md` §22).
- The `"STIX_CONTROL"` token and the SPW TC-receive path: **fixed since
  P2-4/P2-6** (accepted-diffs §17/§19) — the token is stripped per
  connection and framed TC units reach the guest's RX DMA. This bullet
  described the baseline only; on the baseline model TC in is still dead.
- **TM DOES flow** (correcting the earlier "console-only" recollection): the
  model emits real telemetry on TCP 20000+getuid() — raw packets, NO 4-byte
  TSIM length prefix (that framing exists only in the FSW-side
  ioTcpServer). See `tm.golden.log` (37 B housekeeping ~2 s cadence,
  CCSDS-looking 0x49 header). The ported v11.1 model produces the same
  stream (`boot-smoke.sh <new-binary> --with-tm`).
- grlib_stixflash read asserts `addr == 0`; write asserts a strict
  RAM→page→cmd ordering; unknown offsets `assert(0)`. CTRL SCET-set writes
  (`0xC8`/`0xCC`) `assert(0)`. CTRL `reg[addr/4]` indexing is bounded by the
  region size (`sizeof(stixCtrlRegs_s)` = 0xF4).
