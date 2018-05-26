#ifndef __ETERNAL_TCP_RAW_SOCKET_UTILS__
#define __ETERNAL_TCP_RAW_SOCKET_UTILS__

#include "Headers.hpp"

#include "base64.hpp"

namespace et {
class RawSocketUtils {
 public:
  static void writeAll(int fd, const char* buf, size_t count);

  static void readAll(int fd, char* buf, size_t count);

  static inline void writeB64(int fd, const char* buf, size_t count) {
    int encodedLength = base64::Base64::EncodedLength(count);
    string s(encodedLength, '\0');
    if (!base64::Base64::Encode(buf, count, &s[0], s.length())) {
      throw runtime_error("b64 decode failed");
    }
    writeAll(fd, &s[0], s.length());
  }

  static inline void readB64(int fd, char* buf, size_t count) {
    int encodedLength = base64::Base64::EncodedLength(count);
    string s(encodedLength, '\0');
    readAll(fd, &s[0], s.length());
    if(!base64::Base64::Decode((const char*)&s[0], s.length(), buf, count)) {
      throw runtime_error("b64 decode failed");
    }
  }

  static inline void readB64EncodedLength(int fd, string* out, size_t encodedLength) {
    string s(encodedLength, '\0');
    readAll(fd, &s[0], s.length());
    if(!base64::Base64::Decode(s, out)) {
      throw runtime_error("b64 decode failed");
    }
  }

  static inline string readMessage(int fd) {
    int64_t length;
    readAll(fd, (char*)&length, sizeof(int64_t));
    if (length < 0 || length > 128 * 1024 * 1024) {
      // If the message is < 0 or too big, assume this is a bad packet and throw
      string s("Invalid size (<0 or >128 MB):");
      s += std::to_string(length);
      throw std::runtime_error(s.c_str());
    }
    string s(length, 0);
    readAll(fd, &s[0], length);
    return s;
  }

  static inline void writeMessage(int fd, const string& s) {
    int64_t length = s.length();
    writeAll(fd, (const char*)&length, sizeof(int64_t));
    writeAll(fd, &s[0], length);
  }

  template <typename T>
  static inline T readProto(int fd) {
    T t;
    int64_t length;
    readAll(fd, (char*)&length, sizeof(int64_t));
    if (length < 0 || length > 128 * 1024 * 1024) {
      // If the message is < 0 or too big, assume this is a bad packet and throw
      string s("Invalid size (<0 or >128 MB):");
      s += std::to_string(length);
      throw std::runtime_error(s.c_str());
    }
    string s(length, 0);
    readAll(fd, &s[0], length);
    if (!t.ParseFromString(s)) {
      throw std::runtime_error("Invalid proto");
    }
    return t;
  }

  template <typename T>
  static inline T readProtoJson(int fd) {
    T t;
    int64_t length;
    readAll(fd, (char*)&length, sizeof(int64_t));
    if (length < 0 || length > 128 * 1024 * 1024) {
      // If the message is < 0 or too big, assume this is a bad packet and throw
      string s("Invalid size (<0 or >128 MB):");
      s += std::to_string(length);
      throw std::runtime_error(s.c_str());
    }
    string s(length, 0);
    readAll(fd, &s[0], length);
    auto status = google::protobuf::util::JsonStringToMessage(s, &t);
    VLOG(1) << "STATUS: " << status;
    return t;
  }

  template <typename T>
  static inline void writeProto(int fd, const T& t) {
    string s;
    t.SerializeToString(&s);
    int64_t length = s.length();
    writeAll(fd, (const char*)&length, sizeof(int64_t));
    writeAll(fd, &s[0], length);
  }

  template <typename T>
  static inline void writeProtoJson(int fd, const T& t) {
    string s;
    auto status = google::protobuf::util::MessageToJsonString(t, &s);
    VLOG(1) << "STATUS: " << status;
    int64_t length = s.length();
    writeAll(fd, (const char*)&length, sizeof(int64_t));
    writeAll(fd, &s[0], length);
  }
};
}  // namespace et
#endif  // __ETERNAL_TCP_RAW_SOCKET_UTILS__
