set -e
set -x

echo "PBUILDERSATISFYDEPENDSCMD=/usr/lib/pbuilder/pbuilder-satisfydepends-apt" > ~/.pbuilderrc

for distro in stretch; do
    rm -Rf EternalTCP/debian
    cp -Rf debian_SOURCE EternalTCP/debian
    sed -i "s/##DISTRO##/${distro}/g" EternalTCP/debian/changelog

    rm -Rf *.dsc
    pushd EternalTCP
    debuild -S
    popd
    pbuilder-dist ${distro} amd64 update
    pbuilder-dist ${distro} amd64 build *.dsc
    pbuilder-dist ${distro} i386 update
    pbuilder-dist ${distro} i386 build *.dsc
    #pbuilder-dist ${distro} armhf update
    #pbuilder-dist ${distro} armhf build *.dsc
    #pbuilder-dist ${distro} armel update
    #pbuilder-dist ${distro} armel build *.dsc

    aptly repo add et-${distro} ~/pbuilder/${distro}*_result/*.deb
    aptly publish drop ${distro}
    aptly publish repo et-${distro}
done

rsync -raz --delete --progress ~/.aptly/public/* ~/github/debian-et/debian-source/

echo "Go to ~/github/debian-et/ and push the new packages."
