#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#include <sys/un.h>
#endif

#include "HtmServer.hpp"

namespace et {
int64_t HtmServer::controlClientPid(int fd) {
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

void HtmServer::reapControlClient(int64_t peer) {
  set<pid_t> targets;
  auto consider = [&](pid_t pid) {
    if (pid > 1 && pid != ::getpid()) {
      targets.insert(pid);
    }
  };
  consider(static_cast<pid_t>(peer));
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

string HtmServer::getPipeName() {
  return string(GetTempDirectory() + "htm.") + GetHtmIpcUser() + string(".ipc");
}
}  // namespace et
