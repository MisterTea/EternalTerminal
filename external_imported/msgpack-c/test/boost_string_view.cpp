#include <msgpack.hpp>
#include <sstream>
#include <iterator>

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

#if defined(MSGPACK_USE_BOOST)
#if (BOOST_VERSION / 100000) >= 1 && ((BOOST_VERSION / 100) % 1000) >= 61

TEST(MSGPACK_BOOST, pack_convert_string_view)
{
    std::stringstream ss;
    std::string s = "ABC";
    boost::string_view val1(s);

    msgpack::pack(ss, val1);

    msgpack::object_handle oh;
    std::string const& str = ss.str();
    msgpack::unpack(oh, str.data(), str.size());
    boost::string_view val2 = oh.get().as<boost::string_view>();
    EXPECT_TRUE(val1 == val2);
}

TEST(MSGPACK_BOOST, object_strinf_view)
{
    std::string s = "ABC";
    boost::string_view val1(s);
    msgpack::object obj(val1);
    boost::string_view val2 = obj.as<boost::string_view>();
    EXPECT_TRUE(val1 == val2);
}

TEST(MSGPACK_BOOST, object_with_zone_string_view)
{
    msgpack::zone z;
    std::string s = "ABC";
    boost::string_view val1(s);
    msgpack::object obj(val1, z);
    boost::string_view val2 = obj.as<boost::string_view>();
    EXPECT_TRUE(val1 == val2);
}

#endif // (BOOST_VERSION / 100000) >= 1 && ((BOOST_VERSION / 100) % 1000) >= 61
#endif // defined(MSGPACK_USE_BOOST)
