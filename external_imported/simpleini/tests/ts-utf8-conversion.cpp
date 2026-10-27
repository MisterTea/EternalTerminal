#define SI_CONVERT_GENERIC
#include "../SimpleIni.h"

#include <gtest/gtest.h>

#include <cctype>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(__APPLE__)
#define SI_TEST_UTF8_REF_ICONV 1
#include <iconv.h>
#elif defined(__has_include)
#if __has_include(<uchar.h>)
#define SI_TEST_UTF8_REF_UCHAR 1
#include <uchar.h>
#endif
#endif

namespace {

bool g_systemUtf8Ready = false;

std::string TestDataPath(const char *a_name) {
  return std::string("data/") + a_name;
}

[[noreturn]] void FailMissingTestData(const char *a_path) {
  std::fprintf(
      stderr,
      "error: test data not found at '%s'.\n"
      "Run tests via ctest from the build directory, or run the tests binary\n"
      "with its working directory set to the folder that contains data/.\n",
      a_path);
  std::exit(1);
}

void RequireTestData(const char *a_name) {
  const std::string path = TestDataPath(a_name);
  std::ifstream in(path);
  if (!in.good()) {
    FailMissingTestData(path.c_str());
  }
}

bool InitSystemUtf8Reference() {
#if defined(SI_TEST_UTF8_REF_ICONV)
  return true;
#elif defined(SI_TEST_UTF8_REF_UCHAR)
  return setlocale(LC_CTYPE, "C.UTF-8") || setlocale(LC_CTYPE, "en_US.UTF-8") ||
         setlocale(LC_CTYPE, "");
#else
  return false;
#endif
}

bool IsAssignedScalar(char32_t cp) {
  return cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
}

#if defined(SI_TEST_UTF8_REF_ICONV)

bool SystemEncode(char32_t cp, char *buf, size_t cap, size_t &outLen) {
  iconv_t cd = iconv_open("UTF-8", "UTF-32LE");
  if (cd == (iconv_t)-1) {
    return false;
  }

  unsigned char in[4] = {
      (unsigned char)(cp & 0xFF),
      (unsigned char)((cp >> 8) & 0xFF),
      (unsigned char)((cp >> 16) & 0xFF),
      (unsigned char)((cp >> 24) & 0xFF),
  };
  char *inbuf = (char *)in;
  size_t inleft = sizeof(in);
  char *outbuf = buf;
  size_t outleft = cap;

  if (iconv(cd, &inbuf, &inleft, &outbuf, &outleft) == (size_t)-1) {
    iconv_close(cd);
    return false;
  }

  iconv_close(cd);
  outLen = cap - outleft;
  return true;
}

bool SystemDecode(const char *buf, size_t len, char32_t &cp, size_t &consumed) {
  iconv_t cd = iconv_open("UTF-32LE", "UTF-8");
  if (cd == (iconv_t)-1) {
    return false;
  }

  unsigned char out[4] = {};
  char *inbuf = (char *)buf;
  size_t inleft = len;
  char *outbuf = (char *)out;
  size_t outleft = sizeof(out);

  if (iconv(cd, &inbuf, &inleft, &outbuf, &outleft) == (size_t)-1) {
    iconv_close(cd);
    return false;
  }

  iconv_close(cd);
  consumed = len - inleft;
  cp = (char32_t)out[0] | ((char32_t)out[1] << 8) | ((char32_t)out[2] << 16) |
       ((char32_t)out[3] << 24);
  return true;
}

#elif defined(SI_TEST_UTF8_REF_UCHAR)

bool SystemEncode(char32_t cp, char *buf, size_t cap, size_t &outLen) {
  mbstate_t st{};
  memset(&st, 0, sizeof(st));
  const size_t n = c32rtomb(buf, cp, &st);
  if (n == (size_t)-1) {
    return false;
  }
  outLen = n;
  (void)cap;
  return true;
}

bool SystemDecode(const char *buf, size_t len, char32_t &cp, size_t &consumed) {
  mbstate_t st{};
  memset(&st, 0, sizeof(st));
  const size_t n = mbrtoc32(&cp, buf, len, &st);
  if (n == (size_t)-1 || n == (size_t)-2 || n == (size_t)-3) {
    return false;
  }
  if (n == 0) {
    if (len == 0 || buf[0] != '\0') {
      return false;
    }
    cp = 0;
    consumed = 1;
    return true;
  }
  consumed = n;
  return true;
}

#endif

std::vector<std::string> LoadHexLines(const char *a_path) {
  std::ifstream in(a_path);
  if (!in.is_open()) {
    FailMissingTestData(a_path);
  }
  std::vector<std::string> payloads;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    if (line.size() % 2 != 0) {
      continue;
    }
    std::string bytes;
    bytes.reserve(line.size() / 2);
    bool ok = true;
    for (size_t i = 0; i < line.size(); i += 2) {
      if (!std::isxdigit((unsigned char)line[i]) ||
          !std::isxdigit((unsigned char)line[i + 1])) {
        ok = false;
        break;
      }
      char hex[3] = {line[i], line[i + 1], '\0'};
      bytes.push_back((char)std::strtoul(hex, nullptr, 16));
    }
    if (ok) {
      payloads.push_back(bytes);
    }
  }
  return payloads;
}

