set -e
set -x

wget `curl https://api.github.com/repos/mistertea/EternalTerminal/releases/latest | jq '.tarball_url' | sed 's/"//g'` -O `curl https://api.github.com/repos/mistertea/EternalTerminal/releases/latest | jq '.tag_name' | sed 's/"//g' | sed 's/et-v/et_/g' | sed 's/$/.orig.tar.gz/g'`

for distro in `distro-info --supported | grep -v experimental | grep -v sid`; do
    rm -Rf EternalTerminal/debian
    cp -Rf debian_SOURCE EternalTerminal/debian
    sed -i "s/##DISTRO##/${distro}/g" EternalTerminal/debian/changelog

    rm -Rf *.dsc
    pushd EternalTerminal
    debuild -S
    popd
    pbuilder-dist ${distro} amd64 update
    pbuilder-dist ${distro} amd64 build *.dsc
    pbuilder-dist ${distro} i386 update
    pbuilder-dist ${distro} i386 build *.dsc
    pbuilder-dist ${distro} armhf update
    pbuilder-dist ${distro} armhf build *.dsc
    pbuilder-dist ${distro} armel update
    pbuilder-dist ${distro} armel build *.dsc
    pbuilder-dist ${distro} arm64 update
    pbuilder-dist ${distro} arm64 build *.dsc

    aptly repo add -force-replace=true et-${distro} ~/pbuilder/${distro}*_result/*.deb
    aptly publish drop ${distro} || true
    aptly publish repo et-${distro}
done

rsync -raz --delete --progress ~/.aptly/public/* ~/debian-et/debian-source/

echo "Go to ~/debian-et/ and push the new packages."
