#include "SecureFileUnix.hpp"
#include "ServerFifoPath.hpp"

namespace et {

/**
 * @file
 *
 * Provides utilities for creating and finding the server fifo path, handling
 * cases where etserver is running as either root or another user.
 *
 * When running as root, this applies the following principles to be defensive:
 * - Only use "/var/run" as the fifo directory.
 * - Do not query environment variables.
 * - Do not create directories or change file permissions.
 *
 * Unsafe ownership, links, or writable ancestors cause an error. A safe
 * user-owned fifo directory is restricted to 0700.
 */

namespace {

// As root, prefer "/var/run" since it is not world-writeable.
const string ROUTER_FIFO_BASENAME = "etserver.idpasskey.fifo";
const string ROOT_FIFO_DIRECTORY = "/var/run";
const string ROOT_ROUTER_FIFO_NAME =
    ROOT_FIFO_DIRECTORY + "/" + ROUTER_FIFO_BASENAME;

struct ValueWithDefault {
  string value;
  bool isDefault;
};

bool IsRoot() { return ::geteuid() == 0; }

bool IsAbsolutePath(const string& path) {
  return (!path.empty() && path[0] == '/');
}

string GetHome() {
  const char* home = getenv("HOME");
  CHECK_NOTNULL(home)
      << "Failed to get the value of the $HOME environment variable.";

  string homeStr(home);
  CHECK(IsAbsolutePath(homeStr))
      << "Unexpected relative path for $HOME environment variable: " << homeStr;
  return fs::path(homeStr).lexically_normal().string();
}

/**
 * Get the value of XDG_RUNTIME_DIR, by following the specification defined
 * here:
 * https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html
 */
ValueWithDefault GetXdgRuntimeDir() {
  // If the env doesn't exist, or is not an absolute path, fallback to
  // $HOME/.local/share since it can be created on mac as well.
  //
  // Per the spec:
  // > If an implementation encounters a relative path in any of these variables
  // > it should consider the path invalid and ignore it
  if (const char* dataHome = getenv("XDG_RUNTIME_DIR")) {
    if (IsAbsolutePath(dataHome)) {
      return ValueWithDefault{fs::path(dataHome).lexically_normal().string(),
                              /*isDefault*/ false};
    }
  }

  return ValueWithDefault{GetHome() + string("/.local/share"),
                          /*isDefault*/ true};
}

}  // namespace

ServerFifoPath::ServerFifoPath() = default;

void ServerFifoPath::setPathOverride(string path) {
  CHECK(!path.empty()) << "Server fifo path must not be empty";
  pathOverride = path;
}

void ServerFifoPath::createDirectoriesIfRequired() {
  // No action required unless we're running as non-root.
  if (pathOverride || IsRoot()) {
    return;
  }

  // Validating only etserver's mode misses replacement through a writable
  // parent. Walk all ancestors without following user-planted links.
  const auto runtime = GetXdgRuntimeDir();
  if (!runtime.isDefault && !fs::is_directory(runtime.value)) {
    throw std::runtime_error("XDG_RUNTIME_DIR must already exist");
  }
  private_storage::ensureDirectory(runtime.value + "/etserver");
}

string ServerFifoPath::getPathForCreation() {
  if (pathOverride) {
    return pathOverride.value();
  } else if (IsRoot()) {
    return ROOT_ROUTER_FIFO_NAME;
  } else {
    return GetXdgRuntimeDir().value + string("/etserver/") +
           ROUTER_FIFO_BASENAME;
  }
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
  } else {
    SocketEndpoint rootRouterEndpoint;
    rootRouterEndpoint.set_name(ROOT_ROUTER_FIFO_NAME);
    routerFd = socketHandler->connect(rootRouterEndpoint);
    if (routerFd >= 0) {
      // Successfully connected.
      return routerFd;
    }

    if (!IsRoot()) {
      // Fallback to trying the non-root location.
      SocketEndpoint nonRootRouterEndpoint;
      nonRootRouterEndpoint.set_name(GetXdgRuntimeDir().value +
                                     string("/etserver/") +
                                     ROUTER_FIFO_BASENAME);
      routerFd = socketHandler->connect(nonRootRouterEndpoint);
    }

    if (routerFd < 0) {
      reportConnectionError();
    }
  }

  return routerFd;
}

}  // namespace et
