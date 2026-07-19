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

#include "design_script_board_compiler.h"
#include "lossless_sexpr_document.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
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

    JSON project = { { "name", name }, { "properties", JSON::object() } };
    const std::set<std::string> allowed = { "title", "company", "revision", "date", "comment" };
    std::set<std::string>       fields;

    for( size_t i = 2; i < node.children.size(); ++i )
    {
        const size_t      child = node.children[i];
        const std::string head = aDocument.ListHead( child );
        std::string       value;

        if( !allowed.contains( head ) || !parseSingleValueForm( aDocument, child, value ) )
        {
            diagnostic( aResult, "error", "invalid_project_field",
                        "project fields must be title, company, revision, date, or comment with "
                        "one scalar value" );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_project_field",
                        "project field '" + head + "' occurs more than once" );
            continue;
        }

        project["properties"][head] = value;
    }

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

    JSON library = { { "kind", kind }, { "id", id } };
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

    JSON component = { { "reference", reference }, { "properties", JSON::object() } };
    std::set<std::string> singletonFields;
    std::set<std::string> propertyNames;

    for( size_t i = 2; i < node.children.size(); ++i )
    {
        const size_t      child = node.children[i];
        const std::string head = aDocument.ListHead( child );
        const DOCUMENT::NODE& field = aDocument.Nodes()[child];

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
                        "component supports symbol, value, footprint, property, and dnp" );
            continue;
        }

        std::string value;

        if( !parseSingleValueForm( aDocument, child, value ) || value.empty() )
        {
            diagnostic( aResult, "error", "invalid_component_field",
                        "component symbol, value, and footprint require one non-empty value" );
            continue;
        }

        if( !singletonFields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_component_field",
                        "component field '" + head + "' occurs more than once" );
            continue;
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
        std::string number;

        if( aDocument.ListHead( child ) != "pin" || pin.children.size() != 3
            || !scalarText( aDocument, pin.children[1], reference )
            || !scalarText( aDocument, pin.children[2], number )
            || !validIdentifier( reference ) || number.empty() || number.size() > 64 )
        {
            diagnostic( aResult, "error", "invalid_pin",
                        "net endpoints must use (pin COMPONENT PIN_NUMBER)" );
            continue;
        }

        net["pins"].push_back( { { "component", reference }, { "number", number } } );
        aReferencedComponents.emplace_back( reference );

        const std::string endpoint = reference + ":" + number;

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
    const std::set<std::string> allowed = { "manufacturer", "mpn", "supplier", "sku",
                                             "lifecycle", "quantity", "unit_price" };
    std::set<std::string> fields;

    for( size_t i = 2; i < node.children.size(); ++i )
    {
        const size_t      child = node.children[i];
        const std::string head = aDocument.ListHead( child );
        const DOCUMENT::NODE& field = aDocument.Nodes()[child];

        if( !allowed.contains( head ) || field.children.size() != 2
            || !isScalar( aDocument, field.children[1] ) )
        {
            diagnostic( aResult, "error", "invalid_source_field",
                        "source fields must be manufacturer, mpn, supplier, sku, lifecycle, "
                        "quantity, or unit_price with one value" );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "error", "duplicate_source_field",
                        "source field '" + head + "' occurs more than once" );
            continue;
        }

        source[head] = scalarValue( aDocument, field.children[1] );
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


