# Detector emulation plan: tiers 0-3 (FSM/QL, IDeF-X, synthetic counts, ROTB science) plus DSS replay

Status: draft, 2026-11-11, uncommitted. Validated 2026-11-11 (see
"Validation" at the end); every validator item and the user decisions of
2026-11-11 are folded into the body. A second pass (2026-11-11) folded
the TM/TC ICD extraction (`notes-tmtc-icd-detector.md`) and the ELUT
decision. Base: track worktree
`~/dev/qemu-stix-tc` (branch `stix-uprev-tc`, HEAD `a83fe9a3a5`, Phase 4
done). The user chose "detector emulation, tiers 0-3" as the next phase,
and then added the DSS `.dssevs` replay to it (D-7).

Sources:
- the model (`hw/sparc/grlib_stixctrl.c`, `StixIdpu.h`, `leon3.c`,
  `tests/qtest/stix.c`, `scripts/stix/*.sh`,
  `scripts/stix/baseline/capture-golden.sh`);
- a read-only pass over the FSW, `~/dev/STIX-FSW/ASW`. Paths are relative
  to it: **BSW** = `BSW_ASW/src/BSW/...`, **ASW** = `ASW/src/...`. The
  FSW team's own TSIM I/O models are in `~/dev/STIX-FSW/TSIM_IO/ASW_UT/src_io/iom/`;
- the private doc notes in `~/dev/stix-doc-notes/` (from confidential
  documents; the facts are used here, and nothing is quoted at length):
  `notes-hswicd-dataflow.md` (HSWICD + TN-0041), `notes-fpga-arch-verif.md`
  (RP-0067 + RP-0119), `notes-dss-idefx-caliste.md` (DSS UM, IDeF-X DS,
  Caliste DD/TN), `notes-dss-app.md` (the DSS PC application),
  `notes-dssevs-fakeflare.md` (the decoded user `.dssevs` file) and
  `notes-tmtc-icd-detector.md` (the TM/TC ICD STIX-ICD-0812 I4R8 draft,
  cross-checked against the FSW; cited here as **ICD:line**, body table
  numbers, which run one higher than the ICD's TOC);
- the gate artefacts: `docs/stix-port/baseline/*.golden.log` and earlier
  nominal-smoke run directories.

Fact tags:
- **[C]** read in code (FSW or model), with file:line;
- **[D]** a document says it. The documents are released issues and may
  miss late changes; where code disagrees, code wins (see "Doc vs code");
- **[G]** seen in a gate artefact (golden trace, run log);
- **[V]** decoded from the user's data file (notes-dssevs-fakeflare);
- **[L]** to be verified live before it becomes a gate expectation;
- **[I]** inferred;
- **[A]** an assumption the user accepted (2026-11-11). Nothing is gated
  on an [A].

## Goal

The FSW's detector paths run for real in NOMINAL:
1. **Tier 0.** The readout FSM obeys its command strobes, fsmSta is live,
   the QL accumulator bank switches every (QlIntTime+1)×100 ms (4 s with
   the FSW default), and IRQ 3 fires. The FSW's QL chain (QL task, RCR,
   light curves, background, variance, spectra, flare detection) runs.
2. **Tier 1.** An IDeF-X register store (32 ASICs × 12 registers) behind
   the serial command interface: CmdReady, writes, read-back with short
   registers left-aligned (`value << (8-len)`). The 384 NOMINAL-entry
   timeouts disappear, and the periodic ASIC verification passes.
3. **Tier 2.** A synthetic "sky": count rates per `fpga_det`, `pix` and
   `ebin`, with a runtime **flare switch** over QMP (`/machine/stixctrl`,
   like the Phase 4 injection properties).
4. **Tier 3.** Nominal (EACC/ETRG) integrations with the FPGA's swap rule,
   and the 49-block science chunks they put into the ROTB, which the FSW
   compacts into its archive.
5. **Replay (D-7).** The user's DSS `.dssevs` file can replace the
   synthetic sky, from a pre-scanned event table, started at NOMINAL entry
   or by QMP with an offset.

A new gate, `scripts/stix/detector-smoke.sh`, proves QL TM appears, that
its counts follow the flare switch, and that a count deposited at a known
`fpga_det` shows up at the right `subcol` in the TM (the index-space
end-to-end check). Its optional `--replay` mode is the one replay gate.

## Corrections to the brief

- **The boot UART golden shows no detector timeouts.**
  `docs/stix-port/baseline/uart.golden.log` (48 lines) has only the
  "init:" lines and four `LUTMgmt could not find LUT` lines [G].
  - The boot does time out on the detectors, but silently.
    `OpModeMgmt.c:1056-1064` [C] calls `Detectors_resetReg(grp, 2)` for
    all 16 groups. Each call polls ideFxCmdSta 1000 times and returns
    `E_RET_TIMEOUT` (`Detectors.c:627-639, 677-689` [C]), and nobody logs
    it. The trace golden `qemu-stdout.golden.log` shows exactly 16000
    `Read requested: addr = 0x0, regval = 0x0` lines [G].
  - **The 384 timeouts are a NOMINAL-entry effect.** They are the
    `XrayDetFDIR.c:731` lines in nominal-smoke's UART [G], from
    `XrayDetFDIR_resetASICregistersToDefault` (`OpModeMgmt.c:576` [C]).
  - **So the boot UART golden is expected not to change in any commit of
    this plan.**
- **QL TM is APID 0x5DC, not a dedicated QL APID.** It is TM(21,6) with
  PID 93 / category 12 = 0x5DC (`TM_21.c:772-800`, `TMData.h:63-67,
  445-447` [C]). The SSID is the first application-data byte
  (`TM_21.c:333` [C]): 30 light curves, 31 background, 32 spectra,
  33 variance, 34 flare flag (`TMData.h:625-629` [C]).
- **QL TM is not periodic.** Each QL module sends only when its packet
  is full, or when NOMINAL is left (`QLAccuDblBuffs_deinit`,
  `QLAccuDblBuffs.c:117-125`, called from `OpModeMgmt.c:171` and `:300`
  [C]). The flush sends only modules with data (`TMForm.c:720-760` [C]).
  Light curves fill at 7 B per 4 s, so a full packet takes about 39 min
  (`QLLightCurve.c:205-214` [C], `TMData.h:554-555` [C]).
  **A gate must leave NOMINAL to see QL TM.** The NOMINAL→CONF flush is
  unconditional (`OpModeMgmt.c:282-306` [C]).

## Index spaces

The Caliste → IDeF-X → FPGA → ASW → TM chain renumbers detectors and pixels
several times, and it is not 1:1 at every step. **Every property,
variable, trace argument and table in this plan and in the code carries
its index space in its name**, using the names below. A bare "det",
"pixel" or "channel" is not allowed.

| Name | Space | Range | Defined in (FSW) [C] | Used for |
|---|---|---|---|---|
| `asic_ch` | IDeF-X channel | 0-31 | `commonFSW.h:149`; R3 6-bit fields in SW order `utils.c:521-555`; SW↔wire order `utils_reorderThreshold3` `utils.c:663-677` (self-inverse) | R3 thresholds, R12 ALIMON, R4 |
| `pix` | Caliste pixel J | 0-11 (12 = guard ring, never accumulated) | `utils_pixelId2channelId` `utils.c:50-53`, inverse `utils.c:56-62`, `MASK_OF_PIXEL_CHANNELS` `utils.h:10-21` | LACC/EACC dimension 0 (`Detectors.h:39`) |
| `fpga_det` | FPGA detector | 0-31 = 2·`grp` + `asic` | `dete_data` `utils.c:74-107`, `UTILS_GROUPID2SUBCOLID` `utils.h:125-132` | LACC/EACC dimension 1 (`Detectors.h:68-79`), ROTB chunk payload order, **all FPGA register detector masks** (SUMDMASK, VARDMASK, fsmCalMask) |
| `grp` | IDeF-X group | 0-15 = `fpga_det`/2 | `StixIdpu.h` IDEFXCONFIG_ENABLE_V_QxGy; `dete_data` | GrpAddr, WriteMask, ReadMode, Enable bits, LTRG/ETRG index |
| `quarter` | power quarter | 0-3 = `grp`/4 | `dete_data` quarterId | idefxConfig Q1..Q4ON, QuarterSeu |
| `asic` | ASIC in a group | 0/1 (2 = both, "All") | `0xBFD30\|asic<<6` `Detectors.c:747, 806, 868` | slow-control addressing |
| `subcol` | ASW sub-collimator = ICD "detector index" (number − 1); `subcol` 8 = CFL, 9 = BKG (ICD Table 32, ICD:3505-3678, ICD:3557-3561 [D=C]) | 0-31 (ICD detector number 1-32) | `utils_subcolIdFromFpgaId` / `utils_fpgaIdFromSubcolId` `utils.c:113-127`; `utils_subcolMask2fpgaMask` `utils.c:222-232` | every ASW parameter mask, QL TM detector index (`QLAccuDblBuffs.c:175`), archive control word (`RotAccuCompactor.c:300-315`) |
| `fe_sc` | front-end ("old") sub-collimator | 1-32 | `Detectors_fpgaFromFrontendMinusOne` `Detectors.c:536-540` = `utils_oldSubcolIdToFpgaId` `utils.c:129-133`; inverse `Detectors_frontendFromFpga` `Detectors.c:528-534` | calibration report; the DSS app's "sc" |
| `dss_det` | DSS event detector field | 0-31 | DSS app table [D, notes-dss-app §3] | `.dssevs` bits 15:11 |
| `ebin` | science energy bin | 0-31 | LACC dimension 2 (ELUT output); LC bands mask 0x10821082 = `ebin` 1-6, 7-11, 12-16, 17-22, 23-27 (`ParamMgmtDef.h:254`) | QL TM, SUMEMASK/VAREMASK bits |
| `adc` | raw 12-bit ADC value | 0-4095 | ICD §7.2.11.2 (ICD:19596) | `.dssevs` events, IDeF-X ADC answers, ELUT input |

The ICD agrees with the FSW on `quarter` (Table 31, ICD:3490-3508), `grp`
(Table 33, ICD:3681-3784, all 16 pairs), `pix` (Table 34, ICD:3786-3840)
and `asic_ch` (Table 35, ICD:3844-3944) [D=C]. The ICD never mentions
`fpga_det`: the one non-1:1 step that TM users never see is `subcol` ↔
`fpga_det` (e.g. `subcol` 2 = `fpga_det` 14, 5 = 2, 8 = 13, 9 = 10).
In TM and TC, every detector mask, the SSID 32 detector index, the
L0/L1 sample detector field and the EID detector parameters are `subcol`.

**Key identities and differences:**
- **`dss_det` = `fpga_det` (identity).** The DSS app's `fe_sc`→`dss_det`
  table (2,3,14,15,0,4,5,12,13,8,1,6,7,10,11,9, 24,26,27,22,23,16,25,28,
  29,20,21,17,30,31,18,19) is entry-for-entry equal to
  `Detectors_fpgaFromFrontendMinusOne` (`Detectors.c:536-540`) and
  `utils_oldSubcolIdToFpgaId` (`utils.c:129-133`) [C]. So a `.dssevs`
  event's detector field is already the FPGA detector index, and the DSS
  "sc" is `fe_sc`. Replay uses `fpga_det = dss_det` with no lookup.
  (notes-dss-app §6 item 3 maps script ids through
  `utils_fpgaIdFromSubcolId`, the new numbering; that contradicts the
  table equality, and the code wins: Doc vs code 13.)
- **`pix` → `asic_ch`:** `pix` 0..11 = `asic_ch` 26, 15, 8, 1, 29, 18, 5,
  0, 30, 21, 11, 3; guard ring `pix` 12 = `asic_ch` 31 (`utils.c:50-53`
  [C]; RP-0019 agrees entry for entry [D]).
- **R3 threshold of `asic_ch` c** in the model's raw wire-order store =
  `utils_getReg3(c, utils_reorderThreshold3(raw))`, which mirrors the FSW
  exactly [C]. Pixel gating is derived, not inferred.
- **Pixel-mask bit orders differ between FSW users:**
  - QL software masks: bit i = `pix` i (`QLLightCurve.c:289`,
    `QLSpectra.c:271`, `QLFlareDet.c:109` [C]);
  - **RCR masks are reversed:** bit 11−i = `pix` i, bit 12 = attenuator
    (`RateCtrl.c:711-719` [C]);
  - the FPGA-side order of VARPMASK, SUMEMASK, VAREMASK and of R12
    ALIMON / R4 per `asic_ch` is not defined in the code. The model
    assumes **bit i = `pix` i / `ebin` i / `asic_ch` i** [A]. All defaults
    are all ones (0xFFF, 0xFFE, 0x7FFE0000, 0xFFFFFFFF), which hides the
    order; nothing is gated on it (Doc vs code 12).
- **ASW masks are `subcol`-indexed; FPGA masks are `fpga_det`-indexed.**
  The FSW converts with `utils_subcolMask2fpgaMask` before writing
  SUMDMASK, VARDMASK and fsmCalMask (`OpModeMgmt.c:634`,
  `QLVariance.c:86`, `QLDetPerf.c:184-190`, `Detectors.c:1190` [C]). The
  model only sees `fpga_det` masks.
- The QL LC, flare and variance detector masks default to 0xFFFFFCFF
  (`subcol` bits; `ParamMgmtDef.h:251, 297, 331` [C]). They exclude
  `subcol` 8 and 9 = `fpga_det` 13 and 10.

**The conversion tables live in one place.** `stix_det.c` holds one
`static const` table set, each copied from the FSW table named above with
its file:line in a comment: `fpga_det`↔`subcol`, `fpga_det`↔`fe_sc`,
`fpga_det`→(`grp`, `asic`), `grp`→`quarter`, `pix`↔`asic_ch`. No other
file contains an index mapping. A realize-time self-check asserts that each
forward/inverse pair composes to the identity.

**End-to-end check** (D-6):
- a qtest deposits N counts at (`fpga_det` F, `pix` P, `ebin` B) through
  the runtime knob `det-inject` and asserts the big-endian u16 at LACC
  byte offset ((P·32+F)·32+B)·2 of the finished bank;
- `det-chunk-format` covers the same layout in the ROTB payload;
- detector-smoke deposits a one-shot excess at a known `fpga_det` and
  asserts that the dumped QL spectra TM (SSID 32, whose per-structure
  detector index is a `subcol`, `QLSpectra.c:133-150` [C], ICD Table 105)
  shows it only in the sample whose detector byte is
  `utils_subcolIdFromFpgaId[F]`, at byte B+1 of that 35-byte sample
  (one compressed count per `ebin` 0..31) [D=C];
- the replay gate (D-7) checks the same mapping with the user's data:
  events on `fpga_det` 9-15 must show only at `subcol`
  {14, 9, 15, 7, 8, 2, 3} [V].

## Key facts the plan rests on

### The CTRL device today [C]
- `grlib_stixctrl.c` stores 0x00-0x4C (ideFxCmdSta .. fsmCalRes) as plain
  registers (`grlib_stixctrl.c:870`). There is no CmdReady, no strobe
  logic, no live status and no timer. `irq_detectors` (sysbus IRQ 0) is
  wired to IRQMP line 3 (`leon3.c:470-471`) and never raised.
- **`grlib_stixctrl_reset` (`grlib_stixctrl.c:957-1003`) does not touch
  `s->reg`.** All stored register values survive a device reset today
  (Phase 4 met the same issue with hkProt).
- `stixctrl_hk_value` reads `s->reg.idefxConfig & 0xF` for DET_C
  (`grlib_stixctrl.c:479`, accepted-diffs §22).
- LACC/EACC/TLUT/ELUT live in the 2 MB SRAM RAM region, LVAR in the
  0x400 BRAM region, LTRG and ETRG in their own regions
  (`grlib_stixctrl.c:927-943`; accepted-diffs §13). The regions survive
  a system reset.
- **The LVAR region is shared.** Bank n is 0xE0200000 + n×0x100, of
  which only 0xA0 bytes (40 slots × u32, `Detectors.h:42`
  `Detectors_MAXINTTIME 40`) are LVAR (`StixIdpu.h:302-320`). Slot 42 of
  bank 0 is the WDT `UNCHANGED_STORAGE` at 0xE02000A8 (`WDT.c:74`), and
  MemBram is at 0xE02003C0 (§13). The model must write only the first
  0xA0 bytes of each bank.
- ROTB: a device-internal ring of `rotb-blocks` × 512 B, with the
  command and status logic of §12. PUTBLOCK works only in TESTMODE.
  **There is no hardware fill path.**
- The boot leaves rotbConfig at 0x80000900, which is ENABLE=1 and
  TESTMODE=0 (`RotBuf.c:54-65` [C]; golden trace lines 188-202 [G]).
  SAFE never sets FSM Enable, which therefore must gate chunk
  generation.
- The fsmCmd writes at boot are 0x2, 0x1002, 0x1802 [G]: the
  ClearEnable strobe stays stored, so every later `fsmCmd |=` re-issues
  it.
