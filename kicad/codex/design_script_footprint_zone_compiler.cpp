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

#include "design_script_footprint_zone_compiler.h"
#include "kichad_from_chars.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/multiprecision/cpp_int.hpp>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_FOOTPRINT_ZONE_COMPILER::RESULT;
using WIDE_INTEGER = boost::multiprecision::int128_t;
using POINT = std::pair<int64_t, int64_t>;

constexpr size_t MAX_ZONE_NAME_BYTES = 256;
constexpr size_t MAX_ZONE_HOLES = 64;
constexpr size_t MAX_ZONE_POINTS = 8192;


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

    return std::all_of( aValue.begin(), aValue.end(), []( unsigned char aCharacter )
    {
        return std::isalnum( aCharacter ) || aCharacter == '_' || aCharacter == '-'
               || aCharacter == '.' || aCharacter == '+';
    } );
}


bool oneValue( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalar( aDocument, node.children[1], aValue );
}


bool boolean( const std::string& aText, bool& aValue )
{
    if( aText != "true" && aText != "false" )
        return false;

    aValue = aText == "true";
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

    if( !std::isfinite( rounded )
        || rounded < static_cast<long double>( std::numeric_limits<int>::min() )
        || rounded > static_cast<long double>( std::numeric_limits<int>::max() ) )
    {
        return false;
    }

    aNanometers = static_cast<int64_t>( rounded );
    return true;
}


bool area( const std::string& aText, int64_t& aSquareNanometers )
{
    long double value = 0.0L;
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const std::from_chars_result converted = KICHAD::FromChars( begin, end, value );

    if( converted.ec != std::errc() || converted.ptr == begin || !std::isfinite( value )
        || value < 0.0L )
    {
        return false;
    }

    const std::string_view unit( converted.ptr, static_cast<size_t>( end - converted.ptr ) );
    long double scale = 0.0L;

    if( unit == "mm2" )
        scale = 1'000'000.0L;
    else if( unit == "mil2" )
        scale = 25'400.0L;
    else if( unit == "um2" )
        scale = 1'000.0L;
    else if( unit == "nm2" )
        scale = 1.0L;
    else if( unit == "in2" )
        scale = 25'400'000.0L;
    else
        return false;

    const long double rounded = std::round( value * scale * scale );

    if( !std::isfinite( rounded ) || rounded < 0.0L
        || rounded >= 9'223'372'036'854'775'808.0L )
    {
        return false;
    }

    aSquareNanometers = static_cast<int64_t>( rounded );
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

    if( tenths < -3'600'000.0L || tenths > 3'600'000.0L
        || std::fabs( tenths - degrees * 10.0L ) > 0.000001L )
    {
        return false;
    }

    aTenths = static_cast<int64_t>( tenths );
    return true;
}


bool ratio( const std::string& aText, int64_t& aPpm )
{
    long double value = 0.0L;
    const char* begin = aText.data();
    const char* end = begin + aText.size();
    const std::from_chars_result converted = KICHAD::FromChars( begin, end, value );

    if( converted.ec != std::errc() || converted.ptr != end || !std::isfinite( value ) )
        return false;

    const long double ppm = std::round( value * 1'000'000.0L );

    if( ppm < 0.0L || ppm > 1'000'000.0L )
        return false;

    aPpm = static_cast<int64_t>( ppm );
    return true;
}


bool unsignedInteger( const std::string& aText, uint32_t& aValue )
{
    const std::from_chars_result converted =
            std::from_chars( aText.data(), aText.data() + aText.size(), aValue );
    return converted.ec == std::errc() && converted.ptr == aText.data() + aText.size();
}


bool collectFields( const DOCUMENT& aDocument, size_t aNode, size_t aBegin,
                    const std::set<std::string>& aAllowed,
                    std::map<std::string, size_t>& aFields, RESULT& aResult,
                    const std::string& aLabel )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    bool ok = true;

    for( size_t index = aBegin; index < node.children.size(); ++index )
    {
        const size_t child = node.children[index];
        const std::string head = aDocument.ListHead( child );

        if( !aAllowed.contains( head ) )
        {
            diagnostic( aResult, "unknown_authored_footprint_zone_field",
                        aLabel + " does not support field " + head );
            ok = false;
        }
        else if( !aFields.emplace( head, child ).second )
        {
            diagnostic( aResult, "duplicate_authored_footprint_zone_field",
                        aLabel + " field " + head + " occurs more than once" );
            ok = false;
        }
    }

    return ok;
}


