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

#include "design_script_schematic_planner.h"

#include "design_script_pcb_planner.h"
#include "lossless_sexpr_document.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>


namespace
{

using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT;

constexpr int SCHEMATIC_FILE_VERSION = 20260306;


void diagnostic( RESULT& aResult, const std::string& aCode, const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", "error" },
                                     { "code", aCode },
                                     { "message", aMessage } } );
}


std::string quoted( const std::string& aText )
{
    std::string result = "\"";

    for( char character : aText )
    {
        switch( character )
        {
        case '\\': result += "\\\\"; break;
        case '"':  result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:   result.push_back( character ); break;
        }
    }

    result.push_back( '"' );
    return result;
}


std::string millimetres( int64_t aNanometres )
{
    const bool negative = aNanometres < 0;
    uint64_t magnitude = negative ? static_cast<uint64_t>( -( aNanometres + 1 ) ) + 1
                                  : static_cast<uint64_t>( aNanometres );
    const uint64_t whole = magnitude / 1'000'000;
    uint64_t fraction = magnitude % 1'000'000;
    std::ostringstream output;

    if( negative )
        output << '-';

    output << whole;

    if( fraction != 0 )
    {
        std::string digits = std::to_string( fraction + 1'000'000 ).substr( 1 );

        while( !digits.empty() && digits.back() == '0' )
            digits.pop_back();

        output << '.' << digits;
    }

    return output.str();
}


std::string stableUuid( const std::string& aProject, const std::string& aKind,
                        const std::string& aLogicalId )
{
    return KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid( aProject, aKind, aLogicalId );
}


std::string effects( const std::string& aJustify )
{
    std::string result =
            "    (effects\n"
            "      (font (size 1.27 1.27))";

    if( !aJustify.empty() )
        result += "\n      (justify " + aJustify + ")";

    return result + "\n    )\n";
}


int sideAngle( const std::string& aSide )
{
    if( aSide == "left" )
        return 180;

    if( aSide == "top" )
        return 90;

    if( aSide == "bottom" )
        return 270;

    return 0;
}


std::string sheetPin( const JSON& aPin, const std::string& aUuid )
{
    std::ostringstream output;
    output << "    (pin " << quoted( aPin.at( "name" ).get<std::string>() ) << ' '
           << aPin.at( "direction" ).get<std::string>() << "\n"
           << "      (at " << millimetres( aPin.at( "position" ).at( "xNm" ).get<int64_t>() )
           << ' ' << millimetres( aPin.at( "position" ).at( "yNm" ).get<int64_t>() )
           << ' ' << sideAngle( aPin.at( "side" ).get<std::string>() ) << ")\n"
           << "      (uuid " << quoted( aUuid ) << ")\n"
           << effects( "left" )
           << "    )\n";
    return output.str();
}


