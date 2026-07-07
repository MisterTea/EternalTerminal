#ifndef __ET_SOCKET_HANDLER__
#define __ET_SOCKET_HANDLER__

#include "Headers.hpp"
#include "Packet.hpp"

namespace et {
/**
 * @brief Provides an abstract API for socket reads/writes and lifecycle
 * management.
 */
class SocketHandler {
 public:
  /** @brief Ensures derived handlers can clean up platform-specific resources.
   */
  virtual ~SocketHandler() {}

  /**
   * @brief Returns true when the kernel reports data ready to read on a
   * descriptor.
   */
  virtual bool hasData(int fd) = 0;
  /**
   * @brief Reads up to count bytes from fd.
   */
  virtual ssize_t read(int fd, void* buf, size_t count) = 0;
  /**
   * @brief Writes up to count bytes to fd.
   */
  virtual ssize_t write(int fd, const void* buf, size_t count) = 0;

  /**
   * @brief Reads exactly `count` bytes, retrying on EAGAIN until the buffer
   * fills.
   * @param timeout Whether to enforce the internal transfer timeout while
   * waiting.
   */
  void readAll(int fd, void* buf, size_t count, bool timeout);
  /**
   * @brief Attempts to write the full buffer and returns -1 on timeout/failure.
   * @return Total bytes written or -1 when the socket deadlocks.
   */
  int writeAllOrReturn(int fd, const void* buf, size_t count);
  /**
   * @brief Attempts to write all bytes, throwing if the operation times out or
   * fails.
   */
  void writeAllOrThrow(int fd, const void* buf, size_t count, bool timeout);

  /**
   * @brief Reads a length-prefixed protobuf from the socket.
   * @tparam T Protobuf message type.
   * @throws std::runtime_error on invalid length or parse failure.
   */
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

  /**
   * @brief Serializes and writes a length-prefixed protobuf message.
   */
  template <typename T>
  inline void writeProto(int fd, const T& t, bool timeout) {
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

  /**
   * @brief Reads a length-prefixed binary packet and deserializes it.
   * @returns false when the packet length is zero (empty message).
   */
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

  /**
   * @brief Serializes and writes a packet with a leading length prefix.
   */
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

  /** @brief Sends a base64-encoded version of the provided buffer. */
  void writeB64(int fd, const char* buf, size_t count);
  /** @brief Reads a fixed amount of base64-encoded data and decodes it. */
  void readB64(int fd, char* buf, size_t count);
  /**
   * @brief Reads an explicitly encoded length string and decodes it into @p
   * out.
   * @param encodedLength Byte count of the supplied base64 string.
   */
  void readB64EncodedLength(int fd, string* out, size_t encodedLength);

  /**
   * @brief Opens a connection to the specified endpoint.
   * @return File descriptor representing the socket (or -1 on failure).
   */
  virtual int connect(const SocketEndpoint& endpoint) = 0;
  /**
   * @brief Starts listening on the endpoint and returns the active listen fds.
   */
  virtual set<int> listen(const SocketEndpoint& endpoint) = 0;
  /**
   * @brief Returns any fds associated with the endpoint (listening or
   * otherwise).
   */
  virtual set<int> getEndpointFds(const SocketEndpoint& endpoint) = 0;
  /**
   * @brief Accepts a pending connection on the given listening fd.
   */
  virtual int accept(int fd) = 0;
  /**
   * @brief Stops accepting new connections on the given endpoint.
   */
  virtual void stopListening(const SocketEndpoint& endpoint) = 0;
  /** @brief Closes the supplied socket descriptor. */
  virtual void close(int fd) = 0;
  /** @brief Returns all currently active (read/write) sockets. */
  virtual vector<int> getActiveSockets() = 0;
};
}  // namespace et

#endif  // __ET_SOCKET_HANDLER__
