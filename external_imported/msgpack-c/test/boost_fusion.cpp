#include <msgpack.hpp>
#include <sstream>
#include <iterator>
#include <cmath>

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

#include <boost/fusion/adapted/struct/define_struct.hpp>
#include <boost/fusion/adapted/struct/adapt_struct.hpp>

const double kEPS = 1e-10;

BOOST_FUSION_DEFINE_STRUCT(
    BOOST_PP_EMPTY(),
    mystruct,
    (int, f1)
    (double, f2)
)

TEST(MSGPACK_BOOST, fusion_pack_unpack_convert)
{
    std::stringstream ss;
    mystruct val1;
    val1.f1 = 42;
    val1.f2 = 123.45;
    msgpack::pack(ss, val1);
    std::string const& str = ss.str();
    msgpack::object_handle oh =
        msgpack::unpack(str.data(), str.size());
    mystruct val2 = oh.get().as<mystruct>();
    EXPECT_TRUE(val1.f1 == val2.f1);
    EXPECT_TRUE(fabs(val2.f2 - val1.f2) <= kEPS);
}

TEST(MSGPACK_BOOST, object_with_zone_convert)
{
    mystruct val1;
    val1.f1 = 42;
    val1.f2 = 123.45;
    msgpack::zone z;
    msgpack::object obj(val1, z);
    mystruct val2 = obj.as<mystruct>();
    EXPECT_TRUE(val1.f1 == val2.f1);
    EXPECT_TRUE(fabs(val2.f2 - val1.f2) <= kEPS);
}

#if !defined(MSGPACK_USE_CPP03)

struct no_def_con1 {
    no_def_con1() = delete;
    no_def_con1(int i):i(i) {}
    int i;
    MSGPACK_DEFINE(i);
};

inline bool operator==(no_def_con1 const& lhs, no_def_con1 const& rhs) {
    return lhs.i == rhs.i;
}

inline bool operator!=(no_def_con1 const& lhs, no_def_con1 const& rhs) {
    return !(lhs == rhs);
}

struct no_def_con2 {
    no_def_con2() = delete;
    no_def_con2(int i):i(i) {}
    int i;
    MSGPACK_DEFINE(i);
};

inline bool operator==(no_def_con2 const& lhs, no_def_con2 const& rhs) {
    return lhs.i == rhs.i;
}

inline bool operator!=(no_def_con2 const& lhs, no_def_con2 const& rhs) {
    return !(lhs == rhs);
}


namespace msgpack {
MSGPACK_API_VERSION_NAMESPACE(MSGPACK_DEFAULT_API_NS) {
    namespace adaptor {

    template <>
    struct as<no_def_con1> {
        no_def_con1 operator()(msgpack::object const& o) const {
            if (o.type != msgpack::type::ARRAY) throw msgpack::type_error();
            if (o.via.array.size != 1) throw msgpack::type_error();
            return no_def_con1(o.via.array.ptr[0].as<int>());
        }
    };

    template <>
    struct as<no_def_con2> {
        no_def_con2 operator()(msgpack::object const& o) const {
            if (o.type != msgpack::type::ARRAY) throw msgpack::type_error();
            if (o.via.array.size != 1) throw msgpack::type_error();
            return no_def_con2(o.via.array.ptr[0].as<int>());
        }
    };

    } // adaptor
} // MSGPACK_API_VERSION_NAMESPACE(MSGPACK_DEFAULT_API_NS)
} // msgpack

struct mystruct_no_def_con {
    mystruct_no_def_con() = delete;
    // Constructor that have parameters corresponding to BOOST_FUSION_ADAPT_STRUCT is mandatory.
    // See *1, *2, and *3
    mystruct_no_def_con(
        no_def_con1 i,
        no_def_con2 j,
        no_def_con1 k):
        f1(std::move(i)),
        f2(std::move(j)),
        f3(std::move(k)) {}

    no_def_con1 f1;
    no_def_con2 f2;
    no_def_con1 f3;
};

inline bool operator==(mystruct_no_def_con const& lhs, mystruct_no_def_con const& rhs) {
    return lhs.f1 == rhs.f1 && lhs.f2 == rhs.f2 && lhs.f3 == rhs.f3;
}

inline bool operator!=(mystruct_no_def_con const& lhs, mystruct_no_def_con const& rhs) {
    return !(lhs == rhs);
}

