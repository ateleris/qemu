#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# link-smoke: the live gate of the platform link (PL-4): the TM relay, the
# TC proxy, the keep-alive, the time sync, the time-code switches and the
# first fault, against the real ASW. REST through the controller's API, a
# fake GS UDP listener for the relayed TM. Ports and run dir from
# plat-slot.sh; legacy (STIX_PORT_BASE unset): the API on 21525, QEMU
# TM/TC 21526/21527, GS TC 21528, the fake GS on 21531; run dir
# /tmp/stix-plat/link-smoke (a fresh sparse flash made by QEMU). Checks:
#   boot: the boot report TM (EID 0x0022) reaches the fake GS
#   the automatic time sync at the first TM: verdict ok, no manual send
#   proxy: TC(17,1) from the fake GS gives TM(1,1), TM(17,2), TM(1,7)
#   keep-alive: no 0x142E for 90 s; paused 40 s mid-run, still none (a
#     sanity check: the FSW timer freezes too); off: 0x142E within 40 s
#   time sync: TM(1,7) within 3 s, the first heartbeat's CUC equals the
#     OBT (verdict ok, ordering ok); a second send while pending: 409
#   time codes off: the sync gets TM(1,8) FID 48035 within 4 s
#   once: exactly one code
#   SpW down: keep-alive suppressed, time sync 409, a GS TC dropped and
#     logged, no TM; up again: TM resumes
#   fault.inject spw.link.disconnect: the same silence, logged as fault
#   link-down set while frozen survives cont: no TM until link up
#   relay: the fake GS got byte-identical bare SPs, one per tm.jsonl line
#   the UI page this server serves matches its verbs and CSP (--check-ui)
#
# Usage: link-smoke.sh <qemu-binary>   (STIX_ASW_ELF as in boot-smoke.sh)
# Legacy: run it under flock /tmp/stix-qtest-21000.lock; STIX_PORT_BASE=
# auto|k takes a slot (lib/slot.sh, band and run dir -s<k>). --dry-run
# (first argument) prints the ports and run dir and starts nothing.
# Exit codes: 0 pass (or SKIP), 1 a check failed, 2 environment problem.
set -euo pipefail

DRY=0
if [ "${1:-}" = --dry-run ]; then DRY=1; shift; fi
USAGE="usage: link-smoke.sh [--dry-run] <qemu-binary>"
QEMU_BIN="$(realpath -e "${1:?$USAGE}")" || exit 2
. "$(dirname "${BASH_SOURCE[0]}")/../../lib/paths.sh"
ASW_ELF="${STIX_ASW_ELF:-$STIX_DEFAULT_ELF}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=plat-slot.sh
. "$HERE/plat-slot.sh"
if [ "$DRY" = 1 ] || [ ! -f "$ASW_ELF" ]; then
    plat_slot_setup link-smoke 21525 peek
    [ "$DRY" = 0 ] || { plat_print_ports; exit 0; }
    echo "SKIP: ASW ELF absent ($ASW_ELF)"
    exit 0
fi
plat_slot_setup link-smoke 21525
RUN="$PLAT_RUN"
export STIX_SLOT_TAG
plat_ports_free

mkdir -p "$RUN"
cat > "$RUN/link-smoke.toml" <<EOF
mode = "interactive"
[run_dir]
path = "$RUN"
fresh = true
[qemu]
binary = "$QEMU_BIN"
elf = "$ASW_ELF"
[ports]
api = $PLAT_API
qemu_tm = $PLAT_QEMU_TM
qemu_tc = $PLAT_QEMU_TC
gs_tc = $PLAT_GS_TC
[gs]
tm_dests = ["127.0.0.1:$PLAT_SINK"]
tc_during_link_down = "drop"
[power]
off_time_s = 2.0
EOF

exec python3 - "$HERE/../stixplat" "$RUN" <<'PYEOF'
import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

PKG, RUN = sys.argv[1], sys.argv[2]
sys.path.insert(0, os.path.dirname(PKG))
from stixplat import decode, tcbuild          # noqa: E402

API = "http://127.0.0.1:%s/api/v1" % os.environ["PLAT_API"]
GS = ("127.0.0.1", int(os.environ["PLAT_SINK"]))
GS_TC = ("127.0.0.1", int(os.environ["PLAT_GS_TC"]))
TAG = os.environ.get("STIX_SLOT_TAG", "")
NS = 1_000_000_000


class Fail(Exception):
    pass


def ok(msg):
    print("ok   " + msg)
    sys.stdout.flush()


def check(cond, msg):
    if not cond:
        raise Fail(msg)


