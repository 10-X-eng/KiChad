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

#include "design_script_symbol_text_compiler.h"
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
using RESULT = KICHAD::DESIGN_SCRIPT_SYMBOL_TEXT_COMPILER::RESULT;

constexpr int64_t MAX_COORDINATE_NM = 2'000'000'000LL;
constexpr size_t MAX_TEXT_BYTES = 16 * 1024;


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


bool identifier( const std::string& aValue )
{
    if( aValue.empty() || aValue.size() > 128 )
        return false;

    for( unsigned char character : aValue )
    {
        if( !( std::isalnum( character ) || character == '_' || character == '-'
               || character == '.' || character == '+' ) )
        {
            return false;
        }
    }

    return true;
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


bool point( const DOCUMENT& aDocument, size_t aNode, JSON& aPoint )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string xText;
    std::string yText;
    int64_t x = 0;
    int64_t y = 0;

    if( node.kind != DOCUMENT::NODE_KIND::LIST || node.children.size() != 3
        || !scalar( aDocument, node.children[1], xText )
        || !scalar( aDocument, node.children[2], yText )
        || !distance( xText, x ) || !distance( yText, y ) )
    {
        return false;
    }

    aPoint = { { "xNm", x }, { "yNm", y } };
    return true;
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


bool color( const DOCUMENT& aDocument, size_t aNode, JSON& aColor,
            bool aAllowDefault = false )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string value;

    if( aAllowDefault && node.children.size() == 2
        && scalar( aDocument, node.children[1], value ) && value == "default" )
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

    if( node.kind != DOCUMENT::NODE_KIND::LIST || node.children.size() != 5
        || !scalar( aDocument, node.children[1], redText )
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


bool compileStroke( const DOCUMENT& aDocument, size_t aNode, JSON& aStroke )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string widthText;
    std::string style;
    int64_t width = 0;
    static const std::set<std::string> styles = {
        "default", "solid", "dash", "dot", "dash_dot", "dash_dot_dot"
    };

    if( node.children.size() < 3 || node.children.size() > 4
        || !scalar( aDocument, node.children[1], widthText )
        || !scalar( aDocument, node.children[2], style ) || !distance( widthText, width )
        || width < 0 || width > 10'000'000 || !styles.contains( style ) )
    {
        return false;
    }

    aStroke = { { "widthNm", width }, { "style", style } };

    if( node.children.size() == 4 )
    {
        JSON parsedColor;

        if( aDocument.ListHead( node.children[3] ) != "color"
            || !color( aDocument, node.children[3], parsedColor ) )
        {
            return false;
        }

        aStroke["color"] = std::move( parsedColor );
    }

    return true;
}


bool compileFill( const DOCUMENT& aDocument, size_t aNode, JSON& aFill )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string type;
    static const std::set<std::string> types = {
        "none", "outline", "background", "color", "hatch", "reverse_hatch", "cross_hatch"
    };

    if( node.children.size() < 2 || node.children.size() > 3
        || !scalar( aDocument, node.children[1], type ) || !types.contains( type ) )
    {
        return false;
    }

    aFill = { { "type", type } };

    if( node.children.size() == 3 )
    {
        JSON parsedColor;

        if( aDocument.ListHead( node.children[2] ) != "color"
            || !color( aDocument, node.children[2], parsedColor ) )
        {
            return false;
        }

        aFill["color"] = std::move( parsedColor );
    }

    return type != "color" || aFill.contains( "color" );
}


