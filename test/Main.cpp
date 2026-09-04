#define CATCH_CONFIG_RUNNER

#include <cctype>
#include <cstring>
#include <iostream>

#ifndef WIN32
#include <signal.h>
#include <unistd.h>
#endif

#include "LogHandler.hpp"
#include "TelemetryService.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {
bool argEqualsIgnoreCase(const char* a, const char* b) {
  while (*a && *b) {
    if (tolower(static_cast<unsigned char>(*a)) !=
        tolower(static_cast<unsigned char>(*b))) {
      return false;
    }
    ++a;
    ++b;
  }
  return *a == *b;
}
}  // namespace

int main(int argc, char** argv) {
  srand(1);

  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] == '-') {
      continue;
    }
    if (argEqualsIgnoreCase(argv[i], "ghostty")) {
      argv[i] = const_cast<char*>("[.ghostty]");
    } else if (argEqualsIgnoreCase(argv[i], "iterm2")) {
      std::cerr
          << "iTerm2 e2e is not part of default Catch2/CTest. Run:\n"
          << "  python3 test/system_tests/iterm2_htm_e2e.py --htm build/htm "
             "--htmd build/htmd\n"
          << "  python3 test/system_tests/iterm2_htm_stress_e2e.py --htm "
             "build/htm --htmd build/htmd\n"
          << "  python3 test/system_tests/iterm2_tmux_ctrlc_e2e.py\n";
      return 2;
    } else if (argEqualsIgnoreCase(argv[i], "hyper")) {
      std::cerr << "Hyper e2e lives in the hyper-htm repo. Run: npm run "
                   "test:system\n";
      return 2;
    }
  }

  bool listOnly = false;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--list-tests") == 0 || strcmp(argv[i], "-l") == 0) {
      listOnly = true;
      break;
    }
  }

  // Setup easylogging configurations
  el::Configurations defaultConf =
      et::LogHandler::setupLogHandler(&argc, &argv);
  et::LogHandler::setupStdoutLogger();
  // el::Loggers::setVerboseLevel(9);

  et::HandleTerminate();
#ifndef WIN32
  // Match UnixSocketHandler::initSocket(). Tests that drive socketpairs with
  // raw ::write() never call initSocket, and a peer closing during recover
  // would otherwise kill the process with SIGPIPE (debian Portability CI).
  ::signal(SIGPIPE, SIG_IGN);
#endif

  string logDirectory;
#ifdef WIN32
  logDirectory = GetTempDirectory() + "et_test_" + genRandomAlphaNum(12);
  fs::create_directories(logDirectory);
#else
  string logDirectoryPattern = GetTempDirectory() + string("et_test_XXXXXXXX");
  logDirectory = string(mkdtemp(&logDirectoryPattern[0]));
#endif
  if (!listOnly) {
    CLOG(INFO, "stdout") << "Writing log to " << logDirectory << endl;
  }
  et::LogHandler::setupLogFiles(&defaultConf, logDirectory, "log", true, true);

  // Reconfigure default logger to apply settings above
  el::Loggers::reconfigureLogger("default", defaultConf);

  TelemetryService::create(false, "", "");

#ifndef WIN32
  // HTM tests send Bourne-shell syntax (printf, awk, exit). Login csh/zsh on
  // some CI images otherwise ignore those commands and time out.
  setenv("SHELL", "/bin/sh", 1);
#endif

  int result = Catch::Session().run(argc, argv);

  TelemetryService::get()->shutdown();
  TelemetryService::destroy();

  // Release file sinks before removing the per-process log directory. Windows
  // does not allow unlinking open log files.
  el::Loggers::flushAll();
  el::Loggers::unregisterLogger("default");
  el::Loggers::unregisterLogger("stdout");

  try {
    fs::remove_all(logDirectory);
  } catch (const fs::filesystem_error& e) {
    std::cerr << "Failed to remove test log directory " << logDirectory << ": "
              << e.what() << '\n';
  }
  return result;
}
