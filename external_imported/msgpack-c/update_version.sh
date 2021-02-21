#!/bin/sh

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 version" >&2
    exit 1
fi

major=`echo $1 | sed -e "s/^\([0-9]*\)\.[0-9]*\.[0-9]*/\1/"`
minor=`echo $1 | sed -e "s/^[0-9]*\.\([0-9]*\)\.[0-9]*/\1/"`
revision=`echo $1 | sed -e "s/^[0-9]*\.[0-9]*\.\([0-9]*\)/\1/"`

sed -i -e "s/^\(#define MSGPACK_VERSION_MAJOR[[:space:]]*\)[0-9]*/\1$major/" include/msgpack/version_master.h
sed -i -e "s/^\(#define MSGPACK_VERSION_MINOR[[:space:]]*\)[0-9]*/\1$minor/" include/msgpack/version_master.h
sed -i -e "s/^\(#define MSGPACK_VERSION_REVISION[[:space:]]*\)[0-9]*/\1$revision/" include/msgpack/version_master.h

sed -i -e "s/\(^Version \)[0-9]*\.[0-9]*\.[0-9]*/\1$1/" README.md
sed -i -e "s/\(^version: \)[0-9]*\.[0-9]*\.[0-9]*/\1$1/" appveyor.yml
