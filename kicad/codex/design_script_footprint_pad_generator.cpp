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

#include "design_script_footprint_pad_generator.h"

#include "design_script_footprint_backdrill_generator.h"
#include "design_script_footprint_custom_pad_generator.h"
#include "design_script_footprint_hole_treatment_generator.h"
#include "design_script_footprint_padstack_generator.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>


namespace
{

using JSON = nlohmann::json;
using BACKDRILL_GENERATOR = KICHAD::DESIGN_SCRIPT_FOOTPRINT_BACKDRILL_GENERATOR;
using CUSTOM_GENERATOR = KICHAD::DESIGN_SCRIPT_FOOTPRINT_CUSTOM_PAD_GENERATOR;
using HOLE_TREATMENT_GENERATOR = KICHAD::DESIGN_SCRIPT_FOOTPRINT_HOLE_TREATMENT_GENERATOR;
using PADSTACK_GENERATOR = KICHAD::DESIGN_SCRIPT_FOOTPRINT_PADSTACK_GENERATOR;


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
    uint64_t magnitude = negative
                                 ? static_cast<uint64_t>( -( aPartsPerMillion + 1 ) ) + 1
                                 : static_cast<uint64_t>( aPartsPerMillion );
    std::string digits = std::to_string( magnitude % 1'000'000 );
    std::string result = ( negative ? "-" : "" )
                         + std::to_string( magnitude / 1'000'000 );