bool parseDistanceForm( const DOCUMENT& aDocument, size_t aNode, int64_t& aValue )
{
    std::string text;
    return oneValue( aDocument, aNode, text ) && distance( text, aValue );
}


bool copperLayer( const std::string& aLayer )
{
    if( aLayer == "F.Cu" || aLayer == "B.Cu" )
        return true;

    if( !aLayer.starts_with( "In" ) || !aLayer.ends_with( ".Cu" ) )
        return false;

    const std::string number = aLayer.substr( 2, aLayer.size() - 5 );
    unsigned int index = 0;
    const std::from_chars_result parsed =
            std::from_chars( number.data(), number.data() + number.size(), index );
    return parsed.ec == std::errc() && parsed.ptr == number.data() + number.size()
           && index >= 1 && index <= 30;
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


bool parsePoint( const DOCUMENT& aDocument, size_t aNode, JSON& aPoint )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string xText;
    std::string yText;
    int64_t x = 0;
    int64_t y = 0;

    if( aDocument.ListHead( aNode ) != "point" || node.children.size() != 3
        || !scalar( aDocument, node.children[1], xText )
        || !scalar( aDocument, node.children[2], yText )
        || !distance( xText, x ) || !distance( yText, y ) )
    {
        return false;
    }

    aPoint = { { "xNm", x }, { "yNm", y } };
    return true;
}


std::vector<POINT> points( const JSON& aLine )
{
    std::vector<POINT> result;

    if( !aLine.is_array() )
        return result;

    for( const JSON& point : aLine )
    {
        if( !point.is_object() || !point.contains( "xNm" ) || !point["xNm"].is_number_integer()
            || !point.contains( "yNm" ) || !point["yNm"].is_number_integer() )
        {
            return {};
        }

        result.emplace_back( point["xNm"].get<int64_t>(), point["yNm"].get<int64_t>() );
    }

    return result;
}


WIDE_INTEGER orientation( const POINT& aStart, const POINT& aEnd, const POINT& aPoint )
{
    return WIDE_INTEGER( aEnd.first - aStart.first ) * ( aPoint.second - aStart.second )
           - WIDE_INTEGER( aEnd.second - aStart.second ) * ( aPoint.first - aStart.first );
}


bool pointOnSegment( const POINT& aPoint, const POINT& aStart, const POINT& aEnd )
{
    return orientation( aStart, aEnd, aPoint ) == 0
           && aPoint.first >= std::min( aStart.first, aEnd.first )
           && aPoint.first <= std::max( aStart.first, aEnd.first )
           && aPoint.second >= std::min( aStart.second, aEnd.second )
           && aPoint.second <= std::max( aStart.second, aEnd.second );
}


bool segmentsIntersect( const POINT& aStart, const POINT& aEnd,
                        const POINT& bStart, const POINT& bEnd )
{
    const WIDE_INTEGER a = orientation( aStart, aEnd, bStart );
    const WIDE_INTEGER b = orientation( aStart, aEnd, bEnd );
    const WIDE_INTEGER c = orientation( bStart, bEnd, aStart );
    const WIDE_INTEGER d = orientation( bStart, bEnd, aEnd );

    if( ( ( a > 0 && b < 0 ) || ( a < 0 && b > 0 ) )
        && ( ( c > 0 && d < 0 ) || ( c < 0 && d > 0 ) ) )
    {
        return true;
    }

    return ( a == 0 && pointOnSegment( bStart, aStart, aEnd ) )
           || ( b == 0 && pointOnSegment( bEnd, aStart, aEnd ) )
           || ( c == 0 && pointOnSegment( aStart, bStart, bEnd ) )
           || ( d == 0 && pointOnSegment( aEnd, bStart, bEnd ) );
}


