#include "SshSetupHandler.hpp"

#include "HostParsing.hpp"

namespace et {
namespace {

bool isAlphaNumeric(const string& value) {
  return !value.empty() &&
         all_of(value.begin(), value.end(), [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'A' && character <= 'Z') ||
                  (character >= 'a' && character <= 'z');
         });
}

optional<pair<string, string>> parseIdPasskey(const string& output) {
  const string marker = "IDPASSKEY:";
  const size_t idLength = 16;
  const size_t payloadLength = idLength + 1 + 32;
  const size_t markerIndex = output.find(marker);
  if (markerIndex == string::npos) {
    return nullopt;
  }

  const size_t payloadIndex = markerIndex + marker.length();
  if (output.length() - payloadIndex < payloadLength) {
    return nullopt;
  }

  const string payload = output.substr(payloadIndex, payloadLength);
  if (payload[idLength] != '/') {
    return nullopt;
  }
  const size_t suffixIndex = payloadIndex + payloadLength;
  if (suffixIndex < output.length() &&
      string(" \t\n\r").find(output[suffixIndex]) == string::npos) {
    return nullopt;
  }

  const string id = payload.substr(0, idLength);
  const string passkey = payload.substr(idLength + 1);
  if (!isAlphaNumeric(id) || !isAlphaNumeric(passkey)) {
    return nullopt;
  }

  return make_pair(id, passkey);
}

[[noreturn]] void failSshSetup(const string& message) {
  // The ssh command and server output can both contain an id/passkey pair.
  CLOG(INFO, "stdout") << message << endl;
  throw runtime_error(message);
}

}  // namespace

const string SshSetupHandler::ETTERMINAL_BIN = "etterminal";

namespace {
/** @brief Characters OpenSSH can splice into an unquoted ProxyJump -F path. */
bool isSshConfigPathCharSafeForProxyJump(unsigned char c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
      (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_' || c == '-') {
    return true;
  }
#ifdef WIN32
  // Native Windows paths need a drive colon and backslash; those are not
  // Bourne metacharacters, and Unix clients never pass Windows paths.
  return c == ':' || c == '\\';
#else
  return false;
#endif
}
}  // namespace

bool SshSetupHandler::IsSshConfigPathSafeForProxyJump(const string& path) {
  if (!std::filesystem::path(path).is_absolute()) {
    return false;
  }
  for (unsigned char c : path) {
    if (!isSshConfigPathCharSafeForProxyJump(c)) {
      return false;
    }
  }
  return true;
}

string genCommand(const string& passkey, const string& id,
                  const string& clientTerm, const string& user, bool kill,
                  const string& etterminal_path, const string& options) {
  string ssh_script_prefix;
  string etterminal_bin = etterminal_path.empty()
                              ? SshSetupHandler::ETTERMINAL_BIN
                              : etterminal_path;

  string command = "echo '" + id + "/" + passkey + "_" + clientTerm + "' | " +
                   etterminal_bin + " " + options;

  // Kill old ET sessions of the user
  if (kill) {
    ssh_script_prefix =
        "pkill etterminal -u " + user + "; sleep 0.5; " + ssh_script_prefix;
  }

  return ssh_script_prefix + command;
}

