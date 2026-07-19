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

#include <build_version.h>

#include <wx/dir.h>
#include <wx/filename.h>


namespace KICHAD::CODEX_TOOLS
{

nlohmann::json ProjectSpec()
{
    nlohmann::json schema = { { "type", "object" },
                              { "additionalProperties", false },
                              { "required", nlohmann::json::array( { "operation" } ) } };
    schema["properties"]["operation"] =
            { { "type", "string" }, { "enum", nlohmann::json::array( { "context" } ) } };

    return { { "type", "function" },
             { "name", "project" },
             { "description",
               "Read the active KiChad project context. Use operation 'context' to discover "
               "the stable KiCad version, design files, and whether a turn snapshot allows "
               "mutation." },
             { "inputSchema", std::move( schema ) } };
}

} // namespace KICHAD::CODEX_TOOLS


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleProject(
        const JSON& aArguments, const wxString& aProjectPath, bool aMutationAvailable ) const
{
    if( !aArguments.is_object() || !aArguments.contains( "operation" )
        || !aArguments["operation"].is_string()
        || aArguments["operation"].get<std::string>() != "context" )
    {
        return failure( "invalid_arguments", "project.operation must be 'context'" );
    }

    wxString path = aProjectPath;

    if( !wxFileName::DirExists( path ) )
        return failure( "project_unavailable", "No readable project directory is active" );

    JSON files = JSON::array();
    wxDir directory( path );
    wxString name;
    bool found = directory.GetFirst( &name, wxS( "*.kicad_*" ), wxDIR_FILES );

    while( found )
    {
        wxFileName file( path, name );
        wxULongLong size = file.GetSize();
        JSON item = { { "name", std::string( name.ToUTF8() ) } };

        if( size != wxInvalidSize )
            item["bytes"] = size.GetValue();

        files.emplace_back( std::move( item ) );
        found = directory.GetNext( &name );
    }

    JSON payload = {
        { "operation", "context" },
        { "projectPath", std::string( path.ToUTF8() ) },
        { "kicadVersion", std::string( GetBuildVersion().ToUTF8() ) },
        { "files", std::move( files ) },
        { "mutationAvailable", aMutationAvailable }
    };

    return success( payload );
}
