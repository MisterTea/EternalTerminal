#include "../SimpleIni.h"
#include "gtest/gtest.h"

class TestDeletion : public ::testing::Test {
protected:
  void SetUp() override;

protected:
  CSimpleIniA ini;
};

void TestDeletion::SetUp() { ini.SetUnicode(); }

// Test Delete entire section
TEST_F(TestDeletion, TestDeleteSection) {
  std::string input = "[section1]\n"
                      "key1 = value1\n"
                      "key2 = value2\n"
                      "\n"
                      "[section2]\n"
                      "key3 = value3\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_TRUE(ini.SectionExists("section1"));

  bool result = ini.Delete("section1", nullptr);
  ASSERT_TRUE(result);

  ASSERT_FALSE(ini.SectionExists("section1"));
  ASSERT_TRUE(ini.SectionExists("section2"));

  // Deleting again should return false
  result = ini.Delete("section1", nullptr);
  ASSERT_FALSE(result);
}

// Test Delete single key
TEST_F(TestDeletion, TestDeleteKey) {
  std::string input = "[section]\n"
                      "key1 = value1\n"
                      "key2 = value2\n"
                      "key3 = value3\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_TRUE(ini.KeyExists("section", "key2"));

  bool result = ini.Delete("section", "key2");
  ASSERT_TRUE(result);

  ASSERT_FALSE(ini.KeyExists("section", "key2"));
  ASSERT_TRUE(ini.KeyExists("section", "key1"));
  ASSERT_TRUE(ini.KeyExists("section", "key3"));

  // Section should still exist
  ASSERT_TRUE(ini.SectionExists("section"));
  ASSERT_EQ(ini.GetSectionSize("section"), 2);
}

// Test Delete key with removeEmpty option
TEST_F(TestDeletion, TestDeleteKeyRemoveEmpty) {
  std::string input = "[section]\n"
                      "key1 = value1\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_TRUE(ini.SectionExists("section"));

  // Delete with removeEmpty = true
  bool result = ini.Delete("section", "key1", true);
  ASSERT_TRUE(result);

  // Section should be removed
  ASSERT_FALSE(ini.SectionExists("section"));
}

// Test Delete key without removeEmpty leaves empty section
TEST_F(TestDeletion, TestDeleteKeyKeepEmpty) {
  std::string input = "[section]\n"
                      "key1 = value1\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // Delete with removeEmpty = false (default)
  bool result = ini.Delete("section", "key1", false);
  ASSERT_TRUE(result);

  // Section should still exist but be empty
  ASSERT_TRUE(ini.SectionExists("section"));
  ASSERT_EQ(ini.GetSectionSize("section"), 0);
}

// Test Delete non-existent key
TEST_F(TestDeletion, TestDeleteMissing) {
  std::string input = "[section]\n"
                      "key = value\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  bool result = ini.Delete("section", "missing");
  ASSERT_FALSE(result);

  result = ini.Delete("missing_section", nullptr);
  ASSERT_FALSE(result);
}

// Test DeleteValue with multikey
TEST_F(TestDeletion, TestDeleteValueMultikey) {
  ini.SetMultiKey(true);

  std::string input = "[section]\n"
                      "key = value1\n"
                      "key = value2\n"
                      "key = value3\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  CSimpleIniA::TNamesDepend values;
  ini.GetAllValues("section", "key", values);
  ASSERT_EQ(values.size(), 3);

  // Delete specific value
  bool result = ini.DeleteValue("section", "key", "value2");
  ASSERT_TRUE(result);

  // Should have 2 values left
  values.clear();
  ini.GetAllValues("section", "key", values);
  ASSERT_EQ(values.size(), 2);

  // Verify value2 is gone
  bool found = false;
  for (auto it = values.begin(); it != values.end(); ++it) {
    if (strcmp(it->pItem, "value2") == 0) {
      found = true;
      break;
    }
  }
  ASSERT_FALSE(found);
}

// Test DeleteValue removes all when value is NULL
TEST_F(TestDeletion, TestDeleteValueNull) {
  ini.SetMultiKey(true);

  std::string input = "[section]\n"
                      "key = value1\n"
                      "key = value2\n"
                      "key = value3\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // Delete all values (NULL means all)
  bool result = ini.DeleteValue("section", "key", nullptr);
  ASSERT_TRUE(result);

  ASSERT_FALSE(ini.KeyExists("section", "key"));
}

