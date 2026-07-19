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

#include "design_script_compiler.h"

#include "design_script_capabilities.h"

#include "design_script_board_compiler.h"
#include "lossless_sexpr_document.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <limits>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include <picosha2.h>
#include <wx/string.h>


namespace
{

using JSON = nlohmann::json;
using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;

constexpr size_t MAX_SCRIPT_BYTES = 1024 * 1024;
constexpr size_t MAX_TOP_LEVEL_FORMS = 20000;
constexpr size_t MAX_IDENTIFIER_BYTES = 128;
constexpr size_t MAX_TITLE_BLOCK_TEXT_BYTES = 4096;
constexpr size_t MAX_SCHEMATIC_PROPERTY_BYTES = 4096;
constexpr size_t MAX_LIBRARIES = 512;
constexpr size_t MAX_PROJECT_LIBRARIES_PER_TABLE = 256;


void diagnostic( KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult, const std::string& aSeverity,
                 const std::string& aCode, const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", aSeverity },
                                     { "code", aCode },
                                     { "message", aMessage } } );
}


bool isScalar( const DOCUMENT& aDocument, size_t aNode )
{
    return aNode < aDocument.Nodes().size()
           && aDocument.Nodes()[aNode].kind != DOCUMENT::NODE_KIND::LIST;
}


bool scalarText( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    if( !isScalar( aDocument, aNode ) )
        return false;

    aValue = aDocument.AtomText( aNode );
    return true;
}


bool validIdentifier( const std::string& aValue )
{
    if( aValue.empty() || aValue.size() > MAX_IDENTIFIER_BYTES )
        return false;

    return std::all_of( aValue.begin(), aValue.end(),
                        []( unsigned char aCharacter )
                        {
                            return std::isalnum( aCharacter ) || aCharacter == '_'
                                   || aCharacter == '-' || aCharacter == '+'
                                   || aCharacter == '.' || aCharacter == '/'
                                   || aCharacter == '#';
                        } );
}


bool libraryIdNickname( const std::string& aValue, std::string& aNickname )
{
    const size_t separator = aValue.find( ':' );

    if( separator == std::string::npos || separator == 0 || separator + 1 == aValue.size()
        || aValue.find( ':', separator + 1 ) != std::string::npos || aValue.size() > 512 )
    {
        return false;
    }

    aNickname = aValue.substr( 0, separator );
    return validIdentifier( aNickname );
}


JSON scalarValue( const DOCUMENT& aDocument, size_t aNode )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    const std::string     value = aDocument.AtomText( aNode );

    if( node.kind == DOCUMENT::NODE_KIND::STRING )
        return value;

    if( value == "true" )
        return true;

    if( value == "false" )
        return false;

    int64_t integer = 0;
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    std::from_chars_result converted = std::from_chars( begin, end, integer );

    if( converted.ec == std::errc() && converted.ptr == end )
        return integer;

    return value;
}


JSON expressionToIr( const DOCUMENT& aDocument, size_t aNode )
{
    if( isScalar( aDocument, aNode ) )
        return scalarValue( aDocument, aNode );

    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    JSON                  arguments = JSON::array();

    for( size_t i = 1; i < node.children.size(); ++i )
        arguments.emplace_back( expressionToIr( aDocument, node.children[i] ) );

    return { { "op", aDocument.ListHead( aNode ) }, { "args", std::move( arguments ) } };
}


bool parseSingleValueForm( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalarText( aDocument, node.children[1], aValue );
}


bool parseDistance( const std::string& aText, int64_t& aNanometers )
{
    long double value = 0.0L;
    std::from_chars_result converted =
            std::from_chars( aText.data(), aText.data() + aText.size(), value );

    if( converted.ec != std::errc() || converted.ptr == aText.data()
        || !std::isfinite( value ) )
    {
        return false;
    }

    const std::string_view unit( converted.ptr,
                                 static_cast<size_t>( aText.data() + aText.size()
                                                      - converted.ptr ) );
    long double scale = 0.0L;

    if( unit == "mm" )
        scale = 1000000.0L;
    else if( unit == "mil" )
        scale = 25400.0L;
    else if( unit == "um" )
        scale = 1000.0L;
    else if( unit == "nm" )
        scale = 1.0L;
    else if( unit == "in" )
        scale = 25400000.0L;
    else
        return false;

    const long double rounded = std::round( value * scale );

    if( !std::isfinite( rounded ) || rounded < -9223372036854775808.0L
        || rounded >= 9223372036854775808.0L )
    {
        return false;
    }

    aNanometers = static_cast<int64_t>( rounded );
    return true;
}


bool parseTime( const std::string& aText, int64_t& aFemtoseconds )
{
    long double value = 0.0L;
    std::from_chars_result converted =
            std::from_chars( aText.data(), aText.data() + aText.size(), value );

    if( converted.ec != std::errc() || converted.ptr == aText.data()
        || !std::isfinite( value ) )
    {
        return false;
    }

    const std::string_view unit( converted.ptr,
                                 static_cast<size_t>( aText.data() + aText.size()
                                                      - converted.ptr ) );
    long double scale = 0.0L;

    if( unit == "fs" )
        scale = 1.0L;
    else if( unit == "ps" )
        scale = 1000.0L;
    else
        return false;

    const long double rounded = std::round( value * scale );

    if( !std::isfinite( rounded ) || rounded < -9223372036854775808.0L
        || rounded >= 9223372036854775808.0L )
    {
        return false;
    }

    aFemtoseconds = static_cast<int64_t>( rounded );
    return true;
}


bool parseFiniteDecimal( const std::string& aText, double& aValue,
                         const std::string_view aRequiredSuffix = {} )
{
    std::from_chars_result converted =
            std::from_chars( aText.data(), aText.data() + aText.size(), aValue );

    if( converted.ec != std::errc() || converted.ptr == aText.data()
        || !std::isfinite( aValue ) )
    {
        return false;
    }

    return std::string_view( converted.ptr,
                             static_cast<size_t>( aText.data() + aText.size()
                                                  - converted.ptr ) ) == aRequiredSuffix;
}


bool parseBoundedInteger( const std::string& aText, int64_t aMinimum, int64_t aMaximum,
                          int64_t& aValue )
{
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    std::from_chars_result converted = std::from_chars( begin, end, aValue );
    return converted.ec == std::errc() && converted.ptr == end
           && aValue >= aMinimum && aValue <= aMaximum;
}


bool parseHexColor( const std::string& aText, JSON& aColor )
{
    if( aText.size() != 7 && aText.size() != 9 )
        return false;

    if( aText.front() != '#' )
        return false;

    int channels[4] = { 0, 0, 0, 255 };

    for( size_t channel = 0; channel < ( aText.size() - 1 ) / 2; ++channel )
    {
        const char* begin = aText.data() + 1 + channel * 2;
        const char* end = begin + 2;
        std::from_chars_result converted = std::from_chars( begin, end, channels[channel], 16 );

        if( converted.ec != std::errc() || converted.ptr != end )
            return false;
    }

    aColor = { { "r", channels[0] / 255.0 },
               { "g", channels[1] / 255.0 },
               { "b", channels[2] / 255.0 },
               { "a", channels[3] / 255.0 } };
    return true;
}


bool boundedBusRanges( const std::string& aPattern )
{
    size_t range = 0;

    while( ( range = aPattern.find( "..", range ) ) != std::string::npos )
    {
        const size_t open = aPattern.rfind( '[', range );
        const size_t close = aPattern.find( ']', range + 2 );

        if( open != std::string::npos && close != std::string::npos )
        {
            int64_t first = 0;
            int64_t last = 0;
            const char* firstBegin = aPattern.data() + open + 1;
            const char* firstEnd = aPattern.data() + range;
            const char* lastBegin = aPattern.data() + range + 2;
            const char* lastEnd = aPattern.data() + close;
            std::from_chars_result firstResult =
                    std::from_chars( firstBegin, firstEnd, first );
            std::from_chars_result lastResult = std::from_chars( lastBegin, lastEnd, last );

            if( firstResult.ec == std::errc() && firstResult.ptr == firstEnd
                && lastResult.ec == std::errc() && lastResult.ptr == lastEnd
                && std::fabs( static_cast<long double>( first )
                              - static_cast<long double>( last ) ) > 255.0L )
            {
                return false;
            }
        }

        range += 2;
    }

    return true;
}


JSON compileProject( const DOCUMENT& aDocument, size_t aNode,
                     KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           name;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], name )
        || name.empty() || name.size() > MAX_IDENTIFIER_BYTES )
    {
        diagnostic( aResult, "error", "invalid_project",
                    "project requires a non-empty name of at most 128 bytes" );
        return JSON::object();
    }

    JSON project = { { "name", name } };
    JSON titleBlock = { { "title", "" }, { "company", "" }, { "revision", "" },
                        { "date", "" }, { "comments", JSON::array() } };

    for( int i = 0; i < 9; ++i )
        titleBlock["comments"].emplace_back( "" );

    const std::set<std::string> allowed = { "title", "company", "revision", "date" };
    std::set<std::string>       fields;
    std::set<int>               comments;
    bool                        hasTitleBlock = false;

    for( size_t i = 2; i < node.children.size(); ++i )
    {
        const size_t      child = node.children[i];
        const std::string head = aDocument.ListHead( child );
        std::string       value;

        if( head == "comment" )
        {
            const DOCUMENT::NODE& comment = aDocument.Nodes()[child];
            std::string indexText;
            int         index = 0;

            if( comment.children.size() != 3
                || !scalarText( aDocument, comment.children[1], indexText )
                || !scalarText( aDocument, comment.children[2], value ) )
            {
                diagnostic( aResult, "error", "invalid_project_comment",
                            "project comment requires an index from 1 through 9 and one scalar "
                            "value" );
                continue;
            }

            const char* begin = indexText.data();
            const char* end = begin + indexText.size();
            std::from_chars_result converted = std::from_chars( begin, end, index );

            if( converted.ec != std::errc() || converted.ptr != end || index < 1 || index > 9
                || value.size() > MAX_TITLE_BLOCK_TEXT_BYTES )
            {
                diagnostic( aResult, "error", "invalid_project_comment",
                            "project comment index must be 1 through 9 and its value at most "
                            "4096 bytes" );
                continue;
            }

            if( !comments.emplace( index ).second )
            {
                diagnostic( aResult, "error", "duplicate_project_comment",
                            "project comment index " + std::to_string( index )
                                    + " occurs more than once" );
                continue;
            }

            titleBlock["comments"][index - 1] = value;
            hasTitleBlock = true;
            continue;
        }

        if( !allowed.contains( head ) || !parseSingleValueForm( aDocument, child, value )
            || value.size() > MAX_TITLE_BLOCK_TEXT_BYTES )
        {
            diagnostic( aResult, "error", "invalid_project_field",
                        "project fields must be title, company, revision, or date with one "
                        "scalar value of at most 4096 bytes; comments use (comment INDEX VALUE)" );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_project_field",
                        "project field '" + head + "' occurs more than once" );
            continue;
        }

        titleBlock[head] = value;
        hasTitleBlock = true;
    }

    if( hasTitleBlock )
        project["titleBlock"] = std::move( titleBlock );

    return project;
}


JSON compileLibrary( const DOCUMENT& aDocument, size_t aNode,
                     KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           kind;
    std::string           id;

    if( node.children.size() < 3 || !scalarText( aDocument, node.children[1], kind )
        || !scalarText( aDocument, node.children[2], id )
        || ( kind != "symbol" && kind != "footprint" && kind != "model" )
        || !validIdentifier( id ) )
    {
        diagnostic( aResult, "error", "invalid_library",
                    "library requires kind symbol, footprint, or model and a bounded identifier" );
        return JSON::object();
    }

    JSON library = { { "kind", kind }, { "id", id }, { "uri", nullptr } };
    std::set<std::string> fields;

    for( size_t i = 3; i < node.children.size(); ++i )
    {
        const size_t      child = node.children[i];
        const std::string head = aDocument.ListHead( child );
        std::string       value;

        if( ( head != "uri" && head != "table" )
            || !parseSingleValueForm( aDocument, child, value ) )
        {
            diagnostic( aResult, "error", "invalid_library_field",
                        "library fields must be uri or table with one scalar value" );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_library_field",
                        "library field '" + head + "' occurs more than once" );
            continue;
        }

        library[head] = value;
    }

    const std::string table = library.value( "table", "" );

    if( table != "global" && table != "project" )
    {
        diagnostic( aResult, "error", "invalid_library_table",
                    "library requires exactly one (table global|project)" );
    }

    if( table == "global" && !library["uri"].is_null() )
    {
        diagnostic( aResult, "error", "redundant_global_library_uri",
                    "global library dependencies use their installed nickname and cannot set uri" );
    }

    if( table == "project" )
    {
        const std::string uri = library["uri"].is_string()
                                      ? library["uri"].get<std::string>()
                                      : std::string();
        const bool safe = uri.starts_with( "${KIPRJMOD}/" ) && uri.size() <= 4096
                          && uri.size() > std::strlen( "${KIPRJMOD}/" )
                          && uri.find( '\0' ) == std::string::npos
                          && uri.find_first_of( "\r\n\"\\" ) == std::string::npos
                          && uri.find( "/../" ) == std::string::npos
                          && !uri.ends_with( "/.." );
        const bool nativeSuffix = kind == "symbol" ? uri.ends_with( ".kicad_sym" )
                                : kind == "footprint" ? uri.ends_with( ".pretty" )
                                                      : true;

        if( !safe || !nativeSuffix )
        {
            diagnostic( aResult, "error", "invalid_project_library_uri",
                        "project library uri must be a safe ${KIPRJMOD}/ path ending in "
                        ".kicad_sym for symbols or .pretty for footprints" );
        }
    }

    return library;
}


