#include <Lmcons.h>

#include <climits>

#include "ConsoleUtf8DecoderWindows.hpp"
#include "PlatformUtils.hpp"

namespace et {
string GetTempDirectory() {
  WCHAR tempPath[65536];
  const DWORD length = GetTempPathW(65536, tempPath);
  if (length == 0 || length >= 65536) {
    throw std::runtime_error("Cannot find Windows temporary directory");
  }
  std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
  return converter.to_bytes(std::wstring(tempPath, length));
}

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
constexpr size_t kConsoleWriteChunk = 64 * 1024;

size_t incompleteUtf8Suffix(const string& bytes) {
  if (bytes.empty()) {
    return 0;
  }
  size_t start = bytes.size() - 1;
  auto continuation = [](unsigned char c) { return (c & 0xc0) == 0x80; };
  while (start > 0 && bytes.size() - start < 4 &&
         continuation(static_cast<unsigned char>(bytes[start]))) {
    --start;
  }
  const auto first = static_cast<unsigned char>(bytes[start]);
  const size_t expected = first >= 0xc2 && first <= 0xdf   ? 2
                          : first >= 0xe0 && first <= 0xef ? 3
                          : first >= 0xf0 && first <= 0xf4 ? 4
                                                           : 0;
  const size_t available = bytes.size() - start;
  if (expected == 0 || available >= expected) {
    return 0;
  }
  if (available > 1) {
    const auto second = static_cast<unsigned char>(bytes[start + 1]);
    // Defer only prefixes that can still form a valid scalar value.
    if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second > 0x9f) ||
        (first == 0xf0 && second < 0x90) || (first == 0xf4 && second > 0x8f)) {
      return 0;
    }
  }
  return available;
}

struct ConsoleStreamState {
  std::mutex mutex;
  HANDLE handle = INVALID_HANDLE_VALUE;
  ConsoleUtf8Decoder decoder;
};

void writeConsoleUtf8(HANDLE handle, const char* buf, size_t count,
                      ConsoleUtf8Decoder* decoder) {
  // The console decodes WriteConsoleA with its code page, not UTF-8.
  size_t inputOffset = 0;
  while (inputOffset < count) {
    const size_t inputChunk = std::min(count - inputOffset, kConsoleWriteChunk);
    const std::wstring wide = decoder->decode(buf + inputOffset, inputChunk);
    inputOffset += inputChunk;
    size_t offset = 0;
    while (offset < wide.size()) {
      DWORD written = 0;
      if (!WriteConsoleW(handle, wide.data() + offset,
                         static_cast<DWORD>(wide.size() - offset), &written,
                         NULL) ||
          written == 0) {
        return;
      }
      offset += written;
    }
  }
}
}  // namespace

std::wstring ConsoleUtf8Decoder::decode(const char* buf, size_t count) {
  std::wstring result;
  size_t offset = 0;
  while (offset < count) {
    const size_t chunk = std::min(count - offset, kConsoleWriteChunk);
    pending.append(buf + offset, chunk);
    offset += chunk;
    const size_t complete = pending.size() - incompleteUtf8Suffix(pending);
    if (complete > 0) {
      const int inputLength = static_cast<int>(complete);
      const int wideLength =
          MultiByteToWideChar(CP_UTF8, 0, pending.data(), inputLength, NULL, 0);
      if (wideLength > 0) {
        const size_t start = result.size();
        result.resize(start + static_cast<size_t>(wideLength));
        MultiByteToWideChar(CP_UTF8, 0, pending.data(), inputLength,
                            &result[start], wideLength);
      }
      pending.erase(0, complete);
    }
  }
  return result;
}

void WriteToStdStream(int stdFd, const char* buf, size_t count) {
  static ConsoleStreamState stdoutState;
  static ConsoleStreamState stderrState;
  ConsoleStreamState& state =
      stdFd == STDERR_FILENO ? stderrState : stdoutState;
  lock_guard<std::mutex> guard(state.mutex);
  const HANDLE handle = GetStdHandle(
      stdFd == STDERR_FILENO ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
  if (handle != state.handle) {
    state.decoder.reset();
    state.handle = handle;
  }
  DWORD consoleMode = 0;
  if (GetConsoleMode(handle, &consoleMode)) {
    writeConsoleUtf8(handle, buf, count, &state.decoder);
    return;
  }
  state.decoder.reset();
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

bool ConsoleKeyEventHasInput(const KEY_EVENT_RECORD& key) {
  if (!key.bKeyDown) {
    return false;
  }
  if (key.uChar.UnicodeChar != 0) {
    return true;
  }
  if ((key.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) == 0) {
    return false;
  }
  // VkKeyScanW: low byte is the virtual key that produces the character, high
  // byte the modifiers it needs (1 Shift, 2 Ctrl, 4 Alt), -1 if no key
  // produces it. Ctrl is the chord modifier, so only Shift and Alt have to
  // agree.
  const bool shiftHeld = (key.dwControlKeyState & SHIFT_PRESSED) != 0;
  const bool altHeld =
      (key.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
  for (const WCHAR c : {L'@', L' ', L'2', L'`'}) {
    const SHORT scan = VkKeyScanW(c);
    if (scan != -1 && LOBYTE(scan) == key.wVirtualKeyCode &&
        ((HIBYTE(scan) & 1) != 0) == shiftHeld &&
        ((HIBYTE(scan) & 4) != 0) == altHeld) {
      return true;
    }
  }
  return false;
}
}  // namespace et
