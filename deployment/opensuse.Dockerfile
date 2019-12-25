FROM opensuse/leap:latest
LABEL maintainer="Jason Gauci (jgmath2000@gmail.com)"

WORKDIR /root
RUN mkdir .ssh
COPY id_rsa .ssh/
COPY id_rsa.pub .ssh/
COPY .gnupg .gnupg

RUN zypper --non-interactive install -t pattern devel_C_C++
RUN zypper --non-interactive install git openssh cmake gcc-c++ protobuf-devel libsodium-devel

RUN git config --global user.email "jgmath2000@gmail.com"
RUN git config --global user.name "Jason Gauci"

RUN ssh-keyscan github.com >> ~/.ssh/known_hosts
RUN git clone --branch release git@github.com:MisterTea/EternalTerminal.git
