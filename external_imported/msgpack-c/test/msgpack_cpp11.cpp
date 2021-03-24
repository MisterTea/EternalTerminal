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

#if !defined(MSGPACK_USE_CPP03)

class TestEnumClassMemberClass
{
public:
    TestEnumClassMemberClass()
        : t1(TestEnumClassType::STATE_A), t2(TestEnumClassType::STATE_B), t3(TestEnumClassType::STATE_C) {}

    enum class TestEnumClassType:long {
        STATE_INVALID = 0,
            STATE_A = 1,
            STATE_B = 2,
            STATE_C = 3
        };
    TestEnumClassType t1;
    TestEnumClassType t2;
    TestEnumClassType t3;

    MSGPACK_DEFINE(t1, t2, t3);
};

MSGPACK_ADD_ENUM(TestEnumClassMemberClass::TestEnumClassType);

using namespace std;

const unsigned int kLoop = 10000;
const unsigned int kElements = 100;


// C++11

TEST(MSGPACK_CPP11, simple_tuple)
{
    msgpack::sbuffer sbuf;
    std::tuple<bool, std::string, double> val1(true, "kzk", 12.3);
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::tuple<bool, std::string, double> val2 = oh.get().as<std::tuple<bool, std::string, double> >();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_CPP11, simple_tuple_empty)
{
    msgpack::sbuffer sbuf;
    std::tuple<> val1;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::tuple<> val2 = oh.get().as<std::tuple<> >();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_CPP11, simple_tuple_size_greater_than_as)
{
    msgpack::sbuffer sbuf;
    std::tuple<bool, std::string, int> val1(true, "kzk", 42);
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::tuple<bool, std::string, double, int> val2 = oh.get().as<std::tuple<bool, std::string, double, int> >();
    EXPECT_EQ(std::get<0>(val1), std::get<0>(val2));
    EXPECT_EQ(std::get<1>(val1), std::get<1>(val2));
    EXPECT_EQ(std::get<2>(val1), std::get<2>(val2));
}

TEST(MSGPACK_CPP11, simple_tuple_size_greater_than_convert)
{
    msgpack::sbuffer sbuf;
    std::tuple<bool, std::string, int> val1(true, "kzk", 42);
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::tuple<bool, std::string, double, int> val2;
    oh.get().convert(val2);
    EXPECT_EQ(std::get<0>(val1), std::get<0>(val2));
    EXPECT_EQ(std::get<1>(val1), std::get<1>(val2));
    EXPECT_EQ(std::get<2>(val1), std::get<2>(val2));
}

TEST(MSGPACK_CPP11, simple_tuple_size_less_than_as)
{
    msgpack::sbuffer sbuf;
    std::tuple<bool, std::string, int> val1(true, "kzk", 42);
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::tuple<bool, std::string> val2 = oh.get().as<std::tuple<bool, std::string> >();
    EXPECT_EQ(std::get<0>(val1), std::get<0>(val2));
    EXPECT_EQ(std::get<1>(val1), std::get<1>(val2));
}

TEST(MSGPACK_CPP11, simple_tuple_size_less_than_convert)
{
    msgpack::sbuffer sbuf;
    std::tuple<bool, std::string, int> val1(true, "kzk", 42);
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::tuple<bool, std::string> val2;
    oh.get().convert(val2);
    EXPECT_EQ(std::get<0>(val1), std::get<0>(val2));
    EXPECT_EQ(std::get<1>(val1), std::get<1>(val2));
}

TEST(MSGPACK_CPP11, simple_array)
{
    for (unsigned int k = 0; k < kLoop; k++) {
        array<int, kElements> val1;
        for (unsigned int i = 0; i < kElements; i++)
            val1[i] = rand();
        msgpack::sbuffer sbuf;
        msgpack::pack(sbuf, val1);
        msgpack::object_handle oh =
            msgpack::unpack(sbuf.data(), sbuf.size());
        EXPECT_EQ(oh.get().type, msgpack::type::ARRAY);
        array<int, kElements> val2 = oh.get().as<array<int, kElements> >();
        EXPECT_EQ(val1.size(), val2.size());
        EXPECT_TRUE(equal(val1.begin(), val1.end(), val2.begin()));
    }
}

TEST(MSGPACK_CPP11, simple_array_empty)
{
    array<int, 0> val1;
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    EXPECT_EQ(oh.get().type, msgpack::type::ARRAY);
    array<int, 0> val2 = oh.get().as<array<int, 0> >();
    EXPECT_EQ(val1.size(), val2.size());
    EXPECT_TRUE(equal(val1.begin(), val1.end(), val2.begin()));
}

TEST(MSGPACK_CPP11, simple_array_size_less_than)
{
    array<int, 2> val1 { {1 , 2} };
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    EXPECT_EQ(oh.get().type, msgpack::type::ARRAY);
    array<int, 1> val2;
    try {
        oh.get().convert(val2);
        EXPECT_TRUE(false);
    }
    catch (msgpack::type_error const&) {
        EXPECT_TRUE(true);
    }
}