def req(method, path, body=None):
    data = None if body is None else json.dumps(body).encode()
    r = urllib.request.Request(API + path, data=data, method=method)
    r.add_header("X-Stix-Platform", "1")
    if data is not None:
        r.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(r, timeout=30) as f:
            return f.status, json.loads(f.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def cmd(verb, args=None, want=200):
    st, r = req("POST", "/cmd/" + verb, args or {})
    check(st == want, "%s %r: HTTP %d %r" % (verb, args, st, r))
    return r.get("result") if isinstance(r, dict) else r


def status():
    return req("GET", "/status")[1]


def sim_now():
    return req("GET", "/time")[1]["sim_time"]


def records(kind=None, path=None):
    out = []
    names = sorted((n for n in os.listdir(RUN) if n.startswith("log.jsonl")),
                   key=lambda n: (n != "log.jsonl", n), reverse=True)
    for n in names:
        if n.endswith(".gz"):
            continue
        with open(os.path.join(RUN, n)) as f:
            for line in f:
                if not line.endswith("\n"):
                    continue
                r = json.loads(line)
                if (kind is None or r["kind"] == kind) and \
                        (path is None or r["path"] == path):
                    out.append(r)
    out.sort(key=lambda r: r["seq"])
    return out


class FakeGS:
    """The GS end of the relay: collects every datagram (bare SP)."""

    def __init__(self):
        self.sk = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sk.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 22)
        self.sk.bind(GS)
        self.sk.settimeout(0.2)
        self.got = []           # (monotonic, bytes)
        self.lock = threading.Lock()
        self.stop = False
        self.t = threading.Thread(target=self._loop, daemon=True)
        self.t.start()

    def _loop(self):
        while not self.stop:
            try:
                data, _ = self.sk.recvfrom(70000)
            except socket.timeout:
                continue
            with self.lock:
                self.got.append((time.monotonic(), data))

    def mark(self):
        with self.lock:
            return len(self.got)

    def since(self, mark):
        with self.lock:
            return [(t, decode.decode(b"\x49\x02\x00\x00" + d), d)
                    for t, d in self.got[mark:]]

    def wait(self, mark, pred, timeout, what):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for t, d, raw in self.since(mark):
                if d is not None and pred(d):
                    return d
            time.sleep(0.05)
        raise Fail("timed out (%.0f s) waiting for %s" % (timeout, what))

    def send_tc(self, sp):
        self.sk.sendto(sp, GS_TC)

    def close(self):
        self.stop = True
        self.t.join(2)
        self.sk.close()


def eid(d, e):
    return d["type"] == 5 and d.get("eid") == e


def tm_is(d, t, s):
    return (d["type"], d["subtype"]) == (t, s)


def wait_sim(seconds):
    """Wait until `seconds` of sim time have passed (bench time)."""
    t0 = sim_now()
    while sim_now() - t0 < seconds * NS:
        time.sleep(0.2)


def emits(boot_id):
    return sum(1 for r in records("log", "trace.stixspw_timecode_emit")
               if r["boot_id"] == boot_id)


