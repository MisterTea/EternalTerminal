#pragma once

// ust_windows.hpp - Windows stacktrace implementation (MSVC + MinGW)
// Debug info requirements for filename/lineno symbolication:
//   MSVC: compile with /Zi and link with /DEBUG (CMake RelWithDebInfo does this)
//   MinGW: compile with -g -ggdb3 -fno-omit-frame-pointer (CMake enables -g -ggdb3 on UNIX)
// This header ensures the necessary libs are linked via pragma.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef _MSC_VER
#include <DbgHelp.h>
#include <shlwapi.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "shlwapi.lib")
// Ensure PDB/debug info is retained for Sym* APIs.
// MSVC: /Zi is required; CMake should enable it for RelWithDebInfo.
// No additional flags can be set from header, but pragma ensures linking.
#else
// MinGW (Windows GCC/Clang) does not ship the Windows SDK's DbgHelp; use
// CaptureStackBackTrace + addr2line symbolication instead.
#include <array>
#include <cstdio>
#include <libgen.h>
#include <sys/stat.h>
#if !defined(_MSC_VER) && (defined(__MINGW32__) || defined(__MINGW64__))
#include <psapi.h>
#endif
#if defined(__MINGW32__) || defined(__MINGW64__)
#define WEXITSTATUS(w) (((w) >> 8) & 0xff)
#else
#include <sys/wait.h>
#endif
#endif

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#ifndef UST_NOINLINE
#if defined(_MSC_VER)
#define UST_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define UST_NOINLINE __attribute__((noinline))
#else
#define UST_NOINLINE
#endif
#endif

namespace ust {

#ifdef _MSC_VER
// Fast raw capture without DbgHelp symbolization. Used for STERROR.
UST_NOINLINE inline StackTrace generate_raw() {
  std::vector<StackTraceEntry> stackTrace;
  void* stack[MAX_STACK_FRAMES];
  // Skip this generate_raw() frame itself (skip 1)
  USHORT frames = CaptureStackBackTrace(1, MAX_STACK_FRAMES, stack, NULL);
  for (USHORT i = 0; i < frames; ++i) {
    DWORD64 address = (DWORD64)(stack[i]);
    std::string binaryFileName;
    HMODULE module = nullptr;
    char path[4096];
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<const char*>(stack[i]), &module)) {
      const auto length = GetModuleFileNameA(module, path, sizeof(path));
      if (length > 0 && length < sizeof(path)) {
        binaryFileName = std::string(path);
        std::replace(binaryFileName.begin(), binaryFileName.end(), '\\', '/');
      }
    }
    stackTrace.push_back(StackTraceEntry(
        (int)stackTrace.size(), addressToString(address), binaryFileName, "",
        "", -1));
    if ((int)stackTrace.size() >= MAX_STACK_FRAMES) break;
  }
  return StackTrace(stackTrace);
}

// Visual studio uses CaptureStackBackTrace + DbgHelp to get stack trace info
UST_NOINLINE inline StackTrace generate() {
  static std::mutex mtx;
  std::lock_guard<std::mutex> lock(mtx);
  std::vector<StackTraceEntry> stackTrace;
  HANDLE process = GetCurrentProcess();

  // Initialize DbgHelp once per process (thread-safe)
  static std::once_flag symInitFlag;
  std::call_once(symInitFlag, [&]() {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(process, NULL, TRUE);
  });

  void* stack[MAX_STACK_FRAMES];
  // Skip this generate() frame itself (skip 1)
  USHORT frames = CaptureStackBackTrace(1, MAX_STACK_FRAMES, stack, NULL);

  for (USHORT i = 0; i < frames; ++i) {
    DWORD64 address = (DWORD64)(stack[i]);

    std::string functionName;
    std::string binaryFileName;
    std::string sourceFileName;
    int lineNumber = -1;

    // Symbol resolution
    const int cnBufferSize = 4096;
    unsigned char byBuffer[sizeof(SYMBOL_INFO) + cnBufferSize * sizeof(char)];
    SYMBOL_INFO* pSymbol = (SYMBOL_INFO*)byBuffer;
    memset(pSymbol, 0, sizeof(SYMBOL_INFO) + cnBufferSize * sizeof(char));
    pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    pSymbol->MaxNameLen = cnBufferSize;
    DWORD64 displacement = 0;
    if (SymFromAddr(process, address, &displacement, pSymbol)) {
      functionName = std::string(pSymbol->Name);
    }

    // Source file & line
    IMAGEHLP_LINE64 theLine;
    memset(&theLine, 0, sizeof(theLine));
    theLine.SizeOfStruct = sizeof(theLine);
    DWORD displacement32 = 0;
    if (SymGetLineFromAddr64(process, address, &displacement32, &theLine)) {
      sourceFileName = std::string(theLine.FileName);
      lineNumber = int(theLine.LineNumber);
    }

    // Module / binary file name
    IMAGEHLP_MODULE64 moduleInfo;
    memset(&moduleInfo, 0, sizeof(moduleInfo));
    moduleInfo.SizeOfStruct = sizeof(moduleInfo);
    if (SymGetModuleInfo64(process, address, &moduleInfo)) {
      binaryFileName = std::string(moduleInfo.ImageName);
      std::replace(binaryFileName.begin(), binaryFileName.end(), '\\', '/');
    }

    stackTrace.push_back(StackTraceEntry(
        (int)stackTrace.size(), addressToString(address), binaryFileName,
        functionName, sourceFileName, lineNumber));

    if ((int)stackTrace.size() >= MAX_STACK_FRAMES) break;
  }

  // Keep Sym initialized for caching; do not call SymCleanup every time
  // to avoid repeated load overhead. SymCleanup will be done on process exit.
  // If you need to cleanup, uncomment:
  // SymCleanup(process);

  return StackTrace(stackTrace);
}
#else
// MinGW (Windows but using GCC/Clang toolchain) - uses CaptureStackBackTrace + addr2line

