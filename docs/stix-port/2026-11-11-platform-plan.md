# Platform controller plan: a spacecraft-side test bench for the STIX emulator

Status: draft, 2026-11-11, validated 2026-11-11 (the review is folded
into the body; see "Review resolutions"). Base: uprev `d2c6cae28a`, track
worktree `~/dev/qemu-stix-platform` (branch `stix-uprev-platform`). This
work runs in parallel with the detector phase
(`2026-11-11-detector-plan.md`) on its own branch.

**Sources:**
- a read-only pass over `~/dev/STIX-FSW/ASW`. Paths are relative to
  `ASW/ASW/src` unless they say BSW, which is `ASW/BSW_ASW/src/BSW`;
- the TM/TC ICD export STIX-ICD-0812 I4R8 draft. "ICD:line" is the line
  in the text export. The ICD is confidential, so only facts are given
  here;
- this repo;
- the ground station's IDB-derived TM definitions
  (`ATS-GroundStation-stix/definitions/telemetry/stix_idb_tlm.json`,
  read-only);
- the simulation-environment survey `~/dev/stix-doc-notes/sim-env-survey.md`
  (cited as "survey §n").

**Tags:**
- **[C]** read in the FSW code;
- **[D]** read in the ICD;
- **[M]** read in this repo (model code, scripts, accepted-diffs);
- **[G]** read in the GS IDB JSON;
- **[I]** inferred; it needs a live check before it becomes a gate
  expectation.

When the code and the ICD disagree, the code wins and the difference is
flagged.

## Goal

The user's words: boot the container and pass it a small config. Then
either:
- **run** one or more test scenarios and exit with pass or fail plus
  reports; or
- get everything **ready without booting**, then drive the spacecraft
  side live from a small web UI: "boot STIX", "start SpW", "stop SpW",
  "send time code", "stop time codes", and so on.

The point is to see how the real FSW behaves when SpW is down, comes up
or goes down again, when time codes or the keep-alive stop, and so on,
without reconfiguring QEMU or restarting the container.

**Deliverables:**
1. **Small, opt-in or read-only QEMU additions:**
   - a platform on-board time (OBT) as the time-code source;
   - a one-shot time code and a time-code value injection;
   - a virtual-clock read-out;
   - read-only hardware-state snapshots.
2. **The controller:** one Python process, standard library only. It
   owns QEMU, QMP, the platform TCs (TC(9,129), TC(20,128)), the TM relay,
   the state model, the fault catalogue and the scenarios. Its interface
   is a versioned REST + SSE API, plus a JSONL log. The web page is one
   client of that API.
3. **A Dockerfile** under `scripts/stix/platform/` that builds this QEMU
   and runs the controller as the container's main process.

## Key facts the plan rests on

### TC(20,128): the keep-alive

**The timeout [C/D].** It is 30 s, hard-coded, and not a parameter.
- `FDIR_startTC20countdown` fires a one-shot RTEMS timer after
  `RTEMS_MILLISECONDS_TO_TICKS(30000)` (FDIR.c:570-580).
- The ICD agrees: EID 0x142E means "Service 20 not received for 30 s"
  (ICD:8740-8745) [D].

**What re-arms it [C]:**
- SAFE entry at boot (OpModeMgmt.c:968-969, just before the boot report).
  This gives the boot-armed 0x142E at about +29 s in the gates;
- each TC(20,128) (TcService20.c:39-43);
- `FDIR_enableTC20reaction` (FDIR.c:1512-1519): cancel, then
  `rtems_timer_reset`, a fresh 30 s;
- **every NOMINAL entry:** `setMode(E_OPMODE_NOMINAL)` re-applies the ASW
  config, `aswConfig(E_ASW_PARAM_CONFIG_REACTION_TO_SERVICE_20, param)`
  (OpModeMgmt.c:565-566 → HWConfig.c:389-398). It re-arms when the
  parameter is 1 and cancels when it is 0. So in NOMINAL the 0x142E comes
  30 s after the **later** of the last TC(20,128) and the NOMINAL entry.
- Later SAFE entries do **not** re-arm. Only `OpModeMgmt_initMode` does,
  at boot (OpModeMgmt.c:950-969).

**The reaction [C]** (FDIR.c:582-605):
- If `CONFIG_FSW_REACTION_TO_SERVICE_20` is 0, nothing happens. This is
  the parameter ReactionToS20: default 1, range 0..1 (ParamMgmtDef.h:164).
  TC(236,6) ASW parameter 4 sets it, and disabling it cancels the timer
  (HWConfig.c:390-398, FDIR.c:1523-1527).
- Otherwise the FSW sends TM(5,1) **0x142E** (TMForm.c:694-697).
- In SAFE or MAINTENANCE that is all. In any other mode the FSW signals a
  move to SAFE.
- The timer is one-shot: there is **one 0x142E per lapse**.

**The packet [C/D]:**
- APID 0x31C (PID 49, category 12; TCData.c:24, ICD Table 87,
  ICD:10784-10942).
- Packet length field 241, checked exactly (TCData.c:103-107), so the
  packet is **248 bytes**, which is `TCDATA_MAX_PACKET_LENGTH`. The
  252-byte SpW unit fits `SPWCONTROL_RX_PACKET_SIZE` (configFSW.h:169). The 236 bytes
  of application data are 36 platform bytes plus 10 × 20 instrument
  bytes.
- Allowed in any mode (TCVerifDynamic.c:56).
- The FSW ignores the content (TcService20.c:37).
- The ICD says ACK = 0: no TM(1,1) or TM(1,7) (ICD:10944).

**The sequence count [C/D]:**
- It is stored in `SSC_for_PID49_CAT12_SYSCTRL` only when the **source
  ID is 14** (TCData.c:195-201, TcService20.c:46-49).
- SID 2 reports it as the 14-bit "number of TC20_128" (HKAcquire.c:433,
  TM_3.c:146-147).
- The OBC increments it at 8 Hz, and instruments receive the packet "at
  an agreed frequency up to 8 Hz" (ICD:1311, :10946).
- **Neither the FSW nor the ICD gives the STIX period** (open question
  Q1).

The SuSW ignores TC(20,128) altogether (ICD:10783).

### TC(9,129): time synchronisation

**The packet [C/D]:**
- APID 0x5AC (PID 90, category 12).
- Length field 11 (TCData.c:22).
- Application data: coarse u32, then fine u16 (TCData.h:89-90; ICD
  Table 84, ICD:10536-10620).
- Allowed in any mode (TCVerifDynamic.c:54).
- The ICD notes that the S/C does not send it every second.

**The checks [C]:**
- fine ≠ 0, or coarse = 0, gives TM(1,8) FID **48041**
  (TcService9.c:56-61).
- FID **48034** ("already in progress", Timing.c:160-166) is
  **unreachable** with this FSW. All TCs run serially in the TCBuffer
  task (TCBuffer.c:302 → TCExec.c:60 → `Timing_handleTC9`), so a second
  TC(9,129) waits in the queue until the first returns, and by then
  `activeTC9coarse` is 0 again. It then applies its stale coarse 1-2
  codes late. The controller therefore never has two syncs in flight
  (D-PLd). 48034 stays a failure matcher.

**The semantics [C]:**
- The time applies at the **next** time code (Timing.c:20-23, 156-158).
- `Timing_handleTC9` polls in 100 ms steps for up to
  `TIMING_MAX_SYNCHRONISATION_TIMECODE_MS` = **2000**
  (ParamMgmtDef.h:542).
- Then it sends TM(1,7), or TM(1,8) FID **48035** if no code came
  (Timing.c:168-196; ICD:2028-2032).
- While it waits, the TC execution path is blocked for up to 2 s [C]:
  the TCBuffer task executes every TC (TCBuffer.c:302), and the queue
  holds 64 (configFSW.h:172).
- TM(1,7) comes only when the TC's ACK has bit 0x08 (TMForm.c:121);
  TM(1,8) comes regardless (TMForm.c:128-148).

**Applying the code [C]:**
- The FSW sets SCET = (coarse, 0) and marks the time synced
  (Timing.c:204-222).
- It then syncs the code's 6 LSBs against its local time:
  - a shift above 0x40000 (4 s) is refused and traced;
  - a shift below 0x1000 (62.5 ms) is ignored;
  - a shift in between is corrected by the median of 5 samples
    (Timing.c:224-327; ParamMgmtDef.h:541-544).
- **So TC coarse & 63 must equal the TIMECNT of the code that applies
  it.** A 1 s miss heals after 5 codes. A miss of 4 s or more is never
  corrected.

**Without time codes [C]:**
- After 60 000 ms (ParamMgmtDef.h:206) the FSW sets the UNSYNCED bit 31
  and sends TM(5,2) **0x542B** (Timing.c:63-91).
- The first code back within 4 s resyncs the time and sends TM(5,1)
  **0x1429** (Timing.c:275-286).

### Source IDs [C/D]
- The values (TCData.h:10-22; ICD Table 4, ICD:885-920):
  - on-board: 11 AOCS, **14 System Control**, 15 OBCP;
  - ground: 110-113, **120 direct commands**, 121/122 spare. 121 is
    `SOURCE_ID_SpareGS0` in the FSW (TCData.h:18) and "Spare Ground
    source 1" in the ICD;
  - 0 = "ground", which the gates and the GS use (stix_pus.py:37).
- **No source-ID check** exists (TCVerifStatic.c:88-127), so 14 and 121
  are accepted. The FSW does not check the TC sequence count
  (ICD:1308-1310).
- **The per-source SCC parameters work only for TC(20,128).**
  `TCData_storeSuccessfulTcSCC` (TCData.c:195-240) selects a parameter by
  (PID, category, source). TcService20.c:46-49 passes the real PID 49.
  The caller for every other completed TC, TMForm.c:118, passes the
  16-bit packet ID into the `uint8_t in_PID`: 0x1DAC truncates to 0xAC,
  not 90, so they all land in `SSC_for_other`. The PID-90 parameters are
  dead, and only `SSC_for_PID49_CAT12_SYSCTRL` is read (HKAcquire.c:433).
  This is a defect candidate ("Findings for the FSW team").
- **The TM side.** A TM(1,x)/TM(17,2) carries the TC's source ID as its
  destination ID (ICD:1110, :1307), and **TM sequence counters are kept
  per (APID, destination)** (TM_1.c:140/175 via
  `TMForm_getIndexFromDestination`, TMForm.c:55-96; ICD:1312). So the
  acks for source-14 and source-121 TCs form their own SSC streams inside
  APID 0x5A1.

### The model today [M]

**Time codes (§20):**
- generated on `QEMU_CLOCK_VIRTUAL` at each rollover of the **IDPU's own
  SCET** (read through the `scet-source` link to stixctrl), with TIMECNT
  = SCET coarse & 63 (grlib_stixspw.c:403-480);
- re-armed by every guest SCET commit (grlib_stixspw.c:434-437).
- So the "spacecraft clock" is the IDPU clock, and any TC(9,129) is
  self-consistent. **No independent platform time exists.**

**Runtime QOM properties readable today:**
- `/machine/stixspw`: `timecodes`, `link-down`, `tm-dropped`,
  `tmtc-connected`, `rx-dropped`, `tc-resync`;
- `/machine/stixctrl`: `adc-inject[N]` (the injection, not the
  effective value);
- init-time booleans: `hk-adc`, `powerstat`, `hk-prot`, `det-model`
  (grlib_stixctrl.c:1100-1127, grlib_stixspw.c:1013-1036).

**Not readable without MMIO reads:**
- hkCmdSta (the enables, POWERSTAT);
- idefxConfig (quarter power);
- the FSM registers;
- the effective HK ADC value per channel;
- hkIrqSta and hkProtConfig (the protection);
- the watchdog state;
- the SCET;
- the SpW status and control (LS, TR/TQ/IE/LI).

An MMIO read through `human-monitor-command xp` would run the device read
handlers, which the survey warns against (survey §3.4: no
bus-semantics pokes).

**Other model facts:**
- `stixctrl_hk_value` applies the injection first, then the table and
  state overrides (grlib_stixctrl.c:460-505). The injection-free "truth"
  value is not separable today.
- The attenuator is not modelled (grlib_stixctrl.c:410-415, ATT_C
  static).
- **QMP:** the gates use a unix socket. `run-qemu-gs.sh` `STIX_GS_QMP`
  takes a unix path only. The probe's `QmpClient` is AF_UNIX only and
  skips events.
- The gates run QEMU with `-no-reboot`.
- **The flash** is sparse and inverted; an erase punches a hole (§9).
  QEMU creates a 17,716,740,096-byte `flash.bin` in the CWD when it is
  missing.
- Any NOMINAL entry rewrites the flash context (ground-station.md).
- **There is no vmstate** in stixctrl or stixspw (§30).

## Decisions (D-PL)

### D-PLa: the ground station and the platform controller are separate (design rule)

The ATS ground station (GS) is generic and external. It is the **STIX
operator's console**. The controller is the **spacecraft plus the test
bench**.

| Concern | Ground station | Platform controller | QEMU model |
|---|---|---|---|
| STIX operator TCs (ZIX…: modes, parameters, FDIR config, dumps) | **owns** | only as labelled `tc.gs.send` scenario steps (below) | executes (FSW) |
| ELUT upload as an operator activity | **owns** | config hook only, labelled as GS emulation | executes |
| TM display, operator decoding, archive | **owns** | decodes only what it checks (events, acks, CUC, the HK fields of D-PLp); its log is a test record, not an archive | produces |
| TM transport to the GS | — | **relays transparently** (bytes unchanged) | sends to the controller |
| GS TC transport to STIX | sends | **proxies transparently**: it adds the SpW header and drops TCs while the link is down | receives |
| Power (on, off, cycle, boot), pause | — | **owns** | process, `-S`, `cont`, `stop` |
| SpW link up and down | — | **owns** | `link-down` |
| Time codes, OBT | — | **owns** | generates (PL-1) |
| Time sync TC(9,129), keep-alive TC(20,128) | — | **owns**, source 14 | executes |
| Hardware state model (truth and presented) | — | **reads, serves and logs** | exposes read-only (PL-2) |
| Fault catalogue and value forcing | — | **owns** | hooks (`adc-inject[N]`, …) |
| DSS stimulus (flare, `.dssevs`, later `.scmd`) | — | **owns**: the DSS front-end selects and controls (D-PLm) | consumes: the detector model, on the virtual clock |
| Scenarios, expectations, reports | — | **owns** | — |

