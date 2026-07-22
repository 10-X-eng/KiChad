/*
 * This program source code file is part of KiChad, a Codex-integrated downstream of KiCad.
 *
 * Copyright (C) 2026 KiChad Developers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "design_script_physical_synthesizer.h"
#include "kichad_from_chars.h"

#include "lossless_sexpr_document.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>


namespace
{

using JSON = nlohmann::json;
using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;

constexpr size_t MAX_GRID_CELLS = 4'000'000;
constexpr size_t MAX_PLACEMENT_CANDIDATES = 8'000'000;
constexpr size_t MAX_ROUTE_EXPANSIONS = 2'000'000;


struct POINT
{
    int64_t x = 0;
    int64_t y = 0;

    auto operator<=>( const POINT& ) const = default;
};


struct BOX
{
    int64_t minimumX = std::numeric_limits<int64_t>::max();
    int64_t minimumY = std::numeric_limits<int64_t>::max();
    int64_t maximumX = std::numeric_limits<int64_t>::min();
    int64_t maximumY = std::numeric_limits<int64_t>::min();

    bool Valid() const
    {
        return minimumX <= maximumX && minimumY <= maximumY;
    }

    void Include( const POINT& aPoint )
    {
        minimumX = std::min( minimumX, aPoint.x );
        minimumY = std::min( minimumY, aPoint.y );
        maximumX = std::max( maximumX, aPoint.x );
        maximumY = std::max( maximumY, aPoint.y );
    }

    BOX Inflated( int64_t aDistance ) const
    {
        return { minimumX - aDistance, minimumY - aDistance,
                 maximumX + aDistance, maximumY + aDistance };
    }

    bool Contains( const POINT& aPoint ) const
    {
        return aPoint.x >= minimumX && aPoint.x <= maximumX
               && aPoint.y >= minimumY && aPoint.y <= maximumY;
    }

    bool Intersects( const BOX& aOther ) const
    {
        return minimumX <= aOther.maximumX && maximumX >= aOther.minimumX
               && minimumY <= aOther.maximumY && maximumY >= aOther.minimumY;
    }
};


struct PAD
{
    std::string number;
    POINT       position;
    int64_t     width = 0;
    int64_t     height = 0;
};


struct FOOTPRINT
{
    BOX                   padBounds;
    BOX                   frontCourtyard;
    BOX                   backCourtyard;
    std::map<std::string, PAD> pads;
};


struct PLACEMENT
{
    POINT position;
    int   rotation = 0;
    bool  back = false;
    BOX   bounds;
};


bool nativeMillimetres( const std::string& aText, int64_t& aNanometers )
{
    long double value = 0.0L;
    const std::from_chars_result parsed =
            KICHAD::FromChars( aText.data(), aText.data() + aText.size(), value );

    if( parsed.ec != std::errc() || parsed.ptr != aText.data() + aText.size()
        || !std::isfinite( value ) )
    {
        return false;
    }

    const long double converted = std::round( value * 1.0e6L );

    if( !std::isfinite( converted ) || converted < -1.0e12L || converted > 1.0e12L )
        return false;

    aNanometers = static_cast<int64_t>( converted );
    return true;
}


std::vector<size_t> directLists( const DOCUMENT& aDocument, size_t aParent,
                                 const std::string& aHead )
{
    std::vector<size_t> result;

    for( size_t child : aDocument.Nodes().at( aParent ).children )
    {
        if( aDocument.Nodes().at( child ).kind == DOCUMENT::NODE_KIND::LIST
            && aDocument.ListHead( child ) == aHead )
        {
            result.push_back( child );
        }
    }

    return result;
}


bool parseFootprint( const std::string& aSource, FOOTPRINT& aFootprint,
                     std::string& aError )
{
    std::string parseError;
    std::unique_ptr<DOCUMENT> document = DOCUMENT::Parse( aSource, &parseError );

    if( !document || document->Roots().size() != 1
        || document->ListHead( document->Roots().front() ) != "footprint" )
    {
        aError = parseError.empty() ? "footprint source has no footprint root" : parseError;
        return false;
    }

    for( size_t padNode : directLists( *document, document->Roots().front(), "pad" ) )
    {
        const DOCUMENT::NODE& pad = document->Nodes().at( padNode );
        std::string number;

        if( pad.children.size() < 4
            || document->Nodes().at( pad.children[1] ).kind == DOCUMENT::NODE_KIND::LIST )
        {
            aError = "footprint contains a malformed pad";
            return false;
        }

        number = document->AtomText( pad.children[1] );
        const std::vector<size_t> positions = directLists( *document, padNode, "at" );
        const std::vector<size_t> sizes = directLists( *document, padNode, "size" );

        if( number.empty() || positions.size() != 1 || sizes.size() != 1 )
        {
            aError = "footprint pads require unique numbers, positions, and sizes";
            return false;
        }

        const DOCUMENT::NODE& position = document->Nodes().at( positions.front() );
        const DOCUMENT::NODE& size = document->Nodes().at( sizes.front() );
        PAD parsed;
        parsed.number = number;

        if( position.children.size() < 3 || size.children.size() != 3
            || document->Nodes().at( position.children[1] ).kind == DOCUMENT::NODE_KIND::LIST
            || document->Nodes().at( position.children[2] ).kind == DOCUMENT::NODE_KIND::LIST
            || document->Nodes().at( size.children[1] ).kind == DOCUMENT::NODE_KIND::LIST
            || document->Nodes().at( size.children[2] ).kind == DOCUMENT::NODE_KIND::LIST
            || !nativeMillimetres( document->AtomText( position.children[1] ), parsed.position.x )
            || !nativeMillimetres( document->AtomText( position.children[2] ), parsed.position.y )
            || !nativeMillimetres( document->AtomText( size.children[1] ), parsed.width )
            || !nativeMillimetres( document->AtomText( size.children[2] ), parsed.height )
            || parsed.width <= 0 || parsed.height <= 0
            || !aFootprint.pads.emplace( number, parsed ).second )
        {
            aError = "footprint contains an invalid or duplicate pad";
            return false;
        }

        aFootprint.padBounds.Include( { parsed.position.x - parsed.width / 2,
                                        parsed.position.y - parsed.height / 2 } );
        aFootprint.padBounds.Include( { parsed.position.x + parsed.width / 2,
                                        parsed.position.y + parsed.height / 2 } );
    }

    if( aFootprint.pads.empty() || !aFootprint.padBounds.Valid() )
    {
        aError = "footprint synthesis requires at least one bounded pad";
        return false;
    }

    const size_t root = document->Roots().front();

    for( size_t child : document->Nodes().at( root ).children )
    {
        const std::string head = document->ListHead( child );

        if( !head.starts_with( "fp_" ) )
            continue;

        const std::vector<size_t> layers = directLists( *document, child, "layer" );

        if( layers.size() != 1 || document->Nodes().at( layers.front() ).children.size() != 2 )
            continue;

        const std::string layer = document->AtomText(
                document->Nodes().at( layers.front() ).children[1] );
        BOX* courtyard = layer == "F.CrtYd" ? &aFootprint.frontCourtyard
                         : layer == "B.CrtYd" ? &aFootprint.backCourtyard : nullptr;

        if( !courtyard )
            continue;

        if( head == "fp_circle" )
        {
            const std::vector<size_t> centers = directLists( *document, child, "center" );
            const std::vector<size_t> ends = directLists( *document, child, "end" );

            if( centers.size() == 1 && ends.size() == 1
                && document->Nodes().at( centers.front() ).children.size() >= 3
                && document->Nodes().at( ends.front() ).children.size() >= 3 )
            {
                POINT center;
                POINT end;

                if( nativeMillimetres( document->AtomText(
                                               document->Nodes().at( centers.front() ).children[1] ),
                                       center.x )
                    && nativeMillimetres( document->AtomText(
                                                  document->Nodes().at( centers.front() ).children[2] ),
                                          center.y )
                    && nativeMillimetres( document->AtomText(
                                                  document->Nodes().at( ends.front() ).children[1] ),
                                          end.x )
                    && nativeMillimetres( document->AtomText(
                                                  document->Nodes().at( ends.front() ).children[2] ),
                                          end.y ) )
                {
                    const int64_t radius = static_cast<int64_t>( std::ceil( std::hypot(
                            static_cast<long double>( end.x - center.x ),
                            static_cast<long double>( end.y - center.y ) ) ) );
                    courtyard->Include( { center.x - radius, center.y - radius } );
                    courtyard->Include( { center.x + radius, center.y + radius } );
                }
            }
        }

        const auto includeCoordinates = [&]( const auto& self, size_t aNode ) -> void
        {
            const DOCUMENT::NODE& node = document->Nodes().at( aNode );
            const std::string coordinateHead = document->ListHead( aNode );

            if( ( coordinateHead == "start" || coordinateHead == "end"
                  || coordinateHead == "mid" || coordinateHead == "center"
                  || coordinateHead == "xy" )
                && node.children.size() >= 3 )
            {
                int64_t x = 0;
                int64_t y = 0;

                if( nativeMillimetres( document->AtomText( node.children[1] ), x )
                    && nativeMillimetres( document->AtomText( node.children[2] ), y ) )
                {
                    courtyard->Include( { x, y } );
                }
            }

            for( size_t nested : node.children )
            {
                if( document->Nodes().at( nested ).kind == DOCUMENT::NODE_KIND::LIST )
                    self( self, nested );
            }
        };
        includeCoordinates( includeCoordinates, child );
    }

    if( !aFootprint.frontCourtyard.Valid() )
        aFootprint.frontCourtyard = aFootprint.padBounds;

    if( !aFootprint.backCourtyard.Valid() )
        aFootprint.backCourtyard = aFootprint.padBounds;

    return true;
}


POINT rotate( const POINT& aPoint, int aDegrees )
{
    switch( ( aDegrees % 360 + 360 ) % 360 )
    {
    case 90: return { -aPoint.y, aPoint.x };
    case 180: return { -aPoint.x, -aPoint.y };
    case 270: return { aPoint.y, -aPoint.x };
    default: return aPoint;
    }
}


BOX placedBounds( const FOOTPRINT& aFootprint, const POINT& aPosition, int aRotation,
                  bool aBack )
{
    BOX result;
    const BOX& source = aBack ? aFootprint.backCourtyard : aFootprint.frontCourtyard;

    for( const POINT& corner :
         { POINT{ source.minimumX, source.minimumY },
           POINT{ source.minimumX, source.maximumY },
           POINT{ source.maximumX, source.minimumY },
           POINT{ source.maximumX, source.maximumY } } )
    {
        const POINT transformed = rotate( corner, aRotation );
        result.Include( { transformed.x + aPosition.x, transformed.y + aPosition.y } );
    }

    return result;
}


bool jsonPoint( const JSON& aValue, POINT& aPoint )
{
    if( !aValue.is_object() || !aValue.contains( "xNm" )
        || !aValue["xNm"].is_number_integer() || !aValue.contains( "yNm" )
        || !aValue["yNm"].is_number_integer() )
    {
        return false;
    }

    aPoint = { aValue["xNm"].get<int64_t>(), aValue["yNm"].get<int64_t>() };
    return true;
}


void includeJsonPoints( const JSON& aValue, BOX& aBounds )
{
    if( aValue.is_object() )
    {
        if( aValue.contains( "xNm" ) && aValue["xNm"].is_number_integer()
            && aValue.contains( "yNm" ) && aValue["yNm"].is_number_integer() )
        {
            aBounds.Include( { aValue["xNm"].get<int64_t>(),
                               aValue["yNm"].get<int64_t>() } );
        }

        for( const auto& [name, child] : aValue.items() )
        {
            (void) name;
            includeJsonPoints( child, aBounds );
        }
    }
    else if( aValue.is_array() )
    {
        for( const JSON& child : aValue )
            includeJsonPoints( child, aBounds );
    }
}


int64_t snapUp( int64_t aValue, int64_t aOrigin, int64_t aGrid )
{
    const int64_t delta = aValue - aOrigin;
    const int64_t quotient = delta / aGrid;
    const int64_t remainder = delta % aGrid;
    return aOrigin + ( quotient + ( remainder > 0 ? 1 : 0 ) ) * aGrid;
}


POINT snap( const POINT& aPoint, const BOX& aBoard, int64_t aGrid )
{
    const auto coordinate = [&]( int64_t aValue, int64_t aOrigin )
    {
        return aOrigin + static_cast<int64_t>( std::llround(
                                   static_cast<long double>( aValue - aOrigin ) / aGrid ) )
                         * aGrid;
    };
    return { coordinate( aPoint.x, aBoard.minimumX ),
             coordinate( aPoint.y, aBoard.minimumY ) };
}


void occupyBox( const BOX& aBox, const BOX& aBoard, int64_t aGrid,
                std::set<POINT>& aOccupied )
{
    if( !aBox.Valid() )
        return;

    const int64_t minimumX = std::max( aBoard.minimumX,
                                      snapUp( aBox.minimumX, aBoard.minimumX, aGrid ) );
    const int64_t minimumY = std::max( aBoard.minimumY,
                                      snapUp( aBox.minimumY, aBoard.minimumY, aGrid ) );
    const int64_t maximumX = std::min( aBoard.maximumX, aBox.maximumX );
    const int64_t maximumY = std::min( aBoard.maximumY, aBox.maximumY );

    for( int64_t y = minimumY; y <= maximumY; y += aGrid )
        for( int64_t x = minimumX; x <= maximumX; x += aGrid )
            aOccupied.insert( { x, y } );
}


void occupySegment( const POINT& aStart, const POINT& aEnd, int64_t aRadius,
                    const BOX& aBoard, int64_t aGrid, std::set<POINT>& aOccupied )
{
    const BOX candidates = { std::min( aStart.x, aEnd.x ) - aRadius,
                             std::min( aStart.y, aEnd.y ) - aRadius,
                             std::max( aStart.x, aEnd.x ) + aRadius,
                             std::max( aStart.y, aEnd.y ) + aRadius };
    const int64_t minimumX = std::max( aBoard.minimumX,
                                      snapUp( candidates.minimumX, aBoard.minimumX, aGrid ) );
    const int64_t minimumY = std::max( aBoard.minimumY,
                                      snapUp( candidates.minimumY, aBoard.minimumY, aGrid ) );
    const int64_t maximumX = std::min( aBoard.maximumX, candidates.maximumX );
    const int64_t maximumY = std::min( aBoard.maximumY, candidates.maximumY );
    const long double deltaX = static_cast<long double>( aEnd.x - aStart.x );
    const long double deltaY = static_cast<long double>( aEnd.y - aStart.y );
    const long double lengthSquared = deltaX * deltaX + deltaY * deltaY;
    const long double radiusSquared = static_cast<long double>( aRadius ) * aRadius;

    for( int64_t y = minimumY; y <= maximumY; y += aGrid )
    {
        for( int64_t x = minimumX; x <= maximumX; x += aGrid )
        {
            long double fraction = 0.0L;

            if( lengthSquared > 0.0L )
            {
                fraction = ( static_cast<long double>( x - aStart.x ) * deltaX
                             + static_cast<long double>( y - aStart.y ) * deltaY )
                           / lengthSquared;
                fraction = std::clamp( fraction, 0.0L, 1.0L );
            }

            const long double nearestX = aStart.x + fraction * deltaX;
            const long double nearestY = aStart.y + fraction * deltaY;
            const long double distanceX = x - nearestX;
            const long double distanceY = y - nearestY;

            if( distanceX * distanceX + distanceY * distanceY <= radiusSquared )
                aOccupied.insert( { x, y } );
        }
    }
}


std::vector<POINT> routePath( const POINT& aStart, const POINT& aEnd, const BOX& aBoard,
                              int64_t aGrid, const std::vector<BOX>& aObstacles,
                              const std::set<POINT>& aOccupied )
{
    const POINT start = snap( aStart, aBoard, aGrid );
    const POINT end = snap( aEnd, aBoard, aGrid );
    const int64_t columns = ( aBoard.maximumX - aBoard.minimumX ) / aGrid + 1;
    const int64_t rows = ( aBoard.maximumY - aBoard.minimumY ) / aGrid + 1;

    if( columns <= 0 || rows <= 0
        || static_cast<uint64_t>( columns ) * static_cast<uint64_t>( rows ) > MAX_GRID_CELLS )
    {
        return {};
    }

    const auto key = [&]( const POINT& point )
    {
        return std::pair{ ( point.x - aBoard.minimumX ) / aGrid,
                          ( point.y - aBoard.minimumY ) / aGrid };
    };
    const auto point = [&]( const std::pair<int64_t, int64_t>& cell )
    {
        return POINT{ aBoard.minimumX + cell.first * aGrid,
                      aBoard.minimumY + cell.second * aGrid };
    };
    using CELL = std::pair<int64_t, int64_t>;
    using QUEUED = std::tuple<int64_t, int64_t, CELL>;
    std::priority_queue<QUEUED, std::vector<QUEUED>, std::greater<>> open;
    std::map<CELL, int64_t> costs;
    std::map<CELL, CELL> parents;
    const CELL startCell = key( start );
    const CELL endCell = key( end );
    costs[startCell] = 0;
    open.emplace( std::abs( endCell.first - startCell.first )
                          + std::abs( endCell.second - startCell.second ),
                  0, startCell );
    size_t expansions = 0;

    while( !open.empty() && expansions++ < MAX_ROUTE_EXPANSIONS )
    {
        const auto [priority, cost, current] = open.top();
        (void) priority;
        open.pop();

        if( costs[current] != cost )
            continue;

        if( current == endCell )
            break;

        for( const CELL& delta : { CELL{ 1, 0 }, CELL{ 0, 1 }, CELL{ -1, 0 }, CELL{ 0, -1 } } )
        {
            const CELL next{ current.first + delta.first, current.second + delta.second };

            if( next.first < 0 || next.second < 0 || next.first >= columns || next.second >= rows )
                continue;

            const POINT nextPoint = point( next );
            bool blocked = next != endCell && next != startCell && aOccupied.contains( nextPoint );

            for( const BOX& obstacle : aObstacles )
                blocked = blocked || ( next != endCell && next != startCell
                                        && obstacle.Contains( nextPoint ) );

            if( blocked )
                continue;

            const int64_t nextCost = cost + 1;

            if( !costs.contains( next ) || nextCost < costs[next] )
            {
                costs[next] = nextCost;
                parents[next] = current;
                const int64_t heuristic = std::abs( endCell.first - next.first )
                                          + std::abs( endCell.second - next.second );
                open.emplace( nextCost + heuristic, nextCost, next );
            }
        }
    }

    if( !costs.contains( endCell ) )
        return {};

    std::vector<POINT> reversed = { point( endCell ) };
    CELL cursor = endCell;

    while( cursor != startCell )
    {
        cursor = parents.at( cursor );
        reversed.push_back( point( cursor ) );
    }

    std::reverse( reversed.begin(), reversed.end() );
    std::vector<POINT> compressed;

    for( const POINT& routePoint : reversed )
    {
        if( compressed.size() >= 2 )
        {
            const POINT& first = compressed[compressed.size() - 2];
            const POINT& second = compressed.back();

            if( ( first.x == second.x && second.x == routePoint.x )
                || ( first.y == second.y && second.y == routePoint.y ) )
            {
                compressed.back() = routePoint;
                continue;
            }
        }

        compressed.push_back( routePoint );
    }

    std::vector<POINT> exact;
    const auto append = [&]( const POINT& aPoint )
    {
        if( exact.empty() || exact.back() != aPoint )
            exact.push_back( aPoint );
    };
    append( aStart );

    if( compressed.front() != aStart )
        append( { compressed.front().x, aStart.y } );

    for( const POINT& routePoint : compressed )
        append( routePoint );

    if( compressed.back() != aEnd )
        append( { aEnd.x, compressed.back().y } );

    append( aEnd );
    compressed.clear();

    for( const POINT& routePoint : exact )
    {
        if( compressed.size() >= 2 )
        {
            const POINT& first = compressed[compressed.size() - 2];
            const POINT& second = compressed.back();

            if( ( first.x == second.x && second.x == routePoint.x )
                || ( first.y == second.y && second.y == routePoint.y ) )
            {
                compressed.back() = routePoint;
                continue;
            }
        }

        compressed.push_back( routePoint );
    }

    return compressed;
}

} // namespace


KICHAD::DESIGN_SCRIPT_PHYSICAL_SYNTHESIZER::RESULT
KICHAD::DESIGN_SCRIPT_PHYSICAL_SYNTHESIZER::Synthesize(
        const JSON& aCompilerIr, const JSON& aFootprintSources )
{
    RESULT result;
    result.ir = aCompilerIr;

    if( !aCompilerIr.is_object() || !aCompilerIr.contains( "synthesis" )
        || !aCompilerIr["synthesis"].is_object() )
    {
        result.ok = true;
        result.summary = { { "placements", 0 }, { "routes", 0 } };
        return result;
    }

    BOX board;
    size_t outlineShapes = 0;

    for( const JSON& statement : aCompilerIr.value( "pcb", JSON::array() ) )
    {
        if( statement.value( "kind", "" ) == "outline_shape" )
        {
            ++outlineShapes;

            if( statement.value( "graphic", JSON::object() ).value( "kind", "" )
                != "rectangle" )
            {
                result.error = "physical synthesis currently requires one rectangular board outline; author exact placement and copper for curved, cutout, or multi-island boards";
                return result;
            }

            includeJsonPoints( statement.value( "graphic", JSON::object() ), board );
        }
    }

    if( outlineShapes != 1 || !board.Valid() || board.maximumX == board.minimumX
        || board.maximumY == board.minimumY )
    {
        result.error = "physical synthesis requires exactly one bounded rectangular board outline";
        return result;
    }

    std::map<std::string, const JSON*> components;
    std::map<std::string, FOOTPRINT> footprints;

    for( const JSON& component : aCompilerIr["schematic"]["components"] )
    {
        if( !component.is_object() || !component.contains( "reference" )
            || !component["reference"].is_string() || !component.contains( "footprint" )
            || !component["footprint"].is_string() )
        {
            continue;
        }

        const std::string reference = component["reference"].get<std::string>();
        const std::string libraryId = component["footprint"].get<std::string>();
        components[reference] = &component;

        if( !aFootprintSources.is_object() || !aFootprintSources.contains( libraryId )
            || !aFootprintSources[libraryId].is_string() )
        {
            result.error = "physical synthesis requires inventoried footprint " + libraryId;
            return result;
        }

        if( !footprints.contains( libraryId )
            && !parseFootprint( aFootprintSources[libraryId].get<std::string>(),
                                footprints[libraryId], result.error ) )
        {
            result.error = libraryId + ": " + result.error;
            return result;
        }
    }

    std::map<std::string, PLACEMENT> placements;
    std::vector<BOX> occupied;

    for( const JSON& statement : result.ir["pcb"] )
    {
        if( statement.value( "kind", "" ) != "place" )
            continue;

        const std::string reference = statement.value( "component", "" );

        if( !components.contains( reference ) )
            continue;

        const std::string libraryId =
                components[reference]->at( "footprint" ).get<std::string>();
        const POINT position{ statement["position"]["xNm"].get<int64_t>(),
                              statement["position"]["yNm"].get<int64_t>() };
        const int rotation = static_cast<int>( std::llround(
                statement.value( "rotationDegrees", 0.0 ) ) );
        const bool back = statement.value( "side", "front" ) == "back";
        const BOX bounds = placedBounds( footprints.at( libraryId ), position, rotation, back );
        placements[reference] = { position, rotation, back, bounds };
        occupied.push_back( bounds );
    }

    size_t synthesizedPlacements = 0;
    const JSON placementPolicy = aCompilerIr["synthesis"].value( "placement", JSON( nullptr ) );

    if( placementPolicy.is_object() )
    {
        const int64_t grid = placementPolicy.value( "gridNm", int64_t( 1'000'000 ) );
        const int64_t clearance = placementPolicy.value( "clearanceNm", int64_t( 500'000 ) );
        const int64_t edge = placementPolicy.value( "edgeClearanceNm", int64_t( 1'000'000 ) );
        const BOX usable = { board.minimumX + edge, board.minimumY + edge,
                             board.maximumX - edge, board.maximumY - edge };
        std::map<std::string, std::set<std::string>> connected;

        for( const JSON& net : aCompilerIr["schematic"]["nets"] )
        {
            std::vector<std::string> references;

            for( const JSON& pin : net.value( "pins", JSON::array() ) )
            {
                const std::string reference = pin.value( "component", "" );

                if( components.contains( reference ) )
                    references.push_back( reference );
            }

            for( const std::string& first : references )
                for( const std::string& second : references )
                    if( first != second )
                        connected[first].insert( second );
        }

        std::vector<std::string> pending;

        for( const auto& [reference, component] : components )
            if( !placements.contains( reference ) )
                pending.push_back( reference );

        std::sort( pending.begin(), pending.end(), [&]( const std::string& aLeft,
                                                       const std::string& aRight )
                   {
                       if( connected[aLeft].size() != connected[aRight].size() )
                           return connected[aLeft].size() > connected[aRight].size();

                       return aLeft < aRight;
                   } );

        const int64_t firstX = snapUp( usable.minimumX, board.minimumX, grid );
        const int64_t firstY = snapUp( usable.minimumY, board.minimumY, grid );
        const long double columns = firstX <= usable.maximumX
                                            ? static_cast<long double>( usable.maximumX - firstX )
                                                              / grid
                                                      + 1.0L
                                            : 0.0L;
        const long double rows = firstY <= usable.maximumY
                                         ? static_cast<long double>( usable.maximumY - firstY )
                                                           / grid
                                                   + 1.0L
                                         : 0.0L;
        const long double candidates = columns * rows * pending.size()
                                       * placementPolicy["rotationsDegrees"].size();

        if( !usable.Valid() || columns <= 0.0L || rows <= 0.0L
            || candidates > MAX_PLACEMENT_CANDIDATES )
        {
            result.error = "placement synthesis search is empty or exceeds 8000000 bounded candidates; enlarge/coarsen the grid or place components explicitly";
            return result;
        }

        for( const std::string& reference : pending )
        {
            const JSON& component = *components.at( reference );
            const std::string libraryId = component["footprint"].get<std::string>();
            const FOOTPRINT& footprint = footprints.at( libraryId );
            bool found = false;
            int64_t bestScore = std::numeric_limits<int64_t>::max();
            PLACEMENT best;

            for( const JSON& rotationJson : placementPolicy["rotationsDegrees"] )
            {
                const int rotation = rotationJson.get<int>();

                for( int64_t y = snapUp( usable.minimumY, board.minimumY, grid );
                     y <= usable.maximumY; y += grid )
                {
                    for( int64_t x = snapUp( usable.minimumX, board.minimumX, grid );
                         x <= usable.maximumX; x += grid )
                    {
                        const POINT candidate{ x, y };
                        const BOX bounds = placedBounds( footprint, candidate, rotation, false );

                        if( bounds.minimumX < usable.minimumX || bounds.maximumX > usable.maximumX
                            || bounds.minimumY < usable.minimumY
                            || bounds.maximumY > usable.maximumY )
                        {
                            continue;
                        }

                        bool collision = false;

                        for( const BOX& existing : occupied )
                            collision = collision || bounds.Inflated( clearance ).Intersects( existing );

                        if( collision )
                            continue;

                        int64_t score = ( y - board.minimumY ) + ( x - board.minimumX );
                        bool hasConnected = false;

                        for( const std::string& neighbor : connected[reference] )
                        {
                            if( placements.contains( neighbor ) )
                            {
                                hasConnected = true;
                                score += std::abs( x - placements[neighbor].position.x )
                                         + std::abs( y - placements[neighbor].position.y );
                            }
                        }

                        if( !hasConnected )
                            score *= 2;

                        if( !found || score < bestScore )
                        {
                            found = true;
                            bestScore = score;
                            best = { candidate, rotation, false, bounds };
                        }
                    }
                }
            }

            if( !found )
            {
                result.error = "placement synthesis could not fit component " + reference
                               + "; enlarge the outline, reduce clearance, or place it explicitly";
                return result;
            }

            placements[reference] = best;
            occupied.push_back( best.bounds );
            result.ir["pcb"].push_back(
                    { { "kind", "place" }, { "component", reference },
                      { "position", { { "xNm", best.position.x }, { "yNm", best.position.y } } },
                      { "rotationDegrees", best.rotation }, { "side", "front" },
                      { "locked", false }, { "presentation", JSON::object() },
                      { "typed", true }, { "synthesized", true } } );
            ++synthesizedPlacements;
        }
    }

    size_t synthesizedRoutes = 0;
    const JSON routingPolicy = aCompilerIr["synthesis"].value( "routing", JSON( nullptr ) );

    if( routingPolicy.is_object() )
    {
        const int64_t grid = routingPolicy.value( "gridNm", int64_t( 250'000 ) );
        const int64_t clearance = routingPolicy.value( "clearanceNm", int64_t( 200'000 ) );
        const int64_t width = routingPolicy.value( "widthNm", int64_t( 250'000 ) );
        const std::string layer = routingPolicy.value( "layer", "F.Cu" );
        const long double columns = static_cast<long double>( board.maximumX - board.minimumX )
                                            / grid
                                    + 1.0L;
        const long double rows = static_cast<long double>( board.maximumY - board.minimumY )
                                         / grid
                                 + 1.0L;

        if( columns <= 0.0L || rows <= 0.0L || columns * rows > MAX_GRID_CELLS )
        {
            result.error = "routing synthesis grid exceeds 4000000 bounded cells; use a coarser grid or author exact copper";
            return result;
        }

        std::set<std::string> explicitlyRouted;

        for( const JSON& statement : result.ir["pcb"] )
        {
            if( statement.value( "kind", "" ) == "trace"
                || statement.value( "kind", "" ) == "arc"
                || statement.value( "kind", "" ) == "via" )
            {
                explicitlyRouted.emplace( statement.value( "net", "" ) );
            }
        }

        std::set<POINT> occupiedRouteCells;
        size_t routeId = 0;

        for( const JSON& statement : result.ir["pcb"] )
        {
            const std::string kind = statement.value( "kind", "" );

            if( ( kind == "trace" || kind == "arc" )
                && statement.value( "layer", "" ) == layer )
            {
                POINT start;
                POINT end;

                if( !jsonPoint( statement.value( "start", JSON::object() ), start )
                    || !jsonPoint( statement.value( "end", JSON::object() ), end ) )
                {
                    result.error = "existing copper contains malformed route geometry";
                    return result;
                }

                if( kind == "trace" )
                {
                    occupySegment( start, end,
                                   clearance + statement.value( "widthNm", int64_t( 0 ) ) / 2
                                           + width / 2,
                                   board, grid, occupiedRouteCells );
                }
                else
                {
                    BOX arcBounds;
                    POINT mid;
                    arcBounds.Include( start );
                    arcBounds.Include( end );

                    if( jsonPoint( statement.value( "mid", JSON::object() ), mid ) )
                        arcBounds.Include( mid );

                    occupyBox( arcBounds.Inflated(
                                       clearance
                                       + statement.value( "widthNm", int64_t( 0 ) ) / 2
                                       + width / 2 ),
                               board, grid, occupiedRouteCells );
                }
            }
            else if( kind == "via" )
            {
                POINT position;

                if( jsonPoint( statement.value( "position", JSON::object() ), position ) )
                {
                    const int64_t diameter = std::max(
                            statement.value( "diameterNm", int64_t( 0 ) ),
                            statement.value( "drillNm", int64_t( 0 ) ) );
                    occupyBox( BOX{ position.x, position.y, position.x, position.y }
                                       .Inflated( clearance + diameter / 2 + width / 2 ),
                               board, grid, occupiedRouteCells );
                }
            }
            else if( kind == "keepout" )
            {
                bool blocksLayer = false;

                for( const JSON& keepoutLayer : statement.value( "layers", JSON::array() ) )
                    blocksLayer = blocksLayer || keepoutLayer == layer;

                if( blocksLayer
                    && statement.value( "prohibitions", JSON::object() )
                               .value( "tracks", false ) )
                {
                    BOX keepoutBounds;
                    includeJsonPoints( statement, keepoutBounds );
                    occupyBox( keepoutBounds.Inflated( width / 2 ), board, grid,
                               occupiedRouteCells );
                }
            }
        }

        std::vector<const JSON*> routedNets;

        for( const JSON& net : aCompilerIr["schematic"]["nets"] )
            routedNets.push_back( &net );

        std::sort( routedNets.begin(), routedNets.end(), []( const JSON* aLeft,
                                                             const JSON* aRight )
                   {
                       return aLeft->value( "name", "" ) < aRight->value( "name", "" );
                   } );

        for( const JSON* netPointer : routedNets )
        {
            const JSON& net = *netPointer;
            const std::string netName = net.value( "name", "" );

            if( explicitlyRouted.contains( netName ) )
                continue;

            struct ENDPOINT
            {
                POINT point;
                std::string component;
            };
            std::vector<ENDPOINT> endpoints;

            for( const JSON& pin : net.value( "pins", JSON::array() ) )
            {
                const std::string reference = pin.value( "component", "" );
                const std::string number = pin.value( "number", "" );

                if( !components.contains( reference ) || !placements.contains( reference ) )
                    continue;

                const std::string libraryId =
                        components[reference]->at( "footprint" ).get<std::string>();
                const FOOTPRINT& footprint = footprints.at( libraryId );
                const auto pad = footprint.pads.find( number );

                if( pad == footprint.pads.end() )
                {
                    result.error = "routing synthesis could not map " + reference + " pin "
                                   + number + " to footprint " + libraryId;
                    return result;
                }

                const PLACEMENT& placement = placements.at( reference );
                const POINT transformed = rotate( pad->second.position, placement.rotation );
                endpoints.push_back( { { placement.position.x + transformed.x,
                                         placement.position.y + transformed.y },
                                       reference } );
            }

            if( endpoints.size() < 2 )
                continue;

            std::sort( endpoints.begin(), endpoints.end(), []( const ENDPOINT& aLeft,
                                                               const ENDPOINT& aRight )
                       {
                           return std::tie( aLeft.component, aLeft.point.x, aLeft.point.y )
                                  < std::tie( aRight.component, aRight.point.x,
                                              aRight.point.y );
                       } );

            for( size_t endpointIndex = 1; endpointIndex < endpoints.size(); ++endpointIndex )
            {
                std::vector<BOX> obstacles;

                for( const auto& [reference, placement] : placements )
                {
                    if( reference != endpoints.front().component
                        && reference != endpoints[endpointIndex].component )
                    {
                        obstacles.push_back( placement.bounds.Inflated(
                                clearance + width / 2 ) );
                    }
                }

                const std::vector<POINT> path = routePath(
                        endpoints[endpointIndex].point, endpoints.front().point, board, grid,
                        obstacles, occupiedRouteCells );

                if( path.size() < 2 )
                {
                    result.error = "routing synthesis could not find an obstacle-free path for net "
                                   + netName + "; add space, change the routing grid, or author exact copper";
                    return result;
                }

                for( size_t pointIndex = 1; pointIndex < path.size(); ++pointIndex )
                {
                    if( path[pointIndex - 1] == path[pointIndex] )
                        continue;

                    const std::string logicalId = "synth-route-"
                                                  + std::to_string( routeId++ );
                    result.ir["pcb"].push_back(
                            { { "kind", "trace" }, { "logicalId", logicalId },
                              { "net", netName },
                              { "start", { { "xNm", path[pointIndex - 1].x },
                                           { "yNm", path[pointIndex - 1].y } } },
                              { "end", { { "xNm", path[pointIndex].x },
                                         { "yNm", path[pointIndex].y } } },
                              { "widthNm", width }, { "layer", layer },
                              { "locked", false }, { "typed", true },
                              { "synthesized", true } } );
                    ++synthesizedRoutes;
                }

                const int64_t radius = ( clearance + width / 2 + grid - 1 ) / grid;

                for( size_t segment = 1; segment < path.size(); ++segment )
                {
                    POINT cursor = snap( path[segment - 1], board, grid );
                    const POINT end = snap( path[segment], board, grid );
                    const int64_t stepX = cursor.x == end.x ? 0 : cursor.x < end.x ? grid : -grid;
                    const int64_t stepY = cursor.y == end.y ? 0 : cursor.y < end.y ? grid : -grid;

                    while( true )
                    {
                        for( int64_t offsetY = -radius; offsetY <= radius; ++offsetY )
                        {
                            for( int64_t offsetX = -radius; offsetX <= radius; ++offsetX )
                            {
                                occupiedRouteCells.insert(
                                        { cursor.x + offsetX * grid,
                                          cursor.y + offsetY * grid } );
                            }
                        }

                        if( cursor == end )
                            break;

                        cursor.x += stepX;
                        cursor.y += stepY;
                    }
                }
            }
        }
    }

    result.ok = true;
    result.summary = { { "placements", synthesizedPlacements },
                       { "routes", synthesizedRoutes },
                       { "totalBoardStatements", result.ir["pcb"].size() } };
    return result;
}
