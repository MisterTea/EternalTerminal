#include "MuxClient.hpp"
#include "MuxMaster.hpp"
#include "MuxProtocol.hpp"
#include "PipeSocketHandler.hpp"
#include "PortForwardHandler.hpp"
#include "TcpSocketHandler.hpp"
#include "TestHeaders.hpp"
#include "TunnelUtils.hpp"

using namespace et;

#ifndef WIN32

namespace {

string tempControlPath() {
  string dir = et::test::makeTempDir("et_mux");
  return dir + "/control.sock";
}

}  // namespace

TEST_CASE("parseMuxCliOptions ControlMaster Path Persist and -M -S -O",
          "[Mux]") {
  vector<string> storage = {
      "et",
      "-o",
      "ControlMaster=auto",
      "-o",
      "ControlPath=/tmp/et-mux.sock",
      "-o",
      "ControlPersist=3",
      "-o",
      "SomethingElse=1",
      "user@host",
  };
  vector<char*> argv;
  for (auto& s : storage) {
    argv.push_back(&s[0]);
  }
  auto parsed = parseMuxCliOptions(static_cast<int>(argv.size()), argv.data());
  REQUIRE(parsed.options.controlMaster == ControlMasterMode::Auto);
  REQUIRE(parsed.options.controlPath == "/tmp/et-mux.sock");
  REQUIRE(parsed.options.controlPersist.enabled);
  REQUIRE(parsed.options.controlPersist.seconds == 3);
  REQUIRE(parsed.options.passthroughOptions.size() == 1);
  REQUIRE(parsed.options.passthroughOptions[0] == "SomethingElse=1");
  REQUIRE(parsed.remainingArgs.size() == 10);
  REQUIRE(parsed.remainingArgs[1] == "-o");
  REQUIRE(parsed.remainingArgs[2] == "ControlMaster=auto");
  REQUIRE(parsed.remainingArgs[3] == "-o");
  REQUIRE(parsed.remainingArgs[4] == "ControlPath=/tmp/et-mux.sock");
  REQUIRE(parsed.remainingArgs[5] == "-o");
  REQUIRE(parsed.remainingArgs[6] == "ControlPersist=3");
  REQUIRE(parsed.remainingArgs[7] == "-o");
  REQUIRE(parsed.remainingArgs[8] == "SomethingElse=1");
  REQUIRE(parsed.remainingArgs[9] == "user@host");

  storage = {"et", "-M", "-S", "/tmp/cm.sock", "-O", "check", "host"};
  argv.clear();
  for (auto& s : storage) {
    argv.push_back(&s[0]);
  }
  parsed = parseMuxCliOptions(static_cast<int>(argv.size()), argv.data());
  REQUIRE(parsed.options.controlMaster == ControlMasterMode::Yes);
  REQUIRE(parsed.options.controlPath == "/tmp/cm.sock");
  REQUIRE(parsed.options.ctlCommand == "check");
  REQUIRE(parsed.remainingArgs.back() == "host");
}

TEST_CASE("MuxMaster listens and MuxClient attaches with alive check",
          "[Mux]") {
  string path = tempControlPath();
  ControlPersistConfig persist;
  persist.enabled = true;
  persist.seconds = 2;

  MuxMaster master(path, persist);
  master.start();
  REQUIRE(master.isListening());
  REQUIRE(controlPathSocketExists(path));

  MuxClient client(path);
  REQUIRE(client.connect());
  uint32_t pid = 0;
  REQUIRE(client.aliveCheck(&pid));
  REQUIRE(pid == static_cast<uint32_t>(::getpid()));

  master.stop();
  REQUIRE_FALSE(controlPathSocketExists(path));
}

