#include "SshSetupHandler.hpp"

#include <sodium.h>
#include <sys/wait.h>

namespace et {
string genRandom(int len) {
  static const char alphanum[] =
      "0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz";
  string s(len, '\0');

  for (int i = 0; i < len; ++i) {
    s[i] = alphanum[randombytes_uniform(sizeof(alphanum) - 1)];
  }

  return s;
}

string genCommand(const string &passkey, const string &id,
                  const string &clientTerm, const string &user, bool kill,
                  const string &command_prefix, const string &options) {
  string SSH_SCRIPT_PREFIX;

  string COMMAND = "echo \"" + id + "/" + passkey + "_" + clientTerm +
                   "\n\" | " + command_prefix + " etterminal " + options;

  // Kill old ET sessions of the user
  if (kill) {
    SSH_SCRIPT_PREFIX =
        "pkill etterminal -u " + user + "; " + SSH_SCRIPT_PREFIX;
  }

  return SSH_SCRIPT_PREFIX + COMMAND;
}

string SshSetupHandler::SetupSsh(const string &user, const string &host,
                                 const string &host_alias, int port,
                                 const string &jumphost, int jport, bool kill,
                                 int vlevel, const string &cmd_prefix,
                                 optional<string> serverFifo) {
  string clientTerm("xterm-256color");
  auto envString = getenv("TERM");
  if (envString != NULL) {
    // Default to xterm-256color
    clientTerm = envString;
  }
  string passkey = genRandom(32);
  string id = genRandom(16);
  string cmdoptions{"--verbose=" + std::to_string(vlevel)};
  if (bool(serverFifo)) {
    cmdoptions += " --serverfifo=" + *serverFifo;
  }

  string SSH_SCRIPT_DST =
      genCommand(passkey, id, clientTerm, user, kill, cmd_prefix, cmdoptions);

  int link_client[2];
  char buf_client[4096];
  if (pipe(link_client) == -1) {
    LOG(FATAL) << "pipe";
    exit(1);
  }

  pid_t pid = fork();
  string SSH_USER_PREFIX = "";
  if (!user.empty()) {
    SSH_USER_PREFIX += user + "@";
  }
  if (!pid) {
    // start etserver daemon on dst.
    dup2(link_client[1], 1);
    close(link_client[0]);
    close(link_client[1]);
    // run the command in interactive mode
    if (!jumphost.empty()) {
      execlp("ssh", "ssh", "-J", (SSH_USER_PREFIX + jumphost).c_str(),
             (SSH_USER_PREFIX + host_alias).c_str(), (SSH_SCRIPT_DST).c_str(),
             NULL);
    } else {
      execlp("ssh", "ssh", (SSH_USER_PREFIX + host_alias).c_str(),
             SSH_SCRIPT_DST.c_str(), NULL);
    }

    LOG(INFO) << "execl error";
    exit(1);
  } else if (pid < 0) {
    LOG(INFO) << "Failed to fork";
    exit(1);
  } else {
    close(link_client[1]);
    wait(NULL);
    int nbytes = read(link_client[0], buf_client, sizeof(buf_client));
    try {
      if (nbytes <= 0) {
        // Ssh failed
        cout << "Error starting ET process through ssh, please make sure your "
                "ssh works first"
             << endl;
        exit(1);
      }
      auto sshBuffer = string(buf_client, nbytes);
      auto passKeyIndex = sshBuffer.find(string("IDPASSKEY:"));
      if (passKeyIndex == string::npos) {
        // Returned value not contain "IDPASSKEY:"
        cout << "Error in authentication with etserver: " << sshBuffer
             << ", please make sure you don't print anything in server's "
                ".bashrc/.zshrc"
             << endl;
        exit(1);
      }
      auto idpasskey = sshBuffer.substr(passKeyIndex + 10, 16 + 1 + 32);
      auto idpasskey_splited = split(idpasskey, '/');
      string returned_id = idpasskey_splited[0];
      string returned_passkey = idpasskey_splited[1];
      if (returned_id == id && returned_passkey == passkey) {
        LOG(INFO) << "etserver started";
      } else {
        LOG(FATAL) << "client/server idpasskey doesn't match: " << id
                   << " != " << returned_id << " or " << passkey
                   << " != " << returned_passkey;
      }
    } catch (const runtime_error &err) {
      cout << "Error initializing connection" << err.what() << endl;
    }
    // start jumpclient daemon on jumphost.
    if (!jumphost.empty()) {
      /* If jumphost is set, we need to pass dst host and port to jumphost
       * and connect to jumphost here */
      int link_jump[2];
      char buf_jump[4096];
      if (pipe(link_jump) == -1) {
        LOG(FATAL) << "pipe";
        exit(1);
      }
      pid_t pid_jump = fork();
      if (pid_jump < 0) {
        LOG(FATAL) << "Failed to fork";
        exit(1);
      } else if (pid_jump == 0) {
        dup2(link_jump[1], 1);
        close(link_jump[0]);
        close(link_jump[1]);
        string jump_cmdoptions = cmdoptions + " --jump --dsthost=" + host +
                                 " --dstport=" + to_string(port);
        string SSH_SCRIPT_JUMP = genCommand(passkey, id, clientTerm, user, kill,
                                            cmd_prefix, jump_cmdoptions);
        // start command in interactive mode
        SSH_SCRIPT_JUMP = "$SHELL -lc \'" + SSH_SCRIPT_JUMP + "\'";
        execlp("ssh", "ssh", jumphost.c_str(), SSH_SCRIPT_JUMP.c_str(), NULL);
      } else {
        close(link_jump[1]);
        wait(NULL);
        int nbytes = read(link_jump[0], buf_jump, sizeof(buf_jump));
        if (nbytes <= 0) {
          // At this point "ssh -J jumphost dst" already works.
          cout << "etserver jumpclient failed to start" << endl;
          exit(1);
        }
        try {
          auto idpasskey = split(string(buf_jump), ':')[1];
          idpasskey.erase(idpasskey.find_last_not_of(" \n\r\t") + 1);
          idpasskey = idpasskey.substr(0, 16 + 1 + 32);
          auto idpasskey_splited = split(idpasskey, '/');
          string returned_id = idpasskey_splited[0];
          string returned_passkey = idpasskey_splited[1];
          if (returned_id == id && returned_passkey == passkey) {
            LOG(INFO) << "jump client started.";
          } else {
            LOG(FATAL) << "client/server idpasskey doesn't match: " << id
                       << " != " << returned_id << " or " << passkey
                       << " != " << returned_passkey;
          }
        } catch (const runtime_error &err) {
          cout << "Error initializing connection" << err.what() << endl;
        }
      }
    }
  }
  return id + "/" + passkey;
}
}  // namespace et
