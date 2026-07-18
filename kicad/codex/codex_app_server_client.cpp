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

#include "codex_app_server_client.h"

#include <vector>

#include <wx/log.h>
#include <wx/intl.h>
#include <wx/utils.h>


class CODEX_PROCESS : public wxProcess
{
public:
    explicit CODEX_PROCESS( wxEvtHandler* aParent ) : wxProcess( aParent ), m_selfDelete( false ) {}

    void SelfDeleteOnTerminate()
    {
        m_selfDelete = true;
        Detach();
    }

    void OnTerminate( int aPid, int aStatus ) override
    {
        if( m_selfDelete )
            delete this;
        else
            wxProcess::OnTerminate( aPid, aStatus );
    }

private:
    bool m_selfDelete;
};


CODEX_APP_SERVER_CLIENT::CODEX_APP_SERVER_CLIENT() :
        m_process( nullptr ),
        m_pid( 0 ),
        m_pollTimer( this ),
        m_nextRequestId( 1 )
{
    Bind( wxEVT_TIMER, &CODEX_APP_SERVER_CLIENT::onPoll, this, m_pollTimer.GetId() );
    Bind( wxEVT_END_PROCESS, &CODEX_APP_SERVER_CLIENT::onProcessTerminated, this );
}


CODEX_APP_SERVER_CLIENT::~CODEX_APP_SERVER_CLIENT()
{
    Stop();
    Unbind( wxEVT_TIMER, &CODEX_APP_SERVER_CLIENT::onPoll, this, m_pollTimer.GetId() );
    Unbind( wxEVT_END_PROCESS, &CODEX_APP_SERVER_CLIENT::onProcessTerminated, this );
}


bool CODEX_APP_SERVER_CLIENT::Start()
{
    if( IsRunning() )
        return true;

    wxString executable;

    if( !wxGetEnv( wxS( "KICHAD_CODEX_EXECUTABLE" ), &executable ) || executable.IsEmpty() )
        executable = wxS( "codex" );

    std::vector<wxString>       argumentStorage = { executable, wxS( "app-server" ) };
    std::vector<const wchar_t*> arguments;

    for( const wxString& argument : argumentStorage )
        arguments.emplace_back( argument.wc_str() );

    arguments.emplace_back( nullptr );

    m_process = new CODEX_PROCESS( this );
    m_process->Redirect();
    m_pid = wxExecute( arguments.data(), wxEXEC_ASYNC, m_process );

    if( m_pid == 0 )
    {
        delete m_process;
        m_process = nullptr;
        setState( false, wxString::Format( _( "Could not start Codex executable '%s'." ),
                                           executable ) );
        return false;
    }

    m_stdoutBuffer.clear();
    m_stderrBuffer.clear();
    m_pendingRequests.clear();
    m_pollTimer.Start( POLL_INTERVAL_MS );
    setState( true, _( "Connecting to Codex..." ) );
    return true;
}


void CODEX_APP_SERVER_CLIENT::Stop()
{
    m_pollTimer.Stop();
    m_pendingRequests.clear();

    if( !m_process )
        return;

    CODEX_PROCESS* process = static_cast<CODEX_PROCESS*>( m_process );
    process->SelfDeleteOnTerminate();

    // Closing stdin asks app-server to finish cleanly.  Terminate only this exact child if it does
    // not observe EOF before the host is torn down.
    process->CloseOutput();

    if( m_pid > 0 && wxProcess::Exists( m_pid ) )
        wxProcess::Kill( static_cast<int>( m_pid ), wxSIGTERM, wxKILL_NOCHILDREN );

    m_process = nullptr;
    m_pid = 0;
}


int64_t CODEX_APP_SERVER_CLIENT::SendRequest( const std::string& aMethod, const JSON& aParams,
                                              RESPONSE_HANDLER aHandler )
{
    const int64_t requestId = m_nextRequestId++;
    JSON          message = { { "id", requestId }, { "method", aMethod }, { "params", aParams } };

    if( !writeMessage( message ) )
        return 0;

    if( aHandler )
        m_pendingRequests.emplace( requestId, std::move( aHandler ) );

    return requestId;
}


