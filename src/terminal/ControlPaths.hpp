#ifndef __ET_CONTROL_PATHS_HPP__
#define __ET_CONTROL_PATHS_HPP__

#include "Headers.hpp"
#include "SessionStore.hpp"

/*
 * Local, per-user discovery for control sessions.  Backgrounded `et --ctl`
 * sessions each own a socket at ~/.et/sessions/<name>.sock; `etctl sessions`
 * enumerates that directory.  The directory is created 0700 and the sockets are
 * 0600, so another local user can neither see nor open them.
 *
 * That is the same directory a named session's record lives in, deliberately: a
 * session's socket and its saved record are two artifacts of one named thing,
 * so they share a directory rather than each inventing their own.  The records
 * are bare names and the sockets are suffixed, so neither listing sees the
 * other's files.
 */
namespace et {
namespace control_paths {

// Create a directory (and any missing parents) at 0700; fatal on real errors.
inline void mkdirp0700(const string& dir) {
  if (dir.empty()) {
    return;
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec && !fs::is_directory(dir)) {
    throw std::runtime_error("could not create " + dir + ": " + ec.message());
  }
#ifndef WIN32
  // POSIX permission bits have no Windows equivalent; the directory is only
  // access-restricted on platforms where that concept applies.
  fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
#endif
}

// The directory holding a session's socket, alongside its saved record.
inline string controlDir() { return sessionDirPath(); }

// Resolve (and materialize) the control directory.
inline string ensureControlDir() {
  const string dir = controlDir();
  mkdirp0700(dir);
  return dir;
}

inline string socketPathForName(const string& name) {
  return controlDir() + "/" + name + ".sock";
}

/*
 * Names of sessions whose socket files currently exist (liveness is confirmed
 * separately by connecting).  Returns sorted names without the .sock suffix.
 */
inline vector<string> listSessionNames() {
  vector<string> names;
  std::error_code ec;
  fs::directory_iterator it(controlDir(), ec);
  if (ec) {
    return names;  // no directory yet => no sessions
  }
  const string suffix = ".sock";
  for (const auto& entry : it) {
    const string n = entry.path().filename().string();
    if (n.size() > suffix.size() &&
        n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) {
      names.push_back(n.substr(0, n.size() - suffix.size()));
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace control_paths
}  // namespace et

#endif  // __ET_CONTROL_PATHS_HPP__
