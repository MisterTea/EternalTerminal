#include "HtmServer.hpp"

#ifndef WIN32
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#include <sys/un.h>
#endif
#endif

#include "ControlCommands.hpp"

namespace et {
namespace {
#ifndef WIN32
pid_t unixPeerPid(int fd) {
  if (fd < 0) {
    return -1;
  }
#ifdef __APPLE__
  pid_t pid = -1;
  socklen_t len = sizeof(pid);
  if (::getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &len) == 0) {
    return pid;
  }
#elif defined(SO_PEERCRED)
  struct ucred cred;
  memset(&cred, 0, sizeof(cred));
  socklen_t len = sizeof(cred);
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
    return cred.pid;
  }
#endif
  return -1;
}

void reapControlClient(pid_t peer) {
  set<pid_t> targets;
  auto consider = [&](pid_t pid) {
    if (pid > 1 && pid != ::getpid()) {
      targets.insert(pid);
    }
  };
  consider(peer);
  {
    string path = GetTempDirectory() + "htm." + GetHtmIpcUser() + ".client.pid";
    FILE* fp = fopen(path.c_str(), "r");
    if (fp) {
      int parsed = -1;
      if (fscanf(fp, "%d", &parsed) == 1 && parsed > 1 &&
          parsed != ::getpid()) {
        bool isHtm = true;
#if defined(__linux__)
        string commPath = string("/proc/") + to_string(parsed) + "/comm";
        FILE* commFp = fopen(commPath.c_str(), "r");
        if (commFp) {
          char commBuf[64];
          if (fgets(commBuf, sizeof(commBuf), commFp)) {
            size_t len = strlen(commBuf);
            while (len > 0 &&
                   (commBuf[len - 1] == '\r' || commBuf[len - 1] == '\n')) {
              commBuf[--len] = '\0';
            }
            isHtm = (strcmp(commBuf, "htm") == 0);
          } else {
            isHtm = false;
          }
          fclose(commFp);
        } else {
          isHtm = false;
        }
#endif
        if (isHtm) {
          consider(static_cast<pid_t>(parsed));
        }
      }
      fclose(fp);
      ::unlink(path.c_str());
    }
  }
#ifdef __APPLE__
  {
    uid_t uid = ::getuid();
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (bytes > 0) {
      vector<pid_t> pids(static_cast<size_t>(bytes) / sizeof(pid_t) + 16);
      bytes = proc_listpids(PROC_ALL_PIDS, 0, pids.data(),
                            static_cast<int>(pids.size() * sizeof(pid_t)));
      int count = bytes > 0 ? bytes / static_cast<int>(sizeof(pid_t)) : 0;
      for (int i = 0; i < count; i++) {
        if (pids[i] <= 0) {
          continue;
        }
        char name[128];
        if (proc_name(pids[i], name, sizeof(name)) <= 0) {
          continue;
        }
        if (strcmp(name, "htm") != 0) {
          continue;
        }
        struct proc_bsdinfo info;
        if (proc_pidinfo(pids[i], PROC_PIDTBSDINFO, 0, &info, sizeof(info)) <=
            0) {
          continue;
        }
        if (info.pbi_uid == uid) {
          consider(pids[i]);
        }
      }
    }
  }
#endif
  LOG(INFO) << "detach reap " << targets.size()
            << " htm client(s) self=" << ::getpid();
  // Give a healthy client time to drain leftover PTY input, restore blocking
  // stdio, and _exit. SIGKILL leftover `htm` only if it is still wedged so
  // `htm; exec $SHELL` matches tmux -CC.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    bool anyAlive = false;
    for (pid_t pid : targets) {
      if (::kill(pid, 0) == 0) {
        anyAlive = true;
        break;
      }
    }
    if (!anyAlive) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  for (pid_t pid : targets) {
    if (::kill(pid, 0) == 0) {
      LOG(INFO) << "SIGKILL htm client " << pid;
      ::kill(pid, SIGKILL);
    }
  }
}
#endif
}  // namespace
HtmServer::HtmServer(shared_ptr<SocketHandler> _socketHandler,
                     const SocketEndpoint& endpoint)
    : IpcPairServer(_socketHandler, endpoint),
      skipLfAfterCr(false),
      running(true),
      paneDumpRequested(false) {
  state.setWriter(&writer);
}

void HtmServer::handleClientData() {
  char buf[4096];
  int rc = socketHandler->read(endpointFd, buf, sizeof(buf));
  if (rc <= 0) {
    LOG(INFO) << "Client disconnect";
    closeEndpoint();
    writer.clearSocket();
    return;
  }
  lineBuf.append(buf, static_cast<size_t>(rc));
  // iTerm2's tmux gateway writes CR by default. Real tmux sees LF because the
  // PTY has ICRNL; htm puts stdin in raw mode, so split on CR and LF here.
  if (skipLfAfterCr) {
    if (!lineBuf.empty() && lineBuf[0] == '\n') {
      lineBuf.erase(0, 1);
    }
    skipLfAfterCr = false;
  }
  size_t pos = 0;
  while (true) {
    size_t termLen = 0;
    size_t nl = string::npos;
    for (size_t i = pos; i < lineBuf.size(); i++) {
      if (lineBuf[i] == '\n') {
        nl = i;
        termLen = 1;
        break;
      }
      if (lineBuf[i] == '\r') {
        nl = i;
        if (i + 1 < lineBuf.size() && lineBuf[i + 1] == '\n') {
          termLen = 2;
        } else {
          termLen = 1;
          skipLfAfterCr = (i + 1 == lineBuf.size());
        }
        break;
      }
    }
    if (nl == string::npos) {
      if (pos > 0) {
        lineBuf.erase(0, pos);
      }
      break;
    }
    string line = lineBuf.substr(pos, nl - pos);
    pos = nl + termLen;
    processLine(line);
    if (endpointFd < 0) {
      lineBuf.clear();
      return;
    }
  }
}

