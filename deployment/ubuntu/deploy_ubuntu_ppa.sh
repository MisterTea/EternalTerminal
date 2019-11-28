set -e

rm -Rf ./*.changes

pushd EternalTCP

for distro in cosmic bionic xenial artful disco; do
    rm -Rf debian
    cp -Rf ../debian_SOURCE debian
    sed -i "s/##DISTRO##/${distro}/g" debian/changelog
    debuild -S
    dput -f ppa:jgmath2000/et ../*${distro}*.changes
done

popd