TEST(MSGPACK_CPP11, simple_array_size_greater_than)
{
    array<int, 2> val1 { {1 , 2} };
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    EXPECT_EQ(oh.get().type, msgpack::type::ARRAY);
    array<int, 3> val2;
    oh.get().convert(val2);
    EXPECT_EQ(val1[0], val2[0]);
    EXPECT_EQ(val1[1], val2[1]);
}

TEST(MSGPACK_CPP11, simple_buffer_array_char)
{
    for (unsigned int k = 0; k < kLoop; k++) {
        array<char, kElements> val1;
        for (unsigned int i = 0; i < kElements; i++)
            val1[i] = static_cast<char>(rand());
        msgpack::sbuffer sbuf;
        msgpack::pack(sbuf, val1);
        msgpack::object_handle oh =
            msgpack::unpack(sbuf.data(), sbuf.size());
        EXPECT_EQ(oh.get().type, msgpack::type::BIN);
        array<char, kElements> val2 = oh.get().as<array<char, kElements> >();
        EXPECT_EQ(val1.size(), val2.size());
        EXPECT_TRUE(equal(val1.begin(), val1.end(), val2.begin()));
    }
}

TEST(MSGPACK_CPP11, simple_buffer_array_char_empty)
{
    array<char, 0> val1;
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    EXPECT_EQ(oh.get().type, msgpack::type::BIN);
    array<char, 0> val2 = oh.get().as<array<char, 0> >();
    EXPECT_EQ(val1.size(), val2.size());
    EXPECT_TRUE(equal(val1.begin(), val1.end(), val2.begin()));
}

TEST(MSGPACK_CPP11, simple_buffer_array_unsigned_char)
{
    if (!msgpack::is_same<uint8_t, unsigned char>::value) return;
    for (unsigned int k = 0; k < kLoop; k++) {
        array<unsigned char, kElements> val1;
        for (unsigned int i = 0; i < kElements; i++)
            val1[i] = static_cast<unsigned char>(rand());
        msgpack::sbuffer sbuf;
        msgpack::pack(sbuf, val1);
        msgpack::object_handle oh =
            msgpack::unpack(sbuf.data(), sbuf.size());
        EXPECT_EQ(oh.get().type, msgpack::type::BIN);
        array<unsigned char, kElements> val2 = oh.get().as<array<unsigned char, kElements> >();
        EXPECT_EQ(val1.size(), val2.size());
        EXPECT_TRUE(equal(val1.begin(), val1.end(), val2.begin()));
    }
}

TEST(MSGPACK_CPP11, simple_buffer_array_unsigned_char_empty)
{
    if (!msgpack::is_same<uint8_t, unsigned char>::value) return;
    array<unsigned char, 0> val1;
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    EXPECT_EQ(oh.get().type, msgpack::type::BIN);
    array<unsigned char, 0> val2 = oh.get().as<array<unsigned char, 0> >();
    EXPECT_EQ(val1.size(), val2.size());
    EXPECT_TRUE(equal(val1.begin(), val1.end(), val2.begin()));
}

// strong typedefs
namespace test {

template <class Key>
struct hash : std::hash<Key> {
    using std::hash<Key>::hash;
};

template <class Key>
struct equal_to : std::equal_to<Key> {
    using std::equal_to<Key>::equal_to;
};

template <class Key>
struct set_allocator : std::allocator<Key> {
    using std::allocator<Key>::allocator;
};

template <class Key, class T>
struct map_allocator : std::allocator<std::pair<const Key, T>> {
    using std::allocator<std::pair<const Key, T>>::allocator;
};

template <class T>
struct allocator : std::allocator<T> {
    using std::allocator<T>::allocator;
};

} // namespace test


TEST(MSGPACK_STL, simple_buffer_forward_list)
{
    using type = forward_list<int, test::allocator<int>>;
    for (unsigned int k = 0; k < kLoop; k++) {
        type val1;
        for (unsigned int i = 0; i < kElements; i++)
            val1.push_front(rand());
        msgpack::sbuffer sbuf;
        msgpack::pack(sbuf, val1);
        msgpack::object_handle oh =
            msgpack::unpack(sbuf.data(), sbuf.size());
        type val2 = oh.get().as<type >();
        EXPECT_EQ(val1, val2);
    }
}

TEST(MSGPACK_STL, simple_buffer_forward_list_empty)
{
    using type = forward_list<int, test::allocator<int>>;
    type val1;
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    type val2 = oh.get().as<type >();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_STL, simple_buffer_unordered_map)
{
    using type = unordered_map<int, int, test::hash<int>, test::equal_to<int>, test::map_allocator<int, int>>;
    for (unsigned int k = 0; k < kLoop; k++) {
        type val1;
        for (unsigned int i = 0; i < kElements; i++)
            val1[rand()] = rand();
        msgpack::sbuffer sbuf;
        msgpack::pack(sbuf, val1);
        msgpack::object_handle oh =
            msgpack::unpack(sbuf.data(), sbuf.size());
        type val2 = oh.get().as<type >();
        EXPECT_EQ(val1, val2);
    }
}

