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

#include "design_script_pcb_planner.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>

#include <picosha2.h>


namespace
{

using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT;


void diagnostic( RESULT& aResult, const std::string& aSeverity, const std::string& aCode,
                 const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", aSeverity },
                                     { "code", aCode },
                                     { "message", aMessage } } );
}


int hexValue( char aDigit )
{
    if( aDigit >= '0' && aDigit <= '9' )
        return aDigit - '0';

    return aDigit - 'a' + 10;
}


std::string layerEnum( const std::string& aLayer )
{
    std::string result = "BL_" + aLayer;

    for( char& character : result )
    {
        if( character == '.' )
            character = '_';
    }

    return result;
}


JSON vectorProto( const JSON& aVector )
{
    return { { "xNm", std::to_string( aVector.at( "xNm" ).get<int64_t>() ) },
             { "yNm", std::to_string( aVector.at( "yNm" ).get<int64_t>() ) } };
}


JSON polyLineProto( const JSON& aLine )
{
    JSON nodes = JSON::array();

    for( const JSON& node : aLine.at( "nodes" ) )
        nodes.push_back( { { "point", vectorProto( node.at( "point" ) ) } } );

    return { { "nodes", std::move( nodes ) }, { "closed", true } };
}


JSON polySetProto( const JSON& aPolygons )
{
    JSON polygons = JSON::array();

    for( const JSON& polygon : aPolygons )
    {
        JSON holes = JSON::array();

        for( const JSON& hole : polygon.at( "holes" ) )
            holes.emplace_back( polyLineProto( hole ) );

        polygons.push_back( { { "outline", polyLineProto( polygon.at( "outline" ) ) },
                              { "holes", std::move( holes ) } } );
    }

    return { { "polygons", std::move( polygons ) } };
}


JSON planStackup( const JSON& aStatement )
{
    const std::map<std::string, std::string> typeEnums = {
        { "copper", "BSLT_COPPER" }, { "dielectric", "BSLT_DIELECTRIC" },
        { "silkscreen", "BSLT_SILKSCREEN" }, { "soldermask", "BSLT_SOLDERMASK" },
        { "solderpaste", "BSLT_SOLDERPASTE" }
    };
    JSON edge = { { "plating",
                    { { "hasEdgePlating", aStatement.at( "edgePlating" ) } } } };
    const std::string edgeConnector = aStatement.at( "edgeConnector" ).get<std::string>();

    if( edgeConnector != "none" )
        edge["connector"] = { { "bevelled", edgeConnector == "bevelled" } };

    JSON layers = JSON::array();

    for( const JSON& source : aStatement.at( "layers" ) )
    {
        const std::string category = source.at( "category" ).get<std::string>();
        JSON layer = {
            { "layer", category == "dielectric"
                               ? "BL_UNDEFINED"
                               : layerEnum( source.at( "layer" ).get<std::string>() ) },
            { "enabled", true },
            { "type", typeEnums.at( category ) },
            { "typeName", source.at( "typeName" ) }
        };
        const int64_t thickness = source.at( "thicknessNm" ).get<int64_t>();

        if( thickness > 0 )
            layer["thickness"] = { { "valueNm", std::to_string( thickness ) } };

        if( category == "dielectric" )
        {
            layer["dielectricLayerId"] = source.at( "dielectricIndex" );
            layer["dielectric"] = {
                { "layer",
                  JSON::array( { { { "epsilonR", source.at( "epsilonR" ) },
                                   { "lossTangent", source.at( "lossTangent" ) },
                                   { "materialName", source.at( "material" ) },
                                   { "thickness",
                                     { { "valueNm", std::to_string( thickness ) } } },
                                   { "thicknessLocked", source.at( "locked" ) },
                                   { "colorName", source.at( "color" ) } } } ) } };
        }
        else if( category == "soldermask" )
        {
            layer["materialName"] = source.at( "material" );
            layer["colorName"] = source.at( "color" );
            layer["epsilonR"] = source.at( "epsilonR" );
            layer["lossTangent"] = source.at( "lossTangent" );
        }
        else if( category == "silkscreen" )
        {
            layer["materialName"] = source.at( "material" );
            layer["colorName"] = source.at( "color" );
        }

        layers.emplace_back( std::move( layer ) );
    }

    return { { "action", "update_stackup" },
             { "stackup",
               { { "finish", { { "typeName", aStatement.at( "finish" ) } } },
                 { "impedance",
                   { { "isControlled", aStatement.at( "impedanceControlled" ) } } },
                 { "edge", std::move( edge ) },
                 { "layers", std::move( layers ) } } } };
}


