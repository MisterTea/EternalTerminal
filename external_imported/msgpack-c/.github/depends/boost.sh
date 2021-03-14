#!/bin/sh

usage()
{
  cat <<EOL
  -b   - 32-bit or 64-bit library, maybe 32, 64 or both
  -t   - the toolset, maybe gcc, clang or both
EOL
}

build_boost()
{
  BASE=`pwd`/..
  ./b2 -j4 --toolset=$1 --prefix=${BASE}/usr --libdir="${BASE}/usr/$1/lib$2" --with-chrono --with-context --with-filesystem --with-system --with-timer address-model=$2 install
}

bit="64"
toolset="gcc"

while getopts "b:t:" c; do
  case "$c" in
    b)
      bit="$OPTARG"
      [ "$bit" != "32" ] && [ "$bit" != "64" ] && [ "$bit" != "both" ] && usage && exit 1
      ;;
    t)
      toolset="$OPTARG"
      [ "$toolset" != "gcc" ] && [ "$toolset" != "clang" ] && [ "$toolset" != "both" ] && usage && exit 1
      ;;
    ?*)
      echo "invalid arguments." && exit 1
      ;;
  esac
done

wget https://dl.bintray.com/boostorg/release/1.71.0/source/boost_1_71_0.tar.bz2
tar xf boost_1_71_0.tar.bz2
cd boost_1_71_0
./bootstrap.sh

build()
{
  if [ "$bit" = "both" ]; then
    build_boost $1 32
    build_boost $1 64
  else
    build_boost $1 $bit
  fi
}

if [ "$toolset" = "both" ]; then
  build gcc
  build clang
else
  build $toolset
fi
