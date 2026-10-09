#include <algorithm>

#include "ConsoleKeyEventWindows.hpp"
#include "ControlMode.hpp"
#include "HtmClient.hpp"
#include "PlatformUtils.hpp"

namespace et {
namespace {
void writeControlOutput(const char* buf, size_t n) {
  DWORD consoleMode = 0;
  if (!GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &consoleMode)) {
    WriteToStdStream(STDOUT_FILENO, buf, n);
    return;
  }
  // ConPTY strips tmux DCS. Carry control bytes in private CSI sequences
  // (CSI ?777;b0;b1;...q). ConPTY keeps ~16 CSI parameters; the first is
  // 777, so each sequence can hold at most 15 payload bytes.
  constexpr size_t kChunkSize = 15;
  for (size_t offset = 0; offset < n; offset += kChunkSize) {
    const size_t end = std::min(n, offset + kChunkSize);
    string encoded = "\x1b[?777";
    for (size_t i = offset; i < end; ++i) {
      encoded += ";" + to_string(static_cast<unsigned char>(buf[i]));
    }
    encoded += "q";
    WriteToStdStream(STDOUT_FILENO, encoded.data(), encoded.size());
  }
}

void writeDcs() {
  writeControlOutput(kControlModeDcs, strlen(kControlModeDcs));
}
}  // namespace

void HtmClient::run() {
  const int BUF_SIZE = 1024;
  char buf[BUF_SIZE];
  HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
  DWORD consoleMode = 0;
  bool isConsole = GetConsoleMode(stdinHandle, &consoleMode) != 0;
  if (isConsole) {
    DWORD rawMode = consoleMode;
    rawMode &=
        ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
    rawMode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    if (!SetConsoleMode(stdinHandle, rawMode)) {
      throw std::runtime_error("Cannot put stdin in raw console mode");
    }
  }
  // ConPTY synthesizes CTRL_C_EVENT for Ctrl+C / some Ctrl+Shift chords even
  // when the GUI already handled the shortcut (e.g. Windows Terminal new-tab).
  SetConsoleCtrlHandler([](DWORD) -> BOOL { return TRUE; }, TRUE);

  auto inputStarted = std::make_shared<std::atomic_bool>(false);
  auto inputClosed = std::make_shared<std::atomic_bool>(false);
  std::thread{[handler = socketHandler, endpoint = endpointFd, stdinHandle,
               isConsole, inputStarted, inputClosed]() {
    char input[1024];
    inputStarted->store(true);
    while (true) {
      int n = 0;
      if (isConsole) {
        INPUT_RECORD record{};
        DWORD recordsRead = 0;
        if (!ReadConsoleInputW(stdinHandle, &record, 1, &recordsRead)) {
          inputClosed->store(true);
          return;
        }
        if (recordsRead != 1 || record.EventType != KEY_EVENT) {
          continue;
        }
        const KEY_EVENT_RECORD& key = record.Event.KeyEvent;
        if (!consoleKeyEventHasInput({key.bKeyDown != 0, key.wVirtualKeyCode,
                                      key.uChar.UnicodeChar,
                                      key.dwControlKeyState})) {
          continue;
        }
        const wchar_t wide = key.uChar.UnicodeChar;
        n = WideCharToMultiByte(CP_UTF8, 0, &wide, 1, input, sizeof(input),
                                NULL, NULL);
      } else {
        DWORD bytesRead = 0;
        if (!ReadFile(stdinHandle, input, sizeof(input), &bytesRead, NULL) ||
            bytesRead == 0) {
          inputClosed->store(true);
          return;
        }
        n = static_cast<int>(bytesRead);
      }
      try {
        handler->writeAllOrThrow(endpoint, input, n, false);
      } catch (...) {
        inputClosed->store(true);
        return;
      }
    }
  }}.detach();
  while (!inputStarted->load()) {
    std::this_thread::yield();
  }
  writeDcs();

  while (true) {
    bool didWork = false;
    if (inputClosed->load()) {
      throw std::runtime_error("stdin has closed abruptly.");
    }
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(endpointFd, &readSet);
    timeval timeout{0, 0};
    const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
    if (ready == SOCKET_ERROR) {
      throw std::runtime_error("Cannot inspect HTM socket");
    }
    if (ready > 0) {
      int rc = socketHandler->read(endpointFd, buf, BUF_SIZE);
      if (rc < 0) {
        throw std::runtime_error("Cannot read from raw socket");
      }
      if (rc == 0) {
        LOG(INFO) << "htmd has closed";
        endpointFd = -1;
        return;
      }
      writeControlOutput(buf, static_cast<size_t>(rc));
      didWork = true;
    }
    if (!didWork) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}
}  // namespace et
