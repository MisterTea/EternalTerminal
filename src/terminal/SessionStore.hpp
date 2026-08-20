#ifndef __ET_SESSION_STORE__
#define __ET_SESSION_STORE__

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Headers.hpp"

namespace et {
/**
 * @brief A persisted et session that can be reattached to after the client
 * process (or machine) restarts.
 */
struct SessionInfo {
  /** @brief User-facing name; must satisfy isValidSessionName. */
  string name;
  /** @brief Hostname or IP the ET server listens on. */
  string host;
  /** @brief ET server port. */
  int port;
  /** @brief Session id assigned during setup. */
  string id;
  /** @brief Session passkey. Secret material. */
  string passkey;
  /** @brief Unix time when the session was saved. */
  int64_t savedAt;
};

/**
 * @brief Checks that a name is safe to use as a session file name:
 * 1-63 characters of [A-Za-z0-9._-] starting with an alphanumeric.
 */
bool isValidSessionName(const string& name);

/**
 * @brief Returns the session directory (<home>/.et/sessions) without
 * creating it.
 * @throws std::runtime_error if the user's home directory is unknown.
 */
string sessionDirPath();

/**
 * @brief Persists a session atomically. The session directory and file are
 * created with owner-only permissions because the file contains a passkey.
 * @throws std::runtime_error if the name is invalid or writing fails.
 */
void saveSession(const SessionInfo& info);

/**
 * @brief Lists all valid saved sessions, sorted by name. Corrupt entries are
 * skipped with a warning.
 */
vector<SessionInfo> listSessions();

/**
 * @brief Loads one saved session, or nullopt if it is missing or corrupt.
 */
optional<SessionInfo> loadSession(const string& name);

/**
 * @brief Removes a saved session file if present.
 */
void deleteSession(const string& name);

}  // namespace et

#endif  // __ET_SESSION_STORE__
