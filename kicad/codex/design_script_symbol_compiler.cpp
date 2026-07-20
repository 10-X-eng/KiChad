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

#include "design_script_symbol_compiler.h"

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
using RESULT = KICHAD::DESIGN_SCRIPT_SYMBOL_COMPILER::RESULT;

constexpr size_t MAX_TEXT_BYTES = 4096;
constexpr size_t MAX_SYMBOL_PROPERTIES = 256;
constexpr size_t MAX_SYMBOL_UNITS = 256;
constexpr size_t MAX_UNIT_ITEMS = 4096;
constexpr int64_t MAX_COORDINATE_NM = 2'000'000'000LL;


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


bool libraryId( const std::string& aValue, std::string& aLibrary, std::string& aName )
{
    const size_t separator = aValue.find( ':' );

    if( separator == std::string::npos || aValue.find( ':', separator + 1 ) != std::string::npos )
        return false;

    aLibrary = aValue.substr( 0, separator );
    aName = aValue.substr( separator + 1 );
    return identifier( aLibrary ) && identifier( aName );
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


bool integer( const std::string& aText, int64_t aMinimum, int64_t aMaximum, int64_t& aValue )
{
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const std::from_chars_result converted = std::from_chars( begin, end, aValue );
    return converted.ec == std::errc() && converted.ptr == end
           && aValue >= aMinimum && aValue <= aMaximum;
}


bool point( const DOCUMENT& aDocument, size_t aNode, int64_t& aX, int64_t& aY )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string x;
    std::string y;
    return node.children.size() == 3 && scalar( aDocument, node.children[1], x )
           && scalar( aDocument, node.children[2], y ) && distance( x, aX )
           && distance( y, aY );
}


JSON compileRectangle( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult,
                       std::set<std::string>& aIds )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], id )
        || !identifier( id ) )
    {
        diagnostic( aResult, "invalid_authored_symbol_rectangle",
                    "symbol rectangle requires a bounded logical ID" );
        return JSON::object();
    }

    if( !aIds.emplace( id ).second )
    {
        diagnostic( aResult, "duplicate_authored_symbol_item",
                    "symbol unit item ID " + id + " occurs more than once" );
    }

    JSON rectangle = { { "kind", "rectangle" }, { "id", id },
                       { "stroke", { { "widthNm", 0 }, { "style", "default" } } },
                       { "fill", "none" } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_authored_symbol_rectangle_field",
                        "rectangle " + id + " field " + head + " occurs more than once" );
            continue;
        }

        if( head == "from" || head == "to" )
        {
            int64_t x = 0;
            int64_t y = 0;

            if( !point( aDocument, child, x, y ) )
            {
                diagnostic( aResult, "invalid_authored_symbol_rectangle_point",
                            "rectangle " + id + " " + head
                                    + " requires two coordinates between -2 m and 2 m" );
                continue;
            }

            rectangle[head] = { { "xNm", x }, { "yNm", y } };
        }
        else if( head == "stroke" )
        {
            const DOCUMENT::NODE& stroke = aDocument.Nodes().at( child );
            std::string widthText;
            std::string style;
            int64_t width = 0;
            static const std::set<std::string> styles = {
                "default", "solid", "dash", "dot", "dash_dot", "dash_dot_dot"
            };

            if( stroke.children.size() != 3
                || !scalar( aDocument, stroke.children[1], widthText )
                || !scalar( aDocument, stroke.children[2], style )
                || !distance( widthText, width ) || width < 0 || width > 10'000'000
                || !styles.contains( style ) )
            {
                diagnostic( aResult, "invalid_authored_symbol_rectangle_stroke",
                            "rectangle " + id
                                    + " stroke requires 0..10 mm and a supported line style" );
                continue;
            }

            rectangle["stroke"] = { { "widthNm", width }, { "style", style } };
        }
        else if( head == "fill" )
        {
            std::string fill;

            if( !oneValue( aDocument, child, fill )
                || ( fill != "none" && fill != "outline" && fill != "background" ) )
            {
                diagnostic( aResult, "invalid_authored_symbol_rectangle_fill",
                            "rectangle " + id + " fill must be none, outline, or background" );
                continue;
            }

            rectangle["fill"] = fill;
        }
        else
        {
            diagnostic( aResult, "unknown_authored_symbol_rectangle_field",
                        "rectangle " + id + " supports from, to, stroke, and fill" );
        }
    }

    if( !rectangle.contains( "from" ) || !rectangle.contains( "to" ) )
    {
        diagnostic( aResult, "missing_authored_symbol_rectangle_geometry",
                    "rectangle " + id + " requires from and to" );
    }
    else if( rectangle["from"] == rectangle["to"] )
    {
        diagnostic( aResult, "degenerate_authored_symbol_rectangle",
                    "rectangle " + id + " must have nonzero width or height" );
    }

    return rectangle;
}


