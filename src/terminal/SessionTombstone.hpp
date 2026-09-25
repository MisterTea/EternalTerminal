#ifndef __ET_SESSION_TOMBSTONE_HPP__
#define __ET_SESSION_TOMBSTONE_HPP__

#include "Headers.hpp"
#include "SessionStore.hpp"

/*
 * Where a session records why it ended.
 *
 * A control session's socket is unlinked when its daemon exits, so the next
 * command gets ENOENT and cannot tell "this session ended, here is why" from
 * "you never opened it" or "something removed it". The daemon knows the reason
 * at the moment it exits; the tombstone is where it leaves that reason behind.
 *
 * It lives beside the session's saved record, under the name it ended as, and
 * is suffixed so a session listing never mistakes a note for a session.
 */
namespace et {
namespace session_tombstone {

inline string pathForName(const string& name) {
  if (!isValidSessionName(name)) {
    throw std::runtime_error("invalid session name '" + name + "'");
  }
  return sessionDirPath() + "/" + name + ".gone";
}

// Record why a session ended. Best effort: a session that cannot leave a note
// is no worse off than one that never wrote one.
inline void write(const string& name, const string& reason) {
  try {
    std::ofstream out(pathForName(name), std::ios::trunc);
    if (!out) {
      return;
    }
    out << (long long)::time(nullptr) << " " << reason << "\n";
  } catch (const std::exception& err) {
    LOG(WARNING) << "Could not write session tombstone: " << err.what();
  }
}

// Read back a tombstone as "<reason> (N seconds ago)", or "" if there is none.
inline string read(const string& name) {
  try {
    std::ifstream in(pathForName(name));
    long long when = 0;
    if (!(in >> when)) {
      return "";
    }
    string reason;
    std::getline(in, reason);
    while (!reason.empty() && reason.front() == ' ') {
      reason.erase(reason.begin());
    }
    if (reason.empty()) {
      return "";
    }
    const long long age = (long long)::time(nullptr) - when;
    if (age < 0) {
      return reason;
    }
    return reason + " (" + std::to_string(age) + "s ago)";
  } catch (const std::exception&) {
    return "";
  }
}

// Clear any previous tombstone; a session that is starting has not ended.
inline void clear(const string& name) {
  try {
    std::error_code ec;
    fs::remove(pathForName(name), ec);
  } catch (const std::exception&) {
  }
}

}  // namespace session_tombstone
}  // namespace et

#endif  // __ET_SESSION_TOMBSTONE_HPP__
