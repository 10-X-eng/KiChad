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

#include <build_version.h>

#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/utils.h>


CODEX_TOOL_REGISTRY::CODEX_TOOL_REGISTRY( std::function<wxString()> aProjectPathProvider,
                                          std::function<bool()> aMutationGuard ) :
        m_projectPathProvider( std::move( aProjectPathProvider ) ),
        m_mutationGuard( std::move( aMutationGuard ) )
{}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::Specs() const
{
    return JSON::array(
            { { { "type", "function" },
                { "name", "project" },
                { "description",
                  "Read the active KiChad project context. This tool is currently read-only; use "
                  "operation 'context' to discover the stable KiCad version and design files." },
                { "inputSchema",
                  { { "type", "object" },
                    { "additionalProperties", false },
                    { "required", JSON::array( { "operation" } ) },
                    { "properties",
                      { { "operation",
                          { { "type", "string" }, { "enum", JSON::array( { "context" } ) } } } } } } } } } );
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::Handle( const std::string& aTool,
                                                        const JSON& aArguments ) const
{
    if( aTool == "project" )
        return handleProject( aArguments );

    return failure( "unknown_tool", "The requested tool is not advertised by KiChad" );
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::handleProject( const JSON& aArguments ) const
{
    if( !aArguments.is_object() || aArguments.value( "operation", "" ) != "context" )
        return failure( "invalid_arguments", "project.operation must be 'context'" );

    wxString path = projectPath();

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
        { "mutationAvailable", m_mutationGuard && m_mutationGuard() }
    };

    return success( payload );
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::success( const JSON& aPayload ) const
{
    JSON envelope = { { "ok", true }, { "data", aPayload } };
    return { { "contentItems", JSON::array( { { { "type", "inputText" },
                                                { "text", envelope.dump() } } } ) },
             { "success", true } };
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::failure( const std::string& aCode,
                                                        const std::string& aMessage ) const
{
    JSON envelope = { { "ok", false },
                      { "error", { { "code", aCode }, { "message", aMessage } } } };
    return { { "contentItems", JSON::array( { { { "type", "inputText" },
                                                { "text", envelope.dump() } } } ) },
             { "success", false } };
}


wxString CODEX_TOOL_REGISTRY::projectPath() const
{
    wxString path = m_projectPathProvider ? m_projectPathProvider() : wxString();

    return path;
}
