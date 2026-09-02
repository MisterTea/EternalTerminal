#include <fcntl.h>

#include <atomic>
#include <thread>

#include "HtmClient.hpp"
#include "HtmServer.hpp"
#include "HtmTestHelpers.hpp"
#include "IpcPairClient.hpp"
#include "TestHeaders.hpp"

using namespace et;
using namespace et::htmtest;

#ifndef WIN32
namespace {
class HtmServerHarness {
 public:
  HtmServerHarness()
      : handler(std::make_shared<PipeSocketHandler>()),
        endpoint(endpointFor(ipc.path)),
        server(handler, endpoint) {
    skipIfThreadSanitizer();
    runner = std::thread([this]() { server.run(); });
    client.reset(new IpcPairClient(handler, endpoint));
    REQUIRE(waitUntil([&]() { return server.getEndpointFd() >= 0; }, 5000));
    REQUIRE(waitUntil(
        [&]() {
          pump();
          return hasLinePrefix(lines, "%session-changed");
        },
        8000));
  }

  ~HtmServerHarness() { stop(); }

  void stop() {
    server.requestStop();
    if (client && client->getEndpointFd() >= 0) {
      try {
        sendLine(handler, client->getEndpointFd(), "kill-server");
      } catch (...) {
      }
    }
    if (runner.joinable()) {
      runner.join();
    }
  }

  void pump() {
    incoming += readUntil(handler, client->getEndpointFd(), 1, 50);
    lines = splitLines(incoming);
  }

  string command(const string& line, int timeoutMs = 8000) {
    size_t before = incoming.size();
    sendLine(handler, client->getEndpointFd(), line);
    REQUIRE(waitUntil(
        [&]() {
          pump();
          return incoming.find("%end ", before) != string::npos ||
                 incoming.find("%error ", before) != string::npos;
        },
        timeoutMs));
    return incoming.substr(before);
  }

  UniqueIpcPath ipc;
  shared_ptr<PipeSocketHandler> handler;
  SocketEndpoint endpoint;
  HtmServer server;
  std::thread runner;
  unique_ptr<IpcPairClient> client;
  string incoming;
  vector<string> lines;
};
}  // namespace
#endif

TEST_CASE("HtmServer getPipeName includes the uid", "[Htm][HtmServer]") {
  string name = HtmServer::getPipeName();
  REQUIRE(name.find("htm.") != string::npos);
  REQUIRE(name.find(".ipc") != string::npos);
}

#ifndef WIN32
TEST_CASE("HtmServer attach handshake is server-originated",
          "[Htm][HtmServer]") {
  HtmServerHarness h;
  bool sawBegin0 = false;
  bool sawEnd0 = false;
  for (const string& line : h.lines) {
    if (line.compare(0, 7, "%begin ") == 0 && line.size() >= 2 &&
        line.compare(line.size() - 2, 2, " 0") == 0) {
      sawBegin0 = true;
    }
    if (line.compare(0, 5, "%end ") == 0 && line.size() >= 2 &&
        line.compare(line.size() - 2, 2, " 0") == 0) {
      sawEnd0 = true;
    }
  }
  REQUIRE(sawBegin0);
  REQUIRE(sawEnd0);
  auto beginAt = h.incoming.find("%begin ");
  auto endAt = h.incoming.find("%end ");
  REQUIRE(beginAt != string::npos);
  REQUIRE(endAt != string::npos);
  REQUIRE(beginAt < endAt);
  REQUIRE(h.incoming.find("%session-changed") > endAt);
}

TEST_CASE("HtmServer accepts CR-terminated control commands",
          "[Htm][HtmServer]") {
  HtmServerHarness h;
  string cmd = string("display-message -p '#{version}'") + '\r';
  size_t before = h.incoming.size();
  h.handler->writeAllOrThrow(h.client->getEndpointFd(), cmd.data(),
                             static_cast<int>(cmd.size()), false);
  REQUIRE(waitUntil(
      [&]() {
        h.pump();
        return h.incoming.find(HTM_TMUX_VERSION, before) != string::npos;
      },
      8000));
}

TEST_CASE("HtmServer replies once per command in a semicolon list",
          "[Htm][HtmServer]") {
  HtmServerHarness h;
  size_t before = h.incoming.size();
  sendLine(h.handler, h.client->getEndpointFd(),
           "display-message -p '#{version}'; list-windows -F '#{window_id}'");
  REQUIRE(waitUntil(
      [&]() {
        h.pump();
        string chunk = h.incoming.substr(before);
        int ends = 0;
        for (size_t p = 0; (p = chunk.find("%end ", p)) != string::npos;
             p += 5) {
          ends++;
        }
        return ends >= 2 && chunk.find(HTM_TMUX_VERSION) != string::npos &&
               chunk.find("@") != string::npos;
      },
      8000));
}

