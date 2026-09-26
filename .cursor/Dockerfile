FROM ubuntu:24.04

# Non-interactive apt for reproducible, unattended builds.
ENV DEBIAN_FRONTEND=noninteractive

# System toolchain and libraries required to build Eternal Terminal with the
# system-dependency path (-DDISABLE_VCPKG=ON), run its test suite, lint, and
# collect coverage. These are stable, so they live in the image layer.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        g++ \
        cmake \
        ninja-build \
        pkg-config \
        git \
        curl \
        ca-certificates \
        zip \
        unzip \
        tar \
        autoconf \
        automake \
        libtool \
        libtool-bin \
        autoconf-archive \
        protobuf-compiler \
        libprotobuf-dev \
        libsodium-dev \
        libgflags-dev \
        libutempter-dev \
        libcurl4-openssl-dev \
        libssl-dev \
        libunwind-dev \
        libboost-dev \
        clang-format-18 \
        openssh-server \
        openssh-client \
        gdb \
        lcov \
        python3 \
    && rm -rf /var/lib/apt/lists/*

# The ubuntu:24.04 image already ships a non-root "ubuntu" user (uid 1000).
USER ubuntu
