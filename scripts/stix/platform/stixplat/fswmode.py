# SPDX-License-Identifier: GPL-2.0-or-later
"""The FSW operating mode leaf (U2): `fsw.mode`, set from the TM the
relay sees -- the TM(5,1) EID 0x0021 mode change (source "event", with
the reason) and the TM(3,25) SID 1/2 mode nibble (source "hk").

The leaf's truth is {value, name, source, reason, obt, t}: obt is the
packet's own CUC time code, t the bench time. An HK sampled before a
mode change but delivered after its 0x0021 event must not revert it,
so a sample replaces the leaf only when its OBT is >= the leaf's OBT;
when either OBT is missing, arrival order. The leaf is reset to null
at power-off and at a new boot (a boot_id change).
"""

from . import decode as decode_mod

MODE_EID = 0x21                 # EIDS["mode change"]


class FswMode:
    PATH = "fsw.mode"

    def __init__(self, state, clock, emit):
        """@state is the StateTree, @clock the bench clock and @emit a
        fn({path: value}) that stores the leaf and logs its change
        (core.Controller.state_platform)."""
        self.state = state
        self.clock = clock
        self.emit = emit

    # -- the relay listener ----------------------------------------------------
    def on_tm(self, unit, dec):
        """The relay-listener form: fn(unit, decoded) per TM."""
        if not dec:
            return
        mc = dec.get("mode_change")
        if dec.get("type") == 5 and dec.get("eid") == MODE_EID and mc:
            self.update({"value": mc["to"], "name": mc["to_name"],
                         "source": "event", "reason": mc["reason_name"],
                         "obt": dec.get("cuc")})
        elif dec.get("apid") == decode_mod.APID_HK and \
                dec.get("sid") in (1, 2) and "mode" in dec:
            self.update({"value": dec["mode"],
                         "name": decode_mod.mode_name(dec["mode"]),
                         "source": "hk", "obt": dec.get("cuc")})

    OBT_WINDOW_NS = 5 * 10**9   # sim time

    def update(self, sample):
        """The ordering rule, then the leaf (truth = the sample + t)."""
        lf = self.state.get(self.PATH)
        old = lf.get("truth") if isinstance(lf, dict) else None
        now = self.clock.now()[0]
        # OBT decides only between samples arriving close together (an HK
        # sampled before a mode change but delivered after its event); past
        # the window arrival order wins, so an OBT that steps backwards (a
        # time sync, an obt.jump fault) cannot freeze the leaf.
        if isinstance(old, dict) and isinstance(old.get("obt"), int) and \
                isinstance(sample.get("obt"), int) and \
                sample["obt"] < old["obt"] and \
                now - old.get("t", now) <= self.OBT_WINDOW_NS:
            return False
        sample["t"] = now
        self.emit({self.PATH: sample})
        return True

    def reset(self):
        """Power off or a new boot: null until the next mode TM."""
        lf = self.state.get(self.PATH)
        if lf is not None and lf.get("truth") is not None:
            self.emit({self.PATH: None})
