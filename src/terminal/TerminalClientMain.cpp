#include <ctime>
#include <cxxopts.hpp>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>

#include "BinaryStdioConsole.hpp"
#include "ClientArgParsing.hpp"
#include "Headers.hpp"
#include "HostParsing.hpp"
#include "MuxClient.hpp"
#include "MuxMaster.hpp"
#include "MuxProtocol.hpp"
#include "OpenSshLocalQueries.hpp"
#include "ParseConfigFile.hpp"
#include "PipeSocketHandler.hpp"
#include "PseudoTerminalConsole.hpp"
#include "SessionStore.hpp"
#include "SshSetupHandler.hpp"
#include "SubprocessUtils.hpp"
#include "TelemetryService.hpp"
#include "TerminalClient.hpp"
#include "TunnelUtils.hpp"
#include "WinsockContext.hpp"

using namespace et;

bool ping(SocketEndpoint socketEndpoint,
          shared_ptr<SocketHandler> clientSocketHandler) {
  VLOG(1) << "Connecting";
  int socketFd = clientSocketHandler->connect(socketEndpoint);
  if (socketFd == -1) {
    VLOG(1) << "Could not connect to host";
    return false;
  }
  clientSocketHandler->close(socketFd);
  return true;
}

void handleParseException(std::exception& e, cxxopts::Options& options) {
  CLOG(INFO, "stdout") << "Exception: " << e.what() << "\n" << endl;
  CLOG(INFO, "stdout") << options.help({}) << endl;
  exit(1);
}

template <class T, class DefaultT>
T extractSingleOptionWithDefault(const cxxopts::ParseResult& result,
                                 const cxxopts::Options& options,
                                 const string& name, DefaultT defaultValue) {
  auto count = result.count(name);
  if (count == 0) {
    return defaultValue;
  }
  if (count == 1) {
    return result[name].as<T>();
  }
  CLOG(INFO, "stdout") << "Value for " << name
                       << " must be specified only once\n";
  CLOG(INFO, "stdout") << options.help({}) << endl;
  exit(0);
}

enum class AttachResult { ATTACHED, INVALID_SESSION, FAILED };
enum class KillResult { KILLED, INVALID_SESSION, FAILED };

bool deleteSavedSession(const string& name) {
  try {
    deleteSession(name);
    return true;
  } catch (const std::exception& e) {
    CLOG(INFO, "stdout") << "Warning: Could not delete saved session '" << name
                         << "': " << e.what() << endl;
    return false;
  }
}

string displayTitle(const string& title) {
  if (title.empty()) {
    return "-";
  }
  if (title.size() <= 32) {
    return title;
  }
  // Leave room for the 3-byte ellipsis without splitting a UTF-8 sequence.
  size_t keep = 29;
  while (keep > 0 && (static_cast<unsigned char>(title[keep]) & 0xc0) == 0x80) {
    --keep;
  }
  return title.substr(0, keep) + "…";
}

void printSessionCandidate(const SessionInfo& session) {
  CLOG(INFO, "stdout") << "  " << session.name << " ["
                       << displayTitle(session.title) << "] (" << session.host
                       << ":" << session.port << ")" << endl;
}

bool sessionNameIsOccupied(const string& name) {
  try {
    const fs::path path = sessionDirPath() + "/" + name;
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec) {
      return ec.value() != ENOENT;
    }
    return status.type() != fs::file_type::not_found;
  } catch (...) {
    return true;
  }
}

string makeDefaultSessionName() {
  char date[16];
  const time_t now = time(NULL);
  struct tm localTm;
#ifdef WIN32
  localtime_s(&localTm, &now);
#else
  localtime_r(&now, &localTm);
#endif
  strftime(date, sizeof(date), "%Y%m%d", &localTm);

  const string base = string(date) + "-";
  string lastCandidate;
  for (int attempt = 0; attempt < 16; ++attempt) {
    const string candidate = base + genRandomAlphaNum(4);
    lastCandidate = candidate;
    if (!sessionNameIsOccupied(candidate)) {
      return candidate;
    }
  }

  // The no-clobber save will fail on an occupied name.
  return lastCandidate;
}

optional<SessionInfo> resolveSavedSession(const string& query) {
  const vector<SessionInfo> savedSessions = listSessions();
  for (const auto& candidate : savedSessions) {
    if (candidate.name == query) {
      return candidate;
    }
  }

  if (!query.empty()) {
    const string lowercaseQuery = lowercaseAscii(query);
    vector<SessionInfo> matches;
    for (const auto& candidate : savedSessions) {
      if (lowercaseAscii(candidate.name).find(lowercaseQuery) != string::npos ||
          lowercaseAscii(candidate.title).find(lowercaseQuery) !=
              string::npos) {
        matches.push_back(candidate);
      }
    }
    if (matches.size() == 1) {
      return matches.front();
    }
    if (matches.size() > 1) {
      CLOG(INFO, "stdout") << "Multiple saved sessions match '" << query
                           << "':" << endl;
      for (const auto& candidate : matches) {
        printSessionCandidate(candidate);
      }
      return nullopt;
    }
  }

  CLOG(INFO, "stdout") << "No saved session named '" << query << "'" << endl;
  for (const auto& candidate : savedSessions) {
    printSessionCandidate(candidate);
  }
  return nullopt;
}

AttachResult attachSavedSession(const string& name, const SessionInfo& session,
                                const string& command, bool noexit,
                                bool noTerminal, int keepaliveDuration) {
  SocketEndpoint endpoint;
  endpoint.set_name(session.host);
  endpoint.set_port(session.port);
  shared_ptr<SocketHandler> socket(new TcpSocketHandler());
  shared_ptr<SocketHandler> pipeSocket(new PipeSocketHandler());

  if (!ping(endpoint, socket)) {
    CLOG(INFO, "stdout") << "Could not reach the ET server: " << endpoint.name()
                         << ":" << endpoint.port() << endl;
    return AttachResult::FAILED;
  }

  shared_ptr<Console> console;
  if (!noTerminal) {
    console.reset(new PseudoTerminalConsole());
  }
  bool sessionEnded = false;
  try {
    TerminalClient client(
        socket, pipeSocket, endpoint, session.id, session.passkey, console,
        /*jumphost=*/false, /*tunnels=*/"", /*reverseTunnels=*/"",
        /*forwardSshAgent=*/false, /*identityAgent=*/"", keepaliveDuration,
        /*envVars=*/{}, /*noPty=*/false, /*command=*/"",
        /*dynamicForwards=*/{}, /*stdioForward=*/"",
        /*maxConnectAttempts=*/15,
        /*resumeSavedSession=*/true, [name]() { return touchSession(name); },
        [name](const string& title) {
          return updateSessionTitle(name, title);
        });
    client.run(command, noexit);
    sessionEnded = client.sessionEndedByServer();
  } catch (const runtime_error& err) {
    if (string(err.what()) == TerminalClient::INVALID_SESSION_CONNECT_ERROR) {
      return AttachResult::INVALID_SESSION;
    }
    CLOG(INFO, "stdout") << "Could not attach to session '" << name
                         << "': " << err.what() << endl;
    return AttachResult::FAILED;
  }

  if (sessionEnded) {
    deleteSavedSession(name);
  }
  return AttachResult::ATTACHED;
}

