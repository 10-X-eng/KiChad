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
#include "kichad_from_chars.h"

#include "design_script_board_asset_compiler.h"
#include "design_script_board_graphic_compiler.h"
#include "design_script_board_outline_compiler.h"
#include "design_script_board_text_box_compiler.h"
#include "design_script_board_table_compiler.h"
#include "design_script_layout_compiler.h"
#include "design_script_physical_synthesis_compiler.h"
#include "design_script_teardrop_compiler.h"
#include "design_script_via_backdrill_compiler.h"
#include "design_script_via_padstack_compiler.h"
#include "design_script_via_protection_compiler.h"

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
constexpr size_t MAX_TEXT_BYTES = 64 * 1024;
constexpr size_t MAX_FONT_NAME_BYTES = 256;
constexpr size_t MAX_HYPERLINK_BYTES = 2048;
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
            KICHAD::FromChars( aText.data(), aText.data() + aText.size(), value );

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
            KICHAD::FromChars( aText.data(), aText.data() + aText.size(), value );

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
            KICHAD::FromChars( aText.data(), aText.data() + aText.size(), aDegrees );

    return converted.ec == std::errc() && converted.ptr != aText.data()
           && std::string_view( converted.ptr,
                                static_cast<size_t>( aText.data() + aText.size() - converted.ptr ) )
                      == "deg"
           && std::isfinite( aDegrees ) && std::abs( aDegrees ) <= 360000.0;
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
            KICHAD::FromChars( text.data(), text.data() + text.size(), aValue );
    return converted.ec == std::errc() && converted.ptr == text.data() + text.size()
           && std::isfinite( aValue ) && aValue >= 0.0 && aValue <= 1.0;
}


bool parseNumberForm( const DOCUMENT& aDocument, size_t aNode, double& aValue )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           text;

    if( node.kind != DOCUMENT::NODE_KIND::LIST || node.children.size() != 2
        || !scalarText( aDocument, node.children[1], text ) )
    {
        return false;
    }

    std::from_chars_result converted =
            KICHAD::FromChars( text.data(), text.data() + text.size(), aValue );
    return converted.ec == std::errc() && converted.ptr == text.data() + text.size()
           && std::isfinite( aValue );
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


bool boardLayer( const std::string& aLayer )
{
    if( copperLayer( aLayer ) )
        return true;

    static const std::set<std::string> FIXED_LAYERS = {
        "B.Adhes", "F.Adhes", "B.Paste", "F.Paste", "B.SilkS", "F.SilkS",
        "B.Mask", "F.Mask", "Dwgs.User", "Cmts.User", "Eco1.User", "Eco2.User",
        "Edge.Cuts", "Margin", "B.CrtYd", "F.CrtYd", "B.Fab", "F.Fab"
    };

    if( FIXED_LAYERS.contains( aLayer ) )
        return true;

    if( !aLayer.starts_with( "User." ) )
        return false;

    int layer = 0;
    const char* begin = aLayer.data() + 5;
    const char* end = aLayer.data() + aLayer.size();
    std::from_chars_result converted = std::from_chars( begin, end, layer );
    return converted.ec == std::errc() && converted.ptr == end && layer >= 1 && layer <= 45;
}


bool internalVector( const JSON& aVector )
{
    const int64_t x = aVector.value( "xNm", int64_t( 0 ) );
    const int64_t y = aVector.value( "yNm", int64_t( 0 ) );
    return x >= std::numeric_limits<int>::min() && x <= std::numeric_limits<int>::max()
           && y >= std::numeric_limits<int>::min() && y <= std::numeric_limits<int>::max();
}


