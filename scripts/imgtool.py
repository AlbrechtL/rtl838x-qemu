#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Inspect and unpack OpenWrt realtek (rtl838x) firmware images.

The realtek images are legacy U-Boot uImages whose 32-bit magic has been
replaced by a vendor value (0x83800000 for the Zyxel GS1900 series, see
"openwrt,ih-magic" in the device tree).  The uImage payload is not the kernel
itself but OpenWrt's "rt-loader", with the LZMA-compressed kernel appended to
it.  The kernel in turn carries its device tree appended at the end
(CONFIG_MIPS_RAW_APPENDED_DTB=y).

The vendor (Zyxel) firmware is the same header around a gzip'ed Linux 2.6.19
kernel with its root filesystem inside, which the stock bootloader inflates.
Where an image is expected, the zip Zyxel distributes it in will do as well.

Teltonika's RutOS for the TSW2xx is an OpenWrt sysupgrade image: a uImage
with the standard magic around an LZMA kernel (device tree appended, no
rt-loader), then the squashfs root filesystem, then a signature trailer.

ALLNET's firmware for the ALL-SG8208M, from the ODM behind the GS1900, is the
Zyxel image again with 0x00000006 for a magic.

Netgear's firmware for the GS108Tv3, GS110TPv3 and GS110TPP is a uImage with
its own magic, "NGE ", around an LZMA kernel with its root file system
inside; Realtek's SDK again, on Linux 3.18.

HPE's Comware for the 1920 series is no uImage at all: a table of segments,
BootWare's two halves and the application, each behind a 0x154-byte header,
the application a 7-Zip archive around one LZMA-compressed raw image for
0x80100000.  The machine boots the file as it is, given to -kernel.

    info           dump the uImage header, locate rt-loader and the LZMA blob
    extract-kernel decompress the kernel (for the loader-bypass boot path)
    dtb            extract the appended device tree blob
    dts            pretty-print the appended device tree
    unzip          write out the firmware file inside a vendor's zip
    mkflash        build a 16 MiB flash image with the firmware installed and
                   the two U-Boot environments the vendor firmware reads; for
                   ALLNET's, the same in its layout; for Netgear's, in its
                   32 MiB one; for HPE's
                   firmware, a 32 MiB flash with a MAC address in it
