#include "MuxClient.hpp"
#include "MuxMaster.hpp"
#include "MuxProtocol.hpp"
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
  REQUIRE(parsed.remainingArgs.size() == 2);
  REQUIRE(parsed.remainingArgs[1] == "user@host");

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

  uint32_t sessionId = 0;
  REQUIRE(client.newSession("true", false, -1, -1, -1, &sessionId, &error));
  REQUIRE(sessionId >= 1);
  REQUIRE(master.sessionCount() >= 1);

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
