#include <msgpack.hpp>
#include <gtest/gtest.h>
#include <sstream>

// To avoid link error
TEST(visitor, dummy)
{
}

#if MSGPACK_DEFAULT_API_VERSION >= 2

struct json_like_visitor : msgpack::null_visitor {
    json_like_visitor(std::string& s):m_s(s) {}

    bool visit_nil() {
        m_s += "null";
        return true;
    }
    bool visit_boolean(bool v) {
        if (v) m_s += "true";
        else m_s += "false";
        return true;
    }
    bool visit_positive_integer(uint64_t v) {
        std::stringstream ss;
        ss << v;
        m_s += ss.str();
        return true;
    }
    bool visit_negative_integer(int64_t v) {
        std::stringstream ss;
        ss << v;
        m_s += ss.str();
        return true;
    }
    bool visit_str(const char* v, uint32_t size) {
        // I omit escape process.
        m_s += '"' + std::string(v, size) + '"';
        return true;
    }
    bool start_array(uint32_t /*num_elements*/) {
        m_s += "[";
        return true;
    }
    bool end_array_item() {
        m_s += ",";
        return true;
    }
    bool end_array() {
        m_s.erase(m_s.size() - 1, 1); // remove the last ','
        m_s += "]";
        return true;
    }
    bool start_map(uint32_t /*num_kv_pairs*/) {
        m_s += "{";
        return true;
    }
    bool end_map_key() {
        m_s += ":";
        return true;
    }
    bool end_map_value() {
        m_s += ",";
        return true;
    }
    bool end_map() {
        m_s.erase(m_s.size() - 1, 1); // remove the last ','
        m_s += "}";
        return true;
    }
    void parse_error(size_t /*parsed_offset*/, size_t /*error_offset*/) {
        EXPECT_TRUE(false);
    }
    void insufficient_bytes(size_t /*parsed_offset*/, size_t /*error_offset*/) {
        EXPECT_TRUE(false);
    }
    std::string& m_s;
};

TEST(visitor, json_like)
{
    std::stringstream ss;
    msgpack::packer<std::stringstream> p(ss);
    p.pack_map(1);
    p.pack("key");
    p.pack_array(3);
    p.pack(42);
    p.pack_nil();
    p.pack(true);

    std::string json_like;
    json_like_visitor v(json_like);
    std::size_t off = 0;
    bool ret = msgpack::parse(ss.str().data(), ss.str().size(), off, v);
    EXPECT_TRUE(ret);
    EXPECT_EQ("{\"key\":[42,null,true]}", json_like);
}

struct parse_error_check_visitor : msgpack::null_visitor {
    parse_error_check_visitor(bool& called):m_called(called) {}
    void parse_error(size_t parsed_offset, size_t error_offset) {
        EXPECT_EQ(static_cast<size_t>(1), parsed_offset);
        EXPECT_EQ(static_cast<size_t>(2), error_offset);
        m_called = true;
    }
    bool& m_called;
};

TEST(visitor, parse_error)
{
    bool called = false;
    parse_error_check_visitor v(called);
    std::size_t off = 0;
    char const data[] = { static_cast<char>(0x93u), 0x01u, static_cast<char>(0xc1u), 0x03u };
    bool ret = msgpack::parse(data, sizeof(data), off, v);
    EXPECT_FALSE(ret);
    EXPECT_TRUE(called);
#if MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(0u, off);
#else  // MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(2u, off);
#endif // MSGPACK_DEFAULT_API_VERSION < 3
}

struct insuf_bytes_check_visitor : msgpack::null_visitor {
    insuf_bytes_check_visitor(bool& called):m_called(called) {}
    void insufficient_bytes(size_t parsed_offset, size_t error_offset) {
        EXPECT_EQ(static_cast<size_t>(2), parsed_offset);
        EXPECT_EQ(static_cast<size_t>(3), error_offset);
        m_called = true;
    }
    bool& m_called;
};

