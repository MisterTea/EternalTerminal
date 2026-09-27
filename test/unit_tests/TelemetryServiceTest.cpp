#include "TelemetryService.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {
class TestTelemetryService : public TelemetryService {
 public:
  TestTelemetryService() : TelemetryService(false, "", "unit-test") {}

  size_t bufferedLogs() {
    lock_guard<recursive_mutex> guard(logMutex);
    return logBuffer.size();
  }

  bool isShuttingDown() {
    lock_guard<recursive_mutex> guard(logMutex);
    return shuttingDown;
  }
};
}  // namespace

TEST_CASE("TelemetryService disabled logging and shutdown",
          "[TelemetryService]") {
  TestTelemetryService telemetry;

  const vector<el::Level> levels = {
      el::Level::Global,  el::Level::Trace, el::Level::Debug,
      el::Level::Fatal,   el::Level::Error, el::Level::Warning,
      el::Level::Verbose, el::Level::Info,  el::Level::Unknown,
  };
  for (auto level : levels) {
    telemetry.logToDatadog("message", level, "file.cpp", 42);
    telemetry.logToSentry(level, "message");
  }
  REQUIRE(telemetry.bufferedLogs() == levels.size());

  telemetry.shutdown();
  REQUIRE(telemetry.isShuttingDown());
  REQUIRE_NOTHROW(telemetry.shutdown());
}

TEST_CASE("TelemetryService bounds its pending log queue",
          "[TelemetryService]") {
  TestTelemetryService telemetry;
  for (size_t i = 0; i < 16 * 1024 + 2; ++i) {
    telemetry.logToDatadog("message", el::Level::Info, "file.cpp", 42);
  }
  REQUIRE(telemetry.bufferedLogs() == 16 * 1024 + 1);
  telemetry.shutdown();
}

#ifndef WIN32
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>

TEST_CASE("TelemetryService crash signal terminates cleanly",
          "[TelemetryService]") {
#if defined(USE_SENTRY) && !defined(NO_TELEMETRY)
  int pipefd[2];
  REQUIRE(pipe(pipefd) == 0);

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    // Child process: isolate stdio
    close(pipefd[0]);
    dup2(pipefd[1], STDERR_FILENO);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);

    // Ensure telemetry is not disabled via environment in test child
    unsetenv("ET_NO_TELEMETRY");

    // Reset any test runner signal handlers
    signal(SIGSEGV, SIG_DFL);
    signal(SIGABRT, SIG_DFL);

    string dbPath = string("/tmp/sentry-test-crash-") + to_string(getpid());
    TelemetryService::create(true, dbPath, "unit-test");

    // Trigger crash signal handled by sentryShutdownHandler
    raise(SIGSEGV);

    _exit(0);
  }

  // Parent process
  close(pipefd[1]);

  // Give child up to 500ms to terminate
  usleep(500000);

  int status = 0;
  pid_t wait_result = waitpid(pid, &status, WNOHANG);

  char buffer[4096];
  fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
  ssize_t bytes_read = read(pipefd[0], buffer, sizeof(buffer) - 1);
  if (bytes_read > 0) {
    buffer[bytes_read] = '\0';
  } else {
    buffer[0] = '\0';
  }
  close(pipefd[0]);

  bool child_exited = (wait_result != 0);

  if (!child_exited) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
  }

  INFO("Child output: " << buffer);
  INFO("wait_result: " << wait_result << ", child_exited: " << child_exited);

  // Child must terminate cleanly rather than entering an infinite loop
  CHECK(child_exited);
  if (child_exited) {
    CHECK(WIFSIGNALED(status));
    CHECK((WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGABRT));
  }
#else
  SUCCEED("Telemetry/Sentry disabled in this build configuration");
#endif
}
#endif
