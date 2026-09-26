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

// Options that consume the following argv token. Everything after the host
// positional is a remote command word, including tokens that look like flags.
// OpenSSH letters whose meaning changed (`-p`, `-l`, `-c`, `-t`, `-v`, ...)
// are removed by the short-flag pre-pass and are not listed here. `-D` and
// `-W` are re-emitted by that pre-pass and still take a value. `-j` is still
// the jumphost and is re-emitted without consuming its value, so it is listed
// here along with the named-session options.
inline bool etOptionConsumesValue(const string& arg) {
  return arg == "-u" || arg == "--username" || arg == "--port" ||
         arg == "--command" || arg == "--terminal-path" || arg == "--tunnel" ||
         arg == "-r" || arg == "--reversetunnel" || arg == "-j" ||
         arg == "--jumphost" || arg == "--jport" || arg == "--jserverfifo" ||
         arg == "--verbose" || arg == "-k" || arg == "--keepalive" ||
         arg == "--logdir" || arg == "--ssh-socket" || arg == "-F" ||
         arg == "--ssh-config" || arg == "--telemetry" ||
         arg == "--serverfifo" || arg == "--ssh-option" || arg == "-o" ||
         arg == "--disconnect-timeout" || arg == "-D" || arg == "--dynamic" ||
         arg == "-W" || arg == "--stdio-forward" || arg == "--name" ||
         arg == "--attach" || arg == "--kill";
}

struct EtArgvSplit {
  vector<string> clientArgs;
  vector<string> commandOperands;
};

inline EtArgvSplit splitEtArgvAtHost(const vector<string>& args) {
  EtArgvSplit split;
  if (args.empty()) {
    return split;
  }
  split.clientArgs.push_back(args[0]);
  size_t i = 1;
  // A bare `--` before the host ends client option parsing (ssh-style). Do
  // not treat following tokens as remote operands yet — the next token is
  // the host. Re-emit `--` into clientArgs so cxxopts will accept a host
  // that looks like a flag; without a following host token, `--` alone must
  // not be forwarded (that would leave cxxopts with no host).
  bool optionsEnded = false;
  for (; i < args.size(); ++i) {
    const string& arg = args[i];
    if (!optionsEnded && arg == "--") {
      optionsEnded = true;
      continue;
    }
    if (!optionsEnded && arg.size() >= 2 && arg[0] == '-') {
      split.clientArgs.push_back(arg);
      // `--serverfifo=/tmp/x` already carries its value. A separate value
      // token is only consumed for a bare option name.
      if (arg.find('=') == string::npos && etOptionConsumesValue(arg) &&
          i + 1 < args.size()) {
        split.clientArgs.push_back(args[++i]);
      }
      continue;
    }
    if (optionsEnded) {
      split.clientArgs.push_back("--");
    }
    split.clientArgs.push_back(arg);
    ++i;
    break;
  }
  for (; i < args.size(); ++i) {
    split.commandOperands.push_back(args[i]);
  }
  return split;
}

// Prefer a positional command over --command when both are present. `-c` is an
// OpenSSH cipher spec and is not a remote command.
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
