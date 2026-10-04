#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot Netgear's firmware for the GS108Tv3 under the rtl838x machine and check it.

The firmware is installed into a scratch 32 MiB flash in the GS108Tv3's
layout, the way "./rtl838x.sh mkflash" does it, and the machine, seeing
Netgear's magic in the image slot, builds itself as a GS108Tv3.  The
GS308T's firmware, with a magic of its own, makes it a GS308T, and the
GS110TUP's a GS110TUP; the checks are the same.  Everything
after that goes through the vendor CLI on the serial console and through the
front-panel ports.  Run it through "./rtl838x.sh test-stock <image>", which
recognises the image and supplies the container.
"""

import argparse
import os
import re
import socket
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "scripts"))

import imgtool  # noqa: E402
from test_boot import Console, Wire, arp, check, is_arp_reply  # noqa: E402
from test_stock import free_port, http_get, send  # noqa: E402

# The prompt is the model the machine is built as, by the image's magic.
MODELS = {imgtool.NETGEAR_MAGIC: "GS108Tv3",
          imgtool.NETGEAR_MAGIC_GS110TUP: "GS110TUP",
          imgtool.NETGEAR_MAGIC_GS308T: "GS308T"}
PROMPT = r"GS108Tv3# "
# The factory account; the first login has to change its password.
USER, PASSWORD = "admin", "password"
NEW_PASSWORD = "Qemu-1234"

SWITCH_IP = "192.168.0.239"     # the firmware's factory default
HOST_IP = "192.168.0.2"         # QEMU's user network, as seen from the switch
MAC = imgtool.DEFAULT_NETGEAR_BDINFO["ethaddr"]
SN = imgtool.DEFAULT_NETGEAR_BDINFO["SN"]


def cli(con, cmd, timeout=60):
    """Run a command, paging through its output."""
    send(con, cmd)
    out = ""
    while True:
        m = con.expect(PROMPT + r"|--More--", timeout)
        out += m.string[:m.start()]
        if m.group() != "--More--":
            return out.replace("\r", "")
        # The pager throws away what was typed before it asked.
        time.sleep(1)
        con.proc.stdin.write(b" ")
        con.proc.stdin.flush()


def login(con, password, timeout):
    con.expect(r"Press any key to continue", timeout)
    send(con, "")
    con.expect(r"Username: ", 60)
    send(con, USER)
    con.expect(r"Password: ", 60)
    send(con, password)


def power_cycle(monitor):
    with socket.create_connection(("127.0.0.1", monitor), timeout=10) as m:
        m.sendall(b"system_reset\n")
        time.sleep(1)


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack(">%dH" % (len(data) // 2), data))
    s = (s >> 16) + (s & 0xffff)
    s += s >> 16
    return ~s & 0xffff


def icmp_echo(src_mac, dst_mac, src_ip, dst_ip, ident=0x5157):
    icmp = struct.pack(">BBHHH", 8, 0, 0, ident, 1) + b"rtl838x-qemu"
    icmp = icmp[:2] + struct.pack(">H", checksum(icmp)) + icmp[4:]
    ip = struct.pack(">BBHHHBBH4s4s", 0x45, 0, 20 + len(icmp), 1, 0, 64, 1, 0,
                     socket.inet_aton(src_ip), socket.inet_aton(dst_ip))
    ip = ip[:10] + struct.pack(">H", checksum(ip)) + ip[12:]
    return dst_mac + src_mac + b"\x08\x00" + ip + icmp


def is_echo_reply(frame):
    return (frame[12:14] == b"\x08\x00" and frame[23] == 1 and
            frame[34] == 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--qemu", default="qemu-system-mips")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    return run(args)


def send_until(con, line, pattern, tries=5, timeout=20):
    """Type a line until the CLI answers it: what is typed while it is busy
    printing, or before it asks, is thrown away."""
    for _ in range(tries - 1):
        send(con, line)
        try:
            return con.expect(pattern, timeout)
        except TimeoutError:
            pass
    send(con, line)
    return con.expect(pattern, timeout)


def change_password(con):
    """What the first login does where the firmware does not insist."""
    send_until(con, "configure", r"\(config\)# ")
    send_until(con, "username %s privilege 15 password %s"
               % (USER, NEW_PASSWORD), r"Old password: ")
    m = send_until(con, PASSWORD, r"\(config\)# |incorrect")
    if m.group() != "(config)# ":
        raise RuntimeError("the CLI refused the factory password")
    send_until(con, "exit", PROMPT)


def run(args):
    global PROMPT
    image = imgtool.load(args.image)
    PROMPT = r"%s# " % MODELS[image.magic]
    flash = tempfile.NamedTemporaryFile(prefix="rtl838x-flash-", suffix=".bin")
    flash.write(imgtool.mkflash_netgear(image.data,
                                        imgtool.DEFAULT_NETGEAR_BDINFO,
                                        imgtool.DEFAULT_SYSINFO))
    flash.flush()

    # lan1 and lan2 are cables this test holds the other end of, lan3 is
    # QEMU's user network, renumbered into the switch's factory subnet and
    # forwarding a local port to its web server.  The firmware asks for an
    # address over DHCP and falls back to its factory one, so that is the
    # one the user network leases it.  lan4 to lan8 stay empty.  No
    # -no-reboot: the test power-cycles the switch through the monitor.
    wires = [Wire(), Wire()]
    web = free_port()
    monitor = free_port()
    argv = [args.qemu, "-M", "rtl838x", "-m", "128", "-nographic",
            "-drive", "if=mtd,format=raw,file=%s" % flash.name,
            "-monitor", "tcp:127.0.0.1:%d,server=on,wait=off" % monitor]
    for w in wires:
        argv += ["-nic", w.nic_arg]
    argv += ["-nic", "user,model=rtl838x-port,net=192.168.0.0/24,host=%s,"
                     "dhcpstart=%s,hostfwd=tcp:127.0.0.1:%d-%s:80"
             % (HOST_IP, SWITCH_IP, web, SWITCH_IP)]
    print("booting: %s" % " ".join(argv))

    con = Console(argv, args.verbose)
    failures = []
    try:
        for w in wires:
            w.accept()

        tests = []

        # The kernel runs with "quiet": the first word on the console is
        # the firmware's, once Realtek's SDK has found its board.
        login(con, PASSWORD, args.timeout)
        print("%-40s PASS" % "reaches the vendor CLI")
        # 7.1 makes the first login change the password; 1.0 does not.
        if con.expect(PROMPT + r"|Enter new password", 60).group() == \
                "Enter new password":
            send(con, NEW_PASSWORD)
            con.expect(r"Confirm new password", 60)
            send(con, NEW_PASSWORD)
            con.expect(PROMPT, 60)
        else:
            change_password(con)
        print("%-40s PASS" % "logs in, changing the factory password")

        version = cli(con, "show version")
        tests.append(check("show version names the firmware",
                           re.search(r"Firmware Version +: \d+\.\d", version)
                           is not None, version.strip()))
        tests.append(check("MAC and serial come from the flash",
                           re.search(r"MAC Address +: %s" % re.escape(MAC),
                                     version, re.I) is not None and
                           re.search(r"SN +: %s" % SN, version) is not None,
                           version.strip()))

        # The SDK only looks at a port once the switch says its link changed.
        status = ""
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            status = cli(con, "show interfaces GigabitEthernet 1-8")
            rows = dict(re.findall(r"GigabitEthernet(\d) is (\w+)", status))
            if [rows.get(str(p)) for p in (1, 2, 3)] == ["up"] * 3:
                break
            time.sleep(3)
        tests.append(check("the three cabled ports are up",
                           [rows.get(str(p)) for p in (1, 2, 3)] == ["up"] * 3,
                           str(rows)))
        tests.append(check("the uncabled ports are down",
                           all(rows.get(str(p)) == "down" for p in range(4, 9)),
                           str(rows)))

        # The lease names the user network's gateway; the factory settings
        # have 192.168.0.254.
        ip = ""
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            ip = cli(con, "show ip")
            if re.search(r"Status.*Default Gateway: %s" % re.escape(HOST_IP),
                         ip, re.S):
                break
            time.sleep(5)
        tests.append(check("gets a lease from the host on port 3",
                           re.search(r"Status.*IP Address: %s.*Default "
                                     r"Gateway: %s" % (re.escape(SWITCH_IP),
                                                       re.escape(HOST_IP)),
                                     ip, re.S) is not None, ip.strip()))

        # From outside, as in test_boot.py.
        station_a = b"\x52\x54\x00\xaa\xbb\xcc"
        station_b = b"\x52\x54\x00\xdd\xee\xff"

        wires[0].send(arp(station_a, "192.168.0.100", SWITCH_IP))
        reply = wires[0].recv(is_arp_reply)
        tests.append(check("the switch answers an arp for its address",
                           reply is not None, "no reply on port 1"))
        if reply:
            wires[0].send(icmp_echo(station_a, reply[6:12], "192.168.0.100",
                                    SWITCH_IP))
        tests.append(check("the switch answers a ping on port 1",
                           reply is not None and
                           wires[0].recv(is_echo_reply) is not None,
                           "no echo reply on port 1"))

        wires[1].send(arp(station_b, "192.168.0.200", "192.168.0.201"))
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
                           re.match(r"HTTP/1\.\d 200", page) is not None,
                           page[:200]))

        # The new password is in the configuration, which "save" writes to
        # JFFS2 in the flash.  Only a flash that kept it lets it log in.
        cli(con, "save", timeout=120)
        power_cycle(monitor)
        login(con, NEW_PASSWORD, args.timeout)
        m = con.expect(PROMPT + r"|Enter new password|Authentication Failed",
                       60)
        tests.append(check("the new password survives a power cycle",
                           m.group() == PROMPT, m.group()))

        # What the SDK prints when the board is not what it expects.
        complaints = re.findall(r"[^\n]*(?:RTK_INIT_FAILURE|Oops|"
                                r"Kernel panic|Segmentation fault|"
                                r"Restore Factory Default)[^\n]*", con.log)
        tests.append(check("no SDK failures or kernel oops",
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

    print()
    if failures:
        print("%d check(s) failed" % len(failures))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
