#include <msgpack.hpp>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif //defined(__GNUC__)

#include <gtest/gtest.h>

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif //defined(__GNUC__)

#include <cmath>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

enum enum_test {
    elem
};

MSGPACK_ADD_ENUM(enum_test);

struct outer_enum {
    enum enum_test {
        elem
    };
};

MSGPACK_ADD_ENUM(outer_enum::enum_test);

#if !defined(MSGPACK_USE_CPP03)

enum class enum_class_test {
    elem
};

MSGPACK_ADD_ENUM(enum_class_test);

struct outer_enum_class {
    enum class enum_class_test {
        elem
    };
};

MSGPACK_ADD_ENUM(outer_enum_class::enum_class_test);

#endif // !defined(MSGPACK_USE_CPP03)



using namespace std;

const unsigned int kLoop = 1000;
const unsigned int kElements = 100;
const double kEPS = 1e-10;

// bool
TEST(object_with_zone, bool)
{
    bool v = true;
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_EQ(obj.as<bool>(), v);
    v = false;
    EXPECT_TRUE(obj.as<bool>());
}

// char
TEST(object_with_zone, char)
{
    char v = 1;
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_EQ(obj.as<char>(), v);
    v = 2;
    EXPECT_EQ(obj.as<char>(), 1);
}

// signed integer family
TEST(object_with_zone, signed_char)
{
    signed char v = -1;
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_EQ(obj.as<signed char>(), v);
    v = -2;
    EXPECT_EQ(obj.as<signed char>(), -1);
}

TEST(object_with_zone, signed_short)
{
    signed short v = -1;
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_EQ(obj.as<signed short>(), v);
    v = -2;
    EXPECT_EQ(obj.as<signed short>(), -1);
}

TEST(object_with_zone, signed_int)
{
    signed int v = -1;
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_EQ(obj.as<signed int>(), v);
    v = -2;
    EXPECT_EQ(obj.as<signed int>(), -1);
}

TEST(object_with_zone, signed_long)
{
    signed long v = -1;
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_EQ(obj.as<signed long>(), v);
    v = -2;
    EXPECT_EQ(obj.as<signed long>(), -1);
}

TEST(object_with_zone, signed_long_long)
{
    signed long long v = -1;
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_EQ(obj.as<signed long long>(), v);
    v = -2;
    EXPECT_EQ(obj.as<signed long long>(), -1);
}

// unsigned integer family
TEST(object_with_zone, unsigned_char)
{
    unsigned char v = 1;
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_EQ(obj.as<unsigned char>(), v);
    v = 2;
    EXPECT_EQ(obj.as<unsigned char>(), 1);
}

TEST(object_with_zone, unsigned_short)
{
    unsigned short v = 1;
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_EQ(obj.as<unsigned short>(), v);
    v = 2;
    EXPECT_EQ(obj.as<unsigned short>(), 1);
}

TEST(object_with_zone, unsigned_int)
{
    unsigned int v = 1;
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_EQ(obj.as<unsigned int>(), v);
    v = 2;
    EXPECT_EQ(obj.as<unsigned int>(), 1u);
}

TEST(object_with_zone, unsigned_long)
{
    unsigned long v = 1;
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_EQ(obj.as<unsigned long>(), v);
    v = 2;
    EXPECT_EQ(obj.as<unsigned long>(), 1u);
}

TEST(object_with_zone, unsigned_long_long)
{
    unsigned long long v = 1;
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_EQ(obj.as<unsigned long long>(), v);
    v = 2;
    EXPECT_EQ(obj.as<unsigned long long>(), 1u);
}

// float
TEST(object_with_zone, float)
{
    float v = 1.23f;
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_EQ(obj.type, msgpack::type::FLOAT32);
    EXPECT_TRUE(fabs(obj.as<float>() - v) <= kEPS);
    v = 4.56f;
    EXPECT_TRUE(fabs(obj.as<float>() - static_cast<float>(1.23)) <= kEPS);
}

// double
TEST(object_with_zone, double)
{
    double v = 1.23;
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_EQ(obj.type, msgpack::type::FLOAT64);
    EXPECT_TRUE(fabs(obj.as<double>() - v) <= kEPS);
    v = 4.56;
    EXPECT_TRUE(fabs(obj.as<double>() - 1.23) <= kEPS);
}

