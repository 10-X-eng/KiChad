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

#ifndef KICHAD_CODEX_TOOL_REGISTRY_H
#define KICHAD_CODEX_TOOL_REGISTRY_H

#include <functional>
#include <string>

#include <nlohmann/json.hpp>
#include <wx/filename.h>
#include <wx/string.h>


/** Registry for the bounded set of native dynamic tools advertised to Codex. */
class CODEX_TOOL_REGISTRY
{
public:
    using JSON = nlohmann::json;
    using NATIVE_CHECK_RUNNER = std::function<bool( const std::string&, const wxFileName&,
                                                     std::string&, std::string& )>;
    static constexpr int SCHEMA_VERSION = 6;

    explicit CODEX_TOOL_REGISTRY( std::function<wxString()> aProjectPathProvider,
                                  std::function<bool()> aMutationGuard = {},
                                  std::function<wxString()> aIpcSocketDirectoryProvider = {},
                                  std::function<bool( const wxFileName&, std::string& )>
                                          aSchematicValidator = {},
                                  NATIVE_CHECK_RUNNER aNativeCheckRunner = {} );

    JSON Specs() const;
    JSON Handle( const std::string& aTool, const JSON& aArguments ) const;
    JSON HandleWithContext( const std::string& aTool, const JSON& aArguments,
                            const wxString& aProjectPath, bool aMutationAvailable,
                            const wxString& aIpcSocketDirectory = wxString() ) const;

private:
    JSON handleProject( const JSON& aArguments, const wxString& aProjectPath,
                        bool aMutationAvailable ) const;
    JSON handleInspect( const JSON& aArguments, const wxString& aProjectPath ) const;
    JSON handleDesign( const JSON& aArguments, const wxString& aProjectPath,
                       bool aMutationAvailable, const wxString& aIpcSocketDirectory ) const;
    JSON handlePcb( const JSON& aArguments, const wxString& aProjectPath,
                    bool aMutationAvailable, const wxString& aIpcSocketDirectory ) const;
    JSON handleVerify( const JSON& aArguments, const wxString& aProjectPath ) const;
    JSON handleSourcingVerify( const JSON& aArguments, const wxString& aProjectPath ) const;
    JSON success( const JSON& aPayload ) const;
    JSON failure( const std::string& aCode, const std::string& aMessage ) const;
    wxString projectPath() const;

    std::function<wxString()> m_projectPathProvider;
    std::function<bool()>     m_mutationGuard;
    std::function<wxString()> m_ipcSocketDirectoryProvider;
    std::function<bool( const wxFileName&, std::string& )> m_schematicValidator;
    NATIVE_CHECK_RUNNER m_nativeCheckRunner;
};

#endif // KICHAD_CODEX_TOOL_REGISTRY_H
