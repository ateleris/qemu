# Phase 4 detailed plan: error injection and POWERSTAT

Status: done, 2026-11-11 (P4-1..P4-6; see "As built: Phase 4
acceptance"). Validated before the work started. Base: uprev `73558c3571`, track worktree
`~/dev/qemu-stix-tc` (branch `stix-uprev-tc`). The user chose "error
injection" (with POWERSTAT folded in) as the Phase 4 scope.

Sources: two read-only recon passes over `~/dev/STIX-FSW/ASW` (paths below
are relative to `ASW/ASW/src` unless they say BSW), plus a dump of
`ParamMgmt_parameters` from the gate ELF. A tag of **V** means the fact was
read in code or the binary; **I** means it was inferred and needs a live check.

## Goal

The FSW's fault detection can be exercised from outside at run time:
1. HK ADC readings can be overridden per channel while the guest runs.
2. The FPGA's HK hardware protection (over-threshold comparator and IRQ) is
   modelled for the case the FSW arms in SAFE (SPW_C).
3. The SpaceWire link can be forced down and back up, and time codes can be
   stopped (this already works).
4. POWERSTAT (hkCmdSta bits 9:5) reports power-good for the commanded
   supplies, so the FSW's POWERSTAT-gated checks are live.

One gate script proves the FSW reacts as its code says, with the TM events
named below.

## Key facts the plan rests on

- **The gate ELF's HK limits are 0..4095 (V).** `ParamMgmt_parameters`
  (0x40080908 in `ASW_em_baseline.elf`) entries 0..45 are all
  `{0, 0, 0, 0xFFF}`. It is an AX build (ParamMgmtDef.h:67-113). The ADC is
  12-bit, so **an injected value can never trip a software limit on its own.
  Every software-limit test first tightens a limit by TC(236,10).**
- **TC(236,10)** (V, FDIR.c:1019-1100, TCData.h:151-153) has application data
  `fdirMask u32, paramType u8, value u32`. paramType is the enum position in
  FDIR.h (for example `FDIR_HK_VOLTAGE_LINE_1V5_LOW_LIMIT` = 1 under mask
  0x00080000, and `FDIR_HK_TEMP_LINE_PSU_UPP_LIMIT` = 2 under mask
  0x00100000). It is allowed in SAFE (TCVerifDynamic.c ~:71). TC(237,18) is
  not allowed in SAFE.
- **When the line checks go live (V).** The FDIR timer runs every 1 s. Until
  `auto_fdir_protection_prevented` is TRUE only `regularSpwCheck` runs
  (HKMonitor.c:347-351). `FDIR_timerEnableFdir` sets it 15 s after the boot
  report and sends TM(5,1) **0x14A3** (FDIR.c:500-562). `FDIR_config`
  (TC(236,10)) also sets the flag (FDIR.c:1023), so a TC(236,10) sent
  **before** 0x14A3 cancels the automatic enable of the HW protections.
  **Send TC(236,10) only after 0x14A3.**
- **The software-limit reaction (V).** A TM goes out when the consecutive
  out-of-limit count equals the threshold: 3 for the currents, voltages and
  PSU/PCB/FPGA/ATT temperatures, 16 for DET_Qx_T, ASP and TIM
  (ParamMgmtDef.h:116-153, HKMonitor.c:467).
  - The FSW sends TM(5,3) **0x9449** `[id u8, value u16, limit u16]` and
    signals a move to SAFE. If the mode is SAFE at the end of the cycle it
    also sends TM(5,4) **0xD420** `[u32 function mask]`.
  - In NOMINAL it also sends TM(5,1) `0021 04 02 02`.
  - There is no recovery event: a value back in limits only zeroes the
    counter (HKMonitor.c:452-457).
- **POWERSTAT (V).** `PSU_getPowerStatusMask` = hkCmdSta >> 5 & 0x1F
  (PSU.c:12-16): bit 5 LV, 6 HV01, 7 HV17, 8 SPW0, 9 SPW1 (StixIdpu.h:988-994).
  It is a PSU power-good, not the commanded state (PSU.h:36-43).
  - The FSW uses it to skip the SPW0/1_V, ATT_V and HV checks
    (HKMonitor.c:357-370), in `HKMonitor_spwCheck` during SpW init
    (SpwControl.c:472-491), in `regularSpwCheck`, and in the SID2
    status4 bits 17-19, 28 and 29 (HKAcquire.c:314-370).
  - PSU.c writes hkCmdSta read-modify-write (PSU.c:20-90).
  - With POWERSTAT = the commanded enables, every current model value passes
    the newly live checks under both AX and PFM limits (recon §3).
- **HW protection (V/I).**
  - Registers (StixIdpu.h:1003-1036, 1650-1657; HK.c:205-300):
    - hkIrqSta 0x90: IRQEN 8:0, PROTSTAT 17:9, HOTSAMPLE 31:20.
    - hkProtConfig[9] at 0x94..0xB4: threshold 11:0, PROTEN bit 31. The
      index order is 1V5, 2V5, 3V3, SPW, DET, ATT_C, ATT_V, HV17, HV01; the
      IRQ bit values are 256..1 in that order.
    - unlockingReg 0xD0.
  - `FDIR_init` arms only SPW_C: threshold 3826, PROTEN 1 (FDIR.c:308-324).
    `FDIR_timerEnableFdir` arms all nine IRQs. The other thresholds are
    written only on mode transitions or by TC, so in SAFE they keep their
    reset value (see D-P4d for the ICD values).
  - The ISR `HK_IrqCallback` (FDIR.c:829-947) masks the line's IRQ, sends
    TM(5,3) **0x9434** `[id u8, HOTSAMPLE u16, threshold u16]`, clears
    PROTSTAT and signals SAFE.
  - The real FPGA also cuts the supply (V: TN REQ-043, DD §5.6). **The model
    does not cut power**, or the SpW link dies before the event is seen
    (deviation, D-P4d).
- **SpaceWire (V).** The model has one link with `status` fixed at RUN, and no
  error bits are ever set.
  - The time-code timeout is 60 000 ms (ParamMgmtDef.h:206). On expiry the
    FSW sends TM(5,2) **0x542B** and sets coarse-time bit 31. The next code
    within the 4 s drift limit resyncs and sends TM(5,1) **0x1429**
    (Timing.c:63-91, 275-286).
  - `FDIR_SpWLinkFailure` counts only link state READY (2), because it
    compares against the wrong enum (FDIR.c:711; FSW bug, recorded as is).
    After 10 s (`SPW_LINK_PROBLEM_TRANSIENT_DURATION_MS`) it sends TM(5,2)
    **0x5435**, param `(ext&3)<<6 | (LS&0xF)<<2 | link`. On restore it sends
    TM(5,1) **0x142D**.
  - TMBuffer does not send while LS != RUN, retries every 1 s, and after 10 s
    sends 0x5435 and starts TM queue backups to flash (TMBuffer.c:662-665,
    925-947). Events raised during the outage reach the ground only after
    recovery.
- **The boot self-test** checks only ch24, ch28 and ch44 against hard-coded
  windows (HK.c:34-39). A bad value gives 0x8002 + 0xD420, and boot continues.

## Decisions (D-P4)

Revised after the validation and the document review (sections at the end).

- **D-P4a: injection interface.** Use QOM properties set over QMP.
  - stixctrl gets class properties `adc-inject[0..127]` through
    `object_class_property_add(klass, name, "uint32", get, set, NULL,
    GUINT_TO_POINTER(ch))`. The setter traces, stores, and runs the
    protection comparator.
  - `0xFFFFFFFF` means "no override" and is the default. Any other value
    outside 0..0xFFF is rejected with an error.
  - Both devices get stable QOM paths, `/machine/stixctrl` and
    `/machine/stixspw` (object_property_add_child in leon3.c before
    realize). The qtests `ctrl-irq-lines` and `stix_spw_path` move to these
    paths.
  - Gates reach them with `-qmp unix:<run dir>/qmp.sock,server=on,wait=off`
    (the path must stay under 108 bytes).
- **D-P4b: reset.** Injections are cleared on device reset.
- **D-P4c: POWERSTAT = (commanded enables & ~blockade) << 5, computed on
  read.** The commanded enables are hkCmdSta bits 4:0 (LV, HV01, HV17,
  SPW0, SPW1). blockade is always 0 in Phase 4; it is the hook for a later
  power-cut option.
  - Guest writes to bits 9:5 are ignored. There is no power-up delay.
  - The property `powerstat` (bool, default on) allows A/B comparison.
  - SID2 status4 bits 17/18/19/28/29 change, so the goldens are regenerated
    once.
- **D-P4d: HW protection.** Model all nine comparators.
  - Threshold reset values come from ICD Table 4-75: 1V5 2282, 2V5 3416,
    3V3 3826, SPW 3826, DET 546, ATT_C 751, ATT_V 4095, HV17 2323,
    HV01 2323. They are document values only: the FSW never writes the
    eight non-SPW thresholds, so the code cannot confirm them. The
    released documents may miss late spec changes; if the live behaviour
    looks off, revisit these first.
  - The comparator is strict ">" on the line's channel (0, 1, 2, 3, 4, 8, 12,
    16, 20). It trips on the second consecutive high sample, evaluated at each
    conversion of that channel and when an injection is set (two evaluations).
    It sets PROTSTAT only when ProtEn = 1.
  - HOTSAMPLE is one shared read-only field. PROTSTAT is write-1-to-clear,
    and IRQEN is read/write. irq_hk pulses only when a PROTSTAT bit newly
    sets and its IRQEN bit is set.
  - The unlock register: writing 0xA5 unlocks one write to a protected
    register (a threshold, or a ProtEn clear). Setting ProtEn needs no unlock.
  - Deviation: **no power cut.** The real FPGA cuts the supply (TN REQ-043,
    DD §5.6). The model leaves it on, so the event is observable.
