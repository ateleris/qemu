# Phase 3 detailed plan: HK ADC values and NOMINAL mode (2026-11-10)

Track worktree `~/dev/qemu-stix-tc` (branch `stix-uprev-tc`), base `fb03c6f775`.
Same process as Phase 2: a GLM worker implements, an independent reviewer reads a frozen diff
snapshot, I re-run red/green and the gates, commit without AI attribution, and fast-forward
`stix-emul-qemu-uprev`.

## Decisions (Laszlo, 2026-11-10)

| Id | Decision |
|---|---|
| D-P3a | HK ADC: **fixed nominal values**. POWERSTAT (`hkCmdSta` bits 9:5) stays unmodelled and reads 0. |
| D-P3b | Detector electronics: **left as they are**. The 384 IDeF-X write timeouts at NOMINAL entry stay, and are documented. The IDeF-X ready bit must **not** be modelled on its own (trap below). |
| D-P3c | TC(20,128) keep-alive: **not provided**. The NOMINAL recipe disables the reaction with TC(236,6) in CONFIGURATION. There is no QEMU generator and no GS reaction. |

## Inputs

- `drafts/2026-11-10-recon-hk-adc.md`: the ADC interface, every FSW check on HK values, and the table of nominal raw values (§4).
- `drafts/2026-11-10-recon-phase3-nominal.md`: what the SAFE→CONFIGURATION→NOMINAL transitions do to hardware, the expected TM and the failure signatures.
- `drafts/2026-11-10-recon-tc20.md` and `drafts/2026-11-10-recon-phase3-gs-generators.md`: the TC20 background. They are not used by the plan after D-P3c.
- **Experiment p3probe4** (2026-11-10, normal ELF, TCP probe on port 21600).
  - **Not a fresh flash.** It reused `/tmp/stix-p3-probe3/flash.bin`, which p3probe3 had written at its NOMINAL entry with S20 = 0. That is why it has no boot TM(5,3) 0x8030 ("no CONTEXT file") and no SAFE-mode TM(5,1) 0x142E.
  - It proves stable NOMINAL **with TC(236,6) sent**. It proves nothing about fresh-flash timing.
  - Fresh-flash runs (p3probe, p3probe2, p3probe3) show 0x8030 at boot, 0x14A3 at CUC 0x10-0x12, and one 0x142E at CUC ~0x21 (~33 s) while still in SAFE. That one is harmless.
  - Sends: CONF at 40 s, TC(236,6) S20=0 at 50 s, NOMINAL at 60 s, TC(17,1) at 130 s.
  - Result:
    - TM(5,1) 0x0021 `02 04 00`;
    - TM(5,2) 0x542C p1=01 (no LUTs);
    - TM(5,1) 0x0021 `04 05 00` at CUC 0x40.
  - It then stayed in NOMINAL until the end (CUC 0x82), with no fallback.
  - Heartbeats were continuous; TC(17,1) at 130 s was fully answered.
  - UART: 384 × `writing register … failed (XrayDetFDIR.c:731)` and `LUTMgmt min or max temp not set` / `T1LUT applying error`.
  - The earlier "TM stops in NOMINAL" note was a probe artifact: gate mode exits once every expectation matched. It is retracted.

## Commits

### P3-1 HK ADC value model (`hw/sparc/grlib_stixctrl.c`, `tests/qtest/stix.c`)

**Behaviour**
- On a write to `hkAdc` (0x8C):
  - `ch = value & 0x7F`
  - the register becomes `(hk_value(s, ch) & 0xFFF) << 20 | 1 << 7 | ch`
  - READY stays immediate, as today.
- `hk_value` returns a static table entry (recon-hk-adc §4, the "Proposed raw" column) with these overrides, all from **commanded** register state:
  - SPW0_V / SPW1_V (48/52): on 3240, off 20, from `hkCmdSta` bit 3 / bit 4 (the SPWEN field is bits 4:3).
  - DPU_SPW_C (3): 50 / 900 / 1830 for 0 / 1 / 2 SPWEN lines.
  - DET_C (4): `20 + 75 * popcount(idefxConfig & 0xF)`.
  - ATT_V (12): 1670 when LVEN, else 10.
  - HV_17_32_V (16) / HV_01_16_V (20): 1000 when HVxxEN, else 16 / 15.
  - Unused channels (not in the 38-channel map) read 0.
  - ATT_C (8) is a static 20. The attenuator is not modelled to move.
    - **Latent:** 20 is below the PFM two-motor minimum of 28 (ATT2M_C).
    - No NOMINAL path moves the attenuator (RateCtrl_reset(FALSE)). A future TC-driven attenuator test needs a motion-dependent value of 110 or more, kept below 0x100.
    - Record this in the known limits.
- POWERSTAT stays 0 (D-P3a). With POWERSTAT = 0 the FSW skips the SpW, ATT_V and HV voltage lines. The overrides exist so that SID2 HK and any later POWERSTAT model see consistent values, and so that state-dependent limits pass under PFM/context limits, not only under the AX 0..4095 limits.
- New property `hk-adc` (bool, default **true**). `false` keeps the legacy VAL = 0 behaviour, for A/B comparison and as a rollback.
- The value table lives in the C file as a `static const uint16_t[128]`, with one comment line per channel naming the line and the PFM limit it satisfies.