TEST_CASE("HtmServer recovers and speaks control mode", "[Htm][HtmServer]") {
  HtmServerHarness h;
  string ver = h.command("display-message -p '#{version}'");
  REQUIRE(ver.find(HTM_TMUX_VERSION) != string::npos);

  string windows = h.command("list-windows -F '#{window_id} #{window_layout}'");
  REQUIRE(windows.find("@") != string::npos);

  h.command("new-window -P -F '#{window_id}'");
  REQUIRE(waitUntil(
      [&]() {
        h.pump();
        return hasLinePrefix(h.lines, "%window-add");
      },
      5000));

  h.command("split-window -h");
  REQUIRE(waitUntil(
      [&]() {
        h.pump();
        return h.incoming.find("%layout-change") != string::npos;
      },
      5000));

  h.command("refresh-client -C 80x24");
  h.command("send-keys -t %0 printf Space CC_MARK Enter");
  REQUIRE(waitUntil(
      [&]() {
        h.pump();
        return h.incoming.find("CC_MARK") != string::npos;
      },
      8000));
}

TEST_CASE("HtmServer layout-change matches tmux 3.x fields",
          "[Htm][HtmServer]") {
  auto fields = [](const string& line) {
    vector<string> out;
    string cur;
    for (char c : line) {
      if (c == ' ') {
        out.push_back(cur);
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    out.push_back(cur);
    return out;
  };
  auto isTmux3 = [&](const string& line) {
    auto f = fields(line);
    return f.size() >= 5 && f[0] == "%layout-change" && !f[1].empty() &&
           f[1][0] == '@' && f[2].find('x') != string::npos &&
           f[3].find('x') != string::npos;
  };

  HtmServerHarness h;
  REQUIRE(waitUntil(
      [&]() {
        h.pump();
        for (const string& line : h.lines) {
          if (isTmux3(line) && line.find(" *") != string::npos) {
            return true;
          }
        }
        return false;
      },
      5000));

  h.command("split-window -h");
  REQUIRE(waitUntil(
      [&]() {
        h.pump();
        int n = 0;
        for (const string& line : h.lines) {
          if (isTmux3(line)) {
            n++;
          }
        }
        return n >= 2;
      },
      5000));

  h.command("resize-pane -Z");
  REQUIRE(waitUntil(
      [&]() {
        h.pump();
        for (const string& line : h.lines) {
          if (isTmux3(line) && line.find(" *Z") != string::npos) {
            return true;
          }
        }
        return false;
      },
      5000));
}

TEST_CASE("HtmServer streams concurrent output from windows and splits",
          "[Htm][HtmServer]") {
  HtmServerHarness h;
  h.command("new-window");
  h.command("split-window -v");
  string panes = h.command("list-panes -F '#{pane_id}'");
  REQUIRE(panes.find("%") != string::npos);
}

TEST_CASE("HtmServer stops when the last pane is closed", "[Htm][HtmServer]") {
  HtmServerHarness h;
  h.command("kill-pane -t %0");
  REQUIRE(waitUntil(
      [&]() {
        h.pump();
        return h.incoming.find("%exit") != string::npos ||
               h.server.getEndpointFd() < 0;
      },
      8000));
}

TEST_CASE("HtmServer survives an abrupt client hangup", "[Htm][HtmServer]") {
  HtmServerHarness h;
  h.client->closeEndpoint();
  REQUIRE(waitUntil([&]() { return h.server.getEndpointFd() < 0; }, 5000));
}

TEST_CASE("HtmServer detach on empty line", "[Htm][HtmServer]") {
  HtmServerHarness h;
  sendLine(h.handler, h.client->getEndpointFd(), "");
  REQUIRE(waitUntil(
      [&]() {
        h.pump();
        return h.incoming.find("%exit") != string::npos;
      },
      5000));
}

TEST_CASE("HtmServer titles zoom paste and sessions", "[Htm][HtmServer]") {
  HtmServerHarness h;
  h.command("rename-window titlewin");
  REQUIRE(waitUntil(
      [&]() {
        h.pump();
        return h.incoming.find("%window-renamed") != string::npos;
      },
      5000));
  h.command("select-pane -T panetitle");
  h.command("resize-pane -Z");
  h.command("set-buffer -b buffer0 hello");
  REQUIRE(waitUntil(
      [&]() {
        h.pump();
        return h.incoming.find("%paste-buffer-changed") != string::npos;
      },
      5000));
  string buf = h.command("show-buffer -b buffer0");
  REQUIRE(buf.find("hello") != string::npos);
  h.command("new-session -s extra");
  string sessions = h.command("list-sessions -F '#{session_name}'");
  REQUIRE(sessions.find("extra") != string::npos);
  h.command("refresh-client -f pause-after=0");
}

TEST_CASE("HtmServer swap-pane set-option break-pane and unlink-window",
          "[Htm][HtmServer]") {
  HtmServerHarness h;
  h.command("split-window -h");
  string swapped = h.command("swap-pane -s %0 -t %1");
  REQUIRE(swapped.find("%error") == string::npos);
  REQUIRE(swapped.find("%end") != string::npos);

  string setOut = h.command("set -t $1 @iterm2_id guid-xyz");
  REQUIRE(setOut.find("%error") == string::npos);
  string shown = h.command("show -v -q -t $1 @iterm2_id");
  REQUIRE(shown.find("guid-xyz") != string::npos);
  REQUIRE(shown.find("%error") == string::npos);
  string quiet = h.command("show -v -q -t $1 @missing_option");
  REQUIRE(quiet.find("%error") == string::npos);

  h.command("set -p -t %0 @uservars pane-vars");
  string paneOpt = h.command("show-options -v -q -p -t %0 @uservars");
  REQUIRE(paneOpt.find("pane-vars") != string::npos);

  string broken = h.command("break-pane -P -F #{window_id} -s %1");
  REQUIRE(broken.find("%error") == string::npos);
  REQUIRE(broken.find("@") != string::npos);

  h.command("new-window");
  string unlinked = h.command("unlink-window -k -t @2");
  REQUIRE(unlinked.find("%error") == string::npos);

  string linked = h.command("link-window -s $1:@1 -t $1:+");
  REQUIRE(linked.find("%error") == string::npos);
  string moved = h.command("move-window -s $1:@1 -t $1:+");
  REQUIRE(moved.find("%error") == string::npos);
}

TEST_CASE("HtmServer reconnect recaptures pane contents", "[Htm][HtmServer]") {
  HtmServerHarness h;
  h.command("refresh-client -C 80x24");
  h.command("send-keys -t %0 printf Space RECAP_MARK Enter");
  REQUIRE(waitUntil(
      [&]() {
        h.pump();
        return h.incoming.find("RECAP_MARK") != string::npos;
      },
      8000));
  string first = h.command("capture-pane -p -t %0");
  REQUIRE(first.find("RECAP_MARK") != string::npos);

  h.client->closeEndpoint();
  REQUIRE(waitUntil([&]() { return h.server.getEndpointFd() < 0; }, 5000));
  h.incoming.clear();
  h.lines.clear();
  h.client.reset(new IpcPairClient(h.handler, h.endpoint));
  REQUIRE(waitUntil(
      [&]() {
        h.pump();
        return hasLinePrefix(h.lines, "%session-changed");
      },
      8000));
  string second = h.command("capture-pane -p -t %0");
  REQUIRE(second.find("RECAP_MARK") != string::npos);
}

TEST_CASE("HtmClient forwards stdin and exits when htmd closes",
          "[Htm][HtmClient]") {
  skipIfThreadSanitizer();
  UniqueIpcPath ipc;
  auto handler = std::make_shared<PipeSocketHandler>();
  auto endpoint = endpointFor(ipc.path);
  HtmServer server(handler, endpoint);
  std::thread runner([&]() { server.run(); });
  int stdinPipe[2];
  REQUIRE(pipe(stdinPipe) == 0);
  int stdoutPipe[2];
  REQUIRE(pipe(stdoutPipe) == 0);
  int oldIn = dup(STDIN_FILENO);
  int oldOut = dup(STDOUT_FILENO);
  dup2(stdinPipe[0], STDIN_FILENO);
  dup2(stdoutPipe[1], STDOUT_FILENO);
  close(stdinPipe[0]);
  close(stdoutPipe[1]);
  int outFlags = fcntl(stdoutPipe[0], F_GETFL, 0);
  if (outFlags >= 0) {
    fcntl(stdoutPipe[0], F_SETFL, outFlags | O_NONBLOCK);
  }

  std::atomic<bool> clientDone{false};
  std::thread clientThread([&]() {
    try {
      HtmClient client(handler, endpoint);
      client.run();
    } catch (...) {
    }
    clientDone = true;
  });

  REQUIRE(waitUntil(
      [&]() {
        char buf[256];
        ssize_t n = read(stdoutPipe[0], buf, sizeof(buf));
        if (n > 0) {
          string s(buf, n);
          return s.find("1000p") != string::npos ||
                 s.find("%session-changed") != string::npos;
        }
        return false;
      },
      8000));

  string kill = "kill-server\n";
  REQUIRE(write(stdinPipe[1], kill.data(), kill.size()) ==
          ssize_t(kill.size()));
  REQUIRE(waitUntil([&]() { return clientDone.load(); }, 8000));
  clientThread.join();
  server.requestStop();
  if (runner.joinable()) {
    runner.join();
  }
  dup2(oldIn, STDIN_FILENO);
  dup2(oldOut, STDOUT_FILENO);
  close(oldIn);
  close(oldOut);
  close(stdinPipe[1]);
  close(stdoutPipe[0]);
}
#endif
