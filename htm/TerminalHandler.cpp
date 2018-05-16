#include "TerminalHandler.hpp"

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
TerminalHandler::TerminalHandler() : run(true) {}

void TerminalHandler::start() {
  pid_t pid = forkpty(&masterFd, NULL, NULL, NULL);
  switch (pid) {
    case -1:
      FATAL_FAIL(pid);
    case 0: {
      passwd* pwd = getpwuid(getuid());
      chdir(pwd->pw_dir);
      string terminal = string(::getenv("SHELL"));
      setenv("HTM_VERSION", ET_VERSION, 1);
      execl(terminal.c_str(), terminal.c_str(), "--login", NULL);
      exit(0);
      break;
    }
    default: {
      // parent
      VLOG(1) << "pty opened " << masterFd << endl;
      childPid = pid;
#ifdef WITH_UTEMPTER
      {
        char buf[1024];
        sprintf(buf, "htm [%lld]", (long long)getpid());
        utempter_add_record(masterFd, buf);
      }
#endif
      break;
    }
  }
}

#define MAX_BUFFER (1024)

string TerminalHandler::pollUserTerminal() {
  if (!run) {
    return string();
  }

#define BUF_SIZE (16 * 1024)
  char b[BUF_SIZE];

  // Data structures needed for select() and
  // non-blocking I/O.
  fd_set rfd;
  timeval tv;

  FD_ZERO(&rfd);
  FD_SET(masterFd, &rfd);
  tv.tv_sec = 0;
  tv.tv_usec = 10000;
  select(masterFd + 1, &rfd, NULL, NULL, &tv);

  try {
    // Check for data to receive; the received
    // data includes also the data previously sent
    // on the same master descriptor (line 90).
    if (FD_ISSET(masterFd, &rfd)) {
      // Read from terminal and write to client
      memset(b, 0, BUF_SIZE);
      int rc = read(masterFd, b, BUF_SIZE);
      if (rc < 0) {
        // Terminal failed for some reason, bail.
        throw std::runtime_error("Terminal Failure");
      }
      if (rc > 0) {
        string newChars(b, rc);
        vector<string> tokens = split(newChars, '\n');
        if (buffer.empty()) {
          buffer.insert(buffer.end(), tokens.begin(), tokens.end());
        } else {
          buffer.back().append(tokens.front());
          buffer.insert(buffer.end(), tokens.begin() + 1, tokens.end());
        }
        if (buffer.size() > MAX_BUFFER) {
          int amountToErase = buffer.size() - MAX_BUFFER;
          buffer.erase(buffer.begin(), buffer.begin() + amountToErase);
        }
        LOG(INFO) << "BUFFER LINES: " << buffer.size() << " " << tokens.size()
                  << endl;
        return newChars;
      } else {
        LOG(INFO) << "Terminal session ended";
#if __NetBSD__  // this unfortunateness seems to be fixed in NetBSD-8 (or at \
               // least -CURRENT) sadness for now :/
        int throwaway;
        FATAL_FAIL(waitpid(childPid, &throwaway, WUNTRACED));
#else
        siginfo_t childInfo;
        int rc = waitid(P_PID, childPid, &childInfo, WEXITED);
        if (rc < 0 && errno != ECHILD) {
          FATAL_FAIL(rc);
        }
#endif
        run = false;
#ifdef WITH_UTEMPTER
        utempter_remove_record(masterFd);
#endif
        return string();
      }
    }
  } catch (std::exception ex) {
    LOG(INFO) << ex.what();
    run = false;
#ifdef WITH_UTEMPTER
    utempter_remove_record(masterFd);
#endif
  }

  return string();
}

void TerminalHandler::appendData(const string& data) {
  RawSocketUtils::writeAll(masterFd, &data[0], data.length());
}

void TerminalHandler::updateTerminalSize(int col, int row) {
  winsize tmpwin;
  tmpwin.ws_row = row;
  tmpwin.ws_col = col;
  tmpwin.ws_xpixel = 0;
  tmpwin.ws_ypixel = 0;
  ioctl(masterFd, TIOCSWINSZ, &tmpwin);
}

void TerminalHandler::stop() {
  kill(childPid, SIGKILL);
  run = false;
}

}  // namespace et
