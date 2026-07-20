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

#include "design_script_teardrop_generator.h"

#include <cstdint>
#include <string>


namespace
{

using JSON = nlohmann::json;


std::string decimalPpm( int64_t aPartsPerMillion )
{
    const uint64_t magnitude = static_cast<uint64_t>( aPartsPerMillion );
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


bool validRatio( const JSON& aTeardrop, const char* aField, int64_t aMinimum )
{
    return aTeardrop.contains( aField ) && aTeardrop[aField].is_number_integer()
           && aTeardrop[aField].get<int64_t>() >= aMinimum
           && aTeardrop[aField].get<int64_t>() <= 1'000'000;
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_TEARDROP_GENERATOR::Render( const JSON& aTeardrop,
                                               std::string& aSource )
{
    if( !aTeardrop.is_object()
        || !aTeardrop.contains( "enabled" ) || !aTeardrop["enabled"].is_boolean()
        || !validRatio( aTeardrop, "targetLengthRatioPpm", 200'000 )
        || !validRatio( aTeardrop, "targetWidthRatioPpm", 600'000 )
        || !validRatio( aTeardrop, "trackWidthLimitPpm", 0 )
        || !aTeardrop.contains( "maxLengthNm" )
        || !aTeardrop["maxLengthNm"].is_number_integer()
        || aTeardrop["maxLengthNm"].get<int64_t>() < 0
        || aTeardrop["maxLengthNm"].get<int64_t>() > 1'000'000'000LL
        || !aTeardrop.contains( "maxWidthNm" )
        || !aTeardrop["maxWidthNm"].is_number_integer()
        || aTeardrop["maxWidthNm"].get<int64_t>() < 0
        || aTeardrop["maxWidthNm"].get<int64_t>() > 1'000'000'000LL
        || !aTeardrop.contains( "edges" ) || !aTeardrop["edges"].is_string()
        || ( aTeardrop["edges"] != "straight" && aTeardrop["edges"] != "curved" )
        || !aTeardrop.contains( "allowTwoSegments" )
        || !aTeardrop["allowTwoSegments"].is_boolean()
        || !aTeardrop.contains( "preferZoneConnections" )
        || !aTeardrop["preferZoneConnections"].is_boolean() )
    {
        return false;
    }

    const auto yesNo = []( bool aValue ) { return aValue ? "yes" : "no"; };
    aSource += "\t\t(teardrops"
               " (best_length_ratio "
               + decimalPpm( aTeardrop["targetLengthRatioPpm"].get<int64_t>() ) + ")"
               " (max_length " + millimetres( aTeardrop["maxLengthNm"].get<int64_t>() ) + ")"
               " (best_width_ratio "
               + decimalPpm( aTeardrop["targetWidthRatioPpm"].get<int64_t>() ) + ")"
               " (max_width " + millimetres( aTeardrop["maxWidthNm"].get<int64_t>() ) + ")"
               " (curved_edges " + yesNo( aTeardrop["edges"] == "curved" ) + ")"
               " (filter_ratio "
               + decimalPpm( aTeardrop["trackWidthLimitPpm"].get<int64_t>() ) + ")"
               " (enabled " + yesNo( aTeardrop["enabled"].get<bool>() ) + ")"
               " (allow_two_segments "
               + yesNo( aTeardrop["allowTwoSegments"].get<bool>() ) + ")"
               " (prefer_zone_connections "
               + yesNo( aTeardrop["preferZoneConnections"].get<bool>() ) + "))\n";
    return true;
}

} // namespace KICHAD
