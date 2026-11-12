# Attaching the ATS ground station to the STIX emulator

Status 2026-11-11 (the Phase 2 end-to-end run, plus Phase 4 fault injection):

| Direction | Status |
|---|---|
| **TM (emulator → ground station)** | Works end to end. Heartbeats, HK and TC reports are decoded in the ground station's UI and history, over direct UDP (`--udp`) or over TCP with the bridge. |
| **TC (ground station → FSW)** | Works end to end over direct UDP (`run-qemu-gs.sh --udp`, no bridge), verified 2026-11-10 with QEMU `866ee37709` and ground station `0dcb1af`. TC(17,1) gave TM(1,1), TM(17,2) and TM(1,7). TC(9,129) coarse 0x10000000 gave TM(1,1) and TM(1,7), and the heartbeat CUC then counted from 0x10000000. The transcript is in the Phase 2 acceptance checklist. Over TCP the bridge with `--forward-tc` reaches the same RX path (P2-6; shown in the 2026-11-10 bridge demo). |
| **Modes (SAFE → CONFIGURATION → NOMINAL)** | Works: the three TCs of the NOMINAL recipe below move the FSW through the modes. The gate `nominal-smoke.sh` passes in both modes (positive and `--negative`; gate record in `accepted-diffs.md` §23), including SID2 with the NOMINAL mode nibble. **A NOMINAL entry writes a flash context — use a scratch run dir** (see the recipe). |
| **Fault injection** | Works from the outside over QMP (Phase 4): HK ADC values, the HK hardware protection, time-code loss and SpW link-down. The gate `fault-smoke.sh` passes (positive and `--negative`; `accepted-diffs.md` §27-29). See "Fault injection" below. |
| **Detector (tiers 0+1)** | The FSW drives the modelled detector interface (FSM, QL banks, IRQ 3, IDeF-X registers; `accepted-diffs.md` §31-32) without errors. The gate `detector-smoke.sh` passes (positive and `--negative`; §33): the ASIC verification runs silently, and the NOMINAL-exit flush carries QL TM(21,6) SSID 30-34 on APID 0x5DC, all zero (no synthetic sky yet). `ql-check.py` decodes such packets. |

The findings, with file:line references on both sides, are in
`drafts/2026-11-10-gs-attach-report.md`.

The **platform controller** (`docs/stix-port/platform.md`, PL-7) is a
second way to attach the ground station: it relays TM to 21501 and
proxies the GS's TCs from 21502, replacing QEMU on these ports, so the
satellite entry below is unchanged (in the container it sends TM to
`host.docker.internal:21501`). Do not run `run-qemu-gs.sh --udp` or
`stix_gs_bridge.py` alongside it — both bind 21502; see platform.md's
"Troubleshooting".

## How it fits together

```
 QEMU (STIX model)            bridge (scripts/stix/gs)          ATS ground station
 TCP server :21500   ─TM─▶  strip 4-byte SpW header   ─UDP─▶  in_port  :21501
 (SpW hdr + CCSDS,          split on CCSDS length              space_packet layer
  no length prefix)                                            web UI :21503
                     ◀─TC─  add SpW hdr 40 02 00 00   ◀─UDP─  out_port :21502
```

The bridge exists because the two sides frame packets differently
(it is optional since P2-8 — see "Direct UDP (no bridge)" below):
- **QEMU** is a TCP server that streams whatever the FSW puts into the SpW
  TX DMA. Each unit is a 4-byte SpaceWire CCSDS-transfer header
  (`49 02 00 00` for OBC traffic, `30 02 00 00` for SSMM/science), followed
  by the CCSDS packet. There is no length prefix, so the stream is split on
  the CCSDS length field.
- **The ground station** speaks UDP only, with one bare Space Packet per
  datagram, and matches telemetry definitions by APID.

STIX APIDs are PID << 4 | category:

| APID | Content |
|---|---|
| 0x5E5 | heartbeat |
| 0x5A4 | HK |
| 0x5A7 | events |
| 0x5A1 | TC verification |
| 0x5BC / 0x5CC / 0x5DC | science |

## Ground station side: what it needs

- **Basic TM needs no code change.** It needs only configuration and data:
  - a satellite entry in `instance/satellites.json` (shown below);
  - the hand-written STIX definitions `definitions/telemetry/stix_tlm.json`.
    They decode the heartbeat plus the headers of HK, events and acks.
- **Full TM decoding from the IDB needs the branch's code changes**
  (`14f6ee4`: PUS key, bit fields, calibrations). See "Definitions
  generated from the IDB" below.
- **One code change, only while liboqs isn't installed.** Without the
  native liboqs, the ground station's PKI import raises `SystemExit`, and
  the satellite never connects. Commit `0635cad` on branch
  `stix-qemu-integration` catches that and runs with PKI disabled
  (9 lines in `app/connection.py`). With liboqs installed (needs `cmake` and
  `ninja`, i.e. sudo), the change isn't needed. Laszlo may want it on main
  anyway, as a robustness fix.