bool DecodeUtf8Sequence(const std::string &a_utf8,
                        std::vector<char32_t> &a_cps) {
  const char *src = a_utf8.data();
  const char *end = a_utf8.data() + a_utf8.size();
  a_cps.clear();
  while (src < end) {
    char32_t cp = 0;
    if (!SI_UTF8::Decode(src, end, cp)) {
      return false;
    }
    a_cps.push_back(cp);
  }
  return true;
}

bool ConvertFromStoreUtf8(const std::string &a_utf8) {
  SI_ConvertW<wchar_t> conv(true);
  const size_t uLen = conv.SizeFromStore(a_utf8.data(), a_utf8.size());
  if (uLen == (size_t)-1) {
    return false;
  }
  std::vector<wchar_t> out(uLen + 1, 0);
  return conv.ConvertFromStore(a_utf8.data(), a_utf8.size(), out.data(), uLen);
}

const std::vector<std::string> &RejectCorpus() {
  static const std::vector<std::string> corpus =
      LoadHexLines(TestDataPath("utf8-reject.hex").c_str());
  return corpus;
}

class SystemUtf8Environment : public ::testing::Environment {
public:
  void SetUp() override { g_systemUtf8Ready = InitSystemUtf8Reference(); }
};

const ::testing::Environment *const kSystemUtf8Environment =
    ::testing::AddGlobalTestEnvironment(new SystemUtf8Environment);

class Utf8RejectTest : public ::testing::TestWithParam<std::string> {};

TEST_P(Utf8RejectTest, Decode_Rejects) {
  std::vector<char32_t> cps;
  EXPECT_FALSE(DecodeUtf8Sequence(GetParam(), cps));
}

TEST_P(Utf8RejectTest, ConvertFromStore_Rejects) {
  EXPECT_FALSE(ConvertFromStoreUtf8(GetParam()));
}

TEST_P(Utf8RejectTest, LoadData_Rejects) {
  const std::string data =
      std::string("[s]\nkey = value\nbad = ") + GetParam() + "\n";
  CSimpleIniW ini;
  ini.SetUnicode();
  EXPECT_NE(ini.LoadData(data.c_str(), data.size()), SI_OK);
}

INSTANTIATE_TEST_SUITE_P(ExternalRejectCorpus, Utf8RejectTest,
                         ::testing::ValuesIn(RejectCorpus()));

} // namespace

TEST(Utf8Conversion, EncodeAndDecodeMatchSystemLibraryForAllAssignedScalars) {
#if !defined(SI_TEST_UTF8_REF_ICONV) && !defined(SI_TEST_UTF8_REF_UCHAR)
  GTEST_SKIP() << "No system UTF-8 reference implementation available";
#endif
  if (!g_systemUtf8Ready) {
    GTEST_SKIP() << "System UTF-8 reference unavailable";
  }

  for (char32_t cp = 0; cp <= 0x10FFFF; ++cp) {
    if (!IsAssignedScalar(cp)) {
      continue;
    }

    char sysBuf[8] = {};
    char ourBuf[8] = {};
    size_t sysLen = 0;
    ASSERT_TRUE(SystemEncode(cp, sysBuf, sizeof(sysBuf), sysLen))
        << std::hex << cp;

    const int ourLen = SI_UTF8::Encode(cp, ourBuf, sizeof(ourBuf));
    ASSERT_GE(ourLen, 0) << std::hex << cp;
    ASSERT_EQ((size_t)ourLen, sysLen) << std::hex << cp;
    ASSERT_EQ(memcmp(sysBuf, ourBuf, sysLen), 0) << std::hex << cp;

    char32_t sysCp = 0;
    char32_t ourCp = 0;
    size_t sysConsumed = 0;
    ASSERT_TRUE(SystemDecode(sysBuf, sysLen, sysCp, sysConsumed))
        << std::hex << cp;
    ASSERT_EQ(sysConsumed, (size_t)sysLen) << std::hex << cp;

    const char *ourSrc = ourBuf;
    ASSERT_TRUE(SI_UTF8::Decode(ourSrc, ourBuf + ourLen, ourCp))
        << std::hex << cp;
    ASSERT_EQ((size_t)(ourSrc - ourBuf), (size_t)ourLen) << std::hex << cp;

    ASSERT_EQ(sysCp, cp) << std::hex << cp;
    ASSERT_EQ(ourCp, cp) << std::hex << cp;

    const char *roundSrc = sysBuf;
    char32_t roundCp = 0;
    ASSERT_TRUE(SI_UTF8::Decode(roundSrc, sysBuf + sysLen, roundCp))
        << std::hex << cp;
    ASSERT_EQ(roundCp, cp) << std::hex << cp;
  }
}

