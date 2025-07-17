// Copyright 2015 The Crashpad Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "snapshot/win/exception_snapshot_win.h"

#include <windows.h>

#include <string>

#include "base/files/file_path.h"
#include "base/strings/utf_string_conversions.h"
#include "gtest/gtest.h"
#include "snapshot/win/exception_snapshot_win.h"
#include "snapshot/win/process_snapshot_win.h"
#include "test/errors.h"
#include "test/test_paths.h"
#include "test/win/child_launcher.h"
#include "util/file/file_io.h"
#include "util/thread/thread.h"
#include "util/win/exception_handler_server.h"
#include "util/win/registration_protocol_win.h"
#include "util/win/scoped_handle.h"
#include "util/win/scoped_process_suspend.h"

namespace crashpad {
namespace test {
namespace {

// Runs the ExceptionHandlerServer on a background thread.
class RunServerThread : public Thread {
 public:
  // Instantiates a thread which will invoke server->Run(delegate);
  RunServerThread(ExceptionHandlerServer* server,
                  ExceptionHandlerServer::Delegate* delegate)
      : server_(server), delegate_(delegate) {}

  RunServerThread(const RunServerThread&) = delete;
  RunServerThread& operator=(const RunServerThread&) = delete;

  ~RunServerThread() override {}

 private:
  // Thread:
  void ThreadMain() override { server_->Run(delegate_); }

  ExceptionHandlerServer* server_;
  ExceptionHandlerServer::Delegate* delegate_;
};

// During destruction, ensures that the server is stopped and the background
// thread joined.
class ScopedStopServerAndJoinThread {
 public:
  ScopedStopServerAndJoinThread(ExceptionHandlerServer* server, Thread* thread)
      : server_(server), thread_(thread) {}

  ScopedStopServerAndJoinThread(const ScopedStopServerAndJoinThread&) = delete;
  ScopedStopServerAndJoinThread& operator=(
      const ScopedStopServerAndJoinThread&) = delete;

  ~ScopedStopServerAndJoinThread() {
    server_->Stop();
    thread_->Join();
  }

 private:
  ExceptionHandlerServer* server_;
  Thread* thread_;
};

class CrashingDelegate : public ExceptionHandlerServer::Delegate {
 public:
  CrashingDelegate(HANDLE server_ready, HANDLE completed_test_event)
      : server_ready_(server_ready),
        completed_test_event_(completed_test_event),
        break_near_(0) {}

  CrashingDelegate(const CrashingDelegate&) = delete;
  CrashingDelegate& operator=(const CrashingDelegate&) = delete;

  ~CrashingDelegate() {}

  void set_break_near(WinVMAddress break_near) { break_near_ = break_near; }

  void ExceptionHandlerServerStarted() override { SetEvent(server_ready_); }

  unsigned int ExceptionHandlerServerException(
      HANDLE process,
      WinVMAddress exception_information_address,
      WinVMAddress debug_critical_section_address) override {
    ScopedProcessSuspend suspend(process);
    ProcessSnapshotWin snapshot;
    snapshot.Initialize(process,
                        ProcessSuspensionState::kSuspended,
                        exception_information_address,
                        debug_critical_section_address);

    // Confirm the exception record was read correctly.
    EXPECT_NE(snapshot.Exception()->ThreadID(), 0u);
    EXPECT_EQ(EXCEPTION_BREAKPOINT, snapshot.Exception()->Exception());

    // Verify the exception happened at the expected location with a bit of
    // slop space to allow for reading the current PC before the exception
    // happens. See TestCrashingChild().
#if !defined(NDEBUG)
    // Debug build is likely not optimized and contains more instructions.
    constexpr uint64_t kAllowedOffset = 200;
#else
    constexpr uint64_t kAllowedOffset = 100;
#endif
    EXPECT_GT(snapshot.Exception()->ExceptionAddress(), break_near_);
    EXPECT_LT(snapshot.Exception()->ExceptionAddress(),
              break_near_ + kAllowedOffset);

    SetEvent(completed_test_event_);

    return snapshot.Exception()->Exception();
  }

