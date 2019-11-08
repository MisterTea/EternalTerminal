set -e
set -x

rm -rf build deps out
mkdir -p build
mkdir -p deps/out/bin
mkdir -p out
cd deps || exit
export PATH="$PWD"/out/bin:/usr/sbin:/usr/bin:/sbin:/bin
curl -OL https://cmake.org/files/v3.9/cmake-3.9.4.tar.gz
tar xvzf cmake-3.9.4.tar.gz
cd cmake-3.9.4 || exit
./configure --prefix="$PWD"/../out
make -j8 install
cd .. || exit
curl -OL http://ftp.gnu.org/gnu/autoconf/autoconf-2.69.tar.gz
tar xvzf autoconf-2.69.tar.gz
cd autoconf-2.69
./configure --prefix="$PWD"/../out
make -j8 install
cd .. || exit
curl -OL http://ftp.gnu.org/gnu/automake/automake-1.15.1.tar.gz
tar xvzf automake-1.15.1.tar.gz
cd automake-1.15.1
./configure --prefix="$PWD"/../out
make -j8 install
cd .. || exit
curl -OL http://ftpmirror.gnu.org/libtool/libtool-2.4.6.tar.gz
tar xvzf libtool-2.4.6.tar.gz
cd libtool-2.4.6
./configure --prefix="$PWD"/../out --disable-shared --enable-static
make -j8 install
cd .. || exit
git clone https://github.com/jedisct1/libsodium.git
git clone https://github.com/google/protobuf.git
cd protobuf || exit
git checkout v3.4.1
cmake -DCMAKE_INSTALL_PREFIX="$PWD"/../out -Dprotobuf_BUILD_TESTS=OFF ./cmake
make -j8 install
cd ../libsodium || exit
git checkout 1.0.12
./autogen.sh
./configure --prefix="$PWD"/../out --disable-shared --enable-static
make -j8 install
cd ../../build || exit
if  [[ -f "$PWD/../deps/out/lib64/libprotobuf.a" ]]; then
PROTO_LIB_DIR="$PWD/../deps/out/lib64"
else
PROTO_LIB_DIR="$PWD/../deps/out/lib"
fi
DISABLE_CRASH_LOG=${DISABLE_CRASH_LOG:-OFF}
cmake \
    -DProtobuf_INCLUDE_DIR="$PWD"/../deps/out/include \
    -DProtobuf_LIBRARY_DEBUG="$PROTO_LIB_DIR"/libprotobuf.a \
    -DProtobuf_LIBRARY_RELEASE="$PROTO_LIB_DIR"/libprotobuf.a \
    -DProtobuf_LITE_LIBRARY_DEBUG="$PROTO_LIB_DIR"/libprotobuf-lite.a \
    -DProtobuf_LITE_LIBRARY_RELEASE="$PROTO_LIB_DIR"/libprotobuf-lite.a \
    -DProtobuf_PROTOC_EXECUTABLE="$PWD"/../deps/out/bin/protoc \
    -DProtobuf_PROTOC_LIBRARY_DEBUG="$PROTO_LIB_DIR"/libprotoc.a \
    -DProtobuf_PROTOC_LIBRARY_RELEASE="$PROTO_LIB_DIR"/libprotoc.a \
    -Dsodium_INCLUDE_DIR="$PWD"/../deps/out/include \
    -Dsodium_LIBRARY_DEBUG="$PWD"/../deps/out/lib/libsodium.a \
    -Dsodium_LIBRARY_RELEASE="$PWD"/../deps/out/lib/libsodium.a \
    -Dsodium_USE_STATIC_LIBS=ON \
    -DCMAKE_INSTALL_PREFIX="$PWD"/../out \
    -DDISABLE_CRASH_LOG="$DISABLE_CRASH_LOG" \
    ../
make -j8 install
cd ../out || exit
echo "Done!  Static binaries of et and etserver are in the out/bin directory"