JSON compilePin( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult,
                 std::set<std::string>& aIds, std::set<std::string>& aNumbers )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string number;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], number )
        || number.empty() || number.size() > 64 || number.find_first_of( "\r\n\0" ) != std::string::npos )
    {
        diagnostic( aResult, "invalid_authored_symbol_pin",
                    "symbol pin requires a nonempty number of at most 64 bytes" );
        return JSON::object();
    }

    const std::string logicalId = "pin:" + number;

    if( !aIds.emplace( logicalId ).second || !aNumbers.emplace( number ).second )
    {
        diagnostic( aResult, "duplicate_authored_symbol_pin_number",
                    "pin number " + number + " occurs more than once in one unit/body style" );
    }

    JSON pin = { { "kind", "pin" }, { "number", number }, { "name", "" },
                 { "electrical", "passive" }, { "shape", "line" },
                 { "orientation", "right" }, { "lengthNm", 2'540'000 },
                 { "hidden", false }, { "nameSizeNm", 1'270'000 },
                 { "numberSizeNm", 1'270'000 } };
    std::set<std::string> fields;
    static const std::set<std::string> electricalTypes = {
        "input", "output", "bidirectional", "tri_state", "passive", "free",
        "unspecified", "power_in", "power_out", "open_collector", "open_emitter",
        "no_connect"
    };
    static const std::set<std::string> shapes = {
        "line", "inverted", "clock", "inverted_clock", "input_low", "clock_low",
        "output_low", "edge_clock_high", "non_logic"
    };
    static const std::set<std::string> orientations = { "right", "down", "left", "up" };

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_authored_symbol_pin_field",
                        "pin " + number + " field " + head + " occurs more than once" );
            continue;
        }

        if( head == "at" )
        {
            int64_t x = 0;
            int64_t y = 0;

            if( !point( aDocument, child, x, y ) )
            {
                diagnostic( aResult, "invalid_authored_symbol_pin_position",
                            "pin " + number + " at requires two bounded coordinates" );
                continue;
            }

            pin["at"] = { { "xNm", x }, { "yNm", y } };
            continue;
        }

        std::string value;

        if( !oneValue( aDocument, child, value ) )
        {
            diagnostic( aResult, "invalid_authored_symbol_pin_field",
                        "pin " + number + " field " + head + " requires one value" );
            continue;
        }

        if( head == "name" )
        {
            if( value.size() > 256 || value.find( '\0' ) != std::string::npos )
                diagnostic( aResult, "invalid_authored_symbol_pin_name",
                            "pin name may contain at most 256 UTF-8 bytes" );
            else
                pin["name"] = value;
        }
        else if( head == "electrical" )
        {
            if( !electricalTypes.contains( value ) )
                diagnostic( aResult, "invalid_authored_symbol_pin_electrical",
                            "pin electrical type is not supported by KiCad 10" );
            else
                pin["electrical"] = value;
        }
        else if( head == "shape" )
        {
            if( !shapes.contains( value ) )
                diagnostic( aResult, "invalid_authored_symbol_pin_shape",
                            "pin graphical shape is not supported by KiCad 10" );
            else
                pin["shape"] = value;
        }
        else if( head == "orientation" )
        {
            if( !orientations.contains( value ) )
                diagnostic( aResult, "invalid_authored_symbol_pin_orientation",
                            "pin orientation must be right, down, left, or up" );
            else
                pin["orientation"] = value;
        }
        else if( head == "length" || head == "name_size" || head == "number_size" )
        {
            int64_t parsed = 0;
            const int64_t minimum = head == "length" ? 0 : 10'000;

            if( !distance( value, parsed ) || parsed < minimum || parsed > 100'000'000 )
            {
                diagnostic( aResult, "invalid_authored_symbol_pin_dimension",
                            "pin " + head + " is outside its bounded physical range" );
            }
            else
            {
                pin[head == "length" ? "lengthNm"
                                      : head == "name_size" ? "nameSizeNm" : "numberSizeNm"] = parsed;
            }
        }
        else if( head == "hidden" )
        {
            bool parsed = false;

            if( !boolean( value, parsed ) )
                diagnostic( aResult, "invalid_authored_symbol_pin_hidden",
                            "pin hidden must be true or false" );
            else
                pin["hidden"] = parsed;
        }
        else
        {
            diagnostic( aResult, "unknown_authored_symbol_pin_field",
                        "pin supports name, electrical, shape, at, orientation, length, hidden, "
                        "name_size, and number_size" );
        }
    }

    if( !pin.contains( "at" ) )
    {
        diagnostic( aResult, "missing_authored_symbol_pin_position",
                    "pin " + number + " requires at" );
    }

    return pin;
}