- **For TC (later, Phase 2), code changes ARE needed.** The ground station
  has no serializer for STIX's PUS TCs. STIX needs:
  - a big-endian secondary header with service and subtype per command;
  - subtypes ≥ 128;
  - big-endian parameters;
  - a CRC-16/CCITT-FALSE trailer.
  
  The planned shape is a `stix_pus` serializer (the same hook `sdls_ep`
  uses), plus `definitions/commands/stix_cmd.json`.

### Where the ground-station changes are (2026-11-10)

| What | Branch | Where |
|---|---|---|
| Generic fixes, proposed for `main` | `fix/robustness-and-html-escaping` | Pushed; **PR ateleris/ATS-GroundStation#12**, reviewer Beckicious. Worktree `/home/laszlo/dev/ATS-GroundStation-fixes`. |
| Everything, including the STIX-only parts | `stix-qemu-integration` | **Local only, not pushed.** Worktree `/home/laszlo/dev/ATS-GroundStation-stix`. |

What the PR contains:
- the cherry-picks of `0635cad`, `14f6ee4`, `5980aa2`, `8990d41` and `b4f8e31`;
- a fix-up commit `f9d5a6d`, which makes one test and `CLAUDE.md` independent of STIX.

What is STIX-only and stays on `stix-qemu-integration`:
- the definitions: `232efd3`, `0330e5f`, `b7fcdf5`;
- the converter: `0c3068c`, `b93bd1f`.

Once PR #12 is merged, rebase `stix-qemu-integration` onto the new `main`.
The five generic commits then drop out, apart from the fix-up's small
test and `CLAUDE.md` differences.

The full commit list of `stix-qemu-integration` (never merged to `main`):

```
2946e01 docs: STIX satellite over direct UDP to the QEMU emulator
0dcb1af commands: stix_pus stops encoding at the PUS size limit
1306f7e commands: stix_pus serializer (big-endian PUS TC with CRC-16/CCITT-FALSE)
b443eec commands: PUS service type and bit widths in STIX TC definitions
b4f8e31 telemetry: reject markup characters in units and calibration texts
8990d41 frontend: escape definition and telemetry text before inserting HTML
b7fcdf5 definitions: STIX TC generated from IDB v2.26.36
b93bd1f scripts: stix_idb_to_gs.py also converts telecommands (--tc-dir)
5980aa2 commands: refuse to send with a serializer that is not implemented
0330e5f definitions: STIX TM generated from IDB v2.26.36
0c3068c scripts: STIX IDB (SCOS-2000 MIB in SQLite) to TM definition converter
14f6ee4 telemetry: PUS packet key, bit fields and calibrations in TM definitions
232efd3 definitions: STIX FSW telemetry (heartbeat, HK SID, events, TC acks)
0635cad connection: keep the link up when liboqs is missing (PKI disabled)
```

The last six commits are **code changes** that the IDB definitions need:
- PUS matching by type, subtype and first-parameter key;
- bit fields and calibrations;
- a guard that refuses to send with a serializer that isn't implemented.

`8990d41` fixes HTML injection from definition and TM text. It applies
to `main` too, so it is a candidate for a cherry-pick there.

## Definitions generated from the IDB

`scripts/stix_idb_to_gs.py` on the branch converts the STIX instrument
database (SCOS-2000 MIB tables in SQLite) into ground-station JSON. Use
IDB **v2.26.36**, which matches the FSW in use. The STIXCore copy is at
`/home/laszlo/dev/STIX-TESTING/stix_ax_tm_processor/.venv/lib/python3.12/site-packages/stixcore/data/idb/v2.26.36/idb.sqlite`.

```bash
cd /home/laszlo/dev/ATS-GroundStation-stix
.venv/bin/python scripts/stix_idb_to_gs.py <idb.sqlite> \
    -o definitions/telemetry/stix_idb_tlm.json --tc-dir definitions/commands -v
```

What it produces:
- **TM:** all 246 packets. 201 are complete. 45 are partial: the
  variable-length (VPD) packets, which have only their fixed head decoded.
- **TC:** 100 commands, in `stix_idb_cmd_5ac.json` and
  `stix_idb_cmd_31c.json`.

The generated files are committed, so the converter only needs to be
re-run for a new IDB. Checked live against QEMU: all 246 packets decode,
and 112 of 112 sampled parameter values agree with STIXCore.

`stix_idb_tlm.json` supersedes the hand-written `stix_tlm.json`. Import
one of the two, not both.

## One-time setup

```bash
# Ground station worktree on its own branch (already exists on this machine)
git -C /home/laszlo/dev/ATS-GroundStation worktree add \
    /home/laszlo/dev/ATS-GroundStation-stix stix-qemu-integration
cd /home/laszlo/dev/ATS-GroundStation-stix
uv sync                                   # creates .venv (gitignored, ~44 MB)

# Satellite entry (instance/ is gitignored, created by the app on first run)
mkdir -p instance && cat > instance/satellites.json <<'EOF'
{
  "stix": {
    "id": "stix",
    "name": "STIX (QEMU)",
    "transport": { "out_host": "127.0.0.1", "out_port": 21502, "in_port": 21501 },
    "layer_settings": {},
    "outgoing_layers": ["space_packet"],
    "incoming_layers": ["space_packet"]
  }
}
EOF
```

