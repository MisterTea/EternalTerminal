#!/bin/bash

cd example/cmake

ret=$?
if [ $ret -ne 0 ]
then
    exit $ret
fi

rm -f msgpack-c

ret=$?
if [ $ret -ne 0 ]
then
    exit $ret
fi

ln -s ../.. msgpack-c

ret=$?
if [ $ret -ne 0 ]
then
    exit $ret
fi

mkdir build

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

cmake -DEXAMPLE_MSGPACK_EMBEDDED=ON ..

ret=$?
if [ $ret -ne 0 ]
then
    exit $ret
fi

make example example-static

ret=$?
if [ $ret -ne 0 ]
then
    exit $ret
fi

exit 0
