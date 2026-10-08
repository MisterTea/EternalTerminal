#ifndef WIN32
#include <poll.h>
#endif

#include <future>

#include "PipeSocketHandler.hpp"
#include "PortForwardHandler.hpp"
#include "RawSocketUtils.hpp"
#include "SshAgentSessionBind.hpp"
#include "TcpSocketHandler.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {
constexpr char SSH_AGENT_FAILURE = 5;
constexpr char SSH_AGENT_SUCCESS = 6;
constexpr char SSH_AGENTC_REQUEST_IDENTITIES = 11;
constexpr char SSH_AGENT_IDENTITIES_ANSWER = 12;

string u32(uint32_t v) {
  return {char(v >> 24), char(v >> 16), char(v >> 8), char(v)};
}

string sshString(const string& s) { return u32(s.size()) + s; }

string sessionBind(const string& hostKey, bool forwarding) {
  return string(1, char(27)) + sshString("session-bind@openssh.com") +
         sshString(hostKey) + sshString("session-id-" + hostKey) +
         sshString("signature-" + hostKey) + string(1, char(forwarding));
}

string frame(const string& body) { return u32(body.size()) + body; }

PortForwardDestinationRequest agentDestination(const string& path) {
  PortForwardDestinationRequest request;
  request.mutable_destination()->set_name(path);
  request.set_fd(7);
  return request;
}

/**
 * SocketHandler double for one destination socket (fd 42): reads come from
 * `reads` while it is non-empty; writes are recorded, or fail with EPIPE.
 */
class ScriptedSocketHandler : public SocketHandler {
 public:
  bool hasData(int) override { return !reads.empty(); }
  ssize_t read(int, void* buf, size_t count) override {
    string chunk = reads.front().substr(0, count);
    reads.pop_front();
    memcpy(buf, chunk.data(), chunk.size());
    return chunk.size();
  }
  ssize_t write(int, const void* buf, size_t count) override {
    if (failWrites) {
      SetErrno(EPIPE);
      return -1;
    }
    written.append((const char*)buf, count);
    return count;
  }
  int connect(const SocketEndpoint&) override { return 42; }
  set<int> listen(const SocketEndpoint&) override { return {}; }
  set<int> getEndpointFds(const SocketEndpoint&) override { return {}; }
  int accept(int) override { return -1; }
  void stopListening(const SocketEndpoint&) override {}
  void close(int fd) override { closed.push_back(fd); }
  vector<int> getActiveSockets() override { return {}; }

  std::deque<string> reads;
  string written;
  bool failWrites = false;
  vector<int> closed;
};

// Agent messages here are a few bytes, so one send() moves a whole frame.
bool sendFrame(int fd, const string& body) {
  string framed = frame(body);
#ifdef WIN32
  return ::send(fd, framed.data(), static_cast<int>(framed.size()), 0) ==
         static_cast<int>(framed.size());
#else
  return ::send(fd, framed.data(), framed.size(), MSG_NOSIGNAL) ==
         ssize_t(framed.size());
#endif
}

