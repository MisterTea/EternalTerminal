#include <atomic>
#include <chrono>
#include <fstream>
#include <future>

#ifndef WIN32
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "ETerminal.pb.h"
#include "FakeConsole.hpp"
#include "FakeSshSetupHandler.hpp"
#include "PipeSocketHandler.hpp"
#include "PipeUserTerminal.hpp"
#include "SshSetupHandler.hpp"
#include "SubprocessUtils.hpp"
#include "TerminalClient.hpp"
#include "TerminalServer.hpp"
#include "TestHeaders.hpp"
#include "UserTerminalHandler.hpp"

using namespace et;

#ifndef WIN32
TEST_CASE("PipeUserTerminal keeps binary stdout and stderr separate",
          "[RawCommandChannel]") {
  // POSIX octal escapes: dash (Ubuntu /bin/sh), FreeBSD sh, bash, and zsh.
  // Hex \\xHH is not portable (dash prints it literally; FreeBSD sh drops it).
  const string cmd =
      "printf '\\000\\001\\002STDOUT'; printf '\\377\\376\\375STDERR' 1>&2";
  PipeUserTerminal term(cmd);
  int stdoutFd = term.setup(-1);
  REQUIRE(stdoutFd >= 0);
  int stderrFd = term.getStderrFd();
  REQUIRE(stderrFd >= 0);
  REQUIRE(term.getInputFd() >= 0);
  REQUIRE(term.getInputFd() != stdoutFd);
  REQUIRE(stderrFd != stdoutFd);

  string stdoutBytes;
  string stderrBytes;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  char buf[256];
  while (std::chrono::steady_clock::now() < deadline &&
         (stdoutBytes.find("STDOUT") == string::npos ||
          stderrBytes.find("STDERR") == string::npos)) {
    ssize_t n = read(stdoutFd, buf, sizeof(buf));
    if (n > 0) {
      stdoutBytes.append(buf, n);
    }
    n = read(stderrFd, buf, sizeof(buf));
    if (n > 0) {
      stderrBytes.append(buf, n);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  REQUIRE(stdoutBytes == (string("\0\x01\x02", 3) + "STDOUT"));
  REQUIRE(stderrBytes == (string("\xff\xfe\xfd", 3) + "STDERR"));
  REQUIRE(stdoutBytes.find("STDERR") == string::npos);
  REQUIRE(stderrBytes.find("STDOUT") == string::npos);

  term.handleSessionEnd();
  term.cleanup();
}
#endif

TEST_CASE("Raw command channel: separate streams and no shell ; exit inject",
          "[RawCommandChannel][Integration]") {
  auto serverSocketHandler = make_shared<PipeSocketHandler>();
  auto routerSocketHandler = make_shared<PipeSocketHandler>();
  auto clientSocketHandler = make_shared<PipeSocketHandler>();
  auto clientPipeSocketHandler = make_shared<PipeSocketHandler>();
  auto consoleSocketHandler = make_shared<PipeSocketHandler>();

  string pipeDirectory = test::makeTempDir("et_raw_e2e");
  SocketEndpoint routerEndpoint;
  routerEndpoint.set_name(pipeDirectory + "/router");
  SocketEndpoint serverEndpoint;
  serverEndpoint.set_name(pipeDirectory + "/server");

  shared_ptr<TerminalServer> server(
      new TerminalServer(serverSocketHandler, serverEndpoint,
                         routerSocketHandler, routerEndpoint));
  thread serverThread([server]() { server->run(); });
  std::this_thread::sleep_for(std::chrono::seconds(1));

  string sideFile = pipeDirectory + "/stdin_seen";
#ifndef WIN32
  // Trailing `true` keeps /bin/sh from exec'ing `cat >file` as the last
  // command (FreeBSD sh), which would close the pipe write end while cat
  // still runs. Handler also closes stdin on stdout EOF as a backstop.
  string command =
      "printf '\\000\\001OUT'; printf '\\376\\377ERR' 1>&2; cat >'" + sideFile +
      "'; true";
#else
  // Smoke command only; binary stream checks are Unix-only below. `more` is
  // used so the session stays up until the client closes stdin (same role as
  // Unix `cat`).
  string command = "echo OUT& more";
#endif

  auto fakeSubprocessUtils = make_shared<FakeSubprocessUtils>();
  auto sshSetupHandler = make_shared<FakeSshSetupHandler>(fakeSubprocessUtils);
  auto [id, passkey] = sshSetupHandler->SetupSsh(
      "", "localhost", "localhost", 2022, "", "", false, 0, "", "", {});

  // Seed terminal is replaced when TermInit carries no_pty + command.
  auto seedTerm = make_shared<FakeUserTerminal>(routerSocketHandler);
  auto uth = make_shared<UserTerminalHandler>(
      routerSocketHandler, seedTerm, true, routerEndpoint, id + "/" + passkey);
  thread uthThread([uth]() { uth->run(); });
  std::this_thread::sleep_for(std::chrono::seconds(1));

#ifndef WIN32
  int stderrPipe[2];
  REQUIRE(pipe(stderrPipe) == 0);
  int savedStderr = dup(STDERR_FILENO);
  REQUIRE(savedStderr >= 0);
  REQUIRE(dup2(stderrPipe[1], STDERR_FILENO) >= 0);
  close(stderrPipe[1]);
  int flags = fcntl(stderrPipe[0], F_GETFL, 0);
  fcntl(stderrPipe[0], F_SETFL, flags | O_NONBLOCK);
#endif

  auto fakeConsole = make_shared<FakeConsole>(consoleSocketHandler);
  shared_ptr<TerminalClient> terminalClient(new TerminalClient(
      clientSocketHandler, clientPipeSocketHandler, serverEndpoint, id, passkey,
      fakeConsole, false, "", "", false, "", MAX_CLIENT_KEEP_ALIVE_DURATION, {},
      true /* noPty */, command));

  thread clientThread(
      [terminalClient, command]() { terminalClient->run(command, false); });

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);

  // Wait for FakeConsole setup then collect stdout.
  while (std::chrono::steady_clock::now() < deadline &&
         !fakeConsole->isSetup()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(fakeConsole->isSetup());

  string stdoutCollected;
  auto collectStdout = async(std::launch::async, [&]() {
    while (std::chrono::steady_clock::now() < deadline) {
      try {
        stdoutCollected += fakeConsole->getTerminalData(1);
        if (stdoutCollected.find("OUT") != string::npos) {
          return;
        }
      } catch (...) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
  });

#ifndef WIN32
  string stderrCollected;
  char buf[256];
  while (std::chrono::steady_clock::now() < deadline &&
         stderrCollected.find("ERR") == string::npos) {
    ssize_t n = read(stderrPipe[0], buf, sizeof(buf));
    if (n > 0) {
      stderrCollected.append(buf, n);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
#endif

  collectStdout.wait_for(std::chrono::seconds(15));

  // Closing the client closes remote stdin so `cat` finishes.
  terminalClient->shutdown();
  if (clientThread.joinable()) {
    clientThread.join();
  }
  uth->shutdown();
  if (uthThread.joinable()) {
    uthThread.join();
  }
  server->shutdown();
  if (serverThread.joinable()) {
    serverThread.join();
  }

#ifndef WIN32
  dup2(savedStderr, STDERR_FILENO);
  close(savedStderr);
  close(stderrPipe[0]);

  REQUIRE(stdoutCollected.find(string("\0\x01", 2) + "OUT") != string::npos);
  REQUIRE(stderrCollected.find(string("\xfe\xff", 2) + "ERR") != string::npos);
  REQUIRE(stdoutCollected.find("ERR") == string::npos);
  REQUIRE(stderrCollected.find("OUT") == string::npos);

  // Prove the command was not typed into a shell as "cmd; exit".
  std::ifstream in(sideFile);
  string stdinSeen((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  REQUIRE(stdinSeen.find("; exit") == string::npos);
  REQUIRE(stdinSeen.find("printf") == string::npos);
#endif

  // PipeSocketHandler leaves endpoint files; removeContents then the dir.
  ::remove((pipeDirectory + "/router").c_str());
  ::remove((pipeDirectory + "/server").c_str());
  ::remove(sideFile.c_str());
  test::removeTempDir(pipeDirectory);
}