"""

import argparse
import binascii
import lzma
import re
import struct
import io
import sys
import zipfile
import zlib

UIMAGE_MAGIC = 0x27051956
RTL_MAGIC = 0x83800000
NETGEAR_MAGIC = 0x4e474520     # "NGE "
ALLNET_MAGIC = 0x00000006
HDR_LEN = 64

COMP_GZIP = 1
COMP_LZMA = 3

# The GS1900-8's flash, as the stock bootloader and both firmwares lay it out.
FLASH_SIZE = 16 * 1024 * 1024
FLASH_BDINFO = 0x40000      # U-Boot environment ("u-boot-env")
FLASH_SYSINFO = 0x50000     # second environment ("u-boot-env2")
FLASH_ENV_SIZE = 0x10000
FLASH_FIRMWARE = 0x260000   # first image slot
FLASH_FIRMWARE2 = 0x930000  # second image slot, where the vendor layout puts it

# The ALLNET ALL-SG8208M's flash: the GS1900's partitions, all of them 256 KiB
# further up behind a bootloader twice the size.
ALLNET_BDINFO = 0x80000
ALLNET_SYSINFO = 0x90000
ALLNET_FIRMWARE = 0x2a0000

# The Teltonika TSW2xx's flash: the same chip, laid out differently.
TSW_UBOOT_ENV = 0x80000     # "u-boot-env"
TSW_CONFIG = 0x90000        # "config", the manufacturing data
TSW_FIRMWARE = 0xa0000      # "firmware"
TSW_FIRMWARE_END = 0xf70000  # "event-log" follows

# The Netgear GS108Tv3's flash: 32 MiB, the environments further up, and two
# image slots of 0xe80000 from 0x300000.  The partitions between are JFFS2,
# which the firmware formats on first boot.
NETGEAR_FLASH_SIZE = 32 * 1024 * 1024
NETGEAR_BDINFO = 0xe0000
NETGEAR_SYSINFO = 0xf0000
NETGEAR_FIRMWARE = 0x300000
NETGEAR_FIRMWARE_SIZE = 0xe80000

# Enough of an environment for the vendor firmware to come up: it reads both
# and dereferences what it did not find.  The values are this project's, not
# a dump of a real switch.
DEFAULT_BDINFO = {
    "bootcmd": "boota",
    "bootdelay": "1",
    "baudrate": "115200",
    "ethaddr": "02:E0:4C:83:80:01",
    "ipaddr": "192.168.1.1",
    "serverip": "192.168.1.111",
}
DEFAULT_SYSINFO = {
    "bootpartition": "0",
    "bootmsg": "1",
    "resetdefault": "0",
}
# Netgear's firmware reads the same, and its serial number from "SN": "show
# version" and the configuration report it, and the Insight agent sends it.
# The value is this project's.
DEFAULT_NETGEAR_BDINFO = dict(DEFAULT_BDINFO, SN="QEMU000000001")

# Realtek's SDK, which RutOS loads as a module, will not start without a
# hardware profile, and takes its name from the U-Boot environment.  This is
# the one matching the TSW202: an RTL8380M, its eight internal PHYs and two
# SFP cages.  "ethaddr" is added from the manufacturing data's MAC, as on a
# real unit: the machine programs it into the switch the way U-Boot does,
# and the firmware needs its own address and its bridge's to be the same.
DEFAULT_TSW_UBOOT_ENV = {
    "baudrate": "115200",
    "boardmodel": "RTL8380M_INTPHY_2FIB_1G_DEMO",
}

# The manufacturing data in "config", at the offsets the device tree's mnfinfo
# node reads them from.  The product code picks the model: its first six
# characters select the variant of the SFP ports and the PoE controllers in
# the device tree, and without a known one the kernel oopses setting up DSA.
# The MAC is binary, the rest ASCII.  The values are this project's.
TSW_MNFINFO_FIELDS = {"mac": 0x00, "name": 0x10, "serial": 0x30,
                      "batch": 0x40, "hwver": 0x50}
DEFAULT_TSW_MNFINFO = {
    "mac": "02:E0:4C:83:80:01",
    "name": "TSW202000000",
    "serial": "0000000001",
    "batch": "0001",
    "hwver": "0001",
}

# There is no bootloader to install -- the machine does its job -- but the
# vendor firmware looks through the LOADER partition for U-Boot's version
# string and fails "show version" without one.  It reads this placeholder as
# version 0.0.0, built on the date in the brackets.
LOADER_STUB = b"U-Boot 0.0-svn0 (Jan 01 2000 - 00:00:00)\0"
# ALLNET's looks for the Realtek SDK's U-Boot 2011.12 instead, and takes the
# version from the brackets after it: 0.0.0 here.
ALLNET_LOADER_STUB = b"U-Boot 2011.12.(0.0.0) (Jan 01 2000 - 00:00:00)\0"

COMPRESSION = {0: "none", 1: "gzip", 2: "bzip2", 3: "lzma", 4: "lzo", 5: "lz4", 6: "zstd"}

# HPE's Comware: the segment table, and the application's type in it.
CMW_SEG_TABLE = 0x20
CMW_SEG_ENTRY_SIZE = 0x18
CMW_SEG_HEADER_SIZE = 0x154
CMW_SEG_NAMES = {0x05000001: "BootWare (basic)", 0x05000000: "BootWare (extended)",
                 0x04000000: "application"}
CMW_SEG_APP = 0x04000000
SEVENZIP_SIG = b"7z\xbc\xaf\x27\x1c"

# The HPE 1920's flash: 32 MiB, Comware's file system from 0x300000, and in
# the last sector the manufacturing record Comware takes its MAC address
# from.  The record is a CRC-16 (XMODEM) of the 0x3c2 bytes behind it, then
# those bytes, with the MAC 0x68 bytes from the start.  Without a valid one every switch has
# Comware's default address, 00e0-fc00-3620.  The rest of the record is left
# zero: Comware's own defaults, and nothing in it is a dump of a real unit.
HPE_FLASH_SIZE = 32 * 1024 * 1024
HPE_MNFINFO = 0x1ff0000
HPE_MNFINFO_LEN = 0x3c2
HPE_MNFINFO_MAC = 0x68
DEFAULT_HPE_MNFINFO = {"mac": "02:E0:4C:83:80:01"}


class Image:
    def __init__(self, data):
        self.data = data
        (self.magic, self.hcrc, self.time, self.size, self.load, self.ep,
         self.dcrc) = struct.unpack(">7I", data[:28])
        self.os, self.arch, self.type, self.comp = data[28:32]
        self.name = data[32:64].rstrip(b"\0").decode("ascii", "replace")
        if self.magic not in (UIMAGE_MAGIC, RTL_MAGIC, NETGEAR_MAGIC,
                              ALLNET_MAGIC):
            raise ValueError(f"not a uImage: magic 0x{self.magic:08x}")
        # ALLNET's magic is too weak to go by alone.
        if (self.magic == ALLNET_MAGIC and
                zlib.crc32(data[:4] + bytes(4) + data[8:HDR_LEN]) != self.hcrc):
            raise ValueError("not a uImage: magic 0x00000006, bad header CRC")

    @property
    def payload(self):
        return self.data[HDR_LEN:HDR_LEN + self.size]

    def gunzip(self):
        """The inflated payload of a vendor image."""
        return zlib.decompressobj(31).decompress(self.payload)

    def lzma_offset(self):
        """Offset of the LZMA-alone stream appended after rt-loader."""
        if self.comp in (COMP_GZIP, COMP_LZMA):
            return None
        for m in re.finditer(rb"\x5d\x00\x00", self.data):
            off = m.start()
            if off < HDR_LEN:
                continue
            try:
                lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(
                    self.data[off:off + 0x40000])
            except lzma.LZMAError:
                continue
            return off
        return None

    def unlzma(self):
        """The unpacked payload of a Teltonika image."""
        return lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(
            self.payload)

    def kernel(self):
        if self.comp == COMP_GZIP:
            return self.gunzip()
        if self.comp == COMP_LZMA:
            return self.unlzma()
        off = self.lzma_offset()
        if off is None:
            raise ValueError("no LZMA kernel found in payload")
        return lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(
            self.data[off:])

    def dtb(self):
        k = self.kernel()
        best = None
        for m in re.finditer(b"\xd0\x0d\xfe\xed", k):
            off = m.start()
            totalsize, _, _, _, ver = struct.unpack(">5I", k[off + 4:off + 24])
            if ver == 17 and 1024 < totalsize <= len(k) - off:
                best = (off, totalsize)
        if best is None:
            raise ValueError("no appended DTB found")
        off, totalsize = best
        return k[off:off + totalsize]

    def is_netgear(self):
        """Whether this is Netgear's firmware, by its magic."""
        return self.magic == NETGEAR_MAGIC

    def is_allnet(self):
        """Whether this is ALLNET's firmware, by its magic."""
        return self.magic == ALLNET_MAGIC

    def is_tsw(self):
        """Whether this is Teltonika's firmware for the TSW2xx."""
        if self.comp != COMP_LZMA:
            return False
        try:
            return b"teltonika,tsw2" in self.dtb()
        except ValueError:
            return False


