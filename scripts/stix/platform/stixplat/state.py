# SPDX-License-Identifier: GPL-2.0-or-later
"""The hardware state tree (platform plan D-PLo).

Leaves are {truth, presented, reported, unit, index_space, t_sim,
status, fault_ids}. PL-3 fills them from the two `hw-state` JSONs and
the controller's own platform state; `reported` (TM) is PL-8.

Change detection skips the volatile leaves (clocks and counters that
move on every sample), so a `state` record means a real change.
"""

import threading

# leaves that move on their own every sample: served, never logged as a
# change
VOLATILE = ("idpu.scet.coarse", "idpu.scet.fine", "idpu.watchdog.count",
            "platform.obt", "platform.timecode.last_timecnt",
            "platform.timecode.last_vclock_ns",
            "platform.timecode.next_due_vclock_ns")


def leaf(truth, presented=None, unit=None, index_space=None, t_sim=0,
         status="ok"):
    return {"truth": truth,
            "presented": truth if presented is None else presented,
            "reported": None, "unit": unit, "index_space": index_space,
            "t_sim": t_sim, "status": status, "fault_ids": []}


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
                    old["t_sim"] = lf["t_sim"]
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
            ids = [f for t, fs in faults.items() for f in fs
                   if p == t or p.startswith(t + ".")]
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