// vector

TEST(object_with_zone, vector)
{
    for (unsigned int k = 0; k < kLoop; k++) {
        vector<int> v1;
        v1.push_back(1);
        for (unsigned int i = 1; i < kElements; i++)
            v1.push_back(static_cast<int>(i));
        msgpack::zone z;
        msgpack::object obj(v1, z);
        EXPECT_TRUE(obj.as<vector<int> >() == v1);
        v1.front() = 42;
        EXPECT_EQ(obj.as<vector<int> >().front(), 1);
    }
}

// vector_char
TEST(object_with_zone, vector_char)
{
    for (unsigned int k = 0; k < kLoop; k++) {
        vector<char> v1;
        v1.push_back(1);
        for (unsigned int i = 1; i < kElements; i++)
            v1.push_back(static_cast<char>(i));
        msgpack::zone z;
        msgpack::object obj(v1, z);
        EXPECT_TRUE(obj.as<vector<char> >() == v1);
        v1.front() = 42;
        EXPECT_EQ(obj.as<vector<char> >().front(), 1);
    }
}

TEST(object_without_zone, vector_char)
{
    for (unsigned int k = 0; k < kLoop; k++) {
        vector<char> v1;
        v1.push_back(1);
        for (unsigned int i = 1; i < kElements; i++)
            v1.push_back(static_cast<char>(i));
        msgpack::object obj(v1);
        EXPECT_TRUE(obj.as<vector<char> >() == v1);
        v1.front() = 42;
        // obj refer to v1
        EXPECT_EQ(obj.as<vector<char> >().front(), 42);
    }
}

// vector_unsgined_char
TEST(object_with_zone, vector_unsigned_char)
{
    if (!msgpack::is_same<uint8_t, unsigned char>::value) return;
    for (unsigned int k = 0; k < kLoop; k++) {
        vector<unsigned char> v1;
        v1.push_back(1);
        for (unsigned int i = 1; i < kElements; i++)
            v1.push_back(static_cast<unsigned char>(i));
        msgpack::zone z;
        msgpack::object obj(v1, z);
        EXPECT_TRUE(obj.as<vector<unsigned char> >() == v1);
        v1.front() = 42;
        EXPECT_EQ(obj.as<vector<unsigned char> >().front(), 1);
    }
}

TEST(object_without_zone, vector_unsigned_char)
{
    if (!msgpack::is_same<uint8_t, unsigned char>::value) return;
    for (unsigned int k = 0; k < kLoop; k++) {
        vector<unsigned char> v1;
        v1.push_back(1);
        for (unsigned int i = 1; i < kElements; i++)
            v1.push_back(static_cast<unsigned char>(i));
        msgpack::object obj(v1);
        EXPECT_TRUE(obj.as<vector<unsigned char> >() == v1);
        v1.front() = 42;
        // obj refer to v1
        EXPECT_EQ(obj.as<vector<unsigned char> >().front(), 42);
    }
}

// list
TEST(object_with_zone, list)
{
    for (unsigned int k = 0; k < kLoop; k++) {
        list<int> v1;
        v1.push_back(1);
        for (unsigned int i = 1; i < kElements; i++)
            v1.push_back(static_cast<int>(i));
        msgpack::zone z;
        msgpack::object obj(v1, z);
        EXPECT_TRUE(obj.as<list<int> >() == v1);
        v1.front() = 42;
        EXPECT_EQ(obj.as<list<int> >().front(), 1);
    }
}

// deque
TEST(object_with_zone, deque)
{
    for (unsigned int k = 0; k < kLoop; k++) {
        deque<int> v1;
        v1.push_back(1);
        for (unsigned int i = 1; i < kElements; i++)
            v1.push_back(static_cast<int>(i));
        msgpack::zone z;
        msgpack::object obj(v1, z);
        EXPECT_TRUE(obj.as<deque<int> >() == v1);
        v1.front() = 42;
        EXPECT_EQ(obj.as<deque<int> >().front(), 1);
    }
}

// string
TEST(object_with_zone, string)
{
    string v = "abc";
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_EQ(obj.as<string>(), v);
    v[0] = 'd';
    EXPECT_EQ(obj.as<string>()[0], 'a');
}

