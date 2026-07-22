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

#include "design_script_footprint_graphic_compiler.h"
#include "kichad_from_chars.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>
#include <utility>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_FOOTPRINT_GRAPHIC_COMPILER::RESULT;

constexpr int64_t MAX_COORDINATE_NM = 2'000'000'000LL;
constexpr int64_t MAX_DIMENSION_NM = 1'000'000'000LL;
constexpr size_t MAX_POLYGON_POINTS = 4096;


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

    if( !std::isfinite( rounded ) || rounded < -MAX_COORDINATE_NM
        || rounded > MAX_COORDINATE_NM )
    {
        return false;
    }

    aNanometers = static_cast<int64_t>( rounded );
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


bool samePoint( const JSON& aLeft, const JSON& aRight )
{
    return aLeft["xNm"] == aRight["xNm"] && aLeft["yNm"] == aRight["yNm"];
}


bool compileStroke( const DOCUMENT& aDocument, size_t aNode, JSON& aStroke )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string widthText;
    std::string style;
    int64_t width = 0;
    static const std::set<std::string> styles = {
        "solid", "dash", "dash_dot", "dash_dot_dot", "dot"
    };

    if( node.children.size() != 3 || !scalar( aDocument, node.children[1], widthText )
        || !distance( widthText, width ) || width < 0 || width > MAX_DIMENSION_NM
        || !scalar( aDocument, node.children[2], style ) || !styles.contains( style ) )
    {
        return false;
    }

    aStroke = { { "widthNm", width }, { "style", style } };
    return true;
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


bool innerCopperLayer( const std::string& aLayer )
{
    if( !aLayer.starts_with( "In" ) || !aLayer.ends_with( ".Cu" ) )
        return false;

    const std::string_view number( aLayer.data() + 2, aLayer.size() - 5 );
    int index = 0;
    const std::from_chars_result parsed =
            std::from_chars( number.data(), number.data() + number.size(), index );
    return parsed.ec == std::errc() && parsed.ptr == number.data() + number.size()
           && index >= 1 && index <= 30;
}


bool compileLayers( const DOCUMENT& aDocument, size_t aNode, JSON& aLayers )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    static const std::set<std::string> fixed = {
        "F.Cu", "B.Cu", "F.Adhes", "B.Adhes", "F.Paste", "B.Paste",
        "F.SilkS", "B.SilkS", "F.Mask", "B.Mask", "Dwgs.User",
        "Cmts.User", "Eco1.User", "Eco2.User", "Edge.Cuts", "Margin",
        "F.CrtYd", "B.CrtYd", "F.Fab", "B.Fab"
    };
    std::set<std::string> unique;

    if( node.children.size() < 2 || node.children.size() > 33 )
        return false;

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        std::string layer;

        if( !scalar( aDocument, node.children[index], layer )
            || ( !fixed.contains( layer ) && !userLayer( layer )
                 && !innerCopperLayer( layer ) )
            || !unique.emplace( layer ).second )
        {
            return false;
        }

        aLayers.push_back( layer );
    }

    return true;
}


bool compileNullableMargin( const DOCUMENT& aDocument, size_t aNode, JSON& aMargin )
{
    std::string value;
    int64_t parsed = 0;

    if( !oneValue( aDocument, aNode, value ) )
        return false;

    if( value == "inherit" )
    {
        aMargin = nullptr;
        return true;
    }

    if( !distance( value, parsed ) || parsed < -100'000'000 || parsed > 100'000'000 )
        return false;

    aMargin = parsed;
    return true;
}


long double signedArea2( const JSON& aPoints )
{
    long double area = 0.0L;

    for( size_t index = 0; index < aPoints.size(); ++index )
    {
        const JSON& current = aPoints[index];
        const JSON& next = aPoints[( index + 1 ) % aPoints.size()];
        area += static_cast<long double>( current["xNm"].get<int64_t>() )
                        * static_cast<long double>( next["yNm"].get<int64_t>() )
                - static_cast<long double>( next["xNm"].get<int64_t>() )
                        * static_cast<long double>( current["yNm"].get<int64_t>() );
    }

    return area;
}

} // namespace


namespace KICHAD
{

bool DESIGN_SCRIPT_FOOTPRINT_GRAPHIC_COMPILER::IsGraphicHead( const std::string& aHead )
{
    static const std::set<std::string> heads = {
        "line", "rectangle", "arc", "circle", "polygon", "bezier"
    };
    return heads.contains( aHead );
}


DESIGN_SCRIPT_FOOTPRINT_GRAPHIC_COMPILER::RESULT
DESIGN_SCRIPT_FOOTPRINT_GRAPHIC_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    const std::string kind = aDocument.ListHead( aNode );
    std::string id;

    if( !IsGraphicHead( kind ) || node.children.size() < 2
        || !scalar( aDocument, node.children[1], id ) || !identifier( id ) )
    {
        diagnostic( result, "invalid_authored_footprint_graphic_id",
                    "footprint graphic requires a supported primitive and unique bounded ID" );
        return result;
    }

