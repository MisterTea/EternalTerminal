Introduction
======
This library is fork of (https://github.com/adamvr/arduino-base64).

All credits go to its author [Adam Rudd](https://github.com/adamvr).

I have only modified it a little, to be able to use it with stl strings.

But the algorithm remained untouched.

Installation
======
Library is header only, so you can just copy base64.h where you want it.

Or you can use [CMake](http://cmake.org/)
```
cd base64; mkdir build; cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr ..
```
Generates basic makefile
```
make check
```
Runs simple test that will generate random alphanum and binary strings, tries to encode/decode them and compares the input and decoded string
```
make install
```
Installs base64.h to /usr/include (depending on CMAKE_INSTALL_PREFIX)
