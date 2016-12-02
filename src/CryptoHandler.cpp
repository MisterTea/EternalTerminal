#include "CryptoHandler.hpp"

#define GCRYPT_FAIL(X) { int rc = (X); if((rc)) LOG(FATAL) << "Error: (" << rc << "): " << gcry_strsource(rc) << "/" << gcry_strerror(rc); }

int CryptoHandlerInitialized = 0;
void initCryptoHandler() {
  /* Version check should be the very first call because it
     makes sure that important subsystems are intialized. */
  if (!gcry_check_version (GCRYPT_VERSION))
  {
    LOG(FATAL) << "gcrypt: library version mismatch";
  }

  gcry_error_t err = 0;

  /* We don't want to see any warnings, e.g. because we have not yet
     parsed program options which might be used to suppress such
     warnings. */
  err = gcry_control (GCRYCTL_SUSPEND_SECMEM_WARN);

  /* ... If required, other initialization goes here.  Note that the
     process might still be running with increased privileges and that
     the secure memory has not been intialized.  */

  /* Allocate a pool of 16k secure memory.  This make the secure memory
     available and also drops privileges where needed.  */
  err |= gcry_control (GCRYCTL_INIT_SECMEM, 16384, 0);

  /* It is now okay to let Libgcrypt complain when there was/is
     a problem with the secure memory. */
  err |= gcry_control (GCRYCTL_RESUME_SECMEM_WARN);

  /* ... If required, other initialization goes here.  */

  /* Tell Libgcrypt that initialization has completed. */
  err |= gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);

  if (err) {
    LOG(FATAL) << "gcrypt: failed initialization";
  }
}

#define BLOCK_GCRY_CIPHER GCRY_CIPHER_AES256   // Pick the cipher here
#define BLOCK_GCRY_MODE GCRY_CIPHER_MODE_CBC // Pick the cipher mode here
#define STREAMING_GCRY_MODE GCRY_CIPHER_MODE_CTR

CryptoHandler::CryptoHandler(const string& key) {
  if (CryptoHandlerInitialized == 0) {
    initCryptoHandler();
    CryptoHandlerInitialized = 1;
  }
  GCRYPT_FAIL(gcry_cipher_open(&handle, BLOCK_GCRY_CIPHER, STREAMING_GCRY_MODE, 0));
  if (key.length()*8 != 256) {
    throw runtime_error("Invalid key length");
  }
  GCRYPT_FAIL(gcry_cipher_setkey(handle, key.c_str(), 256/8));
}

CryptoHandler::~CryptoHandler() {
  gcry_cipher_close(handle);
}

string CryptoHandler::encrypt(const string& buffer) {
  string retval(buffer.length(), '\0');
  GCRYPT_FAIL(gcry_cipher_encrypt(handle, &retval[0], retval.length(), buffer.c_str(), buffer.length()));
  return retval;
}

string CryptoHandler::decrypt(const string& buffer) {
  string retval(buffer.length(), '\0');
  GCRYPT_FAIL(gcry_cipher_decrypt(handle, &retval[0], retval.length(), buffer.c_str(), buffer.length()));
  return retval;
}

void CryptoHandler::encryptInPlace(string& buffer) {
  GCRYPT_FAIL(gcry_cipher_encrypt(handle, &buffer[0], buffer.length(), NULL, 0));
}

void CryptoHandler::decryptInPlace(string& buffer) {
  GCRYPT_FAIL(gcry_cipher_decrypt(handle, &buffer[0], buffer.length(), NULL, 0));
}
