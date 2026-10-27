
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#ifndef CATCH_ENUM_INFO_HPP_INCLUDED
#define CATCH_ENUM_INFO_HPP_INCLUDED

#include <catch2/internal/catch_stringref.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace Catch {
    namespace Detail {

        struct EnumInfo {
            StringRef m_name;
            std::vector<std::pair<int64_t, StringRef>> m_values;

            StringRef lookup( int64_t value ) const;
        };

        EnumInfo makeEnumInfo( StringRef enumName,
                               StringRef allValueNames,
                               std::vector<int64_t> const& values );
        template <typename E>
        Detail::EnumInfo makeEnumInfo( StringRef enumName,
                                       StringRef allEnums,
                                       std::initializer_list<E> values ) {
            static_assert( sizeof( int64_t ) >= sizeof( E ),
                           "Cannot serialize enum to int64_t" );
            std::vector<int64_t> intValues;
            intValues.reserve( values.size() );
            for ( auto enumValue : values )
                intValues.push_back( static_cast<int64_t>( enumValue ) );
            return makeEnumInfo( enumName, allEnums, intValues );
        }

        std::vector<StringRef> parseEnums( StringRef enums );

    } // namespace Detail
} // namespace Catch

#endif // CATCH_ENUM_INFO_HPP_INCLUDED
