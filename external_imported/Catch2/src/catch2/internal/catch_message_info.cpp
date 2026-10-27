
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0

#include <catch2/internal/catch_message_info.hpp>
#include <catch2/internal/catch_thread_local.hpp>

namespace Catch {

    namespace {
        // Messages are owned by their individual threads, so the counter should
        // be thread-local as well. Alternative consideration: atomic counter,
        // so threads don't share IDs and things are easier to debug.
        static CATCH_INTERNAL_THREAD_LOCAL unsigned int messageIDCounter = 0;
    }

    MessageInfo::MessageInfo( StringRef _macroName,
                              SourceLineInfo const& _lineInfo,
                              ResultWas::OfType _type )
    :   macroName( _macroName ),
        lineInfo( _lineInfo ),
        type( _type ),
        sequence( ++messageIDCounter )
    {}

} // end namespace Catch
