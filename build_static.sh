set -e

mkdir -p build
mkdir -p deps/out
mkdir -p out
cd deps || exit
git clone https://github.com/gflags/gflags.git
git clone https://github.com/google/glog.git
git clone https://github.com/jedisct1/libsodium.git
git clone https://github.com/google/protobuf.git
cd gflags || exit
git checkout v2.2.0
cmake -DCMAKE_INSTALL_PREFIX="$PWD"/../out -DBUILD_SHARED_LIBS=OFF ./
make -j8 install
cd ../glog || exit
git checkout v0.3.5
cmake -DCMAKE_INSTALL_PREFIX="$PWD"/../out -DWITH_GFLAGS=ON -DWITH_THREADS=ON -Dgflags_DIR="$PWD"../out/lib/cmake/gflags ./
make -j8 install
cd ../protobuf || exit
git checkout v3.3.0
cmake -DCMAKE_INSTALL_PREFIX="$PWD"/../out -Dprotobuf_BUILD_TESTS=OFF ./cmake
make -j8 install
cd ../libsodium || exit
git checkout 1.0.12
./autogen.sh
./configure --prefix="$PWD"/../out --disable-shared --enable-static
make -j8 install
cd ../../build || exit
cmake \
    -DGFLAGS_INCLUDE_DIR="$PWD"/../deps/out/include \
    -DGFLAGS_LIBRARY="$PWD"/../deps/out/lib/libgflags.a \
    -DGLOG_INCLUDE_DIR="$PWD"/../deps/out/include \
    -DGLOG_LIBRARY="$PWD"/../deps/out/lib/libglog.a \
    -DProtobuf_INCLUDE_DIR="$PWD"/../deps/out/include \
    -DProtobuf_LIBRARY_DEBUG="$PWD"/../deps/out/lib/libprotobuf.a \
    -DProtobuf_LIBRARY_RELEASE="$PWD"/../deps/out/lib/libprotobuf.a \
    -DProtobuf_LITE_LIBRARY_DEBUG="$PWD"/../deps/out/lib/libprotobuf-lite.a \
    -DProtobuf_LITE_LIBRARY_RELEASE="$PWD"/../deps/out/lib/libprotobuf-lite.a \
    -DProtobuf_PROTOC_EXECUTABLE="$PWD"/../deps/out/bin/protoc \
    -DProtobuf_PROTOC_LIBRARY_DEBUG="$PWD"/../deps/out/lib/libprotoc.a \
    -DProtobuf_PROTOC_LIBRARY_RELEASE="$PWD"/../deps/out/lib/libprotoc.a \
    -Dsodium_INCLUDE_DIR="$PWD"/../deps/out/include \
    -Dsodium_LIBRARY_DEBUG="$PWD"/../deps/out/lib/libsodium.a \
    -Dsodium_LIBRARY_RELEASE="$PWD"/../deps/out/lib/libsodium.a \
    -Dsodium_USE_STATIC_LIBS=ON \
    -DCMAKE_INSTALL_PREFIX="$PWD"/../out \
    ../
make -j8 install
cd ../out | exit
echo "Done!"
