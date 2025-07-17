// Copyright 2020 The Crashpad Authors. All rights reserved.
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

#import "test/ios/host/cptest_application_delegate.h"
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_images.h>
#include <mach-o/nlist.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vector>

#import "Service/Sources/EDOHostNamingService.h"
#import "Service/Sources/EDOHostService.h"
#import "Service/Sources/NSObject+EDOValueObject.h"
#include "base/strings/sys_string_conversions.h"
#include "client/annotation.h"
#include "client/annotation_list.h"
#include "client/crash_report_database.h"
#include "client/crashpad_client.h"
#include "client/crashpad_info.h"
#include "client/simple_string_dictionary.h"
#include "snapshot/minidump/process_snapshot_minidump.h"
#import "test/ios/host/cptest_crash_view_controller.h"
#import "test/ios/host/cptest_shared_object.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

using OperationStatus = crashpad::CrashReportDatabase::OperationStatus;
using Report = crashpad::CrashReportDatabase::Report;

namespace {

base::FilePath GetDatabaseDir() {
  base::FilePath database_dir([NSFileManager.defaultManager
                                  URLsForDirectory:NSDocumentDirectory
                                         inDomains:NSUserDomainMask]
                                  .lastObject.path.UTF8String);
  return database_dir.Append("crashpad");
}

std::unique_ptr<crashpad::CrashReportDatabase> GetDatabase() {
  base::FilePath database_dir = GetDatabaseDir();
  std::unique_ptr<crashpad::CrashReportDatabase> database =
      crashpad::CrashReportDatabase::Initialize(database_dir);
  return database;
}

OperationStatus GetPendingReports(std::vector<Report>* pending_reports) {
  std::unique_ptr<crashpad::CrashReportDatabase> database(GetDatabase());
  return database->GetPendingReports(pending_reports);
}

[[clang::optnone]] void recurse(int counter) {
  // Fill up the stack faster.
  int arr[1024];
  arr[0] = counter;
  if (counter > INT_MAX)
    return;
  recurse(++counter);
}

}  // namespace

@interface CPTestApplicationDelegate ()
- (void)processIntermediateDumps;
@end

@implementation CPTestApplicationDelegate {
  crashpad::CrashpadClient client_;
}

@synthesize window = _window;

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
  // Start up crashpad.
  std::map<std::string, std::string> annotations = {
      {"prod", "xcuitest"}, {"ver", "1"}, {"plat", "iOS"}, {"crashpad", "yes"}};

  NSArray<NSString*>* arguments = [[NSProcessInfo processInfo] arguments];
  if ([arguments containsObject:@"--alternate-client-annotations"]) {
    annotations = {{"prod", "some_app"},
                   {"ver", "42"},
                   {"plat", "macOS"},
                   {"crashpad", "no"}};
  }
  if (client_.StartCrashpadInProcessHandler(
          GetDatabaseDir(), "", annotations)) {
    client_.ProcessIntermediateDumps();
  }

  self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
  [self.window makeKeyAndVisible];
  self.window.backgroundColor = UIColor.greenColor;

  CPTestCrashViewController* controller =
      [[CPTestCrashViewController alloc] init];
  self.window.rootViewController = controller;

  // Start up EDO.
  [EDOHostService serviceWithPort:12345
                       rootObject:[[CPTestSharedObject alloc] init]
                            queue:dispatch_get_main_queue()];

  return YES;
}

- (void)processIntermediateDumps {
  client_.ProcessIntermediateDumps();
}

@end

@implementation CPTestSharedObject

- (NSString*)testEDO {
  return @"crashpad";
}

- (void)processIntermediateDumps {
  CPTestApplicationDelegate* delegate =
      (CPTestApplicationDelegate*)UIApplication.sharedApplication.delegate;
  [delegate processIntermediateDumps];
}

- (void)clearPendingReports {
  std::unique_ptr<crashpad::CrashReportDatabase> database(GetDatabase());
  std::vector<crashpad::CrashReportDatabase::Report> pending_reports;
  database->GetPendingReports(&pending_reports);
  for (auto report : pending_reports) {
    database->DeleteReport(report.uuid);
  }
}

- (int)pendingReportCount {
  std::vector<Report> pending_reports;
  OperationStatus status = GetPendingReports(&pending_reports);
  if (status != crashpad::CrashReportDatabase::kNoError) {
    return -1;
  }
  return pending_reports.size();
}

- (int)pendingReportException {
  std::vector<Report> pending_reports;
  OperationStatus status = GetPendingReports(&pending_reports);
  if (status != crashpad::CrashReportDatabase::kNoError ||
      pending_reports.size() != 1) {
    return -1;
  }

  auto reader = std::make_unique<crashpad::FileReader>();
  reader->Open(pending_reports[0].file_path);
  crashpad::ProcessSnapshotMinidump process_snapshot;
  process_snapshot.Initialize(reader.get());
  return static_cast<int>(process_snapshot.Exception()->Exception());
}

