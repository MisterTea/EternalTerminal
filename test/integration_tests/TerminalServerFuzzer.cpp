#include "FuzzableTerminalServer.hpp"

namespace et {

static std::unique_ptr<FuzzableTerminalServer> sServer;

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
  sServer.reset(new FuzzableTerminalServer());

  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  PipeSocketHandler sockerHandler;
  const int fd = sockerHandler.connect(sServer->serverEndpoint);
  if (fd == -1) {
    // Ignore if we fail to connect.
    return 0;
  }

  sockerHandler.write(fd, data, size);

  // Shutdown the server, to verify that it gracefully exits.
  sServer->terminalServer->shutdown();

  sockerHandler.close(fd);

  return 0;
}

}  // namespace et