- `capture-golden.sh` bounds the run by time (`timeout 90`), not by
  lines, and the trace golden is a manual reference that no gate diffs
  (`baseline/README.md:138-141`) [C].

### The FSW's FSM, QL and IRQ 3 use [C]
- **All strobes are `fsmCmd |= bit`** (BSW `Detectors.c:136, 186, 1093-1124,
  1198-1201`).
  - The model must self-clear bits 8:0 on read. Otherwise a stored
    ClearEnable re-fires on the ISR's QlClear read-modify-write.
  - The configuration bits are read back by RMW. These are CalTq 31:16,
    FifoIrqEn 12, QlIrqEn 11, CalIrqEn 10 and ActiveTlut 9
    (`Detectors.c:248-312, 489-508`).
- **`Detectors_init`** (called at boot, `OpModeMgmt.c:1056`) sets
  QlIrqEn = 1 and FifoIrqEn = 1, hooks `detISR` and enables IRQ 3
  (`Detectors.c:315-327`).
- **The ISR** (`Detectors.c:122-243`):
  - on QlReady: writes QlClear, picks bank **(ActiveQl−1) mod 4**
    (`:144-166`) and calls `Detectors_callbackQuickLook`;
  - on CalReady: reads CalCount/CalRes, writes CalClear;
  - on QueueOverflow: writes OverflowClear;
  - SEU: `testPulseCmdSta.QuarterSeu` (31:28) & `idefxConfig.SeuIrqEn`
    (`:217`). It must read 0 or XrayDetFDIR_SEU runs;
  - at the end it clears the IRQMP pending bit (`:233`). A pulse per
    event is enough.
- `Detectors_fsmReset` clears the FSW's own `detQLReady` (`Detectors.c:1125`).
- **The QL callback** returns at once unless the mode is NOMINAL. It
  timestamps "now − (QL period+1)" and wakes the QL task
  (`QLAccuDblBuffs.c:133-158`).
  - The task runs RCR, reads 12×32×32 LACC words plus 16 LTRG
    (`readQL`, `:162-200`) and LVAR (QLVariance), then runs the LC, BG,
    spectra, detector-performance and flare-location steps (`:205-260`).
  - Spectra are built once at period 8, then wait 256 periods
    (`QLSpectra.c:65-66, 118-184`).
  - **QL TM needs no events.** Zero counts drive the whole chain.
- **NOMINAL entry** (`OpModeMgmt.c:470-690`), in order:
  1. quarter power on (`:470`);
  2. group enable mask 0xFFFF (`:501`, `HWConfig.c:144-149`); the
     verification period parameter is written back (`:502`);
  3. readout mode mask 0xFFFF;
  4. the 384 ASIC register writes (`:576`);
  5. LUT updates (with an empty flash: 0x542C, "T1LUT applying error");
  6. `setNominalIntParams(9, 199, 1800)` = min 1 s / max 20 s / 1800
     events (`:625`, `ParamMgmtDef.h:382-384`);
  7. SUMEMASK 0xFFE (`ebin` bits) / SUMDMASK 0xFFFFFFFF (`fpga_det` bits,
     via `utils_subcolMask2fpgaMask`, `:634`; `ParamMgmtDef.h:386-387`);
  8. `QLAccuDblBuffs_init` → `setQLIntTime(39)` = 4 s
     (`QLAccuDblBuffs.c:85`, `ParamMgmtDef.h:385`);
  9. `clearNominalAcc` + `clearQL` (software memset of all banks,
     `Detectors.c:340-366`);
  10. SetEnable and Reset (`:669-672`);
  11. `RotBuf_setTestMode(FALSE)` (`:689`).
- **SAFE and CONFIGURATION entry write ClearEnable** (`OpModeMgmt.c:128,
  258`). SAFE also sets ROTB test mode. The only other FSM users are
  TC-driven procedures: the BG/calibration accumulator
  (`BGAccumulator.c:159-204`), the HW self-test (`HWSelftest.c:620-628`),
  test pulses and threshold scans (`XRayADCsCtrl.c`). The SAFE boot
  never enables the FSM.

### The FSW's IDeF-X use [C]
- **Transmission** (`Detectors.c:677-724`):
  - one 20-bit word per write to ideFxCmdSta, as `code<<16 | data`
    with GrpAddr = `grp`;
  - `WAIT_FOR_READY` (1000 polls of bit 31) before and after each word
    (`:627-639`);
  - after each word the response is read from `idefxModeData[15:0]`.
  - Every operation first clears the group's ReadMode bit and sets
    WriteMask = 1<<`grp` (`WRITE_TO_GROUPS`, `:657`, `:811`), and
    restores both at the end.
- **Words.**
  - `writeReg` (`:728-785`): `0`, `0xBFD30|asic<<6`, `0x80000|reg<<7`,
    then one `0x80000|byte<<7` per byte, then `0x48000`, `0`.
  - `readReg` (`:787-851`): the same with `0x84000|reg<<7` (`:817`);
    each data word `0x80000` returns a byte. The last byte is shifted
    `>> (8 - len%8)` when `len%8 != 0` (`:840`). All the short registers
    are single bytes; R3, R4 and R12 are multiples of 8.
  - `resetReg` (`:853-881`): `0`, `0xBFF30|asic<<6`, `0`.
  - `readTemp` (`:883-950`): a read address block for reg 13, then
    **cmd 1** twice (`:916, :933`), with readA ≥ readB required
    (`XRayADCsCtrl.c:540-546`).
  - `adcRead` (`:976-1003`) uses **cmd 1** (`:996`); `onDemandRead`
    (`:1009-1087`). All ADC reads are TC-only paths
    (`background.c:143-150`).
- **Register sizes:** R1 2, R2 3, R3 192, R4 32, R5 4, R6 2, R7 5, R8 2,
  R9 2, R10 3, R11 3, R12 32 bits (`ASW include/commonFSW.h:251-262`,
  `XRayADCsCtrl.c:317-331`).
  - R3 passes through `utils_reorderThreshold3` before the write and
    after the read (`XRayADCsCtrl.c:3019-3035, 674-710`; `utils.c:663-677`).
    The reorder is self-inverse, so raw byte storage round-trips.