pair<string, string> SshSetupHandler::SetupSsh(
    const string& user, const string& host, const string& host_alias, int port,
    const string& jumphost, const string& jServerFifo, bool kill, int vlevel,
    const string& cmd_prefix, const string& serverFifo,
    const std::vector<std::string>& ssh_options) {
  string clientTerm("xterm-256color");
  auto envString = getenv("TERM");
  if (envString != NULL) {
    // Default to xterm-256color
    clientTerm = envString;
  }
  string passkey = genRandomAlphaNum(32);
  string id = genRandomAlphaNum(16);

  id[0] = id[1] = id[2] = 'X';  // For compatibility with old servers that do
                                // not generate their own keys

  string cmdoptions{"--verbose=" + std::to_string(vlevel)};
  if (!serverFifo.empty()) {
    cmdoptions += " --serverfifo=" + serverFifo;
  }

  string SSH_SCRIPT_DST =
      genCommand(passkey, id, clientTerm, user, kill, cmd_prefix, cmdoptions);

  string SSH_USER_PREFIX = "";
  if (!user.empty()) {
    SSH_USER_PREFIX += user + "@";
  }

  std::vector<std::string> ssh_args;
  if (!sshConfigPath_.empty()) {
    // An explicit `-F` suppresses OpenSSH's user and system configuration.
    // OpenSSH also propagates it to the implicit proxy command created by
    // `-J`, so the explicit jumphost uses the same selected policy.
    ssh_args.push_back("-F");
    ssh_args.push_back(sshConfigPath_);
  }
  if (!jumphost.empty()) {
    ssh_args.push_back("-J");
    ssh_args.push_back(jumphost);
  }

  ssh_args.push_back(SSH_USER_PREFIX + host_alias);

  for (auto& arg : ssh_options) {
    ssh_args.push_back("-o" + arg);
  }

  ssh_args.push_back(SSH_SCRIPT_DST);

  VLOG(1) << "Trying ssh connection to " << SSH_USER_PREFIX + host_alias
          << endl;
  string sshBuffer;
  try {
    sshBuffer =
        subprocessUtils_->SubprocessToStringInteractive("ssh", ssh_args);
  } catch (const std::exception&) {
    failSshSetup(
        "Error starting ET process through ssh, please make sure your ssh "
        "works first");
  }

  if (sshBuffer.empty()) {
    failSshSetup(
        "Error starting ET process through ssh, please make sure your ssh "
        "works first");
  }

  const auto serverCredentials = parseIdPasskey(sshBuffer);
  if (!serverCredentials) {
    failSshSetup(
        "Error in authentication with etserver, please make sure you don't "
        "print anything in server's .bashrc/.zshrc and that etserver returns "
        "a valid IDPASSKEY response");
  }
  id = serverCredentials->first;
  passkey = serverCredentials->second;
  if (displayLoginOutput_) {
    const string loginOutput = ExtractLoginOutput(sshBuffer);
    if (!loginOutput.empty()) {
      CLOG(INFO, "stdout") << loginOutput;
      if (loginOutput.back() != '\n') {
        CLOG(INFO, "stdout") << endl;
      }
    }
  }
  LOG(INFO) << "etserver started";

  // start jumpclient daemon on jumphost.
  if (!jumphost.empty()) {
    /* If jumphost is set, we need to pass dst host and port to jumphost
     * and connect to jumphost here */
    string jump_cmdoptions{"--verbose=" + std::to_string(vlevel)};
    if (!jServerFifo.empty()) {
      jump_cmdoptions += " --serverfifo=" + jServerFifo;
    }
    jump_cmdoptions = jump_cmdoptions + " --jump --dsthost=" + host +
                      " --dstport=" + to_string(port);
    string SSH_SCRIPT_JUMP = genCommand(passkey, id, clientTerm, user, kill,
                                        cmd_prefix, jump_cmdoptions);

    // Parse jumphost to extract port for -p flag (ssh destination doesn't
    // support user@host:port, only -J does)
    ParsedHostString parsedJump = parseHostString(jumphost);

    // Strip brackets from IPv6 addresses for ssh destination
    // (ssh [::1] fails, but ssh ::1 works)
    string jumphostAddr = parsedJump.host;
    if (jumphostAddr.length() >= 2 && jumphostAddr.front() == '[' &&
        jumphostAddr.back() == ']') {
      jumphostAddr = jumphostAddr.substr(1, jumphostAddr.length() - 2);
    }

    string jumphostDest = parsedJump.user.empty()
                              ? jumphostAddr
                              : parsedJump.user + "@" + jumphostAddr;

    std::vector<std::string> jump_ssh_args;
    if (!sshConfigPath_.empty()) {
      jump_ssh_args.push_back("-F");
      jump_ssh_args.push_back(sshConfigPath_);
    }
    if (!parsedJump.portSuffix.empty()) {
      // portSuffix includes the colon, e.g. ":22"
      jump_ssh_args.push_back("-p");
      jump_ssh_args.push_back(parsedJump.portSuffix.substr(1));
    }
    // ssh_options configure the destination. Jump-specific options are
    // resolved independently from the jumphost's SSH configuration.
    jump_ssh_args.push_back(jumphostDest);
    jump_ssh_args.push_back(SSH_SCRIPT_JUMP);

    string sshLinkBuffer;
    try {
      sshLinkBuffer =
          subprocessUtils_->SubprocessToStringInteractive("ssh", jump_ssh_args);
    } catch (const std::exception&) {
      failSshSetup("etserver jumpclient failed to start");
    }
    if (sshLinkBuffer.length() <= 0) {
      // At this point "ssh -J jumphost dst" already works.
      failSshSetup("etserver jumpclient failed to start");
    }

    const auto jumpCredentials = parseIdPasskey(sshLinkBuffer);
    if (!jumpCredentials) {
      failSshSetup(
          "Error initializing connection: etserver jumpclient returned an "
          "invalid IDPASSKEY response");
    }
    id = jumpCredentials->first;
    passkey = jumpCredentials->second;
  }

  if (id.length() == 0 || passkey.length() == 0) {
    STFATAL << "Somehow missing id or passkey: " << id.length() << " "
            << passkey.length();
  }
  return {id, passkey};
}

string SshSetupHandler::ExtractLoginOutput(const string& sshOutput) {
  const string marker = "IDPASSKEY:";
  const auto markerPosition = sshOutput.find(marker);
  if (markerPosition == string::npos) {
    return sshOutput;
  }

  constexpr size_t credentialLength = 16 + 1 + 32;
  const size_t credentialEnd =
      min(sshOutput.size(), markerPosition + marker.size() + credentialLength);
  string output = sshOutput.substr(0, markerPosition);
  string suffix = sshOutput.substr(credentialEnd);

  // etterminal prints the credential on its own line. Remove that line break,
  // while preserving all actual login output around it.
  if (!suffix.empty() && suffix.front() == '\r') {
    suffix.erase(0, 1);
  }
  if (!suffix.empty() && suffix.front() == '\n') {
    suffix.erase(0, 1);
  }
  output += suffix;
  return output;
}
}  // namespace et
