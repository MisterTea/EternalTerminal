sudo sbuild-createchroot --include=eatmydata,ccache,gnupg --arch=amd64 jessie /srv/chroot/jessie-amd64-sbuild http://deb.debian.org/debian
sudo sbuild-createchroot --include=eatmydata,ccache,gnupg --arch=i386 jessie /srv/chroot/jessie-i386-sbuild http://deb.debian.org/debian
sudo bash -x /usr/sbin/sbuild-createchroot --include=eatmydata,ccache,gnupg --arch=armhf --foreign jessie /srv/chroot/jessie-armhf-sbuild http://deb.debian.org/debian
