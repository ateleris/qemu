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

## 9. Flash storage: sparse, inverted (D-I)

**What:** the flash backing file now stores every byte XOR 0xFF (new
qdev property `inverted-storage`, default `on`): a file byte 0x00 is
guest 0xFF. Erased flash is therefore a sparse hole, and a hole reads
back as erased.

**Why:** the host runs as a WSL VM on a nearly full Windows disk. The
FSW filesystem writes FLASH B (MCM1 chip 0, page 0x1d240 and up, about
9.4 GB into the file), so neither a short file nor a plain sparse file
works: holes read 0x00, which the FSW's boot scan classifies as
bad-block markers. Inverting makes the hole mean erased, which is what
a fresh 17.7 GB image actually is.

**Creation:** when the file does not exist, the inverted default creates
it with one `ftruncate` to 17,716,740,096 bytes — instant, zero blocks —
instead of the old 16.5 GB 0xFF fill loop (kept unchanged for
`inverted-storage=off`). A failed `ftruncate` is reported with
`error_report` + `exit(1)`, matching the existing open-failure handling
in the same function (`init_and_load_flash_file` takes no `errp`).
`stixflash_create` traces the event (path, size, inverted).

**Erase:** an inverted erase punches a hole over the containing block
(`fallocate(FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE)`, guarded by
`CONFIG_FALLOCATE_PUNCH_HOLE` like `block/file-posix.c`), clamped to
EOF so D-H is preserved (pages past EOF are skipped, the file is never
extended). The `FILE *` is `fflush`ed before the punch — buffered data
written after the punch would refill the hole. If the punch fails or
the build lacks it, the erase falls back to writing 0x00 pages
(`stixflash_punch_fallback` trace). Plain mode still writes 0xFF.

**Read/write:** reads invert the freshly read page into `page_data`
(beyond-EOF reads are unchanged: guest 0xFF, D-H). Writes invert the
guest image into a scratch buffer (`io_page`), so `page_data` always
holds guest bytes; beyond-EOF writes still fail with FAILED + IRQ and
never extend the file; the post-write `fflush` stays.

**Legacy and format incompatibility:** `inverted-storage=off` restores
byte-identical plain storage, which is what keeps all-0xFF files — such
as the frozen baseline copy `/tmp/stix-baseline-run/flash.bin` (never
touched) — usable. Conversely, feeding a PLAIN `flash.bin` to the
default reads all 0x00, i.e. every block bad: use `inverted-storage=off`
or convert the file by XOR 0xFF.

Evidence: qtest `stix/flash-storage` (missing file → sparse 17.7 GB with
a guest-erased read; write/read/erase round trip with host-side ~P and
hole checks; legacy mode with a 1 MB all-0xFF file), plus the reworked
host checks in `stix/flash-addressing` (~pattern, erased = host 0x00)
and the now hole-filled 1 MB common temp file. Red shown on the B2
binary: the guest read the zero-filled common file as 0x00000000, the
addressing host checks failed against plain-stored bytes, and QEMU
refused to start on the unknown `inverted-storage` property (the
missing-file case was guarded out of the red run — the B2 binary would
have filled 16.5 GB). All guest-visible expectations of the existing
tests are unchanged; the new tests are expected RED on the 5.2 wrapper.

## 10. CTRL watchdog (0xD4): modeled, golden regenerated (baseline-inherited defect)

**Baseline:** 0xD4 was plain storage. A write stored the raw command word
and a read returned it, so COUNT (bits 10:5) read back 0. The FSW Watchdog
task checks COUNT after every feed (`HWControl/Watchdog.c:199`). It logged
`Watchdog *E invalid number of cycles after reset: exp=63 hw=0` once per
second and raised the FDIR path, which the hardware never takes.

**Model (R3):** a command register, with semantics taken from the FSW
driver (BSW `WDT.c` header comment, `WDT.c`, `HWControl/Watchdog.c`):
- Not running at power-up or after a system reset.
- CHANGE sets RELOADVAL only. RELOAD loads COUNT from RELOADVAL and starts
  counting. DISABLE freezes COUNT, ENABLE resumes from it. FORCE resets
  the IDPU at once. SAFE does nothing.
