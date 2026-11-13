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

**Model (R3; RELOAD amended 2026-11-12, see the resolution below):** a
command register. FSW `file:line` below is STIX-FSW 600a7a72 as in
§34: `Watchdog.c` is `ASW/ASW/src/HWControl/Watchdog.c`, `WDT.c` is
`ASW/BSW_ASW/src/BSW/WDT/WDT.c`.
- Disabled at power-up and after every system reset: COUNT = RELOADVAL
  = 0x3F (63), the counter does not run.
- RELOAD ("feed") loads COUNT from RELOADVAL and restarts the 105 ms
  cycle from now. It never changes enabled/disabled: a disabled
  watchdog just holds the reloaded value.
- ENABLE resumes counting from the current COUNT, DISABLE freezes it at
  the current COUNT (both idempotent). CHANGE sets RELOADVAL only.
  FORCE/FORCE1/FORCE2 reset the IDPU at once. SAFE does nothing.
- While counting, COUNT drops by one every 105 ms
  (`WATCHDOG_CYCLE_DURATION`, `Watchdog.c:12`) on `QEMU_CLOCK_VIRTUAL`,
  counted lazily. At 0 the model requests a guest reset (~6.6 s after
  the last feed at the FSW's 63 cycles). With `-no-reboot`, QEMU then
  exits.
- A read returns RELOADVAL and the live COUNT. The hw-state watchdog
  section is `running`/`enabled`/`count`/`reload` (schema 3; `enabled`
  is the ENABLE/DISABLE command state, added 2026-11-12).
- Trace events: `stixctrl_watchdog_cmd`, `stixctrl_watchdog_reset`.

**Resolution (was the open point, 2026-11-12):** RELOAD no longer
starts a disabled watchdog. The old "RELOAD also enables counting"
reading came from the BSW `WDT.c:21` header note, which was written for
FPGA build 248 (`WDT.c:13`), whose command list has no ENABLE — build
252 added CHANGE (`WDT.c:29-34`) and the ENABLE/DISABLE emitter
`WDT_setState` (`WDT.c:297-309`). The flight driver enables explicitly:
`Watchdog_setRefreshInterval` (`Watchdog.c:122-136`) and
`Watchdog_enable` (`Watchdog.c:143-151`) both write ENABLE, then DISABLE
on a `-DDISABLE_WATCHDOG` build, then feed (`WDT_feedWdg`,
`WDT.c:312-315` = RELOAD). The AX gate ELFs are
`-DDISABLE_WATCHDOG` builds (the build task adds it, `tasks.vs.json:14`)
whose Watchdog task still feeds every second (`Watchdog_feed`,
`Watchdog.c:165-193`); on hardware their watchdog therefore never runs,
and the old model ran it. This closes the flash-latency plan's
"watchdog DISABLE stickiness" follow-up (Q5) and the Phase 1 hand-back
item "Watchdog RELOAD while DISABLEd".

**Gate effect:** the AX gates no longer have a live watchdog; a stall
on an AX ELF no longer resets the IDPU (the soak's TM-flow gate is the
only liveness verdict there, and `edac-smoke.sh` is unaffected — its
reset is the trap handler's FORCE, `WDT.c:182`, not the counter). The
PFM gates are unchanged: both PFM ELFs enable the watchdog (their init
ends ENABLE, RELOAD), so the negative mode still expires at ~6.6 s and
the positive still feeds once per second. The golden is unaffected: the
per-feed COUNT check (`Watchdog.c:197`) reads 63 right after the feed
in both the old and the new model, the cycles check at
`Watchdog.c:180-190` is compiled out of `-DDISABLE_WATCHDOG` builds,
and the boot UART is silent after init either way.

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

**Evidence:** qtest `/sparc/stix/ctrl-watchdog` (rewritten for the
amended semantics): power-up disabled with COUNT = RELOADVAL = 63 and no
reset after 100 cycles; the `DISABLE_WATCHDOG` init sequence (CHANGE,
RELOAD, DISABLE, ENABLE, DISABLE, RELOAD — `Watchdog.c:54-70`,
`:122-136`, `:143-151`) leaves it frozen at 63, never resets with 1 s
feeds and never resets after 100 unfed cycles; RELOAD while disabled
does not start counting (COUNT stays 63 after 20 cycles); ENABLE+RELOAD
counts one per 105 ms; a 1 s feed cadence never expires; no feed resets
once the count runs out (~6.6 s); DISABLE freezes and ENABLE resumes
(both idempotent); a system reset returns to disabled with COUNT 63;
FORCE resets at once. It is expected red on the 5.2 wrapper.

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

## 35. Nominal integration and ROTB science chunks (baseline: no integration, empty ring)

**Baseline:** the nominal side of the detector model did nothing. The
QL switch and the synthetic sky (§31, §34) filled LACC/LTRG and SUMCNT,
but `fsmIntTime` and `fsmIntEvents` were plain storage,
`fsmSta.ActiveNominal` read 0, the EACC/ETRG banks were never written,
and only PUTBLOCK in TESTMODE filled the ROTB ring (§12). No detector
chunk ever reached the archive, so the FSW's science path downstream of
the ROTB was untested.

Detector plan D-5 (`docs/stix-port/2026-11-11-detector-plan.md`,
"### D-5", decision D-Dg; the "As built" note there).

**FSW tree:** every FSW `file:line` in this section is STIX-FSW
600a7a72, as in §34 (`Detectors.c` is
`BSW_ASW/src/BSW/Detectors/Detectors.c`).

**The integration and the swap (D-Dg).** The nominal integration runs
on the same 100 ms tick as the QL sky, so only while FSM Enable = 1.
It ends at the first tick t (ticks since its start) with
`((t >= IntTimeMax + 1) || (SUMCNT >= IntEventsMax)) &&
t >= IntTimeMin + 1` [D, TN Table 5-8; RP-0067]. The parameters are
latched at the start of each integration, from `fsmIntTime` (min 15:0 /
max 31:16, in ticks − 1) and `fsmIntEvents` (`Detectors.c:1140-1149`),
and re-latch at every swap and at the Reset strobe. The FSW programs
9 / 199 / 1800 (1 s min, 20 s max, 1800 events) at NOMINAL entry
(`OpModeMgmt.c:625`, `ParamMgmtDef.h:382-384`); the register reset
values are 0/0/0, with which every tick would swap — the reason the
qtest helpers now program the FSW's parameters. At the swap
(`stix_det_nominal_swap`):
1. the finished bank's EACC is materialized from the integration's own
   exposure (`nom_expo`): the same Poisson draw per cell as the QL
   LACC (`stix_det_draw_acc`, factored out of §34's materializer, the
   same `[pix][fpga_det][ebin]` u16 big-endian layout), but it only
   accumulates the ticks in which the `fpga_det` was in SUMDMASK (EACC
   is SUMDMASK-gated [D, RP-0067]; QLACC is not);
2. ETRG per `grp` (`grp` = `fpga_det`/2, u32 big-endian) = 1.25 × (the
   drawn EACC events of its two `fpga_det` + one Poisson draw of the
   expected events of the read-out `fpga_det` **outside** SUMDMASK),
   rounded, clamped at 24 bits: **the triggers are not masked by
   SUMDMASK** [I];
3. `ActiveNominal` (fsmSta bit 4) toggles, SUMCNT restarts at 0, the
   parameters re-latch;
4. with `det-rotb` on, the bank goes into the ROTB ring as one chunk
   (below);
5. the finished bank's EACC and ETRG are zeroed after every swap, also
   when no chunk was taken (read-and-clear [D]), so every integration
   starts from zero.

A fresh enable (SetEnable at tick 0) also latches the parameters and
zeroes the active EACC/ETRG bank (no stale RAM: the banks survive a
reset, §13). A device reset restarts the integration in bank 0 and
re-latches; `ActiveNominal` and the banks stay. The banks: EACC0/1 at
0xE01A0000 + n·0x8000 (0x6000 used per bank) in the SRAM region,
ETRG0/1 at 0xE0600000 + n·0x40 in their own region. The FSW never
reads the banks in NOMINAL; it consumes the chunk
(`RotAccumulator.c:64-140`). `stix_det_realize` now takes the owning
`STIXCTRL` and the ETRG region; `stix_det.h` declares the two new
controller helpers (`stixctrl_scet_reg`, `stixctrl_rotb_append`).

**The chunk layout.** One chunk = 49 blocks of 512 B (block numbers
0..48; `RotAccumulator.h`'s `LAST_DETECTOR_BLOCK_NUMBER` 48), built
into a scratch buffer and handed over whole; `RotAccuCompactor.c:76-270`
is the FSW's builder. Block n (0..48): u16 big-endian descriptor
`0x8000 | n` (type 0x8000 = detector data, `RotAccumulator.h:16-22`;
block number 5:0), u32 big-endian SCET coarse at +2 and u16 fine at +6
— exactly what the 0xC8/0xCC reads compose at the swap, with the
UNSYNCED status flag in bit 31 of the coarse (`stixctrl_scet_reg`;
`ArchiveManager.c:131` masks it) — then 504 data bytes from +8 in
every block. The stream is the EACC bank first (0x6000 B = 12 `pix` ×
32 `fpga_det` × 32 `ebin` u16), then ETRG (0x40 B = one u32 per `grp`,
on the wire hw/lw u16 pairs, `RotAccuCompactor.c:174-245`), then a
56 B zero pad in block 48 (24640 + 56 = 49 × 504). Everything
big-endian, as the banks are. Layout trap for a later TM gate (plan
D-5): the chunk carries `[pix][fpga_det][ebin]`; the compactor
converts `fpga_det` → `subcol` (`RotAccuCompactor.c:298-300`).

**The append and `det-rotb`.** `stixctrl_rotb_append`
(`grlib_stixctrl.c`) is called once per swap from timer context (BQL
held) and shares the ring push with PUTBLOCK (factored into
`stixctrl_rotb_push`). It appends only with rotbConfig ENABLE = 1
(bit 31) and TESTMODE = 0 (bit 13; in TESTMODE the window and PUTBLOCK
own the ring, §12), and only whole: with fewer free blocks than 49 the
chunk is dropped and IRQ 7 (`IRQ_STIXCTRL_ROTB`, IRQMP line 7) pulses
once. The docs say the hardware overwrites the oldest block; dropping
keeps §12's drop-new rule and never leaves a partial chunk (plan Doc
vs code 7). Reaching capacity exactly still sets FULL, as for
PUTBLOCK. The qdev property `det-rotb` (default on) keeps the swaps
and makes no chunk when off, for the gate negative.

**Re-entrancy (D-Dj).** The tick writes guest RAM outside any MMIO
dispatch (the banks through `memory_region_get_ram_ptr`, the ring
through the controller's host buffer), so it now engages the
controller's `mem_reentrancy_guard` for its whole body and restores it
afterwards, as the SpW DMA paths do (the a83fe9a3a5 rule); should a
future tick path reach the controller's own MMIO, the access is
refused ("Blocked re-entrant IO"). The QMP setters still only store
fields.

**Traces.** Three new events, every line stamped with the virtual
clock (`vclock_ns`): `stixdet_nominal_swap` (done and active
`nom_bank`, `int_ticks`, `sumcnt`, `eacc_events`), `stixdet_chunk`
(49 blocks, SCET `coarse_reg:fine`, `int_ticks`, `eacc_events`,
`rotb_count`) and `stixdet_chunk_drop` (`rotb-disabled` |
`rotb-testmode` | `ring-full` | `det-rotb-off`).

**qtests** (`tests/qtest/stix.c`; all six RED on the D-4 binary):
- `det-chunk-format`: a 5 s integration (max time 50 ticks) gives one
  49-block chunk, decoded the way `RotAccumulator`/`RotAccuCompactor`
  decode it: descriptors, whole block numbers 0..48
  (`RotAccuCompactor.c:88-96`), every block's SCET equal to the
  0xC8/0xCC read now, the counts from +8 of every block in
  rot-counter order (`pix << 10 | fpga_det << 5 | ebin`), ETRG as
  hw/lw pairs, the pad zero. Every ETRG word is exactly 1.25 ×
  (rounded) the decoded EACC events of its `grp` (no SUMDMASK bit
  clear); the `pix` the R3 switched off (`fpga_det` 18, `asic_ch` 1 =
  `pix` 3) is zero at its own offsets only; nothing lands in `ebin`
  0/31; the total matches the rate; the EACC/ETRG banks read zero
  afterwards. The next integration uses bank 1 and makes the next
  chunk 5 s later, with a later SCET.
- `det-nominal-rule`: the quiet sky ends at the max time (200 ticks;
  SUMCNT stays below 1800), the flare (about 4600 events per tick) at
  the min time (10 ticks); a new min or max applies from the next
  integration on (latched); a Reset strobe restarts the count and
  re-latches; an event limit of 100 with no min time ends on the
  events (8..30 ticks), with SUMCNT zeroed after.
- `det-chunk-gating`: in TESTMODE and with ENABLE = 0 the swap still
  happens (`ActiveNominal` toggles) and the ring stays as it is; with
  the FSM disabled nothing swaps; `det-rotb=off` swaps without
  chunks.
- `det-chunk-full`: with `rotb-blocks=60` the first chunk fits; the
  second finds 11 free blocks, is dropped whole with one IRQ 7 pulse,
  and BLOCKCOUNT stays 49 with FULL still 0; every further dropped
  chunk pulses again; draining returns the first chunk intact and the
  next chunk fits again.
- `det-chunk-sumdmask`: with `fpga_det` 5 masked, its chunk counts are
  0 while its QL LACC counts are not (QLACC is not masked); ETRG
  `grp` 2 (`fpga_det` 4 and 5) is not masked — it exceeds 1.25 ×
  `fpga_det` 4's EACC by about 1.25 × 5000, the unmasked `fpga_det` 5's
  expected events — while every other `grp` is exactly 1.25 × its EACC
  events.
- `det-chunk-guard`: after timer-driven swaps and chunks every CTRL
  MMIO path still answers (the detector registers, the composed
  `rotbCmdSta`, GETBLOCK and the block window, and IRQ 3 still
  pulses); a guard left engaged would refuse all of them. Per the
  plan's as-built note: the release half is mutation-checked; the
  engage half is not observable from a qtest.
- helpers: `nom_set_params`, `det_start_nom` and `sky_start_nom`
  program the nominal parameters, and the older `det_start` /
  `sky_start` now default to the FSW's 9/199/1800, so the D-1..D-4
  tests keep their behaviour (with zeros every tick would swap and
  reset SUMCNT); the gap sentinels move from "past LACC3" — that is
  EACC0, 0xE01A0000, which D-5 now writes — to the unused 0x2000 of
  each EACC stride.

**Gates.**
- `detector-smoke.sh` runs the D-5 checks in every mode (positive,
  `--negative`, `--flare`, `--flare --negative` and the new
  `--no-rotb`); the timeline is unchanged. QEMU also traces
  `stixdet_scene`, `stixdet_nominal_swap`, `stixdet_chunk`,
  `stixdet_chunk_drop` and `stixctrl_rotb_cmd` into the same trace
  file, and `chunk-trace-check.py` (new, stdlib only) checks it — all
  on the virtual clock and tick counts the model writes into the
  lines, never the wall `<time>` stamps (§41):
  - check `chunk`: every nominal swap made a 49-block chunk and none
    was dropped; every quiet integration took exactly 200 ticks (the
    FSW's 20 s max; SUMCNT stays below 1800), at least 5 of them (1
    with `--flare`); consecutive chunks are the later one's
    `int_ticks` apart within a tenth of a tick (the D-1 tick timer
    re-arms from its late callback, so a tick is 100 ms + the
    callback's lateness — the plan's as-built note measured 200 ticks
    in 20.000141 s live; a lost or extra tick is 100 ms off and still
    fails); and GETBLOCK ≥ 49 × (chunks − 1), i.e. the FSW drained
    the ring (the newest chunk stays).
  - check `chunk-flare` (`--flare` only): at least 40 integrations
    inside the flare windows (from the `stixdet_scene` lines), each
    exactly 10 ticks (the min time; the 1800 events come within a
    tick at 2000 cps). A `--flare` run uses `--min-quiet 1`.
  - check `uart-chunk` (every mode): "Skipped first Det data chunk"
    (`ArchiveManager.c:140`; the archive drops the first chunk, later
    ones are archived silently) exactly once, and no `*E` line from
    the chunk path, the archive or the filesystem on the fresh flash
    (the FSW TRACE_TAGs Archive, RotAccuComp, RotAccumulator, FS,
    FSapi, MemStream, MemCommon, FIFO). This resolves the plan's [L]
    item. The tag list is split over two assignments for checkpatch
    (f88b160d55).
  - the run prints the flash.bin KiB allocated, for the D-5 NAND cost
    record (plan D-5 Performance).
- `--no-rotb` (new): the zero sky plus
  `-global grlib-stixctrl.det-rotb=off` (run dir suffix `-norotb`);
  refused together with `--flare` or `--negative` (exit 2). It passes
  only when exactly the two chunk checks fail as planned: `uart-chunk`
  (no "Skipped first Det data chunk") and `chunk`
  (`chunk-trace-check.py --expect-none`: at least 3 nominal swaps, no
  chunk at all, drop reasons only `det-rotb-off`).
- `--flare --negative` must now fail exactly `ql-contrast` **and**
  `chunk-flare` (no flip: no LC contrast, no 1 s integrations).
- `chunk-trace-check.py --self-test` builds synthetic traces: the
  positive rules and each of their failures (a short quiet
  integration, too few chunks, a dropped chunk, a 48-block chunk, an
  undrained ring, a swap without a chunk line, a cadence break), the
  late-timer tolerance (141 µs over 200 ticks passes), the flare
  classification (quiet/flare/mixed across a switch), the flare
  negative, and `--expect-none`.
- nominal-smoke needs no change: "Skipped first Det data chunk" now
  appears there too and was never in its forbids (plan Gate impact).
- `test-gate-slots.sh` covers the `-norotb` run dir in slot mode, the
  refused flag combinations and the `-global` flags in the dry run.

**Deviations from the plan's D-5 section** (the two its As-built note
marks):
- the gate negative for the chunk checks is the new `--no-rotb` mode
  (the plan had only the flare negative), and the flare negative's
  expected failure set grows from `ql-contrast` to
  `ql-contrast chunk-flare`;
- the trace golden ("qemu-stdout.golden.log unchanged, checked") is
  checked from the detector-smoke trace (no `stixdet_nominal_swap`
  before the first SetEnable strobe), not by a slot-mode
  `capture-golden.sh` run, which needs the shared run dir.

**Gate record** (2026-11-12, `stix-uprev-det5` rebased on 451d4d399e, slot 2):
- suite (qt.sh all): 68 of 68 ok, including `det-chunk-guard`;
- boot-smoke PASS (golden unchanged); tc-smoke PASS;
- nominal-smoke PASS, and `--negative` PASS;
- fault-smoke PASS, and `--negative` PASS;
- detector-smoke: positive PASS; `--negative` PASS (failed as planned); `--flare` PASS; `--flare --negative` PASS (failed as planned); `--no-rotb` PASS (failed exactly `uart-chunk chunk`).

## 40. Flight ELUT load (baseline: none)

Plan: `2026-11-11-elut-plan.md` (E-1..E-3). Scripts and docs only: no
model change, no golden change. The ELUT section of `ground-station.md`
has the index-space table, the recipe and the template flash.

- **The tool** (`scripts/stix/elut-upload.py`, stdlib only) turns the
  flight CSV into 96 TC(237,7) (4 rows each, 226 B units, length field
  0x00D7) plus TC(237,8) (`03 | lut_id`), with CRC-16/CCITT-FALSE, and
  checks a pmemsave dump of the FPGA ELUT (`val[pix][fpga_det][adc]`,
  1 572 864 B at 0xE0000000). `check-dump` replays applyELUT literally
  through `subcol = utils_subcolIdFromFpgaId[fpga_det]`, runs an
  independent oracle from the CSV edges, and first proves its own
  sensitivity on the CSV (identity table 348 differing rows,
  wrong-direction table 276, exactly as predicted; else exit 2).
- **B0 = 0, B32 = 0** (user decision 2026-11-11): B[1..31] are the CSV
  edges, so ebin 0 covers `adc` 0..e[0]−1 and ebin 31 covers
  e[30]..4095; nothing is unassigned. The ops value of the lowest
  boundary is unknown here.
- **The gate** (`scripts/stix/elut-smoke.sh`, optional, SKIP 77 without
  `STIX_ELUT_CSV` or the ELF) boots twice on one fresh private flash
  (an empty sparse `flash.bin`, as §41's fresh-flash gates):
  - run 1 (235 s): connect 0x8030/0x0022/0x14A3; CONF at 35, S20 = 0
    at 40, dump r1-pre at 45; 96 × TC(237,7) at 50 + 0.75 k, each
    TM(1,1)@5 and TM(1,7)@5; TC(237,8) LUT ID 1 at 127, TM(1,7)@38;
    dump r1-apply at 170; NOMINAL at 175 (`0021 04 05`@45, TM(1,7)@45,
    `542c01`@45; forbid `542c03`, `0021 05 02`, 0x142E, 0x9449); dump
    r1-nominal at 225. TM(1,2)/TM(1,8) forbidden for the whole run;
  - run 2 (110 s), a new QEMU process on the same flash, no TC(237,7):
    0x8030 forbidden; CONF, S20, dump r2-pre at 45, NOMINAL at 50,
    dump r2-nominal at 100;
  - dumps: r1-pre and r2-pre constant 0, the other three PASS. UART run
    1: `could not find LUT (type=3, id=0` present, 4 × `problem erasing
    LUT type=3` (the first store), no write/read/apply/context-store
    errors. UART run 2: `could not find LUT (type=1` present, **no**
    `could not find LUT (type=3` and no `problem reading LUT (type=3`.
  - **The persistence proof is run 2**, not r1-nominal: a failed flash
    load leaves the RAM table untouched (LUTMgmt.c:1140), so r1-nominal
    would pass even then. In a new process RAM `ELUT[]` is zero and only
    `load_lut` can fill it.
- **The first live attempt used a sparse copy of the master flash**
  (`/tmp/stix-uprev-run/flash.bin`) and failed at +50 s: no 0x8030. The
  master holds a context. The gate now pre-creates an empty sparse
  flash; a context-free start is what makes run 2's forbidden 0x8030
  prove the context run 1 stored.
- **0x542C:** p1 = 1 (T1LUT, no temperature LUT) came once at every
  NOMINAL entry of both runs and stays; loading the ELUT does not remove
  it. The ELUT apply error would be p1 = 3 (`542c03`), forbidden; `542c01`
  is now expected as its positive control.
- **Run 2 boot:** no 0x142E (the boot TC20 timer) was seen; the context
  carries S20 = 0. Neither expected nor forbidden at connect.
- **E-4 addendum (implemented, pending gates):** the positive run 2 also
  proves the no-fallback side on the real ELUT. The gate writes a tiny
  synthetic `.dssevs` into its own run dir (never a user file: one 0xE
  setup word with baseline 2000 for `dss_det` 0, then 4 events at
  `adc` 2000, `fpga_det` 0, `pix` 0, wait 0), starts run 2's QEMU with
  `-global grlib-stixctrl.det-replay-file=` (realize-time) and the
  `stixdet_elut_fallback` / `stixdet_replay_scan` / `stixdet_replay_state`
  traces, sets `det-source=replay-dss` by QMP at @5, and ends NOMINAL
  with TC(236,1) at 115 so the exit flush sends the one SSID 32 dump.
  `elut-replay-check.py` then asserts the pre-scan line, a
  `state=start (nominal)` line (the replay started at the SetEnable
  edge where stix_det.c classifies the 384 rows — D-7 has no
  per-classification summary trace, so this plus the SSID 32 check
  carry the vacuity guard), zero `ELUT fallback` lines, and the counts
  at exactly the CSV's ebin of `adc` 2000 in exactly the one subcol:
  at `adc ≤ base` the linear fallback would have dropped the events,
  so only the honoured branch can pass. `test-elut-smoke.sh` covers
  the vacuous and fallback-line FAILs and the full fake PASS;
  `test-elut-upload.sh` pins the Python index tables against
  `stix_det.c`'s transcriptions. The replay runs inside the gate's own
  run dir only; the DSS replay caveat below (a seeded flash, and
  detector-smoke deleting its flash at every start) still applies.
- **FSW behaviour, confirmed live by `--negative`:** without TC(237,8)
  the upload is not stored, but a NOMINAL entry on a flash with no ELUT
  file for the current ID applies it anyway: `load_lut(3, 0)` misses and
  keeps RAM, then applyELUT writes it (r1-nominal PASS, r1-apply still
  0). After a reboot the RAM table is zero and the NOMINAL entry writes
  all 31 (r2-nominal constant 31); `could not find LUT (type=3, id=0`
  is logged. With a file for the current ID the reload would overwrite
  an unapplied upload instead.
- **FSW observation, not modelled:** `fill_table` and `fill_buffer`
  walk 16 896 packed entries over the 25 344-B ELUT table
  (LUTMgmt.c:635-652 and :722-745 at STIX-FSW 600a7a72), i.e. 8 448 B past
  `ELUT[]` into `Tmno` (the sky LUT, next in the baseline ELF). Harmless
  here: no sky LUT, `Tmno` all zero; flight behaves the same.
- **Apply time:** in QEMU, with the flash latency off, TC(237,8) to its
  TM(1,7) took 0.05 s (apply, 4 flash copies, ID set); the NOMINAL entry
  including the ELUT re-apply 3.35 s (monotonic, from the probe's
  `[m=]` stamps). The ICD's flight figure is about 2 minutes; QEMU's
  SRAM and flash are much faster than the flight FPGA path, so no
  timing conclusion carries over.
  - [L] re-run elut-smoke once flash-latency F-3 makes latency the
    default. **Done 2026-11-12** on the integration tip ad5472dfb1
    (latency on by default), slot 4: elut-smoke PASS and `--negative`
    PASS.
- **SpW RX:** the 226-B units are the largest TCs ever sent; the
  `stixspw_rx_truncated` trace stayed silent (QEMU runs with it on).
- **Disk:** the seeded flash allocates 148 KiB (apparent 16.5 GiB); the
  `--save-template` copy 144 KiB. Slot mode deletes the dumps, flash and
  QEMU logs on PASS.
- **Caveats:** a seeded flash or template has a context (no 0x8030 at
  boot), so it must not be used for gates that expect one; on the
  flight ELUT a DSS replay (baseline about `adc` 2000) lands almost
  entirely in ebin 30/31 (plan "DSS replay caveat"). No gate proves
  that the CSV column `Detector` is `subcol` (0-based); the GSW reader
  `stx_read_elut.pro`, stixpy and the ICD corroborate it, and the user
  believes so (not certain). If it were `fpga_det`, every gate would
  still pass.

**Gate record** (2026-11-11, worktree `stix-uprev-elut` on f9853b57d0,
normal ELF, slot 3, `elut_table_20260529.csv` sha256 919d9055…21ccf2):
- positive with `--save-template`: PASS (`slot=3 busy=0`);
- `--negative`: PASS (`slot=3 busy=0`);
- positive on the E-2 commit (2e9a588e8e): PASS (`slot=3 busy=0`; apply 0.06 s, NOMINAL entry 3.38 s);
- host tests: `test-elut-upload.sh` 27/27 (with `STIX_FSW_SRC`),
  `test-elut-smoke.sh` 44/44, the tool's self-test and the probe
  self-test PASS.

## 36. det-inject: the index-space end-to-end deposit (baseline: no way to place a count)

**Baseline:** the only count source was the synthetic sky (§34), whose
counts are template-shaped and uniform; there was no way to place
counts at a chosen (`pix`, `fpga_det`, `ebin`) cell, and the one
non-1:1 step of the detector chain — `fpga_det` ↔ `subcol`
("Index spaces"; the ICD never mentions `fpga_det`) — was pinned only
by the model's own conversion tables and self-check (§32), never
end to end through the real FSW and its TM.

Detector plan D-6 (`docs/stix-port/2026-11-11-detector-plan.md`,
"### D-6", decisions D-Db and D-Df; "Index spaces", "End-to-end
check"; the "As built" note there).

**FSW tree:** the one FSW fact in this section is STIX-FSW 600a7a72
`utils_subcolIdFromFpgaId[5] = 10` (`ASW/ASW/src/utils.c:113-119`,
the table the model transcribed in §32). Nothing is copied from the
FSW beyond that cited value.

**The property (D-Db).** `det-inject` (str, write-only, on
`/machine/stixctrl`, added by `stix_det_class_init` like the other
scene knobs): `fpga_det=F,pix=P,ebin=B,count=N` — a one-shot deposit
of N counts at (pix P, fpga_det F, ebin B). The keys are mandatory, so
the index spaces are explicit. Rejected, storing nothing: a missing
key, an unknown key, a duplicate key, an empty or malformed field, a
value that is not a number, `count=0`, and an index outside its space
(`fpga_det` > 31, `pix` > 11 — the guard ring is never accumulated —,
`ebin` > 31); the space ends are inclusive. The setter only stores the
request and traces (D-Dj); `qom-get` answers "is not readable"
(write-only).

**The sink (D-Df event sink).** `stix_det_add_counts(fpga_det, pix,
ebin, n)` (declared in `stix_det.h` for the D-7 replay to share) adds
the counts per cell to two new deposit arrays, `inj_ql` and `inj_nom`,
on top of the drawn sky — a deposit must land at its own cell, so the
template's `pix`/`ebin` shares stay out of it. `stix_det_draw_acc`
(§34/§35) adds the cell's deposit to its Poisson draw and clears it,
so the counts wrap at 16 bits with the draw and reach:

- the LACC of the finished QL bank at the next QL swap (inj_ql), and
  through it the QL products;
- EACC — and so the next ROTB chunk (§35) — at the next nominal swap
  (inj_nom), **keeping the SUMDMASK rule** [D, RP-0067, the same as the
  sky's]: a deposit at an `fpga_det` outside SUMDMASK does not land in
  EACC and counts in ETRG only, through the nominal exposure's
  `nom_trg_extra_grp` (the triggers are not masked [I]); QLACC is not
  masked.

The sky's per-tick gating applies, read live at the applying tick: the
(fpga_det, pix) pair counts only where `stix_det_pix_gate` has the pix
on — where it is off, the deposit is dropped, like a real event.
SUMCNT takes the SUMDMASK × SUMEMASK share and LVAR the VARDMASK ×
VARPMASK × VAREMASK share (the inject is applied before the LVAR slot
write, so the share goes into the slot this tick writes). The triggers
get the same 1.25 × per event as drawn counts. The setter applies the
request at the next 100 ms tick (D-Dj); the deposits are cleared with
the exposure they belong to (a fresh enable, a device reset), and a
device reset also drops a not-yet-applied request (in-flight model
state, not a scene knob). Trace: `stixdet_inject(fpga_det, pix, ebin,
n)`, fired by the setter.

**qtests** (`tests/qtest/stix.c`; all five RED on the D-5 binary):
- `det-inject-lacc`: three deposits (fpga_det 5/pix 0/ebin 20 ×250,
  16/7/3 ×1000, 30/11/30 ×61) on three banks of the zero sky; the
  big-endian u16 at LACC byte ((P·32+F)·32+B)·2 equals N (read raw and
  decoded), the six in-range neighbours F±1, P±1, B±1 are 0, and the
  bank total is exactly N;
- `det-inject-chunk`: the 250-count deposit reaches the next chunk's
  payload at the matching offset (`nom_read_chunk`'s
  `[pix][fpga_det][ebin]` decode), the neighbours are 0, the whole
  chunk's EACC sums to exactly 250, ETRG grp 2 is
  (5·250+2)/4 with every other grp 0, the banks read zero after the
  swap; then with `fpga_det` 7 masked out of SUMDMASK a 500-count
  deposit stays out of EACC and lifts ETRG grp 3 by 1.25 × ~500 (the
  Poisson draw, 5σ bounds);
- `det-inject-props`: every rejection above; a rejected set stores
  nothing (the bank then holds only the one valid deposit); the keys
  may come in any order; `qom-get` fails (write-only, D-Db);
- `det-inject-gated`: with TH = 63 on asic_ch 1 = pix 3 of fpga_det 18
  (the det-chunk-format gate), a deposit at the gated-off 18/3 is
  dropped whole — the gate check comes before the inj_ql increment —:
  the finished LACC bank and the next chunk's EACC (and ETRG) stay all
  zero, while the same deposit at the gated-on 18/2 in the same run
  lands at its own cell in both;
- `det-inject-reset`: a deposit set and then hit by a system reset
  before any tick is gone (in-flight model state, not a scene knob):
  after the scene is re-armed — with the pair gated on, so a surviving
  request would land at the first tick — the LACC bank past the QL
  swap is all zero and SUMCNT is 0.

**Gates.** `detector-smoke.sh` carries the deposit in its `--flare`
modes only (see the deviation below): at @85 — T_nom is due within 10 s
of the @70 TC, so this is T_nom + 5..15 s, always inside the first 8 QL
periods (T_nom + 32 s) that SSID 32 sums — the probe sends
`qom-set /machine/stixctrl det-inject "fpga_det=5,pix=0,ebin=20,
count=250"` through the existing QMP socket. The deposit lands in the
quiet phase (@85 < 135), so the SSID 32 dump holds the deposit plus a
few quiet counts. A new check `ql-inject` runs
`ql-check.py --ssid32-inject 10 20 0x2F 0x10` on the dump, where 10 =
`utils_subcolIdFromFpgaId[5]` (the one FSW-sourced index value in the
scripts — `stix_det.c` already carries the full table, §32 —, cited at
the check), 0x2F is the S0 byte of 250 (monotone, so the
few quiet counts on top only raise it) and 0x10 the quiet-sky bound. It
asserts, in the one SSID 32 dump of the exit flush:
- the sample whose detector byte (0-based `subcol`, ICD Table 105) is
  10 holds ≥ 0x2F at byte 21 (= `ebin` 20 + 1);
- byte 21 of every other sample is < 0x10;
- the target's other bytes are only compared with its `subcol` 9/11
  neighbours' range and logged — the plan's [L] item, resolved by the
  log until a live run says otherwise.

The deposit also shows as a one-sample spike in SSID 30 band 3
(`ebin` 17-22 contains `ebin` 20) and in SSID 33; the sky checks see
both, nothing asserts them. QLDetPerf stays below red: the target
collects about 250 + 160 counts in one 8 s check (above Cbad 300 but
below 5 × median ≈ 800, and only once — going red needs Nrep 15
consecutive checks), and the gate forbids 0x942E/0x5437 anyway.

`ql-check.py` gains `--ssid32-inject SUBCOL EBIN MIN_SPIKE MAX_QUIET`
(hex tolerated) and `check_ssid32_inject`; its self-test adds the six
cases above (a passing dump, the deposit at the wrong `subcol` — two
mismatches: the quiet target and the spiking neighbour —, a too-small
deposit, a leak into another sample, a dump without the target
`subcol`, and a dump with no SSID 32 product at all).

**Deviations from the plan's D-6 section:**
- the deposit runs in the `--flare` modes only (`--flare` and
  `--flare --negative`, which injects too: the deposit does not depend
  on the flare switch). The plan's D-3 modes keep their zero sky and
  their byte-for-byte checks — a deposit would break the zero-flush
  contract ("every value byte 0") at SSID 30, 32 and 33 — and the
  expected-failure sets of `--negative` (`uart-idefx trace-verif`),
  `--no-rotb` (`uart-chunk chunk`) and `--flare --negative`
  (`ql-contrast chunk-flare`) are unchanged, since `ql-inject` passes
  in every mode that runs it;
- the deposit is sent at @85, not at exactly T_nom + 8 s: the probe has
  no send-after-expect group, and a fixed offset in [T_nom + 5,
  T_nom + 15] is inside the 8-period window for every T_nom the probe
  accepts (≤ 80);
- in the TM the check can only see the `subcol` dimension (the SSID 32
  sample sums all `pix`), so the `pix` and `ebin` neighbours are
  pinned by the qtests at the LACC/EACC offsets, as the plan's
  "End-to-end check" paragraph already assigns.

**Gate record** (2026-11-12, `stix-uprev-det6` rebased on ad5472dfb1 =
platform + flash latency on, slot 2):
- suite (qt.sh all): 92 of 92 ok, including the five `det-inject-*`;
  `det-inject-gated` and `det-inject-reset` go red when the pix-gate
  check is moved below the `inj_ql` add or the reset line is removed;
- boot-smoke PASS (golden unchanged); tc-smoke PASS;
- nominal-smoke PASS, and `--negative` PASS;
- fault-smoke PASS, and `--negative` PASS;
- detector-smoke: positive PASS; `--negative` PASS (failed as planned);
  `--flare` PASS (`ql-inject`: 250 counts at fpga_det 5, pix 0, ebin 20
  seen at subcol 10, byte 21 = 0x2F); `--flare --negative` PASS (failed
  as planned); `--no-rotb` PASS (failed exactly `uart-chunk chunk`).
- The first `--flare` run on the pre-rebase build failed `trace-verif`
  only: a QlClear strobe inside a ~30 ms verification run split it
  (136 + 248). Fixed in idefx-trace-check.py (bbd794466b: a QlClear ends
  only a complete run); the failed trace re-judges to 4 x 384.

## 37. DSS .dssevs replay (baseline: no ADC-domain count source)

**Baseline:** the only count sources were the synthetic sky (§34) and
the det-inject deposit (§36), both generated directly in `ebin` space;
there was no way to feed the accumulators from the DSS bench data, and
no count path went through the FPGA ELUT at all.

Detector plan D-7 (`docs/stix-port/2026-11-11-detector-plan.md`,
"### D-7", decision D-Dk, the line-772 note that the ELUT rule applies
only to ADC-domain sources, and "The user's `.dssevs` file"; the "As
built" note there).

**The format.** A `.dssevs` file is a stream of big-endian u32 words
with no header, decoded from the user's file (plan tag [V]; nothing
from it is committed or copied): `[31:20] val12 | [19:16] type |
[15:11] dss_det | [10:0] wait11`, the wait in 20 ns units, and every
word's wait adds to the replay time. Types 0x0-0xB are events (the type
is the `pix` 0..11, the value the raw 12-bit `adc`); 0xD and 0xE are
one setup word per `dss_det` (the 0xE value, about 2000, is the
baseline; the 0xD one is ignored (setup words are not modelled)); 0xF
is wait-only padding (the val-3072 marker included). A word of type
0xC, which the format does not define, a size that is not a multiple
of 4, and a file longer than 2^32 ticks of replay time are malformed.

**Index spaces.** `fpga_det = dss_det` (identity — the DSS app's
`fe_sc` → `dss_det` table equals `Detectors_fpgaFromFrontendMinusOne`
and `utils_oldSubcolIdToFpgaId`, plan "Index spaces"), so a replayed
event's detector field is used as the `fpga_det` with no lookup; the
`pix` comes from the type field; the `ebin` comes from the ELUT or the
fallback below. Every new name carries its space: the table's `t_tick`
(replay time in 100 ms ticks), `replay_base_dss_det` (the baseline per
`dss_det`), `replay_row_fallback` (per `fpga_det`, `pix`).

**The pre-scan (D-Dk).** `det-replay-file` (a qdev property on
`grlib-stixctrl`, init-time) names the file; `stix_det_realize` scans
it in 1 MB chunks (never whole into memory), decodes each word by its
type and keeps only the events in a table `{t_tick u32, fpga_det u8,
pix u8, adc u16}` (about 60 k entries for the user's 176 MB file). The
scan time and the table size are traced (`stixdet_replay_scan`). A
missing, unreadable or malformed file is a realize error naming the
path, so QEMU does not start; the realize-failure path frees the tick
timer, as the controller does for its own allocations (a realize
failure does not run the device's unrealize).

**The ELUT (D-Dk, honoured and fallback).** Each event's `ebin` is
looked up in the FPGA ELUT the FSW wrote — `val[pix][fpga_det][adc]`
(`Detectors.h:47-52`) at 0xE0000000 — and an entry of 32 (unassigned)
drops the event, as the FPGA would. A **degenerate** row — all 4096
entries equal: all 0 (power-on/EDAC), all 31 (what `applyELUT` writes
from the all-zero boundary table of an empty flash, `LUTMgmt.c:912` —
its comment "= 0 - 30" is wrong), or any other constant — cannot be a
calibration, so the row falls back to the non-physical linear binning
`ebin = (adc − base) × 32 / (4096 − base)` clamped to 0..31, with
`base` = the `dss_det`'s 0xE baseline from the file (0 if it has none);
events with `adc ≤ base` are dropped. With base 2000 one bin is 65.5
`adc`. This **differs from what the FPGA would do with this ELUT**
(hardware would bin every event into the row's constant); the trace
`stixdet_elut_fallback(fpga_det, pix, value)` fires once per newly
degenerate row per classification, with the constant as `value`. The
rows are classified at each fsmCmd SetEnable 0→1 edge and at each QL
swap while `det-source=replay-dss` (a 1.5 MiB scan every 4 s only in
that mode), so a TC(237,8) apply during NOMINAL takes effect within one
period. The synthetic sky (§34) and det-inject (§36) are unaffected:
they never go through the ELUT.

**The clock (D-Dk, D-PLm req 3).** The scene knobs `det-replay-trigger`
(`nominal` — the default, the first SetEnable 0→1 edge after a reset,
i.e. NOMINAL entry — or `qmp`), `det-replay-offset-ms` (file
milliseconds at start; renamed per D-PLm req 3 from D-Db's
`det-replay-offset` uint32 seconds before D-7 landed, so the two never
coexist) and `det-replay-start` (true starts a loaded replay now,
false stops it, reading gives the running state) are runtime
properties on `/machine/stixctrl` like the other scene knobs, and
survive a device reset; a running replay stops at one and re-arms on
its trigger (trigger qmp re-arms only through a new
`det-replay-start=true`). `det-source=replay-dss` is accepted only
when a table is loaded, so it must be set after QEMU is up (a
`-global` at startup is applied before the realize scan and rejected);
the replay replaces the synthetic sky. The setter of
`det-replay-start` only stores the request (D-Dj); the next 100 ms
tick applies it. The replay clock is `det-replay-offset-ms` + virtual
time since the start, advanced one 100 ms tick per tick; the offset
converts to ticks with integer maths (`offset_ms / 100`), so a
sub-tick offset rounds down to the tick, and a value past 2^32 ticks
is rejected. Each tick feeds the events whose `t_tick` falls in it
through `stix_det_add_counts` — the D-Df event sink of §36 — so the
D-6 gating (quarter, `grp` enable, ReadMode, R3 TH < 63 per `pix`) and
the mask rules (SUMDMASK for EACC, the ETRG 1.25× per event, SUMCNT's
SUMDMASK × SUMEMASK share, LVAR's VARDMASK × VARPMASK × VAREMASK
share) apply unchanged. The offset skips the events with `t_tick`
below it (an event exactly at the offset arrives in the first tick).
At the end of the table the replay stops (counts zero); no loop. Gain
is fixed at 1× (no knob). Traces: `stixdet_replay_state` at start,
stop, end of file and device reset.

**The status (D-PLm req 4).** `det-replay-status` is a read-only
string on `/machine/stixctrl`, built in one getter call (atomic):
`state=idle|armed|running|ended|stopped,pos_ns=…,events=…,dropped=…,
unsupported=…,table=…,count=…,duration_ns=…`. `idle` = no table
loaded; `armed` = loaded, not started (the nominal trigger waiting for
the SetEnable edge, or trigger qmp waiting for
`det-replay-start=true`); `running`; `ended` = EOF reached; `stopped`
= stopped by `det-replay-start=false` — a device reset re-arms
instead, so it reads `armed` again. `pos_ns` is the replay clock
position (`t_tick` × 100 ms); `events` the events fed so far (since
the last start); `dropped` counts only what the D-6 gate dropped (the
pix gate above; cheap and exact — the ELUT's own drops, an unassigned
ebin or an adc at or below the fallback baseline, are not counted);
`unsupported` the words of a kind the model does not implement (the
0xD setup words; 0xF padding is not one); `table` the file path
(empty when none); `count` the events in the table; `duration_ns` the
last event's `t_tick` × 100 ms. D-PLm req 2 (the runtime load by
path), the STXEVT v1 format and the loop are deferred to the platform
track.

**qtests** (`tests/qtest/stix.c`, one block after the det-inject
block, each with its own small synthetic `.dssevs` in a temp dir —
never the user's file; all RED on the D-6 binary, where
`det-replay-file` is an unknown `-global` so QEMU exits):
- `det-replay-decode`: events with `pix` = type and
  `fpga_det` = `dss_det` (events on `dss_det` 2, 9, 10 and 15 land at
  those `fpga_det`), the baseline 2000 taken from the 0xE words, 0xF
  wait-only — and the wait sum pinned by the bank each event lands in
  (QlIntTime 9: the t_tick-0 events in bank 0, the event after 2 s of
  padding in bank 1, after 4 s in bank 3); the linear fallback bins
  adc 2001 → `ebin` 0, 2262 → 4, 2393 → 6, 4095 → 31; adc at or below
  the baseline is dropped; one LTRG per event per `grp` (1.25×
  rounded); SUMCNT counts the SUMEMASK share;
- `det-replay-elut` (the qtest writes the rows into 0xE0000000 +
  (`pix`·32 + `fpga_det`)·4096 directly): an honoured row (ebin =
  adc/128, 32 for adc < 256, its `dss_det` baseline lowered to 64 so
  the 32-entry region lies above it) bins exactly as written and the
  32 entry drops its event where the fallback would have binned it 0;
  an all-31 row and an all-0 row both take the linear fallback; the
  rows are independent in one run; a row rewritten between two swaps
  switches branch at the next swap; `stixdet_elut_fallback` counted
  from a `-D` log: once per degenerate row, never for the honoured
  row, never twice for the rewritten one;
- `det-replay-trigger`: with trigger nominal the first events arrive in
  the first tick after SetEnable (QlIntTime 0, one bank per tick) and
  at their `t_tick` thereafter; with qmp a full period passes empty
  until `det-replay-start=true`, and false stops it (the banks then
  stay empty through the event that would have been due);
  `det-replay-offset-ms=1000` skips the t_tick-0 event, and a
  non-multiple of 100 rounds down (5050 ms is tick 50: the event at
  t_tick 50 arrives, an offset past 2^32 ticks is rejected); after the
  last event the replay stops (no loop, no new counts); a system_reset
  stops it and re-arms the nominal trigger (the events arrive again
  with a fresh clock), while with trigger qmp nothing re-arms until a
  new `det-replay-start=true`. At the key points `det-replay-status`
  reads `armed` before the start, `running` mid-file, `ended` after
  the EOF and `stopped` after the `det-replay-start=false`, with
  `events=` fed so far and `count=` the table's events (and the
  no-table `idle` line in `det-replay-missing`);
- `det-replay-missing`: the realize errors (missing path, a path
  through a missing directory, a 6-byte file, a type-0xC word) via the
  `g_test_trap_subprocess` pattern of the platform realize-error test,
  each naming the path or the defect; and, without a table,
  `det-source=replay-dss` and `det-replay-start=true` are rejected
  while `det-replay-trigger`/`det-replay-offset-ms`/`det-replay-start`
  read back their defaults and `det-replay-status` reads
  `state=idle`.

**Deviations from the plan's D-7 section:**
- the optional `detector-smoke.sh --replay` gate (SKIP 77 without
  `STIX_DSSEVS_FILE`, the SSID 32/SSID 30 checks, the fallback-trace
  assertion) is **not built yet**: this change delivers the replay,
  the properties and the qtests; the gate and its gate record stay
  pending (see below);
- the fallback trace fires once per newly degenerate row per
  classification, so the first classification of an empty-flash run
  traces all 384 (`fpga_det`, `pix`) rows once — the plan's "once per
  row and replay start", read literally;
- a file longer than 2^32 ticks (about 13.6 years) of replay time is
  treated as malformed: the table's `t_tick` is u32 by D-Dk;
- the plan does not say whether a word of the undefined type 0xC is
  tolerated; the model rejects the file (a clear message beats a
  silently ignored stream), and the qtest pins it.

**Gate record** (2026-11-12, `stix-uprev-det7` on 6d07ffd4e3 = D-6, with
the review fixes and the D-PLm req 3/4 pass, slot 2):
- suite (qt.sh all): 96 of 96 ok, including `det-replay-decode`,
  `-elut`, `-trigger` and `-missing` (five subprocess cases, the last
  one the 2^26-event cap on a sparse file: 2.9 s, 532 MB peak);
  `det-replay-trigger` goes red when the false branch of
  `det-replay-start` stops retracting a pending start;
- boot-smoke PASS (golden unchanged); tc-smoke PASS;
- nominal-smoke PASS, and `--negative` PASS;
- fault-smoke PASS, and `--negative` PASS;
- detector-smoke: positive PASS; `--negative` PASS (failed as planned);
  `--flare` PASS; `--flare --negative` PASS (failed as planned);
  `--no-rotb` PASS (failed as planned).
- The optional `detector-smoke --replay` gate is not built (deferred,
  see above); the replay is covered by the qtests only.

## 38. Known limits after the detector phase

What the detector model (§31-§37) deliberately does **not** do, so a
missing or surprising behaviour is not mistaken for a bug (plan D-8,
"Out of scope"). The GS-side recipes are in `ground-station.md`,
"Detector scene".

- **Calibration never completes.** SetCalEnable only sets CalEnable:
  CalReady never sets and CalCount/CalRes stay 0 (§31), so the
  TC-driven calibration, test-pulse and threshold-scan procedures
  cannot run to completion. The IDeF-X ADC answers are canned (§32):
  readTemp gives A − B = 0x200, adcRead and the on-demand reads give
  1850.
- **No aspect ROTB blocks** (plan D-Dg; §35 builds only the 0x8000
  detector descriptor): the ring carries only detector chunks.
- **No SEU injection** (§31): QuarterSeu always reads 0. The IDeF-X
  registers are also outside the memory-upset track (§45).
- **The RCR → attenuator-motion path is untested** (§22, §34, §43):
  above about 17.5 k counts/s per `fpga_det` the FSW's RCR inserts the
  attenuator. The model animates 0x5C-commanded moves and the ATT_C
  current since §43 (A-2/A-3: 20 idle, +100 per moving driven motor,
  +300 stalled), but the gate ELF (AX) never writes 0x5C, so there
  ATT_C stays 20 and the RCR's insertion command is never exercised —
  keep the rates low. The `det-*-cps` properties allow up to 1 000 000
  for experiments, at the user's risk.
- **The synthetic sky and `det-inject` bypass the ELUT** (deliberate
  deviation, D-Df/Doc vs code 20): they generate counts directly in
  `ebin` space, while hardware with an empty flash would bin every
  event into `ebin` 31. A comparison against real hardware with an
  empty flash must not use the synthetic sky.
- **The replay's linear fallback is non-physical** (§37): on a
  degenerate (constant) ELUT row the model bins linearly above the
  file's baseline, which differs from what the FPGA would do with this
  ELUT (bin everything into `ebin` 31). `stixdet_elut_fallback` traces
  it once per newly degenerate row per classification.
- **The replay gain is fixed at 1×**, and about 25 of 32 `fpga_det`
  stay dark with the known file. Only QLDetPerf is known to react to
  dark detectors; the live `--replay` gate run that would confirm
  nothing else reacts is still pending (no `--replay` gate yet, §37).
- **The optional `detector-smoke.sh --replay` gate is not built**
  (§37): the SSID 32/SSID 30 replay checks and the SKIP-77
  `STIX_DSSEVS_FILE` mode are pending, and §37's gate record stays
  open until it runs.
- **The LVAR slot rule is a model choice** (§31): the documents do not
  say what the FPGA does with the ticks of a QL period beyond slot 39;
  the model drops them, so it never writes past the bank's 0xA0 bytes
  (the WDT storage at +0xA8 and MemBram stay intact, qtest-pinned).
- **ROTB overflow drops the whole new chunk** instead of overwriting
  the oldest block (§35, Doc vs code 7). Unreachable while the FSW
  drains the ring.
- **ClearEnable freezes and Reset partially clears** (§31): both are
  model choices ([I] in the plan); the FSW's SetEnable-then-Reset
  order makes the difference invisible.
- **The ISR can spend the hk-prot unlock token** (§31): a QlClear
  landing between HK.c's token write and its protected store drops
  that store. Hardware-faithful, nearly unreachable under TCG,
  accepted; `det-token-spend` pins the rule.
- **A disabled `grp` in a powered `quarter` reads zeros** and so trips
  the ASIC-verification FDIR (0x542E → 0x9438; §32, first stated §24)
  — plausibly hardware-faithful. No gate disables a `grp`; a later TC
  gate must expect it.
- **THS/TPG can stall the QL task for about 1.8 h** (§31): a latched
  QlIntTime of 0xFFFF outlives THS_cleanup until the next period end.
  Later gates that run THS or TPG must expect it.
- **The NOMINAL-entry R3 is mirrored twice in the FSW** (STIX-FSW
  issue #1069, §34): only `pix` 0, 3, 6, 8 (and usually 7, via an
  out-of-bounds read) are gated on, so the observed sky is 0.475 of
  the template estimate. `det-counts-r3-fsw` shows all 12 `pix`
  counting with a corrected build; recalibrate the flare thresholds
  then.
- **`det-seed` is `-global`-only** (§34, an explicit exception to the
  runtime-knob rule), and **no run is bit-for-bit reproducible** (no
  icount; the same seed gives statistically equal, not identical,
  banks).
- **No vmstate** (§31): the detector scene, counters and IDeF-X store
  are not migrated or snapshotted.
- **BRAM width truncation is not modelled** (the §13 gap): the FSW's
  over-wide BRAM writes are stored whole. Out-of-scope candidates for
  a next phase (plan): a science-TM gate (TC(238,x) L0), the RP-0119
  QL dump replay, an ADC-domain synthetic sky through the ELUT,
  ALIMON-based pixel gating, FIFO overflow.

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

## P-1. Platform time and read-only hardware state (baseline: none)

Platform plan PL-1 and PL-2 (`docs/stix-port/2026-11-11-platform-plan.md`,
D-PLb, D-PLc, D-PLo, D-PLr). The heading is non-numeric on purpose: it
becomes §39+ when this branch merges with the detector branch (§31-§38).

**Baseline:** the time codes followed only the IDPU's own SCET (§20), so
the "spacecraft clock" was the instrument clock and no time-sync test
meant anything. There was no one-shot or wrong-value code.

**The default keeps §20.** `timecode-source` defaults to `scet`: the
codes, their TIMECNT, the commit hook and every golden are as in §20.
Everything below is opt-in or a runtime property that nothing sets
unless a test or the platform controller does.

**Model (PL-1, stixspw):**
- **`timecode-source`** (qdev string, `scet` | `obt`; any other value is
  a realize error) and **`obt-epoch`** (qdev uint64, CUC 32.16, default
  `0x10000000 << 16`, so coarse & 63 = 0; wider than 48 bits is a realize
  error).
- **In `obt` mode** the OBT = obt_base + (vclock − vclock_base) · 65536
  / 1e9 (whole seconds and the remainder converted separately, so long
  runs do not overflow). The timer fires at the OBT rollover (the
  remainder rounded up, so the OBT at the deadline is the new coarse,
  fine 0); TIMECNT = OBT coarse & 63. The guest SCET commit hook does not
  move the codes. The skip rule (fine ≥ 0x1000) is unchanged.
- **`obt`** (runtime uint64, CUC 32.16 in bits 47:0): the getter reads
  the OBT, or in `scet` mode the SCET (coarse without UNSYNCED, as
  `stixctrl_scet_now`), with no side effect. The setter is an OBT jump,
  in `obt` mode only: it rebases at the current vclock and re-arms. A
  value past 48 bits or a set in `scet` mode is refused. The OBT survives
  a device reset (like `timecodes`).
- **`timecode-once`** (runtime bool): true arms one correct code at the
  next rollover while `timecodes` is false; a no-op while it is true;
  refused while `link-down` is true or control.TR is 0 (traced with
  `emitted=0`). The getter is the pending state. A pending once is
  dropped (traced, `stixspw_timecode_once_drop`) by `link-down = true`,
  by any `timecodes` write and by a `false` write. It survives a reset;
  with TR = 0 after the reset it is consumed with nothing emitted.
  Likewise, a `timecode-once` that QMP accepted is **silently consumed**
  (only the `stixspw_timecode_manual … emitted=0` trace shows it) if the
  guest clears control.TR before the rollover: the set returns success
  and no code reaches the FSW.
- **`timecode-inject`** (runtime write-only uint32, 0..63): one code now
  with that TIMECNT through the same TR/TQ/IE path and D-M latch as the
  timer (`stixspw_tc_emit`, factored out of the timer). 64 and above,
  `link-down` and TR = 0 are refused.
- **Re-entrancy:** the once/inject paths only latch registers and pulse
  IRQ 13; no DMA runs, so they need no re-entrancy guard (the TX/RX paths
  reachable from QMP keep theirs, §29).
- **Traces:** `stixspw_timecode_manual(kind, timecnt, emitted)`,
  `stixspw_timecode_once_drop(why)`, `stixspw_obt_set(old, new)`;
  `stixspw_rx_deliver` gains `vclock_ns` (the D-PLd ordering check).
  Traces are in no golden.

**Evidence (PL-1):** qtests `/sparc/stix/spw-obt`, `spw-timecode-once`,
`spw-timecode-inject`. RED before PL-1: QEMU refuses
`-global grlib-stixspw.timecode-source=obt` ("Property not found"), and
`qom-set timecode-once` / `timecode-inject` fail.

**Model (PL-2, read-only state, D-PLo/D-PLr):**
- **PL-2a, `stixctrl_hk_value` split:** `stixctrl_hk_truth` (the table
  plus the hkCmdSta/idefxConfig overrides, no injection) and
  `stixctrl_hk_value` = the injection if set, else the truth. A pure
  refactor: the hk-adc, powerstat, adc-inject and hk-prot qtests and the
  boot/nominal/fault gates are unchanged.
- **`/machine/stixctrl` `hw-state`** (read-only str, JSON via QDict and
  `qobject_to_json`, `"schema": 1`, `vclock_ns`): `model` (the init-time
  switches); `supplies` (hkCmdSta enables 4:0 and POWERSTAT 9:5 as
  `stixctrl_hkcmdsta` composes them, per line `enabled`/`good`);
  `hk_adc` keyed by `ch` for every channel with a table entry: `truth`,
  `presented`, `injected` and `last` (the last guest conversion's value
  and `vclock_ns`, null since reset), plus any channel with an active
  injection even when its table entry is 0 (truth 0); `hk_prot` (IRQEN, PROTSTAT,
  HOTSAMPLE and the 9 lines with `line`, `ch`, `bit`, `threshold`,
  `prot_en`); `watchdog` (running, count, reload); `scet` (coarse,
  fine, `unsynced`); `det.quarter_on[quarter]` (idefxConfig 3:0; the
  FSM, QL bank and replay come later through the detector's accessor).
  The only new store is the last conversion in the hkAdc write path
  (cleared at reset), golden-neutral.
- **`/machine/stixspw` `hw-state`** (same form): `ls`, `link_down`,
  `status`, `control` (raw, TR/TQ/IE/LI); `timecode` (source, enabled,
  once_pending, last TIMECNT and its `vclock_ns` from any emitted code,
  the next due `vclock_ns` from the pending timer, null when none);
  `obt` with `obt_kind` (`platform` in obt mode; `scet` when it is the
  IDPU's own SCET, which must never be labelled the platform OBT);
  `rx_queued`; `counters` (tm_dropped, rx_dropped, tc_resync,
  tmtc_connected).
- **`/machine/stixspw` `vclock-ns`** (read-only uint64):
  `qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)`, the controller's time master.
- **The read-only proof:** the getters read the register files and
  model state directly and never call an MMIO handler (the CTRL read
  handler stores the SCET and the watchdog into reg on every read); no
  register, timer, queue or IRQ is touched.
- **The contract (binding for later additions):** `hw-state` is **pure
  reads only**. The controller polls it at 2 Hz, so every accessor it
  calls (the detector's `det` section, the attenuator's truth/presented
  getters, any later module) must be a side-effect-free read: no MMIO
  handler, no register write, no timer, IRQ, queue or counter change.
  The ctrl-hw-state qtest's equal-reads and no-io_read-trace check is
  the guard.
- **Scripts (PL-2b and PL-2):** the probe's `--qmp` and `QmpClient` take
  `tcp:HOST:PORT` as well as a unix path (a bad address exits 2);
  `run-qemu-gs.sh` `STIX_GS_QMP` takes `tcp:127.0.0.1:PORT` or
  `tcp:localhost:PORT` (numeric port, other hosts refused), and the
  unix form produces the same `-qmp unix:…,server=on,wait=off` as
  before.

- **The warm-reset window (PL-4 review NIT):** the device reset of a
  warm guest reset (`-action reboot=reset`) clears `link-down` and every
  `adc-inject[N]`, and the controller re-applies them when the QMP
  `RESET` event reaches it, while the guest already runs. For that
  window the FSW sees the link up and the true ADC values: a QMP event
  read plus one `qom-set` round trip per switch, of the order of 1 ms
  of wall time each [I, not measured], so a few ms for a handful of
  forcings. No FSW-visible effect is expected [I]: after a reset the
  boot software runs from PROM and loads the ASW from flash for well
  over a second before the ASW starts the SpW link and the HK ADC
  sequence. A synchronous re-apply is possible without a C change
  (`reboot=shutdown,shutdown=pause`, then `system_reset`, re-apply
  while frozen, `cont`), but it changes the warm policy's argv, the
  power state machine and the bench-clock pause accounting, so it is
  left as a follow-up; the cold policy re-applies before `cont` already.

**Evidence (PL-2):** qtests `/sparc/stix/ctrl-hw-state` (schema, the
enables and POWERSTAT after hkCmdSta writes, `quarter_on` after
idefxConfig, truth 2048 vs presented 1800 on an injected ch 24 with the
last conversion, a DET_C trip in PROTSTAT/HOTSAMPLE, the watchdog, the
UNSYNCED flag after a 0xC8/0xCC commit with bit 31; two reads equal and
no `stixctrl_io_read`/`stixspw_io_read` trace line while the getters
run, with an MMIO read as the positive control), `spw-hw-state` and
`vclock-ns`. RED before PL-2: `hw-state` and `vclock-ns` are unknown
properties.

**Review fixes (PL-1/PL-2 review):** qtests `plat-realize-error`
(timecode-source `utc` and obt-epoch 2^48 refused at realize, named in
the error) and `plat-review-extras` (`timecode-once=false` cancels; a
once survives a reset and is consumed with `emitted=0` while TR = 0;
once and inject in obt mode; `obt_kind`; an injected unused channel in
`hk_adc`). RED before the fixes: `obt_kind` missing. The probe connects
TCP with `socket.create_connection` and takes `tcp:[::1]:N`;
`run-qemu-gs.sh` takes host 127.0.0.1, localhost or [::1] (`::1` is
bracketed for QEMU), port 1..65535, and refuses any comma. The probe self-test gains q8 (a real TCP fake-QMP listener)
and q9 (the address forms); RED before PL-2b: "cannot connect to
tcp:127.0.0.1:N: No such file or directory". `scripts/stix/tests/
test-run-qemu-gs.sh` runs the script against an argv-recording stub
(no QEMU): 11 cases; RED against the pre-PL-2 script, the two tcp
accept cases fail with "STIX_GS_QMP must be an absolute path".

## 42. NAND flash latency: ops complete from a timer (baseline: synchronous inside the cmdSta write)

Plan: `2026-11-11-flash-latency-plan.md` (section number: the next free
one — §41 is gate ports; the attenuator and platform tracks have not
claimed theirs yet). The PFM flight build
(`ASW_pfm_v186.elf`) reset at ≈6.8 s: the FS-init task (priority 250)
holds the filesystem mutex and is lifted to priority 1 by inheritance
(`configFSW.h:199`, `MemFilesystem.c:477-481`), and with the baseline's
zero-latency flash it never blocks during the 130,800-read boot scan, so
the watchdog task never runs. The fix models the NAND timings; the
watchdog starvation is then the FSW's own priority-inheritance quirk,
documented under "FSW oddities" below.

- **Latency, per op** (`-global grlib-stixflash.latency=...`, default
  **on**): page read 130 µs, page program 300 µs, block erase 1,500 µs
  (`tread-us`/`tprog-us`/`tbers-us`, each ≤ 40,000 µs, 0 = inline).
  These are datasheet-typical values **[A]** — the FSW tree and its TSIM
  flash model carry no timings (`FlashApiTsim.c` is synchronous too).
  The FSW's own page-wait timeouts are 50 ms (`configFSW.h:336-338`), so
  the defaults stay far below them.
- **Issue/complete split.** The cmdSta write only latches the op
  (BUSY = 1, READY = 0); completion — host file I/O, the read DMA,
  status, one IRQ pulse, last — runs inline at latency 0 or from a
  `QEMU_CLOCK_VIRTUAL` timer. A read's file I/O therefore **moves from
  the cmdSta write to completion** (baseline: everything inside the
  write). A program still samples guest RAM at issue; WRPROT reads 1 at
  all times (the clear/restore dance the guest could never observe is
  gone).
- **Reset aborts a pending op** (D-F4): no IRQ, nothing committed to the
  file, trace `stixflash_op_abort`. On hardware an interrupted program
  can leave a partial page; the model's clean abort is accepted.
  A non-reset command while busy is dropped (`stixflash_busy_reject`);
  a sequence error while busy sets no status and pulses no IRQ.
- **`latency=off` is not byte-identical on error paths.** The read DMA's
  result is now checked: a refused or failed DMA completes the op
  FAILED | READY plus the IRQ (trace `stixflash_error "dma"`), where the
  baseline ignored `physical_memory_write`'s result and completed OK.
- **The FSW's ISR last-ditch busy-poll** (`Flash.c:257-273, 378-406`:
  10,000 cmdSta reads after a fatal ISR path) succeeds only if those
  reads take longer than t_op in host time — the completion timer fires
  from the main loop between the vCPU's MMIO reads. Fatal-path only.
- **FIX (correcting F-1's "no behaviour change"):** the new device reset
  handler (`grlib_stixflash.c:919-928`) returns cmdSta and write_counter
  to 0. Before it, a guest system reset at write_counter 1 or 2 left the
  stale count, so the next boot's `Flash_reset` RAMAD write
  (`Flash.c:81-84, 212`) failed as a sequence error. Qtest
  `/sparc/stix/flash-reset-state` pins it.
- **The device is unmigratable** (commit `530a7c441c`): savevm and
  migrate are refused with a message naming grlib-stixflash, because a
  snapshot with an op in flight would silently drop the op, its
  completion IRQ and the sequence state (no STIX device has a vmstate).
  savevm stays unused until the platform roadmap's savevm item (R-r3)
  adds one.
- **Measured PFM boot.** With latency on, the debug ELF
  (`ASW_pfm_dbg.elf`) reaches "Main delete init" — FS init done — at
  about **62 s of guest time**. First live release-ELF run
  (`ASW_pfm_v186.elf`, fresh flash, probe offsets): 0x0022 + 0x0021
  (SAFE) at +0.4 s, 0x14A3 at +16 s, 0x142E at +32 s, FS init still
  running at +46 s, 47 watchdog RELOADs and no reset — FDIR and the
  TC20 timer are **not** gated by FS init, and the mode change waits on
  the filesystem/context (a CONF sent before 0x0032 was accepted but
  never executed; the gate now anchors CONF on max(0x142E, 0x0032)).
  The plan's lower bound is ≈26 s (130,800 page reads at the defaults).
  Gate: `scripts/stix/pfm-boot-smoke.sh` (positive + `--negative`; the
  negative proves the gate sees the original failure: latency off ends
  in the watchdog's "count expired" reset).
  Live gate record 2026-11-12 (slot 4, latency on): release PASS
  (0x0032 at +58.3 s), `--negative` PASS (count expired, QEMU exited),
  `STIX_PFM_DBG=1` PASS (0x0032 at +56.3 s; UART "delete init" at
  00:00:54.0). The first live runs found two gate bugs, fixed:
  - SID2 HK on the PFM comes every 64 s
    (`HK_TM_SID2_REPORTING_PERIOD` 512 x 125 ms, `ParamMgmtDef.h:204`,
    `HKReport.c:183`; no context file, so the default), so the NOMINAL
    group's SID2 mode-5 check has its own 70 s window, not the
    group's 5 s; the first run reached NOMINAL (0x0021 04 05) and
    failed only on that window.
  - The FSW ends UART lines with `\n\r`, so each line of
    `uart.pfm.txt` starts with a CR; the key-line order check strips
    it (it made the UART time `\r00:00:54.5` and the check fail).
- **Golden note.** `capture-golden.sh` runs `-trace 'stix*'`, so a
  future recapture of `qemu-stdout.golden.log` grows by the per-op
  `stixflash_op_start`/`stixflash_op_done` lines (≈15k per PFM boot). No
  gate compares that file.

**FSW oddities observed (for the FSW team; paths relative to the FSW
tree ASW/):**

1. `configFSW.h:199` redefines `RTEMS_DEFAULT_SEMAPHORE` as a binary
   **priority-inheritance** semaphore; `mutexAPI`
   (`MemFilesystem.c:477-481`) uses it, so the priority-250 FS-init task
   runs at priority 1 while INIT (1) waits on it — against the intent
   stated at `configFSW.h:263-268` ("watchdog has highest priority").
   Safe on hardware only because every NAND op blocks the task.
2. `MemFilesystem.c:1018-1060`: the async FS-init task holds `mutexAPI`
   across all partitions: 130,800 page reads on PFM at every boot, even
   on an empty flash.
3. `Flash.c:306-318, 352-376`: the polling fast path is commented out;
   its comments give ~2 ms per read and ~3 ms per write.
4. `Flash.c:214-221`: `Flash_reset` judges chip health from 2,000 BUSY
   polls with no time base.
5. `MemFilesystem.c:41-44`: the comment says the ICD reserves 8 MiB per
   flash; the code reserves 16 MiB.
6. `Flash.c:303-323`: the driver masks the flash IRQ around the command
   start; correctness when an op completes instantly relies on the IRQMP
   pending latch and RTEMS event latching.
7. `tasks.vs.json:14`: the default build adds `-DDISABLE_WATCHDOG`.
8. `WDT.c` `WDT_setWdgInterval` ends with DISABLE; whether RELOAD
   re-arms after DISABLE decides whether such a build has a live
   watchdog at all.

## 43. Attenuator mechanism and AttnCmdSta (baseline: plain storage)

Attenuator plan A-1 and A-2
(`docs/stix-port/2026-11-11-attenuator-plan.md`, D-Aa..Af, Ah, Ai). The
heading is non-numeric on purpose: it gets a number at the merge, like
P-1 above.

**Baseline:** AttnCmdSta (0x5C) was plain storage. A PFM ELF would read
OPERATIONSTATUS = 0, "moving", forever, wait 3 s at every SAFE entry and
end every move in the FSW's 3.3 s timeout (0x942B). The gate ELF never
touches 0x5C (an AX build, the stub is pure software), so on AX the
model is invisible; only ATT_C is real on both builds (A-2).

**Model (hw/sparc/stix_att.c, the child object
`/machine/stixctrl/attenuator`, type `stix-att`):**
- **`att-model`** (qdev bool on stixctrl, default on): off keeps 0x5C
  as plain storage, never reset, with `fsw-accesses` at 0. It is
  settable only before realize: a qom-set after realize is rejected by
  the generic qdev post-realize rule (the error names the property);
  `-global` applies at device_post_init, before realize, and
  `att-fault-reject` pins the rejection. On is safe
  because the gate ELF never accesses 0x5C [G, E]: the UART and trace
  goldens are unchanged, and 0x60/0x64 stay plain storage either way.
- **Timing knobs** (qdev on stixctrl, D-Ah): `att-initial` (out|in,
  default out — the AX stub's `isAB`; anything else is a realize
  error, like `timecode-source`), `att-travel-ms` (1000, the user's
  "usually 1 second-ish", Q3), `att-ramp-ms` (1600, the drive window
  from the PWM ramp), `att-debounce-ms` (10 [I]), `att-timeout-finishes`
  (on [I], Q4). `att-travel-ms`/`att-ramp-ms` 0 is a realize error (a
  zero travel would divide by zero).
- **The mechanism (truth, D-Ad):** one shaft, `pos_ppm` 0 = OUT,
  1 000 000 = IN, 1 ppm per travel-ms ns (end to end = travel time);
  the position survives a device reset. AB actuates iff pos = 0, BC iff
  pos = 1e6, both released in transit [I]. M1/M2 drive one shaft at
  nominal speed; a motor is powered iff hkCmdSta LVEN was set at the
  command (LV sampled at the command [A]).
- **Lazy evaluation (D-Ac):** no timer, BH, DMA or IRQ. Every 0x5C
  access, every ATT_C conversion or hw-state poll of ch 8 (A-2) and
  every state getter (`truth.position*`, `truth.last-end`,
  the sensors, `presented.*`; not the counters `truth.moves` and
  `fsw-accesses`) advances the pure state to QEMU_CLOCK_VIRTUAL;
  `truth.position` reads `moving-*` only while the shaft really
  travels (not for an LV-off or motorless command, nor in the debounce
  at the end stop); with `att-model=off`, `presented.cmdsta` is the
  plain storage the guest reads; the move's end (target switch + debounce, or the
  ramp end) is solved analytically at the command. The advance is
  idempotent, so an extra read changes nothing (the PL-1/PL-2 purity
  note); getters advance but never trace (D-Ai), so a move end a
  getter passes first leaves no `stixatt_end` line. With no
  out-of-MMIO path, the a83fe9a3a5 re-entrancy guard rule does not
  apply (D-Ai).
- **The controller (D-Ae):** a write of 1/2/3 (open M1/M2/both) or
  4/8/12 (close) starts — or restarts, from the current position — a
  move: OPERATIONSTATUS 0, the commanded MOTORxUSED (the drive flag,
  AttCtrl.c:387-388), the ramp window starts. The move ends
  `att-debounce-ms` after the presented target switch actuates
  (already-actuated included: a redundant move clears MOTORxUSED after
  10 ms), or at the ramp end with TIMEOUT = 1 and, with
  `att-timeout-finishes`, OPERATIONSTATUS = 1. ERRORMASK (bit 4)
  ignores the switches and runs the full ramp without TIMEOUT. 6 stops
  now (OPERATIONSTATUS 1, the flags clear, the position stays). Any
  other value is stored for readback only; status bits 11:5 are
  ignored on write. Idle reads: 0x500 at OUT, 0x300 at IN. A device
  reset aborts a running move at the reset instant (the position is
  kept, the registers go idle including the bits-4:0 readback, and
  `truth.last-end` = reset — recorded only when a move was actually
  running).
- **The ATT_C current (A-2, D-Af):** the PL-2a split carries hkAdc
  ch 8: `stixctrl_hk_truth(8)` is the mechanism — 20 idle, +100 per
  powered, live, driven motor while the shaft moves, +300 while it is
  driven but not moving (stalled on its target end stop: the override
  between the end stop and the ramp end, and the debounce after the
  target switch actuates, redundant commands included; the block point
  and the jam arrive with A-3) — and `stixctrl_hk_value(8)` is
  adc-inject[8] forcing first, else the model's presented value (==
  truth until A-3). att-model=off keeps the static table 20 in both
  halves. 120/220 stay under 0x100, so the FSW sums them
  (AttCtrl.c:344) and inside the PFM soft limits; the 320/620 stalls
  leave the sum and stay below the 751 HW threshold (no trip, D-P4d).
  The truth/presented functions advance the lazy state idempotently
  and never trace (the 2-20 Hz hw-state poll runs through them, the
  PL-1/PL-2 purity note); a move end a conversion commits is traced
  with its computed event time by the next 0x5C access. ATT_V stays
  derived from LVEN and ATT_T static 2450: no thermal model.
- **QOM (D-Ah), all read-only:** `truth.position` (out|in|moving-in|
  moving-out|between), `truth.position-ppm`, `truth.sensor-ab`/`-bc`,
  `truth.motor1-driving`/`motor2-driving` (torque, not the flag),
  `truth.moves` (restarts count), `truth.last-end`
  (none|target|timeout|override|emergency|fail|reset; fail is A-3),
  `truth.last-end-ns`, `presented.sensor-ab`/`-bc` (== truth until
  A-3), `presented.cmdsta` (the 0x5C readback as a read returns it
  now, no side effects), `fsw-accesses` (guest 0x5C accesses since
  realize, not cleared on reset; 0 on the AX gate ELF — the R5/Q9 AX
  artefact label) and, with A-2, `truth.att-c` (through
  stixctrl_hk_truth, so the static 20 with att-model=off) and
  `presented.att-c` (through stixctrl_hk_value, so adc-inject[8]
  included); for those the child holds an owner back-pointer to the
  controller, set at realize like stix_det's.
  There are no setters yet (A-3): qom-set is rejected ("is not
  writable").
- **Traces:** `stixatt_cmd(cmd, override, pos_ppm)` on every 0x5C
  write; `stixatt_end(reason, at_ns, pos_ppm)` when a 0x5C access
  commits a move end, carrying the computed event time (a ch 8
  conversion that commits one stays silent; see the ATT_C bullet).
  The unit is silent at realize, at reset and on 0x60/0x64, so the
  `-trace 'stix*'` golden is unchanged.
- **The [I]/[A] semantics recorded for A-later (Q2-Q5):** the
  stop-on-presented-switch rule and the 10 ms debounce, the
  timeout/finished relation, the override behaviour, the emergency
  semantics, OPERATIONSTATUS reset value 1, "a command during a move
  restarts it", the travel time (Q3) and the switch polarity (Q5:
  `stuck_open` = the bit reads 0). Each is one knob or one constant.

**Evidence:** qtests `/sparc/stix/att-*` in the `stix` binary
(registered behind `stix_att_register()`, in one appended block before
main()): `att-reset`, `att-close-open` (M1, M2, both), `att-redundant`,
`att-override`, `att-lv-off`, `att-emergency`, `att-reset-midmove`,
`att-initial-in`, `att-config1`, `att-model-off`, `att-qom-names`,
`att-determinism` (identical reads with and without interleaved
qom-gets; no `stixatt_*` line inside a getter), and the negatives
`att-reject-status-bits`, `att-reject-cmd-values`,
`att-reject-restart`, `att-reject-readonly`. RED before A-1: the
attenuator object and its properties do not exist and 0x5C is plain
storage (0, not 0x500). The move-timing tests set
`att-travel-ms=500`: the plan's +250/+510 timelines predate its Q3
answer (the default stays 1000).

A-2 adds `att-current` (20 idle, 120/220 with one/both motors moving,
20 after the end), `att-current-override` (120/220 to the end stop at
500 ms, then 320/620 stalled at IN until the 1600 ms ramp end, then
20), `att-current-redundant` (a redundant close at IN reads 320 during
the 10 ms debounce, then 20), `att-current-inject` (hw-state hk_adc.8
shows truth = presented with no fault; adc-inject[8] wins at the
conversion, in hw-state and in truth.att-c/presented.att-c while the
truth keeps the mechanism) and `att-current-prot` (the armed ATT_C
line never trips at the 620 stall; an 800 injection trips as the
positive control that the line is armed), and extends `att-model-off`
and `att-lv-off` (ch 8 reads 20). RED before A-2: ch 8 read the static
20 in every state.

A-3 adds the fault layer (D-Ag):

**(b) The layering: truth → named fault → forcing, per reader.**
- **The switches:** the truth switches follow the mechanism; the
  presented switches apply the `sensor_*.stuck_*` layer and feed both
  the 0x5C readback and the controller's stop rule (a stuck switch
  also changes when the motor stops, D-Ae). A stuck-closed target ends
  the move one debounce after the write — the drive runs that debounce,
  so the truth ends one debounce of travel off the end (the plan's
  outcome table shows the truth at the old end; Doc vs code 12 there).
  A stuck-open target never ends the move, even with the truth already
  on the end (the FPGA only knows the switch).
- **ATT_C:** truth (the mechanism) → `current.zero` /
  `current.offset` (clamp(truth + offset), −4095..4095 raw counts) →
  adc-inject[8], in the PL-2a split's presented half. The comparator
  samples at conversions only: a fault set does not trip, the next two
  consecutive conversions above the threshold do (D-Af).
- **The mechanism:** `motorN.dead` removes the motor's torque and its
  draw (the MOTORxUSED flag still sets, [I]); `mechanism.jammed`
  freezes the shaft while the driven motors stall against it (+300 per
  motor, like the end stop).
- **Forcing** is separate from the faults: `force.cmdsta-set` /
  `force.cmdsta-clear` (uint32, bits 11:5 only, else rejected) apply
  inside `stix_att_cmdsta()`, so the MMIO read, `presented.cmdsta` and
  the write-path trace see the same value; a bit in both masks reads 0
  (the clear wins); no physics — a forced BC bit does not stop a move;
  cleared on device reset like adc-inject. `fault-clear` does not
  touch them.

**(c) The catalogue and its persistence.** The nine A-3 rows are the
platform's dotted IDs (D-PLq), one QOM property each under
/machine/stixctrl/attenuator: the four `sensor_ab/bc.stuck_open/_closed`,
`motor1.dead`/`motor2.dead`, `mechanism.jammed` (new, proposed to the
platform), `current.zero` and `current.offset` (new, renamed from
`att_c.offset`; `current.scale` stays A-later). They are persistent
until cleared **and survive a device reset** — a broken switch stays
broken over an IDPU reboot — which deliberately differs from Phase 4's
D-P4b; the adc-inject forcings and the cmdsta forcing masks still
clear on reset. `fault-clear` (an id or "all") sets the inactive value
back and rejects unknown ids; `faults-active` reads "id=value,..."
("" when none). Every accepted set or clear emits trace
`stixatt_fault(id, value)` (redundant sets included, and once per
member of a `fault-clear "all"`), so the controller's JSONL can mirror
it. Sets are rejected with an error while `att-model=off` and on
out-of-range parameters. A mechanism fault set or clear mid-move folds
the position at the boundary under the old faults and re-solves the
end from now (piecewise, D-Ag); the ramp end stays anchored at the
command, so a mid-move inject never extends the drive past it — a
target end whose debounce no longer fits inside the window ends at
the ramp end with the timeout semantics. The platform catalogue
(`scripts/stix/platform/catalogue/faults-v1.json`) carries the nine
rows with `qom` actions; `expected_detection` stays empty until a live
PFM run confirms the outcome table (all [L]). The catalogue's schema
has no per-fault "description" field, so none was added.

A-3 qtests in the same block: one per catalogue row
(`att-fault-sensor-ab-open`, `-ab-closed`, `-bc-open`, `-bc-closed`,
`att-fault-motor-dead`, `att-fault-jammed`, `att-fault-current-zero`,
`att-fault-current-offset`), plus `att-fault-midmove` (set at 250 ms,
cleared at 700 ms, the end commits at 710 ms with the fault and end
traces checked), the mid-move set+clear pair for the truth rows
(`att-fault-m1-dead-midmove`: frozen at 500000 ppm from the 250 ms
set to the 500 ms clear, the end re-solved to 760 ms;
`att-fault-jammed-midmove`: the same fold with the 320 stall, the end
re-solved to 610 ms), `att-fault-window-end` (a stuck-closed set at
1595 ms ends at the 1600 ms ramp end with TIMEOUT, never past it),
`att-fault-reset` (the fault survives, the masks do
not), `att-fault-trip` (the set does not sample, the second conversion
trips), `att-fault-reject`, `att-force-cmdsta`, `att-fault-verbs` and
`att-fault-hw-state` (six hw-state polls across one move, each
asserting the attenuator object). RED before A-3: the properties are
unknown to
qom-set and 0x5C reads carry no forced bits. With no fault set the
model is untouched: the getters do not trace and the boot/trace
goldens stay identical.

A-4 adds the probe extensions and the gate. `tmtc-probe.py` (each
change its own hunk after D-3's `--dump-apid` work, self-tested):
`--expect-return VALUE` attaches to the latest `--qmp-cmd` group and
fails the run unless the return equals VALUE — JSON when it parses (a
qom-get uint64, bool or string), the raw string otherwise;
`--forbid APID:T:S[:HEX]@SECONDS` disarms the forbid that many seconds
after its group's send, and without the suffix the forbid stays armed
for the whole run as before. The gate is `scripts/stix/att-smoke.sh`
(slot mode per §41: `STIX_PORT_BASE=<k|auto> scripts/stix/att-smoke.sh
<qemu> [--negative|--check-guard|--dry-run]`; legacy mode leaves the
21000 flock to the caller) with the SID2 checker
`scripts/stix/att-sid2-check.py`.

**(d) The gate record — PASS and SKIP listed separately.** att-smoke
decides the ELF flavour positively: `nm` finding a symbol whose name
contains `moveStartCoarseT` (suffixed, `.555127` on the gate ELF) means
AX; otherwise the script boots once and reads `fsw-accesses` over QMP
after the boot — 0 accesses on a non-AX ELF is an environment error
(exit 2), so a stripped or `HW_VERSION_FSW_IDPU` ELF cannot pass as
PFM. On the gate ELF only the AX subset runs; the record line lists the
parts, and the process exits 77 (the SKIP convention, the README's
exit-code table):

    ATT-SMOKE: PASS (AX subset), SKIP (PFM sequence), SKIP (silence run)

Lived 2026-11-12 on the gate ELF: 0x14A3, then `9434 06 0334 02EF`
inside the 5 s window after the @35 `current.offset` = 800 set, and
`fsw-accesses` = 0 at +45 s — the att → presented → comparator wiring,
live on AX. The `--negative` (the set skipped, the inactive value 0)
passes only when exactly the 0x9434 expectation goes unmatched;
`--check-guard` runs the run-dir guards without booting.

The PFM sequence and the silence run are implemented but unconfirmed
live (no PFM ELF, Q1): every PFM payload stays [L], and there is no PFM
UART golden (att-smoke's expectations are its own). The silence run's
SID2 evidence is `att-sid2-check.py --expect-bc-closed` on the probe's
`--dump-apid` dump of 0x5A4 and expects **status4 bit 13 (BC) set and
bit 14 (AB) clear** (`HKAcquire.c:378-379`), not the plan's original
"both bits set": the model ends the stuck-closed move one 10 ms debounce
off OUT (the drive runs the debounce, Doc vs code 12), which releases
the healthy AB. The PFM sequence still uses the fixed connect-relative
offsets although the flash-latency handoff voided them (the plan's
"Blocking change"): re-anchoring on FS-ready (0x0032, then 0x142E) is
open (A-later).

**Known limits:** no thermal model (ATT_T static 2450, ATT_V derived
from LVEN); LV sampled at the command, not continuously [A]; the switch
polarity is assumed, not documented (Q5: `stuck_open` = the bit reads
0); the FPGA semantics behind the knobs — stop-on-switch, the debounce,
the timeout/finished relation, override, FAILFLAG, the travel time and
the currents — are inferred [I] (Q2-Q5); AX blindness: only
`current.offset` reaches the FSW on the gate ELF, and `fsw-accesses` (0
there, not cleared on reset) is the R5/Q9 label that keeps the AX
artefact out of the mismatch reports; and the two TM(1,7) per TC move
(correction 3) are never used as evidence that a move ended.

## 44. Flash damage injection (baseline: the NAND is always perfect)

Plan: `2026-11-12-flash-damage-plan.md` (X-1..X-5, each with its
"As built" note; those notes are the source of truth here). The
operator recipes are in `ground-station.md`, "Fault injection" →
"NAND flash damage"; the gate is in the suite list of
`baseline/README.md`.

**Baseline:** nothing could damage the NAND. Erase and program always
succeeded, every read returned the stored bytes bit-exactly, and a
flash image stayed perfect forever: no bad blocks, no stuck cells, no
corrupted files. The FSW's grown-bad-block marking, BCH correction and
corrupted-file paths all existed but could never fire. With no damage
configured nothing changes (D-X6): one `damage == NULL` check per op,
no `stixflash_damage_*` trace line, status/timing/IRQ behaviour
unchanged, the goldens byte-identical (qtest `flash-dmg-quiet`).

**Model (X-1/X-2, `grlib_stixflash.c`):** one per-instance damage
list, two sources, one entry grammar.
- Kinds: `erase-fail` (block scope), `program-fail` (page), `stuck`
  (page with `bit=<byte>.<bit>`, or whole block = bit 0 of every byte),
  with `value` 0 or 1 (O4: stuck cells have both polarities; `stuck0`
  stays as the value-0 alias and takes no `value`). After the parse
  every entry is canonical (mcm/chip + the page inside the chip).
- All damage applies at `stixflash_complete()` only: a failing erase or
  program still takes its full latency (§42's timer, on by default),
  then completes FAILED|READY with one IRQ and commits nothing — the
  FSW sees a normal slow op that failed. `stuck` applies between the
  file read and the DMA, on every read, so it also corrupts a freshly
  programmed page, like a real stuck cell. The guest-facing failure
  walks the same `stixflash_error why=erase-fail|program-fail` line as
  every other failure.
- **The grown-bad-block marker is the FSW's own (correction 1):** a
  failed erase/program inside `File_write` makes the FSW erase the
  block and program `badBlockMarker = 0` into pages 0 and 1 itself
  (`BSW_ASW/src/BSW/Flash/File.c:582-598`), so the device never marks
  anything and the marker the FSW writes persists in the image across
  reboots — the factory→grown lifecycle of real NAND with one
  mechanism. Two documented nuances: the marking erase also fails under
  erase-fail and the marker program lands anyway (the model overwrites
  pages cleanly, `(void)Erase_block`, `File.c:592`; real NAND could
  stay unmarked — accepted); and a program-fail on both marker pages
  defeats the marker, while one on a single page leaves the other
  (the scan marks BAD when either page carries one,
  `File.c:318-339`). The gate's check part injects page 0 of the
  one-page context store and the persistence still shows (X-4 As built
  5).
- Traces: `stixflash_damage_load(n, path)` once after a successful
  sidecar parse (n = 0 for an empty `"damage"` array),
  `stixflash_damage_hit(kind, page_address, cmd_sta)` once per hit
  (kind `stuck0|stuck1` by value). Both silent with no sidecar or no
  runtime entries.

**The surface (D-X3):**
- Sidecar: qdev `flash-damage-map` (string, default "" = none), e.g.
  `-global grlib-stixflash.flash-damage-map=<run dir>/flash.bin.dmg`,
  parsed at realize. JSON `{"v":1,"damage":[...]}`, a small file next
  to the flash. A missing file is silence (ENOENT only — any other open
  error fails realize), a malformed one fails realize naming the path
  and the reason, over 1 MiB fails realize
  (`flash-dmg-map-parse`/`stix.c`).
- Runtime verbs on `/machine/stixflash` (added in instance_init, so
  they exist before realize): `damage-add` (write-only; one sidecar
  `"damage"` entry as a JSON object **string**), `damage-remove`
  (write-only; `all` or the exact spec), `damage-list` (read-only). A
  spec that is already active (map or runtime) is rejected ("the spec
  is already active"); the list is capped at 65536 entries (map +
  runtime); a rejected set errors and changes nothing (D-Ag).
  `damage-remove` matches by the canonical record, so any written form
  of the same spec (linear or mcm/chip, `stuck` or `stuck0`) removes
  it; only `src=runtime` rows are removable — `all` drops every runtime
  row and is idempotent, the sidecar rows are boot configuration and
  only go away by rebooting without the sidecar.
- Reset: the entries survive a guest/system reset (they model broken
  cells; the reset handler touches only the op state). A reset drops a
  pending op before completion, so an aborted op can never take a hit
  (D-F4, pinned by `flash-dmg-reset`); an add/remove while an op is
  pending takes effect at that op's completion.
- Reserved rule: the containing linear block must be >= 64 (the
  ASW-image area is not FSW-visible on the gate ELF,
  `MemFilesystem.c:43-44`). The sidecar has no override; `damage-add`
  honours the `allow-reserved` qdev bool (default false);
  `damage-remove` is never blocked (removing injects nothing).
- `damage-list` rows (the controller's read-back truth), FSW-linear
  index space, `\n`-separated, map rows first (sidecar order) then
  runtime rows in add order:
  `src=map|runtime kind=erase-fail block=<linear>`,
  `src=... kind=program-fail page=<linear>`,
  `src=... kind=stuck value=<0|1> page=<linear> bit=<byte>.<bit>`,
  `src=... kind=stuck value=<0|1> block=<linear>`.

**The grammar (both addressing forms):** JSON keys `kind`, `block`,
`page`, `mcm`, `chip`, `bit`, `value`; either the FSW-linear form
(`block` or `page` alone) or the unit-test framework's `mcm`+`chip`
with `page` (chip-relative), `block` (in the chip) or `block`+`page`
(the argument list of `Flash_Curr_Img_DirectWrite`,
`BSW_ASW/src/BSW/Flash/Flash.c:280-310`: linear page =
`mcm<<21 | chip<<18 | block*64 + page`; the framework's `offset=<o>`
has no sidecar key — the tool resolves it to the bit `offset.bit`).
`bit` is stuck-only, `"byte.bit"` with byte < 4224 and bit < 8; unknown
keys are rejected. One parser serves sidecar and verbs
(`stixflash_dmg_entry`); the tool's `validate_spec` mirrors it so the
tool never writes a map the device would reject at realize.

**The offline tool (X-3, `scripts/stix/flash-damage.py`, stdlib only):**
- `bad-block --block N` (or the framework form `--mcm M --chip C
  --block B`; `--fs-range` for every FS-scanned block: A 64-1983,
  B 32832-34751 with `LIMIT_FS_SIZE=32`) — marker 0 into the spares of
  pages 0 and 1, single-byte writes (the same guest view as the FSW's
  full-page marker program, sparser on disk).
- `corrupt-file --type T [--spare S --coarse C --fine F --duration D]
  --mode header|data|address` — locates the file by its 10-byte name
  (chunked spare-area scan of the FS regions, one file per block) and
  flips a deterministic pattern, each candidate verified with the
  tool's own BCH decoder before anything is written: `header`
  (uncorrectable first-page header — the file "does not exist" at
  boot), `data` (> 7 flips in one 528-B sub-block uncorrectable,
  `--bits` given; `--sub-block` selects it), `address` (review finding
  5, the TN-required "data block written at the wrong address" aspect:
  the spare `pageAddress` u32 rewritten to a wrong but valid page and
  sub-block 7's ECC re-encoded, so the read passes BCH and reaches the
  FSW's address check, `File.c:691-699`).
- `show --type T ...` — truth read-back: the file's block (linear and
  mcm/chip/block) and pages with `pageAddress`, `fileSize`,
  `logicalWriteTime`; `device` — writes the sidecar with the O4 stuck
  grammar. `--self-test` runs fully offline (two synthetic sparse
  images, both storage modes, its own negatives) and pins the encoder
  against the committed reference vectors.
- Guards: it applies the inversion itself (default; `--plain` for
  `inverted-storage=off`), never writes past EOF (the file size is the
  capacity, §7 D-H), re-checks the 64 MiB sparse cap after every edit,
  refuses the shared master dirs unless `--allow-shared`, and refuses
  blocks < 64 unless `--allow-reserved`.
- Names: context names carry the build's CONTEXT_VERSION in name byte 1
  (`ContextMgmt.h:11` says 35; the AX gate ELF stores 0x22), so
  `show`/`corrupt-file` take `--spare any` (FSW finding F18). The type
  codes are the sequential enum values — context0 = 6, context1 = 7,
  lut = 8; the header's per-line comments say 8/9 and are wrong
  (`FilesystemDefinition.h:81-92`).

**Flight-BCH facts the tool depends on (FSW finding F19).**
`encode_bch` byte-swaps every aligned 32-bit word unconditionally
(`MemoryMgmt/BCH.c:300-310`, the Linux `cpu_to_be32` hard-coded for a
little-endian host), so on the big-endian LEON the stored ECC is the
standard BCH of the **word-swapped** sub-block — and `BCH_decode`
applies its fixes at swapped-stream indices (`BCH.c:1480-1486`): a real
error in byte b is "corrected" in byte b^3, same bit — the decode
returns ok with fixes = 1 and the page reads back with the original
error plus the mirrored one. **The emulator does not model ECC at all**
(the model stores raw pages, the plan's correction 3); the guest does
(`File.h:106-122`, `MemoryMgmt/BCH.c`). The tool's `bch_encode`/
`bch_decode` are the flight forms; the standard core stays pinned by
`scripts/stix/tests/bch13t7-reference-vector.txt`, the flight forms by
`bch13t7-leon-vector.txt` (the master flash's stored header ECC plus a
LEON-faithful build of `BCH.c`). Consequence for the outcome rows: a
"correctable" flip is NOT corrected in flight, and the >= 4-corrections
rewrite path (`MemFilesystem.c:604-655`) stores the miscorrected data
with a fresh, valid ECC — the X-4b correctable flavour's expectations
must first be re-derived from this before it graduates (X-4b is not
implemented in v1, later).

**Boot-time context damage is fatal (FSW finding F20).** INIT waits for
the FILE task's context reads (`MemFilesystem.c:542-544`,
`:1038-1051`) before `TMBuffer_init` (`main.c:136-140`); the read
error's TM(5,2) 0x543B report then hits the uncreated TMBuffer
semaphores → ASSERT (`TcTm/TMBuffer.c:276`) → Halting → trap → reset,
every boot while the file is damaged. Under `-no-reboot` QEMU exits by
itself; the UART shows the read-error line ("address corrupt" for the
wrong-address mode, "Unrecoverable error in file" for the uncorrectable
one) and no "init: TM buffer"; the ASSERT's backtrace (0x543B in some
frame) is printed only sometimes and is reported, not pinned. **0x543B
is therefore not observable as TM for context damage at boot** — the
smoke pins the crash instead.

**Catalogue mapping (D-X5).** `scripts/stix/platform/catalogue/
faults-v1.json` reserves the `flash.*` IDs as whole-line comments next
to the attenuator's (the loader has no reserved/disabled-row form, so
comments are the only non-injectable form): the three device rows
`flash.block.erase_fail`, `flash.page.program_fail`, `flash.cell.stuck`
(class memory, `changes: "truth"`, `until_cleared`, action qom
`damage-add` with `damage-remove` as the clear on `/machine/stixflash`
— adopting them needs a platform amendment, hence comment-only) and the
three offline rows `flash.block.bad_factory`, `flash.file.corrupt`,
`flash.file.wrong_address` (setup-only, no `action`: pre-boot steps,
the planned `hooks.*` verb `flash.damage` running the tool).

**The gate (X-4, `scripts/stix/flash-damage-smoke.sh`):** positive +
`--negative`, AX gate ELF only (O1; a PFM flavour is opt-in later),
slot mode only (the offline damage edits a private copy of the master).
Four parts, each in its own run dir; the parts that need stored files
run a SEED first (a mode drive whose TC(236,6) param change makes the
NOMINAL-entry store re-write both context files, waited on CONTEXT1's
`logicalWriteTime`, then a clean QMP quit):
- **boot** (offline, before QEMU): `bad-block --block 610` (TMMGMT,
  blocks 604-783) + `corrupt-file --mode header` on a LUT file when the
  seed left one → 0x0032 `0032 01 0002 0001 <err>` (used=0002: the two
  context files; bad=1; error=1 only with the LUT damaged) and 0x5439
  only when the LUT exists — on AX a missing LUT file alone sends none
  (the golden UART has the four "could not find LUT" lines) [L]. No
  context damage here (F20).
- **check** (device, runtime): `damage-add` program-fail on page 0 of
  the first free CB0 block after the context files (show-derived at run
  time), `damage-list` read-back over QMP; the param TC (4094 vs the
  seeded 4095) makes the run's NOMINAL-entry store fire and burn the
  block; TC(238,6) `{0, 3839, 0}` (MAINTENANCE-only, entered via
  NOMINAL → SAFE → MAINTENANCE; `badBlockHandling=0` because with 1 the
  check erases the grown-bad block — a program-fail-only block erases
  fine, so it healed before the reset, seen live) → TM(238,7) on APID
  0x5A8 with bad_after=1 [L]; then a QMP `system_reset` and the second
  boot's 0x0032 shows bad+1 — the persistence point, D-X1 [L].
- **addr** and **stuck** (offline): `corrupt-file --mode address` /
  `--mode data --bits 8` on CONTEXT1 page 0 → the F20 boot crash, QEMU
  exits by itself.
- `--negative` runs the boot part undamaged and passes only when
  exactly the boot part's damage expectations are unmatched and nothing
  else failed. Every [L] payload was confirmed by a live probe run
  before it froze (X-4's "As built", seven live-driven changes: the
  file-type enum, the flight BCH, the seed wait on `logicalWriteTime`,
  the F20 crash, the check page 0, TC(238,6) MAINTENANCE-only, and
  `badBlockHandling=0`). The `--flavour` plumbing for the X-4b
  scenarios exists in the script, but the X-4b flavours are not
  implemented in v1 (later).

**Evidence:** 11 qtests behind `stix_flashdmg_register()`
(`tests/qtest/stix.c`), all RED on the pre-X-1/pre-X-2 binaries (the
property or the verb is unknown, so the first assert aborts): X-1's
`flash-dmg-erase-fail` (FAILED|READY after the full `tbers-us`, one
IRQ, the file bytes unchanged, the FSW-style marker program still
landing), `-program-fail`, `-stuck0`, `-stuck1` (O4: an erased page
unaffected, a programmed 0 on the bit reads 1), `-stuck-multi` (two
stuck bits on one page, the later entry wins), `-marker-plain` (the
inversion rule pinned in both storage modes), `-quiet`, `-map-parse`
(malformed sidecar fails realize, missing file fine); X-2's
`-runtime-add` (inject → full-latency erase fails with one hit →
remove the identical spec → erase succeeds), `-reset` (the entries
survive `system_reset`; the dropped pending op takes no hit),
`-reject` (unknown kind, reserved block with `allow-reserved` off,
non-JSON, unknown key, duplicate add, remove of an unknown spec or of
the sidecar's own spec — each leaving `damage-list` unchanged; then
`all`, and the `allow-reserved=on` positive). The tool's self-test
(197 checks, < 1 s) and the two committed BCH vector files pin the
offline side. The gate pins the FSW-visible outcomes; its [L] payloads
came from the live probe runs recorded in the plan's X-4 "As built".

## 45. Memory upsets and EDAC (baseline: memories never fail)

Plan: `2026-11-12-memory-upset-plan.md` (commits M-1..M-7; numbered
§45 at the merge, after the flash damage §44).

**Baseline:** memories never fail. The EDAC registers were plain
storage (only §5's 0x54 write masking existed), nothing could be
injected, and nothing counted, latched, interrupted or trapped; the
boot print read the reset zeros. EEPROM/PROM stays out of scope (the
boot image is immutable) and keeps its plain-storage register
behaviour.

- **The EDAC unit** (`hw/sparc/stix_edac.[ch]`, TYPE_STIX_EDAC at
  `/machine/stixctrl/edac`, a QOM child of the stixctrl device): owns
  the poison table and the sparse overlays, the SEC counters and
  DeDetected latches of the three EDAC memories (SDRAM, CACHE, ROTBUF;
  EEPROM is out of scope and stays plain storage), the injection
  interface and the `edac-at-boot` property (D-Uk). With nothing armed
  the unit is byte-invisible: no overlay exists, the register and
  window hooks early-return on an empty table, nothing counts up,
  nothing traces.
- **The injection interface (D-Ug).** All on `/machine/stixctrl/edac`:
  `upset-inject` (write-only string), `upset-clear` (`"all"` or the
  record id — the 0-based position in the armed list, the same order
  `upsets-active` lists them), `upsets-active` (read-only JSON: one
  record per armed poison — id, memory, address, mask, mode, persist;
  rotbuf records additionally carry `block` and `offset` — plus
  per-memory `injected`/`corrected`/`detected` counters) and
  `edac-enable` (M-3, below). A rejected set (unknown memory, unaligned
  address, bitmask 0 or wider than the word, a rotbuf block or offset
  out of range) is an error that names the offending field and changes
  nothing; a re-inject of an already-armed word replaces its record and
  counts a fresh `injected`. The counters are host-side statistics that
  survive a reset, like the poisons (the M-1 reset hook clears only the
  SEC/DE latches).
- **Register semantics (M-1).** The composed fields are read-only and
  read 0 until a commit counts an upset; the write-1 strobes self-clear
  and reset counter and latch:
  - `Sdram:Edac` 0x80000C04: SECCOUNT 7:0 and DEDETECTED 31 composed on
    read; EDACON/SCRUBRATE stored; the EDACRST strobe (bit 13) clears
    (the FSW `EDAC.c:294-297` set-then-clear pair).
  - `CacheCmdSta` 0x80000550: SECCOUNT 15:8 and DEDETECTED 18 composed;
    OPMODE/SCRUBRATE/EDACON stored; the EDACRESET strobe (bit 17)
    clears (`EDAC.c:299-302`).
  - `RotbCmdSta` 0x80000554: SECCOUNT 31:24 and DEDETECTED 4 composed;
    the 0x54 write filter now treats bit 2 as the strobe (its own
    comment anticipated this); the FSW's `~(1u << 2)` clear write
    (0xFFFFFFFB) still lands as RESETFIFO — the documented side effect,
    unchanged.
- **cacheCmdSta reset value (declared golden change).** The ICD reset
  puts EdacOn = 1 (STIX-ICD-0026 txt:2119); the baseline model read 0
  (plain storage, zeroed at power-on). cacheCmdSta now takes
  0x00010000 at every device reset. The boot UART golden changes on
  exactly one line — the boot print's `E_EDAC_MODULE_CACHE = 0`
  becomes `= 1` (`uart.golden.log` line 19). The trace golden changes
  on 8 lines (it logs read values and stored write values): the seven
  0x50 lines (boot print read, `RamUtils_setSRAMModeToCache` RMW,
  `EDAC_init` strobe pair — the strobe bit never appears in the stored
  word) and the first 0x54 strobe write (0x4 traces as stored 0x0).
  The UART golden is updated (boot-smoke diffed exactly that line before
  the update). The trace golden is a manual reference only (baseline
  README) and, as for the flash-latency track, is not recaptured; a
  future recapture shows these 8 lines.
- **OpMode and ScrubRate keep reset 0 against the ICD** (txt:2124 says
  OpMode 1, txt:2122 ScrubRate 3): OPMODE 0 keeps the CACHE DE→IRQ path
  live with no FSW setup (the `EDAC.c:69-76` HW comment: SRAM boots as
  main memory, ASW switches it to interrupt), ScrubRate 0 keeps the
  scrub algorithm's OFF early-return (`FDIR/EDAC.c:230-234`).
- **Sdram:Edac resets at device reset.** grlib_stixsdram gained its
  first reset handler: the edac register takes its ICD reset
  (EdacOn 0, ScrubRate 0, txt:3243-3253) at every device reset.
  configStatus is untouched (the INIT repair owns it). The device
  reaches the unit through a strong `edac` link set by leon3.c (the
  scet-source pattern); with no unit linked the register keeps the
  composed semantics with zero counters (`stix_edac_*` with
  `e == NULL`), with the ICD reset. Invisible to the goldens: nothing
  sets or reads SDRAM EDACON (correction 6 of the plan).
- **`edac-at-boot` (D-Uk).** The property stands in for the unknown
  SuSW hand-over state (open question Q1): `hw-reset` (default, the ICD
  values: cache on, sdram/rotb off), `all-on`, or a 3-bit mask
  (1 = sdram, 2 = cache, 4 = rotb). Applied at every device reset — a
  watchdog reboot re-loads it, like an FPGA re-loading its defaults.
  Settable over qom-set on `/machine/stixctrl/edac` or as
  `-global stix-edac.edac-at-boot=...` (applied at the first reset, so
  a live gate gets EDACON on from boot). The owners' device resets
  apply it (stixctrl for cacheCmdSta and rotbConfig, stixsdram for the
  edac register), not the unit's own reset hook: in this tree a hook
  registered during realize runs before the sysbus devices reset (the
  bus joins the root reset container at the end of
  `machine_run_board_init`, `machine.c:1771`), so the owners would
  clobber the bits; the unit's hook clears its counters, where the
  order does not matter. Live SDRAM/ROTB DE gates run `all-on` until
  Q1 is answered.
- **The overlay design (M-2, D-Uc).** Truth is the host RAM bytes; the
  flip lives in the poison table, not the backing bytes — an overlay
  read fetches the clean word from the backing RAM and XORs the mask.
  On the first poison in an aligned 4 KB page a priority subregion with
  `read_with_attrs`/`write_with_attrs` handlers covers that page (a
  plain `.read` would have won the dispatch); it is removed when the
  page empties, and a removed struct is parked on a freelist and
  reused, never freed inside its own read dispatch. All 1/2/4-byte
  accesses are forwarded (`valid.max_access_size` 8, the M-3 review
  fix: with 4 every guest ldd/std on an overlaid page was a silent
  decode error — the load returned 0, the store was dropped). A write
  touching any byte of a poisoned word is a read-modify-write of the
  EDAC word: the untouched bytes keep the flip (EDACON = 0, or a DE
  re-encoded as the cell's content), except an EDACON = 1 SEU, which
  the RMW's read half corrects without counting. Host-side writers go
  through the same rule: stix_det's LACC/EACC materialization calls
  `stixctrl_edac_sram_write` (`stix_edac_host_write`), so a QL swap
  retires every poison it overwrites, and the ROTB FPGA append (M-4)
  clears every block it writes. Traces: `stix_edac_inject`,
  `stix_edac_effect` (kinds `sec`, `ded`, `raw`, `write-clear`,
  `host-clear`), `stix_edac_clear`, `stix_edac_enable` — from the QOM
  verbs and real intercepted accesses only, never from a getter. Known
  bounded window: after an inject over QMP a running vCPU can still hit
  a stale RAM TLB entry for a few TBs (the flush is async), so a first
  read may be clean while the poison stays armed; the campaigns poll
  (the ROTB block window is MMIO and never cached in a RAM TLB entry).
- **One-shot and persist (D-Uh).** SEU poisons self-retire on the first
  read with EDACON = 1 (corrected + counted); a one-shot DE retires on
  the first *detected* read (with EDACON = 0, or — CACHE — OPMODE = 1,
  the flip is returned raw and the poison stays armed: nothing
  corrected the cell); `persist=1` keeps a poison armed through
  everything and models a stuck cell (boot-loop risk documented in the
  catalogue; no gate uses it). Upsets survive a system reset — a reboot
  does not heal a memory cell — and die at a power cycle. This differs
  from Phase-4's "injections clear with the device" rule (D-P4b) and
  follows the attenuator's physical-fault persistence (D-Ag).
- **The OPMODE gate (M-2).** For CACHE the DEDETECTED latch and the
  IRQ 6 pulse gate on OPMODE = 0 (cache mode) as well as EDACON = 1,
  per the IRQ-table comment (`StixIdpu.h:1547`); the DE *data* flip is
  returned regardless of OPMODE — the register comment gates the IRQ,
  not the data. The SEU path gates on EDACON only [I — the qtests never
  read an SEU with OPMODE = 1; one constant to change if the FSW side
  says otherwise]. cacheCmdSta resets with OPMODE = 0 (the accepted
  divergence below), so the CACHE DE → IRQ path is live with no FSW
  setup.
- **SDRAM: the AHBSTAT window and the trap (M-3).** The DE overlay read
  (EDACON = 1) latches DEDETECTED/SECCOUNT, latches the failing word
  address into a new 4-byte AHBSTAT window at 0x80000F00 (`StixIdpu.h`
  `SDRAM_AHBSTAT_BASE`; owned by the unit, mapped by leon3.c via
  `stix_edac_ahbstat_mr()`; reset value 0, the latch survives a system
  reset — the post-reboot record is the BRAM copy the FSW's
  `storeBadTrapInfo` made — and writes drop) and returns MEMTX_ERROR;
  the *data* under the error response is the flipped word (undefined on
  real hardware; the trap path never sees it). `edac-enable` on the
  unit (`"all-on"` or a 3-bit mask: 1 sdram, 2 cache, 4 rotb) ORs the
  EdacOn bits a software write could set into the register owners'
  stored words and never clears a bit; a module whose owner is not
  wired refuses the whole set (all-or-nothing, so a caller is never
  left with a silently missed module) and an empty mask is refused;
  write-only, traced (`stix_edac_enable`), not qtest-gated — it is a
  platform fault verb.
- **Behaviour change in `target/sparc` (M-3): a MEMTX_ERROR on a LEON3
  data access now raises trap 0x09.** `sparc_cpu_do_transaction_failed`
  (`target/sparc/ldst_helper.c`) raises `TT_DFAULT` (0x09) when the
  transaction failed with an AHB error response (`MEMTX_ERROR`), the
  access is a data access (not an instruction fetch) and the CPU is a
  LEON3 — discriminated by `CPU_FEATURE_CACHE_CTRL`, which only the
  LEON3 model sets (`cpu.c:544-546`) and which `ldst_helper.c` already
  uses as the LEON gate for the cache ASIs (`:602,:821`). The raise is
  independent of the MMU enable (ASW runs MMU-off; unassigned memory,
  `MEMTX_DECODE_ERROR`, keeps the MMU-gated behaviour, so the HWSelftest
  store to 0xF0000000 still does not trap). This is a behaviour change
  only for `MEMTX_ERROR`, which nothing returned before: every device
  mapped in leon3.c (stixctrl, stixsdram, stixflash, stixspw, irqmp,
  gptimer, apbuart, ahb/apb pnp) uses plain `.read`/`.write` handlers,
  which always dispatch as MEMTX_OK, and no machine sets
  `ignore_memory_transaction_failures`. The first (and only) producer
  is the EDAC unit's SDRAM DE overlay read.
- **Known limit, and the data-words-only rule (M-3).** An instruction
  fetch from a DE word with EDACON = 1 does **not** trap (the raise
  excludes `is_exec`): the flipped word is executed, the poison retires
  and AHBSTAT/DEDETECTED latch a code address — hardware would take an
  instruction-access error. SDRAM DE injections are therefore data
  words only; the catalogue's `memory.sdram.ded` row carries the rule
  in its preconditions.
- **ROTB upsets and the block:offset grammar (M-4).** The ring is host
  RAM the CPU sees only through the 512 B block window at 0xE0800000
  (the block at the read pointer), so there is no overlay:
  `stix_edac_rotb_check()` runs in the window read handler with the
  overlay read's semantics — SEU with EDACON = 1 returns the corrected
  word (nothing to write back: the ring already holds it) and counts
  SECCOUNT (rotbCmdSta 31:24); DE returns the flip, latches DEDETECTED
  (bit 4) and pulses the shared IRQ 6; ROTBUF never errors the bus (no
  trap path). Grammar: `rotbuf,<block>:<offset>,<bitmask>[,mode]
  [,persist]` — block 0..rotb-blocks-1 (bounds from the ring size, the
  error names the bound), offset 0..511, 4-aligned; the poison carries
  the ring byte offset `block * 512 + offset` as its address and the
  traces log that offset. The FPGA append (`stixctrl_rotb_append`)
  clears the poisons of every block it memcpy's (the D-Uc write rule), a
  CPU store through the window (the TESTMODE fill path, where the store
  is not ignored) clears the words it touches, and the EDACRESET strobe
  needed no new code (M-1's 0x54 filter already treats bit 2 as the
  strobe) — poisons survive the strobe and the FIFO commands (clearing
  the status is not curing the cell).
- **BRAM silent flips (M-5).** `bram,<address>,<bitmask>` — an absolute
  guest address in the 0x400 lvar window 0xE0200000..0xE02003FF (LVAR0-3
  included), any alignment (there is no EDAC word), the mask XORs the
  four bytes at the address big-endian; `mode`/`persist` are refused
  (a flip is silent by construction) and each accepted inject flips
  once (a repeat flips again). No poison record, no counters, no
  status, no IRQ: the flip is applied at inject time to the host bytes
  (`memory_region_get_ram_ptr` + `memory_region_set_dirty`, the
  stix_det pattern) and `upsets-active` stays byte-identical (its
  counters remain sdram/cache/rotbuf); the only trace is the inject's.
  The flip survives a system reset and dies at a power cycle (the lvar
  window is plain RAM, zeroed at power-on only). The FSW-visible
  outcome is the WDT `UNCHANGED_STORAGE` record at 0xE02000A8, read at
  every boot (`WDT_callAfterReset`): garbage `sramTestResult` or a
  spurious `WDT_RESET_TRAP` [L — nothing in ASW formats the record
  into TM yet; the FSW clears the record after reading, so exactly one
  boot is affected]. The MemBram record at 0xE02003C0 has no ASW reader
  at all (setters only) — mechanism regression only.
- **Seeded random bursts (M-6a).** `random-seed` (uint32, default
  0 = off — seed 0 refuses `random-count` — plain state that survives a
  reset) and `random-count` (write-only; the burst fires on the set)
  draw deterministic one-bit upsets: xorshift32, Marsaglia's 13/17/5
  triple, the state restarts from the seed at every set, and each upset
  costs three draws in a fixed order — the memory (draw % the wired
  EDAC memories, in enum order sdram/cache/rotbuf; BRAM is never drawn:
  no EDAC, no record to observe), the word (draw % the extent,
  4-aligned; the backing region's size for sdram/cache, the ring bytes
  for rotbuf), the bit (draw % 32 → mask 1u<<bit, so every drawn upset
  is a one-bit SEU; `mode`/`persist` are not reachable). A word already
  poisoned is skipped and the attempt's three draws are spent; the
  burst gives up after 1024 attempts with the placed count named.
  Refusals (count 0, seed 0, a count over the free table slots —
  all-or-nothing, no memory wired) change nothing. The model stores
  only the 32-bit data word (the EDAC check bits are implicit in the
  correction machinery), so every drawn bit is a data bit — a random
  SDRAM upset may land in a code word and correct silently on the next
  fetch (SECDED corrects instruction fetches). A drawn upset arms
  through the same `stix_edac_poison_arm()` tail as `upset-inject`:
  same overlays, counters and traces. Repeating the same seed skips
  the words it already poisoned and continues the draw sequence into
  fresh ones.
- **The `hw-state.edac` section (M-6a).** One accessor
  (`stix_edac_hw_state`) appends the section after `det` — the same
  JSON as `upsets-active`, which became a thin wrapper over it. Pure
  like every hw-state section (reads the table and the counters,
  touches nothing, never traces). The `/machine/stixctrl` root `schema`
  is bumped 1 → 2 (the layout changed; the SPW hw-state stays at 1) —
  the declared impact.
- **The catalogue (M-6a).** The eight rows of the plan's fault table
  are entries in `scripts/stix/platform/catalogue/faults-v1.json`
  (`class: memory`, `changes: truth`), the qom action on
  `/machine/stixctrl/edac`, property `upset-inject`, the fixed upset
  riding as a string `value` (one line in `faults.py` widened the qom
  `value` to `(int, str)`; the loader's action key is `path`). They are
  one-shot events (`transient` for the sdram/cache/rotbuf rows,
  `permanent` for the bram rows — the obt.jump precedent),
  `reversible: false` — the generic qom action can undo only the
  property it set and `upset-clear` is a different property, so the
  plan table's "reversible" is the device's (`upset-clear`, a rewrite,
  the first corrected read), not the platform's — and each carries a
  dead but required `"clear": 0` (without an action `clear` the apply
  would qom-get the write-only property). `memory.random.burst` is not
  an entry: one qom action sets one property and the burst needs seed +
  count; the ground-station recipes cover the manual flow.
- **The live gate and its results (M-6b).** `scripts/stix/edac-smoke.sh`
  (the fault-smoke skeleton: slot library, the canonical run-dir guard
  and the QMP socket length check, `--dry-run`/`--check-guard`, the
  probe started first with QMP connected at the first due
  `--qmp-cmd`, log tails before every verdict, every check recorded so
  the negative can compare the set). The run is deliberately non-default
  and **not a golden change**: `-global stix-edac.edac-at-boot=all-on`
  makes the boot print read `E_EDAC_MODULE_SDRAM = 1` and
  `E_EDAC_MODULE_ROTBUF = 1` (`main.c:122-125`) under grep-only UART
  checks, and it is the only STIX gate without `-no-reboot` (the
  injected DE must reboot the guest). **Live record (2026-11-12): the
  SDRAM part passes** — the inject word `sdram,0x4008337C,0x3` is a
  data word, the `hkLineLimits[7]` entry (symbol at 0x40083344, nm on
  the AX gate ELF; entry 7 starts at 0x4008337C), read every second by
  `HKMonitor_regularCheck`; the gate checks the layout live with HMP
  `xp /4wx 0x4008337C` and requires `4008337c: 0x00000fff 0x00000003`
  (entry 7 = {min 0, max 0x0fff, count 3}; the live run pinned exactly
  that). Within ~1 s of the inject the UART prints `Unexpected trap
  (0x09)` and `data_access_exception`, the FSW forces the watchdog, the
  model resets the guest, and the second boot reports 0x0022 and 0x14A3
  (exactly one trap print and one reboot — the one-shot default, no
  boot loop). **No SAFE cache reader — the cache step is dropped from
  the v1 gate:** the M-6a pre-live design expected SID2 status1 bits
  2:0 = 1 in SAFE; the live run resolved that [L] negatively, as the
  code review predicted — nothing reads the CACHE SRAM in SAFE (the QL
  LACC reads and the TLUT re-trim are NOMINAL-gated; the HK path reads
  only ADC lines and SDRAM-resident state), so a CACHE DE is never
  latched there. The mechanism stays pinned by the M-2 qtests; a
  NOMINAL-window cache leg is a later item (it needs the nominal-smoke
  TC sequence, ~+70 s). The catalogue's SDRAM rows moved from the M-6a
  words 0x40001000/0x40002000 (both inside `.text`,
  0x40000000..0x4007F100) to the `hkLineLimits` data words
  (`0x4008337C` ded, `0x40083374` seu — entry 6, also read every
  second), and `memory.cache.ded` gained the NOMINAL precondition. No
  0x542F SEU step (Q4, the 1000 s report period).
- **Tests.** The `stix_edac_register()` block in `tests/qtest/stix.c`:
  `edac-reg-reset` and `edac-reg-readonly` are RED on the pre-M-1
  binary (cacheCmdSta read 0; the composed fields and the 0x54 strobe
  bit came back from plain storage); `edac-boot-state`, `edac-strobe`
  and `edac-rotb-strobe-quirk` are red-by-construction (the property
  and the strobe semantics are new). The strobes are exercised with
  forced latches via the unit's test-only `test-latch-*` properties.
  All later tests are red-by-construction on their predecessors (no
  verbs, no overlay, no properties before their commit): the nine M-2
  cache tests (`edac-cache-seu`, `-seu-raw`, `-ded`, `-opmode`,
  `-tlut`, `-persist`, `edac-inject-reject`, `-subword`,
  `-swap-poison`), the M-3 SDRAM tests (`edac-sdram-seu`, `-seu-raw`,
  `-ded-ahbstat`, `-ded-trap-runtime` (the trap must land in the
  guest's tt 0x09 slot at TBR+0x90, `int32_helper.c:163`, with AHBSTAT
  holding the failing address), `-persist`, and the review's
  `-dword` for the 8-byte overlay accesses), the M-4 ROTB tests
  (`edac-rotb-seu`, `-ded`, `-fpga-block`, `edac-enable-all`), the
  M-5 BRAM tests (`edac-bram-wdt`, `-flip`, `-reject`) and the M-6a
  tests (`edac-random` — it carries a reference implementation of the
  pinned draw, checked against canonical xorshift32 outputs computed
  outside the test — and `edac-hw-state`).

## 46. tm-hold: TM kept whole until the first client (baseline: dropped silently)

Plan: the gate speed-up plan §3 (P0b + P0a), kept with the gate tooling
outside this repo (`~/dev/stix-state/tools/gates-speedup-plan.md`).
Measured evidence (V2 of that plan): two parallel gate legs failed with
"0x0022 not matched" because the TM probe connected after the boot TM
was gone — APID 0x5A7 sequence 0 (0x0022 Application SW boot success)
and 1 (0x0021 Mode transition success) are emitted ~0.3 s after QEMU
start, the probe retried the connect every 0.5 s, and `stix_tmtc_send`
dropped TM with no client connected. A pre-existing race in every
probe-based smoke; parallelism only made it likely enough to see.

- **The property** (`-global grlib-stixspw.tm-hold=on`, default **off**,
  gate-only): with `tm-hold=on` and **no client ever connected yet**,
  `stix_tmtc_send` appends each TM packet **whole** to the existing
  out-queue (`TMTC_OUT_QUEUE_SIZE`, 256 KiB) instead of dropping it.
  All-or-nothing: a packet that does not fit whole is dropped whole and
  counted in the existing `tm-dropped` counter (the same rule the
  connected path applies). On the **first accept** the device arms the
  client read+write handlers, then sets a "first client seen" latch,
  then flushes the queue; a flush remainder on EAGAIN is finished by
  the armed writable handler (flushing before arming can strand held
  TM). The latch is set even if the flush fails and closes the client:
  after the first client has gone, the behaviour is the baseline
  no-client silent drop again (not counted).
- **Scope.** The hold belongs to the TCP transport: it is copied into
  `StixTmtc.tm_hold` at realize only when the TMTC TCP server starts
  (`tmtc-server=on`, or auto outside qtest — the smokes' case). With
  the server off there is no out-queue to hold anything in, so
  `tm-hold=on` alone changes nothing; the UDP transport (§21) is
  untouched. The platform controller's live sessions
  (`scripts/stix/platform/stixplat`) run `tmtc-server=off` and never
  pass the property: default off leaves the platform and every live
  session byte-identical.
- **Reset behaviour.** The hold and the latch survive a machine reset:
  `grlib_stixspw_reset` keeps the whole TMTC transport (P2-1, §14), and
  the queue and latch live in it. A reset before the first connect
  therefore delivers both boots' TM at the first connect (pinned by
  the `tm-hold` qtest's reset case).
- **Why gate-only.** The smokes pass the property in one shared place:
  slot mode's `STIX_SLOT_QEMU_ARGS` (`scripts/stix/lib/slot.sh`,
  stix_slot_setup) carries `-global grlib-stixspw.tm-hold=on` next to
  the slot's `tmtc-port`, so every smoke that boots a probe — and only
  slot-mode boots (the gates all run in slot mode) — holds the boot TM
  until the probe connects. Legacy mode (`STIX_PORT_BASE` unset) keeps
  today's behaviour.
- **P0a** (`scripts/stix/tmtc-probe.py`): the connect retry interval is
  20 ms instead of 500 ms (only the TM connect loop; the QMP client's
  own retry is untouched). Alone not a proof — with the hold it only
  shortens the wait for the listen.
- **Connect-time forbids.** Checked per smoke (`file:line`, the group
  each forbid arms with): no connect-time `--forbid` matches the boot
  TM the hold now delivers — 0x0022 and 0x0021 (TM(5,1) 0x5A7),
  0x0032, 0x8030 (only on a fresh flash), 0x14A3, 0x142E and HK seq 0.
  elut-smoke run 2's connect-time `--forbid 0x5A7:5:3:8030` gets
  stronger (it now actually sees boot TM), as intended: run 2 boots
  with the stored context, so a boot 0x8030 there is the failure the
  forbid exists for. The `--forbid 0x5A7:5:1:0021` in att-smoke and
  every mode-transition forbid attach to later send groups and ignore
  what arrived before their arm instant.
- **Tests.** `tests/qtest/stix.c` `/sparc/stix/tm-hold` (one QEMU per
  case, `tmtc-server=on` explicit): off = silent drop, no count;
  on = three pre-connect sends delivered whole and in order at the
  connect, nothing after the first client; the full queue with a
  1024-byte-rcvbuf client (the flush's EAGAIN remainder still arrives
  in order); 66 held packets — the queue takes exactly 64, two dropped
  whole, `tm-dropped` = 2, the stream packet-aligned; the hold across
  `system_reset` delivers both boots' TM. RED on the pre-P0b binary:
  `tm-hold` is an unknown `-global` property, so QEMU dies at start
  (the off case pins the unchanged default on both).

## 47. gdb-legacy: stop reply for old GDB (baseline/upstream: "T05core:NN;")

**Baseline and upstream:** the system-mode gdbstub answers GDB's initial
`?` query with `T05core:NN;` (`gdbstub/system.c`,
`gdb_build_stop_packet`). GDB before 7.1 does not know the `core` field
and rejects the packet ("Malformed packet(b) (missing colon): re:01;"),
so the STIX toolchain's debugger, `sparc-rtems-gdb` 6.8 from the RTEMS
4.10 Windows build (`ASW\RTEMS-4.10\bin\sparc-rtems-gdb.exe`), cannot
attach.

- **The option:** `-machine leon3_generic,gdb-legacy=on` (a machine
  property; with `-M leon3_generic` already given, `-machine
  gdb-legacy=on` merges into it). The `?` reply then is
  `T05thread:NN;` (the thread id as the other stop replies format it,
  `gdb_append_thread_id`). Default off: upstream behaviour, byte for
  byte. Modern GDB (15.1 tested) accepts both forms; only this packet
  changes.
- **Checked live (2026-11-13):** with the option, `sparc-rtems-gdb` 6.8
  attaches, breaks at `Init`, continues, backtraces, shows locals and
  steps on the AX gate ELF; without it the old error stays. Its
  remaining warnings (packet size limited to 16384, no XML target
  description) are harmless.
- **Platform controller:** add it per config,
  `qemu.extra_args = ["-machine", "gdb-legacy=on", "-gdb",
  "tcp:127.0.0.1:1234"]`.
- **Test:** qtest `gdb-legacy-stop` asks `?` over a raw gdbstub socket
  in both modes (under `-accel tcg -S` with a dummy BIOS: the qtest
  accelerator has no gdbstub) and reads the property back.
- **Debugger note:** GDB's `kill` (Visual Studio "Stop Debugging", the
  end of a `-batch` run) makes QEMU exit ("Terminated via GDBstub");
  detach instead to keep the bench running.
