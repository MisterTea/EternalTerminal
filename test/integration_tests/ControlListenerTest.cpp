#include "ControlConsole.hpp"
#include "ControlListener.hpp"
#include "ControlProtocol.hpp"
#include "ETerminal.pb.h"
#include "TestHeaders.hpp"

using namespace et;

namespace {

string makeTempSocketPath() {
  // Keep the path short: AF_UNIX sun_path is ~104 bytes.
  char tmpl[] = "/tmp/etctl_test_XXXXXX";
  char* dir = mkdtemp(tmpl);
  REQUIRE(dir != nullptr);
  return string(dir) + "/c.sock";
}

int connectTo(const string& path) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  REQUIRE(::connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0);
  return fd;
}

// One request, one response over a fresh connection (matches the daemon's
// one-shot-per-connection model).
std::pair<uint8_t, string> rpc(const string& path, uint8_t opcode,
                               const string& payload) {
  int fd = connectTo(path);
  control_proto::writeFrame(fd, opcode, payload);
  uint8_t respOp = 0;
  string respPayload;
  bool ok = control_proto::readFrame(fd, &respOp, &respPayload);
  ::close(fd);
  REQUIRE(ok);
  return {respOp, respPayload};
}

}  // namespace

TEST_CASE("ControlListenerEndToEnd", "[ControlListener]") {
  auto console = std::make_shared<ControlConsole>();
  string path = makeTempSocketPath();
  std::atomic<bool> killed{false};
  ControlListener listener(console, path, [&]() { killed = true; }, nullptr,
                           "tester@example.host");
  listener.start();

  SECTION("write injects input reaching the console fd") {
    auto [op, payload] = rpc(path, CTL_WRITE, "echo hi\n");
    REQUIRE(op == CTL_OK);
    // run() would read injected input off getFd(); confirm it arrived.
    char buf[64];
    ssize_t n = ::read(console->getFd(), buf, sizeof(buf));
    REQUIRE(n == 8);
    REQUIRE(string(buf, n) == "echo hi\n");
  }

  SECTION("read returns server output non-destructively, by cursor") {
    console->write("line one\n");
    auto [op, payload] = rpc(path, CTL_READ, control_proto::encodeCursor(0));
    REQUIRE(op == CTL_READ_RESP);
    ScrollbackRead r = control_proto::decodeReadResp(payload);
    REQUIRE(r.data == "line one\n");
    REQUIRE(r.truncated == false);

    // Re-read from the returned cursor: only new bytes.
    console->write("line two\n");
    auto [op2, payload2] =
        rpc(path, CTL_READ, control_proto::encodeCursor(r.nextCursor));
    ScrollbackRead r2 = control_proto::decodeReadResp(payload2);
    REQUIRE(r2.data == "line two\n");
  }

  SECTION("sniff returns the tagged input+output exchange") {
    // Inject input (via the socket) and produce output (via the console).
    rpc(path, CTL_WRITE, "ls\r");
    // Drain the injected input off the pipe so it doesn't linger.
    char buf[16];
    ::read(console->getFd(), buf, sizeof(buf));
    console->write("file1 file2\n");

    auto [op, payload] = rpc(path, CTL_SNIFF, control_proto::encodeCursor(0));
    REQUIRE(op == CTL_SNIFF_RESP);
    TranscriptRead tr = control_proto::decodeTranscriptResp(payload);
    REQUIRE(tr.records.size() == 2);
    REQUIRE(tr.records[0].dir == '>');
    REQUIRE(tr.records[0].bytes == "ls\r");
    REQUIRE(tr.records[1].dir == '<');
    REQUIRE(tr.records[1].bytes == "file1 file2\n");
    REQUIRE(tr.truncated == false);
  }

  SECTION("secret write reaches the shell but is redacted from sniff") {
    auto [op, payload] = rpc(path, CTL_WRITE_SECRET, "hunter2\n");
    REQUIRE(op == CTL_OK);
    // The shell still receives the real bytes.
    char buf[32];
    ssize_t n = ::read(console->getFd(), buf, sizeof(buf));
    REQUIRE(string(buf, n > 0 ? (size_t)n : 0) == "hunter2\n");
    // But sniff shows only the placeholder, never the plaintext.
    auto [pop, ppayload] = rpc(path, CTL_SNIFF, control_proto::encodeCursor(0));
    REQUIRE(pop == CTL_SNIFF_RESP);
    TranscriptRead tr = control_proto::decodeTranscriptResp(ppayload);
    REQUIRE(tr.records.size() == 1);
    REQUIRE(tr.records[0].dir == '>');
    REQUIRE(tr.records[0].bytes == "<secret>");
  }

  SECTION("resize updates the console terminal size") {
    TerminalInfo ti;
    ti.set_row(40);
    ti.set_column(120);
    string payload;
    ti.SerializeToString(&payload);
    auto [op, resp] = rpc(path, CTL_RESIZE, payload);
    REQUIRE(op == CTL_OK);
    TerminalInfo got = console->getTerminalInfo();
    REQUIRE(got.row() == 40);
    REQUIRE(got.column() == 120);
  }

  SECTION("info reports liveness, host, and size") {
    auto [op, payload] = rpc(path, CTL_INFO, "");
    REQUIRE(op == CTL_INFO_RESP);
    REQUIRE(payload.find("alive=1") != string::npos);
    REQUIRE(payload.find("host=tester@example.host") != string::npos);
    REQUIRE(payload.find("rows=24") != string::npos);
    REQUIRE(payload.find("cols=132") != string::npos);
  }

  SECTION("kill acknowledges and fires the callback") {
    auto [op, payload] = rpc(path, CTL_KILL, "");
    REQUIRE(op == CTL_OK);
    // Give the callback a beat to run on the listener thread.
    for (int i = 0; i < 50 && !killed; i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(killed.load());
  }

  listener.shutdown();
  // Socket is unlinked on shutdown.
  REQUIRE(::access(path.c_str(), F_OK) != 0);
}