JSON compileComponent( const DOCUMENT& aDocument, size_t aNode,
                       KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           reference;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], reference )
        || !validIdentifier( reference ) )
    {
        diagnostic( aResult, "error", "invalid_component",
                    "component requires a bounded logical reference" );
        return JSON::object();
    }

    JSON component = { { "reference", reference },
                       { "properties", JSON::object() },
                       { "units", JSON::array() } };
    std::set<std::string> singletonFields;
    std::set<std::string> propertyNames;
    std::set<int64_t>     unitNumbers;

    for( size_t i = 2; i < node.children.size(); ++i )
    {
        const size_t      child = node.children[i];
        const std::string head = aDocument.ListHead( child );
        const DOCUMENT::NODE& field = aDocument.Nodes()[child];

        if( head == "unit" )
        {
            std::string numberText;
            int64_t     number = 0;

            if( field.children.size() < 2
                || !scalarText( aDocument, field.children[1], numberText )
                || !parseBoundedInteger( numberText, 1, 256, number ) )
            {
                diagnostic( aResult, "error", "invalid_component_unit",
                            "component unit requires a number from 1 through 256" );
                continue;
            }

            if( !unitNumbers.emplace( number ).second )
            {
                diagnostic( aResult, "error", "duplicate_component_unit",
                            "component " + reference + " unit " + numberText
                                    + " occurs more than once" );
                continue;
            }

            JSON unit = { { "number", number } };
            std::set<std::string> unitFields;

            for( size_t unitIndex = 2; unitIndex < field.children.size(); ++unitIndex )
            {
                const size_t unitChild = field.children[unitIndex];
                const std::string unitHead = aDocument.ListHead( unitChild );

                if( !unitFields.emplace( unitHead ).second )
                {
                    diagnostic( aResult, "error", "duplicate_component_unit_field",
                                "component unit field '" + unitHead
                                        + "' occurs more than once" );
                    continue;
                }

                if( unitHead == "sheet" || unitHead == "mirror" )
                {
                    std::string value;

                    if( !parseSingleValueForm( aDocument, unitChild, value )
                        || ( unitHead == "sheet" && !validIdentifier( value ) )
                        || ( unitHead == "mirror" && value != "none" && value != "x"
                             && value != "y" && value != "xy" ) )
                    {
                        diagnostic( aResult, "error", "invalid_component_unit_" + unitHead,
                                    unitHead == "sheet"
                                            ? "component unit sheet must be a bounded sheet ID"
                                            : "component unit mirror must be none, x, y, or xy" );
                    }
                    else
                    {
                        unit[unitHead] = value;
                    }

                    continue;
                }

                if( unitHead == "at" )
                {
                    const DOCUMENT::NODE& position = aDocument.Nodes()[unitChild];
                    std::string xText;
                    std::string yText;
                    int64_t x = 0;
                    int64_t y = 0;

                    if( position.children.size() != 3
                        || !scalarText( aDocument, position.children[1], xText )
                        || !scalarText( aDocument, position.children[2], yText )
                        || !parseDistance( xText, x ) || !parseDistance( yText, y )
                        || x < 0 || y < 0 || x > 2'000'000'000 || y > 2'000'000'000 )
                    {
                        diagnostic( aResult, "error", "invalid_component_unit_position",
                                    "component unit at requires two distances from 0 to 2 m" );
                    }
                    else
                    {
                        unit["position"] = { { "xNm", x }, { "yNm", y } };
                    }

                    continue;
                }

                if( unitHead == "rotation" )
                {
                    std::string value;
                    double degrees = 0.0;

                    if( !parseSingleValueForm( aDocument, unitChild, value )
                        || !parseFiniteDecimal( value, degrees, "deg" )
                        || ( degrees != 0.0 && degrees != 90.0 && degrees != 180.0
                             && degrees != 270.0 ) )
                    {
                        diagnostic( aResult, "error", "invalid_component_unit_rotation",
                                    "component unit rotation must be 0deg, 90deg, 180deg, or "
                                    "270deg" );
                    }
                    else
                    {
                        unit["rotationDegrees"] = static_cast<int>( degrees );
                    }

                    continue;
                }

                diagnostic( aResult, "error", "unknown_component_unit_field",
                            "component unit supports sheet, at, rotation, and mirror" );
            }

            for( const char* required : { "sheet", "position", "rotationDegrees", "mirror" } )
            {
                if( !unit.contains( required ) )
                {
                    diagnostic( aResult, "error", "missing_component_unit_field",
                                "component " + reference + " unit " + numberText
                                        + " is missing " + required );
                }
            }

            component["units"].emplace_back( std::move( unit ) );
            continue;
        }

        if( head == "property" )
        {
            std::string key;
            std::string value;

            if( field.children.size() != 3
                || !scalarText( aDocument, field.children[1], key )
                || !scalarText( aDocument, field.children[2], value ) || key.empty() )
            {
                diagnostic( aResult, "error", "invalid_component_property",
                            "component property requires a name and value" );
                continue;
            }

            if( !propertyNames.emplace( key ).second )
            {
                diagnostic( aResult, "error", "duplicate_component_property",
                            "component property '" + key + "' occurs more than once" );
                continue;
            }

            if( key == "Reference" || key == "Value" || key == "Footprint"
                || key == "Datasheet" || key == "Description" )
            {
                diagnostic( aResult, "error", "reserved_component_property",
                            "component property '" + key
                                    + "' is a native field controlled by KDS" );
                continue;
            }

            component["properties"][key] = value;
            continue;
        }

        if( head == "dnp" )
        {
            std::string value;

            if( !singletonFields.emplace( head ).second )
            {
                diagnostic( aResult, "error", "duplicate_component_field",
                            "component field 'dnp' occurs more than once" );
            }
            else if( !parseSingleValueForm( aDocument, child, value )
                || ( value != "true" && value != "false" ) )
            {
                diagnostic( aResult, "error", "invalid_component_dnp",
                            "component dnp must be true or false" );
            }
            else
            {
                component["dnp"] = value == "true";
            }

            continue;
        }

        if( head != "symbol" && head != "value" && head != "footprint" )
        {
            diagnostic( aResult, "error", "unknown_component_field",
                        "component supports symbol, value, footprint, property, dnp, and unit" );
            continue;
        }

        std::string value;

        if( !parseSingleValueForm( aDocument, child, value ) || value.empty() )
        {
            diagnostic( aResult, "error", "invalid_component_field",
                        "component symbol and value require one non-empty value; footprint "
                        "requires LIBRARY:ITEM or none" );
            continue;
        }

        if( !singletonFields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_component_field",
                        "component field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "footprint" && value == "none" )
        {
            component[head] = nullptr;
            continue;
        }

        if( head == "symbol" || head == "footprint" )
        {
            std::string nickname;

            if( !libraryIdNickname( value, nickname ) )
            {
                diagnostic( aResult, "error", "invalid_component_library_id",
                            "component " + head + " must use bounded LIBRARY:ITEM syntax" );
            }
        }

        component[head] = value;
    }

    for( const char* required : { "symbol", "value", "footprint" } )
    {
        if( !component.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_component_field",
                        "component " + reference + " is missing " + required );
        }
    }

    return component;
}


JSON compileNet( const DOCUMENT& aDocument, size_t aNode,
                 KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult,
                 std::vector<std::string>& aReferencedComponents,
                 std::set<std::string>& aConnectedPins, size_t& aPinConnections )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           name;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], name )
        || name.empty() || name.size() > MAX_IDENTIFIER_BYTES )
    {
        diagnostic( aResult, "error", "invalid_net", "net requires a bounded name" );
        return JSON::object();
    }

    JSON net = { { "name", name }, { "pins", JSON::array() } };

    for( size_t i = 2; i < node.children.size(); ++i )
    {
        const size_t child = node.children[i];
        const DOCUMENT::NODE& pin = aDocument.Nodes()[child];
        std::string reference;
        std::string unitText;
        std::string number;
        int64_t     unit = 0;

        if( aDocument.ListHead( child ) != "pin" || pin.children.size() != 4
            || !scalarText( aDocument, pin.children[1], reference )
            || !scalarText( aDocument, pin.children[2], unitText )
            || !scalarText( aDocument, pin.children[3], number )
            || !parseBoundedInteger( unitText, 1, 256, unit )
            || !validIdentifier( reference ) || number.empty() || number.size() > 64 )
        {
            diagnostic( aResult, "error", "invalid_pin",
                        "net endpoints must use (pin COMPONENT UNIT PIN_NUMBER)" );
            continue;
        }

        net["pins"].push_back( { { "component", reference },
                                 { "unit", unit },
                                 { "number", number } } );
        aReferencedComponents.emplace_back( reference );

        const std::string endpoint = reference + ":" + unitText + ":" + number;

        if( !aConnectedPins.emplace( endpoint ).second )
        {
            diagnostic( aResult, "error", "duplicate_pin_connection",
                        "pin " + endpoint + " is assigned to more than one net endpoint" );
        }

        ++aPinConnections;
    }

    if( net["pins"].size() < 2 )
    {
        diagnostic( aResult, "error", "underspecified_net",
                    "net " + name + " must connect at least two pins" );
    }

    return net;
}


JSON compileNoConnect( const DOCUMENT& aDocument, size_t aNode,
                       KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult,
                       std::vector<std::string>& aReferencedComponents,
                       std::set<std::string>& aConnectedPins, size_t& aPinConnections )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           reference;
    std::string           unitText;
    std::string           number;
    int64_t               unit = 0;

    if( node.children.size() != 4
        || !scalarText( aDocument, node.children[1], reference )
        || !scalarText( aDocument, node.children[2], unitText )
        || !scalarText( aDocument, node.children[3], number )
        || !validIdentifier( reference )
        || !parseBoundedInteger( unitText, 1, 256, unit )
        || number.empty() || number.size() > 64 )
    {
        diagnostic( aResult, "error", "invalid_no_connect",
                    "no_connect must use (no_connect COMPONENT UNIT PIN_NUMBER)" );
        return JSON::object();
    }

    const std::string endpoint = reference + ":" + unitText + ":" + number;

    if( !aConnectedPins.emplace( endpoint ).second )
    {
        diagnostic( aResult, "error", "duplicate_pin_connection",
                    "pin " + endpoint + " is assigned more than once" );
    }

    aReferencedComponents.emplace_back( reference );
    ++aPinConnections;
    return { { "component", reference }, { "unit", unit }, { "number", number } };
}


bool parseSchematicPoint( const DOCUMENT& aDocument, size_t aNode, JSON& aPoint )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string xText;
    std::string yText;
    int64_t x = 0;
    int64_t y = 0;

    if( node.children.size() != 3
        || !scalarText( aDocument, node.children[1], xText )
        || !scalarText( aDocument, node.children[2], yText )
        || !parseDistance( xText, x ) || !parseDistance( yText, y )
        || x < 0 || y < 0 || x > 2'000'000'000 || y > 2'000'000'000 )
    {
        return false;
    }

    aPoint = { { "xNm", x }, { "yNm", y } };
    return true;
}


bool parseSchematicStroke( const DOCUMENT& aDocument, size_t aNode, JSON& aStroke )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string widthText;
    std::string lineStyle;
    int64_t width = 0;
    static const std::set<std::string> STYLES = {
        "default", "solid", "dash", "dot", "dash_dot", "dash_dot_dot"
    };

    if( node.children.size() != 3
        || !scalarText( aDocument, node.children[1], widthText )
        || !scalarText( aDocument, node.children[2], lineStyle )
        || !STYLES.contains( lineStyle )
        || ( widthText != "default"
             && ( !parseDistance( widthText, width ) || width < 10'000
                  || width > 10'000'000 ) ) )
    {
        return false;
    }

    aStroke = { { "widthNm", widthText == "default" ? 0 : width },
                { "lineStyle", lineStyle } };
    return true;
}


JSON compileSchematicLine( const DOCUMENT& aDocument, size_t aNode,
                           const std::string& aKind,
                           KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string id;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], id )
        || !validIdentifier( id ) )
    {
        diagnostic( aResult, "error", "invalid_schematic_" + aKind,
                    aKind + " requires a bounded stable ID" );
        return JSON::object();
    }

    JSON line = { { "kind", aKind }, { "id", id } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_schematic_" + aKind + "_field",
                        aKind + " field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "sheet" )
        {
            std::string sheet;

            if( !parseSingleValueForm( aDocument, child, sheet )
                || !validIdentifier( sheet ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_" + aKind + "_sheet",
                            aKind + " sheet must be a bounded sheet ID" );
            }
            else
            {
                line["sheet"] = sheet;
            }
        }
        else if( head == "from" || head == "to" )
        {
            JSON point;

            if( !parseSchematicPoint( aDocument, child, point ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_" + aKind + "_point",
                            aKind + " " + head + " requires two distances from 0 to 2 m" );
            }
            else
            {
                line[head] = std::move( point );
            }
        }
        else if( head == "stroke" )
        {
            JSON stroke;

            if( !parseSchematicStroke( aDocument, child, stroke ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_" + aKind + "_stroke",
                            aKind + " stroke requires default or 0.01 mm through 10 mm and "
                                    "a native line style" );
            }
            else
            {
                line["stroke"] = std::move( stroke );
            }
        }
        else
        {
            diagnostic( aResult, "error", "unknown_schematic_" + aKind + "_field",
                        aKind + " supports sheet, from, to, and stroke" );
        }
    }

    for( const char* required : { "sheet", "from", "to", "stroke" } )
    {
        if( !line.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_schematic_" + aKind + "_field",
                        aKind + " " + id + " is missing " + required );
        }
    }

    if( line.contains( "from" ) && line.contains( "to" ) && line["from"] == line["to"] )
    {
        diagnostic( aResult, "error", "zero_length_schematic_" + aKind,
                    aKind + " " + id + " must have distinct endpoints" );
    }

    if( aKind == "bus_entry" && line.contains( "from" ) && line.contains( "to" ) )
    {
        const int64_t dx = line["to"]["xNm"].get<int64_t>()
                           - line["from"]["xNm"].get<int64_t>();
        const int64_t dy = line["to"]["yNm"].get<int64_t>()
                           - line["from"]["yNm"].get<int64_t>();

        if( dx == 0 || dy == 0 || std::abs( dx ) > 10'000'000
            || std::abs( dy ) > 10'000'000 )
        {
            diagnostic( aResult, "error", "invalid_schematic_bus_entry_geometry",
                        "bus_entry must be diagonal and at most 10 mm on each axis" );
        }
    }

    return line;
}


JSON compileSchematicJunction( const DOCUMENT& aDocument, size_t aNode,
                               KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string id;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], id )
        || !validIdentifier( id ) )
    {
        diagnostic( aResult, "error", "invalid_schematic_junction",
                    "junction requires a bounded stable ID" );
        return JSON::object();
    }

    JSON junction = { { "kind", "junction" }, { "id", id } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_schematic_junction_field",
                        "junction field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "sheet" )
        {
            std::string sheet;

            if( !parseSingleValueForm( aDocument, child, sheet )
                || !validIdentifier( sheet ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_junction_sheet",
                            "junction sheet must be a bounded sheet ID" );
            }
            else
            {
                junction["sheet"] = sheet;
            }
        }
        else if( head == "at" )
        {
            JSON point;

            if( !parseSchematicPoint( aDocument, child, point ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_junction_position",
                            "junction at requires two distances from 0 to 2 m" );
            }
            else
            {
                junction["position"] = std::move( point );
            }
        }
        else if( head == "diameter" )
        {
            std::string value;
            int64_t diameter = 0;

            if( !parseSingleValueForm( aDocument, child, value )
                || ( value != "auto"
                     && ( !parseDistance( value, diameter ) || diameter < 10'000
                          || diameter > 10'000'000 ) ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_junction_diameter",
                            "junction diameter requires auto or 0.01 mm through 10 mm" );
            }
            else
            {
                junction["diameterNm"] = value == "auto" ? 0 : diameter;
            }
        }
        else if( head == "color" )
        {
            std::string value;
            JSON color;

            if( !parseSingleValueForm( aDocument, child, value )
                || ( value != "default" && !parseHexColor( value, color ) ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_junction_color",
                            "junction color requires default or #RRGGBB[AA]" );
            }
            else if( value == "default" )
            {
                junction["color"] = nullptr;
            }
            else
            {
                junction["color"] = {
                    { "r", std::lround( color["r"].get<double>() * 255.0 ) },
                    { "g", std::lround( color["g"].get<double>() * 255.0 ) },
                    { "b", std::lround( color["b"].get<double>() * 255.0 ) },
                    { "a", std::lround( color["a"].get<double>() * 255.0 ) }
                };
            }
        }
        else
        {
            diagnostic( aResult, "error", "unknown_schematic_junction_field",
                        "junction supports sheet, at, diameter, and color" );
        }
    }

    for( const char* required : { "sheet", "position", "diameterNm", "color" } )
    {
        if( !junction.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_schematic_junction_field",
                        "junction " + id + " is missing " + required );
        }
    }

    return junction;
}


JSON compileSchematicLabel( const DOCUMENT& aDocument, size_t aNode,
                            KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string id;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], id )
        || !validIdentifier( id ) )
    {
        diagnostic( aResult, "error", "invalid_schematic_label",
                    "label requires a bounded stable ID" );
        return JSON::object();
    }

    JSON label = { { "kind", "label" }, { "id", id } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_schematic_label_field",
                        "label field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "sheet" || head == "net" || head == "scope" || head == "shape" )
        {
            std::string value;

            if( !parseSingleValueForm( aDocument, child, value )
                || ( head == "sheet" && !validIdentifier( value ) )
                || ( head == "net"
                     && ( value.empty() || value.size() > MAX_IDENTIFIER_BYTES ) )
                || ( head == "scope" && value != "local" && value != "global" )
                || ( head == "shape" && value != "none" && value != "input"
                     && value != "output" && value != "bidirectional"
                     && value != "tri_state" && value != "passive" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_label_" + head,
                            "label " + head + " has an invalid semantic value" );
            }
            else
            {
                label[head] = value;
            }

            continue;
        }

        if( head == "at" )
        {
            JSON point;

            if( !parseSchematicPoint( aDocument, child, point ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_label_position",
                            "label at requires two distances from 0 to 2 m" );
            }
            else
            {
                label["position"] = std::move( point );
            }

            continue;
        }

        if( head == "rotation" )
        {
            std::string value;
            double degrees = 0.0;

            if( !parseSingleValueForm( aDocument, child, value )
                || !parseFiniteDecimal( value, degrees, "deg" )
                || ( degrees != 0.0 && degrees != 90.0 && degrees != 180.0
                     && degrees != 270.0 ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_label_rotation",
                            "label rotation must be 0deg, 90deg, 180deg, or 270deg" );
            }
            else
            {
                label["rotationDegrees"] = static_cast<int>( degrees );
            }

            continue;
        }

        if( head == "size" )
        {
            JSON size;

            if( !parseSchematicPoint( aDocument, child, size )
                || size["xNm"].get<int64_t>() < 100'000
                || size["yNm"].get<int64_t>() < 100'000
                || size["xNm"].get<int64_t>() > 50'000'000
                || size["yNm"].get<int64_t>() > 50'000'000 )
            {
                diagnostic( aResult, "error", "invalid_schematic_label_size",
                            "label size requires two dimensions from 0.1 mm through 50 mm" );
            }
            else
            {
                label["size"] = std::move( size );
            }

            continue;
        }

        if( head == "thickness" )
        {
            std::string value;
            int64_t thickness = 0;

            if( !parseSingleValueForm( aDocument, child, value )
                || ( value != "auto"
                     && ( !parseDistance( value, thickness ) || thickness < 10'000
                          || thickness > 10'000'000 ) ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_label_thickness",
                            "label thickness requires auto or 0.01 mm through 10 mm" );
            }
            else
            {
                label["thicknessNm"] = value == "auto" ? 0 : thickness;
            }

            continue;
        }

        if( head == "justify" )
        {
            const DOCUMENT::NODE& justify = aDocument.Nodes()[child];
            std::string horizontal;
            std::string vertical;

            if( justify.children.size() != 3
                || !scalarText( aDocument, justify.children[1], horizontal )
                || !scalarText( aDocument, justify.children[2], vertical )
                || ( horizontal != "left" && horizontal != "center"
                     && horizontal != "right" )
                || ( vertical != "top" && vertical != "center" && vertical != "bottom" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_label_justify",
                            "label justify requires left|center|right and top|center|bottom" );
            }
            else
            {
                label["justify"] = { { "horizontal", horizontal },
                                     { "vertical", vertical } };
            }

            continue;
        }

        if( head == "bold" || head == "italic" )
        {
            std::string value;

            if( !parseSingleValueForm( aDocument, child, value )
                || ( value != "true" && value != "false" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_label_" + head,
                            "label " + head + " must be true or false" );
            }
            else
            {
                label[head] = value == "true";
            }

            continue;
        }

        diagnostic( aResult, "error", "unknown_schematic_label_field",
                    "label supports sheet, scope, net, at, rotation, shape, size, thickness, "
                    "justify, bold, and italic" );
    }

    for( const char* required : { "sheet", "scope", "net", "position", "rotationDegrees",
                                  "shape", "size", "thicknessNm", "justify", "bold",
                                  "italic" } )
    {
        if( !label.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_schematic_label_field",
                        "label " + id + " is missing " + required );
        }
    }

    if( label.contains( "scope" ) && label.contains( "shape" )
        && ( ( label["scope"] == "local" && label["shape"] != "none" )
             || ( label["scope"] == "global" && label["shape"] == "none" ) ) )
    {
        diagnostic( aResult, "error", "invalid_schematic_label_scope_shape",
                    "local labels require shape none; global labels require an electrical shape" );
    }

    return label;
}


