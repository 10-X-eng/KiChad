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


std::string CODEX_THREAD_STORE::Load( const wxString& aProjectPath,
                                      int aToolSchemaVersion ) const
{
    const wxString path = storagePath();

    if( !wxFileName::FileExists( path ) )
        return {};

    wxFFile file( path, wxS( "rb" ) );

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

    const std::string key = projectKey( aProjectPath );

    if( !document["projects"].contains( key ) || !document["projects"][key].is_object() )
        return {};

    const nlohmann::json& entry = document["projects"][key];

    if( !entry.contains( "threadId" ) || !entry["threadId"].is_string()
        || !entry.contains( "toolSchemaVersion" )
        || !entry["toolSchemaVersion"].is_number_integer()
        || entry["toolSchemaVersion"].get<int>() != aToolSchemaVersion )
    {
        return {};
    }

    return entry["threadId"].get<std::string>();
}


bool CODEX_THREAD_STORE::Save( const wxString& aProjectPath, const std::string& aThreadId,
                               int aToolSchemaVersion, wxString* aError ) const
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

    nlohmann::json document = { { "version", 2 }, { "projects", nlohmann::json::object() } };
    wxFFile existing;

    if( wxFileName::FileExists( path ) )
        existing.Open( path, wxS( "rb" ) );

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

        existing.Close();
    }

    document["version"] = 2;
    document["projects"][projectKey( aProjectPath )] =
            { { "threadId", aThreadId }, { "toolSchemaVersion", aToolSchemaVersion } };

    wxString temporaryPath = path + wxS( ".tmp" );
    wxFFile temporary( temporaryPath, wxS( "wb" ) );

    if( !temporary.IsOpened()
        || !temporary.Write( wxString::FromUTF8( document.dump( 2 ) ), wxConvUTF8 )
        || !temporary.Flush() )
    {
        temporary.Close();
        wxRemoveFile( temporaryPath );

        if( aError )
            *aError = _( "Could not write the KiChad Codex conversation index." );

        return false;
    }

    temporary.Close();

    if( !wxRenameFile( temporaryPath, path, true ) )
    {
        wxRemoveFile( temporaryPath );

        if( aError )
            *aError = _( "Could not atomically update the KiChad Codex conversation index." );

        return false;
    }

    return true;
}


bool CODEX_THREAD_STORE::Clear( const wxString& aProjectPath, wxString* aError ) const
{
    const wxString path = storagePath();

    if( !wxFileName::FileExists( path ) )
        return true;

    wxFFile existing( path, wxS( "rb" ) );
    wxString contents;

    if( !existing.IsOpened() || !existing.ReadAll( &contents, wxConvUTF8 ) )
    {
        if( aError )
            *aError = _( "Could not read the KiChad Codex conversation index." );

        return false;
    }

    existing.Close();
    nlohmann::json document = nlohmann::json::parse( std::string( contents.ToUTF8() ), nullptr,
                                                     false );

    if( !document.is_object() || !document.contains( "projects" )
        || !document["projects"].is_object() )
    {
        if( aError )
            *aError = _( "The KiChad Codex conversation index is invalid." );

        return false;
    }

    document["projects"].erase( projectKey( aProjectPath ) );
    const wxString temporaryPath = path + wxS( ".tmp" );
    wxFFile temporary( temporaryPath, wxS( "wb" ) );

    if( !temporary.IsOpened()
        || !temporary.Write( wxString::FromUTF8( document.dump( 2 ) ), wxConvUTF8 )
        || !temporary.Flush() )
    {
        temporary.Close();
        wxRemoveFile( temporaryPath );

        if( aError )
            *aError = _( "Could not update the KiChad Codex conversation index." );

        return false;
    }

    temporary.Close();

    if( !wxRenameFile( temporaryPath, path, true ) )
    {
        wxRemoveFile( temporaryPath );

        if( aError )
            *aError = _( "Could not atomically update the KiChad Codex conversation index." );

        return false;
    }

    return true;
}


wxString CODEX_THREAD_STORE::storagePath() const
{
    return m_storagePath.IsEmpty() ? KICHAD::CODEX_PATHS::ThreadIndex() : m_storagePath;
}


std::string CODEX_THREAD_STORE::projectKey( const wxString& aProjectPath ) const
{
    wxFileName path = wxFileName::DirName( aProjectPath );
    path.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    return std::string( path.GetFullPath().ToUTF8() );
}
