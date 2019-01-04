#ifndef __ET_LOG_HANDLER__
#define __ET_LOG_HANDLER__

#include "Headers.hpp"

namespace et {
class LogHandler {
 public:
  static el::Configurations setupLogHandler(int *argc, char ***argv);
  static void setupLogFile(el::Configurations *defaultConf, string filename,
                           string maxlogsize = "20971520");
  static void rolloutHandler(const char *filename, std::size_t size);
  static string stderrToFile(const string &pathPrefix);
};
}  // namespace et
#endif  // __ET_LOG_HANDLER__
