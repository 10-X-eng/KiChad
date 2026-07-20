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

#include "design_script_footprint_settings_compiler.h"

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
using RESULT = KICHAD::DESIGN_SCRIPT_FOOTPRINT_SETTINGS_COMPILER::RESULT;


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

    if( !std::isfinite( rounded )
        || rounded < static_cast<long double>( std::numeric_limits<int>::min() )
        || rounded > static_cast<long double>( std::numeric_limits<int>::max() ) )
    {
        return false;
    }

    aNanometers = static_cast<int64_t>( rounded );
    return true;
}


bool decimalPpm( const std::string& aText, int64_t& aPpm )
{
    long double value = 0.0L;
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const std::from_chars_result converted = std::from_chars( begin, end, value );

    if( converted.ec != std::errc() || converted.ptr != end || !std::isfinite( value ) )
        return false;

    const long double ppm = std::round( value * 1'000'000.0L );

    if( ppm < -1'000'000.0L || ppm > 1'000'000.0L )
        return false;

    aPpm = static_cast<int64_t>( ppm );
    return true;
}


bool innerLayer( const std::string& aLayer, int& aIndex )
{
    if( !aLayer.starts_with( "In" ) || !aLayer.ends_with( ".Cu" ) )
        return false;

    const std::string_view number( aLayer.data() + 2, aLayer.size() - 5 );
    const std::from_chars_result parsed =
            std::from_chars( number.data(), number.data() + number.size(), aIndex );
    return parsed.ec == std::errc() && parsed.ptr == number.data() + number.size()
           && aIndex >= 1 && aIndex <= 30;
}


bool copperLayer( const std::string& aLayer )
{
    int index = 0;
    return aLayer == "F.Cu" || aLayer == "B.Cu" || innerLayer( aLayer, index );
}


bool userLayer( const std::string& aLayer )
{
    if( !aLayer.starts_with( "User." ) )
        return false;

    const std::string_view number( aLayer.data() + 5, aLayer.size() - 5 );
    int index = 0;
    const std::from_chars_result parsed =
            std::from_chars( number.data(), number.data() + number.size(), index );
    return parsed.ec == std::errc() && parsed.ptr == number.data() + number.size()
           && index >= 1 && index <= 45;
}


bool physicalLayer( const std::string& aLayer )
{
    static const std::set<std::string> fixed = {
        "F.Cu", "B.Cu", "F.Adhes", "B.Adhes", "F.Paste", "B.Paste",
        "F.SilkS", "B.SilkS", "F.Mask", "B.Mask", "Dwgs.User",
        "Cmts.User", "Eco1.User", "Eco2.User", "Edge.Cuts", "Margin",
        "F.CrtYd", "B.CrtYd", "F.Fab", "B.Fab"
    };
    return fixed.contains( aLayer ) || copperLayer( aLayer ) || userLayer( aLayer );
}


} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_FOOTPRINT_SETTINGS_COMPILER::RESULT
DESIGN_SCRIPT_FOOTPRINT_SETTINGS_COMPILER::CompileRules(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    result.value = { { "clearanceNm", nullptr }, { "solderMaskMarginNm", nullptr },
                     { "solderPasteMarginNm", nullptr },
                     { "solderPasteMarginPpm", nullptr },
                     { "zoneConnection", "inherit" } };
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    const std::set<std::string> supported = {
        "clearance", "solder_mask_margin", "solder_paste_margin",
        "solder_paste_margin_ratio", "zone_connection"
    };
    std::map<std::string, size_t> fields;

    if( aDocument.ListHead( aNode ) != "rules" )
    {
        diagnostic( result, "invalid_authored_footprint_rules",
                    "footprint rules requires one rules form" );
        return result;
    }

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !supported.contains( head ) )
            diagnostic( result, "unknown_authored_footprint_rule",
                        "footprint rules does not support " + head );
        else if( !fields.emplace( head, child ).second )
            diagnostic( result, "duplicate_authored_footprint_rule",
                        "footprint rule " + head + " occurs more than once" );
    }

    for( const std::string& field : supported )
    {
        if( !fields.contains( field ) )
            diagnostic( result, "missing_authored_footprint_rule",
                        "footprint rules requires explicit " + field );
    }

    for( const char* field : { "clearance", "solder_mask_margin", "solder_paste_margin" } )
    {
        if( !fields.contains( field ) )
            continue;

        std::string value;
        const std::string fieldName = field;
        int64_t parsed = 0;

        if( !oneValue( aDocument, fields[field], value ) )
        {
            diagnostic( result, "invalid_authored_footprint_rule",
                        std::string( field ) + " requires inherit or one distance" );
        }
        else if( value == "inherit" )
        {
            result.value[fieldName == "clearance" ? "clearanceNm"
                         : fieldName == "solder_mask_margin"
                                 ? "solderMaskMarginNm" : "solderPasteMarginNm"] = nullptr;
        }
        else if( !distance( value, parsed )
                 || ( fieldName == "clearance" && parsed < 0 )
                 || ( fieldName != "clearance"
                      && ( parsed < -100'000'000 || parsed > 100'000'000 ) ) )
        {
            diagnostic( result, "invalid_authored_footprint_rule",
                        std::string( field ) + " has an invalid physical distance" );
        }
        else
        {
            result.value[fieldName == "clearance" ? "clearanceNm"
                         : fieldName == "solder_mask_margin"
                                 ? "solderMaskMarginNm" : "solderPasteMarginNm"] = parsed;
        }
    }

    if( fields.contains( "solder_paste_margin_ratio" ) )
    {
        std::string value;
        int64_t parsed = 0;

        if( !oneValue( aDocument, fields["solder_paste_margin_ratio"], value ) )
            diagnostic( result, "invalid_authored_footprint_rule",
                        "solder_paste_margin_ratio requires inherit or one decimal" );
        else if( value == "inherit" )
            result.value["solderPasteMarginPpm"] = nullptr;
        else if( !decimalPpm( value, parsed ) )
            diagnostic( result, "invalid_authored_footprint_rule",
                        "solder_paste_margin_ratio must be from -1 through 1" );
        else
            result.value["solderPasteMarginPpm"] = parsed;
    }

    if( fields.contains( "zone_connection" ) )
    {
        std::string value;
        static const std::set<std::string> values = {
            "inherit", "none", "thermal", "solid", "pth_thermal"
        };

        if( !oneValue( aDocument, fields["zone_connection"], value )
            || !values.contains( value ) )
        {
            diagnostic( result, "invalid_authored_footprint_rule",
                        "zone_connection must be inherit, none, thermal, solid, or pth_thermal" );
        }
        else
        {
            result.value["zoneConnection"] = value;
        }
    }

    result.ok = result.diagnostics.empty();
    return result;
}


