#include "SessionStore.hpp"

#include <cstdlib>
#include <filesystem>
#include <regex>

#include "Headers.hpp"
#include "PlatformUtils.hpp"
#include "SessionStorePlatform.hpp"

namespace et {
namespace {
namespace fs = std::filesystem;
using namespace session_storage;

string homeDir() {
  const char* envHome = getenv("HOME");
  if (envHome != nullptr && envHome[0] != '\0') {
    return string(envHome);
  }
  if (const optional<string> accountHome = GetAccountHomeDirectory()) {
    return *accountHome;
  }
  throw std::runtime_error("Could not determine user home directory");
}

bool isPrintableNoBreaks(const string& value) {
  return !value.empty() && value.find_first_of("\r\n") == string::npos;
}
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

  const string contents =
      string("version=1\nname=") + info.name + string("\nhost=") + info.host +
      string("\nport=") + std::to_string(info.port) + string("\nid=") +
      info.id + string("\npasskey=") + info.passkey + string("\nsavedat=") +
      std::to_string(info.savedAt) + string("\ntitle=") + info.title + "\n";
  writeSessionFile(tmpPath, finalPath, contents, replaceExisting);
}

optional<SessionInfo> loadSession(const string& name) {
  if (!isValidSessionName(name)) {
    return std::nullopt;
  }
  try {
    const fs::path path = sessionDirPath() + "/" + name;
    verifySessionDirectories(path.parent_path(), true);
    int64_t lastSeenAt = 0;
    const optional<string> contents = readSessionFile(path, name, &lastSeenAt);
    if (!contents) {
      return std::nullopt;
    }
    std::istringstream in(*contents);

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
    info.lastSeenAt = lastSeenAt;
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
  try {
    verifySessionDirectories(path.parent_path(), true);
  } catch (...) {
    return false;
  }
  return touchSessionFile(path);
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
  try {
    verifySessionDirectories(dir, true);
  } catch (const std::exception& e) {
    LOG(WARNING) << "Could not list sessions: " << e.what();
    return sessions;
  }
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
  verifySessionDirectories(path.parent_path(), true);
  deleteSessionFile(path);
}
}  // namespace et