JSON compileUnit( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult,
                  std::set<std::string>& aUnitKeys )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string numberText;
    int64_t number = 0;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], numberText )
        || ( numberText != "common" && !integer( numberText, 1, 256, number ) ) )
    {
        diagnostic( aResult, "invalid_authored_symbol_unit",
                    "symbol unit requires common or a number from 1 through 256" );
        return JSON::object();
    }

    if( numberText == "common" )
        number = 0;

    int64_t bodyStyle = 1;
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];

        if( aDocument.ListHead( child ) != "body_style" )
            continue;

        std::string value;

        if( !fields.emplace( "body_style" ).second || !oneValue( aDocument, child, value )
            || !integer( value, 1, 64, bodyStyle ) )
        {
            diagnostic( aResult, "invalid_authored_symbol_body_style",
                        "symbol unit body_style must occur once and be 1 through 64" );
        }
    }

    const std::string key = std::to_string( number ) + ":" + std::to_string( bodyStyle );

    if( !aUnitKeys.emplace( key ).second )
    {
        diagnostic( aResult, "duplicate_authored_symbol_unit",
                    "symbol unit/body_style " + key + " occurs more than once" );
    }

    JSON unit = { { "number", number }, { "bodyStyle", bodyStyle },
                  { "items", JSON::array() } };
    std::set<std::string> itemIds;
    std::set<std::string> pinNumbers;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "body_style" )
            continue;

        if( unit["items"].size() >= MAX_UNIT_ITEMS )
        {
            diagnostic( aResult, "too_many_authored_symbol_unit_items",
                        "a symbol unit may contain at most 4096 items" );
            break;
        }

        if( head == "rectangle" )
        {
            unit["items"].push_back(
                    compileRectangle( aDocument, child, aResult, itemIds ) );
        }
        else if( head == "pin" )
        {
            if( number == 0 )
            {
                diagnostic( aResult, "pin_in_common_symbol_unit",
                            "pins require a numbered symbol unit; common is graphics-only" );
            }

            unit["items"].push_back(
                    compilePin( aDocument, child, aResult, itemIds, pinNumbers ) );
        }
        else
        {
            diagnostic( aResult, "unknown_authored_symbol_unit_item",
                        "symbol unit currently supports body_style, rectangle, and pin" );
        }
    }

    return unit;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_SYMBOL_COMPILER::RESULT DESIGN_SCRIPT_SYMBOL_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;
    std::string library;
    std::string name;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], id )
        || !libraryId( id, library, name ) )
    {
        diagnostic( result, "invalid_authored_symbol_id",
                    "symbol requires one bounded LIBRARY:NAME identifier" );
        return result;
    }

    result.symbol = {
        { "id", id }, { "library", library }, { "name", name },
        { "reference", "U" }, { "value", name }, { "footprint", "" },
        { "datasheet", "" }, { "description", "" }, { "keywords", "" },
        { "excludeFromSim", false }, { "inBom", true }, { "onBoard", true },
        { "inPosFiles", true }, { "hidePinNames", false }, { "hidePinNumbers", false },
        { "pinNamesOffsetNm", 0 }, { "properties", JSON::array() },
        { "units", JSON::array() }
    };
    std::set<std::string> fields;
    std::set<std::string> propertyNames;
    std::set<std::string> unitKeys;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "unit" )
        {
            if( result.symbol["units"].size() >= MAX_SYMBOL_UNITS )
            {
                diagnostic( result, "too_many_authored_symbol_units",
                            "a symbol may contain at most 256 unit/body-style declarations" );
                continue;
            }

            result.symbol["units"].push_back( compileUnit( aDocument, child, result, unitKeys ) );
            continue;
        }

        if( head == "property" )
        {
            const DOCUMENT::NODE& property = aDocument.Nodes().at( child );
            std::string propertyName;
            std::string value;

            if( result.symbol["properties"].size() >= MAX_SYMBOL_PROPERTIES
                || property.children.size() != 3
                || !scalar( aDocument, property.children[1], propertyName )
                || !scalar( aDocument, property.children[2], value )
                || propertyName.empty() || propertyName.size() > 128
                || value.size() > MAX_TEXT_BYTES )
            {
                diagnostic( result, "invalid_authored_symbol_property",
                            "symbol property requires a 1..128-byte name and value up to 4096 bytes" );
                continue;
            }

            static const std::set<std::string> reserved = {
                "Reference", "Value", "Footprint", "Datasheet", "Description",
                "ki_keywords", "ki_fp_filters"
            };

            if( reserved.contains( propertyName ) )
            {
                diagnostic( result, "reserved_authored_symbol_property",
                            "symbol property " + propertyName + " has a dedicated KDS field" );
                continue;
            }

            if( !propertyNames.emplace( propertyName ).second )
            {
                diagnostic( result, "duplicate_authored_symbol_property",
                            "symbol property " + propertyName + " occurs more than once" );
                continue;
            }

            result.symbol["properties"].push_back(
                    { { "name", propertyName }, { "value", value } } );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( result, "duplicate_authored_symbol_field",
                        "symbol " + id + " field " + head + " occurs more than once" );
            continue;
        }

        std::string value;

        if( !oneValue( aDocument, child, value ) )
        {
            diagnostic( result, "invalid_authored_symbol_field",
                        "symbol " + id + " field " + head + " requires one value" );
            continue;
        }

        if( head == "reference" )
        {
            if( value.empty() || value.size() > 16
                || value.find_first_of( "\r\n\0" ) != std::string::npos )
                diagnostic( result, "invalid_authored_symbol_reference",
                            "symbol reference must contain 1 through 16 safe bytes" );
            else
                result.symbol["reference"] = value;
        }
        else if( head == "value" || head == "footprint" || head == "datasheet"
                 || head == "description" || head == "keywords" )
        {
            if( value.size() > MAX_TEXT_BYTES || value.find( '\0' ) != std::string::npos )
                diagnostic( result, "invalid_authored_symbol_text",
                            "symbol text fields may contain at most 4096 UTF-8 bytes" );
            else
                result.symbol[head] = value;
        }
        else if( head == "exclude_from_sim" || head == "in_bom" || head == "on_board"
                 || head == "in_pos_files" || head == "hide_pin_names"
                 || head == "hide_pin_numbers" )
        {
            bool parsed = false;

            if( !boolean( value, parsed ) )
            {
                diagnostic( result, "invalid_authored_symbol_boolean",
                            "symbol " + head + " must be true or false" );
            }
            else
            {
                const std::string key = head == "exclude_from_sim" ? "excludeFromSim"
                                      : head == "in_bom" ? "inBom"
                                      : head == "on_board" ? "onBoard"
                                      : head == "in_pos_files" ? "inPosFiles"
                                      : head == "hide_pin_names" ? "hidePinNames"
                                                                     : "hidePinNumbers";
                result.symbol[key] = parsed;
            }
        }
        else if( head == "pin_names_offset" )
        {
            int64_t parsed = 0;

            if( !distance( value, parsed ) || parsed < 0 || parsed > 100'000'000 )
                diagnostic( result, "invalid_authored_symbol_pin_names_offset",
                            "pin_names_offset must be between 0 and 100 mm" );
            else
                result.symbol["pinNamesOffsetNm"] = parsed;
        }
        else
        {
            diagnostic( result, "unknown_authored_symbol_field",
                        "symbol supports reference, value, footprint, datasheet, description, "
                        "keywords, inclusion flags, pin display fields, property, and unit" );
        }
    }

    bool hasNumberedUnit = false;

    for( const JSON& unit : result.symbol["units"] )
    {
        if( unit.is_object() && unit.value( "number", 0 ) > 0 )
            hasNumberedUnit = true;
    }

    if( !hasNumberedUnit )
    {
        diagnostic( result, "missing_authored_symbol_unit",
                    "authored symbol " + id + " requires at least one numbered unit" );
    }

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
