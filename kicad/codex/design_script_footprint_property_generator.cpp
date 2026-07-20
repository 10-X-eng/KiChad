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

#include "design_script_footprint_property_generator.h"

#include <cstdint>
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


bool validUuid( const std::string& aUuid )
{
    return aUuid.size() == 36 && aUuid[8] == '-' && aUuid[13] == '-'
           && aUuid[18] == '-' && aUuid[23] == '-';
}


bool renderEffects( const JSON& aProperty, std::string& aSource )
{
    if( !aProperty.contains( "font" ) || !aProperty["font"].is_object()
        || !aProperty.contains( "justify" ) || !aProperty["justify"].is_object() )
    {
        return false;
    }

    const JSON& font = aProperty["font"];
    const JSON& justify = aProperty["justify"];
    static const std::set<std::string> horizontalValues = { "left", "center", "right" };
    static const std::set<std::string> verticalValues = { "top", "center", "bottom" };

    if( !font.contains( "face" ) || !font["face"].is_string()
        || !font.contains( "size" ) || !font["size"].is_object()
        || !font["size"].contains( "heightNm" )
        || !font["size"]["heightNm"].is_number_integer()
        || !font["size"].contains( "widthNm" )
        || !font["size"]["widthNm"].is_number_integer()
        || !font.contains( "lineSpacingPpm" ) || !font["lineSpacingPpm"].is_number_integer()
        || !font.contains( "thicknessNm" )
        || !( font["thicknessNm"].is_null() || font["thicknessNm"].is_number_integer() )
        || !font.contains( "bold" ) || !font["bold"].is_boolean()
        || !font.contains( "italic" ) || !font["italic"].is_boolean()
        || !justify.contains( "horizontal" ) || !justify["horizontal"].is_string()
        || !horizontalValues.contains( justify["horizontal"].get<std::string>() )
        || !justify.contains( "vertical" ) || !justify["vertical"].is_string()
        || !verticalValues.contains( justify["vertical"].get<std::string>() )
        || !justify.contains( "mirrored" ) || !justify["mirrored"].is_boolean() )
    {
        return false;
    }

    const int64_t height = font["size"]["heightNm"].get<int64_t>();
    const int64_t width = font["size"]["widthNm"].get<int64_t>();

    if( height <= 0 || width <= 0 )
        return false;

    aSource += "\t\t(effects\n\t\t\t(font\n";

    if( font["face"].get<std::string>() != "default" )
        aSource += "\t\t\t\t(face " + quoteText( font["face"].get<std::string>() ) + ")\n";

    aSource += "\t\t\t\t(size " + fixedDecimal( height, 1'000'000 ) + " "
               + fixedDecimal( width, 1'000'000 ) + ")\n";

    if( font["lineSpacingPpm"].get<int64_t>() != 1'000'000 )
        aSource += "\t\t\t\t(line_spacing "
                   + fixedDecimal( font["lineSpacingPpm"].get<int64_t>(), 1'000'000 ) + ")\n";

    if( font["thicknessNm"].is_number_integer() )
        aSource += "\t\t\t\t(thickness "
                   + fixedDecimal( font["thicknessNm"].get<int64_t>(), 1'000'000 ) + ")\n";

    if( font["bold"].get<bool>() )
        aSource += "\t\t\t\t(bold yes)\n";

    if( font["italic"].get<bool>() )
        aSource += "\t\t\t\t(italic yes)\n";

    aSource += "\t\t\t)\n";
    const std::string horizontal = justify["horizontal"].get<std::string>();
    const std::string vertical = justify["vertical"].get<std::string>();
    const bool mirrored = justify["mirrored"].get<bool>();

    if( horizontal != "center" || vertical != "center" || mirrored )
    {
        aSource += "\t\t\t(justify";

        if( horizontal != "center" )
            aSource += " " + horizontal;

        if( vertical != "center" )
            aSource += " " + vertical;

        if( mirrored )
            aSource += " mirror";

        aSource += ")\n";
    }

    aSource += "\t\t)\n";
    return true;
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_FOOTPRINT_PROPERTY_GENERATOR::Render(
        const JSON& aProperty, const std::string& aUuid, std::string& aSource )
{
    if( !aProperty.is_object() || !aProperty.contains( "id" )
        || !aProperty["id"].is_string() || aProperty["id"].get<std::string>().empty()
        || !aProperty.contains( "name" ) || !aProperty["name"].is_string()
        || aProperty["name"].get<std::string>().empty()
        || !aProperty.contains( "value" ) || !aProperty["value"].is_string()
        || !aProperty.contains( "at" ) || !aProperty["at"].is_object()
        || !aProperty["at"].contains( "xNm" ) || !aProperty["at"]["xNm"].is_number_integer()
        || !aProperty["at"].contains( "yNm" ) || !aProperty["at"]["yNm"].is_number_integer()
        || !aProperty.contains( "rotationTenths" )
        || !aProperty["rotationTenths"].is_number_integer()
        || !aProperty.contains( "layer" ) || !aProperty["layer"].is_string()
        || aProperty["layer"].get<std::string>().empty()
        || !aProperty.contains( "visible" ) || !aProperty["visible"].is_boolean()
        || !aProperty.contains( "keepUpright" ) || !aProperty["keepUpright"].is_boolean()
        || !aProperty.contains( "knockout" ) || !aProperty["knockout"].is_boolean()
        || !validUuid( aUuid ) )
    {
        return false;
    }

    aSource += "\t(property " + quoteText( aProperty["name"].get<std::string>() ) + " "
               + quoteText( aProperty["value"].get<std::string>() ) + "\n"
               "\t\t(at " + fixedDecimal( aProperty["at"]["xNm"].get<int64_t>(), 1'000'000 )
               + " " + fixedDecimal( aProperty["at"]["yNm"].get<int64_t>(), 1'000'000 )
               + " " + fixedDecimal( aProperty["rotationTenths"].get<int64_t>(), 10 ) + ")\n";

    if( !aProperty["keepUpright"].get<bool>() )
        aSource += "\t\t(unlocked yes)\n";

    aSource += "\t\t(layer " + quoteText( aProperty["layer"].get<std::string>() );

    if( aProperty["knockout"].get<bool>() )
        aSource += " knockout";

    aSource += ")\n";

    if( !aProperty["visible"].get<bool>() )
        aSource += "\t\t(hide yes)\n";

    aSource += "\t\t(uuid " + quoteText( aUuid ) + ")\n";

    if( !renderEffects( aProperty, aSource ) )
        return false;

    aSource += "\t)\n";
    return true;
}

} // namespace KICHAD
