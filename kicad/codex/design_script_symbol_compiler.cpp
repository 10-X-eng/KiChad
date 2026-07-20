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

#include "design_script_symbol_field_compiler.h"
#include "design_script_symbol_graphics_compiler.h"
#include "design_script_symbol_text_compiler.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_SYMBOL_COMPILER::RESULT;
using FIELD_COMPILER = KICHAD::DESIGN_SCRIPT_SYMBOL_FIELD_COMPILER;
using GRAPHICS_COMPILER = KICHAD::DESIGN_SCRIPT_SYMBOL_GRAPHICS_COMPILER;
using TEXT_COMPILER = KICHAD::DESIGN_SCRIPT_SYMBOL_TEXT_COMPILER;

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
                 { "numberSizeNm", 1'270'000 }, { "alternates", JSON::array() } };
    std::set<std::string> fields;
    std::set<std::string> alternateNames;
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

        if( head == "alternate" )
        {
            const DOCUMENT::NODE& alternate = aDocument.Nodes().at( child );
            std::string alternateName;
            std::string alternateElectrical;
            std::string alternateShape;

            if( alternate.children.size() != 4
                || !scalar( aDocument, alternate.children[1], alternateName )
                || !scalar( aDocument, alternate.children[2], alternateElectrical )
                || !scalar( aDocument, alternate.children[3], alternateShape )
                || alternateName.empty() || alternateName.size() > 256
                || alternateName.find( '\0' ) != std::string::npos
                || !electricalTypes.contains( alternateElectrical )
                || !shapes.contains( alternateShape ) )
            {
                diagnostic( aResult, "invalid_authored_symbol_pin_alternate",
                            "pin " + number
                                    + " alternate requires a unique name, electrical type, and shape" );
            }
            else if( !alternateNames.emplace( alternateName ).second )
            {
                diagnostic( aResult, "duplicate_authored_symbol_pin_alternate",
                            "pin " + number + " alternate " + alternateName
                                    + " occurs more than once" );
            }
            else
            {
                pin["alternates"].push_back( { { "name", alternateName },
                                                { "electrical", alternateElectrical },
                                                { "shape", alternateShape } } );
            }

            continue;
        }

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
                        "name_size, number_size, and alternate" );
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
                  { "displayName", "" }, { "items", JSON::array() } };
    std::set<std::string> itemIds;
    std::set<std::string> pinNumbers;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "body_style" )
            continue;

        if( head == "display_name" )
        {
            std::string displayName;

            if( !fields.emplace( head ).second || number == 0
                || !oneValue( aDocument, child, displayName ) || displayName.empty()
                || displayName.size() > 256 || displayName.find( '\0' ) != std::string::npos )
            {
                diagnostic( aResult, "invalid_authored_symbol_unit_display_name",
                            "a numbered symbol unit accepts one display_name of 1 through 256 "
                            "UTF-8 bytes" );
            }
            else
            {
                unit["displayName"] = displayName;
            }

            continue;
        }

        if( unit["items"].size() >= MAX_UNIT_ITEMS )
        {
            diagnostic( aResult, "too_many_authored_symbol_unit_items",
                        "a symbol unit may contain at most 4096 items" );
            break;
        }

        if( GRAPHICS_COMPILER::IsGraphic( head ) )
        {
            GRAPHICS_COMPILER::RESULT graphic = GRAPHICS_COMPILER::Compile( aDocument, child );

            for( const JSON& entry : graphic.diagnostics )
                aResult.diagnostics.push_back( entry );

            const std::string itemId = graphic.item.value( "id", "" );

            if( !itemId.empty() && !itemIds.emplace( itemId ).second )
            {
                diagnostic( aResult, "duplicate_authored_symbol_item",
                            "symbol unit item ID " + itemId + " occurs more than once" );
            }

            unit["items"].push_back( std::move( graphic.item ) );
        }
        else if( TEXT_COMPILER::IsText( head ) )
        {
            TEXT_COMPILER::RESULT text = TEXT_COMPILER::Compile( aDocument, child );

            for( const JSON& entry : text.diagnostics )
                aResult.diagnostics.push_back( entry );

            const std::string itemId = text.item.value( "id", "" );

            if( !itemId.empty() && !itemIds.emplace( itemId ).second )
            {
                diagnostic( aResult, "duplicate_authored_symbol_item",
                            "symbol unit item ID " + itemId + " occurs more than once" );
            }

            unit["items"].push_back( std::move( text.item ) );
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
                        "symbol unit supports body_style, graphics, text, text_box, and pin" );
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
        { "units", JSON::array() }, { "power", "normal" }, { "extends", "" },
        { "declaredFields", JSON::array() }, { "duplicatePinNumbersAreJumpers", false },
        { "jumperGroups", JSON::array() }, { "fieldLayouts", JSON::object() },
        { "bodyStyles", { { "mode", "default" }, { "names", JSON::array() } } },
        { "unitsLocked", false }, { "footprintFilters", JSON::array() }
    };
    std::set<std::string> fields;
    std::set<std::string> propertyNames;
    std::set<std::string> unitKeys;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "footprint_filter" )
        {
            std::string filter;

            if( result.symbol["footprintFilters"].size() >= 256
                || !oneValue( aDocument, child, filter ) || filter.empty()
                || filter.size() > 256 || filter.find_first_of( " \t\r\n" ) != std::string::npos
                || filter.find( '\0' ) != std::string::npos )
            {
                diagnostic( result, "invalid_authored_symbol_footprint_filter",
                            "footprint_filter requires one unique whitespace-free pattern" );
            }
            else
            {
                bool duplicate = false;

                for( const JSON& existing : result.symbol["footprintFilters"] )
                    duplicate = duplicate || existing == filter;

                if( duplicate )
                    diagnostic( result, "duplicate_authored_symbol_footprint_filter",
                                "footprint filter " + filter + " occurs more than once" );
                else
                    result.symbol["footprintFilters"].push_back( filter );
            }

            continue;
        }

        if( head == "body_styles" )
        {
            if( !fields.emplace( head ).second )
            {
                diagnostic( result, "duplicate_authored_symbol_body_styles",
                            "symbol body_styles occurs more than once" );
                continue;
            }

            const DOCUMENT::NODE& declaration = aDocument.Nodes().at( child );
            JSON names = JSON::array();
            bool valid = declaration.children.size() >= 2;
            bool demorgan = false;

            for( size_t style = 1; valid && style < declaration.children.size(); ++style )
            {
                std::string styleName;
                valid = scalar( aDocument, declaration.children[style], styleName )
                        && !styleName.empty() && styleName.size() <= 128
                        && styleName.find( '\0' ) == std::string::npos;

                if( style == 1 && declaration.children.size() == 2 && styleName == "demorgan" )
                    demorgan = true;
                else
                    names.push_back( styleName );
            }

            if( !valid || ( !demorgan && ( names.empty() || names.size() > 64 ) ) )
                diagnostic( result, "invalid_authored_symbol_body_styles",
                            "body_styles requires demorgan or 1 through 64 unique display names" );
            else
            {
                std::set<std::string> unique;

                for( const JSON& styleName : names )
                    valid = valid && unique.emplace( styleName.get<std::string>() ).second;

                if( !valid )
                    diagnostic( result, "duplicate_authored_symbol_body_style_name",
                                "body style display names must be unique" );
                else
                    result.symbol["bodyStyles"] = { { "mode", demorgan ? "demorgan" : "named" },
                                                      { "names", std::move( names ) } };
            }

            continue;
        }

        if( head == "jumper_group" )
        {
            if( result.symbol["jumperGroups"].size() >= 256 )
            {
                diagnostic( result, "too_many_authored_symbol_jumper_groups",
                            "a symbol may contain at most 256 jumper groups" );
                continue;
            }

            const DOCUMENT::NODE& group = aDocument.Nodes().at( child );
            JSON numbers = JSON::array();
            std::set<std::string> unique;

            for( size_t valueIndex = 1; valueIndex < group.children.size(); ++valueIndex )
            {
                std::string pinNumber;

                if( !scalar( aDocument, group.children[valueIndex], pinNumber )
                    || pinNumber.empty() || pinNumber.size() > 64
                    || pinNumber.find( '\0' ) != std::string::npos
                    || !unique.emplace( pinNumber ).second )
                {
                    diagnostic( result, "invalid_authored_symbol_jumper_group",
                                "jumper_group requires at least two unique bounded pin numbers" );
                    numbers = JSON::array();
                    break;
                }

                numbers.push_back( pinNumber );
            }

            if( numbers.size() < 2 )
                diagnostic( result, "invalid_authored_symbol_jumper_group",
                            "jumper_group requires at least two unique bounded pin numbers" );
            else
                result.symbol["jumperGroups"].push_back( std::move( numbers ) );

            continue;
        }

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

        static const std::set<std::string> authoredFieldHeads = {
            "reference", "value", "footprint", "datasheet", "description", "keywords"
        };

        if( authoredFieldHeads.contains( head ) )
        {
            if( !fields.emplace( head ).second )
            {
                diagnostic( result, "duplicate_authored_symbol_field",
                            "symbol " + id + " field " + head + " occurs more than once" );
                continue;
            }

            FIELD_COMPILER::RESULT compiledField = FIELD_COMPILER::Compile( aDocument, child );

            for( const JSON& entry : compiledField.diagnostics )
                result.diagnostics.push_back( entry );

            if( !compiledField.field.is_object() || !compiledField.field.contains( "value" )
                || !compiledField.field["value"].is_string()
                || !compiledField.field.contains( "layout" ) )
            {
                continue;
            }

            const std::string value = compiledField.field["value"].get<std::string>();

            if( head == "reference"
                && ( value.empty() || value.size() > 16
                     || value.find_first_of( "\r\n\0" ) != std::string::npos ) )
            {
                diagnostic( result, "invalid_authored_symbol_reference",
                            "symbol reference must contain 1 through 16 safe bytes" );
                continue;
            }

            result.symbol[head] = value;
            result.symbol["fieldLayouts"][head] = std::move( compiledField.field["layout"] );
            continue;
        }

        if( head == "property" )
        {
            FIELD_COMPILER::RESULT compiledField = FIELD_COMPILER::Compile( aDocument, child );

            for( const JSON& entry : compiledField.diagnostics )
                result.diagnostics.push_back( entry );

            const std::string propertyName = compiledField.field.value( "name", "" );

            if( result.symbol["properties"].size() >= MAX_SYMBOL_PROPERTIES
                || !compiledField.field.is_object() || propertyName.empty()
                || !compiledField.field.contains( "value" )
                || !compiledField.field["value"].is_string()
                || !compiledField.field.contains( "layout" ) )
            {
                diagnostic( result, "invalid_authored_symbol_property",
                            "symbol property requires a 1..128-byte name and value up to 4096 bytes" );
                continue;
            }

            static const std::set<std::string> reserved = {
                "Reference", "Value", "Footprint", "Datasheet", "Description",
                "ki_keywords", "ki_fp_filters", "ki_locked"
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

            result.symbol["properties"].push_back( std::move( compiledField.field ) );
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

        if( head == "exclude_from_sim" || head == "in_bom" || head == "on_board"
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
        else if( head == "power" )
        {
            if( value != "normal" && value != "global" && value != "local" )
                diagnostic( result, "invalid_authored_symbol_power",
                            "symbol power must be normal, global, or local" );
            else
                result.symbol["power"] = value;
        }
        else if( head == "extends" )
        {
            if( !identifier( value ) || value == name )
                diagnostic( result, "invalid_authored_symbol_parent",
                            "symbol extends requires a different symbol name in the same library" );
            else
                result.symbol["extends"] = value;
        }
        else if( head == "duplicate_pin_numbers_are_jumpers" )
        {
            bool parsed = false;

            if( !boolean( value, parsed ) )
                diagnostic( result, "invalid_authored_symbol_jumper_flag",
                            "duplicate_pin_numbers_are_jumpers must be true or false" );
            else
                result.symbol["duplicatePinNumbersAreJumpers"] = parsed;
        }
        else if( head == "units_locked" )
        {
            bool parsed = false;

            if( !boolean( value, parsed ) )
                diagnostic( result, "invalid_authored_symbol_units_locked",
                            "units_locked must be true or false" );
            else
                result.symbol["unitsLocked"] = parsed;
        }
        else
        {
            diagnostic( result, "unknown_authored_symbol_field",
                        "symbol supports reference, value, footprint, datasheet, description, "
                        "keywords, inclusion flags, pin display fields, library metadata, "
                        "property, and unit" );
        }
    }

    bool hasNumberedUnit = false;
    bool hasPowerInput = false;
    std::set<std::string> availablePinNumbers;
    std::map<int64_t, std::string> unitDisplayNames;
    int64_t maximumBodyStyle = 1;

    for( const JSON& unit : result.symbol["units"] )
    {
        if( unit.is_object() && unit.value( "number", 0 ) > 0 )
            hasNumberedUnit = true;

        if( unit.is_object() )
        {
            maximumBodyStyle = std::max<int64_t>( maximumBodyStyle,
                                                  unit.value( "bodyStyle", int64_t{ 1 } ) );
            const int64_t unitNumber = unit.value( "number", 0 );
            const std::string displayName = unit.value( "displayName", "" );

            if( !displayName.empty() )
            {
                auto [entry, inserted] = unitDisplayNames.emplace( unitNumber, displayName );

                if( !inserted && entry->second != displayName )
                    diagnostic( result, "conflicting_authored_symbol_unit_display_name",
                                "all body styles of one unit require the same display_name" );
            }
        }

        if( unit.is_object() && unit.contains( "items" ) && unit["items"].is_array() )
        {
            for( const JSON& item : unit["items"] )
            {
                hasPowerInput = hasPowerInput
                                || ( item.value( "kind", "" ) == "pin"
                                     && item.value( "electrical", "" ) == "power_in" );

                if( item.value( "kind", "" ) == "pin" )
                    availablePinNumbers.insert( item.value( "number", "" ) );
            }
        }
    }

    for( const std::string& field : fields )
        result.symbol["declaredFields"].push_back( field );

    const bool derived = !result.symbol["extends"].get_ref<const std::string&>().empty();
    const std::string bodyStyleMode = result.symbol["bodyStyles"]["mode"].get<std::string>();
    const int64_t availableBodyStyles = bodyStyleMode == "demorgan" ? 2
                                            : bodyStyleMode == "named"
                                                      ? result.symbol["bodyStyles"]["names"].size()
                                                      : 1;

    if( maximumBodyStyle > availableBodyStyles )
        diagnostic( result, "undeclared_authored_symbol_body_style",
                    "unit body_style exceeds the declared body_styles inventory" );

    if( derived && !result.symbol["units"].empty() )
    {
        diagnostic( result, "derived_authored_symbol_has_units",
                    "derived symbol " + id + " inherits units and cannot declare its own" );
    }
    else if( !derived && !hasNumberedUnit )
    {
        diagnostic( result, "missing_authored_symbol_unit",
                    "authored symbol " + id + " requires at least one numbered unit" );
    }

    if( derived && result.symbol["power"] != "normal" )
        diagnostic( result, "derived_authored_symbol_power",
                    "derived symbol " + id + " inherits power semantics" );

    if( derived && !result.symbol["jumperGroups"].empty() )
        diagnostic( result, "derived_authored_symbol_jumper_groups",
                    "derived symbol " + id + " inherits jumper groups" );

    if( derived && ( !result.symbol["footprintFilters"].empty()
                     || result.symbol["unitsLocked"].get<bool>()
                     || bodyStyleMode != "default" ) )
        diagnostic( result, "derived_authored_symbol_library_metadata",
                    "derived symbol " + id + " inherits filters, locking, and body styles" );

    if( derived )
    {
        static const std::set<std::string> derivedFields = {
            "reference", "value", "footprint", "datasheet", "description", "keywords", "extends"
        };

        for( const std::string& field : fields )
        {
            if( !derivedFields.contains( field ) )
                diagnostic( result, "unsupported_derived_authored_symbol_field",
                            "derived symbol " + id + " cannot override " + field );
        }
    }

    std::set<std::string> groupedPins;

    for( const JSON& group : result.symbol["jumperGroups"] )
    {
        for( const JSON& number : group )
        {
            const std::string pinNumber = number.get<std::string>();

            if( !availablePinNumbers.contains( pinNumber ) )
                diagnostic( result, "unknown_authored_symbol_jumper_pin",
                            "jumper group references absent pin " + pinNumber );
            else if( !groupedPins.emplace( pinNumber ).second )
                diagnostic( result, "duplicate_authored_symbol_jumper_pin",
                            "pin " + pinNumber + " occurs in more than one jumper group" );
        }
    }

    if( result.symbol["power"] != "normal"
        && ( result.symbol["reference"].get_ref<const std::string&>().empty()
             || result.symbol["reference"].get_ref<const std::string&>().front() != '#'
             || !hasPowerInput ) )
    {
        diagnostic( result, "invalid_authored_power_symbol",
                    "global/local power symbols require a # reference and a power_in pin" );
    }

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
