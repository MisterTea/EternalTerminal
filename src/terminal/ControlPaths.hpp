#ifndef __ET_CONTROL_PATHS_HPP__
#define __ET_CONTROL_PATHS_HPP__

#include "Headers.hpp"
#include "SessionStore.hpp"

/*
 * Local, per-user discovery for control sessions.  Backgrounded `et --ctl`
 * sessions each own a socket at ~/.et/control/<name>.sock; `etctl sessions`
 * enumerates that directory.  The directory is created 0700 and the sockets are
 * 0600, so another local user can neither see nor open them.
 *
 * It is a sibling of the saved-session directory rather than the same one.
 * Co-locating them reads well until a name collides: `~/.et/sessions/<name>` is
 * one file per session, and a session may legally be named "foo.sock", which is
 * exactly where session "foo" wants its socket.  Keeping the live control files
 * in their own directory leaves the saved-session namespace entirely to
 * SessionStore, so neither side has to reserve names from the other.
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

// The directory holding live control sockets, beside the saved-session
// directory rather than inside it.
inline string controlDir() {
  return fs::path(sessionDirPath()).parent_path().string() + "/control";
}

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
