# SPDX-License-Identifier: GPL-2.0-or-later
"""The TC(20,128) keep-alive (platform plan D-PLe).

Paced on bench time (SimClock deadlines fire only on real vclock
samples), so a pause or a slow host can never fake a lapse. Each
deadline is the previous one plus the period, so the poller's sampling
jitter does not stretch the period; slots missed in a stall are counted,
not sent as a burst. The sequence count is the OBC's 8 Hz counter,
floor(t_sim * 8) mod 2^14, which never jumps back across a power cycle.
Suppressed (and counted) while the link is down or STIX is not running.

At the default 125 ms a record per TC would be 8 records a second, so
the log gets one summary per `summary_every` sends plus the first and
the last of each run (a run ends at a disable, a suppression, a boot or
an off) and the first failure of a failing stretch. The RX-delivery
trace of each TC20 is counted, not logged (trace_filter).
"""

import re
import threading

from . import tcbuild

TC20_UNIT_LEN = 4 + 248         # the SpW header + TC(20,128)
RX_LEN_RE = re.compile(r"\blen=(\d+)")


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
        self.summary_every = k["summary_every"]
        self.started = False
        self.gen = 0
        self.due = None
        self.sent = 0               # since the controller started
        self.suppressed = 0
        self.missed = 0
        self.rx_deliver = 0         # TC20 RX traces counted, not logged
        self.outstanding = 0        # TC20 sent, RX trace not yet seen
        self.last_seq = None
        self.last_why = None
        self.failing = None
        self.run = None             # the current run's counters
        self.lock = threading.Lock()

    # -- lifecycle (power hooks) ---------------------------------------------
    def on_boot(self):
        """A boot or a guest reset: restart per autostart."""
        with self.lock:
            self._end_run("boot")
            self.started = self.autostart == "boot"
            self.outstanding = 0
            self._restart()

    def on_off(self):
        with self.lock:
            self._end_run("off")
            self.gen += 1
            self.started = False
            self.outstanding = 0

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
            if not self.enabled:
                self._end_run("disabled")
            self._restart()
            return self._status()

    def status(self):
        with self.lock:
            return self._status()

    def _status(self):
        return {"enabled": self.enabled, "period_s": self.period_ns / 1e9,
                "autostart": self.autostart, "started": self.started,
                "sent": self.sent, "suppressed": self.suppressed,
                "missed": self.missed, "rx_deliver": self.rx_deliver,
                "last_seq": self.last_seq, "failing": self.failing}

    def trace_filter(self, name, text):
        """True drops the trace line from the log: the RX delivery of a
        TC20 this keep-alive sent (a unit of that length while one is
        outstanding)."""
        if name != "stixspw_rx_deliver":
            return False
        m = RX_LEN_RE.search(text)
        if not m or int(m.group(1)) != TC20_UNIT_LEN:
            return False
        with self.lock:
            if self.outstanding <= 0:
                return False
            self.outstanding -= 1
            self.rx_deliver += 1
            if self.run is not None:
                self.run["rx_deliver"] += 1
        return True

    # -- the tick ------------------------------------------------------------
    def _restart(self):
        self.gen += 1
        self.due = self.clock.exact_sim() + self.period_ns
        gen = self.gen
        self.clock.at(self.due, lambda: self._tick(gen))

    def _rearm(self):
        """The next deadline from the previous one (no drift); slots
        already past are skipped and counted."""
        now = self.clock.exact_sim()
        self.due += self.period_ns
        if self.due <= now:
            k = (now - self.due) // self.period_ns + 1
            self.missed += k
            self.due += k * self.period_ns
        gen = self.gen
        self.clock.at(self.due, lambda: self._tick(gen))

    def _tick(self, gen):
        with self.lock:
            if gen != self.gen:
                return              # superseded (a set, a boot, an off)
            self._rearm()
            if not (self.enabled and self.started):
                return
            why = self.can_send()
            if why is not None:
                self.suppressed += 1
                self._end_run("suppressed")
                if why != self.last_why:
                    self.last_why = why
                    self.log.emit("ctl", "tc", "platform.keepalive",
                                  {"suppressed": True, "why": why})
                return
            self.last_why = None
            seq = tcbuild.keepalive_seq(self.clock.exact_sim())
            pkt = tcbuild.tc20_128(seq)
            try:
                self.send_unit(tcbuild.unit(pkt))
            except OSError as e:
                if self.failing is None:
                    self.failing = str(e)
                    self._summary("failure", error=self.failing, seq=seq)
                return
            self.failing = None
            self.sent += 1
            self.outstanding += 1
            self.last_seq = seq
            if self.run is None:
                self.run = {"sent": 0, "first_seq": seq, "rx_deliver": 0}
            self.run["sent"] += 1
            n = self.run["sent"]
            if n == 1:
                self._summary("first")
            elif n % self.summary_every == 0:
                self._summary("every")

    def _summary(self, reason, **extra):
        r = self.run or {}
        data = {"summary": True, "reason": reason,
                "sent": r.get("sent", 0), "first_seq": r.get("first_seq"),
                "last_seq": self.last_seq,
                "rx_deliver": r.get("rx_deliver", 0),
                "suppressed": self.suppressed, "missed": self.missed,
                "period_s": self.period_ns / 1e9,
                "service": 20, "subtype": 128,
                "source": tcbuild.SOURCE_PLATFORM}
        data.update(extra)
        self.log.emit("ctl", "tc", "platform.keepalive", data)

    def _end_run(self, why):
        if self.run is not None and self.run["sent"] > 0:
            self._summary("last", ended=why)
        self.run = None