KillResult killSavedSession(const SessionInfo& session) {
  SocketEndpoint endpoint;
  endpoint.set_name(session.host);
  endpoint.set_port(session.port);
  shared_ptr<SocketHandler> socket(new TcpSocketHandler());
  shared_ptr<SocketHandler> pipeSocket(new PipeSocketHandler());

  if (!ping(endpoint, socket)) {
    CLOG(INFO, "stdout") << "Could not reach the ET server: " << endpoint.name()
                         << ":" << endpoint.port() << endl;
    return KillResult::FAILED;
  }

  try {
    TerminalClient client(
        socket, pipeSocket, endpoint, session.id, session.passkey,
        /*console=*/nullptr, /*jumphost=*/false, /*tunnels=*/"",
        /*reverseTunnels=*/"", /*forwardSshAgent=*/false,
        /*identityAgent=*/"", MAX_CLIENT_KEEP_ALIVE_DURATION,
        /*envVars=*/{}, /*noPty=*/false, /*command=*/"",
        /*dynamicForwards=*/{}, /*stdioForward=*/"",
        /*maxConnectAttempts=*/3,
        /*resumeSavedSession=*/true);
    if (!client.killSession(15)) {
      CLOG(INFO, "stdout")
          << "The server did not confirm termination of session '"
          << session.name << "'" << endl;
      return KillResult::FAILED;
    }
  } catch (const runtime_error& err) {
    if (string(err.what()) == TerminalClient::INVALID_SESSION_CONNECT_ERROR) {
      return KillResult::INVALID_SESSION;
    }
    CLOG(INFO, "stdout") << "Could not kill session '" << session.name
                         << "': " << err.what() << endl;
    return KillResult::FAILED;
  }
  return KillResult::KILLED;
}

// Resolved SSH config information for a host
struct ResolvedSshConfig {
  string hostname;  // Resolved HostName (or original if not an alias)
  string username;  // Username from SSH config (empty if not specified)
};

// Parse the selected SSH policy. An empty path preserves ET's legacy ambient
// behavior, "none" disables parsing, and every other value names the sole
// configuration file to read.
void parseSelectedSshConfig(const string& host, Options* options,
                            const string& sshConfigPath) {
  if (sshConfigPath == "none") {
    return;
  }
  if (!sshConfigPath.empty()) {
    parse_ssh_config_file(host.c_str(), options, sshConfigPath);
    return;
  }

  // Share seen[] across user then system so first-wins (user over system),
  // including explicit zero/"no" values that look unset on Options alone.
  int seen[SOC_END - SOC_UNSUPPORTED] = {0};
  char* homeDir = ssh_get_user_home_dir();
  if (homeDir != NULL) {
    parse_ssh_config_file(host.c_str(), options,
                          string(homeDir) + USER_SSH_CONFIG_PATH, seen);
    free(homeDir);
  }
  parse_ssh_config_file(host.c_str(), options, SYSTEM_SSH_CONFIG_PATH, seen);
}

// Resolve a host alias via SSH config lookup
ResolvedSshConfig resolveSshConfigHost(const string& hostAlias,
                                       const string& sshConfigPath) {
  ResolvedSshConfig result;
  result.hostname = hostAlias;  // Default to original if not resolved

  Options opts = {NULL, NULL, NULL, NULL, NULL, NULL, 0,    0, 0,
                  0,    0,    NULL, NULL, 0,    0,    NULL, {}};

  ssh_options_set(&opts, SSH_OPTIONS_HOST, hostAlias.c_str());
  parseSelectedSshConfig(hostAlias, &opts, sshConfigPath);

  if (opts.host) {
    result.hostname = string(opts.host);
  }
  if (opts.username) {
    result.username = string(opts.username);
  }

  freeOptionsFields(&opts);
  return result;
}