TEST(MSGPACK_STL, simple_buffer_unordered_map_empty)
{
    using type = unordered_map<int, int, test::hash<int>, test::equal_to<int>, test::map_allocator<int, int>>;
    type val1;
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    type val2 = oh.get().as<type >();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_STL, simple_buffer_unordered_multimap)
{
    using type = unordered_multimap<int, int, test::hash<int>, test::equal_to<int>, test::map_allocator<int, int>>;
    for (unsigned int k = 0; k < kLoop; k++) {
        type val1;
        for (unsigned int i = 0; i < kElements; i++) {
            int i1 = rand();
            val1.insert(make_pair(i1, rand()));
            val1.insert(make_pair(i1, rand()));
        }
        msgpack::sbuffer sbuf;
        msgpack::pack(sbuf, val1);
        msgpack::object_handle oh =
            msgpack::unpack(sbuf.data(), sbuf.size());
        type val2 = oh.get().as<type >();

        EXPECT_EQ(val1, val2);
    }
}

TEST(MSGPACK_STL, simple_buffer_unordered_multimap_empty)
{
    using type = unordered_multimap<int, int, test::hash<int>, test::equal_to<int>, test::map_allocator<int, int>>;
    type val1;
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    type val2 = oh.get().as<type >();

    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_STL, simple_buffer_unordered_set)
{
    using type = unordered_set<int, test::hash<int>, test::equal_to<int>, test::set_allocator<int>>;
    for (unsigned int k = 0; k < kLoop; k++) {
        type val1;
        for (unsigned int i = 0; i < kElements; i++)
            val1.insert(rand());
        msgpack::sbuffer sbuf;
        msgpack::pack(sbuf, val1);
        msgpack::object_handle oh =
            msgpack::unpack(sbuf.data(), sbuf.size());
        type val2 = oh.get().as<type>();
        EXPECT_EQ(val1, val2);
    }
}

TEST(MSGPACK_STL, simple_buffer_unordered_set_empty)
{
    using type = unordered_set<int, test::hash<int>, test::equal_to<int>, test::set_allocator<int>>;
    type val1;
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    type val2 = oh.get().as<type>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_STL, simple_buffer_unordered_multiset)
{
    using type = unordered_multiset<int, test::hash<int>, test::equal_to<int>, test::set_allocator<int>>;
    for (unsigned int k = 0; k < kLoop; k++) {
        type val1;
        for (unsigned int i = 0; i < kElements; i++)
            val1.insert(rand());
        msgpack::sbuffer sbuf;
        msgpack::pack(sbuf, val1);
        msgpack::object_handle oh =
            msgpack::unpack(sbuf.data(), sbuf.size());
        type val2 = oh.get().as<type >();
        EXPECT_EQ(val1, val2);
    }
}

TEST(MSGPACK_STL, simple_buffer_unordered_multiset_empty)
{
    using type = unordered_multiset<int, test::hash<int>, test::equal_to<int>, test::set_allocator<int>>;
    type val1;
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    type val2 = oh.get().as<type >();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_USER_DEFINED, simple_buffer_enum_class_member)
{
    TestEnumClassMemberClass val1;
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    TestEnumClassMemberClass val2 = oh.get().as<TestEnumClassMemberClass>();
    EXPECT_EQ(val1.t1, val2.t1);
    EXPECT_EQ(val1.t2, val2.t2);
    EXPECT_EQ(val1.t3, val2.t3);
}

struct no_def_con {
    no_def_con() = delete;
    no_def_con(int i):i(i) {}
    int i;
    MSGPACK_DEFINE(i);
};

inline bool operator==(no_def_con const& lhs, no_def_con const& rhs) {
    return lhs.i == rhs.i;
}

inline bool operator!=(no_def_con const& lhs, no_def_con const& rhs) {
    return !(lhs == rhs);
}

inline bool operator<(no_def_con const& lhs, no_def_con const& rhs) {
    return lhs.i <  rhs.i;
}

namespace msgpack {
MSGPACK_API_VERSION_NAMESPACE(MSGPACK_DEFAULT_API_NS) {
    namespace adaptor {
    template <>
    struct as<no_def_con> {
        no_def_con operator()(msgpack::object const& o) const {
            if (o.type != msgpack::type::ARRAY) throw msgpack::type_error();
            if (o.via.array.size != 1) throw msgpack::type_error();
            return no_def_con(o.via.array.ptr[0].as<int>());
        }
    };
    } // adaptor
} // MSGPACK_API_VERSION_NAMESPACE(MSGPACK_DEFAULT_API_NS)
} // msgpack

