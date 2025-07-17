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

#include "snapshot/minidump/process_snapshot_minidump.h"

#include <utility>

#include "base/logging.h"
#include "base/notreached.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "minidump/minidump_extensions.h"
#include "snapshot/memory_map_region_snapshot.h"
#include "snapshot/minidump/minidump_simple_string_dictionary_reader.h"
#include "snapshot/minidump/minidump_string_reader.h"
#include "util/file/file_io.h"

namespace crashpad {

namespace internal {

class MemoryMapRegionSnapshotMinidump : public MemoryMapRegionSnapshot {
 public:
  MemoryMapRegionSnapshotMinidump(MINIDUMP_MEMORY_INFO info) : info_(info) {}
  ~MemoryMapRegionSnapshotMinidump() override = default;

  const MINIDUMP_MEMORY_INFO& AsMinidumpMemoryInfo() const override {
    return info_;
  }

 private:
  MINIDUMP_MEMORY_INFO info_;
};

}  // namespace internal

ProcessSnapshotMinidump::ProcessSnapshotMinidump()
    : ProcessSnapshot(),
      header_(),
      stream_directory_(),
      stream_map_(),
      modules_(),
      threads_(),
      unloaded_modules_(),
      mem_regions_(),
      mem_regions_exposed_(),
      custom_streams_(),
      crashpad_info_(),
      system_snapshot_(),
      exception_snapshot_(),
      arch_(CPUArchitecture::kCPUArchitectureUnknown),
      annotations_simple_map_(),
      file_reader_(nullptr),
      process_id_(kInvalidProcessID),
      create_time_(0),
      user_time_(0),
      kernel_time_(0),
      initialized_() {}

ProcessSnapshotMinidump::~ProcessSnapshotMinidump() {}

bool ProcessSnapshotMinidump::Initialize(FileReaderInterface* file_reader) {
  INITIALIZATION_STATE_SET_INITIALIZING(initialized_);

  file_reader_ = file_reader;

  if (!file_reader_->SeekSet(0)) {
    return false;
  }

  if (!file_reader_->ReadExactly(&header_, sizeof(header_))) {
    return false;
  }

  if (header_.Signature != MINIDUMP_SIGNATURE) {
    LOG(ERROR) << "minidump signature mismatch";
    return false;
  }

  if (header_.Version != MINIDUMP_VERSION) {
    LOG(ERROR) << "minidump version mismatch";
    return false;
  }

  if (!file_reader->SeekSet(header_.StreamDirectoryRva)) {
    return false;
  }

  stream_directory_.resize(header_.NumberOfStreams);
  if (!stream_directory_.empty() &&
      !file_reader_->ReadExactly(
          &stream_directory_[0],
          header_.NumberOfStreams * sizeof(stream_directory_[0]))) {
    return false;
  }

  for (const MINIDUMP_DIRECTORY& directory : stream_directory_) {
    const MinidumpStreamType stream_type =
        static_cast<MinidumpStreamType>(directory.StreamType);
    if (stream_map_.find(stream_type) != stream_map_.end()) {
      LOG(ERROR) << "duplicate streams for type " << directory.StreamType;
      return false;
    }

    stream_map_[stream_type] = &directory.Location;
  }

  if (!InitializeCrashpadInfo() || !InitializeMiscInfo() ||
      !InitializeModules() || !InitializeSystemSnapshot() ||
      !InitializeMemoryInfo() || !InitializeExtraMemory() ||
      !InitializeThreads() || !InitializeCustomMinidumpStreams() ||
      !InitializeExceptionSnapshot()) {
    return false;
  }

  INITIALIZATION_STATE_SET_VALID(initialized_);
  return true;
}

crashpad::ProcessID ProcessSnapshotMinidump::ProcessID() const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  return process_id_;
}

crashpad::ProcessID ProcessSnapshotMinidump::ParentProcessID() const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  NOTREACHED();  // https://crashpad.chromium.org/bug/10
  return 0;
}

void ProcessSnapshotMinidump::SnapshotTime(timeval* snapshot_time) const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  snapshot_time->tv_sec = header_.TimeDateStamp;
  snapshot_time->tv_usec = 0;
}