int main(int argc, char** argv) {
  WinsockContext context;
  string tmpDir = GetTempDirectory();

  // Setup easylogging configurations
  el::Configurations defaultConf = LogHandler::setupLogHandler(&argc, &argv);
  LogHandler::setupStdoutLogger();

  et::HandleTerminate();

  // Empty when the session is not saved.
  string sessionName = "";
  bool sessionEndedByServer = false;

  // Override easylogging handler for sigint
  ::signal(SIGINT, et::InterruptSignalHandler);

  Options sshConfigOptions = {
      NULL,  // username
      NULL,  // host
      NULL,  // sshdir
      NULL,  // knownhosts
      NULL,  // ProxyCommand
      NULL,  // ProxyJump
      0,     // timeout
      0,     // port
      0,     // StrictHostKeyChecking
      0,     // ssh2
      0,     // ssh1
      NULL,  // gss_server_identity
      NULL,  // gss_client_identity
      0,     // gss_delegate_creds
      0,     // forward_agent
      NULL,  // identity_agent
      {}     // local_forwards (empty vector)
  };

  // Parse OpenSSH mux Control* options before cxxopts. --ssh-option remains
  // the bootstrap-ssh escape hatch and is not reused for session mux.
  MuxParseResult muxParse;
  try {
    muxParse = parseMuxCliOptions(argc, argv);
  } catch (const std::exception& ex) {
    CLOG(INFO, "stdout") << "Exception: " << ex.what() << endl;
    exit(1);
  }
  MuxOptions muxOptions = muxParse.options;
  vector<string> argvStorage = muxParse.remainingArgs;
  vector<char*> argvPointers;
  argvPointers.reserve(argvStorage.size() + 1);
  for (auto& s : argvStorage) {
    argvPointers.push_back(&s[0]);
  }
  argvPointers.push_back(nullptr);
  argc = static_cast<int>(argvStorage.size());
  argv = argvPointers.data();

  // Parse command line arguments
  cxxopts::Options options("et", "Remote shell for the busy and impatient");
  try {
    options.allow_unrecognised_options();
    options.positional_help("");
    options.custom_help(
        "[OPTION...] [user@]host[:port] [command...]\n\n"
        "  Note that 'host' can be a hostname or ipv4 address with or without "
        "a port\n  or an ipv6 address. If the ipv6 address is abbreviated with "
        ":: then it must\n  be specified without a port (use --port).\n"
        "  A positional command after the host is equivalent to --command "
        "(ssh-style).\n"
        "  -p is the sshd port, not the etserver port. -L/-R/-D/-W are "
        "OpenSSH forwards.\n"
        "  -t requests a pty; --tunnel is the ET forward syntax.\n\n"
        "  OpenSSH mux: -M / -o ControlMaster=yes|auto|no, "
        "-S / -o ControlPath=PATH, -o ControlPersist=yes|<seconds>|no, "
        "-O check|exit|stop|forward|cancel.");

    options.add_options()             //
        ("h,help", "Print help")      //
        ("version", "Print version")  //
        ("V",
         "Print an OpenSSH-compatible version line and exit")  //
        ("G",
         "Print resolved OpenSSH-style configuration for the destination and "
         "exit without connecting")  //
        ("u,username", "Username",
         cxxopts::value<std::string>())  //
        ("host", "Remote host name",
         cxxopts::value<std::string>())  //
        ("port", "Remote machine etserver port",
         cxxopts::value<int>()->default_value("2022"))  //
        ("command", "Run command on connect and exit after command is run",
         cxxopts::value<std::string>())  //
        ("noexit",
         "Used together with --command to not exit after command is run")  //
        ("terminal-path",
         "Path to etterminal on server side. "
         "Use if etterminal is not on the system path.",
         cxxopts::value<std::string>())  //
        ("tunnel",
         "Tunnel: Array of source:destination ports or "
         "srcStart-srcEnd:dstStart-dstEnd (inclusive) port ranges (e.g. "
         "10080:80,10443:443, 10090-10092:8000-8002), ssh-style -L/-R "
         "argument, or Unix socket paths (e.g. "
         "/tmp/local.sock:/tmp/remote.sock, 8080:/tmp/remote.sock, "
         "/tmp/local.sock:8080). Defaults to localhost for bind address "
         "unless ssh-style tunnel argument is used.",
         cxxopts::value<std::string>())  //
        ("r,reversetunnel",
         "Reverse Tunnel: Same syntax as --tunnel but reversed. "
         "-R is the OpenSSH form.",
         cxxopts::value<std::string>())  //
        ("j,jumphost", "jumphost between localhost and destination",
         cxxopts::value<std::string>())  //
        ("jport", "Jumphost machine port",
         cxxopts::value<int>()->default_value("2022"))  //
        ("jserverfifo",
         "If set, communicate to jumphost on the matching fifo name",
         cxxopts::value<string>()->default_value(""))  //
        ("kill-other-sessions",
         "kill all old sessions belonging to the user")  //
        ("close-on-hangup",
         "terminate the remote session when this terminal receives SIGHUP or "
         "closes")  //
        ("disconnect-timeout",
         "Minutes a disconnected etterminal may stay alive before etserver "
         "closes it for this session. 0 means no timeout. Overrides the "
         "etserver global when set.",
         cxxopts::value<int>())  //
        ("macserver",
         "Set when connecting to an macOS server.  Sets "
         "--terminal-path=/usr/local/bin/etterminal")  //
        ("verbose", "Log verbosity. Overrides repeatable -v when both are set.",
         cxxopts::value<int>())  //
        ("k,keepalive", "Client keepalive duration in seconds",
         cxxopts::value<int>())  //
        ("logdir", "Base directory for log files.",
         cxxopts::value<std::string>()->default_value(tmpDir))  //
        ("logtostdout", "Write log to stdout")                  //
        ("silent", "Disable logging")                           //
        ("no-terminal",
         "Do not create a local terminal. The remote shell still starts. "
         "-N is the OpenSSH form and runs no remote command.")  //
        ("D,dynamic",
         "Dynamic application-level port forwarding: listen on "
         "[bind_address:]port and accept SOCKS4/SOCKS5 connections that "
         "choose a remote TCP or Unix destination after connect (ssh -D). "
         "May be specified multiple times.",
         cxxopts::value<std::vector<std::string>>())  //
        ("W,stdio-forward",
         "Forward client stdio to host:port (or a Unix socket path) over the "
         "secure channel without a remote shell (ssh -W). Implies no local "
         "terminal.",
         cxxopts::value<std::string>())  //
        ("T,no-pty",
         "Run the remote command on pipes instead of a pty (binary stdio, "
         "separate stderr, no shell injection). -t requests a pty and wins "
         "when it appears later.")                         //
        ("forward-ssh-agent", "Forward ssh-agent socket")  //
        ("ssh-socket", "The ssh-agent socket to forward",
         cxxopts::value<std::string>())  //
        ("F,ssh-config",
         "Read only this absolute SSH configuration file (or 'none')",
         cxxopts::value<std::string>())  //
        ("no-ssh-config",
         "Do not read user or system SSH configuration for the destination "
         "or jumphost")  //
        ("telemetry",
         "Allow et to anonymously send errors to guide future improvements",
         cxxopts::value<bool>()->default_value("true"))  //
        ("name", "Name this session so it can be reattached later",
         cxxopts::value<std::string>())                             //
        ("no-persist", "Do not save credentials for this session")  //
        ("attach", "Reattach by session name or unique title substring",
         cxxopts::value<std::string>())  //
        ("kill", "End a saved session by name or unique title substring",
         cxxopts::value<std::string>())           //
        ("list", "List saved sessions and exit")  //
        ("serverfifo",
         "If set, communicate to etserver on the matching fifo name",
         cxxopts::value<std::string>()->default_value(""))  //
        ("ssh-option", "Options to pass down to `ssh -o`",
         cxxopts::value<std::vector<std::string>>())  //
        ("o",
         "OpenSSH-style session option applied to the resolved config "
         "(e.g. -o ConnectTimeout=10). Distinct from --ssh-option.",
         cxxopts::value<std::vector<std::string>>())  //
        // These letters are removed by the short-flag pre-pass. They are
        // registered so --help lists the OpenSSH meanings.
        ("p", "sshd port. Does not change the etserver port (--port).",
         cxxopts::value<int>())                                  //
        ("l", "Remote username", cxxopts::value<std::string>())  //
        ("c", "Cipher spec passed to the bootstrap ssh",
         cxxopts::value<std::string>())                                       //
        ("t", "Request a pty. Does not open a tunnel.")                       //
        ("x", "Accepted and ignored (disable X11). Does not kill sessions.")  //
        ("f",
         "Background after the session is up. Does not forward ssh-agent.")  //
        ("N", "Do not run a remote command (forwards only)")                 //
        ("v",
         "Increase log verbosity. Repeatable. --verbose=N overrides it.")  //
        ("e", "Escape character. Accepted and not applied.",
         cxxopts::value<std::string>())  //
        ("L", "Local port forward (OpenSSH -L). Repeatable.",
         cxxopts::value<std::vector<std::string>>())  //
        ("R", "Remote port forward (OpenSSH -R). Repeatable.",
         cxxopts::value<std::vector<std::string>>())  //
        ("i", "Identity file passed to the bootstrap ssh. Repeatable.",
         cxxopts::value<std::vector<std::string>>())  //
        ("J", "Jump host. Same as --jumphost; the later flag wins.",
         cxxopts::value<std::string>());

    options.parse_positional({"host"});
    vector<string> rawArgs;
    rawArgs.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
      rawArgs.emplace_back(argv[i]);
    }
    EtArgvSplit argvSplit = splitEtArgvAtHost(rawArgs);
    vector<char*> clientArgv;
    clientArgv.reserve(argvSplit.clientArgs.size());
    for (auto& arg : argvSplit.clientArgs) {
      clientArgv.push_back(&arg[0]);
    }
    auto result =
        options.parse(static_cast<int>(clientArgv.size()), clientArgv.data());
    TerminalClient::configureCloseOnHangup(result.count("close-on-hangup"));
    if (result.count("close-on-hangup")) {
#ifdef WIN32
      SetConsoleCtrlHandler(TerminalClient::consoleCtrlHandler, TRUE);
#else
      ::signal(SIGHUP, TerminalClient::requestHangupClose);
#endif
    }

    if (result.count("help")) {
      CLOG(INFO, "stdout") << options.help({}) << endl;
      exit(0);
    }

    if (result.count("version")) {
      CLOG(INFO, "stdout") << "et version " << ET_VERSION << endl;
      exit(0);
    }

    // -O control commands talk only to an existing master socket.
    if (!muxOptions.ctlCommand.empty()) {
      if (muxOptions.controlPath.empty()) {
        CLOG(INFO, "stdout")
            << "-O requires ControlPath (-S or -o ControlPath=...)" << endl;
        exit(1);
      }
      MuxClient ctl(muxOptions.controlPath);
      if (muxOptions.ctlCommand == "forward" ||
          muxOptions.ctlCommand == "cancel") {
        if (!ctl.connect()) {
          CLOG(INFO, "stdout")
              << "Control socket connect failed: " << muxOptions.controlPath
              << endl;
          exit(1);
        }
        string tunnel_arg =
            mergeForwardSpecs(extractSingleOptionWithDefault<string>(
                                  result, options, "tunnel", ""),
                              muxParse.ssh.localForwards);
        if (tunnel_arg.empty()) {
          CLOG(INFO, "stdout") << "-O " << muxOptions.ctlCommand
                               << " requires -L or --tunnel" << endl;
          exit(1);
        }
        auto requests = parseRangesToRequests(tunnel_arg);
        int rc = 0;
        for (const auto& pfsr : requests) {
          MuxOpenForwardRequest fwd = muxForwardFromTunnel(pfsr);
          string error;
          bool ok = muxOptions.ctlCommand == "forward"
                        ? ctl.openForward(fwd, &error)
                        : ctl.closeForward(fwd, &error);
          if (!ok) {
            CLOG(INFO, "stdout") << error << endl;
            rc = 1;
          }
        }
        exit(rc);
      }
      exit(ctl.runCtlCommand(muxOptions.ctlCommand));
    }

    // Attach to an existing ControlMaster instead of opening a new session.
    if (shouldAttachToMuxMaster(muxOptions)) {
      MuxClient passenger(muxOptions.controlPath);
      if (!passenger.connect()) {
        CLOG(INFO, "stdout") << "Failed to attach to ControlPath "
                             << muxOptions.controlPath << endl;
        exit(1);
      }
      string tunnel_arg = mergeForwardSpecs(
          extractSingleOptionWithDefault<string>(result, options, "tunnel", ""),
          muxParse.ssh.localForwards);
      if (!tunnel_arg.empty()) {
        auto requests = parseRangesToRequests(tunnel_arg);
        for (const auto& pfsr : requests) {
          MuxOpenForwardRequest fwd = muxForwardFromTunnel(pfsr);
          string error;
          if (!passenger.openForward(fwd, &error)) {
            CLOG(INFO, "stdout")
                << "Mux open forward failed: " << error << endl;
            exit(1);
          }
        }
      }
      string command = resolveRemoteCommand(
          argvSplit.commandOperands, result.count("command") > 0,
          result.count("command") ? result["command"].as<string>() : "");
      if (remoteCommandConflictsWithNoCommand(muxParse.ssh.noRemoteCommand,
                                              command)) {
        CLOG(INFO, "stdout")
            << "-N cannot be combined with a remote command" << endl;
        exit(1);
      }
      const bool wantTty = muxParse.ssh.pty != PtyOverride::Disable &&
                           !muxParse.ssh.noRemoteCommand;
#ifndef WIN32
      uint32_t sessionId = 0;
      uint32_t exitStatus = 255;
      string error;
      if (!passenger.newSession(command, wantTty, STDIN_FILENO, STDOUT_FILENO,
                                STDERR_FILENO, &sessionId, &error,
                                &exitStatus)) {
        CLOG(INFO, "stdout") << "Mux new session failed: " << error << endl;
        exit(1);
      }
#else
      uint32_t sessionId = 0;
      uint32_t exitStatus = 255;
      string error;
      if (!passenger.newSession(command, wantTty, -1, -1, -1, &sessionId,
                                &error, &exitStatus)) {
        CLOG(INFO, "stdout") << "Mux new session failed: " << error << endl;
        exit(1);
      }
#endif
      exit(static_cast<int>(exitStatus));
    }

    if (result.count("V")) {
      CLOG(INFO, "stdout") << openSshCompatibilityVersionLine() << endl;
      exit(0);
    }

    if (result.count("kill") &&
        (result.count("name") || result.count("attach") ||
         result.count("list") || result.count("host"))) {
      CLOG(INFO, "stdout")
          << "--kill takes a saved session name; it cannot be combined with "
             "--name, --attach, --list, or a host"
          << endl;
      exit(1);
    }

    if (result.count("no-persist") &&
        (result.count("name") || result.count("attach") ||
         result.count("kill"))) {
      CLOG(INFO, "stdout")
          << "--no-persist cannot be combined with --name, --attach, or "
             "--kill"
          << endl;
      exit(1);
    }

    if (result.count("list")) {
      CLOG(INFO, "stdout") << left << setw(24) << "NAME" << ' ' << setw(34)
                           << "TITLE" << ' ' << setw(24) << "HOST" << ' '
                           << setw(8) << "PORT" << ' ' << "LAST SEEN" << endl;
      const int64_t now = static_cast<int64_t>(time(NULL));
      for (const auto& session : listSessions()) {
        CLOG(INFO, "stdout")
            << left << setw(24) << session.name << ' ' << setw(34)
            << displayTitle(session.title) << ' ' << setw(24) << session.host
            << ' ' << setw(8) << session.port << ' '
            << formatLastSeen(session.lastSeenAt, now) << endl;
      }
      exit(0);
    }

    if (result.count("attach") &&
        (result.count("name") || result.count("host"))) {
      CLOG(INFO, "stdout") << "--attach takes a session name; it cannot be "
                              "combined with --name or a host"
                           << endl;
      exit(1);
    }
    if (result.count("attach") &&
        (result.count("tunnel") || result.count("reversetunnel") ||
         result.count("dynamic") || result.count("forward-ssh-agent") ||
         result.count("jumphost") || muxParse.ssh.jumpHostSet ||
         !muxParse.ssh.localForwards.empty() ||
         !muxParse.ssh.remoteForwards.empty())) {
      CLOG(INFO, "stdout")
          << "--attach cannot be combined with port forwarding "
             "(--tunnel, -r, -L, -R, -D), --forward-ssh-agent, or a jumphost "
             "(-j/-J); reconnect without --attach to establish forwarding or "
             "a jumphost"
          << endl;
      exit(1);
    }

    if (remotePtyDisabled(muxParse.ssh) &&
        (result.count("name") || result.count("attach"))) {
      CLOG(INFO, "stdout")
          << "-T/--no-pty sessions are not saved and cannot be named or "
             "reattached; drop --name/--attach"
          << endl;
      exit(1);
    }

    const string earlyCommand = resolveRemoteCommand(
        argvSplit.commandOperands, result.count("command") > 0,
        result.count("command") ? result["command"].as<string>() : "");
    if (remoteCommandConflictsWithNoCommand(muxParse.ssh.noRemoteCommand,
                                            earlyCommand)) {
      CLOG(INFO, "stdout") << "-N cannot be combined with a remote command"
                           << endl;
      exit(1);
    }

    int verboseLevel = muxParse.ssh.verboseCount;
    if (result.count("verbose")) {
      // --verbose=N wins over repeatable -v.
      verboseLevel = result["verbose"].as<int>();
    }
    el::Loggers::setVerboseLevel(verboseLevel);

    // silent Flag, since etclient doesn't read /etc/et.cfg file
    if (result.count("silent")) {
      defaultConf.setGlobally(el::ConfigurationType::Enabled, "false");
    }

    LogHandler::setupLogFiles(&defaultConf, result["logdir"].as<string>(),
                              "etclient", result.count("logtostdout"),
                              !result.count("logtostdout"));

    el::Loggers::reconfigureLogger("default", defaultConf);
    // set thread name
    el::Helpers::setThreadName("client-main");

    // Install log rotation callback
    el::Helpers::installPreRollOutCallback(LogHandler::rolloutHandler);

    GOOGLE_PROTOBUF_VERIFY_VERSION;
    srand(1);

    // -f backgrounds after the session is up. Fork before any worker thread
    // exists so the parent can wait, then exit, without duplicating threads.
    // The child signals once TerminalClient has connected.
#ifndef WIN32
    int backgroundWriteFd = -1;
    if (muxParse.ssh.background && result.count("host") && !result.count("G")) {
      int notifyPipe[2] = {-1, -1};
      if (pipe(notifyPipe) != 0) {
        CLOG(INFO, "stdout") << "Failed to background: pipe failed" << endl;
        exit(1);
      }
      pid_t pid = fork();
      if (pid < 0) {
        CLOG(INFO, "stdout") << "Failed to background: fork failed" << endl;
        exit(1);
      }
      if (pid > 0) {
        close(notifyPipe[1]);
        char byte = 0;
        ssize_t n = ::read(notifyPipe[0], &byte, 1);
        _exit(n == 1 && byte == 1 ? 0 : 1);
      }
      close(notifyPipe[0]);
      backgroundWriteFd = notifyPipe[1];
    }
#endif

    TelemetryService::create(result["telemetry"].as<bool>(),
                             tmpDir + "/.sentry-native-et", "Client");

    if (result.count("kill")) {
      const optional<SessionInfo> session =
          resolveSavedSession(result["kill"].as<string>());
      if (!session) {
        exit(1);
      }
      const KillResult killResult = killSavedSession(*session);
      if (killResult == KillResult::INVALID_SESSION) {
        if (!deleteSavedSession(session->name)) {
          exit(1);
        }
        CLOG(INFO, "stdout")
            << "Session '" << session->name
            << "' was already gone; removed stale record" << endl;
        exit(0);
      }
      if (killResult == KillResult::FAILED) {
        CLOG(INFO, "stdout") << "Session '" << session->name
                             << "' was not removed; retry --kill or delete "
                                "~/.et/sessions/"
                             << session->name << " manually" << endl;
        exit(1);
      }
      if (!deleteSavedSession(session->name)) {
        exit(1);
      }
      CLOG(INFO, "stdout") << "Killed session '" << session->name << "'"
                           << endl;
      exit(0);
    }

    if (result.count("attach")) {
      const optional<SessionInfo> session =
          resolveSavedSession(result["attach"].as<string>());
      if (!session) {
        exit(1);
      }
      const string attachName = session->name;

      int attachKeepalive = extractSingleOptionWithDefault<int>(
          result, options, "keepalive", MAX_CLIENT_KEEP_ALIVE_DURATION);
      const AttachResult attachResult = attachSavedSession(
          attachName, *session,
          result.count("command") ? result["command"].as<string>() : "",
          result.count("noexit"), result.count("no-terminal"), attachKeepalive);
      if (attachResult == AttachResult::INVALID_SESSION) {
        deleteSavedSession(attachName);
        CLOG(INFO, "stdout")
            << "Session '" << attachName << "' is no longer running on "
            << session->host << endl;
        exit(1);
      }
      if (attachResult == AttachResult::FAILED) {
        exit(1);
      }
      exit(0);
    }
    string username = "";
    if (result.count("username")) {
      username = result["username"].as<string>();
    }
    int destinationPort = result["port"].as<int>();
    string destinationHost;

    // Parse command-line argument
    if (!result.count("host")) {
      CLOG(INFO, "stdout") << "Missing host to connect to" << endl;
      CLOG(INFO, "stdout") << options.help({}) << endl;
      exit(0);
    }
    string host_arg = result["host"].as<std::string>();
    ParsedEtDestination parsedDestination;
    try {
      parsedDestination = parseEtDestinationHost(host_arg);
    } catch (const std::invalid_argument&) {
      CLOG(INFO, "stdout") << "Invalid host positional arg: " << host_arg
                           << endl;
      exit(1);
    }
    if (!parsedDestination.username.empty()) {
      username = parsedDestination.username;
    }
    // -l is the OpenSSH login name and wins over -u and user@.
    if (muxParse.ssh.loginNameSet) {
      username = muxParse.ssh.loginName;
    }
    if (parsedDestination.hasExplicitPort) {
      destinationPort = parsedDestination.port;
    }
    destinationHost = parsedDestination.host;
    // host_alias is used for the initiating ssh call, if sshd runs on a port
    // other than 22, either configure your .ssh/config with an alias with an
    // overridden port or pass --ssh-option Port=<sshd_port>
    string host_alias = destinationHost;

    const bool jumphostSpecified =
        result.count("jumphost") > 0 || muxParse.ssh.jumpHostSet;
    string jumphost =
        resolveJumpHost(muxParse.ssh, result.count("jumphost") > 0,
                        extractSingleOptionWithDefault<string>(result, options,
                                                               "jumphost", ""));
    if (strcasecmp(jumphost.c_str(), "none") == 0) {
      jumphost.clear();
    }
    if (result.count("ssh-config") && result.count("no-ssh-config")) {
      CLOG(INFO, "stdout")
          << "--ssh-config and --no-ssh-config are mutually exclusive" << endl;
      exit(1);
    }
    string sshConfigPath;
    if (result.count("ssh-config")) {
      sshConfigPath = result["ssh-config"].as<string>();
      if (sshConfigPath != "none" &&
          !std::filesystem::path(sshConfigPath).is_absolute()) {
        CLOG(INFO, "stdout")
            << "--ssh-config must be an absolute path or 'none': "
            << sshConfigPath << endl;
        exit(1);
      }
      if (sshConfigPath != "none" &&
          !SshSetupHandler::IsSshConfigPathSafeForProxyJump(sshConfigPath)) {
        CLOG(INFO, "stdout")
            << "--ssh-config must contain only ASCII letters, digits, '/', "
#ifdef WIN32
               "'\\\\', ':', "
#endif
               "'.', '_', and '-'; OpenSSH does not quote this path when "
               "propagating it through ProxyJump"
            << endl;
        exit(1);
      }
      if (sshConfigPath != "none") {
        std::error_code configError;
        std::filesystem::file_status configStatus =
            std::filesystem::symlink_status(sshConfigPath, configError);
        bool regularFile = std::filesystem::is_regular_file(configStatus);
        bool readableFile = false;
        if (!configError && regularFile) {
          std::ifstream configFile(sshConfigPath);
          readableFile = configFile.good();
        }
        if (!readableFile) {
          CLOG(INFO, "stdout")
              << "--ssh-config must name a readable, non-symlink regular file"
              << endl;
          exit(1);
        }
      }
    } else if (result.count("no-ssh-config")) {
      sshConfigPath = "none";
    }
    bool noSshConfig = sshConfigPath == "none";
    int keepaliveDuration = extractSingleOptionWithDefault<int>(
        result, options, "keepalive", MAX_CLIENT_KEEP_ALIVE_DURATION);
    if (keepaliveDuration < 1 ||
        keepaliveDuration > MAX_CLIENT_KEEP_ALIVE_DURATION) {
      CLOG(INFO, "stdout") << "Keep-alive duration must between 1 and "
                           << MAX_CLIENT_KEEP_ALIVE_DURATION << " seconds"
                           << endl;
      CLOG(INFO, "stdout") << options.help({}) << endl;
      exit(0);
    }

    optional<int> disconnectTimeoutMinutes;
    if (result.count("disconnect-timeout")) {
      int minutes = result["disconnect-timeout"].as<int>();
      if (minutes < 0) {
        CLOG(INFO, "stdout")
            << "--disconnect-timeout must be a non-negative number of minutes"
            << endl;
        CLOG(INFO, "stdout") << options.help({}) << endl;
        exit(1);
      }
      if (minutes > std::numeric_limits<int>::max() / 60) {
        CLOG(INFO, "stdout") << "--disconnect-timeout is too large" << endl;
        CLOG(INFO, "stdout") << options.help({}) << endl;
        exit(1);
      }
      disconnectTimeoutMinutes = minutes;
    }

    if (!noSshConfig) {
      ssh_options_set(&sshConfigOptions, SSH_OPTIONS_HOST,
                      destinationHost.c_str());
      parseSelectedSshConfig(destinationHost, &sshConfigOptions, sshConfigPath);
      if (sshConfigOptions.host) {
        LOG(INFO) << "Parsed ssh config file, connecting to "
                  << sshConfigOptions.host;
        destinationHost = string(sshConfigOptions.host);
      }
    }

    // --name attaches if the session exists and creates it otherwise.
    optional<SessionInfo> namedSession;
    if (result.count("name")) {
      sessionName = result["name"].as<string>();
      if (!isValidSessionName(sessionName)) {
        CLOG(INFO, "stdout") << "Invalid session name: " << sessionName << endl;
        exit(1);
      }
#ifdef WIN32
      CLOG(INFO, "stdout")
          << "Warning: Session persistence is unavailable on Windows until "
             "owner-only credential storage is configured"
          << endl;
      sessionName.clear();
#else
      try {
        namedSession = loadSession(sessionName);
      } catch (const std::exception& e) {
        CLOG(INFO, "stdout")
            << "Warning: Named session storage is unavailable: " << e.what()
            << ". Continuing without saving this session." << endl;
        sessionName.clear();
      }
#endif
    }

    // Parse username: cmdline > sshconfig > localuser
    if (username.empty()) {
      if (sshConfigOptions.username) {
        username = string(sshConfigOptions.username);
      } else {
        char* usernamePtr = ssh_get_local_username();
        username = string(usernamePtr);
        SAFE_FREE(usernamePtr);
      }
    }

    // Apply OpenSSH-style -o session options after config resolution so they
    // override file values. --ssh-option remains a separate bootstrap-ssh path.
    if (result.count("o")) {
      for (const auto& sessionOption : result["o"].as<std::vector<string>>()) {
        if (!applySessionOption(&sshConfigOptions, sessionOption)) {
          CLOG(INFO, "stdout")
              << "Invalid -o option: " << sessionOption << endl;
          exit(1);
        }
        string key = sessionOption;
        size_t sep = key.find('=');
        if (sep == string::npos) {
          sep = key.find(' ');
        }
        if (sep != string::npos) {
          key = key.substr(0, sep);
        }
        key = lowercaseAscii(trimAsciiBlanks(std::move(key)));
        if (key == "hostname" && sshConfigOptions.host) {
          destinationHost = string(sshConfigOptions.host);
        } else if (key == "user" && sshConfigOptions.username) {
          username = string(sshConfigOptions.username);
        }
      }
    }

    // -p is the sshd port. It overrides config and -o Port, and it does not
    // change the etserver port (that stays --port / host:port).
    if (muxParse.ssh.sshPortSet) {
      int sshPort = muxParse.ssh.sshPort;
      if (ssh_options_set(&sshConfigOptions, SSH_OPTIONS_PORT, &sshPort) != 0) {
        CLOG(INFO, "stdout") << "Invalid sshd port: " << sshPort << endl;
        exit(1);
      }
    }

    if (result.count("G")) {
      CLOG(INFO, "stdout") << formatOpenSshResolvedConfig(
          host_alias, destinationHost, username, sshConfigOptions);
      exit(0);
    }

    // Parse jumphost: cmd > sshconfig
    if (!jumphostSpecified && sshConfigOptions.ProxyJump &&
        strcasecmp(sshConfigOptions.ProxyJump, "none") != 0 &&
        jumphost.length() == 0) {
      string proxyjump = string(sshConfigOptions.ProxyJump);
      // Keep full ProxyJump value including SSH port for ssh -J command
      jumphost = proxyjump;
      LOG(INFO) << "ProxyJump found for dst in ssh config: " << proxyjump;
    }

    bool is_jumphost = false;
    SocketEndpoint socketEndpoint;
    if (!jumphost.empty()) {
      is_jumphost = true;
      LOG(INFO) << "Setting port to jumphost port";

      // Parse [user@]host[:sshport] format
      ParsedHostString parsed = parseHostString(jumphost);

      // Resolve jumphost aliases only when SSH configuration is enabled.
      // In --no-ssh-config mode, keep the command-line host and user exact.
      ResolvedSshConfig resolved =
          resolveSshConfigHost(parsed.host, sshConfigPath);
      if (resolved.hostname != parsed.host) {
        LOG(INFO) << "Resolved jumphost alias '" << parsed.host
                  << "' to hostname: " << resolved.hostname;
      }

      // Determine username: command-line > SSH config > local user
      string jumphostUser = parsed.user;
      if (jumphostUser.empty() && !resolved.username.empty()) {
        jumphostUser = resolved.username;
        LOG(INFO) << "Using jumphost username from SSH config: "
                  << jumphostUser;
      }
      if (jumphostUser.empty() && !noSshConfig) {
        char* localUsernamePtr = ssh_get_local_username();
        jumphostUser = string(localUsernamePtr);
        SAFE_FREE(localUsernamePtr);
      }

      // Reconstruct jumphost with resolved hostname for SSH -J flag
      jumphost = (jumphostUser.empty() ? "" : jumphostUser + "@") +
                 resolved.hostname + parsed.portSuffix;

      socketEndpoint.set_name(resolved.hostname);
      socketEndpoint.set_port(result["jport"].as<int>());
    } else {
      socketEndpoint.set_name(destinationHost);
      socketEndpoint.set_port(destinationPort);
    }

    bool forwardingRequested =
        result.count("tunnel") || result.count("reversetunnel") ||
        result.count("dynamic") || result.count("forward-ssh-agent") ||
        !muxParse.ssh.localForwards.empty() ||
        !muxParse.ssh.remoteForwards.empty();
#ifndef WIN32
    forwardingRequested = forwardingRequested ||
                          sshConfigOptions.forward_agent ||
                          !sshConfigOptions.local_forwards.empty();
#endif

    if (is_jumphost) {
      if (namedSession) {
        CLOG(INFO, "stdout")
            << "Session '" << sessionName
            << "' cannot be reattached through a jumphost because the saved "
               "record does not contain jumphost metadata; use --attach "
               "without a jumphost"
            << endl;
        exit(1);
      }
      if (!result.count("no-persist")) {
        CLOG(INFO, "stdout")
            << "Warning: Sessions using a jumphost are not saved because the "
               "saved record does not contain jumphost metadata"
            << endl;
        sessionName.clear();
      }
    } else if (!result.count("name") && !result.count("no-persist") &&
               !remotePtyDisabled(muxParse.ssh)) {
      // Neither --attach nor a restarted etserver can resume a -T stream.
#ifdef WIN32
      CLOG(INFO, "stdout")
          << "Warning: Session persistence is unavailable on Windows until "
             "owner-only credential storage is configured"
          << endl;
#else
      sessionName = makeDefaultSessionName();
#endif
    }

    if (forwardingRequested && !sessionName.empty()) {
      CLOG(INFO, "stdout")
          << "Warning: Saved-session reattach restores the shell but does "
             "not recreate port or SSH agent forwarding"
          << endl;
    }

    if (namedSession) {
      if (namedSession->host != socketEndpoint.name() ||
          namedSession->port != socketEndpoint.port()) {
        CLOG(INFO, "stdout") << "session " << sessionName << " is saved for "
                             << namedSession->host << ":" << namedSession->port
                             << "; use --attach " << sessionName
                             << " or a different --name" << endl;
        exit(1);
      }

      const AttachResult attachResult = attachSavedSession(
          sessionName, *namedSession,
          resolveRemoteCommand(
              argvSplit.commandOperands, result.count("command") > 0,
              result.count("command") ? result["command"].as<string>() : ""),
          result.count("noexit"), result.count("no-terminal"),
          keepaliveDuration);
      if (attachResult == AttachResult::ATTACHED) {
        exit(0);
      }
      if (attachResult == AttachResult::FAILED) {
        exit(1);
      }

      deleteSavedSession(sessionName);
      CLOG(INFO, "stdout") << "Session '" << sessionName
                           << "' is no longer running; creating a fresh session"
                           << endl;
    }

    shared_ptr<SocketHandler> clientSocket(new TcpSocketHandler());
    shared_ptr<SocketHandler> clientPipeSocket(new PipeSocketHandler());

    if (!ping(socketEndpoint, clientSocket)) {
      CLOG(INFO, "stdout") << "Could not reach the ET server: "
                           << socketEndpoint.name() << ":"
                           << socketEndpoint.port() << endl;
      exit(1);
    }

    string jServerFifo = "";
    if (result["jserverfifo"].as<string>() != "") {
      jServerFifo = result["jserverfifo"].as<string>();
    }

    string serverFifo = "";
    if (result["serverfifo"].as<string>() != "") {
      serverFifo = result["serverfifo"].as<string>();
    }
    std::vector<string> ssh_options;
    if (result.count("ssh-option")) {
      ssh_options = result["ssh-option"].as<std::vector<string>>();
    }
    string etterminal_path = "";
    if (result.count("macserver") > 0) {
      etterminal_path = "/usr/local/bin/etterminal";
    }
    if (result.count("terminal-path")) {
      etterminal_path = result["terminal-path"].as<string>();
    }

    shared_ptr<Console> console;
    string stdioForward = extractSingleOptionWithDefault<string>(
        result, options, "stdio-forward", "");
    // -t / -T / --no-pty: the later flag wins. None means the existing pty
    // path.
    const bool noPty = remotePtyDisabled(muxParse.ssh);
    string command = resolveRemoteCommand(
        argvSplit.commandOperands, result.count("command") > 0,
        result.count("command") ? result["command"].as<string>() : "");
    const string commandOptionsError =
        remoteCommandOptionsError(muxParse.ssh, command, !stdioForward.empty());
    if (!commandOptionsError.empty()) {
      CLOG(INFO, "stdout") << commandOptionsError << endl;
      if (noPty && command.empty()) {
        CLOG(INFO, "stdout") << options.help({}) << endl;
      }
      exit(1);
    }
    if (!stdioForward.empty() || muxParse.ssh.noRemoteCommand ||
        result.count("no-terminal")) {
      // -W and -N do not attach a local shell. --no-terminal only hides the
      // local console; the remote shell still starts.
    } else if (noPty) {
      console.reset(new BinaryStdioConsole());
    } else {
      console.reset(new PseudoTerminalConsole());
    }

    bool forwardAgent = result.count("forward-ssh-agent") > 0;
    string sshSocket = "";
#ifndef WIN32
    if (sshConfigOptions.identity_agent) {
      sshSocket = string(sshConfigOptions.identity_agent);
    }
    forwardAgent |= sshConfigOptions.forward_agent;
#endif
    if (result.count("ssh-socket")) {
      sshSocket = result["ssh-socket"].as<string>();
    }
    TelemetryService::get()->logToDatadog("Session Started", el::Level::Info,
                                          __FILE__, __LINE__);
    string tunnel_arg = mergeForwardSpecs(
        extractSingleOptionWithDefault<string>(result, options, "tunnel", ""),
        muxParse.ssh.localForwards);
    string r_tunnel_arg =
        mergeForwardSpecs(extractSingleOptionWithDefault<string>(
                              result, options, "reversetunnel", ""),
                          muxParse.ssh.remoteForwards);
    vector<string> dynamicForwards;
    if (result.count("dynamic")) {
      dynamicForwards = result["dynamic"].as<vector<string>>();
    }

    for (const auto& localForward : sshConfigOptions.local_forwards) {
      string tunnelEntry =
          to_string(localForward.first) + ":" + to_string(localForward.second);
      LOG(INFO) << "Adding tunnel from SSH config LocalForward: "
                << tunnelEntry;
      if (tunnel_arg.empty()) {
        tunnel_arg = tunnelEntry;
      } else {
        tunnel_arg += "," + tunnelEntry;
      }
    }

    auto subprocessUtils = make_shared<SubprocessUtils>();
    SshSetupHandler sshSetupHandler(subprocessUtils, sshConfigPath);
    // -T uses BinaryStdioConsole so the remote command owns stdout. SSH
    // banners and shell prompts must stay off that channel.
    sshSetupHandler.setDisplayLoginOutput(console != nullptr && !noPty);
    const BootstrapSshPort bootstrapPort = bootstrapSshPort(muxParse.ssh);
    sshSetupHandler.setBootstrapOverrides(
        bootstrapPort.set, bootstrapPort.port, muxParse.ssh.identityFiles,
        muxParse.ssh.cipherSet ? muxParse.ssh.cipher : "");
    pair<string, string> idpasskeypair;
    try {
      idpasskeypair = sshSetupHandler.SetupSsh(
          username, destinationHost, host_alias, destinationPort, jumphost,
          jServerFifo, result.count("kill-other-sessions") > 0, verboseLevel,
          etterminal_path, serverFifo, ssh_options);
    } catch (const runtime_error&) {
      // SetupSsh already printed a message without the ssh output.
      exit(1);
    }

    // Save before connecting so a local failure leaves the session
    // recoverable.
    if (!sessionName.empty()) {
      try {
        SessionInfo sessionInfo;
        sessionInfo.name = sessionName;
        sessionInfo.host = socketEndpoint.name();
        sessionInfo.port = socketEndpoint.port();
        sessionInfo.id = idpasskeypair.first;
        sessionInfo.passkey = idpasskeypair.second;
        sessionInfo.savedAt = (int64_t)time(NULL);
        saveSession(sessionInfo, /*replaceExisting=*/false);
      } catch (const std::exception& se) {
        LOG(WARNING) << "Could not save session '" << sessionName
                     << "': " << se.what();
        CLOG(INFO, "stdout")
            << "Warning: Could not save session '" << sessionName
            << "': this connection will not be recoverable "
               "after the client exits"
            << endl;
        sessionName = "";
      }
    }

    TerminalClient terminalClient(
        clientSocket, clientPipeSocket, socketEndpoint, idpasskeypair.first,
        idpasskeypair.second, console, is_jumphost, tunnel_arg, r_tunnel_arg,
        forwardAgent, sshSocket, keepaliveDuration, sshConfigOptions.env_vars,
        noPty, command, dynamicForwards, stdioForward,
        /*maxConnectAttempts=*/3,
        /*resumeSavedSession=*/false,
        [&sessionName]() {
          return sessionName.empty() || touchSession(sessionName);
        },
        [&sessionName](const string& title) {
          return sessionName.empty() || updateSessionTitle(sessionName, title);
        },
        disconnectTimeoutMinutes, muxParse.ssh.noRemoteCommand);

#ifndef WIN32
    if (backgroundWriteFd >= 0) {
      ::setsid();
      int devnull = ::open("/dev/null", O_RDONLY);
      if (devnull >= 0) {
        ::dup2(devnull, STDIN_FILENO);
        if (devnull != STDIN_FILENO) {
          ::close(devnull);
        }
      }
      char ok = 1;
      if (::write(backgroundWriteFd, &ok, 1) != 1) {
        CLOG(INFO, "stdout") << "Failed to detach background client" << endl;
      }
      ::close(backgroundWriteFd);
      backgroundWriteFd = -1;
    }
#endif

    unique_ptr<MuxMaster> muxMaster;
    if (shouldBecomeMuxMaster(muxOptions)) {
      muxMaster = make_unique<MuxMaster>(muxOptions.controlPath,
                                         muxOptions.controlPersist);
      muxMaster->setPortForwardHandler(terminalClient.getPortForwardHandler());
      try {
        muxMaster->start();
        LOG(INFO) << "ControlMaster listening on " << muxOptions.controlPath;
      } catch (const std::exception& ex) {
        CLOG(INFO, "stdout")
            << "Failed to start ControlMaster: " << ex.what() << endl;
        exit(1);
      }
    }
    const int remoteExitStatus =
        terminalClient.run(command, result.count("noexit"));
    sessionEndedByServer = terminalClient.sessionEndedByServer();

    if (muxMaster) {
      muxMaster->notifyPrimaryClientExited();
      if (muxOptions.controlPersist.enabled) {
        // Passenger attach is only available while ControlPersist keeps the
        // transport serviced; interactive primary sessions keep the console.
        bool handlerReady = false;
        terminalClient.serviceIdleUntil([&]() {
          // Register the handler on the first service tick so idleServicing
          // is already true before any passenger can attach.
          if (!handlerReady) {
            muxMaster->setPassengerSessionHandler(
                [&terminalClient](int inFd, int outFd, int errFd,
                                  const string& passengerCommand,
                                  bool /*wantTty*/) -> uint32_t {
                  terminalClient.beginPassengerWatch();
                  return terminalClient.runPassengerSession(inFd, outFd, errFd,
                                                            passengerCommand);
                });
            muxMaster->setPassengerCancelHandler([&terminalClient]() {
              terminalClient.cancelPassengerSession();
            });
            handlerReady = true;
          }
          return muxMaster->isRunning() && !muxMaster->persistExpired();
        });
      }
      muxMaster->stop();
    }

    // Any other exit leaves the remote shell running and reattachable.
    if (!sessionName.empty() && sessionEndedByServer) {
      deleteSavedSession(sessionName);
    }

    // Clean up ssh config options
    freeOptionsFields(&sshConfigOptions);

#ifdef WIN32
    WSACleanup();
#endif

    TelemetryService::get()->shutdown();
    TelemetryService::destroy();

    // Uninstall log rotation callback
    el::Helpers::uninstallPreRollOutCallback();

    return remoteExitStatus;
  } catch (TunnelParseException& tpe) {
    handleParseException(tpe, options);
  } catch (cxxopts::exceptions::exception& oe) {
    handleParseException(oe, options);
  }

  // Clean up ssh config options
  freeOptionsFields(&sshConfigOptions);

#ifdef WIN32
  WSACleanup();
#endif

  TelemetryService::get()->shutdown();
  TelemetryService::destroy();

  // Any other exit leaves the remote shell running and reattachable.
  if (!sessionName.empty() && sessionEndedByServer) {
    deleteSavedSession(sessionName);
  }

  // Uninstall log rotation callback
  el::Helpers::uninstallPreRollOutCallback();

  return 0;
}
