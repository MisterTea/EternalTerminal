#include "TelemetryService.hpp"

#include "JsonLib.hpp"
#include "SimpleIni.h"

#if defined(USE_SENTRY)
#include "sentry.h"
#endif

namespace et {
#ifdef USE_SENTRY
inline sentry_level_e logLevelToSentry(el::Level l) {
  switch (l) {
    case el::Level::Info:
      return SENTRY_LEVEL_INFO;
    case el::Level::Warning:
      return SENTRY_LEVEL_WARNING;
    case el::Level::Error:
      return SENTRY_LEVEL_ERROR;
    case el::Level::Fatal:
      return SENTRY_LEVEL_FATAL;
    default:
      return SENTRY_LEVEL_DEBUG;
  }
}
#endif

inline std::string logLevelToString(el::Level l) {
  switch (l) {
    case el::Level::Global:
      return "Global";
    case el::Level::Trace:
      return "Trace";
    case el::Level::Debug:
      return "Debug";
    case el::Level::Fatal:
      return "Fatal";
    case el::Level::Error:
      return "Error";
    case el::Level::Warning:
      return "Warning";
    case el::Level::Verbose:
      return "Verbose";
    case el::Level::Info:
      return "Info";
    case el::Level::Unknown:
    default:
      return "Unknown";
  }
}

class TelemetryDispatcher : public el::LogDispatchCallback {
 protected:
  void handle(const el::LogDispatchData* data) noexcept override {
    if (TelemetryService::exists() &&
        data->dispatchAction() == el::base::DispatchAction::NormalLog &&
        data->logMessage()->logger()->id() != "stdout") {
      // Put the message on the fist line to make a better header
      auto logText =
          data->logMessage()->message() + "\n" +
          data->logMessage()->logger()->logBuilder()->build(
              data->logMessage(),
              data->dispatchAction() == el::base::DispatchAction::NormalLog);
      if (data->logMessage()->level() == el::Level::Fatal) {
        TelemetryService::get()->logToSentry(data->logMessage()->level(),
                                             logText);
      }
      if (data->logMessage()->level() == el::Level::Fatal ||
          data->logMessage()->level() == el::Level::Error) {
        TelemetryService::get()->logToDatadog(
            logText, data->logMessage()->level(), data->logMessage()->file(),
            data->logMessage()->line());
      }
    }
  }
};

void shutdownTelemetry() {
  if (TelemetryService::exists()) {
    cerr << "Shutting down sentry" << endl;
    auto ts = TelemetryService::get();
    ts->shutdown();
  }
}

TelemetryService::TelemetryService(const bool _allow,
                                   const string& databasePath,
                                   const string& _environment)
    : allowed(_allow),
      environment(_environment),
      logHttpClient(new httplib::Client(
          "https://browser-http-intake.logs.datadoghq.com")),
      shuttingDown(false) {
#ifdef NO_TELEMETRY
  allowed = false;
  return;
#else
  char* disableTelementry = ::getenv("ET_NO_TELEMETRY");
  if (disableTelementry) {
    allowed = false;
  }
  if (allowed) {
    auto telemetryConfigPath = sago::getConfigHome() + "/et/telemetry.ini";
    telemetryId = sole::uuid4();
    if (fs::exists(telemetryConfigPath)) {
      // Load the config file
      CSimpleIniA ini(true, false, false);
      SI_Error rc = ini.LoadFile(telemetryConfigPath.c_str());
      if (rc == 0) {
        const char* telemetryIdString = ini.GetValue("Sentry", "Id", NULL);
        if (telemetryIdString) {
          telemetryId = sole::rebuild(telemetryIdString);
        } else {
          STFATAL << "Invalid telemetry config";
        }
      } else {
        STFATAL << "Invalid config file: " << telemetryConfigPath;
      }
    } else {
      // Ensure directory exists
      fs::create_directories(sago::getConfigHome() + "/et");

      // Create ini
      CSimpleIniA ini(true, false, false);
      ini.SetValue("Sentry", "Id", telemetryId.str().c_str());
      ini.SaveFile(telemetryConfigPath.c_str());

      // Let user know about telemetry
      CLOG(INFO, "stdout")
          << "Eternal Terminal collects crashes and errors in order to help us "
             "improve your experience.\nThe data collected is anonymous.\nYou "
             "can opt-out of telemetry by setting the environment variable "
             "ET_NO_TELEMETRY to any non-empty value."
          << endl;
    }

#ifdef USE_SENTRY
    cerr << "Setting up and starting sentry" << endl;
    sentry_options_t* options = sentry_options_new();
    logHttpClient->set_compress(true);

    // this is an example. for real usage, make sure to set this explicitly to
    // an app specific cache location.
    sentry_options_set_database_path(options, databasePath.c_str());
    sentry_options_set_dsn(
        options,
        "https://46412bae7f0244d5abf84e17fdaf71d2@o496602.ingest.sentry.io/"
        "6143885");
    sentry_options_set_symbolize_stacktraces(options, true);
    sentry_options_set_release(options, "EternalTerminal@" ET_VERSION);
    sentry_options_set_environment(options, environment.c_str());

    sentry_init(options);

    auto sentryShutdownHandler = [](int i) { shutdownTelemetry(); };
    sentry_value_t user = sentry_value_new_object();
    sentry_value_set_by_key(user, "id",
                            sentry_value_new_string(telemetryId.str().c_str()));
    sentry_set_user(user);

    vector<int> signalsToCatch = {
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
#ifdef SIGKILL
        SIGKILL,
#endif
    };
    for (auto it : signalsToCatch) {
      signal(it, sentryShutdownHandler);
    }

#ifdef SIGTERM
    signal(SIGTERM, [](int i) {
      // In addition to exiting like the default SIGTERM handler would do,
      // let's shutdown sentry telemetry
      shutdownTelemetry();
      et::TerminateSignalHandler(i);
    });
#endif
#ifdef SIGINT
    signal(SIGINT, [](int i) {
      shutdownTelemetry();
      // Normally this is configured in TerminalServerMain, TerminalClientMain,
      // and TerminalMain, but we need to forward the call since Sentry
      // overrides it.  This is important to handle SIGINT from Ctrl-C.
      et::InterruptSignalHandler(i);
    });
#endif
#endif
    atexit([] { shutdownTelemetry(); });

    el::Helpers::installLogDispatchCallback<TelemetryDispatcher>(
        "TelemetryDispatcher");
    auto* dispatcher = el::Helpers::logDispatchCallback<TelemetryDispatcher>(
        "TelemetryDispatcher");

    dispatcher->setEnabled(true);

    logSendingThread.reset(new thread([this]() {
      auto nextDumpTime = std::chrono::system_clock::now();
      while (true) {
        bool lastRun;
        string payload;
        int logBufferSize;
        {
          lock_guard<recursive_mutex> guard(logMutex);
          lastRun = shuttingDown;
          logBufferSize = (int)logBuffer.size();
        }
        if (logBufferSize) {
          if (logBufferSize >= 1024 ||
              nextDumpTime < std::chrono::system_clock::now() || lastRun) {
            nextDumpTime =
                std::chrono::system_clock::now() + chrono::seconds(30);
            {
              lock_guard<recursive_mutex> guard(logMutex);
              payload = json(logBuffer).dump(4);
              logBuffer.clear();
            }
            httplib::Headers headers;
            headers.emplace("DD-API-KEY", "e5e757f30a9e567f95b16b7673b09253");

            logHttpClient->set_connection_timeout(0,
                                                  300000);  // 300 milliseconds
            logHttpClient->set_read_timeout(1, 0);          // 1 second
            logHttpClient->set_write_timeout(1, 0);         // 1 second

            logHttpClient->Post(
                "/v1/input/"
                "pubfe47c2f8dfb3e8c26eb66ba4a456ec79?ddsource=browser&ddtags="
                "sdk_version:2.1.1",
                headers, payload, "application/json");
          }
        }
        this_thread::sleep_for(chrono::milliseconds(100));
        if (lastRun) {
          this_thread::sleep_for(chrono::milliseconds(400));
          break;
        }
      }
    }));
  }
#endif
}

TelemetryService::~TelemetryService() {
  lock_guard<recursive_mutex> guard(logMutex);
  if (!shuttingDown) {
    cerr << "Destroyed telemetryService without a shutdown";
  }
}

void TelemetryService::logToSentry(el::Level level, const string& message) {
  if (!allowed) return;
#ifdef USE_SENTRY
  sentry_capture_event(sentry_value_new_message_event(
      logLevelToSentry(level), "stderr", message.c_str()));
#endif
}

void TelemetryService::logToDatadog(const string& logText, el::Level logLevel,
                                    const string& filename, const int line) {
  map<string, string> messageJson = {
      {"message", logText},         {"level", logLevelToString(logLevel)},
      {"Environment", environment}, {"Application", "Eternal Terminal"},
      {"Version", ET_VERSION},      {"TelemetryId", telemetryId.str()},
      {"File", filename},           {"Line", to_string(line)}};

  lock_guard<recursive_mutex> lock(logMutex);
  if (logBuffer.size() > 16 * 1024) {
    // Ignore if the buffer is full
    return;
  }
  logBuffer.push_back(messageJson);
}

void TelemetryService::shutdown() {
  {
    lock_guard<recursive_mutex> guard(logMutex);
    if (shuttingDown) {
      return;
    }
    shuttingDown = true;
  }
#ifdef USE_SENTRY
  sentry_shutdown();
#endif
  if (logSendingThread) {
    logSendingThread->join();
    logSendingThread.reset();
  }
}

shared_ptr<TelemetryService> TelemetryService::telemetryServiceInstance;
}  // namespace et