void ProcessSnapshotMinidump::ProcessStartTime(timeval* start_time) const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  start_time->tv_sec = create_time_;
  start_time->tv_usec = 0;
}

void ProcessSnapshotMinidump::ProcessCPUTimes(timeval* user_time,
                                              timeval* system_time) const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  user_time->tv_sec = user_time_;
  user_time->tv_usec = 0;
  system_time->tv_sec = kernel_time_;
  system_time->tv_usec = 0;
}

void ProcessSnapshotMinidump::ReportID(UUID* report_id) const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  *report_id = crashpad_info_.report_id;
}

void ProcessSnapshotMinidump::ClientID(UUID* client_id) const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  *client_id = crashpad_info_.client_id;
}

const std::map<std::string, std::string>&
ProcessSnapshotMinidump::AnnotationsSimpleMap() const {
  // TODO(mark): This method should not be const, although the interface
  // currently imposes this requirement. Making it non-const would allow
  // annotations_simple_map_ to be lazily constructed: InitializeCrashpadInfo()
  // could be called here, and from other locations that require it, rather than
  // calling it from Initialize().
  // https://crashpad.chromium.org/bug/9
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  return annotations_simple_map_;
}

const SystemSnapshot* ProcessSnapshotMinidump::System() const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  return &system_snapshot_;
}

std::vector<const ThreadSnapshot*> ProcessSnapshotMinidump::Threads() const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  std::vector<const ThreadSnapshot*> threads;
  for (const auto& thread : threads_) {
    threads.push_back(thread.get());
  }
  return threads;
}

std::vector<const ModuleSnapshot*> ProcessSnapshotMinidump::Modules() const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  std::vector<const ModuleSnapshot*> modules;
  for (const auto& module : modules_) {
    modules.push_back(module.get());
  }
  return modules;
}

std::vector<UnloadedModuleSnapshot> ProcessSnapshotMinidump::UnloadedModules()
    const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  NOTREACHED();  // https://crashpad.chromium.org/bug/10
  return unloaded_modules_;
}

const ExceptionSnapshot* ProcessSnapshotMinidump::Exception() const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  if (exception_snapshot_.IsValid()) {
    return &exception_snapshot_;
  }
  // Allow caller to know whether the minidump contained an exception stream.
  return nullptr;
}

std::vector<const MemoryMapRegionSnapshot*> ProcessSnapshotMinidump::MemoryMap()
    const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  return mem_regions_exposed_;
}

std::vector<HandleSnapshot> ProcessSnapshotMinidump::Handles() const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  NOTREACHED();  // https://crashpad.chromium.org/bug/10
  return std::vector<HandleSnapshot>();
}

std::vector<const MemorySnapshot*> ProcessSnapshotMinidump::ExtraMemory()
    const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  std::vector<const MemorySnapshot*> chunks;
  for (const auto& chunk : extra_memory_) {
    chunks.push_back(chunk.get());
  }
  return chunks;
}

const ProcessMemory* ProcessSnapshotMinidump::Memory() const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  return nullptr;
}

std::vector<const MinidumpStream*>
ProcessSnapshotMinidump::CustomMinidumpStreams() const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);

  std::vector<const MinidumpStream*> result;
  result.reserve(custom_streams_.size());
  for (const auto& custom_stream : custom_streams_) {
    result.push_back(custom_stream.get());
  }
  return result;
}