JSON planRules( const JSON& aRules )
{
    const auto distance = [&]( const char* aName )
    {
        return JSON( { { "valueNm", std::to_string( aRules.at( aName ).get<int64_t>() ) } } );
    };
    JSON rules = {
        { "minimumClearance", distance( "minimumClearanceNm" ) },
        { "minimumConnectionWidth", distance( "minimumConnectionWidthNm" ) },
        { "minimumTrackWidth", distance( "minimumTrackWidthNm" ) },
        { "minimumViaAnnularWidth", distance( "minimumViaAnnularWidthNm" ) },
        { "minimumViaDiameter", distance( "minimumViaDiameterNm" ) },
        { "minimumThroughHoleDiameter", distance( "minimumThroughHoleDiameterNm" ) },
        { "minimumMicroviaDiameter", distance( "minimumMicroviaDiameterNm" ) },
        { "minimumMicroviaDrill", distance( "minimumMicroviaDrillNm" ) },
        { "minimumHoleToHole", distance( "minimumHoleToHoleNm" ) },
        { "minimumCopperToHoleClearance", distance( "minimumCopperToHoleClearanceNm" ) },
        { "minimumSilkscreenClearance", distance( "minimumSilkscreenClearanceNm" ) },
        { "minimumGrooveWidth", distance( "minimumGrooveWidthNm" ) },
        { "minimumResolvedSpokes", aRules.at( "minimumResolvedSpokes" ) },
        { "minimumSilkscreenTextHeight", distance( "minimumSilkscreenTextHeightNm" ) },
        { "minimumSilkscreenTextThickness",
          distance( "minimumSilkscreenTextThicknessNm" ) },
        { "copperEdgeClearanceMode",
          aRules.at( "copperEdgeClearanceMode" ) == "legacy" ? "BCECM_LEGACY"
                                                               : "BCECM_EXPLICIT" },
        { "minimumCopperToEdgeClearance",
          distance( "minimumCopperToEdgeClearanceNm" ) },
        { "useHeightForLengthCalculations",
          aRules.at( "useHeightForLengthCalculations" ) },
        { "maximumError", distance( "maximumErrorNm" ) },
        { "allowFilletsOutsideZoneOutline",
          aRules.at( "allowFilletsOutsideZoneOutline" ) }
    };
    return { { "action", "update_rules" }, { "rules", std::move( rules ) } };
}


JSON planNetClasses( const JSON& aNetClasses )
{
    const auto distance = []( int64_t aValue )
    {
        return JSON( { { "valueNm", std::to_string( aValue ) } } );
    };
    const auto padstack = [&]( const JSON& aDiameter, const JSON& aDrill )
    {
        JSON stack = JSON::object();

        if( !aDiameter.is_null() )
        {
            const std::string value = std::to_string( aDiameter.get<int64_t>() );
            stack["copperLayers"] = JSON::array(
                    { { { "layer", "BL_F_Cu" },
                        { "shape", "PSS_CIRCLE" },
                        { "size", { { "xNm", value }, { "yNm", value } } } } } );
        }

        if( !aDrill.is_null() )
        {
            const std::string value = std::to_string( aDrill.get<int64_t>() );
            stack["drill"] = { { "diameter", { { "xNm", value }, { "yNm", value } } },
                               { "shape", "DS_CIRCLE" } };
        }

        return stack;
    };
    JSON settings = { { "netClasses", JSON::array() }, { "assignments", JSON::array() } };

    for( const JSON& source : aNetClasses.at( "classes" ) )
    {
        JSON board = JSON::object();
        JSON schematic = JSON::object();
        const auto addDistance = [&]( JSON& aTarget, const char* aTargetName,
                                      const char* aSourceName )
        {
            if( !source.at( aSourceName ).is_null() )
                aTarget[aTargetName] = distance( source.at( aSourceName ).get<int64_t>() );
        };

        addDistance( board, "clearance", "clearanceNm" );
        addDistance( board, "trackWidth", "trackWidthNm" );
        addDistance( board, "diffPairTrackWidth", "diffPairWidthNm" );
        addDistance( board, "diffPairGap", "diffPairGapNm" );
        addDistance( board, "diffPairViaGap", "diffPairViaGapNm" );

        if( !source.at( "viaDiameterNm" ).is_null() || !source.at( "viaDrillNm" ).is_null() )
            board["viaStack"] = padstack( source.at( "viaDiameterNm" ),
                                          source.at( "viaDrillNm" ) );

        if( !source.at( "microviaDiameterNm" ).is_null()
            || !source.at( "microviaDrillNm" ).is_null() )
        {
            board["microviaStack"] = padstack( source.at( "microviaDiameterNm" ),
                                               source.at( "microviaDrillNm" ) );
        }

        if( !source.at( "pcbColor" ).is_null() )
            board["color"] = source.at( "pcbColor" );

        if( !source.at( "tuningProfile" ).is_null() )
            board["tuningProfile"] = source.at( "tuningProfile" );

        addDistance( schematic, "wireWidth", "wireWidthNm" );
        addDistance( schematic, "busWidth", "busWidthNm" );

        if( !source.at( "schematicColor" ).is_null() )
            schematic["color"] = source.at( "schematicColor" );

        if( !source.at( "lineStyle" ).is_null() )
        {
            static const std::map<std::string, std::string> LINE_STYLES = {
                { "solid", "SLS_SOLID" },
                { "dash", "SLS_DASH" },
                { "dot", "SLS_DOT" },
                { "dash_dot", "SLS_DASHDOT" },
                { "dash_dot_dot", "SLS_DASHDOTDOT" }
            };
            schematic["lineStyle"] =
                    LINE_STYLES.at( source.at( "lineStyle" ).get<std::string>() );
        }

        settings["netClasses"].push_back(
                { { "name", source.at( "name" ) },
                  { "priority", source.at( "priority" ) },
                  { "board", std::move( board ) },
                  { "schematic", std::move( schematic ) },
                  { "type", "NCT_EXPLICIT" } } );
    }

    for( const JSON& source : aNetClasses.at( "assignments" ) )
    {
        for( const JSON& netClass : source.at( "classes" ) )
        {
            settings["assignments"].push_back(
                    { { "pattern", source.at( "pattern" ) }, { "netClass", netClass } } );
        }
    }

    return { { "action", "update_net_classes" }, { "settings", std::move( settings ) } };
}


