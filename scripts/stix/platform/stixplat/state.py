# SPDX-License-Identifier: GPL-2.0-or-later
"""The hardware state tree (platform plan D-PLo).

Leaves are {truth, presented, reported, unit, index_space, t_sim,
t_changed, status, fault_ids}. PL-3 fills them from the two
`hw-state` JSONs and the controller's own platform state; `reported`
(TM) is PL-8. `t_changed` is the t_sim of the leaf's last
truth/presented change (UI plan D4): set when `_update` stores a new
value, kept while it holds.

Change detection skips the volatile leaves (clocks and counters that
move on every sample), so a `state` record means a real change.
"""

import threading

# leaves that move on their own every sample: served, never logged as a
# change
VOLATILE = ("idpu.scet.coarse", "idpu.scet.fine", "idpu.watchdog.count",
            "platform.obt", "platform.timecode.last_timecnt",
            "platform.timecode.last_vclock_ns",
            "platform.timecode.next_due_vclock_ns",
            "platform.keepalive.sent", "platform.keepalive.suppressed",
            # PL-11c: the replay position and the ROTB FIFO pointers
            # move on their own while the detector runs
            "instrument.detector.replay.pos_ns",
            "instrument.detector.replay.events",
            "instrument.detector.replay.dropped",
            "instrument.detector.replay.unsupported",
            "instrument.rotb.count", "instrument.rotb.rd",
            "instrument.rotb.wr")


def leaf(truth, presented=None, unit=None, index_space=None, t_sim=0,
         status="ok"):
    return {"truth": truth,
            "presented": truth if presented is None else presented,
            "reported": None, "unit": unit, "index_space": index_space,
            "t_sim": t_sim, "t_changed": t_sim, "status": status,
            "fault_ids": []}


def leaves_from_hw(ctrl, spw, t_sim):
    """Flatten the two hw-state objects (either may be None)."""
    out = {}
    if spw:
        out["idpu.spw.ls"] = leaf(spw["ls"], t_sim=t_sim)
        out["idpu.spw.link_down"] = leaf(spw["link_down"], t_sim=t_sim)
        for k, v in spw["control"].items():
            out["idpu.spw.control." + k] = leaf(v, t_sim=t_sim)
        out["idpu.spw.rx_queued"] = leaf(spw["rx_queued"], t_sim=t_sim)
        for k, v in spw["counters"].items():
            out["idpu.spw.counters." + k] = leaf(v, t_sim=t_sim)
        tc = spw["timecode"]
        for k in ("source", "enabled", "once_pending", "last_timecnt",
                  "last_vclock_ns", "next_due_vclock_ns"):
            out["platform.timecode." + k] = leaf(tc.get(k), t_sim=t_sim)
        out["platform.obt"] = leaf(spw.get("obt"), unit="cuc32.16",
                                   t_sim=t_sim)
        out["platform.obt_kind"] = leaf(spw.get("obt_kind"), t_sim=t_sim)
    if ctrl:
        for name, ln in ctrl["supplies"]["lines"].items():
            out["instrument.supplies.%s.enabled" % name] = \
                leaf(ln["enabled"], t_sim=t_sim)
            out["instrument.supplies.%s.good" % name] = \
                leaf(ln["good"], t_sim=t_sim)
        for ch, c in ctrl["hk_adc"].items():
            out["instrument.hk_adc.%s" % ch] = leaf(
                c["truth"], c["presented"], unit="adc", index_space="ch",
                t_sim=t_sim)
        p = ctrl["hk_prot"]
        for k in ("irqen", "protstat", "hotsample"):
            out["instrument.hk_prot." + k] = leaf(p[k], t_sim=t_sim)
        for ln in p["lines"]:
            base = "instrument.hk_prot.%d." % ln["line"]
            out[base + "threshold"] = leaf(ln["threshold"], unit="adc",
                                           index_space="line", t_sim=t_sim)
            out[base + "prot_en"] = leaf(ln["prot_en"], index_space="line",
                                         t_sim=t_sim)
        for k, v in ctrl["watchdog"].items():
            out["idpu.watchdog." + k] = leaf(v, t_sim=t_sim)
        for k, v in ctrl["scet"].items():
            out["idpu.scet." + k] = leaf(v, t_sim=t_sim)
        for q, on in enumerate(ctrl["det"]["quarter_on"]):
            out["instrument.detector.quarter.%d" % q] = leaf(
                on, index_space="quarter", t_sim=t_sim)
        # PL-11c (D-PSb): the det section's scene/FSM/replay fields (the
        # stix_det_hw_state accessor, D-PLm req 8) and the rotb section.
        # .get within these sections, so a hw-state without the new
        # fields still maps them (the older sections above still index
        # directly); quarter_on keeps its path (no rename — API v1).
        det = ctrl.get("det")
        if det:
            for k in ("source", "flare", "quiet_cps", "flare_cps",
                      "ql_bank"):
                out["instrument.detector." + k] = leaf(det.get(k),
                                                       t_sim=t_sim)
            for k, v in det.get("fsm", {}).items():
                out["instrument.detector.fsm." + k] = leaf(v, t_sim=t_sim)
            rep = det.get("replay")
            if rep:
                for k, v in rep.items():
                    # pos_ns and the event counters join VOLATILE
                    # above: they move on their own while a replay runs
                    out["instrument.detector.replay." + k] = leaf(
                        v, t_sim=t_sim)
        rb = ctrl.get("rotb")
        if rb:
            for k in ("count", "full", "enable", "rd", "wr", "capacity"):
                out["instrument.rotb." + k] = leaf(rb.get(k), t_sim=t_sim)
        # PL-11b (D-PSb): the attenuator and EDAC sections the stixctrl
        # hw-state already carries (stix_att.c:966-998 wired at
        # grlib_stixctrl.c:1621, stix_edac.c:714-759 at :1624-1625),
        # mapped as-is — no emulator change. .get throughout, so an
        # older-schema hw-state without the sections still loads.
        att = ctrl.get("attenuator")
        if att:
            for k, v in att.get("truth", {}).items():
                out["instrument.attenuator.truth." + k] = leaf(
                    v, t_sim=t_sim)
            for k, v in att.get("presented", {}).items():
                out["instrument.attenuator.presented." + k] = leaf(
                    v, t_sim=t_sim)
            out["instrument.attenuator.fsw_accesses"] = leaf(
                att.get("fsw_accesses"), t_sim=t_sim)
            out["instrument.attenuator.faults_active"] = leaf(
                att.get("faults_active"), t_sim=t_sim)
        ed = ctrl.get("edac")
        if ed:
            # instrument.memory.bram maps no leaf: a bram upset arms no
            # poison and is silent (stix_edac.c:887-891), and the
            # counters loop skips the bram (no EDAC, :747-750), so the
            # memory.bram.* faults can never tag one
            for mem, c in ed.get("counters", {}).items():
                for k, v in c.items():
                    out["instrument.memory.%s.%s" % (mem, k)] = leaf(
                        v, t_sim=t_sim)
            # the armed poisons as one leaf (the upsets-active records,
            # stix_edac.c:730-740); they change only on arm/clear, so
            # their change records are real events
            out["instrument.memory.poisons"] = leaf(ed.get("poisons"),
                                                    t_sim=t_sim)
    return out