inline std::string SystemToStr(const char* cmd) {
  std::array<char, 128> buffer;
  std::string result;
  FILE* pipe = popen(cmd, "r");
  if (!pipe) {
    throw std::runtime_error("popen() failed!");
  }
  while (!feof(pipe)) {
    if (fgets(buffer.data(), 128, pipe) != nullptr) result += buffer.data();
  }
  auto closeValue = pclose(pipe);
  auto exitCode = WEXITSTATUS(closeValue);
  if (exitCode) {
    return "";
  }
  return result;
}

// PE debug information records addresses relative to the image's preferred
// link-time base, while CaptureStackBackTrace returns addresses relative to
// the image's (possibly ASLR-relocated) load base.  Translate between the two
// before invoking GNU addr2line.
inline uint64_t addr2lineAddress(void* address, HMODULE moduleHandle,
                                 const std::string& fileName) {
  if (!address || !moduleHandle) return uint64_t(address);

  // The Windows loader rewrites OptionalHeader.ImageBase in the in-memory PE
  // header after applying ASLR, so read the preferred base from the file.
  IMAGE_DOS_HEADER dosHeader{};
  IMAGE_NT_HEADERS ntHeaders{};
  std::ifstream image(fileName, std::ios::binary);
  image.read(reinterpret_cast<char*>(&dosHeader), sizeof(dosHeader));
  if (!image || dosHeader.e_magic != IMAGE_DOS_SIGNATURE) {
    return uint64_t(address);
  }
  image.seekg(dosHeader.e_lfanew);
  image.read(reinterpret_cast<char*>(&ntHeaders), sizeof(ntHeaders));
  if (!image || ntHeaders.Signature != IMAGE_NT_SIGNATURE) {
    return uint64_t(address);
  }

  const uint64_t loadedBase = reinterpret_cast<uint64_t>(moduleHandle);
  const uint64_t absoluteAddress = reinterpret_cast<uint64_t>(address);
  if (absoluteAddress < loadedBase) return absoluteAddress;
  return uint64_t(ntHeaders.OptionalHeader.ImageBase) +
         (absoluteAddress - loadedBase);
}

UST_NOINLINE inline StackTrace generate_raw() {
  std::vector<StackTraceEntry> stackTrace;
  void* stack[MAX_STACK_FRAMES];
  int numFrames = CaptureStackBackTrace(1, MAX_STACK_FRAMES, stack, NULL);
  for (int a = 0; a < numFrames; a++) {
    HMODULE moduleHandle;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (const char*)stack[a], &moduleHandle);
    std::string fileName(4096, '\0');
    auto fileNameSize =
        GetModuleFileNameA(moduleHandle, &fileName[0], fileName.size());
    if (fileNameSize == 0 || fileNameSize == (ssize_t)fileName.size()) {
      fileName = "";
    } else {
      fileName = fileName.substr(0, fileNameSize);
      std::replace(fileName.begin(), fileName.end(), '\\', '/');
    }
    std::string addr = addressToString(uint64_t(stack[a]));
    StackTraceEntry entry(a, addr, fileName, "", "", -1);
    stackTrace.push_back(entry);
  }
  return StackTrace(stackTrace);
}

