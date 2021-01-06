#ifndef __ET_HEADERS__
#define __ET_HEADERS__

#define CPPHTTPLIB_ZLIB_SUPPORT (1)
#define CPPHTTPLIB_OPENSSL_SUPPORT (1)
// httplib has to come before windows.h
#include "httplib.h"

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
#elif WIN32
#include <WinSock2.h>
#include <Ws2tcpip.h>
#include <afunix.h>
#include <signal.h>

#include <codecvt>
inline int close(int fd) { return ::closesocket(fd); }
#else
#include <pty.h>
#include <signal.h>
#endif

#ifdef WIN32
#else
#include <arpa/inet.h>
#include <grp.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <paths.h>
#include <pthread.h>
#include <pwd.h>
#include <resolv.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <google/protobuf/message_lite.h>
#include <sodium.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <ctime>
#include <cxxopts.hpp>
#include <deque>
#include <exception>
#include <fstream>
#include <iostream>
#include <locale>
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

#include "ET.pb.h"
#include "ETerminal.pb.h"
#include "ThreadPool.h"
#include "base64.hpp"
#include "easylogging++.h"
#include "json.hpp"
#include "sole.hpp"
#if !defined(__ANDROID__)
#include "ust.hpp"
#endif

#ifdef WITH_UTEMPTER
#include <utempter.h>
#endif

#include "sentry.h"

#if WIN32
#define popen _popen
#define pclose _pclose

/* ssize_t is not defined on Windows */
#ifndef ssize_t
#if defined(_WIN64)
typedef signed __int64 ssize_t;
#else
typedef signed long ssize_t;
#endif
#endif /* !ssize_t */

/* On MSVC, ssize_t is SSIZE_T */
#ifdef _MSC_VER
#include <BaseTsd.h>
#define ssize_t SSIZE_T
#endif

#endif

using namespace std;

using json = nlohmann::json;

// The ET protocol version supported by this binary
static const int PROTOCOL_VERSION = 6;

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

#if defined(__ANDROID__)
#define STFATAL LOG(FATAL) << "No Stack Trace on Android" << endl

#define STERROR LOG(ERROR) << "No Stack Trace on Android" << endl
#else
#define STFATAL LOG(FATAL) << "Stack Trace: " << endl << ust::generate()

#define STERROR LOG(ERROR) << "Stack Trace: " << endl << ust::generate()
#endif

#ifdef WIN32
inline string WindowsErrnoToString() {
  const int BUFSIZE = 4096;
  char buf[BUFSIZE];
  FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 NULL, WSAGetLastError(),
                 MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf, BUFSIZE, NULL);
  string s(buf, BUFSIZE);
  return s;
}
#define FATAL_FAIL(X)                             \
  if (((X) == -1))                                \
    LOG(FATAL) << "Error: (" << WSAGetLastError() \
               << "): " << WindowsErrnoToString();

#define FATAL_FAIL_UNLESS_EINVAL(X) FATAL_FAIL(X)

#else
#define FATAL_FAIL(X) \
  if (((X) == -1)) STFATAL << "Error: (" << errno << "): " << strerror(errno);

// On BSD/OSX we can get EINVAL if the remote side has closed the connection
// before we have initialized it.
#define FATAL_FAIL_UNLESS_EINVAL(X)   \
  if (((X) == -1) && errno != EINVAL) \
    STFATAL << "Error: (" << errno << "): " << strerror(errno);
#endif

#ifndef ET_VERSION
#define ET_VERSION "unknown"
#endif

namespace et {
inline std::ostream& operator<<(std::ostream& os,
                                const et::SocketEndpoint& se) {
  if (se.has_name()) {
    os << se.name();
  }
  if (se.has_port()) {
    os << ":" << se.port();
  }
  return os;
}

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

template <typename T>
inline T stringToProto(const string& s) {
  T t;
  if (!t.ParseFromString(s)) {
    STFATAL << "Error parsing string to proto: " << s.length() << " " << s;
  }
  return t;
}

template <typename T>
inline string protoToString(const T& t) {
  string s;
  if (!t.SerializeToString(&s)) {
    STFATAL << "Error serializing proto to string";
  }
  return s;
}

inline bool waitOnSocketData(int fd) {
  fd_set fdset;
  FD_ZERO(&fdset);
  FD_SET(fd, &fdset);
  timeval tv;
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  VLOG(4) << "Before selecting sockFd";
  FATAL_FAIL(select(fd + 1, &fdset, NULL, NULL, &tv));
  return FD_ISSET(fd, &fdset);
}

inline string genRandomAlphaNum(int len) {
  static const char alphanum[] =
      "0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz";
  string s(len, '\0');

  for (int i = 0; i < len; ++i) {
    s[i] = alphanum[randombytes_uniform(sizeof(alphanum) - 1)];
  }

  return s;
}

}  // namespace et

inline bool operator==(const google::protobuf::MessageLite& msg_a,
                       const google::protobuf::MessageLite& msg_b) {
  return (msg_a.GetTypeName() == msg_b.GetTypeName()) &&
         (msg_a.SerializeAsString() == msg_b.SerializeAsString());
}

inline bool operator!=(const google::protobuf::MessageLite& msg_a,
                       const google::protobuf::MessageLite& msg_b) {
  return (msg_a.GetTypeName() != msg_b.GetTypeName()) ||
         (msg_a.SerializeAsString() != msg_b.SerializeAsString());
}

inline string GetTempDirectory() {
#ifdef WIN32
  WCHAR buf[65536];
  int retval = GetTempPath(65536, buf);
  int a = 0;
  std::wstring_convert<std::codecvt_utf8_utf16<wchar_t> > converter;
  std::string tmpDir = converter.to_bytes(wstring(buf, retval));
#else
  string tmpDir = _PATH_TMP;
#endif
  return tmpDir;
}

#endif
