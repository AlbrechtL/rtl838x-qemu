#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build and run the RTL838x QEMU machine.  Everything happens in containers,
# so Docker, git and a POSIX shell are all that is needed on the host.
#
#   ./rtl838x.sh help
#
set -eu

# Where the caller was, so that image paths on the command line resolve the
# way they look, and where the repository is, which is what gets mounted.
invocation_dir=$PWD
root=$(cd "$(dirname "$0")" && pwd)
cd "$root"

DEFAULT_IMAGE=images/openwrt-realtek-rtl838x-zyxel_gs1900-8-a1-initramfs-kernel.bin
IMAGE=${IMAGE:-$DEFAULT_IMAGE}
BUILDER=${BUILDER:-rtl838x-qemu-builder}
RUNTIME=${RUNTIME:-rtl838x-qemu-runtime}
DOCKER=${DOCKER:-docker}
DEBUG=${DEBUG:-0}

QEMU=qemu-system-mips
QEMU_BIN=out/qemu/bin/$QEMU

# Filled in by resolve_image.
image_host=""       # path on the host, for messages
image_guest=""      # path the container sees
image_mount=""      # extra docker -v argument, when the image is outside the repo
image_consumed=0    # whether the image came from the command line
image_mode=ro       # how an image outside the repository is mounted
container_net=""    # extra docker run arguments for the network

die() {
    echo "rtl838x: $*" >&2
    exit 1
}

# Run in a container as the invoking user, so nothing in the repository ends
# up owned by root.  A terminal when there is one to attach to; otherwise stdin
# is still passed on, so that the console can be driven through a pipe.
in_container() {
    image=$1
    shift
    tty="-i"
    [ -t 0 ] && tty="-it"
    # shellcheck disable=SC2086
    "$DOCKER" run --rm $tty $container_net \
        -v "$root:/work" -w /work $image_mount \
        -u "$(id -u):$(id -g)" -e HOME=/tmp \
        "$image" "$@"
}

# QEMU run by hand shares the host's network, and so does QEMU run from here:
# hostfwd, socket listen= and connect=, -s and -gdb then open and reach ports
# on the host, as they would without the container.  The tests keep a network
# of their own: they drive everything from inside the container.
with_host_network() {
    container_net="--network host"
}

