#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Add an "rtl8380" CPU model to the pinned QEMU tree.

QEMU's 4KEc model has the right PRid, ISA and TLB for this SoC but omits
MIPS16e.  The silicon has it: OpenWrt compiles userspace for this target with
MIPS16 instructions, and those binaries run on real GS1900 hardware -- without
the ASE the guest dies the moment init is executed.

The new entry is derived from whatever the tree's 4KEc entry says, so a rebase
onto a newer QEMU picks up any changes to it automatically.
"""

import re
import sys

path = sys.argv[1]
src = open(path).read()

if '"rtl8380"' in src:
    sys.exit(0)

m = re.search(r'\n    \{\n        \.name = "4KEc",.*?\n    \},\n', src, re.S)
if not m:
    sys.exit('add-cpu: could not find the 4KEc CPU definition in %s' % path)

block = m.group(0)
new = block.replace('.name = "4KEc",', '.name = "rtl8380",')
new = new.replace('.insn_flags = CPU_MIPS32R2,',
                  '.insn_flags = CPU_MIPS32R2 | ASE_MIPS16,')
if new == block:
    sys.exit('add-cpu: the 4KEc definition does not look as expected')

comment = ('\n    /*\n'
           '     * Realtek RTL8380M: a 4KEc core with MIPS16e, which OpenWrt\n'
           '     * userspace for this SoC is compiled to use.\n'
           '     */')

src = src[:m.end()] + comment + new + src[m.end():]
open(path, 'w').write(src)
print("sync: added the rtl8380 CPU model to target/mips/cpu-defs.c.inc")
