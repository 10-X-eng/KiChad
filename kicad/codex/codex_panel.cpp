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

#include "codex_panel.h"

#include <build_version.h>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/filename.h>
#include <wx/intl.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>


CODEX_PANEL::CODEX_PANEL( wxWindow* aParent, std::function<wxString()> aProjectPathProvider ) :
        wxPanel( aParent ),
        m_projectPathProvider( std::move( aProjectPathProvider ) ),
        m_toolRegistry( m_projectPathProvider ),
        m_status( nullptr ),
        m_loginButton( nullptr ),
        m_modelChoice( nullptr ),
        m_reasoningChoice( nullptr ),
        m_transcript( nullptr ),
        m_input( nullptr ),
        m_sendButton( nullptr ),
        m_stopButton( nullptr ),
        m_initialized( false ),
        m_authenticated( false ),
        m_threadLoaded( false )
{
    wxBoxSizer* root = new wxBoxSizer( wxVERTICAL );

    wxBoxSizer* statusRow = new wxBoxSizer( wxHORIZONTAL );
    m_status = new wxStaticText( this, wxID_ANY, _( "Codex is starting..." ) );
    m_loginButton = new wxButton( this, wxID_ANY, _( "Sign in" ) );
    m_loginButton->Disable();
    statusRow->Add( m_status, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 8 ) );
    statusRow->Add( m_loginButton, 0, wxALIGN_CENTER_VERTICAL );
    root->Add( statusRow, 0, wxEXPAND | wxALL, FromDIP( 8 ) );

    wxBoxSizer* modelRow = new wxBoxSizer( wxHORIZONTAL );
    m_modelChoice = new wxChoice( this, wxID_ANY );
    m_reasoningChoice = new wxChoice( this, wxID_ANY );
    m_modelChoice->Append( _( "Loading models..." ) );
    m_modelChoice->SetSelection( 0 );
    m_modelChoice->Disable();
    m_reasoningChoice->Append( _( "Default reasoning" ) );
    m_reasoningChoice->SetSelection( 0 );
    m_reasoningChoice->Disable();
    modelRow->Add( m_modelChoice, 1, wxRIGHT, FromDIP( 6 ) );
    modelRow->Add( m_reasoningChoice, 1 );
    root->Add( modelRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    root->Add( new wxStaticLine( this ), 0, wxEXPAND );

    m_transcript = new wxTextCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                   wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxTE_BESTWRAP );
    root->Add( m_transcript, 1, wxEXPAND | wxALL, FromDIP( 8 ) );

    m_input = new wxTextCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                              FromDIP( wxSize( -1, 90 ) ), wxTE_MULTILINE | wxTE_PROCESS_ENTER );
    m_input->SetHint( _( "Describe the board you want Codex to design..." ) );
    root->Add( m_input, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    wxBoxSizer* actionRow = new wxBoxSizer( wxHORIZONTAL );
    m_stopButton = new wxButton( this, wxID_ANY, _( "Stop" ) );
    m_sendButton = new wxButton( this, wxID_ANY, _( "Send" ) );
    m_stopButton->Disable();
    m_sendButton->Disable();
    actionRow->AddStretchSpacer();
    actionRow->Add( m_stopButton, 0, wxRIGHT, FromDIP( 6 ) );
    actionRow->Add( m_sendButton );
    root->Add( actionRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    SetSizer( root );

    m_loginButton->Bind( wxEVT_BUTTON, &CODEX_PANEL::onLogin, this );
    m_sendButton->Bind( wxEVT_BUTTON, &CODEX_PANEL::onSend, this );
    m_stopButton->Bind( wxEVT_BUTTON, &CODEX_PANEL::onStop, this );
    m_modelChoice->Bind( wxEVT_CHOICE, &CODEX_PANEL::onModelChanged, this );

    m_client.SetMessageHandler( [this]( const JSON& aMessage ) { onAppServerMessage( aMessage ); } );
    m_client.SetStateHandler(
            [this]( bool aRunning, const wxString& aDetail )
            {
                onAppServerState( aRunning, aDetail );
            } );

    if( m_client.Start() )
        initializeAppServer();
}


CODEX_PANEL::~CODEX_PANEL()
{
    m_client.SetMessageHandler( {} );
    m_client.SetStateHandler( {} );
}


void CODEX_PANEL::initializeAppServer()
{
    JSON params = {
        { "clientInfo",
          { { "name", "kichad" }, { "title", "KiChad" },
            { "version", std::string( GetBuildVersion().ToUTF8() ) } } },
        { "capabilities", { { "experimentalApi", true }, { "requestAttestation", false } } }
    };

    m_client.SendRequest(
            "initialize", params,
            [this]( const JSON& aResponse )
            {
                if( aResponse.contains( "error" ) )
                {
                    setStatus( _( "Codex protocol initialization failed." ) );
                    return;
                }

                m_initialized = true;
                m_client.SendNotification( "initialized" );
                setStatus( _( "Checking Codex account..." ) );
                readAccount();
                readModels();
            } );
}


void CODEX_PANEL::readAccount()
{
    m_client.SendRequest(
            "account/read", { { "refreshToken", false } },
            [this]( const JSON& aResponse )
            {
                if( !aResponse.contains( "result" ) )
                {
                    setStatus( _( "Could not read the Codex account." ) );
                    m_loginButton->Enable();
                    return;
                }

                const JSON& account = aResponse["result"].value( "account", JSON() );
                m_authenticated = account.is_object();

                if( m_authenticated && account.value( "type", "" ) == "chatgpt" )
                {
                    std::string email = account.value( "email", "" );
                    setStatus( email.empty() ? _( "Signed in with ChatGPT" )
                                             : wxString::Format( _( "ChatGPT: %s" ),
                                                                 wxString::FromUTF8( email ) ) );
                    m_loginButton->SetLabel( _( "Sign out" ) );
                    m_sendButton->Enable( m_initialized );
                }
                else
                {
                    setStatus( _( "Sign in with ChatGPT to use Codex." ) );
                    m_loginButton->SetLabel( _( "Sign in" ) );
                    m_sendButton->Disable();
                }

                m_loginButton->Enable();
            } );
}


void CODEX_PANEL::readModels()
{
    m_client.SendRequest(
            "model/list", { { "includeHidden", false } },
            [this]( const JSON& aResponse )
            {
                m_models.clear();
                m_modelChoice->Clear();

                if( aResponse.contains( "result" ) && aResponse["result"].contains( "data" ) )
                {
                    for( const JSON& model : aResponse["result"]["data"] )
                    {
                        if( !model.value( "hidden", false ) )
                        {
                            m_models.emplace_back( model );
                            m_modelChoice->Append( wxString::FromUTF8( model.value( "displayName",
                                                                                  model.value( "model", "" ) ) ) );
                        }
                    }
                }

                if( m_models.empty() )
                {
                    m_modelChoice->Append( _( "No models available" ) );
                    m_modelChoice->SetSelection( 0 );
                    m_modelChoice->Disable();
                    return;
                }

                size_t defaultIndex = 0;

                for( size_t i = 0; i < m_models.size(); ++i )
                {
                    if( m_models[i].value( "isDefault", false ) )
                    {
                        defaultIndex = i;
                        break;
                    }
                }

                m_modelChoice->SetSelection( static_cast<int>( defaultIndex ) );
                m_modelChoice->Enable();
                updateReasoningChoices();
            } );
}


void CODEX_PANEL::updateReasoningChoices()
{
    m_reasoningChoice->Clear();

    int selection = m_modelChoice->GetSelection();

    if( selection < 0 || static_cast<size_t>( selection ) >= m_models.size() )
    {
        m_reasoningChoice->Append( _( "Default reasoning" ) );
        m_reasoningChoice->SetSelection( 0 );
        m_reasoningChoice->Disable();
        return;
    }

    const JSON& model = m_models[selection];
    std::string defaultEffort = model.value( "defaultReasoningEffort", "" );
    int         defaultSelection = 0;

    for( const JSON& option : model.value( "supportedReasoningEfforts", JSON::array() ) )
    {
        std::string effort = option.value( "reasoningEffort", "" );

        if( effort.empty() )
            continue;

        int index = m_reasoningChoice->Append( wxString::FromUTF8( effort ) );

        if( effort == defaultEffort )
            defaultSelection = index;
    }

    if( m_reasoningChoice->GetCount() == 0 )
        m_reasoningChoice->Append( _( "Default reasoning" ) );

    m_reasoningChoice->SetSelection( defaultSelection );
    m_reasoningChoice->Enable( m_reasoningChoice->GetCount() > 1 );
}


void CODEX_PANEL::startThreadAndTurn( const std::string& aMessage )
{
    wxString cwd = projectPath();

    JSON params = {
        { "cwd", std::string( cwd.ToUTF8() ) },
        { "runtimeWorkspaceRoots", JSON::array( { std::string( cwd.ToUTF8() ) } ) },
        { "approvalPolicy", "never" },
        { "sandbox", "read-only" },
        { "ephemeral", false },
        { "historyMode", "paginated" },
        { "serviceName", "KiChad" },
        { "baseInstructions",
          "You are the KiChad electronics design agent. Use only the native KiChad dynamic tools "
          "advertised by the host for design work. Never use shell, arbitrary code execution, GUI "
          "automation, MCP, or direct ad-hoc file rewriting." },
        { "config",
          { { "features",
              { { "shell_tool", false }, { "unified_exec", false }, { "apps", false },
                { "browser_use", false }, { "computer_use", false },
                { "image_generation", false }, { "multi_agent", false } } },
            { "web_search", "disabled" } } }
    };

    params["dynamicTools"] = m_toolRegistry.Specs();

    int modelSelection = m_modelChoice->GetSelection();

    if( modelSelection >= 0 && static_cast<size_t>( modelSelection ) < m_models.size() )
        params["model"] = m_models[modelSelection].value( "model", "" );

    m_client.SendRequest(
            "thread/start", params,
            [this, aMessage]( const JSON& aResponse )
            {
                if( !aResponse.contains( "result" ) )
                {
                    appendTranscript( _( "\n[Could not start a persistent Codex conversation.]\n" ) );
                    setBusy( false );
                    return;
                }

                m_threadId = aResponse["result"]["thread"].value( "id", "" );

                if( m_threadId.empty() )
                {
                    appendTranscript( _( "\n[Codex returned no conversation identifier.]\n" ) );
                    setBusy( false );
                    return;
                }

                m_threadLoaded = true;

                wxString saveError;

                if( !m_threadStore.Save( m_threadProjectPath, m_threadId, &saveError ) )
                    appendTranscript( wxString::Format( _( "\n[Conversation persistence: %s]\n" ),
                                                        saveError ) );

                startTurn( aMessage );
            } );
}


void CODEX_PANEL::resumeThreadAndTurn( const std::string& aMessage )
{
    wxString cwd = projectPath();
    JSON params = {
        { "threadId", m_threadId },
        { "cwd", std::string( cwd.ToUTF8() ) },
        { "runtimeWorkspaceRoots", JSON::array( { std::string( cwd.ToUTF8() ) } ) },
        { "approvalPolicy", "never" },
        { "sandbox", "read-only" },
        { "initialTurnsPage",
          { { "limit", 20 }, { "sortDirection", "asc" }, { "itemsView", "full" } } }
    };

    m_client.SendRequest(
            "thread/resume", params,
            [this, aMessage]( const JSON& aResponse )
            {
                if( !aResponse.contains( "result" ) )
                {
                    appendTranscript( _( "\n[Could not resume the saved Codex conversation.]\n" ) );
                    m_threadId.clear();
                    m_threadLoaded = false;
                    setBusy( false );
                    return;
                }

                m_threadLoaded = true;
                const JSON& page = aResponse["result"].value( "initialTurnsPage", JSON() );

                if( page.is_object() && page.contains( "data" ) )
                {
                    m_transcript->Clear();

                    for( const JSON& turn : page["data"] )
                    {
                        for( const JSON& item : turn.value( "items", JSON::array() ) )
                        {
                            std::string type = item.value( "type", "" );

                            if( type == "userMessage" )
                            {
                                for( const JSON& content : item.value( "content", JSON::array() ) )
                                {
                                    if( content.value( "type", "" ) == "text" )
                                    {
                                        appendTranscript( wxString::Format(
                                                _( "\nYou: %s\n" ),
                                                wxString::FromUTF8( content.value( "text", "" ) ) ) );
                                    }
                                }
                            }
                            else if( type == "agentMessage" )
                            {
                                appendTranscript( wxString::Format(
                                        _( "\nCodex: %s\n" ),
                                        wxString::FromUTF8( item.value( "text", "" ) ) ) );
                            }
                            else if( type == "dynamicToolCall" )
                            {
                                appendTranscript( wxString::Format(
                                        _( "\n[tool: %s — %s]\n" ),
                                        wxString::FromUTF8( item.value( "tool", "" ) ),
                                        item.value( "success", false ) ? _( "success" )
                                                                        : _( "failed" ) ) );
                            }
                        }
                    }
                }

                startTurn( aMessage );
            } );
}


void CODEX_PANEL::startTurn( const std::string& aMessage )
{
    JSON params = {
        { "threadId", m_threadId },
        { "input", JSON::array( { { { "type", "text" }, { "text", aMessage },
                                     { "text_elements", JSON::array() } } } ) }
    };

    int modelSelection = m_modelChoice->GetSelection();

    if( modelSelection >= 0 && static_cast<size_t>( modelSelection ) < m_models.size() )
        params["model"] = m_models[modelSelection].value( "model", "" );

    int reasoningSelection = m_reasoningChoice->GetSelection();

    if( reasoningSelection >= 0 && m_reasoningChoice->IsEnabled() )
        params["effort"] = std::string( m_reasoningChoice->GetString( reasoningSelection ).ToUTF8() );

    appendTranscript( _( "\nCodex: " ) );
    m_client.SendRequest(
            "turn/start", params,
            [this]( const JSON& aResponse )
            {
                if( aResponse.contains( "result" ) )
                    m_turnId = aResponse["result"]["turn"].value( "id", "" );
                else
                {
                    appendTranscript( _( "[turn failed to start]\n" ) );
                    setBusy( false );
                }
            } );
}


void CODEX_PANEL::appendTranscript( const wxString& aText )
{
    m_transcript->AppendText( aText );
    m_transcript->ShowPosition( m_transcript->GetLastPosition() );
}


void CODEX_PANEL::setBusy( bool aBusy )
{
    m_sendButton->Enable( !aBusy && m_initialized && m_authenticated );
    m_stopButton->Enable( aBusy );
    m_input->Enable( !aBusy );
}


void CODEX_PANEL::setStatus( const wxString& aStatus )
{
    m_status->SetLabel( aStatus );
    Layout();
}


wxString CODEX_PANEL::projectPath() const
{
    wxString path = m_projectPathProvider ? m_projectPathProvider() : wxString();

    if( path.IsEmpty() || !wxFileName::DirExists( path ) )
        path = wxGetCwd();

    return path;
}


void CODEX_PANEL::selectProjectThread()
{
    wxString activePath = projectPath();

    if( activePath == m_threadProjectPath )
        return;

    m_threadProjectPath = activePath;
    m_threadId = m_threadStore.Load( activePath );
    m_turnId.clear();
    m_threadLoaded = false;

    if( !m_threadId.empty() )
        setStatus( _( "Persistent Codex conversation ready." ) );
}


void CODEX_PANEL::onAppServerMessage( const JSON& aMessage )
{
    const std::string method = aMessage.value( "method", "" );

    if( method == "account/login/completed" )
    {
        readAccount();
        readModels();
    }
    else if( method == "item/agentMessage/delta" )
    {
        appendTranscript( wxString::FromUTF8( aMessage["params"].value( "delta", "" ) ) );
    }
    else if( method == "item/started" )
    {
        const JSON& item = aMessage["params"]["item"];

        if( item.value( "type", "" ) == "dynamicToolCall" )
        {
            appendTranscript( wxString::Format( _( "\n[tool: %s]\n" ),
                                                wxString::FromUTF8( item.value( "tool", "" ) ) ) );
        }
    }
    else if( method == "turn/started" )
    {
        m_turnId = aMessage["params"]["turn"].value( "id", "" );
    }
    else if( method == "turn/completed" )
    {
        appendTranscript( wxS( "\n" ) );
        m_turnId.clear();
        setBusy( false );
    }
    else if( method == "error" )
    {
        appendTranscript( wxString::Format( _( "\n[Codex error: %s]\n" ),
                                            wxString::FromUTF8(
                                                    aMessage["params"].value( "message", "unknown" ) ) ) );
    }
    else if( method == "item/tool/call" && aMessage.contains( "id" ) )
    {
        const JSON& params = aMessage["params"];
        std::string tool = params.value( "tool", "" );
        JSON arguments = params.value( "arguments", JSON::object() );
        appendTranscript( wxString::Format( _( "\n[tool request: %s %s]\n" ),
                                            wxString::FromUTF8( tool ),
                                            wxString::FromUTF8( arguments.dump() ) ) );

        JSON result = m_toolRegistry.Handle( tool, arguments );
        m_client.SendResponse( aMessage["id"], result );
        appendTranscript( wxString::Format( _( "[tool result: %s]\n" ),
                                            result.value( "success", false ) ? _( "success" )
                                                                             : _( "failed" ) ) );
    }
}


void CODEX_PANEL::onAppServerState( bool aRunning, const wxString& aDetail )
{
    setStatus( aDetail );

    if( !aRunning )
    {
        m_initialized = false;
        m_authenticated = false;
        m_loginButton->Disable();
        m_sendButton->Disable();
        m_stopButton->Disable();
    }
}


void CODEX_PANEL::onLogin( wxCommandEvent& aEvent )
{
    if( m_authenticated )
    {
        m_client.SendRequest( "account/logout", JSON::object(),
                              [this]( const JSON& ) { readAccount(); } );
        return;
    }

    m_loginButton->Disable();
    setStatus( _( "Starting ChatGPT sign-in..." ) );
    m_client.SendRequest(
            "account/login/start", { { "type", "chatgpt" }, { "codexStreamlinedLogin", true },
                                     { "useHostedLoginSuccessPage", true },
                                     { "appBrand", "codex" } },
            [this]( const JSON& aResponse )
            {
                if( aResponse.contains( "result" ) )
                {
                    std::string authUrl = aResponse["result"].value( "authUrl", "" );

                    if( !authUrl.empty() )
                    {
                        wxLaunchDefaultBrowser( wxString::FromUTF8( authUrl ) );
                        setStatus( _( "Complete ChatGPT sign-in in your browser..." ) );
                        return;
                    }
                }

                setStatus( _( "Could not start ChatGPT sign-in." ) );
                m_loginButton->Enable();
            } );
}


void CODEX_PANEL::onSend( wxCommandEvent& aEvent )
{
    selectProjectThread();

    wxString message = m_input->GetValue();
    message.Trim( true ).Trim( false );

    if( message.IsEmpty() )
        return;

    m_input->Clear();
    appendTranscript( wxString::Format( _( "\nYou: %s\n" ), message ) );
    setBusy( true );

    std::string utf8Message( message.ToUTF8() );

    if( m_threadId.empty() )
        startThreadAndTurn( utf8Message );
    else if( !m_threadLoaded )
        resumeThreadAndTurn( utf8Message );
    else
        startTurn( utf8Message );
}


void CODEX_PANEL::onStop( wxCommandEvent& aEvent )
{
    if( m_threadId.empty() || m_turnId.empty() )
        return;

    m_client.SendRequest( "turn/interrupt", { { "threadId", m_threadId }, { "turnId", m_turnId } } );
    setStatus( _( "Stopping Codex turn..." ) );
}


void CODEX_PANEL::onModelChanged( wxCommandEvent& aEvent )
{
    updateReasoningChoices();
}