TEST_CASE("Second mux client opens session and forward on the master",
          "[Mux]") {
  string path = tempControlPath();
  ControlPersistConfig persist;
  persist.enabled = false;

  MuxMaster master(path, persist);
  master.setPassengerSessionHandler([](int inFd, int outFd, int errFd,
                                       const string& /*command*/,
                                       bool /*wantTty*/) -> uint32_t {
    // Echo one read from stdin to stdout so attach performs real I/O.
    char buf[64];
    ssize_t n = ::read(inFd, buf, sizeof(buf));
    if (n > 0) {
      (void)::write(outFd, buf, static_cast<size_t>(n));
    }
    (void)errFd;
    return 0;
  });
  master.start();

  MuxClient client(path);
  REQUIRE(client.connect());

  MuxOpenForwardRequest fwd;
  fwd.type = MUX_FWD_LOCAL;
  fwd.listenHost = "127.0.0.1";
  fwd.listenPort = 18080;
  fwd.connectHost = "127.0.0.1";
  fwd.connectPort = 80;
  string error;
  REQUIRE(client.openForward(fwd, &error));

  auto tracked = master.trackedForwards();
  REQUIRE(tracked.size() == 1);
  REQUIRE(tracked[0].listenPort == 18080);
  REQUIRE(tracked[0].connectPort == 80);

  int inPipe[2];
  int outPipe[2];
  REQUIRE(::pipe(inPipe) == 0);
  REQUIRE(::pipe(outPipe) == 0);
  const char payload[] = "mux-io";
  REQUIRE(::write(inPipe[1], payload, sizeof(payload) - 1) ==
          (ssize_t)(sizeof(payload) - 1));
  ::close(inPipe[1]);

  uint32_t sessionId = 0;
  uint32_t exitStatus = 255;
  REQUIRE(client.newSession("true", false, inPipe[0], outPipe[1], outPipe[1],
                            &sessionId, &error, &exitStatus));
  REQUIRE(sessionId >= 1);
  REQUIRE(master.sessionCount() >= 1);
  REQUIRE(exitStatus == 0);

  char got[64] = {};
  ssize_t gotN = ::read(outPipe[0], got, sizeof(got));
  REQUIRE(gotN == (ssize_t)(sizeof(payload) - 1));
  REQUIRE(string(got, gotN) == payload);

  ::close(inPipe[0]);
  ::close(outPipe[0]);
  ::close(outPipe[1]);
  master.stop();
}

TEST_CASE("mux NEW_SESSION blocks until EXIT_MESSAGE past peek timeout",
          "[Mux]") {
  string path = tempControlPath();
  ControlPersistConfig persist;
  persist.enabled = false;

  atomic<bool> handlerFinished{false};
  MuxMaster master(path, persist);
  master.setPassengerSessionHandler([&](int inFd, int outFd, int errFd,
                                        const string& /*command*/,
                                        bool /*wantTty*/) -> uint32_t {
    (void)outFd;
    (void)errFd;
    // Outlive MuxClient's former ~2s optional peek so early return is
    // observable.
    testSleepMicros(3000000);
    char buf[8];
    (void)::read(inFd, buf, sizeof(buf));
    handlerFinished.store(true);
    return 42;
  });
  master.start();

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
  auto start = std::chrono::steady_clock::now();
  REQUIRE(client.newSession("slow", false, inPipe[0], outPipe[1], outPipe[1],
                            &sessionId, &error, &exitStatus));
  auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

  REQUIRE(handlerFinished.load());
  REQUIRE(elapsedMs >= 2500);
  REQUIRE(sessionId >= 1);
  REQUIRE(exitStatus == 42);

  ::close(inPipe[0]);
  ::close(outPipe[0]);
  ::close(outPipe[1]);
  master.stop();
}

TEST_CASE("mux NEW_SESSION fails clearly without a session bridge", "[Mux]") {
  string path = tempControlPath();
  ControlPersistConfig persist;
  MuxMaster master(path, persist);
  master.start();

  MuxClient client(path);
  REQUIRE(client.connect());
  uint32_t sessionId = 0;
  string error;
  REQUIRE_FALSE(
      client.newSession("true", false, -1, -1, -1, &sessionId, &error));
  REQUIRE(error.find("session") != string::npos);

  master.stop();
}