std::string customRuleLiteral( const JSON& aValue )
{
    const std::string domain = aValue.at( "domain" ).get<std::string>();

    if( domain == "distance" )
    {
        const int64_t nanometers = aValue.at( "value" ).get<int64_t>();
        const bool negative = nanometers < 0;
        const uint64_t magnitude = negative
                                           ? static_cast<uint64_t>( -( nanometers + 1 ) ) + 1
                                           : static_cast<uint64_t>( nanometers );
        const uint64_t whole = magnitude / 1'000'000;
        const uint64_t remainder = magnitude % 1'000'000;
        std::string result = negative ? "-" : "";
        result += std::to_string( whole );

        if( remainder != 0 )
        {
            std::ostringstream fraction;
            fraction << std::setw( 6 ) << std::setfill( '0' ) << remainder;
            std::string digits = fraction.str();

            while( digits.back() == '0' )
                digits.pop_back();

            result += "." + digits;
        }

        return result + "mm";
    }

    if( domain == "time" )
        return std::to_string( aValue.at( "value" ).get<int64_t>() ) + "fs";

    if( domain == "angle" )
    {
        std::string literal = aValue.at( "value" ).dump();

        if( literal.size() > 2 && literal.ends_with( ".0" ) )
            literal.resize( literal.size() - 2 );

        // KiCad's DRC parser classifies track_angle as unitless even though the value is
        // semantically expressed in degrees.  Keep the KDS surface explicitly typed and lower
        // to the bare numeric form accepted by the native parser.
        return literal;
    }

    return std::to_string( aValue.at( "value" ).get<int64_t>() );
}


std::string planCustomConstraint( const JSON& aConstraint )
{
    const std::string type = aConstraint.at( "type" ).get<std::string>();
    const std::string kind = aConstraint.at( "kind" ).get<std::string>();
    std::ostringstream output;
    output << "    (constraint " << type;

    if( kind == "assertion" )
    {
        output << ' ' << JSON( aConstraint.at( "test" ).get<std::string>() ).dump();
    }
    else if( kind == "zone_connection" )
    {
        output << ' ' << aConstraint.at( "style" ).get<std::string>();
    }
    else if( kind == "disallow" )
    {
        for( const JSON& item : aConstraint.at( "items" ) )
            output << ' ' << item.get<std::string>();
    }
    else if( kind == "count" )
    {
        output << ' ' << aConstraint.at( "value" ).get<int64_t>();
    }
    else if( kind == "ratio" )
    {
        // KiCad stores relative paste margin in per-mille and divides the parsed integer by 1000.
        output << " (opt " << aConstraint.at( "valuePermille" ).get<int64_t>() << ')';
    }
    else if( kind == "range" )
    {
        const JSON& values = aConstraint.at( "values" );

        for( const char* field : { "min", "opt", "max" } )
        {
            if( values.contains( field ) )
                output << " (" << field << ' ' << customRuleLiteral( values.at( field ) ) << ')';
        }

        if( type == "skew" && aConstraint.at( "domain" ) == "diff_pairs" )
            output << " (within_diff_pairs)";
    }

    output << ")\n";
    return output.str();
}


JSON planCustomRules( const JSON& aCustomRules )
{
    std::ostringstream output;
    output << "(version 1)\n";

    for( const JSON& rule : aCustomRules.at( "rules" ) )
    {
        output << "\n(rule " << JSON( rule.at( "name" ).get<std::string>() ).dump() << "\n";

        if( rule.at( "condition" ) != "always" )
        {
            output << "  (condition "
                   << JSON( rule.at( "condition" ).get<std::string>() ).dump() << ")\n";
        }

        if( rule.at( "layer" ) != "all" )
            output << "  (layer " << rule.at( "layer" ).get<std::string>() << ")\n";

        output << "  (severity " << rule.at( "severity" ).get<std::string>() << ")\n";

        for( const JSON& constraint : rule.at( "constraints" ) )
            output << planCustomConstraint( constraint );

        output << ")\n";
    }

    return { { "action", "update_custom_rules" },
             { "customRules", { { "present", true }, { "source", output.str() } } } };
}


std::string lockedEnum( const JSON& aStatement )
{
    return aStatement.value( "locked", false ) ? "LS_LOCKED" : "LS_UNLOCKED";
}


JSON planOutline( const JSON& aStatement, const std::string& aProject )
{
    const std::string logicalId = aStatement.at( "logicalId" ).get<std::string>();
    const std::string itemId =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid( aProject, "shape", logicalId );
    JSON item = {
        { "id", { { "value", itemId } } },
        { "shape",
          { { "attributes",
              { { "stroke",
                  { { "width",
                      { { "valueNm",
                          std::to_string( aStatement.at( "lineWidthNm" ).get<int64_t>() ) } } },
                    { "style", "SLS_SOLID" } } },
                { "fill", { { "fillType", "GFT_UNFILLED" } } } } },
            { "rectangle",
              { { "topLeft", vectorProto( aStatement.at( "topLeft" ) ) },
                { "bottomRight", vectorProto( aStatement.at( "bottomRight" ) ) } } } } },
        { "layer", layerEnum( aStatement.at( "layer" ).get<std::string>() ) },
        { "locked", lockedEnum( aStatement ) }
    };

    return { { "action", "upsert" },
             { "itemType", "shape" },
             { "logicalId", logicalId },
             { "itemId", itemId },
             { "item", std::move( item ) } };
}


