#ifndef __ET_LOG_HANDLER__
#define __ET_LOG_HANDLER__

#include "Headers.hpp"

namespace et {
/**
 * @brief Configures easylogging++ so EternalTerminal can control log files.
 */
class LogHandler {
 public:
  /**
   * @brief Initializes logging using the supplied `argc/argv` parameters.
   * @return A default configuration that callers can further customize.
   */
  static el::Configurations setupLogHandler(int* argc, char*** argv);

  /**
   * @brief Sets up file-based logging, optionally writing stderr to disk.
   * @param defaultConf Base easylogging configuration that will be mutated.
   */
  static void setupLogFiles(el::Configurations* defaultConf, const string& path,
                            const string& filenamePrefix,
                            bool logToStdout = false,
                            bool redirectStderrToFile = false,
                            bool appendPid = false,
                            string maxlogsize = "20971520");

  /**
   * @brief Performs log rotation by removing the supplied filename.
   */
  static void rolloutHandler(const char* filename, std::size_t size);

  /**
   * @brief Reconfigures the easylogging stdout logger so it just writes
   * messages.
   */
  static void setupStdoutLogger();

  /**
   * @brief Writes subprocess stderr onto the terminal captured before stderr
   * was redirected into a log file, and onto the current stderr so the log
   * keeps a copy.
   * @return false when stderr has not been redirected, so the caller writes
   * to the current stderr itself.
   */
  static bool forwardSubprocessStderr(const char* data, size_t len);

  /**
   * @brief Closes the terminal stderr saved for forwardSubprocessStderr.
   *
   * Tests restore the process stderr and call this so later cases do not
   * write banners at a closed descriptor.
   */
  static void releasePreservedUserStderr();

 private:
  /**
   * @brief Redirects stderr to a file created in the specified directory.
   */
  static void stderrToFile(const string& path, const string& stderrFilename);

  /**
   * @brief Ensures the directory exists and creates a new log file.
   */
  static string createLogFile(const string& path, const string& filename);
};
}  // namespace et
#endif  // __ET_LOG_HANDLER__