JSON compileSchematicDirectiveProperty(
        const DOCUMENT& aDocument, size_t aNode,
        KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult,
        const std::string& aDirectiveId )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string name;
    std::string value;

    if( node.children.size() < 3 || !scalarText( aDocument, node.children[1], name )
        || !scalarText( aDocument, node.children[2], value ) || name.empty()
        || name.size() > MAX_IDENTIFIER_BYTES
        || value.size() > MAX_SCHEMATIC_PROPERTY_BYTES )
    {
        diagnostic( aResult, "error", "invalid_schematic_directive_property",
                    "directive " + aDirectiveId
                            + " property requires a 1..128-byte name and a value up to 4096 bytes" );
        return JSON::object();
    }

    JSON property = { { "name", name }, { "value", value } };
    std::set<std::string> fields;

    for( size_t index = 3; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_schematic_directive_property_field",
                        "directive " + aDirectiveId + " property " + name + " field '"
                                + head + "' occurs more than once" );
            continue;
        }

        if( head == "at" || head == "size" )
        {
            JSON point;

            if( !parseSchematicPoint( aDocument, child, point )
                || ( head == "size"
                     && ( point["xNm"].get<int64_t>() < 100'000
                          || point["yNm"].get<int64_t>() < 100'000
                          || point["xNm"].get<int64_t>() > 50'000'000
                          || point["yNm"].get<int64_t>() > 50'000'000 ) ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_directive_property_" + head,
                            head == "at"
                                    ? "directive property at requires two distances from 0 to 2 m"
                                    : "directive property size requires two dimensions from 0.1 mm through 50 mm" );
            }
            else
            {
                property[head == "at" ? "position" : "size"] = std::move( point );
            }

            continue;
        }

        if( head == "rotation" )
        {
            std::string rotation;
            double degrees = 0.0;

            if( !parseSingleValueForm( aDocument, child, rotation )
                || !parseFiniteDecimal( rotation, degrees, "deg" )
                || ( degrees != 0.0 && degrees != 90.0 && degrees != 180.0
                     && degrees != 270.0 ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_directive_property_rotation",
                            "directive property rotation must be 0deg, 90deg, 180deg, or 270deg" );
            }
            else
            {
                property["rotationDegrees"] = static_cast<int>( degrees );
            }

            continue;
        }

        if( head == "thickness" )
        {
            std::string thicknessText;
            int64_t thickness = 0;

            if( !parseSingleValueForm( aDocument, child, thicknessText )
                || ( thicknessText != "auto"
                     && ( !parseDistance( thicknessText, thickness ) || thickness < 10'000
                          || thickness > 10'000'000 ) ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_directive_property_thickness",
                            "directive property thickness requires auto or 0.01 mm through 10 mm" );
            }
            else
            {
                property["thicknessNm"] = thicknessText == "auto" ? 0 : thickness;
            }

            continue;
        }

        if( head == "justify" )
        {
            const DOCUMENT::NODE& justify = aDocument.Nodes()[child];
            std::string horizontal;
            std::string vertical;

            if( justify.children.size() != 3
                || !scalarText( aDocument, justify.children[1], horizontal )
                || !scalarText( aDocument, justify.children[2], vertical )
                || ( horizontal != "left" && horizontal != "center"
                     && horizontal != "right" )
                || ( vertical != "top" && vertical != "center" && vertical != "bottom" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_directive_property_justify",
                            "directive property justify requires left|center|right and top|center|bottom" );
            }
            else
            {
                property["justify"] = { { "horizontal", horizontal },
                                         { "vertical", vertical } };
            }

            continue;
        }

        if( head == "bold" || head == "italic" || head == "visible" )
        {
            std::string boolean;

            if( !parseSingleValueForm( aDocument, child, boolean )
                || ( boolean != "true" && boolean != "false" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_directive_property_" + head,
                            "directive property " + head + " must be true or false" );
            }
            else
            {
                property[head] = boolean == "true";
            }

            continue;
        }

        diagnostic( aResult, "error", "unknown_schematic_directive_property_field",
                    "directive property supports at, rotation, size, thickness, justify, bold, "
                    "italic, and visible" );
    }

    for( const char* required : { "position", "rotationDegrees", "size", "thicknessNm",
                                  "justify", "bold", "italic", "visible" } )
    {
        if( !property.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_schematic_directive_property_field",
                        "directive " + aDirectiveId + " property " + name + " is missing "
                                + required );
        }
    }

    return property;
}


JSON compileSchematicDirective( const DOCUMENT& aDocument, size_t aNode,
                                KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string id;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], id )
        || !validIdentifier( id ) )
    {
        diagnostic( aResult, "error", "invalid_schematic_directive",
                    "directive requires a bounded stable ID" );
        return JSON::object();
    }

    JSON directive = { { "kind", "directive" }, { "id", id },
                       { "properties", JSON::array() } };
    std::set<std::string> fields;
    std::set<std::string> propertyNames;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "property" )
        {
            JSON property = compileSchematicDirectiveProperty( aDocument, child, aResult, id );
            const std::string name = property.value( "name", "" );

            if( !name.empty() && !propertyNames.emplace( name ).second )
            {
                diagnostic( aResult, "error", "duplicate_schematic_directive_property",
                            "directive " + id + " property " + name
                                    + " occurs more than once" );
            }

            directive["properties"].emplace_back( std::move( property ) );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_schematic_directive_field",
                        "directive field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "sheet" || head == "shape" )
        {
            std::string value;

            if( !parseSingleValueForm( aDocument, child, value )
                || ( head == "sheet" && !validIdentifier( value ) )
                || ( head == "shape" && value != "dot" && value != "round"
                     && value != "diamond" && value != "rectangle" ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_directive_" + head,
                            "directive " + head + " has an invalid semantic value" );
            }
            else
            {
                directive[head] = value;
            }

            continue;
        }

        if( head == "target" )
        {
            const DOCUMENT::NODE& target = aDocument.Nodes()[child];
            std::string kind;
            std::string name;

            if( target.children.size() != 3
                || !scalarText( aDocument, target.children[1], kind )
                || !scalarText( aDocument, target.children[2], name ) || kind != "net"
                || name.empty() || name.size() > MAX_IDENTIFIER_BYTES )
            {
                diagnostic( aResult, "error", "invalid_schematic_directive_target",
                            "directive target currently requires net and a bounded declared net name" );
            }
            else
            {
                directive["target"] = { { "kind", kind }, { "name", name } };
            }

            continue;
        }

        if( head == "at" )
        {
            JSON point;

            if( !parseSchematicPoint( aDocument, child, point ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_directive_position",
                            "directive at requires two distances from 0 to 2 m" );
            }
            else
            {
                directive["position"] = std::move( point );
            }

            continue;
        }

        if( head == "rotation" )
        {
            std::string rotation;
            double degrees = 0.0;

            if( !parseSingleValueForm( aDocument, child, rotation )
                || !parseFiniteDecimal( rotation, degrees, "deg" )
                || ( degrees != 0.0 && degrees != 90.0 && degrees != 180.0
                     && degrees != 270.0 ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_directive_rotation",
                            "directive rotation must be 0deg, 90deg, 180deg, or 270deg" );
            }
            else
            {
                directive["rotationDegrees"] = static_cast<int>( degrees );
            }

            continue;
        }

        if( head == "length" )
        {
            std::string lengthText;
            int64_t length = 0;

            if( !parseSingleValueForm( aDocument, child, lengthText )
                || !parseDistance( lengthText, length ) || length < 100'000
                || length > 50'000'000 )
            {
                diagnostic( aResult, "error", "invalid_schematic_directive_length",
                            "directive length requires 0.1 mm through 50 mm" );
            }
            else
            {
                directive["lengthNm"] = length;
            }

            continue;
        }

        diagnostic( aResult, "error", "unknown_schematic_directive_field",
                    "directive supports sheet, target, at, rotation, shape, length, and property" );
    }

    for( const char* required : { "sheet", "target", "position", "rotationDegrees", "shape",
                                  "lengthNm" } )
    {
        if( !directive.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_schematic_directive_field",
                        "directive " + id + " is missing " + required );
        }
    }

    if( directive["properties"].empty() )
    {
        diagnostic( aResult, "error", "missing_schematic_directive_property",
                    "directive " + id + " requires at least one property" );
    }
    else if( directive["properties"].size() > 64 )
    {
        diagnostic( aResult, "error", "too_many_schematic_directive_properties",
                    "directive " + id + " supports at most 64 properties" );
    }

    return directive;
}


JSON compileSchematicBusAlias( const DOCUMENT& aDocument, size_t aNode,
                               KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string name;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], name )
        || !validIdentifier( name ) )
    {
        diagnostic( aResult, "error", "invalid_schematic_bus_alias",
                    "bus_alias requires a bounded name" );
        return JSON::object();
    }

    JSON alias = { { "name", name }, { "members", JSON::array() } };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_schematic_bus_alias_field",
                        "bus_alias field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "sheet" )
        {
            std::string sheet;

            if( !parseSingleValueForm( aDocument, child, sheet )
                || !validIdentifier( sheet ) )
            {
                diagnostic( aResult, "error", "invalid_schematic_bus_alias_sheet",
                            "bus_alias sheet must be a bounded sheet ID" );
            }
            else
            {
                alias["sheet"] = sheet;
            }

            continue;
        }

        if( head == "members" )
        {
            const DOCUMENT::NODE& members = aDocument.Nodes()[child];
            std::set<std::string> uniqueMembers;

            for( size_t memberIndex = 1; memberIndex < members.children.size(); ++memberIndex )
            {
                std::string member;

                if( !scalarText( aDocument, members.children[memberIndex], member )
                    || member.empty() || member.size() > MAX_IDENTIFIER_BYTES )
                {
                    diagnostic( aResult, "error", "invalid_schematic_bus_alias_member",
                                "bus_alias members must be bounded net names" );
                    continue;
                }

                if( !uniqueMembers.emplace( member ).second )
                {
                    diagnostic( aResult, "error", "duplicate_schematic_bus_alias_member",
                                "bus_alias member " + member + " occurs more than once" );
                    continue;
                }

                alias["members"].push_back( member );
            }

            continue;
        }

        diagnostic( aResult, "error", "unknown_schematic_bus_alias_field",
                    "bus_alias supports sheet and members" );
    }

    if( !alias.contains( "sheet" ) )
    {
        diagnostic( aResult, "error", "missing_schematic_bus_alias_field",
                    "bus_alias " + name + " is missing sheet" );
    }

    if( !fields.contains( "members" ) || alias["members"].empty()
        || alias["members"].size() > 256 )
    {
        diagnostic( aResult, "error", "invalid_schematic_bus_alias_members",
                    "bus_alias " + name + " requires 1 through 256 unique members" );
    }

    return alias;
}


JSON compileSource( const DOCUMENT& aDocument, size_t aNode,
                    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult,
                    std::vector<std::string>& aReferencedComponents )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           reference;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], reference )
        || !validIdentifier( reference ) )
    {
        diagnostic( aResult, "error", "invalid_source",
                    "source requires a bounded component reference" );
        return JSON::object();
    }

    JSON source = { { "component", reference } };
    const std::set<std::string> allowed = {
        "manufacturer", "mpn",       "datasheet", "lifecycle", "supplier", "sku",
        "product_url",  "available", "verified_on", "quantity", "unit_price", "notes"
    };
    std::set<std::string> fields;

    const auto boundedString = []( const JSON& aValue, size_t aMaximum )
    {
        if( !aValue.is_string() )
            return false;

        const std::string& value = aValue.get_ref<const std::string&>();
        return !value.empty() && value.size() <= aMaximum
               && std::none_of( value.begin(), value.end(),
                                []( unsigned char aCharacter )
                                {
                                    return aCharacter < 0x20 || aCharacter == 0x7F;
                                } );
    };

    const auto httpsUrl = [&]( const JSON& aValue )
    {
        if( !boundedString( aValue, 4096 ) )
            return false;

        const std::string& value = aValue.get_ref<const std::string&>();

        if( !value.starts_with( "https://" ) || value.size() <= 8 )
            return false;

        const size_t authorityEnd = value.find_first_of( "/?#", 8 );
        const size_t authoritySize = ( authorityEnd == std::string::npos ? value.size()
                                                                            : authorityEnd )
                                     - 8;
        return authoritySize > 0 && value.find_first_of( " \t\r\n" ) == std::string::npos;
    };

    const auto isoDate = []( const JSON& aValue )
    {
        if( !aValue.is_string() )
            return false;

        const std::string& value = aValue.get_ref<const std::string&>();

        if( value.size() != 10 || value[4] != '-' || value[7] != '-' )
            return false;

        for( size_t i = 0; i < value.size(); ++i )
        {
            if( i != 4 && i != 7 && !std::isdigit( static_cast<unsigned char>( value[i] ) ) )
                return false;
        }

        const int year = std::stoi( value.substr( 0, 4 ) );
        const int month = std::stoi( value.substr( 5, 2 ) );
        const int day = std::stoi( value.substr( 8, 2 ) );
        static constexpr int DAYS[] = { 31, 28, 31, 30, 31, 30,
                                        31, 31, 30, 31, 30, 31 };

        if( year < 2000 || year > 9999 || month < 1 || month > 12 || day < 1 )
            return false;

        int maximumDay = DAYS[month - 1];

        if( month == 2 && ( year % 400 == 0 || ( year % 4 == 0 && year % 100 != 0 ) ) )
            maximumDay = 29;

        return day <= maximumDay;
    };

    for( size_t i = 2; i < node.children.size(); ++i )
    {
        const size_t      child = node.children[i];
        const std::string head = aDocument.ListHead( child );
        const DOCUMENT::NODE& field = aDocument.Nodes()[child];

        if( !allowed.contains( head ) || field.children.size() != 2
            || !isScalar( aDocument, field.children[1] ) )
        {
            diagnostic( aResult, "error", "invalid_source_field",
                        "source fields must use one supported scalar evidence value" );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_source_field",
                        "source field '" + head + "' occurs more than once" );
            continue;
        }

        JSON value = scalarValue( aDocument, field.children[1] );
        bool valid = true;

        if( head == "manufacturer" || head == "mpn" || head == "supplier" || head == "sku" )
            valid = boundedString( value, 512 );
        else if( head == "datasheet" || head == "product_url" )
            valid = httpsUrl( value );
        else if( head == "lifecycle" )
            valid = value.is_string()
                    && std::set<std::string>{ "active", "nrnd", "last_time_buy", "obsolete" }
                               .contains( value.get<std::string>() );
        else if( head == "verified_on" )
            valid = isoDate( value );
        else if( head == "available" )
            valid = value.is_number_integer() && value.get<int64_t>() >= 0
                    && value.get<int64_t>() <= 1'000'000'000'000LL;
        else if( head == "quantity" )
            valid = value.is_number_integer() && value.get<int64_t>() >= 1
                    && value.get<int64_t>() <= 1'000'000;
        else if( head == "unit_price" )
            valid = boundedString( value, 128 );
        else if( head == "notes" )
            valid = boundedString( value, 2048 );

        if( !valid )
        {
            diagnostic( aResult, "error", "invalid_source_value",
                        "source field '" + head + "' has an invalid evidence value" );
            continue;
        }

        source[head] = std::move( value );
    }

    if( !source.contains( "manufacturer" ) || !source.contains( "mpn" ) )
    {
        diagnostic( aResult, "warning", "incomplete_source",
                    "source for " + reference + " has no verified manufacturer and MPN pair" );
    }

    aReferencedComponents.emplace_back( reference );
    return source;
}