namespace std {
template <> struct hash<no_def_con> {
    size_t operator()(const no_def_con & x) const {
        return hash<int>()(x.i);
    }
};
} // std

TEST(MSGPACK_NO_DEF_CON, simple_buffer)
{
    no_def_con val1(42);
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());

    no_def_con val2 = oh.get().as<no_def_con>();
    EXPECT_EQ(val1, val2);
}

struct no_def_con_composite {
    no_def_con_composite() = delete;
    no_def_con_composite(int i):ndc(i) {}
    no_def_con_composite(no_def_con const& a):ndc(a) {}
    no_def_con ndc;
    MSGPACK_DEFINE(ndc);
};

inline bool operator==(no_def_con_composite const& lhs, no_def_con_composite const& rhs) {
    return lhs.ndc == rhs.ndc;
}

inline bool operator!=(no_def_con_composite const& lhs, no_def_con_composite const& rhs) {
    return !(lhs == rhs);
}

inline bool operator<(no_def_con_composite const& lhs, no_def_con_composite const& rhs) {
    return lhs.ndc < rhs.ndc;
}

namespace msgpack {
MSGPACK_API_VERSION_NAMESPACE(MSGPACK_DEFAULT_API_NS) {
    namespace adaptor {
    template <>
    struct as<no_def_con_composite> {
        no_def_con_composite operator()(msgpack::object const& o) const {
            if (o.type != msgpack::type::ARRAY) throw msgpack::type_error();
            if (o.via.array.size != 1) throw msgpack::type_error();
            return no_def_con_composite(o.via.array.ptr[0].as<no_def_con>());
        }
    };
    } // adaptor
} // MSGPACK_API_VERSION_NAMESPACE(MSGPACK_DEFAULT_API_NS)
} // msgpack

TEST(MSGPACK_NO_DEF_CON_COMPOSITE, simple_buffer)
{
    no_def_con_composite val1(42);
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    no_def_con_composite val2 = oh.get().as<no_def_con_composite>();
    EXPECT_EQ(val1, val2);
}

struct no_def_con_inherit : no_def_con {
    no_def_con_inherit() = delete;
    no_def_con_inherit(no_def_con const& a):no_def_con(a) {}
    MSGPACK_DEFINE(MSGPACK_BASE(no_def_con));
};

namespace msgpack {
MSGPACK_API_VERSION_NAMESPACE(MSGPACK_DEFAULT_API_NS) {
    namespace adaptor {
    template <>
    struct as<no_def_con_inherit> {
        no_def_con_inherit operator()(msgpack::object const& o) const {
            if (o.type != msgpack::type::ARRAY) throw msgpack::type_error();
            if (o.via.array.size != 1) throw msgpack::type_error();
            return no_def_con_inherit(o.via.array.ptr[0].as<no_def_con>());
        }
    };
    } // adaptor
} // MSGPACK_API_VERSION_NAMESPACE(MSGPACK_DEFAULT_API_NS)
} // msgpack

TEST(MSGPACK_NO_DEF_CON_INHERIT, simple_buffer)
{
    no_def_con_inherit val1(42);
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    no_def_con_inherit val2 = oh.get().as<no_def_con_inherit>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_VECTOR, simple_buffer)
{
    std::vector<no_def_con> val1 { 1, 2, 3 };
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::vector<no_def_con> val2 = oh.get().as<std::vector<no_def_con>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_LIST, simple_buffer)
{
    std::list<no_def_con> val1 { 1, 2, 3 };
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::list<no_def_con> val2 = oh.get().as<std::list<no_def_con>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_SET, simple_buffer)
{
    std::set<no_def_con> val1 { 1, 2, 3 };
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::set<no_def_con> val2 = oh.get().as<std::set<no_def_con>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_MULTISET, simple_buffer)
{
    std::multiset<no_def_con> val1 { 1, 2, 3 };
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::multiset<no_def_con> val2 = oh.get().as<std::multiset<no_def_con>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_ASSOC_VECTOR, simple_buffer)
{
    msgpack::type::assoc_vector<no_def_con, no_def_con_composite> val1 { {1, 2}, {3, 4}, {5, 6}};
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    msgpack::type::assoc_vector<no_def_con, no_def_con_composite> val2
        = oh.get().as<msgpack::type::assoc_vector<no_def_con, no_def_con_composite>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_DEF_CON_ASSOC_VECTOR, simple_buffer)
{
    msgpack::type::assoc_vector<no_def_con, int> val1 { {1, 2}, {3, 4}, {5, 6}};
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    msgpack::type::assoc_vector<no_def_con, int> val2
        = oh.get().as<msgpack::type::assoc_vector<no_def_con, int>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_MAP, simple_buffer)
{
    std::map<no_def_con, no_def_con_composite> val1 { {1, 2}, {3, 4}, {5, 6}};
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::map<no_def_con, no_def_con_composite> val2
        = oh.get().as<std::map<no_def_con, no_def_con_composite>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_DEF_CON_MAP, simple_buffer)
{
    std::map<no_def_con, int> val1 { {1, 2}, {3, 4}, {5, 6}};
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::map<no_def_con, int> val2
        = oh.get().as<std::map<no_def_con, int>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_MULTIMAP, simple_buffer)
{
    std::multimap<no_def_con, no_def_con_composite> val1 { {1, 2}, {3, 4}, {5, 6}};
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::multimap<no_def_con, no_def_con_composite> val2
        = oh.get().as<std::multimap<no_def_con, no_def_con_composite>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_DEF_CON_MULTIMAP, simple_buffer)
{
    std::multimap<no_def_con, int> val1 { {1, 2}, {3, 4}, {5, 6}};
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::multimap<no_def_con, int> val2
        = oh.get().as<std::multimap<no_def_con, int>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_DEQUE, simple_buffer)
{
    std::deque<no_def_con> val1 { 1, 2, 3 };
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::deque<no_def_con> val2 = oh.get().as<std::deque<no_def_con>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_PAIR, simple_buffer)
{
    std::pair<no_def_con, no_def_con_composite> val1 {1, 2};
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::pair<no_def_con, no_def_con_composite> val2
        = oh.get().as<std::pair<no_def_con, no_def_con_composite>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_DEF_CON_PAIR, simple_buffer)
{
    std::pair<no_def_con, int> val1 {1, 2};
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::pair<no_def_con, int> val2
        = oh.get().as<std::pair<no_def_con, int>>();
    EXPECT_EQ(val1, val2);
}



