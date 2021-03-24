
#include "minidump/minidump_stacktrace_writer.h"

#include <stddef.h>

#include <limits>
#include <utility>

#include "base/logging.h"
#include "snapshot/exception_snapshot.h"
#include "snapshot/thread_snapshot.h"
#include "util/file/file_writer.h"

namespace crashpad {

size_t align_to_8(size_t size) {
  size_t rest = size % 8;
  if (rest == 0) {
    return 0;
  } else {
    return 8 - rest;
  }
}

MinidumpStacktraceListWriter::MinidumpStacktraceListWriter()
    : MinidumpStreamWriter(),
      threads_(),
      frames_(),
      symbol_bytes_(),
      stacktrace_header_() {}

MinidumpStacktraceListWriter::~MinidumpStacktraceListWriter() {}

void MinidumpStacktraceListWriter::InitializeFromSnapshot(
    const std::vector<const ThreadSnapshot*>& thread_snapshots,
    const MinidumpThreadIDMap& thread_id_map,
    const ExceptionSnapshot* exception_snapshot) {
  DCHECK_EQ(state(), kStateMutable);

  DCHECK(threads_.empty());
  DCHECK(frames_.empty());
  DCHECK(symbol_bytes_.empty());

  for (auto thread_snapshot : thread_snapshots) {
    internal::RawThread thread;

    auto thread_id_it = thread_id_map.find(thread_snapshot->ThreadID());
    DCHECK(thread_id_it != thread_id_map.end());
    thread.thread_id = thread_id_it->second;
    thread.start_frame = (uint32_t)frames_.size();

    std::vector<FrameSnapshot> frames = thread_snapshot->StackTrace();

    // filter out the stack frames that are *above* the exception addr, as those
    // are related to exception handling, and not really useful.
    if (exception_snapshot &&
        thread_snapshot->ThreadID() == exception_snapshot->ThreadID()) {
      auto it = begin(frames);
      for (; it != end(frames); it++)
        if (it->InstructionAddr() == exception_snapshot->ExceptionAddress()) {
          break;
        }
      if (it < end(frames)) {
        frames.erase(begin(frames), it);
      }
    }

    for (auto frame_snapshot : frames) {
      internal::RawFrame frame;
      frame.instruction_addr = frame_snapshot.InstructionAddr();
      frame.symbol_offset = (uint32_t)symbol_bytes_.size();

      auto symbol = frame_snapshot.Symbol();

      symbol_bytes_.reserve(symbol.size());
      symbol_bytes_.insert(symbol_bytes_.end(), symbol.begin(), symbol.end());

      frame.symbol_len = (uint32_t)symbol.size();

      frames_.push_back(frame);
    }

    thread.num_frames = (uint32_t)frames_.size() - thread.start_frame;

    threads_.push_back(thread);
  }

  stacktrace_header_.version = 1;
  stacktrace_header_.num_threads = (uint32_t)threads_.size();
  stacktrace_header_.num_frames = (uint32_t)frames_.size();
  stacktrace_header_.symbol_bytes = (uint32_t)symbol_bytes_.size();
}

size_t MinidumpStacktraceListWriter::SizeOfObject() {
  DCHECK_GE(state(), kStateFrozen);

  size_t header_size = sizeof(stacktrace_header_);
  header_size += align_to_8(header_size);
  size_t threads_size = threads_.size() * sizeof(internal::RawThread);
  threads_size += align_to_8(threads_size);
  size_t frames_size = frames_.size() * sizeof(internal::RawFrame);
  frames_size += align_to_8(frames_size);

  return header_size + threads_size + frames_size + symbol_bytes_.size();
}

size_t MinidumpStacktraceListWriter::Alignment() {
  // because we are writing `uint64_t` that are 8-byte aligned
  return 8;
}

bool MinidumpStacktraceListWriter::WriteObject(
    FileWriterInterface* file_writer) {
  DCHECK_EQ(state(), kStateWritable);

  uint64_t padding = 0;
  WritableIoVec iov;
  // header, threads, frames, symbol_bytes
  std::vector<WritableIoVec> iovecs(4);

  iov.iov_base = &stacktrace_header_;
  iov.iov_len = sizeof(stacktrace_header_);
  iovecs.push_back(iov);

  // align the length of iov to a multiple of 8 and write zeros as padding
  iov.iov_base = &padding;
  iov.iov_len = align_to_8(iov.iov_len);
  if (iov.iov_len > 0) {
    iovecs.push_back(iov);
  }

  if (!threads_.empty()) {
    iov.iov_base = &threads_.front();
    iov.iov_len = threads_.size() * sizeof(internal::RawThread);
    iovecs.push_back(iov);

    iov.iov_base = &padding;
    iov.iov_len = align_to_8(iov.iov_len);
    if (iov.iov_len > 0) {
      iovecs.push_back(iov);
    }
  }

  if (!frames_.empty()) {
    iov.iov_base = &frames_.front();
    iov.iov_len = frames_.size() * sizeof(internal::RawFrame);
    iovecs.push_back(iov);

    iov.iov_base = &padding;
    iov.iov_len = align_to_8(iov.iov_len);
    if (iov.iov_len > 0) {
      iovecs.push_back(iov);
    }
  }

  if (!symbol_bytes_.empty()) {
    iov.iov_base = &symbol_bytes_.front();
    iov.iov_len = symbol_bytes_.size();
    iovecs.push_back(iov);
  }

  return file_writer->WriteIoVec(&iovecs);
}

MinidumpStreamType MinidumpStacktraceListWriter::StreamType() const {
  return kMinidumpStreamTypeSentryStackTraces;
}

}  // namespace crashpad
