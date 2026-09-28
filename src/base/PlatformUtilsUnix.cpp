#include "PlatformUtils.hpp"
#include "RawSocketUtils.hpp"

namespace et {
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
}  // namespace et
