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


std::string stableUuid( const std::string& aProject, const std::string& aKind,
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


std::string lockedEnum( const JSON& aStatement )
{
    return aStatement.value( "locked", false ) ? "LS_LOCKED" : "LS_UNLOCKED";
}


JSON planOutline( const JSON& aStatement, const std::string& aProject )
{
    const std::string logicalId = aStatement.at( "logicalId" ).get<std::string>();
    const std::string itemId = stableUuid( aProject, "shape", logicalId );
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


JSON planRoute( const JSON& aStatement, const std::string& aProject )
{
    const std::string kind = aStatement.at( "kind" ).get<std::string>();
    const std::string logicalId = aStatement.at( "logicalId" ).get<std::string>();
    const std::string itemId = stableUuid( aProject, kind, logicalId );
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
    const std::string itemId = stableUuid( aProject, "via", logicalId );
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

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_PCB_PLANNER::RESULT DESIGN_SCRIPT_PCB_PLANNER::Plan( const JSON& aCompilerIr )
{
    RESULT result;
    result.counts = { { "upserts", 0 }, { "placements", 0 }, { "unsupported", 0 } };

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
                                                 kind == "stackup"
                                                         ? "KiCad 10 IPC stackup mutation is not implemented"
                                                         : "backend type checker is not implemented" } } );
                ++result.counts["unsupported"].get_ref<int64_t&>();
            }
        }
    }
    catch( const JSON::exception& error )
    {
        diagnostic( result, "error", "invalid_board_ir", error.what() );
        result.operations = JSON::array();
        result.counts = { { "upserts", 0 }, { "placements", 0 }, { "unsupported", 0 } };
        return result;
    }

    result.fullyLowered = result.diagnostics.empty()
                          && result.counts["unsupported"].get<int64_t>() == 0;
    return result;
}

} // namespace KICHAD
