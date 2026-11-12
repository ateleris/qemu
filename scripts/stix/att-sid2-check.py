#!/usr/bin/env python3
"""Check the SID2 housekeeping TM(3,25) status4 attenuator switch bits.

Input: a dump as tmtc-probe.py --dump-apid 0x5A4:FILE writes it (the
att-smoke silence run): one whole CCSDS packet per line, hex, WITHOUT
the 4-byte SpW header, so the application data starts at byte 16 (the
10-byte TM data field header follows the 6 CCSDS primary-header
bytes). Blank lines and lines starting with '#' are ignored.

The SID 2 layout is TM_3.c's TMForm_createTM_3_25_SID_2 (TM_3.c:20-167):
application byte 0 is the SID (2), byte 1 the mode byte (SW running
1b | instrument number 3b | instrument mode 4b), then sixteen 12-bit
ADC pairs (3 bytes each), then status1..status4 as big-endian u32 at
application bytes 50, 54, 58 and 62. In status4, bit 14 is the
position-AB flag and bit 13 the position-BC flag (HKAcquire.c:378-379)
— the switches as the FPGA presents them to the FSW.

--expect-bc-closed is the att-smoke silence-run check: in the LAST
packet of the dump (the caller guarantees it postdates the move; the
smoke counts the probe log's 0x5A4 stamps after the TC's send), bit 13
(BC) must read 1 and bit 14 (AB) 0: with sensor_bc.stuck_closed the
presented switches say BC actuated and AB released, so the FSW reads
"IN" while the mechanism is one 10 ms debounce off OUT. The plan's
original wording expected both bits set; the model as built ends the
move one debounce off the end, which releases the healthy AB, so only
BC sets (Doc vs code 12). [L] confirm on the live run.

Usage:
  att-sid2-check.py DUMP [--expect-bc-closed]
  att-sid2-check.py --self-test
Exit codes: 0 decoded (and the check passed), 1 a decode error or a
failed check, 2 bad usage or an unreadable file. stdlib only.
"""
import argparse
import sys

SID2_APID = 0x5A4
DATA_OFF = 16           # TMData_DATA_FIELD (TMData.h:56)
MODE_OFF = DATA_OFF + 1
STATUS4_OFF = DATA_OFF + 62
AB_BIT = 14             # STATUS4_ACTIVE_ATT_AB_POS (HKAcquire.c:378)
BC_BIT = 13             # STATUS4_ACTIVE_ATT_BC_POS (HKAcquire.c:379)


class DecodeError(Exception):
    pass


def parse_packet(line, where):
    """One dump line -> dict(status4, ab, bc, mode, where)."""
    try:
        pkt = bytes.fromhex(line.strip())
    except ValueError:
        raise DecodeError(f"{where}: not hex")
    if len(pkt) < STATUS4_OFF + 4:
        raise DecodeError(f"{where}: {len(pkt)} bytes, too short for "
                          f"status4 at byte {STATUS4_OFF}")
    apid = ((pkt[0] << 8) | pkt[1]) & 0x7FF
    if apid != SID2_APID:
        raise DecodeError(f"{where}: APID 0x{apid:03X}, want "
                          f"0x{SID2_APID:03X}")
    if pkt[0] & 0x10:
        raise DecodeError(f"{where}: a TC packet (type bit set)")
    plen = (pkt[4] << 8) | pkt[5]
    if len(pkt) != plen + 7:
        raise DecodeError(f"{where}: {len(pkt)} bytes, the CCSDS length "
                          f"field says {plen + 7}")
    if (pkt[7], pkt[8]) != (3, 25):
        raise DecodeError(f"{where}: TM({pkt[7]},{pkt[8]}), want TM(3,25)")
    if pkt[DATA_OFF] != 2:
        raise DecodeError(f"{where}: SID {pkt[DATA_OFF]}, want 2")
    status4 = int.from_bytes(pkt[STATUS4_OFF:STATUS4_OFF + 4], "big")
    return {"status4": status4,
            "ab": (status4 >> AB_BIT) & 1,
            "bc": (status4 >> BC_BIT) & 1,
            "mode": pkt[MODE_OFF] & 0xF,
            "where": where}


