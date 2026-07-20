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

#include "design_script_symbol_graphics_generator.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>


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
    if( aPartsPerMillion == 0 )
        return "0";

    if( aPartsPerMillion == 1'000'000 )
        return "1";

    std::string digits = std::to_string( aPartsPerMillion );
    std::string result = "0." + std::string( 6 - digits.size(), '0' ) + digits;

    while( result.back() == '0' )
        result.pop_back();

    return result;
}


bool validPoint( const JSON& aPoint )
{
    return aPoint.is_object() && aPoint.contains( "xNm" ) && aPoint["xNm"].is_number_integer()
           && aPoint.contains( "yNm" ) && aPoint["yNm"].is_number_integer();
}


bool validColor( const JSON& aColor )
{
    if( !aColor.is_object() || !aColor.contains( "red" ) || !aColor["red"].is_number_integer()
        || !aColor.contains( "green" ) || !aColor["green"].is_number_integer()
        || !aColor.contains( "blue" ) || !aColor["blue"].is_number_integer()
        || !aColor.contains( "alphaPpm" ) || !aColor["alphaPpm"].is_number_integer() )
    {
        return false;
    }

    return aColor["red"].get<int64_t>() >= 0 && aColor["red"].get<int64_t>() <= 255
           && aColor["green"].get<int64_t>() >= 0 && aColor["green"].get<int64_t>() <= 255
           && aColor["blue"].get<int64_t>() >= 0 && aColor["blue"].get<int64_t>() <= 255
           && aColor["alphaPpm"].get<int64_t>() >= 0
           && aColor["alphaPpm"].get<int64_t>() <= 1'000'000;
}


std::string renderColor( const JSON& aColor )
{
    return "(color " + std::to_string( aColor["red"].get<int64_t>() ) + " "
           + std::to_string( aColor["green"].get<int64_t>() ) + " "
           + std::to_string( aColor["blue"].get<int64_t>() ) + " "
           + decimalPpm( aColor["alphaPpm"].get<int64_t>() ) + ")";
}


bool renderStyle( const JSON& aItem, std::string& aSource )
{
    static const std::set<std::string> strokeStyles = {
        "default", "solid", "dash", "dot", "dash_dot", "dash_dot_dot"
    };
    static const std::set<std::string> fillTypes = {
        "none", "outline", "background", "color", "hatch", "reverse_hatch", "cross_hatch"
    };

    if( !aItem.contains( "stroke" ) || !aItem["stroke"].is_object()
        || !aItem["stroke"].contains( "widthNm" )
        || !aItem["stroke"]["widthNm"].is_number_integer()
        || !aItem["stroke"].contains( "style" ) || !aItem["stroke"]["style"].is_string()
        || !strokeStyles.contains( aItem["stroke"]["style"].get<std::string>() )
        || !aItem.contains( "fill" ) || !aItem["fill"].is_object()
        || !aItem["fill"].contains( "type" ) || !aItem["fill"]["type"].is_string()
        || !fillTypes.contains( aItem["fill"]["type"].get<std::string>() ) )
    {
        return false;
    }

    aSource += "\t\t\t\t(stroke\n"
               "\t\t\t\t\t(width "
               + millimetres( aItem["stroke"]["widthNm"].get<int64_t>() ) + ")\n"
               "\t\t\t\t\t(type " + aItem["stroke"]["style"].get<std::string>() + ")\n";

    if( aItem["stroke"].contains( "color" ) )
    {
        if( !validColor( aItem["stroke"]["color"] ) )
            return false;

        aSource += "\t\t\t\t\t" + renderColor( aItem["stroke"]["color"] ) + "\n";
    }

    aSource += "\t\t\t\t)\n"
               "\t\t\t\t(fill\n"
               "\t\t\t\t\t(type " + aItem["fill"]["type"].get<std::string>() + ")\n";

    if( aItem["fill"].contains( "color" ) )
    {
        if( !validColor( aItem["fill"]["color"] ) )
            return false;

        aSource += "\t\t\t\t\t" + renderColor( aItem["fill"]["color"] ) + "\n";
    }

    aSource += "\t\t\t\t)\n";
    return true;
}