JSON compileRules( const DOCUMENT& aDocument, size_t aNode,
                   KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    struct DISTANCE_FIELD
    {
        const char* sourceName;
        const char* irName;
        int64_t     minimum;
        int64_t     maximum;
    };

    static constexpr DISTANCE_FIELD DISTANCES[] = {
        { "minimum_clearance", "minimumClearanceNm", 0, 25000000 },
        { "minimum_connection_width", "minimumConnectionWidthNm", 0, 100000000 },
        { "minimum_track_width", "minimumTrackWidthNm", 0, 25000000 },
        { "minimum_via_annular_width", "minimumViaAnnularWidthNm", 0, 25000000 },
        { "minimum_via_diameter", "minimumViaDiameterNm", 0, 25000000 },
        { "minimum_through_hole_diameter", "minimumThroughHoleDiameterNm", 0, 25000000 },
        { "minimum_microvia_diameter", "minimumMicroviaDiameterNm", 0, 10000000 },
        { "minimum_microvia_drill", "minimumMicroviaDrillNm", 0, 10000000 },
        { "minimum_hole_to_hole", "minimumHoleToHoleNm", 0, 10000000 },
        { "minimum_copper_to_hole_clearance", "minimumCopperToHoleClearanceNm", 0,
          100000000 },
        { "minimum_silkscreen_clearance", "minimumSilkscreenClearanceNm", -10000000,
          100000000 },
        { "minimum_groove_width", "minimumGrooveWidthNm", 0, 25000000 },
        { "minimum_silkscreen_text_height", "minimumSilkscreenTextHeightNm", 0,
          100000000 },
        { "minimum_silkscreen_text_thickness", "minimumSilkscreenTextThicknessNm", 0,
          25000000 },
        { "maximum_error", "maximumErrorNm", 1000, 100000 }
    };
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    const std::set<std::string> allowed = {
        "minimum_clearance",
        "minimum_connection_width",
        "minimum_track_width",
        "minimum_via_annular_width",
        "minimum_via_diameter",
        "minimum_through_hole_diameter",
        "minimum_microvia_diameter",
        "minimum_microvia_drill",
        "minimum_hole_to_hole",
        "minimum_copper_to_hole_clearance",
        "minimum_silkscreen_clearance",
        "minimum_groove_width",
        "minimum_resolved_spokes",
        "minimum_silkscreen_text_height",
        "minimum_silkscreen_text_thickness",
        "minimum_copper_to_edge_clearance",
        "use_height_for_length_calculations",
        "maximum_error",
        "allow_fillets_outside_zone_outline"
    };
    std::map<std::string, size_t> fields;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t      fieldNode = node.children[index];
        const std::string name = aDocument.ListHead( fieldNode );

        if( !allowed.contains( name ) )
        {
            diagnostic( aResult, "error", "unknown_rules_field",
                        "rules contains an unknown global board constraint" );
            continue;
        }

        if( !fields.emplace( name, fieldNode ).second )
        {
            diagnostic( aResult, "error", "duplicate_rules_field",
                        "rules field '" + name + "' occurs more than once" );
        }
    }

    JSON rules = { { "kind", "rules" } };

    for( const DISTANCE_FIELD& specification : DISTANCES )
    {
        int64_t     value = 0;
        std::string text;
        const auto  found = fields.find( specification.sourceName );

        if( found == fields.end()
            || !parseSingleValueForm( aDocument, found->second, text )
            || !parseDistance( text, value ) || value < specification.minimum
            || value > specification.maximum )
        {
            diagnostic( aResult, "error", "invalid_rules_distance",
                        std::string( "rules " ) + specification.sourceName
                                + " is required and outside its native range" );
        }

        rules[specification.irName] = value;
    }

    int         resolvedSpokes = 0;
    std::string resolvedSpokesText;
    auto        spokesField = fields.find( "minimum_resolved_spokes" );

    if( spokesField == fields.end()
        || !parseSingleValueForm( aDocument, spokesField->second, resolvedSpokesText ) )
    {
        diagnostic( aResult, "error", "invalid_rules_resolved_spokes",
                    "rules minimum_resolved_spokes is required from 0 through 99" );
    }
    else
    {
        std::from_chars_result converted =
                std::from_chars( resolvedSpokesText.data(),
                                 resolvedSpokesText.data() + resolvedSpokesText.size(),
                                 resolvedSpokes );

        if( converted.ec != std::errc()
            || converted.ptr != resolvedSpokesText.data() + resolvedSpokesText.size()
            || resolvedSpokes < 0 || resolvedSpokes > 99 )
        {
            diagnostic( aResult, "error", "invalid_rules_resolved_spokes",
                        "rules minimum_resolved_spokes is required from 0 through 99" );
        }
    }

    rules["minimumResolvedSpokes"] = resolvedSpokes;
    bool        useHeight = false;
    std::string useHeightText;
    auto        useHeightField = fields.find( "use_height_for_length_calculations" );

    if( useHeightField == fields.end()
        || !parseSingleValueForm( aDocument, useHeightField->second, useHeightText )
        || ( useHeightText != "true" && useHeightText != "false" ) )
    {
        diagnostic( aResult, "error", "invalid_rules_length_policy",
                    "rules use_height_for_length_calculations must be explicitly true or false" );
    }
    else
    {
        useHeight = useHeightText == "true";
    }

    rules["useHeightForLengthCalculations"] = useHeight;
    bool        allowExternalFillets = false;
    std::string allowExternalFilletsText;
    auto        allowExternalFilletsField = fields.find( "allow_fillets_outside_zone_outline" );

    if( allowExternalFilletsField == fields.end()
        || !parseSingleValueForm( aDocument, allowExternalFilletsField->second,
                                  allowExternalFilletsText )
        || ( allowExternalFilletsText != "true" && allowExternalFilletsText != "false" ) )
    {
        diagnostic( aResult, "error", "invalid_rules_zone_fillet_policy",
                    "rules allow_fillets_outside_zone_outline must be explicitly true or false" );
    }
    else
    {
        allowExternalFillets = allowExternalFilletsText == "true";
    }

    rules["allowFilletsOutsideZoneOutline"] = allowExternalFillets;
    int64_t     edgeClearance = 0;
    std::string edgeText;
    std::string edgeMode;
    auto        edgeField = fields.find( "minimum_copper_to_edge_clearance" );

    if( edgeField == fields.end()
        || !parseSingleValueForm( aDocument, edgeField->second, edgeText ) )
    {
        diagnostic( aResult, "error", "invalid_rules_edge_clearance",
                    "rules minimum_copper_to_edge_clearance requires legacy or a distance" );
    }
    else if( edgeText == "legacy" )
    {
        edgeMode = "legacy";
    }
    else if( !parseDistance( edgeText, edgeClearance ) || edgeClearance < 0
             || edgeClearance > 25000000 )
    {
        diagnostic( aResult, "error", "invalid_rules_edge_clearance",
                    "rules minimum_copper_to_edge_clearance requires legacy or 0mm through 25mm" );
    }
    else
    {
        edgeMode = "explicit";
    }

    rules["copperEdgeClearanceMode"] = edgeMode;
    rules["minimumCopperToEdgeClearanceNm"] = edgeClearance;
    const int64_t annular = rules["minimumViaAnnularWidthNm"].get<int64_t>();

    if( rules["minimumViaDiameterNm"].get<int64_t>()
        < rules["minimumThroughHoleDiameterNm"].get<int64_t>() + 2 * annular )
    {
        diagnostic( aResult, "error", "inconsistent_rules_via_geometry",
                    "minimum_via_diameter cannot satisfy the drill and annular-width constraints" );
    }

    if( rules["minimumMicroviaDiameterNm"].get<int64_t>()
        < rules["minimumMicroviaDrillNm"].get<int64_t>() + 2 * annular )
    {
        diagnostic( aResult, "error", "inconsistent_rules_microvia_geometry",
                    "minimum_microvia_diameter cannot satisfy the drill and annular-width constraints" );
    }

    return rules;
}


JSON compileNetClass( const DOCUMENT& aDocument, size_t aNode, size_t aPriority,
                      KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    struct DISTANCE_FIELD
    {
        const char* sourceName;
        const char* irName;
        int64_t     minimum;
        int64_t     maximum;
        int64_t     quantum;
    };

    static constexpr DISTANCE_FIELD DISTANCES[] = {
        { "clearance", "clearanceNm", 0, 500000000, 1 },
        { "track_width", "trackWidthNm", 1, 25000000, 1 },
        { "via_diameter", "viaDiameterNm", 1, 25000000, 1 },
        { "via_drill", "viaDrillNm", 1, 25000000, 1 },
        { "microvia_diameter", "microviaDiameterNm", 1, 10000000, 1 },
        { "microvia_drill", "microviaDrillNm", 1, 10000000, 1 },
        { "diff_pair_width", "diffPairWidthNm", 1, 25000000, 1 },
        { "diff_pair_gap", "diffPairGapNm", 0, 100000000, 1 },
        { "diff_pair_via_gap", "diffPairViaGapNm", 0, 100000000, 1 },
        { "wire_width", "wireWidthNm", 100, 100000000, 100 },
        { "bus_width", "busWidthNm", 100, 100000000, 100 }
    };
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string name;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], name )
        || name.empty() || name.size() > 256 )
    {
        diagnostic( aResult, "error", "invalid_netclass_name",
                    "class requires a non-empty name of at most 256 bytes" );
        name.clear();
    }

    const bool isDefault = name == "Default";
    const std::set<std::string> allowed = {
        "clearance",          "track_width",       "via_diameter",
        "via_drill",          "microvia_diameter", "microvia_drill",
        "diff_pair_width",    "diff_pair_gap",     "diff_pair_via_gap",
        "tuning_profile",     "pcb_color",         "wire_width",
        "bus_width",          "schematic_color",   "line_style"
    };
    std::map<std::string, size_t> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t fieldNode = node.children[index];
        const std::string fieldName = aDocument.ListHead( fieldNode );

        if( !allowed.contains( fieldName ) )
        {
            diagnostic( aResult, "error", "unknown_netclass_field",
                        "class contains an unknown netclass field" );
            continue;
        }

        if( !fields.emplace( fieldName, fieldNode ).second )
        {
            diagnostic( aResult, "error", "duplicate_netclass_field",
                        "netclass field '" + fieldName + "' occurs more than once" );
        }
    }

    JSON netClass = { { "name", name },
                      { "priority", isDefault ? std::numeric_limits<int32_t>::max()
                                               : static_cast<int64_t>( aPriority ) } };

    for( const DISTANCE_FIELD& specification : DISTANCES )
    {
        const auto found = fields.find( specification.sourceName );
        std::string valueText;

        if( found == fields.end()
            || !parseSingleValueForm( aDocument, found->second, valueText ) )
        {
            diagnostic( aResult, "error", "invalid_netclass_distance",
                        std::string( "class " ) + specification.sourceName
                                + " is required with one distance or inherit" );
            netClass[specification.irName] = nullptr;
            continue;
        }

        if( valueText == "inherit" )
        {
            if( isDefault )
            {
                diagnostic( aResult, "error", "invalid_default_netclass_inheritance",
                            std::string( "Default class cannot inherit " )
                                    + specification.sourceName );
            }

            netClass[specification.irName] = nullptr;
            continue;
        }

        int64_t value = 0;

        if( !parseDistance( valueText, value ) || value < specification.minimum
            || value > specification.maximum || value % specification.quantum != 0 )
        {
            diagnostic( aResult, "error", "invalid_netclass_distance",
                        std::string( "class " ) + specification.sourceName
                                + " is outside its exact native range or resolution" );
        }

        netClass[specification.irName] = value;
    }

    const auto parsePolicy = [&]( const char* aField, const char* aIrName,
                                  const std::set<std::string>& aValues )
    {
        const auto found = fields.find( aField );
        std::string value;

        if( found == fields.end()
            || !parseSingleValueForm( aDocument, found->second, value )
            || !aValues.contains( value ) )
        {
            diagnostic( aResult, "error", "invalid_netclass_policy",
                        std::string( "class " ) + aField + " has an invalid semantic value" );
            netClass[aIrName] = nullptr;
            return;
        }

        if( value == "inherit" )
        {
            if( isDefault )
            {
                diagnostic( aResult, "error", "invalid_default_netclass_inheritance",
                            std::string( "Default class cannot inherit " ) + aField );
            }

            netClass[aIrName] = nullptr;
        }
        else
        {
            netClass[aIrName] = value;
        }
    };

    parsePolicy( "line_style", "lineStyle",
                 { "inherit", "solid", "dash", "dot", "dash_dot", "dash_dot_dot" } );

    const auto parseColor = [&]( const char* aField, const char* aIrName )
    {
        const auto found = fields.find( aField );
        std::string value;
        JSON color;

        if( found == fields.end()
            || !parseSingleValueForm( aDocument, found->second, value ) )
        {
            diagnostic( aResult, "error", "invalid_netclass_color",
                        std::string( "class " ) + aField
                                + " requires default, inherit, or #RRGGBBAA" );
            netClass[aIrName] = nullptr;
        }
        else if( ( isDefault && value == "default" )
                 || ( !isDefault && value == "inherit" ) )
        {
            netClass[aIrName] = nullptr;
        }
        else if( ( isDefault && value == "inherit" ) || ( !isDefault && value == "default" )
                 || !parseHexColor( value, color ) )
        {
            diagnostic( aResult, "error", "invalid_netclass_color",
                        std::string( "class " ) + aField
                                + " uses a color policy that is invalid for this class" );
            netClass[aIrName] = nullptr;
        }
        else
        {
            netClass[aIrName] = std::move( color );
        }
    };

    parseColor( "pcb_color", "pcbColor" );
    parseColor( "schematic_color", "schematicColor" );
    const auto tuningField = fields.find( "tuning_profile" );
    std::string tuningProfile;

    if( tuningField == fields.end()
        || !parseSingleValueForm( aDocument, tuningField->second, tuningProfile )
        || tuningProfile.empty() || tuningProfile.size() > 256
        || ( isDefault && tuningProfile == "inherit" )
        || ( !isDefault && tuningProfile == "none" ) )
    {
        diagnostic( aResult, "error", "invalid_netclass_tuning_profile",
                    "Default tuning_profile requires none or a name; other classes require "
                    "inherit or a name" );
        netClass["tuningProfile"] = nullptr;
    }
    else if( tuningProfile == "none" || tuningProfile == "inherit" )
    {
        netClass["tuningProfile"] = nullptr;
    }
    else
    {
        netClass["tuningProfile"] = tuningProfile;
    }

    return netClass;
}


