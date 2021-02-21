#!/bin/bash

c_headers=(
    include/msgpack.h
    include/msgpack/gcc_atomic.h
    include/msgpack/pack.h
    include/msgpack/pack_define.h
    include/msgpack/pack_template.h
    include/msgpack/sbuffer.h
    include/msgpack/timestamp.h
    include/msgpack/unpack.h
    include/msgpack/unpack_template.h
    include/msgpack/util.h
    include/msgpack/version.h
    include/msgpack/vrefbuffer.h
)

rm -f c_headers.tmp
for hdr in ${c_headers[@]}; do
    echo $hdr >> c_headers.tmp
done

find src -name "*.c" | sed -e 's/\s\+/\n/g' | sort > srcs.tmp
find include -name "*.h" | grep -vFf c_headers.tmp | sed -e 's/\s\+/\n/g' | sort > c_cpp_headers.tmp
find include -name "*.hpp" | sed -e 's/\s\+/\n/g' | sort > cpp_headers.tmp

echo 'LIST (APPEND msgpackc_SOURCES' > Files.cmake
cat srcs.tmp | sed -e 's/^/    /g' >> Files.cmake
echo -e ')\n' >> Files.cmake

echo '# For both C and C++ libraries
LIST (APPEND msgpackc_HEADERS' >> Files.cmake
cat c_cpp_headers.tmp | sed -e 's/^/    /g' >> Files.cmake
echo -e ')\n' >> Files.cmake

echo 'IF (NOT MSGPACK_CXX_ONLY)
    # Only for C library
    LIST (APPEND msgpackc_HEADERS' >> Files.cmake
cat c_headers.tmp | sed -e 's/^/        /g' >> Files.cmake
echo -e '    )\nENDIF ()\n' >> Files.cmake

echo 'IF (MSGPACK_ENABLE_CXX)
    # Only for C++ library
    LIST (APPEND msgpackc_HEADERS' >> Files.cmake
cat cpp_headers.tmp | sed -e 's/^/        /g' >> Files.cmake
echo -e '    )\nENDIF ()' >> Files.cmake

rm -f srcs.tmp c_headers.tmp cpp_headers.tmp c_cpp_headers.tmp
