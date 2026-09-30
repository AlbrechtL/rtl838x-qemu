# RTL838x under QEMU

Runs OpenWrt for Realtek RTL838x switch SoCs in an emulator. The target is the
Zyxel GS1900-8 image in `images/`, booted unmodified.

[OpenWrt images](https://downloads.openwrt.org/snapshots/targets/realtek/rtl838x/),
[Ethernet Switch OS images](https://albrechtl.github.io/ethernet-switch-os/)
and Zyxel's own firmware for the GS1900-8 all run; see
[Tested firmware](#tested-firmware) for exactly which.

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
* [Vendor firmware](#vendor-firmware)
  * [Why it wants a flash](#why-it-wants-a-flash)
  * [What it took](#what-it-took)
  * [What does not work](#what-does-not-work)
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
| Zyxel V2.90(AAHH.2)C0 ([download](https://download.zyxel.com/GS1900-8/firmware/GS1900-8_2.90(AAHH.2)C0.zip)) | Zyxel GS1900-8 | RTL8380M | In the emulator, by `./rtl838x.sh test-stock`; see [Vendor firmware](#vendor-firmware) |

Whatever an image was built for, the machine identifies itself as an RTL8380M
and has the GS1900-8's eight ports and 16 MiB flash. Images for other RTL838x
boards may work as well, as long as their device tree asks for nothing more.

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
[vendor firmware](#vendor-firmware)), `clean`, `distclean`, `help`.

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
first image slot, and starts it. It does so on every reset, so after an
upgrade the reboot starts the new firmware. With `-kernel`, the kernel is
booted instead, on every reset too, and the flash is just there to be
installed to -- the way a TFTP-booted initramfs installs a firmware on the real
switch:

```sh
qemu-system-mips -M rtl838x -m 128 -nographic -no-reboot \
    -kernel <initramfs image> -drive if=mtd,format=raw,file=flash.bin
# install, then boot what was installed:
qemu-system-mips -M rtl838x -m 128 -nographic \
    -drive if=mtd,format=raw,file=flash.bin
```

`-drive if=mtd,...,snapshot=on` keeps every write in a temporary file instead,
so the flash file is left as it was when QEMU exits, while a reboot inside
QEMU still sees the writes.

Which chip the flash identifies as is the machine's `flash-model` property,
any 16 MiB part QEMU's `m25p80` models: `-machine flash-model=mx25l12805d`
gives the JEDEC ID of the chip that is actually soldered to the board, which
is the only one the vendor firmware accepts.

## Vendor firmware

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

The account is the factory one, `admin` / `1234`, and the address the factory
one too, 192.168.1.1. QEMU's user network can be renumbered to match, which
makes the host 192.168.1.2 from the switch's side and puts the web interface
on <http://localhost:8080> once the console says `Press any key to continue`:

```sh
./rtl838x.sh run-flash flash.bin -machine flash-model=mx25l12805d \
    -nic user,net=192.168.1.0/24,host=192.168.1.2,hostfwd=tcp::8080-192.168.1.1:80
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
| `0x040000` | `BDINFO` | A U-Boot environment. `--bdinfo name=value` adds to it. |
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

None of it changed what OpenWrt sees: `./rtl838x.sh test` passes as before.

### What does not work

* `show mac address-table` lists the switch's own address and nothing else.
  The firmware reads the hardware's L2 table, and this machine keeps what it
  learns in a table of its own; see [Known gaps](#known-gaps). Forwarding is
  not affected.
* The firmware makes up its MAC address from the one the machine programs into
  the switch, which is random per QEMU process, and ignores `ethaddr`.
* Port counters read zero, as they do under OpenWrt.
* The second UART is only there with a second `-serial`; the firmware pokes it
  regardless, which is the only `-d unimp` output a boot produces. The only
  `guest_errors` one is the flash model not knowing command `0xff`, which the
  vendor driver sends once while probing.
* Untested: firmware upgrade from the CLI or the web interface, the second
  image slot, SSH, SNMP, anything involving LAGs, ACLs, IGMP snooping, PoE or
  more than one VLAN, and a reboot from inside the firmware.

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
payload is gzip'ed, which is inflated to its load address first: that is what
the [vendor firmware](#vendor-firmware) is.

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
| SPI-NOR controller | `0x18001200` | 16 MiB MX25L12855E on chip select 0, see [Flash](#flash) |
| UART | `0x18002000` | 16550, reg-shift 2 |
| Watchdog | `0x18003150` | Two phase, resets the machine so `reboot` works |
| GPIO | `0x18003500` | 24 lines |
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
  `4KEc` with MIPS16e. The ASE is missing from QEMU's model but present in
  the silicon, and OpenWrt compiles userspace for this target with MIPS16
  instructions. Without it the kernel boots fine and then dies the instant it
  executes `/init`. Because it's a static patch rather than derived from the
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

`rtl838x_port.c` is one QEMU NIC per front-panel port, and the link state the
guest sees through the PHY is the backend's.

## Known gaps

* Statically programmed FDB entries are not consulted. The driver writes them
  into the hardware L2 table, which this machine stores but does not read back
  for forwarding; only learned addresses are. The `failed to add … to fdb:
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
* The RTL8231 GPIO expander on the bit-banged MDIO bus is absent, so the reset
  button and the system LED do not exist.
* The flash has no memory-mapped window, which only the stock bootloader
  reads through, and the machine loads only the first image slot at
  `0x260000`; `bootpartition` in the U-Boot environment is not consulted.
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
tests/test_stock.py the same for the vendor firmware, from a scratch flash and
                    through the vendor CLI
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
