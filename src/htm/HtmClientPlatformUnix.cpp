#include "ControlMode.hpp"
#include "DaemonCreator.hpp"
#include "HtmClient.hpp"
#include "HtmClientPlatform.hpp"
#include "HtmServer.hpp"
#include "SubprocessUtils.hpp"

#ifdef __APPLE__
#include <libproc.h>
#include <mach-o/dyld.h>
#endif
#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

namespace et {
namespace htm_client_platform {
namespace {
string siblingHtmdPath() {
  char buf[PATH_MAX];
  buf[0] = '\0';
#ifdef __APPLE__
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) != 0) {
    return "htmd";
  }
#else
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) {
    return "htmd";
  }
  buf[n] = '\0';
#endif
  char resolved[PATH_MAX];
  if (!realpath(buf, resolved)) {
    return "htmd";
  }
  fs::path exe(resolved);
  fs::path htmd = exe.parent_path() / "htmd";
  if (!fs::exists(htmd)) {
    return "htmd";
  }
  return htmd.string();
}

#if defined(__linux__)
string htmdPidsForUser(uid_t uid) {
  DIR* dir = opendir("/proc");
  if (!dir) {
    string command = string("pgrep -x -U ") + to_string(uid) + string(" htmd");
    return SystemToStr(command.c_str());
  }
  string out;
  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (!isdigit(entry->d_name[0])) {
      continue;
    }
    string pidStr(entry->d_name);
    string commPath = string("/proc/") + pidStr + "/comm";
    FILE* fp = fopen(commPath.c_str(), "r");
    if (!fp) {
      continue;
    }
    char commBuf[64];
    bool isHtmd = false;
    if (fgets(commBuf, sizeof(commBuf), fp)) {
      size_t len = strlen(commBuf);
      while (len > 0 &&
             (commBuf[len - 1] == '\r' || commBuf[len - 1] == '\n')) {
        commBuf[--len] = '\0';
      }
      isHtmd = (strcmp(commBuf, "htmd") == 0);
    }
    fclose(fp);
    if (!isHtmd) {
      continue;
    }
    string statusPath = string("/proc/") + pidStr + "/status";
    FILE* sfp = fopen(statusPath.c_str(), "r");
    if (!sfp) {
      continue;
    }
    char lineBuf[256];
    bool uidMatches = false;
    bool isZombie = false;
    while (fgets(lineBuf, sizeof(lineBuf), sfp)) {
      if (strncmp(lineBuf, "State:", 6) == 0) {
        const char* state = lineBuf + 6;
        while (*state == ' ' || *state == '\t') {
          ++state;
        }
        isZombie = (*state == 'Z');
      } else if (strncmp(lineBuf, "Uid:", 4) == 0) {
        int ruid = -1;
        if (sscanf(lineBuf + 4, "%d", &ruid) == 1 &&
            static_cast<uid_t>(ruid) == uid) {
          uidMatches = true;
        }
      }
    }
    fclose(sfp);
    if (uidMatches && !isZombie) {
      out += pidStr;
      out += '\n';
    }
  }
  closedir(dir);
  return out;
}
#elif defined(__APPLE__)
string htmdPidsForUser(uid_t uid) {
  // proc_bsdinfo::pbi_status contains the BSD p_stat value. SZOMB is not
  // exposed by the public macOS SDK headers, but its ABI value is 5.
  constexpr uint32_t kZombieProcessStatus = 5;
  int bytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
  if (bytes <= 0) {
    return "";
  }
  vector<pid_t> pids(static_cast<size_t>(bytes) / sizeof(pid_t) + 16);
  bytes = proc_listpids(PROC_ALL_PIDS, 0, pids.data(),
                        static_cast<int>(pids.size() * sizeof(pid_t)));
  int count = bytes > 0 ? bytes / static_cast<int>(sizeof(pid_t)) : 0;
  string out;
  for (int i = 0; i < count; i++) {
    if (pids[i] <= 0) {
      continue;
    }
    char name[128];
    if (proc_name(pids[i], name, sizeof(name)) <= 0) {
      continue;
    }
    if (strcmp(name, "htmd") != 0) {
      continue;
    }
    struct proc_bsdinfo info;
    if (proc_pidinfo(pids[i], PROC_PIDTBSDINFO, 0, &info, sizeof(info)) <= 0) {
      continue;
    }
    if (info.pbi_uid != uid || info.pbi_status == kZombieProcessStatus) {
      continue;
    }
    out += to_string(pids[i]);
    out += '\n';
  }
  return out;
}
#else
string htmdPidsForUser(uid_t uid) {
  string command = string("pgrep -x -U ") + to_string(uid) + string(" htmd");
  return SystemToStr(command.c_str());
}
#endif

void writeHtmExitSequence() {
  const char* st = kControlModeSt;
  int flags = fcntl(STDOUT_FILENO, F_GETFL);
  if (flags >= 0) {
    fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK);
  }
  ::write(STDOUT_FILENO, st, strlen(st));
}

void term(int) {
  // Never write to the PTY or restore the tty from a signal handler: both
  // can block forever when the GUI has stopped draining DCS. tmux just dies.
  ::_exit(1);
}
}  // namespace

void installTerminationHandlers(void (* /*restoreTerminal*/)()) {
  struct sigaction action;
  memset(&action, 0, sizeof(struct sigaction));
  action.sa_handler = term;
  sigaction(SIGTERM, &action, NULL);
}

void ensureDaemon(bool killExisting) {
  uid_t myuid = getuid();
  const string pipeName = HtmServer::getPipeName();
  auto htmdPids = [&]() { return htmdPidsForUser(myuid); };
  if (killExisting) {
    LOG(INFO) << "Killing previous htmd";
    string running = htmdPids();
    string pidStr;
    for (char ch : running) {
      if (ch == '\n') {
        if (!pidStr.empty()) {
          ::kill(static_cast<pid_t>(atoi(pidStr.c_str())), SIGTERM);
          pidStr.clear();
        }
      } else {
        pidStr += ch;
      }
    }
    if (!pidStr.empty()) {
      ::kill(static_cast<pid_t>(atoi(pidStr.c_str())), SIGTERM);
    }
    for (int i = 0; i < 50; i++) {
      if (htmdPids().empty()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::unlink(pipeName.c_str());
    string clientPidPath =
        GetTempDirectory() + "htm." + GetHtmIpcUser() + ".client.pid";
    ::unlink(clientPidPath.c_str());
  }

  if (htmdPids().empty()) {
    int daemonResult = DaemonCreator::create(false, "");
    if (daemonResult == DaemonCreator::CHILD) {
      string path = siblingHtmdPath();
      execl(path.c_str(), "htmd", (char*)nullptr);
      execlp("htmd", "htmd", (char*)nullptr);
      _exit(1);
    }
  }

  for (int i = 0; i < 100; i++) {
    if (::access(pipeName.c_str(), F_OK) == 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

int finishClient(int code, void (* /*restoreTerminal*/)()) {
  writeHtmExitSequence();
  drainHtmStdin();
  int outFlags = fcntl(STDOUT_FILENO, F_GETFL);
  if (outFlags >= 0) {
    fcntl(STDOUT_FILENO, F_SETFL, outFlags & ~O_NONBLOCK);
  }
  ::_exit(code);
}
}  // namespace htm_client_platform
}  // namespace et
