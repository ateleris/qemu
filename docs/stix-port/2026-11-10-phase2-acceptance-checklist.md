# Phase 2 acceptance checklist

The Phase 2 goal, from `2026-11-10-phase2-detailed-plan.md`: a TC sent by the ATS ground
station, or by a script, reaches the real FSW through the SpW RX DMA path. The FSW verifies
it, executes it, and reports TM(1,1), the service TM, and TM(1,7). The FSW gets SpW time
codes from QEMU and stays time-synchronised. The ground station talks to QEMU directly over
UDP, with no bridge.

**Status: all eight Q commits, G1 and G2 are in; the end-to-end gate passed.** What is left
is listed under "Open items" and "Cleanup".

## State

- [x] **uprev tip** = `81abea7e30`. Check with
  `git -C /home/laszlo/dev/qemu-stix-uprev rev-parse --short HEAD`.
  `stix-uprev-tc` (worktree `/home/laszlo/dev/qemu-stix-tc`) is at the same commit. Each
  commit was fast-forwarded onto uprev after review; there are no merges or rewrites.
- [x] **Commits since the Phase 1 checklist** (`git log --oneline 5beaff0bb8..stix-emul-qemu-uprev`):

  | Commit | What |
  |---|---|
  | `2370355d6d` | GS branches note |
  | `705e55034f`, `f1e97ed420` | the gate scripts default to the normal FSW ELF, plus the README note |
  | `9817c65735` P2-1 | hermetic TM server: no server under qtest; realize errors instead of `exit` |
  | `550c14f6d8` P2-2 | SPW/TMTC/CTRL console prints become trace events |
  | `9a82427ae9` P2-3 | SPW register semantics: W1C, reserved bits, IRQ gating with a deferred latch (D-M), reset |
  | `f9e51611c8` P2-4 | TMTC transport on the main loop, non-blocking TM, one client, accept disarmed while connected |
  | `96311b8637` P2-5 | SPW RX descriptor DMA with a 64-unit no-spill queue (D-L); RX held while control.IE = 0 |
  | `dd050e13e0` P2-6 | TC ingress over TCP (D-K framing), `tmtc-probe.py` send/expect, new gate `tc-smoke.sh` |
  | `73542cfb20`, `dc9b1b2d19` P2-7 | SpW time codes on `QEMU_CLOCK_VIRTUAL` at the SCET rollover (D-J), plus the gate record |
  | `866ee37709`, `81abea7e30` P2-8 | UDP transport for the ground station (D-B), plus the end-to-end record |

- [x] **Ground station** (branch `stix-qemu-integration`, worktree
  `/home/laszlo/dev/ATS-GroundStation-stix`, local only, never on `main`):
  - G1 `b443eec`, `1306f7e`, `0dcb1af`: the `stix_pus` serializer, with 160 tests passing;
  - G2 `2946e01`: `docs/stix-qemu.md`.

  The generic fixes are in PR ateleris/ATS-GroundStation#12 (branch
  `fix/robustness-and-html-escaping`).
- [x] **No AI trailers.**
  - `git -C /home/laszlo/dev/qemu-stix-uprev log --format=%B 5beaff0bb8..HEAD | grep -ci 'co-authored\|claude'` → `0`.
  - On the GS branch, `b4f8e31..HEAD` → `0`.
- [x] **Backup ref unchanged.** `stix-emul-qemu-uprev-backup-20261110` → `1f42ecb32e`.
- [x] **Baseline `flash.bin` untouched.** `stat -c '%s %y' /tmp/stix-baseline-run/flash.bin` →
  `17716740096 2026-11-09 15:30:10`.
- [ ] **Main tree: one stray directory.** `git -C /home/laszlo/dev/qemu status --short`
  shows the same three modified files and the r3 status doc as before Phase 2, plus
  `.playwright-mcp/` (2 files). That directory was created by the browser check during the
  ground-station demo. It is a tool output directory, not source. Remove it when convenient
  (see Cleanup); it is left for the owner because that tree is read-only for the agent.

