FROM debian:latest
LABEL maintainer="Jason Gauci (jgmath2000@gmail.com)"

WORKDIR /root
COPY ubuntu .
RUN mkdir .ssh
COPY id_rsa .ssh/
COPY id_rsa.pub .ssh/
RUN chmod ao-rwx .ssh/id_rsa
COPY ubuntu/debian_SOURCE ./debian_SOURCE

RUN apt update && apt upgrade -y && apt install -y build-essential git devscripts aptly dput jq libsodium-dev libprotobuf-dev protobuf-compiler cmake libutempter-dev debhelper dh-systemd pbuilder ubuntu-dev-tools

RUN git config --global user.email "jgmath2000@gmail.com"
RUN git config --global user.name "Jason Gauci"

RUN echo "PBUILDERSATISFYDEPENDSCMD=/usr/lib/pbuilder/pbuilder-satisfydepends-apt" > ~/.pbuilderrc

RUN ssh-keyscan github.com >> ~/.ssh/known_hosts
RUN git clone --branch release git@github.com:MisterTea/EternalTerminal.git
RUN git clone git@github.com:MisterTea/debian-et.git
RUN mkdir -p EternalTerminal/build
WORKDIR /root/EternalTerminal/build
RUN cmake ..
RUN make -j`nproc`

RUN pbuilder-dist stretch i386 create --debootstrapopts --variant=buildd && \
    pbuilder-dist stretch amd64 create --debootstrapopts --variant=buildd && \
    pbuilder-dist stretch armhf create --debootstrapopts --variant=buildd && \
    pbuilder-dist stretch armel create --debootstrapopts --variant=buildd

COPY debian/build_all_deb.sh .
