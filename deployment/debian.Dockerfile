FROM debian:latest
LABEL maintainer="Jason Gauci (jgmath2000@gmail.com)"

WORKDIR /root
COPY ubuntu .
RUN mkdir .ssh
COPY id_rsa .ssh/
COPY id_rsa.pub .ssh/
RUN chmod ao-rwx .ssh/id_rsa
COPY debian/debian_SOURCE ./debian_SOURCE

RUN echo "For debian, docker doesn't work with pbuilder.  Use vagrant" && exit 1

RUN apt update && apt upgrade -y && apt install -y build-essential git devscripts aptly dput jq libsodium-dev libprotobuf-dev protobuf-compiler cmake libutempter-dev debhelper dh-systemd pbuilder ubuntu-dev-tools

RUN echo "PBUILDERSATISFYDEPENDSCMD=/usr/lib/pbuilder/pbuilder-satisfydepends-apt" > ~/.pbuilderrc

RUN git clone --branch release https://github.com/MisterTea/EternalTerminal.git
RUN git clone https://github.com/MisterTea/debian-et.git
RUN mkdir -p EternalTerminal/build
WORKDIR /root/EternalTerminal/build
RUN cmake ..
RUN make -j`nproc`

RUN pbuilder-dist stretch i386 create --debootstrapopts --variant=buildd && \
    pbuilder-dist stretch amd64 create --debootstrapopts --variant=buildd && \
    pbuilder-dist stretch armhf create --debootstrapopts --variant=buildd && \
    pbuilder-dist stretch armel create --debootstrapopts --variant=buildd

COPY debian/build_all_deb.sh .
