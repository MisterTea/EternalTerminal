FROM ubuntu:latest
LABEL maintainer="Jason Gauci (jgmath2000@gmail.com)"

WORKDIR /root
RUN mkdir .ssh
COPY id_rsa .ssh/
COPY id_rsa.pub .ssh/
COPY .gnupg .gnupg
COPY debian/debian_SOURCE ./debian_SOURCE
RUN chmod ao-rwx .ssh/id_rsa
ARG DEBIAN_FRONTEND=noninteractive

RUN apt update && apt upgrade -y && apt install -y distro-info build-essential git devscripts ubuntu-dev-tools aptly dput jq libsodium-dev libprotobuf-dev protobuf-compiler cmake ninja-build libutempter-dev debhelper dh-systemd

RUN git clone --branch release https://github.com/MisterTea/EternalTerminal.git

COPY ubuntu/deploy_ubuntu_ppa.sh .
