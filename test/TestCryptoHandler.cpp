#include "Headers.hpp"

#include "gtest/gtest.h"

#include "CryptoHandler.hpp"

using namespace et;

TEST(CryptoHandler, DoesEncryptDecrypt) {
  string key = "12345678901234567890123456789012";
  shared_ptr<CryptoHandler> encryptHandler(new CryptoHandler(key, 0));
  shared_ptr<CryptoHandler> decryptHandler(new CryptoHandler(key, 0));
  string message = "ET Phone Home";
  string encryptedMessage = encryptHandler->encrypt(message);
  EXPECT_NE(message, encryptedMessage);
  string decryptedMessage = decryptHandler->decrypt(encryptedMessage);
  EXPECT_EQ(message, decryptedMessage);
}