void HtmServer::processLine(const string& line) {
  vector<string> commands = splitControlCommandList(line);
  if (commands.empty()) {
    commands.push_back(line);
  }
  for (const string& cmd : commands) {
    LOG(INFO) << "control command: " << cmd;
    ControlAction action = executeControlCommand(&state, &writer, cmd);
    if (action == ControlAction::Error) {
      // iTerm2 auto-fails the rest of a `;` command list after %error.
      break;
    }
    if (action == ControlAction::Detach) {
#ifndef WIN32
      pid_t peer = unixPeerPid(endpointFd);
#endif
      writer.tryNotify("%exit");
      closeEndpoint();
      writer.clearSocket();
#ifndef WIN32
      reapControlClient(peer);
#endif
      return;
    }
    if (action == ControlAction::KillServer) {
#ifndef WIN32
      pid_t peer = unixPeerPid(endpointFd);
#endif
      writer.tryNotify("%exit");
      closeEndpoint();
      writer.clearSocket();
#ifndef WIN32
      reapControlClient(peer);
#endif
      running.store(false);
      return;
    }
  }
}

void HtmServer::run() {
  while (running.load()) {
    if (endpointFd < 0) {
      writePaneDumpIfRequested();
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      try {
        pollAccept();
      } catch (const std::exception& re) {
        STERROR << re.what();
        try {
          closeEndpoint();
        } catch (const std::exception& closeEx) {
          LOG(INFO) << "closeEndpoint after accept/recover: " << closeEx.what();
        }
        writer.clearSocket();
      }
      continue;
    }

    try {
      bool readable = false;
#ifndef WIN32
      struct pollfd pfd;
      pfd.fd = endpointFd;
      pfd.events = POLLIN;
      pfd.revents = 0;
      int pr = ::poll(&pfd, 1, 10);
      if (pr < 0 && GetErrno() != EINTR) {
        throw std::runtime_error(string("poll failed: ") +
                                 strerror(GetErrno()));
      }
      if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) &&
          !(pfd.revents & POLLIN)) {
        LOG(INFO) << "Client hangup";
        closeEndpoint();
        writer.clearSocket();
        continue;
      }
      readable = (pfd.revents & POLLIN) != 0;
#else
      fd_set rfd;
      timeval tv;
      FD_ZERO(&rfd);
      FD_SET(endpointFd, &rfd);
      tv.tv_sec = 0;
      tv.tv_usec = 10000;
      select(endpointFd + 1, &rfd, NULL, NULL, &tv);
      readable = FD_ISSET(endpointFd, &rfd) != 0;
#endif
      if (readable) {
        handleClientData();
      }
      if (endpointFd > 0) {
        state.pollOutput();
        writePaneDumpIfRequested();
        // Shell-exit of the last pane does not go through kill-pane; still
        // end control mode the way tmux -CC does when no windows remain.
        if (state.empty()) {
#ifndef WIN32
          pid_t peer = unixPeerPid(endpointFd);
#endif
          writer.tryNotify("%exit");
          closeEndpoint();
          writer.clearSocket();
#ifndef WIN32
          reapControlClient(peer);
#endif
          running.store(false);
        }
      }
    } catch (const std::exception& re) {
      try {
        closeEndpoint();
      } catch (const std::exception& closeEx) {
        LOG(INFO) << "closeEndpoint after disconnect: " << closeEx.what();
      }
      writer.clearSocket();
      LOG(INFO) << "Client disconnect: " << re.what();
    }
  }
  try {
    closeEndpoint();
  } catch (const std::exception& closeEx) {
    LOG(INFO) << "closeEndpoint on shutdown: " << closeEx.what();
  }
  writer.clearSocket();
  state.stopAll();
}

void HtmServer::recover() {
  writer.setSocket(socketHandler, endpointFd);
  lineBuf.clear();
  skipLfAfterCr = false;
  // tmux -CC prints an empty server-originated block on attach. iTerm2 will not
  // send refresh-client / list-windows until it sees this %end (flags 0).
  writer.beginServerOriginated();
  writer.end();
  state.attachNotifications();
}

string HtmServer::getPipeName() {
#ifdef WIN32
  return string("htm.") + GetHtmIpcUser() + string(".ipc");
#else
  return string(GetTempDirectory() + "htm.") + GetHtmIpcUser() + string(".ipc");
#endif
}

string HtmServer::getPaneDumpPath() {
  return string(GetTempDirectory() + "htm.") + GetHtmIpcUser() +
         string(".panes");
}

void HtmServer::writePaneDumpIfRequested() {
  if (!paneDumpRequested.exchange(false)) {
    return;
  }
  const string path = getPaneDumpPath();
  const string tmp = path + ".tmp";
  FILE* fp = fopen(tmp.c_str(), "w");
  if (!fp) {
    LOG(WARNING) << "pane dump fopen failed: " << tmp;
    return;
  }
  string body = state.dumpAllPanesText();
  fwrite(body.data(), 1, body.size(), fp);
  fclose(fp);
#ifdef WIN32
  _unlink(path.c_str());
  rename(tmp.c_str(), path.c_str());
#else
  ::rename(tmp.c_str(), path.c_str());
#endif
}

#ifdef WIN32
string HtmServer::getShutdownEventName() {
  return string("Local\\EternalTerminal.HtmShutdown.") + GetHtmIpcUser();
}

string HtmServer::getPaneDumpEventName() {
  return string("Local\\EternalTerminal.HtmPaneDump.") + GetHtmIpcUser();
}
#endif
}  // namespace et
