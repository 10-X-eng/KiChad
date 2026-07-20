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

#include "design_script_footprint_padstack_generator.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>


namespace
{

using JSON = nlohmann::json;


std::string millimetres( int64_t aNanometers )
{
    if( aNanometers == 0 )
        return "0";

    const bool negative = aNanometers < 0;
    const uint64_t magnitude = negative
                                       ? static_cast<uint64_t>( -( aNanometers + 1 ) ) + 1
                                       : static_cast<uint64_t>( aNanometers );
    const uint64_t whole = magnitude / 1'000'000;
    const uint64_t fraction = magnitude % 1'000'000;
    std::string result = negative ? "-" : "";
    result += std::to_string( whole );

    if( fraction != 0 )
    {
        std::string digits = std::to_string( fraction );
        result += "." + std::string( 6 - digits.size(), '0' ) + digits;

        while( result.back() == '0' )
            result.pop_back();
    }

    return result;
}


std::string decimalPpm( int64_t aPartsPerMillion )
{
    const bool negative = aPartsPerMillion < 0;
    const uint64_t magnitude = negative
                                       ? static_cast<uint64_t>( -( aPartsPerMillion + 1 ) ) + 1
                                       : static_cast<uint64_t>( aPartsPerMillion );
    const uint64_t fraction = magnitude % 1'000'000;
    std::string result = ( negative ? "-" : "" )
                         + std::to_string( magnitude / 1'000'000 );

    if( fraction != 0 )
    {
        std::string digits = std::to_string( fraction );
        result += "." + std::string( 6 - digits.size(), '0' ) + digits;

        while( result.back() == '0' )
            result.pop_back();
    }

    return result;
}


std::string degrees( int64_t aTenths )
{
    const bool negative = aTenths < 0;
    const uint64_t magnitude = negative
                                       ? static_cast<uint64_t>( -( aTenths + 1 ) ) + 1
                                       : static_cast<uint64_t>( aTenths );
    std::string result = negative ? "-" : "";
    result += std::to_string( magnitude / 10 );

    if( magnitude % 10 != 0 )
        result += "." + std::to_string( magnitude % 10 );

    return result;
}


bool point( const JSON& aPoint )
{
    return aPoint.is_object() && aPoint.contains( "xNm" )
           && aPoint["xNm"].is_number_integer() && aPoint.contains( "yNm" )
           && aPoint["yNm"].is_number_integer();
}


bool customLayerName( const std::string& aLayer )
{
    if( aLayer == "B.Cu" )
        return true;

    if( !aLayer.starts_with( "In" ) || !aLayer.ends_with( ".Cu" ) )
        return false;

    const std::string_view number( aLayer.data() + 2, aLayer.size() - 5 );
    int index = 0;
    const std::from_chars_result parsed =
            std::from_chars( number.data(), number.data() + number.size(), index );
    return parsed.ec == std::errc() && parsed.ptr == number.data() + number.size()
           && index >= 1 && index <= 30;
}


bool renderLayer( const JSON& aLayer, const std::string& aMode, std::string& aSource )
{
    static const std::set<std::string> shapes = {
        "circle", "rect", "oval", "trapezoid", "roundrect", "chamfered_rect"
    };
    static const std::map<std::string, int> zoneConnections = {
        { "none", 0 }, { "thermal", 1 }, { "solid", 2 }, { "tht_thermal", 3 }
    };

    if( !aLayer.is_object() || !aLayer.contains( "name" ) || !aLayer["name"].is_string()
        || !aLayer.contains( "shape" ) || !aLayer["shape"].is_string()
        || !shapes.contains( aLayer["shape"].get<std::string>() )
        || !aLayer.contains( "size" ) || !point( aLayer["size"] )
        || !aLayer.contains( "offset" ) || !point( aLayer["offset"] )
        || !aLayer.contains( "trapezoidDelta" ) || !point( aLayer["trapezoidDelta"] ) )
    {
        return false;
    }

    const std::string name = aLayer["name"].get<std::string>();

    if( ( aMode == "front_inner_back" && name != "inner" && name != "B.Cu" )
        || ( aMode == "custom" && !customLayerName( name ) ) )
    {
        return false;
    }

    const std::string nativeName = name == "inner" ? "Inner" : name;
    const std::string shape = aLayer["shape"].get<std::string>();
    const std::string nativeShape = shape == "chamfered_rect" ? "roundrect" : shape;
    const int64_t width = aLayer["size"]["xNm"].get<int64_t>();
    const int64_t height = aLayer["size"]["yNm"].get<int64_t>();

    if( width <= 0 || height <= 0 || ( shape == "circle" && width != height ) )
        return false;

    aSource += "\t\t\t(layer \"" + nativeName + "\"\n"
               "\t\t\t\t(shape " + nativeShape + ")\n"
               "\t\t\t\t(size " + millimetres( width ) + " " + millimetres( height ) + ")\n";

    const int64_t deltaX = aLayer["trapezoidDelta"]["xNm"].get<int64_t>();
    const int64_t deltaY = aLayer["trapezoidDelta"]["yNm"].get<int64_t>();

    if( deltaX != 0 || deltaY != 0 )
    {
        if( shape != "trapezoid" || std::abs( deltaX ) > height
            || std::abs( deltaY ) > width )
        {
            return false;
        }

        aSource += "\t\t\t\t(rect_delta " + millimetres( deltaX ) + " "
                   + millimetres( deltaY ) + ")\n";
    }

    const int64_t offsetX = aLayer["offset"]["xNm"].get<int64_t>();
    const int64_t offsetY = aLayer["offset"]["yNm"].get<int64_t>();

    if( offsetX != 0 || offsetY != 0 )
        aSource += "\t\t\t\t(offset " + millimetres( offsetX ) + " "
                   + millimetres( offsetY ) + ")\n";

    if( shape == "roundrect" || shape == "chamfered_rect" )
    {
        if( !aLayer.contains( "roundrectRadiusNm" )
            || !aLayer["roundrectRadiusNm"].is_number_integer() )
        {
            return false;
        }

        const int64_t radius = aLayer["roundrectRadiusNm"].get<int64_t>();

        if( radius < 0 || ( shape == "roundrect" && radius == 0 )
            || radius * 2 > std::min( width, height ) )
        {
            return false;
        }

        const int64_t ratio = static_cast<int64_t>(
                std::llround( static_cast<long double>( radius ) * 1'000'000.0L
                              / static_cast<long double>( std::min( width, height ) ) ) );
        aSource += "\t\t\t\t(roundrect_rratio " + decimalPpm( ratio ) + ")\n";
    }
    else if( !aLayer.contains( "roundrectRadiusNm" )
             || !aLayer["roundrectRadiusNm"].is_null() )
    {
        return false;
    }

    if( shape == "chamfered_rect" )
    {
        if( !aLayer.contains( "chamferRatioPpm" )
            || !aLayer["chamferRatioPpm"].is_number_integer()
            || aLayer["chamferRatioPpm"].get<int64_t>() <= 0
            || aLayer["chamferRatioPpm"].get<int64_t>() > 500'000
            || !aLayer.contains( "chamferCorners" ) || !aLayer["chamferCorners"].is_array()
            || aLayer["chamferCorners"].empty() )
        {
            return false;
        }

        static const std::set<std::string> allowed = {
            "top_left", "top_right", "bottom_left", "bottom_right"
        };
        std::set<std::string> unique;
        aSource += "\t\t\t\t(chamfer_ratio "
                   + decimalPpm( aLayer["chamferRatioPpm"].get<int64_t>() ) + ")\n"
                   "\t\t\t\t(chamfer";

        for( const JSON& corner : aLayer["chamferCorners"] )
        {
            if( !corner.is_string() || !allowed.contains( corner.get<std::string>() )
                || !unique.emplace( corner.get<std::string>() ).second )
            {
                return false;
            }

            aSource += " " + corner.get<std::string>();
        }

        aSource += ")\n";
    }
    else if( !aLayer.contains( "chamferRatioPpm" ) || !aLayer["chamferRatioPpm"].is_null()
             || !aLayer.contains( "chamferCorners" ) || !aLayer["chamferCorners"].empty() )
    {
        return false;
    }

    const std::pair<const char*, const char*> distances[] = {
        { "thermalSpokeWidthNm", "thermal_bridge_width" },
        { "thermalGapNm", "thermal_gap" }, { "clearanceNm", "clearance" }
    };

    for( const auto& [field, token] : distances )
    {
        if( !aLayer.contains( field ) )
            return false;

        if( aLayer[field].is_number_integer() )
            aSource += "\t\t\t\t(" + std::string( token ) + " "
                       + millimetres( aLayer[field].get<int64_t>() ) + ")\n";
        else if( !aLayer[field].is_null() )
            return false;
    }

    if( !aLayer.contains( "thermalSpokeAngleTenths" ) )
        return false;

    if( aLayer["thermalSpokeAngleTenths"].is_number_integer() )
        aSource += "\t\t\t\t(thermal_bridge_angle "
                   + degrees( aLayer["thermalSpokeAngleTenths"].get<int64_t>() ) + ")\n";
    else if( !aLayer["thermalSpokeAngleTenths"].is_null() )
        return false;

    const std::string zoneConnection = aLayer.value( "zoneConnection", "" );

    if( zoneConnection != "inherit" )
    {
        if( !zoneConnections.contains( zoneConnection ) )
            return false;

        aSource += "\t\t\t\t(zone_connect "
                   + std::to_string( zoneConnections.at( zoneConnection ) ) + ")\n";
    }

    aSource += "\t\t\t)\n";
    return true;
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_FOOTPRINT_PADSTACK_GENERATOR::Render(
        const JSON& aPadstack, std::string& aSource )
{
    if( !aPadstack.is_object() || !aPadstack.contains( "mode" )
        || !aPadstack["mode"].is_string()
        || ( aPadstack["mode"] != "front_inner_back" && aPadstack["mode"] != "custom" )
        || !aPadstack.contains( "layers" ) || !aPadstack["layers"].is_array()
        || aPadstack["layers"].empty() )
    {
        return false;
    }

    const std::string mode = aPadstack["mode"].get<std::string>();
    std::set<std::string> names;
    aSource += "\t\t(padstack\n\t\t\t(mode " + mode + ")\n";

    for( const JSON& layer : aPadstack["layers"] )
    {
        const std::string name = layer.value( "name", "" );

        if( name.empty() || !names.emplace( name ).second
            || !renderLayer( layer, mode, aSource ) )
        {
            return false;
        }
    }

    if( mode == "front_inner_back"
        && ( names.size() != 2 || !names.contains( "inner" ) || !names.contains( "B.Cu" ) ) )
    {
        return false;
    }

    aSource += "\t\t)\n";
    return true;
}

} // namespace KICHAD
