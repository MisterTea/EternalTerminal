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

#include "client/crash_report_database.h"

#include <sys/stat.h>

#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "util/file/directory_reader.h"
#include "util/file/file_helper.h"
#include "util/file/filesystem.h"

#include <mpack.h>

namespace crashpad {

namespace {
constexpr base::FilePath::CharType kAttachmentsDirectory[] =
    FILE_PATH_LITERAL("attachments");

std::string FixAttachmentName(std::string name) {
  std::replace_if(name.begin(), name.end(), [&](char c) 
  { 
      return c != '_' && c != '-' && c != '.' && !isalnum(c);
  }, '_');
  
  return name;
}

// If the file exists, "-N" is appended to the base name before the extension,
// where N is a number starting from 1.
base::FilePath EnsureUniqueFile(const base::FilePath& dir,
                                const base::FilePath::StringType& filename) {
  const base::FilePath path = dir.Append(filename);
  if (!IsRegularFile(path)) {
    return path;
  }

  // support common double extensions like ".tar.gz"
  const base::FilePath basename =
      path.BaseName().RemoveFinalExtension().RemoveFinalExtension();
  const base::FilePath::StringType extension =
      path.RemoveFinalExtension().FinalExtension() + path.FinalExtension();

  // find the next available "filename-N.ext"
  size_t n = 1;
  constexpr size_t max_n = 4096;  // arbitrary but reasonable limit to break out
  base::FilePath unique = path;
  do {
#if BUILDFLAG(IS_WIN)
    base::FilePath::StringType ns = std::to_wstring(n);
#else
    base::FilePath::StringType ns = std::to_string(n);
#endif
    base::FilePath::StringType filename_n =
        basename.value() + FILE_PATH_LITERAL("-") + ns + extension;
    unique = dir.Append(filename_n);
  } while (IsRegularFile(unique) && ++n < max_n);

  if (n >= max_n) {
    LOG(ERROR) << "failed to find a unique file name for "
               << base::FilePath(filename);
    return path;
  }

  return unique;
}

// Escapes a string for JSON (double quotes, backslash, control chars)
std::string EscapeJsonString(const std::string& input) {
  std::string output;
  output.reserve(input.size() + 8);
  for (char c : input) {
    switch (c) {
      case '\"':
        output += "\\\"";
        break;
      case '\\':
        output += "\\\\";
        break;
      case '\b':
        output += "\\b";
        break;
      case '\f':
        output += "\\f";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          output += buf;
        } else {
          output += c;
        }
    }
  }
  return output;
}

std::string MpackToJsonString(mpack_node_t node) {
  switch (mpack_node_type(node)) {
    case mpack_type_nil:
      return "null";
    case mpack_type_bool:
      return mpack_node_bool(node) ? "true" : "false";
    case mpack_type_int:
      return std::to_string(mpack_node_i64(node));
    case mpack_type_uint:
      return std::to_string(mpack_node_u64(node));
    case mpack_type_float:
      return std::to_string(mpack_node_float(node));
    case mpack_type_double:
      return std::to_string(mpack_node_double(node));
    case mpack_type_str: {
      return "\"" +
             EscapeJsonString(
                 std::string(mpack_node_str(node), mpack_node_strlen(node))) +
             "\"";
    }
    case mpack_type_array: {
      std::string result = "[";
      size_t n = mpack_node_array_length(node);
      for (size_t i = 0; i < n; ++i) {
        if (i > 0) {
          result += ",";
        }
        result += MpackToJsonString(mpack_node_array_at(node, i));
      }
      result += "]";
      return result;
    }
    case mpack_type_map: {
      std::string result = "{";
      size_t n = mpack_node_map_count(node);
      for (size_t i = 0; i < n; ++i) {
        if (i > 0) {
          result += ",";
        }
        mpack_node_t key = mpack_node_map_key_at(node, i);
        mpack_node_t val = mpack_node_map_value_at(node, i);
        std::string key_str(mpack_node_str(key), mpack_node_strlen(key));
        result += "\"" + key_str + "\":" + MpackToJsonString(val);
      }
      result += "}";
      return result;
    }
    default:
      LOG(ERROR) << "Unsupported mpack node type: " << mpack_node_type(node);
      return "";
  }
}

}  // namespace

