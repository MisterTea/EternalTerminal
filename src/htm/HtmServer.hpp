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
  void requestPaneDump() { paneDumpRequested.store(true); }
  static string getPipeName();
  static string getPaneDumpPath();
#ifdef WIN32
  /** @brief Returns the per-user event used for graceful Windows restarts. */
  static string getShutdownEventName();
  /** @brief Returns the per-user event that requests a pane-text dump. */
  static string getPaneDumpEventName();
#endif
  virtual void recover();

 protected:
  void handleClientData();
  void processLine(const string& line);
  void writePaneDumpIfRequested();

  MultiplexerState state;
  ControlWriter writer;
  string lineBuf;
  bool skipLfAfterCr;
  std::atomic<bool> running;
  std::atomic<bool> paneDumpRequested;
};
}  // namespace et

#endif
