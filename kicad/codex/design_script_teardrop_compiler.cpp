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

#include "design_script_teardrop_compiler.h"
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
using RESULT = KICHAD::DESIGN_SCRIPT_TEARDROP_COMPILER::RESULT;

constexpr int64_t MAX_DISTANCE_NM = 1'000'000'000LL;


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

    if( !std::isfinite( rounded ) || rounded < 0
        || rounded > static_cast<long double>( MAX_DISTANCE_NM ) )
    {
        return false;
    }

    aNanometers = static_cast<int64_t>( rounded );
    return true;
}


bool ratio( const std::string& aText, int64_t aMinimumPpm, int64_t& aPpm )
{
    long double value = 0.0L;
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const std::from_chars_result converted = KICHAD::FromChars( begin, end, value );

    if( converted.ec != std::errc() || converted.ptr != end || !std::isfinite( value ) )
        return false;

    const long double ppm = std::round( value * 1'000'000.0L );

    if( ppm < static_cast<long double>( aMinimumPpm ) || ppm > 1'000'000.0L )
        return false;

    aPpm = static_cast<int64_t>( ppm );
    return true;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_TEARDROP_COMPILER::RESULT DESIGN_SCRIPT_TEARDROP_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );

    if( aDocument.ListHead( aNode ) != "teardrop" )
    {
        diagnostic( result, "invalid_authored_teardrop", "teardrop requires one semantic form" );
        return result;
    }

    result.teardrop = {
        { "enabled", false }, { "targetLengthRatioPpm", 0 }, { "maxLengthNm", 0 },
        { "targetWidthRatioPpm", 0 }, { "maxWidthNm", 0 }, { "edges", "" },
        { "trackWidthLimitPpm", 0 }, { "allowTwoSegments", false },
        { "preferZoneConnections", false }
    };
    std::set<std::string> fields;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const DOCUMENT::NODE& field = aDocument.Nodes().at( child );
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( result, "duplicate_authored_teardrop_field",
                        "teardrop field " + head + " occurs more than once" );
            continue;
        }

        if( head == "target_length" || head == "target_width" )
        {
            std::string ratioText;
            std::string maximumText;
            int64_t ratioPpm = 0;
            int64_t maximumNm = 0;
            const int64_t minimum = head == "target_length" ? 200'000 : 600'000;

            if( field.children.size() != 3 || !scalar( aDocument, field.children[1], ratioText )
                || !scalar( aDocument, field.children[2], maximumText )
                || !ratio( ratioText, minimum, ratioPpm )
                || !distance( maximumText, maximumNm ) )
            {
                diagnostic( result, "invalid_authored_teardrop_target",
                            head + " requires a supported ratio and non-negative maximum distance" );
            }
            else
            {
                result.teardrop[head == "target_length"
                                        ? "targetLengthRatioPpm" : "targetWidthRatioPpm"] = ratioPpm;
                result.teardrop[head == "target_length" ? "maxLengthNm" : "maxWidthNm"] = maximumNm;
            }

            continue;
        }

        std::string value;

        if( field.children.size() != 2 || !scalar( aDocument, field.children[1], value ) )
        {
            diagnostic( result, "invalid_authored_teardrop_field",
                        "teardrop field " + head + " requires one value" );
            continue;
        }

        if( head == "enabled" || head == "allow_two_segments"
            || head == "prefer_zone_connections" )
        {
            bool parsed = false;

            if( !boolean( value, parsed ) )
                diagnostic( result, "invalid_authored_teardrop_boolean",
                            head + " requires true or false" );
            else
                result.teardrop[head == "enabled" ? "enabled"
                                        : head == "allow_two_segments" ? "allowTwoSegments"
                                                                         : "preferZoneConnections"] = parsed;
        }
        else if( head == "edges" )
        {
            if( value != "straight" && value != "curved" )
                diagnostic( result, "invalid_authored_teardrop_edges",
                            "teardrop edges must be straight or curved" );
            else
                result.teardrop["edges"] = value;
        }
        else if( head == "track_width_limit" )
        {
            int64_t parsed = 0;

            if( !ratio( value, 0, parsed ) )
                diagnostic( result, "invalid_authored_teardrop_track_width_limit",
                            "track_width_limit must be a ratio from 0 through 1" );
            else
                result.teardrop["trackWidthLimitPpm"] = parsed;
        }
        else
        {
            diagnostic( result, "unknown_authored_teardrop_field",
                        "unsupported teardrop field " + head );
        }
    }

    static const std::set<std::string> required = {
        "enabled", "target_length", "target_width", "edges", "track_width_limit",
        "allow_two_segments", "prefer_zone_connections"
    };

    for( const std::string& field : required )
    {
        if( !fields.contains( field ) )
            diagnostic( result, "incomplete_authored_teardrop",
                        "teardrop requires explicit " + field );
    }

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
