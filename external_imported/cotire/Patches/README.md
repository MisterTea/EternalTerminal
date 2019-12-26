This directory contains patch files to enable cotire for some popular open sources packages that
use CMake as a build system.

For example, to apply Cotire to LLVM 3.0, first copy `cotire.cmake` to a directory on the CMake
module search path (e.g., `llvm-3.0.src/cmake/modules`).

Then apply the corresponding patch:

    $ cd /path/to/llvm-3.0.src
    $ patch -p1 < /path/to/llvm-3.0.src.patch

Then proceed with an out-of-source CMake build:

    $ mkdir build; cd build
    $ cmake ..
    -- The C compiler identification is GNU 4.2.1
    -- The CXX compiler identification is Clang 3.1.0
    ...
    $ make
    [  0%] Generating C unity source lib/Support/cotire/LLVMSupport_C_unity.c
    [  0%] Generating CXX unity source lib/Support/cotire/LLVMSupport_CXX_unity.cxx
    [  0%] Generating CXX prefix header lib/Support/cotire/LLVMSupport_CXX_prefix.hxx
    [  0%] Building CXX precompiled header lib/Support/cotire/LLVMSupport_CXX_prefix.hxx.gch
    ...
