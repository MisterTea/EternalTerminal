#pragma once

#include "Headers.hpp"

#ifndef NO_TELEMETRY
#if __has_include(<boost/filesystem.hpp>)
#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;
#elif __has_include(<filesystem>)
#include <filesystem>
namespace fs = filesystem;
#elif __has_include(<experimental/filesystem>)
#include <experimental/filesystem>
namespace fs = std::experimental;
#else
#pragma message "No filesystem library found (which Telemetry needs)."
#endif
#endif

namespace httplib {
class Client;
}

namespace et {
class TelemetryService {
 public:
  TelemetryService(const bool _allow, const string& databasePath,
                   const string& environment);

  virtual ~TelemetryService();

  void logToSentry(el::Level level, const std::string& message);

  void logToDatadog(const string& logText, el::Level logLevel,
                    const string& filename, const int line);

  static void create(bool _allow, const string& databasePath,
                     const string& environment) {
    telemetryServiceInstance.reset(
        new TelemetryService(_allow, databasePath, environment));
  }

  static void destroy() { telemetryServiceInstance.reset(); }

  void shutdown();

  static bool exists() { return telemetryServiceInstance.get() != NULL; }

  static shared_ptr<TelemetryService> get() {
    if (telemetryServiceInstance) {
      return telemetryServiceInstance;
    }
    STFATAL << "Tried to get a singleton before it was created!";
    return NULL;
  }

 protected:
  static shared_ptr<TelemetryService> telemetryServiceInstance;
  bool allowed;
  string environment;
  unique_ptr<httplib::Client> logHttpClient;
  recursive_mutex logMutex;
  vector<map<string, string>> logBuffer;
  bool shuttingDown;
  unique_ptr<thread> logSendingThread;
  sole::uuid telemetryId;
};

}  // namespace et
