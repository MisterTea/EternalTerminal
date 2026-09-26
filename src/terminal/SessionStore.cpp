#include "SessionStore.hpp"

#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>

#include "Headers.hpp"

#ifndef WIN32
#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>
#endif

namespace et {
namespace {
namespace fs = std::filesystem;
string homeDir() {
  const char* envHome = getenv("HOME");
  if (envHome != nullptr && envHome[0] != '\0') {
    return string(envHome);
  }
#ifdef WIN32
  const char* userProfile = getenv("USERPROFILE");
  if (userProfile != nullptr && userProfile[0] != '\0') {
    return string(userProfile);
  }
#else
  struct passwd pwd;
  struct passwd* pwdbuf;
  long pwSize = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (pwSize > 0) {
    char* buf = static_cast<char*>(malloc(pwSize));
    if (buf != nullptr) {
      if (getpwuid_r(getuid(), &pwd, buf, pwSize, &pwdbuf) == 0 &&
          pwdbuf != nullptr && pwdbuf->pw_dir[0] != '\0') {
        string result = string(pwdbuf->pw_dir);
        free(buf);
        return result;
      }
      free(buf);
    }
  }
#endif
  throw std::runtime_error("Could not determine user home directory");
}

bool isPrintableNoBreaks(const string& value) {
  return !value.empty() && value.find_first_of("\r\n") == string::npos;
}

#ifndef WIN32
bool isOwnedDirectory(const struct stat& fileStat) {
  return S_ISDIR(fileStat.st_mode) && fileStat.st_uid == getuid() &&
         (fileStat.st_mode & (S_IRWXG | S_IRWXO)) == 0;
}

bool isOwnedSessionFile(const struct stat& fileStat) {
  return S_ISREG(fileStat.st_mode) && fileStat.st_uid == getuid() &&
         (fileStat.st_mode & (S_IRWXG | S_IRWXO)) == 0 &&
         fileStat.st_nlink == 1;
}

bool lstatPath(const fs::path& path, struct stat* fileStat) {
  return ::lstat(path.c_str(), fileStat) == 0;
}

void verifySessionDirectory(const fs::path& path, bool allowMissing) {
  struct stat fileStat;
  if (!lstatPath(path, &fileStat)) {
    if (allowMissing && errno == ENOENT) {
      return;
    }
    throw std::runtime_error("Could not inspect session directory: " +
                             string(strerror(errno)));
  }
  if (!isOwnedDirectory(fileStat)) {
    throw std::runtime_error(
        "Session directory has unsafe owner or permissions");
  }
}

void verifySessionDirectories(const fs::path& sessionsPath, bool allowMissing) {
  verifySessionDirectory(sessionsPath.parent_path(), allowMissing);
  verifySessionDirectory(sessionsPath, allowMissing);
}

void ensureDir(const fs::path& path) {
  struct stat fileStat;
  if (!lstatPath(path, &fileStat)) {
    const int inspectErrno = errno;
    if (inspectErrno != ENOENT) {
      throw std::runtime_error("Could not create session directory: " +
                               string(strerror(inspectErrno)));
    }
    if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
      throw std::runtime_error("Could not create session directory: " +
                               string(strerror(errno)));
    }
    if (!lstatPath(path, &fileStat)) {
      throw std::runtime_error("Could not inspect session directory: " +
                               string(strerror(errno)));
    }
  }
  if (!S_ISDIR(fileStat.st_mode) || fileStat.st_uid != getuid()) {
    throw std::runtime_error(
        "Session directory has unsafe owner or is not a directory");
  }
  if ((fileStat.st_mode & (S_IRWXG | S_IRWXO)) != 0 &&
      ::chmod(path.c_str(), 0700) != 0) {
    throw std::runtime_error("Could not set session directory permissions: " +
                             string(strerror(errno)));
  }
  verifySessionDirectory(path, false);
}

int openSessionFile(const fs::path& path) {
  int flags = O_RDONLY | O_NONBLOCK;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  return ::open(path.c_str(), flags);
}

optional<string> readSessionContents(const fs::path& path, const string& name,
                                     int64_t* lastSeenAt) {
  struct stat pathStat;
  if (!lstatPath(path, &pathStat)) {
    return std::nullopt;
  }
  if (!S_ISREG(pathStat.st_mode)) {
    LOG(WARNING) << "Skipping unsafe session file '" << name << "'";
    return std::nullopt;
  }
  const int fd = openSessionFile(path);
  if (fd < 0) {
    return std::nullopt;
  }

  struct stat fileStat;
  const bool safeFile =
      ::fstat(fd, &fileStat) == 0 && isOwnedSessionFile(fileStat);
  if (!safeFile) {
    ::close(fd);
    LOG(WARNING) << "Skipping unsafe session file '" << name << "'";
    return std::nullopt;
  }
  *lastSeenAt = static_cast<int64_t>(fileStat.st_mtime);

  string contents;
  char buffer[4096];
  while (true) {
    const ssize_t bytesRead = ::read(fd, buffer, sizeof(buffer));
    if (bytesRead < 0 && errno == EINTR) {
      continue;
    }
    if (bytesRead < 0) {
      ::close(fd);
      return std::nullopt;
    }
    if (bytesRead == 0) {
      break;
    }
    if (contents.size() + static_cast<size_t>(bytesRead) > 64 * 1024) {
      ::close(fd);
      LOG(WARNING) << "Skipping oversized session file '" << name << "'";
      return std::nullopt;
    }
    contents.append(buffer, static_cast<size_t>(bytesRead));
  }
  ::close(fd);
  return contents;
}
#else
void ensureDir(const fs::path& path) {
  std::error_code ec;
  fs::create_directories(path, ec);
  if (ec) {
    throw std::runtime_error("Could not create directory " + path.string() +
                             ": " + ec.message());
  }
}
#endif
}  // namespace