- **D-P4e: SpW link fault.** stixspw gets a runtime bool `link-down`.
  - While it is true: LS = READY (2), even if SPW_open writes LS; DE is set;
    the error IRQ is raised (when IE|LI, as SPW_open sets them) on entry and
    then every 1 s; time codes stop; RX units stay queued.
  - On false: LS = RUN; time codes and RX resume.
  - The error IRQ is mandatory: it is the path that produces the events.

## Commits

### P4-1: POWERSTAT
- Compose bits 9:5 on read (D-P4c) and add the `powerstat` property.
- qtest: each enable maps to its bit; a written 1 in 9:5 does not stick;
  `powerstat=off` keeps plain storage.
- Regenerate the goldens and add accepted-diffs §25 (SID2 status4 bits).
- Gates: the full set, with nominal-smoke in both modes. HKMonitor_spwCheck
  now runs for real during SpW init.

### P4-2: ADC injection and stable QOM paths
- The `adc-inject[N]` accessors (D-P4a); the injection overrides the table
  and the state overrides; clear on reset; trace on set and clear.
- The `/machine/stixctrl` and `/machine/stixspw` children; the two qtests
  updated.
- qtest via qom-set:
  - an injected channel reads back, and the other channels are unchanged;
  - clearing restores the nominal value;
  - a reset clears it;
  - an out-of-range value is rejected.

