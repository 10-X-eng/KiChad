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

#include "design_script_footprint_graphic_generator.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>


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


bool validPoint( const JSON& aPoint )
{
    return aPoint.is_object() && aPoint.contains( "xNm" )
           && aPoint["xNm"].is_number_integer() && aPoint.contains( "yNm" )
           && aPoint["yNm"].is_number_integer();
}


std::string nativePoint( const JSON& aPoint )
{
    return millimetres( aPoint["xNm"].get<int64_t>() ) + " "
           + millimetres( aPoint["yNm"].get<int64_t>() );
}


bool validUuid( const std::string& aUuid )
{
    return aUuid.size() == 36 && aUuid[8] == '-' && aUuid[13] == '-'
           && aUuid[18] == '-' && aUuid[23] == '-';
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_FOOTPRINT_GRAPHIC_GENERATOR::Render(
        const JSON& aGraphic, const std::string& aUuid, std::string& aSource )
{
    static const std::set<std::string> kinds = {
        "line", "rectangle", "arc", "circle", "polygon", "bezier"
    };
    static const std::set<std::string> styles = {
        "solid", "dash", "dash_dot", "dash_dot_dot", "dot"
    };
    static const std::map<std::string, std::string> nativeKinds = {
        { "line", "line" }, { "rectangle", "rect" }, { "arc", "arc" },
        { "circle", "circle" }, { "polygon", "poly" }, { "bezier", "curve" }
    };
    static const std::map<std::string, std::string> nativeFills = {
        { "none", "no" }, { "solid", "yes" }, { "hatch", "hatch" },
        { "reverse_hatch", "reverse_hatch" }, { "cross_hatch", "cross_hatch" }
    };

    if( !aGraphic.is_object() || !aGraphic.contains( "kind" )
        || !aGraphic["kind"].is_string()
        || !kinds.contains( aGraphic["kind"].get<std::string>() )
        || !aGraphic.contains( "layers" ) || !aGraphic["layers"].is_array()
        || aGraphic["layers"].empty() || !aGraphic.contains( "stroke" )
        || !aGraphic["stroke"].is_object()
        || !aGraphic["stroke"].contains( "widthNm" )
        || !aGraphic["stroke"]["widthNm"].is_number_integer()
        || !aGraphic["stroke"].contains( "style" )
        || !aGraphic["stroke"]["style"].is_string()
        || !styles.contains( aGraphic["stroke"]["style"].get<std::string>() )
        || !aGraphic.contains( "fill" ) || !aGraphic["fill"].is_string()
        || !nativeFills.contains( aGraphic["fill"].get<std::string>() )
        || !aGraphic.contains( "locked" ) || !aGraphic["locked"].is_boolean()
        || !validUuid( aUuid ) )
    {
        return false;
    }

    const std::string kind = aGraphic["kind"].get<std::string>();
    aSource += "\t(fp_" + nativeKinds.at( kind ) + "\n";

    auto emitPoint = [&]( const char* aToken, const char* aField )
    {
        if( !aGraphic.contains( aField ) || !validPoint( aGraphic[aField] ) )
            return false;

        aSource += "\t\t(" + std::string( aToken ) + " "
                   + nativePoint( aGraphic[aField] ) + ")\n";
        return true;
    };

    if( kind == "line" || kind == "rectangle" )
    {
        if( !emitPoint( "start", "start" ) || !emitPoint( "end", "end" ) )
            return false;

        if( kind == "rectangle" )
        {
            if( !aGraphic.contains( "radiusNm" )
                || !aGraphic["radiusNm"].is_number_integer() )
            {
                return false;
            }

            const int64_t radius = aGraphic["radiusNm"].get<int64_t>();

            if( radius < 0 )
                return false;

            if( radius > 0 )
                aSource += "\t\t(radius " + millimetres( radius ) + ")\n";
        }
    }
    else if( kind == "arc" )
    {
        if( !emitPoint( "start", "start" ) || !emitPoint( "mid", "mid" )
            || !emitPoint( "end", "end" ) )
        {
            return false;
        }
    }
    else if( kind == "circle" )
    {
        if( !aGraphic.contains( "center" ) || !validPoint( aGraphic["center"] )
            || !aGraphic.contains( "radiusNm" )
            || !aGraphic["radiusNm"].is_number_integer()
            || aGraphic["radiusNm"].get<int64_t>() <= 0 )
        {
            return false;
        }

        const int64_t radius = aGraphic["radiusNm"].get<int64_t>();
        JSON endpoint = aGraphic["center"];
        endpoint["xNm"] = endpoint["xNm"].get<int64_t>() + radius;
        aSource += "\t\t(center " + nativePoint( aGraphic["center"] ) + ")\n"
                   "\t\t(end " + nativePoint( endpoint ) + ")\n";
    }
    else if( kind == "polygon" )
    {
        if( !aGraphic.contains( "points" ) || !aGraphic["points"].is_array()
            || aGraphic["points"].size() < 3 )
        {
            return false;
        }

        aSource += "\t\t(pts\n";

        for( const JSON& point : aGraphic["points"] )
        {
            if( !validPoint( point ) )
                return false;

            aSource += "\t\t\t(xy " + nativePoint( point ) + ")\n";
        }

        aSource += "\t\t)\n";
    }
    else if( kind == "bezier" )
    {
        const char* fields[] = { "start", "control1", "control2", "end" };

        for( const char* field : fields )
        {
            if( !aGraphic.contains( field ) || !validPoint( aGraphic[field] ) )
                return false;
        }

        aSource += "\t\t(pts\n"
                   "\t\t\t(xy " + nativePoint( aGraphic["start"] ) + ")\n"
                   "\t\t\t(xy " + nativePoint( aGraphic["control1"] ) + ")\n"
                   "\t\t\t(xy " + nativePoint( aGraphic["control2"] ) + ")\n"
                   "\t\t\t(xy " + nativePoint( aGraphic["end"] ) + ")\n"
                   "\t\t)\n";
    }

    const int64_t strokeWidth = aGraphic["stroke"]["widthNm"].get<int64_t>();

    if( strokeWidth < 0 )
        return false;

    aSource += "\t\t(stroke\n"
               "\t\t\t(width " + millimetres( strokeWidth ) + ")\n"
               "\t\t\t(type " + aGraphic["stroke"]["style"].get<std::string>() + ")\n"
               "\t\t)\n";

    if( kind == "rectangle" || kind == "circle" || kind == "polygon" )
        aSource += "\t\t(fill " + nativeFills.at( aGraphic["fill"].get<std::string>() ) + ")\n";
    else if( aGraphic["fill"] != "none" )
        return false;

    std::set<std::string> uniqueLayers;

    if( aGraphic["layers"].size() == 1 )
    {
        if( !aGraphic["layers"][0].is_string() )
            return false;

        aSource += "\t\t(layer " + quoteText( aGraphic["layers"][0].get<std::string>() ) + ")\n";
    }
    else
    {
        aSource += "\t\t(layers";

        for( const JSON& layer : aGraphic["layers"] )
        {
            if( !layer.is_string() || !uniqueLayers.emplace( layer.get<std::string>() ).second )
                return false;

            aSource += " " + quoteText( layer.get<std::string>() );
        }

        aSource += ")\n";
    }

    if( aGraphic.contains( "solderMaskMarginNm" )
        && aGraphic["solderMaskMarginNm"].is_number_integer() )
    {
        aSource += "\t\t(solder_mask_margin "
                   + millimetres( aGraphic["solderMaskMarginNm"].get<int64_t>() ) + ")\n";
    }
    else if( !aGraphic.contains( "solderMaskMarginNm" )
             || !aGraphic["solderMaskMarginNm"].is_null() )
    {
        return false;
    }

    if( aGraphic["locked"].get<bool>() )
        aSource += "\t\t(locked yes)\n";

    aSource += "\t\t(uuid " + quoteText( aUuid ) + ")\n\t)\n";
    return true;
}

} // namespace KICHAD
