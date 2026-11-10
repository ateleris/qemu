# Ground station attach spike (2026-11-10)

Sub-agent report, verified by the orchestrator: GS main clean, branch `stix-qemu-integration` (2 commits), processes stopped. Bridge and QEMU launcher: `scripts/stix/gs/`; how-to: `docs/stix-port/ground-station.md`.

## Outcome: yes, the ground station receives and decodes the emulator's TM, both in its web UI and in its stored history. TC cannot reach the FSW yet.

**Evidence from the run** (QEMU r3 binary, ASW_em_baseline.elf, about 2 minutes):
- **Bridge counts:** 101 packets on APID 0x5E5 (heartbeat) and 1 on 0x5A4 (HK), all with SpW target 0x49. 102 forwarded to the ground station, 0 resync bytes.
- **Ground station history:** 97 entries.
  - 88 decoded as `STIX_TM_3_25_SID4_HEARTBEAT`.
  - 8 heartbeats came in before I imported the definition, so they show as raw.
  - 1 decoded as `STIX_TM_3_25_HK`: 109 B, SID = 2 (maxi).
- **Decoded heartbeat example:** PUS version byte 0x10, service 3/25, destination 14, time coarse 0x800000F8 (the top bit is set, which looks like the "not synchronised" CUC flag), SID 4, coarse time, flare message, Z/Y location, flare duration, attenuator motion flag. All zero apart from SID and time, which is plausible for an idle instrument.
- **Screenshot:** the station page with the TM viewer filling with heartbeats, status "Connected": `/tmp/stix-gs-work/gs-station.png`.
- **No buffering in QEMU:** TM produced while no client is connected is dropped, so the first sequence counts are lost (`send_tm_packet`, `tmtc_io_server.c:106-109`).

## How to reproduce
1. **QEMU:** `/tmp/stix-gs-work/run-qemu.sh &`. This is your command line, run from `/tmp/stix-gs-run`. It filters QEMU's very noisy MMIO printfs on stdout down to the connect and packet lines in `/tmp/stix-gs-run/qemu.log`.
2. **Ground station:** `cd /home/laszlo/dev/ATS-GroundStation-stix && .venv/bin/uvicorn main:app --host 127.0.0.1 --port 21503 &`
   - It reads `instance/satellites.json` (gitignored). That defines satellite `stix`: UDP `in_port` 21501, `out_host:out_port` 127.0.0.1:21502, in and out pipelines `["space_packet"]` only, no TM frame layer and no SDLS.
   - Startup takes about 30 s because liboqs-python tries to clone and build liboqs and fails (see blockers).
3. **Bridge:** `python3 /tmp/stix-gs-work/stix_gs_bridge.py --forward-tc &`
4. **Definitions (once per fresh `instance/`):** `curl -X POST -H 'Content-Type: application/json' --data @definitions/telemetry/stix_tlm.json http://127.0.0.1:21503/api/sat/stix/telemetry/definitions/import`
5. **View:** open http://127.0.0.1:21503/sat/stix, or `curl 'http://127.0.0.1:21503/api/sat/stix/telemetry/history?limit=5'`.
6. **Stop:** `kill` the uvicorn, bridge and qemu-system-sparc PIDs. Don't use a broad `pkill -f` from a shell whose own command line contains the pattern; it killed my shell. Everything is stopped now, and ports 21500-21503 are free.

## Framing and transport
**Ground station side:**
- UDP is its only transport, one datagram per Space Packet: `app/transport/udp.py:18-106`. It is hard-wired at `app/connection.py:26,454-458`.
- There is no TCP and no TSIM length prefix. TM is polled every 50 ms: `app/connection.py:136-150`.
- The incoming `space_packet` layer parses raw CCSDS. TM definitions are matched by APID only (`app/telemetry/store.py:85-93`), with a `byte_order: big` option.