bool linesIntersect( const std::vector<POINT>& aFirst, const std::vector<POINT>& aSecond )
{
    for( size_t first = 0; first < aFirst.size(); ++first )
    {
        for( size_t second = 0; second < aSecond.size(); ++second )
        {
            if( segmentsIntersect( aFirst[first], aFirst[( first + 1 ) % aFirst.size()],
                                   aSecond[second], aSecond[( second + 1 ) % aSecond.size()] ) )
            {
                return true;
            }
        }
    }

    return false;
}


bool selfIntersects( const std::vector<POINT>& aPoints )
{
    for( size_t first = 0; first < aPoints.size(); ++first )
    {
        const size_t firstEnd = ( first + 1 ) % aPoints.size();

        for( size_t second = first + 1; second < aPoints.size(); ++second )
        {
            const size_t secondEnd = ( second + 1 ) % aPoints.size();

            if( first == second || firstEnd == second || secondEnd == first )
                continue;

            if( segmentsIntersect( aPoints[first], aPoints[firstEnd],
                                   aPoints[second], aPoints[secondEnd] ) )
            {
                return true;
            }
        }
    }

    return false;
}


bool pointInside( const POINT& aPoint, const std::vector<POINT>& aLine )
{
    int winding = 0;

    for( size_t index = 0; index < aLine.size(); ++index )
    {
        const POINT& start = aLine[index];
        const POINT& end = aLine[( index + 1 ) % aLine.size()];

        if( pointOnSegment( aPoint, start, end ) )
            return false;

        if( start.second <= aPoint.second )
        {
            if( end.second > aPoint.second && orientation( start, end, aPoint ) > 0 )
                ++winding;
        }
        else if( end.second <= aPoint.second && orientation( start, end, aPoint ) < 0 )
        {
            --winding;
        }
    }

    return winding != 0;
}


bool validateLine( const JSON& aLine, RESULT& aResult, const std::string& aLabel )
{
    const std::vector<POINT> line = points( aLine );

    if( line.size() < 3 )
    {
        diagnostic( aResult, "invalid_authored_footprint_zone_polygon",
                    aLabel + " requires at least three points" );
        return false;
    }

    std::set<POINT> unique;
    WIDE_INTEGER doubledArea = 0;

    for( size_t index = 0; index < line.size(); ++index )
    {
        if( !unique.emplace( line[index] ).second )
        {
            diagnostic( aResult, "duplicate_authored_footprint_zone_point",
                        aLabel + " must not repeat a point; closure is implicit" );
            return false;
        }

        const POINT& next = line[( index + 1 ) % line.size()];
        doubledArea += WIDE_INTEGER( line[index].first ) * next.second
                       - WIDE_INTEGER( next.first ) * line[index].second;
    }

    if( doubledArea == 0 )
    {
        diagnostic( aResult, "zero_area_authored_footprint_zone_polygon",
                    aLabel + " must enclose a non-zero area" );
        return false;
    }

    if( selfIntersects( line ) )
    {
        diagnostic( aResult, "self_intersecting_authored_footprint_zone_polygon",
                    aLabel + " must not self-intersect" );
        return false;
    }

    return true;
}


