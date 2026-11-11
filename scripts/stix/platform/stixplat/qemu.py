# SPDX-License-Identifier: GPL-2.0-or-later
"""QEMU: the argv builder, the binary guard, the `-S` spawn and the
stderr (trace) and UART readers (platform plan D-PLf, D-PLg, D-PLl).
"""

import json
import os
import re
import subprocess
import threading
import time

STIXSPW = "/machine/stixspw"
STIXCTRL = "/machine/stixctrl"
GUARD_MSG = "QEMU predates 21f898484f; it would write a 16.5 GB flash"


class BinaryError(Exception):
    pass


def reset_args(policy):
    """D-PLf: the guest-reset policy as QEMU actions."""
    if policy == "warm":
        return ["-action", "reboot=reset"]
    if policy == "cold":
        return ["-no-reboot"]
    if policy == "halt":
        return ["-action", "reboot=shutdown,shutdown=pause"]
    raise ValueError("on_guest_reset %r" % policy)


def build_argv(cfg, run_dir, binary=None):
    """The QEMU command line. Always frozen (-S): power on is not boot."""
    q = cfg["qemu"]
    p = cfg["ports"]
    spw = "grlib-stixspw."
    argv = [binary or q["binary"],
            "-M", "leon3_generic", "-m", q["memory"], "-S",
            "-display", "none", "-monitor", "none",
            # append=on: QEMU's file chardev truncates otherwise
            # (char-file.c), and every boot's UART must survive
            "-chardev", "file,id=stixuart,path=%s,append=on"
            % os.path.join(run_dir, "uart.raw"),
            "-serial", "chardev:stixuart",
            "-qmp", "unix:%s,server=on,wait=off"
            % os.path.join(run_dir, "qmp.sock"),
            "-global", "grlib-stixflash.flash-file=%s"
            % os.path.join(run_dir, "flash.bin"),
            "-global", "grlib-stixflash.inverted-storage=on",
            "-global", spw + "tmtc-server=off",
            "-global", spw + "udp-tm-host=127.0.0.1",
            "-global", spw + "udp-tm-port=%d" % p["qemu_tm"],
            "-global", spw + "udp-tc-port=%d" % p["qemu_tc"],
            "-global", spw + "udp-spw-header=on",
            "-global", spw + "timecode-source=%s" % q["timecode_source"],
            "-global", spw + "obt-epoch=%d" % q["obt_epoch"]]
    argv += reset_args(q["on_guest_reset"])
    for t in q["traces"]:
        argv += ["-trace", t]
    argv += ["-kernel", q["elf"]]
    argv += list(q["extra_args"])
    return argv


# -- the binary guard (D-PLl) ---------------------------------------------
_guard_cache = {}


def probe_binary(binary, timeout=10.0):
    """Ask the binary, without instantiating the machine (-M none), if
    grlib-stixflash has `inverted-storage`. Returns (ok, detail); the
    result is cached per (path, size, mtime)."""
    real = os.path.realpath(binary)
    try:
        st = os.stat(real)
    except OSError as e:
        raise BinaryError("QEMU binary %s: %s" % (binary, e))
    key = (real, st.st_size, st.st_mtime_ns)
    if key in _guard_cache:
        return _guard_cache[key] + (True,)
    cmds = [{"execute": "qmp_capabilities"},
            {"execute": "qom-list-properties",
             "arguments": {"typename": "grlib-stixflash"}},
            {"execute": "quit"}]
    inp = "".join(json.dumps(c) + "\n" for c in cmds)
    try:
        r = subprocess.run([real, "-M", "none", "-S", "-display", "none",
                            "-monitor", "none", "-qmp", "stdio"],
                           input=inp.encode(), capture_output=True,
                           timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise BinaryError("QEMU binary probe %s: %s" % (binary, e))
    props = None
    for line in r.stdout.decode(errors="replace").splitlines():
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        ret = msg.get("return")
        if isinstance(ret, list):
            props = [x.get("name") for x in ret if isinstance(x, dict)]
    if props is None:
        res = (False, "no qom-list-properties answer for grlib-stixflash")
    elif "inverted-storage" in props:
        res = (True, "grlib-stixflash has inverted-storage")
    else:
        res = (False, GUARD_MSG)
    _guard_cache[key] = res
    return res + (False,)


def check_binary(binary, run_dir):
    """Refuse an old binary (always: also with an existing flash.bin it
    would read the inverted file as plain storage)."""
    ok, detail, cached = probe_binary(binary)
    if not ok:
        raise BinaryError("%s: %s" % (binary, detail))
    return detail, cached


# -- the process ----------------------------------------------------------
TRACE_RE = re.compile(r"^(?:\d+@\d+\.\d+:)?(stix\w+) (.*)$")


class QemuProcess:
    """One QEMU process: spawned frozen; stderr (traces) and UART are
    read by threads and handed to @on_line(kind, text)."""

    def __init__(self, argv, run_dir, on_line):
        self.argv = argv
        self.run_dir = run_dir
        self.on_line = on_line
        self.proc = None
        self.threads = []
        self.stop_evt = threading.Event()
        self.qemu_log = None

    def spawn(self):
        self.qemu_log = open(os.path.join(self.run_dir, "qemu.log"), "ab")
        uart = os.path.join(self.run_dir, "uart.raw")
        self.uart_start = os.path.getsize(uart) if os.path.exists(uart) \
            else 0
        self.proc = subprocess.Popen(self.argv, cwd=self.run_dir,
                                     stdin=subprocess.DEVNULL,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
        for fn, name in ((self._stderr_loop, "qemu-stderr"),
                         (self._uart_loop, "qemu-uart")):
            t = threading.Thread(target=fn, name=name, daemon=True)
            t.start()
            self.threads.append(t)
        return self.proc.pid

    def _stderr_loop(self):
        for raw in self.proc.stdout:
            self.qemu_log.write(raw)
            self.qemu_log.flush()
            line = raw.decode(errors="replace").rstrip("\r\n")
            m = TRACE_RE.match(line)
            if m:
                self.on_line("trace", line)
            elif line:
                self.on_line("stderr", line)

    def _uart_loop(self):
        """Tail uart.raw (QEMU writes it with -serial file:)."""
        path = os.path.join(self.run_dir, "uart.raw")
        pos = self.uart_start
        buf = b""
        while True:
            alive = self.proc.poll() is None
            try:
                with open(path, "rb") as f:
                    if os.fstat(f.fileno()).st_size < pos:
                        pos = 0       # truncated under us: start over
                        buf = b""
                    f.seek(pos)
                    chunk = f.read()
            except OSError:
                chunk = b""
            if chunk:
                pos += len(chunk)
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    self.on_line("uart", line.decode(errors="replace")
                                 .rstrip("\r"))
            elif not alive or self.stop_evt.is_set():
                break
            else:
                time.sleep(0.05)
        if buf:
            self.on_line("uart", buf.decode(errors="replace").rstrip("\r"))

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def wait(self, timeout):
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def terminate(self):
        if self.alive():
            self.proc.terminate()

    def kill(self):
        if self.alive():
            self.proc.kill()

    def finish(self):
        """After exit: drain the readers and close the log."""
        self.stop_evt.set()
        for t in self.threads:
            t.join(timeout=2)
        if self.qemu_log is not None:
            self.qemu_log.close()
            self.qemu_log = None