    if( magnitude % 1'000'000 != 0 )
    {
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


bool validPoint( const JSON& aPoint )
{
    return aPoint.is_object() && aPoint.contains( "xNm" )
           && aPoint["xNm"].is_number_integer() && aPoint.contains( "yNm" )
           && aPoint["yNm"].is_number_integer();
}


bool validUuid( const std::string& aUuid )
{
    return aUuid.size() == 36 && aUuid[8] == '-' && aUuid[13] == '-'
           && aUuid[18] == '-' && aUuid[23] == '-';
}


} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_FOOTPRINT_PAD_GENERATOR::Render( const JSON& aPad,
                                                    const std::string& aUuid,
                                                    std::string& aSource )
{
    static const std::set<std::string> types = {
        "smd", "thru_hole", "connect", "np_thru_hole"
    };
    static const std::set<std::string> shapes = {
        "circle", "rect", "oval", "trapezoid", "roundrect", "chamfered_rect", "custom"
    };
    static const std::map<std::string, std::string> propertyTokens = {
        { "bga", "pad_prop_bga" }, { "global_fiducial", "pad_prop_fiducial_glob" },
        { "local_fiducial", "pad_prop_fiducial_loc" },
        { "testpoint", "pad_prop_testpoint" }, { "heatsink", "pad_prop_heatsink" },
        { "castellated", "pad_prop_castellated" },
        { "mechanical", "pad_prop_mechanical" }, { "pressfit", "pad_prop_pressfit" }
    };
    static const std::map<std::string, int> zoneConnections = {
        { "none", 0 }, { "thermal", 1 }, { "solid", 2 }, { "tht_thermal", 3 }
    };
    static const std::map<std::string, std::string> layerTokens = {
        { "all_copper", "*.Cu" }, { "all_mask", "*.Mask" },
        { "F.Cu", "F.Cu" }, { "B.Cu", "B.Cu" }, { "F.Mask", "F.Mask" },
        { "B.Mask", "B.Mask" }, { "F.Paste", "F.Paste" },
        { "B.Paste", "B.Paste" }, { "F.Adhes", "F.Adhes" },
        { "B.Adhes", "B.Adhes" }
    };

    if( !aPad.is_object() || !aPad.contains( "number" ) || !aPad["number"].is_string()
        || !aPad.contains( "type" ) || !aPad["type"].is_string()
        || !types.contains( aPad["type"].get<std::string>() )
        || !aPad.contains( "shape" ) || !aPad["shape"].is_string()
        || !shapes.contains( aPad["shape"].get<std::string>() )
        || !aPad.contains( "at" ) || !validPoint( aPad["at"] )
        || !aPad.contains( "size" ) || !validPoint( aPad["size"] )
        || !aPad.contains( "rotationTenths" ) || !aPad["rotationTenths"].is_number_integer()
        || !aPad.contains( "layers" ) || !aPad["layers"].is_array()
        || !aPad.contains( "shapeOffset" ) || !validPoint( aPad["shapeOffset"] )
        || !aPad.contains( "trapezoidDelta" ) || !validPoint( aPad["trapezoidDelta"] )
        || !validUuid( aUuid ) )
    {
        return false;
    }

    const std::string type = aPad["type"].get<std::string>();
    const std::string shape = aPad["shape"].get<std::string>();
    const std::string nativeShape = shape == "chamfered_rect" ? "roundrect" : shape;
    const int64_t width = aPad["size"]["xNm"].get<int64_t>();
    const int64_t height = aPad["size"]["yNm"].get<int64_t>();

    if( width <= 0 || height <= 0 )
        return false;

    aSource += "\t(pad " + quoteText( aPad["number"].get<std::string>() ) + " " + type + " "
               + nativeShape + "\n"
               "\t\t(at " + millimetres( aPad["at"]["xNm"].get<int64_t>() ) + " "
               + millimetres( aPad["at"]["yNm"].get<int64_t>() ) + " "
               + degrees( aPad["rotationTenths"].get<int64_t>() ) + ")\n"
               "\t\t(size " + millimetres( width ) + " " + millimetres( height ) + ")\n";

    const int64_t deltaX = aPad["trapezoidDelta"]["xNm"].get<int64_t>();
    const int64_t deltaY = aPad["trapezoidDelta"]["yNm"].get<int64_t>();

    if( deltaX != 0 || deltaY != 0 )
        aSource += "\t\t(rect_delta " + millimetres( deltaX ) + " "
                   + millimetres( deltaY ) + ")\n";

    const int64_t offsetX = aPad["shapeOffset"]["xNm"].get<int64_t>();
    const int64_t offsetY = aPad["shapeOffset"]["yNm"].get<int64_t>();
    const bool hasOffset = offsetX != 0 || offsetY != 0;

    if( !aPad["drill"].is_null() || hasOffset )
    {
        aSource += "\t\t(drill";

        if( aPad["drill"].is_object() )
        {
            const std::string drillShape = aPad["drill"].value( "shape", "" );
            const int64_t drillX = aPad["drill"].value( "xNm", int64_t{ 0 } );
            const int64_t drillY = aPad["drill"].value( "yNm", int64_t{ 0 } );

            if( ( drillShape != "round" && drillShape != "oval" ) || drillX <= 0
                || drillY <= 0 )
            {
                return false;
            }

            if( drillShape == "oval" )
                aSource += " oval";

            aSource += " " + millimetres( drillX );

            if( drillY != drillX )
                aSource += " " + millimetres( drillY );
        }

        if( hasOffset )
            aSource += "\n\t\t\t(offset " + millimetres( offsetX ) + " "
                       + millimetres( offsetY ) + ")\n\t\t";

        aSource += ")\n";
    }

    if( aPad.contains( "backdrills" ) && aPad["backdrills"].is_object() )
    {
        if( type != "thru_hole"
            || !BACKDRILL_GENERATOR::Render( aPad["backdrills"], aSource ) )
        {
            return false;
        }
    }
    else if( !aPad.contains( "backdrills" ) || !aPad["backdrills"].is_null() )
    {
        return false;
    }

    const std::string property = aPad.value( "property", "none" );

    if( property != "none" )
    {
        if( !propertyTokens.contains( property ) )
            return false;

        aSource += "\t\t(property " + propertyTokens.at( property ) + ")\n";
    }

    aSource += "\t\t(layers";
    std::set<std::string> uniqueLayers;

    for( const JSON& layer : aPad["layers"] )
    {
        if( !layer.is_string() || !layerTokens.contains( layer.get<std::string>() )
            || !uniqueLayers.emplace( layer.get<std::string>() ).second )
        {
            return false;
        }

        aSource += " " + quoteText( layerTokens.at( layer.get<std::string>() ) );
    }

    aSource += ")\n";

    if( type == "thru_hole" )
    {
        aSource += "\t\t(remove_unused_layers "
                   + std::string( aPad.value( "removeUnusedLayers", false ) ? "yes" : "no" )
                   + ")\n";

        if( aPad.value( "removeUnusedLayers", false ) )
            aSource += "\t\t(keep_end_layers "
                       + std::string( aPad.value( "keepEndLayers", false ) ? "yes" : "no" )
                       + ")\n";
    }

    if( shape == "roundrect" || shape == "chamfered_rect" )
    {
        if( !aPad.contains( "roundrectRadiusNm" )
            || !aPad["roundrectRadiusNm"].is_number_integer() )
        {
            return false;
        }

        const int64_t radius = aPad["roundrectRadiusNm"].get<int64_t>();

        if( radius < 0 || ( shape == "roundrect" && radius == 0 )
            || radius * 2 > std::min( width, height ) )
            return false;

        const int64_t ratioPpm = static_cast<int64_t>(
                std::llround( static_cast<long double>( radius ) * 1'000'000.0L
                              / static_cast<long double>( std::min( width, height ) ) ) );
        aSource += "\t\t(roundrect_rratio " + decimalPpm( ratioPpm ) + ")\n";
    }

    if( shape == "chamfered_rect" )
    {
        if( !aPad.contains( "chamferRatioPpm" )
            || !aPad["chamferRatioPpm"].is_number_integer()
            || aPad["chamferRatioPpm"].get<int64_t>() <= 0
            || aPad["chamferRatioPpm"].get<int64_t>() > 500'000
            || !aPad.contains( "chamferCorners" ) || !aPad["chamferCorners"].is_array()
            || aPad["chamferCorners"].empty() )
        {
            return false;
        }

        static const std::set<std::string> corners = {
            "top_left", "top_right", "bottom_left", "bottom_right"
        };
        std::set<std::string> unique;
        aSource += "\t\t(chamfer_ratio "
                   + decimalPpm( aPad["chamferRatioPpm"].get<int64_t>() ) + ")\n"
                   "\t\t(chamfer";

        for( const JSON& corner : aPad["chamferCorners"] )
        {
            if( !corner.is_string() || !corners.contains( corner.get<std::string>() )
                || !unique.emplace( corner.get<std::string>() ).second )
            {
                return false;
            }

            aSource += " " + corner.get<std::string>();
        }

        aSource += ")\n";
    }
    else if( ( aPad.contains( "chamferRatioPpm" ) && !aPad["chamferRatioPpm"].is_null() )
             || ( aPad.contains( "chamferCorners" ) && !aPad["chamferCorners"].empty() ) )
    {
        return false;
    }

    if( shape == "custom" )
    {
        if( !aPad.contains( "custom" ) || !aPad["custom"].is_object()
            || !CUSTOM_GENERATOR::Render( aPad["custom"], aSource ) )
        {
            return false;
        }
    }
    else if( !aPad.contains( "custom" ) || !aPad["custom"].is_null() )
    {
        return false;
    }

    const std::string pinFunction = aPad.value( "pinFunction", "" );
    const std::string pinType = aPad.value( "pinType", "" );

    if( !pinFunction.empty() )
        aSource += "\t\t(pinfunction " + quoteText( pinFunction ) + ")\n";

    if( !pinType.empty() )
        aSource += "\t\t(pintype " + quoteText( pinType ) + ")\n";

    if( aPad.value( "dieLengthNm", int64_t{ 0 } ) > 0 )
        aSource += "\t\t(die_length "
                   + millimetres( aPad["dieLengthNm"].get<int64_t>() ) + ")\n";

    const std::pair<const char*, const char*> distanceOverrides[] = {
        { "solderMaskMarginNm", "solder_mask_margin" },
        { "solderPasteMarginNm", "solder_paste_margin" },
        { "clearanceNm", "clearance" },
        { "thermalSpokeWidthNm", "thermal_bridge_width" },
        { "thermalGapNm", "thermal_gap" }
    };

    for( const auto& [field, token] : distanceOverrides )
    {
        if( aPad.contains( field ) && aPad[field].is_number_integer() )
            aSource += "\t\t(" + std::string( token ) + " "
                       + millimetres( aPad[field].get<int64_t>() ) + ")\n";
        else if( !aPad.contains( field ) || !aPad[field].is_null() )
            return false;
    }

    if( aPad.contains( "solderPasteMarginPpm" )
        && aPad["solderPasteMarginPpm"].is_number_integer() )
    {
        aSource += "\t\t(solder_paste_margin_ratio "
                   + decimalPpm( aPad["solderPasteMarginPpm"].get<int64_t>() ) + ")\n";
    }
    else if( !aPad.contains( "solderPasteMarginPpm" )
             || !aPad["solderPasteMarginPpm"].is_null() )
    {
        return false;
    }

    const std::string zoneConnection = aPad.value( "zoneConnection", "" );

    if( zoneConnection != "inherit" )
    {
        if( !zoneConnections.contains( zoneConnection ) )
            return false;

        aSource += "\t\t(zone_connect " + std::to_string( zoneConnections.at( zoneConnection ) )
                   + ")\n";
    }

    if( aPad.contains( "thermalSpokeAngleTenths" )
        && aPad["thermalSpokeAngleTenths"].is_number_integer() )
    {
        aSource += "\t\t(thermal_bridge_angle "
                   + degrees( aPad["thermalSpokeAngleTenths"].get<int64_t>() ) + ")\n";
    }
    else if( !aPad.contains( "thermalSpokeAngleTenths" )
             || !aPad["thermalSpokeAngleTenths"].is_null() )
    {
        return false;
    }

    if( aPad.contains( "padstack" ) && aPad["padstack"].is_object() )
    {
        if( type != "thru_hole" || !PADSTACK_GENERATOR::Render( aPad["padstack"], aSource ) )
            return false;
    }
    else if( !aPad.contains( "padstack" ) || !aPad["padstack"].is_null() )
    {
        return false;
    }

    if( aPad.contains( "holeTreatment" ) && aPad["holeTreatment"].is_object() )
    {
        if( !HOLE_TREATMENT_GENERATOR::Render( aPad["holeTreatment"], aSource ) )
            return false;
    }
    else if( !aPad.contains( "holeTreatment" ) || !aPad["holeTreatment"].is_null() )
    {
        return false;
    }

    aSource += "\t\t(uuid " + quoteText( aUuid ) + ")\n\t)\n";
    return true;
}

} // namespace KICHAD
