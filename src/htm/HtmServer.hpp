#ifndef __HTM_SERVER_H__
#define __HTM_SERVER_H__

#include "Headers.hpp"
#include "IpcPairServer.hpp"
#include "MultiplexerState.hpp"

namespace et {
/**
 * @brief Server side of the HTM IPC pair that drives the terminal multiplexer.
 *
 * Receives UI commands, drives the multiplexer state, and streams pane output
 * back to the HTM client via Base64-encoded packets.
 */
class HtmServer : public IpcPairServer {
 public:
  HtmServer(shared_ptr<SocketHandler> _socketHandler,
            const SocketEndpoint& endpoint);
  /** @brief Main loop that accepts connections and dispatches HTM events. */
  void run();
  /** @brief Returns the default pipe path used by the local HTM server. */
  static string getPipeName();
  /** @brief Sends the current multiplexer state after a reconnect. */
  virtual void recover();
  /** @brief Writes an informational debug string to the connected client. */
  void sendDebug(const string& msg);

 protected:
  /** @brief State keeps track of panes/tabs/splits and terminal buffers. */
  MultiplexerState state;
  /** @brief Termination flag that exits `run()` when false. */
  bool running;
};
}  // namespace et

#endif  // __HTM_SERVER_H__