std::string pointValues( const JSON& aPoint )
{
    return millimetres( aPoint["xNm"].get<int64_t>() ) + " "
           + millimetres( aPoint["yNm"].get<int64_t>() );
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_SYMBOL_GRAPHICS_GENERATOR::Render( const JSON& aItem,
                                                      std::string& aSource )
{
    if( !aItem.is_object() || !aItem.contains( "kind" ) || !aItem["kind"].is_string()
        || !aItem.contains( "private" ) || !aItem["private"].is_boolean() )
    {
        return false;
    }

    const std::string kind = aItem["kind"].get<std::string>();
    const std::string privateToken = aItem["private"].get<bool>() ? " private" : "";
    static const std::set<std::string> supported = {
        "arc", "bezier", "circle", "polyline", "rectangle"
    };

    if( !supported.contains( kind ) )
        return false;

    aSource += "\t\t\t(" + kind + privateToken + "\n";

    if( kind == "rectangle" )
    {
        if( !aItem.contains( "from" ) || !validPoint( aItem["from"] )
            || !aItem.contains( "to" ) || !validPoint( aItem["to"] ) )
        {
            return false;
        }

        aSource += "\t\t\t\t(start " + pointValues( aItem["from"] ) + ")\n"
                   "\t\t\t\t(end " + pointValues( aItem["to"] ) + ")\n";

        if( aItem.contains( "radiusNm" ) )
        {
            if( !aItem["radiusNm"].is_number_integer() )
                return false;

            aSource += "\t\t\t\t(radius "
                       + millimetres( aItem["radiusNm"].get<int64_t>() ) + ")\n";
        }
    }
    else if( kind == "circle" )
    {
        if( !aItem.contains( "center" ) || !validPoint( aItem["center"] )
            || !aItem.contains( "radiusNm" ) || !aItem["radiusNm"].is_number_integer() )
        {
            return false;
        }

        aSource += "\t\t\t\t(center " + pointValues( aItem["center"] ) + ")\n"
                   "\t\t\t\t(radius "
                   + millimetres( aItem["radiusNm"].get<int64_t>() ) + ")\n";
    }
    else if( kind == "arc" )
    {
        for( const char* field : { "start", "mid", "end" } )
        {
            if( !aItem.contains( field ) || !validPoint( aItem[field] ) )
                return false;

            aSource += "\t\t\t\t(" + std::string( field ) + " "
                       + pointValues( aItem[field] ) + ")\n";
        }
    }
    else if( kind == "bezier" )
    {
        static const std::vector<std::string> fields = { "start", "control1", "control2", "end" };

        for( const std::string& field : fields )
        {
            if( !aItem.contains( field ) || !validPoint( aItem[field] ) )
                return false;
        }

        aSource += "\t\t\t\t(pts\n"
                   "\t\t\t\t\t(xy " + pointValues( aItem["start"] ) + ")\n"
                   "\t\t\t\t\t(xy " + pointValues( aItem["control1"] ) + ")\n"
                   "\t\t\t\t\t(xy " + pointValues( aItem["control2"] ) + ")\n"
                   "\t\t\t\t\t(xy " + pointValues( aItem["end"] ) + ")\n"
                   "\t\t\t\t)\n";
    }
    else
    {
        if( !aItem.contains( "points" ) || !aItem["points"].is_array()
            || aItem["points"].size() < 2 )
        {
            return false;
        }

        aSource += "\t\t\t\t(pts\n";

        for( const JSON& point : aItem["points"] )
        {
            if( !validPoint( point ) )
                return false;

            aSource += "\t\t\t\t\t(xy " + pointValues( point ) + ")\n";
        }

        aSource += "\t\t\t\t)\n";
    }

    if( !renderStyle( aItem, aSource ) )
        return false;

    aSource += "\t\t\t)\n";
    return true;
}

} // namespace KICHAD
