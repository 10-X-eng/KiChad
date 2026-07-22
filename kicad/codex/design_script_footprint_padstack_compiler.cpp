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

#include "design_script_footprint_padstack_compiler.h"
#include "kichad_from_chars.h"

#include "design_script_footprint_custom_pad_compiler.h"

#include <algorithm>
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
using RESULT = KICHAD::DESIGN_SCRIPT_FOOTPRINT_PADSTACK_COMPILER::RESULT;
using CUSTOM_COMPILER = KICHAD::DESIGN_SCRIPT_FOOTPRINT_CUSTOM_PAD_COMPILER;

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

    if( !std::isfinite( rounded ) || rounded < -MAX_DISTANCE_NM
        || rounded > MAX_DISTANCE_NM )
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

    if( tenths < -3600.0L || tenths > 3600.0L
        || std::fabs( tenths - degrees * 10.0L ) > 0.000001L )
    {
        return false;
    }

    aTenths = static_cast<int64_t>( tenths );
    return true;
}


bool decimalPpm( const std::string& aText, int64_t aMinimum, int64_t aMaximum,
                 int64_t& aPartsPerMillion )
{
    long double value = 0.0L;
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const std::from_chars_result converted = KICHAD::FromChars( begin, end, value );

    if( converted.ec != std::errc() || converted.ptr != end || !std::isfinite( value ) )
        return false;

    const long double ppm = std::round( value * 1'000'000.0L );

    if( ppm < static_cast<long double>( aMinimum )
        || ppm > static_cast<long double>( aMaximum ) )
    {
        return false;
    }

    aPartsPerMillion = static_cast<int64_t>( ppm );
    return true;
}


bool oneValue( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalar( aDocument, node.children[1], aValue );
}


bool point( const DOCUMENT& aDocument, size_t aNode, JSON& aPoint )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string xText;
    std::string yText;
    int64_t x = 0;
    int64_t y = 0;

    if( node.children.size() != 3 || !scalar( aDocument, node.children[1], xText )
        || !scalar( aDocument, node.children[2], yText )
        || !distance( xText, x ) || !distance( yText, y ) )
    {
        return false;
    }

    aPoint = { { "xNm", x }, { "yNm", y } };
    return true;
}


bool chamfers( const DOCUMENT& aDocument, size_t aNode, JSON& aCorners )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    static const std::set<std::string> allowed = {
        "top_left", "top_right", "bottom_left", "bottom_right"
    };
    std::set<std::string> unique;

    if( node.children.size() < 2 || node.children.size() > 5 )
        return false;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        std::string corner;

        if( !scalar( aDocument, node.children[index], corner ) || !allowed.contains( corner )
            || !unique.emplace( corner ).second )
        {
            return false;
        }

        aCorners.push_back( corner );
    }

    return true;
}


bool customLayerName( const std::string& aLayer )
{
    if( aLayer == "B.Cu" )
        return true;

    if( !aLayer.starts_with( "In" ) || !aLayer.ends_with( ".Cu" ) )
        return false;

    const std::string_view number( aLayer.data() + 2, aLayer.size() - 5 );
    int index = 0;
    const std::from_chars_result parsed =
            std::from_chars( number.data(), number.data() + number.size(), index );
    return parsed.ec == std::errc() && parsed.ptr == number.data() + number.size()
           && index >= 1 && index <= 30;
}


