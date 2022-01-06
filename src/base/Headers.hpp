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
#elif defined(_MSC_VER)
#include <WinSock2.h>
#include <Ws2tcpip.h>
#include <afunix.h>
#include <signal.h>
#include <tchar.h>
#include <tlhelp32.h>
#include <windows.h>
#include <winerror.h>

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

#ifndef NO_TELEMETRY
#if __has_include(<filesystem>)
#include <filesystem>
#else
#include <experimental/filesystem>
using namespace std::experimental;
#endif
#endif

#include "ET.pb.h"
#include "ETerminal.pb.h"
#include "ThreadPool.h"
#include "base64.h"
#include "easylogging++.h"
#include "sago/platform_folders.h"
#include "sole.hpp"

#if !defined(__ANDROID__)
#include "ust.hpp"
#endif

#ifdef WITH_UTEMPTER
#include <utempter.h>
#endif

#if defined(_MSC_VER)
#define popen _popen
#define pclose _pclose

/* ssize_t is not defined on Windows */
#ifndef ssize_t
#if defined(_WIN64)
typedef signed __int64 ssize_t;
#else
typedef int ssize_t;
#endif
#endif /* !ssize_t */

/* On MSVC, ssize_t is SSIZE_T */
#ifdef _MSC_VER
#include <BaseTsd.h>
#define ssize_t SSIZE_T
#endif

#endif

using namespace std;

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

inline int GetErrno() {
#ifdef WIN32
  auto retval = WSAGetLastError();
  if (retval >= 10000) {
    // Do some translation
    switch (retval) {
      case WSAEWOULDBLOCK:
        return EWOULDBLOCK;
      case WSAEADDRINUSE:
        return EADDRINUSE;
      case WSAEINPROGRESS:
        return EINPROGRESS;
      case WSAENOTSOCK:
        return ENOTSOCK;
      case WSAECONNRESET:
        return ECONNRESET;
      case WSAECONNABORTED:
        return ECONNABORTED;
      case WSAECONNREFUSED:
        return ECONNREFUSED;
      default:
        STFATAL << "Unmapped WSA error: " << retval;
    }
  }
  return retval;
#else
  return errno;
#endif
}

inline void SetErrno(int e) {
#ifdef WIN32
  WSASetLastError(e);
#else
  errno = e;
#endif
}

#ifdef WIN32
inline string WinsockErrnoToString() {
  const int BUFSIZE = 4096;
  char buf[BUFSIZE];
  auto charsWritten = FormatMessageA(
      FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
      WSAGetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf,
      BUFSIZE, NULL);
  if (charsWritten) {
    string s(buf, charsWritten + 1);
    return s;
  }
  return "Unknown Error";
}

inline string WinErrnoToString() {
  const int BUFSIZE = 4096;
  char buf[BUFSIZE];
  auto charsWritten = FormatMessageA(
      FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
      GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf, BUFSIZE,
      NULL);
  if (charsWritten) {
    string s(buf, charsWritten + 1);
    return s;
  }
  return "Unknown Error";
}

#define FATAL_FAIL(X)                             \
  if (((X) == -1))                                \
    LOG(FATAL) << "Error: (" << WSAGetLastError() \
               << "): " << WinsockErrnoToString();

#define FATAL_FAIL_UNLESS_ZERO(X)                 \
  if (((X) != 0))                                 \
    LOG(FATAL) << "Error: (" << WSAGetLastError() \
               << "): " << WinsockErrnoToString();

template <class Type>
inline void FATAL_FAIL_IF_ZERO(Type X) {
  if (!X) {
    STFATAL << "Error: (" << GetLastError() << "): " << WinErrnoToString();
  }
}

#define FATAL_FAIL_UNLESS_EINVAL(X) FATAL_FAIL(X)

inline void FATAL_FAIL_UNLESS_S_OK(HRESULT X) {
  if (X != S_OK) {
    LPVOID lpMsgBuf;
    LPVOID lpDisplayBuf;

    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS,
                  NULL, X, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                  (LPTSTR)&lpMsgBuf, 0, NULL);

    STFATAL << "Error (" << X << "): " << ((LPCTSTR)lpMsgBuf);
  }
}

#define STDIN_FILENO fileno(stdin)
#define STDOUT_FILENO fileno(stdout)