JSON compileOutline( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& outline = aDocument.Nodes().at( aNode );
    JSON result = { { "outer", JSON::array() }, { "holes", JSON::array() } };

    if( outline.children.size() != 2
        || aDocument.ListHead( outline.children[1] ) != "polygon" )
    {
        diagnostic( aResult, "invalid_authored_footprint_zone_outline",
                    "footprint zone outline requires exactly one polygon; use another zone for a disjoint area" );
        return result;
    }

    const DOCUMENT::NODE& polygon = aDocument.Nodes().at( outline.children[1] );
    size_t pointCount = 0;

    for( size_t index = 1; index < polygon.children.size(); ++index )
    {
        const size_t child = polygon.children[index];
        const std::string head = aDocument.ListHead( child );

        if( head == "point" )
        {
            JSON point;

            if( !parsePoint( aDocument, child, point ) )
            {
                diagnostic( aResult, "invalid_authored_footprint_zone_point",
                            "zone points require bounded X and Y distances" );
            }
            else if( pointCount++ >= MAX_ZONE_POINTS )
            {
                if( pointCount == MAX_ZONE_POINTS + 1 )
                    diagnostic( aResult, "too_many_authored_footprint_zone_points",
                                "a footprint zone may contain at most 8192 points" );
            }
            else
            {
                result["outer"].push_back( std::move( point ) );
            }
        }
        else if( head == "hole" )
        {
            if( result["holes"].size() >= MAX_ZONE_HOLES )
            {
                diagnostic( aResult, "too_many_authored_footprint_zone_holes",
                            "a footprint zone may contain at most 64 holes" );
                continue;
            }

            JSON hole = JSON::array();
            const DOCUMENT::NODE& holeNode = aDocument.Nodes().at( child );

            for( size_t holeIndex = 1; holeIndex < holeNode.children.size(); ++holeIndex )
            {
                JSON point;

                if( !parsePoint( aDocument, holeNode.children[holeIndex], point ) )
                {
                    diagnostic( aResult, "invalid_authored_footprint_zone_point",
                                "zone hole points require bounded X and Y distances" );
                }
                else if( pointCount++ >= MAX_ZONE_POINTS )
                {
                    if( pointCount == MAX_ZONE_POINTS + 1 )
                        diagnostic( aResult, "too_many_authored_footprint_zone_points",
                                    "a footprint zone may contain at most 8192 points" );
                }
                else
                {
                    hole.push_back( std::move( point ) );
                }
            }

            validateLine( hole, aResult, "zone hole" );
            result["holes"].push_back( std::move( hole ) );
        }
        else
        {
            diagnostic( aResult, "unknown_authored_footprint_zone_geometry",
                        "zone polygons accept only point and hole declarations" );
        }
    }

    validateLine( result["outer"], aResult, "zone outline" );
    const std::vector<POINT> outer = points( result["outer"] );
    std::vector<std::vector<POINT>> holes;

    for( const JSON& holeJson : result["holes"] )
    {
        const std::vector<POINT> hole = points( holeJson );

        if( outer.size() >= 3 && hole.size() >= 3
            && ( linesIntersect( outer, hole ) || !pointInside( hole.front(), outer ) ) )
        {
            diagnostic( aResult, "authored_footprint_zone_hole_outside_outline",
                        "each zone hole must be strictly inside its outer polygon" );
        }

        if( hole.size() < 3 )
        {
            holes.push_back( hole );
            continue;
        }

        for( const std::vector<POINT>& existing : holes )
        {
            if( existing.size() < 3 )
                continue;

            if( linesIntersect( existing, hole ) || pointInside( hole.front(), existing )
                || pointInside( existing.front(), hole ) )
            {
                diagnostic( aResult, "overlapping_authored_footprint_zone_holes",
                            "zone holes must not overlap or contain one another" );
                break;
            }
        }

        holes.push_back( hole );
    }

    return result;
}


JSON compileBorder( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string style;
    int64_t pitch = 0;

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], style )
        || ( style != "solid" && style != "diagonal_full" && style != "diagonal_edge" ) )
    {
        diagnostic( aResult, "invalid_authored_footprint_zone_border",
                    "footprint zone border must be solid, diagonal_full, or diagonal_edge" );
    }

    const bool diagonal = style == "diagonal_full" || style == "diagonal_edge";
    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 2, { "pitch" }, fields, aResult, "zone border" );

    if( diagonal )
    {
        if( !fields.contains( "pitch" )
            || !parseDistanceForm( aDocument, fields["pitch"], pitch )
            || pitch < 100'000 || pitch > 2'000'000 )
        {
            diagnostic( aResult, "invalid_authored_footprint_zone_border_pitch",
                        "diagonal zone borders require a pitch from 0.1mm through 2mm" );
        }
    }
    else if( fields.contains( "pitch" ) )
    {
        diagnostic( aResult, "unexpected_authored_footprint_zone_border_pitch",
                    "border pitch is valid only for diagonal borders" );
    }

    return { { "style", style }, { "pitchNm", pitch } };
}


