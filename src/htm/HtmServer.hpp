#ifndef __HTM_SERVER_H__
#define __HTM_SERVER_H__

#include "ControlMode.hpp"
#include "Headers.hpp"
#include "IpcPairServer.hpp"
#include "MultiplexerState.hpp"

namespace et {
/**
 * @brief Control-mode server that multiplexes panes and speaks tmux -CC.
 */
class HtmServer : public IpcPairServer {
 public:
  HtmServer(shared_ptr<SocketHandler> _socketHandler,
            const SocketEndpoint& endpoint);
  void run();
  void requestStop() { running.store(false); }
  static string getPipeName();
#ifdef WIN32
  /** @brief Returns the per-user event used for graceful Windows restarts. */
  static string getShutdownEventName();
#endif
  virtual void recover();

 protected:
  void handleClientData();
  void processLine(const string& line);

  MultiplexerState state;
  ControlWriter writer;
  string lineBuf;
  bool skipLfAfterCr;
  std::atomic<bool> running;
};
}  // namespace et

#endif
