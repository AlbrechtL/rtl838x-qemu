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

patch="$root/patches/rtl838x.patch"

# Restore the upstream files the patch touches before applying it.  The patch
# is the only thing that edits them, so nothing is lost -- and without this,
# changing the patch leaves a tree where neither it nor its predecessor
# applies, which looks exactly like upstream drift but is not.
git -C "$qemu" apply --numstat "$patch" | awk '{ print $3 }' | while read -r f; do
    git -C "$qemu" checkout -- "$f" 2>/dev/null || true
done

if git -C "$qemu" apply --check "$patch" 2>/dev/null; then
    git -C "$qemu" apply "$patch"
    echo "sync: applied patches/rtl838x.patch"
else
    echo "sync: patches/rtl838x.patch no longer applies -- likely upstream drift after a qemu rebase, needs regenerating" >&2
    exit 1
fi
