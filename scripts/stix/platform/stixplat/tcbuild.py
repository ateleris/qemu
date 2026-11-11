# SPDX-License-Identifier: GPL-2.0-or-later
"""TC building (platform plan D-PLi, D-PLd, D-PLe): the CCSDS/PUS TC
space packet, the CRC-16/CCITT-FALSE trailer and the 4-byte SpW header
that the model's UDP TC port expects (udp-spw-header=on).

CRC-16/CCITT-FALSE: init 0xFFFF, poly 0x1021, no reflection, no final
xor, over the packet without its CRC.
"""

import struct

SPW_HDR = bytes((0x40, 0x02, 0x00, 0x00))   # the unit header (D-K)
APID_TC = 0x5AC          # PID 90, category 12
APID_TC20 = 0x31C        # PID 49, category 12 (TCData.c:24)
SOURCE_PLATFORM = 14     # System Control (TC20 counter, D-PLa)
SOURCE_GS_EMU = 121      # SpareGS0 (tc.gs.send)
PLATFORM_SERVICES = (9, 20)


def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


def tc_packet(apid, seq, service, subtype, data=b"", ack=0x9, source=0):
    """A TC space packet with its CRC. @seq is the 14-bit sequence count
    (standalone packet, flags 0b11)."""
    if not 0 <= seq < 1 << 14:
        raise ValueError("TC sequence count %r is not 14-bit" % seq)
    dfh = bytes(((1 << 4) | (ack & 0xF), service, subtype, source))
    body = dfh + bytes(data)
    length = len(body) + 2 - 1          # + CRC, minus one (CCSDS)
    hdr = struct.pack(">HHH", 0x1000 | 0x0800 | (apid & 0x7FF),
                      0xC000 | seq, length)
    pkt = hdr + body
    return pkt + struct.pack(">H", crc16(pkt))


def unit(packet):
    """The SpW unit the model's TC port takes."""
    return SPW_HDR + packet


def tc9_129(seq, coarse, fine=0, source=SOURCE_PLATFORM, ack=0x9):
    """Time synchronisation: coarse u32 + fine u16 (TCData.h:89-90)."""
    return tc_packet(APID_TC, seq, 9, 129, struct.pack(">IH", coarse, fine),
                     ack=ack, source=source)


def tc20_128(seq, source=SOURCE_PLATFORM):
    """The keep-alive: 236 zero bytes, ACK 0, length field 241, 248 bytes
    (TCData.c:103-107; the FSW ignores the content)."""
    return tc_packet(APID_TC20, seq, 20, 128, bytes(236), ack=0,
                     source=source)


def keepalive_seq(sim_ns):
    """The OBC's 8 Hz counter on bench time: floor(t * 8) mod 2^14."""
    return (sim_ns * 8 // 1_000_000_000) % (1 << 14)


def parse_tc(pkt):
    """(apid, seq, service, subtype, source, ack, crc_ok) of a TC space
    packet (no SpW header), or None if it is not a well-formed TC."""
    if len(pkt) < 12:
        return None
    w0, w1, ln = struct.unpack(">HHH", pkt[:6])
    if not w0 & 0x1000 or len(pkt) != ln + 7:
        return None
    ok = crc16(pkt[:-2]) == struct.unpack(">H", pkt[-2:])[0]
    return (w0 & 0x7FF, w1 & 0x3FFF, pkt[7], pkt[8], pkt[9], pkt[6] & 0xF,
            ok)
