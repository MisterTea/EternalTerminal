#include "PipeSocketHandler.hpp"
#include "RawSocketUtils.hpp"
#include "TestHeaders.hpp"
#include "TestSocketPair.hpp"

using namespace et;

namespace {
class TrackedSocketHandler : public PipeSocketHandler {
 public:
  void track(int fd) {
    addToActiveSockets(fd);
    initSocket(fd);
  }
};
}  // namespace

TEST_CASE("Socket handler transfers bytes through native sockets",
          "[SocketIo]") {
  int sockets[2];
  REQUIRE(et::test::createTestSocketPair(sockets) == 0);
  TrackedSocketHandler handler;
  handler.track(sockets[0]);
  handler.track(sockets[1]);

  const string payload = "native socket roundtrip";
  const ssize_t written =
      handler.write(sockets[0], payload.data(), payload.size());
  string received(payload.size(), '\0');
  ssize_t bytesRead = -1;
  if (written == static_cast<ssize_t>(payload.size())) {
    bytesRead = handler.read(sockets[1], received.data(), received.size());
  }
  handler.close(sockets[0]);
  handler.close(sockets[1]);
  REQUIRE(written == static_cast<ssize_t>(payload.size()));
  REQUIRE(bytesRead == static_cast<ssize_t>(payload.size()));
  REQUIRE(received == payload);
}

TEST_CASE("Pipe sockets connect in the OS temporary directory", "[SocketIo]") {
  const string directory = et::test::makeTempDir("et_socket_io");
  SocketEndpoint endpoint;
  endpoint.set_name(directory + "/socket");
  PipeSocketHandler handler;
  handler.listen(endpoint);
  const int client = handler.connect(endpoint);
  if (client >= 0) {
    handler.close(client);
  }
  handler.stopListening(endpoint);
  et::test::removeTempDir(directory);
  REQUIRE(client >= 0);
}
