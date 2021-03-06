#pragma once

#include "Headers.hpp"

namespace et {
class TelemetryService {
 public:
  TelemetryService(const bool _allow, const string& databasePath,
                   const string& environment);

  virtual ~TelemetryService();

  void logToSentry(el::Level level, const std::string& message);

  void logToDatadog(map<string, string> message);

  void logToAll(el::Level level, const std::string& message);

  static void create(bool _allow, const string& databasePath,
                     const string& environment) {
    telemetryServiceInstance.reset(
        new TelemetryService(_allow, databasePath, environment));
  }

  static void destroy() { telemetryServiceInstance.reset(); }

  void shutdown() {
    if (shuttingDown) {
      return;
    }
    shuttingDown = true;
#ifdef USE_SENTRY
    sentry_shutdown();
#endif
    if (logSendingThread) {
      logSendingThread->join();
      logSendingThread.reset();
    }
  }

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
  httplib::Client logHttpClient;
  recursive_mutex logMutex;
  vector<map<string, string>> logBuffer;
  bool shuttingDown;
  unique_ptr<thread> logSendingThread;
};

}  // namespace et
