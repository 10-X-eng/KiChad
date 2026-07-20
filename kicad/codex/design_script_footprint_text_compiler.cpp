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

#include "design_script_footprint_text_compiler.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_FOOTPRINT_TEXT_COMPILER::RESULT;

constexpr int64_t MAX_COORDINATE_NM = 2'000'000'000LL;
constexpr int64_t MAX_TEXT_SIZE_NM = 100'000'000LL;
constexpr size_t MAX_TEXT_BYTES = 64 * 1024;
constexpr size_t MAX_TEXT_BOX_POINTS = 4096;


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
    const std::from_chars_result converted = std::from_chars( begin, end, value );

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


bool angle( const std::string& aText, int64_t& aTenths )
{
    if( !aText.ends_with( "deg" ) )
        return false;

    const std::string_view number( aText.data(), aText.size() - 3 );
    long double degrees = 0.0L;
    const std::from_chars_result converted =
            std::from_chars( number.data(), number.data() + number.size(), degrees );

    if( converted.ec != std::errc() || converted.ptr != number.data() + number.size()
        || !std::isfinite( degrees ) )
    {
        return false;
    }

    const long double tenths = std::round( degrees * 10.0L );

    if( tenths < -3600.0L || tenths > 3600.0L
        || std::fabs( tenths - degrees * 10.0L ) > 0.000001L )
    {
        return false;
    }

    aTenths = static_cast<int64_t>( tenths );
    return true;
}


bool decimalPpm( const std::string& aText, int64_t aMinimum, int64_t aMaximum,
                 int64_t& aPartsPerMillion )
{
    long double value = 0.0L;
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const std::from_chars_result converted = std::from_chars( begin, end, value );

    if( converted.ec != std::errc() || converted.ptr != end || !std::isfinite( value ) )
        return false;

    const long double ppm = std::round( value * 1'000'000.0L );

    if( ppm < static_cast<long double>( aMinimum )
        || ppm > static_cast<long double>( aMaximum ) )
    {
        return false;
    }

    aPartsPerMillion = static_cast<int64_t>( ppm );
    return true;
}


