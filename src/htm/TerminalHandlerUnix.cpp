#include <chrono>
#include <fstream>
#include <stdexcept>

#include "ETerminal.pb.h"
#include "RawSocketUtils.hpp"
#include "TerminalHandler.hpp"

#ifdef __APPLE__
#include <libproc.h>
#elif defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif

#ifdef CODE_COVERAGE
extern "C" void __gcov_reset(void);
#endif

namespace et {
TerminalHandler::TerminalHandler()
    : masterFd(-1), childPid(-1), run(false), bufferLength(0) {}

bool TerminalHandler::isRunning() { return run; }

int64_t TerminalHandler::childProcessId() const {
  return childPid > 0 ? static_cast<int64_t>(childPid) : 0;
}

string TerminalHandler::foregroundCommand() const {
  auto name_of = [](pid_t pid) -> string {
    if (pid <= 0) {
      return string();
    }
    string comm;
#ifdef __APPLE__
    char name[128];
    if (proc_name(pid, name, sizeof(name)) > 0) {
      comm = string(name);
    }
#elif defined(__FreeBSD__)
    // Avoid sys/user.h: kinfo_proc collides with std::thread.
    char path[1024] = {};
    size_t len = sizeof(path);
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, pid};
    if (sysctl(mib, 4, path, &len, NULL, 0) == 0 && path[0]) {
      string p(path);
      auto slash = p.find_last_of('/');
      comm = slash == string::npos ? p : p.substr(slash + 1);
    }
#else
    ifstream in(string("/proc/") + to_string(pid) + "/comm");
    getline(in, comm);
#endif
    if (comm == "pgrep" || comm == "pkill" || comm == "htmd" || comm == "htm") {
      return string();
    }
    return comm;
  };
  string comm;
  if (masterFd >= 0) {
    pid_t pgid = tcgetpgrp(masterFd);
    if (pgid > 0 && pgid != childPid) {
      comm = name_of(pgid);
    }
  }
  if (comm.empty()) {
    comm = name_of(childPid);
  }
  return comm;
}