TEST(visitor, insuf_bytes)
{
    bool called = false;
    insuf_bytes_check_visitor v(called);
    std::size_t off = 0;
    char const data[] = { static_cast<char>(0x93u), 0x01u, 0x01u };
    bool ret = msgpack::parse(data, sizeof(data), off, v);
    EXPECT_FALSE(ret);
    EXPECT_TRUE(called);
    // Even if MSGPACK_DEFAULT_API_VERSION is 2, then off is updated
    // in the case of insufficient bytes.
    EXPECT_EQ(3u, off);
}

struct return_false_array_val_visitor : msgpack::null_visitor {
    return_false_array_val_visitor(std::size_t& times):m_times(times) {}
    bool visit_positive_integer(uint64_t) {
        if (++m_times == 2) return false;
        return true;
    }
    std::size_t& m_times;
};

TEST(visitor, return_false_array_val)
{
    std::size_t times = 0;
    return_false_array_val_visitor v(times);
    std::size_t off = 0;
    char const data[] = { static_cast<char>(0x93u), 0x01u, 0x02u, 0x03u };
    bool ret = msgpack::parse(data, sizeof(data), off, v);
    EXPECT_FALSE(ret);
    EXPECT_EQ(2u, times);
#if MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(0u, off);
#else  // MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(2u, off);
#endif // MSGPACK_DEFAULT_API_VERSION < 3
}

struct return_false_start_array_item_visitor : msgpack::null_visitor {
    return_false_start_array_item_visitor(std::size_t& times):m_times(times) {}
    bool start_array_item() {
        if (++m_times == 2) return false;
        return true;
    }
    std::size_t& m_times;
};

TEST(visitor, return_false_start_array_item)
{
    std::size_t times = 0;
    return_false_start_array_item_visitor v(times);
    std::size_t off = 0;
    char const data[] = { static_cast<char>(0x93u), 0x01u, 0x02u, 0x03u };
    bool ret = msgpack::parse(data, sizeof(data), off, v);
    EXPECT_FALSE(ret);
    EXPECT_EQ(2u, times);
#if MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(0u, off);
#else  // MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(2u, off);
#endif // MSGPACK_DEFAULT_API_VERSION < 3
}

struct return_false_end_array_item_visitor : msgpack::null_visitor {
    return_false_end_array_item_visitor(std::size_t& times):m_times(times) {}
    bool end_array_item() {
        if (++m_times == 2) return false;
        return true;
    }
    std::size_t& m_times;
};

TEST(visitor, return_false_end_array_item)
{
    std::size_t times = 0;
    return_false_end_array_item_visitor v(times);
    std::size_t off = 0;
    char const data[] = { static_cast<char>(0x93u), 0x01u, 0x02u, 0x03u };
    bool ret = msgpack::parse(data, sizeof(data), off, v);
    EXPECT_FALSE(ret);
    EXPECT_EQ(2u, times);
#if MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(0u, off);
#else  // MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(2u, off);
#endif // MSGPACK_DEFAULT_API_VERSION < 3
}

struct return_false_start_array_visitor : msgpack::null_visitor {
    bool start_array(uint32_t) {
        return false;
    }
};

TEST(visitor, return_false_start_array)
{
    return_false_start_array_visitor v;
    std::size_t off = 0;
    char const data[] = { static_cast<char>(0x93u), 0x01u, 0x02u, 0x03u };
    bool ret = msgpack::parse(data, sizeof(data), off, v);
    EXPECT_FALSE(ret);
    EXPECT_EQ(0u, off);
}

struct return_false_start_array0_visitor : msgpack::null_visitor {
    bool start_array(uint32_t) {
        return false;
    }
};

TEST(visitor, return_false_start_array0)
{
    return_false_start_array0_visitor v;
    std::size_t off = 0;
    char const data[] = { static_cast<char>(0x90u) };
    bool ret = msgpack::parse(data, sizeof(data), off, v);
    EXPECT_FALSE(ret);
    EXPECT_EQ(0u, off);
}