JSON compileConnection( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string style;
    int64_t thermalGap = 0;
    int64_t thermalSpokeWidth = 0;
    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 2, { "thermal_gap", "thermal_spoke_width" },
                   fields, aResult, "zone connection" );

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], style )
        || ( style != "none" && style != "solid" && style != "thermal"
             && style != "pth_thermal" ) )
    {
        diagnostic( aResult, "invalid_authored_footprint_zone_connection",
                    "zone connection must be none, solid, thermal, or pth_thermal" );
    }

    const bool thermal = style == "thermal" || style == "pth_thermal";

    if( thermal )
    {
        if( !fields.contains( "thermal_gap" )
            || !parseDistanceForm( aDocument, fields["thermal_gap"], thermalGap )
            || thermalGap <= 0 )
        {
            diagnostic( aResult, "invalid_authored_footprint_zone_thermal_gap",
                        "thermal connections require a positive thermal_gap" );
        }

        if( !fields.contains( "thermal_spoke_width" )
            || !parseDistanceForm( aDocument, fields["thermal_spoke_width"], thermalSpokeWidth )
            || thermalSpokeWidth <= 0 )
        {
            diagnostic( aResult, "invalid_authored_footprint_zone_thermal_spoke_width",
                        "thermal connections require a positive thermal_spoke_width" );
        }
    }
    else if( !fields.empty() )
    {
        diagnostic( aResult, "unexpected_authored_footprint_zone_thermal_setting",
                    "thermal dimensions are valid only for thermal connections" );
    }

    return { { "style", style }, { "thermalGapNm", thermalGap },
             { "thermalSpokeWidthNm", thermalSpokeWidth } };
}


JSON compileIslands( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string mode;
    int64_t minimumArea = 0;
    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 2, { "minimum_area" }, fields, aResult,
                   "zone islands" );

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], mode )
        || ( mode != "remove_all" && mode != "keep_all" && mode != "remove_below" ) )
    {
        diagnostic( aResult, "invalid_authored_footprint_zone_islands",
                    "zone islands must be remove_all, keep_all, or remove_below" );
    }

    if( mode == "remove_below" )
    {
        std::string value;

        if( !fields.contains( "minimum_area" )
            || !oneValue( aDocument, fields["minimum_area"], value )
            || !area( value, minimumArea ) || minimumArea <= 0 )
        {
            diagnostic( aResult, "invalid_authored_footprint_zone_minimum_island_area",
                        "remove_below requires a positive minimum_area with square units" );
        }
    }
    else if( fields.contains( "minimum_area" ) )
    {
        diagnostic( aResult, "unexpected_authored_footprint_zone_minimum_island_area",
                    "minimum_area is valid only with remove_below" );
    }

    return { { "mode", mode }, { "minimumAreaNm2", minimumArea } };
}


