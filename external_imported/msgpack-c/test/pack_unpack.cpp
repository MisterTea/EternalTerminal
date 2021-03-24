#include <msgpack.hpp>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif //defined(__GNUC__)

#include <gtest/gtest.h>

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif //defined(__GNUC__)

#include <sstream>

TEST(pack, num)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);
}


TEST(pack, vector)
{
    msgpack::sbuffer sbuf;
    std::vector<int> vec;
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);
    msgpack::pack(sbuf, vec);
}


TEST(pack, to_ostream)
{
    std::ostringstream stream;
    msgpack::pack(stream, 1);
}


struct myclass {
    myclass() : num(0), str("default") { }

    myclass(int num, const std::string& str) :
        num(num), str(str) { }

    ~myclass() { }

    int num;
    std::string str;

    MSGPACK_DEFINE(num, str);
};


TEST(pack, myclass)
{
    msgpack::sbuffer sbuf;
    myclass m(1, "msgpack");
    msgpack::pack(sbuf, m);
}


TEST(unpack, int_ret_no_offset_no_ref)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);

    msgpack::object_handle oh = msgpack::unpack(sbuf.data(), sbuf.size());
    EXPECT_EQ(1, oh.get().as<int>());
}

TEST(unpack, int_ret_offset_no_ref)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);

    std::size_t off = 0;

    msgpack::object_handle oh = msgpack::unpack(sbuf.data(), sbuf.size(), off);
    EXPECT_EQ(1, oh.get().as<int>());
    EXPECT_EQ(off, sbuf.size());
}

TEST(unpack, int_ret_no_offset_ref)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);
    bool referenced;

    msgpack::object_handle oh = msgpack::unpack(sbuf.data(), sbuf.size(), referenced);
    EXPECT_EQ(1, oh.get().as<int>());
    EXPECT_FALSE(referenced);
}

TEST(unpack, int_ret_offset_ref)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);
    std::size_t off = 0;
    bool referenced;

    msgpack::object_handle oh = msgpack::unpack(sbuf.data(), sbuf.size(), off, referenced);
    EXPECT_EQ(1, oh.get().as<int>());
    EXPECT_FALSE(referenced);
    EXPECT_EQ(off, sbuf.size());
}


TEST(unpack, int_no_offset_no_ref)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);
    msgpack::object_handle oh;

    msgpack::unpack(oh, sbuf.data(), sbuf.size());
    EXPECT_EQ(1, oh.get().as<int>());
}

TEST(unpack, int_offset_no_ref)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);
    msgpack::object_handle oh;

    std::size_t off = 0;

    msgpack::unpack(oh, sbuf.data(), sbuf.size(), off);
    EXPECT_EQ(1, oh.get().as<int>());
    EXPECT_EQ(off, sbuf.size());
}

TEST(unpack, int_no_offset_ref)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);
    msgpack::object_handle oh;
    bool referenced;

    msgpack::unpack(oh, sbuf.data(), sbuf.size(), referenced);
    EXPECT_EQ(1, oh.get().as<int>());
    EXPECT_FALSE(referenced);
}

TEST(unpack, int_offset_ref)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);
    msgpack::object_handle oh;
    std::size_t off = 0;
    bool referenced;

    msgpack::unpack(oh, sbuf.data(), sbuf.size(), off, referenced);
    EXPECT_EQ(1, oh.get().as<int>());
    EXPECT_FALSE(referenced);
    EXPECT_EQ(off, sbuf.size());
}

#if MSGPACK_DEFAULT_API_VERSION == 1

TEST(unpack, int_pointer_off_no_ref)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);
    msgpack::object_handle oh;

    std::size_t off = 0;

    // obsolete
#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif // defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
    msgpack::unpack(&oh, sbuf.data(), sbuf.size(), &off);
#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
#pragma GCC diagnostic pop
#endif // defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
    EXPECT_EQ(1, oh.get().as<int>());
    EXPECT_EQ(off, sbuf.size());
}

