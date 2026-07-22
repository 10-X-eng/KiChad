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

#include "design_script_via_padstack_compiler.h"
#include "kichad_from_chars.h"

#include "design_script_footprint_custom_pad_compiler.h"

#include <algorithm>
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
using RESULT = KICHAD::DESIGN_SCRIPT_VIA_PADSTACK_COMPILER::RESULT;
using CUSTOM_COMPILER = KICHAD::DESIGN_SCRIPT_FOOTPRINT_CUSTOM_PAD_COMPILER;


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

    if( !std::isfinite( rounded )
        || std::fabs( rounded ) > static_cast<long double>( std::numeric_limits<int32_t>::max() ) )
    {
        return false;
    }

    aNanometers = static_cast<int64_t>( rounded );
    return true;
}


bool decimalPpm( const std::string& aText, int64_t& aPartsPerMillion )
{
    long double value = 0.0L;
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const std::from_chars_result converted = KICHAD::FromChars( begin, end, value );

    if( converted.ec != std::errc() || converted.ptr != end || !std::isfinite( value ) )
        return false;

    const long double ppm = std::round( value * 1'000'000.0L );

    if( ppm <= 0.0L || ppm > 500'000.0L )
        return false;

    aPartsPerMillion = static_cast<int64_t>( ppm );
    return true;
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


bool layerName( const std::string& aLayer )
{
    if( aLayer == "F.Cu" || aLayer == "B.Cu" || aLayer == "inner" )
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


JSON compileLayer( const DOCUMENT& aDocument, size_t aNode, const std::string& aMode,
                   RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string name;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], name )
        || !layerName( name )
        || ( aMode == "front_inner_back"
             && name != "F.Cu" && name != "inner" && name != "B.Cu" )
        || ( aMode == "custom" && name == "inner" ) )
    {
        diagnostic( aResult, "invalid_authored_via_padstack_layer",
                    "via padstack layer name is invalid for its mode" );
        return JSON::object();
    }

    JSON layer = {
        { "name", name }, { "shape", "" },
        { "offset", { { "xNm", 0 }, { "yNm", 0 } } },
        { "trapezoidDelta", { { "xNm", 0 }, { "yNm", 0 } } },
        { "roundrectRadiusNm", nullptr }, { "chamferRatioPpm", nullptr },
        { "chamferCorners", JSON::array() }, { "custom", nullptr }
    };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_authored_via_padstack_layer_field",
                        "via padstack layer " + name + " field " + head + " is duplicated" );
            continue;
        }

        if( head == "size" || head == "offset" || head == "trapezoid_delta" )
        {
            JSON parsed;

            if( !point( aDocument, child, parsed ) )
                diagnostic( aResult, "invalid_authored_via_padstack_geometry",
                            "via layer " + head + " requires two bounded distances" );
            else
                layer[head == "trapezoid_delta" ? "trapezoidDelta" : head] = parsed;

            continue;
        }

        if( head == "chamfer" )
        {
            if( !chamfers( aDocument, child, layer["chamferCorners"] ) )
                diagnostic( aResult, "invalid_authored_via_padstack_chamfer",
                            "via chamfer requires unique named corners" );

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
            diagnostic( aResult, "invalid_authored_via_padstack_layer_field",
                        "via padstack layer field " + head + " requires one value" );
            continue;
        }

        if( head == "shape" )
        {
            static const std::set<std::string> shapes = {
                "circle", "rect", "oval", "trapezoid", "roundrect", "chamfered_rect",
                "custom"
            };

            if( !shapes.contains( value ) )
                diagnostic( aResult, "invalid_authored_via_padstack_shape",
                            "via layer shape is not supported" );
            else
                layer["shape"] = value;
        }
        else if( head == "roundrect_radius" )
        {
            int64_t parsed = 0;

            if( !distance( value, parsed ) || parsed < 0 )
                diagnostic( aResult, "invalid_authored_via_padstack_roundrect_radius",
                            "roundrect_radius requires a non-negative distance" );
            else
                layer["roundrectRadiusNm"] = parsed;
        }
        else if( head == "chamfer_ratio" )
        {
            int64_t parsed = 0;

            if( !decimalPpm( value, parsed ) )
                diagnostic( aResult, "invalid_authored_via_padstack_chamfer_ratio",
                            "chamfer_ratio must be greater than zero and at most 0.5" );
            else
                layer["chamferRatioPpm"] = parsed;
        }
        else
        {
            diagnostic( aResult, "unknown_authored_via_padstack_layer_field",
                        "unsupported via padstack layer field " + head );
        }
    }

    if( layer["shape"].get<std::string>().empty() || !layer.contains( "size" ) )
        diagnostic( aResult, "incomplete_authored_via_padstack_layer",
                    "each via padstack layer requires shape and size" );

    if( layer.contains( "size" ) )
    {
        const int64_t width = layer["size"]["xNm"].get<int64_t>();
        const int64_t height = layer["size"]["yNm"].get<int64_t>();
        const std::string shape = layer["shape"].get<std::string>();

        if( width <= 0 || height <= 0 )
            diagnostic( aResult, "invalid_authored_via_padstack_size",
                        "via padstack sizes must be positive" );

        if( shape == "circle" && width != height )
            diagnostic( aResult, "invalid_authored_via_padstack_circle",
                        "via circle size must have equal dimensions" );

        if( shape == "roundrect" || shape == "chamfered_rect" )
        {
            if( layer["roundrectRadiusNm"].is_null()
                || ( shape == "roundrect" && layer["roundrectRadiusNm"].get<int64_t>() == 0 )
                || layer["roundrectRadiusNm"].get<int64_t>() * 2
                           > std::min( width, height ) )
            {
                diagnostic( aResult, "invalid_authored_via_padstack_roundrect_radius",
                            "via layer radius is invalid for its size" );
            }
        }
        else if( !layer["roundrectRadiusNm"].is_null() )
        {
            diagnostic( aResult, "unexpected_authored_via_padstack_roundrect_radius",
                        "roundrect_radius only applies to rounded/chamfered shapes" );
        }

        if( shape == "chamfered_rect" )
        {
            if( layer["chamferRatioPpm"].is_null() || layer["chamferCorners"].empty() )
                diagnostic( aResult, "incomplete_authored_via_padstack_chamfer",
                            "chamfered via layers require ratio and corners" );
        }
        else if( !layer["chamferRatioPpm"].is_null() || !layer["chamferCorners"].empty() )
        {
            diagnostic( aResult, "unexpected_authored_via_padstack_chamfer",
                        "chamfer fields only apply to chamfered via layers" );
        }

        if( shape == "custom" )
        {
            if( !layer["custom"].is_object() )
                diagnostic( aResult, "incomplete_authored_via_padstack_custom_layer",
                            "custom via layer requires one complete custom geometry form" );
        }
        else if( !layer["custom"].is_null() )
        {
            diagnostic( aResult, "unexpected_authored_via_padstack_custom_layer",
                        "custom geometry only applies to custom via layers" );
        }

        const int64_t deltaX = layer["trapezoidDelta"]["xNm"].get<int64_t>();
        const int64_t deltaY = layer["trapezoidDelta"]["yNm"].get<int64_t>();

        if( shape != "trapezoid" && ( deltaX != 0 || deltaY != 0 ) )
            diagnostic( aResult, "unexpected_authored_via_padstack_trapezoid_delta",
                        "trapezoid_delta only applies to trapezoid via layers" );
        else if( shape == "trapezoid"
                 && ( std::abs( deltaX ) > height || std::abs( deltaY ) > width ) )
            diagnostic( aResult, "invalid_authored_via_padstack_trapezoid_delta",
                        "via trapezoid_delta cannot invert the pad" );
    }

    return layer;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_VIA_PADSTACK_COMPILER::RESULT
