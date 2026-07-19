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

#include "design_script_board_compiler.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <vector>

#include <boost/multiprecision/cpp_int.hpp>


namespace
{

using JSON = nlohmann::json;
using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using RESULT = KICHAD::DESIGN_SCRIPT_BOARD_COMPILER::RESULT;

constexpr size_t MAX_IDENTIFIER_BYTES = 128;
constexpr size_t MAX_ZONE_NAME_BYTES = 256;
constexpr size_t MAX_ZONE_POLYGONS = 32;
constexpr size_t MAX_ZONE_HOLES_PER_POLYGON = 64;
constexpr size_t MAX_ZONE_NODES = 8192;


void diagnostic( RESULT& aResult, const std::string& aSeverity, const std::string& aCode,
                 const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", aSeverity },
                                     { "code", aCode },
                                     { "message", aMessage } } );
}


bool isScalar( const DOCUMENT& aDocument, size_t aNode )
{
    return aNode < aDocument.Nodes().size()
           && aDocument.Nodes()[aNode].kind != DOCUMENT::NODE_KIND::LIST;
}


bool scalarText( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    if( !isScalar( aDocument, aNode ) )
        return false;

    aValue = aDocument.AtomText( aNode );
    return true;
}


bool validIdentifier( const std::string& aValue )
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


JSON scalarValue( const DOCUMENT& aDocument, size_t aNode )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    const std::string     value = aDocument.AtomText( aNode );

    if( node.kind == DOCUMENT::NODE_KIND::STRING )
        return value;

    if( value == "true" )
        return true;

    if( value == "false" )
        return false;

    int64_t integer = 0;
    std::from_chars_result converted =
            std::from_chars( value.data(), value.data() + value.size(), integer );

    if( converted.ec == std::errc() && converted.ptr == value.data() + value.size() )
        return integer;

    return value;
}


JSON expressionToIr( const DOCUMENT& aDocument, size_t aNode )
{
    if( isScalar( aDocument, aNode ) )
        return scalarValue( aDocument, aNode );

    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    JSON                  arguments = JSON::array();

    for( size_t i = 1; i < node.children.size(); ++i )
        arguments.emplace_back( expressionToIr( aDocument, node.children[i] ) );

    return { { "op", aDocument.ListHead( aNode ) }, { "args", std::move( arguments ) } };
}


bool parseDistance( const std::string& aText, int64_t& aNanometers )
{
    long double value = 0.0L;
    std::from_chars_result converted =
            std::from_chars( aText.data(), aText.data() + aText.size(), value );

    if( converted.ec != std::errc() || converted.ptr == aText.data()
        || !std::isfinite( value ) )
    {
        return false;
    }

    const std::string_view unit( converted.ptr,
                                 static_cast<size_t>( aText.data() + aText.size() - converted.ptr ) );
    long double scale = 0.0L;

    if( unit == "mm" )
        scale = 1000000.0L;
    else if( unit == "mil" )
        scale = 25400.0L;
    else if( unit == "um" )
        scale = 1000.0L;
    else if( unit == "nm" )
        scale = 1.0L;
    else if( unit == "in" )
        scale = 25400000.0L;
    else
        return false;

    const long double scaled = value * scale;
    const long double rounded = std::round( scaled );
    constexpr long double INT64_LOWER = -9223372036854775808.0L;
    constexpr long double INT64_UPPER_EXCLUSIVE = 9223372036854775808.0L;

    if( !std::isfinite( scaled ) || rounded < INT64_LOWER
        || rounded >= INT64_UPPER_EXCLUSIVE )
    {
        return false;
    }

    aNanometers = static_cast<int64_t>( rounded );
    return true;
}


bool parseArea( const std::string& aText, int64_t& aSquareNanometers )
{
    long double value = 0.0L;
    std::from_chars_result converted =
            std::from_chars( aText.data(), aText.data() + aText.size(), value );

    if( converted.ec != std::errc() || converted.ptr == aText.data()
        || !std::isfinite( value ) || value < 0.0L )
    {
        return false;
    }

    const std::string_view unit( converted.ptr,
                                 static_cast<size_t>( aText.data() + aText.size() - converted.ptr ) );
    long double scale = 0.0L;

    if( unit == "mm2" )
        scale = 1000000.0L;
    else if( unit == "mil2" )
        scale = 25400.0L;
    else if( unit == "um2" )
        scale = 1000.0L;
    else if( unit == "nm2" )
        scale = 1.0L;
    else if( unit == "in2" )
        scale = 25400000.0L;
    else
        return false;

    const long double scaled = value * scale * scale;
    const long double rounded = std::round( scaled );
    constexpr long double INT64_UPPER_EXCLUSIVE = 9223372036854775808.0L;

    if( !std::isfinite( scaled ) || rounded >= INT64_UPPER_EXCLUSIVE )
        return false;

    aSquareNanometers = static_cast<int64_t>( rounded );
    return true;
}


bool parseAngle( const std::string& aText, double& aDegrees )
{
    std::from_chars_result converted =
            std::from_chars( aText.data(), aText.data() + aText.size(), aDegrees );

    return converted.ec == std::errc() && converted.ptr != aText.data()
           && std::string_view( converted.ptr,
                                static_cast<size_t>( aText.data() + aText.size() - converted.ptr ) )
                      == "deg"
           && std::isfinite( aDegrees ) && std::abs( aDegrees ) <= 360000.0;
}


bool parseInteger( const DOCUMENT& aDocument, size_t aNode, int& aValue )
{
    std::string text;

    if( !scalarText( aDocument, aNode, text ) )
        return false;

    std::from_chars_result converted =
            std::from_chars( text.data(), text.data() + text.size(), aValue );
    return converted.ec == std::errc() && converted.ptr == text.data() + text.size();
}


bool parseUnsignedForm( const DOCUMENT& aDocument, size_t aNode, uint32_t& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           text;

    if( node.kind != DOCUMENT::NODE_KIND::LIST || node.children.size() != 2
        || !scalarText( aDocument, node.children[1], text ) )
    {
        return false;
    }

    std::from_chars_result converted =
            std::from_chars( text.data(), text.data() + text.size(), aValue );
    return converted.ec == std::errc() && converted.ptr == text.data() + text.size();
}


bool parseBooleanForm( const DOCUMENT& aDocument, size_t aNode, bool& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           text;

    if( node.kind != DOCUMENT::NODE_KIND::LIST || node.children.size() != 2
        || !scalarText( aDocument, node.children[1], text )
        || ( text != "true" && text != "false" ) )
    {
        return false;
    }

    aValue = text == "true";
    return true;
}


bool parseDistanceForm( const DOCUMENT& aDocument, size_t aNode, int64_t& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           text;
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalarText( aDocument, node.children[1], text ) && parseDistance( text, aValue );
}


bool parseAreaForm( const DOCUMENT& aDocument, size_t aNode, int64_t& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           text;
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalarText( aDocument, node.children[1], text ) && parseArea( text, aValue );
}


