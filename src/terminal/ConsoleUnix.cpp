#include "Console.hpp"
#include "RawSocketUtils.hpp"
#include "StdioConsole.hpp"

namespace et {
namespace {
constexpr size_t kConsoleReadSize = 16 * 1024;

/**
 * @brief Reads @p readFd, using whether @p ttyFd is a tty to tell a user
 * closing the terminal (FAILED) from a launcher leaving an unreadable
 * descriptor behind (CLOSED).
 */
ConsoleInputStatus readConsoleFd(int readFd, int ttyFd, string* out) {
  char b[kConsoleReadSize];
  const ssize_t rc = ::read(readFd, b, sizeof(b));
  const int savedErrno = errno;
  if (rc > 0) {
    out->append(b, static_cast<size_t>(rc));
    return ConsoleInputStatus::DATA;
  }
  if (rc == 0) {
    if (isatty(ttyFd)) {
      LOG(INFO) << "Console EOF";
      return ConsoleInputStatus::FAILED;
    }
    LOG(INFO) << "Console is not a tty and is at EOF, disabling console input";
    return ConsoleInputStatus::CLOSED;
  }
  if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK ||
      savedErrno == EINTR) {
    return ConsoleInputStatus::NONE;
  }
  if (!isatty(ttyFd)) {
    LOG(INFO) << "Console is not a tty and cannot be read (" << savedErrno
              << "): " << strerror(savedErrno) << ", disabling console input";
    return ConsoleInputStatus::CLOSED;
  }
  LOG(INFO) << "Console read error: (" << savedErrno
            << "): " << strerror(savedErrno);
  return ConsoleInputStatus::FAILED;
}
}  // namespace

void Console::write(const string& s) {
  RawSocketUtils::writeAll(getFd(), s.data(), s.length());
}

size_t Console::writeSome(const string& s) {
  if (s.empty()) {
    return 0;
  }
  const ssize_t rc = ::write(getFd(), s.data(), s.size());
  if (rc < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return 0;
    }
    throw std::runtime_error(string("console write failed: ") +
                             strerror(errno));
  }
  return static_cast<size_t>(rc);
}

ConsoleInputStatus Console::readInput(const set<int>& /*readyFds*/,
                                      string* out) {
  return readConsoleFd(getFd(), getFd(), out);
}

void StdioConsole::setup() {
  setSocketBlocking(STDIN_FILENO, false);
  setSocketBlocking(STDOUT_FILENO, false);
}

vector<int> StdioConsole::getInputPollFds() {
  // Keystrokes arrive on stdin. stdout is polled too because launchers such
  // as nohup(1) replace it with a regular file while leaving the tty on stdin;
  // reading that descriptor fails and disables input instead of spinning.
  return {STDOUT_FILENO, STDIN_FILENO};
}

int StdioConsole::getOutputPollFd() { return STDOUT_FILENO; }

ConsoleInputStatus StdioConsole::readInput(const set<int>& readyFds,
                                           string* out) {
  const int readFd =
      readyFds.count(STDIN_FILENO) != 0 ? STDIN_FILENO : STDOUT_FILENO;
  return readConsoleFd(readFd, STDOUT_FILENO, out);
}
}  // namespace et