#if MSGPACK_DEFAULT_API_VERSION == 1

TEST(object_without_zone, string)
{
    string v = "abc";
    msgpack::zone z;
    msgpack::object obj(v);
    EXPECT_EQ(obj.as<string>(), v);
    v[0] = 'd';
    EXPECT_EQ(obj.as<string>()[0], 'd');
}

#endif // MSGPACK_DEFAULT_API_VERSION == 1

// wstring
TEST(object_with_zone, wstring)
{
    wstring v = L"abc";
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_EQ(obj.as<wstring>(), v);
    v[0] = 'd';
    EXPECT_EQ(obj.as<wstring>()[0], L'a');
}

// char*
TEST(object_with_zone, char_ptr)
{
    char v[] = "abc";
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_EQ(obj.as<string>(), std::string(v));
    v[0] = 'd';
    EXPECT_EQ(obj.as<string>()[0], 'a');
}

#if MSGPACK_DEFAULT_API_VERSION == 1

TEST(object_without_zone, char_ptr)
{
    char v[] = "abc";
    msgpack::zone z;
    msgpack::object obj(v);
    EXPECT_EQ(obj.as<string>(), std::string(v));
    v[0] = 'd';
    EXPECT_EQ(obj.as<string>()[0], 'd');
}

#endif // MSGPACK_DEFAULT_API_VERSION == 1

// raw_ref
TEST(object_with_zone, raw_ref)
{
    string s = "abc";
    msgpack::type::raw_ref v(s.data(), static_cast<uint32_t>(s.size()));
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_TRUE(obj.as<msgpack::type::raw_ref>() == v);
    s[0] = 'd';
    // even if with_zone, not copied due to raw_ref
    // Basically, the combination raw_ref and object::wit_zone
    // is meaningless.
    EXPECT_TRUE(obj.as<msgpack::type::raw_ref>() == v);
}

TEST(object_without_zone, raw_ref)
{
    string s = "abc";
    msgpack::type::raw_ref v(s.data(), static_cast<uint32_t>(s.size()));
    msgpack::zone z;
    msgpack::object obj(v);
    EXPECT_TRUE(obj.as<msgpack::type::raw_ref>() == v);
    s[0] = 'd';
    EXPECT_TRUE(obj.as<msgpack::type::raw_ref>() == v);
}

// pair
TEST(object_with_zone, pair)
{
    typedef pair<int, string> test_t;
    test_t v(1, "abc");
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_TRUE(obj.as<test_t>() == v);
    v.first = 42;
    EXPECT_EQ(obj.as<test_t>().first, 1);
}

// set
TEST(object_with_zone, set)
{
    for (unsigned int k = 0; k < kLoop; k++) {
        set<int> v1;
        for (unsigned int i = 0; i < kElements; i++)
            v1.insert(static_cast<int>(i));
        msgpack::zone z;
        msgpack::object obj(v1, z);
        EXPECT_TRUE(obj.as<set<int> >() == v1);
    }
}

// multiset
TEST(object_with_zone, multiset)
{
    for (unsigned int k = 0; k < kLoop; k++) {
        multiset<int> v1;
        for (unsigned int i = 0; i < kElements; i++)
            v1.insert(i % (kElements / 2));
        msgpack::zone z;
        msgpack::object obj(v1, z);
        EXPECT_TRUE(obj.as<multiset<int> >() == v1);
    }
}

// map
TEST(object_with_zone, map)
{
    typedef map<int, int> test_t;
    for (unsigned int k = 0; k < kLoop; k++) {
        test_t v1;
        for (unsigned int i = 0; i < kElements; i++)
            v1.insert(std::make_pair(i, i*2));
        msgpack::zone z;
        msgpack::object obj(v1, z);
        EXPECT_TRUE(obj.as<test_t >() == v1);
    }
}

// multimap
TEST(object_with_zone, multimap)
{
    typedef multimap<int, int> test_t;
    for (unsigned int k = 0; k < kLoop; k++) {
        test_t v1;
        for (unsigned int i = 0; i < kElements; i++)
            v1.insert(std::make_pair(i % (kElements / 2), i*2));
        msgpack::zone z;
        msgpack::object obj(v1, z);
        EXPECT_TRUE(obj.as<test_t >() == v1);
    }
}