## Per commit

The routine for every commit:
- the GLM worker implemented in `qemu-stix-tc`;
- an independent reviewer read a frozen snapshot of the diff (the reviews are under `drafts/`);
- fix rounds followed until no blocker or major finding was left;
- the orchestrator re-ran red and green itself, by building with the HEAD device files swapped
  back in, then ran the gates and committed.

- [x] **P2-1** `9817c65735`.
  - tmtc-server is OnOffAuto, and auto means off under qtest.
  - Realize reports an error instead of calling `exit`.
  - New tests: tmtc-hermetic, tmtc-bind-error, flash-create-error.
- [x] **P2-2** `550c14f6d8`.
  - 26 trace events; default output is silent.
  - New guard: `scripts/stix/tests/test-no-console-prints.sh`.
  - The stdout golden was recaptured with `-trace 'stix*'`.
- [x] **P2-3** `9a82427ae9`.
  - The test `spw-regs` is new: red was the `0x181e` read-back returning 0.
  - spw-tx was edited as decided: the descriptor is armed EN|WR|IE, IE is set first, the TX
    uses TE|TI.
  - Review M1: the reset test now proves that the latch is cleared.
- [x] **P2-4** `f9e51611c8`.
  - New tests tm-tcp, tm-stalled-client and tm-reconnect: red is a timeout on the old binary.
  - Review M1: a second waiting client made the main loop busy-spin at about 93% of a core.
    Fixed by disarming accept while a client is connected; a `/proc` probe shows 0% after the
    fix.
  - The stdout golden was recaptured.
- [x] **P2-5** `96311b8637`.
  - New test spw-rx: red was "Property rx-inject not found".
  - Review M1: PR was lost to the FSW's `dmaControl |= TE` read-modify-write while IE = 0.
    - Fix: RX is held while IE = 0 and retried on the IE 0→1 edge.
    - The orchestrator reproduced the red on the pre-fix snapshot:
      `0x08000010 == 0x0a000000`.
  - Second fix round: a model change made only to make a test pass, which kept a register
    across reset, was rejected. The selector readback follows GRSPW instead, and the FSW
    never reads it (`SPW.c:545` is a write).
- [x] **P2-6** `dd050e13e0`.
  - New test tc-tcp: red was that no descriptor got filled.
  - The probe had two blockers: its old mode crashed, and it read TM application data at
    SP[10] instead of SP[16]. Both were fixed, and the probe's self-test now pins the four
    real FSW TM units.
  - First real TC: `tc-smoke` PASS.
  - Known limitation: the TC plausibility check requires the DFH flag (see Open items).
- [x] **P2-7** `73542cfb20`.
  - New test spw-timecode: red was "no TO at the rollover".
  - Review B1: the probe's `CucExpect` had no `window`, so tc-smoke failed on every run.
    Fixed, with a self-test of the verdict loop.
  - The UART golden is unchanged after normalization, and TM(5,2) EID 0x542B "time code
    lost" is gone.
  - The stdout golden was recaptured.
- [x] **P2-8** `866ee37709`.
  - New tests spw-udp, spw-udp-bind-error and spw-udp-host-error: red was "Property
    udp-tm-port not found".
  - Review M2: the bridge and `--udp` cannot run together. This is documented, and
    `run-qemu-gs.sh` now shows startup failures.
  - Order finding: the worker had weakened the FIFO test after seeing reordering.
    - Cause: one fresh socket per datagram. One sender socket: 0 of 300 bursts out of order.
      A fresh socket per datagram: 219 of 300.
    - The test now sends from one socket and asserts strict order.
    - The ground station sends from one persistent socket (`app/transport/udp.py`), so real
      TCs keep their order.

## Gates (commands to re-run)