class StateTree:
    def __init__(self):
        self.lock = threading.Lock()
        self.leaves = {}
        self.fault_ids = None       # () -> {target path: [fault ids]}

    def set_platform(self, values, t_sim):
        """Controller-owned truth (power, mode ...): {path: value}."""
        return self._update({p: leaf(v, t_sim=t_sim)
                             for p, v in values.items()}, replace=False)

    def update_hw(self, ctrl, spw, t_sim):
        """A new poll. Returns {path: (old, new)} of the non-volatile
        leaves whose truth or presented changed."""
        return self._update(leaves_from_hw(ctrl, spw, t_sim), replace=False)

    def clear_hw(self):
        """Power off: the IDPU and instrument subtrees are gone."""
        with self.lock:
            for p in [p for p in self.leaves
                      if p.startswith(("idpu.", "instrument.")) or
                      p.startswith("platform.timecode.") or
                      p in ("platform.obt", "platform.obt_kind")]:
                del self.leaves[p]

    def _update(self, new, replace):
        changes = {}
        with self.lock:
            for p, lf in new.items():
                old = self.leaves.get(p)
                if old is not None and old["truth"] == lf["truth"] and \
                        old["presented"] == lf["presented"]:
                    old["t_sim"] = lf["t_sim"]   # t_changed is kept
                    continue
                self.leaves[p] = lf
                if p not in VOLATILE:
                    changes[p] = (None if old is None else
                                  {"truth": old["truth"],
                                   "presented": old["presented"]},
                                  {"truth": lf["truth"],
                                   "presented": lf["presented"]})
        return changes

    def get(self, prefix=None):
        """The leaves under @prefix (a dotted path) as a nested dict;
        None if nothing matches."""
        faults = self.fault_ids() if self.fault_ids else {}
        with self.lock:
            items = sorted((p, dict(lf)) for p, lf in self.leaves.items()
                           if prefix is None or p == prefix or
                           p.startswith(prefix + "."))
        for p, lf in items:
            # a catalogue target names the signal (instrument.attenuator.
            # sensor_ab); a device that exports both sides has leaves
            # under .truth./.presented. — match the target on either
            q = p.replace(".truth.", ".").replace(".presented.", ".")
            ids = [f for t, fs in faults.items() for f in fs
                   if any(x == t or x.startswith(t + ".") for x in (p, q))]
            if ids:
                lf["fault_ids"] = ids
        if not items:
            return None
        if prefix is not None and len(items) == 1 and items[0][0] == prefix:
            return items[0][1]
        root = {}
        for p, lf in items:
            rel = p if prefix is None else p[len(prefix) + 1:]
            node = root
            parts = rel.split(".")
            for part in parts[:-1]:
                node = node.setdefault(part, {})
            node[parts[-1]] = lf
        return root
