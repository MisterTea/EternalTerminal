// Copyright 2018 The Crashpad Authors. All rights reserved.
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

#ifndef CRASHPAD_HANDLER_LINUX_CRASH_REPORT_EXCEPTION_HANDLER_H_
#define CRASHPAD_HANDLER_LINUX_CRASH_REPORT_EXCEPTION_HANDLER_H_

#include <map>
#include <string>

#include "client/crash_report_database.h"
#include "handler/crash_report_upload_thread.h"
#include "handler/linux/exception_handler_server.h"
#include "handler/user_stream_data_source.h"
#include "util/linux/exception_handler_protocol.h"
#include "util/linux/ptrace_connection.h"
#include "util/misc/address_types.h"
#include "util/misc/uuid.h"

namespace crashpad {

class ProcessSnapshotLinux;
class ProcessSnapshotSanitized;

//! \brief An exception handler that writes crash reports for exceptions
//!     to a CrashReportDatabase.
class CrashReportExceptionHandler : public ExceptionHandlerServer::Delegate {
 public:
  //! \brief Creates a new object that will store crash reports in \a database.
  //!
  //! \param[in] database The database to store crash reports in. Weak.
  //! \param[in] upload_thread The upload thread to notify when a new crash
  //!     report is written into \a database. Report upload is skipped if this
  //!     value is `nullptr`.
  //! \param[in] process_annotations A map of annotations to insert as
  //!     process-level annotations into each crash report that is written. Do
  //!     not confuse this with module-level annotations, which are under the
  //!     control of the crashing process, and are used to implement Chrome’s
  //!     “crash keys.” Process-level annotations are those that are beyond the
  //!     control of the crashing process, which must reliably be set even if
  //!     the process crashes before it’s able to establish its own annotations.
  //!     To interoperate with Breakpad servers, the recommended practice is to
  //!     specify values for the `"prod"` and `"ver"` keys as process
  //!     annotations.
  //! \param[in] attachments A vector of file paths that should be captured with
  //!     each report at the time of the crash.
  //! \param[in] write_minidump_to_database Whether the minidump shall be
  //!     written to database.
  //! \param[in] write_minidump_to_log Whether the minidump shall be written to
  //!     log.
  //! \param[in] user_stream_data_sources Data sources to be used to extend
  //!     crash reports. For each crash report that is written, the data sources
  //!     are called in turn. These data sources may contribute additional
  //!     minidump streams. `nullptr` if not required.
  CrashReportExceptionHandler(
      CrashReportDatabase* database,
      CrashReportUploadThread* upload_thread,
      const std::map<std::string, std::string>* process_annotations,
      const std::vector<base::FilePath>* attachments,
      bool write_minidump_to_database,
      bool write_minidump_to_log,
      const UserStreamDataSources* user_stream_data_sources);

  CrashReportExceptionHandler(const CrashReportExceptionHandler&) = delete;
  CrashReportExceptionHandler& operator=(const CrashReportExceptionHandler&) =
      delete;

  ~CrashReportExceptionHandler() override;

  // ExceptionHandlerServer::Delegate:

  bool HandleException(pid_t client_process_id,
                       uid_t client_uid,
                       const ExceptionHandlerProtocol::ClientInformation& info,
                       VMAddress requesting_thread_stack_address = 0,
                       pid_t* requesting_thread_id = nullptr,
                       UUID* local_report_id = nullptr) override;

  bool HandleExceptionWithBroker(
      pid_t client_process_id,
      uid_t client_uid,
      const ExceptionHandlerProtocol::ClientInformation& info,
      int broker_sock,
      UUID* local_report_id = nullptr) override;

 private:
  bool HandleExceptionWithConnection(
      PtraceConnection* connection,
      const ExceptionHandlerProtocol::ClientInformation& info,
      uid_t client_uid,
      VMAddress requesting_thread_stack_address,
      pid_t* requesting_thread_id,
      UUID* local_report_id = nullptr);

  bool WriteMinidumpToDatabase(ProcessSnapshotLinux* process_snapshot,
                               ProcessSnapshotSanitized* sanitized_snapshot,
                               bool write_minidump_to_log,
                               UUID* local_report_id);
  bool WriteMinidumpToLog(ProcessSnapshotLinux* process_snapshot,
                          ProcessSnapshotSanitized* sanitized_snapshot);

  CrashReportDatabase* database_;  // weak
  CrashReportUploadThread* upload_thread_;  // weak
  const std::map<std::string, std::string>* process_annotations_;  // weak
  const std::vector<base::FilePath>* attachments_;  // weak
  bool write_minidump_to_database_;
  bool write_minidump_to_log_;
  const UserStreamDataSources* user_stream_data_sources_;  // weak
};

}  // namespace crashpad

#endif  // CRASHPAD_HANDLER_LINUX_CRASH_REPORT_EXCEPTION_HANDLER_H_
