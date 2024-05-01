# See here for image contents: https://github.com/microsoft/vscode-dev-containers/tree/v0.224.3/containers/cpp/.devcontainer/base.Dockerfile

# [Choice] Debian / Ubuntu version (use Debian 11, Ubuntu 18.04/21.04 on local arm64/Apple Silicon): debian-11, debian-10, ubuntu-21.04, ubuntu-20.04, ubuntu-18.04
ARG VARIANT="bullseye"
FROM mcr.microsoft.com/vscode/devcontainers/cpp:0-${VARIANT}

ARG USERNAME=vscode

# [Optional] Install CMake version different from what base image has already installed. 
# CMake reinstall choices: none, 3.21.5, 3.22.2, or versions from https://cmake.org/download/
ARG REINSTALL_CMAKE_VERSION_FROM_SOURCE="3.23.1"

# Use installed binaries from the system.
# Do not download latest version of CMake and Ninja during vcpkg bootstrap!
ENV VCPKG_FORCE_SYSTEM_BINARIES=1
ENV VCPKG_USE_SYSTEM_BINARIES=1

# Optionally install the cmake for vcpkg
COPY ./reinstall-cmake.sh /tmp/
RUN if [ "${REINSTALL_CMAKE_VERSION_FROM_SOURCE}" != "none" ]; then \
    chmod +x /tmp/reinstall-cmake.sh && /tmp/reinstall-cmake.sh ${REINSTALL_CMAKE_VERSION_FROM_SOURCE}; \
    fi \
    && rm -f /tmp/reinstall-cmake.sh

# Install dependencies.
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install --no-install-recommends \
    libboost-dev \
    libsodium-dev \
    libprotobuf-dev \
    protobuf-compiler \
    libgflags-dev \
    libutempter-dev \
    build-essential \
    ninja-build \
    # Note that in Ubuntu 21.04, there is no libcurl-dev, use libcurl4-openssl-dev
    libcurl4-openssl-dev

#
# Set up command history volume
# See https://code.visualstudio.com/remote/advancedcontainers/persist-bash-history
#
RUN mkdir /commandhistory \
    && chown -R $USERNAME /commandhistory
