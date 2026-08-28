/**
 * TerminalServer::handleConnection() waits for the client's initial payload. It
 * must be able to give up: otherwise the thread spins forever on a client that
 * never speaks, and run() never finishes joining it at shutdown.
 */

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <future>
#include <thread>

#include "PipeSocketHandler.hpp"
#include "ServerClientConnection.hpp"
#include "TerminalServer.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {
// Reads and writes raw descriptors, so a plain socketpair can stand in for a
// connected client. UnixSocketHandler refuses descriptors it did not create.
class FdSocketHandler : public SocketHandler {
 public:
  bool hasData(int fd) override { return waitOnSocketData(fd); }

  ssize_t read(int fd, void* buf, size_t count) override {
    return ::read(fd, buf, count);
  }

  ssize_t write(int fd, const void* buf, size_t count) override {
    return ::write(fd, buf, count);
  }

  int connect(const SocketEndpoint&) override { return -1; }
  set<int> listen(const SocketEndpoint&) override { return {}; }
  set<int> getEndpointFds(const SocketEndpoint&) override { return {}; }
  int accept(int) override { return -1; }
  void stopListening(const SocketEndpoint&) override {}
  void close(int fd) override { ::close(fd); }
  vector<int> getActiveSockets() override { return {}; }
};

// Exposes the initial-payload deadline so it can be tested without waiting out
// the production value.
class TestTerminalServer : public TerminalServer {
 public:
  TestTerminalServer(shared_ptr<SocketHandler> socketHandler,
                     const SocketEndpoint& serverEndpoint,
                     shared_ptr<PipeSocketHandler> pipeSocketHandler,
                     const SocketEndpoint& routerEndpoint)
      : TerminalServer(std::move(socketHandler), serverEndpoint,
                       std::move(pipeSocketHandler), routerEndpoint) {}

  void setInitialPayloadTimeout(int seconds) {
    initialPayloadTimeoutSec = seconds;
  }
};

// Owns the temp directory holding the server and router pipes.
struct ServerFixture {
  string directory;
  string serverPipePath;
  string routerPipePath;
  shared_ptr<PipeSocketHandler> serverSocketHandler;
  shared_ptr<PipeSocketHandler> routerSocketHandler;
  // Heap allocated so a worker can keep it alive past this scope, see below.
  shared_ptr<TestTerminalServer> server;

  SocketEndpoint serverEndpoint;
  SocketEndpoint routerEndpoint;

  ServerFixture() {
    string pattern = GetTempDirectory() + string("et_handleconn_XXXXXX");
    const char* created = mkdtemp(&pattern[0]);
    REQUIRE(created != nullptr);
    directory = string(created);
    serverPipePath = directory + "/pipe_server";
    routerPipePath = directory + "/pipe_router";

    serverSocketHandler = make_shared<PipeSocketHandler>();
    routerSocketHandler = make_shared<PipeSocketHandler>();

    serverEndpoint.set_name(serverPipePath);
    routerEndpoint.set_name(routerPipePath);
    server =
        make_shared<TestTerminalServer>(serverSocketHandler, serverEndpoint,
                                        routerSocketHandler, routerEndpoint);
  }

  ~ServerFixture() {
    // Both constructors above start listening; nothing else closes those
    // sockets, since TerminalServer::shutdown() only sets the halt flag.
    serverSocketHandler->stopListening(serverEndpoint);
    routerSocketHandler->stopListening(routerEndpoint);
    ::remove(serverPipePath.c_str());
    ::remove(routerPipePath.c_str());
    ::rmdir(directory.c_str());
  }
};

// Runs handleConnection() on its own thread and reports whether it returned
// within the deadline. A regression makes it never return, so the worker is
// detached rather than joined on failure: that lets the suite report the
// failure instead of hanging, and the worker owns shared_ptrs to everything it
// touches, so leaking it stays safe.
struct HandleConnectionRun {
  shared_ptr<std::promise<void>> finished = make_shared<std::promise<void>>();
  std::future<void> future = finished->get_future();
  std::thread worker;

  HandleConnectionRun(shared_ptr<TerminalServer> server,
                      shared_ptr<ServerClientConnection> connection) {
    auto done = finished;
    worker = std::thread([server, connection, done]() {
      server->handleConnection(connection);
      done->set_value();
    });
  }

  ~HandleConnectionRun() {
    if (worker.joinable()) {
      worker.detach();
    }
  }

  bool returnedWithin(std::chrono::seconds deadline) {
    if (future.wait_for(deadline) != std::future_status::ready) {
      return false;
    }
    worker.join();
    return true;
  }
};

// A live but silent client: the socket stays open and nothing ever arrives on
// it, which is the state that used to keep the thread looping forever. The
// connection gets its own handler because UnixSocketHandler refuses
// descriptors it did not create.
shared_ptr<ServerClientConnection> makeSilentClient(int fds[2]) {
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  // Non-blocking, as initSocket() leaves every socket the server owns. A
  // blocking descriptor would park BackedReader::read() instead of reporting
  // that no data is available.
  REQUIRE(::fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
  return make_shared<ServerClientConnection>(
      make_shared<FdSocketHandler>(), "silent-client", fds[0],
      "abcdefghijklmnopqrstuvwxyz012345");
}
}  // namespace

TEST_CASE("handleConnection returns when the connection is shutting down",
          "[HandleConnectionTimeout][integration]") {
  ServerFixture fixture;

  auto connection = make_shared<ServerClientConnection>(
      fixture.serverSocketHandler, "shutdown-client", -1,
      "abcdefghijklmnopqrstuvwxyz012345");

  connection->shutdown();

  HandleConnectionRun run(fixture.server, connection);
  REQUIRE(run.returnedWithin(std::chrono::seconds(5)));
}

TEST_CASE("handleConnection returns when the server is halted",
          "[HandleConnectionTimeout][integration]") {
  ServerFixture fixture;

  int fds[2] = {-1, -1};
  auto connection = makeSilentClient(fds);

  fixture.server->shutdown();

  HandleConnectionRun run(fixture.server, connection);
  const bool returned = run.returnedWithin(std::chrono::seconds(5));

  if (returned) {
    // Only safe once handleConnection() is done, since shutdown() waits on the
    // mutex it holds. Skipping it on failure keeps teardown from hanging.
    connection->shutdown();
  }
  ::close(fds[1]);
  REQUIRE(returned);
}

TEST_CASE("handleConnection gives up on a client that never sends a payload",
          "[HandleConnectionTimeout][integration]") {
  ServerFixture fixture;
  fixture.server->setInitialPayloadTimeout(1);

  int fds[2] = {-1, -1};
  auto connection = makeSilentClient(fds);

  // Neither halted nor shutting down, so only the deadline can end this wait.
  HandleConnectionRun run(fixture.server, connection);
  const bool returned = run.returnedWithin(std::chrono::seconds(10));

  if (returned) {
    connection->shutdown();
  }
  ::close(fds[1]);
  REQUIRE(returned);
}
