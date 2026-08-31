#include <thread>

#include "HtmTestHelpers.hpp"
#include "IpcPairClient.hpp"
#include "IpcPairServer.hpp"
#include "TestHeaders.hpp"

using namespace et;
using namespace et::htmtest;

namespace {
class RecoveringServer : public IpcPairServer {
 public:
  int recoverCount = 0;
  RecoveringServer(shared_ptr<SocketHandler> handler,
                   const SocketEndpoint& endpoint)
      : IpcPairServer(handler, endpoint) {}
  void recover() override { recoverCount++; }
};
}  // namespace

TEST_CASE("IpcPairClient connects to a listening server", "[Htm][Ipc]") {
  UniqueIpcPath ipc;
  auto handler = std::make_shared<PipeSocketHandler>();
  auto endpoint = endpointFor(ipc.path);
  RecoveringServer server(handler, endpoint);

  IpcPairClient client(handler, endpoint);
  REQUIRE(client.getEndpointFd() >= 0);

  server.pollAccept();
  REQUIRE(server.getEndpointFd() >= 0);
  REQUIRE(server.recoverCount == 1);

  client.closeEndpoint();
}

TEST_CASE("IpcPairClient retries until the server listens", "[Htm][Ipc]") {
  UniqueIpcPath ipc;
  auto handler = std::make_shared<PipeSocketHandler>();
  auto endpoint = endpointFor(ipc.path);

  std::unique_ptr<RecoveringServer> server;
  std::thread listenLater([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    server.reset(new RecoveringServer(handler, endpoint));
  });

  IpcPairClient client(handler, endpoint);
  listenLater.join();
  REQUIRE(client.getEndpointFd() >= 0);
  REQUIRE(server != nullptr);
  server->pollAccept();
  REQUIRE(server->recoverCount == 1);
  client.closeEndpoint();
}

TEST_CASE("IpcPairClient throws after retries if nothing listens",
          "[Htm][Ipc]") {
  UniqueIpcPath ipc;
  auto handler = std::make_shared<PipeSocketHandler>();
  auto endpoint = endpointFor(ipc.path + ".missing");
  REQUIRE_THROWS_AS(IpcPairClient(handler, endpoint), std::runtime_error);
}

TEST_CASE("IpcPairServer replaces an existing client on a new accept",
          "[Htm][Ipc]") {
  UniqueIpcPath ipc;
  auto handler = std::make_shared<PipeSocketHandler>();
  auto endpoint = endpointFor(ipc.path);
  RecoveringServer server(handler, endpoint);

  IpcPairClient first(handler, endpoint);
  server.pollAccept();
  REQUIRE(server.recoverCount == 1);
  int firstFd = first.getEndpointFd();

  IpcPairClient second(handler, endpoint);
  server.pollAccept();
  REQUIRE(server.recoverCount == 2);

  string data = readUntil(handler, firstFd, 1, 1000);
  if (!data.empty()) {
    REQUIRE(data[0] == SESSION_END);
  }

  second.closeEndpoint();
}

TEST_CASE("IpcPairServer pollAccept is a no-op without a client", "[Htm][Ipc]") {
  UniqueIpcPath ipc;
  auto handler = std::make_shared<PipeSocketHandler>();
  auto endpoint = endpointFor(ipc.path);
  RecoveringServer server(handler, endpoint);
  server.pollAccept();
  REQUIRE(server.getEndpointFd() < 0);
  REQUIRE(server.recoverCount == 0);
}

TEST_CASE("IpcPairEndpoint closeEndpoint is idempotent", "[Htm][Ipc]") {
  UniqueIpcPath ipc;
  auto handler = std::make_shared<PipeSocketHandler>();
  auto endpoint = endpointFor(ipc.path);
  RecoveringServer server(handler, endpoint);
  IpcPairClient client(handler, endpoint);
  server.pollAccept();
  client.closeEndpoint();
  client.closeEndpoint();
  server.closeEndpoint();
  server.closeEndpoint();
}