bool isValidSessionName(const string& name) {
  static const std::regex pattern(R"(^[A-Za-z0-9][A-Za-z0-9._-]{0,62}$)");
  return std::regex_match(name, pattern);
}

string sessionDirPath() { return homeDir() + "/.et/sessions"; }

void saveSession(const SessionInfo& info, bool replaceExisting) {
  if (!isValidSessionName(info.name)) {
    throw std::runtime_error("Invalid session name: " + info.name);
  }
  if (!isPrintableNoBreaks(info.host) || !isPrintableNoBreaks(info.id) ||
      !isPrintableNoBreaks(info.passkey) || info.port <= 0 ||
      info.port > 65535 || info.title.find_first_of("\r\n") != string::npos) {
    throw std::runtime_error("Session fields must be non-empty and printable");
  }

  const fs::path dir = sessionDirPath();
  ensureDir(homeDir() + "/.et");
  ensureDir(dir);

  const fs::path tmpPath = dir / ("." + info.name + "." + genRandomAlphaNum(8));
  const fs::path finalPath = dir / info.name;

  // Replacing a hard-linked file would leave the old passkey readable via the
  // other link.
#ifndef WIN32
  struct stat finalStat;
  if (lstatPath(finalPath, &finalStat) && S_ISREG(finalStat.st_mode) &&
      finalStat.st_nlink != 1) {
    throw std::runtime_error(
        "Could not replace session file with unsafe hard links");
  }
#endif

  string contents =
      string("version=1\nname=") + info.name + string("\nhost=") + info.host +
      string("\nport=") + std::to_string(info.port) + string("\nid=") +
      info.id + string("\npasskey=") + info.passkey + string("\nsavedat=") +
      std::to_string(info.savedAt) + string("\ntitle=") + info.title + "\n";
#ifdef WIN32
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
#else
  int tmpFd = ::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (tmpFd < 0) {
    throw std::runtime_error("Could not create temp session file: " +
                             string(strerror(errno)));
  }
  try {
    if (::fchmod(tmpFd, 0600) != 0) {
      throw std::runtime_error("Could not set session file permissions: " +
                               string(strerror(errno)));
    }
    size_t written = 0;
    while (written < contents.size()) {
      const ssize_t rc =
          ::write(tmpFd, contents.data() + written, contents.size() - written);
      if (rc < 0 && errno == EINTR) {
        continue;
      }
      if (rc <= 0) {
        throw std::runtime_error("Could not write session file: " +
                                 string(strerror(errno)));
      }
      written += static_cast<size_t>(rc);
    }
    if (::fsync(tmpFd) != 0) {
      throw std::runtime_error("Could not sync session file: " +
                               string(strerror(errno)));
    }
    if (::close(tmpFd) != 0) {
      tmpFd = -1;
      throw std::runtime_error("Could not close session file: " +
                               string(strerror(errno)));
    }
    tmpFd = -1;
  } catch (...) {
    if (tmpFd >= 0) {
      ::close(tmpFd);
    }
    fs::remove(tmpPath);
    throw;
  }
#endif
#ifndef WIN32
  auto syncSessionDirectory = [&dir]() {
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const int dirFd = ::open(dir.c_str(), flags);
    if (dirFd < 0) {
      throw std::runtime_error("Could not open session directory: " +
                               string(strerror(errno)));
    }
    const int syncResult = ::fsync(dirFd);
    const int syncErrno = errno;
    ::close(dirFd);
    if (syncResult != 0) {
      throw std::runtime_error("Could not sync session directory: " +
                               string(strerror(syncErrno)));
    }
  };
  if (!replaceExisting) {
    // link(2) never replaces an existing entry, unlike rename(2).
    if (::link(tmpPath.c_str(), finalPath.c_str()) != 0) {
      const string error = strerror(errno);
      fs::remove(tmpPath);
      throw std::runtime_error("Could not create session file: " + error);
    }
    if (::unlink(tmpPath.c_str()) != 0) {
      const string error = strerror(errno);
      fs::remove(tmpPath);
      throw std::runtime_error("Could not finalize session file: " + error);
    }
    syncSessionDirectory();
  } else {
    std::error_code ec;
    fs::rename(tmpPath, finalPath, ec);
    if (ec) {
      fs::remove(tmpPath);
      throw std::runtime_error("Could not move session file into place: " +
                               ec.message());
    }
    syncSessionDirectory();
  }
#else
  if (!replaceExisting) {
    fs::remove(tmpPath);
    throw std::runtime_error(
        "Non-replacing session storage is unavailable on Windows");
  }
  std::error_code ec;
  fs::rename(tmpPath, finalPath, ec);
  if (ec) {
    fs::remove(tmpPath);
    throw std::runtime_error("Could not move session file into place: " +
                             ec.message());
  }
#endif
}

