#ifndef __ETERNAL_TCP_HEADERS__
#define __ETERNAL_TCP_HEADERS__

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h> /* POSIX Threads */
#include <stdint.h>
#if __FreeBSD__
#define _WITH_GETLINE
#endif
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if __APPLE__
#include <sys/ucred.h>
#include <util.h>
#elif __FreeBSD__
#include <libutil.h>
#elif __NetBSD__  // do not need pty.h on NetBSD
#else
#include <pty.h>
#endif

#if __NetBSD__
#include <util.h>
#endif

#include <pwd.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <deque>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <gflags/gflags.h>

#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>
#include "ET.pb.h"
#include "easylogging++.h"

using namespace std;

namespace google {}
namespace gflags {}
using namespace google;
using namespace gflags;

// The ET protocol version supported by this binary
static const int PROTOCOL_VERSION = 4;

// Nonces for CryptoHandler
static const unsigned char CLIENT_SERVER_NONCE_MSB = 0;
static const unsigned char SERVER_CLIENT_NONCE_MSB = 1;

#define FATAL_FAIL(X) \
  if (((X) == -1))    \
    LOG(FATAL) << "Error: (" << errno << "): " << strerror(errno);

template <typename Out>
inline void split(const std::string& s, char delim, Out result) {
  std::stringstream ss;
  ss.str(s);
  std::string item;
  while (std::getline(ss, item, delim)) {
    *(result++) = item;
  }
}

inline std::vector<std::string> split(const std::string& s, char delim) {
  std::vector<std::string> elems;
  split(s, delim, std::back_inserter(elems));
  return elems;
}

inline std::string SystemToStr(const char* cmd) {
  std::array<char, 128> buffer;
  std::string result;
  std::shared_ptr<FILE> pipe(popen(cmd, "r"), pclose);
  if (!pipe) throw std::runtime_error("popen() failed!");
  while (!feof(pipe.get())) {
    if (fgets(buffer.data(), 128, pipe.get()) != nullptr)
      result += buffer.data();
  }
  return result;
}

inline bool replace(std::string& str, const std::string& from,
                    const std::string& to) {
  size_t start_pos = str.find(from);
  if (start_pos == std::string::npos) return false;
  str.replace(start_pos, from.length(), to);
  return true;
}

inline int replaceAll(std::string& str, const std::string& from,
                      const std::string& to) {
  if (from.empty()) return 0;
  int retval = 0;
  size_t start_pos = 0;
  while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
    retval++;
    str.replace(start_pos, from.length(), to);
    start_pos += to.length();  // In case 'to' contains 'from', like replacing
                               // 'x' with 'yx'
  }
  return retval;
}

#endif
