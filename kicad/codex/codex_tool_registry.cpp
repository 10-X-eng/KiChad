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

#include <exception>
#include <utility>


CODEX_TOOL_REGISTRY::CODEX_TOOL_REGISTRY( std::function<wxString()> aProjectPathProvider,
                                          std::function<bool()> aMutationGuard,
                                          std::function<wxString()> aIpcSocketDirectoryProvider,
                                          std::function<bool( const wxFileName&, std::string& )>
                                                  aSchematicValidator,
                                          NATIVE_CHECK_RUNNER aNativeCheckRunner,
                                          NATIVE_FABRICATION_RUNNER aNativeFabricationRunner,
                                          std::function<bool( const wxFileName&, std::string& )>
                                                  aSymbolLibraryValidator ) :
        m_projectPathProvider( std::move( aProjectPathProvider ) ),
        m_mutationGuard( std::move( aMutationGuard ) ),
        m_ipcSocketDirectoryProvider( std::move( aIpcSocketDirectoryProvider ) ),
        m_schematicValidator( std::move( aSchematicValidator ) ),
        m_nativeCheckRunner( std::move( aNativeCheckRunner ) ),
        m_nativeFabricationRunner( std::move( aNativeFabricationRunner ) ),
        m_symbolLibraryValidator( std::move( aSymbolLibraryValidator ) )
{}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::Specs() const
{
    return JSON::array( { KICHAD::CODEX_TOOLS::ProjectSpec(),
                          KICHAD::CODEX_TOOLS::InspectSpec(),
                          KICHAD::CODEX_TOOLS::DesignSpec(),
                          KICHAD::CODEX_TOOLS::PcbSpec(),
                          KICHAD::CODEX_TOOLS::VerifySpec(),
                          KICHAD::CODEX_TOOLS::FabricateSpec() } );
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::Handle( const std::string& aTool,
                                                        const JSON& aArguments ) const
{
    wxString socketDirectory =
            m_ipcSocketDirectoryProvider ? m_ipcSocketDirectoryProvider() : wxString();
    return HandleWithContext( aTool, aArguments, projectPath(),
                              m_mutationGuard && m_mutationGuard(), socketDirectory );
}


CODEX_TOOL_REGISTRY::JSON CODEX_TOOL_REGISTRY::HandleWithContext(
        const std::string& aTool, const JSON& aArguments, const wxString& aProjectPath,
        bool aMutationAvailable, const wxString& aIpcSocketDirectory,
        bool aFinalActionApproved ) const
{
    try
    {
        if( aTool == "project" )
            return handleProject( aArguments, aProjectPath, aMutationAvailable );

        if( aTool == "inspect" )
            return handleInspect( aArguments, aProjectPath );

        if( aTool == "design" )
            return handleDesign( aArguments, aProjectPath, aMutationAvailable,
                                 aIpcSocketDirectory );

        if( aTool == "pcb" )
            return handlePcb( aArguments, aProjectPath, aMutationAvailable, aIpcSocketDirectory );

        if( aTool == "verify" )
            return handleVerify( aArguments, aProjectPath );

        if( aTool == "fabricate" )
        {
            return handleFabricate( aArguments, aProjectPath, aMutationAvailable,
                                    aFinalActionApproved );
        }

        return failure( "unknown_tool", "The requested tool is not advertised by KiChad" );
    }
    catch( const std::exception& error )
    {
        return failure( "tool_failed", error.what() );
    }
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