bool ProcessSnapshotMinidump::InitializeCrashpadInfo() {
  const auto& stream_it = stream_map_.find(kMinidumpStreamTypeCrashpadInfo);
  if (stream_it == stream_map_.end()) {
    return true;
  }

  constexpr size_t crashpad_info_min_size =
      offsetof(decltype(crashpad_info_), reserved);
  size_t remaining_data_size = stream_it->second->DataSize;
  if (remaining_data_size < crashpad_info_min_size) {
    LOG(ERROR) << "crashpad_info size mismatch";
    return false;
  }

  if (!file_reader_->SeekSet(stream_it->second->Rva)) {
    return false;
  }

  if (!file_reader_->ReadExactly(&crashpad_info_, crashpad_info_min_size)) {
    return false;
  }
  remaining_data_size -= crashpad_info_min_size;

  // Read `reserved` if available.
  size_t crashpad_reserved_size = sizeof(crashpad_info_.reserved);
  if (remaining_data_size >= crashpad_reserved_size) {
    if (!file_reader_->ReadExactly(&crashpad_info_.reserved,
                                   crashpad_reserved_size)) {
      return false;
    }
    remaining_data_size -= crashpad_reserved_size;
  } else {
    crashpad_info_.reserved = 0;
  }

  // Read `address_mask` if available.
  size_t crashpad_address_mask_size = sizeof(crashpad_info_.address_mask);
  if (remaining_data_size >= crashpad_address_mask_size) {
    if (!file_reader_->ReadExactly(&crashpad_info_.address_mask,
                                   crashpad_address_mask_size)) {
      return false;
    }
    remaining_data_size -= crashpad_address_mask_size;
  } else {
    crashpad_info_.address_mask = 0;
  }

  if (crashpad_info_.version != MinidumpCrashpadInfo::kVersion) {
    LOG(ERROR) << "crashpad_info version mismatch";
    return false;
  }

  return internal::ReadMinidumpSimpleStringDictionary(
      file_reader_,
      crashpad_info_.simple_annotations,
      &annotations_simple_map_);
}

bool ProcessSnapshotMinidump::InitializeMiscInfo() {
  const auto& stream_it = stream_map_.find(kMinidumpStreamTypeMiscInfo);
  if (stream_it == stream_map_.end()) {
    return true;
  }

  if (!file_reader_->SeekSet(stream_it->second->Rva)) {
    return false;
  }

  const size_t size = stream_it->second->DataSize;
  if (size != sizeof(MINIDUMP_MISC_INFO_5) &&
      size != sizeof(MINIDUMP_MISC_INFO_4) &&
      size != sizeof(MINIDUMP_MISC_INFO_3) &&
      size != sizeof(MINIDUMP_MISC_INFO_2) &&
      size != sizeof(MINIDUMP_MISC_INFO)) {
    LOG(ERROR) << "misc_info size mismatch";
    return false;
  }

  MINIDUMP_MISC_INFO_5 info;
  if (!file_reader_->ReadExactly(&info, size)) {
    return false;
  }

  switch (stream_it->second->DataSize) {
    case sizeof(MINIDUMP_MISC_INFO_5):
    case sizeof(MINIDUMP_MISC_INFO_4):
#if defined(WCHAR_T_IS_UTF16)
      full_version_ = base::WideToUTF8(info.BuildString);
#else
      full_version_ = base::UTF16ToUTF8(info.BuildString);
#endif
      full_version_ = full_version_.substr(0, full_version_.find(';'));
      [[fallthrough]];
    case sizeof(MINIDUMP_MISC_INFO_3):
    case sizeof(MINIDUMP_MISC_INFO_2):
    case sizeof(MINIDUMP_MISC_INFO):
      // TODO(jperaza): Save the remaining misc info.
      // https://crashpad.chromium.org/bug/10
      process_id_ = info.ProcessId;
      create_time_ = info.ProcessCreateTime;
      user_time_ = info.ProcessUserTime;
      kernel_time_ = info.ProcessKernelTime;
  }

  return true;
}

