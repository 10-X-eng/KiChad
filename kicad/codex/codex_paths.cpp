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

#include "codex_paths.h"

#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>


namespace KICHAD::CODEX_PATHS
{

wxString ConfigRoot()
{
    wxString configRoot;

#if defined( __UNIX__ ) && !defined( __WXOSX__ )
    if( !wxGetEnv( wxS( "XDG_CONFIG_HOME" ), &configRoot ) || configRoot.IsEmpty()
        || !wxFileName::DirName( configRoot ).IsAbsolute() )
    {
        wxFileName fallback( wxGetHomeDir(), wxEmptyString );
        fallback.AppendDir( wxS( ".config" ) );
        configRoot = fallback.GetFullPath();
    }
#else
    configRoot = wxStandardPaths::Get().GetUserConfigDir();
#endif

    wxFileName path( configRoot, wxEmptyString );
    path.AppendDir( wxS( "kichad" ) );
    return path.GetFullPath();
}


wxString AppServerHome()
{
    wxFileName path( ConfigRoot(), wxEmptyString );
    path.AppendDir( wxS( "codex" ) );
    return path.GetFullPath();
}


wxString ThreadIndex()
{
    wxFileName path( ConfigRoot(), wxS( "codex-threads.json" ) );
    return path.GetFullPath();
}

} // namespace KICHAD::CODEX_PATHS