**Why these values:** every value sits inside both the BSW boot self-test window (1V5 1951..2193, 2V5 3357..3473, 2V9 3784..4094) and the PFM/IDB soft limits. The build flavour (AX or PFM) and a flash context with flight limits therefore don't matter.

**Expected golden changes** (they need a baseline regeneration and an `accepted-diffs.md` §22 entry):
- UART: the 3 × `HKAcquire reference voltage n … out of range` lines disappear.
- The boot report becomes success: TM(5,1) EID 0x0022, application data `0022 00000008 00000000 ba` (today: TM(5,3) `8002 00000008 00000008 ba`). There is **no** TM(5,3) 0x8002 + TM(5,4) 0xD420.
  - `tm.golden.log` starts after boot seq 0-3, so this change does **not** show there.
  - Pin it with a boot expectation in nominal-smoke.sh (P3-2): `0x5A7:5:1:002200000008000000` in the connect group.
- **Checked 2026-11-10 (run p3ctx, a sparse copy of `/tmp/stix-uprev-run/flash.bin`):** the shared flash holds a context, so there is no 0x8030 at boot. **S20 is still enabled:**
  - 0x14A3 arrives at CUC 0x10 and 0x142E at CUC 0x21, exactly as on a fresh flash.
  - CONF at 45 s, then TM(5,1) `00210204`, with no fallback.
  - **Explained:** `tm.golden.log` was last captured in 2d2b782b51 (2026-11-09). That was before the watchdog model (5cda8ac82e) and before the D-I flash. `capture-golden.sh` does not regenerate it.
    - Its 0x8021 ("warm SW reset by WDT failed") and the late 0x14A3 therefore come from the old raw-storage watchdog, not from the flash context.
    - The shared flash needs no rework. tm.golden stays a historical artifact; §22 notes this.
- The HK TM(3,25) payloads change.
- qemu-stdout `0x8c` read values change (VAL in bits 31:20).
- The LUTMgmt temperature messages may change (the det-temp average goes 0 → ~2450). Record whatever happens; don't tune values to hide it.

**qtests**
- Channel 24/28/44 values; one per-line spot check from each group (current, temp, ASP).
- SPWEN 0/1/2/3 → SPW_C and SPW0/1_V.
- idefxConfig quarters 0..4 → DET_C.
- LVEN/HVxxEN → ATT_V/HV.
- Unused channel → 0; READY set; ADDR echoed.
- `hk-adc=false` → VAL 0.
- POWERSTAT bits 9:5 still read back what was written (they stay plain storage; no synthesis).

**Red/green:** the new qtests fail on HEAD and pass on the change. Boot-smoke passes. The soak and capture-golden are regenerated and the diffs are explained.

### P3-2 Probe hold mode and the NOMINAL gate (`scripts/stix/tmtc-probe.py`, new `scripts/stix/nominal-smoke.sh`)

**Probe**
- `--hold`: in gate mode, keep listening until `--seconds` even after every expectation matched. The verdict comes at the end.
- `--forbid APID:T:S[:DATAHEX]` (repeatable), **scoped like `--expect`**: it belongs to the send group it follows. It arms at that group's send time. A matching TM after arming fails the run; one before arming is ignored.
  - Global forbids would false-fail: the boot report `0021 00 02 01` and the fresh-flash SAFE-mode 0x142E are normal.
- Patterns are exact prefixes. No `??` wildcards are needed: use `00210502` (NOMINAL→SAFE) and `00210402` (CONFIGURATION→SAFE).
- The self-test covers:
  - a hold run passing;
  - a forbidden TM after arming failing;
  - a forbidden TM *before* arming not failing;
  - a forbidden TM after all expectations matched still failing in hold mode.
- Expectations match in **any order** within their window (tmtc-probe.py:296-305). The plan does not rely on order.
- The usage text says plainly that gate mode without `--hold` exits on the last match.

**nominal-smoke.sh**
- Same structure as `tc-smoke.sh`: its own run dir, a fresh device-created flash.bin, normal ELF via `STIX_ASW_ELF`, the 21000 lock.
- **It must not use the shared `/tmp/stix-uprev-run` flash.bin.** Any first NOMINAL entry writes a flash context (ContextMgmt_deinit at OpModeMgmt.c:317), not just S20. Later boots on that flash would start from that context, and the golden would drift. Every run gets a fresh, empty run dir. The script creates it, and the device creates the sparse flash (~16 KB real).
- Sequence and expectations:
  1. Connect group. These expectations also prove the flash is fresh:
     - TM(5,3) 0x8030 (no context file);
     - the boot report TM(5,1) `0022…` (P3-1);
     - TM(5,1) 0x14A3 (FDIR enable);
     - TM(5,1) 0x142E (the boot TC20 timer, ~33 s, harmless in SAFE).
  2. CONF at **50 s** (as implemented; at least 10 s after the 0x142E); 40 s left only 5-8 s of margin.
     - Expect TM(1,1), TM(5,1) `00210204`, TM(1,7).
     - Forbid `00210402` from here on.
  3. TC(236,6) S20=0 at 60 s.
     - Expect TM(1,1) and TM(1,7). It sends **two** TM(1,7) (HWConfig.c:1044 plus TcService236.c:213); don't assert exactly one.
  4. NOMINAL at 70 s.
     - Expect TM(1,1), TM(5,1) `00210405`, TM(1,7) within 20 s.
     - Armed forbids from here on:
       - `0x5A7:5:1:00210502`;
       - TM(1,8);
       - TM(5,1) 0x142E;
       - TM(5,3) 0x9449.
     - Also expect SID2 HK TM(3,25) on 0x5A4 with application data starting `0215` (mode byte at SP[17]). Give it a window `@85` (as implemented), since SID2 comes every 64 s.
  5. TC(17,1) at 165 s → TM(1,1), TM(17,2), TM(1,7). Also expect a heartbeat (0x5E5) in this window.
  6. `--hold --seconds 190`.
