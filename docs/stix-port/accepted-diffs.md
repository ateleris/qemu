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
hardcoding; since §26 the CTRL is at the fixed path `/machine/stixctrl`
and only the IRQMP is found by type), and checks for i in 0..4 that `qom-get
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

## 16. SPW register semantics: W1C, reserved bits, IRQ gating (baseline: write clobbered status/dmaControl, IRQ always)

**Baseline:** the SPW write handler raw-stored the register, then forced
`status = 0xA00000` and cleared `control.RS` on **every** write to any
register, re-processed `dmaControl` on every write (TX on TE, then
`dmaControl = PS or 0` — wiping the latched bits), and pulsed IRQ 13
unconditionally on every TX. The FSW's `SPW_open` only passed because
its clear write (no TE) reads back 0; the latched write `0x181e` read
back 0, and any non-dmaControl write destroyed the dmaControl state.

**Model (P2-3):**
- **status:** W1C for TO/CE/ER/DE/PE/WE/IA/EE; LS (23:21) stays at RUN
  and nothing sets the error bits (recon-fsw §5: "the model should never
  set the error bits"). status is only touched by status writes now.
- **control:** raw store; RS self-clears (on the control write, instead
  of on every write) and drops any latched IRQ — the minimal core-reset
  semantics, so a latch from before `SPW_close` (SPW.c:622-632) or the
  AHB-error reinit cannot be pulsed by the next `SPW_open` IE 0→1
  write; a control write that takes IE 0→1 pulses the latched IRQ.
- **dmaControl:** PR/PS/TA/RA are W1C; TE/RE/TI/RI/AI/RD/NS/LE
  latch as written (the FSW's RMWs carry them, SPW.c:264-360, 725);
  RX (bit 10) is the read-only reception status and AT (bit 9) a
  self-clearing strobe — the model never sets them, so writes are
  ignored and both read 0; bits 15:13 and 31:17 read 0, so `0xFFFE01E0`
  reads back 0 and `0x181e` reads back its latched bits.
- **TX:** runs only on a dmaControl write with TE set; TE always clears,
  armed or not (correction 9); TXDSC_EN gates it (clear: no send, no
  write-back, no PS, no IRQ, traced as `stixspw_tx_skip_en`); the
  completion sets PS and needs TXDSC_IE **and** dmaControl.TI for the
  IRQ.
- **D-M (assumption, revised by the P2-5 review):** control.IE gates
  every SpW IRQ, unlike stock GRSPW where IE gates only link-error
  IRQs. For TX (and, from P2-7, tick) an event arriving while IE = 0
  is latched (`irq_latched`) and the IRQ is pulsed once on the next
  IE 0→1 write. For RX latching is not enough: PR set inside the
  FSW's `SPW_send` IE=0 window would be eaten by its own
  `dmaControl |= TE` read-modify-write (W1C, SPW.c:725) before the
  deferred pulse, and the ISR would skip the unit. A unit arriving
  while IE = 0 is therefore **not delivered at all** — it waits in
  the no-spill queue (RD untouched) and the IE 0→1 edge retries the
  queue before the latched pulse. Invisible to the FSW: the link
  back-pressures for a few µs. Traces: `stixspw_irq_latched`,
  `stixspw_irq_deferred`.
- **Reset handler:** registers go to their power-on values
  (status = 0xA00000, everything else 0) and the latch clears; guest
  RAM and the TMTC transport (P2-1) are untouched. The RX queue and
  the descriptor selector of P2-5 clear here too. This is a
  behaviour change for warm
  resets: a guest-triggered system reset (the CTRL/watchdog reset
  request) now restores the SPW registers, where the baseline kept
  them. It is FSW-neutral because `SPW_open` runs again after every
  reset.
- Trace note: `stixspw_io_write`/`stixspw_dma_control` now log the
  post-write register value (after W1C/latch and TX processing), so the
  trace stream shows the read-back state.

**Pinned-test edit (decision of 2026-11-10, correction 1):**
`test_spw_tx` now arms the descriptor exactly as the FSW does —
ctrl `0x7000` = EN|WR|IE (SPW.c:718-721) — sets control.IE first (the
SPW_send critical section, SPW.c:707-731) and writes `TE|TI` (the
SPW.c:725 `dmaControl |= TE`). Every other expectation is unchanged.
On the pre-P2-3 binary the edited test still passes, which shows the
edit only adds bits the FSW uses; without it, the new IE/TX gating
would have broken a pinned test (a plan stop condition).

**Correction 15 (golden):** in the golden stdout the ISR runs inside
the IE=0 window (stdout lines 16359-16364); after D-M it runs after the
IE restore — in every SPW_send cycle, not only there — so the stdout
order changes. The UART is not expected to change — boot-smoke and a
short soak (reviewer-side) confirm it; any UART diff is a
stop-and-report.

**Evidence:** qtest `/sparc/stix/spw-regs` (the full SPW_open sequence
with read-backs, including the RX/AT write behaviour; status W1C; PS
set/W1C/kept across another-register writes; TE clearing with and
without TXDSC_EN; the IRQ matrix — deferred while IE=0, direct with
IE=1, absent with TXDSC_IE or TI clear, each proven to have run a TX by
the descriptor EN clear and a fresh PS; RS dropping the latch; reset
restoring power-on values and clearing the latch, with positive
controls on both sides of the reset). Red on the pre-P2-3 binary at the
first failing read-back: `0x181e` read `0x00000000`, wanted
`0x0000181e`. Green: `spw-regs`, the edited `spw-tx`, and all 24 tests.
checkpatch on the diff: 0 errors, 0 warnings. The `stix*` trace-event
count rises 38 → 41 (`stixspw_tx_skip_en`, `stixspw_irq_latched`,
`stixspw_irq_deferred`).

## 17. TMTC transport on the main loop; non-blocking TM with whole-packet drop (baseline: pthread, blocking send)

**Baseline:** a raw `pthread` ran the whole transport: `accept`/`recv`
blocked on its own thread (writing `clientSocket` with no lock), TC
bytes reached the device callback without the BQL, the client fd was
closed twice per disconnect, and — the real defect — `send_tm_packet`
called blocking `send()` on the vCPU/main-loop path: with a stalled
client the whole of QEMU froze. A direct probe against the pre-P2-4
binary (stalled client, SO_RCVBUF 1024, never reading) hung the machine
on the **8th** 4 KiB TX; the qtest socket stopped answering.

**Model (P2-4, D-N):**
- The transport lives in a `StixTmtc` state struct owned by the SPW
  device — no file globals, no thread. One listening socket, one
  client, everything in `qemu_set_fd_handler` main-loop handlers under
  the BQL: `qemu_socket`/`qemu_accept` (CLOEXEC), non-blocking via
  `qemu_set_blocking(fd, false, errp)`.
- **TM send never blocks.** The exchange is queued into a 256 KiB
  whole-packet FIFO and flushed from the client's writable handler. A
  packet that does not fit the queue is dropped **whole** (never split
  on enqueue), counted in the read-only QOM property `tm-dropped` and
  traced (`stixtmtc_tm_dropped`); a partial `send()` keeps its
  remainder at the queue head and is finished before anything else, so
  no half packet ever reaches the wire (that would desync the CCSDS
  framing). With **no client** the packet is dropped silently
  (`stixtmtc_socket_closed` as before) and is **not** counted in
  `tm-dropped` — the counter means "a client was connected and the
  model had to drop".
- **One client.** A second connection waits in the listen backlog
  (listen(1)) until the first disconnects. The accept handler is
  unregistered while a client is connected — QEMU fd handlers are
  level-triggered, so with a pending second connection the old
  always-armed handler returned on every main-loop pass and
  busy-spun the loop at ~93 % of a core (P2-4-review M1, shown with
  a /proc CPU-time probe, see Evidence). A disconnect closes once,
  clears the queue and re-arms the accept. Unrealize removes the
  handlers and closes the fds.
- **STIX_CONTROL strip (D-K):** the handshake is stripped when it forms
  the first 12 bytes of a connection (traced
  `stixtmtc_token_stripped`); anything else at the stream head is TC
  data and goes through. A stream head shorter than 12 bytes that is
  not the token is held back until more bytes arrive (and is dropped
  with the connection if it disconnects) — no real client does this:
  both tools send the 12-byte `STIX_CONTROL` first and the smallest
  TC unit is ≥ 16 bytes. The token split across recv chunks and the
  token-less head get their tests with the P2-6 TC work (`tc-tcp`).
  TC bytes are received into a per-connection buffer and handed to
  the device callback in the main loop; in this commit the callback
  still traces and drops — reassembly across recv chunks is P2-6.
- **Wire format unchanged:** TM is the raw `[4-byte SpW header][CCSDS
  packet]` exactly as DMA'd, no length prefix, nothing else on the
  stream. `scripts/stix/tmtc-probe.py` and
  `scripts/stix/gs/stix_gs_bridge.py` work unchanged.
- New read-only QOM properties on `grlib-stixspw`: `tm-dropped`
  (counter) and `tmtc-connected` (accept state — the test-side
  observable for correction 16's "wait for the accept before driving a
  TX"). No vmstate exists for the device (pre-existing). A future one
  carries `irq_latched` and `tm_dropped` only and starts with an empty
  queue and no client: the TCP transport does not migrate, so the
  queue and the connection are re-created empty on the destination.
- Trace changes: `stixtmtc_wait_client` is gone (no accept loop), and
  `stixtmtc_tm_dropped`/`stixtmtc_token_stripped` are new.

**Not changed:** the `tmtc-server`/`tmtc-port` contract (P2-1: OnOffAuto,
auto = off under qtest, `errp`/bool init with the same bind-error text —
`tmtc-hermetic` and `tmtc-bind-error` pass unchanged); the device reset
semantics (P2-3: a system reset does not touch the transport); the dead
TSIM framing comments are gone with the rewrite.

**Evidence:** qtests `/sparc/stix/tm-tcp` (connect + token + bounded
accept wait + one TX + byte-exact read of the 8 DMA'd bytes on a
non-blocking socket, then a non-blocking read that must hit EAGAIN —
nothing else on the stream), `/sparc/stix/tm-stalled-client` (small
SO_RCVBUF client that never reads, 2000 × 4 KiB TXes, every qtest round
trip returns, wall-clock bounded, `tm-dropped` > 0; every TX is
numbered in its first data word and the drained stream is parsed on
whole-packet boundaries: only whole packets, strictly increasing
numbers, gaps only from drops, received + dropped == sent — the
no-split invariant the bridge's CCSDS splitter depends on; 72 packets
received, 1928 dropped) and `/sparc/stix/tm-reconnect` (B waits in the
backlog while A is served; after A disconnects the re-armed accept
takes B and the next TX is byte-exact on B). Red on the pre-P2-4
binary: all three wait for `tmtc-connected` and time out with
"tmtc-connected is unknown" (the direct probe above shows the hang the
new transport removes). The review M1 spin was shown on the pre-fix
binary with a /proc CPU-time probe (A connected, B in the backlog, no
traffic: ~93 % of a core over 3 s, vs 0.0 % with only A; post-fix
0.0 % in both states) — a qtest cannot observe the spin, so the probe
stays out of the suite on purpose. Green: all three TM tests,
`tmtc-hermetic`, `tmtc-bind-error`, `spw-tx`, `spw-regs`, and all 27
tests. checkpatch on the diff: 0 errors, 0 warnings.

## 18. SPW RX descriptor DMA with a no-spill queue (baseline: dead TSIM code)

**Baseline:** the whole RX path was dead TSIM code inside
`recv_packet_callback` (commented out since the qemu port): an 8-byte
TSIM prefix skip, host-endian dword swaps, `tsim_read/write_cpu_mem`
and a `length < 8 ? 8` length floor. No unit ever reached guest
memory; TC bytes were traced and dropped.

**Model (P2-5, FSW-RX §2, D-L):** `stixspw_rx_deliver(s, buf, len)` —
- The unit is delivered when **control.IE is set**, the descriptor at
  `dmaRxDescAddr + 8*rx_index` is armed (EN) **and** dmaControl has
  RE and RD set; the descriptor words are read with big-endian
  helpers (correction 16). An IE=0 window is the FSW's `SPW_send`
  critical section: PR set inside it would be eaten by the FSW's own
  `dmaControl |= TE` read-modify-write (W1C, SPW.c:725) and the
  deferred ISR would skip the unit (M1 of the P2-5 review), so the
  unit waits in the queue — **without touching RD**, which is a
  DMA-channel condition, not a descriptor one — and the IE 0→1 edge
  retries the queue before the latched TX/event pulse.
- Delivery DMAs `min(len, dmaMaxRxLength)` bytes to `daddr`, writes
  ctrl back as `min(len, dmaMaxRxLength) | TR` when truncated
  (correction 11: the length field is the byte count written, so a
  300-byte unit gives `252|TR`), keeps WR/IE, clears EN, advances the
  selector (WR wraps to the table base, and the selector itself wraps
  at the 128 descriptors of DESCSEL), sets PR, and raises IRQ 13 when
  RI and the descriptor's IE are set — delivery only happens with
  control.IE set (see M1), so the pulse is direct.
- **D-L (no-spill queue):** depth 64, matching the FSW's TC queue
  (`configFSW.h:172`). A unit that arrives while 64 are pending is
  dropped whole and counted in the read-only QOM property
  `rx-dropped`. RD is cleared **only** when a unit is pending on a
  descriptor condition (correction 10 — `spw-regs`'s `0x181e`
  read-back stays valid); the queue is retried on every dmaControl
  write with RD set (the ISR's post-re-arm write, `SPW.c:349-350`)
  and on the IE 0→1 edge. FIFO order is never jumped. The model is
  **always no-spill**: NS=0 (spill) is not honoured — the FSW always
  sets NS (`0x181e`).
- **Selector:** the descriptor table is `dmaRxDescAddr` masked to its
  1 KiB alignment (DESCBASEADDR, bits 31:10) with DESCSEL bits 9:3 as
  the selector (`StixIdpu.h:1451-1456`); a write sets both, and a
  **read returns `(base & ~0x3ff) | (rx_index << 3)`** — the readback
  follows GRSPW, where the core exposes the selector in the register.
  The FSW never reads the register (its only access is the write at
  SPW.c:545), so the readback is invisible to it. `rx_index` wraps at
  128. A device reset clears the queue, the selector and the register
  (power-on value 0, visible in the readback).
  `rx-dropped` is a lifetime counter, like `tm-dropped`.
- The dead TSIM code is deleted; `recv_packet_callback` stays as the
  TCP hook — P2-6 wires its reassembly to `stixspw_rx_deliver`.

**Test path:** the write-only QOM property `rx-inject` (a hex string,
spaces allowed) delivers one unit per set. It is **qtest-only**: the
setter refuses with an error unless `qtest_enabled()`, so a real run
or the boot gates cannot inject, and there is no guest-reachable
path into it.

**Not changed:** the registers a delivery does not touch, the TX path,
and the P2-4 transport (`spw-regs`, `spw-tx`, all TM tests pass
unchanged). No vmstate yet (pre-existing); a future one must carry
the RX queue (all queued units), `rx_head`, `rx_count`, `rx_index`
and `irq_latched` besides the P2-4 state, or in-flight TCs are lost
on migration.

**Evidence:** qtest `/sparc/stix/spw-rx` — two descriptors armed as
the FSW arms them (`SPW.c:413-434`, `IE|EN`, WR on the last,
252-byte buffers, `dmaMaxRxLength` 252, control.IE, dmaControl
`0x181e`), driven with real TC(17,1) packets (FSW-RX §6): (a) unit 1
lands byte-exact in buffer 0, ctrl = len with EN cleared and IE kept,
PR set, IRQ 13 pending; (b) unit 2 lands in desc1 and the next unit
wraps to desc0 only after it is re-armed (queued meanwhile, RD
cleared); (c) a 300-byte unit gives ctrl `252|TR` and leaves the
guard after the buffer untouched; (d) with both descriptors unarmed
there is no guest write at all, RD is cleared, and a re-arm plus an
RD write delivers; (e) 65 queued units give `rx-dropped` = 1 and the
first 64 are delivered in order as the descriptors are re-armed;
(f) **M1 of the P2-5 review**: with control.IE = 0 (the `SPW_send`
window) a unit is held — no DMA, no ctrl write-back, no PR, RD
untouched — then the FSW's own sequence runs (a TX, then
`dmaControl = readl(dmaControl) | TE`), and the IE 0→1 edge delivers
the unit (ctrl = len, byte-exact, PR set, IRQ 13 pending); plus the
gates: RI = 0 and descriptor IE = 0 deliver without an IRQ, RE = 0
queues until RE and RD are written; (g) `qtest_system_reset` clears
the queue and the selector: the selector is 1 before the reset (a
unit was delivered into desc0) and shows in the register readback
(GRSPW DESCSEL, bits 9:3 of `dmaRxDescAddr`); after the reset the
readback is 0, the base rewrite plus the RD write deliver nothing
(the queued unit is gone), and a fresh unit lands in desc0
byte-exactly. Red on the pre-P2-5 binary: `rx-inject refused: Property
'grlib-stixspw.rx-inject' not found`. Red for M1 on the pre-fix
code: the IE=0 inject was delivered at once (ctrl read `0x08000010`
= IE|len with EN already cleared, PR set inside the window) instead
of being held (`IE|EN`). Green: `spw-rx`, `spw-regs`, `spw-tx`,
`tm-tcp`, and all 28 tests. checkpatch on the diff: 0 errors,
0 warnings.

## 19. TC ingress over TCP with CCSDS-length framing (baseline: TCs dropped)

**Baseline:** the TCP callback traced and dropped whatever the socket
delivered; no TC byte could reach the guest (the dead TSIM path, §18).

**Model (P2-6, D-K):** per connection, after the `STIX_CONTROL` strip,
the receive path frames the stream into units of
`[4-byte SpW header][CCSDS Space Packet]`, back to back, with no
length prefix:
- the unit length is `4 + 7 + BE16(unit[8:10])` — the CCSDS packet
  length field at SP[4:6] (the TSIM 8-byte RX prefix stays dropped),
  and `QEMU_BUILD_BUG_ON(11 + 0xffff > TC_UNIT_MAX)` pins the cap
  instead of an unreachable runtime branch;
- units are reassembled across `recv` chunks; a unit is delivered
  **whole**, never before its full length has arrived;
- the resync rule: header byte 1 must be 0x02 — anything else drops
  one byte, counts one in the read-only QOM property `tc-resync` and
  traces `stixtmtc_resync`, then retries. `tc-resync` counts one per
  dropped byte and is never reset (like `rx-dropped` and
  `tm-dropped`);
- plausibility (P2-6 review m4, this implementer's call: implement):
  a head whose SP[0] is not CCSDS version 0, type TC with the DFH flag
  (the FSW rejects anything else, FSW-RX §3.2 checks 2-4) is treated
  like a bad byte 1 — resync. Without it a stray 0x02 at byte 1 of
  garbage could lock onto a bogus length and swallow up to 64 KiB of
  real TCs behind it. It is FSW-safe: a TC failing this check would be
  rejected by the FSW anyway;
- each complete unit goes to `stixspw_rx_deliver` (P2-5), which
  applies truncation, the no-spill queue and the IE-window hold; the
  device callback runs on the main loop, BQL held;
- a unit still pending when the connection closes is dropped with the
  connection (the framing state resets on accept and close);
- **no socket back-pressure (D-L, review m5):** the transport reads
  8 KiB per main-loop pass however full the RX path is, and never
  pushes back. Beyond the 2 armed descriptors plus the 64 queued
  units, TCs are dropped (counted in `rx-dropped`) while the socket
  keeps draining — a GS burst of more than ~66 TCs while the FSW is
  not re-arming loses TCs. Memory stays bounded: one 65546-byte
  framing buffer plus at most 64 queued units of at most 65546 bytes
  (≈ 4.2 MiB), since `rx_deliver` copies the whole unit even though
  only `min(len, dmaMaxRxLength)` is ever DMA'd. Dropping the read
  interest while the queue is full (and re-arming it from the retry)
  is a possible later improvement.
- Traces: `stixtmtc_tc_unit(len)`, `stixtmtc_resync`;
  `stixspw_recv_packet_callback` now carries the unit length. While
  the trace file was touched, the two legacy P2-4 texts became "TM
  packet handed to the transport" and "TM dropped: no client" (P2-4
  review n4); the event names are unchanged.

**Not changed:** the wire format (D-K, unchanged for
`scripts/stix/tmtc-probe.py` and `scripts/stix/gs/stix_gs_bridge.py`),
the TM side, the token-strip rule, P2-5's RX semantics, and the P2-1
server contract (`tmtc-hermetic`, `tmtc-bind-error` pass unchanged).

**Tests:** qtest `/sparc/stix/tc-tcp` — the token plus TC(17,1) seq 0
in three writes, then seq 1 + one 0xEE garbage byte + seq 2 in one
write: desc0 = seq 0 and desc1 = seq 1, byte-exact with lengths 16,
guard bytes after each 16-byte unit untouched, `tc-resync` = 1 (the
counter is checked instead of the one resync trace the plan asked
for — equivalent and stronger), and after a desc0 re-arm plus a
dmaControl RD write (correction 4) desc0 = seq 2. A disconnect
mid-unit (12 bytes, header parsed, body incomplete) discards the
partial unit; after a reconnect the next whole unit is delivered
byte-exactly and still with `tc-resync` = 1 (review m6). All waits
bounded. Red on the pre-P2-6 binary: the bounded fill wait expired
with desc0 still armed (`0x0a000000`) — no descriptor was filled, the
bytes were traced and dropped. `tmtc-probe.py --self-test` pins the
framer (split reassembly, the garbage-byte resync, garbage heads and
tails, the four real FSW TM units from the reviewer's run, the plain
argparse mode of soak/boot-smoke, and the two-send schedule) and the
expectation matcher — whose application data starts at **SP[16] for
TM** (the 10-byte TM data field header, `TMData.h:423`,
`TMData_DATA_FIELD = 16`, `TM_1.c:29-30`) and at SP[10] for TC
(P2-6 review B2). The end-to-end gate is `scripts/stix/tc-smoke.sh`:
boots the normal ELF like boot-smoke, sends TC(17,1) seq 0 at
`STIX_TC_SEND_AT` (20 s) and expects TM(1,1) `0x5A1:1:1:1DACC000`,
TM(17,2) `0x5A7:17:2` and TM(1,7) `0x5A1:1:7:1DACC000` within
`STIX_TC_WINDOW` (10 s), then — after the first window closes — a
corrupt-CRC TC and TM(1,2) keyed on `0x5A1:1:2:1DACC0010002`
(TCPacketID+SeqCtl+failureID 2, `TCBuffer.c:336-342`, review M2);
prints one `TC-SMOKE: PASS`/`FAIL (<which expectation>)`/`ERROR`
verdict line with exit codes 0/1/2, stops QEMU at the end, supports
`--dry-run`. tc-smoke on the normal ELF (reviewer's run,
2026-11-10): `TC-SMOKE: PASS`; TM(1,1), TM(17,2) and TM(1,7) for
TC(17,1) seq 0 in the same second, and TM(1,2) with FID 2 for the
corrupt-CRC TC 9 s later:

    EXPECT OK: 0x5A1:1:1:1DACC000 (unit len=24)
    EXPECT OK: 0x5A7:17:2 (unit len=20)
    EXPECT OK: 0x5A1:1:7:1DACC000 (unit len=24)
    EXPECT OK: 0x5A1:1:2:1DACC0010002 (unit len=32)

Known limitation: the TC plausibility check also requires the DFH
flag, so a TC without a data field header never reaches the FSW and
cannot be used to provoke its TM(1,2) rejection.
Green: `tc-tcp`, `spw-rx`, `tm-tcp`, and all 29 tests; boot-smoke;
soak 300 s; checkpatch on the diff: 0 errors, 0 warnings.

## 20. SpW time codes generated at the SCET rollover (baseline: none)

**Baseline:** nothing ever set `status.TO` from a time source; the FSW
never received a SpW time code. Its `Timing` task saw no code for 60 s,
set the UNSYNCED flag itself (`IIR_setElapsedTime`) and sent
TM(5,2) EID 0x542B "time code lost"; the golden's 60 s SCET write is
that path.

**Model (P2-7, D-J):** QEMU plays the spacecraft OBC — the time master
on the real mission — so the time codes are generated in QEMU, on
`QEMU_CLOCK_VIRTUAL`, not by the ground or a bridge:
- The SPW device holds a `scet-source` QOM link to the CTRL device
  (wired in `leon3.c` before realize); `timecodes=on` without the link
  fails realize with `errp`. The SCET is read through the new
  `stixctrl_scet_now()` accessor (grlib_stix.h), the same formula as
  the 0xC8 read; the coarse without the UNSYNCED bit is fine for
  TIMECNT. Correction 7 moved `OBJECT_DECLARE_SIMPLE_TYPE(STIXCTRL,
  GRLIB_STIXCTRL)` into `grlib_stix.h` so the link is typed.
- A `QEMU_CLOCK_VIRTUAL` timer fires at the next coarse rollover (the
  CTRL fine field gives the offset into the running second). On fire:
  if fine >= 0x1000 the code is **not** emitted and the timer
  reschedules to the true next rollover (traced) — the skip covers a
  timer callback that ran more than 62.5 ms late, which is possible
  without -icount when the main loop is starved; a synchronised FSW
  would adjust SCET toward such a code (`Timing.c:199-328`). A guest
  SCET set cannot reach this path: every 0xCC commit re-arms the timer
  through the hook below. Otherwise, and only when control.TR (bit 11)
  is set, it latches `time = coarse & 63` (TIMECNT bits 5:0, TCTRL
  7:6 = 0), sets `status.TO` and raises IRQ 13 when control.TQ (bit 8)
  is set — gated by control.IE with the D-M latch, like RX and TX.
  Then it schedules the next rollover. Traces:
  `stixspw_timecode_emit`, `stixspw_timecode_skip`,
  `stixspw_timecode_arm`.
- **The commit hook (correction 6):** after a guest 0xC8/0xCC commit
  the CTRL calls a registered callback (`stixctrl_set_scet_commit_hook`,
  internal API in `grlib_stix.h`); the SPW re-arms its timer, so the
  next code comes at the NEW rollover with the new coarse & 63 — exact
  after TC(9,129) too.
- **The `timecodes` property (correction 8):** default on,
  `object_property_add_bool` in instance_init, so it is settable at
  run time (`qom-set`) as well as before realize. `false` deletes the
  timer (a lost time code), `true` re-arms it.
- **Reset (correction 16):** the timer survives a system reset — QEMU
  timers are not deleted, and the CTRL keeps its
  `time_correction_ns`, so the SCET keeps running. The SPW reset also
  re-arms; that is belt-and-braces (a surviving timer fires at the
  same rollover either way). TR cleared by the reset suppresses codes
  until the FSW re-writes control at `SPW_open`.

**tc-smoke extension:** after the corrupt-CRC check it sends
TC(9,129) coarse 0x10000000 fine 0 (seq 2, the recon's bytes
`40 02 00 00 1D AC C0 02 00 0B 19 09 81 00 10 00 00 00 00 00 FA 63`)
and expects TM(1,7) `0x5A1:1:7:1DACC002@3` (the `@3` tightens that
one expectation to 3 s) plus a later heartbeat whose CUC coarse shows
the new SCET. The FSW's TM carries the SCET as CUC time in the PUS TM
data field header: coarse = SP[10:14] big-endian (unit bytes 14:18),
fine = SP[14:16] — confirmed on the real heartbeat stream (APID 0x5E5,
type 3 subtype 25, coarse +1 per second); `--expect-cuc 0x5E5:1000000`
hex-prefix-matches it. The probe self-test pins the offset with the
real units.

**Expected FSW change:** with a time code at every second, the FSW
boots "synced", never logs TM(5,2) EID 0x542B "time code lost" at
60 s, and never performs the 60 s SCET write; the golden UART should
only lose those time-sync lines — any other diff is a stop condition.

Gates (reviewer's run, 2026-11-10, normal ELF): boot-smoke PASS; soak
600 s PASS; tc-smoke PASS with all six expectations, and no TM(5,2)
EID 0x542B "time code lost" in its 90 s run:

    EXPECT OK: 0x5A1:1:1:1DACC000 (unit len=24)
    EXPECT OK: 0x5A7:17:2 (unit len=20)
    EXPECT OK: 0x5A1:1:7:1DACC000 (unit len=24)
    EXPECT OK: 0x5A1:1:2:1DACC0010002 (unit len=32)
    EXPECT OK: 0x5A1:1:7:1DACC002@3 (unit len=24)
    EXPECT OK: 0x5E5:1000000 (unit len=37)

The UART golden turned out identical after normalization: the
time-code-lost report is TM, not a console line, so there was no UART
diff at all. The stdout golden is recaptured: it gains the
stixspw_timecode_arm/emit events (one code per second) and the ISR's
extra SPW accesses, and loses the two stixctrl_scet_write events of
the old 60 s resynchronisation.

**Evidence:** qtest `/sparc/stix/spw-timecode`: (a) a rollover sets
`status.TO` with `time & 63 == coarse & 63` and TCTRL 0, IRQ 13
pending; (b) a TX in between (the spw-tx sequence) does not clear TO,
a write-1 does; (b2) a rollover with control.IE = 0 sets TO with no
IRQ and the IE 0->1 write pulses the latch (the `SPW_send` window);
(b3) with control.TQ = 0, TO set and TIMECNT latched but no IRQ;
(c) with control.TR = 0 a rollover gives no TO; (d) `qom-set
timecodes false` silences three rollovers, `true` re-arms; (e) a
guest SCET commit mid-second (0xC8/0xCC, as `test_ctrl_scet` does it)
moves the next code to the new rollover with the new coarse & 63
(correction 6); (f) after `qtest_system_reset` the timer is alive but
silent while TR = 0, and emits again after the control write. Red on
the pre-P2-7 binary: the first rollover gave no TO (`status.TO == 0`).
Green: `spw-timecode`, `ctrl-scet`, `ctrl`, `spw-regs`, `spw-tx`,
`spw-rx`, `tc-tcp`, and all 30 tests. checkpatch on the diff: 0
errors, 0 warnings.

## 21. UDP transport: one bare Space Packet per datagram (baseline: TCP only)

**Baseline:** the only transport is the TCP server (D-K units), so the
ground station — which speaks UDP with one bare Space Packet per
datagram — needed the Python bridge
(`scripts/stix/gs/stix_gs_bridge.py`) to strip and re-add the 4-byte
SpW header and to split and join the TCP stream on the CCSDS length.

**Model (P2-8, D-B as amended):** four new properties on
`grlib-stixspw`:

| Property | Type | Default | Meaning |
|---|---|---|---|
| `udp-tm-host` | string | 127.0.0.1 | TM datagram destination (numeric IPv4 only, no name resolution on the realize path) |
| `udp-tm-port` | uint32 | 0 | TM destination port; 0 = off |
| `udp-tc-port` | uint32 | 0 | local TC bind; 0 = off |
| `udp-spw-header` | bool | off | keep the 4-byte SpW header in both directions |

- The sockets are plain `qemu_socket(AF_INET, SOCK_DGRAM)`, non-blocking
  and **not connected** (D-N: QEMU's `char-udp` connects its socket,
  which would drop datagrams from the GS's ephemeral source port). They
  open at realize when the respective port is non-zero, independent of
  the TCP `tmtc-server` OnOffAuto — UDP works with the TCP server off.
  A port above 65535 is an `errp` error; a bind failure is an `errp`
  error carrying `127.0.0.1:<port>` and strerror. The TC socket is
  served by a main-loop fd handler, like the TCP side.
- **TM:** every TX is one `sendto` of one datagram, the 4-byte SpW
  header stripped unless `udp-spw-header=on`. Never blocks (a datagram
  is all-or-nothing by protocol). With TCP also on, one TX reaches both
  — TCP raw (D-K), UDP stripped.
- **TM failure accounting (item f, review m5):** `tm-dropped` is now a
  **per-sink** counter: the TCP queue-full drop and a UDP `sendto`
  failure each count, so one TX can count twice (once per failing
  sink) — or once, when only one sink failed. An unconnected UDP socket
  never sees ICMP port-unreachable feedback on Linux, so a TX with
  nothing listening is indistinguishable from a delivered one and is
  **not** counted — the datagram is silently lost, like the TCP
  no-client drop (the review's kernel check: `__udp4_lib_err` ignores
  ICMP for a socket that is not established unless IP_RECVERR is set,
  which the model never sets). Only synchronous `sendto` failures
  (EAGAIN, ENOBUFS, a bad route) are counted. Both UDP TM traces log
  the datagram length (stripped), not the DMA'd length.
- **TC:** each datagram from any source is exactly one unit. The model
  prepends `40 02 00 00` (what the bridge would add) unless
  `udp-spw-header=on`, then hands the unit to the same RX DMA path as
  TCP (no-spill queue, truncation, PR, IRQ). The size bounds follow the
  framing mode: a datagram smaller than a bare Space Packet header
  (7 bytes) — or `4 + 7` with the header kept, the TCP framer's minimum
  unit — or bigger than the largest possible unit is dropped and traced
  (`stixtmtc_udp_tc_dropped`); in stripped mode the ceiling is 65542
  (`TC_UNIT_MAX - 4`), which a 16-bit CCSDS length cannot exceed
  anyway. There is no resync logic: the datagram boundaries are the
  framing, and a malformed unit is the FSW's CRC problem, not a framing
  one.
- **Counters (review m5, decided):** UDP TC size drops are traced only,
  with no QOM counter — unlike the TCP `tc-resync` (which signals a
  framing problem on a healthy byte stream), a size drop means the
  sender is broken, and the trace is enough. UDP TM failures share
  `tm-dropped` (per-sink, above); a separate `tm-udp-dropped` was
  considered and not added.
- **The TC drain loop is capped** at 64 datagrams per wake-up (the
  no-spill queue's length): past that every further datagram is only
  `rx-dropped`, and draining a full socket buffer would hold the BQL
  where the TCP side reads one 8 KiB chunk per wake-up (review m4).
- **TCP unchanged** (D-K): framing, token strip, resync — all as in
  §17/§19. The `stixspw_recv_packet_callback` trace message drops its
  "TCP" prefix: it is now the callback for both transports. The stdout
  golden changes only by that trace text — the golden run has no TC
  traffic (no callback lines) and the `stixtmtc_udp_*` events fire only
  when a `udp-*` port is set; boot-smoke compares only the UART.
- `run-qemu-gs.sh --udp` starts the emulator with
  `udp-tm-port=21501` and `udp-tc-port=21502` (the GS's documented
  ports); the TCP server stays on 21500. **The bridge cannot run
  alongside `--udp`** (both bind 127.0.0.1:21502, and both would feed
  TM to 21501); it remains for TCP-only setups. `run-qemu-gs.sh`
  rejects unknown arguments, validates the port overrides, and prints
  `qemu.log`'s tail when QEMU dies at startup. The 5.2 wrapper strips
  the four `udp-*` properties like `tmtc-server`.

Gates (reviewer's run, 2026-11-10, normal ELF): boot-smoke PASS; soak
300 s PASS; tc-smoke over TCP PASS (all six expectations). The §5
end-to-end UDP run (QEMU `866ee37709` with `--udp` and ground station
`0dcb1af`, no bridge) passed. TC(17,1) `1DACC000000519110100D77E` gave
TM(1,1), TM(17,2) and TM(1,7). TC(9,129)
`1DACC001000B1909810010000000000035C6` gave TM(1,1) and TM(1,7) 4–6 s
later, and the heartbeat CUC counted on from 0x10000000. The transcript
is in the Phase 2 acceptance checklist.

**Evidence:** qtest `/sparc/stix/spw-udp`: (a) `udp-tm-port` alone (no
TCP server): one TX yields exactly one datagram, the DMA'd buffer minus
its 4-byte header, first byte CCSDS version 0, and a follow-up
non-blocking recv hits EAGAIN; (b) a bare TC(17,1) datagram from an
unrelated ephemeral source port fills an armed descriptor with
`40 02 00 00` + the SP (len 16, ctrl = IE|16), nothing beyond the unit
written; an undersized datagram (6 bytes, below the bare-SP floor) is
dropped with the descriptor untouched and the next valid one still
delivered; three datagrams sent back to back from one sender socket
come out in send order through the drain loop and the no-spill queue
— the kernel keeps order within one flow (the ground station sends
every TC from one socket), but does not order across flows: fresh
sockets per datagram reorder, measured at about two in three
three-datagram bursts, which is why the burst shares one sender; the
upper size bound is untestable over IPv4 loopback: a UDP payload
above 65507 fails `sendto` with EMSGSIZE, and the bound lies above
that; (c) with
`udp-spw-header=on` a TM datagram keeps the header, a TC datagram is
delivered unchanged, nothing beyond the unit; (d) TCP and UDP both on:
one TX reaches both, TCP raw and UDP stripped; (f) `udp-tm-port` with
nothing listening: three TXs (the loopback ICMP would only surface on
a later send, so a wrongly connected socket would count here) complete
with EN cleared, PS set, IRQ 13, QEMU alive and `tm-dropped` unchanged.
`/sparc/stix/spw-udp-bind-error`: an occupied `udp-tc-port` fails
realize with `127.0.0.1:<port>` and "Address already in use" on stderr
(the TCP server, when also enabled, is torn down on that path — not
externally observable: with `&error_fatal` the process exits, so every
fd dies with it; the cleanup call keeps the "a failed realize leaks
nothing" invariant). `/sparc/stix/spw-udp-host-error`: a `udp-tm-host`
that is not IPv4 fails realize with the value in the message. Red on
the pre-P2-8 binary: `Property 'grlib-stixspw.udp-tm-port' not found`
(the tc-port and host variants likewise in the subprocess tests).
Green: `spw-udp`, `spw-udp-bind-error`, `spw-udp-host-error`,
`tm-tcp`, `tc-tcp`, `spw-rx`, `spw-tx`, `spw-timecode`, and the full
suite. checkpatch on the diff: 0 errors, 0 warnings.

## 22. HK ADC returns nominal values (baseline: VAL 0)

**Baseline:** `hkAdc` (CTRL 0x8C) was plain storage with READY forced
on write, so a read returned `cmd | 0x80` and VAL (bits 31:20) was
always 0. The BSW boot self-test (1V5 1951..2193, 2V5 3357..3473, 2V9
3784..4094; HK.c:34-39) therefore failed and the boot report became
TM(5,3) 0x8002 + TM(5,4) 0xD420 instead of the success TM(5,1) 0x0022.

**Model (P3-1, D-P3a):** an `hkAdc` write now converts immediately and
the register becomes
`(VAL & 0xFFF) << 20 | 1 << 7 | (value & 0x7F)`, bits 19:8 zero. The
write replaces the whole register (a later write is not OR-ed into a
stale VAL). READY stays immediate (the real FPGA scans continuously;
only the first 130 ms after reset are pending, HK.c:142, and the FSW's
poll HK.c:130-158 therefore never waits). VAL comes from a
`static const uint16_t[128]` table (recon-hk-adc §4 "Proposed raw");
every value sits inside both the BSW self-test window and the PFM/IDB
soft limits (except ATT_C with two motors, see below), so the build
flavour (AX/PFM) and a flash context with flight limits do not matter.
Seven lines are overridden at conversion time from the CURRENT
commanded register state (`hkCmdSta`, `idefxConfig`, as they stand
when the write happens):

| Line | Rule |
|---|---|
| DPU_SPW_C (3) | 50 / 900 / 900 / 1830 for SPWEN (hkCmdSta bits 4:3) = 0/1/2/3 lines (`spw_c_limits[n]`) |
| DET_C (4) | `20 + 75 * popcount(idefxConfig & 0xF)` (`det_c_limits[n]`) |
| ATT_V (12) | 1670 when LVEN (bit 0), else 10 |
| HV_17_32_V (16) | 1000 when HV1732EN (bit 2), else 16 |
| HV_01_16_V (20) | 1000 when HV0116EN (bit 1), else 15 |
| SPW0_V (48) | 3240 when hkCmdSta bit 3 (SPWEN bit 0) is set, else 20 |
| SPW1_V (52) | 3240 when hkCmdSta bit 4 (SPWEN bit 1) is set, else 20 |

**POWERSTAT stays 0 (D-P3a, deliberate; superseded by §25, P4-1, which
composes it from the enables):** `hkCmdSta` bits 9:5 remain
plain storage — what is written comes back, nothing is synthesised.
With POWERSTAT = 0 the SpW voltage checks (`HKMonitor_spwCheck` gates
SPW0_V/SPW1_V on the status bits) and the ATT_V/HV checks
(`HKMonitor_regularCheck`) stay skipped, so `SpwControl_init` cannot
fall into offline mode and no new pass/fail outcomes arise. The values
do change, and so does the detector-temperature/TLUT path: the 64 s
average goes 0 → ~2450, which may re-select the TLUT or emit
TM(5,2) 0x542C (recorded, not tuned away). Synthesising POWERSTAT in
the same commit would arm those checks, and the SpW voltages would
then have to track SPWEN atomically or a PFM/context run falls offline
— the recon's "zero-risk step 1" line. The state overrides above exist
so SID2 HK and any later POWERSTAT model see values consistent with
the commanded state, and so the state-dependent limits pass under PFM
and context limits, not only under the AX 0..4095 window.

**No protection compare and no HK IRQ:** the FPGA's 9-channel threshold
compare (`hkProtConfig`, IRQ 4) is left out; all proposed values sit
below every default threshold (SPW 3826, the rest 0xFFF).

**ATT_C (8) is a static 20** — the attenuator is not modelled to move.
Latent limit: 20 is below the PFM two-motor minimum of 28 (ATT2M_C).
No NOMINAL path moves the attenuator (`RateCtrl_reset(FALSE)`), so
nothing reads it in that window today; a future TC-driven attenuator
test must return a motion-dependent value of 110 or more, kept below
0x100 (`AttCtrl.c:342` drops samples >= 0x100 from the summed current).

**Property `hk-adc` (bool, default true):** `false` restores the legacy
VAL = 0 behaviour (READY still forced), for A/B comparison and rollback.

No new trace events: the existing `stixctrl_io_write`/`io_read` traces
already show the synthesised register value at 0x8C.

**Golden changes** (regenerated with `capture-golden.sh` on the normal ELF, shared run dir `/tmp/stix-uprev-run`):
- **`uart.golden.log`:** the three `HKAcquire *E reference voltage n is with value 0 out of range (HKAcquire.c:460)` lines after `do: selftest` are gone. Apart from them, the normalized log is identical, and boot-smoke passes against the new golden.
- **`qemu-stdout.golden.log`:**
  - Every `0x8c` access now carries VAL. For example, the first self-test read is `0x80000098`, which is ch 24, VAL 0x800 = 2048.
  - The rest of the diff is the usual run-to-run timing noise: SCET reads at 0xC8/0xCC, `hkCmdSta` poll counts, SPW status polls and TX dump words. The HK TM payloads also carry the new values.
- **Boot report:** on a fresh flash (run p31boot) the first event is TM(5,1) `0022 00000008 00000000 ba`, the success report. TM(5,3) 0x8002 and TM(5,4) 0xD420 are no longer sent. The only UART `*E` lines left at boot are the four `LUTMgmt could not find LUT` lines, which come from the empty flash.
- **`tm.golden.log`** is not regenerated by `capture-golden.sh`. It was last captured in 2d2b782b51 (2026-11-09), before the watchdog model (5cda8ac82e) and before the D-I flash. Its boot TM(5,3) 0x8021 ("warm SW reset by WDT failed") and its late 0x14A3 come from that older model. It stays a historical artifact; P3-1 does not change it.
- **5.2 wrapper:** `/sparc/stix/hk-adc` is expected red (the 5.2 model has VAL 0). `ctrl` and `spw-tx` stay green there.

**Evidence:** qtest `/sparc/stix/hk-adc`: (a) ch 24 → 2048, ch 28 →
3413, ch 44 → 3958 (the self-test channels) with READY set and ADDR
echoed; (b) ch 0 → 700, ch 32 → 2450, ch 60 → 2000, ch 68 → 3125,
ch 71 → 1700; (c) SPWEN 0..3 → SPW_C 50/900/900/1830 and SPW0_V/SPW1_V
3240 only when their own bit is set, else 20; (d) idefxConfig masks
0x0/0x1/0x3/0x7/0xF/0x5/0xA → DET_C 20/95/170/245/320/170/170, and a
bit above the field (0x10) does not count; (e) LVEN → ATT_V 1670/10,
HV0116EN → 1000/15, HV1732EN → 1000/16; (f) unused channels 5/100/127
→ VAL 0, READY set; (g) `hk-adc=off` → ch 24 reads VAL 0 with READY set;
(h) POWERSTAT bits 9:5 read back as written (0x3E0, and bit 5 alone),
nothing synthesised (since §25 case h instead pins that written status
bits do not stick). Red on the pre-P3-1 binary: `(VAL == 2048)` failed
with 0 (and the `hk-adc` property is unknown). Green: `hk-adc`, `ctrl`
(the READY pin), and the full suite (34 tests). checkpatch on the diff:
0 errors, 0 warnings.

## 23. NOMINAL gate (baseline: none)

**Purpose:** `scripts/stix/nominal-smoke.sh` is the SAFE →
CONFIGURATION → NOMINAL gate (P3-2). It boots the model, drives the
mode changes with TC(236,1) CONF (mode 4), TC(236,6) S20 = 0 and
TC(236,1) NOM (mode 5), and expects the PUS reports of
recon-phase3-nominal §4: the connect-time group (TM(5,3) 0x8030 fresh
flash, the P3-1 boot success 0x0022, FDIR enable 0x14A3, the boot TC20
timer 0x142E), then CONF → `0021 02 04`, S20 = 0 → TM(1,1)/TM(1,7) (the
FSW sends two TM(1,7); one is expected, never "exactly one"), NOMINAL →
`0021 04 05` plus SID2 (0x5A4) with the mode nibble 5, then a
TC(17,1)/heartbeat health check at 165 s. `--negative` drops the S20=0
send and passes only when the TC20 fallback is seen — both 0x142E and
`0021 05 02` (NOMINAL → SAFE), about 30 s after NOMINAL entry; the
probe does not check their order.

**Probe extensions it relies on** (all self-tested, no socket needed):
`--hold` (gate mode keeps listening until `--seconds` even after the
last expectation matched — without it the run exits on the last match,
which is why plain probing near a mode change once looked like "TM
stopped"); `--forbid APID:T:S[:DATAHEX]` (the negation of `--expect`,
attached to the most recent `--send-hex` or to the connect group,
arming at that group's send — a match before the send is ignored, a
match after it fails the run with `FORBIDDEN TM: ...`, no `@window`);
expectations still match in any order within their window.

**The fresh-flash rule:** every run starts from an EMPTY run dir
(default `/tmp/stix-nominal-run`, override `STIX_NOMINAL_RUN_DIR`) so
the device creates a new sparse `flash.bin`. Any NOMINAL entry writes a
flash context (`ContextMgmt_deinit`, `OpModeMgmt.c:317`), so a reused
flash silently changes every later boot — the S20 value, parameter
limits and LV/HV defaults drift, and the golden would follow. The run
dir is canonicalised first (`realpath -m`, which also resolves
symlinks and relative paths), and anything at or above the two shared
run dirs `/tmp/stix-uprev-run` and `/tmp/stix-baseline-run` — or `/`
itself — is refused; the script clears only its own run dir's files,
by explicit path. Like tc-smoke.sh it does not take the 21000 flock
itself (the caller must hold it; no script does this for you), and it
uses the same port (20000+uid) and one-QEMU `ss` guard.

**Expected NOMINAL-entry side effects, not failures:** the 384 IDeF-X
register-write timeouts on the UART — `XrayDetFDIR.c:731` logs the
failure and does `res += FSW_ERROR; // LIE: do not return`, so the
NOMINAL path carries on (the model has no IDeF-X interface); one
TM(5,2) 0x542C p1 = 1 (no LUTs in flash) — expected but **not** checked
by the gate: it was seen in the p3probe4 experiment, and the recon
marks it inferred; LUTMgmt "could not find LUT" messages; and SID2
reporting LV/HV/SpW **off** in NOMINAL, because POWERSTAT (hkCmdSta
bits 9:5) stays unmodelled at 0 (D-P3a; since §25 it follows the
enables). The gate counts the 384
timeouts and prints the number; another count is a `WARN:` line, not a
failure. **Updated by D-2 (§32):** the IDeF-X interface is now modelled,
so the 384 writes complete and the expected count is **0** in both
modes; any other count is a `FAIL` (exit 1). The UART must contain "init: SAFE mode" and must not match
`storing new context[01] failed`.