Without TM frames or SDLS, the pipeline is just `space_packet`, so
CryptoLib isn't needed.

## Running it (three terminals)

Paths assume the main-line worktree `/home/laszlo/dev/qemu-stix-uprev` with
a build in `build/`. **Rebuild it first**
(`PATH=/home/laszlo/venvs/qemu-build/bin:$PATH make -C build -j8 qemu-system-sparc`).
A binary older than commit `21f898484f` (D-I) doesn't know the sparse,
inverted flash format:
- if `flash.bin` is missing, it writes a plain 16.5 GB file;
- if the new file is present, it reads it as all bad blocks.

```bash
# 1. Emulator: TM server on 21500. 21000 belongs to the qtests and gates,
#    so never use it here. Normal FSW ELF.
/home/laszlo/dev/qemu-stix-uprev/scripts/stix/gs/run-qemu-gs.sh \
    /home/laszlo/dev/qemu-stix-uprev/build/qemu-system-sparc

# 2. Ground station web app (startup takes ~30 s while liboqs-python tries
#    and fails to build liboqs; harmless with commit 0635cad)
cd /home/laszlo/dev/ATS-GroundStation-stix
.venv/bin/uvicorn main:app --host 127.0.0.1 --port 21503

# 3. Bridge (--forward-tc also passes TCs on to QEMU; they are dropped there today)
python3 /home/laszlo/dev/qemu-stix-uprev/scripts/stix/gs/stix_gs_bridge.py --forward-tc
```

Once per fresh `instance/`, import the STIX definitions. Use the
IDB-generated TM file; the commands are optional, because TC can't be
sent yet:

```bash
cd /home/laszlo/dev/ATS-GroundStation-stix
curl -X POST -H 'Content-Type: application/json' \
    --data @definitions/telemetry/stix_idb_tlm.json \
    http://127.0.0.1:21503/api/sat/stix/telemetry/definitions/import
for f in definitions/commands/stix_idb_cmd_*.json; do
  curl -X POST -H 'Content-Type: application/json' --data @"$f" \
      http://127.0.0.1:21503/api/sat/stix/commands/import
done
```

**Watch it:**
- the ground station UI at http://127.0.0.1:21503/sat/stix: the TM viewer
  fills with `STIX_TM_3_25_SID4_HEARTBEAT` about once per second, plus
  occasional HK;
- `curl 'http://127.0.0.1:21503/api/sat/stix/telemetry/history?limit=5'`;
- the bridge's packet counts per APID, every 10 s on its stdout;
- the FSW console: `tail -f /tmp/stix-gs-run/uart.raw | tr -d '\000'` (see
  `baseline/README.md`, "Watching a run").

**Stop:** Ctrl-C in each terminal, or `kill` the three PIDs. Don't use a
broad `pkill -f` from a shell whose own command line contains the pattern:
it kills that shell.

## Direct UDP (no bridge)

Since P2-8 the emulator itself speaks the ground station's framing
(decision D-B: one bare Space Packet per datagram, in both directions),
so the bridge is optional. `run-qemu-gs.sh --udp` adds two `-global`
options to the same QEMU command line:

- `grlib-stixspw.udp-tm-port=21501` — every TM packet also goes out as
  one UDP datagram to `127.0.0.1:21501`, the 4-byte SpW header stripped;
- `grlib-stixspw.udp-tc-port=21502` — QEMU binds 21502/udp and accepts
  each datagram from any source port as one TC unit, adding the SpW
  header (`40 02 00 00`) internally before the RX DMA.

`udp-tm-host` (default 127.0.0.1) and `udp-spw-header=on` (deliver the
header in both directions) exist for tools that want them; the ground
station needs neither. The TCP server on 21500 stays up, so
`tmtc-probe.py` still works alongside. **The bridge does not:** `--udp`
and `stix_gs_bridge.py` are mutually exclusive — both bind
127.0.0.1:21502, so whichever starts second fails (QEMU's realize error
lands only in `qemu.log`; `run-qemu-gs.sh` prints it when QEMU dies at
startup), and even without the bind clash both would feed TM to 21501,
so the ground station would see every packet twice. Stop the bridge
before starting `--udp`; the bridge is for runs without `--udp`.

```
 QEMU (STIX model)                        ATS ground station
 sendto 127.0.0.1:21501  ──TM datagrams─▶  in_port  :21501
 recv on 21502/udp       ◀─TC datagrams──  out_port :21502
                                         web UI   :21503
```

The satellite config is unchanged from "One-time setup" (in_port 21501,
out 127.0.0.1:21502). Two terminals instead of three:

