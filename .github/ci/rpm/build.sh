#!/usr/bin/env bash
# Builds the et RPM inside a rockylinux:8 container.
#
# EL8's glibc 2.28 is the oldest in wide support, so binaries built against it
# run on that and every newer distro. gcc-toolset links the libstdc++ symbols
# newer than EL8's system GCC into the binary, and vcpkg links the remaining
# dependencies statically, so the package needs only glibc, libstdc++ and a
# few long-stable libraries at runtime.
#
# Usage: build.sh SOURCE_DIR OUTPUT_DIR
#   VCPKG_DEFAULT_BINARY_CACHE  reuse built dependency archives (optional)
set -euo pipefail

src=$(cd "$1" && pwd)
out=$2
mkdir -p "$out"

dnf install -y dnf-plugins-core epel-release
dnf config-manager --set-enabled powertools
dnf install -y \
  autoconf \
  autoconf-archive \
  automake \
  bzip2 \
  cmake \
  curl \
  file \
  gcc-toolset-14 \
  gcc-toolset-14-libatomic-devel \
  git \
  kernel-headers \
  libtool \
  libunwind-devel \
  libutempter-devel \
  ninja-build \
  perl \
  perl-IPC-Cmd \
  pkgconf-pkg-config \
  python3.11 \
  rpm-build \
  tar \
  unzip \
  xz \
  xz-devel \
  zip

# meson, which vcpkg builds some ports with, needs Python 3.7 or later.
alternatives --set python3 /usr/bin/python3.11

# shellcheck disable=SC1091
source /opt/rh/gcc-toolset-14/enable

# VCPKG_FORCE_SYSTEM_BINARIES stays unset so vcpkg builds its ports with the
# CMake it pins: its make-based ports rely on policy CMP0174, which EL8's
# CMake 3.26 lacks.
export VCPKG_DEFAULT_BINARY_CACHE="${VCPKG_DEFAULT_BINARY_CACHE:-${src}/.vcpkg-binary-cache}"
export VCPKG_BINARY_SOURCES="clear;files,${VCPKG_DEFAULT_BINARY_CACHE},readwrite"
mkdir -p "$VCPKG_DEFAULT_BINARY_CACHE"

# The checkout belongs to the runner's user, not the container's root.
git config --global --add safe.directory '*'

# CMake finds vcpkg's pkgconf first, which searches no system directories on
# aarch64, so the bash completion directory is set explicitly.
build=$(mktemp -d)
cmake -S "$src" -B "$build" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDISABLE_TELEMETRY=ON \
  -DBUILD_TESTING=OFF \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DBASH_COMPLETION_COMPLETIONSDIR=/usr/share/bash-completion/completions
cmake --build "$build"
(cd "$build" && cpack -G RPM)

for rpm in "$build"/*.rpm; do
  rpm -qip "$rpm"
  rpm -qpR "$rpm"
  cp "$rpm" "$out/"
done
