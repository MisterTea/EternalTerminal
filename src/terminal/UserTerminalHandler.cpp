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
#elif __NetBSD__  // do not need pty.h on NetBSD
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
UserTerminalHandler::UserTerminalHandler(
    shared_ptr<SocketHandler> _socketHandler, bool _noratelimit)
    : socketHandler(_socketHandler), noratelimit(_noratelimit) {}

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
    socketHandler->writePacket(
        routerFd, Packet(TerminalPacketType::IDPASSKEY, idPasskey));
  } catch (const std::runtime_error &re) {
    LOG(FATAL) << "Error connecting to router: " << re.what();
  }
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
      VLOG(1) << "Child process " << pid << " launching terminal " << terminal;
      setenv("ET_VERSION", ET_VERSION, 1);
      execl(terminal.c_str(), terminal.c_str(), "--login", NULL);
      exit(0);
      break;
    }
    default: {
      // parent
      VLOG(1) << "pty opened " << masterfd;
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
#if __NetBSD__  // this unfortunateness seems to be fixed in NetBSD-8 (or at
                // least -CURRENT) sadness for now :/
          int throwaway;
          FATAL_FAIL(waitpid(childPid, &throwaway, WUNTRACED));
#else
          siginfo_t childInfo;
          FATAL_FAIL(waitid(P_PID, childPid, &childInfo, WEXITED));
#endif
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
            ioctl(masterFd, TIOCSWINSZ, &tmpwin);
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

#ifdef WITH_UTEMPTER
  utempter_remove_record(masterFd);
#endif
}
}  // namespace et
