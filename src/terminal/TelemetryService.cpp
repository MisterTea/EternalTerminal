#include "TelemetryService.hpp"

namespace et {
class SentryDispatcher : public el::LogDispatchCallback {
 protected:
  void handle(const el::LogDispatchData* data) noexcept override {
    if (TelemetryService::exists() &&
        data->dispatchAction() == el::base::DispatchAction::NormalLog &&
        (data->logMessage()->level() == el::Level::Fatal ||
         data->logMessage()->level() == el::Level::Error)) {
      auto logText = data->logMessage()->logger()->logBuilder()->build(
          data->logMessage(),
          data->dispatchAction() == el::base::DispatchAction::NormalLog);
      TelemetryService::get()->log(SENTRY_LEVEL_FATAL, logText.c_str());
    }
  }
};

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

    sentry_value_t user = sentry_value_new_object();
    sentry_value_set_by_key(user, "ip_address",
                            sentry_value_new_string("{{auto}}"));
    sentry_set_user(user);

    auto sentryShutdownHandler = [](int i) {
      cerr << "Shutting down sentry" << endl;
      sentry_shutdown();
    };
    vector<int> signalsToCatch = {
#ifdef SIGINT
        SIGINT,
#endif
#ifdef SIGILL
        SIGILL,
#endif
#ifdef SIGABRT
        SIGABRT,
#endif
#ifdef SIGFPE
        SIGFPE,
#endif
#ifdef SIGSEGV
        SIGSEGV,
#endif
#ifdef SIGTERM
        SIGTERM,
#endif
#ifdef SIGKILL
        SIGKILL,
#endif
    };
    for (auto it : signalsToCatch) {
      signal(it, sentryShutdownHandler);
    }
    atexit([] {
      cerr << "Shutting down sentry" << endl;
      sentry_shutdown();
    });

    el::Helpers::installLogDispatchCallback<SentryDispatcher>(
        "SentryDispatcher");
    auto* dispatcher =
        el::Helpers::logDispatchCallback<SentryDispatcher>("SentryDispatcher");

    dispatcher->setEnabled(true);
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
