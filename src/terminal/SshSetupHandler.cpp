#include "SshSetupHandler.hpp"

#include "SubprocessToString.hpp"

namespace et {
string genCommand(const string& passkey, const string& id,
                  const string& clientTerm, const string& user, bool kill,
                  const string& command_prefix, const string& options) {
  string SSH_SCRIPT_PREFIX;

  string COMMAND = "echo '" + id + "/" + passkey + "_" + clientTerm + "\n' | " +
                   command_prefix + " etterminal " + options;

  // Kill old ET sessions of the user
  if (kill) {
    SSH_SCRIPT_PREFIX =
        "pkill etterminal -u " + user + "; sleep 0.5; " + SSH_SCRIPT_PREFIX;
  }

  return SSH_SCRIPT_PREFIX + COMMAND;
}

string SshSetupHandler::SetupSsh(const string& user, const string& host,
                                 const string& host_alias, int port,
                                 const string& jumphost, int jport, bool kill,
                                 int vlevel, const string& cmd_prefix,
                                 const string& serverFifo,
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

  std::vector<std::string> ssh_args ;
  if (!jumphost.empty()) {
    ssh_args = {
      "-J",
      SSH_USER_PREFIX + jumphost,
    };
  }

  ssh_args.push_back(SSH_USER_PREFIX + host_alias);

  for (auto& arg : ssh_options) {
    ssh_args.push_back("-o" + arg);
  }

  ssh_args.push_back(SSH_SCRIPT_DST);

  auto sshBuffer = SubprocessToStringInteractive("ssh", ssh_args);

  try {
    if (sshBuffer.length() <= 0) {
      // Ssh failed
      CLOG(INFO, "stdout")
          << "Error starting ET process through ssh, please make sure your "
             "ssh works first"
          << endl;
      exit(1);
    }
    auto passKeyIndex = sshBuffer.find(string("IDPASSKEY:"));
    if (passKeyIndex == string::npos) {
      // Returned value not contain "IDPASSKEY:"
      CLOG(INFO, "stdout")
          << "Error in authentication with etserver: " << sshBuffer
          << ", please make sure you don't print anything in server's "
             ".bashrc/.zshrc"
          << endl;
      exit(1);
    }
    auto idpasskey = sshBuffer.substr(passKeyIndex + 10, 16 + 1 + 32);
    auto idpasskey_splited = split(idpasskey, '/');
    id = idpasskey_splited[0];
    passkey = idpasskey_splited[1];
    LOG(INFO) << "etserver started";
  } catch (const runtime_error& err) {
    CLOG(INFO, "stdout") << "Error initializing connection" << err.what()
                         << endl;
  }

  // start jumpclient daemon on jumphost.
  if (!jumphost.empty()) {
    /* If jumphost is set, we need to pass dst host and port to jumphost
     * and connect to jumphost here */
    string cmdoptions{"--verbose=" + std::to_string(vlevel)};
    string jump_cmdoptions = cmdoptions + " --jump --dsthost=" + host +
                             " --dstport=" + to_string(port);
    string SSH_SCRIPT_JUMP = genCommand(passkey, id, clientTerm, user, kill,
                                        cmd_prefix, jump_cmdoptions);

    string sshLinkBuffer =
        SubprocessToStringInteractive("ssh", {jumphost, SSH_SCRIPT_JUMP});
    if (sshLinkBuffer.length() <= 0) {
      // At this point "ssh -J jumphost dst" already works.
      CLOG(INFO, "stdout") << "etserver jumpclient failed to start" << endl;
      exit(1);
    }
    try {
      auto idpasskey = split(sshLinkBuffer, ':')[1];
      idpasskey.erase(idpasskey.find_last_not_of(" \n\r\t") + 1);
      idpasskey = idpasskey.substr(0, 16 + 1 + 32);
      auto idpasskey_splited = split(idpasskey, '/');
      id = idpasskey_splited[0];
      passkey = idpasskey_splited[1];
    } catch (const runtime_error& err) {
      CLOG(INFO, "stdout") << "Error initializing connection" << err.what()
                           << endl;
    }
  }

  if (id.length() == 0 || passkey.length() == 0) {
    STFATAL << "Somehow missing id or passkey: " << id.length() << " "
            << passkey.length();
  }
  return id + "/" + passkey;
}
}  // namespace et
