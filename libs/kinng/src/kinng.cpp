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

#include <kinng.h>

#include <algorithm>
#include <limits>
#include <utility>

#include <nng/nng.h>
#include <nng/protocol/reqrep0/rep.h>
#include <nng/protocol/reqrep0/req.h>
#include <wx/log.h>


/**
 * Trace nng server debug output
 * @ingroup trace_env_vars
 */
static const wxChar TraceNng[] = wxT( "KINNG" );
static constexpr size_t MAX_REQUEST_BYTES = 64 * 1024 * 1024;


KINNG_REQUEST_RESULT KINNG_REQUEST_CLIENT::Request( const std::string& aSocketUrl,
                                                    const std::string& aRequest,
                                                    std::chrono::milliseconds aTimeout )
{
    KINNG_REQUEST_RESULT result;

    if( aSocketUrl.empty() )
    {
        result.errorMessage = "the IPC socket URL is empty";
        return result;
    }

    if( aRequest.size() > MAX_REQUEST_BYTES )
    {
        result.errorMessage = "the IPC request exceeds 64 MiB";
        return result;
    }

    if( aTimeout.count() <= 0 )
    {
        result.errorMessage = "the IPC timeout must be positive";
        return result;
    }

    nng_socket socket;
    int        retCode = nng_req0_open( &socket );

    if( retCode != 0 )
    {
        result.errorCode = retCode;
        result.errorMessage = std::string( "could not open IPC request socket: " )
                              + nng_strerror( retCode );
        return result;
    }

    auto fail = [&]( int aErrorCode, const char* aOperation )
    {
        result.errorCode = aErrorCode;
        result.errorMessage = std::string( aOperation ) + ": " + nng_strerror( aErrorCode );
        nng_close( socket );
        return result;
    };

    const int timeout = static_cast<int>( std::min<int64_t>(
            aTimeout.count(), std::numeric_limits<int>::max() ) );
    nng_socket_set_ms( socket, NNG_OPT_SENDTIMEO, timeout );
    nng_socket_set_ms( socket, NNG_OPT_RECVTIMEO, timeout );
    nng_socket_set_size( socket, NNG_OPT_RECVMAXSZ, MAX_REQUEST_BYTES );

    retCode = nng_dial( socket, aSocketUrl.c_str(), nullptr, NNG_FLAG_NONBLOCK );

    if( retCode != 0 )
        return fail( retCode, "could not connect to IPC socket" );

    retCode = nng_send( socket, const_cast<char*>( aRequest.data() ), aRequest.size(), 0 );

    if( retCode != 0 )
        return fail( retCode, "could not send IPC request" );

    char*  response = nullptr;
    size_t responseSize = 0;
    retCode = nng_recv( socket, &response, &responseSize, NNG_FLAG_ALLOC );

    if( retCode != 0 )
        return fail( retCode, "could not receive IPC response" );

    result.response.assign( response, responseSize );
    result.success = true;
    nng_free( response, responseSize );
    nng_close( socket );
    return result;
}


KINNG_REQUEST_SERVER::KINNG_REQUEST_SERVER( const std::string& aSocketUrl ) :
        m_socketUrl( aSocketUrl ),
        m_callback()
{
    Start();
}


KINNG_REQUEST_SERVER::~KINNG_REQUEST_SERVER()
{
    Stop();
}


bool KINNG_REQUEST_SERVER::Running() const
{
    return m_thread.joinable();
}


bool KINNG_REQUEST_SERVER::Start()
{
    if( Running() )
        return true;

    m_shutdown.store( false );
    m_thread = std::thread( [&]() { listenThread(); } );
    return true;
}


void KINNG_REQUEST_SERVER::Stop()
{
    if( !m_thread.joinable() )
        return;

    m_shutdown.store( true );

    {
        std::lock_guard<std::mutex> lock( m_mutex );
        m_replyReady.notify_all();
    }

    m_thread.join();
}


void KINNG_REQUEST_SERVER::Reply( const std::string& aReply )
{
    std::lock_guard<std::mutex> lock( m_mutex );
    m_pendingReply = aReply;
    m_replyReady.notify_all();
}


void KINNG_REQUEST_SERVER::SetCallback( std::function<void(std::string*)> aFunc )
{
    std::lock_guard<std::mutex> lock( m_mutex );
    m_callback = std::move( aFunc );
}


void KINNG_REQUEST_SERVER::listenThread()
{
    nng_socket   socket;
    nng_listener listener;
    int          retCode = 0;

    wxLogTrace( TraceNng, wxS( "KINNG_REQUEST_SERVER starting" ) );

    retCode = nng_rep0_open( &socket );

    if( retCode != 0 )
    {
        wxLogTrace( TraceNng,
                    wxString::Format( wxS( "Got error code %d from nng_rep0_open!" ), retCode ) );
        return;
    }

    retCode = nng_listener_create( &listener, socket, m_socketUrl.c_str() );

    if( retCode != 0 )
    {
        wxLogTrace( TraceNng,
                    wxString::Format( wxS( "Got error code %d from nng_listener_create!" ),
                                      retCode ) );
        nng_close( socket );
        return;
    }

    nng_socket_set_ms( socket, NNG_OPT_RECVTIMEO, 500 );

    retCode = nng_listener_start( listener, 0 );

    if( retCode != 0 )
    {
        wxLogTrace( TraceNng,
                    wxString::Format( wxS( "Got error code %d from nng_listener_start!" ),
                                      retCode ) );
        nng_close( socket );
        return;
    }

    wxLogTrace( TraceNng, wxS( "KINNG_REQUEST_SERVER listener has started" ) );

    while( !m_shutdown.load() )
    {
        char*  buf = nullptr;
        size_t sz = 0;

        retCode = nng_recv( socket, &buf, &sz, NNG_FLAG_ALLOC );

        if( retCode == NNG_ETIMEDOUT )
            continue;

        if( retCode != 0 )
        {
            nng_free( buf, sz );
            wxLogTrace( TraceNng,
                        wxString::Format( wxS( "Got error code %d from nngc_recv!" ), retCode ) );
            break;
        }

        m_sharedMessage.assign( buf, sz );
        nng_free( buf, sz );

        std::function<void(std::string*)> callback;

        {
            std::lock_guard<std::mutex> lock( m_mutex );
            callback = m_callback;
        }

        if( callback )
            callback( &m_sharedMessage );

        std::unique_lock<std::mutex> lock( m_mutex );
        m_replyReady.wait( lock,
                           [&]() { return m_shutdown.load() || !m_pendingReply.empty(); } );

        if( m_shutdown.load() )
            break;

        retCode = nng_send( socket, const_cast<std::string::value_type*>( m_pendingReply.c_str() ),
                            m_pendingReply.length(), 0 );

        if( retCode != 0 )
        {
            wxLogTrace( TraceNng,
                        wxString::Format( wxS( "Got error code %d from nng_send!" ), retCode ) );
        }

        m_pendingReply.clear();
    }

    wxLogTrace( TraceNng, wxS( "KINNG_REQUEST_SERVER shutting down" ) );

    nng_close( socket );
}
