#ifndef __ET_MUX_MASTER__
#define __ET_MUX_MASTER__

#include "MuxProtocol.hpp"
#include "PortForwardHandler.hpp"

namespace et {

struct MuxTrackedForward {
  uint32_t type = MUX_FWD_LOCAL;
  string listenHost;
  uint32_t listenPort = 0;
  string connectHost;
  uint32_t connectPort = 0;
};

/**
 * @brief OpenSSH ControlMaster listener on a Unix ControlPath socket.
 *
 * Speaks PROTOCOL.mux hello / new-session / tcpip forward / alive / terminate.
 * Optionally drives PortForwardHandler when OPEN_FWD arrives from a client.
 */
class MuxMaster {
 public:
  MuxMaster(string controlPath, ControlPersistConfig persist);
  ~MuxMaster();

  MuxMaster(const MuxMaster&) = delete;
  MuxMaster& operator=(const MuxMaster&) = delete;

  void setPortForwardHandler(shared_ptr<PortForwardHandler> handler);
  /** @brief Bind ControlPath and start the accept/serve thread. */
  void start();
  void stop();

  bool isRunning() const;
  bool isListening() const;
  string controlPath() const;
  size_t activeClientCount() const;
  size_t sessionCount() const;
  vector<MuxTrackedForward> trackedForwards() const;

  /**
   * @brief Signal that the master's own interactive client has exited.
   * Starts the ControlPersist timer when enabled.
   */
  void notifyPrimaryClientExited();

  /** @brief True once the persist window elapsed and the master is stopping. */
  bool persistExpired() const;

 private:
  void acceptLoop();
  void serveClient(int clientFd);
  bool handleHello(MuxConnection* conn, MuxBuffer* body);
  bool handleRequest(MuxConnection* conn, uint32_t type, MuxBuffer* body);

  bool replyOk(MuxConnection* conn, uint32_t requestId);
  bool replyAlive(MuxConnection* conn, uint32_t requestId);
  bool replyFailure(MuxConnection* conn, uint32_t type, uint32_t requestId,
                    const string& reason);
  bool replySessionOpened(MuxConnection* conn, uint32_t requestId,
                          uint32_t sessionId);

  bool openForward(const MuxTrackedForward& fwd, string* error);
  bool closeForward(const MuxTrackedForward& fwd);

  struct ClientSlot {
    int fd = -1;
    thread thr;
  };

  void closeListenFd();

  string path;
  ControlPersistConfig persistConfig;
  shared_ptr<PortForwardHandler> portForwardHandler;

  int listenFd = -1;
  vector<ClientSlot> clientSlots;
  atomic<bool> running{false};
  atomic<bool> acceptNew{true};
  atomic<bool> terminateRequested{false};
  atomic<bool> primaryExited{false};
  atomic<bool> persistDone{false};
  mutable recursive_mutex mutex;
  thread worker;
  size_t nextSessionId = 1;
  size_t clients = 0;
  size_t sessions = 0;
  vector<MuxTrackedForward> forwards;
  chrono::steady_clock::time_point persistDeadline{};
  bool persistArmed = false;
};

}  // namespace et

#endif  // __ET_MUX_MASTER__
