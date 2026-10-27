#include "../SimpleIni.h"
#include "gtest/gtest.h"

class TestBoolean : public ::testing::Test {
protected:
  void SetUp() override;

protected:
  CSimpleIniA ini;
};

void TestBoolean::SetUp() { ini.SetUnicode(); }

// Test GetBoolValue with all recognized TRUE values
TEST_F(TestBoolean, TestGetBoolValueTrue) {
  std::string input = "[bools]\n"
                      "true1 = true\n"
                      "true2 = t\n"
                      "true3 = yes\n"
                      "true4 = y\n"
                      "true5 = 1\n"
                      "true6 = on\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_TRUE(ini.GetBoolValue("bools", "true1", false));
  ASSERT_TRUE(ini.GetBoolValue("bools", "true2", false));
  ASSERT_TRUE(ini.GetBoolValue("bools", "true3", false));
  ASSERT_TRUE(ini.GetBoolValue("bools", "true4", false));
  ASSERT_TRUE(ini.GetBoolValue("bools", "true5", false));
  ASSERT_TRUE(ini.GetBoolValue("bools", "true6", false));
}

// Test GetBoolValue with all recognized FALSE values
TEST_F(TestBoolean, TestGetBoolValueFalse) {
  std::string input = "[bools]\n"
                      "false1 = false\n"
                      "false2 = f\n"
                      "false3 = no\n"
                      "false4 = n\n"
                      "false5 = 0\n"
                      "false6 = off\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_FALSE(ini.GetBoolValue("bools", "false1", true));
  ASSERT_FALSE(ini.GetBoolValue("bools", "false2", true));
  ASSERT_FALSE(ini.GetBoolValue("bools", "false3", true));
  ASSERT_FALSE(ini.GetBoolValue("bools", "false4", true));
  ASSERT_FALSE(ini.GetBoolValue("bools", "false5", true));
  ASSERT_FALSE(ini.GetBoolValue("bools", "false6", true));
}

// Test GetBoolValue with case variations
TEST_F(TestBoolean, TestGetBoolValueCaseInsensitive) {
  std::string input = "[bools]\n"
                      "upper = TRUE\n"
                      "mixed = YeS\n"
                      "lower = false\n"
                      "caps = NO\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_TRUE(ini.GetBoolValue("bools", "upper", false));
  ASSERT_TRUE(ini.GetBoolValue("bools", "mixed", false));
  ASSERT_FALSE(ini.GetBoolValue("bools", "lower", true));
  ASSERT_FALSE(ini.GetBoolValue("bools", "caps", true));
}

// Test GetBoolValue with unrecognized values (should return default)
TEST_F(TestBoolean, TestGetBoolValueUnrecognized) {
  std::string input = "[bools]\n"
                      "invalid1 = maybe\n"
                      "invalid2 = 2\n"
                      "invalid3 = \n"
                      "invalid4 = enabled\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // Unrecognized values should return default
  ASSERT_TRUE(ini.GetBoolValue("bools", "invalid1", true));
  ASSERT_FALSE(ini.GetBoolValue("bools", "invalid1", false));

  ASSERT_TRUE(ini.GetBoolValue("bools", "invalid2", true));
  ASSERT_FALSE(ini.GetBoolValue("bools", "invalid2", false));

  ASSERT_TRUE(ini.GetBoolValue("bools", "invalid3", true));
  ASSERT_FALSE(ini.GetBoolValue("bools", "invalid3", false));

  ASSERT_TRUE(ini.GetBoolValue("bools", "invalid4", true));
  ASSERT_FALSE(ini.GetBoolValue("bools", "invalid4", false));
}

// Test GetBoolValue with missing key
TEST_F(TestBoolean, TestGetBoolValueMissing) {
  std::string input = "[bools]\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_TRUE(ini.GetBoolValue("bools", "missing", true));
  ASSERT_FALSE(ini.GetBoolValue("bools", "missing", false));

  ASSERT_TRUE(ini.GetBoolValue("missing_section", "key", true));
  ASSERT_FALSE(ini.GetBoolValue("missing_section", "key", false));
}

