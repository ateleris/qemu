# SPDX-License-Identifier: GPL-2.0-or-later
"""TC(9,129) timed to the next time code (platform plan D-PLd).

The FSW applies the time at the next time code, so the TC's
coarse & 63 must equal that code's TIMECNT. The controller reads the
OBT (coarse and fine atomically); with >= 0.5 s to the rollover it
sends coarse + 1 now, otherwise it waits on bench time for the
rollover + 50 ms and sends the new coarse + 1. That leaves >= 0.5 s for
UDP, RX DMA and the TC task, inside the FSW's 2 s wait.

One sync in flight (review R2): another TC(9,129), from this verb or
tc.platform.send, gets 409 until the first one's TM(1,7)/TM(1,8)
arrives or 3 s of bench time pass.

The verification is an `expect` record: TM(1,7) within 3 s, then the
first heartbeat's 48-bit CUC against the OBT at its arrival (within
0.25 s + a relay allowance, bit 31 clear). A TM(1,8) (48035, 48034,
48041) is a failure.
"""

import re
import threading
import time

from . import tcbuild
from .dispatch import VerbError

NS = 1_000_000_000
CUC_S = 65536
TC_PACKET_ID = 0x1800 | tcbuild.APID_TC     # TC, sec hdr, APID 0x5AC
TOL_CUC = int(0.25 * CUC_S)
RELAY_ALLOW_CUC = int(0.05 * CUC_S)
EMIT_RE = re.compile(r"timecnt=(\d+)")


def plan_send(obt):
    """D-PLd: (send_now, coarse_target, wait_ns) from an OBT reading."""
    coarse, fine = obt >> 16, obt & 0xFFFF
    left_ns = (CUC_S - fine) * NS // CUC_S
    if left_ns >= NS // 2:
        return True, (coarse + 1) & 0xFFFFFFFF, 0
    return False, None, left_ns + 50_000_000