bool parseVectorForm( const DOCUMENT& aDocument, size_t aNode, JSON& aVector )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           xText;
    std::string           yText;
    int64_t               x = 0;
    int64_t               y = 0;

    if( node.kind != DOCUMENT::NODE_KIND::LIST || node.children.size() != 3
        || !scalarText( aDocument, node.children[1], xText )
        || !scalarText( aDocument, node.children[2], yText ) || !parseDistance( xText, x )
        || !parseDistance( yText, y ) )
    {
        return false;
    }

    aVector = { { "xNm", x }, { "yNm", y } };
    return true;
}


bool parseAngleForm( const DOCUMENT& aDocument, size_t aNode, double& aDegrees )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           text;
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalarText( aDocument, node.children[1], text ) && parseAngle( text, aDegrees );
}


bool parseScalarForm( const DOCUMENT& aDocument, size_t aNode, std::string& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    return node.kind == DOCUMENT::NODE_KIND::LIST && node.children.size() == 2
           && scalarText( aDocument, node.children[1], aValue );
}


bool parseRatioForm( const DOCUMENT& aDocument, size_t aNode, double& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           text;

    if( node.kind != DOCUMENT::NODE_KIND::LIST || node.children.size() != 2
        || !scalarText( aDocument, node.children[1], text ) )
    {
        return false;
    }

    std::from_chars_result converted =
            std::from_chars( text.data(), text.data() + text.size(), aValue );
    return converted.ec == std::errc() && converted.ptr == text.data() + text.size()
           && std::isfinite( aValue ) && aValue >= 0.0 && aValue <= 1.0;
}


bool collectFields( const DOCUMENT& aDocument, size_t aNode, size_t aBegin,
                    const std::set<std::string>& aAllowed,
                    std::map<std::string, size_t>& aFields, RESULT& aResult,
                    const std::string& aStatement )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    bool                  valid = true;

    for( size_t i = aBegin; i < node.children.size(); ++i )
    {
        const std::string head = aDocument.ListHead( node.children[i] );

        if( !aAllowed.contains( head ) )
        {
            diagnostic( aResult, "error", "unknown_board_field",
                        aStatement + " does not support field '" + head + "'" );
            valid = false;
            continue;
        }

        if( !aFields.emplace( head, node.children[i] ).second )
        {
            diagnostic( aResult, "error", "duplicate_board_field",
                        aStatement + " field '" + head + "' occurs more than once" );
            valid = false;
        }
    }

    return valid;
}


bool copperLayer( const std::string& aLayer )
{
    if( aLayer == "F.Cu" || aLayer == "B.Cu" )
        return true;

    if( !aLayer.starts_with( "In" ) || !aLayer.ends_with( ".Cu" ) )
        return false;

    int layer = 0;
    const char* begin = aLayer.data() + 2;
    const char* end = aLayer.data() + aLayer.size() - 3;
    std::from_chars_result converted = std::from_chars( begin, end, layer );
    return converted.ec == std::errc() && converted.ptr == end && layer >= 1 && layer <= 30;
}


JSON compileStackup( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 1, { "copper_layers", "thickness" }, fields, aResult,
                   "stackup" );
    int     copperLayers = 0;
    int64_t thickness = 0;

    if( !fields.contains( "copper_layers" )
        || aDocument.Nodes()[fields["copper_layers"]].children.size() != 2
        || !parseInteger( aDocument,
                          aDocument.Nodes()[fields["copper_layers"]].children[1], copperLayers )
        || copperLayers < 2 || copperLayers > 32 || copperLayers % 2 != 0 )
    {
        diagnostic( aResult, "error", "invalid_stackup_layers",
                    "stackup requires an even copper_layers value from 2 through 32" );
    }

    if( !fields.contains( "thickness" )
        || !parseDistanceForm( aDocument, fields["thickness"], thickness ) || thickness <= 0 )
    {
        diagnostic( aResult, "error", "invalid_stackup_thickness",
                    "stackup requires a positive physical thickness" );
    }

    return { { "kind", "stackup" },
             { "copperLayers", copperLayers },
             { "thicknessNm", thickness },
             { "typed", true } };
}


void compileOutline( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult,
                     std::set<std::string>& aLogicalIds )
{
    const DOCUMENT::NODE& outline = aDocument.Nodes()[aNode];

    if( outline.children.size() < 2 )
    {
        diagnostic( aResult, "error", "empty_outline",
                    "outline requires at least one rect declaration" );
        return;
    }

    for( size_t i = 1; i < outline.children.size(); ++i )
    {
        const size_t rectNode = outline.children[i];

        if( aDocument.ListHead( rectNode ) != "rect" )
        {
            diagnostic( aResult, "error", "unsupported_outline_shape",
                        "KDS version 1 currently supports rect board outlines" );
            continue;
        }

        std::map<std::string, size_t> fields;
        collectFields( aDocument, rectNode, 1, { "id", "at", "size", "line_width" }, fields,
                       aResult, "outline rect" );
        std::string logicalId;
        JSON        topLeft = JSON::object();
        JSON        size = JSON::object();
        int64_t     lineWidth = 50000;

        if( !fields.contains( "id" )
            || !parseScalarForm( aDocument, fields["id"], logicalId )
            || !validIdentifier( logicalId ) )
        {
            diagnostic( aResult, "error", "invalid_board_id",
                        "outline rect requires a bounded (id LOGICAL_ID)" );
        }
        else if( !aLogicalIds.emplace( logicalId ).second )
        {
            diagnostic( aResult, "error", "duplicate_board_id",
                        "board logical id " + logicalId + " occurs more than once" );
        }

        if( !fields.contains( "at" ) || !parseVectorForm( aDocument, fields["at"], topLeft ) )
        {
            diagnostic( aResult, "error", "invalid_outline_position",
                        "outline rect requires (at X Y) with explicit physical units" );
        }

        if( !fields.contains( "size" ) || !parseVectorForm( aDocument, fields["size"], size )
            || size.value( "xNm", int64_t( 0 ) ) <= 0 || size.value( "yNm", int64_t( 0 ) ) <= 0 )
        {
            diagnostic( aResult, "error", "invalid_outline_size",
                        "outline rect requires a positive (size WIDTH HEIGHT)" );
        }

        if( fields.contains( "line_width" )
            && ( !parseDistanceForm( aDocument, fields["line_width"], lineWidth )
                 || lineWidth <= 0 ) )
        {
            diagnostic( aResult, "error", "invalid_outline_line_width",
                        "outline line_width must be a positive physical distance" );
        }

        const int64_t x = topLeft.value( "xNm", int64_t( 0 ) );
        const int64_t y = topLeft.value( "yNm", int64_t( 0 ) );
        const int64_t width = size.value( "xNm", int64_t( 0 ) );
        const int64_t height = size.value( "yNm", int64_t( 0 ) );
        int64_t       right = x;
        int64_t       bottom = y;

        if( width > 0 && x <= std::numeric_limits<int64_t>::max() - width )
            right += width;
        else if( width > 0 )
            diagnostic( aResult, "error", "outline_coordinate_overflow",
                        "outline rect exceeds the supported coordinate range" );

        if( height > 0 && y <= std::numeric_limits<int64_t>::max() - height )
            bottom += height;
        else if( height > 0 )
            diagnostic( aResult, "error", "outline_coordinate_overflow",
                        "outline rect exceeds the supported coordinate range" );

        JSON bottomRight = { { "xNm", right }, { "yNm", bottom } };

        aResult.statements.push_back( { { "kind", "outline_rect" },
                                        { "logicalId", logicalId },
                                        { "topLeft", std::move( topLeft ) },
                                        { "bottomRight", std::move( bottomRight ) },
                                        { "lineWidthNm", lineWidth },
                                        { "layer", "Edge.Cuts" },
                                        { "typed", true } } );
    }
}


