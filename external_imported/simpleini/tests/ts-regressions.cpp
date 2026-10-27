#include "../SimpleIni.h"
#include "gtest/gtest.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#if defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>
#endif

// ---------------------------------------------------------------------------
// malloc interposer (Linux + glibc) for LoadData leak regression tests
// ---------------------------------------------------------------------------
#if defined(__linux__) && defined(__GLIBC__)

static std::atomic<bool> g_fail_small_allocs{false};
static std::atomic<size_t> g_small_alloc_threshold{256};
static std::atomic<bool> g_fail_after_n_allocs{false};
static std::atomic<int> g_alloc_budget{0};

extern "C" void *__real_malloc(size_t size);

extern "C" void *__wrap_malloc(size_t size) {
  if (g_fail_after_n_allocs.load()) {
    const int budget = g_alloc_budget.fetch_sub(1);
    if (budget <= 0) {
      return nullptr;
    }
  }
  if (g_fail_small_allocs && size < g_small_alloc_threshold.load()) {
    return nullptr;
  }
  return __real_malloc(size);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  return __wrap_malloc(size);
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  return __wrap_malloc(size);
}

static size_t CurrentHeapBytes() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  const size_t bytes = static_cast<size_t>(mallinfo().uordblks);
#pragma GCC diagnostic pop
  return bytes;
}

#endif // __linux__ && __GLIBC__

// Passthrough converter local to this TU so LoadData is not merged with
// instantiations compiled under other SI_* defines in the test binary.
struct RegressionConvertA {
  bool m_bStoreIsUtf8;

  RegressionConvertA() : m_bStoreIsUtf8(false) {}
  explicit RegressionConvertA(bool a_bStoreIsUtf8)
      : m_bStoreIsUtf8(a_bStoreIsUtf8) {}
  RegressionConvertA(const RegressionConvertA &rhs)
      : m_bStoreIsUtf8(rhs.m_bStoreIsUtf8) {}
  RegressionConvertA &operator=(const RegressionConvertA &rhs) {
    m_bStoreIsUtf8 = rhs.m_bStoreIsUtf8;
    return *this;
  }

  size_t SizeFromStore(const char *a_pInputData, size_t a_uInputDataLen) {
    (void)a_pInputData;
    return a_uInputDataLen;
  }

  bool ConvertFromStore(const char *a_pInputData, size_t a_uInputDataLen,
                        char *a_pOutputData, size_t a_uOutputDataSize) {
    if (a_uInputDataLen > a_uOutputDataSize) {
      return false;
    }
    memcpy(a_pOutputData, a_pInputData, a_uInputDataLen);
    return true;
  }
};

using RegressionIni =
    CSimpleIniTempl<char, SI_NoCase<char>, RegressionConvertA>;

// ---------------------------------------------------------------------------
// Failing converter for Set*Value regression tests (issue 5)
// ---------------------------------------------------------------------------
struct FailConvertFromStore {
  bool m_bStoreIsUtf8;

  FailConvertFromStore() : m_bStoreIsUtf8(false) {}
  explicit FailConvertFromStore(bool a_bStoreIsUtf8)
      : m_bStoreIsUtf8(a_bStoreIsUtf8) {}
  FailConvertFromStore(const FailConvertFromStore &rhs)
      : m_bStoreIsUtf8(rhs.m_bStoreIsUtf8) {}
  FailConvertFromStore &operator=(const FailConvertFromStore &rhs) {
    m_bStoreIsUtf8 = rhs.m_bStoreIsUtf8;
    return *this;
  }

  size_t SizeFromStore(const char *a_pInputData, size_t a_uInputDataLen) {
    (void)a_pInputData;
    return a_uInputDataLen;
  }

  bool ConvertFromStore(const char *, size_t, char *, size_t) { return false; }

  size_t SizeToStore(const char *a_pInputData) {
    return strlen(a_pInputData) + 1;
  }

  bool ConvertToStore(const char *a_pInputData, char *a_pOutputData,
                      size_t a_uOutputDataSize) {
    const size_t uLen = strlen(a_pInputData) + 1;
    if (uLen > a_uOutputDataSize) {
      return false;
    }
    memcpy(a_pOutputData, a_pInputData, uLen);
    return true;
  }
};

