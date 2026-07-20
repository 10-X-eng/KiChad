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

#include "design_script_symbol_field_generator.h"

#include <cstdint>
#include <set>
#include <string>


namespace
{

using JSON = nlohmann::json;


std::string quoted( const std::string& aText )
{
    std::string result = "\"";

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


std::string decimalMillionths( int64_t aValue )
{
    const bool negative = aValue < 0;
    const uint64_t magnitude = negative ? static_cast<uint64_t>( -( aValue + 1 ) ) + 1
                                        : static_cast<uint64_t>( aValue );
    std::string result = negative ? "-" : "";
    result += std::to_string( magnitude / 1'000'000 );
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


std::string degrees( int64_t aTenths )
{
    return aTenths % 10 == 0 ? std::to_string( aTenths / 10 )
                             : std::to_string( aTenths / 10 ) + "."
                                       + std::to_string( aTenths % 10 );
}


bool validColor( const JSON& aColor )
{
    return aColor.is_object() && aColor.contains( "red" ) && aColor["red"].is_number_integer()
           && aColor.contains( "green" ) && aColor["green"].is_number_integer()
           && aColor.contains( "blue" ) && aColor["blue"].is_number_integer()
           && aColor.contains( "alphaPpm" ) && aColor["alphaPpm"].is_number_integer()
           && aColor["red"].get<int64_t>() >= 0 && aColor["red"].get<int64_t>() <= 255
           && aColor["green"].get<int64_t>() >= 0 && aColor["green"].get<int64_t>() <= 255
           && aColor["blue"].get<int64_t>() >= 0 && aColor["blue"].get<int64_t>() <= 255
           && aColor["alphaPpm"].get<int64_t>() >= 0
           && aColor["alphaPpm"].get<int64_t>() <= 1'000'000;
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_SYMBOL_FIELD_GENERATOR::Render( const std::string& aName,
                                                   const std::string& aValue,
                                                   const JSON& aLayout,
                                                   std::string& aSource )
{
    static const std::set<std::string> horizontalValues = { "left", "center", "right" };
    static const std::set<std::string> verticalValues = { "top", "center", "bottom" };

    if( !aLayout.is_object() || !aLayout.contains( "position" )
        || !aLayout["position"].is_object() || !aLayout["position"].contains( "xNm" )
        || !aLayout["position"]["xNm"].is_number_integer()
        || !aLayout["position"].contains( "yNm" )
        || !aLayout["position"]["yNm"].is_number_integer()
        || !aLayout.contains( "rotationTenths" )
        || !aLayout["rotationTenths"].is_number_integer()
        || !aLayout.contains( "visible" ) || !aLayout["visible"].is_boolean()
        || !aLayout.contains( "showName" ) || !aLayout["showName"].is_boolean()
        || !aLayout.contains( "autoplace" ) || !aLayout["autoplace"].is_boolean()
        || !aLayout.contains( "private" ) || !aLayout["private"].is_boolean()
        || !aLayout.contains( "widthNm" ) || !aLayout["widthNm"].is_number_integer()
        || !aLayout.contains( "heightNm" ) || !aLayout["heightNm"].is_number_integer()
        || !aLayout.contains( "font" ) || !aLayout["font"].is_string()
        || !aLayout.contains( "lineSpacingPpm" )
        || !aLayout["lineSpacingPpm"].is_number_integer()
        || !aLayout.contains( "thicknessNm" )
        || !( aLayout["thicknessNm"].is_null() || aLayout["thicknessNm"].is_number_integer() )
        || !aLayout.contains( "color" )
        || !( aLayout["color"].is_null() || validColor( aLayout["color"] ) )
        || !aLayout.contains( "horizontal" ) || !aLayout["horizontal"].is_string()
        || !horizontalValues.contains( aLayout["horizontal"].get<std::string>() )
        || !aLayout.contains( "vertical" ) || !aLayout["vertical"].is_string()
        || !verticalValues.contains( aLayout["vertical"].get<std::string>() )
        || !aLayout.contains( "bold" ) || !aLayout["bold"].is_boolean()
        || !aLayout.contains( "italic" ) || !aLayout["italic"].is_boolean()
        || !aLayout.contains( "hyperlink" ) || !aLayout["hyperlink"].is_string() )
    {
        return false;
    }

    aSource += "\t\t(property ";

    if( aLayout["private"].get<bool>() )
        aSource += "private ";

    aSource += quoted( aName ) + " " + quoted( aValue ) + "\n"
               "\t\t\t(at "
               + decimalMillionths( aLayout["position"]["xNm"].get<int64_t>() ) + " "
               + decimalMillionths( aLayout["position"]["yNm"].get<int64_t>() ) + " "
               + degrees( aLayout["rotationTenths"].get<int64_t>() ) + ")\n"
               "\t\t\t(show_name " + ( aLayout["showName"].get<bool>() ? "yes" : "no" )
               + ")\n"
               "\t\t\t(do_not_autoplace "
               + ( aLayout["autoplace"].get<bool>() ? "no" : "yes" ) + ")\n";

    if( !aLayout["visible"].get<bool>() )
        aSource += "\t\t\t(hide yes)\n";

    aSource += "\t\t\t(effects\n"
               "\t\t\t\t(font\n";

    if( !aLayout["font"].get_ref<const std::string&>().empty() )
        aSource += "\t\t\t\t\t(face " + quoted( aLayout["font"].get<std::string>() ) + ")\n";

    aSource += "\t\t\t\t\t(size "
               + decimalMillionths( aLayout["heightNm"].get<int64_t>() ) + " "
               + decimalMillionths( aLayout["widthNm"].get<int64_t>() ) + ")\n";

    if( aLayout["lineSpacingPpm"].get<int64_t>() != 1'000'000 )
        aSource += "\t\t\t\t\t(line_spacing "
                   + decimalMillionths( aLayout["lineSpacingPpm"].get<int64_t>() ) + ")\n";

    if( !aLayout["thicknessNm"].is_null() )
        aSource += "\t\t\t\t\t(thickness "
                   + decimalMillionths( aLayout["thicknessNm"].get<int64_t>() ) + ")\n";

    if( aLayout["bold"].get<bool>() )
        aSource += "\t\t\t\t\t(bold yes)\n";

    if( aLayout["italic"].get<bool>() )
        aSource += "\t\t\t\t\t(italic yes)\n";

    if( !aLayout["color"].is_null() )
    {
        const JSON& color = aLayout["color"];
        aSource += "\t\t\t\t\t(color " + std::to_string( color["red"].get<int64_t>() )
                   + " " + std::to_string( color["green"].get<int64_t>() ) + " "
                   + std::to_string( color["blue"].get<int64_t>() ) + " "
                   + decimalMillionths( color["alphaPpm"].get<int64_t>() ) + ")\n";
    }

    aSource += "\t\t\t\t)\n";
    const std::string horizontal = aLayout["horizontal"].get<std::string>();
    const std::string vertical = aLayout["vertical"].get<std::string>();

    if( horizontal != "center" || vertical != "center" )
    {
        aSource += "\t\t\t\t(justify";

        if( horizontal != "center" )
            aSource += " " + horizontal;

        if( vertical != "center" )
            aSource += " " + vertical;

        aSource += ")\n";
    }

    if( !aLayout["hyperlink"].get_ref<const std::string&>().empty() )
        aSource += "\t\t\t\t(href " + quoted( aLayout["hyperlink"].get<std::string>() ) + ")\n";

    aSource += "\t\t\t)\n"
               "\t\t)\n";
    return true;
}

} // namespace KICHAD