BOOST_FUSION_ADAPT_STRUCT(
    mystruct_no_def_con,
    f1, // *1
    f2, // *2
    f3  // *3
)


// MSVC2015's std::tuple requires default constructor during 'as' process.
// It doesn't support Expression SFINAE yet, then 'as' is fallbacked to 'convert'.
// After MSVC would support Expression SFINAE, remove this guard.
#if !defined(_MSC_VER)

TEST(MSGPACK_BOOST, pack_convert_no_def_con)
{
    std::stringstream ss;
    mystruct_no_def_con val1(no_def_con1(1), no_def_con2(2), no_def_con1(3));
    msgpack::pack(ss, val1);
    std::string const& str = ss.str();
    msgpack::object_handle oh =
        msgpack::unpack(str.data(), str.size());
    mystruct_no_def_con val2 = oh.get().as<mystruct_no_def_con>();
    EXPECT_TRUE(val1 == val2);
}

#endif // !defined(_MSC_VER)

struct mystruct_no_def_con_def_con {
    mystruct_no_def_con_def_con() = delete;
    // Constructor that have parameters corresponding to BOOST_FUSION_ADAPT_STRUCT is mandatory.
    // See *1, *2, and *3
    mystruct_no_def_con_def_con(
        no_def_con1 i,
        no_def_con2 j,
        int k):
        f1(std::move(i)),
        f2(std::move(j)),
        f3(std::move(k)) {}

    no_def_con1 f1;
    no_def_con2 f2;
    int f3;
};

inline bool operator==(mystruct_no_def_con_def_con const& lhs, mystruct_no_def_con_def_con const& rhs) {
    return lhs.f1 == rhs.f1 && lhs.f2 == rhs.f2 && lhs.f3 == rhs.f3;
}

inline bool operator!=(mystruct_no_def_con_def_con const& lhs, mystruct_no_def_con_def_con const& rhs) {
    return !(lhs == rhs);
}

BOOST_FUSION_ADAPT_STRUCT(
    mystruct_no_def_con_def_con,
    f1, // *1
    f2, // *2
    f3  // *3
)


// MSVC2015's std::tuple requires default constructor during 'as' process.
// It doesn't support Expression SFINAE yet, then 'as' is fallbacked to 'convert'.
// After MSVC would support Expression SFINAE, remove this guard.
#if !defined(_MSC_VER)

TEST(MSGPACK_BOOST, pack_convert_no_def_con_def_con)
{
    std::stringstream ss;
    mystruct_no_def_con_def_con val1(no_def_con1(1), no_def_con2(2), 3);
    msgpack::pack(ss, val1);
    std::string const& str = ss.str();
    msgpack::object_handle oh =
        msgpack::unpack(str.data(), str.size());
    mystruct_no_def_con_def_con val2 = oh.get().as<mystruct_no_def_con_def_con>();
    EXPECT_TRUE(val1 == val2);
}

#endif // !defined(_MSC_VER)

#endif // !defined(MSGPACK_USE_CPP03

#include <boost/fusion/include/std_pair.hpp>

TEST(MSGPACK_BOOST, fusion_pack_unpack_convert_pair)
{
    std::stringstream ss;
    std::pair<bool, int> val1(false, 42);
    msgpack::pack(ss, val1);
    std::string const& str = ss.str();
    msgpack::object_handle oh =
        msgpack::unpack(str.data(), str.size());
    std::pair<bool, int>  val2 = oh.get().as<std::pair<bool, int> >();
    EXPECT_TRUE(val1.first == val2.first);
    EXPECT_TRUE(val1.second == val2.second);
}

#if !defined(MSGPACK_USE_CPP03)

#include <boost/fusion/include/std_tuple.hpp>

TEST(MSGPACK_BOOST, fusion_pack_unpack_convert_tuple)
{
    std::stringstream ss;
    std::tuple<bool, int> val1(false, 42);
    msgpack::pack(ss, val1);
    std::string const& str = ss.str();
    msgpack::object_handle oh =
        msgpack::unpack(str.data(), str.size());
    std::tuple<bool, int> val2 = oh.get().as<std::tuple<bool, int> >();
    EXPECT_TRUE(val1 == val2);
}

#endif // !defined(MSGPACK_USE_CPP03)

#endif // defined(MSGPACK_USE_BOOST)
