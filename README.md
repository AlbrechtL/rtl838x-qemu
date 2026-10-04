# RTL838x under QEMU

Runs OpenWrt for Realtek RTL838x switch SoCs in an emulator. The target is the
Zyxel GS1900-8 image in `images/`, booted unmodified.

> **Note:** This repository was generated with the help of AI assistants. It
> serves the author's own purpose: testing RTL838x firmwares.

[OpenWrt images](https://downloads.openwrt.org/snapshots/targets/realtek/rtl838x/),
[Ethernet Switch OS images](https://albrechtl.github.io/ethernet-switch-os/),
Zyxel's own firmware for the GS1900-8, Teltonika's RutOS for the TSW2xx,
HPE's Comware for the 1920-8G, Netgear's firmware for the GS108Tv3 and the
GS308T and ALLNET's for the ALL-SG8208M all run;
see [Tested firmware](#tested-firmware) for exactly which.

Current state: the machine boots the stock firmware to an OpenWrt shell, DSA
comes up, and the switch switches. Each of the eight front-panel ports is its
own QEMU network device, frames cross between them and the CPU, and VLAN
membership, spanning tree and BPDU trapping all behave. The SPI-NOR flash is
modelled too, so a firmware installed into it boots, keeps its configuration,
and can be upgraded.

```
$ ./rtl838x.sh test
reaches the console prompt               PASS
SoC is identified as RTL8380M            PASS
the SPI-NOR flash is detected            PASS
lan1..lan8 exist                         PASS
the seven cabled ports are up            PASS
an uncabled port has no carrier          PASS
ports negotiated 1 Gbps                  PASS
the guest answers a broadcast arp        PASS
the guest answers a unicast arp          PASS
lan1 to lan2 is switched in hardware     PASS
a frame is not reflected to its source   PASS
guest clock advances                     PASS
otto timer is the clocksource            PASS
no kernel oops or unhandled faults       PASS
```

## Contents

* [Tested firmware](#tested-firmware)
* [Quick start](#quick-start)
* [Networking](#networking)
* [Flash](#flash)
* [Zyxel firmware](#zyxel-firmware)
  * [Why it wants a flash](#why-it-wants-a-flash)
  * [What it took](#what-it-took)
  * [What does not work](#what-does-not-work)
* [Teltonika firmware](#teltonika-firmware)
  * [What mkflash writes](#what-mkflash-writes)
  * [What RutOS needed](#what-rutos-needed)
  * [What RutOS lacks](#what-rutos-lacks)
* [HPE firmware](#hpe-firmware)
  * [What Comware needed](#what-comware-needed)
  * [What Comware lacks](#what-comware-lacks)
* [Netgear firmware](#netgear-firmware)
  * [What Netgear's firmware needed](#what-netgears-firmware-needed)
  * [What Netgear's firmware lacks](#what-netgears-firmware-lacks)
  * [The GS308T](#the-gs308t)
  * [The GS110TUP](#the-gs110tup)
* [ALLNET firmware](#allnet-firmware)
  * [What ALLNET's firmware needed](#what-allnets-firmware-needed)
  * [What ALLNET's firmware lacks](#what-allnets-firmware-lacks)
* [How the image boots](#how-the-image-boots)
* [What is modelled](#what-is-modelled)
* [How the data path works](#how-the-data-path-works)
* [Known gaps](#known-gaps)
* [Layout](#layout)
* [Debugging](#debugging)
* [License](#license)

## Tested firmware

| Firmware | Built for | SoC | Tested |
|---|---|---|---|
| OpenWrt snapshot, `openwrt-realtek-rtl838x-zyxel_gs1900-8-a1-initramfs-kernel.bin` (the image in `images/`) | Zyxel GS1900-8 (A1) | RTL8380M | In the emulator, by `./rtl838x.sh test` |
| [Ethernet Switch OS](https://albrechtl.github.io/ethernet-switch-os/) | Zyxel GS1900-8 | RTL8380M | In the emulator, and the same image on a real GS1900-8 |
| Ethernet Switch OS | an RTL8382MI board | RTL8382MI | In the emulator |
| Zyxel V2.90(AAHH.2)C0 ([download](https://download.zyxel.com/GS1900-8/firmware/GS1900-8_2.90(AAHH.2)C0.zip)) | Zyxel GS1900-8 | RTL8380M | In the emulator, by `./rtl838x.sh test-stock`; see [Zyxel firmware](#zyxel-firmware) |
| Teltonika RutOS TSW2_R_00.01.10.2 ([download](https://firmware.teltonika-networks.com/1.10.2/TSW2/TSW2_R_00.01.10.2_WEBUI.bin)) | Teltonika TSW2xx, run as a TSW202 | RTL8380M | In the emulator, by `./rtl838x.sh test-stock`; see [Teltonika firmware](#teltonika-firmware) |
| HPE Comware 5.20.99 Release 1121 ([download](https://h30326.www3.hpe.com/hpn/1920-8G-JG920A_5.20.R1121.zip?merchantId=ASP_DROPBOX)) | HPE 1920-8G (JG920A) | RTL8380M | In the emulator, by `./rtl838x.sh test-stock`; see [HPE firmware](#hpe-firmware) |
| Netgear 7.1.1.12 ([download](https://www.downloads.netgear.com/files/GDC/GS108Tv3/GS108Tv3_GS110TPv3_GS110TPPv1_V7.1.1.12.zip)) | Netgear GS108Tv3, GS110TPv3, GS110TPP v1; run as a GS108Tv3 | RTL8380M | In the emulator, by `./rtl838x.sh test-stock`; see [Netgear firmware](#netgear-firmware) |
| Netgear 1.0.4.4 ([download](https://www.downloads.netgear.com/files/GDC/GS308T/GS308T_GS310TP_V1.0.4.4.zip)) | Netgear GS308T, GS310TP; run as a GS308T | RTL8380M | In the emulator, by `./rtl838x.sh test-stock`; see [The GS308T](#the-gs308t) |
| ALLNET 2.2.1.2959 ([download](https://www.allnet.de/ftp-downloads/allnet/switches/all-sg8208m/all-sg8208m-version_2.2.1_vmlinux.bix.zip)) | ALLNET ALL-SG8208M | RTL8380M | In the emulator, by `./rtl838x.sh test-stock`; see [ALLNET firmware](#allnet-firmware) |

Whatever an image was built for, the machine identifies itself as an RTL8380M
and has the GS1900-8's eight ports and 16 MiB flash, except for HPE's
firmware, which makes it a 1920-8G with a 32 MiB flash, and Netgear's, which
makes it a GS108Tv3 or a GS308T, with a 32 MiB flash too. Images for other RTL838x
boards may work as well, as long as their device tree asks for nothing more;
the TSW2xx's two SFP cages are there only as far as its device tree says so,
and are empty.

## Quick start

Everything runs in containers; nothing but Docker, git and a POSIX shell is
needed on the host.

```sh
git submodule update --init      # QEMU, pinned at v11.1.1
./rtl838x.sh build               # ~10 min the first time, seconds after that
./rtl838x.sh run                 # boot the image; leave with Ctrl-A x
./rtl838x.sh test                # automated boot test
```

Other commands: `run-log` (adds `-d unimp,guest_errors` into `out/qemu.log`),
`shell` (a shell in the build container), `info` and `dts` (inspect the
firmware image), `mkflash`, `run-flash` and `test-stock` (for the
[Zyxel](#zyxel-firmware), [Teltonika](#teltonika-firmware),
[HPE](#hpe-firmware), [Netgear](#netgear-firmware) and
[ALLNET](#allnet-firmware) firmware),
`clean`, `distclean`, `help`.

`run`, `run-log`, `test`, `info` and `dts` take the image as their first
argument, defaulting to the one in `images/`. QEMU options all start with a
dash, so a bare word is unambiguously an image and anything else is passed
through to QEMU:

```sh
./rtl838x.sh run                             # the default image
./rtl838x.sh run ~/builds/other.bin          # somewhere else entirely
./rtl838x.sh run ~/builds/other.bin -s -S    # and wait for gdb
./rtl838x.sh run -d in_asm                   # default image, QEMU options
```

Images outside this directory are bind-mounted into the container
automatically, so they can live anywhere. `IMAGE`, `BUILDER`, `RUNTIME`,
`DOCKER` and `DEBUG` can also be set in the environment.

## Networking

Each front-panel port is a QEMU network device of its own, and the ports claim
`-nic` options in the order they are written: the first becomes `lan1`, the
second `lan2`, and so on. Any backend works.

```sh
qemu-system-mips -M rtl838x -m 128 -nographic -kernel <image> \
    -nic user \                                  # lan1 reaches the internet
    -nic socket,listen=:1234 \                   # lan2 waits for another switch
    -nic tap,ifname=tap0,script=no                # lan3 is a host interface
```

A port that claims nothing has no cable in it: the PHY reports no carrier, the
guest shows `NO-CARRIER` and nothing is forwarded to it. `set_link lan3 off` in
the monitor pulls the cable on a port that does have one, and the guest sees the
link drop and the addresses behind it flushed. With no networking options at all
QEMU supplies its usual default, which `lan1` picks up, so a bare boot comes up
with user networking on the first port; `-nic none` gives a switch with nothing
plugged into anything.

Two instances joined by a `socket` pair make a two-switch topology, which is
what running spanning tree against something other than itself needs:

```sh
./rtl838x.sh run -nic socket,listen=:1234                 # switch A, lan1
./rtl838x.sh run -nic socket,connect=127.0.0.1:1234       # switch B, lan1
```

`./rtl838x.sh run`, `run-log` and `run-flash` put QEMU in a container that
shares the host's network, so ports behave as if QEMU ran on the host itself:
`hostfwd`, `socket` `listen=` and `connect=`, and `-s`/`-gdb` all open or
reach ports on the host, and two instances on the same host find each other at
`127.0.0.1`. `tap` still needs `/dev/net/tun` and the right to use it, which
the container does not have; for that, run `out/qemu/bin/qemu-system-mips`
directly. `test` and `test-stock` keep a network of their own, since they
drive everything from inside the container.

## Flash

The GS1900-8's 16 MiB SPI-NOR is QEMU's `m25p80` model of a Macronix
MX25L12855E: same size, 64 KiB sectors, and a JEDEC ID that the kernel
recognises without SFDP tables (the MX25L12805D's ID is shared with parts that
have them, so a current kernel insists on reading tables QEMU's model of that
chip does not have). It sits behind the controller `spi-realtek-rtl.c` drives,
modelled closely enough for the kernel's `spi-nor` driver and everything on
top of it: the partitions, `mtdsplit`, squashfs and JFFS2, `flashcp`,
`sysupgrade` and SWUpdate.

Its contents come from `-drive if=mtd`, a raw file of exactly 16 MiB. Without
one the flash starts erased and is lost when QEMU exits. An erased flash is all
`0xff`:

```sh
tr '\000' '\377' < /dev/zero | head -c 16M > flash.bin
```

With a flash and no `-kernel`, the machine does what the stock bootloader does
with `bootpartition=0`: it loads the uImage at `0x260000`, the start of the
first image slot, and starts it -- or, with nothing there, the one at
`0xa0000`, where the TSW2xx keeps its firmware, the one at `0x300000`,
where the GS108Tv3 does, or the one at `0x2a0000`, where the ALL-SG8208M
does. It does so on every reset, so
after an upgrade the reboot starts the new firmware. With `-kernel`, the
kernel is booted instead, on every reset too, and the flash is just there to
be installed to -- the way a TFTP-booted initramfs installs a firmware on the
real switch:

```sh
qemu-system-mips -M rtl838x -m 128 -nographic -no-reboot \
    -kernel <initramfs image> -drive if=mtd,format=raw,file=flash.bin
# install, then boot what was installed:
qemu-system-mips -M rtl838x -m 128 -nographic \
    -drive if=mtd,format=raw,file=flash.bin
```

The bootloader also programs the switch's MAC address registers with
`ethaddr` from its environment, and the firmware takes its address from there.
The machine does the same when the flash holds an environment with one, at
`0x40000` as on the GS1900, `0x80000` as on the TSW2xx and the ALL-SG8208M,
or `0xe0000` as on the GS108Tv3; otherwise the address
is a random one, fixed for the life of the QEMU process.

`-drive if=mtd,...,snapshot=on` keeps every write in a temporary file instead,
so the flash file is left as it was when QEMU exits, while a reboot inside
QEMU still sees the writes.

Which chip the flash identifies as is the machine's `flash-model` property,
any 16 MiB part QEMU's `m25p80` models: `-machine flash-model=mx25l12805d`
gives the JEDEC ID of the chip that is actually soldered to the board, which
is the only one the Zyxel and ALLNET firmware accept.

## Zyxel firmware

Zyxel's firmware for the GS1900-8 boots to its CLI and its web interface, and
switches. It is a different animal from OpenWrt -- Linux 2.6.19, no device
tree, and Realtek's SDK in a dozen proprietary kernel modules doing what DSA
does -- so it is a second opinion on every device model here, from a driver
written by the people who made the chip.

The firmware is not in this repository. Tested is V2.90(AAHH.2)C0, from
<https://download.zyxel.com/GS1900-8/firmware/GS1900-8_2.90(AAHH.2)C0.zip>;
the zip can be used as it is, the tools take the `.bix` out of it.

```sh
./rtl838x.sh mkflash 'images/GS1900-8_2.90(AAHH.2)C0.zip' flash.bin
./rtl838x.sh run-flash flash.bin -machine flash-model=mx25l12805d
```

About half a minute later, the first boot having generated its SSH host keys:

```
Probe: SPI CS1 Flash Type MX25L12845E
Creating 7 MTD partitions on "Total SPI FLASH":
0x00000000-0x00040000 : "LOADER"
0x00040000-0x00050000 : "BDINFO"
0x00050000-0x00060000 : "SYSINFO"
0x00060000-0x00160000 : "JFFS2 CFG"
0x00160000-0x00260000 : "JFFS2 LOG"
0x00260000-0x00930000 : "RUNTIME"
0x00930000-0x01000000 : "RUNTIME2"
...
Press any key to continue
Username: admin
Password: ****
GS1900# show version
Boot Version     : V0.0.0.0 | 01/01/2000
Firmware Version : V2.90(AAHH.2) | 05/07/2026
GS1900# show interfaces 1-8 status
Port  Name                 Status      Vlan  Duplex  Speed    Type
1                          connected   1     a-full  a-1000M  Copper
2                          notconnect  1     auto    auto     Copper
...
```

The account is the factory one, `admin` / `1234`. The firmware asks for an
address over DHCP first and falls back to the factory one, 192.168.1.1, so
QEMU's user network can be renumbered to match and lease it that very
address. That makes the host 192.168.1.2 from the switch's side and puts the
web interface on <http://localhost:8080> once the console says
`Press any key to continue`:

```sh
./rtl838x.sh run-flash flash.bin -machine flash-model=mx25l12805d \
    -nic user,net=192.168.1.0/24,host=192.168.1.2,dhcpstart=192.168.1.1,hostfwd=tcp::8080-192.168.1.1:80
```

```
$ ./rtl838x.sh test-stock 'images/GS1900-8_2.90(AAHH.2)C0.zip'
the vendor kernel finds the flash        PASS
the vendor partition map is created      PASS
reaches the vendor CLI                   PASS
logs in with the factory account         PASS
show version names the firmware          PASS
the three cabled ports are connected     PASS
the uncabled ports are not               PASS
the switch answers an arp for its address PASS
port 1 to port 2 is switched in hardware PASS
a frame is not reflected to its source   PASS
the switch pings a host on port 3        PASS
the web interface answers                PASS
the configuration is saved to flash      PASS
no kernel oops or fatal signals          PASS
```

### Why it wants a flash

`./rtl838x.sh run` on the `.bix` loads and starts the kernel, but the firmware
does not get far without a flash it recognises with the right things in it.
`mkflash` writes a 16 MiB image holding:

| Offset | Partition | Content |
|---|---|---|
| `0x000000` | `LOADER` | Erased, but for a placeholder U-Boot version string. There is no bootloader to install, the machine does its job, yet `show version` searches this partition for the string and fails without one. It reads the placeholder as `V0.0.0.0`. |
| `0x040000` | `BDINFO` | A U-Boot environment. `--bdinfo name=value` adds to it. Its `ethaddr`, `02:E0:4C:83:80:01` unless changed, is what the machine programs into the switch and the firmware reports as its MAC address. |
| `0x050000` | `SYSINFO` | A second environment, the firmware's own: `bootpartition`, `bootmsg`, `resetdefault`. `--sysinfo name=value` adds to it. The board module reads it while loading and dereferences a null pointer if the flash did not probe. |
| `0x060000` | `JFFS2 CFG`, `JFFS2 LOG` | Erased. The firmware formats them on first boot and keeps its configuration, keys and log there. |
| `0x260000` | `RUNTIME` | The `.bix`, byte for byte. |

The values in the two environments are this project's, enough for the firmware
to come up, not a dump of a real switch.

### What it took

The `.bix` has the same vendor-magic uImage header as the OpenWrt image, but
around a gzip'ed kernel loaded at `0x80000000`, where OpenWrt's is an
uncompressed loader. The machine inflates it, as the bootloader's `bootm`
would. Past that, everything the vendor firmware needed was something the
hardware does and the Linux driver never notices:

* **The flash has to be the real chip.** The vendor kernel has a table of 25
  JEDEC IDs and no SFDP, and `c2 26 18` is not in it; a current kernel wants
  SFDP tables from anything answering `c2 20 18`, which QEMU's model of that
  chip does not have. Hence `flash-model`.
* **`SMI_PORTn_ADDR` reset to the identity.** The SDK looks up the MDIO
  address of each port's PHY in these registers before every access. At zero
  it probes the PHY at address 0 for all ports and finds none.
* **A PHY reset is a link change.** The SDK resets each PHY at the end of its
  bring-up, and only looks at a port's link after the switch core's link-change
  interrupt. Powering a PHY down now takes the link down, and a reset or a
  restarted negotiation latches a change. The MAC's speed and duplex status
  registers are modelled too, since that is where the SDK reads them.
* **Isolation and flooding have reset values.** Nothing isolated, and the last
  row of the multicast port mask table floods everywhere. The SDK builds its
  own flood mask by copying that row.
* **The CPU port is in no VLAN.** The SDK leaves it out of VLAN 1 on purpose.
  Frames for the switch's own address reach it regardless, and broadcast ARP
  requests because `SPCL_TRAP_ARP_CTRL` has them copied there.
* **The descriptor's last eight bytes are the driver's.** The receive tag is
  six words, not the ten the Linux driver declares. The SDK keeps two pointers
  behind it and follows them from its interrupt handler; overwriting them was
  a kernel panic on the first received frame.
* **Short frames carry no FCS space.** The SDK adds four bytes to a frame's
  length only if the frame is long enough for a wire already. An ARP request
  is queued as 42 bytes, all of them payload, and the hardware pads it.
* **Transmit raises an interrupt**, and the ring position registers read back.
  The SDK frees a transmitted packet from the one after consulting the other.
* **The receive tag names its protocol.** The top byte of the tag's second
  word is 4, as an in-band RTL8380 CPU tag has it after its EtherType. The
  SDK decodes the tag only when it is set; without it, it looks for an
  in-band tag in the frame, finds none, and the frame comes from no port.
  ARP did not mind; spanning tree threw every BPDU away, and a looped
  topology stormed.
* **It switches on the outer VLAN tag.** `VLAN_PORT_FWD` has every port
  classify by the outer tag, whose PVID is the upper half of `PB_VLAN`, and
  `VLAN_PORT_ACCEPT_FRAME_TYPE` and `VLAN_PORT_IGR_FLTR` say what a port
  admits. Read as the inner tag, as OpenWrt uses it, every port stayed in
  VLAN 1 whatever the CLI configured.

None of it changed what OpenWrt sees: `./rtl838x.sh test` passes as before.

### What does not work

* `show mac address-table` lists the switch's own address and nothing else.
  The firmware reads the hardware's L2 table, and this machine keeps what it
  learns in a table of its own; see [Known gaps](#known-gaps). Forwarding is
  not affected.
* Port counters read zero, as they do under OpenWrt.
* The second UART is only there with a second `-serial`; the firmware pokes it
  regardless, which is the only `-d unimp` output a boot produces. The only
  `guest_errors` one is the flash model not knowing command `0xff`, which the
  vendor driver sends once while probing.
* Untested: firmware upgrade from the CLI or the web interface, the second
  image slot, SSH, SNMP, anything involving LAGs, ACLs, IGMP snooping or PoE,
  and a reboot from inside the firmware. RSTP and VLANs are exercised by
  [ethernet-switch-os-test](https://github.com/AlbrechtL/ethernet-switch-os-test)'s
  suites, which run against this firmware too.

## Teltonika firmware

Teltonika's TSW2xx switches are the same RTL8380M with the same eight copper
ports, plus two SFP cages. Their firmware, RutOS, is OpenWrt underneath --
Linux 5.10, an OpenWrt DSA driver from before 2023 -- with Realtek's SDK
loaded on top of it as kernel modules. So it drives the hardware two ways at
once, and has opinions about both.

The firmware is not in this repository. Tested is TSW2_R_00.01.10.2, from
<https://firmware.teltonika-networks.com/1.10.2/TSW2/TSW2_R_00.01.10.2_WEBUI.bin>,
the sysupgrade image as it is. The machine is set up as a TSW202; its ports
are `port1`..`port8`, which are the `-nic`s in order as `lan1`..`lan8` are
under OpenWrt, and `sfp1` and `sfp2`, whose cages are empty.

```sh
./rtl838x.sh mkflash images/TSW2_R_00.01.10.2_WEBUI.bin tsw.bin
./rtl838x.sh run-flash tsw.bin
```

The console is there about ten seconds later; the network takes half a
minute more:

```
  Hardware-profile probe
 (RTL8380M_INTPHY_2FIB_1G_DEMO)
...
Please press Enter to activate this console.

TSW202 login: root
Password: *******
root@TSW202:~# cat /etc/version
TSW2_R_00.01.10.2
root@TSW202:~# ip -br addr show dev br0.1
br0.1@br0        UP             192.168.1.2/24 10.0.2.15/24 ...
```

The login is `root`, password `admin01`: a real unit's password is
printed on its label and stored, hashed, in its manufacturing data, and RutOS
falls back to `admin01` when there is none. The switch's own address is
192.168.1.2, and it also runs a DHCP client on the same bridge, so QEMU's user
network leases it 10.0.2.15 and needs no renumbering: the web interface is on
<http://localhost:8080> and <https://localhost:8443> once the lease is in.

```sh
./rtl838x.sh run-flash tsw.bin \
    -nic user,hostfwd=tcp::8080-:80,hostfwd=tcp::8443-:443
```

`test-stock` recognises the image and runs `tests/test_tsw.py`:

```
$ ./rtl838x.sh test-stock images/TSW2_R_00.01.10.2_WEBUI.bin
the SDK finds its hardware profile       PASS
reaches the console                      PASS
logs in with the fallback password       PASS
the firmware names its version           PASS
it takes itself for a TSW202             PASS
port1..port8, sfp1, sfp2 exist           PASS
the three cabled ports are up            PASS
the empty ports and cages are not        PASS
ports negotiated 1 Gbps                  PASS
gets a lease from the host on port3      PASS
the switch pings the host                PASS
the switch answers an arp for its address PASS
port1 to port2 is switched in hardware   PASS
a frame is not reflected to its source   PASS
the web interface answers                PASS
the overlay is JFFS2 on flash            PASS
no kernel oops or unhandled faults       PASS
```

### What mkflash writes

The TSW2xx lays its flash out differently from the GS1900, and `mkflash`
tells the two firmwares apart by the device tree inside the image:

| Offset | Partition | Content |
|---|---|---|
| `0x000000` | `u-boot` | Erased. |
| `0x080000` | `u-boot-env` | A U-Boot environment: `boardmodel=RTL8380M_INTPHY_2FIB_1G_DEMO`, the Realtek SDK's name for an RTL8380M with its internal PHYs and two fibre ports, without which the SDK module oopses; and `ethaddr`, the manufacturing MAC. `--bdinfo name=value` adds to it. |
| `0x090000` | `config` | Manufacturing data: MAC (binary), product code `TSW202000000`, serial, batch, hardware version. `--mnfinfo name=value` changes a field. The product code picks among the device tree's per-model variants of the SFP ports and PoE controllers; without one the kernel oopses setting up DSA. |
| `0x0a0000` | `firmware` | The image, byte for byte: kernel, squashfs, and the signature trailer. The first boot formats the JFFS2 overlay behind the root filesystem. |
| `0xf70000` | `event-log` | Erased. |

The values are this project's, not a dump of a real unit; the MAC is
`02:e0:4c:83:80:01`, and the ports count up from it.

### What RutOS needed

* **An LZMA uImage.** The kernel is a plain uImage around an LZMA kernel, no
  rt-loader; the machine unpacks it as U-Boot's `bootm` would, with liblzma.
* **The firmware slot at `0xa0000`**, which the machine looks at when the
  GS1900's is empty.
* **A fourth busy bit.** DSA setup drives an indirect access engine at
  `0xe3e0` that upstream never uses, and spins on bit 15 of `0xe3e4`.
* **DMA addresses are 29 bits.** The driver gives the CPU-port DMA engine
  KSEG1 addresses, `0xa4000000` for `0x04000000`. The top bits are not wired,
  and taken literally they pointed at nothing: not a frame crossed the CPU
  port, and after 160 transmits the ring was full.
* **"Received" is not "ran out".** The RX status word has one of each per
  ring. The model raised both for every frame, which current OpenWrt folds
  together and does not mind; this driver answers "ran out" by tearing the
  ring down and dropping what is in it.
* **Every row of the multicast port mask table floods everywhere out of
  reset.** The SDK points broadcast flooding at row 504, having shifted the
  row number to where the RTL839x keeps it -- OpenWrt did the same until
  [1b7fd84](https://github.com/openwrt/openwrt/commit/1b7fd8464c2b170d0862e0c19fff11fd8dc50611).
  It works on the switch because nothing ever writes row 504.
* **The switch's own address comes from the bootloader.** Hardware learning
  is off on the CPU port, and the SDK's unknown-unicast flood row leaves the
  CPU out, so a frame for the switch only arrives through the static L2 entry
  the driver installs for its conduit's address -- which it read from the MAC
  registers U-Boot filled in with `ethaddr`, and which the bridge uses too.
  Hence the machine taking `ethaddr` from the flash, and the forwarding engine
  consulting static entries (see [Known gaps](#known-gaps)).
* **The board pulls its GPIO lines up.** Inputs read back as 0 before, so
  each bit-banged I2C bus sat through a stuck-low clock on every transfer
  while probing its PoE controllers, TPM and SFP modules -- 80 seconds to the
  console instead of 10 -- and both cages and the reset button read as
  occupied and pressed. It is a property of the board, not the SoC: Zyxel's
  firmware reads board straps from the same lines and does not recognise a
  GS1900 with them all high. So the machine pulls them up only when the
  firmware sits where a TSW2xx keeps it.

None of it changed what OpenWrt or the Zyxel firmware see: both test suites
pass as before.

### What RutOS lacks

* PoE: the three PoE controllers on the bit-banged I2C bus are not there, and
  the kernel says so (`failed to start POE chip`). Neither are the TPM or
  anything else on those buses, and the SFP cages are always empty.
* Port counters read zero, as they do under OpenWrt: there are no MIB
  counters.
* The `u-boot` partition is empty, so the kernel cannot read a bootloader
  version from it, and says so.
* Every broadcast and multicast frame leaves the other ports twice, a fraction
  of a millisecond apart. The switch floods it as RutOS programmed it to,
  and RutOS's DSA tag code then has the bridge flood it again: it marks a
  received frame as forwarded by the hardware only if it is unicast and not
  ARP or RARP. Nothing among the registers RutOS writes keeps the hardware
  from flooding, so this is taken to be the firmware's doing; whether a real
  TSW does the same has not been checked.
* Untested: firmware upgrade (which checks the image's signature), SSH,
  anything beyond the default VLAN, the web interface past its login page,
  and a reboot from inside the firmware.

## HPE firmware

HPE's 1920 series runs Comware 5.20, which is not Linux: H3C's own real-time
kernel, with a Linux 2.6.16 ("BLINUX") hosted as one of its tasks and
Realtek's SDK 2.1 compiled in. The 1920-8G is an RTL8380M with the eight
copper ports, two SFP cages behind the SoC's SerDes, and a 32 MiB flash.

The firmware is not in this repository. Tested is 5.20.99 Release 1121 for
the JG920A, `1920-8G-JG920A_5.20.R1121.zip`, from
<https://h30326.www3.hpe.com/hpn/1920-8G-JG920A_5.20.R1121.zip?merchantId=ASP_DROPBOX>;
the zip can be used as it is.
The other six 1920 models' images of the same release unpack the same way,
but only the 1920-8G's has been booted.

On the switch, BootWare, HPE's bootloader, loads `main.bin` from Comware's
file system on the flash. This machine has no BootWare, so it does with the
firmware file what BootWare would: given to `-kernel`, it is recognised,
its application unpacked from the 7-Zip archive inside it to `0x80100000`
and started, and the machine is built as a 1920-8G. `run-flash` takes the
firmware after the flash for that:

```sh
./rtl838x.sh mkflash images/1920-8G-JG920A_5.20.R1121.zip hpe.bin
./rtl838x.sh run-flash hpe.bin images/1920-8G-JG920A_5.20.R1121.zip \
    -nic user,hostfwd=tcp::8080-:80
```

`mkflash` writes a 32 MiB flash that is erased but for the manufacturing
record in its last sector, which holds the MAC address
(`--mnfinfo mac=...`, `02:E0:4C:83:80:01` unless changed); without it every
switch is `00e0-fc00-3620`. The console offers a login after about two
minutes:

```
Press ENTER to get started.
Login authentication
Username:admin
Password:
<HPE>summary
...
Mac address: 02e0-4c83-8001
Current boot app is: flash:/main.bin
HPE Comware Platform Software
Comware Software, Version 5.20.99, Release 1121
...
HPE 1920-8G Switch
128M    bytes DRAM
32M     bytes Flash Memory
[SubSlot 0] 8GE+2SFP Hardware Version is Ver.A
```

The account is `admin` with no password. The 1920's CLI is cut down to a
handful of commands; `_cmdline-mode on`, with the password
`Jinhua1920unauthorized`, unlocks the rest. The factory settings give VLAN 1
no address, `ipsetup` does:

```
<HPE>ipsetup ip-address 10.0.2.15 24 default-gateway 10.0.2.2
<HPE>ping 10.0.2.2
    Reply from 10.0.2.2: bytes=56 Sequence=1 ttl=255 time=7 ms
```

after which the web interface is on <http://localhost:8080>. The flash comes
erased and Comware does not format it by itself: `format flash:` in the full
CLI, then `save`, and the configuration survives a restart. Comware's own
`reboot` refuses, as `flash:/main.bin` does not exist; `system_reset` in the
QEMU monitor is the power switch.

```
$ ./rtl838x.sh test-stock images/1920-8G-JG920A_5.20.R1121.zip
the machine unpacks the application      PASS
Comware recognises its flash             PASS
logs in with the factory account         PASS
summary names the switch and firmware    PASS
the MAC address comes from the flash     PASS
the full CLI unlocks                     PASS
the three cabled ports are up at 1 Gbps  PASS
the empty ports and SFP cages are down   PASS
the switch pings a host on port 3        PASS
port 1 to port 2 is switched in hardware PASS
a frame is not reflected to its source   PASS
the web interface answers                PASS
the configuration is saved to flash      PASS
it survives a power cycle                PASS
no SDK failures or dead loops            PASS
```

### What Comware needed

* **A stack.** Its entry stub stores to the stack first thing, and the CPU
  came out of reset with `sp` at zero. The machine now leaves one 1 MiB below
  the top of RAM, as a bootloader would.
* **The flash it knows, at 32 MiB.** A table of 24 JEDEC IDs, and a chip it
  does not find parks the boot in a `j .` with no output. It keeps records
  at the very end of a 32 MiB chip, so it is an MX25L25635E, which the
  machine picks itself for HPE's firmware.
* **The flash's memory-mapped window.** Comware reads the flash through the
  SPI controller's window at `0xb4000000`, 32 MiB of it, which the machine
  now has; each load becomes a read on the SPI bus, so the window sees what
  the guest wrote.
* **The four-byte-address strap.** Bit 29 of `0xbb00100c` tells Comware to
  address the flash with four bytes.
  Without it the upper 16 MiB wrapped onto the lower and the file system
  could not be written.
* **A SoC revision.** Bits 5:1 of `0xbb000ff0` at zero mean a first-cut chip
  whose second timer counts at `0x310c`. Comware then used Otto timer 0's
  interrupt register as its cycle counter, which never moves, and printed
  `ERROR:divide must not be zero!` some 150,000 times during boot while
  computing CPU usage.
* **The SerDes strap.** `INT_MODE_CTRL` says what is behind ports 24 and 26;
  the SDK counts them as ports only when it says fibre, and complained about
  spanning tree on both SFP ports for every instance until it did.
* **Tagged frames from the CPU lose their tag at an access port.** Comware
  hands every frame to the switch tagged with its VLAN and a port mask, and
  has the hardware strip the tag where the VLAN has the port untagged.
* **"Learn this source."** Bit 3 of the transmit tag's second word has the
  switch learn the frame's source address on the CPU port. It is how the
  switch finds out where Comware's own address is; nothing came back to it
  before.
* **Per-port VLAN egress filtering.** Comware keeps the CPU port out of every
  VLAN and leaves its egress filter off in `VLAN_PORT_EGR_FLTR`, so flooded
  frames may reach it all the same. The model now honours the register. The
  Zyxel firmware's DHCP client runs before it sets the bit for the CPU port,
  and now gets a lease too, as it would on a real switch.
* **ACL rule moves complete.** Configuring an address moves ACL rules; the
  SDK waits on bit 0 of `0x6160` forever, which Comware's watchdog reports as
  a dead loop in the console task.

OpenWrt, the Zyxel and the Teltonika firmware pass their tests as before.

### What Comware lacks

* No BootWare: the flash cannot boot by itself, `reboot` refuses, firmware
  upgrade has nothing to upgrade, and `summary` reports the Bootrom version
  as `000`.
* Broadcasts from the ports do not reach Comware. It has the switch trap ARP
  and DHCP to the CPU with ACL rules, and the machine models no ACLs. It
  resolves addresses itself, so it reaches a host and is reached back once
  it has spoken first, as `ping` does and as `ipsetup` does with its
  gratuitous ARP; a host that ARPs for it before that gets no answer, and
  DHCP (`ipsetup dhcp` and the automatic configuration at first boot) does
  not get a lease.
* The rest of the manufacturing record is empty: no serial number, and the
  device's other identity Comware fills in with its defaults.
* Untested: the SFP ports with anything in them (the cages are always empty),
  PoE models, SSH, SNMP, anything past the web interface's login page, and
  every 1920 model but the 1920-8G.

## Netgear firmware

Netgear's GS108Tv3 is an RTL8380M with eight copper ports, a 32 MiB flash
and an RTL8231 GPIO expander. Its firmware, shared with the GS110TPv3 and
the GS110TPP v1, is Realtek's reference firmware for managed switches,
as Zyxel's is -- the same CLI, the SDK as kernel modules -- but on Linux 3.18,
with the root file system an initramfs inside the kernel.

The firmware is not in this repository. Tested is 7.1.1.12,
`GS108Tv3_GS110TPv3_GS110TPPv1_V7.1.1.12.zip`, from
<https://www.downloads.netgear.com/files/GDC/GS108Tv3/GS108Tv3_GS110TPv3_GS110TPPv1_V7.1.1.12.zip>;
the zip can be used as it is, the tools take the `.bix` out of it.

```sh
./rtl838x.sh mkflash images/GS108Tv3_GS110TPv3_GS110TPPv1_V7.1.1.12.zip netgear.bin
./rtl838x.sh run-flash netgear.bin \
    -nic user,net=192.168.0.0/24,host=192.168.0.2,dhcpstart=192.168.0.239,hostfwd=tcp::8080-192.168.0.239:80
```

The machine finds the image at `0x300000`, takes the `NGE ` magic for
Netgear's, and builds itself as a GS108Tv3: the 32 MiB flash, the RTL8231,
the board's reset button. `mkflash` writes the flash in that board's layout:

| Offset | Partition | Content |
|---|---|---|
| `0x0000000` | `LOADER` | Erased: there is no bootloader to install, and `show version` reports an empty loader version. |
| `0x00e0000` | `BDINFO` | A U-Boot environment, the GS1900's plus `SN`, the serial number, which the firmware reports and the Insight agent sends: `QEMU000000001` unless changed. `ethaddr`, `02:E0:4C:83:80:01` unless changed, is the switch's MAC address. `--bdinfo name=value` adds to it. |
| `0x00f0000` | `SYSINFO` | The second environment, as on the GS1900. `--sysinfo name=value` adds to it. |
| `0x0100000` | `JFFS2 CFG`, `JFFS2 LOG` | Erased. The firmware formats them on first boot. |
| `0x0300000` | `RUNTIME` | The `.bix`, byte for byte. |
| `0x1180000` | `RUNTIME2` | Erased. |

The kernel runs with `quiet`, so the console stays dark until the firmware
asks for a key, about a minute and a half in:

```
Press any key to continue
Username: admin
Password: ********
Please change your password from the default settings. ...
Enter new password  :*********
Confirm new password:*********
GS108Tv3# show version
Loader Version   :
Loader Date      :
Firmware Version : 7.1.1.12 (7.1.1.12)
Firmware Date    : Aug 08 2025 - 00:18:32

MAC Address      : 02:E0:4C:83:80:01
SN               : QEMU000000001
```

The account is `admin` / `password`, and the first login has to change the
password. The firmware asks for an address over DHCP and falls back to
192.168.0.239; the user network above leases it that very address, so the web
interface is on <http://localhost:8080>.

```
$ ./rtl838x.sh test-stock images/GS108Tv3_GS110TPv3_GS110TPPv1_V7.1.1.12.zip
reaches the vendor CLI                   PASS
logs in, changing the factory password   PASS
show version names the firmware          PASS
MAC and serial come from the flash       PASS
the three cabled ports are up            PASS
the uncabled ports are down              PASS
gets a lease from the host on port 3     PASS
the switch answers an arp for its address PASS
the switch answers a ping on port 1      PASS
port 1 to port 2 is switched in hardware PASS
a frame is not reflected to its source   PASS
the web interface answers                PASS
the new password survives a power cycle  PASS
no SDK failures or kernel oops           PASS
```

### What Netgear's firmware needed

* **Its magic.** The `.bix` is a uImage with `NGE ` for its magic, around an
  LZMA kernel for `0x80000000`; the machine takes it for one.
* **Vectored interrupts.** The kernel installs its interrupt handlers with
  `set_vi_handler()`, which is a `BUG()` on a CPU without `Config3.VInt`, so
  it stopped before the console was up. The `rtl8380` CPU model has it now;
  the real chip must, for the firmware to run on it.
* **The 32 MiB flash, addressed with four bytes.** The kernel lays out the
  partitions from the chip's size, and the second image slot needs the upper
  16 MiB. Without the four-byte strap (bit 29 of `0x100c`, as on the HPE)
  the JFFS2 partitions read back the start of the flash and the first boot
  wrote over the image.
* **The RTL8231, and the board ID on it.** The SDK reads the model from four
  pins of an RTL8231 at MDIO address 31, through the switch's
  `EXT_GPIO_INDRT_ACCESS` engine at `0xa09c`, after setting the expander up
  by bit-banging MDIO on GPIO lines A2 and A3. Pins 1, 0, 2 and 3 are the
  ID's bits 0 to 3: 1 is the GS108Tv3, 3 the GS110TPv3, 15 the GS110TPP. With
  no profile for what it read, `rtcore.ko` refused to load, and nothing of
  the switch came up. The machine has the expander on both paths, its
  inputs high but for the ID.
* **The reset button is released.** It is GPIO line A0, active low; read as
  0, the firmware took it for held and restored its factory defaults and
  rebooted on every boot. The board pulls it up.
* **The receive tag names the VLAN.** Word 3 of the receive CPU tag carries
  the VLAN the switch classified the frame into. The SDK hands a frame to
  Linux only on that VLAN's interface; with 0 there it dropped everything,
  and the switch got no DHCP lease and answered no ARP. OpenWrt does not
  read the word, the other vendor firmwares do not mind it.

OpenWrt, the Zyxel, the Teltonika and the HPE firmware pass their tests as
before.

### What Netgear's firmware lacks

* The Insight cloud agent: `CloudAgent` restarts `xagent`, which exits with
  status 255 straight away, three times a second, logging each time on the
  console. It sends nothing on the network first; why it fails was not
  found. The CLI works through it, with the log lines in between.
* `ping` takes an IP address for a host name and answers `Invalid host name`;
  a host name works (`ping time-a.netgear.com`, with the internet behind the
  user network). Whether a real switch does the same has not been checked.
* Port counters read zero, as they do under OpenWrt.
* Only the GS108Tv3: the GS110TPv3 and the GS110TPP read other board IDs and
  have PoE, and the GS110TPv3 two SFP ports, none of which the machine has.
* Untested: firmware upgrade, the second image slot, `reboot` from the CLI,
  SSH, SNMP, the web interface past its login page, LAGs, VLANs beyond the
  default one, and spanning tree against another switch.

### The GS308T

The firmware for the GS308T and the GS310TP, 1.0.4.4
(`GS308T_GS310TP_V1.0.4.4.zip`, from
<https://www.downloads.netgear.com/files/GDC/GS308T/GS308T_GS310TP_V1.0.4.4.zip>),
is the same Realtek reference firmware, an older build of it, with `NGC5`
for its magic. It boots on the same machine: `mkflash` writes it into the
same 32 MiB layout, and the machine, seeing the magic, builds itself as a
GS308T. The only difference to the GS108Tv3 is the board ID on the RTL8231:
0 is the GS308T, 2 the GS310TP. The prompt is `GS308T#`.

```sh
./rtl838x.sh mkflash images/GS308T_GS310TP_V1.0.4.4.zip gs308t.bin
./rtl838x.sh run-flash gs308t.bin \
    -nic user,net=192.168.0.0/24,host=192.168.0.2,dhcpstart=192.168.0.239,hostfwd=tcp::8080-192.168.0.239:80
```

The account is `admin` / `password` again, but this firmware does not insist
on a new password at the first login; `test-stock` changes it from the CLI
instead (`username admin privilege 15 password <new>`, which asks for the
old one) and passes the same fourteen checks. Only the GS308T is exercised:
the GS310TP has PoE and two SFP ports. Not checked on it: the Insight agent
and `ping` behaviour noted above for 7.1.1.12.

### The GS110TUP

The firmware for the GS110TUP and the GS710TUP, 1.0.5.13
(`GS110TUPv1_GS710TUPv1_V1.0.5.13.zip`, from
<https://www.downloads.netgear.com/files/GDC/GS710TUP/GS110TUPv1_GS710TUPv1_V1.0.5.13.zip>),
has `NGG ` for its magic and reads board ID 14 (GS110TUP) or 13 (GS710TUP).
The machine recognises it and builds itself as a GS110TUP, but the firmware
does not come up: both models have PoE and an SFP port behind an external
PHY, and the machine has neither.

* The SDK's PoE driver talks to a Broadcom BCM59121 PoE controller on the
  second UART, with Broadcom's 12-byte command frames (checksum the sum of
  the first eleven bytes), starting with `0x20`, get system info. Unanswered,
  `ski.ko` refuses to load, and the timer interrupt it left registered then
  panics the kernel. Answering every frame with its own command and a valid
  checksum, from a script on a second `-serial`, is enough to get past it.
* The board's hardware profile puts an RTL8214C quad PHY at MAC 16, behind
  one of the SFP ports. With nothing answering there, the SDK has no driver
  for the port, `rsd_eee_init` gets `0xf030` (unavailable) for it, the SKI
  core fails, and `l2g_igmp` oopses on the L3 lock it never created.

## ALLNET firmware

ALLNET's ALL-SG8208M is an RTL8380M with eight copper ports, a 16 MiB flash
and an RTL8231 GPIO expander. It comes from the ODM behind the GS1900, and so
does its firmware: Zyxel's, down to the Linux 2.6.19 kernel, the CLI and the
web interface, with ALLNET's name on it.

The firmware is not in this repository. Tested is 2.2.1.2959,
`all-sg8208m-version_2.2.1_vmlinux.bix.zip`, from
<https://www.allnet.de/ftp-downloads/allnet/switches/all-sg8208m/all-sg8208m-version_2.2.1_vmlinux.bix.zip>
(download page:
<https://www.allnet.de/nc/en/allnet-brand/support/treiber-firmware/download/112533/>);
the zip can be used as it is, the tools take the `.bix` out of it.

```sh
./rtl838x.sh mkflash images/all-sg8208m-version_2.2.1_vmlinux.bix.zip allnet.bin
./rtl838x.sh run-flash allnet.bin -machine flash-model=mx25l12805d \
    -nic user,net=192.168.1.0/24,host=192.168.1.2,dhcpstart=192.168.1.1,hostfwd=tcp::8080-192.168.1.1:80
```

The machine finds the image at `0x2a0000`, takes it for ALLNET's, and gives
the board its RTL8231. The flash wants the same chip as Zyxel's firmware, hence
`flash-model`. `mkflash` writes the flash in the ALL-SG8208M's layout, which
is the GS1900's with every partition 256 KiB further up behind a bootloader
twice the size:

| Offset | Partition | Content |
|---|---|---|
| `0x000000` | `LOADER` | Erased, but for a placeholder U-Boot version string, `U-Boot 2011.12.(0.0.0)`, which `show version` searches for and fails without. It reads it as `0.0.0`. |
| `0x080000` | `BDINFO` | The GS1900's U-Boot environment. `--bdinfo name=value` adds to it. |
| `0x090000` | `SYSINFO` | The second environment, as on the GS1900. `--sysinfo name=value` adds to it. |
| `0x0a0000` | `JFFS2 CFG`, `JFFS2 LOG` | Erased. The firmware formats them on first boot. |
| `0x2a0000` | `RUNTIME` | The `.bix`, byte for byte. |
| `0x950000` | `RUNTIME2` | Erased. |

About a minute later:

```
Probe: SPI CS1 Flash Type MX25L12845E
Creating 7 MTD partitions on "Total SPI FLASH":
0x00000000-0x00080000 : "LOADER"
...
0x002a0000-0x00950000 : "RUNTIME"
0x00950000-0x01000000 : "RUNTIME2"
...
Got it bingo! just button default reset reboot!
...
Press any key to continue
Username: admin
Password: *****
Switch# show version
Loader Version   : 0.0.0
Loader Date      : Jan 01 2000 - 00:00:00
Firmware Version : 2.2.1.2959
Firmware Date    : May 21 2018 - 14:34:36
Switch# show interfaces GigabitEthernet 1-8 status
Port  Name                 Status      Vlan  Duplex  Speed    Type
gi1                        connected   1     a-full  a-1000M  Copper
gi2                        notconnect  1     auto    auto     Copper
...
```

The account is `admin` / `admin`. Despite the console, nothing is reset or
rebooted: ALLNET's first-boot hook prints the "bingo" line whenever there is no
`startup-config` yet, and only applies its own defaults. The board LED line
says `24G+4SFP Switch` whatever the board. As with Zyxel's, the firmware asks
for an address over DHCP and falls back to 192.168.1.1, so the user network
above leases it that address and the web interface is on
<http://localhost:8080>.

```
$ ./rtl838x.sh test-stock images/all-sg8208m-version_2.2.1_vmlinux.bix.zip
the vendor kernel finds the flash        PASS
the vendor partition map is created      PASS
reaches the vendor CLI                   PASS
logs in with the factory account         PASS
show version names the firmware          PASS
the three cabled ports are connected     PASS
the uncabled ports are not               PASS
the switch answers an arp for its address PASS
port 1 to port 2 is switched in hardware PASS
a frame is not reflected to its source   PASS
the switch pings a host on port 3        PASS
the web interface answers                PASS
the configuration is saved to flash      PASS
no factory reset from the reset button   PASS
no kernel oops or fatal signals          PASS
```

### What ALLNET's firmware needed

* **Its magic.** The `.bix` is the Zyxel image with `0x00000006` for a
  magic, around the same gzip'ed kernel for `0x80000000`. So weak a magic is
  only taken with a header CRC that checks out.
* **Its flash layout.** With the image in the GS1900's slot, the firmware
  found its environments and JFFS2 partitions 256 KiB off, failed to mount
  them, and lost its configuration on every boot.
* **The RTL8231, and the reset button on it.** The board module picks the
  8-port model itself; the SDK then sets up an RTL8231 by bit-banging MDIO on
  GPIO lines A2 and A3 (`Initial SMI driver failed` four times without it)
  and polls the reset button on its pin 3, active low. Read as 0, the
  firmware took it for held, restored its factory defaults and rebooted on
  every boot. The machine has the expander, as for Netgear's firmware, with
  every pin high.

OpenWrt, the Zyxel, the Teltonika, the HPE and the Netgear firmware pass their
tests as before.

### What ALLNET's firmware lacks

What [Zyxel's](#what-does-not-work) lacks: the MAC address table shows only the
switch's own address, and port counters read zero. Untested: firmware upgrade,
the second image slot, `reboot` from the CLI, SSH, SNMP, the web interface
past its front page, LAGs, VLANs beyond the default one, and spanning tree
against another switch.

## How the image boots

`images/…-initramfs-kernel.bin` is not a plain kernel. `./rtl838x.sh info` breaks
it down:

| Offset | Content |
|---|---|
| `0x0000` | U-Boot image header with the magic replaced by `0x83800000`, the value the stock bootloader looks for (the device tree names it as `openwrt,ih-magic`) |
| `0x0040` | `rt-loader`, position independent code that relocates itself, prints the SoC type, and decompresses the kernel |
| `0x515c` | LZMA stream holding a 17 MB kernel with its device tree appended |

The machine parses that header, copies the payload to `0x80100000` and starts
executing, so `rt-loader` runs exactly as it does on the real switch. From
flash it does the same with the uImage at `0x260000`, see [Flash](#flash). ELF
`vmlinux` files and raw kernels are also accepted, and so is a uImage whose
payload is gzip'ed or LZMA-compressed, which is unpacked to its load address
first: that is what the [Zyxel](#zyxel-firmware), the
[Teltonika](#teltonika-firmware), the [Netgear](#netgear-firmware) and the
[ALLNET](#allnet-firmware) firmware are, the last two with magics of their
own, `NGE ` (`NGC5` for the GS308T's) and `0x00000006`.

Because the device tree is appended to the kernel, QEMU never supplies one:
the hardware model has to match what is already inside the image. `./rtl838x.sh dts`
prints it — that file is the specification this machine implements.

## What is modelled

All of it lives in `src/hw/mips/`, copied into the pinned QEMU tree at build
time by `scripts/sync.sh`.

| Device | Address | Notes |
|---|---|---|
| Interrupt controller | `0x18003000` | 32 sources onto 5 outputs, wired to MIPS IP2..IP6 |
| Otto timer | `0x18003100` | Five count-up timers; clocksource *and* clockevent |
| Memory controller | `0x18001000` | Reports 128 MiB to both rt-loader and the kernel |
| SPI-NOR controller | `0x18001200` | 16 MiB MX25L12855E on chip select 0, see [Flash](#flash), or 32 MiB for HPE's and Netgear's firmware; memory-mapped at `0x14000000` |
| UART | `0x18002000` | 16550, reg-shift 2 |
| Watchdog | `0x18003150` | Two phase, resets the machine so `reboot` works |
| GPIO | `0x18003500` | 24 lines; inputs pulled up on the TSW2xx; an RTL8231 expander on two of them on the Netgear boards and the ALL-SG8208M |
| Switch core | `0x1b000000` | SoC ID, PLLs, thermal, table engine, MDIO, 8 PHYs |
| CPU-port DMA | `0x1b009f00` | Two rings each way, 32-byte descriptors, 20-byte CPU tag |
| Forwarding | `0x1b000000` | FDB, VLANs, spanning tree, isolation, flooding, RMA traps |
| Ports | — | Eight `rtl838x-port` NICs, one per front-panel port |

Three details cost real debugging time and are worth knowing before changing
anything:

* **The UART is passed `DEVICE_LITTLE_ENDIAN`**, which looks wrong on a
  big-endian machine. The registers are one byte wide at a four byte stride,
  so a 32-bit read returns the register in the *top* lane — `rt-loader` polls
  the line status register for `0x20000000`, not `0x20`. That placement is how
  QEMU spells it. Get this wrong and the machine boots in complete silence.
* **The CPU model is `rtl8380`**, added by `patches/rtl838x.patch`: QEMU's
  `4KEc` with MIPS16e and vectored interrupts. The ASE is missing from QEMU's
  model but present in the silicon, and OpenWrt compiles userspace for this
  target with MIPS16 instructions. Without it the kernel boots fine and then
  dies the instant it executes `/init`. Vectored interrupts (`Config3.VInt`)
  are what Netgear's kernel installs its interrupt handlers with; without
  them it stops at a `BUG()` before the console is up. Because it's a static patch rather than derived from the
  tree, a future qemu rebase that reshapes the `4KEc` struct will need this
  patch regenerated by hand.
* **Several registers must clear themselves.** Three are busy-waited on with
  no timeout at all, so getting one wrong hangs the boot with no output:
  `0x6168` bit 0 (ACL clear, during DSA probe), `0x3370` bit 26 (L2 flush),
  and the SPI ready bit at `0x18001208` bit 27. The PHY's BMCR
  restart-autonegotiation bit is the same kind of trap in miniature: leave it
  set and every port stays down while everything else looks healthy.

A full boot of OpenWrt produces no `-d unimp,guest_errors` output at all, so
nothing the guest touches is unmapped, and neither does moving traffic through
it.

## How the data path works

The device tree gives the ethernet node no `reg` of its own — the NIC's
registers are a handful of offsets inside the switch window — so one device
models all of it, split across three files by what they do.

`rtl838x_eth.c` is the CPU-port DMA engine. A ring is an array of 32-bit
entries, each the physical address of a 32-byte descriptor with two flag bits
in the low bits the alignment leaves free: ownership, and a wrap marker on the
last entry. The descriptor carries the buffer, its capacity, the length and a
20-byte CPU tag. The tag is not part of the frame: the source port and the
reason a frame reached the CPU live only there.

Two conventions in the driver are silent when they go wrong. Every buffer ends
in four bytes of FCS space, which the driver trims on receive without looking
and counts in the length on transmit — on a DSA frame those four bytes are the
tag trailer, left there deliberately for the hardware to overwrite. And reason
6, "special trap", is the only value that makes the driver clear
`l2_offloaded`; a BPDU that arrives without it is a BPDU the bridge assumes has
already been forwarded, and spanning tree never converges.

`rtl838x_fwd.c` reconstructs what the silicon does between the ports out of
state the driver has already programmed: VLAN membership and untagged-egress
masks from the table engine, spanning tree state from the MSTI table, the port
isolation matrix, the port-based VLAN registers and the flood masks. Only the
forwarding database is kept separately — the hardware layout is an 8192x4 hash
plus a CAM with no valid bit and a hash function nobody has written down, and a
software table forwards identically.

One indirection there is worth knowing about, because reading past it looks
like it works: `L2_FLD_PMSK` does not hold port masks. It holds two nine-bit
row numbers into the multicast port mask table, and the driver's value for them
reads as a perfectly plausible port mask that happens to exclude the CPU port.

VLANs are classified per port by the inner or the outer tag, as
`VLAN_PORT_FWD` says, each with its own PVID in `PB_VLAN` and its own
acceptable frame types; Linux and RutOS use the inner tag, Zyxel's firmware the
outer one, matched against 0x8100 all the same. A frame from a port outside
its VLAN is forwarded, dropped or trapped as `VLAN_PORT_IGR_FLTR` says for that
port. Before any of that the MAC drops runts, shorter than 60 bytes without
FCS, and frames longer than `MAC_MAX_LEN_CTRL` allows. The socket and dgram
backends deliver frames as they are sent, which is how a test station can
send a runt; slirp and tap pad short frames to the minimum first.

`rtl838x_port.c` is one QEMU NIC per front-panel port, and the link state the
guest sees through the PHY is the backend's.

## Known gaps

* Statically programmed FDB entries count for little. The driver writes them
  into the hardware L2 table, which this machine stores, but forwards from
  what it has learned; a static unicast entry is found only for an address
  nothing has been learned for, by a scan, whatever VLAN it was installed in.
  Static multicast entries are not consulted. The `failed to add … to fdb:
  -524` message during boot is the driver's own, and unrelated.
* Only spanning tree instance 0 is modelled, which covers STP and RSTP but not
  MSTP: every VLAN follows the common instance.
* Multicast follows broadcast. The per-group port masks that IGMP snooping
  drives are stored but not consulted.
* There are no MIB counters, so `ethtool -S` and the per-port byte and packet
  counts in `/sys/class/net/lanN/statistics` all read zero however much traffic
  has crossed the port.
* Frames are forwarded at once and in order, with no queues, no shaping and no
  rate limiting, so anything measuring bandwidth or priority measures the host.
* The RTL8231 GPIO expander on the bit-banged MDIO bus is there only on the
  Netgear boards and the ALL-SG8208M, with its pins as inputs; elsewhere it
  is absent, and with it the reset button and the system LED.
* The machine loads only the first image slot, at `0x260000`, `0xa0000`,
  `0x300000` or `0x2a0000`; `bootpartition` in the U-Boot environment is not consulted. HPE's
  BootWare is not emulated at all.
* There are no ACLs: the rules are stored, but nothing matches against them,
  so nothing is trapped, redirected or dropped by one.
* Migration saves the register window, the PHYs and the ring cursors, but not
  the table engine's contents or the forwarding database, so a restored machine
  forgets what it had learned.

## Layout

```
rtl838x.sh          build, run and test; everything goes through this
src/hw/mips/        device models and the board
src/include/hw/mips/rtl838x.h
scripts/sync.sh     copies the models into qemu/ and applies patches/rtl838x.patch
patches/rtl838x.patch  Kconfig, meson.build and CPU model changes to upstream files
scripts/build.sh    configure + ninja, runs inside the build container
scripts/imgtool.py  inspect/unpack the firmware image, build a flash image
docker/             build container and slim runtime container
tests/test_boot.py  boots the image and checks it over the serial console,
                    including the data path, by plugging a socket netdev into
                    lan1 and lan2 and speaking Ethernet at them
tests/test_stock.py the same for the Zyxel and ALLNET firmware, from a
                    scratch flash and through the vendor CLI
tests/test_tsw.py   the same for the Teltonika firmware, which test_stock.py
                    hands it
tests/test_hpe.py   the same for HPE's firmware, which test_stock.py hands it
tests/test_netgear.py the same for Netgear's firmware (GS108Tv3 and GS308T),
                    which test_stock.py
                    hands it
qemu/               submodule, pinned to v11.1.1
```

`scripts/sync.sh` is idempotent and re-runs on every build, so rebasing onto a
newer QEMU is `git -C qemu checkout <tag>` followed by `./rtl838x.sh build`. The
only edits it makes to upstream files come from applying `patches/rtl838x.patch`
(one Kconfig stanza, one meson line and one CPU definition), and it restores
those files from the submodule's HEAD first, so editing the patch — adding a
file to the meson line, say — does not leave a tree where neither the new patch
nor the old one applies. If a qemu bump changes any of the three patched files
for real, the patch fails to apply and `sync.sh` exits asking for it to be
regenerated.

## Debugging

```sh
./rtl838x.sh run-log            # unimplemented registers and guest errors
./rtl838x.sh run -d in_asm      # instruction trace, for hangs before the console
./rtl838x.sh run -s -S          # wait for gdb on :1234
```

Set `RTL838X_MDIO_DEBUG` to 1 in `src/hw/mips/rtl838x_switch.c` to trace every
MDIO transaction; that is how the PHY behaviour was brought up.
`RTL838X_ETH_DEBUG` in `rtl838x_eth.c` traces every frame crossing the CPU port
and `RTL838X_FWD_DEBUG` in `rtl838x_fwd.c` traces every forwarding decision,
including the drops and the register values behind them, which is how to find
out why a frame did not arrive.

## License

GPL-2.0-or-later, matching QEMU's `hw/mips/` (this project's device models are
written to be copied into that tree and are meant to be upstreamable, so they
are bound to that license regardless; everything else follows for
consistency). See `LICENSE`.
