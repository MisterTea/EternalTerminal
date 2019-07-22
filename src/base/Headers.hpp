#ifndef __ET_HEADERS__
#define __ET_HEADERS__

#if __FreeBSD__
#define _WITH_GETLINE
#endif

#if __APPLE__
#include <sys/ucred.h>
#include <util.h>
#elif __FreeBSD__
#include <libutil.h>
#include <sys/socket.h>
#elif __NetBSD__  // do not need pty.h on NetBSD
#include <util.h>
#else
#include <pty.h>
#include <signal.h>
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <pwd.h>
#include <resolv.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <ctime>
#include <deque>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <gflags/gflags.h>

#include "easylogging++.h"
#include "msgpack.hpp"

#include "base64.hpp"
#include "json.hpp"
#include "sole.hpp"

#include "ctpl_stl.h"

using namespace std;

namespace google {}
namespace gflags {}
using namespace google;
using namespace gflags;

using json = nlohmann::json;
using namespace ctpl;

// The ET protocol version supported by this binary
static const int PROTOCOL_VERSION = 4;

// Nonces for CryptoHandler
static const unsigned char CLIENT_SERVER_NONCE_MSB = 0;
static const unsigned char SERVER_CLIENT_NONCE_MSB = 1;

// system ssh config files
const string SYSTEM_SSH_CONFIG_PATH = "/etc/ssh/ssh_config";
const string USER_SSH_CONFIG_PATH = "/.ssh/config";

// Keepalive configs
const int CLIENT_KEEP_ALIVE_DURATION = 5;
// This should be at least double the value of CLIENT_KEEP_ALIVE_DURATION to
// allow enough time.
const int SERVER_KEEP_ALIVE_DURATION = 11;

#define FATAL_FAIL(X) \
  if (((X) == -1))    \
    LOG(FATAL) << "Error: (" << errno << "): " << strerror(errno);

#define FATAL_IF_FALSE(X)  \
  if (!(X)) {              \
    LOG(FATAL) << "Error"; \
  }

// On BSD/OSX we can get EINVAL if the remote side has closed the connection
// before we have initialized it.
#define FATAL_FAIL_UNLESS_EINVAL(X)   \
  if (((X) == -1) && errno != EINVAL) \
    LOG(FATAL) << "Error: (" << errno << "): " << strerror(errno);

#ifndef ET_VERSION
#define ET_VERSION "unknown"
#endif

namespace et {
enum ConnectStatus {
  NEW_CLIENT = 1,
  RETURNING_CLIENT = 2,
  INVALID_KEY = 3,
  MISMATCHED_PROTOCOL = 4,
};

enum EtPacketType {
  // Count down from 254 to avoid collisions
  HEARTBEAT = 254,
  INITIAL_PAYLOAD = 253,
};

enum TerminalPacketType {
  KEEP_ALIVE = 0,
  TERMINAL_BUFFER = 1,
  TERMINAL_INFO = 2,
  PORT_FORWARD_SOURCE_REQUEST = 3,
  PORT_FORWARD_SOURCE_RESPONSE = 4,
  PORT_FORWARD_DESTINATION_REQUEST = 5,
  PORT_FORWARD_DESTINATION_RESPONSE = 6,
  PORT_FORWARD_DATA = 7,
  IDPASSKEY = 8,
};

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
}  // namespace et

#endif