UST_NOINLINE inline StackTrace generate() {
#if defined(__MINGW32__) && !defined(_GLIBCXX_HAS_GTHREADS)
  // MinGW win32 thread model has no std::mutex / std::call_once - use CRITICAL_SECTION
  static CRITICAL_SECTION cs;
  static bool cs_init = false;
  if (!cs_init) {
    InitializeCriticalSection(&cs);
    cs_init = true;
  }
  struct CsGuard {
    CRITICAL_SECTION* c;
    CsGuard(CRITICAL_SECTION* cs_) : c(cs_) { EnterCriticalSection(c); }
    ~CsGuard() { LeaveCriticalSection(c); }
  } guard(&cs);
#else
  static std::mutex mtx;
  std::lock_guard<std::mutex> lock(mtx);
#endif

  std::vector<StackTraceEntry> stackTrace;
  void* stack[MAX_STACK_FRAMES];
  int numFrames = CaptureStackBackTrace(1, MAX_STACK_FRAMES, stack, NULL);
  for (int a = 0; a < numFrames; a++) {
    HMODULE moduleHandle;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (const char*)stack[a], &moduleHandle);
    std::string fileName(4096, '\0');
    auto fileNameSize =
        GetModuleFileNameA(moduleHandle, &fileName[0], fileName.size());
    if (fileNameSize == 0 || fileNameSize == (ssize_t)fileName.size()) {
      fileName = "";
    } else {
      fileName = fileName.substr(0, fileNameSize);
      std::replace(fileName.begin(), fileName.end(), '\\', '/');
    }
    std::string addr = addressToString(uint64_t(stack[a]));
    StackTraceEntry entry(a, addr, fileName, "", "", -1);
    stackTrace.push_back(entry);
  }

  // Fetch source file & line numbers via addr2line (same as Unix)
  std::map<std::string, std::list<std::string>> fileAddresses;
  std::map<std::string, std::list<std::string>> fileData;
  for (int a = 0; a < numFrames; a++) {
    HMODULE moduleHandle;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (const char*)stack[a], &moduleHandle);
    std::string fileName(4096, '\0');
    auto fileNameSize =
        GetModuleFileNameA(moduleHandle, &fileName[0], fileName.size());
    if (fileNameSize == 0 || fileNameSize == (ssize_t)fileName.size()) {
      fileName = "";
    } else {
      fileName = fileName.substr(0, fileNameSize);
      std::replace(fileName.begin(), fileName.end(), '\\', '/');
    }
    std::string addrStr =
        addressToString(addr2lineAddress(stack[a], moduleHandle, fileName));
    if (fileName.length()) {
      if (fileAddresses.find(fileName) == fileAddresses.end()) {
        fileAddresses[fileName] = {};
      }
      fileAddresses.at(fileName).push_back(addrStr);
    }
  }
  for (const auto& it : fileAddresses) {
    std::string fileName = it.first;
    std::ostringstream ss;
    ss << "addr2line -C -f -p -e " << fileName << " ";
    for (const auto& it2 : it.second) {
      ss << it2 << " ";
    }
    auto addrLineOutput = SystemToStr(ss.str().c_str());
    if (addrLineOutput.length()) {
      auto outputLines = split(addrLineOutput, '\n');
      fileData[fileName] =
          std::list<std::string>(outputLines.begin(), outputLines.end());
    }
  }
  std::regex addrToLineRegex("^(.+?) at (.+):([0-9]+)");
  for (auto& it : stackTrace) {
    if (it.binaryFileName.length() &&
        fileData.find(it.binaryFileName) != fileData.end()) {
      std::string outputLine = fileData.at(it.binaryFileName).front();
      fileData.at(it.binaryFileName).pop_front();
      if (outputLine == std::string("?? ??:0")) {
        continue;
      }
      std::smatch matches;
      if (regex_search(outputLine, matches, addrToLineRegex)) {
        it.functionName = matches[1];
        it.sourceFileName = matches[2];
        it.lineNumber = std::stoi(matches[3]);
      }
    }
  }

  return StackTrace(stackTrace);
}
#endif

}  // namespace ust
