#include "CryptoHandler.hpp"

#define SODIUM_FAIL(X)                                         \
  {                                                            \
    int rc = (X);                                              \
    if ((rc) == -1) STFATAL << "Crypto Error: (" << rc << ")"; \
  }
namespace et {

CryptoHandler::CryptoHandler(const string& _key, unsigned char nonceMSB) {
  lock_guard<std::mutex> guard(cryptoMutex);
  if (-1 == sodium_init()) {
    STFATAL << "libsodium init failed";
  }
  if (_key.length() != crypto_secretbox_KEYBYTES) {
    STFATAL << "Invalid key length";
  }
  memcpy(key, &_key[0], _key.length());
  memset(nonce, 0, crypto_secretbox_NONCEBYTES);
  nonce[crypto_secretbox_NONCEBYTES - 1] = nonceMSB;
}

CryptoHandler::~CryptoHandler() {}

string CryptoHandler::encrypt(const string& buffer) {
  lock_guard<std::mutex> guard(cryptoMutex);
  incrementNonce();
  string retval(buffer.length() + crypto_secretbox_MACBYTES, '\0');
  SODIUM_FAIL(crypto_secretbox_easy((unsigned char*)&retval[0],
                                    (const unsigned char*)buffer.c_str(),
                                    buffer.length(), nonce, key));
  return retval;
}

string CryptoHandler::decrypt(const string& buffer) {
  lock_guard<std::mutex> guard(cryptoMutex);
  incrementNonce();
  string retval(buffer.length() - crypto_secretbox_MACBYTES, '\0');
  if (crypto_secretbox_open_easy((unsigned char*)&retval[0],
                                 (const unsigned char*)buffer.c_str(),
                                 buffer.length(), nonce, key) == -1) {
    STFATAL << "Decrypt failed.  Possible key mismatch?";
  }
  return retval;
}

void CryptoHandler::incrementNonce() {
  // Increment nonce
  for (int a = 0; a < int(crypto_secretbox_NONCEBYTES); a++) {
    nonce[a]++;
    if (nonce[a]) {
      // When nonce[a]==0, it means we rolled over to the next digit;
      break;
    }
  }
}
}  // namespace et