**Boundary rules (recommended):**
- **STIX TCs from the controller.**
  - In interactive mode: none. The UI has no ZIX buttons.
  - In scenarios: only the explicit step `{"do":"tc.gs.send"}` (one
    vocabulary, D-PLn). It emulates a GS action
    so that headless tests ("enter NOMINAL, then lose the keep-alive")
    need no GS. It is labelled `GS-EMULATION` in the log and the report,
    and it is enabled by `allow_gs_emulation` (default true in run mode,
    false in interactive mode).
  - Why: CI must reproduce scenarios without a GS, and the operator path
    must not grow a second, divergent console.
  - **`tc.platform.send` is restricted to services 9 and 20 in every
    mode** (decided). It also carries the malformed variants of R-d, but
    still only as service 9 or 20. Any other service is refused with 403.
    Operator TCs belong to the GS.
  - **Hooks and fault preconditions that need operator TCs**
    (`hooks.elut_preload`, the TC(236,10) limit and count of D-PLq) run
    only as `tc.gs.send` steps under `allow_gs_emulation`. The catalogue
    declares them and never issues them.
- **Source IDs.**
  - Platform TCs use **14**. That is the on-board System Control source,
    and TC(20,128) needs it for the SID 2 counter.
  - `tc.gs.send` uses **121** (FSW `SpareGS0`, ICD "Spare Ground
    source 1"; configurable).
  - The real GS keeps its own source (0 or 120).
  - So every TC's origin shows in the TM(1,x)/TM(17,2) destination ID
    and in that destination's own TM sequence counter (Source IDs). The
    decoder keys TM continuity on (APID, destination). The FSW's
    per-source SCC parameters do **not** show it (Source IDs).
- **The relay is transparent.**
  - Bare Space Packets (D-B) go to the GS byte for byte, in arrival
    order, including the TM(1,x) for platform TCs.
  - The GS config is unchanged (in 21501, out 21502).
  - The relay can fan out to more than one destination, so an optional
    COSMOS or Yamcs can be attached later as a client (D-PLt).
  - The GS therefore also receives the acks for TCs it never sent
    (destinations 14 and 121), in their own SSC streams. The docs say so.
    An optional relay filter exists and is off by default.
- **GS TCs while the link is down are dropped and logged.** That is
  realistic, and it replaces the model's hold-and-deliver-later queue,
  an accepted deviation (§29). `gs_tc_during_link_down = "hold"` restores
  the queueing.
- **Platform TCs while the link is down or STIX is off are not sent.**
  The keep-alive counts them as "suppressed", and a time sync returns
  409.

### D-PLb: a platform OBT as the time-code source (QEMU)
- **qdev `timecode-source` on stixspw:** `scet` (the default; today's
  behaviour, so every gate and golden is unchanged) or `obt`.
- **In `obt` mode:**
  - OBT = obt_base + (vclock − vclock_base) · 65536 / 1e9, in CUC 4+2
    units;
  - the codes come at the OBT rollovers with TIMECNT = OBT coarse & 63;
  - the guest SCET commit hook does not move them;
  - the skip rule (fine ≥ 0x1000) is unchanged.
- **The runtime `obt` property** (uint64, CUC 32.16):
  - get: the OBT, or the SCET in `scet` mode;
  - set: allowed in `obt` mode only (an OBT jump, which re-arms the
    timer);
  - qdev `obt-epoch` gives the start value.
- The OBT survives a device reset, like `timecodes`.
- No drift in v1 (roadmap R-d).
- **The epoch and the 6-LSB alignment (decided; overridable).** The FSW
  sees only the code's 6 LSBs (Timing.c:236-270). The IDPU SCET starts
  at vclock 0 like the OBT (`stixctrl_scet_now`, grlib_stixctrl.c:172-178).
  - `obt_epoch` is configurable. The default 0x10000000 has
    `& 63 = 0`, so at boot the codes match the FSW's SCET LSBs: no
    shift trace and no jump. The disagreement is only in the upper bits,
    which only the CUC shows, until TC(9,129) aligns them.
  - `obt_continuity = "aligned"` (default): across a cold power cycle the
    continued OBT is rounded **up** to the next value with `& 63` equal to
    the new SCET's LSBs at `cont` (a forward jump of at most 63 s,
    logged). `"exact"` keeps the true continuation and with it an
    arbitrary mismatch.
  - A scenario variant forces a mismatch: `obt_epoch & 63 = 32` gives a
    32 s shift, above 4 s, so the FSW refuses every code until TC(9,129)
    (`gate-timesync.json`, variant `mismatch`). 1..4 would instead give a
    median jump after 5 codes.
- **Why:** in `scet` mode the "spacecraft" follows the instrument, so no
  time-sync test means anything. `obt` reproduces the real situation:
  after boot the FSW clock disagrees with the codes until TC(9,129)
  aligns it [I, live check in PL-4].

### D-PLc: one time code and a wrong time code (QEMU)
- **`timecode-once`** (bool):
  - true arms exactly one correct code at the next rollover, even when
    `timecodes` = false;
  - a no-op when `timecodes` = true;
  - refused while `link-down` is true.
  - This is the UI's "send time code". It lands on a rollover, so it
    cannot drag the FSW clock (§20).
- **`timecode-inject`** (uint32 0..63, write-only):
  - emits one code now, with that TIMECNT, through the TR/TQ/IE/latch
    path;
  - refused while `link-down` is true.
  - It is the fault tool for a wrong value or a mid-second code.
- **Edge cases:**
  - with control.TR = 0 (before `SPW_open`) the model emits nothing
    (grlib_stixspw.c:470). Both properties return an error then, and the
    trace records `emitted=0`;
  - a pending `once` is dropped, and traced, by `link-down = true` and by
    `timecodes` changes. `set_timecodes(false)` calls `timer_del`;
  - `stixspw_tc_arm`, the commit hook and the reset re-arm honour
    `once_pending` and the OBT source.
- New trace: `stixspw_timecode_manual(kind, timecnt, emitted)`.

