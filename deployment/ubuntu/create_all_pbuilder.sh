set -e

echo "PBUILDERSATISFYDEPENDSCMD=/usr/lib/pbuilder/pbuilder-satisfydepends-apt" > ~/.pbuilderrc

rm -Rf ~/pbbuilder

pbuilder-dist stretch i386 create --debootstrapopts --variant=buildd
pbuilder-dist stretch amd64 create --debootstrapopts --variant=buildd
pbuilder-dist stretch armhf create --debootstrapopts --variant=buildd
pbuilder-dist stretch armel create --debootstrapopts --variant=buildd