    result.graphic = {
        { "id", id }, { "kind", kind }, { "layers", JSON::array() },
        { "stroke", JSON::object() }, { "fill", "none" }, { "locked", false },
        { "solderMaskMarginNm", nullptr }, { "net", "" }, { "radiusNm", 0 },
        { "points", JSON::array() }
    };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "point" && kind == "polygon" )
        {
            if( result.graphic["points"].size() >= MAX_POLYGON_POINTS )
            {
                diagnostic( result, "too_many_authored_footprint_graphic_points",
                            "a footprint polygon may contain at most 4096 points" );
                continue;
            }

            JSON parsed;

            if( !point( aDocument, child, parsed ) )
                diagnostic( result, "invalid_authored_footprint_graphic_point",
                            "polygon point requires two bounded physical coordinates" );
            else
                result.graphic["points"].push_back( std::move( parsed ) );

            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( result, "duplicate_authored_footprint_graphic_field",
                        "footprint graphic " + id + " field " + head + " occurs more than once" );
            continue;
        }

        if( head == "start" || head == "mid" || head == "end" || head == "center"
            || head == "control1" || head == "control2" )
        {
            JSON parsed;

            if( !point( aDocument, child, parsed ) )
                diagnostic( result, "invalid_authored_footprint_graphic_geometry",
                            "footprint graphic " + id + " " + head
                                    + " requires two bounded physical coordinates" );
            else
                result.graphic[head] = std::move( parsed );

            continue;
        }

        if( head == "stroke" )
        {
            if( !compileStroke( aDocument, child, result.graphic["stroke"] ) )
                diagnostic( result, "invalid_authored_footprint_graphic_stroke",
                            "graphic stroke requires WIDTH and a supported line style" );

            continue;
        }

        if( head == "layers" )
        {
            if( !compileLayers( aDocument, child, result.graphic["layers"] ) )
                diagnostic( result, "invalid_authored_footprint_graphic_layers",
                            "graphic layers require unique supported KiCad footprint layers" );

            continue;
        }

        if( head == "solder_mask_margin" )
        {
            if( !compileNullableMargin( aDocument, child,
                                        result.graphic["solderMaskMarginNm"] ) )
            {
                diagnostic( result, "invalid_authored_footprint_graphic_mask_margin",
                            "solder mask margin requires inherit or a bounded distance" );
            }

            continue;
        }

        if( head == "net" )
        {
            std::string net;

            if( !oneValue( aDocument, child, net ) || net.size() > 128 )
                diagnostic( result, "invalid_authored_footprint_graphic_net",
                            "graphic net requires none or one bounded net name" );
            else
                result.graphic["net"] = net == "none" ? "" : net;

            continue;
        }

        std::string value;

        if( !oneValue( aDocument, child, value ) )
        {
            diagnostic( result, "invalid_authored_footprint_graphic_field",
                        "footprint graphic " + id + " field " + head + " requires one value" );
            continue;
        }

        if( head == "fill" )
        {
            static const std::set<std::string> fills = {
                "none", "solid", "hatch", "reverse_hatch", "cross_hatch"
            };

            if( !fills.contains( value ) )
                diagnostic( result, "invalid_authored_footprint_graphic_fill",
                            "graphic fill must be none, solid, hatch, reverse_hatch, or cross_hatch" );
            else
                result.graphic["fill"] = value;
        }
        else if( head == "locked" )
        {
            bool parsed = false;

            if( !boolean( value, parsed ) )
                diagnostic( result, "invalid_authored_footprint_graphic_lock",
                            "graphic locked must be true or false" );
            else
                result.graphic["locked"] = parsed;
        }
        else if( head == "radius" )
        {
            int64_t parsed = 0;

            if( !distance( value, parsed ) || parsed < 0 || parsed > MAX_DIMENSION_NM )
                diagnostic( result, "invalid_authored_footprint_graphic_radius",
                            "graphic radius requires a non-negative bounded distance" );
            else
                result.graphic["radiusNm"] = parsed;
        }
        else
        {
            diagnostic( result, "unknown_authored_footprint_graphic_field",
                        "unsupported footprint graphic field " + head );
        }
    }

    const auto require = [&]( const char* aField )
    {
        if( !result.graphic.contains( aField ) )
        {
            diagnostic( result, "missing_authored_footprint_graphic_geometry",
                        "footprint " + kind + " " + id + " requires " + aField );
            return false;
        }

        return true;
    };
    const bool hasStroke = result.graphic["stroke"].contains( "widthNm" );

    if( !hasStroke )
        diagnostic( result, "missing_authored_footprint_graphic_stroke",
                    "footprint graphic " + id + " requires one explicit stroke" );

    if( result.graphic["layers"].empty() )
        diagnostic( result, "missing_authored_footprint_graphic_layers",
                    "footprint graphic " + id + " requires one explicit layers form" );

    if( kind == "line" )
    {
        if( require( "start" ) && require( "end" )
            && samePoint( result.graphic["start"], result.graphic["end"] ) )
        {
            diagnostic( result, "degenerate_authored_footprint_graphic",
                        "footprint line endpoints must differ" );
        }
    }
    else if( kind == "rectangle" )
    {
        if( require( "start" ) && require( "end" ) )
        {
            const int64_t width = std::llabs( result.graphic["end"]["xNm"].get<int64_t>()
                                              - result.graphic["start"]["xNm"].get<int64_t>() );
            const int64_t height = std::llabs( result.graphic["end"]["yNm"].get<int64_t>()
                                               - result.graphic["start"]["yNm"].get<int64_t>() );
            const int64_t radius = result.graphic["radiusNm"].get<int64_t>();

            if( width == 0 || height == 0 || radius * 2 > std::min( width, height ) )
                diagnostic( result, "degenerate_authored_footprint_graphic",
                            "rectangle must have area and radius no greater than half its short side" );
        }
    }
    else if( kind == "circle" )
    {
        if( require( "center" ) && fields.contains( "radius" ) )
        {
            const int64_t centerX = result.graphic["center"]["xNm"].get<int64_t>();
            const int64_t radius = result.graphic["radiusNm"].get<int64_t>();

            if( radius > 0 && centerX > MAX_COORDINATE_NM - radius )
                diagnostic( result, "invalid_authored_footprint_graphic_radius",
                            "circle center plus radius exceeds the bounded coordinate space" );
        }

        if( !fields.contains( "radius" ) || result.graphic["radiusNm"].get<int64_t>() <= 0 )
            diagnostic( result, "degenerate_authored_footprint_graphic",
                        "circle requires one positive radius" );
    }
    else if( kind == "arc" )
    {
        if( require( "start" ) && require( "mid" ) && require( "end" ) )
        {
            const JSON& start = result.graphic["start"];
            const JSON& mid = result.graphic["mid"];
            const JSON& end = result.graphic["end"];
            const long double cross =
                    static_cast<long double>( mid["xNm"].get<int64_t>()
                                              - start["xNm"].get<int64_t>() )
                            * static_cast<long double>( end["yNm"].get<int64_t>()
                                                        - start["yNm"].get<int64_t>() )
                    - static_cast<long double>( mid["yNm"].get<int64_t>()
                                                - start["yNm"].get<int64_t>() )
                            * static_cast<long double>( end["xNm"].get<int64_t>()
                                                        - start["xNm"].get<int64_t>() );

            if( samePoint( start, mid ) || samePoint( mid, end ) || samePoint( start, end )
                || cross == 0.0L )
            {
                diagnostic( result, "degenerate_authored_footprint_graphic",
                            "arc start, mid, and end must be distinct and non-collinear" );
            }
        }
    }
    else if( kind == "polygon" )
    {
        const JSON& points = result.graphic["points"];

        if( points.size() < 3 )
            diagnostic( result, "degenerate_authored_footprint_graphic",
                        "polygon requires at least three implicit-closure points" );
        else
        {
            for( size_t index = 0; index < points.size(); ++index )
            {
                if( samePoint( points[index], points[( index + 1 ) % points.size()] ) )
                {
                    diagnostic( result, "degenerate_authored_footprint_graphic",
                                "polygon points must not repeat consecutively or repeat closure" );
                    break;
                }
            }

            if( signedArea2( points ) == 0.0L )
                diagnostic( result, "degenerate_authored_footprint_graphic",
                            "polygon points must enclose non-zero area" );
        }
    }
    else if( kind == "bezier" )
    {
        if( require( "start" ) && require( "control1" ) && require( "control2" )
            && require( "end" )
            && samePoint( result.graphic["start"], result.graphic["control1"] )
            && samePoint( result.graphic["start"], result.graphic["control2"] )
            && samePoint( result.graphic["start"], result.graphic["end"] ) )
        {
            diagnostic( result, "degenerate_authored_footprint_graphic",
                        "Bezier points cannot all be identical" );
        }
    }

    const bool fillable = kind == "rectangle" || kind == "circle" || kind == "polygon";

    if( !fillable && result.graphic["fill"] != "none" )
        diagnostic( result, "invalid_authored_footprint_graphic_fill",
                    "only rectangles, circles, and polygons support fill" );

    if( kind != "rectangle" && fields.contains( "radius" ) )
    {
        // A circle's semantic radius is valid; every other primitive rejects it.
        if( kind != "circle" )
            diagnostic( result, "invalid_authored_footprint_graphic_radius",
                        "only rectangle and circle primitives support radius" );
    }

    if( !result.graphic["solderMaskMarginNm"].is_null() )
    {
        std::set<std::string> layers;

        for( const JSON& layer : result.graphic["layers"] )
        {
            if( layer.is_string() )
                layers.emplace( layer.get<std::string>() );
        }

        const bool frontMask = layers == std::set<std::string>{ "F.Cu", "F.Mask" };
        const bool backMask = layers == std::set<std::string>{ "B.Cu", "B.Mask" };

        if( !frontMask && !backMask )
            diagnostic( result, "invalid_authored_footprint_graphic_mask_margin",
                        "local solder mask margin requires exactly F.Cu/F.Mask or B.Cu/B.Mask" );
    }

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
