#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "LogHandler.hpp"
#include "PipeSocketHandler.hpp"
#include "TelemetryService.hpp"
#include "TerminalClient.hpp"
#include "TestHeaders.hpp"

#ifndef WIN32
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace et;

namespace {

#ifndef WIN32
const string kKey = "12345678901234567890123456789012";
const string kClientId = "1234567890123456";

struct ReproResult {
  bool timedOut = false;
  int status = -1;
  string output;
};

int lastAcceptCount(const string& output) {
  int count = -1;
  const string marker = "ACCEPTS=";
  size_t pos = 0;
  while ((pos = output.find(marker, pos)) != string::npos) {
    pos += marker.size();
    count = atoi(output.c_str() + pos);
  }
  return count;
}

// Runs et-test again as a clean process. TerminalClient calls exit(1) when the
// initial connection gives up, which would take down the Catch runner.
ReproResult runIssue866Repro(const string& mode, const string& directory,
                             int timeoutSec) {
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    STFATAL << "pipe failed: " << strerror(errno);
  }

  setenv("ET_REPRO_866", mode.c_str(), 1);
  setenv("ET_REPRO_866_DIR", directory.c_str(), 1);
  pid_t pid = fork();
  if (pid < 0) {
    unsetenv("ET_REPRO_866");
    unsetenv("ET_REPRO_866_DIR");
    close(pipefd[0]);
    close(pipefd[1]);
    STFATAL << "fork failed: " << strerror(errno);
  }
  if (pid == 0) {
    setpgid(0, 0);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[0]);
    close(pipefd[1]);
    char exe[] = "/proc/self/exe";
    char* argv[] = {exe, nullptr};
    execv(exe, argv);
    _exit(127);
  }
  unsetenv("ET_REPRO_866");
  unsetenv("ET_REPRO_866_DIR");
  setpgid(pid, pid);
  close(pipefd[1]);

  ReproResult result;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
  while (true) {
    pollfd pfd;
    pfd.fd = pipefd[0];
    pfd.events = POLLIN;
    int remainMs =
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now())
                             .count());
    if (remainMs < 0) {
      remainMs = 0;
    }
    int polled = poll(&pfd, 1, remainMs);
    if (polled > 0) {
      char buffer[1024];
      ssize_t n = read(pipefd[0], buffer, sizeof(buffer));
      if (n > 0) {
        result.output.append(buffer, static_cast<size_t>(n));
        continue;
      }
    }

    int status = 0;
    pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      result.status = status;
      // Drain anything written just before exit.
      while (true) {
        char buffer[1024];
        ssize_t n = read(pipefd[0], buffer, sizeof(buffer));
        if (n > 0) {
          result.output.append(buffer, static_cast<size_t>(n));
          continue;
        }
        break;
      }
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      result.timedOut = true;
      kill(-pid, SIGKILL);
      waitpid(pid, &status, 0);
      result.status = status;
      break;
    }
  }
  close(pipefd[0]);
  return result;
}

void expectInitialConnectGivesUp(const ReproResult& result, bool checkAccepts) {
  INFO("timedOut=" << result.timedOut << " status=" << result.status
                   << " output:\n"
                   << result.output);
  REQUIRE_FALSE(result.timedOut);
  REQUIRE(WIFEXITED(result.status));
  REQUIRE(WEXITSTATUS(result.status) == 1);
  REQUIRE(result.output.find("Could not make initial connection") !=
          string::npos);
  REQUIRE(result.output.find("CONSTRUCTOR_RETURNED") == string::npos);
  if (checkAccepts) {
    // Giving up on a live socket must not call connect() again. A second
    // connect() is the RETURNING_CLIENT path in issue 862.
    REQUIRE(lastAcceptCount(result.output) == 1);
  }
}
#endif

#ifndef WIN32
void configureReproLogging(int* argc, char*** argv) {
  el::Configurations defaultConf = LogHandler::setupLogHandler(argc, argv);
  LogHandler::setupStdoutLogger();
  defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "true");
  defaultConf.setGlobally(el::ConfigurationType::ToFile, "false");
  el::Loggers::reconfigureLogger("default", defaultConf);
}
#endif

