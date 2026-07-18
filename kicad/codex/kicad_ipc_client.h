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

#ifndef KICHAD_KICAD_IPC_CLIENT_H
#define KICHAD_KICAD_IPC_CLIENT_H

#include <chrono>
#include <string>

#include <import_export.h>
#include <api/common/envelope.pb.h>
#include <api/common/types/base_types.pb.h>
#include <google/protobuf/message.h>
#include <wx/string.h>


struct KICHAD_IPC_TARGET
{
    std::string                              socketUrl;
    std::string                              kicadToken;
    kiapi::common::types::DocumentSpecifier document;
};


/** Worker-thread client for KiCad 10's supported protobuf IPC API. */
class KICHAD_IPC_CLIENT
{
public:
    explicit KICHAD_IPC_CLIENT(
            std::string aClientName = "org.kichad.codex",
            wxString aSocketDirectory = wxString(),
            std::chrono::milliseconds aTimeout = std::chrono::milliseconds( 2000 ) );

    bool FindOpenPcb( const wxString& aProjectPath, const wxString& aBoardPath,
                      KICHAD_IPC_TARGET& aTarget, std::string& aError ) const;

    bool Call( const KICHAD_IPC_TARGET& aTarget, const google::protobuf::Message& aRequest,
               kiapi::common::ApiResponse& aResponse, std::string& aError ) const;

    bool BeginCommit( const KICHAD_IPC_TARGET& aTarget, std::string& aCommitId,
                      std::string& aError ) const;
    bool EndCommit( const KICHAD_IPC_TARGET& aTarget, const std::string& aCommitId,
                    bool aCommit, const std::string& aMessage, std::string& aError ) const;

    static wxString DefaultSocketDirectory();

private:
    bool callSocket( const std::string& aSocketUrl, const std::string& aKicadToken,
                     const google::protobuf::Message& aRequest,
                     kiapi::common::ApiResponse& aResponse, std::string& aError,
                     std::chrono::milliseconds aTimeout ) const;

    std::string               m_clientName;
    wxString                  m_socketDirectory;
    std::chrono::milliseconds m_timeout;
};

#endif // KICHAD_KICAD_IPC_CLIENT_H
