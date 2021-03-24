#include <iostream>

#include "base64.h"

#include <ctime>
#include <cstdlib>
#include <cstring>

#include <iostream>
#include <string>

#include <assert.h>

#ifndef TESTS
#define TESTS 10000
#endif

void GenerateRandomString(std::string *string, size_t size) {
  string->resize(size);

  for (size_t i = 0; i < size; ++i) {
    (*string)[i] = rand() % 256;
  }
}

void GenerateRandomAlphaNumString(std::string *string, size_t size) {
  static const char kAlphaNum[] =
      "0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz";

  string->resize(size);

  for (size_t i = 0; i < size; ++i) {
    (*string)[i] = kAlphaNum[rand() % (sizeof(kAlphaNum) - 1)];
  }
}

long GenerateRandomNumber(long max) {
  return rand() % max;
}

long GenerateRandomNumber(long min, long max) {
  return rand() % (max - min) + min;
}

bool TestBase64(const std::string &input, bool strip_padding = false) {
  static std::string encoded;
  static std::string decoded;

  if (!Base64::Encode(input, &encoded)) {
    std::cout << "Failed to encode input string" << std::endl;
    return false;
  }

  if (strip_padding) Base64::StripPadding(&encoded);

  if (!Base64::Decode(encoded, &decoded)) {
    std::cout << "Failed to decode encoded string" << std::endl;
    return false;
  }

  if (input != decoded) {
    std::cout << "Input and decoded string differs" << std::endl;
    return false;
  }

  return true;
}

bool TestCBase64(const std::string &input, bool strip_padding = false) {
  static std::string encoded;
  static std::string decoded;

  encoded.resize(Base64::EncodedLength(input));
  if (!Base64::Encode(input.c_str(), input.size(), &encoded[0], encoded.size())) {
    std::cout << "Failed to encode input string" << std::endl;
    return false;
  }

  if (strip_padding) Base64::StripPadding(&encoded);

  decoded.resize(Base64::DecodedLength(encoded));
  if (!Base64::Decode(encoded.c_str(), encoded.size(), &decoded[0], decoded.size())) {
    std::cout << "Failed to decode encoded string" << std::endl;
    return false;
  }

  if (input != decoded) {
    std::cout << "Input and decoded string differs" << std::endl;
    return false;
  }

  return true;
}

int main() {
  srand(time(NULL));

  std::string input;

  for (size_t i = 0; i < TESTS; ++i) {
    GenerateRandomAlphaNumString(&input, GenerateRandomNumber(100, 200));

    if (!TestBase64(input)) return -1;
    if (!TestCBase64(input)) return -1;
  }

  for (size_t i = 0; i < TESTS; ++i) {
    GenerateRandomString(&input, GenerateRandomNumber(100, 200));

    if (!TestBase64(input)) return -1;
    if (!TestCBase64(input)) return -1;
  }

  for (size_t i = 0; i < TESTS; ++i) {
    GenerateRandomAlphaNumString(&input, GenerateRandomNumber(100, 200));

    if (!TestBase64(input, true)) return -1;
    if (!TestCBase64(input, true)) return -1;
  }

  for (size_t i = 0; i < TESTS; ++i) {
    GenerateRandomString(&input, GenerateRandomNumber(100, 200));

    if (!TestBase64(input, true)) return -1;
    if (!TestCBase64(input, true)) return -1;
  }

  return 0;
}
