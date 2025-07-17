#include <msgpack.hpp>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif //defined(__GNUC__)

#include <gtest/gtest.h>

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif //defined(__GNUC__)

template <typename T>
void check_size(size_t size) {
    T v(0);
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, v);
    EXPECT_EQ(size, sbuf.size());
}

TEST(fixint, size)
{
    check_size<msgpack::type::fix_int8>(2);
    check_size<msgpack::type::fix_int16>(3);
    check_size<msgpack::type::fix_int32>(5);
    check_size<msgpack::type::fix_int64>(9);

    check_size<msgpack::type::fix_uint8>(2);
    check_size<msgpack::type::fix_uint16>(3);
    check_size<msgpack::type::fix_uint32>(5);
    check_size<msgpack::type::fix_uint64>(9);
}


template <typename T>
void check_convert() {
    T v1(typename T::value_type(-11));
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, v1);

    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());

    T v2;
    oh.get().convert(v2);

    EXPECT_EQ(v1.get(), v2.get());

    EXPECT_EQ(oh.get(), msgpack::object(T(v1.get())));
}

TEST(fixint, convert)
{
    check_convert<msgpack::type::fix_int8>();
    check_convert<msgpack::type::fix_int16>();
    check_convert<msgpack::type::fix_int32>();
    check_convert<msgpack::type::fix_int64>();

    check_convert<msgpack::type::fix_uint8>();
    check_convert<msgpack::type::fix_uint16>();
    check_convert<msgpack::type::fix_uint32>();
    check_convert<msgpack::type::fix_uint64>();
}