def main(gs, ctl):
    for _ in range(100):
        try:
            if req("GET", "/status")[0] == 200:
                break
        except OSError:
            pass
        time.sleep(0.1)
    else:
        raise Fail("the API did not come up")

    # -- boot: the boot report through the relay ------------------------
    cmd("platform.power.on")
    cmd("platform.power.boot")
    m0 = 0
    gs.wait(m0, lambda d: eid(d, 0x0022), 50, "the boot report (EID 0x0022)")
    ok("boot: the boot report TM (EID 0x0022) reached the fake GS")
    gs.wait(m0, lambda d: eid(d, 0x14A3), 60, "FDIR enabled (EID 0x14A3)")
    sim_first_tm = records("tm")[0]["sim_time"]

    # -- the automatic time sync (B2): no manual send ----------------------
    auto = v = None
    end = time.monotonic() + 30
    while time.monotonic() < end and v is None:
        autos = records("log", "platform.timesync.auto")
        if autos:
            auto = autos[0]["data"]
            check(auto["ok"] and auto["trigger"] == "first_tm",
                  "auto sync %r" % auto)
            for e in records("expect", "platform.timesync"):
                if e["data"]["seq"] == auto["seq"]:
                    v = e["data"]
        time.sleep(0.2)
    check(v is not None, "no verdict for the auto sync %r" % auto)
    check(v["verdict"] == "ok" and v["ordering"] == "ok" and not v["raw"],
          "auto sync verdict %r" % v)
    check(not records("cmd", "platform.timesync.send"),
          "a manual send before the auto check")
    ok("auto time sync at the first TM: verdict ok (%.3f s), no manual "
       "send" % v["diff_s"])

    # -- proxy: TC(17,1) from the fake GS ------------------------------
    m = gs.mark()
    tc17 = tcbuild.tc_packet(tcbuild.APID_TC, 100, 17, 1, source=0)
    gs.send_tc(tc17)
    want = {(1, 1): False, (17, 2): False, (1, 7): False}
    end = time.monotonic() + 5
    while time.monotonic() < end and not all(want.values()):
        for _, d, _ in gs.since(m):
            if d is not None and (d["type"], d["subtype"]) in want:
                if d["type"] != 1 or d["tc"]["seq"] == 100:
                    want[(d["type"], d["subtype"])] = True
        time.sleep(0.05)
    check(all(want.values()), "proxy: TM %r" % want)
    tcs = [r for r in records("tc", "tc.gs") if r["data"].get("seq") == 100]
    check(tcs and tcs[-1]["data"]["delivered"], "proxy: no tc record")
    ok("proxy: TC(17,1) from the fake GS gave TM(1,1), TM(17,2), TM(1,7)")

    # -- keep-alive -------------------------------------------------------
    st = status()["platform"]["keepalive"]
    check(st["started"] and st["sent"] > 0, "keep-alive not running %r" % st)
    m_ka = 0
    while sim_now() - sim_first_tm < 50 * NS:
        time.sleep(0.5)
    cmd("sim.pause")
    time.sleep(40)
    cmd("sim.resume")
    while sim_now() - sim_first_tm < 90 * NS:
        time.sleep(0.5)
    bad = [d for _, d, _ in gs.since(m_ka) if d and eid(d, 0x142E)]
    check(not bad, "0x142E with the keep-alive on")
    ok("keep-alive: no 0x142E in 90 s of sim time (a 40 s pause midway, "
       "a sanity check)")
    m = gs.mark()
    cmd("platform.keepalive.set", {"enabled": False})
    t0 = sim_now()
    d = gs.wait(m, lambda d: eid(d, 0x142E), 60, "0x142E with it off")
    t1 = sim_now()
    check(t1 - t0 < 40 * NS, "0x142E after %.1f s" % ((t1 - t0) / NS))
    cmd("platform.keepalive.set", {"enabled": True})
    ok("keep-alive off: 0x142E within %.1f s of sim time" % ((t1 - t0) / NS))

    # -- time sync ----------------------------------------------------------
    m = gs.mark()
    r = cmd("platform.timesync.send")
    seq = r["seq"]
    st, r2 = req("POST", "/cmd/platform.timesync.send", {})
    check(st == 409, "second sync while pending: %d %r" % (st, r2))
    st, r3 = req("POST", "/cmd/tc.platform.send",
                 {"hex": tcbuild.tc9_129(seq + 1, r["coarse"]).hex()})
    check(st == 409, "raw TC(9,129) while pending: %d %r" % (st, r3))
    d = gs.wait(m, lambda d: tm_is(d, 1, 7) and d["tc"]["seq"] == seq, 3,
                "TM(1,7) of the sync")
    v = None
    end = time.monotonic() + 20
    while time.monotonic() < end and v is None:
        for e in records("expect", "platform.timesync"):
            if e["data"]["seq"] == seq:
                v = e["data"]
        time.sleep(0.2)
    check(v is not None, "no time-sync verdict")
    check(v["verdict"] == "ok" and v["ordering"] == "ok",
          "time sync verdict %r" % v)
    ok("time sync: TM(1,7) in 3 s; CUC 0x%012X vs OBT 0x%012X (%.3f s), "
       "RX before the applying emit; a second send got 409"
       % (v["cuc"], v["obt"], v["diff_s"]))

    # -- time codes off: TM(1,8) 48035 ------------------------------------
    cmd("platform.timecode.enable", {"enabled": False})
    time.sleep(1.5)
    m = gs.mark()
    r = cmd("platform.timesync.send")
    d = gs.wait(m, lambda d: tm_is(d, 1, 8) and d["tc"]["seq"] == r["seq"],
                4, "TM(1,8) with the time codes off")
    check(d["fid"] == 48035, "FID %r" % d.get("fid"))
    ok("time codes off: TM(1,8) FID 48035")

    # -- once ----------------------------------------------------------------
    boot = status()["boot_id"]
    time.sleep(2)
    n0 = emits(boot)
    cmd("platform.timecode.once")
    time.sleep(3)
    n1 = emits(boot)
    check(n1 - n0 == 1, "once gave %d codes" % (n1 - n0))
    cmd("platform.timecode.enable", {"enabled": True})
    ok("once: exactly one code")

    # -- SpW down ----------------------------------------------------------
    sup0 = status()["platform"]["keepalive"]["suppressed"]
    cmd("platform.spw.link", {"state": "down"})
    time.sleep(0.5)
    m = gs.mark()
    st, r = req("POST", "/cmd/platform.timesync.send", {})
    check(st == 409, "time sync with SpW down: %d" % st)
    gs.send_tc(tcbuild.tc_packet(tcbuild.APID_TC, 101, 17, 1, source=0))
    time.sleep(3)
    sup1 = status()["platform"]["keepalive"]["suppressed"]
    check(sup1 > sup0, "keep-alive not suppressed (%d)" % sup1)
    drops = [x for x in records("tc", "tc.gs") if x["data"].get("seq") == 101]
    check(drops and drops[-1]["data"].get("dropped"), "GS TC not dropped")
    check(not gs.since(m), "%d TM with SpW down" % len(gs.since(m)))
    m = gs.mark()
    cmd("platform.spw.link", {"state": "up"})
    gs.wait(m, lambda d: True, 10, "TM after the link is up")
    ok("SpW down: keep-alive suppressed (%d), sync 409, GS TC dropped, "
       "no TM; up: TM resumes" % (sup1 - sup0))

    # -- the fault ----------------------------------------------------------
    r = cmd("fault.inject", {"id": "spw.link.disconnect"})
    time.sleep(0.5)
    m = gs.mark()
    time.sleep(3)
    check(not gs.since(m), "TM during the link fault")
    check(status()["platform"]["link"] == "down", "link not down")
    m = gs.mark()
    cmd("fault.clear", {"id": "spw.link.disconnect"})
    gs.wait(m, lambda d: True, 10, "TM after the fault clear")
    f = [x["data"]["action"] for x in records("fault",
                                              "fault.spw.link.disconnect")]
    check(f == ["inject", "clear"], "fault records %r" % f)
    ok("fault spw.link.disconnect: silence like link down, logged as fault "
       "(seed %d)" % r["seed"])

    # -- link-down set while frozen survives cont -------------------------
    cmd("platform.power.off")
    cmd("platform.power.on")
    cmd("platform.spw.link", {"state": "down"})
    m = gs.mark()
    cmd("platform.power.boot")
    boot = status()["boot_id"]
    time.sleep(20)
    check(not gs.since(m), "TM with link-down set while frozen")
    check(not [x for x in records("tm") if x["boot_id"] == boot],
          "tm records with link-down")
    cmd("platform.spw.link", {"state": "up"})
    gs.wait(m, lambda d: True, 15, "TM after link up in the new boot")
    ok("link-down set while frozen survived cont: no TM in 20 s, TM after up")