Exit codes as tc-smoke.sh: 0 pass (or SKIP/dry-run/check-guard), 1 a
failed expectation, a forbidden TM, a closed connection or a failed
UART check, 2 environment problem. `--dry-run` prints the QEMU and
probe command lines and exits 0; `--check-guard` runs only the run-dir
guard.

**Gate record** (2026-11-10, worktree build at f2e8664506 plus this change, normal ELF, fresh flash per run):
- **Positive** (`STIX_NOMINAL_RUN_DIR=/tmp/stix-nominal-pos`): PASS.
  - Boot: 0x0022, 0x8030, 0x14A3 at +13 s, 0x142E at +29 s.
  - CONF at +50 s: `0021 02 04`.
  - S20=0 at +60 s: acked.
  - NOM at +70 s: `0021 04 05` 4 s later.
  - SID2 `0215` (mode NOMINAL) 60 s after entry.
  - TC(17,1) at +165 s: fully answered, heartbeat present.
  - No forbidden TM; 384 IDeF-X timeouts.
- **Negative** (`--negative`, `/tmp/stix-nominal-neg`): PASS.
  - NOMINAL entered at 23:20:54, 0x142E at 23:21:21 (27 s later), `0021 05 02` at 23:21:22.
  - A first negative run at 23:11 gave the same 27 s.