```bash
# 1. Emulator with direct UDP:
/home/laszlo/dev/qemu-stix-tc/scripts/stix/gs/run-qemu-gs.sh --udp \
    /home/laszlo/dev/qemu-stix-tc/build/qemu-system-sparc

# 2. Ground station web app, as above
cd /home/laszlo/dev/ATS-GroundStation-stix
.venv/bin/uvicorn main:app --host 127.0.0.1 --port 21503
```

TCs are expected to work end to end, pending the reviewer's §5 run: the
UI sends TC(17,1) and the acks TM(1,1)/TM(17,2)/TM(1,7) should come back
as TM. That needs the ground station's `stix_pus` serializer and the IDB
command definitions, which live on branch `stix-qemu-integration` (see
"Where the ground-station changes are"). The bridge remains for TCP-only
setups (a ground station build that cannot speak UDP).

UDP-specific notes: there is no connection handshake, so datagrams sent
before the other side starts are simply lost (same as the TCP no-client
drop); datagram order is kept within one sender socket — which is what
the ground station uses for every TC — but NOT across different source
sockets, so a tool that opens a fresh socket per datagram can have its
bursts reordered (the model delivers in arrival order);
`tmtc-probe.py` stays TCP-only — it talks to 21500; and QEMU binds TC
on 127.0.0.1 only (`udp-tm-host` can point TM at a remote address, but
a remote ground station cannot send TC to this model).

## NOMINAL mode

The FSW boots into SAFE. Three telecommands move it to NOMINAL; all
three are in the ground station's IDB command definitions (branch
`stix-qemu-integration`), and the model gate `nominal-smoke.sh` runs
the same sequence (gate record in `accepted-diffs.md` §23).

**The recipe, in order:**

1. **Wait for the boot TC20 timer.** Enter CONFIGURATION only after
   TM(5,1) EID 0x142E has arrived — the boot-armed TC(20,128) timer
   fires about 30 s after boot, and its reaction would pull the FSW
   back to SAFE. (The gate waits until +50 s.)
   **PFM builds (the flight ELF) boot much slower**, and the order
   differs from the AX gate ELF: with the NAND latency model on,
   TM(5,1) 0x14A3 and 0x142E are NOT gated by FS init (measured on the
   release ELF: 0x14A3 at +16 s, 0x142E at +32 s, FS init later than
   +46 s), and the mode change waits on the filesystem/context. So wait
   for **both** TM(5,1) 0x0032 (Filesystem initialisation finished) and
   0x142E — in either order — and enter CONFIGURATION only 5 s after
   the **later** of the two, exactly what `scripts/stix/pfm-boot-smoke.sh`
   does. Measured FS-init time: about 62 s of guest time on the debug
   ELF (`accepted-diffs.md` §42); the model lower bound is ≈26 s.
2. **ZIX36001 "STIX Mode transition"** — TC(236,1) with
   "Operation mode (PIX00080)" = **4** (CONFIGURATION) and
   "Mode transition (PIX00081)" = **0** (MANUAL_NOMINAL, as
   used by the gate).
   Expected: TM(1,1), TM(5,1) `0021 02 04` (CONFIGURATION ← SAFE,
   reason TC), TM(1,7) — near-instant.
3. **ZIX36607 "STIX ASW config"** — TC(236,6) with "ASW" fixed **7**,
   "Parameter ID ASW (PIX00095)" = **4** (ReactionToS20) and
   "Parameter (236,6) (PIX00120)" = **0**, which disables the
   TC(20,128) reaction. Send it AFTER entering CONFIGURATION: the
   CONFIGURATION entry resets the RAM copy of the context, which wipes
   a value set in SAFE. Expected: TM(1,1) and TM(1,7) (the FSW sends
   two TM(1,7) here).
4. **ZIX36001 again** with "Operation mode" = **5** (NOMINAL),
   "Mode transition" = 0. Expected: TM(1,1) at once, then a few
   seconds later TM(5,1) `0021 04 05` (NOMINAL ← CONFIGURATION) and
   TM(1,7); SID2 (0x5A4, TM(3,25)) with mode byte 0x15 follows at the
   64 s HK cadence.

The raw TC units are in `scripts/stix/nominal-smoke.sh` (the FSW does
not check the sequence count; the CRC trailers are precomputed).

**Flash-context warning.** Any NOMINAL entry writes a flash context
(`ContextMgmt_deinit`), and every later boot **on that flash.bin**
starts from it — the TC20 reaction stays disabled, the other context
parameters come from it too, and a golden captured on such a flash
would differ. `run-qemu-gs.sh` has **no** fresh-flash mechanism: it
uses `STIX_GS_RUN_DIR` (default `/tmp/stix-gs-run`), creates the
directory, `cd`s into it and lets the device create `flash.bin` only
when it is missing — so a reused run dir silently reuses its old
flash (its only built-in refusal is `/tmp/stix-baseline-run`). For
NOMINAL sessions, point it at a **new, empty** directory and never
reuse it for a second NOMINAL session:

