#ifndef __ET_SOCKET_HANDLER__
#define __ET_SOCKET_HANDLER__

#include "Headers.hpp"
#include "Packet.hpp"

namespace et {
class SocketHandler {
 public:
  virtual ~SocketHandler() {}

  virtual bool hasData(int fd) = 0;
  virtual ssize_t read(int fd, void* buf, size_t count) = 0;
  virtual ssize_t write(int fd, const void* buf, size_t count) = 0;

  void readAll(int fd, void* buf, size_t count, bool timeout);
  int writeAllOrReturn(int fd, const void* buf, size_t count);
  void writeAllOrThrow(int fd, const void* buf, size_t count, bool timeout);

  template <typename T>
  inline T readProto(int fd, bool timeout) {
    T t;
    int64_t length;
    readAll(fd, &length, sizeof(int64_t), timeout);
    if (length < 0 || length > 128 * 1024 * 1024) {
      // If the message is <= 0 or too big, assume this is a bad packet and
      // throw
      string s = string("Invalid size (<0 or >128 MB): ") + to_string(length);
      throw std::runtime_error(s.c_str());
    }
    if (length == 0) {
      return t;
    }
    string s(length, '\0');
    readAll(fd, &s[0], length, timeout);
    if (!t.ParseFromString(s)) {
      throw std::runtime_error("Invalid proto");
    }
    return t;
  }

  template <typename T>
  inline void writeProto(int fd, const T& t, bool timeout) {
    lock_guard<std::mutex> guard(writeMutex);
    string s;
    if (!t.SerializeToString(&s)) {
      STFATAL << "Serialization of " << t.GetTypeName() << " failed!";
    }
    int64_t length = s.length();
    if (length < 0 || length > 128 * 1024 * 1024) {
      STFATAL << "Invalid proto length: " << length << " For proto "
              << t.GetTypeName();
    }
    writeAllOrThrow(fd, &length, sizeof(int64_t), timeout);
    if (length > 0) {
      writeAllOrThrow(fd, &s[0], length, timeout);
    }
  }

  inline bool readPacket(int fd, Packet* packet) {
    int64_t length;
    readAll(fd, (char*)&length, sizeof(int64_t), false);
    if (length < 0 || length > 128 * 1024 * 1024) {
      // If the message is < 0 or too big, assume this is a bad packet and throw
      string s("Invalid size (<0 or >128 MB): ");
      s += std::to_string(length);
      throw std::runtime_error(s.c_str());
    }
    if (length == 0) {
      return false;
    }
    string s(length, '\0');
    readAll(fd, &s[0], length, false);
    *packet = Packet(s);
    return true;
  }

  inline void writePacket(int fd, const Packet& packet) {
    string s = packet.serialize();
    int64_t length = s.length();
    if (length < 0 || length > 128 * 1024 * 1024) {
      STFATAL << "Invalid message length: " << length;
    }
    writeAllOrThrow(fd, (const char*)&length, sizeof(int64_t), false);
    if (length) {
      writeAllOrThrow(fd, &s[0], length, false);
    }
  }

  inline void writeB64(int fd, const char* buf, size_t count) {
    size_t encodedLength = base64::Base64::EncodedLength(count);
    string s(encodedLength, '\0');
    if (!base64::Base64::Encode(buf, count, &s[0], s.length())) {
      throw runtime_error("b64 decode failed");
    }
    writeAllOrThrow(fd, &s[0], s.length(), false);
  }

  inline void readB64(int fd, char* buf, size_t count) {
    size_t encodedLength = base64::Base64::EncodedLength(count);
    string s(encodedLength, '\0');
    readAll(fd, &s[0], s.length(), false);
    if (!base64::Base64::Decode((const char*)&s[0], s.length(), buf, count)) {
      throw runtime_error("b64 decode failed");
    }
  }

  inline void readB64EncodedLength(int fd, string* out, size_t encodedLength) {
    string s(encodedLength, '\0');
    readAll(fd, &s[0], s.length(), false);
    if (!base64::Base64::Decode(s, out)) {
      throw runtime_error("b64 decode failed");
    }
  }

  virtual int connect(const SocketEndpoint& endpoint) = 0;
  virtual set<int> listen(const SocketEndpoint& endpoint) = 0;
  virtual set<int> getEndpointFds(const SocketEndpoint& endpoint) = 0;
  virtual int accept(int fd) = 0;
  virtual void stopListening(const SocketEndpoint& endpoint) = 0;
  virtual void close(int fd) = 0;
  virtual vector<int> getActiveSockets() = 0;

 protected:
  // This mutex only exists to remove a tsan error in integration tests
  mutex writeMutex;
};
}  // namespace et

#endif  // __ET_SOCKET_HANDLER__
