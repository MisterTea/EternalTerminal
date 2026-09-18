#ifndef __ET_SESSION_CREDENTIALS_HPP__
#define __ET_SESSION_CREDENTIALS_HPP__

#include "Headers.hpp"

/*
 * Where a named session's credentials live, so a later `et` can adopt the
 * session instead of stranding it.
 *
 * A session is identified on the server by an id and a passkey, minted during
 * the SSH bootstrap. The server keeps a session alive when its client goes away
 * (it cannot tell a crashed client from one about to reconnect) and recognizes
 * a returning client by those two values alone, so whoever holds them can take
 * the session over. Ordinarily they only live in the client's memory, which is
 * why losing the process orphans the remote shell: the work keeps running with
 * nothing able to reach it.
 *
 * Caching them under the session's name closes that gap. They are the session
 * credential, so the directory is 0700 and the file 0600: anyone who can read
 * the file can drive the remote shell.
 */
namespace et {
namespace session_creds {

// Session-credential directory, overridable with ET_SESSION_DIR (absolute
// paths only); defaults to ~/.et/sessions.
inline string sessionDir() {
  const char* env = getenv("ET_SESSION_DIR");
  if (env && env[0] == '/') {
    return string(env);
  }
  const char* home = getenv("HOME");
  if (!home || home[0] != '/') {
    throw std::runtime_error("HOME is unset or not an absolute path");
  }
  return string(home) + "/.et/sessions";
}

/*
 * Session names become path components, so they are restricted rather than
 * escaped: letters, digits and the handful of separators a name plausibly
 * wants. That rules out "/" and ".." outright, so a name can never point the
 * credential file outside its directory.
 */
inline bool isValidSessionName(const string& name) {
  if (name.empty() || name.size() > 128) {
    return false;
  }
  for (const char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    if (!ok) {
      return false;
    }
  }
  // Rejecting a leading dot also disposes of "." and "..".
  return name[0] != '.';
}

inline string credsPathForName(const string& name) {
  if (!isValidSessionName(name)) {
    throw std::runtime_error(
        "invalid session name '" + name +
        "' (letters, digits, '-', '_' and '.' only, not starting with '.')");
  }
  return sessionDir() + "/" + name + ".creds";
}

// Create the credential directory (and any missing parents) at 0700.
inline void ensureSessionDir() {
  const string dir = sessionDir();
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

/*
 * Read the cached credentials for `name`. Returns false when there is nothing
 * usable, which is not an error: it just means there is no session to adopt yet
 * and the caller should bootstrap one.
 */
inline bool load(const string& name, string* id, string* passkey) {
  try {
    std::ifstream in(credsPathForName(name));
    return bool(in >> *id >> *passkey) && !id->empty() && !passkey->empty();
  } catch (const std::runtime_error& err) {
    LOG(WARNING) << "Could not read session credentials: " << err.what();
    return false;
  }
}

/*
 * Cache the credentials for `name`, replacing anything already there. Failing
 * to save is not fatal: the session works, it just cannot be adopted later.
 */
inline void save(const string& name, const string& id, const string& passkey) {
  try {
    ensureSessionDir();
    const string path = credsPathForName(name);
    const int fd =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) {
      LOG(WARNING) << "Could not cache session credentials at " << path << ": "
                   << strerror(errno);
      return;
    }
    const string line = id + " " + passkey + "\n";
    if (::write(fd, line.data(), line.size()) != (ssize_t)line.size()) {
      LOG(WARNING) << "Short write caching session credentials";
    }
    ::close(fd);
  } catch (const std::runtime_error& err) {
    LOG(WARNING) << "Could not cache session credentials: " << err.what();
  }
}

/*
 * Forget a session's credentials. Call this once the session is known to be
 * gone rather than merely unreachable, so a later `--attach` bootstraps a new
 * session instead of presenting a key the server has already discarded.
 */
inline void forget(const string& name) {
  try {
    ::unlink(credsPathForName(name).c_str());
  } catch (const std::runtime_error& err) {
    LOG(WARNING) << "Could not drop session credentials: " << err.what();
  }
}

/*
 * Where a session records why it ended.
 *
 * A control session's socket is unlinked when its daemon exits, so the next
 * command gets ENOENT and cannot tell "this session ended, here is why" from
 * "you never opened it" or "something removed it". The daemon knows the reason
 * at the moment it exits; the tombstone is where it leaves that reason behind.
 */
inline string tombstonePathForName(const string& name) {
  return sessionDir() + "/" + name + ".gone";
}

// Record why a session ended. Best effort: a session that cannot leave a note
// is no worse off than one that never wrote one.
inline void writeTombstone(const string& name, const string& reason) {
  try {
    ensureSessionDir();
    std::ofstream out(tombstonePathForName(name), std::ios::trunc);
    if (!out) {
      return;
    }
    out << (long long)::time(nullptr) << " " << reason << "\n";
  } catch (const std::runtime_error& err) {
    LOG(WARNING) << "Could not write session tombstone: " << err.what();
  }
}

// Read back a tombstone as "<reason> (N seconds ago)", or "" if there is none.
inline string readTombstone(const string& name) {
  try {
    std::ifstream in(tombstonePathForName(name));
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
  } catch (const std::runtime_error&) {
    return "";
  }
}

// Clear any previous tombstone; a session that is starting has not ended.
inline void clearTombstone(const string& name) {
  try {
    ::unlink(tombstonePathForName(name).c_str());
  } catch (const std::runtime_error&) {
  }
}

}  // namespace session_creds
}  // namespace et

#endif  // __ET_SESSION_CREDENTIALS_HPP__
