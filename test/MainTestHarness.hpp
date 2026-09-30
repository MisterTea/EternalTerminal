#ifndef __ET_MAIN_TEST_HARNESS__
#define __ET_MAIN_TEST_HARNESS__

#include <iostream>
#include <sstream>

#include "LogHandler.hpp"
#include "RawSocketUtils.hpp"
#include "TelemetryService.hpp"
#include "TestHeaders.hpp"

#ifndef WIN32
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace et {
namespace test {
struct MainResult {
  int exitCode = -1;
  string output;
};

/**
 * Runs an executable's main function in-process with the given argv (argv[0]
 * included). Captures everything written to std::cout / std::cerr, and
 * restores the logger, verbosity, and telemetry state that the main function
 * replaces so later assertions in the same test still log normally.
 *
 * Paths that call LogHandler::setupLogFiles must pass a temporary log
 * directory, and etserver / et must pass --logtostdout so stderr is not
 * redirected into a log file.
 */
template <class MainFunction>
MainResult runMain(const vector<string>& args, MainFunction&& mainFunction) {
  vector<string> storage = args;
  vector<char*> argv;
  for (auto& arg : storage) {
    argv.push_back(&arg[0]);
  }
  argv.push_back(nullptr);

  struct Restore {
    el::Configurations defaultConf =
        *el::Loggers::getLogger("default")->configurations();
    el::Configurations stdoutConf =
        *el::Loggers::getLogger("stdout")->configurations();
    el::base::type::VerboseLevel verboseLevel = el::Loggers::verboseLevel();
    std::streambuf* cout = std::cout.rdbuf();
    std::streambuf* cerr = std::cerr.rdbuf();

    ~Restore() {
      el::Loggers::flushAll();
      std::cout.rdbuf(cout);
      std::cerr.rdbuf(cerr);
      el::Loggers::reconfigureLogger("default", defaultConf);
      el::Loggers::reconfigureLogger("stdout", stdoutConf);
      el::Loggers::setVerboseLevel(verboseLevel);
      el::Helpers::uninstallPreRollOutCallback();
      if (!TelemetryService::exists()) {
        TelemetryService::create(false, "", "");
      }
    }
  };

  std::ostringstream captured;
  MainResult result;
  {
    Restore restore;
    std::cout.rdbuf(captured.rdbuf());
    std::cerr.rdbuf(captured.rdbuf());
    result.exitCode =
        mainFunction(static_cast<int>(argv.size() - 1), argv.data());
  }
  result.output = captured.str();
  return result;
}

inline bool contains(const string& haystack, const string& needle) {
  return haystack.find(needle) != string::npos;
}

#ifndef WIN32
struct ChildMainResult {
  // False when the main function ended the process itself (exit, abort, or a
  // signal) instead of returning its status.
  bool returned = false;
  bool timedOut = false;
  int exitCode = -1;
  int signal = 0;
  string output;
};

/**
 * Runs an executable's main function in a forked child so a main that calls
 * exit(), aborts, or hangs cannot take the test process down with it. The
 * child's stdout and stderr are captured; a hung child is killed after
 * `timeoutSeconds`.
 */
template <class MainFunction>
ChildMainResult runMainInChild(const vector<string>& args,
                               MainFunction&& mainFunction,
                               int timeoutSeconds = 60) {
  int outputPipe[2];
  int statusPipe[2];
  FATAL_FAIL(::pipe(outputPipe));
  FATAL_FAIL(::pipe(statusPipe));
  std::cout.flush();
  std::cerr.flush();
  ::fflush(nullptr);

  pid_t pid = ::fork();
  FATAL_FAIL(pid);
  if (pid == 0) {
    ::close(outputPipe[0]);
    ::close(statusPipe[0]);
    ::dup2(outputPipe[1], STDOUT_FILENO);
    ::dup2(outputPipe[1], STDERR_FILENO);
    ::close(outputPipe[1]);
    vector<string> storage = args;
    vector<char*> argv;
    for (auto& arg : storage) {
      argv.push_back(&arg[0]);
    }
    argv.push_back(nullptr);
    int code = mainFunction(static_cast<int>(argv.size() - 1), argv.data());
    el::Loggers::flushAll();
    std::cout.flush();
    std::cerr.flush();
    ::fflush(nullptr);
    RawSocketUtils::writeAll(statusPipe[1], reinterpret_cast<char*>(&code),
                             sizeof(code));
    ::_exit(0);
  }

  ::close(outputPipe[1]);
  ::close(statusPipe[1]);
  ChildMainResult result;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
  char buffer[4096];
  while (true) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      result.timedOut = true;
      ::kill(pid, SIGKILL);
      break;
    }
    pollfd pfd{outputPipe[0], POLLIN, 0};
    int ready = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
    if (ready <= 0) {
      continue;
    }
    ssize_t n = ::read(outputPipe[0], buffer, sizeof(buffer));
    if (n <= 0) {
      break;
    }
    result.output.append(buffer, n);
  }
  ::close(outputPipe[0]);

  int status = 0;
  ::waitpid(pid, &status, 0);
  int code = 0;
  if (!result.timedOut &&
      ::read(statusPipe[0], &code, sizeof(code)) == sizeof(code)) {
    result.returned = true;
    result.exitCode = code;
  } else if (WIFEXITED(status)) {
    result.exitCode = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.signal = WTERMSIG(status);
  }
  ::close(statusPipe[0]);
  return result;
}

