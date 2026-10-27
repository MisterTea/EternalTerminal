#define SI_SUPPORT_IOSTREAMS
#include "../SimpleIni.h"
#include "gtest/gtest.h"

#include <cstring>
#include <istream>
#include <sstream>
#include <streambuf>
#include <string>

TEST(IostreamLoad, LoadsTextStream) {
  CSimpleIniA ini;
  std::istringstream input("[section]\nkey = value\n");
  ASSERT_EQ(ini.LoadData(input), SI_OK);
  ASSERT_STREQ(ini.GetValue("section", "key"), "value");
}

TEST(IostreamLoad, RejectsBinaryStreamWithNulByte) {
  CSimpleIniA ini;
  std::string data = "key = value\n";
  data[3] = '\0';
  std::istringstream input(data);
  ASSERT_EQ(ini.LoadData(input), SI_FAIL);
  ASSERT_TRUE(ini.IsEmpty());
}

class SizedStreamBuf : public std::streambuf {
public:
  explicit SizedStreamBuf(size_t a_uBytes) : m_remaining(a_uBytes) {}

protected:
  std::streamsize xsgetn(char *a_pDest, std::streamsize a_nCount) override {
    if (m_remaining == 0) {
      return 0;
    }
    const size_t nLimit = static_cast<size_t>(a_nCount);
    const size_t nChunk = m_remaining < nLimit ? m_remaining : nLimit;
    std::memset(a_pDest, 'a', nChunk);
    m_remaining -= nChunk;
    return static_cast<std::streamsize>(nChunk);
  }

  int_type underflow() override {
    if (m_remaining == 0) {
      return traits_type::eof();
    }
    return 'a';
  }

private:
  size_t m_remaining;
};

TEST(IostreamLoad, RejectsStreamLargerThanMaxFileSize) {
  CSimpleIniA ini;
  SizedStreamBuf buf(SI_MAX_FILE_SIZE + 1);
  std::istream input(&buf);
  ASSERT_EQ(ini.LoadData(input), SI_FILE);
  ASSERT_TRUE(ini.IsEmpty());
}

TEST(LoadFileRegression, LoadsFromFilePointer) {
  FILE *fp = NULL;
#if defined(_MSC_VER) && __STDC_WANT_SECURE_LIB__
  ASSERT_EQ(tmpfile_s(&fp), 0);
#else
  fp = tmpfile();
#endif
  ASSERT_NE(fp, nullptr);

  const char data[] = "[section]\nkey = value\n";
  ASSERT_EQ(fwrite(data, 1, sizeof(data) - 1, fp), sizeof(data) - 1);

  CSimpleIniA ini;
  ASSERT_EQ(ini.LoadFile(fp), SI_OK);
  fclose(fp);

  ASSERT_STREQ(ini.GetValue("section", "key"), "value");
}
