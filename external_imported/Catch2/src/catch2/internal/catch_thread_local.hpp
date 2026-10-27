
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#ifndef CATCH_THREAD_LOCAL_HPP_INCLUDED
#define CATCH_THREAD_LOCAL_HPP_INCLUDED

#include <catch2/catch_user_config.hpp>

#if defined( CATCH_CONFIG_THREAD_SAFE_ASSERTIONS )
#define CATCH_INTERNAL_THREAD_LOCAL thread_local
#else
#define CATCH_INTERNAL_THREAD_LOCAL
#endif

#endif // CATCH_THREAD_LOCAL_HPP_INCLUDED