- (NSDictionary*)getAnnotations {
  std::vector<Report> pending_reports;
  OperationStatus status = GetPendingReports(&pending_reports);
  if (status != crashpad::CrashReportDatabase::kNoError ||
      pending_reports.size() != 1) {
    return @{};
  }

  auto reader = std::make_unique<crashpad::FileReader>();
  reader->Open(pending_reports[0].file_path);
  crashpad::ProcessSnapshotMinidump process_snapshot;
  process_snapshot.Initialize(reader.get());

  NSDictionary* dict = @{
    @"simplemap" : [@{} mutableCopy],
    @"vector" : [@[] mutableCopy],
    @"objects" : [@[] mutableCopy]
  };
  for (const auto* module : process_snapshot.Modules()) {
    for (const auto& kv : module->AnnotationsSimpleMap()) {
      [dict[@"simplemap"] setValue:@(kv.second.c_str())
                            forKey:@(kv.first.c_str())];
    }
    for (const std::string& annotation : module->AnnotationsVector()) {
      [dict[@"vector"] addObject:@(annotation.c_str())];
    }
    for (const auto& annotation : module->AnnotationObjects()) {
      if (annotation.type !=
          static_cast<uint16_t>(crashpad::Annotation::Type::kString)) {
        continue;
      }
      std::string value(reinterpret_cast<const char*>(annotation.value.data()),
                        annotation.value.size());
      [dict[@"objects"]
          addObject:@{@(annotation.name.c_str()) : @(value.c_str())}];
    }
  }
  return [dict passByValue];
}

- (NSDictionary*)getProcessAnnotations {
  std::vector<Report> pending_reports;
  OperationStatus status = GetPendingReports(&pending_reports);
  if (status != crashpad::CrashReportDatabase::kNoError ||
      pending_reports.size() != 1) {
    return @{};
  }

  auto reader = std::make_unique<crashpad::FileReader>();
  reader->Open(pending_reports[0].file_path);
  crashpad::ProcessSnapshotMinidump process_snapshot;
  process_snapshot.Initialize(reader.get());
  NSDictionary* dict = [@{} mutableCopy];
  for (const auto& kv : process_snapshot.AnnotationsSimpleMap()) {
    [dict setValue:@(kv.second.c_str()) forKey:@(kv.first.c_str())];
  }

  return [dict passByValue];
}

- (void)crashBadAccess {
  strcpy(nullptr, "bla");
}

- (void)crashKillAbort {
  kill(getpid(), SIGABRT);
}

- (void)crashSegv {
  long* zero = nullptr;
  *zero = 0xc045004d;
}

- (void)crashTrap {
  __builtin_trap();
}

- (void)crashAbort {
  abort();
}

- (void)crashException {
  std::vector<int> empty_vector = {};
  empty_vector.at(42);
}

- (void)crashNSException {
  // EDO has its own sinkhole, so dispatch this away.
  dispatch_async(dispatch_get_main_queue(), ^{
    NSError* error = [NSError errorWithDomain:@"com.crashpad.xcuitests"
                                         code:200
                                     userInfo:@{@"Error Object" : self}];

    [[NSException exceptionWithName:NSInternalInconsistencyException
                             reason:@"Intentionally throwing error."
                           userInfo:@{NSUnderlyingErrorKey : error}] raise];
  });
}

- (void)crashUnrecognizedSelectorAfterDelay {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wundeclared-selector"
  [self performSelector:@selector(does_not_exist) withObject:nil afterDelay:1];
#pragma clang diagnostic pop
}

- (void)catchNSException {
  @try {
    NSArray* empty_array = @[];
    [empty_array objectAtIndex:42];
  } @catch (NSException* exception) {
  } @finally {
  }
}

- (void)crashCoreAutoLayoutSinkhole {
  // EDO has its own sinkhole, so dispatch this away.
  dispatch_async(dispatch_get_main_queue(), ^{
    UIView* unattachedView = [[UIView alloc] init];
    UIWindow* window = [UIApplication sharedApplication].windows[0];
    [NSLayoutConstraint activateConstraints:@[
      [window.rootViewController.view.bottomAnchor
          constraintEqualToAnchor:unattachedView.bottomAnchor],
    ]];
  });
}

- (void)crashRecursion {
  recurse(0);
}

- (void)crashWithCrashInfoMessage {
  dlsym(nullptr, nullptr);
}

- (void)crashWithDyldErrorString {
  std::string crashy_initializer =
      base::SysNSStringToUTF8([[NSBundle mainBundle]
          pathForResource:@"crashpad_snapshot_test_module_crashy_initializer"
                   ofType:@"so"]);
  dlopen(crashy_initializer.c_str(), RTLD_LAZY | RTLD_LOCAL);
}

- (void)crashWithAnnotations {
  // This is “leaked” to crashpad_info.
  crashpad::SimpleStringDictionary* simple_annotations =
      new crashpad::SimpleStringDictionary();
  simple_annotations->SetKeyValue("#TEST# pad", "break");
  simple_annotations->SetKeyValue("#TEST# key", "value");
  simple_annotations->SetKeyValue("#TEST# pad", "crash");
  simple_annotations->SetKeyValue("#TEST# x", "y");
  simple_annotations->SetKeyValue("#TEST# longer", "shorter");
  simple_annotations->SetKeyValue("#TEST# empty_value", "");

  crashpad::CrashpadInfo* crashpad_info =
      crashpad::CrashpadInfo::GetCrashpadInfo();

  crashpad_info->set_simple_annotations(simple_annotations);

  crashpad::AnnotationList::Register();  // This is “leaked” to crashpad_info.

  static crashpad::StringAnnotation<32> test_annotation_one{"#TEST# one"};
  static crashpad::StringAnnotation<32> test_annotation_two{"#TEST# two"};
  static crashpad::StringAnnotation<32> test_annotation_three{
      "#TEST# same-name"};
  static crashpad::StringAnnotation<32> test_annotation_four{
      "#TEST# same-name"};

  test_annotation_one.Set("moocow");
  test_annotation_two.Set("this will be cleared");
  test_annotation_three.Set("same-name 3");
  test_annotation_four.Set("same-name 4");
  test_annotation_two.Clear();
  abort();
}

@end