JSON compileFill( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string mode;
    int64_t thickness = 0;
    int64_t gap = 0;
    int64_t orientationTenths = 0;
    int64_t smoothingPpm = 0;
    int64_t holeAreaPpm = 0;
    std::string border = "minimum";
    std::string smoothing = "none";
    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 2,
                   { "thickness", "gap", "orientation", "edge_smoothing",
                     "hole_min_area_ratio", "border" },
                   fields, aResult, "zone fill" );

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], mode )
        || ( mode != "solid" && mode != "hatched" ) )
    {
        diagnostic( aResult, "invalid_authored_footprint_zone_fill",
                    "zone fill must be solid or hatched" );
    }

    if( mode == "hatched" )
    {
        if( !fields.contains( "thickness" )
            || !parseDistanceForm( aDocument, fields["thickness"], thickness )
            || thickness <= 0 )
        {
            diagnostic( aResult, "invalid_authored_footprint_zone_hatch_thickness",
                        "hatched fill requires a positive thickness" );
        }

        if( !fields.contains( "gap" ) || !parseDistanceForm( aDocument, fields["gap"], gap )
            || gap <= 0 )
        {
            diagnostic( aResult, "invalid_authored_footprint_zone_hatch_gap",
                        "hatched fill requires a positive gap" );
        }

        std::string value;

        if( !fields.contains( "orientation" )
            || !oneValue( aDocument, fields["orientation"], value )
            || !angle( value, orientationTenths ) )
        {
            diagnostic( aResult, "invalid_authored_footprint_zone_hatch_orientation",
                        "hatched fill requires an orientation with a deg suffix" );
        }

        if( fields.contains( "edge_smoothing" ) )
        {
            const size_t smoothingNode = fields["edge_smoothing"];
            const DOCUMENT::NODE& smoothingForm = aDocument.Nodes().at( smoothingNode );
            std::map<std::string, size_t> smoothingFields;
            collectFields( aDocument, smoothingNode, 2, { "amount" }, smoothingFields,
                           aResult, "zone hatch edge_smoothing" );

            if( smoothingForm.children.size() < 2
                || !scalar( aDocument, smoothingForm.children[1], smoothing )
                || ( smoothing != "none" && smoothing != "chamfer" && smoothing != "fillet" ) )
            {
                diagnostic( aResult, "invalid_authored_footprint_zone_hatch_smoothing",
                            "edge_smoothing must be none, chamfer, or fillet" );
            }

            if( smoothing == "none" )
            {
                if( smoothingFields.contains( "amount" ) )
                    diagnostic( aResult, "unexpected_authored_footprint_zone_hatch_smoothing_amount",
                                "none edge smoothing has no amount" );
            }
            else if( !smoothingFields.contains( "amount" )
                     || !oneValue( aDocument, smoothingFields["amount"], value )
                     || !ratio( value, smoothingPpm ) || smoothingPpm == 0 )
            {
                diagnostic( aResult, "invalid_authored_footprint_zone_hatch_smoothing_amount",
                            "chamfer and fillet edge smoothing require a positive amount ratio" );
            }
        }

        if( fields.contains( "hole_min_area_ratio" )
            && ( !oneValue( aDocument, fields["hole_min_area_ratio"], value )
                 || !ratio( value, holeAreaPpm ) ) )
        {
            diagnostic( aResult, "invalid_authored_footprint_zone_hatch_hole_ratio",
                        "hole_min_area_ratio must be from zero through one" );
        }

        if( fields.contains( "border" )
            && ( !oneValue( aDocument, fields["border"], border )
                 || ( border != "minimum" && border != "hatch" ) ) )
        {
            diagnostic( aResult, "invalid_authored_footprint_zone_hatch_border",
                        "hatched fill border must be minimum or hatch" );
        }
    }
    else if( !fields.empty() )
    {
        diagnostic( aResult, "unexpected_authored_footprint_solid_zone_fill_setting",
                    "solid zone fill has no hatch settings" );
    }

    return { { "mode", mode }, { "thicknessNm", thickness }, { "gapNm", gap },
             { "orientationTenths", orientationTenths }, { "smoothing", smoothing },
             { "smoothingPpm", smoothingPpm }, { "holeMinimumAreaPpm", holeAreaPpm },
             { "borderMode", border } };
}


JSON compileCornerSmoothing( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string style;
    int64_t radius = 0;
    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 2, { "radius" }, fields, aResult,
                   "zone corner_smoothing" );

    if( node.children.size() < 2 || !scalar( aDocument, node.children[1], style )
        || ( style != "none" && style != "chamfer" && style != "fillet" ) )
    {
        diagnostic( aResult, "invalid_authored_footprint_zone_corner_smoothing",
                    "corner_smoothing must be none, chamfer, or fillet" );
    }

    if( style == "none" )
    {
        if( fields.contains( "radius" ) )
            diagnostic( aResult, "unexpected_authored_footprint_zone_corner_radius",
                        "none corner smoothing has no radius" );
    }
    else if( !fields.contains( "radius" )
             || !parseDistanceForm( aDocument, fields["radius"], radius ) || radius <= 0 )
    {
        diagnostic( aResult, "invalid_authored_footprint_zone_corner_radius",
                    "chamfer and fillet corner smoothing require a positive radius" );
    }

    return { { "style", style }, { "radiusNm", radius } };
}


