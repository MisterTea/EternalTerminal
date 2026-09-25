#include "SubprocessUtils.hpp"

namespace et {
#ifdef WIN32
#define BUFSIZE 4096

[[noreturn]] void ThrowWindowsError(const char* function);

string SubprocessUtils::SubprocessToStringInteractive(
    const string& command, const vector<string>& args) {
  SECURITY_ATTRIBUTES saAttr;
  HANDLE g_hChildStd_IN_Rd = NULL;
  HANDLE g_hChildStd_IN_Wr = NULL;
  HANDLE g_hChildStd_OUT_Rd = NULL;
  HANDLE g_hChildStd_OUT_Wr = NULL;
  HANDLE g_hChildStd_ERR_Rd = NULL;
  HANDLE g_hChildStd_ERR_Wr = NULL;

  // Set the bInheritHandle flag so pipe handles are inherited.

  saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
  saAttr.bInheritHandle = TRUE;
  saAttr.lpSecurityDescriptor = NULL;

  // Create a pipe for the child process's STDOUT.

  if (!CreatePipe(&g_hChildStd_OUT_Rd, &g_hChildStd_OUT_Wr, &saAttr, 0))
    ThrowWindowsError("StdoutRd CreatePipe");

  // Ensure the read handle to the pipe for STDOUT is not inherited.

  if (!SetHandleInformation(g_hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0))
    ThrowWindowsError("Stdout SetHandleInformation");

  // Create a pipe for the child process's STDERR (streamed live; not mixed
  // into the captured stdout credential buffer).

  if (!CreatePipe(&g_hChildStd_ERR_Rd, &g_hChildStd_ERR_Wr, &saAttr, 0))
    ThrowWindowsError("StderrRd CreatePipe");

  if (!SetHandleInformation(g_hChildStd_ERR_Rd, HANDLE_FLAG_INHERIT, 0))
    ThrowWindowsError("Stderr SetHandleInformation");

  // Create a pipe for the child process's STDIN.

  if (!CreatePipe(&g_hChildStd_IN_Rd, &g_hChildStd_IN_Wr, &saAttr, 0))
    ThrowWindowsError("Stdin CreatePipe");

  // Ensure the write handle to the pipe for STDIN is not inherited.

  if (!SetHandleInformation(g_hChildStd_IN_Wr, HANDLE_FLAG_INHERIT, 0))
    ThrowWindowsError("Stdin SetHandleInformation");

  // Create a child process that uses the previously created pipes for STDIN and
  // STDOUT.
  string localCommand = command;
  for (auto arg : args) {
    localCommand += " ";
    localCommand += arg;
  }
  PROCESS_INFORMATION piProcInfo;
  STARTUPINFO siStartInfo;
  BOOL bSuccess = FALSE;

  // Set up members of the PROCESS_INFORMATION structure.

  ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));

  // Set up members of the STARTUPINFO structure.
  // This structure specifies the STDIN and STDOUT handles for redirection.

  ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));
  siStartInfo.cb = sizeof(STARTUPINFO);
  siStartInfo.hStdError = g_hChildStd_ERR_Wr;
  siStartInfo.hStdOutput = g_hChildStd_OUT_Wr;
  siStartInfo.hStdInput = g_hChildStd_IN_Rd;
  siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

  // Create the child process.

  std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
  std::wstring wide = converter.from_bytes(localCommand);

  bSuccess = CreateProcess(NULL,
                           &(wide[0]),    // command line
                           NULL,          // process security attributes
                           NULL,          // primary thread security attributes
                           TRUE,          // handles are inherited
                           0,             // creation flags
                           NULL,          // use parent's environment
                           NULL,          // use parent's current directory
                           &siStartInfo,  // STARTUPINFO pointer
                           &piProcInfo);  // receives PROCESS_INFORMATION

  if (!bSuccess) {
    CloseHandle(g_hChildStd_OUT_Rd);
    CloseHandle(g_hChildStd_OUT_Wr);
    CloseHandle(g_hChildStd_ERR_Rd);
    CloseHandle(g_hChildStd_ERR_Wr);
    CloseHandle(g_hChildStd_IN_Rd);
    CloseHandle(g_hChildStd_IN_Wr);
    ThrowWindowsError("CreateProcess");
  } else {
    // Close handles to the stdin and stdout pipes no longer needed by the child
    // process. If they are not explicitly closed, there is no way to recognize
    // that the child process has ended.

    CloseHandle(g_hChildStd_OUT_Wr);
    CloseHandle(g_hChildStd_ERR_Wr);
    CloseHandle(g_hChildStd_IN_Rd);
    // This API captures output but does not currently provide input. Closing
    // the remaining parent write end ensures programs waiting on stdin see
    // EOF rather than hanging forever.
    CloseHandle(g_hChildStd_IN_Wr);
  }

  // Drain stdout (captured) and stderr (streamed live) before waiting so a
  // chatty child cannot fill a pipe and deadlock against WaitForSingleObject.
  DWORD dwRead;
  CHAR chBuf[BUFSIZE];
  string childOutput = "";
  HANDLE parentStderr = GetStdHandle(STD_ERROR_HANDLE);
  bool stdoutOpen = true;
  bool stderrOpen = true;

  while (stdoutOpen || stderrOpen) {
    bool drained = false;

    auto tryRead = [&](HANDLE src, bool* openFlag, bool isStderr) {
      DWORD available = 0;
      if (!PeekNamedPipe(src, NULL, 0, NULL, &available, NULL)) {
        *openFlag = false;
        return;
      }
      if (available == 0) {
        return;
      }
      drained = true;
      DWORD toRead = (available < BUFSIZE) ? available : BUFSIZE;
      bSuccess = ReadFile(src, chBuf, toRead, &dwRead, NULL);
      if (!bSuccess || dwRead == 0) {
        *openFlag = false;
        return;
      }
      if (isStderr) {
        if (parentStderr != NULL && parentStderr != INVALID_HANDLE_VALUE) {
          DWORD written = 0;
          WriteFile(parentStderr, chBuf, dwRead, &written, NULL);
        }
      } else {
        childOutput += string((const char*)chBuf, (size_t)dwRead);
      }
    };

    if (stdoutOpen) {
      tryRead(g_hChildStd_OUT_Rd, &stdoutOpen, false);
    }
    if (stderrOpen) {
      tryRead(g_hChildStd_ERR_Rd, &stderrOpen, true);
    }

    if (!drained) {
      // No data right now. If the child has already exited, a successful peek
      // with zero bytes means that pipe has reached EOF.
      if (WaitForSingleObject(piProcInfo.hProcess, 10) == WAIT_OBJECT_0) {
        DWORD remainingOut = 1;
        DWORD remainingErr = 1;
        if (stdoutOpen &&
            PeekNamedPipe(g_hChildStd_OUT_Rd, NULL, 0, NULL, &remainingOut,
                          NULL) &&
            remainingOut == 0) {
          stdoutOpen = false;
        }
        if (stderrOpen &&
            PeekNamedPipe(g_hChildStd_ERR_Rd, NULL, 0, NULL, &remainingErr,
                          NULL) &&
            remainingErr == 0) {
          stderrOpen = false;
        }
      }
    }
  }

  CloseHandle(g_hChildStd_OUT_Rd);
  CloseHandle(g_hChildStd_ERR_Rd);
  WaitForSingleObject(piProcInfo.hProcess, INFINITE);
  CloseHandle(piProcInfo.hThread);
  CloseHandle(piProcInfo.hProcess);

  // The remaining open handles are cleaned up when this process terminates.
  // To avoid resource leaks in a larger application, close handles explicitly.

  return childOutput;
}

