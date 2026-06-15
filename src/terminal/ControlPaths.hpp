#ifndef __ET_CONTROL_PATHS_HPP__
#define __ET_CONTROL_PATHS_HPP__

#include <dirent.h>

#include "Headers.hpp"
#include "SessionCredentials.hpp"

/*
 * Local, per-user discovery for control sessions.  Backgrounded `et --ctl`
 * sessions each own a socket at ~/.et/sessions/<name>.sock; `etctl sessions`
 * enumerates that directory.  The directory is created 0700 and the sockets are
 * 0600, so another local user can neither see nor open them.
 *
 * That is the same directory `et --attach` keeps its session credentials in,
 * deliberately: a session's socket and its credentials are two artifacts of one
 * named thing, so they share a directory and an override rather than each
 * inventing their own.
 */
namespace et {
namespace control_paths {

// Create a directory 0700 if missing; fatal on any other error.
inline void ensureDir0700(const string& dir) {
  if (::mkdir(dir.c_str(), 0700) == -1 && errno != EEXIST) {
    throw std::runtime_error("could not create " + dir + ": " +
                             strerror(errno));
  }
}

// Create a directory (and any missing parents) at 0700; fatal on real errors.
inline void mkdirp0700(const string& dir) {
  if (dir.empty()) {
    return;
  }
  for (size_t p = 1; p <= dir.size(); ++p) {
    if (p == dir.size() || dir[p] == '/') {
      const string sub = dir.substr(0, p);
      if (!sub.empty() && ::mkdir(sub.c_str(), 0700) == -1 && errno != EEXIST) {
        throw std::runtime_error("could not create " + sub + ": " +
                                 strerror(errno));
      }
    }
  }
}

// The directory holding a session's socket, alongside its credentials.
inline string controlDir() { return session_creds::sessionDir(); }

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
  DIR* d = ::opendir(controlDir().c_str());
  if (!d) {
    return names;  // no directory yet => no sessions
  }
  const string suffix = ".sock";
  struct dirent* ent;
  while ((ent = ::readdir(d)) != NULL) {
    string n(ent->d_name);
    if (n.size() > suffix.size() &&
        n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) {
      names.push_back(n.substr(0, n.size() - suffix.size()));
    }
  }
  ::closedir(d);
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace control_paths
}  // namespace et

#endif  // __ET_CONTROL_PATHS_HPP__
