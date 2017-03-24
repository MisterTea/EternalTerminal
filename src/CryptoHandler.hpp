#ifndef __ETERNAL_TCP_CRYPTO_HANDLER__
#define __ETERNAL_TCP_CRYPTO_HANDLER__

#include "Headers.hpp"

#include <sodium.h>

namespace et {

class CryptoHandler {
 public:
  explicit CryptoHandler(const string& key, unsigned char nonceMSB);
  ~CryptoHandler();

  string encrypt(const string& buffer);
  string decrypt(const string& buffer);

 protected:
  void incrementNonce();
  unsigned char nonce[crypto_secretbox_NONCEBYTES];
  unsigned char key[crypto_secretbox_KEYBYTES];
 private:
  mutex cryptoMutex;
};
}

#endif  // __ETERNAL_TCP_CRYPTO_HANDLER__
