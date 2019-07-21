#ifndef __MESSAGE_READER_H__
#define __MESSAGE_READER_H__

#include "Headers.hpp"

namespace et {
class MessageReader {
 public:
  MessageReader() {}

  MessageReader(const string& s) { load(s); }

  inline void load(const string& s) {
    unpackHandler.remove_nonparsed_buffer();
    unpackHandler.reserve_buffer(s.size());
    memcpy(unpackHandler.buffer(), s.c_str(), s.length());
    unpackHandler.buffer_consumed(s.length());
  }

  template <unsigned long i>
  inline void load(const std::array<char, i>& a, int size) {
    unpackHandler.remove_nonparsed_buffer();
    unpackHandler.reserve_buffer(size);
    memcpy(unpackHandler.buffer(), &a[0], size);
    unpackHandler.buffer_consumed(size);
  }

  template <typename T>
  inline T readPrimitive() {
    msgpack::object_handle oh;
    FATAL_IF_FALSE(unpackHandler.next(oh));
    T t = oh.get().convert();
    return t;
  }

  template <typename K, typename V>
  inline map<K, V> readMap() {
    msgpack::object_handle oh;
    FATAL_IF_FALSE(unpackHandler.next(oh));
    map<K, V> t = oh.get().convert();
    return t;
  }

  template <typename T>
  inline T readClass() {
    T t;
    string s = readPrimitive<string>();
    if (s.length() != sizeof(T)) {
      throw std::runtime_error("Invalid Class Size");
    }
    memcpy(&t, &s[0], sizeof(T));
    return t;
  }

  template <typename T>
  inline T readProto() {
    T t;
    string s = readPrimitive<string>();
    if (!t.ParseFromString(s)) {
      throw std::runtime_error("Invalid proto");
    }
    return t;
  }

  inline int64_t sizeRemaining() {
    // TODO: Make sure this is accurate
    return unpackHandler.nonparsed_size();
  }

 protected:
  msgpack::unpacker unpackHandler;
};
}  // namespace et

#endif  // __MESSAGE_READER_H__