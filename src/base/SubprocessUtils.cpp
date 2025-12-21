#include "SubprocessUtils.hpp"

namespace et {
#ifdef WIN32
#define BUFSIZE 4096

void ErrorExit(PTSTR);

string SubprocessUtils::SubprocessToStringInteractive(
    const string& command, const vector<string>& args) {
  SECURITY_ATTRIBUTES saAttr;
  HANDLE g_hChildStd_IN_Rd = NULL;
  HANDLE g_hChildStd_IN_Wr = NULL;
  HANDLE g_hChildStd_OUT_Rd = NULL;
  HANDLE g_hChildStd_OUT_Wr = NULL;

  // Set the bInheritHandle flag so pipe handles are inherited.

  saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
  saAttr.bInheritHandle = TRUE;
  saAttr.lpSecurityDescriptor = NULL;

  // Create a pipe for the child process's STDOUT.

  if (!CreatePipe(&g_hChildStd_OUT_Rd, &g_hChildStd_OUT_Wr, &saAttr, 0))
    ErrorExit(TEXT("StdoutRd CreatePipe"));

  // Ensure the read handle to the pipe for STDOUT is not inherited.

  if (!SetHandleInformation(g_hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0))
    ErrorExit(TEXT("Stdout SetHandleInformation"));

  // Create a pipe for the child process's STDIN.

  if (!CreatePipe(&g_hChildStd_IN_Rd, &g_hChildStd_IN_Wr, &saAttr, 0))
    ErrorExit(TEXT("Stdin CreatePipe"));

  // Ensure the write handle to the pipe for STDIN is not inherited.

  if (!SetHandleInformation(g_hChildStd_IN_Wr, HANDLE_FLAG_INHERIT, 0))
    ErrorExit(TEXT("Stdin SetHandleInformation"));

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
  siStartInfo.hStdError = g_hChildStd_OUT_Wr;
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

  // If an error occurs, exit the application.
  if (!bSuccess)
    ErrorExit(TEXT("CreateProcess"));
  else {
    // Close handles to the child process and its primary thread.
    // Some applications might keep these handles to monitor the status
    // of the child process, for example.

    CloseHandle(piProcInfo.hProcess);
    CloseHandle(piProcInfo.hThread);

    // Close handles to the stdin and stdout pipes no longer needed by the child
    // process. If they are not explicitly closed, there is no way to recognize
    // that the child process has ended.

    CloseHandle(g_hChildStd_OUT_Wr);
    CloseHandle(g_hChildStd_IN_Rd);
  }

  // Read from pipe that is the standard output for child process.
  // Read output from the child process's pipe for STDOUT
  // and write to the parent process's pipe for STDOUT.
  // Stop when there is no more data.
  DWORD dwRead, dwWritten;
  CHAR chBuf[BUFSIZE];
  bSuccess = FALSE;
  HANDLE hParentStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
  string childOutput = "";

  for (;;) {
    bSuccess = ReadFile(g_hChildStd_OUT_Rd, chBuf, BUFSIZE, &dwRead, NULL);
    if (!bSuccess || dwRead == 0) break;

    childOutput += string((const char*)chBuf, (size_t)dwRead);
  }

  // The remaining open handles are cleaned up when this process terminates.
  // To avoid resource leaks in a larger application, close handles explicitly.

  return childOutput;
}

#include <stdio.h>
#include <strsafe.h>
#include <tchar.h>
#include <windows.h>

void ErrorExit(PTSTR lpszFunction)

// Format a readable error message, display a message box,
// and exit from the application.
{
  LPVOID lpMsgBuf;
  LPVOID lpDisplayBuf;
  DWORD dw = GetLastError();

  FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                    FORMAT_MESSAGE_IGNORE_INSERTS,
                NULL, dw, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                (LPTSTR)&lpMsgBuf, 0, NULL);

  lpDisplayBuf = (LPVOID)LocalAlloc(
      LMEM_ZEROINIT,
      (lstrlen((LPCTSTR)lpMsgBuf) + lstrlen((LPCTSTR)lpszFunction) + 40) *
          sizeof(TCHAR));
  StringCchPrintf((LPTSTR)lpDisplayBuf, LocalSize(lpDisplayBuf) / sizeof(TCHAR),
                  TEXT("%s failed with error %d: %s"), lpszFunction, dw,
                  lpMsgBuf);
  MessageBox(NULL, (LPCTSTR)lpDisplayBuf, TEXT("Error"), MB_OK);

  LocalFree(lpMsgBuf);
  LocalFree(lpDisplayBuf);
  ExitProcess(1);
}
#else
string SubprocessUtils::SubprocessToStringInteractive(
    const string& command, const vector<string>& args) {
  int link_client[2];
  char buf_client[4096];
  if (pipe(link_client) == -1) {
    STFATAL << "pipe";
    exit(1);
  }

  pid_t pid = fork();
  if (pid == 0) {
    // child process
    dup2(link_client[1], STDOUT_FILENO);
    close(link_client[0]);
    close(link_client[1]);

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
    close(link_client[1]);
    wait(NULL);
    string sshBuffer;
    while (true) {
      int nbytes = read(link_client[0], buf_client, sizeof(buf_client));
      if (nbytes <= 0) {
        break;
      }
      sshBuffer += string(buf_client, nbytes);
    }
    return sshBuffer;
  } else {
    LOG(INFO) << "Failed to fork";
    exit(1);
  }
}
#endif

}  // namespace et
