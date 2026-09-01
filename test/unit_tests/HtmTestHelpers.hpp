#ifndef __HTM_TEST_HELPERS_HPP__
#define __HTM_TEST_HELPERS_HPP__

#include <chrono>
#include <functional>
#include <thread>

#include "HtmHeaderCodes.hpp"
#include "JsonLib.hpp"
#include "PipeSocketHandler.hpp"
#include "TestHeaders.hpp"
#include "base64.h"

#ifdef WIN32
#include <windows.h>
#endif

#if defined(__has_feature)
#define ET_HTM_TSAN_FEATURE __has_feature(thread_sanitizer)
#else
#define ET_HTM_TSAN_FEATURE 0
#endif

namespace et {
namespace htmtest {

inline bool runningUnderThreadSanitizer() {
#if defined(__SANITIZE_THREAD__) || ET_HTM_TSAN_FEATURE
  return true;
#else
  return false;
#endif
}

inline void skipIfThreadSanitizer() {
  if (runningUnderThreadSanitizer()) {
    SKIP("forkpty from a worker thread is unsupported under ThreadSanitizer");
  }
}

inline string b64Int32(int32_t value) {
  string encoded(Base64::EncodedLength(4), '\0');
  REQUIRE(Base64::Encode(reinterpret_cast<const char*>(&value), 4, &encoded[0],
                         encoded.size()));
  return encoded;
}

inline string b64Bytes(const string& data) {
  if (data.empty()) {
    return "";
  }
  string encoded(Base64::EncodedLength(data.size()), '\0');
  REQUIRE(
      Base64::Encode(data.data(), data.size(), &encoded[0], encoded.size()));
  return encoded;
}

inline int32_t decodeB64Int32(const string& encoded) {
  int32_t value = 0;
  REQUIRE(encoded.size() >= 8);
  REQUIRE(
      Base64::Decode(encoded.data(), 8, reinterpret_cast<char*>(&value), 4));
  return value;
}

class UniqueIpcPath {
 public:
  UniqueIpcPath() {
#ifdef WIN32
    string root = GetTempDirectory();
    dir = root + "htm_test_" + to_string(GetCurrentProcessId()) + "_" +
          to_string(GetTickCount64());
    REQUIRE(CreateDirectoryA(dir.c_str(), NULL) != 0);
    path = dir + "\\ipc";
#else
    string tmpl = GetTempDirectory() + string("htm_test_XXXXXX");
    char* created = mkdtemp(&tmpl[0]);
    REQUIRE(created != nullptr);
    dir = created;
    path = dir + "/ipc";
#endif
  }
  ~UniqueIpcPath() {
    ::remove(path.c_str());
#ifdef WIN32
    RemoveDirectoryA(dir.c_str());
#else
    ::remove(dir.c_str());
#endif
  }
  string dir;
  string path;
};

inline SocketEndpoint endpointFor(const string& path) {
  SocketEndpoint endpoint;
  endpoint.set_name(path);
  return endpoint;
}

inline string readUntil(shared_ptr<SocketHandler> handler, int fd,
                        size_t minBytes, int timeoutMs = 3000) {
  string out;
  const auto start = std::chrono::steady_clock::now();
  while (out.size() < minBytes) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    if (elapsed > timeoutMs) {
      break;
    }
    if (handler->hasData(fd)) {
      char buf[4096];
      ssize_t n = handler->read(fd, buf, sizeof(buf));
      if (n > 0) {
        out.append(buf, n);
      } else {
        break;
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  return out;
}

inline bool waitUntil(const std::function<bool()>& pred, int timeoutMs = 5000) {
  const auto start = std::chrono::steady_clock::now();
  while (!pred()) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    if (elapsed > timeoutMs) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return true;
}

inline void sendPacket(shared_ptr<SocketHandler> handler, int fd, char header,
                       const string& payload) {
  handler->writeAllOrThrow(fd, &header, 1, false);
  int32_t length = int32_t(payload.size());
  handler->writeB64(fd, reinterpret_cast<const char*>(&length), 4);
  if (!payload.empty()) {
    handler->writeAllOrThrow(fd, payload.data(), payload.size(), false);
  }
}

inline void sendInsertKeys(shared_ptr<SocketHandler> handler, int fd,
                           const string& paneId, const string& data) {
  string encoded = b64Bytes(data);
  sendPacket(handler, fd, INSERT_KEYS, paneId + encoded);
}

inline void sendDebugKeys(shared_ptr<SocketHandler> handler, int fd,
                          const string& keys) {
  sendPacket(handler, fd, INSERT_DEBUG_KEYS, keys);
}

struct HtmPacket {
  char header;
  string payload;
};

inline bool popPacket(string* buffer, HtmPacket* out) {
  if (buffer->empty()) {
    return false;
  }
  char header = (*buffer)[0];
  if (header == SESSION_END) {
    out->header = header;
    out->payload.clear();
    buffer->erase(0, 1);
    return true;
  }
  if (buffer->size() < 9) {
    return false;
  }
  int32_t length = decodeB64Int32(buffer->substr(1, 8));
  if (length < 0 || buffer->size() < 9u + size_t(length)) {
    return false;
  }
  out->header = header;
  out->payload = buffer->substr(9, size_t(length));
  buffer->erase(0, 9 + size_t(length));
  return true;
}

inline void consumeInitSequence(string* buffer) {
  static const string kInit = "\x1b[###q";
  auto pos = buffer->find(kInit);
  if (pos != string::npos) {
    buffer->erase(0, pos + kInit.size());
  }
}

inline json firstJsonValue(const json& object) {
  REQUIRE(object.is_object());
  REQUIRE_FALSE(object.empty());
  return object.begin().value();
}

inline string firstJsonKey(const json& object) {
  REQUIRE(object.is_object());
  REQUIRE_FALSE(object.empty());
  return object.begin().key();
}

inline bool decodeAppendToPane(const HtmPacket& packet, string* paneId,
                               string* body) {
  static const size_t kUuidLen = 36;
  if (packet.header != APPEND_TO_PANE || packet.payload.size() < kUuidLen) {
    return false;
  }
  *paneId = packet.payload.substr(0, kUuidLen);
  string encoded = packet.payload.substr(kUuidLen);
  if (encoded.empty()) {
    body->clear();
    return true;
  }
  return Base64::Decode(encoded, body);
}

}  // namespace htmtest
}  // namespace et

#endif