- **Defaults** (`ParamMgmtDef.h:525-536` `ASIC_REGISTER_n_HW_DEFAULT`;
  the gate ELF's parameter records agree): R1 1, R2 0, R3 62, R4 0,
  R5 15, R6 1, R7 0, R8 3, R9 0, R10 3, R11 3, R12 0xFFFFFFFF. When the FSW
  writes R3 it uses 62 for pixel channels and 63 for the others
  (`XRayADCsCtrl.c:205-209`). The RCR state-0 R3 equals the default R3
  (`RCR_PIXEL_0 = 0xFFF`, `ParamMgmtDef.h:350`).
- **ASIC verification** (`XrayDetFDIR.c:140-300, 596-690`):
  - It runs every `ASIC_VERIFICATION_PERIOD` seconds (default 0x1000 =
    4096 s, `ParamMgmtDef.h:227`). The counter is the 1 s FDIR timer's;
    it counts from boot and resets on each fire (`FDIR.c:480-486`), and
    the run happens only in NOMINAL (`XrayDetFDIR.c:310-330`).
  - It reads all 12 registers of the powered ASICs and compares them
    with the defaults. R3 is compared with the RCR's current thresholds
    (`:664`).
  - On a mismatch: TM(5,2) **0x542E**, a rewrite, a second check, then
    TM(5,1) **0x142C** or TM(5,3) **0x9438** plus a permanent
    `CALISTE_IGNORE_FLAGS` update (`:196-283`).
  - **A successful run is silent** (`ENABLE_TRACE_DETFDIR` 0,
    `configFSW.h:89`; `:189-193`): no UART line, no TM.
  - **A read timeout is not a mismatch:** `XRayADCsCtrl_readASICReg`
    returns FSW_ERROR (`XRayADCsCtrl.c:699-703`) and `verifyRegisters`
    skips that register (`XrayDetFDIR.c:611-615`). So with no IDeF-X
    model the verification also ends "all good".
  - The period can be set with **TC(236,6) system 2, parameter 2**, a
    u16 in 0..0xFFFF (`HWConfig.h:12, 40`, `HWConfig.c:974-976`,
    `TCVerifDynamic.c:366`).
- **The Phase 3 trap still holds** (accepted-diffs §24): CmdReady alone
  makes every later read-back return zeros, and the verification then
  ignores every ASIC for good. **CmdReady and the register store must land
  in one commit.**

### The FSW's science path [C]
- `RotAccumulator_doTask` runs only in NOMINAL (`RotAccumulator.c:64-140`):
  - it reads BLOCKCOUNT and walks blocks through the 0xE0800000 window;
  - the type is `(block32[0] >> 16) & 0xC000`: 0x8000 detector, 0x4000
    aspect (`RotAccumulator.h:16-22`);
  - the block number is `& 0x3F` and must be ≤ 48. **Anything else is
    `ASSERT(0)`** (`RotAccumulator.c:150-220`);
  - each block carries a u16 descriptor, a 6-byte SCET and 504 data
    bytes; data starts at offset 8 in **every** block.
- `RotAccuCompactor_processDetectorBlock` (`RotAccuCompactor.c:76-270`):
  - it requires block numbers 0, 1, … 48 in sequence ("packet sequence
    error" otherwise);
  - it takes the timestamp from block 0, which is the integration end
    (`ArchiveManager.c:131`);
  - it reads 48 × 252 + 192 u16 counts (`[pix][fpga_det][ebin]`), then
    16 × (u16 hw, u16 lw) triggers per `grp`, then a 56 B pad
    (`:174-245`);
  - it hands the chunk to `ArchiveManager_addDetectorData` (flash
    archive), which prints the TRACE_ERROR **"Skipped first Det data
    chunk"** for the first chunk (`ArchiveManager.c:140`). A compacted
    chunk is at most `MAX_DETECTOR_DATA` 49216 B (`RotAccuCompactor.h:8`).
  - **No TM is produced by the chunk itself.**
- The ROTB IRQ 7 ISR sets `b_rot_buf_overrun_occured`. The task then
  skips 60 s of blocks by the SCET at +2 of every block
  (`RotAccumulator.c:55, 100-125`).

### TM and RCR facts for the gate [C]
- QL TM goes to SSMM. The SSMM mask default 0x1DF1F1F enables SSID
  30-34 (`ParamMgmtDef.h:388`, `commonFSW.h:219-223`,
  `TMBuffer.c:293-310`). The model's TM transport forwards every packet
  [C, `tmtc-probe.py:95-112` parses after the SpW header], so they reach
  the probe.
- **SSID 30 layout** (`TM_21.c:315-398`): SSID, coarse u32, fine u16,
  intTime u16, deteMask u32 (`subcol` bits), pixelMask u16, two schema
  bytes, energy mask u32, nBands u8; then per band a u16 count N and N
  compressed bytes; then N trigger bytes and N RCR bytes, each after its
  own u16 N.
  - The compression S0 is monotone, so the compressed bytes can be
    compared directly.
- **RCR** (`RateCtrl.c:300-360`, `ParamMgmtDef.h:343-349`): the RCR
  rises when the median group trigger count per QL period exceeds
  L1 = 140000, i.e. 35 k/s per `grp`, about 17.5 k counts/s per
  `fpga_det`.
  - A rise moves the attenuator (`RateCtrl.c:495-515`), which the model
    does not animate (ATT_C is a static 20, §22).
  - **Synthetic rates must stay well below L1 by default.**
- **QLDetPerf** (`QLDetPerf.c:115-180`; Kbad 5, Cbad 300, Nrep 15,
  `ParamMgmtDef.h:336-341`) flags a detector "noisy" when it has more than
  5 × max(median, 1) **and** more than 300 counts per 2 periods
  (`QLDetPerf.c:124-143`). The check runs every 2 QL periods (8 s) on
  per-`subcol` counts in `ebin` 1-30 (mask 0x7FFFFFFE) over all pixels;
  the median includes CFL and BKG. A detector with Nrep (15) yellows in
  the last Nbad (15) checks (about 2 min) goes red: TM(5,3) **0x942E**,
  TM(5,2) **0x5437** (`TMForm.c:612-706`), and removal from the active
  LC/FD/VAR masks and SUMDMASK (notes-tmtc-icd-detector §6).
  **The synthetic sky must be uniform across `fpga_det`, and the replay
  gain is fixed at 1×.**
- The TSIM QL I/O model (`TSIM_IO/.../QLinputsTsim.c`) replays per-period
  binary dumps: `qlacc` 24576 B of u16 BE, `qltrg` 64 B of u32 BE and
  `qlvar` 160 B of u32 BE. These are the RP-0119 verification output
  formats [D, notes-fpga-arch-verif §5].
  - Its `DetectorsApiTsim.c` forces CmdReady = 1 and
    testPulseCmdSta.Busy = 1.
  - It is prior art for the "always ready" choice.

### QL TM decode for the gates [D=C] (ICD Tables 103-107, `TM_21.c:315-647`)
- **Framing.** APID 0x5DC (PID 93 cat 12), TM(21,6); the **SSID is
  CCSDS byte 16** (`TMData.h:56` TMData_DATA_FIELD); **no CRC/PEC**
  (`TM_21.c` create functions). The SpW transport adds a 4-byte header in
  front. A product that spans packets uses FIRST/CONT/LAST segmentation,
  and a flush may end with an empty termination packet
  (`TMForm_nextSegmentationGroupingFlagQL`). All offsets below are
  relative to the SSID byte; multi-byte fields are big-endian.
- **Compression.** Schema byte `0 S KKK MMM`; default for all QL
  products **0x2B = S0, K5, M3** (`ParamMgmtDef.h:256-279, 335`,
  `CompressionAlgorithms.c:18-90`). Compress: V < 16 → C = V; else shift
  right until V < 16, e = shifts + 1, C = (V & 7) + (e << 3). Truncating
  and monotone. **Decode (M = 3):** C < 16 → V = C; else
  V ≈ (8 + (C & 7)) << ((C >> 3) − 1), a lower bound (true
  V < (9 + m) << (e − 1)). Examples: 100 → 0x24, 1000 → 0x3F,
  2160 → 0x48, 10000 → 0x59, 65535 → 0x6F. Triggers with the default
  schema are S0-compressed too.
- **SSID 30 (light curves)** offsets: 0 SSID 0x1E; 1 coarse u32; 5 fine
  u16; 7 integration time (0.1 s, 0-indexed, 39); 9 detector mask u32
  (`subcol` bits, default 0xFFFFFCFF: no CFL, no BKG); 13 pixel mask
  (0x0FFF, bit p = `pix` p); 15 LC SKM 0x2B; 16 TA SKM<<1 | upper edge
  (0x56); 17 lower edge mask 0x10821082; 21 E = 5; **22 band-0 N (u16),
  24 band-0 bytes**, then bands 1-4, then u16 N + N trigger bytes, then
  u16 N + N RCR bytes. Bands: `ebin` 1-6, 7-11, 12-16, 17-22, 23-27
  (`utils.c:360-445`); `ebin` 0 and 28-31 are in no band. LC[band] =
  Σ LACC over masked `pix`, `subcol` in the LC mask **and** QLDetPerf's
  active mask, `ebin` in the band (`QLAccuDblBuffs.c:164-189`). One sample
  per 4 s. **Worked example:** 1 count in every cell for one QL period →
  band 0 = 6 `ebin` × 12 `pix` × 30 `subcol` = 2160 → 0x48.
- **SSID 31 (background):** integration time 79 (**8 s**, nInt 2,
  `ParamMgmtDef.h:265`); counts only from `subcol` 9 (BKG = `fpga_det`
  10), large `pix` 0-7 (`QLBGMon.c:162-181`); triggers from LTRG
  `grp` 4 (`BCKG_ASSOC_GROUP`, `commonFSW.h:161`), although the BKG
  detector is in `grp` 5 (Doc vs code 17). One sample per 8 s.
- **SSID 32 (spectra), the index gate:** 7 integration time 319 (32 s);
  11 pixel mask; 13 N; then per sample (35 B, starting at 15): detector
  index = `subcol`, 32 compressed counts `ebin` 0..31, 1 trigger byte
  (LTRG of that `subcol`'s `grp`), 1 integration offset. Default det mask
  all 32. It sums **8 QL periods (32 s) from the start of NOMINAL**, emits
  all 32 detectors at once, then waits 256 periods (**1024 s**) before the
  next dump (`QLSpectra.c` processAccumulatedQLData, `ParamMgmtDef.h:258-264`).
  One dump is 1120 B; the packet fills after the 4th dump, so **a gate
  gets exactly the first dump in the NOMINAL-exit flush**, provided NOMINAL
  lasted ≥ 32 s and the deposit fell in the first 8 periods.
- **SSID 33 (variance):** 7 integration time 39; 9 samples per value 40;
  10 det mask (`subcol`) 0xFFFFFCFF; 14 energy mask 0x7FFE0000
  (`ebin` 17-30); 18 pixel mask; 20 SKM; 21 N; 23 one byte per QL period.
  The value is Σ_{i<39} (|v_i − ⟨v⟩|/4)² with ⟨v⟩ = Σ/40: **LVAR slot 39 is
  never read at the default period** (`QLVariance.c` calculate; Doc vs
  code 18). For a uniform Poisson rate r per slot the value ≈ 39·r/16.
- **SSID 34 (flare flag):** 7 integration time 39; 9 N; then 3 B per QL
  period: status (bits 2:0 thermal class, 4:3 non-thermal, 6:5 location,
  7 in progress), Z, Y (int8 arcmin) (`QLFlareDet.c:33-36, 197-221`).
  Produced every period even though `QL_FLARE_ENABLE_TM` defaults to
  FALSE (`ParamMgmtDef.h:316`, not checked on this path).
- **Packet fill times:** LC about 38.7 min, BG longer, spectra after the
  4th dump, variance about 4.5 h, flare about 90 min. So in a
  minutes-long NOMINAL window **every one of SSID 30-34 leaves only in the
  exit flush**, each with all its samples since NOMINAL entry.
- **Science TM (not gated in this phase):** SSID 20-24 on APID 0x5BC need
  a TC(238,x) user request. The L0 (SSID 20) sample control word is
  **`pix[15:12] | subcol[11:7] | ebin[6:2] | cont[1:0]`** (ICD Table 94,
  `SciReport.c:291-325`), while the archive word the compactor builds from
  the ROTB is **`cont[15:14] | pix[13:10] | subcol[9:5] | ebin[4:0]`**
  (`RotAccuCompactor.c:290-338`). The ROTB itself carries `fpga_det`; the
  compactor converts it to `subcol` (`RotAccuCompactor.c:298-300`). TM
  L0 = `subcol`. The 16 trigger accumulators in the L0 time bin are in
  `grp` order (ICD Table 33).

### The TCs the gates use [D=C]
All are TC packets on APID 0x5AC (PID 90 cat 12), built like nominal-smoke's
(`nominal-smoke.sh:131-137`: SpW header, CCSDS with a distinct sequence
count, data field header `19 <svc> <sub> 00`, CRC-16/CCITT-FALSE).
- **TC(236,1) mode transition** (ICD Table 119, ICD:14574-14664): app data
  `<mode> <flag>`, mode 2 SAFE, 4 CONF, 5 NOMINAL. CONF = `04 00`
  (`TC_CONF`), NOMINAL = `05 00` (`TC_NOM`). Leaving NOMINAL is what
  flushes QL TM (`QLAccuDblBuffs.c:117-125`).
- **TC(236,6) HW parameter:** `07 04 00 00` (`TC_S20`, nominal-smoke) and
  `02 02 00 1E` (system 2 detectors, parameter 2 ASIC verification period,
  u16 30 s; `HWConfig.h:12, 40`, `HWConfig.c:974-976`,
  `TCVerifDynamic.c:366`; confirmed by the validator).
- **There is no service-236 QL TC.** The ICD TOC's "TC(236,9)" (ICD:468)
  is TC(237,9) in the body and the code (Doc vs code 14). The gates
  change no QL parameter; if a later gate must:
  - **TC(237,9)** (ICD Tables 165-166, ICD:20444-21332, `TCData.h:229-330`)
    sends the whole 215-byte QL parameter set (QL period u16 at 0, LC at
    44-58, VAR 59-70, SP 71-80, DF 81-89, …) and applies it at once
    (`QLAccuDblBuffs.c:320-339`; the FPGA latches the period at the next
    QL period, D-Dd). The ICD forbids a QL period below 39 (ICD:20545).
  - **TC(237,18)** loads one ASW parameter: u16 parameter ID = the
    `ParamMgmtDef.h` enum index (NOMINAL_QL_PERIOD **315** = 0x13B,
    SUMEMASK 316, SUMDMASK 317, SSMM mask 318, ASIC_VERIFICATION_PERIOD
    157), u8 sub-ID (0 value, 1 min, 2 max), u32 value
    (`TcService237.c:66-80`). The value lands in ParamMgmt only; QL
    modules re-read it at NOMINAL entry.
- **TC(21,1)/(21,2)** enable/disable SSIDs to SSMM with a u32 mask
  (ICD Tables 88-90, `TcService21.c:39-60`, `commonFSW.h:209-226`):
  SSID 10-14 = bits 0-4, **20-24 = bits 8-12, 30-34 = bits 16-20**,
  41/42/43 = bits 22-24. The default 0x1DF1F1F already enables all, so
  the gates send none.

### The ELUT [C]
- The FPGA ELUT is `uint8 val[pix 12][fpga_det 32][adc 4096]` = 1.5 MiB
  at **0xE0000000-0xE017FFFF**, the start of the SRAM RAM window
  (`Detectors.h:47-52, 109` `Detectors_ELUT`; `StixIdpu.h:257-259`
  `STIXCTRL_ELUT_START/SIZE`). The model already backs it with RAM (§13).
  Value = `ebin` 0-31, or **32 = unassigned** (`LUTMgmt.c:834-935`).
- The FSW writes it in `applyELUT` from a 33-boundary table per
  (`fpga_det`, `pix`) (ICD §7.2.11.2, Table 156, ICD:19596-20115;
  uploaded by TC(237,7), applied by TC(237,8)), and verifies it by
  reading it back (`LUTMgmt.c:958`).
- **`applyELUT` runs on every CONF→NOMINAL entry** (`OpModeMgmt.c:580-597`).
  With an empty flash the boundary table stays all-zero (`load_lut`
  fails silently, `LUTMgmt.c:1111-1135`), every science channel in turn
  claims [0, 4096), and **every FPGA ELUT entry ends up 31**: real
  hardware would put every event in `ebin` 31. No event is raised
  (0x542C fires only on an apply error). The comment "= 0 - 30" at
  `LUTMgmt.c:912` is wrong; the loop reaches 31. `EDAC_on` would clear the
  ELUT to 0 (all → `ebin` 0), but it is only called from `HWConfig.c:95`.
- `ebin` 31 is outside the LC bands (1-27), the QLDetPerf mask (1-30), the
  flare masks and the variance mask; only SSID 32 channel 31 and the
  triggers would show it.

### The user's `.dssevs` file [V] (notes-dssevs-fakeflare)
- 176,001,024 B = 44,000,256 big-endian u32 words, no header. Word =
  `[31:20] val12 | [19:16] type | [15:11] dss_det | [10:0] wait` (20 ns
  units). Every word's wait adds to the time.
- **Types:** 0x0-0xB are events (type = `pix`, val = `adc`); 0xD and 0xE
  are one setup word per `dss_det` each (0xD val 75, probably a threshold;
  0xE val 2000, the baseline); 0xF is wait-only padding (val 2048, one
  3072 marker), 43.9 M of them.
- Total duration 1800.1 s. About 60 k events, only on `dss_det` 9-15
  (= `fpga_det` 9-15 = `subcol` {14, 9, 15, 7, 8, 2, 3}), even over
  `pix` 0-7, about 10× fewer on `pix` 8-11.
- 0-900 s quiet, about 5.6 events/s in total; 900-1800 s a flat step
  "flare" of about 62 events/s (about 9 cps per lit `fpga_det`).
- `adc` spectrum line-like: peaks in 2176-2560, 2816-3072, 3584-3840.
- Of the 7 lit detectors, `fpga_det` 13 and 10 (`subcol` 8, 9) are outside
  the LC/flare/variance masks, so **5 lit detectors feed the LC**.
- The file is confidential, lives outside the repo, and is never
  committed, nor is anything derived from it.

## Decisions (D-D)

- **D-Da: where the code lives.** Add one new compilation unit,
  `hw/sparc/stix_det.c` with an internal header `stix_det.h`, holding
  `StixDetState`, which is embedded in `STIXCTRL`. It is not a new QOM
  device.
  - `grlib_stixctrl.c` delegates APB offsets 0x00-0x4C to
    `stix_det_read`/`stix_det_write`, and its reset and realize call
    into it. The functions take the `STIXCTRL` pointer.
  - **One source of truth for registers:** the stored register fields stay
    in `s->reg` (so `stixctrl_hk_value`'s DET_C keeps working unchanged).
    `StixDetState` holds only derived and internal state (timers, latched
    parameters, decoders, the IDeF-X store, exposure, PRNG, replay table).
  - Build: `sparc_ss.add(when: 'CONFIG_GRLIB', if_true: files('stix_det.c'))`
    next to `grlib_stixctrl.c` in `hw/sparc/meson.build`.
  - Why not a separate device: the detector registers sit in CTRL's one
    APB bank; the accumulators live in CTRL's RAM regions; the chunks go
    into CTRL's ROTB ring; and Phase 4 already put the runtime knobs on
    `/machine/stixctrl`. A second sysbus device would need overlapping
    MMIO, shared RAM pointers and a second reset order. The split keeps
    `grlib_stixctrl.c` (1105 lines) from doubling.
- **D-Db: QOM properties** (all on `/machine/stixctrl`; names carry the
  index space where they take one).
  - Init-time (qdev) properties:
    - `det-model` (bool, default on): off keeps today's raw store for
      0x00-0x4C and today's no-reset behaviour (A/B and rollback, like
      `hk-adc`);
    - `det-idefx` (bool, default on): off keeps ideFxCmdSta raw, which
      reproduces the 384 timeouts;
    - `det-rotb` (bool, default on, D-5): off disables chunk generation;
    - `det-seed` (uint32, default 1): the PRNG seed;
    - `det-replay-file` (string, default empty, D-7): the `.dssevs` path,
      pre-scanned at realize.
  - Runtime properties: `object_class_property_add` with setters, as for
    `adc-inject[N]`, because a qdev property cannot be set after realize.
    - `det-source` (str): `zero` | `synthetic` (default `synthetic` from
      D-4; only `zero` exists before) | `replay-dss` (D-7; accepted only
      when a table is loaded). Replay **replaces** the synthetic sky.
    - `det-flare` (bool, default false): the flare switch.
    - `det-quiet-cps` and `det-flare-cps` (uint32, counts/s per
      `fpga_det`; defaults 20 and 2000; 0..1 000 000, rejected outside).
    - `det-inject` (str, D-6, write-only): `fpga_det=F,pix=P,ebin=B,count=N`,
      a one-shot deposit into the active QL bank and the running nominal
      integration. Keys are mandatory, so the index spaces are explicit.
    - `det-replay-trigger` (str, D-7): `nominal` (default; start at the
      first FSM SetEnable 0→1 edge after reset, i.e. NOMINAL entry) or
      `qmp`.
    - `det-replay-offset` (uint32 s, D-7, default 0): file time at start.
    - `det-replay-start` (bool, D-7): writing true starts the replay now
      (trigger `qmp`); false stops it; reading gives the running state.
    - Setters validate, store fields, trace (`stixdet_scene`) and take
      effect at the next 100 ms tick (D-Dj).
  - **Reset:** the scene knobs (`det-source`, `det-flare`, the rates, the
    replay trigger and offset) **survive** a device reset, like `timecodes`
    (§20). They describe the Sun, not the IDPU. A running replay stops,
    and re-arms on its trigger. Everything else resets (D-Dh).
- **D-Dc: FSM register semantics (tier 0).**
  - fsmCmd: bits 31:16, 12:9 are stored and read back. Bits 8:0 are
    strobes, act on the write and read 0. Reserved bits 15:13 and 4:3
    read 0.
  - **SetEnable** sets Enable and starts the tick. **ClearEnable** clears
    Enable and stops the tick, and wins if both are set; it **freezes**
    everything else (QlReady, SUMCNT, the counters and the banks stay as
    they are) [I].
  - **Reset** clears QlReady, QueueOverflow and SUMCNT, restarts both
    integration counters and re-latches the parameters; it keeps
    ActiveQl, ActiveNominal and the banks [I]. The FSW always issues
    SetEnable then Reset (`OpModeMgmt.c:669-672`) and clears its own
    `detQLReady` at Reset (`Detectors.c:1125`), so the choice is not
    FSW-visible.
  - QlClear clears QlReady. CalClear clears CalReady and CalCount.
    OverflowClear clears QueueOverflow. SetCalEnable sets CalEnable;
    calibration never completes (a known limit).
  - fsmSta (0x18) is read-only and composed live: Enable 0, Overflow 1,
    ActiveQl 3:2, ActiveNominal 4, QlReady 5, CalReady 6, CalEnable 7.
  - fsmEvents (0x30) is read-only: SUMCNT so far.
  - fsmCalCount, fsmCalRes and fsmResult1/2 read 0. The FSW never reads
    fsmResult1/2 [C, grep].
  - testPulseCmdSta (0x10) is composed: Busy (bit 16) reads 1 (idle,
    active-low, `Detectors.c:966-971`) and QuarterSeu 31:28 reads 0. The
    other bits are stored.
- **D-Dd: QL timing, bank lifecycle and IRQ (tier 0).**
  - A `QEMU_CLOCK_VIRTUAL` tick of 100 ms runs **only while Enable = 1**.
    It uses absolute deadlines, so it does not drift.
  - QlIntTime is latched at the start of each QL period [D, RP-0067
    A 5423-5425], so a TC change takes effect at the next period.
  - **LVAR slot:** tick k of the period (0-based) adds to LVAR slot k
    only while **k ≤ 39**; the LVAR counts of later ticks in a longer
    period (QlIntTime is 16 bits) are dropped. The hardware behaviour is
    undocumented [D silent]; this is a model choice that keeps every
    write inside 0xA0 and away from 0xE02000A8. At the default period
    (40 ticks) the model fills slots 0-39, and the FSW reads only 0-38
    (SSID 33 uses min(QL period, 40) = 39 intervals, `QLVariance.c`
    calculate): slot 39 is written but unused.
  - **Bank lifecycle (defined once, used by D-1, D-4 and D-6):**
    1. while bank b is active, the model accumulates into a host-side
       shadow (D-Df) and never writes bank b's RAM, except to zero it on
       activation (step 3) and to write its LVAR slots per tick;
    2. at the swap the model writes bank b in full from the shadow (all
       zeros with `det-source=zero`) and pulses the IRQ;
    3. it sets ActiveQl = (b+1) & 3 and zeroes that newly active bank
       (LACC 0x6000, LTRG 0x40, LVAR **0xA0 only**);
    4. **after that the model does not touch bank b again until it becomes
       active again, three swaps later.** The FSW reads it after the IRQ.
  - At the swap: set QlReady; `qemu_irq_pulse(irq_detectors)` if QlIrqEn.
  - Why Enable gates the tick: the FSW only consumes QL in NOMINAL and
    clears Enable in SAFE and CONF, so SAFE gates (boot, soak, tc, fault)
    see no IRQ 3. The docs do not say whether the FPGA's 100 ms counter
    runs with Enable = 0 (notes-fpga-arch-verif §4, open "Q1"); nothing
    in the FSW depends on it.
  - "Zero on activation" stands in for RotbCtrl's read-and-clear (see
    "Doc vs code" 6).
- **D-De: IDeF-X model (tier 1)**, at the FSW-visible register level, not
  at STROBE/DIN bit level.
  - **ideFxCmdSta:** a write executes at once. A read returns the stored
    fields with **CmdReady (bit 31) = 1**. Reset value: 0x80000000.
  - **idefxModeData:** bits 31:16 (ReadMode, `grp` bits) are R/W. Bits
    15:0 are read-only: the response of the last command.
  - **idefxWriteMask** bits 31:16 (`grp` bits) are R/W.
  - **Delivery:**
    - a word goes to every `grp` in WriteMask; with WriteMask = 0 it goes
      to GrpAddr's `grp` (lenient);
    - a `grp` takes part only if its `quarter` is powered
      (idefxConfig[`quarter`]), its Enable bit (idefxConfig[19:4]) is set
      and its ReadMode bit is 0 [D, RP-0067 IdefxCtrls];
    - the response comes from GrpAddr's `grp`, or 0 if that `grp` did not
      take part;
    - a command to a non-participating `grp` is ignored, and CmdReady
      stays 1 [D].
  - **Per-`grp` decoder:**
    - cmd 0 resets the transaction;
    - cmd 11 latches SC/RD/All/address from CmdData 15:4. RD=0, SC=1 is
      slow control, with address 3 → `asic` 0 and 7 → `asic` 1; All = 1
      addresses both. RD=1, SC=1 resets the registers of both `asic` to
      the defaults;
    - the first cmd 8 is the address block: bit 14 is R/W, bits 13:7
      the register;
    - each further cmd 8 is a data block: a write shifts byte 14:7 into
      the register; a read returns the next byte;
    - cmd 4 ends the transaction;
    - **cmd 1 (ADC read) answers by context:** the first cmd 1 after a
      reg-13 read address block returns A = 0x900, the cmd 1 after the
      following data block returns B = 0x700 (A ≥ B,
      `XRayADCsCtrl.c:540-546`); any other cmd 1 returns the baseline
      1850 [D, DSS app TempScript]. cmd 2/3 return 1850.
  - **Registers:**
    - a register is an N-bit shift register that keeps the last N bits
      written (IDeF-X DS); a read emits them MSB first, left-aligned,
      zero padded;
    - so a register shorter than 8 bits reads back `value << (8-len)`,
      which `readReg` shifts back (`Detectors.c:840`);
    - R3 is stored raw, in wire order: the FSW reorders it on both sides;
    - reg 13 (TEMP) is a dummy that sets the ADC context.
  - **Defaults** (device reset, the RD=SC=1 command, and a `quarter`-power
    0→1 edge): the FSW HW defaults, **[C]** `ParamMgmtDef.h:525-536`
    (R1 1, R2 0, R3 62 on every `asic_ch`, R4 0, R5 15, R6 1, R7 0, R8 3,
    R9 0, R10 3, R11 3, R12 0xFFFFFFFF); the IDeF-X DS table agrees [D].
    The FSW rewrites all twelve at NOMINAL entry.
  - **Not modelled:** SEU. QuarterSeu is always 0.
- **D-Df: the count source (tier 2): synthesize accumulators, not events.**
  - **Per 100 ms tick** the model evaluates the gating and advances
    scalar bookkeeping:
    - **gating per tick, not as a swap-time snapshot** (every
      verification run, now every 30 s in the gate, briefly clears one
      `grp`'s ReadMode, `Detectors.c:811`; a snapshot would zero that
      `grp` for a whole period). A (`fpga_det`, `pix`) pair counts in a
      tick only if its `quarter` is powered, its `grp` is enabled and not
      in config mode, and the R3 threshold of `asic_ch` =
      `pix2asic_ch[pix]` is not 63;
    - ALIMON (R12) is **not** used for gating (its per-channel bit order
      is undefined, [A], Doc vs code 12);
    - exposure: for each enabled (`fpga_det`, `pix`) the model adds
      `rate_quiet·dt` or `rate_flare·dt` to `expo[fpga_det][pix][profile]`
      (two profiles; 768 additions per tick);
    - LVAR: draws the slot's masked count for the tick (VARPMASK `pix`,
      VARDMASK `fpga_det`, VAREMASK `ebin`, with the [A] bit order) and
      adds it to the current slot (D-Dd slot rule);
    - SUMCNT: draws the tick's SUMDMASK-`fpga_det`/SUMEMASK-`ebin` count.
  - **At a QL swap** it fills the finished LACC bank with
    Poisson(Σ_profile template[profile][pix][ebin] × expo[fpga_det][pix][profile])
    (12288 draws) plus any injected or replayed counts, writes LTRG per
    `grp`, and clears the exposure.
  - At a nominal swap (D-Dg) it fills EACC and ETRG the same way from a
    second exposure set.
  - **Masks** (read live from the registers):
    - SUMCNT counts SUMDMASK `fpga_det` in SUMEMASK `ebin`;
    - EACC is gated by SUMDMASK [D, RP-0067; FSW agrees: QLDetPerf drops
      noisy detectors from SUMDMASK, `QLDetPerf.c:184-190`];
    - QLACC is not masked.
  - **Widths:** LACC/EACC wrap at 16 bits, LTRG/ETRG clamp at 24 bits,
    LVAR clamps at 16 bits [D, BRAM widths].
  - **Byte order:** all accumulators are stored big-endian (`stw_be_p`,
    `stl_be_p`); the FSW reads native SPARC u16/u32 (`Detectors.h:68-95`,
    `RotAccuCompactor.c:180-225` [C]).
  - **Template** [I; the energies are approximate and gate-irrelevant,
    because no ELUT is involved]:
    - uniform over `fpga_det` (QLDetPerf);
    - large `pix` 0-7 weight 1.0, small `pix` 8-11 weight 0.22 [D: area
      ratio about 4.5];
    - quiet energy: 10 % flat in `ebin` 1-11, 15 % flat in `ebin` 12-30,
      and 75 % in Ba-133 lines (`ebin` 18 at 31 keV 45 %, `ebin` 19 at
      35 keV 10 %, `ebin` 27 at 81 keV 20 %);
    - flare energy: 70 % thermal (exponential over `ebin` 1-10, ×0.7 per
      bin) and 30 % power law over `ebin` 11-25;
    - triggers per `grp` = 1.25 × the valid events of its two `fpga_det`
      (RP-0119 measured 1.6 in one test [D]); replayed events add 1 trigger
      each.
  - **Rates** [I]:
    - quiet 20 cps per `fpga_det`: SUMCNT ≈ 64/s, so a nominal swap every
      20 s (max time);
    - flare 2000 cps per `fpga_det`: SUMCNT ≈ 45 k/s, so a swap every 1 s
      (min time). `grp` triggers ≈ 20 k per QL period, far below
      L1 = 140 k, so the RCR stays at 0.
  - **PRNG:** xorshift64*, reseeded from `det-seed` at reset. Poisson
    by inversion for λ < 30, by a rounded normal approximation above.
    Runs are statistically, not bit-for-bit, reproducible (no icount).
  - **Why no events:**
    - the FSW never needs individual events in NOMINAL (QL and science
      read accumulators only);
    - a flare at 2000 cps × 32 `fpga_det` is 64 k events/s, which would
      cost more host time than the whole guest;
    - with an empty flash the FSW-written ELUT maps every `adc` to
      `ebin` 31 (Key facts, "The ELUT"), so an ADC-domain event path would
      put every count outside the LC bands.
  - **The synthetic sky is generated directly in `ebin` space and does not
    go through the ELUT**, whatever the FSW wrote. So is `det-inject`
    (D-6). This deviates from hardware with an empty flash (which would
    show only `ebin` 31) and is recorded in accepted-diffs. The ELUT rule
    of D-Dk applies only to ADC-domain sources: the replay, and any later
    synthetic source that works in `adc`.
  - **Cost:** about 768 additions and a few draws per tick, about 25 k
    draws per 4 s QL period, plus up to one chunk per second in a flare.
    That is well under 1 % of a host core [I]; D-4 and D-5 measure it.
  - **Event sink:** `stix_det_add_counts(fpga_det, pix, ebin, n)` adds
    integer counts to the active QL shadow, the running nominal shadow
    and SUMCNT/LVAR (with the same per-tick gating). `det-inject` (D-6)
    and the replay (D-7) both use it.
- **D-Dg: nominal integration and ROTB chunks (tier 3).**
  - **Swap rule**, checked once per tick:
    `((t ≥ IntTimeMax+1) || (SUMCNT ≥ IntEventsMax)) && t ≥ IntTimeMin+1`,
    with t in ticks [D, TN Table 5-8; RP-0067]. The parameters are
    latched at the start of each integration.
  - At a swap:
    1. materialize EACC/ETRG of the finished bank;
    2. toggle ActiveNominal;
    3. reset SUMCNT;
    4. if ENABLE ∧ ¬TESTMODE ∧ FSM Enable, append one chunk.
  - **The chunk:** 49 blocks. Each block is a descriptor
    `0x8000 | n` (n = 0..48), the SCET at the end of the integration
    (the value 0xC8/0xCC would read then) and 504 bytes of the payload
    stream: EACC 0x6000 (`[pix][fpga_det][ebin]`) + ETRG 0x40 (`grp`) =
    24640 B, with the last block zero-padded by 56 B. Everything is
    big-endian (`RotAccuCompactor.c:76-270` [C]).
  - After the copy, the finished EACC/ETRG bank is zeroed
    (read-and-clear [D]).
  - **Ring full:** the whole chunk is dropped if fewer than 49 blocks are
    free, and IRQ 7 pulses once.
    - The docs say the hardware overwrites the oldest block (see
      "Doc vs code" 7). Dropping the whole chunk never leaves a partial
      chunk, and it keeps §12's drop-new rule.
    - The ring holds about 2100 chunks, so this is unreachable while the
      FSW drains it.
  - **Aspect blocks are not generated** (a known limit).
- **D-Dh: reset.** A device reset (with `det-model=on`):
  - writes explicit reset values into `s->reg` for 0x00-0x4C:
    ideFxCmdSta 0x80000000, testPulseCmdSta Busy = 1, every other
    register 0 (so the fsmCmd config bits, idefxConfig, ReadMode,
    WriteMask, fsmQlConfig, the masks and the int params do not survive a
    watchdog reboot);
  - stops the tick and clears ActiveQl, ActiveNominal, SUMCNT, the
    exposure and the shadows;
  - idles the decoders and resets the IDeF-X registers to the defaults
    (the IDPU reset cuts the ASIC supply [I]);
  - reseeds the PRNG and stops a running replay (the table stays loaded).
  - It does not clear the RAM banks (they survive, §13) or the scene
    knobs (D-Db).
  - `det-model=off` keeps today's behaviour (no register reset).
  - No vmstate, as for the rest of stixctrl (§30).
- **D-Di: gates.**
  - A new `detector-smoke.sh`. nominal-smoke stays the mode-transition
    gate, so its timing and forbids stay stable.
  - The probe gains `--dump-apid APID:FILE` (full units, not the
    48-byte log prefix, `tmtc-probe.py:510-518`).
  - A small decoder (`scripts/stix/ql-check.py`) reads the dump (all of
    SSID 30-34, with the offsets and S0 decode of "QL TM decode for the
    gates"; SSID 32 by `subcol`).
  - **ASIC verification evidence comes from the model trace**, because a
    successful run is silent: one run = 384 read address blocks
    `0x84000|reg<<7` (12 registers × 32 ASICs) in `stixdet_idefx_cmd`,
    clustered within about 1 s.
- **D-Dj: re-entrancy (the Phase 4 double-free lesson).**
  - The timer callback writes accumulators through
    `memory_region_get_ram_ptr` and the ROTB ring through its host buffer,
    never with `address_space_*`; each RAM write is followed by
    `memory_region_set_dirty`.
  - QMP setters only store fields; the next tick applies them. No MMIO
    dispatch happens from timer or QMP context.
  - The only outputs from timer context are `qemu_irq_pulse`.
  - The chunk write runs in the timer callback under the BQL, so the
    vCPU sees all 49 blocks at once.
- **D-Dk: replay (D-7).**
  - **Pre-scan at realize** when `det-replay-file` is set: read the file
    in 1 MB chunks (never whole into memory), decode each word **by its
    type field** (bits 19:16), add every word's wait to the time, and keep
    only events in a table `{t_tick u32, fpga_det u8, pix u8, adc u16}`
    (about 60 k entries, under 1 MB). Types 0x0-0xB are events (type =
    `pix`); 0xE records the per-`dss_det` baseline; 0xD is recorded and
    unused; 0xF and the marker are wait only. Words are never streamed per
    tick. A missing or unreadable file is a realize error with the path.
    Scan time is logged (`stixdet_replay_scan`).
  - **Detector map:** `fpga_det = dss_det` (identity, see "Index spaces").
  - **ADC → `ebin`: honour the FPGA ELUT, else a linear fallback**
    (coordinator decision, 2026-11-11):
    - The model looks each event up in the ELUT the FSW wrote:
      `ebin = ELUT[pix][fpga_det][adc]` at 0xE0000000 (Key facts, "The
      ELUT"); 32 (unassigned) drops the event, as the FPGA would.
    - **Fallback** per (`fpga_det`, `pix`) row when the row is
      **degenerate**: all 4096 entries equal (all 0, which is the
      power-on/EDAC state, or all 31, which is what `applyELUT` writes from
      the all-zero boundary table of an empty flash). Then
      `ebin = floor((adc − base[dss_det]) × 32 / (4096 − base[dss_det]))`,
      clamped to 0..31, with base from the file's 0xE setup word (2000 in
      the user file); events with `adc ≤ base` are dropped. With base 2000
      one bin is 65.5 `adc`, so the file's lines land in `ebin` 2-28 [V].
    - The fallback is **non-physical**: it emits the trace
      `stixdet_elut_fallback(fpga_det, pix, value)` once per row and
      replay start, and the docs (D-8, accepted-diffs) state: "differs
      from what the FPGA would do with this ELUT; hardware would bin all
      in `ebin` 31".
    - Why "degenerate" and not only "all zero": the boundary table the
      FSW keeps in its RAM is all zero, but the FPGA ELUT the model sees
      is all **31** after `applyELUT`. A literal all-zero test would
      never fire in the gates and the replay would land entirely in
      `ebin` 31. A constant row cannot be a real calibration, so treating
      it as "no ELUT" is safe.
    - The rows are classified at each SetEnable 0→1 edge and at each QL
      swap (a 1.5 MiB scan every 4 s, only while `det-source=replay-dss`),
      so a TC(237,8) apply during NOMINAL takes effect within one period.
  - **Clock:** replay time = `det-replay-offset` + virtual time since the
    start, advanced per tick; each tick feeds the table entries whose
    `t_tick` falls in it through `stix_det_add_counts`. The start is
    `det-replay-trigger=nominal` (first SetEnable 0→1 after reset) or a
    QMP `det-replay-start=true`, never boot. At the end of the file the
    replay stops (counts zero); no loop.
  - **Gain 1×, fixed** (no gain knob). At 1× the busiest `fpga_det` sees
    about 72 counts per 2 QL periods, far below QLDetPerf's 300, even
    with the median at 0 because 25 detectors are dark. Above about 4× the
    lit detectors would go noisy.
  - Replay **replaces** the synthetic sky (`det-source=replay-dss`).

## Commits

Each commit writes its qtests first. They must be red on the parent
binary at their first new assertion, and green after.

### D-1: FSM strobes, live fsmSta, QL swap and IRQ 3, register reset (tier 0)
- `stix_det.c/.h` (D-Da), the fsmCmd/fsmSta/fsmEvents/testPulseCmdSta
  semantics (D-Dc), the 100 ms tick, the bank lifecycle, the LVAR slot
  rule and IRQ 3 (D-Dd), the register reset values (D-Dh), the
  re-entrancy rule (D-Dj), `det-model`, and the trace events
  `stixdet_fsm_strobe`, `stixdet_ql_swap` and `stixdet_tick_arm`.
- qtests:
  - `det-fsm-strobes`: bits 8:0 read 0; SetEnable/ClearEnable/Reset/
    QlClear act as D-Dc says (including what Reset and ClearEnable do to
    QlReady and SUMCNT); config bits round-trip; a `|=` RMW does not
    re-fire ClearEnable.
  - `det-ql-swap`: with QlIntTime = 39, after `clock_step(4 s)`
    ActiveQl = 1 and QlReady = 1. Four swaps wrap to 0. QlIntTime = 9
    gives a 1 s period. A mid-period change applies at the next period.
    Enable = 0 means no swap.
  - `det-ql-irq`: an IRQMP pending bit 3 on a swap only with QlIrqEn.
    QlClear clears QlReady. No pulse without a swap.
  - `det-ql-banks` (the lifecycle of D-Dd): after a swap, write a marker
    into bank (ActiveQl−1) & 3; it survives the next two swaps and is
    zeroed on the third (when that bank becomes active again). With
    QlIntTime = 39 and with **QlIntTime = 99**, **0xE02000A8 and
    0xE02003C0 are unchanged** after four swaps. `ahb-windows` and
    `bram-persist` stay green.
  - `det-testpulse`: Busy reads 1 and QuarterSeu 0, even after a
    write of 0xF0000000.
  - `det-reg-reset`: write every register 0x00-0x4C, `system_reset`,
    read back the D-Dh reset values; `hk-adc` DET_C reads 20 after it.
  - `det-model-off`: raw store and no register reset, as before.
- Goldens: the UART golden must stay identical. The trace golden is
  regenerated for manual comparison (fsmCmd 0x2/0x1002/0x1802 now trace
  as 0x0/0x1000/0x1800, plus the fsmSta reads and the new `stixdet_*`
  lines, which `-trace 'stix*'` includes).
- Gates: the full Phase 4 set.
  - nominal-smoke positive: unchanged [L] (the QL task now runs every
    4 s in NOMINAL; QL TM stays queued).
  - nominal-smoke `--negative`: TM(21,6) 0x5DC SSIDs 30, 31, 33 and 34
    appear after `0021 05 02`, from the flush, plus SSID 32 if NOMINAL
    lasted ≥ 32 s (the flush sends only products with samples,
    `TMForm.c:720-760`) [L]. Not forbidden; recorded.
  - fault-smoke: the register reset changes nothing visible [L] (the boot
    writes quarter power 0 anyway; DET_C stays 20).
- accepted-diffs §31.

### D-2: IDeF-X interface, register store and index tables (tier 1)
- D-De in full: CmdReady, the decoder, the 32×13 store, read-back, the
  cmd 1 context answers, delivery gating, the default and RD/SC resets,
  `det-idefx`, and the traces `stixdet_idefx_cmd(grp, word)` and
  `stixdet_idefx_reg(fpga_det, reg, …)`. **CmdReady and the store are
  one commit** (§24 trap).
- The index conversion table set ("Index spaces") lands here, with the
  realize-time self-check, because the decoder needs `grp`/`asic` →
  `fpga_det`.
- qtests (they drive the exact FSW word sequences):
  - `det-idefx-ready`: reset reads 0x80000000; CmdReady stays 1 after
    any command.
  - `det-idefx-roundtrip`: every register 1..12 on both `asic` of two
    `grp`. R1 = 0x01 reads back 0x40, R7 = 0x15 reads back 0xA8; the
    24 bytes of R3 come back in order; R4 and R12 round-trip as 32-bit;
    a write of more bytes than the length keeps the last N bits.
  - `det-idefx-broadcast`: `asic` = 2 (All) writes both ASICs.
  - `det-idefx-reset`: `0xBFF30` restores the defaults; so do a
    `quarter`-power 0→1 edge and `system_reset`.
  - `det-idefx-gating`: a disabled `grp`, ReadMode = 1 or an unpowered
    `quarter` ignores the command; CmdReady = 1; the response is 0.
  - `det-idefx-adc`: the `readTemp` sequence gives A then B with A ≥ B;
    an `adcRead` cmd 1 gives 1850.
  - `det-idefx-off`: `det-idefx=off` reads CmdReady 0 (the legacy
    behaviour).
- Goldens: the UART golden must stay identical [L]. Boot `resetReg` now
  succeeds silently. The trace golden is regenerated for manual
  comparison: the 16000 polls of 0x0 become 16 × 3 writes with their
  polls. The capture is time-bounded (90 s), so removing lines does not
  shift a line window.
- `nominal-smoke.sh`: the IDeF-X count becomes **0 expected**. A
  non-zero count is now a FAIL (it was a WARN with 384). The script header
  and §23 note are updated.
- Live check before D-3 expectations: NOMINAL entry has no
  `XrayDetFDIR.c:731` lines [L].
- accepted-diffs §32.

### D-3: detector-smoke gate (tiers 0+1), probe dump
- `tmtc-probe.py --dump-apid APID:FILE` (appends whole CCSDS packets
  without the 4-byte SpW header, hex, one per line, so the SSID is byte
  16), with a self-test.
- `scripts/stix/ql-check.py`: decodes the dump per "QL TM decode for the
  gates": groups packets by SSID, reassembles FIRST/CONT/LAST segments,
  tolerates an empty termination packet, decodes S0 bytes with the M = 3
  formula, and prints per-SSID sample counts and per-sample values. It
  has its own self-test on hand-built packets, including the worked
  example (2160 → 0x48).
- `scripts/stix/detector-smoke.sh`: built like nominal-smoke, with QMP
  like fault-smoke, and `-trace stixdet_idefx_cmd` into its own trace
  file.
  - Run dir `STIX_DETECTOR_RUN_DIR` (default `/tmp/stix-detector-run`),
    the same guard, a fresh flash, `--hold`, `--dry-run`,
    `--check-guard`, `--negative`.
  - Timeline (seconds from connect):
    - connect: the nominal-smoke group (0x0022, 0x8030, 0x14A3, 0x142E).
    - @50 TC(236,1) `04 00` (`TC_CONF`): TM(5,1) `0021 02 04`.
    - @60 TC(236,6) `07 04 00 00` (`TC_S20`): TM(1,7).
    - @62 TC(236,6) `02 02 00 1E` (verification period 30 s): TM(1,7).
    - @70 TC(236,1) `05 00` (`TC_NOM`): TM(5,1) `0021 04 05` within 10 s.
      Call the time it is seen T_nom.
    - Forbidden from NOM onward:
      - 0x542E, 0x9438 and 0x142C (any verification mismatch);
      - 0x9433 and 0x1427 (SEU);
      - 0x942E and 0x5437 (noisy detector);
      - `0021 05 02`, TM(1,8) and 0x142E.
    - @215 TC(236,1) `04 00` (a new sequence count and CRC): TM(5,1)
      `0021 05 04` [L] within 10 s, and the exit flush of **all five** QL
      products within 15 s: `0x5DC:21:6:1e`, `:1f`, `:20`, `:21`, `:22`.
      NOMINAL lasts about 140 s, well over the 32 s SSID 32 needs.
    - Hold to 240 s.
  - **Flush content** (`ql-check.py`; tiers 0+1, so the sky is zero):
    - SSID 30: about (215 − T_nom)/4 samples [L ±2]; every band, trigger
      and RCR byte 0; header fields at their defaults (integration time 39,
      det mask 0xFFFFFCFF, pixel mask 0x0FFF, SKM 0x2B, edges 0x10821082,
      E = 5);
    - SSID 31: about (215 − T_nom)/8 samples, all 0;
    - SSID 32: exactly one dump: 32 samples whose detector bytes are
      0..31 once each, all count bytes 0;
    - SSID 33: about (215 − T_nom)/4 values, all 0, samples-per-value 40;
    - SSID 34: about (215 − T_nom)/4 status triplets, all 0.
  - The UART must contain "init: SAFE mode", must have **no
    `XrayDetFDIR.c:731`**, and must have none of "packet sequence
    error", "RotBuf overrun", "UNKNOWN RB-BLOCK" or "TOO MANY RB-BLOCKS".
  - **Verification evidence (trace):** at least 3 clusters of 384 read
    address blocks (`0x84000|reg<<7`) in the NOMINAL window (+~80 …
    +210). The first may land within 1 s of the TC (the FDIR counter
    resets on each fire).
  - **`--negative`** runs with `-global grlib-stixctrl.det-idefx=off`.
    No TM fault is expected: a read timeout is not a mismatch (Key facts).
    It must fail exactly on the UART check (384 `XrayDetFDIR.c:731`
    lines plus the "failed to read register" TRACE_ERRORs) and on the
    trace check (no IDeF-X command words at all; the trace is empty).
    The TM forbids must stay quiet.
- Gates: the full set plus detector-smoke, positive and negative.
- accepted-diffs §33 (gate record).
- **As built (2026-11-11, accepted-diffs §33):**
  - the trace check is `scripts/stix/idefx-trace-check.py` (with a
    self-test) on `-msg timestamp=on` +
    `-trace enable=stixdet_idefx_cmd,file=<run>/idefx.trace`; clusters
    split at a > 5 s gap, each exactly 384 (every register on every
    `grp` 0..15 twice), inside [T_nom − 2, T_exit + 2] s; the negative
    requires the trace present and empty (`--expect-none`), so a
    missing trace fails it;
  - SKIP exits 77 (new-gate convention);
  - ql-check: SSID 34 offset 7 is `delta_time_seconds` (FSW
    `deltaTimeSeconds`); the APID-wide sequence counter makes the dump
    contiguous, not a product's segments (they interleave with other
    SSIDs), so the dump is checked for gaps and each group for
    increasing counts;
  - the flush sample counts are checked against T_exit − T_nom (both TM
    arrival times), ±2;
  - `--negative` passes only on the failed set {uart-idefx,
    trace-verif}, with exactly 384 `:731` lines and "failed to read
    register" lines 12 × k, k ≥ 3 (observed 48, 4 runs);
  - the plan's `--hold` is the probe's; the script has no hold flag;
  - observed: 4 verification runs at T_nom +20, +51, +84, +115 s (the
    first counter fire after the +62 s TC came in CONF and was skipped,
    `XrayDetFDIR.c:310-315`); flush order SSID 33, 30, 31, 34, 32, all
    standalone, 35/17/32/35/35 samples for a 139 s NOMINAL, all zero.

### D-4: synthetic sky and the flare switch (tier 2)
- D-Df: the template, PRNG, Poisson, per-tick gating and exposure; the
  materialization of LACC/LTRG/LVAR; live SUMCNT; `det-source`,
  `det-flare`, `det-quiet-cps`, `det-flare-cps` and `det-seed`; the
  trace `stixdet_scene`.
- qtests:
  - `det-counts-zero`: `det-source=zero` gives an all-zero finished bank
    after a swap (consistent with the D-Dd lifecycle).
  - `det-counts-quiet`: the bank sum ≈ 32 × 20 × 4 × the `pix` and
    `ebin` fractions, within 5 σ; the per-`fpga_det` spread is below the
    QLDetPerf limit; `ebin` 18/19/27 dominate.
  - `det-counts-flare`: `qom-set det-flare true` at run time gives a
    100× bank sum at the next swap and a thermal dominance in `ebin` 1-10.
    False brings it back.
  - `det-counts-gating`: `quarter` off, `grp` disable, ReadMode and a
    TH = 63 `pix` each give zeros where expected; a ReadMode pulse of
    one tick removes only about one tick of exposure (per-tick gating).
  - `det-counts-seed`: the same seed and steps give identical banks.
  - `det-lvar-ltrg`: slots 0..39 filled, masks applied, slot sum ≈ masked
    LACC sum; LTRG ≈ 1.25 × events; the clamps; big-endian layout.
  - `det-props`: out-of-range rates and unknown sources are rejected;
    the scene survives `system_reset`; a reset mid-period leaves no
    pending timer.
- detector-smoke gains the flare step:
  - @75-135 quiet;
  - @135 QMP `det-flare=true`;
  - @195 `det-flare=false`;
  - @215 CONF as before.
  - `ql-check.py` decodes the dumped SSID 30 (band 0 at rel. 24). The
    LC band-0 bytes of the flare samples (after +140) must have a median
    at least 8 compressed steps above the quiet median (before +130) [L].
    Expected [I]: quiet band 0 ≈ 20 cps × 30 `subcol` × 4 s × 5.5 % ≈ 130
    → 0x28; flare ≈ 2000 × 30 × 4 × 64 % ≈ 150 k → 0x79. No LACC cell
    comes near the u16 wrap (largest flare cell ≈ 200 per period). All
    RCR bytes must be 0.
  - The flush content of D-3 changes to: SSID 30-34 non-zero; SSID 32
    (first 32 s, quiet) non-zero for all 32 `subcol`, dominated by `ebin`
    18/19/27; SSID 31 (BKG = `fpga_det` 10, large `pix`) non-zero.
  - SSID 34 flags in the flare window are logged; an assertion follows
    only after a live look [L]. (Flare detection uses `ebin` 1-6 thermal
    and 17-22 non-thermal medians over `subcol`, `QLFlareDet.c:99-160`.)
  - `--negative` without the QMP flip must fail only the LC contrast
    check.
- Performance: QEMU user+sys CPU of detector-smoke with
  `det-source=zero` vs `synthetic`. Budget +10 %; recorded.
- Gates: the full set (nominal-smoke now runs with quiet counts [L]).
- accepted-diffs §34.
- **As built (2026-11-11, accepted-diffs §34):**
  - the scene properties are added by `stix_det_class_init(klass,
    offsetof(STIXCTRL, det))` and defaulted in `stix_det_instance_init`
    (so `-global` and a `qom-set` at `-S` work); `det-seed` is qdev;
    the traces are `stixdet_scene` (with the virtual time) and
    `stixdet_ql_sky` (events per finished bank);
  - counting needs the `grp`'s ReadMode bit **1** (readout; the FSW sets
    0xFFFF at NOMINAL entry, STIX-FSW 600a7a72 `OpModeMgmt.c:513`) besides quarter power,
    Enable and R3 TH < 63; R3 TH of `asic_ch` c is the 6-bit field c,
    MSB first, of the raw wire bytes (= the FSW's
    `utils_getReg3(utils_reorderThreshold3(raw))`);
  - LTRG = (5 × events + 2) / 4 per `grp` (1.25×, rounded);
  - **deviation:** the D-3 modes of detector-smoke keep the zero sky
    (`-global grlib-stixctrl.det-source=zero`) and their checks; the
    flare step is the new `--flare` mode, and `--flare --negative`
    (det-flare false @135) must fail exactly `ql-contrast`. The plan's
    "`--negative` without the QMP flip" is that mode;
  - `det-seed` is settable only with `-global` (seeded at realize and
    reset), an explicit exception to D-PLm req. 5. A scene switch
    mid-period acts from the next tick; exposure already gathered is
    still drawn at the swap;
  - **observed levels are 0.475 of the [I] estimates above**, because
    of an FSW bug (STIX-FSW issue #1069). The NOMINAL-entry R3 (RCR
    state 0, STIX-FSW 600a7a72 `RateCtrl.c:575, 898-913`) is mirrored
    twice: the local channel inversion, commit 44e8526b, on top of the
    global R3 reorder, commit 60e69d75. It enables 4 fixed pixels
    (`asic_ch` 1, 5, 26, 30 = `pix` 3, 6, 0, 8). `pix` 7 (`asic_ch` 0)
    depends on an out-of-bounds read of the active-pixel list at index
    12. Ratio 0.475 with `pix` 7 on (this run), 0.363 with it off.
    Quiet LC band 0 is about 0x1F (≥ 60), flare about 0x70 (≥ 65536).
    The `--flare` thresholds hold in either case; a fixed FSW (×2.1 to
    ×2.8) may raise RCR at 2000 cps, so re-calibrate then
    (`det-counts-r3-fsw` shows all 12 `pix` count with a correct R3
    build).

