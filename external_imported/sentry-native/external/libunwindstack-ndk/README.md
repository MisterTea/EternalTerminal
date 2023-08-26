# libunwindstack-ndk

This repository contains a patched version of libunwindstack to build for NDK.
It's originally based on the work of ivanarh ([libunwindstack-ndk](https://github.com/ivanarh/libunwindstack-ndk)), but now tracks the changes of [upstream](https://android.googlesource.com/platform/system/unwinding)(https://android.googlesource.com/platform/system/unwinding)..

Because not everything builds on android or is no longer useful (MIPS support,
GNU debug info for symbolication) we removed it from the codebase.