bool CODEX_APP_SERVER_CLIENT::SendNotification( const std::string& aMethod, const JSON& aParams )
{
    JSON message = { { "method", aMethod } };

    if( !aParams.empty() )
        message["params"] = aParams;

    return writeMessage( message );
}


bool CODEX_APP_SERVER_CLIENT::SendResponse( const JSON& aId, const JSON& aResult )
{
    return writeMessage( { { "id", aId }, { "result", aResult } } );
}


bool CODEX_APP_SERVER_CLIENT::SendError( const JSON& aId, int aCode, const std::string& aMessage )
{
    return writeMessage( { { "id", aId },
                           { "error", { { "code", aCode }, { "message", aMessage } } } } );
}


bool CODEX_APP_SERVER_CLIENT::writeMessage( const JSON& aMessage )
{
    if( !IsRunning() || !m_process->GetOutputStream() )
        return false;

    const std::string serialized = aMessage.dump() + "\n";
    wxOutputStream*   stream = m_process->GetOutputStream();
    stream->Write( serialized.data(), serialized.size() );
    stream->Sync();

    if( !stream->IsOk() )
    {
        setState( false, _( "Lost the Codex app-server input stream." ) );
        return false;
    }

    return true;
}


void CODEX_APP_SERVER_CLIENT::consumeStream( wxInputStream* aStream, std::string& aBuffer,
                                             bool aParseJson )
{
    if( !aStream )
        return;

    char buffer[4096];

    while( aStream->CanRead() )
    {
        aStream->Read( buffer, sizeof( buffer ) );
        aBuffer.append( buffer, aStream->LastRead() );
    }

    size_t newline = 0;

    while( ( newline = aBuffer.find( '\n' ) ) != std::string::npos )
    {
        std::string line = aBuffer.substr( 0, newline );
        aBuffer.erase( 0, newline + 1 );

        if( !line.empty() && line.back() == '\r' )
            line.pop_back();

        if( line.empty() )
            continue;

        if( aParseJson )
            dispatchLine( line );
        else
            wxLogTrace( wxS( "KICHAD_CODEX" ), wxS( "app-server: %s" ), wxString::FromUTF8( line ) );
    }
}


void CODEX_APP_SERVER_CLIENT::dispatchLine( const std::string& aLine )
{
    JSON message = JSON::parse( aLine, nullptr, false );

    if( message.is_discarded() )
    {
        setState( false, _( "Codex app-server returned malformed JSON." ) );
        wxLogTrace( wxS( "KICHAD_CODEX" ), wxS( "Malformed app-server JSON: %s" ),
                    wxString::FromUTF8( aLine ) );
        return;
    }

    if( message.contains( "id" ) && !message.contains( "method" ) && message["id"].is_number_integer() )
    {
        int64_t requestId = message["id"].get<int64_t>();
        auto    pending = m_pendingRequests.find( requestId );

        if( pending != m_pendingRequests.end() )
        {
            RESPONSE_HANDLER handler = std::move( pending->second );
            m_pendingRequests.erase( pending );
            handler( message );
        }

        return;
    }

    if( m_messageHandler )
        m_messageHandler( message );
}


void CODEX_APP_SERVER_CLIENT::setState( bool aRunning, const wxString& aDetail )
{
    if( m_stateHandler )
        m_stateHandler( aRunning, aDetail );
}


void CODEX_APP_SERVER_CLIENT::onPoll( wxTimerEvent& aEvent )
{
    if( !m_process )
        return;

    consumeStream( m_process->GetInputStream(), m_stdoutBuffer, true );
    consumeStream( m_process->GetErrorStream(), m_stderrBuffer, false );
}


void CODEX_APP_SERVER_CLIENT::onProcessTerminated( wxProcessEvent& aEvent )
{
    if( !m_process || aEvent.GetPid() != m_pid )
        return;

    consumeStream( m_process->GetInputStream(), m_stdoutBuffer, true );
    consumeStream( m_process->GetErrorStream(), m_stderrBuffer, false );
    m_pollTimer.Stop();

    wxProcess* finished = m_process;
    m_process = nullptr;
    m_pid = 0;
    m_pendingRequests.clear();
    delete finished;

    setState( false, wxString::Format( _( "Codex app-server exited with status %d." ),
                                      aEvent.GetExitCode() ) );
}