#ifndef WIN32
// Handshake only. The terminal INITIAL_RESPONSE is intentionally never sent.
// Accepted sockets stay open so a later connect() is also counted.
void serveSilentHandshake(shared_ptr<SocketHandler> serverHandler, int serverFd,
                          std::atomic<bool>* stop) {
  el::Helpers::setThreadName("silent-server");
  vector<int> held;
  int accepts = 0;
  while (!stop->load()) {
    if (serverHandler->hasData(serverFd)) {
      int fd = serverHandler->accept(serverFd);
      if (fd >= 0) {
        ++accepts;
        fprintf(stdout, "ACCEPTS=%d\n", accepts);
        fflush(stdout);
        try {
          serverHandler->readProto<ConnectRequest>(fd, true);
          ConnectResponse response;
          response.set_status(NEW_CLIENT);
          serverHandler->writeProto(fd, response, true);
          held.push_back(fd);
        } catch (const std::runtime_error& err) {
          LOG(INFO) << "Silent handshake failed: " << err.what();
          serverHandler->close(fd);
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  for (int fd : held) {
    serverHandler->close(fd);
  }
}
#endif

}  // namespace

#ifndef WIN32
// Spawned via ET_REPRO_866 so exit(1) inside TerminalClient is the process
// status. See the Catch cases below.
int RunIssue866Repro(int argc, char** argv, const char* mode) {
  configureReproLogging(&argc, &argv);
  ::signal(SIGPIPE, SIG_IGN);
  TelemetryService::create(false, "", "repro-866");

  const char* directory = getenv("ET_REPRO_866_DIR");
  if (directory == nullptr || directory[0] == '\0') {
    fprintf(stderr, "ET_REPRO_866_DIR is not set\n");
    return 2;
  }
  string pipePath = string(directory) + "/pipe";
  SocketEndpoint endpoint;
  endpoint.set_name(pipePath);

  auto clientHandler = shared_ptr<SocketHandler>(new PipeSocketHandler());
  auto pipeHandler = shared_ptr<SocketHandler>(new PipeSocketHandler());
  shared_ptr<SocketHandler> serverHandler;
  std::thread serverThread;
  std::atomic<bool> stopServer{false};
  if (string(mode) == "silent") {
    serverHandler.reset(new PipeSocketHandler());
    serverHandler->listen(endpoint);
    int serverFd = *(serverHandler->getEndpointFds(endpoint).begin());
    serverThread =
        std::thread(serveSilentHandshake, serverHandler, serverFd, &stopServer);
  } else if (string(mode) != "missing") {
    fprintf(stderr, "Unknown ET_REPRO_866 mode: %s\n", mode);
    return 2;
  }

  // Issue 866: a failed initial connect must exit. Today the constructor
  // counts the failure, then falls through to the unconditional break and
  // returns as if the session existed. run() would then spin in writePacket.
  TerminalClient client(clientHandler, pipeHandler, endpoint, kClientId, kKey,
                        nullptr, false, "", "", false, "", 5, {});

  fputs("CONSTRUCTOR_RETURNED\n", stdout);
  fflush(stdout);
  stopServer.store(true);
  if (serverThread.joinable()) {
    serverThread.join();
  }
  _exit(0);
}
#endif

#ifdef WIN32
TEST_CASE("TerminalClient exits when the initial connection fails",
          "[TerminalClient][issue866]") {
  SKIP("Process-spawn repro uses fork/exec");
}

TEST_CASE("TerminalClient exits when the initial response never arrives",
          "[TerminalClient][issue866]") {
  SKIP("Process-spawn repro uses fork/exec");
}
#else
// https://github.com/MisterTea/EternalTerminal/issues/866
TEST_CASE("TerminalClient exits when the initial connection fails",
          "[TerminalClient][issue866]") {
  string directory = et::test::makeTempDir("et_866_missing");
  ReproResult result = runIssue866Repro("missing", directory, 5);
  fs::remove_all(directory);
  expectInitialConnectGivesUp(result, false);
}

// https://github.com/MisterTea/EternalTerminal/issues/866
// connect() succeeds, but INITIAL_RESPONSE never arrives. The constructor
// waits about three seconds and then returns. It should keep that socket and
// eventually exit, without calling connect() a second time.
TEST_CASE("TerminalClient exits when the initial response never arrives",
          "[TerminalClient][issue866]") {
  string directory = et::test::makeTempDir("et_866_silent");
  ReproResult result = runIssue866Repro("silent", directory, 20);
  fs::remove_all(directory);
  expectInitialConnectGivesUp(result, true);
}
#endif