JSON planText( const JSON& aStatement, const std::string& aProject )
{
    const std::string logicalId = aStatement.at( "logicalId" ).get<std::string>();
    const std::string itemId =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid( aProject, "text", logicalId );
    const std::map<std::string, std::string> horizontalEnums = {
        { "left", "HA_LEFT" }, { "center", "HA_CENTER" }, { "right", "HA_RIGHT" }
    };
    const std::map<std::string, std::string> verticalEnums = {
        { "top", "VA_TOP" }, { "center", "VA_CENTER" }, { "bottom", "VA_BOTTOM" }
    };
    JSON item = {
        { "id", { { "value", itemId } } },
        { "text",
          { { "position", vectorProto( aStatement.at( "position" ) ) },
            { "attributes",
              { { "fontName", aStatement.at( "fontName" ) },
                { "horizontalAlignment",
                  horizontalEnums.at(
                          aStatement.at( "horizontalJustification" ).get<std::string>() ) },
                { "verticalAlignment",
                  verticalEnums.at(
                          aStatement.at( "verticalJustification" ).get<std::string>() ) },
                { "angle", { { "valueDegrees", aStatement.at( "angleDegrees" ) } } },
                { "lineSpacing", aStatement.at( "lineSpacing" ) },
                { "strokeWidth",
                  { { "valueNm",
                      std::to_string( aStatement.at( "strokeNm" ).get<int64_t>() ) } } },
                { "italic", aStatement.at( "italic" ) },
                { "bold", aStatement.at( "bold" ) },
                { "underlined", aStatement.at( "underlined" ) },
                { "visible", true },
                { "mirrored", aStatement.at( "mirrored" ) },
                { "multiline", aStatement.at( "multiline" ) },
                { "keepUpright", aStatement.at( "keepUpright" ) },
                { "size", vectorProto( aStatement.at( "size" ) ) } } },
            { "text", aStatement.at( "value" ) },
            { "hyperlink", aStatement.at( "hyperlink" ) } } },
        { "layer", layerEnum( aStatement.at( "layer" ).get<std::string>() ) },
        { "knockout", aStatement.at( "knockout" ) },
        { "locked", lockedEnum( aStatement ) }
    };

    return { { "action", "upsert" },
             { "itemType", "text" },
             { "logicalId", logicalId },
             { "itemId", itemId },
             { "item", std::move( item ) } };
}