bool oneValue( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalar( aDocument, node.children[1], aValue );
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


bool samePoint( const JSON& aLeft, const JSON& aRight )
{
    return aLeft["xNm"] == aRight["xNm"] && aLeft["yNm"] == aRight["yNm"];
}


bool userLayer( const std::string& aLayer )
{
    if( !aLayer.starts_with( "User." ) )
        return false;

    const std::string_view number( aLayer.data() + 5, aLayer.size() - 5 );
    int index = 0;
    const std::from_chars_result parsed =
            std::from_chars( number.data(), number.data() + number.size(), index );
    return parsed.ec == std::errc() && parsed.ptr == number.data() + number.size()
           && index >= 1 && index <= 45;
}


bool layer( const std::string& aLayer )
{
    static const std::set<std::string> fixed = {
        "F.Cu", "B.Cu", "F.Adhes", "B.Adhes", "F.Paste", "B.Paste",
        "F.SilkS", "B.SilkS", "F.Mask", "B.Mask", "Dwgs.User",
        "Cmts.User", "Eco1.User", "Eco2.User", "Edge.Cuts", "Margin",
        "F.CrtYd", "B.CrtYd", "F.Fab", "B.Fab"
    };
    return fixed.contains( aLayer ) || userLayer( aLayer );
}


JSON compileFont( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    JSON font = {
        { "face", "default" },
        { "size", { { "heightNm", 0 }, { "widthNm", 0 } } },
        { "lineSpacingPpm", 1'000'000 }, { "thicknessNm", nullptr },
        { "bold", false }, { "italic", false }
    };
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::set<std::string> fields;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_authored_footprint_text_font_field",
                        "footprint text font field " + head + " occurs more than once" );
            continue;
        }

        const DOCUMENT::NODE& field = aDocument.Nodes().at( child );

        if( head == "size" )
        {
            std::string heightText;
            std::string widthText;
            int64_t height = 0;
            int64_t width = 0;

            if( field.children.size() != 3
                || !scalar( aDocument, field.children[1], heightText )
                || !scalar( aDocument, field.children[2], widthText )
                || !distance( heightText, height ) || !distance( widthText, width )
                || height <= 0 || width <= 0 || height > MAX_TEXT_SIZE_NM
                || width > MAX_TEXT_SIZE_NM )
            {
                diagnostic( aResult, "invalid_authored_footprint_text_size",
                            "font size requires positive bounded HEIGHT WIDTH distances" );
            }
            else
            {
                font["size"] = { { "heightNm", height }, { "widthNm", width } };
            }

            continue;
        }

        std::string value;

        if( !oneValue( aDocument, child, value ) )
        {
            diagnostic( aResult, "invalid_authored_footprint_text_font_field",
                        "font field " + head + " requires one value" );
            continue;
        }

        if( head == "face" )
        {
            if( value.empty() || value.size() > 256 || value.find( '\0' ) != std::string::npos
                || value.find_first_of( "\r\n" ) != std::string::npos )
            {
                diagnostic( aResult, "invalid_authored_footprint_text_font_face",
                            "font face requires default or one bounded local font name" );
            }
            else
            {
                font["face"] = value;
            }
        }
        else if( head == "line_spacing" )
        {
            int64_t parsed = 0;

            if( !decimalPpm( value, 100'000, 10'000'000, parsed ) )
                diagnostic( aResult, "invalid_authored_footprint_text_line_spacing",
                            "font line_spacing must be from 0.1 through 10" );
            else
                font["lineSpacingPpm"] = parsed;
        }
        else if( head == "thickness" )
        {
            int64_t parsed = 0;

            if( value == "auto" )
                font["thicknessNm"] = nullptr;
            else if( !distance( value, parsed ) || parsed < 0 || parsed > MAX_TEXT_SIZE_NM )
                diagnostic( aResult, "invalid_authored_footprint_text_thickness",
                            "font thickness requires auto or a non-negative bounded distance" );
            else
                font["thicknessNm"] = parsed;
        }
        else if( head == "bold" || head == "italic" )
        {
            bool parsed = false;

            if( !boolean( value, parsed ) )
                diagnostic( aResult, "invalid_authored_footprint_text_font_style",
                            "font bold and italic require true or false" );
            else
                font[head] = parsed;
        }
        else
        {
            diagnostic( aResult, "unknown_authored_footprint_text_font_field",
                        "font supports face, size, line_spacing, thickness, bold, and italic" );
        }
    }

    if( !fields.contains( "size" ) )
        diagnostic( aResult, "missing_authored_footprint_text_size",
                    "footprint text requires one explicit font size" );

    if( font["thicknessNm"].is_number_integer()
        && font["thicknessNm"].get<int64_t>()
                   > std::min( font["size"]["heightNm"].get<int64_t>(),
                               font["size"]["widthNm"].get<int64_t>() ) / 2 )
    {
        diagnostic( aResult, "invalid_authored_footprint_text_thickness",
                    "font thickness cannot exceed half the shorter text dimension" );
    }

    return font;
}


bool compileJustify( const DOCUMENT& aDocument, size_t aNode, JSON& aJustify )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string horizontal;
    std::string vertical;
    std::string mirroredText;
    bool mirrored = false;
    static const std::set<std::string> horizontalValues = { "left", "center", "right" };
    static const std::set<std::string> verticalValues = { "top", "center", "bottom" };

    if( node.children.size() != 4
        || !scalar( aDocument, node.children[1], horizontal )
        || !scalar( aDocument, node.children[2], vertical )
        || !scalar( aDocument, node.children[3], mirroredText )
        || !horizontalValues.contains( horizontal ) || !verticalValues.contains( vertical )
        || !boolean( mirroredText, mirrored ) )
    {
        return false;
    }

    aJustify = { { "horizontal", horizontal }, { "vertical", vertical },
                 { "mirrored", mirrored } };
    return true;
}


bool compileStroke( const DOCUMENT& aDocument, size_t aNode, JSON& aStroke )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string widthText;
    std::string style;
    int64_t width = 0;
    static const std::set<std::string> styles = {
        "solid", "dash", "dash_dot", "dash_dot_dot", "dot"
    };

    if( node.children.size() != 3 || !scalar( aDocument, node.children[1], widthText )
        || !distance( widthText, width ) || width < 0 || width > MAX_TEXT_SIZE_NM
        || !scalar( aDocument, node.children[2], style ) || !styles.contains( style ) )
    {
        return false;
    }

    aStroke = { { "widthNm", width }, { "style", style } };
    return true;
}


