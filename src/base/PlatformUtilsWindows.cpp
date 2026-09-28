#include <Lmcons.h>

#include <climits>

#include "PlatformUtils.hpp"

namespace et {
optional<string> GetAccountHomeDirectory() {
  const char* userProfile = getenv("USERPROFILE");
  if (userProfile == nullptr || userProfile[0] == '\0') {
    return nullopt;
  }
  return string(userProfile);
}

optional<string> GetAccountUsername() {
  char username[UNLEN + 1];
  DWORD usernameLen = UNLEN + 1;
  if (!GetUserNameA(username, &usernameLen) || usernameLen == 0) {
    return nullopt;
  }
  return string(username);
}

namespace {
void writeConsoleUtf8(HANDLE handle, const char* buf, size_t count) {
  // The console decodes WriteConsoleA with its code page, not UTF-8.
  const int inputLength =
      static_cast<int>(std::min<size_t>(count, static_cast<size_t>(INT_MAX)));
  const int wideLength =
      MultiByteToWideChar(CP_UTF8, 0, buf, inputLength, NULL, 0);
  if (wideLength <= 0) {
    return;
  }
  std::wstring wide(static_cast<size_t>(wideLength), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, buf, inputLength, &wide[0], wideLength);
  size_t offset = 0;
  while (offset < wide.size()) {
    DWORD written = 0;
    const DWORD chunk =
        static_cast<DWORD>(std::min<size_t>(wide.size() - offset, MAXDWORD));
    if (!WriteConsoleW(handle, wide.data() + offset, chunk, &written, NULL) ||
        written == 0) {
      return;
    }
    offset += written;
  }
}
}  // namespace

void WriteToStdStream(int stdFd, const char* buf, size_t count) {
  const HANDLE handle = GetStdHandle(
      stdFd == STDERR_FILENO ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
  DWORD consoleMode = 0;
  if (GetConsoleMode(handle, &consoleMode)) {
    writeConsoleUtf8(handle, buf, count);
    return;
  }
  size_t offset = 0;
  while (offset < count) {
    DWORD written = 0;
    const DWORD chunk =
        static_cast<DWORD>(std::min<size_t>(count - offset, MAXDWORD));
    if (!WriteFile(handle, buf + offset, chunk, &written, NULL) ||
        written == 0) {
      return;
    }
    offset += written;
  }
}
}  // namespace et