### P4-3: fault gate (SW limit and time-code loss)
- The probe gains `--qmp PATH` and a QMP send-group kind. It does the
  capabilities handshake, skips async events, and fails unless the reply is
  `return`.
- New `scripts/stix/fault-smoke.sh`, built like nominal-smoke.sh: run dir
  `STIX_FAULT_RUN_DIR`, the same guard, fresh flash, `--hold`, `--dry-run`,
  `--check-guard`. Sequence, with times from connect (0x14A3 comes at about
  +13 s):
  - connect: 0x14A3 within 50 s;
  - @25 TC(236,10) `00080000 01 0000079F` (1V5_V low = 1951): TM(1,7);
  - @30 inject `adc-inject[24]` = 1800: `9449 07 0708 079F` and
    `D420 00080000` within 10 s;
  - @45 clear it;
  - @50 TC(236,10) `00100000 02 00000ED8` (PSU_T upper = 3800): TM(1,7);
  - @55 inject `adc-inject[40]` = 3900: `9449 17 0F3C 0ED8` and
    `D420 00100000` within 10 s;
  - @70 clear it; forbid 0x9449 from @75 to the end;
  - @75 `timecodes` = false: 0x542B within 75 s;
  - @155 `timecodes` = true: 0x1429 within 10 s;
  - hold to 175 s.
  - Do not forbid 0x142E: the boot-armed TC20 timer fires at about +29 s, a
    no-op in SAFE.