- **Unchanged:** tc-smoke PASS, boot-smoke PASS (P3-1 golden), probe self-test PASS.
- **Shared flash:** `/tmp/stix-uprev-run/flash.bin` mtime is still 13:05, i.e. untouched by the gate.

## 24. Known limits after Phase 3

What the model deliberately does **not** do, so a missing behaviour is
not mistaken for a bug (plan P3-3; decisions D-P3a..c):

- **No detector/IDeF-X model.** The ASIC serial interface is absent and
  the 384 register writes time out (§23). The trap for later work
  (recon-phase3-nominal §2.3): modelling only `ideFxCmdSta.CMDREADY`
  makes things **worse** — the ASIC verification run every 4096 s would
  read back zeros, emit TM(5,2) 0x542E and TM(5,3) 0x9438, and OR every
  ASIC into `CALISTE_IGNORE_FLAGS`. An IDeF-X model must include
  register readback, or not exist at all.
- **No QL, science or ROTB data.** The readout FSM status stays 0, no
  QL IRQ fires, and ROTB blocks come only from the detector hardware
  that is not modelled — so no QL TM is produced and BLOCKCOUNT stays
  0 (recon-phase3-nominal §2, §3).
- **No POWERSTAT** (resolved by §25, P4-1). hkCmdSta bits 9:5 stay plain storage at 0 (D-P3a):
  SID2 shows LV/HV/SpW off even in NOMINAL, and the FSW's SpW, ATT_V
  and HV voltage checks stay skipped. Synthesising it later requires
  the SpW voltages to track SPWEN atomically or a PFM/context run falls
  into offline mode (§22).
- **No HK hardware protection IRQ.** The FPGA's threshold compare
  (`hkProtConfig`, IRQ 4) is not modelled, so no HK protection FDIR can
  trigger.
- **`hkIrqSta` stores the FSW's clear writes raw** (cosmetic): a
  write-1-to-clear is not implemented, so the register reads back what
  the FSW wrote (the golden shows `0x3fe20`).
- **ATT_C (channel 8) is a static 20** — the attenuator is not modelled
  to move. Latent: below the PFM two-motor minimum of 28 (ATT2M_C); a
  future TC-driven attenuator test needs a motion-dependent value of
  110..255 (AttCtrl.c:342 sums only < 0x100). See §22.
- **`HK_getADCReadout` has no lock** between its hkAdc write and its
  read (HK.c:130-157): a preempted reader can get another channel's
  value. This was invisible at ADC = 0 and is an FSW property, not
  something to model around (plan, Phase 3 risks).
- **`tm.golden.log` is a historical artifact, not regenerated** by
  `capture-golden.sh` (§22): it was last captured before the watchdog
  model and the D-I flash, and Phase 3 does not change it.


## 25. POWERSTAT follows the commanded supplies (baseline: plain storage, 0)

**Baseline:** `hkCmdSta` (CTRL 0x88) was plain storage. POWERSTAT
(bits 9:5) read back whatever was written; the FSW never writes ones
there, so it read 0. The FSW therefore saw every supply as unpowered.
- It skipped the SPW0/1_V, ATT_V and HV checks (HKMonitor.c:357-370),
  and `HKMonitor_spwCheck` during SpW init (SpwControl.c:472-491).
- SID2 reported LV/HV/SpW off in every mode (§22, §24).

**Model (P4-1, D-P4c):**
- POWERSTAT is the PSU power-good (PSU.h:36-43), composed on read
  (`stixctrl_hkcmdsta`): bit 5 LV, 6 HV01, 7 HV17, 8 SPW0 and 9 SPW1
  follow LVEN, HV0116EN, HV1732EN and SPWEN[1:0] (bits 0..4).
- The write handler drops bits 9:5. The PSU driver writes the register
  read-modify-write (PSU.c:20-90), so a stored status bit would keep a
  supply "good" after its enable is cleared.
- The other fields (HV0116VAL, HV1732VAL) stay plain storage.
- There is no power-up delay; the FSW waits 10 ms before it looks
  (commonFSW.h:319).
- There is no protection cut-off. The real FPGA latches a supply off on a
  protection trip (TN REQ-043, DD §5.6); the model keeps a `blockade`
  mask, always 0, as the hook for that (D-P4d).
- `powerstat=off` (property, default on) restores the plain storage for
  A/B runs.

**FSW effect (reviewed against the source; all values pass):**
- SpW init now reads SPW0_V/SPW1_V for the powered lines and gets 3240
  (AX 0..4095, PFM 3011..3468).
- The ATT_V/HV checks run only while LV/HV are commanded on (1670 and
  1000; PFM 585..3370 and 0..2543).
- No FSW code waits for POWERSTAT to drop.
- Under the gate ELF's limits (all 0..0xFFF, `spw_c_limits` too) nothing
  can trip.

**Golden changes:**
- `uart.golden.log`: unchanged apart from the timestamps (boot-smoke
  passes on the normalized log).
- `qemu-stdout.golden.log`: the hkCmdSta reads show the status bits
  (0x18 becomes 0x318 with both SpW lines on), and SpW init adds
  SPW0_V/SPW1_V conversions (for example `0xca8000b0` = ch 48, VAL
  3240). The rest shifts by those lines.
- SID2 (live capture, `powerstat` on and off, otherwise identical boots):
  exactly one byte of the 109-byte packet differs, by two adjacent bits
  (0x18 vs 0x00). This matches status4 SPW0/SPW1 (bits 28/29,
  HKAcquire.c:329-336), the only supplies on in SAFE.

**Tests:**
- `/sparc/stix/powerstat`:
  - the reset value is 0;
  - each enable maps to its own bit, and all on gives 0x3FF;
  - written status bits do not stick;
  - the FSW's read-clear-LVEN-write pattern drops LV to off;
  - the HV value fields are kept;
  - `powerstat=off` keeps plain storage.
- `/sparc/stix/hk-adc` case h now pins that written status bits do not
  stick.
- Both were red on the pre-P4-1 binary. The full suite is 35 ok.

**Gates:** boot-smoke (new golden), soak 300 s, tc-smoke, the probe
self-test, and nominal-smoke in both modes (384 IDeF-X timeouts, as
before) all pass. The shared `/tmp/stix-uprev-run/flash.bin` is
untouched.

**Known gap:** reset does not clear `hkCmdSta` (pre-existing). After a
watchdog reset POWERSTAT therefore reports the old supplies on until the
BSW rewrites the register.

## 26. HK ADC injection over QMP; fixed QOM paths (baseline: none)

**Baseline:** there was no way to change an HK reading while the guest
runs. stixctrl and stixspw were unnamed `/machine/unattached/device[N]`
children, so tests had to find them by a type scan.

**Model (P4-2, D-P4a/b):**
- stixctrl has 128 class properties `adc-inject[0]`..`adc-inject[127]`
  (uint32), one per hkAdc channel.
  - `0xFFFFFFFF` means no override and is the default.
  - Any value from 0 to 0xFFF replaces that channel's reading at the next
    conversion. It beats both the nominal table and the state overrides
    (§22).
  - Other values are rejected with a QMP error, not masked, so a typo in
    a script fails loudly.
  - Set and clear are traced (`stixctrl_adc_inject_*`).
- A device reset (a system reset, including the watchdog's) clears every
  injection. A reboot is therefore deterministic, and a stale SPW*_V
  injection cannot break SpW init after a reset.
- Injection is runtime-only, over QMP. The reset at machine start clears a
  `-global ...adc-inject[N]=X`. With `hk-adc=off` the legacy path (VAL 0)
  ignores injections.
- Clearing needs the literal 4294967295: `-1` is rejected by the uint32
  visitor.
- leon3.c adds the devices as `/machine/stixctrl` and `/machine/stixspw`.
  The qtests `ctrl-irq-lines` and the SPW path helper use them now.

**Use** (QMP socket, for example `-qmp unix:<run dir>/qmp.sock,server=on,wait=off`):

    {"execute": "qom-set", "arguments": {"path": "/machine/stixctrl",
     "property": "adc-inject[24]", "value": 1800}}
    {"execute": "qom-set", "arguments": {"path": "/machine/stixctrl",
     "property": "adc-inject[24]", "value": 4294967295}}

On the gate ELF every HK limit is 0..0xFFF, so an injection alone never
trips a software check. Tighten the limit first with TC(236,10), and only
after TM(5,1) 0x14A3 (§27).

**Tests:** `/sparc/stix/adc-inject` covers:
- all 128 channels start at 0xFFFFFFFF;
- an injected value reads back while its neighbours are unchanged;
- an injection beats the SPW_C state override;
- 0 and 0xFFF are accepted;
- 0x1000 and 0xFFFFFFFE are rejected and the value is kept;
- clearing restores the nominal value;
- clearing SPW_C brings its state override back (1830);
- `-1` is rejected;
- `qtest_system_reset` clears all 128 injections;
- there is no `adc-inject[128]`.

`ctrl-irq-lines` asserts that stixctrl is no longer under
/machine/unattached. Both failed on the pre-P4-2 binary.

## 27. Fault gate: HK limit trips and time-code loss (baseline: none)

**Purpose:** `scripts/stix/fault-smoke.sh` is the SAFE-mode fault gate
(P4-3). It boots the model on a fresh flash with a QMP socket in the run
dir (`-qmp unix:<run dir>/qmp.sock,server=on,wait=off`), drives faults
through the §26 injection and the stixspw `timecodes` property, and
expects the FSW's own reports. It stays in SAFE: every trip signals a
move to SAFE, which is harmless there.

**Probe extension it relies on:** `--qmp PATH` and `--qmp-cmd JSON`. A
QMP command is a send group like `--send-hex`: it takes the preceding
`--send-at`, the expectations and forbids after it attach to it, and
the TM already buffered is drained before it runs (as for a TC). The
probe connects at the first due command (it starts before QEMU), does
the greeting and `qmp_capabilities`, and skips async events until the
`return`. An `error` reply, a closed socket or no reply within 5 s
fails the run with `QMP ERROR: ...` (rc 1). A bad JSON or a
`--qmp-cmd` without `--qmp` is an argument error (rc 2). The
self-test covers success, an error reply, events before the return, a
close, a timeout, and run_gate with a QMP group (pass, and an error
reply that fails at once).

**Sequence** (seconds from the TMTC connect):

| at | action | expected |
|---|---|---|
| connect | — | TM(5,1) `14A3` within 30 s; TM(1,2) and TM(1,8) forbidden for the whole run |
| 35 | TC(236,10) `00080000 22 00000003` (1V5_V count = 3) | TM(1,1), TM(1,7) `1DACC001`, 5 s |
| 37 | TC(236,10) `00080000 01 0000079F` (1V5_V low = 1951) | TM(1,1), TM(1,7) `1DACC002`, 5 s |
| 40 | `adc-inject[24]` = 1800 | TM(5,3) `9449 07 0708 079F`, TM(5,4) `D420 00080000`, 10 s |
| 55 | `adc-inject[24]` cleared | — |
| 58 | TC(236,10) `00100000 22 00000003` (PSU_T count = 3) | TM(1,1), TM(1,7) `1DACC003`, 5 s |
| 60 | TC(236,10) `00100000 02 00000ED8` (PSU_T upper = 3800) | TM(1,1), TM(1,7) `1DACC004`, 5 s |
| 65 | `adc-inject[40]` = 3900 | `9449 17 0F3C 0ED8`, `D420 00100000`, 10 s |
| 80 | `adc-inject[40]` cleared | — |
| 85 | `adc-inject[4]` (DET_C) = 600 (P4-4, §28) | TM(5,3) `9434 05 0258 0222`, 10 s |
| 95 | `adc-inject[4]` cleared | TM(5,3) `9434` forbidden from here on |
| 100 | stixspw `timecodes` = false | TM(5,2) `542B` within 90 s; TM(5,3) `9449` forbidden from here on |
| 195 | `timecodes` = true | TM(5,1) `1429` within 10 s |
| 215 | end (`--hold`) | — |

Since P4-4 the connect group also forbids TM(5,3) `9434` for every
line id except DET_C's (`943401`, `03`, `04`, `06`, `0c`, `0d`, `0e`),
so a nominal reading that trips at the 0x14A3 arming fails the run.
The rows from 100 on moved by 15 s, and `STIX_FAULT_SECONDS` (default
215) must be at least 215. The record below is from the 200 s schedule
before that change.

The TC(236,10) application data is `fdirMask u32, paramType u8,
value u32` (TCData.h:151-153). paramType is the FDIR.h enum position:
1V5 low limit = 1, PSU upper limit = 2, and `*_CONSECUTIVE_MEASUREMENTS`
= 34 (0x22) in both enums. The TC units use the CRC routine that
reproduces the other gates' TCs exactly. A TC(236,10) before 0x14A3
cancels the automatic FDIR enable (FDIR.c:1022), so 0x14A3 must come
within 30 s and the first TC goes at +35 s. If the TC came first,
0x14A3 would never arrive and the gate would fail loudly. 0x14A3 was
seen about 14 s after the connect, so a host running at half speed
still passes. 0x142E
(the boot-armed TC20 timer) is not forbidden.

**FSW bug found by this gate: the persistence count is 255 after boot,
not 3.** ParamMgmtDef.h gives every `HKMON_OUT_OF_BOUNDS_TRIGGERED_*`
the value 3 (range 1..255). But `ParamMgmt_init` (ParamMgmt.c:34-36)
calls `HKMonitor_updateParameter` three times per parameter: value,
then min, then max. The count branch (HKMonitor.c:960-964) ignores the
type and stores whatever it gets, so the max (255) wins. A line must
therefore stay out of limits for 255 consecutive 1 s checks before
0x9449.
- Seen live on the gate ELF, through QMP `human-monitor-command`
  `xp`:
  - every `hkLineLimits[].count` (0x40083344) is 0xFF;
  - after TC(236,10) the 1V5_V entry is `079F 0FFF 000000FF`;
  - `paramsOutOfBoundsCount[7]` went 4 → 11 under injection, with no
    TM.
- The first gate run (limit TC only) failed:
  `not matched: 0x5A7:5:3:9449070708079f@10 0x5A7:5:4:d42000080000@10`.
  A trace showed the FSW reading 0x708 on channel 24 once a second.
- The gate therefore sets the count to 3 by TC first. That TC goes
  through `ParamMgmt_setParamValue` (type OTHER), which stores the
  value. TCVerifDynamic.c:511/562 allows 1..255.
- The model is not involved. Whether flight contexts hide this (a
  context load calls the same setters) is open.

**Gate record** (2026-11-11, worktree build at 6734968e7b plus this
change, normal ELF, fresh flash per run, under the 21000 flock). These
runs used the pre-review schedule, 10 s earlier, with the TC acks on
the 20 s default window. The lead's rerun on the final schedule
follows below.
- **Positive** (`STIX_FAULT_RUN_DIR=/tmp/stix-p4-fault-pos2`): PASS,
  2 min 48 s. The CUC coarse of each TM is in parentheses; SCET ≈
  connect + 1 s.
  - Boot: 0x0022, `0021 00 02 01`, `8030 01 01`, 0x0032, then 0x14A3
    (16) about 14 s after the connect, and 0x142E (32).
  - The four TC(236,10): TM(1,1) and TM(1,7) in the same second each
    (26, 28, 48, 50).
  - 1V5_V: TM(5,3) `9449 07 0708 079F` and TM(5,4) `D420 00080000`
    (33). That is 2-3 s after the injection, i.e. the third check.
  - PSU_T: `9449 17 0F3C 0ED8` and `D420 00100000` (59), 3 s after
    the injection.
  - Time-code loss: TM(5,2) `542B` with no parameters (0x8000008B),
    about 61 s after `timecodes` = false. That is the 60 s timeout
    plus the check period. From then on the CUC carries bit 31.
  - Recovery: TM(5,1) `1429` (0x8000009C) within 1 s of
    `timecodes` = true. The next heartbeat has bit 31 clear (0x9D).
  - No 9449 after +75 s; no TM(1,2) or TM(1,8).
- **Only observed, not checked:**
  - the UART prints `Transitioning to safe mode (FDIR.c:629)` once per
    trip (00:00:31 and 00:00:55);
  - no mode-transition TM follows (already in SAFE);
  - TM(5,4) carries the line's FDIR function mask, not the line;
  - 0x0032 at boot.
- **Negative** (`--negative`, `/tmp/stix-p4-fault-neg`): PASS,
  39 s. `adc-inject[24]` = 2048 (inside 1951..4095). 0x14A3 and the
  first two TCs' acks matched, and the probe failed at +40 s with
  exactly `not matched: 0x5A7:5:3:9449070708079f@10
  0x5A7:5:4:d42000080000@10`. Negative mode passes only on that set,
  with no forbidden TM and no QMP error.
- **Shared flash:** the `/tmp/stix-uprev-run/flash.bin` mtime is
  still 2026-11-10 13:05, and the `/tmp/stix-baseline-run/flash.bin`
  mtime is still 2026-11-09 15:30.

**Lead rerun on the final schedule** (after review; the same build;
`/tmp/stix-p4-fault-lead`, under the flock): fault-smoke PASS;
`--negative` PASS, with exactly `not matched: 0x5A7:5:3:9449070708079f@10
0x5A7:5:4:d42000080000@10`. The four 5 s ack windows had expired by
then, so the acks are enforced. In the same session: probe self-test,
boot-smoke, tc-smoke, and nominal-smoke in both modes, all PASS with the
refactored probe. The `/tmp/stix-uprev-run/flash.bin` mtime is
unchanged (2026-11-10 13:05:54).

Exit codes as nominal-smoke.sh (0 pass or SKIP/dry-run/check-guard, 1
a failed expectation, a forbidden TM, a QMP error, a closed connection
or a failed UART check, 2 environment problem). The run-dir guard is
nominal-smoke's (realpath, refuse the shared run dirs, their parents
and /). It also refuses a run dir whose `qmp.sock` path would be 108
bytes or more. `STIX_FAULT_SECONDS` (default 215 since P4-4) must be at least
215. A failed QMP connect (`QMP ERROR: cannot connect`) is an
environment problem, rc 2.