JSON planDimension( const JSON& aStatement, const std::string& aProject )
{
    const std::string logicalId = aStatement.at( "logicalId" ).get<std::string>();
    const std::string itemId =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid( aProject, "dimension", logicalId );
    const std::string style = aStatement.at( "dimensionStyle" ).get<std::string>();
    const JSON& geometry = aStatement.at( "geometry" );
    const std::map<std::string, std::string> horizontalEnums = {
        { "left", "HA_LEFT" }, { "center", "HA_CENTER" }, { "right", "HA_RIGHT" }
    };
    const std::map<std::string, std::string> verticalEnums = {
        { "top", "VA_TOP" }, { "center", "VA_CENTER" }, { "bottom", "VA_BOTTOM" }
    };
    const std::map<std::string, std::string> unitEnums = {
        { "in", "DU_INCHES" }, { "mil", "DU_MILS" }, { "mm", "DU_MILLIMETERS" },
        { "automatic", "DU_AUTOMATIC" }
    };
    const std::map<std::string, std::string> unitFormatEnums = {
        { "no_suffix", "DUF_NO_SUFFIX" }, { "bare_suffix", "DUF_BARE_SUFFIX" },
        { "paren_suffix", "DUF_PAREN_SUFFIX" }
    };
    const std::map<std::string, std::string> precisionEnums = {
        { "fixed_0", "DP_FIXED_0" }, { "fixed_1", "DP_FIXED_1" },
        { "fixed_2", "DP_FIXED_2" }, { "fixed_3", "DP_FIXED_3" },
        { "fixed_4", "DP_FIXED_4" }, { "fixed_5", "DP_FIXED_5" },
        { "scaled_in_2", "DP_SCALED_IN_2" }, { "scaled_in_3", "DP_SCALED_IN_3" },
        { "scaled_in_4", "DP_SCALED_IN_4" }, { "scaled_in_5", "DP_SCALED_IN_5" }
    };
    const std::map<std::string, std::string> arrowEnums = {
        { "inward", "DAD_INWARD" }, { "outward", "DAD_OUTWARD" }
    };
    const std::map<std::string, std::string> textPositionEnums = {
        { "outside", "DTP_OUTSIDE" }, { "inline", "DTP_INLINE" },
        { "manual", "DTP_MANUAL" }
    };
    JSON item = {
        { "id", { { "value", itemId } } },
        { "locked", lockedEnum( aStatement ) },
        { "layer", layerEnum( aStatement.at( "layer" ).get<std::string>() ) },
        { "overrideTextEnabled", aStatement.at( "overrideEnabled" ) },
        { "overrideText", aStatement.at( "overrideText" ) },
        { "prefix", aStatement.at( "prefix" ) },
        { "suffix", aStatement.at( "suffix" ) },
        { "unit", unitEnums.at( aStatement.at( "units" ).get<std::string>() ) },
        { "unitFormat",
          unitFormatEnums.at( aStatement.at( "unitFormat" ).get<std::string>() ) },
        { "arrowDirection",
          arrowEnums.at( aStatement.at( "arrowDirection" ).get<std::string>() ) },
        { "precision",
          precisionEnums.at( aStatement.at( "precision" ).get<std::string>() ) },
        { "suppressTrailingZeroes", aStatement.at( "suppressTrailingZeroes" ) },
        { "lineThickness",
          { { "valueNm", std::to_string( aStatement.at( "lineWidthNm" ).get<int64_t>() ) } } },
        { "arrowLength",
          { { "valueNm", std::to_string( aStatement.at( "arrowLengthNm" ).get<int64_t>() ) } } },
        { "extensionOffset",
          { { "valueNm",
              std::to_string( aStatement.at( "extensionOffsetNm" ).get<int64_t>() ) } } },
        { "textPosition",
          textPositionEnums.at( aStatement.at( "textPosition" ).get<std::string>() ) },
        { "keepTextAligned", aStatement.at( "keepTextAligned" ) }
    };

    if( style != "center" )
    {
        const JSON& text = aStatement.at( "text" );
        item["text"] = {
            { "position", vectorProto( text.at( "position" ) ) },
            { "attributes",
              { { "fontName", text.at( "fontName" ) },
                { "horizontalAlignment",
                  horizontalEnums.at(
                          text.at( "horizontalJustification" ).get<std::string>() ) },
                { "verticalAlignment",
                  verticalEnums.at( text.at( "verticalJustification" ).get<std::string>() ) },
                { "angle", { { "valueDegrees", text.at( "angleDegrees" ) } } },
                { "lineSpacing", 1.0 },
                { "strokeWidth",
                  { { "valueNm", std::to_string( text.at( "strokeNm" ).get<int64_t>() ) } } },
                { "italic", text.at( "italic" ) },
                { "bold", text.at( "bold" ) },
                { "underlined", text.at( "underlined" ) },
                { "visible", true },
                { "mirrored", text.at( "mirrored" ) },
                { "multiline", false },
                { "keepUpright", false },
                { "size", vectorProto( text.at( "size" ) ) } } },
            { "text", aStatement.at( "overrideText" ) },
            { "hyperlink", "" }
        };
    }

    if( style == "aligned" )
    {
        item["aligned"] = {
            { "start", vectorProto( geometry.at( "start" ) ) },
            { "end", vectorProto( geometry.at( "end" ) ) },
            { "height",
              { { "valueNm",
                  std::to_string( geometry.at( "heightNm" ).get<int64_t>() ) } } },
            { "extensionHeight",
              { { "valueNm",
                  std::to_string( geometry.at( "extensionHeightNm" ).get<int64_t>() ) } } }
        };
    }
    else if( style == "orthogonal" )
    {
        item["orthogonal"] = {
            { "start", vectorProto( geometry.at( "start" ) ) },
            { "end", vectorProto( geometry.at( "end" ) ) },
            { "height",
              { { "valueNm",
                  std::to_string( geometry.at( "heightNm" ).get<int64_t>() ) } } },
            { "extensionHeight",
              { { "valueNm",
                  std::to_string( geometry.at( "extensionHeightNm" ).get<int64_t>() ) } } },
            { "alignment", geometry.at( "axis" ) == "x" ? "AA_X_AXIS" : "AA_Y_AXIS" }
        };
    }
    else if( style == "radial" )
    {
        item["radial"] = {
            { "center", vectorProto( geometry.at( "center" ) ) },
            { "radiusPoint", vectorProto( geometry.at( "radiusPoint" ) ) },
            { "leaderLength",
              { { "valueNm",
                  std::to_string( geometry.at( "leaderLengthNm" ).get<int64_t>() ) } } }
        };
    }
    else if( style == "leader" )
    {
        const std::map<std::string, std::string> borderEnums = {
            { "none", "DTBS_NONE" }, { "rectangle", "DTBS_RECTANGLE" },
            { "circle", "DTBS_CIRCLE" }, { "roundrect", "DTBS_ROUNDRECT" }
        };
        item["leader"] = {
            { "start", vectorProto( geometry.at( "start" ) ) },
            { "end", vectorProto( geometry.at( "end" ) ) },
            { "borderStyle", borderEnums.at( geometry.at( "border" ).get<std::string>() ) }
        };
    }
    else
    {
        item["center"] = { { "center", vectorProto( geometry.at( "center" ) ) },
                           { "end", vectorProto( geometry.at( "end" ) ) } };
    }

    return { { "action", "upsert" },
             { "itemType", "dimension" },
             { "logicalId", logicalId },
             { "itemId", itemId },
             { "item", std::move( item ) } };
}


JSON planRoute( const JSON& aStatement, const std::string& aProject )
{
    const std::string kind = aStatement.at( "kind" ).get<std::string>();
    const std::string logicalId = aStatement.at( "logicalId" ).get<std::string>();
    const std::string itemId =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid( aProject, kind, logicalId );
    JSON item = {
        { "id", { { "value", itemId } } },
        { "start", vectorProto( aStatement.at( "start" ) ) },
        { "end", vectorProto( aStatement.at( "end" ) ) },
        { "width",
          { { "valueNm", std::to_string( aStatement.at( "widthNm" ).get<int64_t>() ) } } },
        { "layer", layerEnum( aStatement.at( "layer" ).get<std::string>() ) },
        { "net", { { "name", aStatement.at( "net" ) } } },
        { "locked", lockedEnum( aStatement ) }
    };

    if( kind == "arc" )
        item["mid"] = vectorProto( aStatement.at( "mid" ) );

    return { { "action", "upsert" },
             { "itemType", kind },
             { "logicalId", logicalId },
             { "itemId", itemId },
             { "item", std::move( item ) } };
}


