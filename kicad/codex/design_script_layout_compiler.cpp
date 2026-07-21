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

#include "design_script_layout_compiler.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string_view>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_LAYOUT_COMPILER::RESULT;

constexpr size_t MAX_IDENTIFIER_BYTES = 128;
constexpr size_t MAX_LAYOUT_CONSTRAINTS = 4096;
constexpr int64_t MAX_DISTANCE_NM = 2'000'000'000LL;


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


bool distance( const std::string& aText, int64_t& aNanometers )
{
    long double value = 0.0L;
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const std::from_chars_result converted = std::from_chars( begin, end, value );

    if( converted.ec != std::errc() || converted.ptr == begin || !std::isfinite( value ) )
        return false;

    const std::string_view unit( converted.ptr,
                                 static_cast<size_t>( end - converted.ptr ) );
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

    if( !std::isfinite( rounded ) || rounded < 0.0L || rounded > MAX_DISTANCE_NM )
        return false;

    aNanometers = static_cast<int64_t>( rounded );
    return true;
}


bool distanceField( const DOCUMENT& aDocument, size_t aNode, int64_t& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string text;
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalar( aDocument, node.children[1], text ) && distance( text, aValue );
}


bool unsignedField( const DOCUMENT& aDocument, size_t aNode, uint32_t& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string text;

    if( node.kind != DOCUMENT::NODE_KIND::LIST || node.children.size() != 2
        || !scalar( aDocument, node.children[1], text ) )
    {
        return false;
    }

    const std::from_chars_result parsed =
            std::from_chars( text.data(), text.data() + text.size(), aValue );
    return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size();
}


bool oneScalar( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalar( aDocument, node.children[1], aValue );
}


bool collectFields( const DOCUMENT& aDocument, size_t aNode, size_t aBegin,
                    const std::set<std::string>& aAllowed,
                    std::map<std::string, size_t>& aFields, RESULT& aResult,
                    const std::string& aContext )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    bool valid = true;

    for( size_t index = aBegin; index < node.children.size(); ++index )
    {
        const std::string head = aDocument.ListHead( node.children[index] );

        if( !aAllowed.contains( head ) )
        {
            diagnostic( aResult, "unknown_layout_field",
                        aContext + " does not support field '" + head + "'" );
            valid = false;
        }
        else if( !aFields.emplace( head, node.children[index] ).second )
        {
            diagnostic( aResult, "duplicate_layout_field",
                        aContext + " field '" + head + "' occurs more than once" );
            valid = false;
        }
    }

    return valid;
}


void compileBoard( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 1, { "maximum_width", "maximum_height" },
                   fields, aResult, "layout board" );
    JSON board = JSON::object();

    for( const auto& [name, key] :
         std::array<std::pair<const char*, const char*>, 2>{
                 std::pair{ "maximum_width", "maximumWidthNm" },
                 std::pair{ "maximum_height", "maximumHeightNm" } } )
    {
        int64_t value = 0;

        if( !fields.contains( name ) || !distanceField( aDocument, fields[name], value )
            || value <= 0 )
        {
            diagnostic( aResult, "invalid_layout_board_limit",
                        std::string( "layout board requires a positive (" ) + name
                                + " DISTANCE)" );
        }
        else
        {
            board[key] = value;
        }
    }

    aResult.layout["board"] = std::move( board );
}


void compileNear( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult,
                  std::set<std::string>& aKeys )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string first;
    std::string second;
    std::map<std::string, size_t> fields;

    if( node.children.size() < 4 || !scalar( aDocument, node.children[1], first )
        || !identifier( first ) || !scalar( aDocument, node.children[2], second )
        || !identifier( second ) || first == second )
    {
        diagnostic( aResult, "invalid_layout_near",
                    "layout near requires two distinct component references" );
    }

    collectFields( aDocument, aNode, 3, { "maximum" }, fields, aResult, "layout near" );
    int64_t maximum = 0;

    if( !fields.contains( "maximum" )
        || !distanceField( aDocument, fields["maximum"], maximum ) || maximum <= 0 )
    {
        diagnostic( aResult, "invalid_layout_near",
                    "layout near requires a positive (maximum DISTANCE)" );
    }

    std::string orderedFirst = std::min( first, second );
    std::string orderedSecond = std::max( first, second );
    const std::string key = "near\n" + orderedFirst + "\n" + orderedSecond;

    if( identifier( first ) && identifier( second ) && !aKeys.emplace( key ).second )
        diagnostic( aResult, "duplicate_layout_constraint",
                    "layout near constraint occurs more than once" );

    aResult.componentReferences.push_back( first );
    aResult.componentReferences.push_back( second );
    aResult.layout["placement"].push_back(
            { { "kind", "near" }, { "first", first }, { "second", second },
              { "maximumNm", maximum } } );
}


