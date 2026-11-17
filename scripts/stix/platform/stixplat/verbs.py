# SPDX-License-Identifier: GPL-2.0-or-later
"""The verb registry: every verb a controller registers, so run mode
can refuse a scenario with a mistyped verb before it starts anything
(review S4). The self-test checks it equals a live dispatcher's."""

VERBS = frozenset({
    "att.cmdsta.clear", "att.cmdsta.set",
    "debug.qmp", "det.adc.set", "det.align.set", "det.inject",
    "det.replay.config", "det.replay.start", "det.replay.stop",
    "det.rotb.burst", "det.scene.set",
    "det.tape.delete", "det.tape.play", "det.tape.save", "det.tape.stop",
    "fault.clear", "fault.inject",
    "force.clear", "force.set", "log.mark", "mem.edac.enable",
    "mem.upset.burst", "mem.upset.clear", "param.read",
    "param.read_all", "param.refresh", "param.reset_all", "param.set",
    "platform.flash.list", "platform.flash.load", "platform.flash.save",
    "platform.keepalive.set", "platform.obt.set",
    "platform.power.boot", "platform.power.cycle", "platform.power.off",
    "platform.power.on", "platform.spw.link", "platform.timecode.enable",
    "platform.timecode.inject", "platform.timecode.once",
    "platform.timesync.send", "run.baseline", "run.baseline.clear",
    "run.close", "run.compare", "run.open", "scenario.abort",
    "scenario.run", "sim.pause", "sim.resume", "tc.gs.send",
    "tc.platform.send", "view.delete", "view.save"})