// MSVC2015's std::tuple requires default constructor during 'as' process.
// It doesn't support Expression SFINAE yet, then 'as' is fallbacked to 'convert'.
// After MSVC would support Expression SFINAE, remove this guard.
#if !defined(_MSC_VER)

TEST(MSGPACK_NO_DEF_CON_TUPLE, simple_buffer)
{
    std::tuple<no_def_con, no_def_con, no_def_con_composite> val1 {1, 2, 3};
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::tuple<no_def_con, no_def_con, no_def_con_composite> val2
        = oh.get().as<std::tuple<no_def_con, no_def_con, no_def_con_composite>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_DEF_CON_TUPLE, simple_buffer)
{
    std::tuple<no_def_con, no_def_con, int> val1 {1, 2, 3};
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::tuple<no_def_con, no_def_con, int> val2
        = oh.get().as<std::tuple<no_def_con, no_def_con, int>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_MSGPACK_TUPLE, simple_buffer)
{
    msgpack::type::tuple<no_def_con, no_def_con, no_def_con_composite> val1 {1, 2, 3};
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    msgpack::type::tuple<no_def_con, no_def_con, no_def_con_composite> val2
        = oh.get().as<msgpack::type::tuple<no_def_con, no_def_con, no_def_con_composite>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_DEF_CON_MSGPACK_TUPLE, simple_buffer)
{
    msgpack::type::tuple<no_def_con, no_def_con, int> val1 {1, 2, 3};
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    msgpack::type::tuple<no_def_con, no_def_con, int> val2
        = oh.get().as<msgpack::type::tuple<no_def_con, no_def_con, int>>();
    EXPECT_EQ(val1, val2);
}

#endif // !define(_MSC_VER)

TEST(MSGPACK_NO_DEF_FORWARD_LIST, simple_buffer)
{
    std::forward_list<no_def_con> val1 { 1, 2, 3 };
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::forward_list<no_def_con> val2 = oh.get().as<std::forward_list<no_def_con>>();
    EXPECT_TRUE(val1 == val2);
}

TEST(MSGPACK_NO_DEF_CON_UNORDERED_SET, simple_buffer)
{
    std::unordered_set<no_def_con> val1 { 1, 2, 3 };
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::unordered_set<no_def_con> val2 = oh.get().as<std::unordered_set<no_def_con>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_UNORDERED_MULTISET, simple_buffer)
{
    std::unordered_multiset<no_def_con> val1 { 1, 2, 3 };
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::unordered_multiset<no_def_con> val2 = oh.get().as<std::unordered_multiset<no_def_con>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_UNORDERED_MAP, simple_buffer)
{
    std::unordered_map<no_def_con, no_def_con_composite> val1 { {1, 2}, {3, 4}, {5, 6}};
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::unordered_map<no_def_con, no_def_con_composite> val2
        = oh.get().as<std::unordered_map<no_def_con, no_def_con_composite>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_DEF_CON_UNORDERED_MAP, simple_buffer)
{
    std::unordered_map<no_def_con, int> val1 { {1, 2}, {3, 4}, {5, 6}};
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::unordered_map<no_def_con, int> val2
        = oh.get().as<std::unordered_map<no_def_con, int>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_UNORDERED_MULTIMAP, simple_buffer)
{
    std::unordered_multimap<no_def_con, no_def_con_composite> val1 { {1, 2}, {3, 4}, {5, 6}};
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::unordered_multimap<no_def_con, no_def_con_composite> val2
        = oh.get().as<std::unordered_multimap<no_def_con, no_def_con_composite>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_DEF_CON_UNORDERED_MULTIMAP, simple_buffer)
{
    std::unordered_multimap<no_def_con, int> val1 { {1, 2}, {3, 4}, {5, 6}};
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::unordered_multimap<no_def_con, int> val2
        = oh.get().as<std::unordered_multimap<no_def_con, int>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_NO_DEF_CON_ARRAY, simple_buffer)
{
    std::array<no_def_con, 3> val1 { { 1, 2, 3 } };
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::array<no_def_con, 3> val2 = oh.get().as<std::array<no_def_con, 3>>();
    EXPECT_EQ(val1, val2);
}

