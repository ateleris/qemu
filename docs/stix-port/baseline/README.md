# STIX Golden Baseline

**Baseline model:** branch `stix-emul-qemu-v5.2.0`, tip `93a9824b2d` ("trying to
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
| `qemu-stdout.golden.log` | QEMU device prints — a free register-access trace (CTRL/SPW/flash) |
| `asm-head.golden.log` | first 200 translated guest blocks (`-d in_asm`) — bootloader/CPU-start tripwire |
| `run-flags.txt` | the exact invocation |

## Run rules (do not break these)

- **No `-icount`.** Under icount the WDT path reaches the CTRL SCET-set
  registers (`0xC8`/`0xCC`) and hits `assert(0)` in `grlib_stixctrl.c`
  (QEMU SIGABRT, exit 134). Determinism is provided by content stability
  instead — verified: two 90 s runs produce identical normalized content.
- **`flash.bin` (16.5 GB = `FLASH_TOTAL_STORAGE`) persists in
  `/tmp/stix-baseline-run/` and must never be deleted.** First creation takes
  ~2.5 min (16.5 GB of 0xFF); every timeout shorter than that produced the
  misleading "3-line boot" runs of 2026-11-09. If it must be recreated,
  allow a 600 s run.
- The TMTC server binds `IO_TCP_SERVER_BASE_PORT + getuid()` = **21000**
  (uid 1000), not the 2221 constant in `tmtc_io_server.h`.
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

# 5. TM probe (raw packets, no TSIM prefix):
python3 scripts/stix/tmtc-probe.py --seconds 60
```

Notes:
- `flash.bin` (16.5 GB) lives in `/tmp/stix-baseline-run/` (created once by
  `capture-golden.sh`); boot smokes must run there and must never delete it.
- The wrapper locates the old binary via `STIX_OLD_QEMU`
  (default `/tmp/qemu-stix-baseline/build/sparc-softmmu/qemu-system-sparc`).
- `qemu-stdout.golden.log` (the device register-access print stream) is a
  committed reference for MANUAL comparison - no automated gate diffs it
  yet; it is the designated second net for ground-station-era work.

## Known non-issues (absence in the port is NOT a regression)

- `main_cpu_reset` psret/psref/wim patch is inert on the `-kernel` path
  (PROM bootloader stub sets `entry = 0`; verified in v5.2 source and in
  `-d in_asm` output).
- APBUART reset value `0xf` is overwritten by the stub's `3` before the
  guest's first instruction; the stub hardcodes GPTIMER scaler 39 (40 MHz)
  while the machine runs 20 MHz.
- CTRL IRQ wiring: all five outputs connected via `sysbus_connect_irq(dev, 0, ...)`
  — last-wins to the ROTB CPU IRQ line; CTRL raises no IRQs today.
- ROTB init passes accidentally: an early BSP write of `~0x4` to `rotbCmdSta`
  leaves READY (bit 3) set, so `RotBuf_init`'s first poll read succeeds.
  `rotbConfig` ends at `0x900` (ENABLE bit 31 never set; the test-mode branch
  runs because the EDAC param is 0 on the emulator).
- EDAC configuration prints all-zero on the emulator (hardware shows
  EEPROM/CACHE = 1).
- Expected steady-state guest errors: `HKAcquire *E reference voltage ... out
  of range` (ADC reads 0), `LUTMgmt *E could not find LUT` (empty flash).
  These are part of the golden content, not failures.
- `tmtc_io_server.c` defines the `"STIX_CONTROL"` token but never checks it;
  the SPW TC-receive path is a printf stub (TC in cannot reach the guest).
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