CrashReportDatabase::Report::Report()
    : uuid(),
      file_path(),
      id(),
      creation_time(0),
      uploaded(false),
      last_upload_attempt_time(0),
      upload_attempts(0),
      upload_explicitly_requested(false),
      total_size(0u) {}

CrashReportDatabase::NewReport::NewReport()
    : writer_(std::make_unique<FileWriter>()),
      file_remover_(),
      attachment_writers_(),
      attachment_removers_(),
      uuid_(),
      database_() {}

CrashReportDatabase::NewReport::~NewReport() = default;

bool CrashReportDatabase::NewReport::Initialize(
    CrashReportDatabase* database,
    const base::FilePath& directory,
    const base::FilePath::StringType& extension,
    const UUID* uuid) {
  database_ = database;

  if (uuid && !uuid->IsZero()) {
    uuid_ = *uuid;
  } else if (!uuid_.InitializeWithNew()) {
    return false;
  }

#if BUILDFLAG(IS_WIN)
  const std::wstring uuid_string = uuid_.ToWString();
#else
  const std::string uuid_string = uuid_.ToString();
#endif

  const base::FilePath path = directory.Append(uuid_string + extension);
  if (!writer_->Open(
          path, FileWriteMode::kCreateOrFail, FilePermissions::kOwnerOnly)) {
    return false;
  }
  file_remover_.reset(path);
  return true;
}

FileReaderInterface* CrashReportDatabase::NewReport::Reader() {
  auto reader = std::make_unique<FileReader>();
  if (!reader->Open(file_remover_.get())) {
    return nullptr;
  }
  reader_ = std::move(reader);
  return reader_.get();
}

FileWriter* CrashReportDatabase::NewReport::AddAttachment(
    const std::string& name) {
  base::FilePath report_attachments_dir = database_->AttachmentsPath(uuid_);
  if (!LoggingCreateDirectory(
          report_attachments_dir, FilePermissions::kOwnerOnly, true)) {
    return nullptr;
  }
#if BUILDFLAG(IS_WIN)
  const std::wstring name_string = base::UTF8ToWide(FixAttachmentName(name));
#else
  const std::string name_string = FixAttachmentName(name);
#endif
  base::FilePath attachment_path =
      EnsureUniqueFile(report_attachments_dir, name_string);
  auto writer = std::make_unique<FileWriter>();
  if (!writer->Open(attachment_path,
                    FileWriteMode::kCreateOrFail,
                    FilePermissions::kOwnerOnly)) {
    return nullptr;
  }
  attachment_writers_.emplace_back(std::move(writer));
  attachment_removers_.emplace_back(ScopedRemoveFile(attachment_path));
  return attachment_writers_.back().get();
}

void CrashReportDatabase::UploadReport::InitializeAttachments() {
  base::FilePath report_attachments_dir = database_->AttachmentsPath(uuid);
  DirectoryReader dir_reader;
  if (!dir_reader.Open(report_attachments_dir)) {
    return;
  }

  base::FilePath filename;
  DirectoryReader::Result dir_result;
  while ((dir_result = dir_reader.NextFile(&filename)) ==
         DirectoryReader::Result::kSuccess) {
    const base::FilePath filepath(report_attachments_dir.Append(filename));
    std::unique_ptr<FileReader> file_reader(std::make_unique<FileReader>());
    if (!file_reader->Open(filepath)) {
      continue;
    }
    attachment_readers_.emplace_back(std::move(file_reader));
#if BUILDFLAG(IS_WIN)
    const std::string name_string = base::WideToUTF8(filename.value());
#else
    const std::string name_string = filename.value();
#endif
    attachment_map_[name_string] = attachment_readers_.back().get();
  }
}

CrashReportDatabase::UploadReport::UploadReport()
    : Report(),
      reader_(std::make_unique<FileReader>()),
      database_(nullptr),
      attachment_readers_(),
      attachment_map_(),
      report_metrics_(false) {}

CrashReportDatabase::UploadReport::~UploadReport() {
  if (database_) {
    database_->RecordUploadAttempt(this, false, std::string());
  }
}