JSON compilePlace( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult,
                   std::set<std::string>& aPlacedComponents )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           reference;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], reference )
        || !validIdentifier( reference ) )
    {
        diagnostic( aResult, "error", "invalid_place_reference",
                    "place requires a bounded component reference" );
    }

    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 2, { "at", "rotation", "side", "locked" }, fields,
                   aResult, "place" );
    JSON        position = JSON::object();
    double      rotation = 0.0;
    std::string side = "front";
    bool        locked = false;

    if( !fields.contains( "at" ) || !parseVectorForm( aDocument, fields["at"], position ) )
    {
        diagnostic( aResult, "error", "invalid_place_position",
                    "place requires (at X Y) with explicit physical units" );
    }

    if( fields.contains( "rotation" )
        && !parseAngleForm( aDocument, fields["rotation"], rotation ) )
    {
        diagnostic( aResult, "error", "invalid_place_rotation",
                    "place rotation must use an explicit deg suffix" );
    }

    if( fields.contains( "side" )
        && ( !parseScalarForm( aDocument, fields["side"], side )
             || ( side != "front" && side != "back" ) ) )
    {
        diagnostic( aResult, "error", "invalid_place_side",
                    "place side must be front or back" );
    }

    if( fields.contains( "locked" )
        && !parseBooleanForm( aDocument, fields["locked"], locked ) )
    {
        diagnostic( aResult, "error", "invalid_place_locked",
                    "place locked must be true or false" );
    }

    if( validIdentifier( reference ) )
    {
        if( !aPlacedComponents.emplace( reference ).second )
        {
            diagnostic( aResult, "error", "duplicate_board_placement",
                        "component " + reference + " is placed more than once" );
        }

        aResult.componentReferences.emplace_back( reference );
    }

    return { { "kind", "place" },
             { "component", reference },
             { "position", std::move( position ) },
             { "rotationDegrees", rotation },
             { "side", side },
             { "locked", locked },
             { "typed", true } };
}


int copperLayerIndex( const std::string& aLayer, int aCopperLayers )
{
    if( aLayer == "F.Cu" )
        return 0;

    if( aLayer == "B.Cu" )
        return aCopperLayers - 1;

    if( !aLayer.starts_with( "In" ) || !aLayer.ends_with( ".Cu" ) )
        return -1;

    int layer = 0;
    const char* begin = aLayer.data() + 2;
    const char* end = aLayer.data() + aLayer.size() - 3;
    std::from_chars_result converted = std::from_chars( begin, end, layer );

    if( converted.ec != std::errc() || converted.ptr != end || layer < 1
        || layer > aCopperLayers - 2 )
    {
        return -1;
    }

    return layer;
}


void validateStackupLayers( const JSON& aStatements, RESULT& aResult )
{
    int copperLayers = 2;

    for( const JSON& statement : aStatements )
    {
        if( statement.value( "kind", "" ) == "stackup" )
            copperLayers = statement.value( "copperLayers", 0 );
    }

    if( copperLayers < 2 || copperLayers > 32 || copperLayers % 2 != 0 )
        return;

    for( const JSON& statement : aStatements )
    {
        const std::string kind = statement.value( "kind", "" );

        if( kind == "trace" || kind == "arc" )
        {
            const std::string layer = statement.value( "layer", "" );

            if( copperLayer( layer ) && copperLayerIndex( layer, copperLayers ) < 0 )
            {
                diagnostic( aResult, "error", "route_layer_outside_stackup",
                            "route layer " + layer + " is not present in the declared stackup" );
            }
        }
        else if( kind == "via" )
        {
            const std::string startLayer = statement.value( "startLayer", "" );
            const std::string endLayer = statement.value( "endLayer", "" );
            const int start = copperLayerIndex( startLayer, copperLayers );
            const int end = copperLayerIndex( endLayer, copperLayers );

            if( ( copperLayer( startLayer ) && start < 0 )
                || ( copperLayer( endLayer ) && end < 0 ) )
            {
                diagnostic( aResult, "error", "via_layer_outside_stackup",
                            "via layers are not present in the declared stackup" );
                continue;
            }

            if( start < 0 || end < 0 )
                continue;

            if( start == end )
            {
                diagnostic( aResult, "error", "invalid_via_span",
                            "via start and end layers must differ" );
                continue;
            }

            const std::string type = statement.value( "viaType", "" );
            const bool startOuter = start == 0 || start == copperLayers - 1;
            const bool endOuter = end == 0 || end == copperLayers - 1;

            if( type == "blind" && startOuter == endOuter )
            {
                diagnostic( aResult, "error", "invalid_blind_via_span",
                            "blind vias must connect one outer and one inner copper layer" );
            }
            else if( type == "buried" && ( startOuter || endOuter ) )
            {
                diagnostic( aResult, "error", "invalid_buried_via_span",
                            "buried vias must connect two inner copper layers" );
            }
            else if( type == "micro" && std::abs( start - end ) != 1 )
            {
                diagnostic( aResult, "error", "invalid_microvia_span",
                            "microvias must connect adjacent copper layers" );
            }
        }
        else if( kind == "zone" )
        {
            for( const JSON& layer : statement.value( "layers", JSON::array() ) )
            {
                const std::string layerName = layer.is_string() ? layer.get<std::string>() : "";

                if( copperLayer( layerName )
                    && copperLayerIndex( layerName, copperLayers ) < 0 )
                {
                    diagnostic( aResult, "error", "zone_layer_outside_stackup",
                                "zone layer " + layerName
                                        + " is not present in the declared stackup" );
                }
            }
        }
    }
}