JSON compileProhibit( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    std::map<std::string, size_t> fields;
    const std::set<std::string> names = { "copper", "vias", "tracks", "pads", "footprints" };
    collectFields( aDocument, aNode, 1, names, fields, aResult, "zone prohibit" );
    JSON result = JSON::object();
    bool any = false;

    for( const std::string& name : names )
    {
        std::string value;
        bool parsed = false;

        if( !fields.contains( name ) || !oneValue( aDocument, fields[name], value )
            || !boolean( value, parsed ) )
        {
            diagnostic( aResult, "invalid_authored_footprint_zone_prohibition",
                        "rule-area prohibit requires explicit true/false " + name );
        }

        result[name] = parsed;
        any = any || parsed;
    }

    if( !any )
        diagnostic( aResult, "ineffective_authored_footprint_zone_rule_area",
                    "a rule area must prohibit at least one item category" );

    return result;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_FOOTPRINT_ZONE_COMPILER::RESULT
DESIGN_SCRIPT_FOOTPRINT_ZONE_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aNode )
{
    RESULT result;
    const DOCUMENT::NODE& node = aDocument.Nodes().at( aNode );
    std::string id;

    if( aDocument.ListHead( aNode ) != "zone" || node.children.size() < 2
        || !scalar( aDocument, node.children[1], id ) || !identifier( id ) )
    {
        diagnostic( result, "invalid_authored_footprint_zone_id",
                    "footprint zone requires one unique bounded logical ID" );
        return result;
    }

    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 2,
                   { "purpose", "name", "layers", "outline", "clearance", "min_thickness",
                     "connection", "islands", "fill", "priority", "border",
                     "corner_smoothing", "prohibit", "locked" },
                   fields, result, "footprint zone" );
    std::string purpose;
    std::string name = id;
    bool locked = false;
    uint32_t priority = 0;
    int64_t clearance = 0;
    int64_t minimumThickness = 250'000;
    JSON layers = JSON::array();
    JSON outline = { { "outer", JSON::array() }, { "holes", JSON::array() } };
    JSON border = { { "style", "solid" }, { "pitchNm", 500'000 } };
    JSON cornerSmoothing = { { "style", "none" }, { "radiusNm", 0 } };
    JSON connection = { { "style", "thermal" }, { "thermalGapNm", 500'000 },
                        { "thermalSpokeWidthNm", 500'000 } };
    JSON islands = { { "mode", "remove_below" }, { "minimumAreaNm2", 10'000'000'000LL } };
    JSON fill = { { "mode", "solid" }, { "thicknessNm", 0 }, { "gapNm", 0 },
                  { "orientationTenths", 0 }, { "smoothing", "none" },
                  { "smoothingPpm", 0 }, { "holeMinimumAreaPpm", 0 },
                  { "borderMode", "minimum" } };
    JSON prohibit = nullptr;
    std::string value;

    if( !fields.contains( "purpose" ) || !oneValue( aDocument, fields["purpose"], purpose )
        || ( purpose != "copper" && purpose != "keepout" ) )
    {
        diagnostic( result, "invalid_authored_footprint_zone_purpose",
                    "footprint zone purpose must be copper or keepout" );
    }

    if( fields.contains( "name" )
        && ( !oneValue( aDocument, fields["name"], name ) || name.empty()
             || name.size() > MAX_ZONE_NAME_BYTES || name.find( '\0' ) != std::string::npos ) )
    {
        diagnostic( result, "invalid_authored_footprint_zone_name",
                    "zone name must contain 1 through 256 bounded UTF-8 bytes" );
    }

    if( !fields.contains( "layers" ) )
    {
        diagnostic( result, "invalid_authored_footprint_zone_layers",
                    "footprint zone requires one layers declaration" );
    }
    else
    {
        const DOCUMENT::NODE& layerNode = aDocument.Nodes().at( fields["layers"] );
        std::set<std::string> unique;

        if( layerNode.children.size() < 2 || layerNode.children.size() > 33 )
            diagnostic( result, "invalid_authored_footprint_zone_layers",
                        "zone layers requires 1 through 32 unique layers" );

        for( size_t index = 1; index < layerNode.children.size(); ++index )
        {
            std::string layer;
            const bool valid = scalar( aDocument, layerNode.children[index], layer )
                               && ( purpose == "copper" ? copperLayer( layer )
                                                        : layer == "all_copper"
                                                          || physicalLayer( layer ) )
                               && unique.emplace( layer ).second;

            if( !valid )
                diagnostic( result, "invalid_authored_footprint_zone_layers",
                            purpose == "copper"
                                    ? "copper zones require distinct named copper layers"
                                    : "keepouts require distinct physical layers or all_copper" );
            else
                layers.push_back( layer );
        }
    }

    if( !fields.contains( "outline" ) )
        diagnostic( result, "invalid_authored_footprint_zone_outline",
                    "footprint zone requires one explicit polygon outline" );
    else
        outline = compileOutline( aDocument, fields["outline"], result );

    if( fields.contains( "border" ) )
        border = compileBorder( aDocument, fields["border"], result );

    if( fields.contains( "corner_smoothing" ) )
        cornerSmoothing = compileCornerSmoothing( aDocument, fields["corner_smoothing"], result );

    if( fields.contains( "locked" )
        && ( !oneValue( aDocument, fields["locked"], value ) || !boolean( value, locked ) ) )
    {
        diagnostic( result, "invalid_authored_footprint_zone_locked",
                    "zone locked must be true or false" );
    }

    if( fields.contains( "priority" )
        && ( !oneValue( aDocument, fields["priority"], value )
             || !unsignedInteger( value, priority )
             || priority > static_cast<uint32_t>( std::numeric_limits<int>::max() ) ) )
    {
        diagnostic( result, "invalid_authored_footprint_zone_priority",
                    "zone priority must be an integer from 0 through 2147483647" );
    }

    if( purpose == "copper" )
    {
        if( !fields.contains( "clearance" )
            || !parseDistanceForm( aDocument, fields["clearance"], clearance ) || clearance < 0 )
        {
            diagnostic( result, "invalid_authored_footprint_zone_clearance",
                        "copper zone requires a non-negative clearance" );
        }

        if( !fields.contains( "min_thickness" )
            || !parseDistanceForm( aDocument, fields["min_thickness"], minimumThickness )
            || minimumThickness <= 0 )
        {
            diagnostic( result, "invalid_authored_footprint_zone_min_thickness",
                        "copper zone requires a positive min_thickness" );
        }

        if( !fields.contains( "connection" ) )
            diagnostic( result, "invalid_authored_footprint_zone_connection",
                        "copper zone requires an explicit connection policy" );
        else
            connection = compileConnection( aDocument, fields["connection"], result );

        if( !fields.contains( "islands" ) )
            diagnostic( result, "invalid_authored_footprint_zone_islands",
                        "copper zone requires an explicit islands policy" );
        else
            islands = compileIslands( aDocument, fields["islands"], result );

        if( !fields.contains( "fill" ) )
            diagnostic( result, "invalid_authored_footprint_zone_fill",
                        "copper zone requires an explicit fill policy" );
        else
            fill = compileFill( aDocument, fields["fill"], result );

        if( fields.contains( "prohibit" ) )
            diagnostic( result, "unexpected_authored_footprint_zone_prohibit",
                        "prohibit is valid only for keepout zones" );
    }
    else if( purpose == "keepout" )
    {
        if( !fields.contains( "prohibit" ) )
            diagnostic( result, "invalid_authored_footprint_zone_prohibition",
                        "keepout zone requires an explicit prohibit policy" );
        else
            prohibit = compileProhibit( aDocument, fields["prohibit"], result );

        for( const char* field : { "clearance", "min_thickness", "connection", "islands", "fill",
                                   "priority", "corner_smoothing" } )
        {
            if( fields.contains( field ) )
                diagnostic( result, "unexpected_authored_footprint_keepout_setting",
                            std::string( field ) + " is valid only for copper zones" );
        }
    }

    result.zone = { { "id", id }, { "purpose", purpose }, { "name", name },
                    { "layers", std::move( layers ) }, { "outline", std::move( outline ) },
                    { "clearanceNm", clearance }, { "minThicknessNm", minimumThickness },
                    { "connection", std::move( connection ) }, { "islands", std::move( islands ) },
                    { "fill", std::move( fill ) }, { "priority", priority },
                    { "border", std::move( border ) },
                    { "cornerSmoothing", std::move( cornerSmoothing ) },
                    { "prohibit", std::move( prohibit ) }, { "locked", locked } };
    result.ok = result.diagnostics.empty();
    return result;
}

} // namespace KICHAD
