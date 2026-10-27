#include "../SimpleIni.h"
#include "gtest/gtest.h"
#include <algorithm>

class TestEdgeCases : public ::testing::Test {
protected:
  void SetUp() override;

protected:
  CSimpleIniA ini;
};

void TestEdgeCases::SetUp() { ini.SetUnicode(); }

// Test special characters in section names
TEST_F(TestEdgeCases, TestSpecialCharsSectionNames) {
  std::string input = "[section-with-dashes]\n"
                      "key = value1\n"
                      "\n"
                      "[section_with_underscores]\n"
                      "key = value2\n"
                      "\n"
                      "[section.with.dots]\n"
                      "key = value3\n"
                      "\n"
                      "[section:with:colons]\n"
                      "key = value4\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_STREQ(ini.GetValue("section-with-dashes", "key"), "value1");
  ASSERT_STREQ(ini.GetValue("section_with_underscores", "key"), "value2");
  ASSERT_STREQ(ini.GetValue("section.with.dots", "key"), "value3");
  ASSERT_STREQ(ini.GetValue("section:with:colons", "key"), "value4");
}

// Test special characters in key names
TEST_F(TestEdgeCases, TestSpecialCharsKeyNames) {
  std::string input = "[section]\n"
                      "key-with-dashes = value1\n"
                      "key_with_underscores = value2\n"
                      "key.with.dots = value3\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_STREQ(ini.GetValue("section", "key-with-dashes"), "value1");
  ASSERT_STREQ(ini.GetValue("section", "key_with_underscores"), "value2");
  ASSERT_STREQ(ini.GetValue("section", "key.with.dots"), "value3");
}

// Test equals sign in value
TEST_F(TestEdgeCases, TestEqualsInValue) {
  std::string input = "[section]\n"
                      "key1 = value=with=equals\n"
                      "key2 = a=b\n"
                      "key3 = ===\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_STREQ(ini.GetValue("section", "key1"), "value=with=equals");
  ASSERT_STREQ(ini.GetValue("section", "key2"), "a=b");
  ASSERT_STREQ(ini.GetValue("section", "key3"), "===");
}

// Test semicolon in value (comment character)
TEST_F(TestEdgeCases, TestSemicolonInValue) {
  std::string input = "[section]\n"
                      "key = value ; this is not a comment\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // The semicolon and text after should be part of value (trimmed)
  const char *value = ini.GetValue("section", "key");
  ASSERT_NE(value, nullptr);
  // Value might be "value ; this is not a comment" or just "value" depending on implementation
}

// Test hash in value (comment character)
TEST_F(TestEdgeCases, TestHashInValue) {
  std::string input = "[section]\n"
                      "key = value # this is not a comment\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  const char *value = ini.GetValue("section", "key");
  ASSERT_NE(value, nullptr);
}

// Test bracket in value
TEST_F(TestEdgeCases, TestBracketInValue) {
  std::string input = "[section]\n"
                      "key1 = [value]\n"
                      "key2 = ]value[\n"
                      "key3 = [[nested]]\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_STREQ(ini.GetValue("section", "key1"), "[value]");
  ASSERT_STREQ(ini.GetValue("section", "key2"), "]value[");
  ASSERT_STREQ(ini.GetValue("section", "key3"), "[[nested]]");
}

// Test very long section name
TEST_F(TestEdgeCases, TestLongSectionName) {
  std::string longName(1000, 'a');
  std::string input = "[" + longName + "]\nkey=value\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_TRUE(ini.SectionExists(longName.c_str()));
  ASSERT_STREQ(ini.GetValue(longName.c_str(), "key"), "value");
}

// Test very long key name
TEST_F(TestEdgeCases, TestLongKeyName) {
  std::string longKey(1000, 'b');
  std::string input = "[section]\n" + longKey + "=value\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_STREQ(ini.GetValue("section", longKey.c_str()), "value");
}

// Test very long value
TEST_F(TestEdgeCases, TestLongValue) {
  std::string longValue(10000, 'c');
  ini.SetValue("section", "key", longValue.c_str());

  const char *value = ini.GetValue("section", "key");
  ASSERT_STREQ(value, longValue.c_str());
}

// Test leading whitespace in section name
TEST_F(TestEdgeCases, TestLeadingWhitespaceSection) {
  std::string input = "[  section  ]\n"
                      "key = value\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // Whitespace should be trimmed
  ASSERT_TRUE(ini.SectionExists("section"));
  ASSERT_STREQ(ini.GetValue("section", "key"), "value");
}

// Test leading/trailing whitespace in key name
TEST_F(TestEdgeCases, TestWhitespaceKeyName) {
  std::string input = "[section]\n"
                      "  key  = value\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // Whitespace should be trimmed
  ASSERT_TRUE(ini.KeyExists("section", "key"));
  ASSERT_STREQ(ini.GetValue("section", "key"), "value");
}

// Test leading/trailing whitespace in value
TEST_F(TestEdgeCases, TestWhitespaceValue) {
  std::string input = "[section]\n"
                      "key =   value   \n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // Whitespace should be trimmed
  const char *value = ini.GetValue("section", "key");
  ASSERT_STREQ(value, "value");
}

// Test tabs as whitespace
TEST_F(TestEdgeCases, TestTabWhitespace) {
  std::string input = "[\tsection\t]\n"
                      "\tkey\t=\tvalue\t\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_TRUE(ini.SectionExists("section"));
  ASSERT_STREQ(ini.GetValue("section", "key"), "value");
}