JSON compileLayer( const DOCUMENT& aDocument, size_t aNode, const std::string& aMode,
                   RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string name;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], name )
        || ( aMode == "front_inner_back" && name != "inner" && name != "B.Cu" )
        || ( aMode == "custom" && !customLayerName( name ) ) )
    {
        diagnostic( aResult, "invalid_authored_footprint_padstack_layer",
                    aMode == "front_inner_back"
                            ? "front_inner_back layers must be inner or B.Cu"
                            : "custom padstack layers must be In1.Cu through In30.Cu or B.Cu" );
        return JSON::object();
    }

    JSON layer = {
        { "name", name }, { "shape", "" },
        { "offset", { { "xNm", 0 }, { "yNm", 0 } } },
        { "trapezoidDelta", { { "xNm", 0 }, { "yNm", 0 } } },
        { "roundrectRadiusNm", nullptr }, { "chamferRatioPpm", nullptr },
        { "chamferCorners", JSON::array() }, { "custom", nullptr },
        { "clearanceNm", nullptr },
        { "zoneConnection", "inherit" }, { "thermalSpokeWidthNm", nullptr },
        { "thermalGapNm", nullptr }, { "thermalSpokeAngleTenths", nullptr }
    };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_authored_footprint_padstack_layer_field",
                        "padstack layer " + name + " field " + head + " occurs more than once" );
            continue;
        }

        if( head == "size" || head == "offset" || head == "trapezoid_delta" )
        {
            JSON parsed;

            if( !point( aDocument, child, parsed ) )
                diagnostic( aResult, "invalid_authored_footprint_padstack_geometry",
                            "padstack layer " + name + " " + head
                                    + " requires two bounded distances" );
            else
                layer[head == "trapezoid_delta" ? "trapezoidDelta" : head] = parsed;

            continue;
        }

        if( head == "chamfer" )
        {
            if( !chamfers( aDocument, child, layer["chamferCorners"] ) )
                diagnostic( aResult, "invalid_authored_footprint_padstack_chamfer",
                            "padstack chamfer requires one through four unique corner names" );

            continue;
        }

        if( head == "custom" )
        {
            CUSTOM_COMPILER::RESULT custom = CUSTOM_COMPILER::CompileLayer( aDocument, child );

            for( JSON& entry : custom.diagnostics )
                aResult.diagnostics.push_back( std::move( entry ) );

            layer["custom"] = std::move( custom.custom );
            continue;
        }

        std::string value;

        if( !oneValue( aDocument, child, value ) )
        {
            diagnostic( aResult, "invalid_authored_footprint_padstack_layer_field",
                        "padstack layer " + name + " field " + head + " requires one value" );
            continue;
        }

        if( head == "shape" )
        {
            static const std::set<std::string> shapes = {
                "circle", "rect", "oval", "trapezoid", "roundrect", "chamfered_rect",
                "custom"
            };

            if( !shapes.contains( value ) )
                diagnostic( aResult, "invalid_authored_footprint_padstack_shape",
                            "per-layer shape must be circle, rect, oval, trapezoid, roundrect, "
                            "chamfered_rect, or custom" );
            else
                layer["shape"] = value;
        }
        else if( head == "roundrect_radius" || head == "clearance"
                 || head == "thermal_spoke_width" || head == "thermal_gap" )
        {
            const std::string key = head == "roundrect_radius" ? "roundrectRadiusNm"
                                    : head == "clearance" ? "clearanceNm"
                                    : head == "thermal_spoke_width" ? "thermalSpokeWidthNm"
                                                                       : "thermalGapNm";

            if( value == "inherit" && head != "roundrect_radius" )
            {
                layer[key] = nullptr;
            }
            else
            {
                int64_t parsed = 0;

                if( !distance( value, parsed ) || parsed < 0 )
                    diagnostic( aResult, "invalid_authored_footprint_padstack_distance",
                                head + " requires a non-negative bounded distance"
                                + ( head == "roundrect_radius" ? "" : " or inherit" ) );
                else
                    layer[key] = parsed;
            }
        }
        else if( head == "chamfer_ratio" )
        {
            int64_t parsed = 0;

            if( !decimalPpm( value, 1, 500'000, parsed ) )
                diagnostic( aResult, "invalid_authored_footprint_padstack_chamfer_ratio",
                            "padstack chamfer_ratio must be greater than zero and at most 0.5" );
            else
                layer["chamferRatioPpm"] = parsed;
        }
        else if( head == "thermal_spoke_angle" )
        {
            int64_t parsed = 0;

            if( value == "inherit" )
                layer["thermalSpokeAngleTenths"] = nullptr;
            else if( !angle( value, parsed ) )
                diagnostic( aResult, "invalid_authored_footprint_padstack_thermal_angle",
                            "thermal_spoke_angle requires inherit or exact 0.1-degree units" );
            else
                layer["thermalSpokeAngleTenths"] = parsed;
        }
        else if( head == "zone_connection" )
        {
            static const std::set<std::string> connections = {
                "inherit", "none", "thermal", "solid", "tht_thermal"
            };

            if( !connections.contains( value ) )
                diagnostic( aResult, "invalid_authored_footprint_padstack_zone_connection",
                            "zone_connection must be inherit, none, thermal, solid, or tht_thermal" );
            else
                layer["zoneConnection"] = value;
        }
        else
        {
            diagnostic( aResult, "unknown_authored_footprint_padstack_layer_field",
                        "unsupported padstack layer field " + head );
        }
    }

    if( layer["shape"].get<std::string>().empty() || !layer.contains( "size" ) )
        diagnostic( aResult, "incomplete_authored_footprint_padstack_layer",
                    "padstack layer " + name + " requires shape and size" );

    if( layer.contains( "size" ) )
    {
        const int64_t width = layer["size"]["xNm"].get<int64_t>();
        const int64_t height = layer["size"]["yNm"].get<int64_t>();
        const std::string shape = layer["shape"].get<std::string>();

        if( width <= 0 || height <= 0 )
            diagnostic( aResult, "invalid_authored_footprint_padstack_size",
                        "padstack layer size must be positive" );

        if( shape == "circle" && width != height )
            diagnostic( aResult, "invalid_authored_footprint_padstack_circle",
                        "per-layer circle size must have equal width and height" );

        if( shape == "roundrect" || shape == "chamfered_rect" )
        {
            if( layer["roundrectRadiusNm"].is_null()
                || ( shape == "roundrect" && layer["roundrectRadiusNm"].get<int64_t>() == 0 )
                || layer["roundrectRadiusNm"].get<int64_t>() * 2
                           > std::min( width, height ) )
            {
                diagnostic( aResult, "invalid_authored_footprint_padstack_roundrect_radius",
                            "rounded/chamfered per-layer radius is invalid for its size" );
            }
        }
        else if( !layer["roundrectRadiusNm"].is_null() )
        {
            diagnostic( aResult, "unexpected_authored_footprint_padstack_roundrect_radius",
                        "roundrect_radius only applies to rounded/chamfered per-layer shapes" );
        }

        if( shape == "chamfered_rect" )
        {
            if( layer["chamferRatioPpm"].is_null() || layer["chamferCorners"].empty() )
                diagnostic( aResult, "incomplete_authored_footprint_padstack_chamfer",
                            "chamfered_rect layer requires chamfer_ratio and corners" );
        }
        else if( !layer["chamferRatioPpm"].is_null() || !layer["chamferCorners"].empty() )
        {
            diagnostic( aResult, "unexpected_authored_footprint_padstack_chamfer",
                        "per-layer chamfer fields only apply to chamfered_rect" );
        }

        if( shape == "custom" )
        {
            if( !layer["custom"].is_object() )
                diagnostic( aResult, "incomplete_authored_footprint_padstack_custom_layer",
                            "custom padstack layer requires one complete custom geometry form" );
        }
        else if( !layer["custom"].is_null() )
        {
            diagnostic( aResult, "unexpected_authored_footprint_padstack_custom_layer",
                        "custom geometry only applies to custom padstack layers" );
        }

        const int64_t deltaX = layer["trapezoidDelta"]["xNm"].get<int64_t>();
        const int64_t deltaY = layer["trapezoidDelta"]["yNm"].get<int64_t>();

        if( shape != "trapezoid" && ( deltaX != 0 || deltaY != 0 ) )
            diagnostic( aResult, "unexpected_authored_footprint_padstack_trapezoid_delta",
                        "trapezoid_delta only applies to per-layer trapezoids" );
        else if( shape == "trapezoid"
                 && ( std::abs( deltaX ) > height || std::abs( deltaY ) > width ) )
            diagnostic( aResult, "invalid_authored_footprint_padstack_trapezoid_delta",
                        "per-layer trapezoid_delta cannot invert the pad" );
    }

    return layer;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_FOOTPRINT_PADSTACK_COMPILER::RESULT
DESIGN_SCRIPT_FOOTPRINT_PADSTACK_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string mode;

    if( aDocument.ListHead( aNode ) != "padstack" || node.children.size() < 3
        || !scalar( aDocument, node.children[1], mode )
        || ( mode != "front_inner_back" && mode != "custom" ) )
    {
        diagnostic( result, "invalid_authored_footprint_padstack",
                    "padstack requires front_inner_back or custom mode and layer entries" );
        return result;
    }

    result.padstack = { { "mode", mode }, { "layers", JSON::array() } };
    std::set<std::string> names;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];

        if( aDocument.ListHead( child ) != "layer" )
        {
            diagnostic( result, "unknown_authored_footprint_padstack_field",
                        "padstack contains only semantic layer entries" );
            continue;
        }

        JSON layer = compileLayer( aDocument, child, mode, result );
        const std::string name = layer.value( "name", "" );

        if( !name.empty() && !names.emplace( name ).second )
            diagnostic( result, "duplicate_authored_footprint_padstack_layer",
                        "padstack layer " + name + " occurs more than once" );

        result.padstack["layers"].push_back( std::move( layer ) );
    }

    if( mode == "front_inner_back"
        && ( names.size() != 2 || !names.contains( "inner" ) || !names.contains( "B.Cu" ) ) )
    {
        diagnostic( result, "incomplete_authored_footprint_padstack",
                    "front_inner_back requires exactly inner and B.Cu layer definitions" );
    }
    else if( mode == "custom" && names.empty() )
    {
        diagnostic( result, "incomplete_authored_footprint_padstack",
                    "custom padstack requires at least one explicit copper-layer override" );
    }

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
