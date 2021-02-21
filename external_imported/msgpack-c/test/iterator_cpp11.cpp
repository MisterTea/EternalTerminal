#include <msgpack.hpp>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif //defined(__GNUC__)

#include <gtest/gtest.h>

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif //defined(__GNUC__)

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

using namespace std;

#if !defined(MSGPACK_USE_CPP03)
#include <iterator>

// C++11

constexpr unsigned int VECTOR_SIZE = 100;
constexpr unsigned int MAP_SIZE    = 100;

TEST(iterator, vector)
{
    using vec_type = vector<unsigned int>;
    vec_type vec;
    vec.reserve(VECTOR_SIZE);
    for (unsigned int i = 0; i < VECTOR_SIZE; i++) {
        vec.push_back(static_cast<unsigned int>(rand()));
    }
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, vec);

    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());

    auto const& msgarr = oh.get().via.array;
    auto dist = std::distance(begin(msgarr), end(msgarr));
    auto vecSize = vec.size();
    EXPECT_EQ(static_cast<size_t>(dist), vecSize);

    vec_type::const_iterator correct = std::begin(vec);
    for (auto const& obj : msgarr) {
        auto u64 = *correct;
        EXPECT_EQ(obj.as<unsigned int>(), u64);
        ++correct;
    }
}

TEST(iterator, map)
{
    using map_type = map<unsigned int, unsigned int>;
    map_type map;
    for (unsigned int i = 0; i < MAP_SIZE; i++) {
        map[static_cast<unsigned int>(rand())] = static_cast<unsigned int>(rand());
    }
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, map);

    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());

    auto const& msgmap = oh.get().via.map;
    auto dist = std::distance(begin(msgmap), end(msgmap));
    auto mapSize = map.size();
    EXPECT_EQ(static_cast<size_t>(dist), mapSize);

    for (auto const& kv : msgmap) {
        auto key = kv.key.as<unsigned int>();
        auto val = kv.val.as<unsigned int>();
        auto correct = map[key];
        EXPECT_EQ(val, correct);
    }
}

#endif // !defined(MSGPACK_USE_CPP03
