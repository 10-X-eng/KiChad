/*
 * This program source code file is part of KiChad, a Codex-integrated downstream of KiCad.
 *
 * Copyright (C) 2026 KiChad Developers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "design_script_footprint_backdrill_generator.h"

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>


namespace
{

using JSON = nlohmann::json;


std::string millimetres( int64_t aNanometers )
{
    const uint64_t magnitude = static_cast<uint64_t>( aNanometers );
    std::string result = std::to_string( magnitude / 1'000'000 );
    const uint64_t fraction = magnitude % 1'000'000;

    if( fraction != 0 )
    {
        std::string digits = std::to_string( fraction );
        result += "." + std::string( 6 - digits.size(), '0' ) + digits;

        while( result.back() == '0' )
            result.pop_back();
    }

    return result;
}


int innerLayerIndex( const std::string& aLayer )
{
    if( !aLayer.starts_with( "In" ) || !aLayer.ends_with( ".Cu" ) )
        return -1;

    const std::string_view number( aLayer.data() + 2, aLayer.size() - 5 );
    int index = 0;
    const std::from_chars_result parsed =
            std::from_chars( number.data(), number.data() + number.size(), index );

    if( parsed.ec != std::errc() || parsed.ptr != number.data() + number.size()
        || index < 1 || index > 30 )
    {
        return -1;
    }

    return index;
}


bool validOperation( const JSON& aOperation, const char* aSide )
{
    return aOperation.is_object() && aOperation.value( "side", "" ) == aSide
           && aOperation.contains( "diameterNm" )
           && aOperation["diameterNm"].is_number_integer()
           && aOperation["diameterNm"].get<int64_t>() > 0
           && aOperation["diameterNm"].get<int64_t>() <= std::numeric_limits<int32_t>::max()
           && aOperation.contains( "stopLayer" ) && aOperation["stopLayer"].is_string()
           && innerLayerIndex( aOperation["stopLayer"].get<std::string>() ) > 0;
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_FOOTPRINT_BACKDRILL_GENERATOR::Render(
        const JSON& aBackdrills, std::string& aSource )
{
    if( !aBackdrills.is_object() || !aBackdrills.contains( "top" )
        || !aBackdrills.contains( "bottom" )
        || ( aBackdrills["top"].is_null() && aBackdrills["bottom"].is_null() ) )
    {
        return false;
    }

    int topStop = -1;
    int bottomStop = -1;

    if( !aBackdrills["top"].is_null() )
    {
        if( !validOperation( aBackdrills["top"], "top" ) )
            return false;

        topStop = innerLayerIndex( aBackdrills["top"]["stopLayer"].get<std::string>() );
    }

    if( !aBackdrills["bottom"].is_null() )
    {
        if( !validOperation( aBackdrills["bottom"], "bottom" ) )
            return false;

        bottomStop = innerLayerIndex( aBackdrills["bottom"]["stopLayer"].get<std::string>() );
    }

    if( topStop > 0 && bottomStop > 0 && topStop >= bottomStop )
        return false;

    const auto render = [&]( const char* aSide, const char* aToken, const char* aStartLayer )
    {
        const JSON& operation = aBackdrills[aSide];

        if( operation.is_null() )
            return;

        aSource += "\t\t(" + std::string( aToken ) + "\n"
                   "\t\t\t(size "
                   + millimetres( operation["diameterNm"].get<int64_t>() ) + ")\n"
                   "\t\t\t(layers \"" + std::string( aStartLayer ) + "\" \""
                   + operation["stopLayer"].get<std::string>() + "\")\n"
                   "\t\t)\n";
    };

    render( "top", "backdrill", "F.Cu" );
    render( "bottom", "tertiary_drill", "B.Cu" );
    return true;
}

} // namespace KICHAD