```bash
STIX_GS_RUN_DIR=/tmp/stix-gs-nominal-$(date +%Y%m%d-%H%M%S) \
    scripts/stix/gs/run-qemu-gs.sh --udp build/qemu-system-sparc
```

Never use `/tmp/stix-uprev-run` (the golden run dir) or
`/tmp/stix-baseline-run` (frozen). For scripted runs,
`nominal-smoke.sh` enforces all of this itself.

**What NOMINAL looks like here:**
- heartbeats (0x5E5) continue, and SID2 (0x5A4, TM(3,25)) arrives every
  64 s with mode 0x15;
- **no quick-look or science TM** — there is no detector/IDeF-X model
  and no ROTB data path;
- at NOMINAL entry the UART shows 384 `*E writing register … failed`
  lines (`XrayDetFDIR.c:731`): the ASIC register writes time out and
  the NOMINAL path carries on. Expected, not a failure;
- since P4-1, SID2 shows the commanded supplies as powered: POWERSTAT
  (hkCmdSta bits 9:5) follows the enables (`accepted-diffs.md` §25).
  Before P4-1 it read 0, so LV/HV/SpW showed off in every mode;
- one TM(5,2) 0x542C (no LUTs in flash) is also expected at entry.

## Energy LUT (ELUT)

The FSW bins each event's raw ADC value into one of 32 science energy
bins through the FPGA energy LUT. A fresh flash has none: at NOMINAL
entry the FSW applies an all-zero RAM table, which puts every ADC value
into bin 31. The flight ELUT can be uploaded by TC and stored in the
flash, like on the spacecraft. Plan: `2026-11-11-elut-plan.md`; gate
record: `accepted-diffs.md` §40.

### Index spaces

Detector and pixel numbers differ between the CSV, the TC and the FPGA.
The tool and gate name the space in every variable and option.

| Name | Space | Range | Where |
|---|---|---|---|
| `subcol` | sub-collimator = ICD detector index (number − 1) | 0-31 | CSV column `Detector` (0-based); first index of the FSW RAM table `ELUT[subcol][pix][33]` |
| `det_id_1based` | ICD detector number = `subcol` + 1 | 1-32 | TC(237,7) "Detector ID"; the FSW subtracts 1 |
| `fpga_det` | FPGA detector | 0-31 | dimension 1 of the FPGA ELUT `val[pix][fpga_det][adc]` at 0xE0000000 |
| `pix` | Caliste pixel | 0-11 | CSV column `Pixel`; TC "Pixel ID"; dimension 0 of both tables |
| `ebin` | science energy bin | 0-31 (32 = unassigned) | the FPGA ELUT value |
| `adc` | raw 12-bit ADC value | 0-4095 | dimension 2 of the FPGA ELUT; the CSV edges |

The chain:

```
CSV row (Pixel = pix p, Detector = subcol d, edges e[0..30])
  -> TC(237,7) structure: det_id_1based = d + 1, pix = p, B[0..32]
  -> FSW RAM ELUT[d][p][0..32]
  -> at apply, for each fpga_det f: d = utils_subcolIdFromFpgaId[f]
       FPGA val[p][f][adc] = the ebin of adc under B[] of row (d, p)
```

`fpga_det` → `subcol` is **not** the identity: only 0, 1 and 4 map to
themselves. An identity table would mis-bin 29 of 32 detectors, the
reversed table 23. The checker proves on every run that it can see both
mistakes.

**Boundaries: B0 = 0 and B32 = 0** (user decision 2026-11-11). B[1..31]
are the CSV's 31 edges. So bin 0 covers `adc` 0..e[0]−1, bin 31 covers
e[30]..4095, and no ADC value is unassigned. The ops value of the lowest
boundary is not known here.

**Residual risk.** No gate can prove that the CSV column `Detector`
means `subcol`. The GSW reader (`stx_read_elut.pro`, `DET_ID 0-31`),
stixpy and the ICD say it does, and the user believes so (not certain:
"0 = first detector, 31 = last"). If it were `fpga_det`, every gate
would still pass and 29 detectors would be mis-binned.

### Picking the CSV

The flight tables are in `STIX-CONF/elut/` (read-only). Pick by file
name or by the validity dates in `elut_index.csv`, **never by its index
column**: index 17 is used twice. The table current since 2026-06-15 is
`elut_table_20260529.csv`. The tool reads the file by path only and never
copies it.

### Recipe

The gate does everything and leaves a seeded flash:

```bash
# slot mode (any free slot; the slot dir's flash.bin is deleted on PASS,
# so keep it with --save-template; the target's directory must exist)
mkdir -p /tmp/stix-elut-template
STIX_PORT_BASE=auto \
STIX_ELUT_CSV=$HOME/dev/STIX-CONF/elut/elut_table_20260529.csv \
    scripts/stix/elut-smoke.sh build/qemu-system-sparc \
    --save-template /tmp/stix-elut-template/flash.bin

# legacy mode: hold the 21000 lock yourself; the seeded flash stays in
# /tmp/stix-elut-run until the next elut-smoke run
flock /tmp/stix-qtest-21000.lock env STIX_ELUT_CSV=... \
    scripts/stix/elut-smoke.sh build/qemu-system-sparc
```

