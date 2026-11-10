# Attaching the ATS ground station to the STIX emulator

Status 2026-11-10:

| Direction | Status |
|---|---|
| **TM (emulator → ground station)** | Works end to end. Heartbeats and HK are decoded in the ground station's UI and history. |
| **TC (ground station → FSW)** | Reaches QEMU, but the model's SpW receive path is a stub, so the TC is dropped. Not usable until roadmap Phase 2. |

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

The bridge exists because the two sides frame packets differently:
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

- **For TM, no code change is needed.** Only configuration and data:
  - a satellite entry in `instance/satellites.json` (shown below);
  - the STIX telemetry definitions `definitions/telemetry/stix_tlm.json`,
    which are data (JSON), not code.
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

All ground-station changes live on branch **`stix-qemu-integration`**
(worktree `/home/laszlo/dev/ATS-GroundStation-stix`), never on `main`:

```
232efd3 definitions: STIX FSW telemetry (heartbeat, HK SID, events, TC acks)
0635cad connection: keep the link up when liboqs is missing (PKI disabled)
```

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

Once per fresh `instance/`, import the STIX telemetry definitions:

```bash
cd /home/laszlo/dev/ATS-GroundStation-stix
curl -X POST -H 'Content-Type: application/json' \
    --data @definitions/telemetry/stix_tlm.json \
    http://127.0.0.1:21503/api/sat/stix/telemetry/definitions/import
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

## Notes and limits

- **Order doesn't matter.** The bridge retries the QEMU connection. QEMU
  drops TM produced while no client is connected (no buffering), so the
  first packets of a boot are lost if the bridge starts late.
- **One TM client at a time.** QEMU accepts a single TCP client, so don't
  run `tmtc-probe.py` on 21500 while the bridge is connected.
- **Definitions are keyed by APID only** in the ground station. HK SID 1
  and SID 2 share 0x5A4, so only the SID field is decoded for HK, and
  events decode only the event ID.
- **The first time field has bit 31 set** (`0x800000F8`). That is the CUC
  "not synchronised" flag: no time sync has been sent, which TC needs.
- **Ports used:**

  | Port | Use |
  |---|---|
  | 21500/tcp | QEMU TM |
  | 21501/udp | ground station TM in |
  | 21502/udp | ground station TC out (the bridge listens) |
  | 21503/tcp | ground station web UI |
