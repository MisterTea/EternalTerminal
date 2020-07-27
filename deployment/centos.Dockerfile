FROM centos:7
LABEL maintainer="Jason Gauci (jgmath2000@gmail.com)"

WORKDIR /root
RUN mkdir .ssh
COPY id_rsa .ssh/
COPY id_rsa.pub .ssh/
RUN chmod 0400 .ssh/id_rsa

RUN yum update -y
RUN yum groupinstall -y "Development Tools"
RUN yum install -y epel-release
RUN yum install --nogpgcheck -y openssh-server cmake3 gcc-c++ protobuf-devel protobuf-lite-devel libsodium-devel
RUN alternatives --install /usr/local/bin/cmake cmake /usr/bin/cmake3 20 \
    --slave /usr/local/bin/ctest ctest /usr/bin/ctest3 \
    --slave /usr/local/bin/cpack cpack /usr/bin/cpack3 \
    --slave /usr/local/bin/ccmake ccmake /usr/bin/ccmake3 \
    --family cmake

RUN git config --global user.email "jgmath2000@gmail.com"
RUN git config --global user.name "Jason Gauci"

RUN echo -e "StrictHostKeyChecking no\n" >> ~/.ssh/config

RUN git clone --branch release git@github.com:MisterTea/EternalTerminal.git
RUN mkdir -p EternalTerminal/build
WORKDIR /root/EternalTerminal/build
RUN cmake -DFULL_PROTOBUF=ON ..
RUN make -j`nproc`
RUN ./et-test
