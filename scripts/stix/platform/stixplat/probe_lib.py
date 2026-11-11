# SPDX-License-Identifier: GPL-2.0-or-later
"""The probe as a library (platform plan D-PLi): the controller imports
scripts/stix/tmtc-probe.py by path for frame_units, unit_fields and the
Expect/CucExpect/Forbid matchers. The probe file does not move, so the
gates that run it stay unchanged.
"""

import importlib.util
import os

PROBE_PATH = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "tmtc-probe.py"))
NEEDED = ("frame_units", "unit_fields", "Expect", "CucExpect", "Forbid",
          "qmp_address")
_probe = None


def load():
    global _probe
    if _probe is None:
        spec = importlib.util.spec_from_file_location("stix_tmtc_probe",
                                                      PROBE_PATH)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        missing = [n for n in NEEDED if not hasattr(mod, n)]
        if missing:
            raise ImportError("tmtc-probe.py lacks %s" % ", ".join(missing))
        _probe = mod
    return _probe