The probe refactor changes one behaviour: a malformed `--expect` or
`--forbid` is now an argument error (rc 2, "bad arguments"). It used to
be a traceback with rc 1. tc-smoke and nominal-smoke never pass a
malformed spec.

## 28. HK hardware protection comparator and IRQ (baseline: plain storage)

**Baseline:** hkIrqSta (0x90), hkProtConfig[9] (0x94..0xB4) and
UnlockingReg (0xD0) were plain storage. The thresholds read 0, nothing
ever tripped, `irq_hk` (IRQMP line 4) was never raised, and the unlock
token was not enforced. The FSW's HW-protection FDIR (`HK_IrqCallback`,
FDIR.c:829-947) was dead code.

**Model (P4-4, D-P4d):**
- Nine comparators. Line i (`hkProtConfig[i]`, selector i + 1) watches
  HK ADC channel 0, 1, 2, 3, 4, 8, 12, 16, 20 (1V5_C, 2V5_C, 3V3_C,
  SPW_C, DET_C, ATT_C, ATT_V, HV_17_32_V, HV_01_16_V) and owns
  IRQEN/PROTSTAT bit `1 << (8 - i)` (1V5 = 0x100 ... HV01 = 0x001,
  ICD Tables 4-72/4-73).
- Reset (`grlib_stixctrl_reset`, explicit, since reset does not clear
  `reg`): thresholds from ICD Table 4-75 (2282, 3416, 3826, 3826, 546,
  751, 4095, 2323, 2323), ProtEn 0, hkIrqSta 0, no token.
- **Sampling.** The real loop samples every line continuously (< 1 ms)
  and needs two consecutive samples 850 µs apart strictly above the
  threshold. The model samples a line only at two events: a guest
  conversion of its channel (an hkAdc write) and a set or clear of its
  `adc-inject`. "Consecutive" means two such events in a row with the
  value `>` the threshold; a sample at or below it restarts the count.
  An injection is therefore sample 1 and the FSW's next conversion of
  that channel is sample 2. The count runs with ProtEn 0 too.
- PROTSTAT sets only with ProtEn = 1. A newly set bit latches the
  sample in HOTSAMPLE (one shared field, last trip wins) and pulses
  `irq_hk` (`qemu_irq_pulse`, like ROTB) if its IRQEN bit is set. A bit
  that is still set is not re-raised; once the FSW clears it, the next
  high sample sets it again. Setting IRQEN for an already-set PROTSTAT
  raises nothing.
- hkIrqSta writes: IRQEN (8:0) R/W, PROTSTAT (17:9) write-1-to-clear
  (`HK_clearProtectionStatus` writes `tmp | mask`, then `tmp`,
  HK.c:272-277), HOTSAMPLE (31:20) and 19:18 read-only.
- hkProtConfig writes keep ProtEn (31) and the threshold (11:0); 30:12
  read 0. Without the token only a ProtEn 0 → 1 takes effect; a
  threshold change or a ProtEn clear in the same write is dropped and
  traced (`stixctrl_hk_prot_locked`). This is "write protected by
  UnlockingReg Token (except for protection enabling)" (ICD 4.49).
- UnlockingReg: a write whose bits 7:0 are 0xA5 arms the token; any
  other value disarms it. The token is spent by the next write to ANY
  register of the CTRL bank ("unlocks protected registers for one write
  cycle", ICD 4.52), not only by a hkProtConfig write. 0xD0 reads 0
  (write-only). The FSW always writes the protected register right
  after the token (HK.c:213-214, :240-241) and sets ProtEn without it
  (HK.c:218-222), so every FSW path works. A reset clears the token.
- Token details:
  - It is spent by any write to the CTRL register MemoryRegion
    (0x000-0x0FF). Writes to the ROTB block window (and the AHB data
    windows) are other regions and do not spend it.
  - An ISR write to a CTRL register between the token and the
    protected store (reachable, e.g. with icount) drops the protected
    write. Accepted as HW-faithful ("one write cycle").
  - Only bits 7:0 are compared (the Token field), so 0xFFFFFFA5
    unlocks too.
- **Shared HOTSAMPLE:** if a second line trips before the ISR reads
  hkIrqSta, the ISR reports the second line's sample for both bits (as
  the one-field hardware would).
- **No vmstate:** the device has none, so the comparator state
  (counts, token) is not migrated or snapshotted, like the rest of it.
- Traces: `stixctrl_hk_prot_trip` (selector, channel, sample,
  threshold, irq), `stixctrl_hk_prot_unlock`, `stixctrl_hk_prot_locked`.
- **No supply cut** (deviation, D-P4d): the real FPGA latches the
  non-essential and/or SpW supplies off on a trip (TN REQ-043, DD
  §5.6). The POWERSTAT `blockade` (`stixctrl_hkcmdsta`, §25) stays 0;
  the comment there marks where a later cut option would set it.
- The comparator needs `hk-adc=on` (no ADC model, no samples).
- `hk-prot=off` (property, default on) keeps the legacy plain storage
  of 0x90..0xB4 and 0xD0 (reset leaves them alone, nothing trips).

**FSW effect (reviewed against the source):**
- `FDIR_init` (FDIR.c:308-326) clears ProtEn of the eight non-SPW lines
  (with the token), writes the SPW_C threshold (3826) and sets its
  ProtEn per context, IRQEN = SPW_C only.
- 15 s after the boot report `FDIR_timerEnableFdir` (FDIR.c:529-552)
  sets ProtEn on all nine lines and all nine IRQEN bits, then 0x14A3.
  The non-SPW thresholds keep their reset values in SAFE.
- Every nominal model reading (§22) is below its threshold, so nothing
  trips on a plain boot (the gate forbids the other line ids). ATT_V's
  4095 can never be exceeded by a 12-bit sample.
- On a trip the ISR masks the line's IRQEN, sends TM(5,3) 0x9434
  `[id u8, HOTSAMPLE u16, threshold u16]`, clears PROTSTAT and signals
  SAFE (harmless in SAFE).
- FSW bug (recorded, not modelled around): a 2V5_C trip is reported
  with id 1, E_HK_DPU_1V5_C (FDIR.c:869).

**Tests:** `/sparc/stix/hk-prot`:
- the reset values of all nine thresholds, hkIrqSta and 0xD0;
- hkIrqSta field access (IRQEN R/W, HOTSAMPLE/reserved RO);
- the unlock rules: locked threshold write ignored, wrong token, one
  write per token, the token spent by an unrelated CTRL write, ProtEn
  set without a token (threshold part dropped), ProtEn clear needs it,
  reserved bits read 0;
- DET_C: equal is not above; one high sample does not trip, the second
  does (PROTSTAT, HOTSAMPLE, IRQ); no new IRQ while the bit is set; the
  FSW clear pattern; re-set after a clear; a low sample restarts the
  count; IRQEN 0 gives PROTSTAT without IRQ, and a later IRQEN raises
  nothing; ProtEn 0 gives nothing, re-enabling trips at the next sample;
- every line's channel and bit; two lines set with the last HOTSAMPLE;
  an unprotected channel never trips;
- a reset restores everything (ATT_V 4095 cannot trip), the token does
  not survive a reset;
