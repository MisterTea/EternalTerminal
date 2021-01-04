#pragma once

#include "Headers.hpp"

namespace et {
class TelemetryService {
 public:
  TelemetryService(bool _allow, const string& databasePath,
                   const string& environment);

  virtual ~TelemetryService();

  void log(sentry_level_e level, const std::string& message);

  static void create(bool _allow, const string& databasePath,
                     const string& environment) {
    telemetryServiceInstance.reset(
        new TelemetryService(_allow, databasePath, environment));
  }

  static void destroy() { telemetryServiceInstance.reset(); }

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
};

}  // namespace et