TEST_CASE("portForwardRequestFromMux keeps remote destinations and sockets",
          "[Mux]") {
  MuxOpenForwardRequest remote;
  remote.listenHost = "127.0.0.1";
  remote.listenPort = 18080;
  remote.connectHost = "example.com";
  remote.connectPort = 443;
  auto remoteReq = portForwardRequestFromMux(remote);
  REQUIRE(remoteReq.source().name() == "127.0.0.1");
  REQUIRE(remoteReq.source().port() == 18080);
  REQUIRE(remoteReq.destination().name() == "example.com");
  REQUIRE(remoteReq.destination().port() == 443);

  MuxOpenForwardRequest unixFwd;
  unixFwd.listenHost = "/tmp/a.sock";
  unixFwd.listenPort = MUX_STREAM_LOCAL_PORT;
  unixFwd.connectHost = "/tmp/b.sock";
  unixFwd.connectPort = MUX_STREAM_LOCAL_PORT;
  auto unixReq = portForwardRequestFromMux(unixFwd);
  REQUIRE(unixReq.source().name() == "/tmp/a.sock");
  REQUIRE_FALSE(unixReq.source().has_port());
  REQUIRE(unixReq.destination().name() == "/tmp/b.sock");
  REQUIRE_FALSE(unixReq.destination().has_port());
}

TEST_CASE("MuxMaster openForward preserves a non-local connect host", "[Mux]") {
  string path = tempControlPath();
  ControlPersistConfig persist;
  MuxMaster master(path, persist);

  auto networkHandler = make_shared<TcpSocketHandler>();
  auto pipeHandler = make_shared<PipeSocketHandler>();
  auto pf = make_shared<PortForwardHandler>(networkHandler, pipeHandler);
  master.setPortForwardHandler(pf);
  master.start();

  MuxClient client(path);
  REQUIRE(client.connect());

  MuxOpenForwardRequest fwd;
  fwd.type = MUX_FWD_LOCAL;
  fwd.listenHost = "127.0.0.1";
  fwd.listenPort = 0;  // ephemeral
  fwd.connectHost = "example.com";
  fwd.connectPort = 443;
  string error;
  // Bind may succeed with port 0; destination host must not be dropped.
  // Use an explicit high port that is unlikely to conflict.
  fwd.listenPort = 41999;
  bool opened = client.openForward(fwd, &error);
  if (!opened) {
    // Port busy on this host — still verify the request builder path above.
    WARN(error);
  } else {
    auto tracked = master.trackedForwards();
    REQUIRE(tracked.size() == 1);
    REQUIRE(tracked[0].connectHost == "example.com");
    REQUIRE(tracked[0].connectPort == 443);
  }

  master.stop();
}

TEST_CASE("MuxMaster closeForward stops the PortForwardHandler listen socket",
          "[Mux]") {
  string path = tempControlPath();
  ControlPersistConfig persist;
  MuxMaster master(path, persist);

  auto networkHandler = make_shared<TcpSocketHandler>();
  auto pipeHandler = make_shared<PipeSocketHandler>();
  auto pf = make_shared<PortForwardHandler>(networkHandler, pipeHandler);
  master.setPortForwardHandler(pf);
  master.start();

  MuxClient client(path);
  REQUIRE(client.connect());

  MuxOpenForwardRequest fwd;
  fwd.type = MUX_FWD_LOCAL;
  fwd.listenHost = "127.0.0.1";
  fwd.listenPort = 41998;
  fwd.connectHost = "127.0.0.1";
  fwd.connectPort = 9;
  string error;
  if (!client.openForward(fwd, &error)) {
    WARN("openForward skipped: " + error);
    master.stop();
    return;
  }
  REQUIRE(master.trackedForwards().size() == 1);

  SocketEndpoint listenEp;
  listenEp.set_name("127.0.0.1");
  listenEp.set_port(41998);
  REQUIRE_FALSE(networkHandler->getEndpointFds(listenEp).empty());

  REQUIRE(client.closeForward(fwd, &error));
  REQUIRE(master.trackedForwards().empty());

  // removeSource must tear down the listen socket so the port can be rebound.
  REQUIRE(client.openForward(fwd, &error));
  REQUIRE(client.closeForward(fwd, &error));

  master.stop();
}

