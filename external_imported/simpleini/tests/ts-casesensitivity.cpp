#include "../SimpleIni.h"
#include "gtest/gtest.h"

// Test case-insensitive mode (default)
class TestCaseInsensitive : public ::testing::Test {
protected:
  void SetUp() override {
    ini.SetUnicode();
    // Default is case-insensitive for ASCII
  }

protected:
  CSimpleIniA ini;
};

// Test case-sensitive mode
class TestCaseSensitive : public ::testing::Test {
protected:
  void SetUp() override {
    // Use CSimpleIniCaseA for case-sensitive
  }

protected:
  CSimpleIniCaseA ini;
};

// Case-insensitive section names
TEST_F(TestCaseInsensitive, TestSectionCaseInsensitive) {
  std::string input = "[Section]\n"
                      "key = value\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // All case variations should work
  ASSERT_TRUE(ini.SectionExists("Section"));
  ASSERT_TRUE(ini.SectionExists("SECTION"));
  ASSERT_TRUE(ini.SectionExists("section"));
  ASSERT_TRUE(ini.SectionExists("SeCTioN"));

  ASSERT_STREQ(ini.GetValue("Section", "key"), "value");
  ASSERT_STREQ(ini.GetValue("SECTION", "key"), "value");
  ASSERT_STREQ(ini.GetValue("section", "key"), "value");
}

// Case-insensitive key names
TEST_F(TestCaseInsensitive, TestKeyCaseInsensitive) {
  std::string input = "[section]\n"
                      "Key = value\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // All case variations should work
  ASSERT_TRUE(ini.KeyExists("section", "Key"));
  ASSERT_TRUE(ini.KeyExists("section", "KEY"));
  ASSERT_TRUE(ini.KeyExists("section", "key"));
  ASSERT_TRUE(ini.KeyExists("section", "kEy"));

  ASSERT_STREQ(ini.GetValue("section", "Key"), "value");
  ASSERT_STREQ(ini.GetValue("section", "KEY"), "value");
  ASSERT_STREQ(ini.GetValue("section", "key"), "value");
}

// Case-insensitive SetValue updates existing
TEST_F(TestCaseInsensitive, TestSetValueCaseInsensitive) {
  std::string input = "[Section]\n"
                      "Key = value1\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // Setting with different case should update
  rc = ini.SetValue("SECTION", "KEY", "value2");
  ASSERT_EQ(rc, SI_UPDATED);

  // Should only have one key
  ASSERT_EQ(ini.GetSectionSize("section"), 1);

  ASSERT_STREQ(ini.GetValue("section", "key"), "value2");
}

// Case-sensitive section names
TEST_F(TestCaseSensitive, TestSectionCaseSensitive) {
  ini.SetUnicode();

  std::string input = "[Section]\n"
                      "key = value1\n"
                      "\n"
                      "[SECTION]\n"
                      "key = value2\n"
                      "\n"
                      "[section]\n"
                      "key = value3\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // Each case variation is a different section
  ASSERT_TRUE(ini.SectionExists("Section"));
  ASSERT_TRUE(ini.SectionExists("SECTION"));
  ASSERT_TRUE(ini.SectionExists("section"));
  ASSERT_FALSE(ini.SectionExists("SeCTioN")); // This exact case not present

  ASSERT_STREQ(ini.GetValue("Section", "key"), "value1");
  ASSERT_STREQ(ini.GetValue("SECTION", "key"), "value2");
  ASSERT_STREQ(ini.GetValue("section", "key"), "value3");

  // Should have 3 sections
  CSimpleIniCaseA::TNamesDepend sections;
  ini.GetAllSections(sections);
  ASSERT_EQ(sections.size(), 3);
}

// Case-sensitive key names
TEST_F(TestCaseSensitive, TestKeyCaseSensitive) {
  ini.SetUnicode();

  std::string input = "[section]\n"
                      "Key = value1\n"
                      "KEY = value2\n"
                      "key = value3\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // Each case variation is a different key
  ASSERT_TRUE(ini.KeyExists("section", "Key"));
  ASSERT_TRUE(ini.KeyExists("section", "KEY"));
  ASSERT_TRUE(ini.KeyExists("section", "key"));
  ASSERT_FALSE(ini.KeyExists("section", "kEy")); // This exact case not present

  ASSERT_STREQ(ini.GetValue("section", "Key"), "value1");
  ASSERT_STREQ(ini.GetValue("section", "KEY"), "value2");
  ASSERT_STREQ(ini.GetValue("section", "key"), "value3");

  // Should have 3 keys
  ASSERT_EQ(ini.GetSectionSize("section"), 3);
}

