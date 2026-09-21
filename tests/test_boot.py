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

    argv = [args.qemu, "-M", "rtl838x", "-m", "128", "-nographic", "-no-reboot",
            "-kernel", args.image]
    print("booting: %s" % " ".join(argv))

    con = Console(argv, args.verbose)
    failures = []
    try:
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

        ports = ["lan%d" % i for i in range(1, 9)]

        links = con.run("echo /sys/class/net/*")
        missing = [p for p in ports if not re.search(r"\b%s\b" % p, links)]
        tests.append(check("lan1..lan8 exist", not missing,
                           "missing: %s; got: %s" % (missing, links.split())))

        # netifd takes a while after the console appears to enslave the
        # ports and bring them up, so poll rather than sampling once.
        state = ""
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            state = con.run("cat /sys/class/net/lan*/operstate")
            if state.split().count("up") == 8:
                break
            con.run("sleep 5")
        tests.append(check("all eight ports are up",
                           state.split().count("up") == 8,
                           "operstate: %s" % state.split()))

        speed = con.run("cat /sys/class/net/lan*/speed")
        tests.append(check("ports negotiated 1 Gbps",
                           speed.split().count("1000") == 8,
                           "speeds: %s" % speed.split()))

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
        con.close()

    print()
    if failures:
        print("%d check(s) failed" % len(failures))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
