# SPDX-License-Identifier: GPL-2.0-or-later
"""The TM relay and the TC proxy (platform plan D-PLg, D-PLa).

TM:  QEMU --(SpW hdr + SP, UDP)--> qemu_tm --(bare SP)--> every GS dest
TC:  GS --(bare SP)--> gs_tc --(SpW hdr + SP)--> QEMU's qemu_tc

The relay is transparent: the bytes reach the GS unchanged and in
arrival order. Every TM is one line of tm.jsonl and one `tm` record.
A GS TC while the link is down or STIX is off is dropped and logged
(gs.tc_during_link_down = "drop"), or held and delivered when the link
comes back ("hold", the model's old behaviour): at most gs.hold_max
(default 32, below the FSW's 64-unit queue), each drop beyond it
logged, and the queue is dropped (logged) at power off and at boot.
"""

import json
import os
import socket
import threading

from . import decode as decode_mod
from . import tcbuild

UDP_MAX = 65535 + 8


class Relay:
    def __init__(self, cfg, run_dir, log, clock, link_ok):
        """@link_ok() -> None when TCs may reach QEMU, else the reason."""
        self.cfg = cfg
        self.log = log
        self.clock = clock
        self.link_ok = link_ok
        p = cfg["ports"]
        self.qemu_tc = ("127.0.0.1", p["qemu_tc"])
        self.dests = [(h, int(port)) for h, port in
                      (d.rsplit(":", 1) for d in cfg["gs"]["tm_dests"])]
        self.policy = cfg["gs"]["tc_during_link_down"]
        self.hold_max = cfg["gs"]["hold_max"]
        self.held = []
        self.tm_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.tm_sock.bind(("127.0.0.1", p["qemu_tm"]))
        self.gs_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.gs_sock.bind((cfg["gs"]["bind"], p["gs_tc"]))
        self.out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # a receiver blocked in recvfrom keeps a closed socket's port
        # bound, so the loops wake up to see `stopping`
        for sk in (self.tm_sock, self.gs_sock):
            sk.settimeout(0.2)
        self.tm_path = os.path.join(run_dir, "tm.jsonl")
        self.tm_file = open(self.tm_path, "a", encoding="utf-8")
        self.lock = threading.Lock()
        self.listeners = []         # fn(unit, decoded) per TM
        self.tm_count = 0
        self.stopping = False
        self.threads = []
        for fn, name in ((self._tm_loop, "tm-relay"),
                         (self._tc_loop, "tc-proxy")):
            t = threading.Thread(target=fn, name=name, daemon=True)
            t.start()
            self.threads.append(t)

    # -- TM ---------------------------------------------------------------
    def _tm_loop(self):
        while not self.stopping:
            try:
                data, _ = self.tm_sock.recvfrom(UDP_MAX)
            except socket.timeout:
                continue
            except OSError:
                break
            if len(data) < 4:
                continue
            self.handle_tm(data)

    def handle_tm(self, unit):
        sp = unit[4:]
        for d in self.dests:
            try:
                self.out.sendto(sp, d)
            except OSError:
                pass            # a GS that is not listening is fine
        dec = decode_mod.decode(unit)
        with self.lock:
            self.tm_count += 1
            rec = self.log.emit("tm", "tm", "tm.0x%03x" % (dec["apid"] if dec
                                                          else 0),
                                {"hex": sp.hex(), "decoded": dec,
                                 "text": decode_mod.describe(dec)},
                                sim_est=True)
            self.tm_file.write(json.dumps(
                {"seq": rec.seq, "sim_time": rec.obj["sim_time"],
                 "boot_id": rec.obj["boot_id"], "hex": sp.hex()},
                separators=(",", ":")) + "\n")
            self.tm_file.flush()
        for fn in list(self.listeners):
            try:
                fn(unit, dec)
            except Exception as e:
                self.log.emit("ctl", "log", "relay.listener_error",
                              {"error": "%s: %s" % (type(e).__name__, e)})

    # -- TC ---------------------------------------------------------------
    def _tc_loop(self):
        while not self.stopping:
            try:
                data, src = self.gs_sock.recvfrom(UDP_MAX)
            except socket.timeout:
                continue
            except OSError:
                break
            self.gs_tc(data, src)

    def gs_tc(self, sp, src=None, source="tc", path="tc.gs"):
        """A TC from the GS (bare SP): add the SpW header and forward,
        or apply the link-down policy."""
        why = self.link_ok()
        info = _tc_info(sp)
        if why is not None:
            if self.policy == "hold":
                with self.lock:
                    room = len(self.held) < self.hold_max
                    if room:
                        self.held.append((sp, source, path))
                if room:
                    self.log.emit(source, "tc", path, dict(
                        info, delivered=False, held=True, why=why))
                else:
                    self.log.emit(source, "tc", path, dict(
                        info, delivered=False, dropped=True,
                        why="%s, and the hold queue is full (%d)"
                        % (why, self.hold_max)))
            else:
                self.log.emit(source, "tc", path, dict(
                    info, delivered=False, dropped=True, why=why))
            return False
        self.send_unit(tcbuild.unit(sp))
        self.log.emit(source, "tc", path, dict(info, delivered=True))
        return True

    def release_held(self):
        with self.lock:
            held, self.held = self.held, []
        for sp, source, path in held:
            self.gs_tc(sp, source=source, path=path)

    def drop_held(self, why):
        """At power off and boot: held TCs belong to a gone boot."""
        with self.lock:
            held, self.held = self.held, []
        if held:
            self.log.emit("ctl", "tc", "tc.gs.held", {
                "dropped": len(held), "why": why,
                "seqs": [(_tc_info(sp).get("seq")) for sp, _, _ in held]})

    def send_unit(self, unit):
        self.out.sendto(unit, self.qemu_tc)

    def close(self):
        self.stopping = True
        for t in self.threads:
            t.join(timeout=2)
        for s in (self.tm_sock, self.gs_sock, self.out):
            try:
                s.close()
            except OSError:
                pass
        self.tm_file.close()


def _tc_info(sp):
    p = tcbuild.parse_tc(sp)
    info = {"hex": sp.hex(), "len": len(sp)}
    if p is not None:
        apid, seq, svc, sub, src, ack, ok = p
        info.update(apid=apid, seq=seq, service=svc, subtype=sub,
                    source=src, ack=ack, crc_ok=ok)
    return info
