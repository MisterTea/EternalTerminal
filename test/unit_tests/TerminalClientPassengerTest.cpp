#include <atomic>
#include <chrono>
#include <thread>

#include "FakeConsole.hpp"
#include "FakeSshSetupHandler.hpp"
#include "MuxClient.hpp"
#include "MuxMaster.hpp"
#include "MuxProtocol.hpp"
#include "PipeSocketHandler.hpp"
#include "TerminalClient.hpp"
#include "TerminalServer.hpp"
#include "TestHeaders.hpp"
#include "UserTerminalHandler.hpp"

using namespace et;

#ifndef WIN32

namespace {

// Must match the marker TerminalClient injects for commanded passengers.
constexpr const char* kPassengerExitMarker = "__ET_PASSENGER_EXIT__:";

string tempControlPath() {
  string dir = et::test::makeTempDir("et_mux_pass");
  return dir + "/control.sock";
}

struct PassengerBridgeStack {
  string pipeDirectory;
  shared_ptr<PipeSocketHandler> serverSocketHandler;
  shared_ptr<PipeSocketHandler> routerSocketHandler;
  shared_ptr<PipeSocketHandler> clientSocketHandler;
  shared_ptr<PipeSocketHandler> clientPipeSocketHandler;
  shared_ptr<FakeUserTerminal> fakeUserTerminal;
  shared_ptr<TerminalServer> server;
  shared_ptr<UserTerminalHandler> uth;
  shared_ptr<TerminalClient> terminalClient;
  thread serverThread;
  thread uthThread;
  atomic<bool> keepServicing{true};
  thread serviceThread;

  PassengerBridgeStack() {
    serverSocketHandler = make_shared<PipeSocketHandler>();
    routerSocketHandler = make_shared<PipeSocketHandler>();
    clientSocketHandler = make_shared<PipeSocketHandler>();
    clientPipeSocketHandler = make_shared<PipeSocketHandler>();

    pipeDirectory = test::makeTempDir("et_passenger");
    SocketEndpoint routerEndpoint;
    routerEndpoint.set_name(pipeDirectory + "/router");
    SocketEndpoint serverEndpoint;
    serverEndpoint.set_name(pipeDirectory + "/server");

    server = make_shared<TerminalServer>(serverSocketHandler, serverEndpoint,
                                         routerSocketHandler, routerEndpoint);
    serverThread = thread([this]() { server->run(); });
    testSleepMicros(200000);

    auto fakeSubprocessUtils = make_shared<FakeSubprocessUtils>();
    auto sshSetupHandler =
        make_shared<FakeSshSetupHandler>(fakeSubprocessUtils);
    auto [id, passkey] = sshSetupHandler->SetupSsh(
        "", "localhost", "localhost", 2022, "", "", false, 0, "", "", {});

    fakeUserTerminal = make_shared<FakeUserTerminal>(routerSocketHandler);
    uth = make_shared<UserTerminalHandler>(routerSocketHandler,
                                           fakeUserTerminal, true,
                                           routerEndpoint, id + "/" + passkey);
    uthThread = thread([this]() { uth->run(); });
    // setup() sleeps ~1s inside FakeUserTerminal; give it a moment to bind
    // before the client connects. Fd publication is mutex-protected so a
    // concurrent drainKeystrokes will just spin until serverClientFd is set.
    testSleepMicros(1200000);

    terminalClient = make_shared<TerminalClient>(
        clientSocketHandler, clientPipeSocketHandler, serverEndpoint, id,
        passkey, nullptr, false, "", "", false, "",
        MAX_CLIENT_KEEP_ALIVE_DURATION, vector<pair<string, string>>{});

    serviceThread = thread([this]() {
      terminalClient->serviceIdleUntil(
          [this]() { return keepServicing.load(); });
    });
    testSleepMicros(150000);
  }

  ~PassengerBridgeStack() { teardown(); }

