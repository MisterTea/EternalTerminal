#ifndef __ET_SUBPROCESS_UTILS__
#define __ET_SUBPROCESS_UTILS__

#include "Headers.hpp"

namespace et {
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
