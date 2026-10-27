
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0

/**\file
 * Test that assertions and messages are thread-safe.
 *
 * This is done by spamming assertions and messages on multiple subthreads.
 * In manual, this reliably causes segfaults if the test is linked against
 * a non-thread-safe version of Catch2.
 *
 * The CTest test definition should also verify that the final assertion
 * count is correct.
 */

#include <catch2/catch_test_macros.hpp>

#include <thread>
#include <vector>

TEST_CASE( "Failed REQUIRE in the main thread is fine", "[!shouldfail]" ) {
    std::vector<std::thread> threads;
    for ( size_t t = 0; t < 4; ++t) {
        threads.emplace_back( [t]() {
            CAPTURE(t);
            for (size_t i = 0; i < 100; ++i) {
                CAPTURE(i);
                CHECK( false );
                CHECK( true );
            }
        } );
    }

    for (auto& t : threads) {
        t.join();
    }

    REQUIRE( false );
}

TEST_CASE( "Using unscoped messages in sibling threads", "[!shouldfail]" ) {
    std::vector<std::thread> threads;
    for ( size_t t = 0; t < 4; ++t) {
        threads.emplace_back( [t]() {
            UNSCOPED_INFO("thread " << t << " start");
            for (size_t i = 0; i < 100; ++i) {
                for (size_t j = 0; j < 4; ++j) {
                    UNSCOPED_INFO("t=" << i << ", " << j);
                }
                CHECK( false );
            }
        } );
    }

    for (auto& t : threads) {
        t.join();
    }

    REQUIRE( false );
}
