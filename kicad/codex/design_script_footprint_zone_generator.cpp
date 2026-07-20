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

#include "design_script_footprint_zone_generator.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>


namespace
{

using JSON = nlohmann::json;


std::string quoteText( const std::string& aText )
{
    std::string result = "\"";
    result.reserve( aText.size() + 2 );

    for( unsigned char character : aText )
    {
        switch( character )
        {
        case '\\': result += "\\\\"; break;
        case '"':  result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:   result.push_back( static_cast<char>( character ) ); break;
        }
    }

    result.push_back( '"' );
    return result;
}


std::string fixedDecimal( int64_t aValue, uint64_t aScale )
{
    const bool negative = aValue < 0;
    const uint64_t magnitude = negative
                                       ? static_cast<uint64_t>( -( aValue + 1 ) ) + 1
                                       : static_cast<uint64_t>( aValue );
    std::string result = negative ? "-" : "";
    result += std::to_string( magnitude / aScale );
    const uint64_t remainder = magnitude % aScale;

    if( remainder != 0 )
    {
        std::string fraction = std::to_string( remainder );
        const size_t digits = std::to_string( aScale ).size() - 1;
        result += "." + std::string( digits - fraction.size(), '0' ) + fraction;

        while( result.back() == '0' )
            result.pop_back();
    }

    return result;
}


std::string millimetres( int64_t aNanometers )
{
    return fixedDecimal( aNanometers, 1'000'000 );
}


std::string squareMillimetres( int64_t aSquareNanometers )
{
    return fixedDecimal( aSquareNanometers, 1'000'000'000'000ULL );
}


std::string decimalPpm( int64_t aPpm )
{
    return fixedDecimal( aPpm, 1'000'000 );
}


std::string degrees( int64_t aTenths )
{
    return fixedDecimal( aTenths, 10 );
}


bool validPoint( const JSON& aPoint )
{
    return aPoint.is_object() && aPoint.contains( "xNm" )
           && aPoint["xNm"].is_number_integer() && aPoint.contains( "yNm" )
           && aPoint["yNm"].is_number_integer();
}


long double signedArea( const JSON& aPoints )
{
    long double result = 0.0L;

    for( size_t index = 0; index < aPoints.size(); ++index )
    {
        const JSON& current = aPoints[index];
        const JSON& next = aPoints[( index + 1 ) % aPoints.size()];
        result += static_cast<long double>( current["xNm"].get<int64_t>() )
                          * static_cast<long double>( next["yNm"].get<int64_t>() )
                  - static_cast<long double>( next["xNm"].get<int64_t>() )
                          * static_cast<long double>( current["yNm"].get<int64_t>() );
    }

    return result;
}


bool renderPolygon( const JSON& aPoints, bool aHole, std::string& aSource )
{
    if( !aPoints.is_array() || aPoints.size() < 3 )
        return false;

    for( const JSON& point : aPoints )
    {
        if( !validPoint( point ) )
            return false;
    }

    std::vector<const JSON*> ordered;
    ordered.reserve( aPoints.size() );

    for( const JSON& point : aPoints )
        ordered.push_back( &point );

    const bool clockwise = signedArea( aPoints ) < 0.0L;

    if( clockwise != aHole )
        std::reverse( ordered.begin(), ordered.end() );

    aSource += "\t\t(polygon\n\t\t\t(pts";

    for( const JSON* point : ordered )
    {
        aSource += "\n\t\t\t\t(xy " + millimetres( ( *point )["xNm"].get<int64_t>() )
                   + " " + millimetres( ( *point )["yNm"].get<int64_t>() ) + ")";
    }

    aSource += "\n\t\t\t)\n\t\t)\n";
    return true;
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_FOOTPRINT_ZONE_GENERATOR::Render(
        const JSON& aZone, const std::string& aUuid, std::string& aSource )
{
    static const std::vector<std::pair<const char*, JSON::value_t>> required = {
        { "id", JSON::value_t::string }, { "purpose", JSON::value_t::string },
        { "name", JSON::value_t::string }, { "layers", JSON::value_t::array },
        { "outline", JSON::value_t::object }, { "clearanceNm", JSON::value_t::number_integer },
        { "minThicknessNm", JSON::value_t::number_integer },
        { "connection", JSON::value_t::object }, { "islands", JSON::value_t::object },
        { "fill", JSON::value_t::object }, { "priority", JSON::value_t::number_unsigned },
        { "border", JSON::value_t::object }, { "cornerSmoothing", JSON::value_t::object },
        { "locked", JSON::value_t::boolean }
    };

    if( !aZone.is_object() || aUuid.empty() )
        return false;

    for( const auto& [field, type] : required )
    {
        if( !aZone.contains( field ) || aZone[field].type() != type )
            return false;
    }

    const std::string purpose = aZone["purpose"].get<std::string>();

    if( ( purpose != "copper" && purpose != "keepout" ) || aZone["layers"].empty()
        || !aZone["outline"].contains( "outer" )
        || !aZone["outline"]["outer"].is_array()
        || !aZone["outline"].contains( "holes" )
        || !aZone["outline"]["holes"].is_array() )
    {
        return false;
    }

    const JSON& border = aZone["border"];
    const std::string borderStyle = border.value( "style", "" );
    static const std::map<std::string, std::string> borderTokens = {
        { "solid", "none" }, { "diagonal_full", "full" }, { "diagonal_edge", "edge" }
    };

    if( !borderTokens.contains( borderStyle ) || !border.contains( "pitchNm" )
        || !border["pitchNm"].is_number_integer() )
    {
        return false;
    }

    aSource += "\t(zone\n";

    if( aZone["locked"].get<bool>() )
        aSource += "\t\t(locked yes)\n";

    if( aZone["layers"].size() == 1 )
    {
        if( !aZone["layers"][0].is_string() )
            return false;

        std::string layer = aZone["layers"][0].get<std::string>();
        aSource += "\t\t(layer " + quoteText( layer == "all_copper" ? "*.Cu" : layer ) + ")\n";
    }
    else
    {
        aSource += "\t\t(layers";

        for( const JSON& layerValue : aZone["layers"] )
        {
            if( !layerValue.is_string() )
                return false;

            std::string layer = layerValue.get<std::string>();
            aSource += " " + quoteText( layer == "all_copper" ? "*.Cu" : layer );
        }

        aSource += ")\n";
    }

    aSource += "\t\t(uuid " + quoteText( aUuid ) + ")\n"
               "\t\t(name " + quoteText( aZone["name"].get<std::string>() ) + ")\n"
               "\t\t(hatch " + borderTokens.at( borderStyle ) + " "
               + millimetres( border["pitchNm"].get<int64_t>() ) + ")\n";

    if( purpose == "copper" && aZone["priority"].get<uint32_t>() > 0 )
        aSource += "\t\t(priority " + std::to_string( aZone["priority"].get<uint32_t>() ) + ")\n";

    const JSON& connection = aZone["connection"];
    const std::string connectionStyle = connection.value( "style", "" );
    static const std::map<std::string, std::string> connectionTokens = {
        { "thermal", "" }, { "pth_thermal", " thru_hole_only" },
        { "solid", " yes" }, { "none", " no" }
    };

    if( !connectionTokens.contains( connectionStyle ) )
        return false;

    aSource += "\t\t(connect_pads" + connectionTokens.at( connectionStyle ) + "\n"
               "\t\t\t(clearance "
               + millimetres( aZone["clearanceNm"].get<int64_t>() ) + ")\n"
               "\t\t)\n"
               "\t\t(min_thickness "
               + millimetres( aZone["minThicknessNm"].get<int64_t>() ) + ")\n";

    if( purpose == "keepout" )
    {
        if( !aZone.contains( "prohibit" ) || !aZone["prohibit"].is_object() )
            return false;

        const JSON& prohibit = aZone["prohibit"];
        const std::pair<const char*, const char*> fields[] = {
            { "tracks", "tracks" }, { "vias", "vias" }, { "pads", "pads" },
            { "copper", "copperpour" }, { "footprints", "footprints" }
        };
        aSource += "\t\t(keepout\n";

        for( const auto& [field, token] : fields )
        {
            if( !prohibit.contains( field ) || !prohibit[field].is_boolean() )
                return false;

            aSource += "\t\t\t(" + std::string( token ) + " "
                       + ( prohibit[field].get<bool>() ? "not_allowed" : "allowed" ) + ")\n";
        }

        aSource += "\t\t)\n\t\t(placement (enabled no))\n";
    }

    const JSON& fill = aZone["fill"];
    const JSON& islands = aZone["islands"];
    const JSON& smoothing = aZone["cornerSmoothing"];

    aSource += "\t\t(fill";

    if( purpose == "copper" && fill.value( "mode", "" ) == "hatched" )
    {
        aSource += "\n\t\t\t(mode hatch)\n"
                   "\t\t\t(hatch_thickness " + millimetres( fill["thicknessNm"].get<int64_t>() ) + ")\n"
                   "\t\t\t(hatch_gap " + millimetres( fill["gapNm"].get<int64_t>() ) + ")\n"
                   "\t\t\t(hatch_orientation " + degrees( fill["orientationTenths"].get<int64_t>() ) + ")\n";

        const std::string edgeSmoothing = fill.value( "smoothing", "none" );

        if( edgeSmoothing != "none" )
        {
            const int level = edgeSmoothing == "chamfer" ? 1 : edgeSmoothing == "fillet" ? 2 : -1;

            if( level < 0 )
                return false;

            aSource += "\t\t\t(hatch_smoothing_level " + std::to_string( level ) + ")\n"
                       "\t\t\t(hatch_smoothing_value "
                       + decimalPpm( fill["smoothingPpm"].get<int64_t>() ) + ")\n";
        }

        aSource += "\t\t\t(hatch_border_algorithm "
                   + std::string( fill.value( "borderMode", "minimum" ) == "hatch"
                                          ? "hatch_thickness" : "min_thickness" ) + ")\n"
                   "\t\t\t(hatch_min_hole_area "
                   + decimalPpm( fill["holeMinimumAreaPpm"].get<int64_t>() ) + ")\n";
    }
    else if( purpose == "copper" && fill.value( "mode", "" ) != "solid" )
    {
        return false;
    }

    aSource += "\n\t\t\t(thermal_gap "
               + millimetres( connection.value( "thermalGapNm", int64_t( 500'000 ) ) ) + ")\n"
               "\t\t\t(thermal_bridge_width "
               + millimetres( connection.value( "thermalSpokeWidthNm", int64_t( 500'000 ) ) ) + ")\n";

    if( purpose == "copper" )
    {
        const std::string smoothingStyle = smoothing.value( "style", "" );

        if( smoothingStyle != "none" )
        {
            if( ( smoothingStyle != "chamfer" && smoothingStyle != "fillet" )
                || !smoothing.contains( "radiusNm" ) || !smoothing["radiusNm"].is_number_integer() )
            {
                return false;
            }

            aSource += "\t\t\t(smoothing " + smoothingStyle + ")\n"
                       "\t\t\t(radius "
                       + millimetres( smoothing["radiusNm"].get<int64_t>() ) + ")\n";
        }

        const std::string islandMode = islands.value( "mode", "" );
        const int nativeMode = islandMode == "remove_all" ? 0
                             : islandMode == "keep_all" ? 1
                             : islandMode == "remove_below" ? 2 : -1;

        if( nativeMode < 0 )
            return false;

        aSource += "\t\t\t(island_removal_mode " + std::to_string( nativeMode ) + ")\n";

        if( nativeMode == 2 )
            aSource += "\t\t\t(island_area_min "
                       + squareMillimetres( islands["minimumAreaNm2"].get<int64_t>() ) + ")\n";
    }

    aSource += "\t\t)\n";

    if( !renderPolygon( aZone["outline"]["outer"], false, aSource ) )
        return false;

    for( const JSON& hole : aZone["outline"]["holes"] )
    {
        if( !renderPolygon( hole, true, aSource ) )
            return false;
    }

    aSource += "\t)\n";
    return true;
}

} // namespace KICHAD
