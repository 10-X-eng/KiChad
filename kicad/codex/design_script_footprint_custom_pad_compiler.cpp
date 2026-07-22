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

#include "design_script_footprint_custom_pad_compiler.h"
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
using RESULT = KICHAD::DESIGN_SCRIPT_FOOTPRINT_CUSTOM_PAD_COMPILER::RESULT;

constexpr int64_t MAX_COORDINATE_NM = 1'000'000'000LL;
constexpr size_t MAX_PRIMITIVES = 4096;
constexpr size_t MAX_POINTS = 4096;


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

    if( node.children.size() != 3 || !scalar( aDocument, node.children[1], xText )
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


long double polygonArea2( const JSON& aPoints )
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


JSON compilePrimitive( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    const std::string kind = aDocument.ListHead( aNode );
    std::string id;
    static const std::set<std::string> kinds = {
        "line", "rectangle", "arc", "circle", "polygon", "bezier"
    };

    if( !kinds.contains( kind ) || node.children.size() < 2
        || !scalar( aDocument, node.children[1], id ) || !identifier( id ) )
    {
        diagnostic( aResult, "invalid_authored_custom_pad_primitive_id",
                    "custom pad primitive requires one supported shape and bounded ID" );
        return JSON::object();
    }

    JSON primitive = {
        { "id", id }, { "kind", kind }, { "widthNm", 0 }, { "fill", false },
        { "radiusNm", 0 }, { "points", JSON::array() }
    };
    std::set<std::string> fields;

    for( size_t index = 2; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "point" && kind == "polygon" )
        {
            JSON parsed;

            if( primitive["points"].size() >= MAX_POINTS || !point( aDocument, child, parsed ) )
                diagnostic( aResult, "invalid_authored_custom_pad_primitive_point",
                            "custom polygon accepts 3 through 4096 bounded points" );
            else
                primitive["points"].push_back( std::move( parsed ) );

            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( aResult, "duplicate_authored_custom_pad_primitive_field",
                        "custom primitive " + id + " field " + head + " occurs more than once" );
            continue;
        }

        if( head == "start" || head == "mid" || head == "end" || head == "center"
            || head == "control1" || head == "control2" )
        {
            JSON parsed;

            if( !point( aDocument, child, parsed ) )
                diagnostic( aResult, "invalid_authored_custom_pad_primitive_geometry",
                            "custom primitive " + id + " " + head
                                    + " requires two bounded coordinates" );
            else
                primitive[head] = std::move( parsed );

            continue;
        }

        std::string value;

        if( !oneValue( aDocument, child, value ) )
        {
            diagnostic( aResult, "invalid_authored_custom_pad_primitive_field",
                        "custom primitive field " + head + " requires one value" );
            continue;
        }

        if( head == "width" || head == "radius" )
        {
            int64_t parsed = 0;

            if( !distance( value, parsed ) || parsed < 0 )
                diagnostic( aResult, "invalid_authored_custom_pad_primitive_distance",
                            "custom primitive width/radius requires a non-negative distance" );
            else
                primitive[head == "width" ? "widthNm" : "radiusNm"] = parsed;
        }
        else if( head == "fill" )
        {
            bool parsed = false;

            if( !boolean( value, parsed ) )
                diagnostic( aResult, "invalid_authored_custom_pad_primitive_fill",
                            "custom primitive fill requires true or false" );
            else
                primitive["fill"] = parsed;
        }
        else
        {
            diagnostic( aResult, "unknown_authored_custom_pad_primitive_field",
                        "unsupported custom pad primitive field " + head );
        }
    }

    const auto require = [&]( const char* aField )
    {
        if( !primitive.contains( aField ) )
        {
            diagnostic( aResult, "missing_authored_custom_pad_primitive_geometry",
                        "custom " + kind + " " + id + " requires " + aField );
            return false;
        }

        return true;
    };

    if( kind == "line" )
    {
        if( require( "start" ) && require( "end" )
            && samePoint( primitive["start"], primitive["end"] ) )
        {
            diagnostic( aResult, "degenerate_authored_custom_pad_primitive",
                        "custom line endpoints must differ" );
        }
    }
    else if( kind == "rectangle" )
    {
        if( require( "start" ) && require( "end" ) )
        {
            const int64_t width = std::llabs( primitive["end"]["xNm"].get<int64_t>()
                                              - primitive["start"]["xNm"].get<int64_t>() );
            const int64_t height = std::llabs( primitive["end"]["yNm"].get<int64_t>()
                                               - primitive["start"]["yNm"].get<int64_t>() );

            if( width == 0 || height == 0
                || primitive["radiusNm"].get<int64_t>() * 2 > std::min( width, height ) )
            {
                diagnostic( aResult, "degenerate_authored_custom_pad_primitive",
                            "custom rectangle must have area and a valid corner radius" );
            }
        }
    }
    else if( kind == "circle" )
    {
        require( "center" );

        if( !fields.contains( "radius" ) || primitive["radiusNm"].get<int64_t>() <= 0 )
            diagnostic( aResult, "degenerate_authored_custom_pad_primitive",
                        "custom circle requires one positive radius" );
    }
    else if( kind == "arc" )
    {
        if( require( "start" ) && require( "mid" ) && require( "end" ) )
        {
            const JSON& start = primitive["start"];
            const JSON& mid = primitive["mid"];
            const JSON& end = primitive["end"];
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
                diagnostic( aResult, "degenerate_authored_custom_pad_primitive",
                            "custom arc points must be distinct and non-collinear" );
            }
        }
    }
    else if( kind == "polygon" )
    {
        const JSON& points = primitive["points"];

        if( points.size() < 3 )
            diagnostic( aResult, "degenerate_authored_custom_pad_primitive",
                        "custom polygon requires at least three points" );
        else
        {
            for( size_t index = 0; index < points.size(); ++index )
            {
                if( samePoint( points[index], points[( index + 1 ) % points.size()] ) )
                {
                    diagnostic( aResult, "degenerate_authored_custom_pad_primitive",
                                "custom polygon cannot repeat consecutive or closure points" );
                    break;
                }
            }

            if( polygonArea2( points ) == 0.0L )
                diagnostic( aResult, "degenerate_authored_custom_pad_primitive",
                            "custom polygon must enclose non-zero area" );
        }
    }
    else if( kind == "bezier" )
    {
        if( require( "start" ) && require( "control1" ) && require( "control2" )
            && require( "end" ) && samePoint( primitive["start"], primitive["control1"] )
            && samePoint( primitive["start"], primitive["control2"] )
            && samePoint( primitive["start"], primitive["end"] ) )
        {
            diagnostic( aResult, "degenerate_authored_custom_pad_primitive",
                        "custom bezier control points cannot all coincide" );
        }
    }

    const bool fillable = kind == "rectangle" || kind == "circle" || kind == "polygon";

    if( !fillable && fields.contains( "fill" ) )
        diagnostic( aResult, "invalid_authored_custom_pad_primitive_fill",
                    "only custom rectangles, circles, and polygons accept fill" );

    if( kind != "rectangle" && kind != "circle" && fields.contains( "radius" ) )
        diagnostic( aResult, "invalid_authored_custom_pad_primitive_radius",
                    "only custom rectangles and circles accept radius" );

    return primitive;
}

