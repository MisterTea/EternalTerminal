#pragma once

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>

#include "ParseConfigFile.hpp"

namespace et {

inline string openSshCompatibilityVersionLine() {
  return string("OpenSSH_9.9p1 EternalTerminal_") + ET_VERSION;
}

inline string lowercaseAscii(string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(tolower(c)); });
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
  if (eq != string::npos) {
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

  while (!key.empty() && isblank(static_cast<unsigned char>(key.front()))) {
    key.erase(key.begin());
  }
  while (!key.empty() && isblank(static_cast<unsigned char>(key.back()))) {
    key.pop_back();
  }
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
    return ssh_options_set(options, SSH_OPTIONS_PORT_STR, value.c_str()) == 0;
  }
  if (keyLower == "connecttimeout") {
    if (strcasecmp(value.c_str(), "none") == 0) {
      options->timeout = 0;
      return true;
    }
    long timeout = strtol(value.c_str(), nullptr, 10);
    if (timeout < 0) {
      return false;
    }
    return ssh_options_set(options, SSH_OPTIONS_TIMEOUT, &timeout) == 0;
  }
  if (keyLower == "serveraliveinterval") {
    long interval = strtol(value.c_str(), nullptr, 10);
    if (interval < 0) {
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

  // Unrecognized session options are ignored (OpenSSH warns; we stay quiet).
  return true;
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
  return out.str();
}

}  // namespace et
