#!/bin/bash

mkdir -p build

ret=$?
if [ $ret -ne 0 ]
then
    exit $ret
fi

cd build

ret=$?
if [ $ret -ne 0 ]
then
    exit $ret
fi

if [ "${ARCH}" == "32" ]
then
    echo "64 bit support required for regressions"
    exit 1
fi

cmake -DMSGPACK_FUZZ_REGRESSION="ON" -DMSGPACK_CXX11="ON" -DMSGPACK_BOOST=${BOOST} -DMSGPACK_SAN=${MSGPACK_SAN} -v ..

ret=$?
if [ $ret -ne 0 ]
then
    exit $ret
fi

make

ret=$?
if [ $ret -ne 0 ]
then
    exit $ret
fi

make test

ret=$?
if [ $ret -ne 0 ]
then
    exit $ret
fi

exit 0
