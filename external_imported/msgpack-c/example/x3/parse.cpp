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

struct json_like_visitor : msgpack::v2::null_visitor {
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
    bool visit_float32(float v) {
        std::stringstream ss;
        ss << v;
        m_s += ss.str();
        return true;
    }
    bool visit_float64(double v) {
        std::stringstream ss;
        ss << v;
        m_s += ss.str();
        return true;
    }
    bool visit_str(const char* v, uint32_t size) {
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
    }
    void insufficient_bytes(size_t /*parsed_offset*/, size_t /*error_offset*/) {
    }
    std::string& m_s;
};

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
        std::string str;
        bool ret = msgpack::parse(it, end, json_like_visitor(str));
        // it is updated here.
        assert(ret);
        std::cout << str << std::endl;
    }
    {
        std::string str;
        bool ret = msgpack::parse(it, end, json_like_visitor(str));
        // it is updated here.
        assert(ret);
        std::cout << str << std::endl;
    }
}