// msgpack_tuple
TEST(object_with_zone, msgpack_tuple)
{
    typedef msgpack::type::tuple<int, string, bool> test_t;
    test_t v(1, "abc", true);
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_EQ(msgpack::type::get<0>(obj.as<test_t>()), 1);
    EXPECT_EQ(msgpack::type::get<1>(obj.as<test_t>()), "abc");
    EXPECT_EQ(msgpack::type::get<2>(obj.as<test_t>()), true);
    msgpack::type::get<0>(v) = 42;
    EXPECT_EQ(msgpack::type::get<0>(obj.as<test_t>()), 1);
}

TEST(object_with_zone, msgpack_tuple_empty)
{
    typedef msgpack::type::tuple<> test_t;
    test_t v;
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_EQ(obj.via.array.size, 0u);
}

// TR1

#ifdef MSGPACK_HAS_STD_TR1_UNORDERED_MAP
#include <tr1/unordered_map>
#include "msgpack/adaptor/tr1/unordered_map.hpp"
TEST(object_with_zone, tr1_unordered_map)
{
    typedef tr1::unordered_map<int, int> test_t;
    for (unsigned int k = 0; k < kLoop; k++) {
        test_t v1;
        for (unsigned int i = 0; i < kElements; i++)
            v1[rand()] = rand();
        msgpack::zone z;
        msgpack::object obj(v1, z);
        test_t v2 = obj.as<test_t>();
        EXPECT_EQ(v1.size(), v2.size());
        test_t::const_iterator it;
        for (it = v1.begin(); it != v1.end(); ++it) {
            EXPECT_TRUE(v2.find(it->first) != v2.end());
            EXPECT_EQ(it->second, v2.find(it->first)->second);
        }
    }
}


TEST(object_with_zone, tr1_unordered_multimap)
{
    typedef tr1::unordered_multimap<int, int> test_t;
    for (unsigned int k = 0; k < kLoop; k++) {
        test_t v1;
        for (unsigned int i = 0; i < kElements; i++) {
            int i1 = rand();
            v1.insert(make_pair(i1, rand()));
            v1.insert(make_pair(i1, rand()));
        }
        msgpack::zone z;
        msgpack::object obj(v1, z);
        test_t v2 = obj.as<test_t>();
        vector<pair<int, int> > vec1, vec2;
        tr1::unordered_multimap<int, int>::const_iterator it;
        for (it = v1.begin(); it != v1.end(); ++it)
            vec1.push_back(make_pair(it->first, it->second));
        for (it = v2.begin(); it != v2.end(); ++it)
            vec2.push_back(make_pair(it->first, it->second));
        EXPECT_EQ(v1.size(), v2.size());
        EXPECT_EQ(vec1.size(), vec2.size());
        sort(vec1.begin(), vec1.end());
        sort(vec2.begin(), vec2.end());
        EXPECT_TRUE(vec1 == vec2);
    }
}
#endif

#ifdef MSGPACK_HAS_STD_TR1_UNORDERED_SET
#include <tr1/unordered_set>
#include "msgpack/adaptor/tr1/unordered_set.hpp"
TEST(object_with_zone, tr1_unordered_set)
{
    typedef tr1::unordered_set<int> test_t;
    for (unsigned int k = 0; k < kLoop; k++) {
        test_t v1;
        for (unsigned int i = 0; i < kElements; i++)
            v1.insert(rand());
        msgpack::zone z;
        msgpack::object obj(v1, z);
        test_t v2 = obj.as<test_t>();
        EXPECT_EQ(v1.size(), v2.size());
        tr1::unordered_set<int>::const_iterator it;
        for (it = v1.begin(); it != v1.end(); ++it)
            EXPECT_TRUE(v2.find(*it) != v2.end());
    }
}