- While running, COUNT drops by one every 105 ms (`WATCHDOG_CYCLE_DURATION`)
  on `QEMU_CLOCK_VIRTUAL`, counted lazily. At 0 the model requests a guest
  reset (~6.6 s after the last feed at the FSW's 63 cycles). With
  `-no-reboot`, QEMU then exits.
- A read returns RELOADVAL and the live COUNT.
- Trace events: `stixctrl_watchdog_cmd`, `stixctrl_watchdog_reset`.

**Open point:** the ELFs are built with `-DDISABLE_WATCHDOG`, yet
`Watchdog_feed()` still issues RELOAD every second. The model treats RELOAD
as "load and run", per the `WDT.c` header. Whether RELOAD restarts
counting while disabled is still to be confirmed against the FPGA/HSWICD
description (roadmap §3). Effect: a Watchdog task that stalls for over
~6.6 s resets the IDPU, and the soak reports it as an early QEMU exit
(exit 2).

**Golden regenerated** (`docs/stix-port/baseline/`, `run-flags.txt`). It
is now captured from this model, not the repaired v5.2 model, on the
**normal** FSW ELF `ASW_em_baseline.elf` (sha256 in `run-flags.txt`). The
traced build would make the golden depend on a debug configuration. The
flash is a device-created sparse, inverted file (D-I, §9). Against the old
golden, the normalized UART log differs **only** by the removed per-second
`Watchdog *E invalid number of cycles` lines; there are no other added or
changed lines. Two consecutive 90 s boots on the same flash give identical
normalized logs. `asm-head.golden.log` changes only as described in §1:
the SMP preamble, and the scaler at 19 instead of 39 (§6).
`qemu-stdout.golden.log` was recaptured from the same run; it is a manual
reference and is not gated.

**Soak gate:** with the watchdog modeled, the normal ELF's UART goes
silent after boot, so the soak's liveness gate is TM flow per 60 s slice
(commit "stix: soak heartbeat = TM flow").

**Evidence:** qtest `/sparc/stix/ctrl-watchdog`: power-up not running,
CHANGE keeps COUNT, RELOAD starts at 63, one cycle per 105 ms, the
DISABLE/ENABLE freeze, a 1 s feed cadence never expires, no feed resets
once the count runs out, reset stops it, and FORCE resets at once. It is
expected red on the 5.2 wrapper.

## 11. CTRL IRQ outputs wired per line (baseline: all on output 0, last-wins)

**Baseline:** `leon3.c` connected all five CTRL IRQ outputs with
`sysbus_connect_irq(dev, 0, …)`. Each call re-set the same QOM link, so the
last call won: only output 0 stayed connected, and it pointed at the ROTB
CPU IRQ line 7 — the detectors output (0) feeding the ROTB line, and
outputs 1–4 (hk, other, edac, rotb) left unconnected entirely.

**Model:** the first argument becomes `0,1,2,3,4`, matching the outputs'
realize order (`grlib_stixctrl.c`: detectors, hk, other, edac, rotb), so
each output reaches its own line: detectors→3, hk→4, other→5, edac→6,
rotb→7 (`StixIdpu.h` `IRQ_STIXCTRL_*`). The APB PnP entries are unchanged
(CTRL was always registered with IRQ 3).

**Guest-visible effect: none yet.** CTRL raises no IRQs in this commit;
the first device to pulse a CTRL output is the ROTB overflow path (A2,
D-F, line 7). The wiring change only decides *where* such a pulse would
land, so the FSW boot log is unchanged.

**Evidence:** qtest `/sparc/stix/ctrl-irq-lines`. It lists
`/machine/unattached` over QMP, finds the CTRL and IRQMP devices by their
`child<grlib-stixctrl>`/`child<grlib-irqmp>` type fields (no `device[N]`
hardcoding), and checks for i in 0..4 that `qom-get
<ctrl>/sysbus-irq[i]` — the QOM link each sysbus IRQ output exposes —
returns `<irqmp>/unnamed-gpio-in[3+i]`, the canonical path of IRQMP's
input pin. Red on the pre-fix binary: `[0]` returned
`…/unnamed-gpio-in[7]`; green after, all five match. It is expected red on
the 5.2 wrapper.

## 12. ROTB data path modeled (baseline: register store only)

**Baseline:** 0x54 (rotbCmdSta) and 0x58 (rotbConfig) were plain register
stores (with §5's write masking), and nothing else existed: no FIFO
state, no ring, no BLOCK window. The FSW's whole rotating-buffer
machinery — commands, BLOCKCOUNT, FULL, the 0xE0800000 window — read
zeroes and had no effect.

**Model (A2, D-A device-internal RAM):**
- **Composed status read.** 0x54 is no longer stored/read raw: the read
  composes the stored COMMAND/EDACRESET strobe bits (2:0) with live
  status — READY (bit 3) = rotbConfig.ENABLE, FULL (bit 5) = ring state,
  BLOCKCOUNT (23:7) = block count. DEDETECTED and SECCOUNT (31:24) read
  0 (no EDAC error model). The §5 READY emulation in the 0x58 handler is
  gone; ENABLE is the sole READY source now.
- **Commands execute on EVERY 0x54 write** (the FSW issues each command
  as a read-modify-write of 0x54, RotBuf.c:151-167, so no write
  re-issues a stale command): 1 GETBLOCK advances the read pointer
  (holds on empty), 2 PUTBLOCK advances the write pointer in TESTMODE
  only (ignored otherwise — in normal mode HW feeds blocks), 3 RESETFIFO
  clears the pointers. Commands run regardless of ENABLE (lenient),
  visible in the `stixctrl_rotb_cmd` trace. Pointers wrap at
  `rotb-blocks` (new qdev property, uint32, default
  `STIXCTRL_ROTB_MAX_BLOCKS` = 104448, realize error outside 1..0x1FFFF,
  the BLOCKCOUNT field width).
- **IRQ 7 rule (D-F):** reaching capacity sets FULL only. A PUTBLOCK
  into a full ring is dropped (drop-new) and pulses IRQ 7 once per
  rejected write. **Assumption:** that a *software* PUTBLOCK raises IRQ 7
  at all is unconfirmed until the HSWICD (roadmap §3); it is harmless
  because no FSW path writes into a full ring. The FSW's own fills reach
  exactly cap with IRQ 7 enabled since boot (HWSelftest.c:510-526,
  RotAccumulator.c:259-266) and must stay silent — covered by
  `rotb-full-irq` and `rotb-selftest-shape`.
- **BLOCK window** (MMIO index 1, mapped at 0xE0800000, one 512 B block,
  big-endian, 1–4 byte access): reads always show the block at the read
  pointer; TESTMODE writes land in the block at the write pointer.
  Window writes while full or outside TESTMODE are ignored (traced,
  `stixctrl_rotb_ignored`) and **never pulse IRQ 7** — D-F counts
  rejected PUTBLOCK commands, not stores (the FSW fills a block with
  hundreds of byte/word stores; pulsing per store would be an IRQ storm).
- **Q7 side effect, documented:** the EDAC strobe-clear write 0xFFFFFFFB
  carries COMMAND = 3, so it executes RESETFIFO. Harmless at the FSW's
  call sites (EDAC.c:305-306 callers), which clear the FIFO anyway
  (RotAccumulator.c:295, HWSelftest.c:569).
- **Reset keeps content:** a system reset clears the ring pointers and
  FULL but not the ring bytes (zeroed only at realize). The FSW issues
  RESETFIFO at init (RotBuf.c:78), so it never relies on content across
  a reset.
- **Host RAM:** ~53.5 MB per default instance (104448 × 512 B, g_malloc0
  at realize, freed at unrealize). Tests set
  `-global grlib-stixctrl.rotb-blocks=<4..8>`.
- **Boot impact to watch (corrections item 11):** BLOCKCOUNT now reports
  real values, so the FSW's HK level check (HKAcquire.c:325) sees them
  after the EDAC zero-fill and the self-test. Boot-smoke and a short
  soak (reviewer-side) must confirm no new UART line; any diff is a
  stop-and-report.

**Evidence:** qtests `/sparc/stix/rotb-fifo` (pointer arithmetic,
TESTMODE gate, empty-ring hold), `rotb-window` (markers through rd/wr,
byte-wise fill), `rotb-full-irq` (silent fill to cap, IRQ 7 on the
rejected PUTBLOCK, drop-new order, FULL clearing), `rotb-selftest-shape`
(the FSW fill-until-FULL/drain-compare algorithm, bounded loop),
`rotb-strobe` (strobe pair with blocks queued: count 3 → 0 via the Q7
RESETFIFO, no SECCOUNT garbage), `rotb-reset` (pointers reset, content
kept). All red on the pre-A2 binary at their first data-path assertion
(count stayed 0, window read 0, selftest fill exhausted its bound at
n=24); all green after. Existing `/sparc/stix/ctrl-rotb` passes
unchanged on the composed 0x54 read. All six are expected red on the 5.2
wrapper.

## 13. STIXCTRL AHB windows mapped (baseline: unmapped)

**Baseline:** the CTRL device registered only its APB register bank; the
whole 0xE0000000–0xE06000FF AHB data area was unmapped — writes were
dropped and reads returned 0.

**Model (A3, D-A device RAM, D-G):** four plain RAM regions on the CTRL
device, sysbus MMIO indices 2–5, mapped in `leon3.c`:

| Index | Base | Size | Contents (StixIdpu.h) |
|---|---|---|---|
| 2 | 0xE0000000 | 0x200000 | SRAM: ELUT, LACC0-3, EACC0-1, TLUT0-1 (`STIXCTRL_SRAM_SIZE`) |
| 3 | 0xE0200000 | 0x400 | LVAR BRAM incl. WDT `UNCHANGED_STORAGE` 0xE02000A8 (WDT.c:74) and `MemBram_STORAGE_ADDRESS` 0xE02003C0 (commonFSW.h:228) |
| 4 | 0xE0400000 | 0x100 | LTRG0-3 |
| 5 | 0xE0600000 | 0x80 | ETRG0-1 |

Sizes use the StixIdpu.h macros where they exist: SRAM from
`STIXCTRL_SRAM_SIZE`; LTRG/ETRG derived from their `*_END − *_START + 1`.
The LVAR window has no macro: 0x400 covers LVAR0-3 (4 × 0x100 stride,
ending 0xE020039F) plus the BRAM areas behind them — MemBram's 64 B end
exactly at 0xE0200400.

**Persistence (D-G):** the regions are zeroed at power-on (realize) only
and survive system reset — QEMU RAM is not cleared by the reset path, and
the CTRL reset handler deliberately touches nothing (comment in
`grlib_stixctrl_reset`). This matters now that R3's watchdog can reset
the guest: BRAM carries the FSW's reboot-persistent state. All widths
(8/16/32-bit) are accepted.

**Fidelity gap, not modeled:** the real BRAM keeps only the low 16 bits
of each 32-bit write (WDT.c:90-92). The model stores all 32 bits. No FSW
path reads the upper half is known; recorded here per the roadmap.

**Boot impact:** the FSW writes MemBram (a byte store, OpModeMgmt.c:950-954
→ MemBram.c:79-82) and reads and clears UNCHANGED_STORAGE (WDT.c:239-276)
at every boot. Those accesses hit unmapped space today; after this commit
they land in fresh RAM, which reads 0 exactly like unmapped space did, so
no golden diff is expected. Boot-smoke (reviewer-side) confirms it; any
diff is a stop-and-report.

**Evidence:** qtests `/sparc/stix/ahb-windows` (fresh instance reads 0;
distinct patterns across all four windows including the LACC banks, an
SRAM gap and each window's last word; 16-bit and 8-bit access at MemBram
with big-endian byte placement; window edges do not alias) and
`/sparc/stix/bram-persist` (UNCHANGED_STORAGE, a MemBram byte and an SRAM
word survive `qtest_system_reset`). Red on the pre-A3 binary: the first
write/read check failed (read 0x00000000 after the write). Both are
expected red on the 5.2 wrapper.

## 14. TM server off under qtest; realize errors instead of exit(1) (baseline: exit)

**Baseline:** the SPW realize always started the TMTC TCP server, and a
bind/socket failure ended QEMU with `exit(1)` after a stdout print
(`tmtc_io_server.c:200,206`) — so every qtest and every upstream generic
test (qom-test, cdrom-test) that instantiates `leon3_generic` fought over
port 20000+getuid() = 21000, and the suite ran only under the
`/tmp/stix-qtest-21000.lock` convention. The flash realize did the same
for its backing file: a `while (f == NULL)` creation loop whose create and
ftruncate failures `exit(1)` (`grlib_stixflash.c:574,582,596`), printing
only "can't create file <errno>" — no path, and on stdout.

**Model (P2-1):**
- New `grlib-stixspw` property `tmtc-server` (OnOffAuto, default `auto`).
  `auto` starts the server **except when the qtest harness drives QEMU**;
  `on`/`off` force it everywhere (boot gates and the GS use the default or
  `on`, and keep the server). With the server off, TM sends drop silently,
  exactly as with no client connected ("Error: socket is closed" print
  stays). `tmtc-port` keeps its meaning (0 = 20000+getuid()).
  - Predicate note (deviation from the plan's `qtest_enabled()`): the gate
    is `qtest_driver()` — the `-qtest` chardev connected — because
    `/sparc/stix/gptimer-prescaler` boots real guest code with
    `-accel tcg` while the qtest chardev is attached; with the
    accel-based check the server would still bind 21000 there and the
    held-port hermeticity gate would fail. `qtest_driver()` is set before
    machine realize (`qemu_create_late_backends()` runs before
    `machine_run_board_init()`), so it is reliable in realize.
- `init_tmtc_server()` returns bool and takes `Error **errp`; the
  `exit(1)`s became `error_setg_errno` messages that carry the port
  ("TMTC server: cannot bind 127.0.0.1:<port>"; strerror supplies
  "Address already in use"). `listen()` is checked too, and every socket
  error path closes the fd before reporting. Realize propagates through
  `&error_fatal` (`leon3.c`), so QEMU still exits, with the reason on
  stderr.
- Flash realize: the retry loop became a one-shot creation; create,
  ftruncate-size and reopen failures name the backing-file path via
  `error_setg_errno` ("stixflash: cannot create flash file <path>").
  A failed ftruncate closes the just-created file, clears `f` and unlinks
  it, so no empty backing file survives for the next run. No `leon3.c`
  change (both devices already realize under `&error_fatal`).

**Not changed:** the pthread accept/recv server itself (P2-4 moves the
transport onto the main loop); the `recv_packet_callback` TC stub; every
other `qemu_printf` (P2-2). The 5.2 wrapper strips the new
`grlib-stixspw.tmtc-server` global like `flash-file`. In the qtests,
`stix_private_tm_port_arg()` (the pid%20000 guess) is gone; tests hold
ports race-free with bind-:0+listen helpers. `stix_free_tcp_port()`
(release-then-rebind) comes back only when a test needs a port QEMU
itself will bind (P2-4 `tm-tcp`), where the window is unavoidable.

**Evidence:** qtests `/sparc/stix/tmtc-hermetic` (holds 21000 itself,
starts the machine, PnP readl alive; the hold is only skipped when the
port is genuinely busy (EADDRINUSE), other failures skip the test; red
before: "Bind failed... port: 21000, error: 98" then the qtest abort),
`/sparc/stix/tmtc-bind-error` (`g_test_trap_subprocess` child starts with
`tmtc-server=on` on a held port; asserts the child failed and stderr
matches one glob `*127.0.0.1:<port>*Address already in use*`; red before:
the property was unknown — "can't apply global grlib-stixspw.tmtc-server=on:
Property ... not found"), `/sparc/stix/flash-create-error` (child passes
`flash-file=<tmpdir>/missing/flash.bin`; asserts the child failed and
stderr matches `*cannot create flash file <path>*`; red before: exit(1)
with "can't create file 2" on stdout, nothing on stderr). Green: all 23
tests, and the full suite without the port lock while a listener holds
21000; upstream `qom-test -p /sparc/qom/leon3_generic` also passes with
21000 held (red before with the same "Bind failed" exit). Note
`tmtc-hermetic` itself holds 20000+uid while it runs, so a qtest run must
not overlap a boot gate (baseline/README.md).

**On the 5.2 wrapper** the picture is different, and worse than "red":
the 5.2 SPW has no properties at all (it calls
`init_tmtc_server(cb, opaque)` unconditionally and always binds 21000).
`tmtc-hermetic` aborts the whole old-binary run (`qtest_init` aborts in
the parent), so `tmtc-bind-error` and `flash-create-error`, registered
after it, are **not run** there rather than red. `tmtc-bind-error` would
fail on the unknown `grlib-stixspw.tmtc-port` global (the wrapper strips
only `tmtc-server`), not on a bind. Side effect of dropping the old
`tmtc-port` arguments from the existing tests: they no longer abort on
5.2 over an unknown property; with 21000 free they run against the old
device. The exact 5.2 expected-red/not-run list comes from the per-commit
wrapper run (plan §6).

## 15. SPW/TMTC/CTRL console prints are trace events (baseline: qemu_printf)

**Baseline:** every SPW, CTRL and TMTC register access, TM send and
socket event printed to stdout unconditionally via `qemu_printf` (14 live
prints in `grlib_stixspw.c`, 8 in `tmtc_io_server.c`, 3 in
`grlib_stixctrl.c`), and the flash realize printed its backing-file path,
creation notice and size there too (5 prints). Under qtest this polluted
every test's output, and a TM send without a client printed two lines per
packet.

**Model (P2-2, task F1):**
- All live prints in the four device files became trace events, keeping
  the verbatim message texts and arguments: 13 new `stixspw_*` events
  (MMIO read/write incl. the outside-register-block guards, dmaControl,
  TX descriptor, buffer dump, send), 8 `stixtmtc_*` (listen, accept,
  client, recv, TM send / no-client), `stixctrl_io_read`/`stixctrl_io_write`
  added to the CTRL section and `stixflash_page_size`, `stixflash_file`,
  `stixflash_file_size` to the flash section — 26 new events, 38 `stix*`
  events in total. The default (log) backend honours `-trace 'stix*'`;
  nothing else about the build changes. A normal qtest realize now prints
  nothing at all.
- Only two flash realize prints stay user-visible on stderr, in the
  file's `stixflash:` house style: `info_report` for the one-off
  missing-file creation ("stixflash: flash file %s missing, creating a
  new one") and `warn_report` for a failed close. The backing-file path
  and size, like the "page size" constant, are trace events
  (`stixflash_file`, `stixflash_file_size`, `stixflash_page_size`) — a
  deliberate deviation from the plan wording "trace_ or warn_report" for
  the path/size pair, which were plain status lines, not warnings.
- The bare `\n\n\n` decoration prints around the TX dump carried no
  information and are gone; the "Use %u bytes" line survives as
  `stixspw_tx_bytes`, and the per-word dump is one trace line per word
  (`0x%08x`), so the recaptured golden's TX dump layout is one line per
  word instead of one wrapped paragraph. The dead `nCode`/WSAStartup and
  `inet_addr` INADDR_NONE branches in `tmtc_io_server.c` (constant-false,
  unreachable) are deleted outright, so the file has no `qemu_printf`
  left. No device behaviour change: register semantics, TX path,
  transport and realize flow are untouched, and the commented-out prints
  stay as they are.
- Trace calls from `tcHandler` run on the raw pthread (not RCU-registered);
  the log backend takes `rcu_read_lock` there — harmless today, and P2-4
  moves the transport onto the main loop anyway.
- `capture-golden.sh` passes `-trace 'stix*'` (its golden log already
  merges stdout+stderr), so the recaptured golden keeps the same
  information, one `stix*` event per line. `run-qemu-gs.sh` enables
  `-trace 'stixtmtc*' -trace stixspw_tx_send
  -trace stixspw_recv_packet_callback` and keeps `^stix` lines plus
  `qemu-system-sparc:` error/warning reports — the old "Send packet len"
  and "recv_packet" content survives as `stixspw_tx_send` /
  `stixspw_recv_packet_callback`. `boot-smoke.sh`/`soak.sh` only tail the
  stdout log, so they are unaffected; their qemu-stdout logs simply
  contain no device prints unless `-trace` is added.

**Not changed:** the guest-reachable `error_report` on a flash host-I/O
error (`grlib_stixflash.c`, once per instance) — not a `qemu_printf` and
not in the plan's print list; the pre-existing `stixflash_*` /
`stixctrl_*` events; and the live `qemu_printf`s in `grlib_stixwd.c`
(device compiled but never instantiated, not in the P2-2 list).

**Evidence:** the new guard script `scripts/stix/tests/test-no-console-prints.sh`
(runs the `spw-tx`, `ctrl` and `flash` qtests with stdout+stderr captured;
fails on a non-zero exit, a reported skip or failure, or any old console
print, with the flash realize patterns from `grlib_stixflash.c`; covers
the MMIO, TX/no-client and realize paths — the TMTC server paths are off
under qtest, P2-1): red before — "SPW |", "CTRL |" and "page size: "
matched all three tests — green after. All 23 qtests pass.
`-trace help | grep -c stix` = 38 in the default backend, and a
`spw-tx` run with `-trace 'stix*'` shows the events carrying the old
texts (`stixspw_io_write SPW | Write completed addr = 32, ...`).