void compileAlign( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult,
                   std::set<std::string>& aKeys )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string first;
    std::string second;
    std::map<std::string, size_t> fields;

    if( node.children.size() < 5 || !scalar( aDocument, node.children[1], first )
        || !identifier( first ) || !scalar( aDocument, node.children[2], second )
        || !identifier( second ) || first == second )
    {
        diagnostic( aResult, "invalid_layout_align",
                    "layout align requires two distinct component references" );
    }

    collectFields( aDocument, aNode, 3, { "axis", "tolerance" }, fields,
                   aResult, "layout align" );
    std::string axis;
    int64_t tolerance = 0;

    if( !fields.contains( "axis" ) || !oneScalar( aDocument, fields["axis"], axis )
        || ( axis != "x" && axis != "y" ) )
    {
        diagnostic( aResult, "invalid_layout_align",
                    "layout align axis must be x or y" );
    }

    if( !fields.contains( "tolerance" )
        || !distanceField( aDocument, fields["tolerance"], tolerance ) )
    {
        diagnostic( aResult, "invalid_layout_align",
                    "layout align requires a non-negative (tolerance DISTANCE)" );
    }

    const std::string key = "align\n" + std::min( first, second ) + "\n"
                            + std::max( first, second ) + "\n" + axis;

    if( identifier( first ) && identifier( second ) && !axis.empty()
        && !aKeys.emplace( key ).second )
    {
        diagnostic( aResult, "duplicate_layout_constraint",
                    "layout align constraint occurs more than once" );
    }

    aResult.componentReferences.push_back( first );
    aResult.componentReferences.push_back( second );
    aResult.layout["placement"].push_back(
            { { "kind", "align" }, { "first", first }, { "second", second },
              { "axis", axis }, { "toleranceNm", tolerance } } );
}


void compileEdge( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult,
                  std::set<std::string>& aKeys )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string component;
    std::string edge;
    std::map<std::string, size_t> fields;
    static const std::set<std::string> edges = { "left", "right", "top", "bottom" };

    if( node.children.size() < 4 || !scalar( aDocument, node.children[1], component )
        || !identifier( component ) || !scalar( aDocument, node.children[2], edge )
        || !edges.contains( edge ) )
    {
        diagnostic( aResult, "invalid_layout_edge",
                    "layout edge requires COMPONENT and left, right, top, or bottom" );
    }

    collectFields( aDocument, aNode, 3, { "maximum" }, fields, aResult, "layout edge" );
    int64_t maximum = 0;

    if( !fields.contains( "maximum" )
        || !distanceField( aDocument, fields["maximum"], maximum ) )
    {
        diagnostic( aResult, "invalid_layout_edge",
                    "layout edge requires a non-negative (maximum DISTANCE)" );
    }

    if( identifier( component ) && edges.contains( edge )
        && !aKeys.emplace( "edge\n" + component + "\n" + edge ).second )
    {
        diagnostic( aResult, "duplicate_layout_constraint",
                    "layout edge constraint occurs more than once" );
    }

    aResult.componentReferences.push_back( component );
    aResult.layout["placement"].push_back(
            { { "kind", "edge" }, { "component", component }, { "edge", edge },
              { "maximumNm", maximum } } );
}


void compileGroup( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult,
                   std::set<std::string>& aKeys )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;
    std::map<std::string, size_t> fields;

    if( node.children.size() < 4 || !scalar( aDocument, node.children[1], id )
        || !identifier( id ) )
    {
        diagnostic( aResult, "invalid_layout_group",
                    "layout group requires a bounded unique ID" );
    }

    collectFields( aDocument, aNode, 2, { "members", "maximum_span" }, fields,
                   aResult, "layout group" );
    JSON members = JSON::array();
    std::set<std::string> unique;

    if( !fields.contains( "members" ) )
    {
        diagnostic( aResult, "invalid_layout_group",
                    "layout group requires (members COMPONENT...)" );
    }
    else
    {
        const DOCUMENT::NODE& memberNode = aDocument.Nodes().at( fields["members"] );

        for( size_t index = 1; index < memberNode.children.size(); ++index )
        {
            std::string component;

            if( !scalar( aDocument, memberNode.children[index], component )
                || !identifier( component ) || !unique.emplace( component ).second )
            {
                diagnostic( aResult, "invalid_layout_group",
                            "layout group members must be unique component references" );
                continue;
            }

            members.push_back( component );
            aResult.componentReferences.push_back( component );
        }

        if( members.size() < 2 || members.size() > 1024 )
            diagnostic( aResult, "invalid_layout_group",
                        "layout group requires 2 through 1024 members" );
    }

    int64_t maximumSpan = 0;

    if( !fields.contains( "maximum_span" )
        || !distanceField( aDocument, fields["maximum_span"], maximumSpan )
        || maximumSpan <= 0 )
    {
        diagnostic( aResult, "invalid_layout_group",
                    "layout group requires a positive (maximum_span DISTANCE)" );
    }

    if( identifier( id ) && !aKeys.emplace( "group\n" + id ).second )
        diagnostic( aResult, "duplicate_layout_constraint",
                    "layout group " + id + " occurs more than once" );

    aResult.layout["placement"].push_back(
            { { "kind", "group" }, { "id", id }, { "members", std::move( members ) },
              { "maximumSpanNm", maximumSpan } } );
}