// Test sockets get a receive timeout, so a missing peer cannot hang a test.
void setReceiveTimeout(int fd) {
#ifdef WIN32
  DWORD timeout = 10000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
  timeval timeout = {10, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}

bool readExact(int fd, char* buf, size_t count) {
  size_t pos = 0;
  ssize_t n = 1;
  while (pos < count && n > 0) {
    n = RawSocketUtils::readSome(fd, buf + pos, count - pos);
    pos += std::max<ssize_t>(n, 0);
  }
  return pos == count;
}

bool readFrame(int fd, string* body) {
  char length[4] = {};
  bool ok = readExact(fd, length, 4);
  body->assign(ok ? (uint32_t(uint8_t(length[0])) << 24) |
                        (uint32_t(uint8_t(length[1])) << 16) |
                        (uint32_t(uint8_t(length[2])) << 8) | uint8_t(length[3])
                  : 0,
               '\0');
  return ok && readExact(fd, &(*body)[0], body->size());
}

/** Listening Unix socket in a fresh temporary directory, removed on exit. */
struct UnixListener {
  UnixListener() {
    dir = et::test::makeTempDir("et_fake_agent");
    path = dir + "/agent.sock";
    if (path.size() >= sizeof(sockaddr_un{}.sun_path)) {
      et::test::removeTempDir(dir);
      dir.clear();
      path = "et_agent_" + genRandomAlphaNum(8) + ".sock";
    }
    RawSocketUtils::unlinkSocketPath(path);
    fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    REQUIRE(::bind(fd, (sockaddr*)&addr, unixAddressLength(addr)) == 0);
    REQUIRE(::listen(fd, 4) == 0);
  }

  ~UnixListener() {
    RawSocketUtils::closeSocket(fd);
    RawSocketUtils::unlinkSocketPath(path);
    if (!dir.empty()) {
      et::test::removeTempDir(dir);
    }
  }

  string dir;
  string path;
  int fd = -1;
};

/**
 * Minimal ssh-agent: serves connections one at a time, recording every
 * request and answering with `reply(request)`.
 */
class FakeAgent {
 public:
  explicit FakeAgent(function<string(const string&)> _reply)
      : path(listener.path), reply(std::move(_reply)) {
    worker = std::thread([this]() { serve(); });
  }

  ~FakeAgent() {
    stopping = true;
    worker.join();
  }

  vector<string> received() {
    lock_guard<std::mutex> guard(mutex);
    return requests;
  }

 private:
  UnixListener listener;

 public:
  const string path;

 private:
  void serve() {
    while (waitReadable(listener.fd)) {
      int fd = ::accept(listener.fd, nullptr, nullptr);
      if (fd < 0) {
        continue;
      }
      setReceiveTimeout(fd);
      string request;
      while (waitReadable(fd) && readFrame(fd, &request)) {
        {
          lock_guard<std::mutex> guard(mutex);
          requests.push_back(request);
        }
        sendFrame(fd, reply(request));
      }
      RawSocketUtils::closeSocket(fd);
    }
  }

  // Waits until fd is readable, giving up promptly once the test is done.
  bool waitReadable(int fd) {
    while (!stopping) {
#ifdef WIN32
      WSAPOLLFD pfd = {};
      pfd.fd = static_cast<SOCKET>(fd);
      pfd.events = POLLRDNORM | POLLRDBAND;
      if (::WSAPoll(&pfd, 1, 50) > 0) {
        return true;
      }
#else
      pollfd pfd = {fd, POLLIN, 0};
      if (::poll(&pfd, 1, 50) > 0) {
        return true;
      }
#endif
    }
    return false;
  }

  function<string(const string&)> reply;
  std::atomic<bool> stopping{false};
  std::mutex mutex;
  vector<string> requests;
  std::thread worker;
};

string successReply(const string&) { return string(1, SSH_AGENT_SUCCESS); }

void forwardToAgent(PortForwardHandler* handler, int socketId,
                    const string& data) {
  PortForwardData pwd;
  pwd.set_sourcetodestination(true);
  pwd.set_socketid(socketId);
  pwd.set_buffer(data);
  handler->handlePacket(Packet(uint8_t(TerminalPacketType::PORT_FORWARD_DATA),
                               protoToString(pwd)),
                        nullptr);
}

// What the forwarded client receives from the agent once at least `minBytes`
// arrived, or when `wait` runs out.
string agentOutput(PortForwardHandler* handler, size_t minBytes,
                   std::chrono::milliseconds wait = std::chrono::seconds(10)) {
  const auto deadline = std::chrono::steady_clock::now() + wait;
  string output;
  while (output.size() < minBytes &&
         std::chrono::steady_clock::now() < deadline) {
    vector<PortForwardDestinationRequest> requests;
    vector<PortForwardData> data;
    handler->update(&requests, &data);
    for (const auto& pwd : data) {
      REQUIRE_FALSE(pwd.has_error());
      REQUIRE_FALSE(pwd.has_closed());
      output += pwd.buffer();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return output;
}
}  // namespace

TEST_CASE("AgentSessionBindReplies drops only the bind replies",
          "[SshAgentSessionBind]") {
  string bindReplies =
      frame(string(1, SSH_AGENT_SUCCESS)) + frame(string(1, SSH_AGENT_FAILURE));
  string clientReply = frame(string(1, SSH_AGENT_IDENTITIES_ANSWER) + u32(0));

  SECTION("Replies split across reads") {
    AgentSessionBindReplies replies(2, "/tmp/agent.sock");
    string stream = bindReplies + clientReply;
    string forwarded;
    for (char byte : stream) {
      string out;
      REQUIRE(replies.consume(string(1, byte), &out));
      forwarded += out;
    }
    CHECK(forwarded == clientReply);
  }

  SECTION("Client data in the same read as the last bind reply") {
    AgentSessionBindReplies replies(2, "/tmp/agent.sock");
    string out;
    REQUIRE(replies.consume(bindReplies.substr(0, 3), &out));
    CHECK(out.empty());
    REQUIRE(replies.consume(bindReplies.substr(3) + clientReply, &out));
    CHECK(out == clientReply);
    REQUIRE(replies.consume("more", &out));
    CHECK(out == "more");
  }

  SECTION("Malformed reply length") {
    AgentSessionBindReplies replies(1, "/tmp/agent.sock");
    string out;
    CHECK_FALSE(replies.consume(u32(0) + "x", &out));
  }
}

TEST_CASE("Forwarded agent connections are bound before data flows",
          "[SshAgentSessionBind]") {
  vector<string> binds = {sessionBind("jump-key", true),
                          sessionBind("dest-key", true)};
  auto pipeHandler = make_shared<PipeSocketHandler>();
  PortForwardHandler handler(make_shared<TcpSocketHandler>(), pipeHandler);

  SECTION("Bindings precede forwarded requests and their replies are hidden") {
    FakeAgent agent(successReply);
    handler.setSshAgentSessionBinds(agent.path, binds);
    auto response = handler.createDestination(agentDestination(agent.path));
    REQUIRE_FALSE(response.has_error());

    forwardToAgent(&handler, response.socketid(),
                   frame(string(1, SSH_AGENTC_REQUEST_IDENTITIES)));
    string expected = frame(string(1, SSH_AGENT_SUCCESS));
    CHECK(agentOutput(&handler, expected.size()) == expected);

    auto received = agent.received();
    REQUIRE(received.size() == 3);
    CHECK(received[0] == binds[0]);
    CHECK(received[1] == binds[1]);
    CHECK(received[2] == string(1, SSH_AGENTC_REQUEST_IDENTITIES));
  }

  SECTION("Agents without the extension are still forwarded") {
    FakeAgent agent([](const string&) { return string(1, SSH_AGENT_FAILURE); });
    handler.setSshAgentSessionBinds(agent.path, binds);
    auto response = handler.createDestination(agentDestination(agent.path));
    REQUIRE_FALSE(response.has_error());

    forwardToAgent(&handler, response.socketid(),
                   frame(string(1, SSH_AGENTC_REQUEST_IDENTITIES)));
    string expected = frame(string(1, SSH_AGENT_FAILURE));
    CHECK(agentOutput(&handler, expected.size()) == expected);
  }

  SECTION("A slow agent does not block the client") {
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    FakeAgent agent([released](const string&) {
      released.wait();
      return string(1, SSH_AGENT_SUCCESS);
    });
    // Declared after the agent: unblocks it even when a REQUIRE below throws,
    // before ~FakeAgent joins its thread.
    std::unique_ptr<std::promise<void>, void (*)(std::promise<void>*)>
        releaseAgent(&release, [](std::promise<void>* p) { p->set_value(); });
    handler.setSshAgentSessionBinds(agent.path, binds);

    auto start = std::chrono::steady_clock::now();
    auto response = handler.createDestination(agentDestination(agent.path));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    REQUIRE_FALSE(response.has_error());

    forwardToAgent(&handler, response.socketid(),
                   frame(string(1, SSH_AGENTC_REQUEST_IDENTITIES)));
    // The agent cannot answer before it is released.
    CHECK(agentOutput(&handler, 1, std::chrono::milliseconds(500)).empty());
    releaseAgent.reset();
    string expected = frame(string(1, SSH_AGENT_SUCCESS));
    CHECK(agentOutput(&handler, expected.size()) == expected);
  }

  SECTION("Other unix sockets are not bound") {
    FakeAgent agent(successReply);
    FakeAgent otherSocket(successReply);
    handler.setSshAgentSessionBinds(agent.path, binds);
    auto response =
        handler.createDestination(agentDestination(otherSocket.path));
    REQUIRE_FALSE(response.has_error());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(otherSocket.received().empty());
  }
}

TEST_CASE("Agent bindings that cannot be sent fail the forwarded connection",
          "[SshAgentSessionBind]") {
  auto pipeHandler = make_shared<ScriptedSocketHandler>();
  pipeHandler->failWrites = true;
  PortForwardHandler handler(make_shared<ScriptedSocketHandler>(), pipeHandler);
  handler.setSshAgentSessionBinds("/tmp/agent.sock",
                                  {sessionBind("dest-key", true)});

  auto response =
      handler.createDestination(agentDestination("/tmp/agent.sock"));

  CHECK(response.error() == "could not send ssh agent session binding");
  CHECK(pipeHandler->closed == vector<int>{42});
}

TEST_CASE("A malformed bind reply closes the forwarded connection",
          "[SshAgentSessionBind]") {
  auto pipeHandler = make_shared<ScriptedSocketHandler>();
  PortForwardHandler handler(make_shared<ScriptedSocketHandler>(), pipeHandler);
  string bind = sessionBind("dest-key", true);
  handler.setSshAgentSessionBinds("/tmp/agent.sock", {bind});
  auto response =
      handler.createDestination(agentDestination("/tmp/agent.sock"));
  REQUIRE_FALSE(response.has_error());
  CHECK(pipeHandler->written == frame(bind));

  pipeHandler->reads.push_back(u32(0) + "x");
  vector<PortForwardDestinationRequest> requests;
  vector<PortForwardData> data;
  handler.update(&requests, &data);

  REQUIRE(data.size() == 1);
  CHECK(data[0].has_error());
  CHECK_FALSE(data[0].has_buffer());
  CHECK(pipeHandler->closed == vector<int>{42});
}

TEST_CASE("Bind replies are not forwarded even when they arrive alone",
          "[SshAgentSessionBind]") {
  auto pipeHandler = make_shared<ScriptedSocketHandler>();
  PortForwardHandler handler(make_shared<ScriptedSocketHandler>(), pipeHandler);
  handler.setSshAgentSessionBinds("/tmp/agent.sock",
                                  {sessionBind("dest-key", true)});
  REQUIRE_FALSE(handler.createDestination(agentDestination("/tmp/agent.sock"))
                    .has_error());
  vector<PortForwardDestinationRequest> requests;
  vector<PortForwardData> data;

  pipeHandler->reads.push_back(frame(string(1, SSH_AGENT_SUCCESS)));
  handler.update(&requests, &data);
  CHECK(data.empty());

  string clientReply = frame(string(1, SSH_AGENT_IDENTITIES_ANSWER) + u32(0));
  pipeHandler->reads.push_back(clientReply);
  handler.update(&requests, &data);
  REQUIRE(data.size() == 1);
  CHECK(data[0].buffer() == clientReply);
}
