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

#include <boost/test/unit_test.hpp>

#include <kicad/codex/codex_thread_store.h>

#include <nlohmann/json.hpp>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/utils.h>


BOOST_AUTO_TEST_SUITE( CodexThreadStore )


BOOST_AUTO_TEST_CASE( ResumesOnlyThreadsWithTheCurrentNativeToolSchema )
{
    const wxString root = wxFileName::CreateTempFileName( wxS( "kichad-thread-store-" ) );
    BOOST_REQUIRE( wxRemoveFile( root ) );
    BOOST_REQUIRE( wxFileName::Mkdir( root, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );
    const wxString index = wxFileName( root, wxS( "threads.json" ) ).GetFullPath();
    const wxString project = wxFileName( root, wxS( "project" ) ).GetFullPath();
    BOOST_REQUIRE( wxFileName::Mkdir( project, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );

    CODEX_THREAD_STORE store( index );
    wxString error;
    BOOST_REQUIRE_MESSAGE( store.Save( project, "thread-current", 2, &error ), error );
    BOOST_CHECK_EQUAL( store.Load( project, 2 ), "thread-current" );
    BOOST_CHECK( store.Load( project, 1 ).empty() );
    BOOST_CHECK( store.Load( wxFileName( root, wxS( "other" ) ).GetFullPath(), 2 ).empty() );

    wxFFile file( index, wxS( "rb" ) );
    BOOST_REQUIRE( file.IsOpened() );
    wxString text;
    BOOST_REQUIRE( file.ReadAll( &text, wxConvUTF8 ) );
    nlohmann::json document = nlohmann::json::parse( std::string( text.ToUTF8() ) );
    BOOST_CHECK_EQUAL( document["version"].get<int>(), 2 );
    file.Close();

    BOOST_CHECK( wxFileName::Rmdir( root, wxPATH_RMDIR_RECURSIVE ) );
}


BOOST_AUTO_TEST_CASE( RejectsLegacyUnversionedThreadBindings )
{
    const wxString root = wxFileName::CreateTempFileName( wxS( "kichad-thread-legacy-" ) );
    BOOST_REQUIRE( wxRemoveFile( root ) );
    BOOST_REQUIRE( wxFileName::Mkdir( root, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );
    const wxString index = wxFileName( root, wxS( "threads.json" ) ).GetFullPath();
    const wxString project = wxFileName( root, wxS( "project" ) ).GetFullPath();
    BOOST_REQUIRE( wxFileName::Mkdir( project, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );

    wxFileName canonicalProject = wxFileName::DirName( project );
    canonicalProject.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    nlohmann::json legacy = {
        { "version", 1 },
        { "projects",
          { { std::string( canonicalProject.GetFullPath().ToUTF8() ), "thread-legacy" } } }
    };
    wxFFile file( index, wxS( "wb" ) );
    BOOST_REQUIRE( file.IsOpened() );
    BOOST_REQUIRE( file.Write( wxString::FromUTF8( legacy.dump() ), wxConvUTF8 ) );
    file.Close();

    CODEX_THREAD_STORE store( index );
    BOOST_CHECK( store.Load( project, 2 ).empty() );
    BOOST_CHECK( wxFileName::Rmdir( root, wxPATH_RMDIR_RECURSIVE ) );
}


BOOST_AUTO_TEST_CASE( RejectsCorruptedToolSchemaWithoutThrowing )
{
    const wxString root = wxFileName::CreateTempFileName( wxS( "kichad-thread-corrupt-" ) );
    BOOST_REQUIRE( wxRemoveFile( root ) );
    BOOST_REQUIRE( wxFileName::Mkdir( root, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );
    const wxString index = wxFileName( root, wxS( "threads.json" ) ).GetFullPath();
    const wxString project = wxFileName( root, wxS( "project" ) ).GetFullPath();
    BOOST_REQUIRE( wxFileName::Mkdir( project, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) );

    wxFileName canonicalProject = wxFileName::DirName( project );
    canonicalProject.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    nlohmann::json corrupted = {
        { "version", 2 },
        { "projects",
          { { std::string( canonicalProject.GetFullPath().ToUTF8() ),
              { { "threadId", "thread-corrupt" }, { "toolSchemaVersion", "bad" } } } } }
    };
    wxFFile file( index, wxS( "wb" ) );
    BOOST_REQUIRE( file.IsOpened() );
    BOOST_REQUIRE( file.Write( wxString::FromUTF8( corrupted.dump() ), wxConvUTF8 ) );
    file.Close();

    CODEX_THREAD_STORE store( index );
    std::string loaded;
    BOOST_CHECK_NO_THROW( loaded = store.Load( project, 2 ) );
    BOOST_CHECK( loaded.empty() );
    BOOST_CHECK( wxFileName::Rmdir( root, wxPATH_RMDIR_RECURSIVE ) );
}


BOOST_AUTO_TEST_SUITE_END()