TEST(object_with_zone, tr1_unordered_multiset)
{
    typedef tr1::unordered_set<int> test_t;
    for (unsigned int k = 0; k < kLoop; k++) {
        test_t v1;
        for (unsigned int i = 0; i < kElements; i++) {
            int i1 = rand();
            v1.insert(i1);
            v1.insert(i1);
        }
        msgpack::zone z;
        msgpack::object obj(v1, z);
        test_t v2 = obj.as<test_t>();
        vector<int> vec1, vec2;
        tr1::unordered_multiset<int>::const_iterator it;
        for (it = v1.begin(); it != v1.end(); ++it)
            vec1.push_back(*it);
        for (it = v2.begin(); it != v2.end(); ++it)
            vec2.push_back(*it);
        EXPECT_EQ(v1.size(), v2.size());
        EXPECT_EQ(vec1.size(), vec2.size());
        sort(vec1.begin(), vec1.end());
        sort(vec2.begin(), vec2.end());
        EXPECT_TRUE(vec1 == vec2);
    }
}
#endif

#ifdef MSGPACK_HAS_STD_UNORDERED_MAP
#include <unordered_map>
#include "msgpack/adaptor/tr1/unordered_map.hpp"
TEST(object_with_zone, unordered_map)
{
    typedef unordered_map<int, int> test_t;
    for (unsigned int k = 0; k < kLoop; k++) {
        test_t v1;
        for (unsigned int i = 0; i < kElements; i++)
            v1[rand()] = rand();
        msgpack::zone z;
        msgpack::object obj(v1, z);
        test_t v2 = obj.as<test_t>();
        EXPECT_EQ(v1.size(), v2.size());
        test_t::const_iterator it;
        for (it = v1.begin(); it != v1.end(); ++it) {
            EXPECT_TRUE(v2.find(it->first) != v2.end());
            EXPECT_EQ(it->second, v2.find(it->first)->second);
        }
    }
}

TEST(object_with_zone, unordered_multimap)
{
   typedef unordered_multimap<int, int> test_t;
    for (unsigned int k = 0; k < kLoop; k++) {
        test_t v1;
        for (unsigned int i = 0; i < kElements; i++) {
            int i1 = rand();
            v1.insert(make_pair(i1, rand()));
            v1.insert(make_pair(i1, rand()));
        }
        msgpack::zone z;
        msgpack::object obj(v1, z);
        test_t v2 = obj.as<test_t>();
        vector<pair<int, int> > vec1, vec2;
        unordered_multimap<int, int>::const_iterator it;
        for (it = v1.begin(); it != v1.end(); ++it)
            vec1.push_back(make_pair(it->first, it->second));
        for (it = v2.begin(); it != v2.end(); ++it)
            vec2.push_back(make_pair(it->first, it->second));
        EXPECT_EQ(v1.size(), v2.size());
        EXPECT_EQ(vec1.size(), vec2.size());
        sort(vec1.begin(), vec1.end());
        sort(vec2.begin(), vec2.end());
        EXPECT_TRUE(vec1 == vec2);
    }
}
#endif

#ifdef MSGPACK_HAS_STD_UNORDERED_SET
#include <unordered_set>
#include "msgpack/adaptor/tr1/unordered_set.hpp"
TEST(object_with_zone, unordered_set)
{
    typedef unordered_set<int> test_t;
    for (unsigned int k = 0; k < kLoop; k++) {
        test_t v1;
        for (unsigned int i = 0; i < kElements; i++)
            v1.insert(rand());
        msgpack::zone z;
        msgpack::object obj(v1, z);
        test_t v2 = obj.as<test_t>();
        EXPECT_EQ(v1.size(), v2.size());
        unordered_set<int>::const_iterator it;
        for (it = v1.begin(); it != v1.end(); ++it)
            EXPECT_TRUE(v2.find(*it) != v2.end());
    }
}

TEST(object_with_zone, unordered_multiset)
{
    typedef unordered_set<int> test_t;
    for (unsigned int k = 0; k < kLoop; k++) {
        test_t v1;
        for (unsigned int i = 0; i < kElements; i++) {
            int i1 = rand();
            v1.insert(i1);
            v1.insert(i1);
        }
        msgpack::zone z;
        msgpack::object obj(v1, z);
        test_t v2 = obj.as<test_t>();
        vector<int> vec1, vec2;
        unordered_multiset<int>::const_iterator it;
        for (it = v1.begin(); it != v1.end(); ++it)
            vec1.push_back(*it);
        for (it = v2.begin(); it != v2.end(); ++it)
            vec2.push_back(*it);
        EXPECT_EQ(v1.size(), v2.size());
        EXPECT_EQ(vec1.size(), vec2.size());
        sort(vec1.begin(), vec1.end());
        sort(vec2.begin(), vec2.end());
        EXPECT_TRUE(vec1 == vec2);
    }
}
#endif

