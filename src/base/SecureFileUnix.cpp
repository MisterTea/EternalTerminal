#include "SecureFileUnix.hpp"

namespace et::private_storage {
namespace {
class FileDescriptor {
 public:
  explicit FileDescriptor(int value) : fd(value) {}
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  FileDescriptor(FileDescriptor&& other) noexcept : fd(other.fd) {
    other.fd = -1;
  }
  FileDescriptor& operator=(FileDescriptor&& other) noexcept {
    if (fd >= 0) ::close(fd);
    fd = other.fd;
    other.fd = -1;
    return *this;
  }
  ~FileDescriptor() {
    if (fd >= 0) ::close(fd);
  }
  int get() const { return fd; }
  void closeChecked() {
    const int value = fd;
    fd = -1;
    if (::close(value) != 0) throw std::runtime_error("Could not close file");
  }

 private:
  int fd;
};

[[noreturn]] void fail(const fs::path& path, const string& reason) {
  throw std::runtime_error("Unsafe storage path '" + path.string() +
                           "': " + reason);
}

struct stat inspect(int fd, const fs::path& path) {
  struct stat st;
  if (::fstat(fd, &st) != 0) fail(path, strerror(errno));
  return st;
}

// A group-writable entry is still private when its group is the owner's user
// private group, as with umask 002: named after the owner, the owner's
// primary group, and listing no other member. This is Debian's OpenSSH rule
// plus the name check, so a shared primary group such as SUSE's "users" does
// not count.
bool isUserPrivateGroup(const struct stat& st) {
  long size = sysconf(_SC_GETPW_R_SIZE_MAX);
  vector<char> pwBuffer(size > 0 ? size : 16 * 1024);
  passwd pwd;
  passwd* owner = nullptr;
  if (getpwuid_r(st.st_uid, &pwd, pwBuffer.data(), pwBuffer.size(), &owner) !=
          0 ||
      owner == nullptr) {
    return false;
  }
  size = sysconf(_SC_GETGR_R_SIZE_MAX);
  vector<char> grBuffer(size > 0 ? size : 16 * 1024);
  group grp;
  group* entry = nullptr;
  if (getgrgid_r(st.st_gid, &grp, grBuffer.data(), grBuffer.size(), &entry) !=
          0 ||
      entry == nullptr) {
    return false;
  }
  if (strcmp(entry->gr_name, owner->pw_name) != 0 ||
      owner->pw_gid != entry->gr_gid) {
    return false;
  }
  return entry->gr_mem[0] == nullptr ||
         (entry->gr_mem[1] == nullptr &&
          strcmp(entry->gr_mem[0], owner->pw_name) == 0);
}

bool writableByOthers(const struct stat& st) {
  if ((st.st_mode & S_IWOTH) != 0) return true;
  return (st.st_mode & S_IWGRP) != 0 && !isUserPrivateGroup(st);
}

void verifyAncestor(const struct stat& st, const fs::path& path) {
  // A root-owned sticky directory such as /tmp cannot have a user-owned
  // entry replaced by another user. Still verify every entry underneath it.
  const bool stickyRoot = st.st_uid == 0 && (st.st_mode & S_ISVTX) != 0;
  if (!S_ISDIR(st.st_mode) || (st.st_uid != geteuid() && st.st_uid != 0) ||
      (writableByOthers(st) && !stickyRoot)) {
    fail(path, "directory owner or write permissions are unsafe");
  }
}

FileDescriptor openDirectory(const fs::path& input, bool create,
                             bool makePrivate, unsigned symlinkDepth = 0) {
  fs::path path = fs::absolute(input).lexically_normal();
  // Normalize a trailing slash too, so a final symlink is never mistaken
  // for an intermediate system alias.
  if (path != path.root_path() && path.filename().empty()) {
    path = path.parent_path();
  }
  if (makePrivate && path == path.root_path()) {
    fail(path, "cannot use a filesystem root as private storage");
  }
  FileDescriptor current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (current.get() < 0) fail(path, strerror(errno));
  fs::path traversed = "/";
  fs::path resolved = "/";
  verifyAncestor(inspect(current.get(), traversed), traversed);
  for (const auto& component : path.relative_path()) {
    if (component == "." || component.empty()) continue;
    const string name = component.string();
    const struct stat parent = inspect(current.get(), traversed);
    traversed /= component;
    fs::path nextResolved = resolved / component;
    int fd = ::openat(current.get(), name.c_str(),
                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT && create) {
      if (::mkdirat(current.get(), name.c_str(), 0700) != 0 &&
          errno != EEXIST) {
        fail(traversed, strerror(errno));
      }
      fd = ::openat(current.get(), name.c_str(),
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    if (fd < 0) {
      // Unix platforms use trusted root-owned aliases such as /var ->
      // /private/var. Permit these only in non-writable ancestors, never as
      // the final private directory. User-planted links are refused.
      struct stat linkStat;
      if ((!makePrivate || traversed != path) && !writableByOthers(parent) &&
          ::fstatat(current.get(), name.c_str(), &linkStat,
                    AT_SYMLINK_NOFOLLOW) == 0 &&
          S_ISLNK(linkStat.st_mode) && linkStat.st_uid == 0) {
        if (symlinkDepth >= 40) fail(traversed, "too many system symlinks");
        char target[4096];
        const ssize_t length =
            ::readlinkat(current.get(), name.c_str(), target, sizeof(target));
        if (length < 0) fail(traversed, strerror(errno));
        if (static_cast<size_t>(length) == sizeof(target)) {
          fail(traversed, "system symlink target is too long");
        }
        const fs::path destination(string(target, static_cast<size_t>(length)));
        const fs::path targetPath =
            destination.is_absolute() ? destination : resolved / destination;
        // A trusted alias can still point through an unsafe directory. Check
        // its entire target path before any later library follows the alias.
        current = openDirectory(targetPath, false, false, symlinkDepth + 1);
        resolved = fs::canonical(targetPath);
        continue;
      }
    }
    if (fd < 0) fail(traversed, strerror(errno));
    current = FileDescriptor(fd);
    verifyAncestor(inspect(current.get(), traversed), traversed);
    resolved = nextResolved;
  }
  if (makePrivate) {
    const struct stat st = inspect(current.get(), path);
    if (st.st_uid != geteuid() || writableByOthers(st)) {
      fail(path, "private directory owner or write permissions are unsafe");
    }
    if (::fchmod(current.get(), 0700) != 0) fail(path, strerror(errno));
  }
  return current;
}

void verifyFile(int fd, const fs::path& path) {
  const struct stat st = inspect(fd, path);
  if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 ||
      writableByOthers(st)) {
    fail(path, "file type, owner, links or write permissions are unsafe");
  }
  if (::fchmod(fd, 0600) != 0) fail(path, strerror(errno));
}

void writeContents(int fd, const fs::path& path, const string& contents) {
  size_t written = 0;
  while (written < contents.size()) {
    const ssize_t rc =
        ::write(fd, contents.data() + written, contents.size() - written);
    if (rc < 0 && errno == EINTR) continue;
    if (rc <= 0) fail(path, "could not write file");
    written += static_cast<size_t>(rc);
  }
}
}  // namespace

void ensureDirectory(const fs::path& path) { openDirectory(path, true, true); }

optional<string> readFile(const fs::path& path, size_t maxBytes) {
  auto parent = openDirectory(fs::absolute(path).parent_path(), false, false);
  FileDescriptor fd(::openat(parent.get(), path.filename().c_str(),
                             O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  if (fd.get() < 0) {
    if (errno == ENOENT) return nullopt;
    fail(path, strerror(errno));
  }
  verifyFile(fd.get(), path);
  string contents;
  char buffer[4096];
  while (true) {
    const ssize_t rc = ::read(fd.get(), buffer, sizeof(buffer));
    if (rc < 0 && errno == EINTR) continue;
    if (rc < 0) fail(path, strerror(errno));
    if (rc == 0) break;
    if (static_cast<size_t>(rc) > maxBytes - contents.size()) {
      fail(path, "file exceeds size limit");
    }
    contents.append(buffer, static_cast<size_t>(rc));
  }
  return contents;
}

bool createFile(const fs::path& path, const string& contents) {
  auto parent = openDirectory(fs::absolute(path).parent_path(), false, false);
  // Publish complete contents atomically. Concurrent telemetry initializers
  // must never read a partially written identifier.
  const string temporary =
      "." + path.filename().string() + "." + genRandomAlphaNum(32);
  FileDescriptor fd(::openat(
      parent.get(), temporary.c_str(),
      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600));
  if (fd.get() < 0) fail(path, strerror(errno));
  try {
    verifyFile(fd.get(), path);
    writeContents(fd.get(), path, contents);
    fd.closeChecked();
    if (::linkat(parent.get(), temporary.c_str(), parent.get(),
                 path.filename().c_str(), 0) != 0) {
      if (errno != EEXIST) fail(path, strerror(errno));
      ::unlinkat(parent.get(), temporary.c_str(), 0);
      return false;
    }
    if (::unlinkat(parent.get(), temporary.c_str(), 0) != 0) {
      fail(path, strerror(errno));
    }
  } catch (...) {
    ::unlinkat(parent.get(), temporary.c_str(), 0);
    throw;
  }
  return true;
}

void replaceFile(const fs::path& path, const string& contents) {
  auto parent = openDirectory(fs::absolute(path).parent_path(), false, false);
  FileDescriptor fd(
      ::openat(parent.get(), path.filename().c_str(),
               O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600));
  if (fd.get() < 0) fail(path, strerror(errno));
  // Never truncate or write an existing inode before checking it.
  verifyFile(fd.get(), path);
  if (::ftruncate(fd.get(), 0) != 0) fail(path, strerror(errno));
  writeContents(fd.get(), path, contents);
  fd.closeChecked();
}
}  // namespace et::private_storage
