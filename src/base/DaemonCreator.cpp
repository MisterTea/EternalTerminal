#include "DaemonCreator.hpp"

#ifdef WIN32
#include <windows.h>
#endif

namespace et {
#ifdef WIN32
int DaemonCreator::createSessionLeader() { return 0; }

int DaemonCreator::create(bool parentExit, string childPidFile) {
  wchar_t modulePath[MAX_PATH];
  fs::path htmdPath;
  if (GetModuleFileNameW(NULL, modulePath, MAX_PATH) != 0) {
    htmdPath = fs::path(modulePath).parent_path() / L"htmd.exe";
  }

  wstring cmdLine;
  const wchar_t* application = nullptr;
  if (!htmdPath.empty() && fs::exists(htmdPath)) {
    application = htmdPath.c_str();
    cmdLine = L"\"" + htmdPath.wstring() + L"\"";
  } else {
    cmdLine = L"htmd.exe";
  }

  STARTUPINFOW si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));

  vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
  cmdBuf.push_back(L'\0');

  BOOL ok =
      CreateProcessW(application, cmdBuf.data(), NULL, NULL, FALSE,
                     DETACHED_PROCESS | CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
  if (!ok) {
    STFATAL << "Failed to start htmd: " << GetLastError();
  }

  if (!childPidFile.empty()) {
    std::ofstream pidFile(childPidFile.c_str());
    if (pidFile) {
      pidFile << pi.dwProcessId << "\n";
    }
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  if (parentExit) {
    exit(EXIT_SUCCESS);
  }
  return PARENT;
}
#else
int DaemonCreator::createSessionLeader() { return ::daemon(0, 0); }

int DaemonCreator::create(bool parentExit, string childPidFile) {
  pid_t pid;

  /* Fork off the parent process */
  pid = fork();

  /* An error occurred */
  if (pid < 0) exit(EXIT_FAILURE);

  /* Success: Return so the parent can continue */
  if (pid > 0) {
    if (parentExit) {
      exit(EXIT_SUCCESS);
    }
    return PARENT;
  }

  /* On success: The child process becomes session leader */
  if (setsid() < 0) exit(EXIT_FAILURE);

  /* Catch, ignore and handle signals */
  signal(SIGHUP, SIG_IGN);

  /* Fork off for the second time*/
  pid = fork();

  /* An error occurred */
  if (pid < 0) exit(EXIT_FAILURE);

  /* Success: Let the parent terminate */
  if (pid > 0) exit(EXIT_SUCCESS);

  /* Child process, write pid file */
  if (childPidFile != "") {
    int pidFilehandle = open(childPidFile.c_str(), O_RDWR | O_CREAT, 0600);
    if (pidFilehandle == -1) {
      STFATAL << "Error opening pidfile for writing: " << childPidFile;
    }

    // Max pid length for x86_64 is 2^22 ~ 4000000
    std::stringstream pid_ss;
    pid_ss << getpid() << "\n";
    std::string pid_str = pid_ss.str();
    ssize_t bytesWritten =
        write(pidFilehandle, pid_str.c_str(), pid_str.length());
    FATAL_FAIL(bytesWritten);
    if (static_cast<size_t>(bytesWritten) != pid_str.length()) {
      STFATAL << "Error writing pidfile: " << childPidFile;
    }
    FATAL_FAIL(close(pidFilehandle));
  }

  /* Change the working directory to the root directory */
  /* or another appropriated directory */
  FATAL_FAIL(chdir("/"));

  auto fd = open("/dev/null", O_WRONLY | O_CREAT, 0666);
  dup2(fd, STDOUT_FILENO);
  dup2(fd, STDERR_FILENO);

  auto fd2 = open("/dev/null", O_RDONLY);
  dup2(fd2, STDIN_FILENO);

  return CHILD;
}
#endif
}  // namespace et