```bash
cd /home/laszlo/dev/qemu-stix-uprev
export PATH=/home/laszlo/venvs/qemu-build/bin:$PATH STIX_RUN_DIR=/tmp/stix-uprev-run
make -C build -j8 qemu-system-sparc tests/qtest/stix
flock /tmp/stix-qtest-21000.lock env QTEST_QEMU_BINARY=$PWD/build/qemu-system-sparc \
    build/tests/qtest/stix --tap | grep -c '^ok'                       # 33
flock /tmp/stix-qtest-21000.lock bash scripts/stix/boot-smoke.sh build/qemu-system-sparc
flock /tmp/stix-qtest-21000.lock bash scripts/stix/soak.sh build/qemu-system-sparc 300
flock /tmp/stix-qtest-21000.lock bash scripts/stix/tc-smoke.sh build/qemu-system-sparc
python3 scripts/stix/tmtc-probe.py --self-test
```

- [x] **Full suite:** 33/33 ok at `866ee37709`.
- [x] **Boot-smoke:** PASS after every commit. The UART golden is unchanged through Phase 2.
- [x] **Soak:** PASS after every commit; 600 s after P2-7.
- [x] **tc-smoke** (from P2-6 on): PASS after P2-6, P2-7 and P2-8. The run after P2-8:

  ```
  EXPECT OK: 0x5A1:1:1:1DACC000 (unit len=24)        TC(17,1) accepted
  EXPECT OK: 0x5A7:17:2 (unit len=20)                connection test report
  EXPECT OK: 0x5A1:1:7:1DACC000 (unit len=24)        TC(17,1) completed
  EXPECT OK: 0x5A1:1:2:1DACC0010002 (unit len=32)    corrupt CRC rejected, FID 2
  EXPECT OK: 0x5A1:1:7:1DACC002@3 (unit len=24)      TC(9,129) completed
  EXPECT OK: 0x5E5:1000000 (unit len=37)             heartbeat CUC at 0x1000000x
  ```

  In each run: no TM(5,2) EID 0x542B.
- [x] **Probe self-test:** PASS.
- [x] **5.2 wrapper:** run from `/tmp/stix-uprev-qtest-scratch` as in Phase 1.
  - spw-tx stays ok.
  - Every new Phase 2 test is red, as expected, with
    `stix_spw_path: 'path' should not be NULL`.
- [x] **Golden files:**
  - `uart.golden.log` is unchanged through Phase 2.
  - `qemu-stdout.golden.log` was recaptured at P2-2, P2-4, P2-6 and P2-7. Each time only the
    expected events changed (P2-7: `stixspw_timecode_*` added, the two `stixctrl_scet_write`
    events of the 60 s resync gone).

## End-to-end gate (plan §5)

The run: QEMU `866ee37709` with `run-qemu-gs.sh --udp`, the ground station `0dcb1af` on
21503, **no bridge**. TCs were sent through the ground station's command endpoint
(`POST /api/sat/stix/commands/send`, the same path the UI uses). Transcript, 2026-11-10:

```
## send TC(17,1)                  -> {"hex":["1DACC000000519110100D77E"]}
## send TC(9,129) 0x10000000      -> {"hex":["1DACC001000B1909810010000000000035C6"]}
## TM history (non-heartbeat, newest first)
21:52:30 0x5a1 TM(1,7) appdata 1DACC001
21:52:28 0x5a1 TM(1,1) appdata 1DACC001
21:52:24 0x5a1 TM(1,7) appdata 1DACC000
21:52:24 0x5a7 TM(17,2)
21:52:23 0x5a1 TM(1,1) appdata 1DACC000
## latest heartbeats (CUC coarse at SP[10:14])
21:52:34 coarse 10000003
21:52:33 coarse 10000001
21:52:31 coarse 10000000
21:52:29 coarse 0000002d
```

- [x] TC(17,1) encodes to the reference bytes and gets TM(1,1), TM(17,2) and TM(1,7), all
  decoded in the GS history.
- [x] TC(9,129) gets TM(1,1) and TM(1,7). The heartbeat SCET follows the new time. The CRC
  `35C6` was checked independently.