JSON compileNamedFacet( const DOCUMENT& aDocument, size_t aNode, const std::string& aFacet,
                        KICHAD::DESIGN_SCRIPT_COMPILER::RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           name;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], name )
        || !validIdentifier( name ) )
    {
        diagnostic( aResult, "error", "invalid_" + aFacet,
                    aFacet + " requires a bounded logical name" );
        return JSON::object();
    }

    JSON statements = JSON::array();

    for( size_t i = 2; i < node.children.size(); ++i )
    {
        if( aDocument.ListHead( node.children[i] ).empty() )
        {
            diagnostic( aResult, "error", "invalid_" + aFacet + "_statement",
                        aFacet + " payloads must contain named expressions" );
            continue;
        }

        statements.emplace_back( expressionToIr( aDocument, node.children[i] ) );
    }

    return { { "name", name }, { "statements", std::move( statements ) } };
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
    return {
        { "language", "kichad-design" },
        { "version", LANGUAGE_VERSION },
        { "syntax", "s-expression" },
        { "root", "kichad_design" },
        { "deterministic", true },
        { "hostCodeExecution", false },
        { "topLevelForms",
          JSON::array( {
                  { { "form", "(version 1)" }, { "required", true } },
                  { { "form", "(project NAME (title TEXT) (company TEXT) (revision TEXT))" },
                    { "required", true } },
                  { { "form", "(units mm|mil)" }, { "default", "mm" } },
                  { { "form", "(library symbol|footprint|model ID (uri URI))" } },
                  { { "form",
                      "(component REF (symbol LIB:ID) (value VALUE) (footprint LIB:ID) "
                      "(property NAME VALUE) (dnp true|false))" } },
                  { { "form", "(net NAME (pin REF NUMBER) (pin REF NUMBER) ...)" } },
                  { { "form", "(sheet NAME ...)" } },
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
                      "(source REF (manufacturer NAME) (mpn PART) (supplier NAME) (sku PART) "
                      "(quantity N))" } },
                  { { "form", "(check erc|drc|sourcing|footprints|fabrication)" } },
                  { { "form", "(output gerbers|drill|pick_place|bom|step|pdf)" } }
          } ) },
        { "compilerPasses",
          JSON::array( { "parse", "typecheck", "resolve", "plan", "snapshot", "schematic",
                         "libraries", "pcb", "sourcing", "erc", "drc", "fabrication" } ) },
        { "example",
          "(kichad_design\n"
          "  (version 1)\n"
          "  (project sensor (title \"Sensor Board\"))\n"
          "  (units mm)\n"
          "  (component R1 (symbol \"Device:R\") (value \"10k\")\n"
          "    (footprint \"Resistor_SMD:R_0603_1608Metric\"))\n"
          "  (component LED1 (symbol \"Device:LED\") (value \"GREEN\")\n"
          "    (footprint \"LED_SMD:LED_0603_1608Metric\"))\n"
          "  (net LED_A (pin R1 1) (pin LED1 1))\n"
          "  (check erc)\n"
          "  (check drc)\n"
          "  (output gerbers))" }
    };
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
            { "nets", JSON::array() } } },
        { "pcb", JSON::array() },
        { "rules", nullptr },
        { "netClasses", nullptr },
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
    std::set<std::string>    sourceIds;
    std::set<std::string>    checkKinds;
    std::set<std::string>    outputKinds;
    std::set<std::string>    connectedPins;
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
        else if( form == "sheet" )
        {
            JSON sheet = compileNamedFacet( *document, formNode, "sheet", result );
            const std::string name = sheet.value( "name", "" );

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
                "gerbers", "drill", "pick_place", "bom", "step", "pdf"
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

    if( !result.ir["schematic"]["components"].empty() || !result.ir["schematic"]["nets"].empty() )
        passes.emplace_back( "schematic" );

    if( !result.ir["pcb"].empty() )
        passes.emplace_back( "pcb" );

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
            { "pinConnections", pinConnections },
            { "boardStatements", result.ir["pcb"].size() },
            { "rules", result.ir["rules"].is_object() ? 1 : 0 },
            { "netClasses", result.ir["netClasses"].is_object()
                                      ? result.ir["netClasses"]["classes"].size()
                                      : 0 },
            { "netClassAssignments", result.ir["netClasses"].is_object()
                                               ? result.ir["netClasses"]["assignments"].size()
                                               : 0 },
            { "sourcingRecords", result.ir["sourcing"].size() },
            { "checks", result.ir["checks"].size() },
            { "outputs", result.ir["outputs"].size() } } }
    };

    result.ok = !containsError( result.diagnostics );
    return result;
}

} // namespace KICHAD
