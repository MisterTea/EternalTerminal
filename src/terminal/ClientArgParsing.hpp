#ifndef __ET_CLIENT_ARG_PARSING__
#define __ET_CLIENT_ARG_PARSING__

#include "Headers.hpp"

namespace et {

// Destination host after stripping optional user@ and :port from the host
// positional, matching TerminalClientMain's [user@]host[:port] rules (including
// unbracketed IPv6 forms).
struct ParsedEtDestination {
  string username;  // from user@ prefix; empty if absent
  string host;
  bool hasExplicitPort = false;
  int port = 0;
};

inline ParsedEtDestination parseEtDestinationHost(const string& host_arg_in) {
  ParsedEtDestination result;
  string host_arg = host_arg_in;

  if (host_arg.find('@') != string::npos) {
    int i = host_arg.find('@');
    result.username = host_arg.substr(0, i);
    host_arg = host_arg.substr(i + 1);
  }

  if (host_arg.find(':') != string::npos) {
    int colon_count = std::count(host_arg.begin(), host_arg.end(), ':');
    if (colon_count == 1) {
      // ipv4 or hostname with port specified
      int port_colon_pos = host_arg.rfind(':');
      result.port = stoi(host_arg.substr(port_colon_pos + 1));
      result.hasExplicitPort = true;
      host_arg = host_arg.substr(0, port_colon_pos);
    } else {
      // maybe ipv6 (colon_count >= 2)
      if (host_arg.find("::") != string::npos) {
        // ipv6 with double colon zero abbreviation and no port
        // leave host_arg as is
      } else {
        if (colon_count == 7) {
          // ipv6, fully expanded, without port
        } else if (colon_count == 8) {
          // ipv6, fully expanded, with port
          int port_colon_pos = host_arg.rfind(':');
          result.port = stoi(host_arg.substr(port_colon_pos + 1));
          result.hasExplicitPort = true;
          host_arg = host_arg.substr(0, port_colon_pos);
        } else {
          throw std::invalid_argument("Invalid host positional arg: " +
                                      host_arg_in);
        }
      }
    }
  }
  result.host = host_arg;
  return result;
}

// Join remote command operands with spaces the way ssh joins command argv
// words (preserve each word; do not run a local shell).
inline string joinRemoteCommandOperands(const vector<string>& operands) {
  string command;
  for (size_t i = 0; i < operands.size(); ++i) {
    if (i > 0) {
      command += " ";
    }
    command += operands[i];
  }
  return command;
}

// Prefer a positional command over -c/--command when both are present.
inline string resolveRemoteCommand(const vector<string>& positionalOperands,
                                   bool hasCommandFlag,
                                   const string& commandFlagValue) {
  if (!positionalOperands.empty()) {
    return joinRemoteCommandOperands(positionalOperands);
  }
  if (hasCommandFlag) {
    return commandFlagValue;
  }
  return "";
}

}  // namespace et

#endif  // __ET_CLIENT_ARG_PARSING__
