# Flash latency plan: NAND operation latency so the PFM build boots

Status: validated 2026-11-11; review folded in (see "Review
resolutions"); uncommitted. Base: track
worktree `~/dev/qemu-stix-flash` (branch `stix-uprev-flash`, HEAD
`9c7bd11322`). Goal from the user: the PFM flight build
(`ASW_pfm_v186.elf`, GitHub release v186) must boot in the emulator. Today
the emulated watchdog resets it at about 6.8 s.

Sources:
- the model: `hw/sparc/grlib_stixflash.c`, `grlib_stixspw.c` (the
  re-entrancy guard of `a83fe9a3a5`), `grlib_stixctrl.c` (watchdog),
  `leon3.c`, `tests/qtest/stix.c`, `scripts/stix/*.sh`;
- a read-only pass over the FSW tree
  `/mnt/c/Users/laszl/Development/stix_asw_src/ASW` (the tree the v186 ELF
  was built from; its DWARF paths point there). Paths are relative to it:
  **BSW** = `BSW_ASW/src/BSW/...`, **ASW** = `ASW/src/...`;
- the FSW team's TSIM I/O flash model
  `~/dev/STIX-FSW/TSIM_IO/ASW_UT/src_io/iom/FlashApiTsim.c` (also
  synchronous; it has no timing to borrow);
- the measurements below (2026-11-11). They ran in throwaway run dirs
  `/tmp/stix-flashplan-1` and `-2` (now deleted), with TMTC ports
  21650/21662 and gdb ports 22650/22661, the binary
  `~/dev/qemu-stix-tc/build/qemu-system-sparc` (built at `9c7bd11322`)
  and a fresh sparse flash each time;
- two PFM ELFs, kept in `/home/laszlo/dev/stix-elf/` (outside any repo;
  each with a `.sha256` file, plus a `PROVENANCE.md`; the measurements
  above ran them from `/tmp/stix-elf/`, same bytes):
  - **release** `ASW_pfm_v186.elf` (GitHub release v186): no DEBUG_PRINT,
    so no UART;
  - **debug** `ASW_pfm_dbg.elf`: built by the user 2026-11-11 21:11,
    sha256 `116b5089ebf4a093e94b4922a53bee2eb7d4fd1d7b80ded2b2803db076de8310`.
    It is PFM + DEBUG_PRINT + **TRACE_TO_FLASH** ("Build production
    GU/PFM debug", `tasks.vs.json:74`; to be confirmed by the user):
    `debug_flush` calls `MemCommon_flashErase/Write` (`debug.c:144-171`)
    and the live task table has no debug task. Not TRACE_TO_TM.
    It has the full FS (`memFilesystemLocal` 0x134b10) and no AX stub
    symbols. Its UART shows progress.
  - Coordinator's run of the debug ELF: 43 UART lines; main init runs
    through `init: SAFE mode` at 00:00:00.127, then nothing more, then
    the watchdog reset at ≈6.6 s. This matches the root cause below: INIT
    blocks right after SAFE entry.

Fact tags:
- **[C]** read in code (FSW or model), with file:line;
- **[M]** measured today in a throwaway run (gdb, MMIO trace, TM probe or
  host micro-benchmark). The numbers come from one WSL host and are
  host-dependent;
- **[G]** seen in a committed gate artefact (`docs/stix-port/baseline/`);
- **[L]** to be verified live in the commit that relies on it;
- **[I]** inferred;
- **[A]** an assumption, here mostly datasheet timings that are not in the
  FSW code. Nothing is gated on an [A].

## Goal

1. Flash commands take virtual time. A page read, page program and block
   erase each keep BUSY set for a configurable latency. Completion (data,
   status and IRQ) comes from a `QEMU_CLOCK_VIRTUAL` timer.
2. **The PFM ELF boots:** no watchdog reset, the filesystem-init event
   TM(5,1) 0x0032 arrives, HK TM keeps flowing, and NOMINAL is reachable
   by TC.
3. The AX gate ELF and all existing gates and goldens keep passing. The
   boot UART golden is expected not to change (see "Goldens").
4. A new gate, `scripts/stix/pfm-boot-smoke.sh`, proves 2. Its
   `--negative` mode proves the gate catches the regression (latency off
   gives a watchdog reset).

## Corrections to the brief (measured root cause)

The brief's hypothesis was "the high-priority init task never yields and
starves the watchdog task". The mechanism is real but more specific:

- **The filesystem init task is the lowest-priority task, not a
  high-priority one.** `FILESYSTEM_TASK_PRIORITY` is 250 and
  `WATCHDOG_TASK_PRIORITY` is 10 (`ASW/configFSW.h:269, 283` [C]). The
  comment above them says the watchdog "has highest priority" so the
  system never resets (`configFSW.h:263-268` [C]).
- **Priority inheritance lifts FILE from 250 to 1.**
  - The FSW redefines `RTEMS_DEFAULT_SEMAPHORE` as a binary semaphore with
    `RTEMS_INHERIT_PRIORITY` (`configFSW.h:199` [C]).
  - The filesystem API mutex `mutexAPI` is created with it
    (`MemoryMgmt/MemFilesystem.c:477-481` [C]). The init task holds that
    mutex for the whole scan of every partition (`lockAPI` at
    `MemFilesystem.c:1022`, `unlockAPI` at `:1060` [C]).
  - INIT (priority 1) blocks on `mutexAPI`. The gdb stack walk at 3.5 s
    [M] shows:
    `Init+368 → OpModeMgmt_initMode → ContextMgmt_init → store_new_context
    → FSdelete → rtems_semaphore_obtain`.
    INIT's `Wait.id` equals `mutexAPI` [M]: 0x1a010003 in the release
    ELF, 0x1a01000b in the debug ELF (the ids are build-specific).
  - So task FILE runs with current priority 1 and real priority 250 [M:
    the RTEMS task table].
- **FILE never blocks, because every flash op completes inside the MMIO
  write.**
  - `Flash_readPage` masks the flash IRQ, starts the op, unmasks the IRQ
    and returns `E_FLASH_RES_OK_DELAYED` (`BSW/Flash/Flash.c:303-323`
    [C]).
  - The model already pulsed the IRQ during the `cmdSta` write
    (`grlib_stixflash.c:339`, executed from `:514` [C]). The ISR therefore
    runs at the unmask and sends the event
    (`MemFilesystem_callbackFlashOperation`, `MemFilesystem.c:547-556`
    [C]) **before** `Read_page` waits for it
    (`rtems_event_receive`, `MemFilesystem.c:1187` [C]).
  - The receive returns at once and FILE never leaves the CPU.
  - WDG (priority 10) is READY (state 0), and its 1 s
    `rtems_task_wake_after` has expired: ticks advance normally, 651 at
    3.5 s [M]. But it never runs.
  - Watchdog is fed twice: once in `Watchdog_setRefreshInterval`
    (`HWControl/Watchdog.c:70, 133`) and once at the task's first loop
    (`:97`) [C]. The reset follows 63 × 105 ms ≈ 6.6 s later [M: fed at
    +0.0 s, "count expired" at +6.87 s].
  - Every other task is starved as well. **With latency off, no TM at
    all leaves before FS init ends.** The boot report, 0x0021, 0x8030 and
    0x0032 arrive in one burst [M].
- **Ruled out: an IRQ storm or livelock at interrupt level** (the
  coordinator's alternative hypothesis, checked 2026-11-11):
  - **One IRQ per op** [M]. A 5.3 s run of the debug PFM ELF with
    `-trace grlib_irqmp_ack -trace memory_region_ops_write` gives:
    - 46,535 flash cmdSta writes;
    - 46,518 IRQ 1 (flash) acks. The 16 RESETs raise no IRQ, and one op
      was in flight at the cut;
    - 1,046 IRQ 10 acks: the 5 ms RTEMS clock tick at ≈200/s, delivered
      normally;
    - 6 others (IRQ 13).
  - So the flash IRQ is neither stuck nor re-raised. Its rate is bounded
    by the op rate (≈1 per 66–110 µs), and every ack is matched by one
    command.
  - **The CPU is at task level, not interrupt level.**
    - Over 11 gdb samples 0.5 s apart [M], `_ISR_Nest_level` was 0 in 9;
      the other 2 were caught in the flash or tick ISR.
    - `_Thread_Executing` and `_Thread_Heir` were always the FILE task
      (0x42d08e08).
    - `_Watchdog_Ticks_since_boot` rose steadily (93 → 1,171 over 6.2 s).
  - The earlier backtrace "in the flash IRQ callback under CLOCK_SPEED
    (start.S)" was one sample of the ISR stack. `CLOCK_SPEED` is the
    nearest symbol to the trap-table entry at 0x40001800, not a stuck
    trap.
  - **Ordering is as the driver expects** (`Flash.c:301-322` [C]):
    `currentControlBlock` is set before `START_FLASH_OPERATION`, and
    `waitingTaskForFlash` is set by `Read_page` before `Flash_readPage`
    (`MemFilesystem.c:1175` [C]). The model raises the IRQ inside the
    cmdSta write while the FSW has it masked (`IRQ_disableInterrupt`,
    `Flash.c:303`). IRQMP latches it, and the ISR runs at
    `IRQ_enableInterrupt`.
    - The callback order is therefore right. What is unlike the hardware
      is the *timing*: the event is sent before `rtems_event_receive`, so
      the receive never blocks.
  - **Which task runs `memfilesystem_async_init_task`, at which
    priority?**
    - It is task `FILE`, created with `FILESYSTEM_TASK_PRIORITY` = 250
      (`MemFilesystem.c:510-518` [C]).
    - The RTEMS table at 3.5 s gives FILE `real_priority` 250 and
      `current_priority` **1** [M]. INIT (priority 1) waits on
      `mutexAPI`, a priority-inheritance semaphore (`configFSW.h:199`
      [C]), held by FILE.
    - WDG (10) is READY, but the scheduler picks the highest-priority
      ready task, FILE at 1, every time.
    - "Priority starvation" is ruled out only for the nominal priorities;
      the *inherited* priority is what starves WDG.
- **Why only PFM:** the scan length.
  - `File_init` reads 2 pages per block for each of the 60 blocks of a
    control block (`BSW/Flash/File.c:318-333` [C]; `FS_CONFIG_MAX_FILES`
    = 60, `BSW/include/FilesystemDefinition.h:48` [C]). An empty flash
    costs 120 reads per control block.
  - AX builds use `-DLIMIT_FS_SIZE=32` (`tasks.vs.json:14, 26, 50` [C]):
    64 control blocks, so **7,680 reads**.
  - "Build production GU/PFM" is `-DHW_VERSION_PFM_IDPU` only
    (`tasks.vs.json:62` [C]). It gets (0x8000−64)/60 = 545 control blocks
    per flash (`MemFilesystem.c:46-52` [C]), 1,090 in all, so **130,800
    reads**: 17× AX.
  - This matches `nm -S`: `memFilesystemLocal` is 0x12200 bytes in AX
    against 0x134b10 in PFM [M].
- **Host cost is a contributor, not the cause.**
  - The emulator does about 15,000 reads per guest second, ≈66 µs per op
    wall and virtual time [M: two pageAddress samples, 2,825 ms of ticks
    apart].
  - The PFM scan therefore needs ≈8.7 s against the 6.6 s window. With
    MMIO tracing on: 62,927 reads and 16 resets, then the reset [M].
  - The model's stdio I/O pattern costs 7.6–17.8 µs per op on the sparse
    17.7 GB file (warm or cold) [M: micro-benchmark of
    `fseeko(END)+ftello+fseeko+fread(4224)+XOR`]. That is 12–27 % of
    the 66 µs. The rest is TCG guest CPU.
  - **Even with free host I/O, the scan would need ≈6.5 s**, right at the
    edge, and a faster host only hides the starvation. Host-side tuning
    alone is rejected as the fix (D-F8 keeps it as an optional speed-up).
- **The proof of the root cause** [M]:
  - A gdb loop wrote `RELOAD` (0x20000000) into the watchdog register
    (0x800005D4) every 2 s, standing in for WDG. With it, the PFM ELF:
    - finished FS init: TM(5,1) 0x0032 `01 0002 0000 0000 fffe` at
      ≈2,700 ticks ≈13.5 s guest time;
    - sent 0x0022, 0x0021, 0x8030 and 0x14A3, then HK TM(3,25) on 0x5E5
      every second;
    - after the nominal-smoke TCs (CONF @50, S20 @60, NOM @70), reached
      CONF (`0021 02 04`) and NOMINAL (`0021 04 05`, TM(1,7) for each TC).
  - The same feeder on the **debug** PFM ELF: after `init: SAFE mode`
    (0.122 s), the UART prints the tail that the AX golden also has
    (`uart.golden.log:44-48` [G]): four `LUTMgmt *E could not find LUT`
    lines at 14.422–14.425 s, then `Main delete init` at 14.440 s. That
    is FS-init end, the same ≈14 s as the release ELF.
    - From 15.44 s on, a `Watchdog *E incorrect cycles remaining` line
      every 2 s. This is an **artefact of the external feeder**: its
      asynchronous RELOADs defeat `Watchdog_feed`'s count check
      (`Watchdog.c:181-189` [C]). It is not expected with latency on
      [L]. The AX boot already has a natural line of this kind at ≈60 s
      (`boot-smoke.sh:76-78` [C]), so the gate must not forbid
      `Watchdog *E` lines in general.
  - So the watchdog starvation is the only blocker we saw. The run
    stopped at 100 s, so later PFM-only behaviour is unexplored [L].

## "If the filesystem check starves the WD, why not in flight?" (the user's question)

The same code runs in flight, with the same inherited priority 1 for FILE
during the boot scan. It does not reset there because on the hardware
every flash op **takes time and FILE sleeps through it**:

1. `Read_page` starts the op and calls `rtems_event_receive`
   (`MemFilesystem.c:1178-1187` [C]).
2. On the IDPU, the NAND op is still running when the receive is reached:
   tR plus a 4,224 B transfer, ≈130 µs [A]; the driver's own comment
   says ~2 ms (`Flash.c:306` [C]).
3. So FILE **blocks**. RTEMS dispatches the highest-priority *ready* task.
   When WDG's 1 s delay has expired, that is WDG (10), so it runs
   `Watchdog_feed` during the next NAND wait. HK, TM and FDIR (30–50) run
   in the remaining waits.
4. The flash IRQ wakes FILE when the op completes, and FILE preempts
   again, until the next op.

FILE therefore holds the CPU only for its own short CPU bursts between
ops, and every NAND wait is a scheduling point. The boot scan on the
IDPU is long (130,800 reads, so tens of seconds to minutes [I]), but the
watchdog is fed on time all through it.

The emulator removes the waits: the op, the IRQ and the event all finish
inside the cmdSta MMIO write. FILE never blocks, and with inherited
priority 1 it is always the task picked. The FSW relies *implicitly* on
NAND latency to yield. That is FSW oddity 1 below, harmless on hardware.

**AX is affected too, but its scan is short.** It runs the same
starvation for 7,680 reads, ≈0.45 s ("init: SAFE mode" at 0.157 s →
LUTMgmt at 0.58 s in the golden [G]), well inside 6.6 s.

## Key facts the plan rests on

### The FSW flash driver [C]

- Command start (`START_FLASH_OPERATION`, `Flash.c:81-84`):
  - three writes in this order: ramAddress (0x08), pageAddress (0x04)
    = `chip<<20 | page`, then cmdSta (0x00) = `MEMORY_TYPE_BIT | cmd |
    mcm<<3`;
  - commands: RESET 0, ERASE 1, READ 2, WRITE 3 (`Flash.c:56-60`).
- The ISR (`Flash.c:125-174`):
  - it ignores the IRQ unless BUSY (bit 4) == 0 and READY (bit 30) == 1;
  - for erase and write it checks FAILED (bit 24), then WRPROT (bit 31,
    1 = not protected);
  - for a read it executes `flush` (cache invalidate) and reports OK.
- Only one op is in flight. The driver refuses a second op while
  `currentControlBlock` is set (`Flash.c:239-244, 289-294, 335-340`).
- `Flash_reset` (`Flash.c:192-231`):
  - it aborts the current op (`terminateOperation(E_FLASH_RES_ERROR_RESET)`);
  - it then issues RESET to all 16 chips and polls BUSY == 0 for at most
    2,000 register reads per chip, **with no time base**. Reset must stay
    effectively instantaneous (D-F4).
- From an ISR, erase and write busy-poll up to 10,000 reads
  (`Flash.c:257-273, 378-406`). This is a last-ditch path only.
- The callers wait with timeouts of 50 ms
  (`FILESYSTEM_PAGE_READ_TIMEOUT`, `_WRITE_`, `_ERASE_BLOCK_`,
  `configFSW.h:336-338`; used at `MemFilesystem.c:1104, 1187, 1264`). A
  timeout triggers a flash reset, a 20 ms wait and a retry, up to 4 tries
  (`MemFilesystem.c:1173-1194, 208-218`). **Latency must stay well below
  50 ms.**
- With a 5 ms tick (`CONFIGURE_MICROSECONDS_PER_TICK 5000`,
  `configFSW.h:218`), the 50 ms timeout is 10 ticks.
- HW timing hints in comments of the disabled polling fast path:
  - "usually completes within 1000 iterations, ~2ms" for a read
    (`Flash.c:306`);
  - "~1200 iterations, ~3ms" for a write (`Flash.c:352`).
  - These include a `rtems_task_wake_after(1 ms)` and the FPGA transfer.
    They are not chip timings [I].

### The model today [C]

- Everything happens inside the third MMIO write (`grlib_stixflash.c:488-556`):
  - host file I/O;
  - the DMA (`physical_memory_write` / `_read`, `:336, :369`);
  - the status update and `qemu_irq_pulse`.
- The IRQ is pulsed at six sites, all **before** READY is set at `:543`:
  `flash_fail` `:171`, the erase paths `:233, :252, :296`, read `:339`,
  program `:393`.
- `write_counter` is zeroed by the success path (`:554`) and by
  `flash_fail` (`:169`).
- The cmdSta handler stores `cmd_sta = value` and re-derives
  command/mcm before any check (`:448-455`).
- BUSY is never set: the FSW writes cmdSta with bit 4 = 0. READY is
  cleared at `:494` and set again at `:543` within the same handler.
- WRPROT is forced to 1 at `:450-451` ("small workaround, this un-busies
  the FLASH; should be updated once we want to have the flash busy"). It
  is cleared and restored around erase and write (`:504-509, :515-520`),
  so the guest can never observe the cleared value.
- Each op calls `flash_file_size()` (`:113-121`): one
  `fseeko(SEEK_END)` + `ftello`. Then an `fseeko` and a stdio
  `fread`/`fwrite` of 4,224 B; erase uses punch-hole (`:236-249`).
- The device reset handler is empty (`:664-667`). There is no unrealize
  (realize `:645`) and no vmstate.
- The device has no QOM child name. `stixctrl` and `stixspw` have one
  (`leon3.c:459, 503`); the flash is created at `leon3.c:491` without one.
- The properties are `flash-file` and `inverted-storage` (`:670-673`).
- The qtests `/sparc/stix/flash`, `flash-error-paths`, `flash-addressing`
  and `flash-storage` (`tests/qtest/stix.c:202, 251, 420, 527`) assume
  that completion is synchronous. They issue through `flash_issue()`
  (`:79-85`) and check status, RAM and IRQ at once.

### Re-entrancy [C]

- On the MMIO path, `memory.c` engages
  `DEVICE(s)->mem_reentrancy_guard`, so a DMA that targets the device's
  own MMIO is refused.
- A timer callback runs outside any MMIO dispatch, with the BQL held.
  There the guard is **not** engaged unless the device engages it, as
  `stixspw_guard_enter/exit` do (`grlib_stixspw.c:184-196`, fix
  `a83fe9a3a5`).
- The guard protects only the device's **own** region
  (`system/memory.c:546-556`, `MEMTX_ACCESS_ERROR`). A 4,224 B page DMA
  that starts in the flash window (0x80000800, 256 B) runs on into EEPROM
  0x900, SPW 0xA00 and SDRAM-ctrl 0xC00 (`StixIdpu.h:224-227`), and those
  writes are not blocked. That is today's in-MMIO behaviour too.

### NAND timings [A]

`FilesystemDefinition.h:14` [C] names the part: Samsung K9F8G08X0M,
4,096 + 128 B pages, 64 pages per block. The FSW tree and the TSIM model
carry **no timings**. Typical values from the public datasheet family
(not checked against the exact datasheet, so all [A]):

| op | chip time | page transfer (4,224 B × ~25 ns) | model default |
|---|---|---|---|
| page read | tR ≤ 25 µs | ≈106 µs | **130 µs** |
| page program | tPROG ≈200 µs typ | ≈106 µs | **300 µs** |
| block erase | tBERS ≈1.5 ms typ | none | **1,500 µs** |
| reset | tRST ≤ 5–500 µs | none | **0 (synchronous)** |

The FPGA's real NAND bus rate is unknown (open question Q2).

### What latency costs [I, from the measured 66 µs/op]

- Emulated time per op ≈ 66 µs + t_op, plus the block → idle → IRQ →
  dispatch round trip that latency adds. So these figures are **lower
  bounds** [L in F-3/F-5].
- PFM scan at the defaults: 130,800 × 196 µs ≈ **26 s** to 0x0032 (today
  ≈13.5 s, then the watchdog reset). The gate must not depend on it (it
  waits for 0x142E before CONF, see "PFM boot gate").
- AX scan: 7,680 × 196 µs ≈ **1.5 s**. Today it is ≈0.45 s: the golden's
  `init: SAFE mode` is at 0.157 s and the `LUTMgmt` lines at 0.58 s [G].
- At "~2 ms per read" (the Flash.c comment), the PFM scan would take
  ≈4.4 min, which is why that is not the default.

## Decisions (D-F)

- **D-F1: one execution path; issue latches, completion executes.**
  - The third write (cmdSta, `write_counter == 3`) validates the
    sequence, latches an op record and sets the status:
    - the record holds cmd, mcm, chip, page, ramAddress and, for a
      program only, the RAM page snapshot;
    - BUSY = 1, READY = 0, FAILED = 0.
  - With latency 0 for that op, `stixflash_complete()` runs inline, which
    is today's behaviour. Otherwise it runs from a `QEMU_CLOCK_VIRTUAL`
    timer at now + t_op.
  - Completion does the following:
    - performs the host file I/O (read / program / punch-hole erase);
    - performs the read DMA;
    - sets BUSY = 0, READY = 1, FAILED from the result, WRPROT = 1;
    - pulses the IRQ, **once, last**. All six pulse sites of today's
      execute helpers (`:171, :233, :252, :296, :339, :393`) move here,
      after the status update; the helpers only return a result.
  - **Completion never touches `write_counter`.** The counter belongs to
    issue: it is zeroed when the op latches. A timer completion can fire
    between `Flash_reset`'s RAMAD/PAGEAD writes and its RESET cmdSta write
    (`Flash.c:81-84, 212`); zeroing the counter there would turn the RESET
    into a "sequence" failure with a stray IRQ. Completion-time failures
    therefore use a status-only variant of `flash_fail` (FAILED | READY,
    BUSY = 0, IRQ; no counter change).
  - The virtual clock stops while the VM is paused (gdb, QMP stop), and
    qtest drives it with `clock_step`.
- **D-F2: when RAM is touched.**
  - A **program samples guest RAM at issue**, inside the MMIO write
    where the guard is already engaged by memory.c. The FPGA moves the
    page into the NAND page register before tPROG [A]; the FSW does not
    touch the buffer while the op is in flight (one op at a time, under
    `lockAPI` [C]).
  - A **read writes guest RAM at completion**, matching the order data
    arrives on hardware, so the FSW's ISR `flush` (`Flash.c:170`) runs
    after the data lands.
  - Erase does no DMA.
- **D-F3: the read DMA from the timer runs under the re-entrancy
  guard.**
  - The completion callback wraps the DMA in
    `stixflash_guard_enter/exit`, a copy of the `stixspw_guard_*`
    pattern: save `engaged_in_io`, set it true, restore it.
  - The DMA uses `address_space_write(&address_space_memory, …,
    MEMTXATTRS_UNSPECIFIED, …)` and checks the `MemTxResult`.
  - A refused or failed DMA (e.g. ramAddress inside the flash's own MMIO
    window) completes the op with **FAILED | READY** and the IRQ, plus a
    `stixflash_error("dma")` trace.
  - The inline path (latency 0) runs inside the MMIO write, where the
    guard is already engaged, so both paths behave the same.
  - A qtest pins this (F-2).
  - Consequence: `latency=off` is **not byte-identical** to today on
    error paths. Today `physical_memory_write` ignores the result
    (`:336`), so a refused DMA completes OK; with the check it gives
    FAILED. The FSW never does this. Documented in accepted-diffs (F-6).
  - A program whose RAM sample at issue is refused (ramAddress inside the
    flash window) latches a "sample failed" flag: completion gives
    FAILED | READY and the IRQ, and nothing is written to the file.
- **D-F4: reset and abort.**
  - RESET (cmd 0) stays synchronous: BUSY clears in the cmdSta write.
    Reason: `Flash_reset` polls only 2,000 times with no time base
    (`Flash.c:214-221` [C]).
  - A RESET while an op is pending **aborts** it:
    - `timer_del`, and the op record is dropped;
    - no IRQ for the aborted op, no read DMA, no program or erase
      committed to the file;
    - trace `stixflash_op_abort`.
  - On hardware an interrupted program can leave a partial page. The
    model's clean abort is a documented divergence (accepted-diffs entry
    in F-6).
  - RAMAD and PAGEAD writes while busy are accepted: `Flash_reset`
    issues them before its RESET. They do not touch the latched op
    record.
  - A non-reset command while busy is dropped, with trace
    `stixflash_busy_reject`; the op in flight continues. The FSW never
    does this (`Flash.c:239-244` [C]).
  - **The busy decision comes before any register mutation.** Today the
    cmdSta handler overwrites `cmd_sta`, command and mcm before checking
    anything (`:448-455`). While an op is pending:
    - the written value is decoded into locals first; RESET aborts, any
      other command is rejected, and neither touches the pending op's
      status bits;
    - a sequence error (cmdSta or PAGEAD out of order) is traced
      (`stixflash_error("sequence")`) and resets `write_counter`, but
      does **not** set FAILED/READY or pulse the IRQ: those would land on
      the op in flight, and the FSW ISR (`Flash.c:132-174`) would credit
      them to it.
  - The device reset handler (today empty) does `timer_del`, drops any
    pending op, and sets `write_counter = 0` and `cmd_sta = 0` (the
    power-on value of the zeroed instance).
  - **Timer lifetime.** The timer is created as the **last** step of
    realize, after `init_and_load_flash_file` has succeeded (realize can
    fail late: the `flash-create-error` qtest). A new unrealize does
    `timer_free` (pattern: `grlib_stixctrl.c:978-984`) and closes the
    file.
- **D-F5: status bits.**
  - In flight: BUSY = 1, READY = 0.
  - At completion: BUSY = 0, READY = 1.
  - FAILED is latched at completion: host I/O error, write beyond the
    backing file (D-H), or refused DMA.
  - WRPROT = 1 at all times. The clear/restore dance at
    `grlib_stixflash.c:505-530` goes away (the guest never saw it), and
    so does the "small workaround" comment.
  - A guest-commanded sequence error (`flash_fail("sequence")`) stays
    immediate, as today, **when no op is pending**. No op starts, so there
    is nothing to delay. While busy, see D-F4.
- **D-F6: QOM properties** (static, set before realize; `-global
  grlib-stixflash.<name>=…`):

  | property | type | default | meaning |
  |---|---|---|---|
  | `latency` | bool | **on** | off = every op completes inline (today's model) |
  | `tread-us` | uint32 | 130 | page read |
  | `tprog-us` | uint32 | 300 | page program |
  | `tbers-us` | uint32 | 1500 | block erase |

  - Validation at realize: each value ≤ 40,000 µs, else `error_setg`
    naming the FSW's 50 ms timeout. 0 means "inline for that op type".
  - Values from 1 to 19 µs are allowed, with a `warn_report`. Under
    no-icount the virtual clock tracks the host, and the guest's
    issue → `rtems_event_receive` path costs a few µs of host time. A
    timer that fires before the receive brings back today's non-blocking
    behaviour [I].
  - There is no runtime (qom-set) knob, and the gates use no `t*-us`
    override: they run the shipped defaults (Q3 decided).
- **D-F7: the default is ON, with no golden recapture expected.**
  - The default fixes the flight build without per-gate flags: the user,
    the platform controller and the attenuator track all run PFM ELFs.
    Off-by-default would bring the 6.8 s reset back on every manual run.
  - Per the hardware it is the more faithful default: every NAND op takes
    time.
  - The boot UART golden is cut at `init: SAFE mode` (`boot-smoke.sh:79-83`
    [C]). INIT first blocks on `mutexAPI` only *after* that line
    (ContextMgmt_init is in OpModeMgmt_initMode [M]). Before it, the only
    flash wait is the context read in `MemCommon_init`: 2+2 control
    blocks, 480 reads, ≈49 ms today and ≈94 ms at the defaults. During
    that wait INIT is blocked anyway and only the lines' timestamps
    change, which `normalize-log.py:18,34` strips [C].
  - **So the normalized boot cut should stay byte-identical [I → L in
    F-3].** If F-3 finds a diff, it stops and reports. There is no
    recapture without the user's OK (feedback rule: pause after
    failures).
- **D-F8: host-cost mitigation is optional and deferred (F-later, Q4).**
  - Replace the per-op `fseeko(END)/ftello` with a file size cached at
    realize: the file never grows (D-H).
  - Replace stdio with `pread`/`pwrite` on `fileno` (no stdio buffer, no
    `fflush`).
  - Measured: 2.5–2.8 µs per read with `pread`, against 7.6–17.8 µs for
    the stdio pattern [M]. That saves ≈1–2 s of PFM scan.
  - mmap is rejected: a 17.7 GB mapping of a sparse file interacts badly
    with punch-hole erases and gains little over `pread`.
  - A page cache is rejected: the scan reads each page once.
- **D-F9: observability.**
  - The flash gets a QOM child name, `/machine/stixflash`, in
    `leon3.c:491`, like stixctrl/stixspw.
  - Read-only counters: `ops-read`, `ops-prog`, `ops-erase`, `ops-abort`
    (uint64).
  - Trace events: `stixflash_op_start(cmd, mcm, chip, page, delay_us)`,
    `stixflash_op_done(cmd, page_address, cmd_sta)`,
    `stixflash_op_abort(cmd)`, `stixflash_busy_reject(cmd)`.
  - Note: `capture-golden.sh` runs `-trace 'stix*'`, so a future
    recapture of `qemu-stdout.golden.log` grows by ≈15k lines. No gate
    compares that file [C: grep]. F-6 documents it.
- **D-F10: the PFM boot gate is a new script, not a mode of boot-smoke.**
  - `scripts/stix/pfm-boot-smoke.sh`, on the gate-ports slot library
    when it has landed.
  - The PFM ELF has no UART, so the gate is TM-only (see "PFM boot
    gate").

## Commits (test-first: each commit adds its tests first, then the code)

Merge order: F-1 and F-2 land **after gate-ports has merged into
`stix-uprev-tc`** (gate-ports rewrites the qtest setup helpers in
`tests/qtest/stix.c`); this branch rebases onto it first. F-5 uses
gate-ports' `scripts/stix/lib/slot.sh`.

### F-1: issue/complete split, no behaviour change

- Refactor `grlib_stixflash.c` into `stixflash_issue()` (latch) and
  `stixflash_complete()` (execute), called inline. Add the op record and
  the D-F4 device-reset handler.
- Hoist all six IRQ pulse sites (`:171, :233, :252, :296, :339, :393`)
  out of the execute helpers into `stixflash_complete()`, after the status
  update (D-F1). `write_counter` is zeroed at issue only; completion never
  touches it. Add the status-only completion-failure variant of
  `flash_fail`.
- Create the op state at the end of realize; add an unrealize (D-F4
  timer lifetime; the timer itself arrives in F-2).
- Tests: the four existing flash qtests pass unchanged. Add a
  `/sparc/stix/flash-reset-state` qtest: a system reset (qtest
  `system_reset`) mid-sequence leaves `write_counter` 0 (a fresh
  three-write sequence succeeds) and cmdSta 0.
- Gate: `make check-qtest-sparc` (stix only).
- Correction (F-6, accepted-diffs §42): the "no behaviour change" above
  holds only between guest MMIO sequences. The new reset handler is a
  FIX: before it, a system reset at write_counter 1 or 2 left the stale
  count, so the next boot's `Flash_reset` RAMAD write failed as a
  sequence error.

### F-2: latency timer, BUSY/READY, guarded DMA, properties (default still off)

- Add the D-F1..D-F6 and D-F9 code. The `latency` default is still
  **off**, so this commit cannot move any gate.
- qtests first, all with `-global grlib-stixflash.latency=on`:
  - `flash-latency-read`:
    - after issue: BUSY = 1, READY = 0, no IRQMP pending bit, RAM
      unchanged;
    - `clock_step(129 µs)`: still busy;
    - `clock_step(2 µs)`: BUSY = 0, READY = 1, IRQ pending, RAM = page;
      exactly one IRQ.
  - `flash-latency-prog`:
    - the RAM buffer is changed after issue, and the file gets the
      issue-time image;
    - the file is unchanged before completion.
  - `flash-latency-erase`: completes at 1,500 µs; erased only then; one
    IRQ.
  - `flash-latency-abort`:
    - RAMAD/PAGEAD/RESET during a pending read: BUSY clears at once;
    - no IRQ at the old deadline, RAM untouched, `ops-abort` = 1;
    - **the completion-between-writes case:** RAMAD, PAGEAD,
      `clock_step` past the deadline (the op completes), then the RESET
      cmdSta: the RESET executes, no "sequence" error, FAILED clear.
  - `flash-latency-busy-reject`:
    - a READ issued during a pending op is dropped; the first op completes
      normally, with its own status and one IRQ;
    - an out-of-order cmdSta/PAGEAD write during a pending op sets no
      FAILED/READY and pulses no IRQ; the op completes normally (D-F4).
  - `flash-latency-dma-guard`:
    - first program the page so its guest image is all zero (inverted
      storage), so the run-on into EEPROM/SPW/SDRAM-ctrl writes zeros;
    - ramAddress = 0x80000800 (the flash's own window); completion gives
      FAILED | READY and the IRQ; the flash's own cmdSta reads
      FAILED | READY (not page data); QEMU stays up.
  - `flash-latency-props`: `tread-us=40001` fails realize (subprocess
    test, like `flash-create-error`); `tread-us=0` is inline.
  - `flash-latency-system-reset`: a system reset during a pending op
    gives no IRQ afterwards.
- Existing tests get a `flash_wait()` helper (`clock_step(max t + 1 µs)`,
  a no-op cost when off), called after every `flash_issue`, so they pass
  under both defaults.

### F-3: flip the default on; verify the AX suite

- `latency` defaults to on — flipped only here, and kept only if the
  whole AX suite passes.
- Run the full suite on the AX gate ELF under the 21000 lock convention
  or a slot: boot-smoke (incl. `--with-tm`), tc-smoke, nominal-smoke
  (incl. `--negative`), fault-smoke, detector-smoke, soak and the qtests.
- Record in the commit message:
  - the AX FS-init time, from `stixflash_op_done` count and timestamps or
    the TM(5,1) 0x0032 arrival [L];
  - the measured mean op latency from the trace timestamps [L];
  - that the normalized boot cut is identical [L].
- **Stop rule:** any diff or failure means stop and report. Do not
  re-capture, re-tune windows or retry in a loop.

### F-5: `scripts/stix/pfm-boot-smoke.sh` (the PFM boot gate)

- See "PFM boot gate" below. Two flavours on the same script: the TM
  checks for the release ELF, plus the UART checks when the ELF is the
  debug build. A `--dry-run`, a `--negative` and a `--no-nominal` mode.
- Flavour detection: an ELF with UART output contains the string
  `init: SAFE mode` (`strings ELF | grep -q 'init: SAFE mode'`: AX 1,
  debug 1, v186 0 [M]). Then the UART checks are added; the TM checks
  always run. (Not the `debug_flush` symbol: it marks TRACE_TO_* builds,
  and the DEBUG_PRINT-only AX gate ELF has none.)
- Pre-flight self-test: with the ELF absent → SKIP exit 77; with an AX
  ELF (a `nm` symbol containing `moveStartCoarseT`, the att plan's rule)
  → exit 2. The PFM ELFs have none [M].
- Sources `slot.sh` (see "Interactions").
- **As built (2026-11-12, second correction after the first live
  release-ELF run).** The gate is `scripts/stix/pfm-boot-smoke.sh` plus
  its TM driver `scripts/stix/pfm-boot-drive.py` (tmtc-probe.py's
  `--send-at` cannot anchor a send on a TM arrival, and a probe restart
  per phase would drop the connection, so the driver stays on the
  socket, stamps every TM with its `[m=...]` offset from the connect and
  re-schedules the sends itself; `--replay` re-judges a driver log
  through the same checks). **Measured release-ELF timeline with latency
  on (first live run, fresh flash):** 0x0022 + 0x0021 (SAFE) at +0.4 s,
  0x14A3 at +16 s, 0x142E at +32 s, FS init still running at +46 s, 47
  watchdog RELOADs and no reset — **FDIR and the TC20 boot timer are NOT
  gated by FS init**, and the mode change waits on the
  filesystem/context (a CONF sent before 0x0032 was accepted but never
  executed). Windows now (offsets are probe seconds from the connect,
  chained ones from the named event's arrival):
  - TM(5,1) 0x0022 Application SW boot success by @20, and before
    0x0032 (the witness that the FS task yields);
  - TM(5,1) 0x14A3 FDIR protection enabled by @40 (measured 16 s);
  - TM(5,1) 0x142E Preparing for shutdown by 0x14A3 + 25 s, capped @160
    (failure "no 0x142E by @160");
  - TM(5,1) 0x0032 Filesystem initialisation finished, status 01, and
    TM(5,3) 0x8030 Context file(s) not found by @150 each (fresh
    flash);
  - then CONF at max(0x142E, 0x0032) + 5 s (no fixed floor), S20 = 0 at
    CONF + 10 s, NOMINAL at CONF + 20 s, the NOMINAL checks by
    NOMINAL + 5 s (nominal-smoke's TC bytes and NOMINAL checks);
  - the deadline checks run before any send is scheduled, so an expired
    window is named (e.g. "no 0x0032 ... by @150"), never a later
    step's;
  - forbidden throughout: TM(5,3) 0x8021 Watchdog failure detected, any
    `stixctrl_watchdog_reset` trace line, and (debug flavour only) a
    UART "Read timeout";
  - `STIX_PFM_SECONDS` default 240 still fits (worst case CONF at
    155 s + 35 s of checks); `--negative` keeps the 30 s window
    (`latency=off` must end in the watchdog reset). The measured 0x0032
    offset is printed in the verdict, per ELF flavour.

### F-6: docs

- `docs/stix-port/baseline/README.md`: the PFM gate recipe and ELF
  provenance (pointing at `/home/laszlo/dev/stix-elf/PROVENANCE.md`).
- `accepted-diffs.md`:
  - a clean abort instead of a partial program;
  - the datasheet [A] latencies;
  - a read's file I/O moves to completion;
  - `latency=off` checks the DMA result (a refused DMA fails; today it
    silently succeeded);
  - the FSW's ISR last-ditch busy-poll (`Flash.c:257-273, 378-406`,
    10,000 cmdSta reads, a fatal-path flash dump) succeeds only if those
    reads take longer than t_op in host time. The timer fires from the
    main loop between the vCPU's MMIO reads.
- `ground-station.md`: PFM boot takes ≈26 s or more to FS-ready.
- The `qemu-stdout.golden.log` growth note.
- This plan's "FSW oddities" copied into the findings notes.

### F-later (deferred, Q4): host I/O via pread/pwrite, cached size

- D-F8. Tests: the four flash qtests and `flash-storage` (inverted /
  plain) pass. A new `flash-size-cached` qtest checks that an
  out-of-file page still reads 0xFF and fails a program.
- Measure the per-op wall cost before and after: PFM scan time to 0x0032
  in pfm-boot-smoke's log [L].

## PFM boot gate

(Superseded by the "As built" note in F-5: the first live release-ELF
run showed 0x14A3/0x142E are not gated by FS init, and the windows and
the CONF anchor changed. Kept for the reasoning.)

What "PFM boots" means, without a UART:

1. **Connect group** (offsets from the probe's connect; QEMU starts 1 s
   after the probe, as in `boot-smoke.sh:97-104`):
   - TM(5,1) `002200000008000000` (the boot report) **@20**. With latency
     on it must arrive *before* 0x0032. That ordering is the direct
     witness that FILE now yields; with latency off everything arrives in
     one burst after FS init [M].
   - TM(5,3) `8030` (no context file: proves a fresh flash) @60.
   - TM(5,1) `0032 01` (FS init finished, status true; `TMForm.c:631-641`
     and `MemFilesystem.c:1064` [C]) **@60**. At the defaults it is
     expected at ≈26 s or later (a lower bound) [I → L].
   - TM(5,1) `14a3` @60 and `142e` @70: the TC20 boot timer fired ≈12 s
     after 0x14A3 in the measured run [M].
2. **No watchdog reset, with a live trace.**
   - QEMU runs with
     `-trace enable=stixctrl_watchdog_*,file=$RUN_DIR/wdg.trace` and
     `-no-reboot`.
   - The gate needs: no `stixctrl_watchdog_reset` line, **at least one
     `stixctrl_watchdog_cmd … cmd=1` line** (the trace pipe is proven
     live, so an empty result is not vacuous), and QEMU alive at the
     deadline (rc 124 from `timeout`; under `-no-reboot` a reset exits
     QEMU).
3. **Forbidden TM:** TM(5,3) `8021` anywhere in the run. EID 0x8021 is
   "Warm SW reset by WDT failed" (`TcTm/TMData.h:317`), sent once by
   `FDIR_watchdogFailure` (`FDIR/FDIR.c:788-814`) from `Watchdog_feed`'s
   count checks (`Watchdog.c:181-188` "incorrect cycles remaining", and
   `:197-208` count ≠ reload after RELOAD for 4 s, `commonFSW.h:318`),
   once FDIR is on (after 0x14A3).
   - The 0x8021 seen ≈14 s after 0x0032 in the measured runs was the gdb
     feeder's artefact: its asynchronous RELOADs defeat the count check.
   - With latency on, a 0x8021 means a **model watchdog bug**: stop and
     report, do not allow it.
4. **Liveness:** after 0x0032, at least one HK TM(3,25) on APID 0x5E5
   per 2 s on average until the end, checked post-run from the probe log
   (the HK cadence is 1 s [M]).
5. **NOMINAL** (default; `--no-nominal` skips it). nominal-smoke's TC
   bytes and expectations, sent only after FS init and after the TC20
   timer:
   - **CONF is sent only after 0x142E has been seen.** The gate waits for
     0x142E; if it has not arrived by @55, it fails with a clear message
     ("no 0x142E by @55: boot too slow or stuck") instead of sending CONF
     on a fixed offset. In a test run with CONF at +30 s, the TC20 timer
     fired in CONF and fell back to SAFE (`0021 04 02`); the later S20 TC
     then got TM(1,8) [M].
   - CONF at max(@60, 0x142E + 5 s); S20 = 0 at CONF + 10 s; NOMINAL at
     CONF + 20 s;
   - expect `0021 04 05`, the TM(1,7) for each TC, and SID2 0x5A4 with
     mode nibble 5 within NOMINAL + 5 s;
   - forbid `0021 05 02`, TM(1,8) and `0021 04 02`.
6. **UART checks, debug ELF only** (flavour detected by the
   `init: SAFE mode` string; `STIX_PFM_DBG_ELF`, default
   `/home/laszlo/dev/stix-elf/ASW_pfm_dbg.elf`). The UART is normalized
   with `normalize-log.py` (timestamps and banner build stamp stripped).
   **Key lines only, no second golden (Q8):**
   - `init: SAFE mode`;
   - after it, the four `LUTMgmt … could not find LUT (type=1..4 …)`
     lines;
   - then `Main delete init`.
     - With latency on, `delete init` is expected at ≈26 s of UART time
       or later [I → L]; today it never comes [M]; with the feeder,
       14.44 s [M].
   - The UART `delete init` timestamp is the precise FS-init time that
     F-3 and F-5 report for PFM.
   - Forbidden: any UART line containing `Fatal error` (`main.c:57-60`
     [C], the DEBUG_PRINT panic path), and any `Read timeout`,
     `Write timeout` or erase-timeout `TRACE_ERROR` line
     (`MemFilesystem.c:1190` and its write/erase twins): the witness that
     latency never comes near the FSW's 50 ms timeouts.
   - No ban on `Watchdog *E` lines in general (the AX boot has a natural
     one at ≈60 s, `boot-smoke.sh:76-78`); the 0x8021 ban (item 3) is the
     watchdog check that counts.
   - The watchdog check is the same as item 2.
   - The TM checks 1-5 run as well. The debug ELF is PFM +
     TRACE_TO_FLASH, so the release ELF's TM expectations must also hold
     on it (a subset check, not TM equality) [L].
7. **`--negative`:**
   - `-global grlib-stixflash.latency=off`, 30 s;
   - pass only if `wdg.trace` contains "count expired" and QEMU exited by
     itself.
   - With the debug ELF, also require that the UART stops at
     `init: SAFE mode` (no `delete init`). Reproduced in validation: 43
     lines, then the reset [M].
   - This keeps the gate honest: it is a clean A/B (only `latency`
     differs) and proves the gate can see the original failure.

Which ELF the suite runs (Q3 decided):
- the **default suite** runs the release ELF at the shipped defaults:
  positive + `--negative`. No `t*-us` override, no runtime knob;
- the debug flavour is opt-in (`STIX_PFM_DBG=1`), one separate QEMU run;
- an absent ELF is a SKIP for that flavour, and both absent means exit
  77.

Run shape:
- `STIX_PFM_SECONDS`, default 180 (≈3 min, accepted);
- a fresh run dir per run: an empty dir, and the device creates the
  sparse `flash.bin`;
- never copy a flash and never use a shared flash: a NOMINAL entry writes
  context (nominal-smoke's rule);
- run dir and ports from the slot (below); the run dir is removed at the
  end unless `STIX_KEEP_RUN=1`.

ELF (Q7 decided):
- the ELFs live in `/home/laszlo/dev/stix-elf/` (outside any repo), each
  with a `.sha256` file, plus `PROVENANCE.md`;
- `STIX_PFM_ELF`, default `/home/laszlo/dev/stix-elf/ASW_pfm_v186.elf`;
  provenance `gh release download v186 -R i4Ds/STIX-FSW -p
  ASW_pfm_v186.elf`; sha256
  `2d479b54754aa27d2024639972bb7d82ff866bc50bcc02b4628f4852c08ebd40` [M];
- `STIX_PFM_DBG_ELF`, default `/home/laszlo/dev/stix-elf/ASW_pfm_dbg.elf`:
  user build 2026-11-11 21:11, `tasks.vs.json:74` (PFM + DEBUG_PRINT +
  TRACE_TO_FLASH; to be confirmed by the user), sha256
  `116b5089ebf4a093e94b4922a53bee2eb7d4fd1d7b80ded2b2803db076de8310`;
- the gate checks each ELF against its `.sha256` and prints a mismatch
  as a WARN, not a FAIL, so a newer release can be tried.

## Interactions

- **Merge order.** F-1/F-2 land after gate-ports has merged into
  `stix-uprev-tc` (it rewrites the qtest setup helpers in
  `tests/qtest/stix.c`, near lines 168, 1641-1770 and 3186, plus
  `boot-smoke.sh`, `capture-golden.sh`, `baseline/README.md` and
  `accepted-diffs.md`); rebase first. `stix-uprev-tc` is at this track's
  base `9c7bd11322` (no conflict). The att worktree holds only its plan.
- **Gate-ports slots** (`scripts/stix/lib/slot.sh`, landed via
  gate-ports):
  - pfm-boot-smoke sources it: `stix_slot_setup`, then
    `STIX_SLOT_QEMU_ARGS` (`-global grlib-stixspw.tmtc-port=$TMTC_PORT`,
    `slot.sh:206`) and the slot's run-dir suffix;
  - it takes **no** private flash copy (`stix_flash_private`,
    `slot.sh:227`), since a fresh sparse file costs nothing;
  - legacy mode (`STIX_PORT_BASE` unset) keeps port 20000+uid, and the
    caller holds the 21000 lock, as for the other gates.
  - The `-gdb` port is not part of the slot band and is never used by
    the gate.
- **Attenuator track:** a blocking change, see "Required change to the
  attenuator plan" below.
- **Platform controller:** it boots whatever ELF the user picks. With
  F-3 there is no flag to remember. It should show "FS init" until
  0x0032 arrives (≈26 s or more on PFM).

## Required change to the attenuator plan (blocking)

The att plan (`~/dev/qemu-stix-att/docs/stix-port/2026-11-11-attenuator-plan.md`)
must change before its PFM sequence can pass:
- Its PFM sequence **cannot run before F-3**: any PFM ELF resets at
  ≈6.8 s.
- Its timings assume an AX-like boot: "0x14A3 within 30 s", then CONF @35
  (att plan `:830-835`):
  - at the defaults, PFM's 0x0032 comes at ≈26 s or later and 0x14A3
    after it [I], too close to 30 s;
  - CONF @35 lands next to the TC20 timer (≈12 s after 0x14A3 [M]) and
    falls back to SAFE, as measured.
- **Required:** anchor the PFM sequence on 0x0032/0x142E: wait for
  0x0032 and then 0x142E, and send CONF **only after 0x142E** (as
  pfm-boot-smoke does), with the later steps relative to CONF. No
  `tread-us` override (Q3).
- Unchanged and fine: its flavour detection (`moveStartCoarseT` in `nm`
  means AX) works on both PFM ELFs (no such symbol [M]). For UART it can
  use the debug ELF (`/home/laszlo/dev/stix-elf/ASW_pfm_dbg.elf`), which
  answers its Q1; TM-only checks work with v186.
- The ELF defaults move to `/home/laszlo/dev/stix-elf/`.

## Goldens: what changes, when, and why

- `uart.golden.log`: no change expected in any commit (D-F7). F-3
  verifies this [L].
- `tm.golden.log`, `asm-head.golden.log`: not regenerated by this track.
  Boot TM order on AX may shift by ms but the golden is not a gate input
  [C: grep].
- `qemu-stdout.golden.log`: not recaptured. A future capture gains the
  per-op `stixflash_op_*` lines (D-F9), documented in F-6.
- There is no PFM golden. pfm-boot-smoke is expectation-based.

## Gate impact

| gate | effect at the defaults | risk |
|---|---|---|
| boot-smoke | boot cut unchanged [I → L in F-3]; AX FS-ready +≈1 s | low |
| boot-smoke `--with-tm` | 40 s window, AX TM within 2 s | low |
| tc-smoke / fault-smoke / detector-smoke | AX FS-ready +≈1 s; TC @20, CONF @50, `14a3@30` all absorb it | low [L] |
| nominal-smoke | NOMINAL entry writes context: a few erases and programs, +ms | low [L] |
| soak | none expected; run in F-3 | low [L] |
| qtests | flash tests get `flash_wait()` (F-2) | none |
| new pfm-boot-smoke | ≈3 min per run; default suite = release ELF positive + `--negative` | runtime (accepted, Q3) |

## Risks and checks

- **Timer granularity under no-icount.** QEMU sets the timer slack to
  1 ns (`util/qemu-timer.c:677-678`), so host slack is not a factor. The
  extra per-op cost is the guest's block → idle → IRQ → dispatch round
  trip; a longer wait only means FILE blocks more. F-3 records the
  measured mean from the trace timestamps [L].
- **A latency set too short may not block.** If t_op is less than the
  guest's issue → receive path (µs of host time), the IRQ beats the
  receive and starvation returns. Mitigated by the D-F6 warn below 20 µs
  and by the `--negative` mode, which shows what starvation looks like.
- **PFM boot time.** ≈26 s to FS-ready at the defaults, a lower bound. If
  real HW timing says ~2 ms per read (the Flash.c comment), the honest
  default would give ≈4.4 min. That is why the defaults are
  datasheet-typical [A]; a real boot → 0x0032 time is still asked for
  (open question below).
- **The FSW's ISR last-ditch busy-poll** (`Flash.c:257-273, 378-406`):
  10,000 cmdSta reads must outlast t_op in host time; the timer fires
  from the main loop between the vCPU's MMIO reads. A fatal-path flash
  dump only; documented in accepted-diffs (F-6).
- **Out-of-MMIO DMA.** It is designed in (D-F3) and pinned by the
  `flash-latency-dma-guard` qtest. Any future move of the program's RAM
  sample into the timer must take the same guard. The guard does not
  cover neighbouring devices (see "Re-entrancy").
- **A populated flash scans slower.** Used blocks add BCH decode and
  catalogue work (`File.c:338-380` [C]). The gate always uses a fresh
  flash. A long-lived flash (the platform's) boots slower; measured in
  F-later if it is taken.
- **The model's watchdog semantics (not this track; follow-up item
  "watchdog DISABLE stickiness").** The AX gate ELF is a
  `-DDISABLE_WATCHDOG` build [C+M]:
  - `Watchdog_setRefreshInterval` writes ENABLE 0xC0000000, DISABLE
    0xA0000000, RELOAD 0x20000000 to 0x800005D4 (`Watchdog.c:129-133`),
    and `Watchdog_feed` has no code for `:180-190` [M: disassembly];
  - the golden trace shows cmd 7, 1, 5, 6, 5, 1 [G:
    `qemu-stdout.golden.log:17-22`]: DISABLE leaves `running=0`, the next
    RELOAD sets `running=1`, because the model's RELOAD re-arms a stopped
    counter (`stixctrl_wdg_run` from `grlib_stixctrl.c:241-243` [C]);
  - so the AX gates run with a live watchdog only through this model
    behaviour. They depend on it, so it is **not changed in this track**.
  - Both PFM ELFs have the watchdog on (the debug ELF writes ENABLE,
    RELOAD; v186 ends ENABLE, RELOAD) [M].
  - The earlier "55 s gdb halt, no reset" spike ran the AX ELF with the
    counter re-armed, so its conclusion (the counter runs on
    `QEMU_CLOCK_VIRTUAL`, which stops while halted) holds for the model,
    but it was never shown on a watchdog-on build: re-check on PFM after
    F-3.
- **No vmstate.** The device has none today, and a pending op would need
  the timer and the op record in it. Snapshots are out of scope.

## FSW oddities

1. `ASW/configFSW.h:199` redefines the RTEMS name
   `RTEMS_DEFAULT_SEMAPHORE` as a binary **priority-inheritance**
   semaphore. `mutexAPI` (`MemFilesystem.c:477-481`) uses it.
   - So any priority-1 caller of the filesystem API (INIT, via
     `ContextMgmt_init → store_new_context → FSdelete`) lifts the
     priority-250 FS init task to 1 for the whole scan.
   - That breaks the intent stated at `configFSW.h:263-268` ("watchdog
     has highest priority").
   - On hardware it is safe only because each NAND op blocks the task.
2. `MemFilesystem.c:1018-1060`: the async FS init task holds `mutexAPI`
   across *all* partitions. INIT's SAFE-mode entry (context store) waits
   for the full scan: 130,800 page reads on PFM, every boot, even on an
   empty flash.
3. `BSW/Flash/Flash.c:306-318, 352-376`: the polling fast path is
   commented out. Its comments give HW costs of ~2 ms per read and ~3 ms
   per write.
4. `BSW/Flash/Flash.c:214-221`: `Flash_reset` decides chip health from
   2,000 BUSY polls with no time base. The result depends on CPU speed.
5. `MemFilesystem.c:41-44`: the comment says the ICD reserves 8 MiB per
   flash; the code reserves 16 MiB ("let's ignore that for now").
6. `Flash.c:303-323`: the driver masks the flash IRQ around the command
   start. An IRQ from an op that completes instantly is taken only at the
   unmask, before the caller waits. Correctness relies on the IRQMP
   pending latch and on RTEMS events latching.
7. `tasks.vs.json:14`: the default "Build" task adds
   `-DDISABLE_WATCHDOG`. Only "Build with WD ON" and the production tasks
   run with the watchdog.
8. `WDT.c` `WDT_setWdgInterval` ends with DISABLE; on a DISABLE_WATCHDOG
   build `Watchdog_setRefreshInterval` then writes ENABLE, DISABLE, RELOAD
   (`Watchdog.c:124-133`). Whether RELOAD re-arms after DISABLE on the
   FPGA decides whether such a build has a live watchdog at all.

## Decisions on Q1-Q8 (2026-11-11, coordinator; the user may override)

- **Q1 Default:** latency **on**, flipped only in F-3 after the whole AX
  suite passes.
- **Q2 Timings:** datasheet defaults (read 130 / program 300 / erase
  1,500 µs, all [A]). Still open for the user: see below.
- **Q3 Gate runtime:** ≈3 min accepted. The default suite runs the
  release ELF at the shipped defaults; no `tread-us` override, no runtime
  knob.
- **Q4 Host I/O:** deferred to "F-later".
- **Q5 Watchdog DISABLE:** a separate follow-up item, "watchdog DISABLE
  stickiness"; not changed here, the AX gates depend on it.
- **Q6 0x8021:** explained (the gdb-feeder artefact); forbidden in F-5.
  With latency on it means a model watchdog bug.
- **Q7 ELF storage:** `/home/laszlo/dev/stix-elf/` (outside any repo),
  each ELF with a `.sha256`, plus `PROVENANCE.md`; gate defaults point
  there.
- **Q8 Debug-ELF golden:** key lines only, no second golden.

## Open questions for the user

- **O1. Real timing.** Do you have a real boot → TM(5,1) 0x0032 time on
  the PFM or EM? It would let us calibrate the [A] defaults. (The FSW
  comment's ~2 ms per read would mean a ≈4.4 min PFM scan.)
- **O2. Debug ELF build.** Please confirm `ASW_pfm_dbg.elf` was built
  with "Build production GU/PFM debug" (`tasks.vs.json:74`: PFM +
  DEBUG_PRINT + TRACE_TO_FLASH), as its code indicates, for
  `PROVENANCE.md`.
- **O3.** Any override of the Q1-Q8 decisions above.

## Out of scope

- A fault injection for flash errors (FAILED, bad blocks, a stuck BUSY)
  over QMP. It is a natural follow-up once the op record exists.
- Concurrent multi-chip ops (the FSW serialises them).
- A vmstate or snapshot of the flash device.
- A full-run PFM UART golden: the gate pins only the debug ELF's
  post-SAFE key lines (Q8).
- Watchdog DISABLE stickiness in the CTRL model (follow-up, Q5).

## Review resolutions

Independent validation 2026-11-11: FSW read-only, the model at
`9c7bd11322`, the three ELFs (sha256 match), `gdb-multiarch` disassembly
and two throwaway runs of the debug ELF (`/tmp/stix-flashrev-1`, TMTC
21650/21651, gdb 22650/22651, fresh sparse flash, deleted). All items
accepted and folded in.

| # | finding | resolution (where) |
|---|---|---|
| R1 | Completion zeroed `write_counter` (`:554`, `flash_fail` `:169`); a timer completion between `Flash_reset`'s PAGEAD and RESET cmdSta writes would fail the RESET | counter zeroed at issue only; status-only completion failure (D-F1, F-1); qtest case in `flash-latency-abort` (F-2) |
| R2 | cmdSta handler mutates `cmd_sta`/command/mcm before checks (`:448-455`); a sequence error while busy would clobber the pending op and pulse an IRQ | busy decision before mutation; sequence error while busy sets no status, no IRQ (D-F4, D-F5); `flash-latency-busy-reject` extended (F-2) |
| R3 | Six IRQ pulse sites inside the execute helpers, before READY (`:171, :233, :252, :296, :339, :393`; READY `:543`) | hoisted into `stixflash_complete()`, once, after the status update (D-F1, F-1); "The model today" |
| R4 | dma-guard qtest at 0x80000800 runs 4,224 B into EEPROM/SPW/SDRAM-ctrl; the guard covers only the own region (`memory.c:546-556`) | page pre-programmed to an all-zero guest image; assert own cmdSta FAILED\|READY (F-2); "Re-entrancy" note |
| R5 | No unrealize; realize can fail late | timer created last in realize; unrealize `timer_free`s and closes the file (D-F4, F-1) |
| R6 | Debug ELF is TRACE_TO_FLASH (`tasks.vs.json:74`), not `:98`; `debug_flush` is a TRACE_TO_* marker, absent from the DEBUG_PRINT AX ELF | provenance fixed (Sources, ELF); flavour detection by the `init: SAFE mode` string (F-5); TM check on the debug ELF is a subset, not equality (gate item 6); O2 asks the user to confirm |
| R7 | Q6: 0x8021 = "Warm SW reset by WDT failed" (`TMData.h:317`, `FDIR.c:788-814`, `Watchdog.c:181-188, 197-208`), the feeder artefact | forbidden in F-5 (gate item 3); with latency on it means a model watchdog bug |
| R8 | Att PFM sequence ("0x14A3 within 30 s", CONF @35) fails at the defaults | blocking section "Required change to the attenuator plan" |
| r1 | Evidence: golden DISABLE trace is `qemu-stdout.golden.log:17-22`, not 3-5; `mutexAPI` id is ELF-specific; model line refs drifted | fixed in "Risks", "Corrections", "The model today" |
| r2 | Timer slack is 1 ns (`util/qemu-timer.c:677-678`); 26 s is a lower bound | Risks rewritten; "What latency costs"; gate waits for 0x142E before CONF, fails clearly by @55 (gate item 5) |
| r3 | ISR last-ditch busy-poll vs t_op | Risks; accepted-diffs entry (F-6) |
| r4 | Forbid UART `Read timeout`/write/erase timeouts | gate item 6 |
| r5 | Prove the watchdog trace is live | `stixctrl_watchdog_*` traced, ≥ 1 `cmd=1` required (gate item 2) |
| r6 | `latency=off` not byte-identical once the DMA result is checked; refused program sample undefined | D-F3; accepted-diffs (F-6) |
| r7 | Default suite: release ELF only | gate "Which ELF"; debug opt-in |
| r8 | Merge order vs gate-ports | F-1/F-2 after gate-ports merges into `stix-uprev-tc`; F-5 on `slot.sh` (Commits, Interactions) |
| C | Confirmed: root cause live on the debug ELF (FILE cur 1 / real 250, INIT waits on `mutexAPI`, WDG ready, `_ISR_Nest_level` 0, reset after `init: SAFE mode`); read counts (`memFilesystemLocal` 0x12200 vs 0x134b10 → exactly 64 vs 1,090 CBs, AX = `LIMIT_FS_SIZE=32`); AX gate ELF `DISABLE_WATCHDOG`, both PFM ELFs watchdog on; UART-golden inference sound with the F-3 stop rule; gate windows absorb +≈1 s; re-entrancy pattern matches `a83fe9a3a5`; F-5 non-vacuous and `--negative` a clean A/B | no change |
