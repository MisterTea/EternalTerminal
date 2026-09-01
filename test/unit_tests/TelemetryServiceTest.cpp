#include "TelemetryService.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {
class TestTelemetryService : public TelemetryService {
 public:
  TestTelemetryService() : TelemetryService(false, "", "unit-test") {}

  size_t bufferedLogs() {
    lock_guard<recursive_mutex> guard(logMutex);
    return logBuffer.size();
  }

  bool isShuttingDown() {
    lock_guard<recursive_mutex> guard(logMutex);
    return shuttingDown;
  }
};
}  // namespace

TEST_CASE("TelemetryService disabled logging and shutdown",
          "[TelemetryService]") {
  TestTelemetryService telemetry;

  const vector<el::Level> levels = {
      el::Level::Global,  el::Level::Trace, el::Level::Debug,
      el::Level::Fatal,   el::Level::Error, el::Level::Warning,
      el::Level::Verbose, el::Level::Info,  el::Level::Unknown,
  };
  for (auto level : levels) {
    telemetry.logToDatadog("message", level, "file.cpp", 42);
    telemetry.logToSentry(level, "message");
  }
  REQUIRE(telemetry.bufferedLogs() == levels.size());

  telemetry.shutdown();
  REQUIRE(telemetry.isShuttingDown());
  REQUIRE_NOTHROW(telemetry.shutdown());
}

TEST_CASE("TelemetryService bounds its pending log queue",
          "[TelemetryService]") {
  TestTelemetryService telemetry;
  for (size_t i = 0; i < 16 * 1024 + 2; ++i) {
    telemetry.logToDatadog("message", el::Level::Info, "file.cpp", 42);
  }
  REQUIRE(telemetry.bufferedLogs() == 16 * 1024 + 1);
  telemetry.shutdown();
}
