
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0

/**\file
 * Checks that when we use up an unscoped message (e.g. `UNSCOPED_INFO`),
 * with an assertion, and then add another message later, it will be
 * properly reported with later failing assertion.
 *
 * This needs separate binary to avoid the main test binary's validating
 * listener, which disables the assertion fast path.
 */

#include <catch2/catch_test_macros.hpp>

TEST_CASE(
    "Delayed unscoped message clearing does not catch newly inserted messages",
    "[messages][unscoped][!shouldfail]" ) {
    UNSCOPED_INFO( "a" );
    REQUIRE( true );
    UNSCOPED_INFO( "b" );
    REQUIRE( false );
}
