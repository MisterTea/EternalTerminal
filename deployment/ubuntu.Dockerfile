FROM ubuntu:latest
LABEL maintainer="Jason Gauci (jgmath2000@gmail.com)"

WORKDIR /root
RUN mkdir .ssh
COPY id_rsa .ssh/
COPY id_rsa.pub .ssh/
COPY .gnupg .gnupg
COPY ubuntu/debian_SOURCE ./debian_SOURCE
RUN chmod ao-rwx .ssh/id_rsa

RUN apt update && apt upgrade -y && apt install -y build-essential git devscripts ubuntu-dev-tools aptly dput jq libsodium-dev libprotobuf-dev protobuf-compiler cmake libutempter-dev debhelper dh-systemd

RUN git config --global user.email "jgmath2000@gmail.com"
RUN git config --global user.name "Jason Gauci"

RUN ssh-keyscan github.com >> ~/.ssh/known_hosts
RUN git clone --branch release git@github.com:MisterTea/EternalTerminal.git

COPY ubuntu/deploy_ubuntu_ppa.sh .
