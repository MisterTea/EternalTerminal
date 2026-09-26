#pragma once

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <sstream>
#include <string>

#include "ParseConfigFile.hpp"

#if !defined(_MSC_VER)
extern "C" {
extern char** environ;
}
#endif

namespace et {

inline string openSshCompatibilityVersionLine() {
  return string("OpenSSH_9.9p1 EternalTerminal_") + ET_VERSION;
}

inline string lowercaseAscii(string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(tolower(c)); });
  return value;
}

/** @brief Trim leading/trailing ASCII blanks (space/tab), matching OpenSSH
 *  `-o "Key = Value"` acceptance. */
inline string trimAsciiBlanks(string value) {
  while (!value.empty() && isblank(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() && isblank(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

inline string normalizeControlMasterDump(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return "false";
  }
  string normalized = lowercaseAscii(value);
  if (normalized == "no" || normalized == "false") {
    return "false";
  }
  return normalized;
}

inline string normalizeYesNoDump(int value) { return value ? "yes" : "no"; }

inline bool parseYesNoOption(const string& value, int* out) {
  if (strcasecmp(value.c_str(), "yes") == 0 ||
      strcasecmp(value.c_str(), "true") == 0) {
    *out = 1;
    return true;
  }
  if (strcasecmp(value.c_str(), "no") == 0 ||
      strcasecmp(value.c_str(), "false") == 0) {
    *out = 0;
    return true;
  }
  return false;
}

/** @brief Parse value as a full-string nonnegative long (rejects trailing
 *  garbage and empty/partial parses that strtol alone would accept). */
inline bool parseEntireNonNegativeLong(const string& value, long* out) {
  if (out == nullptr || value.empty()) {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const long parsed = strtol(value.c_str(), &end, 10);
  if (end == value.c_str() || end == nullptr || *end != '\0' ||
      errno == ERANGE || parsed < 0) {
    return false;
  }
  *out = parsed;
  return true;
}

/** @brief Apply one OpenSSH-style `-o Key=Value` (or `Key Value`) session
 *  option to the resolved Options. Distinct from `--ssh-option`, which is
 *  only forwarded to the bootstrap ssh process. */
inline bool applySessionOption(Options* options, const string& option) {
  if (options == nullptr || option.empty()) {
    return false;
  }

  string key;
  string value;
  size_t eq = option.find('=');
  // Equals form when the key side (trimmed) is a single token. Space form
  // otherwise, including `-o "SetEnv NAME=VALUE"` where the value contains `=`.
  bool useEqualsForm = false;
  if (eq != string::npos) {
    string keySide = trimAsciiBlanks(option.substr(0, eq));
    if (!keySide.empty() && keySide.find_first_of(" \t") == string::npos) {
      useEqualsForm = true;
    }
  }
  if (useEqualsForm) {
    key = option.substr(0, eq);
    value = option.substr(eq + 1);
  } else {
    size_t start = 0;
    while (start < option.size() &&
           isblank(static_cast<unsigned char>(option[start]))) {
      start++;
    }
    size_t sp = start;
    while (sp < option.size() &&
           !isblank(static_cast<unsigned char>(option[sp]))) {
      sp++;
    }
    if (sp == start || sp >= option.size()) {
      return false;
    }
    key = option.substr(start, sp - start);
    size_t valueStart = sp;
    while (valueStart < option.size() &&
           isblank(static_cast<unsigned char>(option[valueStart]))) {
      valueStart++;
    }
    value = option.substr(valueStart);
  }

  key = trimAsciiBlanks(std::move(key));
  value = trimAsciiBlanks(std::move(value));
  if (key.empty()) {
    return false;
  }

  string keyLower = lowercaseAscii(key);
  int yes = 0;

  if (keyLower == "hostname") {
    return ssh_options_set(options, SSH_OPTIONS_HOST, value.c_str()) == 0;
  }
  if (keyLower == "user") {
    return ssh_options_set(options, SSH_OPTIONS_USER, value.c_str()) == 0;
  }
  if (keyLower == "port") {
    long port = 0;
    if (!parseEntireNonNegativeLong(value, &port) || port < 1 || port > 65535) {
      return false;
    }
    int portInt = static_cast<int>(port);
    return ssh_options_set(options, SSH_OPTIONS_PORT, &portInt) == 0;
  }
  if (keyLower == "connecttimeout") {
    if (strcasecmp(value.c_str(), "none") == 0) {
      options->timeout = 0;
      return true;
    }
    long timeout = 0;
    if (!parseEntireNonNegativeLong(value, &timeout)) {
      return false;
    }
    return ssh_options_set(options, SSH_OPTIONS_TIMEOUT, &timeout) == 0;
  }
  if (keyLower == "serveraliveinterval") {
    long interval = 0;
    if (!parseEntireNonNegativeLong(value, &interval)) {
      return false;
    }
    return ssh_options_set(options, SSH_OPTIONS_SERVERALIVEINTERVAL,
                           &interval) == 0;
  }
  if (keyLower == "clearallforwardings") {
    if (!parseYesNoOption(value, &yes)) {
      return false;
    }
    return ssh_options_set(options, SSH_OPTIONS_CLEARALLFORWARDINGS, &yes) == 0;
  }
  if (keyLower == "exitonforwardfailure") {
    if (!parseYesNoOption(value, &yes)) {
      return false;
    }
    return ssh_options_set(options, SSH_OPTIONS_EXITONFORWARDFAILURE, &yes) ==
           0;
  }
  if (keyLower == "batchmode") {
    if (!parseYesNoOption(value, &yes)) {
      return false;
    }
    return ssh_options_set(options, SSH_OPTIONS_BATCHMODE, &yes) == 0;
  }
  if (keyLower == "remotecommand") {
    return ssh_options_set(options, SSH_OPTIONS_REMOTECOMMAND, value.c_str()) ==
           0;
  }
  if (keyLower == "controlmaster") {
    return ssh_options_set(options, SSH_OPTIONS_CONTROLMASTER, value.c_str()) ==
           0;
  }
  if (keyLower == "controlpath") {
    return ssh_options_set(options, SSH_OPTIONS_CONTROLPATH, value.c_str()) ==
           0;
  }
  if (keyLower == "controlpersist") {
    return ssh_options_set(options, SSH_OPTIONS_CONTROLPERSIST,
                           value.c_str()) == 0;
  }
  if (keyLower == "localforward") {
    return ssh_options_set(options, SSH_OPTIONS_LOCALFORWARD, value.c_str()) ==
           0;
  }
  if (keyLower == "remoteforward") {
    return ssh_options_set(options, SSH_OPTIONS_REMOTEFORWARD, value.c_str()) ==
           0;
  }
  if (keyLower == "dynamicforward") {
    return ssh_options_set(options, SSH_OPTIONS_DYNAMICFORWARD,
                           value.c_str()) == 0;
  }
  if (keyLower == "sendenv") {
    if (value.empty()) {
      return false;
    }
    size_t pos = 0;
    bool any = false;
    while (pos < value.size()) {
      while (pos < value.size() &&
             isblank(static_cast<unsigned char>(value[pos]))) {
        pos++;
      }
      if (pos >= value.size()) {
        break;
      }
      size_t end = pos;
      while (end < value.size() &&
             !isblank(static_cast<unsigned char>(value[end]))) {
        end++;
      }
      string pattern = value.substr(pos, end - pos);
      if (ssh_options_set(options, SSH_OPTIONS_SENDENV, pattern.c_str()) != 0) {
        return false;
      }
      any = true;
      pos = end;
    }
    return any;
  }
  if (keyLower == "setenv") {
    if (value.empty()) {
      return false;
    }
    size_t pos = 0;
    bool any = false;
    while (pos < value.size()) {
      while (pos < value.size() &&
             isblank(static_cast<unsigned char>(value[pos]))) {
        pos++;
      }
      if (pos >= value.size()) {
        break;
      }
      size_t end = pos;
      while (end < value.size() &&
             !isblank(static_cast<unsigned char>(value[end]))) {
        end++;
      }
      string assignment = value.substr(pos, end - pos);
      if (ssh_options_set(options, SSH_OPTIONS_SETENV, assignment.c_str()) !=
          0) {
        return false;
      }
      any = true;
      pos = end;
    }
    return any;
  }

  // Unrecognized session options are ignored (OpenSSH warns; we stay quiet).
  return true;
}

/** @brief OpenSSH wipes every forward when ClearAllForwardings is yes. */
inline void applyClearAllForwardings(Options* options) {
  if (options == nullptr || !options->clear_all_forwardings) {
    return;
  }
  options->local_forwards.clear();
  options->remote_forwards.clear();
  options->dynamic_forwards.clear();
}

/**
 * @brief ET keepalive interval. An explicit --keepalive wins. Otherwise a
 *        positive ServerAliveInterval is used, clamped to ET's maximum.
 *        Zero leaves the ET default: the server drops a quiet client.
 */
inline int resolveEtKeepaliveSeconds(bool keepaliveExplicit, int keepaliveValue,
                                     unsigned long serverAliveInterval) {
  if (keepaliveExplicit || serverAliveInterval == 0) {
    return keepaliveValue;
  }
  if (serverAliveInterval >
      static_cast<unsigned long>(MAX_CLIENT_KEEP_ALIVE_DURATION)) {
    return MAX_CLIENT_KEEP_ALIVE_DURATION;
  }
  return static_cast<int>(serverAliveInterval);
}

/**
 * @brief Command-line command wins. RemoteCommand none means no command.
 *        `-N` (SessionType none) suppresses config RemoteCommand the same way
 *        OpenSSH does; a non-empty CLI command is still returned so callers can
 *        report the -N conflict.
 */
inline string resolveConfiguredRemoteCommand(const string& cliCommand,
                                             const char* remoteCommand,
                                             bool noRemoteCommand = false) {
  if (!cliCommand.empty()) {
    return cliCommand;
  }
  if (noRemoteCommand) {
    return "";
  }
  if (remoteCommand == nullptr || remoteCommand[0] == '\0' ||
      strcasecmp(remoteCommand, "none") == 0) {
    return "";
  }
  return remoteCommand;
}

/** @brief Pass BatchMode through to bootstrap ssh when ET resolved it. */
inline void appendBatchModeSshOption(vector<string>* sshOptions,
                                     int batchMode) {
  if (sshOptions == nullptr || !batchMode) {
    return;
  }
  for (const auto& option : *sshOptions) {
    string lower = lowercaseAscii(option);
    if (lower.rfind("batchmode", 0) == 0) {
      return;
    }
  }
  sshOptions->push_back("BatchMode=yes");
}

/** @brief Copy local variables whose names match SendEnv patterns. */
inline vector<pair<string, string>> selectSendEnv(
    const vector<string>& patterns,
    const vector<pair<string, string>>& localEnv) {
  vector<pair<string, string>> selected;
  for (const auto& pattern : patterns) {
    for (const auto& env : localEnv) {
      if (!sshEnvPatternMatches(pattern, env.first)) {
        continue;
      }
      bool already = false;
      for (const auto& have : selected) {
        if (have.first == env.first) {
          already = true;
          break;
        }
      }
      if (!already) {
        selected.push_back(env);
      }
    }
  }
  return selected;
}

/** @brief SendEnv matches first, then SetEnv assignments replace them. */
inline vector<pair<string, string>> mergeSessionEnvironment(
    const vector<string>& sendPatterns,
    const vector<pair<string, string>>& setenv,
    const vector<pair<string, string>>& localEnv) {
  vector<pair<string, string>> merged = selectSendEnv(sendPatterns, localEnv);
  for (const auto& assigned : setenv) {
    bool replaced = false;
    for (auto& existing : merged) {
      if (existing.first == assigned.first) {
        existing.second = assigned.second;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      merged.push_back(assigned);
    }
  }
  return merged;
}

inline vector<pair<string, string>> captureLocalEnviron() {
  vector<pair<string, string>> vars;
#ifdef _MSC_VER
  // Prefer the CRT environ block over Win32 GetEnvironmentStringsA so this
  // header does not need windows.h (httplib must be included before it).
  char** env = _environ;
#else
  char** env = environ;
#endif
  if (env == nullptr) {
    return vars;
  }
  for (char** it = env; *it != nullptr; ++it) {
    string entry(*it);
    size_t eq = entry.find('=');
    if (eq == string::npos || eq == 0) {
      continue;
    }
    vars.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
  }
  return vars;
}

inline string formatOpenSshResolvedConfig(const string& hostAlias,
                                          const string& hostname,
                                          const string& user,
                                          const Options& opts) {
  std::ostringstream out;
  out << "host " << hostAlias << "\n";
  out << "user " << user << "\n";
  out << "hostname " << hostname << "\n";
  out << "port " << (opts.port ? opts.port : 22) << "\n";
  out << "batchmode " << normalizeYesNoDump(opts.batch_mode) << "\n";
  out << "clearallforwardings "
      << normalizeYesNoDump(opts.clear_all_forwardings) << "\n";
  out << "exitonforwardfailure "
      << normalizeYesNoDump(opts.exit_on_forward_failure) << "\n";
  out << "serveraliveinterval " << opts.server_alive_interval << "\n";
  if (opts.timeout == 0) {
    out << "connecttimeout none\n";
  } else {
    out << "connecttimeout " << opts.timeout << "\n";
  }
  out << "controlmaster " << normalizeControlMasterDump(opts.control_master)
      << "\n";
  if (opts.control_path && opts.control_path[0] != '\0') {
    out << "controlpath " << opts.control_path << "\n";
  }
  if (opts.control_persist && opts.control_persist[0] != '\0') {
    string persist = lowercaseAscii(opts.control_persist);
    if (persist == "false") {
      persist = "no";
    }
    out << "controlpersist " << persist << "\n";
  } else {
    out << "controlpersist no\n";
  }
  if (opts.remote_command && opts.remote_command[0] != '\0') {
    out << "remotecommand " << opts.remote_command << "\n";
  }
  for (const auto& forward : opts.local_forwards) {
    out << "localforward " << forward << "\n";
  }
  for (const auto& forward : opts.remote_forwards) {
    out << "remoteforward " << forward << "\n";
  }
  for (const auto& forward : opts.dynamic_forwards) {
    out << "dynamicforward " << forward << "\n";
  }
  for (const auto& pattern : opts.send_env) {
    out << "sendenv " << pattern << "\n";
  }
  for (const auto& env : opts.env_vars) {
    out << "setenv " << env.first << "=" << env.second << "\n";
  }
  return out.str();
}

}  // namespace et