void TerminalHandler::start(const string& cwd, int cols, int rows) {
  winsize ws;
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = static_cast<unsigned short>(cols > 0 ? cols : 80);
  ws.ws_row = static_cast<unsigned short>(rows > 0 ? rows : 24);
  pid_t pid = forkpty(&masterFd, NULL, NULL, &ws);
  switch (pid) {
    case -1:
      throw std::runtime_error(string("forkpty failed: ") +
                               strerror(GetErrno()));
    case 0: {
      passwd* pwd = getpwuid(getuid());
      if (pwd == NULL) {
        LOG(FATAL)
            << "Not able to fork a terminal because getpwuid returns null";
      }
      if (!cwd.empty()) {
        if (chdir(cwd.c_str()) != 0 && pwd->pw_dir) {
          chdir(pwd->pw_dir);
        }
      } else if (pwd->pw_dir) {
        chdir(pwd->pw_dir);
      }
      const char* shellEnv = ::getenv("SHELL");
      string terminal =
          (shellEnv && shellEnv[0]) ? string(shellEnv) : string("/bin/sh");
      setenv("HTM_VERSION", ET_VERSION, 1);
      // Match tmux -f /dev/null (default-terminal screen) so shells send the
      // same OSC/title sequences iTerm2 sees under tmux -CC.
      setenv("TERM", "screen", 1);
      // zsh's default PROMPT_EOL_MARK is a highlighted `%` plus spaces to the
      // right margin. GUI panes (Hyper, iTerm2) reflow that padding into a
      // stray `%` on its own line. Empty the mark so a fresh pane is clean.
      setenv("PROMPT_EOL_MARK", "", 1);
      // Non-login: inherit PATH from htmd and skip login scripts that may
      // switch to csh (FreeBSD's default user shell).
#ifdef CODE_COVERAGE
      // Drop inherited counters so the child does not dump .gcda on a failed
      // execl (exit/atexit) while the parent is still running under ctest.
      __gcov_reset();
#endif
      execl(terminal.c_str(), terminal.c_str(), NULL);
      _exit(127);
      break;
    }
    default: {
      // parent
      VLOG(1) << "pty opened " << masterFd << endl;
      childPid = pid;
      run = true;
      int flags = fcntl(masterFd, F_GETFL, 0);
      if (flags >= 0) {
        fcntl(masterFd, F_SETFL, flags | O_NONBLOCK);
      }
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

string TerminalHandler::pollUserTerminal() {
  if (!run || masterFd < 0) {
    return string();
  }
  flushPendingWrite();

#define BUF_SIZE (16 * 1024)
  char b[BUF_SIZE];

  fd_set rfd;
  timeval tv;

  FD_ZERO(&rfd);
  FD_SET(masterFd, &rfd);
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  select(masterFd + 1, &rfd, NULL, NULL, &tv);

  string collected;
  bool hungup = false;
  try {
    if (FD_ISSET(masterFd, &rfd)) {
      while (true) {
        int rc = read(masterFd, b, BUF_SIZE);
        if (rc > 0) {
          collected.append(b, rc);
          continue;
        }
        if (rc < 0 && (GetErrno() == EAGAIN || GetErrno() == EWOULDBLOCK ||
                       GetErrno() == EINTR)) {
          break;
        }
        // rc == 0 (Linux EOF) or EIO (BSD): PTY slave closed.
        hungup = true;
        LOG(INFO) << "Terminal session ended";
        break;
      }
    }
  } catch (const std::exception& ex) {
    LOG(INFO) << ex.what();
    hungup = true;
  }

  if (childPid > 0) {
    int status = 0;
    int wr = waitpid(childPid, &status, WNOHANG);
    if (wr == childPid || (wr < 0 && GetErrno() == ECHILD)) {
      hungup = true;
      childPid = -1;
    }
  }

  if (hungup) {
    run = false;
#ifdef WITH_UTEMPTER
    utempter_remove_record(masterFd);
#endif
  }

  if (!collected.empty()) {
    return bufferOutput(collected);
  }
  return string();
}

void TerminalHandler::flushPendingWrite() {
  if (masterFd < 0 || pendingWrite.empty()) {
    return;
  }
  while (!pendingWrite.empty()) {
    ssize_t rc = write(masterFd, pendingWrite.data(), pendingWrite.size());
    if (rc > 0) {
      pendingWrite.erase(0, static_cast<size_t>(rc));
      continue;
    }
    if (rc < 0 && (GetErrno() == EAGAIN || GetErrno() == EWOULDBLOCK ||
                   GetErrno() == EINTR)) {
      return;
    }
    LOG(INFO) << "Terminal write failed";
    run = false;
    pendingWrite.clear();
    return;
  }
}

void TerminalHandler::appendData(const string& data) {
  if (masterFd < 0 || data.empty()) {
    return;
  }
  pendingWrite.append(data);
  const size_t maxPending = 1024 * 1024;
  if (pendingWrite.size() > maxPending) {
    pendingWrite.erase(0, pendingWrite.size() - maxPending);
  }
  flushPendingWrite();
}

void TerminalHandler::updateTerminalSize(int col, int row) {
  if (masterFd < 0) {
    return;
  }
  winsize tmpwin;
  tmpwin.ws_row = row;
  tmpwin.ws_col = col;
  tmpwin.ws_xpixel = 0;
  tmpwin.ws_ypixel = 0;
  ioctl(masterFd, TIOCSWINSZ, &tmpwin);
}

void TerminalHandler::stop() {
  run = false;
  // Close the master PTY first so a child blocked on a full output buffer
  // is unblocked (SIGHUP/EIO) before we wait for it.
  if (masterFd >= 0) {
#ifdef WITH_UTEMPTER
    utempter_remove_record(masterFd);
#endif
    pendingWrite.clear();
    close(masterFd);
    masterFd = -1;
  }
  if (childPid > 0) {
    kill(childPid, SIGKILL);
    int status = 0;
    for (int i = 0; i < 50; i++) {
      int rc = waitpid(childPid, &status, WNOHANG);
      if (rc == childPid || (rc < 0 && GetErrno() == ECHILD)) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    childPid = -1;
  }
}
}  // namespace et
