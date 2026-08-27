#include "TcpSocketHandler.hpp"
#include "TestHeaders.hpp"

using namespace et;

TEST_CASE("TcpSocketHandler uses a configurable listen backlog",
          "[TcpSocketHandler]") {
  SECTION("defaults when unspecified") {
    TcpSocketHandler handler;
    REQUIRE(handler.getListenBacklog() ==
            TcpSocketHandler::DEFAULT_LISTEN_BACKLOG);
  }

  SECTION("keeps an explicit value") {
    TcpSocketHandler handler(512);
    REQUIRE(handler.getListenBacklog() == 512);
  }

  SECTION("falls back to the default on a non-positive value") {
    TcpSocketHandler zero(0);
    REQUIRE(zero.getListenBacklog() ==
            TcpSocketHandler::DEFAULT_LISTEN_BACKLOG);
    TcpSocketHandler negative(-1);
    REQUIRE(negative.getListenBacklog() ==
            TcpSocketHandler::DEFAULT_LISTEN_BACKLOG);
  }
}

TEST_CASE("TcpSocketHandler listens with a non-default backlog",
          "[TcpSocketHandler]") {
  TcpSocketHandler handler(64);
  SocketEndpoint endpoint;
  // Loopback and an ephemeral port: no firewall prompts, no fixed-port clashes.
  endpoint.set_name("127.0.0.1");
  endpoint.set_port(0);

  set<int> fds = handler.listen(endpoint);
  REQUIRE_FALSE(fds.empty());
  handler.stopListening(endpoint);
}