using FailingSetIni =
    CSimpleIniTempl<char, SI_NoCase<char>, FailConvertFromStore>;

// Issue 5: SetLongValue must fail when storage conversion fails.
TEST(SetValueRegression, SetLongValueFailsWhenConversionFails) {
  FailingSetIni ini;
  const SI_Error rc = ini.SetLongValue("section", "key", 42);
  ASSERT_EQ(rc, SI_FAIL);
  ASSERT_FALSE(ini.KeyExists("section", "key"));
}

// ---------------------------------------------------------------------------
// Failing converter for Save regression tests (issue 2)
// ---------------------------------------------------------------------------
struct FailOnFourthConvertToStore {
  bool m_bStoreIsUtf8;
  mutable int m_calls;

  FailOnFourthConvertToStore() : m_bStoreIsUtf8(false), m_calls(0) {}
  explicit FailOnFourthConvertToStore(bool a_bStoreIsUtf8)
      : m_bStoreIsUtf8(a_bStoreIsUtf8), m_calls(0) {}
  FailOnFourthConvertToStore(const FailOnFourthConvertToStore &rhs)
      : m_bStoreIsUtf8(rhs.m_bStoreIsUtf8), m_calls(rhs.m_calls) {}
  FailOnFourthConvertToStore &operator=(const FailOnFourthConvertToStore &rhs) {
    m_bStoreIsUtf8 = rhs.m_bStoreIsUtf8;
    m_calls = rhs.m_calls;
    return *this;
  }

  size_t SizeFromStore(const char *a_pInputData, size_t a_uInputDataLen) {
    (void)a_pInputData;
    return a_uInputDataLen;
  }

  bool ConvertFromStore(const char *a_pInputData, size_t a_uInputDataLen,
                        char *a_pOutputData, size_t a_uOutputDataSize) {
    if (a_uInputDataLen > a_uOutputDataSize) {
      return false;
    }
    memcpy(a_pOutputData, a_pInputData, a_uInputDataLen);
    return true;
  }

  size_t SizeToStore(const char *a_pInputData) {
    return strlen(a_pInputData) + 1;
  }

  bool ConvertToStore(const char *a_pInputData, char *a_pOutputData,
                      size_t a_uOutputDataSize) {
    if (++m_calls >= 4) {
      return false;
    }
    const size_t uLen = strlen(a_pInputData) + 1;
    if (uLen > a_uOutputDataSize) {
      return false;
    }
    memcpy(a_pOutputData, a_pInputData, uLen);
    return true;
  }
};

using FailingSaveIni =
    CSimpleIniTempl<char, SI_NoCase<char>, FailOnFourthConvertToStore>;

// Issue 2: Save must not corrupt multiline values when line output conversion fails.
TEST(SaveRegression, DoesNotCorruptMultilineValueWhenOutputFails) {
  FailingSaveIni ini(false, false, true);
  ini.SetValue("section", "key", "line1\nline2");

  std::string output;
  const SI_Error rc = ini.Save(output);
  ASSERT_EQ(rc, SI_FAIL);

  const char *value = ini.GetValue("section", "key");
  ASSERT_NE(value, nullptr);
  ASSERT_STREQ(value, "line1\nline2");
}

// Issue 4: Reset must restore load-order counter so new data starts from scratch.
TEST(ResetRegression, RestoresLoadOrderCounter) {
  CSimpleIniA ini;
  ini.SetValue("section", "key", "value");

  CSimpleIniA::TNamesDepend sections;
  ini.GetAllSections(sections);
  const int order_before_reset = sections.front().nOrder;

  ini.Reset();
  ini.SetValue("section", "key", "value");
  ini.GetAllSections(sections);
  EXPECT_EQ(sections.front().nOrder, order_before_reset);
}

