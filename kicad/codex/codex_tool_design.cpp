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

#include "codex_tool_registry.h"

#include "codex_tool_internal.h"
#include "design_script_compiler.h"
#include "design_script_pcb_planner.h"
#include "design_script_pcb_reconciler.h"
#include "design_script_schematic_planner.h"
#include "design_script_schematic_reconciler.h"
#include "design_script_symbol_resolver.h"
#include "kicad_ipc_client.h"
#include "kicad_ipc_transaction.h"

#include <kiid.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <api/board/board.pb.h>
#include <api/common/types/project_settings.pb.h>
#include <google/protobuf/util/json_util.h>
#include <picosha2.h>
#include <wx/base64.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/utils.h>


namespace
{

using JSON = nlohmann::json;

constexpr size_t MAX_DESIGN_SCRIPT_BYTES = 1024 * 1024;
constexpr size_t MAX_DESIGN_STATE_BYTES = 64 * 1024 * 1024;
constexpr size_t MAX_SCHEMATIC_INVENTORY_BYTES = 32 * 1024 * 1024;


struct PROJECT_LIBRARY_TABLE_UPDATE
{
    std::string kind;
    wxFileName  path;
    std::string source;
    size_t      rows = 0;
    bool        previousPresent = false;
    std::string previousSource;
    bool        applied = false;
};


struct SCHEMATIC_FILE_UPDATE
{
    std::string relativePath;
    wxFileName  path;
    std::string source;
    bool        previousPresent = false;
    std::string previousSource;
    bool        applied = false;
};

} // namespace


