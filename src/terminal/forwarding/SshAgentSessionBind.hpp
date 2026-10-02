#ifndef __ET_SSH_AGENT_SESSION_BIND__
#define __ET_SSH_AGENT_SESSION_BIND__

#include "Headers.hpp"
#include "SocketHandler.hpp"

/**
 * OpenSSH agent session binding (`session-bind@openssh.com`, OpenSSH 8.9+).
 *
 * ssh tells its agent which host key and SSH session each agent connection
 * belongs to, which is what makes destination-constrained keys
 * (`ssh-add -h`) enforceable. ET forwards the agent over its own transport, so
 * the client replays the binding ssh made while bootstrapping the session,
 * flagged as forwarded, at the start of every forwarded agent connection. This
 * is the same message ssh itself sends on an agent-forwarding channel.
 */
namespace et {

/** @brief Largest agent message accepted (OpenSSH's AGENT_MAX_LEN). */
constexpr uint32_t SSH_AGENT_MAX_MESSAGE_LENGTH = 256 * 1024;

/**
 * @brief Converts the session-bind request ssh sends for its own
 * authentication into the forwarded form.
 * @param request Agent message body, without the length prefix.
 * @return The same binding with is_forwarding set, or nullopt when @p request
 * is not a well-formed, non-forwarded session-bind.
 */
optional<string> forwardedSessionBind(const string& request);

/**
 * @brief OpenSSH-style fingerprint (`SHA256:...`) of the host key in a
 * session-bind request, or nullopt when @p bind is not one.
 */
optional<string> sessionBindHostKeyFingerprint(const string& bind);

/**
 * @brief Writes @p binds on a freshly connected agent socket, ahead of any
 * forwarded data, without waiting for the replies (see
 * AgentSessionBindReplies).
 * @return false when the socket could not be written.
 */
bool sendAgentSessionBinds(SocketHandler* socketHandler, int fd,
                           const string& agentSocketPath,
                           const vector<string>& binds);

/**
 * @brief Removes the agent's replies to the session binds from the start of
 * its output, so the forwarded client only sees replies to its own requests.
 *
 * The agent answers requests in order, so the first replies on the
 * connection belong to the binds. Refusals (agents without the extension) are
 * logged and tolerated like ssh does.
 */
class AgentSessionBindReplies {
 public:
  AgentSessionBindReplies(size_t bindCount, const string& agentSocketPath);

  /**
   * @brief Consumes agent output.
   * @param forward Receives the bytes that follow the last bind reply.
   * @return false when a bind reply is malformed; the stream cannot be
   * resynchronized.
   */
  bool consume(const string& data, string* forward);

 private:
  size_t bindCount_;
  size_t repliesSeen_ = 0;
  string agentSocketPath_;
  string pending_;
};

#ifndef WIN32
/**
 * @brief Agent proxy handed to the bootstrap ssh as its IdentityAgent.
 *
 * Relays every request to the real agent and records the session-bind ssh
 * sends after it has verified the server's host key.
 */
class SshAgentSessionBindRecorder {
 public:
  /**
   * @param upstreamAgentPath Agent to relay to; empty means ssh had no agent,
   * so every request is answered with SSH_AGENT_FAILURE.
   * @throws std::runtime_error when the listening socket cannot be created.
   */
  explicit SshAgentSessionBindRecorder(const string& upstreamAgentPath);
  ~SshAgentSessionBindRecorder();

  SshAgentSessionBindRecorder(const SshAgentSessionBindRecorder&) = delete;
  SshAgentSessionBindRecorder& operator=(const SshAgentSessionBindRecorder&) =
      delete;

  /** @brief Socket path to pass to ssh as IdentityAgent. */
  const string& socketPath() const { return socketPath_; }

  /**
   * @brief Forwarded form of the last session-bind recorded.
   *
   * ProxyJump/ProxyCommand helpers inherit the agent and authenticate before
   * the final hop, so the last binding belongs to the destination host.
   */
  optional<string> lastForwardedBind();

 private:
  struct Client {
    int fd;
    int upstreamFd;
    string buffer;
  };

  void run();
  bool serviceClient(Client* client);
  bool relay(Client* client, const string& request, string* reply);
  bool readFrame(int fd, string* body);
  bool readExact(int fd, char* buf, size_t count, bool* warnedSlow);
  int connectUpstream();

  string upstreamAgentPath_;
  string directory_;
  string socketPath_;
  int listenFd_ = -1;
  int stopPipe_[2] = {-1, -1};
  std::mutex mutex_;
  optional<string> lastForwardedBind_;
  std::thread worker_;
};
#endif

}  // namespace et

#endif  // __ET_SSH_AGENT_SESSION_BIND__
