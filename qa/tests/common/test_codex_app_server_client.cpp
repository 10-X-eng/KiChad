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

#include <kicad/codex/codex_app_server_client.h>

#include <cstring>
#include <string>

#include <nlohmann/json.hpp>
#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/utils.h>


namespace
{

class ENVIRONMENT_GUARD
{
public:
    ENVIRONMENT_GUARD( const wxString& aName, const wxString& aValue ) :
            m_name( aName ),
            m_hadValue( wxGetEnv( aName, &m_value ) )
    {
        wxSetEnv( m_name, aValue );
    }

    ~ENVIRONMENT_GUARD()
    {
        if( m_hadValue )
            wxSetEnv( m_name, m_value );
        else
            wxUnsetEnv( m_name );
    }

private:
    wxString m_name;
    wxString m_value;
    bool     m_hadValue;
};

} // namespace


BOOST_AUTO_TEST_SUITE( CodexAppServerClient )


BOOST_AUTO_TEST_CASE( DeliversLargeJsonRpcFrameAcrossPipeBackpressure )
{
    const wxString root = wxFileName::CreateTempFileName( wxS( "kichad-codex-pipe-" ) );
    BOOST_REQUIRE( wxRemoveFile( root ) );
    BOOST_REQUIRE( wxFileName::Mkdir( root, 0700, wxPATH_MKDIR_FULL ) );
    const wxString script = wxFileName( root, wxS( "slow-app-server" ) ).GetFullPath();
    const wxString capture = wxFileName( root, wxS( "captured.ndjson" ) ).GetFullPath();
    const wxString codexHome = wxFileName( root, wxS( "codex-home" ) ).GetFullPath();
    const char* scriptSource =
            "#!/usr/bin/env python3\n"
            "import os\n"
            "import sys\n"
            "import time\n"
            "time.sleep(0.5)\n"
            "with open(os.environ['KICHAD_CODEX_CAPTURE'], 'wb', buffering=0) as output:\n"
            "    while True:\n"
            "        line = sys.stdin.buffer.readline()\n"
            "        if not line:\n"
            "            break\n"
            "        output.write(line)\n";
    wxFFile scriptFile( script, wxS( "wb" ) );
    BOOST_REQUIRE( scriptFile.IsOpened() );
    BOOST_REQUIRE( scriptFile.Write( scriptSource, std::strlen( scriptSource ) ) );
    scriptFile.Close();
    BOOST_REQUIRE_EQUAL( wxChmod( script, 0700 ), 0 );

    ENVIRONMENT_GUARD executableGuard( wxS( "KICHAD_CODEX_EXECUTABLE" ), script );
    ENVIRONMENT_GUARD homeGuard( wxS( "KICHAD_CODEX_HOME" ), codexHome );
    ENVIRONMENT_GUARD captureGuard( wxS( "KICHAD_CODEX_CAPTURE" ), capture );
    CODEX_APP_SERVER_CLIENT client;
    bool reportedUnavailable = false;
    client.SetStateHandler(
            [&]( bool aRunning, const wxString& )
            {
                if( !aRunning )
                    reportedUnavailable = true;
            } );
    BOOST_REQUIRE( client.Start() );

    const std::string payload( 512 * 1024, 'K' );
    const nlohmann::json response = {
        { "id", 71 }, { "result", { { "payload", payload } } }
    };
    const std::string expected = response.dump() + "\n";
    BOOST_REQUIRE_GT( expected.size(), 64 * 1024 );
    BOOST_REQUIRE( client.SendResponse( 71, { { "payload", payload } } ) );

    bool captured = false;

    for( int attempt = 0; attempt < 120 && !captured; ++attempt )
    {
        wxMilliSleep( 25 );

        // A normal outbound notification also asks the client to drain any older queued frame.
        // The ordered queue guarantees these probes cannot overtake the large response.
        BOOST_REQUIRE( client.SendNotification( "transport/probe",
                                                { { "attempt", attempt } } ) );
        captured = wxFileName::FileExists( capture )
                   && wxFileName::GetSize( capture ) >= expected.size();
    }

    BOOST_REQUIRE_MESSAGE( captured, "slow app-server did not receive the complete JSON-RPC frame" );
    wxFFile captureFile( capture, wxS( "rb" ) );
    BOOST_REQUIRE( captureFile.IsOpened() );
    wxString capturedText;
    BOOST_REQUIRE( captureFile.ReadAll( &capturedText, wxConvUTF8 ) );
    captureFile.Close();
    const std::string bytes( capturedText.ToUTF8() );
    const size_t newline = bytes.find( '\n' );
    BOOST_REQUIRE_NE( newline, std::string::npos );
    BOOST_CHECK_EQUAL( bytes.substr( 0, newline + 1 ), expected );
    BOOST_CHECK( !reportedUnavailable );

    client.Stop();
    BOOST_CHECK( wxFileName::Rmdir( root, wxPATH_RMDIR_RECURSIVE ) );
}


BOOST_AUTO_TEST_SUITE_END()