  void teardown() {
    keepServicing = false;
    if (serviceThread.joinable()) {
      serviceThread.join();
    }
    if (terminalClient) {
      terminalClient->shutdown();
      terminalClient.reset();
    }
    if (uth) {
      uth->shutdown();
    }
    if (uthThread.joinable()) {
      uthThread.join();
    }
    uth.reset();
    if (server) {
      server->shutdown();
    }
    if (serverThread.joinable()) {
      serverThread.join();
    }
    server.reset();
    fakeUserTerminal.reset();
    if (!pipeDirectory.empty()) {
      ::remove((pipeDirectory + "/router").c_str());
      ::remove((pipeDirectory + "/server").c_str());
      test::removeTempDir(pipeDirectory);
      pipeDirectory.clear();
    }
  }
};

}  // namespace

TEST_CASE("passenger stdin EOF keeps bridging until session end",
          "[Mux][Passenger]") {
  PassengerBridgeStack stack;

  int inPipe[2];
  int outPipe[2];
  REQUIRE(::pipe(inPipe) == 0);
  REQUIRE(::pipe(outPipe) == 0);
  ::close(inPipe[1]);

  atomic<bool> finished{false};
  uint32_t status = 255;
  string error;
  auto start = std::chrono::steady_clock::now();
  thread passengerThread([&]() {
    try {
      status = stack.terminalClient->runPassengerSession(inPipe[0], outPipe[1],
                                                         outPipe[1], "");
    } catch (const std::exception& ex) {
      error = ex.what();
      status = 254;
    }
    finished.store(true);
  });

  testSleepMicros(400000);
  bool stillRunning = !finished.load();
  stack.keepServicing = false;
  passengerThread.join();
  auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

  ::close(inPipe[0]);
  ::close(outPipe[0]);
  ::close(outPipe[1]);
  stack.teardown();

  INFO(error);
  REQUIRE(error.empty());
  REQUIRE(stillRunning);
  REQUIRE(elapsedMs >= 350);
  REQUIRE(status == 1);
}

TEST_CASE("commanded passenger does not exit the shared shell",
          "[Mux][Passenger]") {
  PassengerBridgeStack stack;

  int inPipe[2];
  int outPipe[2];
  REQUIRE(::pipe(inPipe) == 0);
  REQUIRE(::pipe(outPipe) == 0);
  ::close(inPipe[1]);

  atomic<bool> firstDone{false};
  uint32_t firstStatus = 255;
  string firstError;
  thread firstPassenger([&]() {
    try {
      firstStatus = stack.terminalClient->runPassengerSession(
          inPipe[0], outPipe[1], outPipe[1], "echo hello");
    } catch (const std::exception& ex) {
      firstError = ex.what();
      firstStatus = 254;
    }
    firstDone.store(true);
  });

  string typed = stack.fakeUserTerminal->drainKeystrokes(128, 2500);

  // Always complete or tear down before asserts so threads are joinable-safe.
  stack.fakeUserTerminal->simulateTerminalResponse(
      string("\n") + kPassengerExitMarker + "0\n");

  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
  while (!firstDone.load() && std::chrono::steady_clock::now() < deadline) {
    testSleepMicros(20000);
  }
  if (!firstDone.load()) {
    stack.keepServicing = false;
  }
  firstPassenger.join();

  INFO(firstError);
  INFO(typed);
  REQUIRE(firstError.empty());
  const string expectedTyped = string("(echo hello); printf '\\n") +
                               kPassengerExitMarker + "%d\\n' $?\n";
  REQUIRE(typed == expectedTyped);
  REQUIRE(!typed.empty());
  REQUIRE(typed.back() == '\n');
  REQUIRE(typed.find("; exit") == string::npos);
  REQUIRE(firstDone.load());
  REQUIRE(firstStatus == 0);
  REQUIRE(stack.keepServicing.load());

  int inPipe2[2];
  int outPipe2[2];
  REQUIRE(::pipe(inPipe2) == 0);
  REQUIRE(::pipe(outPipe2) == 0);
  ::close(inPipe2[1]);

  atomic<bool> secondDone{false};
  uint32_t secondStatus = 255;
  thread secondPassenger([&]() {
    try {
      secondStatus = stack.terminalClient->runPassengerSession(
          inPipe2[0], outPipe2[1], outPipe2[1], "echo again");
    } catch (...) {
      secondStatus = 254;
    }
    secondDone.store(true);
  });

  string typed2 = stack.fakeUserTerminal->drainKeystrokes(128, 2500);
  INFO(typed2);
  const string expectedTyped2 = string("(echo again); printf '\\n") +
                                kPassengerExitMarker + "%d\\n' $?\n";
  REQUIRE(typed2 == expectedTyped2);
  REQUIRE(!typed2.empty());
  REQUIRE(typed2.back() == '\n');
  REQUIRE(typed2.find("; exit") == string::npos);

  stack.fakeUserTerminal->simulateTerminalResponse(
      string("\n") + kPassengerExitMarker + "0\n");

  deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
  while (!secondDone.load() && std::chrono::steady_clock::now() < deadline) {
    testSleepMicros(20000);
  }
  if (!secondDone.load()) {
    stack.keepServicing = false;
  }
  secondPassenger.join();

  ::close(inPipe[0]);
  ::close(outPipe[0]);
  ::close(outPipe[1]);
  ::close(inPipe2[0]);
  ::close(outPipe2[0]);
  ::close(outPipe2[1]);
  stack.teardown();

  REQUIRE(secondDone.load());
  REQUIRE(secondStatus == 0);
}

