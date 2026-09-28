#include <io.h>
#include <windows.h>

#include "UserSocketOps.hpp"

namespace et {
namespace {

bool pathTooLong(const string& path) {
  return path.size() >= sizeof(sockaddr_un::sun_path);
}

void fatalClose(int fd) {
  if (fd >= 0) {
    ::closesocket(fd);
  }
}

// Windows AF_UNIX client sockets do not autobind: bind a stable per-process
// pathname before connect, mirroring PipeSocketHandler. Reused across calls
// so no client files accumulate; a stale entry from a dead process is
// unlinked first.
string clientBindPath() {
  string path = GetTempDirectory() + "et_ucl." +
                to_string(GetCurrentProcessId()) + ".sock";
  for (char& c : path) {
    if (c == '\\') {
      c = '/';
    }
  }
  return path;
}

}  // namespace

void UserSocketOps::coverageExit(int code) { ::_exit(code); }

int UserSocketOps::listenAtPath(const string& path) {
  if (pathTooLong(path)) {
    SetErrno(ENAMETOOLONG);
    return -1;
  }

  SOCKET fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET) {
    return -1;
  }

  sockaddr_un local;
  memset(&local, 0, sizeof(local));
  local.sun_family = AF_UNIX;
  strncpy(local.sun_path, path.c_str(), sizeof(local.sun_path) - 1);

  // Only removes a path the current user can unlink.
  _unlink(local.sun_path);

  if (::bind(fd, reinterpret_cast<struct sockaddr*>(&local),
             unixAddressLength(local)) == SOCKET_ERROR ||
      ::listen(fd, 5) == SOCKET_ERROR) {
    const int err = GetErrno();
    fatalClose(static_cast<int>(fd));
    SetErrno(err);
    return -1;
  }
  if (fd > INT_MAX) {
    fatalClose(static_cast<int>(fd));
    SetErrno(EMFILE);
    return -1;
  }
  return static_cast<int>(fd);
}

int UserSocketOps::connectAtPath(const string& path) {
  if (pathTooLong(path)) {
    SetErrno(ENAMETOOLONG);
    return -1;
  }

  SOCKET fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET) {
    return -1;
  }

  const string bindPath = clientBindPath();
  sockaddr_un client;
  memset(&client, 0, sizeof(client));
  client.sun_family = AF_UNIX;
  strncpy(client.sun_path, bindPath.c_str(), sizeof(client.sun_path) - 1);
  DeleteFileA(client.sun_path);
  if (::bind(fd, reinterpret_cast<struct sockaddr*>(&client),
             unixAddressLength(client)) == SOCKET_ERROR) {
    const int err = GetErrno();
    fatalClose(static_cast<int>(fd));
    SetErrno(err);
    return -1;
  }

  sockaddr_un remote;
  memset(&remote, 0, sizeof(remote));
  remote.sun_family = AF_UNIX;
  strncpy(remote.sun_path, path.c_str(), sizeof(remote.sun_path) - 1);

  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&remote),
                unixAddressLength(remote)) == SOCKET_ERROR) {
    const int err = GetErrno();
    ::shutdown(fd, SD_BOTH);
    fatalClose(static_cast<int>(fd));
    DeleteFileA(client.sun_path);
    SetErrno(err);
    return -1;
  }
  if (fd > INT_MAX) {
    ::shutdown(fd, SD_BOTH);
    fatalClose(static_cast<int>(fd));
    DeleteFileA(client.sun_path);
    SetErrno(EMFILE);
    return -1;
  }
  return static_cast<int>(fd);
}

int UserSocketOps::listenUnixAsUser(const string& path, uid_t /*uid*/,
                                    gid_t /*gid*/) {
  // No privilege separation on Windows; act as the current user.
  return listenAtPath(path);
}

int UserSocketOps::connectUnixAsUser(const string& path, uid_t /*uid*/,
                                     gid_t /*gid*/) {
  return connectAtPath(path);
}
}  // namespace et