- accepted-diffs §27: the gate record.
- **As built (live finding).** On this build every HK persistence count is
  255, not 3. ParamMgmt_init (ParamMgmt.c:34-36) passes value, then min,
  then max to HKMonitor_updateParameter, and the count branch
  (HKMonitor.c:960-964) ignores the type, so max (255) wins.
  - The gate therefore sends TC(236,10) `<mask> 22 00000003`
    (*_CONSECUTIVE_MEASUREMENTS = 3) before each limit TC.
  - 0x14A3 is required by @25, because a TC(236,10) before it cancels the
    auto-enable.
  - `--negative` injects 2048 and must miss exactly the 9449/D420
    expectations.

### P4-4: HK HW protection
- The comparator, the registers, the unlock register and irq_hk (D-P4d).
- qtest:
  - the reset thresholds;
  - ">" versus "=";
  - two-sample persistence;
  - ProtEn, IRQEN and PROTSTAT write-1-to-clear;
  - HOTSAMPLE;
  - the unlock rules;
  - an IRQ only on a new bit.
- fault-smoke gains these steps, with no TC needed (all nine lines are armed
  after 0x14A3):
  - inject `adc-inject[4]` (DET_C) = 600: `9434 05 0258 0222`. The DET_C id
    is 5 (HKData.h:160).
  - Clear it. The IRQ stays masked after the trip, so the step runs once.
  - Optionally inject 2V5_C = 3500: reported as id 1, the FSW bug at
    FDIR.c:869.
  - Each trip signals SAFE, which is harmless in SAFE.

### P4-5: SpW link-down
- `link-down` (D-P4e).
- qtest: LS and DE; the IRQ only with IE|LI; RX held while down and delivered
  after; time codes stop.
- fault-smoke step: link-down for 20 s. After it is cleared, expect
  `5435 48`, allow a second 0x5435 (count and order not pinned), and expect
  `142D 00`.
- Record FDIR.c:711 as observed behaviour. The TN (REQ-029/038) intends any
  link error to count; the code and the gate ELF count only READY. The model
  follows the code. Whether this is a bug or a late spec change is open.

### P4-6: docs
- ground-station.md "Fault injection": the QMP socket, the qom-set commands,
  the ZIX36010 limit recipe (FDIR mask, FDIR parameter ID, Parameter value),
  and the expected events.
- accepted-diffs: the known limits, including the no-power-cut deviation and
  the FSW bugs. The Phase 4 checklist and the GS-repo doc line.

## Gates per commit

The Phase 3 set plus fault-smoke from P4-3 on:
- suite;
- boot-smoke against the golden;
- soak 300 s;
- tc-smoke;
- the probe self-test;
- nominal-smoke in both modes;
- fault-smoke.

## As built: Phase 4 acceptance

**Commits** (branch `stix-uprev-tc`):
- [x] P4-1 POWERSTAT: `1c6b92acd2` (accepted-diffs §25).
- [x] P4-2 ADC injection and stable QOM paths: `6734968e7b` (§26).
- [x] P4-3 fault-smoke gate and probe `--qmp`: `4700254fe4` (§27).
- [x] P4-4 HK HW protection: `27091e745f` (§28).
- [x] P4-5 SpW link-down: `ca01170003` (§29).
- [x] P4-6 docs: this commit. ground-station.md "Fault injection",
  accepted-diffs §30 (known limits after Phase 4), this checklist, the
  GS-repo line in `docs/stix-qemu.md`, and `STIX_GS_QMP` in
  `run-qemu-gs.sh`.

**Gates** (the full set on the P4-5 tree, normal ELF, fresh flash per
gate run, under the 21000 flock). Each passes:
- [x] suite: 38 ok (35 after P4-1, 36 after P4-2, 37 after P4-4);
- [x] checkpatch: no style problems;
- [x] probe self-test;
- [x] boot-smoke: the normalized UART matches the golden (the golden
  changed once, at P4-1);
- [x] soak 300 s;
- [x] tc-smoke;
- [x] nominal-smoke, positive and `--negative`;
- [x] fault-smoke, positive and `--negative` (the negative fails with
  exactly the two @40 expectations);