TEST_CASE("back-to-back commanded passengers inject each command",
          "[Mux][Passenger]") {
  // Regression: after passenger 1 finishes, runPassengerSession clears
  // `active` and returns; a second attach can set `active` again before the
  // idle loop observes the inactive gap. Per-session locals
  // (injectedPassengerCommand / passengerInputDisabled) must not stick across
  // that missed gap, or the second command is never typed.
  PassengerBridgeStack stack;

  int inPipe1[2];
  int outPipe1[2];
  int inPipe2[2];
  int outPipe2[2];
  REQUIRE(::pipe(inPipe1) == 0);
  REQUIRE(::pipe(outPipe1) == 0);
  REQUIRE(::pipe(inPipe2) == 0);
  REQUIRE(::pipe(outPipe2) == 0);
  ::close(inPipe1[1]);
  ::close(inPipe2[1]);

  atomic<bool> firstReturned{false};
  atomic<bool> bothDone{false};
  uint32_t firstStatus = 255;
  uint32_t secondStatus = 255;
  string error;
  thread passenger([&]() {
    try {
      firstStatus = stack.terminalClient->runPassengerSession(
          inPipe1[0], outPipe1[1], outPipe1[1], "echo hello");
      firstReturned.store(true);
      // Immediately re-attach — no multi-second drain gap for idle to reset.
      secondStatus = stack.terminalClient->runPassengerSession(
          inPipe2[0], outPipe2[1], outPipe2[1], "echo again");
    } catch (const std::exception& ex) {
      error = ex.what();
      firstStatus = 254;
      secondStatus = 254;
    }
    bothDone.store(true);
  });

  const string expectedTyped1 = string("(echo hello); printf '\\n") +
                                kPassengerExitMarker + "%d\\n' $?\n";
  const string expectedTyped2 = string("(echo again); printf '\\n") +
                                kPassengerExitMarker + "%d\\n' $?\n";

  string typed1 = stack.fakeUserTerminal->drainKeystrokes(128, 2500);
  INFO(typed1);
  REQUIRE(typed1 == expectedTyped1);
  stack.fakeUserTerminal->simulateTerminalResponse(
      string("\n") + kPassengerExitMarker + "0\n");

  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
  while (!firstReturned.load() && std::chrono::steady_clock::now() < deadline) {
    testSleepMicros(5000);
  }
  REQUIRE(firstReturned.load());
  REQUIRE(firstStatus == 0);

  string typed2 = stack.fakeUserTerminal->drainKeystrokes(128, 2500);
  INFO(typed2);
  INFO(error);
  REQUIRE(typed2 == expectedTyped2);

  stack.fakeUserTerminal->simulateTerminalResponse(
      string("\n") + kPassengerExitMarker + "0\n");

  deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
  while (!bothDone.load() && std::chrono::steady_clock::now() < deadline) {
    testSleepMicros(20000);
  }
  if (!bothDone.load()) {
    stack.keepServicing = false;
  }
  passenger.join();

  ::close(inPipe1[0]);
  ::close(outPipe1[0]);
  ::close(outPipe1[1]);
  ::close(inPipe2[0]);
  ::close(outPipe2[0]);
  ::close(outPipe2[1]);
  stack.teardown();

  REQUIRE(error.empty());
  REQUIRE(bothDone.load());
  REQUIRE(secondStatus == 0);
}

