#ifndef __HTM_TEST_HELPERS_HPP__
#define __HTM_TEST_HELPERS_HPP__

#include <chrono>
#include <functional>
#include <thread>

#include "ControlMode.hpp"
#include "PipeSocketHandler.hpp"
#include "TestHeaders.hpp"

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

class UniqueIpcPath {
 public:
  UniqueIpcPath() {
#ifdef WIN32
    path = "htm_test_" + to_string(GetCurrentProcessId()) + "_" +
           to_string(GetTickCount64()) + ".ipc";
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

inline void sendLine(shared_ptr<SocketHandler> handler, int fd,
                     const string& line) {
  string payload = line;
  if (payload.empty() || payload.back() != '\n') {
    payload.push_back('\n');
  }
  handler->writeAllOrThrow(fd, payload.data(), static_cast<int>(payload.size()),
                           false);
}

inline vector<string> splitLines(const string& text) {
  vector<string> lines;
  string cur;
  for (char c : text) {
    if (c == '\n') {
      if (!cur.empty() && cur.back() == '\r') {
        cur.pop_back();
      }
      lines.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) {
    lines.push_back(cur);
  }
  return lines;
}

inline bool hasLinePrefix(const vector<string>& lines, const string& prefix) {
  for (const string& line : lines) {
    if (line.compare(0, prefix.size(), prefix) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace htmtest
}  // namespace et

#endif
