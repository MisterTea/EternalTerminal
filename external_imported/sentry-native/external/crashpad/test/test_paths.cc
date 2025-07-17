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

#include "test/test_paths.h"

#include <stdlib.h>
#include <sys/stat.h>

#include "base/logging.h"
#include "build/build_config.h"
#include "util/misc/paths.h"

namespace crashpad {
namespace test {

namespace {

bool IsTestDataRoot(const base::FilePath& candidate) {
  const base::FilePath marker_path =
      candidate.Append(FILE_PATH_LITERAL("test"))
          .Append(FILE_PATH_LITERAL("test_paths_test_data_root.txt"));

#if !BUILDFLAG(IS_WIN)
  struct stat stat_buf;
  int rv = stat(marker_path.value().c_str(), &stat_buf);
#else
  struct _stat stat_buf;
  int rv = _wstat(marker_path.value().c_str(), &stat_buf);
#endif

  return rv == 0;
}

base::FilePath TestDataRootInternal() {
#if BUILDFLAG(IS_FUCHSIA)
  base::FilePath asset_path("/pkg/data");
  if (!IsTestDataRoot(asset_path)) {
    LOG(WARNING) << "test data root seems invalid, continuing anyway";
  }
  return asset_path;
#else  // BUILDFLAG(IS_FUCHSIA)
#if !BUILDFLAG(IS_WIN)
  const char* environment_value = getenv("CRASHPAD_TEST_DATA_ROOT");
#else  // BUILDFLAG(IS_WIN)
  const wchar_t* environment_value = _wgetenv(L"CRASHPAD_TEST_DATA_ROOT");
#endif

  if (environment_value) {
    // It was specified explicitly, so use it even if it seems incorrect.
    if (!IsTestDataRoot(base::FilePath(environment_value))) {
      LOG(WARNING) << "CRASHPAD_TEST_DATA_ROOT seems invalid, honoring anyway";
    }

    return base::FilePath(environment_value);
  }

  base::FilePath executable_path;
  if (Paths::Executable(&executable_path)) {
#if BUILDFLAG(IS_IOS) || BUILDFLAG(IS_ANDROID)
    // On Android and iOS, test data is in a crashpad_test_data directory
    // adjacent to the main executable. On iOS, this refers to the main
    // executable file inside the .app bundle, so crashpad_test_data is also
    // inside the bundle.
    base::FilePath candidate = executable_path.DirName()
                               .Append("crashpad_test_data");
#else  // BUILDFLAG(IS_IOS) || BUILDFLAG(IS_ANDRID)
    // In a standalone build, the test executable is usually at
    // out/{Debug,Release} relative to the Crashpad root.
    base::FilePath candidate =
        base::FilePath(executable_path.DirName()
                           .Append(base::FilePath::kParentDirectory)
                           .Append(base::FilePath::kParentDirectory));
#endif  // BUILDFLAG(IS_IOS) || BUILDFLAG(IS_ANDROID)
    if (IsTestDataRoot(candidate)) {
      return candidate;
    }

    // In an in-Chromium build, the test executable is usually at
    // out/{Debug,Release} relative to the Chromium root, and the Crashpad root
    // is at third_party/crashpad/crashpad relative to the Chromium root.
    candidate = candidate.Append(FILE_PATH_LITERAL("third_party"))
                    .Append(FILE_PATH_LITERAL("crashpad"))
                    .Append(FILE_PATH_LITERAL("crashpad"));
    if (IsTestDataRoot(candidate)) {
      return candidate;
    }
  }

  // If nothing else worked, use the current directory, issuing a warning if it
  // doesn’t seem right.
  if (!IsTestDataRoot(base::FilePath(base::FilePath::kCurrentDirectory))) {
    LOG(WARNING) << "could not locate a valid test data root";
  }

  return base::FilePath(base::FilePath::kCurrentDirectory);
#endif  // BUILDFLAG(IS_FUCHSIA)
}

#if BUILDFLAG(IS_WIN) && defined(ARCH_CPU_64_BITS)

// Returns the pathname of a directory containing 32-bit test build output.
//
// It would be better for this to be named 32BitOutputDirectory(), but that’s
// not a legal name.
base::FilePath Output32BitDirectory() {
  const wchar_t* environment_value = _wgetenv(L"CRASHPAD_TEST_32_BIT_OUTPUT");
  if (!environment_value) {
    return base::FilePath();
  }

  return base::FilePath(environment_value);
}

#endif  // BUILDFLAG(IS_WIN) && defined(ARCH_CPU_64_BITS)

}  // namespace

// static
base::FilePath TestPaths::Executable() {
  base::FilePath executable_path;
  CHECK(Paths::Executable(&executable_path));
#if defined(CRASHPAD_IS_IN_FUCHSIA)
  executable_path = base::FilePath("/pkg/bin/app");
#endif
  return executable_path;
}

// static
base::FilePath TestPaths::ExpectedExecutableBasename(
    const base::FilePath::StringType& name) {
#if BUILDFLAG(IS_FUCHSIA)
  // Apps in Fuchsia packages are always named "app".
  return base::FilePath("app");
#else  // BUILDFLAG(IS_FUCHSIA)
#if defined(CRASHPAD_IS_IN_CHROMIUM)
  base::FilePath::StringType executable_name(
      FILE_PATH_LITERAL("crashpad_tests"));
#else  // CRASHPAD_IS_IN_CHROMIUM
  base::FilePath::StringType executable_name(name);
#endif  // CRASHPAD_IS_IN_CHROMIUM

#if BUILDFLAG(IS_WIN)
  executable_name += FILE_PATH_LITERAL(".exe");
#endif  // BUILDFLAG(IS_WIN)

  return base::FilePath(executable_name);
#endif  // BUILDFLAG(IS_FUCHSIA)
}

// static
base::FilePath TestPaths::TestDataRoot() {
  static base::FilePath* test_data_root =
      new base::FilePath(TestDataRootInternal());
  return *test_data_root;
}

// static
base::FilePath TestPaths::BuildArtifact(
    const base::FilePath::StringType& module,
    const base::FilePath::StringType& artifact,
    FileType file_type,
    Architecture architecture) {
  base::FilePath directory;
  switch (architecture) {
    case Architecture::kDefault:
      directory = Executable().DirName();
      break;

#if BUILDFLAG(IS_WIN) && defined(ARCH_CPU_64_BITS)
    case Architecture::k32Bit:
      directory = Output32BitDirectory();
      CHECK(!directory.empty());
      break;
#endif  // BUILDFLAG(IS_WIN) && ARCH_CPU_64_BITS
  }

  base::FilePath::StringType test_name =
      FILE_PATH_LITERAL("crashpad_") + module + FILE_PATH_LITERAL("_test");
#if !defined(CRASHPAD_IS_IN_CHROMIUM) && !BUILDFLAG(IS_FUCHSIA)
  CHECK(Executable().BaseName().RemoveFinalExtension().value() == test_name);
#endif  // !CRASHPAD_IS_IN_CHROMIUM

  base::FilePath::StringType extension;
  switch (file_type) {
    case FileType::kNone:
      break;

    case FileType::kExecutable:
#if BUILDFLAG(IS_WIN)
      extension = FILE_PATH_LITERAL(".exe");
#elif BUILDFLAG(IS_FUCHSIA)
      directory = base::FilePath(FILE_PATH_LITERAL("/pkg/bin"));
#endif  // BUILDFLAG(IS_WIN)
      break;

    case FileType::kLoadableModule:
#if BUILDFLAG(IS_WIN)
      extension = FILE_PATH_LITERAL(".dll");
#else  // BUILDFLAG(IS_WIN)
      extension = FILE_PATH_LITERAL(".so");
#endif  // BUILDFLAG(IS_WIN)

#if BUILDFLAG(IS_FUCHSIA)
      // TODO(scottmg): .so files are currently deployed into /boot/lib, where
      // they'll be found (without a path) by the loader. Application packaging
      // infrastructure is in progress, so this will likely change again in the
      // future.
      directory = base::FilePath();
#endif
      break;

    case FileType::kCertificate:
#if defined(CRASHPAD_IS_IN_FUCHSIA)
      directory = base::FilePath(FILE_PATH_LITERAL("/pkg/data"));
#endif
      extension = FILE_PATH_LITERAL(".pem");
      break;
  }

  return directory.Append(test_name + FILE_PATH_LITERAL("_") + artifact +
                          extension);
}

#if BUILDFLAG(IS_WIN) && defined(ARCH_CPU_64_BITS)

// static
bool TestPaths::Has32BitBuildArtifacts() {
  return !Output32BitDirectory().empty();
}

#endif  // BUILDFLAG(IS_WIN) && defined(ARCH_CPU_64_BITS)

}  // namespace test
}  // namespace crashpad
