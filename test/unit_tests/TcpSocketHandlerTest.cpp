#include <stdexcept>

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

// The kernel accept queue depth cannot be read back portably, so this only
// covers that a custom backlog reaches listen() without being rejected.
TEST_CASE("TcpSocketHandler listen succeeds with a custom backlog",
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

TEST_CASE("TcpSocketHandler listen succeeds with IPv4 on localhost",
          "[TcpSocketHandler]") {
  TcpSocketHandler handler;
  SocketEndpoint endpoint;
  endpoint.set_name("127.0.0.1");
  endpoint.set_port(0);
  set<int> fds = handler.listen(endpoint);
  REQUIRE_FALSE(fds.empty());
  handler.stopListening(endpoint);
}

TEST_CASE("TcpSocketHandler connect reports why it failed",
          "[TcpSocketHandler]") {
  TcpSocketHandler handler;
  SocketEndpoint endpoint;

  SECTION("unresolvable hostname") {
    endpoint.set_name("nonexistent.invalid");
    endpoint.set_port(2022);
    REQUIRE(handler.connect(endpoint) == -1);
    REQUIRE(handler.getLastConnectError().rfind(
                "Could not resolve hostname nonexistent.invalid", 0) == 0);
  }

  SECTION("refused on a closed loopback port") {
    endpoint.set_name("127.0.0.1");
    endpoint.set_port(1);
    REQUIRE(handler.connect(endpoint) == -1);
    REQUIRE(handler.getLastConnectError() == strerror(ECONNREFUSED));
  }

  SECTION("cleared by a successful connect") {
    endpoint.set_name("nonexistent.invalid");
    endpoint.set_port(2022);
    REQUIRE(handler.connect(endpoint) == -1);

    SocketEndpoint listenEndpoint;
    listenEndpoint.set_name("127.0.0.1");
    listenEndpoint.set_port(0);
    set<int> fds = handler.listen(listenEndpoint);
    REQUIRE_FALSE(fds.empty());
    sockaddr_storage addr;
    socklen_t addrLen = sizeof(addr);
    REQUIRE(::getsockname(*fds.begin(), (sockaddr*)&addr, &addrLen) == 0);
    SocketEndpoint connectEndpoint;
    connectEndpoint.set_name("127.0.0.1");
    connectEndpoint.set_port(ntohs(((sockaddr_in*)&addr)->sin_port));

    int fd = handler.connect(connectEndpoint);
    REQUIRE(fd >= 0);
    REQUIRE(handler.getLastConnectError().empty());
    handler.close(fd);
    handler.stopListening(listenEndpoint);
  }
}

TEST_CASE("TcpSocketHandler listen throws on unresolvable hostname",
          "[TcpSocketHandler]") {
  TcpSocketHandler handler;
  SocketEndpoint endpoint;
  endpoint.set_name("nonexistent.invalid");
  endpoint.set_port(0);
  bool threw = false;
  try {
    set<int> fds = handler.listen(endpoint);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  REQUIRE(threw);
}
