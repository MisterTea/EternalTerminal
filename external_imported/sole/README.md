sole <a href="https://travis-ci.org/r-lyeh/sole"><img src="https://api.travis-ci.org/r-lyeh/sole.svg?branch=master" align="right" /></a>
====

- Sole is a lightweight C++11 library to generate universally unique identificators (UUID).
- Sole provides interface for UUID versions 0, 1 and 4.
- Sole rebuilds UUIDs from hexadecimal and base62 cooked strings as well.
- Sole is tiny, header-only, cross-platform.
- Sole is zlib/libpng licensed.

### Some theory
- UUID version 1 (48-bit MAC address + 60-bit clock with a resolution of 100 ns)
- UUID version 4 (122-bits of randomness)
- Use v1 if you are worried about leaving it up to probabilities.
- Use v4 if you are worried about security issues and determinism.

### About custom version 0
- UUID version 0 (16-bit PID + 48-bit MAC address + 60-bit clock with a resolution of 100ns since Unix epoch)
- Format is EPOCH_LOW-EPOCH_MID-VERSION(0)|EPOCH_HI-PID-MAC

### Public API
- `sole::uuid` 128-bit UUID base type that allows comparison and sorting. `std::ostream <<` friendly. `.str()` to get a cooked hex string. `.base62()` to get a cooked base62 string. `.pretty()` to get a pretty decomposed report.
- `sole::uuid0()` creates an UUID v0.
- `sole::uuid1()` creates an UUID v1.
- `sole::uuid4()` creates an UUID v4.
- `sole::rebuild()` rebuilds an UUID from given string or 64-bit tuple.

### Showcase
```c++
~sole> cat sample.cc
#include <iostream>
#include "sole.hpp"

int main() {
    sole::uuid u0 = sole::uuid0(), u1 = sole::uuid1(), u4 = sole::uuid4();

    std::cout << "uuid v0 string : " << u0 << std::endl;
    std::cout << "uuid v0 base62 : " << u0.base62() << std::endl;
    std::cout << "uuid v0 pretty : " << u0.pretty() << std::endl << std::endl;

    std::cout << "uuid v1 string : " << u1 << std::endl;
    std::cout << "uuid v1 base62 : " << u1.base62() << std::endl;
    std::cout << "uuid v1 pretty : " << u1.pretty() << std::endl << std::endl;

    std::cout << "uuid v4 string : " << u4 << std::endl;
    std::cout << "uuid v4 base62 : " << u4.base62() << std::endl;
    std::cout << "uuid v4 pretty : " << u4.pretty() << std::endl << std::endl;

    u1 = sole::rebuild("F81D4FAE-7DEC-11D0-A765-00A0C91E6BF6");
    std::cout << "uuid v1 rebuilt: " << u1 << " -> " << u1.pretty() << std::endl;

    u4 = sole::rebuild("GITheR4tLlg-BagIW20DGja");
    std::cout << "uuid v4 rebuilt: " << u4 << " -> " << u4.pretty() << std::endl;
}

~sole> g++ sample.cc -std=c++11 -lrt && ./a.out
uuid v0 string : 00aed2f9-c5f8-0030-0fd8-00ffb77bd832
uuid v0 base62 : 3dNJHWv0aW-1MKpXy7mEmf
uuid v0 pretty : version=0,timestamp="03/07/2013 12:19:43",mac=00ffb77bd832,pid=4056,

uuid v1 string : 14314b83-e3ca-11e2-8b83-00ffb77bd832
uuid v1 base62 : 1jU2TXBD9t4-BycINxiP5Jh
uuid v1 pretty : version=1,timestamp="03/07/2013 12:19:43",mac=00ffb77bd832,clock_seq=2947,

uuid v4 string : fa237b32-d580-42db-aeb9-b09a1d90067e
uuid v4 base62 : LTTsO5t3jMR-F03eZqkMchC
uuid v4 pretty : version=4,randbits=fa237b32d58002db2eb9b09a1d90067e,

uuid v1 rebuilt : f81d4fae-7dec-11d0-a765-00a0c91e6bf6 -> version=1,timestamp="03/02/1997 18:43:12",mac=00a0c91e6bf6,clock_seq=10085,
uuid v4 rebuilt : bdd55e2f-6f6b-4088-8703-ddedba9456a2 -> version=4,randbits=bdd55e2f6f6b0088703ddedba9456a2,
```

### Special notes
- clang/g++ users: both `-std=c++11` and `-lrt` may be required when compiling `sole.cpp`

### Changelog
- v1.0.1 (2017/05/16): Improve UUID4 and base62 performance; Fix warnings
- v1.0.0 (2016/02/03): Initial semver adherence; Switch to header-only; Remove warnings
