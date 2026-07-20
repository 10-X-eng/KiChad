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

#include "design_script_symbol_text_generator.h"

#include <cstdint>
#include <set>
#include <string>


namespace
{

using JSON = nlohmann::json;


std::string quoted( const std::string& aText )
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
    if( aPartsPerMillion == 0 )
        return "0";

    const bool negative = aPartsPerMillion < 0;
    const uint64_t magnitude = negative
                                       ? static_cast<uint64_t>( -( aPartsPerMillion + 1 ) ) + 1
                                       : static_cast<uint64_t>( aPartsPerMillion );
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


std::string degrees( int64_t aTenths )
{
    if( aTenths % 10 == 0 )
        return std::to_string( aTenths / 10 );

    return std::to_string( aTenths / 10 ) + "." + std::to_string( aTenths % 10 );
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


bool renderEffects( const JSON& aEffects, std::string& aSource )
{
    static const std::set<std::string> horizontalValues = { "left", "center", "right" };
    static const std::set<std::string> verticalValues = { "top", "center", "bottom" };

    if( !aEffects.is_object() || !aEffects.contains( "font" ) || !aEffects["font"].is_string()
        || !aEffects.contains( "widthNm" ) || !aEffects["widthNm"].is_number_integer()
        || !aEffects.contains( "heightNm" ) || !aEffects["heightNm"].is_number_integer()
        || !aEffects.contains( "thicknessNm" )
        || !( aEffects["thicknessNm"].is_null()
              || aEffects["thicknessNm"].is_number_integer() )
        || !aEffects.contains( "bold" ) || !aEffects["bold"].is_boolean()
        || !aEffects.contains( "italic" ) || !aEffects["italic"].is_boolean()
        || !aEffects.contains( "lineSpacingPpm" )
        || !aEffects["lineSpacingPpm"].is_number_integer()
        || !aEffects.contains( "color" )
        || !( aEffects["color"].is_null() || validColor( aEffects["color"] ) )
        || !aEffects.contains( "horizontal" ) || !aEffects["horizontal"].is_string()
        || !horizontalValues.contains( aEffects["horizontal"].get<std::string>() )
        || !aEffects.contains( "vertical" ) || !aEffects["vertical"].is_string()
        || !verticalValues.contains( aEffects["vertical"].get<std::string>() )
        || !aEffects.contains( "hyperlink" ) || !aEffects["hyperlink"].is_string() )
    {
        return false;
    }

    aSource += "\t\t\t\t(effects\n"
               "\t\t\t\t\t(font\n";

    if( !aEffects["font"].get_ref<const std::string&>().empty() )
        aSource += "\t\t\t\t\t\t(face " + quoted( aEffects["font"].get<std::string>() ) + ")\n";

    aSource += "\t\t\t\t\t\t(size "
               + millimetres( aEffects["heightNm"].get<int64_t>() ) + " "
               + millimetres( aEffects["widthNm"].get<int64_t>() ) + ")\n";

    if( aEffects["lineSpacingPpm"].get<int64_t>() != 1'000'000 )
        aSource += "\t\t\t\t\t\t(line_spacing "
                   + decimalPpm( aEffects["lineSpacingPpm"].get<int64_t>() ) + ")\n";

    if( !aEffects["thicknessNm"].is_null() )
        aSource += "\t\t\t\t\t\t(thickness "
                   + millimetres( aEffects["thicknessNm"].get<int64_t>() ) + ")\n";

    if( aEffects["bold"].get<bool>() )
        aSource += "\t\t\t\t\t\t(bold yes)\n";

    if( aEffects["italic"].get<bool>() )
        aSource += "\t\t\t\t\t\t(italic yes)\n";

    if( !aEffects["color"].is_null() )
        aSource += "\t\t\t\t\t\t" + renderColor( aEffects["color"] ) + "\n";

    aSource += "\t\t\t\t\t)\n";

    const std::string horizontal = aEffects["horizontal"].get<std::string>();
    const std::string vertical = aEffects["vertical"].get<std::string>();

    if( horizontal != "center" || vertical != "center" )
    {
        aSource += "\t\t\t\t\t(justify";

        if( horizontal != "center" )
            aSource += " " + horizontal;

        if( vertical != "center" )
            aSource += " " + vertical;

        aSource += ")\n";
    }

    if( !aEffects["hyperlink"].get_ref<const std::string&>().empty() )
        aSource += "\t\t\t\t\t(href " + quoted( aEffects["hyperlink"].get<std::string>() ) + ")\n";

    aSource += "\t\t\t\t)\n";
    return true;
}


bool renderStrokeAndFill( const JSON& aItem, std::string& aSource )
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

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_SYMBOL_TEXT_GENERATOR::Render( const JSON& aItem, std::string& aSource )
{
    if( !aItem.is_object() || !aItem.contains( "kind" ) || !aItem["kind"].is_string()
        || !aItem.contains( "text" ) || !aItem["text"].is_string()
        || !aItem.contains( "private" ) || !aItem["private"].is_boolean()
        || !aItem.contains( "at" ) || !validPoint( aItem["at"] )
        || !aItem.contains( "rotationTenths" )
        || !aItem["rotationTenths"].is_number_integer()
        || !aItem.contains( "effects" ) || !aItem["effects"].is_object() )
    {
        return false;
    }

    const std::string kind = aItem["kind"].get<std::string>();

    if( kind != "text" && kind != "text_box" )
        return false;

    aSource += "\t\t\t(" + kind;

    if( aItem["private"].get<bool>() )
        aSource += " private";

    aSource += " " + quoted( aItem["text"].get<std::string>() ) + "\n"
               "\t\t\t\t(at " + millimetres( aItem["at"]["xNm"].get<int64_t>() ) + " "
               + millimetres( aItem["at"]["yNm"].get<int64_t>() ) + " ";

    if( kind == "text" )
    {
        aSource += std::to_string( aItem["rotationTenths"].get<int64_t>() ) + ")\n";
    }
    else
    {
        if( !aItem.contains( "boxSize" ) || !aItem["boxSize"].is_object()
            || !aItem["boxSize"].contains( "widthNm" )
            || !aItem["boxSize"]["widthNm"].is_number_integer()
            || !aItem["boxSize"].contains( "heightNm" )
            || !aItem["boxSize"]["heightNm"].is_number_integer()
            || !aItem.contains( "marginsNm" ) || !aItem["marginsNm"].is_array()
            || aItem["marginsNm"].size() != 4 )
        {
            return false;
        }

        aSource += degrees( aItem["rotationTenths"].get<int64_t>() ) + ")\n"
                   "\t\t\t\t(size "
                   + millimetres( aItem["boxSize"]["widthNm"].get<int64_t>() ) + " "
                   + millimetres( aItem["boxSize"]["heightNm"].get<int64_t>() ) + ")\n"
                   "\t\t\t\t(margins";

        for( const JSON& margin : aItem["marginsNm"] )
        {
            if( !margin.is_number_integer() )
                return false;

            aSource += " " + millimetres( margin.get<int64_t>() );
        }

        aSource += ")\n";

        if( !renderStrokeAndFill( aItem, aSource ) )
            return false;
    }

    if( !renderEffects( aItem["effects"], aSource ) )
        return false;

    aSource += "\t\t\t)\n";
    return true;
}

} // namespace KICHAD