def sevenzip_number(buf, pos):
    """7-Zip's variable-length number at buf[pos]; returns (value, next pos)."""
    first, pos = buf[pos], pos + 1
    value = 0
    for i in range(8):
        mask = 0x80 >> i
        if not first & mask:
            return value | ((first & (mask - 1)) << (8 * i)), pos
        value |= buf[pos] << (8 * i)
        pos += 1
    return value, pos


def unpack_7z(arc):
    """The one file of a Comware 7-Zip archive: one LZMA coder, plain header.

    This is as much of the format as Comware's archives use, the same subset
    the machine reads (rtl838x.c), and anything else is refused.
    """
    if arc[:6] != SEVENZIP_SIG:
        raise ValueError("not a 7-Zip archive")
    next_off, next_len = struct.unpack("<QQ", arc[12:28])
    hdr = arc[32 + next_off:32 + next_off + next_len]

    def expect(pos, *ids):
        for i in ids:
            if hdr[pos] != i:
                raise ValueError("unsupported 7-Zip header")
            pos += 1
        return pos

    pos = expect(0, 0x01, 0x04, 0x06)            # header, streams, pack info
    pack_pos, pos = sevenzip_number(hdr, pos)
    count, pos = sevenzip_number(hdr, pos)
    pos = expect(pos, 0x09)                      # sizes
    pack_len, pos = sevenzip_number(hdr, pos)
    if count != 1:
        raise ValueError("unsupported 7-Zip header")
    if hdr[pos] == 0x0a:                         # CRCs, all defined
        pos = expect(pos + 1, 0x01) + 4
    pos = expect(pos, 0x00, 0x07, 0x0b)          # end, unpack info, folder
    folders, pos = sevenzip_number(hdr, pos)
    pos = expect(pos, 0x00)                      # not external
    coders, pos = sevenzip_number(hdr, pos)
    pos = expect(pos, 0x23, 0x03, 0x01, 0x01)    # LZMA, with properties
    nprops, pos = sevenzip_number(hdr, pos)
    if folders != 1 or coders != 1 or nprops != 5:
        raise ValueError("unsupported 7-Zip header")
    props, pos = hdr[pos:pos + 5], pos + 5
    pos = expect(pos, 0x0c)                      # unpacked size
    size, pos = sevenzip_number(hdr, pos)

    alone = props + struct.pack("<Q", size) + arc[32 + pack_pos:32 + pack_pos + pack_len]
    return lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(alone)


