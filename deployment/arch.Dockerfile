FROM archlinux:latest
LABEL maintainer="Jason Gauci (jgmath2000@gmail.com)"

RUN pacman -Syu --noconfirm jq git base-devel sudo go openssh emacs
RUN useradd builduser
RUN passwd -d builduser
RUN printf 'builduser ALL=(ALL) ALL\n' | tee -a /etc/sudoers # Allow the builduser passwordless sudo
USER builduser
RUN sudo mkdir -p ~/.ssh
RUN sudo chown -R builduser /home/builduser
WORKDIR /home/builduser

COPY id_rsa .ssh/
COPY id_rsa.pub .ssh/

RUN git clone https://aur.archlinux.org/yay.git
WORKDIR /home/builduser/yay
RUN makepkg -si --noconfirm
RUN yay -Syu --noconfirm eternalterminal

WORKDIR /home/builduser
RUN git clone https://aur.archlinux.org/eternalterminal.git arch_et

RUN git clone --branch `curl https://api.github.com/repos/mistertea/EternalTerminal/releases/latest | jq '.tag_name' | sed 's/"//g'` https://github.com/MisterTea/EternalTerminal.git
RUN mkdir -p EternalTerminal/build
WORKDIR /home/builduser/EternalTerminal/build
RUN cmake ..
RUN make -j`nproc`