void compilePlacement( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::set<std::string> keys;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        if( aResult.layout["placement"].size() >= MAX_LAYOUT_CONSTRAINTS )
        {
            diagnostic( aResult, "too_many_layout_constraints",
                        "layout supports at most 4096 placement constraints" );
            break;
        }

        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "near" )
            compileNear( aDocument, child, aResult, keys );
        else if( head == "align" )
            compileAlign( aDocument, child, aResult, keys );
        else if( head == "edge" )
            compileEdge( aDocument, child, aResult, keys );
        else if( head == "group" )
            compileGroup( aDocument, child, aResult, keys );
        else
            diagnostic( aResult, "unknown_layout_placement_constraint",
                        "layout placement constraint '" + head + "' is not supported" );
    }
}


bool compileGeometry( const DOCUMENT& aDocument, size_t aNode, std::string& aGeometry )
{
    static const std::set<std::string> geometries = {
        "any", "orthogonal", "octilinear"
    };
    return oneScalar( aDocument, aNode, aGeometry ) && geometries.contains( aGeometry );
}


JSON compileRoutingPolicy( const DOCUMENT& aDocument, size_t aNode, size_t aBegin,
                           bool aDefaults, RESULT& aResult, const std::string& aContext )
{
    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, aBegin,
                   { "geometry", "maximum_vias", "maximum_length" },
                   fields, aResult, aContext );
    JSON policy = JSON::object();
    std::string geometry;
    uint32_t maximumVias = 0;
    int64_t maximumLength = 0;

    if( fields.contains( "geometry" ) )
    {
        if( !compileGeometry( aDocument, fields["geometry"], geometry ) )
            diagnostic( aResult, "invalid_layout_routing_geometry",
                        aContext + " geometry must be any, orthogonal, or octilinear" );
        else
            policy["geometry"] = geometry;
    }

    if( fields.contains( "maximum_vias" ) )
    {
        if( !unsignedField( aDocument, fields["maximum_vias"], maximumVias )
            || maximumVias > 1'000'000 )
        {
            diagnostic( aResult, "invalid_layout_routing_vias",
                        aContext + " maximum_vias must be between 0 and 1000000" );
        }
        else
        {
            policy["maximumVias"] = maximumVias;
        }
    }

    if( fields.contains( "maximum_length" ) )
    {
        if( !distanceField( aDocument, fields["maximum_length"], maximumLength )
            || maximumLength <= 0 )
        {
            diagnostic( aResult, "invalid_layout_routing_length",
                        aContext + " maximum_length must be a positive distance" );
        }
        else
        {
            policy["maximumLengthNm"] = maximumLength;
        }
    }

    if( aDefaults )
    {
        if( !policy.contains( "geometry" ) || !policy.contains( "maximumVias" ) )
            diagnostic( aResult, "incomplete_layout_routing_defaults",
                        "layout routing defaults require geometry and maximum_vias" );
    }
    else if( policy.empty() )
    {
        diagnostic( aResult, "empty_layout_net_policy",
                    "a layout net policy must override at least one routing field" );
    }

    return policy;
}


