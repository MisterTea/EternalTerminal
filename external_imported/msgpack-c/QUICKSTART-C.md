# Implementation Status

The serialization library is production-ready.

Currently, RPC implementation is not available.

# Install

## Install with package manager

### MacOS with MacPorts

On MacOS, you can install MessagePack for C using MacPorts.

```
$ sudo port install msgpack
```

You might need to run `sudo port selfupdate` before installing to update the package repository.

You can also install via Homebrew.

```
$ brew install msgpack
```

### FreeBSD with Ports Collection

On FreeBSD, you can use Ports Collection. Install [net/msgpack](http://www.freebsd.org/cgi/cvsweb.cgi/ports/devel/msgpack/) package.

### Gentoo Linux with Portage

On Gentoo Linux, you can use emerge. Install [dev-libs/msgpack](http://gentoo-portage.com/dev-libs/msgpack) package.

### Windows with vcpkg

There are several package managers available, and vcpkg is typical.

```
$ vcpkg install msgpack:x64-windows
```
## Install with source code

### Build with cmake

You need to install cmake (2.8.12 or higher) first.

```
$ git clone https://github.com/msgpack/msgpack-c.git
$ cd msgpack-c
$ mkdir build
$ cd build
$ cmake ..
$ cmake --build .
$ cmake --build . --target install
```
### Build with autotools

In versions 1.4.2 and below, you can use `autotools` to build on UNIX-like platforms.
```
$ wget https://github.com/msgpack/msgpack-c/archive/cpp-1.3.0.tar.gz
$ tar zxvf msgpack-1.3.0.tar.gz
$ cd msgpack-1.3.0
$ ./bootstrap  # If the 'configure' script already exists, you can omit this step.
$ ./configure
$ make
$ sudo make install
```

# Serialization QuickStart for C

## First program

Include `msgpack.h` header and link `msgpack` library to use MessagePack on your program.

```c
#include <msgpack.h>
#include <stdio.h>

int main(void) {

        /* creates buffer and serializer instance. */
        msgpack_sbuffer* buffer = msgpack_sbuffer_new();
        msgpack_packer* pk = msgpack_packer_new(buffer, msgpack_sbuffer_write);

        /* serializes ["Hello", "MessagePack"]. */
        msgpack_pack_array(pk, 2);
        msgpack_pack_bin(pk, 5);
        msgpack_pack_bin_body(pk, "Hello", 5);
        msgpack_pack_bin(pk, 11);
        msgpack_pack_bin_body(pk, "MessagePack", 11);

        /* deserializes it. */
        msgpack_unpacked msg;
        msgpack_unpacked_init(&msg);
        msgpack_unpack_return ret = msgpack_unpack_next(&msg, buffer->data, buffer->size, NULL);

        /* prints the deserialized object. */
        msgpack_object obj = msg.data;
        msgpack_object_print(stdout, obj);  /*=> ["Hello", "MessagePack"] */

        /* cleaning */
        msgpack_sbuffer_free(buffer);
        msgpack_packer_free(pk);
}
```

## Simple program with a loop

```c
#include <msgpack.h>
#include <stdio.h>

int main(void) {

        /* creates buffer and serializer instance. */
        msgpack_sbuffer* buffer = msgpack_sbuffer_new();
        msgpack_packer* pk = msgpack_packer_new(buffer, msgpack_sbuffer_write);

        int j;

        for(j = 0; j<23; j++) {
           /* NB: the buffer needs to be cleared on each iteration */
           msgpack_sbuffer_clear(buffer);

           /* serializes ["Hello", "MessagePack"]. */
           msgpack_pack_array(pk, 3);
           msgpack_pack_bin(pk, 5);
           msgpack_pack_bin_body(pk, "Hello", 5);
           msgpack_pack_bin(pk, 11);
           msgpack_pack_bin_body(pk, "MessagePack", 11);
           msgpack_pack_int(pk, j);

           /* deserializes it. */
           msgpack_unpacked msg;
           msgpack_unpacked_init(&msg);
           msgpack_unpack_return ret = msgpack_unpack_next(&msg, buffer->data, buffer->size, NULL);

           /* prints the deserialized object. */
           msgpack_object obj = msg.data;
           msgpack_object_print(stdout, obj);  /*=> ["Hello", "MessagePack"] */
           puts("");
        }

        /* cleaning */
        msgpack_sbuffer_free(buffer);
        msgpack_packer_free(pk);
}
```

## Streaming feature

```c
#include <msgpack.h>
#include <stdio.h>

int main(void) {
        /* serializes multiple objects using msgpack_packer. */
        msgpack_sbuffer* buffer = msgpack_sbuffer_new();
        msgpack_packer* pk = msgpack_packer_new(buffer, msgpack_sbuffer_write);
        msgpack_pack_int(pk, 1);
        msgpack_pack_int(pk, 2);
        msgpack_pack_int(pk, 3);

        /* deserializes these objects using msgpack_unpacker. */
        msgpack_unpacker pac;
        msgpack_unpacker_init(&pac, MSGPACK_UNPACKER_INIT_BUFFER_SIZE);

        /* feeds the buffer. */
        msgpack_unpacker_reserve_buffer(&pac, buffer->size);
        memcpy(msgpack_unpacker_buffer(&pac), buffer->data, buffer->size);
        msgpack_unpacker_buffer_consumed(&pac, buffer->size);

        /* now starts streaming deserialization. */
        msgpack_unpacked result;
        msgpack_unpacked_init(&result);

        while(msgpack_unpacker_next(&pac, &result)) {
            msgpack_object_print(stdout, result.data);
            puts("");
        }

        /* results:
         * $ gcc stream.cc -lmsgpackc -o stream
         * $ ./stream
         * 1
         * 2
         * 3
         */
}
```
