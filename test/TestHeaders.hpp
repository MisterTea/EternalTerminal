#ifndef __TEST_HEADERS_HPP__
#define __TEST_HEADERS_HPP__

#include "Headers.hpp"

#undef CHECK
#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

inline void removeOrMissing(const string& path) {
  if (::remove(path.c_str()) != 0 && GetErrno() != ENOENT) {
    FATAL_FAIL(-1);
  }
}

#endif
