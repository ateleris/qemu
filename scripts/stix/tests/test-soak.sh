#!/usr/bin/env bash
# Tests for scripts/stix/soak.sh. The first version sampled the UART log
# only after QEMU had been killed by its timeout, so both samples read the
# same closed file and every run "FAILED — heartbeat stopped": the FSW
# "stall at FSW ~4.8 min" was the 600 s kill landing at FSW time 0.48x
# wall (see docs/stix-port/2026-11-09-task-b-stall-analysis.md). These
# cases pin that liveness is sampled while QEMU is still running.
#
# Since R3 models the watchdog, the once-per-second UART error print is
# gone and the soak heartbeat is TM FLOW: the gate samples the probe log's
# TM chunk lines per SLICE-second slice of the window. UART growth is
# reported but not gated on.
#
# Plain bash; each case uses a fresh temp run dir with an empty flash.bin
# and a dummy ELF — the real 16.5 GB flash.bin is never touched. Short
# windows via STIX_SOAK_WINDOW / STIX_SOAK_SLICE / STIX_SOAK_MARGIN. TM is
# faked with make_fake_probe (same output format as tmtc-probe.py), since
# fake-qemu.sh opens no TMTC port.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SOAK="$REPO_ROOT/scripts/stix/soak.sh"
FAKE="$REPO_ROOT/scripts/stix/tests/fake-qemu.sh"
PASS=0
FAIL=0

report() { # name ok|<failure detail>
    if [ "$2" = "ok" ]; then
        echo "ok   - $1"
        PASS=$((PASS + 1))
    else
        echo "FAIL - $1: $2"
        FAIL=$((FAIL + 1))
    fi
}

fresh_run_dir() {
    local d
    d="$(mktemp -d)"
    : > "$d/flash.bin"
    echo "dummy elf" > "$d/dummy.elf"
    echo "$d"
}

# Fake TM probe, passed to soak.sh via STIX_TMTC_PROBE (fake-qemu.sh opens
# no TMTC port, so the real probe would never see a byte). Mimics
# tmtc-probe.py's output: "# connected", then a chunk line in the exact
# probe format ("HH:MM:SS +NNNNB <hex>") every 0.5 s for
# FAKE_PROBE_TM_SECONDS seconds (env; default: forever until killed), each
# line flushed — soak.sh counts these lines in the probe log while the
# probe still runs (its stdout is block-buffered when redirected to a
# file). After the TM window it stays alive and silent; on SIGTERM it
# prints "# total <FAKE_PROBE_TOTAL_BYTES, 37> bytes" like the real probe's
# summary, then exits.
make_fake_probe() { # dir
    cat > "$1/fake-probe.py" <<'EOF'
import os, signal, time
total = os.environ.get("FAKE_PROBE_TOTAL_BYTES", "37")
tm_seconds = os.environ.get("FAKE_PROBE_TM_SECONDS")
print("# connected (fake probe)", flush=True)
def on_term(sig, frame):
    print(f"# total {total} bytes", flush=True)
    os._exit(0)
signal.signal(signal.SIGTERM, on_term)
end = None if tm_seconds is None else time.monotonic() + float(tm_seconds)
while end is None or time.monotonic() < end:
    print(f"{time.strftime('%H:%M:%S')} +{37:4d}B " + "ab" * 37, flush=True)
    time.sleep(0.5)
while True:  # TM over: alive and silent until killed
    time.sleep(3600)
EOF
}

run_soak() { # dir total [env...]  -> sets out, rc, elapsed
    local d="$1" total="$2"; shift 2
    local t0=$SECONDS
    out="$(env STIX_RUN_DIR="$d" STIX_ASW_ELF="$d/dummy.elf" \
        STIX_SOAK_WINDOW=4 STIX_SOAK_SLICE=2 STIX_SOAK_MARGIN=2 "$@" \
        bash "$SOAK" "$FAKE" "$total" 2>&1)"
    rc=$?
    elapsed=$((SECONDS - t0))
}

chmod +x "$FAKE" 2>/dev/null || true