TEST(unpack, int_pointer_off_no_ref_explicit)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);
    msgpack::object_handle oh;

    std::size_t off = 0;

    // obsolete
#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif // defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
    msgpack::unpack(&oh, sbuf.data(), sbuf.size(), &off, MSGPACK_NULLPTR);
#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
#pragma GCC diagnostic pop
#endif // defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
    EXPECT_EQ(1, oh.get().as<int>());
    EXPECT_EQ(off, sbuf.size());
}

TEST(unpack, int_pointer_no_off_ref)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);
    msgpack::object_handle oh;
    bool referenced;

    // obsolete
#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif // defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
    msgpack::unpack(&oh, sbuf.data(), sbuf.size(), MSGPACK_NULLPTR, &referenced);
#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
#pragma GCC diagnostic pop
#endif // defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
    EXPECT_EQ(1, oh.get().as<int>());
    EXPECT_FALSE(referenced);
}

TEST(unpack, int_pointer_off_ref)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);
    msgpack::object_handle oh;
    bool referenced;
    std::size_t off = 0;

    // obsolete
#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif // defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
    msgpack::unpack(&oh, sbuf.data(), sbuf.size(), &off, &referenced);
#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
#pragma GCC diagnostic pop
#endif // defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
    EXPECT_EQ(1, oh.get().as<int>());
    EXPECT_EQ(off, sbuf.size());
    EXPECT_FALSE(referenced);
}


TEST(unpack, int_default_null_pointer)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);
    msgpack::object_handle oh;

    // obsolete
#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif // defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
    msgpack::unpack(&oh, sbuf.data(), sbuf.size());
#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
#pragma GCC diagnostic pop
#endif // defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
    EXPECT_EQ(1, oh.get().as<int>());
}

#endif // MSGPACK_DEFAULT_API_VERSION == 1

TEST(unpack, int_zone_no_offset_no_ref)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);

    msgpack::zone z;
    msgpack::object obj = msgpack::unpack(z, sbuf.data(), sbuf.size());
    EXPECT_EQ(1, obj.as<int>());
}

TEST(unpack, int_zone_offset_no_ref)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);

    std::size_t off = 0;

    msgpack::zone z;
    msgpack::object obj = msgpack::unpack(z, sbuf.data(), sbuf.size(), off);
    EXPECT_EQ(1, obj.as<int>());
    EXPECT_EQ(off, sbuf.size());
}

TEST(unpack, int_zone_no_offset_ref)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);
    bool referenced;

    msgpack::zone z;
    msgpack::object obj = msgpack::unpack(z, sbuf.data(), sbuf.size(), referenced);
    EXPECT_EQ(1, obj.as<int>());
    EXPECT_FALSE(referenced);
}

TEST(unpack, int_zone_offset_ref)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);
    std::size_t off = 0;
    bool referenced;

    msgpack::zone z;
    msgpack::object obj = msgpack::unpack(z, sbuf.data(), sbuf.size(), off, referenced);
    EXPECT_EQ(1, obj.as<int>());
    EXPECT_FALSE(referenced);
    EXPECT_EQ(off, sbuf.size());
}


TEST(unpack, sequence)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);
    msgpack::pack(sbuf, 2);
    msgpack::pack(sbuf, 3);

    std::size_t off = 0;

    msgpack::object_handle oh;

    msgpack::unpack(oh, sbuf.data(), sbuf.size(), off);
    EXPECT_EQ(1, oh.get().as<int>());

    msgpack::unpack(oh, sbuf.data(), sbuf.size(), off);
    EXPECT_EQ(2, oh.get().as<int>());

    msgpack::unpack(oh, sbuf.data(), sbuf.size(), off);
    EXPECT_EQ(3, oh.get().as<int>());

    EXPECT_EQ(off, sbuf.size());
}


TEST(unpack, convert_to_object_handle)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);
    msgpack::unpacked msg;

    msgpack::unpack(msg, sbuf.data(), sbuf.size());
    msgpack::object_handle oh(msgpack::move(msg));
    EXPECT_EQ(1, oh.get().as<int>());

}

