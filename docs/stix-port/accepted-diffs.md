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

## 7. Flash controller error contract: STATUS.FAILED instead of abort (baseline asserts)

The baseline flash controller carried 14 `assert`s reachable from guest
stores (`grlib_stixflash.c` write/read handlers and the three execute
routines): a register written out of the ram→page→cmd order, a read
mid-sequence, a read of an offset other than 0, a write past the register
file, an unknown command (including 4 = `COMMAND_FSM_ACCESS`, which the
FSW driver defines but never issues), a page operation past EOF, and any
host I/O short count all **aborted QEMU**. Findings E1.

**New behavior:** every failure ends with the FSW's error contract —
STATUS.FAILED (bit 24) and STATUS.READY (bit 30) set, BUSY (bit 4)
cleared, the three-write sequence aborted, one IRQ 1 pulse, and a
`stixflash_error` trace (`-trace 'stixflash*'`). The FSW's ISR
(`BSW_ASW/src/BSW/Flash/Flash.c:138-148`) waits for the IRQ, then
requires BUSY==0 && READY==1 before it reads FAILED, so FAILED always
travels with READY and the IRQ. Per failure kind:

- Sequence violation (a register written out of order): the command
  fails; the next correct three-write sequence works normally and its
  success path clears FAILED.
- Unknown register offset write: ignored with a trace, no IRQ, no state
  change. Reads: offset 0 returns cmdSta as before; offsets 4/8 return
  the stored page/ram address; a read mid-sequence or of an unknown
  offset is tolerated (traced, no assert).
- Unknown command: the command fails (`flash_fail`), no abort.
- Host I/O failures (short fread/fwrite, failed seek): the command
  fails; one `error_report` per device instance, not per page.

**D-H (backing capacity = file size):** a page is beyond EOF when
`offset + FLASH_PAGE_SIZE > file_size` (this also fixes the baseline's
`+ 1` off-by-one, which demanded a phantom extra byte after the last
complete page). Reads past EOF return an erased page (0xFF) and succeed;
erases skip the past-EOF pages and succeed without touching the file; a
write past EOF fails with FAILED and never extends the file. Why: a
sparse write would leave holes that read 0x00, which the FSW boot scan
classifies as bad-block markers (`BSW_ASW/src/BSW/Flash/File.c:328`, `badBlockMarker != 0xFF`).

**No change for the FSW:** with a full-size, all-0xFF `flash.bin` every
FSW-issued sequence (RESET over all 16 chips, in-order ram→page→cmd
writes, offsets well inside the file) takes exactly the same path as
before. The FSW only ever checks FAILED for ERASE/WRITE; the read path
never reads it (`Flash.c:169-171`), consistent with beyond-EOF reads
succeeding.

Evidence: qtest `stix/flash-error-paths` pins all six failure kinds
(red shown: SIGABRT `grlib_stixflash.c:240` on the first out-of-order
PAGEAD write); the existing `stix/flash` and the full suite (8 tests)
stay green. New tests use a private TM port.

**Addressing (partly):** file positions and the file-size read are now
64-bit (`fseeko`/`ftello`, `off_t`), which removes the 512 MiB `ftell`
truncation. The offset is still the raw pageAddress register x 4224, so
chip >= 1 no longer wraps into chip 0 but lands at the raw-register
offset; the correct chip stride (2^18 pages) and the MCM bit follow in
the next section (B2), which lands together with this one.

## 8. Flash addressing: 64-bit offsets, chip stride, MCM (baseline-inherited defect)

The baseline computed the file offset as the raw pageAddress register x
4224 in 32-bit math. The FSW issues a linear page address split as
mcm = addr>>21, chip = addr>>18, page = addr & 0x3FFFF
(SPLIT_ADDRESS, `BSW_ASW/src/BSW/Flash/Flash.c:70-73`) and encodes it as
pageAddress = chip<<20 | page, cmdSta bit 3 = mcm (Flash.c:81-84). Three
defects followed: chip c was placed at c·2^20·4224 instead of the real
c·2^18·4224 chip stride (misaligned inside chip 0); the MCM bit was
ignored, so FLASH B's control region aliased exactly onto FLASH A's
(MCM1 block 64 == MCM0 block 64); and the 16.5 GB file was truncated to
512 MiB through the uint32 `ftell`, after which reads/erases of chip-0
pages >= ~127,100 aborted (that truncation is already gone with §7's
64-bit file size — this section fixes the remaining two).

**Fix:** the offset is now
`((mcm * 8 + chip) * (4096 * 64) + (page & 0x3FFFF)) * 4224`, computed in
uint64_t from the already-decoded chip (pageAddress bits 22:20) and MCM
(cmdSta bit 3). An erase addresses the block CONTAINING the commanded
page — base = page & ~63, 64 pages, never crossing a chip. The FSW
driver aligns the page itself before issuing
(`Flash.c:251-254`, `page &= ~0x3F`; the corrections addendum cites
:254), so the device-side containing-block rule is defensive, not
FSW-driven. Every write and erase is `fflush`ed before the completion
IRQ (no `fsync`), so the host side can observe the page while QEMU is
still running.

**Chip 0/MCM 0 offsets are bit-for-bit unchanged** (page·4224), so
existing `flash.bin` content stays valid; behavior for every FSW-issued
sequence on the full-size file is unchanged except that data now lands
where the FSW expects it. Evidence: qtest `stix/flash-addressing` on a
full-size sparse (17,716,740,096 B, holes read 0x00) backing file pins
chip-0 compatibility (guard), the chip-1 stride at 1,107,296,256, the
MCM0/MCM1 separation at 17,301,504 vs 8,875,671,552 plus a device-side
read-back, a write/read past the old 512 MiB truncation point (guard,
green since §7), and the containing-block erase (commanded with page 10:
page 63 erased, page 64 preserved). Red shown on the B1 binary: the
chip-1 write landed at the raw-register offset 4,429,185,024, the MCM1
write overwrote MCM0's page, and the unaligned erase wiped pages
10..73. The new tests are expected RED on the 5.2 wrapper (they pin
B1/B2 behavior the baseline lacks; `flash-error-paths` likewise).
