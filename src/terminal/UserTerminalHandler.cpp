#include "UserTerminalHandler.hpp"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>

#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifdef WITH_UTEMPTER
#include <utempter.h>
#endif

#include "RawSocketUtils.hpp"
#include "ServerConnection.hpp"
#include "UserTerminalRouter.hpp"

#include "ETerminal.pb.h"

namespace et {
UserTerminalHandler::UserTerminalHandler(
    shared_ptr<SocketHandler> _socketHandler, shared_ptr<UserTerminal> _term,
    bool _noratelimit)
    : socketHandler(_socketHandler), term(_term), noratelimit(_noratelimit) {}

void UserTerminalHandler::connectToRouter(const string &idPasskey) {
  routerFd = socketHandler->connect(SocketEndpoint(ROUTER_FIFO_NAME));

  if (routerFd < 0) {
    if (errno == ECONNREFUSED) {
      cout << "Error:  The Eternal Terminal daemon is not running.  Please "
              "(re)start the et daemon on the server."
           << endl;
    } else {
      cout << "Error:  Connection error communicating with et deamon: "
           << strerror(errno) << "." << endl;
    }
    exit(1);
  }

  try {
    socketHandler->writeMessage(routerFd, idPasskey);
  } catch (const std::runtime_error &re) {
    LOG(FATAL) << "Error connecting to router: " << re.what();
  }
}

void UserTerminalHandler::run() {
  int masterfd = term->setup(routerFd);
  VLOG(1) << "pty opened " << masterfd;
  runUserTerminal(masterfd);
  close(routerFd);
}

void UserTerminalHandler::runUserTerminal(int masterFd) {
  bool run = true;

#define BUF_SIZE (16 * 1024)
  char b[BUF_SIZE];

  time_t lastSecond = time(NULL);
  int64_t outputPerSecond = 0;

  while (run) {
    // Data structures needed for select() and
    // non-blocking I/O.
    fd_set rfd;
    timeval tv;

    FD_ZERO(&rfd);
    FD_SET(masterFd, &rfd);
    FD_SET(routerFd, &rfd);
    int maxfd = max(masterFd, routerFd);
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    select(maxfd + 1, &rfd, NULL, NULL, &tv);
    VLOG(4) << "select is done";

    time_t currentSecond = time(NULL);
    if (lastSecond != currentSecond) {
      outputPerSecond = 0;
      lastSecond = currentSecond;
    }

    try {
      // Check for data to receive; the received
      // data includes also the data previously sent
      // on the same master descriptor (line 90).
      if (FD_ISSET(masterFd, &rfd) && (noratelimit || outputPerSecond < 1024)) {
        // Read from terminal and write to client, with a limit in rows/sec
        memset(b, 0, BUF_SIZE);
        int rc = read(masterFd, b, BUF_SIZE);
        VLOG(4) << "Read from terminal";
        FATAL_FAIL(rc);
        if (rc > 0) {
          string s(b, rc);
          outputPerSecond += std::count(s.begin(), s.end(), '\n');
          socketHandler->writeAllOrThrow(routerFd, b, rc, false);
          VLOG(4) << "Write to client: "
                  << std::count(s.begin(), s.end(), '\n');
        } else {
          LOG(INFO) << "Terminal session ended";
          term->handleSessionEnd();
          run = false;
          break;
        }
      }

      if (FD_ISSET(routerFd, &rfd)) {
        char packetType;
        int rc = read(routerFd, &packetType, 1);
        FATAL_FAIL(rc);
        if (rc == 0) {
          throw std::runtime_error(
              "Router has ended abruptly.  Killing terminal session.");
        }
        switch (packetType) {
          case TERMINAL_BUFFER: {
            TerminalBuffer tb =
                socketHandler->readProto<TerminalBuffer>(routerFd, false);
            VLOG(4) << "Read from router";
            const string &buffer = tb.buffer();
            RawSocketUtils::writeAll(masterFd, &buffer[0], buffer.length());
            VLOG(4) << "Write to terminal";
            break;
          }
          case TERMINAL_INFO: {
            TerminalInfo ti =
                socketHandler->readProto<TerminalInfo>(routerFd, false);
            winsize tmpwin;
            tmpwin.ws_row = ti.row();
            tmpwin.ws_col = ti.column();
            tmpwin.ws_xpixel = ti.width();
            tmpwin.ws_ypixel = ti.height();
            term->setInfo(tmpwin);
            break;
          }
        }
      }
    } catch (const std::exception &ex) {
      LOG(INFO) << ex.what();
      run = false;
      break;
    }
  }

  term->cleanup();
}
}  // namespace et
