FROM fedora:latest
LABEL maintainer="Jason Gauci (jgmath2000@gmail.com)"

WORKDIR /root
RUN mkdir .ssh
COPY id_rsa .ssh/
COPY id_rsa.pub .ssh/
RUN chmod -R 0700 ~/.ssh

ENV KRB5_TRACE="/dev/stdout kinit jjg@FEDORAPROJECT.ORG"

RUN dnf update -y
RUN dnf install -y @development-tools et openssh-server fedpkg fedora-packager cmake gcc-c++ protobuf-devel protobuf-lite-devel libsodium-devel

ENV KRB5CCNAME=/tmp/ticket
# RUN kinit jjg@FEDORAPROJECT.ORG

RUN git config --global user.email "jgmath2000@gmail.com"
RUN git config --global user.name "Jason Gauci"

RUN echo -e "StrictHostKeyChecking no\n" >> ~/.ssh/config
RUN echo "jjg" > ~/.fedora.upn
RUN git clone --branch release git@github.com:MisterTea/EternalTerminal.git
RUN fedpkg clone et

RUN mkdir -p EternalTerminal/build
WORKDIR /root/EternalTerminal/build
RUN cmake ..
RUN make -j`nproc`
RUN ./et-test