TEST(MSGPACK_CHRONO, system_clock)
{
    std::chrono::system_clock::time_point val1;
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);

    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::chrono::system_clock::time_point val2 = oh.get().as<std::chrono::system_clock::time_point>();
    EXPECT_EQ(val1, val2);

    std::chrono::system_clock::time_point val3;
    oh.get().convert(val3);
    EXPECT_EQ(val1, val3);
}

TEST(MSGPACK_CHRONO, system_clock_32)
{
    std::chrono::system_clock::time_point val1(std::chrono::seconds(0x12345678L));
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    char packed[] = {
        static_cast<char>(0xd6u),
        static_cast<char>(-1),
        static_cast<char>(0x12),
        static_cast<char>(0x34),
        static_cast<char>(0x56),
        static_cast<char>(0x78)
    };
    EXPECT_EQ(memcmp(sbuf.data(), packed, sizeof(packed)), 0);

    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::chrono::system_clock::time_point val2 = oh.get().as<std::chrono::system_clock::time_point>();
    EXPECT_EQ(val1, val2);

    std::chrono::system_clock::time_point val3;
    oh.get().convert(val3);
    EXPECT_EQ(val1, val3);
}

TEST(MSGPACK_CHRONO, system_clock_32_max)
{
    std::chrono::system_clock::time_point val1(std::chrono::seconds(0xffffffffL));
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    char packed[] = {
        static_cast<char>(0xd6u),
        static_cast<char>(-1),
        static_cast<char>(0xffu),
        static_cast<char>(0xffu),
        static_cast<char>(0xffu),
        static_cast<char>(0xffu)
    };
    EXPECT_EQ(memcmp(sbuf.data(), packed, sizeof(packed)), 0);

    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::chrono::system_clock::time_point val2 = oh.get().as<std::chrono::system_clock::time_point>();
    EXPECT_EQ(val1, val2);

    std::chrono::system_clock::time_point val3;
    oh.get().convert(val3);
    EXPECT_EQ(val1, val3);
}


TEST(MSGPACK_CHRONO, system_clock_64)
{
    std::chrono::system_clock::time_point val1(std::chrono::seconds(0x31234567L));
    val1 +=
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::nanoseconds(0x312345678L)
        );
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);

    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::chrono::system_clock::time_point val2 = oh.get().as<std::chrono::system_clock::time_point>();
    EXPECT_EQ(val1, val2);

    std::chrono::system_clock::time_point val3;
    oh.get().convert(val3);
    EXPECT_EQ(val1, val3);
}

TEST(MSGPACK_CHRONO, system_clock_64_max)
{
    std::chrono::system_clock::time_point val1(std::chrono::seconds(0xffffffffL));

    val1 +=
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::nanoseconds(0x3b9ac9ffL) // 999,999,999
        );
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);
    char packed_nano[] = {
        static_cast<char>(0xd7u),
        static_cast<char>(-1),
        static_cast<char>(0xeeu), // 999,999,999 << 2
        static_cast<char>(0x6b),
        static_cast<char>(0x27),
        static_cast<char>(0xfcu),
        static_cast<char>(0xffu), // 32 bit sec
        static_cast<char>(0xffu),
        static_cast<char>(0xffu),
        static_cast<char>(0xffu)
    };
    char packed_micro[] = {
        static_cast<char>(0xd7u),
        static_cast<char>(-1),
        static_cast<char>(0xeeu), // 999,999,000 << 2
        static_cast<char>(0x6b),
        static_cast<char>(0x18),
        static_cast<char>(0x60),
        static_cast<char>(0xffu), // 32 bit sec
        static_cast<char>(0xffu),
        static_cast<char>(0xffu),
        static_cast<char>(0xffu)
    };
    if (std::chrono::system_clock::duration::period::ratio::den == 1000000000) {
        EXPECT_EQ(memcmp(sbuf.data(), packed_nano, sizeof(packed_nano)), 0);
    }
    else if (std::chrono::system_clock::duration::period::ratio::den == 1000000) {
        EXPECT_EQ(memcmp(sbuf.data(), packed_micro, sizeof(packed_micro)), 0);
    }

    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::chrono::system_clock::time_point val2 = oh.get().as<std::chrono::system_clock::time_point>();
    EXPECT_EQ(val1, val2);

    std::chrono::system_clock::time_point val3;
    oh.get().convert(val3);
    EXPECT_EQ(val1, val3);
}