bool compileMargins( const DOCUMENT& aDocument, size_t aNode, JSON& aMargins )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );

    if( node.children.size() != 5 )
        return false;

    static const char* names[] = { "leftNm", "topNm", "rightNm", "bottomNm" };

    for( size_t index = 0; index < 4; ++index )
    {
        std::string text;
        int64_t value = 0;

        if( !scalar( aDocument, node.children[index + 1], text ) || !distance( text, value )
            || value < 0 || value > MAX_TEXT_SIZE_NM )
        {
            return false;
        }

        aMargins[names[index]] = value;
    }

    return true;
}


bool compileBox( const DOCUMENT& aDocument, size_t aNode, JSON& aText )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string values[4];
    int64_t parsed[4] = {};

    if( node.children.size() != 5 )
        return false;

    for( size_t index = 0; index < 4; ++index )
    {
        if( !scalar( aDocument, node.children[index + 1], values[index] )
            || !distance( values[index], parsed[index] ) )
        {
            return false;
        }
    }

    if( parsed[0] == parsed[2] || parsed[1] == parsed[3] )
        return false;

    aText["outline"] = "rectangle";
    aText["start"] = { { "xNm", parsed[0] }, { "yNm", parsed[1] } };
    aText["end"] = { { "xNm", parsed[2] }, { "yNm", parsed[3] } };
    return true;
}


bool compilePolygon( const DOCUMENT& aDocument, size_t aNode, JSON& aText )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    JSON points = JSON::array();

    if( node.children.size() < 4 || node.children.size() > MAX_TEXT_BOX_POINTS + 1 )
        return false;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        if( aDocument.ListHead( node.children[index] ) != "point" )
            return false;

        JSON parsed;

        if( !point( aDocument, node.children[index], parsed ) )
            return false;

        if( !points.empty() && samePoint( points.back(), parsed ) )
            return false;

        points.push_back( std::move( parsed ) );
    }

    if( samePoint( points.front(), points.back() ) )
        return false;

    long double area = 0.0L;

    for( size_t index = 0; index < points.size(); ++index )
    {
        const JSON& current = points[index];
        const JSON& next = points[( index + 1 ) % points.size()];
        area += static_cast<long double>( current["xNm"].get<int64_t>() )
                        * static_cast<long double>( next["yNm"].get<int64_t>() )
                - static_cast<long double>( next["xNm"].get<int64_t>() )
                        * static_cast<long double>( current["yNm"].get<int64_t>() );
    }

    if( area == 0.0L )
        return false;

    aText["outline"] = "polygon";
    aText["points"] = std::move( points );
    return true;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_FOOTPRINT_TEXT_COMPILER::RESULT
