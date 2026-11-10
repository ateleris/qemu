# Accepted differences: v11.1 port vs golden baseline

Every difference the acceptance runs surfaced between the ported model
(QEMU 11.1, branch `stix-emul-qemu-v11.1`) and the golden baseline
(repaired v5.2 model). Compared: qtest device suite (4/4 PASS, no
differences), boot smoke (PASS — no UART differences), `-d in_asm` head
(enumerated below).

## 1. `-d in_asm` head: bootloader preamble (accepted)

The v11.1 PROM bootloader stub (`write_bootloader`, reworked by upstream's
January 2024 LEON3 multiprocessor series) prepends four instructions before
the stores the baseline knows:

    0x00000000:  rd  %asr17, %g2
    0x00000004:  srl  %g2, 0x1c, %g2
    0x00000008:  tst  %g2
    0x0000000c:  bne  0x84

Everything after the preamble is instruction-identical to the baseline
head: UART control `= 3` at `0x80000108`, GPTIMER scaler at `0x80000304`
(now `19` instead of the baseline's `39` — see section 6), timer
counter/config stores, then the jump to the ELF entry.

**Why accepted:** on the single-CPU boot the `bne` is not taken (asr17
reports no secondary CPUs present), so guest-observable behavior is
unchanged — proven by the passing boot smoke and identical PnP table.
**Risk:** if the FSW ever inspects the PROM contents or counts instructions
before the UART write, the preamble is visible.

## 2. PnP table: no difference

All 12 PnP words match the baseline byte-for-byte (the qtest caught one
real deviation during the port — the GPTIMER entry's IRQ field carried
upstream's 6 instead of the baseline's 10 — fixed in
`986ae3e937` by using the baseline IRQ values).

## 3. Boot UART log: no difference

`boot-smoke.sh` against `build/qemu-system-sparc`: PASS (normalized boot
sequence identical through `init: SAFE mode`).

## Deliberately preserved non-identical behavior

Documented in `docs/stix-port/baseline/README.md` (known non-issues): the
inert-on-`-kernel` psret/psref/wim patch, APBUART reset `0xf` overwritten
by the stub, CTRL IRQ index-0 wiring, the accidental ROTB READY pass,
guest-reachable device `assert`s, the uint32 `ftell` truncation with the
16.5 GB flash, and the vestigial `STIX_CONTROL` token. These match the
baseline; they are not port deviations.

## 4. CTRL SCET set (0xC8/0xCC): implemented per HSWICD commit protocol (baseline asserts)

The baseline model `assert(0)`s on any write to the SCET registers
(`longTimerSec` 0xC8 / `longTimerSubSec` 0xCC), so the FSW's time-set
path killed QEMU ~135 s wall / ~60 s FSW into every run (review finding
T1). The port implements the set semantics confirmed with Laszlo
(decision D1, amended to the HSWICD commit protocol after a 600 s soak
showed the first draft crashing the FSW's HK pipeline):

- `0xC8` write **stages only**: seconds field := value & 0x7FFFFFFF
  (bits 30:0); bit 31 is the UNSYNCED status flag
  (`TIMING_COARSE_UNSYNCED`, Timing.c) and is stored for readback. The
  running time is not touched (HSWICD 4.51: "When written SubSec and
  previously stored LongTimerSec.Secs fields will be set as a new value
  of on-board time" — see `IIR_setElapsedTime`, IIR.c).
- `0xCC` write **commits**: on-board time := (staged seconds,
  value & 0xFFFF). Bits 31:16 of 0xCC are status and do not affect the
  time.
- Reads of 0xC8 return running-seconds | (UNSYNCED << 31); the FSW
  checks that bit (Timing.c:136, 232).
- Internally one ns-resolution correction (`time_correction_ns`) against
  `QEMU_CLOCK_VIRTUAL`; reads convert to coarse/fine (1/65536 s) on the
  fly.

**Why amended:** the first D1 draft took all 32 bits of the 0xC8 write
as seconds. The FSW's lost-timecode handler (Timing.c
`timerLostSpWTCsCallback`, fires ~60 s FSW into any run without a ground
station) sets the UNSYNCED flag on 0xC8 — the draft turned that flag-set
into a +68-year clock jump, wedged the FSW's TM scheduling, and the
rotating buffer overfilled (`HKAcquire.c:325` assert, soak RED). The
soak gate caught it; the commit protocol fixed it.

Evidence: qtest `stix/ctrl-scet` pins the protocol (stage-only, commit,
flag readback, free-run, backwards, large epoch, flagged commit) — full
suite 5/5 green on the new build; the same suite against the 5.2
baseline build goes RED on `ctrl-scet` by design (the test pins the fix
the baseline lacks). Post-fix 600 s soak and boot smoke PASS (results
in the Task B commit). The baseline never survived past the ~135 s
time-set, so there is no baseline UART beyond the golden boot window to
diff against.

## 5. CTRL rotbCmdSta (0x54): strobe/status write masking (baseline-inherited defect)

The baseline model stores every CTRL write raw. The FSW's EDAC counter
reset for the rotating buffer (EDAC.c, `E_EDAC_MODULE_ROTBUF`) clears
its strobe with a full-register write of `~(1u << EDACRESET)` =
0xFFFFFFFB, so the model's `rotbCmdSta` read back with BLOCKCOUNT
(bits 23:7) = 131071. The FSW's HK report asserts when the rotating
buffer fill level (computed from BLOCKCOUNT) exceeds its 3-bit field
(`HKAcquire.c:325`) — level 8 > mask 7 — and halts.

**Why it surfaced now, and why it is a model defect and not FSW
behavior:** BLOCKCOUNT/SECCOUNT/READY/FULL/DEDETECTED are read-only
status on the real FPGA; a strobe write cannot touch them. The defect
is baseline-inherited (identical raw store in the 5.2 model) and was
masked for as long as the SCET `assert(0)` killed every run at ~60 s
FSW — 4 FSW-seconds before the first HK report after the EDAC strobe.
Fixing T1 unmasked it (two soak runs RED at FSW 00:01:04).

**Fix:** writes to 0x54 latch only the writable fields — COMMAND
(bits 1:0) and EDACRESET (bit 2); status fields ignore writes. The
FSW's GETBLOCK read-modify-write still lands in COMMAND.

**Companion repair — rotbCmdSta.READY:** with the raw store gone, the
FSW's `RotBuf_init` READY wait (bit 3, timeout fatal: "Fatal error: 1"
at init) exposed that the model never asserted READY at all — the
baseline passed it **by accident**, because the same EDAC strobe-clear
write 0xFFFFFFFB happened to set bit 3. The model now emulates the
real FPGA behavior: the rotbConfig (0x58) ENABLE bit (31) asserts
READY, disabling clears it.

Evidence: qtest `stix/ctrl-rotb` pins both (red shown: readback
`0xfffffff8` after the strobe pair; READY `(0 == 1)` after enable);
suite 6/6 green; boot smoke PASS (init path restored, golden UART
match intact). The EEPROM/SDRAM/CACHE EDAC
resets use read-modify-writes of single bits and are unaffected.

## 6. GPTIMER prescaler from the -kernel boot stub: 19, not 39 (baseline-inherited defect)

The boot stub in `hw/sparc/leon3.c` stood in for the IDPU boot PROM with
upstream's GPTIMER scaler reload of `39` — a 1 MHz tick for a 40 MHz
LEON3. The STIX machine runs at 20 MHz (`CPU_CLK`), so the timer ticked at
500 kHz. This section previously claimed the RTEMS BSP reprograms the
prescaler at boot; it does not: the BSW expects the boot PROM to have set
`IIR_GPT_PRESCALER` = 19 (`BSW_ASW/.../IIR.h`) and never writes it.

**Effect:** RTEMS assumes 1 us per timer count, so FSW uptime
(`Timing_getMainTime`, task periods, timeouts) ran at ~0.48x wall clock
while the CTRL SCET counter ran at the full rate — two FSW clocks that
never disagree on hardware. Measured on every soak log: 600 s wall ->
FSW ~290 s; 1800 s -> FSW 873 s. That ratio is also why the old soak
gate's false "stall" always appeared at FSW ~4.8 min
(`2026-11-09-task-b-stall-analysis.md`).

**Fix:** the stub writes `CPU_CLK / 1000000 - 1` (= 19). qtest
`stix/gptimer-prescaler` boots a spin-loop ELF under TCG and pins the
value (red shown: `39 == 19`). Boot smoke compares only the boot
sequence, so the golden stays valid; the steady-state cadence per wall
second doubles.
