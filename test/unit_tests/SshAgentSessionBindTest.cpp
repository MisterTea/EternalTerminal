#ifndef WIN32
#include <poll.h>
#endif

#include <future>

#include "PipeSocketHandler.hpp"
#include "PortForwardHandler.hpp"
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

#ifndef WIN32
// Agent messages here are a few bytes, so one send() moves a whole frame.
bool sendFrame(int fd, const string& body) {
  string framed = frame(body);
  return ::send(fd, framed.data(), framed.size(), MSG_NOSIGNAL) ==
         ssize_t(framed.size());
}

// Test sockets get a receive timeout, so a missing peer cannot hang a test.
void setReceiveTimeout(int fd) {
  timeval timeout = {10, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

bool readExact(int fd, char* buf, size_t count) {
  size_t pos = 0;
  ssize_t n = 1;
  while (pos < count && n > 0) {
    n = ::read(fd, buf + pos, count - pos);
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

int connectUnix(const string& path) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  REQUIRE(::connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0);
  setReceiveTimeout(fd);
  return fd;
}

// Sends `request` to an agent socket and returns the reply body.
string agentRoundTrip(int fd, const string& request) {
  string reply;
  REQUIRE(sendFrame(fd, request));
  REQUIRE(readFrame(fd, &reply));
  return reply;
}

/** Listening Unix socket in a fresh temporary directory, removed on exit. */
struct UnixListener {
  UnixListener() {
    char dirTemplate[] = "/tmp/et_fake_agent_XXXXXX";
    REQUIRE(mkdtemp(dirTemplate) != nullptr);
    dir = dirTemplate;
    path = dir + "/agent.sock";
    fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    REQUIRE(::bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0);
    REQUIRE(::listen(fd, 4) == 0);
  }

  ~UnixListener() {
    ::close(fd);
    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
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
      setReceiveTimeout(fd);
      string request;
      while (waitReadable(fd) && readFrame(fd, &request)) {
        {
          lock_guard<std::mutex> guard(mutex);
          requests.push_back(request);
        }
        sendFrame(fd, reply(request));
      }
      ::close(fd);
    }
  }

  // Waits until fd is readable, giving up promptly once the test is done.
  bool waitReadable(int fd) {
    while (!stopping) {
      pollfd pfd = {fd, POLLIN, 0};
      if (::poll(&pfd, 1, 50) > 0) {
        return true;
      }
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
#endif
}  // namespace

TEST_CASE("forwardedSessionBind marks ssh's own binding as forwarded",
          "[SshAgentSessionBind]") {
  auto forwarded = forwardedSessionBind(sessionBind("host-key", false));
  REQUIRE(forwarded);
  REQUIRE(*forwarded == sessionBind("host-key", true));
}

TEST_CASE("forwardedSessionBind rejects other agent messages",
          "[SshAgentSessionBind]") {
  string bind = sessionBind("host-key", false);
  CHECK_FALSE(forwardedSessionBind(sessionBind("host-key", true)));
  CHECK_FALSE(forwardedSessionBind(string(1, SSH_AGENTC_REQUEST_IDENTITIES)));
  CHECK_FALSE(forwardedSessionBind(""));
  CHECK_FALSE(forwardedSessionBind(bind.substr(0, bind.size() - 1)));
  CHECK_FALSE(forwardedSessionBind(bind + "x"));
  CHECK_FALSE(forwardedSessionBind(string(1, char(27)) + sshString("query") +
                                   sshString("x")));
  CHECK_FALSE(forwardedSessionBind(sessionBind("", false)));
  // Too short for a length prefix, and a string running past the message.
  CHECK_FALSE(forwardedSessionBind(string(1, char(27)) + "ab"));
  CHECK_FALSE(
      forwardedSessionBind(string(1, char(27)) + u32(100) + "session-bind"));
}

TEST_CASE("sessionBindHostKeyFingerprint matches ssh-keygen",
          "[SshAgentSessionBind]") {
  string hostKey;
  REQUIRE(Base64::Decode(
      "AAAAC3NzaC1lZDI1NTE5AAAAID4NdJ9B1LOS7+li7shEB10b9lQEjdsbe2oNQEW8fZP7",
      &hostKey));
  CHECK(sessionBindHostKeyFingerprint(sessionBind(hostKey, true)) ==
        "SHA256:1Z8TElQaniPWrBI8ZeIOaUH5BDBpeoK85Bt65Kr7ppY");
  CHECK_FALSE(
      sessionBindHostKeyFingerprint(string(1, SSH_AGENTC_REQUEST_IDENTITIES)));
}

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

TEST_CASE("SshAgentSessionBindRecorder relays to the agent and records binds",
          "[SshAgentSessionBind]") {
#ifdef WIN32
  SKIP("Unix-domain ssh-agent sockets");
#else
  FakeAgent agent([](const string& request) {
    if (request[0] == SSH_AGENTC_REQUEST_IDENTITIES) {
      return string(1, SSH_AGENT_IDENTITIES_ANSWER) + u32(0);
    }
    return string(1, SSH_AGENT_SUCCESS);
  });
  SshAgentSessionBindRecorder recorder(agent.path);
  CHECK_FALSE(recorder.lastForwardedBind());

  // A ProxyJump helper binds first; ssh binds the destination last.
  int jumpFd = connectUnix(recorder.socketPath());
  CHECK(agentRoundTrip(jumpFd, sessionBind("jump-key", false)) ==
        string(1, SSH_AGENT_SUCCESS));
  // The fake agent serves one connection at a time.
  ::close(jumpFd);
  int fd = connectUnix(recorder.socketPath());
  CHECK(agentRoundTrip(fd, sessionBind("dest-key", false)) ==
        string(1, SSH_AGENT_SUCCESS));
  CHECK(agentRoundTrip(fd, string(1, SSH_AGENTC_REQUEST_IDENTITIES)) ==
        string(1, SSH_AGENT_IDENTITIES_ANSWER) + u32(0));

  ::close(fd);

  REQUIRE(recorder.lastForwardedBind() == sessionBind("dest-key", true));
  // ssh's own binding reaches the real agent unchanged.
  auto received = agent.received();
  REQUIRE(received.size() == 3);
  CHECK(received[0] == sessionBind("jump-key", false));
  CHECK(received[1] == sessionBind("dest-key", false));
#endif
}

TEST_CASE("SshAgentSessionBindRecorder without a usable agent fails requests",
          "[SshAgentSessionBind]") {
#ifdef WIN32
  SKIP("Unix-domain ssh-agent sockets");
#else
  string upstream;
  SECTION("No agent") { upstream = ""; }
  SECTION("Agent socket missing") { upstream = "/nonexistent/et/agent.sock"; }
  SECTION("Agent path too long") { upstream = "/tmp/" + string(200, 'a'); }

  string socketPath;
  {
    SshAgentSessionBindRecorder recorder(upstream);
    socketPath = recorder.socketPath();
    int fd = connectUnix(socketPath);
    CHECK(agentRoundTrip(fd, string(1, SSH_AGENTC_REQUEST_IDENTITIES)) ==
          string(1, SSH_AGENT_FAILURE));
    CHECK(agentRoundTrip(fd, sessionBind("dest-key", false)) ==
          string(1, SSH_AGENT_FAILURE));
    ::close(fd);
    CHECK(recorder.lastForwardedBind() == sessionBind("dest-key", true));
  }
  CHECK_FALSE(fs::exists(fs::path(socketPath).parent_path()));
#endif
}

TEST_CASE("SshAgentSessionBindRecorder handles split and pipelined requests",
          "[SshAgentSessionBind]") {
#ifdef WIN32
  SKIP("Unix-domain ssh-agent sockets");
#else
  // Echoes each request so replies can be matched to requests.
  FakeAgent agent([](const string& request) {
    return string(1, SSH_AGENT_SUCCESS) + request;
  });
  SshAgentSessionBindRecorder recorder(agent.path);
  int fd = connectUnix(recorder.socketPath());

  string bind = frame(sessionBind("dest-key", false));
  string identities = frame(string(1, SSH_AGENTC_REQUEST_IDENTITIES));
  // A header and part of a body, then the rest together with a second
  // request.
  REQUIRE(::send(fd, bind.data(), 6, MSG_NOSIGNAL) == 6);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  string rest = bind.substr(6) + identities;
  REQUIRE(::send(fd, rest.data(), rest.size(), MSG_NOSIGNAL) ==
          ssize_t(rest.size()));

  string reply;
  REQUIRE(readFrame(fd, &reply));
  CHECK(reply == string(1, SSH_AGENT_SUCCESS) + sessionBind("dest-key", false));
  REQUIRE(readFrame(fd, &reply));
  CHECK(reply == string(1, SSH_AGENT_SUCCESS) +
                     string(1, SSH_AGENTC_REQUEST_IDENTITIES));
  CHECK(recorder.lastForwardedBind() == sessionBind("dest-key", true));
  ::close(fd);
#endif
}

TEST_CASE("SshAgentSessionBindRecorder drops connections it cannot serve",
          "[SshAgentSessionBind]") {
#ifdef WIN32
  SKIP("Unix-domain ssh-agent sockets");
#else
  string reply;
  SECTION("Malformed request length") {
    SshAgentSessionBindRecorder recorder("");
    int fd = connectUnix(recorder.socketPath());
    string zeroLength = u32(0);
    REQUIRE(::send(fd, zeroLength.data(), 4, MSG_NOSIGNAL) == 4);
    CHECK_FALSE(readFrame(fd, &reply));
    ::close(fd);
  }

  SECTION("Malformed agent reply") {
    // An empty body goes out as a zero-length frame.
    FakeAgent agent([](const string&) { return string(); });
    SshAgentSessionBindRecorder recorder(agent.path);
    int fd = connectUnix(recorder.socketPath());
    REQUIRE(sendFrame(fd, string(1, SSH_AGENTC_REQUEST_IDENTITIES)));
    CHECK_FALSE(readFrame(fd, &reply));
    ::close(fd);
  }

  SECTION("Agent closes without replying") {
    UnixListener upstream;
    SshAgentSessionBindRecorder recorder(upstream.path);
    int fd = connectUnix(recorder.socketPath());
    // The recorder connects to the agent when it accepts the client.
    int agentFd = ::accept(upstream.fd, nullptr, nullptr);
    setReceiveTimeout(agentFd);
    REQUIRE(sendFrame(fd, string(1, SSH_AGENTC_REQUEST_IDENTITIES)));
    string request;
    REQUIRE(readFrame(agentFd, &request));
    ::close(agentFd);
    CHECK_FALSE(readFrame(fd, &reply));
    ::close(fd);
  }
#endif
}

TEST_CASE("SshAgentSessionBindRecorder shuts down while a request is pending",
          "[SshAgentSessionBind]") {
#ifdef WIN32
  SKIP("Unix-domain ssh-agent sockets");
#else
  UnixListener upstream;  // Accepts nothing, so requests never get a reply.
  auto recorder = make_unique<SshAgentSessionBindRecorder>(upstream.path);
  int idle = connectUnix(recorder->socketPath());
  int fd = connectUnix(recorder->socketPath());
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  REQUIRE(sendFrame(fd, string(1, SSH_AGENTC_REQUEST_IDENTITIES)));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auto start = std::chrono::steady_clock::now();
  recorder.reset();
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
  // Both the waiting and the idle client are disconnected.
  string reply;
  CHECK_FALSE(readFrame(fd, &reply));
  CHECK_FALSE(readFrame(idle, &reply));
  ::close(fd);
  ::close(idle);
#endif
}

TEST_CASE("SshAgentSessionBindRecorder keeps waiting for a slow agent",
          "[SshAgentSessionBind]") {
#ifdef WIN32
  SKIP("Unix-domain ssh-agent sockets");
#else
  // Slower than the 5 s warning, as when the agent asks to confirm a key.
  FakeAgent agent([](const string&) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5500));
    return string(1, SSH_AGENT_SUCCESS);
  });
  SshAgentSessionBindRecorder recorder(agent.path);
  int fd = connectUnix(recorder.socketPath());
  CHECK(agentRoundTrip(fd, string(1, SSH_AGENTC_REQUEST_IDENTITIES)) ==
        string(1, SSH_AGENT_SUCCESS));
  ::close(fd);
#endif
}

TEST_CASE("Forwarded agent connections are bound before data flows",
          "[SshAgentSessionBind]") {
#ifdef WIN32
  SKIP("Unix-domain ssh-agent sockets");
#else
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
#endif
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
