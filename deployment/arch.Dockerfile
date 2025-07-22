FROM archlinux:latest
LABEL maintainer="Jason Gauci (jgmath2000@gmail.com)"

RUN pacman -Syu --noconfirm
RUN pacman -Syu --noconfirm --needed jq git base-devel sudo go openssh emacs ninja libunwind libutempter namcap curl zip unzip tar openssl libsodium protobuf wget

RUN useradd -m -g users -G wheel -s /bin/bash et

# Add user to sudoers
RUN echo "%wheel ALL=(ALL:ALL) NOPASSWD: ALL" >> /etc/sudoers
RUN echo "et:mypassword" | chpasswd

WORKDIR /home/et
RUN mkdir .ssh
COPY id_rsa .ssh/
COPY id_rsa.pub .ssh/
COPY .gnupg .gnupg
COPY arch/update_package.sh ./update_package.sh
RUN chown -R et .ssh .gnupg
RUN chmod ao-rwx .ssh/id_rsa
RUN chmod -R 0700 /home/et/.gnupg /home/et/.ssh

USER et

RUN git config --global user.email "jgmath2000@gmail.com"
RUN git config --global user.name "Jason Gauci"
RUN ssh-keyscan github.com >> ~/.ssh/known_hosts
RUN ssh-keyscan aur.archlinux.org >> ~/.ssh/known_hosts

RUN git clone https://aur.archlinux.org/yay.git
WORKDIR /home/et/yay
RUN makepkg -si --noconfirm
RUN yay -Syu --noconfirm eternalterminal || true
WORKDIR /home/et

#RUN git clone ssh://aur@aur.archlinux.org/eternalterminal.git arch_et
RUN git clone --branch `curl https://api.github.com/repos/mistertea/EternalTerminal/releases/latest | jq '.tag_name' | sed 's/"//g'` git@github.com:MisterTea/EternalTerminal.git
RUN mkdir -p EternalTerminal/build
WORKDIR /home/et/EternalTerminal/build
RUN cmake -DDISABLE_VCPKG=ON ..
RUN make -j`nproc` || true
