#include "TelemetryService.hpp"

namespace et {
class TelemetryDispatcher : public el::LogDispatchCallback {
 protected:
  void handle(const el::LogDispatchData* data) noexcept override {
    if (TelemetryService::exists() &&
        data->dispatchAction() == el::base::DispatchAction::NormalLog &&
        data->logMessage()->logger()->id() != "stdout") {
      auto logText = data->logMessage()->logger()->logBuilder()->build(
          data->logMessage(),
          data->dispatchAction() == el::base::DispatchAction::NormalLog);
      if (data->logMessage()->level() == el::Level::Fatal ||
          data->logMessage()->level() == el::Level::Error) {
        TelemetryService::get()->logToSentry(SENTRY_LEVEL_FATAL,
                                             logText.c_str());
      }
      string levelString;
      switch (data->logMessage()->level()) {
        case el::Level::Global:
          levelString = "Global";
          break;
        case el::Level::Trace:
          levelString = "Trace";
          break;
        case el::Level::Debug:
          levelString = "Debug";
          break;
        case el::Level::Fatal:
          levelString = "Fatal";
          break;
        case el::Level::Error:
          levelString = "Error";
          break;
        case el::Level::Warning:
          levelString = "Warning";
          break;
        case el::Level::Verbose:
          levelString = "Verbose";
          break;
        case el::Level::Info:
          levelString = "Info";
          break;
        case el::Level::Unknown:
          levelString = "Unknown";
          break;
      }
      TelemetryService::get()->logToDatadog(
          {{"message", logText}, {"level", levelString}});
    }
  }
};

void shutdownTelemetry() {
  cerr << "Shutting down sentry" << endl;
  if (TelemetryService::exists()) {
    auto ts = TelemetryService::get();
    ts->shutdown();
  }
}

TelemetryService::TelemetryService(bool _allow, const string& databasePath,
                                   const string& _environment)
    : allowed(_allow),
      environment(_environment),
      logHttpClient("https://browser-http-intake.logs.datadoghq.com"),
      shuttingDown(false) {
  if (allowed) {
    sentry_options_t* options = sentry_options_new();
    logHttpClient.set_compress(true);

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

    auto sentryShutdownHandler = [](int i) { shutdownTelemetry(); };
    sentry_value_t user = sentry_value_new_object();
    sentry_value_set_by_key(user, "ip_address",
                            sentry_value_new_string("{{auto}}"));
    sentry_set_user(user);

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
    atexit([] { shutdownTelemetry(); });

    el::Helpers::installLogDispatchCallback<TelemetryDispatcher>(
        "TelemetryDispatcher");
    auto* dispatcher = el::Helpers::logDispatchCallback<TelemetryDispatcher>(
        "TelemetryDispatcher");

    dispatcher->setEnabled(true);

    logSendingThread.reset(new thread([this]() {
      auto nextDumpTime = std::chrono::system_clock::now();
      while (!shuttingDown) {
        string payload;
        int logBufferSize;
        {
          lock_guard<recursive_mutex> guard(logMutex);
          logBufferSize = (int)logBuffer.size();
        }
        if (logBufferSize) {
          if (logBufferSize >= 1024 ||
              nextDumpTime < std::chrono::system_clock::now()) {
            nextDumpTime =
                std::chrono::system_clock::now() + chrono::seconds(30);
            {
              lock_guard<recursive_mutex> guard(logMutex);
              payload = json(logBuffer).dump(4);
              logBuffer.clear();
            }
            httplib::Headers headers;
            headers.emplace("DD-API-KEY", "e5e757f30a9e567f95b16b7673b09253");

            logHttpClient.set_connection_timeout(0,
                                                 300000);  // 300 milliseconds
            logHttpClient.set_read_timeout(1, 0);          // 1 second
            logHttpClient.set_write_timeout(1, 0);         // 1 second

            if (shuttingDown) {
              // httplib isn't exit-safe, so we try our best to avoid calling it
              // on shutdown
              break;
            }
            logHttpClient.Post(
                "/v1/input/"
                "pubfe47c2f8dfb3e8c26eb66ba4a456ec79?ddsource=browser&ddtags="
                "sdk_version:2.1.1",
                headers, payload, "application/json");
          }
        }
        this_thread::sleep_for(chrono::milliseconds(100));
      }
    }));
  }
}

TelemetryService::~TelemetryService() {
  if (!shuttingDown) {
    STERROR << "Destroyed telemetryService without a shutdown";
  }
}

void TelemetryService::logToSentry(sentry_level_e level,
                                   const string& message) {
  if (!allowed) return;
  sentry_capture_event(
      sentry_value_new_message_event(level, "stderr", message.c_str()));
}

void TelemetryService::logToDatadog(map<string, string> message) {
  lock_guard<recursive_mutex> lock(logMutex);
  if (logBuffer.size() > 16 * 1024) {
    // Ignore if the buffer is full
    return;
  }
  message["Environment"] = environment;
  message["Application"] = "Eternal Terminal";
  message["Version"] = ET_VERSION;
  logBuffer.push_back(message);
}

shared_ptr<TelemetryService> TelemetryService::telemetryServiceInstance;
}  // namespace et
