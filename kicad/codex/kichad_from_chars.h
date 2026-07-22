/*
 * This program source code file is part of KiChad, a Codex-integrated downstream of KiCad.
 *
 * Copyright (C) 2026 KiChad Developers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef KICHAD_FROM_CHARS_H
#define KICHAD_FROM_CHARS_H

#include <charconv>
#include <type_traits>

#if defined( __APPLE__ )
#include <fast_float/fast_float.h>
#endif

namespace KICHAD
{

/**
 * std::from_chars with a floating-point fallback for Apple's libc++, which only
 * implements the integer overloads.  On every other platform this forwards to
 * std::from_chars unchanged, so the qualified Linux parse behavior is identical.
 *
 * fast_float has no long double parser, so Apple floating-point input is parsed
 * as double and widened.  long double and double are the same width on arm64
 * macOS, so no precision is lost there.
 */
template <typename T>
inline std::from_chars_result FromChars( const char* aFirst, const char* aLast, T& aValue )
{
#if defined( __APPLE__ )
    if constexpr( std::is_floating_point_v<T> )
    {
        double parsed = 0.0;
        fast_float::from_chars_result result = fast_float::from_chars( aFirst, aLast, parsed );

        if( result.ec == std::errc() )
            aValue = static_cast<T>( parsed );

        return std::from_chars_result{ result.ptr, result.ec };
    }
    else
    {
        return std::from_chars( aFirst, aLast, aValue );
    }
#else
    return std::from_chars( aFirst, aLast, aValue );
#endif
}

} // namespace KICHAD

#endif // KICHAD_FROM_CHARS_H
