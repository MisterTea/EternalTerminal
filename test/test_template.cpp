#include "Headers.hpp"

#include "gtest/gtest.h"

#include "CryptoHandler.hpp"

TEST ( A, B ) { SUCCEED ( ); }

TEST(CryptoHandler, DoesEncryptDecrypt) {
  CryptoHandler::init();
  string key = "12345678901234567890123456789012";
  string message = "";
  for (int a=0;a<key.length();a++) {
    // The message length has to be a multiple of the key length
    message.append("ET Phone Home");
  }
  string encryptedMessage = CryptoHandler::encrypt(message, key);
  EXPECT_NE(message, encryptedMessage);
  string decryptedMessage = CryptoHandler::decrypt(encryptedMessage, key);
  EXPECT_EQ(message, decryptedMessage);
}


int main ( int argc, char **argv ) {
  srand(1);
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  testing::InitGoogleTest ( &argc, argv );
  return RUN_ALL_TESTS ( );
}
