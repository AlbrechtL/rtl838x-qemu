# SPDX-License-Identifier: GPL-2.0-or-later
# Toolchain for building qemu-system-mips.  The repository is bind-mounted at
# /work rather than copied in, so edit-build cycles do not rebuild the image.
FROM debian:13-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        ccache \
        git \
        pkgconf \
        ninja-build \
        meson \
        python3 \
        python3-venv \
        python3-setuptools \
        python3-tomli \
        flex \
        bison \
        libglib2.0-dev \
        libpixman-1-dev \
        zlib1g-dev \
        libslirp-dev \
        libcapstone-dev \
    && rm -rf /var/lib/apt/lists/*

ENV HOME=/tmp
WORKDIR /work