- [x] the shared `/tmp/stix-uprev-run/flash.bin` mtime is unchanged
  (2026-11-10 13:05:54).

P4-6 changes only docs and `run-qemu-gs.sh`. The script change was
checked with `sh -n`/`dash -n` and a stub binary, not with QEMU.

**Deviations from the plan, as built:**
- **The persistence count is 255, not 3** (FSW finding, §27). The gate
  sends TC(236,10) `<mask> 22 00000003` before each limit TC. The
  validator's "persistence of 3" was wrong.
- **The schedule.** The first TC goes at +35 s, not @25/@20: 0x14A3
  must come within 30 s. The rows moved by 15 s for DET_C at +85 (P4-4)
  and gained link-down at +210/+230 (P4-5). The run is 250 s, not
  175 s. 0x9449 is forbidden from +100, not @75.
- **The second 0x5435 has param 48, not 08** (§29). The ISR's next call
  sets the external flag again before TMBuffer reports. Both 0x5435s
  and 0x142D come out in one burst after the clear, so the 0x5435
  window is 35 s.
- **More forbids.** The connect group forbids 0x9434 for every line id
  but DET_C's, and 0x9434 is forbidden from +95.
- **The optional 2V5_C step is not in the gate.** The id-1 bug is
  recorded only (§28, §30).
- **The unlock token** is spent by any write to the CTRL register
  bank, not only a protected one ("one write cycle", §28).
- **Added properties:** `hk-prot` (default on; off keeps plain storage,
  §28). `link-down` is cleared at reset like the injections (§29).
- **TX during link-down waits** (TE stays set) instead of aborting.
  dmaControl.LE is ignored (§29).
- **The GS QMP option** (`STIX_GS_QMP`) was not in the plan. The P4-6
  docs needed it.

## Risks and checks

- **The gate ELF vs the source.** The ELF was built from a source state that
  may differ from `~/dev/STIX-FSW`. Every **I** item, and every EID or
  payload, is confirmed by a live probe before it becomes a gate expectation
  (as in Phase 3).
- **POWERSTAT makes SpW init run real checks.** If anything fails, SpW init
  can drop the link and TM stops. P4-1 runs boot-smoke first.
- **Each trip signals SAFE.** That is harmless in SAFE, but a gate run in
  NOMINAL would leave it. fault-smoke stays in SAFE.
- **The flash context.** TMBuffer flash backups during link-down write flash,
  so fault-smoke always uses its own fresh run dir.
- **The rtems_task_wake_after call in the AHB-error ISR** (SpwControl.c:459)
  makes RA/TA injection risky (I). It is out of scope.

## Out of scope (Phase 5 candidates)

- An IDeF-X register file, FSM/QL and ROTB science data.
- TX descriptor LE injection and AHB error injection.
- A TC(20,128) generator; the generic GS fixes.

## History: validation (2026-11-11), folded into the decisions above

An independent validator checked the plan against the source and the gate ELF.
- Confirmed: every TC(236,10) and 0x9449/0xD420 fact, the ids (1V5_V = 7,
  SPW_C = 4, PSU_T = 0x17), the persistence of 3, and the ELF values (limits
  0..0xFFF, FDIR_ENABLED_FUNCTIONS 0x3114C, SPW threshold 0xEF2,
  timeouts 60000 and 10000, and the FDIR.c:711 enum bug present in the ELF).
- POWERSTAT passes SpW init and all newly live checks.

Required changes:
1. **P4-2.** `object_class_property_add_uint32_ptr` has no setter hook. Use
   `object_class_property_add(klass, "adc-inject[N]", "uint32", get, set,
   NULL, GUINT_TO_POINTER(N))` with visit_type_uint32. The setter traces and
   calls the comparator. Reject values outside 0..0xFFF other than
   0xFFFFFFFF; do not mask them.
   Moving the devices to `/machine/stixctrl` and `/machine/stixspw` breaks
   two qtests, `ctrl-irq-lines` (tests/qtest/stix.c:1098-1118) and
   `stix_spw_path` (:2003-2025), which look under /machine/unattached.
   Update them.
