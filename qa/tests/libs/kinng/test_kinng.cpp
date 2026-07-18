/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2023 Jon Evans <jon@craftyjon.com>
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <wx/filename.h>
#include <wx/stdpaths.h>

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <kinng.h>
#include <kiid.h>

#include <import_export.h>
#include <api/common/envelope.pb.h>

BOOST_AUTO_TEST_SUITE( KiNNG )

BOOST_AUTO_TEST_CASE( CreateIPCResponder )
{
    wxFileName socketPath( wxFileName::GetTempDir(),
                           wxS( "test-kinng-" ) + KIID().AsString() + wxS( ".sock" ) );
    KINNG_REQUEST_SERVER server( "ipc://" + socketPath.GetFullPath().ToStdString() );
    BOOST_CHECK( server.Running() );
}


BOOST_AUTO_TEST_CASE( RequestRoundTrip )
{
    wxFileName socketPath( wxFileName::GetTempDir(),
                           wxS( "test-kinng-" ) + KIID().AsString() + wxS( ".sock" ) );
    KINNG_REQUEST_SERVER server( "ipc://" + socketPath.GetFullPath().ToStdString() );
    server.SetCallback( [&server]( std::string* aRequest )
                        { server.Reply( "reply:" + *aRequest ); } );

    KINNG_REQUEST_RESULT result = KINNG_REQUEST_CLIENT::Request(
            server.SocketPath(), "hello", std::chrono::milliseconds( 1000 ) );
    BOOST_REQUIRE_MESSAGE( result.success, result.errorMessage );
    BOOST_CHECK_EQUAL( result.response, "reply:hello" );
}


BOOST_AUTO_TEST_CASE( RequestTimeoutIsBounded )
{
    wxFileName socketPath( wxFileName::GetTempDir(),
                           wxS( "test-kinng-" ) + KIID().AsString() + wxS( ".sock" ) );
    KINNG_REQUEST_SERVER server( "ipc://" + socketPath.GetFullPath().ToStdString() );
    server.SetCallback( []( std::string* ) {} );

    KINNG_REQUEST_RESULT result = KINNG_REQUEST_CLIENT::Request(
            server.SocketPath(), "no reply", std::chrono::milliseconds( 50 ) );
    BOOST_CHECK( !result.success );
    BOOST_CHECK_NE( result.errorCode, 0 );
    BOOST_CHECK( !result.errorMessage.empty() );
}

BOOST_AUTO_TEST_SUITE_END()