# Work out where a firmware image lives and how the container can reach it.
# Anything inside the repository is already under the /work mount; anything
# else gets its own read-only mount, so images can be kept wherever suits.
# A second argument names the directory it is mounted under, for when two
# images are.
resolve_image() {
    case $1 in
        /*) abs=$1 ;;
        *)  abs=$invocation_dir/$1 ;;
    esac

    [ -f "$abs" ] || die "firmware image not found: $1"
    abs=$(cd "$(dirname "$abs")" && pwd)/$(basename "$abs")

    image_host=$abs
    case $abs in
        "$root"/*)
            image_guest=/work/${abs#"$root"/}
            image_mount=""
            ;;
        *)
            image_guest=${2:-/image}/$(basename "$abs")
            image_mount="-v $abs:$image_guest:$image_mode"
            ;;
    esac
}

# QEMU cannot read a vendor's zip, so the firmware inside it is written out
# under out/ and booted from there.
unzip_image() {
    case $image_host in
        *.zip|*.ZIP)
            mkdir -p out/firmware
            unzipped=out/firmware/$(basename "${image_host%.*}").bin
            python3 scripts/imgtool.py unzip "$image_host" -o "$unzipped"
            image_host=$root/$unzipped
            image_guest=/work/$unzipped
            image_mount=""
            ;;
    esac
}

# Commands that act on an image take it as their first argument.  QEMU options
# all start with a dash, so a leading bare word is unambiguously the image.
take_image() {
    image_consumed=0
    case ${1:--} in
        -*) resolve_image "$IMAGE" ;;
        *)  resolve_image "$1"; image_consumed=1 ;;
    esac
}

cmd_help() {
    cat <<USAGE
usage: ./rtl838x.sh <command> [arguments]

  build                   build qemu-system-mips with the rtl838x machine
  run [image] [args...]   boot a firmware image; extra arguments go to QEMU
                          (leave the guest with Ctrl-A x); QEMU uses the
                          host's network, so hostfwd ports open on the host
  run-log [image] [...]   boot with unimplemented-register logging to
                          out/qemu.log
  test [image]            boot an image and check it over the serial console
  test-stock <image>      the same for a vendor image (Zyxel .bix, Teltonika
                          TSW2xx, HPE 1920 .bin, Netgear GS108Tv3 .bix,
                          ALLNET ALL-SG8208M .bix), booted with a scratch
                          flash
  mkflash <image> <flash> write a 16 MiB flash image with the firmware
                          installed and a U-Boot environment (in ALLNET's
                          layout for its firmware), a 32 MiB one
                          for Netgear's, or for HPE's firmware a 32 MiB one
                          with a MAC address; extra arguments go to
                          "imgtool.py mkflash"
  run-flash <flash> [image] [...]
                          boot what is installed in a flash image, the way the
                          stock bootloader does, or the image given after it
                          (HPE's, which the machine cannot boot from flash);
                          writes go to the file; the host's network, as with
                          run
  shell                   open a shell in the build container
  info [image]            summarise a firmware image
  dts [image]             print the device tree embedded in a firmware image
  clean                   remove the built emulator
  distclean               also remove the QEMU build tree and compiler cache

The image defaults to \$IMAGE, currently
  $IMAGE
Images outside this directory are mounted into the container automatically,
and a vendor's zip can stand in for the firmware file inside it.

Environment: IMAGE, BUILDER, RUNTIME, DOCKER, DEBUG=1 (build QEMU with -O0
and assertions).
USAGE
}

cmd_builder_image() {
    "$DOCKER" build -t "$BUILDER" -f docker/Dockerfile.builder docker
}

cmd_build() {
    cmd_builder_image
    # scripts/build.sh syncs the models into the pinned tree, then configures
    # and compiles.  DEBUG is passed through to configure.
    tty=""
    [ -t 0 ] && tty="-it"
    # shellcheck disable=SC2086
    "$DOCKER" run --rm $tty \
        -v "$root:/work" -w /work \
        -u "$(id -u):$(id -g)" -e HOME=/tmp -e DEBUG="$DEBUG" \
        "$BUILDER" scripts/build.sh
}

# The runtime image copies out/qemu in, so it is rebuilt whenever the emulator
# is.  Docker's cache makes that nearly free when nothing changed.  The
# emulator itself is rebuilt when the models or the patch are newer than it,
# so a pull or an edit never runs against a stale binary.
cmd_runtime_image() {
    if [ ! -x "$QEMU_BIN" ] ||
        [ -n "$(find src patches scripts/build.sh scripts/sync.sh \
                    -newer "$QEMU_BIN" -print -quit)" ]; then
        cmd_build
    fi
    "$DOCKER" build -q -t "$RUNTIME" -f docker/Dockerfile.runtime . >/dev/null
}

run_qemu() {
    unzip_image
    cmd_runtime_image
    with_host_network
    echo "booting $image_host" >&2
    in_container "$RUNTIME" "$QEMU" \
        -M rtl838x -m 128 -nographic -no-reboot \
        -kernel "$image_guest" "$@"
}

cmd_run() {
    take_image "$@"
    if [ "$image_consumed" = 1 ]; then shift; fi
    run_qemu "$@"
}

# No -kernel: the machine loads the uImage from the flash's first image slot.
# HPE's Comware is booted by a BootWare this machine does not have, so its
# firmware comes after the flash and is given to -kernel, the flash being
# where its configuration goes.
cmd_run_flash() {
    [ $# -ge 1 ] || die "run-flash needs a flash image"
    image_mode=rw
    resolve_image "$1"
    shift
    flash_host=$image_host
    flash_guest=$image_guest
    flash_mount=$image_mount
    kernel=""
    case ${1:--} in
        -*) ;;
        *)
            image_mode=ro
            resolve_image "$1" /firmware
            shift
            unzip_image
            kernel=$image_guest
            ;;
    esac
    image_mount="$flash_mount ${kernel:+$image_mount}"
    cmd_runtime_image
    with_host_network
    if [ -n "$kernel" ]; then
        echo "booting $image_host with flash $flash_host" >&2
        set -- -kernel "$kernel" "$@"
    else
        echo "booting from flash $flash_host" >&2
    fi
    in_container "$RUNTIME" "$QEMU" \
        -M rtl838x -m 128 -nographic -no-reboot \
        -drive "if=mtd,format=raw,file=$flash_guest" "$@"
}

cmd_mkflash() {
    [ $# -ge 2 ] || die "mkflash needs a firmware image and a flash image to write"
    resolve_image "$1"
    case $2 in
        /*) out=$2 ;;
        *)  out=$invocation_dir/$2 ;;
    esac
    shift 2
    python3 scripts/imgtool.py mkflash "$image_host" -o "$out" "$@"
}

cmd_run_log() {
    take_image "$@"
    if [ "$image_consumed" = 1 ]; then shift; fi
    mkdir -p out
    run_qemu -d unimp,guest_errors -D /work/out/qemu.log "$@"
    echo "wrote out/qemu.log"
}

cmd_test() {
    take_image "$@"
    if [ "$image_consumed" = 1 ]; then shift; fi
    cmd_runtime_image
    in_container "$RUNTIME" python3 tests/test_boot.py --image "$image_guest" "$@"
}

cmd_test_stock() {
    [ $# -ge 1 ] || die "test-stock needs a vendor firmware image"
    resolve_image "$1"
    shift
    cmd_runtime_image
    in_container "$RUNTIME" python3 tests/test_stock.py --image "$image_guest" "$@"
}

cmd_shell() {
    cmd_builder_image
    in_container "$BUILDER" bash
}

cmd_info() {
    take_image "$@"
    python3 scripts/imgtool.py info "$image_host"
}

cmd_dts() {
    take_image "$@"
    python3 scripts/imgtool.py dts "$image_host"
}

cmd_clean() {
    rm -rf out/qemu out/qemu.log
}

cmd_distclean() {
    cmd_clean
    rm -rf qemu/build out/ccache
}

command -v "$DOCKER" >/dev/null 2>&1 || die "$DOCKER not found"

case "${1:-help}" in
    help|-h|--help)   cmd_help ;;
    build|qemu-build) shift; cmd_build ;;
    builder-image)    shift; cmd_builder_image ;;
    runtime-image)    shift; cmd_runtime_image ;;
    run)              shift; cmd_run "$@" ;;
    run-log)          shift; cmd_run_log "$@" ;;
    run-flash)        shift; cmd_run_flash "$@" ;;
    mkflash)          shift; cmd_mkflash "$@" ;;
    test)             shift; cmd_test "$@" ;;
    test-stock)       shift; cmd_test_stock "$@" ;;
    shell)            shift; cmd_shell ;;
    info)             shift; cmd_info "$@" ;;
    dts)              shift; cmd_dts "$@" ;;
    clean)            shift; cmd_clean ;;
    distclean)        shift; cmd_distclean ;;
    *)                echo "rtl838x: unknown command '$1'" >&2; echo >&2
                      cmd_help >&2; exit 1 ;;
esac