- writing 0 to PROTSTAT (the ISR's IRQ-mask write) keeps the bit;
- an armed token is disarmed by a wrong value;
- `hk-prot=off`: plain storage, no trip, no IRQ;
- `hk-adc=off`: reset values and the lock apply, an injection plus
  conversions never trips.
RED on the pre-P4-4 binary (thresholds read 0). The full suite is 37 ok.

**Gate:** fault-smoke (§27) gains the DET_C step: `adc-inject[4]` =
600 at +85 s, TM(5,3) `9434 05 0258 0222` within 10 s, cleared at
+95 s, after which every 0x9434 is forbidden.

**Gate record** (2026-11-11, worktree at 4700254fe4 plus this change,
normal ELF, fresh flash per run, under the 21000 flock):
- **Positive** (`/tmp/stix-p4-hkprot-pos`): PASS, 3 min 27 s. 0x14A3
  ~14 s after the connect; every §27 expectation as before. DET_C: the
  injection at +85 s (10:45:27), TM(5,3)
  `9434 05 0258 0222` (CUC coarse 0x56) at 10:45:28, i.e. within 1 s
  (the FSW's next ch 4 conversion). The UART shows `HK callback`
  (FDIR.c:831), `FDIR ID = 5 FDIR ID val = 600, FDIR ID limit = 546`
  and `Transitioning to safe mode (FDIR.c:941)`. Exactly one 0x9434 in
  the run. `542B` ~60 s after `timecodes` = false (+100 s), `1429`
  within 1 s of `timecodes` = true (+195 s).
- **Negative** (`/tmp/stix-p4-hkprot-neg`): PASS, with exactly
  `not matched: 0x5A7:5:3:9449070708079f@10 0x5A7:5:4:d42000080000@10`
  (the probe stops at +50 s, before the DET_C step).
- boot-smoke PASS (normalized UART matches the golden: no golden
  change). The shared flash mtimes are unchanged
  (`/tmp/stix-uprev-run` 2026-11-10 13:05:54, `/tmp/stix-baseline-run`
  2026-11-09 15:30:10).

## 29. SpW link-down injection (baseline: the link is always up)

**Baseline:** status.LS always read RUN (5) and nothing set DE or any
other link-error bit. The FSW's SpW link FDIR (`SPW_callbackLink`,
`FDIR_SpWLinkFailure`, TMBuffer's 10 s report) was dead code in the
model.

SPW.c below is the ASW tree's `ASW/BSW_ASW/src/BSW/SPW/SPW.c`.

**Model (P4-5, D-P4e):** stixspw has a runtime bool `link-down`
(`qom-set /machine/stixspw link-down true|false`, like `timecodes`).
- Runtime-only: a set before realize is refused, and a device reset
  (a system reset, including the watchdog's) clears it.
- **While it is true:**
  - status.LS reads READY (2). The guest cannot change it: status
    writes are W1C only, so SPW_open's clear write (LS field 0,
    SPW.c:549-558) leaves READY.
  - DE (status bit 3) is set on entry and again every 1 s
    (`QEMU_CLOCK_VIRTUAL`). It stays W1C: the ISR's clear
    (SPW.c:227-233) works, and the next period sets it again.
  - With each DE, IRQ 13 is raised when control.LI (bit 9) is set,
    gated by control.IE (bit 3) with the D-M latch (LI with IE = 0
    latches, and the IE 0 -> 1 write pulses it). SPW_open sets IE|LI
    (SPW.c:567-570). This is the primary detection path.
  - Time codes stop: the timer is cancelled and re-arming is blocked.
    The `timecodes` value itself is not changed.
  - RX units from the wire (TCP, UDP or `rx-inject`) wait in the
    existing 64-unit no-spill queue. The RD write and the IE edge do
    not release them. Overflow drops and counts as before
    (`rx-dropped`).
  - **TX (choice):** a dmaControl write with TE leaves TE set and sends
    nothing. The descriptor stays armed and PS is not set. The exchange
    runs when the link comes back, as a GRSPW waits for Run when the
    abort-on-link-error bit (dmaControl.LE) is clear, and the FSW never
    sets it. The FSW practically never reaches this path: SPW_send
    returns `E_SPW_RES_ERROR_LINK` before arming the descriptor when
    LS != RUN (SPW.c:692-696), so TMBuffer keeps the TM queued and
    retries every 1 s (TMBuffer.c:924-933). Only a qom-set that lands
    between SPW.c:693 and :725 reaches it, and the model handles that
    case: TE waits.
- **On false:**
  - LS reads RUN. DE stays until the guest clears it.
  - The link-error timer stops.
  - Time codes re-arm, but only if `timecodes` is true.
  - A waiting TE exchange runs, then the queued RX units are delivered.
- Setting the current value again is a no-op (no new entry IRQ).
- **Re-entrancy guard (fix after review).** Three paths run DMA from
  the main loop, outside an MMIO dispatch, where QEMU's per-device
  re-entrancy guard is not engaged: the link-up clear, the TCP/UDP
  receive handler and `rx-inject`.
  - The bug on ca01170003: a held RX unit's descriptor daddr was the
    device's own dmaControl. Delivering it during the clear wrote RD
    there, and the nested `stixspw_rx_retry` delivered and freed the
    queue head the outer retry still held. That is a guest-triggerable
    double free (glibc abort).
  - The fix: these paths now engage `mem_reentrancy_guard` around the
    DMA, as `qemu_bh_new_guarded` does. A guest descriptor address
    inside the device's MMIO is refused, the same as on the in-MMIO
    paths (memory.c "Blocked re-entrant IO", MEMTX_ACCESS_ERROR, the
    write is dropped).
  - `stixspw_rx_retry` also refuses to nest (the `rx_retrying` flag);
    the queue order is unchanged.
  - Test: `/sparc/stix/spw-link-down-reentry` holds two units, with
    desc0 at dmaControl. It is RED on ca01170003 (`free(): double free
    detected in tcache 2`, SIGABRT). The full suite is 39 ok.
- Traces: `stixspw_link_down` (also on a reset that brings the link
  up), `stixspw_link_error` and `stixspw_tx_link_wait`.

**Deviations:**
- A real GRSPW with autostart cycles through ErrorReset, ErrorWait,
  Ready, Started and Connecting while the far end is gone. Only Run is
  stable. The model pins READY, because that is the only value the
  FSW's FDIR counts (below). The other error bits (CE, ER, PE, EE) are
  never set.
- The 1 s DE/IRQ period is a model choice. The real reconnect attempts
  come at the link's timing (about 12.8 µs per ErrorWait), so the ISR
  would run far more often. The FSW cannot tell the difference: it
  needs only one error past its 10 s transient window.
- RX units that arrive during the outage are delivered after it. On
  real HW the ground could not send them at all.
- dmaControl.LE (abort on link error) is ignored: a TE always waits. The
  FSW never sets LE. A real GRSPW with LE = 1 would abort the TX
  instead of waiting.

**FSW behaviour (observed, not modelled around): FDIR.c:711 counts only
LS == READY.** `FDIR_SpWLinkFailure` ignores every status except
`E_SPW_RES_ERROR_LINK`. That constant belongs to the `SPW_result_e`
enum, but it is compared with an `SPW_linkStatus_e` value. Its value
equals READY (2), so an outage seen as ErrorReset (0), ErrorWait (1),
Started (3) or Connecting (4) is not counted. The gate ELF has the same
comparison (the phase-4 plan's validation). The FDIR TN means any link
error to count (REQ-029: no communication; REQ-038: no duplicate
reports). The code does not do that. The model follows the code, so
the injection reads READY. Whether this is a bug or a late spec change
is open (the released docs may differ from the product).

**Seen live (the FSW reaction):**
- The ISR calls `SPW_callbackLink(2)` once per IRQ, which calls
  `FDIR_SpWLinkFailure(line 0, 2, external = TRUE)`.
- The first call starts the transient window. The first call after
  10 s sends TM(5,2) 0x5435 with param 0x48
  (`external << 6 | READY << 2 | line 0`, FDIR.c:772).
- About 1 s later a second 0x5435 comes. This is TMBuffer's report
  (TMBuffer.c:940-946), 10 s after the last successful TX. Its param
  is also **0x48**, not the 0x08 the plan expected: the ISR's next
  call ORs `possiblyExternal` back to TRUE before TMBuffer reports
  (FDIR.c:659, reset at :775). The gate checks neither the count nor
  the order of the 0x5435s.
- All TM is queued during the outage. Heartbeats are not even queued
  (HKReport.c:145-147). Both 0x5435 and then TM(5,1) 0x142D 00 (line
  0, FDIR.c:690-697) come out within a second of the clear. 0x142D is
  sent at the first successful TX, which calls `FDIR_SpWLinkFailure`
  with RUN (SpwControl.c:401).
- The TMBuffer flash backups (TMBuffer.c:947) start after 10 s. This
  is why fault-smoke must use its own fresh flash (§27).

**Tests:** `/sparc/stix/spw-link-down` covers:
- the default: false, LS = RUN, no DE;
- down: LS = READY, DE and IRQ 13 at once, and a repeated set raises
  nothing new;
- the W1C clear of DE, then DE and the IRQ back after 1 s (not after
  0.5 s);
- the SPW_open status write keeps READY;
- no time code over two rollovers, with `timecodes` still true;
- an injected RX unit is held, even through an RD write and the IE
  edge;
- the status write 0xFFFFFFFF keeps READY, and DE is still clear
  0.5 s after the W1C;
- a TE waits: TE stays set, EN stays armed, no PS, and no datagram on
  the UDP TM port. The descriptor has no IE, so the IRQ at the clear
  comes from the RX delivery alone;
- LI = 0 gives DE and no IRQ;
- LI with IE = 0 latches, and the IE edge pulses;
- the clear: LS = RUN, the unit is delivered (PR, IRQ), the TX
  completes (PS, EN cleared, TE clear), its 8 bytes arrive as one UDP
  datagram (SpW header stripped), and no DE follows. The order of TX
  and RX at the clear (TX first) is not asserted;
- time codes resume;
- `timecodes` = false survives an outage, and `timecodes` = true set
  while down waits for the clear;
- a clear with control.IE = 0 keeps the unit held (buffer untouched,
  PR = 0, no IRQ), and the IE edge delivers it with the IRQ;
- a system reset clears `link-down` (LS = RUN, no DE later).

RED on the pre-P4-5 binary: `link-down` is unknown
(`stix.c:4400: 'ok' should be TRUE`). The full suite is 38 ok.

**Gate:** fault-smoke (§27) gains two rows. `STIX_FAULT_SECONDS`
(default 250) must be at least 250 (negative mode excepted, as
before).

| at | action | expected |
|---|---|---|
| 210 | stixspw `link-down` = true | TM(5,2) `5435 48` within 35 s (it comes after the clear) |
| 230 | `link-down` = false | TM(5,1) `142D 00` within 10 s |
| 250 | end (`--hold`) | — |

The forbids already in force stay: 0x9434 from +95, 0x9449 from +100,
and TM(1,2)/(1,8) for the whole run. Nothing during the outage trips
them. `--negative` is unchanged: it fails at +40 s and stops long
before these rows.

**Gate record** (2026-11-11, worktree at 27091e745f plus this change,
normal ELF, fresh flash per run, under the 21000 flock):
- **Positive** (`/tmp/stix-p45-pos`): PASS, 3 min 59 s. Every earlier
  expectation matched as before: 0x9434 at +85, `542B`, and `1429` at
  +195.
  - The UART shows 20 `SPW_callbackLink status = 2` (about 1/s, from
    00:03:16.6 to 00:03:34.3), and `FDIR_SpWReportComFailure ... Link
    Status = 2, IsPossiblyExternal = 1` at 00:03:26.9 and at
    00:03:27.9.
  - All three TMs arrived in one burst after the clear (11:27:48):
    - `5435 48`, CUC coarse 0xDD, about 10 s after the down;
    - `5435 48`, 0xDF;
    - `142D 00`, 0xE6, at the clear.
  - No other TM in the outage, and nothing forbidden.
- **Negative** (`/tmp/stix-p45-neg`): PASS, with exactly
  `not matched: 0x5A7:5:3:9449070708079f@10 0x5A7:5:4:d42000080000@10`.
- boot-smoke PASS (the normalized UART matches the golden, so the
  golden is unchanged).
- The shared flash mtimes are unchanged: `/tmp/stix-uprev-run`
  2026-11-10 13:05:54 and `/tmp/stix-baseline-run` 2026-11-09 15:30:10.

## 30. Known limits after Phase 4

What the Phase 4 fault injection deliberately does **not** do, and the
FSW behaviour it exposes, so a surprise is not mistaken for a model bug
(plan P4-6). The GS-side recipe is in `ground-station.md`, "Fault
injection".

- **No HW power cut** (D-P4d, §28). The real FPGA latches the
  non-essential and/or SpW supplies off on a protection trip (TN
  REQ-043, DD §5.6). The model only raises the IRQ, so the event stays
  observable. The POWERSTAT `blockade` mask (§25) stays 0 as the hook
  for a later cut option.
- **The persistence count is 255 after boot** (FSW finding, §27).
  `ParamMgmt_init` lets the max (255) win over the value 3
  (ParamMgmt.c:34-36, HKMonitor.c:960-964). A SW limit trip therefore
  needs TC(236,10) `<mask> 22 00000003` before the limit TC. The model
  is not involved.
- **A 2V5_C trip is reported with id 1** (E_HK_DPU_1V5_C, FDIR.c:869;
  FSW bug, §28). Not modelled around.
- **Only READY counts as a link error** (FDIR.c:711, §29). The FSW
  compares an `SPW_linkStatus_e` with an `SPW_result_e` constant whose
  value equals READY. The FDIR TN (REQ-029/038) means any link error
  to count. The model follows the code. Bug or late spec change: open.
- **ATT_V cannot trip.** Its reset threshold is 4095 (ICD Table 4-75),
  and the comparator is a strict `>` on a 12-bit sample (§28). The FSW
  writes the non-SPW thresholds only by TC.
- **The link-down model pins READY, with DE and the error IRQ every
  1 s** (§29). A real GRSPW cycles through ErrorReset, ErrorWait, Ready,
  Started and Connecting at link timing. The CE, ER, PE and EE bits are
  never set. RX units that arrive during the outage are delivered after
  it.
- **dmaControl.LE is ignored** (§29). A TX armed during the outage
  waits for the link instead of aborting. The FSW never sets LE.
- **No vmstate** (§28). stixctrl and stixspw have none, so the
  injections, the comparator counts, the unlock token and `link-down`
  are not migrated or snapshotted.
- **`timecodes` survives a reset** (§20), unlike `adc-inject[N]` and
  `link-down`, which a reset clears (§26, §29).
- **The HW protection thresholds are document values** (D-P4d, §28).
  The FSW never writes the eight non-SPW thresholds in SAFE, so the code
  cannot confirm the ICD reset values.

## 31. Detector FSM, QL bank switch and IRQ 3 (baseline: plain storage)

**Baseline:** CTRL stored 0x00-0x4C (ideFxCmdSta .. fsmCalRes) as plain
registers. The fsmCmd strobes stayed stored, so the FSW's boot writes
`fsmCmd |= bit` read back 0x2, 0x1002 and 0x1802. fsmSta never moved.
No timer ran, `irq_detectors` (IRQMP line 3) was never raised, and the
FSW's QL chain (`detISR`, the QL task, RCR, light curves, background,
variance, spectra, flare detection) was dead code. A device reset left
every one of these registers as it was.

Detector plan D-1 (`docs/stix-port/2026-11-11-detector-plan.md`,
decisions D-Da, D-Dc, D-Dd, D-Dh, D-Dj). FSW paths below are in the ASW
tree; BSW `Detectors.c` is `BSW_ASW/src/BSW/Detectors/Detectors.c`.

**Code layout (D-Da):** a new compilation unit `hw/sparc/stix_det.c` with
the internal header `stix_det.h`. It is not a QOM device.
- `StixDetState` is embedded in `STIXCTRL` as `det`.
- `grlib_stixctrl.c` delegates APB 0x00-0x4C to `stix_det_read` and
  `stix_det_write`, after the hk-prot block, so a detector write still
  spends an unlock token (§28). Realize, unrealize and reset call into it.
- The registers stay in `s->reg`, the one source of truth. DET_C
  (`stixctrl_hk_value`, §22) still reads `s->reg.idefxConfig`.
  `StixDetState` holds only the derived and internal state.
- **Deviation from D-Da's wording:** the functions take the
  `StixDetState` pointer, not the `STIXCTRL` pointer. `struct STIXCTRL`
  is private to `grlib_stixctrl.c`. So `stix_det_realize` binds the
  state to the controller's register file, its SRAM, LVAR and LTRG
  regions and `irq_detectors`. This keeps the struct private and leaves
  `include/hw/sparc/grlib_stix.h` unchanged.
- Property `det-model` (bool, default on). With it off, 0x00-0x4C stay
  plain storage and are never reset, which is the pre-D-1 model (A/B
  comparison and rollback, like `hk-adc`).

**Model (D-Dc):**
- **fsmCmd (0x14):**
  - Bits 31:16 (CalTq) and 12:9 (FifoIrqEn, QlIrqEn, CalIrqEn,
    ActiveTlut) are stored and read back.
  - Bits 8:0 are strobes. They act on the write and read 0. Reserved
    bits 15:13 and 4:3 read 0.
  - The FSW sets every strobe with `fsmCmd |= bit` (`Detectors.c:136,
    186, 1093-1124, 1198-1201`). A stored ClearEnable would therefore
    fire again on every later read-modify-write, such as the ISR's
    QlClear.
- **The strobes:**
  - SetEnable 0 → 1 sets Enable and starts the 100 ms tick. A **fresh
    enable**, one that finds the period at tick 0 (the first after a
    device reset, or after a Reset while disabled), also zeroes the
    active bank (see the lifecycle).
  - ClearEnable clears Enable and stops the tick. It wins when both are
    in one write, and it freezes everything else: QlReady, ActiveQl,
    SUMCNT and the banks stay as they are [I].
  - Reset clears QlReady and SUMCNT and restarts the QL period: the tick
    is re-phased to Reset + 100 ms (only while enabled; a Reset while
    disabled only restarts the period count) and QlIntTime is
    re-latched. ActiveQl
    and the banks are kept [I]. The FSW always issues SetEnable and then
    Reset (`OpModeMgmt.c:669-672`), and it clears its own `detQLReady`
    at Reset (`Detectors.c:1125`), so the choice is not FSW-visible.
  - QlClear clears QlReady.
  - SetCalEnable sets CalEnable. Calibration never completes (a known
    limit), and only a device reset clears CalEnable.
  - CalClear and OverflowClear are accepted, but the status they clear
    never sets: there is no calibration and no FIFO model, so CalReady
    and QueueOverflow read 0.
- **fsmSta (0x18)** is read-only and composed live: Enable (bit 0),
  ActiveQl (3:2), QlReady (5) and CalEnable (7). QueueOverflow (1),
  ActiveNominal (4, D-5) and CalReady (6) read 0.
- **Other read-only registers:**
  - fsmEvents (0x30) reads SUMCNT. It is always 0 until D-4 adds a count
    source.
  - fsmResult1/2, fsmCalCount and fsmCalRes read 0 (the FSW never reads
    fsmResult1/2).
  - Writes to all of these, and to fsmSta, are dropped. They trace
    `stixdet_ro_write_dropped` instead of `stixctrl_io_write`.
- **testPulseCmdSta (0x10):** Busy (bit 16) reads 1, which is idle
  (active low, `Detectors.c:966-971`). QuarterSeu (31:28) reads 0, so the
  ISR's SEU branch (`Detectors.c:217`) never runs. The other bits are
  stored. The TSIM I/O model forces Busy = 1 in the same way.
- **ideFxCmdSta (0x00):** stays plain storage in D-1 (see Deviations).

**Model (D-Dd):**
- **The tick.** A `QEMU_CLOCK_VIRTUAL` timer of 100 ms runs only while
  Enable = 1. Its deadlines are absolute (each one is the previous plus
  100 ms), so it does not drift.
- **The QL period.** A period is QlIntTime + 1 ticks: 4 s with the FSW
  default of 39 (ICD (N+1)×0.1 s; Doc vs code 11 of the plan).
- **The latch.** QlIntTime is latched at the start of each period: at
  the swap, at Reset, and at a SetEnable 0 → 1 that finds the period at
  tick 0. A mid-period TC change therefore takes effect at the next
  period [D, RP-0067].
- **LVAR.** Tick k (0-based) of the period writes its count to LVAR slot
  k of the active bank while k ≤ 39, clamped to 16 bits.
  - The count is 0 until D-4. It is a per-tick accumulator, written to
    the slot at the end of the tick, which equals "zeroed on activation,
    then added to".
  - Later ticks of a longer period are dropped (QlIntTime is 16 bits).
    The hardware behaviour is undocumented, so this is a model choice.
  - It keeps every write inside each bank's 0xA0 bytes. The WDT
    `UNCHANGED_STORAGE` (0xE02000A8, `WDT.c:74`) and MemBram
    (0xE02003C0, §13) share the LVAR region.
- **The bank lifecycle**, defined once:
  - While a bank is active, only its LVAR slots are written, one per
    tick (above).
  - **At the swap:**
    1. The finished bank b's **LACC (0x6000) and LTRG (0x40)** are
       written in full from the shadow. In tier 0 the shadow is all
       zeros. Its LVAR is **not** rewritten, because it holds the
       per-tick slots.
    2. ActiveQl becomes (b + 1) & 3, and that bank is **zeroed on
       activation**: LACC 0x6000, LTRG 0x40 and LVAR 0xA0 only.
    3. QlReady is set, and `qemu_irq_pulse(irq_detectors)` fires if
       QlIrqEn (fsmCmd bit 11) is set.
    4. Bank b is not touched again until it becomes active again, three
       swaps later. The FSW reads it after the IRQ as bank
       (ActiveQl − 1) mod 4 (`Detectors.c:144-166`).
  - **On a fresh enable** the active bank is zeroed the same way, so no
    RAM from before a watchdog reset leaks into a QL period. D-4 and D-6
    rely on this. A non-fresh enable (mid-period, after ClearEnable)
    keeps the bank: ClearEnable freezes.
  - Every zeroing is followed by `memory_region_set_dirty`, and stays
    within each bank's LACC, LTRG and LVAR 0xA0 bytes.
- **"Zero on activation"** stands in for RotbCtrl's read-and-clear
  (plan, Doc vs code 6). An immediate clear after the swap would hand
  the FSW zeros.
- **Why Enable gates the tick:** the FSW consumes QL only in NOMINAL. It
  clears Enable in SAFE and CONF (`OpModeMgmt.c:128, 258`), so the SAFE
  gates see no tick and no IRQ 3. The docs do not say whether the
  FPGA's 100 ms counter runs with Enable = 0; nothing in the FSW
  depends on it.

**Reset (D-Dh), with `det-model=on`:**
- Explicit values for 0x00-0x4C: testPulseCmdSta = Busy, every other
  register 0. Before D-1 a reset left them all in place. Now the fsmCmd
  config bits, idefxConfig (quarter power, enables, SeuIrqEn), ReadMode,
  WriteMask, fsmQlConfig, the masks and the integration parameters do
  not survive a watchdog reboot.
- The tick stops, and Enable, QlReady, CalEnable, ActiveQl, SUMCNT, the
  period counter and the latched QlIntTime clear.
- The RAM banks are kept (they survive a reset, §13).
- No vmstate, as for the rest of stixctrl (§30).

**Re-entrancy (D-Dj, the §29 lesson):**
- The tick writes the banks only through `memory_region_get_ram_ptr`,
  each write followed by `memory_region_set_dirty`. It never uses
  `address_space_*`.
- Its only other output is `qemu_irq_pulse`. No MMIO dispatch happens
  from timer context, so the device's re-entrancy guard is not needed
  there.
- The MMIO path (the strobes) never writes guest RAM.
- There are no QMP setters in D-1.

**Deviations:**
- **ideFxCmdSta resets to 0, not D-Dh's 0x80000000.** CmdReady without
  the IDeF-X register store is the §24 trap: read-back returns zeros,
  and the ASIC verification then ignores every ASIC for good. The plan
  itself requires CmdReady and the store to land in one commit (D-2).
  So D-1 keeps 0 (the power-on value the trace golden shows), and D-2
  changes the reset value together with the store. `det-reg-reset`
  asserts 0 for now.
- CalReady and QueueOverflow never set, and calibration never completes
  (known limits, plan "TC-driven procedures stay unmodelled").
- ActiveNominal reads 0 until D-5 (no nominal integration yet).

**FSW-visible notes (review, 2026-11-11):**
- **The ISR can spend the unlock token.** The IRQ 3 ISR's fsmCmd write
  (QlClear) is a CTRL write. It spends the hk-prot unlock token if it
  lands between HK.c's token write and its protected hkProtConfig store
  (HK.c:213-214, :240-241). The protected write is then dropped. This
  is hardware-faithful ("one write cycle", §28), nearly unreachable
  under TCG, and accepted. `det-token-spend` pins the rule.
- **THS/TPG can stall QL for about 1.8 h.** `XRayADCsCtrl_THS_setup`
  plus Reset can latch a QlIntTime of 0xFFFF. That value outlives
  THS_cleanup, because no Reset or 0 → 1 edge follows it, so the
  current period runs 65536 ticks (about 1.8 h) before the restored
  QlIntTime is latched. This follows RP-0067's latch rule and is
  FSW-visible. Later gates that run THS or TPG must expect it.

**Doc vs code** (the code wins; the plan's numbering):
- 2: four QL banks (FSW and ICD), not TN-0041's "double buffered".
- 6: a bank is cleared when it becomes active again, not by RotbCtrl
  right after the swap.
- 11: the period is (N+1)×100 ms.
- Doc vs code 1 (CmdReady wording) applies from D-2.

**Traces:**
- `stixdet_fsm_strobe(strobes, enable, active_ql_bank, ql_ready)`, on
  every fsmCmd write with a strobe;
- `stixdet_ql_swap(done_ql_bank, active_ql_bank, ql_int_time, irq)`;
- `stixdet_tick_arm(armed, deadline_ns)`, only at start, re-phase and
  stop, not per tick;
- `stixdet_ro_write_dropped(addr, value)`, for a write to a read-only
  detector register.

**Tests** (RED first on 784ee6c9f4, then green):
- `det-fsm-strobes`:
  - strobes read 0, reserved bits read 0, config round-trips;
  - a `|=` RMW does not re-fire ClearEnable;
  - ClearEnable wins over SetEnable;
  - Reset clears QlReady and SUMCNT, keeps Enable and ActiveQl, and
    restarts the period;
  - ClearEnable freezes;
  - QlClear clears QlReady;
  - the read-only registers ignore writes;
  - SetCalEnable sets CalEnable.
  - RED: `fsmCmd` read back `0x1234fe19` (the strobe and reserved bits
    were stored).
- `det-ql-swap`:
  - QlIntTime 39: no swap at 3.9 s; ActiveQl 1 and QlReady at 4 s;
  - four swaps wrap to 0;
  - a mid-period change to 9 applies at the next period, after which
    the period is 1 s;
  - Enable = 0 means no swap.
  - RED: QlReady stayed 0.
- `det-ql-irq`:
  - IRQMP pending bit 3 sets at a swap only with QlIrqEn;
  - no pulse between swaps;
  - QlClear clears QlReady;
  - clearing QlIrqEn stops the pulses.
  - RED: QlReady stayed 0.
- `det-ql-banks`:
  - a fresh-enable pass with QlIntTime 9: a pattern in the active bank 0
    before the first enable is zeroed by the enable; after the first swap,
    LVAR slots 10..39 of the finished bank are still zero (the period
    writes only slots 0..9). The pass runs on a fresh instance and again
    after a `system_reset`;
  - a non-fresh enable (mid-period, after ClearEnable) keeps an LVAR
    marker;
  - garbage in all four banks and sentinels in every gap: LVAR
    0xA0-0xFF of each stride, LACC 0x6000-0x7FFF of each stride, 16 B
    just below LACC0 and 16 B past the LACC3 stride. LTRG3 ends at the
    end of the 0x100 ltrg region, so there is no LTRG word past it;
  - after a swap the finished bank and the new active bank are zero,
    and the other two are untouched;
  - a marker written into bank (ActiveQl − 1) & 3 survives two swaps
    and is zeroed on the third;
  - run with QlIntTime 39 and 99; afterwards every sentinel, including
    0xE02000A8 and 0xE02003C0, is unchanged.
  - RED: ActiveQl stayed 0. With only the enable-time zeroing removed:
    `0x5717e000 == 0x00000000` at 0xE0180000.
- `det-testpulse`:
  - Busy = 1 and QuarterSeu = 0, also after a write of 0xF0000000 or
    0xFFFFFFFF (which reads 0x0FFFFFFF);
  - the other bits are stored.
  - RED: read 0.
- `det-reg-reset`:
  - every register 0x00-0x4C is written with 0xFFFFFFFF (DET_C reads
    320), the FSM is started with QlIrqEn, then `system_reset`;
  - afterwards the D-Dh values read back, DET_C is 20, and there is no
    swap and no IRQ 3 within 5 s.
  - RED: reg 0x00 read 0xffffffff after the reset.
- `det-model-off`:
  - plain storage (fsmCmd 0xFFFFFFFF, fsmSta writable,
    testPulseCmdSta 0);
  - no tick and no IRQ;
  - the registers survive a reset.
  - RED: the property is unknown.
- `det-token-spend`: an unlock token, then an fsmCmd write, then an
  hkProtConfig[4] threshold write; the threshold stays at 546.
- `ahb-windows`, `bram-persist` and `hk-adc` are unchanged and green.
- The full suite is 47 ok.

**Goldens:**
- `uart.golden.log`: unchanged. boot-smoke PASS, and the normalized UART
  of the capture run is identical to the golden.
- `qemu-stdout.golden.log`: regenerated with `capture-golden.sh` on the
  normal ELF in the shared run dir (§22), from a scratch out-dir, so
  that only this file was copied in. The D-1 changes:
  - The boot fsmCmd writes now trace as stored 0x0, 0x0, 0x1000 and
    0x1800, where they were 0x2, 0x2, 0x1002 and 0x1802.
  - One `stixdet_fsm_strobe strobes=0x2 enable=0` line is new (the SAFE
    ClearEnable).
  - No tick, swap or IRQ appears, because SAFE never sets Enable.
  - The 16000 `addr = 0x0` polls are unchanged (D-2 removes them).
- The committed trace golden was last regenerated at P4-1. The new
  capture therefore also folds in the P4-4 hk-prot lines (the
  `hkProtConfig` reset thresholds and the `stixctrl_hk_prot_unlock`
  lines), plus the usual timing noise (SCET sub-seconds, time-code arm
  offsets, SpW dump words).
- The capture script needs an absolute QEMU path, because it changes
  into the run dir. A relative path fails silently: it leaves an 88-byte
  trace log and copies a stale UART file.

**Gate record** (2026-11-11, worktree `stix-uprev-tc` at 784ee6c9f4
plus this change, normal ELF, under the 21000 flock):
- boot-smoke (`/tmp/stix-uprev-run`): PASS (normalized UART matches the
  golden).
- **nominal-smoke positive** (`/tmp/stix-d1-nominal`, fresh flash): PASS,
  3 min 4 s.
  - 384 `XrayDetFDIR.c:731` IDeF-X timeouts, as expected.
  - [L] confirmed: no TM(21,6) 0x5DC in the run. QL TM stays queued
    while NOMINAL lasts.
  - The set of UART message types (numbers masked, the 731 lines
    excluded) is the same as in the earlier Phase 4 nominal runs.
- **nominal-smoke `--negative`** (same dir, wiped by the script): PASS.
  - NOMINAL lasted about 28 s: `0021 04 05` at 14:33:46, then `142E`
    and `0021 05 02` at 14:34:14.
  - [L] confirmed: four TM(21,6) 0x5DC packets come at once after
    `0021 05 02`, each unsegmented (sequence flags 3). They are SSID 33,
    30, 31 and 34, in that order.
  - Their start times are the NOMINAL entry (coarse 0x4A).
  - Sample counts: SSID 30 has N = 7 per band (int time 39, detector
    mask 0xFFFFFCFF, pixel mask 0x0FFF, 5 bands), SSID 33 has 7 values,
    SSID 31 has 3 samples (int time 79, i.e. 8 s), SSID 34 has 7
    triples.
  - The visible content is zero. The probe log keeps only a 48-byte
    prefix, so later bytes were not checked.
  - **No SSID 32**, as the plan predicted for a NOMINAL shorter than
    32 s.
  - This is the first live proof of the chain: IRQ 3, the ISR, the QL
    task and the flush on the NOMINAL → SAFE path (`OpModeMgmt.c:171`,
    the same deinit as CONF). Not forbidden; recorded.
- **fault-smoke positive** (`/tmp/stix-d1-fault`, fresh flash):
  PASS. Every
  expectation matched as before:
  - `14a3`;
  - the TC(17,1) and limit pairs with 0x9449 and 0x9434 TM(5,4);
  - [L] DET_C: `9434 05 0258 0222` (the threshold is still 546) at
    +85 s;
  - `542B`, `1429`, `5435 48` and `142D 00`.
  - No 0x5DC TM. The register reset changes nothing visible: the boot
    writes quarter power 0 anyway, and DET_C stays 20 in SAFE.
- **After the review fixes** (fresh-enable zeroing, the dropped-write
  trace, `det-token-spend`):
  - the full suite is 47 ok and checkpatch is clean;
  - boot-smoke PASS (the normalized UART matches the golden);
  - the trace golden is not regenerated: SAFE never enables, and the
    boot writes no read-only detector register.
- The shared flash mtimes are unchanged: `/tmp/stix-uprev-run`
  2026-11-10 13:05:54 and `/tmp/stix-baseline-run` 2026-11-09 15:30:10.

## 32. IDeF-X interface, ASIC register store and index tables (baseline: plain storage, CmdReady 0)

**Baseline:** CTRL stored ideFxCmdSta, idefxModeData and idefxWriteMask
as plain registers, so CmdReady (ideFxCmdSta bit 31) read 0 and every
FSW transmission timed out after 1000 polls (`Detectors.c:627-639`). At
boot, `Detectors_resetReg` × 16 (`OpModeMgmt.c:1061-1064`) timed out
silently: the trace golden had 16000 `addr = 0x0, regval = 0x0` polls.
At NOMINAL entry the 384 register writes
(`XrayDetFDIR_resetASICregistersToDefault`, `OpModeMgmt.c:576`) each
logged `XrayDetFDIR.c:731` (§23). The periodic ASIC verification only
passed because a read timeout is skipped, not compared
(`XrayDetFDIR.c:611-615`).

Detector plan D-2 (`docs/stix-port/2026-11-11-detector-plan.md`,
decision D-De, the D-Dh reset value and "Index spaces"). **CmdReady and
the register store land in this one commit**, as §24 and the plan
require: CmdReady alone would make every read-back return zeros, and
the verification would then mark every ASIC ignored for good.

**Index spaces (one table set, `stix_det.c`):** the FSW tables,
transcribed number for number, each with its file:line:
- `subcol` of each `fpga_det` (`utils_subcolIdFromFpgaId`,
  `utils.c:113-119`) and the inverse (`utils_fpgaIdFromSubcolId`,
  `utils.c:121-127`);
- `fe_sc` (1..32) of each `fpga_det` (`Detectors_frontendFromFpga`,
  `Detectors.c:528-534`) and `fpga_det` of `fe_sc` − 1
  (`Detectors_fpgaFromFrontendMinusOne`, `Detectors.c:536-540`, equal to
  `utils_oldSubcolIdToFpgaId`, `utils.c:129-133`);
- the `dete_data` columns `fpga_det`, `grp`, `asic` and `quarter`,
  indexed by `subcol` as in the FSW (`utils.c:74-107`);
- `asic_ch` of each `pix` 0..12 (`utils_pixelId2channelId`,
  `utils.c:50-53`) and the inverse (`utils_channelId2pixelId`,
  `utils.c:56-62`, 255 = none).

The code uses the identities `fpga_det = 2·grp + asic` and
`quarter = grp / 4` through two helpers. **The realize-time self-check**
(`stix_det_index_selfcheck`) proves the set is **consistent**, not that
it is faithful to the FSW (a consistent but wrong transcription would
pass):
- every entry is in range (< 32, `fe_sc` 1..32, `pix` < 13 or 255)
  before any is used as an index; a bad entry is reported through errp
  with its table, index and value;
- every forward/inverse pair composes to the identity both ways;
- every `dete_data` row agrees with the `subcol` table and with both
  identities;
- `pix` → `asic_ch` is injective, and only those 13 `asic_ch` have a
  `pix`.

A failure is a realize error, so QEMU does not start. A mutation check
(one `dete_data` quarter changed from 1 to 2) made QEMU exit with
`stix_det: dete_data disagrees with the index identities at subcol 2`;
the file was restored. **Fidelity** was checked value by value
against the FSW tree with a scripted dev aid kept outside the repo
(`check-index-tables.py` in the session scratchpad): all 7 FSW tables
plus the 4 `dete_data` columns MATCH, and a mutated copy is reported
as DIFF. An independent review found the same. No qtest drives the
self-check: it cannot fail on
the committed tables, and every qtest starts QEMU through it. D-2 uses
only `grp`, `asic`, `quarter` and `fpga_det`; the `subcol`, `fe_sc` and
`pix` tables are there for D-4 to D-7.

**Model (D-De), at the FSW-visible register level:**
- **ideFxCmdSta (0x00):** a write executes at once. It stores GrpAddr
  (23:20), CmdCounter (19:16) and CmdData (15:0); reserved 30:24 read 0.
  CmdReady (bit 31) always reads 1. Reset value 0x80000000 (D-Dh).
- **idefxModeData (0x04):** ReadMode (31:16, `grp` bits) is R/W. 15:0 is
  read-only: the response of the last command.
- **idefxWriteMask (0x08):** the mask (31:16, `grp` bits) is R/W.
  Reserved 15:0 read 0 (the FSW writes them 0).
- **idefxConfig (0x0C):** stored as before (DET_C still reads it, §22).
  A quarter-power 0 → 1 edge (bits 3:0) resets the ASIC registers of
  that `quarter`'s four `grp` and idles their decoders (a power-on
  reset).
- **Delivery:** a word goes to every `grp` in WriteMask, or to GrpAddr's
  `grp` when WriteMask is 0 (lenient). A `grp` takes part only if its
  `quarter` is powered, its Enable bit (idefxConfig 19:4) is set and its
  ReadMode bit is 0 [D, RP-0067]. Otherwise the word is ignored and
  CmdReady stays 1. The response comes from GrpAddr's `grp`, or is 0.
- **The per-`grp` decoder:**
  - cmd 0 (RESET) and cmd 4 (SCEND) end the transaction.
  - cmd 11 (SCINITSEQ) latches RD (bit 9), SC (8), All (7) and the
    address (6:4). RD = 0, SC = 1 opens slow control: address 3 is `asic`
    0, address 7 is `asic` 1, and All = 1 addresses both (the FSW word
    is `0xBFD30 | asic << 6`: `asic` 2 sets All). RD = SC = 1 resets
    the **addressed** ASIC to the defaults, both with All [A, below].
    RD = 1, SC = 0 (readout) is not modelled. Every SCINITSEQ first
    idles the decoder.
  - The first cmd 8 is the address block: R/W in bit 14, the register in
    13:7. Each further cmd 8 is a data block. A write shifts byte 14:7
    into the register; a read answers the next byte.
  - cmd 1 answers by context: A = 0x900 for the first cmd 1 after a
    reg-13 read address block, B = 0x700 for the cmd 1 after the
    following data block, else the baseline 1850. cmd 2 and 3 answer
    1850. Every other command answers 0.
- **Registers:**
  - 32 ASICs (`fpga_det`) × registers 1..12, sizes 2, 3, 192, 32, 4,
    2, 5, 2, 2, 3, 3, 32 bits (`commonFSW.h:251-262`).
  - Each is an N-bit shift register that keeps the last N bits written
    (IDeF-X DS). A read emits the N bits MSB first, left-aligned and
    zero padded, so a short register reads `value << (8 − N)`, which
    `readReg` shifts back (`Detectors.c:838-841`). Bytes past the end
    read 0.
  - R3 is stored raw, in wire order (the FSW reorders it on both sides,
    `utils.c:663-677`, self-inverse).
  - R0 (TEST) and R13 (TEMP) store nothing. R13 only sets the ADC
    context.
- **Defaults** (`ParamMgmtDef.h:525-536`; the DS table agrees): R1 1,
  R2 0, R3 62 on every `asic_ch` (raw FB EF BE × 8), R4 0, R5 15, R6 1,
  R7 0, R8 3, R9 0, R10 3, R11 3, R12 0xFFFFFFFF. They load at device
  reset, on the RD = SC = 1 command and on a quarter-power edge.
- **Device reset (det-model on):** ideFxCmdSta = 0x80000000, the other
  detector registers as §31; every decoder idles (a half-open
  transaction is dropped) and all 32 ASICs take the defaults (the IDPU
  reset cuts the ASIC supply [I]).
- **`det-idefx`** (qdev bool, default on): off keeps 0x00-0x0C plain
  storage with CmdReady 0, which reproduces the 384 timeouts. The rest
  of the detector model stays on; the reset writes ideFxCmdSta 0.
- **Re-entrancy:** the decoder runs in the MMIO write path only and
  touches no guest RAM, timer or IRQ.

**Design choices and deviations:**
- **Boot `resetReg` × 16 is a no-op.** The boot powers nothing and
  disables every `grp` first (`OpModeMgmt.c:1057-1064`), so the words
  are ignored, as RP-0067 says commands to a disabled `grp` are
  (plan Doc vs code 9). The registers already hold the defaults from
  the device reset. The result is never logged, so this is invisible.
- **A read with All = 1, or of no ASIC, answers 0.** The FSW never does
  this (`readReg` allows `asic` ≤ 1, `Detectors.c:800`); the DS gives
  All a readout meaning only.
- **Reads are non-destructive** and restart at byte 0 with each
  SCINITSEQ (which idles the decoder).
- **RD = SC = 1 resets only the addressed ASIC** [A]. The IDeF-X DS
  says "(1,1) resets the registers of all ASICs". But the FSW resets one
  ASIC per `subcol`: `XRayADCsCtrl_resetASIC` calls
  `Detectors_resetReg(grp, 0 or 1)` once per masked `subcol`
  (`XRayADCsCtrl.c:833`). Resetting the sibling too would make the next
  verification report it (0x542E), a TM the hardware most likely does
  not produce. The model follows the FSW's intent and the address
  field; the boot's `resetReg(grp, 2)` (All) still resets both. This is
  a doc-vs-code item; the code's reading wins.
- **A disabled `grp` in a powered `quarter` reads zeros.** If a TC
  clears a `grp`'s Enable bit while its `quarter` stays powered (ReadMode
  does not matter: `readReg` clears it itself), the verification still reads that `grp`, because
  `dmask` is built per powered quarter (`XrayDetFDIR.c:151-163`). The
  reads answer 0, so the FSW reports 0x542E, rewrites (also ignored),
  then sends 0x9438 and puts the ASICs into `CALISTE_IGNORE_FLAGS` for
  good. This is plausibly hardware-faithful (RP-0067: commands to a
  disabled `grp` are ignored; the FSW's TC procedures note that
  "QUARTER POWER & GROUP ENABLED is tested elsewehere (TC)",
  `XRayADCsCtrl.c:519`, also :1023, :1258, :1539). No gate disables
  a `grp`; it is stated here so that a later TC gate expects it.
- **ADC answers are canned:** readTemp gives A − B = 0x200, adcRead and
  the on-demand reads give 1850 (a known limit, plan "TC-driven
  procedures stay unmodelled").
- **No SEU** (QuarterSeu stays 0, §31).
- **The ALIMON (R12) and R4 per-`asic_ch` bit order is not used.** It is
  undefined in the code; D-2 only stores bytes (plan Doc vs code 12).

**Doc vs code** (the code wins; the plan's numbering):
- 1: the HSWICD's "busy, active low, reset 1" means 1 = ready, as in the
  FSW and the TSIM model. The model reads 1.
- 8: the DSS manual never documents slow-control read-back; the FSW's
  verification depends on it, so it is modelled.
- 9: the boot register reset is a no-op on hardware (see above).
- New: the DS's RD = SC = 1 "all ASICs" versus the FSW's per-ASIC
  `resetReg` (see Design choices). The model resets the addressed ASIC.

**Traces:**
- `stixdet_idefx_cmd(grp, word, delivered_grp_mask, response)`, one per
  ideFxCmdSta write. `word` is the FSW's 20-bit form (`cmd << 16 |
  data`), so a verification read address block shows as
  `word 0x84000|reg<<7`; `grp` is GrpAddr.
- `stixdet_idefx_reg(fpga_det, reg, is_read, byte_idx, byte)`, one per
  data block per addressed ASIC.
- `stixdet_idefx_defaults(fpga_det_mask, why)`: device reset,
  RD=SC=1, or quarter power on.

**Tests** (RED first on d2c6cae28a, then green):
- `det-idefx-ready`: reset reads 0x80000000; CmdReady 1 before and after
  every word of the boot's resetReg × 16 (unpowered), a write/read, a
  resetReg, a readTemp and the undecoded cmd 5/9/12/15; the command
  fields read back (0x7FF5ABCD → 0x80F5ABCD); idefxModeData 15:0 and
  idefxWriteMask 15:0 are not writable; 0x80000000 again after
  `system_reset`. RED: `0x00000000 == 0x80000000`.
- `det-idefx-roundtrip`: registers 1..12 on both `asic` of `grp` 3 and
  12 through the FSW's writeReg/readReg; the neighbouring `grp` 2 and
  13 keep their defaults; R1 0x01 reads 0x40 on the wire and R7 0x15
  reads 0xA8; R4 and R12 as u32; overlong writes keep the last N bits
  (R4 five bytes, R1 and R7 two bytes). RED: CmdReady 0 at the first
  word.
- `det-idefx-broadcast`: `asic` 2 writes R5 and the 24 bytes of R3 into
  both ASICs, and not into the neighbours. RED: as above.
- `det-idefx-reset`: resetReg(grp, 0) restores asic 0 only and
  resetReg(grp, 1) asic 1 only (each sibling keeps its value);
  resetReg(grp, 2) (All) restores both. A quarter power 1 → 0 → 1 edge
  restores only that quarter and idles its decoders (a half-open write
  across the edge writes nothing). A write of the already powered mask
  does not reset. `system_reset` restores all and drops a half-open
  transaction. RED: as above. The edge-idle assertion was mutation
  checked: without the idle it read 3, not 15.
- `det-idefx-gating`: a disabled `grp`, ReadMode = 1 and an unpowered
  `quarter` each ignore the command, with CmdReady 1 and response 0;
  WriteMask 0 delivers to GrpAddr; a WriteMask naming another `grp`
  delivers there, and the response from a GrpAddr outside the mask is 0.
  With two `grp` in WriteMask holding different R5 values, a read
  answers GrpAddr's value (0x40 for `grp` 6, 0x50 for `grp` 7). ReadMode
  gates reads too: `grp` 6 in readout mode answers 0, alone or next to
  `grp` 7, while `grp` 7 still answers. RED: as above.
- `det-idefx-adc`: readTemp gives A 0x900 then B 0x700 and leaves the
  registers alone; adcRead gives 1850 in rx[1] and 0 elsewhere; cmd 2
  and 3 give 1850. RED: as above.
- `det-idefx-off`: CmdReady 0 (a transmission would time out); the three
  registers store raw; the reset still runs (ideFxCmdSta 0,
  testPulseCmdSta Busy). RED: the property is unknown.
- `det-reg-reset` now expects ideFxCmdSta 0x80000000. RED:
  `0x00000000 == 0x80000000`.
- The full suite is 54 ok; checkpatch is clean on the diff and on
  `stix_det.c`/`.h` with `-f`.

**nominal-smoke (D-2 changes):**
- 0 `XrayDetFDIR.c:731` lines are expected in both modes; any other
  count is a FAIL.
- Zero timeouts alone would also pass if NOMINAL entry returned early,
  before `OpModeMgmt.c:576` (the `gracefulFDIRtoSAFEModeRequest`
  returns). So QEMU now runs with `-trace stixdet_idefx_cmd`, and the
  gate also requires **positive evidence**: at least 32 delivered write
  address blocks for **each** register 1..12. A write address block is
  the word after a delivered SCINITSEQ (0xBFD30/0xBFD70/0xBFDB0) of the
  same GrpAddr with R/W = 0. Tracking per GrpAddr keeps the count right
  if FSW tasks interleave on different `grp`.
- The count is exact and robust: `XrayDetFDIR_resetASICregistersToDefault`
  is 32 ASICs × 12 registers. The only other writer in these runs is
  the RCR state-0 R3 write (`RateCtrl.c:575`), so R3 reads 64. Per
  register is stricter than a total of 384, which R3 alone could fill.
- Why not a probed TM marker: no TM is provably sent after `:576` on
  every path, and the trace counts the writes themselves.
- The check needs gawk (`strtonum`, `and`, `rshift`); its absence is
  exit 2.

**Goldens:**
- `uart.golden.log`: unchanged. boot-smoke PASS, and the normalized UART
  of the capture run is identical to the golden (whole file).
- `qemu-stdout.golden.log`: regenerated with `capture-golden.sh` (an
  absolute QEMU path) into a scratch out-dir; only this file was copied
  in. 30606 → 14815 lines:
  - the 16000 `addr = 0x0` polls become 112 ideFxCmdSta accesses
    (16 × (1 + 3 × 2)) reading 0x80000000 (0x800bffb0 after the
    resetReg word);
  - 48 new `stixdet_idefx_cmd` lines, all with
    `delivered_grp_mask 0x0000`: nothing is powered at boot;
  - one `stixdet_idefx_defaults … (device reset)` line;
  - the idefxWriteMask/idefxModeData accesses of the 16 resetReg
    frames.
  - The capture is time-bounded (90 s), so the removal does not shift a
    line window.

**Gate record** (2026-11-11, worktree `stix-uprev-tc` at d2c6cae28a
plus this change, normal ELF, every QEMU run under the 21000 flock):
- boot-smoke (`/tmp/stix-uprev-run`): PASS (the normalized UART matches
  the golden).
- **nominal-smoke positive** (`/tmp/stix-d2-nominal`, fresh flash):
  PASS.
  - [L] confirmed: **0** `XrayDetFDIR.c:731` lines; NOMINAL entry
    (`0021 04 05`) logs no IDeF-X error at all. The UART has 58 lines.
  - Its set of UART message types (numbers masked) equals that of the
    D-1 run still on disk (`/tmp/stix-d1-nominal`, the negative run),
    minus that run's 731 lines and its expected "Transitioning to safe
    mode" line. No new UART line and no new TM: the TM(5,x)
    set is the same, with the one TM(5,2) 0x542C p1 = 1 (no LUTs), and
    there is no 0x5DC while NOMINAL lasts.
- **nominal-smoke `--negative`** (same dir, wiped by the script): PASS.
  - 0 timeouts. NOMINAL lasted about 31 s (`0021 04 05` at 15:33:13,
    `142E` and `0021 05 02` at 15:33:43-44).
  - The exit flush is as in D-1: four TM(21,6) 0x5DC packets, SSID 33,
    30, 31 and 34, and no SSID 32.
- **[L] ASIC verification evidence** (an ad-hoc traced variant of the
  positive run, kept outside the repo, `/tmp/stix-d2-verif`, fresh
  flash):
  - It adds TC(236,6) `02 02 00 1E` (verification period 30 s; acked,
    TM(1,7)) at +62 s, `-trace 'stixdet_idefx*'`, and forbids 0x542E,
    0x9438 and 0x142C from NOM onward. PASS: no forbid fired.
  - The trace has **3 verification runs of exactly 384 read address
    blocks** (`0x84000|reg<<7`, reg 1..12, each over 32 ASICs, 16
    `grp`). All 1152 were delivered, and their 3936 read data blocks
    are 3 × 1312, i.e. 41 bytes per ASIC.
  - The writes are the 384 NOMINAL-entry writes plus a second R3 write
    on all 32 ASICs, which is the RCR state-0 threshold write
    (`RateCtrl.c:575`) [I]. It timed out silently before D-2.
  - Its UART message types and TM set equal the positive run's.
  - A successful verification is silent, as the plan says: the trace
    is the only evidence.
- **fault-smoke positive** (`/tmp/stix-d2-fault`, fresh flash): PASS.
  The same 18 expectations as in D-1 matched, including DET_C
  `9434 05 0258 0222`, and there was no 0x5DC TM. SAFE never powers a
  quarter, so the IDeF-X model is idle.
- The full qtest suite is 54 ok; checkpatch is clean.
- The shared flash mtimes are unchanged: `/tmp/stix-uprev-run`
  2026-11-10 13:05:54 and `/tmp/stix-baseline-run` 2026-11-09 15:30:10.
- **After the review fixes** (RD = SC = 1 per addressed ASIC,
  range-checked self-check, edge idles decoders, the realize error path
  frees the watchdog timer and the ROTB ring, the new gating and reset
  tests, and the nominal-smoke evidence check):
  - the full suite is 54 ok; checkpatch is clean (diff and `-f`);
  - boot-smoke PASS (the normalized UART matches the golden). The trace
    golden is not regenerated: the boot's `resetReg(grp, 2)` is still
    undelivered, so the trace is unchanged;
  - nominal-smoke positive and `--negative` (`/tmp/stix-d2r-nominal`,
    fresh flash): both PASS with 0 timeouts and `R1..R12 = 32` delivered
    write address blocks each (R3 = 64);
  - the shared flash mtimes are unchanged.

## 33. Detector smoke gate and the probe's full-packet dump (baseline: none)

**Baseline:** no gate looked at the detector model through the FSW. The
probe logged at most 48 bytes of each unit, so no QL TM(21,6) could be
decoded, and a successful ASIC verification left no evidence at all.

Detector plan D-3 (`docs/stix-port/2026-11-11-detector-plan.md`,
"### D-3", Key facts "QL TM decode for the gates" and "The TCs the
gates use", Validation R1/R2). Scripts only; no model change.

**`tmtc-probe.py --dump-apid APID:FILE`** (repeatable, one file per
APID): every framed TM unit of that APID is appended to FILE as its
whole CCSDS packet (no 4-byte SpW header, so the SSID is byte 16), hex,
one per line, flushed per line. It works in gate and plain mode and
changes nothing else (the 48-byte log lines stay). Bad specs (no file,
bad or out-of-range APID, an APID twice) are exit 2. The self-test
adds a case: a 68-byte QL unit split across two chunks is dumped once,
whole, only when complete; an ack APID goes to its own file; an
undumped APID is not written; the bad specs are rejected. A second
case drives the plain mode (`run_plain`, factored out of `main`) over
a socketpair and checks the same dump line.

**`ql-check.py`** (new, stdlib only) decodes such a dump:
- packet checks: APID 0x5DC, TM (not TC), TM(21,6), the CCSDS length
  equals the line length, a QL SSID 30-34, and the SSID body consumes
  every byte (truncation and trailing bytes are errors; every band,
  trigger and RCR block must repeat the first block's N);
- segmentation (CCSDS byte 2 bits 7:6, `TMData.h:89-92`): STANDALONE,
  or FIRST, CONT…, LAST per SSID; each segment is a full packet with
  its own header (`TMForm_nextSegmentationGroupingFlagQL`), so a
  product's samples are concatenated; an empty LAST (the termination
  packet) is tolerated and flagged; CONT/LAST without FIRST, FIRST
  while open and a product never closed are errors;
- sequence counts: the 0x5DC counter is one per APID
  (`TM_21.c:780`, `TMForm_SequenceCounters.APID_93_12`), shared by all
  QL SSIDs, so one product's segments interleave with other SSIDs'
  packets and are not contiguous among themselves (the flush above
  counts 0..4 across five SSIDs). So the dump as a whole must count up
  by one (mod 2^14; a gap is a lost packet), and within a FIRST..LAST
  group the counts must strictly increase;
- S0 (M = 3) compress and decode, the decode printed as a lower and an
  upper bound;
- the SSID 30-34 layouts of `TM_21.c:315-647`, the field names carrying
  their index space and SSID 34's offset 7 keeping the FSW name
  `delta_time_seconds` (`deltaTimeSeconds`, `TM_21.c:624`, 0.1 s
  units) (`det_mask_subcol`, `lower_edge_mask_ebin`,
  `energy_mask_ebin`, `subcol N ebin[0..31]`, `pix_mask`);
- `--samples` prints every sample; `--expect-zero-flush SECONDS` is the
  gate check (below).
- `--self-test`: the S0 vectors of the plan (0, 15, 16, 100 → 0x24,
  1000 → 0x3F, **2160 → 0x48** decoding to [2048, 2304), 10000 → 0x59,
  65535 → 0x6F) and monotonicity; the worked example built as an
  SSID 30 packet (6 `ebin` × 12 `pix` × 30 `subcol` = 2160, band-0 N at
  rel. 22, the byte 0x48 at rel. 24); one packet per SSID 30-34; the
  SSID 32 sample layout; FIRST/CONT/LAST reassembly interleaved with
  another SSID and a FIRST + empty-LAST pair; eight known-bad inputs
  that must raise; and the zero-flush check catching a nonzero byte, a
  missing SSID, wrong sample counts, a duplicated `subcol` and a
  non-default mask. Mutation checked: a wrong S0 exponent, a dropped
  LAST and a disabled nonzero check each fail the self-test.

**`detector-smoke.sh`** (new), built like nominal-smoke with the
fault-smoke QMP socket (unused here; for D-4/D-6) and the same run-dir
guard (`STIX_DETECTOR_RUN_DIR`, default `/tmp/stix-detector-run`),
fresh flash, `--dry-run`, `--check-guard`, `--negative`; the probe runs
with `--hold`, 240 s (`STIX_DETECTOR_SECONDS`, ≥ 235). The port is
20000 + uid, as in the other gates. Timeline (seconds from connect):
- connect: 0x8030, 0x0022, 0x14A3, 0x142E (nominal-smoke's group);
- @50 TC(236,1) `04 00`: TM(1,1), `0021 02 04`, TM(1,7); forbid
  `0021 04 02`;
- @60 TC(236,6) `07 04 00 00` and @62 TC(236,6) `02 02 00 1E`
  (verification period 30 s; seq 3, CRC 8AE9): TM(1,1), TM(1,7) each;
- @70 TC(236,1) `05 00` (seq 4, CRC 0E3C): `0021 04 05` within 10 s
  (T_nom). Forbidden from here to the end: 0x542E, 0x9438, 0x142C,
  0x9433, 0x1427, 0x942E, 0x5437, `0021 05 02`, TM(1,8), 0x142E (the
  TM(5,x) subtype from the EID's severity nibble);
- @215 TC(236,1) `04 00` (seq 5, CRC D62E): `0021 05 04` within 10 s
  (T_exit) and the flush `0x5DC:21:6:1e`…`:22` within 15 s; forbid
  `0021 04 02`.

After the run every check runs and records its name, so the report is
complete:
- **probe**: every expectation matched, no forbid fired;
- **ql**: `ql-check.py --expect-zero-flush (T_exit − T_nom)`: exactly
  one product per SSID 30-34; the default headers (int time 39/79/319/
  39/39, det mask 0xFFFFFCFF, pixel mask 0x0FFF, SKM 0x2B, edges
  0x10821082 with no upper edge, E = 5, samples per value 40, energy
  mask 0x7FFE0000); every value byte 0; SSID 30/33/34 within ±2 of
  window/4 and SSID 31 of window/8; SSID 32 exactly the 32 `subcol`
  0..31 once each. The window is taken from the two TM arrival times,
  not from 215 − T_nom, which is the same quantity measured;
- **uart**: "init: SAFE mode"; none of "storing new context[01]
  failed", "packet sequence error", "RotBuf overrun", "UNKNOWN
  RB-BLOCK", "TOO MANY RB-BLOCKS";
- **uart-idefx**: no `XrayDetFDIR.c:731` and no "failed to read
  register" (`XrayDetFDIR.c:613`);
- **trace-verif** (`scripts/stix/idefx-trace-check.py`, stdlib): QEMU
  runs with `-msg timestamp=on -trace
  enable=stixdet_idefx_cmd,file=<run>/idefx.trace`, so each trace line
  has a UTC ISO time. Delivered read address blocks (`word & 0xFC07F ==
  0x84000`, reg 1..12, `delivered_grp_mask != 0`) are clustered by a
  gap > 5 s. Each cluster must be exactly 384, with every register
  read on every `grp` 0..15 (`grp` = `fpga_det`/2) exactly twice; it
  must lie in [T_nom − 2, T_exit + 2] s (the probe's local HH:MM:SS
  converted to epoch, anchored at the run start); there must be ≥ 3;
  an undelivered read address block is a failure too. A missing or
  unreadable trace is exit 2. Its `--self-test` builds synthetic lines:
  three runs of 384 pass; 383 blocks, an undelivered block, a run
  outside the window, one `grp` read for another's register (`grp` 7
  ×4, `grp` 3 ×0, still 384 in total) and only two runs each fail;
  `--expect-none` accepts only an empty trace; a probe time after
  midnight lands on the next day.

**`--negative`** adds `-global grlib-stixctrl.det-idefx=off` (§32).
Per Validation R1 (option 1; the `det-idefx-readback=zero` TM-level
knob was rejected) no TM fault is expected. The gate passes only if
the failed set is exactly {uart-idefx, trace-verif}, with exactly 384
write timeouts and "failed to read register" lines a multiple of 12
(one per register per run, `XrayDetFDIR.c:611-615`), at least 36 (3
runs). trace-verif counts as failed as planned only when
`idefx-trace-check.py --expect-none` finds the trace present and with
no IDeF-X command words at all (the trace is empty: with det-idefx off
no `stixdet_idefx_cmd` fires). A missing, unreadable or non-empty trace
records a different check (`trace-not-empty`), so the negative FAILS;
it is not the vacuous branch of the positive check. The probe (all
forbids quiet, the flush seen), ql and uart must still pass.

**Exit codes:** 0 pass, 1 fail, 2 environment, **77 SKIP** (no ELF),
the convention for new gates (README "Gate exit codes"; the older
gates keep 0 on SKIP as legacy).

**Design choices:**
- The trace time stamps come from QEMU's own `-msg timestamp=on`
  (`qemu_log` prefixes the ISO time); no model or trace-event change.
  Clustering by time rather than by sequence keeps the check correct
  if NOMINAL-entry writes or other `grp` traffic interleave.
- Per-cluster exact 384 and per-register 32 rather than a total
  ≥ 1152, so one long run or a partial run cannot pass.
- The flush SSIDs are probe expectations (timed) and ql-check checks
  their content; the dump keeps everything for the D-4/D-6 gates.
- No interactive hold flag: the plan's `--hold` is the probe's.

**Observed QL TM** (positive run): the exit flush is five standalone
packets, in the order SSID 33, 30, 31, 34, 32 (sequence counts 0..4),
of 74, 297, 146, 132 and 1151 bytes (SSID 32: 16 + 15 + 32 × 35).
NOMINAL lasted 139 s (T_nom 16:35:02, T_exit 16:37:21):
- SSID 30: 35 samples (139/4 = 34.75), all band, trigger and RCR bytes
  0; int time 39, `det_mask_subcol` 0xFFFFFCFF, `pix_mask` 0x0FFF, LC
  and TA SKM 0x2B, `lower_edge_mask_ebin` 0x10821082, E = 5;
- SSID 31: 17 samples (17.4), all 0; int time 79;
- SSID 32: one dump, 32 samples, `subcol` 0..31 in order, every `ebin`
  count and trigger 0, int offset 0; int time 319;
- SSID 33: 35 values, all 0; samples per value 40, `energy_mask_ebin`
  0x7FFE0000;
- SSID 34: 35 status triplets, all 0.
The TM(5,x) set is that of nominal-smoke (0x8030, 0x0022, 0x14A3,
0x142E, `0021 00 02`, `0021 02 04`, `0021 04 05`, `0021 05 04`, the
0x542C p1 = 1 of the empty LUT flash, 0x0032); no forbidden EID. The
UART has 59 lines (58 newline-terminated plus the unterminated last
one; `wc -l` says 58).

**ASIC verification** (positive): 4 runs of 384 delivered read address
blocks at T_nom +20.1, +50.9, +83.5 and +114.9 s, each 0.03-0.04 s
long. The spacing is 30.8-32.6 s of host time for the 30 s period
(the FSW counts its 1 s FDIR ticks; the host-time spread is the
emulation's, not checked). The first came 20 s into NOMINAL, not at
entry: the FDIR counter fires in every mode and resets on each fire
(`FDIR.c:479-486`), while `XrayDetFDIR_verifyASICreg` returns outside
NOMINAL (`XrayDetFDIR.c:310-315`). The counter was already past 30
when the TC set the period at +62, so it fired in CONF the next second
(skipped), and the next fire was 30 s later, about 20 s after T_nom
[C, consistent with the timing]. The plan's "the first may land within
1 s of the TC" is the CONF fire, which leaves no trace; the window
check allows a run at entry too.

**Gate record** (2026-11-11, worktree `stix-uprev-tc` at 93438dfa3b
plus this change, normal ELF, every QEMU run under the 21000 flock):
- `ql-check.py --self-test` and `tmtc-probe.py --self-test`: PASS.
- **detector-smoke positive** (`/tmp/stix-d3-pos`, fresh flash):
  `DETECTOR-SMOKE: PASS` (above).
- **detector-smoke `--negative`** (`/tmp/stix-d3-neg`, fresh flash):
  `DETECTOR-SMOKE (negative): PASS (failed exactly as planned: 384
  write timeouts, 48 read failures = 4 verification runs, no IDeF-X
  command words at all; the trace is empty)` (wording of the review
  rerun). The trace file is empty (with det-idefx off no
  `stixdet_idefx_cmd` fires); NOMINAL lasted 135 s; the flush and its
  decode are identical in content to the positive run (35/17/32/35/35
  samples, all 0); no forbid fired.
- The full qtest suite: 54 ok. boot-smoke (`/tmp/stix-uprev-run`):
  PASS, the normalized UART matches the golden.
- nominal-smoke positive (`/tmp/stix-d3-nominal`, fresh flash): PASS,
  0 `XrayDetFDIR.c:731`, R1..R12 = 32 delivered write address blocks
  (R3 = 64), as in §32.
- The shared flash mtimes are unchanged: `/tmp/stix-uprev-run`
  2026-11-10 13:05:54 and `/tmp/stix-baseline-run` 2026-11-09 15:30:10.
- **After the implementation review** (the trace analyser moved to
  `idefx-trace-check.py` with the per-`grp` check and `--expect-none`;
  ql-check sequence-count checks and the SSID 34 field name; the
  plain-mode dump self-test; SKIP 77; docs):
  - `ql-check.py --self-test`, `idefx-trace-check.py --self-test` and
    `tmtc-probe.py --self-test`: PASS. Replayed on the first runs'
    files, the analyser gives the same 4 × 384 (17136 command words, 0
    undelivered) and `--expect-none` passes on the empty negative trace;
    a missing trace is exit 2.
  - detector-smoke positive (`/tmp/stix-d3r-pos`): PASS; NOMINAL 139 s,
    the same flush (35/17/32/35/35, all 0); 4 runs × 384 at T_nom
    +21.5, +53.4, +89.3, +124.0 s.
  - detector-smoke `--negative` (`/tmp/stix-d3r-neg`): PASS, 384 write
    timeouts, 48 read failures, `CHECK trace-verif: FAIL as planned
    (the trace is empty: no IDeF-X command words at all)`.
  - **The negative with no trace** (`/tmp/stix-d3r-negdel`):
    `idefx.trace` was deleted ~5 s after QEMU created it. The analyser
    reported exit 2 (cannot read), the check was recorded as
    `trace-not-empty`, and the gate printed `DETECTOR-SMOKE (negative):
    FAIL (failed checks: 'uart-idefx trace-not-empty', ...)`, rc 1, as
    required.
  - The shared flash mtimes are unchanged.

## 34. Synthetic sky and the flare switch (baseline: no counts)

**Baseline:** the QL banks were written only with zeros (§31): every
LACC cell, every LTRG word and every LVAR slot of a finished bank was
0, SUMCNT (fsmEvents) stayed 0, and the FSW's QL products (SSID 30-34)
carried only zeros. There was no way to make the detectors see
anything.

Detector plan D-4 (`docs/stix-port/2026-11-11-detector-plan.md`,
decisions D-Db, D-Df, D-Dd, D-Dj; "Index spaces").

**FSW tree:** every FSW `file:line` in this section is STIX-FSW
600a7a72 (`ASW/ASW/src/...`; `Detectors.c` is
`BSW_ASW/src/BSW/Detectors/Detectors.c`). None is from stix_asw_src.

**The source: accumulators, not events (D-Df).**
- **Per 100 ms tick** (inside the existing D-1 tick, so only while FSM
  Enable = 1, i.e. in NOMINAL):
  - each (`fpga_det`, `pix` 0..11) is gated: its `grp` (= `fpga_det` /
    2) must read out (its `quarter` powered in idefxConfig, its Enable
    bit set, its **ReadMode bit 1**; the FSW sets the mode mask to
    0xFFFF at NOMINAL entry, `OpModeMgmt.c:513`, and clears one `grp`'s
    bit during each slow-control transaction, `Detectors.c:811`), and
    the R3 threshold of `asic_ch` = `utils_pixelId2channelId[pix]` must
    not be 63. The gating is evaluated every tick, so a verification
    run that clears one `grp`'s ReadMode for part of a tick costs that
    `grp` at most a tick, not a period;
  - the threshold is read from the raw (wire-order) R3 store: the FSW's
    `utils_getReg3(asic_ch, utils_reorderThreshold3(raw))`
    (`utils.c:538-555, 663-677`) is the 6-bit field number `asic_ch` of
    the raw bytes counted MSB first (checked exhaustively against a
    transcription of both FSW functions on random registers);
  - ALIMON (R12) is not used (its per-`asic_ch` bit order is undefined,
    plan Doc vs code 12);
  - every gated pair adds `rate × 0.1 s` to its exposure for the active
    profile (quiet, or flare while `det-flare` is true);
  - SUMCNT gets a Poisson draw of the tick's counts in SUMDMASK
    `fpga_det` × SUMEMASK `ebin` (all `pix`), saturating at 2^32 − 1;
    fsmEvents reads it live;
  - the tick's LVAR count is a Poisson draw of the counts in VARDMASK
    `fpga_det` × VARPMASK `pix` × VAREMASK `ebin`, written to the
    slot as before (slot rule of §31, clamped at 16 bits).
  - The FPGA-only masks are read with bit i = index i ([A], plan "Index
    spaces"; SUMDMASK and VARDMASK are `fpga_det`-indexed, the FSW
    converts from `subcol` before writing them).
- **At the QL swap** the finished bank's LACC is drawn cell by cell,
  `[pix][fpga_det][ebin]` u16 big-endian at byte
  ((`pix`·32 + `fpga_det`)·32 + `ebin`)·2: Poisson(Σ_profile `pix`
  share × `ebin` share × exposure), wrapping at 16 bits. LTRG per `grp`
  (u32 big-endian) = round(1.25 × the drawn events of `fpga_det` 2·`grp`
  and 2·`grp` + 1), clamped at 24 bits. Then the exposure clears. With
  `det-source=zero` the exposure is zero and the bank is all zeros, so
  the §31 lifecycle is unchanged; the D-1 tests still see zeros because
  their detectors are never powered.
- The exposure also clears on a fresh enable (with the bank, §31) and on
  a device reset; a Reset strobe keeps it (it keeps the banks).
- **Template [I]:** uniform over `fpga_det` (QLDetPerf); `pix` share
  1.0 for `pix` 0-7 and 0.22 for `pix` 8-11, normalized; quiet: 10 %
  flat over `ebin` 1-11, 15 % flat over `ebin` 12-30, and 45 / 10 /
  20 % in `ebin` 18 / 19 / 27 (Ba-133 lines); flare: 70 % thermal over
  `ebin` 1-10 (×0.7 per bin) and 30 % power law (bin number^−3) over
  `ebin` 11-25. Nothing in `ebin` 0 or 31. The rate is counts/s per
  `fpga_det`, so a period holds about rate × 4 s × 32 counts.
- **PRNG:** xorshift64*, seeded through splitmix64 from `det-seed`
  (qdev, default 1) at every device reset. Poisson by inversion below a
  mean of 30, by a rounded normal (Box-Muller) above. On one host the
  same seed and the same steps give the same banks (`det-counts-seed`);
  across hosts the libm may differ, and there is no icount, so runs are
  statistically reproducible only.
- **No ELUT:** the sky is generated in `ebin` space. With an empty
  flash the FSW writes 31 into every FPGA ELUT entry (plan Key facts,
  "The ELUT"), so real hardware would put every count in `ebin` 31.
  **This is a deliberate deviation** (plan Doc vs code 20): a gate that
  compares QL against real hardware with an empty flash must not use the
  synthetic sky.

**The scene properties (D-Db), on `/machine/stixctrl`:**
- runtime class properties with setters (they also work before realize,
  so `-global grlib-stixctrl.det-source=zero` and a `qom-set` at `-S`
  both work):
  - `det-source` (str): `zero` | `synthetic` (default `synthetic`);
    anything else is rejected (`replay-dss` arrives with D-7);
  - `det-flare` (bool, default false): the flare switch;
  - `det-quiet-cps`, `det-flare-cps` (uint32, counts/s per `fpga_det`,
    defaults 20 and 2000, 0..1 000 000; above is rejected, not
    clamped);
- `det-seed` (qdev uint32, default 1, init-time): **an explicit
  exception to D-PLm req. 5**. It is settable only with `-global` (or
  `-device`), not by `qom-set` at `-S`, because the PRNG is seeded at
  realize and at each device reset;
- a setter only stores the field and traces `stixdet_scene(source,
  flare, quiet_cps, flare_cps, vclock_ns)`; the next tick on the
  virtual clock uses it;
- the scene survives a device reset (it describes the Sun, not the
  IDPU); the exposure and the PRNG state do not;
- **a switch mid-period** acts from the next 100 ms tick and does not
  rewrite the past. Each tick adds exposure under the source, flare
  state and rate in force at that tick. Exposure already gathered stays
  and is drawn at the swap. So `synthetic` → `zero` mid-period still
  gives a non-zero bank, holding the ticks before the switch, and the
  next bank is all zeros. `zero` → `synthetic` gives a partial bank.
  A flare flip mid-period mixes the two profiles in proportion to the
  ticks spent in each. The swap draws Σ_profile, so the bank is one
  Poisson draw of the mix;
- the swap traces `stixdet_ql_sky(done_ql_bank, source, flare,
  lacc_events)`.
- The code adds the properties from `stix_det_class_init(klass,
  offsetof(STIXCTRL, det))` and sets the defaults in
  `stix_det_instance_init`, so `grlib_stixctrl.c` has one call site each
  and `struct STIXCTRL` stays private.

**Re-entrancy (D-Dj, the a83fe9a3a5 rule):** the new work runs in the
tick, under the BQL, and writes the banks only through
`memory_region_get_ram_ptr` + `memory_region_set_dirty`; its only other
output is the existing `qemu_irq_pulse`. Nothing dispatches MMIO or DMA
outside a handler, so `mem_reentrancy_guard` is not engaged. The QMP
setters store fields only.

**Platform interface (platform plan D-PLm, requirements on the detector
model):**
- met by D-4: the flare switch and the sky knobs are QMP `qom-set`
  properties, readable (req. 4: `det-source`, `det-flare`, the rates),
  settable while frozen at `-S` (req. 5, `det-props`; `det-seed` is the
  stated exception above), applied on the
  virtual clock at the next 100 ms tick (req. 6), no per-event
  streaming (rule 1), and every name and trace argument carries its
  index space (req. 7);
- deferred to D-7 (replay): the STXEVT v1 table and its runtime load by
  path (req. 1-2), `det-replay-trigger`/`-start`, the single
  `det-replay-offset-ms` (which replaces D-Db's `det-replay-offset`
  before D-7 lands), `det-replay-loop` (req. 3), `det-replay-status`
  (req. 4) and the replay start/stop trace with virtual time (req. 6);
- deferred to the platform merge (D-PLo): the `det` section of
  stixctrl's `hw-state` (req. 8); this branch has no `hw-state` yet.

**Gates:**
- **`detector-smoke.sh`** now runs its D-3 modes (positive and
  `--negative`) with `-global grlib-stixctrl.det-source=zero`, so the
  zero-sky checks and their verdicts are unchanged. **Deviation from the
  plan:** the plan put the flare step into the positive mode; the D-4
  brief asked to keep the zero-sky check working with the switch off,
  so the flare is a separate mode:
  - `--flare`: the default synthetic sky, QMP `det-flare=true` @135 and
    `false` @195 (seconds from connect; the CONF @215 as before).
    `ql-check.py --expect-sky-flush (T_exit − T_nom)` checks the flush
    (the D-3 headers and cadences; SSID 30 band 0, SSID 31 and SSID 33
    non-zero in every sample but at most one; every SSID 30 RCR byte 0;
    SSID 32 non-zero for every `subcol`, nothing in `ebin` 0/31, at
    least half the decoded counts in `ebin` 18/19/27); and
    `ql-check.py --lc-contrast ON OFF` (a separate check,
    `ql-contrast`) with the probe's QMP send times minus T_nom: the
    median SSID 30 band-0 byte of the flare samples (≥ ON + 6 s, ≤ OFF −
    6 s) at least 8 compressed steps above the quiet median (≤ ON −
    6 s), ≥ 5 samples each. SSID 34 flags in the window are printed.
  - `--flare --negative` sends `det-flare=false` @135 (no flip; the QMP
    path and the timing stay) and passes only when exactly
    `ql-contrast` fails.
  - Every mode prints the QEMU process's user+sys CPU (`/proc`), for
    the cost record below.
- `ql-check.py` gains `--expect-sky-flush` and `--lc-contrast`, the
  zero-flush check shares the product/header/cadence part with them,
  and the self-test adds a synthetic-sky flush (passes both) and seven
  bad ones (a zero sky, no flip, a too-short window, a risen RCR, a
  count in `ebin` 31, a spectrum without lines, a dark `subcol`).
  Mutation checked: with the contrast threshold at 0 the self-test
  fails.
- nominal-smoke now runs on the quiet synthetic sky (the default, no
  `-global`). It has no QL, RCR or QLDetPerf check. Its forbids are
  unchanged: no NOMINAL → SAFE (TM(5,1) 0021 05 02), no TM(1,8), no
  TC20 reaction (TM(5,1) 0x142E) and no TM(5,3) 0x9449. It also needs
  zero IDeF-X write timeouts (`XrayDetFDIR.c:731`) in the UART. A
  synthetic sky that broke NOMINAL would show there; a risen RCR would
  not. Only `detector-smoke --flare` asserts that every SSID 30 RCR
  byte is 0.

**FSW bug: the NOMINAL R3 enables 4 fixed pixels plus pix 7 by
chance** (confirmed by the coordinator; filed as STIX-FSW issue #1069).
The first live run gave about 0.47 of the plan's [I] count levels in
every QL product: quiet LC band 0 about 60 instead of about 130, and
SSID 32 about 320 instead of 640 per `subcol` in 32 s. The model's own
`stixdet_ql_sky` trace showed about 1200 events per quiet period
instead of 2560.

The cause is the R3 the FSW writes at NOMINAL entry, the RCR state-0
register set (STIX-FSW 600a7a72 `RateCtrl.c:575`). Decoded the FSW's
own way, `utils_getReg3(utils_reorderThreshold3(wire))`, threshold 62
sits on:
- 4 fixed channels: `asic_ch` 1, 5, 26, 30 = `pix` 3, 6, 0, 8;
- `asic_ch` 0 = `pix` 7, which depends on an out-of-bounds read.

Every other channel is 63.

Root cause (STIX-FSW 600a7a72 `RateCtrl.c:898-913`): the build mirrors
the channel twice.
- The local inversion `utils_channelId2pixelId[NUMBER_OF_ASIC_CHANNELS
  - 1 - iChan]` (commit 44e8526b, 2019-09-06) stacks on the global R3
  field reorder (`utils_reorderThreshold3`, commit 60e69d75,
  2019-09-09). Field c therefore takes the active flag of the pixel on
  `asic_ch` 31 − c.
- For `iChan` 0 the lookup yields 12 (the guard ring), and
  `pixels_active_rcr_*[12]` reads one past the 12-entry list. So `pix`
  7 depends on whatever byte follows the list.

Level ratios against the plan's [I], from the `pix` weights (1.0 large,
0.22 small, 8.88 in all):
- `pix` 7 on: 4.22 / 8.88 = **0.475** (this run);
- `pix` 7 off: 3.22 / 8.88 = **0.363**.

The model follows the registers, so this is what the FPGA would count
with this FSW. The TM pixel masks still say 0x0FFF. The model is not
changed. `det-counts-r3-fsw` builds R3 in the FSW's layout,
independently of the model's decoder, and shows two things:
- a correct, non-mirrored build of the full mask makes all 12 `pix`
  count;
- the mirrored build gives {0, 3, 6, 8} or {0, 3, 6, 7, 8}, depending
  on the guard slot.

**The --flare gate holds in either case, and is calibrated against this
bug.**
- Its thresholds do not scale with the enabled pixel weight:
  - SSID 30, 31 and 33 non-zero in all samples but one (a quiet
    period still holds about 900 events at 0.363);
  - SSID 32 non-zero per `subcol`, nothing in `ebin` 0/31, the lines
    holding at least half (the `ebin` shares do not depend on `pix`);
  - the LC contrast of ≥ 8 compressed steps (the flare/quiet rate
    ratio is 100 in both cases).
- **A fixed FSW** raises every level by 1/0.475 = 2.1× (or 2.8× from
  0.363). At the default 2000 cps flare that may push the rate control
  above state 0. The gate's "every SSID 30 RCR byte 0" would then fail
  even though the model is right. Re-calibrate `det-flare-cps` (or
  allow RCR > 0 in the flare window) when #1069 is fixed.

**Gate record (D-4, 2026-11-11).** Each run in a fresh
`/tmp/stix-d4-*` dir under the 21000 flock, using sparse flash copies.
Shared flash mtimes unchanged (uprev 2026-11-10 13:05:54, baseline
2026-11-09 15:30:10). RED on the D-3 binary: all 7 new qtests failed
(missing props, scene NULL, flare 0 vs λ 2560). GREEN: all 7 ok.
Final lines:
- suite (qt.sh all): 61 of 61 ok (54 + 7 new), exit 0
- probe / ql-check / idefx-trace-check self-tests: `SELF-TEST: PASS`
- boot-smoke: `BOOT SMOKE: PASS (normalized UART matches golden)`
- tc-smoke: `TC-SMOKE: PASS`
- nominal-smoke ±: `NOMINAL-SMOKE: PASS`, `NOMINAL-SMOKE (negative): PASS`
- fault-smoke ±: `FAULT-SMOKE: PASS`, `FAULT-SMOKE (negative): PASS`
- detector-smoke ±: `DETECTOR-SMOKE: PASS` (zero flush unchanged), `DETECTOR-SMOKE (negative): PASS`
- flare ±: `DETECTOR-SMOKE (flare positive): PASS`, `DETECTOR-SMOKE (flare negative): PASS`
- checkpatch: 0/0 on the diff (only the missing Signed-off-by) and
  with `-f` on `stix_det.c` and `stix_det.h`

Flare positive TM:
- switch: on at T_nom+58 s, off at T_nom+115 s
- SSID 30 band 0: 0x1f quiet (13 samples), 0x70 flare (11 samples);
  RCR stays 0
- SSID 34: flagged in 14 of 14 flare periods (status 162, 194); 128
  when quiet
- SSID 33 variance: about 25 quiet, about 57 flare
- SSID 32: mean about 321 per `subcol`; ebin 18, 19 and 27 dominate;
  ebin 0 and 31 are 0

QEMU CPU, user + sys: zero sky 5.2 + 7.9 s; flare 5.7 + 7.4 s.

The first flare-negative run failed `trace-verif`. The cause was a
script bug: `--negative` also selected the D-3 det-idefx-off
`--expect-none` branch. Fixed by adding `IDEFX_NEG`; one re-run then
passed.

**Review fixes (D-4 review, 2026-11-11).** No blockers. Changes:
- the R3 finding is now the confirmed FSW bug #1069 (above);
- the nominal-smoke wording is corrected;
- the FSW tree is named;
- the `--flare` calibration note, the `det-seed` exception and the
  mid-period switch are added;
- new qtest `det-counts-r3-fsw`. It is a characterization test,
  mutation-checked: dropping the wire field reversal in its own
  builder fails it;
- the cps properties take a per-property opaque (det and field
  offsets) instead of a `strcmp` on the name.

Re-run, each under the 21000 flock in fresh `/tmp/stix-d4r-*` dirs:
- self-tests (probe, ql-check, idefx-trace-check): PASS
- `DETECTOR-SMOKE: PASS`
- `DETECTOR-SMOKE (negative): PASS`
- `DETECTOR-SMOKE (flare positive): PASS`
- `DETECTOR-SMOKE (flare negative): PASS`
- `NOMINAL-SMOKE: PASS`
- checkpatch: 0/0 apart from the missing Signed-off-by
- suite (qt.sh all): 62 of 62 ok, exit 0

## 41. Gate ports: slots for parallel gate runs (baseline: one global lock)

Plan: `2026-11-11-gateports-plan.md`. Four tracks (tc, platform, elut,
att) can run their gates at the same time without the global 21000 lock.

- **Slots** (`scripts/stix/lib/slot.sh`). A slot is one lock file, one
  port band and one run-dir suffix; holding the lock owns all three.
  - Slot k: ports `B = 20000+uid+100k` .. `B+99`, lock
    `/tmp/stix-qtest-<B>.lock`. B+0 is the TMTC port, B+1/B+2 are
    reserved for UDP TM/TC, B+3..B+99 are the qtest suite's.
  - Slot 0 (21000) is legacy, manual and 5.2-wrapper use only. `auto`
    takes slots 1..`STIX_SLOTS` (default 3) and refuses a count whose
    bands reach the GS ports (21500-21502; N <= 4 at uid 1000). The CPU
    cap is N auto runs + 1 legacy run.
  - In slot mode a gate passes `-global grlib-stixspw.tmtc-port=B`, gives
    the probe `--port B`, runs in `<run dir>-s<k>` and appends `slot=k
    busy=<other held slots> load=<1 min loadavg>` to its verdict lines.
    `busy` is read from `/proc/locks` without taking any lock.
  - The helper never retries a gate (D-G6). A FAIL with busy > 0 is
    rerun alone, by hand; if it then passes, it is a load flake: record it
    here and lower `STIX_SLOTS`.
  - **New gate scripts copy the pattern**: `. lib/slot.sh;
    stix_slot_setup; RUN_DIR="$(stix_slot_dir "$RUN_DIR")"` at the
    `TMTC_PORT=` line, `"${STIX_SLOT_QEMU_ARGS[@]}"` in each QEMU
    invocation, `${STIX_SLOT_TAG}` on each verdict line, and
    `stix_flash_private` after `cd "$RUN_DIR"` for a gate that boots the
    shared master.
- **Legacy mode is byte-identical** on the QEMU command line, the probe
  argv, the verdict lines and capture-golden's recorded flags
  (`scripts/stix/tests/test-gate-slots.sh` compares them with the
  pre-change scripts). The one addition is the disk guard: every gate,
  legacy included, exits 2 before QEMU when `df /mnt/c` shows less than
  `STIX_DISK_MIN_GB` (3) GB free (the WSL VHDX lives on C: and never
  shrinks). A `/mnt/c` that exists but is not a mountpoint is an error,
  not a reason to measure `/`. Paths that boot nothing (`--dry-run`,
  `--check-guard`, the ELF-absent SKIP) only peek at the slot: no lock,
  no wait, no disk guard.
- **The master flash is not copied in legacy mode.** Measured: SAFE boots
  on the already-booted master issue no page write or erase (mtime fixed
  since 2026-11-10 13:05:54, 32 blocks). Slot mode boots private sparse
  copies (`cp --sparse=always`, refreshed per run, under a shared flock
  on `.flash.lock`; refused when the master is missing, the copy
  allocates >= 64 MB or its size differs) as insurance against future
  writers. capture-golden.sh refuses slot mode and holds `.flash.lock`
  exclusively. On PASS a slot-mode gate deletes its flash copy (and
  nominal-smoke its trace log); FAIL/ERROR keep everything.
- An explicit `STIX_RUN_DIR` / `STIX_NOMINAL_RUN_DIR` /
  `STIX_FAULT_RUN_DIR` also gets the `-s<k>` suffix in slot mode, so two
  auto runs with the same setting cannot share a dir. The nominal/fault
  run-dir guard runs on the suffixed path.
- `tmtc-hermetic` holds an ephemeral port (`tmtc-port=<held>`): the
  `tmtc-port=0` default path is no longer exercised by it. On the 5.2
  baseline it SKIPs explicitly (no `tmtc-port` property), and only there:
  on any other binary a missing property fails it.
- Suite QEMU ports come from the slot band B+3..B+99 when a slot is held
  (`STIX_QTEST_PORT_BASE` = B, exported by `lib/slot.sh`; a set but
  unusable value fails the test). `STIX_PORT_BASE` stays the selector. Without a
  slot, the old ephemeral release-then-bind window remains.
- `scripts/stix/qt.sh --baseline` runs the 5.2 wrapper in slot 0 from
  `/tmp/stix-qt-baseline` with a sparse copy of the master: the wrapper
  strips `flash-file`, and the 5.2 model writes a full 16.5 GB flash.bin
  into a CWD that has none. It is for `tmtc-hermetic` and other tests
  that do not read flash content: the master is stored inverted (D-I),
  which the 5.2 model reads as all-0x00 flash, so flash-content tests
  fail there spuriously. By default it runs only
  `STIX_QT_BASELINE_TESTS` (`tmtc-hermetic`); `--full` runs the whole
  suite anyway.
- **detector-smoke keeps every mode's evidence.** Each mode has its own
  run dir: `<dir>` (positive), `<dir>-neg`, `<dir>-flare`,
  `<dir>-flareneg`, then `-s<k>` in slot mode (for example
  `/tmp/stix-detector-run-flare-s2`). A run writes `.unfinished` and removes it only on PASS; the
  next run of that mode moves an unfinished dir aside to
  `<dir>.fail-<date>-<pid>` instead of reusing it (a D-4 gate run lost a
  --negative `ql` FAIL that way: the next mode overwrote it). A failed
  `ql` / `ql-contrast` check names its first ql-check mismatch (and how
  many more) in the CHECK line and in the verdict. No ql tolerance
  changed.
- **Gate timing is monotonic.** The WSL wall clock is stepped back to
  the Windows time by systemd-timesyncd, mid-run (measured wall /
  monotonic 0.75-0.88 over 10-20 s, then 1.000 a minute later). On
  2026-11-11 three parallel detector-smoke runs FAILED `ql` that way:
  the window from the probe's wall HH:MM:SS read 130 s while the guest
  ran about 141 s (35 SSID 30 samples, correct for 141 s). Now:
  - tmtc-probe.py appends `[m=<CLOCK_MONOTONIC s>]` to every event line
    (chunks, TM, EXPECT OK, sends, QMP); its own `--send-at` / `@N`
    windows were monotonic already.
  - detector-smoke takes T_nom, T_exit and the flare switch times from
    those stamps; ql-check only receives the resulting seconds.
  - idefx-trace-check needs no clock at all. The trace is wall-stamped
    by QEMU, so a first version placed the monotonic window on the wall
    clock and reported INCONCLUSIVE (exit 3) beyond 0.5 % drift; on this
    host every 240 s run spans about seven 2.5 s steps (timesyncd polls
    every 32 s, offset -1.6 s), so that was INCONCLUSIVE every time. Now
    QEMU also traces `stixdet_fsm_strobe` into the same file
    (`-trace events=<run dir>/idefx.events,file=...`), and NOMINAL is the
    span from the strobe that sets the FSM Enable to the one that clears
    it, by line order. Measured: SetEnable (0x1) precedes the first
    verification run, only QlClear strobes (Enable 1) come between runs,
    ClearEnable (0x2) follows the last. A run ends at a strobe or a > 5 s
    gap (relative, so a backward step inside a 0.05 s run joins).
  - detector-smoke prints the run's wall/monotonic ratio for the record;
    no verdict depends on it.
  - The other gates were checked: nominal-, fault- and tc-smoke time
    everything inside the probe (monotonic) or with `timeout`/`sleep`;
    soak slices on `/proc/uptime`; boot-smoke --with-tm only uses
    `timeout`. None reads a wall-clock time.
  - Wall-clock steps can no longer change a gate verdict, so a drifting
    clock is no reason to hold a gate round.
- **Fresh-flash gates pre-create flash.bin.** nominal-, fault- and
  detector-smoke delete flash.bin per run and then
  `truncate -s 17716740096 flash.bin`, in legacy and slot mode, so no
  QEMU ever creates the file itself (an older build fills a missing
  flash.bin densely: 16.5 GB on C:). An empty sparse file of full size
  is what the current device creates anyway (inverted storage: a hole
  reads as erased 0xFF).
- Leaked `/tmp/stix-flash-*` qtest files multiply with parallel suites
  (small, out of scope).