JSON compileRoute( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult,
                   std::set<std::string>& aLogicalIds )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           net;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], net )
        || net.empty() || net.size() > MAX_IDENTIFIER_BYTES )
    {
        diagnostic( aResult, "error", "invalid_route_net", "route requires a bounded net name" );
    }

    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 2,
                   { "id", "from", "mid", "to", "width", "layer", "locked" }, fields,
                   aResult, "route" );
    std::string logicalId;
    JSON        start = JSON::object();
    JSON        mid = JSON::object();
    JSON        end = JSON::object();
    int64_t     width = 0;
    std::string layer;
    bool        locked = false;

    if( !fields.contains( "id" ) || !parseScalarForm( aDocument, fields["id"], logicalId )
        || !validIdentifier( logicalId ) )
    {
        diagnostic( aResult, "error", "invalid_board_id",
                    "route requires a bounded (id LOGICAL_ID)" );
    }
    else if( !aLogicalIds.emplace( logicalId ).second )
    {
        diagnostic( aResult, "error", "duplicate_board_id",
                    "board logical id " + logicalId + " occurs more than once" );
    }

    if( !fields.contains( "from" ) || !parseVectorForm( aDocument, fields["from"], start ) )
    {
        diagnostic( aResult, "error", "invalid_route_start",
                    "route requires (from X Y) with explicit physical units" );
    }

    if( !fields.contains( "to" ) || !parseVectorForm( aDocument, fields["to"], end ) )
    {
        diagnostic( aResult, "error", "invalid_route_end",
                    "route requires (to X Y) with explicit physical units" );
    }

    if( fields.contains( "mid" ) && !parseVectorForm( aDocument, fields["mid"], mid ) )
    {
        diagnostic( aResult, "error", "invalid_route_midpoint",
                    "route midpoint must be (mid X Y) with explicit physical units" );
    }

    if( !fields.contains( "width" ) || !parseDistanceForm( aDocument, fields["width"], width )
        || width <= 0 )
    {
        diagnostic( aResult, "error", "invalid_route_width",
                    "route requires a positive physical width" );
    }

    if( !fields.contains( "layer" ) || !parseScalarForm( aDocument, fields["layer"], layer )
        || !copperLayer( layer ) )
    {
        diagnostic( aResult, "error", "invalid_route_layer",
                    "route layer must be F.Cu, B.Cu, or In1.Cu through In30.Cu" );
    }

    if( fields.contains( "locked" )
        && !parseBooleanForm( aDocument, fields["locked"], locked ) )
    {
        diagnostic( aResult, "error", "invalid_route_locked",
                    "route locked must be true or false" );
    }

    if( !start.empty() && start == end )
        diagnostic( aResult, "error", "zero_length_route", "route endpoints must differ" );

    if( !net.empty() )
        aResult.netReferences.emplace_back( net );

    JSON route = { { "kind", mid.empty() ? "trace" : "arc" },
                   { "logicalId", logicalId },
                   { "net", net },
                   { "start", std::move( start ) },
                   { "end", std::move( end ) },
                   { "widthNm", width },
                   { "layer", layer },
                   { "locked", locked },
                   { "typed", true } };

    if( !mid.empty() )
        route["mid"] = std::move( mid );

    return route;
}


JSON compileVia( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult,
                 std::set<std::string>& aLogicalIds )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           net;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], net )
        || net.empty() || net.size() > MAX_IDENTIFIER_BYTES )
    {
        diagnostic( aResult, "error", "invalid_via_net", "via requires a bounded net name" );
    }

    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 2,
                   { "id", "at", "diameter", "drill", "layers", "type", "locked" }, fields,
                   aResult, "via" );
    std::string logicalId;
    JSON        position = JSON::object();
    int64_t     diameter = 0;
    int64_t     drill = 0;
    std::string startLayer;
    std::string endLayer;
    std::string type = "through";
    bool        locked = false;

    if( !fields.contains( "id" ) || !parseScalarForm( aDocument, fields["id"], logicalId )
        || !validIdentifier( logicalId ) )
    {
        diagnostic( aResult, "error", "invalid_board_id",
                    "via requires a bounded (id LOGICAL_ID)" );
    }
    else if( !aLogicalIds.emplace( logicalId ).second )
    {
        diagnostic( aResult, "error", "duplicate_board_id",
                    "board logical id " + logicalId + " occurs more than once" );
    }

    if( !fields.contains( "at" ) || !parseVectorForm( aDocument, fields["at"], position ) )
    {
        diagnostic( aResult, "error", "invalid_via_position",
                    "via requires (at X Y) with explicit physical units" );
    }

    if( !fields.contains( "diameter" )
        || !parseDistanceForm( aDocument, fields["diameter"], diameter ) || diameter <= 0 )
    {
        diagnostic( aResult, "error", "invalid_via_diameter",
                    "via requires a positive physical diameter" );
    }

    if( !fields.contains( "drill" ) || !parseDistanceForm( aDocument, fields["drill"], drill )
        || drill <= 0 || drill >= diameter )
    {
        diagnostic( aResult, "error", "invalid_via_drill",
                    "via drill must be positive and smaller than its diameter" );
    }

    if( fields.contains( "layers" ) )
    {
        const DOCUMENT::NODE& layers = aDocument.Nodes()[fields["layers"]];

        if( layers.children.size() != 3
            || !scalarText( aDocument, layers.children[1], startLayer )
            || !scalarText( aDocument, layers.children[2], endLayer )
            || !copperLayer( startLayer ) || !copperLayer( endLayer ) )
        {
            diagnostic( aResult, "error", "invalid_via_layers",
                        "via layers require two copper-layer names" );
        }
    }
    else
    {
        startLayer = "F.Cu";
        endLayer = "B.Cu";
    }

    if( fields.contains( "type" )
        && ( !parseScalarForm( aDocument, fields["type"], type )
             || ( type != "through" && type != "blind" && type != "buried"
                  && type != "micro" ) ) )
    {
        diagnostic( aResult, "error", "invalid_via_type",
                    "via type must be through, blind, buried, or micro" );
    }

    if( type == "through" && ( startLayer != "F.Cu" || endLayer != "B.Cu" ) )
    {
        diagnostic( aResult, "error", "invalid_through_via_layers",
                    "through vias must span F.Cu to B.Cu" );
    }

    if( fields.contains( "locked" )
        && !parseBooleanForm( aDocument, fields["locked"], locked ) )
    {
        diagnostic( aResult, "error", "invalid_via_locked",
                    "via locked must be true or false" );
    }

    if( !net.empty() )
        aResult.netReferences.emplace_back( net );

    return { { "kind", "via" },
             { "logicalId", logicalId },
             { "net", net },
             { "position", std::move( position ) },
             { "diameterNm", diameter },
             { "drillNm", drill },
             { "startLayer", startLayer },
             { "endLayer", endLayer },
             { "viaType", type },
             { "locked", locked },
             { "typed", true } };
}


bool internalDistance( int64_t aValue, bool aAllowZero = false )
{
    return ( aAllowZero ? aValue >= 0 : aValue > 0 )
           && aValue <= std::numeric_limits<int>::max();
}


