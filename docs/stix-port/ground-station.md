# Attaching the ATS ground station to the STIX emulator

Status 2026-11-10 (after the Phase 2 end-to-end run):

| Direction | Status |
|---|---|
| **TM (emulator → ground station)** | Works end to end. Heartbeats, HK and TC reports are decoded in the ground station's UI and history, over direct UDP (`--udp`) or over TCP with the bridge. |
| **TC (ground station → FSW)** | Works end to end over direct UDP (`run-qemu-gs.sh --udp`, no bridge), verified 2026-11-10 with QEMU `866ee37709` and ground station `0dcb1af`. TC(17,1) gave TM(1,1), TM(17,2) and TM(1,7). TC(9,129) coarse 0x10000000 gave TM(1,1) and TM(1,7), and the heartbeat CUC then counted from 0x10000000. The transcript is in the Phase 2 acceptance checklist. Over TCP the bridge with `--forward-tc` reaches the same RX path (P2-6; shown in the 2026-11-10 bridge demo). |
| **Modes (SAFE → CONFIGURATION → NOMINAL)** | Works: the three TCs of the NOMINAL recipe below move the FSW through the modes. The gate `nominal-smoke.sh` passes in both modes (positive and `--negative`; gate record in `accepted-diffs.md` §23), including SID2 with the NOMINAL mode nibble. **A NOMINAL entry writes a flash context — use a scratch run dir** (see the recipe). |

The findings, with file:line references on both sides, are in
`drafts/2026-11-10-gs-attach-report.md`.

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
- SID2 shows LV/HV/SpW **off** even in NOMINAL, because the model
  leaves POWERSTAT (hkCmdSta bits 9:5) at 0 (D-P3a); one TM(5,2)
  0x542C (no LUTs in flash) is also expected at entry.

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