TEST_CASE("TerminalClient passenger bridge propagates command exit status",
          "[Mux][Passenger]") {
  PassengerBridgeStack stack;
  string path = tempControlPath();
  ControlPersistConfig persist;
  persist.enabled = false;

  MuxMaster master(path, persist);
  master.setPassengerSessionHandler([&](int inFd, int outFd, int errFd,
                                        const string& command,
                                        bool /*wantTty*/) -> uint32_t {
    stack.terminalClient->beginPassengerWatch();
    return stack.terminalClient->runPassengerSession(inFd, outFd, errFd,
                                                     command);
  });
  master.setPassengerCancelHandler(
      [&]() { stack.terminalClient->cancelPassengerSession(); });
  master.start();

  thread markerThread([&]() {
    string typed = stack.fakeUserTerminal->drainKeystrokes(256, 3000);
    if (!typed.empty()) {
      stack.fakeUserTerminal->simulateTerminalResponse(
          string("\n") + kPassengerExitMarker + "42\n");
    }
  });

  MuxClient client(path);
  REQUIRE(client.connect());

  int inPipe[2];
  int outPipe[2];
  REQUIRE(::pipe(inPipe) == 0);
  REQUIRE(::pipe(outPipe) == 0);
  ::close(inPipe[1]);

  uint32_t sessionId = 0;
  uint32_t exitStatus = 0;
  string error;
  bool ok = client.newSession("sh -c 'exit 42'", false, inPipe[0], outPipe[1],
                              outPipe[1], &sessionId, &error, &exitStatus);
  markerThread.join();

  ::close(inPipe[0]);
  ::close(outPipe[0]);
  ::close(outPipe[1]);
  master.stop();
  stack.teardown();

  INFO(error);
  REQUIRE(ok);
  REQUIRE(sessionId >= 1);
  REQUIRE(exitStatus == 42);
}

