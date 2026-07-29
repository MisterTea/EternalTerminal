#include "UserSocketOps.hpp"

#ifndef WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace et {
namespace {
struct ResultHeader {
  int status;  // 0 ok, -1 error
  int err;
};

void fatalClose(int fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}
}  // namespace

void UserSocketOps::sendFd(int channel, int fdToSend, int status, int err) {
  ResultHeader header{status, err};
  struct iovec iov;
  iov.iov_base = &header;
  iov.iov_len = sizeof(header);

  char control[CMSG_SPACE(sizeof(int))];
  memset(control, 0, sizeof(control));

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  if (status == 0 && fdToSend >= 0) {
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fdToSend, sizeof(int));
  }

  // Best-effort; child exits immediately after.
  ::sendmsg(channel, &msg, 0);
}

int UserSocketOps::recvFd(int channel, int* errOut) {
  ResultHeader header;
  struct iovec iov;
  iov.iov_base = &header;
  iov.iov_len = sizeof(header);

  char control[CMSG_SPACE(sizeof(int))];
  memset(control, 0, sizeof(control));

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);

  ssize_t n = ::recvmsg(channel, &msg, 0);
  if (n != (ssize_t)sizeof(header)) {
    if (errOut) {
      *errOut = EIO;
    }
    return -1;
  }
  if (header.status != 0) {
    if (errOut) {
      *errOut = header.err ? header.err : EIO;
    }
    return -1;
  }

  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  if (cmsg == nullptr || cmsg->cmsg_level != SOL_SOCKET ||
      cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len < CMSG_LEN(sizeof(int))) {
    if (errOut) {
      *errOut = EIO;
    }
    return -1;
  }
  int fd = -1;
  memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
  if (errOut) {
    *errOut = 0;
  }
  return fd;
}

void UserSocketOps::childListen(int resultFd, const string& path) {
  if (path.size() >= sizeof(sockaddr_un::sun_path)) {
    sendFd(resultFd, -1, -1, ENAMETOOLONG);
    _exit(1);
  }

  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    sendFd(resultFd, -1, -1, errno);
    _exit(1);
  }

  sockaddr_un local;
  memset(&local, 0, sizeof(local));
  local.sun_family = AF_UNIX;
  strncpy(local.sun_path, path.c_str(), sizeof(local.sun_path) - 1);

  // Only removes a path the dropped-privilege user can unlink.
  ::unlink(local.sun_path);

  if (::bind(fd, (struct sockaddr*)&local, sizeof(local)) < 0) {
    int err = errno;
    fatalClose(fd);
    sendFd(resultFd, -1, -1, err);
    _exit(1);
  }
  if (::listen(fd, 5) < 0) {
    int err = errno;
    fatalClose(fd);
    sendFd(resultFd, -1, -1, err);
    _exit(1);
  }
  if (::fchmod(fd, S_IRUSR | S_IWUSR | S_IXUSR) < 0) {
    // fchmod on unix sockets is unsupported on some platforms; fall back to
    // path chmod. Still running as the session user.
    ::chmod(local.sun_path, S_IRUSR | S_IWUSR | S_IXUSR);
  }

  sendFd(resultFd, fd, 0, 0);
  fatalClose(fd);
  _exit(0);
}

void UserSocketOps::childConnect(int resultFd, const string& path) {
  if (path.size() >= sizeof(sockaddr_un::sun_path)) {
    sendFd(resultFd, -1, -1, ENAMETOOLONG);
    _exit(1);
  }

  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    sendFd(resultFd, -1, -1, errno);
    _exit(1);
  }

  sockaddr_un remote;
  memset(&remote, 0, sizeof(remote));
  remote.sun_family = AF_UNIX;
  strncpy(remote.sun_path, path.c_str(), sizeof(remote.sun_path) - 1);

  if (::connect(fd, (struct sockaddr*)&remote, sizeof(remote)) < 0) {
    int err = errno;
    fatalClose(fd);
    sendFd(resultFd, -1, -1, err);
    _exit(1);
  }

  sendFd(resultFd, fd, 0, 0);
  fatalClose(fd);
  _exit(0);
}

int UserSocketOps::runAsUser(Op op, const string& path, uid_t uid, gid_t gid) {
  int sv[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
    return -1;
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    int err = errno;
    fatalClose(sv[0]);
    fatalClose(sv[1]);
    SetErrno(err);
    return -1;
  }

  if (pid == 0) {
    fatalClose(sv[0]);
    // Drop privileges before any path operation. Do not use logging here.
    // Clear supplemental groups when running as root so we do not retain the
    // parent's group set. setgroups(2) requires privilege and is skipped
    // otherwise (e.g. already-unprivileged test processes).
    if (::geteuid() == 0) {
      if (::setgroups(1, &gid) != 0) {
        sendFd(sv[1], -1, -1, errno);
        _exit(1);
      }
    }
    if (::setgid(gid) != 0) {
      sendFd(sv[1], -1, -1, errno);
      _exit(1);
    }
    if (::setuid(uid) != 0) {
      sendFd(sv[1], -1, -1, errno);
      _exit(1);
    }
    if (op == Op::LISTEN) {
      childListen(sv[1], path);
    } else {
      childConnect(sv[1], path);
    }
    _exit(1);
  }

  fatalClose(sv[1]);
  int err = 0;
  int fd = recvFd(sv[0], &err);
  fatalClose(sv[0]);

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      break;
    }
  }

  if (fd < 0) {
    SetErrno(err ? err : EIO);
    return -1;
  }
  return fd;
}

int UserSocketOps::listenUnixAsUser(const string& path, uid_t uid, gid_t gid) {
  return runAsUser(Op::LISTEN, path, uid, gid);
}

int UserSocketOps::connectUnixAsUser(const string& path, uid_t uid, gid_t gid) {
  return runAsUser(Op::CONNECT, path, uid, gid);
}
}  // namespace et
#endif