### D-PLd: timing TC(9,129) to the next time code (controller)
- **The mechanism:**
  1. `qom-get /machine/stixspw obt` gives the coarse and fine values
     atomically.
  2. The next rollover is Δ = (65536 − fine)/65536 s away in virtual
     time.
  3. If Δ ≥ 0.5 s, the controller sends coarse + 1 now. Otherwise it
     waits for the sim clock (D-PLr) to pass the rollover plus 50 ms,
     then sends the new coarse + 1 (Δ ≈ 0.95 s).
  4. That leaves at least 0.5 s for UDP, the RX DMA and the TC task, and
     keeps the code within the FSW's 2 s wait (Timing.c:20-23, :169).
  5. **One sync in flight.** A second `platform.timesync.send` returns
     409 until the first gets its TM(1,7)/TM(1,8), or for 3 s (Key facts:
     a queued TC(9,129) applies late, and 48034 never comes).
  6. The TC is built with **ACK 0x9** (acceptance + completion, as
     tc-smoke's `19`), source 14 and CRC-16/CCITT-FALSE.
- **The target:**
  - "auto" = the next code's coarse;
  - an explicit coarse is allowed (negative tests), with a warning when
    coarse & 63 ≠ the next TIMECNT.
- **The verification**, as a log record and a scenario `await`:
  - TM(1,7) within 3 s;
  - **the exact time:** the full 48-bit CUC of the first heartbeat
    (0x5E5) after TM(1,7) equals the OBT at its arrival within
    **0.25 s + the measured relay latency**, with bit 31 clear. A ±1 s
    window would hide the one-code-late apply: that leaves the CUC
    exactly 1 s behind for 5 codes before the median corrects it
    (Timing.c:288-326);
  - the ordering trace: the TC unit's RX delivery
    (`stixspw_rx_deliver`, with the virtual time, added in PL-1) comes
    before the `stixspw_timecode_emit` whose TIMECNT = coarse & 63;
  - TM(1,8) 48035, 48034 or 48041 is a failure;
  - the `stixspw_timecode_emit` traces (QEMU stderr) confirm the TIMECNT
    of each code.
- **The residual race:** if the main loop starves past the window, or the
  TC queues behind a slow GS TC, the sync applies one code late, and the
  FSW median-corrects it in 5 codes. The exact-time check and the
  ordering trace catch it; the verdict names it `late_apply`.
- **Rejected:** QEMU injecting a TC unit after the next code. It would
  put PUS and CRC building into the model, and the FSW's tolerance makes
  it unnecessary.

### D-PLe: the TC(20,128) keep-alive (controller)
- **The period:** default 1 s (Q1). Paced on **virtual time** (D-PLr), so
  a QMP `stop` or a slow host cannot fake a lapse.
- **The sequence count** follows the OBC's 8 Hz counter:
  floor(t_sim · 8) mod 2^14 (ICD:1311).
- The packet: source 14, ACK 0 (ICD; the FSW accepts any subset of 0x9,
  TCVerifStatic.c:121), 236 zero bytes, CRC-16/CCITT-FALSE.
- The count uses bench time (D-PLr), so it never jumps back across a
  power cycle.
- **Start:** by default from the first TM after boot. `autostart =
  "first_tm" | "boot" | "off"`.
- Suppressed while the link is down or STIX is off. So an outage longer
  than 30 s gives 0x142E, which is real and the point of the bench.

### D-PLf: the power model (controller)
- **Power on:** spawn QEMU with `-S`. The device is realized, the machine
  reset is done, and no instruction has run.
- **Boot:** QMP `cont`. Its sim time is T0 for scenarios.
- **Switches set while frozen** apply from the first instruction:
  `link-down`, `timecodes`, `obt`, forcings, the DSS scene [I: confirm
  that `link-down` set at `-S` survives `cont`].
- **Power off:** QMP `quit`, then SIGTERM, then SIGKILL after 5 s. RAM,
  BRAM and the SCET are lost; the flash stays (§9; the flash `fflush`es
  after every write).
- **Power cycle:** off, an off-time (default 2 s), on, then boot if
  `cycle_boots`. It is always a **cold** cycle (a new process).
  `system_reset` is not a power cycle: the SCET and the time-code timer
  survive it (§20).
- **OBT continuity:** before `cont`, the controller sets `obt` = the last
  OBT + the off-time, which is timed on the wall clock (D-PLr bench
  time), then aligns it per `obt_continuity` (D-PLb).
- **The `-S` survival is [C/M].** The machine reset runs inside
  `qemu_machine_creation_done` (machine.c:1791), before the main loop
  serves QMP, and `qmp_cont` resets nothing unless `runstate_needs_reset`
  (qmp-cmds.c:80). So `link-down`, `timecodes`, `obt` and `adc-inject`
  set while frozen survive `cont`. core-smoke keeps the live check.
- **Guest reset** (the watchdog, an FSW reboot, a FORCE). The policy is
  `on_guest_reset = "warm" | "cold" | "halt"`, **default `warm`**
  (decided; overridable). Every guest reset is a `power` log record with
  its reason and the policy applied.
  - **`warm`** is in-process, as on the hardware. QEMU runs with
    `-action reboot=reset` instead of `-no-reboot`, and the controller
    handles the QMP `RESET` event (reason `guest-reset`). BRAM (MemBram,
    WDT `UNCHANGED_STORAGE`) survives, as §13 intends (the FSW reads and
    clears it at every boot, WDT.c:239-276). The SCET, the vclock, the
    OBT and the flash continue; `boot_id` increments.
    - Afterwards the controller re-applies what the device reset clears:
      `link-down` (cleared, grlib_stixspw.c:1053-1058), and the HK
      injections and forcings (cleared, grlib_stixctrl.c:1015-1022), if
      they are still set in the controller's state. `timecodes` and the
      OBT survive on their own. The keep-alive restarts per `autostart`.
  - **`cold`**: `-no-reboot`; QMP `SHUTDOWN` (guest-reset), and QEMU
    exits. The controller cold-restarts, which is a power cycle: BRAM,
    the SCET and the reset reason are lost. It is labelled "cold restart
    (not hardware-like)" in the log and the reports.
  - **`halt`**: `-action reboot=shutdown,shutdown=pause` keeps the process
    for inspection (GDB, PL-10). The controller reports `halted`.
- **Pause and resume** are QMP `stop`/`cont`. The virtual clock, the OBT,
  the codes, the keep-alive and the scenario timeouts all freeze
  together (D-PLr).

### D-PLg: the transport topology (controller)
```
QEMU stixspw --TM (SpW hdr + SP), UDP--> controller :21511 --bare SP, UDP--> GS in  :21501 (+ optional clients)
(udp-spw-header=on)                      relay, decode, log
             <--TC (SpW hdr + SP), UDP-- controller        <--bare SP, UDP--  GS out :21502
bound 127.0.0.1:21512                    proxy :21502, TC9/TC20 sender
QMP unix:<run>/qmp.sock <--------------- controller (holds the single QMP client; reads events)
TCP TMTC server: off (-global grlib-stixspw.tmtc-server=off)
```
- With the SpW header on, the units are the probe's framing exactly.
- The TC proxy is mandatory in Docker: QEMU binds TC on 127.0.0.1 only.
- **QEMU's TCP TMTC server is off.** UDP is independent of it
  (grlib_stixspw.c:840-858). Its default port, 20000 + uid = 21000, is
  the gates' port, and 21500 is the GS bridge's and `run-qemu-gs.sh`'s
  (ground-station.md:181). A probe that must attach gets a 2151x port
  explicitly.
- **The ports:**
  - interactive and run mode: 21511 and 21512 (QEMU side), 8080 (API);
  - the GS side: the controller binds **21502/udp on purpose**, because it
    replaces QEMU for the GS, and sends TM to 21501;
  - the controller's own live gates use **2152x** and a fake GS on 2153x.
    They never use 21000, 21500-21503 or the interactive ports, so a gate
    can run while an interactive controller is up.

### D-PLh: file formats
- **Config:** TOML via `tomllib` (stdlib ≥ 3.11). It is hand-edited and
  takes comments.
  - Fields: `mode = "interactive" | "run"`; the scenario list; the run
    dir (path, `fresh`); the QEMU binary, ELF, `timecode_source`,
    `obt_epoch`, `on_guest_reset`; the ports; the GS destinations; the
    keep-alive and time-sync autostart; `allow_gs_emulation`; the path
    to the IDB TM JSON (D-PLp); `hooks.elut_preload`.
- **Scenarios, the fault catalogue, the API and the logs: JSON**
  (survey §3.3).
- **Run mode:** each scenario (and each selected variant, D-PLq) runs in
  its own fresh QEMU process and, by default, a fresh flash.
  - Output: `reports/<name>[.<variant>].json` plus a text summary,
    `summary.json` and the JSONL slice.
  - Exit codes: **0** all passed, **1** a scenario failed, **2** a
    config or scenario error, **3** an infrastructure failure.

### D-PLi: reuse the probe; a controller QMP client with events
- The controller imports `tmtc-probe.py` with `importlib`, for
  `frame_units`, `unit_fields` and `Expect`/`CucExpect`/`Forbid`. **The
  probe file does not move**, so the gates are unchanged.
- **The probe has no CRC code** (tc-smoke hard-codes its hex).
  `tcbuild.py` implements CRC-16/CCITT-FALSE: init 0xFFFF, poly 0x1021,
  no reflection, over the packet without its CRC. Checked: it gives
  `FA 63` for the tc-smoke TC(9,129).
- **The controller's `QmpConnection`:** a reader thread, an event queue
  (`SHUTDOWN`, `STOP`, `RESUME`, `RESET`), one command at a time matched
  by `id`, and the same handshake.
- QEMU allows one QMP client, and the controller holds it. External
  tools use the REST API.

### D-PLj: security (no authentication, so localhost only)
- **QMP:**
  - a unix socket in the run dir (mode 0600);
  - an optional second `-qmp tcp:127.0.0.1:N`, not published in Docker;
  - publishing it would need 0.0.0.0 inside the container, which the
    docs advise against.
- **UI and API:**
  - in the container they bind 0.0.0.0, and `run-container.sh` publishes
    them **only** on `127.0.0.1` (`-p 127.0.0.1:8080:8080`, and the same
    for 21502/udp);
  - natively the controller binds 127.0.0.1.
- **Stdlib protections against CSRF and DNS rebinding:**
  - refuse a `Host` that is not `localhost`, `127.0.0.1` or `[::1]`
    (with an optional port);
  - every state change is a `POST` with `application/json` **and**
    `X-Stix-Platform: 1`;
  - no CORS headers;
  - SSE is GET-only and read-only.
- **`run-qemu-gs.sh` `STIX_GS_QMP`** also takes `tcp:127.0.0.1:N` or
  `tcp:localhost:N` (PL-2). Other hosts are refused.

### D-PLk: extension points
- **A controller module** has:
  - `name`;
  - `verbs()`: the commands it adds to the shared dispatcher (D-PLn);
  - `state()`: its subtree (D-PLo);
  - `faults()`: its catalogue entries (D-PLq);
  - `describe()`: the controls, with types, ranges, units and the
    **index space**. The UI renders generic panels from it;
  - `on_power(event)`: it re-applies its state after a cold power-on,
    before `cont`.
- **Namespaces:**
  - `platform.*` (the core: power, spw, timecode, obt, timesync,
    keepalive);
  - `force.*` and `fault.*` (D-PLq);
  - `stim.<name>.*` (DSS, R-a0);
  - `view.<name>` (read-only views, such as the flash browser R-c).
- Adding a module never touches the core, the runner or the page layout.

### D-PLl: run dir, flash and the sparse check
- **Contents:** `flash.bin` (made by QEMU), `uart.raw`, `qemu.log`,
  `qmp.sock`, `log.jsonl` (D-PLn), `tm.jsonl` and `reports/`.
- **The guard:** the controller refuses `/tmp/stix-*-run` and
  `/tmp/stix-baseline-run`. It never touches `flash.bin` itself, except
  for the later read-only flash view (R-c).
  - Its own gates use `/tmp/stix-plat/<gate>/`, which the guard's glob
    does not match. (A `/tmp/stix-plat-run` would.)
- `fresh = true` removes only its own named files.
- **The run-dir preflight (before the first spawn).** The controller
  creates `.stixplat-sparse-probe`, truncates it to 1 GiB, checks that
  `st_blocks` stays near 0, tries `FALLOC_FL_PUNCH_HOLE` on it, and
  deletes it. On a failure it refuses: "run dir does not support sparse
  files or hole punching". It also refuses an existing `flash.bin` whose
  `st_blocks·512` is ≥ 1 GiB (not sparse). This runs **before** QEMU can
  `ftruncate` a 17.7 GB file on a filesystem without sparse support (a
  `/mnt/c` or grpcfuse bind mount).
- **The sparse check (second line):** after the first power-on,
  `st_blocks·512` must still be under 1 GiB. Otherwise the controller
  powers off and fails.
- **The binary guard (hard rule).** A QEMU older than `21f898484f` (D-I,
  sparse inverted flash) **writes a plain 16.5 GB `flash.bin`** when the
  file is missing. Before every power-on with no `flash.bin` present, the
  controller probes the binary **without instantiating the machine**:
  `qemu-system-sparc -M none -S -display none -qmp stdio` with
  `qom-list-properties typename=grlib-stixflash`. The type must have
  `inverted-storage`, or the controller **refuses to start**: "QEMU
  predates 21f898484f; it would write a 16.5 GB flash". `-M none` creates
  no leon3 devices, so the probe cannot create a flash.
  - The controller also passes `-global grlib-stixflash.inverted-storage=on`
    explicitly.
  - The probe result is cached per binary (path, size, mtime) and logged.
  - With an existing `flash.bin` an old binary is still refused by
    default: it would read the inverted file as plain storage.
- **Flash copies are sparse only.** The controller never copies a flash.
  Docs and scripts that copy one (a backup, a checkpoint) must use `cp
  --sparse=always` (or `tar --sparse`) and check `du` afterwards. A plain
  `cp`, `docker cp`, or a tar without `--sparse` is forbidden in the
  docs. `docker cp` does not keep holes, so a volume is backed up with
  `docker run --rm -v stix-run:/v … tar --sparse -cf - …`.
- **Retention and cleanup** (`[retention]` in the config):
  - Run mode writes every scenario run dir under
    `<run_root>/runs/<UTC stamp>-<scenario>[.<variant>]/`.
  - The controller keeps the last `keep_runs` (default 10) and the failed
    ones from the last `keep_failed_days` (default 7). It deletes the
    rest at start and after each run, and only dirs that carry its own
    marker file `.stixplat-run`.
  - A scenario's `flash.bin` is deleted at the end of a **passed** run
    (`keep_flash = "failed"`, the default), since it is rebuilt fresh
    anyway. A failed run keeps it (sparse, typically a few MB) for
    inspection.
  - `log.jsonl` and `tm.jsonl` rotate at `max_log_mb` (default 50 MB,
    gzip of the rotated part); at most `max_log_files` (default 5) are
    kept per run dir.
  - `max_total_mb` (default 2048) caps the whole run root. When it is
    over the cap, the oldest passed runs go first. The controller refuses
    to start a run when the free space under the run root is below
    `min_free_mb` (default 1024). The free space is measured **inside**
    the volume, which in Docker Desktop is also the real VHDX limit.
  - The verb `run.cleanup {dry_run}` and `GET /api/v1/runs` expose the
    same logic (D-PLn), and each deletion is a `log` record.
  - The interactive run dir (persistent flash) is never cleaned
    automatically.

### D-PLm: the DSS front-end and the detector model (interface rule)
- **No per-event QMP streaming;** it is far too slow at flare rates.
  - The controller turns a stimulus into a **pre-scanned event table**,
    writes it into the run dir (the same FS as QEMU) and has QEMU
    **load it by path**.
  - It then controls the run with start/offset/stop, the flare switch and
    the source, and reads back the status. All of this is QMP.
  - There is no bulk QMP load.
  - **Pacing is QEMU-side, on the virtual clock.**
- **Inputs:**
  - `.dssevs` (D-7 format: big-endian u32 `[31:20] adc | [19:16] type |
    [15:11] dss_det | [10:0] wait (20 ns)`);
  - later `.scmd` (R-e).
  - Conversions are cached in the run dir, keyed by size, mtime and
    sha256. A 176 MB file takes tens of seconds in pure Python [I].
- **Index spaces:**
  - `dss_det` = `fpga_det` (identity; utils.c:129-133,
    Detectors.c:528-540; the detector plan's "Index spaces"). Note:
    utils.c:121-127 is `utils_fpgaIdFromSubcolId`, the **new** `subcol`
    → `fpga_det` table. It is the wrong one here, and notes-dss-app §6
    item 3 uses it by mistake (detector plan, Doc vs code 13);
  - the `.scmd` "sc" is `fe_sc` (**1..32**), and
    `fpga_det = utils_oldSubcolIdToFpgaId[fe_sc − 1]`
    (= `Detectors_fpgaFromFrontendMinusOne`, Detectors.c:536-540). The
    −1 is part of the rule;
  - every **stimulus-interface** field names its space, and `subcol`
    never appears there. The TM-side checks (the R-a0 QL awaits) are
    `subcol`-indexed and say so.
- **Requirements on the detector plan** (input to the D-4/D-7 briefs):
  1. **The event-table format "STXEVT" v1.** QEMU loads it in addition
     to (or instead of) raw `.dssevs`.
     - Little-endian. A 32-byte header: magic `STXEVT\0\0`, `version`
       u16 = 1, `index_space` u16 = 1 (`fpga_det`/`pix`/`adc`),
       `record_size` u16 = 16, reserved u16, `count` u64,
       `duration_ns` u64.
     - Records of 16 bytes, sorted by `t_ns`: `t_ns` u64 from the table
       start; `kind` u8; `fpga_det` u8 (0..31, 0xFF = none); `pix` u8
       (0..12, 12 = guard ring: counted and dropped); reserved u8;
       `value` u16; `aux` u16.
     - `kind`: 0 hit (`adc` 0..4095); 1 baseline (`adc`); 2/3
       temperature ADC 1/2; 4 aux temperature (`aux` = sensor 0..7,
       `value` = 8-bit code); 5 SEU (`value` = `pix` mask); 6 false
       trigger (0..63); 7 PSU load (`aux` = `quarter` 0..3, `value`
       0..7); 8 threshold (the `.dssevs` 0xD word, `value` = its val).
     - **Kind 1 (baseline) is consumed, never "unsupported":** D-Dk's
       ELUT fallback takes `base[dss_det]` from it (the 0xE words).
       Kind 8 may be counted as unsupported, but the converter never
       drops it.
     - Kinds the model does not implement are **counted and reported**,
       never silently ignored.
     - Load errors are QMP errors that name the path and the reason.
  2. **Load by path at run time:** `det-replay-file` (init-time in D-Db)
     becomes runtime-settable, or a new `det-replay-load` is added.
     - It works while the VM is frozen, or running with no replay
       active.
     - An STXEVT load must not stall the main loop (60 k events is about
       1 MB).
     - A raw `.dssevs` load at run time may be refused.
  3. **Control:**
     - `det-replay-trigger` (`nominal` | `qmp`) and `det-replay-start`,
       as in D-Db;
     - **one offset property, `det-replay-offset-ms`** (uint64; second
       steps are too coarse). It **replaces** D-Db's `det-replay-offset`
       (uint32 s) before D-7 lands, so two overlapping properties never
       exist;
     - `det-replay-start=false` stops the replay;
     - an optional `det-replay-loop`.
  4. **An atomic status read-back:** one read-only string
     `det-replay-status` = `state=idle|armed|running|ended|stopped,
     pos_ns=…,events=…,dropped=…,unsupported=…,table=…,count=…,
     duration_ns=…`. Plus readable `det-source`, `det-flare` and the
     rates (D-Db).
  5. **Every scene knob is settable while frozen at `-S`.** After a cold
     power cycle the controller re-applies the scene before `cont`.
  6. **Timing:** events on `QEMU_CLOCK_VIRTUAL` from the start instant,
     where offset = the table time at the start. The model bins them to
     its 100 ms tick (D-Dk), so the `t_ns` precision is only for the
     record. The start and stop
     instants are traced (`stixdet_replay_state`) with the virtual time.
  7. **Naming:** every property, trace argument and table field carries
     its index space (`fpga_det`, `pix`, `adc`, `quarter`).
  8. **The hardware state (D-PLo):** the detector adds a `det` section to
     stixctrl's `hw-state` (quarter power, FSM enable, the active QL
     bank, the replay status line), through a `stix_det` accessor. That
     keeps the merge with this branch to a single call site.

### D-PLn: API-first: REST + SSE + JSONL, one vocabulary (survey §3.1, §2)
- **The UI is one client of the API and holds no state the API does not
  serve.** Everything the UI shows or does is in the API and in the log.
- **One command vocabulary, two front-ends** (the COSMOS pattern,
  survey §3.1):
  - REST `POST /api/v1/cmd/<verb>` with JSON args, and a scenario step
    `{"do": "<verb>", "args": {…}}`, go through **the same dispatcher**,
    with the same validation and the same log record.
  - **The v1 verbs:**
    - `platform.power.{on,boot,off,cycle}`;
    - `sim.{pause,resume}`;
    - `platform.spw.link {state: up|down}`;
    - `platform.timecode.{enable,once,inject}`;
    - `platform.obt.set`;
    - `platform.timesync.send {coarse: auto|N}`;
    - `platform.keepalive.set {enabled, period_s}`;
    - `force.{set,clear}`;
    - `fault.{inject,clear}`;
    - `tc.platform.send` (raw, source 14; **services 9 and 20 only**,
      D-PLa);
    - `tc.gs.send` (GS emulation, gated by `allow_gs_emulation`);
    - `scenario.{run,abort}`;
    - `log.mark`;
    - `run.cleanup` (D-PLl);
    - `debug.attach {port}` / `debug.detach` (PL-10).
    - Modules add `stim.*` later.
  - **The namespaces** are those of D-PLk plus `sim.*`, `tc.*`,
    `scenario.*`, `log.*`, `run.*` and `debug.*`.
- **Read endpoints:**
  - `GET /api/v1/status`: a snapshot of the power, link, time code, OBT,
    keep-alive, mode, the active faults and forcings, the scenario, and
    the latest seq;
  - `GET /api/v1/state` and `/api/v1/state/<path>` (D-PLo);
  - `GET /api/v1/faults` (the catalogue plus the active ones);
  - `GET /api/v1/time` (sim, wall, OBT, running/paused);
  - `GET /api/v1/log?since=<seq>&kinds=…` (a JSONL slice);
  - `GET /api/v1/verbs` (the vocabulary with arg schemas; the UI and the
    docs are generated from it).
- **The stream:** `GET /api/v1/stream?topics=cmd,state,tm,tc,fault,expect,
  mismatch,uart,log` is SSE: one stream with typed topics.
  - Resume uses `Last-Event-ID` = seq (the Yamcs idea, survey §3.1).
  - It is not a WebSocket, because that needs hand-written framing in
    stdlib.
- **One record schema for the SSE events and `log.jsonl`:**
  `{"v":1,"seq":N,"sim_time":<ns>,"boot_id":B,"vclock_ns":<ns|null>,"wall_time":"…Z","source":"ctl|qemu|fsw|tm|tc|scn|api","kind":"cmd|result|state|power|tm|tc|fault|force|expect|mismatch|uart|log|debug","path":"<state path or verb>","data":{…}}`.
  - `sim_time` is the continuous **bench time** (D-PLr, decided): it never
    goes back across power cycles. `boot_id` counts every power-on and
    every guest reset. `vclock_ns` is the raw QEMU virtual clock of the
    current process (null while off). Records the controller cannot sample
    exactly (a TM arrival) carry `"sim_est": true`.
  - **Kind → topic:** `cmd` and `result` → `cmd`; `state` and `power` →
    `state`; `fault` and `force` → `fault`; `debug` → `log`; every other
    kind → the topic of the same name.
  - Every command, its result, every state change, every decoded TM and
    TC, every UART line, every fault and forcing, and every expectation
    verdict is one record. **The log is the recording** (D-PLs).
- **Versioning and stability:**
  - The path prefix `/api/v1`; `"v":1` in every record; `"api":1` in
    scenario and catalogue files.
  - **v1 is provisional until PL-7 is accepted** (the Docker release).
    Breaking changes are allowed until then, with a CHANGES note.
  - **From then on v1 is additive-only:** new verbs, fields, kinds and
    topics are fine, and clients must ignore unknown fields. A rename,
    a removal or a semantic change goes into `/api/v2`, and v1 stays
    served for at least one release.
  - The JSONL schema follows the same rule. A reader keys on `v`.
  - `GET /api/v1/verbs` is the normative list. A self-test checks that
    it matches the docs table.

### D-PLo: the hardware state model, a tree with three layers (survey §3.2; NOS3 data providers, Basilisk truth vs sensed)
- **The tree** (paths are the API's `path`):
  - `platform.{power, spw.link, timecode, obt, keepalive, timesync}`
    (controller-owned truth);
  - `idpu.{scet, watchdog, spw.{ls, control, rx_queued, counters}}`;
  - `instrument.supplies.{lv, hv01, hv17, spw0, spw1}`;
  - `instrument.hk_adc.<ch_hk>`;
  - `instrument.hk_prot.<line>`;
  - `instrument.detector.{quarter.<q>, fsm}`;
  - later `instrument.attenuator.*`;
  - `stimulus.dss.*`; `scenario.*`.
- **Each leaf** is `{truth, presented, reported, unit, index_space,
  t_sim, status: ok|pending|mismatch|stale|fault|unknown, fault_ids}`.
  - `truth`: what the hardware or "physics" is.
  - `presented`: what the emulated register or ADC shows the FSW. It
    equals truth unless a forcing or a fault is active.
  - `reported`: what STIX says in TM (D-PLp).
  - **"Break a component" changes `presented`, never `truth`.**
- **Readable today, through `qom-get`, with no QEMU change:**

  | Item | Property |
  |---|---|
  | `platform.spw.link` (as forced) | `/machine/stixspw` `link-down` |
  | `platform.timecode.enabled` | `/machine/stixspw` `timecodes` |
  | `instrument.hk_adc.<ch>` forcing | `/machine/stixctrl` `adc-inject[N]` |
  | SpW counters | `tm-dropped`, `rx-dropped`, `tc-resync`, `tmtc-connected` |
  | model switches | init-time `hk-adc`, `powerstat`, `hk-prot`, `det-model` |

- **Needs new read-only QOM properties** (PL-2):
  - **`/machine/stixctrl` `hw-state`** (str, read-only). A JSON object
    built with QDict and `qobject_to_json`, sampled atomically on the
    main loop, with `"schema":1` and `vclock_ns`:
    - `supplies`: the hkCmdSta enables (LV, HV01, HV17, SPW0, SPW1,
      bits 4:0) and POWERSTAT (bits 9:5, as composed by
      `stixctrl_hkcmdsta`);
    - `hk_adc`: for each used channel, `truth` (table + state overrides,
      **without** the injection) and `presented` (with it). This needs
      `stixctrl_hk_value` split into `_truth` and the injection wrapper,
      a pure refactor with identical results. Also the last conversion
      (`value`, `vclock_ns`) per channel, for staleness. That needs one
      store in the hkAdc write path: not a pure getter, but
      golden-neutral;
    - `hk_prot`: IRQEN, PROTSTAT, HOTSAMPLE, and the 9 thresholds and
      ProtEn bits;
    - `watchdog`: running, count, reload (`stixctrl_wdg_count`);
    - `scet`: coarse, fine (`stixctrl_scet_now`) and the UNSYNCED flag
      (`unsynced_flag`, grlib_stixctrl.c:707);
    - `det`: `quarter_on[4]` (idefxConfig 3:0) now. The FSM, the QL bank
      and the replay come through the detector branch's accessor
      (D-PLm req. 8).
  - **`/machine/stixspw` `hw-state`** (str, read-only):
    - LS, `link_down`, control TR/TQ/IE/LI;
    - `timecode`: source, enabled, `once_pending`, last TIMECNT and its
      `vclock_ns`, the next due `vclock_ns`;
    - `obt`; the RX queue depth; the counters; `vclock_ns`.
  - **`/machine/stixspw` `vclock-ns`** (uint64, read-only):
    `qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)`. This is the controller's
    time master (D-PLr).
  - Each is a getter with no side effects and no register writes, about
    150 lines in total plus qtests. A string-JSON property is chosen over
    QAPI structs so that generic `qapi/` files stay untouched; the
    `schema` key versions it.
- **Polling:** the controller samples both `hw-state`s at 2 Hz while
  running (two QMP round trips of well under 1 ms). It logs a `state`
  record only when a leaf changes, and immediately after every command.
- **The read-only proof:** the getters never call the MMIO handlers. The
  CTRL read handler stores `longTimerSec/SubSec/watchDog` into `s->reg`
  on every read (grlib_stixctrl.c:704-710), and `stix_det_read` composes
  live state. The qtest therefore compares two `hw-state` reads with no
  `clock_step` in between, and asserts that no `stixctrl_io_read` or
  `stixspw_io_read` trace fires during the getters.

### D-PLp: truth vs telemetry (survey §3.2)
- **The comparison:** for each item with a TM counterpart, `truth`,
  `presented` and `reported` are compared, each with a **tolerance** and
  a **latency allowance of N HK cycles** (default 2 of the item's
  packet's cadence).
  - Inside the allowance the status is `pending` (yellow).
  - Beyond it, a disagreement is `mismatch` (red), and a `mismatch` log
    record is emitted.
  - No TM for longer than the allowance is `stale`.
  - A disagreement with an active fault on the item is `fault` (expected
    by design; the record names the fault id).
- **v1 items:**
  - **supplies:** hkCmdSta enables vs POWERSTAT vs SID 2 bytes 62-63
    "LV / HV01-16 / HV17-32 enabled" and "SPW0/1 power status" [G];
  - **quarters:** idefxConfig vs SID 2 "Q1..Q4 power status" [G];
  - **HK values:** presented ADC vs the SID 1/SID 2 HK_* fields (1V5_C,
    SPW_C, SPW0/1_V, 1V5_V, ATT_V/T, HV_x_V, DET_Qx_T, ATT_C, DET_C) [G].
    The tolerance is 0 LSB [I: confirm that the FSW reports the raw
    sample without averaging];
  - **active SpW link:** SID 1/2 "Active SPW link" vs the model;
  - **the TC20 count:** the controller's last seq & 0x3FFF vs SID 2;
  - **time:** OBT vs the heartbeat CUC (bit 31 = unsynced);
  - **mode:** the 0x0021 events vs the SID 1/2 mode nibble. This is a TM
    vs TM check; there is no hardware truth for the mode.
- **The decoder:**
  - **The item dictionary** maps each tree path to the IDB parameter
    name (NIXD…) (survey §3.2, the OpenSVF name contract). A renamed or
    missing parameter fails loudly at start.
  - Offsets and bit positions come from the GS's `stix_idb_tlm.json`
    (configured path, read-only, **not vendored**; the IDB is the
    user's). Without it the comparison is disabled, with a status note.
  - The self-test cross-checks the dictionary's offsets against
    TM_3.c's packing on a real SID 2 unit [I: the 12-bit packing and
    bit offsets in the JSON need confirming].
  - The HK cadences: SID 2 every 64 s (ground-station.md); SID 1 to be
    read from the FSW [I].

### D-PLq: the fault catalogue, separate from value forcing (survey §3.4; SMP2 Fail/Unfail, TEMU)
- **Forcing** (`force.set {path, value}` / `force.clear`) sets a
  `presented` value directly. In v1 that is `instrument.hk_adc.<ch>` via
  `adc-inject[N]`. It is a low-level tool and is logged as `force`.
- **Named failures** (`fault.inject {id, params, trigger}` /
  `fault.clear {id}`) come from a JSON catalogue. Each entry has:
  - `id` (dotted, e.g. `attenuator.sensor_ab.stuck_open`);
  - `class` (`component|signal|link|memory|timing`) and `target` (a tree
    path);
  - `mode` (`stuck_at|offset|drop|disconnect|wrong_value|bitflip|…`);
  - `params` (with units, ranges and the index space);
  - `persistence` (`transient|until_cleared|permanent|until_write`);
  - `trigger` (`now|at_sim|on_event|on_tm`) and `reversible`;
  - `preconditions` (for example "a TC(236,10) limit is tightened");
  - `expected_detection`: a list of TM matchers with a latency, in the
    probe grammar, which the scenario runner can assert;
  - `seed` (for noisy modes).
- **v1 entries,** all on existing model hooks:
  - `power.<line>.undervoltage` and `.overvoltage`, `thermal.<sensor>.
    overtemp` (adc presented value, with the SW-limit preconditions of
    ground-station.md);
  - `hk_prot.<line>.overcurrent` (adc above the threshold, giving
    0x9434);
  - `spw.link.disconnect` (`link-down`);
  - `timecode.missing` (`timecodes` off);
  - `timecode.wrong_value` (inject);
  - `obt.jump` (an `obt` set);
  - `keepalive.missing` (keep-alive off).
  - Some of these duplicate platform switches (SpW down, time codes
    off). Using the fault form records the *intent* (a failure, with its
    expected detection) for reports.
- **Every inject and clear is a `fault` log record** with the resolved
  params and seed, so a run can be replayed from its log (D-PLs).
- A fault changes `presented` only (D-PLo). The comparison view then
  shows it as `fault` straight away.

### D-PLr: time: QEMU's virtual clock is the single master (survey §3.5)
- **The times in every record:**
  - `sim_time`: the **bench time** (decided) = Σ (the final vclock of each
    earlier QEMU process) + Σ (the off-times) + the current vclock. A cold
    power-on and a `cold` guest reset start a new process whose vclock
    starts at 0. While STIX is off no vclock exists, so the off-time is
    timed on the **wall clock**, the one documented exception to "no wall
    pacing";
  - `boot_id`, `vclock_ns` and `wall_time`;
  - in `data`, the OBT where relevant.
- **The mechanism (v1):** a controller `SimClock`.
  - It samples `qom-get /machine/stixspw vclock-ns` at 20 Hz while QEMU
    runs (a sub-ms QMP round trip), and keeps a sim↔wall estimate from
    the last sample.
  - The QMP `STOP` and `RESUME` events freeze and unfreeze it.
  - **All controller timers are bench-time deadlines:** the keep-alive,
    the scenario `at`/`after` and `timeout`, the D-PLd wait and the stale
    checks. The off-time runs on the wall clock (above).
  - A STOP from the gdbstub (runstate `debug`, PL-10) freezes the clock
    like any STOP.
  - **QMP is one serial channel.** A slow command (the `link-down` clear
    runs TX and RX DMA) delays a clock sample. The client is never held
    across a wait, and each sample's round trip is recorded (min/max in
    `status`).
  - The scheduler wakes at the earlier of the estimated due time and
    50 ms, re-reads `vclock-ns`, and fires only when it has passed. The
    jitter is ≤ 50 ms plus a round trip: enough for a 1 s keep-alive
    against a 30 s timeout. Precise per-second edges come from the
    `stixspw_timecode_emit` traces.
  - Under `-icount` (roadmap) nothing is paced on the wall clock, but
    with `sleep=off` the 50 ms wall poll becomes up to (speed × 50 ms) of
    sim jitter. Precise pacing there needs R-t's `stop-at-vclock-ns`.
- **Rejected for v1:**
  - a custom QMP event (`STIX_TICK`), which needs schema changes in
    generic `qapi/` files;
  - a tick chardev, which is more plumbing for the same 50 ms.
- **Roadmap R-t:**
  - `stop-at-vclock-ns`: a write-only property on stixspw that arms a
    virtual timer and calls `vm_stop`, about 40 lines. It gives "run to
    T" and stepping (survey §3.5 option a);
  - speed control via `-icount shift=N,align=…,sleep=off`, with a
    measured-ratio readout;
  - single-stepping.
- **Pause and resume** are QMP `stop`/`cont` in v1 (`sim.pause` /
  `sim.resume`).

### D-PLs: record and replay in three tiers (survey §3.6)
- **Tier 1, always on (v1):**
  - `log.jsonl` holds every input (commands, TCs sent, time codes, link
    and power changes, forcings, faults with seeds, stimulus file + offset
    + hash) and every output (TM hex, UART lines, state changes,
    verdicts);
  - plus a run header: the config, the ELF sha256, the QEMU `git
    describe` and the scenario.
  - **Replay** (roadmap R-r1, S) re-issues the recorded commands at their
    `sim_time` against a fresh QEMU and diffs the outputs. It is
    deterministic only as far as the guest is.
  - A UI "load a log" view (roadmap) feeds the same views from a file
    (the Yamcs replay-processor idea).
- **Tier 2 (roadmap R-r2): `-icount … rr=record`** for bit-exact replay.
  - **Prerequisite:** every input must pass through QEMU's recorded
    devices. stixspw's TCP and UDP sockets and stixflash's stdio file I/O
    bypass them.
  - So SpW would have to move to a chardev and the flash to the block
    layer. **L**; risky.
- **Tier 3 (roadmap R-r3): `savevm`/`loadvm` checkpoints** (skip the
  boot).
  - **Prerequisite:** vmstate for stixctrl, stixspw and stixflash (none
    today, §30), and a snapshot-capable flash backend (qcow2 through the
    block layer, or a reflink copy of the sparse file at the checkpoint).
  - **M-L.**

### D-PLu: disk budget (a hard constraint)
- **The fact.** The real free disk is about **19 GB** (2026-11-11; it was
  about 13 GB earlier that day) on the Windows host
  C:, which holds the WSL VHDX and the Docker Desktop images. WSL's `df`
  (814 GB) is misleading, and **the VHDXs do not shrink when files are
  deleted**: every GB written stays used until the VHDX is compacted by
  hand. Every byte the controller, the image or a run writes counts
  permanently.
- **The image budget.** A multi-stage build. The final image carries
  only `qemu-system-sparc`, its runtime libraries, `python3` and the
  controller.
  - **Builder stage (estimate):**

    | Component | Size |
    |---|---|
    | `ubuntu:24.04` | ≈ 80 MB |
    | apt build deps (gcc, ninja, glib-dev, pixman-dev, python3-venv, flex, bison, git) | ≈ 450-550 MB |
    | build context (the tree without `.git` and `build*/`) | ≈ 175 MB |
    | sparc-softmmu build dir (the host's `qemu-stix-uprev/build` is 924 MB) | ≈ 0.9 GB |
    | **builder total** | **≈ 1.6-1.8 GB** |

    BuildKit's cache can add about the same again until it is pruned.
    **Plan for a ≈ 2-3.5 GB peak** during the build.
  - **Final stage (estimate):**

    | Component | Size |
    |---|---|
    | `ubuntu:24.04` | ≈ 80 MB |
    | `python3-minimal` + `libpython3.12-stdlib` | ≈ 30-40 MB |
    | `libglib2.0-0t64` + `libpixman-1-0` + `zlib1g` + `tini` | ≈ 15-20 MB |
    | the `qemu-system-sparc` binary (the host build is 38 MB) | ≈ 40 MB |
    | the controller, probe and UI | < 1 MB |
    | **final total** | **≈ 170-200 MB** |

  - **Not** `make install`. It would add the `pc-bios` firmware, keymaps
    and the other tools, ≥ 100 MB that leon3 with `-kernel` never uses.
    The final stage copies `build/qemu-system-sparc` alone.
  - `--no-install-recommends`, and no `python3-full`. The runtime libs
    are pinned to what `ldd` lists for the builder's binary. The builder
    has no `libcurl-dev` and no `libssh-dev`, so the binary does not pull
    curl or ssh (the host build links both).
  - **Pinned base images.** `ubuntu:24.04@sha256:<digest>` in both
    stages, so no surprise pulls, and the layer is shared between them
    (it is stored once). No `python:3.x` image (≈ 1 GB), no
    `-full`/`-dev` images in the final stage, and no second distro.
  - The Docker build always builds **the current tree** (`COPY` of the
    worktree), never a pre-built or downloaded binary. So the image
    always contains the D-I sparse flash (`21f898484f`). The binary guard
    (D-PLl) still runs in the container.
- **Build approval.** **Local image builds need the user's OK first.**
  PL-7 is written and reviewed, but its build gates run only once disk is
  freed or a build is approved. See PL-7.
- **Post-build hygiene** (`run-container.sh build` prints these and runs
  none of them without asking):
  - `docker builder prune -f` (drops the cached builder layers);
  - `docker image prune -f`;
  - the VHDX compaction note (`wsl --shutdown`, then `Optimize-VHD` or
    `diskpart compact vdisk` on the Docker Desktop and WSL VHDXs). This
    runs on Windows, by the user.
- **The flash on the volume** is sparse only (D-PLl). A fresh flash costs
  about 16 KB of real blocks (the device creates it with one
  `ftruncate`). A NOMINAL run with TM backups costs a few MB.
- **Running natively (no Docker) remains the default dev path.** It
  costs no image space at all, and the container is for when the disk
  allows it.

### D-PLt: what to avoid (survey §3.7)
- **No AGPL or Builder's-licence code inside the controller.** Yamcs is
  AGPLv3, and OpenC3 COSMOS moved to a Builder's licence (survey §1.7,
  §1.8). Nothing is copied from them; only ideas are borrowed (and cited).
- **No COSMOS or Yamcs as a hard dependency.** TM/TC stay clean CCSDS
  Space Packets at the relay and proxy, so either can be attached later
  as an optional client (TM fan-out, TC into the proxy).
- **No wall-clock pacing, no UI-only state, and no extra ad-hoc sockets
  per feature** (one API, one stream). No register writes through bus
  semantics for faults: hooks only, and the read-only state properties.

## Commits

Every commit is test-first: the test is seen red, then the code is
written.
- **The first series is PL-1..PL-7.**
- **PL-8 (truth vs TM) and PL-9 (the diagram)** are planned commits after
  the core. The API carries what they need from PL-3 on.
- **PL-10 (GDB attach)** needs only PL-3 (the verbs) and PL-6 (the
  button). It can land any time after PL-6.
- The roadmap items are not commits of this plan.

### PL-1: QEMU platform time: OBT source, one-shot and injected codes
- stixspw gains `timecode-source`, `obt-epoch`, `obt`, `timecode-once`
  and `timecode-inject` (D-PLb, D-PLc), plus traces.
- **qtests** (an appended block in `tests/qtest/stix.c`; the detector
  branch also appends there, so the merge is trivial):
  - `spw-obt`:
    - TIMECNT = OBT & 63 at the OBT rollover;
    - a guest SCET commit does not move the codes;
    - an `obt` set re-arms;
    - `obt` advances with `clock_step`;
    - in `scet` mode `obt` = the SCET, and a set is rejected;
    - the OBT survives a reset.
  - `spw-timecode-once`: exactly one code with `timecodes` off; a no-op
    when on; refused while `link-down`; an error with control.TR = 0; a
    pending `once` dropped by `link-down` and by a `timecodes` change.
  - `spw-timecode-inject`: the given TIMECNT now, through TR/TQ/IE and
    the latch; 64 rejected; refused while down; an error with TR = 0.
- Also in PL-1: the trace `stixspw_rx_deliver` gains the virtual time
  (for the D-PLd ordering check). Traces are not part of any golden.
- **Red** on `d2c6cae28a`: the properties are unknown.
- **Gates:** the full set, because this is C:
  - the suite and checkpatch;
  - the probe self-test;
  - boot-smoke (**golden unchanged, required**: the default is `scet`);
  - soak 300 s and tc-smoke;
  - nominal-smoke ± and fault-smoke ±;
  - the shared flash mtimes unchanged.

### PL-2: QEMU read-only state; QMP over TCP for the scripts
- stixctrl `hw-state` and stixspw `hw-state` + `vclock-ns` (D-PLo,
  D-PLr).
- The `stixctrl_hk_value` split into truth and the injection wrapper,
  which must give identical results.
- `run-qemu-gs.sh` `STIX_GS_QMP` accepts `tcp:127.0.0.1:N`. The probe's
  `QmpClient` takes `tcp:HOST:PORT`, and its self-test gains a TCP
  fake-QMP case.
- **qtests:**
  - `ctrl-hw-state`:
    - parses as JSON with schema 1;
    - the enables and POWERSTAT follow hkCmdSta writes;
    - `quarter_on` follows idefxConfig;
    - for an injected channel, `truth` ≠ `presented` and `truth` = the
      pre-injection value;
    - the protection fields follow a trip;
    - **reading it twice changes nothing:** two reads with no
      `clock_step` in between are equal, and no `*_io_read` trace fires
      during the getter (D-PLo);
    - the SCET UNSYNCED flag follows a guest 0xCC commit with bit 31.
  - `spw-hw-state`: LS and `link_down`; the time-code fields after a
    rollover.
  - `vclock-ns`: monotonic, and it advances exactly with `clock_step`.
- accepted-diffs: a new section headed `## P-1.` (non-numeric; it becomes
  §39+ at the merge, since §31-§38 are the detector's and `d2c6cae28a`
  already carries §31) for PL-1 + PL-2. It records that the default
  `scet` keeps §20.
- **Order inside PL-2:** the `stixctrl_hk_value` truth split is its own
  first, small commit (PL-2a), so that the detector's DET_C edits
  (D-4/D-5) rebase onto it. The probe's TCP `QmpClient` is a second
  isolated commit (PL-2b), coordinated with the detector worktree's
  pending probe change.
- **Gates:** the full set, as PL-1. `run-qemu-gs.sh` is checked with
  `sh -n`/`dash -n` and a stub for both QMP forms (as in P4-6).

### PL-3: controller core: API v1, SSE, JSONL, sim clock, power, state tree
- **The package** `scripts/stix/platform/stixplat/`:
  - `config.py`, `qemu.py` (the argv builder, `-S` spawn, the stderr
    trace reader), `qmp.py` (`QmpConnection`), `simclock.py` (D-PLr),
    `power.py` (D-PLf);
  - `state.py` (the tree, truth/presented from `hw-state`, change
    detection);
  - `log.py` (the D-PLn record, `log.jsonl`, the in-memory ring for
    SSE);
  - `dispatch.py` (verbs, arg schemas); `api.py` (`ThreadingHTTPServer`,
    REST, SSE, the D-PLj filter);
  - `probe_lib.py` (D-PLi).
- **The entry point** is `scripts/stix/platform/stixplat.py`.
- **Verbs:** `platform.power.*`, `sim.pause`/`sim.resume`, `log.mark`.
- **Endpoints:** `status`, `state`, `time`, `log`, `stream`, `verbs`.
- **`--self-test`** (offline, no QEMU):
  - the config;
  - the argv golden;
  - the run-dir guard and the sparse check;
  - **the binary guard** against two fake binaries:
    - a stub answering `qom-list-properties` without `inverted-storage`:
      refused, and no `flash.bin` is created;
    - one with it: accepted;
  - **retention** on a temp run root with synthetic run dirs:
    - only dirs with the marker are deleted;
    - `keep_runs`, `keep_failed_days`, `max_total_mb` and `min_free_mb`
      are honoured;
    - log rotation;
    - a dry run deletes nothing;
  - `QmpConnection` against a fake (events between replies, errors,
    timeout);
  - `SimClock` against a scripted `vclock-ns`: deadlines hold across a
    STOP/RESUME; no firing while paused; bench time is continuous across
    a scripted power cycle (the off-time from a fake wall clock) and
    across a warm `RESET`;
  - the run-dir preflight against a fake non-sparse filesystem;
  - the guest-reset policies (`warm`, `cold`, `halt`) against a fake QEMU
    emitting `RESET` or `SHUTDOWN`;
  - the power state machine against a fake QEMU;
  - the record schema (every field present, `seq` strictly increasing);
  - SSE framing and `Last-Event-ID` resume;
  - the D-PLj request filter;
  - `GET /api/v1/verbs` equals the docs table.
- **Live gate `tests/core-smoke.sh`** (curl only, no browser):
  - start interactive: frozen, `flash.bin` sparse;
  - an SSE client (`curl -N`) in the background collects records;
  - boot: the UART banner within 20 s (also as `uart` records). The boot
    report TM is checked in link-smoke (PL-4), because the TM relay is a
    PL-4 module;
  - pause for 10 s, then resume: `sim_time` advanced by less than 0.5 s
    across the pause;
  - power off (same inode), on, boot, cycle;
  - `timecodes = false` set while frozen survives `cont` (the time-code
    timer stays silent in the trace). This is a raw QMP check through the
    controller's QMP client; the `link-down` case moves to link-smoke,
    where `platform.spw.link` exists;
  - `sim_time` is continuous across the cycle, and `boot_id` increments;
  - SIGTERM: QEMU quits cleanly;
  - **the SSE records equal the `log.jsonl` lines** (same seqs, same
    bytes).
- **Existing gates:** the probe self-test only (no C change).

### PL-4: platform TCs, TM relay and decoder, TC proxy, forcing and the first faults
- **Modules:**
  - `tcbuild.py`, `timesync.py` (D-PLd), `keepalive.py` (D-PLe, sim
    paced);
  - `relay.py` (relay, `tm.jsonl`, fan-out, the TC proxy with the
    link-down policy);
  - `decode.py`: the EID names for the platform events, TM(1,x) with the
    FID, the heartbeat CUC, the SID 1/SID 2 mode and the TC20 count;
  - `faults.py` (the catalogue loader and the v1 entries, D-PLq);
  - `force.py`.
- **Verbs:** `platform.spw.link`, `platform.timecode.*`,
  `platform.obt.set`, `platform.timesync.send`, `platform.keepalive.set`,
  `force.*`, `fault.*`, `tc.platform.send`, `tc.gs.send`.
- **Self-test additions:**
  - the CRC reproduces every TC unit in fault-smoke, nominal-smoke and
    the tc-smoke TC(9,129) (`… FA 63`);
  - TC(20,128): 248 bytes, length 241, APID 0x31C, source 14, ACK 0, the
    8 Hz sequence count;
  - the D-PLd window logic against a scripted `obt`;
  - the decoder on the probe self-test's real units;
  - the catalogue's schema validation, and a fault inject/clear giving
    `fault` records with seeds.
- **Live gate `tests/link-smoke.sh`** (REST + SSE, a fake GS UDP
  listener):
  - **boot:** the boot report TM arrives through the relay (moved here
    from core-smoke);
  - **`link-down` set while frozen survives `cont`** (moved from
    core-smoke): no TM until `platform.spw.link up`;
  - **relay:** the fake GS gets byte-identical bare SPs, one per
    `tm.jsonl` line;
  - **proxy:** TC(17,1) from the fake GS gives TM(1,1), TM(17,2) and
    TM(1,7);
  - **keep-alive:**
    - on from the first TM: no 0x142E for 90 s;
    - paused for 40 s mid-run: still no 0x142E. This is a sanity check
      only: the FSW's timer freezes with the vclock too, so a wall-paced
      sender would also pass. The sim-pacing proof is the `SimClock`
      self-test;
    - off: 0x142E within 40 s;
  - **time sync** (`obt` mode, the default epoch): TM(1,7) within 3 s;
    then the first heartbeat's 48-bit CUC equals the OBT within
    0.25 s + latency, with bit 31 clear (D-PLd); the RX-delivery trace
    comes before the applying emit, and its TIMECNT = coarse & 63;
  - a second `timesync.send` while one is pending gets 409;
  - time sync with time codes off: TM(1,8) FID 48035 within 4 s;
  - `once`: exactly one code;
  - **SpW down:** the keep-alive is suppressed, a time sync gets 409, and
    a GS TC is dropped and logged;
  - `fault.inject spw.link.disconnect` gives the same TM as
    `platform.spw.link down`, but logged as `fault`.
- **Existing gates:** the probe self-test only.

### PL-5: the scenario runner (JSON, await/assert, variants) and `mode = "run"`
- **The scenario JSON** (survey §3.3; F´ await/assert):
  ```json
  {"api":1, "name":"spw-outage", "seed":1, "duration_s":100,
   "variants":["nominal","fault_no_recovery","fault_with_recovery"],
   "setup":[{"do":"platform.power.on"}],
   "steps":[
    {"at":0, "do":"platform.power.boot",
     "await_event":[{"eid":"14a3","timeout_s":50}],
     "assert_no_tm":["0x5A1:1:2","0x5A1:1:8"]},
    {"at":60, "do":"fault.inject", "args":{"id":"spw.link.disconnect"},
     "variants":["fault_*"],
     "assert_silence":{"after_s":2,"for_s":15},
     "await_state":[{"path":"platform.spw.link","eq":"down"}]},
    {"at":60, "variants":["fault_with_recovery"],
     "await_event":[{"eid":"5435","data":"48","timeout_s":35}]},
    {"at":80, "do":"fault.clear", "args":{"id":"spw.link.disconnect"},
     "variants":["fault_with_recovery"],
     "await_event":[{"eid":"142d","data":"00","timeout_s":10}]}],
   "teardown":[{"do":"platform.power.off"}]}
  ```
- **Primitives:**
  - `await_tm` (the probe grammar `APID:T:S[:hex]`);
  - `await_event` (EID + parameter prefix);
  - `await_uart` (regex);
  - `await_state` (path + predicate);
  - `assert_no_tm` / `assert_no_event` (a forbid that arms at its step);
  - `assert_silence {after_s, for_s}` (no TM unit at all in the window);
  - `assert_consistent` (path glob; it passes when the D-PLp status is
    `ok`, or `fault` with the expected fault). **It arrives in PL-8.**
    Until then it is a scenario error (exit 2), never a silent pass;
  - `expect_cuc`.
  - The predicates are `eq, ne, lt, gt, within(tol), in_set, changed,
    stable_for(dt)`.
- **Timeouts and `at`/`after` are sim time.** The TM matching reuses the
  probe's `Expect`/`Forbid` with the same semantics: unordered within the
  window, the forbid arming at its step.
- **Variants** follow the ESA FDIR test classes: nominal, fault without
  recovery, fault with recovery. Steps are filtered by a `variants`
  glob, and run mode runs each listed variant as its own run.
- **Verdicts:** each step is pass, fail, timeout or error, with the seq
  range as evidence. The reports and `summary.json` carry the log slice.
- **Verbs:** `scenario.run {file, variant}` and `scenario.abort` (also
  usable from the UI).
- **Gate scenarios** in `scripts/stix/platform/scenarios/`:
  - `gate-spw-link.json`, **derived from** the fault-smoke link-down rows
    (fault-smoke.sh stays the canonical C gate). It adds a 1 Hz
    keep-alive that fault-smoke does not have; there the boot-armed
    0x142E at about +29 s is tolerated. The rows:
    - 0x14A3 @50;
    - `fault_no_recovery`: at +60 down, then **TM silence** (`SPW_send`
      refuses while LS ≠ RUN, SPW.c:692-696, so the queued 0x5435 never
      leaves), `platform.spw.link = down`, and the keep-alive counts
      "suppressed";
    - `fault_with_recovery`: at +60 down; at +80 up → `5435 48` (queued,
      sent after the clear, within the 35 s window) and `142D 00` @10;
      no 0x142E for the 20 s outage;
    - `nominal`: no fault, no 0x5435, 0x142D or 0x142E;
  - `gate-keepalive.json`, including the NOMINAL-entry re-arm: enter
    NOMINAL (`tc.gs.send`), stop the keep-alive, and expect 0x142E 30 s
    after the later of the last TC(20,128) and the entry, then the move
    to SAFE;
  - `gate-timesync.json` (`obt` mode), variants `aligned` (the default
    epoch: the exact-time check passes after TC(9,129)) and `mismatch`
    (`obt_epoch & 63 = 32`: no 0x1429, a CUC far from the OBT before the
    TC(9,129), and exact after it);
  - `gate-timecode-loss.json` (optional, about 150 s: 0x542B @75, then
    0x1429 @10);
  - `gate-negative.json`: `142D` awaited before the clear (5 s). It must
    FAIL with exactly that expectation unmatched.
- **`tests/run-smoke.sh`:** run mode over the gate scenarios exits 0; the
  negative scenario alone exits 1 with exactly one unmatched await.
- **The runner self-test:** a scripted fake TM stream and a scripted sim
  clock (ordering, windows, forbids, variants, abort, a pause inside a
  timeout).
- **Existing gates:** the probe self-test.

### PL-6: the web UI (an API client only)
- **One file,** `scripts/stix/platform/ui/index.html`: plain HTML, CSS
  and JS, no build, no CDN, served at `/`.
- **Panels:**
  - power (on, boot, off, cycle, pause, resume), SpW, time codes (on,
    off, once, inject), time sync, keep-alive, OBT;
  - the faults and forcings, generated from `GET /api/v1/faults` and
    `describe()`;
  - scenarios;
  - live: the event log from the SSE stream (decoded TM(5,x), acks, mode,
    link), the UART tail (`uart` topic), the switch states and a state
    table from `/api/v1/state`.
- **Every control posts a `cmd/<verb>`.** Everything shown comes from
  `status`, `state` or the stream. Reloading the page loses nothing.
- **Gates (no browser):**
  - the self-test parses `index.html`: every `data-verb` exists in
    `GET /api/v1/verbs`;
  - the page contains no `localStorage` of state, and no external URL;
  - `core-smoke` and `link-smoke` re-run against the server that serves
    the page.
- **Optional (browser):** a manual checklist in the docs.

### PL-7: Dockerfile, sample configs, docs (**the image build is deferred until disk is freed or the build is approved**, D-PLu)
- **Without a build,** the commit is verified by:
  - `hadolint` if available, otherwise review only;
  - a check that the Dockerfile copies only `build/qemu-system-sparc`
    (not `make install`) and pins both base images by digest;
  - `run-container.sh` checked with `sh -n` and a `docker` stub that
    records the argv. The argv must show 127.0.0.1 publishing, the named
    volume and the read-only mounts;
  - the docs.
  - The Docker gates below run later, after approval, and their result is
    added to "As built".
- **`scripts/stix/platform/Dockerfile`**, multi-stage (the size budget is
  in D-PLu). The build context
  is the repo root; `Dockerfile.dockerignore` (BuildKit) excludes
  `build*/`, `.git`, `**/flash.bin` and the run dirs. **Not `*.bin`:**
  that would drop `pc-bios/*.bin`, and with `install_blobs` at its
  default the meson setup lists them (`install_data(blobs)`,
  pc-bios/meson.build:95-97), so configure fails on the missing files.
  - **Stage `build`** (`ubuntu:24.04`):
    - `build-essential ninja-build python3 python3-venv pkg-config
      libglib2.0-dev libpixman-1-dev flex bison git`;
    - `../configure --target-list=sparc-softmmu --disable-docs
      --disable-install-blobs --enable-werror`, then
      `make -j$(nproc) qemu-system-sparc` (no `install`, D-PLu), then
      `strip`;
    - the meson wraps download during configure, so the build needs
      network.
  - **Stage `run`** (`ubuntu:24.04`):
    - `python3-minimal` + `libpython3.12-stdlib` (3.12, with
      `tomllib`), `libglib2.0-0t64`, `libpixman-1-0`, `zlib1g`, `tini`,
      all `--no-install-recommends`;
    - `COPY --from=build /src/build/qemu-system-sparc /opt/qemu/bin/`,
      `scripts/stix/tmtc-probe.py` and `scripts/stix/platform/`;
    - a non-root user. The image creates `/var/lib/stix` owned by it,
      because a new named volume takes its ownership from the image
      directory;
    - `ENTRYPOINT ["tini","--","python3","/opt/stix/platform/stixplat.py"]`
      and `CMD ["--config","/config/platform.toml"]`.
  - **Not in the image:**
    - the FSW ELF (`/elf`, read-only);
    - the flash and the run data (the volume at `/var/lib/stix`, not
      `/run`, which is often a tmpfs);
    - the config (`/config`, read-only), user scenarios (`/scenarios`)
      and the IDB JSON (`/idb`, read-only, optional).
- **`run-container.sh`** is the only documented way to run it:
  ```
  docker run --rm --name stix-platform \
    -p 127.0.0.1:8080:8080 -p 127.0.0.1:21502:21502/udp \
    --add-host=host.docker.internal:host-gateway \
    -v stix-run:/var/lib/stix -v "$ELF_DIR":/elf:ro -v "$CONF_DIR":/config:ro \
    -v "$SCEN_DIR":/scenarios:ro [-v "$IDB_DIR":/idb:ro] stix-platform
  ```
  - **The flash volume:**
    - a **named volume**, or a Linux ext4/xfs bind mount, with sparse
      files and `FALLOC_FL_PUNCH_HOLE`;
    - **never `/mnt/c`**; the sparse check refuses it;
    - **persistent:** keep the volume with `fresh = false`. **Fresh:**
      `fresh = true`, or a new volume.
  - **The GS on the host:**
    - TM goes to `host.docker.internal:21501`. The GS binds its TM port
      on all interfaces (udp.py:99), so `host-gateway` reaches it on a
      Linux engine. On Docker Desktop the docs give the check;
    - the GS sends TC to 127.0.0.1:21502;
    - the GS config is unchanged.
  - `--network host` (Linux) is documented as an alternative.
- **Sample configs:** `interactive.toml` and `run-gates.toml`.
- **Docs:**
  - `docs/stix-port/platform.md`:
    - usage and the ownership table;
    - the API reference, generated from `/verbs`, with the record schema
      and the versioning rule;
    - the scenario and fault-catalogue references;
    - timing, security and troubleshooting.
  - A pointer in ground-station.md.
  - The accepted-diffs gate record.
  - This plan's "As built".
  - **API v1 is declared stable here** (D-PLn).
- **Gates (deferred until the build is approved):**
  - `docker build`, with the builder peak and the final size recorded
    (the final image must be ≤ 250 MB). Then `docker builder prune`;
  - the binary guard inside the container passes;
  - the container in run mode over the gate scenarios exits 0, and with
    the negative scenario exits 1;
  - the interactive container passes `core-smoke` on 127.0.0.1:8080;
  - `docker port` shows only 127.0.0.1;
  - `du flash.bin` is under 100 MB.

### PL-8 (after the core): truth vs telemetry
- **The decoder** for the D-PLp items: the item dictionary plus the IDB
  JSON (D-PLp).
- **Comparison** with tolerance and latency, `mismatch` records, leaf
  `status`, and a working `assert_consistent`.
- **Self-test:**
  - the dictionary resolves against the JSON;
  - a recorded real SID 2 unit decodes to the values TM_3.c packs (the
    offsets cross-checked);
  - the latency logic against a scripted HK stream.
- **Live gate** `gate-consistency.json`:
  - nominal SAFE: every item is `ok` after 2 SID 2 cycles (about 130 s);
  - force `instrument.hk_adc.24` = 1800: the item goes `pending`, then
    `fault` (a forced item is expected), never `mismatch`;
  - a deliberately wrong dictionary entry (a test fixture) gives a
    `mismatch` record.

### PL-9 (after PL-8): the instrument diagram
- An inline SVG in `index.html`:
  - IDPU, supplies, the SpW link, time codes, detector quarters, HK
    lines and the protection;
  - the attenuator later.
- Each element carries `data-path`, and its colour comes from the leaf
  `status` and value: on, off, fault, mismatch, stale.
- **No new API:** everything comes from `/api/v1/state` and the `state`/
  `mismatch` topics (D-PLo carries it from PL-3).
- **Gate:** the self-test checks that every `data-path` in the SVG exists
  in the state tree; plus the manual browser checklist.

### PL-10: GDB attach (after PL-6)

Based on the coordinator's spike of 2026-11-11 [M, live].

- **The mechanism:** QMP `human-monitor-command`
  `gdbserver tcp:127.0.0.1:2222` at run time, and `gdbserver none` to
  close it. Port 2222 is grmon's, so existing debugger configs can
  attach.
  - In the container the gdbstub binds `0.0.0.0:2222` inside, and
    `run-container.sh` publishes it **only** as `-p 127.0.0.1:2222:2222`
    (the D-PLj rule). Natively it binds 127.0.0.1.
- **Verified in the spike** with gdb-multiarch 15.1 (WSL):
  - attach;
  - `tbreak rtems_task_wake_after`;
  - `bt` into `Watchdog_task` (Watchdog.c:100);
  - a source listing;
  - detach, after which the VM keeps running.
- **Time while halted:**
  - a halt pauses the CPU and `QEMU_CLOCK_VIRTUAL`, and QEMU emits a
    `STOP` (runstate `debug`);
  - the STIX WDT model counts on the virtual clock, so a 55 s halt caused
    no reset. The maximum interval is 255 × 105 ms ≈ 27 s. Unlike the
    hardware, no watchdog-disabled build is needed;
  - the controller's bench-time timers (TC20, time sync, scenario
    timeouts) pause with it (D-PLr);
  - while the runstate is `debug`, `sim.resume` is refused (409), so the
    controller never resumes behind GDB's back.
- **The verbs:** `debug.attach {port=2222}` / `debug.detach`, plus a UI
  button. Localhost only. Each attach, detach, halt and resume is a
  `debug` log record.
- **The ELF** is config: the emulator runs any ELF. The ASW Makefile
  builds with `-Os`, so for clean stepping the user builds an `-O0` ELF
  and points the config at it. The docs say so.
- **Shipped debugger configs** (`scripts/stix/platform/debug/`):
  - **WSL gdb-multiarch:** a `gdbinit.sample` with `set architecture
    sparc`, `target remote localhost:2222`, and the source mapping
    `set substitute-path C:\\Users\\laszl\\Development\\stix_asw_src\\ASW/ASW /mnt/c/Users/laszl/Development/stix_asw_src/ASW/ASW`;
  - the mapping rules: double the backslashes, and end the prefix just
    before a `/`, because the DWARF paths mix separators. The path is an
    example; the user's prefix is a config value;
  - a **VS Code `launch.json`** (cppdbg, `miDebuggerPath` gdb-multiarch,
    `miDebuggerServerAddress localhost:2222`, the same `sourceFileMap`);
  - a **Visual Studio `launch.vs.json` QEMU variant** of the user's grmon
    config: `target remote localhost:2222`, no `monitor …` commands and
    no `load`. The Windows gdb is `ASW/RTEMS-4.10/bin/sparc-rtems-gdb.exe`
    (gdb 6.8, compatibility with today's gdbstub unverified [I]) or a
    modern Windows sparc gdb.
- **Self-test:**
  - the HMP strings for attach and detach;
  - the port is refused unless it binds 127.0.0.1 (natively);
  - `sim.resume` is refused in runstate `debug`;
  - the `debug` records.
- **Live gate `tests/debug-smoke.sh`.** It **skips** when
  `gdb-multiarch` is missing.
  - `debug.attach`;
  - `gdb-multiarch -batch` with `target remote :2222`,
    `tbreak rtems_task_wake_after`, `continue`, `bt` (expect a
    `Watchdog_task` frame or another task frame), a 40 s hold (above the
    27 s WDT maximum), `detach`;
  - then: no guest reset, `STOP`/`RESUME` records, heartbeats resume, and
    `sim_time` advanced by less than 1 s across the halt.
  - **Red:** the `debug.*` verbs are unknown before this commit.
- **Existing gates:** the probe self-test (no C change).

## Gates per commit (summary)

| Commit | New gates | Existing gates |
|---|---|---|
| PL-1 | qtests spw-obt, spw-timecode-once, spw-timecode-inject | **full set**; goldens unchanged; flash mtimes unchanged |
| PL-2 | qtests ctrl-hw-state, spw-hw-state, vclock-ns; probe self-test (TCP); `run-qemu-gs.sh` stub check | **full set**, as PL-1 |
| PL-3 | `stixplat.py --self-test` (including the binary guard and retention); core-smoke.sh | probe self-test |
| PL-4 | self-test (CRC, builders, timing, decoder, catalogue); link-smoke.sh | probe self-test |
| PL-5 | self-test (runner); run-smoke.sh (3 variants + negative) | probe self-test |
| PL-6 | self-test (UI verbs); smokes against the UI server | — |
| PL-7 | now: Dockerfile and `run-container.sh` static checks, argv stub; **deferred until approved (disk)**: docker build (final ≤ 250 MB, then prune), container run mode + negative, interactive smoke, port and sparse checks | — |
| PL-8 | self-test (dictionary, decode, latency); gate-consistency.json | — |
| PL-9 | self-test (SVG paths) | — |
| PL-10 | self-test (HMP strings, resume refusal, records); debug-smoke.sh (skips without gdb-multiarch) | probe self-test |

- **The impact on existing gates:**
  - Only PL-1 and PL-2 touch QEMU. The time-code additions are opt-in
    (default `scet`), and the state properties are read-only getters, so
    every golden, gate and qtest stays unchanged.
  - The `stixctrl_hk_value` split is a refactor, proven by the unchanged
    boot-smoke, nominal-smoke and fault-smoke results.
  - The probe gains only the TCP QMP form, and `run-qemu-gs.sh` keeps
    the unix form byte for byte.
- **Isolation:** the controller gates use `/tmp/stix-plat/<gate>/` and
  the 2152x/2153x ports (D-PLg). They never use 21000, 21500-21503 or
  the interactive 2151x ports.

## Risks and checks

- **Sim pacing jitter** (≤ 50 ms) and the D-PLd race on a starved host,
  or behind a queued GS TC. The FSW heals a 1 s miss; the exact-time
  check and the ordering trace catch it (`late_apply`).
  - A 30-minute sync soak is a candidate for "As built".
- **`link-down` set at `-S` [C/M]:** the reset order makes it survive
  (D-PLf). link-smoke checks it live. The fallback is to re-apply it
  right after `cont`.
- **`obt`-mode boot [I]:** with the default epoch the 6 LSBs match
  (D-PLb). In the `mismatch` variant, or with `obt_continuity = "exact"`
  after a cold cycle, the FSW ignores codes more than 4 s off until
  TC(9,129) (Timing.c:269-273). A shift trace once per second on the
  UART is noise, recorded as such.
- **TC execution blocks up to 2 s per TC(9,129) [C]:** a GS TC or a
  TC(20,128) right after it waits (TCBuffer.c:302; the queue holds 64).
  This is FSW behaviour, and it is documented.
- **The warm reset [I]:** `-action reboot=reset` with the `-kernel` boot
  stub must restart the FSW as the watchdog-reset path does. The first
  live watchdog or FORCE reset under the controller confirms it, and
  `cold` is the fallback.
- **The decoder [I]:**
  - the 0x0021 payload layout;
  - the 12-bit packing and bit offsets in the IDB JSON;
  - whether the HK values are raw;
  - the SID 1 cadence.
  - All are checked on live units before any await uses them.
- **The hw-state polling load:** 2 Hz × 2 small JSONs is negligible. A
  check that the getter has no side effects is part of the qtest.
- **Disk (D-PLu): about 19 GB really free on C:, and the VHDXs never
  shrink.**
  - **The image build:** a peak of ≈ 2-3.5 GB (builder plus cache). It
    is deferred until approved, then pruned. The final image is ≈ 200 MB.
  - **A pre-D-I QEMU** would write 16.5 GB on the first power-on. The
    binary guard refuses it, and the Dockerfile always builds the current
    tree.
  - **A non-sparse flash copy** (plain `cp`, `docker cp`, tar without
    `--sparse`) materialises 16.5 GB. The docs forbid it, the controller
    never copies, and the sparse check catches a non-sparse volume.
  - **Accumulating run dirs and logs:** retention (D-PLl), with a
    `min_free_mb` refusal.
  - **Measuring free space:** WSL `df` overstates it. The controller
    measures inside the run root, and the docs tell the user to check
    C: on Windows.
  - The Docker build also needs network (meson wraps).
- **Python:** `tomllib` needs ≥ 3.11 (the host has 3.12.3).
- **Merge with the detector branch:**
  - `trace-events`: the entries go in separate sections (stixspw vs
    stixdet), so the risk is low;
  - `grlib_stixctrl.c`: `hw-state` sits in one new function at the end;
    the detector's fields come through an accessor (D-PLm req. 8). The
    `stixctrl_hk_value` split lands first, as PL-2a, so that the
    detector's DET_C edits rebase onto it;
  - `tests/qtest/stix.c`: the platform qtests go in
    `tests/qtest/stix-platform.c`, or behind one
    `stix_platform_register()` call, so `main()` gets one line;
  - `scripts/stix/tmtc-probe.py`: the detector worktree has about 120
    uncommitted lines there. PL-2b (the TCP `QmpClient`) stays small and
    is coordinated with them;
  - the goldens: detector D-2 regenerated `qemu-stdout.golden.log`, and
    this branch never regenerates one. After the merge, boot-smoke,
    nominal-smoke and fault-smoke re-run on the merged tree;
  - accepted-diffs: the non-numeric `## P-n.` headings are renumbered
    to §39+ at the merge (`d2c6cae28a` already carries §31).
- **QEMU's single QMP client:** the ad-hoc `qom_set` snippets in
  ground-station.md do not work while the controller runs. The REST API
  replaces them, and the docs say so.

## Open questions for the user

1. **The TC(20,128) period for STIX.** Neither the FSW nor the ICD fixes
   it ("agreed frequency up to 8 Hz"; the timeout is fixed at 30 s,
   FDIR.c:574). Is 1 Hz right?
2. **The spacecraft switch-on order.** The proposed default: power on →
   link up and time codes on at once → boot → keep-alive from the first
   TM → TC(9,129) manual (or `timesync.autostart = "first_tm"`). Is this
   the real order?
3. **The OBT epoch.** Decided default (overridable): configurable, with
   0x10000000 so that `coarse & 63` matches at boot, aligned continuity
   across power-offs, and a `mismatch` scenario variant (D-PLb). Open: is
   a mission-epoch-derived value wanted instead?
4. **A guest reset.** Decided default (overridable): `warm`, in-process
   with BRAM kept; `cold` and `halt` remain options (D-PLf). Open: is
   warm right for every reset source, or should a FORCE reset behave
   differently?
5. **GS emulation** (D-PLa): are labelled `tc.gs.send` steps with source
   121 acceptable in scenarios?

## Extension roadmap (later phases, NOT in PL-1..PL-10)

Each item is a controller module (D-PLk) plus a model change. Faults
join the catalogue (D-PLq), and the state joins `hw-state` (D-PLo). None
changes the core.

### R-a0: DSS controls (early: once detector D-4/D-7 have landed)
- **Prerequisites:** D-4 and D-7 plus the D-PLm requirements 1-8.
- **Module `stim.dss`:**
  - the flare, the source and the rates;
  - `.dssevs` → STXEVT conversion (cached);
  - load, start with offset, stop, status;
  - state at `stimulus.dss.*`;
  - QL TM awaits (reusing detector-smoke's decoders where exposed).
- Size **S** (2-3 days). The risk is mostly the numbering (D-PLm).

### R-e: `.scmd` compiler into event tables (later)
- **The grammar** (notes-dss-app §2, kept local):
  - `det(sc,pix,amp)` and `wait N ns|us|ms|dt` (Δt 20 ns; a wait above
    2048 Δt splits into dummies; an event costs 1 Δt, a set-command
    2 Δt);
  - `auxtemp`, `setbaseline`, `settempadc1/2`, `setload`, `seu`,
    `setfalsetrig`;
  - `;` comments; case-insensitive; float arguments with range checks.
- **The output** is STXEVT:
  - `det` becomes kind 0 with
    `fpga_det = utils_oldSubcolIdToFpgaId[fe_sc − 1]` (D-PLm; the −1 is
    part of the rule);
  - the set-commands become kinds 1-7;
  - errors give the line number.
- **Self-test:** the sample-script outcome from the notes (3072 hits per
  quarter, `pix` 0..11 of 8 detectors, `adc` 500..531).
- **Feasibility:**
  - high for `det` and `wait`;
  - the set-commands need detector model knobs that do not exist yet,
    so they are reported as unsupported until then.
- Size: **S-M** (about 400 lines of Python plus tests, 3-4 days). Each
  model knob group is **M**.

### R-att: the attenuator as a breakable component
- **The FSW interface [C]:**
  - **The registers:** STIXCTRL `attnCmdSta` 0x5C and `attnConfig1` 0x60
    (StixIdpu.h:1639-1640). The fields (StixIdpu.h:696-730): COMMAND
    3:0, ERRORMASK 4, TIMEOUT 5, MOTOR1USED 6, MOTOR2USED 7,
    OPERATIONSTATUS 8, POSITIONBC 9, POSITIONAB 10, FAILFLAG 11.
    attnConfig1 holds the fast and slow ramp time quanta.
  - **The access:** BSW Attenuator.c:99-238 (command write, read-back of
    position AB/BC, busy, motor 1/2 used, timeout, fail flag).
  - **The check after each move:** AttCtrl.c:265 calls
    `FDIR_attCorrelationCheck` (FDIR.h:459-468, FDIR.c:1289ff). It
    compares the motors used with the command, and the positions with
    the command. It is gated by `fdir_autonomy_reaction_enabled` and the
    FDIR function mask `FDIR_ATT_CORRELATION_CHECK`.
  - **The errors:** `FSW_ERROR_ATT_UNPOWERED` 0x1601, `_BUSY` 0x1602,
    `_MOVEMENT` 0x1603, `_WRONG_MOTOR_USED` 0x1610,
    `_WRONG_POSITIONS` 0x1620, `_WRONG_MOTOR_AND_POSITION` 0x1630, and
    `_FDIR_SKIPPED` 0x1600 (commonFSW.h:75-82).
  - **The HK:** ATT_C is ADC channel 8 and ATT_V channel 12 (the model
    table, grlib_stixctrl.c:410-418). ATT_T is `E_HK_ATT_T`, HK index
    38 (HKData.h:196; the ADC channel is to be confirmed [I]).
  - **The current sum:** AttCtrl.c:342 sums only motion values below
    0x100, so a move needs an ATT_C of 110..255 (grlib_stixctrl.c:410-415
    comment).
  - **SID 2** reports the ATT M1/M2 moving bits, the AB "open" and BC
    "closed" flags, the motion count and the current sum [G].
- **The model** (new, in stixctrl, `det`-style split into
  `stix_att.c`):
  - the motor state machine: command → per-motor run time from the ramp
    quanta → the position sensors update → OPERATIONSTATUS clears;
  - ATT_C during motion; the TIMEOUT and FAILFLAG semantics;
  - ATT_V follows LV;
  - `hw-state.attenuator {truth: position, motors; presented: sensors,
    flags}`.
- **Faults:**
  - `attenuator.sensor_ab.stuck_open|stuck_closed`;
  - `attenuator.sensor_bc.stuck_*`;
  - `attenuator.motor1.dead` (no motion, no current);
  - `attenuator.motor2.dead`;
  - `attenuator.move.slow` (factor), which gives TIMEOUT;
  - `attenuator.current.zero` (motion with no current).
  - Each one changes `presented` only, and its expected detection is
    the correlation-check outcome [I: which TM carries 0x1620 and
    0x1630 is to be read in AttCtrl.c / TM_5].
- **Prerequisites:** the register semantics from the HW/SW ICD (notes
  in `notes-fdir-idpu.md` and `notes-hswicd-dataflow.md`), and a TC path
  that moves the attenuator (rate control in NOMINAL, or a manual TC).
- Size **M-L** (1.5-2 weeks: the model, qtests, gate scenarios). Risk:
  medium. The timing and register semantics are document-based, and the
  released docs may differ from the product.

### Other breakable components (the same treatment)
- **HV supplies** (HV01-16, HV17-32):
  - truth from hkCmdSta HV enables;
  - presented on ch 20/16 and POWERSTAT;
  - faults: `hv.<n>.no_output`, `.overvoltage`, `.slow_ramp`,
    `.trip` (with the latched power-cut option, POWERSTAT `blockade`,
    §25/§30);
  - the FSW checks HKMonitor.c:357-370, and depolarisation is in SID 2.
  - **S-M.**
- **A detector quarter:**
  - truth from idefxConfig;
  - faults: `det.quarter<q>.no_power` (DET_C and the counts stay off),
    `.overcurrent` (DET_C above 546, giving 0x9434), `.seu` (QuarterSeu);
  - it depends on the detector plan's quarter gating.
  - **S** after D-4.
- **Thermistors** (DET_Qx_T ch 32-35, PSU_T 40, ATT_T, the ASP TIMs):
  - the Basilisk taxonomy on the presented value (survey §3.4): bias,
    scale, `noise(σ, seed)`, `drift(rate)`, stuck, open (4095) and short
    (0);
  - noise and drift live model-side, on the virtual clock, as a small
    per-channel transform in `stixctrl_hk_value`'s presented wrapper.
  - **S.**
- **The watchdog** (modelled, §10):
  - `wdt.fire_now`, `wdt.disabled` (it never fires), `wdt.fast`;
  - the reaction is a guest reset, handled by `on_guest_reset`.
  - **S.**
- **Flash/NAND:**
  - program and erase failures on a given block (STATUS.FAILED, §7),
    bad-block markers, and bit flips (R-b);
  - the FSW: BCH correction, file rewrite and 0x543B (R-b).
  - **S-M.**
- **The SpW link, deeper** (a tap): drop, corrupt or delay single
  packets, credit errors, other LS states (ErrorReset, …, which FDIR.c:711
  does not count, §29), and port failover (PO/AP/NP). **M.**
- **The aspect system:** today only its HK channels are modelled (ASP_*
  in the table, grlib_stixctrl.c:437-450). The photodiode and VSENS
  faults are possible as presented-value faults (**S**). A full aspect
  model is **L** and out of scope.
- **EDAC and memory** (R-a) and **flash SEU** (R-b) follow.

### R-a: SEU/DEU injection in RAM, SRAM and BRAM (EDAC)
- **The model today [M]:**
  - no EDAC anywhere. The SDRAM controller only sets the INIT bit
    (grlib_stixsdram.c);
  - rotbCmdSta SECCOUNT and DEDETECTED read 0 (grlib_stixctrl.c:271-275);
  - the CTRL EDAC IRQ is wired to IRQMP 6 (leon3.c:476-477) but never
    raised;
  - there is no FTMCTRL PROM/EEPROM EDAC.
- **The FSW [C]:**
  - four modules: EEPROM, SDRAM (FTSDCTRL16 `edac`), CACHE = SRAM
    (cacheCmdSta) and ROTBUF (rotbCmdSta) (BSW EDAC.c:115-300);
  - every `EDAC_SEU_REPORT_PERIOD_S` (default 1000 s,
    ParamMgmtDef.h:195) the FSW reads and resets the SEC counts and sends
    TM(5,2) **0x542F** with four counts (EDAC.c:140-190);
  - scrub-rate changes give TM(5,1) **0x0030** (EDAC.c:193-280);
  - an SRAM or ROTB double error raises the IRQ, then
    `EDAC_callback` counts it for HK and flags the ROTB DEU
    (EDAC.c:110-134);
  - **an SDRAM double error is an AHB error, so a trap and a reboot**
    (EDAC.c:128-131).
- **Model changes:**
  1. SEC counters and DE flags per module, driven by catalogue faults
     (`edac.<module>.sec`, `.deu`), with the IRQ for CACHE and ROTB.
     With EDAC on a single error never changes data, so a counter is
     faithful (TEMU semantics, survey §3.4);
  2. an SDRAM DEU as a poisoned word: the next read is an AHB error, and
     a write cures it (a tiny MMIO overlay);
  3. optionally, a plain content change with EDAC off (QEMU has no QMP
     physical-memory write, so this needs a model property).
- **Gates:**
  - 0x542F after a shortened report period;
  - the HK DE counters;
  - an SDRAM DEU leading to a reboot.
- Size: **S-M** (counters + IRQ); **M** (the SDRAM trap). Risk: medium,
  because of the LEON3 AHB-error trap details [I].

### R-b: SEU/DEU injection in flash
- **The FSW [C]:**
  - 4096 + 128-byte pages, 64 per block, 16.5 GB in total
    (FilesystemDefinition.h:15-37);
  - **software BCH** (MemoryMgmt/BCH.c; the ECC sub-blocks in BSW
    File.c:361/506/659);
  - a corrected read traces "BCH corrected N SEUs"
    (MemFilesystem.c:604-607);
  - at `FS_REWRITE_FILES_AFTER_N_SEUS` (default 4, ParamMgmtDef.h:538)
    the file is rewritten (MemFilesystem.c:608-615);
  - an uncorrectable read gives TM(5,2) **0x543B** with the page address
    (MemFilesystem.c:589-591, TMForm.c:689-692).
- **Model changes:**
  - none for offline flips (power off, the §8 address map, the §9
    inversion);
  - a `flash-flip` property for live flips, applied only while paused,
    because of the stdio buffering.
- Size: **S** offline, **S-M** live. Risk: low.

### R-c: flash browser (read-only view)
- **It needs:**
  - the §8/§9 address map;
  - the page header and the filename (type, 6-byte SCET, duration;
    FilesystemDefinition.h:58-91);
  - the file types (:80-91) and block states (:121-128);
  - the control-block intervals (MemFilesystem.c:265-275);
  - the exact header from BSW File.c (to be read before coding).
- **The module `view.flash`:**
  - the catalogue and the file list;
  - hex pages; decoded context and LUT where the formats are known;
  - `O_RDONLY` reads, labelled "may be torn" while QEMU runs.
- Size **M**. Risk: format details [I]; it is safe because it is
  read-only.

### R-t, R-r: time control and record/replay
- **R-t:**
  - `stop-at-vclock-ns` (run-to-T and stepping, about 40 lines of C);
  - `-icount` speed control (`realtime|max|x<factor>`) with a
    measured-ratio readout;
  - single-stepping.
  - **S-M.**
- **R-r1:** replay a run from `log.jsonl` (re-issue the commands at their
  `sim_time`, then diff the outputs) and a UI "load a log" view. **S.**
- **R-r2:** `-icount rr` (needs SpW on a chardev and the flash on the
  block layer). **L.**
- **R-r3:** savevm/loadvm checkpoints (needs vmstate in stixctrl,
  stixspw and stixflash, and a snapshot-capable flash). **M-L.**

### R-d: smaller items
- `obt-drift-ppb` (tests the median adjustment, Timing.c:296-326). **S.**
- Malformed platform TCs (bad CRC, length or source) via
  `tc.platform.send`. **XS.**
- Other IDPU on (StixStatus bit 6 + irq_other → 0x944A and SAFE). **S.**
- TC(20,128) content (the STIX 20-octet block, ICD Table 69) for
  inter-instrument tests. **XS** (the FSW ignores it today).

## Findings for the FSW team

These are defect candidates, found by reading the code during this plan's
review. They do not change the controller design. The coordinator
reports them to the user.

1. **The per-source TC SCC is lost for every PID-90 TC.**
   `TMForm_sendExecutionCompletedReport` calls
   `TCData_storeSuccessfulTcSCC(in_tcSummary.packetId, …)` (TMForm.c:118).
   `packetId` is the 16-bit CCSDS packet ID (`uint16_t`,
   TMForm_ExecutionSummary.h:7; `TMFORM_TC_PACKET_ID`, TMForm.h:24), but
   the parameter is `uint8_t in_PID` (TCData.c:195). 0x1DAC truncates to
   0xAC, which never equals `PID_90`, so every completed TC stores its SCC
   in `SSC_for_other`. The ten `SSC_for_PID90_CAT12_*` parameters
   (ParamMgmtDef.h:498-507) are never written. TC(20,128) is unaffected:
   it passes the real `processId` (TcService20.c:46-49). The likely fix is
   to pass the PID (`(packetId >> 4) & 0x7F`) and the category.
2. **FID 48034 is unreachable** (informational). All TCs execute in the
   single TCBuffer task (TCBuffer.c:302), and `Timing_handleTC9` blocks
   in it, so `activeTC9coarse` is always 0 when the next TC(9,129) runs
   (Timing.c:160-166). A second TC(9,129) therefore applies its coarse
   late instead of being rejected.

## Review resolutions

The 2026-11-11 validation (an independent read of the FSW, the ICD, the
model, the GS and the detector branch; no build or run) is folded into
the body. The coordinator's decisions of the same day are recorded as
defaults that the user can override.

| # | Finding | Resolution (where) |
|---|---|---|
| R1 | NOMINAL entry re-arms the TC20 timer (OpModeMgmt.c:565-566) | Key facts TC(20,128); `gate-keepalive.json` (PL-5) |
| R2 | The per-source SCC is dead for PID 90 (TMForm.c:118 truncation); TM SSC is per (APID, destination); 121 = FSW SpareGS0 | Key facts Source IDs; D-PLa; Findings 1 |
| R3 | TCs run serially: 48034 unreachable, a second TC(9,129) applies late | Key facts TC(9,129); D-PLd step 5 (409 while pending); Findings 2 |
| R4 | The ±1 s CUC check hides a late apply; TM(1,7) needs ACK 0x08 | D-PLd (exact 48-bit check, ordering trace, ACK 0x9); PL-1 RX trace; PL-4 |
| R5 | `sim_time` undefined across power cycles and while off | **Decision 2:** bench time + `boot_id` + `vclock_ns` in every record (D-PLn, D-PLr, D-PLe) |
| R6 | A cold restart on a guest reset loses BRAM warm-reset state (§13) | **Decision 1:** `on_guest_reset = "warm"` default, `cold`/`halt` options, always logged (D-PLf) |
| R7 | PL-3/PL-5 gates use PL-4/PL-8 features | PL-3 core-smoke trimmed; TM and `link-down` checks moved to PL-4; `assert_consistent` is an error until PL-8 |
| R8 | `fault_no_recovery` cannot see the queued 0x5435 | PL-5 example and `gate-spw-link.json`: TM silence + state; 0x5435 only with recovery; "derived from" fault-smoke |
| R9 | The flash guard ran after the damage | D-PLl run-dir preflight (sparse + punch-hole probe, non-sparse `flash.bin` refused); the binary guard was already in the body |
| R10 | `*.bin` dockerignore breaks configure; no approval gate | PL-7 (`**/flash.bin`, `--disable-install-blobs`, `strip`); **Decision 3:** the build stays behind user approval; D-PLu budget, about 19 GB free, permanent VHDX growth |
| R11 | Port contradictions; 21500 and 21000 clash | D-PLg (`tmtc-server=off`, interactive 2151x and 21502 on purpose, gates 2152x/2153x); gate isolation |
| R12 | The guard refuses `/tmp/stix-plat-run` | D-PLl: gates use `/tmp/stix-plat/<gate>/` |
| R13 | `tc.platform.send` was an unrestricted console; `gs_tc` vs `tc.gs.send` | **Decision 4:** services 9 and 20 only, in every mode (D-PLa, D-PLn); one verb name `tc.gs.send` |
| R14 | The probe has no CRC | D-PLi: `tcbuild.py` CRC-16/CCITT-FALSE, checked against `FA 63` |
| R15 | DSS numbering: `fe_sc − 1`, utils.c:129-133, STXEVT baseline and 0xD kinds, one offset property | D-PLm, R-e |
| R16 | Merge-risk list incomplete | Risks "Merge with the detector branch"; PL-2a/PL-2b ordering; `## P-n.` headings |
| — | OBT epoch alignment | **Decision 5:** configurable, aligned by default, `mismatch` variant (D-PLb, `gate-timesync.json`) |
| — | The SCC truncation | **Decision 6:** "Findings for the FSW team", no design change |
| — | GDB attach (the coordinator's spike) | New **PL-10** |
| rec | The keep-alive pause test does not discriminate pacing | PL-4: relabelled a sanity check |
| rec | `-icount` jitter; QMP serial-channel RTT | D-PLr |
| rec | `hw-state` UNSYNCED flag; the per-channel conversion store; the read-only proof | D-PLo; PL-2 qtests |
| rec | `timecode-once`/`-inject` with TR = 0, a pending once | D-PLc; PL-1 qtests |
| rec | The kind → topic map, namespaces, `[::1]` in the Host filter | D-PLn; D-PLj |
| rec | Docker GS TM path, volume ownership, `/var/lib/stix` | PL-7 |
| rec | The GS sees acks for platform TCs | D-PLa (documented; optional filter, off) |
| rec | Operator TCs in hooks and fault preconditions | D-PLa: `tc.gs.send` only, the catalogue declares them |
| rec | `subcol` scope; STXEVT tick binning | D-PLm |
| rec | The SID 2 cadence (512, ParamMgmtDef.h:205) | Left as [I] in D-PLp until the unit is read in HKReport |

**Confirmed in the review (no change needed):**
- the TC(20,128) timeout, reaction, APID, exact length, any-mode
  applicability and the SID 2 count;
- the TC(9,129) APID, length, 48041, the 2 s wait, 48035, the 6-LSB match
  and its thresholds, and 0x542B/0x1429;
- the model's time-code generator, TR/TQ/IE with the D-M latch, the
  runtime properties, the reset behaviour, UDP TC bound to 127.0.0.1, and
  no vmstate;
- the `-S` reset order;
- that the `scet` default is golden-neutral (the golden is the UART log);
- the feasibility of the pure getters and of 20 Hz polling;
- the record schema, SSE with stdlib, and `tomllib` on 3.12;
- detector consistency on `dss_det` = `fpga_det`, the guard ring, the
  ELUT path and the scene knobs at `-S`.

## As built

- **PL-1..PL-3** are on `stix-uprev-platform`. accepted-diffs P-1 holds
  the C-side record.
- **PL-4** is on `stix-uprev-platform` (no C change). Deviations:
  - `decode.py` decodes the HK TM(3,25) SID and mode only. **The SID 2
    TC(20,128) count is left to PL-8's IDB-based decoder**: in the FSW's
    SID 2 layout it sits after many variable-length fields, so a
    hand-written offset would be fragile.
  - `tc.platform.send` takes services 9 and 20 from source 14 only (403
    otherwise), and a TC(9,129) gets 409 while a time sync is pending
    (review R2), the same as a second `platform.timesync.send`.
  - The fake QEMU stores the platform `qom-set`s and returns them
    (`x-fake-set-log`), so the self-test checks the QMP side of faults,
    forcing and the warm-reset re-apply.
  - link-smoke checks "link-down set while frozen survives cont" in a
    second boot at its end, so the first boot's boot report is checked
    with the link up.
- **TODO (on the rebase onto gate-ports):** move
  `scripts/stix/platform/tests/core-smoke.sh` and `link-smoke.sh` to the
  gate slot scheme (`scripts/stix/lib/slot.sh`: a slot run dir, slot
  ports, the shared slot lock) instead of their fixed ports (core-smoke
  21520-21522; link-smoke 21525-21528 and the fake GS on 21531) and
  `/tmp/stix-plat/core-smoke`, `/tmp/stix-plat/link-smoke`. gate-ports is not merged yet (PL-3 review
  S7).