 private:
  HANDLE server_ready_;  // weak
  HANDLE completed_test_event_;  // weak
  WinVMAddress break_near_;
};

void TestCrashingChild(TestPaths::Architecture architecture) {
  // Set up the registration server on a background thread.
  ScopedKernelHANDLE server_ready(CreateEvent(nullptr, false, false, nullptr));
  ASSERT_TRUE(server_ready.is_valid()) << ErrorMessage("CreateEvent");
  ScopedKernelHANDLE completed(CreateEvent(nullptr, false, false, nullptr));
  ASSERT_TRUE(completed.is_valid()) << ErrorMessage("CreateEvent");
  CrashingDelegate delegate(server_ready.get(), completed.get());

  ExceptionHandlerServer exception_handler_server(true);
  std::wstring pipe_name(L"\\\\.\\pipe\\test_name");
  exception_handler_server.SetPipeName(pipe_name);
  RunServerThread server_thread(&exception_handler_server, &delegate);
  server_thread.Start();
  ScopedStopServerAndJoinThread scoped_stop_server_and_join_thread(
      &exception_handler_server, &server_thread);

  EXPECT_EQ(WaitForSingleObject(server_ready.get(), INFINITE), WAIT_OBJECT_0)
      << ErrorMessage("WaitForSingleObject");

  // Spawn a child process, passing it the pipe name to connect to.
  base::FilePath child_test_executable =
      TestPaths::BuildArtifact(L"snapshot",
                               L"crashing_child",
                               TestPaths::FileType::kExecutable,
                               architecture);
  ChildLauncher child(child_test_executable, pipe_name);
  ASSERT_NO_FATAL_FAILURE(child.Start());

  // The child tells us (approximately) where it will crash.
  WinVMAddress break_near_address;
  LoggingReadFileExactly(child.stdout_read_handle(),
                         &break_near_address,
                         sizeof(break_near_address));
  delegate.set_break_near(break_near_address);

  // Wait for the child to crash and the exception information to be validated.
  EXPECT_EQ(WaitForSingleObject(completed.get(), INFINITE), WAIT_OBJECT_0)
      << ErrorMessage("WaitForSingleObject");

  EXPECT_EQ(child.WaitForExit(), EXCEPTION_BREAKPOINT);
}

#if defined(ADDRESS_SANITIZER)
// https://crbug.com/845011
#define MAYBE_ChildCrash DISABLED_ChildCrash
#else
#define MAYBE_ChildCrash ChildCrash
#endif
TEST(ExceptionSnapshotWinTest, MAYBE_ChildCrash) {
  TestCrashingChild(TestPaths::Architecture::kDefault);
}

#if defined(ARCH_CPU_64_BITS)
TEST(ExceptionSnapshotWinTest, ChildCrashWOW64) {
  if (!TestPaths::Has32BitBuildArtifacts()) {
    GTEST_SKIP();
  }

  TestCrashingChild(TestPaths::Architecture::k32Bit);
}
#endif  // ARCH_CPU_64_BITS

class SimulateDelegate : public ExceptionHandlerServer::Delegate {
 public:
  SimulateDelegate(HANDLE server_ready, HANDLE completed_test_event)
      : server_ready_(server_ready),
        completed_test_event_(completed_test_event),
        dump_near_(0) {}

  SimulateDelegate(const SimulateDelegate&) = delete;
  SimulateDelegate& operator=(const SimulateDelegate&) = delete;

  ~SimulateDelegate() {}

  void set_dump_near(WinVMAddress dump_near) { dump_near_ = dump_near; }

  void ExceptionHandlerServerStarted() override { SetEvent(server_ready_); }

  unsigned int ExceptionHandlerServerException(
      HANDLE process,
      WinVMAddress exception_information_address,
      WinVMAddress debug_critical_section_address) override {
    ScopedProcessSuspend suspend(process);
    ProcessSnapshotWin snapshot;
    snapshot.Initialize(process,
                        ProcessSuspensionState::kSuspended,
                        exception_information_address,
                        debug_critical_section_address);
    EXPECT_TRUE(snapshot.Exception());
    EXPECT_EQ(snapshot.Exception()->Exception(), 0x517a7edu);

    // Verify the dump was captured at the expected location with some slop
    // space.
#if defined(ADDRESS_SANITIZER)
    // ASan instrumentation inserts more instructions between the expected
    // location and what's reported. https://crbug.com/845011.
    constexpr uint64_t kAllowedOffset = 500;
#elif !defined(NDEBUG)
    // Debug build is likely not optimized and contains more instructions.
    constexpr uint64_t kAllowedOffset = 200;
#else
    constexpr uint64_t kAllowedOffset = 100;
#endif
    EXPECT_GT(snapshot.Exception()->Context()->InstructionPointer(),
              dump_near_);
    EXPECT_LT(snapshot.Exception()->Context()->InstructionPointer(),
              dump_near_ + kAllowedOffset);

    EXPECT_EQ(snapshot.Exception()->ExceptionAddress(),
              snapshot.Exception()->Context()->InstructionPointer());

    SetEvent(completed_test_event_);

    return 0;
  }

