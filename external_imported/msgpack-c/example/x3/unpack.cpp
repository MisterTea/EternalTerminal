// MessagePack for C++ example
//
// Copyright (C) 2017 KONDO Takatoshi
//
//    Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//    http://www.boost.org/LICENSE_1_0.txt)
//

#include <iostream>
#include <sstream>
#include <cassert>

// MSGPACK_USE_X3_PARSE should be defined before including msgpack.hpp
// It usually defined as a compiler option as -DMSGPACK_USE_X3_PARSE.

#include <msgpack.hpp>

int main() {
    std::stringstream ss;
    std::map<std::string, std::vector<int>> v1 {
        { "ABC", { 1, 2, 3 } },
        { "DEFG", { 4, 5 } }
    };
    std::vector<std::string> v2 {
        "HIJ", "KLM", "NOP"
    };
    msgpack::pack(ss, v1);
    msgpack::pack(ss, v2);

    std::string const& buf = ss.str();
    auto it = buf.begin();
    auto end = buf.end();
    {
        auto oh = msgpack::unpack(it, end);
        // it is updated here.
        assert(v1 == (oh.get().as<std::map<std::string, std::vector<int>>>()));
    }
    {
        auto oh = msgpack::unpack(it, end);
        assert(v2 == oh.get().as<std::vector<std::string>>());
    }
}