Without `STIX_ELUT_CSV` (or the ELF) the gate prints `SKIP:` and exits
77. It takes about 6 minutes: two boots, 235 s and 110 s of probe time.

By hand, in CONFIGURATION (after the NOMINAL recipe's steps 1-3):

```bash
python3 scripts/stix/elut-upload.py encode --csv <CSV> --lut-id 1 \
    --emit hex     # 96 TC(237,7) units, then the TC(237,8) unit
```

Send the units in order, one every 0.75 s or slower, and wait for each
TM(1,7) before the next. Then send TC(237,8) and wait for its TM(1,7):
the FPGA is written and 4 flash copies are stored. **Then store the
context once**: entering NOMINAL stores it, and so does leaving
MAINTENANCE (its only exit is to SAFE, which runs `ContextMgmt_deinit`,
OpModeMgmt.c:144-147); until then the LUT ID lives in the RAM context
and a reboot loads LUT ID 0 again. `--emit probe-args`
prints the same sequence as `tmtc-probe.py` arguments, with the ack
expectations.

The FSW accepts both TCs only in CONFIGURATION or MAINTENANCE.

### What success looks like

- every TC(237,7) and the TC(237,8): TM(1,1) and TM(1,7), never TM(1,8);
- the TC(237,8) TM(1,7) comes after the apply and the store;
- at the NOMINAL entry: `0021 04 05` and TM(1,7), no TM(5,2) `542c03`
  (ELUT apply error). The TM(5,2) 0x542C with p1 = 1 (no temperature
  LUT) still comes once per boot: it is not about the ELUT;
- the UART of the upload boot: `could not find LUT (type=3, id=0` at
  boot and four `problem erasing LUT type=3 … expected for the first
  time` at the store (no file to delete yet). These are expected;
- the UART of every later boot: **no** `could not find LUT (type=3`,
  and no TM(5,3) 0x8030 (the flash holds a context);
- the FPGA ELUT (QMP `pmemsave` of 1 572 864 bytes at 0xE0000000) passes
  `elut-upload.py check-dump --csv <CSV> --dump FILE`. Before any
  NOMINAL entry in a boot it is all 0 (power-on RAM): the FSW writes the
  FPGA ELUT at the CONF → NOMINAL entry, not at boot.

### Template flash

A seeded flash (ELUT ID 1 and a context) can be reused as a template.

- **Preferred:** `elut-smoke.sh --save-template PATH`. It copies after
  the PASS and before any cleanup, refuses an existing target, a target
  under `/mnt/` or on a filesystem that keeps no holes, and checks the
  copy (apparent size equal, allocated < 64 MiB).
- **By hand, with QEMU stopped:**

  ```bash
  timeout 30 cp --sparse=always /tmp/stix-elut-run/flash.bin \
      /tmp/stix-elut-template/flash.bin
  ls -lh /tmp/stix-elut-template/flash.bin  # apparent: 16.5 GiB
  du -h  /tmp/stix-elut-template/flash.bin  # allocated: about 150 KiB
  ```

  Only to an ext4 path under `/tmp`, never under `/mnt/`.
- **Never overwrite an existing flash.** `/tmp/stix-gs-run/flash.bin`
  (the `STIX_GS_RUN_DIR` default of `run-qemu-gs.sh`) is your GS state:
  move it aside first. To use a template, copy it (sparse) into a new
  `STIX_GS_RUN_DIR`.
- **Warning: a non-sparse copy is 16.5 GiB and fills the host disk**
  (about 13 GB usable on C:; WSL's `df /` misleads, look at
  `df -h /mnt/c`). Never a plain `cp`, `rsync` without `-S`, `tar`
  without `-S`, or a copy from the Windows side.
- A template has a context: no 0x8030 at boot, and the context's
  settings (S20 = 0 after the gate) apply. Gates that expect 0x8030
  make their own fresh flash and never use it.

### DSS replay caveat

The DSS `.dssevs` events sit on a baseline of about `adc` 2000, with
lines at 2182-3820. The flight ELUT has 150 keV at `adc` 2223-2303, so
**on an ELUT-loaded flash a DSS replay lands almost entirely in bins
30/31**, outside the quick-look bands. That is not a bug: the DSS bench
ADC scale is not the flight calibration. detector-smoke always makes a
fresh flash, so the gates are not affected; a replay started by hand on
a template or a GS run dir holding one is.

## Fault injection

Since Phase 4 you can inject faults while the FSW runs and watch its
FDIR react in the ground station. The injection goes over QEMU's QMP
socket, not through the ground station. The model side is in
`accepted-diffs.md` §26-29. The gate `fault-smoke.sh` runs every case
below (§27).

### Starting QEMU with a QMP socket

`run-qemu-gs.sh` adds a QMP socket when `STIX_GS_QMP` is set:

```bash
RUN=/tmp/stix-gs-fault-$(date +%Y%m%d-%H%M%S)
STIX_GS_RUN_DIR=$RUN STIX_GS_QMP=$RUN/qmp.sock \
    scripts/stix/gs/run-qemu-gs.sh --udp build/qemu-system-sparc
```

- The script passes `-qmp unix:PATH,server=on,wait=off`, so QEMU does
  not wait for a QMP client.
- PATH must be absolute, contain no comma, and be under 108 bytes (the
  unix socket path limit). The script refuses anything else.
- Use a new, empty run dir. A link-down makes TMBuffer write flash
  backups (§29), and a NOMINAL entry writes a flash context (see
  "NOMINAL mode").

### Sending qom-set commands

QMP is one JSON object per line. The client reads the greeting and
sends `qmp_capabilities` before any other command. socat is not
installed here, so use this shell function (python3 only):

```bash
qom_set() {  # qom_set <socket> <QOM path> <property> <JSON value>
    python3 - "$@" <<'EOF'
import json, socket, sys
sock, path, prop, value = sys.argv[1:]
s = socket.socket(socket.AF_UNIX)
s.connect(sock)
f = s.makefile("rw")
f.readline()  # the greeting
for cmd in ({"execute": "qmp_capabilities"},
            {"execute": "qom-set", "arguments": {
                "path": path, "property": prop, "value": json.loads(value)}}):
    f.write(json.dumps(cmd) + "\n")
    f.flush()
    while "event" in (reply := json.loads(f.readline())):
        pass  # an async event, not the reply
print(json.dumps(reply))
EOF
}

qom_set $RUN/qmp.sock /machine/stixctrl 'adc-inject[24]' 1800
qom_set $RUN/qmp.sock /machine/stixctrl 'adc-inject[24]' 4294967295
qom_set $RUN/qmp.sock /machine/stixspw link-down true
```

It prints `{"return": {}}` on success and `{"error": ...}` otherwise.
QEMU accepts one QMP client at a time; the function disconnects after
each command.

For scripted runs, `tmtc-probe.py --qmp PATH` sends the same commands
on a schedule (`--send-at S --qmp-cmd JSON`) and checks the TM, as
`fault-smoke.sh` does. The probe is the TCP TM client on 21500, so it
cannot run next to the bridge (see "Notes and limits").

### The properties

| QOM path | Property | Values | Effect |
|---|---|---|---|
| `/machine/stixctrl` | `adc-inject[N]`, N = 0..127 | 0..0xFFF; 4294967295 (0xFFFFFFFF) = no override, the default | Replaces the reading of HK ADC channel N from the next conversion. It also feeds the HW protection comparator. Other values are rejected with a QMP error. Clear with the literal 4294967295: `-1` is rejected. |
| `/machine/stixspw` | `timecodes` | true (default), false | false stops the SpW time codes. true re-arms them at the next rollover. |
| `/machine/stixspw` | `link-down` | false (default), true | true: the link status reads READY, DE and the error IRQ come every 1 s, the FSW stops sending TM, time codes stop, and TCs from the ground are held. false: the link reads RUN again, and the held TCs are delivered. |

**Reset:** a device reset (a system reset, including the watchdog's)
clears every `adc-inject[N]` and `link-down`. `timecodes` is not
cleared: its timer survives a reset (§20), so set it back to true
yourself. `run-qemu-gs.sh` passes `-no-reboot`, so a reset there stops
QEMU anyway.

The channels the recipes use: 24 = DPU_1V5_V, 40 = PSU_T, 4 = DET_C.
The HW protection watches channels 0, 1, 2, 3, 4, 8, 12, 16 and 20
(1V5_C, 2V5_C, 3V3_C, SPW_C, DET_C, ATT_C, ATT_V, HV_17_32_V,
HV_01_16_V), with the reset thresholds 2282, 3416, 3826, 3826, 546,
751, 4095, 2323 and 2323 (§28).

### The SW limit recipe

**On the gate ELF every HK limit is 0..0xFFF**, so an injected 12-bit
value alone never trips a SW check. Tighten the limit first, with
ZIX36010 "STIX Configure FDIR function" (TC(236,10)):

1. **Wait for TM(5,1) EID 0x14A3** (FDIR enabled, about 14 s after
   QEMU starts). A TC(236,10) sent before it cancels the automatic FDIR enable
   (FDIR.c:1022): 0x14A3 then never comes, and the HW protections stay
   off.
2. **Send ZIX36010 with the persistence count** (paramType
   `*_CONSECUTIVE_MEASUREMENTS` = 34) set to 3. The FSW boots with the
   count 255, not 3 (an FSW finding, §27), so without this TC the line
   must stay out of limits for 255 consecutive 1 s checks.
3. **Send ZIX36010 with the limit.**
4. **Inject** the value.

Each TC gives TM(1,1) and TM(1,7) in the same second. The parameters,
with the names in the ground station's IDB command definitions:

| Step | FDIR mask (PIX00107) | FDIR parameter ID (PIX00289) | Parameter value (236,10) (PIX00290) |
|---|---|---|---|
| 1V5 count | 0x00080000 (524288) | 34 (0x22) | 3 |
| 1V5 low limit | 0x00080000 (524288) | 1 (1V5 low limit) | 1951 (0x79F) |
| PSU_T count | 0x00100000 (1048576) | 34 (0x22) | 3 |
| PSU_T upper limit | 0x00100000 (1048576) | 2 (PSU upper limit) | 3800 (0xED8) |

The raw application data is `fdirMask u32, paramType u8, value u32`,
big-endian (TCData.h:151-153), for example `00080000 22 00000003`. The
complete TC units are in `scripts/stix/fault-smoke.sh`.

The limits stay tightened after the injection is cleared. The nominal
readings (2048 on 1V5_V, 2300 on PSU_T) are inside them. A value back
in limits only resets the counter; there is no recovery event.

### Expected TM

All events are on APID 0x5A7. The data is shown as the gate matches it
(the EID, then the parameters).

| Case | TC first | Injection | Expected TM |
|---|---|---|---|
| 1V5 low | 1V5 count, 1V5 low limit | `adc-inject[24]` = 1800 | TM(5,3) `9449 07 0708 079F` (id 7, value, limit) and TM(5,4) `D420 00080000`, about 3 s later (the third 1 s check) |
| PSU temp high | PSU_T count, PSU_T upper limit | `adc-inject[40]` = 3900 | TM(5,3) `9449 17 0F3C 0ED8` and TM(5,4) `D420 00100000`, about 3 s later |
| DET_C HW protection | none (all nine lines are armed at 0x14A3) | `adc-inject[4]` = 600 | TM(5,3) `9434 05 0258 0222` (id 5, sample, threshold 546), within 1 s |
| Time-code loss | none | `timecodes` = false | TM(5,2) `542B` (no parameters) about 61 s later. From then on the CUC carries bit 31 |
| | | `timecodes` = true | TM(5,1) `1429` within 1 s |
| Link-down | none | `link-down` = true, for at least 11 s (the gate holds 20 s) | nothing during the outage, not even heartbeats |
| | | `link-down` = false | TM(5,2) `5435 48`, once or twice, and TM(5,1) `142D 00`, all within 1 s |

Notes:
- Clear each `adc-inject[N]` when the case is done (4294967295).
- After a HW protection trip the FSW masks that line's IRQ. A second
  injection on the same line in the same boot gives no 0x9434.
- The FSW reports a 2V5_C trip with id 1, like 1V5_C (FDIR.c:869).
  ATT_V's threshold 4095 cannot be exceeded by a 12-bit value.
- During a link-down all TM is queued. The 0x5435 reports come out
  after the clear. Their parameter 0x48 means external, READY, line 0
  (§29).

**Every HK trip signals a move to SAFE.** The FSW sends the SAFE
signal for each 0x9449 and 0x9434 (FDIR.c:629, :941), and the UART
prints `Transitioning to safe mode`. In SAFE nothing changes. In
NOMINAL the FSW leaves NOMINAL, so run the fault cases in SAFE, or
re-enter NOMINAL afterwards with the recipe above. Time-code loss and
link-down do not change the mode in the source (Timing.c:63-91,
FDIR.c:768-776).

## Notes and limits

- **Order doesn't matter.** The bridge retries the QEMU connection. QEMU
  drops TM produced while no client is connected (no buffering), so the
  first packets of a boot are lost if the bridge starts late.
- **One TM client at a time.** QEMU accepts a single TCP client, so don't
  run `tmtc-probe.py` on 21500 while the bridge is connected.
- **The bridge and `--udp` are mutually exclusive.** Both bind
  127.0.0.1:21502 (the bridge's TC listener, QEMU's TC socket) and both
  would send TM to 21501. Whichever starts second fails — the bridge
  with an OSError traceback, QEMU with a realize error that
  `run-qemu-gs.sh` surfaces only when QEMU dies within the first
  second. Run one or the other.
- **Definition matching.** On `main`, definitions are keyed by APID only.
  HK SID 1 and SID 2 share 0x5A4, so only the SID field can be decoded.
  Commit `14f6ee4` on the branch adds matching by APID + PUS type +
  subtype + first parameter (the SID or event ID), which every packet in
  the IDB file uses.
- **The definition store re-reads its JSON file for every packet.** That
  is fine at the current TM rate, but a known inefficiency.
- **The first time field has bit 31 set** (`0x800000F8`). That is the CUC
  "not synchronised" flag: no time sync has been sent, which TC needs.
- **Ports used:**

  | Port | Use |
  |---|---|
  | 21500/tcp | QEMU TM (TCP; the bridge or `tmtc-probe.py` connects) |
  | 21501/udp | ground station TM in (bridge forwards, or QEMU sends direct with `--udp`) |
  | 21502/udp | ground station TC out (the bridge listens, or QEMU binds it with `--udp`) |
  | 21503/tcp | ground station web UI |
