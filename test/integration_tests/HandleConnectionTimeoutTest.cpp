/**
 * TerminalServer::handleConnection() waits for the client's initial payload. It
 * must be able to give up: otherwise the thread spins forever on a client that
 * never speaks, and run() never finishes joining it at shutdown.
 */

#include <unistd.h>

#include <chrono>
#include <future>
#include <thread>

#include "PipeSocketHandler.hpp"
#include "ServerClientConnection.hpp"
#include "TerminalServer.hpp"
#include "TestHeaders.hpp"

using namespace et;

TEST_CASE("handleConnection returns when the connection is shutting down",
          "[HandleConnectionTimeout][integration]") {
  auto serverSocketHandler = make_shared<PipeSocketHandler>();
  auto routerSocketHandler = make_shared<PipeSocketHandler>();

  string pattern = GetTempDirectory() + string("et_handleconn_XXXXXX");
  string directory = string(mkdtemp(&pattern[0]));
  REQUIRE_FALSE(directory.empty());

  const string serverPipePath = directory + "/pipe_server";
  const string routerPipePath = directory + "/pipe_router";
  SocketEndpoint serverEndpoint;
  serverEndpoint.set_name(serverPipePath);
  SocketEndpoint routerEndpoint;
  routerEndpoint.set_name(routerPipePath);

  // Heap allocated and captured by value below so the worker keeps everything
  // it touches alive even if this scope exits first.
  auto server = make_shared<TerminalServer>(
      serverSocketHandler, serverEndpoint, routerSocketHandler, routerEndpoint);
  auto connection = make_shared<ServerClientConnection>(
      serverSocketHandler, "shutdown-client", -1,
      "abcdefghijklmnopqrstuvwxyz012345");
  connection->shutdown();

  auto handled = make_shared<std::promise<void>>();
  std::future<void> handledFuture = handled->get_future();

  // A regression makes handleConnection() never return. Detaching instead of
  // joining lets the suite report that as a failure rather than hang.
  struct DetachOnFailure {
    std::thread worker;
    ~DetachOnFailure() {
      if (worker.joinable()) {
        worker.detach();
      }
    }
  } guard{std::thread([server, connection, handled]() {
    server->handleConnection(connection);
    handled->set_value();
  })};

  REQUIRE(handledFuture.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
  guard.worker.join();

  ::remove(serverPipePath.c_str());
  ::remove(routerPipePath.c_str());
  ::rmdir(directory.c_str());
}
