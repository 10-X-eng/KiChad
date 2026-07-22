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

#include "design_script_via_protection_compiler.h"
#include "kichad_from_chars.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_VIA_PROTECTION_COMPILER::RESULT;


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

    if( !std::isfinite( rounded ) || rounded <= 0
        || rounded > static_cast<long double>( std::numeric_limits<int32_t>::max() ) )
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


JSON sidePair( const DOCUMENT& aDocument, size_t aNode, const std::string& aFeature,
               const std::set<std::string>& aValues, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    JSON pair = { { "front", "inherit" }, { "back", "inherit" } };
    std::set<std::string> sides;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string side = aDocument.ListHead( child );
        std::string value;

        if( ( side != "front" && side != "back" ) || !sides.emplace( side ).second
            || !oneValue( aDocument, child, value ) || !aValues.contains( value ) )
        {
            diagnostic( aResult, "invalid_authored_via_" + aFeature,
                        aFeature + " requires unique explicit front and back values" );
            continue;
        }

        pair[side] = value;
    }

    if( sides.size() != 2 )
        diagnostic( aResult, "incomplete_authored_via_" + aFeature,
                    aFeature + " requires both front and back behavior" );

    return pair;
}


JSON machining( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string side;
    std::string mode;

    if( node.children.size() < 4 || !scalar( aDocument, node.children[1], side )
        || !scalar( aDocument, node.children[2], mode )
        || ( side != "front" && side != "back" )
        || ( mode != "counterbore" && mode != "countersink" ) )
    {
        diagnostic( aResult, "invalid_authored_via_post_machining",
                    "post_machining requires front/back and counterbore/countersink" );
        return JSON::object();
    }

    JSON result = {
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
            diagnostic( aResult, "invalid_authored_via_post_machining_field",
                        "post-machining fields are unique one-value forms" );
            continue;
        }

        if( head == "diameter" || head == "depth" )
        {
            int64_t parsed = 0;

            if( !distance( value, parsed ) )
                diagnostic( aResult, "invalid_authored_via_post_machining_distance",
                            "post-machining diameter/depth requires a positive distance" );
            else
                result[head == "diameter" ? "diameterNm" : "depthNm"] = parsed;
        }
        else if( head == "angle" )
        {
            int64_t parsed = 0;

            if( !angle( value, parsed ) )
                diagnostic( aResult, "invalid_authored_via_post_machining_angle",
                            "post-machining angle requires 0.1 through 180 degrees" );
            else
                result["angleTenths"] = parsed;
        }
        else
        {
            diagnostic( aResult, "unknown_authored_via_post_machining_field",
                        "post_machining supports diameter, depth, and angle" );
        }
    }

    if( result["diameterNm"].is_null() )
        diagnostic( aResult, "incomplete_authored_via_post_machining",
                    "post_machining requires a finished diameter" );

    if( mode == "counterbore" && result["depthNm"].is_null() )
        diagnostic( aResult, "incomplete_authored_via_post_machining",
                    "counterbore requires depth" );

    if( mode == "counterbore" && !result["angleTenths"].is_null() )
        diagnostic( aResult, "invalid_authored_via_post_machining_angle",
                    "counterbore does not accept angle" );

    if( mode == "countersink" && result["angleTenths"].is_null() )
        diagnostic( aResult, "incomplete_authored_via_post_machining",
                    "countersink requires included angle" );

    return result;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_VIA_PROTECTION_COMPILER::RESULT
DESIGN_SCRIPT_VIA_PROTECTION_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );

    if( aDocument.ListHead( aNode ) != "protection" )
    {
        diagnostic( result, "invalid_authored_via_protection",
                    "via protection requires one protection form" );
        return result;
    }

    result.protection = {
        { "tenting", nullptr }, { "covering", nullptr }, { "plugging", nullptr },
        { "filling", nullptr }, { "capping", nullptr },
        { "frontPostMachining", nullptr }, { "backPostMachining", nullptr }
    };
    std::set<std::string> singletonFields;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "post_machining" )
        {
            JSON compiled = machining( aDocument, child, result );
            const std::string side = compiled.value( "side", "" );
            const std::string key = side == "front" ? "frontPostMachining"
                                    : side == "back" ? "backPostMachining" : "";

            if( !key.empty() )
            {
                if( !result.protection[key].is_null() )
                    diagnostic( result, "duplicate_authored_via_post_machining",
                                "each via side accepts one post-machining operation" );
                else
                    result.protection[key] = std::move( compiled );
            }

            continue;
        }

        if( !singletonFields.emplace( head ).second )
        {
            diagnostic( result, "duplicate_authored_via_protection_field",
                        "via protection field " + head + " occurs more than once" );
            continue;
        }

        if( head == "tenting" )
        {
            result.protection[head] = sidePair(
                    aDocument, child, head, { "inherit", "open", "tented" }, result );
        }
        else if( head == "covering" )
        {
            result.protection[head] = sidePair(
                    aDocument, child, head, { "inherit", "covered", "uncovered" }, result );
        }
        else if( head == "plugging" )
        {
            result.protection[head] = sidePair(
                    aDocument, child, head, { "inherit", "plugged", "unplugged" }, result );
        }
        else if( head == "filling" || head == "capping" )
        {
            std::string value;
            const std::set<std::string> values = head == "filling"
                    ? std::set<std::string>{ "inherit", "filled", "unfilled" }
                    : std::set<std::string>{ "inherit", "capped", "uncapped" };

            if( !oneValue( aDocument, child, value ) || !values.contains( value ) )
                diagnostic( result, "invalid_authored_via_" + head,
                            head + " requires one explicit manufacturing state" );
            else
                result.protection[head] = value;
        }
        else
        {
            diagnostic( result, "unknown_authored_via_protection_field",
                        "protection supports tenting, covering, plugging, filling, capping, "
                        "and post_machining" );
        }
    }

    bool empty = true;

    for( const auto& [key, value] : result.protection.items() )
        empty = empty && value.is_null();

    if( empty )
        diagnostic( result, "empty_authored_via_protection",
                    "protection requires at least one manufacturing instruction" );

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
