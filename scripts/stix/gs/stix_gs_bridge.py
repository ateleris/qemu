#!/usr/bin/env python3
"""Bridge between the STIX QEMU TMTC TCP server and the ATS ground station (UDP).

TM:  QEMU (TCP server, 127.0.0.1:21500) streams what the FSW hands to the
     SpW TX DMA: [4-byte SpW CCSDS-Packet-Transfer header][CCSDS TM packet]
     back-to-back, no length prefix. Header = target(0x49 OBC / 0x30 SSMM),
     protocol id 0x02, 0x00, 0x00 (FSW TMData.h TMData_CCSDS_*).
     We split the stream on the CCSDS length field, strip the SpW header and
     send each Space Packet as one UDP datagram to the ground station in_port.

TC:  The ground station sends each TC Space Packet as one UDP datagram to its
     out_port. We listen there, log it, prepend the SpW header the FSW expects
     for TCs (target 0x40, 0x02, 0x00, 0x00 - FSW TCData.h TCDATA_CCSDS_*)
     and, with --forward-tc, write it to the QEMU TCP socket unframed. The
     QEMU model's TC receive path is a stub today, so nothing reaches the FSW.

Stdlib only.  Usage:
  stix_gs_bridge.py [--qemu-port 21500] [--gs-in-port 21501] [--gs-out-port 21502]
                    [--forward-tc] [--stats 10]
"""
import argparse
import collections
import select
import socket
import sys
import time

SPW_HDR = 4      # SpW CCSDS packet transfer header
SP_HDR = 6       # CCSDS primary header
SPW_TC_HDR = bytes([0x40, 0x02, 0x00, 0x00])


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def connect(port):
    while True:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=2)
            s.sendall(b"STIX_CONTROL")   # vestigial token; the model ignores it
            s.setblocking(False)
            log(f"connected to QEMU TMTC server 127.0.0.1:{port}")
            return s
        except OSError:
            time.sleep(0.5)


def split_packets(buf, stats):
    """Pop complete [SpW hdr][SP] units from buf; return list of SP bytes."""
    out = []
    while len(buf) >= SPW_HDR + SP_HDR:
        if buf[1] != 0x02:               # not a CCSDS-over-SpW header: resync
            stats["resync_bytes"] += 1
            del buf[0]
            continue
        sp_len = SP_HDR + ((buf[SPW_HDR + 4] << 8) | buf[SPW_HDR + 5]) + 1
        if len(buf) < SPW_HDR + sp_len:
            break                        # wait for the rest
        target = buf[0]
        sp = bytes(buf[SPW_HDR:SPW_HDR + sp_len])
        del buf[:SPW_HDR + sp_len]
        apid = ((sp[0] & 0x07) << 8) | sp[1]
        stats[f"apid 0x{apid:03X} (target 0x{target:02X})"] += 1
        out.append(sp)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu-port", type=int, default=21500)
    ap.add_argument("--gs-host", default="127.0.0.1")
    ap.add_argument("--gs-in-port", type=int, default=21501, help="GS UDP TM in_port")
    ap.add_argument("--gs-out-port", type=int, default=21502, help="GS UDP TC out_port (we listen)")
    ap.add_argument("--forward-tc", action="store_true", help="write TCs (SpW hdr + SP) to QEMU")
    ap.add_argument("--stats", type=float, default=10.0, help="stats interval in s")
    args = ap.parse_args()

    udp_out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_tc = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_tc.bind(("127.0.0.1", args.gs_out_port))
    log(f"TM -> udp {args.gs_host}:{args.gs_in_port}; TC <- udp 127.0.0.1:{args.gs_out_port}")

    stats = collections.Counter()
    qemu = connect(args.qemu_port)
    buf = bytearray()
    next_stats = time.monotonic() + args.stats
    while True:
        r, _, _ = select.select([qemu, udp_tc], [], [], 1.0)
        if qemu in r:
            try:
                chunk = qemu.recv(65536)
            except BlockingIOError:
                chunk = None
            if chunk == b"":
                log("QEMU closed the connection; reconnecting")
                qemu.close()
                buf.clear()
                qemu = connect(args.qemu_port)
                continue
            if chunk:
                buf += chunk
                for sp in split_packets(buf, stats):
                    udp_out.sendto(sp, (args.gs_host, args.gs_in_port))
                    stats["tm_forwarded"] += 1
        if udp_tc in r:
            tc, _ = udp_tc.recvfrom(65536)
            stats["tc_from_gs"] += 1
            log(f"TC from GS ({len(tc)} B): {tc.hex()}")
            if args.forward_tc:
                qemu.sendall(SPW_TC_HDR + tc)
                stats["tc_to_qemu"] += 1
        if time.monotonic() >= next_stats:
            next_stats += args.stats
            log("stats: " + ", ".join(f"{k}={v}" for k, v in sorted(stats.items())))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