JSON compileNetClasses( const DOCUMENT& aDocument, size_t aNode,
                        KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    JSON classes = JSON::array();
    JSON assignments = JSON::array();
    std::set<std::string> foldedNames;
    std::set<std::string> exactNames;
    std::set<std::pair<std::string, std::string>> assignmentPairs;
    bool sawAssignment = false;
    size_t userPriority = 0;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "class" )
        {
            if( sawAssignment )
            {
                diagnostic( aResult, "error", "noncanonical_netclass_order",
                            "all class declarations must precede netclass assignments" );
            }

            JSON netClass = compileNetClass( aDocument, child, userPriority, aResult );
            const std::string name = netClass.value( "name", "" );
            wxString folded = wxString::FromUTF8( name ).Lower();
            const wxScopedCharBuffer foldedBuffer = folded.ToUTF8();
            const std::string foldedUtf8 = foldedBuffer.data() ? foldedBuffer.data() : "";

            if( !name.empty() && !foldedNames.emplace( foldedUtf8 ).second )
            {
                diagnostic( aResult, "error", "duplicate_netclass",
                            "netclass names must be unique without regard to case" );
            }

            exactNames.emplace( name );

            if( name != "Default" )
                ++userPriority;

            classes.emplace_back( std::move( netClass ) );
            continue;
        }

        if( head != "assign" )
        {
            diagnostic( aResult, "error", "unknown_net_classes_form",
                        "net_classes accepts only class and assign forms" );
            continue;
        }

        sawAssignment = true;
        const DOCUMENT::NODE& assignmentNode = aDocument.Nodes()[child];
        std::map<std::string, size_t> fields;

        for( size_t fieldIndex = 1; fieldIndex < assignmentNode.children.size(); ++fieldIndex )
        {
            const size_t fieldNode = assignmentNode.children[fieldIndex];
            const std::string fieldName = aDocument.ListHead( fieldNode );

            if( fieldName != "pattern" && fieldName != "classes" )
            {
                diagnostic( aResult, "error", "unknown_netclass_assignment_field",
                            "assign supports only pattern and classes" );
                continue;
            }

            if( !fields.emplace( fieldName, fieldNode ).second )
            {
                diagnostic( aResult, "error", "duplicate_netclass_assignment_field",
                            "netclass assignment fields may occur once" );
            }
        }

        std::string pattern;
        const auto patternField = fields.find( "pattern" );

        if( patternField == fields.end()
            || !parseSingleValueForm( aDocument, patternField->second, pattern )
            || pattern.empty() || pattern.size() > 256 || !boundedBusRanges( pattern ) )
        {
            diagnostic( aResult, "error", "invalid_netclass_pattern",
                        "assign pattern must be non-empty, at most 256 bytes, and have bounded "
                        "bus ranges" );
        }

        JSON classNames = JSON::array();
        const auto classesField = fields.find( "classes" );

        if( classesField == fields.end() )
        {
            diagnostic( aResult, "error", "invalid_netclass_assignment",
                        "assign requires a non-empty classes list" );
        }
        else
        {
            const DOCUMENT::NODE& classesNode = aDocument.Nodes()[classesField->second];
            std::set<std::string> seenClasses;

            if( classesNode.children.size() < 2 )
            {
                diagnostic( aResult, "error", "invalid_netclass_assignment",
                            "assign requires a non-empty classes list" );
            }

            for( size_t classIndex = 1; classIndex < classesNode.children.size(); ++classIndex )
            {
                std::string className;

                if( !scalarText( aDocument, classesNode.children[classIndex], className )
                    || className.empty() || className.size() > 256 )
                {
                    diagnostic( aResult, "error", "invalid_netclass_assignment",
                                "assignment class names must be bounded scalar values" );
                    continue;
                }

                if( className == "Default" || !exactNames.contains( className ) )
                {
                    diagnostic( aResult, "error", "unknown_netclass_assignment",
                                "assignment references an unknown or redundant Default class" );
                }

                if( !seenClasses.emplace( className ).second
                    || !assignmentPairs.emplace( pattern, className ).second )
                {
                    diagnostic( aResult, "error", "duplicate_netclass_assignment",
                                "pattern/class assignments must be unique" );
                }

                classNames.emplace_back( className );
            }
        }

        assignments.push_back( { { "pattern", pattern }, { "classes", std::move( classNames ) } } );
    }

    if( classes.empty() || classes.front().value( "name", "" ) != "Default"
        || std::count_if( classes.begin(), classes.end(),
                          []( const JSON& aClass )
                          {
                              return aClass.value( "name", "" ) == "Default";
                          } ) != 1 )
    {
        diagnostic( aResult, "error", "invalid_default_netclass",
                    "net_classes requires exactly one Default class as its first declaration" );
    }

    if( classes.size() > 256 )
    {
        diagnostic( aResult, "error", "too_many_netclasses",
                    "net_classes supports at most 256 classes" );
    }

    if( assignmentPairs.size() > 1024 )
    {
        diagnostic( aResult, "error", "too_many_netclass_assignments",
                    "net_classes supports at most 1024 pattern/class assignments" );
    }

    if( !classes.empty() && classes.front().value( "name", "" ) == "Default"
        && classes.front()["viaDiameterNm"].is_number_integer()
        && classes.front()["viaDrillNm"].is_number_integer()
        && classes.front()["microviaDiameterNm"].is_number_integer()
        && classes.front()["microviaDrillNm"].is_number_integer() )
    {
        const JSON& defaults = classes.front();

        if( defaults["viaDiameterNm"].get<int64_t>()
            < defaults["viaDrillNm"].get<int64_t>() )
        {
            diagnostic( aResult, "error", "inconsistent_netclass_via_geometry",
                        "Default netclass via diameter cannot be smaller than its drill" );
        }

        if( defaults["microviaDiameterNm"].get<int64_t>()
            < defaults["microviaDrillNm"].get<int64_t>() )
        {
            diagnostic( aResult, "error", "inconsistent_netclass_microvia_geometry",
                        "Default netclass microvia diameter cannot be smaller than its drill" );
        }

        for( size_t index = 1; index < classes.size(); ++index )
        {
            const JSON& netClass = classes[index];
            const int64_t viaDiameter = netClass["viaDiameterNm"].is_null()
                                                ? defaults["viaDiameterNm"].get<int64_t>()
                                                : netClass["viaDiameterNm"].get<int64_t>();
            const int64_t viaDrill = netClass["viaDrillNm"].is_null()
                                             ? defaults["viaDrillNm"].get<int64_t>()
                                             : netClass["viaDrillNm"].get<int64_t>();
            const int64_t microviaDiameter = netClass["microviaDiameterNm"].is_null()
                                                     ? defaults["microviaDiameterNm"].get<int64_t>()
                                                     : netClass["microviaDiameterNm"].get<int64_t>();
            const int64_t microviaDrill = netClass["microviaDrillNm"].is_null()
                                                  ? defaults["microviaDrillNm"].get<int64_t>()
                                                  : netClass["microviaDrillNm"].get<int64_t>();

            if( viaDiameter < viaDrill )
            {
                diagnostic( aResult, "error", "inconsistent_netclass_via_geometry",
                            "effective netclass via diameter cannot be smaller than its drill" );
            }

            if( microviaDiameter < microviaDrill )
            {
                diagnostic( aResult, "error", "inconsistent_netclass_microvia_geometry",
                            "effective netclass microvia diameter cannot be smaller than its drill" );
            }
        }
    }

    return { { "kind", "net_classes" },
             { "classes", std::move( classes ) },
             { "assignments", std::move( assignments ) } };
}


JSON compileCustomRuleValue( const std::string& aText, const std::string& aDomain,
                             KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult,
                             const std::string& aConstraint )
{
    if( aDomain == "distance" || aDomain == "distance_or_time" )
    {
        int64_t nanometers = 0;

        if( parseDistance( aText, nanometers )
            && nanometers >= -10000000000LL && nanometers <= 10000000000LL )
        {
            return { { "domain", "distance" }, { "value", nanometers } };
        }

        if( aDomain == "distance_or_time" )
        {
            int64_t femtoseconds = 0;

            if( parseTime( aText, femtoseconds )
                && femtoseconds >= -1000000000000000LL
                && femtoseconds <= 1000000000000000LL )
            {
                return { { "domain", "time" }, { "value", femtoseconds } };
            }
        }

        diagnostic( aResult, "error", "invalid_custom_rule_value",
                    "custom rule constraint " + aConstraint
                            + " requires a bounded distance"
                            + ( aDomain == "distance_or_time" ? " or time" : "" )
                            + " literal" );
        return JSON::object();
    }

    if( aDomain == "integer" )
    {
        int64_t value = 0;

        if( !parseBoundedInteger( aText, 0, 1000000, value ) )
        {
            diagnostic( aResult, "error", "invalid_custom_rule_value",
                        "custom rule constraint " + aConstraint
                                + " requires an integer from 0 through 1000000" );
        }

        return { { "domain", "integer" }, { "value", value } };
    }

    double degrees = 0.0;

    if( !parseFiniteDecimal( aText, degrees, "deg" ) || degrees < 0.0 || degrees > 180.0 )
    {
        diagnostic( aResult, "error", "invalid_custom_rule_value",
                    "custom rule track_angle values require 0deg through 180deg" );
    }

    return { { "domain", "angle" }, { "value", degrees } };
}


JSON compileCustomConstraint( const DOCUMENT& aDocument, size_t aNode,
                              KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           type;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], type ) )
    {
        diagnostic( aResult, "error", "invalid_custom_constraint",
                    "custom rule constraints require a supported type" );
        return JSON::object();
    }

    if( type == "via_dangling" || type == "bridged_mask" )
    {
        if( node.children.size() != 2 )
        {
            diagnostic( aResult, "error", "invalid_custom_constraint",
                        type + " does not accept values" );
        }

        return { { "type", type }, { "kind", "flag" } };
    }

    if( type == "assertion" )
    {
        std::string test;

        if( node.children.size() != 3 || aDocument.ListHead( node.children[2] ) != "test"
            || !parseSingleValueForm( aDocument, node.children[2], test ) || test.empty()
            || test.size() > 8192 || test.find( "${" ) != std::string::npos )
        {
            diagnostic( aResult, "error", "invalid_custom_assertion",
                        "assertion requires one bounded (test EXPRESSION)" );
        }

        return { { "type", type }, { "kind", "assertion" }, { "test", test } };
    }

    if( type == "zone_connection" )
    {
        std::string style;

        if( node.children.size() != 3 || aDocument.ListHead( node.children[2] ) != "style"
            || !parseSingleValueForm( aDocument, node.children[2], style )
            || ( style != "solid" && style != "thermal_reliefs" && style != "none" ) )
        {
            diagnostic( aResult, "error", "invalid_custom_zone_connection",
                        "zone_connection requires (style solid|thermal_reliefs|none)" );
        }

        return { { "type", type }, { "kind", "zone_connection" }, { "style", style } };
    }

    if( type == "disallow" )
    {
        static const std::vector<std::string> ORDER = {
            "track", "via", "through_via", "blind_via", "buried_via", "micro_via",
            "pad", "zone", "text", "graphic", "hole", "footprint"
        };
        JSON items = JSON::array();
        std::set<std::string> seen;
        size_t previousRank = 0;
        bool haveRank = false;

        if( node.children.size() != 3 || aDocument.ListHead( node.children[2] ) != "items" )
        {
            diagnostic( aResult, "error", "invalid_custom_disallow",
                        "disallow requires one non-empty (items TYPE ...) list" );
            return { { "type", type }, { "kind", "disallow" }, { "items", items } };
        }

        const DOCUMENT::NODE& itemsNode = aDocument.Nodes()[node.children[2]];

        if( itemsNode.children.size() < 2 )
        {
            diagnostic( aResult, "error", "invalid_custom_disallow",
                        "disallow items cannot be empty" );
        }

        for( size_t index = 1; index < itemsNode.children.size(); ++index )
        {
            std::string item;

            if( !scalarText( aDocument, itemsNode.children[index], item ) )
            {
                diagnostic( aResult, "error", "invalid_custom_disallow",
                            "disallow items must be scalar object types" );
                continue;
            }

            const auto found = std::find( ORDER.begin(), ORDER.end(), item );

            if( found == ORDER.end() || !seen.emplace( item ).second )
            {
                diagnostic( aResult, "error", "invalid_custom_disallow",
                            "disallow contains an unknown or duplicate object type" );
                continue;
            }

            const size_t rank = static_cast<size_t>( found - ORDER.begin() );

            if( haveRank && rank <= previousRank )
            {
                diagnostic( aResult, "error", "noncanonical_custom_disallow_order",
                            "disallow items must use canonical object-type order" );
            }

            previousRank = rank;
            haveRank = true;
            items.emplace_back( item );
        }

        if( seen.contains( "via" )
            && ( seen.contains( "through_via" ) || seen.contains( "blind_via" )
                 || seen.contains( "buried_via" ) || seen.contains( "micro_via" ) ) )
        {
            diagnostic( aResult, "error", "redundant_custom_disallow",
                        "disallow via cannot be combined with individual via types" );
        }

        return { { "type", type }, { "kind", "disallow" }, { "items", items } };
    }

    if( type == "min_resolved_spokes" )
    {
        std::string valueText;
        int64_t     value = 0;

        if( node.children.size() != 3 || aDocument.ListHead( node.children[2] ) != "count"
            || !parseSingleValueForm( aDocument, node.children[2], valueText )
            || !parseBoundedInteger( valueText, 0, 4, value ) )
        {
            diagnostic( aResult, "error", "invalid_custom_spoke_count",
                        "min_resolved_spokes requires (count 0..4)" );
        }

        return { { "type", type }, { "kind", "count" }, { "value", value } };
    }

    if( type == "solder_paste_rel_margin" )
    {
        std::string valueText;
        double      value = 0.0;
        int64_t     valuePermille = 0;

        if( node.children.size() != 3 || aDocument.ListHead( node.children[2] ) != "ratio"
            || !parseSingleValueForm( aDocument, node.children[2], valueText )
            || !parseFiniteDecimal( valueText, value ) || value < -10.0 || value > 10.0
            || std::abs( value * 1000.0 - std::round( value * 1000.0 ) ) > 1e-9 )
        {
            diagnostic( aResult, "error", "invalid_custom_paste_ratio",
                        "solder_paste_rel_margin requires a finite (ratio VALUE) from -10 to 10 "
                        "at 0.001 resolution" );
        }
        else
        {
            valuePermille = static_cast<int64_t>( std::llround( value * 1000.0 ) );
        }

        return { { "type", type },
                 { "kind", "ratio" },
                 { "valuePermille", valuePermille } };
    }

    struct RANGE_SPEC
    {
        const char*            domain;
        std::set<std::string>  allowed;
        std::set<std::string>  required;
    };
    static const std::map<std::string, RANGE_SPEC> SPECS = {
        { "annular_width", { "distance", { "min", "max" }, {} } },
        { "clearance", { "distance", { "min" }, { "min" } } },
        { "creepage", { "distance", { "min" }, { "min" } } },
        { "connection_width", { "distance", { "min" }, { "min" } } },
        { "courtyard_clearance", { "distance", { "min" }, { "min" } } },
        { "diff_pair_gap", { "distance", { "min", "opt", "max" }, {} } },
        { "diff_pair_uncoupled", { "distance", { "max" }, { "max" } } },
        { "edge_clearance", { "distance", { "min" }, { "min" } } },
        { "hole_clearance", { "distance", { "min" }, { "min" } } },
        { "hole_size", { "distance", { "min", "opt", "max" }, {} } },
        { "hole_to_hole", { "distance", { "min" }, { "min" } } },
        { "length", { "distance_or_time", { "min", "opt", "max" }, {} } },
        { "physical_clearance", { "distance", { "min" }, { "min" } } },
        { "physical_hole_clearance", { "distance", { "min" }, { "min" } } },
        { "silk_clearance", { "distance", { "min" }, { "min" } } },
        { "skew", { "distance_or_time", { "min", "opt", "max" }, {} } },
        { "solder_mask_expansion", { "distance", { "opt" }, { "opt" } } },
        { "solder_mask_sliver", { "distance", { "min" }, { "min" } } },
        { "solder_paste_abs_margin", { "distance", { "opt" }, { "opt" } } },
        { "text_height", { "distance", { "min", "max" }, {} } },
        { "text_thickness", { "distance", { "min", "max" }, {} } },
        { "thermal_relief_gap", { "distance", { "min" }, { "min" } } },
        { "thermal_spoke_width", { "distance", { "opt" }, { "opt" } } },
        { "track_width", { "distance", { "min", "opt", "max" }, {} } },
        { "track_angle", { "angle", { "min", "max" }, {} } },
        { "track_segment_length", { "distance", { "min", "max" }, {} } },
        { "via_count", { "integer", { "min", "max" }, {} } },
        { "via_diameter", { "distance", { "min", "opt", "max" }, {} } }
    };
    const auto specification = SPECS.find( type );

    if( specification == SPECS.end() )
    {
        diagnostic( aResult, "error", "unknown_custom_constraint",
                    "unsupported custom rule constraint '" + type + "'" );
        return JSON::object();
    }

    JSON values = JSON::object();
    std::string skewDomain;
    int previousRank = -1;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t fieldNode = node.children[index];
        const std::string field = aDocument.ListHead( fieldNode );

        if( type == "skew" && field == "domain" )
        {
            if( !skewDomain.empty()
                || !parseSingleValueForm( aDocument, fieldNode, skewDomain )
                || ( skewDomain != "nets" && skewDomain != "diff_pairs" ) )
            {
                diagnostic( aResult, "error", "invalid_custom_skew_domain",
                            "skew requires one (domain nets|diff_pairs)" );
            }

            continue;
        }

        const auto allowed = specification->second.allowed.find( field );
        const int rank = field == "min" ? 0 : field == "opt" ? 1 : field == "max" ? 2 : -1;
        std::string text;

        if( allowed == specification->second.allowed.end() || rank < 0
            || !parseSingleValueForm( aDocument, fieldNode, text ) )
        {
            diagnostic( aResult, "error", "invalid_custom_constraint_field",
                        "constraint " + type + " contains an unsupported value field" );
            continue;
        }

        if( values.contains( field ) || rank <= previousRank )
        {
            diagnostic( aResult, "error", "noncanonical_custom_constraint_values",
                        "constraint values must occur once in min, opt, max order" );
        }

        previousRank = rank;
        values[field] = compileCustomRuleValue( text, specification->second.domain,
                                                aResult, type );
    }

    if( values.empty() )
    {
        diagnostic( aResult, "error", "missing_custom_constraint_value",
                    "constraint " + type + " requires at least one value" );
    }

    for( const std::string& required : specification->second.required )
    {
        if( !values.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_custom_constraint_value",
                        "constraint " + type + " requires " + required );
        }
    }

    if( type == "skew" && skewDomain.empty() )
    {
        diagnostic( aResult, "error", "invalid_custom_skew_domain",
                    "skew requires explicit (domain nets|diff_pairs)" );
    }

    std::string valueDomain;
    long double minimum = 0.0L;
    bool        haveMinimum = false;

    for( const char* field : { "min", "opt", "max" } )
    {
        if( !values.contains( field ) || !values[field].is_object()
            || !values[field].contains( "domain" ) )
        {
            continue;
        }

        const std::string domain = values[field]["domain"].get<std::string>();

        if( valueDomain.empty() )
            valueDomain = domain;
        else if( domain != valueDomain )
            diagnostic( aResult, "error", "mixed_custom_constraint_domains",
                        "constraint " + type + " cannot mix distance and time values" );

        if( domain == "distance" || domain == "time" || domain == "integer"
            || domain == "angle" )
        {
            const long double current = domain == "angle"
                                                ? values[field]["value"].get<double>()
                                                : values[field]["value"].get<int64_t>();

            if( haveMinimum && current < minimum )
            {
                diagnostic( aResult, "error", "unordered_custom_constraint_range",
                            "constraint " + type + " requires min <= opt <= max" );
            }

            minimum = current;
            haveMinimum = true;
        }
    }

    JSON constraint = { { "type", type }, { "kind", "range" }, { "values", values } };

    if( type == "skew" )
        constraint["domain"] = skewDomain;

    return constraint;
}