 private:
  HANDLE server_ready_;  // weak
  HANDLE completed_test_event_;  // weak
  WinVMAddress dump_near_;
};

void TestDumpWithoutCrashingChild(TestPaths::Architecture architecture) {
  // Set up the registration server on a background thread.
  ScopedKernelHANDLE server_ready(CreateEvent(nullptr, false, false, nullptr));
  ASSERT_TRUE(server_ready.is_valid()) << ErrorMessage("CreateEvent");
  ScopedKernelHANDLE completed(CreateEvent(nullptr, false, false, nullptr));
  ASSERT_TRUE(completed.is_valid()) << ErrorMessage("CreateEvent");
  SimulateDelegate delegate(server_ready.get(), completed.get());

  ExceptionHandlerServer exception_handler_server(true);
  std::wstring pipe_name(L"\\\\.\\pipe\\test_name");
  exception_handler_server.SetPipeName(pipe_name);
  RunServerThread server_thread(&exception_handler_server, &delegate);
  server_thread.Start();
  ScopedStopServerAndJoinThread scoped_stop_server_and_join_thread(
      &exception_handler_server, &server_thread);

  EXPECT_EQ(WaitForSingleObject(server_ready.get(), INFINITE), WAIT_OBJECT_0)
      << ErrorMessage("WaitForSingleObject");

  // Spawn a child process, passing it the pipe name to connect to.
  base::FilePath child_test_executable =
      TestPaths::BuildArtifact(L"snapshot",
                               L"dump_without_crashing",
                               TestPaths::FileType::kExecutable,
                               architecture);
  ChildLauncher child(child_test_executable, pipe_name);
  ASSERT_NO_FATAL_FAILURE(child.Start());

  // The child tells us (approximately) where it will capture a dump.
  WinVMAddress dump_near_address;
  LoggingReadFileExactly(child.stdout_read_handle(),
                         &dump_near_address,
                         sizeof(dump_near_address));
  delegate.set_dump_near(dump_near_address);

  // Wait for the child to crash and the exception information to be validated.
  EXPECT_EQ(WaitForSingleObject(completed.get(), INFINITE), WAIT_OBJECT_0)
      << ErrorMessage("WaitForSingleObject");

  EXPECT_EQ(child.WaitForExit(), 0u);
}

#if defined(ADDRESS_SANITIZER)
// https://crbug.com/845011
#define MAYBE_ChildDumpWithoutCrashing DISABLED_ChildDumpWithoutCrashing
#else
#define MAYBE_ChildDumpWithoutCrashing ChildDumpWithoutCrashing
#endif
TEST(SimulateCrash, MAYBE_ChildDumpWithoutCrashing) {
  TestDumpWithoutCrashingChild(TestPaths::Architecture::kDefault);
}

#if defined(ARCH_CPU_64_BITS)
TEST(SimulateCrash, ChildDumpWithoutCrashingWOW64) {
  if (!TestPaths::Has32BitBuildArtifacts()) {
    GTEST_SKIP();
  }

  TestDumpWithoutCrashingChild(TestPaths::Architecture::k32Bit);
}
#endif  // ARCH_CPU_64_BITS

TEST(ExceptionSnapshot, TooManyExceptionParameters) {
  ProcessReaderWin process_reader;
  ASSERT_TRUE(process_reader.Initialize(GetCurrentProcess(),
                                        ProcessSuspensionState::kRunning));

  // Construct a fake exception record and CPU context.
  auto exception_record = std::make_unique<EXCEPTION_RECORD>();
  exception_record->ExceptionCode = STATUS_FATAL_APP_EXIT;
  exception_record->ExceptionFlags = EXCEPTION_NONCONTINUABLE;
  exception_record->ExceptionAddress = reinterpret_cast<PVOID>(0xFA15E);
  // One more than is permitted in the struct.
  exception_record->NumberParameters = EXCEPTION_MAXIMUM_PARAMETERS + 1;
  for (int i = 0; i < EXCEPTION_MAXIMUM_PARAMETERS; ++i) {
    exception_record->ExceptionInformation[i] = 1000 + i;
  }

  auto cpu_context = std::make_unique<internal::CPUContextUnion>();

  auto exception_pointers = std::make_unique<EXCEPTION_POINTERS>();
  exception_pointers->ExceptionRecord =
      reinterpret_cast<PEXCEPTION_RECORD>(exception_record.get());
  exception_pointers->ContextRecord =
      reinterpret_cast<PCONTEXT>(cpu_context.get());

  internal::ExceptionSnapshotWin snapshot;
  ASSERT_TRUE(snapshot.Initialize(
      &process_reader,
      GetCurrentThreadId(),
      reinterpret_cast<WinVMAddress>(exception_pointers.get()),
      nullptr));

  EXPECT_EQ(STATUS_FATAL_APP_EXIT, snapshot.Exception());
  EXPECT_EQ(static_cast<uint32_t>(EXCEPTION_NONCONTINUABLE),
            snapshot.ExceptionInfo());
  EXPECT_EQ(0xFA15Eu, snapshot.ExceptionAddress());
  EXPECT_EQ(static_cast<size_t>(EXCEPTION_MAXIMUM_PARAMETERS),
            snapshot.Codes().size());
  for (size_t i = 0; i < EXCEPTION_MAXIMUM_PARAMETERS; ++i) {
    EXPECT_EQ(1000 + i, snapshot.Codes()[i]);
  }
}

}  // namespace
}  // namespace test
}  // namespace crashpad
