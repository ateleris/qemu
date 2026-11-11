# SPDX-License-Identifier: GPL-2.0-or-later
"""The bench time (platform plan D-PLr).

sim_time = sum(final vclock of each earlier QEMU process)
         + sum(the off-times, on the wall clock)
         + the current process's vclock.

QEMU's virtual clock is the master: the controller samples
`vclock-ns` and estimates between samples from the host monotonic
clock, only while the VM runs (STOP/RESUME freeze and unfreeze it).
Reported times never go backwards. Deadlines are bench-time deadlines
and fire only once a real sample has passed them, never on an estimate.
"""

import heapq
import itertools
import threading
import time

OFF, FROZEN, RUNNING, PAUSED = "off", "frozen", "running", "paused"


class SimClock:
    def __init__(self, mono_fn=time.monotonic):
        self.mono = mono_fn
        self.lock = threading.RLock()
        self.base_ns = 0           # bench time at this process's vclock 0
        self.vclock = None         # last sampled vclock (None while off)
        self.sample_mono = None
        self.mode = OFF
        self.off_since = self.mono()
        self.boot_id = 0
        self.last_reported = 0
        self.deadlines = []        # heap of (sim_ns, n, callback)
        self.counter = itertools.count()

    # -- the process lifecycle -------------------------------------------
    def process_started(self):
        """A new QEMU process (frozen at vclock 0): the off-time ends."""
        with self.lock:
            if self.mode == OFF:
                self.base_ns += int((self.mono() - self.off_since) * 1e9)
            self.vclock = 0
            self.sample_mono = self.mono()
            self.mode = FROZEN
            self.boot_id += 1

    def process_ended(self, final_vclock=None):
        """The process is gone: fold its vclock in, start the off-time."""
        with self.lock:
            if self.mode == OFF:
                return
            v = self._vclock_now() if final_vclock is None else \
                max(final_vclock, self.vclock or 0)
            self.base_ns += v
            self.vclock = None
            self.mode = OFF
            self.off_since = self.mono()

    def guest_reset(self):
        """A warm guest reset: same process, vclock continues."""
        with self.lock:
            self.boot_id += 1

    def resumed(self):
        with self.lock:
            if self.mode != OFF:
                self.sample_mono = self.mono()
                self.mode = RUNNING

    def stopped(self):
        with self.lock:
            if self.mode == RUNNING:
                self.vclock = self._vclock_now()
                self.sample_mono = self.mono()
                self.mode = PAUSED

    # -- samples ----------------------------------------------------------
    def sample(self, vclock_ns):
        """A real `vclock-ns` reading of the current process."""
        with self.lock:
            if self.mode == OFF:
                return
            if self.vclock is None or vclock_ns >= self.vclock:
                self.vclock = vclock_ns
            self.sample_mono = self.mono()

    def _vclock_now(self):
        if self.vclock is None:
            return 0
        if self.mode == RUNNING:
            return self.vclock + int((self.mono() - self.sample_mono) * 1e9)
        return self.vclock

    def now(self):
        """(sim_time, boot_id, vclock_ns or None, estimated)."""
        with self.lock:
            if self.mode == OFF:
                sim = self.base_ns + int((self.mono() - self.off_since)
                                         * 1e9)
                vclock, est = None, False
            else:
                vclock = self._vclock_now()
                sim = self.base_ns + vclock
                est = self.mode == RUNNING
            if sim < self.last_reported:
                sim = self.last_reported
            self.last_reported = sim
            return sim, self.boot_id, vclock, est

    def exact_sim(self):
        """The bench time of the last real sample (no estimate)."""
        with self.lock:
            if self.mode == OFF:
                return self.now()[0]
            return self.base_ns + (self.vclock or 0)

    def running(self):
        return self.mode == RUNNING

    # -- deadlines --------------------------------------------------------
    def at(self, sim_ns, callback):
        with self.lock:
            heapq.heappush(self.deadlines, (sim_ns, next(self.counter),
                                            callback))

    def after(self, delta_ns, callback):
        self.at(self.exact_sim() + delta_ns, callback)

    def next_due(self):
        with self.lock:
            return self.deadlines[0][0] if self.deadlines else None

    def run_due(self):
        """Fire the deadlines the last real sample has passed. Never
        while paused or off (no virtual time passes then)."""
        fired = []
        while True:
            with self.lock:
                if self.mode != RUNNING or not self.deadlines:
                    break
                exact = self.base_ns + (self.vclock or 0)
                if self.deadlines[0][0] > exact:
                    break
                _, _, cb = heapq.heappop(self.deadlines)
            fired.append(cb)
            cb()
        return fired

    def wake_in(self, cap_s=0.05):
        """How long the scheduler may sleep: the earlier of the estimated
        due time and @cap_s."""
        with self.lock:
            due = self.next_due()
            if due is None or self.mode != RUNNING:
                return cap_s
            left = (due - self.base_ns - self._vclock_now()) / 1e9
            return max(0.0, min(cap_s, left))