JSON compileStackup( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 1,
                   { "finish", "impedance_controlled", "edge_connector", "edge_plating",
                     "layers" },
                   fields, aResult, "stackup" );
    std::string finish;
    bool        impedanceControlled = false;
    std::string edgeConnector;
    bool        edgePlating = false;

    if( !fields.contains( "finish" )
        || !parseScalarForm( aDocument, fields["finish"], finish ) || finish.empty()
        || finish.size() > 128 || finish.find( '\r' ) != std::string::npos
        || finish.find( '\n' ) != std::string::npos )
    {
        diagnostic( aResult, "error", "invalid_stackup_finish",
                    "stackup finish requires 1 through 128 UTF-8 bytes on one line" );
    }

    if( !fields.contains( "impedance_controlled" )
        || !parseBooleanForm( aDocument, fields["impedance_controlled"],
                              impedanceControlled ) )
    {
        diagnostic( aResult, "error", "invalid_stackup_impedance_policy",
                    "stackup impedance_controlled must be explicitly true or false" );
    }

    if( !fields.contains( "edge_connector" )
        || !parseScalarForm( aDocument, fields["edge_connector"], edgeConnector )
        || ( edgeConnector != "none" && edgeConnector != "yes"
             && edgeConnector != "bevelled" ) )
    {
        diagnostic( aResult, "error", "invalid_stackup_edge_connector",
                    "stackup edge_connector must be none, yes, or bevelled" );
    }

    if( !fields.contains( "edge_plating" )
        || !parseBooleanForm( aDocument, fields["edge_plating"], edgePlating ) )
    {
        diagnostic( aResult, "error", "invalid_stackup_edge_plating",
                    "stackup edge_plating must be explicitly true or false" );
    }

    JSON layers = JSON::array();
    int  copperLayers = 0;
    int  dielectricLayers = 0;
    int64_t totalThickness = 0;
    std::set<std::string> boardLayers;

    if( !fields.contains( "layers" ) )
    {
        diagnostic( aResult, "error", "invalid_stackup_layers",
                    "stackup requires one explicit top-to-bottom layers declaration" );
    }
    else
    {
        const DOCUMENT::NODE& declaration = aDocument.Nodes()[fields["layers"]];

        if( declaration.children.size() < 4 )
        {
            diagnostic( aResult, "error", "invalid_stackup_layers",
                        "stackup layers requires at least two copper layers and one dielectric" );
        }

        for( size_t index = 1; index < declaration.children.size(); ++index )
        {
            const size_t layerNode = declaration.children[index];
            const DOCUMENT::NODE& node = aDocument.Nodes()[layerNode];
            const std::string category = aDocument.ListHead( layerNode );

            if( category != "copper" && category != "dielectric"
                && category != "soldermask" && category != "solderpaste"
                && category != "silkscreen" )
            {
                diagnostic( aResult, "error", "invalid_stackup_layer_type",
                            "stackup layers accepts copper, dielectric, soldermask, "
                            "solderpaste, or silkscreen" );
                continue;
            }

            if( node.children.size() < 2 )
            {
                diagnostic( aResult, "error", "invalid_stackup_layer",
                            "each stackup layer requires a board layer or dielectric type" );
                continue;
            }

            std::string identity;

            if( !scalarText( aDocument, node.children[1], identity ) )
            {
                diagnostic( aResult, "error", "invalid_stackup_layer",
                            "stackup layer identity must be a scalar" );
                continue;
            }

            std::set<std::string> allowed;

            if( category == "copper" )
                allowed = { "thickness" };
            else if( category == "dielectric" )
                allowed = { "thickness", "material", "epsilon_r", "loss_tangent", "locked" };
            else if( category == "soldermask" )
                allowed = { "thickness", "material", "epsilon_r", "loss_tangent", "color" };
            else if( category == "silkscreen" )
                allowed = { "material", "color" };

            std::map<std::string, size_t> layerFields;
            collectFields( aDocument, layerNode, 2, allowed, layerFields, aResult,
                           "stackup " + category );
            JSON entry = { { "category", category }, { "enabled", true } };
            int64_t thickness = 0;
            std::string material;
            std::string color;
            double epsilonR = 0.0;
            double lossTangent = 0.0;
            bool locked = false;

            if( category == "dielectric" )
            {
                if( identity != "core" && identity != "prepreg" )
                {
                    diagnostic( aResult, "error", "invalid_stackup_dielectric_type",
                                "stackup dielectric type must be core or prepreg" );
                }

                entry["typeName"] = identity;
                entry["dielectricIndex"] = ++dielectricLayers;
            }
            else
            {
                const bool correctLayer =
                        ( category == "copper" && copperLayer( identity ) )
                        || ( category == "soldermask"
                             && ( identity == "F.Mask" || identity == "B.Mask" ) )
                        || ( category == "solderpaste"
                             && ( identity == "F.Paste" || identity == "B.Paste" ) )
                        || ( category == "silkscreen"
                             && ( identity == "F.SilkS" || identity == "B.SilkS" ) );

                if( !correctLayer || !boardLayers.emplace( identity ).second )
                {
                    diagnostic( aResult, "error", "invalid_stackup_board_layer",
                                "stackup contains an invalid or duplicate physical board layer" );
                }

                entry["layer"] = identity;

                if( category == "copper" )
                {
                    entry["typeName"] = "copper";
                    ++copperLayers;
                }
                else if( category == "soldermask" )
                {
                    entry["typeName"] = identity == "F.Mask" ? "Top Solder Mask"
                                                                  : "Bottom Solder Mask";
                }
                else if( category == "solderpaste" )
                {
                    entry["typeName"] = identity == "F.Paste" ? "Top Solder Paste"
                                                                   : "Bottom Solder Paste";
                }
                else
                {
                    entry["typeName"] = identity == "F.SilkS" ? "Top Silk Screen"
                                                                   : "Bottom Silk Screen";
                }
            }

            if( category == "copper" || category == "dielectric"
                || category == "soldermask" )
            {
                int64_t maximum = category == "copper" ? 1000000 : 10000000;

                if( !layerFields.contains( "thickness" )
                    || !parseDistanceForm( aDocument, layerFields["thickness"], thickness )
                    || thickness <= 0 || thickness > maximum )
                {
                    diagnostic( aResult, "error", "invalid_stackup_layer_thickness",
                                "stackup layer thickness is missing or outside its native range" );
                }

                if( thickness > 0 && totalThickness <= 20000000 - thickness )
                    totalThickness += thickness;
                else if( thickness > 0 )
                    totalThickness = 20000001;
            }

            if( category == "dielectric" || category == "soldermask" )
            {
                if( !layerFields.contains( "material" )
                    || !parseScalarForm( aDocument, layerFields["material"], material )
                    || material.empty() || material.size() > 128
                    || material.find( '\r' ) != std::string::npos
                    || material.find( '\n' ) != std::string::npos )
                {
                    diagnostic( aResult, "error", "invalid_stackup_material",
                                "stackup material requires 1 through 128 UTF-8 bytes on one line" );
                }

                if( !layerFields.contains( "epsilon_r" )
                    || !parseNumberForm( aDocument, layerFields["epsilon_r"], epsilonR )
                    || epsilonR < 1.0 || epsilonR > 100.0 )
                {
                    diagnostic( aResult, "error", "invalid_stackup_epsilon_r",
                                "stackup epsilon_r must be from 1 through 100" );
                }

                if( !layerFields.contains( "loss_tangent" )
                    || !parseRatioForm( aDocument, layerFields["loss_tangent"], lossTangent ) )
                {
                    diagnostic( aResult, "error", "invalid_stackup_loss_tangent",
                                "stackup loss_tangent must be from 0 through 1" );
                }
            }

            if( category == "dielectric" )
            {
                if( !layerFields.contains( "locked" )
                    || !parseBooleanForm( aDocument, layerFields["locked"], locked ) )
                {
                    diagnostic( aResult, "error", "invalid_stackup_dielectric_lock",
                                "stackup dielectric locked must be explicitly true or false" );
                }
            }

            if( category == "silkscreen" )
            {
                if( !layerFields.contains( "material" )
                    || !parseScalarForm( aDocument, layerFields["material"], material )
                    || material.empty() || material.size() > 128
                    || material.find( '\r' ) != std::string::npos
                    || material.find( '\n' ) != std::string::npos )
                {
                    diagnostic( aResult, "error", "invalid_stackup_material",
                                "stackup material requires 1 through 128 UTF-8 bytes on one line" );
                }
            }

            if( category == "silkscreen" || category == "soldermask" )
            {
                if( !layerFields.contains( "color" )
                    || !parseScalarForm( aDocument, layerFields["color"], color )
                    || color.empty() || color.size() > 64
                    || color.find( '\r' ) != std::string::npos
                    || color.find( '\n' ) != std::string::npos )
                {
                    diagnostic( aResult, "error", "invalid_stackup_color",
                                "stackup color requires 1 through 64 UTF-8 bytes on one line" );
                }
            }

            entry["thicknessNm"] = thickness;
            entry["material"] = material;
            entry["epsilonR"] = epsilonR;
            entry["lossTangent"] = lossTangent;
            entry["color"] = color;
            entry["locked"] = locked;
            layers.emplace_back( std::move( entry ) );
        }
    }

    if( copperLayers < 2 || copperLayers > 32 || copperLayers % 2 != 0
        || dielectricLayers != copperLayers - 1 )
    {
        diagnostic( aResult, "error", "invalid_stackup_layers",
                    "stackup requires 2 through 32 even copper layers with one dielectric "
                    "between every adjacent pair" );
    }

    int prefixRank = -1;
    int suffixRank = -1;
    bool sawCopper = false;
    bool expectDielectric = false;
    int  expectedInner = 1;

    for( const JSON& entry : layers )
    {
        const std::string category = entry.value( "category", "" );
        const std::string layer = entry.value( "layer", "" );

        if( !sawCopper )
        {
            if( category == "copper" )
            {
                sawCopper = true;
                expectDielectric = true;

                if( layer != "F.Cu" )
                    diagnostic( aResult, "error", "invalid_stackup_order",
                                "the first copper layer in a stackup must be F.Cu" );
            }
            else
            {
                int rank = layer == "F.SilkS" ? 0 : layer == "F.Paste" ? 1
                                                                  : layer == "F.Mask" ? 2 : -1;

                if( rank < 0 || rank <= prefixRank )
                    diagnostic( aResult, "error", "invalid_stackup_order",
                                "top technical layers must be F.SilkS, F.Paste, F.Mask in order" );

                prefixRank = rank;
            }

            continue;
        }

        if( layer == "B.Cu" )
        {
            if( expectDielectric )
                diagnostic( aResult, "error", "invalid_stackup_order",
                            "every adjacent copper pair requires one dielectric" );

            expectDielectric = false;
            suffixRank = 0;
            continue;
        }

        if( suffixRank >= 0 )
        {
            int rank = layer == "B.Mask" ? 1 : layer == "B.Paste" ? 2
                                                            : layer == "B.SilkS" ? 3 : -1;

            if( rank < 0 || rank <= suffixRank )
                diagnostic( aResult, "error", "invalid_stackup_order",
                            "bottom technical layers must be B.Mask, B.Paste, B.SilkS in order" );

            suffixRank = rank;
            continue;
        }

        if( expectDielectric )
        {
            if( category != "dielectric" )
                diagnostic( aResult, "error", "invalid_stackup_order",
                            "a dielectric must follow every copper layer except B.Cu" );

            expectDielectric = false;
        }
        else
        {
            const std::string expected = expectedInner <= copperLayers - 2
                                                 ? "In" + std::to_string( expectedInner ) + ".Cu"
                                                 : "B.Cu";

            if( category != "copper" || layer != expected )
                diagnostic( aResult, "error", "invalid_stackup_order",
                            "copper layers must be F.Cu, sequential inner layers, then B.Cu" );

            ++expectedInner;
            expectDielectric = true;
        }
    }

    if( suffixRank < 0 )
        diagnostic( aResult, "error", "invalid_stackup_order",
                    "the final copper layer in a stackup must be B.Cu" );

    if( totalThickness <= 0 || totalThickness > 20000000 )
        diagnostic( aResult, "error", "invalid_stackup_thickness",
                    "derived stackup thickness must be positive and at most 20mm" );

    return { { "kind", "stackup" },
             { "finish", finish },
             { "impedanceControlled", impedanceControlled },
             { "edgeConnector", edgeConnector },
             { "edgePlating", edgePlating },
             { "layers", std::move( layers ) },
             { "copperLayers", copperLayers },
             { "thicknessNm", totalThickness },
             { "typed", true } };
}


void compileOutline( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult,
                     std::set<std::string>& aLogicalIds )
{
    const DOCUMENT::NODE& outline = aDocument.Nodes()[aNode];

    if( outline.children.size() < 2 )
    {
        diagnostic( aResult, "error", "empty_outline",
                    "outline requires at least one Edge.Cuts graphic primitive" );
        return;
    }

    for( size_t i = 1; i < outline.children.size(); ++i )
    {
        KICHAD::DESIGN_SCRIPT_BOARD_OUTLINE_COMPILER::RESULT compiled =
                KICHAD::DESIGN_SCRIPT_BOARD_OUTLINE_COMPILER::Compile(
                        aDocument, outline.children[i] );

        for( JSON& entry : compiled.diagnostics )
            aResult.diagnostics.push_back( std::move( entry ) );

        const std::string logicalId = compiled.statement.value( "logicalId", "" );

        if( !logicalId.empty() && !aLogicalIds.emplace( logicalId ).second )
            diagnostic( aResult, "error", "duplicate_board_id",
                        "board logical id " + logicalId + " occurs more than once" );

        if( compiled.statement.is_object() && !logicalId.empty() )
            aResult.statements.push_back( std::move( compiled.statement ) );
    }
}


