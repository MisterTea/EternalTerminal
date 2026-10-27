
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0

/**\file
 * Checks that when `STATIC_CHECK` is deferred to runtime and fails, it
 * does not abort the test case.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_templated.hpp>

#if defined( CATCH_INTERNAL_CONSTEXPR_MATCHERS_ENABLED )

namespace {
    struct MatchNoneMatcher final : public Catch::Matchers::MatcherGenericBase {
    public:
        template <typename Any>
        constexpr bool match( Any&& ) const {
            return false;
        }

        std::string describe() const override {
            using namespace std::string_literals;
            return "Matches anything"s;
        }
    };

    constexpr MatchNoneMatcher MatchNone() { return MatchNoneMatcher(); }

} // namespace

#endif

TEST_CASE("Deferred static checks") {
    STATIC_CHECK(1 == 2);
    STATIC_CHECK_FALSE(1 != 2);
#if defined(CATCH_INTERNAL_CONSTEXPR_MATCHERS_ENABLED)
    STATIC_CHECK_THAT(1, MatchNone());
#endif
    // This last assertion must be executed too
    CHECK(1 == 2);
}
