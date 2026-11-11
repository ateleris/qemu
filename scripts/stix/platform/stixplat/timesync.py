# SPDX-License-Identifier: GPL-2.0-or-later
"""TC(9,129) timed to the next time code (platform plan D-PLd).

The FSW applies the time at the next time code, so the TC's
coarse & 63 must equal that code's TIMECNT. The controller reads the
OBT (coarse and fine atomically); with >= 0.5 s to the rollover it
sends coarse + 1 now, otherwise it waits on bench time for the
rollover + 50 ms, reads the OBT again and applies the same rule (an OBT
jump while waiting means another wait). That leaves >= 0.5 s for UDP,
RX DMA and the TC task, inside the FSW's 2 s wait.

One sync in flight (review R2): a sync being prepared or sent, from
this verb or a raw TC(9,129) through tc.platform.send, makes another
one get 409 until its TM(1,7)/TM(1,8) to destination 14 arrives or 3 s
of bench time pass. The verb runs outside the dispatcher lock (it may
wait ~0.5 s of bench time), so the reservation is taken atomically.

The verification is an `expect` record: TM(1,7) within 3 s, then the
first heartbeat's 48-bit CUC against the OBT at its arrival (within
0.25 s + a relay allowance, bit 31 clear), and the ordering: the RX
delivery of this TC's unit (matched on its length, so an interleaved
TC20 or GS TC cannot stand in) before the emit whose TIMECNT is
coarse & 63. A CUC exactly 1 s off, or the wrong order, is
`late_apply`; a TM(1,8) (48035, 48034, 48041) is a failure.
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
RX_LEN_RE = re.compile(r"\blen=(\d+)")
MAX_WAITS = 3


def plan_send(obt):
    """D-PLd: (send_now, coarse_target, wait_ns) from an OBT reading."""
    coarse, fine = obt >> 16, obt & 0xFFFF
    left_ns = (CUC_S - fine) * NS // CUC_S
    if left_ns >= NS // 2:
        return True, (coarse + 1) & 0xFFFFFFFF, 0
    return False, None, left_ns + 50_000_000


class TimeSync:
    def __init__(self, clock, log, read_obt, send_unit, can_send,
                 next_seq, wall=time.monotonic, autostart="off"):
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
        self.autostart = autostart
        self.auto_armed = False

    # -- the automatic sync (B2), once per boot --------------------------
    def on_boot(self):
        """A boot or a guest reset: arm the automatic sync; "boot"
        sends it now, "first_tm" at the first TM."""
        with self.lock:
            self.auto_armed = self.autostart != "off"
            now = self.autostart == "boot"
            if now:
                self.auto_armed = False
        if now:
            self._auto("boot")

    def on_off(self):
        with self.lock:
            self.auto_armed = False

    def _auto(self, trigger):
        def run():
            data = {"trigger": trigger}
            try:
                data.update(self.send(), ok=True)
            except VerbError as e:
                data.update(ok=False, status=e.status, error=str(e))
            except Exception as e:      # QMP gone (a power-off race)
                data.update(ok=False, error="%s: %s" % (type(e).__name__,
                                                        e))
            self.log.emit("ctl", "log", "platform.timesync.auto", data)
        threading.Thread(target=run, name="timesync-auto",
                         daemon=True).start()

    def _busy(self):
        """Under self.lock: None, or why another TC(9,129) must wait."""
        p = self.pending
        if p is None:
            return None
        if p.get("preparing"):
            return "a TC(9,129) is being prepared (pending)"
        if self.clock.exact_sim() - p["sent_sim"] >= 3 * NS:
            self._verdict(p, "timeout", "no TM(1,7)/TM(1,8) in 3 s")
            return None
        return ("a TC(9,129) (seq %d, coarse 0x%08X) is pending"
                % (p["seq"], p["coarse"]))

    def busy(self):
        with self.lock:
            return self._busy()

    def _reserve(self):
        with self.lock:
            why = self._busy()
            if why is not None:
                raise VerbError(409, why)
            self.pending = {"preparing": True}

    def _unreserve(self):
        with self.lock:
            if self.pending is not None and self.pending.get("preparing"):
                self.pending = None

    def send(self, coarse="auto", wait_timeout_s=10.0):
        why = self.can_send()
        if why is not None:
            raise VerbError(409, "time sync: %s" % why)
        if coarse != "auto" and (isinstance(coarse, bool) or
                                 not isinstance(coarse, int) or
                                 not 0 <= coarse < 1 << 32):
            raise VerbError(400, "coarse must be \"auto\" or a u32")
        self._reserve()
        try:
            obt, target, waited = self._target(wait_timeout_s)
            warn = None
            if coarse != "auto":
                if coarse & 63 != target & 63:
                    warn = ("coarse & 63 = %d but the next code's TIMECNT "
                            "is %d" % (coarse & 63, target & 63))
                target = coarse
            seq = self.next_seq()
            unit = tcbuild.unit(tcbuild.tc9_129(seq, target))
            self._sent(unit, seq, target, raw=False, extra={
                "obt_read": obt, "waited": waited, "warning": warn})
        except BaseException:
            self._unreserve()
            raise
        return {"seq": seq, "coarse": target, "waited": waited,
                "warning": warn}

    def send_raw(self, sp):
        """SF3: a raw TC(9,129) from tc.platform.send is a sync in
        flight exactly like send(), verified the same way."""
        p = tcbuild.parse_tc(sp)
        coarse = int.from_bytes(sp[10:14], "big") if len(sp) >= 14 else 0
        self._reserve()
        try:
            self._sent(tcbuild.unit(sp), p[1] if p else None, coarse,
                       raw=True, extra={"crc_ok": p[6] if p else False})
        except BaseException:
            self._unreserve()
            raise

    def _target(self, timeout_s):
        """(OBT read, target coarse, waited): wait while the 0.5 s rule
        says so, re-checking after each wait."""
        waited = False
        end = self.wall() + timeout_s
        for _ in range(MAX_WAITS):
            obt, _ = self.read_obt()
            now, target, wait_ns = plan_send(obt)
            if now:
                return obt, target, waited
            waited = True
            done = threading.Event()
            self.clock.after(wait_ns, done.set)
            while not done.is_set():
                if self.wall() >= end:
                    raise VerbError(409, "time sync: no rollover within "
                                    "%.0f s of wall time (paused?)"
                                    % timeout_s)
                done.wait(0.02)
        raise VerbError(409, "time sync: the OBT kept landing in the last "
                        "0.5 s of a second (%d waits)" % MAX_WAITS)

    def _sent(self, unit, seq, coarse, raw, extra):
        with self.lock:
            self.send_unit(unit)
            sent = self.clock.exact_sim()
            self.pending = {"seq": seq, "coarse": coarse, "sent_sim": sent,
                            "log_seq": self.log.seq, "rx_len": len(unit),
                            "raw": raw}
        data = {"service": 9, "subtype": 129, "seq": seq, "coarse": coarse,
                "raw": raw, "source": tcbuild.SOURCE_PLATFORM}
        data.update(extra)
        rec = self.log.emit("ctl", "tc", "platform.timesync", data)
        with self.lock:
            if self.pending is not None and self.pending.get("seq") == seq:
                self.pending["tc_rec"] = rec.seq
        self.clock.after(3 * NS, self.busy)

    # -- verification ---------------------------------------------------------
    def on_tm(self, unit, dec):
        if dec is None:
            return
        if self.auto_armed and self.autostart == "first_tm":
            with self.lock:
                fire, self.auto_armed = self.auto_armed, False
            if fire:
                self._auto("first_tm")
        with self.lock:
            p = self.pending
            if p is not None and not p.get("preparing") and \
                    dec["type"] == 1 and dec["subtype"] in (7, 8) and \
                    dec["dest"] == tcbuild.SOURCE_PLATFORM and \
                    dec.get("tc", {}).get("packet_id") == TC_PACKET_ID and \
                    dec["tc"]["seq"] == p["seq"]:
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
        tol = TOL_CUC + RELAY_ALLOW_CUC
        if dec["unsynced"]:
            verdict, why = "fail", "heartbeat bit 31 (unsynced) is set"
        elif abs(diff) <= tol:
            verdict, why = ("ok", None) if ordering != "late" else \
                ("late_apply", "the applying code came before the TC")
        elif abs(abs(diff) - CUC_S) <= tol:
            verdict, why = "late_apply", "the CUC is 1 s off the OBT"
        else:
            verdict, why = "fail", "the CUC is %.3f s off the OBT" % (
                diff / CUC_S)
        self._verdict(p, verdict, why, cuc=cuc, obt=obt_now,
                      diff_s=diff / CUC_S, ordering=ordering)

    def _ordering(self, p):
        """The RX delivery of this TC's unit (its length) before the
        emit whose TIMECNT is coarse & 63 (both traces, in stderr
        order)."""
        rx = emit = None
        for r in self.log.since(p.get("log_seq", 0)):
            path = r.obj["path"]
            line = r.obj["data"].get("line", "")
            if path == "trace.stixspw_rx_deliver" and rx is None:
                m = RX_LEN_RE.search(line)
                if m and int(m.group(1)) == p["rx_len"]:
                    rx = r.seq
            elif path == "trace.stixspw_timecode_emit" and emit is None:
                m = EMIT_RE.search(line)
                if m and int(m.group(1)) == p["coarse"] & 63:
                    emit = r.seq
        if rx is None or emit is None:
            return "unknown"
        return "ok" if rx < emit else "late"

    def _verdict(self, p, verdict, why, **extra):
        if p is self.pending:
            self.pending = None
        data = {"what": "timesync", "seq": p["seq"], "coarse": p["coarse"],
                "raw": p.get("raw", False), "verdict": verdict,
                "ok": verdict == "ok", "why": why}
        data.update(extra)
        self.history.append(data)
        self.log.emit("ctl", "expect", "platform.timesync", data)

    def status(self):
        with self.lock:
            return {"pending": dict(self.pending) if self.pending else None,
                    "last": self.history[-1] if self.history else None}