#if defined(__linux__) && defined(__GLIBC__)
// Issue 3: Delete must release copied key comments from m_strings.
TEST(DeleteRegression, DoesNotLeakCopiedComments) {
  CSimpleIniA ini;
  const size_t heap_before = CurrentHeapBytes();

  for (int i = 0; i < 200; i++) {
    const std::string key = "key" + std::to_string(i);
    ini.SetValue("section", key.c_str(), "value", "; comment");
    ini.Delete("section", key.c_str());
  }

  const size_t heap_after = CurrentHeapBytes();
  ASSERT_LE(heap_after, heap_before + 4096);
}
#endif

// Issue 1: LoadData must not leak its parse buffer when parsing fails after
// conversion (second LoadData into an instance that already owns m_pData).
#if defined(__linux__) && defined(__GLIBC__)
TEST(LoadDataRegression, DoesNotLeakParseBufferOnAddEntryFailure) {
  RegressionIni ini;
  ASSERT_EQ(ini.LoadData("[existing]\nkey = value\n"), SI_OK);

  std::string second = "; file comment\n\n";
  for (int i = 0; i < 500; i++) {
    second += "[section" + std::to_string(i) + "]\nkey = value\n";
  }

  const size_t heap_before = CurrentHeapBytes();

  g_fail_small_allocs = true;
  const SI_Error rc = ini.LoadData(second);
  g_fail_small_allocs = false;

  ASSERT_EQ(rc, SI_NOMEM);

  const size_t heap_after = CurrentHeapBytes();
  ASSERT_LE(heap_after, heap_before + 4096);
}
#else
TEST(LoadDataRegression, DoesNotLeakParseBufferOnAddEntryFailure) {
  GTEST_SKIP() << "malloc interposer requires Linux glibc";
}
#endif

// Incremental LoadData must not leave a partial merge when AddEntry fails.
#if defined(__linux__) && defined(__GLIBC__)
TEST(LoadDataRegression, DoesNotPartiallyMergeOnAddEntryFailure) {
  RegressionIni ini;
  ASSERT_EQ(ini.LoadData("[existing]\nkey = value\n"), SI_OK);
  ASSERT_TRUE(ini.SectionExists("existing"));
  ASSERT_FALSE(ini.SectionExists("section0"));

  std::string second = "; file comment\n\n";
  for (int i = 0; i < 500; i++) {
    second += "[section" + std::to_string(i) + "]\nkey = value\n";
  }

  g_fail_after_n_allocs = true;
  g_alloc_budget = 20;
  const SI_Error rc = ini.LoadData(second);
  g_fail_after_n_allocs = false;

  ASSERT_EQ(rc, SI_NOMEM);
  ASSERT_TRUE(ini.SectionExists("existing"));
  ASSERT_FALSE(ini.SectionExists("section0"));
  ASSERT_FALSE(ini.SectionExists("section100"));
  ASSERT_STREQ(ini.GetValue("existing", "key"), "value");
}
#else
TEST(LoadDataRegression, DoesNotPartiallyMergeOnAddEntryFailure) {
  GTEST_SKIP() << "malloc interposer requires Linux glibc";
}
#endif

// SetValue must release replaced value strings stored in m_strings.
#if defined(__linux__) && defined(__GLIBC__)
TEST(SetValueRegression, DoesNotLeakReplacedValues) {
  RegressionIni ini;
  const size_t heap_before = CurrentHeapBytes();

  for (int i = 0; i < 200; ++i) {
    ini.SetValue("section", "key", "value1");
    ini.SetValue("section", "key", "value2");
  }

  const size_t heap_after = CurrentHeapBytes();
  ASSERT_LE(heap_after, heap_before + 4096);
}
#else
TEST(SetValueRegression, DoesNotLeakReplacedValues) {
  GTEST_SKIP() << "malloc interposer requires Linux glibc";
}
#endif

TEST(SetValueRegression, RejectsOversizedValue) {
  RegressionIni ini;
  std::string huge(SI_MAX_FILE_SIZE, 'x');
  const SI_Error rc = ini.SetValue("section", "key", huge.c_str());
  ASSERT_EQ(rc, SI_NOMEM);
  ASSERT_FALSE(ini.KeyExists("section", "key"));
}
