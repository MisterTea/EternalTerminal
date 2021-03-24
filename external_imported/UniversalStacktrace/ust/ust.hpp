#pragma once

#ifdef _WIN32
#include <DbgHelp.h>
#include <windows.h>
#else
#include <cxxabi.h>
#include <errno.h>

#if __has_include(<libunwind.h>)
#define USE_UNWIND (1)
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#elif __has_include(<execinfo.h>)
#define USE_UNWIND (0)
#include <execinfo.h>
#else
#error Please install libunwind
#endif

#include <stdio.h>
#endif

#ifdef _MSC_VER
#include <shlwapi.h>
#else
#include <libgen.h>
#if defined(__MINGW32__) || defined(__MINGW64__)
#define WEXITSTATUS(w) (((w) >> 8) & 0xff)
#else
#include <sys/wait.h>
#endif
#endif

#ifdef __APPLE__
#include <unistd.h>
#endif

#ifdef __linux__
#include <linux/limits.h>
#endif

#include <array>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace ust {
template <typename Out>
inline void split(const std::string &s, char delim, Out result) {
  std::stringstream ss;
  ss.str(s);
  std::string item;
  while (std::getline(ss, item, delim)) {
    *(result++) = item;
  }
}

inline std::vector<std::string> split(const std::string &s, char delim) {
  std::vector<std::string> elems;
  split(s, delim, std::back_inserter(elems));
  return elems;
}