bool appendZonePoint( const DOCUMENT& aDocument, size_t aNode, JSON& aNodes,
                      size_t& aNodeCount, RESULT& aResult, const std::string& aLabel )
{
    JSON point;

    if( aDocument.ListHead( aNode ) != "point" || !parseVectorForm( aDocument, aNode, point ) )
    {
        diagnostic( aResult, "error", "invalid_zone_point",
                    aLabel + " requires (point X Y) values with explicit physical units" );
        return false;
    }

    const int64_t x = point.value( "xNm", int64_t( 0 ) );
    const int64_t y = point.value( "yNm", int64_t( 0 ) );

    if( x < std::numeric_limits<int>::min() || x > std::numeric_limits<int>::max()
        || y < std::numeric_limits<int>::min() || y > std::numeric_limits<int>::max() )
    {
        diagnostic( aResult, "error", "zone_coordinate_out_of_range",
                    aLabel + " coordinates must fit KiCad 10's internal coordinate range" );
        return false;
    }

    if( aNodeCount >= MAX_ZONE_NODES )
    {
        if( aNodeCount == MAX_ZONE_NODES )
        {
            diagnostic( aResult, "error", "too_many_zone_points",
                        "zone geometry is limited to 8192 total points" );
        }

        ++aNodeCount;
        return false;
    }

    ++aNodeCount;
    aNodes.push_back( { { "point", std::move( point ) } } );
    return true;
}


using ZONE_POINT = std::pair<int64_t, int64_t>;
using WIDE_INTEGER = boost::multiprecision::int128_t;


std::vector<ZONE_POINT> zonePoints( const JSON& aLine )
{
    std::vector<ZONE_POINT> points;

    if( !aLine.is_object() || !aLine.contains( "nodes" ) || !aLine["nodes"].is_array() )
        return points;

    points.reserve( aLine["nodes"].size() );

    for( const JSON& node : aLine["nodes"] )
    {
        if( !node.is_object() || !node.contains( "point" ) || !node["point"].is_object()
            || !node["point"].contains( "xNm" ) || !node["point"]["xNm"].is_number_integer()
            || !node["point"].contains( "yNm" ) || !node["point"]["yNm"].is_number_integer() )
        {
            return {};
        }

        points.emplace_back( node["point"]["xNm"].get<int64_t>(),
                             node["point"]["yNm"].get<int64_t>() );
    }

    return points;
}


WIDE_INTEGER orientation( const ZONE_POINT& aStart, const ZONE_POINT& aEnd,
                          const ZONE_POINT& aPoint )
{
    return WIDE_INTEGER( aEnd.first - aStart.first ) * ( aPoint.second - aStart.second )
           - WIDE_INTEGER( aEnd.second - aStart.second ) * ( aPoint.first - aStart.first );
}


bool pointOnSegment( const ZONE_POINT& aPoint, const ZONE_POINT& aStart,
                     const ZONE_POINT& aEnd )
{
    return orientation( aStart, aEnd, aPoint ) == 0
           && aPoint.first >= std::min( aStart.first, aEnd.first )
           && aPoint.first <= std::max( aStart.first, aEnd.first )
           && aPoint.second >= std::min( aStart.second, aEnd.second )
           && aPoint.second <= std::max( aStart.second, aEnd.second );
}


bool segmentsIntersect( const ZONE_POINT& aStart, const ZONE_POINT& aEnd,
                        const ZONE_POINT& bStart, const ZONE_POINT& bEnd )
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


bool lineSelfIntersects( const std::vector<ZONE_POINT>& aPoints )
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


bool linesIntersect( const std::vector<ZONE_POINT>& aFirst,
                     const std::vector<ZONE_POINT>& aSecond )
{
    for( size_t first = 0; first < aFirst.size(); ++first )
    {
        const size_t firstEnd = ( first + 1 ) % aFirst.size();

        for( size_t second = 0; second < aSecond.size(); ++second )
        {
            const size_t secondEnd = ( second + 1 ) % aSecond.size();

            if( segmentsIntersect( aFirst[first], aFirst[firstEnd],
                                   aSecond[second], aSecond[secondEnd] ) )
            {
                return true;
            }
        }
    }

    return false;
}


