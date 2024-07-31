FROM alpine:latest

# Packages
RUN apk add --update --no-cache \
        bash \
        libsodium \
        openssh \
        protobuf-dev \
        sudo \
        tzdata \
        bsd-compat-headers \
        openssl-dev \
        linux-headers \
        autoconf automake libtool

RUN apk add build-base libstdc++ libc6-compat zip unzip curl git

RUN apk add boost-dev \
        build-base \
        git \
        gflags-dev \
        libsodium-dev \
        libutempter-dev \
        m4 \
        perl \
        alpine-sdk \
        protobuf-dev \
        libunwind-dev

# Install modern version of cmake
RUN wget https://github.com/Kitware/CMake/releases/download/v3.30.1/cmake-3.30.1.tar.gz
RUN tar xvzf cmake-3.30.1.tar.gz
RUN cd cmake-3.30.1/ ; ./bootstrap --parallel=`nproc`; make -j`nproc`; make install; cd ..

# Install modern version of ninja
RUN wget https://github.com/ninja-build/ninja/archive/refs/tags/v1.12.1.zip
RUN unzip ./v1.12.1.zip
RUN cd ninja-1.12.1/ && /usr/local/bin/cmake . && make -j`nproc` && make install && cp /usr/local/bin/ninja /bin/ && cd ..

# EternalTerminal
RUN git clone --depth=1 --shallow-submodules --recurse-submodules https://github.com/MisterTea/EternalTerminal.git /EternalTerminal

RUN cd /EternalTerminal; \
    mkdir build; \
    cd build; \
    VCPKG_FORCE_SYSTEM_BINARIES=1 /usr/local/bin/cmake ../; \
    make -j`nproc`; \
    make install