RESULT compileCustom( const DOCUMENT& aDocument, size_t aNode, bool aPadWide )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );

    if( aDocument.ListHead( aNode ) != "custom" )
    {
        diagnostic( result, "invalid_authored_custom_pad",
                    "custom pad geometry requires one custom form" );
        return result;
    }

    result.custom = { { "anchor", "" },
                      { "clearance", aPadWide ? JSON( "" ) : JSON( nullptr ) },
                      { "primitives", JSON::array() } };
    std::set<std::string> fields;
    std::set<std::string> primitiveIds;
    static const std::set<std::string> primitiveHeads = {
        "line", "rectangle", "arc", "circle", "polygon", "bezier"
    };

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( primitiveHeads.contains( head ) )
        {
            if( result.custom["primitives"].size() >= MAX_PRIMITIVES )
            {
                diagnostic( result, "too_many_authored_custom_pad_primitives",
                            "custom pad accepts at most 4096 primitives" );
                continue;
            }

            JSON primitive = compilePrimitive( aDocument, child, result );
            const std::string id = primitive.value( "id", "" );

            if( !id.empty() && !primitiveIds.emplace( id ).second )
                diagnostic( result, "duplicate_authored_custom_pad_primitive_id",
                            "custom pad primitive ID " + id + " occurs more than once" );

            result.custom["primitives"].push_back( std::move( primitive ) );
            continue;
        }

        if( !fields.emplace( head ).second )
        {
            diagnostic( result, "duplicate_authored_custom_pad_field",
                        "custom pad field " + head + " occurs more than once" );
            continue;
        }

        std::string value;

        if( !oneValue( aDocument, child, value ) )
        {
            diagnostic( result, "invalid_authored_custom_pad_field",
                        "custom pad field " + head + " requires one value" );
            continue;
        }

        if( head == "anchor" )
        {
            if( value != "circle" && value != "rect" )
                diagnostic( result, "invalid_authored_custom_pad_anchor",
                            "custom pad anchor must be circle or rect" );
            else
                result.custom["anchor"] = value;
        }
        else if( head == "clearance" )
        {
            if( !aPadWide )
                diagnostic( result, "unsupported_authored_padstack_layer_custom_clearance",
                            "per-layer custom geometry inherits the pad-wide zone clearance mode" );
            else if( value != "outline" && value != "convex_hull" )
                diagnostic( result, "invalid_authored_custom_pad_clearance",
                            "custom pad clearance must be outline or convex_hull" );
            else
                result.custom["clearance"] = value;
        }
        else
        {
            diagnostic( result, "unknown_authored_custom_pad_field",
                        aPadWide
                                ? "custom pad supports anchor, clearance, and semantic primitives"
                                : "padstack-layer custom geometry supports anchor and semantic primitives" );
        }
    }

    if( result.custom["anchor"].get<std::string>().empty()
        || ( aPadWide && result.custom["clearance"].get<std::string>().empty() )
        || result.custom["primitives"].empty() )
    {
        diagnostic( result, "incomplete_authored_custom_pad",
                    aPadWide
                            ? "custom pad requires anchor, clearance, and at least one primitive"
                            : "padstack-layer custom geometry requires anchor and at least one primitive" );
    }

    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_FOOTPRINT_CUSTOM_PAD_COMPILER::RESULT
DESIGN_SCRIPT_FOOTPRINT_CUSTOM_PAD_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    return compileCustom( aDocument, aNode, true );
}


DESIGN_SCRIPT_FOOTPRINT_CUSTOM_PAD_COMPILER::RESULT
DESIGN_SCRIPT_FOOTPRINT_CUSTOM_PAD_COMPILER::CompileLayer(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    return compileCustom( aDocument, aNode, false );
}

} // namespace KICHAD