DESIGN_SCRIPT_FOOTPRINT_TEXT_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    const std::string kind = aDocument.ListHead( aNode );
    std::string id;
    std::string content;

    if( ( kind != "text" && kind != "text_box" ) || node.children.size() < 3
        || !scalar( aDocument, node.children[1], id ) || !identifier( id )
        || !scalar( aDocument, node.children[2], content ) || content.size() > MAX_TEXT_BYTES
        || content.find( '\0' ) != std::string::npos )
    {
        diagnostic( result, "invalid_authored_footprint_text_header",
                    "footprint text requires a supported kind, unique bounded ID, and bounded content" );
        return result;
    }

    result.text = {
        { "id", id }, { "kind", kind }, { "content", content }, { "rotationTenths", 0 },
        { "layer", "" }, { "locked", false }, { "keepUpright", true },
        { "knockout", false }, { "font", JSON::object() }, { "justify", JSON::object() }
    };

    if( kind == "text_box" )
    {
        result.text["outline"] = "";
        result.text["points"] = JSON::array();
        result.text["margins"] = JSON::object();
        result.text["border"] = true;
        result.text["stroke"] = JSON::object();
    }

    std::set<std::string> fields;

    for( size_t index = 3; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( result, "duplicate_authored_footprint_text_field",
                        "footprint " + kind + " " + id + " field " + head
                                + " occurs more than once" );
            continue;
        }

        if( head == "at" && kind == "text" )
        {
            if( !point( aDocument, child, result.text["at"] ) )
                diagnostic( result, "invalid_authored_footprint_text_position",
                            "footprint text at requires two bounded coordinates" );

            continue;
        }

        if( head == "font" )
        {
            result.text["font"] = compileFont( aDocument, child, result );
            continue;
        }

        if( head == "justify" )
        {
            if( !compileJustify( aDocument, child, result.text["justify"] ) )
                diagnostic( result, "invalid_authored_footprint_text_justify",
                            "justify requires HORIZONTAL VERTICAL MIRRORED" );

            continue;
        }

        if( kind == "text_box" && head == "box" )
        {
            if( !compileBox( aDocument, child, result.text ) )
                diagnostic( result, "invalid_authored_footprint_text_box_outline",
                            "text box box requires X1 Y1 X2 Y2 with non-zero area" );

            continue;
        }

        if( kind == "text_box" && head == "polygon" )
        {
            if( !compilePolygon( aDocument, child, result.text ) )
                diagnostic( result, "invalid_authored_footprint_text_box_outline",
                            "text box polygon requires 3 through 4096 non-degenerate points" );

            continue;
        }

        if( kind == "text_box" && head == "margins" )
        {
            if( !compileMargins( aDocument, child, result.text["margins"] ) )
                diagnostic( result, "invalid_authored_footprint_text_box_margins",
                            "text box margins require non-negative LEFT TOP RIGHT BOTTOM distances" );

            continue;
        }

        if( kind == "text_box" && head == "stroke" )
        {
            if( !compileStroke( aDocument, child, result.text["stroke"] ) )
                diagnostic( result, "invalid_authored_footprint_text_box_stroke",
                            "text box stroke requires WIDTH and a supported line style" );

            continue;
        }

        std::string value;

        if( !oneValue( aDocument, child, value ) )
        {
            diagnostic( result, "invalid_authored_footprint_text_field",
                        "footprint text field " + head + " requires one value" );
            continue;
        }

        if( head == "rotation" )
        {
            int64_t parsed = 0;

            if( !angle( value, parsed ) )
                diagnostic( result, "invalid_authored_footprint_text_rotation",
                            "text rotation requires a bounded tenth-degree angle" );
            else
                result.text["rotationTenths"] = parsed;
        }
        else if( head == "layer" )
        {
            if( !::layer( value ) )
                diagnostic( result, "invalid_authored_footprint_text_layer",
                            "text layer requires one supported KiCad footprint layer" );
            else
                result.text["layer"] = value;
        }
        else if( head == "locked" || head == "knockout"
                 || ( head == "keep_upright" && kind == "text" )
                 || ( head == "border" && kind == "text_box" ) )
        {
            bool parsed = false;

            if( !boolean( value, parsed ) )
                diagnostic( result, "invalid_authored_footprint_text_boolean",
                            "text boolean fields require true or false" );
            else
                result.text[head == "keep_upright" ? "keepUpright" : head] = parsed;
        }
        else
        {
            diagnostic( result, "unknown_authored_footprint_text_field",
                        "unsupported footprint " + kind + " field " + head );
        }
    }

    if( result.text["layer"].get<std::string>().empty() )
        diagnostic( result, "missing_authored_footprint_text_layer",
                    "footprint " + kind + " requires one explicit layer" );

    if( result.text["font"].empty() )
        diagnostic( result, "missing_authored_footprint_text_font",
                    "footprint " + kind + " requires one explicit font" );

    if( result.text["justify"].empty() )
        diagnostic( result, "missing_authored_footprint_text_justify",
                    "footprint " + kind + " requires one explicit justify form" );

    if( kind == "text" && !result.text.contains( "at" ) )
        diagnostic( result, "missing_authored_footprint_text_position",
                    "footprint text requires one explicit at position" );

    if( kind == "text_box" )
    {
        if( fields.contains( "box" ) && fields.contains( "polygon" ) )
            diagnostic( result, "duplicate_authored_footprint_text_box_outline",
                        "footprint text_box accepts exactly one box or polygon outline" );

        if( result.text["outline"].get<std::string>().empty() )
            diagnostic( result, "missing_authored_footprint_text_box_outline",
                        "footprint text_box requires exactly one box or polygon outline" );

        if( result.text["margins"].empty() )
            diagnostic( result, "missing_authored_footprint_text_box_margins",
                        "footprint text_box requires explicit four-sided margins" );

        if( result.text["stroke"].empty() )
            diagnostic( result, "missing_authored_footprint_text_box_stroke",
                        "footprint text_box requires one explicit border stroke" );
    }

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
