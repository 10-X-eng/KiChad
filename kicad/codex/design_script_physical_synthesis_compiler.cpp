/*
 * This program source code file is part of KiChad, a Codex-integrated downstream of KiCad.
 *
 * Copyright (C) 2026 KiChad Developers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "design_script_physical_synthesis_compiler.h"

#include "lossless_sexpr_document.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <string_view>


namespace
{

using JSON = nlohmann::json;
using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using RESULT = KICHAD::DESIGN_SCRIPT_PHYSICAL_SYNTHESIS_COMPILER::RESULT;


void diagnostic( RESULT& aResult, const std::string& aCode, const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", "error" }, { "code", aCode },
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


bool distance( const std::string& aText, int64_t& aNanometers )
{
    long double value = 0.0L;
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const std::from_chars_result parsed = std::from_chars( begin, end, value );

    if( parsed.ec != std::errc() || parsed.ptr == begin || !std::isfinite( value ) )
        return false;

    const std::string_view unit( parsed.ptr, static_cast<size_t>( end - parsed.ptr ) );
    const std::map<std::string_view, long double> scales = {
        { "mm", 1.0e6L }, { "mil", 25400.0L }, { "um", 1000.0L }, { "nm", 1.0L }
    };
    const auto scale = scales.find( unit );

    if( scale == scales.end() )
        return false;

    const long double converted = std::round( value * scale->second );

    if( !std::isfinite( converted ) || converted < 1.0L || converted > 1.0e9L )
        return false;

    aNanometers = static_cast<int64_t>( converted );
    return true;
}


bool oneDistance( const DOCUMENT& aDocument, size_t aNode, int64_t& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string text;
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalar( aDocument, node.children[1], text ) && distance( text, aValue );
}


bool oneScalar( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalar( aDocument, node.children[1], aValue );
}


JSON compilePlacement( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    JSON placement = { { "gridNm", 1'000'000 }, { "clearanceNm", 500'000 },
                       { "edgeClearanceNm", 1'000'000 },
                       { "rotationsDegrees", JSON::array( { 0, 90 } ) } };
    std::set<std::string> fields;

    for( size_t index = 1; index < aDocument.Nodes().at( aNode ).children.size(); ++index )
    {
        const size_t child = aDocument.Nodes().at( aNode ).children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_synthesis_placement_field",
                        "synthesis placement repeats field " + head );
            continue;
        }

        if( head == "grid" || head == "clearance" || head == "edge_clearance" )
        {
            int64_t parsed = 0;

            if( !oneDistance( aDocument, child, parsed ) )
                diagnostic( aResult, "invalid_synthesis_placement_distance",
                            "placement " + head + " requires a positive bounded distance" );
            else
                placement[head == "grid" ? "gridNm"
                          : head == "clearance" ? "clearanceNm" : "edgeClearanceNm"] = parsed;
        }
        else if( head == "rotations" )
        {
            const DOCUMENT::NODE& form = aDocument.Nodes().at( child );
            JSON rotations = JSON::array();
            std::set<int> unique;

            for( size_t rotationIndex = 1; rotationIndex < form.children.size(); ++rotationIndex )
            {
                std::string text;
                int rotation = 0;
                const char* rotationBegin = nullptr;
                const char* rotationEnd = nullptr;
                std::from_chars_result converted;

                if( scalar( aDocument, form.children[rotationIndex], text )
                    && text.ends_with( "deg" ) )
                {
                    rotationBegin = text.data();
                    rotationEnd = text.data() + text.size() - 3;
                    converted = std::from_chars( rotationBegin, rotationEnd, rotation );
                }

                if( !rotationBegin || converted.ec != std::errc()
                    || converted.ptr != rotationEnd
                    || ( rotation != 0 && rotation != 90 && rotation != 180
                         && rotation != 270 )
                    || !unique.emplace( rotation ).second )
                {
                    diagnostic( aResult, "invalid_synthesis_placement_rotations",
                                "placement rotations must be unique 0deg, 90deg, 180deg, or 270deg" );
                    continue;
                }

                rotations.push_back( rotation );
            }

            if( rotations.empty() )
                diagnostic( aResult, "empty_synthesis_placement_rotations",
                            "placement rotations requires at least one orientation" );
            else
                placement["rotationsDegrees"] = std::move( rotations );
        }
        else
        {
            diagnostic( aResult, "unknown_synthesis_placement_field",
                        "synthesis placement does not support field " + head );
        }
    }

    return placement;
}


JSON compileRouting( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    JSON routing = { { "gridNm", 250'000 }, { "clearanceNm", 200'000 },
                     { "widthNm", 250'000 }, { "layer", "F.Cu" } };
    std::set<std::string> fields;

    for( size_t index = 1; index < aDocument.Nodes().at( aNode ).children.size(); ++index )
    {
        const size_t child = aDocument.Nodes().at( aNode ).children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_synthesis_routing_field",
                        "synthesis routing repeats field " + head );
            continue;
        }

        if( head == "grid" || head == "clearance" || head == "width" )
        {
            int64_t parsed = 0;

            if( !oneDistance( aDocument, child, parsed ) )
                diagnostic( aResult, "invalid_synthesis_routing_distance",
                            "routing " + head + " requires a positive bounded distance" );
            else
                routing[head + "Nm"] = parsed;
        }
        else if( head == "layer" )
        {
            std::string layer;

            if( !oneScalar( aDocument, child, layer )
                || ( layer != "F.Cu" && layer != "B.Cu" ) )
            {
                diagnostic( aResult, "invalid_synthesis_routing_layer",
                            "routing layer must be F.Cu or B.Cu" );
            }
            else
            {
                routing["layer"] = layer;
            }
        }
        else
        {
            diagnostic( aResult, "unknown_synthesis_routing_field",
                        "synthesis routing does not support field " + head );
        }
    }

    if( routing["gridNm"].get<int64_t>() < routing["widthNm"].get<int64_t>() )
        diagnostic( aResult, "invalid_synthesis_routing_grid",
                    "routing grid must be at least the route width" );

    return routing;
}

} // namespace


KICHAD::DESIGN_SCRIPT_PHYSICAL_SYNTHESIS_COMPILER::RESULT
KICHAD::DESIGN_SCRIPT_PHYSICAL_SYNTHESIS_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    result.synthesis = { { "placement", nullptr }, { "routing", nullptr } };
    const LOSSLESS_SEXPR_DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );

    if( node.kind != LOSSLESS_SEXPR_DOCUMENT::NODE_KIND::LIST
        || aDocument.ListHead( aNode ) != "synthesize" )
    {
        diagnostic( result, "invalid_synthesis", "synthesize must be a named list" );
        return result;
    }

    std::set<std::string> sections;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( ( head != "placement" && head != "routing" )
            || !sections.emplace( head ).second )
        {
            diagnostic( result, "invalid_synthesis_section",
                        "synthesize supports one placement and/or one routing section" );
            continue;
        }

        result.synthesis[head] = head == "placement"
                                         ? compilePlacement( aDocument, child, result )
                                         : compileRouting( aDocument, child, result );
    }

    if( sections.empty() )
        diagnostic( result, "empty_synthesis", "synthesize requires placement and/or routing" );

    return result;
}