**QEMU side:**
- `send_tm_packet` writes the TX-descriptor buffer as-is, with no length prefix (`tmtc_io_server.c:103-138`, called from `grlib_stixspw.c:246`).
- That buffer is not bare CCSDS. Every unit is a **4-byte SpW CCSDS-packet-transfer header followed by the CCSDS packet**. The header is target 0x49 (OBC) or 0x30 (SSMM, used for science), then 0x02, 0x00, 0x00 (FSW `TcTm/TMData.h:427-436`). That header is the "first byte other than 0x49" you mentioned.
- So you can't key on byte 0. Find packet boundaries with byte 1 == 0x02 plus the CCSDS length field.

**What I chose:** a bridge (option b). It is stdlib Python, about 110 lines:
- TCP client to port 21500, sends the `STIX_CONTROL` token.
- Splits the stream into SpW header + Space Packet, strips the header, and sends each packet as a UDP datagram to the ground station.
- Listens on UDP 21502 for TCs from the ground station, prepends the TC SpW header `40 02 00 00` (FSW `TCData.h:30-36`), and with `--forward-tc` writes it unframed to QEMU.

APIDs seen are PID<<4 | category (`TMData.h:439-448`):

| APID | Content |
|---|---|
| 0x5E5 | heartbeat |
| 0x5A4 | HK |
| 0x5A7 | events |
| 0x5A1 | TC verification |
| 0x5BC / 0x5CC / 0x5DC | science |

The TM data field header is 10 bytes: PUS byte, service, subtype, destination, CUC 4+2 (`TMData.h:411-423`).

## TC: what is missing
**Transport test.** I sent one TC through the ground station's raw-send endpoint: `15ACC001000319110100`. It reached the bridge and then QEMU, which logged `recv_packet_callback` and dropped it. The TCP path works; everything behind it is a stub.

**What the FSW expects** in its RX buffer:
- `40 02 00 00`, then a PUS TC.
- 6-byte primary header: type 1, secondary-header flag 1, APID 0x5AC (PID 90, category 12, `TCData.c:202`), sequence flags 11.
- 4-byte data field header: flag/PUS version/ack nibble, service, subtype, source ID (`TCData.h:66-75`).
- Application data, then CRC-16 over everything before the CRC.
- At most 248 B (`TCData.h:25-28`).
- The CRC is CCITT-FALSE: poly 0x1021, init 0xFFFF (`BSW/Utils/Common.c:13`, `TCVerifStatic.c:195-213`). That is the same algorithm as the ground station's `app/models/ccsds/crc.py`.

**Ground station gaps** (on the branch, not done):
1. The secondary header is per subsystem, little-endian, and only accepts a 7-bit `func_code` (`app/commands/serializer.py:69-100`, `app/commands/models.py:60-80`). STIX needs service and subtype per command, and subtypes ≥128 are common.
2. Parameter serialization is little-endian only (`serializer.py:11-47`); STIX is big-endian.
3. There is no packet CRC trailer. Post-wrap compute can only patch header fields (`space_packet_layer.py:~110-150`).
4. Raw-send forces the secondary-header flag to 0 (`routes.py:66-90`).
5. The SpW `40 02 00 00` header has to be added somewhere; the bridge does it today.

The cleanest fix is a custom `serializer: "stix_pus"`, using the existing hook that `sdls_ep` already uses (`SubsystemDefinition.serializer`), plus a CRC trailer and a `definitions/commands/stix_cmd.json`.

**QEMU-side gaps** (inputs for the Phase 2 plan):
1. **RX is dead code.** `recv_packet_callback` (`grlib_stixspw.c:76-157`) is commented out and still uses TSIM-era calls (`tsim_read_cpu_mem`, `swap32`, `rxIndex`).
2. **Implementing RX needs:**
   - Read the RX descriptor at `dmaRxDescAddr + 8*rxIndex` with `physical_memory_read` and bswap it.
   - Require EN set, write the data to `daddr`, honour `dmaMaxRxLength` and the TR flag.
   - Write back length plus EN cleared, wrap on WR.
   - Set `dmaControl.PR` and pulse the IRQ.
   - The FSW ISR needs exactly this sequence: `SPW.c:291-350`.