### D-5: nominal integration and ROTB chunks (tier 3)
- D-Dg: the swap rule, the EACC/ETRG materialization, the chunk writer,
  drop-whole-chunk plus IRQ 7, `det-rotb`, and the traces
  `stixdet_nominal_swap` and `stixdet_chunk` / `stixdet_chunk_drop`.
  - The ROTB append shares the §12 ring helpers. PUTBLOCK in TESTMODE is
    unchanged.
- qtests:
  - `det-chunk-format`: decode the 49 blocks as `RotAccuCompactor` does
    (descriptor, sequence, timestamp, data at +8 in every block, payload
    = EACC bank `[pix][fpga_det][ebin]` + ETRG by `grp`, 56 B pad). The
    decode must equal what was materialized.
  - `det-nominal-rule`: min/max/events with SUMCNT from the quiet and
    flare rates (a 20 s vs 1 s period); params latched per integration.
  - `det-chunk-gating`: no chunk in TESTMODE, with ENABLE = 0 or with FSM
    Enable = 0; the swap still happens.
  - `det-chunk-full`: `rotb-blocks=60` → one chunk fits, the second
    drops whole, one IRQ 7, BLOCKCOUNT stays 49.
  - `det-chunk-sumdmask`: a masked `fpga_det`'s EACC is 0 while LACC is
    not.
  - The existing `rotb-*` tests stay green.
