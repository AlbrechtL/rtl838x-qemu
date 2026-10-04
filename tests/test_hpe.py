#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot HPE's Comware for the 1920 series under the rtl838x machine and check it.

The machine boots the firmware file as BootWare would, given to -kernel, and
takes it for an HPE 1920-8G.  The flash is a scratch one from
"./rtl838x.sh mkflash", erased but for the manufacturing record that carries
the switch's MAC address, which Comware formats and saves its configuration
to.  Run it through "./rtl838x.sh test-stock <image>", which recognises the
image and supplies the container.
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
from test_boot import Console, Wire, arp, check  # noqa: E402
from test_stock import free_port, http_get, send  # noqa: E402

# The factory account has no password.  The second one unlocks the full CLI
# with "_cmdline-mode on"; HPE leaves the 1920 with a handful of commands
# otherwise, not enough to look at ports or the file system.
USER, PASSWORD = "admin", ""
CMDLINE_PASSWORD = "Jinhua1920unauthorized"

PROMPT = r"<HPE>"
# The factory settings leave VLAN 1 without an address; ipsetup gives it the
# one QEMU's user network expects its guest at.
SWITCH_IP = "10.0.2.15"
HOST_IP = "10.0.2.2"            # QEMU's user network, as seen from the switch
MAC = imgtool.DEFAULT_HPE_MNFINFO["mac"]


def cli(con, cmd, timeout=60):
    """Run a command, answering its questions and paging its output."""
    send(con, cmd)
    out = ""
    while True:
        m = con.expect(PROMPT + r"|---- More ----|\[Y/N\]:?|"
                       r"Please input password:|press the enter key\):",
                       timeout)
        out += m.string[:m.start()]
        if m.group() == PROMPT:
            return out.replace("\r", "")
        if m.group() == "---- More ----":
            con.proc.stdin.write(b" ")
            con.proc.stdin.flush()
        elif m.group().startswith("[Y/N]"):
            send(con, "Y")
        elif m.group().startswith("Please input password"):
            send(con, CMDLINE_PASSWORD)
        else:
            send(con, "")


def login(con, timeout):
    con.expect(r"Press ENTER to get started", timeout)
    send(con, "")
    con.expect(r"Username:", 60)
    send(con, USER)
    con.expect(r"Password:", 60)
    send(con, PASSWORD)
    con.expect(PROMPT, 60)


