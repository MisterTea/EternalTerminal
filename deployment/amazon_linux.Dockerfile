FROM amazonlinux as base

RUN yum install -y \
    boost-devel \
    cmake3 \
    gcc \
    gcc-c++ \
    git \
    kernel-devel \
    libatomic \
    libunwind-devel \
    libutempter-devel \
    make \
    ncurses-devel \
    perl \
    perl-Data-Dumper \
    perl-IPC-Cmd \
    protobuf-compiler \
    protobuf-devel \
    tar \
    zip

WORKDIR /root

RUN echo $'git clone --recurse-submodules --branch release https://github.com/MisterTea/EternalTerminal.git \n\
    mkdir -p EternalTerminal/build \n\
    cd EternalTerminal/build \n\
    cmake3 .. \n\
    make -j`nproc` && make install' > build.sh

