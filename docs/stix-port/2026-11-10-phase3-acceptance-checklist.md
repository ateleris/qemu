# Phase 3 acceptance checklist

The Phase 3 goal, from `2026-11-10-phase3-detailed-plan.md`: the HK ADC
returns nominal values so the boot self-test passes and the FSW can be
driven SAFE → CONFIGURATION → NOMINAL with telecommands; the NOMINAL
gate exists; the docs (NOMINAL recipe, known limits) are in place.

**Status: all three Q commits are in; the gate records are filled in
(accepted-diffs §22/§23).** What is left is listed under "Open items"
and "Cleanup".

## State

- [ ] **uprev tip** — verify with
  `git -C /home/laszlo/dev/qemu-stix-uprev rev-parse --short HEAD`
  after the fast-forward of this commit. `stix-uprev-tc` (worktree
  `/home/laszlo/dev/qemu-stix-tc`) is at the same commit; each commit
  was fast-forwarded onto uprev after review.
- [x] **Commits of Phase 3** (`git log --oneline fb03c6f775..stix-uprev-tc`):

  | Commit | What |
  |---|---|
  | `ece1ccf8fb` | the Phase 3 detailed plan (HK ADC values, NOMINAL mode) |
  | `f2e8664506` P3-1 | HK ADC returns nominal values (D-P3a): the 128-entry nominal table, the state overrides from hkCmdSta/idefxConfig, property `hk-adc` |
  | `0499cfb506` P3-2 | the NOMINAL gate and the probe hold/forbid: `nominal-smoke.sh` (positive + `--negative` + `--check-guard`), `tmtc-probe.py --hold/--forbid` |
  | P3-3 (this commit) | the docs: the NOMINAL recipe in `ground-station.md`, §24 known limits, the Phase 2 checklist inputs update, the runbook warning, this checklist |

- [x] **No AI trailers** — same rule as Phase 2
  (`git log --format=%B | grep -ci 'co-authored\|claude'` → 0).
- [x] **The GS-side doc commit** (plan P3-3, last bullet): branch
  `stix-qemu-integration`, `docs/stix-qemu.md` "Limits" now has the
  three-TC NOMINAL recipe in place of the TC(20,128) sentence
  (GS commit d5213aa).

## Decisions in force

- **D-P3a:** the HK ADC returns fixed nominal values; POWERSTAT
  (hkCmdSta bits 9:5) stays unmodelled and reads 0 (§22, §24).
- **D-P3b:** the detector electronics stay as they are; the 384 IDeF-X
  write timeouts at NOMINAL entry remain, and the IDeF-X ready bit is
  NOT modelled on its own (§24, the trap).
- **D-P3c:** no TC(20,128) keep-alive. The NOMINAL recipe disables the
  reaction with TC(236,6) in CONFIGURATION; there is no generator and
  no GS reaction.

## Gates (commands to re-run)

```bash
cd /home/laszlo/dev/qemu-stix-tc
export PATH=/home/laszlo/venvs/qemu-build/bin:$PATH
make -C build -j8 qemu-system-sparc tests/qtest/stix
flock /tmp/stix-qtest-21000.lock env QTEST_QEMU_BINARY=$PWD/build/qemu-system-sparc \
    build/tests/qtest/stix --tap | grep -c '^ok'                  # 34
flock /tmp/stix-qtest-21000.lock bash scripts/stix/boot-smoke.sh build/qemu-system-sparc
flock /tmp/stix-qtest-21000.lock bash scripts/stix/soak.sh build/qemu-system-sparc 300
flock /tmp/stix-qtest-21000.lock bash scripts/stix/tc-smoke.sh build/qemu-system-sparc
python3 scripts/stix/tmtc-probe.py --self-test
flock /tmp/stix-qtest-21000.lock bash scripts/stix/nominal-smoke.sh build/qemu-system-sparc
flock /tmp/stix-qtest-21000.lock bash scripts/stix/nominal-smoke.sh \
    build/qemu-system-sparc --negative
```

