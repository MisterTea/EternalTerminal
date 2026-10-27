# docker build --build-arg BASE=alpine:3.21
ARG BASE=alpine:latest
FROM ${BASE}

ARG PIP_INSTALL=
COPY requirements.txt /tmp/requirements.txt

RUN apk add --no-cache \
    bash \
    build-base \
    cargo \
    clang \
    cmake \
    curl \
    file \
    git \
    icu \
    linux-headers \
    lsb-release-minimal \
    llvm \
    mitmproxy \
    moreutils \
    nodejs \
    npm \
    openssl-dev \
    perl \
    powershell \
    python3 \
    python3-dev \
    py3-pip \
    rustup \
    sudo \
    tar \
    tree \
    wget

# 3.23+
RUN if awk -F. '{ exit !($1 > 3 || ($1 == 3 && $2 >= 23)) }' /etc/alpine-release; then \
        (apk add --no-cache llvm22-libs || \
            apk add --no-cache --repository=https://dl-cdn.alpinelinux.org/alpine/edge/main llvm22-libs) && \
        cargo install --locked --git https://github.com/aya-rs/bpf-linker --root /usr/local bpf-linker && \
        RUSTUP_INIT_SKIP_PATH_CHECK=yes rustup-init -y --profile minimal --default-toolchain none && \
        . /root/.cargo/env && \
        rustup toolchain install nightly --profile minimal --component rust-src && \
        ln -s /root/.cargo/bin/rustup /usr/local/bin/rustup && \
        if [ -n "${PIP_INSTALL}" ]; then \
            python3 -m pip install --break-system-packages --upgrade --constraint /tmp/requirements.txt ${PIP_INSTALL}; \
        fi; \
    fi
ENV CARGO_HOME=/root/.cargo
ENV RUSTUP_HOME=/root/.rustup

RUN curl -sSL --retry 5 https://dot.net/v1/dotnet-install.sh | bash -eo pipefail /dev/stdin --channel 10.0 --install-dir /usr/share/dotnet
RUN ln -s /usr/share/dotnet/dotnet /usr/local/bin/dotnet

# https://github.com/actions/runner-images/blob/main/images/ubuntu/scripts/build/install-python.sh
COPY pip.conf /etc/pip.conf
ENV PIPX_BIN_DIR=/opt/pipx_bin
ENV PIPX_HOME=/opt/pipx
RUN python3 -m pip install pipx
RUN python3 -m pipx ensurepath
ENV PATH="PIPX_BIN_DIR:$PATH"