JSON planVia( const JSON& aStatement, const std::string& aProject )
{
    const std::string logicalId = aStatement.at( "logicalId" ).get<std::string>();
    const std::string itemId =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid( aProject, "via", logicalId );
    const std::string startLayer = layerEnum( aStatement.at( "startLayer" ).get<std::string>() );
    const std::string endLayer = layerEnum( aStatement.at( "endLayer" ).get<std::string>() );
    const std::string diameter = std::to_string( aStatement.at( "diameterNm" ).get<int64_t>() );
    const std::string drill = std::to_string( aStatement.at( "drillNm" ).get<int64_t>() );
    const std::string type = aStatement.at( "viaType" ).get<std::string>();
    std::string       typeEnum;

    if( type == "through" )
        typeEnum = "VT_THROUGH";
    else if( type == "blind" )
        typeEnum = "VT_BLIND";
    else if( type == "buried" )
        typeEnum = "VT_BURIED";
    else
        typeEnum = "VT_MICRO";

    JSON item = {
        { "id", { { "value", itemId } } },
        { "position", vectorProto( aStatement.at( "position" ) ) },
        { "padStack",
          { { "type", "PST_NORMAL" },
            { "layers", JSON::array( { startLayer, endLayer } ) },
            { "drill",
              { { "startLayer", startLayer },
                { "endLayer", endLayer },
                { "diameter", { { "xNm", drill }, { "yNm", drill } } },
                { "shape", "DS_CIRCLE" } } },
            { "unconnectedLayerRemoval", "ULR_KEEP" },
            { "copperLayers",
              JSON::array( { { { "layer", "BL_F_Cu" },
                                { "shape", "PSS_CIRCLE" },
                                { "size", { { "xNm", diameter }, { "yNm", diameter } } } } } ) } } },
        { "locked", lockedEnum( aStatement ) },
        { "net", { { "name", aStatement.at( "net" ) } } },
        { "type", typeEnum }
    };

    return { { "action", "upsert" },
             { "itemType", "via" },
             { "logicalId", logicalId },
             { "itemId", itemId },
             { "item", std::move( item ) } };
}


JSON planZone( const JSON& aStatement, const std::string& aProject )
{
    const std::string logicalId = aStatement.at( "logicalId" ).get<std::string>();
    const std::string itemId =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid( aProject, "zone", logicalId );
    const JSON& connection = aStatement.at( "connection" );
    const JSON& islands = aStatement.at( "islands" );
    const JSON& fill = aStatement.at( "fill" );
    const JSON& border = aStatement.at( "border" );
    const std::string connectionStyle = connection.at( "style" ).get<std::string>();
    const std::string islandMode = islands.at( "mode" ).get<std::string>();
    const std::string fillMode = fill.at( "mode" ).get<std::string>();
    const std::string borderStyle = border.at( "style" ).get<std::string>();
    JSON layers = JSON::array();

    for( const JSON& layer : aStatement.at( "layers" ) )
        layers.emplace_back( layerEnum( layer.get<std::string>() ) );

    const std::map<std::string, std::string> connectionEnums = {
        { "none", "ZCS_NONE" }, { "solid", "ZCS_FULL" },
        { "thermal", "ZCS_THERMAL" }, { "pth_thermal", "ZCS_PTH_THERMAL" }
    };
    const std::map<std::string, std::string> islandEnums = {
        { "remove_all", "IRM_ALWAYS" }, { "keep_all", "IRM_NEVER" },
        { "remove_below", "IRM_AREA" }
    };
    const std::map<std::string, std::string> borderEnums = {
        { "solid", "ZBS_SOLID" }, { "diagonal_full", "ZBS_DIAGONAL_FULL" },
        { "diagonal_edge", "ZBS_DIAGONAL_EDGE" }, { "invisible", "ZBS_INVISIBLE" }
    };
    JSON layerProperties = JSON::array();

    for( const JSON& property : aStatement.at( "layerProperties" ) )
    {
        layerProperties.push_back(
                { { "layer", layerEnum( property.at( "layer" ).get<std::string>() ) },
                  { "hatchingOffset", vectorProto( property.at( "offset" ) ) } } );
    }

    JSON item = {
        { "id", { { "value", itemId } } },
        { "type", "ZT_COPPER" },
        { "layers", std::move( layers ) },
        { "outline", polySetProto( aStatement.at( "polygons" ) ) },
        { "name", aStatement.at( "name" ) },
        { "copperSettings",
          { { "connection",
              { { "zoneConnection", connectionEnums.at( connectionStyle ) },
                { "thermalSpokes",
                  { { "width",
                      { { "valueNm",
                          std::to_string(
                                  connection.at( "thermalSpokeWidthNm" ).get<int64_t>() ) } } },
                    { "gap",
                      { { "valueNm",
                          std::to_string( connection.at( "thermalGapNm" ).get<int64_t>() ) } } } } } } },
            { "clearance",
              { { "valueNm", std::to_string( aStatement.at( "clearanceNm" ).get<int64_t>() ) } } },
            { "minThickness",
              { { "valueNm",
                  std::to_string( aStatement.at( "minThicknessNm" ).get<int64_t>() ) } } },
            { "islandMode", islandEnums.at( islandMode ) },
            { "minIslandArea",
              std::to_string( islands.at( "minimumAreaNm2" ).get<int64_t>() ) },
            { "fillMode", fillMode == "solid" ? "ZFM_SOLID" : "ZFM_HATCHED" },
            { "hatchSettings",
              { { "thickness",
                  { { "valueNm",
                      std::to_string( fill.at( "thicknessNm" ).get<int64_t>() ) } } },
                { "gap",
                  { { "valueNm", std::to_string( fill.at( "gapNm" ).get<int64_t>() ) } } },
                { "orientation",
                  { { "valueDegrees", fill.at( "orientationDegrees" ) } } },
                { "hatchSmoothingRatio", fill.at( "smoothingRatio" ) },
                { "hatchHoleMinAreaRatio", fill.at( "holeMinimumAreaRatio" ) },
                { "borderMode", fill.at( "borderMode" ).get<std::string>() == "minimum"
                                          ? "ZHFBM_USE_MIN_ZONE_THICKNESS"
                                          : "ZHFBM_USE_HATCH_THICKNESS" } } },
            { "net", { { "name", aStatement.at( "net" ) } } },
            { "teardrop", { { "type", "TDT_NONE" } } } } },
        { "priority", aStatement.at( "priority" ) },
        { "filled", false },
        { "filledPolygons", JSON::array() },
        { "border",
          { { "style", borderEnums.at( borderStyle ) },
            { "pitch",
              { { "valueNm", std::to_string( border.at( "pitchNm" ).get<int64_t>() ) } } } } },
        { "locked", lockedEnum( aStatement ) },
        { "layerProperties", std::move( layerProperties ) }
    };

    return { { "action", "upsert" },
             { "itemType", "zone" },
             { "logicalId", logicalId },
             { "itemId", itemId },
             { "item", std::move( item ) } };
}


