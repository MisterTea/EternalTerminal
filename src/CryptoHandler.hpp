#ifndef __ETERNAL_TCP_CRYPTO_HANDLER__
#define __ETERNAL_TCP_CRYPTO_HANDLER__

#include "Headers.hpp"

class CryptoHandler {
public:
  static void init();
  static string encrypt(const string& buffer, string key);
  static string decrypt(const string& buffer, string key);

  static void encryptInPlace(string& buffer, string key);
  static void decryptInPlace(string& buffer, string key);
};

#endif // __ETERNAL_TCP_CRYPTO_HANDLER__