optional<SessionInfo> loadSession(const string& name) {
  if (!isValidSessionName(name)) {
    return std::nullopt;
  }
  try {
    const fs::path path = sessionDirPath() + "/" + name;
#ifndef WIN32
    verifySessionDirectories(path.parent_path(), true);
    int64_t lastSeenAt = 0;
    const optional<string> contents =
        readSessionContents(path, name, &lastSeenAt);
    if (!contents) {
      return std::nullopt;
    }
    std::istringstream in(*contents);
#else
    if (!fs::is_regular_file(path)) {
      return std::nullopt;
    }

    std::ifstream in(path);
    if (!in) {
      return std::nullopt;
    }
#endif

    SessionInfo info;
    bool haveVersion = false;
    bool havePort = false;
    bool haveSavedAt = false;
    string line;
    while (std::getline(in, line)) {
      if (line.empty()) {
        continue;
      }
      const auto eq = line.find('=');
      if (eq == string::npos || eq == 0) {
        return std::nullopt;
      }
      const string key = line.substr(0, eq);
      const string value = line.substr(eq + 1);
      if (key == "version") {
        haveVersion = (value == "1");
      } else if (key == "name") {
        info.name = value;
      } else if (key == "host") {
        info.host = value;
      } else if (key == "port") {
        info.port = std::stoi(value);
        havePort = true;
      } else if (key == "id") {
        info.id = value;
      } else if (key == "passkey") {
        info.passkey = value;
      } else if (key == "savedat") {
        info.savedAt = std::stoll(value);
        haveSavedAt = true;
      } else if (key == "title") {
        info.title = value;
      }
    }

    if (!haveVersion || !havePort || !haveSavedAt || info.name != name ||
        info.host.empty() || info.id.empty() || info.passkey.empty() ||
        info.port <= 0 || info.port > 65535) {
      return std::nullopt;
    }
#ifndef WIN32
    info.lastSeenAt = lastSeenAt;
#else
    std::error_code ec;
    const auto mtime = fs::last_write_time(path, ec);
    if (ec) {
      return std::nullopt;
    }
    info.lastSeenAt =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::clock_cast<std::chrono::system_clock>(mtime)
                .time_since_epoch())
            .count();
#endif
    return info;
  } catch (const std::exception& e) {
    LOG(WARNING) << "Could not load session '" << name << "': " << e.what();
    return std::nullopt;
  }
}

