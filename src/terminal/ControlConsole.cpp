#include "ControlConsole.hpp"

#include "RawSocketUtils.hpp"

namespace et {

namespace {
const int kDefaultRows = 24;
// 132 columns is the standard "wide" terminal (the VT100/VT220 132-column mode);
// it is a better default than 80 for a session whose viewer hasn't sized it yet,
// since attach/observe only learn the real width once someone connects.
const int kDefaultCols = 132;

int64_t nowSeconds() { return (int64_t)time(NULL); }
}  // namespace

ControlConsole::ControlConsole(size_t scrollbackCapBytes)
    : scrollback(scrollbackCapBytes),
      lastActivityTime(nowSeconds()),
      createdTime(nowSeconds()) {
  inputPipe[0] = inputPipe[1] = -1;
#ifndef WIN32
  FATAL_FAIL(::pipe(inputPipe));
  /*
   * Non-blocking read end: run() probes it via select(), and a spurious wakeup
   * must never block the loop.
   */
  int flags = fcntl(inputPipe[0], F_GETFL, 0);
  fcntl(inputPipe[0], F_SETFL, flags | O_NONBLOCK);
#endif
  size.set_row(kDefaultRows);
  size.set_column(kDefaultCols);
  size.set_width(0);
  size.set_height(0);
}

ControlConsole::~ControlConsole() {
#ifndef WIN32
  if (inputPipe[0] >= 0) {
    ::close(inputPipe[0]);
  }
  if (inputPipe[1] >= 0) {
    ::close(inputPipe[1]);
  }
#endif
}

TerminalInfo ControlConsole::getTerminalInfo() {
  lock_guard<std::mutex> guard(sizeMutex);
  return size;
}

int ControlConsole::getFd() { return inputPipe[0]; }

void ControlConsole::write(const string& s) {
  // run() calls this with each TERMINAL_BUFFER of server output.
  scrollback.append(s);
  transcript.append('<', s);
  touchActivity();
}

void ControlConsole::injectInput(const string& bytes, bool secret) {
#ifndef WIN32
  if (inputPipe[1] < 0 || bytes.empty()) {
    return;
  }
  RawSocketUtils::writeAll(inputPipe[1], &bytes[0], bytes.length());
  transcript.append('>', secret ? string("<secret>") : bytes);
  touchActivity();
#endif
}

ScrollbackRead ControlConsole::readOutput(int64_t cursor) const {
  return scrollback.read(cursor);
}

TranscriptRead ControlConsole::readTranscript(int64_t cursor) const {
  return transcript.read(cursor);
}

void ControlConsole::setSize(int row, int column, int width, int height) {
  lock_guard<std::mutex> guard(sizeMutex);
  size.set_row(row);
  size.set_column(column);
  size.set_width(width);
  size.set_height(height);
}

int64_t ControlConsole::lastActivity() const {
  lock_guard<std::mutex> guard(activityMutex);
  return lastActivityTime;
}

void ControlConsole::touchActivity() {
  lock_guard<std::mutex> guard(activityMutex);
  lastActivityTime = nowSeconds();
}

}  // namespace et