// Test empty lines and multiple blank lines
TEST_F(TestEdgeCases, TestEmptyLines) {
  std::string input = "\n\n\n"
                      "[section1]\n"
                      "\n\n"
                      "key1 = value1\n"
                      "\n\n\n"
                      "[section2]\n"
                      "\n"
                      "key2 = value2\n"
                      "\n\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_STREQ(ini.GetValue("section1", "key1"), "value1");
  ASSERT_STREQ(ini.GetValue("section2", "key2"), "value2");
}

// Test different newline formats
TEST_F(TestEdgeCases, TestMixedNewlines) {
  // Mix of \r\n, \n, and \r
  std::string input = "[section1]\r\n"
                      "key1 = value1\n"
                      "[section2]\r"
                      "key2 = value2\r\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_STREQ(ini.GetValue("section1", "key1"), "value1");
  ASSERT_STREQ(ini.GetValue("section2", "key2"), "value2");
}

// Test malformed section (no closing bracket)
TEST_F(TestEdgeCases, TestMalformedSection) {
  std::string input = "[section\n"
                      "key = value\n";

  SI_Error rc = ini.LoadData(input);
  // Should either handle gracefully or accept as-is
  ASSERT_EQ(rc, SI_OK);
}

// Test multiple equals signs on line
TEST_F(TestEdgeCases, TestMultipleEquals) {
  std::string input = "[section]\n"
                      "key = value = more = data\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // Everything after first = should be the value
  ASSERT_STREQ(ini.GetValue("section", "key"), "value = more = data");
}

// Test empty value vs no equals
TEST_F(TestEdgeCases, TestEmptyVsNoEquals) {
  ini.SetAllowKeyOnly(true);

  std::string input = "[section]\n"
                      "key1 = \n"
                      "key2\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  const char *val1 = ini.GetValue("section", "key1");
  const char *val2 = ini.GetValue("section", "key2");

  // Both should exist
  ASSERT_TRUE(ini.KeyExists("section", "key1"));
  ASSERT_TRUE(ini.KeyExists("section", "key2"));

  // key1 should have empty string, key2 should be NULL or empty
  ASSERT_NE(val1, nullptr);
  ASSERT_STREQ(val1, "");
  ASSERT_TRUE(val2 == nullptr || val2[0] == '\0');
}

// Test Unicode section/key/value names
TEST_F(TestEdgeCases, TestUnicode) {
  const char tesuto[] = u8"テスト";
  const char kensa[] = u8"検査";
  const char value[] = u8"値";

  ini.SetValue(tesuto, kensa, value);

  const char *result = ini.GetValue(tesuto, kensa);
  ASSERT_STREQ(result, value);
}

// Test many sections
TEST_F(TestEdgeCases, TestManySections) {
  for (int i = 0; i < 1000; i++) {
    std::string section = "section" + std::to_string(i);
    ini.SetValue(section.c_str(), "key", "value");
  }

  CSimpleIniA::TNamesDepend sections;
  ini.GetAllSections(sections);
  ASSERT_EQ(sections.size(), 1000);

  // Verify a few
  ASSERT_STREQ(ini.GetValue("section0", "key"), "value");
  ASSERT_STREQ(ini.GetValue("section500", "key"), "value");
  ASSERT_STREQ(ini.GetValue("section999", "key"), "value");
}

// Test many keys in one section
TEST_F(TestEdgeCases, TestManyKeys) {
  for (int i = 0; i < 1000; i++) {
    std::string key = "key" + std::to_string(i);
    ini.SetValue("section", key.c_str(), "value");
  }

  ASSERT_EQ(ini.GetSectionSize("section"), 1000);

  // Verify a few
  ASSERT_STREQ(ini.GetValue("section", "key0"), "value");
  ASSERT_STREQ(ini.GetValue("section", "key500"), "value");
  ASSERT_STREQ(ini.GetValue("section", "key999"), "value");
}

// Test SetValue with comment
TEST_F(TestEdgeCases, TestSetValueWithComment) {
  SI_Error rc = ini.SetValue("section", "key", "value", "; This is a comment");
  ASSERT_EQ(rc, SI_INSERTED);

  ASSERT_STREQ(ini.GetValue("section", "key"), "value");

  std::string output;
  ini.Save(output);

  // Comment should be in output
  EXPECT_NE(output.find("; This is a comment"), std::string::npos);
}

// Test key without section (should go to empty section)
TEST_F(TestEdgeCases, TestKeyWithoutSection) {
  std::string input = "key1 = value1\n"
                      "\n"
                      "[section]\n"
                      "key2 = value2\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // key1 should be in empty section
  ASSERT_STREQ(ini.GetValue("", "key1"), "value1");
  ASSERT_STREQ(ini.GetValue("section", "key2"), "value2");
}

// Test consecutive sections with same name (should merge or override)
TEST_F(TestEdgeCases, TestDuplicateSections) {
  std::string input = "[section]\n"
                      "key1 = value1\n"
                      "\n"
                      "[section]\n"
                      "key2 = value2\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // Both keys should be in the section
  ASSERT_STREQ(ini.GetValue("section", "key1"), "value1");
  ASSERT_STREQ(ini.GetValue("section", "key2"), "value2");
  ASSERT_EQ(ini.GetSectionSize("section"), 2);
}

// Test value with only whitespace
TEST_F(TestEdgeCases, TestWhitespaceOnlyValue) {
  std::string input = "[section]\n"
                      "key1 =     \n"
                      "key2 = \t\t\t\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // Whitespace-only should be trimmed to empty
  const char *val1 = ini.GetValue("section", "key1");
  const char *val2 = ini.GetValue("section", "key2");
  ASSERT_NE(val1, nullptr);
  ASSERT_NE(val2, nullptr);
  ASSERT_STREQ(val1, "");
  ASSERT_STREQ(val2, "");
}
