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

#include "design_script_symbol_field_compiler.h"
#include "kichad_from_chars.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_SYMBOL_FIELD_COMPILER::RESULT;

constexpr int64_t MAX_COORDINATE_NM = 2'000'000'000LL;
constexpr size_t MAX_FIELD_TEXT_BYTES = 4096;


void diagnostic( RESULT& aResult, const std::string& aCode, const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", "error" },
                                     { "code", aCode },
                                     { "message", aMessage } } );
}


bool scalar( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    if( aNode >= aDocument.Nodes().size()
        || aDocument.Nodes()[aNode].kind == DOCUMENT::NODE_KIND::LIST )
    {
        return false;
    }

    aValue = aDocument.AtomText( aNode );
    return true;
}


bool oneValue( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalar( aDocument, node.children[1], aValue );
}


bool boolean( const std::string& aText, bool& aValue )
{
    if( aText == "true" )
    {
        aValue = true;
        return true;
    }

    if( aText == "false" )
    {
        aValue = false;
        return true;
    }

    return false;
}


bool distance( const std::string& aText, int64_t& aNanometers )
{
    long double value = 0.0L;
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const std::from_chars_result converted = KICHAD::FromChars( begin, end, value );

    if( converted.ec != std::errc() || converted.ptr == begin || !std::isfinite( value ) )
        return false;

    const std::string_view unit( converted.ptr, static_cast<size_t>( end - converted.ptr ) );
    long double scale = 0.0L;

    if( unit == "mm" )
        scale = 1'000'000.0L;
    else if( unit == "mil" )
        scale = 25'400.0L;
    else if( unit == "um" )
        scale = 1'000.0L;
    else if( unit == "nm" )
        scale = 1.0L;
    else if( unit == "in" )
        scale = 25'400'000.0L;
    else
        return false;

    const long double rounded = std::round( value * scale );

    if( !std::isfinite( rounded ) || rounded < -MAX_COORDINATE_NM
        || rounded > MAX_COORDINATE_NM )
    {
        return false;
    }

    aNanometers = static_cast<int64_t>( rounded );
    return true;
}


bool integer( const std::string& aText, int64_t aMinimum, int64_t aMaximum, int64_t& aValue )
{
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const std::from_chars_result converted = std::from_chars( begin, end, aValue );
    return converted.ec == std::errc() && converted.ptr == end
           && aValue >= aMinimum && aValue <= aMaximum;
}


bool angle( const std::string& aText, int64_t& aTenths )
{
    long double degrees = 0.0L;
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const std::from_chars_result converted = KICHAD::FromChars( begin, end, degrees );

    if( converted.ec != std::errc() || converted.ptr == begin
        || std::string_view( converted.ptr, static_cast<size_t>( end - converted.ptr ) ) != "deg"
        || !std::isfinite( degrees ) || degrees < -3600.0L || degrees > 3600.0L )
    {
        return false;
    }

    const long double tenths = degrees * 10.0L;
    const int64_t rounded = static_cast<int64_t>( std::round( tenths ) );

    if( std::abs( tenths - rounded ) > 0.0000001L )
        return false;

    aTenths = rounded % 3600;

    if( aTenths < 0 )
        aTenths += 3600;

    return true;
}


bool decimalPpm( const std::string& aText, int64_t aMinimum, int64_t aMaximum,
                 int64_t& aPartsPerMillion )
{
    long double value = 0.0L;
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const std::from_chars_result converted = KICHAD::FromChars( begin, end, value );

    if( converted.ec != std::errc() || converted.ptr != end || !std::isfinite( value ) )
        return false;

    const long double scaled = value * 1'000'000.0L;
    const int64_t rounded = static_cast<int64_t>( std::round( scaled ) );

    if( rounded < aMinimum || rounded > aMaximum
        || std::abs( scaled - rounded ) > 0.0000001L )
    {
        return false;
    }

    aPartsPerMillion = rounded;
    return true;
}


bool color( const DOCUMENT& aDocument, size_t aNode, JSON& aColor )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string value;

    if( node.children.size() == 2 && scalar( aDocument, node.children[1], value )
        && value == "default" )
    {
        aColor = nullptr;
        return true;
    }

    std::string redText;
    std::string greenText;
    std::string blueText;
    std::string alphaText;
    int64_t red = 0;
    int64_t green = 0;
    int64_t blue = 0;
    int64_t alpha = 0;

    if( node.children.size() != 5 || !scalar( aDocument, node.children[1], redText )
        || !scalar( aDocument, node.children[2], greenText )
        || !scalar( aDocument, node.children[3], blueText )
        || !scalar( aDocument, node.children[4], alphaText )
        || !integer( redText, 0, 255, red ) || !integer( greenText, 0, 255, green )
        || !integer( blueText, 0, 255, blue )
        || !decimalPpm( alphaText, 0, 1'000'000, alpha ) )
    {
        return false;
    }

    aColor = { { "red", red }, { "green", green }, { "blue", blue },
               { "alphaPpm", alpha } };
    return true;
}