void compileBundle( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult,
                    std::set<std::string>& aIds )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;
    std::map<std::string, size_t> fields;

    if( node.children.size() < 4 || !scalar( aDocument, node.children[1], id )
        || !identifier( id ) )
    {
        diagnostic( aResult, "invalid_layout_bundle",
                    "layout routing bundle requires a bounded unique ID" );
    }

    collectFields( aDocument, aNode, 2, { "nets", "maximum_skew" }, fields,
                   aResult, "layout routing bundle" );
    JSON nets = JSON::array();
    std::set<std::string> unique;

    if( !fields.contains( "nets" ) )
    {
        diagnostic( aResult, "invalid_layout_bundle",
                    "layout routing bundle requires (nets NET...)" );
    }
    else
    {
        const DOCUMENT::NODE& netNode = aDocument.Nodes().at( fields["nets"] );

        for( size_t index = 1; index < netNode.children.size(); ++index )
        {
            std::string net;

            if( !scalar( aDocument, netNode.children[index], net ) || !identifier( net )
                || !unique.emplace( net ).second )
            {
                diagnostic( aResult, "invalid_layout_bundle",
                            "layout bundle nets must be unique bounded net names" );
                continue;
            }

            nets.push_back( net );
            aResult.netReferences.push_back( net );
        }

        if( nets.size() < 2 || nets.size() > 1024 )
            diagnostic( aResult, "invalid_layout_bundle",
                        "layout routing bundle requires 2 through 1024 nets" );
    }

    int64_t maximumSkew = 0;

    if( !fields.contains( "maximum_skew" )
        || !distanceField( aDocument, fields["maximum_skew"], maximumSkew ) )
    {
        diagnostic( aResult, "invalid_layout_bundle",
                    "layout routing bundle requires a non-negative maximum_skew" );
    }

    if( identifier( id ) && !aIds.emplace( id ).second )
        diagnostic( aResult, "duplicate_layout_bundle",
                    "layout routing bundle " + id + " occurs more than once" );

    aResult.layout["routing"]["bundles"].push_back(
            { { "id", id }, { "nets", std::move( nets ) },
              { "maximumSkewNm", maximumSkew } } );
}


void compileRouting( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    bool sawDefaults = false;
    std::set<std::string> nets;
    std::set<std::string> bundleIds;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const DOCUMENT::NODE& childNode = aDocument.Nodes().at( child );
        const std::string head = aDocument.ListHead( child );

        if( head == "defaults" )
        {
            if( sawDefaults )
                diagnostic( aResult, "duplicate_layout_routing_defaults",
                            "layout routing defaults occurs more than once" );

            aResult.layout["routing"]["defaults"] =
                    compileRoutingPolicy( aDocument, child, 1, true, aResult,
                                          "layout routing defaults" );
            sawDefaults = true;
        }
        else if( head == "net" )
        {
            std::string net;

            if( childNode.children.size() < 3
                || !scalar( aDocument, childNode.children[1], net ) || !identifier( net ) )
            {
                diagnostic( aResult, "invalid_layout_net_policy",
                            "layout routing net requires a bounded net name and policy" );
            }

            if( identifier( net ) && !nets.emplace( net ).second )
                diagnostic( aResult, "duplicate_layout_net_policy",
                            "layout routing net " + net + " occurs more than once" );

            JSON policy = compileRoutingPolicy( aDocument, child, 2, false, aResult,
                                                "layout routing net " + net );
            policy["net"] = net;
            aResult.layout["routing"]["nets"].push_back( std::move( policy ) );
            aResult.netReferences.push_back( net );
        }
        else if( head == "bundle" )
        {
            compileBundle( aDocument, child, aResult, bundleIds );
        }
        else
        {
            diagnostic( aResult, "unknown_layout_routing_statement",
                        "layout routing statement '" + head + "' is not supported" );
        }
    }

    if( !sawDefaults )
        diagnostic( aResult, "missing_layout_routing_defaults",
                    "layout routing requires one defaults declaration" );
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_LAYOUT_COMPILER::RESULT DESIGN_SCRIPT_LAYOUT_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    result.layout = { { "kind", "layout" },
                      { "board", JSON::object() },
                      { "placement", JSON::array() },
                      { "routing",
                        { { "defaults", JSON::object() },
                          { "nets", JSON::array() },
                          { "bundles", JSON::array() } } },
                      { "typed", true } };
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::set<std::string> sections;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head != "board" && head != "placement" && head != "routing" )
        {
            diagnostic( result, "unknown_layout_section",
                        "layout section '" + head + "' is not supported" );
            continue;
        }

        if( !sections.emplace( head ).second )
        {
            diagnostic( result, "duplicate_layout_section",
                        "layout " + head + " occurs more than once" );
            continue;
        }

        if( head == "board" )
            compileBoard( aDocument, child, result );
        else if( head == "placement" )
            compilePlacement( aDocument, child, result );
        else
            compileRouting( aDocument, child, result );
    }

    if( !sections.contains( "board" ) )
        diagnostic( result, "missing_layout_board", "layout requires one board section" );

    if( !sections.contains( "routing" ) )
        diagnostic( result, "missing_layout_routing", "layout requires one routing section" );

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