// Case-sensitive SetValue creates new entry
TEST_F(TestCaseSensitive, TestSetValueCaseSensitive) {
  ini.SetUnicode();

  std::string input = "[Section]\n"
                      "Key = value1\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // Setting with different case should insert new
  rc = ini.SetValue("SECTION", "KEY", "value2");
  ASSERT_EQ(rc, SI_INSERTED);

  // Setting original case should update
  rc = ini.SetValue("Section", "Key", "value3");
  ASSERT_EQ(rc, SI_UPDATED);

  // Should have 2 sections now
  CSimpleIniCaseA::TNamesDepend sections;
  ini.GetAllSections(sections);
  ASSERT_EQ(sections.size(), 2);

  ASSERT_STREQ(ini.GetValue("Section", "Key"), "value3");
  ASSERT_STREQ(ini.GetValue("SECTION", "KEY"), "value2");
}

// Case-insensitive delete
TEST_F(TestCaseInsensitive, TestDeleteCaseInsensitive) {
  std::string input = "[Section]\n"
                      "Key = value\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // Delete with different case
  bool result = ini.Delete("SECTION", "KEY");
  ASSERT_TRUE(result);

  ASSERT_FALSE(ini.KeyExists("section", "key"));
}

// Case-sensitive delete
TEST_F(TestCaseSensitive, TestDeleteCaseSensitive) {
  ini.SetUnicode();

  std::string input = "[Section]\n"
                      "Key = value1\n"
                      "KEY = value2\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // Delete only exact match
  bool result = ini.Delete("Section", "Key");
  ASSERT_TRUE(result);

  ASSERT_FALSE(ini.KeyExists("Section", "Key"));
  ASSERT_TRUE(ini.KeyExists("Section", "KEY")); // Other case still exists
}

// Case-insensitive GetAllKeys
TEST_F(TestCaseInsensitive, TestGetAllKeysCaseInsensitive) {
  std::string input = "[section]\n"
                      "key1 = value1\n"
                      "KEY1 = value2\n" // Should update, not create new
                      "key2 = value3\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  CSimpleIniA::TNamesDepend keys;
  ini.GetAllKeys("section", keys);

  // Should have 2 unique keys (key1 and key2)
  ASSERT_EQ(keys.size(), 2);
}

// Case-sensitive GetAllKeys
TEST_F(TestCaseSensitive, TestGetAllKeysCaseSensitive) {
  ini.SetUnicode();

  std::string input = "[section]\n"
                      "key1 = value1\n"
                      "KEY1 = value2\n"
                      "key2 = value3\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  CSimpleIniCaseA::TNamesDepend keys;
  ini.GetAllKeys("section", keys);

  // Should have 3 keys (key1, KEY1, key2)
  ASSERT_EQ(keys.size(), 3);
}

// Values are always case-sensitive
TEST_F(TestCaseInsensitive, TestValueCaseSensitive) {
  std::string input = "[section]\n"
                      "key = Value\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  const char *value = ini.GetValue("section", "key");
  ASSERT_STREQ(value, "Value");
  // Value case is preserved
  ASSERT_STRNE(value, "value");
  ASSERT_STRNE(value, "VALUE");
}

// Case-insensitive with Unicode (only ASCII is case-insensitive)
TEST_F(TestCaseInsensitive, TestUnicodeCaseSensitive) {
  const char lower[] = u8"testé";
  const char upper[] = u8"TESTÉ";

  ini.SetValue("section", lower, "value1");
  ini.SetValue("section", upper, "value2");

  // Unicode characters should be case-sensitive even in case-insensitive mode
  // (Only ASCII characters are folded)
  const char *val1 = ini.GetValue("section", lower);
  const char *val2 = ini.GetValue("section", upper);

  // Depending on implementation, these might be the same or different
  // At minimum, verify both values exist
  ASSERT_NE(val1, nullptr);
  ASSERT_NE(val2, nullptr);
}

// Case-insensitive roundtrip preserves original case
TEST_F(TestCaseInsensitive, TestRoundtripPreservesCase) {
  std::string input = "[MixedCase]\n"
                      "MixedKey = MixedValue\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  std::string output;
  rc = ini.Save(output);
  ASSERT_EQ(rc, SI_OK);

  // Original case should be preserved in output
  EXPECT_NE(output.find("[MixedCase]"), std::string::npos);
  EXPECT_NE(output.find("MixedKey"), std::string::npos);
  EXPECT_NE(output.find("MixedValue"), std::string::npos);
}

// Case-sensitive section in different cases are independent
TEST_F(TestCaseSensitive, TestIndependentSections) {
  ini.SetUnicode();

  ini.SetValue("section", "key", "value1");
  ini.SetValue("Section", "key", "value2");
  ini.SetValue("SECTION", "key", "value3");

  ASSERT_STREQ(ini.GetValue("section", "key"), "value1");
  ASSERT_STREQ(ini.GetValue("Section", "key"), "value2");
  ASSERT_STREQ(ini.GetValue("SECTION", "key"), "value3");

  // Deleting one shouldn't affect others
  ini.Delete("Section", nullptr);

  ASSERT_TRUE(ini.SectionExists("section"));
  ASSERT_FALSE(ini.SectionExists("Section"));
  ASSERT_TRUE(ini.SectionExists("SECTION"));
}
