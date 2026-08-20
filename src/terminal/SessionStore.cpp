#include "SessionStore.hpp"

#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>

#include "Headers.hpp"

#ifndef WIN32
#include <pwd.h>
#include <unistd.h>
#endif

namespace et {
namespace {
constexpr const char kSessionVersion[] = "1";
const std::regex kSessionNameRegex{
    R"regex(^[A-Za-z0-9][A-Za-z0-9._-]{0,62}$)regex"};
namespace fs = std::filesystem;
string homeDir() {
  // Prefer the environment so the location is predictable and testable.
  const char* envHome = getenv("HOME");
  if (envHome != nullptr && envHome[0] != '\0') {
    return string(envHome);
  }
#ifdef WIN32
  const char* userProfile = getenv("USERPROFILE");
  if (userProfile != nullptr && userProfile[0] != '\0') {
    return string(userProfile);
  }
#else
  // Fall back to the passwd entry.
  struct passwd pwd;
  struct passwd* pwdbuf;
  long pwSize = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (pwSize > 0) {
    char* buf = static_cast<char*>(malloc(pwSize));
    if (buf != nullptr) {
      if (getpwuid_r(getuid(), &pwd, buf, pwSize, &pwdbuf) == 0 &&
          pwdbuf != nullptr && pwdbuf->pw_dir[0] != '\0') {
        string result = string(pwdbuf->pw_dir);
        free(buf);
        return result;
      }
      free(buf);
    }
  }
#endif
  throw std::runtime_error("Could not determine user home directory");
}

bool isPrintableNoBreaks(const string& value) {
  return !value.empty() && value.find_first_of("\r\n") == string::npos;
}

void ensureDir(const fs::path& path) {
  std::error_code ec;
  fs::create_directories(path, ec);
  if (ec) {
    throw std::runtime_error("Could not create directory " + path.string() +
                             ": " + ec.message());
  }
#ifndef WIN32
  if (chmod(path.c_str(), 0700) != 0) {
    throw std::runtime_error("Could not set permissions on " + path.string() +
                             ": " + strerror(errno));
  }
#endif
}
}  // namespace

bool isValidSessionName(const string& name) {
  return std::regex_match(name, kSessionNameRegex);
}

string sessionDirPath() { return homeDir() + "/.et/sessions"; }

void saveSession(const SessionInfo& info) {
  if (!isValidSessionName(info.name)) {
    throw std::runtime_error("Invalid session name: " + info.name);
  }
  if (!isPrintableNoBreaks(info.host) || !isPrintableNoBreaks(info.id) ||
      !isPrintableNoBreaks(info.passkey) || info.port <= 0 ||
      info.port > 65535) {
    throw std::runtime_error("Session fields must be non-empty and printable");
  }

  const fs::path dir = sessionDirPath();
  ensureDir(homeDir() + "/.et");
  ensureDir(dir);

  // Write to a temp file in the same directory, then rename into place so a
  // crashed writer can never leave a half-written session file behind.
  const fs::path tmpPath = dir / ("." + info.name + "." + genRandomAlphaNum(8));
  const fs::path finalPath = dir / info.name;

  string contents = string("version=") + kSessionVersion + string("\nname=") +
                    info.name + string("\nhost=") + info.host +
                    string("\nport=") + std::to_string(info.port) +
                    string("\nid=") + info.id + string("\npasskey=") +
                    info.passkey + string("\nsavedat=") +
                    std::to_string(info.savedAt) + "\n";
  {
    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
    if (!out) {
      throw std::runtime_error("Could not create temp session file");
    }
    out << contents;
    out.flush();
    if (!out) {
      throw std::runtime_error("Could not write session file");
    }
  }
  std::error_code ec;
  fs::rename(tmpPath, finalPath, ec);
  if (ec) {
    fs::remove(tmpPath);
    throw std::runtime_error("Could not move session file into place: " +
                             ec.message());
  }
#ifndef WIN32
  if (chmod(finalPath.c_str(), 0600) != 0) {
    LOG(WARNING) << "Could not set session file permissions: "
                 << strerror(errno);
  }
#endif
}

optional<SessionInfo> loadSession(const string& name) {
  if (!isValidSessionName(name)) {
    return std::nullopt;
  }
  const fs::path path = sessionDirPath() + "/" + name;
  if (!fs::is_regular_file(path)) {
    return std::nullopt;
  }

  std::ifstream in(path);
  if (!in) {
    return std::nullopt;
  }

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
      haveVersion = (value == kSessionVersion);
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
    }
  }

  if (!haveVersion || !havePort || !haveSavedAt || info.name != name ||
      info.host.empty() || info.id.empty() || info.passkey.empty() ||
      info.port <= 0 || info.port > 65535) {
    return std::nullopt;
  }
  return info;
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
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    // No session directory yet is not an error.
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
  std::error_code ec;
  if (fs::is_regular_file(path) && fs::remove(path, ec) && ec) {
    LOG(WARNING) << "Could not delete session file: " << path.string();
  }
}
}  // namespace et
