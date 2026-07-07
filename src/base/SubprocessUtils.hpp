#ifndef __ET_SUBPROCESS_UTILS__
#define __ET_SUBPROCESS_UTILS__

#include "Headers.hpp"

namespace et {
/**
 * @brief Executes a shell command and captures all stdout into a string.
 * @param cmd Null-terminated command string passed to popen.
 */
inline std::string SystemToStr(const char* cmd) {
  std::array<char, 128> buffer;
  std::string result;
  std::shared_ptr<FILE> pipe(popen(cmd, "r"), pclose);
  if (!pipe) throw std::runtime_error("popen() failed!");
  while (!feof(pipe.get())) {
    if (fgets(buffer.data(), 128, pipe.get()) != nullptr)
      result += buffer.data();
  }
  return result;
}

/**
 * @brief Utility class for executing subprocesses and capturing output.
 */
class SubprocessUtils {
 public:
  virtual ~SubprocessUtils() = default;

  /**
   * @brief Runs a command with arguments while capturing its stdout without a
   * shell.
   */
  virtual string SubprocessToStringInteractive(const string& command,
                                               const vector<string>& args);
};
}  // namespace et

#endif  // __ET_SUBPROCESS_UTILS__