TEST(MSGPACK_CHRONO, system_clock_impl_min)
{
    std::chrono::system_clock::time_point val1(std::chrono::system_clock::time_point::min());
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);

    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::chrono::system_clock::time_point val2 = oh.get().as<std::chrono::system_clock::time_point>();
    EXPECT_EQ(val1, val2);

    std::chrono::system_clock::time_point val3;
    oh.get().convert(val3);
    EXPECT_EQ(val1, val3);
}

TEST(MSGPACK_CHRONO, system_clock_impl_max)
{
    std::chrono::system_clock::time_point val1(std::chrono::system_clock::time_point::max());

    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);

    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::chrono::system_clock::time_point val2 = oh.get().as<std::chrono::system_clock::time_point>();
    EXPECT_EQ(val1, val2);

    std::chrono::system_clock::time_point val3;
    oh.get().convert(val3);
    EXPECT_EQ(val1, val3);
}

TEST(MSGPACK_CHRONO, system_clock_impl_now)
{
    std::chrono::system_clock::time_point val1(std::chrono::system_clock::now());

    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, val1);

    msgpack::object_handle oh =
        msgpack::unpack(sbuf.data(), sbuf.size());
    std::chrono::system_clock::time_point val2 = oh.get().as<std::chrono::system_clock::time_point>();
    EXPECT_EQ(val1, val2);

    std::chrono::system_clock::time_point val3;
    oh.get().convert(val3);
    EXPECT_EQ(val1, val3);
}

TEST(MSGPACK_TIMESPEC, timespec_pack_convert_zero)
{
    std::stringstream ss;
    timespec val1{ 0, 0 };

    msgpack::pack(ss, val1);
    std::string const& str = ss.str();
    EXPECT_EQ(str.data()[0], static_cast<char>(0xd6u));

    msgpack::object_handle oh;
    msgpack::unpack(oh, str.data(), str.size());
    timespec val2 = oh.get().as<timespec>();
    EXPECT_EQ(val1.tv_sec, val2.tv_sec);
    EXPECT_EQ(val1.tv_nsec, val2.tv_nsec);
}

TEST(MSGPACK_TIMESPEC, timespec_object_with_zone_zero)
{
    msgpack::zone z;
    timespec val1{ 0, 0 };
    msgpack::object obj(val1, z);
    timespec val2 = obj.as<timespec>();
    EXPECT_EQ(val1.tv_sec, val2.tv_sec);
    EXPECT_EQ(val1.tv_nsec, val2.tv_nsec);
}

TEST(MSGPACK_TIMESPEC, timespec_pack_convert_32bit_sec)
{
    std::stringstream ss;
    timespec val1{ std::numeric_limits<decltype(std::declval<timespec>().tv_sec)>::is_signed ? INT32_MAX : UINT32_MAX, 0 };

    msgpack::pack(ss, val1);
    std::string const& str = ss.str();
    EXPECT_EQ(str.data()[0], static_cast<char>(0xd6u));

    msgpack::object_handle oh;
    msgpack::unpack(oh, str.data(), str.size());
    timespec val2 = oh.get().as<timespec>();
    EXPECT_EQ(val1.tv_sec, val2.tv_sec);
    EXPECT_EQ(val1.tv_nsec, val2.tv_nsec);
}

TEST(MSGPACK_TIMESPEC, timespec_object_with_zone_32bit_sec)
{
    msgpack::zone z;
    timespec val1{ std::numeric_limits<decltype(std::declval<timespec>().tv_sec)>::is_signed ? INT32_MAX : UINT32_MAX, 0 };
    msgpack::object obj(val1, z);
    timespec val2 = obj.as<timespec>();
    EXPECT_EQ(val1.tv_sec, val2.tv_sec);
    EXPECT_EQ(val1.tv_nsec, val2.tv_nsec);
}

TEST(MSGPACK_TIMESPEC, timespec_pack_convert_max_nano)
{
    std::stringstream ss;
    timespec val1{ 0, 999999999 };

    msgpack::pack(ss, val1);
    std::string const& str = ss.str();
    EXPECT_EQ(str.data()[0], static_cast<char>(0xd7u));
    msgpack::object_handle oh;
    msgpack::unpack(oh, str.data(), str.size());
    timespec val2 = oh.get().as<timespec>();
    EXPECT_EQ(val1.tv_sec, val2.tv_sec);
    EXPECT_EQ(val1.tv_nsec, val2.tv_nsec);
}