def finish(gs, ctl):
    ctl.send_signal(signal.SIGTERM)
    rc = ctl.wait(30)
    check(rc == 0, "controller rc %d" % rc)
    time.sleep(0.5)
    gs.close()
    with open(os.path.join(RUN, "tm.jsonl")) as f:
        lines = [json.loads(x)["hex"] for x in f]
    got = [d.hex() for _, d in gs.got]
    check(len(got) == len(lines) and got == lines,
          "relay: %d datagrams vs %d tm.jsonl lines; first diff at %s"
          % (len(got), len(lines),
             next((i for i, (a, b) in enumerate(zip(got, lines)) if a != b),
                  min(len(got), len(lines)))))
    ok("relay: %d bare SPs byte-identical to tm.jsonl, one per line"
       % len(got))


def run():
    gs = FakeGS()
    out = open(os.path.join(RUN, "controller.out"), "w")
    ctl = subprocess.Popen([sys.executable, os.path.join(PKG, "..",
                                                         "stixplat.py"),
                            "--config", os.path.join(RUN, "link-smoke.toml")],
                           stdout=out, stderr=subprocess.STDOUT)
    try:
        main(gs, ctl)
        ui = subprocess.run([sys.executable,
                             os.path.join(PKG, "..", "stixplat.py"),
                             "--check-ui", API[:-len("/api/v1")]],
                            capture_output=True, text=True, timeout=60)
        check(ui.returncode == 0, ui.stdout.strip() or ui.stderr.strip())
        ok(ui.stdout.strip()[5:])
        finish(gs, ctl)
    except Fail as e:
        print("LINK-SMOKE: FAIL: %s%s" % (e, TAG))
        return 1
    finally:
        if ctl.poll() is None:
            ctl.send_signal(signal.SIGTERM)
            try:
                ctl.wait(30)
            except subprocess.TimeoutExpired:
                ctl.kill()
        gs.stop = True
    print("LINK-SMOKE: PASS" + TAG)
    return 0


sys.exit(run())
PYEOF
