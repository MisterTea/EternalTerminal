#include "IpcPairClient.hpp"

namespace et {
IpcPairClient::IpcPairClient(const string &pipeName) : IpcPairEndpoint(-1) {
  sockaddr_un remote;

  for (int retry = 0; retry < 5; retry++) {
    endpointFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    FATAL_FAIL(endpointFd);
    remote.sun_family = AF_UNIX;
    strcpy(remote.sun_path, pipeName.c_str());
    LOG(INFO) << "Connecting to IPC server " << pipeName << " (attempt "
              << (retry + 1) << ")";
    if (::connect(endpointFd, (struct sockaddr *)&remote, sizeof(sockaddr_un)) <
        0) {
      ::close(endpointFd);
      sleep(1);
    } else {
      return;
    }
  }
  throw std::runtime_error("Connect to IPC failed");
}

}  // namespace et