#include "ServerFifoPath.hpp"

#ifdef WIN32

namespace et {

namespace {

const string ROUTER_FIFO_BASENAME = "etserver.idpasskey.fifo";

string WindowsFifoDirectory() {
  string tmp = GetTempDirectory();
  for (char& c : tmp) {
    if (c == '\\') {
      c = '/';
    }
  }
  return tmp;
}

string DefaultWindowsFifoPath() {
  string dir = WindowsFifoDirectory();
  if (!dir.empty() && dir.back() != '/') {
    dir += '/';
  }
  return dir + "etserver." + GetHtmIpcUser() + ".fifo";
}

}  // namespace

ServerFifoPath::ServerFifoPath() = default;

void ServerFifoPath::setPathOverride(string path) {
  CHECK(!path.empty()) << "Server fifo path must not be empty";
  pathOverride = path;
}

void ServerFifoPath::createDirectoriesIfRequired() {
  if (pathOverride) {
    return;
  }
  // %TEMP% always exists; create it if a custom TEMP points elsewhere.
  try {
    fs::create_directories(WindowsFifoDirectory());
  } catch (const std::exception& ex) {
    LOG(FATAL) << "Failed to create server fifo directory: "
               << WindowsFifoDirectory() << "\nError: " << ex.what();
  }
}

string ServerFifoPath::getPathForCreation() {
  if (pathOverride) {
    return pathOverride.value();
  }
  return DefaultWindowsFifoPath();
}

optional<SocketEndpoint> ServerFifoPath::getEndpointForConnect() {
  if (pathOverride) {
    SocketEndpoint endpoint;
    endpoint.set_name(pathOverride.value());
    return endpoint;
  } else {
    return std::nullopt;
  }
}

void reportConnectionError() {
  const int localErrno = GetErrno();

  if (localErrno == ECONNREFUSED) {
    CLOG(INFO, "stdout")
        << "Error:  The Eternal Terminal daemon is not running.  Please "
           "(re)start the et daemon on the server."
        << endl;
    throw std::runtime_error(
        "The Eternal Terminal daemon is not running. Please (re)start the et "
        "daemon on the server.");
  } else {
    CLOG(INFO, "stdout")
        << "Error:  Connection error communicating with et daemon: "
        << strerror(localErrno) << "." << endl;
    throw std::runtime_error("Connection error communicating with et daemon: " +
                             string(strerror(localErrno)));
  }
}

int ServerFifoPath::detectAndConnect(
    const optional<SocketEndpoint> specificRouterEndpoint,
    const shared_ptr<SocketHandler>& socketHandler) {
  int routerFd = -1;
  if (specificRouterEndpoint) {
    routerFd = socketHandler->connect(specificRouterEndpoint.value());
    if (routerFd < 0) {
      reportConnectionError();
    }
    return routerFd;
  }
  // No override: single per-user location on Windows (no root/non-root split).
  SocketEndpoint defaultEndpoint;
  defaultEndpoint.set_name(DefaultWindowsFifoPath());
  routerFd = socketHandler->connect(defaultEndpoint);
  if (routerFd < 0) {
    reportConnectionError();
  }
  return routerFd;
}

}  // namespace et
}  // namespace et
#endif
