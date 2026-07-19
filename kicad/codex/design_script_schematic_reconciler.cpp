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

#include "design_script_schematic_reconciler.h"

#include "lossless_sexpr_document.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>


namespace
{

using DOCUMENT = KICHAD::LOSSLESS_SEXPR_DOCUMENT;
using JSON = nlohmann::json;
using RESULT = KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::RESULT;

constexpr size_t MAX_SCHEMATIC_FILE_BYTES = 16 * 1024 * 1024;


void diagnostic( RESULT& aResult, const std::string& aCode, const std::string& aMessage )
{
    aResult.diagnostics.push_back( { { "severity", "error" },
                                     { "code", aCode },
                                     { "message", aMessage } } );
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


std::string directScalar( const DOCUMENT& aDocument, size_t aParent,
                          const std::string& aHead )
{
    const std::vector<size_t> lists = directLists( aDocument, aParent, aHead );

    if( lists.size() != 1 )
        return {};

    const DOCUMENT::NODE& list = aDocument.Nodes().at( lists.front() );

    if( list.children.size() != 2
        || aDocument.Nodes().at( list.children[1] ).kind == DOCUMENT::NODE_KIND::LIST )
    {
        return {};
    }

    return aDocument.AtomText( list.children[1] );
}


std::string directUuid( const DOCUMENT& aDocument, size_t aNode )
{
    return directScalar( aDocument, aNode, "uuid" );
}


std::string managedItemUuid( const DOCUMENT& aDocument, size_t aNode )
{
    if( aDocument.ListHead( aNode ) != "rule_area" )
        return directUuid( aDocument, aNode );

    const std::vector<size_t> polylines = directLists( aDocument, aNode, "polyline" );
    return polylines.size() == 1 ? directUuid( aDocument, polylines.front() ) : std::string();
}


bool parseSchematic( const std::string& aSource, std::unique_ptr<DOCUMENT>& aDocument,
                     size_t& aRoot, std::string& aError )
{
    if( aSource.empty() || aSource.size() > MAX_SCHEMATIC_FILE_BYTES )
    {
        aError = "schematic source must contain 1 byte to 16 MiB";
        return false;
    }

    aDocument = DOCUMENT::Parse( aSource, &aError );

    if( !aDocument || aDocument->Roots().size() != 1 )
    {
        if( aError.empty() )
            aError = "schematic must contain exactly one root expression";

        return false;
    }

    aRoot = aDocument->Roots().front();

    if( aDocument->ListHead( aRoot ) != "kicad_sch" )
    {
        aError = "schematic root expression must be kicad_sch";
        return false;
    }

    if( directScalar( *aDocument, aRoot, "uuid" ).empty() )
    {
        aError = "schematic must contain exactly one direct screen UUID";
        return false;
    }

    return true;
}


bool validManagedItem( const JSON& aItem )
{
    const bool base = aItem.is_object() && aItem.contains( "file" ) && aItem["file"].is_string()
           && aItem.contains( "kind" ) && aItem["kind"].is_string()
           && ( aItem["kind"] == "sheet" || aItem["kind"] == "hierarchical_label"
                || aItem["kind"] == "symbol" || aItem["kind"] == "global_label"
                || aItem["kind"] == "label" || aItem["kind"] == "no_connect"
                || aItem["kind"] == "wire"
                || aItem["kind"] == "bus" || aItem["kind"] == "bus_entry"
                || aItem["kind"] == "junction" || aItem["kind"] == "rule_area"
                || aItem["kind"] == "netclass_flag" || aItem["kind"] == "text"
                || aItem["kind"] == "text_box"
                || aItem["kind"] == "polyline" || aItem["kind"] == "rectangle"
                || aItem["kind"] == "circle" || aItem["kind"] == "arc"
                || aItem["kind"] == "bezier"
                || aItem["kind"] == "lib_symbol"
                || aItem["kind"] == "bus_alias" )
           && aItem.contains( "logicalId" ) && aItem["logicalId"].is_string()
           && aItem.contains( "uuid" ) && aItem["uuid"].is_string();
    return base
           && ( aItem.value( "kind", "" ) != "lib_symbol"
                || ( aItem.contains( "libraryId" ) && aItem["libraryId"].is_string() ) )
           && ( aItem.value( "kind", "" ) != "bus_alias"
                || ( aItem.contains( "name" ) && aItem["name"].is_string() ) );
}


bool isDirectItem( const JSON& aItem )
{
    return aItem.value( "kind", "" ) != "lib_symbol"
           && aItem.value( "kind", "" ) != "bus_alias";
}


bool validateDesiredExpression( const JSON& aItem, std::string& aError )
{
    if( !validManagedItem( aItem ) || !aItem.contains( "source" )
        || !aItem["source"].is_string() || !isDirectItem( aItem ) )
    {
        aError = "planned schematic item is malformed";
        return false;
    }

    std::unique_ptr<DOCUMENT> document =
            DOCUMENT::Parse( aItem["source"].get<std::string>(), &aError );

    if( !document || document->Roots().size() != 1 )
    {
        if( aError.empty() )
            aError = "planned schematic item must contain one expression";

        return false;
    }

    const size_t root = document->Roots().front();

    if( document->ListHead( root ) != aItem["kind"].get<std::string>()
        || managedItemUuid( *document, root ) != aItem["uuid"].get<std::string>() )
    {
        aError = "planned schematic item kind or UUID is inconsistent";
        return false;
    }

    return true;
}


bool validateDesiredLibrarySymbol( const JSON& aItem, std::string& aError )
{
    if( !validManagedItem( aItem ) || aItem.value( "kind", "" ) != "lib_symbol"
        || !aItem.contains( "source" ) || !aItem["source"].is_string() )
    {
        aError = "planned schematic library symbol is malformed";
        return false;
    }

    std::unique_ptr<DOCUMENT> document =
            DOCUMENT::Parse( aItem["source"].get<std::string>(), &aError );

    if( !document || document->Roots().size() != 1 )
    {
        if( aError.empty() )
            aError = "planned schematic library symbol must contain one expression";

        return false;
    }

    const size_t root = document->Roots().front();
    const DOCUMENT::NODE& node = document->Nodes().at( root );
    return document->ListHead( root ) == "symbol" && node.children.size() >= 2
           && document->Nodes().at( node.children[1] ).kind != DOCUMENT::NODE_KIND::LIST
           && document->AtomText( node.children[1] )
                      == aItem["libraryId"].get<std::string>();
}


bool validateDesiredBusAlias( const JSON& aItem, std::string& aError )
{
    if( !validManagedItem( aItem ) || aItem.value( "kind", "" ) != "bus_alias"
        || !aItem.contains( "source" ) || !aItem["source"].is_string() )
    {
        aError = "planned schematic bus alias is malformed";
        return false;
    }

    std::unique_ptr<DOCUMENT> document =
            DOCUMENT::Parse( aItem["source"].get<std::string>(), &aError );

    if( !document || document->Roots().size() != 1 )
    {
        if( aError.empty() )
            aError = "planned schematic bus alias must contain one expression";

        return false;
    }

    const size_t root = document->Roots().front();
    const DOCUMENT::NODE& node = document->Nodes().at( root );
    return document->ListHead( root ) == "bus_alias" && node.children.size() >= 2
           && document->Nodes().at( node.children[1] ).kind != DOCUMENT::NODE_KIND::LIST
           && document->AtomText( node.children[1] ) == aItem["name"].get<std::string>();
}


bool reconcileBusAliases( DOCUMENT& aDocument, size_t aRoot, const JSON& aDesired,
                          const std::map<std::string, JSON>& aPrevious,
                          RESULT& aResult, std::string& aInsertions, std::string& aError )
{
    std::map<std::string, size_t> nativeAliases;

    for( size_t alias : directLists( aDocument, aRoot, "bus_alias" ) )
    {
        const DOCUMENT::NODE& node = aDocument.Nodes().at( alias );

        if( node.children.size() < 2
            || aDocument.Nodes().at( node.children[1] ).kind == DOCUMENT::NODE_KIND::LIST )
        {
            aError = "schematic contains a bus_alias without a scalar name";
            return false;
        }

        const std::string name = aDocument.AtomText( node.children[1] );

        if( !nativeAliases.emplace( name, alias ).second )
        {
            aError = "schematic contains duplicate bus_alias " + name;
            return false;
        }
    }

    std::map<std::string, JSON> desired;

    for( const JSON& alias : aDesired )
        desired.emplace( alias["name"].get<std::string>(), alias );

    for( const auto& [ name, alias ] : desired )
    {
        auto native = nativeAliases.find( name );

        if( native == nativeAliases.end() )
        {
            aInsertions += "  " + alias["source"].get<std::string>() + "\n";
        }
        else if( !aPrevious.contains( name ) )
        {
            aError = "bus_alias " + name + " is unmanaged and cannot be claimed by KDS";
            return false;
        }
        else if( !aDocument.ReplaceNode( native->second,
                                         alias["source"].get<std::string>(), &aError ) )
        {
            return false;
        }

        ++aResult.counts["itemsUpserted"].get_ref<int64_t&>();
    }

    for( const auto& [ name, previous ] : aPrevious )
    {
        if( desired.contains( name ) )
            continue;

        auto native = nativeAliases.find( name );

        if( native != nativeAliases.end()
            && !aDocument.RemoveNode( native->second, &aError ) )
        {
            return false;
        }

        if( native != nativeAliases.end() )
            ++aResult.counts["itemsRemoved"].get_ref<int64_t&>();
    }

    return true;
}


bool reconcileLibrarySymbols( DOCUMENT& aDocument, size_t aRoot, const JSON& aDesired,
                              const std::map<std::string, JSON>& aPrevious,
                              RESULT& aResult, std::string& aError )
{
    const std::vector<size_t> libraries = directLists( aDocument, aRoot, "lib_symbols" );

    if( libraries.size() != 1 )
    {
        aError = "schematic must contain exactly one direct lib_symbols expression";
        return false;
    }

    const size_t libraryNode = libraries.front();
    std::map<std::string, size_t> nativeSymbols;

    for( size_t child : aDocument.Nodes().at( libraryNode ).children )
    {
        if( aDocument.Nodes().at( child ).kind != DOCUMENT::NODE_KIND::LIST
            || aDocument.ListHead( child ) != "symbol" )
        {
            continue;
        }

        const DOCUMENT::NODE& symbol = aDocument.Nodes().at( child );

        if( symbol.children.size() < 2
            || aDocument.Nodes().at( symbol.children[1] ).kind == DOCUMENT::NODE_KIND::LIST )
        {
            aError = "lib_symbols contains a symbol without a scalar library ID";
            return false;
        }

        const std::string libraryId = aDocument.AtomText( symbol.children[1] );

        if( !nativeSymbols.emplace( libraryId, child ).second )
        {
            aError = "lib_symbols contains duplicate symbol " + libraryId;
            return false;
        }
    }

    std::map<std::string, JSON> desired;

    for( const JSON& symbol : aDesired )
        desired.emplace( symbol["libraryId"].get<std::string>(), symbol );

    std::string insertions;

    for( const auto& [ libraryId, symbol ] : desired )
    {
        auto native = nativeSymbols.find( libraryId );

        if( native == nativeSymbols.end() )
        {
            insertions += "\n    " + symbol["source"].get<std::string>();
        }
        else if( !aPrevious.contains( libraryId ) )
        {
            aError = "cached symbol " + libraryId
                     + " is unmanaged and cannot be claimed by KDS";
            return false;
        }
        else if( !aDocument.ReplaceNode( native->second,
                                         symbol["source"].get<std::string>(), &aError ) )
        {
            return false;
        }

        ++aResult.counts["itemsUpserted"].get_ref<int64_t&>();
    }

    for( const auto& [ libraryId, previous ] : aPrevious )
    {
        if( desired.contains( libraryId ) )
            continue;

        auto native = nativeSymbols.find( libraryId );

        if( native != nativeSymbols.end()
            && !aDocument.RemoveNode( native->second, &aError ) )
        {
            return false;
        }

        if( native != nativeSymbols.end() )
            ++aResult.counts["itemsRemoved"].get_ref<int64_t&>();
    }

    if( !insertions.empty()
        && !aDocument.InsertBeforeClosingList( libraryNode, insertions + "\n  ", &aError ) )
    {
        return false;
    }

    return true;
}


bool reconcileRootMetadata( DOCUMENT& aDocument, size_t aRoot, const JSON& aFile,
                            std::string& aInsertions, std::string& aError )
{
    if( !aFile.value( "root", false ) )
        return true;

    const bool ownsTitleBlock = aFile.value( "rootTitleBlockOwned", false );

    if( ( ownsTitleBlock
          && ( !aFile.contains( "rootTitleBlockSource" )
               || !aFile["rootTitleBlockSource"].is_string() ) )
        || ( !ownsTitleBlock
             && ( !aFile.contains( "rootTitleSource" )
                  || !aFile["rootTitleSource"].is_string() ) )
        || !aFile.contains( "rootInstancesSource" )
        || !aFile["rootInstancesSource"].is_string() )
    {
        aError = "root schematic plan is missing native metadata expressions";
        return false;
    }

    const std::vector<size_t> titleBlocks = directLists( aDocument, aRoot, "title_block" );

    if( titleBlocks.size() > 1 )
    {
        aError = "root schematic contains duplicate title blocks";
        return false;
    }

    if( titleBlocks.empty() )
    {
        std::string expression = ownsTitleBlock
                                         ? "  "
                                                   + aFile["rootTitleBlockSource"]
                                                             .get<std::string>()
                                                   + "\n"
                                         : "  (title_block\n    "
                                                   + aFile["rootTitleSource"].get<std::string>()
                                                   + "\n  )\n";
        const std::vector<size_t> libraries = directLists( aDocument, aRoot, "lib_symbols" );

        if( libraries.size() == 1 )
            return aDocument.InsertBeforeNode( libraries.front(), expression, &aError );

        return aDocument.InsertBeforeClosingList( aRoot, expression, &aError );
    }

    if( ownsTitleBlock )
    {
        if( !aDocument.ReplaceNode( titleBlocks.front(),
                                    aFile["rootTitleBlockSource"].get<std::string>(), &aError ) )
        {
            return false;
        }
    }
    else
    {
        const std::vector<size_t> titles = directLists( aDocument, titleBlocks.front(), "title" );

        if( titles.size() > 1 )
        {
            aError = "root schematic title block contains duplicate titles";
            return false;
        }

        if( titles.empty() )
        {
            if( !aDocument.InsertBeforeClosingList(
                        titleBlocks.front(), "\n    "
                                                     + aFile["rootTitleSource"].get<std::string>()
                                                     + "\n  ", &aError ) )
            {
                return false;
            }
        }
        else if( !aDocument.ReplaceNode( titles.front(),
                                         aFile["rootTitleSource"].get<std::string>(), &aError ) )
        {
            return false;
        }
    }

    const std::vector<size_t> instances = directLists( aDocument, aRoot, "sheet_instances" );

    if( instances.size() > 1 )
    {
        aError = "root schematic contains duplicate sheet_instances expressions";
        return false;
    }

    if( !instances.empty() )
    {
        if( !aInsertions.empty()
            && !aDocument.InsertBeforeNode( instances.front(), aInsertions, &aError ) )
        {
            return false;
        }

        aInsertions.clear();
        return aDocument.ReplaceNode( instances.front(),
                                      aFile["rootInstancesSource"].get<std::string>(), &aError );
    }

    const std::vector<size_t> embeddedFonts = directLists( aDocument, aRoot, "embedded_fonts" );
    const std::string expression = aInsertions + "  "
                                   + aFile["rootInstancesSource"].get<std::string>() + "\n";
    aInsertions.clear();

    if( !embeddedFonts.empty() )
        return aDocument.InsertBeforeNode( embeddedFonts.front(), expression, &aError );

    return aDocument.InsertBeforeClosingList( aRoot, expression, &aError );
}

} // namespace


namespace KICHAD
{

DESIGN_SCRIPT_SCHEMATIC_RECONCILER::RESULT
DESIGN_SCRIPT_SCHEMATIC_RECONCILER::Reconcile( const JSON& aOperation,
                                               const JSON& aPreviousManagedItems,
                                               const JSON& aLiveFiles )
{
    RESULT result;
    result.counts = { { "filesCreated", 0 }, { "filesUpdated", 0 },
                      { "filesUnchanged", 0 }, { "itemsUpserted", 0 },
                      { "itemsRemoved", 0 } };

    if( !aOperation.is_object()
        || aOperation.value( "action", "" ) != "reconcile_schematic_hierarchy"
        || !aOperation.contains( "files" ) || !aOperation["files"].is_array()
        || !aOperation.contains( "managedItems" ) || !aOperation["managedItems"].is_array()
        || !aPreviousManagedItems.is_array() || !aLiveFiles.is_array() )
    {
        diagnostic( result, "invalid_schematic_plan",
                    "schematic reconciliation requires one valid hierarchy plan and inventory" );
        return result;
    }

    std::map<std::string, std::map<std::string, JSON>> previousByFile;
    std::map<std::string, std::map<std::string, JSON>> previousLibrariesByFile;
    std::map<std::string, std::map<std::string, JSON>> previousBusAliasesByFile;
    std::set<std::string> previousLogicalIds;

    for( const JSON& item : aPreviousManagedItems )
    {
        if( !validManagedItem( item )
            || !previousLogicalIds.emplace( item["kind"].get<std::string>() + "\n"
                                            + item["logicalId"].get<std::string>() ).second
            || !( isDirectItem( item )
                          ? previousByFile[item["file"].get<std::string>()]
                                    .emplace( item["uuid"].get<std::string>(), item ).second
                  : item["kind"] == "lib_symbol"
                          ? previousLibrariesByFile[item["file"].get<std::string>()]
                                    .emplace( item["libraryId"].get<std::string>(), item ).second
                          : previousBusAliasesByFile[item["file"].get<std::string>()]
                                    .emplace( item["name"].get<std::string>(), item ).second ) )
        {
            diagnostic( result, "invalid_schematic_state",
                        "managed schematic state contains malformed or duplicate ownership" );
            return result;
        }
    }

    std::map<std::string, JSON> desiredFiles;
    std::set<std::string> desiredLogicalIds;

    for( const JSON& file : aOperation["files"] )
    {
        if( !file.is_object() || !file.contains( "path" ) || !file["path"].is_string()
            || !file.contains( "screenUuid" ) || !file["screenUuid"].is_string()
            || !file.contains( "root" ) || !file["root"].is_boolean()
            || !file.contains( "items" ) || !file["items"].is_array()
            || !file.contains( "libSymbols" ) || !file["libSymbols"].is_array()
            || !file.contains( "busAliases" ) || !file["busAliases"].is_array()
            || !file.contains( "newDocumentSource" )
            || !file["newDocumentSource"].is_string()
            || !desiredFiles.emplace( file["path"].get<std::string>(), file ).second )
        {
            diagnostic( result, "invalid_schematic_plan",
                        "schematic plan contains a malformed or duplicate file" );
            return result;
        }

        for( const JSON& item : file["items"] )
        {
            std::string validationError;

            if( !validateDesiredExpression( item, validationError )
                || !desiredLogicalIds.emplace( item["kind"].get<std::string>() + "\n"
                                               + item["logicalId"].get<std::string>() ).second )
            {
                diagnostic( result, "invalid_schematic_plan",
                            validationError.empty() ? "schematic plan contains duplicate ownership"
                                                    : validationError );
                return result;
            }
        }

        for( const JSON& item : file["libSymbols"] )
        {
            std::string validationError;

            if( !validateDesiredLibrarySymbol( item, validationError )
                || !desiredLogicalIds.emplace( item["kind"].get<std::string>() + "\n"
                                               + item["logicalId"].get<std::string>() ).second )
            {
                diagnostic( result, "invalid_schematic_plan",
                            validationError.empty() ? "schematic plan contains duplicate ownership"
                                                    : validationError );
                return result;
            }
        }

        for( const JSON& item : file["busAliases"] )
        {
            std::string validationError;

            if( !validateDesiredBusAlias( item, validationError )
                || !desiredLogicalIds.emplace( item["kind"].get<std::string>() + "\n"
                                               + item["logicalId"].get<std::string>() ).second )
            {
                diagnostic( result, "invalid_schematic_plan",
                            validationError.empty() ? "schematic plan contains duplicate ownership"
                                                    : validationError );
                return result;
            }
        }
    }

    if( aOperation["managedItems"].size() != desiredLogicalIds.size() )
    {
        diagnostic( result, "invalid_schematic_plan",
                    "schematic file items and ownership inventory are inconsistent" );
        return result;
    }

    std::set<std::string> operationLogicalIds;

    for( const JSON& ownership : aOperation["managedItems"] )
    {
        if( !validManagedItem( ownership ) )
        {
            diagnostic( result, "invalid_schematic_plan",
                        "schematic ownership inventory contains a malformed item" );
            return result;
        }

        const std::string key = ownership["kind"].get<std::string>() + "\n"
                                + ownership["logicalId"].get<std::string>();

        if( !operationLogicalIds.emplace( key ).second || !desiredLogicalIds.contains( key ) )
        {
            diagnostic( result, "invalid_schematic_plan",
                        "schematic file items and ownership inventory are inconsistent" );
            return result;
        }

        auto desiredFile = desiredFiles.find( ownership["file"].get<std::string>() );

        if( desiredFile == desiredFiles.end() )
        {
            diagnostic( result, "invalid_schematic_plan",
                        "schematic ownership references an undesired file" );
            return result;
        }

        const JSON& candidates = ownership["kind"] == "lib_symbol"
                                         ? desiredFile->second["libSymbols"]
                                 : ownership["kind"] == "bus_alias"
                                         ? desiredFile->second["busAliases"]
                                         : desiredFile->second["items"];
        const auto match = std::find_if(
                candidates.begin(), candidates.end(), [&]( const JSON& aItem )
                {
                    return aItem.value( "kind", "" ) == ownership.value( "kind", "" )
                           && aItem.value( "logicalId", "" )
                                      == ownership.value( "logicalId", "" )
                           && aItem.value( "uuid", "" ) == ownership.value( "uuid", "" )
                           && aItem.value( "file", "" ) == ownership.value( "file", "" )
                           && aItem.value( "libraryId", "" )
                                      == ownership.value( "libraryId", "" )
                           && aItem.value( "name", "" ) == ownership.value( "name", "" );
                } );

        if( match == candidates.end() )
        {
            diagnostic( result, "invalid_schematic_plan",
                        "schematic file items and ownership inventory are inconsistent" );
            return result;
        }
    }

    std::map<std::string, JSON> liveFiles;

    for( const JSON& live : aLiveFiles )
    {
        if( !live.is_object() || !live.contains( "path" ) || !live["path"].is_string()
            || !live.contains( "present" ) || !live["present"].is_boolean()
            || ( live["present"].get<bool>()
                 && ( !live.contains( "source" ) || !live["source"].is_string() ) )
            || !liveFiles.emplace( live["path"].get<std::string>(), live ).second )
        {
            diagnostic( result, "invalid_schematic_inventory",
                        "schematic inventory contains a malformed or duplicate file" );
            return result;
        }
    }

    std::set<std::string> relevantPaths;

    for( const auto& [ path, file ] : desiredFiles )
        relevantPaths.emplace( path );

    for( const auto& [ path, items ] : previousByFile )
        relevantPaths.emplace( path );

    for( const auto& [ path, symbols ] : previousLibrariesByFile )
        relevantPaths.emplace( path );

    for( const auto& [ path, aliases ] : previousBusAliasesByFile )
        relevantPaths.emplace( path );

    for( const std::string& path : relevantPaths )
    {
        auto liveEntry = liveFiles.find( path );

        if( liveEntry == liveFiles.end() )
        {
            diagnostic( result, "invalid_schematic_inventory",
                        "schematic inventory is missing " + path );
            return result;
        }

        const JSON& live = liveEntry->second;
        const bool present = live["present"].get<bool>();
        auto desiredEntry = desiredFiles.find( path );

        if( !present && desiredEntry == desiredFiles.end() )
        {
            ++result.counts["filesUnchanged"].get_ref<int64_t&>();
            continue;
        }

        if( !present )
        {
            const std::string source =
                    desiredEntry->second["newDocumentSource"].get<std::string>();
            std::unique_ptr<DOCUMENT> parsed;
            size_t root = DOCUMENT::NO_NODE;
            std::string parseError;

            if( !parseSchematic( source, parsed, root, parseError )
                || directScalar( *parsed, root, "uuid" )
                           != desiredEntry->second["screenUuid"].get<std::string>() )
            {
                diagnostic( result, "invalid_schematic_plan",
                            parseError.empty() ? "new schematic screen UUID is inconsistent"
                                               : parseError );
                return result;
            }

            result.fileActions.push_back( { { "path", path },
                                            { "previousPresent", false },
                                            { "previousSource", "" },
                                            { "source", source } } );
            ++result.counts["filesCreated"].get_ref<int64_t&>();
            result.counts["itemsUpserted"] =
                    result.counts["itemsUpserted"].get<int64_t>()
                    + desiredEntry->second["items"].size()
                    + desiredEntry->second["libSymbols"].size()
                    + desiredEntry->second["busAliases"].size();
            continue;
        }

        const std::string previousSource = live["source"].get<std::string>();
        std::unique_ptr<DOCUMENT> document;
        size_t root = DOCUMENT::NO_NODE;
        std::string editError;

        if( !parseSchematic( previousSource, document, root, editError ) )
        {
            diagnostic( result, "invalid_native_schematic", path + ": " + editError );
            return result;
        }

        if( desiredEntry != desiredFiles.end()
            && directScalar( *document, root, "uuid" )
                       != desiredEntry->second["screenUuid"].get<std::string>() )
        {
            diagnostic( result, "stale_schematic_inventory",
                        path + " changed screen identity after planning" );
            return result;
        }

        std::map<std::string, size_t> nativeItems;
        std::map<std::string, std::string> nativeKinds;

        for( size_t child : document->Nodes().at( root ).children )
        {
            if( document->Nodes().at( child ).kind != DOCUMENT::NODE_KIND::LIST )
                continue;

            const std::string uuid = managedItemUuid( *document, child );

            if( uuid.empty() )
                continue;

            if( !nativeItems.emplace( uuid, child ).second )
            {
                diagnostic( result, "invalid_native_schematic",
                            path + " contains duplicate direct item UUID " + uuid );
                return result;
            }

            nativeKinds.emplace( uuid, document->ListHead( child ) );
        }

        std::map<std::string, JSON> desiredItems;

        if( desiredEntry != desiredFiles.end() )
        {
            for( const JSON& item : desiredEntry->second["items"] )
                desiredItems.emplace( item["uuid"].get<std::string>(), item );
        }

        std::string insertions;

        for( const auto& [ uuid, item ] : desiredItems )
        {
            auto native = nativeItems.find( uuid );

            if( native == nativeItems.end() )
            {
                insertions += "  " + item["source"].get<std::string>() + "\n";
            }
            else if( nativeKinds.at( uuid ) != item["kind"].get<std::string>() )
            {
                diagnostic( result, "schematic_identity_collision",
                            path + " uses managed UUID " + uuid + " for an unmanaged kind" );
                return result;
            }
            else if( !document->ReplaceNode( native->second,
                                             item["source"].get<std::string>(), &editError ) )
            {
                diagnostic( result, "schematic_edit_failed", path + ": " + editError );
                return result;
            }

            ++result.counts["itemsUpserted"].get_ref<int64_t&>();
        }

        for( const auto& [ uuid, previousItem ] : previousByFile[path] )
        {
            if( desiredItems.contains( uuid ) )
                continue;

            auto native = nativeItems.find( uuid );

            if( native == nativeItems.end() )
                continue;

            if( nativeKinds.at( uuid ) != previousItem["kind"].get<std::string>() )
            {
                diagnostic( result, "schematic_identity_collision",
                            path + " changed the kind of previously managed UUID " + uuid );
                return result;
            }

            if( !document->RemoveNode( native->second, &editError ) )
            {
                diagnostic( result, "schematic_edit_failed", path + ": " + editError );
                return result;
            }

            ++result.counts["itemsRemoved"].get_ref<int64_t&>();
        }

        const JSON desiredLibrarySymbols = desiredEntry == desiredFiles.end()
                                                   ? JSON::array()
                                                   : desiredEntry->second["libSymbols"];

        if( !reconcileLibrarySymbols( *document, root, desiredLibrarySymbols,
                                      previousLibrariesByFile[path], result, editError ) )
        {
            diagnostic( result, "schematic_edit_failed", path + ": " + editError );
            return result;
        }

        const JSON desiredBusAliases = desiredEntry == desiredFiles.end()
                                               ? JSON::array()
                                               : desiredEntry->second["busAliases"];

        if( !reconcileBusAliases( *document, root, desiredBusAliases,
                                  previousBusAliasesByFile[path], result, insertions,
                                  editError ) )
        {
            diagnostic( result, "schematic_edit_failed", path + ": " + editError );
            return result;
        }

        if( desiredEntry != desiredFiles.end()
            && !reconcileRootMetadata( *document, root, desiredEntry->second,
                                       insertions, editError ) )
        {
            diagnostic( result, "schematic_edit_failed", path + ": " + editError );
            return result;
        }

        if( !insertions.empty() )
        {
            std::vector<size_t> anchors = directLists( *document, root, "sheet_instances" );

            if( anchors.empty() )
                anchors = directLists( *document, root, "embedded_fonts" );

            const bool inserted = anchors.empty()
                                          ? document->InsertBeforeClosingList(
                                                    root, insertions, &editError )
                                          : document->InsertBeforeNode(
                                                    anchors.front(), insertions, &editError );

            if( !inserted )
            {
                diagnostic( result, "schematic_edit_failed", path + ": " + editError );
                return result;
            }
        }

        std::string desiredSource;

        if( !document->Render( desiredSource, &editError ) )
        {
            diagnostic( result, "schematic_edit_failed", path + ": " + editError );
            return result;
        }

        if( desiredSource == previousSource )
        {
            ++result.counts["filesUnchanged"].get_ref<int64_t&>();
        }
        else
        {
            result.fileActions.push_back( { { "path", path },
                                            { "previousPresent", true },
                                            { "previousSource", previousSource },
                                            { "source", desiredSource } } );
            ++result.counts["filesUpdated"].get_ref<int64_t&>();
        }
    }

    result.managedItems = aOperation["managedItems"];
    result.ok = true;
    return result;
}

} // namespace KICHAD