bool pointInsideLine( const ZONE_POINT& aPoint, const std::vector<ZONE_POINT>& aLine )
{
    int winding = 0;

    for( size_t index = 0; index < aLine.size(); ++index )
    {
        const ZONE_POINT& start = aLine[index];
        const ZONE_POINT& end = aLine[( index + 1 ) % aLine.size()];

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


bool validateZoneLine( const JSON& aNodes, RESULT& aResult, const std::string& aLabel )
{
    if( !aNodes.is_array() || aNodes.size() < 3 )
    {
        diagnostic( aResult, "error", "invalid_zone_polygon",
                    aLabel + " requires at least three points" );
        return false;
    }

    std::set<std::pair<int64_t, int64_t>> unique;
    std::vector<std::pair<int64_t, int64_t>> points;
    points.reserve( aNodes.size() );

    for( const JSON& node : aNodes )
    {
        const JSON& point = node.at( "point" );
        std::pair<int64_t, int64_t> coordinate = {
            point.at( "xNm" ).get<int64_t>(), point.at( "yNm" ).get<int64_t>()
        };
        points.emplace_back( coordinate );

        if( !unique.emplace( coordinate ).second )
        {
            diagnostic( aResult, "error", "duplicate_zone_point",
                        aLabel + " must not repeat a point; closure is implicit" );
            return false;
        }
    }

    WIDE_INTEGER doubledArea = 0;

    for( size_t i = 0; i < points.size(); ++i )
    {
        const auto& current = points[i];
        const auto& next = points[( i + 1 ) % points.size()];
        doubledArea += WIDE_INTEGER( current.first ) * next.second
                       - WIDE_INTEGER( next.first ) * current.second;
    }

    if( doubledArea == 0 )
    {
        diagnostic( aResult, "error", "zero_area_zone_polygon",
                    aLabel + " must enclose a non-zero area" );
        return false;
    }

    if( lineSelfIntersects( points ) )
    {
        diagnostic( aResult, "error", "self_intersecting_zone_polygon",
                    aLabel + " must not self-intersect" );
        return false;
    }

    return true;
}


void validateZoneTopology( const JSON& aPolygons, RESULT& aResult )
{
    for( size_t polygonIndex = 0; polygonIndex < aPolygons.size(); ++polygonIndex )
    {
        const JSON& polygon = aPolygons[polygonIndex];
        const std::vector<ZONE_POINT> outer = zonePoints( polygon.at( "outline" ) );

        if( outer.size() < 3 )
            continue;

        std::vector<std::vector<ZONE_POINT>> holes;

        for( const JSON& holeJson : polygon.at( "holes" ) )
        {
            std::vector<ZONE_POINT> hole = zonePoints( holeJson );

            if( hole.size() < 3 )
                continue;

            if( linesIntersect( outer, hole ) || !pointInsideLine( hole.front(), outer ) )
            {
                diagnostic( aResult, "error", "zone_hole_outside_polygon",
                            "each zone hole must be strictly inside its polygon" );
            }

            for( const std::vector<ZONE_POINT>& existing : holes )
            {
                if( linesIntersect( existing, hole )
                    || pointInsideLine( hole.front(), existing )
                    || pointInsideLine( existing.front(), hole ) )
                {
                    diagnostic( aResult, "error", "overlapping_zone_holes",
                                "zone holes must not overlap or contain one another" );
                    break;
                }
            }

            holes.emplace_back( std::move( hole ) );
        }

        for( size_t otherIndex = 0; otherIndex < polygonIndex; ++otherIndex )
        {
            const std::vector<ZONE_POINT> other =
                    zonePoints( aPolygons[otherIndex].at( "outline" ) );

            if( other.size() < 3 )
                continue;

            if( linesIntersect( outer, other ) || pointInsideLine( outer.front(), other )
                || pointInsideLine( other.front(), outer ) )
            {
                diagnostic( aResult, "error", "overlapping_zone_polygons",
                            "zone polygons must be disjoint; use a hole for subtraction" );
            }
        }
    }
}


JSON compileZoneOutline( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& outline = aDocument.Nodes()[aNode];
    JSON                  polygons = JSON::array();
    size_t                totalNodes = 0;

    if( outline.children.size() < 2 )
    {
        diagnostic( aResult, "error", "invalid_zone_outline",
                    "zone outline requires at least one polygon" );
        return polygons;
    }

    for( size_t polygonIndex = 1; polygonIndex < outline.children.size(); ++polygonIndex )
    {
        if( polygons.size() >= MAX_ZONE_POLYGONS )
        {
            diagnostic( aResult, "error", "too_many_zone_polygons",
                        "zone outlines are limited to 32 polygons" );
            break;
        }

        const size_t polygonNode = outline.children[polygonIndex];

        if( aDocument.ListHead( polygonNode ) != "polygon" )
        {
            diagnostic( aResult, "error", "invalid_zone_outline",
                        "zone outline accepts only polygon declarations" );
            continue;
        }

        const DOCUMENT::NODE& polygon = aDocument.Nodes()[polygonNode];
        JSON                  outerNodes = JSON::array();
        JSON                  holes = JSON::array();

        for( size_t childIndex = 1; childIndex < polygon.children.size(); ++childIndex )
        {
            const size_t child = polygon.children[childIndex];
            const std::string head = aDocument.ListHead( child );

            if( head == "point" )
            {
                appendZonePoint( aDocument, child, outerNodes, totalNodes, aResult,
                                 "zone polygon" );
            }
            else if( head == "hole" )
            {
                if( holes.size() >= MAX_ZONE_HOLES_PER_POLYGON )
                {
                    diagnostic( aResult, "error", "too_many_zone_holes",
                                "each zone polygon is limited to 64 holes" );
                    continue;
                }

                const DOCUMENT::NODE& hole = aDocument.Nodes()[child];
                JSON                  holeNodes = JSON::array();

                for( size_t holeIndex = 1; holeIndex < hole.children.size(); ++holeIndex )
                {
                    appendZonePoint( aDocument, hole.children[holeIndex], holeNodes,
                                     totalNodes, aResult, "zone hole" );
                }

                validateZoneLine( holeNodes, aResult, "zone hole" );
                holes.push_back( { { "nodes", std::move( holeNodes ) }, { "closed", true } } );
            }
            else
            {
                diagnostic( aResult, "error", "unknown_zone_geometry",
                            "zone polygon accepts only point and hole declarations" );
            }
        }

        validateZoneLine( outerNodes, aResult, "zone polygon" );
        polygons.push_back( { { "outline",
                                { { "nodes", std::move( outerNodes ) }, { "closed", true } } },
                              { "holes", std::move( holes ) } } );
    }

    validateZoneTopology( polygons, aResult );

    return polygons;
}


JSON compileZoneConnection( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           style;
    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 2, { "thermal_gap", "thermal_spoke_width" }, fields,
                   aResult, "zone connection" );
    int64_t thermalGap = 0;
    int64_t thermalSpokeWidth = 0;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], style )
        || ( style != "none" && style != "solid" && style != "thermal"
             && style != "pth_thermal" ) )
    {
        diagnostic( aResult, "error", "invalid_zone_connection",
                    "zone connection must be none, solid, thermal, or pth_thermal" );
    }

    const bool thermal = style == "thermal" || style == "pth_thermal";

    if( thermal )
    {
        if( !fields.contains( "thermal_gap" )
            || !parseDistanceForm( aDocument, fields["thermal_gap"], thermalGap )
            || !internalDistance( thermalGap ) )
        {
            diagnostic( aResult, "error", "invalid_zone_thermal_gap",
                        "thermal zone connections require a positive thermal_gap" );
        }

        if( !fields.contains( "thermal_spoke_width" )
            || !parseDistanceForm( aDocument, fields["thermal_spoke_width"],
                                   thermalSpokeWidth )
            || !internalDistance( thermalSpokeWidth ) )
        {
            diagnostic( aResult, "error", "invalid_zone_thermal_spoke_width",
                        "thermal zone connections require a positive thermal_spoke_width" );
        }
    }
    else if( fields.contains( "thermal_gap" ) || fields.contains( "thermal_spoke_width" ) )
    {
        diagnostic( aResult, "error", "unexpected_zone_thermal_setting",
                    "thermal dimensions are valid only for thermal connection styles" );
    }

    return { { "style", style },
             { "thermalGapNm", thermalGap },
             { "thermalSpokeWidthNm", thermalSpokeWidth } };
}


JSON compileZoneIslands( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           mode;
    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 2, { "minimum_area" }, fields, aResult,
                   "zone islands" );
    int64_t minimumArea = 0;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], mode )
        || ( mode != "remove_all" && mode != "keep_all" && mode != "remove_below" ) )
    {
        diagnostic( aResult, "error", "invalid_zone_island_mode",
                    "zone islands must be remove_all, keep_all, or remove_below" );
    }

    if( mode == "remove_below" )
    {
        if( !fields.contains( "minimum_area" )
            || !parseAreaForm( aDocument, fields["minimum_area"], minimumArea )
            || minimumArea <= 0 )
        {
            diagnostic( aResult, "error", "invalid_zone_minimum_island_area",
                        "remove_below requires a positive minimum_area with square units" );
        }
    }
    else if( fields.contains( "minimum_area" ) )
    {
        diagnostic( aResult, "error", "unexpected_zone_minimum_island_area",
                    "minimum_area is valid only with remove_below" );
    }

    return { { "mode", mode }, { "minimumAreaNm2", minimumArea } };
}


