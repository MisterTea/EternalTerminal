# docker build --build-arg BASE=alpine:3.21
ARG BASE=alpine:latest
FROM ${BASE}

RUN apk update
RUN apk add \
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
    mitmproxy \
    moreutils \
    perl \
    powershell \
    python3 \
    python3-dev \
    py3-pip \
    sudo \
    tar \
    tree \
    wget

RUN curl -sSL --retry 5 https://dot.net/v1/dotnet-install.sh | bash -eo pipefail /dev/stdin --channel 8.0 --install-dir /usr/share/dotnet
RUN ln -s /usr/share/dotnet/dotnet /usr/local/bin/dotnet

# https://github.com/actions/runner-images/blob/main/images/ubuntu/scripts/build/install-python.sh
COPY pip.conf /etc/pip.conf
ENV PIPX_BIN_DIR=/opt/pipx_bin
ENV PIPX_HOME=/opt/pipx
RUN python3 -m pip install pipx
RUN python3 -m pipx ensurepath
ENV PATH="PIPX_BIN_DIR:$PATH"