# --- case a: guest prints until the timeout kills it => PASS, and the
# script ends with the window (not a full window after QEMU died). The
# elapsed budget is 20 s, not 10+4: when the fake probe is still alive at
# the end, soak.sh waits up to 5x1 s for it to print its total. ---
d="$(fresh_run_dir)"
make_fake_probe "$d"
run_soak "$d" 10 FAKE_QEMU_MODE=heartbeat FAKE_QEMU_RUN_SECONDS=60 STIX_TMTC_PROBE="$d/fake-probe.py"
if [ "$rc" -eq 0 ] && echo "$out" | grep -q "SOAK: PASS" && [ "$elapsed" -le 20 ]; then
    report "case a: live guest to the timeout => PASS within the window" ok
else
    report "case a: live guest to the timeout => PASS within the window" \
        "rc=$rc elapsed=${elapsed}s: $out"
fi
rm -rf "$d"

# --- case b: UART freeze alone is not a failure (R3 models the watchdog,
# so a healthy guest can have a silent UART): QEMU alive, UART silent, but
# TM keeps flowing => PASS exit 0. (A hung guest with no TM is case h.) ---
d="$(fresh_run_dir)"
make_fake_probe "$d"
run_soak "$d" 10 FAKE_QEMU_MODE=freeze FAKE_QEMU_FREEZE_AFTER=2 FAKE_QEMU_RUN_SECONDS=60 STIX_TMTC_PROBE="$d/fake-probe.py"
if [ "$rc" -eq 0 ] && echo "$out" | grep -q "SOAK: PASS"; then
    report "case b: UART freeze alone is not a failure => PASS exit 0" ok
else
    report "case b: UART freeze alone is not a failure => PASS exit 0" "rc=$rc: $out"
fi
rm -rf "$d"

# --- case c: QEMU crashes => exit 2, without sitting out the window ---
d="$(fresh_run_dir)"
run_soak "$d" 10 FAKE_QEMU_MODE=crash
if [ "$rc" -eq 2 ] && [ "$elapsed" -le 5 ]; then
    report "case c: crashing QEMU => exit 2 promptly" ok
else
    report "case c: crashing QEMU => exit 2 promptly" "rc=$rc elapsed=${elapsed}s: $out"
fi
rm -rf "$d"

# --- case d: QEMU exits cleanly before the window ends => exit 2 (a
# -no-reboot guest reset/halt is not a healthy soak) ---
d="$(fresh_run_dir)"
run_soak "$d" 10 FAKE_QEMU_MODE=heartbeat FAKE_QEMU_RUN_SECONDS=3
if [ "$rc" -eq 2 ]; then
    report "case d: early clean QEMU exit => exit 2" ok
else
    report "case d: early clean QEMU exit => exit 2" "rc=$rc: $out"
fi
rm -rf "$d"

# --- case e: window does not fit in the run => exit 2 up front ---
d="$(fresh_run_dir)"
run_soak "$d" 5 FAKE_QEMU_MODE=heartbeat
if [ "$rc" -eq 2 ] && [ "$elapsed" -le 2 ]; then
    report "case e: window longer than the run => exit 2" ok
else
    report "case e: window longer than the run => exit 2" "rc=$rc elapsed=${elapsed}s: $out"
fi
rm -rf "$d"

# --- case f: healthy run where the TM probe has already exited at the end
# (what a real probe does when QEMU's socket closes) => still exit 0. The
# first real PASS printed "SOAK: PASS" and exited 1: the EXIT trap's kill
# of the gone probe failed under set -e. The probe prints chunk lines
# through the whole gate window (window 4s..8s), prints its total and
# exits ~1.8 s before the run ends. ---
d="$(fresh_run_dir)"
cat > "$d/fake-probe.py" <<'EOF'
import time
print("# connected (fake probe)", flush=True)
end = time.monotonic() + 8.2
while time.monotonic() < end:
    print(f"{time.strftime('%H:%M:%S')} +{37:4d}B " + "ab" * 37, flush=True)
    time.sleep(0.5)