JSON compileZoneFill( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           mode;
    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 2,
                   { "thickness", "gap", "orientation", "smoothing",
                     "hole_min_area_ratio", "border" },
                   fields, aResult, "zone fill" );
    int64_t thickness = 0;
    int64_t gap = 0;
    double orientation = 0.0;
    double smoothing = 0.0;
    double holeMinimumAreaRatio = 0.0;
    std::string border = "minimum";

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], mode )
        || ( mode != "solid" && mode != "hatched" ) )
    {
        diagnostic( aResult, "error", "invalid_zone_fill_mode",
                    "zone fill must be solid or hatched" );
    }

    if( mode == "hatched" )
    {
        if( !fields.contains( "thickness" )
            || !parseDistanceForm( aDocument, fields["thickness"], thickness )
            || !internalDistance( thickness ) )
        {
            diagnostic( aResult, "error", "invalid_zone_hatch_thickness",
                        "hatched zone fill requires a positive thickness" );
        }

        if( !fields.contains( "gap" )
            || !parseDistanceForm( aDocument, fields["gap"], gap )
            || !internalDistance( gap ) )
        {
            diagnostic( aResult, "error", "invalid_zone_hatch_gap",
                        "hatched zone fill requires a positive gap" );
        }

        if( !fields.contains( "orientation" )
            || !parseAngleForm( aDocument, fields["orientation"], orientation ) )
        {
            diagnostic( aResult, "error", "invalid_zone_hatch_orientation",
                        "hatched zone fill requires an orientation with a deg suffix" );
        }

        if( fields.contains( "smoothing" )
            && !parseRatioForm( aDocument, fields["smoothing"], smoothing ) )
        {
            diagnostic( aResult, "error", "invalid_zone_hatch_smoothing",
                        "zone hatch smoothing must be a ratio from 0 through 1" );
        }

        if( fields.contains( "hole_min_area_ratio" )
            && !parseRatioForm( aDocument, fields["hole_min_area_ratio"],
                                holeMinimumAreaRatio ) )
        {
            diagnostic( aResult, "error", "invalid_zone_hatch_hole_ratio",
                        "zone hatch hole_min_area_ratio must be from 0 through 1" );
        }

        if( fields.contains( "border" )
            && ( !parseScalarForm( aDocument, fields["border"], border )
                 || ( border != "minimum" && border != "hatch" ) ) )
        {
            diagnostic( aResult, "error", "invalid_zone_hatch_border",
                        "zone hatch border must be minimum or hatch" );
        }
    }
    else if( !fields.empty() )
    {
        diagnostic( aResult, "error", "unexpected_solid_zone_fill_setting",
                    "solid zone fill has no hatch settings" );
    }

    return { { "mode", mode },
             { "thicknessNm", thickness },
             { "gapNm", gap },
             { "orientationDegrees", orientation },
             { "smoothingRatio", smoothing },
             { "holeMinimumAreaRatio", holeMinimumAreaRatio },
             { "borderMode", border } };
}


JSON compileZoneBorder( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           style;
    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 2, { "pitch" }, fields, aResult, "zone border" );
    int64_t pitch = 0;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], style )
        || ( style != "solid" && style != "diagonal_full"
             && style != "diagonal_edge" && style != "invisible" ) )
    {
        diagnostic( aResult, "error", "invalid_zone_border_style",
                    "zone border must be solid, diagonal_full, diagonal_edge, or invisible" );
    }

    const bool diagonal = style == "diagonal_full" || style == "diagonal_edge";

    if( diagonal )
    {
        if( !fields.contains( "pitch" )
            || !parseDistanceForm( aDocument, fields["pitch"], pitch )
            || !internalDistance( pitch ) )
        {
            diagnostic( aResult, "error", "invalid_zone_border_pitch",
                        "diagonal zone borders require a positive pitch" );
        }
    }
    else if( fields.contains( "pitch" ) )
    {
        diagnostic( aResult, "error", "unexpected_zone_border_pitch",
                    "border pitch is valid only for diagonal borders" );
    }

    return { { "style", style }, { "pitchNm", pitch } };
}


JSON compileZoneHatchOffsets( const DOCUMENT& aDocument, size_t aNode,
                              const JSON& aLayers, RESULT& aResult )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::set<std::string> allowedLayers;
    std::set<std::string> seenLayers;
    JSON                  properties = JSON::array();

    for( const JSON& layer : aLayers )
    {
        if( layer.is_string() )
            allowedLayers.emplace( layer.get<std::string>() );
    }

    for( size_t index = 1; index < node.children.size(); ++index )
    {
        const size_t propertyNode = node.children[index];
        const DOCUMENT::NODE& property = aDocument.Nodes()[propertyNode];
        std::string layer;
        std::string xText;
        std::string yText;
        int64_t x = 0;
        int64_t y = 0;

        if( aDocument.ListHead( propertyNode ) != "layer" || property.children.size() != 4
            || !scalarText( aDocument, property.children[1], layer )
            || !scalarText( aDocument, property.children[2], xText )
            || !scalarText( aDocument, property.children[3], yText )
            || !parseDistance( xText, x ) || !parseDistance( yText, y )
            || x < std::numeric_limits<int>::min() || x > std::numeric_limits<int>::max()
            || y < std::numeric_limits<int>::min() || y > std::numeric_limits<int>::max()
            || !allowedLayers.contains( layer ) || !seenLayers.emplace( layer ).second )
        {
            diagnostic( aResult, "error", "invalid_zone_hatch_offset",
                        "each hatch offset must uniquely name a zone layer and two bounded distances" );
            continue;
        }

        properties.push_back( { { "layer", layer },
                                { "offset", { { "xNm", x }, { "yNm", y } } } } );
    }

    if( node.children.size() < 2 )
    {
        diagnostic( aResult, "error", "invalid_zone_hatch_offset",
                    "hatch_offsets requires at least one layer offset" );
    }

    return properties;
}


