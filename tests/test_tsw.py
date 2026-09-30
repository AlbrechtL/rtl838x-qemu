#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot Teltonika's RutOS for the TSW2xx under the rtl838x machine and check it.

The firmware boots from flash, so the image is installed into a scratch
flash first, the way "./rtl838x.sh mkflash" does it -- with the U-Boot
environment and manufacturing data a real unit carries -- and the machine is
started without -kernel.  Run it through "./rtl838x.sh test-stock <image>",
which recognises the image and supplies the container.
"""

import argparse
import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "scripts"))

import imgtool  # noqa: E402
from test_boot import PROMPT, Console, Wire, arp, check, is_arp_reply  # noqa: E402
from test_stock import free_port, http_get  # noqa: E402

# With no password hash in the manufacturing data, RutOS falls back to this
# one for root and admin alike.
USER, PASSWORD = "root", "admin01"

SWITCH_IP = "192.168.1.2"       # the firmware's factory default, static
GUEST_DHCP_IP = "10.0.2.15"     # what QEMU's user network leases it
HOST_IP = "10.0.2.2"            # QEMU's user network, as seen from the switch


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--qemu", default="qemu-system-mips")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    return run(args)


def run(args):
    image = imgtool.load(args.image)
    env = dict(imgtool.DEFAULT_TSW_UBOOT_ENV)
    mnfinfo = dict(imgtool.DEFAULT_TSW_MNFINFO)
    env["ethaddr"] = mnfinfo["mac"]
    flash = tempfile.NamedTemporaryFile(prefix="rtl838x-flash-", suffix=".bin")
    flash.write(imgtool.mkflash_tsw(image.data, env, mnfinfo))
    flash.flush()

    # port1 and port2 are cables this test holds the other end of, port3 is
    # QEMU's user network, which leases the switch an address and forwards
    # a local port to its web server.  port4 to port8 stay empty.
    wires = [Wire(), Wire()]
    web = free_port()
    argv = [args.qemu, "-M", "rtl838x", "-m", "128", "-nographic",
            "-no-reboot", "-drive", "if=mtd,format=raw,file=%s" % flash.name]
    for w in wires:
        argv += ["-nic", w.nic_arg]
    argv += ["-nic", "user,model=rtl838x-port,hostfwd=tcp:127.0.0.1:%d-:80"
             % web]
    print("booting: %s" % " ".join(argv))

    con = Console(argv, args.verbose)
    failures = []
    try:
        for w in wires:
            w.accept()

        tests = []

        # Realtek's SDK, loaded as a module, is the first thing to go wrong:
        # it oopses without a hardware profile named in the environment.
        con.expect(r"Hardware-profile probe[^(]{0,80}"
                   r"\(RTL8380M_INTPHY_2FIB_1G_DEMO\)", args.timeout)
        print("%-40s PASS" % "the SDK finds its hardware profile")

        con.expect(r"Please press Enter to activate this console",
                   args.timeout)
        print("%-40s PASS" % "reaches the console")
        deadline = time.monotonic() + 60
        while True:
            con.send("")
            try:
                con.expect(r"login: ", 5)
                break
            except TimeoutError:
                if time.monotonic() > deadline:
                    raise
        con.send(USER)
        con.expect(r"Password: ", 30)
        con.send(PASSWORD)
        con.expect(r"[#$] ", 60)
        print("%-40s PASS" % "logs in with the fallback password")
        con.send("export PS1='RTL'\"TEST> \"")
        con.expect(re.escape(PROMPT), 60)
        con.run("dmesg -n 1")

        version = con.run("cat /etc/version")
        tests.append(check("the firmware names its version",
                           version.strip().startswith("TSW2_R_"),
                           version.strip()))

        name = con.run("mnf_info --name")
        tests.append(check("it takes itself for a TSW202",
                           name.strip().startswith("TSW202"), name.strip()))

        ports = ["port%d" % i for i in range(1, 9)] + ["sfp1", "sfp2"]
        links = con.run("echo /sys/class/net/*")
        missing = [p for p in ports if not re.search(r"\b%s\b" % p, links)]
        tests.append(check("port1..port8, sfp1, sfp2 exist", not missing,
                           "missing: %s" % missing))

        state = []
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            state = con.run(
                "cat /sys/class/net/port*/operstate /sys/class/net/sfp*/operstate"
            ).split()
            if state[:3] == ["up"] * 3:
                break
            con.run("sleep 5")
        tests.append(check("the three cabled ports are up",
                           state[:3] == ["up"] * 3, "operstate: %s" % state))
        tests.append(check("the empty ports and cages are not",
                           len(state) == 10 and "up" not in state[3:],
                           "operstate: %s" % state))

        speed = con.run("cat /sys/class/net/port[123]/speed").split()
        tests.append(check("ports negotiated 1 Gbps",
                           speed == ["1000"] * 3, "speeds: %s" % speed))

        # QEMU's user network hands out an address on port3, which takes the
        # whole data path: broadcast out and in, then unicast back to the
        # switch's own address, which only arrives through the static entry
        # the driver installed for it.
        addrs = ""
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            addrs = con.run("ip -4 -br addr show dev br0.1")
            if GUEST_DHCP_IP in addrs:
                break
            con.run("sleep 5")
        tests.append(check("gets a lease from the host on port3",
                           GUEST_DHCP_IP in addrs, addrs.strip()))

        ping = con.run("ping -c 3 %s" % HOST_IP, timeout=60)
        tests.append(check("the switch pings the host",
                           "3 packets received" in ping, ping.strip()))

        station_a = b"\x52\x54\x00\xaa\xbb\xcc"
        station_b = b"\x52\x54\x00\xdd\xee\xff"

        wires[0].send(arp(station_a, "192.168.1.100", SWITCH_IP))
        reply = wires[0].recv(is_arp_reply)
        tests.append(check("the switch answers an arp for its address",
                           reply is not None, "no reply on port1"))

        wires[1].send(arp(station_b, "192.168.1.200", "192.168.1.201"))
        con.run("sleep 1")
        probe = station_b + station_a + b"\x88\xb5" + b"switchtest"
        wires[0].send(probe)
        is_probe = lambda f: f[12:14] == b"\x88\xb5"
        tests.append(check("port1 to port2 is switched in hardware",
                           wires[1].recv(is_probe) is not None,
                           "nothing arrived on port2"))
        tests.append(check("a frame is not reflected to its source",
                           wires[0].recv(is_probe, timeout=2) is None,
                           "port1 saw its own frame"))

        page = http_get(web, timeout=60)
        tests.append(check("the web interface answers",
                           re.match(r"HTTP/1\.[01] 200", page) is not None,
                           page[:200]))

        # The configuration lives in the flash, in JFFS2 behind the rootfs,
        # which the first boot formats while running from a tmpfs overlay.
        mounts = ""
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            mounts = con.run("grep overlay /proc/mounts")
            if re.search(r"/dev/mtdblock\d+ /overlay jffs2", mounts):
                break
            con.run("sleep 5")
        tests.append(check("the overlay is JFFS2 on flash",
                           re.search(r"/dev/mtdblock\d+ /overlay jffs2",
                                     mounts) is not None, mounts.strip()))

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
        flash.close()

    print()
    if failures:
        print("%d check(s) failed" % len(failures))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