class TimeSync:
    def __init__(self, clock, log, read_obt, send_unit, can_send,
                 next_seq, wall=time.monotonic):
        self.clock = clock
        self.log = log
        self.read_obt = read_obt     # -> (obt, bench time of the read)
        self.send_unit = send_unit
        self.can_send = can_send
        self.next_seq = next_seq     # the source-14 APID 0x5AC counter
        self.wall = wall
        self.lock = threading.Lock()
        self.pending = None
        self.awaiting_hb = None
        self.history = []

    def busy(self):
        """None, or why another TC(9,129) must wait (409)."""
        with self.lock:
            p = self.pending
            if p is None:
                return None
            if self.clock.exact_sim() - p["sent_sim"] >= 3 * NS:
                self._verdict(p, "timeout", "no TM(1,7)/TM(1,8) in 3 s")
                return None
            return ("a TC(9,129) (seq %d, coarse 0x%08X) is pending"
                    % (p["seq"], p["coarse"]))

    def send(self, coarse="auto", wait_timeout_s=10.0):
        why = self.can_send()
        if why is not None:
            raise VerbError(409, "time sync: %s" % why)
        why = self.busy()
        if why is not None:
            raise VerbError(409, why)
        obt, _ = self.read_obt()
        now, target, wait_ns = plan_send(obt)
        if not now:
            target = self._wait_rollover(wait_ns, wait_timeout_s)
        warn = None
        if coarse != "auto":
            if not isinstance(coarse, int) or not 0 <= coarse < 1 << 32:
                raise VerbError(400, "coarse must be \"auto\" or a u32")
            if coarse & 63 != target & 63:
                warn = ("coarse & 63 = %d but the next code's TIMECNT is %d"
                        % (coarse & 63, target & 63))
            target = coarse
        seq = self.next_seq()
        pkt = tcbuild.tc9_129(seq, target)
        with self.lock:
            self.send_unit(tcbuild.unit(pkt))
            sent = self.clock.exact_sim()
            self.pending = {"seq": seq, "coarse": target, "sent_sim": sent,
                            "log_seq": self.log.seq}
        rec = self.log.emit("ctl", "tc", "platform.timesync",
                            {"service": 9, "subtype": 129, "seq": seq,
                             "coarse": target, "obt_read": obt,
                             "waited": not now, "warning": warn,
                             "source": tcbuild.SOURCE_PLATFORM})
        with self.lock:
            if self.pending is not None and self.pending["seq"] == seq:
                self.pending["tc_rec"] = rec.seq
        self.clock.after(3 * NS, self.busy)
        return {"seq": seq, "coarse": target, "waited": not now,
                "warning": warn}

    def _wait_rollover(self, wait_ns, timeout_s):
        done = threading.Event()
        self.clock.after(wait_ns, done.set)
        end = self.wall() + timeout_s
        while not done.is_set():
            if self.wall() >= end:
                raise VerbError(409, "time sync: no rollover within %.0f s "
                                "of wall time (paused?)" % timeout_s)
            done.wait(0.02)
        obt, _ = self.read_obt()
        return ((obt >> 16) + 1) & 0xFFFFFFFF

    # -- verification ---------------------------------------------------------
    def on_tm(self, unit, dec):
        if dec is None:
            return
        with self.lock:
            p = self.pending
            if p is not None and dec["type"] == 1 and \
                    dec.get("tc", {}).get("packet_id") == TC_PACKET_ID and \
                    dec["tc"]["seq"] == p["seq"] and \
                    dec["subtype"] in (7, 8):
                if dec["subtype"] == 8:
                    self._verdict(p, "fail", "TM(1,8) FID %s (%s)"
                                  % (dec.get("fid"), dec.get("fid_name")))
                else:
                    p["tm17_sim"] = self.clock.now()[0]
                    self.awaiting_hb = p
                    self.pending = None
                return
            hb = self.awaiting_hb
            if hb is not None and dec.get("heartbeat"):
                self.awaiting_hb = None
                self._check_heartbeat(hb, dec)

    def _check_heartbeat(self, p, dec):
        obt, at_sim = self.read_obt(cached=True)
        now = self.clock.now()[0]
        obt_now = obt + (now - at_sim) * CUC_S // NS
        cuc = dec["cuc"]
        diff = cuc - obt_now
        ordering = self._ordering(p)
        if dec["unsynced"]:
            verdict, why = "fail", "heartbeat bit 31 (unsynced) is set"
        elif abs(diff) <= TOL_CUC + RELAY_ALLOW_CUC:
            verdict, why = ("ok", None) if ordering != "late" else \
                ("late_apply", "the applying code came before the TC")
        elif abs(abs(diff) - CUC_S) <= TOL_CUC + RELAY_ALLOW_CUC:
            verdict, why = "late_apply", "the CUC is 1 s off the OBT"
        else:
            verdict, why = "fail", "the CUC is %.3f s off the OBT" % (
                diff / CUC_S)
        self._verdict(p, verdict, why, cuc=cuc, obt=obt_now,
                      diff_s=diff / CUC_S, ordering=ordering)

    def _ordering(self, p):
        """The RX delivery of the TC unit before the emit whose TIMECNT
        is coarse & 63 (both traces, in stderr order)."""
        rx = emit = None
        for r in self.log.since(p.get("log_seq", 0)):
            path = r.obj["path"]
            if path == "trace.stixspw_rx_deliver" and rx is None:
                rx = r.seq
            elif path == "trace.stixspw_timecode_emit":
                m = EMIT_RE.search(r.obj["data"].get("line", ""))
                if m and int(m.group(1)) == p["coarse"] & 63 and \
                        emit is None:
                    emit = r.seq
        if rx is None or emit is None:
            return "unknown"
        return "ok" if rx < emit else "late"

    def _verdict(self, p, verdict, why, **extra):
        if p is self.pending:
            self.pending = None
        data = {"what": "timesync", "seq": p["seq"], "coarse": p["coarse"],
                "verdict": verdict, "ok": verdict == "ok", "why": why}
        data.update(extra)
        self.history.append(data)
        self.log.emit("ctl", "expect", "platform.timesync", data)

    def status(self):
        with self.lock:
            return {"pending": dict(self.pending) if self.pending else None,
                    "last": self.history[-1] if self.history else None}