TEST(unpack, convert_to_object_handle_direct)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);
    msgpack::object_handle oh(msgpack::unpack(sbuf.data(), sbuf.size()));
    EXPECT_EQ(1, oh.get().as<int>());

}

TEST(unpack, convert_to_object_handle_direct_implicit)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);
    msgpack::object_handle oh = msgpack::unpack(sbuf.data(), sbuf.size());
    EXPECT_EQ(1, oh.get().as<int>());

}

TEST(unpack, insufficient_bytes_ref)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 255); // uint8 (2bytes)

    std::size_t off = 0;

    msgpack::object_handle oh;
    try {
        msgpack::unpack(oh, sbuf.data(), 1, off);
        EXPECT_TRUE(false);
    }
    catch (msgpack::insufficient_bytes const&) {
        EXPECT_TRUE(true);
#if MSGPACK_DEFAULT_API_VERSION < 3
        EXPECT_EQ(off, 0u);
#else  // MSGPACK_DEFAULT_API_VERSION < 3
        EXPECT_EQ(1u, off);
#endif // MSGPACK_DEFAULT_API_VERSION < 3
    }
}

TEST(unpack, insufficient_bytes_object_handle)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 255); // uint8 (2bytes)

    std::size_t off = 0;

    try {
        msgpack::object_handle oh(msgpack::unpack(sbuf.data(), 1, off));
        EXPECT_TRUE(false);
    }
    catch (msgpack::insufficient_bytes const&) {
        EXPECT_TRUE(true);
#if MSGPACK_DEFAULT_API_VERSION < 3
        EXPECT_EQ(off, 0u);
#else  // MSGPACK_DEFAULT_API_VERSION < 3
        EXPECT_EQ(1u, off);
#endif // MSGPACK_DEFAULT_API_VERSION < 3
    }
}

TEST(unpack, insufficient_bytes_zone)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 255); // uint8 (2bytes)

    std::size_t off = 0;

    try {
        msgpack::zone z;
        msgpack::unpack(z, sbuf.data(), 1, off);
        EXPECT_TRUE(false);
    }
    catch (msgpack::insufficient_bytes const&) {
        EXPECT_TRUE(true);
#if MSGPACK_DEFAULT_API_VERSION < 3
        EXPECT_EQ(off, 0u);
#else  // MSGPACK_DEFAULT_API_VERSION < 3
        EXPECT_EQ(1u, off);
#endif // MSGPACK_DEFAULT_API_VERSION < 3
    }
}

TEST(unpack, parse_error)
{
    msgpack::sbuffer sbuf;

    char c = '\xc1';
    sbuf.write(&c, 1);

    bool thrown = false;
    msgpack::object_handle oh;
    try {
        msgpack::unpack(oh, sbuf.data(), sbuf.size());
        EXPECT_TRUE(false);
    }
    catch (msgpack::parse_error const&) {
        thrown = true;
    }
    EXPECT_TRUE(thrown);
}

TEST(unpack, returned_parse_error)
{
    msgpack::sbuffer sbuf;

    char c = '\xc1';
    sbuf.write(&c, 1);

    bool thrown = false;
    try {
        msgpack::unpack(sbuf.data(), sbuf.size());
        EXPECT_TRUE(false);
    }
    catch (msgpack::parse_error const&) {
        thrown = true;
    }
    EXPECT_TRUE(thrown);
}

TEST(unpack, zone_parse_error)
{
    msgpack::sbuffer sbuf;

    char c = '\xc1';
    sbuf.write(&c, 1);

    bool thrown = false;
    msgpack::zone z;
    try {
        msgpack::unpack(z, sbuf.data(), sbuf.size());
        EXPECT_TRUE(false);
    }
    catch (msgpack::parse_error const&) {
        thrown = true;
    }
    EXPECT_TRUE(thrown);
}

TEST(unpack, extra_bytes)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);

    msgpack::object_handle oh = msgpack::unpack(sbuf.data(), sbuf.size() + 1);
    EXPECT_EQ(1, oh.get().as<int>());
}