JSON compileCustomRules( const DOCUMENT& aDocument, size_t aNode,
                         KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    JSON rules = JSON::array();
    std::set<std::string> names;
    size_t constraintCount = 0;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t ruleNode = node.children[index];
        const DOCUMENT::NODE& ruleForm = aDocument.Nodes()[ruleNode];
        std::string name;

        if( aDocument.ListHead( ruleNode ) != "rule" || ruleForm.children.size() < 6
            || !scalarText( aDocument, ruleForm.children[1], name ) || name.empty()
            || name.size() > 256 )
        {
            diagnostic( aResult, "error", "invalid_custom_rule",
                        "custom_rules entries require a bounded rule name, condition, layer, "
                        "severity, and at least one constraint" );
            continue;
        }

        if( !names.emplace( name ).second )
        {
            diagnostic( aResult, "error", "duplicate_custom_rule",
                        "custom rule names must be unique" );
        }

        std::string condition;
        std::string layer;
        std::string severity;

        if( aDocument.ListHead( ruleForm.children[2] ) != "condition"
            || !parseSingleValueForm( aDocument, ruleForm.children[2], condition )
            || condition.empty() || condition.size() > 8192
            || condition.find( "${" ) != std::string::npos )
        {
            diagnostic( aResult, "error", "invalid_custom_rule_condition",
                        "custom rule condition must be always or one bounded native expression" );
        }

        auto validLayer = []( const std::string& aLayer )
        {
            return !aLayer.empty() && aLayer.size() <= 64
                   && std::all_of( aLayer.begin(), aLayer.end(),
                                   []( unsigned char aCharacter )
                                   {
                                       return std::isalnum( aCharacter ) || aCharacter == '_'
                                              || aCharacter == '-' || aCharacter == '.'
                                              || aCharacter == '*';
                                   } );
        };

        if( aDocument.ListHead( ruleForm.children[3] ) != "layer"
            || !parseSingleValueForm( aDocument, ruleForm.children[3], layer )
            || !validLayer( layer ) )
        {
            diagnostic( aResult, "error", "invalid_custom_rule_layer",
                        "custom rule layer must be all, outer, inner, or a bounded KiCad layer "
                        "pattern" );
        }

        if( aDocument.ListHead( ruleForm.children[4] ) != "severity"
            || !parseSingleValueForm( aDocument, ruleForm.children[4], severity )
            || ( severity != "ignore" && severity != "warning" && severity != "error"
                 && severity != "exclusion" ) )
        {
            diagnostic( aResult, "error", "invalid_custom_rule_severity",
                        "custom rule severity must be ignore, warning, error, or exclusion" );
        }

        JSON constraints = JSON::array();
        std::set<std::string> constraintTypes;

        for( size_t constraintIndex = 5; constraintIndex < ruleForm.children.size();
             ++constraintIndex )
        {
            const size_t constraintNode = ruleForm.children[constraintIndex];

            if( aDocument.ListHead( constraintNode ) != "constraint" )
            {
                diagnostic( aResult, "error", "noncanonical_custom_rule_clause",
                            "custom rule clauses must be condition, layer, severity, then "
                            "constraints" );
                continue;
            }

            JSON constraint = compileCustomConstraint( aDocument, constraintNode, aResult );
            const std::string type = constraint.value( "type", "" );

            if( !type.empty() && !constraintTypes.emplace( type ).second )
            {
                diagnostic( aResult, "error", "duplicate_custom_constraint",
                            "a custom rule may contain each constraint type once" );
            }

            constraints.emplace_back( std::move( constraint ) );
            ++constraintCount;
        }

        if( constraints.size() > 64 )
        {
            diagnostic( aResult, "error", "too_many_custom_constraints",
                        "a custom rule supports at most 64 constraints" );
        }

        rules.push_back( { { "name", name },
                           { "condition", condition },
                           { "layer", layer },
                           { "severity", severity },
                           { "constraints", std::move( constraints ) } } );
    }

    if( rules.size() > 512 || constraintCount > 4096 )
    {
        diagnostic( aResult, "error", "too_many_custom_rules",
                    "custom_rules supports at most 512 rules and 4096 constraints" );
    }

    return { { "kind", "custom_rules" }, { "rules", std::move( rules ) } };
}


JSON compileSheet( const DOCUMENT& aDocument, size_t aNode,
                   KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           id;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], id )
        || !validIdentifier( id ) )
    {
        diagnostic( aResult, "error", "invalid_sheet",
                    "sheet requires a bounded logical identifier" );
        return JSON::object();
    }

    JSON sheet = { { "id", id }, { "parent", nullptr }, { "pins", JSON::array() } };
    std::set<std::string> fields;
    std::set<std::string> pinNames;

    for( size_t i = 2; i < node.children.size(); ++i )
    {
        const size_t child = node.children[i];
        const std::string head = aDocument.ListHead( child );
        const DOCUMENT::NODE& field = aDocument.Nodes()[child];

        if( head == "pin" )
        {
            std::string name;
            std::string direction;

            if( field.children.size() < 5
                || !scalarText( aDocument, field.children[1], name ) || name.empty()
                || name.size() > MAX_IDENTIFIER_BYTES
                || !scalarText( aDocument, field.children[2], direction )
                || !std::set<std::string>( { "input", "output", "bidirectional",
                                             "tri_state", "passive" } )
                            .contains( direction ) )
            {
                diagnostic( aResult, "error", "invalid_sheet_pin",
                            "sheet pin requires a bounded name, native electrical direction, "
                            "position, and side" );
                continue;
            }

            if( !pinNames.emplace( name ).second )
            {
                diagnostic( aResult, "error", "duplicate_sheet_pin",
                            "sheet " + id + " pin " + name + " occurs more than once" );
                continue;
            }

            JSON pin = { { "name", name }, { "direction", direction } };
            std::set<std::string> pinFields;

            for( size_t fieldIndex = 3; fieldIndex < field.children.size(); ++fieldIndex )
            {
                const size_t pinFieldNode = field.children[fieldIndex];
                const std::string pinHead = aDocument.ListHead( pinFieldNode );

                if( !pinFields.emplace( pinHead ).second )
                {
                    diagnostic( aResult, "error", "duplicate_sheet_pin_field",
                                "sheet pin field '" + pinHead + "' occurs more than once" );
                    continue;
                }

                if( pinHead == "at" )
                {
                    const DOCUMENT::NODE& at = aDocument.Nodes()[pinFieldNode];
                    std::string xText;
                    std::string yText;
                    int64_t x = 0;
                    int64_t y = 0;

                    if( at.children.size() != 3
                        || !scalarText( aDocument, at.children[1], xText )
                        || !scalarText( aDocument, at.children[2], yText )
                        || !parseDistance( xText, x ) || !parseDistance( yText, y )
                        || x < 0 || y < 0 || x > 2'000'000'000LL
                        || y > 2'000'000'000LL )
                    {
                        diagnostic( aResult, "error", "invalid_sheet_pin_position",
                                    "sheet pin position must contain two distances from 0 to 2 m" );
                    }
                    else
                    {
                        pin["position"] = { { "xNm", x }, { "yNm", y } };
                    }
                }
                else if( pinHead == "side" )
                {
                    std::string side;

                    if( !parseSingleValueForm( aDocument, pinFieldNode, side )
                        || !std::set<std::string>( { "left", "right", "top", "bottom" } )
                                    .contains( side ) )
                    {
                        diagnostic( aResult, "error", "invalid_sheet_pin_side",
                                    "sheet pin side must be left, right, top, or bottom" );
                    }
                    else
                    {
                        pin["side"] = side;
                    }
                }
                else
                {
                    diagnostic( aResult, "error", "unknown_sheet_pin_field",
                                "sheet pin supports only at and side" );
                }
            }

            if( !pin.contains( "position" ) || !pin.contains( "side" ) )
            {
                diagnostic( aResult, "error", "missing_sheet_pin_field",
                            "sheet pin requires exactly one at and side" );
            }

            sheet["pins"].emplace_back( std::move( pin ) );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_sheet_field",
                        "sheet field '" + head + "' occurs more than once" );
            continue;
        }

        if( head == "title" || head == "file" || head == "parent" )
        {
            std::string value;

            if( !parseSingleValueForm( aDocument, child, value ) || value.empty()
                || value.size() > 4096 )
            {
                diagnostic( aResult, "error", "invalid_sheet_field",
                            "sheet title, file, and parent require one bounded value" );
                continue;
            }

            if( head == "title" && value.size() > 256 )
            {
                diagnostic( aResult, "error", "invalid_sheet_title",
                            "sheet title may contain at most 256 bytes" );
            }
            else if( head == "file" )
            {
                const bool safe = value.ends_with( ".kicad_sch" )
                                  && value.front() != '/' && value.front() != '\\'
                                  && value.find( '\0' ) == std::string::npos
                                  && value.find_first_of( "\r\n\\" ) == std::string::npos
                                  && !value.starts_with( "../" )
                                  && value.find( "/../" ) == std::string::npos
                                  && !value.ends_with( "/.." ) && value.find( ':' ) == std::string::npos;

                if( !safe )
                {
                    diagnostic( aResult, "error", "invalid_sheet_file",
                                "sheet file must be a safe project-relative .kicad_sch path" );
                }
            }
            else if( head == "parent" && value != "none" && !validIdentifier( value ) )
            {
                diagnostic( aResult, "error", "invalid_sheet_parent",
                            "sheet parent must be none or a bounded sheet identifier" );
            }

            sheet[head] = head == "parent" && value == "none" ? JSON( nullptr ) : JSON( value );
        }
        else if( head == "at" || head == "size" )
        {
            std::string xText;
            std::string yText;
            int64_t x = 0;
            int64_t y = 0;

            if( field.children.size() != 3
                || !scalarText( aDocument, field.children[1], xText )
                || !scalarText( aDocument, field.children[2], yText )
                || !parseDistance( xText, x ) || !parseDistance( yText, y )
                || ( head == "at" && ( x < 0 || y < 0 ) )
                || ( head == "size" && ( x <= 0 || y <= 0 ) )
                || x > 2'000'000'000LL || y > 2'000'000'000LL )
            {
                diagnostic( aResult, "error", "invalid_sheet_" + head,
                            "sheet " + head + " requires two bounded physical distances" );
            }
            else
            {
                sheet[head == "at" ? "position" : "size"] = {
                    { "xNm", x }, { "yNm", y }
                };
            }
        }
        else
        {
            diagnostic( aResult, "error", "unknown_sheet_field",
                        "sheet supports title, file, parent, at, size, and pin" );
        }
    }

    for( const char* required : { "title", "file" } )
    {
        if( !sheet.contains( required ) )
        {
            diagnostic( aResult, "error", "missing_sheet_field",
                        "sheet " + id + " is missing " + required );
        }
    }

    if( !fields.contains( "parent" ) )
    {
        diagnostic( aResult, "error", "missing_sheet_field",
                    "sheet " + id + " is missing parent" );
    }

    if( sheet["pins"].size() > 256 )
    {
        diagnostic( aResult, "error", "too_many_sheet_pins",
                    "a sheet may declare at most 256 hierarchical pins" );
    }

    return sheet;
}


JSON compileEnumeratedFacet( const DOCUMENT& aDocument, size_t aNode, const std::string& aFacet,
                             const std::set<std::string>& aAllowed,
                             KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    std::string kind;

    if( !parseSingleValueForm( aDocument, aNode, kind ) || !aAllowed.contains( kind ) )
    {
        diagnostic( aResult, "error", "invalid_" + aFacet,
                    aFacet + " must contain exactly one supported KDS version 1 kind" );
        return JSON::object();
    }

    return { { "kind", kind } };
}


