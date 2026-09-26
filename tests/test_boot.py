#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot the firmware image under the rtl838x machine and check it from inside.

Drives the serial console over a pipe, so it needs nothing but the standard
library.  Run it through "./rtl838x.sh test", which supplies the container.
"""

import argparse
import os
import re
import select
import socket
import struct
import subprocess
import sys
import time

PROMPT = "RTLTEST> "
CONSOLE_BANNER = "Please press Enter to activate this console"

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


class Console:
    def __init__(self, argv, verbose=False):
        self.verbose = verbose
        self.buf = ""
        self.proc = subprocess.Popen(
            argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, bufsize=0)

    def expect(self, pattern, timeout):
        """Read until pattern (a regex) appears; returns the match object."""
        deadline = time.monotonic() + timeout
        rx = re.compile(pattern)
        while True:
            m = rx.search(self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return m
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(
                    "timed out after %ds waiting for %r\n--- last output ---\n%s"
                    % (timeout, pattern, self.buf[-2000:]))
            if self.proc.poll() is not None:
                raise RuntimeError("emulator exited with %d\n--- last output ---\n%s"
                                   % (self.proc.returncode, self.buf[-2000:]))
            if select.select([self.proc.stdout], [], [], min(remaining, 1.0))[0]:
                chunk = os.read(self.proc.stdout.fileno(), 4096)
                if not chunk:
                    continue
                text = ANSI.sub("", chunk.decode("utf-8", "replace"))
                if self.verbose:
                    sys.stdout.write(text)
                    sys.stdout.flush()
                self.buf += text

    def send(self, line):
        self.proc.stdin.write((line + "\n").encode())
        self.proc.stdin.flush()

    def run(self, cmd, timeout=60):
        """Run a shell command and return its output, echo line removed."""
        self.send(cmd)
        m = self.expect(re.escape(PROMPT), timeout)
        out = m.string[:m.start()]
        newline = out.find("\n")
        return out[newline + 1:] if newline >= 0 else out

    def close(self):
        self.proc.kill()
        self.proc.wait()


class Wire:
    """One end of a QEMU socket netdev, i.e. a cable into a switch port.

    QEMU's socket backend frames on a stream as a four byte big-endian length
    followed by the frame, which is little enough to speak here and lets the
    test stand in for a station plugged into the front of the switch.
    """

    def __init__(self):
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.sock = None
        self.buf = b""

    @property
    def nic_arg(self):
        return "socket,model=rtl838x-port,connect=127.0.0.1:%d" % (
            self.listener.getsockname()[1],)

    def accept(self, timeout=30):
        self.listener.settimeout(timeout)
        self.sock, _ = self.listener.accept()

    def send(self, frame):
        frame += b"\x00" * max(0, 60 - len(frame))
        self.sock.sendall(struct.pack(">I", len(frame)) + frame)

    def recv(self, match, timeout=8):
        """The first frame satisfying match, or None once timeout runs out."""
        deadline = time.monotonic() + timeout
        while True:
            while len(self.buf) >= 4:
                n = struct.unpack(">I", self.buf[:4])[0]
                if len(self.buf) < 4 + n:
                    break
                frame, self.buf = self.buf[4:4 + n], self.buf[4 + n:]
                if match(frame):
                    return frame
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self.sock.settimeout(remaining)
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                return None
            if not data:
                return None
            self.buf += data

    def close(self):
        if self.sock:
            self.sock.close()
        self.listener.close()


def arp(src_mac, src_ip, target_ip, op=1, dst_mac=b"\xff" * 6,
        target_mac=b"\x00" * 6):
    return (dst_mac + src_mac + b"\x08\x06"
            + struct.pack(">HHBBH", 1, 0x0800, 6, 4, op)
            + src_mac + socket.inet_aton(src_ip)
            + target_mac + socket.inet_aton(target_ip))


def is_arp_reply(frame):
    return frame[12:14] == b"\x08\x06" and frame[20:22] == b"\x00\x02"


def check(name, ok, detail=""):
    print("%-40s %s%s" % (name, "PASS" if ok else "FAIL",
                          "" if ok else "  " + detail))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--qemu", default="qemu-system-mips")
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    # Ports claim the -nic options in order, so lan1 and lan2 get a cable this
    # test can drive, lan3..lan7 get a backend that goes nowhere, and lan8
    # deliberately gets nothing: an empty socket has to look like an empty
    # socket from inside the guest.
    wires = [Wire(), Wire()]
    argv = [args.qemu, "-M", "rtl838x", "-m", "128", "-nographic", "-no-reboot",
            "-kernel", args.image]
    for w in wires:
        argv += ["-nic", w.nic_arg]
    for hub in range(3, 8):
        argv += ["-nic", "hubport,hubid=%d,model=rtl838x-port" % hub]
    print("booting: %s" % " ".join(argv))

    con = Console(argv, args.verbose)
    failures = []
    try:
        for w in wires:
            w.accept()
        con.expect(re.escape(CONSOLE_BANNER), args.timeout)
        print("%-40s PASS" % "reaches the console prompt")

        con.send("")
        con.expect(r"[#$] ", 60)
        # Split the marker across two shell strings so that the echo of this
        # very command does not contain it -- otherwise the echo is mistaken
        # for the first prompt and every later reply is off by one command.
        con.send("export PS1='RTL'\"TEST> \"")
        con.expect(re.escape(PROMPT), 60)

        tests = []

        cpu = con.run("grep -m1 'system type' /proc/cpuinfo")
        tests.append(check("SoC is identified as RTL8380M",
                           "RTL8380M" in cpu, cpu.strip()))

        # No -drive if=mtd: the flash is there, erased, and the kernel
        # still finds the chip and the partitions its device tree lists.
        mtd = con.run("cat /proc/mtd")
        tests.append(check("the SPI-NOR flash is detected",
                           '"firmware"' in mtd, mtd.strip()))

        ports = ["lan%d" % i for i in range(1, 9)]

        links = con.run("echo /sys/class/net/*")
        missing = [p for p in ports if not re.search(r"\b%s\b" % p, links)]
        tests.append(check("lan1..lan8 exist", not missing,
                           "missing: %s; got: %s" % (missing, links.split())))

        # netifd takes a while after the console appears to enslave the
        # ports and bring them up, so poll rather than sampling once.
        state = []
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            state = con.run("cat /sys/class/net/lan*/operstate").split()
            if state[:7].count("up") == 7:
                break
            con.run("sleep 5")
        tests.append(check("the seven cabled ports are up",
                           state[:7].count("up") == 7,
                           "operstate: %s" % state))
        tests.append(check("an uncabled port has no carrier",
                           len(state) == 8 and state[7] != "up",
                           "lan8: %s" % state[7:]))

        speed = con.run("cat /sys/class/net/lan*/speed").split()
        tests.append(check("ports negotiated 1 Gbps",
                           speed[:7].count("1000") == 7,
                           "speeds: %s" % speed))

        # The data path, from outside.  A station appears on lan1 and asks
        # the guest for its own address: the frame is broadcast, so it has to
        # be flooded to the CPU port, reach the bridge, and the answer has to
        # come back out of the port it came in on.  That is the whole path,
        # both directions, including the CPU tag in each.
        station_a = b"\x52\x54\x00\xaa\xbb\xcc"
        station_b = b"\x52\x54\x00\xdd\xee\xff"

        wires[0].send(arp(station_a, "192.168.1.100", "192.168.1.1"))
        reply = wires[0].recv(is_arp_reply)
        tests.append(check("the guest answers a broadcast arp",
                           reply is not None, "no reply on lan1"))

        # The same question asked of the guest directly, which this time is a
        # known address rather than a flood: it exercises the lookup.
        if reply:
            wires[0].send(arp(station_a, "192.168.1.100", "192.168.1.1",
                              dst_mac=reply[6:12], target_mac=reply[6:12]))
            tests.append(check("the guest answers a unicast arp",
                               wires[0].recv(is_arp_reply) is not None,
                               "no reply on lan1"))

        # Port to port, with no help from the CPU: teach the switch where the
        # second station is, then send it something from the first.
        wires[1].send(arp(station_b, "192.168.1.200", "192.168.1.201"))
        con.run("sleep 1")
        probe = station_b + station_a + b"\x88\xb5" + b"switchtest"
        wires[0].send(probe)
        is_probe = lambda f: f[12:14] == b"\x88\xb5"
        tests.append(check("lan1 to lan2 is switched in hardware",
                           wires[1].recv(is_probe) is not None,
                           "nothing arrived on lan2"))
        tests.append(check("a frame is not reflected to its source",
                           wires[0].recv(is_probe, timeout=2) is None,
                           "lan1 saw its own frame"))

        # The Otto timer is the only clocksource here; if it stalls, the
        # system looks alive but every timeout in the guest hangs.
        t0 = float(con.run("cut -d' ' -f1 /proc/uptime").strip())
        wall = time.monotonic()
        con.run("sleep 3")
        t1 = float(con.run("cut -d' ' -f1 /proc/uptime").strip())
        elapsed = time.monotonic() - wall
        tests.append(check("guest clock advances", 2.0 < (t1 - t0) < elapsed + 5,
                           "guest %.1fs vs host %.1fs" % (t1 - t0, elapsed)))

        clocksource = con.run(
            "cat /sys/devices/system/clocksource/clocksource0/current_clocksource")
        tests.append(check("otto timer is the clocksource",
                           "otto" in clocksource, clocksource.strip()))

        bad = con.run("dmesg | grep -cE 'Unhandled|BUG:|Oops'")
        tests.append(check("no kernel oops or unhandled faults",
                           bad.strip().startswith("0"), bad.strip()))

        failures = [t for t in tests if not t]
    except (TimeoutError, RuntimeError) as e:
        print("FAIL: %s" % e)
        failures = [False]
    finally:
        for w in wires:
            w.close()
        con.close()

    print()
    if failures:
        print("%d check(s) failed" % len(failures))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