// User defined class
class TestClass
{
public:
    TestClass() : i(0), s("kzk") {}
    int i;
    string s;
    MSGPACK_DEFINE(i, s);
};

TEST(object_with_zone, user_defined)
{
    TestClass v1;
    msgpack::zone z;
    msgpack::object obj(v1, z);
    TestClass v2 = obj.as<TestClass>();
    EXPECT_EQ(v1.i, v2.i);
    EXPECT_EQ(v1.s, v2.s);
}

TEST(object_with_zone, construct_enum)
{
    msgpack::zone z;
    msgpack::object obj(elem, z);
    EXPECT_EQ(msgpack::type::POSITIVE_INTEGER, obj.type);
    EXPECT_EQ(static_cast<uint64_t>(elem), obj.via.u64);
}

#if !defined(MSGPACK_USE_CPP03)

TEST(object_with_zone, construct_enum_newstyle)
{
    msgpack::zone z;
    msgpack::object obj(enum_test::elem, z);
    EXPECT_EQ(msgpack::type::POSITIVE_INTEGER, obj.type);
    EXPECT_EQ(elem, obj.via.u64);
}

#endif // !defined(MSGPACK_USE_CPP03)

TEST(object_with_zone, construct_enum_outer)
{
    msgpack::zone z;
    msgpack::object obj(outer_enum::elem, z);
    EXPECT_EQ(msgpack::type::POSITIVE_INTEGER, obj.type);
    EXPECT_EQ(static_cast<uint64_t>(elem), obj.via.u64);
}

// User defined inheriting classes
struct top {
    int t;
    MSGPACK_DEFINE(t);
};

struct mid1 : top {
    int m1;
    MSGPACK_DEFINE(MSGPACK_BASE(top), m1);
};

struct mid2 : top {
    int m2;
    MSGPACK_DEFINE(m2, MSGPACK_BASE(top));
};

struct bottom : mid1, mid2 {
    int b;
    MSGPACK_DEFINE(MSGPACK_BASE(mid1), MSGPACK_BASE(mid2), b);
};

TEST(object_with_zone, user_defined_non_virtual)
{
    bottom b;
    b.b = 1;
    b.m1 = 2;
    b.m2 = 3;
    b.mid1::t = 4;
    b.mid2::t = 5;

    msgpack::zone z;
    msgpack::object obj(b, z);
#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 7)) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif // defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 7)) && !defined(__clang__)
    bottom br = obj.as<bottom>();
#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 7)) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif // defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 7)) && !defined(__clang__)
    EXPECT_EQ(b.b, br.b);
    EXPECT_EQ(b.m1, br.m1);
    EXPECT_EQ(b.m2, br.m2);
    EXPECT_EQ(b.mid1::t, br.mid1::t);
    EXPECT_EQ(b.mid2::t, br.mid2::t);
}

struct v_top {
    int t;
    MSGPACK_DEFINE(t);
};

struct v_mid1 : virtual v_top {
    int m1;
    MSGPACK_DEFINE(m1);
};

struct v_mid2 : virtual v_top {
    int m2;
    MSGPACK_DEFINE(m2);
};

struct v_bottom : v_mid1, v_mid2 {
    int b;
    MSGPACK_DEFINE(MSGPACK_BASE(v_mid1), MSGPACK_BASE(v_mid2), MSGPACK_BASE(v_top), b);
};

TEST(object_with_zone, user_defined_virtual)
{
    v_bottom b;
    b.b = 1;
    b.m1 = 2;
    b.m2 = 3;
    b.t = 4;

    msgpack::zone z;
    msgpack::object obj(b, z);
#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 7)) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif // defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 7)) && !defined(__clang__)
    v_bottom br = obj.as<v_bottom>();
#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 7)) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif // defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 7)) && !defined(__clang__)
    EXPECT_EQ(b.b, br.b);
    EXPECT_EQ(b.m1, br.m1);
    EXPECT_EQ(b.m2, br.m2);
    EXPECT_EQ(b.t, br.t);
}

