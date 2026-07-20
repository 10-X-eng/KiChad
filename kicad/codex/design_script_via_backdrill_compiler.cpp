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

#include "design_script_via_backdrill_compiler.h"

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
using RESULT = KICHAD::DESIGN_SCRIPT_VIA_BACKDRILL_COMPILER::RESULT;


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

    if( !std::isfinite( rounded ) || rounded <= 0
        || rounded > static_cast<long double>( std::numeric_limits<int32_t>::max() ) )
    {
        return false;
    }

    aNanometers = static_cast<int64_t>( rounded );
    return true;
}


JSON compileSide( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    const std::string side = aDocument.ListHead( aNode );
    JSON backdrill = { { "side", side }, { "diameterNm", 0 }, { "stopLayer", "" } };
    std::set<std::string> fields;

    if( side != "top" && side != "bottom" )
    {
        diagnostic( aResult, "invalid_authored_via_backdrill_side",
                    "backdrills contains only top and bottom operations" );
        return backdrill;
    }

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );
        std::string value;

        if( !fields.emplace( head ).second || !oneValue( aDocument, child, value ) )
        {
            diagnostic( aResult, "invalid_authored_via_backdrill_field",
                        "backdrill fields are unique one-value forms" );
            continue;
        }

        if( head == "diameter" )
        {
            int64_t parsed = 0;

            if( !distance( value, parsed ) )
                diagnostic( aResult, "invalid_authored_via_backdrill_diameter",
                            "backdrill diameter requires a positive bounded distance" );
            else
                backdrill["diameterNm"] = parsed;
        }
        else if( head == "stop_layer" )
        {
            if( value.empty() || value.size() > 32 )
                diagnostic( aResult, "invalid_authored_via_backdrill_stop_layer",
                            "backdrill stop_layer requires one bounded copper layer" );
            else
                backdrill["stopLayer"] = value;
        }
        else
        {
            diagnostic( aResult, "unknown_authored_via_backdrill_field",
                        "backdrill supports diameter and stop_layer" );
        }
    }

    if( backdrill["diameterNm"].get<int64_t>() <= 0
        || backdrill["stopLayer"].get<std::string>().empty() )
    {
        diagnostic( aResult, "incomplete_authored_via_backdrill",
                    "each backdrill requires diameter and stop_layer" );
    }

    return backdrill;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_VIA_BACKDRILL_COMPILER::RESULT
DESIGN_SCRIPT_VIA_BACKDRILL_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );

    if( aDocument.ListHead( aNode ) != "backdrills" )
    {
        diagnostic( result, "invalid_authored_via_backdrills",
                    "via backdrilling requires one backdrills form" );
        return result;
    }

    result.backdrills = { { "top", nullptr }, { "bottom", nullptr } };

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string side = aDocument.ListHead( child );
        JSON operation = compileSide( aDocument, child, result );

        if( ( side == "top" || side == "bottom" ) && !result.backdrills[side].is_null() )
            diagnostic( result, "duplicate_authored_via_backdrill",
                        "each via side accepts one backdrill" );
        else if( side == "top" || side == "bottom" )
            result.backdrills[side] = std::move( operation );
    }

    if( result.backdrills["top"].is_null() && result.backdrills["bottom"].is_null() )
        diagnostic( result, "empty_authored_via_backdrills",
                    "backdrills requires a top or bottom operation" );

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