- detector-smoke gains a trace-based check (`-trace stixdet_chunk
  -trace stixctrl_rotb_cmd`):
  - chunks in quiet come about every 20 s, and in the flare about every
    1 s;
  - GETBLOCK count ≥ 49 × (chunks − 1), i.e. the FSW drained the ring;
  - **positive UART evidence:** "Skipped first Det data chunk"
    (`ArchiveManager.c:140`) appears once;
  - the UART forbids already listed hold, plus no archive or filesystem
    error on the fresh flash [L].
- nominal-smoke: "Skipped first Det data chunk" now appears there too;
  it is allowed, not forbidden.
- No science TM is checked: SSID 20-24 (APID 0x5BC) need a TC(238,x)
  user request. Layout trap for a later gate, recorded now: the chunk
  carries `[pix][fpga_det][ebin]`; the compactor converts `fpga_det` →
  `subcol` (`RotAccuCompactor.c:298-300`) into the archive word
  `cont[15:14] | pix[13:10] | subcol[9:5] | ebin[4:0]`, while the L0 TM
  sample word is `pix[15:12] | subcol[11:7] | ebin[6:2] | cont[1:0]`
  (ICD Table 94, `SciReport.c:291-325`). A D-6 deposit at `fpga_det` F
  must show as `utils_subcolIdFromFpgaId[F]` in either word.
- Performance as in D-4, **plus the emulated NAND cost**: a flare makes
  about 1 chunk/s, compacted to about 35-40 kB (≤ 49216 B), i.e. a 256 KB
  flash block about every 7 s. Record host CPU and flash growth in
  `STIX_DETECTOR_RUN_DIR`.
- accepted-diffs §35.

### D-6: index-space end-to-end check
- `det-inject` (D-Db) through `stix_det_add_counts`, with the trace
  `stixdet_inject(fpga_det, pix, ebin, n)`.
- qtests:
  - `det-inject-lacc`: for several (`fpga_det` F, `pix` P, `ebin` B),
    inject N, swap, and assert the big-endian u16 at LACC byte offset
    ((P·32+F)·32+B)·2 of the finished bank, and zero at the neighbours
    (F±1, P±1, B±1);
  - `det-inject-chunk`: the same counts appear at the matching payload
    offset of the next chunk;
  - `det-inject-props`: missing keys, unknown keys and out-of-range
    values (`fpga_det` > 31, `pix` > 11, `ebin` > 31) are rejected.
