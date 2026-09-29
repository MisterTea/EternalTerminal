#include "TestHeaders.hpp"

#ifdef WIN32
#include "ConsoleUtf8DecoderWindows.hpp"
#include "PlatformUtils.hpp"
#endif

using namespace et;

TEST_CASE("Console UTF-8 decoding preserves every multibyte split",
          "[ConsoleUtf8]") {
#ifdef WIN32
  const vector<pair<string, std::wstring>> examples = {
      {"ASCII", L"ASCII"},
      {"\xc2\xa2", L"\u00a2"},
      {"\xe2\x82\xac", L"\u20ac"},
      {"\xf0\x9f\x99\x82", L"\U0001f642"},
      {"A\xc2\xa2\xe2\x82\xac\xf0\x9f\x99\x82"
       "Z",
       L"A\u00a2\u20ac\U0001f642Z"}};
  for (const auto& example : examples) {
    for (size_t split = 0; split <= example.first.size(); ++split) {
      ConsoleUtf8Decoder decoder;
      std::wstring decoded = decoder.decode(example.first.data(), split);
      decoded += decoder.decode(example.first.data() + split,
                                example.first.size() - split);
      CHECK(decoded == example.second);
    }
    ConsoleUtf8Decoder decoder;
    std::wstring decoded;
    for (const char byte : example.first) {
      decoded += decoder.decode(&byte, 1);
    }
    CHECK(decoded == example.second);
  }
#else
  SKIP("Windows console UTF-16 conversion");
#endif
}

TEST_CASE("Console UTF-8 decoding keeps output streams independent",
          "[ConsoleUtf8]") {
#ifdef WIN32
  ConsoleUtf8Decoder stdoutDecoder;
  ConsoleUtf8Decoder stderrDecoder;
  CHECK(stdoutDecoder.decode("\xe2", 1).empty());
  CHECK(stderrDecoder.decode("\xf0\x9f", 2).empty());
  CHECK(stdoutDecoder.decode("\x82\xac", 2) == L"\u20ac");
  CHECK(stderrDecoder.decode("\x99\x82", 2) == L"\U0001f642");
  CHECK(stdoutDecoder.decode("OK", 2) == L"OK");
#else
  SKIP("Windows console UTF-16 conversion");
#endif
}

TEST_CASE("Console UTF-8 decoding does not retain malformed prefixes",
          "[ConsoleUtf8]") {
#ifdef WIN32
  ConsoleUtf8Decoder decoder;
  CHECK(decoder.decode("\xe2", 1).empty());
  CHECK(decoder.decode("A", 1) == L"\ufffdA");
  CHECK(decoder.decode("\x80", 1) == L"\ufffd");
  CHECK(decoder.decode("\xc0\xaf", 2) == L"\ufffd\ufffd");
  CHECK(decoder.decode("\xe0", 1).empty());
  CHECK(decoder.decode("\x80", 1) == L"\ufffd");
  CHECK(decoder.decode("OK", 2) == L"OK");
  CHECK(decoder.decode("\xe2", 1).empty());
  decoder.reset();
  CHECK(decoder.decode("OK", 2) == L"OK");
#else
  SKIP("Windows console UTF-16 conversion");
#endif
}

TEST_CASE("Redirected output preserves raw bytes across writes",
          "[ConsoleUtf8]") {
#ifdef WIN32
  struct RedirectedStderr {
    HANDLE original = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE readHandle = NULL;
    HANDLE writeHandle = NULL;
    ~RedirectedStderr() {
      SetStdHandle(STD_ERROR_HANDLE, original);
      if (writeHandle) CloseHandle(writeHandle);
      if (readHandle) CloseHandle(readHandle);
    }
  } stream;
  REQUIRE(CreatePipe(&stream.readHandle, &stream.writeHandle, NULL, 0));
  REQUIRE(SetStdHandle(STD_ERROR_HANDLE, stream.writeHandle));
  const string bytes("\0\xff\xe2\x82\xac", 5);
  for (size_t split = 0; split <= bytes.size(); ++split) {
    WriteToStdStream(STDERR_FILENO, bytes.data(), split);
    WriteToStdStream(STDERR_FILENO, bytes.data() + split, bytes.size() - split);
    string received(bytes.size(), '\0');
    size_t offset = 0;
    while (offset < received.size()) {
      DWORD count = 0;
      REQUIRE(ReadFile(stream.readHandle, received.data() + offset,
                       static_cast<DWORD>(received.size() - offset), &count,
                       NULL));
      REQUIRE(count > 0);
      offset += count;
    }
    CHECK(received == bytes);
  }
#else
  SKIP("Windows standard-handle redirection");
#endif
}