struct return_false_end_array_visitor : msgpack::null_visitor {
    bool end_array() {
        return false;
    }
};

TEST(visitor, return_false_end_array)
{
    return_false_end_array_visitor v;
    std::size_t off = 0;
    char const data[] = { static_cast<char>(0x93u), 0x01u, 0x02u, 0x03u };
    bool ret = msgpack::parse(data, sizeof(data), off, v);
    EXPECT_FALSE(ret);
#if MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(0u, off);
#else  // MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(3u, off);
#endif // MSGPACK_DEFAULT_API_VERSION < 3
}

struct return_false_end_array0_visitor : msgpack::null_visitor {
    bool end_array() {
        return false;
    }
};

TEST(visitor, return_false_end_array0)
{
    return_false_end_array0_visitor v;
    std::size_t off = 0;
    char const data[] = { static_cast<char>(0x90u) };
    bool ret = msgpack::parse(data, sizeof(data), off, v);
    EXPECT_FALSE(ret);
    EXPECT_EQ(0u, off);
}

struct return_false_map_val_visitor : msgpack::null_visitor {
    return_false_map_val_visitor(std::size_t& times):m_times(times) {}
    bool visit_positive_integer(uint64_t) {
        if (++m_times == 2) return false;
        return true;
    }
    std::size_t& m_times;
};

TEST(visitor, return_false_map_val)
{
    std::size_t times = 0;
    return_false_map_val_visitor v(times);
    std::size_t off = 0;
    char const data[] = { static_cast<char>(0x83u), 0x01u, 0x02u, 0x03u, 0x01u, 0x02u, 0x03u };
    bool ret = msgpack::parse(data, sizeof(data), off, v);
    EXPECT_FALSE(ret);
    EXPECT_EQ(2u, times);
#if MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(0u, off);
#else  // MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(2u, off);
#endif // MSGPACK_DEFAULT_API_VERSION < 3
}

struct return_false_start_map_key_visitor : msgpack::null_visitor {
    return_false_start_map_key_visitor(std::size_t& times):m_times(times) {}
    bool start_map_key() {
        if (++m_times == 2) return false;
        return true;
    }
    std::size_t& m_times;
};

TEST(visitor, return_false_start_map_key)
{
    std::size_t times = 0;
    return_false_start_map_key_visitor v(times);
    std::size_t off = 0;
    char const data[] = { static_cast<char>(0x83u), 0x01u, 0x02u, 0x03u, 0x01u, 0x02u, 0x03u };
    bool ret = msgpack::parse(data, sizeof(data), off, v);
    EXPECT_FALSE(ret);
    EXPECT_EQ(2u, times);
#if MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(0u, off);
#else  // MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(3u, off);
#endif // MSGPACK_DEFAULT_API_VERSION < 3
}

struct return_false_end_map_key_visitor : msgpack::null_visitor {
    return_false_end_map_key_visitor(std::size_t& times):m_times(times) {}
    bool end_map_key() {
        if (++m_times == 2) return false;
        return true;
    }
    std::size_t& m_times;
};

TEST(visitor, return_false_end_map_key)
{
    std::size_t times = 0;
    return_false_end_map_key_visitor v(times);
    std::size_t off = 0;
    char const data[] = { static_cast<char>(0x83u), 0x01u, 0x02u, 0x03u, 0x01u, 0x02u, 0x03u };
    bool ret = msgpack::parse(data, sizeof(data), off, v);
    EXPECT_FALSE(ret);
    EXPECT_EQ(2u, times);
#if MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(0u, off);
#else  // MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(3u, off);
#endif // MSGPACK_DEFAULT_API_VERSION < 3
}

