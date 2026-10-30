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

TEST_CASE("TranslateSocketError maps socket errors to errno values",
          "[TcpSocketHandler]") {
  SECTION("leaves errno values unchanged") {
    REQUIRE(TranslateSocketError(0) == 0);
    REQUIRE(TranslateSocketError(ECONNREFUSED) == ECONNREFUSED);
    REQUIRE(TranslateSocketError(ETIMEDOUT) == ETIMEDOUT);
    REQUIRE(TranslateSocketError(ENETUNREACH) == ENETUNREACH);
    REQUIRE(TranslateSocketError(EHOSTUNREACH) == EHOSTUNREACH);
  }

#ifdef WIN32
  SECTION("translates Winsock errors") {
    REQUIRE(TranslateSocketError(WSAECONNREFUSED) == ECONNREFUSED);
    REQUIRE(TranslateSocketError(WSAETIMEDOUT) == ETIMEDOUT);
    REQUIRE(TranslateSocketError(WSAENETUNREACH) == ENETUNREACH);
    REQUIRE(TranslateSocketError(WSAEHOSTUNREACH) == EHOSTUNREACH);
    REQUIRE(TranslateSocketError(WSAEADDRNOTAVAIL) == EADDRNOTAVAIL);
  }
#endif
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

TEST_CASE("TcpSocketHandler peer-disconnected sockopt classification",
          "[TcpSocketHandler]") {
  // The errnos a setsockopt() on a freshly accepted/connected socket can
  // return when the peer has already disconnected must be tolerated rather
  // than aborting the server. ECONNRESET is the case that crashed etserver
  // on FreeBSD (its tcp_ctloutput() returns ECONNRESET once the tcpcb has
  // been dropped); EINVAL is the equivalent on macOS/BSD.
  REQUIRE(TcpSocketHandler::isPeerDisconnectedSockoptError(ECONNRESET));
  REQUIRE(TcpSocketHandler::isPeerDisconnectedSockoptError(EINVAL));
  REQUIRE(TcpSocketHandler::isPeerDisconnectedSockoptError(ENOTCONN));

  // Genuine logic errors must still be treated as fatal.
  REQUIRE_FALSE(TcpSocketHandler::isPeerDisconnectedSockoptError(EBADF));
  REQUIRE_FALSE(TcpSocketHandler::isPeerDisconnectedSockoptError(ENOPROTOOPT));
  REQUIRE_FALSE(TcpSocketHandler::isPeerDisconnectedSockoptError(ENOTSOCK));
  REQUIRE_FALSE(TcpSocketHandler::isPeerDisconnectedSockoptError(EFAULT));
}

#ifndef WIN32
TEST_CASE("TcpSocketHandler accept survives clients that reset before accept",
          "[TcpSocketHandler]") {
  // A client that connects and immediately resets (SO_LINGER 0 close) can
  // leave a dead connection in the accept queue. accept() + initSocket()
  // on it must not abort the server, and the listener must keep working.
  TcpSocketHandler handler;
  SocketEndpoint endpoint;
  endpoint.set_name("127.0.0.1");
  endpoint.set_port(0);
  set<int> fds = handler.listen(endpoint);
  REQUIRE_FALSE(fds.empty());
  const int listenFd = *fds.begin();

  sockaddr_storage addr;
  socklen_t addrLen = sizeof(addr);
  REQUIRE(::getsockname(listenFd, (sockaddr*)&addr, &addrLen) == 0);
  const int port = ntohs(((sockaddr_in*)&addr)->sin_port);

  for (int i = 0; i < 5; i++) {
    const int raw = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(raw >= 0);
    sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::connect(raw, (sockaddr*)&serverAddr, sizeof(serverAddr)) == 0);
    struct linger resetLinger;
    resetLinger.l_onoff = 1;
    resetLinger.l_linger = 0;
    REQUIRE(::setsockopt(raw, SOL_SOCKET, SO_LINGER, (char*)&resetLinger,
                         sizeof(resetLinger)) == 0);
    REQUIRE(::close(raw) == 0);

    // Either the dead connection is accepted (and closed here) or the
    // kernel has already discarded it; neither may abort the process.
    const int accepted = handler.accept(listenFd);
    if (accepted >= 0) {
      handler.close(accepted);
    }
  }

  // The listener still accepts a real client afterwards.
  SocketEndpoint connectEndpoint;
  connectEndpoint.set_name("127.0.0.1");
  connectEndpoint.set_port(port);
  const int clientFd = handler.connect(connectEndpoint);
  REQUIRE(clientFd >= 0);
  const int accepted = handler.accept(listenFd);
  REQUIRE(accepted >= 0);
  handler.close(accepted);
  handler.close(clientFd);
  handler.stopListening(endpoint);
}
#endif

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
