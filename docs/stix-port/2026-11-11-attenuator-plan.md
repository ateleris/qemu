# Attenuator model with fault modes: plan

Status: validated 2026-11-11, review folded (see "Review resolutions"),
uncommitted. Base: track worktree `~/dev/qemu-stix-att` (branch
`stix-uprev-att`, from `d2c6cae28a` = detector D-1). **Implementation
starts after the platform track's PL-2 has landed** (the
`stixctrl_hk_value` truth/presented split, PL-2a). It is then rebased
onto the integration tip, which includes D-2 (`93438dfa3b`) and D-3.

The user wants to break hardware parts and watch what STIX does,
starting with the attenuator. A separate platform-test controller
(another track, `~/dev/qemu-stix-platform`, plan committed at
`644cd59628`) drives the faults over QMP and shows truth against
telemetry. Ownership is split as follows:
- **the platform track owns the fault-ID namespace and the controller
  catalogue** (D-PLq): the IDs, `expected_detection`, preconditions and
  the UI;
- **this track owns the device side:** the model and its QOM surface
  under `/machine/stixctrl/attenuator`.

Decisions recorded on 2026-11-11 (overridable defaults; Q1-Q6 stay open):
- **reduced scope:** A-1 and A-2 now; A-3 only with the faults that do
  not depend on Q2-Q5; the rest moves to "A-later";
- **the platform's fault IDs** are used, and the renames are listed;
- **`fsw-accesses`** exists so that the UI can label the AX artefact;
- **a SKIP exits 77**, and gate records list SKIP apart from PASS;
- **att-smoke has an AX subset** instead of a bare SKIP, and builds on
  the gateports slot library;
- **accepted-diffs:** the heading is "Accepted diff: attenuator model",
  with no § number until the merge.

Sources:
- the model (`hw/sparc/grlib_stixctrl.c`, `stix_det.[ch]`, `StixIdpu.h`,
  `tests/qtest/stix.c`, `scripts/stix/*`);
- a read-only pass over the FSW, `~/dev/STIX-FSW/ASW`. Paths are relative
  to it: **BSW** = `BSW_ASW/src/BSW/...`, **ASW** = `ASW/src/...`. Under
  ASW, the TM files are in `TcTm/TM/` (`TM/TMForm.c` below means
  `ASW/src/TcTm/TM/TMForm.c`), and `include/` is `ASW/src/include/`.
  Two references sit one level up, under `~/dev/STIX-FSW`:
  - the FSW team's TSIM model, `TSIM_IO/ASW_UT/src_io/iom/attenuatorTsim.c`;
  - their manual test, `FSW_IT/tests/attenuator_test.txt`;
- the platform plan (`qemu-stix-platform/docs/stix-port/2026-11-11-platform-plan.md`,
  committed at `644cd59628`): D-PLo, D-PLq, PL-2 and R-att;
- the gateports slot library `qemu-stix-gateports/scripts/stix/lib/slot.sh`
  (in progress);
- the gate artefacts in `docs/stix-port/baseline/` and the gate ELF
  `/tmp/stix-baseline-run/ASW_em_baseline.elf` (symbols read with `nm`);
- the private doc notes in `~/dev/stix-doc-notes/`. Only
  `notes-fdir-idpu.md` says anything about the attenuator (REQ-001,
  REQ-017, REQ-018). `notes-hswicd-dataflow.md` and
  `notes-fpga-arch-verif.md` do not cover the AttnCmdSta register.
  `sim-env-survey.md` §3.2 (state model) and §3.4 (fault catalogue) set
  the conventions used here.

