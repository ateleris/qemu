#!/usr/bin/env python3
"""Probe the STIX model's TMTC TCP server for telemetry.

Connects to the model's TMTC port immediately after QEMU start, sends the
vestigial "STIX_CONTROL" handshake token (for protocol fidelity — the model
never checks it), then reads whatever arrives.

Framing finding (2026-11-09, old build probe): the QEMU bridge sends RAW
packets with NO 4-byte TSIM length prefix (that prefix is added only by the
FSW-side ioTcpServer). The stream is back-to-back CCSDS-looking TM packets
(37 B housekeeping cadence ~2 s). This probe therefore logs raw chunks; it
does not frame.

Usage: tmtc-probe.py [--port PORT] [--seconds N]
       (prints 'NO TM OBSERVED' and exits 1 if nothing arrived)
"""
import argparse
import socket
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    import os
    ap.add_argument("--port", type=int,
                    default=20000 + os.getuid(),  # IO_TCP_SERVER_BASE_PORT + getuid()
                    help="TMTC server port (default: 20000+getuid)")
    ap.add_argument("--seconds", type=float, default=120)
    args = ap.parse_args()

    deadline = time.monotonic() + args.seconds
    # Retry connect for a few seconds in case QEMU is still binding.
    sock = None
    while time.monotonic() < deadline and sock is None:
        try:
            sock = socket.create_connection(("127.0.0.1", args.port), timeout=2)
        except OSError:
            time.sleep(0.5)
    if sock is None:
        print(f"NO TM OBSERVED: could not connect to 127.0.0.1:{args.port}")
        return 1

    sock.sendall(b"STIX_CONTROL")  # 12-byte handshake token (vestigial)
    print(f"# connected to 127.0.0.1:{args.port}, listening {args.seconds}s")

    sock.settimeout(1.0)
    saw_any = False
    total = 0
    while time.monotonic() < deadline:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        except OSError:
            break  # server closed
        if not chunk:
            break
        saw_any = True
        total += len(chunk)
        ts = time.strftime("%H:%M:%S")
        print(f"{ts} +{len(chunk):4d}B {chunk[:48].hex()}")
    if not saw_any:
        print("NO TM OBSERVED")
        return 1
    print(f"# total {total} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
