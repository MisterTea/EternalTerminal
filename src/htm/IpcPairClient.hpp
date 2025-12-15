#ifndef __IPC_PAIR_CLIENT_H__
#define __IPC_PAIR_CLIENT_H__

#include "Headers.hpp"
#include "IpcPairEndpoint.hpp"
#include "SocketHandler.hpp"

namespace et {
/**
 * @brief Client-specific IPC wrapper that connects to the HTM server endpoint.
 *
 * Holds the `SocketEndpoint` fd that `HtmClient` uses for STDIN/STDOUT
 * bridging.
 */
class IpcPairClient : public IpcPairEndpoint {
 public:
  /** @brief Builds the client endpoint by connecting to the router pipe. */
  IpcPairClient(shared_ptr<SocketHandler> _socketHandler,
                const SocketEndpoint& endpoint);
  virtual ~IpcPairClient() {}

 protected:
};
}  // namespace et

#endif  // __IPC_PAIR_CLIENT_H__
