#include "Console.hpp"
#include "PlatformUtils.hpp"
#include "StdioConsole.hpp"

namespace et {
namespace {
constexpr DWORD kConsoleReadSize = 16 * 1024;

ConsoleInputStatus readConsoleKeys(HANDLE handle, string* out) {
  DWORD pending = 0;
  if (!GetNumberOfConsoleInputEvents(handle, &pending) || pending == 0) {
    return ConsoleInputStatus::NONE;
  }
  INPUT_RECORD records[128];
  DWORD count = 0;
  if (!ReadConsoleInputW(handle, records, 128, &count)) {
    return ConsoleInputStatus::NONE;
  }
  // ENABLE_VIRTUAL_TERMINAL_INPUT delivers special keys as VT sequences, so
  // key-down characters are the whole input stream.
  std::wstring wide;
  for (DWORD i = 0; i < count; i++) {
    const INPUT_RECORD& record = records[i];
    if (record.EventType != KEY_EVENT ||
        !ConsoleKeyEventHasInput(record.Event.KeyEvent)) {
      continue;
    }
    const WORD repeat = std::max<WORD>(1, record.Event.KeyEvent.wRepeatCount);
    wide.append(repeat, record.Event.KeyEvent.uChar.UnicodeChar);
  }
  if (wide.empty()) {
    return ConsoleInputStatus::NONE;
  }
  const int wideLength = static_cast<int>(wide.size());
  const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                             wideLength, NULL, 0, NULL, NULL);
  if (utf8Length <= 0) {
    return ConsoleInputStatus::NONE;
  }
  const size_t start = out->size();
  out->resize(start + static_cast<size_t>(utf8Length));
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, &(*out)[start],
                      utf8Length, NULL, NULL);
  return ConsoleInputStatus::DATA;
}

ConsoleInputStatus readHandle(HANDLE handle, DWORD maxBytes, string* out) {
  char b[kConsoleReadSize];
  DWORD bytesRead = 0;
  if (!ReadFile(handle, b, std::min(maxBytes, kConsoleReadSize), &bytesRead,
                NULL)) {
    const DWORD error = GetLastError();
    if (error != ERROR_BROKEN_PIPE && error != ERROR_HANDLE_EOF) {
      LOG(INFO) << "Console stdin read error (" << error
                << "), disabling console input";
      return ConsoleInputStatus::CLOSED;
    }
    bytesRead = 0;
  }
  if (bytesRead == 0) {
    LOG(INFO) << "Console stdin is at EOF, disabling console input";
    return ConsoleInputStatus::CLOSED;
  }
  out->append(b, bytesRead);
  return ConsoleInputStatus::DATA;
}
}  // namespace

void Console::write(const string& s) {
  WriteToStdStream(STDOUT_FILENO, s.data(), s.size());
}

size_t Console::writeSome(const string& s) {
  if (s.empty()) {
    return 0;
  }
  write(s);
  return s.size();
}

ConsoleInputStatus Console::readInput(const set<int>& /*readyFds*/,
                                      string* out) {
  // The default console fd is a socket (e.g. FakeConsole in tests).
  char b[kConsoleReadSize];
  const int rc = ::recv(getFd(), b, sizeof(b), 0);
  const int savedErrno = GetErrno();
  if (rc > 0) {
    out->append(b, static_cast<size_t>(rc));
    return ConsoleInputStatus::DATA;
  }
  if (rc == 0) {
    LOG(INFO) << "Console is at EOF, disabling console input";
    return ConsoleInputStatus::CLOSED;
  }
  if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
    return ConsoleInputStatus::NONE;
  }
  LOG(INFO) << "Console read error (" << savedErrno
            << "): " << strerror(savedErrno) << ", disabling console input";
  return ConsoleInputStatus::CLOSED;
}

void StdioConsole::setup() {}

vector<int> StdioConsole::getInputPollFds() { return {}; }

int StdioConsole::getOutputPollFd() { return -1; }

ConsoleInputStatus StdioConsole::readInput(const set<int>& /*readyFds*/,
                                           string* out) {
  const HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
  if (handle == NULL || handle == INVALID_HANDLE_VALUE) {
    LOG(INFO) << "Console has no stdin, disabling console input";
    return ConsoleInputStatus::CLOSED;
  }
  DWORD consoleMode = 0;
  if (GetConsoleMode(handle, &consoleMode)) {
    return readConsoleKeys(handle, out);
  }
  switch (GetFileType(handle)) {
    case FILE_TYPE_PIPE: {
      DWORD available = 0;
      if (!PeekNamedPipe(handle, NULL, 0, NULL, &available, NULL)) {
        LOG(INFO) << "Console stdin pipe closed, disabling console input";
        return ConsoleInputStatus::CLOSED;
      }
      if (available == 0) {
        return ConsoleInputStatus::NONE;
      }
      return readHandle(handle, available, out);
    }
    case FILE_TYPE_DISK:
      return readHandle(handle, kConsoleReadSize, out);
    default:
      // e.g. NUL under a service or scheduled task: no keyboard, but the
      // session must survive.
      LOG(INFO) << "Console stdin is not readable, disabling console input";
      return ConsoleInputStatus::CLOSED;
  }
}
}  // namespace et