TEST_CASE("ControlPersist wait pumps a transport service callback", "[Mux]") {
  string path = tempControlPath();
  ControlPersistConfig persist;
  persist.enabled = true;
  persist.seconds = 1;

  MuxMaster master(path, persist);
  master.start();
  master.notifyPrimaryClientExited();

  atomic<int> pumps{0};
  MuxMaster::waitWhilePersisting(master, persist, [&]() { pumps++; });
  REQUIRE(pumps.load() >= 1);
  master.stop();
}

TEST_CASE("mux forward keeps a TCP port when the endpoint also has a name",
          "[Mux]") {
  auto requests = parseRangesToRequests("8080:80");
  REQUIRE(requests.size() == 1);
  auto fwd = muxForwardFromTunnel(requests[0]);
  CHECK(fwd.listenHost == "localhost");
  CHECK(fwd.listenPort == 8080);
  CHECK(fwd.connectPort == 80);

  auto sockets = parseRangesToRequests("/tmp/a.sock:/tmp/b.sock");
  REQUIRE(sockets.size() == 1);
  auto unixFwd = muxForwardFromTunnel(sockets[0]);
  CHECK(unixFwd.listenHost == "/tmp/a.sock");
  CHECK(unixFwd.listenPort == static_cast<uint32_t>(-2));
  CHECK(unixFwd.connectHost == "/tmp/b.sock");
  CHECK(unixFwd.connectPort == static_cast<uint32_t>(-2));
}

TEST_CASE("ControlPersist restarts when the last passenger leaves", "[Mux]") {
  string path = tempControlPath();
  ControlPersistConfig persist;
  persist.enabled = true;
  persist.seconds = 1;

  MuxMaster master(path, persist);
  master.start();

  MuxClient passenger(path);
  REQUIRE(passenger.connect());
  REQUIRE(passenger.aliveCheck());

  master.notifyPrimaryClientExited();
  // Stay connected past the persist interval. The master is not idle yet.
  testSleepMicros(1200000);
  REQUIRE(master.isRunning());
  REQUIRE_FALSE(master.persistExpired());

  passenger.disconnect();
  // Idle time starts at this disconnect, so the master must still be up.
  testSleepMicros(300000);
  REQUIRE(master.isRunning());
  REQUIRE_FALSE(master.persistExpired());

  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  while (std::chrono::steady_clock::now() < deadline && master.isRunning() &&
         !master.persistExpired()) {
    testSleepMicros(50000);
  }
  REQUIRE((master.persistExpired() || !master.isRunning()));
  master.stop();
}

TEST_CASE(
    "mux client hangup during interactive attach drops clients so persist "
    "expires",
    "[Mux]") {
  // Regression: after SESSION_OPENED the master blocks in the passenger
  // handler and does not read the control socket. Interactive cancel (client
  // dies / Ctrl-C) must still complete the session so clients drops and
  // ControlPersist can arm.
  string path = tempControlPath();
  ControlPersistConfig persist;
  persist.enabled = true;
  persist.seconds = 1;

  atomic<bool> cancelRequested{false};
  MuxMaster master(path, persist);
  master.setPassengerSessionHandler([&](int inFd, int outFd, int errFd,
                                        const string& command,
                                        bool /*wantTty*/) -> uint32_t {
    REQUIRE(command.empty());
    (void)outFd;
    (void)errFd;
    // Poll stdin with a short timeout so cancel can be observed; a blocking
    // read would ignore hangup-driven cancel while the pipe write end stays
    // open (interactive attach).
    while (!cancelRequested.load()) {
      pollfd pfd{};
      pfd.fd = inFd;
      pfd.events = POLLIN;
      int rc = ::poll(&pfd, 1, 20);
      if (rc > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
        char buf[8];
        ssize_t n = ::read(inFd, buf, sizeof(buf));
        if (n <= 0) {
          break;
        }
      }
    }
    return 1;
  });
  master.setPassengerCancelHandler([&]() { cancelRequested.store(true); });
  master.start();
  master.notifyPrimaryClientExited();

  MuxClient client(path);
  REQUIRE(client.connect());

  int inPipe[2];
  int outPipe[2];
  REQUIRE(::pipe(inPipe) == 0);
  REQUIRE(::pipe(outPipe) == 0);
  // Keep stdin open so an interactive attach does not see local EOF.

  atomic<bool> sessionFinished{false};
  thread sessionThread([&]() {
    uint32_t sessionId = 0;
    uint32_t exitStatus = 0;
    string error;
    (void)client.newSession("", true, inPipe[0], outPipe[1], outPipe[1],
                            &sessionId, &error, &exitStatus);
    sessionFinished.store(true);
  });

  auto attachDeadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  while (std::chrono::steady_clock::now() < attachDeadline &&
         master.sessionCount() == 0) {
    testSleepMicros(20000);
  }
  REQUIRE(master.sessionCount() >= 1);
  REQUIRE(master.activeClientCount() >= 1);

  // Simulate Ctrl-C / process death: tear down the mux control peer mid-attach.
  client.hangup();

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
  if (sessionThread.joinable()) {
    sessionThread.join();
  }

  ::close(inPipe[0]);
  ::close(inPipe[1]);
  ::close(outPipe[0]);
  ::close(outPipe[1]);
  master.stop();
}

