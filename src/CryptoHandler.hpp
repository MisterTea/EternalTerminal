#ifndef __ETERNAL_TCP_CRYPTO_HANDLER__
#define __ETERNAL_TCP_CRYPTO_HANDLER__

#include "Headers.hpp"

#include <gcrypt.h>

class CryptoHandler {
public:
  explicit CryptoHandler(const string& key);
  ~CryptoHandler();

  string encrypt(const string& buffer);
  string decrypt(const string& buffer);

  void encryptInPlace(string& buffer);
  void decryptInPlace(string& buffer);
protected:
  gcry_cipher_hd_t handle;
};

#endif // __ETERNAL_TCP_CRYPTO_HANDLER__