bool ProcessSnapshotMinidump::InitializeModules() {
  const auto& stream_it = stream_map_.find(kMinidumpStreamTypeModuleList);
  if (stream_it == stream_map_.end()) {
    return true;
  }

  std::map<uint32_t, MINIDUMP_LOCATION_DESCRIPTOR> module_crashpad_info_links;
  if (!InitializeModulesCrashpadInfo(&module_crashpad_info_links)) {
    return false;
  }

  if (stream_it->second->DataSize < sizeof(MINIDUMP_MODULE_LIST)) {
    LOG(ERROR) << "module_list size mismatch";
    return false;
  }

  if (!file_reader_->SeekSet(stream_it->second->Rva)) {
    return false;
  }

  uint32_t module_count;
  if (!file_reader_->ReadExactly(&module_count, sizeof(module_count))) {
    return false;
  }

  if (sizeof(MINIDUMP_MODULE_LIST) + module_count * sizeof(MINIDUMP_MODULE) !=
      stream_it->second->DataSize) {
    LOG(ERROR) << "module_list size mismatch";
    return false;
  }

  for (uint32_t module_index = 0; module_index < module_count; ++module_index) {
    const RVA module_rva = stream_it->second->Rva + sizeof(module_count) +
                           module_index * sizeof(MINIDUMP_MODULE);

    const auto& module_crashpad_info_it =
        module_crashpad_info_links.find(module_index);
    const MINIDUMP_LOCATION_DESCRIPTOR* module_crashpad_info_location =
        module_crashpad_info_it != module_crashpad_info_links.end()
            ? &module_crashpad_info_it->second
            : nullptr;

    auto module = std::make_unique<internal::ModuleSnapshotMinidump>();
    if (!module->Initialize(
            file_reader_, module_rva, module_crashpad_info_location)) {
      return false;
    }

    modules_.push_back(std::move(module));
  }

  return true;
}

bool ProcessSnapshotMinidump::InitializeModulesCrashpadInfo(
    std::map<uint32_t, MINIDUMP_LOCATION_DESCRIPTOR>*
        module_crashpad_info_links) {
  module_crashpad_info_links->clear();

  if (crashpad_info_.version != MinidumpCrashpadInfo::kVersion) {
    return false;
  }

  if (crashpad_info_.module_list.Rva == 0) {
    return true;
  }

  if (crashpad_info_.module_list.DataSize <
      sizeof(MinidumpModuleCrashpadInfoList)) {
    LOG(ERROR) << "module_crashpad_info_list size mismatch";
    return false;
  }

  if (!file_reader_->SeekSet(crashpad_info_.module_list.Rva)) {
    return false;
  }

  uint32_t crashpad_module_count;
  if (!file_reader_->ReadExactly(&crashpad_module_count,
                                 sizeof(crashpad_module_count))) {
    return false;
  }

  if (crashpad_info_.module_list.DataSize !=
      sizeof(MinidumpModuleCrashpadInfoList) +
          crashpad_module_count * sizeof(MinidumpModuleCrashpadInfoLink)) {
    LOG(ERROR) << "module_crashpad_info_list size mismatch";
    return false;
  }

  std::unique_ptr<MinidumpModuleCrashpadInfoLink[]> minidump_links(
      new MinidumpModuleCrashpadInfoLink[crashpad_module_count]);
  if (!file_reader_->ReadExactly(
          &minidump_links[0],
          crashpad_module_count * sizeof(MinidumpModuleCrashpadInfoLink))) {
    return false;
  }

  for (uint32_t crashpad_module_index = 0;
       crashpad_module_index < crashpad_module_count;
       ++crashpad_module_index) {
    const MinidumpModuleCrashpadInfoLink& minidump_link =
        minidump_links[crashpad_module_index];
    if (!module_crashpad_info_links
             ->insert(std::make_pair(minidump_link.minidump_module_list_index,
                                     minidump_link.location))
             .second) {
      LOG(WARNING)
          << "duplicate module_crashpad_info_list minidump_module_list_index "
          << minidump_link.minidump_module_list_index;
      return false;
    }
  }

  return true;
}

bool ProcessSnapshotMinidump::InitializeMemoryInfo() {
  const auto& stream_it = stream_map_.find(kMinidumpStreamTypeMemoryInfoList);
  if (stream_it == stream_map_.end()) {
    return true;
  }

  if (stream_it->second->DataSize < sizeof(MINIDUMP_MEMORY_INFO_LIST)) {
    LOG(ERROR) << "memory_info_list size mismatch";
    return false;
  }

  if (!file_reader_->SeekSet(stream_it->second->Rva)) {
    return false;
  }

  MINIDUMP_MEMORY_INFO_LIST list;

  if (!file_reader_->ReadExactly(&list, sizeof(list))) {
    return false;
  }

  if (list.SizeOfHeader != sizeof(list)) {
    return false;
  }

  if (list.SizeOfEntry != sizeof(MINIDUMP_MEMORY_INFO)) {
    return false;
  }

  if (sizeof(MINIDUMP_MEMORY_INFO_LIST) +
          list.NumberOfEntries * list.SizeOfEntry !=
      stream_it->second->DataSize) {
    LOG(ERROR) << "memory_info_list size mismatch";
    return false;
  }

  for (uint32_t i = 0; i < list.NumberOfEntries; i++) {
    MINIDUMP_MEMORY_INFO info;

    if (!file_reader_->ReadExactly(&info, sizeof(info))) {
      return false;
    }

    mem_regions_.emplace_back(
        std::make_unique<internal::MemoryMapRegionSnapshotMinidump>(info));
    mem_regions_exposed_.emplace_back(mem_regions_.back().get());
  }

  return true;
}

