#include "PipeSocketHandler.hpp"

namespace et {
bool PipeSocketHandler::prepareClientSocket(int fd, string* clientPath) {
  // Windows AF_UNIX does not autobind clients. bind/connect must also be the
  // first socket operation, so bind a short, unique pathname before applying
  // non-blocking configuration.
  string path = GetTempDirectory() + "htmc." +
                to_string(GetCurrentProcessId()) + "." +
                to_string(GetTickCount64()) + "." + to_string(fd);
  replace(path.begin(), path.end(), '\\', '/');
  sockaddr_un client;
  ZeroMemory(&client, sizeof(client));
  client.sun_family = AF_UNIX;
  strncpy_s(client.sun_path, sizeof(client.sun_path), path.c_str(), _TRUNCATE);
  DeleteFileA(path.c_str());
  if (::bind(fd, reinterpret_cast<sockaddr*>(&client),
             unixAddressLength(client)) < 0) {
    return false;
  }
  *clientPath = path;
  return true;
}

void PipeSocketHandler::prepareListenSocket(int /*fd*/) {}

void PipeSocketHandler::finishListenSocket(int fd, const string& /*pipePath*/) {
  // bind must be the first operation on a Windows AF_UNIX socket. Configure
  // non-blocking mode only after the address family provider is selected.
  initSocket(fd);
}

int PipeSocketHandler::connectAsUser(const SocketEndpoint& endpoint,
                                     uid_t /*uid*/, gid_t /*gid*/) {
  return connect(endpoint);
}

set<int> PipeSocketHandler::listenAsUser(const SocketEndpoint& endpoint,
                                         uid_t /*uid*/, gid_t /*gid*/) {
  return listen(endpoint);
}

void PipeSocketHandler::minimizeKernelBuffering(int /*fd*/) {}
}  // namespace et