JSON compileFootprintFieldPresentation( const DOCUMENT& aDocument, size_t aNode,
                                        RESULT& aResult, const std::string& aName )
{
    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 1,
                   { "visible", "layer", "at", "size", "stroke", "angle", "justify",
                     "font", "bold", "italic", "underlined", "mirrored", "keep_upright" },
                   fields, aResult, "footprint " + aName + " field" );
    JSON result = JSON::object();

    if( fields.empty() )
    {
        diagnostic( aResult, "error", "empty_footprint_field_presentation",
                    "footprint " + aName + " presentation requires at least one setting" );
        return result;
    }

    if( fields.contains( "visible" ) )
    {
        bool visible = false;

        if( !parseBooleanForm( aDocument, fields["visible"], visible ) )
        {
            diagnostic( aResult, "error", "invalid_footprint_field_visibility",
                        "footprint " + aName + " visible must be true or false" );
        }
        else
        {
            result["visible"] = visible;
        }
    }

    if( fields.contains( "layer" ) )
    {
        std::string layer;

        if( !parseScalarForm( aDocument, fields["layer"], layer )
            || ( layer != "F.SilkS" && layer != "B.SilkS"
                 && layer != "F.Fab" && layer != "B.Fab" ) )
        {
            diagnostic( aResult, "error", "invalid_footprint_field_layer",
                        "footprint " + aName
                                + " layer must be F.SilkS, B.SilkS, F.Fab, or B.Fab" );
        }
        else
        {
            result["layer"] = std::move( layer );
        }
    }

    if( fields.contains( "at" ) )
    {
        JSON position;

        if( !parseVectorForm( aDocument, fields["at"], position )
            || !internalVector( position ) )
        {
            diagnostic( aResult, "error", "invalid_footprint_field_position",
                        "footprint " + aName
                                + " at requires bounded absolute coordinates with physical units" );
        }
        else
        {
            result["position"] = std::move( position );
        }
    }

    int64_t width = 0;
    int64_t height = 0;

    if( fields.contains( "size" ) )
    {
        JSON size;

        if( !parseVectorForm( aDocument, fields["size"], size )
            || ( width = size.value( "xNm", int64_t( 0 ) ) ) < 1000
            || width > 250000000
            || ( height = size.value( "yNm", int64_t( 0 ) ) ) < 1000
            || height > 250000000 )
        {
            diagnostic( aResult, "error", "invalid_footprint_field_size",
                        "footprint " + aName
                                + " size requires width and height from 1um through 250mm" );
        }
        else
        {
            result["size"] = std::move( size );
        }
    }

    if( fields.contains( "stroke" ) )
    {
        int64_t stroke = 0;
        const int64_t maximum = width > 0 && height > 0
                                        ? std::min( width, height ) / 4
                                        : 250000000;

        if( !parseDistanceForm( aDocument, fields["stroke"], stroke )
            || stroke <= 0 || stroke > maximum )
        {
            diagnostic( aResult, "error", "invalid_footprint_field_stroke",
                        "footprint " + aName
                                + " stroke must be positive and no more than one quarter of its supplied size" );
        }
        else
        {
            result["strokeNm"] = stroke;
        }
    }

    if( fields.contains( "angle" ) )
    {
        double angle = 0.0;

        if( !parseAngleForm( aDocument, fields["angle"], angle ) )
        {
            diagnostic( aResult, "error", "invalid_footprint_field_angle",
                        "footprint " + aName + " angle must use an explicit deg suffix" );
        }
        else
        {
            result["angleDegrees"] = angle;
        }
    }

    if( fields.contains( "justify" ) )
    {
        const DOCUMENT::NODE& justify = aDocument.Nodes()[fields["justify"]];
        std::string horizontal;
        std::string vertical;

        if( justify.children.size() != 3
            || !scalarText( aDocument, justify.children[1], horizontal )
            || !scalarText( aDocument, justify.children[2], vertical )
            || ( horizontal != "left" && horizontal != "center" && horizontal != "right" )
            || ( vertical != "top" && vertical != "center" && vertical != "bottom" ) )
        {
            diagnostic( aResult, "error", "invalid_footprint_field_justification",
                        "footprint " + aName
                                + " justify requires left|center|right and top|center|bottom" );
        }
        else
        {
            result["horizontalJustification"] = std::move( horizontal );
            result["verticalJustification"] = std::move( vertical );
        }
    }

    if( fields.contains( "font" ) )
    {
        std::string font;

        if( !parseScalarForm( aDocument, fields["font"], font ) || font.empty()
            || font.size() > MAX_FONT_NAME_BYTES )
        {
            diagnostic( aResult, "error", "invalid_footprint_field_font",
                        "footprint " + aName
                                + " font must be stroke or a font name of at most 256 UTF-8 bytes" );
        }
        else
        {
            result["fontName"] = font == "stroke" ? "" : font;
        }
    }

    for( const char* field : { "bold", "italic", "underlined", "mirrored", "keep_upright" } )
    {
        if( !fields.contains( field ) )
            continue;

        bool value = false;

        if( !parseBooleanForm( aDocument, fields[field], value ) )
        {
            diagnostic( aResult, "error", "invalid_footprint_field_boolean",
                        "footprint " + aName + " " + field + " must be true or false" );
        }
        else
        {
            result[field == std::string( "keep_upright" ) ? "keepUpright" : field] = value;
        }
    }

    return result;
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
    collectFields( aDocument, aNode, 2,
                   { "at", "rotation", "side", "locked", "reference", "value" }, fields,
                   aResult, "place" );
    JSON        position = JSON::object();
    double      rotation = 0.0;
    std::string side = "front";
    bool        locked = false;
    JSON        presentation = JSON::object();

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

    for( const char* field : { "reference", "value" } )
    {
        if( fields.contains( field ) )
        {
            presentation[field] = compileFootprintFieldPresentation(
                    aDocument, fields[field], aResult, field );
        }
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
             { "presentation", std::move( presentation ) },
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
        else if( kind == "text" || kind == "dimension" )
        {
            const std::string layer = statement.value( "layer", "" );

            if( copperLayer( layer ) && copperLayerIndex( layer, copperLayers ) < 0 )
            {
                diagnostic( aResult, "error", kind + "_layer_outside_stackup",
                            kind + " layer " + layer
                                    + " is not present in the declared stackup" );
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

            const JSON& padstack = statement.value( "padstack", JSON( nullptr ) );

            if( padstack.is_object() && padstack.value( "mode", "" ) == "custom" )
            {
                const int first = std::min( start, end );
                const int last = std::max( start, end );
                std::set<int> authored;

                for( const JSON& layer : padstack.value( "layers", JSON::array() ) )
                {
                    const std::string name = layer.value( "name", "" );
                    const int index = copperLayerIndex( name, copperLayers );

                    if( index < first || index > last )
                        diagnostic( aResult, "error", "via_padstack_layer_outside_span",
                                    "custom via padstack layer " + name
                                            + " is outside the drilled span" );
                    else
                        authored.emplace( index );
                }

                for( int index = first; index <= last; ++index )
                {
                    if( !authored.contains( index ) )
                    {
                        diagnostic( aResult, "error", "incomplete_via_padstack_span",
                                    "custom via padstack must define every copper layer in its span" );
                        break;
                    }
                }
            }

            const int first = std::min( start, end );
            const int last = std::max( start, end );

            for( const JSON& layer : statement.value( "forceFlashLayers", JSON::array() ) )
            {
                const std::string name = layer.is_string() ? layer.get<std::string>() : "";
                const int index = copperLayerIndex( name, copperLayers );

                if( index < first || index > last )
                    diagnostic( aResult, "error", "via_force_flash_layer_outside_span",
                                "force_flash layer " + name
                                        + " is outside the via's drilled span" );
            }

            const JSON& backdrills = statement.value( "backdrills", JSON( nullptr ) );

            if( backdrills.is_object() )
            {
                int topStop = -1;
                int bottomStop = -1;

                if( backdrills["top"].is_object() )
                    topStop = copperLayerIndex(
                            backdrills["top"].value( "stopLayer", "" ), copperLayers );

                if( backdrills["bottom"].is_object() )
                    bottomStop = copperLayerIndex(
                            backdrills["bottom"].value( "stopLayer", "" ), copperLayers );

                if( ( topStop >= 0 && ( topStop == 0 || topStop == copperLayers - 1 ) )
                    || ( bottomStop >= 0
                         && ( bottomStop == 0 || bottomStop == copperLayers - 1 ) ) )
                {
                    diagnostic( aResult, "error", "invalid_via_backdrill_stop_layer",
                                "backdrill must stop on an inner copper layer" );
                }

                if( ( backdrills["top"].is_object() && topStop < 0 )
                    || ( backdrills["bottom"].is_object() && bottomStop < 0 ) )
                {
                    diagnostic( aResult, "error", "via_backdrill_layer_outside_stackup",
                                "backdrill stop layer is absent from the board stackup" );
                }

                if( topStop >= 0 && bottomStop >= 0 && topStop >= bottomStop )
                    diagnostic( aResult, "error", "overlapping_via_backdrills",
                                "top and bottom backdrills must leave a plated layer span" );
            }
        }
        else if( kind == "zone" || kind == "keepout" )
        {
            for( const JSON& layer : statement.value( "layers", JSON::array() ) )
            {
                const std::string layerName = layer.is_string() ? layer.get<std::string>() : "";

                if( copperLayer( layerName )
                    && copperLayerIndex( layerName, copperLayers ) < 0 )
                {
                    const std::string code = kind == "zone"
                                                     ? "zone_layer_outside_stackup"
                                                     : "keepout_layer_outside_stackup";
                    diagnostic( aResult, "error", code,
                                kind + " layer " + layerName
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
                   { "id", "at", "diameter", "drill", "layers", "type", "locked",
                     "padstack", "protection", "backdrills", "unconnected_layers",
                     "force_flash", "teardrop" },
                   fields, aResult, "via" );
    std::string logicalId;
    JSON        position = JSON::object();
    int64_t     diameter = 0;
    int64_t     drill = 0;
    std::string startLayer;
    std::string endLayer;
    std::string type = "through";
    bool        locked = false;
    JSON        padstack = nullptr;
    JSON        protection = nullptr;
    JSON        backdrills = nullptr;
    JSON        teardrop = nullptr;
    std::string unconnectedLayers = "keep";
    JSON        forceFlashLayers = JSON::array();

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

    if( fields.contains( "diameter" )
        && ( !parseDistanceForm( aDocument, fields["diameter"], diameter ) || diameter <= 0 ) )
    {
        diagnostic( aResult, "error", "invalid_via_diameter",
                    "via diameter must be a positive physical distance" );
    }

    if( !fields.contains( "drill" ) || !parseDistanceForm( aDocument, fields["drill"], drill )
        || drill <= 0 )
    {
        diagnostic( aResult, "error", "invalid_via_drill",
                    "via drill must be a positive physical distance" );
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

    if( fields.contains( "unconnected_layers" )
        && ( !parseScalarForm( aDocument, fields["unconnected_layers"],
                              unconnectedLayers )
             || ( unconnectedLayers != "keep" && unconnectedLayers != "remove"
                  && unconnectedLayers != "keep_start_end"
                  && unconnectedLayers != "start_end_only" ) ) )
    {
        diagnostic( aResult, "error", "invalid_via_unconnected_layer_policy",
                    "unconnected_layers must be keep, remove, keep_start_end, or "
                    "start_end_only" );
    }

    if( fields.contains( "force_flash" ) )
    {
        const DOCUMENT::NODE& forceFlash = aDocument.Nodes()[fields["force_flash"]];
        std::set<std::string> unique;

        if( forceFlash.children.size() < 2 )
            diagnostic( aResult, "error", "empty_via_force_flash",
                        "force_flash requires at least one copper layer" );

        for( size_t index = 1; index < forceFlash.children.size(); ++index )
        {
            std::string layer;

            if( !scalarText( aDocument, forceFlash.children[index], layer )
                || !copperLayer( layer ) || !unique.emplace( layer ).second )
            {
                diagnostic( aResult, "error", "invalid_via_force_flash_layer",
                            "force_flash requires unique copper-layer names" );
            }
            else
            {
                forceFlashLayers.push_back( layer );
            }
        }

        if( unconnectedLayers == "keep" )
            diagnostic( aResult, "error", "redundant_via_force_flash",
                        "force_flash requires an unconnected_layers removal policy" );
    }

    if( fields.contains( "padstack" ) )
    {
        KICHAD::DESIGN_SCRIPT_VIA_PADSTACK_COMPILER::RESULT compiled =
                KICHAD::DESIGN_SCRIPT_VIA_PADSTACK_COMPILER::Compile(
                        aDocument, fields["padstack"] );

        for( JSON& entry : compiled.diagnostics )
            aResult.diagnostics.push_back( std::move( entry ) );

        padstack = std::move( compiled.padstack );
    }

    if( padstack.is_object() == fields.contains( "diameter" ) )
    {
        diagnostic( aResult, "error", "ambiguous_via_copper_geometry",
                    "via requires exactly one copper representation: diameter or padstack" );
    }

    if( !padstack.is_object() && diameter > 0 && drill >= diameter )
        diagnostic( aResult, "error", "invalid_via_drill",
                    "simple via drill must be smaller than its diameter" );

    if( padstack.is_object() )
    {
        for( const JSON& layer : padstack["layers"] )
        {
            if( layer.contains( "size" )
                && ( drill >= layer["size"]["xNm"].get<int64_t>()
                     || drill >= layer["size"]["yNm"].get<int64_t>() ) )
            {
                diagnostic( aResult, "error", "invalid_via_padstack_annulus",
                            "every via padstack copper shape must exceed the drill" );
            }
        }

        if( padstack.value( "mode", "" ) == "front_inner_back" && type != "through" )
            diagnostic( aResult, "error", "invalid_via_padstack_mode",
                        "front_inner_back padstacks apply only to through vias" );
    }

    if( fields.contains( "protection" ) )
    {
        KICHAD::DESIGN_SCRIPT_VIA_PROTECTION_COMPILER::RESULT compiled =
                KICHAD::DESIGN_SCRIPT_VIA_PROTECTION_COMPILER::Compile(
                        aDocument, fields["protection"] );

        for( JSON& entry : compiled.diagnostics )
            aResult.diagnostics.push_back( std::move( entry ) );

        protection = std::move( compiled.protection );
    }

    if( fields.contains( "backdrills" ) )
    {
        KICHAD::DESIGN_SCRIPT_VIA_BACKDRILL_COMPILER::RESULT compiled =
                KICHAD::DESIGN_SCRIPT_VIA_BACKDRILL_COMPILER::Compile(
                        aDocument, fields["backdrills"] );

        for( JSON& entry : compiled.diagnostics )
            aResult.diagnostics.push_back( std::move( entry ) );

        backdrills = std::move( compiled.backdrills );
    }

    if( fields.contains( "teardrop" ) )
    {
        KICHAD::DESIGN_SCRIPT_TEARDROP_COMPILER::RESULT compiled =
                KICHAD::DESIGN_SCRIPT_TEARDROP_COMPILER::Compile(
                        aDocument, fields["teardrop"] );

        for( JSON& entry : compiled.diagnostics )
            aResult.diagnostics.push_back( std::move( entry ) );

        teardrop = std::move( compiled.teardrop );
    }

    if( backdrills.is_object() )
    {
        if( type != "through" )
            diagnostic( aResult, "error", "invalid_via_backdrill_type",
                        "top/bottom backdrilling requires a through via" );

        for( const char* side : { "top", "bottom" } )
        {
            if( !backdrills[side].is_object() )
                continue;

            if( backdrills[side].value( "diameterNm", int64_t{ 0 } ) <= drill )
                diagnostic( aResult, "error", "invalid_via_backdrill_diameter",
                            "backdrill diameter must exceed the primary via drill" );

            if( !copperLayer( backdrills[side].value( "stopLayer", "" ) ) )
                diagnostic( aResult, "error", "invalid_via_backdrill_stop_layer",
                            "backdrill stop_layer must name a copper layer" );
        }
    }

    if( protection.is_object() )
    {
        const auto sideIsAuthored = [&]( const char* aFeature, const char* aSide )
        {
            return protection[aFeature].is_object()
                   && protection[aFeature].value( aSide, "inherit" ) != "inherit";
        };

        if( startLayer != "F.Cu" && endLayer != "F.Cu"
            && ( sideIsAuthored( "tenting", "front" )
                 || sideIsAuthored( "covering", "front" )
                 || sideIsAuthored( "plugging", "front" )
                 || protection["frontPostMachining"].is_object() ) )
        {
            diagnostic( aResult, "error", "invalid_via_front_protection",
                        "front-side via treatment requires a span that reaches F.Cu" );
        }

        if( startLayer != "B.Cu" && endLayer != "B.Cu"
            && ( sideIsAuthored( "tenting", "back" )
                 || sideIsAuthored( "covering", "back" )
                 || sideIsAuthored( "plugging", "back" )
                 || protection["backPostMachining"].is_object() ) )
        {
            diagnostic( aResult, "error", "invalid_via_back_protection",
                        "back-side via treatment requires a span that reaches B.Cu" );
        }

        for( const char* field : { "frontPostMachining", "backPostMachining" } )
        {
            if( protection[field].is_object()
                && protection[field].value( "diameterNm", int64_t{ 0 } ) <= drill )
            {
                diagnostic( aResult, "error", "invalid_via_post_machining_diameter",
                            "post-machining diameter must exceed the primary via drill" );
            }
        }
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
             { "padstack", std::move( padstack ) },
             { "protection", std::move( protection ) },
             { "backdrills", std::move( backdrills ) },
             { "teardrop", std::move( teardrop ) },
             { "unconnectedLayers", unconnectedLayers },
             { "forceFlashLayers", std::move( forceFlashLayers ) },
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


JSON compileText( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult,
                  std::set<std::string>& aLogicalIds )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           value;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], value )
        || value.empty() || value.size() > MAX_TEXT_BYTES
        || value.find( '\0' ) != std::string::npos || value.find( '\r' ) != std::string::npos )
    {
        diagnostic( aResult, "error", "invalid_text_value",
                    "text requires 1 through 65536 UTF-8 bytes and LF line endings" );
    }

    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 2,
                   { "id", "layer", "at", "size", "stroke", "angle", "justify",
                     "font", "line_spacing", "bold", "italic", "underlined", "mirrored",
                     "keep_upright", "hyperlink", "knockout", "locked" },
                   fields, aResult, "text" );
    std::string logicalId;
    std::string layer;
    JSON        position = JSON::object();
    JSON        size = JSON::object();
    int64_t     stroke = 0;
    double      angle = 0.0;
    std::string horizontal = "center";
    std::string vertical = "center";
    std::string fontName;
    double      lineSpacing = 1.0;
    bool        bold = false;
    bool        italic = false;
    bool        underlined = false;
    bool        mirrored = false;
    bool        keepUpright = false;
    std::string hyperlink;
    bool        knockout = false;
    bool        locked = false;

    if( !fields.contains( "id" ) || !parseScalarForm( aDocument, fields["id"], logicalId )
        || !validIdentifier( logicalId ) )
    {
        diagnostic( aResult, "error", "invalid_board_id",
                    "text requires a bounded (id LOGICAL_ID)" );
    }
    else if( !aLogicalIds.emplace( logicalId ).second )
    {
        diagnostic( aResult, "error", "duplicate_board_id",
                    "board logical id " + logicalId + " occurs more than once" );
    }

    if( !fields.contains( "layer" ) || !parseScalarForm( aDocument, fields["layer"], layer )
        || !boardLayer( layer ) )
    {
        diagnostic( aResult, "error", "invalid_text_layer",
                    "text requires a named KiCad board layer" );
    }

    if( !fields.contains( "at" ) || !parseVectorForm( aDocument, fields["at"], position )
        || !internalVector( position ) )
    {
        diagnostic( aResult, "error", "invalid_text_position",
                    "text requires bounded (at X Y) coordinates with physical units" );
    }

    if( !fields.contains( "size" ) || !parseVectorForm( aDocument, fields["size"], size )
        || size.value( "xNm", int64_t( 0 ) ) < 1000
        || size.value( "xNm", int64_t( 0 ) ) > 250000000
        || size.value( "yNm", int64_t( 0 ) ) < 1000
        || size.value( "yNm", int64_t( 0 ) ) > 250000000 )
    {
        diagnostic( aResult, "error", "invalid_text_size",
                    "text size requires width and height from 1um through 250mm" );
    }

    const int64_t maximumStroke =
            std::min( size.value( "xNm", int64_t( 0 ) ),
                      size.value( "yNm", int64_t( 0 ) ) )
            / 4;

    if( !fields.contains( "stroke" )
        || !parseDistanceForm( aDocument, fields["stroke"], stroke ) || stroke <= 0
        || stroke > maximumStroke )
    {
        diagnostic( aResult, "error", "invalid_text_stroke",
                    "text stroke must be positive and at most one quarter of its smaller size" );
    }

    if( fields.contains( "angle" )
        && !parseAngleForm( aDocument, fields["angle"], angle ) )
    {
        diagnostic( aResult, "error", "invalid_text_angle",
                    "text angle must use an explicit deg suffix" );
    }

    if( fields.contains( "justify" ) )
    {
        const DOCUMENT::NODE& justify = aDocument.Nodes()[fields["justify"]];

        if( justify.children.size() != 3
            || !scalarText( aDocument, justify.children[1], horizontal )
            || !scalarText( aDocument, justify.children[2], vertical )
            || ( horizontal != "left" && horizontal != "center" && horizontal != "right" )
            || ( vertical != "top" && vertical != "center" && vertical != "bottom" ) )
        {
            diagnostic( aResult, "error", "invalid_text_justification",
                        "text justify requires left|center|right and top|center|bottom" );
        }
    }

    if( fields.contains( "font" ) )
    {
        std::string font;

        if( !parseScalarForm( aDocument, fields["font"], font ) || font.empty()
            || font.size() > MAX_FONT_NAME_BYTES )
        {
            diagnostic( aResult, "error", "invalid_text_font",
                        "text font must be stroke or a font name of at most 256 UTF-8 bytes" );
        }
        else if( font != "stroke" )
        {
            fontName = std::move( font );
        }
    }

    if( fields.contains( "line_spacing" )
        && ( !parseNumberForm( aDocument, fields["line_spacing"], lineSpacing )
             || lineSpacing < 0.1 || lineSpacing > 10.0 ) )
    {
        diagnostic( aResult, "error", "invalid_text_line_spacing",
                    "text line_spacing must be a number from 0.1 through 10" );
    }

    const auto parseTextBoolean = [&]( const char* aField, bool& aValue )
    {
        if( fields.contains( aField )
            && !parseBooleanForm( aDocument, fields[aField], aValue ) )
        {
            diagnostic( aResult, "error", "invalid_text_boolean",
                        std::string( "text " ) + aField + " must be true or false" );
        }
    };
    parseTextBoolean( "bold", bold );
    parseTextBoolean( "italic", italic );
    parseTextBoolean( "underlined", underlined );
    parseTextBoolean( "mirrored", mirrored );
    parseTextBoolean( "keep_upright", keepUpright );
    parseTextBoolean( "knockout", knockout );
    parseTextBoolean( "locked", locked );

    if( fields.contains( "hyperlink" )
        && ( !parseScalarForm( aDocument, fields["hyperlink"], hyperlink )
             || hyperlink.size() > MAX_HYPERLINK_BYTES
             || hyperlink.find( '\0' ) != std::string::npos
             || hyperlink.find( '\r' ) != std::string::npos
             || hyperlink.find( '\n' ) != std::string::npos ) )
    {
        diagnostic( aResult, "error", "invalid_text_hyperlink",
                    "text hyperlink must be a single line of at most 2048 UTF-8 bytes" );
    }

    return { { "kind", "text" },
             { "logicalId", logicalId },
             { "value", value },
             { "layer", layer },
             { "position", std::move( position ) },
             { "size", std::move( size ) },
             { "strokeNm", stroke },
             { "angleDegrees", angle },
             { "horizontalJustification", horizontal },
             { "verticalJustification", vertical },
             { "fontName", fontName },
             { "lineSpacing", lineSpacing },
             { "bold", bold },
             { "italic", italic },
             { "underlined", underlined },
             { "mirrored", mirrored },
             { "multiline", value.find( '\n' ) != std::string::npos },
             { "keepUpright", keepUpright },
             { "hyperlink", hyperlink },
             { "knockout", knockout },
             { "locked", locked },
             { "typed", true } };
}


JSON compileDimensionText( const DOCUMENT& aDocument,
                           const std::map<std::string, size_t>& aFields,
                           RESULT& aResult )
{
    JSON        position = { { "xNm", 0 }, { "yNm", 0 } };
    JSON        size = JSON::object();
    int64_t     stroke = 0;
    double      angle = 0.0;
    std::string horizontal = "center";
    std::string vertical = "center";
    std::string fontName;
    bool        bold = false;
    bool        italic = false;
    bool        underlined = false;
    bool        mirrored = false;

    if( aFields.contains( "text_at" )
        && ( !parseVectorForm( aDocument, aFields.at( "text_at" ), position )
             || !internalVector( position ) ) )
    {
        diagnostic( aResult, "error", "invalid_dimension_text_position",
                    "dimension text_at requires bounded coordinates with physical units" );
    }

    if( !aFields.contains( "text_size" )
        || !parseVectorForm( aDocument, aFields.at( "text_size" ), size )
        || size.value( "xNm", int64_t( 0 ) ) < 1000
        || size.value( "xNm", int64_t( 0 ) ) > 250000000
        || size.value( "yNm", int64_t( 0 ) ) < 1000
        || size.value( "yNm", int64_t( 0 ) ) > 250000000 )
    {
        diagnostic( aResult, "error", "invalid_dimension_text_size",
                    "dimension text_size requires width and height from 1um through 250mm" );
    }

    const int64_t maximumStroke =
            std::min( size.value( "xNm", int64_t( 0 ) ),
                      size.value( "yNm", int64_t( 0 ) ) )
            / 4;

    if( !aFields.contains( "text_stroke" )
        || !parseDistanceForm( aDocument, aFields.at( "text_stroke" ), stroke )
        || stroke <= 0 || stroke > maximumStroke )
    {
        diagnostic( aResult, "error", "invalid_dimension_text_stroke",
                    "dimension text_stroke must be positive and at most one quarter of text_size" );
    }

    if( aFields.contains( "text_angle" )
        && !parseAngleForm( aDocument, aFields.at( "text_angle" ), angle ) )
    {
        diagnostic( aResult, "error", "invalid_dimension_text_angle",
                    "dimension text_angle must use an explicit deg suffix" );
    }

    if( aFields.contains( "text_justify" ) )
    {
        const DOCUMENT::NODE& justify = aDocument.Nodes()[aFields.at( "text_justify" )];

        if( justify.children.size() != 3
            || !scalarText( aDocument, justify.children[1], horizontal )
            || !scalarText( aDocument, justify.children[2], vertical )
            || ( horizontal != "left" && horizontal != "center" && horizontal != "right" )
            || ( vertical != "top" && vertical != "center" && vertical != "bottom" ) )
        {
            diagnostic( aResult, "error", "invalid_dimension_text_justification",
                        "dimension text_justify requires horizontal and vertical alignment" );
        }
    }

    if( aFields.contains( "font" ) )
    {
        std::string font;

        if( !parseScalarForm( aDocument, aFields.at( "font" ), font ) || font.empty()
            || font.size() > MAX_FONT_NAME_BYTES )
        {
            diagnostic( aResult, "error", "invalid_dimension_font",
                        "dimension font must be stroke or a bounded installed font name" );
        }
        else if( font != "stroke" )
        {
            fontName = std::move( font );
        }
    }

    const auto parseBoolean = [&]( const char* aField, bool& aValue )
    {
        if( aFields.contains( aField )
            && !parseBooleanForm( aDocument, aFields.at( aField ), aValue ) )
        {
            diagnostic( aResult, "error", "invalid_dimension_text_boolean",
                        std::string( "dimension " ) + aField + " must be true or false" );
        }
    };
    parseBoolean( "bold", bold );
    parseBoolean( "italic", italic );
    parseBoolean( "underlined", underlined );
    parseBoolean( "mirrored", mirrored );

    return { { "position", std::move( position ) },
             { "size", std::move( size ) },
             { "strokeNm", stroke },
             { "angleDegrees", angle },
             { "angleExplicit", aFields.contains( "text_angle" ) },
             { "horizontalJustification", horizontal },
             { "verticalJustification", vertical },
             { "fontName", fontName },
             { "bold", bold },
             { "italic", italic },
             { "underlined", underlined },
             { "mirrored", mirrored } };
}


JSON compileDimension( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult,
                       std::set<std::string>& aLogicalIds )
{
    const DOCUMENT::NODE& node = aDocument.Nodes()[aNode];
    std::string           style;

    if( node.children.size() < 2 || !scalarText( aDocument, node.children[1], style )
        || ( style != "aligned" && style != "orthogonal" && style != "radial"
             && style != "leader" && style != "center" ) )
    {
        diagnostic( aResult, "error", "invalid_dimension_style",
                    "dimension style must be aligned, orthogonal, radial, leader, or center" );
    }

    std::set<std::string> allowed = { "id", "layer", "line_width", "arrow_length", "locked" };
    const std::set<std::string> textFields = {
        "text_at", "text_size", "text_stroke", "text_angle", "text_justify", "font",
        "bold", "italic", "underlined", "mirrored"
    };
    const std::set<std::string> measurementFields = {
        "units", "unit_format", "precision", "suppress_trailing_zeroes", "prefix",
        "suffix", "override"
    };

    if( style != "center" )
        allowed.insert( textFields.begin(), textFields.end() );

    if( style == "aligned" || style == "orthogonal" || style == "radial" )
        allowed.insert( measurementFields.begin(), measurementFields.end() );

    if( style == "aligned" || style == "orthogonal" )
    {
        allowed.insert( { "from", "to", "height", "extension_height", "extension_offset",
                          "arrow_direction", "text_position", "keep_text_aligned" } );
    }

    if( style == "orthogonal" )
        allowed.emplace( "axis" );
    else if( style == "radial" )
        allowed.insert( { "center", "radius_point", "leader_length", "keep_text_aligned" } );
    else if( style == "leader" )
        allowed.insert( { "from", "to", "border", "label", "extension_offset" } );
    else if( style == "center" )
        allowed.insert( { "center", "to" } );

    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 2, allowed, fields, aResult, "dimension " + style );
    std::string logicalId;
    std::string layer;
    int64_t lineWidth = 0;
    int64_t arrowLength = 0;
    int64_t extensionOffset = 0;
    bool locked = false;

    if( !fields.contains( "id" ) || !parseScalarForm( aDocument, fields["id"], logicalId )
        || !validIdentifier( logicalId ) )
    {
        diagnostic( aResult, "error", "invalid_board_id",
                    "dimension requires a bounded (id LOGICAL_ID)" );
    }
    else if( !aLogicalIds.emplace( logicalId ).second )
    {
        diagnostic( aResult, "error", "duplicate_board_id",
                    "board logical id " + logicalId + " occurs more than once" );
    }

    if( !fields.contains( "layer" ) || !parseScalarForm( aDocument, fields["layer"], layer )
        || !boardLayer( layer ) )
    {
        diagnostic( aResult, "error", "invalid_dimension_layer",
                    "dimension requires a named KiCad board layer" );
    }

    if( !fields.contains( "line_width" )
        || !parseDistanceForm( aDocument, fields["line_width"], lineWidth )
        || !internalDistance( lineWidth ) )
    {
        diagnostic( aResult, "error", "invalid_dimension_line_width",
                    "dimension requires a positive bounded line_width" );
    }

    if( !fields.contains( "arrow_length" )
        || !parseDistanceForm( aDocument, fields["arrow_length"], arrowLength )
        || !internalDistance( arrowLength ) )
    {
        diagnostic( aResult, "error", "invalid_dimension_arrow_length",
                    "dimension requires a positive bounded arrow_length" );
    }

    if( fields.contains( "extension_offset" )
        && ( !parseDistanceForm( aDocument, fields["extension_offset"], extensionOffset )
             || !internalDistance( extensionOffset, true ) ) )
    {
        diagnostic( aResult, "error", "invalid_dimension_extension_offset",
                    "dimension extension_offset must be a non-negative bounded distance" );
    }

    if( fields.contains( "locked" )
        && !parseBooleanForm( aDocument, fields["locked"], locked ) )
    {
        diagnostic( aResult, "error", "invalid_dimension_locked",
                    "dimension locked must be true or false" );
    }

    JSON geometry = JSON::object();
    const auto parsePoint = [&]( const char* aField, JSON& aPoint )
    {
        if( !fields.contains( aField )
            || !parseVectorForm( aDocument, fields[aField], aPoint )
            || !internalVector( aPoint ) )
        {
            diagnostic( aResult, "error", "invalid_dimension_geometry",
                        std::string( "dimension requires bounded " ) + aField
                                + " coordinates with physical units" );
        }
    };

    if( style == "aligned" || style == "orthogonal" )
    {
        JSON start = JSON::object();
        JSON end = JSON::object();
        int64_t height = 0;
        int64_t extensionHeight = 0;
        parsePoint( "from", start );
        parsePoint( "to", end );

        if( !fields.contains( "height" )
            || !parseDistanceForm( aDocument, fields["height"], height ) || height == 0
            || height < std::numeric_limits<int>::min()
            || height > std::numeric_limits<int>::max() )
        {
            diagnostic( aResult, "error", "invalid_dimension_height",
                        "aligned and orthogonal dimensions require a non-zero bounded height" );
        }

        if( !fields.contains( "extension_height" )
            || !parseDistanceForm( aDocument, fields["extension_height"], extensionHeight )
            || !internalDistance( extensionHeight, true ) )
        {
            diagnostic( aResult, "error", "invalid_dimension_extension_height",
                        "dimension extension_height must be a non-negative bounded distance" );
        }

        if( !start.empty() && start == end )
            diagnostic( aResult, "error", "zero_length_dimension",
                        "dimension endpoints must differ" );

        geometry = { { "start", std::move( start ) }, { "end", std::move( end ) },
                     { "heightNm", height }, { "extensionHeightNm", extensionHeight } };

        if( style == "orthogonal" )
        {
            std::string axis;

            if( !fields.contains( "axis" ) || !parseScalarForm( aDocument, fields["axis"], axis )
                || ( axis != "x" && axis != "y" ) )
            {
                diagnostic( aResult, "error", "invalid_dimension_axis",
                            "orthogonal dimension axis must be x or y" );
            }

            geometry["axis"] = axis;
        }
    }
    else if( style == "radial" )
    {
        JSON center = JSON::object();
        JSON radiusPoint = JSON::object();
        int64_t leaderLength = 0;
        parsePoint( "center", center );
        parsePoint( "radius_point", radiusPoint );

        if( !fields.contains( "leader_length" )
            || !parseDistanceForm( aDocument, fields["leader_length"], leaderLength )
            || !internalDistance( leaderLength ) )
        {
            diagnostic( aResult, "error", "invalid_dimension_leader_length",
                        "radial dimension requires a positive bounded leader_length" );
        }

        if( !center.empty() && center == radiusPoint )
            diagnostic( aResult, "error", "zero_length_dimension",
                        "radial dimension radius_point must differ from center" );

        geometry = { { "center", std::move( center ) },
                     { "radiusPoint", std::move( radiusPoint ) },
                     { "leaderLengthNm", leaderLength } };
    }
    else if( style == "leader" )
    {
        JSON start = JSON::object();
        JSON end = JSON::object();
        std::string border;
        std::string label;
        parsePoint( "from", start );
        parsePoint( "to", end );

        if( !fields.contains( "border" ) || !parseScalarForm( aDocument, fields["border"], border )
            || ( border != "none" && border != "rectangle" && border != "circle"
                 && border != "roundrect" ) )
        {
            diagnostic( aResult, "error", "invalid_dimension_border",
                        "leader dimension border must be none, rectangle, circle, or roundrect" );
        }

        if( !fields.contains( "label" ) || !parseScalarForm( aDocument, fields["label"], label )
            || label.empty() || label.size() > MAX_TEXT_BYTES
            || label.find( '\r' ) != std::string::npos
            || label.find( '\n' ) != std::string::npos )
        {
            diagnostic( aResult, "error", "invalid_dimension_label",
                        "leader dimension requires a bounded single-line label" );
        }

        if( !start.empty() && start == end )
            diagnostic( aResult, "error", "zero_length_dimension",
                        "leader dimension endpoints must differ" );

        geometry = { { "start", std::move( start ) }, { "end", std::move( end ) },
                     { "border", border }, { "label", label } };
    }
    else if( style == "center" )
    {
        JSON center = JSON::object();
        JSON end = JSON::object();
        parsePoint( "center", center );
        parsePoint( "to", end );

        if( !center.empty() && center == end )
            diagnostic( aResult, "error", "zero_length_dimension",
                        "center dimension endpoint must differ from center" );

        geometry = { { "center", std::move( center ) }, { "end", std::move( end ) } };
    }

    std::string units = "automatic";
    std::string unitFormat = "no_suffix";
    std::string precision = "fixed_0";
    bool suppressTrailingZeroes = false;
    std::string prefix = style == "radial" ? "R " : "";
    std::string suffix;
    bool overrideEnabled = false;
    std::string overrideText;
    const bool measured = style == "aligned" || style == "orthogonal" || style == "radial";

    if( measured )
    {
        if( !fields.contains( "units" ) || !parseScalarForm( aDocument, fields["units"], units )
            || ( units != "mm" && units != "mil" && units != "in"
                 && units != "automatic" ) )
        {
            diagnostic( aResult, "error", "invalid_dimension_units",
                        "measurement dimension units must be mm, mil, in, or automatic" );
        }

        if( !fields.contains( "unit_format" )
            || !parseScalarForm( aDocument, fields["unit_format"], unitFormat )
            || ( unitFormat != "no_suffix" && unitFormat != "bare_suffix"
                 && unitFormat != "paren_suffix" ) )
        {
            diagnostic( aResult, "error", "invalid_dimension_unit_format",
                        "dimension unit_format must be no_suffix, bare_suffix, or paren_suffix" );
        }

        static const std::set<std::string> PRECISIONS = {
            "fixed_0", "fixed_1", "fixed_2", "fixed_3", "fixed_4", "fixed_5",
            "scaled_in_2", "scaled_in_3", "scaled_in_4", "scaled_in_5"
        };

        if( !fields.contains( "precision" )
            || !parseScalarForm( aDocument, fields["precision"], precision )
            || !PRECISIONS.contains( precision ) )
        {
            diagnostic( aResult, "error", "invalid_dimension_precision",
                        "dimension precision must be a canonical fixed or scaled precision" );
        }

        if( fields.contains( "suppress_trailing_zeroes" )
            && !parseBooleanForm( aDocument, fields["suppress_trailing_zeroes"],
                                  suppressTrailingZeroes ) )
        {
            diagnostic( aResult, "error", "invalid_dimension_suppress_zeroes",
                        "dimension suppress_trailing_zeroes must be true or false" );
        }

        const auto parseAffix = [&]( const char* aField, std::string& aValue )
        {
            if( fields.contains( aField )
                && ( !parseScalarForm( aDocument, fields[aField], aValue )
                     || aValue.size() > 256 || aValue.find( '\r' ) != std::string::npos
                     || aValue.find( '\n' ) != std::string::npos ) )
            {
                diagnostic( aResult, "error", "invalid_dimension_affix",
                            std::string( "dimension " ) + aField
                                    + " must be at most 256 UTF-8 bytes on one line" );
            }
        };
        parseAffix( "prefix", prefix );
        parseAffix( "suffix", suffix );

        if( fields.contains( "override" ) )
        {
            overrideEnabled = true;

            if( !parseScalarForm( aDocument, fields["override"], overrideText )
                || overrideText.size() > MAX_TEXT_BYTES
                || overrideText.find( '\r' ) != std::string::npos
                || overrideText.find( '\n' ) != std::string::npos )
            {
                diagnostic( aResult, "error", "invalid_dimension_override",
                            "dimension override must be bounded single-line text" );
            }
        }
    }

    std::string arrowDirection = "outward";
    std::string textPosition = "manual";
    bool keepTextAligned = false;

    if( style == "aligned" || style == "orthogonal" )
    {
        textPosition = "outside";
        keepTextAligned = true;

        if( !fields.contains( "arrow_direction" )
            || !parseScalarForm( aDocument, fields["arrow_direction"], arrowDirection )
            || ( arrowDirection != "inward" && arrowDirection != "outward" ) )
        {
            diagnostic( aResult, "error", "invalid_dimension_arrow_direction",
                        "dimension arrow_direction must be inward or outward" );
        }

        if( !fields.contains( "text_position" )
            || !parseScalarForm( aDocument, fields["text_position"], textPosition )
            || ( textPosition != "outside" && textPosition != "inline"
                 && textPosition != "manual" ) )
        {
            diagnostic( aResult, "error", "invalid_dimension_text_position_mode",
                        "dimension text_position must be outside, inline, or manual" );
        }
    }

    if( style == "aligned" || style == "orthogonal" || style == "radial" )
    {
        if( fields.contains( "keep_text_aligned" )
            && !parseBooleanForm( aDocument, fields["keep_text_aligned"], keepTextAligned ) )
        {
            diagnostic( aResult, "error", "invalid_dimension_keep_text_aligned",
                        "dimension keep_text_aligned must be true or false" );
        }
    }

    JSON text = style == "center" ? JSON::object() : compileDimensionText( aDocument, fields,
                                                                            aResult );
    const bool requiresTextAt = style == "radial" || style == "leader"
                                || textPosition == "manual";

    if( style != "center" && requiresTextAt && !fields.contains( "text_at" ) )
    {
        diagnostic( aResult, "error", "missing_dimension_text_position",
                    "this dimension style or text_position requires text_at" );
    }
    else if( style != "center" && !requiresTextAt && fields.contains( "text_at" ) )
    {
        diagnostic( aResult, "error", "ignored_dimension_text_position",
                    "text_at is valid only for manual, radial, and leader dimensions" );
    }

    if( style != "center" && keepTextAligned && text.value( "angleExplicit", false ) )
    {
        diagnostic( aResult, "error", "ignored_dimension_text_angle",
                    "text_angle is not accepted while keep_text_aligned is true" );
    }

    if( style == "leader" )
    {
        overrideEnabled = true;
        overrideText = geometry.value( "label", "" );
        unitFormat = "no_suffix";
    }

    return { { "kind", "dimension" },
             { "logicalId", logicalId },
             { "dimensionStyle", style },
             { "layer", layer },
             { "geometry", std::move( geometry ) },
             { "text", std::move( text ) },
             { "units", units },
             { "unitFormat", unitFormat },
             { "precision", precision },
             { "suppressTrailingZeroes", suppressTrailingZeroes },
             { "prefix", prefix },
             { "suffix", suffix },
             { "overrideEnabled", overrideEnabled },
             { "overrideText", overrideText },
             { "lineWidthNm", lineWidth },
             { "arrowLengthNm", arrowLength },
             { "extensionOffsetNm", extensionOffset },
             { "arrowDirection", arrowDirection },
             { "textPosition", textPosition },
             { "keepTextAligned", keepTextAligned },
             { "locked", locked },
             { "typed", true } };
}


JSON compileKeepoutProhibitions( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult )
{
    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 1,
                   { "copper", "vias", "tracks", "pads", "footprints" }, fields,
                   aResult, "keepout prohibit" );
    JSON prohibitions = JSON::object();
    bool any = false;

    for( const char* field : { "copper", "vias", "tracks", "pads", "footprints" } )
    {
        bool prohibited = false;

        if( !fields.contains( field )
            || !parseBooleanForm( aDocument, fields[field], prohibited ) )
        {
            diagnostic( aResult, "error", "invalid_keepout_prohibition",
                        "keepout prohibit requires explicit true or false values for copper, "
                        "vias, tracks, pads, and footprints" );
        }

        prohibitions[field] = prohibited;
        any = any || prohibited;
    }

    if( !any )
    {
        diagnostic( aResult, "error", "empty_keepout",
                    "keepout must prohibit at least one board item category" );
    }

    return prohibitions;
}


