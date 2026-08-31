#ifndef __HTM_CLIENT_H__
#define __HTM_CLIENT_H__

#include "Headers.hpp"
#include "IpcPairClient.hpp"

namespace et {
/**
 * @brief IPC client that sends local stdin keystrokes to `htmd` and prints its
 * output.
 *
 * Unix uses `select()` on STDIN and the pipe fd. Windows waits on the stdin
 * handle and the IPC socket because stdin is not a selectable socket.
 */
class HtmClient : public IpcPairClient {
 public:
  /** @brief Initialises the HTM client, binding to the provided pipe. */
  HtmClient(shared_ptr<SocketHandler> _socketHandler,
            const SocketEndpoint& endpoint);
  /** @brief Event loop that forwards data between stdin and the HTM daemon. */
  void run();
};
}  // namespace et

#endif  // __HTM_CLIENT_H__