TEST(MSGPACK_TIMESPEC, timespec_object_with_zone_max_nano)
{
    msgpack::zone z;
    timespec val1{ 0, 999999999 };
    msgpack::object obj(val1, z);
    timespec val2 = obj.as<timespec>();
    EXPECT_EQ(val1.tv_sec, val2.tv_sec);
    EXPECT_EQ(val1.tv_nsec, val2.tv_nsec);
}

TEST(MSGPACK_TIMESPEC, timespec_pack_convert_34bit_sec_max_nano)
{
    if (sizeof(decltype(std::declval<timespec>().tv_sec)) <= 4) return;
    std::stringstream ss;
    timespec val1{ static_cast<decltype(std::declval<timespec>().tv_sec)>(0x3ffffffffULL), 999999999 };

    msgpack::pack(ss, val1);
    std::string const& str = ss.str();
    EXPECT_EQ(str.data()[0], static_cast<char>(0xd7u));

    msgpack::object_handle oh;
    msgpack::unpack(oh, str.data(), str.size());
    timespec val2 = oh.get().as<timespec>();
    EXPECT_EQ(val1.tv_sec, val2.tv_sec);
    EXPECT_EQ(val1.tv_nsec, val2.tv_nsec);
}

TEST(MSGPACK_TIMESPEC, timespec_object_with_zone_34bit_sec_max_nano)
{
    if (sizeof(decltype(std::declval<timespec>().tv_sec)) <= 4) return;
    msgpack::zone z;
    timespec val1{ static_cast<decltype(std::declval<timespec>().tv_sec)>(0x3ffffffffULL), 999999999 };
    msgpack::object obj(val1, z);
    timespec val2 = obj.as<timespec>();
    EXPECT_EQ(val1.tv_sec, val2.tv_sec);
    EXPECT_EQ(val1.tv_nsec, val2.tv_nsec);
}

TEST(MSGPACK_TIMESPEC, timespec_pack_convert_35bit_sec_max_nano)
{
    if (sizeof(decltype(std::declval<timespec>().tv_sec)) <= 4) return;
    std::stringstream ss;
    timespec val1{ static_cast<decltype(std::declval<timespec>().tv_sec)>(0x7ffffffffULL), 999999999 };

    msgpack::pack(ss, val1);
    std::string const& str = ss.str();
    EXPECT_EQ(str.data()[0], static_cast<char>(0xc7u));

    msgpack::object_handle oh;
    msgpack::unpack(oh, str.data(), str.size());
    timespec val2 = oh.get().as<timespec>();
    EXPECT_EQ(val1.tv_sec, val2.tv_sec);
    EXPECT_EQ(val1.tv_nsec, val2.tv_nsec);
}

TEST(MSGPACK_TIMESPEC, timespec_object_with_zone_35bit_sec_max_nano)
{
    if (sizeof(decltype(std::declval<timespec>().tv_sec)) <= 4) return;
    msgpack::zone z;
    timespec val1{ static_cast<decltype(std::declval<timespec>().tv_sec)>(0x7ffffffffULL), 999999999 };
    msgpack::object obj(val1, z);
    timespec val2 = obj.as<timespec>();
    EXPECT_EQ(val1.tv_sec, val2.tv_sec);
    EXPECT_EQ(val1.tv_nsec, val2.tv_nsec);
}

TEST(MSGPACK_TIMESPEC, timespec_pack_convert_64bit_sec_max_nano)
{
    if (sizeof(decltype(std::declval<timespec>().tv_sec)) <= 4) return;
    std::stringstream ss;
    timespec val1{ std::numeric_limits<decltype(std::declval<timespec>().tv_sec)>::max(), 999999999 };

    msgpack::pack(ss, val1);
    std::string const& str = ss.str();
    EXPECT_EQ(str.data()[0], static_cast<char>(0xc7u));

    msgpack::object_handle oh;
    msgpack::unpack(oh, str.data(), str.size());
    timespec val2 = oh.get().as<timespec>();
    EXPECT_EQ(val1.tv_sec, val2.tv_sec);
    EXPECT_EQ(val1.tv_nsec, val2.tv_nsec);
}

TEST(MSGPACK_TIMESPEC, timespec_object_with_zone_64bit_sec_max_nano)
{
    msgpack::zone z;
    timespec val1{ std::numeric_limits<decltype(std::declval<timespec>().tv_sec)>::max(), 999999999 };
    msgpack::object obj(val1, z);
    timespec val2 = obj.as<timespec>();
    EXPECT_EQ(val1.tv_sec, val2.tv_sec);
    EXPECT_EQ(val1.tv_nsec, val2.tv_nsec);
}


#endif // !defined(MSGPACK_USE_CPP03)
