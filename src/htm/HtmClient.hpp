#ifndef __HTM_CLIENT_H__
#define __HTM_CLIENT_H__

#include "Headers.hpp"
#include "IpcPairClient.hpp"

namespace et {
class HtmClient : public IpcPairClient {
 public:
  HtmClient(shared_ptr<SocketHandler> _socketHandler,
            const SocketEndpoint& endpoint);
  void run();
};
}  // namespace et

#endif  // __HTM_CLIENT_H__