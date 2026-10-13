#include "PlatformUtils.hpp"
#include "RawSocketUtils.hpp"

namespace et {
string GetTempDirectory() { return _PATH_TMP; }

namespace {
template <typename Field>
optional<string> lookupPasswdField(Field field) {
  long bufSize = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (bufSize <= 0) {
    bufSize = 16 * 1024;
  }
  vector<char> buf(static_cast<size_t>(bufSize));
  struct passwd pwd;
  struct passwd* result = nullptr;
  if (getpwuid_r(getuid(), &pwd, buf.data(), buf.size(), &result) != 0 ||
      result == nullptr) {
    return nullopt;
  }
  const char* value = field(*result);
  if (value == nullptr || value[0] == '\0') {
    return nullopt;
  }
  return string(value);
}
}  // namespace

optional<string> GetAccountHomeDirectory() {
  return lookupPasswdField([](const passwd& p) { return p.pw_dir; });
}

optional<string> GetAccountUsername() {
  return lookupPasswdField([](const passwd& p) { return p.pw_name; });
}

void WriteToStdStream(int stdFd, const char* buf, size_t count) {
  RawSocketUtils::writeAll(stdFd, buf, count);
}

bool GetPeerCredentials(int fd, uid_t* uid, gid_t* gid) {
#ifdef __linux__
  struct ucred cred;
  socklen_t len = sizeof(cred);
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
    return false;
  }
  *uid = cred.uid;
  *gid = cred.gid;
  return true;
#else
  return ::getpeereid(fd, uid, gid) == 0;
#endif
}

int CreatePrivateFile(const string& path) {
  const int flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC;
  int fd = ::open(path.c_str(), flags, 0600);
  struct stat st;
  if (fd < 0 && errno == EEXIST && ::lstat(path.c_str(), &st) == 0 &&
      S_ISREG(st.st_mode) && st.st_uid == ::geteuid() &&
      ::unlink(path.c_str()) == 0) {
    fd = ::open(path.c_str(), flags, 0600);
  }
  return fd;
}
}  // namespace et
