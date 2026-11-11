# SPDX-License-Identifier: GPL-2.0-or-later
"""Value forcing (platform plan D-PLq): set a `presented` value
directly. In v1 that is instrument.hk_adc.<ch> through stixctrl's
adc-inject[ch]. It is a low-level tool, logged as `force`.

The device reset clears adc-inject (grlib_stixctrl.c), so the active
forcings are re-applied after a warm guest reset and before `cont`
after a cold power-on (D-PLf).
"""

import re
import threading

from .dispatch import VerbError

ADC_PATH = re.compile(r"^instrument\.hk_adc\.(\d+)$")
ADC_CLEAR = 0xFFFFFFFF          # "no override" (fault-smoke's CLEAR)
HK_CHANNELS = 128             # hkAdc ADDR is 7 bits
STIXCTRL = "/machine/stixctrl"


def adc_channel(path):
    m = ADC_PATH.match(path or "")
    if not m or int(m.group(1)) >= HK_CHANNELS:
        raise VerbError(400, "force path %r is not instrument.hk_adc.<ch> "
                        "(ch 0..%d)" % (path, HK_CHANNELS - 1))
    return int(m.group(1))


class Forcing:
    def __init__(self, log, qmp):
        """@qmp() -> the live QmpConnection or None."""
        self.log = log
        self.qmp = qmp
        self.active = {}        # path -> value
        self.lock = threading.Lock()

    def _apply(self, ch, value):
        q = self.qmp()
        if q is None:
            return False
        q.qom_set(STIXCTRL, "adc-inject[%d]" % ch, value)
        return True

    def set(self, path, value, source="api", why=None):
        ch = adc_channel(path)
        if not 0 <= value <= 4095:
            raise VerbError(400, "an HK ADC value is 0..4095")
        with self.lock:
            applied = self._apply(ch, value)
            self.active[path] = value
        self.log.emit(source, "force", path,
                      {"action": "set", "value": value, "ch": ch,
                       "applied": applied, "why": why})
        return {"path": path, "value": value, "applied": applied}

    def clear(self, path, source="api", why=None):
        ch = adc_channel(path)
        with self.lock:
            if path not in self.active:
                raise VerbError(409, "%s is not forced" % path)
            applied = self._apply(ch, ADC_CLEAR)
            del self.active[path]
        self.log.emit(source, "force", path,
                      {"action": "clear", "ch": ch, "applied": applied,
                       "why": why})
        return {"path": path, "applied": applied}

    def reapply(self, why):
        with self.lock:
            items = sorted(self.active.items())
            for path, value in items:
                self._apply(adc_channel(path), value)
        if items:
            self.log.emit("ctl", "force", "force.reapply",
                          {"why": why, "forcings": dict(items)})

    def status(self):
        with self.lock:
            return dict(self.active)
