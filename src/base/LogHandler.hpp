#ifndef __ET_LOG_HANDLER__
#define __ET_LOG_HANDLER__

#include "Headers.hpp"

namespace et {
class LogHandler {
 public:
  static el::Configurations setupLogHandler(int *argc, char ***argv);
  static void setupLogFiles(el::Configurations *defaultConf, const string &path,
                            const string &filenamePrefix,
                            bool logToStdout = false,
                            bool redirectStderrToFile = false,
                            bool appendPid = false,
                            string maxlogsize = "20971520");
  static void rolloutHandler(const char *filename, std::size_t size);
  static void setupStdoutLogger();

 private:
  static void stderrToFile(const string &stderrFilename);
  static void createLogFile(const string &filename);
};
}  // namespace et
#endif  // __ET_LOG_HANDLER__
