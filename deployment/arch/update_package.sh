cd ~/arch_et
VERSION_NUMBER=$(curl https://api.github.com/repos/mistertea/EternalTerminal/releases/latest | jq '.tag_name' | sed -E 's/"et\-v(.*)"/\1/g')
wget https://github.com/MisterTea/EternalTerminal/archive/et-v${VERSION_NUMBER}.tar.gz
NEW_SHA=`sha256sum et-v${VERSION_NUMBER}.tar.gz | awk '{print $1}'`
sed -i -E "s/sha256sums=\('(.*)'\)/sha256sums=\('$NEW_SHA'\)/" PKGBUILD
sed -i -E "s/pkgver='(.*)'/pkgver='$VERSION_NUMBER'/" PKGBUILD
makepkg --printsrcinfo > .SRCINFO
makepkg -Acsf
sudo pacman -U x.pkg.tar.xz
sudo pacman -U --noconfirm eternalterminal-${VERSION_NUMBER}*.pkg.tar*