class Comware:
    """HPE's Comware firmware: its segment table, and the application."""

    def __init__(self, data):
        self.data = data
        self.segments = []
        count = struct.unpack(">I", data[4:8])[0] if len(data) >= 8 else 0
        if not 0 < count <= 8:
            raise ValueError("not a Comware image")
        for i in range(count):
            off = CMW_SEG_TABLE + i * CMW_SEG_ENTRY_SIZE
            kind, start, size = struct.unpack(">III", data[off:off + 12])
            if start + size > len(data) or size < CMW_SEG_HEADER_SIZE:
                raise ValueError("not a Comware image")
            self.segments.append((kind, start, size))
        app = [s for s in self.segments if s[0] == CMW_SEG_APP]
        if len(app) != 1 or self.archive()[:6] != SEVENZIP_SIG:
            raise ValueError("not a Comware image")

    def archive(self):
        _, start, size = [s for s in self.segments if s[0] == CMW_SEG_APP][0]
        return self.data[start + CMW_SEG_HEADER_SIZE:start + size]

    def kernel(self):
        """The application, unpacked: a raw image for 0x80100000."""
        return unpack_7z(self.archive())

    def is_tsw(self):
        return False

    def is_netgear(self):
        return False

    def is_allnet(self):
        return False


def mkflash_hpe(mnfinfo):
    """An HPE 1920's flash: erased, but for the manufacturing record."""
    flash = bytearray(b"\xff" * HPE_FLASH_SIZE)
    record = bytearray(2 + HPE_MNFINFO_LEN)
    for name, value in mnfinfo.items():
        if name != "mac":
            raise ValueError(f"unknown manufacturing field {name!r}")
        mac = bytes.fromhex(value.replace(":", "").replace("-", ""))
        record[HPE_MNFINFO_MAC:HPE_MNFINFO_MAC + 6] = mac
    record[0:2] = struct.pack(">H", binascii.crc_hqx(bytes(record[2:]), 0))
    flash[HPE_MNFINFO:HPE_MNFINFO + len(record)] = record
    return bytes(flash)


