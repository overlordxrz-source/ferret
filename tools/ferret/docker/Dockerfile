# SPDX-License-Identifier: GPL-2.0-only
#
# Ferret build environment.
#
# Provides a reproducible Linux (aarch64) userspace with everything needed to
# cross-build the x86_64 kernel, run its KUnit tests and boot it under QEMU.
#
# Build:  docker build -t ferret-buildenv:1 tools/ferret/docker
# Use:    tools/ferret/docker/run.sh ...

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8

# Kernel build dependencies (Documentation/process/changes.rst) plus the
# LLVM toolchain, QEMU and the Rust toolchain bootstrap.
RUN apt-get update && apt-get install -y --no-install-recommends \
        bc \
        bison \
        ca-certificates \
        ccache \
        clang-18 \
        cpio \
        curl \
        dwarves \
        e2fsprogs \
        flex \
        gcc \
        git \
        kmod \
        libclang-18-dev \
        libelf-dev \
        libssl-dev \
        lld-18 \
        llvm-18 \
        make \
        pkg-config \
        python3 \
        qemu-system-arm \
        qemu-system-x86 \
        rsync \
        xz-utils \
        zstd \
    && rm -rf /var/lib/apt/lists/* \
    && ln -sf /usr/bin/clang-18 /usr/local/bin/clang \
    && ln -sf /usr/bin/ld.lld-18 /usr/local/bin/ld.lld \
    && ln -sf /usr/bin/llvm-ar-18 /usr/local/bin/llvm-ar \
    && ln -sf /usr/bin/llvm-nm-18 /usr/local/bin/llvm-nm \
    && ln -sf /usr/bin/llvm-objcopy-18 /usr/local/bin/llvm-objcopy \
    && ln -sf /usr/bin/llvm-objdump-18 /usr/local/bin/llvm-objdump \
    && ln -sf /usr/bin/llvm-readelf-18 /usr/local/bin/llvm-readelf \
    && ln -sf /usr/bin/llvm-strip-18 /usr/local/bin/llvm-strip

# Pinned Rust toolchain. See docs/ferret/BUILDING.md for the rationale.
ENV RUSTUP_HOME=/opt/rustup \
    CARGO_HOME=/opt/cargo \
    PATH=/opt/cargo/bin:$PATH \
    RUSTUP_TOOLCHAIN=1.88.0

RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | \
        sh -s -- -y --no-modify-path --profile minimal --default-toolchain 1.88.0 \
    && rustup component add rust-src rustfmt clippy \
    && rustup target add x86_64-unknown-none

# bindgen must be new enough for the kernel's bindgen requirements (>= 0.65.1)
# and linked against the clang that the kernel build uses.
RUN cargo install bindgen-cli --version 0.72.1 --locked \
    && ln -sf /opt/cargo/bin/bindgen /usr/local/bin/bindgen

# Detect host CPU count for -j defaults; overridable at run time.
WORKDIR /build
CMD ["/bin/bash"]