print("# total 37 bytes", flush=True)
EOF
run_soak "$d" 10 FAKE_QEMU_MODE=heartbeat FAKE_QEMU_RUN_SECONDS=60 STIX_TMTC_PROBE="$d/fake-probe.py"
if [ "$rc" -eq 0 ] && echo "$out" | grep -q "SOAK: PASS.*37 bytes"; then
    report "case f: probe gone at the end => PASS exit 0" ok
else
    report "case f: probe gone at the end => PASS exit 0" "rc=$rc: $out"
fi
rm -rf "$d"

# --- case g: UART silent the whole run (healthy since R3: no watchdog
# error print), TM flowing => PASS exit 0, and the PASS line reports the
# per-slice TM chunk counts ---
d="$(fresh_run_dir)"
make_fake_probe "$d"
run_soak "$d" 10 FAKE_QEMU_MODE=silent FAKE_QEMU_RUN_SECONDS=60 STIX_TMTC_PROBE="$d/fake-probe.py"
if [ "$rc" -eq 0 ] && echo "$out" | grep -q "SOAK: PASS" \
        && echo "$out" | grep -q "TM chunks/slice"; then
    report "case g: silent UART but TM flowing => PASS exit 0" ok
else
    report "case g: silent UART but TM flowing => PASS exit 0" "rc=$rc: $out"
fi
rm -rf "$d"

# --- case h: QEMU alive, UART heartbeat flowing, but TM stops after 2 s
# (hung guest: no housekeeping TM) => exit 1, "TM heartbeat stopped" ---
d="$(fresh_run_dir)"
make_fake_probe "$d"
run_soak "$d" 10 FAKE_QEMU_MODE=heartbeat FAKE_QEMU_RUN_SECONDS=60 \
    FAKE_PROBE_TM_SECONDS=2 FAKE_PROBE_TOTAL_BYTES=10 STIX_TMTC_PROBE="$d/fake-probe.py"
if [ "$rc" -eq 1 ] && echo "$out" | grep -q "TM heartbeat stopped"; then
    report "case h: TM stops => FAIL exit 1" ok
else
    report "case h: TM stops => FAIL exit 1" "rc=$rc: $out"
fi
rm -rf "$d"

# --- case i: TM flows only in the first slice of the window, then stops
# (window is 4s..8s after start; TM until 5 s) => exit 1 and the message
# names slice 2/2 — pins per-slice gating, not whole-window gating ---
d="$(fresh_run_dir)"
make_fake_probe "$d"
run_soak "$d" 10 FAKE_QEMU_MODE=heartbeat FAKE_QEMU_RUN_SECONDS=60 \
    FAKE_PROBE_TM_SECONDS=5 FAKE_PROBE_TOTAL_BYTES=10 STIX_TMTC_PROBE="$d/fake-probe.py"
if [ "$rc" -eq 1 ] && echo "$out" | grep -q "TM heartbeat stopped" \
        && echo "$out" | grep -q "slice 2/2"; then
    report "case i: TM stops mid-window => FAIL names slice 2/2" ok
else
    report "case i: TM stops mid-window => FAIL names slice 2/2" "rc=$rc: $out"
fi
rm -rf "$d"

# --- case j: window not a positive multiple of the slice => exit 2 up
# front (environment problem, before QEMU starts) ---
d="$(fresh_run_dir)"
run_soak "$d" 10 FAKE_QEMU_MODE=heartbeat STIX_SOAK_WINDOW=5
if [ "$rc" -eq 2 ] && [ "$elapsed" -le 2 ] && echo "$out" | grep -q "STIX_SOAK_SLICE"; then
    report "case j: window 5 not a multiple of slice 2 => exit 2 up front" ok
else
    report "case j: window 5 not a multiple of slice 2 => exit 2 up front" \
        "rc=$rc elapsed=${elapsed}s: $out"
fi
rm -rf "$d"

echo "----------------------------------------"
echo "pass=$PASS fail=$FAIL"
[ "$FAIL" -eq 0 ]