namespace KICHAD::CODEX_TOOLS
{

nlohmann::json DesignSpec()
{
    nlohmann::json schema = { { "type", "object" },
                              { "additionalProperties", false },
                              { "required", nlohmann::json::array( { "operation" } ) } };
    schema["properties"]["operation"] =
            { { "type", "string" },
              { "enum", nlohmann::json::array(
                                { "describe", "read", "compile", "preview", "save", "apply" } ) } };
    schema["properties"]["path"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative reusable .kicad_kds sidecar path." } };
    schema["properties"]["source"] =
            { { "type", "string" }, { "maxLength", MAX_DESIGN_SCRIPT_BYTES },
              { "description", "Inline KiChad Design Script s-expression source." } };
    schema["properties"]["boardPath"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative .kicad_pcb target required by apply." } };
    schema["properties"]["expectedSha256"] =
            { { "type", "string" }, { "minLength", 64 }, { "maxLength", 64 },
              { "description",
                "Required for stale-write protection and to apply the exact compiled revision." } };

    return { { "type", "function" },
             { "name", "design" },
             { "description",
               "Describe, read, compile, preview, atomically save, or transactionally apply a "
               "reusable KiChad Design Script sidecar. KDS programs declare the complete "
               "project—schematic, libraries, PCB intent, sourcing, verification, and "
               "fabrication outputs—for deterministic execution by KiChad compiler backends." },
             { "inputSchema", std::move( schema ) } };
}

} // namespace KICHAD::CODEX_TOOLS


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleDesign(
        const JSON& aArguments, const wxString& aProjectPath, bool aMutationAvailable,
        const wxString& aIpcSocketDirectory ) const
{
    if( !aArguments.is_object() || !aArguments.contains( "operation" )
        || !aArguments["operation"].is_string() )
    {
        return failure( "invalid_arguments", "design.operation must be a string" );
    }

    const std::string operation = aArguments["operation"].get<std::string>();

    if( operation != "describe" && operation != "read" && operation != "compile"
        && operation != "preview" && operation != "save" && operation != "apply" )
    {
        return failure( "invalid_arguments",
                        "design.operation must be 'describe', 'read', 'compile', 'preview', "
                        "'save', or 'apply'" );
    }

    if( operation == "describe" )
        return success( KICHAD::DESIGN_SCRIPT_COMPILER::Describe() );

    const bool hasSource = aArguments.contains( "source" ) && aArguments["source"].is_string();
    const bool hasPath = aArguments.contains( "path" ) && aArguments["path"].is_string();

    if( ( ( operation == "compile" || operation == "preview" ) && hasSource == hasPath )
        || ( operation == "save" && ( !hasSource || !hasPath ) )
        || ( ( operation == "read" || operation == "apply" ) && ( hasSource || !hasPath ) ) )
    {
        std::string message;

        if( operation == "read" || operation == "apply" )
            message = "design." + operation + " requires path and does not accept inline source";
        else if( operation == "save" )
            message = "design.save requires source and path";
        else
            message = "design.compile and design.preview require exactly one of source or path";

        return failure( "invalid_arguments", message );
    }

    if( ( aArguments.contains( "source" ) && !aArguments["source"].is_string() )
        || ( aArguments.contains( "path" ) && !aArguments["path"].is_string() )
        || ( aArguments.contains( "boardPath" ) && !aArguments["boardPath"].is_string() ) )
    {
        return failure( "invalid_arguments",
                        "design source, path, or boardPath has the wrong type" );
    }

    std::string source;
    wxFileName  sidecar;
    std::string pathError;

    if( hasPath )
    {
        const std::string relativePath = aArguments["path"].get<std::string>();

        if( !KICHAD::CODEX_TOOLS::ResolveProjectSidecar( aProjectPath, relativePath, sidecar, pathError ) )
            return failure( "invalid_path", pathError );

        if( ( operation == "read" || operation == "compile" || operation == "preview"
              || operation == "apply" )
            && !sidecar.FileExists() )
            return failure( "read_failed", "KiChad Design Script sidecar does not exist" );
    }

    if( hasSource )
        source = aArguments["source"].get<std::string>();
    else if( !KICHAD::CODEX_TOOLS::ReadDesignScriptSidecar( sidecar, source, pathError ) )
        return failure( "read_failed", pathError );

    if( source.empty() || source.size() > MAX_DESIGN_SCRIPT_BYTES
        || source.find( '\0' ) != std::string::npos )
    {
        return failure( "invalid_source",
                        "KiChad Design Script source must be UTF-8 text containing 1 byte to 1 MiB" );
    }

    KICHAD::DESIGN_SCRIPT_COMPILER::RESULT compiled =
            KICHAD::DESIGN_SCRIPT_COMPILER::Compile( source );

    if( operation == "read" )
    {
        const bool invalidUtf8 = std::any_of(
                compiled.diagnostics.begin(), compiled.diagnostics.end(),
                []( const JSON& aDiagnostic )
                {
                    return aDiagnostic.value( "code", "" ) == "invalid_encoding";
                } );

        if( invalidUtf8 )
            return failure( "invalid_source", "KiChad Design Script source must be valid UTF-8" );

        return success( { { "operation", "read" },
                          { "path", aArguments["path"] },
                          { "source", source },
                          { "bytes", source.size() },
                          { "sourceSha256", compiled.sourceSha256 },
                          { "valid", compiled.ok },
                          { "diagnostics", compiled.diagnostics } } );
    }

    if( operation == "compile" )
    {
        JSON payload = { { "operation", "compile" },
                         { "valid", compiled.ok },
                         { "sourceSha256", compiled.sourceSha256 },
                         { "plan", compiled.plan },
                         { "diagnostics", compiled.diagnostics } };

        if( hasPath )
            payload["path"] = aArguments["path"];

        return success( payload );
    }

    if( operation == "preview" )
    {
        if( !compiled.ok )
        {
            std::string message = "KiChad Design Script did not pass compilation";

            if( !compiled.diagnostics.empty() )
                message += ": " + compiled.diagnostics.front().value( "message", "invalid program" );

            return failure( "compile_failed", message );
        }

        KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT planned =
                KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
        JSON symbolLibrarySources;
        JSON footprintSources;

        if( !KICHAD::CODEX_TOOLS::InventoryProjectSymbolLibraries( aProjectPath, compiled.ir,
                                              symbolLibrarySources, pathError ) )
        {
            return failure( "symbol_inventory_failed", pathError );
        }

        if( !KICHAD::CODEX_TOOLS::InventoryProjectFootprints( aProjectPath, compiled.ir,
                                         footprintSources, pathError ) )
        {
            return failure( "footprint_inventory_failed", pathError );
        }

        KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::RESULT resolvedSymbols =
                KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::Resolve(
                        compiled.ir, symbolLibrarySources );
        KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT schematicPlanned;

        if( resolvedSymbols.ok )
        {
            schematicPlanned = KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                    compiled.ir, JSON::object(), resolvedSymbols.symbols );
        }
        else
        {
            schematicPlanned.counts = resolvedSymbols.counts;
            schematicPlanned.diagnostics = resolvedSymbols.diagnostics;
        }
        JSON items = JSON::array();

        for( const JSON& plannedOperation : planned.operations )
        {
            if( items.size() == 500 )
                break;

            const std::string action = plannedOperation.value( "action", "" );

            if( action == "upsert" )
            {
                items.push_back( { { "action", "manage" },
                                   { "logicalId", plannedOperation["logicalId"] },
                                   { "itemType", plannedOperation["itemType"] },
                                   { "targetId", plannedOperation["itemId"] } } );
            }
            else if( action == "place_by_reference" )
            {
                items.push_back( { { "action", "place" },
                                   { "component", plannedOperation["component"] } } );
            }
            else if( action == "update_stackup" )
            {
                items.push_back( { { "action", "configure_stackup" },
                                   { "physicalLayers",
                                     plannedOperation["stackup"]["layers"].size() } } );
            }
            else if( action == "update_title_block" )
            {
                items.push_back( { { "action", "configure_title_block" } } );
            }
            else if( action == "update_rules" )
            {
                items.push_back( { { "action", "configure_global_board_rules" } } );
            }
            else if( action == "update_net_classes" )
            {
                items.push_back(
                        { { "action", "configure_net_classes" },
                          { "classes", plannedOperation["settings"]["netClasses"].size() },
                          { "assignments",
                            plannedOperation["settings"]["assignments"].size() } } );
            }
            else if( action == "update_project_library_table" )
            {
                items.push_back(
                        { { "action", "configure_project_library_table" },
                          { "kind", plannedOperation["kind"] },
                          { "path", plannedOperation["path"] },
                          { "libraries", plannedOperation["entries"] } } );
            }
            else if( action == "update_custom_rules" )
            {
                items.push_back(
                        { { "action", "configure_custom_board_rules" },
                          { "rules", planned.counts.value( "customRules", 0 ) } } );
            }
            else
            {
                items.push_back( { { "action", "unsupported" },
                                   { "statementKind", plannedOperation["statementKind"] },
                                   { "reason", plannedOperation["reason"] } } );
            }
        }

        JSON boardPlan = { { "fullyLowered", planned.fullyLowered },
                           { "counts", std::move( planned.counts ) },
                           { "diagnostics", std::move( planned.diagnostics ) },
                           { "items", std::move( items ) },
                           { "itemsTruncated", planned.operations.size() > 500 } };

        JSON schematicFiles = JSON::array();

        if( !schematicPlanned.operations.empty() )
        {
            for( const JSON& file : schematicPlanned.operations[0]["files"] )
            {
                schematicFiles.push_back( { { "path", file["path"] },
                                            { "sheetId", file["sheetId"] },
                                            { "page", file["page"] },
                                            { "root", file["root"] },
                                            { "managedItems",
                                              file["items"].size()
                                                      + file["libSymbols"].size()
                                                      + file["busAliases"].size() } } );
            }
        }

        JSON schematicPlan = { { "fullyLowered", schematicPlanned.fullyLowered },
                               { "counts", std::move( schematicPlanned.counts ) },
                               { "diagnostics", std::move( schematicPlanned.diagnostics ) },
                               { "files", std::move( schematicFiles ) } };

        JSON payload = { { "operation", "preview" },
                         { "valid", true },
                         { "sourceSha256", compiled.sourceSha256 },
                         { "compilerPlan", std::move( compiled.plan ) },
                         { "boardPlan", std::move( boardPlan ) },
                         { "schematicPlan", std::move( schematicPlan ) } };

        if( hasPath )
            payload["path"] = aArguments["path"];

        return success( payload );
    }

    if( operation == "apply" )
    {
        if( !aMutationAvailable )
        {
            return failure( "snapshot_required",
                            "A complete pre-turn project snapshot is required to apply KDS" );
        }

        if( !compiled.ok )
        {
            std::string message = "KiChad Design Script did not pass compilation";

            if( !compiled.diagnostics.empty() )
                message += ": " + compiled.diagnostics.front().value( "message", "invalid program" );

            return failure( "compile_failed", message );
        }

        if( !aArguments.contains( "expectedSha256" )
            || !aArguments["expectedSha256"].is_string()
            || aArguments["expectedSha256"].get<std::string>() != compiled.sourceSha256 )
        {
            return failure( "stale_source",
                            "design.apply requires the exact compiled source SHA-256" );
        }

        if( !aArguments.contains( "boardPath" ) || !aArguments["boardPath"].is_string() )
            return failure( "invalid_arguments", "design.apply requires boardPath" );

        const std::string boardRelativePath = aArguments["boardPath"].get<std::string>();
        wxFileName       board;

        if( !KICHAD::CODEX_TOOLS::ResolveProjectFile( aProjectPath, boardRelativePath, board, pathError )
            || board.GetExt() != wxS( "kicad_pcb" ) )
        {
            if( pathError.empty() )
                pathError = "boardPath must identify a project .kicad_pcb file";

            return failure( "invalid_path", pathError );
        }

        KICHAD::DESIGN_SCRIPT_PCB_PLANNER::RESULT planned =
                KICHAD::DESIGN_SCRIPT_PCB_PLANNER::Plan( compiled.ir );
        JSON symbolLibrarySources;
        JSON footprintSources;

        if( !KICHAD::CODEX_TOOLS::InventoryProjectSymbolLibraries( aProjectPath, compiled.ir,
                                              symbolLibrarySources, pathError ) )
        {
            return failure( "symbol_inventory_failed", pathError );
        }

        if( !KICHAD::CODEX_TOOLS::InventoryProjectFootprints( aProjectPath, compiled.ir,
                                         footprintSources, pathError ) )
        {
            return failure( "footprint_inventory_failed", pathError );
        }

        KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::RESULT resolvedSymbols =
                KICHAD::DESIGN_SCRIPT_SYMBOL_RESOLVER::Resolve(
                        compiled.ir, symbolLibrarySources );

        if( !resolvedSymbols.ok )
        {
            return failure(
                    "backend_incomplete",
                    resolvedSymbols.diagnostics.empty()
                            ? "one or more schematic symbols could not be exactly resolved"
                            : resolvedSymbols.diagnostics.front().value(
                                      "message", "schematic symbol resolution failed" ) );
        }

        KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT schematicPreplanned =
                KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                        compiled.ir, JSON::object(), resolvedSymbols.symbols );

        if( !planned.fullyLowered || !schematicPreplanned.fullyLowered )
        {
            std::string message = !planned.fullyLowered
                                          ? "one or more board statements do not have an apply backend"
                                          : "one or more schematic statements do not have an apply backend";

            for( const JSON& action : planned.operations )
            {
                if( action.value( "action", "" ) == "unsupported" )
                {
                    message += ": " + action.value( "reason", "unsupported statement" );
                    break;
                }
            }

            if( !schematicPreplanned.fullyLowered
                && !schematicPreplanned.diagnostics.empty() )
            {
                message += ": " + schematicPreplanned.diagnostics.front().value(
                                           "message", "unsupported schematic statement" );
            }

            return failure( "backend_incomplete", message );
        }

        std::unique_ptr<kiapi::board::BoardStackup> desiredStackup;
        std::unique_ptr<kiapi::common::types::TitleBlockInfo> desiredTitleBlock;
        std::unique_ptr<kiapi::board::BoardDesignRules> desiredRules;
        std::unique_ptr<kiapi::common::project::NetClassSettings> desiredNetClasses;
        JSON desiredLibraryTables = JSON::array();
        std::set<std::string> desiredLibraryTableKinds;
        bool        hasDesiredCustomRules = false;
        bool        desiredCustomRulesPresent = false;
        std::string desiredCustomRulesSource;

        for( const JSON& action : planned.operations )
        {
            const std::string plannedAction = action.value( "action", "" );

            if( plannedAction != "update_stackup" && plannedAction != "update_title_block"
                && plannedAction != "update_rules"
                && plannedAction != "update_net_classes"
                && plannedAction != "update_custom_rules"
                && plannedAction != "update_project_library_table" )
                continue;

            if( plannedAction == "update_project_library_table" )
            {
                const std::string kind = action.value( "kind", "" );
                const std::string expectedPath = kind == "symbol" ? "sym-lib-table"
                                                   : kind == "footprint" ? "fp-lib-table"
                                                                          : "";

                if( expectedPath.empty() || !desiredLibraryTableKinds.emplace( kind ).second
                    || action.value( "path", "" ) != expectedPath
                    || !action.contains( "present" ) || !action["present"].is_boolean()
                    || !action["present"].get<bool>() || !action.contains( "source" )
                    || !action["source"].is_string() )
                {
                    return failure( "invalid_plan",
                                    "KDS produced an invalid project library table operation" );
                }

                size_t      rows = 0;
                std::string validationError;
                const std::string tableSource = action["source"].get<std::string>();

                if( !KICHAD::CODEX_TOOLS::ValidateProjectLibraryTable( kind, tableSource, rows, validationError )
                    || !action.contains( "entries" ) || !action["entries"].is_number_unsigned()
                    || action["entries"].get<size_t>() != rows )
                {
                    return failure( "invalid_plan",
                                    validationError.empty()
                                            ? "KDS project library table row count is inconsistent"
                                            : validationError );
                }

                desiredLibraryTables.push_back( { { "kind", kind },
                                                  { "path", expectedPath },
                                                  { "source", tableSource },
                                                  { "rows", rows } } );
                continue;
            }

            if( plannedAction == "update_custom_rules" )
            {
                if( hasDesiredCustomRules || !action.contains( "customRules" )
                    || !action["customRules"].is_object()
                    || !action["customRules"].contains( "present" )
                    || !action["customRules"]["present"].is_boolean()
                    || !action["customRules"].contains( "source" )
                    || !action["customRules"]["source"].is_string()
                    || action["customRules"]["source"].get_ref<const std::string&>().size()
                               > MAX_DESIGN_SCRIPT_BYTES )
                {
                    return failure( "invalid_plan",
                                    "KDS produced invalid native custom rules" );
                }

                hasDesiredCustomRules = true;
                desiredCustomRulesPresent = action["customRules"]["present"].get<bool>();
                desiredCustomRulesSource =
                        action["customRules"]["source"].get<std::string>();

                if( desiredCustomRulesPresent != !desiredCustomRulesSource.empty() )
                {
                    return failure( "invalid_plan",
                                    "KDS produced inconsistent native custom rules" );
                }

                continue;
            }

            google::protobuf::util::JsonParseOptions options;
            options.ignore_unknown_fields = false;
            google::protobuf::util::Status status;

            if( plannedAction == "update_stackup" )
            {
                if( desiredStackup )
                    return failure( "invalid_plan", "KDS planned more than one board stackup" );

                desiredStackup = std::make_unique<kiapi::board::BoardStackup>();
                status = google::protobuf::util::JsonStringToMessage(
                        action.at( "stackup" ).dump(), desiredStackup.get(), options );
            }
            else if( plannedAction == "update_title_block" )
            {
                if( desiredTitleBlock )
                    return failure( "invalid_plan", "KDS planned more than one title block" );

                desiredTitleBlock =
                        std::make_unique<kiapi::common::types::TitleBlockInfo>();
                status = google::protobuf::util::JsonStringToMessage(
                        action.at( "titleBlock" ).dump(), desiredTitleBlock.get(), options );
            }
            else if( plannedAction == "update_rules" )
            {
                if( desiredRules )
                    return failure( "invalid_plan", "KDS planned more than one global rule set" );

                desiredRules = std::make_unique<kiapi::board::BoardDesignRules>();
                status = google::protobuf::util::JsonStringToMessage(
                        action.at( "rules" ).dump(), desiredRules.get(), options );
            }
            else
            {
                if( desiredNetClasses )
                    return failure( "invalid_plan", "KDS planned more than one netclass table" );

                desiredNetClasses =
                        std::make_unique<kiapi::common::project::NetClassSettings>();
                status = google::protobuf::util::JsonStringToMessage(
                        action.at( "settings" ).dump(), desiredNetClasses.get(), options );
            }

            if( !status.ok() )
                return failure( "invalid_plan", "KDS produced invalid native design settings" );
        }

        if( !desiredLibraryTables.empty() && desiredLibraryTables.size() != 2 )
        {
            return failure( "invalid_plan",
                            "KDS must replace symbol and footprint project tables together" );
        }

        const std::string sourceRelativePath = aArguments["path"].get<std::string>();
        const std::string projectName = compiled.ir["project"]["name"].get<std::string>();
        KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::CONTEXT reconcileContext = {
            sourceRelativePath, boardRelativePath, projectName, compiled.sourceSha256
        };
        wxFileName statePath = sidecar;
        statePath.SetExt( wxS( "kicad_kds_state" ) );
        wxFileName journalPath = sidecar;
        journalPath.SetExt( wxS( "kicad_kds_journal" ) );
        JSON previousState;
        JSON journal;

        if( !KICHAD::CODEX_TOOLS::ReadJsonFile( statePath, previousState, pathError )
            || !KICHAD::CODEX_TOOLS::ReadJsonFile( journalPath, journal, pathError )
            || !KICHAD::CODEX_TOOLS::MergeRecoveryJournal( journal, reconcileContext, previousState, pathError ) )
        {
            return failure( "invalid_managed_state", pathError );
        }

        JSON previousSchematicItems = JSON::array();

        if( previousState.is_object() && previousState.contains( "managedSchematicItems" ) )
        {
            if( !previousState["managedSchematicItems"].is_array() )
            {
                return failure( "invalid_managed_state",
                                "managed schematic ownership must be an array" );
            }

            previousSchematicItems = previousState["managedSchematicItems"];
        }

        std::map<std::string, JSON> preliminarySchematicFiles;

        if( !schematicPreplanned.operations.empty() )
        {
            if( schematicPreplanned.operations.size() != 1
                || schematicPreplanned.operations[0].value( "action", "" )
                           != "reconcile_schematic_hierarchy" )
            {
                return failure( "invalid_plan", "KDS produced an invalid schematic operation" );
            }

            for( const JSON& file : schematicPreplanned.operations[0]["files"] )
                preliminarySchematicFiles.emplace( file["path"].get<std::string>(), file );
        }

        std::set<std::string> schematicInventoryPaths;

        for( const auto& entry : preliminarySchematicFiles )
            schematicInventoryPaths.emplace( entry.first );

        for( const JSON& item : previousSchematicItems )
        {
            if( !item.is_object() || !item.contains( "file" ) || !item["file"].is_string() )
            {
                return failure( "invalid_managed_state",
                                "managed schematic ownership contains an invalid file path" );
            }

            schematicInventoryPaths.emplace( item["file"].get<std::string>() );
        }

        JSON liveSchematicFiles = JSON::array();
        JSON existingScreenUuids = JSON::object();
        std::map<std::string, wxFileName> resolvedSchematicPaths;
        size_t schematicInventoryBytes = 0;

        for( const std::string& relativePath : schematicInventoryPaths )
        {
            wxFileName resolved;
            bool present = false;
            std::string nativeSource;

            if( !KICHAD::CODEX_TOOLS::ResolveProjectSchematic( aProjectPath, relativePath, resolved, pathError )
                || !KICHAD::CODEX_TOOLS::ReadOptionalSchematic( resolved, present, nativeSource, pathError ) )
            {
                return failure( "schematic_inventory_failed", pathError );
            }

            if( nativeSource.size() > MAX_SCHEMATIC_INVENTORY_BYTES
                                             - schematicInventoryBytes )
            {
                return failure( "schematic_inventory_failed",
                                "managed schematic inventory exceeds 32 MiB" );
            }

            schematicInventoryBytes += nativeSource.size();

            resolvedSchematicPaths.emplace( relativePath, resolved );
            JSON live = { { "path", relativePath }, { "present", present } };

            if( present )
                live["source"] = nativeSource;

            liveSchematicFiles.emplace_back( std::move( live ) );

            if( present && preliminarySchematicFiles.contains( relativePath ) )
            {
                std::string screenUuid;

                if( !KICHAD::CODEX_TOOLS::SchematicScreenUuid( nativeSource, screenUuid, pathError ) )
                    return failure( "schematic_inventory_failed", pathError );

                existingScreenUuids[relativePath] = screenUuid;
            }
        }

        KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::RESULT schematicPlanned =
                KICHAD::DESIGN_SCRIPT_SCHEMATIC_PLANNER::Plan(
                        compiled.ir, existingScreenUuids, resolvedSymbols.symbols );

        if( !schematicPlanned.fullyLowered )
        {
            return failure(
                    "backend_incomplete",
                    schematicPlanned.diagnostics.empty()
                            ? "schematic hierarchy could not be lowered against live files"
                            : schematicPlanned.diagnostics.front().value(
                                      "message", "schematic hierarchy planning failed" ) );
        }

        JSON schematicOperation = {
            { "action", "reconcile_schematic_hierarchy" },
            { "project", projectName },
            { "rootFile", "" },
            { "files", JSON::array() },
            { "managedItems", JSON::array() }
        };

        if( !schematicPlanned.operations.empty() )
            schematicOperation = schematicPlanned.operations[0];

        KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::RESULT schematicReconciled =
                KICHAD::DESIGN_SCRIPT_SCHEMATIC_RECONCILER::Reconcile(
                        schematicOperation, previousSchematicItems, liveSchematicFiles );

        if( !schematicReconciled.ok )
        {
            return failure(
                    "schematic_reconcile_failed",
                    schematicReconciled.diagnostics.empty()
                            ? "managed schematic reconciliation failed"
                            : schematicReconciled.diagnostics.front().value(
                                      "message", "managed schematic reconciliation failed" ) );
        }

        std::vector<SCHEMATIC_FILE_UPDATE> schematicUpdates;

        for( const JSON& action : schematicReconciled.fileActions )
        {
            const std::string relativePath = action["path"].get<std::string>();
            auto resolved = resolvedSchematicPaths.find( relativePath );

            if( resolved == resolvedSchematicPaths.end() )
                return failure( "invalid_plan", "schematic action has no bounded inventory" );

            SCHEMATIC_FILE_UPDATE update;
            update.relativePath = relativePath;
            update.path = resolved->second;
            update.source = action["source"].get<std::string>();
            update.previousPresent = action["previousPresent"].get<bool>();
            update.previousSource = action["previousSource"].get<std::string>();
            schematicUpdates.emplace_back( std::move( update ) );
        }

        JSON managedOperations = JSON::array();
        JSON reconcileOperations = JSON::array();
        std::set<std::string> placementReferences;

        for( const JSON& plannedOperation : planned.operations )
        {
            const std::string action = plannedOperation.value( "action", "" );

            if( action == "upsert" )
            {
                managedOperations.push_back( plannedOperation );
                reconcileOperations.push_back( plannedOperation );
            }
            else if( action == "place_by_reference" )
            {
                placementReferences.emplace( plannedOperation["component"].get<std::string>() );
                reconcileOperations.push_back( plannedOperation );
            }
        }

        KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::RESULT preflight =
                KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::Reconcile(
                        managedOperations, previousState, JSON::array(), reconcileContext );

        if( !preflight.ok )
        {
            return failure( "reconcile_failed",
                            preflight.diagnostics.empty()
                                    ? "managed PCB state failed validation"
                                    : preflight.diagnostics.front().value(
                                              "message", "managed PCB state failed validation" ) );
        }

        std::set<std::string> relevantIds;

        for( const JSON& plannedOperation : managedOperations )
            relevantIds.emplace( plannedOperation["itemId"].get<std::string>() );

        for( const std::string& reference : placementReferences )
        {
            relevantIds.emplace( KICHAD::DESIGN_SCRIPT_PCB_PLANNER::StableUuid(
                    projectName, "footprint", reference ) );
        }

        if( !previousState.is_null() )
        {
            for( const JSON& item : previousState["managedPcbItems"] )
                relevantIds.emplace( item["itemId"].get<std::string>() );
        }

        std::vector<PROJECT_LIBRARY_TABLE_UPDATE> libraryTableUpdates;

        for( const JSON& desired : desiredLibraryTables )
        {
            PROJECT_LIBRARY_TABLE_UPDATE update;
            update.kind = desired["kind"].get<std::string>();
            update.source = desired["source"].get<std::string>();
            update.rows = desired["rows"].get<size_t>();

            if( !KICHAD::CODEX_TOOLS::ResolveProjectLibraryTable( aProjectPath,
                                             desired["path"].get<std::string>(),
                                             update.path, pathError )
                || !KICHAD::CODEX_TOOLS::ReadOptionalTextFile( update.path, update.previousPresent,
                                          update.previousSource, pathError ) )
            {
                return failure( "library_table_inventory_failed", pathError );
            }

            libraryTableUpdates.emplace_back( std::move( update ) );
        }

        KICHAD_IPC_CLIENT client( "org.kichad.codex.design", aIpcSocketDirectory );
        KICHAD_IPC_TARGET target;

        if( !client.FindOpenPcb( aProjectPath, board.GetFullPath(), target, pathError ) )
            return failure( "pcb_not_open", pathError );

        kiapi::board::BoardStackup previousStackup;
        kiapi::common::types::TitleBlockInfo previousTitleBlock;
        kiapi::board::BoardDesignRules previousRules;
        kiapi::common::project::NetClassSettings previousNetClasses;
        bool        previousCustomRulesPresent = false;
        std::string previousCustomRulesSource;

        if( desiredStackup && !KICHAD::CODEX_TOOLS::QueryPcbStackup( client, target, previousStackup, pathError ) )
            return failure( "stackup_inventory_failed", pathError );

        if( desiredTitleBlock
            && !KICHAD::CODEX_TOOLS::QueryPcbTitleBlock(
                    client, target, previousTitleBlock, pathError ) )
        {
            return failure( "title_block_inventory_failed", pathError );
        }

        if( desiredRules && !KICHAD::CODEX_TOOLS::QueryPcbRules( client, target, previousRules, pathError ) )
            return failure( "rules_inventory_failed", pathError );

        if( desiredNetClasses
            && !KICHAD::CODEX_TOOLS::QueryNetClassSettings( client, target, previousNetClasses, pathError ) )
        {
            return failure( "netclass_inventory_failed", pathError );
        }

        if( hasDesiredCustomRules
            && !KICHAD::CODEX_TOOLS::QueryPcbCustomRules( client, target, previousCustomRulesPresent,
                                     previousCustomRulesSource, pathError ) )
        {
            return failure( "custom_rules_inventory_failed", pathError );
        }

        JSON liveInventory;

        if( !KICHAD::CODEX_TOOLS::QueryPcbInventory( client, target, relevantIds, liveInventory, pathError ) )
            return failure( "inventory_failed", pathError );

        if( !KICHAD::CODEX_TOOLS::QueryPcbFootprintInventory( client, target, placementReferences,
                                         liveInventory, pathError ) )
        {
            return failure( "inventory_failed", pathError );
        }

        KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::RESULT reconciled =
                KICHAD::DESIGN_SCRIPT_PCB_RECONCILER::Reconcile(
                        reconcileOperations, previousState, liveInventory, reconcileContext );

        if( !reconciled.ok )
        {
            return failure( "reconcile_failed",
                            reconciled.diagnostics.empty()
                                    ? "managed PCB reconciliation failed"
                                    : reconciled.diagnostics.front().value(
                                              "message", "managed PCB reconciliation failed" ) );
        }

        reconciled.nextState["managedSchematicItems"] = schematicReconciled.managedItems;

        bool zoneMutation = false;
        std::set<std::string> expectedZoneIds;

        for( const JSON& managedOperation : managedOperations )
        {
            if( managedOperation.value( "itemType", "" ) == "zone" )
                expectedZoneIds.emplace( managedOperation["itemId"].get<std::string>() );
        }

        for( const JSON& action : reconciled.actions )
        {
            if( action.value( "itemType", "" ) == "zone" )
                zoneMutation = true;
        }

        JSON applyJournal = { { "format", "kichad-kds-apply-journal" },
                              { "version", 1 },
                              { "sourcePath", sourceRelativePath },
                              { "boardPath", boardRelativePath },
                              { "projectName", projectName },
                              { "sourceSha256", compiled.sourceSha256 },
                              { "previousState", previousState },
                              { "preparedState", reconciled.nextState } };

        if( desiredStackup )
        {
            std::string serializedPrevious;
            google::protobuf::util::JsonPrintOptions options;
            options.preserve_proto_field_names = false;
            options.always_print_primitive_fields = true;
            google::protobuf::util::Status status =
                    google::protobuf::util::MessageToJsonString(
                            previousStackup, &serializedPrevious, options );

            if( !status.ok() )
                return failure( "journal_failed", "could not serialize the prior board stackup" );

            applyJournal["previousStackup"] = JSON::parse( serializedPrevious );
        }

        if( desiredTitleBlock )
        {
            std::string serializedPrevious;
            google::protobuf::util::JsonPrintOptions options;
            options.preserve_proto_field_names = false;
            options.always_print_primitive_fields = true;
            google::protobuf::util::Status status =
                    google::protobuf::util::MessageToJsonString(
                            previousTitleBlock, &serializedPrevious, options );

            if( !status.ok() )
            {
                return failure( "journal_failed",
                                "could not serialize the prior board title block" );
            }

            applyJournal["previousTitleBlock"] = JSON::parse( serializedPrevious );
        }

        if( desiredRules )
        {
            std::string serializedPrevious;
            google::protobuf::util::JsonPrintOptions options;
            options.preserve_proto_field_names = false;
            options.always_print_primitive_fields = true;
            google::protobuf::util::Status status =
                    google::protobuf::util::MessageToJsonString(
                            previousRules, &serializedPrevious, options );

            if( !status.ok() )
                return failure( "journal_failed", "could not serialize the prior board rules" );

            applyJournal["previousRules"] = JSON::parse( serializedPrevious );
        }

        if( desiredNetClasses )
        {
            std::string serializedPrevious;
            google::protobuf::util::JsonPrintOptions options;
            options.preserve_proto_field_names = false;
            options.always_print_primitive_fields = true;
            google::protobuf::util::Status status =
                    google::protobuf::util::MessageToJsonString(
                            previousNetClasses, &serializedPrevious, options );

            if( !status.ok() )
            {
                return failure( "journal_failed",
                                "could not serialize the prior netclass settings" );
            }

            applyJournal["previousNetClassSettings"] = JSON::parse( serializedPrevious );
        }

        if( hasDesiredCustomRules )
        {
            applyJournal["previousCustomRules"] = {
                { "present", previousCustomRulesPresent },
                { "source", previousCustomRulesSource }
            };
        }

        if( !libraryTableUpdates.empty() )
        {
            applyJournal["previousProjectLibraryTables"] = JSON::array();

            for( const PROJECT_LIBRARY_TABLE_UPDATE& update : libraryTableUpdates )
            {
                applyJournal["previousProjectLibraryTables"].push_back(
                        { { "kind", update.kind },
                          { "path", update.path.GetFullName().ToStdString() },
                          { "present", update.previousPresent },
                          { "sourceBase64",
                            wxBase64Encode( update.previousSource.data(),
                                            update.previousSource.size() )
                                    .ToStdString() } } );
            }
        }

        if( !schematicUpdates.empty() )
        {
            applyJournal["previousSchematicFiles"] = JSON::array();

            for( const SCHEMATIC_FILE_UPDATE& update : schematicUpdates )
            {
                applyJournal["previousSchematicFiles"].push_back(
                        { { "path", update.relativePath },
                          { "present", update.previousPresent },
                          { "sourceBase64",
                            wxBase64Encode( update.previousSource.data(),
                                            update.previousSource.size() )
                                    .ToStdString() } } );
            }
        }

        if( !KICHAD::CODEX_TOOLS::WriteJsonAtomically( journalPath, applyJournal, pathError ) )
            return failure( "journal_failed", pathError );

        bool titleBlockApplied = false;

        if( desiredTitleBlock )
        {
            if( !KICHAD::CODEX_TOOLS::UpdatePcbTitleBlock(
                        client, target, *desiredTitleBlock, pathError ) )
            {
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::UpdatePcbTitleBlock(
                            client, target, previousTitleBlock, rollbackError ) )
                {
                    pathError += "; title-block rollback also failed: " + rollbackError;
                }

                return failure( "title_block_apply_failed",
                                pathError
                                        + "; the apply journal was retained for safe recovery" );
            }

            titleBlockApplied = true;
        }

