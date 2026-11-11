# SPDX-License-Identifier: GPL-2.0-or-later
"""The TC(20,128) keep-alive (platform plan D-PLe).

Paced on bench time (SimClock deadlines fire only on real vclock
samples), so a pause or a slow host can never fake a lapse. The
sequence count is the OBC's 8 Hz counter, floor(t_sim * 8) mod 2^14,
which never jumps back across a power cycle. Suppressed (and counted)
while the link is down or STIX is not running.
"""

import threading

from . import tcbuild


class KeepAlive:
    def __init__(self, cfg, clock, log, send_unit, can_send):
        """@send_unit(unit) puts a TC on the wire; @can_send() -> None or
        the reason a platform TC cannot go out now."""
        k = cfg["keepalive"]
        self.clock = clock
        self.log = log
        self.send_unit = send_unit
        self.can_send = can_send
        self.enabled = k["enabled"]
        self.period_ns = int(k["period_s"] * 1e9)
        self.autostart = k["autostart"]
        self.started = False
        self.gen = 0
        self.sent = 0
        self.suppressed = 0
        self.last_seq = None
        self.last_why = None
        self.lock = threading.Lock()

    # -- lifecycle (power hooks) ---------------------------------------------
    def on_boot(self):
        """A boot or a guest reset: restart per autostart."""
        with self.lock:
            self.gen += 1
            self.started = self.autostart == "boot"
            self._arm()

    def on_off(self):
        with self.lock:
            self.gen += 1
            self.started = False

    def on_tm(self, unit, dec):
        if self.autostart != "first_tm" or self.started:
            return
        with self.lock:
            if not self.started:
                self.started = True
                self.log.emit("ctl", "log", "platform.keepalive",
                              {"started": "first_tm"})

    def set(self, enabled=None, period_s=None):
        with self.lock:
            if enabled is not None:
                self.enabled = enabled
            if period_s is not None:
                self.period_ns = int(period_s * 1e9)
            self.gen += 1
            self._arm()
            return self.status()

    def status(self):
        return {"enabled": self.enabled, "period_s": self.period_ns / 1e9,
                "autostart": self.autostart, "started": self.started,
                "sent": self.sent, "suppressed": self.suppressed,
                "last_seq": self.last_seq}

    # -- the tick ------------------------------------------------------------
    def _arm(self):
        gen = self.gen
        self.clock.after(self.period_ns, lambda: self._tick(gen))

    def _tick(self, gen):
        with self.lock:
            if gen != self.gen:
                return              # superseded (a set, a boot, an off)
            self._arm()
            if not (self.enabled and self.started):
                return
            why = self.can_send()
            if why is not None:
                self.suppressed += 1
                if why != self.last_why:
                    self.last_why = why
                    self.log.emit("ctl", "tc", "platform.keepalive",
                                  {"suppressed": True, "why": why})
                return
            self.last_why = None
            seq = tcbuild.keepalive_seq(self.clock.exact_sim())
            pkt = tcbuild.tc20_128(seq)
            self.send_unit(tcbuild.unit(pkt))
            self.sent += 1
            self.last_seq = seq
        self.log.emit("ctl", "tc", "platform.keepalive",
                      {"service": 20, "subtype": 128, "seq": seq,
                       "source": tcbuild.SOURCE_PLATFORM, "len": len(pkt)})
