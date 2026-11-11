# SPDX-License-Identifier: GPL-2.0-or-later
"""The controller's TM decoder (platform plan PL-4, D-PLa): only what
the bench checks. Operator decoding belongs to the ground station.

A unit is [4-byte SpW header][space packet]. A TM space packet has the
6-byte primary header, then the 10-byte data field header (SP[6]
version/flags, SP[7] type, SP[8] subtype, SP[9] destination, SP[10:16]
the CUC coarse u32 + fine u16), then the application data at SP[16].
"""

import struct

APID_TCVERIF = 0x5A1     # TM(1,x)
APID_HK = 0x5A4          # TM(3,25) SID 1 / SID 2
APID_EVENT = 0x5A7       # TM(5,x), TM(17,2)
APID_HEARTBEAT = 0x5E5

# the platform-relevant event IDs (the GS has the full list)
EIDS = {
    0x0021: "mode change",
    0x0022: "boot report",
    0x142E: "TC(20,128) not received for 30 s",
    0x1429: "time resynchronised by a time code",
    0x542B: "time unsynchronised (no time code for 60 s)",
    0x14A3: "FDIR enabled",
}
# TM(1,8) failure IDs of TC(9,129) (D-PLd)
TIMESYNC_FIDS = {48034: "sync already in progress",
                 48035: "no time code within 2 s",
                 48041: "fine != 0 or coarse = 0"}


def decode(unit):
    """A dict of the fields the controller uses, or None for a unit too
    short to be a TM."""
    sp = unit[4:]
    if len(sp) < 16 or sp[0] & 0x10:
        return None
    w0, w1, ln = struct.unpack(">HHH", sp[:6])
    coarse, fine = struct.unpack(">IH", sp[10:16])
    d = {"apid": w0 & 0x7FF, "ssc": w1 & 0x3FFF, "len": ln + 7,
         "type": sp[7], "subtype": sp[8], "dest": sp[9],
         "cuc": (coarse << 16) | fine, "unsynced": bool(coarse >> 31)}
    app = sp[16:]
    if d["type"] == 1 and len(app) >= 4:
        pid, seq = struct.unpack(">HH", app[:4])
        d["tc"] = {"packet_id": pid, "apid": pid & 0x7FF,
                   "seq": seq & 0x3FFF}
        if d["subtype"] in (2, 4, 6, 8) and len(app) >= 6:
            d["fid"] = struct.unpack(">H", app[4:6])[0]
            if d["fid"] in TIMESYNC_FIDS:
                d["fid_name"] = TIMESYNC_FIDS[d["fid"]]
    elif d["type"] == 5 and len(app) >= 2:
        d["eid"] = struct.unpack(">H", app[:2])[0]
        d["eid_name"] = EIDS.get(d["eid"])
        d["params"] = app[2:].hex()
    elif d["type"] == 3 and d["subtype"] == 25 and len(app) >= 1:
        d["sid"] = app[0]
        if d["apid"] == APID_HK and d["sid"] in (1, 2) and len(app) >= 2:
            d["mode"] = app[1] & 0x0F
    if d["apid"] == APID_HEARTBEAT:
        d["heartbeat"] = True
    return d


def describe(d):
    """A short text for logs."""
    if d is None:
        return "?"
    s = "0x%03X TM(%d,%d)" % (d["apid"], d["type"], d["subtype"])
    if "eid" in d:
        s += " EID 0x%04X" % d["eid"]
        if d.get("eid_name"):
            s += " (%s)" % d["eid_name"]
    if "tc" in d:
        s += " for 0x%04X/%d" % (d["tc"]["packet_id"], d["tc"]["seq"])
    if "fid" in d:
        s += " FID %d" % d["fid"]
    if "mode" in d:
        s += " SID %d mode %d" % (d["sid"], d["mode"])
    return s