Fact tags:
- **[C]** read in code (FSW or model), with file:line;
- **[E]** read in the gate ELF (`nm`);
- **[G]** seen in a gate artefact;
- **[D]** a document says it. Code wins where they disagree (see "Doc vs
  code");
- **[I]** inferred. Each one is a named knob or a single constant, so a
  wrong guess is cheap to change;
- **[A]** an assumption the user has to accept. Nothing is gated on one;
- **[L]** to be checked live before it becomes a gate expectation.

## Goal

1. The CTRL attenuator controller (APB 0x5C, AttnCmdSta) runs a
   mechanism model: two motors, a position, two position switches (AB,
   BC), move timing on the virtual clock, the ATT_C current, and reset.
2. Three layers can be read or set over QMP `qom-get`/`qom-set`:
   - **truth**: the real mechanism state;
   - **presented**: what the FPGA and the FSW see (the switches, the
     status bits, ATT_C), which may be broken;
   - **faults**: named failures from a catalogue with dotted IDs, kept
     separate from low-level **forcing** of a register value.
3. For each fault, the plan states the FSW-visible outcome (EIDs, mode,
   RCR), marked [L] until a live run confirms it.
4. A qtest per commit, and an optional `att-smoke.sh` gate.

## Corrections to the brief

These change what the track can deliver. Read them first.

1. **The gate ELF never touches the attenuator hardware.**
   `ASW_em_baseline.elf` is an **AX** build. It contains the AX stub's
   statics `isAB.555125`, `isBC.555126` and `moveStartCoarseT.555127`
   [E]. The Windows-tree `ASW_em.elf` is AX too [E].
   - The validator also scanned every function for SPARC `ld`/`st` at
     offset 0x5C:
     - `Attenuator_exeCmd` and all `AttCtrl_*` have none;
     - the only hits in the whole ELF are two unrelated ones;
     - `Attenuator_setPWMLevels` has 4 accesses at 0x60 [E].
   - In AX the driver is a pure software stub (BSW
     `Attenuator/Attenuator.c:20-72, 149-195`):
     - `Attenuator_exeCmd` writes **no** register;
     - AB = command & 3 and BC = command & 12;
     - a move lasts until the SCET second has advanced by 4;
     - timeout and fail always read false.
   - Only the PFM branch (`:73-107, :197-239`) writes and reads AttnCmdSta.
   - The non-Windows Makefile hard-codes `-DHW_VERSION_AX_IDPU`
     (`ASW/Makefile:94-98`). The Windows `tasks.vs.json` has three PFM
     configurations (`:62, :74, :98`).
   - The trace golden has **0** accesses to 0x5C in 90 s and 12 to 0x60,
     AttnConfig1, written by the common `Attenuator_setPWMLevels` [G].
   - **So on the gate ELF the model is invisible.** The FSW cannot see a
     stuck switch, a dead motor or a slow move. One exception is ATT_C:
     the HK ADC is real on both builds, so the current faults are visible
     on AX too, through HK monitoring and the HW protection.
   - Everything move-related needs a **PFM ELF with DEBUG_PRINT**, built
     by the user (Q1). The model and its qtests do not depend on the ELF.
   - On an AX ELF, `att-smoke.sh` runs only its **AX subset**: the ATT_C
     current fault gives 0x9434 (A-4). It does not just SKIP.
   - **On AX the platform's truth-vs-telemetry is meaningless.** The
     stub reports AB/BC from the last command (BSW `Attenuator.c:65-66`),
     while the model never sees a write.
     - After a TC close, SID2 says IN but `truth.position` says out, with
       no fault injected.
     - The device therefore exposes `fsw-accesses` (D-Ah). While it reads
       0, the UI labels the item "FSW does not read the attenuator on
       this ELF", a known artefact, not a mismatch.
2. **One stuck switch never gives the correlation-failure TM.**
   `FDIR_attCorrelationCheck` (ASW `FDIR/FDIR.c:1289-1374`) flags the
   sensors only when they read the **opposite end**:
   - after an open: AB = 0 and BC = 1;
   - after a close: AB = 1 and BC = 0.
   One stuck switch gives (1,1) or (0,0) and passes. The positive
   0x5436 cases are:
   - a dead or jammed motor (the mechanism stays at the start end);
   - two stuck switches that read the opposite end;
   - a lost motor-used flag.
   No plausible model error produces 0x5436 there either: the presented
   (1,1) and an un-applied fault (0,1) both pass. So "silence" is weak
   evidence. The FSW-side evidence comes from telemetry:
   - SID2 status4 carries AB (bit 14) and BC (bit 13)
     (`HK/HKAcquire.c:378-379`);
   - a stuck BC closed at OUT sets both.
   The gate checks the SID2 bits and the QMP truth (A-4).
3. **0x1620 and 0x1630 are never produced.**
   `FSW_ERROR_ATT_WRONG_POSITIONS` and `_MOTOR_AND_POSITION`
   (`include/commonFSW.h:78-80`) are unused. The check returns the raw
   masks 0x10 (motor) and 0x20 (sensor) (`FDIR.c:1279-1280`). They do
   not reach TM, which carries the five raw inputs. `AttCtrl` never sets
   `retVal` (FSW_OK = 0, `commonFSW.h:16`), so the TC completes with
   TM(1,7) even when the check fails (`HWControl/AttCtrl.c:93, 318`) [C].
   - **A TC move gets two TM(1,7), and the first is immediate.**
     - `HWConfig_powerOn`/`_powerOff` return `HWConfig_attenPowerOn/Off`
       without sending a report (`HWConfig.c:853, 946`).
     - `TCService236_execute` then `break`s and sends its own report
       (`TcTm/TcServices/TcService236.c:96-106, 213`), whatever the
       comment at `:95` says.
     - The second TM(1,7) comes at FINISH (`AttCtrl.c:316-320`). This is
       the two-TM(1,7) pattern §23 records for TC(236,6) [C].
   - So **a TM(1,7) never proves that a move ended**. The gates use the
     move outcome instead: the QMP truth, the EIDs, and the forbids on
     0x942B.
   - A TC rejected for LV off or busy probably gives TM(1,8) and then
     TM(1,7): HWConfig reports (`HWConfig.c:933-944`), and the service
     reports the return value again (`:213`) [C/L].
4. **The TC is TC(236,4/5) with system 4**, and it is accepted only in
   MAINTENANCE or CONFIGURATION (`TcTm/TCVerifDynamic.c:64-65`):
   - "power on" = open = OUT (uncovered);
   - "power off" = close = IN (covered) (`HWControl/HWConfig.c:586-684`;
     `attenuator_test.txt`).
   - The subsystem byte is the motor set: 1 = M1, 2 = M2, 3 = both, and
     4/8/12 the same with switch override (`HWConfig.h:135-146`).
   - LV must be on and the attenuator idle. Both are pre-checked in
     HWConfig (`HWConfig.c:839-849, 933-943`) and again in execCmd
     (`AttCtrl.c:419-447`).
5. **There is no attenuator interrupt.** The IRQ list has none
   (BSW `include/private/StixIdpu.h:1684-1693`). The FSW polls every
   50 ms (`AttCtrl.c:133-148`).
6. **`ATTENUATOR_DELAY` (100) is unused** (`commonFSW.h:155`; no
   reference in the tree). The poll period is 50 ms.

## Key facts the plan rests on

### Register (StixIdpu.h:696-740, the model's copy) [C]
- AttnCmdSta 0x5C:
  - COMMAND 3:0: 1 open M1, 2 open M2, 3 open both, 4 close M1, 8 close
    M2, 12 close both, 6 emergency shutdown;
  - ERRORMASK 4 (the "switch override");
  - TIMEOUT 5, MOTOR1USED 6, MOTOR2USED 7, OPERATIONSTATUS 8
    (1 = finished), POSITIONBC 9, POSITIONAB 10, FAILFLAG 11;
  - reserved 31:12.
- AttnConfig1 0x60: FASTRAMPTIMEQUANT 31:24, SLOWRAMPTIMEQUANT 23:12.
  The FSW writes 0x1E0E4000 (fast 30, slow 228, from the default PWM
  levels nominal 70 % and max 20 %, `ParamMgmtDef.h:212-213`) [G]. 0x64
  is `notUsed0` here; the TSIM model calls it ATTNCONFIG2. The FSW never
  touches 0x64.
- The PFM exeCmd writes `cmd` or `cmd | 1<<4` as the whole register
  (BSW `Attenuator.c:97-104`). The PFM getters read single bits
  (`:199-239`).
- The model today stores 0x5C/0x60 as plain registers
  (`grlib_stixctrl.c:901`). A PFM ELF would read
  OPERATIONSTATUS = 0, "moving", forever.
- The TSIM model is plain storage too (`attenuatorTsim.c:29-83`). There
  is no reference behaviour to copy.

### The FSW's move loop (ASW `HWControl/AttCtrl.c`) [C]
- 50 ms RTEMS timer → `AttCtrl_dataAcquisitionHandler` (`:133-148,
  :156-338`).
- **NO_MOVE** (`:160-168`) takes one sample *before* the command and
  then goes to INIT_LOOP on the next tick. INIT_LOOP clears
  `lastMoveUsed*` (`:174-175`), but the sample counts toward the 66.
- **INIT_LOOP** (`:171-235`):
  - `FDIR_AttenuatorMaxCycleRate`;
  - the disabled-motor checks: a disabled motor sends the move to the
    FDIR state, with no TM and no TM(1,7) (`:186-209, :331-336`);
  - `Attenuator_exeCmd`, then **one sample right after the write**;
  - the inferred state is set from the direction (`:229-231`).
- **MOVE**, each tick (`:237-260`): sample; then **FAILFLAG → FAIL**, else
  **OPERATIONSTATUS → DONE**, else **TIMEOUT → TIMEOUT**, else 66 samples
  → TIMEOUT. With the pre-sample this is **about 3.2 s after the write**;
  this plan uses that figure (the `AttCtrl.c:39` comment says 3.3 s).
- **DONE** (`:262-292`): `FDIR_attCorrelationCheck(cmd, AB, BC,
  usedM1, usedM2)`. On failure it disables the commanded motor(s).
- **FAIL / TIMEOUT** (`:294-310`): with **FINISH** (`:312-329`) this sends
  the second TM(1,7) if a TC started the move (correction 3), and TM(5,3)
  **0x942B** (only on FAIL or TIMEOUT).
- The sample (`:340-359`):
  - reads ATT_C through a live HK ADC conversion
    (`HK/HKAcquire.c:519-535`) and sums it only if it is < 0x100;
  - stores the 5-bit flags;
  - ORs MOTOR1USED/MOTOR2USED into `lastMoveUsedMotorX`, since "this is
    reset at the end" (`:387-388`).
- `AttCtrl_execCmd` (`:394-496`):
  - global disable → silent;
  - LV off → FID 48424 or EID 0x5444;
  - busy → FID 48426 or EID 0x5443;
  - a manual (TC) move re-enables the motors it uses, with TM(5,1)
    **0x1423** `[motor u16, reason 2 = manual]` if a motor was disabled.
- `moveIN`/`moveOUT` (autonomous) use `ATT_AUTONOMY_MOTOR_USE` (default 0
  = M1; `ParamMgmtDef.h:546`; `AttCtrl.c:499-549`) and never re-enable.

### FDIR [C]
- **Correlation, REQ-017** (`FDIR.c:1289-1374`):
  - it runs only with autonomy on (set at init, `:370`) and with
    `FDIR_ENABLED_FUNCTIONS` & 0x10000 (the default is 0x3114C, on;
    `ParamMgmtDef.h:154`);
  - the motor check (`:1304-1338`): a single-motor command needs exactly
    that motor's flag; a both-motor command needs at least one;
  - the sensor check: see correction 2;
  - on failure: TM(5,2) **0x5436** `[cmd, AB, BC, m1, m2]`, five u8
    (`TM/TMForm.c:590-600`, `TM_5.c:273-275`).
  - No mode change: "STIX still continues observing".
- **Cycle rate, REQ-018** (`FDIR.c:1387-1470`):
  - more than N1 = 6 moves in T1 = 30 s, or N2 = 10 in T2 = 300 s
    (`ParamMgmtDef.h:160-163`), disables both motors and sends TM(5,2)
    **0x5427** (6 × u32);
  - recovery comes with the next in-limit move (0x1423, reason 1);
  - T1 and T2 are **fixed blocks** that restart when they expire, not
    sliding windows (`FDIR.c:1392-1407`);
  - after the 7th move the 8th TC goes through: execCmd re-enables the
    motors (0x1423, reason 2), and `disabledMotors` = TRUE makes the
    check return without disabling again (`FDIR.c:1446-1450`) [C].
- **0x942B** `[cmd, motors = flags>>3 & 3, sensors = flags & 3, timeout]`,
  four u8 (`TMForm.c:709-718`, `TM_5.c:374-376`).
- **HK**:
  - ATT_C is HK id 6 (`HK/HKData.h:161`);
  - the software limits follow the active motors (`FDIR/HKMonitor.c:274-293,
    436-440`). PFM: 0 motors 0..166, 1 motor 3..629, 2 motors 28..870;
    AX: 0..4095 (`ParamMgmtDef.h:33-37, 82-86`). Phase 4 found the
    persistence count to be 255 on this build (§27);
  - the HW protection: ATT_C threshold 751 at reset (Phase 4 D-P4d),
    armed with the rest at 0x14A3 (`FDIR.c:535-546`). A trip sends 0x9434
    `[6, value, threshold]` and signals SAFE, gated by `b_boot_generated`
    (`FDIR.c:890-897`);
  - **NOMINAL entry re-applies the ATT protection**: the threshold and
    the OCP enable, from parameters captured at boot
    (`SWStatus/OpModeMgmt.c:532-542`; capture in `initStartingHWparameters`,
    `:1015-1023`). CONF entry does not, so a CONF trip stands. A future
    NOMINAL gate checks live whether ATT_C OCP is still enabled there [L].

### Other readers (PFM only) [C]
- SAFE entry waits 3 s if OPERATIONSTATUS = 0, then runs
  `RateCtrl_reset(FALSE)` ("never move ATT") (`SWStatus/OpModeMgmt.c:173-180`).
- Leaving NOMINAL (to SAFE or CONF) runs `RateCtrl_deinit`, which moves OUT
  if the RCR state has the attenuator in (`OpModeMgmt.c:140, 303`,
  `RateCtrl.c:1350-1363`).
- `RateCtrl_reset(TRUE)` (the move-OUT branch, `RateCtrl.c:1030-1041`) has
  **no caller**. The only calls are `RateCtrl_reset(FALSE)`, at
  `RateCtrl.c:695` and `OpModeMgmt.c:180`.
- The RCR moves IN or OUT on an RCR crossing (`RateCtrl.c:494-516`). It
  never reads the switches.
- SID2:
  - status4 bit 20/21 = "M1/M2 active" (`!finished && used`), and bits
    14/13 = AB/BC (`HKAcquire.c:343-353, 378-379`);
  - `attCurrentSummed` (`:258`) and `attMotionTotal` (`:432`);
  - SID4 `attMotionFlag` (`HKReport.c:157`).
- The flare detector uses the **inferred** state: the direction of the last
  started move, read from the switches only once while it is unknown
  (`AttCtrl.c:719-728`, `XrayDataRTProc/FlareDetector.c:164-166`).
- TC(239,20) dumps the last 20 moves as TM(239,21) (`TcService239.c:932-937`,
  `AttCtrl.c:557-645`).

### Timing [C/I]
- The FSW expects a move to end well within 3.3 s (66 × 50 ms) and within
  the 3 s REQ-001 wait (`commonFSW.h:156`).
- **The ramp [I], strongly supported.** The PWM ramp quants give 30·64 =
  1920 and 228·128 = 29184 time units (BSW `Attenuator.c:125-142`).
  - The formula fixes fast·div = 1953125/1000 = 1953.125 and slow·div =
    29296875/1000 = 29296.875 units, whatever the PWM levels
    (`Attenuator.c:127-130`).
  - At 51.2 µs per unit (20 MHz / 1024) that is exactly **100 ms +
    1.5 s = 1.6 s**.
  - So `att-ramp-ms` does not need to follow TC(236,6) PWM-level
    changes. The unit and "the drive stops at the ramp end" stay [I]
    (Q2).
- **The model takes the 1.6 s as the controller's drive window**: the
  motor ramps and then stops, flagging TIMEOUT if the target switch was
  not reached. This is the default of `att-ramp-ms` (Q2).
- **AX artefact [I/L].** The AX stub's move lasts 3-4 s depending on the
  sub-second phase, while the FSW gives up about 3.2 s after the write.
  So about 80 % of AX TC moves end in the FSW's own timeout, with TM(5,3)
  0x942B, whatever the model does. This is a reason not to gate on AX.

## Decisions (D-A)

- **D-Aa: a separate unit, `hw/sparc/stix_att.[ch]`**, built like
  `stix_det.c`:
  - `StixAttState` is embedded in STIXCTRL and bound at realize to
    `&s->reg` (for LVEN in hkCmdSta and AttnConfig1);
  - `stix_att_owns(addr)` covers **0x5C only**. 0x60 and 0x64 stay plain
    storage, so their 12 golden trace lines stay byte-identical;
  - `stix_att_read`, `stix_att_write`, `stix_att_hk_att_c()`,
    `stix_att_realize`, `_reset` and `_unrealize`.
  - **The QOM face is a child object** `TYPE_STIX_ATT` (a plain
    TYPE_OBJECT, not a device) at **`/machine/stixctrl/attenuator`**,
    created with `object_initialize_child` in the stixctrl instance_init.
    All fault and status properties live in `stix_att.c`'s class_init.
  - The hooks in `grlib_stixctrl.c` are about 25 lines: the include, the
    field, the read/write hooks, realize/reset/unrealize, instance_init,
    one qdev property block, and the ATT_C lines in the two halves of
    the PL-2a `stixctrl_hk_value` split (D-Af).
  - **Hook placement** (line numbers at `93438dfa3b`; re-check after
    PL-2):
    - **read:** between the 0x88 hook and the det hook (`:725-739`), in
      its own hunk;
    - **write:** right **after** the hk-prot block (`:763-788`) and before
      the det block (`:790`), as D-1 did. 0x5C writes then spend the
      unlock token like every other write (the D-P4d semantics). The
      write path has no early 0x88 hook (0x88 is masked late, `:881`);
    - **reset:** one call after `stix_det_reset` (`:1045`).
- **D-Ab: `att-model` (bool, default on)**, a qdev property on stixctrl
  like `det-model`. Off keeps 0x5C as plain storage, never reset, and
  ATT_C at the static 20 (legacy, for A/B comparison).
  - **Why on is safe:** the AX gate ELF never accesses 0x5C [G, E]. The
    unit emits no trace at realize or reset, and 0x60 is untouched, so
    **the UART golden and the trace golden do not change**.
  - **Why on is needed:** with off, a PFM ELF reads "moving" forever. It
    would then wait 3 s at each SAFE entry, show M1/M2 "active" bits as
    0 but still call the attenuator active in SID2, and end every move in
    the FSW's 3.3 s timeout (0x942B).
  - **The stop-and-justify check:** the FSW does check the attenuator at
    boot, but only on PFM, and never moves it:
    - `RateCtrl_reset(FALSE)` on SAFE entry (`OpModeMgmt.c:179-180`);
    - the 3 s wait reads OPERATIONSTATUS, which is idle = 1 here.
    No PFM golden exists, so no golden changes. **No stop needed.**
- **D-Ac: lazy evaluation, no timer.** The state is advanced to "now"
  (`qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)`) on every 0x5C access, every
  ATT_C conversion, and every QOM get or set. The next event times are
  solved analytically:
  - the target switch is seen (plus debounce);
  - the end or the block point is reached;
  - the ramp ends.
  The FSW polls every 50 ms, so it cannot tell the difference. Trace
  events for an edge carry the computed event time, not the time it was
  noticed.
- **D-Ad: the mechanism (truth).**
  - **Position** `pos_ppm`: 0 = OUT (uncovered), 1 000 000 = IN
    (covered). It starts from `att-initial` (out|in, default out: the AX
    stub's `isAB = true`, BSW `Attenuator.c:23-24`). It **survives
    device reset**, because the mechanism does not move on an IDPU reset.
  - **Switches (truth):** AB is actuated iff pos = 0 and BC iff
    pos = 10⁶; in transit both are released. FSW meaning: OUT = AB 1,
    BC 0 and IN = AB 0, BC 1 (`AttCtrl.c:723`) [C]. "Both released in
    transit" is [I].
  - **Motors:** M1 and M2 drive one shaft. Any live, powered, commanded
    motor moves it at the nominal speed; with two, the faster one sets
    the speed [A].
  - **The travel** end to end takes `att-travel-ms` (default **500** [A],
    Q3). A motor is powered iff hkCmdSta LVEN was set at the command
    (LV is sampled at the command [A]; the FSW refuses moves with LV off
    and never cuts LV mid-move).
- **D-Ae: the controller (the FPGA, presented to the FSW).**
  - **Write 0x5C** (bits 4:0 only; status bits are ignored on write):
    - **Commands 1/2/3/4/8/12** start a move:
      - OPERATIONSTATUS = 0, TIMEOUT = 0, FAILFLAG = 0;
      - MOTORxUSED = the commanded motors (set even for a dead motor:
        it is the drive flag [I]);
      - the ramp window starts.
      A new command during a move restarts with the new one [I]; the FSW
      never does this (busy check).
    - **6 (emergency)** stops now: OPERATIONSTATUS = 1, the flags clear,
      and the position stays where it is (possibly between the ends).
    - **Any other value** is stored for readback and does nothing [I].
  - **The stop rule, without override:** the move ends `att-debounce-ms`
    (default **10** [I]) after the **presented** target switch (AB for
    open, BC for close) reads actuated, including when it already reads
    actuated at the start.
    - Then OPERATIONSTATUS = 1 and MOTORxUSED = 0 (`AttCtrl.c:387`
      comment) [I].
    - The FPGA sees the same physical switch as the FSW, so a stuck
      switch also changes when the motor stops.
    - Why 10 ms, not 0: with 0, a redundant move would clear MOTORxUSED
      before the FSW's sample right after the write. The FSW would then
      disable the motor with 0x5436 on every redundant move, which
      flight would have shown [I].
      - The redundant moves that occur: an operator TC toward the end
        the mechanism is already at, and `RateCtrl_deinit` moving OUT
        while the RCR state says IN (`OpModeMgmt.c:140, 303`), e.g. after
        a dead M1.
      - `RateCtrl_reset(TRUE)` is not one: it has no caller.
  - **The ramp end:** if the target was not seen by `att-ramp-ms`
    (default **1600** [I]), the move ends with TIMEOUT = 1. With
    `att-timeout-finishes` (default **on** [I], Q4) OPERATIONSTATUS = 1
    too, and the FSW goes to DONE and the correlation check. Off leaves
    OPERATIONSTATUS = 0, and the FSW goes to TIMEOUT and 0x942B. The FSW
    is consistent with both readings, so this is a knob.
  - **Override (ERRORMASK)** ignores the switches. The move always runs
    the full ramp and ends without TIMEOUT [A] (Q4).
  - **Read 0x5C**:
    - bits 4:0 = the last written command and override;
    - TIMEOUT, MOTOR1USED, MOTOR2USED, OPERATIONSTATUS;
    - BC and AB = the presented switches;
    - FAILFLAG;
    - then the forcing masks (D-Ag).
    At reset and idle at OUT it reads **0x500**; idle at IN, 0x300 (plus
    the readback).
  - **Reset** (system or watchdog): a running move is aborted at the
    reset instant; the registers become idle (OPERATIONSTATUS = 1, the
    rest 0); the position is kept.
- **D-Af: the ATT_C current (HK ADC channel 8).**
  - **Layering, on top of PL-2a's split.** `_truth` and the presented
    wrapper are the platform's names:
    - `stixctrl_hk_value_truth(ch 8)` = `stix_att_att_c_truth()` (the
      physics below);
    - the presented wrapper: if `adc-inject[8]` is set it wins (forcing);
      otherwise it returns `stix_att_att_c_presented()`, which applies the
      `attenuator.current.*` faults to the truth.
    - So the platform's `hw-state` `hk_adc.8` shows truth = the
      mechanism's current and presented = what the FSW converts. The
      ordering is: truth → named fault → forcing.
  - **truth** = 20 + Σ over the powered, live, driven motors of 100 while
    the shaft moves and 300 while it is driven but not moving (at an end
    or a block point, or jammed) [A].
    - So: idle 20; one motor moving 120; two 220. Both are < 0x100, so
      they are summed (`AttCtrl.c:344`), and inside the PFM 1-motor and
      2-motor windows.
    - Stall: one motor 320, two 620. Both are < 751 (no HW trip), and
      excluded from the sum.
    - A dead motor draws 0.
  - **presented** = 0 with `current.zero`; otherwise clamp(truth +
    `current.offset`) in A-3. A-later adds `current.scale`:
    clamp(truth·scale/1000 + offset).
  - `adc-inject[8]` (P4-2 forcing) still beats everything, as the
    existing order in `stixctrl_hk_value` already does.
  - ATT_V stays derived from LVEN, and ATT_T stays static 2450. There is
    no thermal model.
  - The comparator samples ATT_C at conversions only; a fault set does
    not sample. The FSW converts ATT_C every 1 s, and every 50 ms during
    a move.
    - This differs from the `adc-inject` setter, which samples at once
      (`grlib_stixctrl.c:1102`). An `attenuator.current.*` set trips
      at the next conversion, up to 1 s later.
    - D-Af and `ground-station.md` say so.
- **D-Ag: faults and forcing (the catalogue, below).**
  - **The IDs are the platform's** (D-PLq, R-att). This track implements
    them as device properties and lists any rename or new ID in the
    catalogue section, for the platform track to adopt.
  - **Named failures** are component or signal faults. They change truth
    or presented through the model's physics (a stuck switch also fools
    the FPGA's stop rule).
    - **Note for the platform track:** D-PLo says that breaking a
      component "changes `presented`, never `truth`". That holds for the
      signal faults (sensors, current).
    - A component fault (`motorN.dead`, `mechanism.jammed`) changes the
      mechanism itself. The device's `truth.*` is the physical mechanism
      *after* the fault (a dead motor leaves it at OUT).
    - The controller shows the item as `fault` through `fault_ids`
      (D-PLp), not as a truth change caused by the UI. R-att's
      "presented only" wording needs this one amendment.
    - Persistence: **until cleared**, and they **survive device reset**
      (a broken switch stays broken when the FSW reboots). This differs
      from Phase 4's D-P4b, and the accepted-diff section records it.
  - **Forcing** is a separate thing: `force.cmdsta-set` and
    `force.cmdsta-clear` (uint32 masks, bits 11:5 only, otherwise
    rejected). They are applied to what a 0x5C read returns, after
    everything else. No physics, the FSW's view only. Cleared on device
    reset, like `adc-inject`.
  - **Injecting during a move:** the state is advanced to now under the
    old faults, then the new value applies from now (piecewise).
  - **Every set or clear** emits trace `stixatt_fault(id, value)`, which
    the controller's JSONL can mirror.
  - **Rejected sets** fail with an error and change nothing:
    - any fault or forcing set while `att-model=off`;
    - a set on a read-only `truth.*`, `presented.*` or `fsw-accesses`;
    - an out-of-range parameter;
    - an unknown ID in `fault-clear`.
- **D-Ah: the QOM surface.** All of it is under
  `/machine/stixctrl/attenuator` unless marked qdev.
  - qdev on stixctrl (static, `-global grlib-stixctrl.X=`):
    `att-model`, `att-initial`, `att-travel-ms`, `att-ramp-ms`,
    `att-debounce-ms`, `att-timeout-finishes`.
  - truth (read-only):
    - `truth.position` (out|in|moving-in|moving-out|between);
    - `truth.position-ppm`;
    - `truth.sensor-ab`, `truth.sensor-bc`;
    - `truth.motor1-driving`, `truth.motor2-driving` (torque, not the
      flag);
    - `truth.att-c`;
    - `truth.moves`;
    - `truth.last-end` (none|target|timeout|override|emergency|fail|reset)
      and `truth.last-end-ns`.
  - presented (read-only):
    - `presented.sensor-ab`, `presented.sensor-bc` (as the FSW reads
      them);
    - `presented.cmdsta` (exactly what a 0x5C read returns now, with no
      side effects);
    - `presented.att-c` (including `adc-inject[8]`).
  - **`fsw-accesses`** (uint64, read-only): the number of guest
    0x5C accesses (reads and writes) since realize. It is not cleared on
    reset.
    - On the AX gate ELF it stays 0, and the UI labels the attenuator
      item as the known AX artefact (correction 1).
    - att-smoke uses it to tell a PFM ELF from an AX one at runtime
      (A-4).
  - faults (read/write): one property per implemented catalogue ID,
    **named by the platform's full dotted ID**, so `qom-list` builds the
    tree for the controller.
  - verbs: `fault-clear` (write: an ID or "all") and `faults-active`
    (read: "id=value,...").
    - The JSON `fault-catalogue` property is **deferred** (A-later). The
      platform catalogue (D-PLq) is the description for now.
  - forcing: `force.cmdsta-set`, `force.cmdsta-clear`.
  - **`hw-state`:** once PL-2's stixctrl `hw-state` exists, the device
    adds an `attenuator` object (truth, presented, `fsw_accesses`,
    `faults_active`) through one accessor, `stix_att_hw_state(QDict *)`.
    This matches the platform's `hw-state.attenuator` (R-att). The
    properties above stay the per-item interface.
  - Dots in QOM property names are a check in the A-1 qtest [L]. If
    QMP refuses them, the names fall back to `-` with the ID kept in the
    catalogue.
- **D-Ai: re-entrancy (the Phase 4 double-free lesson,
  `a83fe9a3a5`).**
  - The unit has **no timer, no BH, no DMA, no IRQ** and never calls
    `address_space_*` or a stixctrl MMIO handler.
  - QMP getters and setters only advance the pure state and store
    fields.
  - The only call from the parent is `stix_att_hk_att_c()` inside
    `stixctrl_hk_value`, which runs from MMIO dispatch or from the
    `adc-inject` setter. Both are pure computation.
  - With no timer, BH, DMA or IRQ there is no re-entrant path, so the
    `a83fe9a3a5` guard rule (out-of-MMIO DMA) does not apply. It must be
    revisited if a later tier adds a timer or any `address_space_*` call.
  - **Getters never trace.** A trace is emitted only by an advance from
    an MMIO access or a conversion, and edge traces carry the computed
    `at_ns`. Otherwise the platform's 2 Hz polling would make the order of
    the trace lines depend on the host.
  - A qtest checks the lazy design's real invariant, **determinism**:
    the same `clock_step` script gives identical 0x5C and ATT_C reads
    with and without interleaved qom-gets. This replaces a
    "no-crash storm".
- **D-Aj: tests in their own binary**, `tests/qtest/stix-att.c` (a meson
  entry next to `'stix'`, `tests/qtest/meson.build:215`). This keeps
  conflicts with the detector track's edits to `stix.c` low. The suite
  recipe gains one command.
  - **The binary copies the temp-flash helper `stix_qtest_new_extra`
    verbatim** (`stix.c:86-109`: a 1 MB sparse temp file passed as
    `-global grlib-stixflash.flash-file=`).
    - Without it, every QEMU the test starts creates a 16.5 GB
      `flash.bin` in the meson CWD.
    - Copying is preferred over a shared header, which would be a `stix.c`
      edit.

## Fault catalogue

The IDs are the platform track's (D-PLq, R-att). Every fault is
persistent until cleared, survives a device reset, and is reversible. The
"inactive" value is the default, and `fault-clear` sets it back.

### A-3 set (the device semantics do not depend on Q2-Q5)

| ID | class | mode / param (inactive) | effect on truth (T) / presented (P) | ID source |
|---|---|---|---|---|
| `attenuator.sensor_ab.stuck_open` | signal | stuck_at 0, bool (false) | P: AB reads 0 for the FPGA and the FSW | platform |
| `attenuator.sensor_ab.stuck_closed` | signal | stuck_at 1, bool | P: AB reads 1 | platform |
| `attenuator.sensor_bc.stuck_open` | signal | stuck_at 0, bool | P: BC reads 0 | platform |
| `attenuator.sensor_bc.stuck_closed` | signal | stuck_at 1, bool | P: BC reads 1 | platform |
| `attenuator.motor1.dead` / `motor2.dead` | component | open circuit, bool | T: no torque and no current; the flag is still set [I] | platform |
| `attenuator.mechanism.jammed` | component | bool | T: no motion; driven motors stall | **new** (proposed to the platform) |
| `attenuator.current.zero` | signal | bool | P: ATT_C reads 0 at all times ("motion with no current") | platform |
| `attenuator.current.offset` | signal | offset, int32 raw counts (0) | P: ATT_C + offset, at all times; the AX-subset driver | **new**, renamed from `att_c.offset` |

### A-later (waits for a PFM ELF and the HSWICD AttnCmdSta section, Q1/Q2)

| ID | class | mode / param (inactive) | effect | ID source |
|---|---|---|---|---|
| `attenuator.move.slow` | component | scale, uint32 ‰ of travel time (0 = off; 2000 = 2× slower), both motors | T: slower shaft | platform; replaces the draft's `motor1.slow`/`motor2.slow` (a per-motor variant can be proposed later) |
| `attenuator.mechanism.partial` | component | block point, uint32 ‰ of the travel from the start end (0 = off) | T: stops there and stalls until the ramp ends | new |
| `attenuator.motor1.flag_lost` / `motor2.flag_lost` | signal | stuck_at 0, bool | P: MOTORxUSED reads 0 | new |
| `attenuator.controller.fail_flag` | component | bool | P: FAILFLAG reads 1 while set; T: the controller stops driving [A] | new |
| `attenuator.current.scale` | signal | scale, uint32 ‰ of the truth (1000) | P: ATT_C × scale | new, renamed from `att_c.scale` |

The JSON `fault-catalogue` property is A-later too.

**Renames from the draft (for the platform catalogue):**
- `att_c.offset` → `current.offset`, and `att_c.scale` → `current.scale`.
  These fit the platform's `current.zero`.
- `motor1.slow`/`motor2.slow` → `move.slow`.
- New IDs the platform catalogue does not yet have: `mechanism.jammed`,
  `current.offset`, and, for A-later, `mechanism.partial`,
  `motorN.flag_lost`, `controller.fail_flag` and `current.scale`.
- The platform track adopts them or renames them before A-3. A rename
  is then one string per property.

"stuck_open" means the register bit reads 0 and "stuck_closed" means it
reads 1. The electrical polarity of the real switch (whether actuated is
a closed contact) is not known [A] (Q5). The names follow the brief.

## FSW-visible outcome per fault

Setup: CONFIGURATION, LV on, FDIR on (after 0x14A3), PFM ELF, defaults
(travel 500, ramp 1600, debounce 10, timeout-finishes on), start at OUT.
**C** = TC(236,5) `04 01` (close with M1 → IN); **O** = TC(236,4) `04 01`.
- Every TC move gets TM(1,1), an **immediate** TM(1,7) from the service,
  and a second TM(1,7) at FINISH (correction 3).
- No fault in this table changes the mode, except the current rows.
- Everything here is [L].
- Rows marked *(A-later)* use the deferred faults.

| fault | command | truth | presented at the end | FSW reaction |
|---|---|---|---|---|
| none | C | IN at 0.5 s | AB 0, BC 1 | none (check OK) |
| none | O at OUT | OUT, ends at 10 ms | AB 1, BC 0 | none |
| motor1.dead | C | OUT | AB 1, BC 0, TIMEOUT | **0x5436 `04 01 00 01 00`**; M1 disabled (no TM); inferred state IN |
| — then | C again | IN | — | **0x1423 `0000 0002`** (manual re-enable), then OK |
| motor1.dead | C, both motors (`04 03`) | IN (M2 moves it) | AB 0, BC 1 | none; only ATT_C (120, not 220) shows it |
| move.slow 2000 *(A-later)* | C | IN at 1.0 s | AB 0, BC 1 | none; more samples in TM(239,21) |
| move.slow 4000 *(A-later)* | C | between, at 80 % | AB 0, BC 0, TIMEOUT | none (check passes); the inferred state is IN |
| mechanism.partial 500 *(A-later)* | C | between, at 50 % | AB 0, BC 0, TIMEOUT | none |
| mechanism.jammed | C | OUT | AB 1, BC 0, TIMEOUT | 0x5436 `04 01 00 01 00`; ATT_C 320 for about 1.6 s |
| sensor_bc.stuck_open | C | IN; stalls until 1.6 s | AB 0, BC 0, TIMEOUT | none |
| sensor_bc.stuck_closed | C at OUT | OUT (ends at 10 ms) | AB 1, BC 1 | **none; the FSW thinks IN** (inferred); SID2 status4 bits 14 and 13 both set (the gate's evidence) |
| sensor_ab.stuck_closed | O at IN | IN (ends at 10 ms) | AB 1, BC 1 | **none; the FSW thinks OUT; the attenuator is stuck in** |
| sensor_ab.stuck_open | O at IN | OUT; stalls until 1.6 s | AB 0, BC 0, TIMEOUT | none |
| ab.stuck_open + bc.stuck_closed | O | OUT | AB 0, BC 1 | **0x5436 `01 00 01 01 00`**; M1 disabled |
| ab.stuck_closed + bc.stuck_open | C | IN | AB 1, BC 0 | 0x5436 `04 01 00 01 00` |
| motor1.flag_lost *(A-later)* | C | IN | AB 0, BC 1; m1 0 | 0x5436 `04 00 01 00 00` (motor info); no SID2 bit 20 |
| controller.fail_flag *(A-later)* | C | OUT (stopped) | FAILFLAG | **TM(5,3) 0x942B `04 01 01 00`** at the first MOVE tick |
| any TIMEOUT row with `att-timeout-finishes=off` | | as above | OPERATIONSTATUS 0 | 0x942B `cmd motors sensors 01` instead of the check; no disable. **The next SAFE entry waits 3 s** |
| current.offset 800 (any build, AX too) | none | — | ATT_C 820 | **0x9434 `06 0334 02EF`**, a SAFE request; from CONF `0021 04 02` (in SAFE: no mode change) |
| current.zero (any build) | none / C | as without the fault | ATT_C 0 | none on AX (0..4095). On PFM, 0 is below the 1-motor minimum 3 and the 2-motor minimum 28 during a move, but the 1 s SW check needs 255 consecutive samples (§27), so no TM. TM(239,21) currents read 0 |
| current.scale 4000 *(A-later)* | C, both | IN | 220 × 4 = **880** while moving | 0x9434 during the move; SAFE |

**RCR (NOMINAL, autonomous) behaviour [C/L]:**
- The same mechanics apply with M1 (the default autonomy motor), with no
  TC and no TM(1,7).
- After one 0x5436, M1 **stays** disabled (autonomy never re-enables it).
  Every later autonomous move goes silently to the FDIR state, while the
  RCR state machine moves on (it never reads the switches) and the
  inferred state keeps the last *started* direction.
- So a dead M1 during a flare gives:
  - one 0x5436 on the IN move;
  - the flare detector believing IN while the truth is OUT;
  - a silent OUT at the flare end.
- This needs the detector track's synthetic flare (D-4) to drive RCR.
  It is a future gate, not one here.

**REQ-018:** 7 TC moves inside one 30 s T1 block (fixed, not sliding)
give 0x5427 and disable both motors.
- The 7th TC is re-enabled by execCmd and then disabled again in
  INIT_LOOP, so it goes to the FDIR state. It gets **only the immediate
  TM(1,7)**, with no FINISH TM(1,7) and no move (an FSW quirk,
  `AttCtrl.c:331-336`) [C/L].
- The 8th TC moves: it is re-enabled (0x1423 reason 2) and not disabled
  again (`FDIR.c:1446-1450`) [C/L].
- This is nominal FDIR, not a fault, and an optional gate step.

## Commits

Each commit is test-first: the new qtests are written and fail (RED)
against the previous commit, then the code makes them pass. The gates
listed are run before the commit.

**Prerequisite for A-1:** the platform's PL-2 has landed (the
`stixctrl_hk_value` split, PL-2a), and the branch is rebased onto it and
onto D-2/D-3.

**The gate set per commit (on AX the model is unreachable by
construction):**
- **per commit:** the suite (both binaries), checkpatch, the probe
  self-test, boot-smoke (**the UART is identical**), and a manual
  trace-golden diff (no `stixatt_*` lines; 0x60 identical);
- **at A-3 and at the merge, the full set:** in addition, soak 300 s,
  tc-smoke, nominal-smoke ±, fault-smoke ±, detector-smoke ± and
  att-smoke (AX subset);
- **gate records list SKIP separately from PASS** (a SKIP exits 77).

### A-1: mechanism and AttnCmdSta (no faults)
- `stix_att.[ch]`, the child object, the hooks (placement per D-Aa),
  `att-model` and the timing properties, the truth/presented read-only
  properties and `fsw-accesses` (D-Aa..Ae, Ah, Ai). Traces:
  `stixatt_cmd(cmd, override, pos_ppm)` and
  `stixatt_end(reason, at_ns, pos_ppm)`.
- qtests (`stix-att`, driving the exact FSW words; LV on through hkCmdSta
  LVEN; the temp-flash helper per D-Aj):
  - `att-reset`: 0x5C reads 0x500; `truth.position` = out;
    `fsw-accesses` counts the reads.
  - `att-close-open`:
    - write 4: OPERATIONSTATUS 0, M1USED 1, AB 1;
    - +250 ms: AB 0, BC 0, moving-in;
    - +510 ms: BC 1, OPERATIONSTATUS 1, M1USED 0;
    - write 1: back to OUT.
    The same for M2 (2/8) and both motors (3/12).
  - `att-redundant`: write 1 at OUT. M1USED reads 1 right after the write
    and 0 after 10 ms.
  - `att-override`: write 4|0x10. It reaches IN at 500 ms but ends at
    1600 ms, with no TIMEOUT.
  - `att-lv-off`: no motion; TIMEOUT and OPERATIONSTATUS at 1600 ms.
  - `att-emergency`: write 6 at 250 ms. It stops `between`, both switches
    0, OPERATIONSTATUS 1.
  - `att-reset-midmove`: `system_reset` at 250 ms. The position is kept,
    the registers are idle, and `truth.last-end` = reset.
  - `att-initial-in`: `-global grlib-stixctrl.att-initial=in` reads
    0x300.
  - `att-config1`: 0x60 round-trips 0x1E0E4000 and the trace is
    unchanged; 0x64 is untouched.
  - `att-model-off`: 0x5C is plain storage and reads 0 at reset.
  - `att-qom-names`: a dotted property name works over QMP.
  - `att-determinism` (replaces a re-entry storm, D-Ai): the same
    `clock_step` script gives identical 0x5C and ATT_C reads with and
    without interleaved qom-gets, and no `stixatt_*` trace fires inside
    a getter.
  - **Negative qtests** (`att-reject-*`):
    - status bits 11:5 in a write are ignored;
    - the unused command values 0, 5, 7, 9-11 and 13-15 are stored for
      readback and cause no motion;
    - a command during a move restarts it [I];
    - qom-set on `truth.*`, `presented.*` and `fsw-accesses` is
      rejected.
- Gates: the per-commit set.
- The accepted-diff section (a).

### A-2: ATT_C current profile
- `stix_att_att_c_truth()` and `_presented()`, wired into the two halves
  of the PL-2a split (D-Af). The `grlib_stixctrl.c:410-418` comment is
  replaced.
- qtests: hkAdc ch 8 reads
  - 20 idle, 120 for one motor moving, 220 for two;
  - **override close:** 120 until 500 ms, then 320 (stalled at IN) until
    1600 ms, then 20;
  - **a two-motor override close:** 220, then 620;
  - 20 again after the end.
  Also:
  - `adc-inject[8]` wins over everything;
  - the platform's `hw-state` `hk_adc.8` shows truth = presented with
    no fault;
  - with hk-prot armed (unlock + ProtEn on line 5), the 620 stall never
    trips 751;
  - `att-model=off` reads the static 20.
- Gates: the per-commit set. The fault-smoke DET_C/1V5/PSU_T steps are
  unaffected.

### A-3: the reduced fault set, forcing and verbs
- The A-3 catalogue rows only, `fault-clear`, `faults-active`, the
  forcing masks, the piecewise inject during a move, the persistence
  rules and the rejections (D-Ag), and the `hw-state` `attenuator`
  object if PL-2's accessor exists. Trace: `stixatt_fault`.
- qtests, one per A-3 row: inject, check truth against presented and the
  register at the times above, clear, check recovery. Plus:
  - `att-fault-reset`: a fault survives `system_reset`; the forcing masks
    do not.
  - `att-fault-midmove`: `sensor_bc.stuck_open` set at 250 ms and cleared
    at 700 ms. The move ends at 710 ms (target seen + debounce).
  - `att-fault-trip`: `current.offset` = 800 with hk-prot line 5 armed.
    The next ch 8 conversion trips (PROTSTAT and IRQ), and the set
    itself does not.
  - `att-fault-reject`: each of these is rejected:
    - a forcing bit outside 11:5;
    - an offset outside −4095..4095;
    - an unknown ID in `fault-clear`;
    - any fault set with `att-model=off`.
- Gates: **the full set**, including att-smoke's AX subset once A-4
  exists (until then, a manual AX `current.offset` probe run). The
  accepted-diff sections (b) and (c).

### A-4: probe extensions and `att-smoke.sh`
- **Probe changes** (`tmtc-probe.py`): land them **after D-3**, whose
  +122 uncommitted lines there (`--dump-apid`) must be in first. Each
  change is its own hunk, with self-test cases:
  - `--expect-return VALUE`: it attaches to the latest `--qmp-cmd` group
    (a qom-get) and fails unless the return equals the value;
  - a **forbid window**, `--forbid ...@SECONDS`: the forbid disarms
    after that many seconds from its group's send. Today a forbid stays
    armed for the rest of the run.
  - TM(1,7) is never used as evidence that a move ended (correction 3).
- **A live probe run first.** Every [L] payload below is confirmed by a
  probe run before it becomes an expectation, as in Phases 3 and 4.
- `scripts/stix/att-smoke.sh <qemu> [--negative] [--dry-run]
  [--check-guard]`, **built on the gateports slot library**
  (`scripts/stix/lib/slot.sh`):
  - `stix_slot_setup`, a per-slot run dir, a private sparse flash
    (`stix_flash_private`), the slot's TMTC port and a QMP socket;
  - no reliance on the 21000 flock;
  - it never reuses a shared flash;
  - if the gateports track has not landed yet, it uses the fault-smoke
    pattern and converts in GP's G-2b.
- **The flavour, decided positively:**
  - `nm $STIX_ASW_ELF` showing a symbol whose name **contains**
    `moveStartCoarseT` (it is suffixed, `.555127`) means AX;
  - otherwise att-smoke boots and reads `fsw-accesses` after the boot.
    A PFM ELF reads 0x5C at the boot SAFE entry (`OpModeMgmt.c:174`,
    PFM getter `Attenuator.c:214-217`) [L].
  - 0 accesses on a "non-AX" ELF fails with exit 2 (environment): a
    stripped or `HW_VERSION_FSW_IDPU` ELF must not pass as PFM.
- **AX subset** (the gate ELF; replaces the bare SKIP):
  - 0x14A3 within 30 s;
  - @35 `current.offset` = 800: `9434 06 0334 02EF` within 5 s, and no
    mode change (SAFE already);
  - @45 `fsw-accesses` = 0 (`--expect-return`);
  - @50 clear; hold to 60 s.
  - This proves the att → presented → comparator wiring live on the gate
    ELF.
  - The PFM sequence then **SKIPs with exit 77** and the record says
    "PASS (AX subset), SKIP (PFM sequence)".
  - Optional `--ax-blind`: the same TC moves as the PFM sequence, with
    only 0x942B allowed (the AX artefact). It is recorded, not gated.
- **PFM sequence**, in seconds from the connect [L]. It needs Q1.
  - 0x14A3 within 30 s;
  - @35 TC(236,1) → CONF (the nominal-smoke bytes): `0021 02 04`;
  - @40 TC(236,4) `05 04` (LV on);
  - @45 `truth.position` = "out", `presented.cmdsta` = 0x500,
    `fsw-accesses` > 0;
  - @46 **C**; @50 truth "in";
  - @52 **O**; @56 truth "out";
  - @58 `motor1.dead` = true; @60 **C**: `5436 04 01 00 01 00` within
    6 s; @66 truth "out"; @67 clear;
  - @70 **C**: `1423 0000 0002`; @75 truth "in";
  - @77 `sensor_ab.stuck_open` + `sensor_bc.stuck_closed`; @79 **O**:
    `5436 01 00 01 01 00`; @85 clear all;
  - @87 `current.offset` = 800: `9434 06 0334 02EF` within 5 s, then
    `0021 04 02`;
  - hold to 100 s.
  - Forbid TM(1,2)/TM(1,8) for the whole run. Forbid 0x942B for the
    whole run (the timeout-finishes default).
- **The stuck-sensor silence case is a second, short probe run** (a
  fresh boot, CONF, LV on, then):
  - `sensor_bc.stuck_closed` = true, then **C** at OUT;
  - forbid 0x5436 for the whole run;
  - truth "out" and `presented.sensor-bc` = true;
  - **SID2 status4 bits 14 and 13 both set** (`HKAcquire.c:378-379`),
    checked from `--dump-apid` of the HK APID by a small checker (the
    pattern of D-3's `ql-check.py`).
- `--negative`, for each of the three parts:
  - **PFM:** skip the `motor1.dead` set. Exactly the @60 0x5436 and the
    @70 0x1423 expectations must fail (and @66 truth "out").
  - **The silence run:** set `sensor_ab.stuck_open` together with
    `sensor_bc.stuck_closed`, and issue **O** at OUT. 0x5436 `01 00 01 01
    00` then fires, and the forbid must fail the run. This proves the
    forbid bites.
  - **AX subset:** skip the `current.offset` set; the 0x9434
    expectation must fail.
- Gates: the full set plus att-smoke ± (the AX subset on the gate ELF;
  the PFM parts only with a PFM ELF).

### A-5: docs
- **The accepted-diff section**, headed **"Accepted diff: attenuator
  model"** with no § number. It is numbered at the merge, as the platform
  track does (§31-38 detector, §39-40 ELUT, §41 gateports, platform
  "§39+"). It covers:
  - (a) the model and its [I]/[A] semantics;
  - (b) the ATT_C profile and the truth → fault → forcing layering;
  - (c) the reduced catalogue, its persistence (which differs from
    D-P4b), and the platform ID mapping;
  - (d) the gate record (PASS and SKIP listed separately);
  - the known limits: no thermal model, LV sampled at the command, the
    unknown polarity, the FPGA unknowns behind knobs, AX blindness
    (`fsw-accesses`), and the two TM(1,7).
- `ground-station.md` "Fault injection" gains an attenuator subsection:
  - the QOM paths and the qom-set recipes;
  - the expected events;
  - the trip latency (next conversion, D-Af);
  - the `-S` + qom-set + `cont` recipe for a fault present from boot.
- `baseline/README.md`: the suite recipe gains `build/tests/qtest/stix-att`.
- This plan's "As built" section.

### A-later (deferred; waits for Q1 and Q2)
- The faults `move.slow`, `mechanism.partial`, `motorN.flag_lost`,
  `controller.fail_flag` and `current.scale` (the A-later table), with a
  qtest per row and their outcome-table rows.
- The JSON `fault-catalogue` property, if the platform catalogue still
  needs a device-side description.
- Re-read the [I]/[A] knobs (stop-on-switch, timeout/finished, override,
  FAILFLAG, travel time, currents) against the HSWICD section. Then
  confirm the outcome table live on the PFM ELF.
- Optional: the REQ-018 step (7 TCs in one T1 block; the 8th moves) and,
  after detector D-4, the RCR/flare gate.

## Goldens and gate impact

- **The UART golden and the trace golden are unchanged** on the AX gate
  ELF (D-Ab). The golden has 0 accesses to 0x5C and 12 to 0x60, both
  before and after D-2's regeneration. A-1 confirms this by a boot-smoke
  run and a manual trace diff. If either differs, stop and investigate:
  it would mean the ELF is not the AX build the facts say it is.
- **The existing gates are unchanged:** boot-smoke, soak, tc-smoke,
  nominal-smoke, fault-smoke and detector-smoke see no 0x5C access on AX.
  ATT_C stays 20 when idle, as today.
- **The suite gains a binary** (`stix-att`). The old 5.2-wrapper run
  (README step 3) is not pointed at it: the 5.2 device has no attenuator
  model.
- **New: att-smoke.** Its AX subset runs on the gate ELF; its PFM
  sequence and silence run need a PFM ELF (they SKIP with exit 77
  otherwise). There is no PFM UART golden. att-smoke has its own
  expectations, and a PFM boot golden is out of scope (Q1).

## Merge order and overlap

- **Order:** platform PL-2 (PL-2a, the split) → this track's A-1. The
  branch is rebased onto the integration tip with D-2 (`93438dfa3b`) and
  D-3. The probe changes in A-4 come after D-3's probe work. att-smoke
  follows gateports G-2 if that has landed.
- Shared files and how to keep the overlap small:
  - `grlib_stixctrl.c`: about 25 hook lines in their own hunks, placed
    per D-Aa:
    - read: between 0x88 and det;
    - write: after hk-prot, before det;
    - reset: after `stix_det_reset`;
    - ATT_C: only in the two halves of the PL-2a split.
    Do not edit around the `stix_det_*` calls. `stix_det.c` keeps
    0x00-0x4C; this track takes 0x5C only.
  - `hw/sparc/trace-events`: the D tracks append to the stixctrl block
    (`:34-48`) and the stixdet block (`:49-56`). The `# stix_att.c`
    block goes where neither appends, before `# tmtc_io_server.c`
    (`:88`).
  - `hw/sparc/meson.build` (`:7`) and `tests/qtest/meson.build` (`:215`):
    one line each. Conflicts there are trivial.
  - `tests/qtest/stix.c`: untouched (D-Aj).
  - `scripts/stix/tmtc-probe.py`: A-4 only, after D-3, in separate
    hunks.
  - `accepted-diffs.md`: the non-numeric heading "Accepted diff:
    attenuator model", numbered at the merge.
- The ATT_C formula and D-2's DET_C quarter rules both live under
  `stixctrl_hk_value`, but in different cases.

## Risks and checks

- **AX blindness (the headline).** Without a PFM ELF, the work is
  correct but the FSW cannot show it.
  - Only `current.offset` reaches the FSW on AX, and `adc-inject[8]` can
    already do that.
  - The reduced scope (A-3 set, A-later) limits the work built on
    unconfirmed FPGA semantics.
  - `fsw-accesses` keeps the UI from reporting the AX artefact as a
    mismatch.
  - Everything gated on FSW move behaviour waits for Q1.
- **The platform interface.** The IDs, the `hw-state.attenuator` shape
  and the "presented only" wording (D-Ag note) are agreed with the
  platform track before A-3. A rename is one string per property.
- **A PFM ELF differs elsewhere:**
  - the HK SW limits are tight (`ParamMgmtDef.h:18-65`). Phase 4 found
    every model value inside the PFM windows. The persistence count is
    255 by a `ParamMgmt_init` code path (§27), so it is the same on PFM;
  - XrayDetFDIR no longer skips the AX bad detectors (`FDIR/XrayDetFDIR.c:627-635`);
  - the PFM DEBUG_PRINT configuration also has TRACE_TO_FLASH
    (`tasks.vs.json:74`), which writes flash.
  att-smoke therefore always runs on a private fresh flash and stays out
  of NOMINAL. The first PFM boot is a recon run, not a gate.
- **The FPGA semantics are inferred.** None of the following are in the
  notes, so each is a knob or one constant:
  - the stop on the switch, the debounce, and the timeout/finished
    relation;
  - override, FAILFLAG, and the OPERATIONSTATUS reset value of 1;
  - the travel time and the currents.
  Q2-Q5 ask for the HSWICD AttnCmdSta section. The faults that depend
  most on these are A-later.
- **The trace golden.** `stixatt_*` matches `-trace 'stix*'`, so the unit
  must not trace at realize, at reset, on 0x60 accesses, or in a getter
  (D-Ai). The A-1 manual diff checks this.
- **QOM names with dots** [L]: checked in A-1, with a fallback.
- **The lazy model and time.** No icount; the virtual clock follows the
  host. The FSW samples at 50 ms, so ±1 tick of jitter means expectations
  use windows (5-6 s), not exact sample counts.
- **TM(1,7) is not evidence** (correction 3). The gates use the truth,
  the EIDs and windowed forbids.
- **SAFE from a current trip** leaves CONF; the MAINTENANCE/CONF → SAFE
  path writes a context to flash (`OpModeMgmt.c:143-147`). Private
  flash only.
- **The flash rule.** No script touches `/tmp/stix-uprev-run/flash.bin`
  or `/tmp/stix-baseline-run/flash.bin`. att-smoke uses the slot
  library's private flash and the fault-smoke guard. The qtest binary
  uses 1 MB temp flash files (D-Aj).

## Doc vs code

Code wins in each case. The model follows the column marked ✔.

1. **REQ-017 wording** (TN via `notes-fdir-idpu.md:137`: "position switch
   does not change after a commanded move → ATT disabled + TM(5,3)").
   The code checks for the *opposite-end* reading, not "no change", and
   sends TM(5,2) 0x5436 (`FDIR.c:1342-1372`, `TMForm.c:590-600`). ✔ code.
   A single stuck switch is therefore silent (correction 2).
2. **The error codes.** `commonFSW.h:78-80` defines 0x1610/0x1620/0x1630,
   and `FDIR.h:466` says the check returns FSW_OK. The code returns
   0x10/0x20/0x30 (`FDIR.c:1279-1280`), and neither value reaches TM.
   ✔ code; the model does not depend on it.
3. **MOTORxUSED lifetime.** `Attenuator.h:63-75` says "the last
   Attenuator command used motor X" (latched). `AttCtrl.c:387-388` says
   it is "reset at the end", and `HKAcquire.c:343-345` ANDs it with
   "active". ✔ set during the move, cleared at the end [I] (Q4).
4. **The poll period.** `ATTENUATOR_DELAY` = 100 ms "between consecutive
   attenuator checks" (`commonFSW.h:155`) is unused. The loop is 50 ms
   (`AttCtrl.c:133-148`). ✔ 50 ms.
5. **The move window.** `AttCtrl.c:39` says "66 samples = 3.3s"; the AX
   stub moves for 3-4 s (BSW `Attenuator.c:36`); the PWM quants imply
   about 1.6 s [I]. ✔ the ramp of 1.6 s; the AX mismatch is noted (AX
   artefact). With the NO_MOVE pre-sample, the FSW actually gives up
   about 3.2 s after the write.
6. **REQ-018** (N1 6 / T1 30 s, N2 10 / T2 300 s, ATT disabled +
   TM(5,2)) matches the code (`ParamMgmtDef.h:160-163`, `FDIR.c:1452-1469`)
   and EID 0x5427. ✔ both.
7. **REQ-001's 3 s wait** matches `WAIT_ATT_OP_END` (`commonFSW.h:156`,
   `OpModeMgmt.c:173-177`). ✔ both.
8. **0x64.** StixIdpu.h has `notUsed0`; the TSIM model has ATTNCONFIG2
   (`attenuatorTsim.c:57-81`). Neither the FSW nor the model uses it.
   ✔ plain storage.
9. **The mode for attenuator TCs.** The FSW_IT script enters MAINTENANCE
   "to be able to test both motors at once". The code accepts
   TC(236,4/5) in MAINTENANCE and CONFIGURATION alike
   (`TCVerifDynamic.c:64-65`). ✔ code: att-smoke uses CONF (proven by
   nominal-smoke).
10. **The TC(236,4/5) report.** The comment at `TcService236.c:95` says
    "HWConfig_powerOn sends TM(1,7) or TM(1,8) itself --> use return".
    The code `break`s and reports again (`:213`). For the attenuator,
    HWConfig sends nothing (`HWConfig.c:853, 946`), and AttCtrl reports
    at FINISH. ✔ code: two TM(1,7) per TC move (correction 3).

## Open questions for the user

1. **Q1: a PFM ELF.** Can you build `ASW_em` with `-DHW_VERSION_PFM_IDPU
   -DDEBUG_PRINT`, without TRACE_TO_FLASH if you can (the `tasks.vs.json`
   entries at :74/:98 are the nearest)? Without it the attenuator faults
   cannot reach the FSW, and only the `attenuator.current.*` faults work
   on the gate ELF (the att-smoke AX subset). The other option, poking the AX stub's `isAB`/`isBC` in
   guest RAM, emulates the FSW's own stub, not hardware, and is
   rejected.
2. **Q2: the HSWICD AttnCmdSta/AttnConfig1 section.** Can you extract it
   into the notes? It would settle:
   - the OPERATIONSTATUS reset value;
   - stop-on-switch and the override;
   - whether a timeout also sets OPERATIONSTATUS;
   - FAILFLAG's cause;
   - the ramp quant unit;
   - whether a command toward an already-actuated switch drives at all.
3. **Q3: the real travel time and the motor currents** (default 500 ms;
   100 raw moving, 300 stalled per motor). Any EM/PFM test log number
   would do.
4. **Q4: the timeout semantics.** Should the default stay "TIMEOUT +
   finished", where the FSW runs the correlation check and timeouts are
   mostly silent, or should it be "TIMEOUT only", where the FSW sends
   0x942B? Both are one knob (`att-timeout-finishes`).
5. **Q5: the switch naming.** The brief's `stuck_open` is read here as
   "the bit reads 0". Is that the intended meaning, or should it follow
   the contact polarity once the ICD says what it is?
6. **Q6: faults across reset.** Physical faults survive an IDPU reset
   here, unlike Phase 4's injections. Is that what you want for the
   platform tests?

Q7-Q10 (scope, ID ownership, AX blindness and the SKIP convention) came
up in the review. They are decided as overridable defaults (see the
status block and "Review resolutions"). Q1-Q6 stay open.

## Out of scope

- ATT_T thermal behaviour; LV switched off during a move; a power cut on
  an ATT_C trip (the Phase 4 no-cut deviation stands).
- The RCR/flare-driven gate (needs detector D-4); a PFM boot golden.
- The deferred faults and the JSON catalogue (A-later), until Q1/Q2.
- The platform-test controller itself (the other track), and its JSONL
  and UI.

## Review resolutions

An independent validation (2026-11-11) produced 8 required (R) and 14
recommended (r) items. Each item was checked against the FSW, the
integration tip `93438dfa3b`, the sibling plans and the gate ELF. The
coordinator accepted all of them and decided Q7-Q10. The validated draft
is kept outside the repo. Each row below says where the item is folded.

| Item | Resolution | Where |
|---|---|---|
| R1 two TM(1,7) per TC move; the first is immediate (`TcService236.c:213`, `HWConfig.c:853/946`) | folded | Correction 3; the outcome-table setup; REQ-018; A-4 (TM(1,7) never used as evidence); Doc vs code 10; Risks |
| R2 probe forbids have no end; the single-stuck-sensor forbid is weak | folded | Correction 2 (SID2 bits 13/14); A-4: a forbid window in the probe, the silence case in its own run with a SID2 check, a negative that proves the forbid bites |
| R3 stix-att must reuse the temp-flash helper (16.5 GB `flash.bin`) | folded | D-Aj; A-1; Risks (flash rule) |
| R4 align with the platform plan (IDs, `hw-state`, the `hk_value` split, "presented only") | folded (Q8) | Status block; D-Aa; D-Af layering; D-Ag note to the platform track; D-Ah `hw-state`; catalogue "ID source" and renames; Merge order (after PL-2) |
| R5 AX truth-vs-telemetry is an artefact | folded (Q9) | Correction 1; D-Ah `fsw-accesses`; A-1 `att-reset`; Risks |
| R6 positive flavour detection; a SKIP is not a PASS | folded (Q10) | A-4 flavour rules (substring `nm`, runtime `fsw-accesses`, exit 2 for a non-AX ELF with 0 accesses); SKIP = exit 77; gate records list SKIP separately (Commits preamble, A-5 (d)) |
| R7 scope: nothing new is FSW-visible on AX; att-smoke must not be vacuous | folded (Q7) | Status block; the catalogue split (A-3 set / A-later); A-3; A-later; the A-4 AX subset (`current.offset` = 800 → 0x9434); Risks |
| R8 hook placement (write path after hk-prot) | folded | D-Aa hook placement; Merge order |
| r1 debounce cites the dead `RateCtrl_reset(TRUE)` | folded | Key facts (Other readers); D-Ae debounce |
| r2 NO_MOVE pre-sample; the give-up is about 3.2 s | folded | Key facts (move loop); Timing; Doc vs code 5 |
| r3 ramp constants: exactly 0.1 s + 1.5 s | folded | Timing |
| r4 scale arithmetic 880 | folded | The outcome table (`current.scale`, A-later) |
| r5 REQ-018 fixed blocks; the 8th TC moves | folded | Key facts (FDIR); the REQ-018 paragraph; A-later |
| r6 `att-model=off`/read-only rejections and negative qtests; the trip qtest | folded | D-Ag rejected sets; A-1 `att-reject-*`; A-3 `att-fault-trip`, `att-fault-reject` |
| r7 `att-reentry` → determinism | folded | D-Ai; A-1 `att-determinism` |
| r8 getters must not trace | folded | D-Ai; Risks (trace golden) |
| r9 trip latency differs from `adc-inject` | folded | D-Af; A-5 `ground-station.md` |
| r10 NOMINAL entry re-applies the ATT protection | folded | Key facts (FDIR/HK); [L] for a future NOMINAL gate |
| r11 merge mechanics (trace-events spot, probe after D-3, slot library, heading) | folded | Merge order; A-4; A-5 ("Accepted diff: attenuator model", no §) |
| r12 path fixes | folded | Sources; Key facts (Other readers); D-Aa line numbers at `93438dfa3b` |
| r13 gate cost per commit | folded | Commits preamble (the per-commit set; the full set at A-3 and at the merge) |
| r14 A-2 stall wording | folded | A-2 qtests |
| Q7 scope | decided: reduced | A-1, A-2 now; A-3 = A-3 set; A-later |
| Q8 ID and surface ownership | decided: the platform owns the IDs and the catalogue; this track owns the device QOM | Status block; Fault catalogue (renames: `att_c.offset`→`current.offset`, `att_c.scale`→`current.scale`, `motorN.slow`→`move.slow`; new: `mechanism.jammed`, `current.offset`, and A-later `mechanism.partial`, `motorN.flag_lost`, `controller.fail_flag`, `current.scale`) |
| Q9 AX blindness in the UI | decided: `fsw-accesses`; the UI shows "FSW does not read the attenuator on this ELF" | Correction 1; D-Ah |
| Q10 SKIP convention | decided: exit 77; SKIP listed separately from PASS | A-4; Commits preamble; A-5 |

Q1-Q6 remain open for the user (see "Open questions for the user").
