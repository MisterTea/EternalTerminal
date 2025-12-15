#ifndef __HTM_CLIENT_H__
#define __HTM_CLIENT_H__

#include "Headers.hpp"
#include "IpcPairClient.hpp"

namespace et {
/**
 * @brief IPC client that sends local stdin keystrokes to `htmd` and prints its
 * output.
 *
 * `run()` uses `select()` to multiplex between STDIN and the connected pipe fd.
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
