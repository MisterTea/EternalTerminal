#include <stdio.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "client/crash_report_database.h"
#include "client/crashpad_client.h"
#include "client/crashpad_info.h"
#include "client/settings.h"

using namespace crashpad;

int init_crashpad() {
  // Cache directory that will store crashpad information and minidumps
  base::FilePath database("crashpad.db");
  // Path to the out-of-process handler executable
  base::FilePath handler("./out/Default/crashpad_handler");
  // URL used to submit minidumps to
  std::string url(
      "http://localhost:8000/api/5/minidump/"
      "?sentry_key=36811373240a4fc6b25f3040693462d5");
  // Optional annotations passed via --annotations to the handler
  std::map<std::string, std::string> annotations;
  // Optional arguments to pass to the handler
  std::vector<std::string> arguments;

  arguments.push_back("--no-rate-limit");

  std::map<std::string, base::FilePath> attachments;
  attachments["attch_log_bla.txt"] = base::FilePath("/tmp/log.txt");

  CrashpadClient client;
  bool success = client.StartHandlerWithAttachments(handler,
                                     database,
                                     database,
                                     url,
                                     annotations,
                                     attachments,
                                     arguments,
                                     /* restartable */ true,
                                     /* asynchronous_start */ false);

  if (success) {
    printf("Started client handler.\n");
  } else {
    printf("Failed to start client handler.\n");
  }

  if (!success) {
    return 1;
  }

  std::unique_ptr<CrashReportDatabase> db =
      CrashReportDatabase::Initialize(database);

  if (db != nullptr && db->GetSettings() != nullptr) {
    db->GetSettings()->SetUploadsEnabled(true);
  }

  // Ensure that the simple annotations dictionary is set in the client.
  CrashpadInfo* crashpad_info = CrashpadInfo::GetCrashpadInfo();

  return 0;
}

void crash(uint sleep_sec) {
  std::cerr << "Prepare to crash, sleeping for " << sleep_sec << " second(s)\n";
  std::this_thread::sleep_for(std::chrono::seconds(sleep_sec));
  memset((char*)0x0, 1, 100);
}

int main(int args, char* argv[]) {
  init_crashpad();

  const uint sleep_sec = args > 1 ? std::stoi(argv[1]) : 1;
  crash(sleep_sec);
}
