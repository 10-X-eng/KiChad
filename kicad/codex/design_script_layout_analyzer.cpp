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

#include "design_script_layout_analyzer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>


namespace
{

using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_LAYOUT_ANALYZER::RESULT;

constexpr long double PI = 3.141592653589793238462643383279502884L;


struct POINT
{
    long double x = 0.0L;
    long double y = 0.0L;
};


struct BOUNDS
{
    long double minimumX = std::numeric_limits<long double>::infinity();
    long double minimumY = std::numeric_limits<long double>::infinity();
    long double maximumX = -std::numeric_limits<long double>::infinity();
    long double maximumY = -std::numeric_limits<long double>::infinity();

    void Include( const POINT& aPoint )
    {
        minimumX = std::min( minimumX, aPoint.x );
        minimumY = std::min( minimumY, aPoint.y );
        maximumX = std::max( maximumX, aPoint.x );
        maximumY = std::max( maximumY, aPoint.y );
    }

    bool Valid() const
    {
        return std::isfinite( minimumX ) && std::isfinite( minimumY )
               && std::isfinite( maximumX ) && std::isfinite( maximumY )
               && maximumX >= minimumX && maximumY >= minimumY;
    }
};


bool point( const JSON& aJson, POINT& aPoint )
{
    if( !aJson.is_object() || !aJson.contains( "xNm" )
        || !aJson["xNm"].is_number_integer() || !aJson.contains( "yNm" )
        || !aJson["yNm"].is_number_integer() )
    {
        return false;
    }

    aPoint.x = aJson["xNm"].get<int64_t>();
    aPoint.y = aJson["yNm"].get<int64_t>();
    return true;
}


long double distance( const POINT& aFirst, const POINT& aSecond )
{
    return std::hypotl( aSecond.x - aFirst.x, aSecond.y - aFirst.y );
}


long double normalizedAngle( long double aAngle )
{
    const long double full = 2.0L * PI;

    while( aAngle < 0.0L )
        aAngle += full;

    while( aAngle >= full )
        aAngle -= full;

    return aAngle;
}


long double counterClockwiseDelta( long double aFrom, long double aTo )
{
    return normalizedAngle( aTo - aFrom );
}


bool angleOnArc( long double aStart, long double aMid, long double aEnd,
                 long double aCandidate )
{
    constexpr long double EPSILON = 1e-12L;
    const long double startToMid = counterClockwiseDelta( aStart, aMid );
    const long double startToEnd = counterClockwiseDelta( aStart, aEnd );
    const bool counterClockwise = startToMid <= startToEnd + EPSILON;

    if( counterClockwise )
        return counterClockwiseDelta( aStart, aCandidate ) <= startToEnd + EPSILON;

    return counterClockwiseDelta( aCandidate, aStart )
           <= counterClockwiseDelta( aEnd, aStart ) + EPSILON;
}


bool arcGeometry( const JSON& aJson, POINT& aStart, POINT& aMid, POINT& aEnd,
                  POINT& aCenter, long double& aRadius, long double& aSweep )
{
    if( !point( aJson.value( "start", JSON::object() ), aStart )
        || !point( aJson.value( "mid", JSON::object() ), aMid )
        || !point( aJson.value( "end", JSON::object() ), aEnd ) )
    {
        return false;
    }

    const long double determinant =
            2.0L * ( aStart.x * ( aMid.y - aEnd.y )
                     + aMid.x * ( aEnd.y - aStart.y )
                     + aEnd.x * ( aStart.y - aMid.y ) );

    if( std::abs( determinant ) < 1e-12L )
        return false;

    const long double startSquare = aStart.x * aStart.x + aStart.y * aStart.y;
    const long double midSquare = aMid.x * aMid.x + aMid.y * aMid.y;
    const long double endSquare = aEnd.x * aEnd.x + aEnd.y * aEnd.y;
    aCenter.x = ( startSquare * ( aMid.y - aEnd.y )
                  + midSquare * ( aEnd.y - aStart.y )
                  + endSquare * ( aStart.y - aMid.y ) ) / determinant;
    aCenter.y = ( startSquare * ( aEnd.x - aMid.x )
                  + midSquare * ( aStart.x - aEnd.x )
                  + endSquare * ( aMid.x - aStart.x ) ) / determinant;
    aRadius = distance( aCenter, aStart );

    if( !std::isfinite( aRadius ) || aRadius <= 0.0L )
        return false;

    const long double startAngle = std::atan2( aStart.y - aCenter.y,
                                                aStart.x - aCenter.x );
    const long double midAngle = std::atan2( aMid.y - aCenter.y,
                                              aMid.x - aCenter.x );
    const long double endAngle = std::atan2( aEnd.y - aCenter.y,
                                              aEnd.x - aCenter.x );
    const long double ccwEnd = counterClockwiseDelta( startAngle, endAngle );
    const bool counterClockwise = counterClockwiseDelta( startAngle, midAngle )
                                  <= ccwEnd + 1e-12L;
    aSweep = counterClockwise ? ccwEnd : counterClockwiseDelta( endAngle, startAngle );
    return std::isfinite( aSweep ) && aSweep > 0.0L;
}


void includeArc( const JSON& aJson, BOUNDS& aBounds )
{
    POINT start;
    POINT mid;
    POINT end;
    POINT center;
    long double radius = 0.0L;
    long double sweep = 0.0L;

    if( !arcGeometry( aJson, start, mid, end, center, radius, sweep ) )
        return;

    aBounds.Include( start );
    aBounds.Include( mid );
    aBounds.Include( end );
    const long double startAngle = std::atan2( start.y - center.y, start.x - center.x );
    const long double midAngle = std::atan2( mid.y - center.y, mid.x - center.x );
    const long double endAngle = std::atan2( end.y - center.y, end.x - center.x );

    for( long double candidate : std::array<long double, 4>{
                 0.0L, PI / 2.0L, PI, 3.0L * PI / 2.0L } )
    {
        if( angleOnArc( startAngle, midAngle, endAngle, candidate ) )
        {
            aBounds.Include( { center.x + radius * std::cos( candidate ),
                               center.y + radius * std::sin( candidate ) } );
        }
    }
}


long double cubicCoordinate( long double aP0, long double aP1, long double aP2,
                             long double aP3, long double aT )
{
    const long double inverse = 1.0L - aT;
    return inverse * inverse * inverse * aP0
           + 3.0L * inverse * inverse * aT * aP1
           + 3.0L * inverse * aT * aT * aP2 + aT * aT * aT * aP3;
}


std::vector<long double> cubicExtrema( long double aP0, long double aP1,
                                       long double aP2, long double aP3 )
{
    const long double a = -aP0 + 3.0L * aP1 - 3.0L * aP2 + aP3;
    const long double b = 3.0L * aP0 - 6.0L * aP1 + 3.0L * aP2;
    const long double c = -3.0L * aP0 + 3.0L * aP1;
    const long double qa = 3.0L * a;
    const long double qb = 2.0L * b;
    std::vector<long double> values;

    if( std::abs( qa ) < 1e-18L )
    {
        if( std::abs( qb ) >= 1e-18L )
        {
            const long double root = -c / qb;

            if( root > 0.0L && root < 1.0L )
                values.push_back( root );
        }

        return values;
    }

    const long double discriminant = qb * qb - 4.0L * qa * c;

    if( discriminant < 0.0L )
        return values;

    const long double squareRoot = std::sqrt( discriminant );

    for( long double root : { ( -qb + squareRoot ) / ( 2.0L * qa ),
                              ( -qb - squareRoot ) / ( 2.0L * qa ) } )
    {
        if( root > 0.0L && root < 1.0L )
            values.push_back( root );
    }

    return values;
}


void includeBezier( const JSON& aGraphic, BOUNDS& aBounds )
{
    POINT start;
    POINT control1;
    POINT control2;
    POINT end;

    if( !point( aGraphic.value( "start", JSON::object() ), start )
        || !point( aGraphic.value( "control1", JSON::object() ), control1 )
        || !point( aGraphic.value( "control2", JSON::object() ), control2 )
        || !point( aGraphic.value( "end", JSON::object() ), end ) )
    {
        return;
    }

    aBounds.Include( start );
    aBounds.Include( end );
    std::set<long double> parameters;

    for( long double value : cubicExtrema( start.x, control1.x, control2.x, end.x ) )
        parameters.emplace( value );

    for( long double value : cubicExtrema( start.y, control1.y, control2.y, end.y ) )
        parameters.emplace( value );

    for( long double value : parameters )
    {
        aBounds.Include( { cubicCoordinate( start.x, control1.x, control2.x, end.x, value ),
                           cubicCoordinate( start.y, control1.y, control2.y, end.y, value ) } );
    }
}


void includeGraphic( const JSON& aGraphic, BOUNDS& aBounds )
{
    const std::string kind = aGraphic.value( "kind", "" );

    if( kind == "circle" )
    {
        POINT center;
        const long double radius = aGraphic.value( "radiusNm", int64_t( 0 ) );

        if( point( aGraphic.value( "center", JSON::object() ), center ) && radius > 0.0L )
        {
            aBounds.Include( { center.x - radius, center.y - radius } );
            aBounds.Include( { center.x + radius, center.y + radius } );
        }
    }
    else if( kind == "arc" )
    {
        includeArc( aGraphic, aBounds );
    }
    else if( kind == "bezier" )
    {
        includeBezier( aGraphic, aBounds );
    }
    else if( kind == "polygon" )
    {
        for( const JSON& candidate : aGraphic.value( "points", JSON::array() ) )
        {
            POINT parsed;

            if( point( candidate, parsed ) )
                aBounds.Include( parsed );
        }
    }
    else
    {
        for( const char* name : { "start", "end" } )
        {
            POINT parsed;

            if( point( aGraphic.value( name, JSON::object() ), parsed ) )
                aBounds.Include( parsed );
        }
    }
}


long double routeLength( const JSON& aStatement )
{
    if( aStatement.value( "kind", "" ) == "arc" )
    {
        POINT start;
        POINT mid;
        POINT end;
        POINT center;
        long double radius = 0.0L;
        long double sweep = 0.0L;
        return arcGeometry( aStatement, start, mid, end, center, radius, sweep )
                       ? radius * sweep
                       : 0.0L;
    }

    POINT start;
    POINT end;
    return point( aStatement.value( "start", JSON::object() ), start )
                   && point( aStatement.value( "end", JSON::object() ), end )
                   ? distance( start, end )
                   : 0.0L;
}


void issue( RESULT& aResult, const std::string& aType, const std::string& aDescription,
            JSON aDetails = JSON::object() )
{
    JSON entry = { { "category", "layout" }, { "type", aType },
                   { "severity", "error" }, { "description", aDescription } };

    if( aDetails.is_object() )
        entry.update( aDetails );

    aResult.issues.push_back( std::move( entry ) );
}


std::string millimeters( long double aNanometers )
{
    return std::to_string( static_cast<double>( aNanometers / 1'000'000.0L ) );
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_LAYOUT_ANALYZER::RESULT DESIGN_SCRIPT_LAYOUT_ANALYZER::Analyze(
        const JSON& aCompilerIr )
{
    RESULT result;
    const JSON* layout = nullptr;
    BOUNDS outline;
    std::map<std::string, POINT> placements;
    std::map<std::string, long double> lengths;
    std::map<std::string, uint64_t> vias;
    std::map<std::string, std::vector<const JSON*>> routes;

    if( !aCompilerIr.is_object() || !aCompilerIr.contains( "pcb" )
        || !aCompilerIr["pcb"].is_array() )
    {
        issue( result, "invalid_layout_input", "Compiled KDS PCB intent is unavailable" );
        result.summary = { { "constraintCount", 0 }, { "issueCount", result.issues.size() } };
        return result;
    }

    for( const JSON& statement : aCompilerIr["pcb"] )
    {
        const std::string kind = statement.value( "kind", "" );

        if( kind == "layout" )
        {
            layout = &statement;
        }
        else if( kind == "outline_shape" )
        {
            includeGraphic( statement.value( "graphic", JSON::object() ), outline );
        }
        else if( kind == "place" )
        {
            POINT position;

            if( point( statement.value( "position", JSON::object() ), position ) )
                placements[statement.value( "component", "" )] = position;
        }
        else if( kind == "trace" || kind == "arc" )
        {
            const std::string net = statement.value( "net", "" );
            lengths[net] += routeLength( statement );
            routes[net].push_back( &statement );
        }
        else if( kind == "via" )
        {
            ++vias[statement.value( "net", "" )];
        }
    }

    if( layout == nullptr )
    {
        issue( result, "missing_layout_contract",
               "KDS has no physical layout acceptance contract",
               { { "recovery",
                   "Add one board (layout ...) declaration with board, placement, and routing intent." } } );
        result.summary = { { "constraintCount", 0 },
                           { "placementCount", placements.size() },
                           { "routedNetCount", routes.size() },
                           { "issueCount", result.issues.size() } };
        return result;
    }

    size_t constraintCount = 0;
    const JSON board = layout->value( "board", JSON::object() );

    if( !outline.Valid() )
    {
        issue( result, "missing_layout_outline",
               "Physical layout qualification requires a bounded board outline" );
    }
    else
    {
        const long double width = outline.maximumX - outline.minimumX;
        const long double height = outline.maximumY - outline.minimumY;
        const int64_t maximumWidth = board.value( "maximumWidthNm", int64_t( 0 ) );
        const int64_t maximumHeight = board.value( "maximumHeightNm", int64_t( 0 ) );
        constraintCount += 2;

        if( maximumWidth <= 0 || width > maximumWidth )
        {
            issue( result, "board_width_exceeded",
                   "Board width exceeds the authored physical-layout limit",
                   { { "actualMm", millimeters( width ) },
                     { "maximumMm", millimeters( maximumWidth ) } } );
        }

        if( maximumHeight <= 0 || height > maximumHeight )
        {
            issue( result, "board_height_exceeded",
                   "Board height exceeds the authored physical-layout limit",
                   { { "actualMm", millimeters( height ) },
                     { "maximumMm", millimeters( maximumHeight ) } } );
        }

        result.summary["board"] = {
            { "minimumXNm", static_cast<int64_t>( std::llround( outline.minimumX ) ) },
            { "minimumYNm", static_cast<int64_t>( std::llround( outline.minimumY ) ) },
            { "maximumXNm", static_cast<int64_t>( std::llround( outline.maximumX ) ) },
            { "maximumYNm", static_cast<int64_t>( std::llround( outline.maximumY ) ) },
            { "widthNm", static_cast<int64_t>( std::llround( width ) ) },
            { "heightNm", static_cast<int64_t>( std::llround( height ) ) }
        };
    }

    for( const JSON& constraint : layout->value( "placement", JSON::array() ) )
    {
        ++constraintCount;
        const std::string kind = constraint.value( "kind", "" );

        if( kind == "near" || kind == "align" )
        {
            const std::string first = constraint.value( "first", "" );
            const std::string second = constraint.value( "second", "" );

            if( !placements.contains( first ) || !placements.contains( second ) )
            {
                issue( result, "unmeasurable_placement_constraint",
                       "A layout relationship references an unplaced component",
                       { { "components", JSON::array( { first, second } ) } } );
                continue;
            }

            if( kind == "near" )
            {
                const long double actual = distance( placements[first], placements[second] );
                const int64_t maximum = constraint.value( "maximumNm", int64_t( 0 ) );

                if( maximum <= 0 || actual > maximum )
                {
                    issue( result, "component_proximity_exceeded",
                           "Component-anchor distance exceeds the authored proximity limit",
                           { { "components", JSON::array( { first, second } ) },
                             { "actualMm", millimeters( actual ) },
                             { "maximumMm", millimeters( maximum ) } } );
                }
            }
            else
            {
                const std::string axis = constraint.value( "axis", "" );
                const long double actual = axis == "x"
                                                   ? std::abs( placements[first].x
                                                               - placements[second].x )
                                                   : std::abs( placements[first].y
                                                               - placements[second].y );
                const int64_t tolerance = constraint.value( "toleranceNm", int64_t( 0 ) );

                if( actual > tolerance )
                {
                    issue( result, "component_alignment_exceeded",
                           "Component anchors exceed the authored alignment tolerance",
                           { { "components", JSON::array( { first, second } ) },
                             { "axis", axis }, { "actualMm", millimeters( actual ) },
                             { "toleranceMm", millimeters( tolerance ) } } );
                }
            }
        }
        else if( kind == "edge" )
        {
            const std::string component = constraint.value( "component", "" );

            if( !outline.Valid() || !placements.contains( component ) )
            {
                issue( result, "unmeasurable_edge_constraint",
                       "An edge relationship requires a placed component and board outline",
                       { { "component", component } } );
                continue;
            }

            const std::string edge = constraint.value( "edge", "" );
            const POINT position = placements[component];
            const long double actual = edge == "left" ? std::abs( position.x - outline.minimumX )
                                       : edge == "right" ? std::abs( outline.maximumX - position.x )
                                       : edge == "top" ? std::abs( position.y - outline.minimumY )
                                                       : std::abs( outline.maximumY - position.y );
            const int64_t maximum = constraint.value( "maximumNm", int64_t( 0 ) );

            if( actual > maximum )
            {
                issue( result, "component_edge_distance_exceeded",
                       "Component anchor is too far from its authored board edge",
                       { { "component", component }, { "edge", edge },
                         { "actualMm", millimeters( actual ) },
                         { "maximumMm", millimeters( maximum ) } } );
            }
        }
        else if( kind == "group" )
        {
            const JSON members = constraint.value( "members", JSON::array() );
            long double maximumActual = 0.0L;
            bool measurable = true;

            for( const JSON& member : members )
            {
                if( !member.is_string() || !placements.contains( member.get<std::string>() ) )
                    measurable = false;
            }

            if( measurable )
            {
                for( size_t first = 0; first < members.size(); ++first )
                {
                    for( size_t second = first + 1; second < members.size(); ++second )
                    {
                        maximumActual = std::max(
                                maximumActual,
                                distance( placements[members[first].get<std::string>()],
                                          placements[members[second].get<std::string>()] ) );
                    }
                }
            }
            else
            {
                issue( result, "unmeasurable_group_constraint",
                       "A layout group contains an unplaced component",
                       { { "group", constraint.value( "id", "" ) } } );
                continue;
            }

            const int64_t maximum = constraint.value( "maximumSpanNm", int64_t( 0 ) );

            if( maximumActual > maximum )
            {
                issue( result, "component_group_span_exceeded",
                       "Functional group span exceeds the authored placement limit",
                       { { "group", constraint.value( "id", "" ) },
                         { "actualMm", millimeters( maximumActual ) },
                         { "maximumMm", millimeters( maximum ) } } );
            }
        }
    }

    const JSON routing = layout->value( "routing", JSON::object() );
    const JSON defaults = routing.value( "defaults", JSON::object() );
    std::map<std::string, JSON> policies;

    for( const JSON& override : routing.value( "nets", JSON::array() ) )
        policies[override.value( "net", "" )] = override;

    std::set<std::string> routedNets;

    for( const auto& [net, statements] : routes )
        routedNets.emplace( net );

    for( const auto& [net, count] : vias )
        routedNets.emplace( net );

    for( const auto& [net, override] : policies )
        routedNets.emplace( net );

    for( const std::string& net : routedNets )
    {
        JSON policy = defaults;

        if( policies.contains( net ) )
        {
            for( auto entry = policies[net].begin(); entry != policies[net].end(); ++entry )
            {
                if( entry.key() != "net" )
                    policy[entry.key()] = entry.value();
            }
        }

        const std::string geometry = policy.value( "geometry", "any" );
        const uint64_t maximumVias = policy.value( "maximumVias", uint64_t( 0 ) );
        ++constraintCount;

        if( vias[net] > maximumVias )
        {
            issue( result, "net_via_limit_exceeded",
                   "Net uses more vias than its authored routing policy permits",
                   { { "net", net }, { "actual", vias[net] },
                     { "maximum", maximumVias } } );
        }

        if( policy.contains( "maximumLengthNm" ) )
        {
            ++constraintCount;
            const int64_t maximum = policy["maximumLengthNm"].get<int64_t>();

            if( lengths[net] > maximum )
            {
                issue( result, "net_length_limit_exceeded",
                       "Net copper length exceeds its authored routing policy",
                       { { "net", net }, { "actualMm", millimeters( lengths[net] ) },
                         { "maximumMm", millimeters( maximum ) } } );
            }
        }

        if( geometry == "any" )
            continue;

        for( const JSON* statement : routes[net] )
        {
            if( statement->value( "kind", "" ) == "arc" )
            {
                issue( result, "net_geometry_violation",
                       "Arc route violates the authored straight-segment geometry policy",
                       { { "net", net }, { "logicalId", statement->value( "logicalId", "" ) },
                         { "geometry", geometry } } );
                continue;
            }

            POINT start;
            POINT end;

            if( !point( statement->value( "start", JSON::object() ), start )
                || !point( statement->value( "end", JSON::object() ), end ) )
            {
                continue;
            }

            const long double dx = std::abs( end.x - start.x );
            const long double dy = std::abs( end.y - start.y );
            const bool orthogonal = dx == 0.0L || dy == 0.0L;
            const bool octilinear = orthogonal || dx == dy;
            const bool accepted = geometry == "orthogonal" ? orthogonal : octilinear;

            if( !accepted )
            {
                issue( result, "net_geometry_violation",
                       "Route segment violates the authored routing geometry policy",
                       { { "net", net }, { "logicalId", statement->value( "logicalId", "" ) },
                         { "geometry", geometry } } );
            }
        }
    }

    for( const JSON& bundle : routing.value( "bundles", JSON::array() ) )
    {
        ++constraintCount;
        long double minimum = std::numeric_limits<long double>::infinity();
        long double maximum = 0.0L;

        for( const JSON& net : bundle.value( "nets", JSON::array() ) )
        {
            const long double length = lengths[net.get<std::string>()];
            minimum = std::min( minimum, length );
            maximum = std::max( maximum, length );
        }

        const long double skew = std::isfinite( minimum ) ? maximum - minimum : 0.0L;
        const int64_t maximumSkew = bundle.value( "maximumSkewNm", int64_t( 0 ) );

        if( skew > maximumSkew )
        {
            issue( result, "routing_bundle_skew_exceeded",
                   "Routing bundle length skew exceeds its authored limit",
                   { { "bundle", bundle.value( "id", "" ) },
                     { "actualMm", millimeters( skew ) },
                     { "maximumMm", millimeters( maximumSkew ) } } );
        }
    }

    JSON netMeasurements = JSON::array();

    for( const std::string& net : routedNets )
    {
        netMeasurements.push_back(
                { { "net", net },
                  { "lengthNm", static_cast<int64_t>( std::llround( lengths[net] ) ) },
                  { "vias", vias[net] }, { "segments", routes[net].size() } } );
    }

    result.summary["constraintCount"] = constraintCount;
    result.summary["placementCount"] = placements.size();
    result.summary["routedNetCount"] = routedNets.size();
    result.summary["nets"] = std::move( netMeasurements );
    result.summary["issueCount"] = result.issues.size();
    result.clean = result.issues.empty();
    return result;
}

} // namespace KICHAD