def read_dump(lines):
    """Every SID2 packet of the dump, in order; empty -> DecodeError."""
    pkts = []
    for n, line in enumerate(lines, 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        pkts.append(parse_packet(line, f"line {n}"))
    if not pkts:
        raise DecodeError("no SID2 packet in the dump")
    return pkts


def check_bc_closed(pkts):
    """The silence-run evidence on the LAST packet (see the module
    docstring): BC set, AB clear. Earlier packets (before the move, AB
    set at OUT) are printed by the caller's report, not checked."""
    last = pkts[-1]
    fails = []
    if not last["bc"]:
        fails.append(f"{last['where']}: status4 bit {BC_BIT} (BC) is 0, "
                     f"want 1 (status4 0x{last['status4']:08X})")
    if last["ab"]:
        fails.append(f"{last['where']}: status4 bit {AB_BIT} (AB) is 1, "
                     f"want 0 (status4 0x{last['status4']:08X})")
    return fails


def build_packet(status4, mode=2):
    """A SID2 TM(3,25) CCSDS packet (self-test helper): SID, mode byte,
    sixteen zero 12-bit pairs, status1..3 = 0, the given status4, then
    the zero rest of the TM_3.c layout."""
    body = bytearray(bytes([2, mode]) + bytes(48))   # SID, mode, 16 pairs
    body += (0).to_bytes(12, "big")                  # status1..status3
    body += status4.to_bytes(4, "big")
    body += bytes(35)                                # the rest of SID 2
    dfh = bytes([0x10, 3, 25, 0]) + bytes(6)         # PUS v1, TM(3,25)
    payload = dfh + bytes(body)
    plen = len(payload) - 1
    hdr = bytes([0x08 | (SID2_APID >> 8), SID2_APID & 0xFF,
                 0xC0, 0, plen >> 8, plen & 0xFF])
    return hdr + payload


def self_test():
    # a packet with only BC set (the expected post-move evidence) passes
    pkts = read_dump([build_packet(1 << BC_BIT).hex()])
    assert pkts[0]["ab"] == 0 and pkts[0]["bc"] == 1
    assert pkts[0]["mode"] == 2
    assert check_bc_closed(pkts) == []
    # earlier packets are not checked: a pre-move packet (AB set at OUT)
    # followed by the post-move one passes
    pkts = read_dump([build_packet(1 << AB_BIT).hex(),
                      build_packet(1 << BC_BIT).hex()])
    assert check_bc_closed(pkts) == []
    # both bits set (the plan's original wording): AB must fail
    pkts = read_dump([build_packet((1 << AB_BIT) | (1 << BC_BIT)).hex()])
    fails = check_bc_closed(pkts)
    assert len(fails) == 1 and "AB) is 1" in fails[0], fails
    # only AB set: both rules fail; neither set: only BC fails
    fails = check_bc_closed(read_dump([build_packet(1 << AB_BIT).hex()]))
    assert len(fails) == 2 and "BC) is 0" in fails[0], fails
    fails = check_bc_closed(read_dump([build_packet(0).hex()]))
    assert len(fails) == 1 and "BC) is 0" in fails[0], fails
    # other status4 content does not disturb the two bits
    assert check_bc_closed(read_dump(
        [build_packet((1 << BC_BIT) | 0x00020000).hex()])) == []

    def must_fail(lines, needle):
        try:
            read_dump(lines)
        except DecodeError as e:
            assert needle in str(e), f"wrong error: {e}"
            return
        raise AssertionError("decoded, want a DecodeError")

    good = build_packet(1 << BC_BIT)
    must_fail([good.hex() + "00"], "CCSDS length")
    short = bytearray(good[:80])        # cut below status4, length kept
    short[5] = 80 - 7
    must_fail([short.hex()], "too short for status4")
    wrong_apid = bytearray(good)
    wrong_apid[1] = 0xA7
    must_fail([wrong_apid.hex()], "APID 0x5A7")
    tc = bytearray(good)
    tc[0] |= 0x10
    must_fail([tc.hex()], "a TC packet")
    wrong_pus = bytearray(good)
    wrong_pus[8] = 26                   # the PUS subtype
    must_fail([wrong_pus.hex()], "TM(3,26)")
    wrong_sid = bytearray(good)
    wrong_sid[DATA_OFF] = 4
    must_fail([wrong_sid.hex()], "SID 4")
    must_fail(["zz"], "not hex")
    must_fail(["", "# comment only"], "no SID2 packet")
    print("ATT-SID2-CHECK SELF-TEST: PASS")
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="Check SID2 status4 attenuator bits in a dump")
    ap.add_argument("dump", nargs="?", help="hex dump, one packet per line")
    ap.add_argument("--expect-bc-closed", action="store_true",
                    help="the silence-run check: in the LAST packet, "
                         "status4 bit 13 (BC) set and bit 14 (AB) clear")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.dump:
        ap.error("a dump file is required")
    try:
        with open(args.dump) as f:
            lines = f.readlines()
    except OSError as e:
        print(f"ATT-SID2-CHECK: ERROR (cannot read {args.dump}: {e})")
        return 2
    try:
        pkts = read_dump(lines)
    except DecodeError as e:
        print(f"ATT-SID2-CHECK: FAIL (decode: {e})")
        return 1
    for i, p in enumerate(pkts):
        print(f"SID2 packet {i} ({p['where']}, mode {p['mode']}): "
              f"status4 0x{p['status4']:08X}, AB {p['ab']}, BC {p['bc']}")
    if not args.expect_bc_closed:
        return 0
    fails = check_bc_closed(pkts)
    for x in fails:
        print(f"ATT-SID2-CHECK MISMATCH: {x}")
    if fails:
        print("ATT-SID2-CHECK: FAIL")
        return 1
    last = pkts[-1]
    print(f"ATT-SID2-CHECK: PASS ({last['where']}: AB 0, BC 1 in the "
          f"last of {len(pkts)} packet(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