JSON compileKeepout( const DOCUMENT& aDocument, size_t aNode, RESULT& aResult,
                     std::set<std::string>& aLogicalIds )
{
    std::map<std::string, size_t> fields;
    collectFields( aDocument, aNode, 1,
                   { "id", "name", "layers", "outline", "prohibit", "border", "locked" },
                   fields, aResult, "keepout" );
    std::string logicalId;
    std::string name;
    JSON layers = JSON::array();
    JSON polygons = JSON::array();
    JSON prohibitions = JSON::object();
    JSON border = { { "style", "solid" }, { "pitchNm", 0 } };
    bool locked = false;

    if( !fields.contains( "id" ) || !parseScalarForm( aDocument, fields["id"], logicalId )
        || !validIdentifier( logicalId ) )
    {
        diagnostic( aResult, "error", "invalid_board_id",
                    "keepout requires a bounded (id LOGICAL_ID)" );
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
        diagnostic( aResult, "error", "invalid_keepout_name",
                    "keepout name must contain 1 through 256 UTF-8 bytes" );
    }

    if( !fields.contains( "layers" ) )
    {
        diagnostic( aResult, "error", "invalid_keepout_layers",
                    "keepout requires one layers declaration" );
    }
    else
    {
        const DOCUMENT::NODE& layerNode = aDocument.Nodes()[fields["layers"]];
        std::set<std::string> uniqueLayers;

        if( layerNode.children.size() < 2 || layerNode.children.size() > 33 )
        {
            diagnostic( aResult, "error", "invalid_keepout_layers",
                        "keepout layers requires 1 through 32 copper layers" );
        }

        for( size_t i = 1; i < layerNode.children.size(); ++i )
        {
            std::string layer;

            if( !scalarText( aDocument, layerNode.children[i], layer )
                || !copperLayer( layer ) || !uniqueLayers.emplace( layer ).second )
            {
                diagnostic( aResult, "error", "invalid_keepout_layers",
                            "keepout layers must be distinct KiCad copper-layer names" );
                continue;
            }

            layers.push_back( layer );
        }
    }

    if( !fields.contains( "outline" ) )
    {
        diagnostic( aResult, "error", "invalid_keepout_outline",
                    "keepout requires an explicit polygon outline" );
    }
    else
    {
        polygons = compileZoneOutline( aDocument, fields["outline"], aResult );
    }

    if( !fields.contains( "prohibit" ) )
    {
        diagnostic( aResult, "error", "invalid_keepout_prohibition",
                    "keepout requires an explicit prohibit declaration" );
    }
    else
    {
        prohibitions = compileKeepoutProhibitions( aDocument, fields["prohibit"], aResult );
    }

    if( fields.contains( "border" ) )
        border = compileZoneBorder( aDocument, fields["border"], aResult );

    if( fields.contains( "locked" )
        && !parseBooleanForm( aDocument, fields["locked"], locked ) )
    {
        diagnostic( aResult, "error", "invalid_keepout_locked",
                    "keepout locked must be true or false" );
    }

    return { { "kind", "keepout" },
             { "logicalId", logicalId },
             { "name", name },
             { "layers", std::move( layers ) },
             { "polygons", std::move( polygons ) },
             { "prohibitions", std::move( prohibitions ) },
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
        "stackup", "outline", "layout", "synthesize", "place", "route", "via",
        "zone", "text", "text_box", "table", "dimension", "keepout", "image", "barcode",
        "line", "rectangle", "arc", "circle", "polygon", "bezier"
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

        if( ( head == "stackup" || head == "outline" || head == "layout"
              || head == "synthesize" )
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
        else if( head == "layout" )
        {
            KICHAD::DESIGN_SCRIPT_LAYOUT_COMPILER::RESULT compiled =
                    KICHAD::DESIGN_SCRIPT_LAYOUT_COMPILER::Compile( aDocument, child );

            for( JSON& entry : compiled.diagnostics )
                result.diagnostics.push_back( std::move( entry ) );

            result.componentReferences.insert( result.componentReferences.end(),
                                                compiled.componentReferences.begin(),
                                                compiled.componentReferences.end() );
            result.netReferences.insert( result.netReferences.end(),
                                          compiled.netReferences.begin(),
                                          compiled.netReferences.end() );

            if( compiled.layout.is_object() )
                result.statements.push_back( std::move( compiled.layout ) );
        }
        else if( head == "synthesize" )
        {
            KICHAD::DESIGN_SCRIPT_PHYSICAL_SYNTHESIS_COMPILER::RESULT compiled =
                    KICHAD::DESIGN_SCRIPT_PHYSICAL_SYNTHESIS_COMPILER::Compile(
                            aDocument, child );

            for( JSON& entry : compiled.diagnostics )
                result.diagnostics.push_back( std::move( entry ) );

            result.synthesis = std::move( compiled.synthesis );
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
        else if( head == "text" )
        {
            result.statements.emplace_back( compileText( aDocument, child, result, logicalIds ) );
        }
        else if( head == "text_box" )
        {
            KICHAD::DESIGN_SCRIPT_BOARD_TEXT_BOX_COMPILER::RESULT compiled =
                    KICHAD::DESIGN_SCRIPT_BOARD_TEXT_BOX_COMPILER::Compile( aDocument, child );

            for( JSON& entry : compiled.diagnostics )
                result.diagnostics.push_back( std::move( entry ) );

            const std::string logicalId = compiled.statement.value( "logicalId", "" );

            if( !logicalId.empty() && !logicalIds.emplace( logicalId ).second )
                diagnostic( result, "error", "duplicate_board_id",
                            "board logical id " + logicalId + " occurs more than once" );

            if( compiled.statement.is_object() && !logicalId.empty() )
                result.statements.push_back( std::move( compiled.statement ) );
        }
        else if( head == "table" )
        {
            KICHAD::DESIGN_SCRIPT_BOARD_TABLE_COMPILER::RESULT compiled =
                    KICHAD::DESIGN_SCRIPT_BOARD_TABLE_COMPILER::Compile( aDocument, child );

            for( JSON& entry : compiled.diagnostics )
                result.diagnostics.push_back( std::move( entry ) );

            const std::string logicalId = compiled.statement.value( "logicalId", "" );

            if( !logicalId.empty() && !logicalIds.emplace( logicalId ).second )
                diagnostic( result, "error", "duplicate_board_id",
                            "board logical id " + logicalId + " occurs more than once" );

            if( compiled.statement.is_object() && !logicalId.empty() )
                result.statements.push_back( std::move( compiled.statement ) );
        }
        else if( head == "dimension" )
        {
            result.statements.emplace_back(
                    compileDimension( aDocument, child, result, logicalIds ) );
        }
        else if( head == "keepout" )
        {
            result.statements.emplace_back(
                    compileKeepout( aDocument, child, result, logicalIds ) );
        }
        else if( KICHAD::DESIGN_SCRIPT_BOARD_ASSET_COMPILER::IsAssetHead( head ) )
        {
            KICHAD::DESIGN_SCRIPT_BOARD_ASSET_COMPILER::RESULT compiled =
                    KICHAD::DESIGN_SCRIPT_BOARD_ASSET_COMPILER::Compile( aDocument, child );

            for( JSON& entry : compiled.diagnostics )
                result.diagnostics.push_back( std::move( entry ) );

            const std::string logicalId = compiled.statement.value( "logicalId", "" );

            if( !logicalId.empty() && !logicalIds.emplace( logicalId ).second )
                diagnostic( result, "error", "duplicate_board_id",
                            "board logical id " + logicalId + " occurs more than once" );

            if( compiled.ok && !logicalId.empty() )
                result.statements.push_back( std::move( compiled.statement ) );
        }
        else if( KICHAD::DESIGN_SCRIPT_BOARD_GRAPHIC_COMPILER::IsGraphicHead( head ) )
        {
            KICHAD::DESIGN_SCRIPT_BOARD_GRAPHIC_COMPILER::RESULT compiled =
                    KICHAD::DESIGN_SCRIPT_BOARD_GRAPHIC_COMPILER::Compile( aDocument, child );

            for( JSON& entry : compiled.diagnostics )
                result.diagnostics.push_back( std::move( entry ) );

            const std::string logicalId = compiled.statement.value( "logicalId", "" );

            if( !logicalId.empty() && !logicalIds.emplace( logicalId ).second )
                diagnostic( result, "error", "duplicate_board_id",
                            "board logical id " + logicalId + " occurs more than once" );

            if( compiled.statement.is_object() && !logicalId.empty() )
            {
                const std::string net =
                        compiled.statement["graphic"].value( "net", "" );

                if( !net.empty() )
                    result.netReferences.emplace_back( net );

                result.statements.push_back( std::move( compiled.statement ) );
            }
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
