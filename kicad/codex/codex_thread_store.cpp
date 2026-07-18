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

#include "codex_thread_store.h"
#include "codex_paths.h"

#include <nlohmann/json.hpp>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/intl.h>


std::string CODEX_THREAD_STORE::Load( const wxString& aProjectPath ) const
{
    wxFFile file( storagePath(), wxS( "rb" ) );

    if( !file.IsOpened() )
        return {};

    wxString contents;

    if( !file.ReadAll( &contents, wxConvUTF8 ) )
        return {};

    nlohmann::json document = nlohmann::json::parse( std::string( contents.ToUTF8() ), nullptr,
                                                     false );

    if( !document.is_object() || !document.contains( "projects" )
        || !document["projects"].is_object() )
    {
        return {};
    }

    return document["projects"].value( projectKey( aProjectPath ), "" );
}


bool CODEX_THREAD_STORE::Save( const wxString& aProjectPath, const std::string& aThreadId,
                               wxString* aError ) const
{
    const wxString path = storagePath();
    wxFileName target( path );

    if( !wxFileName::Mkdir( target.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL )
        && !wxFileName::DirExists( target.GetPath() ) )
    {
        if( aError )
            *aError = _( "Could not create the KiChad Codex settings directory." );

        return false;
    }

    nlohmann::json document = { { "version", 1 }, { "projects", nlohmann::json::object() } };
    wxFFile existing( path, wxS( "rb" ) );

    if( existing.IsOpened() )
    {
        wxString contents;

        if( existing.ReadAll( &contents, wxConvUTF8 ) )
        {
            nlohmann::json parsed = nlohmann::json::parse( std::string( contents.ToUTF8() ),
                                                           nullptr, false );

            if( parsed.is_object() && parsed.contains( "projects" )
                && parsed["projects"].is_object() )
            {
                document = std::move( parsed );
            }
        }
    }

    document["version"] = 1;
    document["projects"][projectKey( aProjectPath )] = aThreadId;

    wxString temporaryPath = path + wxS( ".tmp" );
    wxFFile temporary( temporaryPath, wxS( "wb" ) );

    if( !temporary.IsOpened()
        || !temporary.Write( wxString::FromUTF8( document.dump( 2 ) ), wxConvUTF8 )
        || !temporary.Flush() )
    {
        if( aError )
            *aError = _( "Could not write the KiChad Codex conversation index." );

        return false;
    }

    temporary.Close();

    if( !wxRenameFile( temporaryPath, path, true ) )
    {
        if( aError )
            *aError = _( "Could not atomically update the KiChad Codex conversation index." );

        return false;
    }

    return true;
}


wxString CODEX_THREAD_STORE::storagePath() const
{
    return KICHAD::CODEX_PATHS::ThreadIndex();
}


std::string CODEX_THREAD_STORE::projectKey( const wxString& aProjectPath ) const
{
    wxFileName path = wxFileName::DirName( aProjectPath );
    path.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    return std::string( path.GetFullPath().ToUTF8() );
}