TEST(unpack, zone_extra_bytes)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);

    msgpack::zone z;
    msgpack::object obj = msgpack::unpack(z, sbuf.data(), sbuf.size() + 1);
    EXPECT_EQ(1, obj.as<int>());
}

TEST(unpack, int_off_larger_than_length)
{
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, 1);

    std::size_t off = 2;

    bool thrown = false;
    try {
        msgpack::object_handle oh = msgpack::unpack(sbuf.data(), sbuf.size(), off);
    }
    catch (msgpack::insufficient_bytes const&) {
        thrown = true;
    }
    EXPECT_TRUE(thrown);
    EXPECT_EQ(off, 2u);
}

TEST(unpack, empty_array_fix)
{
    std::string buf;
    buf.push_back(static_cast<char>(0x90u));
    std::size_t off = 0;

    msgpack::object_handle oh = msgpack::unpack(buf.data(), buf.size(), off);
    EXPECT_EQ(oh.get().type, msgpack::type::ARRAY);
    EXPECT_EQ(oh.get().via.array.size, 0u);
    EXPECT_EQ(off, 1u);
}

TEST(unpack, empty_array_16)
{
    std::string buf;
    buf.push_back(static_cast<char>(0xdcu));
    buf.push_back(static_cast<char>(0x00));
    buf.push_back(static_cast<char>(0x00));
    std::size_t off = 0;

    msgpack::object_handle oh = msgpack::unpack(buf.data(), buf.size(), off);
    EXPECT_EQ(oh.get().type, msgpack::type::ARRAY);
    EXPECT_EQ(oh.get().via.array.size, 0u);
    EXPECT_EQ(off, 3u);
}

TEST(unpack, empty_array_32)
{
    std::string buf;
    buf.push_back(static_cast<char>(0xddu));
    buf.push_back(static_cast<char>(0x00));
    buf.push_back(static_cast<char>(0x00));
    buf.push_back(static_cast<char>(0x00));
    buf.push_back(static_cast<char>(0x00));
    std::size_t off = 0;

    msgpack::object_handle oh = msgpack::unpack(buf.data(), buf.size(), off);
    EXPECT_EQ(oh.get().type, msgpack::type::ARRAY);
    EXPECT_EQ(oh.get().via.array.size, 0u);
    EXPECT_EQ(off, 5u);
}

TEST(unpack, empty_map_fix)
{
    std::string buf;
    buf.push_back(static_cast<char>(0x80u));
    std::size_t off = 0;

    msgpack::object_handle oh = msgpack::unpack(buf.data(), buf.size(), off);
    EXPECT_EQ(oh.get().type, msgpack::type::MAP);
    EXPECT_EQ(oh.get().via.map.size, 0u);
    EXPECT_EQ(off, 1u);
}

TEST(unpack, empty_map_16)
{
    std::string buf;
    buf.push_back(static_cast<char>(0xdeu));
    buf.push_back(static_cast<char>(0x00));
    buf.push_back(static_cast<char>(0x00));
    std::size_t off = 0;

    msgpack::object_handle oh = msgpack::unpack(buf.data(), buf.size(), off);
    EXPECT_EQ(oh.get().type, msgpack::type::MAP);
    EXPECT_EQ(oh.get().via.map.size, 0u);
    EXPECT_EQ(off, 3u);
}

TEST(unpack, empty_map_32)
{
    std::string buf;
    buf.push_back(static_cast<char>(0xdfu));
    buf.push_back(static_cast<char>(0x00));
    buf.push_back(static_cast<char>(0x00));
    buf.push_back(static_cast<char>(0x00));
    buf.push_back(static_cast<char>(0x00));
    std::size_t off = 0;

    msgpack::object_handle oh = msgpack::unpack(buf.data(), buf.size(), off);
    EXPECT_EQ(oh.get().type, msgpack::type::MAP);
    EXPECT_EQ(oh.get().via.map.size, 0u);
    EXPECT_EQ(off, 5u);
}
