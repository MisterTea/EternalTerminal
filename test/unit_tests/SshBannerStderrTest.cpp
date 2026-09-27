#include <chrono>
#include <thread>

#include "SubprocessUtils.hpp"
#include "TestHeaders.hpp"

using namespace et;

TEST_CASE("SubprocessToStringInteractive streams SSH banner stderr live",
          "[SubprocessUtils]") {
  SubprocessUtils utils;
  // Banner text on stderr must be visible to the user. With separate pipes it
  // is teed live to the parent stderr; capture that stream for the assertion.
#ifdef WIN32
  // Windows interactive capture is covered by the stdout/stderr drain path;
  // live tee assertions use the Unix poll-based implementation below.
  string result = utils.SubprocessToStringInteractive(
      "cmd.exe",
      {"/D", "/S", "/C",
       "echo banner message 1>&2 & echo "
       "IDPASSKEY:iiiiiiiiiiiiiiii/pppppppppppppppppppppppppppppppp"});
  REQUIRE(result.find("IDPASSKEY:") != string::npos);
  REQUIRE(result.find("banner message") == string::npos);
#else
  int errPipe[2];
  REQUIRE(pipe(errPipe) == 0);
  int savedStderr = dup(STDERR_FILENO);
  REQUIRE(savedStderr >= 0);
  REQUIRE(dup2(errPipe[1], STDERR_FILENO) == STDERR_FILENO);
  close(errPipe[1]);

  string result = utils.SubprocessToStringInteractive(
      "sh", {"-c", "echo banner message >&2; printf 'IDPASSKEY:ok'"});

  string streamed;
  char buf[256];
  struct pollfd pfd;
  pfd.fd = errPipe[0];
  pfd.events = POLLIN;
  while (poll(&pfd, 1, 100) > 0) {
    const ssize_t n = read(errPipe[0], buf, sizeof(buf));
    if (n <= 0) {
      break;
    }
    streamed.append(buf, static_cast<size_t>(n));
  }
  close(errPipe[0]);
  dup2(savedStderr, STDERR_FILENO);
  close(savedStderr);

  REQUIRE(streamed.find("banner message") != string::npos);
  REQUIRE(result.find("IDPASSKEY:ok") != string::npos);
  REQUIRE(result.find("banner message") == string::npos);
#endif
}

TEST_CASE("SubprocessToStringInteractive captures stdout only",
          "[SubprocessUtils]") {
  SubprocessUtils utils;
#ifdef WIN32
  string result = utils.SubprocessToStringInteractive(
      "cmd.exe", {"/D", "/S", "/C", "echo stdout & echo stderr 1>&2"});
#else
  string result = utils.SubprocessToStringInteractive(
      "sh", {"-c", "printf stdout; printf stderr >&2"});
#endif
  REQUIRE(result.find("stdout") != string::npos);
  REQUIRE(result.find("stderr") == string::npos);
}

#ifndef WIN32
TEST_CASE(
    "SubprocessToStringInteractive streams stderr while child still waits",
    "[SubprocessUtils]") {
  // Tailscale-style SSH_MSG_USERAUTH_BANNER: the URL must appear on the
  // parent's stderr while ssh is still blocked waiting for the user.
  const string signalPath =
      "/tmp/et-ssh-banner-signal-" + to_string(getpid()) + ".flag";
  ::unlink(signalPath.c_str());

  const string banner = "Visit https://login.tailscale.com/a/abc123";
  const string credential =
      string("IDPASSKEY:") + string(16, 'i') + "/" + string(32, 'p');

  int errPipe[2];
  REQUIRE(pipe(errPipe) == 0);
  int savedStderr = dup(STDERR_FILENO);
  REQUIRE(savedStderr >= 0);
  REQUIRE(dup2(errPipe[1], STDERR_FILENO) == STDERR_FILENO);
  close(errPipe[1]);

  std::atomic<bool> finished{false};
  string result;
  std::exception_ptr threadError;
  std::thread worker([&]() {
    try {
      SubprocessUtils utils;
      result = utils.SubprocessToStringInteractive(
          "sh", {"-c", "printf '%s\\n' '" + banner +
                           "' >&2; "
                           "while [ ! -f '" +
                           signalPath +
                           "' ]; do sleep 0.05; done; "
                           "printf '%s' '" +
                           credential + "'"});
    } catch (...) {
      threadError = std::current_exception();
    }
    finished = true;
  });

  string streamed;
  char buf[256];
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool sawBanner = false;
  while (!sawBanner && std::chrono::steady_clock::now() < deadline) {
    if (finished.load()) {
      break;
    }
    struct pollfd pfd;
    pfd.fd = errPipe[0];
    pfd.events = POLLIN;
    const int ready = poll(&pfd, 1, 100);
    if (ready > 0 && (pfd.revents & POLLIN)) {
      const ssize_t n = read(errPipe[0], buf, sizeof(buf));
      if (n > 0) {
        streamed.append(buf, static_cast<size_t>(n));
        if (streamed.find(banner) != string::npos) {
          sawBanner = true;
        }
      }
    }
  }

  {
    std::ofstream signalFile(signalPath);
    signalFile << "go\n";
  }
  worker.join();
  ::unlink(signalPath.c_str());
  close(errPipe[0]);
  dup2(savedStderr, STDERR_FILENO);
  close(savedStderr);

  REQUIRE(sawBanner);
  REQUIRE(streamed.find("IDPASSKEY:") == string::npos);
  if (threadError) {
    std::rethrow_exception(threadError);
  }
  REQUIRE(result.find(credential) != string::npos);
  // Credential must stay on the captured stdout path, not the live stderr tee.
  REQUIRE(result.find(banner) == string::npos);
}

TEST_CASE(
    "SubprocessToStringInteractive drains more than pipe capacity before wait",
    "[SubprocessUtils]") {
  // Typical OS pipe capacity is 64KiB. Emitting more than that hangs forever
  // if the parent waitpid's before reading.
  constexpr size_t kBytes = 256 * 1024;
  SubprocessUtils utils;
  string result = utils.SubprocessToStringInteractive(
      "sh",
      {"-c", "dd if=/dev/zero bs=1024 count=256 2>/dev/null | tr '\\0' 'A'"});
  REQUIRE(result.size() == kBytes);
  REQUIRE(result == string(kBytes, 'A'));
}

TEST_CASE(
    "SubprocessToStringInteractive drains large stderr without deadlocking",
    "[SubprocessUtils]") {
  // Same pipe-capacity concern on the stderr tee path: waiting before draining
  // stderr hangs once the child fills the pipe. Tee into /dev/null so the
  // parent's live write cannot block the drain loop under test capture.
  int savedStderr = dup(STDERR_FILENO);
  REQUIRE(savedStderr >= 0);
  int devnull = open("/dev/null", O_WRONLY);
  REQUIRE(devnull >= 0);
  REQUIRE(dup2(devnull, STDERR_FILENO) == STDERR_FILENO);
  close(devnull);

  SubprocessUtils utils;
  const auto start = std::chrono::steady_clock::now();
  string result = utils.SubprocessToStringInteractive(
      "sh", {"-c",
             "dd if=/dev/zero bs=1024 count=256 2>/dev/null | tr '\\0' 'B' "
             ">&2; printf DONE"});
  const auto elapsed = std::chrono::steady_clock::now() - start;

  dup2(savedStderr, STDERR_FILENO);
  close(savedStderr);

  REQUIRE(result == "DONE");
  REQUIRE(elapsed < std::chrono::seconds(10));
}
#endif