bool touchSession(const string& name) {
  if (!isValidSessionName(name)) {
    return false;
  }
  const fs::path path = sessionDirPath() + "/" + name;
#ifndef WIN32
  try {
    verifySessionDirectories(path.parent_path(), true);
  } catch (...) {
    return false;
  }
  const int fd = openSessionFile(path);
  if (fd < 0) {
    return false;
  }
  struct stat fileStat;
  if (::fstat(fd, &fileStat) != 0 || !isOwnedSessionFile(fileStat)) {
    ::close(fd);
    return false;
  }
  const bool touched = ::futimens(fd, nullptr) == 0;
  ::close(fd);
  return touched;
#else
  std::error_code ec;
  if (!fs::is_regular_file(path, ec) || ec) {
    return false;
  }
  fs::last_write_time(path, fs::file_time_type::clock::now(), ec);
  return !ec;
#endif
}

bool updateSessionTitle(const string& name, const string& title) {
  try {
    optional<SessionInfo> info = loadSession(name);
    if (!info) {
      return false;
    }
    info->title = title;
    saveSession(*info);
  } catch (...) {
    return false;
  }
  return true;
}

string formatLastSeen(int64_t lastSeenAt, int64_t now) {
  const int64_t age = std::max<int64_t>(0, now - lastSeenAt);
  if (age <= 30) {
    return "now";
  }
  if (age < 60) {
    return std::to_string(age) + "s ago";
  }
  if (age < 60 * 60) {
    return std::to_string(age / 60) + "m ago";
  }
  if (age < 24 * 60 * 60) {
    return std::to_string(age / (60 * 60)) + "h ago";
  }
  return std::to_string(age / (24 * 60 * 60)) + "d ago";
}

vector<SessionInfo> listSessions() {
  vector<SessionInfo> sessions;
  string dir;
  try {
    dir = sessionDirPath();
  } catch (const std::exception& e) {
    LOG(WARNING) << "Could not list sessions: " << e.what();
    return sessions;
  }
#ifndef WIN32
  try {
    verifySessionDirectories(dir, true);
  } catch (const std::exception& e) {
    LOG(WARNING) << "Could not list sessions: " << e.what();
    return sessions;
  }
#endif
  std::error_code ec;
  const bool isDirectory = fs::is_directory(dir, ec);
  if (ec) {
    LOG(WARNING) << "Could not list sessions: " << ec.message();
    return sessions;
  }
  if (!isDirectory) {
    return sessions;
  }
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    const string name = entry.path().filename().string();
    if (!isValidSessionName(name)) {
      continue;
    }
    optional<SessionInfo> info = loadSession(name);
    if (!info) {
      LOG(WARNING) << "Skipping unreadable or corrupt session file: " << name;
      continue;
    }
    sessions.push_back(*info);
  }

  sort(sessions.begin(), sessions.end(),
       [](const SessionInfo& a, const SessionInfo& b) {
         return a.name < b.name;
       });
  return sessions;
}

void deleteSession(const string& name) {
  if (!isValidSessionName(name)) {
    return;
  }
  const fs::path path = sessionDirPath() + "/" + name;
#ifndef WIN32
  verifySessionDirectories(path.parent_path(), true);
  struct stat fileStat;
  if (!lstatPath(path, &fileStat)) {
    if (errno == ENOENT) {
      return;
    }
    throw std::runtime_error("Could not inspect session file for deletion: " +
                             string(strerror(errno)));
  }
  if (!isOwnedSessionFile(fileStat)) {
    throw std::runtime_error(
        "Refusing to delete a session file with unsafe owner, type, links or "
        "permissions");
  }
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    throw std::runtime_error("Could not delete session file: " +
                             string(strerror(errno)));
  }
#else
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
#endif
}
}  // namespace et
