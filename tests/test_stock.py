#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot the vendor (Zyxel) firmware under the rtl838x machine and check it.

The vendor firmware boots from flash, so the image is installed into a
scratch flash first, the way "./rtl838x.sh mkflash" does it, and the machine
is started without -kernel.  Everything after that goes through the vendor
CLI on the serial console and through the front-panel ports.  Run it through
"./rtl838x.sh test-stock <image.bix>", which supplies the container.
"""

import argparse
import os
import re
import socket
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "scripts"))

import imgtool  # noqa: E402
from test_boot import Console, Wire, arp, check, is_arp_reply  # noqa: E402

# The chip the vendor kernel's flash driver knows the real one as.
FLASH_MODEL = "mx25l12805d"

PROMPT = r"GS1900# "
SWITCH_IP = "192.168.1.1"       # the firmware's factory default
HOST_IP = "192.168.1.2"         # QEMU's user network, as seen from the switch


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def send(con, line):
    """The vendor CLI wants a carriage return, not the newline a shell takes."""
    con.proc.stdin.write((line + "\r").encode())
    con.proc.stdin.flush()


def cli(con, cmd, timeout=60):
    send(con, cmd)
    m = con.expect(PROMPT, timeout)
    return m.string[:m.start()].replace("\r", "")


def http_get(port, timeout=30):
    """The status line and headers of GET / on the forwarded web port."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
                s.sendall(b"GET / HTTP/1.0\r\nHost: %s\r\n\r\n"
                          % SWITCH_IP.encode())
                data = b""
                while len(data) < 4096:
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                if data:
                    return data.decode("latin1")
        except OSError:
            pass
        if time.monotonic() > deadline:
            return ""
        time.sleep(2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--qemu", default="qemu-system-mips")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    image = imgtool.load(args.image)
    flash = tempfile.NamedTemporaryFile(prefix="rtl838x-flash-", suffix=".bin")
    flash.write(imgtool.mkflash(image.data, imgtool.DEFAULT_BDINFO,
                                imgtool.DEFAULT_SYSINFO))
    flash.flush()

    # lan1 and lan2 are cables this test holds the other end of, lan3 is
    # QEMU's user network, renumbered into the switch's factory subnet and
    # forwarding a local port to its web server.  lan4 to lan8 stay empty.
    wires = [Wire(), Wire()]
    web = free_port()
    argv = [args.qemu, "-M", "rtl838x,flash-model=%s" % FLASH_MODEL,
            "-m", "128", "-nographic", "-no-reboot",
            "-drive", "if=mtd,format=raw,file=%s" % flash.name]
    for w in wires:
        argv += ["-nic", w.nic_arg]
    argv += ["-nic", "user,model=rtl838x-port,net=192.168.1.0/24,host=%s,"
                     "hostfwd=tcp:127.0.0.1:%d-%s:80" % (HOST_IP, web, SWITCH_IP)]
    print("booting: %s" % " ".join(argv))

    con = Console(argv, args.verbose)
    failures = []
    try:
        for w in wires:
            w.accept()

        tests = []

        # Probing the flash is the first thing to go wrong: the vendor kernel
        # knows the chip by its JEDEC ID alone and gives up on any other.
        con.expect(r"Probe: SPI CS\d Flash Type \S+", args.timeout)
        print("%-40s PASS" % "the vendor kernel finds the flash")
        con.expect(r'"RUNTIME2"', 60)
        print("%-40s PASS" % "the vendor partition map is created")

        # The first boot generates its SSH host keys before the CLI starts.
        con.expect(r"Press any key to continue", args.timeout)
        print("%-40s PASS" % "reaches the vendor CLI")
        send(con, "")
        con.expect(r"Username: ", 60)
        send(con, "admin")
        con.expect(r"Password: ", 60)
        send(con, "1234")
        con.expect(PROMPT, 60)
        print("%-40s PASS" % "logs in with the factory account")

        version = cli(con, "show version")
        tests.append(check("show version names the firmware",
                           re.search(r"Firmware Version +: V\d+\.\d+\(",
                                     version) is not None, version.strip()))

        # The firmware only looks at a port once it has been told the link
        # changed, and then takes a moment to act on it.
        status = ""
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            status = cli(con, "show interfaces 1-8 status")
            if len(re.findall(r"connected +\d+ +a-full +a-1000M", status)) == 3:
                break
            time.sleep(3)
        rows = {int(m.group(1)): m.group(2) for m in
                re.finditer(r"^(\d) +(\S+)", status, re.M)}
        tests.append(check("the three cabled ports are connected",
                           [rows.get(p) for p in (1, 2, 3)] == ["connected"] * 3
                           and status.count("a-1000M") == 3, status.strip()))
        tests.append(check("the uncabled ports are not",
                           all(rows.get(p) == "notconnect" for p in range(4, 9)),
                           status.strip()))

        # From outside, as in test_boot.py.  The firmware keeps the CPU port
        # out of VLAN 1, so the broadcast only reaches it because the switch
        # copies ARP requests there, and the answer's way back is the lookup.
        station_a = b"\x52\x54\x00\xaa\xbb\xcc"
        station_b = b"\x52\x54\x00\xdd\xee\xff"

        wires[0].send(arp(station_a, "192.168.1.100", SWITCH_IP))
        reply = wires[0].recv(is_arp_reply)
        tests.append(check("the switch answers an arp for its address",
                           reply is not None, "no reply on port 1"))

        wires[1].send(arp(station_b, "192.168.1.200", "192.168.1.201"))
        time.sleep(1)
        probe = station_b + station_a + b"\x88\xb5" + b"switchtest"
        wires[0].send(probe)
        is_probe = lambda f: f[12:14] == b"\x88\xb5"
        tests.append(check("port 1 to port 2 is switched in hardware",
                           wires[1].recv(is_probe) is not None,
                           "nothing arrived on port 2"))
        tests.append(check("a frame is not reflected to its source",
                           wires[0].recv(is_probe, timeout=2) is None,
                           "port 1 saw its own frame"))

        ping = cli(con, "ping %s" % HOST_IP, timeout=90)
        tests.append(check("the switch pings a host on port 3",
                           "4 packets received" in ping, ping.strip()))

        page = http_get(web)
        tests.append(check("the web interface answers",
                           page.startswith("HTTP/"), page[:200]))

        # Writing the configuration goes through JFFS2 to the flash.
        saved = cli(con, "save", timeout=120)
        files = cli(con, "show flash")
        tests.append(check("the configuration is saved to flash",
                           "startup-config" in files,
                           (saved + files).strip()))

        tests.append(check("no kernel oops or fatal signals",
                           not re.search(r"Oops|SIGSEGV|Kernel panic",
                                         con.buf + version + status + ping),
                           "see the console output"))

        failures = [t for t in tests if not t]
    except (TimeoutError, RuntimeError) as e:
        print("FAIL: %s" % e)
        failures = [False]
    finally:
        for w in wires:
            w.close()
        con.close()
        flash.close()

    print()
    if failures:
        print("%d check(s) failed" % len(failures))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
