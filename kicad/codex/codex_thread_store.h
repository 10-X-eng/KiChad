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

#ifndef KICHAD_CODEX_THREAD_STORE_H
#define KICHAD_CODEX_THREAD_STORE_H

#include <string>
#include <utility>

#include <wx/string.h>


/** Persists app-server thread IDs and their native-tool schema keyed by project. */
class CODEX_THREAD_STORE
{
public:
    explicit CODEX_THREAD_STORE( wxString aStoragePath = wxString() ) :
            m_storagePath( std::move( aStoragePath ) )
    {}

    std::string Load( const wxString& aProjectPath, int aToolSchemaVersion ) const;
    bool Save( const wxString& aProjectPath, const std::string& aThreadId, int aToolSchemaVersion,
               wxString* aError = nullptr ) const;

private:
    wxString storagePath() const;
    std::string projectKey( const wxString& aProjectPath ) const;

    wxString m_storagePath;
};

#endif // KICHAD_CODEX_THREAD_STORE_H
