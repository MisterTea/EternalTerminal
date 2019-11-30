set -e

rm -Rf ./*.changes

wget `curl https://api.github.com/repos/mistertea/EternalTerminal/releases/latest | jq '.tarball_url' | sed 's/"//g'` -O `curl https://api.github.com/repos/mistertea/EternalTerminal/releases/latest | jq '.tag_name' | sed 's/"//g' | sed 's/et-v/et_/g' | sed 's/$/.orig.tar.gz/g'`

pushd EternalTerminal

git checkout master
git pull
git checkout `curl https://api.github.com/repos/mistertea/EternalTerminal/releases/latest | jq '.tag_name' | sed 's/"//g'`

for distro in cosmic bionic xenial artful disco; do
    rm -Rf debian
    cp -Rf ../debian_SOURCE debian
    sed -i "s/##DISTRO##/${distro}/g" debian/changelog
    debuild -S
    dput -f ppa:jgmath2000/et ../*${distro}*.changes
done

popd