// Test SetBoolValue
TEST_F(TestBoolean, TestSetBoolValue) {
  SI_Error rc = ini.SetBoolValue("bools", "value1", true);
  ASSERT_EQ(rc, SI_INSERTED);

  bool result = ini.GetBoolValue("bools", "value1", false);
  ASSERT_TRUE(result);

  rc = ini.SetBoolValue("bools", "value2", false);
  ASSERT_EQ(rc, SI_INSERTED);

  result = ini.GetBoolValue("bools", "value2", true);
  ASSERT_FALSE(result);
}

// Test SetBoolValue updates existing value
TEST_F(TestBoolean, TestSetBoolValueUpdate) {
  SI_Error rc = ini.SetBoolValue("bools", "toggle", true);
  ASSERT_EQ(rc, SI_INSERTED);

  ASSERT_TRUE(ini.GetBoolValue("bools", "toggle", false));

  rc = ini.SetBoolValue("bools", "toggle", false);
  ASSERT_EQ(rc, SI_UPDATED);

  ASSERT_FALSE(ini.GetBoolValue("bools", "toggle", true));
}

// Test SetBoolValue output format
TEST_F(TestBoolean, TestSetBoolValueFormat) {
  ini.SetBoolValue("bools", "enabled", true);
  ini.SetBoolValue("bools", "disabled", false);

  std::string output;
  SI_Error rc = ini.Save(output);
  ASSERT_EQ(rc, SI_OK);

  // Should write as "true" and "false"
  EXPECT_NE(output.find("enabled = true"), std::string::npos);
  EXPECT_NE(output.find("disabled = false"), std::string::npos);
}

// Test GetBoolValue with whitespace
TEST_F(TestBoolean, TestGetBoolValueWhitespace) {
  std::string input = "[bools]\n"
                      "padded =   true   \n"
                      "tabs =\tfalse\t\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_TRUE(ini.GetBoolValue("bools", "padded", false));
  ASSERT_FALSE(ini.GetBoolValue("bools", "tabs", true));
}

// Test boolean with multikey
TEST_F(TestBoolean, TestBooleanMultikey) {
  ini.SetMultiKey(true);

  std::string input = "[bools]\n"
                      "flag = true\n"
                      "flag = false\n"
                      "flag = yes\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  bool hasMultiple = false;
  bool result = ini.GetBoolValue("bools", "flag", false, &hasMultiple);
  ASSERT_TRUE(result); // First value is true
  ASSERT_TRUE(hasMultiple);

  // Get all values
  CSimpleIniA::TNamesDepend values;
  ini.GetAllValues("bools", "flag", values);
  ASSERT_EQ(values.size(), 3);
}

// Test SetBoolValue with force replace
TEST_F(TestBoolean, TestSetBoolValueForceReplace) {
  ini.SetMultiKey(true);

  std::string input = "[bools]\n"
                      "value = true\n"
                      "value = false\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // Replace all values
  rc = ini.SetBoolValue("bools", "value", true, nullptr, true);
  ASSERT_EQ(rc, SI_UPDATED);

  // Should only have one value now
  bool hasMultiple = false;
  bool result = ini.GetBoolValue("bools", "value", false, &hasMultiple);
  ASSERT_TRUE(result);
  ASSERT_FALSE(hasMultiple);
}

// Test boolean roundtrip
TEST_F(TestBoolean, TestBooleanRoundtrip) {
  ini.SetBoolValue("test", "bool1", true);
  ini.SetBoolValue("test", "bool2", false);
  ini.SetBoolValue("test", "bool3", true);

  std::string output;
  SI_Error rc = ini.Save(output);
  ASSERT_EQ(rc, SI_OK);

  // Load it back
  CSimpleIniA ini2;
  ini2.SetUnicode();
  rc = ini2.LoadData(output);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_TRUE(ini2.GetBoolValue("test", "bool1", false));
  ASSERT_FALSE(ini2.GetBoolValue("test", "bool2", true));
  ASSERT_TRUE(ini2.GetBoolValue("test", "bool3", false));
}

// Test "of" typo for "off" (documented quirk)
TEST_F(TestBoolean, TestBoolValueOfTypo) {
  std::string input = "[bools]\n"
                      "typo = of\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // "of" is recognized as false (documented quirk)
  ASSERT_FALSE(ini.GetBoolValue("bools", "typo", true));
}
