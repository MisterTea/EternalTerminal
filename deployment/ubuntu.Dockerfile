FROM ubuntu:latest
MAINTAINER Jason Gauci (jgmath2000@gmail.com)

WORKDIR /root
COPY ubuntu .
RUN mkdir .ssh
COPY id_rsa .ssh/
COPY id_rsa.pub .ssh/
RUN chmod ao-rwx .ssh/id_rsa

RUN apt update && apt upgrade && apt install -y build-essential git devscripts ubuntu-dev-tools aptly dput jq

RUN git config --global user.email "jgmath2000@gmail.com"
RUN git config --global user.name "Jason Gauci"

RUN echo "PBUILDERSATISFYDEPENDSCMD=/usr/lib/pbuilder/pbuilder-satisfydepends-apt" > ~/.pbuilderrc

RUN ssh-keyscan github.com >> ~/.ssh/known_hosts
RUN git clone --branch release git@github.com:MisterTea/EternalTerminal.git
RUN mkdir -p EternalTerminal/build
WORKDIR /root/EternalTerminal/build
RUN cmake ..
RUN make -j`nproc`