#if !defined(MSGPACK_USE_CPP03)

TEST(object_with_zone, construct_enum_outer_newstyle)
{
    msgpack::zone z;
    msgpack::object obj(outer_enum::enum_test::elem, z);
    EXPECT_EQ(msgpack::type::POSITIVE_INTEGER, obj.type);
    EXPECT_EQ(elem, obj.via.u64);
}

TEST(object_with_zone, construct_class_enum)
{
    msgpack::zone z;
    msgpack::object obj(enum_class_test::elem, z);
    EXPECT_EQ(msgpack::type::POSITIVE_INTEGER, obj.type);
    EXPECT_EQ(elem, obj.via.u64);
}


TEST(object_with_zone, construct_class_enum_outer)
{
    msgpack::zone z;
    msgpack::object obj(outer_enum_class::enum_class_test::elem, z);
    EXPECT_EQ(msgpack::type::POSITIVE_INTEGER, obj.type);
    EXPECT_EQ(elem, obj.via.u64);
}


TEST(object_with_zone, array)
{
    typedef array<int, kElements> test_t;
    for (unsigned int k = 0; k < kLoop; k++) {
        test_t v1;
        v1[0] = 1;
        for (unsigned int i = 1; i < kElements; i++)
            v1[i] = rand();
        msgpack::zone z;
        msgpack::object obj(v1, z);
        EXPECT_TRUE(obj.as<test_t>() == v1);
        v1.front() = 42;
        EXPECT_EQ(obj.as<test_t>().front(), 1);
    }
}

TEST(object_with_zone, array_char)
{
    typedef array<char, kElements> test_t;
    for (unsigned int k = 0; k < kLoop; k++) {
        test_t v1;
        v1[0] = 1;
        for (unsigned int i = 1; i < kElements; i++)
            v1[i] = static_cast<char>(rand());
        msgpack::zone z;
        msgpack::object obj(v1, z);
        EXPECT_TRUE(obj.as<test_t>() == v1);
        v1.front() = 42;
        EXPECT_EQ(obj.as<test_t>().front(), 1);
    }
}

TEST(object_without_zone, array_char)
{
    typedef array<char, kElements> test_t;
    for (unsigned int k = 0; k < kLoop; k++) {
        test_t v1;
        v1[0] = 1;
        for (unsigned int i = 1; i < kElements; i++)
            v1[i] = static_cast<char>(rand());
        msgpack::object obj(v1);
        EXPECT_TRUE(obj.as<test_t>() == v1);
        v1.front() = 42;
        // obj refer to v1
        EXPECT_EQ(obj.as<test_t>().front(), 42);
    }
}

TEST(object_with_zone, array_unsigned_char)
{
    if (!msgpack::is_same<uint8_t, unsigned char>::value) return;
    typedef array<unsigned char, kElements> test_t;
    for (unsigned int k = 0; k < kLoop; k++) {
        test_t v1;
        v1[0] = 1;
        for (unsigned int i = 1; i < kElements; i++)
            v1[i] = static_cast<unsigned char>(rand());
        msgpack::zone z;
        msgpack::object obj(v1, z);
        EXPECT_TRUE(obj.as<test_t>() == v1);
        v1.front() = 42;
        EXPECT_EQ(obj.as<test_t>().front(), 1);
    }
}

TEST(object_without_zone, array_unsigned_char)
{
    if (!msgpack::is_same<uint8_t, unsigned char>::value) return;
    typedef array<unsigned char, kElements> test_t;
    for (unsigned int k = 0; k < kLoop; k++) {
        test_t v1;
        v1[0] = 1;
        for (unsigned int i = 1; i < kElements; i++)
            v1[i] = static_cast<unsigned char>(rand());
        msgpack::object obj(v1);
        EXPECT_TRUE(obj.as<test_t>() == v1);
        v1.front() = 42;
        // obj refer to v1
        EXPECT_EQ(obj.as<test_t>().front(), 42);
    }
}