bool CrashReportDatabase::UploadReport::Initialize(const base::FilePath& path,
                                                   CrashReportDatabase* db) {
  database_ = db;
  InitializeAttachments();
  return reader_->Open(path);
}

bool CrashReportDatabase::Envelope::Initialize(const base::FilePath& path) {
  path_ = path;
  if (path.empty()) {
    return false;
  }
  writer_ = std::make_unique<FileWriter>();
  if (!writer_->Open(
          path, FileWriteMode::kReuseOrCreate, FilePermissions::kOwnerOnly)) {
    return false;
  }
  writer_->Seek(0, SEEK_END);
  return true;
}

CrashReportDatabase::Envelope::Envelope(const UUID& uuid) : uuid_(uuid) {}

void CrashReportDatabase::Envelope::AddAttachments(
    const std::vector<base::FilePath>& attachments) {
  base::FilePath event;
  std::vector<base::FilePath> breadcrumbs;
  std::vector<base::FilePath> others;

  for (const auto& attachment : attachments) {
#if BUILDFLAG(IS_WIN)
    std::string basename = base::WideToUTF8(attachment.BaseName().value());
#else
    std::string basename = attachment.BaseName().value();
#endif
    if (basename == "__sentry-event") {
      event = attachment;
    } else if (basename.rfind("__sentry-breadcrumb", 0) == 0) {
      breadcrumbs.push_back(attachment);
    } else {
      others.push_back(attachment);
    }
  }

  AddEvent(event, breadcrumbs);
  for (const auto& attachment : others) {
    AddAttachment(attachment);
  }
}

void CrashReportDatabase::Envelope::AddMinidump(FileReaderInterface* reader) {
  FileOffset size = reader->Seek(0, SEEK_END);
  std::string header = base::StringPrintf(
      "{\"type\":\"attachment\","
      "\"length\":%zu,"
      "\"attachment_type\":\"event.minidump\","
      "\"filename\":\"%s.dmp\"}",
      static_cast<size_t>(size),
      uuid_.ToString().c_str());
  writer_->Write("\n", 1);
  writer_->Write(header.data(), header.size());
  writer_->Write("\n", 1);
  reader->Seek(0, SEEK_SET);
  CopyFileContent(reader, writer_.get());
}

void CrashReportDatabase::Envelope::AddEvent(
    const base::FilePath& event,
    const std::vector<base::FilePath>& breadcrumbs) {
  std::string event_data;
  if (!LoggingReadEntireFile(event, &event_data)) {
    return;
  }

  mpack_tree_t event_obj;
  mpack_tree_init_data(&event_obj, event_data.data(), event_data.size());
  mpack_tree_parse(&event_obj);
  std::string event_json = MpackToJsonString(mpack_tree_root(&event_obj));
  mpack_tree_destroy(&event_obj);

  // read all breadcrumb files
  size_t max_breadcrumbs = 0;
  std::vector<std::unique_ptr<mpack_tree_t, mpack_error_t (*)(mpack_tree_t*)>>
      all_breadcrumbs;
  std::vector<std::string> breadcrumb_datas(breadcrumbs.size());
  for (size_t i = 0; i < breadcrumbs.size(); ++i) {
    auto& breadcrumb_data = breadcrumb_datas[i];
    if (!LoggingReadEntireFile(breadcrumbs[i], &breadcrumb_data)) {
      continue;
    }

    size_t count = 0;
    size_t offset = 0;
    while (offset < breadcrumb_data.size()) {
      auto breadcrumb_obj =
          std::unique_ptr<mpack_tree_t, mpack_error_t (*)(mpack_tree_t*)>(
              new mpack_tree_t, mpack_tree_destroy);
      mpack_tree_init_data(breadcrumb_obj.get(),
                           breadcrumb_data.data() + offset,
                           breadcrumb_data.size() - offset);
      mpack_tree_parse(breadcrumb_obj.get());

      size_t size = mpack_tree_size(breadcrumb_obj.get());
      all_breadcrumbs.push_back(std::move(breadcrumb_obj));

      offset += size;
      count++;
    }
    max_breadcrumbs = std::max(max_breadcrumbs, count);
  }

  // sort breadcrumbs by timestamp and limit to max_breadcrumbs
  std::sort(all_breadcrumbs.begin(),
            all_breadcrumbs.end(),
            [](const auto& a, const auto& b) {
              mpack_node_t ts_a =
                  mpack_node_map_cstr(mpack_tree_root(a.get()), "timestamp");
              mpack_node_t ts_b =
                  mpack_node_map_cstr(mpack_tree_root(b.get()), "timestamp");
              return strcmp(mpack_node_str(ts_a), mpack_node_str(ts_b)) < 0;
            });
  std::string breadcrumbs_json;
  size_t start = std::max<size_t>(0, all_breadcrumbs.size() - max_breadcrumbs);
  for (size_t i = start; i < all_breadcrumbs.size(); ++i) {
    if (!breadcrumbs_json.empty()) {
      breadcrumbs_json += ",";
    }
    const auto& breadcrumb_tree = all_breadcrumbs[i];
    breadcrumbs_json +=
        MpackToJsonString(mpack_tree_root(breadcrumb_tree.get()));
  }

  // write event with breadcrumbs
  event_json.erase(0, 1);  // leading '{'
  event_json.pop_back();  // trailing '}'
  std::string payload = base::StringPrintf("{%s,\"breadcrumbs\":[%s]}",
                                           event_json.c_str(),
                                           breadcrumbs_json.c_str());
  std::string header = base::StringPrintf(
      "{\"type\":\"event\","
      "\"length\":%zu}",
      payload.size());
  writer_->Write("\n", 1);
  writer_->Write(header.data(), header.size());
  writer_->Write("\n", 1);
  writer_->Write(payload.data(), payload.size());
  writer_->Write("\n", 1);
}

