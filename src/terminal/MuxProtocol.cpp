#include "MuxProtocol.hpp"

#include <sys/stat.h>

#ifndef WIN32
#include <sys/socket.h>
#include <sys/un.h>
#endif

namespace et {
namespace {

uint32_t peekU32(const unsigned char* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void pokeU32(unsigned char* p, uint32_t value) {
  p[0] = static_cast<unsigned char>((value >> 24) & 0xff);
  p[1] = static_cast<unsigned char>((value >> 16) & 0xff);
  p[2] = static_cast<unsigned char>((value >> 8) & 0xff);
  p[3] = static_cast<unsigned char>(value & 0xff);
}

string toLower(string s) {
  for (char& c : s) {
    c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

bool consumeArg(vector<string>* args, size_t* i, string* out) {
  if (*i + 1 >= args->size()) {
    return false;
  }
  *out = (*args)[++(*i)];
  return true;
}

}  // namespace

void MuxBuffer::clear() {
  buf.clear();
  offset = 0;
}

void MuxBuffer::putU32(uint32_t value) {
  unsigned char raw[4];
  pokeU32(raw, value);
  buf.append(reinterpret_cast<char*>(raw), 4);
}

void MuxBuffer::putBool(bool value) { putU32(value ? 1 : 0); }

void MuxBuffer::putString(const string& value) {
  putU32(static_cast<uint32_t>(value.size()));
  buf.append(value);
}

void MuxBuffer::putBytes(const void* data, size_t len) {
  buf.append(reinterpret_cast<const char*>(data), len);
}

bool MuxBuffer::getU32(uint32_t* value) {
  if (remaining() < 4) {
    return false;
  }
  *value = peekU32(reinterpret_cast<const unsigned char*>(buf.data() + offset));
  offset += 4;
  return true;
}

bool MuxBuffer::getBool(bool* value) {
  uint32_t raw = 0;
  if (!getU32(&raw)) {
    return false;
  }
  *value = raw != 0;
  return true;
}

bool MuxBuffer::getString(string* value) {
  uint32_t len = 0;
  if (!getU32(&len)) {
    return false;
  }
  if (remaining() < len) {
    return false;
  }
  value->assign(buf.data() + offset, len);
  offset += len;
  return true;
}

bool muxWriteAll(int fd, const char* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = ::write(fd, data + sent, len - sent);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

int muxPollFd(int fd, short events, int timeoutMs) {
#ifdef WIN32
  WSAPOLLFD pfd{};
  pfd.fd = static_cast<SOCKET>(fd);
  if (events & POLLIN) {
    pfd.events = static_cast<short>(pfd.events | POLLRDNORM);
  }
  if (events & POLLOUT) {
    pfd.events = static_cast<short>(pfd.events | POLLWRNORM);
  }
  return ::WSAPoll(&pfd, 1, timeoutMs);
#else
  pollfd pfd{};
  pfd.fd = fd;
  pfd.events = events;
  return ::poll(&pfd, 1, timeoutMs);
#endif
}

bool muxReadExact(int fd, char* data, size_t len, int timeoutMs) {
  size_t got = 0;
  while (got < len) {
    if (timeoutMs >= 0) {
      int rc = muxPollFd(fd, POLLIN, timeoutMs);
      if (rc == 0) {
        errno = ETIMEDOUT;
        return false;
      }
      if (rc < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
    }
    ssize_t n = ::read(fd, data + got, len - got);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      errno = EPIPE;
      return false;
    }
    got += static_cast<size_t>(n);
  }
  return true;
}

MuxConnection::MuxConnection(int fd, bool takeOwnership)
    : sockFd(fd), ownsFd(takeOwnership) {}

MuxConnection::~MuxConnection() { close(); }

int MuxConnection::release() {
  ownsFd = false;
  int fd = sockFd;
  sockFd = -1;
  return fd;
}

void MuxConnection::close() {
  if (ownsFd && sockFd >= 0) {
    ::close(sockFd);
  }
  sockFd = -1;
  ownsFd = false;
}

bool MuxConnection::writePacket(const MuxBuffer& body) {
  MuxBuffer framed;
  framed.putString(body.data());
  return muxWriteAll(sockFd, framed.data().data(), framed.size());
}

bool MuxConnection::readPacket(MuxBuffer* body, int timeoutMs) {
  char lenBuf[4];
  if (!muxReadExact(sockFd, lenBuf, 4, timeoutMs)) {
    return false;
  }
  uint32_t len = peekU32(reinterpret_cast<const unsigned char*>(lenBuf));
  if (len > 256 * 1024) {
    errno = EMSGSIZE;
    return false;
  }
  string payload(len, '\0');
  if (len > 0 &&
      !muxReadExact(sockFd, &payload[0], len, timeoutMs < 0 ? -1 : timeoutMs)) {
    return false;
  }
  body->assign(std::move(payload));
  return true;
}

bool MuxConnection::sendFd(int fdToSend) {
#ifdef WIN32
  (void)fdToSend;
  errno = ENOTSUP;
  return false;
#else
  char dummy = '\0';
  iovec iov{};
  iov.iov_base = &dummy;
  iov.iov_len = 1;

  char cmsgBuf[CMSG_SPACE(sizeof(int))];
  memset(cmsgBuf, 0, sizeof(cmsgBuf));
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsgBuf;
  msg.msg_controllen = sizeof(cmsgBuf);

  cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cmsg), &fdToSend, sizeof(int));

  while (true) {
    ssize_t n = ::sendmsg(sockFd, &msg, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    return n == 1;
  }
#endif
}

bool MuxConnection::recvFd(int* outFd) {
#ifdef WIN32
  (void)outFd;
  errno = ENOTSUP;
  return false;
#else
  char dummy = '\0';
  iovec iov{};
  iov.iov_base = &dummy;
  iov.iov_len = 1;

  char cmsgBuf[CMSG_SPACE(sizeof(int))];
  memset(cmsgBuf, 0, sizeof(cmsgBuf));
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsgBuf;
  msg.msg_controllen = sizeof(cmsgBuf);

  auto deadline = chrono::steady_clock::now() + chrono::seconds(5);
  while (true) {
    ssize_t n = ::recvmsg(sockFd, &msg, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (chrono::steady_clock::now() >= deadline) {
          return false;
        }
        pollfd pfd{};
        pfd.fd = sockFd;
        pfd.events = POLLIN;
        ::poll(&pfd, 1, 100);
        continue;
      }
      return false;
    }
    if (n == 0) {
      errno = EPIPE;
      return false;
    }
    break;
  }

  for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
       cmsg = CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
        cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
      memcpy(outFd, CMSG_DATA(cmsg), sizeof(int));
      return true;
    }
  }
  errno = EINVAL;
  return false;
#endif
}

ControlMasterMode parseControlMasterValue(const string& value) {
  string v = toLower(value);
  if (v == "yes" || v == "true") {
    return ControlMasterMode::Yes;
  }
  if (v == "auto") {
    return ControlMasterMode::Auto;
  }
  if (v == "no" || v == "false") {
    return ControlMasterMode::No;
  }
  throw runtime_error("Invalid ControlMaster value: " + value);
}

ControlPersistConfig parseControlPersistValue(const string& value) {
  ControlPersistConfig cfg;
  string v = toLower(value);
  if (v == "no" || v == "false") {
    cfg.enabled = false;
    cfg.seconds = 0;
    return cfg;
  }
  if (v == "yes" || v == "true") {
    cfg.enabled = true;
    cfg.seconds = 0;  // forever
    return cfg;
  }
  char* end = nullptr;
  long seconds = strtol(value.c_str(), &end, 10);
  if (end == value.c_str() || *end != '\0' || seconds < 0) {
    throw runtime_error("Invalid ControlPersist value: " + value);
  }
  cfg.enabled = true;
  cfg.seconds = static_cast<int>(seconds);
  return cfg;
}

bool controlPathSocketExists(const string& path) {
  if (path.empty()) {
    return false;
  }
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) {
    return false;
  }
#ifdef S_IFSOCK
  return S_ISSOCK(st.st_mode);
#else
  return true;
#endif
}

