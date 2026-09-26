#ifndef __ET_SESSION_STORE__
#define __ET_SESSION_STORE__

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Headers.hpp"

namespace et {
struct SessionInfo {
  string name;
  string host;
  int port;
  string id;
  string passkey;
  string title;
  int64_t savedAt;
  // Taken from the session file's mtime.
  int64_t lastSeenAt;
};

// 1-63 characters of [A-Za-z0-9._-], starting with an alphanumeric.
bool isValidSessionName(const string& name);

// <home>/.et/sessions; throws if the home directory is unknown.
string sessionDirPath();

// Writes atomically with owner-only permissions; the file holds a passkey.
void saveSession(const SessionInfo& info, bool replaceExisting = true);

// Sorted by name; corrupt entries are skipped.
vector<SessionInfo> listSessions();

optional<SessionInfo> loadSession(const string& name);

bool touchSession(const string& name);

bool updateSessionTitle(const string& name, const string& title);

string formatLastSeen(int64_t lastSeenAt, int64_t now);

// An absent file is a no-op; throws on unsafe storage or deletion failure.
void deleteSession(const string& name);

}  // namespace et

#endif  // __ET_SESSION_STORE__