JSON compileZone( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult,
                  std::set<std::string>& aLogicalIds )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           net;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], net )
        || !validIdentifier( net ) )
    {
        diagnostic( aResult, "error", "invalid_zone_net",
                    "zone requires a bounded net name" );
    }

    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 2,
                   { "id", "layers", "outline", "name", "clearance", "min_thickness",
                     "connection", "islands", "fill", "hatch_offsets", "priority", "border",
                     "locked" },
                   fields, aResult, "zone" );
    std::string logicalId;
    std::string name;
    JSON        layers = JSON::array();
    JSON        polygons = JSON::array();
    int64_t     clearance = 0;
    int64_t     minimumThickness = 0;
    JSON        connection = JSON::object();
    JSON        islands = JSON::object();
    JSON        fill = JSON::object();
    JSON        layerProperties = JSON::array();
    uint32_t    priority = 0;
    JSON        border = { { "style", "solid" }, { "pitchNm", 0 } };
    bool        locked = false;

    if( !fields.contains( "id" ) || !parseScalarForm( aDocument, fields["id"], logicalId )
        || !validIdentifier( logicalId ) )
    {
        diagnostic( aResult, "error", "invalid_board_id",
                    "zone requires a bounded (id LOGICAL_ID)" );
    }
    else if( !aLogicalIds.emplace( logicalId ).second )
    {
        diagnostic( aResult, "error", "duplicate_board_id",
                    "board logical id " + logicalId + " occurs more than once" );
    }

    name = logicalId;

    if( fields.contains( "name" )
        && ( !parseScalarForm( aDocument, fields["name"], name ) || name.empty()
             || name.size() > MAX_ZONE_NAME_BYTES ) )
    {
        diagnostic( aResult, "error", "invalid_zone_name",
                    "zone name must contain 1 through 256 UTF-8 bytes" );
    }

    if( !fields.contains( "layers" ) )
    {
        diagnostic( aResult, "error", "invalid_zone_layers",
                    "zone requires one layers declaration" );
    }
    else
    {
        const DOCUMENT::NODE& layerNode = aDocument.Nodes()[fields["layers"]];
        std::set<std::string> uniqueLayers;

        if( layerNode.children.size() < 2 || layerNode.children.size() > 33 )
        {
            diagnostic( aResult, "error", "invalid_zone_layers",
                        "zone layers requires 1 through 32 copper layers" );
        }

        for( size_t i = 1; i < layerNode.children.size(); ++i )
        {
            std::string layer;

            if( !scalarText( aDocument, layerNode.children[i], layer ) || !copperLayer( layer )
                || !uniqueLayers.emplace( layer ).second )
            {
                diagnostic( aResult, "error", "invalid_zone_layers",
                            "zone layers must be distinct KiCad copper-layer names" );
                continue;
            }

            layers.push_back( layer );
        }
    }

    if( !fields.contains( "outline" ) )
    {
        diagnostic( aResult, "error", "invalid_zone_outline",
                    "zone requires an explicit polygon outline" );
    }
    else
    {
        polygons = compileZoneOutline( aDocument, fields["outline"], aResult );
    }

    if( !fields.contains( "clearance" )
        || !parseDistanceForm( aDocument, fields["clearance"], clearance )
        || !internalDistance( clearance, true ) )
    {
        diagnostic( aResult, "error", "invalid_zone_clearance",
                    "zone requires a non-negative clearance in physical units" );
    }

    if( !fields.contains( "min_thickness" )
        || !parseDistanceForm( aDocument, fields["min_thickness"], minimumThickness )
        || !internalDistance( minimumThickness ) )
    {
        diagnostic( aResult, "error", "invalid_zone_min_thickness",
                    "zone requires a positive min_thickness in physical units" );
    }

    if( !fields.contains( "connection" ) )
    {
        diagnostic( aResult, "error", "invalid_zone_connection",
                    "zone requires an explicit connection policy" );
    }
    else
    {
        connection = compileZoneConnection( aDocument, fields["connection"], aResult );
    }

    if( !fields.contains( "islands" ) )
    {
        diagnostic( aResult, "error", "invalid_zone_island_mode",
                    "zone requires an explicit islands policy" );
    }
    else
    {
        islands = compileZoneIslands( aDocument, fields["islands"], aResult );
    }

    if( !fields.contains( "fill" ) )
    {
        diagnostic( aResult, "error", "invalid_zone_fill_mode",
                    "zone requires an explicit fill policy" );
    }
    else
    {
        fill = compileZoneFill( aDocument, fields["fill"], aResult );
    }

    if( fields.contains( "hatch_offsets" ) )
    {
        if( fill.value( "mode", "" ) != "hatched" )
        {
            diagnostic( aResult, "error", "unexpected_zone_hatch_offset",
                        "hatch_offsets is valid only for hatched zone fill" );
        }

        layerProperties = compileZoneHatchOffsets( aDocument, fields["hatch_offsets"],
                                                   layers, aResult );
    }

    if( fields.contains( "priority" )
        && !parseUnsignedForm( aDocument, fields["priority"], priority ) )
    {
        diagnostic( aResult, "error", "invalid_zone_priority",
                    "zone priority must be an unsigned 32-bit integer" );
    }

    if( fields.contains( "border" ) )
        border = compileZoneBorder( aDocument, fields["border"], aResult );

    if( fields.contains( "locked" )
        && !parseBooleanForm( aDocument, fields["locked"], locked ) )
    {
        diagnostic( aResult, "error", "invalid_zone_locked",
                    "zone locked must be true or false" );
    }

    if( validIdentifier( net ) )
        aResult.netReferences.emplace_back( net );

    return { { "kind", "zone" },
             { "logicalId", logicalId },
             { "net", net },
             { "name", name },
             { "layers", std::move( layers ) },
             { "polygons", std::move( polygons ) },
             { "clearanceNm", clearance },
             { "minThicknessNm", minimumThickness },
             { "connection", std::move( connection ) },
             { "islands", std::move( islands ) },
             { "fill", std::move( fill ) },
             { "layerProperties", std::move( layerProperties ) },
             { "priority", priority },
             { "border", std::move( border ) },
             { "locked", locked },
             { "typed", true } };
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_BOARD_COMPILER::RESULT DESIGN_SCRIPT_BOARD_COMPILER::Compile(
        const LOSSLESS_SEXPR_DOCUMENT& aDocument, size_t aBoardNode )
{
    RESULT                       result;
    const DOCUMENT::NODE&        board = aDocument.Nodes()[aBoardNode];
    std::set<std::string>        logicalIds;
    std::set<std::string>        placedComponents;
    std::set<std::string>        singletonStatements;
    static const std::set<std::string> known = {
        "stackup", "outline", "place", "route", "via",
        "zone", "text", "dimension", "keepout"
    };

    for( size_t i = 1; i < board.children.size(); ++i )
    {
        const size_t      child = board.children[i];
        const std::string head = aDocument.ListHead( child );

        if( !known.contains( head ) )
        {
            diagnostic( result, "error", "unknown_board_statement",
                        "board statement '" + head + "' is not part of KDS version 1" );
            continue;
        }

        if( ( head == "stackup" || head == "outline" )
            && !singletonStatements.emplace( head ).second )
        {
            diagnostic( result, "error", "duplicate_board_statement",
                        "board " + head + " occurs more than once" );
        }

        if( head == "stackup" )
        {
            result.statements.emplace_back( compileStackup( aDocument, child, result ) );
        }
        else if( head == "outline" )
        {
            compileOutline( aDocument, child, result, logicalIds );
        }
        else if( head == "place" )
        {
            result.statements.emplace_back(
                    compilePlace( aDocument, child, result, placedComponents ) );
        }
        else if( head == "route" )
        {
            result.statements.emplace_back( compileRoute( aDocument, child, result, logicalIds ) );
        }
        else if( head == "via" )
        {
            result.statements.emplace_back( compileVia( aDocument, child, result, logicalIds ) );
        }
        else if( head == "zone" )
        {
            result.statements.emplace_back( compileZone( aDocument, child, result, logicalIds ) );
        }
        else
        {
            result.fullyTyped = false;
            result.statements.push_back( { { "kind", head },
                                           { "typed", false },
                                           { "expression", expressionToIr( aDocument, child ) } } );
        }
    }

    validateStackupLayers( result.statements, result );

    return result;
}

} // namespace KICHAD