DESIGN_SCRIPT_VIA_PADSTACK_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string mode;

    if( aDocument.ListHead( aNode ) != "padstack" || node.children.size() < 3
        || !scalar( aDocument, node.children[1], mode )
        || ( mode != "front_inner_back" && mode != "custom" ) )
    {
        diagnostic( result, "invalid_authored_via_padstack",
                    "via padstack requires front_inner_back or custom mode" );
        return result;
    }

    result.padstack = { { "mode", mode }, { "layers", JSON::array() } };
    std::set<std::string> names;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];

        if( aDocument.ListHead( child ) != "layer" )
        {
            diagnostic( result, "unknown_authored_via_padstack_field",
                        "via padstack contains only semantic layer entries" );
            continue;
        }

        JSON layer = compileLayer( aDocument, child, mode, result );
        const std::string name = layer.value( "name", "" );

        if( !name.empty() && !names.emplace( name ).second )
            diagnostic( result, "duplicate_authored_via_padstack_layer",
                        "via padstack layer " + name + " occurs more than once" );

        result.padstack["layers"].push_back( std::move( layer ) );
    }

    if( mode == "front_inner_back"
        && ( names.size() != 3 || !names.contains( "F.Cu" )
             || !names.contains( "inner" ) || !names.contains( "B.Cu" ) ) )
    {
        diagnostic( result, "incomplete_authored_via_padstack",
                    "front_inner_back via padstack requires F.Cu, inner, and B.Cu" );
    }
    else if( mode == "custom" && names.size() < 2 )
    {
        diagnostic( result, "incomplete_authored_via_padstack",
                    "custom via padstack requires at least two explicit copper layers" );
    }

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
