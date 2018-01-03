#include "UserTerminalRouter.hpp"

#include "RawSocketUtils.hpp"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#if __APPLE__
#include <sys/ucred.h>
#include <util.h>
#elif __FreeBSD__
#include <libutil.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#else
#include <pty.h>
#endif

#include "ETerminal.pb.h"

namespace et {
UserTerminalRouter::UserTerminalRouter() {
  sockaddr_un local;

  serverFd = socket(AF_UNIX, SOCK_STREAM, 0);
  FATAL_FAIL(serverFd);
  local.sun_family = AF_UNIX; /* local is declared before socket() ^ */
  strcpy(local.sun_path, ROUTER_FIFO_NAME);
  unlink(local.sun_path);

  // Also set the accept socket as non-blocking
  {
    int opts;
    opts = fcntl(serverFd, F_GETFL);
    FATAL_FAIL(opts);
    opts |= O_NONBLOCK;
    FATAL_FAIL(fcntl(serverFd, F_SETFL, opts));
  }
  // Also set the accept socket as reusable
  {
    int flag = 1;
    FATAL_FAIL(setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, (char *)&flag,
                          sizeof(int)));
  }

  FATAL_FAIL(::bind(serverFd, (struct sockaddr *)&local, sizeof(sockaddr_un)));
  ::listen(serverFd, 5);
  chmod(local.sun_path, 0777);
}

void UserTerminalRouter::acceptNewConnection(
    shared_ptr<ServerConnection> globalServer) {
  LOG(INFO) << "Listening to id/key FIFO";
  printf("Waiting for a connection...\n");
  sockaddr_un remote;
  socklen_t t = sizeof(remote);
  int terminalFd = ::accept(serverFd, (struct sockaddr *)&remote, &t);
  if (terminalFd < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      FATAL_FAIL(-1);  // LOG(FATAL) with the error
    } else {
      return;  // Nothing to accept this time
    }
  }

  LOG(INFO) << "Connected";

  string buf = RawSocketUtils::readMessage(terminalFd);
  VLOG(1) << "Got passkey: " << buf << endl;
  size_t slashIndex = buf.find("/");
  if (slashIndex == string::npos) {
    LOG(ERROR) << "Invalid idPasskey id/key pair: " << buf;
    close(terminalFd);
  } else {
    string id = buf.substr(0, slashIndex);
    string key = buf.substr(slashIndex + 1);
    idFdMap[id] = terminalFd;
    globalServer->addClientKey(id, key);
    // send config params to terminal
    ConfigParams config;
    config.set_vlevel(FLAGS_v);
    config.set_minloglevel(FLAGS_minloglevel);
    RawSocketUtils::writeProto(terminalFd, config);
  }
}

int UserTerminalRouter::getFd(const string &id) {
  auto it = idFdMap.find(id);
  if (it == idFdMap.end()) {
    LOG(FATAL) << " Tried to read from an id that no longer exists";
  }
  return it->second;
}
}  // namespace et
