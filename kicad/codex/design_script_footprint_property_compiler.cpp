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

#include "design_script_footprint_property_compiler.h"
#include "kichad_from_chars.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_FOOTPRINT_PROPERTY_COMPILER::RESULT;

constexpr int64_t MAX_COORDINATE_NM = 2'000'000'000LL;
constexpr int64_t MAX_TEXT_SIZE_NM = 100'000'000LL;
constexpr size_t MAX_PROPERTY_VALUE_BYTES = 64 * 1024;


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
    return !aValue.empty() && aValue.size() <= 128
           && std::all_of( aValue.begin(), aValue.end(), []( unsigned char aCharacter )
           {
               return std::isalnum( aCharacter ) || aCharacter == '_'
                      || aCharacter == '-' || aCharacter == '.' || aCharacter == '+';
           } );
}


bool oneValue( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalar( aDocument, node.children[1], aValue );
}


bool boolean( const std::string& aText, bool& aValue )
{
    if( aText == "true" || aText == "false" )
    {
        aValue = aText == "true";
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


bool angle( const std::string& aText, int64_t& aTenths )
{
    if( !aText.ends_with( "deg" ) )
        return false;

    const std::string_view number( aText.data(), aText.size() - 3 );
    long double degrees = 0.0L;
    const std::from_chars_result converted =
            KICHAD::FromChars( number.data(), number.data() + number.size(), degrees );
    const long double tenths = std::round( degrees * 10.0L );

    if( converted.ec != std::errc() || converted.ptr != number.data() + number.size()
        || !std::isfinite( degrees ) || tenths < -3600.0L || tenths > 3600.0L
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
    const std::from_chars_result converted = KICHAD::FromChars( begin, end, value );
    const long double ppm = std::round( value * 1'000'000.0L );

    if( converted.ec != std::errc() || converted.ptr != end || !std::isfinite( value )
        || ppm < static_cast<long double>( aMinimum )
        || ppm > static_cast<long double>( aMaximum ) )
    {
        return false;
    }

    aPartsPerMillion = static_cast<int64_t>( ppm );
    return true;
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


bool propertyLayer( const std::string& aLayer )
{
    static const std::set<std::string> fixed = {
        "F.Adhes", "B.Adhes", "F.Paste", "B.Paste", "F.SilkS", "B.SilkS",
        "F.Mask", "B.Mask", "Dwgs.User", "Cmts.User", "Eco1.User",
        "Eco2.User", "Edge.Cuts", "Margin", "F.CrtYd", "B.CrtYd", "F.Fab", "B.Fab"
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
            diagnostic( aResult, "duplicate_authored_footprint_property_font_field",
                        "property font field " + head + " occurs more than once" );
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
                diagnostic( aResult, "invalid_authored_footprint_property_font_size",
                            "property font size requires positive bounded HEIGHT WIDTH distances" );
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
            diagnostic( aResult, "invalid_authored_footprint_property_font_field",
                        "property font field " + head + " requires one value" );
        }
        else if( head == "face" )
        {
            if( value.empty() || value.size() > 256 || value.find( '\0' ) != std::string::npos
                || value.find_first_of( "\r\n" ) != std::string::npos )
            {
                diagnostic( aResult, "invalid_authored_footprint_property_font_face",
                            "property font face requires default or one bounded local name" );
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
                diagnostic( aResult, "invalid_authored_footprint_property_line_spacing",
                            "property font line_spacing must be from 0.1 through 10" );
            else
                font["lineSpacingPpm"] = parsed;
        }
        else if( head == "thickness" )
        {
            int64_t parsed = 0;

            if( value == "auto" )
                font["thicknessNm"] = nullptr;
            else if( !distance( value, parsed ) || parsed < 0 || parsed > MAX_TEXT_SIZE_NM )
                diagnostic( aResult, "invalid_authored_footprint_property_thickness",
                            "property font thickness requires auto or a bounded distance" );
            else
                font["thicknessNm"] = parsed;
        }
        else if( head == "bold" || head == "italic" )
        {
            bool parsed = false;

            if( !boolean( value, parsed ) )
                diagnostic( aResult, "invalid_authored_footprint_property_font_style",
                            "property font bold and italic require true or false" );
            else
                font[head] = parsed;
        }
        else
        {
            diagnostic( aResult, "unknown_authored_footprint_property_font_field",
                        "property font supports face, size, line_spacing, thickness, bold, and italic" );
        }
    }

    if( !fields.contains( "size" ) )
        diagnostic( aResult, "missing_authored_footprint_property_font_size",
                    "property requires one explicit font size" );

    if( font["thicknessNm"].is_number_integer()
        && font["thicknessNm"].get<int64_t>()
                   > std::min( font["size"]["heightNm"].get<int64_t>(),
                               font["size"]["widthNm"].get<int64_t>() ) / 2 )
    {
        diagnostic( aResult, "invalid_authored_footprint_property_thickness",
                    "property font thickness cannot exceed half the shorter text dimension" );
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

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_FOOTPRINT_PROPERTY_COMPILER::RESULT
DESIGN_SCRIPT_FOOTPRINT_PROPERTY_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;

    if( aDocument.ListHead( aNode ) != "property" || node.children.size() < 2
        || !scalar( aDocument, node.children[1], id ) || !identifier( id ) )
    {
        diagnostic( result, "invalid_authored_footprint_property_id",
                    "footprint property requires one unique bounded logical ID" );
        return result;
    }

    result.property = {
        { "id", id }, { "name", "" }, { "value", "" },
        { "at", { { "xNm", 0 }, { "yNm", 0 } } }, { "rotationTenths", 0 },
        { "layer", "" }, { "visible", true }, { "keepUpright", true },
        { "knockout", false }, { "font", JSON::object() }, { "justify", JSON::object() }
    };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( result, "duplicate_authored_footprint_property_field",
                        "footprint property " + id + " field " + head
                                + " occurs more than once" );
            continue;
        }

        if( head == "at" )
        {
            const DOCUMENT::NODE& field = aDocument.Nodes().at( child );
            std::string xText;
            std::string yText;
            int64_t x = 0;
            int64_t y = 0;

            if( field.children.size() != 3
                || !scalar( aDocument, field.children[1], xText )
                || !scalar( aDocument, field.children[2], yText )
                || !distance( xText, x ) || !distance( yText, y ) )
            {
                diagnostic( result, "invalid_authored_footprint_property_position",
                            "property at requires two bounded physical coordinates" );
            }
            else
            {
                result.property["at"] = { { "xNm", x }, { "yNm", y } };
            }

            continue;
        }

        if( head == "font" )
        {
            result.property["font"] = compileFont( aDocument, child, result );
            continue;
        }

        if( head == "justify" )
        {
            if( !compileJustify( aDocument, child, result.property["justify"] ) )
                diagnostic( result, "invalid_authored_footprint_property_justify",
                            "property justify requires HORIZONTAL VERTICAL MIRRORED" );

            continue;
        }

        std::string value;

        if( !oneValue( aDocument, child, value ) )
        {
            diagnostic( result, "invalid_authored_footprint_property_field",
                        "footprint property field " + head + " requires one value" );
        }
        else if( head == "name" )
        {
            if( value.empty() || value.size() > 256 || value.find( '\0' ) != std::string::npos
                || value.find_first_of( "\r\n" ) != std::string::npos )
            {
                diagnostic( result, "invalid_authored_footprint_property_name",
                            "property name requires 1 through 256 bounded UTF-8 bytes" );
            }
            else
            {
                result.property["name"] = value;
            }
        }
        else if( head == "value" )
        {
            if( value.size() > MAX_PROPERTY_VALUE_BYTES
                || value.find( '\0' ) != std::string::npos )
            {
                diagnostic( result, "invalid_authored_footprint_property_value",
                            "property value exceeds 64 KiB or contains NUL" );
            }
            else
            {
                result.property["value"] = value;
            }
        }
        else if( head == "rotation" )
        {
            int64_t parsed = 0;

            if( !angle( value, parsed ) )
                diagnostic( result, "invalid_authored_footprint_property_rotation",
                            "property rotation requires exact 0.1-degree units" );
            else
                result.property["rotationTenths"] = parsed;
        }
        else if( head == "layer" )
        {
            if( !propertyLayer( value ) )
                diagnostic( result, "invalid_authored_footprint_property_layer",
                            "property layer requires one supported footprint text layer" );
            else
                result.property["layer"] = value;
        }
        else if( head == "visible" || head == "keep_upright" || head == "knockout" )
        {
            bool parsed = false;

            if( !boolean( value, parsed ) )
                diagnostic( result, "invalid_authored_footprint_property_boolean",
                            "property boolean fields require true or false" );
            else
                result.property[head == "keep_upright" ? "keepUpright" : head] = parsed;
        }
        else
        {
            diagnostic( result, "unknown_authored_footprint_property_field",
                        "unsupported footprint property field " + head );
        }
    }

    for( const char* required : { "name", "value", "at", "rotation", "layer", "visible",
                                  "keep_upright", "knockout", "font", "justify" } )
    {
        if( !fields.contains( required ) )
            diagnostic( result, "missing_authored_footprint_property_field",
                        "footprint property " + id + " requires explicit " + required );
    }

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
