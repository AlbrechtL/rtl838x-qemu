#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build qemu-system-mips with the RTL838x machine.  Runs inside the builder
# container with the repository bind-mounted at /work.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
build="$root/qemu/build"
prefix="$root/out/qemu"

export CCACHE_DIR="${CCACHE_DIR:-$root/out/ccache}"
mkdir -p "$CCACHE_DIR"

"$root/scripts/sync.sh"

mkdir -p "$build"
cd "$build"

if [ ! -f build.ninja ]; then
    extra=""
    [ "${DEBUG:-0}" = "1" ] && extra="--enable-debug"
    # shellcheck disable=SC2086
    ../configure \
        --target-list=mips-softmmu \
        --prefix="$prefix" \
        --disable-docs --disable-tools --disable-guest-agent \
        --disable-gtk --disable-sdl --disable-vnc --disable-spice \
        --disable-opengl --disable-curses --disable-tpm --disable-libusb \
        --enable-slirp --disable-werror $extra
fi

ninja
ninja install

"$prefix/bin/qemu-system-mips" -M help | grep -q '^rtl838x ' \
    || { echo "build finished but the rtl838x machine is missing" >&2; exit 1; }
echo "built $("$prefix/bin/qemu-system-mips" --version | head -1)"