bool ProcessSnapshotMinidump::InitializeExtraMemory() {
  const auto& stream_it = stream_map_.find(kMinidumpStreamTypeMemoryList);
  if (stream_it == stream_map_.end()) {
    return true;
  }

  if (stream_it->second->DataSize < sizeof(MINIDUMP_MEMORY_LIST)) {
    LOG(ERROR) << "memory_list size mismatch";
    return false;
  }

  if (!file_reader_->SeekSet(stream_it->second->Rva)) {
    return false;
  }

  // MSVC won't let us stack-allocate a MINIDUMP_MEMORY_LIST because of its
  // trailing zero-element array. Luckily we're only interested in its other
  // field anyway: a uint32_t indicating the number of memory descriptors that
  // follow.
  static_assert(
      sizeof(MINIDUMP_MEMORY_LIST) == 4,
      "MINIDUMP_MEMORY_LIST's only actual field should be an uint32_t");
  uint32_t num_ranges;
  if (!file_reader_->ReadExactly(&num_ranges, sizeof(num_ranges))) {
    return false;
  }

  // We have to manually keep track of the locations of the entries in the
  // contiguous list of MINIDUMP_MEMORY_DESCRIPTORs, because the Initialize()
  // function jumps around the file to find the contents of each snapshot.
  FileOffset location = file_reader_->SeekGet();
  for (uint32_t i = 0; i < num_ranges; i++) {
    extra_memory_.emplace_back(
        std::make_unique<internal::MemorySnapshotMinidump>());
    if (!extra_memory_.back()->Initialize(file_reader_,
                                          static_cast<RVA>(location))) {
      return false;
    }
    location += sizeof(MINIDUMP_MEMORY_DESCRIPTOR);
  }

  return true;
}

bool ProcessSnapshotMinidump::InitializeThreads() {
  const auto& stream_it = stream_map_.find(kMinidumpStreamTypeThreadList);
  if (stream_it == stream_map_.end()) {
    return true;
  }

  if (stream_it->second->DataSize < sizeof(MINIDUMP_THREAD_LIST)) {
    LOG(ERROR) << "thread_list size mismatch";
    return false;
  }

  if (!file_reader_->SeekSet(stream_it->second->Rva)) {
    return false;
  }

  uint32_t thread_count;
  if (!file_reader_->ReadExactly(&thread_count, sizeof(thread_count))) {
    return false;
  }

  if (sizeof(MINIDUMP_THREAD_LIST) + thread_count * sizeof(MINIDUMP_THREAD) !=
      stream_it->second->DataSize) {
    LOG(ERROR) << "thread_list size mismatch";
    return false;
  }

  if (!InitializeThreadNames()) {
    return false;
  }

  for (uint32_t thread_index = 0; thread_index < thread_count; ++thread_index) {
    const RVA thread_rva = stream_it->second->Rva + sizeof(thread_count) +
                           thread_index * sizeof(MINIDUMP_THREAD);

    auto thread = std::make_unique<internal::ThreadSnapshotMinidump>();
    if (!thread->Initialize(file_reader_, thread_rva, arch_, thread_names_)) {
      return false;
    }

    threads_.push_back(std::move(thread));
  }

  return true;
}