std::string sheetExpression( const JSON& aSheet, const std::string& aProject,
                             const std::string& aParentPath, int aPage )
{
    const std::string id = aSheet.at( "id" ).get<std::string>();
    const int64_t x = aSheet.at( "position" ).at( "xNm" ).get<int64_t>();
    const int64_t y = aSheet.at( "position" ).at( "yNm" ).get<int64_t>();
    const int64_t width = aSheet.at( "size" ).at( "xNm" ).get<int64_t>();
    const int64_t height = aSheet.at( "size" ).at( "yNm" ).get<int64_t>();
    const std::string uuid = stableUuid( aProject, "schematic_sheet", id );
    std::ostringstream output;
    output << "(sheet\n"
           << "    (at " << millimetres( x ) << ' ' << millimetres( y ) << ")\n"
           << "    (size " << millimetres( width ) << ' ' << millimetres( height ) << ")\n"
           << "    (exclude_from_sim no)\n"
           << "    (in_bom yes)\n"
           << "    (on_board yes)\n"
           << "    (dnp no)\n"
           << "    (stroke (width 0.1524) (type solid))\n"
           << "    (fill (color 0 0 0 0))\n"
           << "    (uuid " << quoted( uuid ) << ")\n"
           << "    (property \"Sheetname\" "
           << quoted( aSheet.at( "title" ).get<std::string>() ) << "\n"
           << "      (at " << millimetres( x ) << ' ' << millimetres( y - 711'600 )
           << " 0)\n"
           << "      (show_name no)\n"
           << "      (do_not_autoplace no)\n"
           << effects( "left bottom" )
           << "    )\n"
           << "    (property \"Sheetfile\" "
           << quoted( aSheet.at( "file" ).get<std::string>() ) << "\n"
           << "      (at " << millimetres( x ) << ' '
           << millimetres( y + height + 1'384'600 ) << " 0)\n"
           << "      (show_name no)\n"
           << "      (do_not_autoplace no)\n"
           << effects( "left top" )
           << "    )\n";

    std::vector<JSON> pins = aSheet.at( "pins" ).get<std::vector<JSON>>();
    std::sort( pins.begin(), pins.end(), []( const JSON& aLeft, const JSON& aRight )
               {
                   return aLeft.at( "name" ).get<std::string>()
                          < aRight.at( "name" ).get<std::string>();
               } );

    for( const JSON& pin : pins )
    {
        output << sheetPin( pin, stableUuid( aProject, "schematic_sheet_pin",
                                             id + "/" + pin.at( "name" ).get<std::string>() ) );
    }

    output << "    (instances\n"
           << "      (project " << quoted( aProject ) << "\n"
           << "        (path " << quoted( aParentPath ) << " (page "
           << quoted( std::to_string( aPage ) ) << "))\n"
           << "      )\n"
           << "    )\n"
           << "  )";
    return output.str();
}


std::string hierarchicalLabel( const JSON& aSheet, const JSON& aPin,
                               const std::string& aProject, size_t aIndex )
{
    const std::string id = aSheet.at( "id" ).get<std::string>();
    const int64_t y = 20'000'000 + static_cast<int64_t>( aIndex ) * 2'540'000;
    std::ostringstream output;
    output << "(hierarchical_label " << quoted( aPin.at( "name" ).get<std::string>() )
           << "\n"
           << "    (shape " << aPin.at( "direction" ).get<std::string>() << ")\n"
           << "    (at 20 " << millimetres( y ) << " 180)\n"
           << effects( "right" )
           << "    (uuid "
           << quoted( stableUuid( aProject, "schematic_hier_label",
                                  id + "/" + aPin.at( "name" ).get<std::string>() ) )
           << ")\n"
           << "  )";
    return output.str();
}


bool validSheetShape( const JSON& aSheet )
{
    if( !aSheet.is_object() || !aSheet.contains( "id" ) || !aSheet["id"].is_string()
        || !aSheet.contains( "parent" )
        || !( aSheet["parent"].is_null() || aSheet["parent"].is_string() )
        || !aSheet.contains( "file" ) || !aSheet["file"].is_string()
        || !aSheet.contains( "title" ) || !aSheet["title"].is_string()
        || !aSheet.contains( "pins" ) || !aSheet["pins"].is_array() )
    {
        return false;
    }

    if( !aSheet["parent"].is_null()
        && ( !aSheet.contains( "position" ) || !aSheet["position"].is_object()
             || !aSheet["position"].contains( "xNm" )
             || !aSheet["position"]["xNm"].is_number_integer()
             || !aSheet["position"].contains( "yNm" )
             || !aSheet["position"]["yNm"].is_number_integer()
             || !aSheet.contains( "size" ) || !aSheet["size"].is_object()
             || !aSheet["size"].contains( "xNm" )
             || !aSheet["size"]["xNm"].is_number_integer()
             || !aSheet["size"].contains( "yNm" )
             || !aSheet["size"]["yNm"].is_number_integer() ) )
    {
        return false;
    }

    for( const JSON& pin : aSheet["pins"] )
    {
        if( !pin.is_object() || !pin.contains( "name" ) || !pin["name"].is_string()
            || !pin.contains( "direction" ) || !pin["direction"].is_string()
            || !pin.contains( "side" ) || !pin["side"].is_string()
            || !pin.contains( "position" ) || !pin["position"].is_object()
            || !pin["position"].contains( "xNm" )
            || !pin["position"]["xNm"].is_number_integer()
            || !pin["position"].contains( "yNm" )
            || !pin["position"]["yNm"].is_number_integer() )
        {
            return false;
        }
    }

    return true;
}


bool validUuid( const std::string& aUuid )
{
    if( aUuid.size() != 36 )
        return false;

    for( size_t index = 0; index < aUuid.size(); ++index )
    {
        if( index == 8 || index == 13 || index == 18 || index == 23 )
        {
            if( aUuid[index] != '-' )
                return false;
        }
        else if( !std::isxdigit( static_cast<unsigned char>( aUuid[index] ) ) )
        {
            return false;
        }
    }

    return true;
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT
DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan( const JSON& aCompilerIr,
                                      const JSON& aExistingScreenUuids )
{
    RESULT result;
    result.counts = { { "files", 0 }, { "sheets", 0 }, { "pins", 0 },
                      { "managedItems", 0 } };

    if( !aCompilerIr.is_object() || aCompilerIr.value( "language", "" ) != "kichad-design"
        || aCompilerIr.value( "version", 0 ) != 1 || !aCompilerIr.contains( "project" )
        || !aCompilerIr["project"].is_object()
        || !aCompilerIr["project"].contains( "name" )
        || !aCompilerIr["project"]["name"].is_string()
        || !aCompilerIr.contains( "schematic" ) || !aCompilerIr["schematic"].is_object()
        || !aCompilerIr["schematic"].contains( "sheets" )
        || !aCompilerIr["schematic"]["sheets"].is_array()
        || !aExistingScreenUuids.is_object() )
    {
        diagnostic( result, "invalid_compiler_ir",
                    "schematic planning requires valid KiChad Design Script version 1 IR" );
        return result;
    }

    const std::string project = aCompilerIr["project"]["name"].get<std::string>();
    const JSON& sourceSheets = aCompilerIr["schematic"]["sheets"];

    if( sourceSheets.empty() )
    {
        result.fullyLowered = true;
        return result;
    }

    std::map<std::string, JSON> sheets;
    std::map<std::string, std::string> fileOwners;
    std::string rootId;

    for( const JSON& sheet : sourceSheets )
    {
        if( !validSheetShape( sheet ) )
        {
            diagnostic( result, "invalid_schematic_ir",
                        "schematic IR contains a malformed sheet or pin" );
            continue;
        }

        const std::string id = sheet["id"].get<std::string>();
        const std::string file = sheet["file"].get<std::string>();

        if( id.empty() || !sheets.emplace( id, sheet ).second )
            diagnostic( result, "invalid_schematic_ir", "schematic sheet IDs must be unique" );

        auto [fileEntry, fileInserted] = fileOwners.emplace( file, id );

        if( !fileInserted )
        {
            diagnostic( result, "shared_sheet_file_not_supported",
                        "sheet file " + file + " is referenced by both " + fileEntry->second
                                + " and " + id
                                + "; shared-screen instance lowering is not yet supported" );
        }

        if( sheet["parent"].is_null() )
        {
            if( !rootId.empty() )
                diagnostic( result, "invalid_schematic_ir", "schematic IR has multiple roots" );

            rootId = id;
        }
    }

    if( rootId.empty() )
        diagnostic( result, "invalid_schematic_ir", "schematic IR has no root sheet" );

    std::map<std::string, std::vector<std::string>> children;

    for( const auto& [ id, sheet ] : sheets )
    {
        if( sheet["parent"].is_null() )
            continue;

        const std::string parent = sheet["parent"].get<std::string>();

        if( !sheets.contains( parent ) )
            diagnostic( result, "invalid_schematic_ir", "sheet " + id + " has no parent " + parent );
        else
            children[parent].push_back( id );
    }

    for( auto& [ parent, childIds ] : children )
        std::sort( childIds.begin(), childIds.end() );

    std::set<std::string> visiting;
    std::set<std::string> visited;
    std::vector<std::string> orderedIds;
    std::function<void( const std::string& )> visit = [&]( const std::string& aId )
    {
        if( visiting.contains( aId ) )
        {
            diagnostic( result, "invalid_schematic_ir", "schematic IR contains a parent cycle" );
            return;
        }

        if( visited.contains( aId ) )
            return;

        visiting.insert( aId );
        visited.insert( aId );
        orderedIds.push_back( aId );

        for( const std::string& child : children[aId] )
            visit( child );

        visiting.erase( aId );
    };

    if( !rootId.empty() )
        visit( rootId );

    if( visited.size() != sheets.size() )
        diagnostic( result, "invalid_schematic_ir", "schematic IR contains unreachable sheets" );

    if( !result.diagnostics.empty() )
        return result;

    std::map<std::string, int> pages;

    for( size_t index = 0; index < orderedIds.size(); ++index )
        pages[orderedIds[index]] = static_cast<int>( index + 1 );

    std::map<std::string, std::string> screenUuids;

    for( const auto& [ id, sheet ] : sheets )
    {
        const std::string file = sheet["file"].get<std::string>();
        std::string screenUuid = stableUuid( project, "schematic_file", file );

        if( aExistingScreenUuids.contains( file ) )
        {
            if( !aExistingScreenUuids[file].is_string()
                || !validUuid( aExistingScreenUuids[file].get<std::string>() ) )
            {
                diagnostic( result, "invalid_screen_identity",
                            "existing screen UUID for " + file + " is invalid" );
                return result;
            }

            screenUuid = aExistingScreenUuids[file].get<std::string>();
        }

        screenUuids.emplace( file, std::move( screenUuid ) );
    }

    const std::string rootScreenUuid =
            screenUuids.at( sheets.at( rootId )["file"].get<std::string>() );
    JSON files = JSON::array();
    JSON managedItems = JSON::array();

    try
    {
        for( const std::string& id : orderedIds )
        {
            const JSON& sheet = sheets.at( id );
            const std::string file = sheet["file"].get<std::string>();
            const std::string screenUuid = screenUuids.at( file );
            JSON items = JSON::array();

            if( id != rootId )
            {
                std::vector<JSON> pins = sheet["pins"].get<std::vector<JSON>>();
                std::sort( pins.begin(), pins.end(), []( const JSON& aLeft, const JSON& aRight )
                           {
                               return aLeft.at( "name" ).get<std::string>()
                                      < aRight.at( "name" ).get<std::string>();
                           } );

                for( size_t pinIndex = 0; pinIndex < pins.size(); ++pinIndex )
                {
                    const JSON& pin = pins[pinIndex];
                    const std::string logicalId = id + "/" + pin["name"].get<std::string>();
                    const std::string uuid =
                            stableUuid( project, "schematic_hier_label", logicalId );
                    const std::string source =
                            hierarchicalLabel( sheet, pin, project, pinIndex );
                    items.push_back( { { "kind", "hierarchical_label" },
                                       { "file", file },
                                       { "logicalId", logicalId },
                                       { "uuid", uuid },
                                       { "source", source } } );
                    managedItems.push_back( { { "file", file },
                                              { "kind", "hierarchical_label" },
                                              { "logicalId", logicalId },
                                              { "uuid", uuid } } );
                }
            }

            for( const std::string& childId : children[id] )
            {
                const JSON& child = sheets.at( childId );
                std::vector<std::string> ancestors;
                std::string cursor = id;

                while( cursor != rootId )
                {
                    ancestors.push_back( cursor );
                    cursor = sheets.at( cursor )["parent"].get<std::string>();
                }

                std::reverse( ancestors.begin(), ancestors.end() );
                std::string parentPath = "/" + rootScreenUuid;

                for( const std::string& ancestor : ancestors )
                {
                    parentPath += "/"
                                  + stableUuid( project, "schematic_sheet", ancestor );
                }

                const std::string uuid = stableUuid( project, "schematic_sheet", childId );
                const std::string source =
                        sheetExpression( child, project, parentPath, pages.at( childId ) );
                items.push_back( { { "kind", "sheet" },
                                   { "file", file },
                                   { "logicalId", childId },
                                   { "uuid", uuid },
                                   { "source", source } } );
                managedItems.push_back( { { "file", file },
                                          { "kind", "sheet" },
                                          { "logicalId", childId },
                                          { "uuid", uuid } } );
            }

            std::ostringstream document;
            document << "(kicad_sch\n"
                     << "  (version " << SCHEMATIC_FILE_VERSION << ")\n"
                     << "  (generator \"eeschema\")\n"
                     << "  (generator_version \"10.0\")\n"
                     << "  (uuid " << quoted( screenUuid ) << ")\n"
                     << "  (paper \"A4\")\n";

            if( id == rootId )
            {
                document << "  (title_block\n"
                         << "    (title " << quoted( sheet["title"].get<std::string>() ) << ")\n"
                         << "  )\n";
            }

            document << "  (lib_symbols)\n";

            for( const JSON& item : items )
                document << "  " << item["source"].get<std::string>() << "\n";

            if( id == rootId )
            {
                document << "  (sheet_instances\n"
                         << "    (path \"/\" (page \"1\"))\n"
                         << "  )\n"
                         << "  (embedded_fonts no)\n";
            }

            document << ")\n";
            std::string parseError;

            if( !LOSSLESS_SEXPR_DOCUMENT::Parse( document.str(), &parseError ) )
            {
                diagnostic( result, "invalid_generated_schematic",
                            "generated " + file + " is not a valid s-expression: " + parseError );
                return result;
            }

            files.push_back( { { "path", file },
                               { "sheetId", id },
                               { "screenUuid", screenUuid },
                               { "page", pages.at( id ) },
                               { "root", id == rootId },
                               { "title", sheet["title"] },
                               { "rootTitleSource",
                                 id == rootId
                                         ? JSON( "(title "
                                                 + quoted( sheet["title"].get<std::string>() )
                                                 + ")" )
                                         : JSON( nullptr ) },
                               { "rootInstancesSource",
                                 id == rootId
                                         ? JSON( "(sheet_instances\n"
                                                 "    (path \"/\" (page \"1\"))\n"
                                                 "  )" )
                                         : JSON( nullptr ) },
                               { "items", std::move( items ) },
                               { "newDocumentSource", document.str() } } );
            result.counts["pins"] = result.counts["pins"].get<size_t>()
                                    + sheet["pins"].size();
        }
    }
    catch( const JSON::exception& error )
    {
        diagnostic( result, "invalid_schematic_ir", error.what() );
        return result;
    }

    result.operations.push_back( { { "action", "reconcile_schematic_hierarchy" },
                                   { "project", project },
                                   { "rootFile", sheets.at( rootId )["file"] },
                                   { "files", std::move( files ) },
                                   { "managedItems", std::move( managedItems ) } } );
    result.counts["files"] = orderedIds.size();
    result.counts["sheets"] = orderedIds.size();
    result.counts["managedItems"] = result.operations[0]["managedItems"].size();
    result.fullyLowered = true;
    return result;
}

} // namespace KICHAD