TEST_CASE(
    "hangup before passenger.active still cancels via cancelPassengerSession",
    "[Mux][Passenger][hangup]") {
  // Regression: hangup can arrive after SESSION_OPENED but before
  // runPassengerSession sets passenger.active. cancelPassengerSession must
  // still complete the session (sticky cancel), not one-shot no-op. Uses the
  // real cancel path — not a sticky test-only flag.
  PassengerBridgeStack stack;
  string path = tempControlPath();
  ControlPersistConfig persist;
  persist.enabled = true;
  persist.seconds = 1;

  atomic<bool> handlerEntered{false};
  atomic<bool> allowActivate{false};

  MuxMaster master(path, persist);
  master.setPassengerSessionHandler([&](int inFd, int outFd, int errFd,
                                        const string& /*command*/,
                                        bool /*wantTty*/) -> uint32_t {
    stack.terminalClient->beginPassengerWatch();
    handlerEntered.store(true);
    // Hold off becoming active until hangup has had a chance to fire cancel
    // while passenger.active is still false.
    auto gateDeadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (!allowActivate.load() &&
           std::chrono::steady_clock::now() < gateDeadline) {
      testSleepMicros(10000);
    }
    return stack.terminalClient->runPassengerSession(inFd, outFd, errFd, "");
  });
  master.setPassengerCancelHandler(
      [&]() { stack.terminalClient->cancelPassengerSession(); });
  master.start();
  master.notifyPrimaryClientExited();

  MuxClient client(path);
  REQUIRE(client.connect());

  int inPipe[2];
  int outPipe[2];
  REQUIRE(::pipe(inPipe) == 0);
  REQUIRE(::pipe(outPipe) == 0);
  // Keep stdin write end open (interactive attach) so local EOF is not the
  // completion signal.

  atomic<bool> sessionFinished{false};
  thread sessionThread([&]() {
    uint32_t sessionId = 0;
    uint32_t exitStatus = 0;
    string error;
    (void)client.newSession("", true, inPipe[0], outPipe[1], outPipe[1],
                            &sessionId, &error, &exitStatus);
    sessionFinished.store(true);
  });

  auto enterDeadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  while (std::chrono::steady_clock::now() < enterDeadline &&
         !handlerEntered.load()) {
    testSleepMicros(10000);
  }
  REQUIRE(handlerEntered.load());
  REQUIRE(master.activeClientCount() >= 1);

  // Hangup while the handler is gated — cancel fires before active.
  client.hangup();
  testSleepMicros(150000);
  allowActivate.store(true);

  auto clientsDeadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  while (std::chrono::steady_clock::now() < clientsDeadline &&
         master.activeClientCount() > 0) {
    testSleepMicros(20000);
  }
  REQUIRE(master.activeClientCount() == 0);

  auto persistDeadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
  while (std::chrono::steady_clock::now() < persistDeadline &&
         master.isRunning() && !master.persistExpired()) {
    testSleepMicros(50000);
  }
  REQUIRE((master.persistExpired() || !master.isRunning()));

  auto joinDeadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  while (std::chrono::steady_clock::now() < joinDeadline &&
         !sessionFinished.load()) {
    testSleepMicros(20000);
  }
  if (!sessionFinished.load()) {
    // Unstick teardown if cancel still races wrong: release gate and stop.
    allowActivate.store(true);
    stack.keepServicing = false;
  }
  if (sessionThread.joinable()) {
    sessionThread.join();
  }

  ::close(inPipe[0]);
  ::close(inPipe[1]);
  ::close(outPipe[0]);
  ::close(outPipe[1]);
  master.stop();
  stack.teardown();
}