#ifndef _MSC_VER
// Needed for calling addr2line / atos
inline std::string SystemToStr(const char *cmd) {
  std::array<char, 128> buffer;
  std::string result;
  FILE *pipe = popen(cmd, "r");
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
#endif

#ifdef _MSC_VER
// Replacement for basename()
inline char *ustBasename(char *path) {
  PathStripPathA(path);
  return path;
}
inline std::string ustBasenameString(std::string input) {
  PathStripPathA(&input[0]);
  return input;
}
#else
inline char *ustBasename(char *path) { return ::basename(path); }
inline std::string ustBasenameString(std::string input) {
  input = std::string(::basename(&input[0]));
  return input;
}
#endif

inline std::string addressToString(uint64_t address) {
  std::ostringstream ss;
  ss << "0x" << std::hex << uint64_t(address);
  return ss.str();
}

static const int MAX_STACK_FRAMES = 64;
class StackTraceEntry {
 public:
  StackTraceEntry(int _stackIndex, const std::string &_address,
                  const std::string &_binaryFileName,
                  const std::string &_functionName,
                  const std::string &_sourceFileName, int _lineNumber)
      : stackIndex(_stackIndex),
        address(_address),
        binaryFileName(_binaryFileName),
        functionName(_functionName),
        sourceFileName(_sourceFileName),
        lineNumber(_lineNumber) {}

  int stackIndex;
  std::string address;
  std::string binaryFileName;
  std::string functionName;
  std::string sourceFileName;
  int lineNumber;

  friend std::ostream &operator<<(std::ostream &ss, const StackTraceEntry &si);

 private:
  StackTraceEntry(void);
};

inline std::ostream &operator<<(std::ostream &ss, const StackTraceEntry &si) {
  ss << "[" << si.stackIndex << "] " << si.address;
  if (!si.functionName.empty()) {
    ss << " " << si.functionName;
  }
  if (si.lineNumber > 0) {
    std::string sourceFileNameCopy = si.sourceFileName;
    ss << " (" << ustBasename(&sourceFileNameCopy[0]) << ":" << si.lineNumber
       << ")";
  }
  return ss;
}

class StackTrace {
 public:
  StackTrace(const std::vector<StackTraceEntry> &_entries)
      : entries(_entries) {}
  friend std::ostream &operator<<(std::ostream &ss, const StackTrace &si);

  std::vector<StackTraceEntry> entries;
};

inline std::ostream &operator<<(std::ostream &ss, const StackTrace &si) {
  for (const auto &it : si.entries) {
    ss << it << "\n";
  }
  return ss;
}

#ifdef _MSC_VER
// Visual studio uses StackWalker to get stack trace info
inline StackTrace generate() {
  std::vector<StackTraceEntry> stackTrace;
  HANDLE process = GetCurrentProcess();
  HANDLE thread = GetCurrentThread();

  CONTEXT context;
  memset(&context, 0, sizeof(CONTEXT));
  context.ContextFlags = CONTEXT_FULL;
  RtlCaptureContext(&context);

  SymSetOptions(SYMOPT_LOAD_LINES);
  SymInitialize(process, NULL, TRUE);

  DWORD image;
  STACKFRAME64 stackframe;
  ZeroMemory(&stackframe, sizeof(STACKFRAME64));

#ifdef _M_IX86
  image = IMAGE_FILE_MACHINE_I386;
  stackframe.AddrPC.Offset = context.Eip;
  stackframe.AddrPC.Mode = AddrModeFlat;
  stackframe.AddrFrame.Offset = context.Ebp;
  stackframe.AddrFrame.Mode = AddrModeFlat;
  stackframe.AddrStack.Offset = context.Esp;
  stackframe.AddrStack.Mode = AddrModeFlat;
#elif _M_X64
  image = IMAGE_FILE_MACHINE_AMD64;
  stackframe.AddrPC.Offset = context.Rip;
  stackframe.AddrPC.Mode = AddrModeFlat;
  stackframe.AddrFrame.Offset = context.Rsp;
  stackframe.AddrFrame.Mode = AddrModeFlat;
  stackframe.AddrStack.Offset = context.Rsp;
  stackframe.AddrStack.Mode = AddrModeFlat;
#elif _M_IA64
  image = IMAGE_FILE_MACHINE_IA64;
  stackframe.AddrPC.Offset = context.StIIP;
  stackframe.AddrPC.Mode = AddrModeFlat;
  stackframe.AddrFrame.Offset = context.IntSp;
  stackframe.AddrFrame.Mode = AddrModeFlat;
  stackframe.AddrBStore.Offset = context.RsBSP;
  stackframe.AddrBStore.Mode = AddrModeFlat;
  stackframe.AddrStack.Offset = context.IntSp;
  stackframe.AddrStack.Mode = AddrModeFlat;
#endif

  // Skip the first frame
  BOOL result = StackWalk64(image, process, thread, &stackframe, &context, NULL,
                            SymFunctionTableAccess64, SymGetModuleBase64, NULL);
  if (result) {
    for (int i = 0; i < MAX_STACK_FRAMES; i++) {
      BOOL result =
          StackWalk64(image, process, thread, &stackframe, &context, NULL,
                      SymFunctionTableAccess64, SymGetModuleBase64, NULL);

      if (!result) {
        break;
      }

      if (stackframe.AddrPC.Offset == stackframe.AddrReturn.Offset) break;

      const int cnBufferSize = 4096;
      unsigned char byBuffer[sizeof(IMAGEHLP_SYMBOL64) + cnBufferSize];
      IMAGEHLP_SYMBOL64 *pSymbol = (IMAGEHLP_SYMBOL64 *)byBuffer;
      memset(pSymbol, 0, sizeof(IMAGEHLP_SYMBOL64) + cnBufferSize);
      pSymbol->SizeOfStruct = sizeof(IMAGEHLP_SYMBOL64);
      pSymbol->MaxNameLength = cnBufferSize;

      std::string binaryFileName;
      std::string functionName;
      DWORD64 displacement = 0;
      if (SymGetSymFromAddr64(process, stackframe.AddrPC.Offset, &displacement,
                              pSymbol)) {
        functionName = std::string(pSymbol->Name);
      }

      DWORD displacement32 = 0;
      IMAGEHLP_LINE64 theLine;
      memset(&theLine, 0, sizeof(theLine));
      theLine.SizeOfStruct = sizeof(theLine);
      std::string sourceFileName;
      int lineNumber = -1;
      if (SymGetLineFromAddr64(process, stackframe.AddrPC.Offset,
                               &displacement32, &theLine)) {
        sourceFileName = std::string(theLine.FileName);
        lineNumber = int(theLine.LineNumber);
      }

      stackTrace.push_back(StackTraceEntry(
          i, addressToString(stackframe.AddrPC.Offset), binaryFileName,
          functionName, sourceFileName, lineNumber));
    }
  }

  SymCleanup(process);

  return StackTrace(stackTrace);
#else
// Non-visual studio compilers use a mess of things:
// Apple uses backtrace() + atos
// Linux uses backtrace() + addr2line
// MinGW uses CaptureStackBackTrace() + addr2line
inline StackTrace generate() {
  // Libunwind and some other functions aren't thread safe.
  static std::mutex mtx;
  std::lock_guard<std::mutex> lock(mtx);

  std::vector<StackTraceEntry> stackTrace;
  std::map<std::string, std::pair<uint64_t, uint64_t> > addressMaps;
  std::string line;
  std::string procMapFileName = std::string("/proc/self/maps");
  std::ifstream infile(procMapFileName.c_str());
  // Some OSes don't have /proc/*/maps, so we won't have base addresses for them
  while (std::getline(infile, line)) {
    std::istringstream iss(line);
    std::string addressRange;
    std::string perms;
    std::string offset;
    std::string device;
    std::string inode;
    std::string path;

    if (!(iss >> addressRange >> perms >> offset >> device >> inode >> path)) {
      break;
    }  // error
    uint64_t startAddress = stoull(split(addressRange, '-')[0], NULL, 16);
    uint64_t endAddress = stoull(split(addressRange, '-')[1], NULL, 16);
    if (addressMaps.find(path) == addressMaps.end()) {
      addressMaps[path] = std::make_pair(startAddress, endAddress);
    } else {
      addressMaps[path].first = std::min(addressMaps[path].first, startAddress);
      addressMaps[path].second = std::max(addressMaps[path].second, endAddress);
    }
  }

#if !USE_UNWIND
  void *stack[MAX_STACK_FRAMES];
  int numFrames;
#endif
#if defined(__MINGW32__) || defined(__MINGW64__)
  numFrames = CaptureStackBackTrace(1, MAX_STACK_FRAMES, stack, NULL);

  for (unsigned short a = 0; a < numFrames; a++) {
    HMODULE moduleHandle;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (const char *)stack[a], &moduleHandle);
    std::string fileName(4096, '\0');
    auto fileNameSize =
        GetModuleFileNameA(moduleHandle, &fileName[0], fileName.size());
    if (fileNameSize == 0 || fileNameSize == (ssize_t)fileName.size()) {
      /* Error, possibly not enough space. */
      fileName = "";
    } else {
      fileName = fileName.substr(0, fileNameSize);
      std::replace(fileName.begin(), fileName.end(), '\\', '/');
    }
    std::string addr = addressToString(uint64_t(stack[a]));
    StackTraceEntry entry(a, addr, fileName, "", "", -1);
    stackTrace.push_back(entry);
  }
#elif USE_UNWIND
  unw_context_t context;
  unw_getcontext(&context);

  unw_cursor_t cursor;
  unw_init_local(&cursor, &context);

  unw_word_t ip;

  for (int a = 0; unw_step(&cursor) > 0; a++) {
    unw_get_reg(&cursor, UNW_REG_IP, &ip);
    static const size_t kMax = 16 * 1024;
    char mangled[kMax];
    unw_word_t offset;
    unw_get_proc_name(&cursor, mangled, kMax, &offset);

    int ok;
    size_t len = kMax;
    char *demangled = abi::__cxa_demangle(mangled, 0, 0, &ok);

    std::string filename;
    uint64_t absoluteAddress = uint64_t(ip);
    uint64_t relativeAddress = 0;
    for (auto &it : addressMaps) {
      if (it.second.first <= absoluteAddress &&
          it.second.second > absoluteAddress) {
        filename = it.first;
        relativeAddress = absoluteAddress - it.second.first;
      }
    }

    StackTraceEntry entry(
        a,
        relativeAddress ? addressToString(relativeAddress)
                        : addressToString(absoluteAddress),
        filename, ok == 0 ? std::string(demangled) : std::string(mangled), "",
        -1);
    if (demangled) {
      free(demangled);
    }
    stackTrace.push_back(entry);
  }
#else
  numFrames = backtrace(stack, MAX_STACK_FRAMES);
  memmove(stack, stack + 1, sizeof(void *) * (numFrames - 1));
  numFrames--;

  char **strings = backtrace_symbols(stack, numFrames);
  if (strings) {
    for (int a = 0; a < numFrames; ++a) {
      std::string addr;
      std::string fileName;
      std::string functionName;

      const std::string line(strings[a]);
#ifdef __APPLE__
      // Example: ust-test                            0x000000010001e883
      // _ZNK5Catch21TestInvokerAsFunction6invokeEv + 19
      auto p = line.find("0x");
      if (p != std::string::npos) {
        addr = line.substr(p);
        auto spaceLoc = addr.find(" ");
        functionName = addr.substr(spaceLoc + 1);
        functionName = functionName.substr(0, functionName.find(" +"));
        addr = addr.substr(0, spaceLoc);
      }
#else
      // Example: ./ust-test(_ZNK5Catch21TestInvokerAsFunction6invokeEv+0x16)
      // [0x55f1278af96e]
      auto parenStart = line.find("(");
      auto parenEnd = line.find(")");
      fileName = line.substr(0, parenStart);
      // Convert filename to canonical path
      char buf[PATH_MAX];
      ::realpath(fileName.c_str(), buf);
      fileName = std::string(buf);
      functionName = line.substr(parenStart + 1, parenEnd - (parenStart + 1));
      // Strip off the offset from the name
      functionName = functionName.substr(0, functionName.find("+"));
      if (addressMaps.find(fileName) != addressMaps.end()) {
        // Make address relative to process start
        addr =
            addressToString(uint64_t(stack[a]) - addressMaps[fileName].first);
      } else {
        addr = addressToString(uint64_t(stack[a]));
      }
#endif
      // Perform demangling if parsed properly
      if (!functionName.empty()) {
        int status = 0;
        auto demangledFunctionName =
            abi::__cxa_demangle(functionName.data(), 0, 0, &status);
        // if demangling is successful, output the demangled function name
        if (status == 0) {
          // Success (see
          // http://gcc.gnu.org/onlinedocs/libstdc++/libstdc++-html-USERS-4.3/a01696.html)
          functionName = std::string(demangledFunctionName);
        }
        if (demangledFunctionName) {
          free(demangledFunctionName);
        }
      }
      StackTraceEntry entry(a, addr, fileName, functionName, "", -1);
      stackTrace.push_back(entry);
    }
    free(strings);
  }
#endif

// Fetch source file & line numbers
#ifdef __APPLE__
  std::ostringstream ss;
  ss << "atos -p " << std::to_string(getpid()) << " ";
  for (int a = 0; a < (int)stackTrace.size(); a++) {
    ss << stackTrace[a].address << " ";
  }
  auto atosOutput = SystemToStr(ss.str().c_str());
  if (atosOutput.length()) {
    auto atosLines = split(atosOutput, '\n');
    std::regex fileLineRegex("\\(([^\\(]+):([0-9]+)\\)");
    for (int a = 0; a < (int)stackTrace.size(); a++) {
      // Find the filename and line number
      std::smatch matches;
      if (regex_search(atosLines[a], matches, fileLineRegex)) {
        stackTrace[a].sourceFileName = matches[1];
        stackTrace[a].lineNumber = std::stoi(matches[2]);
      }
    }
  }
#else
  // Unix & MinGW
  std::map<std::string, std::list<std::string> > fileAddresses;
  std::map<std::string, std::list<std::string> > fileData;
  for (const auto &it : stackTrace) {
    if (it.binaryFileName.length()) {
      if (fileAddresses.find(it.binaryFileName) == fileAddresses.end()) {
        fileAddresses[it.binaryFileName] = {};
      }
      fileAddresses.at(it.binaryFileName).push_back(it.address);
    }
  }
  for (const auto &it : fileAddresses) {
    std::string fileName = it.first;
    std::ostringstream ss;
    ss << "addr2line -C -f -p -e " << fileName << " ";
    for (const auto &it2 : it.second) {
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
  for (auto &it : stackTrace) {
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
#endif

  return StackTrace(stackTrace);
#endif
}  // namespace ust
}  // namespace ust