TEST(Utf8Conversion, OutOfRangeScalar_EncodesReplacement) {
  char buf[4] = {};
  const int n = SI_UTF8::Encode(0x110000, buf, sizeof(buf));
  ASSERT_EQ(n, 3);
  ASSERT_EQ((unsigned char)buf[0], 0xEF);
  ASSERT_EQ((unsigned char)buf[1], 0xBF);
  ASSERT_EQ((unsigned char)buf[2], 0xBD);

  const char *src = buf;
  char32_t cp = 0;
  ASSERT_TRUE(SI_UTF8::Decode(src, buf + n, cp));
  ASSERT_EQ(cp, SI_UTF8::REPLACEMENT);
}

TEST(Utf8Conversion, SurrogateScalar_EncodesReplacement) {
  char buf[4] = {};
  const int n = SI_UTF8::Encode(0xD800, buf, sizeof(buf));
  ASSERT_EQ(n, 3);
  ASSERT_EQ((unsigned char)buf[0], 0xEF);
  ASSERT_EQ((unsigned char)buf[1], 0xBF);
  ASSERT_EQ((unsigned char)buf[2], 0xBD);

  const char *src = buf;
  char32_t cp = 0;
  ASSERT_TRUE(SI_UTF8::Decode(src, buf + n, cp));
  ASSERT_EQ(cp, SI_UTF8::REPLACEMENT);
}

class Utf8IniRoundtripTest : public ::testing::Test {};

TEST_F(Utf8IniRoundtripTest, LoadSave_PreservesUnicodeValues) {
  RequireTestData("utf8-ini-roundtrip.ini");

  // Use \x escapes so expected values are correct on MSVC without /utf-8.
  constexpr wchar_t kGreek[] = L"\x03BA\x1F79\x03C3\x03BC\x03B5";
  constexpr wchar_t kJapanese[] = L"\x30C6\x30B9\x30C8\x4E8C";
  constexpr wchar_t kTest2[] = L"\x30C6\x30B9\x30C8\x0032";
  constexpr wchar_t kInspectionSection[] = L"\x691C\x67FB";
  constexpr wchar_t kMixed[] = L"Hello \x4E16\x754C";

  CSimpleIniW ini;
  ini.SetUnicode();

  ASSERT_EQ(ini.LoadFile(TestDataPath("utf8-ini-roundtrip.ini").c_str()),
            SI_OK);

  const wchar_t *greek = ini.GetValue(L"unicode", L"greek");
  ASSERT_NE(greek, nullptr);
  EXPECT_STREQ(greek, kGreek);

  const wchar_t *japanese = ini.GetValue(L"unicode", L"japanese");
  ASSERT_NE(japanese, nullptr);
  EXPECT_STREQ(japanese, kJapanese);

  const wchar_t *mixed = ini.GetValue(L"unicode", L"mixed");
  ASSERT_NE(mixed, nullptr);
  EXPECT_STREQ(mixed, kMixed);

  const wchar_t *fromSection = ini.GetValue(kInspectionSection, L"test2");
  ASSERT_NE(fromSection, nullptr);
  EXPECT_STREQ(fromSection, kTest2);

  std::string saved;
  ASSERT_EQ(ini.Save(saved), SI_OK);

  CSimpleIniW reloaded;
  reloaded.SetUnicode();
  ASSERT_EQ(reloaded.LoadData(saved), SI_OK);

  EXPECT_STREQ(reloaded.GetValue(L"unicode", L"greek"), kGreek);
  EXPECT_STREQ(reloaded.GetValue(L"unicode", L"japanese"), kJapanese);
  EXPECT_STREQ(reloaded.GetValue(L"unicode", L"mixed"), kMixed);
  EXPECT_STREQ(reloaded.GetValue(kInspectionSection, L"test2"), kTest2);
}
