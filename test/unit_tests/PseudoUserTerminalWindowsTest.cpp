#ifdef WIN32

#include <windows.h>

#include <array>
#include <chrono>
#include <future>
#include <thread>

#include "PseudoUserTerminalWindows.hpp"
#include "TestHeaders.hpp"

namespace et {

class OutputPressureTerminal : public PseudoUserTerminal {
 public:
  std::string queuedOutput() {
    std::lock_guard<std::mutex> guard(pendingMutex);
    return pendingOutput;
  }

  static constexpr size_t outputLimit() { return MAX_PENDING_OUTPUT; }
};

TEST_CASE("PseudoUserTerminal cleanup falls back without a job",
          "[PseudoUserTerminal][windows]") {
  STARTUPINFOW startupInfo = {};
  startupInfo.cb = sizeof(startupInfo);
  PROCESS_INFORMATION processInfo = {};
  wchar_t commandLine[] = L"cmd.exe /D /C exit 0";

  REQUIRE(CreateProcessW(nullptr, commandLine, nullptr, nullptr, FALSE,
                         CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
                         &startupInfo, &processInfo));

  const bool terminated =
      TerminateProcessWithFallbackLocal(processInfo.hProcess, nullptr);
  if (!terminated) {
    TerminateProcess(processInfo.hProcess, 1);
    WaitForSingleObject(processInfo.hProcess, 2000);
  }
  const bool reaped =
      WaitForSingleObject(processInfo.hProcess, 0) == WAIT_OBJECT_0;

  CloseHandle(processInfo.hThread);
  CloseHandle(processInfo.hProcess);

  REQUIRE(terminated);
  REQUIRE(reaped);
}

// ClosePseudoConsole hangs if the host blocks on a full output pipe that
// nobody drains during shutdown.
TEST_CASE(
    "PseudoUserTerminal cleanup does not hang under queued, undrained output",
    "[PseudoUserTerminal][windows]") {
  const char* previousShell = ::getenv("SHELL");
  const bool hadShell = previousShell != nullptr;
  const std::string savedShell = hadShell ? previousShell : "";
  std::array<wchar_t, 32768> testExecutable;
  const DWORD testExecutableLength =
      GetModuleFileNameW(nullptr, testExecutable.data(),
                         static_cast<DWORD>(testExecutable.size()));
  REQUIRE(testExecutableLength > 0);
  REQUIRE(testExecutableLength < testExecutable.size());
  const fs::path helperPath =
      fs::path(std::wstring(testExecutable.data(), testExecutableLength))
          .parent_path() /
      "et-conpty-output-helper.exe";
  REQUIRE(fs::is_regular_file(helperPath));
  REQUIRE(fs::is_directory(helperPath.parent_path()));
  REQUIRE(_putenv_s("SHELL", helperPath.string().c_str()) == 0);
  auto* term = new OutputPressureTerminal();
  const int setupResult = term->setup(-1);
  REQUIRE(_putenv_s("SHELL", savedShell.c_str()) == 0);
  REQUIRE(setupResult == 0);

  const auto pressureDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  string queuedBeforeCleanup;
  while (queuedBeforeCleanup.size() < OutputPressureTerminal::outputLimit() &&
         std::chrono::steady_clock::now() < pressureDeadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    queuedBeforeCleanup = term->queuedOutput();
  }
  const bool capReached =
      queuedBeforeCleanup.size() >= OutputPressureTerminal::outputLimit();

  auto done = std::make_shared<std::promise<void>>();
  std::future<void> doneFuture = done->get_future();
  std::thread cleanupThread([term, done]() {
    term->cleanup();
    done->set_value();
  });

  const auto status = doneFuture.wait_for(std::chrono::seconds(15));
  bool queuedOutputPreserved = false;
  if (status == std::future_status::ready) {
    cleanupThread.join();
    const string queuedAfterCleanup = term->drainOutput();
    queuedOutputPreserved =
        queuedAfterCleanup.size() >= queuedBeforeCleanup.size() &&
        std::equal(queuedBeforeCleanup.begin(), queuedBeforeCleanup.end(),
                   queuedAfterCleanup.begin());
    delete term;
  } else {
    cleanupThread.detach();
  }
  INFO("queued before cleanup: " << queuedBeforeCleanup.size());
  REQUIRE(capReached);
  REQUIRE(status == std::future_status::ready);
  REQUIRE(queuedOutputPreserved);
}

}  // namespace et

#endif  // WIN32