TEST(object_with_zone, forward_list)
{
    for (unsigned int k = 0; k < kLoop; k++) {
        forward_list<int> v1;
        for (unsigned int i = 0; i < kElements; i++)
            v1.push_front(static_cast<int>(i));
        msgpack::zone z;
        msgpack::object obj(v1, z);
        EXPECT_TRUE(obj.as<forward_list<int> >() == v1);
        v1.front() = 42;
        EXPECT_EQ(obj.as<forward_list<int> >().front(), static_cast<int>(kElements - 1));
    }
}

TEST(object_with_zone, tuple)
{
    typedef tuple<int, string, bool> test_t;
    test_t v(1, "abc", true);
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_TRUE(obj.as<test_t>() == v);
}

TEST(object_with_zone, tuple_empty)
{
    typedef tuple<> test_t;
    test_t v;
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_TRUE(obj.as<test_t>() == v);
}

TEST(object_with_zone, system_clock)
{
    std::chrono::system_clock::time_point v;
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_TRUE(obj.as<std::chrono::system_clock::time_point>() == v);
}

TEST(object_with_zone, system_clock_32)
{
    std::chrono::system_clock::time_point v(std::chrono::seconds(0x12345678L));
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_TRUE(obj.as<std::chrono::system_clock::time_point>() == v);
}

TEST(object_with_zone, system_clock_32_max)
{
    std::chrono::system_clock::time_point v(std::chrono::seconds(0xffffffffL));
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_TRUE(obj.as<std::chrono::system_clock::time_point>() == v);
}

TEST(object_with_zone, system_clock_64)
{
    std::chrono::system_clock::time_point v(std::chrono::seconds(0x31234567L));
    v +=
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::nanoseconds(0x312345678L)
        );
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_TRUE(obj.as<std::chrono::system_clock::time_point>() == v);
}

TEST(object_with_zone, system_clock_64_max)
{
    std::chrono::system_clock::time_point v(std::chrono::seconds(0xffffffffL));
    v +=
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::nanoseconds(0x3b9ac9ffL) // 999,999,999
        );
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_TRUE(obj.as<std::chrono::system_clock::time_point>() == v);
}

TEST(object_with_zone, system_clock_impl_min)
{
    std::chrono::system_clock::time_point v(std::chrono::system_clock::time_point::min());
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_TRUE(obj.as<std::chrono::system_clock::time_point>() == v);
}

TEST(object_with_zone, system_clock_impl_max)
{
    std::chrono::system_clock::time_point v(std::chrono::system_clock::time_point::max());
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_TRUE(obj.as<std::chrono::system_clock::time_point>() == v);
}

#endif // !defined(MSGPACK_USE_CPP03)

TEST(object_with_zone, ext_empty)
{
    msgpack::type::ext v;
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_TRUE(obj.as<msgpack::type::ext>() == v);
    EXPECT_TRUE(obj.as<msgpack::type::ext_ref>() == v);
}

TEST(object_with_zone, ext)
{
    msgpack::type::ext v(42, 10);
    for (int i = 0; i < 10; ++i) v.data()[i] = static_cast<char>(i);
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_TRUE(obj.as<msgpack::type::ext>() == v);
    EXPECT_TRUE(obj.as<msgpack::type::ext_ref>() == v);
}

TEST(object_with_zone, ext_from_buf)
{
    char const buf[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    msgpack::type::ext v(42, buf, sizeof(buf));
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_TRUE(obj.as<msgpack::type::ext>() == v);
    EXPECT_TRUE(obj.as<msgpack::type::ext_ref>() == v);
}

TEST(object_with_zone, ext_ref_empty)
{
    msgpack::type::ext_ref v;
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_TRUE(obj.as<msgpack::type::ext>() == v);
    EXPECT_TRUE(obj.as<msgpack::type::ext_ref>() == v);
}

TEST(object_with_zone, ext_ref_from_buf)
{
    char const buf[] = { 77, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    msgpack::type::ext_ref v(buf, sizeof(buf));
    msgpack::zone z;
    msgpack::object obj(v, z);
    EXPECT_TRUE(obj.as<msgpack::type::ext>() == v);
    EXPECT_TRUE(obj.as<msgpack::type::ext_ref>() == v);
}
