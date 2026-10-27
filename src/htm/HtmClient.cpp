#include "HtmClient.hpp"

namespace et {
HtmClient::HtmClient(shared_ptr<SocketHandler> _socketHandler,
                     const SocketEndpoint& endpoint)
    : IpcPairClient(_socketHandler, endpoint) {}
}  // namespace et