bool containsError( const JSON& aDiagnostics )
{
    return std::any_of( aDiagnostics.begin(), aDiagnostics.end(),
                        []( const JSON& aDiagnostic )
                        {
                            return aDiagnostic.value( "severity", "error" ) == "error";
                        } );
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_COMPILER::JSON DESIGN_SCRIPT_COMPILER::Describe()
{
    JSON description = {
        { "language", "kichad-design" },
        { "version", LANGUAGE_VERSION },
        { "syntax", "s-expression" },
        { "root", "kichad_design" },
        { "deterministic", true },
        { "hostCodeExecution", false },
        { "topLevelForms",
          JSON::array( {
                  { { "form", "(version 1)" }, { "required", true } },
                  { { "form",
                      "(project NAME (title TEXT) (company TEXT) (revision TEXT) (date TEXT) "
                      "(comment 1..9 TEXT) ...)" },
                    { "required", true } },
                  { { "form", "(units mm|mil)" }, { "default", "mm" } },
                  { { "form",
                      "(library symbol|footprint|model ID (table global)) | "
                      "(library symbol|footprint|model ID (table project) "
                      "(uri ${KIPRJMOD}/PATH))" } },
                  { { "form",
                      "(component REF (symbol LIB:ID) (value VALUE) "
                      "(footprint LIB:ID|none) "
                      "(property NAME VALUE) (dnp true|false) "
                      "(unit NUMBER (sheet ID) (at X Y) "
                      "(rotation 0deg|90deg|180deg|270deg) (mirror none|x|y|xy)) ...)" } },
                  { { "form", "(net NAME (pin REF UNIT NUMBER) (pin REF UNIT NUMBER) ...)" } },
                  { { "form", "(no_connect REF UNIT NUMBER)" } },
                  { { "form",
                      "(wire|bus|bus_entry ID (sheet ID) (from X Y) (to X Y) "
                      "(stroke default|WIDTH default|solid|dash|dot|dash_dot|dash_dot_dot))" } },
                  { { "form",
                      "(junction ID (sheet ID) (at X Y) (diameter auto|SIZE) "
                      "(color default|#RRGGBB[AA]))" } },
                  { { "form",
                      "(label ID (sheet ID) (scope local|global) (net NAME) (at X Y) "
                      "(rotation ORTHOGONAL) (shape none|input|output|bidirectional|tri_state|passive) "
                      "(size W H) (thickness auto|SIZE) "
                      "(justify left|center|right top|center|bottom) "
                      "(bold true|false) (italic true|false))" } },
                  { { "form",
                      "(directive ID (sheet ID) (target net NAME) (at X Y) "
                      "(rotation ORTHOGONAL) (shape dot|round|diamond|rectangle) "
                      "(length SIZE) (property NAME VALUE (at X Y) "
                      "(rotation ORTHOGONAL) (size W H) (thickness auto|SIZE) "
                      "(justify left|center|right top|center|bottom) "
                      "(bold true|false) (italic true|false) (visible true|false)) ... )" } },
                  { { "form", "(bus_alias NAME (sheet ID) (members NET ...))" } },
                  { { "form",
                      "(sheet ID (parent none|ID) (file PROJECT_PATH.kicad_sch) "
                      "(title TEXT) [(at X Y) (size W H) "
                      "(pin NAME input|output|bidirectional|tri_state|passive "
                      "(at X Y) (side left|right|top|bottom)) ...])" } },
                  { { "form",
                      "(board (stackup ...) (outline (rect (id ID) (at X Y) (size W H))) "
                      "(place REF (at X Y) ...) (route NET (id ID) (from X Y) (to X Y) ...) "
                      "(via NET (id ID) (at X Y) ...) (zone NET ...) (text ...) (dimension ...) "
                      "(keepout ...))" } },
                  { { "form",
                      "(stackup (finish NAME) (impedance_controlled BOOL) "
                      "(edge_connector none|yes|bevelled) (edge_plating BOOL) "
                      "(layers (silkscreen LAYER ...) (solderpaste LAYER) "
                      "(soldermask LAYER ...) (copper LAYER (thickness D)) "
                      "(dielectric core|prepreg (thickness D) (material NAME) "
                      "(epsilon_r N) (loss_tangent N) (locked BOOL)) ...))" } },
                  { { "form",
                      "(zone NET (id ID) (layers F.Cu ...) "
                      "(outline (polygon (point X Y) ... (hole (point X Y) ...))) "
                      "(clearance D) (min_thickness D) "
                      "(connection none|solid|thermal|pth_thermal "
                      "(thermal_gap D) (thermal_spoke_width D)) "
                      "(islands remove_all|keep_all|remove_below ...) "
                      "(fill solid|hatched ...) "
                      "(hatch_offsets (layer F.Cu X Y) ...))" } },
                  { { "form",
                      "(keepout (id ID) (layers F.Cu ...) "
                      "(outline (polygon (point X Y) ... (hole (point X Y) ...))) "
                      "(prohibit (copper BOOL) (vias BOOL) (tracks BOOL) "
                      "(pads BOOL) (footprints BOOL)))" } },
                  { { "form",
                      "(text VALUE (id ID) (layer LAYER) (at X Y) (size W H) "
                      "(stroke D) (angle A) (justify HORIZONTAL VERTICAL) "
                      "(font stroke|NAME) ...)" } },
                  { { "form",
                      "(dimension aligned|orthogonal|radial|leader|center (id ID) "
                      "(layer LAYER) STYLE_GEOMETRY (line_width D) (arrow_length D) ...)" } },
                  { { "form",
                      "(rules (minimum_clearance D) (minimum_connection_width D) "
                      "(minimum_track_width D) (minimum_via_annular_width D) "
                      "(minimum_via_diameter D) (minimum_through_hole_diameter D) "
                      "(minimum_microvia_diameter D) (minimum_microvia_drill D) "
                      "(minimum_hole_to_hole D) (minimum_copper_to_hole_clearance D) "
                      "(minimum_silkscreen_clearance D) (minimum_groove_width D) "
                      "(minimum_resolved_spokes N) (minimum_silkscreen_text_height D) "
                      "(minimum_silkscreen_text_thickness D) "
                      "(minimum_copper_to_edge_clearance D|legacy) "
                      "(use_height_for_length_calculations BOOL) (maximum_error D) "
                      "(allow_fillets_outside_zone_outline BOOL))" } },
                  { { "form",
                      "(net_classes (class Default COMPLETE_FIELDS) "
                      "(class NAME VALUE_OR_INHERIT_FIELDS) ... "
                      "(assign (pattern PATTERN) (classes NAME ...)) ...)" } },
                  { { "form",
                      "(custom_rules (rule NAME (condition always|EXPRESSION) "
                      "(layer all|outer|inner|LAYER) "
                      "(severity ignore|warning|error|exclusion) "
                      "(constraint TYPE TYPED_VALUES) ...))" } },
                  { { "form",
                      "(source REF (manufacturer NAME) (mpn PART) (datasheet HTTPS_URL) "
                      "(lifecycle active|nrnd|last_time_buy|obsolete) (supplier NAME) "
                      "(sku PART) (product_url HTTPS_URL) (available N) "
                      "(verified_on YYYY-MM-DD) (quantity N) [(unit_price TEXT) (notes TEXT)])" } },
                  { { "form", "(check erc|drc|sourcing|footprints|fabrication)" } },
                  { { "form", "(output gerbers|drill|ipcd356|ipc2581|odbpp|pick_place|bom|step|pdf)" } }
          } ) },
        { "compilerPasses",
          JSON::array( { "parse", "typecheck", "resolve", "plan", "snapshot", "schematic",
                         "libraries", "pcb", "sourcing", "erc", "drc", "fabrication" } ) },
        { "example",
          "(kichad_design\n"
          "  (version 1)\n"
          "  (project sensor (title \"Sensor Board\"))\n"
          "  (units mm)\n"
          "  (library symbol Device (table project) "
          "(uri \"${KIPRJMOD}/Device.kicad_sym\"))\n"
          "  (library footprint Resistor_SMD (table project) "
          "(uri \"${KIPRJMOD}/Resistor_SMD.pretty\"))\n"
          "  (library footprint LED_SMD (table project) "
          "(uri \"${KIPRJMOD}/LED_SMD.pretty\"))\n"
          "  (sheet root (parent none) (file \"sensor.kicad_sch\") (title \"Main\"))\n"
          "  (component R1 (symbol \"Device:R\") (value \"10k\")\n"
          "    (footprint \"Resistor_SMD:R_0603_1608Metric\")\n"
          "    (unit 1 (sheet root) (at 40mm 40mm) (rotation 0deg) (mirror none)))\n"
          "  (component LED1 (symbol \"Device:LED\") (value \"GREEN\")\n"
          "    (footprint \"LED_SMD:LED_0603_1608Metric\")\n"
          "    (unit 1 (sheet root) (at 50mm 40mm) (rotation 0deg) (mirror none)))\n"
          "  (net LED_A (pin R1 1 1) (pin LED1 1 1))\n"
          "  (check erc)\n"
          "  (check drc)\n"
          "  (output gerbers))" }
    };
    description["capabilityCoverage"] = DesignScriptCapabilities();
    return description;
}


DESIGN_SCRIPT_COMPILER::RESULT DESIGN_SCRIPT_COMPILER::Compile( const std::string& aSource )
{
    RESULT result;

    if( aSource.empty() || aSource.size() > MAX_SCRIPT_BYTES )
    {
        diagnostic( result, "error", "invalid_source_size",
                    "KiChad Design Script source must contain 1 byte to 1 MiB" );
        return result;
    }

    picosha2::hash256_hex_string( aSource, result.sourceSha256 );

    if( aSource.find( '\0' ) != std::string::npos )
    {
        diagnostic( result, "error", "invalid_encoding",
                    "KiChad Design Script source must not contain embedded NUL bytes" );
        return result;
    }

    const wxString decoded = wxString::FromUTF8( aSource.data(), aSource.size() );
    const wxScopedCharBuffer reencoded = decoded.ToUTF8();

    if( reencoded.length() != aSource.size()
        || std::memcmp( reencoded.data(), aSource.data(), aSource.size() ) != 0 )
    {
        diagnostic( result, "error", "invalid_encoding",
                    "KiChad Design Script source must be valid UTF-8" );
        return result;
    }

    std::string parseError;
    std::unique_ptr<DOCUMENT> document = DOCUMENT::Parse( aSource, &parseError );

    if( !document )
    {
        diagnostic( result, "error", "parse_failed", parseError );
        return result;
    }

    if( document->Roots().size() != 1
        || document->ListHead( document->Roots().front() ) != "kichad_design" )
    {
        diagnostic( result, "error", "invalid_root",
                    "script must contain exactly one kichad_design root expression" );
        return result;
    }

    const DOCUMENT::NODE& root = document->Nodes()[document->Roots().front()];

    if( root.children.size() > MAX_TOP_LEVEL_FORMS + 1 )
    {
        diagnostic( result, "error", "program_too_large",
                    "script contains more than 20000 top-level forms" );
        return result;
    }

    result.ir = {
        { "language", "kichad-design" },
        { "version", LANGUAGE_VERSION },
        { "sourceSha256", result.sourceSha256 },
        { "units", "mm" },
        { "project", JSON::object() },
        { "libraries", JSON::array() },
        { "schematic",
          { { "sheets", JSON::array() }, { "components", JSON::array() },
            { "nets", JSON::array() }, { "noConnects", JSON::array() },
            { "drawings", JSON::array() }, { "busAliases", JSON::array() } } },
        { "pcb", JSON::array() },
        { "rules", nullptr },
        { "netClasses", nullptr },
        { "customRules", nullptr },
        { "sourcing", JSON::array() },
        { "checks", JSON::array() },
        { "outputs", JSON::array() }
    };

    bool                     sawVersion = false;
    bool                     sawProject = false;
    bool                     sawUnits = false;
    bool                     sawBoard = false;
    bool                     boardFullyTyped = true;
    std::set<std::string>    componentIds;
    std::set<std::string>    netNames;
    std::set<std::string>    libraryIds;
    std::set<std::string>    sheetIds;
    bool                     sawRules = false;
    bool                     sawNetClasses = false;
    bool                     sawCustomRules = false;
    std::set<std::string>    sourceIds;
    std::set<std::string>    checkKinds;
    std::set<std::string>    outputKinds;
    std::set<std::string>    connectedPins;
    std::set<std::string>    schematicDrawingIds;
    std::set<std::string>    schematicBusAliasIds;
    std::vector<std::string> referencedComponents;
    std::vector<std::string> referencedNets;
    size_t                   pinConnections = 0;

    for( size_t i = 1; i < root.children.size(); ++i )
    {
        const size_t      formNode = root.children[i];
        const std::string form = document->ListHead( formNode );

        if( form.empty() )
        {
            diagnostic( result, "error", "invalid_top_level_form",
                        "every top-level design form must be a named list" );
            continue;
        }

        if( form == "version" )
        {
            std::string version;

            if( sawVersion || !parseSingleValueForm( *document, formNode, version )
                || version != std::to_string( LANGUAGE_VERSION ) )
            {
                diagnostic( result, "error", "invalid_version",
                            "script requires exactly one (version 1) form" );
            }

            sawVersion = true;
        }
        else if( form == "project" )
        {
            if( sawProject )
                diagnostic( result, "error", "duplicate_project", "project occurs more than once" );

            result.ir["project"] = compileProject( *document, formNode, result );
            sawProject = true;
        }
        else if( form == "units" )
        {
            std::string units;

            if( sawUnits || !parseSingleValueForm( *document, formNode, units )
                || ( units != "mm" && units != "mil" ) )
            {
                diagnostic( result, "error", "invalid_units",
                            "units may occur once and must be mm or mil" );
            }
            else
            {
                result.ir["units"] = units;
            }

            sawUnits = true;
        }
        else if( form == "library" )
        {
            JSON library = compileLibrary( *document, formNode, result );
            const std::string key = library.value( "kind", "" ) + ":"
                                    + library.value( "id", "" );

            if( key != ":" && !libraryIds.emplace( key ).second )
            {
                diagnostic( result, "error", "duplicate_library",
                            "library " + key + " occurs more than once" );
            }

            result.ir["libraries"].emplace_back( std::move( library ) );
        }
        else if( form == "component" )
        {
            JSON component = compileComponent( *document, formNode, result );
            const std::string reference = component.value( "reference", "" );

            if( !reference.empty() && !componentIds.emplace( reference ).second )
            {
                diagnostic( result, "error", "duplicate_component",
                            "component " + reference + " occurs more than once" );
            }

            result.ir["schematic"]["components"].emplace_back( std::move( component ) );
        }
        else if( form == "net" )
        {
            JSON net = compileNet( *document, formNode, result, referencedComponents,
                                   connectedPins, pinConnections );
            const std::string name = net.value( "name", "" );

            if( !name.empty() && !netNames.emplace( name ).second )
            {
                diagnostic( result, "error", "duplicate_net",
                            "net " + name + " occurs more than once" );
            }

            result.ir["schematic"]["nets"].emplace_back( std::move( net ) );
        }
        else if( form == "no_connect" )
        {
            result.ir["schematic"]["noConnects"].emplace_back(
                    compileNoConnect( *document, formNode, result, referencedComponents,
                                      connectedPins, pinConnections ) );
        }
        else if( form == "wire" || form == "bus" || form == "bus_entry" )
        {
            JSON drawing = compileSchematicLine( *document, formNode, form, result );
            const std::string id = drawing.value( "id", "" );

            if( !id.empty() && !schematicDrawingIds.emplace( id ).second )
            {
                diagnostic( result, "error", "duplicate_schematic_drawing_id",
                            "schematic drawing ID " + id + " occurs more than once" );
            }

            result.ir["schematic"]["drawings"].emplace_back( std::move( drawing ) );
        }
        else if( form == "junction" )
        {
            JSON drawing = compileSchematicJunction( *document, formNode, result );
            const std::string id = drawing.value( "id", "" );

            if( !id.empty() && !schematicDrawingIds.emplace( id ).second )
            {
                diagnostic( result, "error", "duplicate_schematic_drawing_id",
                            "schematic drawing ID " + id + " occurs more than once" );
            }

            result.ir["schematic"]["drawings"].emplace_back( std::move( drawing ) );
        }
        else if( form == "label" )
        {
            JSON drawing = compileSchematicLabel( *document, formNode, result );
            const std::string id = drawing.value( "id", "" );

            if( !id.empty() && !schematicDrawingIds.emplace( id ).second )
            {
                diagnostic( result, "error", "duplicate_schematic_drawing_id",
                            "schematic drawing ID " + id + " occurs more than once" );
            }

            result.ir["schematic"]["drawings"].emplace_back( std::move( drawing ) );
        }
        else if( form == "directive" )
        {
            JSON drawing = compileSchematicDirective( *document, formNode, result );
            const std::string id = drawing.value( "id", "" );

            if( !id.empty() && !schematicDrawingIds.emplace( id ).second )
            {
                diagnostic( result, "error", "duplicate_schematic_drawing_id",
                            "schematic drawing ID " + id + " occurs more than once" );
            }

            result.ir["schematic"]["drawings"].emplace_back( std::move( drawing ) );
        }
        else if( form == "bus_alias" )
        {
            JSON alias = compileSchematicBusAlias( *document, formNode, result );
            const std::string key = alias.value( "sheet", "" ) + "\n"
                                    + alias.value( "name", "" );

            if( key != "\n" && !schematicBusAliasIds.emplace( key ).second )
            {
                diagnostic( result, "error", "duplicate_schematic_bus_alias",
                            "bus_alias " + alias.value( "name", "" )
                                    + " occurs more than once on its sheet" );
            }

            result.ir["schematic"]["busAliases"].emplace_back( std::move( alias ) );
        }
        else if( form == "sheet" )
        {
            JSON sheet = compileSheet( *document, formNode, result );
            const std::string name = sheet.value( "id", "" );

            if( !name.empty() && !sheetIds.emplace( name ).second )
            {
                diagnostic( result, "error", "duplicate_sheet",
                            "sheet " + name + " occurs more than once" );
            }

            result.ir["schematic"]["sheets"].emplace_back( std::move( sheet ) );
        }
        else if( form == "board" )
        {
            if( sawBoard )
                diagnostic( result, "error", "duplicate_board", "board occurs more than once" );

            KICHAD::DESIGN_SCRIPT_BOARD_COMPILER::RESULT board =
                    KICHAD::DESIGN_SCRIPT_BOARD_COMPILER::Compile( *document, formNode );

            for( JSON& statement : board.statements )
                result.ir["pcb"].emplace_back( std::move( statement ) );

            for( JSON& boardDiagnostic : board.diagnostics )
                result.diagnostics.emplace_back( std::move( boardDiagnostic ) );

            referencedComponents.insert( referencedComponents.end(),
                                         board.componentReferences.begin(),
                                         board.componentReferences.end() );
            referencedNets.insert( referencedNets.end(), board.netReferences.begin(),
                                   board.netReferences.end() );
            boardFullyTyped = boardFullyTyped && board.fullyTyped;

            sawBoard = true;
        }
        else if( form == "rules" )
        {
            if( sawRules )
                diagnostic( result, "error", "duplicate_rules", "rules occurs more than once" );
            else
                result.ir["rules"] = compileRules( *document, formNode, result );

            sawRules = true;
        }
        else if( form == "net_classes" )
        {
            if( sawNetClasses )
            {
                diagnostic( result, "error", "duplicate_net_classes",
                            "net_classes occurs more than once" );
            }
            else
            {
                result.ir["netClasses"] = compileNetClasses( *document, formNode, result );
            }

            sawNetClasses = true;
        }
        else if( form == "custom_rules" )
        {
            if( sawCustomRules )
            {
                diagnostic( result, "error", "duplicate_custom_rules",
                            "custom_rules occurs more than once" );
            }
            else
            {
                result.ir["customRules"] = compileCustomRules( *document, formNode, result );
            }

            sawCustomRules = true;
        }
        else if( form == "source" )
        {
            JSON source = compileSource( *document, formNode, result, referencedComponents );
            const std::string reference = source.value( "component", "" );

            if( !reference.empty() && !sourceIds.emplace( reference ).second )
            {
                diagnostic( result, "error", "duplicate_source",
                            "source for " + reference + " occurs more than once" );
            }

            result.ir["sourcing"].emplace_back( std::move( source ) );
        }
        else if( form == "check" )
        {
            static const std::set<std::string> allowed = {
                "erc", "drc", "sourcing", "footprints", "fabrication"
            };
            JSON check = compileEnumeratedFacet( *document, formNode, "check", allowed, result );
            const std::string kind = check.value( "kind", "" );

            if( !kind.empty() && !checkKinds.emplace( kind ).second )
            {
                diagnostic( result, "error", "duplicate_check",
                            "check " + kind + " occurs more than once" );
            }

            result.ir["checks"].emplace_back( std::move( check ) );
        }
        else if( form == "output" )
        {
            static const std::set<std::string> allowed = {
                "gerbers", "drill", "ipcd356", "ipc2581", "odbpp", "pick_place", "bom",
                "step", "pdf"
            };
            JSON output = compileEnumeratedFacet( *document, formNode, "output", allowed, result );
            const std::string kind = output.value( "kind", "" );

            if( !kind.empty() && !outputKinds.emplace( kind ).second )
            {
                diagnostic( result, "error", "duplicate_output",
                            "output " + kind + " occurs more than once" );
            }

            result.ir["outputs"].emplace_back( std::move( output ) );
        }
        else
        {
            diagnostic( result, "error", "unknown_top_level_form",
                        "unknown top-level form '" + form + "'" );
        }
    }

    if( !sawVersion )
        diagnostic( result, "error", "missing_version", "script is missing (version 1)" );

    if( !sawProject )
        diagnostic( result, "error", "missing_project", "script is missing project metadata" );

    const JSON& sheets = result.ir["schematic"]["sheets"];

    if( sheets.size() > 128 )
    {
        diagnostic( result, "error", "too_many_sheets",
                    "a script may declare at most 128 schematic sheets" );
    }

    std::map<std::string, const JSON*> sheetsById;
    std::map<std::string, std::string> sheetParents;
    std::map<std::string, std::string> siblingTitles;
    std::map<std::string, std::string> caseFoldedFiles;
    std::vector<std::string> rootSheets;
    size_t sheetPinCount = 0;

    for( const JSON& sheet : sheets )
    {
        if( !sheet.is_object() )
            continue;

        const std::string id = sheet.value( "id", "" );

        if( id.empty() )
            continue;

        sheetsById[id] = &sheet;
        sheetPinCount += sheet.value( "pins", JSON::array() ).size();

        if( sheet["parent"].is_null() )
        {
            rootSheets.emplace_back( id );

            if( sheet.contains( "position" ) || sheet.contains( "size" )
                || !sheet.value( "pins", JSON::array() ).empty() )
            {
                diagnostic( result, "error", "invalid_root_sheet_geometry",
                            "the root sheet cannot have a parent symbol position, size, or pins" );
            }
        }
        else if( sheet["parent"].is_string() )
        {
            const std::string parent = sheet["parent"].get<std::string>();
            sheetParents[id] = parent;

            if( parent == id )
            {
                diagnostic( result, "error", "recursive_sheet_hierarchy",
                            "sheet " + id + " cannot parent itself" );
            }

            if( !sheet.contains( "position" ) || !sheet.contains( "size" ) )
            {
                diagnostic( result, "error", "missing_sheet_geometry",
                            "non-root sheet " + id + " requires at and size" );
            }
            else
            {
                const int64_t x = sheet["position"]["xNm"].get<int64_t>();
                const int64_t y = sheet["position"]["yNm"].get<int64_t>();
                const int64_t right = x + sheet["size"]["xNm"].get<int64_t>();
                const int64_t bottom = y + sheet["size"]["yNm"].get<int64_t>();

                for( const JSON& pin : sheet["pins"] )
                {
                    if( !pin.contains( "position" ) || !pin.contains( "side" ) )
                        continue;

                    const int64_t pinX = pin["position"]["xNm"].get<int64_t>();
                    const int64_t pinY = pin["position"]["yNm"].get<int64_t>();
                    const std::string side = pin["side"].get<std::string>();
                    const bool onBoundary = side == "left" ? pinX == x && pinY >= y && pinY <= bottom
                                          : side == "right" ? pinX == right && pinY >= y && pinY <= bottom
                                          : side == "top" ? pinY == y && pinX >= x && pinX <= right
                                                            : pinY == bottom && pinX >= x && pinX <= right;

                    if( !onBoundary )
                    {
                        diagnostic( result, "error", "sheet_pin_off_edge",
                                    "sheet " + id + " pin " + pin.value( "name", "" )
                                            + " must lie on its declared sheet side" );
                    }
                }
            }

            const std::string siblingKey = parent + "\n" + sheet.value( "title", "" );

            if( !siblingTitles.emplace( siblingKey, id ).second )
            {
                diagnostic( result, "error", "duplicate_sibling_sheet_title",
                            "sibling sheets under " + parent + " cannot share title "
                                    + sheet.value( "title", "" ) );
            }
        }

        if( sheet.contains( "file" ) && sheet["file"].is_string() )
        {
            const std::string file = sheet["file"].get<std::string>();
            std::string folded = file;
            std::transform( folded.begin(), folded.end(), folded.begin(),
                            []( unsigned char aCharacter )
                            {
                                return static_cast<char>( std::tolower( aCharacter ) );
                            } );
            auto [entry, inserted] = caseFoldedFiles.emplace( folded, file );

            if( !inserted && entry->second != file )
            {
                diagnostic( result, "error", "ambiguous_sheet_file_case",
                            "sheet files cannot differ only by letter case" );
            }
        }
    }

    if( sheetPinCount > 4096 )
    {
        diagnostic( result, "error", "too_many_sheet_pins",
                    "a script may declare at most 4096 hierarchical sheet pins" );
    }

    if( !sheets.empty() && rootSheets.size() != 1 )
    {
        diagnostic( result, "error", "invalid_root_sheet_count",
                    "a schematic hierarchy requires exactly one sheet with parent none" );
    }

    if( rootSheets.size() == 1 && result.ir["project"].is_object() )
    {
        const JSON* rootSheet = sheetsById[rootSheets.front()];
        const std::string expected = result.ir["project"].value( "name", "" ) + ".kicad_sch";

        if( rootSheet && rootSheet->value( "file", "" ) != expected )
        {
            diagnostic( result, "error", "invalid_root_sheet_file",
                        "root sheet file must be " + expected );
        }
    }

    for( const auto& [ id, parent ] : sheetParents )
    {
        if( !sheetsById.contains( parent ) )
        {
            diagnostic( result, "error", "unresolved_sheet_parent",
                        "sheet " + id + " references undeclared parent " + parent );
        }
    }

    std::map<std::string, int> sheetVisitState;
    std::function<void( const std::string& )> visitSheet = [&]( const std::string& aId )
    {
        if( sheetVisitState[aId] == 2 )
            return;

        if( sheetVisitState[aId] == 1 )
        {
            diagnostic( result, "error", "recursive_sheet_hierarchy",
                        "schematic sheet hierarchy contains a parent cycle at " + aId );
            return;
        }

        sheetVisitState[aId] = 1;
        auto parent = sheetParents.find( aId );

        if( parent != sheetParents.end() && sheetsById.contains( parent->second ) )
            visitSheet( parent->second );

        sheetVisitState[aId] = 2;
    };

    for( const auto& entry : sheetsById )
        visitSheet( entry.first );

    for( const auto& entry : sheetParents )
    {
        const std::string& id = entry.first;
        auto child = sheetsById.find( id );
        std::set<std::string> ancestorIds;
        std::set<std::string> ancestorFiles;
        std::string current = id;

        while( child != sheetsById.end() )
        {
            if( !ancestorIds.emplace( current ).second )
                break;

            const std::string file = child->second->value( "file", "" );

            if( !file.empty() && !ancestorFiles.emplace( file ).second )
            {
                diagnostic( result, "error", "recursive_sheet_file",
                            "sheet " + id + " recursively reuses file " + file );
                break;
            }

            auto currentParent = sheetParents.find( current );

            if( currentParent == sheetParents.end()
                || !sheetsById.contains( currentParent->second ) )
            {
                break;
            }

            current = currentParent->second;
            child = sheetsById.find( current );
        }
    }

    std::map<std::string, std::set<int64_t>> componentUnits;
    std::map<std::string, bool>              componentHasFootprint;

    if( sheets.empty() && !result.ir["schematic"]["noConnects"].empty() )
    {
        diagnostic( result, "error", "no_connect_without_hierarchy",
                    "no_connect requires an executable schematic hierarchy" );
    }

    for( const JSON& component : result.ir["schematic"]["components"] )
    {
        if( !component.is_object() || !component.contains( "reference" )
            || !component["reference"].is_string() || !component.contains( "units" )
            || !component["units"].is_array() )
        {
            continue;
        }

        const std::string reference = component["reference"].get<std::string>();
        std::set<int64_t>& units = componentUnits[reference];
        componentHasFootprint[reference] = component.contains( "footprint" )
                                           && component["footprint"].is_string();

        if( !sheets.empty() && component["units"].empty() )
        {
            diagnostic( result, "error", "missing_component_units",
                        "component " + reference
                                + " requires at least one explicit schematic unit placement" );
        }
        else if( sheets.empty() && !component["units"].empty() )
        {
            diagnostic( result, "error", "component_unit_without_hierarchy",
                        "component " + reference
                                + " cannot place schematic units without a declared root sheet" );
        }

        for( const JSON& unit : component["units"] )
        {
            if( !unit.is_object() || !unit.contains( "number" )
                || !unit["number"].is_number_integer() || !unit.contains( "sheet" )
                || !unit["sheet"].is_string() )
            {
                continue;
            }

            const int64_t number = unit["number"].get<int64_t>();
            const std::string sheet = unit["sheet"].get<std::string>();
            units.emplace( number );

            if( !sheetsById.contains( sheet ) )
            {
                diagnostic( result, "error", "unresolved_component_sheet",
                            "component " + reference + " unit " + std::to_string( number )
                                    + " references undeclared sheet " + sheet );
            }
        }
    }

    const auto validateEndpointUnit = [&]( const JSON& aEndpoint )
    {
        if( sheets.empty() || !aEndpoint.is_object() || !aEndpoint.contains( "component" )
            || !aEndpoint["component"].is_string() || !aEndpoint.contains( "unit" )
            || !aEndpoint["unit"].is_number_integer() )
        {
            return;
        }

        const std::string reference = aEndpoint["component"].get<std::string>();
        const int64_t unit = aEndpoint["unit"].get<int64_t>();
        auto component = componentUnits.find( reference );

        if( component != componentUnits.end() && !component->second.contains( unit ) )
        {
            diagnostic( result, "error", "unresolved_component_unit",
                        "schematic endpoint " + reference + ":" + std::to_string( unit )
                                + " has no matching unit placement" );
        }
    };

    for( const JSON& net : result.ir["schematic"]["nets"] )
    {
        if( net.is_object() && net.contains( "pins" ) && net["pins"].is_array() )
        {
            for( const JSON& pin : net["pins"] )
                validateEndpointUnit( pin );
        }
    }

    for( const JSON& noConnect : result.ir["schematic"]["noConnects"] )
        validateEndpointUnit( noConnect );

    for( const JSON& statement : result.ir["pcb"] )
    {
        if( !statement.is_object() || statement.value( "kind", "" ) != "place"
            || !statement.contains( "component" ) || !statement["component"].is_string() )
        {
            continue;
        }

        const std::string reference = statement["component"].get<std::string>();
        auto component = componentHasFootprint.find( reference );

        if( component != componentHasFootprint.end() && !component->second )
        {
            diagnostic( result, "error", "component_without_footprint_placement",
                        "board placement references virtual component " + reference
                                + " whose canonical footprint is none" );
        }
    }

    for( const JSON& drawing : result.ir["schematic"]["drawings"] )
    {
        if( !drawing.is_object() || !drawing.contains( "sheet" )
            || !drawing["sheet"].is_string() )
        {
            continue;
        }

        const std::string sheet = drawing["sheet"].get<std::string>();

        if( sheets.empty() )
        {
            diagnostic( result, "error", "schematic_drawing_without_hierarchy",
                        "schematic drawing " + drawing.value( "id", "" )
                                + " requires a declared root sheet" );
        }
        else if( !sheetsById.contains( sheet ) )
        {
            diagnostic( result, "error", "unresolved_schematic_drawing_sheet",
                        "schematic drawing " + drawing.value( "id", "" )
                                + " references undeclared sheet " + sheet );
        }

        if( drawing.value( "kind", "" ) == "label"
            && drawing.contains( "net" ) && drawing["net"].is_string()
            && !netNames.contains( drawing["net"].get<std::string>() ) )
        {
            diagnostic( result, "error", "unresolved_schematic_label_net",
                        "schematic label " + drawing.value( "id", "" )
                                + " references undeclared net "
                                + drawing["net"].get<std::string>() );
        }

        if( drawing.value( "kind", "" ) == "directive"
            && drawing.contains( "target" ) && drawing["target"].is_object()
            && drawing["target"].value( "kind", "" ) == "net"
            && drawing["target"].contains( "name" )
            && drawing["target"]["name"].is_string()
            && !netNames.contains( drawing["target"]["name"].get<std::string>() ) )
        {
            diagnostic( result, "error", "unresolved_schematic_directive_net",
                        "schematic directive " + drawing.value( "id", "" )
                                + " references undeclared net "
                                + drawing["target"]["name"].get<std::string>() );
        }
    }

    for( const JSON& alias : result.ir["schematic"]["busAliases"] )
    {
        if( !alias.is_object() || !alias.contains( "sheet" ) || !alias["sheet"].is_string()
            || !alias.contains( "members" ) || !alias["members"].is_array() )
        {
            continue;
        }

        const std::string sheet = alias["sheet"].get<std::string>();

        if( !sheetsById.contains( sheet ) )
        {
            diagnostic( result, "error", "unresolved_schematic_bus_alias_sheet",
                        "bus_alias " + alias.value( "name", "" )
                                + " references undeclared sheet " + sheet );
        }

        for( const JSON& member : alias["members"] )
        {
            if( member.is_string() && !netNames.contains( member.get<std::string>() ) )
            {
                diagnostic( result, "error", "unresolved_schematic_bus_alias_member",
                            "bus_alias " + alias.value( "name", "" )
                                    + " references undeclared net "
                                    + member.get<std::string>() );
            }
        }
    }

    if( result.ir["libraries"].size() > MAX_LIBRARIES )
    {
        diagnostic( result, "error", "too_many_libraries",
                    "a script may declare at most 512 library dependencies" );
    }

    size_t projectSymbolLibraries = 0;
    size_t projectFootprintLibraries = 0;

    for( const JSON& library : result.ir["libraries"] )
    {
        if( !library.is_object() || library.value( "table", "" ) != "project" )
            continue;

        if( library.value( "kind", "" ) == "symbol" )
            ++projectSymbolLibraries;
        else if( library.value( "kind", "" ) == "footprint" )
            ++projectFootprintLibraries;
    }

    if( projectSymbolLibraries > MAX_PROJECT_LIBRARIES_PER_TABLE
        || projectFootprintLibraries > MAX_PROJECT_LIBRARIES_PER_TABLE )
    {
        diagnostic( result, "error", "too_many_project_libraries",
                    "a project symbol or footprint table may contain at most 256 libraries" );
    }

    if( !result.ir["libraries"].empty() )
    {
        for( const JSON& component : result.ir["schematic"]["components"] )
        {
            for( const char* kind : { "symbol", "footprint" } )
            {
                if( !component.is_object() || !component.contains( kind )
                    || !component[kind].is_string() )
                {
                    continue;
                }

                std::string nickname;

                if( !libraryIdNickname( component[kind].get<std::string>(), nickname ) )
                    continue;

                if( !libraryIds.contains( std::string( kind ) + ":" + nickname ) )
                {
                    diagnostic( result, "error", "unresolved_component_library",
                                "component " + component.value( "reference", "" ) + " uses "
                                        + kind + " library " + nickname
                                        + " without a matching declaration" );
                }
            }
        }
    }

    for( const std::string& reference : referencedComponents )
    {
        if( !componentIds.contains( reference ) )
        {
            diagnostic( result, "error", "unresolved_component",
                        "component reference " + reference + " is not declared" );
        }
    }

    for( const std::string& name : referencedNets )
    {
        if( !netNames.contains( name ) )
        {
            diagnostic( result, "error", "unresolved_net",
                        "net reference " + name + " is not declared" );
        }
    }

    JSON passes = JSON::array( { "parse", "typecheck", "resolve", "plan", "snapshot" } );

    if( !result.ir["libraries"].empty() )
        passes.emplace_back( "libraries" );

    if( !result.ir["schematic"]["components"].empty()
        || !result.ir["schematic"]["nets"].empty()
        || !result.ir["schematic"]["noConnects"].empty()
        || !result.ir["schematic"]["drawings"].empty()
        || !result.ir["schematic"]["busAliases"].empty() )
        passes.emplace_back( "schematic" );

    if( !result.ir["pcb"].empty() )
        passes.emplace_back( "pcb" );

    if( result.ir["customRules"].is_object() )
        passes.emplace_back( "custom_rules" );

    if( !result.ir["sourcing"].empty() )
        passes.emplace_back( "sourcing" );

    if( !result.ir["checks"].empty() )
        passes.emplace_back( "verification" );

    if( !result.ir["outputs"].empty() )
        passes.emplace_back( "fabrication" );

    result.plan = {
        { "passes", std::move( passes ) },
        { "mutationRequired", true },
        { "transactional", true },
        { "boardFullyTyped", boardFullyTyped },
        { "counts",
          { { "libraries", result.ir["libraries"].size() },
            { "sheets", result.ir["schematic"]["sheets"].size() },
            { "components", result.ir["schematic"]["components"].size() },
            { "nets", result.ir["schematic"]["nets"].size() },
            { "noConnects", result.ir["schematic"]["noConnects"].size() },
            { "drawings", result.ir["schematic"]["drawings"].size() },
            { "busAliases", result.ir["schematic"]["busAliases"].size() },
            { "pinConnections", pinConnections },
            { "boardStatements", result.ir["pcb"].size() },
            { "rules", result.ir["rules"].is_object() ? 1 : 0 },
            { "netClasses", result.ir["netClasses"].is_object()
                                      ? result.ir["netClasses"]["classes"].size()
                                      : 0 },
            { "netClassAssignments", result.ir["netClasses"].is_object()
                                               ? result.ir["netClasses"]["assignments"].size()
                                               : 0 },
            { "customRules", result.ir["customRules"].is_object()
                                     ? result.ir["customRules"]["rules"].size()
                                     : 0 },
            { "sourcingRecords", result.ir["sourcing"].size() },
            { "checks", result.ir["checks"].size() },
            { "outputs", result.ir["outputs"].size() } } }
    };

    result.ok = !containsError( result.diagnostics );
    return result;
}

} // namespace KICHAD