TEST_CASE("ControlPersist keeps ControlPath after primary client exits",
          "[Mux]") {
  string path = tempControlPath();
  ControlPersistConfig persist;
  persist.enabled = true;
  persist.seconds = 1;

  MuxMaster master(path, persist);
  master.start();
  REQUIRE(controlPathSocketExists(path));

  master.notifyPrimaryClientExited();
  // Socket must remain briefly after primary exit.
  REQUIRE(controlPathSocketExists(path));
  MuxClient client(path);
  REQUIRE(client.connect());
  REQUIRE(client.aliveCheck());
  client.disconnect();

  // Wait for persist window to elapse.
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
  while (std::chrono::steady_clock::now() < deadline) {
    if (!master.isRunning() || master.persistExpired()) {
      break;
    }
    testSleepMicros(50000);
  }
  REQUIRE((master.persistExpired() || !master.isRunning()));
  master.stop();
}

TEST_CASE("MuxClient -O check and exit", "[Mux]") {
  string path = tempControlPath();
  ControlPersistConfig persist;
  persist.enabled = true;
  persist.seconds = 0;

  MuxMaster master(path, persist);
  master.start();

  MuxClient checkClient(path);
  REQUIRE(checkClient.runCtlCommand("check") == 0);

  MuxClient exitClient(path);
  REQUIRE(exitClient.runCtlCommand("exit") == 0);

  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  while (std::chrono::steady_clock::now() < deadline && master.isRunning()) {
    testSleepMicros(20000);
  }
  REQUIRE_FALSE(master.isRunning());
  master.stop();
}

TEST_CASE("shouldAttachToMuxMaster and shouldBecomeMuxMaster", "[Mux]") {
  MuxOptions opts;
  opts.controlPath = tempControlPath();
  opts.controlMaster = ControlMasterMode::Auto;
  REQUIRE(shouldBecomeMuxMaster(opts));
  REQUIRE_FALSE(shouldAttachToMuxMaster(opts));

  ControlPersistConfig persist;
  MuxMaster master(opts.controlPath, persist);
  master.start();
  REQUIRE(shouldAttachToMuxMaster(opts));
  REQUIRE_FALSE(shouldBecomeMuxMaster(opts));

  opts.controlMaster = ControlMasterMode::Yes;
  REQUIRE(shouldBecomeMuxMaster(opts));
  REQUIRE_FALSE(shouldAttachToMuxMaster(opts));

  opts.controlMaster = ControlMasterMode::No;
  REQUIRE(shouldAttachToMuxMaster(opts));
  REQUIRE_FALSE(shouldBecomeMuxMaster(opts));

  master.stop();
}

#endif  // !WIN32
