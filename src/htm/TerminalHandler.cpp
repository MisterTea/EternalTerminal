#include "TerminalHandler.hpp"

#include "ETerminal.pb.h"
#include "RawSocketUtils.hpp"
#include "ServerConnection.hpp"
#include "UserTerminalRouter.hpp"

namespace et {
TerminalHandler::TerminalHandler() : run(true), bufferLength(0) {}

void TerminalHandler::start() {
#ifdef WIN32

  FATAL_FAIL_IF_ZERO(CreatePipe(&inputReadSide, &inputWriteSide, NULL, 0));
  FATAL_FAIL_IF_ZERO(CreatePipe(&outputReadSide, &outputWriteSide, NULL, 0));

  COORD c = {80, 24};
  FATAL_FAIL_UNLESS_S_OK(
      CreatePseudoConsole(c, inputReadSide, outputWriteSide, 0, &hPC));

  // Prepare Startup Information structure
  STARTUPINFOEX si;
  ZeroMemory(&si, sizeof(si));
  si.StartupInfo.cb = sizeof(STARTUPINFOEX);

  // Discover the size required for the list
  size_t bytesRequired;
  InitializeProcThreadAttributeList(NULL, 1, 0, &bytesRequired);

  // Allocate memory to represent the list
  si.lpAttributeList = (PPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(),
                                                              0, bytesRequired);
  FATAL_FAIL_IF_ZERO(si.lpAttributeList);

  // Initialize the list memory location
  FATAL_FAIL_IF_ZERO(InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0,
                                                       &bytesRequired));

  // Set the pseudoconsole information into the list
  FATAL_FAIL_IF_ZERO(UpdateProcThreadAttribute(
      si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPC,
      sizeof(hPC), NULL, NULL));

  PCWSTR childApplication = L"C:\\windows\\system32\\cmd.exe";

  // Create mutable text string for CreateProcessW command line string.
  const size_t charsRequired =
      wcslen(childApplication) + 1;  // +1 null terminator
  PWSTR cmdLineMutable =
      (PWSTR)HeapAlloc(GetProcessHeap(), 0, sizeof(wchar_t) * charsRequired);

  FATAL_FAIL_IF_ZERO(cmdLineMutable);

  wcscpy_s(cmdLineMutable, charsRequired, childApplication);

  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));

  // Call CreateProcess
  FATAL_FAIL_IF_ZERO(CreateProcessW(NULL, cmdLineMutable, NULL, NULL, FALSE,
                                    EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                                    &si.StartupInfo, &pi));

  readThread.reset(new thread(&TerminalHandler::beginRead, this));
#else
  pid_t pid = forkpty(&masterFd, NULL, NULL, NULL);
  switch (pid) {
    case -1:
      FATAL_FAIL(pid);
    case 0: {
      passwd *pwd = getpwuid(getuid());
      if (pwd == NULL) {
        LOG(FATAL)
            << "Not able to fork a terminal because getpwuid returns null";
      }
      chdir(pwd->pw_dir);
      string terminal = string(::getenv("SHELL"));
      setenv("HTM_VERSION", ET_VERSION, 1);
      execl(terminal.c_str(), terminal.c_str(), "-l", NULL);
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
#endif
}

#define BUF_SIZE (16 * 1024)

#ifdef WIN32
void TerminalHandler::beginRead() {
  char b[BUF_SIZE];
  while (run) {
    DWORD bytesRead;
    FATAL_FAIL_IF_ZERO(ReadFile(outputReadSide, b, BUF_SIZE, &bytesRead, NULL));
    if (bytesRead > 0) {
      scoped_lock<mutex> lock(readBufferMutex);
      readBuffer.append(b, bytesRead);
    }
  }
}
#endif

#define MAX_BUFFER_LINES (1024)
#define MAX_BUFFER_CHARS (128 * MAX_BUFFER_LINES)

string TerminalHandler::pollUserTerminal() {
  if (!run) {
    return string();
  }
  char b[BUF_SIZE];

#ifdef WIN32
  scoped_lock<mutex> lock(readBufferMutex);
  if (readBuffer.empty()) {
    return string();
  }
  if (readBuffer.size() > BUF_SIZE) {
    memcpy(b, readBuffer.c_str(), BUF_SIZE);
    readBuffer.erase(0, BUF_SIZE);
  } else {
    memcpy(b, readBuffer.c_str(), readBuffer.size());
    readBuffer.clear();
  }
  return b;
#else

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
        for (auto &it : tokens) {
          bufferLength += it.length();
        }
        if (buffer.empty()) {
          buffer.insert(buffer.end(), tokens.begin(), tokens.end());
        } else {
          buffer.back().append(tokens.front());
          if (tokens.size() > 1) {
            buffer.insert(buffer.end(), tokens.begin() + 1, tokens.end());
          }
        }
        if (buffer.size() > MAX_BUFFER_LINES) {
          int amountToErase = buffer.size() - MAX_BUFFER_LINES;
          for (auto it = buffer.begin();
               it != buffer.end() && it != (buffer.begin() + amountToErase);
               it++) {
            bufferLength -= it->length();
          }
          buffer.erase(buffer.begin(), buffer.begin() + amountToErase);
        }
        while (bufferLength > MAX_BUFFER_CHARS) {
          bufferLength -= buffer.begin()->length();
          buffer.pop_front();
        }
        LOG(INFO) << "BUFFER LINES: " << buffer.size() << " " << tokens.size()
                  << endl;
        return newChars;
      } else {
        LOG(INFO) << "Terminal session ended";
#if __NetBSD__
        // this unfortunateness seems to be fixed in NetBSD-8 (or at
        // least -CURRENT) sadness for now :/
        int throwaway;
        FATAL_FAIL(waitpid(childPid, &throwaway, WUNTRACED));
#else
        siginfo_t childInfo;
        int rc = waitid(P_PID, childPid, &childInfo, WEXITED);
        if (rc < 0 && GetErrno() != ECHILD) {
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
  } catch (const std::exception &ex) {
    LOG(INFO) << ex.what();
    run = false;
#ifdef WITH_UTEMPTER
    utempter_remove_record(masterFd);
#endif
  }

  return string();
#endif
}

void TerminalHandler::appendData(const string &data) {
#ifdef WIN32
  RawSocketUtils::writeAll(inputWriteSide, &data[0], data.length());
#else
  RawSocketUtils::writeAll(masterFd, &data[0], data.length());
#endif
}

void TerminalHandler::updateTerminalSize(int col, int row) {
#ifdef WIN32
  // Retrieve width and height dimensions of display in
  // characters using theoretical height/width functions
  // that can retrieve the properties from the display
  // attached to the event.
  COORD size;
  size.X = col;
  size.Y = row;

  // Call pseudoconsole API to inform buffer dimension update
  ResizePseudoConsole(hPC, size);
#else
  winsize tmpwin;
  tmpwin.ws_row = row;
  tmpwin.ws_col = col;
  tmpwin.ws_xpixel = 0;
  tmpwin.ws_ypixel = 0;
  ioctl(masterFd, TIOCSWINSZ, &tmpwin);
#endif
}

void TerminalHandler::stop() {
#ifdef WIN32
  ClosePseudoConsole(hPC);
#else
  kill(childPid, SIGKILL);
#endif
  run = false;
}

}  // namespace et
