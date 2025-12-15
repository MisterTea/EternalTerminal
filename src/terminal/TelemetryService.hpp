#pragma once

#include "Headers.hpp"

namespace httplib {
class Client;
}

namespace et {
/**
 * @brief Sends anonymized logs to Datadog/Sentry when telemetry is permitted.
 *
 * Uses a singleton accessible via `create()`/`get()` and buffers logs before
 * sending them on a background thread.
 */
class TelemetryService {
 public:
  TelemetryService(const bool _allow, const string& databasePath,
                   const string& environment);

  virtual ~TelemetryService();

  /** @brief Sends an error/level pair to Sentry through the HTTP client. */
  void logToSentry(el::Level level, const std::string& message);

  /** @brief Sends a formatted Datadog log line including file/line metadata. */
  void logToDatadog(const string& logText, el::Level logLevel,
                    const string& filename, const int line);

  static void create(bool _allow, const string& databasePath,
                     const string& environment) {
    telemetryServiceInstance.reset(
        new TelemetryService(_allow, databasePath, environment));
  }

  static void destroy() { telemetryServiceInstance.reset(); }

  /** @brief Gracefully stops the background thread and flushes the log buffer.
   */
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
  /** @brief Singleton instance returned by `get()`. */
  static shared_ptr<TelemetryService> telemetryServiceInstance;
  /** @brief Indicates whether telemetry payloads are permitted. */
  bool allowed;
  /** @brief Deployment environment identifier (e.g., release channel). */
  string environment;
  /** @brief HTTP client used to post data to Datadog/Sentry. */
  unique_ptr<httplib::Client> logHttpClient;
  /** @brief Guards access to the buffered log state. */
  recursive_mutex logMutex;
  /** @brief Queued telemetry entries waiting to be sent. */
  vector<map<string, string>> logBuffer;
  /** @brief Indicates if `shutdown()` has been called. */
  bool shuttingDown;
  /** @brief Background thread that flushes telemetry asynchronously. */
  unique_ptr<thread> logSendingThread;
  /** @brief Unique identifier emitted with every telemetry batch. */
  sole::uuid telemetryId;
};

}  // namespace et
