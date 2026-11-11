# SPDX-License-Identifier: GPL-2.0-or-later
"""The verb registry: every verb a controller registers, so run mode
can refuse a scenario with a mistyped verb before it starts anything
(review S4). The self-test checks it equals a live dispatcher's."""

VERBS = frozenset({
    "debug.qmp", "fault.clear", "fault.inject", "force.clear", "force.set",
    "log.mark", "platform.keepalive.set", "platform.obt.set",
    "platform.power.boot", "platform.power.cycle", "platform.power.off",
    "platform.power.on", "platform.spw.link", "platform.timecode.enable",
    "platform.timecode.inject", "platform.timecode.once",
    "platform.timesync.send", "scenario.abort", "scenario.run",
    "sim.pause", "sim.resume", "tc.gs.send", "tc.platform.send"})
