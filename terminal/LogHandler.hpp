#ifndef __ETERNAL_TCP_LOG_HANDLER__
#define __ETERNAL_TCP_LOG_HANDLER__

#include "Headers.hpp"

namespace et {
class LogHandler {
 public:
  static el::Configurations SetupLogHandler(int *argc, char ***argv);
  static void SetupLogFile(el::Configurations *defaultConf, string filename);
  static void rolloutHandler(const char *filename, std::size_t size);
};
}  // namespace et
#endif  // __ETERNAL_TCP_LOG_HANDLER__
