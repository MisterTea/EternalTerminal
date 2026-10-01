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

}  // namespace et

#endif  // __ET_SSH_AGENT_SESSION_BIND__
