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

    info           dump the uImage header, locate rt-loader and the LZMA blob
    extract-kernel decompress the kernel (for the loader-bypass boot path)
    dtb            extract the appended device tree blob
    dts            pretty-print the appended device tree
    mkflash        build a 16 MiB flash image with the firmware installed and
                   the two U-Boot environments the vendor firmware reads
"""

import argparse
import lzma
import re
import struct
import io
import sys
import zipfile
import zlib

UIMAGE_MAGIC = 0x27051956
RTL_MAGIC = 0x83800000
HDR_LEN = 64

COMP_GZIP = 1

# The GS1900-8's flash, as the stock bootloader and both firmwares lay it out.
FLASH_SIZE = 16 * 1024 * 1024
FLASH_BDINFO = 0x40000      # U-Boot environment ("u-boot-env")
FLASH_SYSINFO = 0x50000     # second environment ("u-boot-env2")
FLASH_ENV_SIZE = 0x10000
FLASH_FIRMWARE = 0x260000   # first image slot
FLASH_FIRMWARE2 = 0x930000  # second image slot, where the vendor layout puts it

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

# There is no bootloader to install -- the machine does its job -- but the
# vendor firmware looks through the LOADER partition for U-Boot's version
# string and fails "show version" without one.  It reads this placeholder as
# version 0.0.0, built on the date in the brackets.
LOADER_STUB = b"U-Boot 0.0-svn0 (Jan 01 2000 - 00:00:00)\0"

COMPRESSION = {0: "none", 1: "gzip", 2: "bzip2", 3: "lzma", 4: "lzo", 5: "lz4", 6: "zstd"}


class Image:
    def __init__(self, data):
        self.data = data
        (self.magic, self.hcrc, self.time, self.size, self.load, self.ep,
         self.dcrc) = struct.unpack(">7I", data[:28])
        self.os, self.arch, self.type, self.comp = data[28:32]
        self.name = data[32:64].rstrip(b"\0").decode("ascii", "replace")
        if self.magic not in (UIMAGE_MAGIC, RTL_MAGIC):
            raise ValueError(f"not a uImage: magic 0x{self.magic:08x}")

    @property
    def payload(self):
        return self.data[HDR_LEN:HDR_LEN + self.size]

    def gunzip(self):
        """The inflated payload of a vendor image."""
        return zlib.decompressobj(31).decompress(self.payload)

    def lzma_offset(self):
        """Offset of the LZMA-alone stream appended after rt-loader."""
        if self.comp == COMP_GZIP:
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

    def kernel(self):
        if self.comp == COMP_GZIP:
            return self.gunzip()
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


def load(path):
    """The image in a file, or the .bix inside a vendor download's zip."""
    data = open(path, "rb").read()
    if zipfile.is_zipfile(io.BytesIO(data)):
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            names = [n for n in z.namelist() if n.lower().endswith(".bix")]
            if len(names) != 1:
                raise ValueError(f"expected one .bix in {path}, found {names}")
            data = z.read(names[0])
    return Image(data)


def uboot_env(variables):
    """A U-Boot environment sector: CRC32, then NUL-separated name=value."""
    body = b"".join(f"{k}={v}".encode() + b"\0" for k, v in variables.items())
    body = (body + b"\0").ljust(FLASH_ENV_SIZE - 4, b"\0")
    return struct.pack(">I", zlib.crc32(body)) + body


def mkflash(data, bdinfo, sysinfo):
    if len(data) > FLASH_SIZE - FLASH_FIRMWARE:
        raise ValueError("image does not fit the flash")
    flash = bytearray(b"\xff" * FLASH_SIZE)
    flash[0:len(LOADER_STUB)] = LOADER_STUB
    flash[FLASH_BDINFO:FLASH_BDINFO + FLASH_ENV_SIZE] = uboot_env(bdinfo)
    flash[FLASH_SYSINFO:FLASH_SYSINFO + FLASH_ENV_SIZE] = uboot_env(sysinfo)
    flash[FLASH_FIRMWARE:FLASH_FIRMWARE + len(data)] = data
    return bytes(flash)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["info", "extract-kernel", "dtb", "dts",
                                        "mkflash"])
    ap.add_argument("image")
    ap.add_argument("-o", "--output")
    ap.add_argument("--bdinfo", action="append", default=[], metavar="NAME=VALUE",
                    help="mkflash: set a variable in the U-Boot environment")
    ap.add_argument("--sysinfo", action="append", default=[], metavar="NAME=VALUE",
                    help="mkflash: set a variable in the second environment")
    args = ap.parse_args()

    img = load(args.image)

    if args.command == "mkflash":
        if not args.output:
            ap.error("mkflash needs -o")
        bdinfo, sysinfo = dict(DEFAULT_BDINFO), dict(DEFAULT_SYSINFO)
        for env, given in ((bdinfo, args.bdinfo), (sysinfo, args.sysinfo)):
            env.update(v.split("=", 1) for v in given)
        open(args.output, "wb").write(mkflash(img.data, bdinfo, sysinfo))
        print(f"wrote {FLASH_SIZE} bytes to {args.output}", file=sys.stderr)
        return

    if args.command == "info":
        vendor = " (vendor magic)" if img.magic == RTL_MAGIC else ""
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
        elif img.comp == COMP_GZIP:
            k = img.gunzip()
            print(f"gzip kernel 0x{HDR_LEN:04x}.. -> {len(k)} bytes uncompressed")
            m = re.search(rb"Linux version [^\n]*", k)
            if m:
                print(f"            {m.group().decode('ascii', 'replace')}")
        return

    blob = {"extract-kernel": img.kernel, "dtb": img.dtb}.get(args.command)
    if blob:
        data = blob()
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
