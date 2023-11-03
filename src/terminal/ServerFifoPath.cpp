#ifndef WIN32
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
 * For all users, this takes a fail-fast approach, where instead of correcting
 * issues it will crash or error out.
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
  return homeStr;
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
      return ValueWithDefault{dataHome, /*isDefault*/ false};
    }
  }

  return ValueWithDefault{GetHome() + string("/.local/share"),
                          /*isDefault*/ true};
}

void TryCreateDirectory(string dir, mode_t mode) {
  // Reset umask to 0 while creating subdirs, and restore after.
  const mode_t oldMode = ::umask(0);

  if (::mkdir(dir.c_str(), mode) == -1) {
    // Permit EEXIST if the directory already exists.
    CHECK_EQ(errno, EEXIST)
        << "Unexpected result creating " << dir << ": " << strerror(errno);
  }

  CHECK_EQ(::umask(oldMode), 0)
      << "Unexpected result when restoring umask, which should return the "
         "previous overridden value (0).";
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

  const auto xdgRuntimeDir = GetXdgRuntimeDir();
  if (xdgRuntimeDir.isDefault) {
    // Only create directories if the default path is returned.
    //
    // Create subdirectories for ~/.local/share. These may already be created
    // with different permissions on different machines, so also create an
    // etserver subdir to enforce 0700 permissions.
    const string homeDir = GetHome();
    TryCreateDirectory(homeDir + "/.local", 0755);
    TryCreateDirectory(homeDir + "/.local/share", 0755);
  }

  const string etserverDir = xdgRuntimeDir.value + "/etserver";

  // First try creating the directory. TryCreateDirectory will ignore error if
  // the directory already exists.
  TryCreateDirectory(etserverDir, 0700);

  struct stat etserverStat;
  const int statResult = ::stat(etserverDir.c_str(), &etserverStat);
  if (statResult != 0) {
    LOG(FATAL) << "Failed to create server fifo directory: " << etserverDir
               << "\n"
               << "Error: " << strerror(errno);
  }

  // Directory exists, verify that it has the appropriate permissions.
  if (etserverStat.st_uid != ::geteuid()) {
    LOG(FATAL) << "Server fifo directory must be owned by the current "
                  "user: "
               << etserverDir << "\n"
               << "Expected euid=" << ::geteuid()
               << ", actual=" << etserverStat.st_uid;
  }

  if (!S_ISDIR(etserverStat.st_mode)) {
    LOG(FATAL) << "Server fifo directory must be a directory: " << etserverDir;
  }

  // Fail if the folder has write permissions to group or other.
  if ((etserverStat.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    LOG(FATAL) << "Server fifo directory must not provide write access to "
                  "group/other: "
               << etserverDir;
  }
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
  } else {
    CLOG(INFO, "stdout")
        << "Error:  Connection error communicating with et daemon: "
        << strerror(localErrno) << "." << endl;
  }
  exit(1);
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
#endif