JSON planKeepout( const JSON& aStatement, const std::string& aProject )
{
    const std::string logicalId = aStatement.at( "logicalId" ).get<std::string>();
    const std::string itemId =
            KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid( aProject, "rule_area", logicalId );
    const JSON& prohibitions = aStatement.at( "prohibitions" );
    const JSON& border = aStatement.at( "border" );
    const std::string borderStyle = border.at( "style" ).get<std::string>();
    const std::map<std::string, std::string> borderEnums = {
        { "solid", "ZBS_SOLID" }, { "diagonal_full", "ZBS_DIAGONAL_FULL" },
        { "diagonal_edge", "ZBS_DIAGONAL_EDGE" }, { "invisible", "ZBS_INVISIBLE" }
    };
    JSON layers = JSON::array();

    for( const JSON& layer : aStatement.at( "layers" ) )
        layers.emplace_back( layerEnum( layer.get<std::string>() ) );

    JSON item = {
        { "id", { { "value", itemId } } },
        { "type", "ZT_RULE_AREA" },
        { "layers", std::move( layers ) },
        { "outline", polySetProto( aStatement.at( "polygons" ) ) },
        { "name", aStatement.at( "name" ) },
        { "ruleAreaSettings",
          { { "keepoutCopper", prohibitions.at( "copper" ) },
            { "keepoutVias", prohibitions.at( "vias" ) },
            { "keepoutTracks", prohibitions.at( "tracks" ) },
            { "keepoutPads", prohibitions.at( "pads" ) },
            { "keepoutFootprints", prohibitions.at( "footprints" ) },
            { "placementEnabled", false },
            { "placementSourceType", "PRST_UNKNOWN" },
            { "placementSource", "" } } },
        { "priority", 0 },
        { "filled", false },
        { "filledPolygons", JSON::array() },
        { "border",
          { { "style", borderEnums.at( borderStyle ) },
            { "pitch",
              { { "valueNm", std::to_string( border.at( "pitchNm" ).get<int64_t>() ) } } } } },
        { "locked", lockedEnum( aStatement ) },
        { "layerProperties", JSON::array() }
    };

    return { { "action", "upsert" },
             { "itemType", "rule_area" },
             { "logicalId", logicalId },
             { "itemId", itemId },
             { "item", std::move( item ) } };
}

} // namespace


