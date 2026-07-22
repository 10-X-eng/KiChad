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

#include "design_script_footprint_hole_treatment_compiler.h"
#include "kichad_from_chars.h"

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
using RESULT = KICHAD::DESIGN_SCRIPT_FOOTPRINT_HOLE_TREATMENT_COMPILER::RESULT;


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

    if( !std::isfinite( rounded ) || rounded <= 0 || rounded > 1'000'000'000.0L )
        return false;

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

    if( converted.ec != std::errc() || converted.ptr != number.data() + number.size()
        || !std::isfinite( degrees ) )
    {
        return false;
    }

    const long double tenths = std::round( degrees * 10.0L );

    if( tenths <= 0.0L || tenths > 1800.0L
        || std::fabs( tenths - degrees * 10.0L ) > 0.000001L )
    {
        return false;
    }

    aTenths = static_cast<int64_t>( tenths );
    return true;
}


JSON compileTenting( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    JSON tenting = { { "front", "inherit" }, { "back", "inherit" } };
    std::set<std::string> sides;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string side = aDocument.ListHead( child );
        std::string value;

        if( ( side != "front" && side != "back" ) || !sides.emplace( side ).second
            || !oneValue( aDocument, child, value )
            || ( value != "inherit" && value != "open" && value != "tented" ) )
        {
            diagnostic( aResult, "invalid_authored_footprint_pad_tenting",
                        "tenting requires unique front/back values: inherit, open, or tented" );
            continue;
        }

        tenting[side] = value;
    }

    if( sides.size() != 2 )
        diagnostic( aResult, "incomplete_authored_footprint_pad_tenting",
                    "tenting explicitly requires both front and back behavior" );

    return tenting;
}


JSON compileMachining( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string side;
    std::string mode;

    if( node.children.size() < 4 || !scalar( aDocument, node.children[1], side )
        || !scalar( aDocument, node.children[2], mode )
        || ( side != "front" && side != "back" )
        || ( mode != "counterbore" && mode != "countersink" ) )
    {
        diagnostic( aResult, "invalid_authored_footprint_pad_post_machining",
                    "post_machining requires front/back and counterbore/countersink" );
        return JSON::object();
    }

    JSON machining = {
        { "side", side }, { "mode", mode }, { "diameterNm", nullptr },
        { "depthNm", nullptr }, { "angleTenths", nullptr }
    };
    std::set<std::string> fields;

    for( size_t index = 3; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );
        std::string value;

        if( !fields.emplace( head ).second || !oneValue( aDocument, child, value ) )
        {
            diagnostic( aResult, "invalid_authored_footprint_pad_post_machining_field",
                        "post-machining fields must be unique one-value forms" );
            continue;
        }

        if( head == "diameter" || head == "depth" )
        {
            int64_t parsed = 0;

            if( !distance( value, parsed ) )
                diagnostic( aResult, "invalid_authored_footprint_pad_post_machining_distance",
                            "post-machining diameter/depth requires a positive bounded distance" );
            else
                machining[head == "diameter" ? "diameterNm" : "depthNm"] = parsed;
        }
        else if( head == "angle" )
        {
            int64_t parsed = 0;

            if( !angle( value, parsed ) )
                diagnostic( aResult, "invalid_authored_footprint_pad_post_machining_angle",
                            "countersink angle requires 0.1 through 180 degrees" );
            else
                machining["angleTenths"] = parsed;
        }
        else
        {
            diagnostic( aResult, "unknown_authored_footprint_pad_post_machining_field",
                        "post_machining supports diameter, depth, and angle" );
        }
    }

    if( machining["diameterNm"].is_null() )
        diagnostic( aResult, "incomplete_authored_footprint_pad_post_machining",
                    "post_machining requires a finished diameter" );

    if( mode == "counterbore" && machining["depthNm"].is_null() )
        diagnostic( aResult, "incomplete_authored_footprint_pad_post_machining",
                    "counterbore requires a positive depth" );

    if( mode == "counterbore" && !machining["angleTenths"].is_null() )
        diagnostic( aResult, "invalid_authored_footprint_pad_post_machining_angle",
                    "counterbore does not accept an angle" );

    if( mode == "countersink" && machining["angleTenths"].is_null() )
        diagnostic( aResult, "incomplete_authored_footprint_pad_post_machining",
                    "countersink requires an included angle" );

    return machining;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_FOOTPRINT_HOLE_TREATMENT_COMPILER::RESULT
DESIGN_SCRIPT_FOOTPRINT_HOLE_TREATMENT_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );

    if( aDocument.ListHead( aNode ) != "hole_treatment" )
    {
        diagnostic( result, "invalid_authored_footprint_pad_hole_treatment",
                    "hole treatment requires one hole_treatment form" );
        return result;
    }

    result.treatment = {
        { "tenting", nullptr }, { "frontPostMachining", nullptr },
        { "backPostMachining", nullptr }
    };
    bool foundTenting = false;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "tenting" )
        {
            if( foundTenting )
                diagnostic( result, "duplicate_authored_footprint_pad_tenting",
                            "hole_treatment contains tenting once" );
            else
                result.treatment["tenting"] = compileTenting( aDocument, child, result );

            foundTenting = true;
        }
        else if( head == "post_machining" )
        {
            JSON machining = compileMachining( aDocument, child, result );
            const std::string side = machining.value( "side", "" );
            const std::string key = side == "front" ? "frontPostMachining"
                                    : side == "back" ? "backPostMachining" : "";

            if( !key.empty() )
            {
                if( !result.treatment[key].is_null() )
                    diagnostic( result, "duplicate_authored_footprint_pad_post_machining",
                                "each hole side accepts one post-machining operation" );
                else
                    result.treatment[key] = std::move( machining );
            }
        }
        else
        {
            diagnostic( result, "unknown_authored_footprint_pad_hole_treatment_field",
                        "hole_treatment supports tenting and post_machining" );
        }
    }

    if( result.treatment["tenting"].is_null()
        && result.treatment["frontPostMachining"].is_null()
        && result.treatment["backPostMachining"].is_null() )
    {
        diagnostic( result, "empty_authored_footprint_pad_hole_treatment",
                    "hole_treatment requires at least one manufacturing instruction" );
    }

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
