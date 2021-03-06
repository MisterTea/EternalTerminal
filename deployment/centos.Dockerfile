FROM centos:8
LABEL maintainer="Jason Gauci (jgmath2000@gmail.com)"

WORKDIR /root
RUN mkdir .ssh
COPY id_rsa .ssh/
COPY id_rsa.pub .ssh/
RUN chmod 0400 .ssh/id_rsa

RUN dnf -y install dnf-plugins-core
RUN dnf update -y
RUN dnf groupinstall -y "Development Tools"
RUN dnf install -y epel-release
RUN dnf config-manager --set-enabled PowerTools
RUN dnf install --nogpgcheck -y openssh-server cmake3 gcc-c++ protobuf-devel libsodium-devel
RUN alternatives --install /usr/local/bin/cmake cmake /usr/bin/cmake3 20 \
    --slave /usr/local/bin/ctest ctest /usr/bin/ctest3 \
    --slave /usr/local/bin/cpack cpack /usr/bin/cpack3 \
    --slave /usr/local/bin/ccmake ccmake /usr/bin/ccmake3 \
    --family cmake

RUN git clone --branch release https://github.com/MisterTea/EternalTerminal.git
RUN mkdir -p EternalTerminal/build
WORKDIR /root/EternalTerminal/build
RUN cmake -DFULL_PROTOBUF=ON ..
RUN make -j`nproc`
RUN TSAN_OPTIONS="suppressions=../test/test_tsan.suppression" ./et-test