`nominal-smoke.sh` clears its own run dir (default `/tmp/stix-nominal-run`,
override with `STIX_NOMINAL_RUN_DIR`) at every start, so each run boots a
fresh flash; it refuses the shared run dirs itself.

## Acceptance criteria and their evidence

- [x] **The HK ADC returns nominal values** (P3-1, §22):
  - the BSW boot self-test channels read 2048/3413/3958, all 38 mapped
    channels are inside both the self-test windows and the PFM/IDB
    limits; SPWEN/idefxConfig/LVEN/HVxxEN override seven lines at
    conversion time — pinned by qtest `/sparc/stix/hk-adc` (34 tests
    green; red was `(VAL == 2048) == 0`);
  - on a fresh flash the boot report is the success TM(5,1) 0x0022 and
    TM(5,3) 0x8002 + TM(5,4) 0xD420 are gone (run p31boot, §22);
  - the UART golden lost only the three
    `HKAcquire *E reference voltage` lines; boot-smoke passes against
    the regenerated golden;
  - the 5.2 wrapper keeps `hk-adc` red (the 5.2 model reads VAL 0).
- [x] **NOMINAL is reachable and stable** (P3-2, §23 gate record):
  - **positive** (`/tmp/stix-nominal-pos`): PASS — boot 0x0022, 0x8030,
    0x14A3 at +13 s, 0x142E at +29 s; CONF at +50 s → `0021 02 04`;
    S20=0 at +60 s → acked; NOM at +70 s → `0021 04 05` 4 s later; SID2
    `0215` (mode NOMINAL) 60 s after entry; TC(17,1) at +165 s fully
    answered with a heartbeat; no forbidden TM; 384 IDeF-X timeouts;
  - **negative** (`--negative`, `/tmp/stix-nominal-neg`): PASS —
    without TC(236,6) the fallback comes: 0x142E 27 s after the
    NOMINAL report, then `0021 05 02` (NOMINAL → SAFE), in both runs;
  - no `storing new context[01] failed`; the shared flash
    (`/tmp/stix-uprev-run/flash.bin`) untouched (mtime check);
  - tc-smoke, boot-smoke and the probe self-test unchanged (PASS).
- [x] **The docs are in place** (P3-3): the NOMINAL recipe with the GS
  command names and parameters (ground-station.md), the runbook
  warning, the Phase 2 checklist inputs update, and §24 known limits.

## Open items / Phase 4 candidates (plan, "Out of scope")

- Error injection: ADC channel overrides at runtime, the HK protection
  IRQ, SpW link faults.
- POWERSTAT synthesis (with the SpW-voltage/SPWEN atomicity
  requirement, §22).
- An IDeF-X register file (all or nothing, §24's trap); FSM/QL status
  and the QL IRQ; ROTB science/aspect data.
- A TC(20,128) generator and the GS reaction — deliberately not built
  (D-P3c); TC(236,6) replaces it.
- The generic GS fixes: the 50 ms datagram pace and the history cap
  (also Phase 2 open item 2).
- Still open from the Phase 2 checklist: link-error/AHB-error
  injection, more than one TM client.

## Cleanup (the owner runs these, after acceptance)

The Phase 3 experiments left their own run dirs. Each holds a sparse
`flash.bin` of about 16 KB real, but `ls` shows 16.5 GB apparent size —
`du -sh` for the truth:

```bash
ls -lh /tmp/stix-p3-probe* /tmp/stix-p3-ctx /tmp/stix-p3-p31boot \
       /tmp/stix-nominal-run /tmp/stix-nominal-pos /tmp/stix-nominal-neg
rm -rf /tmp/stix-p3-probe* /tmp/stix-p3-ctx /tmp/stix-p3-p31boot \
       /tmp/stix-nominal-run /tmp/stix-nominal-pos /tmp/stix-nominal-neg
```

**Never** delete or reuse `/tmp/stix-uprev-run` (the golden run dir) or
`/tmp/stix-baseline-run` (the frozen v11.1 reference) — see
`baseline/README.md`.