        auto rollbackTitleBlock = [&]( std::string& aMessage )
        {
            if( !titleBlockApplied )
                return;

            std::string rollbackError;

            if( !KICHAD::CODEX_TOOLS::UpdatePcbTitleBlock(
                        client, target, previousTitleBlock, rollbackError ) )
            {
                aMessage += "; title-block rollback also failed: " + rollbackError;
            }
        };

        bool stackupApplied = false;

        if( desiredStackup )
        {
            if( !KICHAD::CODEX_TOOLS::UpdatePcbStackup( client, target, *desiredStackup, pathError ) )
            {
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::UpdatePcbStackup( client, target, previousStackup, rollbackError ) )
                    pathError += "; stackup rollback also failed: " + rollbackError;

                rollbackTitleBlock( pathError );

                return failure( "stackup_apply_failed",
                                pathError + "; the apply journal was retained for safe recovery" );
            }

            stackupApplied = true;
        }

        auto rollbackStackup = [&]( std::string& aMessage )
        {
            if( stackupApplied )
            {
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::UpdatePcbStackup(
                            client, target, previousStackup, rollbackError ) )
                {
                    aMessage += "; stackup rollback also failed: " + rollbackError;
                }
            }

            rollbackTitleBlock( aMessage );
        };

        bool rulesApplied = false;

        if( desiredRules )
        {
            if( !KICHAD::CODEX_TOOLS::UpdatePcbRules( client, target, *desiredRules, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::UpdatePcbRules( client, target, previousRules, rollbackError ) )
                    message += "; board-rules rollback also failed: " + rollbackError;

                rollbackStackup( message );
                return failure( "rules_apply_failed", message );
            }

            rulesApplied = true;
        }

        auto rollbackBoardSettings = [&]( std::string& aMessage )
        {
            if( rulesApplied )
            {
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::UpdatePcbRules( client, target, previousRules, rollbackError ) )
                    aMessage += "; board-rules rollback also failed: " + rollbackError;
            }

            rollbackStackup( aMessage );
        };

        bool netClassesApplied = false;

        if( desiredNetClasses )
        {
            if( !KICHAD::CODEX_TOOLS::UpdateNetClassSettings( client, target, *desiredNetClasses, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::UpdateNetClassSettings( client, target, previousNetClasses, rollbackError ) )
                    message += "; netclass rollback also failed: " + rollbackError;

                rollbackBoardSettings( message );
                return failure( "netclass_apply_failed", message );
            }

            netClassesApplied = true;
        }

        auto rollbackSettings = [&]( std::string& aMessage )
        {
            if( netClassesApplied )
            {
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::UpdateNetClassSettings(
                            client, target, previousNetClasses, rollbackError ) )
                {
                    aMessage += "; netclass rollback also failed: " + rollbackError;
                }
            }

            rollbackBoardSettings( aMessage );
        };

        bool customRulesApplied = false;

        if( hasDesiredCustomRules )
        {
            if( !KICHAD::CODEX_TOOLS::UpdatePcbCustomRules( client, target, desiredCustomRulesPresent,
                                       desiredCustomRulesSource, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::UpdatePcbCustomRules( client, target, previousCustomRulesPresent,
                                           previousCustomRulesSource, rollbackError ) )
                {
                    message += "; custom-rules rollback also failed: " + rollbackError;
                }

                rollbackSettings( message );
                return failure( "custom_rules_apply_failed", message );
            }

            customRulesApplied = true;
        }

        auto rollbackAllSettings = [&]( std::string& aMessage )
        {
            if( customRulesApplied )
            {
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::UpdatePcbCustomRules( client, target, previousCustomRulesPresent,
                                           previousCustomRulesSource, rollbackError ) )
                {
                    aMessage += "; custom-rules rollback also failed: " + rollbackError;
                }
            }

            rollbackSettings( aMessage );
        };

        size_t libraryTablesApplied = 0;

        for( PROJECT_LIBRARY_TABLE_UPDATE& update : libraryTableUpdates )
        {
            if( !KICHAD::CODEX_TOOLS::InstallTextFileAtomically( update.path, true, update.source, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::InstallTextFileAtomically( update.path, update.previousPresent,
                                                update.previousSource, rollbackError ) )
                {
                    message += "; current library-table rollback also failed: "
                               + rollbackError;
                }

                for( auto prior = libraryTableUpdates.rbegin();
                     prior != libraryTableUpdates.rend(); ++prior )
                {
                    if( !prior->applied )
                        continue;

                    rollbackError.clear();

                    if( !KICHAD::CODEX_TOOLS::InstallTextFileAtomically( prior->path, prior->previousPresent,
                                                    prior->previousSource, rollbackError ) )
                    {
                        message += "; library-table rollback also failed: " + rollbackError;
                    }
                }

                rollbackAllSettings( message );
                return failure( "library_table_apply_failed", message );
            }

            update.applied = true;
            ++libraryTablesApplied;
        }

        size_t schematicFilesApplied = 0;

        auto rollbackSchematicFiles = [&]( std::string& aMessage )
        {
            for( auto update = schematicUpdates.rbegin();
                 update != schematicUpdates.rend(); ++update )
            {
                if( !update->applied )
                    continue;

                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::InstallSchematicAtomically( update->path, update->previousPresent,
                                                 update->previousSource, rollbackError ) )
                {
                    aMessage += "; schematic rollback also failed: " + rollbackError;
                }
            }
        };

        for( SCHEMATIC_FILE_UPDATE& update : schematicUpdates )
        {
            if( !KICHAD::CODEX_TOOLS::InstallSchematicAtomically( update.path, true, update.source, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::InstallSchematicAtomically( update.path, update.previousPresent,
                                                 update.previousSource, rollbackError ) )
                {
                    message += "; current schematic rollback also failed: " + rollbackError;
                }

                rollbackSchematicFiles( message );

                for( auto library = libraryTableUpdates.rbegin();
                     library != libraryTableUpdates.rend(); ++library )
                {
                    if( !library->applied )
                        continue;

                    rollbackError.clear();

                    if( !KICHAD::CODEX_TOOLS::InstallTextFileAtomically( library->path, library->previousPresent,
                                                    library->previousSource, rollbackError ) )
                    {
                        message += "; library-table rollback also failed: " + rollbackError;
                    }
                }

                rollbackAllSettings( message );
                return failure( "schematic_apply_failed", message );
            }

            update.applied = true;
            ++schematicFilesApplied;
        }

        if( schematicFilesApplied > 0 )
        {
            pathError.clear();
            std::vector<wxFileName> validationRoots;
            const std::string rootRelativePath = schematicOperation.value( "rootFile", "" );

            if( !rootRelativePath.empty() )
            {
                auto root = resolvedSchematicPaths.find( rootRelativePath );

                if( root == resolvedSchematicPaths.end() )
                    pathError = "planned root schematic has no bounded path";
                else
                    validationRoots.emplace_back( root->second );
            }
            else
            {
                for( const SCHEMATIC_FILE_UPDATE& update : schematicUpdates )
                    validationRoots.emplace_back( update.path );
            }

            for( const wxFileName& validationRoot : validationRoots )
            {
                const bool nativeValid = pathError.empty()
                                         && ( m_schematicValidator
                                                      ? m_schematicValidator(
                                                                validationRoot, pathError )
                                                      : KICHAD::CODEX_TOOLS::ValidateNativeSchematicHierarchy(
                                                                validationRoot, pathError ) );

                if( !nativeValid )
                {
                    std::string message = pathError
                                          + "; the apply journal was retained for safe recovery";
                    rollbackSchematicFiles( message );

                    for( auto library = libraryTableUpdates.rbegin();
                         library != libraryTableUpdates.rend(); ++library )
                    {
                        if( !library->applied )
                            continue;

                        std::string rollbackError;

                        if( !KICHAD::CODEX_TOOLS::InstallTextFileAtomically(
                                    library->path, library->previousPresent,
                                    library->previousSource, rollbackError ) )
                        {
                            message += "; library-table rollback also failed: "
                                       + rollbackError;
                        }
                    }

                    rollbackAllSettings( message );
                    return failure( "schematic_validation_failed", message );
                }
            }
        }

        auto rollbackAllArtifacts = [&]( std::string& aMessage )
        {
            rollbackSchematicFiles( aMessage );

            for( auto update = libraryTableUpdates.rbegin();
                 update != libraryTableUpdates.rend(); ++update )
            {
                if( !update->applied )
                    continue;

                std::string rollbackError;

                if( !KICHAD::CODEX_TOOLS::InstallTextFileAtomically( update->path, update->previousPresent,
                                                update->previousSource, rollbackError ) )
                {
                    aMessage += "; library-table rollback also failed: " + rollbackError;
                }
            }

            rollbackAllSettings( aMessage );
        };

        if( reconciled.actions.empty() )
        {
            if( !KICHAD::CODEX_TOOLS::WriteJsonAtomically( statePath, reconciled.nextState, pathError ) )
            {
                std::string message = pathError
                                      + "; the apply journal was retained for safe recovery";
                rollbackAllArtifacts( message );
                return failure( "state_write_failed", message );
            }

            const bool journalRemoved = wxRemoveFile( journalPath.GetFullPath() );
            JSON payload = { { "operation", "apply" },
                             { "path", sourceRelativePath },
                             { "boardPath", boardRelativePath },
                             { "sourceSha256", compiled.sourceSha256 },
                             { "counts", reconciled.counts },
                             { "transaction",
                               titleBlockApplied || stackupApplied || rulesApplied
                                               || netClassesApplied
                                               || customRulesApplied || libraryTablesApplied > 0
                                               || schematicFilesApplied > 0
                                       ? "design settings applied"
                                       : "no board changes" },
                             { "titleBlockApplied", titleBlockApplied },
                             { "stackupApplied", stackupApplied },
                             { "rulesApplied", rulesApplied },
                             { "netClassesApplied", netClassesApplied },
                             { "customRulesApplied", customRulesApplied },
                             { "libraryTablesApplied", libraryTablesApplied },
                             { "schematicFilesApplied", schematicFilesApplied },
                             { "schematicCounts", schematicReconciled.counts },
                             { "journalRetained", !journalRemoved } };
            return success( payload );
        }

        KICHAD_IPC_COMMIT_GUARD commit( client, target );

        if( !commit.Begin( pathError ) )
        {
            std::string message = pathError
                                  + "; the apply journal was retained for safe recovery";
            rollbackAllArtifacts( message );
            return failure( "transaction_failed", message );
        }

        if( !KICHAD::CODEX_TOOLS::ExecutePcbActions( client, target, reconciled.actions,
                                footprintSources, pathError ) )
        {
            std::string dropError;
            const bool  dropped = commit.Drop( dropError );
            std::string message = pathError + "; the apply journal was retained for safe recovery";

            if( !dropped && !dropError.empty() )
                message += "; transaction drop also failed: " + dropError;

            rollbackAllArtifacts( message );

            return failure( "apply_failed", message );
        }

        if( !commit.Commit( "Apply KiChad Design Script " + sourceRelativePath, pathError ) )
        {
            std::string dropError;
            const bool  dropped = commit.Drop( dropError );
            std::string message = pathError + "; the apply journal was retained for safe recovery";

            if( !dropped && !dropError.empty() )
                message += "; transaction drop also failed: " + dropError;

            rollbackAllArtifacts( message );

            return failure( "transaction_failed", message );
        }

        if( zoneMutation && !KICHAD::CODEX_TOOLS::RefillPcbZones( client, target, expectedZoneIds, pathError ) )
        {
            return failure( "zone_refill_failed",
                            pathError + "; the committed board and retained journal can be "
                                        "reconciled safely on the next apply" );
        }

        if( !KICHAD::CODEX_TOOLS::WriteJsonAtomically( statePath, reconciled.nextState, pathError ) )
        {
            return failure( "state_write_failed",
                            pathError + "; the committed board and retained journal can be "
                                        "reconciled safely on the next apply" );
        }

        const bool journalRemoved = wxRemoveFile( journalPath.GetFullPath() );
        JSON payload = { { "operation", "apply" },
                         { "path", sourceRelativePath },
                         { "boardPath", boardRelativePath },
                         { "sourceSha256", compiled.sourceSha256 },
                         { "counts", reconciled.counts },
                         { "managedItems",
                           reconciled.nextState["managedPcbItems"].size() },
                         { "transaction", "committed" },
                         { "titleBlockApplied", titleBlockApplied },
                         { "stackupApplied", stackupApplied },
                         { "rulesApplied", rulesApplied },
                         { "netClassesApplied", netClassesApplied },
                         { "customRulesApplied", customRulesApplied },
                         { "libraryTablesApplied", libraryTablesApplied },
                         { "schematicFilesApplied", schematicFilesApplied },
                         { "schematicCounts", schematicReconciled.counts },
                         { "zonesRefilled", zoneMutation ? expectedZoneIds.size() : 0 },
                         { "statePath", statePath.GetFullName().ToStdString() },
                         { "journalRetained", !journalRemoved } };
        return success( payload );
    }

    if( !aMutationAvailable )
        return failure( "snapshot_required", "A pre-turn project snapshot is required to save KDS" );

    if( !compiled.ok )
    {
        std::string message = "KiChad Design Script did not pass compilation";

        if( !compiled.diagnostics.empty() )
            message += ": " + compiled.diagnostics.front().value( "message", "invalid program" );

        return failure( "compile_failed", message );
    }

    if( sidecar.FileExists() )
    {
        if( !aArguments.contains( "expectedSha256" )
            || !aArguments["expectedSha256"].is_string() )
        {
            return failure( "stale_source",
                            "expectedSha256 is required when replacing an existing sidecar" );
        }

        std::string existing;

        if( !KICHAD::CODEX_TOOLS::ReadDesignScriptSidecar( sidecar, existing, pathError ) )
            return failure( "read_failed", pathError );

        std::string existingSha256;
        picosha2::hash256_hex_string( existing, existingSha256 );

        if( aArguments["expectedSha256"].get<std::string>() != existingSha256 )
            return failure( "stale_source", "sidecar changed since it was loaded" );
    }

    const wxString temporaryPath =
            sidecar.GetFullPath() + wxS( ".tmp-" ) + KIID().AsString();
    wxFile temporary;

    if( !temporary.Create( temporaryPath, true )
        || temporary.Write( source.data(), source.size() ) != source.size()
        || !temporary.Flush() )
    {
        temporary.Close();
        wxRemoveFile( temporaryPath );
        return failure( "write_failed", "could not durably write the sidecar temporary file" );
    }

    temporary.Close();

    if( !wxRenameFile( temporaryPath, sidecar.GetFullPath(), true ) )
    {
        wxRemoveFile( temporaryPath );
        return failure( "write_failed", "could not atomically install the sidecar" );
    }

    std::string installed;

    if( !KICHAD::CODEX_TOOLS::ReadDesignScriptSidecar( sidecar, installed, pathError ) || installed != source )
        return failure( "write_failed", "sidecar verification failed after atomic installation" );

    JSON payload = { { "operation", "save" },
                     { "path", aArguments["path"] },
                     { "bytes", source.size() },
                     { "sourceSha256", compiled.sourceSha256 },
                     { "valid", true },
                     { "plan", compiled.plan },
                     { "transaction", "snapshot-backed atomic save" } };
    return success( payload );
}
