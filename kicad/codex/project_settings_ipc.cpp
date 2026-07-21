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

#include "project_settings_ipc.h"

#include <api/common/commands/project_commands.pb.h>
#include <api/common/envelope.pb.h>
#include <api/common/types/enums.pb.h>
#include <google/protobuf/empty.pb.h>


namespace
{

kiapi::common::types::DocumentSpecifier projectDocument( const KICHAD_IPC_TARGET& aTarget )
{
    kiapi::common::types::DocumentSpecifier document;
    document.set_type( kiapi::common::types::DOCTYPE_PROJECT );
    document.mutable_project()->CopyFrom( aTarget.document.project() );
    return document;
}


bool sameTextVariables( const kiapi::common::project::TextVariables& aLeft,
                        const kiapi::common::project::TextVariables& aRight )
{
    if( aLeft.variables_size() != aRight.variables_size() )
        return false;

    for( const auto& [name, value] : aLeft.variables() )
    {
        auto found = aRight.variables().find( name );

        if( found == aRight.variables().end() || found->second != value )
            return false;
    }

    return true;
}


bool sameFieldTemplates( const kiapi::common::project::SchematicFieldTemplates& aLeft,
                         const kiapi::common::project::SchematicFieldTemplates& aRight )
{
    if( aLeft.fields_size() != aRight.fields_size() )
        return false;

    for( int index = 0; index < aLeft.fields_size(); ++index )
    {
        const kiapi::common::project::SchematicFieldTemplate& left = aLeft.fields( index );
        const kiapi::common::project::SchematicFieldTemplate& right = aRight.fields( index );

        if( left.name() != right.name() || left.visible() != right.visible()
            || left.url() != right.url() )
        {
            return false;
        }
    }

    return true;
}


bool sameRuleSeverities( const kiapi::common::project::SchematicRuleSeverities& aLeft,
                         const kiapi::common::project::SchematicRuleSeverities& aRight )
{
    if( aLeft.severities_size() != aRight.severities_size() )
        return false;

    for( const auto& [key, severity] : aLeft.severities() )
    {
        auto found = aRight.severities().find( key );

        if( found == aRight.severities().end() || found->second != severity )
            return false;
    }

    return true;
}

} // namespace


bool KICHAD::PROJECT_SETTINGS_IPC::QueryTextVariables(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        kiapi::common::project::TextVariables& aVariables, std::string& aError )
{
    kiapi::common::commands::GetTextVariables request;
    request.mutable_document()->CopyFrom( projectDocument( aTarget ) );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError )
        || !response.message().UnpackTo( &aVariables ) )
    {
        if( aError.empty() )
            aError = "KiCad returned invalid project text variables";

        return false;
    }

    return true;
}


bool KICHAD::PROJECT_SETTINGS_IPC::ReplaceTextVariables(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        const kiapi::common::project::TextVariables& aVariables, std::string& aError )
{
    kiapi::common::commands::SetTextVariables request;
    request.mutable_document()->CopyFrom( projectDocument( aTarget ) );
    request.mutable_variables()->CopyFrom( aVariables );
    request.set_merge_mode( kiapi::common::types::MMM_REPLACE );
    kiapi::common::ApiResponse response;
    google::protobuf::Empty empty;

    if( !aClient.Call( aTarget, request, response, aError )
        || !response.message().UnpackTo( &empty ) )
    {
        if( aError.empty() )
            aError = "KiCad returned invalid updated project text variables";

        return false;
    }

    kiapi::common::project::TextVariables active;

    if( !QueryTextVariables( aClient, aTarget, active, aError )
        || !sameTextVariables( active, aVariables ) )
    {
        if( aError.empty() )
            aError = "KiCad text-variable readback did not match the requested replacement";

        return false;
    }

    return true;
}


bool KICHAD::PROJECT_SETTINGS_IPC::QuerySchematicFieldTemplates(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        kiapi::common::project::SchematicFieldTemplates& aTemplates,
        std::string& aError )
{
    kiapi::common::commands::GetSchematicFieldTemplates request;
    request.mutable_document()->CopyFrom( projectDocument( aTarget ) );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError )
        || !response.message().UnpackTo( &aTemplates ) )
    {
        if( aError.empty() )
            aError = "KiCad returned invalid schematic field templates";

        return false;
    }

    return true;
}


bool KICHAD::PROJECT_SETTINGS_IPC::ReplaceSchematicFieldTemplates(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        const kiapi::common::project::SchematicFieldTemplates& aTemplates,
        std::string& aError )
{
    kiapi::common::commands::SetSchematicFieldTemplates request;
    request.mutable_document()->CopyFrom( projectDocument( aTarget ) );
    request.mutable_templates()->CopyFrom( aTemplates );
    kiapi::common::ApiResponse response;
    kiapi::common::project::SchematicFieldTemplates active;

    if( !aClient.Call( aTarget, request, response, aError )
        || !response.message().UnpackTo( &active )
        || !sameFieldTemplates( active, aTemplates ) )
    {
        if( aError.empty() )
            aError = "KiCad field-template readback did not match the requested replacement";

        return false;
    }

    return true;
}


bool KICHAD::PROJECT_SETTINGS_IPC::QuerySchematicRuleSeverities(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        kiapi::common::project::SchematicRuleSeverities& aSeverities,
        std::string& aError )
{
    kiapi::common::commands::GetSchematicRuleSeverities request;
    request.mutable_document()->CopyFrom( projectDocument( aTarget ) );
    kiapi::common::ApiResponse response;

    if( !aClient.Call( aTarget, request, response, aError )
        || !response.message().UnpackTo( &aSeverities ) )
    {
        if( aError.empty() )
            aError = "KiCad returned invalid schematic rule severities";

        return false;
    }

    return true;
}


bool KICHAD::PROJECT_SETTINGS_IPC::ReplaceSchematicRuleSeverities(
        const KICHAD_IPC_CLIENT& aClient, const KICHAD_IPC_TARGET& aTarget,
        const kiapi::common::project::SchematicRuleSeverities& aSeverities,
        std::string& aError )
{
    kiapi::common::commands::SetSchematicRuleSeverities request;
    request.mutable_document()->CopyFrom( projectDocument( aTarget ) );
    request.mutable_severities()->CopyFrom( aSeverities );
    kiapi::common::ApiResponse response;
    kiapi::common::project::SchematicRuleSeverities active;

    if( !aClient.Call( aTarget, request, response, aError )
        || !response.message().UnpackTo( &active )
        || !sameRuleSeverities( active, aSeverities ) )
    {
        if( aError.empty() )
            aError = "KiCad ERC-severity readback did not match the requested replacement";

        return false;
    }

    return true;
}
