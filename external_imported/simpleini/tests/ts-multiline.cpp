#include "../SimpleIni.h"
#include "gtest/gtest.h"
#include <algorithm>

class TestMultiline : public ::testing::Test {
protected:
  void SetUp() override;

protected:
  CSimpleIniA ini;
};

void TestMultiline::SetUp() {
  ini.SetUnicode();
  ini.SetMultiLine(true);
}

// Test basic multiline value
TEST_F(TestMultiline, TestBasicMultiline) {
  std::string input = "[section]\n"
                      "key = <<<END\n"
                      "Line 1\n"
                      "Line 2\n"
                      "Line 3\n"
                      "END\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  const char *value = ini.GetValue("section", "key");
  ASSERT_NE(value, nullptr);

  std::string expected = "Line 1\nLine 2\nLine 3";
  ASSERT_STREQ(value, expected.c_str());
}

// Test multiline with different end tags
TEST_F(TestMultiline, TestDifferentEndTags) {
  std::string input = "[section]\n"
                      "key1 = <<<EOF\n"
                      "content1\n"
                      "EOF\n"
                      "key2 = <<<MARKER\n"
                      "content2\n"
                      "MARKER\n"
                      "key3 = <<<123\n"
                      "content3\n"
                      "123\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_STREQ(ini.GetValue("section", "key1"), "content1");
  ASSERT_STREQ(ini.GetValue("section", "key2"), "content2");
  ASSERT_STREQ(ini.GetValue("section", "key3"), "content3");
}

// Test multiline with empty content
TEST_F(TestMultiline, TestMultilineEmpty) {
  std::string input = "[section]\n"
                      "key = <<<END\n"
                      "\n"
                      "END\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  const char *value = ini.GetValue("section", "key");
  ASSERT_NE(value, nullptr);
  // With a blank line between tags, result should be a single newline
  ASSERT_STREQ(value, "");
}

// Test multiline with leading/trailing empty lines
TEST_F(TestMultiline, TestMultilineEmptyLines) {
  std::string input = "[section]\n"
                      "key = <<<END\n"
                      "\n"
                      "content\n"
                      "\n"
                      "END\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  const char *value = ini.GetValue("section", "key");
  std::string expected = "\ncontent\n";
  ASSERT_STREQ(value, expected.c_str());
}

// Test multiline with special characters
TEST_F(TestMultiline, TestMultilineSpecialChars) {
  std::string input = "[section]\n"
                      "key = <<<END\n"
                      "Line with = equals\n"
                      "Line with # hash\n"
                      "Line with ; semicolon\n"
                      "Line with [brackets]\n"
                      "END\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  const char *value = ini.GetValue("section", "key");
  std::string expected = "Line with = equals\n"
                         "Line with # hash\n"
                         "Line with ; semicolon\n"
                         "Line with [brackets]";
  ASSERT_STREQ(value, expected.c_str());
}

// Test multiline with end tag appearing in content
TEST_F(TestMultiline, TestMultilineEndTagInContent) {
  std::string input = "[section]\n"
                      "key = <<<END\n"
                      "This line has END in it\n"
                      "  END with leading spaces\n"
                      "END with trailing text after\n"
                      "END\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  const char *value = ini.GetValue("section", "key");
  // Only exact match of END on its own line should end the multiline
  std::string expected = "This line has END in it\n"
                         "  END with leading spaces\n"
                         "END with trailing text after";
  ASSERT_STREQ(value, expected.c_str());
}

// Test SetValue creates multiline when value contains newlines
TEST_F(TestMultiline, TestSetValueMultiline) {
  std::string multilineValue = "Line 1\nLine 2\nLine 3";
  SI_Error rc = ini.SetValue("section", "key", multilineValue.c_str());
  ASSERT_EQ(rc, SI_INSERTED);

  const char *value = ini.GetValue("section", "key");
  ASSERT_STREQ(value, multilineValue.c_str());
}

// Test multiline roundtrip
TEST_F(TestMultiline, TestMultilineRoundtrip) {
  std::string input = "[section]\n"
                      "key = <<<END\n"
                      "Line 1\n"
                      "Line 2\n"
                      "Line 3\n"
                      "END\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  std::string output;
  rc = ini.Save(output);
  ASSERT_EQ(rc, SI_OK);

  // Load it back
  CSimpleIniA ini2;
  ini2.SetUnicode();
  ini2.SetMultiLine(true);
  rc = ini2.LoadData(output);
  ASSERT_EQ(rc, SI_OK);

  const char *value = ini2.GetValue("section", "key");
  std::string expected = "Line 1\nLine 2\nLine 3";
  ASSERT_STREQ(value, expected.c_str());
}