- UART checks after the run:
  - `init: SAFE mode` is present.
  - No match for the regex `storing new context[01] failed`. The real strings are at ContextMgmt.c:2483/2491; the plain text in the earlier draft never matched.
  - The IDeF-X burst is **expected**: count exactly 384 `XrayDetFDIR.c:731` lines and print the count. Do not fail on them; a different count is a warning line in the gate output.
- Runtime target under 4 minutes. Trace events stay off (a `stixctrl_io*` trace at NOMINAL entry is ~30 MB).

**Red/green:** the probe self-test fails on HEAD (no `--hold`/`--forbid`) and passes on the change. `nominal-smoke.sh` passes. Robustness:
- The gate must fail when the TC(236,6) step is removed. Run this negative case once, on a **fresh** flash, and record it in the gate record. On a context that already has S20 = 0 it would wrongly pass.
- Per the code, NOMINAL entry re-arms the 30 s timer (aswConfig S20 = 1 → `rtems_timer_reset`). TM(5,1) 0x142E and `0021 05 02 02` should then follow ~26-30 s after the NOMINAL report. Assert both appear.
- Whether `rtems_timer_reset` on an already-fired timer succeeds under RTEMS 4.8 is unverified, and the call is ASSERTed. If the negative run shows something else, stop and report; don't adapt the gate to it.

### P3-3 Docs

- `docs/stix-port/ground-station.md`: a NOMINAL recipe section with the three TCs, their GS command names and parameters, the expected TM, and the "use a scratch flash" note.
- `docs/stix-port/accepted-diffs.md`:
  - §22: the P3-1 golden changes.
  - §23: the NOMINAL-entry side effects. These are the 384 IDeF-X timeouts, TM(5,2) 0x542C, LUT messages, and SID2 showing LV/HV/SpW off because POWERSTAT = 0.
- A "Known limits" note:
  - No detector model; the IDeF-X ready-bit trap: modelling it alone makes the 4096 s ASIC check mark every ASIC ignored.
  - No QL or science TM; no POWERSTAT; no HK hardware protection IRQ.
  - `hkIrqSta` stores FSW clear writes raw (cosmetic).
- `2026-11-10-phase2-acceptance-checklist.md`: update its Phase 3 inputs line to say TC(20,128) is replaced by TC(236,6).
- GS branch `stix-qemu-integration`, `docs/stix-qemu.md` "Limits": replace the TC(20,128) sentence with a pointer to the NOMINAL recipe. This is a separate GS commit and does not touch `main`.
- A Phase 3 acceptance checklist, written like Phase 2's.

## Gates per commit

The Phase 2 set, plus:
- the qemu-stix suite under the lock, boot-smoke, the soak, capture-golden (regenerated in P3-1), the 5.2 wrapper and the probe self-test;
- `tc-smoke.sh`;
- `nominal-smoke.sh` from P3-2 onward.

## Out of scope (Phase 4 candidates)

- Error injection: ADC channel overrides at runtime, HK protection IRQ, SpW link faults.
- POWERSTAT synthesis.
- An IDeF-X register file; FSM/QL; ROTB science data.
- A TC(20,128) generator; the GS reaction; the generic GS fixes (50 ms pace, history cap).

## Risks

- **P3-1 golden churn** touches every later baseline. Land it first and regenerate once.
- **The LUTMgmt path with real temperatures** may emit new TM(5,2) at the 64 s average. That is accepted if it is explained. Otherwise the default temperatures in the table are wrong, so check against the CIXP0024 curve.
- **Flash context persistence:** any manual NOMINAL experiment on the shared run dir pollutes it. That is any NOMINAL entry, not only one with TC(236,6). The runbook gets a warning. Experiments use a fresh run dir per run; reusing one silently changes the boot (as with p3probe4).
- **HK_getADCReadout has no lock** between its write and its read (HK.c:130-157). A preempted reader could get another channel's value. This was invisible at ADC = 0. It is an FSW property, not something to model around; note it if a limit trip ever shows a value from the wrong channel.
- **The ~48.8M-store ELUT fill** at NOMINAL entry took a few seconds in p3probe4, so it is not a gate timing problem. Keep the 20 s window.