def fdt_to_dts(dtb):
    (_, _, off_st, off_str, _, _, _, _, size_str,
     size_st) = struct.unpack(">10I", dtb[:40])
    strs = dtb[off_str:off_str + size_str]
    out, p, depth = [], off_st, 0
    while p < off_st + size_st:
        tok = struct.unpack(">I", dtb[p:p + 4])[0]
        p += 4
        if tok == 1:  # BEGIN_NODE
            e = dtb.index(b"\0", p)
            name = dtb[p:e].decode()
            p = (e + 4) & ~3
            out.append("\t" * depth + (name or "/") + " {")
            depth += 1
        elif tok == 2:  # END_NODE
            depth -= 1
            out.append("\t" * depth + "};")
        elif tok == 3:  # PROP
            ln, noff = struct.unpack(">II", dtb[p:p + 8])
            p += 8
            val = dtb[p:p + ln]
            p = (p + ln + 3) & ~3
            end = strs.index(b"\0", noff)
            name = strs[noff:end].decode()
            if ln == 0:
                pv = ""
            elif val.endswith(b"\0") and ln > 1 and all(32 <= c < 127 or c == 0 for c in val):
                pv = ' = "' + '", "'.join(val[:-1].decode().split("\0")) + '"'
            elif ln % 4 == 0:
                words = struct.unpack(">%dI" % (ln // 4), val)
                pv = " = <" + " ".join("0x%x" % w for w in words) + ">"
            else:
                pv = " = [" + val.hex() + "]"
            out.append("\t" * depth + name + pv + ";")
        elif tok == 9:  # END
            break
    return "\n".join(out) + "\n"


def firmware(path):
    """The bytes of a firmware file, or of the .bix or .bin in a vendor's zip."""
    data = open(path, "rb").read()
    if zipfile.is_zipfile(io.BytesIO(data)):
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            names = [n for n in z.namelist()
                     if n.lower().endswith((".bix", ".bin"))]
            if len(names) != 1:
                raise ValueError(f"expected one .bix or .bin in {path}, "
                                 f"found {names}")
            data = z.read(names[0])
    return data


def load(path):
    """The image in a file, or the .bix or .bin inside a vendor's zip."""
    data = firmware(path)
    try:
        return Image(data)
    except ValueError:
        try:
            return Comware(data)
        except (ValueError, IndexError, struct.error):
            pass
        raise


def uboot_env(variables):
    """A U-Boot environment sector: CRC32, then NUL-separated name=value."""
    body = b"".join(f"{k}={v}".encode() + b"\0" for k, v in variables.items())
    body = (body + b"\0").ljust(FLASH_ENV_SIZE - 4, b"\0")
    return struct.pack(">I", zlib.crc32(body)) + body


def mkflash_tsw(data, env, mnfinfo):
    """The TSW2xx's layout: environment, manufacturing data, firmware."""
    if len(data) > TSW_FIRMWARE_END - TSW_FIRMWARE:
        raise ValueError("image does not fit the flash")
    flash = bytearray(b"\xff" * FLASH_SIZE)
    flash[TSW_UBOOT_ENV:TSW_UBOOT_ENV + FLASH_ENV_SIZE] = uboot_env(env)
    for name, value in mnfinfo.items():
        if name not in TSW_MNFINFO_FIELDS:
            raise ValueError(f"unknown manufacturing field {name!r}")
        raw = (bytes.fromhex(value.replace(":", "")) if name == "mac"
               else value.encode())
        off = TSW_CONFIG + TSW_MNFINFO_FIELDS[name]
        flash[off:off + len(raw)] = raw
    flash[TSW_FIRMWARE:TSW_FIRMWARE + len(data)] = data
    return bytes(flash)


def mkflash_netgear(data, bdinfo, sysinfo):
    """The GS108Tv3's layout: two environments, the firmware in slot one."""
    if len(data) > NETGEAR_FIRMWARE_SIZE:
        raise ValueError("image does not fit the flash")
    flash = bytearray(b"\xff" * NETGEAR_FLASH_SIZE)
    flash[NETGEAR_BDINFO:NETGEAR_BDINFO + FLASH_ENV_SIZE] = uboot_env(bdinfo)
    flash[NETGEAR_SYSINFO:NETGEAR_SYSINFO + FLASH_ENV_SIZE] = uboot_env(sysinfo)
    flash[NETGEAR_FIRMWARE:NETGEAR_FIRMWARE + len(data)] = data
    return bytes(flash)


def mkflash(data, bdinfo, sysinfo, allnet=False):
    """The GS1900's layout, or with allnet the ALL-SG8208M's."""
    bdinfo_off, sysinfo_off, firmware_off = (
        (ALLNET_BDINFO, ALLNET_SYSINFO, ALLNET_FIRMWARE) if allnet
        else (FLASH_BDINFO, FLASH_SYSINFO, FLASH_FIRMWARE))
    if len(data) > FLASH_SIZE - firmware_off:
        raise ValueError("image does not fit the flash")
    flash = bytearray(b"\xff" * FLASH_SIZE)
    loader = ALLNET_LOADER_STUB if allnet else LOADER_STUB
    flash[0:len(loader)] = loader
    flash[bdinfo_off:bdinfo_off + FLASH_ENV_SIZE] = uboot_env(bdinfo)
    flash[sysinfo_off:sysinfo_off + FLASH_ENV_SIZE] = uboot_env(sysinfo)
    flash[firmware_off:firmware_off + len(data)] = data
    return bytes(flash)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["info", "extract-kernel", "dtb", "dts",
                                        "unzip", "mkflash"])
    ap.add_argument("image")
    ap.add_argument("-o", "--output")
    ap.add_argument("--bdinfo", action="append", default=[], metavar="NAME=VALUE",
                    help="mkflash: set a variable in the U-Boot environment")
    ap.add_argument("--sysinfo", action="append", default=[], metavar="NAME=VALUE",
                    help="mkflash: set a variable in the second environment")
    ap.add_argument("--mnfinfo", action="append", default=[], metavar="NAME=VALUE",
                    help="mkflash, TSW2xx: set a manufacturing field "
                         "(mac, name, serial, batch, hwver); HPE: mac")
    args = ap.parse_args()

    if args.command == "unzip":
        if not args.output:
            ap.error("unzip needs -o")
        data = firmware(args.image)
        open(args.output, "wb").write(data)
        print(f"wrote {len(data)} bytes to {args.output}", file=sys.stderr)
        return

    img = load(args.image)

    if args.command == "mkflash":
        if not args.output:
            ap.error("mkflash needs -o")
        if isinstance(img, Comware):
            mnfinfo = dict(DEFAULT_HPE_MNFINFO)
            mnfinfo.update(v.split("=", 1) for v in args.mnfinfo)
            flash = mkflash_hpe(mnfinfo)
        elif img.is_tsw():
            env, mnfinfo = dict(DEFAULT_TSW_UBOOT_ENV), dict(DEFAULT_TSW_MNFINFO)
            env.update(v.split("=", 1) for v in args.bdinfo)
            mnfinfo.update(v.split("=", 1) for v in args.mnfinfo)
            env.setdefault("ethaddr", mnfinfo["mac"])
            flash = mkflash_tsw(img.data, env, mnfinfo)
        else:
            bdinfo = dict(DEFAULT_NETGEAR_BDINFO if img.is_netgear()
                          else DEFAULT_BDINFO)
            sysinfo = dict(DEFAULT_SYSINFO)
            for env, given in ((bdinfo, args.bdinfo), (sysinfo, args.sysinfo)):
                env.update(v.split("=", 1) for v in given)
            if img.is_netgear():
                flash = mkflash_netgear(img.data, bdinfo, sysinfo)
            else:
                flash = mkflash(img.data, bdinfo, sysinfo, img.is_allnet())
        open(args.output, "wb").write(flash)
        print(f"wrote {len(flash)} bytes to {args.output}", file=sys.stderr)
        return

    if isinstance(img, Comware):
        if args.command == "info":
            print("HPE Comware image")
            for kind, start, size in img.segments:
                name = CMW_SEG_NAMES.get(kind, f"type 0x{kind:08x}")
                print(f"  0x{start:08x} {size:9d} bytes  {name}")
            k = img.kernel()
            print(f"application {len(k)} bytes unpacked, loads at 0x80100000")
            # Every model's strings are in every image; the release is the
            # one a model's name follows.
            m = re.search(rb"(\d+\.\d+\.\d+ Release \d+)\0+(?:[^\0]+\0+)?"
                          rb"[^\0]+ Switch\0", k)
            if m:
                print(f"            Comware {m.group(1).decode()}")
            return
        if args.command != "extract-kernel":
            ap.error(f"{args.command}: a Comware image has no device tree")

    if args.command == "info":
        vendor = {RTL_MAGIC: " (vendor magic)",
                  NETGEAR_MAGIC: " (Netgear)",
                  ALLNET_MAGIC: " (ALLNET)"}.get(img.magic, "")
        print(f"magic       0x{img.magic:08x}{vendor}")
        print(f"name        {img.name}")
        print(f"size        {img.size} bytes")
        print(f"load / ep   0x{img.load:08x} / 0x{img.ep:08x}")
        print(f"compression {COMPRESSION.get(img.comp, img.comp)}")
        off = img.lzma_offset()
        if off is not None:
            print(f"rt-loader   0x{HDR_LEN:04x}..0x{off:04x} ({off - HDR_LEN} bytes)")
            k = img.kernel()
            print(f"lzma kernel 0x{off:04x}.. -> {len(k)} bytes uncompressed")
            try:
                dtb = img.dtb()
                print(f"appended dtb {len(dtb)} bytes")
            except ValueError as e:
                print(f"appended dtb: {e}")
        elif img.comp in (COMP_GZIP, COMP_LZMA):
            k = img.kernel()
            print(f"{COMPRESSION[img.comp]} kernel 0x{HDR_LEN:04x}.. -> "
                  f"{len(k)} bytes uncompressed")
            m = re.search(rb"Linux version [^\n]*", k)
            if m:
                print(f"            {m.group().decode('ascii', 'replace')}")
        return

    blob = {"extract-kernel": "kernel", "dtb": "dtb"}.get(args.command)
    if blob:
        data = getattr(img, blob)()
        if args.output:
            open(args.output, "wb").write(data)
            print(f"wrote {len(data)} bytes to {args.output}", file=sys.stderr)
        else:
            sys.stdout.buffer.write(data)
    else:
        text = fdt_to_dts(img.dtb())
        if args.output:
            open(args.output, "w").write(text)
        else:
            sys.stdout.write(text)


if __name__ == "__main__":
    main()
