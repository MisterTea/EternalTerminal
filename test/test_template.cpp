#include "Headers.hpp"

#include "gtest/gtest.h"

#include "CryptoHandler.hpp"

using namespace et;

TEST(A, B) { SUCCEED(); }

TEST(CryptoHandler, DoesEncryptDecrypt) {
  string key = "12345678901234567890123456789012";
  shared_ptr<CryptoHandler> encryptHandler(new CryptoHandler(key));
  shared_ptr<CryptoHandler> decryptHandler(new CryptoHandler(key));
  string message = "ET Phone Home";
  string encryptedMessage = encryptHandler->encrypt(message);
  EXPECT_NE(message, encryptedMessage);
  string decryptedMessage = decryptHandler->decrypt(encryptedMessage);
  EXPECT_EQ(message, decryptedMessage);
}

TEST(CryptoHandler, DoesEncryptDecryptInPlace) {
  string key = "12345678901234567890123456789012";
  shared_ptr<CryptoHandler> encryptHandler(new CryptoHandler(key));
  shared_ptr<CryptoHandler> decryptHandler(new CryptoHandler(key));
  string message = "ET Phone Home";
  string originalMessage = message;
  encryptHandler->encryptInPlace(&message[0], message.length());
  EXPECT_NE(originalMessage, message);
  decryptHandler->decryptInPlace(&message[0], message.length());
  EXPECT_EQ(originalMessage, message);
}

TEST(CryptoHandler, DoesEncryptDecryptStreaming) {
  string key = "12345678901234567890123456789012";
  shared_ptr<CryptoHandler> encryptHandler(new CryptoHandler(key));
  shared_ptr<CryptoHandler> decryptHandler(new CryptoHandler(key));
  string message = "ET Phone Home";
  string originalMessage = message;
  encryptHandler->encryptInPlace(&message[0], message.length());
  EXPECT_NE(originalMessage, message);
  for (int a = 0; a < message.length(); a++) {
    decryptHandler->decryptInPlace(&message[a], 1);
  }
  EXPECT_EQ(originalMessage, message);
}

TEST(CryptoHandler, DoesEncryptStreamingDecrypt) {
  string key = "12345678901234567890123456789012";
  shared_ptr<CryptoHandler> encryptHandler(new CryptoHandler(key));
  shared_ptr<CryptoHandler> decryptHandler(new CryptoHandler(key));
  string message = "ET Phone Home";
  string originalMessage = message;
  for (int a = 0; a < message.length(); a++) {
    decryptHandler->encryptInPlace(&message[a], 1);
  }
  EXPECT_NE(originalMessage, message);
  encryptHandler->decryptInPlace(&message[0], message.length());
  EXPECT_EQ(originalMessage, message);
}

int main(int argc, char **argv) {
  srand(1);
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
