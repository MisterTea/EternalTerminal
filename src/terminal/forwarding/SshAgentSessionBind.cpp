#include "SshAgentSessionBind.hpp"

namespace et {
namespace {
constexpr uint8_t SSH_AGENT_SUCCESS = 6;

uint32_t getU32(const char* p) {
  return (uint32_t(uint8_t(p[0])) << 24) | (uint32_t(uint8_t(p[1])) << 16) |
         (uint32_t(uint8_t(p[2])) << 8) | uint32_t(uint8_t(p[3]));
}

string frame(const string& body) {
  uint32_t length = body.size();
  string framed = {char(length >> 24), char(length >> 16), char(length >> 8),
                   char(length)};
  framed += body;
  return framed;
}

}  // namespace

bool sendAgentSessionBinds(SocketHandler* socketHandler, int fd,
                           const string& agentSocketPath,
                           const vector<string>& binds) {
  string requests;
  for (const auto& bind : binds) {
    requests += frame(bind);
  }
  if (socketHandler->writeAllOrReturn(fd, requests.data(), requests.size()) !=
      int(requests.size())) {
    LOG(WARNING) << "Could not send " << binds.size()
                 << " session binding(s) to ssh agent " << agentSocketPath
                 << ": " << strerror(GetErrno());
    return false;
  }
  return true;
}

AgentSessionBindReplies::AgentSessionBindReplies(size_t bindCount,
                                                 const string& agentSocketPath)
    : bindCount_(bindCount), agentSocketPath_(agentSocketPath) {}

bool AgentSessionBindReplies::consume(const string& data, string* forward) {
  if (repliesSeen_ == bindCount_) {
    *forward = data;
    return true;
  }
  pending_ += data;
  forward->clear();
  while (repliesSeen_ < bindCount_ && pending_.size() >= 4) {
    uint32_t length = getU32(pending_.data());
    if (length == 0 || length > SSH_AGENT_MAX_MESSAGE_LENGTH) {
      LOG(WARNING) << "ssh agent " << agentSocketPath_
                   << " sent an invalid reply to session binding "
                   << repliesSeen_ + 1 << "/" << bindCount_ << " (length "
                   << length << ")";
      return false;
    }
    if (pending_.size() < 4 + size_t(length)) {
      break;
    }
    repliesSeen_++;
    uint8_t type = pending_[4];
    if (type == SSH_AGENT_SUCCESS) {
      VLOG(1) << "ssh agent " << agentSocketPath_
              << " accepted session binding " << repliesSeen_ << "/"
              << bindCount_;
    } else {
      // Same as ssh: agents without the extension keep working, but
      // destination-constrained keys stay unusable on this connection.
      LOG(INFO) << "ssh agent " << agentSocketPath_
                << " refused session binding " << repliesSeen_ << "/"
                << bindCount_ << " (reply type " << int(type) << ")";
    }
    pending_.erase(0, 4 + size_t(length));
  }
  if (repliesSeen_ == bindCount_) {
    forward->swap(pending_);
    pending_.clear();
  }
  return true;
}

}  // namespace et