- detector-smoke: at T_nom + 8 s (inside the first 8 QL periods, which
  SSID 32 sums) inject 250 counts at `fpga_det` 5, `pix` 0, `ebin` 20.
  `ql-check.py` asserts, in the one SSID 32 dump of the exit flush:
  - the sample whose detector byte (0-based `subcol`, ICD Table 105
    [D=C]) equals `utils_subcolIdFromFpgaId[5]` has byte 21 (= B+1) ≥
    0x2F (S0 of ≥ 250; the quiet background there is about 5 counts);
  - byte 21 of every other sample is < 0x10, and the other bytes of the
    target sample match its neighbours' range [L].
  - QLDetPerf stays quiet: per 8 s the target gets about 160 + 250 = 410
    counts in `ebin` 1-30, above Cbad 300 but below 5 × median ≈ 800, and
    it happens once (Nrep 15).
  - The deposit also shows as a one-sample spike in SSID 30 band 3
    (`ebin` 17-22) if that `subcol` is in the LC mask; logged, not
    asserted.
- Gates: the full set.
- accepted-diffs §36.

### D-7: DSS `.dssevs` replay
- D-Dk: the pre-scan at realize, the event table, the ELUT lookup with
  the linear non-physical fallback for degenerate rows (trace
  `stixdet_elut_fallback`), the trigger/offset clock, and the
  properties `det-replay-file`, `det-replay-trigger`, `det-replay-offset`,
  `det-replay-start`, `det-source=replay-dss`; traces
  `stixdet_replay_scan` and `stixdet_replay_state`.
- qtests (with a **small synthetic `.dssevs` generated by the test** in a
  temp dir, never the user file):
  - `det-replay-decode`: types 0x0-0xB counted as events with
    `pix` = type, 0xD/0xE as setup (baseline taken from 0xE), 0xF as wait
    only; waits sum to the expected time; `fpga_det` = `dss_det`.
  - `det-replay-elut` (pins both branches; the qtest writes the ELUT
    rows into 0xE0000000 + (pix·32 + fpga_det)·4096 directly):
    - **honoured:** a non-degenerate row (e.g. `ebin` = `adc`/128, with
      32 for `adc` < 256) bins replayed events exactly as the row says;
      entries of 32 drop the event; no fallback trace;
    - **fallback:** an all-0 row and an all-31 row both give the linear
      binning: `adc` = base + k·65.5 lands in `ebin` k, `adc ≤ base` is
      dropped, and `stixdet_elut_fallback` fires once per row;
    - rows are independent: one honoured and one fallback row in the same
      run each behave as above;
    - a row rewritten between two swaps switches branch at the next swap.
  - `det-replay-trigger`: with trigger `nominal` nothing arrives before
    SetEnable and the first events arrive in the first tick after it;
    with `qmp`, only after `det-replay-start=true`; the offset skips the
    events before it; end of file stops; `system_reset` stops and re-arms.
  - `det-replay-missing`: a missing file is a realize error naming the
    path; `det-source=replay-dss` without a table is rejected.
- **The one optional replay gate:** `detector-smoke.sh --replay`, file
  from `STIX_DSSEVS_FILE`. It **SKIPs** (exit 77, message) when the
  variable is unset or the file is absent; the full gate set treats
  SKIP as pass. The file and anything derived from it are never
  committed or copied into the run dir's committed artefacts.
  - Timeline: as D-3 to @62; @66 QMP `det-source=replay-dss`,
    `det-replay-trigger=nominal`, `det-replay-offset=860`; @70 NOM (the
    replay starts at SetEnable, a few seconds before T_nom; the file's
    quiet→flare step lands about 40 s later); no flare switch, no inject;
    @215 TC(236,1) `04 00`.
  - Checks:
    - the trace shows `stixdet_elut_fallback` for the lit rows (empty
      flash: `applyELUT` wrote all 31), so the linear fallback is in use;
    - SSID 32 (the first 32 s after T_nom, inside the file's quiet part,
      about 180 events) has non-zero count bytes only in the samples whose
      detector byte (0-based `subcol`, [D=C]) is in {14, 9, 15, 7, 8, 2, 3},
      and only in `ebin` 2-28;
    - SSID 30 LC contrast over the **5** lit detectors in the LC mask
      (`subcol` 8 and 9 are excluded): decode every band with the S0
      formula and sum the bands per sample; the median after T_nom + 50
      must be ≥ 4× the median before T_nom + 35 [L]. Expected [V/I]: quiet
      ≈ 5.6 × 5/7 × 4 ≈ 16 per sample, flare ≈ 62 × 5/7 × 4 ≈ 177;
    - SSID 31 (BKG = `subcol` 9 = `fpga_det` 10, lit) non-zero; logged;
    - the D-3 forbids, **including 0x942E/0x5437** (gain 1×; 25 dark
      `fpga_det` give a median of 0 but the lit ones stay below 300 counts
      per 2 periods);
    - pre-scan time and table size from `stixdet_replay_scan` are
      recorded.
  - About 240 s.
- accepted-diffs §37.

### D-8: docs
- ground-station.md "Detector scene": the QMP knobs (flare, inject,
  replay), the flare recipe and the replay recipe (file stays local).
- The index-space table ("Index spaces") copied into the ground-station
  doc, as the single reference for TM detector and pixel indices.
- accepted-diffs §38: the known limits after the detector phase.
- The detector checklist (this file's "As built"), and the GS-repo line.

## Goldens: what changes, when, and why

| Commit | `uart.golden.log` (gated) | `qemu-stdout.golden.log` (manual reference) | Why |
|---|---|---|---|
| D-1 | unchanged, required | regenerate | the stored fsmCmd values lose their strobe bits [G: 0x2/0x1002/0x1802]; new `stixdet_*` trace lines; SAFE never sets Enable, so there is no IRQ 3 and no UART line |
| D-2 | unchanged, required [L] | regenerate | the boot `resetReg` × 16 now completes: 16000 polls become about 48 writes plus polls. Its result is never logged (`OpModeMgmt.c:1061-1064`) |
| D-3 | unchanged | unchanged | scripts only |
| D-4 | unchanged | unchanged, checked | the counts exist only while Enable = 1 |
| D-5 | unchanged | unchanged, checked | chunks need FSM Enable |
| D-6 | unchanged | unchanged | inject is QMP-only |
| D-7 | unchanged | unchanged | `det-replay-file` empty by default; nothing scanned |

- Regenerate with `capture-golden.sh` on the normal ELF and the shared
  run dir, as in §22. The capture is bounded by `timeout 90`, not by
  line count, so the D-2 removal of 16000 lines does not pull later
  content into a window.
- **A UART golden diff in any commit is a stop-and-report**, not a
  regeneration. The only legitimate cause would be a boot-time detector
  path we have not found.
- `tm.golden.log` stays a historical artifact (§22).

## Gate impact

- **boot-smoke:** unchanged at every commit (see the table).
- **soak 300 s and tc-smoke:** SAFE only, so the detector stays off. No
  change is expected. They run per commit.
- **fault-smoke:** SAFE only. Its forbids are TM(1,2), TM(1,8), the
  0x9434 ids other than DET_C, 0x9449 after +100 and 0x9434 after +95.
  - DET_C (ch 4) stays `20 + 75 × quarters`, which is 20 in SAFE, and
    still reads `s->reg.idefxConfig` (D-Da), which the D-1 reset now
    clears (the FSW sets it to 0 at boot anyway).
  - No QL IRQ, no verification (4096 s) and no chunks happen in SAFE.
  - **No expectation is at risk.** D-2 makes the boot `resetReg`
    succeed, which is invisible.
- **nominal-smoke:**
  - D-1 makes the QL task run in NOMINAL. The positive mode stays in
    NOMINAL, so QL TM stays queued and nothing new is sent [L]. The
    negative mode adds the QL flush TM after `0021 05 02` (not
    forbidden).
  - D-2 changes the IDeF-X count from 384 to 0, so the script changes.
  - D-4 adds quiet counts. The forbids (`0021 05 02`, TM(1,8), 0x142E,
    0x9449) are unaffected unless the RCR or QLDetPerf react (they
    should not at 20 cps [I]).
  - D-5 adds "Skipped first Det data chunk" to its UART (allowed).
- **probe self-test:** extended for `--dump-apid` in D-3.
- **detector-smoke:** new in D-3, extended in D-4, D-5 and D-6; the
  optional `--replay` mode in D-7.

## Risks and checks

- **The gate ELF vs the source.** Every [L] and [I] item, and every
  EID or payload, is probed live before it becomes an expectation (as in
  Phases 3 and 4). In particular:
  - the NOMINAL→CONF event bytes `0021 05 04`;
  - that SSID 30-34 are sent at the CONF flush;
  - the SSID 32 sample layout for the D-6 and D-7 checks (detector byte
    0-based `subcol`, count byte B+1, per ICD Table 105 [D=C]; confirmed
    on the first live dump);
  - when SetEnable happens relative to the `0021 04 05` event (it sets
    T_nom-relative windows).
- **The LVAR/BRAM overlap.** A bank zero or fill that runs past 0xA0
  would destroy the WDT `UNCHANGED_STORAGE` (+0xA8) or MemBram (0x3C0).
  The slot rule (D-Dd) stops at slot 39; `det-ql-banks` covers
  QlIntTime 39 and 99.
- **Register state across reset.** Today `s->reg` survives a reset; D-1
  adds explicit reset values. `det-reg-reset` covers it; fault-smoke is
  re-run at D-1.
- **A bad chunk crashes the FSW.** `RotAccumulator_getType` does
  `ASSERT(0)` on an unknown type or a block number above 48. The chunk
  writer is tested against the FSW's own decode (`det-chunk-format`).
- **Index-space confusion.** Mixing `fpga_det`, `subcol`, `fe_sc` and
  `pix`/`asic_ch` would put counts on the wrong detector or pixel without
  any FSW error. Mitigation: the naming rule, one conversion table set
  with a self-check, and the D-6 end-to-end check through the real TM.
- **The RCR and the attenuator.** Rates above about 17 k cps per
  `fpga_det` make the RCR insert the attenuator, and that path is not
  modelled (ATT_C is static, §22). Docs state the limit. The property
  allows up to 1 M cps for experiments, at the user's risk.
- **The QLDetPerf FDIR** (0x942E/0x5437) fires on a non-uniform sky.
  The synthetic template is uniform, the D-6 inject is one-shot and below
  the thresholds, and the replay gain is fixed at 1×. Every gate forbids
  both EIDs.
- **Dark detectors in replay.** 25 of 32 `fpga_det` stay dark. Whether any
  FSW path other than QLDetPerf reacts to dark detectors is checked live
  in the first `--replay` run [L]; the forbid list catches the known EIDs.
- **ELUT branch confusion.** If the fallback test were "all zero" only,
  the replay would bin everything into `ebin` 31 (what `applyELUT` writes
  on an empty flash) and the LC contrast check would see nothing. D-Dk
  therefore falls back on any constant row; `det-replay-elut` pins both
  branches, and the replay gate asserts that the fallback trace fired.
- **The synthetic sky is not what hardware would show on an empty flash**
  (all `ebin` 31). This is a deliberate, recorded deviation (Doc vs code
  20); a gate that compares against real-HW QL with an empty flash must
  not use the synthetic sky.
- **QL TM segmentation.** A product can span packets, and a flush can end
  with an empty termination packet; `ql-check.py` reassembles by SSID and
  has a self-test for both.
- **Replay start timing.** A start at boot would put the file's step
  outside the gate window; the trigger/offset design avoids it, and
  `det-replay-trigger` is tested.
- **Long QL TM latency.** QL TM leaves only at a NOMINAL exit or when a
  packet fills, so the gate depends on the CONF flush, which is
  unconditional (`OpModeMgmt.c:282-306`). (The attenuator check is on the
  SAFE path and only adds a 3 s wait, `:173-177`.)
- **Flash growth and NAND cost.** NOMINAL archives the chunks to flash,
  about 1 chunk per second in a flare (a 256 KB block about every 7 s).
  Each gate uses its own fresh run dir with the nominal-smoke guard. The
  shared flashes are never touched. D-5 measures the cost.
- **Determinism.** No icount. The gates compare statistics and orderings,
  never exact counts.
- **TC-driven procedures stay unmodelled:** calibration (CalReady never
  sets), test pulses, threshold scans (which read live EACC/ETRG) and the
  ADC reads (canned answers). They are known limits and not gate paths.
- **A watchdog reset in NOMINAL.** D-Dh stops the tick and resets the
  registers, and the FSW re-inits in SAFE. `det-props` checks that a reset
  mid-period leaves no pending timer.

## Doc vs code

Code wins in each case. The model follows the column marked ✔.

1. **CmdReady wording (clarification, not a conflict).** The HSWICD calls
   bit 31 a "busy bit, active low", reset 1. Active-low busy means
   1 = idle = ready, which is what the FSW does (`Detectors.c:627-639`).
   ✔ 1 = ready, reset 1. The TSIM model does the same.
2. **The QL buffer count.** TN-0041 Table 6-8 says QLACC/QLTRG/QLVAR are
   "double buffered (0,1)". The FSW switches over 4 banks
   (`Detectors.c:144-166`), as does the ICD (LACC0-3). ✔ code: 4.
3. **The ROTB size.** TN-0041 says 104857 blocks. The ICD and the FSW
   `ROTB_MAX_BLOCKS` say 104448 (`StixIdpu.h:26`). ✔ 104448 (unchanged,
   §12).
4. **WriteMask.** The ICD's WriteMask description is a copy of
   ReadMode's. The FSW uses it as the destination-`grp` mask
   (`Detectors.c:657-660`). ✔ code.
5. **ETRG address.** RP-0067 §3 puts BRAMTRIG at 0xE0800000 (the ROTB
   window). The FSW header and the CacheCtrl table say 0xE0600000. ✔
   0xE0600000 (unchanged, §13).
6. **When a QL bank is cleared.** RP-0067 says RotbCtrl reads and clears
   the finished set after each swap, with "QL only cleared". The FSW
   reads the **finished** bank after the IRQ (`Detectors.c:144-168`,
   `QLAccuDblBuffs.c:162`). An immediate clear would hand it zeros.
   ✔ code: the bank is cleared when it becomes active again (D-Dd).
7. **ROTB overflow.** RP-0067 says the hardware overwrites the oldest
   block and pulses IRQ 7. The model (§12, D-F) drops new data; D-Dg
   drops a whole chunk. The FSW's overrun handler skips by SCET
   (`RotAccumulator.c:100-125`) and copes with either. This is a model
   deviation, kept and recorded. It is unreachable in the gates.
8. **IDeF-X read-back.** The DSS manual never documents slow-control
   read-back. The FSW's ASIC verification depends on it
   (`XrayDetFDIR.c:596-690`). ✔ code: read-back is modelled.
9. **The boot register reset.** RP-0067 says commands to a disabled `grp`
   are ignored. The FSW resets the ASIC registers at boot right after
   setting the group enable mask to 0 (`OpModeMgmt.c:1057-1064`), so on
   hardware that reset is a no-op. ✔ the doc (the hardware rule), because
   the FSW never checks the result. This is flagged as a likely FSW
   intent bug and is invisible.
10. **The SeuIrqEn field values.** `StixIdpu.h` lists SEUIRQEN_V_Q3/Q4 as
    256/4096 for a 4-bit field (a header codegen error). The FSW writes
    the raw quarter mask & 0xF (`Detectors.c:248-262`). ✔ code: bits
    23:20 = `quarter` 0..3.
11. **Integration-time units.** The ICD says (N+1)×0.1 s, up to 6553.6 s;
    TN-0041 says up to 6553.5 s. The FSW subtracts (period+1) tenths
    from its timestamps (`QLAccuDblBuffs.c:149`). ✔ (N+1)×100 ms.
12. **Undefined FPGA bit orders (assumption, not a conflict).** Neither
    the docs nor the code define the FPGA-side bit order of VARPMASK
    (`pix`), SUMEMASK and VAREMASK (`ebin`), or of R12 ALIMON and R4 per
    `asic_ch`. The FSW's own masks disagree with each other (QL: bit i =
    `pix` i; RCR: bit 11−i = `pix` i, `RateCtrl.c:711-719`). The model
    assumes **bit i = index i** [A]. All defaults are all ones, so this
    is invisible in the gates; nothing is gated on it, and pixel gating
    uses only R3 TH = 63.
13. **DSS script detector numbering.** notes-dss-app §6 item 3 says the
    DSS script sc ids map through `utils_fpgaIdFromSubcolId` (the new
    `subcol` numbering). The DSS app's own table equals
    `Detectors_fpgaFromFrontendMinusOne` (`Detectors.c:535-538`) and
    `utils_oldSubcolIdToFpgaId` (`utils.c:126-130`). ✔ code: the script
    "sc" is `fe_sc`, and `dss_det` = `fpga_det`.

Items 14-20 come from the TM/TC ICD (STIX-ICD-0812 I4R8 draft,
notes-tmtc-icd-detector §7). Several are FSW-internal oddities rather
than doc errors; the model follows the code and the gates expect the
code's behaviour.

14. **QL parameter TC.** The ICD TOC names it "TC(236,9)" (ICD:468). The
    ICD body and the code say TC(237,9) (ICD:20444, `TCData.h:229-330`).
    ✔ code: there is no service-236 QL TC.
