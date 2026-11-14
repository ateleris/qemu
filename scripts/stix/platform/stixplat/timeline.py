# SPDX-License-Identifier: GPL-2.0-or-later
"""The truth sampler (timeline plan A1): one `sample` record per
bench-time tick with the truth of every leaf matching a
[timeline] sample_paths glob, plus "<path>@presented" only where the
presented value differs.

Paced on bench time like the keep-alive: the next deadline is one
period after the previous one (no drift), and slots a slow host has
already missed are skipped (no burst catch-up). Deadlines fire only on
real vclock samples (SimClock.run_due runs while RUNNING only), so a
pause, a power-off or a slow host can neither fake nor burst ticks.
The sampler reads the state tree the poller already fills; it issues
no QMP of its own. No record when power is off or the tree has no
matched leaf.
"""

import fnmatch


class Sampler:
    def __init__(self, clock, state, log, paths, period_s):
        self.clock = clock
        self.state = state
        self.log = log
        self.paths = list(paths)
        self.period_ns = int(period_s * 1e9)
        self.due = 0                # the next bench-time deadline
        self.gen = 0                # superseded ticks see a stale gen
        self.count = 0              # sample records emitted

    # -- lifecycle (the power hooks in core.py) -------------------------------
    def start(self):
        """Arm the first tick; a boot or a guest reset re-arms from now."""
        self.on_boot()

    def stop(self):
        self.gen += 1

    def on_boot(self):
        self.gen += 1
        self._arm()

    def on_off(self):
        self.gen += 1

    # -- the tick ---------------------------------------------------------------
    def _arm(self):
        """The first deadline one period from now (a boot re-arms)."""
        self.due = self.clock.exact_sim() + self.period_ns
        gen = self.gen
        self.clock.at(self.due, lambda: self._tick(gen))

    def _tick(self, gen):
        if gen != self.gen:
            return                  # superseded (a stop, an off, a boot)
        self._rearm()
        data = self._collect()
        if data:
            self.count += 1
            self.log.emit("ctl", "sample", "", data)

    def _rearm(self):
        """The next deadline from the previous one (no drift); slots
        already past are skipped, never caught up with a burst."""
        now = self.clock.exact_sim()
        self.due += self.period_ns
        if self.due <= now:
            k = (now - self.due) // self.period_ns + 1
            self.due += k * self.period_ns
        gen = self.gen
        self.clock.at(self.due, lambda: self._tick(gen))

    def _collect(self):
        with self.state.lock:
            leaves = {p: (lf["truth"], lf["presented"])
                      for p, lf in self.state.leaves.items()}
        data = {}
        for p in sorted(leaves):
            if not any(fnmatch.fnmatchcase(p, g) for g in self.paths):
                continue
            truth, presented = leaves[p]
            # int/float/bool/str as-is; anything else (None, containers)
            # is not plottable and is skipped
            if isinstance(truth, (bool, int, float, str)):
                data[p] = truth
            if presented != truth and \
                    isinstance(presented, (bool, int, float, str)):
                data[p + "@presented"] = presented
        return data