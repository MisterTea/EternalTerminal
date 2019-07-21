#ifndef __MESSAGE_WRITER_H__
#define __MESSAGE_WRITER_H__

#include "Headers.hpp"

namespace et {
class MessageWriter {
 public:
  MessageWriter() : packHandler(buffer) {}

  inline void start() { buffer.clear(); }

  template <typename T>
  inline void writePrimitive(const T& t) {
    packHandler.pack(t);
  }

  template <typename K, typename V>
  inline void writeMap(const map<K, V>& m) {
    packHandler.pack_map(uint32_t(m.size()));
    for (auto& it : m) {
      writePrimitive(it.first);
      writePrimitive(it.second);
    }
  }

  template <typename T>
  inline void writeClass(const T& t) {
    string s(sizeof(T), '\0');
    memcpy(&s[0], &t, sizeof(T));
    writePrimitive<string>(s);
  }

  template <typename T>
  inline void writeProto(const T& t) {
    string s;
    t.SerializeToString(&s);
    writePrimitive<string>(s);
  }

  inline string finish() {
    string s(buffer.data(), buffer.size());
    start();
    return s;
  }

  inline int64_t size() { return buffer.size(); }

 protected:
  msgpack::sbuffer buffer;
  msgpack::packer<msgpack::sbuffer> packHandler;
};
}  // namespace et

#endif  // __MESSAGE_WRITER_H__