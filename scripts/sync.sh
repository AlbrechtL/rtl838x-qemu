#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copy the RTL838x models into the pinned QEMU tree and add the build glue.
# Idempotent, so it is safe to run before every build and after a rebase.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
qemu="$root/qemu"

if [ ! -f "$qemu/hw/mips/meson.build" ]; then
    echo "qemu submodule is not checked out: git submodule update --init" >&2
    exit 1
fi

cp "$root"/src/hw/mips/*.c "$qemu/hw/mips/"
mkdir -p "$qemu/include/hw/mips"
cp "$root"/src/include/hw/mips/*.h "$qemu/include/hw/mips/"

python3 "$root/scripts/add-cpu.py" "$qemu/target/mips/cpu-defs.c.inc"

if ! grep -q '^config RTL838X' "$qemu/hw/mips/Kconfig"; then
    cat >> "$qemu/hw/mips/Kconfig" <<'KCONFIG'

config RTL838X
    bool
    default y
    depends on MIPS && !MIPS64 && TARGET_BIG_ENDIAN
    select SERIAL_MM
    select UNIMP
KCONFIG
    echo "sync: added CONFIG_RTL838X to hw/mips/Kconfig"
fi

if ! grep -q 'CONFIG_RTL838X' "$qemu/hw/mips/meson.build"; then
    sed -i "/CONFIG_MIPS_BOSTON/a\\
mips_ss.add(when: 'CONFIG_RTL838X', if_true: files('rtl838x.c', 'rtl838x_intc.c', 'rtl838x_timer.c', 'rtl838x_socmisc.c', 'rtl838x_gpio.c', 'rtl838x_wdt.c', 'rtl838x_switch.c'))" \
        "$qemu/hw/mips/meson.build"
    echo "sync: added CONFIG_RTL838X to hw/mips/meson.build"
fi
