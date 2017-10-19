#include "UserTerminalHandler.hpp"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
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

#ifdef WITH_UTEMPTER
#include <utempter.h>
#endif

#include "RawSocketUtils.hpp"
#include "ServerConnection.hpp"
#include "UserTerminalRouter.hpp"

#include "ETerminal.pb.h"

namespace et {
UserTerminalHandler::UserTerminalHandler() {}

void UserTerminalHandler::connectToRouter(const string &idPasskey) {
  sockaddr_un remote;

  routerFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  FATAL_FAIL(routerFd);
  remote.sun_family = AF_UNIX;
  strcpy(remote.sun_path, ROUTER_FIFO_NAME);

  if (connect(routerFd, (struct sockaddr *)&remote, sizeof(sockaddr_un)) < 0) {
    close(routerFd);
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
  FATAL_FAIL(
      RawSocketUtils::writeAll(routerFd, &(idPasskey[0]), idPasskey.length()));
  FATAL_FAIL(RawSocketUtils::writeAll(routerFd, "\0", 1));
}

void UserTerminalHandler::run() {
  int masterfd;

  pid_t pid = forkpty(&masterfd, NULL, NULL, NULL);
  switch (pid) {
    case -1:
      FATAL_FAIL(pid);
    case 0: {
      close(routerFd);
      passwd *pwd = getpwuid(getuid());
      chdir(pwd->pw_dir);
      string terminal = string(::getenv("SHELL"));
      VLOG(1) << "Child process " << pid << " launching terminal " << terminal
              << endl;
      setenv("ET_VERSION", ET_VERSION, 1);
      execl(terminal.c_str(), terminal.c_str(), "--login", NULL);
      exit(0);
      break;
    }
    default: {
      // parent
      VLOG(1) << "pty opened " << masterfd << endl;
      runUserTerminal(masterfd, pid);
      close(routerFd);
      break;
    }
  }
}

void UserTerminalHandler::runUserTerminal(int masterFd, pid_t childPid) {
#ifdef WITH_UTEMPTER
  {
    char buf[1024];
    sprintf(buf, "et [%lld]", (long long)getpid());
    utempter_add_record(masterFd, buf);
  }
#endif

  bool run = true;

#define BUF_SIZE (16 * 1024)
  char b[BUF_SIZE];

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

    // Check for data to receive; the received
    // data includes also the data previously sent
    // on the same master descriptor (line 90).
    if (FD_ISSET(masterFd, &rfd)) {
      // Read from terminal and write to client
      memset(b, 0, BUF_SIZE);
      int rc = read(masterFd, b, BUF_SIZE);
      FATAL_FAIL(rc);
      if (rc > 0) {
        RawSocketUtils::writeAll(routerFd, b, rc);
      } else {
        LOG(INFO) << "Terminal session ended";
        siginfo_t childInfo;
        FATAL_FAIL(waitid(P_PID, childPid, &childInfo, WEXITED));
        run = false;
        break;
      }
    }

    try {
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
                RawSocketUtils::readProto<TerminalBuffer>(routerFd);
            const string &buffer = tb.buffer();
            FATAL_FAIL(RawSocketUtils::writeAll(masterFd, &buffer[0],
                                                buffer.length()));
            break;
          }
          case TERMINAL_INFO: {
            TerminalInfo ti = RawSocketUtils::readProto<TerminalInfo>(routerFd);
            winsize tmpwin;
            tmpwin.ws_row = ti.row();
            tmpwin.ws_col = ti.column();
            tmpwin.ws_xpixel = ti.width();
            tmpwin.ws_ypixel = ti.height();
            ioctl(masterFd, TIOCSWINSZ, &tmpwin);
            break;
          }
        }
      }
    } catch (std::exception ex) {
      LOG(INFO) << ex.what();
      run = false;
      break;
    }
  }

#ifdef WITH_UTEMPTER
  utempter_remove_record(masterFd);
#endif
}
}  // namespace et
