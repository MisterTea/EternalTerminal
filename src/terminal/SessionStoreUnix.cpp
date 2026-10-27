#include "SessionStorePlatform.hpp"

namespace et {
namespace session_storage {
namespace {
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

int openSessionFile(const fs::path& path) {
  int flags = O_RDONLY | O_NONBLOCK;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
  flags |= O_NOFOLLOW;
  return ::open(path.c_str(), flags);
}

void syncDirectory(const fs::path& dir) {
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
}
}  // namespace

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

optional<string> readSessionFile(const fs::path& path, const string& name,
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

void writeSessionFile(const fs::path& tmpPath, const fs::path& finalPath,
                      const string& contents, bool replaceExisting) {
  // Replacing a hard-linked file would leave the old passkey readable via the
  // other link.
  struct stat finalStat;
  if (lstatPath(finalPath, &finalStat) && S_ISREG(finalStat.st_mode) &&
      finalStat.st_nlink != 1) {
    throw std::runtime_error(
        "Could not replace session file with unsafe hard links");
  }

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
  } else {
    std::error_code ec;
    fs::rename(tmpPath, finalPath, ec);
    if (ec) {
      fs::remove(tmpPath);
      throw std::runtime_error("Could not move session file into place: " +
                               ec.message());
    }
  }
  syncDirectory(finalPath.parent_path());
}

bool touchSessionFile(const fs::path& path) {
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
}

void deleteSessionFile(const fs::path& path) {
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
}
}  // namespace session_storage
}  // namespace et