3. **Threading:** `tcHandler` is a raw pthread (`tmtc_io_server.c:58-101`). Touching guest memory or IRQs from it needs the BQL, or better, moving the socket to QEMU's main loop (chardev/QIOChannel).
4. **Framing:** TCP is a stream, so packets need reassembly. Either keep the TSIM convention the old code assumed (it skips 8 bytes: a 4-byte length and a 4-byte TSIM header, see the commented code at `tmtc_io_server.c:112-130`), or parse SpW header + CCSDS length like the bridge does. Pick one for both directions and say so in the docs. TM is currently unframed, which is asymmetric.
5. **Register clobbering:** `grlib_stixspw_write` overwrites `status = 0xA00000` and zeroes `dmaControl` apart from PS on every write (`:198-280`). That would wipe the TO bit and the RX enable/RD state; RX needs proper write-1-to-clear semantics.
6. **Timecode:** the old 4-byte ESC-char form sets `reg.time` and `STATUS_TO` (`:96-104`); the FSW calls `SPW_callbackTime` (`SPW.c:200-207`).
7. **Smaller items:**
   - TM is dropped while no client is connected; optionally buffer it.
   - Only one client is accepted at a time (`listen(...,1)`).
   - The `STIX_CONTROL` token reaches the RX callback as if it were data; it must be stripped or handled.

## Blockers and things that need Laszlo
1. **No native liboqs, and no `cmake` or `ninja` to build it.** liboqs-python tries to clone and build at import time, fails, and raises `SystemExit`. `app/pki/manager.py:21` imports oqs at module level, and `get_or_connect` imported it unguarded, so the satellite never connected.
   - The fix needed one minimal code change (commit 1 below): catch the error, log "PKI disabled", leave `pki_manager=None` (the routes already answer 503).
   - The UI shows 503s for handshake and PKI status, which is harmless.
   - APQS/SDLS (CryptoLib) is not needed for a plain `space_packet` pipeline and was never loaded.
   - To get PKI/APQS back: install cmake, ninja and libssl-dev (already present), then let liboqs build into `~/_oqs`, and run `scripts/build_cryptolib.sh`.
2. **No headless mode.** The ground station is a FastAPI web app; its REST API doubles as the CLI. No credentials were needed.

## Ground station branch `stix-qemu-integration`
Worktree `/home/laszlo/dev/ATS-GroundStation-stix`, not pushed. The main checkout is still clean at 2003a46.

`git log --oneline main..stix-qemu-integration`:
- `232efd3` definitions: STIX FSW telemetry (heartbeat, HK SID, events, TC acks)
- `0635cad` connection: keep the link up when liboqs is missing (PKI disabled)

Diff stat:
- `app/connection.py`: 11 lines changed (+9 / -2)
- `definitions/telemetry/stix_tlm.json`: 154 lines added

Definition limitation: the ground station keys definitions by APID only. HK SID 1 and SID 2 share 0x5A4, so only the SID is decoded there; events decode only the event ID.

## Created on disk
**In the ground station worktree:**
- `.venv`: 44 MB, gitignored.
- `instance/`: 704 KB, gitignored; mostly 640 KB of telemetry history plus `satellites.json`.

**In `/tmp/stix-gs-work` (252 KB total):**
- `stix_gs_bridge.py` (4.7 KB)
- `run-qemu.sh` (0.6 KB)
- `gs-station.png` (215 KB)
- `bridge.log` (1.3 KB)
- `gs.log` (13 KB)

**In `/tmp/stix-gs-run` (36 KB on disk):**
- `flash.bin`: sparse, 17.7 GB apparent, 16 KB real.
- `qemu.log` (11 KB)
- `uart.raw` (3.6 KB)

**Not left behind:** no `~/_oqs` was created. The Playwright MCP wrote a temporary `.playwright-mcp/` into `/home/laszlo/dev/qemu`; I moved the screenshot out and deleted the folder. Nothing was touched in qemu-stix-r3, qemu-stix-uprev, STIX-FSW, stix_asw_src or `/tmp/stix-baseline-run`.