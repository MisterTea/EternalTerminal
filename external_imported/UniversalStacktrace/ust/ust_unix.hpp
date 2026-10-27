#pragma once

// ust_unix.hpp - Linux/Unix stacktrace implementation (addr2line)
// Debug info requirements for filename/lineno symbolication:
//   Requires -g -ggdb3 -fno-omit-frame-pointer (CMake enables -g -ggdb3 on UNIX)
//   This header preserves frame pointers for reliable unwinding.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("-fno-omit-frame-pointer")
#endif

#include <cxxabi.h>
#include <errno.h>

#if defined(__ANDROID__)
// Android's bionic libc does not provide execinfo or libunwind
#define USE_UNWIND 0
#elif __has_include(<libunwind.h>)
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
#include <libgen.h>
#include <sys/wait.h>

#ifdef __linux__
#include <linux/limits.h>
#endif

#include <algorithm>
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

// Raw capture without addr2line symbolization. Fast path for STERROR.
UST_NOINLINE inline StackTrace generate_raw() {
  std::vector<StackTraceEntry> stackTrace;
  std::map<std::string, std::pair<uint64_t, uint64_t>> addressMaps;
  std::string line;
  std::string procMapFileName = std::string("/proc/self/maps");
  std::ifstream infile(procMapFileName.c_str());
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
    }
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
  void* stack[MAX_STACK_FRAMES];
  int numFrames;
#endif
#if USE_UNWIND
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
    char* demangled = abi::__cxa_demangle(mangled, 0, 0, &ok);
    std::string filename;
    uint64_t absoluteAddress = uint64_t(ip);
    uint64_t relativeAddress = 0;
    for (auto& it : addressMaps) {
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
#elif defined(__ANDROID__)
  // Android bionic: no backtrace, leave stackTrace empty
#else
  numFrames = backtrace(stack, MAX_STACK_FRAMES);
  memmove(stack, stack + 1, sizeof(void*) * (numFrames - 1));
  numFrames--;
  char** strings = backtrace_symbols(stack, numFrames);
  if (strings) {
    for (int a = 0; a < numFrames; ++a) {
      std::string addr;
      std::string fileName;
      std::string functionName;
      const std::string lineStr(strings[a]);
      // Example: ./ust-test(_ZNK5Catch21TestInvokerAsFunction6invokeEv+0x16) [0x55f1278af96e]
      auto parenStart = lineStr.find("(");
      auto parenEnd = lineStr.find(")");
      fileName = lineStr.substr(0, parenStart);
      char buf[PATH_MAX];
      ::realpath(fileName.c_str(), buf);
      fileName = std::string(buf);
      functionName = lineStr.substr(parenStart + 1, parenEnd - (parenStart + 1));
      functionName = functionName.substr(0, functionName.find("+"));
      if (addressMaps.find(fileName) != addressMaps.end()) {
        addr =
            addressToString(uint64_t(stack[a]) - addressMaps[fileName].first);
      } else {
        addr = addressToString(uint64_t(stack[a]));
      }
      if (!functionName.empty()) {
        int status = 0;
        auto demangledFunctionName =
            abi::__cxa_demangle(functionName.data(), 0, 0, &status);
        if (status == 0) {
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
  return StackTrace(stackTrace);
}

UST_NOINLINE inline StackTrace generate() {
  static std::mutex mtx;
  std::lock_guard<std::mutex> lock(mtx);

  std::vector<StackTraceEntry> stackTrace;
  std::map<std::string, std::pair<uint64_t, uint64_t>> addressMaps;
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
  void* stack[MAX_STACK_FRAMES];
  int numFrames;
#endif
#if USE_UNWIND
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
    char* demangled = abi::__cxa_demangle(mangled, 0, 0, &ok);

    std::string filename;
    uint64_t absoluteAddress = uint64_t(ip);
    uint64_t relativeAddress = 0;
    for (auto& it : addressMaps) {
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
#elif defined(__ANDROID__)
  // Android bionic: no backtrace, leave stackTrace empty
#else
  numFrames = backtrace(stack, MAX_STACK_FRAMES);
  memmove(stack, stack + 1, sizeof(void*) * (numFrames - 1));
  numFrames--;

  char** strings = backtrace_symbols(stack, numFrames);
  if (strings) {
    for (int a = 0; a < numFrames; ++a) {
      std::string addr;
      std::string fileName;
      std::string functionName;

      const std::string lineStr(strings[a]);
      // Example: ./ust-test(_ZNK5Catch21TestInvokerAsFunction6invokeEv+0x16) [0x55f1278af96e]
      auto parenStart = lineStr.find("(");
      auto parenEnd = lineStr.find(")");
      fileName = lineStr.substr(0, parenStart);
      // Convert filename to canonical path
      char buf[PATH_MAX];
      ::realpath(fileName.c_str(), buf);
      fileName = std::string(buf);
      functionName = lineStr.substr(parenStart + 1, parenEnd - (parenStart + 1));
      // Strip off the offset from the name
      functionName = functionName.substr(0, functionName.find("+"));
      if (addressMaps.find(fileName) != addressMaps.end()) {
        // Make address relative to process start
        addr =
            addressToString(uint64_t(stack[a]) - addressMaps[fileName].first);
      } else {
        addr = addressToString(uint64_t(stack[a]));
      }
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

  // Fetch source file & line numbers via addr2line
  std::map<std::string, std::list<std::string>> fileAddresses;
  std::map<std::string, std::list<std::string>> fileData;
  for (const auto& it : stackTrace) {
    if (it.binaryFileName.length()) {
      if (fileAddresses.find(it.binaryFileName) == fileAddresses.end()) {
        fileAddresses[it.binaryFileName] = {};
      }
      fileAddresses.at(it.binaryFileName).push_back(it.address);
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

}  // namespace ust

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC pop_options
#endif