15. **Background integration time.** The ICD says the BG default is 32 s
    (ICD:13056). The code uses nInt 2, i.e. **8 s** (`ParamMgmtDef.h:265`).
    ✔ code: the gates expect one SSID 31 sample per 8 s.
16. **SEU and noisy-detector EID parameters** (only relevant if they ever
    fire; all are forbidden in the gates):
    - 0x5429: the ICD says a detector mask; the code sends a **quarter
      mask** 0..0xF (`XrayDetFDIR.c:418-421`);
    - 0x9433: the ICD says a 1-byte detector ID; the code sends the **low
      byte of a `subcol` mask** (`XrayDetFDIR.c:289, 408`);
    - 0x942E: the ICD says a bitmask of every detector above the limit;
      the code sets **only the noisiest detector's bit** (a bug,
      `QLDetPerf.c:107-150`), and its "rate" fields are counts per 8 s
      check, not rates.
    ✔ code. A gate that decodes these parameters must use the code's
    meaning.
17. **SSID 31 triggers (FSW-internal).** The BG counts come from the BKG
    detector, `subcol` 9 = `fpga_det` 10, which is in `grp` 5 (ICD Table 33,
    `dete_data`). The BG triggers come from LTRG `grp` 4
    (`BCKG_ASSOC_GROUP`, `commonFSW.h:161`, `QLAccuDblBuffs.c:197`), which
    holds `subcol` 13 and 14. ✔ code; the model does nothing special, and
    no gate relies on BG triggers tracking the BKG detector.
18. **Variance slot count.** The ICD calls SSID 33 a variance over 40
    accumulators. The code reads 39 LVAR slots (intervals = QL period =
    39), divides by 40, and reports 40 (`QLVariance.c` calculate). ✔ code;
    LVAR slot 39 is unused at the default period (D-Dd).
19. **`QL_FLARE_ENABLE_TM`.** It exists (the TC(237,9) FD TM-enable byte,
    default FALSE, `ParamMgmtDef.h:316`), but SSID 34 is produced
    regardless. ✔ code: the gates expect SSID 34.
20. **No-ELUT behaviour.** The ICD does not describe an empty ELUT [D
    silent]. The code maps every `adc` to `ebin` 31 (`LUTMgmt.c:834-935`,
    run from `OpModeMgmt.c:580-597`); its comment "= 0 - 30" at
    `LUTMgmt.c:912` is wrong. ✔ code for what the FSW writes. **The model
    deliberately departs from hardware here** for ADC-domain replay
    (D-Dk fallback on a degenerate row), and the synthetic sky never
    uses the ELUT (D-Df). Both are recorded in accepted-diffs.

## Open questions for the user

1. **Flare strength.** The default flare (2000 cps per `fpga_det`) keeps
   the RCR at 0, which is gate-safe. Do you want an X-class preset now
   that drives the RCR? That needs an attenuator motion and ATT_C model
   first. Default: no, out of scope.
2. **ELUT fallback trigger.** The decision says "fall back if the ELUT is
   all zero". The FPGA ELUT the model sees after an empty-flash NOMINAL
   entry is all **31**, not all zero (the all-zero table is the FSW's
   boundary table in its own RAM). The plan therefore falls back on any
   **constant** row (all 0 or all 31 included). Confirm this reading.
   Default: constant row → fallback. **Confirmed 2026-11-11
   (coordinator):** a real ELUT never has a constant row, so a constant
   row means "no ELUT loaded".

Resolved 2026-11-11 (coordinator): the model honours a real ELUT for
ADC-domain sources and falls back to the user's linear binning otherwise,
with a trace warning and a doc note; synthetic `ebin`-space counts are
unaffected.

Resolved 2026-11-11 (user): replay is in this phase (D-7) from a
pre-scanned table with a trigger and offset, gain 1×, one optional gate;
the ADC→`ebin` fallback is linear above the file's baseline; undefined
bit orders are assumed bit i = index i and not gated.

## Out of scope (next-phase candidates)

- Aspect ROTB blocks; calibration (CalTq/CalCount/CalReady); test pulses;
  threshold scans with live EACC; FIFO overflow; SEU injection; a
  noisy-detector knob for the QLDetPerf FDIR; ALIMON-based pixel gating
  (needs the bit order).
- Replay of the RP-0119/TSIM per-period QL dumps (`qlacc`/`qltrg`/`qlvar`);
  an ADC-domain synthetic sky that goes through the ELUT.
- A science-TM gate: a TC(238,x) L0 request and a check that the D-6
  deposit appears as `subcol` `utils_subcolIdFromFpgaId[F]` in the L0
  sample word (layout in D-5).
- An attenuator motion model, which would allow an RCR-driving flare.
- The BRAM width truncation of FSW writes (§13 gap).
- A TM-level negative that reproduces the §24 trap (a
  `det-idefx-readback=zero` knob); see Validation, required 1.

## Validation (2026-11-11)

An independent validator checked the plan against the FSW
(`~/dev/STIX-FSW/ASW`, the paths are relative to it as above), the model
(`grlib_stixctrl.c`, `StixIdpu.h`, `leon3.c`, `tests/qtest/stix.c`,
`scripts/stix/*.sh`) and the gate ELF `/tmp/stix-baseline-run/ASW_em_baseline.elf`.
The ELF was read read-only. No sparc disassembler is installed, so it was
checked through its symbols (host `nm`) and its parameter records (a
python scan). Each record is 16 B: value, default, min, max. No QEMU run
was made.

### Required changes

1. **D-3 `--negative` cannot produce 0x542E or 0x9438.** With
   `det-idefx=off`, `Detectors_readReg` times out, so
   `XRayADCsCtrl_readASICReg` returns FSW_ERROR (`XRayADCsCtrl.c:699-703`).
   `verifyRegisters` then does `continue` without recording an error
   (`XrayDetFDIR.c:611-615`), and the run ends "all good"
   (`XrayDetFDIR.c:189-193`). No TM(5,x) is produced.
   - **Fix:**
     - The negative expects the UART `XrayDetFDIR.c:731` lines (384) plus
       the "failed to read register" TRACE_ERRORs, and no TM fault.
     - Or, if a TM-level negative is wanted, add an init-time knob (for
       example `det-idefx-readback=zero`) that reproduces the §24 trap:
       CmdReady 1, reads 0. That gives 0x542E, a rewrite, then 0x9438 and
       a permanent `CALISTE_IGNORE_FLAGS` update (`XrayDetFDIR.c:196-283`).
       Assert 0x542E and 0x9438 there.
2. **A successful ASIC verification is silent.** `ENABLE_TRACE_DETFDIR` is 0
   (`configFSW.h:89`), so the "All good" path prints nothing, and no TM is
   sent. "At least 3 verification runs" therefore has no evidence in the
   TM or the UART.
   - **Fix:** count them in the model trace. One run is 12 registers × 32
     ASICs = 384 read address blocks `0x84000|reg<<7` (`Detectors.c:817`)
     in `stixdet_idefx_cmd`, spread over ~1 s at a 30 s cadence. The FDIR
     counter resets on each fire (`FDIR.c:480-486`), so the first run can
     land within 1 s of the TC.
   - Assert ≥ 3 × 384 read address blocks in the NOMINAL window, or
     clusters ≥ 3.
3. **LVAR slot overflow into the WDT storage.**
   - LVAR has 40 slots (`Detectors.h:42` `Detectors_MAXINTTIME 40`; the
     table is 0xA0). QlIntTime is a 16-bit field (`StixIdpu.h`
     FSMQLCONFIG_QLINTTIME 31:16; `NOMINAL_QL_PERIOD` max 0xFFFF,
     `ParamMgmtDef.h:385`).
   - A QL period over 4 s would make "one slot per 100 ms tick" write slot
     ≥ 40. Slot 42 is `UNCHANGED_STORAGE` 0xE02000A8 (`WDT.c:74`).
   - **Fix:** D-Dd/D-Df state that the slot index stops at 39 (writes past
     it are dropped). The hardware behaviour is [D]/unknown: record it as
     a model choice.
   - Extend `det-ql-banks` with QlIntTime = 99 and check that 0xE02000A8
     and 0xE02003C0 are unchanged.
4. **The device reset does not touch `s->reg`.** `grlib_stixctrl_reset`
   (`grlib_stixctrl.c:957-1003`) never clears the register file. Phase 4
   learned the same lesson for hkProt.
   - D-Dh lists only internal state. Unless the reset handler sets them,
     the stored fsmCmd config bits (QlIrqEn, FifoIrqEn, CalTq), idefxConfig
     (quarter power, enables, SeuIrqEn), ReadMode, WriteMask, fsmQlConfig,
     masks and int params all survive a watchdog reboot. Also,
     ideFxCmdSta's "reset 0x80000000" never happens.
   - **Fix:** `stix_det_reset` writes explicit reset values for 0x00-0x4C
     (ideFxCmdSta 0x80000000, testPulseCmdSta Busy=1, all others 0) when
     `det-model=on`. Add a qtest: write the registers, `system_reset`,
     then read them back.
   - Keep idefxConfig in `s->reg`, or give stixctrl an accessor:
     `stixctrl_hk_value` reads `s->reg.idefxConfig & 0xF` for DET_C
     (`grlib_stixctrl.c:479`). There must be one source of truth.
5. **D-1 `det-ql-banks` contradicts D-Dd and D-4.**
   - D-Dd step 1 "materializes the finished bank" (zeros in tier 0) at the
     swap. That would overwrite a marker the qtest writes into the
     finished bank before the swap. D-4 `det-counts-zero` also expects an
     all-zero finished bank.
   - **Fix:** define it once. At the swap the model writes the finished
     bank in full (zeros with `det-source=zero`); after that the model
     does not touch the bank until it becomes active again, three swaps
     later.
   - The marker test writes into bank (ActiveQl−1) **after** the swap and
     checks that the marker survives two more swaps and is zeroed on the
     fourth. D-1 and D-4 must then agree.
6. **Index spaces (user theme).** The plan mixes FPGA, sub-collimator,
   DSS, pixel and channel indices without naming them. Here is the
   inventory, with the FSW source for each:

   | Space | Range | Where defined (FSW) | Used for |
   |---|---|---|---|
   | IDeF-X channel | 0-31 | `commonFSW.h:149`; R3 6-bit fields in SW order `utils.c:521-555`; SW↔wire order `utils_reorderThreshold3` `utils.c:663-677` (self-inverse) | R3 thresholds, R12 ALIMON, R4 |
   | Caliste pixel J | 0-12 (12 = guard ring) | `utils_pixelId2channelId` `utils.c:50-53`, inverse `utils.c:56-62`, `MASK_OF_PIXEL_CHANNELS` `utils.h:10-21` | LACC/EACC dimension 0 (0-11 only, `Detectors.h:39`) |
   | FPGA detector | 0-31 = 2·group + ASIC | `dete_data` `utils.c:74-107` (indexFPGA/groupFPGA/deteFPGA), `UTILS_GROUPID2SUBCOLID` `utils.h:125-132` | LACC/EACC `[pix][fpgadet][e]` (`Detectors.h:68-79`), the ROTB chunk payload order, the SUMDMASK/VARDMASK/fsmCalMask bits |
   | Group | 0-15 = fpgadet/2; quarter = group/4 | `StixIdpu.h` IDEFXCONFIG_ENABLE_V_QxGy; `dete_data` quarterId | IDeF-X GrpAddr, WriteMask, ReadMode, Enable, LTRG/ETRG index |
   | IDeF-X ASIC in group | 0/1 (2 = both) | `0xBFD30\|det<<6` `Detectors.c:747, 806, 868`; `UTILS_SUBCOLID2GROUPID_DETID` | slow-control addressing |
   | Sub-collimator (new) | 0-31 (TM: 1-32) | `utils_subcolIdFromFpgaId` / `utils_fpgaIdFromSubcolId` `utils.c:110-124`; masks converted by `utils_subcolMask2fpgaMask` `utils.c:222-232` | every ASW parameter mask, QL TM detector index (`readQL` `QLAccuDblBuffs.c:175`), the archive control word (`RotAccuCompactor.c:300-315`) |
   | Front-end / "old" sub-collimator | 1-32 | `Detectors_fpgaFromFrontendMinusOne` `Detectors.c:535-538` = `utils_oldSubcolIdToFpgaId` `utils.c:126-130`; `Detectors_frontendFromFpga` `Detectors.c:521-527` | calibration report, DSS scripts |
   | DSS det index | 0-31 | DSS app table (notes-dss-app §3) | `.dssevs` bits 15:11 |
   | Energy / science bin | 0-31 | LACC dimension 2 (ELUT output); LC bands from mask 0x10821082 = bins 1-6, 7-11, 12-16, 17-22, 23-27 (`ParamMgmtDef.h:254`) | QL TM, SUMEMASK/VAREMASK bits |
   | Pixel-mask bits | — | QL SW masks: bit i = pixel i (`QLLightCurve.c:289`, `QLSpectra.c:271`, `QLFlareDet.c:109`). **RCR masks are reversed:** bit 11−i = pixel i, bit 12 = attenuator (`RateCtrl.c:711-719`). FPGA VARPMASK: not defined in code | |

   **Mismatches and corrections:**
   - (a) The plan's pixel→channel list matches `utils.c:50-53`. The
     RP-0019 map (pixel:channel, including 12:31) matches it entry for
     entry; there is no difference.
   - (b) The "R3 bit position [I]" is **[C]**. The model stores raw wire
     bytes, so channel c's TH = `utils_getReg3(c, utils_reorderThreshold3(raw))`,
     which mirrors the FSW exactly. Take it out of the risk list.
   - (c) The DSS app's sc→DSS table (2,3,14,15,0,4,5,12,13,8,1,6,7,10,11,9,
     24,26,27,22,23,16,25,28,29,20,21,17,30,31,18,19) is **identical** to
     `utils_oldSubcolIdToFpgaId` and `Detectors_fpgaFromFrontendMinusOne`.
     - So the **DSS det index is the FPGA detector index**, and the DSS
       "sc" is the old front-end numbering.
     - The plan's chain "DSS table + `Detectors_fpgaFromFrontendMinusOne`"
       composes to the identity. State it as `fpga_det = dss_det` and cite
       the table equality.
     - notes-dss-app §6 item 3 says script sc ids map through
       `utils_fpgaIdFromSubcolId` (the new numbering). That contradicts the
       table equality; the code-backed map is `Detectors_fpgaFromFrontendMinusOne[sc−1]`.
   - (d) The FPGA-side bit order of VARPMASK, SUMEMASK, VAREMASK and of
     R12 ALIMON / R4 per channel is not in the code. Every default is all
     ones (0xFFF, 0xFFE, 0x7FFE0000, 0xFFFFFFFF), which hides it. Pixel
     gating on ALIMON depends on it; gating on TH = 63 does not. Open
     question.

   **Fix:**
   - One `static const` conversion table set in `stix_det.c`, copied from
     the FSW tables above with file:line comments: fpga↔subcol, fpga→group
     and ASIC, group→quarter, pixel↔channel.
   - Every property, variable and trace argument carries its space in its
     name (`fpga_det`, `subcol`, `pix`, `chan`, `ebin`, `grp`).
   - An end-to-end check:
     - a qtest (or a debug QMP knob `det-inject`) deposits N counts at a
       known (fpga_det F, pix P, ebin B) and asserts the LACC byte offset
       ((P·32+F)·32+B)·2, big-endian;
     - detector-smoke asserts the dumped QL spectra TM (SSID 32, whose
       per-structure detectorIndex is a sub-collimator) shows the excess
       only at subcol `utils_subcolIdFromFpgaId[F]`, channel B
       (`QLSpectra.c:133-150`);
     - the pixel dimension is covered by the qtest and by
       `det-chunk-format`, because the QL TM sums over pixels.
