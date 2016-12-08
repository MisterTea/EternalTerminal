#include "CryptoHandler.hpp"

#define GCRYPT_FAIL( X )                                                                                              \
  {                                                                                                                   \
    int rc = ( X );                                                                                                   \
    if ( ( rc ) ) LOG ( FATAL ) << "Error: (" << rc << "): " << gcry_strsource ( rc ) << "/" << gcry_strerror ( rc ); \
  }
namespace et {

mutex cryptoMutex;
int CryptoHandlerInitialized = 0;
void initCryptoHandler ( ) {
  /* Version check should be the very first call because it
     makes sure that important subsystems are intialized. */
  if ( !gcry_check_version ( GCRYPT_VERSION ) ) {
    LOG ( FATAL ) << "gcrypt: library version mismatch";
  }

  gcry_error_t err = 0;

  /* We don't want to see any warnings, e.g. because we have not yet
     parsed program options which might be used to suppress such
     warnings. */
  err = gcry_control ( GCRYCTL_SUSPEND_SECMEM_WARN );

  /* ... If required, other initialization goes here.  Note that the
     process might still be running with increased privileges and that
     the secure memory has not been intialized.  */

  /* Allocate a pool of 16k secure memory.  This make the secure memory
     available and also drops privileges where needed.  */
  err |= gcry_control ( GCRYCTL_INIT_SECMEM, 16384, 0 );

  /* It is now okay to let Libgcrypt complain when there was/is
     a problem with the secure memory. */
  err |= gcry_control ( GCRYCTL_RESUME_SECMEM_WARN );

  /* ... If required, other initialization goes here.  */

  /* Tell Libgcrypt that initialization has completed. */
  err |= gcry_control ( GCRYCTL_INITIALIZATION_FINISHED, 0 );

  if ( err ) {
    LOG ( FATAL ) << "gcrypt: failed initialization";
  }
}

#define BLOCK_GCRY_CIPHER GCRY_CIPHER_AES256  // Pick the cipher here
#define BLOCK_GCRY_MODE GCRY_CIPHER_MODE_CBC  // Pick the cipher mode here
#define STREAMING_GCRY_MODE GCRY_CIPHER_MODE_CTR

CryptoHandler::CryptoHandler ( const string& key ) {
  lock_guard< std::mutex > guard ( cryptoMutex );
  if ( CryptoHandlerInitialized == 0 ) {
    CryptoHandlerInitialized = 1;
    initCryptoHandler ( );
  }
  GCRYPT_FAIL ( gcry_cipher_open ( &handle, BLOCK_GCRY_CIPHER, STREAMING_GCRY_MODE, 0 ) );
  if ( key.length ( ) * 8 != 256 ) {
    throw runtime_error ( "Invalid key length" );
  }
  GCRYPT_FAIL ( gcry_cipher_setkey ( handle, key.c_str ( ), 256 / 8 ) );
  int blklen = gcry_cipher_get_algo_blklen ( GCRY_CIPHER_AES256 );
  string counter ( blklen, '\0' );  // TODO: Make the counter a random string
  GCRYPT_FAIL ( gcry_cipher_setctr ( handle, &counter[ 0 ], counter.length ( ) ) )
}

CryptoHandler::~CryptoHandler ( ) {
  lock_guard< std::mutex > guard ( cryptoMutex );
  gcry_cipher_close ( handle );
}

string CryptoHandler::encrypt ( const string& buffer ) {
  lock_guard< std::mutex > guard ( cryptoMutex );
  string retval ( buffer.length ( ), '\0' );
  GCRYPT_FAIL ( gcry_cipher_encrypt ( handle, &retval[ 0 ], retval.length ( ), buffer.c_str ( ), buffer.length ( ) ) );
  return retval;
}

string CryptoHandler::decrypt ( const string& buffer ) {
  lock_guard< std::mutex > guard ( cryptoMutex );
  string retval ( buffer.length ( ), '\0' );
  GCRYPT_FAIL ( gcry_cipher_decrypt ( handle, &retval[ 0 ], retval.length ( ), buffer.c_str ( ), buffer.length ( ) ) );
  return retval;
}

void CryptoHandler::encryptInPlace ( char* buffer, int length ) {
  lock_guard< std::mutex > guard ( cryptoMutex );
  GCRYPT_FAIL ( gcry_cipher_encrypt ( handle, buffer, length, NULL, 0 ) );
}

void CryptoHandler::decryptInPlace ( char* buffer, int length ) {
  lock_guard< std::mutex > guard ( cryptoMutex );
  GCRYPT_FAIL ( gcry_cipher_decrypt ( handle, buffer, length, NULL, 0 ) );
}
}
