
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0

/**\file
 * Call ~~quick_exit~~ inside a test.
 *
 * This is used to test whether Catch2 properly creates the crash guard
 * file based on provided arguments.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

TEST_CASE("quick_exit", "[quick_exit]") {
    // Return 0 as fake "successful" exit, while there should be a guard
    // file created and kept.
    std::exit(0);
    // We cannot use quick_exit because libstdc++ on older MacOS versions didn't support it yet.
    // std::quick_exit(0);
}

TEST_CASE("pass") {}