bool validHyperlink( const std::string& aValue )
{
    if( aValue == "none" )
        return true;

    if( aValue.empty() || aValue.size() > 4096 || aValue.find( '\0' ) != std::string::npos )
        return false;

    if( aValue.front() == '#' )
        return aValue.size() > 1;

    const size_t colon = aValue.find( ':' );

    if( colon == std::string::npos || colon == 0
        || !std::isalpha( static_cast<unsigned char>( aValue.front() ) ) )
    {
        return false;
    }

    for( size_t index = 1; index < colon; ++index )
    {
        const unsigned char character = aValue[index];

        if( !( std::isalnum( character ) || character == '+' || character == '-'
               || character == '.' ) )
        {
            return false;
        }
    }

    return aValue.find_first_of( "\r\n\t " ) == std::string::npos;
}


JSON defaultLayout( const std::string& aRole )
{
    const bool visible = aRole == "reference" || aRole == "value";
    const int64_t y = aRole == "reference" ? -2'540'000
                    : aRole == "value" ? 2'540'000 : 0;
    return {
        { "position", { { "xNm", 0 }, { "yNm", y } } }, { "rotationTenths", 0 },
        { "visible", visible }, { "showName", false }, { "autoplace", true },
        { "private", false }, { "widthNm", 1'270'000 }, { "heightNm", 1'270'000 },
        { "font", "" }, { "lineSpacingPpm", 1'000'000 }, { "thicknessNm", nullptr },
        { "color", nullptr }, { "horizontal", "center" }, { "vertical", "center" },
        { "bold", false }, { "italic", false }, { "hyperlink", "" }
    };
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_SYMBOL_FIELD_COMPILER::RESULT DESIGN_SCRIPT_SYMBOL_FIELD_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    const std::string head = aDocument.ListHead( aNode );
    const bool custom = head == "property";
    std::string name;
    std::string value;
    const size_t settingsStart = custom ? 3 : 2;

    if( ( custom && ( node.children.size() < 3
                      || !scalar( aDocument, node.children[1], name )
                      || !scalar( aDocument, node.children[2], value ) ) )
        || ( !custom && ( node.children.size() < 2
                          || !scalar( aDocument, node.children[1], value ) ) ) )
    {
        diagnostic( result, "invalid_authored_symbol_field",
                    "symbol field requires a name/value and optional complete layout" );
        return result;
    }

    if( !custom )
        name = head;

    if( name.empty() || name.size() > 128 || name.find( '\0' ) != std::string::npos
        || value.size() > MAX_FIELD_TEXT_BYTES || value.find( '\0' ) != std::string::npos )
    {
        diagnostic( result, "invalid_authored_symbol_field",
                    "symbol field name/value is empty, unsafe, or exceeds its bound" );
        return result;
    }

    result.field = { { "name", name }, { "value", value },
                     { "layout", defaultLayout( custom ? "property" : head ) } };
    std::set<std::string> fields;

    for( size_t index = settingsStart; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const DOCUMENT::NODE& setting = aDocument.Nodes().at( child );
        const std::string settingHead = aDocument.ListHead( child );

        if( !fields.emplace( settingHead ).second )
        {
            diagnostic( result, "duplicate_authored_symbol_field_layout_setting",
                        "symbol field " + name + " setting " + settingHead
                                + " occurs more than once" );
            continue;
        }

        if( settingHead == "at" || settingHead == "size" )
        {
            std::string firstText;
            std::string secondText;
            int64_t first = 0;
            int64_t second = 0;
            const bool position = settingHead == "at";

            if( setting.children.size() != 3
                || !scalar( aDocument, setting.children[1], firstText )
                || !scalar( aDocument, setting.children[2], secondText )
                || !distance( firstText, first ) || !distance( secondText, second )
                || ( !position && ( first < 10'000 || second < 10'000
                                     || first > 100'000'000 || second > 100'000'000 ) ) )
            {
                diagnostic( result, "invalid_authored_symbol_field_" + settingHead,
                            "symbol field " + name + " has invalid bounded " + settingHead );
            }
            else if( position )
            {
                result.field["layout"]["position"] = { { "xNm", first }, { "yNm", second } };
            }
            else
            {
                result.field["layout"]["widthNm"] = first;
                result.field["layout"]["heightNm"] = second;
            }

            continue;
        }

        if( settingHead == "color" )
        {
            JSON parsed;

            if( !color( aDocument, child, parsed ) )
                diagnostic( result, "invalid_authored_symbol_field_color",
                            "symbol field color requires default or bounded RGBA" );
            else
                result.field["layout"]["color"] = std::move( parsed );

            continue;
        }

        if( settingHead == "justify" )
        {
            std::string horizontal;
            std::string vertical;
            static const std::set<std::string> horizontalValues = { "left", "center", "right" };
            static const std::set<std::string> verticalValues = { "top", "center", "bottom" };

            if( setting.children.size() != 3
                || !scalar( aDocument, setting.children[1], horizontal )
                || !scalar( aDocument, setting.children[2], vertical )
                || !horizontalValues.contains( horizontal ) || !verticalValues.contains( vertical ) )
            {
                diagnostic( result, "invalid_authored_symbol_field_justify",
                            "symbol field justify requires horizontal then vertical alignment" );
            }
            else
            {
                result.field["layout"]["horizontal"] = horizontal;
                result.field["layout"]["vertical"] = vertical;
            }

            continue;
        }

        std::string settingValue;

        if( !oneValue( aDocument, child, settingValue ) )
        {
            diagnostic( result, "invalid_authored_symbol_field_layout_setting",
                        "symbol field " + name + " setting " + settingHead
                                + " requires one value" );
            continue;
        }

        if( settingHead == "rotation" )
        {
            int64_t parsed = 0;

            if( !angle( settingValue, parsed ) )
                diagnostic( result, "invalid_authored_symbol_field_rotation",
                            "symbol field rotation requires exact 0.1-degree precision" );
            else
                result.field["layout"]["rotationTenths"] = parsed;
        }
        else if( settingHead == "visible" || settingHead == "show_name"
                 || settingHead == "autoplace" || settingHead == "private"
                 || settingHead == "bold" || settingHead == "italic" )
        {
            bool parsed = false;

            if( !boolean( settingValue, parsed ) )
                diagnostic( result, "invalid_authored_symbol_field_boolean",
                            "symbol field " + settingHead + " must be true or false" );
            else
                result.field["layout"][settingHead == "show_name" ? "showName" : settingHead] = parsed;
        }
        else if( settingHead == "font" )
        {
            if( settingValue.empty() || settingValue.size() > 256
                || settingValue.find_first_of( "\r\n\0" ) != std::string::npos )
            {
                diagnostic( result, "invalid_authored_symbol_field_font",
                            "symbol field font requires stroke or a bounded name" );
            }
            else
            {
                result.field["layout"]["font"] = settingValue == "stroke" ? "" : settingValue;
            }
        }
        else if( settingHead == "line_spacing" )
        {
            int64_t parsed = 0;

            if( !decimalPpm( settingValue, 100'000, 10'000'000, parsed ) )
                diagnostic( result, "invalid_authored_symbol_field_line_spacing",
                            "symbol field line_spacing must be 0.1 through 10" );
            else
                result.field["layout"]["lineSpacingPpm"] = parsed;
        }
        else if( settingHead == "thickness" )
        {
            int64_t parsed = 0;

            if( settingValue == "auto" )
                result.field["layout"]["thicknessNm"] = nullptr;
            else if( !distance( settingValue, parsed ) || parsed < 1'000
                     || parsed > 10'000'000 )
                diagnostic( result, "invalid_authored_symbol_field_thickness",
                            "symbol field thickness requires auto or 0.001 to 10 mm" );
            else
                result.field["layout"]["thicknessNm"] = parsed;
        }
        else if( settingHead == "hyperlink" )
        {
            if( !validHyperlink( settingValue ) )
                diagnostic( result, "invalid_authored_symbol_field_hyperlink",
                            "symbol field hyperlink must be none, an absolute URI, or #sheet" );
            else
                result.field["layout"]["hyperlink"] = settingValue == "none" ? "" : settingValue;
        }
        else
        {
            diagnostic( result, "unknown_authored_symbol_field_layout_setting",
                        "symbol field layout setting " + settingHead + " is not supported" );
        }
    }

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