DESIGN_SCRIPT_FOOTPRINT_SETTINGS_COMPILER::RESULT
DESIGN_SCRIPT_FOOTPRINT_SETTINGS_COMPILER::CompileStackup(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    result.value = JSON::array();
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string mode;

    if( aDocument.ListHead( aNode ) != "stackup" || node.children.size() != 3
        || !scalar( aDocument, node.children[1], mode ) || mode != "custom"
        || aDocument.ListHead( node.children[2] ) != "layers" )
    {
        diagnostic( result, "invalid_authored_footprint_stackup",
                    "custom footprint stackup requires (stackup custom (layers F.Cu ... B.Cu))" );
        return result;
    }

    const DOCUMENT::NODE& layers = aDocument.Nodes().at( node.children[2] );
    std::set<std::string> unique;

    if( layers.children.size() < 3 || layers.children.size() > 33
        || ( layers.children.size() - 1 ) % 2 != 0 )
    {
        diagnostic( result, "invalid_authored_footprint_stackup",
                    "custom footprint stackup requires 2 through 32 even copper layers" );
    }

    for( size_t index = 1; index < layers.children.size(); ++index )
    {
        std::string layer;

        if( !scalar( aDocument, layers.children[index], layer ) || !copperLayer( layer )
            || !unique.emplace( layer ).second )
        {
            diagnostic( result, "invalid_authored_footprint_stackup_layer",
                        "custom stackup layers must be unique named copper layers" );
            continue;
        }

        const size_t logicalIndex = index - 1;
        const size_t copperCount = layers.children.size() - 1;
        bool expected = logicalIndex == 0 ? layer == "F.Cu"
                        : logicalIndex + 1 == copperCount ? layer == "B.Cu" : false;

        if( logicalIndex > 0 && logicalIndex + 1 < copperCount )
        {
            int inner = 0;
            expected = innerLayer( layer, inner ) && inner == static_cast<int>( logicalIndex );
        }

        if( !expected )
            diagnostic( result, "noncontiguous_authored_footprint_stackup",
                        "custom stackup must be ordered F.Cu, contiguous InN.Cu layers, B.Cu" );

        result.value.push_back( layer );
    }

    result.ok = result.diagnostics.empty();
    return result;
}


DESIGN_SCRIPT_FOOTPRINT_SETTINGS_COMPILER::RESULT
DESIGN_SCRIPT_FOOTPRINT_SETTINGS_COMPILER::CompilePrivateLayers(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    result.value = JSON::array();
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::set<std::string> unique;

    if( aDocument.ListHead( aNode ) != "private_layers" || node.children.size() < 2
        || node.children.size() > 129 )
    {
        diagnostic( result, "invalid_authored_footprint_private_layers",
                    "private_layers requires 1 through 128 physical layer names" );
        return result;
    }

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        std::string layer;

        if( !scalar( aDocument, node.children[index], layer ) || !physicalLayer( layer )
            || !unique.emplace( layer ).second )
        {
            diagnostic( result, "invalid_authored_footprint_private_layer",
                        "private_layers requires unique named physical layers" );
        }
        else
        {
            result.value.push_back( layer );
        }
    }

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