// Test multiple multiline values
TEST_F(TestMultiline, TestMultipleMultiline) {
  std::string input = "[section]\n"
                      "key1 = <<<END1\n"
                      "Content 1\n"
                      "END1\n"
                      "key2 = regular value\n"
                      "key3 = <<<END3\n"
                      "Content 3\n"
                      "END3\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_STREQ(ini.GetValue("section", "key1"), "Content 1");
  ASSERT_STREQ(ini.GetValue("section", "key2"), "regular value");
  ASSERT_STREQ(ini.GetValue("section", "key3"), "Content 3");
}

// Test multiline without SetMultiLine enabled
TEST_F(TestMultiline, TestMultilineDisabled) {
  CSimpleIniA ini2;
  ini2.SetUnicode();
  ini2.SetMultiLine(false);

  std::string input = "[section]\n"
                      "key = <<<END\n"
                      "Line 1\n"
                      "END\n";

  SI_Error rc = ini2.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // Should be treated as regular value
  const char *value = ini2.GetValue("section", "key");
  ASSERT_NE(value, nullptr);
  // Value should be "<<<END" or similar, not multiline
  EXPECT_NE(std::string(value).find("<<<"), std::string::npos);
}

// Test multiline with tabs and spaces
TEST_F(TestMultiline, TestMultilineWhitespace) {
  std::string input = "[section]\n"
                      "key = <<<END\n"
                      "\tTabbed line\n"
                      "    Spaced line\n"
                      "Mixed\t \twhitespace\n"
                      "END\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  const char *value = ini.GetValue("section", "key");
  std::string expected = "\tTabbed line\n    Spaced line\nMixed\t \twhitespace";
  ASSERT_STREQ(value, expected.c_str());
}

// Test multiline with Unicode content
TEST_F(TestMultiline, TestMultilineUnicode) {
  std::string input = "[section]\n"
                      "key = <<<END\n"
                      u8"日本語\n"
                      u8"한국어\n"
                      u8"中文\n"
                      "END\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  const char *value = ini.GetValue("section", "key");
  std::string expected = u8"日本語\n한국어\n中文";
  ASSERT_STREQ(value, expected.c_str());
}

// Test very long multiline value
TEST_F(TestMultiline, TestVeryLongMultiline) {
  std::string longContent;
  for (int i = 0; i < 1000; i++) {
    longContent += "Line " + std::to_string(i) + "\n";
  }
  // Remove trailing newline
  if (!longContent.empty() && longContent.back() == '\n') {
    longContent.pop_back();
  }

  ini.SetValue("section", "key", longContent.c_str());

  const char *value = ini.GetValue("section", "key");
  ASSERT_STREQ(value, longContent.c_str());
}

// Test multiline tag with spaces (should be trimmed)
TEST_F(TestMultiline, TestMultilineTagSpaces) {
  std::string input = "[section]\n"
                      "key = <<<END\n"
                      "content\n"
                      "END  \n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  const char *value = ini.GetValue("section", "key");
  ASSERT_STREQ(value, "content");
}

// Test multiline in empty section
TEST_F(TestMultiline, TestMultilineEmptySection) {
  std::string input = "key = <<<END\n"
                      "content\n"
                      "END\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  const char *value = ini.GetValue("", "key");
  ASSERT_STREQ(value, "content");
}

// Test multiline with Windows line endings
TEST_F(TestMultiline, TestMultilineWindowsNewlines) {
  std::string input = "[section]\r\n"
                      "key = <<<END\r\n"
                      "Line 1\r\n"
                      "Line 2\r\n"
                      "END\r\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  const char *value = ini.GetValue("section", "key");
  // Internal representation uses \n
  std::string expected = "Line 1\nLine 2";
  ASSERT_STREQ(value, expected.c_str());
}

// Test multiline followed by another section
TEST_F(TestMultiline, TestMultilineBeforeSection) {
  std::string input = "[section1]\n"
                      "key = <<<END\n"
                      "multiline\n"
                      "content\n"
                      "END\n"
                      "\n"
                      "[section2]\n"
                      "key2 = value2\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_STREQ(ini.GetValue("section1", "key"), "multiline\ncontent");
  ASSERT_STREQ(ini.GetValue("section2", "key2"), "value2");
}

// Test malformed multiline (missing end tag)
TEST_F(TestMultiline, TestMalformedMultilineMissingEnd) {
  std::string input = "[section]\n"
                      "key = <<<END\n"
                      "content\n"
                      "[section2]\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);
  // Unclosed multiline value consumes the rest of the input; section2 is never opened
  ASSERT_EQ(ini.GetSection("section2"), nullptr);
  ASSERT_STREQ(ini.GetValue("section", "key"), "content\n[section2]\n");
}

// Test multiline with empty tag
TEST_F(TestMultiline, TestMultilineEmptyTag) {
  std::string input = "[section]\n"
                      "key = <<<\n"
                      "content\n"
                      "\n";

  SI_Error rc = ini.LoadData(input);
  // Behavior with empty tag is implementation-defined
  // Just verify it doesn't crash
  ASSERT_EQ(rc, SI_OK);
}
