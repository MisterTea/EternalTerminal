#pragma once

// ust.hpp - Universal Stacktrace common interface
// Platform-specific implementations are in:
//   ust_windows.hpp - Windows (MSVC DbgHelp + MinGW addr2line)
//   ust_apple.hpp   - macOS (backtrace/libunwind + atos)
//   ust_unix.hpp    - Linux/Unix (backtrace/libunwind + addr2line)
//
// Debug info guarantee for filename/lineno symbolication:
//   - MSVC: requires /Zi (PDB) and /DEBUG link; pragma links dbghelp
//   - GCC/Clang: requires -g -ggdb3 -fno-omit-frame-pointer
// CMakeLists.txt enables -g -ggdb3 on UNIX (see CMakeLists.txt:251).
// Platform headers use pragma push_options to preserve frame pointers.

#include <algorithm>
#include <array>
#include <cstring>
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

// Cross-platform basename helpers - manual handling avoids shlwapi truncation issues
// and is portable across MSVC/MinGW/POSIX.
inline char *ustBasename(char *path) {
  if (!path || !*path) return path;
  char *lastSlash = strrchr(path, '\\');
  char *lastFwd = strrchr(path, '/');
  char *last = nullptr;
  if (lastSlash && lastFwd) last = lastSlash > lastFwd ? lastSlash : lastFwd;
  else if (lastSlash) last = lastSlash;
  else if (lastFwd) last = lastFwd;
  if (last) return last + 1;
  return path;
}
inline std::string ustBasenameString(std::string input) {
  size_t pos = input.find_last_of("/\\");
  if (pos != std::string::npos) {
    return input.substr(pos + 1);
  }
  return input;
}

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
    ss << " (" << ustBasenameString(si.sourceFileName) << ":" << si.lineNumber
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

}  // namespace ust

// Platform dispatch - includes must be after common definitions so they can use
// StackTraceEntry/StackTrace and helpers. Each header provides
//   ust::generate() and ust::generate_raw()
#if defined(_WIN32)
#include "ust_windows.hpp"
#elif defined(__APPLE__)
#include "ust_apple.hpp"
#else
#include "ust_unix.hpp"
#endif