bool ProcessSnapshotMinidump::InitializeThreadNames() {
  const auto& stream_it = stream_map_.find(kMinidumpStreamTypeThreadNameList);
  if (stream_it == stream_map_.end()) {
    return true;
  }

  if (stream_it->second->DataSize < sizeof(MINIDUMP_THREAD_NAME_LIST)) {
    LOG(ERROR) << "thread_name_list size mismatch";
    return false;
  }

  if (!file_reader_->SeekSet(stream_it->second->Rva)) {
    return false;
  }

  uint32_t thread_name_count;
  if (!file_reader_->ReadExactly(&thread_name_count,
                                 sizeof(thread_name_count))) {
    return false;
  }

  if (sizeof(MINIDUMP_THREAD_NAME_LIST) +
          thread_name_count * sizeof(MINIDUMP_THREAD_NAME) !=
      stream_it->second->DataSize) {
    LOG(ERROR) << "thread_name_list size mismatch";
    return false;
  }

  for (uint32_t thread_name_index = 0; thread_name_index < thread_name_count;
       ++thread_name_index) {
    const RVA thread_name_rva =
        stream_it->second->Rva + sizeof(thread_name_count) +
        thread_name_index * sizeof(MINIDUMP_THREAD_NAME);
    if (!file_reader_->SeekSet(thread_name_rva)) {
      return false;
    }
    MINIDUMP_THREAD_NAME minidump_thread_name;
    if (!file_reader_->ReadExactly(&minidump_thread_name,
                                   sizeof(minidump_thread_name))) {
      return false;
    }
    std::string name;
    if (!internal::ReadMinidumpUTF16String(
            file_reader_, minidump_thread_name.RvaOfThreadName, &name)) {
      return false;
    }

    // See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=36566
    const uint32_t thread_id = minidump_thread_name.ThreadId;
    thread_names_.emplace(thread_id, std::move(name));
  }

  return true;
}

bool ProcessSnapshotMinidump::InitializeSystemSnapshot() {
  const auto& stream_it = stream_map_.find(kMinidumpStreamTypeSystemInfo);
  if (stream_it == stream_map_.end()) {
    return true;
  }

  if (stream_it->second->DataSize < sizeof(MINIDUMP_SYSTEM_INFO)) {
    LOG(ERROR) << "system info size mismatch";
    return false;
  }

  if (!system_snapshot_.Initialize(
          file_reader_, stream_it->second->Rva, full_version_)) {
    return false;
  }

  arch_ = system_snapshot_.GetCPUArchitecture();
  return true;
}

bool ProcessSnapshotMinidump::InitializeCustomMinidumpStreams() {
  for (size_t i = 0; i < stream_directory_.size(); i++) {
    const auto& stream = stream_directory_[i];

    // Filter out reserved minidump and crashpad streams.
    const uint32_t stream_type = stream.StreamType;
    if (stream_type <=
            MinidumpStreamType::kMinidumpStreamTypeLastReservedStream ||
        (stream_type >= MinidumpStreamType::kMinidumpStreamTypeCrashpadInfo &&
         stream_type <= MinidumpStreamType::
                            kMinidumpStreamTypeCrashpadLastReservedStream)) {
      continue;
    }

    std::vector<uint8_t> data(stream.Location.DataSize);
    if (!file_reader_->SeekSet(stream.Location.Rva) ||
        !file_reader_->ReadExactly(data.data(), data.size())) {
      LOG(ERROR) << "Failed to read stream with ID 0x" << std::hex
                 << stream_type << std::dec << " at index " << i;
      return false;
    }

    custom_streams_.push_back(
        std::make_unique<MinidumpStream>(stream_type, std::move(data)));
  }

  return true;
}

bool ProcessSnapshotMinidump::InitializeExceptionSnapshot() {
  const auto& stream_it = stream_map_.find(kMinidumpStreamTypeException);
  if (stream_it == stream_map_.end()) {
    return true;
  }

  if (stream_it->second->DataSize < sizeof(MINIDUMP_EXCEPTION_STREAM)) {
    LOG(ERROR) << "system info size mismatch";
    return false;
  }

  if (!exception_snapshot_.Initialize(
          file_reader_, arch_, stream_it->second->Rva)) {
    return false;
  }

  return true;
}

}  // namespace crashpad