TEST_CASE("hangup mid-attach does not poison the next passenger session",
          "[Mux][Passenger][hangup]") {
  // Regression: MuxMaster used to re-fire cancelPassengerSession every ~50ms
  // until the handler finished. cancel always sets sticky cancelRequested;
  // runPassengerSession clears it on exit, then a late re-fire can set it
  // again so the next attach dies immediately with status 1. Sticky cancel
  // already covers the pre-active race — re-fire after apply is poisonous.
  PassengerBridgeStack stack;
  string path = tempControlPath();
  ControlPersistConfig persist;
  persist.enabled = true;
  persist.seconds = 30;

  MuxMaster master(path, persist);
  master.setPassengerSessionHandler([&](int inFd, int outFd, int errFd,
                                        const string& command,
                                        bool /*wantTty*/) -> uint32_t {
    stack.terminalClient->beginPassengerWatch();
    return stack.terminalClient->runPassengerSession(inFd, outFd, errFd,
                                                     command);
  });
  master.setPassengerCancelHandler(
      [&]() { stack.terminalClient->cancelPassengerSession(); });
  master.start();
  master.notifyPrimaryClientExited();

  MuxClient client(path);
  REQUIRE(client.connect());

  int inPipe[2];
  int outPipe[2];
  REQUIRE(::pipe(inPipe) == 0);
  REQUIRE(::pipe(outPipe) == 0);

  atomic<bool> firstFinished{false};
  thread firstSession([&]() {
    uint32_t sessionId = 0;
    uint32_t exitStatus = 0;
    string error;
    // Hangup shuts down the control peer before EXIT_MESSAGE is readable, so
    // newSession typically fails; the poison under test lives on the master
    // TerminalClient cancelRequested flag, not this client-side status.
    (void)client.newSession("", true, inPipe[0], outPipe[1], outPipe[1],
                            &sessionId, &error, &exitStatus);
    firstFinished.store(true);
  });

  auto attachDeadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  while (std::chrono::steady_clock::now() < attachDeadline &&
         master.sessionCount() == 0) {
    testSleepMicros(20000);
  }
  REQUIRE(master.sessionCount() >= 1);
  REQUIRE(master.activeClientCount() >= 1);
  // Let runPassengerSession become active before hangup.
  testSleepMicros(100000);

  // Hangup while attached so cancel fires (and, with the bug, keeps re-firing).
  client.hangup();

  auto clientsDeadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  while (std::chrono::steady_clock::now() < clientsDeadline &&
         master.activeClientCount() > 0) {
    testSleepMicros(20000);
  }
  REQUIRE(master.activeClientCount() == 0);

  auto firstJoinDeadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  while (std::chrono::steady_clock::now() < firstJoinDeadline &&
         !firstFinished.load()) {
    testSleepMicros(20000);
  }
  REQUIRE(firstFinished.load());
  if (firstSession.joinable()) {
    firstSession.join();
  }
  // Give the watch loop time to issue post-exit cancel re-fires if present.
  testSleepMicros(200000);

  ::close(inPipe[0]);
  ::close(inPipe[1]);
  ::close(outPipe[0]);
  ::close(outPipe[1]);

  // Persist must still be armed so a second attach can proceed.
  REQUIRE(master.isRunning());
  REQUIRE_FALSE(master.persistExpired());

  MuxClient client2(path);
  REQUIRE(client2.connect());

  int inPipe2[2];
  int outPipe2[2];
  REQUIRE(::pipe(inPipe2) == 0);
  REQUIRE(::pipe(outPipe2) == 0);
  ::close(inPipe2[1]);

  atomic<bool> secondFinished{false};
  uint32_t secondExitStatus = 255;
  string secondError;
  thread secondSession([&]() {
    uint32_t sessionId = 0;
    (void)client2.newSession("echo again", false, inPipe2[0], outPipe2[1],
                             outPipe2[1], &sessionId, &secondError,
                             &secondExitStatus);
    secondFinished.store(true);
  });

  // If sticky cancel was poisoned, runPassengerSession returns 1 immediately
  // without injecting a command — drain would time out / typed would be empty.
  string typed = stack.fakeUserTerminal->drainKeystrokes(128, 2500);
  if (!typed.empty()) {
    stack.fakeUserTerminal->simulateTerminalResponse(
        string("\n") + kPassengerExitMarker + "0\n");
  }

  auto secondJoinDeadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
  while (std::chrono::steady_clock::now() < secondJoinDeadline &&
         !secondFinished.load()) {
    testSleepMicros(20000);
  }
  if (!secondFinished.load()) {
    stack.keepServicing = false;
  }
  if (secondSession.joinable()) {
    secondSession.join();
  }

  ::close(inPipe2[0]);
  ::close(outPipe2[0]);
  ::close(outPipe2[1]);
  master.stop();
  stack.teardown();

  const string expectedTyped = string("(echo again); printf '\\n") +
                               kPassengerExitMarker + "%d\\n' $?\n";
  INFO(typed);
  INFO(secondError);
  REQUIRE(secondFinished.load());
  REQUIRE(secondError.empty());
  REQUIRE(typed == expectedTyped);
  REQUIRE(secondExitStatus == 0);
}

#endif  // !WIN32
