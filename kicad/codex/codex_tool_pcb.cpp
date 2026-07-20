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
#include "kicad_ipc_client.h"
#include "kicad_ipc_transaction.h"

#include <kiid.h>

#include <memory>
#include <string>
#include <vector>

#include <api/common/commands/editor_commands.pb.h>
#include <google/protobuf/util/field_mask_util.h>
#include <google/protobuf/util/json_util.h>
#include <wx/filename.h>
#include <wx/utils.h>


namespace
{

constexpr size_t MAX_PCB_ARGUMENT_BYTES = 1024 * 1024;
constexpr size_t MAX_PCB_RESULT_BYTES = 256 * 1024;

} // namespace


namespace KICHAD::CODEX_TOOLS
{

nlohmann::json PcbSpec()
{
    nlohmann::json schema = { { "type", "object" },
                              { "additionalProperties", false },
                              { "required", nlohmann::json::array( { "operation", "path" } ) } };
    schema["properties"]["operation"] =
            { { "type", "string" },
              { "enum", nlohmann::json::array( { "status", "describe", "get", "mutate" } ) } };
    schema["properties"]["path"] =
            { { "type", "string" }, { "maxLength", 4096 },
              { "description", "Project-relative .kicad_pcb path." } };
    schema["properties"]["itemType"] =
            { { "type", "string" },
              { "enum", nlohmann::json::array(
                                { "footprint", "trace", "via", "arc", "zone", "rule_area",
                                  "shape", "text", "textbox", "dimension" } ) },
              { "description", "KiCad 10 protobuf board item type." } };
    schema["properties"]["messagePath"] =
            { { "type", "string" }, { "maxLength", 512 },
              { "description",
                "Dot-separated message field path to expand with operation 'describe'." } };
    schema["properties"]["action"] =
            { { "type", "string" },
              { "enum", nlohmann::json::array( { "create", "update", "delete" } ) } };
    schema["properties"]["items"] =
            { { "type", "array" }, { "minItems", 1 }, { "maxItems", 200 },
              { "items", { { "type", "object" } } },
              { "description", "Items encoded with official protobuf JSON field names." } };
    schema["properties"]["ids"] =
            { { "type", "array" }, { "minItems", 1 }, { "maxItems", 500 },
              { "items", { { "type", "string" }, { "maxLength", 36 } } } };
    schema["properties"]["fieldMask"] =
            { { "type", "array" }, { "maxItems", 32 },
              { "items", { { "type", "string" }, { "maxLength", 128 } } },
              { "description",
                "Protobuf protoName field paths; required for update. UUID fields are immutable." } };
    schema["properties"]["limit"] =
            { { "type", "integer" }, { "minimum", 1 }, { "maximum", 200 } };
    schema["properties"]["commitMessage"] =
            { { "type", "string" }, { "maxLength", 256 } };

    return { { "type", "function" },
             { "name", "pcb" },
             { "description",
               "Use the supported KiCad 10 protobuf IPC API for the live PCB Editor. Use "
               "'status' to verify the editor, 'describe' to discover exact typed fields, "
               "'get' to read live items, or 'mutate' to create, field-mask update, or delete "
               "items atomically. Coordinates and distances in protobuf JSON are nanometers." },
             { "inputSchema", std::move( schema ) } };
}

} // namespace KICHAD::CODEX_TOOLS


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handlePcb(
        const JSON& aArguments, const wxString& aProjectPath, bool aMutationAvailable,
        const wxString& aIpcSocketDirectory ) const
{
    if( !aArguments.is_object() || !aArguments.contains( "operation" )
        || !aArguments["operation"].is_string() || !aArguments.contains( "path" )
        || !aArguments["path"].is_string() )
    {
        return failure( "invalid_arguments", "pcb.operation and pcb.path must be strings" );
    }

    if( aArguments.dump().size() > MAX_PCB_ARGUMENT_BYTES )
        return failure( "invalid_arguments", "pcb arguments are limited to 1 MiB" );

    const std::string operation = aArguments["operation"].get<std::string>();

    if( operation != "status" && operation != "describe" && operation != "get"
        && operation != "mutate" )
    {
        return failure( "invalid_arguments",
                        "pcb.operation must be 'status', 'describe', 'get', or 'mutate'" );
    }

    const std::string relativePath = aArguments["path"].get<std::string>();
    wxFileName       resolved;
    std::string      pathError;

    if( !KICHAD::CODEX_TOOLS::ResolveProjectFile( aProjectPath, relativePath, resolved, pathError )
        || resolved.GetExt() != wxS( "kicad_pcb" ) )
    {
        if( pathError.empty() )
            pathError = "pcb.path must identify a project .kicad_pcb file";

        return failure( "invalid_path", pathError );
    }

    if( operation == "mutate" && !aMutationAvailable )
    {
        return failure( "snapshot_required",
                        "A complete pre-turn project snapshot is required before PCB mutation" );
    }

    JSON payload = { { "operation", operation },
                     { "path", relativePath },
                     { "mutationAvailable", aMutationAvailable },
                     { "transport", "KiCad 10 protobuf IPC" } };

    std::string itemType;
    kiapi::common::types::KiCadObjectType objectType =
            kiapi::common::types::KOT_UNKNOWN;

    if( operation != "status" )
    {
        if( !aArguments.contains( "itemType" ) || !aArguments["itemType"].is_string() )
            return failure( "invalid_arguments", "pcb.itemType must be a supported string" );

        itemType = aArguments["itemType"].get<std::string>();
        objectType = KICHAD::CODEX_TOOLS::PcbObjectType( itemType );

        if( objectType == kiapi::common::types::KOT_UNKNOWN )
            return failure( "invalid_arguments", "pcb.itemType is not supported" );
    }

    if( operation == "describe" )
    {
        std::string messagePath;

        if( aArguments.contains( "messagePath" ) )
        {
            if( !aArguments["messagePath"].is_string()
                || aArguments["messagePath"].get_ref<const std::string&>().size() > 512 )
            {
                return failure( "invalid_arguments",
                                "pcb.messagePath must be a string of at most 512 bytes" );
            }

            messagePath = aArguments["messagePath"].get<std::string>();
        }

        std::string descriptorError;
        JSON descriptor = KICHAD::CODEX_TOOLS::DescribePcbMessage( itemType, messagePath, descriptorError );

        if( !descriptorError.empty() )
            return failure( "invalid_arguments", descriptorError );

        payload["schema"] = std::move( descriptor );
        payload["editorRequired"] = false;
        return success( payload );
    }

    KICHAD_IPC_CLIENT client( "org.kichad.codex-" + std::to_string( wxGetProcessId() ),
                              aIpcSocketDirectory );
    KICHAD_IPC_TARGET target;
    std::string       ipcError;
    bool editorOpen = client.FindOpenPcb( aProjectPath, resolved.GetFullPath(), target, ipcError );
    payload["editorOpen"] = editorOpen;

    if( !editorOpen )
    {
        if( operation == "status" )
        {
            payload["detail"] = ipcError;
            return success( payload );
        }

        return failure( "pcb_editor_unavailable", ipcError );
    }

    payload["boardFilename"] = target.document.board_filename();
    payload["projectName"] = target.document.project().name();

    if( operation == "status" )
        return success( payload );

    std::vector<std::string> fieldMask;

    if( aArguments.contains( "fieldMask" ) )
    {
        if( !aArguments["fieldMask"].is_array() || aArguments["fieldMask"].size() > 32 )
            return failure( "invalid_arguments", "pcb.fieldMask must contain at most 32 paths" );

        for( const JSON& field : aArguments["fieldMask"] )
        {
            if( !field.is_string() || field.get_ref<const std::string&>().empty()
                || field.get_ref<const std::string&>().size() > 128 )
            {
                return failure( "invalid_arguments", "pcb.fieldMask contains an invalid path" );
            }

            fieldMask.emplace_back( field.get<std::string>() );
        }

        std::unique_ptr<google::protobuf::Message> prototype = KICHAD::CODEX_TOOLS::NewPcbItem( itemType );

        for( const std::string& field : fieldMask )
        {
            if( !google::protobuf::util::FieldMaskUtil::GetFieldDescriptors(
                        prototype->GetDescriptor(), field, nullptr ) )
            {
                return failure( "invalid_arguments",
                                "pcb.fieldMask contains a path that is invalid for " + itemType );
            }
        }
    }

    if( operation == "get" )
    {
        int limit = 50;

        if( aArguments.contains( "limit" ) )
        {
            if( !aArguments["limit"].is_number_integer() )
                return failure( "invalid_arguments", "pcb.limit must be an integer" );

            limit = aArguments["limit"].get<int>();

            if( limit < 1 || limit > 200 )
                return failure( "invalid_arguments", "pcb.limit must be between 1 and 200" );
        }

        kiapi::common::commands::GetItems request;
        request.mutable_header()->mutable_document()->CopyFrom( target.document );
        request.add_types( objectType );

        kiapi::common::ApiResponse response;

        if( !client.Call( target, request, response, ipcError ) )
            return failure( "ipc_failed", ipcError );

        kiapi::common::commands::GetItemsResponse itemsResponse;

        if( !response.message().UnpackTo( &itemsResponse )
            || itemsResponse.status() != kiapi::common::types::IRS_OK )
        {
            return failure( "ipc_failed", "KiCad returned an invalid get-items response" );
        }

        JSON   items = JSON::array();
        size_t resultBytes = 0;
        size_t totalItems = 0;
        bool   resultCapacityReached = false;

        for( const google::protobuf::Any& item : itemsResponse.items() )
        {
            if( ( itemType == "zone" || itemType == "rule_area" )
                && KICHAD::CODEX_TOOLS::PcbAnyType( item ) != itemType )
            {
                continue;
            }

            ++totalItems;

            if( resultCapacityReached || items.size() >= static_cast<size_t>( limit ) )
            {
                resultCapacityReached = true;
                continue;
            }

            std::unique_ptr<google::protobuf::Message> message = KICHAD::CODEX_TOOLS::NewPcbItem( itemType );

            if( !message || !item.UnpackTo( message.get() ) )
                return failure( "ipc_failed", "KiCad returned an unexpected PCB item type" );

            if( !fieldMask.empty() )
            {
                google::protobuf::FieldMask mask;

                for( const std::string& field : fieldMask )
                    mask.add_paths( field );

                mask.add_paths( "id" );
                google::protobuf::util::FieldMaskUtil::TrimMessage( mask, message.get() );
            }

            google::protobuf::util::JsonPrintOptions options;
            options.preserve_proto_field_names = false;
            std::string serialized;
            google::protobuf::util::Status status =
                    google::protobuf::util::MessageToJsonString( *message, &serialized, options );

            if( !status.ok() )
                return failure( "ipc_failed", status.ToString() );

            if( resultBytes + serialized.size() > MAX_PCB_RESULT_BYTES )
            {
                resultCapacityReached = true;
                continue;
            }

            resultBytes += serialized.size();
            items.emplace_back( JSON::parse( serialized ) );
        }

        payload["itemType"] = itemType;
        payload["totalItems"] = totalItems;
        payload["items"] = std::move( items );
        payload["resultTruncated"] = payload["items"].size() < totalItems;
        return success( payload );
    }

    if( !aArguments.contains( "action" ) || !aArguments["action"].is_string() )
        return failure( "invalid_arguments", "pcb.action must be create, update, or delete" );

    const std::string action = aArguments["action"].get<std::string>();

    if( action != "create" && action != "update" && action != "delete" )
        return failure( "invalid_arguments", "pcb.action must be create, update, or delete" );

    std::string commitMessage = "Codex " + action + " PCB " + itemType;

    if( aArguments.contains( "commitMessage" ) )
    {
        if( !aArguments["commitMessage"].is_string()
            || aArguments["commitMessage"].get_ref<const std::string&>().size() > 256 )
        {
            return failure( "invalid_arguments", "pcb.commitMessage must be at most 256 bytes" );
        }

        if( !aArguments["commitMessage"].get_ref<const std::string&>().empty() )
            commitMessage = aArguments["commitMessage"].get<std::string>();
    }

    KICHAD_IPC_COMMIT_GUARD commit( client, target );

    if( action == "delete" )
    {
        if( !aArguments.contains( "ids" ) || !aArguments["ids"].is_array()
            || aArguments["ids"].empty() || aArguments["ids"].size() > 500 )
        {
            return failure( "invalid_arguments", "pcb.ids must contain 1 to 500 UUIDs" );
        }

        kiapi::common::commands::DeleteItems request;
        request.mutable_header()->mutable_document()->CopyFrom( target.document );

        for( const JSON& id : aArguments["ids"] )
        {
            if( !id.is_string() || !KIID::SniffTest( wxString::FromUTF8( id.get<std::string>() ) ) )
                return failure( "invalid_arguments", "pcb.ids contains an invalid KiCad UUID" );

            request.add_item_ids()->set_value( id.get<std::string>() );
        }

        if( !commit.Begin( ipcError ) )
            return failure( "transaction_failed", ipcError );

        kiapi::common::ApiResponse response;

        if( !client.Call( target, request, response, ipcError ) )
            return failure( "ipc_failed", ipcError );

        kiapi::common::commands::DeleteItemsResponse deleted;

        if( !response.message().UnpackTo( &deleted )
            || deleted.status() != kiapi::common::types::IRS_OK
            || deleted.deleted_items_size() != request.item_ids_size() )
        {
            return failure( "ipc_failed", "KiCad rejected one or more PCB deletions" );
        }

        for( const kiapi::common::commands::ItemDeletionResult& item : deleted.deleted_items() )
        {
            if( item.status() != kiapi::common::commands::IDS_OK )
                return failure( "ipc_failed", "KiCad rejected one or more PCB deletions" );
        }

        if( !commit.Commit( commitMessage, ipcError ) )
            return failure( "transaction_failed", ipcError );

        payload["action"] = action;
        payload["itemType"] = itemType;
        payload["affectedItems"] = aArguments["ids"].size();
        payload["transaction"] = "committed";
        return success( payload );
    }

    if( !aArguments.contains( "items" ) || !aArguments["items"].is_array()
        || aArguments["items"].empty() || aArguments["items"].size() > 200 )
    {
        return failure( "invalid_arguments", "pcb.items must contain 1 to 200 objects" );
    }

    if( action == "update" && fieldMask.empty() )
        return failure( "invalid_arguments", "pcb.fieldMask is required for update" );

    if( action == "update" )
    {
        for( const std::string& field : fieldMask )
        {
            if( field == "id" || field.starts_with( "id." ) )
                return failure( "invalid_arguments", "pcb.fieldMask cannot update an item UUID" );
        }
    }

    if( action == "create" )
    {
        kiapi::common::commands::CreateItems request;
        request.mutable_header()->mutable_document()->CopyFrom( target.document );

        for( const JSON& item : aArguments["items"] )
        {
            std::unique_ptr<google::protobuf::Message> message;

            if( !KICHAD::CODEX_TOOLS::ParsePcbItem( itemType, item, message, ipcError ) )
                return failure( "invalid_arguments", ipcError );

            request.add_items()->PackFrom( *message );
        }

        if( !commit.Begin( ipcError ) )
            return failure( "transaction_failed", ipcError );

        kiapi::common::ApiResponse response;

        if( !client.Call( target, request, response, ipcError ) )
            return failure( "ipc_failed", ipcError );

        kiapi::common::commands::CreateItemsResponse created;

        if( !response.message().UnpackTo( &created )
            || created.status() != kiapi::common::types::IRS_OK
            || created.created_items_size() != request.items_size() )
        {
            return failure( "ipc_failed", "KiCad returned an invalid create-items response" );
        }

        JSON ids = JSON::array();

        for( const kiapi::common::commands::ItemCreationResult& item : created.created_items() )
        {
            if( item.status().code() != kiapi::common::commands::ISC_OK )
                return failure( "ipc_failed", item.status().error_message() );

            std::string id = KICHAD::CODEX_TOOLS::PcbItemId( item.item(), itemType );

            if( !id.empty() )
                ids.emplace_back( std::move( id ) );
        }

        if( !commit.Commit( commitMessage, ipcError ) )
            return failure( "transaction_failed", ipcError );

        payload["action"] = action;
        payload["itemType"] = itemType;
        payload["affectedItems"] = created.created_items_size();
        payload["itemIds"] = std::move( ids );
        payload["transaction"] = "committed";
        return success( payload );
    }

    kiapi::common::commands::UpdateItems request;
    request.mutable_header()->mutable_document()->CopyFrom( target.document );

    for( const std::string& field : fieldMask )
        request.mutable_header()->mutable_field_mask()->add_paths( field );

    for( const JSON& item : aArguments["items"] )
    {
        std::unique_ptr<google::protobuf::Message> message;

        if( !KICHAD::CODEX_TOOLS::ParsePcbItem( itemType, item, message, ipcError ) )
            return failure( "invalid_arguments", ipcError );

        request.add_items()->PackFrom( *message );
    }

    if( !commit.Begin( ipcError ) )
        return failure( "transaction_failed", ipcError );

    kiapi::common::ApiResponse response;

    if( !client.Call( target, request, response, ipcError ) )
        return failure( "ipc_failed", ipcError );

    kiapi::common::commands::UpdateItemsResponse updated;

    if( !response.message().UnpackTo( &updated )
        || updated.status() != kiapi::common::types::IRS_OK
        || updated.updated_items_size() != request.items_size() )
    {
        return failure( "ipc_failed", "KiCad returned an invalid update-items response" );
    }

    for( const kiapi::common::commands::ItemUpdateResult& item : updated.updated_items() )
    {
        if( item.status().code() != kiapi::common::commands::ISC_OK )
            return failure( "ipc_failed", item.status().error_message() );
    }

    if( !commit.Commit( commitMessage, ipcError ) )
        return failure( "transaction_failed", ipcError );

    payload["action"] = action;
    payload["itemType"] = itemType;
    payload["affectedItems"] = updated.updated_items_size();
    payload["transaction"] = "committed";
    return success( payload );
}
