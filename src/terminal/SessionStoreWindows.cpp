#include "SessionStorePlatform.hpp"

namespace et {
namespace session_storage {
void verifySessionDirectories(const fs::path& /*sessionsPath*/,
                              bool /*allowMissing*/) {}

void ensureDir(const fs::path& path) {
  std::error_code ec;
  fs::create_directories(path, ec);
  if (ec) {
    throw std::runtime_error("Could not create directory " + path.string() +
                             ": " + ec.message());
  }
}

optional<string> readSessionFile(const fs::path& path, const string& /*name*/,
                                 int64_t* lastSeenAt) {
  if (!fs::is_regular_file(path)) {
    return std::nullopt;
  }
  std::ifstream in(path);
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream contents;
  contents << in.rdbuf();

  std::error_code ec;
  const auto mtime = fs::last_write_time(path, ec);
  if (ec) {
    return std::nullopt;
  }
  *lastSeenAt = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::clock_cast<std::chrono::system_clock>(mtime)
                        .time_since_epoch())
                    .count();
  return contents.str();
}

void writeSessionFile(const fs::path& tmpPath, const fs::path& finalPath,
                      const string& contents, bool replaceExisting) {
  {
    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
    if (!out) {
      throw std::runtime_error("Could not create temp session file");
    }
    out << contents;
    out.flush();
    if (!out) {
      throw std::runtime_error("Could not write session file");
    }
  }
  if (!replaceExisting) {
    // Without MOVEFILE_REPLACE_EXISTING the move fails if finalPath exists.
    if (!::MoveFileExW(tmpPath.c_str(), finalPath.c_str(),
                       MOVEFILE_WRITE_THROUGH)) {
      const DWORD error = ::GetLastError();
      fs::remove(tmpPath);
      throw std::runtime_error("Could not create session file: Windows error " +
                               std::to_string(error));
    }
    return;
  }
  std::error_code ec;
  fs::rename(tmpPath, finalPath, ec);
  if (ec) {
    fs::remove(tmpPath);
    throw std::runtime_error("Could not move session file into place: " +
                             ec.message());
  }
}

bool touchSessionFile(const fs::path& path) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec) || ec) {
    return false;
  }
  fs::last_write_time(path, fs::file_time_type::clock::now(), ec);
  return !ec;
}

void deleteSessionFile(const fs::path& path) {
  std::error_code ec;
  const auto status = fs::symlink_status(path, ec);
  if (ec == std::errc::no_such_file_or_directory ||
      (!ec && !fs::exists(status))) {
    return;
  }
  if (ec || !fs::is_regular_file(status)) {
    throw std::runtime_error("Could not inspect session file for deletion");
  }
  fs::remove(path, ec);
  if (ec) {
    throw std::runtime_error("Could not delete session file: " + ec.message());
  }
}
}  // namespace session_storage
}  // namespace et