void CrashReportDatabase::Envelope::AddAttachment(
    const base::FilePath& attachment) {
  std::string payload;
  if (!LoggingReadEntireFile(attachment, &payload)) {
    return;
  }

#if BUILDFLAG(IS_WIN)
  std::string basename = base::WideToUTF8(attachment.BaseName().value());
#else
  std::string basename = attachment.BaseName().value();
#endif

  std::string header = base::StringPrintf(
      "{\"type\":\"attachment\","
      "\"length\":%zu,"
      "\"attachment_type\":\"event.attachment\","
      "\"filename\": \"%s\"}",
      payload.size(),
      EscapeJsonString(basename).c_str());
  writer_->Write("\n", 1);
  writer_->Write(header.data(), header.size());
  writer_->Write("\n", 1);
  writer_->Write(payload.data(), payload.size());
  writer_->Write("\n", 1);
}

void CrashReportDatabase::Envelope::Finish() {
  writer_->Close();
}

CrashReportDatabase::OperationStatus CrashReportDatabase::RecordUploadComplete(
    std::unique_ptr<const UploadReport> report_in,
    const std::string& id) {
  UploadReport* report = const_cast<UploadReport*>(report_in.get());

  report->database_ = nullptr;
  return RecordUploadAttempt(report, true, id);
}

base::FilePath CrashReportDatabase::AttachmentsPath(const UUID& uuid) {
#if BUILDFLAG(IS_WIN)
  const std::wstring uuid_string = uuid.ToWString();
#else
  const std::string uuid_string = uuid.ToString();
#endif

  return DatabasePath().Append(kAttachmentsDirectory).Append(uuid_string);
}

base::FilePath CrashReportDatabase::AttachmentsRootPath() {
  return DatabasePath().Append(kAttachmentsDirectory);
}

void CrashReportDatabase::RemoveAttachmentsByUUID(const UUID& uuid) {
  base::FilePath report_attachment_dir = AttachmentsPath(uuid);
  if (!IsDirectory(report_attachment_dir, /*allow_symlinks=*/false)) {
    return;
  }
  DirectoryReader reader;
  if (!reader.Open(report_attachment_dir)) {
    return;
  }

  base::FilePath filename;
  DirectoryReader::Result result;
  while ((result = reader.NextFile(&filename)) ==
         DirectoryReader::Result::kSuccess) {
    const base::FilePath attachment_path(
        report_attachment_dir.Append(filename));
    LoggingRemoveFile(attachment_path);
  }

  LoggingRemoveDirectory(report_attachment_dir);
}

}  // namespace crashpad
