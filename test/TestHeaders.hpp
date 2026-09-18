#ifndef __TEST_HEADERS_HPP__
#define __TEST_HEADERS_HPP__

#include "Headers.hpp"

#undef CHECK
#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

inline void removeOrMissing(const string& path) {
  if (::remove(path.c_str()) != 0 && GetErrno() != ENOENT) {
    FATAL_FAIL(-1);
  }
}

inline void testSleepMicros(uint64_t micros) {
  std::this_thread::sleep_for(std::chrono::microseconds(micros));
}

#ifdef WIN32
// Portable substitute for POSIX sleep(3), used throughout the test suite.
// Named `sleep` so existing call sites compile unchanged on Windows.
inline void sleep(int seconds) {
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
}
#endif

namespace et {
namespace test {
// Creates a fresh empty directory for test sockets/files. Unix uses mkdtemp;
// Windows has no mkdtemp, so a random suffix plus create_directories is used.
inline string makeTempDir(const string& prefix) {
#ifdef WIN32
  string dir = GetTempDirectory() + prefix + "_" + genRandomAlphaNum(12);
  for (char& c : dir) {
    if (c == '\\') {
      c = '/';
    }
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    STFATAL << "Failed to create test temp dir: " << dir << ": "
            << ec.message();
  }
  return dir;
#else
  string tmpPath = GetTempDirectory() + prefix + "_XXXXXXXX";
  const char* created = mkdtemp(&tmpPath[0]);
  if (created == nullptr) {
    STFATAL << "mkdtemp failed: " << strerror(GetErrno());
  }
  return string(created);
#endif
}

// Removes a directory created by makeTempDir (and everything under it).
inline void removeTempDir(const string& dir) {
#ifdef WIN32
  std::error_code ec;
  fs::remove_all(dir, ec);
  if (ec) {
    STFATAL << "Failed to remove test temp dir: " << dir << ": "
            << ec.message();
  }
#else
  FATAL_FAIL(::remove(dir.c_str()));
#endif
}

// Closes a socket/pollable descriptor from a test socket pair.
inline void closeTestFd(int fd) {
#ifdef WIN32
  ::closesocket(fd);
#else
  ::close(fd);
#endif
}
}  // namespace test
}  // namespace et

#endif