/**
 * Runs a background service in a forked child for the lifetime of this
 * object. `serve` must start the service and return; the child then idles
 * until the destructor kills it.
 *
 * Keeping a server's threads out of the test process matters because the main
 * functions reconfigure easylogging and swap std::cout while they run, which
 * races with any other thread that logs.
 */
class BackgroundChild {
 public:
  template <class Serve>
  explicit BackgroundChild(Serve&& serve) {
    int readyPipe[2];
    FATAL_FAIL(::pipe(readyPipe));
    std::cout.flush();
    std::cerr.flush();
    ::fflush(nullptr);
    pid = ::fork();
    FATAL_FAIL(pid);
    if (pid == 0) {
      ::close(readyPipe[0]);
      serve();
      char ready = 1;
      RawSocketUtils::writeAll(readyPipe[1], &ready, 1);
      while (true) {
        ::pause();
      }
    }
    ::close(readyPipe[1]);
    char ready = 0;
    started = ::read(readyPipe[0], &ready, 1) == 1 && ready == 1;
    ::close(readyPipe[0]);
  }

  ~BackgroundChild() {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
  }

  BackgroundChild(const BackgroundChild&) = delete;
  BackgroundChild& operator=(const BackgroundChild&) = delete;

  bool started = false;

 private:
  pid_t pid = -1;
};
#endif

/**
 * Loopback TCP listener on an ephemeral port. Its thread accepts every
 * connection and either closes it at once or holds it open without speaking,
 * which makes the ET handshake fail or stall.
 */
class LoopbackListener {
 public:
  enum class Mode { CloseImmediately, HoldOpen };

  explicit LoopbackListener(Mode mode = Mode::CloseImmediately) : mode(mode) {
    int listenFd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    FATAL_FAIL(listenFd);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    FATAL_FAIL(::bind(listenFd, reinterpret_cast<sockaddr*>(&address),
                      sizeof(address)));
    FATAL_FAIL(::listen(listenFd, 16));
    socklen_t length = sizeof(address);
    FATAL_FAIL(::getsockname(listenFd, reinterpret_cast<sockaddr*>(&address),
                             &length));
    listenPort = ntohs(address.sin_port);
    fd = listenFd;
    acceptThread = std::thread([this]() { acceptLoop(); });
  }

  ~LoopbackListener() {
    stop = true;
    acceptThread.join();
    for (int held : heldFds) {
      RawSocketUtils::closeSocket(held);
    }
    RawSocketUtils::closeSocket(fd);
  }

  int port() const { return listenPort; }
  int acceptCount() const { return accepts.load(); }

 private:
  void acceptLoop() {
    while (!stop) {
      fd_set readFds;
      FD_ZERO(&readFds);
      FD_SET(fd, &readFds);
      timeval timeout{0, 20 * 1000};
      if (::select(fd + 1, &readFds, nullptr, nullptr, &timeout) <= 0) {
        continue;
      }
      int accepted = static_cast<int>(::accept(fd, nullptr, nullptr));
      if (accepted < 0) {
        continue;
      }
      accepts++;
      if (mode == Mode::HoldOpen) {
        heldFds.push_back(accepted);
      } else {
        RawSocketUtils::closeSocket(accepted);
      }
    }
  }

  Mode mode;
  int fd = -1;
  int listenPort = 0;
  std::atomic<bool> stop{false};
  std::atomic<int> accepts{0};
  vector<int> heldFds;
  std::thread acceptThread;
};

// A loopback port with nothing listening on it (racy, but only by the time
// between close and the caller's own bind or connect).
inline int unusedLoopbackPort() {
  int probe = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
  FATAL_FAIL(probe);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  FATAL_FAIL(
      ::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)));
  socklen_t length = sizeof(address);
  FATAL_FAIL(
      ::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length));
  RawSocketUtils::closeSocket(probe);
  return ntohs(address.sin_port);
}
}  // namespace test
}  // namespace et

#endif  // __ET_MAIN_TEST_HARNESS__