- [x] The heartbeats arrive as bare Space Packets (`0DE5…`), with no SpW header.
- [x] The UI was demonstrated live to the owner over the bridge path before P2-7: TM viewer
  plus TC(17,1).

## Decisions in force (all implemented)

- **D-B (amended):** UDP carries bare Space Packets in both directions; `udp-spw-header=on`
  keeps the header.
- **D-J:** time codes on `QEMU_CLOCK_VIRTUAL`.
  - Emitted only when fine < 0x1000; otherwise the timer is rescheduled.
  - The 0xCC commit hook re-arms the timer.
  - The `timecodes` property can be changed at run time.
- **D-K:** TCP framing is `[SpW hdr][CCSDS]`, delimited by the CCSDS length.
  - `STIX_CONTROL` is stripped.
  - Resync on header byte 1 ≠ 0x02, or on a head that does not look like a TC.
- **D-L:** 64-unit no-spill RX queue; counter `rx-dropped`.
- **D-M:** control.IE gates every SpW IRQ, with a deferred pulse. RX delivery is held while
  IE = 0 (P2-5 M1).
- **D-N:** custom transport on the main loop, no chardev.

## Open items handed back

1. **TC without a DFH flag never reaches the FSW.** The TCP plausibility check (P2-6) resyncs
   on such a head, so it cannot provoke the FSW's TM(1,2) rejection. UDP does not have this
   check. Loosen the check to version + type only if that test case is wanted.
2. **The GS reads one UDP datagram every 50 ms** (`ATS-GroundStation app/connection.py:144`).
   A TM burst faster than 20 packets per second can be lost on the GS side, and QEMU's
   `tm-dropped` will not see it. This is a generic GS issue and a candidate for a PR after #12.
3. **`/tmp` leftovers:**
   - 4 `/tmp/stix-flash-*` directories from red runs. The worker's permission to delete them
     was removed after the permission classifier objected.
   - qtest's `/tmp` files on aborted red runs.
4. **Upstream qtests (qom-test and others) create a 17.7 GB sparse `flash.bin` in their working
   directory.** It uses 0 blocks, but it is a trap on the nearly full Windows disk. Point the
   default `flash-file` elsewhere or make it opt-in in Phase 3.
5. **The `grlib_stixwd.c` prints stay.** The device is not instantiated.
6. **Phase 3 inputs** (status 2026-11-10, after Phase 3):
   - NOMINAL mode needs TC(20,128) at least every 30 s
     (`drafts/2026-11-10-recon-tc20.md`) — **superseded by D-P3c:** the
     TC(20,128) reaction is disabled with TC(236,6) ReactionToS20 = 0
     in CONFIGURATION instead; there is no TC(20,128) generator and no
     GS reaction. See the NOMINAL recipe in `ground-station.md` and the
     Phase 3 checklist.
   - The HK ADC model: the boot TM(5,4) and the self-test
     `*E reference voltage` lines come from reading 0
     (`drafts/2026-11-10-recon-hk-adc.md`) — **done**, P3-1
     `f2e8664506`: the boot report is the success TM(5,1) 0x0022 and
     the reference-voltage lines are gone (accepted-diffs §22).
   - Link-error and AHB-error injection. (still open)
   - More than one TM client. (still open)

## Cleanup (run only after acceptance; the owner runs these)

```bash
rm -rf /tmp/stix-flash-* /tmp/stix-spin-*.elf            # red-run leftovers
rm -rf /home/laszlo/dev/qemu/.playwright-mcp             # browser check output in the main tree
# keep /tmp/stix-uprev-run, /tmp/stix-gs-run and /tmp/stix-baseline-run (persistent flash images)
# qemu-stix-tc: keep it for Phase 3, or remove it with
#   git -C /home/laszlo/dev/qemu-stix-uprev worktree remove /home/laszlo/dev/qemu-stix-tc
```
