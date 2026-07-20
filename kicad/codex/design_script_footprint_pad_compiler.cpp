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

#include "design_script_footprint_pad_compiler.h"

#include <algorithm>
#include <cctype>
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
using RESULT = KICHAD::DESIGN_SCRIPT_FOOTPRINT_PAD_COMPILER::RESULT;

constexpr int64_t MAX_COORDINATE_NM = 2'000'000'000LL;
constexpr int64_t MAX_PAD_DIMENSION_NM = 1'000'000'000LL;


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
    if( aValue.empty() || aValue.size() > 128 )
        return false;

    for( unsigned char character : aValue )
    {
        if( !( std::isalnum( character ) || character == '_' || character == '-'
               || character == '.' || character == '+' ) )
        {
            return false;
        }
    }

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

    if( !std::isfinite( rounded ) || rounded < -MAX_COORDINATE_NM
        || rounded > MAX_COORDINATE_NM )
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
            std::from_chars( number.data(), number.data() + number.size(), degrees );

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
    const std::from_chars_result converted = std::from_chars( begin, end, value );

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

    if( node.kind != DOCUMENT::NODE_KIND::LIST || node.children.size() != 3
        || !scalar( aDocument, node.children[1], xText )
        || !scalar( aDocument, node.children[2], yText )
        || !distance( xText, x ) || !distance( yText, y ) )
    {
        return false;
    }

    aPoint = { { "xNm", x }, { "yNm", y } };
    return true;
}


bool nullableDistance( const DOCUMENT& aDocument, size_t aNode, JSON& aValue,
                       bool aNonNegative )
{
    std::string text;
    int64_t parsed = 0;

    if( !oneValue( aDocument, aNode, text ) )
        return false;

    if( text == "inherit" )
    {
        aValue = nullptr;
        return true;
    }

    if( !distance( text, parsed ) || ( aNonNegative && parsed < 0 )
        || parsed < -100'000'000 || parsed > 100'000'000 )
    {
        return false;
    }

    aValue = parsed;
    return true;
}


bool nullableDecimal( const DOCUMENT& aDocument, size_t aNode, JSON& aValue )
{
    std::string text;
    int64_t parsed = 0;

    if( !oneValue( aDocument, aNode, text ) )
        return false;

    if( text == "inherit" )
    {
        aValue = nullptr;
        return true;
    }

    if( !decimalPpm( text, -1'000'000, 1'000'000, parsed ) )
        return false;

    aValue = parsed;
    return true;
}


bool compileLayers( const DOCUMENT& aDocument, size_t aNode, JSON& aLayers )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    static const std::set<std::string> allowed = {
        "all_copper", "all_mask", "F.Cu", "B.Cu", "F.Mask", "B.Mask",
        "F.Paste", "B.Paste", "F.Adhes", "B.Adhes"
    };
    std::set<std::string> unique;

    if( node.children.size() < 2 || node.children.size() > 17 )
        return false;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        std::string layer;

        if( !scalar( aDocument, node.children[index], layer ) || !allowed.contains( layer )
            || !unique.emplace( layer ).second )
        {
            return false;
        }

        aLayers.push_back( layer );
    }

    if( unique.contains( "all_copper" )
        && ( unique.contains( "F.Cu" ) || unique.contains( "B.Cu" ) ) )
    {
        return false;
    }

    if( unique.contains( "all_mask" )
        && ( unique.contains( "F.Mask" ) || unique.contains( "B.Mask" ) ) )
    {
        return false;
    }

    return true;
}


bool compileDrill( const DOCUMENT& aDocument, size_t aNode, JSON& aDrill )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string shape;
    std::string xText;
    std::string yText;
    int64_t x = 0;
    int64_t y = 0;

    if( node.children.size() < 3 || node.children.size() > 4
        || !scalar( aDocument, node.children[1], shape )
        || !scalar( aDocument, node.children[2], xText ) || !distance( xText, x )
        || x <= 0 || x > MAX_PAD_DIMENSION_NM )
    {
        return false;
    }

    if( shape == "round" && node.children.size() == 3 )
    {
        y = x;
    }
    else if( shape == "oval" && node.children.size() == 4
             && scalar( aDocument, node.children[3], yText ) && distance( yText, y )
             && y > 0 && y <= MAX_PAD_DIMENSION_NM )
    {
        // Parsed above.
    }
    else
    {
        return false;
    }

    aDrill = { { "shape", shape }, { "xNm", x }, { "yNm", y } };
    return true;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_FOOTPRINT_PAD_COMPILER::RESULT
