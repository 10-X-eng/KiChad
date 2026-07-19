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
    return aItem.is_object() && aItem.contains( "file" ) && aItem["file"].is_string()
           && aItem.contains( "kind" ) && aItem["kind"].is_string()
           && ( aItem["kind"] == "sheet" || aItem["kind"] == "hierarchical_label" )
           && aItem.contains( "logicalId" ) && aItem["logicalId"].is_string()
           && aItem.contains( "uuid" ) && aItem["uuid"].is_string();
}


bool validateDesiredExpression( const JSON& aItem, std::string& aError )
{
    if( !validManagedItem( aItem ) || !aItem.contains( "source" )
        || !aItem["source"].is_string() )
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
        || directUuid( *document, root ) != aItem["uuid"].get<std::string>() )
    {
        aError = "planned schematic item kind or UUID is inconsistent";
        return false;
    }

    return true;
}


bool reconcileRootMetadata( DOCUMENT& aDocument, size_t aRoot, const JSON& aFile,
                            std::string& aInsertions, std::string& aError )
{
    if( !aFile.value( "root", false ) )
        return true;

    if( !aFile.contains( "rootTitleSource" ) || !aFile["rootTitleSource"].is_string()
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
        std::string expression = "  (title_block\n    "
                                 + aFile["rootTitleSource"].get<std::string>() + "\n  )\n";
        const std::vector<size_t> libraries = directLists( aDocument, aRoot, "lib_symbols" );

        if( libraries.size() == 1 )
            return aDocument.InsertBeforeNode( libraries.front(), expression, &aError );

        return aDocument.InsertBeforeClosingList( aRoot, expression, &aError );
    }

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
    std::set<std::string> previousLogicalIds;

    for( const JSON& item : aPreviousManagedItems )
    {
        if( !validManagedItem( item )
            || !previousLogicalIds.emplace( item["kind"].get<std::string>() + "\n"
                                            + item["logicalId"].get<std::string>() ).second
            || !previousByFile[item["file"].get<std::string>()]
                        .emplace( item["uuid"].get<std::string>(), item ).second )
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
    }

    if( aOperation["managedItems"].size() != desiredLogicalIds.size() )
    {
        diagnostic( result, "invalid_schematic_plan",
                    "schematic file items and ownership inventory are inconsistent" );
        return result;
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
                    + desiredEntry->second["items"].size();
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

            const std::string uuid = directUuid( *document, child );

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
