#ifndef __HTM_SERVER_H__
#define __HTM_SERVER_H__

#include "Headers.hpp"

#include "IpcPairServer.hpp"
#include "MultiplexerState.hpp"

namespace et {
class HtmServer : public IpcPairServer {
 public:
  HtmServer();
  void run();
  static string getPipeName();
  virtual void recover();

 protected:
  MultiplexerState state;
};
}  // namespace et

#endif  // __HTM_SERVER_H__