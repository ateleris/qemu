# STIX IDPU emulation on QEMU 11.1

**This branch (`stix-emul-qemu-v11.1`) is the one to use.** It is the STIX
IDPU emulation — the device models needed to run the Solar Orbiter STIX
flight software (`ASW_em.elf`, RTEMS 4.10, LEON3) under QEMU instead of the
AX-model hardware — ported from the dormant 2022 QEMU 5.2 branch
(`stix-emul-qemu-v5.2.0`) to QEMU v11.1.3, with a regression suite proving
the port behaves like the old model.

Other branches in this fork are stale: `master` is vanilla upstream, and
`stix-emul-qemu-v5.2.0` no longer builds with modern toolchains.

## What this branch adds (on top of upstream QEMU 11.1)

Custom GRLIB-family device models in `hw/sparc/`, wired into the
`leon3_generic` machine:

| Device | What it models |
|---|---|
| `grlib-stixflash` | FTNANDCTRL flash (16.5 GB flash.bin, persists in CWD) |
| `grlib-stixspw` | GRSPW SpaceWire registers + TMTC TCP bridge (TM out) |
| `grlib-stixctrl` | Detector/HK/EDAC/ROTB controller registers incl. SCET clock |
| `grlib-stixsdram` | SDRAM controller register stub (model repair) |
| `grlib-stixwd` | Watchdog (compiled but not instantiated — as 2022) |

Plus baseline-faithful tweaks: 20 MHz CPU clock, APBUART reset value,
AMBA PnP entries (all three STIX devices visible as FTMCTRL), and the
`StixIdpu.h` generated memory map.

## Quick start

```bash
# Build
../configure --target-list=sparc-softmmu --disable-docs --enable-werror && make -j

# Boot the flight software (UART on stdio)
./build/qemu-system-sparc -M leon3_generic -m 64M -no-reboot -nographic \
    -kernel /path/to/ASW_em.elf
```

Telemetry: the model emits TM on TCP `20000 + <your uid>` (raw packets, no
TSIM length prefix) — connect any TCP client right after start.

## Validation and state

- **Regression suite** (device qtests, boot smoke vs golden baseline, TM
  probe, old-build comparison): see
  **[`docs/stix-port/baseline/README.md`](docs/stix-port/baseline/README.md)**
  — it is the complete operator manual.
- **Accepted differences vs the 2022 baseline**:
  [`docs/stix-port/accepted-diffs.md`](docs/stix-port/accepted-diffs.md).
- **Known limitations**: TC injection is stubbed (cannot send TCs to the
  guest yet), the watchdog device is present but not wired, guest-reachable
  device `assert`s are preserved from the baseline (do not use this as a
  sandbox for untrusted code), and the emulated CPU presents an FPU the
  real IDPU part lacks (harmless — the FSW is soft-float). Full list in the
  baseline README.
- Ground-station attachment (ateleris/ATS-GroundStation) is the planned
  follow-up; the open question there is packet framing (raw vs 4-byte
  TSIM prefix).