DESIGN_SCRIPT_FOOTPRINT_PAD_COMPILER::Compile( const LOSSLESS_SEXPR_DOCUMENT& aDocument,
                                               size_t aNode )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], id )
        || !identifier( id ) )
    {
        diagnostic( result, "invalid_authored_footprint_pad_id",
                    "footprint pad requires one unique bounded logical ID" );
        return result;
    }

    result.pad = {
        { "id", id }, { "number", "" }, { "type", "" }, { "shape", "" },
        { "rotationTenths", 0 }, { "layers", JSON::array() }, { "drill", nullptr },
        { "shapeOffset", { { "xNm", 0 }, { "yNm", 0 } } },
        { "trapezoidDelta", { { "xNm", 0 }, { "yNm", 0 } } },
        { "roundrectRadiusNm", nullptr }, { "property", "none" },
        { "pinFunction", "" }, { "pinType", "" }, { "dieLengthNm", 0 },
        { "solderMaskMarginNm", nullptr }, { "solderPasteMarginNm", nullptr },
        { "solderPasteMarginPpm", nullptr }, { "clearanceNm", nullptr },
        { "zoneConnection", "inherit" }, { "thermalSpokeWidthNm", nullptr },
        { "thermalSpokeAngleTenths", nullptr }, { "thermalGapNm", nullptr },
        { "removeUnusedLayers", false }, { "keepEndLayers", false }
    };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !fields.emplace( head ).second )
        {
            diagnostic( result, "duplicate_authored_footprint_pad_field",
                        "footprint pad " + id + " field " + head + " occurs more than once" );
            continue;
        }

        if( head == "at" || head == "size" || head == "shape_offset"
            || head == "trapezoid_delta" )
        {
            JSON parsed;

            if( !point( aDocument, child, parsed ) )
            {
                diagnostic( result, "invalid_authored_footprint_pad_geometry",
                            "footprint pad " + id + " " + head
                                    + " requires two bounded physical coordinates" );
            }
            else
            {
                const std::string key = head == "shape_offset" ? "shapeOffset"
                                        : head == "trapezoid_delta" ? "trapezoidDelta"
                                                                      : head;
                result.pad[key] = std::move( parsed );
            }

            continue;
        }

        if( head == "layers" )
        {
            if( !compileLayers( aDocument, child, result.pad["layers"] ) )
                diagnostic( result, "invalid_authored_footprint_pad_layers",
                            "pad layers require unique supported physical or all_copper/all_mask "
                            "tokens" );

            continue;
        }

        if( head == "drill" )
        {
            if( !compileDrill( aDocument, child, result.pad["drill"] ) )
                diagnostic( result, "invalid_authored_footprint_pad_drill",
                            "drill requires round DIAMETER or oval WIDTH HEIGHT" );

            continue;
        }

        if( head == "solder_mask_margin" || head == "solder_paste_margin"
            || head == "clearance" || head == "thermal_spoke_width" || head == "thermal_gap" )
        {
            const bool nonNegative = head == "clearance" || head == "thermal_spoke_width"
                                     || head == "thermal_gap";
            JSON parsed;

            if( !nullableDistance( aDocument, child, parsed, nonNegative ) )
                diagnostic( result, "invalid_authored_footprint_pad_override",
                            head + " requires inherit or a bounded physical distance" );
            else
                result.pad[head == "solder_mask_margin" ? "solderMaskMarginNm"
                           : head == "solder_paste_margin" ? "solderPasteMarginNm"
                           : head == "clearance" ? "clearanceNm"
                           : head == "thermal_spoke_width" ? "thermalSpokeWidthNm"
                                                             : "thermalGapNm"] = parsed;

            continue;
        }

        if( head == "solder_paste_margin_ratio" )
        {
            JSON parsed;

            if( !nullableDecimal( aDocument, child, parsed ) )
                diagnostic( result, "invalid_authored_footprint_pad_paste_ratio",
                            "solder_paste_margin_ratio requires inherit or -1 through 1" );
            else
                result.pad["solderPasteMarginPpm"] = parsed;

            continue;
        }

        std::string value;

        if( !oneValue( aDocument, child, value ) )
        {
            diagnostic( result, "invalid_authored_footprint_pad_field",
                        "footprint pad " + id + " field " + head + " requires one value" );
            continue;
        }

        if( head == "number" )
        {
            if( value.size() > 64 || value.find( '\0' ) != std::string::npos )
                diagnostic( result, "invalid_authored_footprint_pad_number",
                            "pad number must contain at most 64 UTF-8 bytes" );
            else
                result.pad["number"] = value;
        }
        else if( head == "type" )
        {
            static const std::set<std::string> types = {
                "smd", "thru_hole", "connect", "np_thru_hole"
            };

            if( !types.contains( value ) )
                diagnostic( result, "invalid_authored_footprint_pad_type",
                            "pad type must be smd, thru_hole, connect, or np_thru_hole" );
            else
                result.pad["type"] = value;
        }
        else if( head == "shape" )
        {
            static const std::set<std::string> shapes = {
                "circle", "rect", "oval", "trapezoid", "roundrect"
            };

            if( !shapes.contains( value ) )
                diagnostic( result, "invalid_authored_footprint_pad_shape",
                            "standard pad shape must be circle, rect, oval, trapezoid, or "
                            "roundrect" );
            else
                result.pad["shape"] = value;
        }
        else if( head == "rotation" )
        {
            int64_t parsed = 0;

            if( !angle( value, parsed ) )
                diagnostic( result, "invalid_authored_footprint_pad_rotation",
                            "pad rotation requires exact 0.1-degree units within +/-360 degrees" );
            else
                result.pad["rotationTenths"] = parsed;
        }
        else if( head == "roundrect_radius" )
        {
            int64_t parsed = 0;

            if( !distance( value, parsed ) || parsed <= 0
                || parsed > MAX_PAD_DIMENSION_NM )
            {
                diagnostic( result, "invalid_authored_footprint_pad_roundrect_radius",
                            "roundrect_radius requires a positive bounded distance" );
            }
            else
            {
                result.pad["roundrectRadiusNm"] = parsed;
            }
        }
        else if( head == "property" )
        {
            static const std::set<std::string> properties = {
                "none", "bga", "global_fiducial", "local_fiducial", "testpoint",
                "heatsink", "castellated", "mechanical", "pressfit"
            };

            if( !properties.contains( value ) )
                diagnostic( result, "invalid_authored_footprint_pad_property",
                            "pad property is not a supported KiCad 10 property" );
            else
                result.pad["property"] = value;
        }
        else if( head == "pin_function" || head == "pin_type" )
        {
            if( value.size() > 256 || value.find( '\0' ) != std::string::npos )
                diagnostic( result, "invalid_authored_footprint_pad_pin_metadata",
                            head + " must contain at most 256 UTF-8 bytes" );
            else
                result.pad[head == "pin_function" ? "pinFunction" : "pinType"] = value;
        }
        else if( head == "die_length" )
        {
            int64_t parsed = 0;

            if( !distance( value, parsed ) || parsed < 0 || parsed > MAX_PAD_DIMENSION_NM )
                diagnostic( result, "invalid_authored_footprint_pad_die_length",
                            "die_length requires a non-negative bounded distance" );
            else
                result.pad["dieLengthNm"] = parsed;
        }
        else if( head == "zone_connection" )
        {
            static const std::set<std::string> connections = {
                "inherit", "none", "thermal", "solid", "tht_thermal"
            };

            if( !connections.contains( value ) )
                diagnostic( result, "invalid_authored_footprint_pad_zone_connection",
                            "zone_connection must be inherit, none, thermal, solid, or "
                            "tht_thermal" );
            else
                result.pad["zoneConnection"] = value;
        }
        else if( head == "thermal_spoke_angle" )
        {
            if( value == "inherit" )
            {
                result.pad["thermalSpokeAngleTenths"] = nullptr;
            }
            else
            {
                int64_t parsed = 0;

                if( !angle( value, parsed ) )
                    diagnostic( result, "invalid_authored_footprint_pad_thermal_angle",
                                "thermal_spoke_angle requires inherit or exact 0.1-degree units" );
                else
                    result.pad["thermalSpokeAngleTenths"] = parsed;
            }
        }
        else if( head == "remove_unused_layers" || head == "keep_end_layers" )
        {
            bool parsed = false;

            if( !boolean( value, parsed ) )
                diagnostic( result, "invalid_authored_footprint_pad_boolean",
                            head + " must be true or false" );
            else
                result.pad[head == "remove_unused_layers" ? "removeUnusedLayers"
                                                             : "keepEndLayers"] = parsed;
        }
        else
        {
            diagnostic( result, "unknown_authored_footprint_pad_field",
                        "unsupported footprint pad field " + head );
        }
    }

    const std::string type = result.pad["type"].get<std::string>();
    const std::string shape = result.pad["shape"].get<std::string>();

    if( type.empty() || shape.empty() || !result.pad.contains( "at" )
        || !result.pad.contains( "size" ) || result.pad["layers"].empty() )
    {
        diagnostic( result, "incomplete_authored_footprint_pad",
                    "pad " + id + " requires type, shape, at, size, and layers" );
    }

    if( result.pad.contains( "size" ) )
    {
        const int64_t width = result.pad["size"]["xNm"].get<int64_t>();
        const int64_t height = result.pad["size"]["yNm"].get<int64_t>();

        if( width <= 0 || height <= 0 || width > MAX_PAD_DIMENSION_NM
            || height > MAX_PAD_DIMENSION_NM )
        {
            diagnostic( result, "invalid_authored_footprint_pad_size",
                        "pad " + id + " size must be positive and no greater than 1 metre" );
        }

        if( shape == "circle" && width != height )
        {
            diagnostic( result, "invalid_authored_footprint_pad_circle_size",
                        "circle pads require equal width and height" );
        }

        if( shape == "roundrect" )
        {
            if( result.pad["roundrectRadiusNm"].is_null()
                || result.pad["roundrectRadiusNm"].get<int64_t>() * 2
                           > std::min( width, height ) )
            {
                diagnostic( result, "invalid_authored_footprint_pad_roundrect_radius",
                            "roundrect radius must be positive and no more than half the "
                            "shorter pad side" );
            }
        }
        else if( !result.pad["roundrectRadiusNm"].is_null() )
        {
            diagnostic( result, "unexpected_authored_footprint_pad_roundrect_radius",
                        "roundrect_radius is only valid for roundrect pads" );
        }

        const int64_t deltaX = result.pad["trapezoidDelta"]["xNm"].get<int64_t>();
        const int64_t deltaY = result.pad["trapezoidDelta"]["yNm"].get<int64_t>();

        if( shape != "trapezoid" && ( deltaX != 0 || deltaY != 0 ) )
            diagnostic( result, "unexpected_authored_footprint_pad_trapezoid_delta",
                        "trapezoid_delta is only valid for trapezoid pads" );
        else if( shape == "trapezoid"
                 && ( std::abs( deltaX ) > height || std::abs( deltaY ) > width ) )
            diagnostic( result, "invalid_authored_footprint_pad_trapezoid_delta",
                        "trapezoid_delta cannot invert the pad" );
    }

    const bool drilled = type == "thru_hole" || type == "np_thru_hole";

    if( drilled != result.pad["drill"].is_object() )
        diagnostic( result, "invalid_authored_footprint_pad_drill_semantics",
                    drilled ? "through-hole pads require a drill"
                            : "smd and connect pads cannot declare a drill" );

    if( drilled && result.pad["drill"].is_object() && result.pad.contains( "size" )
        && ( result.pad["drill"]["xNm"].get<int64_t>()
                     > result.pad["size"]["xNm"].get<int64_t>()
             || result.pad["drill"]["yNm"].get<int64_t>()
                        > result.pad["size"]["yNm"].get<int64_t>() ) )
    {
        diagnostic( result, "invalid_authored_footprint_pad_drill_size",
                    "drill dimensions cannot exceed the pad dimensions" );
    }

    if( type == "np_thru_hole" && !result.pad["number"].get_ref<const std::string&>().empty() )
        diagnostic( result, "numbered_authored_footprint_npth_pad",
                    "non-plated through-hole pads cannot have an electrical pad number" );

    std::set<std::string> layers;

    for( const JSON& layer : result.pad["layers"] )
        layers.emplace( layer.get<std::string>() );

    if( drilled && !layers.contains( "all_copper" ) )
        diagnostic( result, "invalid_authored_footprint_through_hole_layers",
                    "through-hole pads require all_copper in their layer set" );

    if( ( type == "smd" || type == "connect" )
        && !layers.contains( "F.Cu" ) && !layers.contains( "B.Cu" ) )
    {
        diagnostic( result, "invalid_authored_footprint_surface_pad_layers",
                    "smd and connect pads require F.Cu or B.Cu" );
    }

    if( result.pad["keepEndLayers"].get<bool>()
        && !result.pad["removeUnusedLayers"].get<bool>() )
    {
        diagnostic( result, "invalid_authored_footprint_pad_keep_end_layers",
                    "keep_end_layers requires remove_unused_layers true" );
    }

    if( result.pad["removeUnusedLayers"].get<bool>() && type != "thru_hole" )
        diagnostic( result, "invalid_authored_footprint_pad_remove_unused_layers",
                    "remove_unused_layers applies only to plated through-hole pads" );

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