namespace KICHAD
{

std::string DESIGN_SCRIPT_PCB_PLANNER::StableUuid( const std::string& aProject,
                                                    const std::string& aKind,
                                                    const std::string& aLogicalId )
{
    std::string identity = "kichad-design-v1";
    identity.push_back( '\0' );
    identity += aProject;
    identity.push_back( '\0' );
    identity += aKind;
    identity.push_back( '\0' );
    identity += aLogicalId;
    std::string digest;
    picosha2::hash256_hex_string( identity, digest );
    std::array<unsigned int, 16> bytes{};

    for( size_t i = 0; i < bytes.size(); ++i )
        bytes[i] = static_cast<unsigned int>( ( hexValue( digest[i * 2] ) << 4 )
                                               | hexValue( digest[i * 2 + 1] ) );

    // RFC 9562 UUIDv8 layout for the application-defined SHA-256 name digest.
    bytes[6] = ( bytes[6] & 0x0fU ) | 0x80U;
    bytes[8] = ( bytes[8] & 0x3fU ) | 0x80U;

    std::ostringstream formatted;
    formatted << std::hex << std::setfill( '0' );

    for( size_t i = 0; i < bytes.size(); ++i )
    {
        if( i == 4 || i == 6 || i == 8 || i == 10 )
            formatted << '-';

        formatted << std::setw( 2 ) << bytes[i];
    }

    return formatted.str();
}

DESIGN_SCRIPT_PCB_PLANNER::RESULT DESIGN_SCRIPT_PCB_PLANNER::Plan( const JSON& aCompilerIr )
{
    RESULT result;
    result.counts = { { "upserts", 0 }, { "placements", 0 }, { "rules", 0 },
                      { "netClasses", 0 }, { "netClassAssignments", 0 },
                      { "customRules", 0 }, { "stackups", 0 },
                      { "unsupported", 0 } };

    if( !aCompilerIr.is_object() || aCompilerIr.value( "language", "" ) != "kichad-design"
        || aCompilerIr.value( "version", 0 ) != 1 || !aCompilerIr.contains( "project" )
        || !aCompilerIr["project"].is_object() || !aCompilerIr["project"].contains( "name" )
        || !aCompilerIr["project"]["name"].is_string() || !aCompilerIr.contains( "pcb" )
        || !aCompilerIr["pcb"].is_array() )
    {
        diagnostic( result, "error", "invalid_compiler_ir",
                    "PCB planning requires valid KiChad Design Script version 1 IR" );
        return result;
    }

    const std::string project = aCompilerIr["project"]["name"].get<std::string>();

    try
    {
        if( aCompilerIr.contains( "netClasses" ) && aCompilerIr["netClasses"].is_object() )
        {
            result.operations.emplace_back( planNetClasses( aCompilerIr["netClasses"] ) );
            result.counts["netClasses"] = aCompilerIr["netClasses"]["classes"].size();
            result.counts["netClassAssignments"] =
                    aCompilerIr["netClasses"]["assignments"].size();
        }

        if( aCompilerIr.contains( "rules" ) && aCompilerIr["rules"].is_object() )
        {
            result.operations.emplace_back( planRules( aCompilerIr["rules"] ) );
            ++result.counts["rules"].get_ref<int64_t&>();
        }

        if( aCompilerIr.contains( "customRules" )
            && aCompilerIr["customRules"].is_object() )
        {
            result.operations.emplace_back( planCustomRules( aCompilerIr["customRules"] ) );
            result.counts["customRules"] = aCompilerIr["customRules"]["rules"].size();
        }

        for( const JSON& statement : aCompilerIr["pcb"] )
        {
            if( !statement.is_object() || !statement.contains( "kind" )
                || !statement["kind"].is_string() )
            {
                diagnostic( result, "error", "invalid_board_ir",
                            "board IR contains an invalid statement" );
                continue;
            }

            const std::string kind = statement["kind"].get<std::string>();

            if( kind == "outline_rect" )
            {
                result.operations.emplace_back( planOutline( statement, project ) );
                ++result.counts["upserts"].get_ref<int64_t&>();
            }
            else if( kind == "stackup" )
            {
                result.operations.emplace_back( planStackup( statement ) );
                ++result.counts["stackups"].get_ref<int64_t&>();
            }
            else if( kind == "trace" || kind == "arc" )
            {
                result.operations.emplace_back( planRoute( statement, project ) );
                ++result.counts["upserts"].get_ref<int64_t&>();
            }
            else if( kind == "via" )
            {
                result.operations.emplace_back( planVia( statement, project ) );
                ++result.counts["upserts"].get_ref<int64_t&>();
            }
            else if( kind == "zone" )
            {
                result.operations.emplace_back( planZone( statement, project ) );
                ++result.counts["upserts"].get_ref<int64_t&>();
            }
            else if( kind == "keepout" )
            {
                result.operations.emplace_back( planKeepout( statement, project ) );
                ++result.counts["upserts"].get_ref<int64_t&>();
            }
            else if( kind == "text" )
            {
                result.operations.emplace_back( planText( statement, project ) );
                ++result.counts["upserts"].get_ref<int64_t&>();
            }
            else if( kind == "dimension" )
            {
                result.operations.emplace_back( planDimension( statement, project ) );
                ++result.counts["upserts"].get_ref<int64_t&>();
            }
            else if( kind == "place" )
            {
                result.operations.push_back( { { "action", "place_by_reference" },
                                               { "component", statement.at( "component" ) },
                                               { "position", statement.at( "position" ) },
                                               { "rotationDegrees",
                                                 statement.at( "rotationDegrees" ) },
                                               { "side", statement.at( "side" ) },
                                               { "locked", statement.at( "locked" ) } } );
                ++result.counts["placements"].get_ref<int64_t&>();
            }
            else
            {
                result.operations.push_back( { { "action", "unsupported" },
                                               { "statementKind", kind },
                                               { "reason",
                                                 "backend type checker is not implemented" } } );
                ++result.counts["unsupported"].get_ref<int64_t&>();
            }
        }
    }
    catch( const JSON::exception& error )
    {
        diagnostic( result, "error", "invalid_board_ir", error.what() );
        result.operations = JSON::array();
        result.counts = { { "upserts", 0 }, { "placements", 0 }, { "rules", 0 },
                          { "netClasses", 0 }, { "netClassAssignments", 0 },
                          { "stackups", 0 }, { "unsupported", 0 } };
        return result;
    }

    result.fullyLowered = result.diagnostics.empty()
                          && result.counts["unsupported"].get<int64_t>() == 0;
    return result;
}

} // namespace KICHAD
