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


namespace
{

using JSON = nlohmann::json;
using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using RESULT = KICHAD::DESIGN_SCRIPT_BOARD_COMPILER::RESULT;

constexpr size_t MAX_IDENTIFIER_BYTES = 128;


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