2. **P4-4.** hkIrqSta:
   - PROTSTAT is write-1-to-clear (HK.c:272-277 writes tmp|mask, then tmp);
     IRQEN is read/write; HOTSAMPLE is read-only.
   - Pulse irq_hk only when a PROTSTAT bit newly sets while its IRQEN is set.
   - Set the 0xFFF threshold reset explicitly in grlib_stixctrl_reset (reset
     does not clear `reg`).
   - The comparator is a strict `>`.
   - After a trip the SPW IRQ stays masked, so a second injection gives no
     0x9434. The channels are 0, 1, 2, 3, 4, 8, 12, 16, 20.
   - FSW bug to note: a 2V5_C trip is reported with id 1 (FDIR.c:869).
3. **P4-5.**
   - The error IRQ (DE, with IE|LI as SPW_open sets them) is mandatory. It is
     the PRIMARY path. While the link is down, TMBuffer does not report on its
     own, and heartbeats are suppressed (HKReport.c:145-147).
   - Expect `5435 48` (ISR). Allow a second 0x5435 (likely 08). Expect
     `142D 00` after recovery. Do not pin the count or order of the 0x5435s.
   - The model keeps LS = READY even if SPW_open writes LS.
4. **Key facts corrections.**
   - The SpW link FDIR precondition uses the commanded SPWEN
     (PSU_getSpWState), not POWERSTAT.
   - The TC20 timer is armed at boot (0x142E at about +29 s, a no-op in
     SAFE). Do not forbid 0x142E.
   - The non-SPW protection thresholds are written only by TC.
5. **P4-1.** The SID2 diff is status4 bits 17/18/19/28/29 only. The
   implementation is POWERSTAT = (hkCmdSta & 0x1F) << 5.
6. **Nice to have.**
   - TC(236,10) at about @20, since 0x14A3 comes at about +13 s.
   - The QMP helper skips async events.
   - Add the PSU_T case (`00100000 02 00000ED8`, inject 3900 →
     `9449 17 0F3C 0ED8` + `D420 00100000`).
   - The QMP socket path must stay under 108 bytes.

## Document review (2026-11-11): STIX-TN-0024 FDIR, STIX-ICD-0026 HW/SW ICD, STIX-DD-0003

Notes: $S/docs/notes-fdir-idpu.md. These supersede the validator's
"0xFFF threshold" item.
1. **HW-protection threshold reset values** (ICD Table 4-75, TN Tables 3-1/3-2):
   - the values are 1V5 2282, 2V5 3416, 3V3 3826, SPW 3826, DET 546, ATT_C 751,
     ATT_V 4095, HV17 2323, HV01 2323;
   - all nominal model values are below them;
   - all nine lines are armed after 0x14A3, so non-SPW gate cases need no TC:
     inject DET_C 600 to get 0x9434 [DET_C id, 600, 546]; inject 2V5_C 3500
     to show the id-1 bug.
2. **The FPGA does cut power** (TN REQ-043, DD §5.6, ICD HkCmdSta). It
   switches off the non-essential and/or SpW supplies, latched until the SW
   clears LvEn/SpwEn. Essential-line overcurrent only raises the IRQ.
   - Keep "no cut" as a documented deviation.
   - Compose POWERSTAT = (en & ~blockade) << 5, with blockade 0, so a later
     cut option is a one-line change.
3. **Comparator.**
   - It needs two consecutive samples 850 µs apart, and the test is strict
     "above".
   - HOTSAMPLE is one shared field.
   - Gating PROTSTAT on ProtEn = 1 is invisible to the FSW.
4. **Unlock register.** Writing 0xA5 unlocks the protected registers for one
   write. Enabling ProtEn needs no unlock. Enforce this; it is cheap and
   compatible with the FSW (HK.c:213-222).
5. **The TN confirms** the limit-trip, SAFE, 0xD420 order and persistence;
   that the 15 s auto-enable and its cancel are intended; and that FDIR.c:711
   is a bug (REQ-029: any link error counts; REQ-038: no duplicate reports).
   It says nothing about time-code loss.
6. **Extra cases** (Phase 4 or later):
   - Other IDPU on: StixStatus bit 6 plus irq_other gives 0x944A and SAFE.
   - EDAC SE rate: 0x542F.
   - EDAC DE: HK counters only.
   - SpW port failover: needs PO/AP/NP.
