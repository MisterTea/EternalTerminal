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
        linux-headers

RUN apk add build-base cmake libstdc++ libc6-compat zip unzip curl git

RUN apk add boost-dev \
        build-base \
        cmake \
        git \
        gflags-dev \
        libsodium-dev \
        libutempter-dev \
        m4 \
        perl \
        alpine-sdk \
        protobuf-dev;

# Install modern version of ninja
RUN wget https://github.com/ninja-build/ninja/archive/refs/tags/v1.12.1.zip
RUN unzip ./v1.12.1.zip
RUN cd ninja-1.12.1/ ; cmake . ; make -j`nproc` ; make install ; cd ..

# EternalTerminal
RUN git clone --depth=1 --shallow-submodules --recurse-submodules https://github.com/MisterTea/EternalTerminal.git /EternalTerminal

#RUN cd /EternalTerminal; \
#    mkdir build; \
#    cd build; \
#    cmake ../;