#else
#define FATAL_FAIL(X) \
  if (((X) == -1))    \
    STFATAL << "Error: (" << GetErrno() << "): " << strerror(GetErrno());

// On BSD/OSX we can get EINVAL if the remote side has closed the connection
// before we have initialized it.
#define FATAL_FAIL_UNLESS_EINVAL(X)        \
  if (((X) == -1) && GetErrno() != EINVAL) \
    STFATAL << "Error: (" << GetErrno() << "): " << strerror(GetErrno());
#endif

#ifndef ET_VERSION
#define ET_VERSION "unknown"
#endif

namespace et {
inline std::ostream &operator<<(std::ostream &os,
                                const et::SocketEndpoint &se) {
  if (se.has_name()) {
    os << se.name();
  }
  if (se.has_port()) {
    os << ":" << se.port();
  }
  return os;
}

template <typename Out>
inline void split(const std::string &s, char delim, Out result) {
  std::stringstream ss;
  ss.str(s);
  std::string item;
  while (std::getline(ss, item, delim)) {
    *(result++) = item;
  }
}

inline std::vector<std::string> split(const std::string &s, char delim) {
  std::vector<std::string> elems;
  split(s, delim, std::back_inserter(elems));
  return elems;
}

inline bool replace(std::string &str, const std::string &from,
                    const std::string &to) {
  auto start_pos = str.find(from);
  if (start_pos == std::string::npos) return false;
  str.replace(start_pos, from.length(), to);
  return true;
}

inline int replaceAll(std::string &str, const std::string &from,
                      const std::string &to) {
  if (from.empty()) return 0;
  int retval = 0;
  auto start_pos = 0;
  while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
    retval++;
    str.replace(start_pos, from.length(), to);
    start_pos += to.length();  // In case 'to' contains 'from', like replacing
                               // 'x' with 'yx'
  }
  return retval;
}

template <typename T>
inline T stringToProto(const string &s) {
  T t;
  if (!t.ParseFromString(s)) {
    STFATAL << "Error parsing string to proto: " << s.length() << " " << s;
  }
  return t;
}

template <typename T>
inline string protoToString(const T &t) {
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

inline string GetTempDirectory() {
#ifdef WIN32
  WCHAR buf[65536];
  int retval = GetTempPath(65536, buf);
  int a = 0;
  std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
  std::string tmpDir = converter.to_bytes(wstring(buf, retval));
#else
  string tmpDir = _PATH_TMP;
#endif
  return tmpDir;
}

inline void HandleTerminate() {
  static bool first = true;
  if (first) {
    first = false;
  } else {
    // If we are recursively terminating, just bail
    return;
  }
  std::set_terminate([]() -> void {
    std::exception_ptr eptr = std::current_exception();
    if (eptr) {
      try {
        std::rethrow_exception(eptr);
      } catch (const std::exception &e) {
        STFATAL << "Uncaught c++ exception: " << e.what();
      }
    } else {
      STFATAL << "Uncaught c++ exception (unknown)";
    }
  });
}

inline void InterruptSignalHandler(int signum) {
  STERROR << "Got interrupt";
  CLOG(INFO, "stdout") << endl
                       << "Got interrupt (perhaps ctrl+c?).  Exiting." << endl;
  ::exit(signum);
}

inline string GetOsUserName() {
#ifdef WIN32
  char buffer[UNLEN + 1] = {0};
  DWORD buffer_len = UNLEN + 1;
  if (!::GetUserNameA(buffer, &buffer_len)) {
    // error handling
  }

  return string(buffer, buffer_len);
#else
  auto uid = getuid();
  return to_string(uid);
#endif
}
}  // namespace et

inline bool operator==(const google::protobuf::MessageLite &msg_a,
                       const google::protobuf::MessageLite &msg_b) {
  return (msg_a.GetTypeName() == msg_b.GetTypeName()) &&
         (msg_a.SerializeAsString() == msg_b.SerializeAsString());
}

inline bool operator!=(const google::protobuf::MessageLite &msg_a,
                       const google::protobuf::MessageLite &msg_b) {
  return (msg_a.GetTypeName() != msg_b.GetTypeName()) ||
         (msg_a.SerializeAsString() != msg_b.SerializeAsString());
}

#endif
