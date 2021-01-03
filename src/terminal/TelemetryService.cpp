#include "TelemetryService.hpp"

namespace et {
TelemetryService::TelemetryService(bool _allow, const string& databasePath,
                                   const string& environment)
    : allowed(_allow) {
  if (allowed) {
    sentry_options_t* options = sentry_options_new();

    // this is an example. for real usage, make sure to set this explicitly to
    // an app specific cache location.
    sentry_options_set_database_path(options, databasePath.c_str());
    sentry_options_set_dsn(
        options,
        "https://51ec60d489224f1da2b63c912a5c7fad@o496602.ingest.sentry.io/"
        "5574732");
    sentry_options_set_symbolize_stacktraces(options, true);
    sentry_options_set_release(options, "EternalTerminal@" ET_VERSION);
    sentry_options_set_environment(options, environment.c_str());

    sentry_init(options);
  }
}

TelemetryService::~TelemetryService() { sentry_shutdown(); }

void TelemetryService::log(sentry_level_e level, const std::string& message) {
  if (!allowed) return;
  sentry_capture_event(
      sentry_value_new_message_event(level, "stderr", message.c_str()));
}

shared_ptr<TelemetryService> TelemetryService::telemetryServiceInstance;
}  // namespace et