string expandControlPath(const string& path) {
  // Token expansion (%h, %C, ...) is out of scope for this branch; accept the
  // path literally so Cursor-style absolute ControlPath values work.
  return path;
}

MuxParseResult parseMuxCliOptions(int argc, char** argv) {
  MuxParseResult result;
  result.remainingArgs.reserve(static_cast<size_t>(argc));
  if (argc > 0) {
    result.remainingArgs.push_back(argv[0]);
  }

  vector<string> args;
  for (int i = 1; i < argc; i++) {
    args.push_back(argv[i]);
  }

  for (size_t i = 0; i < args.size(); i++) {
    const string& arg = args[i];
    if (arg == "-M") {
      result.options.controlMaster = ControlMasterMode::Yes;
      continue;
    }
    if (arg == "-S" || arg.rfind("-S", 0) == 0) {
      string path;
      if (arg == "-S") {
        if (!consumeArg(&args, &i, &path)) {
          throw runtime_error("-S requires a ControlPath argument");
        }
      } else {
        path = arg.substr(2);
      }
      result.options.controlPath = expandControlPath(path);
      continue;
    }
    if (arg == "-O" || arg.rfind("-O", 0) == 0) {
      string cmd;
      if (arg == "-O") {
        if (!consumeArg(&args, &i, &cmd)) {
          throw runtime_error("-O requires a control command");
        }
      } else {
        cmd = arg.substr(2);
      }
      result.options.ctlCommand = toLower(cmd);
      continue;
    }
    if (arg == "-o" || arg.rfind("-o", 0) == 0) {
      string option;
      if (arg == "-o") {
        if (!consumeArg(&args, &i, &option)) {
          throw runtime_error("-o requires an argument");
        }
      } else {
        option = arg.substr(2);
      }
      auto eq = option.find('=');
      string key = eq == string::npos ? option : option.substr(0, eq);
      string value = eq == string::npos ? "yes" : option.substr(eq + 1);
      string keyLower = toLower(key);
      if (keyLower == "controlmaster") {
        result.options.controlMaster = parseControlMasterValue(value);
      } else if (keyLower == "controlpath") {
        result.options.controlPath = expandControlPath(value);
      } else if (keyLower == "controlpersist") {
        result.options.controlPersist = parseControlPersistValue(value);
      } else {
        result.options.passthroughOptions.push_back(option);
      }
      // cxxopts still has to see -o. -G and session options read it from the
      // remaining argv; mux only records the Control* subset above.
      result.remainingArgs.push_back("-o");
      result.remainingArgs.push_back(option);
      continue;
    }
    result.remainingArgs.push_back(arg);
  }

  return result;
}

}  // namespace et