def power_cycle(monitor):
    """What pulling the plug does; Comware's reboot wants main.bin in flash."""
    with socket.create_connection(("127.0.0.1", monitor), timeout=10) as m:
        m.sendall(b"system_reset\n")
        time.sleep(1)


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
    firmware = tempfile.NamedTemporaryFile(prefix="rtl838x-comware-",
                                           suffix=".bin")
    firmware.write(image.data)
    firmware.flush()
    flash = tempfile.NamedTemporaryFile(prefix="rtl838x-flash-", suffix=".bin")
    flash.write(imgtool.mkflash_hpe(imgtool.DEFAULT_HPE_MNFINFO))
    flash.flush()

    # lan1 and lan2 are cables this test holds the other end of, lan3 is
    # QEMU's user network, forwarding a local port to the web server.  lan4
    # to lan8 and both SFP cages stay empty.  No -no-reboot: the test
    # power-cycles the switch through the monitor.
    wires = [Wire(), Wire()]
    web = free_port()
    monitor = free_port()
    argv = [args.qemu, "-M", "rtl838x", "-m", "128", "-nographic",
            "-kernel", firmware.name,
            "-drive", "if=mtd,format=raw,file=%s" % flash.name,
            "-monitor", "tcp:127.0.0.1:%d,server=on,wait=off" % monitor]
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

        con.expect(r"loading Comware application", 60)
        print("%-40s PASS" % "the machine unpacks the application")

        # A flash it does not know by its JEDEC ID, Comware waits on forever,
        # without a word.
        con.expect(r"DRV_SYSM_Flash_bootware_Init", args.timeout)
        print("%-40s PASS" % "Comware recognises its flash")

        login(con, args.timeout)
        print("%-40s PASS" % "logs in with the factory account")

        summary = cli(con, "summary")
        tests.append(check("summary names the switch and firmware",
                           "HPE 1920-8G Switch" in summary and
                           re.search(r"Comware Software, Version \d",
                                     summary) is not None, summary.strip()))
        want = MAC.replace(":", "").lower()
        want = "-".join(want[i:i + 4] for i in (0, 4, 8))
        tests.append(check("the MAC address comes from the flash",
                           "mac address: %s" % want in summary.lower(),
                           summary.strip()))

        unlock = cli(con, "_cmdline-mode on")
        tests.append(check("the full CLI unlocks",
                           "all-command mode" in unlock, unlock.strip()))

        # Comware brings a port up once the PHY says the link is, and the
        # SFP cages are there because the straps say what is behind them.
        brief = ""
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            brief = cli(con, "display interface brief")
            if len(re.findall(r"GE1/0/[123] +UP +1G\(a\)", brief)) == 3:
                break
            time.sleep(3)
        rows = dict(re.findall(r"^GE1/0/(\d+) +(\S+)", brief, re.M))
        tests.append(check("the three cabled ports are up at 1 Gbps",
                           len(re.findall(r"GE1/0/[123] +UP +1G\(a\)",
                                          brief)) == 3, brief.strip()))
        tests.append(check("the empty ports and SFP cages are down",
                           sorted(rows, key=int) == [str(p) for p in
                                                     range(1, 11)] and
                           all(rows[str(p)] == "DOWN" for p in range(4, 11)),
                           brief.strip()))

        cli(con, "ipsetup ip-address %s 24 default-gateway %s"
            % (SWITCH_IP, HOST_IP))

        # The ping goes first: by the time it is answered, the address
        # ipsetup gave is in use.
        ping = cli(con, "ping %s" % HOST_IP, timeout=90)
        tests.append(check("the switch pings a host on port 3",
                           "5 packet(s) received" in ping, ping.strip()))

        # Not checked: an ARP request from a port reaching Comware.  It has
        # the switch trap ARP and DHCP to the CPU with ACL rules, which this
        # machine does not model, so it hears no broadcast from the ports;
        # it resolves addresses itself, as the ping above does.
        station_a = b"\x52\x54\x00\xaa\xbb\xcc"
        station_b = b"\x52\x54\x00\xdd\xee\xff"

        wires[1].send(arp(station_b, "10.0.2.200", "10.0.2.201"))
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

        page = http_get(web, timeout=60)
        tests.append(check("the web interface answers",
                           page.startswith("HTTP/"), page[:200]))

        # The flash comes erased, and Comware does not format it by itself.
        cli(con, "format flash:", timeout=300)
        saved = cli(con, "save", timeout=120)
        files = cli(con, "dir")
        tests.append(check("the configuration is saved to flash",
                           "startup.cfg" in files, (saved + files).strip()))

        power_cycle(monitor)
        login(con, args.timeout)
        summary = cli(con, "summary")
        tests.append(check("it survives a power cycle",
                           re.search(r"IP address: +%s" % re.escape(SWITCH_IP),
                                     summary) is not None, summary.strip()))

        # The console shows what the SDK and Comware's watchdogs complain
        # about: ports the SDK did not find, register bits it waits on.
        complaints = re.findall(r"[^\n]*(?:fail!|dead loop|must not be zero|"
                                r"Oops|Kernel panic)[^\n]*", con.log)
        tests.append(check("no SDK failures or dead loops",
                           not complaints, "\n".join(complaints[:10])))

        failures = [t for t in tests if not t]
    except (TimeoutError, RuntimeError) as e:
        print("FAIL: %s" % e)
        failures = [False]
    finally:
        for w in wires:
            w.close()
        con.close()
        flash.close()
        firmware.close()

    print()
    if failures:
        print("%d check(s) failed" % len(failures))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
