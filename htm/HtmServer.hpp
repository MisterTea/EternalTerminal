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
  void sendDebug(const string &msg);

 protected:
  MultiplexerState state;
  bool running;
};
}  // namespace et

#endif  // __HTM_SERVER_H__