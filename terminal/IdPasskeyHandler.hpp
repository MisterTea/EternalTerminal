#ifndef __ETERNAL_TCP_ID_PASSKEY_HANDLER__
#define __ETERNAL_TCP_ID_PASSKEY_HANDLER__

#include "Headers.hpp"

class IdPasskeyHandler {
 public:
  static void runServer(bool* done);
  static void send(const string& idPasskey);
};

#endif  // __ETERNAL_TCP_ID_PASSKEY_HANDLER__