struct return_false_start_map_value_visitor : msgpack::null_visitor {
    return_false_start_map_value_visitor(std::size_t& times):m_times(times) {}
    bool start_map_value() {
        if (++m_times == 2) return false;
        return true;
    }
    std::size_t& m_times;
};

TEST(visitor, return_false_start_map_value)
{
    std::size_t times = 0;
    return_false_start_map_value_visitor v(times);
    std::size_t off = 0;
    char const data[] = { static_cast<char>(0x83u), 0x01u, 0x02u, 0x03u, 0x01u, 0x02u, 0x03u };
    bool ret = msgpack::parse(data, sizeof(data), off, v);
    EXPECT_FALSE(ret);
    EXPECT_EQ(2u, times);
#if MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(0u, off);
#else  // MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(4u, off);
#endif // MSGPACK_DEFAULT_API_VERSION < 3
}

struct return_false_end_map_value_visitor : msgpack::null_visitor {
    return_false_end_map_value_visitor(std::size_t& times):m_times(times) {}
    bool end_map_value() {
        if (++m_times == 2) return false;
        return true;
    }
    std::size_t& m_times;
};

TEST(visitor, return_false_end_map_value)
{
    std::size_t times = 0;
    return_false_end_map_value_visitor v(times);
    std::size_t off = 0;
    char const data[] = { static_cast<char>(0x83u), 0x01u, 0x02u, 0x03u, 0x01u, 0x02u, 0x03u };
    bool ret = msgpack::parse(data, sizeof(data), off, v);
    EXPECT_FALSE(ret);
    EXPECT_EQ(2u, times);
#if MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(0u, off);
#else  // MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(4u, off);
#endif // MSGPACK_DEFAULT_API_VERSION < 3
}

struct return_false_start_map_visitor : msgpack::null_visitor {
    bool start_map(uint32_t) {
        return false;
    }
};

TEST(visitor, return_false_start_map)
{
    return_false_start_map_visitor v;
    std::size_t off = 0;
    char const data[] = { static_cast<char>(0x83u), 0x01u, 0x02u, 0x03u, 0x01u, 0x02u, 0x03u };
    bool ret = msgpack::parse(data, sizeof(data), off, v);
    EXPECT_FALSE(ret);
    EXPECT_EQ(0u, off);
}

struct return_false_start_map0_visitor : msgpack::null_visitor {
    bool start_map(uint32_t) {
        return false;
    }
};

TEST(visitor, return_false_start_map0)
{
    return_false_start_map0_visitor v;
    std::size_t off = 0;
    char const data[] = { static_cast<char>(0x80u) };
    bool ret = msgpack::parse(data, sizeof(data), off, v);
    EXPECT_FALSE(ret);
    EXPECT_EQ(0u, off);
}

struct return_false_end_map_visitor : msgpack::null_visitor {
    bool end_map() {
        return false;
    }
};

TEST(visitor, return_false_end_map)
{
    return_false_end_map_visitor v;
    std::size_t off = 0;
    char const data[] = { static_cast<char>(0x83u), 0x01u, 0x02u, 0x03u, 0x01u, 0x02u, 0x03u };
    bool ret = msgpack::parse(data, sizeof(data), off, v);
    EXPECT_FALSE(ret);
#if MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(0u, off);
#else  // MSGPACK_DEFAULT_API_VERSION < 3
    EXPECT_EQ(6u, off);
#endif // MSGPACK_DEFAULT_API_VERSION < 3
}

struct return_false_end_map0_visitor : msgpack::null_visitor {
    bool end_map() {
        return false;
    }
};

TEST(visitor, return_false_end_map0)
{
    return_false_end_map0_visitor v;
    std::size_t off = 0;
    char const data[] = { static_cast<char>(0x80u) };
    bool ret = msgpack::parse(data, sizeof(data), off, v);
    EXPECT_FALSE(ret);
    EXPECT_EQ(0u, off);
}

#endif // MSGPACK_DEFAULT_API_VERSION >= 1