bool validHyperlink( const std::string& aValue )
{
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


bool compileTextField( const DOCUMENT& aDocument, size_t aNode, JSON& aItem,
                       RESULT& aResult, const std::string& aKind, const std::string& aId )
{
    const DOCUMENT::NODE& field = aDocument.Nodes().at( aNode );
    const std::string head = aDocument.ListHead( aNode );

    if( head == "at" )
    {
        JSON parsed;

        if( !point( aDocument, aNode, parsed ) )
            diagnostic( aResult, "invalid_authored_symbol_text_position",
                        aKind + " " + aId + " at requires two bounded coordinates" );
        else
            aItem["at"] = std::move( parsed );

        return true;
    }

    if( head == "size" )
    {
        std::string widthText;
        std::string heightText;
        int64_t width = 0;
        int64_t height = 0;

        if( field.children.size() != 3 || !scalar( aDocument, field.children[1], widthText )
            || !scalar( aDocument, field.children[2], heightText )
            || !distance( widthText, width ) || !distance( heightText, height )
            || width < 10'000 || width > 100'000'000
            || height < 10'000 || height > 100'000'000 )
        {
            diagnostic( aResult, "invalid_authored_symbol_text_size",
                        aKind + " " + aId + " size requires width and height from 0.01 to 100 mm" );
        }
        else
        {
            aItem["effects"]["widthNm"] = width;
            aItem["effects"]["heightNm"] = height;
        }

        return true;
    }

    if( head == "justify" )
    {
        std::string horizontal;
        std::string vertical;
        static const std::set<std::string> horizontalValues = { "left", "center", "right" };
        static const std::set<std::string> verticalValues = { "top", "center", "bottom" };

        if( field.children.size() != 3
            || !scalar( aDocument, field.children[1], horizontal )
            || !scalar( aDocument, field.children[2], vertical )
            || !horizontalValues.contains( horizontal ) || !verticalValues.contains( vertical ) )
        {
            diagnostic( aResult, "invalid_authored_symbol_text_justify",
                        aKind + " " + aId + " justify requires horizontal then vertical alignment" );
        }
        else
        {
            aItem["effects"]["horizontal"] = horizontal;
            aItem["effects"]["vertical"] = vertical;
        }

        return true;
    }

    if( head == "color" )
    {
        JSON parsed;

        if( !color( aDocument, aNode, parsed, true ) )
            diagnostic( aResult, "invalid_authored_symbol_text_color",
                        aKind + " " + aId + " color requires default or bounded RGBA" );
        else
            aItem["effects"]["color"] = std::move( parsed );

        return true;
    }

    std::string value;

    if( !oneValue( aDocument, aNode, value ) )
        return false;

    if( head == "private" )
    {
        bool parsed = false;

        if( !boolean( value, parsed ) )
            diagnostic( aResult, "invalid_authored_symbol_text_private",
                        aKind + " " + aId + " private must be true or false" );
        else
            aItem["private"] = parsed;
    }
    else if( head == "rotation" )
    {
        int64_t parsed = 0;

        if( !angle( value, parsed ) )
            diagnostic( aResult, "invalid_authored_symbol_text_rotation",
                        aKind + " " + aId + " rotation requires exact 0.1-degree precision" );
        else
            aItem["rotationTenths"] = parsed;
    }
    else if( head == "font" )
    {
        if( value.empty() || value.size() > 256
            || value.find_first_of( "\r\n\0" ) != std::string::npos )
            diagnostic( aResult, "invalid_authored_symbol_text_font",
                        aKind + " " + aId + " font is unsafe or too long" );
        else
            aItem["effects"]["font"] = value == "stroke" ? "" : value;
    }
    else if( head == "thickness" )
    {
        int64_t parsed = 0;

        if( value == "auto" )
            aItem["effects"]["thicknessNm"] = nullptr;
        else if( !distance( value, parsed ) || parsed < 1'000 || parsed > 10'000'000 )
            diagnostic( aResult, "invalid_authored_symbol_text_thickness",
                        aKind + " " + aId + " thickness requires auto or 0.001 to 10 mm" );
        else
            aItem["effects"]["thicknessNm"] = parsed;
    }
    else if( head == "bold" || head == "italic" )
    {
        bool parsed = false;

        if( !boolean( value, parsed ) )
            diagnostic( aResult, "invalid_authored_symbol_text_style",
                        aKind + " " + aId + " " + head + " must be true or false" );
        else
            aItem["effects"][head] = parsed;
    }
    else if( head == "line_spacing" )
    {
        int64_t parsed = 0;

        if( !decimalPpm( value, 100'000, 10'000'000, parsed ) )
            diagnostic( aResult, "invalid_authored_symbol_text_line_spacing",
                        aKind + " " + aId + " line_spacing must be 0.1 through 10" );
        else
            aItem["effects"]["lineSpacingPpm"] = parsed;
    }
    else if( head == "hyperlink" )
    {
        if( !validHyperlink( value ) )
            diagnostic( aResult, "invalid_authored_symbol_text_hyperlink",
                        aKind + " " + aId + " hyperlink must be an absolute URI or #sheet" );
        else
            aItem["effects"]["hyperlink"] = value;
    }
    else
    {
        return false;
    }

    return true;
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_SYMBOL_TEXT_COMPILER::IsText( const std::string& aHead )
{
    return aHead == "text" || aHead == "text_box";
}


DESIGN_SCRIPT_SYMBOL_TEXT_COMPILER::RESULT DESIGN_SCRIPT_SYMBOL_TEXT_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    const std::string kind = aDocument.ListHead( aNode );
    result.recognized = IsText( kind );

    if( !result.recognized )
        return result;

    std::string id;
    std::string text;

    if( node.children.size() < 3 || !scalar( aDocument, node.children[1], id )
        || !scalar( aDocument, node.children[2], text ) || !identifier( id )
        || text.size() > MAX_TEXT_BYTES || text.find( '\0' ) != std::string::npos )
    {
        diagnostic( result, "invalid_authored_symbol_text",
                    kind + " requires a bounded logical ID and text up to 16384 UTF-8 bytes" );
        return result;
    }

    result.item = {
        { "kind", kind }, { "id", id }, { "text", text }, { "private", false },
        { "rotationTenths", 0 },
        { "effects",
          { { "font", "" }, { "widthNm", 1'270'000 }, { "heightNm", 1'270'000 },
            { "thicknessNm", nullptr }, { "bold", false }, { "italic", false },
            { "lineSpacingPpm", 1'000'000 }, { "color", nullptr },
            { "horizontal", "center" }, { "vertical", "center" },
            { "hyperlink", "" } } }
    };

    if( kind == "text_box" )
    {
        result.item["marginsNm"] = { 0, 0, 0, 0 };
        result.item["stroke"] = { { "widthNm", 0 }, { "style", "default" } };
        result.item["fill"] = { { "type", "none" } };
    }

    std::set<std::string> fields;

    for( size_t index = 3; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( result, "duplicate_authored_symbol_text_field",
                        kind + " " + id + " field " + head + " occurs more than once" );
            continue;
        }

        if( compileTextField( aDocument, child, result.item, result, kind, id ) )
            continue;

        if( kind == "text_box" && head == "box_size" )
        {
            const DOCUMENT::NODE& field = aDocument.Nodes().at( child );
            std::string widthText;
            std::string heightText;
            int64_t width = 0;
            int64_t height = 0;

            if( field.children.size() != 3
                || !scalar( aDocument, field.children[1], widthText )
                || !scalar( aDocument, field.children[2], heightText )
                || !distance( widthText, width ) || !distance( heightText, height )
                || width <= 0 || height <= 0 )
            {
                diagnostic( result, "invalid_authored_symbol_text_box_size",
                            "text_box " + id + " box_size requires positive width and height" );
            }
            else
            {
                result.item["boxSize"] = { { "widthNm", width }, { "heightNm", height } };
            }
        }
        else if( kind == "text_box" && head == "margins" )
        {
            const DOCUMENT::NODE& field = aDocument.Nodes().at( child );
            JSON margins = JSON::array();
            bool valid = field.children.size() == 5;

            for( size_t margin = 1; valid && margin < field.children.size(); ++margin )
            {
                std::string value;
                int64_t parsed = 0;
                valid = scalar( aDocument, field.children[margin], value )
                        && distance( value, parsed ) && parsed >= 0 && parsed <= 100'000'000;
                margins.push_back( parsed );
            }

            if( !valid )
                diagnostic( result, "invalid_authored_symbol_text_box_margins",
                            "text_box " + id + " margins require left, top, right, bottom distances" );
            else
                result.item["marginsNm"] = std::move( margins );
        }
        else if( kind == "text_box" && head == "stroke" )
        {
            JSON stroke;

            if( !compileStroke( aDocument, child, stroke ) )
                diagnostic( result, "invalid_authored_symbol_text_box_stroke",
                            "text_box " + id + " has an invalid bounded stroke" );
            else
                result.item["stroke"] = std::move( stroke );
        }
        else if( kind == "text_box" && head == "fill" )
        {
            JSON fill;

            if( !compileFill( aDocument, child, fill ) )
                diagnostic( result, "invalid_authored_symbol_text_box_fill",
                            "text_box " + id + " has an invalid fill" );
            else
                result.item["fill"] = std::move( fill );
        }
        else
        {
            diagnostic( result, "unknown_authored_symbol_text_field",
                        kind + " " + id + " has unsupported field " + head );
        }
    }

    if( !result.item.contains( "at" ) )
        diagnostic( result, "missing_authored_symbol_text_position",
                    kind + " " + id + " requires at" );

    if( kind == "text_box" && !result.item.contains( "boxSize" ) )
        diagnostic( result, "missing_authored_symbol_text_box_size",
                    "text_box " + id + " requires box_size" );

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