// Test DeleteValue with removeEmpty
TEST_F(TestDeletion, TestDeleteValueRemoveEmpty) {
  ini.SetMultiKey(true);

  std::string input = "[section]\n"
                      "key = value1\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // Delete value with removeEmpty = true
  bool result = ini.DeleteValue("section", "key", "value1", true);
  ASSERT_TRUE(result);

  // Section should be removed
  ASSERT_FALSE(ini.SectionExists("section"));
}

// Test DeleteValue non-existent value
TEST_F(TestDeletion, TestDeleteValueMissing) {
  ini.SetMultiKey(true);

  std::string input = "[section]\n"
                      "key = value1\n"
                      "key = value2\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  bool result = ini.DeleteValue("section", "key", "value3");
  ASSERT_FALSE(result);

  // Values should still be there
  CSimpleIniA::TNamesDepend values;
  ini.GetAllValues("section", "key", values);
  ASSERT_EQ(values.size(), 2);
}

// Test Delete all keys one by one
TEST_F(TestDeletion, TestDeleteAllKeys) {
  std::string input = "[section]\n"
                      "key1 = value1\n"
                      "key2 = value2\n"
                      "key3 = value3\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_EQ(ini.GetSectionSize("section"), 3);

  ini.Delete("section", "key1");
  ASSERT_EQ(ini.GetSectionSize("section"), 2);

  ini.Delete("section", "key2");
  ASSERT_EQ(ini.GetSectionSize("section"), 1);

  ini.Delete("section", "key3");
  ASSERT_EQ(ini.GetSectionSize("section"), 0);

  // Section should still exist
  ASSERT_TRUE(ini.SectionExists("section"));
}

// Test deletion preserves other data
TEST_F(TestDeletion, TestDeletePreservesOthers) {
  std::string input = "[section1]\n"
                      "key1 = value1\n"
                      "key2 = value2\n"
                      "\n"
                      "[section2]\n"
                      "key3 = value3\n"
                      "key4 = value4\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ini.Delete("section1", "key1");

  // Verify section1 still has key2
  ASSERT_TRUE(ini.KeyExists("section1", "key2"));
  const char *value = ini.GetValue("section1", "key2");
  ASSERT_STREQ(value, "value2");

  // Verify section2 is untouched
  ASSERT_TRUE(ini.KeyExists("section2", "key3"));
  ASSERT_TRUE(ini.KeyExists("section2", "key4"));
  value = ini.GetValue("section2", "key3");
  ASSERT_STREQ(value, "value3");
}

// Test Delete and Save roundtrip
TEST_F(TestDeletion, TestDeleteRoundtrip) {
  std::string input = "[section1]\n"
                      "key1 = value1\n"
                      "key2 = value2\n"
                      "\n"
                      "[section2]\n"
                      "key3 = value3\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ini.Delete("section1", "key1");

  std::string output;
  rc = ini.Save(output);
  ASSERT_EQ(rc, SI_OK);

  // Load it back
  CSimpleIniA ini2;
  ini2.SetUnicode();
  rc = ini2.LoadData(output);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_FALSE(ini2.KeyExists("section1", "key1"));
  ASSERT_TRUE(ini2.KeyExists("section1", "key2"));
  ASSERT_TRUE(ini2.KeyExists("section2", "key3"));
}

// Test DeleteValue only deletes exact match
TEST_F(TestDeletion, TestDeleteValueExactMatch) {
  ini.SetMultiKey(true);

  std::string input = "[section]\n"
                      "key = value\n"
                      "key = value123\n"
                      "key = 123value\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  bool result = ini.DeleteValue("section", "key", "value");
  ASSERT_TRUE(result);

  CSimpleIniA::TNamesDepend values;
  ini.GetAllValues("section", "key", values);
  ASSERT_EQ(values.size(), 2);

  // Verify "value" is gone but others remain
  bool found_value123 = false, found_123value = false, found_value = false;
  for (auto it = values.begin(); it != values.end(); ++it) {
    if (strcmp(it->pItem, "value123") == 0)
      found_value123 = true;
    if (strcmp(it->pItem, "123value") == 0)
      found_123value = true;
    if (strcmp(it->pItem, "value") == 0)
      found_value = true;
  }
  ASSERT_TRUE(found_value123);
  ASSERT_TRUE(found_123value);
  ASSERT_FALSE(found_value);
}
