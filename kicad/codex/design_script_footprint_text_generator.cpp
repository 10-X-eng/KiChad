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

#include "design_script_footprint_text_generator.h"

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
    std::string fraction = std::to_string( magnitude % 1'000'000 );
    std::string result = ( negative ? "-" : "" )
                         + std::to_string( magnitude / 1'000'000 );

    if( magnitude % 1'000'000 != 0 )
    {
        result += "." + std::string( 6 - fraction.size(), '0' ) + fraction;

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


bool renderEffects( const JSON& aText, std::string& aSource )
{
    if( !aText.contains( "font" ) || !aText["font"].is_object()
        || !aText.contains( "justify" ) || !aText["justify"].is_object() )
    {
        return false;
    }

    const JSON& font = aText["font"];
    const JSON& justify = aText["justify"];
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

    aSource += "\t\t\t\t(size " + millimetres( height ) + " "
               + millimetres( width ) + ")\n";

    if( font["lineSpacingPpm"].get<int64_t>() != 1'000'000 )
        aSource += "\t\t\t\t(line_spacing "
                   + decimalPpm( font["lineSpacingPpm"].get<int64_t>() ) + ")\n";

    if( font["thicknessNm"].is_number_integer() )
        aSource += "\t\t\t\t(thickness "
                   + millimetres( font["thicknessNm"].get<int64_t>() ) + ")\n";

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

    if( aText.contains( "hyperlink" ) )
    {
        if( !aText["hyperlink"].is_string()
            || aText["hyperlink"].get_ref<const std::string&>().find_first_of( "\0\r\n" )
                       != std::string::npos )
        {
            return false;
        }

        if( !aText["hyperlink"].get_ref<const std::string&>().empty() )
            aSource += "\t\t\t(href "
                       + quoteText( aText["hyperlink"].get<std::string>() ) + ")\n";
    }

    aSource += "\t\t)\n";
    return true;
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_FOOTPRINT_TEXT_GENERATOR::Render(
        const JSON& aText, const std::string& aUuid, std::string& aSource )
{
    if( !aText.is_object() || !aText.contains( "kind" ) || !aText["kind"].is_string()
        || ( aText["kind"] != "text" && aText["kind"] != "text_box" )
        || !aText.contains( "content" ) || !aText["content"].is_string()
        || !aText.contains( "rotationTenths" ) || !aText["rotationTenths"].is_number_integer()
        || !aText.contains( "layer" ) || !aText["layer"].is_string()
        || aText["layer"].get<std::string>().empty()
        || !aText.contains( "locked" ) || !aText["locked"].is_boolean()
        || !aText.contains( "knockout" ) || !aText["knockout"].is_boolean()
        || !validUuid( aUuid ) )
    {
        return false;
    }

    const std::string kind = aText["kind"].get<std::string>();
    aSource += kind == "text" ? "\t(fp_text user " : "\t(fp_text_box ";
    aSource += quoteText( aText["content"].get<std::string>() ) + "\n";

    if( aText["locked"].get<bool>() )
        aSource += "\t\t(locked yes)\n";

    if( kind == "text" )
    {
        if( !aText.contains( "at" ) || !validPoint( aText["at"] )
            || !aText.contains( "keepUpright" ) || !aText["keepUpright"].is_boolean() )
        {
            return false;
        }

        aSource += "\t\t(at " + nativePoint( aText["at"] ) + " "
                   + degrees( aText["rotationTenths"].get<int64_t>() ) + ")\n";

        if( !aText["keepUpright"].get<bool>() )
            aSource += "\t\t(unlocked yes)\n";
    }
    else
    {
        if( !aText.contains( "outline" ) || !aText["outline"].is_string() )
            return false;

        if( aText["outline"] == "rectangle" )
        {
            if( !aText.contains( "start" ) || !validPoint( aText["start"] )
                || !aText.contains( "end" ) || !validPoint( aText["end"] ) )
            {
                return false;
            }

            aSource += "\t\t(start " + nativePoint( aText["start"] ) + ")\n"
                       "\t\t(end " + nativePoint( aText["end"] ) + ")\n";
        }
        else if( aText["outline"] == "polygon" )
        {
            if( !aText.contains( "points" ) || !aText["points"].is_array()
                || aText["points"].size() < 3 )
            {
                return false;
            }

            aSource += "\t\t(pts\n";

            for( const JSON& point : aText["points"] )
            {
                if( !validPoint( point ) )
                    return false;

                aSource += "\t\t\t(xy " + nativePoint( point ) + ")\n";
            }

            aSource += "\t\t)\n";
        }
        else
        {
            return false;
        }

        if( !aText.contains( "margins" ) || !aText["margins"].is_object() )
            return false;

        const JSON& margins = aText["margins"];
        const char* marginFields[] = { "leftNm", "topNm", "rightNm", "bottomNm" };

        for( const char* field : marginFields )
        {
            if( !margins.contains( field ) || !margins[field].is_number_integer()
                || margins[field].get<int64_t>() < 0 )
            {
                return false;
            }
        }

        aSource += "\t\t(margins " + millimetres( margins["leftNm"].get<int64_t>() )
                   + " " + millimetres( margins["topNm"].get<int64_t>() ) + " "
                   + millimetres( margins["rightNm"].get<int64_t>() ) + " "
                   + millimetres( margins["bottomNm"].get<int64_t>() ) + ")\n";

        if( aText["rotationTenths"].get<int64_t>() != 0 )
            aSource += "\t\t(angle "
                       + degrees( aText["rotationTenths"].get<int64_t>() ) + ")\n";
    }

    aSource += "\t\t(layer " + quoteText( aText["layer"].get<std::string>() );

    if( kind == "text" && aText["knockout"].get<bool>() )
        aSource += " knockout";

    aSource += ")\n\t\t(uuid " + quoteText( aUuid ) + ")\n";

    if( !renderEffects( aText, aSource ) )
        return false;

    if( kind == "text_box" )
    {
        if( !aText.contains( "border" ) || !aText["border"].is_boolean()
            || !aText.contains( "stroke" ) || !aText["stroke"].is_object()
            || !aText["stroke"].contains( "widthNm" )
            || !aText["stroke"]["widthNm"].is_number_integer()
            || !aText["stroke"].contains( "style" )
            || !aText["stroke"]["style"].is_string() )
        {
            return false;
        }

        static const std::set<std::string> styles = {
            "solid", "dash", "dash_dot", "dash_dot_dot", "dot"
        };
        const std::string style = aText["stroke"]["style"].get<std::string>();
        const int64_t width = aText["stroke"]["widthNm"].get<int64_t>();

        if( !styles.contains( style ) || width < 0 )
            return false;

        aSource += "\t\t(border "
                   + std::string( aText["border"].get<bool>() ? "yes" : "no" ) + ")\n"
                   "\t\t(stroke\n"
                   "\t\t\t(width " + millimetres( width ) + ")\n"
                   "\t\t\t(type " + style + ")\n"
                   "\t\t)\n"
                   "\t\t(knockout "
                   + std::string( aText["knockout"].get<bool>() ? "yes" : "no" ) + ")\n";
    }

    aSource += "\t)\n";
    return true;
}

} // namespace KICHAD