7. **Replay hook (D-Df), for the user's single `.dssevs` test.** The hook
   as written ("reads `.dssevs` words incrementally per tick") does not fit
   the provided file (notes-dssevs-fakeflare):
   - **Pre-scan.** 176 MB is 44 M words, of which about 60 k are events.
     Pre-scan once (at realize, or with an offline tool) into an event
     table (t_ns, fpga_det, pix, adc). Never stream the wait words per
     tick.
   - **Decode by the type field** (bits 19:16), not "pixel":
     - 0x0-0xB are events (type = pixel);
     - 0xD and 0xE are per-detector setup words (the 0xE value of about
       2000 is the baseline);
     - 0xF is wait-only padding, with val 2048 (bit 31 set) and one 3072
       marker.
     - Every word's wait adds to the time.
   - **Start trigger and offset.** The replay clock must start from a QMP
     `det-replay-start` or from NOMINAL entry (SetEnable), not at boot, and
     take a start offset in seconds (e.g. 880). Otherwise the
     quiet-to-flare step will not land in the gate window.
   - **ADC→bin.** The empty flash has no ELUT (`LUTMgmt could not find LUT`).
     The fallback must subtract the per-detector 0xE baseline. Otherwise
     the file's lines (ADC 2176-3840) all land in high bins. Define the
     fallback bin = clamp((adc − baseline)/k, 1, 31) with k documented
     (open question).
   - **Detector map.** `fpga_det = dss_det` (6c). The file's events sit on
     FPGA 9-15 = subcol {14, 9, 15, 7, 8, 2, 3} (0-based). That is a
     concrete SSID 32 expectation: non-zero only for these detectorIndex
     values.
     - Note that the LC, flare and variance detector masks are 0xFFFFFCFF
       (`ParamMgmtDef.h:251, 297, 331`). They exclude subcol 8 and 9, which
       are FPGA 13 and 10. So only 5 of the 7 lit detectors feed the LC
       contrast check.
   - **Rates and FDIR.** About 9 cps per lit detector in the flare. QLDetPerf
     flags a detector when it has > 5 × max(median, 1) **and** > 300 counts
     per 2 periods (`QLDetPerf.c:124-143`, `ParamMgmtDef.h:336-341`). With
     25 of 32 detectors dark the median is 0, so any replay gain above
     about 4× makes the lit detectors go yellow. After 15 consecutive
     yellows that gives 0x942E/0x5437 and a SUMDMASK change. Keep the gain
     at 1, or do not forbid those EIDs in the replay gate.
   - **Gate.** The file path comes from an init-time property (for example
     `det-replay-file`). The gate SKIPs when the file is absent. Neither
     the file nor anything derived from it is ever committed.
     `det-source=replay-dss` replaces the synthetic sky; it is not added
     to it.

### Recommended changes

1. Retag the IDeF-X power-on defaults as [C]: `ParamMgmtDef.h:525-536`
   `ASIC_REGISTER_n_HW_DEFAULT`. The ELF records at 0x8357c-0x8363c agree:
   1, 0, 62, 0, 15, 1, 0, 3, 0, 3, 3, 0xFFFFFFFF. Pixel channels take 62
   and non-pixel channels 63 (`XRayADCsCtrl.c:205-209`).
2. Fix the risk "`QLAccuDblBuffs_deinit` on CONF only when
   `Attenuator_isOperationFinished()==0`". The CONF-from-NOMINAL flush is
   unconditional (`OpModeMgmt.c:282-306`). The attenuator test is on the
   SAFE path, and it only adds a 3 s wait (`:173-177`). The [L] and the
   SAFE fallback are not needed.
3. **ADC answers.** `adcRead` uses cmd **1** (`Detectors.c:996`), not
   cmd 2. `readTemp` issues cmd 1 twice (`:916, :933`). Define the cmd 1
   response by context: the first cmd 1 after a reg-13 read address block
   = A, the cmd 1 after the following data block = B; any other cmd 1 is
   the baseline. All of them are TC-only paths (`background.c:143-150`).
4. **Re-entrancy (the Phase 4 double-free lesson): state the rule
   explicitly.**
   - The timer callback writes accumulators through
     `memory_region_get_ram_ptr` and the ROTB ring through the host
     buffer, never with `address_space_*`. Follow each RAM write with
     `memory_region_set_dirty`.
   - QMP setters only store fields; the next tick applies them. No MMIO
     dispatch happens from timer or QMP context.
   - The only outputs are `qemu_irq_pulse`.
5. Store all accumulators big-endian (`stw_be_p`, `stl_be_p`). The FSW
   reads native SPARC u16/u32 (`Detectors.h:68-95`, `RotAccuCompactor.c:180-225`).
6. **Count gating.** Evaluate the ReadMode/Enable/power gating per tick,
   not as a snapshot at the swap. Every verification run (now every 30 s)
   briefly clears one group's ReadMode (`Detectors.c:811`), and a
   swap-time snapshot would zero that group for a whole period.
7. **D-5 UART evidence.** `ArchiveManager_addDetectorData` prints
   TRACE_ERROR "Skipped first Det data chunk" for the first chunk
   (`ArchiveManager.c:140`). Use it as positive UART evidence that
   chunks reach the archive. It will also appear in nominal-smoke from
   D-5 on, so do not forbid it.
8. **Archive and flash cost.** The flare rate is about 1 chunk/s,
   compacted to roughly 35-40 kB (bounded by `MAX_DETECTOR_DATA 49216`,
   `RotAccuCompactor.h:8`). That is a 256 KB flash block about every 7 s.
   Measure the emulated NAND write cost in the D-5 performance step, and
   check the UART for archive or filesystem errors on a fresh flash [L].
9. Specify what the Reset and ClearEnable strobes do to QlReady and
   SUMCNT (today [D]). The FSW clears its own `detQLReady` at Reset
   (`Detectors.c:1125`).
10. Doc-vs-code 1 is not a real conflict: "busy, active low, reset 1"
    means 1 = idle = ready, as in the FSW. Reword it.
11. Before relying on the D-2 trace-golden diff, check whether
    `capture-golden.sh` truncates the trace by time or by lines (the golden
    is 30605 lines). Removing 16000 poll lines moves later content into
    the window.
12. Name all FPGA-register masks as FPGA-detector-indexed (SUMDMASK,
    VARDMASK, fsmCalMask). The FSW writes them through
    `utils_subcolMask2fpgaMask` (`OpModeMgmt.c:634`, `QLVariance.c:86`,
    `QLDetPerf.c:184-190`, `Detectors.c:1190`).

### Confirmed claims

- **Registers.**
  - fsmCmd: strobes in bits 8:0, configuration in bits 12:9 and 31:16,
    reserved 15:13 and 4:3. All writes are `|=` or RMW
    (`Detectors.c:131-312, 489-508, 1090-1200`).
  - fsmSta bit positions are as listed (`StixIdpu.h:893-915`).
  - The ISR reads bank (ActiveQl−1) mod 4 after QlClear
    (`Detectors.c:131-167`). It also needs CalReady, QueueOverflow and
    QuarterSeu to read 0, and it clears IRQMP pending (`:233`).
  - IRQ 3 is wired at `leon3.c:470-471`; IRQ_STIXCTRL_DETCTRL = 3.
- **IDeF-X.**
  - CmdReady is bit 31, with 1000-poll timeouts (`Detectors.c:627-639`).
  - The words are as stated (`:747-849`). Read-back shifts only the last
    byte, by `8 − len%8` (`:840`). All the short registers are single
    bytes; R3, R4 and R12 are multiples of 8.
  - The write-back ASSERT in `XRayADCsCtrl_writeRegForScID` is dead code,
    because `in_detSC < 16 && in_detSC > 29` is never true.
  - The RCR state-0 R3 equals the default R3 (`RCR_PIXEL_0 = 0xFFF`,
    `ParamMgmtDef.h:350`; `RateCtrl.c` initStates). There is no 0x542E
    before or after the RCR's first R3 write.
- **Boot and NOMINAL entry.**
  - The boot runs 16 `resetReg` calls right after quarter, enable and mode
    are set to 0 (`OpModeMgmt.c:1056-1064`). There are 16000 polls and
    nothing is logged.
  - NOMINAL entry runs 384 writes (`:576`, `XrayDetFDIR.c:731` is the
    TRACE_ERROR line, 12 × 32). Every OpModeMgmt line reference was
    spot-checked: 128, 171, 258, 300, 470, 501, 576, 625, 669, 672, 689.
- **QL TM.**
  - APID 0x5DC = (93<<4)|12 (`TMData.h:447`, `TM_21.c:780`), with the SSID
    as the first data byte.
  - The flush sends only modules with data (`TMForm.c:720-760`). Spectra
    are built once at period 8 and then wait 256 periods
    (`QLSpectra.c:65-66, 118-184`).
  - LC is 7 B per 4 s, and no QL packet fills within the gate windows.
- **TC.** TC(236,6) `02 02 00 1E` = system 2 (detectors), parameter 2
  (`HWConfig.h:12, 40`), u16 value (`HWConfig.c:974-976`), range 0..0xFFFF
  (`TCVerifDynamic.c:366`). NOMINAL entry writes back the same parameter
  (`OpModeMgmt.c:502`), so the setting survives.
- **Forbids.** None of 0x542E, 0x9438, 0x142C, 0x9433, 0x1427, 0x942E or
  0x5437 fires legitimately with a correct store, QuarterSeu = 0 and a
  uniform sky. Flare detection emits no TM(5,x).
- **ROTB chunk.**
  - The descriptor is `(w0>>16)&0xC000`, the block number `&0x3F` ≤ 48,
    otherwise ASSERT (`RotAccumulator.c:178-220`).
  - Data starts at offset 8 in **every** block. 48 × 252 + 192 counts,
    then 16 × (hw, lw) triggers, then a 56 B pad (`RotAccuCompactor.c:174-245`).
  - The block-0 SCET is the integration end (`ArchiveManager.c:131`).
  - The overrun skip reads the SCET at +2 in every block
    (`RotAccumulator.c:108-121`).
- **ELF parameter records** (value, default, min, max):
  - ASIC_VERIFICATION_PERIOD 0x1000 / 0..0xFFFF;
  - accumulator min/max/count 9 / 199 / 1800;
  - QL period 39;
  - SUMEMASK 0xFFE and SUMDMASK 0xFFFFFFFF;
  - RCR_ENABLED 1, L0 1800, L1 0x222E0;
  - Kbad 5, Cbad 300;
  - quarters 0xF.
  - The rate estimates (SUMCNT ≈ 64/s quiet, 1 s swaps in a flare) are
    consistent with SUMEMASK 0xFFE.
- **Gates.** boot, soak, tc and fault stay in SAFE; only nominal-smoke
  sends mode TCs. The "UART golden unchanged" claim is plausible: the
  only boot-time detector path is the silent `resetReg`. nominal-smoke's
  change from 384 to 0 (non-zero = FAIL) holds.
- **The 11 doc-vs-code items** are resolved in favour of the code. Item 1
  is not a conflict (recommended 10).

### Disposition (folded 2026-11-11)

R n = required item n, r n = recommended item n. Every item was folded into the body except where marked rejected. The
user decisions of 2026-11-11 settle the open points in R6(d) and R7.

| Item | Status | Where |
|---|---|---|
| R1 negative cannot produce 0x542E/0x9438 | folded (option 1); option 2 rejected | Key facts "A read timeout is not a mismatch"; D-3 `--negative` (fails on the UART 731 + "failed to read register" lines and the trace, TM forbids stay quiet). The `det-idefx-readback=zero` knob is rejected for this phase: it adds model surface only to re-prove an FSW path §24 already recorded; listed under Out of scope |
| R2 successful verification is silent | folded | Key facts (IDeF-X, "A successful run is silent"); D-Di; D-3 "Verification evidence (trace)": ≥ 3 clusters of 384 read address blocks |
| R3 LVAR slot overflow | folded | Key facts (CTRL, LVAR 40 slots, `WDT.c:74`); D-Dd "LVAR slot" (stop at slot 39, model choice); D-1 `det-ql-banks` with QlIntTime 99; Risks |
| R4 reset does not touch `s->reg` | folded | Key facts (CTRL); D-Da "one source of truth"; D-Dh explicit reset values; D-1 `det-reg-reset`; Gate impact (fault-smoke); Risks |
| R5 `det-ql-banks` vs materialize | folded | D-Dd "Bank lifecycle (defined once)"; D-1 `det-ql-banks` (marker after the swap, survives two swaps, zeroed on the third, i.e. on re-activation); D-4 `det-counts-zero` |
| R6 index spaces | folded | new section "Index spaces" (inventory with the names `asic_ch`, `pix`, `fpga_det`, `grp`, `quarter`, `asic`, `subcol`, `fe_sc`, `dss_det`, `ebin`, `adc`; identities; mask bit orders; one conversion table set in `stix_det.c`; end-to-end check); names applied throughout; tables in D-2; end-to-end commit D-6 |
| R6(a) pixel map matches | folded | Index spaces, `pix` → `asic_ch` |
| R6(b) R3 bit position is [C] | folded | Index spaces "R3 threshold of `asic_ch` c"; the "inferred" pixel-gating risk removed from Risks |
| R6(c) `dss_det` = `fpga_det` | folded | Index spaces "Key identities"; D-Dk; Doc vs code 13 |
| R6(d) undefined bit orders | folded per user decision | assumed bit i = index i [A], not gated; ALIMON gating dropped (TH = 63 only): Index spaces, D-Df, Doc vs code 12, Out of scope |
| R7 replay hook | folded per user decision | new D-Dk and commit D-7 (pre-scan by type field, trigger + offset, linear non-physical ADC→`ebin` above the 0xE baseline, gain fixed 1×, `det-replay-file`, one optional SKIP-able gate, file never committed); Key facts "The user's `.dssevs` file"; the 5-detector LC contrast in the D-7 gate |
| r1 IDeF-X defaults are [C] | folded | Key facts (IDeF-X, Defaults); D-De |
| r2 CONF flush is unconditional | folded | Corrections to the brief; Risks "Long QL TM latency" (the [L] and the SAFE fallback removed) |
| r3 ADC answers use cmd 1 by context | folded | Key facts (IDeF-X words); D-De decoder; D-2 `det-idefx-adc` |
| r4 re-entrancy rule | folded | new D-Dj; D-1 |
| r5 big-endian accumulators | folded | D-Df "Byte order"; D-4 `det-lvar-ltrg`; D-6 `det-inject-lacc` |
| r6 per-tick gating | folded | D-Df "gating per tick" and exposure; D-4 `det-counts-gating` (one-tick ReadMode pulse) |
| r7 "Skipped first Det data chunk" | folded | Key facts (science path); D-5 positive UART evidence; nominal-smoke allowed (D-5, Gate impact) |
| r8 archive and flash cost | folded | D-5 Performance (NAND cost, flash growth); Risks |
| r9 Reset/ClearEnable on QlReady and SUMCNT | folded | D-Dc (ClearEnable freezes; Reset clears QlReady, QueueOverflow, SUMCNT; both [I], not FSW-visible); D-1 `det-fsm-strobes` |
| r10 Doc vs code 1 is not a conflict | folded | Doc vs code 1 reworded as a clarification |
| r11 capture-golden truncation | folded (checked) | `capture-golden.sh` uses `timeout 90` (time-bounded) and the trace golden is a manual reference (`baseline/README.md:138-141`): Key facts (CTRL); D-2 Goldens; Goldens table and notes |
| r12 FPGA masks are `fpga_det`-indexed | folded | Index spaces ("ASW masks are `subcol`-indexed; FPGA masks are `fpga_det`-indexed"); NOMINAL entry step 7; D-Df Masks |

User directives folded besides the items above: the R3 pixel-gating
"inferred" risk is removed (Risks); the CONF-flush wording is fixed
(r2); replay is D-7 and docs are D-8. The new D-6 is the index-space
end-to-end commit (`det-inject` plus the SSID 32 check). It sits before
the replay because the replay gate relies on the same mapping.

### Second pass (2026-11-11): TM/TC ICD and ELUT

Folded from `notes-tmtc-icd-detector.md` and the coordinator's ELUT
decision:

| Topic | Where |
|---|---|
| ELUT location (0xE0000000, `[pix][fpga_det][adc]`, 32 = unassigned), all-31 on an empty flash | Key facts "The ELUT" |
| Honour the ELUT for ADC-domain sources; linear fallback on a degenerate row with trace and doc note; synthetic sky and inject unaffected | D-Dk; D-Df; D-7 `det-replay-elut` (both branches); replay-gate fallback check; Risks; Doc vs code 20; Open question 2 (the "constant row" reading) |
| QL framing, S0 0x2B decode, SSID 30-34 offsets, bands, 0xFFFFFCFF, worked example, cadences, SSID 32 timing (32 s, 1024 s) | Key facts "QL TM decode for the gates"; D-3 `ql-check.py` and flush content; D-4, D-6 and D-7 checks |
| L0 SSID 20 word vs archive word, `fpga_det` → `subcol` in the compactor | Key facts (science TM); D-5; Out of scope |
| TCs: no service-236 QL TC; TC(237,9) and TC(237,18) (ID 315); TC(21,x) mask bits; every gate TC checked | Key facts "The TCs the gates use"; D-3 timeline with the exact TC(236,1)/(236,6) app data |
| Doc vs code: TOC TC(236,9), BG 8 s vs 32 s, 0x5429, 0x9433, 0x942E, SSID 31 `grp` 4 vs 5, SSID 33 39/40, `QL_FLARE_ENABLE_TM`, no-ELUT | Doc vs code 14-20 |
| VARDMASK/SUMDMASK are `fpga_det`-indexed; LVAR slot 39 unused at the default period | Index spaces (already); D-Dd LVAR slot |
| Gate expectations corrected | D-3: all five SSIDs (30-34) expected at the exit flush, with zero content; D-6: inject at T_nom + 8 s, byte B+1 of the 0-based `subcol` sample; D-7: 0-based `subcol` set, S0-decoded band sums, the fallback trace; nominal-smoke `--negative` SSID 32 conditional |