[[noreturn]] void ThrowWindowsError(const char* function) {
  DWORD dw = GetLastError();
  char* message = nullptr;
  FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 NULL, dw, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                 reinterpret_cast<char*>(&message), 0, NULL);
  string detail = message ? string(message) : string("Unknown error");
  if (message) {
    LocalFree(message);
  }
  throw runtime_error(string(function) + " failed with error " + to_string(dw) +
                      ": " + detail);
}
#else
string SubprocessUtils::SubprocessToStringInteractive(
    const string& command, const vector<string>& args) {
  int stdout_pipe[2];
  int stderr_pipe[2];
  char buf[4096];
  if (pipe(stdout_pipe) == -1) {
    STFATAL << "pipe";
    exit(1);
  }
  if (pipe(stderr_pipe) == -1) {
    STFATAL << "pipe";
    exit(1);
  }

  pid_t pid = fork();
  if (pid == 0) {
    // child process: keep stdout and stderr on separate pipes so the parent
    // can capture credentials without streaming them, while still showing
    // SSH_MSG_USERAUTH_BANNER text live on the parent's stderr.
    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stderr_pipe[1], STDERR_FILENO);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);

    char** argsArray = new char*[args.size() + 2];
    argsArray[0] = strdup(command.c_str());
    for (int a = 0; a < args.size(); a++) {
      argsArray[a + 1] = strdup(args[a].c_str());
    }
    argsArray[args.size() + 1] = NULL;
    execvp(command.c_str(), argsArray);

    LOG(INFO) << "execvp error";
    for (int a = 0; a <= args.size(); a++) {
      free(argsArray[a]);
    }
    delete[] argsArray;
    exit(1);
  } else if (pid > 0) {
    // parent process
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    string stdoutBuffer;
    bool stdoutOpen = true;
    bool stderrOpen = true;
    while (stdoutOpen || stderrOpen) {
      struct pollfd fds[2];
      nfds_t nfds = 0;
      int stdoutIdx = -1;
      int stderrIdx = -1;
      if (stdoutOpen) {
        stdoutIdx = static_cast<int>(nfds);
        fds[nfds].fd = stdout_pipe[0];
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;
      }
      if (stderrOpen) {
        stderrIdx = static_cast<int>(nfds);
        fds[nfds].fd = stderr_pipe[0];
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;
      }

      int ready = poll(fds, nfds, -1);
      if (ready == -1) {
        if (errno == EINTR) {
          continue;
        }
        STFATAL << "poll";
        exit(1);
      }

      auto drainFd = [&](int fd, bool* openFlag, bool isStderr) {
        while (true) {
          int nbytes = read(fd, buf, sizeof(buf));
          if (nbytes < 0) {
            if (errno == EINTR) {
              continue;
            }
            *openFlag = false;
            return;
          }
          if (nbytes == 0) {
            *openFlag = false;
            return;
          }
          if (isStderr) {
            const char* cursor = buf;
            size_t remaining = static_cast<size_t>(nbytes);
            while (remaining > 0) {
              ssize_t written = write(STDERR_FILENO, cursor, remaining);
              if (written < 0) {
                if (errno == EINTR) {
                  continue;
                }
                // Best-effort live tee; keep draining so the child cannot
                // deadlock even if the parent's stderr is unavailable.
                break;
              }
              cursor += written;
              remaining -= static_cast<size_t>(written);
            }
          } else {
            stdoutBuffer.append(buf, static_cast<size_t>(nbytes));
          }
          // Read once per poll readiness notification; additional data will
          // wake poll again. Avoid busy-spinning on a non-blocking fd.
          return;
        }
      };

      if (stdoutIdx >= 0 &&
          (fds[stdoutIdx].revents & (POLLIN | POLLHUP | POLLERR))) {
        drainFd(stdout_pipe[0], &stdoutOpen, false);
      }
      if (stderrIdx >= 0 &&
          (fds[stderrIdx].revents & (POLLIN | POLLHUP | POLLERR))) {
        drainFd(stderr_pipe[0], &stderrOpen, true);
      }
    }

    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
    }
    return stdoutBuffer;
  } else {
    LOG(INFO) << "Failed to fork";
    exit(1);
  }
}
#endif

}  // namespace et
