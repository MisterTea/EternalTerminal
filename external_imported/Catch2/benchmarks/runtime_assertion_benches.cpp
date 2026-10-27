
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_templated.hpp>

namespace {

class MatchAllMatcher final : public Catch::Matchers::MatcherGenericBase {
public:
    template <typename Any>
    bool match( Any&& ) const {
        return true;
    }

    std::string describe() const override {
        using namespace std::string_literals;
        return "Long string that does not fit into SSO"s;
    }
};

MatchAllMatcher MatchAll() { return MatchAllMatcher(); }


TEST_CASE( "REQUIRE", "[assertions]" ) {
    for ( size_t i = 0; i < 10'000'000; ++i ) {
        REQUIRE( true );
    }
}

TEST_CASE( "REQUIRE_THAT", "[assertions][matchers]" ) {
    for ( size_t i = 0; i < 10'000'000; ++i ) {
        REQUIRE_THAT( 1, MatchAll() );
    }
}

TEST_CASE( "REQUIRE_NOTHROW", "[assertions][exceptions]" ) {
    for ( size_t i = 0; i < 10'000'000; ++i ) {
        REQUIRE_NOTHROW( []() {}() );
    }
}

TEST_CASE( "REQUIRE_THROWS", "[assertions][exceptions]" ) {
    for ( size_t i = 0; i < 10'000'000; ++i ) {
        REQUIRE_THROWS( []() { throw 1; }() );
    }
}

} // namespace
