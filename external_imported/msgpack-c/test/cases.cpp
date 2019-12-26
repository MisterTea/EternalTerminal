#include <msgpack.hpp>
#include <fstream>
#include <gtest/gtest.h>

static void feed_file(msgpack::unpacker& pac, const char* path)
{
    std::ifstream fin(path);
    while(true) {
        pac.reserve_buffer(32*1024);
        fin.read(pac.buffer(), pac.buffer_capacity());
        if(fin.bad()) {
            throw std::runtime_error("read failed");
        }
        pac.buffer_consumed(static_cast<size_t>(fin.gcount()));
        if(fin.fail()) {
            break;
        }
    }
}

TEST(cases, format)
{
#if (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 7)) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif // (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 7)) && !defined(__clang__)
    msgpack::unpacker pac;
    msgpack::unpacker pac_compact;

    feed_file(pac, "cases.mpac");
    feed_file(pac_compact, "cases_compact.mpac");

    msgpack::object_handle oh;
    while(pac.next(oh)) {
        msgpack::object_handle oh_compact;
        EXPECT_TRUE( pac_compact.next(oh_compact) );
        EXPECT_EQ(oh_compact.get(), oh.get());
    }

    EXPECT_FALSE( pac_compact.next(oh) );
#if (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 7)) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif // (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 7)) && !defined(__clang__)
}
