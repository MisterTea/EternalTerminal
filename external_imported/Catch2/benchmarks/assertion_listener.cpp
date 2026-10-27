
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

/**
 * Event listener that listens to all assertions, forcing assertion slow path
 */
namespace {
class AssertionSlowPathListener : public Catch::EventListenerBase {
public:
    static std::string getDescription() {
        return "Validates ordering of Catch2's listener events";
    }

    AssertionSlowPathListener(Catch::IConfig const* config) :
        EventListenerBase(config) {
        m_preferences.shouldReportAllAssertions = true;
        m_preferences.shouldReportAllAssertionStarts = true;
    }
};

CATCH_REGISTER_LISTENER( AssertionSlowPathListener )
}
